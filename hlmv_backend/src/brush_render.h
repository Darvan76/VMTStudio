#pragma once

class IMaterial;

enum BrushShape
{
    BRUSH_SHAPE_NONE = 0,
    BRUSH_SHAPE_WALL,
    BRUSH_SHAPE_FLOOR,
    BRUSH_SHAPE_CUBE,
    BRUSH_SHAPE_DISPLACEMENT
};

void BrushRender_SetShape(BrushShape shape);
BrushShape BrushRender_GetShape();
bool BrushRender_IsBrushShape();
void BrushRender_Draw(IMaterial* pMaterial);
