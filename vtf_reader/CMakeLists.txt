# VTFLib++ (LGPL-2.1) is consumed from its own standalone build under
# thirdparty/vtflib (see PHASE 5, Step 0). Build it before configuring this
# project:
#
#   git submodule update --init --recursive
#   cmake -B thirdparty/vtflib/build_vtflib -A Win32 ^
#         -DVTFLIB_BUILD_VTFCMD=OFF -DVTFLIB_BUILD_VTFEDIT=OFF
#   cmake --build thirdparty/vtflib/build_vtflib --config Release
#   cmake --build thirdparty/vtflib/build_vtflib --config Debug
#
# Note: thirdparty/vtflib is pinned at master commit b27ecd3 (after the
# BC4/BC5/BC6H_UNSIGNED id correction) and needs one build-only source patch
# (thirdparty/patches/vtflib-resource-winres.patch: afxres.h -> winres.h, so
# the DLL resource compiles without the optional MFC component). The patch is
# applied below, idempotently, and exposed as the vtflib_apply_patch target.
set(VTFLIB_ROOT "${CMAKE_SOURCE_DIR}/thirdparty/vtflib")
set(VTFLIB_IMPORT_LIB
    "$<IF:$<CONFIG:Debug>,${VTFLIB_ROOT}/build_vtflib/VTFLib/Debug/VTFLib++_d.lib,${VTFLIB_ROOT}/build_vtflib/VTFLib/Release/VTFLib++.lib>")
set(VTFLIB_RUNTIME_DLL
    "$<IF:$<CONFIG:Debug>,${VTFLIB_ROOT}/build_vtflib/bin/VTFLib++_d.dll,${VTFLIB_ROOT}/build_vtflib/bin/VTFLib++.dll>")

# Applying the patches must happen before the standalone VTFLib++ build, so
# do it at configure time (and again on demand via the build target).
set(VTFLIB_PATCH_SCRIPT "${CMAKE_SOURCE_DIR}/thirdparty/patches/apply_vtflib_patches.cmake")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -P "${VTFLIB_PATCH_SCRIPT}"
    RESULT_VARIABLE VTFLIB_PATCH_RESULT
    OUTPUT_VARIABLE VTFLIB_PATCH_OUTPUT
    ERROR_VARIABLE VTFLIB_PATCH_OUTPUT)
if(NOT VTFLIB_PATCH_RESULT EQUAL 0)
    message(FATAL_ERROR "Preparing thirdparty/vtflib failed:\n${VTFLIB_PATCH_OUTPUT}")
endif()
if(VTFLIB_PATCH_OUTPUT)
    message(STATUS "${VTFLIB_PATCH_OUTPUT}")
endif()

add_custom_target(vtflib_apply_patch
    COMMAND "${CMAKE_COMMAND}" -P "${VTFLIB_PATCH_SCRIPT}"
    COMMENT "Ensuring the VTFLib++ patches are applied"
    VERBATIM)

if(NOT EXISTS "${VTFLIB_ROOT}/build_vtflib/VTFLib/Release/VTFLib++.lib")
    message(FATAL_ERROR
        "VTFLib++ has not been built yet (missing build_vtflib/VTFLib/Release/VTFLib++.lib).\n"
        "The Resource.rc patch has already been applied; now run:\n"
        "  cmake -B thirdparty/vtflib/build_vtflib -A Win32 -DVTFLIB_BUILD_VTFCMD=OFF -DVTFLIB_BUILD_VTFEDIT=OFF\n"
        "  cmake --build thirdparty/vtflib/build_vtflib --config Release\n"
        "  cmake --build thirdparty/vtflib/build_vtflib --config Debug")
endif()

add_library(vtf_reader STATIC
    src/vtf_reader.cpp
)

target_include_directories(vtf_reader PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# VTFLib++ headers are private: nothing outside this module may include them.
target_include_directories(vtf_reader PRIVATE
    "${VTFLIB_ROOT}/lib"
)

# vtf_reader uses vmt_core only for its shared foundation types (Result/Error,
# logging) - never for the engine, UI or Windows APIs. vmt_core must never see
# a VTFLib++ header.
target_link_libraries(vtf_reader
    PUBLIC vmt_core
    PRIVATE "${VTFLIB_IMPORT_LIB}" vmtviewer_warnings
)

# Puts the matching VTFLib++ runtime DLL next to an executable that links
# vtf_reader (the import library only satisfies the linker). The paths are
# recomputed inside the function because functions use the caller's variable
# scope, and callers live in other directories.
function(vtf_deploy_vtflib_runtime target)
    set(vtflib_root "${CMAKE_SOURCE_DIR}/thirdparty/vtflib")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "$<IF:$<CONFIG:Debug>,${vtflib_root}/build_vtflib/bin/VTFLib++_d.dll,${vtflib_root}/build_vtflib/bin/VTFLib++.dll>"
                "$<TARGET_FILE_DIR:${target}>"
        COMMENT "Copying the VTFLib++ runtime next to ${target}"
        VERBATIM)
endfunction()
