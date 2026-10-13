// bg3vr_disasm: offline disassembly and signature generation for a PE file (the game exe).
//
//   bg3vr_disasm <exe> <rva> [count]     disassemble `count` instructions (default 24) from rva
//   bg3vr_disasm <exe> --func <rva>      disassemble the whole function containing rva (.pdata)
//   bg3vr_disasm <exe> --sig <rva>       shortest signature starting at rva that is unique in .text
//   bg3vr_disasm <exe> --find "<sig>"    where a signature matches in .text (exit 0 if exactly once)
//   bg3vr_disasm <exe> --vtable <rva> [n] list n vtable slots (default 24) with their first instructions
//   bg3vr_disasm <exe> --xref <rva>      instructions that call, jump to, or RIP-address rva
//   bg3vr_disasm <exe> --str <text>      ASCII and UTF-16 occurrences of text, as RVAs
//   bg3vr_disasm <exe> --ptr <rva>       8-byte absolute pointers to rva outside .text (vtable slots, tables)
//   bg3vr_disasm <exe> --rtti <rva>      the class name of the vtable at rva (MSVC RTTI), and its bases
//
// RVAs are hex (with or without 0x). Signatures wildcard relative branch targets and RIP-relative
// displacements, the parts that change between game builds.

#include "bg3vr/pattern.hpp"

#include <Zydis.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

// If rva points at a printable ASCII or UTF-16 string (4+ chars), returns it for annotation.
std::string string_at(const Image& image, std::uint32_t rva) {
    const auto offset = image.offset_of(rva);
    if (!offset) {
        return {};
    }
    const auto printable = [](std::uint8_t c) { return c >= 0x20 && c < 0x7F; };
    std::string text;
    const std::uint8_t* p = image.file.data() + *offset;
    const std::size_t left = image.file.size() - *offset;
    const bool wide = left > 2 && printable(p[0]) && p[1] == 0;
    const std::size_t step = wide ? 2 : 1;
    for (std::size_t k = 0; k + step <= left && text.size() < 80; k += step) {
        if (!printable(p[k]) || (wide && p[k + 1] != 0)) {
            if (p[k] != 0) {
                return {};
            }
            break;
        }
        text += static_cast<char>(p[k]);
    }
    return text.size() >= 4 ? text : std::string{};
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
        std::string note;
        for (std::uint8_t op = 0; op < ins.info.operand_count_visible; ++op) {
            ZyanU64 absolute = 0;
            if (ins.operands[op].type == ZYDIS_OPERAND_TYPE_MEMORY && ins.operands[op].mem.base == ZYDIS_REGISTER_RIP &&
                ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&ins.info, &ins.operands[op], rva, &absolute))) {
                const std::string text = string_at(image, static_cast<std::uint32_t>(absolute));
                if (!text.empty()) {
                    note = "  ; \"" + text + "\"";
                }
            }
        }
        std::printf("%s %08X  %-36s %s%s\n", rva == mark ? ">>" : "  ", rva, hex.c_str(), ins.text, note.c_str());
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

std::optional<std::uint32_t> rva_of_offset(const Image& image, std::size_t offset) {
    for (const auto& s : image.sections) {
        if (offset >= s.PointerToRawData && offset < s.PointerToRawData + s.SizeOfRawData) {
            return static_cast<std::uint32_t>(s.VirtualAddress + (offset - s.PointerToRawData));
        }
    }
    return std::nullopt;
}

// Every ASCII and UTF-16 occurrence of `needle` in the file, as RVAs with section names.
int find_strings(const Image& image, const std::string& needle) {
    std::vector<std::uint8_t> wide;
    for (char c : needle) {
        wide.push_back(static_cast<std::uint8_t>(c));
        wide.push_back(0);
    }
    const std::vector<std::uint8_t> narrow(needle.begin(), needle.end());
    int hits = 0;
    const std::array<const std::vector<std::uint8_t>*, 2> patterns = {&narrow, &wide};
    for (const auto* pattern : patterns) {
        auto it = image.file.begin();
        while ((it = std::search(it, image.file.end(), pattern->begin(), pattern->end())) != image.file.end()) {
            const std::size_t offset = static_cast<std::size_t>(it - image.file.begin());
            if (const auto rva = rva_of_offset(image, offset)) {
                // Print the whole zero-terminated string around the match.
                std::size_t start = offset;
                const std::size_t step = pattern == &wide ? 2 : 1;
                while (start >= step && image.file[start - step] >= 0x20 && image.file[start - step] < 0x7F &&
                       (step == 1 || image.file[start - step + 1] == 0)) {
                    start -= step;
                }
                std::string text;
                for (std::size_t k = start; k < image.file.size() && image.file[k] >= 0x20 && image.file[k] < 0x7F &&
                                            (step == 1 || image.file[k + 1] == 0) && text.size() < 160;
                     k += step) {
                    text += static_cast<char>(image.file[k]);
                }
                std::printf("%08X  %s  \"%s\"\n", *rva_of_offset(image, start), step == 2 ? "utf16" : "ascii", text.c_str());
                ++hits;
            }
            ++it;
        }
    }
    std::printf("%d hit(s)\n", hits);
    return hits ? 0 : 2;
}

// Every instruction in .pdata functions that calls, jumps to, or addresses `target` (RIP-relative).
int find_xrefs(const Image& image, std::uint32_t target) {
    const auto& dir = image.nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    const auto table = image.offset_of(dir.VirtualAddress);
    if (!table) {
        return 1;
    }
    const auto* entries = reinterpret_cast<const RUNTIME_FUNCTION*>(image.file.data() + *table);
    const std::size_t count = dir.Size / sizeof(RUNTIME_FUNCTION);

    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    ZydisDecodedInstruction ins;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    int hits = 0;
    std::uint32_t last_end = 0;
    for (std::size_t f = 0; f < count; ++f) {
        // Chained unwind entries repeat ranges; skip anything already covered.
        std::uint32_t rva = std::max<std::uint32_t>(entries[f].BeginAddress, last_end);
        const std::uint32_t end = entries[f].EndAddress;
        last_end = std::max(last_end, end);
        const auto base = image.offset_of(rva);
        if (!base || rva >= end) {
            continue;
        }
        const std::uint8_t* code = image.file.data() + *base;
        std::size_t pos = 0;
        while (rva + pos < end) {
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code + pos, end - rva - pos, &ins, ops))) {
                ++pos;
                continue;
            }
            const std::uint32_t here = rva + static_cast<std::uint32_t>(pos);
            for (std::uint8_t i = 0; i < ins.operand_count_visible; ++i) {
                const bool relative = (ops[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && ops[i].imm.is_relative) ||
                                      (ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY && ops[i].mem.base == ZYDIS_REGISTER_RIP);
                ZyanU64 absolute = 0;
                if (relative && ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&ins, &ops[i], here, &absolute)) &&
                    absolute == target) {
                    ZydisDisassembledInstruction text;
                    decode(image, here, text);
                    std::printf("%08X  in fn %08X  %s\n", here, entries[f].BeginAddress, text.text);
                    ++hits;
                }
            }
            pos += ins.length;
        }
    }
    std::printf("%d xref(s)\n", hits);
    return hits ? 0 : 2;
}

// Every aligned 8-byte absolute pointer to `target` in the data sections: vtable slots and function
// tables. For a vtable hit, --vtable on (hit - 8 * slot) lists the rest.
int find_pointers(const Image& image, std::uint32_t target) {
    const std::uint64_t absolute = image.nt->OptionalHeader.ImageBase + target;
    int hits = 0;
    for (const auto& s : image.sections) {
        if (std::strncmp(reinterpret_cast<const char*>(s.Name), ".text", IMAGE_SIZEOF_SHORT_NAME) == 0) {
            continue;
        }
        for (std::size_t at = 0; at + 8 <= s.SizeOfRawData; at += 8) {
            std::uint64_t value = 0;
            std::memcpy(&value, image.file.data() + s.PointerToRawData + at, sizeof(value));
            if (value == absolute) {
                std::printf("%08zX  in %.8s\n", s.VirtualAddress + at, reinterpret_cast<const char*>(s.Name));
                ++hits;
            }
        }
    }
    std::printf("%d pointer(s)\n", hits);
    return hits ? 0 : 2;
}

// MSVC x64 RTTI: vtable[-1] is the CompleteObjectLocator (signature 1, then offsets, then RVAs of the
// TypeDescriptor and the ClassHierarchyDescriptor); a TypeDescriptor's decorated name is at +0x10.
std::string type_name(const Image& image, std::uint32_t type_descriptor) {
    const auto offset = image.offset_of(type_descriptor + 0x10);
    if (!offset) {
        return "?";
    }
    std::string name;
    for (std::size_t i = *offset; i < image.file.size() && image.file[i] && name.size() < 200; ++i) {
        name += static_cast<char>(image.file[i]);
    }
    return name;
}

int print_rtti(const Image& image, std::uint32_t vtable) {
    const std::uint64_t image_base = image.nt->OptionalHeader.ImageBase;
    const auto slot = image.offset_of(vtable - 8);
    if (!slot) {
        return 1;
    }
    std::uint64_t locator = 0;
    std::memcpy(&locator, image.file.data() + *slot, sizeof(locator));
    const auto col = image.offset_of(static_cast<std::uint32_t>(locator - image_base));
    if (!col || image.file[*col] != 1) {
        std::printf("no RTTI locator before %08X\n", vtable);
        return 2;
    }
    std::uint32_t this_offset = 0, type = 0, hierarchy = 0;
    std::memcpy(&this_offset, image.file.data() + *col + 4, 4);
    std::memcpy(&type, image.file.data() + *col + 0x0C, 4);
    std::memcpy(&hierarchy, image.file.data() + *col + 0x10, 4);
    std::printf("vtable %08X: %s (this at +0x%X in the complete object)\n", vtable, type_name(image, type).c_str(),
                this_offset);
    const auto chd = image.offset_of(hierarchy);
    if (!chd) {
        return 0;
    }
    std::uint32_t count = 0, bases = 0;
    std::memcpy(&count, image.file.data() + *chd + 8, 4);
    std::memcpy(&bases, image.file.data() + *chd + 0x0C, 4);
    for (std::uint32_t i = 1; i < count && i < 32; ++i) {
        const auto entry = image.offset_of(bases + i * 4);
        if (!entry) {
            break;
        }
        std::uint32_t base_descriptor = 0, base_type = 0;
        std::memcpy(&base_descriptor, image.file.data() + *entry, 4);
        if (const auto bd = image.offset_of(base_descriptor)) {
            std::memcpy(&base_type, image.file.data() + *bd, 4);
            std::printf("  base %s\n", type_name(image, base_type).c_str());
        }
    }
    return 0;
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
    if (mode == "--str") {
        if (argc < 4) {
            std::fprintf(stderr, "usage: --str <text>\n");
            return 1;
        }
        return find_strings(image, argv[3]);
    }
    if (mode == "--xref") {
        const auto rva = argc > 3 ? parse_rva(argv[3]) : std::nullopt;
        if (!rva) {
            std::fprintf(stderr, "bad rva\n");
            return 1;
        }
        return find_xrefs(image, *rva);
    }
    if (mode == "--rtti") {
        const auto rva = argc > 3 ? parse_rva(argv[3]) : std::nullopt;
        if (!rva) {
            std::fprintf(stderr, "bad rva\n");
            return 1;
        }
        return print_rtti(image, *rva);
    }
    if (mode == "--ptr") {
        const auto rva = argc > 3 ? parse_rva(argv[3]) : std::nullopt;
        if (!rva) {
            std::fprintf(stderr, "bad rva\n");
            return 1;
        }
        return find_pointers(image, *rva);
    }
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
