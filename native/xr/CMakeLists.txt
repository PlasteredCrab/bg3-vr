# bg3vr_xr: OpenXR + D3D11 runtime/session code shared by tools/xr_test and (later) the core.
add_library(bg3vr_xr STATIC src/xr.cpp)
target_include_directories(bg3vr_xr PUBLIC include)
target_compile_definitions(bg3vr_xr PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(bg3vr_xr PUBLIC openxr_loader d3d11 dxgi PRIVATE bg3vr_warnings)
