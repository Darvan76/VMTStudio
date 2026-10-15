#pragma once

#include <windows.h>

// Live animation state for the IPC protocol v2 (docs/IPC_PROTOCOL.md): a
// polling timer posts WM_USER+422 to the host window while the playhead moves,
// and SET_LOOP 0 clamps the playhead to the last frame instead of letting the
// engine wrap it.

// Starts the polling timer when a host window is available (embedded mode).
void AnimControl_Init(HWND hWndMain, HWND hHostWnd);
void AnimControl_Shutdown();

// Consumes WM_TIMER for the animation timer; returns false for foreign timers
// so the caller can dispatch them normally.
bool AnimControl_HandleTimer(WPARAM wParam);

// Forgets the wrap-detection baseline; call after any playhead jump.
void AnimControl_SyncBaseline();

// SET_LOOP <0|1>.
void AnimControl_SetLoopEnabled(bool enabled);
