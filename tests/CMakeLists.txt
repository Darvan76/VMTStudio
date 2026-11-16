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
    vmt_core vtf_reader engine_bridge game_profiles ui
    vmtviewer_warnings
)

# vmt_tests exercises vtf_reader in the module smoke check, so it also needs
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
    session_safety_tests.cpp
    material_save_path_tests.cpp
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

# --- model viewer tests (IPC dumps, model materials, animation, export plan) --

add_executable(model_view_tests
    test_main.cpp
    ipc_dump_tests.cpp
    engine_compatibility_tests.cpp
    model_scene_tests.cpp
    animation_model_tests.cpp
    export_plan_tests.cpp
)

target_include_directories(model_view_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/framework"
)

target_link_libraries(model_view_tests PRIVATE
    ui
    vmtviewer_warnings
)

vtf_deploy_vtflib_runtime(model_view_tests)

add_test(NAME model_view_tests COMMAND model_view_tests)

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

# --- install validation tests --------------------------------------------------

add_executable(install_check_tests
    test_main.cpp
    install_check_tests.cpp
)

target_include_directories(install_check_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/framework"
)

target_link_libraries(install_check_tests PRIVATE
    game_profiles
    vmtviewer_warnings
)

vtf_deploy_vtflib_runtime(install_check_tests)

add_test(NAME install_check_tests COMMAND install_check_tests)

# --- Qt inspector behavior (no QtTest dependency) ------------------------------

if(NOT TARGET Qt6::Widgets OR NOT TARGET Qt6::Network)
    find_package(Qt6 COMPONENTS Widgets Network QUIET)
endif()
if(TARGET Qt6::Widgets AND TARGET Qt6::Network)
    add_executable(inspector_widget_tests
        inspector_widget_tests.cpp
        ../ui/src/qt/inspector_widget.cpp
        ../ui/src/qt/inspector_widget.h
    )
    set_target_properties(inspector_widget_tests PROPERTIES AUTOMOC ON)
    target_include_directories(inspector_widget_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/framework"
        "${CMAKE_SOURCE_DIR}/ui/src/qt"
    )
    target_link_libraries(inspector_widget_tests PRIVATE
        ui Qt6::Widgets vmtstudio_warnings
    )
    vtf_deploy_vtflib_runtime(inspector_widget_tests)
    add_test(NAME inspector_widget_tests COMMAND inspector_widget_tests)

    add_executable(asset_loading_tests
        asset_loading_tests.cpp
        ../ui/src/qt/asset_list_model.cpp
        ../ui/src/qt/asset_list_model.h
        ../ui/src/qt/asset_preview_panel.cpp
        ../ui/src/qt/asset_preview_panel.h
        ../ui/src/qt/asset_thumbnail_service.cpp
        ../ui/src/qt/asset_thumbnail_service.h
        ../ui/src/qt/schema_resources.cpp
        ../ui/src/qt/schema_resources.h
        ../ui/src/qt/export_materials_dialog.cpp
        ../ui/src/qt/export_materials_dialog.h
        ../ui/src/qt/app_updater.cpp
        ../ui/src/qt/app_updater.h
        ../ui/src/qt/connection_dialog.cpp
        ../ui/src/qt/connection_dialog.h
        ../ui/resources/vmtstudio.qrc
    )
    set_target_properties(asset_loading_tests PROPERTIES AUTOMOC ON AUTORCC ON)
    target_include_directories(asset_loading_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/framework"
        "${CMAKE_SOURCE_DIR}/ui/src/qt"
    )
    target_compile_definitions(asset_loading_tests PRIVATE
        VTF_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/vtf"
    )
    target_link_libraries(asset_loading_tests PRIVATE ui Qt6::Widgets Qt6::Network vmtstudio_warnings)
    vtf_deploy_vtflib_runtime(asset_loading_tests)
    add_test(NAME asset_loading_tests COMMAND asset_loading_tests)
    set_tests_properties(asset_loading_tests PROPERTIES TIMEOUT 30)
    set_tests_properties(inspector_widget_tests asset_loading_tests PROPERTIES ENVIRONMENT
        "QT_QPA_PLATFORM=offscreen;QT_QPA_PLATFORM_PLUGIN_PATH=$<TARGET_FILE_DIR:Qt6::Core>/../plugins/platforms"
    )

    # Manual measurements, not CTest timing gates: compare on the same machine.
    add_executable(performance_benchmarks EXCLUDE_FROM_ALL
        performance_benchmarks.cpp
        ../ui/src/qt/asset_browser_dialog.cpp
        ../ui/src/qt/asset_browser_dialog.h
        ../ui/src/qt/asset_list_model.cpp
        ../ui/src/qt/asset_list_model.h
        ../ui/src/qt/asset_preview_panel.cpp
        ../ui/src/qt/asset_preview_panel.h
        ../ui/src/qt/asset_thumbnail_service.cpp
        ../ui/src/qt/asset_thumbnail_service.h
        ../ui/src/qt/inspector_widget.cpp
        ../ui/src/qt/inspector_widget.h
        ../ui/src/qt/vmt_highlighter.cpp
        ../ui/src/qt/vmt_highlighter.h
        ../ui/src/qt/schema_resources.cpp
        ../ui/src/qt/schema_resources.h
        ../ui/resources/vmtstudio.qrc
    )
    set_target_properties(performance_benchmarks PROPERTIES AUTOMOC ON AUTORCC ON)
    target_include_directories(performance_benchmarks PRIVATE
        "${CMAKE_SOURCE_DIR}/ui/src/qt"
        "${CMAKE_SOURCE_DIR}/ui/src"
    )
    target_link_libraries(performance_benchmarks PRIVATE ui Qt6::Widgets vmtstudio_warnings psapi)
    vtf_deploy_vtflib_runtime(performance_benchmarks)
endif()
