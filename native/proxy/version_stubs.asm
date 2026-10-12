; Export trampolines for the version.dll proxy. Each export jumps straight into the
; system version.dll, so arguments and return values pass through untouched.
; Slot order must match kExports in version_proxy.cpp.

EXTERN g_version_procs:QWORD

.code

GetFileVersionInfoA_stub PROC
    jmp QWORD PTR [g_version_procs + 0 * 8]
GetFileVersionInfoA_stub ENDP

GetFileVersionInfoByHandle_stub PROC
    jmp QWORD PTR [g_version_procs + 1 * 8]
GetFileVersionInfoByHandle_stub ENDP

GetFileVersionInfoExA_stub PROC
    jmp QWORD PTR [g_version_procs + 2 * 8]
GetFileVersionInfoExA_stub ENDP

GetFileVersionInfoExW_stub PROC
    jmp QWORD PTR [g_version_procs + 3 * 8]
GetFileVersionInfoExW_stub ENDP

GetFileVersionInfoSizeA_stub PROC
    jmp QWORD PTR [g_version_procs + 4 * 8]
GetFileVersionInfoSizeA_stub ENDP

GetFileVersionInfoSizeExA_stub PROC
    jmp QWORD PTR [g_version_procs + 5 * 8]
GetFileVersionInfoSizeExA_stub ENDP

GetFileVersionInfoSizeExW_stub PROC
    jmp QWORD PTR [g_version_procs + 6 * 8]
GetFileVersionInfoSizeExW_stub ENDP

GetFileVersionInfoSizeW_stub PROC
    jmp QWORD PTR [g_version_procs + 7 * 8]
GetFileVersionInfoSizeW_stub ENDP

GetFileVersionInfoW_stub PROC
    jmp QWORD PTR [g_version_procs + 8 * 8]
GetFileVersionInfoW_stub ENDP

VerFindFileA_stub PROC
    jmp QWORD PTR [g_version_procs + 9 * 8]
VerFindFileA_stub ENDP

VerFindFileW_stub PROC
    jmp QWORD PTR [g_version_procs + 10 * 8]
VerFindFileW_stub ENDP

VerInstallFileA_stub PROC
    jmp QWORD PTR [g_version_procs + 11 * 8]
VerInstallFileA_stub ENDP

VerInstallFileW_stub PROC
    jmp QWORD PTR [g_version_procs + 12 * 8]
VerInstallFileW_stub ENDP

VerLanguageNameA_stub PROC
    jmp QWORD PTR [g_version_procs + 13 * 8]
VerLanguageNameA_stub ENDP

VerLanguageNameW_stub PROC
    jmp QWORD PTR [g_version_procs + 14 * 8]
VerLanguageNameW_stub ENDP

VerQueryValueA_stub PROC
    jmp QWORD PTR [g_version_procs + 15 * 8]
VerQueryValueA_stub ENDP

VerQueryValueW_stub PROC
    jmp QWORD PTR [g_version_procs + 16 * 8]
VerQueryValueW_stub ENDP

END
