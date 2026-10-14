#include "qt/main_window.h"
#include "qt/theme.h"
#include "app_log.h"

#include <QApplication>
#include <QFileInfo>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

int main(int argc, char* argv[]) {
    // Initialize application logging and crash handler
    ui::install_crash_handler();
    
    wchar_t localApp[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH) > 0) {
        ui::log_open(std::filesystem::path(localApp) / L"VMTStudio");
    } else {
        ui::log_open(std::filesystem::temp_directory_path() / L"VMTStudio");
    }
    ui::log_session_begin();

    QApplication app(argc, argv);
    app.setApplicationName("VMT-Studio");
    app.setApplicationVersion("0.2.0");
    app.setOrganizationName("VMT-Studio");

    ui::qt::apply_dark_theme(app);

    ui::qt::MainWindow mainWindow;
    mainWindow.show();

    if (argc > 1) {
        QString initialFile = QString::fromLocal8Bit(argv[1]);
        if (QFileInfo::exists(initialFile) && initialFile.endsWith(".vmt", Qt::CaseInsensitive)) {
            mainWindow.openFile(initialFile);
        }
    }

    int result = app.exec();

    ui::log_session_end();
    ui::log_close();
    return result;
}
