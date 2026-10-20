#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include "miniz.h"

namespace fs = std::filesystem;

// The payload zip resource ID
#define IDR_PAYLOAD_ZIP 101

// Forward command-line arguments without launcher's own argv[0]
static std::wstring build_forward_args(const std::wstring& targetExe) {
    std::wstring result = L"\"" + targetExe + L"\"";
    
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) {
        for (int i = 1; i < argc; ++i) {
            result += L" \"";
            result += argv[i];
            result += L"\"";
        }
        LocalFree(argv);
    }
    return result;
}

static bool extract_payload(const void* zipData, size_t zipSize, const fs::path& destDir, std::wstring& errorDetail) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, zipData, zipSize, 0)) {
        errorDetail = L"Error al inicializar el descompresor ZIP integrado.";
        return false;
    }

    mz_uint numFiles = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < numFiles; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) {
            continue;
        }

        std::string rawName = stat.m_filename ? stat.m_filename : "";
        if (rawName.empty()) {
            continue;
        }

        // Normalize all backslashes to forward slashes
        for (char& c : rawName) {
            if (c == '\\') {
                c = '/';
            }
        }

        // Strip leading slashes if any
        size_t start = rawName.find_first_not_of('/');
        if (start == std::string::npos) {
            continue;
        }
        std::string name = rawName.substr(start);

        // Strip top-level directory prefix if present (e.g. "vmtstudio-v0.2.0-win64/...")
        auto slashPos = name.find('/');
        if (slashPos != std::string::npos) {
            std::string topFolder = name.substr(0, slashPos);
            if (topFolder.rfind("vmtstudio-", 0) == 0) {
                name = name.substr(slashPos + 1);
            }
        } else if (name.rfind("vmtstudio-", 0) == 0) {
            // Root folder entry itself without trailing slash
            continue;
        }

        if (name.empty()) {
            continue;
        }

        bool isDir = stat.m_is_directory || (name.back() == '/');

        // Remove trailing slashes for path construction
        while (!name.empty() && name.back() == '/') {
            name.pop_back();
        }
        if (name.empty()) {
            continue;
        }

        fs::path relPath = fs::u8path(name);
        fs::path outPath = destDir / relPath;

        if (isDir) {
            std::error_code ec;
            fs::create_directories(outPath, ec);
            if (ec) {
                errorDetail = L"No se pudo crear el directorio: " + outPath.wstring();
                mz_zip_reader_end(&zip);
                return false;
            }
        } else {
            std::error_code ec;
            fs::create_directories(outPath.parent_path(), ec);

            FILE* fp = nullptr;
            errno_t err = _wfopen_s(&fp, outPath.c_str(), L"wb");
            if (err != 0 || !fp) {
                errorDetail = L"No se pudo crear el archivo: " + outPath.wstring();
                mz_zip_reader_end(&zip);
                return false;
            }

            if (!mz_zip_reader_extract_to_cfile(&zip, i, fp, 0)) {
                fclose(fp);
                errorDetail = L"Error al descomprimir: " + outPath.wstring();
                mz_zip_reader_end(&zip);
                return false;
            }

            fclose(fp);
        }
    }

    mz_zip_reader_end(&zip);
    return true;
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE /*hPrevInstance*/, PWSTR /*pCmdLine*/, int nCmdShow) {
    (void)hInstance;
    (void)nCmdShow;

    // Optional modern DPI awareness
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // 1. Locate destination in %LOCALAPPDATA%\VMTStudio\app-v0.2.0
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &localAppData)) || !localAppData) {
        MessageBoxW(NULL, L"Error: No se pudo resolver la ruta %LOCALAPPDATA%.", L"VMT-Studio Launcher", MB_ICONERROR);
        return 1;
    }
    fs::path appDir = fs::path(localAppData) / L"VMTStudio" / L"app-v0.2.0";
    CoTaskMemFree(localAppData);

    fs::path targetExe = appDir / L"vmtstudio.exe";
    fs::path readyMarker = appDir / L".ready";

    // 2. Check if already extracted
    std::error_code ec;
    bool needsExtraction = !fs::exists(readyMarker, ec) || !fs::exists(targetExe, ec);

    if (needsExtraction) {
        // Load embedded zip from resources
        HRSRC hRes = FindResourceW(NULL, MAKEINTRESOURCEW(IDR_PAYLOAD_ZIP), (LPCWSTR)RT_RCDATA);
        if (!hRes) {
            MessageBoxW(NULL, L"Error crítico: No se encontró el paquete de recursos integrado.", L"VMT-Studio Launcher", MB_ICONERROR);
            return 1;
        }
        HGLOBAL hMem = LoadResource(NULL, hRes);
        if (!hMem) {
            MessageBoxW(NULL, L"Error al cargar el recurso integrado en memoria.", L"VMT-Studio Launcher", MB_ICONERROR);
            return 1;
        }
        DWORD resSize = SizeofResource(NULL, hRes);
        const void* resData = LockResource(hMem);
        if (!resData || resSize == 0) {
            MessageBoxW(NULL, L"Error al acceder a los datos del paquete integrado.", L"VMT-Studio Launcher", MB_ICONERROR);
            return 1;
        }

        fs::create_directories(appDir, ec);
        std::wstring errDetail;
        if (!extract_payload(resData, resSize, appDir, errDetail)) {
            std::wstring msg = L"Error al preparar los componentes de VMT-Studio:\n" + errDetail;
            MessageBoxW(NULL, msg.c_str(), L"VMT-Studio Launcher", MB_ICONERROR);
            return 1;
        }

        // Mark as ready
        std::ofstream marker(readyMarker);
        if (marker.is_open()) {
            marker << "0.2.0\n";
        }
    }

    // 3. Launch target application
    std::wstring cmdLine = build_forward_args(targetExe);
    std::vector<wchar_t> cmdBuffer(cmdLine.begin(), cmdLine.end());
    cmdBuffer.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessW(
        targetExe.c_str(),
        cmdBuffer.data(),
        NULL,
        NULL,
        FALSE,
        0,
        NULL,
        appDir.c_str(),
        &si,
        &pi
    );

    if (!ok) {
        DWORD err = GetLastError();
        std::wstring msg = L"No se pudo iniciar vmtstudio.exe (Código de error " + std::to_wstring(err) + L").";
        MessageBoxW(NULL, msg.c_str(), L"VMT-Studio Launcher", MB_ICONERROR);
        return 1;
    }

    // Wait for the application to exit and propagate its exit code
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return static_cast<int>(exitCode);
}
