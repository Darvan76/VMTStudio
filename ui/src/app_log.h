#pragma once

// ui app_log - append-only application log with crash-safe properties.
//
// Every line is flushed to disk as it is written, the file is opened in append
// mode, and a vectored exception handler writes a CRASH record through a raw
// Win32 handle that is never buffered. A session marker distinguishes a clean
// exit from a crash/termination, and the next start reports an unclean
// previous session. The log lives in %LOCALAPPDATA%\VMTStudio (the engine
// log stays in %TEMP%\VMTStudio\engine.log).
//
// Internal to the shell: not part of the public, testable ui headers.

#include <filesystem>
#include <string>

namespace ui {

// Creates `dir` and opens <dir>/vmtstudio.log (append). Safe to call again.
void log_open(const std::filesystem::path& dir);
void log_close();
void log_write(const std::string& tag, const std::string& message);
std::filesystem::path log_path();

// Debug console management
void console_open();
void console_close();
void console_focus();
bool is_console_open();

// Session marker: written at start, removed on a clean exit. Call
// log_report_previous_session() after log_open to record an unclean end of the
// previous run in the log.
void log_session_begin();
void log_session_end();
void log_report_previous_session();

// Installs the vectored crash handler (idempotent).
void install_crash_handler();

} // namespace ui
