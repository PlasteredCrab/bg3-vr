#include "bg3vr/module_info.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <vector>

namespace bg3vr {

namespace {

std::wstring query_file_version(const std::wstring& path) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0) {
        return L"unknown";
    }
    std::vector<std::byte> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) {
        return L"unknown";
    }

    // BG3 leaves FileVersion at 1.0.0.0; the real game version is the ProductVersion string.
    struct Translation {
        WORD language;
        WORD codepage;
    };
    Translation* translations = nullptr;
    UINT length = 0;
    if (VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &length) &&
        length >= sizeof(Translation)) {
        const std::wstring key = std::format(L"\\StringFileInfo\\{:04x}{:04x}\\ProductVersion", translations->language,
                                             translations->codepage);
        wchar_t* product = nullptr;
        UINT chars = 0;
        // Stop at the NUL: BG3's resource reports a length that runs past the string.
        if (VerQueryValueW(data.data(), key.c_str(), reinterpret_cast<void**>(&product), &chars) && chars > 1) {
            return std::wstring(product, wcsnlen(product, chars));
        }
    }

    VS_FIXEDFILEINFO* info = nullptr;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info) {
        return L"unknown";
    }
    return std::format(L"{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS),
                       HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
}

} // namespace

ModuleInfo query_main_module() {
    ModuleInfo info;
    HMODULE exe = GetModuleHandleW(nullptr);
    info.base = reinterpret_cast<std::uintptr_t>(exe);

    wchar_t buffer[MAX_PATH * 2];
    const DWORD length = GetModuleFileNameW(exe, buffer, static_cast<DWORD>(std::size(buffer)));
    info.path.assign(buffer, length);

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(info.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(info.base + dos->e_lfanew);
    info.image_size = nt->OptionalHeader.SizeOfImage;

    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        char name[IMAGE_SIZEOF_SHORT_NAME + 1] = {};
        std::memcpy(name, section->Name, IMAGE_SIZEOF_SHORT_NAME);
        const std::size_t size = section->Misc.VirtualSize ? section->Misc.VirtualSize : section->SizeOfRawData;
        info.sections.push_back({name, info.base + section->VirtualAddress, size});
    }

    info.file_version = query_file_version(info.path);
    return info;
}

const Section* find_section(const ModuleInfo& module, std::string_view name) {
    const auto it = std::ranges::find_if(module.sections, [name](const Section& s) { return s.name == name; });
    return it == module.sections.end() ? nullptr : &*it;
}

RendererApi guess_renderer(const ModuleInfo& module) {
    const std::wstring name = std::filesystem::path(module.path).filename().wstring();
    if (_wcsicmp(name.c_str(), L"bg3_dx11.exe") == 0) {
        return RendererApi::D3D11;
    }
    if (_wcsicmp(name.c_str(), L"bg3.exe") == 0) {
        return RendererApi::Vulkan;
    }
    return RendererApi::Unknown;
}

std::string_view to_string(RendererApi api) {
    switch (api) {
    case RendererApi::D3D11: return "D3D11";
    case RendererApi::Vulkan: return "Vulkan";
    case RendererApi::Unknown: return "unknown";
    }
    return "unknown";
}

} // namespace bg3vr
