# apply_vtflib_patches.cmake
#
# Applies the VMTViewer patches to the pinned VTFLib++ submodule if they are
# not applied yet. Idempotent: safe to run at every configure/build.
#
# Usage:
#   cmake -P thirdparty/patches/apply_vtflib_patches.cmake
#
# or from the viewer build tree:
#   cmake --build build --target vtflib_apply_patch

cmake_minimum_required(VERSION 3.21)

get_filename_component(VMTVIEWER_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(VTFLIB_DIR "${VMTVIEWER_ROOT}/thirdparty/vtflib")
set(VTFLIB_PATCH "${VMTVIEWER_ROOT}/thirdparty/patches/vtflib-resource-winres.patch")

if(NOT EXISTS "${VTFLIB_DIR}/VTFLib/Resource.rc")
    message(FATAL_ERROR
        "thirdparty/vtflib is not checked out. Run:\n"
        "  git submodule update --init --recursive")
endif()

if(NOT EXISTS "${VTFLIB_PATCH}")
    message(FATAL_ERROR "Missing patch file: ${VTFLIB_PATCH}")
endif()

file(READ "${VTFLIB_DIR}/VTFLib/Resource.rc" VTFLIB_RESOURCE_TEXT)

# The current patch is the only one; its marker is the winres.h include.
string(FIND "${VTFLIB_RESOURCE_TEXT}" "winres.h" WINRES_POSITION)
if(WINRES_POSITION GREATER -1)
    message(STATUS "VTFLib++ patches already applied")
    return()
endif()

find_program(VMTVIEWER_GIT_EXECUTABLE NAMES git)
if(NOT VMTVIEWER_GIT_EXECUTABLE)
    message(FATAL_ERROR
        "git was not found; apply ${VTFLIB_PATCH} to thirdparty/vtflib manually")
endif()

execute_process(
    COMMAND "${VMTVIEWER_GIT_EXECUTABLE}" -C "${VTFLIB_DIR}" apply --check "${VTFLIB_PATCH}"
    RESULT_VARIABLE VTFLIB_CHECK_RESULT
    OUTPUT_VARIABLE VTFLIB_CHECK_OUTPUT
    ERROR_VARIABLE VTFLIB_CHECK_OUTPUT)
if(NOT VTFLIB_CHECK_RESULT EQUAL 0)
    message(FATAL_ERROR
        "The VTFLib++ patch does not apply cleanly:\n${VTFLIB_CHECK_OUTPUT}\n"
        "Check the state of thirdparty/vtflib (git status) or update the patch file.")
endif()

execute_process(
    COMMAND "${VMTVIEWER_GIT_EXECUTABLE}" -C "${VTFLIB_DIR}" apply "${VTFLIB_PATCH}"
    RESULT_VARIABLE VTFLIB_APPLY_RESULT
    OUTPUT_VARIABLE VTFLIB_APPLY_OUTPUT
    ERROR_VARIABLE VTFLIB_APPLY_OUTPUT)
if(NOT VTFLIB_APPLY_RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to apply ${VTFLIB_PATCH}:\n${VTFLIB_APPLY_OUTPUT}")
endif()

message(STATUS "Applied VTFLib++ patch: afxres.h -> winres.h")
