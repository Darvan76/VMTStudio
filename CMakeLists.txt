cmake_minimum_required(VERSION 3.21)

project(VMTViewer
    VERSION 0.1.0
    DESCRIPTION "Native Windows viewer/editor for Source Engine VMT/VTF"
    LANGUAGES CXX
)

# --- Language settings --------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Toolchain guards ---------------------------------------------------------
if(NOT MSVC)
    message(FATAL_ERROR
        "VMTViewer currently targets MSVC only. Configure with a Visual Studio "
        "generator, e.g. cmake -S . -B build -G \"Visual Studio 17 2022\" -A Win32")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "VMTViewer must be built as 32-bit (x86). Configure with -A Win32 "
        "(detected pointer size: ${CMAKE_SIZEOF_VOID_P}).")
endif()

# --- Runtime library: static CRT (/MT, /MTd) ----------------------------------
# Phase 0 decision: avoids a VC++ Redistributable dependency. We never pass
# allocator-owned C++ objects across the engine DLL boundary, so static CRT is safe.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# --- Warnings -----------------------------------------------------------------
# Applied to our own targets only, never to third-party or (later) Valve SDK headers.
add_library(vmtviewer_warnings INTERFACE)
target_compile_options(vmtviewer_warnings INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive- /Zc:__cplusplus /utf-8>
)

# --- Output layout ------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# Single-config generators default to Release; VS uses its own configuration set.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

enable_testing()

add_subdirectory(vmt_core)
add_subdirectory(vtf_reader)
add_subdirectory(renderer)
add_subdirectory(engine_bridge)
add_subdirectory(game_profiles)
add_subdirectory(ui)
add_subdirectory(tests)
