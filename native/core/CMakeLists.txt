# bg3vr_base: everything except the DLL entry points, so the unit tests can link it.
add_library(bg3vr_base STATIC
    src/blit.cpp
    src/bodyik.cpp
    src/bridge.cpp
    src/camera.cpp
    src/config.cpp
    src/cull.cpp
    src/events.cpp
    src/gpu_trace.cpp
    src/hooks.cpp
    src/log.cpp
    src/memory.cpp
    src/module_info.cpp
    src/pattern.cpp
    src/pose.cpp
    src/probe.cpp
    src/render.cpp
    src/signatures.cpp
    src/locomotion.cpp
    src/picking.cpp
    src/quality.cpp
    src/rayprobe.cpp
    src/skeleton.cpp
    src/dialogue.cpp
    src/grab_math.cpp
    src/ik.cpp
    src/stereo.cpp
    src/trace.cpp
    src/views.cpp
    src/vrplay.cpp
    src/vr_math.cpp
    src/watchdog.cpp
    src/watch.cpp
    src/win_util.cpp
)
target_include_directories(bg3vr_base PUBLIC include)
target_compile_definitions(bg3vr_base PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(bg3vr_base PUBLIC safetyhook version d3d11 d3dcompiler PRIVATE bg3vr_warnings)

# bg3vr_core: the DLL the proxy loads from <game>\bin\bg3vr\.
add_library(bg3vr_core SHARED
    src/bootstrap.cpp
    src/dllmain.cpp
)
target_link_libraries(bg3vr_core PRIVATE bg3vr_base bg3vr_warnings delayimp)
# The shader compiler is only needed when the headset starts.
target_link_options(bg3vr_core PRIVATE /DELAYLOAD:d3dcompiler_47.dll)

# The headset (OpenXR) module, when the SDK is present. openxr_loader.dll is delay-loaded so the
# core still loads, and the game still runs, on machines without it or with [xr] enabled = false.
if(BG3VR_HAVE_OPENXR)
    target_sources(bg3vr_core PRIVATE src/headset.cpp)
    target_compile_definitions(bg3vr_core PRIVATE BG3VR_HAVE_OPENXR=1)
    target_link_libraries(bg3vr_core PRIVATE bg3vr_xr delayimp)
    target_link_options(bg3vr_core PRIVATE /DELAYLOAD:openxr_loader.dll)
endif()