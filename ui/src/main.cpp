#include "qt/main_window.h"
#include "qt/theme.h"
#include "app_log.h"

#include <QApplication>
#include <QFileInfo>
#include <QIcon>
#include <QStringList>

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
#ifdef VMTSTUDIO_VERSION_STRING
    app.setApplicationVersion(VMTSTUDIO_VERSION_STRING);
#else
    app.setApplicationVersion("0.2.0");
#endif
    app.setOrganizationName("VMT-Studio");
    app.setWindowIcon(QIcon(":/icons/app.png"));

    ui::qt::apply_dark_theme(app);

    ui::qt::MainWindow mainWindow;
    mainWindow.show();

    // Command line: [--model <path>] [material.vmt]
    QString initialMaterial;
    QString initialModel;
    const QStringList arguments = app.arguments();
    for (int i = 1; i < arguments.size(); ++i) {
        const QString argument = arguments.at(i);
        if (argument == QStringLiteral("--model")) {
            if (i + 1 < arguments.size()) {
                initialModel = arguments.at(++i);
            }
        } else if (argument.startsWith(QLatin1Char('-'))) {
            continue; // unknown option
        } else if (argument.endsWith(".vmt", Qt::CaseInsensitive)) {
            if (initialMaterial.isEmpty()) {
                initialMaterial = argument;
            }
        } else if (initialModel.isEmpty()) {
            initialModel = argument;
        }
    }

    if (!initialMaterial.isEmpty() && QFileInfo::exists(initialMaterial)) {
        mainWindow.openFile(initialMaterial);
    }
    if (!initialModel.isEmpty()) {
        mainWindow.openModel(initialModel);
    }

    int result = app.exec();

    ui::log_session_end();
    ui::log_close();
    return result;
}
