#include "backend_ipc.h"
#include "backend_query.h"
#include "anim_control.h"

#include <windows.h>
#include <sstream>
#include <vector>
#include <algorithm>
#include <cmath>

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

//-----------------------------------------------------------------------------
// Helpers shared by the v2 command handlers (docs/IPC_PROTOCOL.md)
//-----------------------------------------------------------------------------

static void IPC_TrimString(std::string& text)
{
    size_t first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        text.clear();
        return;
    }
    size_t last = text.find_last_not_of(" \t\r\n");
    text = text.substr(first, last - first + 1);
}

static void IPC_RedrawViewport()
{
    if (g_MatSysWindow)
    {
        g_MatSysWindow->redraw();
    }
}

static bool IPC_HasModel()
{
    return g_pStudioModel && g_pStudioModel->HasModel();
}

// Applies a sequence index; the control panel keeps its combo boxes and the
// frame slider in sync (its selection table has room for 2048 sequences).
static bool IPC_ApplySequenceIndex(int index)
{
    CStudioHdr *hdr = g_pStudioModel->GetStudioHdr();
    if (!hdr || index < 0 || index >= hdr->GetNumSeq())
    {
        return false;
    }

    if (g_ControlPanel && index < 2048)
    {
        g_ControlPanel->setSequence(index);
    }
    else
    {
        g_pStudioModel->SetSequence(index);
    }

    AnimControl_SyncBaseline();
    return true;
}

static bool IPC_ApplyViewFlag(const std::string& flag, bool value)
{
    if (flag == "bones")
    {
        if (g_ControlPanel) g_ControlPanel->setShowBones(value);
        else g_viewerSettings.showBones = value;
    }
    else if (flag == "hitboxes")
    {
        if (g_ControlPanel) g_ControlPanel->setShowHitBoxes(value);
        else g_viewerSettings.showHitBoxes = value;
    }
    else if (flag == "attachments")
    {
        if (g_ControlPanel) g_ControlPanel->setShowAttachments(value);
        else g_viewerSettings.showAttachments = value;
    }
    else if (flag == "ground")
    {
        if (g_ControlPanel) g_ControlPanel->setShowGround(value);
        else g_viewerSettings.showGround = value;
    }
    else if (flag == "movement")
    {
        if (g_ControlPanel) g_ControlPanel->setShowMovement(value);
        else g_viewerSettings.showMovement = value;
    }
    else if (flag == "overlay_wireframe")
    {
        if (g_ControlPanel) g_ControlPanel->setOverlayWireframe(value);
        else g_viewerSettings.overlayWireframe = value;
    }
    else if (flag == "normals")
    {
        if (g_ControlPanel) g_ControlPanel->setShowNormals(value);
        else g_viewerSettings.showNormals = value;
        StudioModel::UpdateStudioRenderConfig(g_viewerSettings.renderMode == RM_WIREFRAME, false,
                                              g_viewerSettings.showNormals, g_viewerSettings.showTangentFrame);
    }
    else if (flag == "tangents")
    {
        if (g_ControlPanel) g_ControlPanel->setShowTangentFrame(value);
        else g_viewerSettings.showTangentFrame = value;
        StudioModel::UpdateStudioRenderConfig(g_viewerSettings.renderMode == RM_WIREFRAME, false,
                                              g_viewerSettings.showNormals, g_viewerSettings.showTangentFrame);
    }
    else if (flag == "shadow")
    {
        if (g_ControlPanel) g_ControlPanel->setShowShadow(value);
        else g_viewerSettings.showShadow = value;
    }
    else if (flag == "sequence_boxes")
    {
        // The renderer reads the setting directly; the panel has no setter
        // for its checkbox, so the checkbox may show a stale state.
        g_viewerSettings.showSequenceBoxes = value;
    }
    else
    {
        return false;
    }
    return true;
}


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

    // IPC protocol v2: the DUMP/RAW response-file directory and the live tick.
    const char* pIpcDir = CommandLine()->ParmValue("-ipc-dir", (const char*)0);
    BackendQuery_SetResponseDir(pIpcDir ? pIpcDir : "");
    BackendQuery_SetMainWindow(hWndMain);
    AnimControl_Init(hWndMain, g_hHostWnd);

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

    // An empty argument (v1) reloads the material of the current override; a
    // name equal to the override reloads and re-resolves it. Any other name is
    // reloaded in place and leaves the override untouched, which is what the
    // UI wants when it edits a material of the loaded model (docs/IPC_PROTOCOL.md,
    // SET_MATERIAL). ReloadMaterials matches by substring, not by exact name.
    const bool isOverrideReload = name.empty() ||
        _stricmp(name.c_str(), g_currentPreviewMaterialName.c_str()) == 0;

    if (isOverrideReload)
    {
        if (g_currentPreviewMaterialName.empty())
        {
            g_pMaterialSystem->ReloadMaterials();
            IPC_RedrawViewport();
            return true;
        }
        g_pMaterialSystem->ReloadMaterials(g_currentPreviewMaterialName.c_str());
        return Backend_SetMaterial(g_currentPreviewMaterialName);
    }

    g_pMaterialSystem->ReloadMaterials(name.c_str());
    IPC_RedrawViewport();
    return true;
}

bool Backend_LoadModel(const std::string& path)
{
    if (path.empty() || !g_MDLViewer) return false;

    BrushRender_SetShape(BRUSH_SHAPE_NONE);
    g_MDLViewer->LoadModelFile(path.c_str());
    BackendQuery_ResetSubmodels();
    AnimControl_SyncBaseline();
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
    if (shape == "sphere")
    {
        if (g_pFileSystem && g_pFileSystem->FileExists("models/shpere_model.mdl", "GAME"))
            modelPath = "models/shpere_model.mdl";
        else if (g_pFileSystem && g_pFileSystem->FileExists("models/sphere_model.mdl", "GAME"))
            modelPath = "models/sphere_model.mdl";
        else if (g_pFileSystem && g_pFileSystem->FileExists("models/preview/preview_sphere.mdl", "GAME"))
            modelPath = "models/preview/preview_sphere.mdl";
        else if (g_pFileSystem && g_pFileSystem->FileExists("models/props_junk/watermelon01.mdl", "GAME"))
            modelPath = "models/props_junk/watermelon01.mdl";
        else
            modelPath = "models/shpere_model.mdl";
    }
    else if (shape == "cube")
    {
        if (g_pFileSystem && g_pFileSystem->FileExists("models/cube_model.mdl", "GAME"))
            modelPath = "models/cube_model.mdl";
        else if (g_pFileSystem && g_pFileSystem->FileExists("models/preview/preview_cube.mdl", "GAME"))
            modelPath = "models/preview/preview_cube.mdl";
        else if (g_pFileSystem && g_pFileSystem->FileExists("models/props_junk/wood_crate001a.mdl", "GAME"))
            modelPath = "models/props_junk/wood_crate001a.mdl";
        else
            modelPath = "models/cube_model.mdl";
    }
    else if (shape == "cylinder")
    {
        if (g_pFileSystem && g_pFileSystem->FileExists("models/cylinder_model.mdl", "GAME"))
            modelPath = "models/cylinder_model.mdl";
        else if (g_pFileSystem && g_pFileSystem->FileExists("models/preview/preview_cylinder.mdl", "GAME"))
            modelPath = "models/preview/preview_cylinder.mdl";
        else if (g_pFileSystem && g_pFileSystem->FileExists("models/props_c17/oildrum001.mdl", "GAME"))
            modelPath = "models/props_c17/oildrum001.mdl";
        else
            modelPath = "models/cylinder_model.mdl";
    }
    else if (shape == "plane")
    {
        modelPath = "models/preview/preview_plane.mdl";
    }
    else
    {
        modelPath = shape;
    }

    if (g_pFileSystem && !g_pFileSystem->FileExists(modelPath.c_str(), "GAME"))
    {
        if (g_pFileSystem->FileExists("models/error.mdl", "GAME"))
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
        // Protocol v2 clamps the scale to 0.01 .. 8.0; a non-finite or
        // non-numeric value is a malformed argument and leaves the backend
        // untouched.
        float speed = 1.0f;
        if (!(iss >> speed) || !std::isfinite(speed))
        {
            return false;
        }
        if (speed < 0.01f) speed = 0.01f;
        if (speed > 8.0f) speed = 8.0f;
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
    else if (verb == "SET_SEQUENCE")
    {
        int index = -1;
        if (!(iss >> index) || !IPC_HasModel())
        {
            return false;
        }
        if (!IPC_ApplySequenceIndex(index))
        {
            return false;
        }
        IPC_RedrawViewport();
        return true;
    }
    else if (verb == "SET_SEQUENCE_LABEL")
    {
        std::string label;
        std::getline(iss >> std::ws, label);
        IPC_TrimString(label);
        if (label.empty() || !IPC_HasModel())
        {
            return false;
        }
        int index = g_pStudioModel->LookupSequence(label.c_str());
        if (index < 0 || !IPC_ApplySequenceIndex(index))
        {
            return false;
        }
        IPC_RedrawViewport();
        return true;
    }
    else if (verb == "SET_FRAME")
    {
        int frame = -1;
        if (!(iss >> frame) || frame < 0 || !IPC_HasModel())
        {
            return false;
        }
        g_pStudioModel->SetFrame(frame); // clamps to [0, last frame]
        if (g_ControlPanel) g_ControlPanel->updateFrameSlider();
        AnimControl_SyncBaseline();
        IPC_RedrawViewport();
        return true;
    }
    else if (verb == "STEP_FRAME")
    {
        int delta = 0;
        if (!(iss >> delta) || !IPC_HasModel())
        {
            return false;
        }
        int frame = (int)(g_pStudioModel->GetFrame() + 0.5f) + delta;
        int maxFrame = g_pStudioModel->GetMaxFrame();
        if (frame < 0) frame = 0;
        if (frame > maxFrame) frame = maxFrame;
        g_pStudioModel->SetFrame(frame);
        if (g_ControlPanel) g_ControlPanel->updateFrameSlider();
        AnimControl_SyncBaseline();
        IPC_RedrawViewport();
        return true;
    }
    else if (verb == "SET_LOOP")
    {
        int enabled = -1;
        if (!(iss >> enabled) || (enabled != 0 && enabled != 1) || !IPC_HasModel())
        {
            return false;
        }
        AnimControl_SetLoopEnabled(enabled != 0);
        IPC_RedrawViewport();
        return true;
    }
    else if (verb == "SET_STOP")
    {
        if (!IPC_HasModel())
        {
            return false;
        }
        g_viewerSettings.pause = true;
        g_pStudioModel->SetFrame(0);
        if (g_ControlPanel) g_ControlPanel->updateFrameSlider();
        AnimControl_SyncBaseline();
        IPC_RedrawViewport();
        return true;
    }
    else if (verb == "SET_BLEND_TIME")
    {
        // StudioModel::SetBlendTime ignores values <= 0; anything else is set.
        float seconds = -1.0f;
        if (!(iss >> seconds) || !std::isfinite(seconds) || seconds < 0.0f || !IPC_HasModel())
        {
            return false;
        }
        g_pStudioModel->SetBlendTime(seconds);
        IPC_RedrawViewport();
        return true;
    }
    else if (verb == "BLEND_NOW")
    {
        if (!IPC_HasModel())
        {
            return false;
        }
        g_pStudioModel->StartBlending();
        AnimControl_SyncBaseline();
        IPC_RedrawViewport();
        return true;
    }
    else if (verb == "SET_OVERLAY")
    {
        int layer = 0;
        int sequence = 0;
        float weight = 0.0f;
        if (!(iss >> layer >> sequence >> weight) || !std::isfinite(weight) ||
            layer < 1 || layer > 4 || !IPC_HasModel())
        {
            return false;
        }

        CStudioHdr* hdr = g_pStudioModel->GetStudioHdr();
        if (!hdr)
        {
            return false;
        }

        if (sequence < 0)
        {
            // Clearing the layer: sequence 0 at weight 0. The panel setter is
            // 1-based and must never index with -1.
            if (g_ControlPanel && hdr->GetNumSeq() > 0)
            {
                g_ControlPanel->setOverlaySequence(layer, 0, 0.0f);
            }
            else
            {
                g_pStudioModel->SetOverlaySequence(layer - 1, 0, 0.0f);
            }
        }
        else
        {
            if (sequence >= hdr->GetNumSeq())
            {
                return false;
            }
            if (weight < 0.0f) weight = 0.0f;
            if (weight > 1.0f) weight = 1.0f;
            if (g_ControlPanel)
            {
                g_ControlPanel->setOverlaySequence(layer, sequence, weight);
            }
            else
            {
                g_pStudioModel->SetOverlaySequence(layer - 1, sequence, weight);
            }
        }

        AnimControl_SyncBaseline();
        IPC_RedrawViewport();
        return true;
    }
    else if (verb == "SET_POSEPARAM")
    {
        int index = -1;
        float value = 0.0f;
        if (!(iss >> index >> value) || !std::isfinite(value) || !IPC_HasModel())
        {
            return false;
        }

        CStudioHdr* hdr = g_pStudioModel->GetStudioHdr();
        if (!hdr || index < 0 || index >= hdr->GetNumPoseParameters())
        {
            return false;
        }

        float minValue = 0.0f;
        float maxValue = 0.0f;
        if (g_pStudioModel->GetPoseParameterRange(index, &minValue, &maxValue))
        {
            if (value < minValue) value = minValue;
            if (value > maxValue) value = maxValue;
        }

        g_pStudioModel->SetPoseParameter(index, value);
        if (g_ControlPanel) g_ControlPanel->updatePoseParameters();
        IPC_RedrawViewport();
        return true;
    }
    else if (verb == "SET_BODYGROUP")
    {
        int group = -1;
        int value = -1;
        if (!(iss >> group >> value) || group < 0 || value < 0 || !IPC_HasModel())
        {
            return false;
        }

        CStudioHdr* hdr = g_pStudioModel->GetStudioHdr();
        if (!hdr || group >= hdr->numbodyparts())
        {
            return false;
        }
        mstudiobodyparts_t* pBodypart = hdr->pBodypart(group);
        if (!pBodypart || value >= pBodypart->nummodels)
        {
            return false;
        }

        // The control panel owns the bodypart/submodel combo boxes; the engine
        // mutation happens through cBodypart::getSelectedIndex().
        if (g_ControlPanel)
        {
            g_ControlPanel->setBodypart(group);
            g_ControlPanel->setSubmodel(value);
        }
        else
        {
            g_pStudioModel->SetBodygroup(group, value);
        }
        BackendQuery_NoteSubmodel(group, value);
        IPC_RedrawViewport();
        return true;
    }
    else if (verb == "SET_SKIN")
    {
        int skin = -1;
        if (!(iss >> skin) || skin < 0 || !IPC_HasModel())
        {
            return false;
        }

        CStudioHdr* hdr = g_pStudioModel->GetStudioHdr();
        if (!hdr || skin >= hdr->numskinfamilies())
        {
            return false;
        }

        g_pStudioModel->SetSkin(skin);
        g_viewerSettings.skin = skin; // the panel combo has no public setter
        IPC_RedrawViewport();
        return true;
    }
    else if (verb == "SET_VIEWFLAG")
    {
        std::string flag;
        int value = -1;
        if (!(iss >> flag >> value) || (value != 0 && value != 1) || !IPC_HasModel())
        {
            return false;
        }
        if (!IPC_ApplyViewFlag(flag, value != 0))
        {
            return false;
        }
        IPC_RedrawViewport();
        return true;
    }
    else if (verb == "DUMP")
    {
        std::string kind;
        std::string token;
        if (!(iss >> kind >> token))
        {
            return false;
        }
        std::string args;
        std::getline(iss >> std::ws, args);
        IPC_TrimString(args);
        return BackendQuery_HandleDump(kind, token, args);
    }
    else if (verb == "RAW")
    {
        std::string token;
        if (!(iss >> token))
        {
            return false;
        }
        std::string path;
        std::getline(iss >> std::ws, path);
        IPC_TrimString(path);
        return BackendQuery_HandleRaw(token, path);
    }
    else if (verb == "QUIT")
    {
        AnimControl_Shutdown();
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
    else if (uMsg == VMTIPC_QUERY_JOB_MESSAGE)
    {
        // Deferred DUMP/RAW work; the WM_COPYDATA handler only queued the job.
        BackendQuery_RunJob(wParam, lParam);
        if (pResult) *pResult = 1;
        return true;
    }
    else if (uMsg == WM_TIMER)
    {
        // Only consume the animation timer; foreign timers (mx widgets) must
        // keep flowing to the normal dispatch.
        if (AnimControl_HandleTimer(wParam))
        {
            if (pResult) *pResult = 0;
            return true;
        }
    }

    return false;
}
