cmake_minimum_required(VERSION 3.21)

# Keep the launcher aligned with the application even when built independently.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../../CMakeLists.txt" ROOT_VERSION_LINE
    REGEX "^[ \t]+VERSION[ \t]+[0-9]+\\.[0-9]+\\.[0-9]+")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" ROOT_PROJECT_VERSION "${ROOT_VERSION_LINE}")
if(NOT ROOT_PROJECT_VERSION)
    message(FATAL_ERROR "Cannot read the VMT Studio version from the root project")
endif()
set(VMTSTUDIO_PORTABLE_VERSION "${ROOT_PROJECT_VERSION}" CACHE STRING "Portable application version")
if(NOT VMTSTUDIO_PORTABLE_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "VMTSTUDIO_PORTABLE_VERSION must be a major.minor.patch version")
endif()

project(VMTStudioPortable VERSION "${VMTSTUDIO_PORTABLE_VERSION}" LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(VTFLIB_MINIZ_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/vtflib/thirdparty/miniz")

set(VMTSTUDIO_PAYLOAD
    "${CMAKE_CURRENT_SOURCE_DIR}/../../dist/vmtstudio-v${PROJECT_VERSION}-win64.zip"
    CACHE FILEPATH "Exact distribution ZIP embedded by this launcher")
get_filename_component(VMTSTUDIO_PAYLOAD "${VMTSTUDIO_PAYLOAD}" ABSOLUTE
    BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
if(NOT EXISTS "${VMTSTUDIO_PAYLOAD}" OR IS_DIRECTORY "${VMTSTUDIO_PAYLOAD}")
    message(FATAL_ERROR "Portable payload does not exist: ${VMTSTUDIO_PAYLOAD}")
endif()
set(VMTSTUDIO_LAUNCHER_ICON "${CMAKE_CURRENT_SOURCE_DIR}/../../ui/win32/app.ico")
set(VMTSTUDIO_LAUNCHER_OUTPUT "VMT-Studio-v${PROJECT_VERSION}-Portable")
configure_file(launcher.rc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/launcher.rc" @ONLY)
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/generated/launcher.rc"
    PROPERTIES OBJECT_DEPENDS "${VMTSTUDIO_PAYLOAD}")

add_executable(VMTStudioPortable WIN32
    main.cpp
    miniz_export.h
    "${CMAKE_CURRENT_BINARY_DIR}/generated/launcher.rc"
    "${VTFLIB_MINIZ_DIR}/miniz.c"
    "${VTFLIB_MINIZ_DIR}/miniz_tdef.c"
    "${VTFLIB_MINIZ_DIR}/miniz_tinfl.c"
    "${VTFLIB_MINIZ_DIR}/miniz_zip.c"
)

target_include_directories(VMTStudioPortable PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${VTFLIB_MINIZ_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}"
)

set_target_properties(VMTStudioPortable PROPERTIES
    OUTPUT_NAME "${VMTSTUDIO_LAUNCHER_OUTPUT}"
)

target_compile_definitions(VMTStudioPortable PRIVATE
    VMTSTUDIO_PORTABLE_VERSION="${PROJECT_VERSION}"
)

target_link_libraries(VMTStudioPortable PRIVATE
    shell32
    ole32
    user32
    advapi32
)
