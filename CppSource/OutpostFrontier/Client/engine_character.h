// Author: Giperion.

#pragma once

// EngineCharacter -- baseline `cmn\EngineCharacter.h` (the FDUMP file name at .rdata 0x005d7188),
// OutpostFrontier.exe 0x004c98a0-0x004ca610 plus the template helpers 0x004ceb30-0x004cec80.

// A character "body" is one skeletal deformable (NeuronRuntime::Deform) built from a Costume's four
// bodyparts, with up to four accessory meshes hung on its hardpoints. HUD_Comm's talking head
// (hud_comm.cpp) is the caller that plays animations on it; UIWin (0x0059aea2 / 0x0059b048 /
// / 0x0059b47f) uses the same cache for the NeuroNet character windows.

// baseline keeps built bodies in one global list the field (ctor the branch ->
// , atexit the branch -> the routine). A node is 0x44 bytes: next, prev, the
// Costume the body was built from (+0x08, 0x34 bytes), the body (+0x3c) and a reference count
// (+0x40). FindOrCreate (the routine) returns the body of a node whose Costume compares equal
// (Costume_is_equal) after bumping its count, else builds a new body and appends a node with
// count 1 -- also when building failed, in which case the node holds a null body. Nothing in
// the PE ever decrements the count: bodies live until HUD_Receiver's destructor empties the
// list (0x004ce99d..0x004ce9d1) or the atexit destructor runs.

// The port keeps the list as std::list<FEngineCharacterCacheNode> (no baseline ABI).

#include "client_cliobj.h"
#include "client_mesh_load.h"
#include "common_reputation.h"
#include "common_tstring.h"
// FDeformPartDesc / FDeformLodDesc (IDeformable::Create's table) come from engine/sceneruntime.h,
// which must be reached through NeuronRuntime.h (it declares IEngine2 before sceneruntime.h does).
#include "NeuronRuntime.h"
#include "x86math.h"

#include <list>

struct IFileSystem;

// ------------------------------------------------------------------------------------------
// The part description handed to IDeformable::CreateAlias (the routine). Built on the stack
// by the routine (0x5c8 bytes). {Count, Entries} is the Parts table Deformable2's
// reads: each entry an FDeformPartDesc with one FDeformLodDesc (the part path's
// Source) -- LodCount 1, Path null, so the part is opened as Source.Name through
// Source.FileSystem, and Flags 1 stops after LOD 0. Source.Fraction comes back written.
// ------------------------------------------------------------------------------------------

// One part path (0x94 bytes, at desc +0x128 + i * 0x94).
struct FEngineCharacterPartPath
{
    TString<0x80> Path;          // +0x00 length, +0x04 chars (<= 0x7f)
    FDeformLodDesc Source;       // +0x84 {*NeuronRuntime::Data, Path's chars, Fraction}
    int TypeId;                  // +0x90 the bodypart kind (0..3)
};

struct FEngineCharacterBodyDesc
{
    // (thiscall, RET 4) -- the eight entries {0,0,0,0,0,-1} and eight empty paths,
    // Count = 0, Entries = &Entry[0]; then the Costume's four bodyparts in the order
    // PartIds[1] (type 0, inserted at the front), PartIds[0] (type 1), PartIds[2] (type 2, through
    // ) and PartIds[3] (type 3). A zero id is skipped; an id the costume table does
    // not know still takes a slot, with an empty path.
    explicit FEngineCharacterBodyDesc(Costume const& Source);

    // (thiscall, RET 8) -- add a part; with TypeId 0 and parts already present it
    // goes in front (the the routine shift, inlined). Full (8) -> nothing.
    void AddPart(int TypeId, char const* Path);
    // (thiscall, RET 0xc) -- fill slot Index: the path (<= 0x7f chars, a null path
    // stores ""), *NeuronRuntime::Data and the path pointer in the pair, entry {1, &pair, 0, 0, 1}, the
    // type. Count is the caller's business.
    void SetPart(int Index, int TypeId, char const* Path);
    // (thiscall) -- move slots 0..6 up by one (7 is lost), re-pointing each moved
    // entry at its new pair and each moved pair at its new path.
    void ShiftPartsUp();
    // (thiscall) -- 0 without parts; else a material-library reference per part
    // (the routine(PathText, 1, -1)), IDeformable::CreateAlias (slot 5) with this desc, an
    // FDUMP (line 0xd5) on failure, and the references released (the routine).
    void* CreateDeformable();

    int Count;                                   // +0x00
    FDeformPartDesc* Entries;                    // +0x04 = &Entry[0]
    unsigned char Unknown08[0x60];               // +0x08 never written
    FDeformPartDesc Entry[8];                    // +0x68
    FEngineCharacterPartPath Part[8];            // +0x128

private:
    FEngineCharacterBodyDesc(FEngineCharacterBodyDesc const&) = delete;
    FEngineCharacterBodyDesc& operator=(FEngineCharacterBodyDesc const&) = delete;
};

// ------------------------------------------------------------------------------------------
// 's stack record (0x11c bytes): what the routine needs to hang one accessory.
// ------------------------------------------------------------------------------------------
struct FEngineCharacterAccessoryLink
{
    // three empty strings and an empty material-path vector.
    FEngineCharacterAccessoryLink();
    // The vector's buffer freed (the routine on +0x10c at 0x004c9d16; the routine is the same
    // body for the unwind funclets 0x005bd97d / 0x005c4cf6).
    ~FEngineCharacterAccessoryLink();

    // (thiscall, RET 4) -- MiscUtilLoadEngineInstance(Mesh, *NeuronRuntime::Data, 0, 0,
    // &MaterialPaths, 0, 0), FindHardpoint(Bone, CharacterHardpoint), then IHardpoint slot 6
    // ConnectHardpoints(hardpoint, {CharacterHardpoint, crc}, instance, {AccessoryHardpoint,
    // crc}); either failure FDUMPs (line 0x4e). Returns the instance whatever happened.
    void* Connect(long Bone);

    TString<0x80> Mesh;                                                     // +0x000
    TString<0x40> AccessoryHardpoint;                                       // +0x084
    TString<0x40> CharacterHardpoint;                                       // +0x0c8
    Archetype::FCliVc6Vector<Archetype::FCliMaterialPathPair> MaterialPaths; // +0x10c

private:
    FEngineCharacterAccessoryLink(FEngineCharacterAccessoryLink const&) = delete;
    FEngineCharacterAccessoryLink& operator=(FEngineCharacterAccessoryLink const&) = delete;
};

// ------------------------------------------------------------------------------------------
// The body (0x298 bytes, operator new at 0x004c9b29).
// ------------------------------------------------------------------------------------------
struct FEngineCharacterBody
{
    // (inlined in FindOrCreate): four mesh holders (the routine), four
    // empty animation names, the transform identity/zero, no deformable, four free animation
    // slots (-1), animation rate 1.0.
    FEngineCharacterBody();
    // Reset(), then the holders' own destructors (the routine: vptr only).
    // is the scalar deleting form.
    ~FEngineCharacterBody();

    // StopAnimations(), release the four accessory holders (the routine), and
    // IDeformable::Destroy (slot 6) the deformable.
    void Reset();
    // with a deformable: IDeformable slot 9 (drop every playback), then every
    // animation slot that is in use (id != -1) is emptied (name "" and id -1).
    void StopAnimations();
    // IDeformable slot 18 on the deformable into an int that starts at -1; -1
    // without a deformable.
    long GetRootBone() const;
    // (thiscall, RET 4) -- Out = the 12 floats IEngine slot 40
    // GetInstanceTransform answers for the root bone. Returns Out.
    FTransform* GetRootTransform(FTransform* Out) const;
    // (thiscall, RET 4) -- with a deformable: IAnimation2 slot 27 MixerWalk(root
    // bone, Dt * AnimRate), IEngine slot 27 UpdateInstance(root bone, 0, Dt * AnimRate), and
    // when IDeformable slot 26 reports no running playback every animation slot is emptied.
    void Advance(float Dt);
    // (thiscall, RET 4) -- UIWinDeform's (+0x8c) copy of FindOrCreate's inline
    // build: Reset(), the deformable from Key's bodyparts, the accessories hung on it. True when
    // there is a deformable.
    bool Build(Costume const& Key);
    // (CharCreate.cpp's copy of an EngineCharacter.h inline) -- IDeformable slot 26
    // GetListCount into a zeroed int; true when no playback runs. No deformable test.
    bool IsPlaybackIdle() const;
    // (thiscall, RET 0x14; CharCreate.cpp's copy of the EngineCharacter.h inline
    // HUD_Comm spells at 0x004cb452 / 0x004cb644) -- RootTransform refreshed from the root bone
    // (IEngine slot 40), then with a non-empty Name: the first free playback slot takes the name,
    // gf\CharacterInfo.cpp's script for it (the routine / the routine) and IDeformable slot 7
    // Play(instance, &AnimIds[slot], script, Name, Arg2, Arg3, Arg4, 1.0f, Arg5, -1.0f, 0);
    // a failure FDUMPs (0x100002, line 0x1f7). True when Play returned 0.
    bool PlayAnimation(char const* Name, unsigned Arg2, float Arg3, float Arg4, int Arg5);

    void* Deformable;                // +0x000 IDeformable instance
    FMeshHolder Accessories[4];        // +0x004 one per accessory (+0x04 = engine instance)
    TString<0x80> AnimNames[4];        // +0x044 the script each playback slot runs
    int AnimIds[4];                    // +0x254 playback ids, -1 = free
    float AnimRate;                    // +0x264 1.0
    FTransform RootTransform;          // +0x268 last root-bone transform read

private:
    FEngineCharacterBody(FEngineCharacterBody const&) = delete;
    FEngineCharacterBody& operator=(FEngineCharacterBody const&) = delete;
};

// ------------------------------------------------------------------------------------------
// The cache.
// ------------------------------------------------------------------------------------------
struct FEngineCharacterCacheNode
{
    Costume CostumeKey;            // node +0x08
    FEngineCharacterBody* Body;    // node +0x3c
    unsigned RefCount;             // node +0x40
};

class FEngineCharacterCache
{
public:
    // the empty list.
    FEngineCharacterCache();
    // (atexit the branch) -- per node: the body's animations stopped, its holders
    // released, its deformable destroyed and the body freed; then the node.
    ~FEngineCharacterCache();

    // (thiscall, RET 4).
    FEngineCharacterBody* FindOrCreate(Costume const& Key);

    // HUD_Receiver's destructor, 0x004ce99d..0x004ce9d1 (inlined there): every body deleted
    // (the routine) and every node erased (the routine). The list itself stays usable.
    void DeleteAll();

private:
    FEngineCharacterCache(FEngineCharacterCache const&) = delete;
    FEngineCharacterCache& operator=(FEngineCharacterCache const&) = delete;

    std::list<FEngineCharacterCacheNode> Nodes;
};

// .
extern FEngineCharacterCache G_EngineCharacterCache;
