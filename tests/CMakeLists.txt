add_executable(vmt_tests
    test_main.cpp
    skeleton_test.cpp
    vmt_core_tests.cpp
    schema_tests.cpp
)

target_include_directories(vmt_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/framework"
)

target_compile_definitions(vmt_tests PRIVATE
    VMT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    VMT_SCHEMA_DIR="${CMAKE_SOURCE_DIR}/data/schema"
)

target_link_libraries(vmt_tests PRIVATE
    vmt_core vtf_reader renderer engine_bridge game_profiles ui
    vmtviewer_warnings
)

# vmt_tests links vtf_reader (for the module identity check), so it also needs
# the VTFLib++ runtime beside the executable.
vtf_deploy_vtflib_runtime(vmt_tests)

add_test(NAME vmt_tests COMMAND vmt_tests)

# --- vtf_reader tests ----------------------------------------------------------

add_executable(vtf_tests
    test_main.cpp
    vtf_tests.cpp
)

target_include_directories(vtf_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/framework"
)

target_compile_definitions(vtf_tests PRIVATE
    VTF_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/vtf"
)

target_link_libraries(vtf_tests PRIVATE
    vtf_reader
    vmtviewer_warnings
)

vtf_deploy_vtflib_runtime(vtf_tests)

add_test(NAME vtf_tests COMMAND vtf_tests)

# --- ui editing-core tests ----------------------------------------------------

add_executable(ui_tests
    test_main.cpp
    ui_tests.cpp
)

target_include_directories(ui_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/framework"
)

target_compile_definitions(ui_tests PRIVATE
    VMT_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    VMT_SCHEMA_DIR="${CMAKE_SOURCE_DIR}/data/schema"
)

target_link_libraries(ui_tests PRIVATE
    ui
    vmtviewer_warnings
)

vtf_deploy_vtflib_runtime(ui_tests)

add_test(NAME ui_tests COMMAND ui_tests)

# --- steam detection tests ----------------------------------------------------

add_executable(steam_detection_tests
    test_main.cpp
    steam_detection_tests.cpp
)

target_include_directories(steam_detection_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/framework"
)

target_link_libraries(steam_detection_tests PRIVATE
    game_profiles
    vmtviewer_warnings
)

vtf_deploy_vtflib_runtime(steam_detection_tests)

add_test(NAME steam_detection_tests COMMAND steam_detection_tests)
