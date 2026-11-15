// Author: Giperion.

#pragma once

#include "common_goods.h"
#include "x86math.h"

#include <cstddef>

// Shared Effect head — Client/space/Effect.cpp (the routine).
// BeamEffect / ParticleEffect / CliEquipLight share this through +0x68.

struct FEffectArch;
struct FVisEffectArch;
struct FSpaceCamera;

struct FEffectBase
{
    explicit FEffectBase(unsigned TypeId);
    void AddRef();
    int Release();
    void Attach(void* WatchableOwner, int EngineIndex, FTransform const* LocalXform);
    virtual unsigned char Start(FEffectArch* Arch);
    virtual void Kill();
    virtual float GetWorldPos(FVector* Out) = 0;
    virtual void SetIntensity(float Intensity);
    virtual unsigned char IsAlive() = 0;
    virtual unsigned char UpdateVisibility(float Dt, FSpaceCamera* Cam) = 0;
    virtual unsigned char TickDraw(float Dt, FSpaceCamera* Cam) = 0;
    virtual unsigned char BuildGeometry(FSpaceCamera* Cam) = 0;
    virtual ~FEffectBase();
    static void operator delete(void* Memory, size_t Size);

    union
    {
        float Pad04;               // +0x04
        FVisEffectArch* Vis;
    };
    unsigned char Flags08;         // +0x08  bit0 live (light), bit1 deactivated
    unsigned char Pad09[3];
    float Priority0C;              // +0x0C
    FTransform LocalXform;         // +0x10 … +0x3F
    int EngineIndex;               // +0x40  = -1
    BaseWatcher Watcher;           // +0x44 … +0x4B
    unsigned char Pad4C[0x0C];     // +0x4C … +0x57
    unsigned RefCount;             // +0x58  the routine / the routine
    unsigned EffectTypeId;         // +0x5C
    void* LinkPrev;                // +0x60
    void* LinkNext;                // +0x64
    void* LinkSelf;                // +0x68
};
static_assert(sizeof(FEffectBase) == 0x6c, "EffectBase through +0x68");
static_assert(offsetof(FEffectBase, EngineIndex) == 0x40, "EngineIndex");
static_assert(offsetof(FEffectBase, RefCount) == 0x58, "RefCount");
static_assert(offsetof(FEffectBase, EffectTypeId) == 0x5c, "EffectTypeId");

// — size==0x84 → Heap::Malloc on the pool the field; else malloc.
void* EffectAlloc(unsigned Size);

// — matching free.
void EffectFree(void* Ptr, unsigned Size);

// — client teardown (0x005b228b): kill and release every live effect
// (EffectTypeListKillAll), clear G_EffectShapeDB, Shrink both effect Heaps (the field,
// ). The the field release step has nothing to release in the port.
void EffectSystemShutdown();

// — zero/init Effect head; TypeId → +0x5c; RefCount=1.
void EffectBaseConstruct(FEffectBase* This, unsigned TypeId);

// (thiscall) — RefCount (+0x58) + 1. No null test.
void EffectAddRef(FEffectBase* This);
// (thiscall) — RefCount - 1; at or below zero the effect deletes itself through its
// own vtable slot +0x20 with 1 (the scalar deleting destructor of the concrete class). Returns
// the new count. No null test.
int EffectRelease(FEffectBase* This);

// (thiscall, RET 4) — slot 0 ("start") of the Effect base table the branch,
// inherited unchanged by the CliEquipLight table the branch. With a non-null Arch: a type
// id at or below zero (+0x5c, signed JG) is taken from the arch's first dword, and when
// gives a row its Priority (+4) lands in +0x0c. Returns 1 either way.
unsigned char EffectBaseStart(FEffectBase* This, FEffectArch* Arch);

// Compatibility entry point for FEffectBase's destructor cleanup; the C++ destructor
// supplies the base vptr transition and member destruction.
void EffectBaseDestruct(FEffectBase* This);

// — attach Watchable(+8) + EngineIndex + local xform (shared).
void EffectAttach(FEffectBase* This, void* WatchableOwner, int EngineIndex,
                  FTransform const* LocalXform);

// The object the effect's watcher holds (+0x44 minus 8), or null. baseline spells this inline
// everywhere: `[+0x44] == 0 → null; [+0x44] - 8 == 0 → null`.
void* EffectWatchedObject(FEffectBase const* This);

// (__thiscall, no args) — detach the effect from its engine instance and keep it
// where it is. Nothing when EngineIndex is -1. When the watcher is empty: link into the type
// bucket if needed (the routine inlined), call ITSELF (baseline 0x004f7e17 -- with the index still
// set and the watcher still empty that call recurses again; ported as written), then Flags08
// |= 2. Otherwise: +0x4c = CObject::get_velocity() of the instance's user data (IEngine +0x80,
// CObject vt+0x54), or zero; +0x10 = GetInstanceTransform(index) × local through
// I3DMathEngine::MulTransform (the field); EngineIndex = -1.
void EffectFreezeToWorld(FEffectBase* This);

// — Effect's own vtable slot +4 (kill / stop), shared by the base table
// , 0x005d9d84 and 0x005da3f8. Link if needed, then the the routine body inlined
// (except that the empty-watcher branch calls the routine itself), then Flags08 |= 2.
void EffectKill(FEffectBase* This);

// (__thiscall, RET 4; callers 0x0050c484 and the tractor beam's slot +0x14
// at 0x005133f5) — the effect's own slot +8 (world position into a local, radius
// on the FPU), the camera's view transform (slot +0x40) applied through I3DMathEngine +0x28
// (the field), then the camera's ClassifySphere (slot +0x3c) with the view-space centre and
// the radius: true when the class is 3 or more (straddling or inside), SETGE on a signed int.
struct FSpaceCamera;
unsigned char EffectInCameraView(FEffectBase* This, FSpaceCamera* Cam);

// (__thiscall, RET 4; callers 0x004fdccb, 0x004fddac, 0x004fe04b, 0x0050c398) —
// the world transform (instance × local through
// when watched and attached, else the local transform) copied into *Out.
void EffectGetWorldTransform(FEffectBase const* This, FTransform* Out);
