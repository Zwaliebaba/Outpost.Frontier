// Author: Giperion.

#pragma once

#include <cstddef>
#include "client_cliobj.h"

struct IEngine;

// Client wrapper: Archetype::Load then optional IEngine+0x5C CreateInstance.
// OutHandle null + OutInstance set -> the instance keeps the archetype handle (cockpit).
// A port helper, defined in client_mesh_load.cpp (formerly an invented Common.dll export).
bool ArchetypeLoadFromPath(IEngine* Engine, char const* RelPath, void** OutHandle,
                           void** OutInstance);

// -------------------------------------------------------------------------------------------
// Client mesh holder + .3db load — baseline the routine / the routine / the routine.

// HUD panels (Steer, Targetable, alerts, WinControl icons) embed a 0x28-byte holder and call
// (path, material path or 0, 1).

// Holder layout (Steer Create 0x004c851c + Targetable ctor stride 0x28):
//   +0x00  vtable (the routine after HUD init; the routine writes the branch)
//   +0x04  IEngine instance handle (-1 empty) — the routine CreateInstance result
//   +0x08  material-library cache entry (the routine via the routine)
//   +0x0c  auxiliary material-library cache entry
//   +0x10  anim / archetype side index (-1); Archetype::Load may write anim out here
//   +0x14..+0x24  five floats (identity 1.0) — CompoundInstanceAnalyze context
//                 (callback the routine → IEngine+0x7c SetInstanceUserData)

// (thiscall, RET 0xC = path, material path, bRegisterCache):
//   1. NEURONCOMPONENTS_CRC::GetCRC32(path)
//   2. IEngine+0x44 FindArchetype — hit → the routine + CompoundInstanceAnalyze +
//      IEngine+0x3c ReleaseHandle, return the routine
//   3. miss → NeuronRuntime::Data+0xc open FileSystem, the routine(path, material path),
//      Archetype::Load, optional the field cache (bRegisterCache!=0), the routine +
//      analyze; ReleaseHandle when cache path not taken
//   Lifetime: holder owns the instance (+0x04). Release = the routine / the routine
//   (DestroyInstance + clear). FS AddRef from Data Create is Release'd at end of load.

// (thiscall, RET 4 = out AABB*): if +0x04 valid → GetInstanceArchetype(+0x64),
//   IRenderer2+0x10 GetExtents(arch, 1.0, out6), ReleaseHandle(arch). Out = 6 floats
//   (max/min XYZ per RendComp Slot13); Targetable stores maxX, (maxX-minX), …
// -------------------------------------------------------------------------------------------

struct IEngine;

// baseline `ObjectHandle` (RTTI .?AUObjectHandle@@, COL 0x005e8300): the object the routine
// constructs, exactly 0x10 bytes (vtable the branch, instance -1, two material-cache
// slots). Its only virtual is the scalar deleting destructor the branch; the destructor
// just resets the vptr -- it does NOT release the instance, so owners release
// explicitly (the star sphere the routine, the Ship's +0x108). The first-person camera embeds
// one bare at +0x190 (ctor call 0x005171a7, next field at +0x1a0); Object3D below derives
// from it.
struct FMeshHolder
{
    FMeshHolder() = default;
    virtual ~FMeshHolder(); // ; scalar deleting wrapper the branch
    void* EngineInstance = reinterpret_cast<void*>(static_cast<ptrdiff_t>(-1)); // +0x04
    Archetype::FCliMaterialEntry* MaterialEntry = nullptr;    // +0x08
    Archetype::FCliMaterialEntry* AuxMaterialEntry = nullptr; // +0x0c
};

static_assert(sizeof(FMeshHolder) == 0x10, "module state object is 0x10");
static_assert(offsetof(FMeshHolder, EngineInstance) == 0x4, "holder +0x4 instance");

// baseline `Object3D : ObjectHandle` (RTTI .?AUObject3D@@, COL 0x005eb5e8), 0x28 bytes, vftable
// whose one slot is the scalar deleting destructor the routine. The constructor is
// always inlined: the routine, the routine on +0x14 (five floats 1.0f), the Object3D vptr,
// +0x10 = -1 (e.g. 0x0059f8dd..0x0059f8fc in WinControl, 0x00491fdc..0x00491ff7 in the NavMap
// item). The destructor the routine is the routine's body -- close the animation script,
// release a bound instance -- then ~ObjectHandle; it runs as the member dtor of every owner
// (21 call sites, most of them EH unwind funclets), so the port holds it as a real destructor.
// Copying is deleted: two holders would release one instance twice.
struct FClientMeshHolder : FMeshHolder
{
    FClientMeshHolder() = default;
    ~FClientMeshHolder() override;
    FClientMeshHolder(FClientMeshHolder const&) = delete;
    FClientMeshHolder& operator=(FClientMeshHolder const&) = delete;

    long AnimOrArchIndex = -1;                            // +0x10
    float UserFloats[5] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f}; // +0x14..+0x24 (the routine)
};

static_assert(sizeof(FClientMeshHolder) == 0x28, "baseline HUD mesh holder 0x28");
static_assert(offsetof(FClientMeshHolder, AnimOrArchIndex) == 0x10, "holder +0x10 anim");
static_assert(offsetof(FClientMeshHolder, UserFloats) == 0x14, "holder +0x14 analyze ctx");

// itself: the four stores and nothing else.
void MeshHolderConstruct(FMeshHolder* Holder);

// IRenderer2 Slot4 / the routine out — 6 floats (AABB max/min XYZ).
struct FClientMeshAabb
{
    float Values[6] = {};
};

// — construct empty holder (vtable pad, instance -1, paths null).
void ClientMeshHolderConstruct(FClientMeshHolder* Holder);

// — instance bound?
bool ClientMeshHolderHasInstance(FMeshHolder const* Holder);

// — destroy the instance and release both material-cache slots.
void ClientMeshHolderRelease(FMeshHolder* Holder);

// — close the animation script, then release a bound mesh holder.
void ClientUiMeshHolderRelease(FClientMeshHolder* Holder);

// (thiscall on the holder, RET 8) - open Path as a NeuronRuntime::Data FileSystem,
// resolve the material-cache entries for Path/AuxPath into +0x08/+0x0c, Archetype::Load the
// FileSystem and, on success, replace the bound instance with CreateInstance(handle, 0).
// Returns Archetype::Load's result. Caller: the star sphere layer load the routine.
bool ClientMeshHolderLoad(FMeshHolder* Holder, char const* Path, char const* AuxPath);

// (thiscall on the holder, RET 8) - a non-null Path resolves its material-cache
// entry into +0x08 (the routine(Path, 1, -1)), a non-null AuxPath into +0x0c. What was there
// before is not released. Caller: the routine (the engine-character accessory material).
void ClientMeshHolderSetMaterials(FMeshHolder* Holder, char const* Path, char const* AuxPath);

// (thiscall on the holder, RET 4) - a bound instance (+0x04 != -1) is destroyed
// (IEngine slot 24), +0x04 set to -1 and BOTH material-cache slots released (the routine);
// then +0x04 = IEngine slot 23 CreateInstance(ArchHandle, 0). True when that is not -1.
// Callers: FLFxMeshFactory 0x004f68d5, the routine (0x0059ecd0 / 0x0059ee35 / 0x0059ee78).
bool ClientMeshHolderBindInstance(FMeshHolder* Holder, void* ArchHandle);

// (thiscall on the holder) - NeuronRuntime::Engine vt+0xa0
// GetInstanceTransform(Holder+0x04).
float* ClientMeshHolderGetTransform(FMeshHolder const* Holder);

// (thiscall on the holder, RET 4) - the instance's bounding sphere in world space:
// IEngine slot 34 GetInstanceExtent(Holder+0x04, 0, &Radius, OutCenter) with Radius zeroed
// first, then the centre through the instance transform (slot 40) by
// I3DMathEngine::TransformXform (+0x28) into a local that is copied over OutCenter. The math
// engine is cached in the field (the routine on first use). Returns the radius. Caller:
// the first-person cockpit pass 0x00518522.
float ClientMeshHolderGetBoundingSphere(FMeshHolder const* Holder, float* OutCenter);

// (thiscall on the holder, RET 4) - NeuronRuntime::Engine vt+0x9c
// SetInstanceMatrix(Holder+0x04, Transform).
void ClientMeshHolderSetMatrix(FMeshHolder* Holder, void* Transform);

// (thiscall on the holder, RET 8) - IRenderer2 slot 7
// (Camera, Holder+0x04, 1.0f, Flags, 0, 0).
void ClientMeshHolderRender(FMeshHolder* Holder, void* Camera, unsigned long Flags);

// (thiscall on the holder, RET 4) - NeuronRuntime::Engine vt+0x8c
// SetInstancePosition(Holder+0x04, Position).
void ClientMeshHolderSetPosition(FMeshHolder* Holder, float const Position[3]);

// (thiscall on the holder) - NeuronRuntime::Engine vt+0x90 GetInstancePosition(Holder+0x04).
float* ClientMeshHolderGetPosition(FMeshHolder const* Holder);

// (thiscall on the holder) - NeuronRuntime::Engine vt+0x98
// GetInstanceOrientation(Holder+0x04): nine floats, row-major.
float* ClientMeshHolderGetOrientation(FMeshHolder const* Holder);

// (thiscall on the holder, RET 4) - NeuronRuntime::Engine vt+0x94
// SetInstanceOrientation(Holder+0x04, Orientation).
void ClientMeshHolderSetOrientation(FMeshHolder* Holder, float const Orientation[9]);

// (thiscall on the holder) - NeuronRuntime::Engine vt+0x6c
// UpdateInstance(Holder+0x04, 0, 0).
void ClientMeshHolderUpdate(FMeshHolder* Holder);

// (thiscall on the holder, RET 4) - the RGBA byte quad at Rgba scaled by 1/255
// into the tint floats +0x14 / +0x18 / +0x1c and +0x24 (UserFloats[0..2], [4]); +0x20 is left
// alone. Callers: HUD_DamageIndicator::Draw (0x004d672b / 0x004d6853 / 0x004d692d /
// ) and the nav-map colour switch 0x004a1a65 (not ported).
void ClientMeshHolderSetColor(FClientMeshHolder* Holder, unsigned char const* Rgba);

// (thiscall on the holder, RET 0xc) -- bind RelPath's archetype to Holder.
// FindArchetype hit: the routine + CompoundInstanceAnalyze, then the handle is released.
// Miss: RelPath is opened as a NeuronRuntime::Data FileSystem, the routine(Holder, RelPath,
// MaterialPath) resolves both material-cache entries (MaterialPath may be null), then
// Archetype::Load; with bRegisterCache the handle is kept in the Win3D model cache
// (the field) instead of being released. Returns the routine (instance bound).
bool ClientMeshLoadFromPath(FClientMeshHolder* Holder, char const* RelPath,
                            char const* MaterialPath, bool bRegisterCache);

// The Win3D model cache the field (a vector, element stride 0xc; the routine builds
// {handle, 0, 0}). It keeps an archetype handle -- and for a preloaded model its two
// material-cache entries -- alive until the Win3D manager shuts down.
struct FWin3DModelCacheEntry
{
    void* ArchHandle;                                   // +0x00
    Archetype::FCliMaterialEntry* MaterialEntry;        // +0x04
    Archetype::FCliMaterialEntry* AuxMaterialEntry;     // +0x08
};

// (thiscall on the field, RET 8) -- preload Path into the model cache.
// Caller: HUD_ContactList::Create (0x004cf6bb, the 16 icon meshes, AuxPath 0).
bool Win3DModelCachePreload(char const* Path, char const* AuxPath);

// (thiscall on the field) -- release every cached handle and both
// material-cache slots of each entry, then empty the vector. Caller: the Win3D manager
// shutdown the routine (0x0059d889).
void Win3DModelCacheClear();

// (thiscall on the holder, RET 4) — false when the instance is -1, else
// GetInstanceArchetype + IRenderer2 Slot4 GetExtents into OutAabb + ReleaseHandle, and true
// whatever Slot4 answered. OutAabb is not cleared on the false path.
bool ClientMeshQueryAabb(FClientMeshHolder* Holder, FClientMeshAabb* OutAabb);
