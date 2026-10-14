#include "app_log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>

namespace ui {
namespace {

FILE* g_stream = nullptr;
std::filesystem::path g_path;
std::filesystem::path g_dir;
HANDLE g_crash_handle = INVALID_HANDLE_VALUE;
bool g_console_open = false;
bool g_ansi_enabled = false;
std::mutex g_app_log_mutex;

std::string now_stamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                  time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
                  time.wSecond, time.wMilliseconds);
    return buffer;
}

void crash_write_raw(const std::string& line) {
    if (g_crash_handle != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(g_crash_handle, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    }
    HANDLE hStdErr = GetStdHandle(STD_ERROR_HANDLE);
    if (hStdErr != INVALID_HANDLE_VALUE && hStdErr != nullptr) {
        DWORD written = 0;
        WriteFile(hStdErr, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    }
}

std::string module_of(const void* address, unsigned long long* out_offset) {
    HMODULE module = nullptr;
    if (address != nullptr &&
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) &&
        module != nullptr) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(module, path, MAX_PATH);
        const std::filesystem::path file(path);
        if (out_offset != nullptr) {
            *out_offset = static_cast<unsigned long long>(
                reinterpret_cast<const unsigned char*>(address) -
                reinterpret_cast<const unsigned char*>(module));
        }
        return file.filename().string();
    }
    if (out_offset != nullptr) {
        *out_offset = 0;
    }
    return "?";
}

LONG CALLBACK crash_handler(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    const bool fatal = code == EXCEPTION_ACCESS_VIOLATION ||
                       code == EXCEPTION_ILLEGAL_INSTRUCTION ||
                       code == EXCEPTION_PRIV_INSTRUCTION ||
                       code == EXCEPTION_STACK_OVERFLOW ||
                       code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
                       code == EXCEPTION_IN_PAGE_ERROR;
    if (!fatal) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    static LONG reported = 0;
    if (InterlockedCompareExchange(&reported, 1, 0) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    unsigned long long offset = 0;
    const std::string module = module_of(info->ExceptionRecord->ExceptionAddress, &offset);
    char buffer[512] = {};
    std::snprintf(buffer, sizeof(buffer),
                  "[%s] CRASH exception=0x%08lX at %s+0x%llX\n", now_stamp().c_str(),
                  static_cast<unsigned long>(code), module.c_str(), offset);
    crash_write_raw(buffer);

    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
        std::snprintf(buffer, sizeof(buffer), "[%s] CRASH access kind=%lu address=0x%llX\n",
                      now_stamp().c_str(),
                      static_cast<unsigned long>(info->ExceptionRecord->ExceptionInformation[0]),
                      static_cast<unsigned long long>(
                          info->ExceptionRecord->ExceptionInformation[1]));
        crash_write_raw(buffer);
    }

    void* frames[16] = {};
    const USHORT count = RtlCaptureStackBackTrace(1, 16, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        unsigned long long frame_offset = 0;
        const std::string frame_module = module_of(frames[i], &frame_offset);
        std::snprintf(buffer, sizeof(buffer), "[%s] CRASH   #%u %s+0x%llX\n",
                      now_stamp().c_str(), static_cast<unsigned>(i), frame_module.c_str(),
                      frame_offset);
        crash_write_raw(buffer);
    }
    FlushFileBuffers(g_crash_handle);
    return EXCEPTION_CONTINUE_SEARCH;
}

std::filesystem::path marker_path() {
    return g_dir.empty() ? std::filesystem::path() : g_dir / L"session.lock";
}

} // namespace

void log_open(const std::filesystem::path& dir) {
    log_close();
    if (dir.empty()) {
        return;
    }
    g_dir = dir;
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    g_path = dir / L"vmtstudio.log";
    FILE* stream = _wfsopen(g_path.c_str(), L"a", _SH_DENYNO);
    g_stream = stream;
    if (g_stream != nullptr) {
        log_write("APP ", "---- VMT-Studio log opened ----");
    }

    // Raw, unbuffered handle for the crash handler.
    if (g_crash_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_crash_handle);
        g_crash_handle = INVALID_HANDLE_VALUE;
    }
    g_crash_handle = CreateFileW(g_path.c_str(), FILE_APPEND_DATA,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
}

void log_close() {
    if (g_stream != nullptr) {
        std::fclose(g_stream);
        g_stream = nullptr;
    }
    if (g_crash_handle != INVALID_HANDLE_VALUE) {
        CloseHandle(g_crash_handle);
        g_crash_handle = INVALID_HANDLE_VALUE;
    }
}

void console_open() {
    if (g_console_open) {
        console_focus();
        return;
    }

    AllocConsole();

    SetConsoleTitleW(L"VMT-Studio - Consola de Depuracion (Debug Console)");

    FILE* fp_out = nullptr;
    FILE* fp_err = nullptr;
    FILE* fp_in = nullptr;
    freopen_s(&fp_out, "CONOUT$", "w", stdout);
    freopen_s(&fp_err, "CONOUT$", "w", stderr);
    freopen_s(&fp_in, "CONIN$", "r", stdin);

    if (stdout != nullptr) std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (stderr != nullptr) std::setvbuf(stderr, nullptr, _IONBF, 0);

    std::ios::sync_with_stdio(true);
    std::cout.clear();
    std::cerr.clear();
    std::cin.clear();

    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE && hOut != nullptr) {
        CONSOLE_SCREEN_BUFFER_INFO csbi{};
        if (GetConsoleScreenBufferInfo(hOut, &csbi)) {
            const SHORT winWidth = csbi.srWindow.Right - csbi.srWindow.Left + 1;
            const SHORT bufferWidth = csbi.dwSize.X > 120 ? csbi.dwSize.X : (winWidth > 120 ? winWidth : 120);
            const COORD newBufferSize{ bufferWidth, 3000 };
            SetConsoleScreenBufferSize(hOut, newBufferSize);
        }
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {
            if (SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
                g_ansi_enabled = true;
            }
        }
    }

    HWND hwndConsole = GetConsoleWindow();
    if (hwndConsole != nullptr) {
        HMENU hMenu = GetSystemMenu(hwndConsole, FALSE);
        if (hMenu != nullptr) {
            DeleteMenu(hMenu, SC_CLOSE, MF_BYCOMMAND);
        }
    }

    g_console_open = true;

    if (g_ansi_enabled) {
        std::printf("\033[96m================================================================================\033[0m\n");
        std::printf("\033[97;1m  VMT-Studio - Consola de Depuracion y Diagnostico\033[0m\n");
        std::printf("\033[96m================================================================================\033[0m\n");
        std::printf("\033[90m  Diagnostico en tiempo real activado. Todos los eventos se mostraran aqui.\033[0m\n");
        std::printf("\033[90m  Para cerrar la consola, simplemente cierra la ventana principal de VMT-Studio.\033[0m\n");
        std::printf("\033[96m--------------------------------------------------------------------------------\033[0m\n\n");
    } else {
        std::printf("================================================================================\n");
        std::printf("  VMT-Studio - Consola de Depuracion y Diagnostico\n");
        std::printf("================================================================================\n");
        std::printf("  Diagnostico en tiempo real activado. Todos los eventos se mostraran aqui.\n");
        std::printf("  Para cerrar la consola, simplemente cierra la ventana principal de VMT-Studio.\n");
        std::printf("--------------------------------------------------------------------------------\n\n");
    }
    std::fflush(stdout);
}

void console_focus() {
    HWND hwndConsole = GetConsoleWindow();
    if (hwndConsole != nullptr) {
        ShowWindow(hwndConsole, SW_SHOW);
        SetForegroundWindow(hwndConsole);
    }
}

bool is_console_open() {
    return g_console_open && (GetConsoleWindow() != nullptr);
}

void console_close() {
    if (g_console_open) {
        FreeConsole();
        g_console_open = false;
    }
}

void log_write(const std::string& tag, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_app_log_mutex);
    const std::string stamp = now_stamp();

    if (g_stream != nullptr) {
        std::fprintf(g_stream, "[%s] %s %s\r\n", stamp.c_str(), tag.c_str(), message.c_str());
        std::fflush(g_stream); // crash-safe: never leave buffered lines behind
    }

    if (g_console_open || GetConsoleWindow() != nullptr) {
        if (g_ansi_enabled) {
            const char* color = "\033[37m";
            if (tag == "ERR " || tag == "FAIL" || tag == "CRSH") color = "\033[91;1m";
            else if (tag == "WARN") color = "\033[93;1m";
            else if (tag == "OK  ") color = "\033[92;1m";
            else if (tag == "APP ") color = "\033[96;1m";
            else if (tag == "ENG ") color = "\033[95;1m";
            else if (tag == "FILE") color = "\033[94;1m";
            else if (tag == "EDIT") color = "\033[36;1m";
            else if (tag == "VAL ") color = "\033[33;1m";
            else if (tag == "BRWS") color = "\033[93m";
            else if (tag == "TEX ") color = "\033[32m";
            else if (tag == "SRC ") color = "\033[35m";

            std::printf("\033[90m[%s]\033[0m %s[%s]\033[0m %s\n",
                        stamp.c_str(), color, tag.c_str(), message.c_str());
        } else {
            std::printf("[%s] [%s] %s\n", stamp.c_str(), tag.c_str(), message.c_str());
        }
        std::fflush(stdout);
    }
}

std::filesystem::path log_path() {
    return g_path;
}

void log_session_begin() {
    const std::filesystem::path marker = marker_path();
    if (marker.empty()) {
        return;
    }
    FILE* file = nullptr;
    _wfopen_s(&file, marker.c_str(), L"w");
    if (file != nullptr) {
        std::fprintf(file, "pid=%lu\n", static_cast<unsigned long>(GetCurrentProcessId()));
        std::fprintf(file, "started=%s\n", now_stamp().c_str());
        std::fclose(file);
    }
}

void log_session_end() {
    const std::filesystem::path marker = marker_path();
    if (marker.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::remove(marker, ec);
}

void log_report_previous_session() {
    const std::filesystem::path marker = marker_path();
    if (marker.empty()) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(marker, ec)) {
        return;
    }
    FILE* file = nullptr;
    _wfopen_s(&file, marker.c_str(), L"r");
    std::string details;
    if (file != nullptr) {
        char line[160] = {};
        while (std::fgets(line, sizeof(line), file) != nullptr) {
            details += line;
        }
        std::fclose(file);
    }
    while (!details.empty() && (details.back() == '\n' || details.back() == '\r')) {
        details.pop_back();
    }
    for (char& c : details) {
        if (c == '\n') {
            c = ' ';
        }
    }
    log_write("WARN", "previous session ended uncleanly (" +
                          (details.empty() ? std::string("no marker details") : details) + ")");
    std::filesystem::remove(marker, ec);
}

void install_crash_handler() {
    static bool installed = false;
    if (installed) {
        return;
    }
    installed = true;
    AddVectoredExceptionHandler(1, crash_handler);
}

} // namespace ui
