# bg3vr_base: utilities shared by the core DLL and the unit tests.
add_library(bg3vr_base STATIC
    src/camera.cpp
    src/config.cpp
    src/hooks.cpp
    src/log.cpp
    src/module_info.cpp
    src/pattern.cpp
    src/signatures.cpp
    src/win_util.cpp
)
target_include_directories(bg3vr_base PUBLIC include)
target_compile_definitions(bg3vr_base PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(bg3vr_base PUBLIC safetyhook version PRIVATE bg3vr_warnings)

# bg3vr_core: the DLL the proxy loads from <game>\bin\bg3vr\.
add_library(bg3vr_core SHARED
    src/bootstrap.cpp
    src/dllmain.cpp
)
target_link_libraries(bg3vr_core PRIVATE bg3vr_base bg3vr_warnings)
