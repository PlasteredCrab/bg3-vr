#include <windows.h>

// All startup work happens in bg3vr_init (bootstrap.cpp), called by the proxy off the loader lock.
BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
