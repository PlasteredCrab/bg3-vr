cmake_minimum_required(VERSION 3.28)

project(bg3vr VERSION 0.0.1 LANGUAGES C CXX ASM_MASM)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "BG3 VR is 64-bit only.")
endif()

# C++23 for std::expected (SafetyHook).
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT: injected DLLs should not depend on whichever CRT the game ships.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_compile_options(
    $<$<COMPILE_LANGUAGE:C,CXX>:/utf-8>
    $<$<COMPILE_LANGUAGE:C,CXX>:/Zi>
)
add_link_options(/DEBUG)

# Strict warnings for our own code only; link this into every first-party target.
add_library(bg3vr_warnings INTERFACE)
target_compile_options(bg3vr_warnings INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:/W4>
    $<$<COMPILE_LANGUAGE:CXX>:/permissive->
)

option(BG3VR_BUILD_TESTS "Build native unit tests" ON)

add_subdirectory(third_party/safetyhook)

# OpenXR loader from the official package fetched by tools/fetch-openxr.ps1 (git-ignored download).
set(BG3VR_OPENXR_SDK "${CMAKE_SOURCE_DIR}/downloads/openxr-1.1.63/extracted" CACHE PATH "Extracted OpenXR loader package")
if(EXISTS "${BG3VR_OPENXR_SDK}/include/openxr/openxr.h")
    add_library(openxr_loader SHARED IMPORTED GLOBAL)
    set_target_properties(openxr_loader PROPERTIES
        IMPORTED_LOCATION "${BG3VR_OPENXR_SDK}/native/x64/release/bin/openxr_loader.dll"
        IMPORTED_IMPLIB "${BG3VR_OPENXR_SDK}/native/x64/release/lib/openxr_loader.lib"
        INTERFACE_INCLUDE_DIRECTORIES "${BG3VR_OPENXR_SDK}/include")
    set(BG3VR_HAVE_OPENXR ON)
else()
    message(STATUS "OpenXR SDK not found at ${BG3VR_OPENXR_SDK}; run tools/fetch-openxr.ps1. XR targets skipped.")
    set(BG3VR_HAVE_OPENXR OFF)
endif()

add_subdirectory(native)
add_subdirectory(tools/disasm)
if(BG3VR_HAVE_OPENXR)
    add_subdirectory(tools/xr_test)
endif()

if(BG3VR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/native)
endif()
