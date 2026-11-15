// Author: Giperion.

#include "space_scene_draw.h"

namespace
{
    unsigned G_SceneDrawCullCount = 0;
}

void OnSpaceSceneObjectDraw(void* /*Renderable*/, unsigned char /*SceneKind*/, unsigned /*Layer*/,
                            int /*SubLayer*/)
{
    // Proxy path abolished. Live walk: space_scene_obj_draw.cpp (the routine…).
}

bool ShouldDrawSpaceSceneObject(void* /*Renderable*/, unsigned char /*SceneKind*/)
{
    return false;
}

unsigned GetSceneDrawCullCount()
{
    return G_SceneDrawCullCount;
}

void ResetSceneDrawCullCountForFrame()
{
    G_SceneDrawCullCount = 0;
}
