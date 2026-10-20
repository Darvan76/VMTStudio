# copy_hlmv_backend.cmake
# Gracefully copies hlmv_backend.exe from 32-bit build to frontend directory
# if present, selecting matching configuration with fallbacks.

if(NOT DEFINED DST OR NOT DEFINED CONFIG)
    message(FATAL_ERROR "DST and CONFIG must be defined")
endif()

set(CANDIDATES "")

if(CONFIG STREQUAL "Debug")
    list(APPEND CANDIDATES "${SRC_DBG}")
    list(APPEND CANDIDATES "${SRC_REL}")
else()
    list(APPEND CANDIDATES "${SRC_REL}")
    list(APPEND CANDIDATES "${SRC_DBG}")
endif()

set(COPIED FALSE)
foreach(CANDIDATE IN LISTS CANDIDATES)
    if(EXISTS "${CANDIDATE}")
        message(STATUS "Copying 32-bit hlmv_backend.exe from ${CANDIDATE} to ${DST}")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CANDIDATE}" "${DST}"
            RESULT_VARIABLE RES
        )
        if(RES EQUAL 0)
            set(COPIED TRUE)
            break()
        endif()
    endif()
endforeach()

if(NOT COPIED)
    message(STATUS "Note: 32-bit hlmv_backend.exe not found in 32-bit build directories. Skipping copy.")
endif()
