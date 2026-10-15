# HLMV Backend for VMT-Studio
add_executable(hlmv_backend WIN32
    # HLMV Core
    src/attachments_window.cpp
    src/brush_render.cpp
    src/camera.cpp
    src/controlpanel.cpp
    src/debugdrawmodel.cpp
    src/fileassociation.cpp
    src/matsyswin.cpp
    src/mdlviewer.cpp
    src/mxlineedit2.cpp
    src/pakviewer.cpp
    src/physmesh.cpp
    src/studio_flex.cpp
    src/studio_render.cpp
    src/studio_utils.cpp
    src/sys_win.cpp
    src/viewersettings.cpp
    src/hlmv_compat.cpp
    src/backend_ipc.cpp
    src/backend_query.cpp
    src/anim_control.cpp
    src/hlmv.rc

    # MX Toolkit
    mxtk/src/win32/mx.cpp
    mxtk/src/common/mxbmp.cpp
    mxtk/src/win32/mxbutton.cpp
    mxtk/src/win32/mxcheckbox.cpp
    mxtk/src/win32/mxchoice.cpp
    mxtk/src/win32/mxchoosecolor.cpp
    mxtk/src/win32/mxfiledialog.cpp
    mxtk/src/win32/mxgroupbox.cpp
    mxtk/src/win32/mxlabel.cpp
    mxtk/src/win32/mxlineedit.cpp
    mxtk/src/win32/mxlistbox.cpp
    mxtk/src/win32/mxlistview.cpp
    mxtk/src/win32/mxmatsyswindow.cpp
    mxtk/src/win32/mxmenu.cpp
    mxtk/src/win32/mxmenubar.cpp
    mxtk/src/win32/mxmessagebox.cpp
    mxtk/src/common/mxpath.cpp
    mxtk/src/common/mxpcx.cpp
    mxtk/src/win32/mxpopupmenu.cpp
    mxtk/src/win32/mxprogressbar.cpp
    mxtk/src/win32/mxradiobutton.cpp
    mxtk/src/win32/mxscrollbar.cpp
    mxtk/src/win32/mxslider.cpp
    mxtk/src/common/mxstring.cpp
    mxtk/src/win32/mxtab.cpp
    mxtk/src/common/mxtga.cpp
    mxtk/src/win32/mxtooltip.cpp
    mxtk/src/win32/mxtreeview.cpp
    mxtk/src/win32/mxwidget.cpp
    mxtk/src/win32/mxwindow.cpp

    # Valve SDK source files
    "${SOURCE_SDK_PUBLIC}/bone_setup.cpp"
    "${SOURCE_SDK_PUBLIC}/collisionutils.cpp"
    "${SOURCE_SDK_PUBLIC}/filesystem_helpers.cpp"
    "${SOURCE_SDK_PUBLIC}/studio.cpp"
)

target_include_directories(hlmv_backend PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/mxtk/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/mxtk/include/mx"
    "${CMAKE_CURRENT_SOURCE_DIR}/common"
    "${SOURCE_SDK_PUBLIC}"
    "${SOURCE_SDK_PUBLIC}/mathlib"
    "${SOURCE_SDK_PUBLIC}/tier0"
    "${SOURCE_SDK_PUBLIC}/tier1"
    "${SOURCE_SDK_PUBLIC}/tier2"
    "${SOURCE_SDK_PUBLIC}/tier3"
)

target_compile_definitions(hlmv_backend PRIVATE
    WIN32
    _WIN32
    _WINDOWS
    COMPILER_MSVC
    COMPILER_MSVC32
    PLATFORM_WINDOWS
    PLATFORM_WINDOWS_PC
    RAD_TELEMETRY_DISABLED
    _CRT_SECURE_NO_WARNINGS
    _CRT_NONSTDC_NO_WARNINGS
)

target_compile_options(hlmv_backend PRIVATE
    /W0
    /permissive
)

target_link_libraries(hlmv_backend PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/appframework.lib"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/bitmap.lib"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/mathlib.lib"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/tier0.lib"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/tier1.lib"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/tier2.lib"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/tier3.lib"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/vstdlib.lib"
    "${CMAKE_CURRENT_SOURCE_DIR}/lib/vtf.lib"
    legacy_stdio_definitions.lib
    comctl32.lib
    imm32.lib
    winmm.lib
)

set_target_properties(hlmv_backend PROPERTIES
    OUTPUT_NAME "hlmv_backend"
    LINK_FLAGS "/ENTRY:mainCRTStartup /SUBSYSTEM:WINDOWS"
)
