// Author: Giperion.

#pragma once

#include "common_cliobj.h"

// Client ICliObj companions + factory (baseline exe the field / the routine).
// Every baseline Archetype::Cli* class (CliSolar, CliShip, CliAsteroid, CliDynamicAsteroid,
// CliEQEngine/Shield/ShieldGenerator/Thruster/Launcher/Gun/Light/Tractor/AttachedFX/
// InternalFX/RepairDroid/TradeLane, CliProj, CliMunition) has its own typed class in
// client_cliobj.cpp; the type -> class table is documented on FCliObjFactory::Create.
// Types with no class of their own get the bare CliRoot (0x54, the routine).
// LOD list at +0x04 (the routine). See implementation notes.

// All of this is OutpostFrontier.exe code in baseline; Common.dll only sees ICliObjFactory
// (src/include/common_cliobj.h). Nothing here is exported from any DLL.

struct IFileSystem;
struct FEffectArch; // vis_effect_arch.h
struct EquipDescVector; // common_goods.h

namespace Archetype
{
    // MaterialLib path-cache entry (Client\cmn\MaterialLib.cpp); layout in client_cliobj.cpp.
    struct FCliMaterialEntry;

    // VC6 std::vector empty-allocator shape: 1-byte cookie + 3 pad + begin/end/cap. The
    // companions' material-path list (ICliObj +0x20) and the list Gf::ObjectProp builds from a
    // PetalDB record (the routine) both have this shape and both reach the routine.
    template <typename T>
    struct FCliVc6Vector
    {
        unsigned char Cookie;
        unsigned char Pad01[3];
        T* Begin;
        T* End;
        T* Capacity;
    };

    static_assert(sizeof(FCliVc6Vector<void*>) == 0x10, "VC6 vector header 0x10");

    // MaterialLib path pair (stride 8). First dword is char* path into the routine /
    // MaterialLib::Load — not a CreateID hash (RE doc correction). Arg is the routine's third
    // argument (-1 from the routine).
    struct FCliMaterialPathPair
    {
        char const* Path;
        unsigned Arg;
    };

    static_assert(sizeof(FCliMaterialPathPair) == 8, "material path pair stride 8");

    // insert-at-end for a material path list (stride 8).
    void PushMaterialPathPair(FCliVc6Vector<FCliMaterialPathPair>* Vec, char const* Path,
                              unsigned Arg);

    // (cdecl) — find-or-load the MaterialLib cache entry for Path, ++RefCount.
    // bForceApply = baseline arg 2, Arg = baseline arg 3 (-1 from the routine / the routine).
    FCliMaterialEntry* ResolveMaterialEntry(char const* Path, bool bForceApply, unsigned Arg);

    // (cdecl) — the same cache lookup for a file system the caller already
    // opened (the routine passes the .cmp's own): find-or-insert Path, and for a new entry
    // load its textures, materials, material animations and meshes from Filesys.
    FCliMaterialEntry* ResolveMaterialEntryFromFs(IFileSystem* Filesys, char const* Path,
                                                  bool bForceApply, unsigned Arg);

    // (cdecl) — resolve at most Max entries of Paths through the routine (with
    // bForceApply) into Out, FDUMP an error first when Paths holds more than Max. Returns the
    // number written; 0 for a null Paths.
    int ResolveMaterialEntryList(FCliVc6Vector<FCliMaterialPathPair> const* Paths,
                                 FCliMaterialEntry** Out, int Max);

    // — DecRef a cache entry through its slot; frees libs + erases at 0.
    int ReleaseMaterialEntrySlot(FCliMaterialEntry** Slot);

    // (cdecl) — client shutdown (0x005b22f6): give every cached entry's texture,
    // material, material-animation and mesh handles back to their libraries regardless of the
    // reference counts, empty the cache, return how many entries there were.
    int FlushMaterialCache();

    // The companion (ICliObj) fields and methods other client code reads by raw offset in
    // baseline: +0x10 the envmap material name (CacheString), +0x20 the material path list, and
    // , the explicit-parameter LOD recompute.
    char const* CliObjEnvmapMaterial(ICliObj* Companion);
    FCliVc6Vector<FCliMaterialPathPair> const* CliObjMaterialPaths(ICliObj* Companion);
    // A solar archetype's companion (CliSolar) +0x60: its shape_name text. Companion must be a
    // CliSolar (baseline reads the offset without a type test).
    char const* CliSolarShapeName(ICliObj* Companion);
    void CliObjRecomputeLodPickExplicit(ICliObj* Companion, long EngineIndex,
                                        float const* BlendSource, bool bSolarOrShip);
    // recompute the companion's LodPick from its bound archetype (kept as is
    // while that archetype is not loaded). Thiscall on the companion in baseline.
    void CliObjRecomputeLodPick(ICliObj* Companion, float const* BlendSource);
    // (cdecl) -- store the two ArchLod globals: the field (byte) and
    // (float, the LOD distance scale). Caller: the level_of_detail apply
    // .
    void CliObjSetArchLodGlobals(unsigned char bMute, float Scale);
    // (cdecl) -- *OutScale = the field when OutScale is not null; returns the
    // byte the field. Caller: the gameobj draw submit the routine (0x005399f9).
    unsigned char CliObjGetArchLodGlobals(float* OutScale);

    class FCliObjFactory final : public ICliObjFactory
    {
    public:
        void Create(int TypeId, ICliObj** Out) override;
    };

    // — the factory WinMain hands to DataConfig::startup.
    extern FCliObjFactory GCliObjFactory;

    // — non-virtual thunk into the routine (build the companion's material
    // cache; baseline's zero applies now, nonzero defers application). Caller: the routine.
    bool CliObjPreloadMaterials(ICliObj* Companion, bool bDeferApply);

    // a TradeLane companion's (CliEQTradeLane) effect reference: the first one
    // resolves its five player-or-ship EffectArch pointers and AddRefs them. Caller: the
    // TradeLane equipment FX constructor the routine (0x00516cb0).
    void CliTradeLaneAddEffectRef(ICliObj* Companion);
    // the matching release; the last one releases and clears the five.
    // Returns the remaining count. Caller: the TradeLane equipment FX dtor the routine.
    int CliTradeLaneReleaseEffectRef(ICliObj* Companion);

    // (thiscall on the companion, RET 4) -- CliRoot's distance-record pick: a
    // random live effect of the record the distance falls into, or null. Callers: GameObject
    // slot 104 (0x00539d50) and slot 105 (0x00539dd3).
    FEffectArch* CliObjPickDistanceRecord(ICliObj* Companion, float Distance);
    // (thiscall on the companion, RET 4) -- the same pick over a
    // CliEQShieldGenerator's shield_hit_effects. Callers 0x0052e81a / 0x0052e96a.
    FEffectArch* CliShieldGeneratorPickHitEffect(ICliObj* Companion, float Distance);
    // (thiscall on the companion) -- a random entry of a CliMunition's live
    // effects, or null. Callers 0x0052ea06 / 0x00539e2e.
    FEffectArch* CliMunitionPickLiveEffect(ICliObj* Companion);
    // A CliShip companion's bay door sound ids: +0x58 bay_doors_open_snd when bOpen, else +0x5c
    // bay_doors_close_snd. Reader: the ship notify the routine case 2.
    ID_String const* CliShipBayDoorSound(ICliObj* Companion, bool bOpen);
    // A CliSolar companion's +0x54 open_sound when bOpen, else +0x58 close_sound. Reader: the
    // solar notify the routine codes 0 and 1.
    ID_String const* CliSolarDoorSound(ICliObj* Companion, bool bOpen);
    // A CliEQEngine companion's +0x9c flame_effect, +0x60 rumble_sound and +0x78
    // character_loop_sound. Companion must be a CliEQEngine (baseline reads the offsets after an
    // Engine class test on the archetype). Readers: Gf ObjectShip the routine (0x00446cfd) and
    // (0x0044804a / 0x0044804d).
    ID_String const* CliEngineFlameEffect(ICliObj* Companion);
    ID_String const* CliEngineRumbleSound(ICliObj* Companion);
    ID_String const* CliEngineCharacterLoopSound(ICliObj* Companion);

    // A CliProj companion's (projectiles: CliMunition and the rest) +0x58 const_effect and
    // +0x54 const_effect_delay. Readers: Projectile slot 92 the routine (0x0054270a /
    // ) and slot 93 the routine.
    ID_String const* CliProjConstEffect(ICliObj* Companion);
    float CliProjConstEffectDelay(ICliObj* Companion);
    // A CliMunition companion's +0x60 HP_trail_parent (CacheString text; "" when unset, never
    // null after the ctor's clear). Reader: Guided slot 112 the branch.
    char const* CliMunitionHpTrailParent(ICliObj* Companion);
    // A CliDynamicAsteroid companion's +0x58 flag and +0x54 particle_effect. Reader: the
    // dynamic asteroid update the routine (0x00526089 / 0x005260c2), no class test.
    bool CliDynamicAsteroidHasParticleEffect(ICliObj* Companion);
    ID_String const* CliDynamicAsteroidParticleEffect(ICliObj* Companion);

    // (cdecl) -- Load(LoadMode) the companion (Root +0x04, vt+0x20) of every
    // mounted, non-internal equipment in the vector whose archetype resolves. Callers: the
    // player ship build the routine (0x0054b7ee, LoadMode 1), 0x0053b073, 0x0053b6f1,
    // .
    void LoadMountedEquipmentCompanions(EquipDescVector const* Items, int LoadMode);
    // (cdecl) -- hook up the materials (IMatHookUp slot 3) of an engine instance
    // and of every instance IEngine::GetRelatedInstance(Instance, 1, ...) walks to. Callers:
    // (0x0054b7fe) and eight Gameobj/equipment paths.
    void HookUpInstanceTreeMaterials(long EngineIndex);
}
