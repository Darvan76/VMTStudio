#include "anim_control.h"

#include <windows.h>
#include <cmath>

#include "studio.h"
#include "StudioModel.h"
#include "ViewerSettings.h"
#include "MatSysWin.h"
#include "ControlPanel.h"

extern void HLMV_Log(const char* fmt, ...);

// The engine renders at up to 100 fps, but the tick is only posted when the
// playhead moved, so the host sees roughly one message per changed frame.
#define ANIM_CONTROL_TIMER_ID ((UINT_PTR)0x4D560001)
#define ANIM_CONTROL_TICK_MS  15

static HWND g_hAnimWnd = NULL;
static HWND g_hAnimHost = NULL;
static bool g_loopEnabled = true;
static bool g_hasBaseline = false;
static float g_lastCycle = 0.0f;
static bool g_hasPosted = false;
static int g_postedFrame = 0;
static int g_postedCycle = 0;

static bool AnimControl_HasModel()
{
	return g_pStudioModel != NULL && g_pStudioModel->HasModel();
}

static void AnimControl_PostTick(float frame, float cycle)
{
	if (!g_hAnimHost || !IsWindow(g_hAnimHost))
	{
		return;
	}

	int tickFrame = (int)floorf(frame + 0.5f);
	int tickCycle = (int)floorf(cycle * 10000.0f + 0.5f);
	if (tickCycle < 0)
	{
		tickCycle = 0;
	}
	if (tickCycle > 10000)
	{
		tickCycle = 10000;
	}

	if (g_hasPosted && tickFrame == g_postedFrame && tickCycle == g_postedCycle)
	{
		return;
	}

	PostMessageA(g_hAnimHost, WM_USER + 422, (WPARAM)tickFrame, (LPARAM)tickCycle);
	g_postedFrame = tickFrame;
	g_postedCycle = tickCycle;
	g_hasPosted = true;
}

void AnimControl_Init(HWND hWndMain, HWND hHostWnd)
{
	g_hAnimWnd = hWndMain;
	g_hAnimHost = (hHostWnd && IsWindow(hHostWnd)) ? hHostWnd : NULL;
	g_loopEnabled = true;
	g_hasBaseline = false;
	g_hasPosted = false;

	if (g_hAnimWnd && g_hAnimHost)
	{
		if (SetTimer(g_hAnimWnd, ANIM_CONTROL_TIMER_ID, ANIM_CONTROL_TICK_MS, NULL) == 0)
		{
			HLMV_Log("[IPC] AnimControl: SetTimer failed, the live tick is disabled\n");
		}
	}
}

void AnimControl_Shutdown()
{
	if (g_hAnimWnd)
	{
		KillTimer(g_hAnimWnd, ANIM_CONTROL_TIMER_ID);
	}
	g_hAnimWnd = NULL;
	g_hAnimHost = NULL;
}

bool AnimControl_HandleTimer(WPARAM wParam)
{
	if ((UINT_PTR)wParam != ANIM_CONTROL_TIMER_ID)
	{
		return false;
	}

	if (!AnimControl_HasModel())
	{
		g_hasBaseline = false;
		g_hasPosted = false;
		return true;
	}

	MDLCACHE_CRITICAL_SECTION_(g_pMDLCache);

	float cycle = g_pStudioModel->GetCycle();
	float frame = g_pStudioModel->GetFrame();

	// SET_LOOP 0: a cycle that fell by more than half wrapped at the sequence
	// end, so put the playhead back on the last frame.
	if (!g_loopEnabled && g_hasBaseline && !g_viewerSettings.pause && cycle + 0.5f < g_lastCycle)
	{
		g_pStudioModel->SetFrame(g_pStudioModel->GetMaxFrame());
		if (g_ControlPanel)
		{
			g_ControlPanel->updateFrameSlider();
		}
		if (g_MatSysWindow)
		{
			g_MatSysWindow->redraw();
		}
		cycle = g_pStudioModel->GetCycle();
		frame = g_pStudioModel->GetFrame();
	}

	g_lastCycle = cycle;
	g_hasBaseline = true;

	AnimControl_PostTick(frame, cycle);
	return true;
}

void AnimControl_SyncBaseline()
{
	if (AnimControl_HasModel())
	{
		g_lastCycle = g_pStudioModel->GetCycle();
		g_hasBaseline = true;
	}
	else
	{
		g_hasBaseline = false;
	}
	// The next tick must publish the new position even when it equals the
	// last posted one (the host may have missed intermediate states).
	g_hasPosted = false;
}

void AnimControl_SetLoopEnabled(bool enabled)
{
	g_loopEnabled = enabled;
	AnimControl_SyncBaseline();
}
