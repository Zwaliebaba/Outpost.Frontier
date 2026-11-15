// Author: Giperion.

#pragma once

// baseline WinObject (RTTI .?AUWinObject@@ : Win3D) -- a Win3D carrying one engine mesh
// instance. Base of WinButton -> HUD_Button (0x005d6fac) and UIWinObject (0x005e5d34).

// ctor the routine, dtor the routine (scalar deleting the routine).
// vftable 0x005d709c (28 slots): Win3D's with these overrides --
//   0 the routine, 5 the routine SetPosition, 10 the routine Destroy,
//   11 the routine Update, 13 the routine Draw.

// Layout:
//   +0x07c FClientMeshHolder (0x28): holder ctor the routine, +0x8c anim = -1,
//          +0x90..+0xa0 = 1.0f; the holder's vftable 0x005d2efc is not modelled
//   +0x0a4 flag byte: bit0 draw the tinted pass, bit1 skip the plain pass (and the lighting of
//          the tinted one); the ctor clears bits 0..2
//   +0x0a5 tint RGBA (the routine zeroes it)
//   size 0xac

#include "client_mesh_load.h"
#include "win3d.h"

#include <stddef.h>

class WinObject : public Win3D
{
public:

    WinObject();

    ~WinObject() override;

    // slot 5 the routine
    void SetPosition(float const* NewPosition) override;
    // slot 10 the routine
    void Destroy() override;
    // slot 11 the routine
    void Update() override;
    // slot 13 the routine
    void Draw() override;

    // (thiscall, RET 0xc) -- load MeshPath into the holder (the routine with
    // (MeshPath, MaterialPath, 0)); on success Create(Parent, 0) through slot 9 and return its
    // result, else false. HUD_Button's "object" key calls it with (0, path, 0).
    bool LoadMeshAndCreate(Win3D* Parent, char const* MeshPath, char const* MaterialPath);

    FClientMeshHolder MeshHolder;   // +0x07c
    unsigned char DrawFlags;        // +0x0a4
    unsigned char TintRgba[4];      // +0x0a5
    unsigned char PadA9[3];         // +0x0a9

private:
    WinObject(WinObject const&) = delete;
    WinObject& operator=(WinObject const&) = delete;
};

static_assert(sizeof(WinObject) == 0xac, "baseline WinObject is 0xac bytes");
static_assert(offsetof(WinObject, MeshHolder) == 0x7c, "WinObject +0x7c mesh holder");
static_assert(offsetof(WinObject, DrawFlags) == 0xa4, "WinObject +0xa4 draw flags");
static_assert(offsetof(WinObject, TintRgba) == 0xa5, "WinObject +0xa5 tint");
