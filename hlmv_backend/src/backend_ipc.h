#pragma once

#include <windows.h>
#include <string>

// Render debug modes
enum RenderDebugMode
{
    RENDER_MODE_SHADED = 0,
    RENDER_MODE_WIREFRAME,
    RENDER_MODE_ALBEDO,
    RENDER_MODE_NORMALS
};

// Global preview material pointer applied via ForcedMaterialOverride
class IMaterial;
extern IMaterial* g_pForcedPreviewMaterial;
extern std::string g_currentPreviewMaterialName;
extern bool g_bEmbedded;

// Initializes IPC (command line parameters, embedding into parent HWND, etc.)
void BackendIPC_Init(HWND hWndMain);

// Polls or handles window messages for IPC
bool BackendIPC_HandleMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT* pResult);

// Executes a command string
bool BackendIPC_ExecuteCommand(const std::string& cmd);

// Core backend actions
bool Backend_SetMaterial(const std::string& name);
bool Backend_ReloadMaterial(const std::string& name);
bool Backend_LoadModel(const std::string& path);
bool Backend_SetShape(const std::string& shape);
bool Backend_MountDirectory(const std::string& path);
bool Backend_SetLighting(float ambR, float ambG, float ambB,
                         float keyDirX, float keyDirY, float keyDirZ,
                         float keyColR, float keyColG, float keyColB,
                         float fillDirX, float fillDirY, float fillDirZ,
                         float fillColR, float fillColG, float fillColB,
                         float bgR, float bgG, float bgB);
bool Backend_SetCamera(float pitch, float yaw, float dist, float tx, float ty, float tz);

// Thumbnail capture
bool Backend_CaptureThumbnail(const std::string& material_name, int size, const std::string& out_path);

// Render debug modes
bool Backend_SetRenderMode(const std::string& mode);

// Animation & speed control
bool Backend_SetAnimation(const std::string& state);
bool Backend_SetSpeed(float speed);
