// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

#include "space_scene_draw.h"

namespace
{
    unsigned G_SceneDrawCullCount = 0;
}

void OnSpaceSceneObjectDraw(void* /*Renderable*/, unsigned char /*SceneKind*/, unsigned /*Layer*/,
                            int /*SubLayer*/)
{
    // Proxy path abolished. Live walk: space_scene_obj_draw.cpp (FUN_0051b250…).
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
