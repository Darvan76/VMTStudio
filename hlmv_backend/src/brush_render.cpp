#include "brush_render.h"

#include "materialsystem/imaterialsystem.h"
#include "materialsystem/imaterial.h"
#include "materialsystem/imaterialvar.h"
#include "materialsystem/imesh.h"
#include "mathlib/vector.h"
#include "tier0/dbg.h"
#include "ViewerSettings.h"
#include "matsyswin.h"

extern IMaterialSystem* g_pMaterialSystem;

static BrushShape s_currentBrushShape = BRUSH_SHAPE_NONE;

void BrushRender_SetShape(BrushShape shape)
{
    s_currentBrushShape = shape;
}

BrushShape BrushRender_GetShape()
{
    return s_currentBrushShape;
}

bool BrushRender_IsBrushShape()
{
    return s_currentBrushShape != BRUSH_SHAPE_NONE;
}

namespace
{

struct BrushVertex
{
    float x, y, z;
    float nx, ny, nz;
    float u0, v0;
    float u1, v1;
    unsigned char r, g, b, a;
    float tx, ty, tz;
    float bx, by, bz;
};

inline void EmitVertex(CMeshBuilder& mb, const BrushVertex& v, bool bHasColor, bool bHasLightmapUV, bool bHasTangent)
{
    mb.Position3f(v.x, v.y, v.z);
    mb.Normal3f(v.nx, v.ny, v.nz);
    mb.TexCoord2f(0, v.u0, v.v0);
    if (bHasLightmapUV)
    {
        mb.TexCoord2f(1, v.u1, v.v1);
    }
    if (bHasColor)
    {
        mb.Color4ub(v.r, v.g, v.b, v.a);
    }
    if (bHasTangent)
    {
        mb.TangentS3f(v.tx, v.ty, v.tz);
        mb.TangentT3f(v.bx, v.by, v.bz);
    }
    mb.AdvanceVertex();
}

void BuildWall(IMesh* pMesh, bool bHasColor, bool bHasLightmapUV, bool bHasTangent)
{
    // Vertical quad centered at (0, 0, 64), width 128 (in Y: -64 to +64), height 128 (in Z: 0 to 128)
    // Facing +X (towards default HLMV camera looking towards origin from +X)
    CMeshBuilder mb;
    mb.Begin(pMesh, MATERIAL_QUADS, 1);

    const BrushVertex verts[4] = {
        // v0: Bottom-Right
        { 0.0f, -64.0f,   0.0f,  1.0f, 0.0f, 0.0f,  1.0f, 1.0f,  0.5f, 0.5f,  255, 255, 255, 255,  0.0f, -1.0f, 0.0f,  0.0f, 0.0f, -1.0f },
        // v1: Bottom-Left
        { 0.0f,  64.0f,   0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f,  0.5f, 0.5f,  255, 255, 255, 255,  0.0f, -1.0f, 0.0f,  0.0f, 0.0f, -1.0f },
        // v2: Top-Left
        { 0.0f,  64.0f, 128.0f,  1.0f, 0.0f, 0.0f,  0.0f, 0.0f,  0.5f, 0.5f,  255, 255, 255, 255,  0.0f, -1.0f, 0.0f,  0.0f, 0.0f, -1.0f },
        // v3: Top-Right
        { 0.0f, -64.0f, 128.0f,  1.0f, 0.0f, 0.0f,  1.0f, 0.0f,  0.5f, 0.5f,  255, 255, 255, 255,  0.0f, -1.0f, 0.0f,  0.0f, 0.0f, -1.0f },
    };

    for (int i = 0; i < 4; ++i)
    {
        EmitVertex(mb, verts[i], bHasColor, bHasLightmapUV, bHasTangent);
    }

    mb.End();
}

void BuildFloor(IMesh* pMesh, bool bHasColor, bool bHasLightmapUV, bool bHasTangent)
{
    // Horizontal 128x128 quad in XY plane at Z=0, facing +Z (normal 0, 0, 1)
    // Centered at (0, 0, 0), X in [-64, 64], Y in [-64, 64]
    CMeshBuilder mb;
    mb.Begin(pMesh, MATERIAL_QUADS, 1);

    const BrushVertex verts[4] = {
        // v0: Near-Right (X=+64, Y=-64)
        {  64.0f, -64.0f, 0.0f,  0.0f, 0.0f, 1.0f,  1.0f, 1.0f,  0.5f, 0.5f,  255, 255, 255, 255,  1.0f, 0.0f, 0.0f,  0.0f, -1.0f, 0.0f },
        // v1: Near-Left (X=+64, Y=+64)
        {  64.0f,  64.0f, 0.0f,  0.0f, 0.0f, 1.0f,  0.0f, 1.0f,  0.5f, 0.5f,  255, 255, 255, 255,  1.0f, 0.0f, 0.0f,  0.0f, -1.0f, 0.0f },
        // v2: Far-Left (X=-64, Y=+64)
        { -64.0f,  64.0f, 0.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f,  0.5f, 0.5f,  255, 255, 255, 255,  1.0f, 0.0f, 0.0f,  0.0f, -1.0f, 0.0f },
        // v3: Far-Right (X=-64, Y=-64)
        { -64.0f, -64.0f, 0.0f,  0.0f, 0.0f, 1.0f,  1.0f, 0.0f,  0.5f, 0.5f,  255, 255, 255, 255,  1.0f, 0.0f, 0.0f,  0.0f, -1.0f, 0.0f },
    };

    for (int i = 0; i < 4; ++i)
    {
        EmitVertex(mb, verts[i], bHasColor, bHasLightmapUV, bHasTangent);
    }

    mb.End();
}

void BuildCube(IMesh* pMesh, bool bHasColor, bool bHasLightmapUV, bool bHasTangent)
{
    // 64x64x64 box centered at (0, 0, 32), 6 quad faces, each face with correct normal, UVs, and tangents
    CMeshBuilder mb;
    mb.Begin(pMesh, MATERIAL_QUADS, 6);

    const BrushVertex verts[24] = {
        // 1. Front face (+X, normal 1, 0, 0)
        {  32.0f, -32.0f,  0.0f,  1.0f, 0.0f, 0.0f,  1.0f, 1.0f,  0.5f, 0.5f,  255, 255, 255, 255,  0.0f, -1.0f, 0.0f,  0.0f, 0.0f, -1.0f },
        {  32.0f,  32.0f,  0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f,  0.5f, 0.5f,  255, 255, 255, 255,  0.0f, -1.0f, 0.0f,  0.0f, 0.0f, -1.0f },
        {  32.0f,  32.0f, 64.0f,  1.0f, 0.0f, 0.0f,  0.0f, 0.0f,  0.5f, 0.5f,  255, 255, 255, 255,  0.0f, -1.0f, 0.0f,  0.0f, 0.0f, -1.0f },
        {  32.0f, -32.0f, 64.0f,  1.0f, 0.0f, 0.0f,  1.0f, 0.0f,  0.5f, 0.5f,  255, 255, 255, 255,  0.0f, -1.0f, 0.0f,  0.0f, 0.0f, -1.0f },

        // 2. Back face (-X, normal -1, 0, 0)
        { -32.0f,  32.0f,  0.0f, -1.0f, 0.0f, 0.0f,  1.0f, 1.0f,  0.5f, 0.5f,  255, 255, 255, 255,  0.0f,  1.0f, 0.0f,  0.0f, 0.0f, -1.0f },
        { -32.0f, -32.0f,  0.0f, -1.0f, 0.0f, 0.0f,  0.0f, 1.0f,  0.5f, 0.5f,  255, 255, 255, 255,  0.0f,  1.0f, 0.0f,  0.0f, 0.0f, -1.0f },
        { -32.0f, -32.0f, 64.0f, -1.0f, 0.0f, 0.0f,  0.0f, 0.0f,  0.5f, 0.5f,  255, 255, 255, 255,  0.0f,  1.0f, 0.0f,  0.0f, 0.0f, -1.0f },
        { -32.0f,  32.0f, 64.0f, -1.0f, 0.0f, 0.0f,  1.0f, 0.0f,  0.5f, 0.5f,  255, 255, 255, 255,  0.0f,  1.0f, 0.0f,  0.0f, 0.0f, -1.0f },

        // 3. Left face (+Y, normal 0, 1, 0)
        {  32.0f,  32.0f,  0.0f,  0.0f, 1.0f, 0.0f,  0.0f, 1.0f,  0.5f, 0.5f,  255, 255, 255, 255,  1.0f,  0.0f, 0.0f,  0.0f, 0.0f, -1.0f },
        { -32.0f,  32.0f,  0.0f,  0.0f, 1.0f, 0.0f,  1.0f, 1.0f,  0.5f, 0.5f,  255, 255, 255, 255,  1.0f,  0.0f, 0.0f,  0.0f, 0.0f, -1.0f },
        { -32.0f,  32.0f, 64.0f,  0.0f, 1.0f, 0.0f,  1.0f, 0.0f,  0.5f, 0.5f,  255, 255, 255, 255,  1.0f,  0.0f, 0.0f,  0.0f, 0.0f, -1.0f },
        {  32.0f,  32.0f, 64.0f,  0.0f, 1.0f, 0.0f,  0.0f, 0.0f,  0.5f, 0.5f,  255, 255, 255, 255,  1.0f,  0.0f, 0.0f,  0.0f, 0.0f, -1.0f },

        // 4. Right face (-Y, normal 0, -1, 0)
        { -32.0f, -32.0f,  0.0f,  0.0f,-1.0f, 0.0f,  0.0f, 1.0f,  0.5f, 0.5f,  255, 255, 255, 255, -1.0f,  0.0f, 0.0f,  0.0f, 0.0f, -1.0f },
        {  32.0f, -32.0f,  0.0f,  0.0f,-1.0f, 0.0f,  1.0f, 1.0f,  0.5f, 0.5f,  255, 255, 255, 255, -1.0f,  0.0f, 0.0f,  0.0f, 0.0f, -1.0f },
        {  32.0f, -32.0f, 64.0f,  0.0f,-1.0f, 0.0f,  1.0f, 0.0f,  0.5f, 0.5f,  255, 255, 255, 255, -1.0f,  0.0f, 0.0f,  0.0f, 0.0f, -1.0f },
        { -32.0f, -32.0f, 64.0f,  0.0f,-1.0f, 0.0f,  0.0f, 0.0f,  0.5f, 0.5f,  255, 255, 255, 255, -1.0f,  0.0f, 0.0f,  0.0f, 0.0f, -1.0f },

        // 5. Top face (+Z, normal 0, 0, 1)
        {  32.0f, -32.0f, 64.0f,  0.0f, 0.0f, 1.0f,  1.0f, 1.0f,  0.5f, 0.5f,  255, 255, 255, 255,  1.0f,  0.0f, 0.0f,  0.0f, -1.0f, 0.0f },
        {  32.0f,  32.0f, 64.0f,  0.0f, 0.0f, 1.0f,  0.0f, 1.0f,  0.5f, 0.5f,  255, 255, 255, 255,  1.0f,  0.0f, 0.0f,  0.0f, -1.0f, 0.0f },
        { -32.0f,  32.0f, 64.0f,  0.0f, 0.0f, 1.0f,  0.0f, 0.0f,  0.5f, 0.5f,  255, 255, 255, 255,  1.0f,  0.0f, 0.0f,  0.0f, -1.0f, 0.0f },
        { -32.0f, -32.0f, 64.0f,  0.0f, 0.0f, 1.0f,  1.0f, 0.0f,  0.5f, 0.5f,  255, 255, 255, 255,  1.0f,  0.0f, 0.0f,  0.0f, -1.0f, 0.0f },

        // 6. Bottom face (-Z, normal 0, 0, -1)
        { -32.0f, -32.0f,  0.0f,  0.0f, 0.0f,-1.0f,  1.0f, 1.0f,  0.5f, 0.5f,  255, 255, 255, 255,  1.0f,  0.0f, 0.0f,  0.0f,  1.0f, 0.0f },
        { -32.0f,  32.0f,  0.0f,  0.0f, 0.0f,-1.0f,  0.0f, 1.0f,  0.5f, 0.5f,  255, 255, 255, 255,  1.0f,  0.0f, 0.0f,  0.0f,  1.0f, 0.0f },
        {  32.0f,  32.0f,  0.0f,  0.0f, 0.0f,-1.0f,  0.0f, 0.0f,  0.5f, 0.5f,  255, 255, 255, 255,  1.0f,  0.0f, 0.0f,  0.0f,  1.0f, 0.0f },
        {  32.0f, -32.0f,  0.0f,  0.0f, 0.0f,-1.0f,  1.0f, 0.0f,  0.5f, 0.5f,  255, 255, 255, 255,  1.0f,  0.0f, 0.0f,  0.0f,  1.0f, 0.0f },
    };

    for (int i = 0; i < 24; ++i)
    {
        EmitVertex(mb, verts[i], bHasColor, bHasLightmapUV, bHasTangent);
    }

    mb.End();
}

void BuildDisplacement(IMesh* pMesh, bool bHasColor, bool bHasLightmapUV, bool bHasTangent)
{
    // Subdivided 16x16 grid (128x128 units) with vertex alpha transitioning smoothly
    // from 0.0 (left side: Y=+64) to 1.0 (right side: Y=-64), and color white (255, 255, 255).
    const int kGrid = 16;
    CMeshBuilder mb;
    mb.Begin(pMesh, MATERIAL_QUADS, kGrid * kGrid);

    for (int ix = 0; ix < kGrid; ++ix)
    {
        const float fracX0 = (float)ix / (float)kGrid;
        const float fracX1 = (float)(ix + 1) / (float)kGrid;
        const float x0 = 64.0f - fracX0 * 128.0f;
        const float x1 = 64.0f - fracX1 * 128.0f;
        const float v0 = 1.0f - fracX0;
        const float v1 = 1.0f - fracX1;

        for (int iy = 0; iy < kGrid; ++iy)
        {
            const float fracY0 = (float)iy / (float)kGrid;
            const float fracY1 = (float)(iy + 1) / (float)kGrid;
            const float y0 = 64.0f - fracY0 * 128.0f;
            const float y1 = 64.0f - fracY1 * 128.0f;
            const float u0 = fracY0;
            const float u1 = fracY1;

            const unsigned char a0 = (unsigned char)(u0 * 255.0f + 0.5f);
            const unsigned char a1 = (unsigned char)(u1 * 255.0f + 0.5f);

            const BrushVertex cellVerts[4] = {
                // v0: Near-Right (x0, y1)
                { x0, y1, 0.0f,  0.0f, 0.0f, 1.0f,  u1, v0,  0.5f, 0.5f,  255, 255, 255, a1,  1.0f, 0.0f, 0.0f,  0.0f, -1.0f, 0.0f },
                // v1: Near-Left (x0, y0)
                { x0, y0, 0.0f,  0.0f, 0.0f, 1.0f,  u0, v0,  0.5f, 0.5f,  255, 255, 255, a0,  1.0f, 0.0f, 0.0f,  0.0f, -1.0f, 0.0f },
                // v2: Far-Left (x1, y0)
                { x1, y0, 0.0f,  0.0f, 0.0f, 1.0f,  u0, v1,  0.5f, 0.5f,  255, 255, 255, a0,  1.0f, 0.0f, 0.0f,  0.0f, -1.0f, 0.0f },
                // v3: Far-Right (x1, y1)
                { x1, y1, 0.0f,  0.0f, 0.0f, 1.0f,  u1, v1,  0.5f, 0.5f,  255, 255, 255, a1,  1.0f, 0.0f, 0.0f,  0.0f, -1.0f, 0.0f },
            };

            for (int v = 0; v < 4; ++v)
            {
                EmitVertex(mb, cellVerts[v], bHasColor, bHasLightmapUV, bHasTangent);
            }
        }
    }

    mb.End();
}

} // namespace

void BrushRender_Draw(IMaterial* pMaterial)
{
    if (!pMaterial || !g_pMaterialSystem)
    {
        return;
    }

    if (s_currentBrushShape == BRUSH_SHAPE_NONE)
    {
        return;
    }

    CMatRenderContextPtr ctx(g_pMaterialSystem);
    if (!ctx)
    {
        return;
    }

    if (g_viewerSettings.renderMode == RM_WIREFRAME && g_materialWireframe)
    {
        pMaterial = g_materialWireframe;
    }

    ctx->Bind(pMaterial);

    // Check if material uses bump mapping
    bool bBumpFound = false;
    IMaterialVar* pBumpVar = pMaterial->FindVar("$bumpmap", &bBumpFound, false);
    if (!bBumpFound || !pBumpVar || !pBumpVar->IsDefined())
    {
        pBumpVar = pMaterial->FindVar("$bumpmap2", &bBumpFound, false);
    }

    if (bBumpFound && pBumpVar && pBumpVar->IsDefined())
    {
        ctx->BindLightmapPage(MATERIAL_SYSTEM_LIGHTMAP_PAGE_WHITE_BUMP);
    }
    else
    {
        ctx->BindLightmapPage(MATERIAL_SYSTEM_LIGHTMAP_PAGE_WHITE);
    }

    IMesh* pMesh = ctx->GetDynamicMesh(true, nullptr, nullptr, pMaterial);
    if (!pMesh)
    {
        return;
    }

    const VertexFormat_t fmt = pMaterial->GetVertexFormat();
    const bool bHasColor = (fmt & VERTEX_COLOR) != 0;
    const bool bHasLightmapUV = (TexCoordSize(1, fmt) > 0);
    const bool bHasTangent = (fmt & VERTEX_TANGENT_S) != 0 || (fmt & VERTEX_TANGENT_SPACE) != 0;

    switch (s_currentBrushShape)
    {
    case BRUSH_SHAPE_WALL:
        BuildWall(pMesh, bHasColor, bHasLightmapUV, bHasTangent);
        break;
    case BRUSH_SHAPE_FLOOR:
        BuildFloor(pMesh, bHasColor, bHasLightmapUV, bHasTangent);
        break;
    case BRUSH_SHAPE_CUBE:
        BuildCube(pMesh, bHasColor, bHasLightmapUV, bHasTangent);
        break;
    case BRUSH_SHAPE_DISPLACEMENT:
        BuildDisplacement(pMesh, bHasColor, bHasLightmapUV, bHasTangent);
        break;
    default:
        return;
    }

    pMesh->Draw();
}
