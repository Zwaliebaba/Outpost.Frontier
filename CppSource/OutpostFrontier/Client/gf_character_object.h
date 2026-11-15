// Author: Giperion.

#pragma once

// CharacterObject (RTTI .?AVCharacterObject@@, vtable 0x005cb930, operator new(0x3b0) in
// ) -- Client\gf\ObjectDeformable.cpp (the FDUMP file name at
// .rdata 0x005cb964). The room object a person in a base is: a deformable body built from a
// Costume's bodyparts (NeuronRuntime::Deform), up to eight accessory meshes hung on its hardpoints, a
// screen hotspot the mouse talks to, an idle script of its own (a Gf script sequence) and the
// behaviour script a click starts.

// baseline slots over RoomObject (0x005e14a4): 0 the routine (deleting dtor), 3 the branch
// (SetFloat1C), 5 the branch (Update), 6 the branch (GetEngineInstance), 7 the branch
// (DrawOpaque), 10..12 the routine / the routine / the routine (hotspot events).

// Built by the room (RoomControl the routine -> the routine -> the routine) for every
// character record the server sends, and by the Gf script spawn (the routine, 0x0044ea12) for
// a DEFORMABLE entity no room character stands in for.

// The fields keep their baseline offsets in comments; the port's RoomObject base is baseline's size
// only in iterator-debug builds, and nothing reads a character by raw offset.

#include "common_csys.h"
#include "common_goods.h" // CharacterProperties, HardpointSummary, CharPlaceInfo
#include "gf_hotspot.h"
#include "room_control.h"

class DetailSwitchTable;
struct FGfScriptSequence;
struct ICamera;

// The per-part LOD switch table a character body draws with (0x124 bytes). CharacterObject
// keeps one at +0x30; the "pc" characters (CharacterProperties +0x1c == 1) all share the one at
// instead (static init 0x00443e40 zero-fills it; the routine releases it).
struct FCharacterLodSet
{
    void* Instance;                   // +0x000 the IDeformable instance
    int Counts[4];                    // +0x004 the LOD count of each bodypart (0 = no part)
    float Switch[4][16];              // +0x014 each LOD's switch distance
    DetailSwitchTable* Tables[4];     // +0x114 the bodypart's DetailSwitchTable
};

// 's part table is IDeformable::Create's FDeformPartDesc / FDeformLodDesc
// (engine/sceneruntime.h): sixteen LOD slots per part, whose Fraction comes back filled.

// The description the routine builds on the stack and CharacterObject's ctor reads (0xa0 bytes;
// ctor the routine, dtor the routine).
struct FCharacterDesc
{
    // CharacterProperties constructed, Placement 0, PlacementIndex -1,
    // bScriptSpatial 0, Pos 0, Orient identity.
    FCharacterDesc();
    // CharacterProperties::destroy, then its destructor.
    ~FCharacterDesc();

    CharacterProperties Props;     // +0x00
    CharPlaceInfo* Placement;      // +0x64 the room placement the character stands on, or null
    int PlacementIndex;            // +0x68 its index in the room's +0x504 vector, -1 = none
    unsigned char bScriptSpatial;  // +0x6c handed to the own script's sequence +0x08
    Vector Pos;                    // +0x70
    Matrix Orient;                 // +0x7c

private:
    FCharacterDesc(FCharacterDesc const&) = delete;
    FCharacterDesc& operator=(FCharacterDesc const&) = delete;
};

struct FCharacterObject;

// The Character's hotspot (vftable 0x005cb9ec, operator new(0x28) at 0x00444764): the screen
// rectangle of its bounding sphere.
class FCharacterHotspot : public FHotspot
{
public:
    // the routine, then +0x20 = 0 and +0x24 = 0.
    FCharacterHotspot();
    // (the routine the deleting form): the Hotspot destructor.
    ~FCharacterHotspot() override;

    // (thiscall, RET 4) -- the rectangle around the owner's engine instance
    // (Engine slot 36) projected with radius 1.0: x - 0.6r, y - r, 1.2r wide, 2r high, each
    // floor(v + 0.5); +0x24 = 0. Not projected: +0x24 = 1 and the rectangle zero.
    void UpdateScreenRect(ICamera* Camera);

    FCharacterObject* Character;  // +0x20
    unsigned char bOffscreen;     // +0x24
};

struct FCharacterObject : FRoomObject
{
    // (thiscall, RET 8) -- members built, the handles -1, then Load(Desc).
    FCharacterObject(FRoomRuntime* InOwner, FCharacterDesc const& Desc);
    // (the routine is the scalar deleting destructor).
    ~FCharacterObject() override;

    void SetFloat1C(float Value) override;          // 3  the branch
    void Update(double DeltaSeconds) override;      // 5  the branch
    void* GetEngineInstance() override;             // 6  the branch
    void DrawOpaque(ICamera* Camera) override;      // 7  the branch
    void Slot10(unsigned long Cookie, int const* Point) override; // 10 the routine (enter)
    void Slot11(unsigned long Cookie, int const* Point) override; // 11 the routine (leave)
    void Slot12(unsigned long Cookie, int const* Point) override; // 12 the routine (click)

    // (thiscall) -- restart the own script: a live sequence is restarted (or
    // played when it never was), else the idle script (+0x1ac) is created and played.
    void RestartOwnScript();
    // (thiscall, RET 8) -- a new Gf sequence playing ScriptName for this
    // character, framed by the placement; null for no name, FDUMP when none can be allocated.
    FGfScriptSequence* CreateOwnScript(char const* ScriptName, unsigned char bSpatial);
    // (thiscall, RET 8) -- the own script becomes ScriptName (unless it already
    // is): the old one destroyed, the new one created and, while not taken over, played.
    void SetOwnScript(char const* ScriptName, unsigned char bSpatial);
    // (thiscall) -- the hotspot answers the mouse only with a behaviour or a
    // role.
    void UpdateHotspotEnabled();

    // (thiscall, RET 4) -- RoomObject +0x04 cleared; with bRelease the +0x360 /
    // +0x368 pair is cleared and the +0x364 name freed, otherwise +0x36c = 2. Caller:
    // SceneScript_Deformable's slot 14 (0x004521f5) with the owning script's +0x55.
    void ResetScriptState(unsigned char bRelease);
    // (thiscall, RET 4) -- a Gf script takes the character over: RoomObject +0x04
    // set (no more self-update); with bKeepOwnScript clear the character's own sequence (+0x360)
    // is destroyed through the Gf script manager (the routine), +0x360 / +0x368 cleared, the
    // +0x364 name freed, and +0x36c = 1. Caller: the spawn's the routine (0x0044ea43) with the
    // owning script's +0x55.
    void TakeOverForScript(unsigned char bKeepOwnScript);
    // (thiscall, RET 4) -- the part named Name: a cached hit from Parts, else
    // CompoundInstanceFindFirstName on the engine instance, cached in the first unused entry
    // (entry 0 when all five are used). -1 when there is none.
    long FindPart(char const* PartName);
    // / the routine (thiscall, RET 4) -- ++ / -- ActiveMotions unless Handle is -1.
    void AddMotionHandle(int Handle);
    void RemoveMotionHandle(int Handle);
    // / the routine (thiscall, RET 4) -- ++ / -- ActiveAims unless Handle is -1.
    void AddAimHandle(void* Handle);
    void RemoveAimHandle(void* Handle);
    // (thiscall, RET 4) -- one of the six "$BGCS_*" context tokens rewritten into
    // the voice-message name voices.ini carries, from this character's costume gender (+0x178),
    // its room's base ([+0x08]+0x34) and its faction (+0x164). Returns the static buffer
    // on every exit (valid until the next call). Callers: the routine (0x00457129,
    // ) with the speaking actor.
    char const* ResolveBgcsContextToken(char const* TemplateName) const;

    // One cached part lookup (baseline +0x324, five of 0xc bytes). the branch builds one,
    // tears it down.
    struct FPartCacheEntry
    {
        FPartCacheEntry();
        ~FPartCacheEntry();

        char* Name;          // +0x00 _strdup'd, freed with free()
        long Part;           // +0x04 -1 = not resolved
        unsigned char bUsed; // +0x08
    };

    static const int kPartCacheEntries = 5;
    static const int kMaxAccessories = 8;

    FCharacterLodSet Lod;              // +0x030 (Lod.Instance: the IDeformable instance)
    CharacterProperties Props;         // +0x154 the server's record (Props.VoiceId is +0x1b4)
    CharPlaceInfo* Placement;          // +0x1b8
    int PlacementIndex;                // +0x1bc -1 = not on a room placement
    int LodIds[4];                     // +0x1c0 the routine's pick per part, -1 = none
    long AccessoryInstances[kMaxAccessories];      // +0x1d0 -1 = none
    HardpointSummary AccessoryHardpoints[kMaxAccessories]; // +0x1f0
    int AccessoryCount;                // +0x2f0
    FCharacterHotspot* Hotspot;        // +0x2f4
    unsigned char bHovered;            // +0x2f8 the mouse is over the hotspot (name tooltip)
    wchar_t* DisplayName;              // +0x2fc Props.NameIds as ids, or Props.NameWide
    int Role;                          // +0x300 0, 1 bartender, 2 trader, 3 shipdealer, 4 equipment
    HardpointSummary Hardpoints;       // +0x304 the body's hardpoints
    FPartCacheEntry Parts[kPartCacheEntries]; // +0x324
    FGfScriptSequence* OwnScript;      // +0x360 the idle script (the routine)
    char* OwnScriptName;               // +0x364 malloc'd
    unsigned char bOwnScriptPlaying;   // +0x368 the routine ran on +0x360
    int ScriptState;                   // +0x36c 0 own, 1 taken over (the routine), 2 released
    int ActiveMotions;                 // +0x370 motion handles SceneScript_Deformable holds
    int ActiveAims;                    // +0x374 aim handles SceneScript_Deformable holds
    unsigned char bPoseDirty;          // +0x378 the pose moved: the next Update walks the mixer
    float LastPos[3];                  // +0x37c the engine instance's position last Update
    float LastOrient[9];               // +0x388 and its orientation
    unsigned char bHasLastPose;        // +0x3ac
    unsigned char bSharedDeform;       // +0x3ad Props +0x1c == 1: the body is the field's

private:
    // (thiscall, RET 4) -- the record, role, body, accessories, hotspot, name and
    // own script.
    void Load(FCharacterDesc const& Desc);
    // (thiscall, RET 4) -- LodIds from the camera distance.
    void PickLods(ICamera* Camera);

    FCharacterObject(FCharacterObject const&) = delete;
    FCharacterObject& operator=(FCharacterObject const&) = delete;
};

// (cdecl) -- the shared "pc" body (the field) destroyed (IDeformable slot 6)
// and its table cleared. Caller: the Gameflow base leave the routine (0x00558a71).
void ReleaseSharedCharacterLod();

// (cdecl) -- a new CharacterObject for Owner from Props at Placement (index
// PlacementIndex) and Frame.
FCharacterObject* CreateCharacterObject(CharacterProperties const& Props, CharPlaceInfo* Placement,
                                        int PlacementIndex, FRoomRuntime* Owner,
                                        unsigned char bScriptSpatial, Csys const& Frame);

// (cdecl) -- play the behaviour BehaviorKey of Room's script behaviours for
// Character as a letterboxed Gf sequence (the behaviour's scripts, dialog texts and characters),
// named after Placement; Callback / CallbackA go to the sequence's stop callback.
void StartCharacterBehaviorScript(FCharacterObject* Character, FRoomRuntime* Room,
                                  CharPlaceInfo const* Placement, unsigned BehaviorKey,
                                  void(__cdecl* Callback)(void*, void*, unsigned char),
                                  void* CallbackA);

// (cdecl) -- the player character of the current room (the routine) put at the
// current room's setpoint SetpointName (stricmp over RoomData +0x164): IDeformable slots 19 / 20
// on its instance, then its slot 3 with the setpoint's height. Callers: NavBar 0x0043ea45 /
// / 0x0043eab3 / 0x0043fff4 / 0x00440027 / 0x00440057.
void PlacePlayerAtSetpoint(char const* SetpointName);
