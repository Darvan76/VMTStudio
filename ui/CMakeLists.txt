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
    src/texture_thumbnail.cpp
    src/material_info.cpp
    src/shader_catalog.cpp
    src/validate.cpp
    src/main_window.cpp
    src/connection_dialog.cpp
    src/folder_picker.cpp
    src/material_browser_window.cpp
    src/preview_options_dialog.cpp
    src/about_dialog.cpp
    src/app_log.cpp
    # Win32 presentation layer: theming/palette + Direct2D/DirectWrite canvas.
    # Deliberately DC-backed (ID2D1DCRenderTarget), never device-backed: the
    # Source engine holds a D3D9 device in this process, so the shell must not
    # create a device or swapchain of its own.
    src/win32_theme.cpp
    src/d2d_canvas.cpp
    # Themed replacements for the common controls the shell used to lean on.
    # Each mirrors the native message IDs (BM_/BN_, TBM_, CB_/CBN_) so the
    # existing SendMessageW call sites keep working unchanged.
    src/theme_button.cpp
    src/theme_slider.cpp
    src/theme_combo.cpp
    # Shared registration + window-chrome glue used by every top-level window.
    src/win32_controls.cpp
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
           # theming: DWM window attributes, SetWindowTheme, HKCU theme read.
           # canvas: Direct2D over a GDI DC + DirectWrite text shaping.
           dwmapi uxtheme advapi32 d2d1 dwrite
)

# The application executable.
add_executable(vmtviewer WIN32
    src/main.cpp
)

# The manifest declares Common Controls v6 + Per-Monitor V2 DPI awareness.
# rc.exe needs a quoted absolute path for the resource file, and CMake does not
# forward target_compile_definitions to the resource compiler - so bake the path
# into a generated .rc at configure time instead.
set(VMTVIEWER_MANIFEST_PATH "${CMAKE_CURRENT_SOURCE_DIR}/win32/app.manifest")
configure_file(win32/app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc" @ONLY)
target_sources(vmtviewer PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/app.rc")

# We own the manifest now, so silence MSBuild's own RT_MANIFEST #1 generation
# (it would collide with the one in the .rc) and its DPI-awareness injection.
# VS_GLOBAL_GenerateManifest alone is not enough: CMake also emits a per-config
# <GenerateManifest>true</GenerateManifest> in a later PropertyGroup that wins.
# /MANIFEST:NO at the linker is the authoritative switch.
set_target_properties(vmtviewer PROPERTIES
    VS_GLOBAL_GenerateManifest OFF
    VS_GLOBAL_EnableDpiAwareness OFF
)
target_link_options(vmtviewer PRIVATE "/MANIFEST:NO")

target_link_libraries(vmtviewer
    PRIVATE ui vmtviewer_warnings user32
)
