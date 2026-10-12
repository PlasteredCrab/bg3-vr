#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bg3vr {

struct Section {
    std::string name;
    std::uintptr_t start = 0;
    std::size_t size = 0;
};

struct ModuleInfo {
    std::wstring path;
    std::uintptr_t base = 0;
    std::size_t image_size = 0;
    std::vector<Section> sections;
    std::wstring file_version;
};

enum class RendererApi { Unknown, D3D11, Vulkan };

ModuleInfo query_main_module();
const Section* find_section(const ModuleInfo& module, std::string_view name);

// bg3_dx11.exe is the DX11 build, bg3.exe the Vulkan build.
RendererApi guess_renderer(const ModuleInfo& module);
std::string_view to_string(RendererApi api);

} // namespace bg3vr
