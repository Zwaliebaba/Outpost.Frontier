// Author: Giperion.

#pragma once

// The frame walk and the per-object submit (the routine, IRenderer2::Slot7) live in
// space_scene_obj_draw.h.

// Legacy foreign space_scene_load SpaceSceneDraw hook — no-op (proxies abolished).
void OnSpaceSceneObjectDraw(void* Renderable, unsigned char SceneKind, unsigned Layer, int SubLayer);

bool ShouldDrawSpaceSceneObject(void* Renderable, unsigned char SceneKind);

unsigned GetSceneDrawCullCount();
void ResetSceneDrawCullCountForFrame();
