# The 'ui' static library holds both the headless editing core (session,
# panel model, settings - Windows-free and unit tested) and the Win32 shell
# (main window, parameter panel, raw text editor).
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
    src/validate.cpp
    src/main_window.cpp
    src/connection_dialog.cpp
    src/material_browser_window.cpp
    src/preview_options_dialog.cpp
    src/about_dialog.cpp
    src/app_log.cpp
)

target_include_directories(ui PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)

# The main window loads data/schema at runtime (Phase 4 schema files).
target_compile_definitions(ui PRIVATE
    VMT_SCHEMA_DIR="${CMAKE_SOURCE_DIR}/data/schema"
)

target_link_libraries(ui
    PUBLIC vmt_core vtf_reader renderer engine_bridge game_profiles
           comctl32 comdlg32 shell32 ole32
    PRIVATE engine_spike_sdk vmtviewer_warnings
)

# The application executable.
add_executable(vmtviewer WIN32
    src/main.cpp
)

target_link_libraries(vmtviewer
    PRIVATE ui vmtviewer_warnings user32
)
