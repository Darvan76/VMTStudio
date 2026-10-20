cmake_minimum_required(VERSION 3.21)

project(VMTStudioPortable LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(VTFLIB_MINIZ_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/vtflib/thirdparty/miniz")

# Ensure payload.zip exists in the launcher source directory or copy from dist/
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../dist/vmtstudio-v0.2.0-win64.zip")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/../../dist/vmtstudio-v0.2.0-win64.zip"
        "${CMAKE_CURRENT_BINARY_DIR}/payload.zip"
        COPYONLY
    )
endif()

add_executable(VMTStudioPortable WIN32
    main.cpp
    miniz_export.h
    launcher.rc
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
    OUTPUT_NAME "VMT-Studio-v0.2.0-Portable"
)

target_link_libraries(VMTStudioPortable PRIVATE
    shell32
    ole32
    user32
    advapi32
)
