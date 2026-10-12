# SafetyHook v0.7.0, amalgamated with Zydis v4.0.0 (static). See README.md.
add_library(safetyhook STATIC
    safetyhook.cpp
    Zydis.c
)
target_include_directories(safetyhook SYSTEM PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(safetyhook PRIVATE /W0)
set_target_properties(safetyhook PROPERTIES C_STANDARD 11)
