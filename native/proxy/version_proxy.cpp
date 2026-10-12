// version.dll proxy. Every export jumps through g_version_procs into the system copy
// (see version_stubs.asm). Inside the game process it also loads bg3vr\bg3vr_core.dll
// on a worker thread, so nothing heavy runs under the loader lock.

#include <windows.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <string>

// Order must match version_stubs.asm.
extern "C" FARPROC g_version_procs[17] = {};

namespace {

constexpr std::array<const char*, 17> kExports = {
    "GetFileVersionInfoA",     "GetFileVersionInfoByHandle", "GetFileVersionInfoExA",
    "GetFileVersionInfoExW",   "GetFileVersionInfoSizeA",    "GetFileVersionInfoSizeExA",
    "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW",  "GetFileVersionInfoW",
    "VerFindFileA",            "VerFindFileW",               "VerInstallFileA",
    "VerInstallFileW",         "VerLanguageNameA",           "VerLanguageNameW",
    "VerQueryValueA",          "VerQueryValueW",
};

HMODULE g_self = nullptr;

void proxy_log(const std::filesystem::path& dir, const std::wstring& message) {
    OutputDebugStringW((L"[bg3vr proxy] " + message + L"\n").c_str());
    std::wofstream out(dir / L"proxy.log", std::ios::app);
    out << message << L'\n';
}

bool load_real_version_dll() {
    wchar_t system_dir[MAX_PATH];
    const UINT length = GetSystemDirectoryW(system_dir, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return false;
    }
    const std::wstring path = std::wstring(system_dir, length) + L"\\version.dll";
    HMODULE real = LoadLibraryW(path.c_str());
    if (!real) {
        return false;
    }
    for (std::size_t i = 0; i < kExports.size(); ++i) {
        g_version_procs[i] = GetProcAddress(real, kExports[i]);
    }
    return true;
}

std::filesystem::path module_path(HMODULE module) {
    wchar_t buffer[MAX_PATH * 2];
    const DWORD length = GetModuleFileNameW(module, buffer, static_cast<DWORD>(std::size(buffer)));
    return std::filesystem::path(std::wstring(buffer, length));
}

bool is_game_process() {
    const std::wstring name = module_path(nullptr).filename().wstring();
    return _wcsicmp(name.c_str(), L"bg3.exe") == 0 || _wcsicmp(name.c_str(), L"bg3_dx11.exe") == 0;
}

DWORD WINAPI load_core(void*) {
    const std::filesystem::path root = module_path(g_self).parent_path() / L"bg3vr";

    std::error_code ec;
    if (std::filesystem::exists(root / L"disabled", ec)) {
        proxy_log(root, L"'disabled' marker present; core not loaded");
        return 0;
    }

    const std::filesystem::path core = root / L"bg3vr_core.dll";
    HMODULE module = LoadLibraryExW(core.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module) {
        proxy_log(root, L"failed to load " + core.wstring() + L" (error " + std::to_wstring(GetLastError()) + L")");
        return 1;
    }

    using InitFn = int (*)(const wchar_t*);
    const auto init = reinterpret_cast<InitFn>(GetProcAddress(module, "bg3vr_init"));
    if (!init) {
        proxy_log(root, L"bg3vr_core.dll has no bg3vr_init export");
        return 1;
    }

    const int result = init(root.c_str());
    if (result != 0) {
        proxy_log(root, L"bg3vr_init failed with code " + std::to_wstring(result));
    }
    return static_cast<DWORD>(result);
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) {
        return TRUE;
    }
    g_self = module;
    DisableThreadLibraryCalls(module);

    if (!load_real_version_dll()) {
        return FALSE;
    }
    if (is_game_process()) {
        if (HANDLE thread = CreateThread(nullptr, 0, load_core, nullptr, 0, nullptr)) {
            CloseHandle(thread);
        }
    }
    return TRUE;
}
