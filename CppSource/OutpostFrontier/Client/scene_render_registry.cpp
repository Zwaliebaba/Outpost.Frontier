// Author: Giperion.

#include "scene_render_registry.h"

// InitSceneRenderRegistry (a port-only "registry ready" log latch with no caller and no baseline
// counterpart) was removed on 2026-09-30.

// RegisterSceneRenderable(void* Obj, void* Parent, bool bEnableDraw) was removed on 2026-09-25.
// It was a raw-offset duplicate of the baseline registration: UnregisterRenderable, clear bit 2,
// SetRenderableParent, RegisterRenderable, then flags. baseline has no such free function:
// (Win3D slot 9) = the routine(this, Parent), the routine(this),
//     flags = (flags & 0xfb) | 3  -> Win3D::Create in win3d.cpp;
// (RET 8; called from 004857af / 004b6d50, pointer at .rdata 005d0f10) = the same
//     two calls, flags &= 0xf8, byte +0x4e7 = 1.
// Neither baseline sequence unregisters first; the leading UnregisterRenderable was ours. Its only
// caller (menu_universe_base.cpp, TryRegisterRoomEngineInstance) registered objects that are
// not Win3D, so they now stay out of the Win3D registry (see ResetMenuRoomSceneObjects).
