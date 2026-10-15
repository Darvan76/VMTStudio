cmake_minimum_required(VERSION 3.21)

project(VMTStudio
    VERSION 0.1.0
    DESCRIPTION "Native Windows material studio for Source Engine VMT/VTF"
    LANGUAGES CXX
)

if(NOT DEFINED CMAKE_PREFIX_PATH AND EXISTS "C:/Qt/6.8.2/msvc2022_64")
    list(APPEND CMAKE_PREFIX_PATH "C:/Qt/6.8.2/msvc2022_64")
endif()

# --- Language settings --------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Toolchain guards ---------------------------------------------------------
if(NOT MSVC)
    message(FATAL_ERROR
        "VMT-Studio currently targets MSVC only. Configure with a Visual Studio generator.")
endif()

# --- Runtime library ----------------------------------------------------------
# Static CRT for 32-bit standalone build; dynamic CRT when building with Qt 6 (64-bit)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
else()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

# --- Warnings -----------------------------------------------------------------
# Applied to our own targets only, never to third-party or (later) Valve SDK headers.
add_library(vmtstudio_warnings INTERFACE)
target_compile_options(vmtstudio_warnings INTERFACE
    $<$<CXX_COMPILER_ID:MSVC>:/W4 /WX /permissive- /Zc:__cplusplus /utf-8>
)
add_library(vmtviewer_warnings ALIAS vmtstudio_warnings)

# --- Sanitizers ---------------------------------------------------------------
# AddressSanitizer (ASan) configuration per .agent/skills/address-sanitizer/SKILL.md
option(ENABLE_ASAN "Enable AddressSanitizer (ASan) for memory safety checks" OFF)
if(ENABLE_ASAN)
    if(MSVC)
        message(STATUS "AddressSanitizer enabled (/fsanitize=address)")
        add_compile_options(/fsanitize=address)
    else()
        message(STATUS "AddressSanitizer enabled (-fsanitize=address)")
        add_compile_options(-fsanitize=address)
        add_link_options(-fsanitize=address)
    endif()
endif()

# --- Output layout ------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# Single-config generators default to Release; VS uses its own configuration set.
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Build-time location of the Source SDK 2013 public headers.
if(DEFINED ENV{SOURCE_SDK_PUBLIC})
    set(DEFAULT_SOURCE_SDK_PUBLIC "$ENV{SOURCE_SDK_PUBLIC}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/source-sdk-2013/src/public")
    set(DEFAULT_SOURCE_SDK_PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/source-sdk-2013/src/public")
elseif(EXISTS "C:/Users/Dario/Documents/source-sdk-2013/source-sdk-2013/src/public")
    set(DEFAULT_SOURCE_SDK_PUBLIC "C:/Users/Dario/Documents/source-sdk-2013/source-sdk-2013/src/public")
else()
    set(DEFAULT_SOURCE_SDK_PUBLIC "")
endif()
set(SOURCE_SDK_PUBLIC "${DEFAULT_SOURCE_SDK_PUBLIC}" CACHE PATH "Source SDK 2013 src/public directory")

enable_testing()

add_subdirectory(vmt_core)
add_subdirectory(vtf_reader)
add_subdirectory(renderer)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_subdirectory(hlmv_backend)
endif()
add_subdirectory(engine_bridge)
add_subdirectory(game_profiles)
add_subdirectory(ui)
add_subdirectory(tests)
