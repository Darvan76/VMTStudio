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
add_executable(vmtstudio WIN32
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
    src/qt/notification_popup.h
    src/qt/notification_popup.cpp
    src/qt/theme.h
    src/qt/theme.cpp
    resources/vmtstudio.qrc
)

target_include_directories(vmtstudio PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

target_compile_definitions(vmtstudio PRIVATE
    VMT_SCHEMA_DIR="${CMAKE_SOURCE_DIR}/data/schema"
)

# Manifest and icon configuration
set(VMTSTUDIO_MANIFEST_PATH "${CMAKE_CURRENT_SOURCE_DIR}/win32/app.manifest")
set(VMTVIEWER_MANIFEST_PATH "${VMTSTUDIO_MANIFEST_PATH}")
set(VMTSTUDIO_ICON_PATH "${CMAKE_CURRENT_SOURCE_DIR}/win32/app.ico")
configure_file(win32/app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc" @ONLY)
target_sources(vmtstudio PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc")

set_target_properties(vmtstudio PROPERTIES
    OUTPUT_NAME "vmtstudio"
    VS_GLOBAL_GenerateManifest OFF
    VS_GLOBAL_EnableDpiAwareness OFF
)
target_link_options(vmtstudio PRIVATE "/MANIFEST:NO")

target_link_libraries(vmtstudio PRIVATE
    ui
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    vmtstudio_warnings
    user32
    shell32
)

# Deploy VTFLib runtime DLL
vtf_deploy_vtflib_runtime(vmtstudio)

# Copy hlmv_backend.exe beside vmtstudio if available from 32-bit build
add_custom_command(TARGET vmtstudio POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${CMAKE_SOURCE_DIR}/build/bin/Release/hlmv_backend.exe"
            "$<TARGET_FILE_DIR:vmtstudio>"
    COMMENT "Copying 32-bit hlmv_backend.exe next to vmtstudio"
    VERBATIM
)

# Deploy Qt runtime using windeployqt
find_program(WINDEPLOYQT_EXE NAMES windeployqt HINTS "C:/Qt/6.8.2/msvc2022_64/bin")
if(WINDEPLOYQT_EXE)
    add_custom_command(TARGET vmtstudio POST_BUILD
        COMMAND "${WINDEPLOYQT_EXE}" --release --no-translations "$<TARGET_FILE:vmtstudio>"
        COMMENT "Deploying Qt runtime libraries for vmtstudio"
        VERBATIM
    )
endif()

# Backward-compatibility target so 'cmake --build . --target vmtviewer' still works
add_custom_target(vmtviewer DEPENDS vmtstudio)
endif() # Qt6_FOUND

