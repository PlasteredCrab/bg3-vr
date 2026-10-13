// bg3vr_disasm: offline disassembly and signature generation for a PE file (the game exe).
//
//   bg3vr_disasm <exe> <rva> [count]     disassemble `count` instructions (default 24) from rva
//   bg3vr_disasm <exe> --func <rva>      disassemble the whole function containing rva (.pdata)
//   bg3vr_disasm <exe> --sig <rva>       shortest signature starting at rva that is unique in .text
//   bg3vr_disasm <exe> --find "<sig>"    where a signature matches in .text (exit 0 if exactly once)
//   bg3vr_disasm <exe> --vtable <rva> [n] list n vtable slots (default 24) with their first instructions
//
// RVAs are hex (with or without 0x). Signatures wildcard relative branch targets and RIP-relative
// displacements, the parts that change between game builds.

#include "bg3vr/pattern.hpp"

#include <Zydis.h>
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace {

struct Image {
    std::vector<std::uint8_t> file;
    const IMAGE_NT_HEADERS64* nt = nullptr;
    std::vector<IMAGE_SECTION_HEADER> sections;

    std::optional<std::size_t> offset_of(std::uint32_t rva) const {
        for (const auto& s : sections) {
            if (rva >= s.VirtualAddress && rva < s.VirtualAddress + s.SizeOfRawData) {
                return s.PointerToRawData + (rva - s.VirtualAddress);
            }
        }
        return std::nullopt;
    }

    const IMAGE_SECTION_HEADER* section(const char* name) const {
        for (const auto& s : sections) {
            if (std::strncmp(reinterpret_cast<const char*>(s.Name), name, IMAGE_SIZEOF_SHORT_NAME) == 0) {
                return &s;
            }
        }
        return nullptr;
    }
};

bool load(const char* path, Image& image) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    image.file.assign(std::istreambuf_iterator<char>(in), {});
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image.file.data());
    if (image.file.size() < sizeof(IMAGE_DOS_HEADER) || dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    image.nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(image.file.data() + dos->e_lfanew);
    if (image.nt->Signature != IMAGE_NT_SIGNATURE || image.nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) {
        return false;
    }
    const IMAGE_SECTION_HEADER* first = IMAGE_FIRST_SECTION(image.nt);
    image.sections.assign(first, first + image.nt->FileHeader.NumberOfSections);
    return true;
}

// The .pdata entry (function start/end RVAs) containing rva.
std::optional<std::pair<std::uint32_t, std::uint32_t>> function_bounds(const Image& image, std::uint32_t rva) {
    const auto& dir = image.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    const auto offset = image.offset_of(dir.VirtualAddress);
    if (!offset) {
        return std::nullopt;
    }
    const auto* entries = reinterpret_cast<const RUNTIME_FUNCTION*>(image.file.data() + *offset);
    const std::size_t count = dir.Size / sizeof(RUNTIME_FUNCTION);
    std::size_t lo = 0, hi = count;
    while (lo < hi) {
        const std::size_t mid = (lo + hi) / 2;
        if (entries[mid].EndAddress <= rva) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < count && entries[lo].BeginAddress <= rva && rva < entries[lo].EndAddress) {
        return std::pair{entries[lo].BeginAddress, entries[lo].EndAddress};
    }
    return std::nullopt;
}

bool decode(const Image& image, std::uint32_t rva, ZydisDisassembledInstruction& out) {
    const auto offset = image.offset_of(rva);
    if (!offset) {
        return false;
    }
    const std::size_t available = std::min<std::size_t>(ZYDIS_MAX_INSTRUCTION_LENGTH, image.file.size() - *offset);
    return ZYAN_SUCCESS(ZydisDisassembleIntel(ZYDIS_MACHINE_MODE_LONG_64, rva, image.file.data() + *offset, available, &out));
}

void print_range(const Image& image, std::uint32_t rva, std::uint32_t end, std::size_t max_count, std::uint32_t mark) {
    for (std::size_t i = 0; i < max_count && rva < end; ++i) {
        ZydisDisassembledInstruction ins;
        if (!decode(image, rva, ins)) {
            std::printf("  %08X  <undecodable>\n", rva);
            return;
        }
        const std::uint8_t* bytes = image.file.data() + *image.offset_of(rva);
        std::string hex;
        for (std::uint8_t k = 0; k < ins.info.length; ++k) {
            char b[4];
            std::snprintf(b, sizeof(b), "%02X ", bytes[k]);
            hex += b;
        }
        std::printf("%s %08X  %-36s %s\n", rva == mark ? ">>" : "  ", rva, hex.c_str(), ins.text);
        rva += ins.info.length;
    }
}

// Bytes of one instruction with build-specific fields replaced by wildcards.
void append_masked(const Image& image, std::uint32_t rva, const ZydisDisassembledInstruction& ins, std::string& sig) {
    const std::uint8_t* bytes = image.file.data() + *image.offset_of(rva);
    std::vector<bool> wild(ins.info.length, false);

    bool rip_relative = false;
    for (std::uint8_t i = 0; i < ins.info.operand_count; ++i) {
        const auto& op = ins.operands[i];
        if (op.type == ZYDIS_OPERAND_TYPE_MEMORY && op.mem.base == ZYDIS_REGISTER_RIP) {
            rip_relative = true;
        }
    }
    if (rip_relative && ins.info.raw.disp.size) {
        for (std::uint8_t k = 0; k < ins.info.raw.disp.size / 8; ++k) {
            wild[ins.info.raw.disp.offset + k] = true;
        }
    }
    for (const auto& imm : ins.info.raw.imm) {
        if (imm.size && imm.is_relative) {
            for (std::uint8_t k = 0; k < imm.size / 8; ++k) {
                wild[imm.offset + k] = true;
            }
        }
    }
    for (std::uint8_t k = 0; k < ins.info.length; ++k) {
        char b[4];
        std::snprintf(b, sizeof(b), "%02X", bytes[k]);
        sig += wild[k] ? "??" : b;
        sig += ' ';
    }
}

int make_signature(const Image& image, std::uint32_t rva) {
    const IMAGE_SECTION_HEADER* text = image.section(".text");
    if (!text) {
        std::fprintf(stderr, "no .text section\n");
        return 1;
    }
    const std::span<const std::uint8_t> code(image.file.data() + text->PointerToRawData, text->SizeOfRawData);

    std::string sig;
    std::uint32_t cursor = rva;
    for (int n = 0; n < 24; ++n) {
        ZydisDisassembledInstruction ins;
        if (!decode(image, cursor, ins)) {
            break;
        }
        append_masked(image, cursor, ins, sig);
        cursor += ins.info.length;
        if (cursor - rva < 8) {
            continue;
        }
        const auto pattern = bg3vr::parse_pattern(sig);
        if (!pattern) {
            continue;
        }
        const auto hits = bg3vr::find_all(code, *pattern, 2);
        if (hits.size() == 1) {
            std::printf("%s\n", sig.c_str());
            std::printf("unique in .text (%u bytes, %d instructions)\n", cursor - rva, n + 1);
            return 0;
        }
    }
    std::printf("%s\nnot unique after 24 instructions\n", sig.c_str());
    return 2;
}

std::optional<std::uint32_t> parse_rva(const char* text) {
    std::string s(text);
    if (s.starts_with("0x") || s.starts_with("0X")) {
        s = s.substr(2);
    }
    char* end = nullptr;
    const unsigned long value = std::strtoul(s.c_str(), &end, 16);
    if (s.empty() || *end) {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(value);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: bg3vr_disasm <exe> <rva> [count] | --func <rva> | --sig <rva>\n");
        return 1;
    }
    Image image;
    if (!load(argv[1], image)) {
        std::fprintf(stderr, "cannot load PE64 file %s\n", argv[1]);
        return 1;
    }

    const std::string mode = argv[2];
    if (mode == "--vtable") {
        const auto rva = argc > 3 ? parse_rva(argv[3]) : std::nullopt;
        const std::size_t count = argc > 4 ? std::strtoul(argv[4], nullptr, 10) : 24;
        if (!rva) {
            std::fprintf(stderr, "bad rva\n");
            return 1;
        }
        const std::uint64_t image_base = image.nt->OptionalHeader.ImageBase;
        for (std::size_t slot = 0; slot < count; ++slot) {
            const auto offset = image.offset_of(*rva + static_cast<std::uint32_t>(slot * 8));
            if (!offset) {
                break;
            }
            std::uint64_t target = 0;
            std::memcpy(&target, image.file.data() + *offset, sizeof(target));
            const auto target_rva = static_cast<std::uint32_t>(target - image_base);
            ZydisDisassembledInstruction ins;
            std::string first;
            std::uint32_t cursor = target_rva;
            for (int n = 0; n < 6 && decode(image, cursor, ins); ++n) {
                first += ins.text;
                first += " ; ";
                cursor += ins.info.length;
            }
            std::printf("slot %2zu  %08X  %s\n", slot, target_rva, first.c_str());
        }
        return 0;
    }
    if (mode == "--find") {
        const auto pattern = argc > 3 ? bg3vr::parse_pattern(argv[3]) : std::nullopt;
        const IMAGE_SECTION_HEADER* text = image.section(".text");
        if (!pattern || !text) {
            std::fprintf(stderr, "bad pattern\n");
            return 1;
        }
        const std::span<const std::uint8_t> code(image.file.data() + text->PointerToRawData, text->SizeOfRawData);
        const auto hits = bg3vr::find_all(code, *pattern, 16);
        for (std::size_t hit : hits) {
            std::printf("%08zX\n", text->VirtualAddress + hit);
        }
        std::printf("%zu hit(s)%s\n", hits.size(), hits.size() >= 16 ? " (capped)" : "");
        return hits.size() == 1 ? 0 : 2;
    }
    if (mode == "--func" || mode == "--sig") {
        const auto rva = argc > 3 ? parse_rva(argv[3]) : std::nullopt;
        if (!rva) {
            std::fprintf(stderr, "bad rva\n");
            return 1;
        }
        if (mode == "--sig") {
            return make_signature(image, *rva);
        }
        const auto bounds = function_bounds(image, *rva);
        if (!bounds) {
            std::fprintf(stderr, "no .pdata entry contains %08X\n", *rva);
            return 1;
        }
        std::printf("function %08X..%08X (%u bytes), >> marks %08X\n", bounds->first, bounds->second,
                    bounds->second - bounds->first, *rva);
        print_range(image, bounds->first, bounds->second, 100000, *rva);
        return 0;
    }

    const auto rva = parse_rva(argv[2]);
    if (!rva) {
        std::fprintf(stderr, "bad rva\n");
        return 1;
    }
    const std::size_t count = argc > 3 ? std::strtoul(argv[3], nullptr, 10) : 24;
    if (const auto bounds = function_bounds(image, *rva)) {
        std::printf("in function %08X..%08X\n", bounds->first, bounds->second);
    }
    print_range(image, *rva, UINT32_MAX, count, *rva);
    return 0;
}
