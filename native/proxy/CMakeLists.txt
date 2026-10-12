# version.dll proxy: the game loads it from its bin folder, it forwards to the system
# version.dll and loads bg3vr_core.dll. See docs/decisions/0001-loader.md.
add_library(bg3vr_proxy SHARED
    version_proxy.cpp
    version_stubs.asm
    version.def
    version.rc
)
set_target_properties(bg3vr_proxy PROPERTIES
    OUTPUT_NAME version
    # Keep the import library away from the system version.lib.
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/proxy-implib
)
target_compile_definitions(bg3vr_proxy PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
