cmake_minimum_required(VERSION 3.21)

find_package(Qt6 COMPONENTS Core Gui Widgets QUIET)

if(Qt6_FOUND)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    set(CMAKE_AUTOUIC ON)
endif()

# The 'ui' static library holds the headless editing core (session, panel model,
# settings, validation, material browser, thumbnails).
add_library(ui STATIC
    src/ui.cpp
    src/session.cpp
    src/panel_model.cpp
    src/settings.cpp
    src/text_encoding.cpp
    src/connection_model.cpp
    src/preview_sync.cpp
    src/preview_options.cpp
    src/material_browser.cpp
    src/texture_thumbnail.cpp
    src/material_info.cpp
    src/shader_catalog.cpp
    src/validate.cpp
    src/app_log.cpp
)

target_include_directories(ui PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_definitions(ui PRIVATE
    VMT_SCHEMA_DIR="${CMAKE_SOURCE_DIR}/data/schema"
)

target_link_libraries(ui
    PUBLIC vmt_core vtf_reader renderer engine_bridge game_profiles
           shell32 ole32
    PRIVATE engine_spike_sdk vmtviewer_warnings advapi32
)

if(Qt6_FOUND)
# The Qt 6 application executable
add_executable(vmtviewer WIN32
    src/main.cpp
    src/qt/main_window.h
    src/qt/main_window.cpp
    src/qt/viewport_widget.h
    src/qt/viewport_widget.cpp
    src/qt/vmt_highlighter.h
    src/qt/vmt_highlighter.cpp
    src/qt/inspector_widget.h
    src/qt/inspector_widget.cpp
    src/qt/material_browser_dialog.h
    src/qt/material_browser_dialog.cpp
    src/qt/preview_options_dialog.h
    src/qt/preview_options_dialog.cpp
    src/qt/connection_dialog.h
    src/qt/connection_dialog.cpp
    src/qt/about_dialog.h
    src/qt/about_dialog.cpp
    src/qt/theme.h
    src/qt/theme.cpp
)

target_include_directories(vmtviewer PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_definitions(vmtviewer PRIVATE
    VMT_SCHEMA_DIR="${CMAKE_SOURCE_DIR}/data/schema"
)

# Manifest configuration
set(VMTVIEWER_MANIFEST_PATH "${CMAKE_CURRENT_SOURCE_DIR}/win32/app.manifest")
configure_file(win32/app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc" @ONLY)
target_sources(vmtviewer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc")

set_target_properties(vmtviewer PROPERTIES
    VS_GLOBAL_GenerateManifest OFF
    VS_GLOBAL_EnableDpiAwareness OFF
)
target_link_options(vmtviewer PRIVATE "/MANIFEST:NO")

target_link_libraries(vmtviewer PRIVATE
    ui
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    vmtviewer_warnings
    user32
    shell32
)

# Deploy VTFLib runtime DLL
vtf_deploy_vtflib_runtime(vmtviewer)

# Copy hlmv_backend.exe beside vmtviewer if available from 32-bit build
add_custom_command(TARGET vmtviewer POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/build/bin/Release/hlmv_backend.exe"
            "$<TARGET_FILE_DIR:vmtviewer>"
    COMMENT "Copying 32-bit hlmv_backend.exe next to vmtviewer"
    VERBATIM
)

# Deploy Qt runtime using windeployqt
find_program(WINDEPLOYQT_EXE NAMES windeployqt HINTS "C:/Qt/6.8.2/msvc2022_64/bin")
if(WINDEPLOYQT_EXE)
    add_custom_command(TARGET vmtviewer POST_BUILD
        COMMAND "${WINDEPLOYQT_EXE}" --release --no-translations "$<TARGET_FILE:vmtviewer>"
        COMMENT "Deploying Qt runtime libraries for vmtviewer"
        VERBATIM
    )
endif()
endif() # Qt6_FOUND

