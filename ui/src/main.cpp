// vmtviewer - application entry point.
//
// The real UI lives in the 'ui' library (parameter panel, raw text editor,
// recent files). This file only enables debug leak checking and hands control
// to ui::run_app.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include "main_window.h"

#include <windows.h>
#include <shellapi.h>

#include <string>

#ifdef _DEBUG
#include <crtdbg.h>
#define VMT_ENABLE_LEAK_CHECK() \
    do { _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF); } while (0)
#else
#define VMT_ENABLE_LEAK_CHECK() \
    do { } while (0)
#endif

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show_command) {
    VMT_ENABLE_LEAK_CHECK();

    // Hidden compatibility probe: run the engine in this disposable process.
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv != nullptr) {
        const bool probe = argc >= 4 && std::wstring(argv[1]) == L"--engine-probe";
        const std::wstring bin = probe ? argv[2] : std::wstring();
        const std::wstring game = probe ? argv[3] : std::wstring();
        LocalFree(argv);
        if (probe) {
            return ui::run_engine_probe(bin, game);
        }
    }

    return ui::run_app(instance, show_command);
}
