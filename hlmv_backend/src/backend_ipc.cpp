#include "backend_ipc.h"

#include <windows.h>
#include <sstream>
#include <vector>
#include <algorithm>

#include "tier0/icommandline.h"
#include "materialsystem/imaterialsystem.h"
#include "materialsystem/imaterial.h"
#include "bitmap/imageformat.h"
#include "filesystem.h"
#include "ViewerSettings.h"
#include "StudioModel.h"
#include "MDLViewer.h"
#include "ControlPanel.h"
#include "MatSysWin.h"
#include "camera.h"
#include "brush_render.h"

#include <cstdio>
#include <filesystem>

#ifndef RM_SHADED
#define RM_SHADED RM_TEXTURED
#endif

#ifndef RM_FLATSHADED
#define RM_FLATSHADED RM_SMOOTHSHADED
#endif

IMaterial* g_pForcedPreviewMaterial = nullptr;
std::string g_currentPreviewMaterialName;
bool g_bEmbedded = false;
static HWND g_hHostWnd = NULL;
extern CCamera g_cam;

extern void HLMV_Log(const char* fmt, ...);

void BackendIPC_Init(HWND hWndMain)
{
    HLMV_Log("[IPC] BackendIPC_Init started, hWndMain=%p\n", hWndMain);
    const char* pEmbed = CommandLine()->ParmValue("-embed", (const char*)0);
    if (pEmbed)
    {
        g_hHostWnd = (HWND)(uintptr_t)strtoull(pEmbed, nullptr, 0);
        HLMV_Log("[IPC] -embed target: %p (IsWindow=%d)\n", g_hHostWnd, IsWindow(g_hHostWnd));
        if (g_hHostWnd && IsWindow(g_hHostWnd))
        {
            g_bEmbedded = true;
            SetWindowTextA(hWndMain, "VMTStudio_HLMV_Backend_Canvas");
            SetParent(hWndMain, g_hHostWnd);
            SetMenu(hWndMain, NULL);

            LONG style = GetWindowLong(hWndMain, GWL_STYLE);
            style &= ~(WS_POPUP | WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
            style |= (WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN);
            SetWindowLong(hWndMain, GWL_STYLE, style);

            LONG exStyle = GetWindowLong(hWndMain, GWL_EXSTYLE);
            exStyle &= ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE);
            SetWindowLong(hWndMain, GWL_EXSTYLE, exStyle);

            RECT rc{};
            GetClientRect(g_hHostWnd, &rc);
            int width = rc.right - rc.left;
            int height = rc.bottom - rc.top;

            SetWindowPos(hWndMain, HWND_TOP, 0, 0, width, height,
                         SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

            if (g_MDLViewer)
            {
                g_MDLViewer->setBounds(0, 0, width, height);
            }
            if (g_MatSysWindow)
            {
                g_MatSysWindow->setBounds(0, 0, width, height);
                g_MatSysWindow->redraw();
            }
            PostMessage(g_hHostWnd, WM_USER + 421, (WPARAM)hWndMain, (LPARAM)GetCurrentProcessId());
            HLMV_Log("[IPC] Embedded reparented and notified host %p (w=%d, h=%d)\n", g_hHostWnd, width, height);
        }
    }

    const char* pPreviewDir = CommandLine()->ParmValue("-preview-dir", (const char*)0);
    if (pPreviewDir && pPreviewDir[0])
    {
        Backend_MountDirectory(pPreviewDir);
    }

    const char* pInitialMat = CommandLine()->ParmValue("-material", (const char*)0);
    if (pInitialMat && pInitialMat[0])
    {
        Backend_SetMaterial(pInitialMat);
    }
}

bool Backend_SetMaterial(const std::string& name)
{
    if (!g_pMaterialSystem) return false;

    if (g_pForcedPreviewMaterial)
    {
        g_pForcedPreviewMaterial->DecrementReferenceCount();
        g_pForcedPreviewMaterial = nullptr;
    }

    if (name.empty())
    {
        g_currentPreviewMaterialName.clear();
        return true;
    }

    IMaterial* pMat = g_pMaterialSystem->FindMaterial(name.c_str(), TEXTURE_GROUP_OTHER, false);
    if (pMat)
    {
        pMat->IncrementReferenceCount();
        g_pForcedPreviewMaterial = pMat;
        g_currentPreviewMaterialName = name;
        if (g_MatSysWindow) g_MatSysWindow->redraw();
        return true;
    }

    return false;
}

bool Backend_ReloadMaterial(const std::string& name)
{
    if (!g_pMaterialSystem) return false;

    const std::string target = name.empty() ? g_currentPreviewMaterialName : name;
    if (!target.empty())
    {
        g_pMaterialSystem->ReloadMaterials(target.c_str());
    }
    else
    {
        g_pMaterialSystem->ReloadMaterials();
    }

    return Backend_SetMaterial(target);
}

bool Backend_LoadModel(const std::string& path)
{
    if (path.empty() || !g_MDLViewer) return false;

    BrushRender_SetShape(BRUSH_SHAPE_NONE);
    g_MDLViewer->LoadModelFile(path.c_str());
    if (g_MatSysWindow) g_MatSysWindow->redraw();
    return true;
}

bool Backend_SetShape(const std::string& shape)
{
    if (shape == "brush_wall" || shape == "wall")
    {
        BrushRender_SetShape(BRUSH_SHAPE_WALL);
        if (g_MatSysWindow) g_MatSysWindow->redraw();
        return true;
    }
    else if (shape == "brush_floor" || shape == "floor")
    {
        BrushRender_SetShape(BRUSH_SHAPE_FLOOR);
        if (g_MatSysWindow) g_MatSysWindow->redraw();
        return true;
    }
    else if (shape == "brush_cube")
    {
        BrushRender_SetShape(BRUSH_SHAPE_CUBE);
        if (g_MatSysWindow) g_MatSysWindow->redraw();
        return true;
    }
    else if (shape == "brush_displacement" || shape == "displacement")
    {
        BrushRender_SetShape(BRUSH_SHAPE_DISPLACEMENT);
        if (g_MatSysWindow) g_MatSysWindow->redraw();
        return true;
    }
    else
    {
        BrushRender_SetShape(BRUSH_SHAPE_NONE);
    }

    std::string modelPath;
    if (shape == "sphere") modelPath = "models/preview/preview_sphere.mdl";
    else if (shape == "cube") modelPath = "models/preview/preview_cube.mdl";
    else if (shape == "cylinder") modelPath = "models/preview/preview_cylinder.mdl";
    else if (shape == "plane") modelPath = "models/preview/preview_plane.mdl";
    else modelPath = shape;

    if (g_pFileSystem && !g_pFileSystem->FileExists(modelPath.c_str(), "GAME"))
    {
        if (shape == "sphere" && g_pFileSystem->FileExists("models/props_junk/watermelon01.mdl", "GAME"))
            modelPath = "models/props_junk/watermelon01.mdl";
        else if (shape == "cylinder" && g_pFileSystem->FileExists("models/props_c17/oildrum001.mdl", "GAME"))
            modelPath = "models/props_c17/oildrum001.mdl";
        else if (shape == "cube" && g_pFileSystem->FileExists("models/props_junk/wood_crate001a.mdl", "GAME"))
            modelPath = "models/props_junk/wood_crate001a.mdl";
        else if (g_pFileSystem->FileExists("models/error.mdl", "GAME"))
            modelPath = "models/error.mdl";
    }

    return Backend_LoadModel(modelPath);
}

bool Backend_MountDirectory(const std::string& path)
{
    if (!g_pFileSystem || path.empty()) return false;
    std::string clean = path;
    if (clean.size() >= 2 && clean.front() == '"' && clean.back() == '"')
    {
        clean = clean.substr(1, clean.size() - 2);
    }
    while (!clean.empty() && (clean.back() == '/' || clean.back() == '\\'))
    {
        clean.pop_back();
    }
    if (clean.empty()) return false;
    g_pFileSystem->AddSearchPath(clean.c_str(), "GAME", PATH_ADD_TO_HEAD);
    HLMV_Log("[IPC] Mounted directory: %s\n", clean.c_str());
    return true;
}

bool Backend_SetLighting(float ambR, float ambG, float ambB,
                         float keyDirX, float keyDirY, float keyDirZ,
                         float keyColR, float keyColG, float keyColB,
                         float fillDirX, float fillDirY, float fillDirZ,
                         float fillColR, float fillColG, float fillColB,
                         float bgR, float bgG, float bgB)
{
    g_viewerSettings.aColor[0] = ambR;
    g_viewerSettings.aColor[1] = ambG;
    g_viewerSettings.aColor[2] = ambB;
    g_viewerSettings.aColor[3] = 1.0f;

    g_viewerSettings.lColor[0] = keyColR;
    g_viewerSettings.lColor[1] = keyColG;
    g_viewerSettings.lColor[2] = keyColB;
    g_viewerSettings.lColor[3] = 1.0f;

    g_viewerSettings.bgColor[0] = bgR;
    g_viewerSettings.bgColor[1] = bgG;
    g_viewerSettings.bgColor[2] = bgB;
    g_viewerSettings.bgColor[3] = 1.0f;

    if (g_MatSysWindow) g_MatSysWindow->redraw();
    return true;
}

bool Backend_SetCamera(float pitch, float yaw, float dist, float tx, float ty, float tz)
{
    g_cam.m_orbit.angles.x = pitch;
    g_cam.m_orbit.angles.y = yaw;
    g_cam.m_orbit.angles.z = 0.0f;
    if (dist > 0.0f) g_cam.m_orbit.zoom = dist;
    g_cam.m_orbit.origin.Init(tx, ty, tz);

    if (g_MatSysWindow) g_MatSysWindow->redraw();
    return true;
}

static bool WriteTga32(const std::string& path, int width, int height, const unsigned char* bgra)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;

    unsigned char header[18] = { 0 };
    header[2] = 2; // uncompressed true-color
    header[12] = (unsigned char)(width & 0xFF);
    header[13] = (unsigned char)((width >> 8) & 0xFF);
    header[14] = (unsigned char)(height & 0xFF);
    header[15] = (unsigned char)((height >> 8) & 0xFF);
    header[16] = 32; // 32 bits per pixel
    header[17] = 0x20 | 0x08; // 0x28 = top-left origin, 8-bit alpha

    size_t written = fwrite(header, 1, 18, f);
    if (written == 18)
    {
        size_t pixelBytes = (size_t)width * height * 4;
        written = fwrite(bgra, 1, pixelBytes, f);
        if (written == pixelBytes)
        {
            fclose(f);
            return true;
        }
    }

    fclose(f);
    return false;
}

static bool WriteBmp32(const std::string& path, int width, int height, const unsigned char* bgra)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;

    BITMAPFILEHEADER bfh = { 0 };
    bfh.bfType = 0x4D42; // "BM"
    bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bfh.bfSize = bfh.bfOffBits + width * height * 4;

    BITMAPINFOHEADER bih = { 0 };
    bih.biSize = sizeof(BITMAPINFOHEADER);
    bih.biWidth = width;
    bih.biHeight = -height; // top-down DIB
    bih.biPlanes = 1;
    bih.biBitCount = 32;
    bih.biCompression = BI_RGB;

    fwrite(&bfh, sizeof(bfh), 1, f);
    fwrite(&bih, sizeof(bih), 1, f);
    fwrite(bgra, 1, (size_t)width * height * 4, f);
    fclose(f);
    return true;
}

static std::vector<std::string> TokenizeArgs(const std::string& str)
{
    std::vector<std::string> tokens;
    std::string current;
    bool inQuote = false;
    bool hadQuote = false;
    for (size_t i = 0; i < str.size(); ++i)
    {
        char c = str[i];
        if (c == '\"')
        {
            inQuote = !inQuote;
            hadQuote = true;
        }
        else if (isspace((unsigned char)c) && !inQuote)
        {
            if (!current.empty() || hadQuote)
            {
                tokens.push_back(current);
                current.clear();
                hadQuote = false;
            }
        }
        else
        {
            current += c;
        }
    }
    if (!current.empty() || hadQuote)
    {
        tokens.push_back(current);
    }
    return tokens;
}

bool Backend_CaptureThumbnail(const std::string& material_name, int size, const std::string& out_path)
{
    if (size <= 0 || out_path.empty() || !g_pMaterialSystem || !g_MatSysWindow)
    {
        return false;
    }

    std::string savedMat = g_currentPreviewMaterialName;
    bool matChanged = false;

    if (!material_name.empty() && material_name != "-" && material_name != savedMat)
    {
        if (Backend_SetMaterial(material_name))
        {
            matChanged = true;
        }
    }

    // Render the frame into the backbuffer
    g_MatSysWindow->draw();

    CMatRenderContextPtr ctx(g_pMaterialSystem);
    if (!ctx)
    {
        if (matChanged)
        {
            Backend_SetMaterial(savedMat);
        }
        return false;
    }

    ctx->Flush(true);

    std::vector<unsigned char> pixels((size_t)size * size * 4);
    ctx->ReadPixels(0, 0, size, size, pixels.data(), IMAGE_FORMAT_BGRA8888);

    try
    {
        std::filesystem::path p(out_path);
        if (p.has_parent_path())
        {
            std::filesystem::create_directories(p.parent_path());
        }
    }
    catch (...)
    {
    }

    std::string ext;
    size_t dotPos = out_path.rfind('.');
    if (dotPos != std::string::npos)
    {
        ext = out_path.substr(dotPos);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    }

    bool success = false;
    if (ext == ".bmp")
    {
        success = WriteBmp32(out_path, size, size, pixels.data());
    }
    else
    {
        success = WriteTga32(out_path, size, size, pixels.data());
    }

    if (matChanged)
    {
        Backend_SetMaterial(savedMat);
    }

    return success;
}

bool Backend_SetRenderMode(const std::string& mode)
{
    std::string m = mode;
    std::transform(m.begin(), m.end(), m.begin(), ::tolower);

    if (m == "shaded")
    {
        g_viewerSettings.renderMode = RM_SHADED;
        g_viewerSettings.showNormals = false;
    }
    else if (m == "wireframe")
    {
        g_viewerSettings.renderMode = RM_WIREFRAME;
        g_viewerSettings.showNormals = false;
    }
    else if (m == "normals")
    {
        g_viewerSettings.showNormals = true;
        g_viewerSettings.renderMode = RM_SHADED;
    }
    else if (m == "albedo")
    {
        g_viewerSettings.renderMode = RM_FLATSHADED;
        g_viewerSettings.showNormals = false;
    }
    else
    {
        return false;
    }

    if (g_ControlPanel)
    {
        g_ControlPanel->setRenderMode(g_viewerSettings.renderMode);
        g_ControlPanel->setShowNormals(g_viewerSettings.showNormals);
    }

    StudioModel::UpdateStudioRenderConfig(
        g_viewerSettings.renderMode == RM_WIREFRAME,
        false,
        g_viewerSettings.showNormals,
        g_viewerSettings.showTangentFrame
    );

    if (g_MatSysWindow)
    {
        g_MatSysWindow->redraw();
    }

    return true;
}

bool Backend_SetAnimation(const std::string& state)
{
    std::string s = state;
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);

    if (s == "play")
    {
        g_viewerSettings.pause = false;
    }
    else if (s == "pause")
    {
        g_viewerSettings.pause = true;
    }
    else if (s == "toggle")
    {
        g_viewerSettings.pause = !g_viewerSettings.pause;
    }
    else
    {
        return false;
    }

    if (g_MatSysWindow)
    {
        g_MatSysWindow->redraw();
    }

    return true;
}

bool Backend_SetSpeed(float speed)
{
    if (speed < 0.0f)
    {
        speed = 0.0f;
    }
    g_viewerSettings.speedScale = speed;

    if (g_ControlPanel)
    {
        g_ControlPanel->setSpeedScale(speed);
    }

    if (g_MatSysWindow)
    {
        g_MatSysWindow->redraw();
    }

    return true;
}

bool BackendIPC_ExecuteCommand(const std::string& cmd)
{
    std::istringstream iss(cmd);
    std::string verb;
    iss >> verb;

    if (verb == "PING")
    {
        return true;
    }
    else if (verb == "MOUNT_DIR")
    {
        std::string path;
        std::getline(iss >> std::ws, path);
        return Backend_MountDirectory(path);
    }
    else if (verb == "SET_MATERIAL")
    {
        std::string mat;
        std::getline(iss >> std::ws, mat);
        return Backend_SetMaterial(mat);
    }
    else if (verb == "RELOAD_MATERIAL")
    {
        std::string mat;
        std::getline(iss >> std::ws, mat);
        return Backend_ReloadMaterial(mat);
    }
    else if (verb == "LOAD_MODEL")
    {
        std::string model;
        std::getline(iss >> std::ws, model);
        return Backend_LoadModel(model);
    }
    else if (verb == "SET_SHAPE")
    {
        std::string shape;
        iss >> shape;
        return Backend_SetShape(shape);
    }
    else if (verb == "CAPTURE_THUMBNAIL")
    {
        std::vector<std::string> args = TokenizeArgs(cmd);
        if (args.size() >= 4)
        {
            std::string mat = args[1];
            if (mat == "-" || mat == "\"\"" || mat == "none")
            {
                mat.clear();
            }
            int size = atoi(args[2].c_str());
            std::string out_path = args[3];
            for (size_t i = 4; i < args.size(); ++i)
            {
                out_path += " " + args[i];
            }
            return Backend_CaptureThumbnail(mat, size, out_path);
        }
        return false;
    }
    else if (verb == "SET_RENDER_MODE")
    {
        std::string mode;
        iss >> mode;
        return Backend_SetRenderMode(mode);
    }
    else if (verb == "SET_ANIMATION")
    {
        std::string state;
        iss >> state;
        return Backend_SetAnimation(state);
    }
    else if (verb == "SET_SPEED")
    {
        float speed = 1.0f;
        iss >> speed;
        return Backend_SetSpeed(speed);
    }
    else if (verb == "SET_LIGHTING")
    {
        float aR = 0.3f, aG = 0.3f, aB = 0.3f;
        float kX = 0.0f, kY = 0.0f, kZ = -1.0f, kR = 1.0f, kG = 1.0f, kB = 1.0f;
        float fX = 0.0f, fY = 0.0f, fZ = 1.0f, fR = 0.2f, fG = 0.2f, fB = 0.2f;
        float bgR = 0.16f, bgG = 0.16f, bgB = 0.17f;
        iss >> aR >> aG >> aB
            >> kX >> kY >> kZ >> kR >> kG >> kB
            >> fX >> fY >> fZ >> fR >> fG >> fB
            >> bgR >> bgG >> bgB;
        return Backend_SetLighting(aR, aG, aB, kX, kY, kZ, kR, kG, kB, fX, fY, fZ, fR, fG, fB, bgR, bgG, bgB);
    }
    else if (verb == "SET_CAM")
    {
        float p = 0.0f, y = 0.0f, d = 0.0f, tx = 0.0f, ty = 0.0f, tz = 0.0f;
        iss >> p >> y >> d >> tx >> ty >> tz;
        return Backend_SetCamera(p, y, d, tx, ty, tz);
    }
    else if (verb == "RESIZE")
    {
        int w = 0, h = 0;
        iss >> w >> h;
        if (w > 0 && h > 0 && g_MDLViewer)
        {
            g_MDLViewer->setBounds(0, 0, w, h);
            if (g_MatSysWindow)
            {
                g_MatSysWindow->setBounds(0, 0, w, h);
                g_MatSysWindow->redraw();
            }
            return true;
        }
    }
    else if (verb == "QUIT")
    {
        PostQuitMessage(0);
        return true;
    }

    return false;
}

bool BackendIPC_HandleMessage(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam, LRESULT* pResult)
{
    if (uMsg == WM_COPYDATA)
    {
        COPYDATASTRUCT* pcds = (COPYDATASTRUCT*)lParam;
        if (pcds && pcds->lpData && pcds->cbData > 0)
        {
            std::string cmd((const char*)pcds->lpData, pcds->cbData);
            while (!cmd.empty() && (cmd.back() == '\0' || cmd.back() == '\r' || cmd.back() == '\n'))
            {
                cmd.pop_back();
            }
            HLMV_Log("[IPC] Executing command: '%s'\n", cmd.c_str());
            bool ok = BackendIPC_ExecuteCommand(cmd);
            HLMV_Log("[IPC] Command '%s' result: %d\n", cmd.c_str(), ok);
            if (pResult) *pResult = ok ? 1 : 0;
            return true;
        }
    }
    else if (uMsg == (WM_USER + 420)) // Fast ping
    {
        HLMV_Log("[IPC] Received Fast Ping (WM_USER+420)\n");
        if (pResult) *pResult = 1;
        return true;
    }

    return false;
}
