#pragma once

#include <windows.h>
#include <string>

// IPC protocol v2 data queries (docs/IPC_PROTOCOL.md): the DUMP/RAW response
// files. The commands are validated in the WM_COPYDATA handler and the engine
// work runs later on the main thread, so the handler stays fast.

// Posted to the backend main window to execute a queued query job.
// wParam: unused. lParam: BackendQueryJob* (consumed by the handler).
#define VMTIPC_QUERY_JOB_MESSAGE (WM_USER + 423)

// Response-file directory; an empty path selects "%TEMP%\VMTStudio\ipc".
// The directory is created when it is missing.
void BackendQuery_SetResponseDir(const std::string& dir);

// Window that receives VMTIPC_QUERY_JOB_MESSAGE; with no window the job runs
// inline in the caller.
void BackendQuery_SetMainWindow(HWND hWnd);

// "DUMP <kind> <token> [args]" / "RAW <token> <path>". Returns true when the
// command is well formed and a job was scheduled; unknown dump kinds and job
// failures are reported through <token>.err.
bool BackendQuery_HandleDump(const std::string& kind, const std::string& token, const std::string& args);
bool BackendQuery_HandleRaw(const std::string& token, const std::string& path);

// Runs the job carried by a VMTIPC_QUERY_JOB_MESSAGE and frees it.
void BackendQuery_RunJob(WPARAM wParam, LPARAM lParam);

// Submodel selection per bodypart: the engine has no getter, so the
// model_scene bodypart rows and SET_BODYGROUP keep this table in sync.
void BackendQuery_ResetSubmodels();
void BackendQuery_NoteSubmodel(int group, int value);
int BackendQuery_GetSubmodel(int group);
