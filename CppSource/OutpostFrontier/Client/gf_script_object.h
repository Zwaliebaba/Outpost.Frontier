// Author: Giperion.

#pragma once

// Gf\Script (Client\Gf\Script.cpp), Gf\ScriptSequence
// (ScriptSequence.cpp), the sequence list at the field and Gf\MirrorDatabase
// (MirrorDatabase.cpp) -- what plays a SceneScript script in a room.

//   * FGfScript (operator new(0xb4), ctor the routine) is one loaded .thn bound to its room:
//     it spawns one SceneScript object per entity (the routine, gf_script_spawn.cpp), keeps the
//     script's streamed / preloaded sound events, and runs the IScriptEngine clock.
//   * FGfScriptSequence (operator new(0x13c), ctor the routine) runs up to 0x20 scripts one
//     after another (entries of 0x40 bytes) and owns what their spawns create: the props of
//     COMPOUND entities (+0x130), the characters of DEFORMABLE entities (+0x124) and the
//     character bindings (+0x114).
//   * FGfScriptManager (the object at the field, vtable 0x005ce114) holds every live
//     sequence: RoomControl creates them (the routine), the Gameflow tick advances them
//     (the routine), the room draw draws their effects (the routine), the reference
//     entities find the entity they name through it (the routine). baseline keeps the Gf audio
//     fade in the same object (+0x10..+0x20); the port keeps that part in sound_config.cpp
//     (GetClientAudioFade / SnapClientAudioFade / TickClientAudioFade), so FGfScriptManager
//     itself does not carry those fields.

// Field offsets are baseline's, in comments; the port's layout is not ABI (baseline's VC6 lists and
// FmtStr pairs are held as current-STL members).

#include "common_csys.h"
#include "common_text.h" // FmtStr
#include "event_handler.h"

#include <list>
#include <string>

class FObjectProp;
class NnOfferDialog;
struct FCharacterObject;
struct FRoomObject;
struct FRoomRuntime;
struct FSceneScriptComponent;
struct FSceneScriptEntitySnap;
struct FSceneScriptSound;
struct ICamera;
struct IScriptEngine;
struct FGfScriptSequence;
class FGfMirrorDatabase;

// One sound event a script plays or preloads (the 0x20-byte payload of a +0xa8 list node,
// built by the routine from the routine's five words).
struct FGfScriptSoundEvent
{
    void* Arch;              // +0x00 Archetype::ISound of the sound entity (SceneScript_Sound +0x7c)
    FSceneScriptSound* Sound;      // +0x04 the SceneScript_Sound that plays it
    int StartMs;             // +0x08 the event's start
    int EndMs;               // +0x0c start + duration
    unsigned char bLoop;     // +0x10 event Flags bit 3
    unsigned char bSpatial;  // +0x11 entity Flags bit 1
    unsigned char Pad12[2];
    int EventId;             // +0x14 the event's unique id
    void* Handle;            // +0x18 ISoundSource of a streamed sound, or null
    unsigned char bStarted;  // +0x1c
    unsigned char Pad1D[3];
};

struct FGfScript
{
    // the empty event list, then the routine.
    FGfScript();
    // every spawned object deleted (identity slot 13), the table freed, the event
    // list freed.
    ~FGfScript();

    // every field back to its default (+0x34 is not written).
    void Init();
    // (thiscall, RET 8) -- +0x30 / +0x34.
    void SetRoom(FRoomRuntime* InRoom, FGfScriptSequence* InSequence);
    // once: the script's sounds stopped, every spawned object told the scene
    // is gone (slot 19), stopped (slot 14) and detached from the engine (slot 13), the
    // sequence's props dropped (the routine), both names freed.
    void Teardown();
    // one SceneScript object per entity of the engine (gf_script_spawn.cpp).
    void Spawn();

    // (thiscall, RET 8) -- a START_SOUND event of a created, non-player sound
    // entity: its archetype, object, span and flags into Out. False otherwise.
    bool CollectSoundEvent(int EventId, FGfScriptSoundEvent* Out);
    // one list entry per event the routine accepts.
    void CollectSoundEvents();
    // every entry not started yet: a plain archetype acquired (slot 3), a
    // streamer started through the routine (bAltStart 1); started either way.
    void StartScriptSounds();
    // the longest time a held voice line still runs past the clock, 0 for none.
    int GetSoundTailMs();
    // every started entry: an archetype released (slot 4), a streamed handle
    // stopped and released; not started any more.
    void StopScriptSounds();
    // (thiscall, RET 4) -- the first entry holding Handle leaves the list. Callers:
    // SceneScript_Sound's dtor and ISoundControl slot 4 (gf_scenescript_sound.cpp).
    void RemoveSoundEvent(void* Handle);
    // (thiscall, RET 8) -- the streamed handle of EventId restarted at Offset
    // (entry found, handle held), or a new source for it (the routine, bAltStart 0). Caller:
    // SceneScript_Sound's ISoundControl slot 3.
    void* PlayStreamedSoundEvent(int EventId, int Offset);

    // (thiscall, RET 0x2c) -- bind the engine and play: spawn, the sound events,
    // IScriptEngine slots 20 (loop), 22 (start) and 26 (clock 0).
    void Start(IScriptEngine* InEngine, char const* InScriptFile, int InBindMode,
               unsigned char bInLoop, unsigned char bInField55, unsigned char bInApplySpatial,
               int InPriority, unsigned char bInFade, char const* InName,
               unsigned char bInPropsToRoom, unsigned char bInSoundsUseFade);
    // (thiscall, RET 4) -- advance the clock by Ms (or run out a paused script);
    // true once finished.
    bool Update(int Ms);
    // IScriptEngine slots 23 (stop) and 22 (start 0).
    void Restart();
    // (thiscall, RET 4) -- identity slot 18 on every spawned object.
    void DrawEffects(ICamera* Camera);
    // (thiscall, RET 8) -- the prop behind entity Index is (un)detached: its
    // engine instance (SceneScript object slot 17) goes to the sequence's the routine. Callers: the
    // hardpoint connect / disconnect slots of SceneScript_Compound (0x00451e7a / 0x00451eba) and
    // SceneScript_Deformable (0x00452d9a / 0x00452dda), with 1 / 0.
    void SetEntityPropDetached(int Index, unsigned char bDetached);
    // +0x44 = 1: the next Update runs the script out.
    void Pause();
    // (thiscall, RET 4) -- Update by the remaining time scaled by Scale
    // (rounded; one more millisecond unless Scale < 1).
    void Advance(float Scale);

    // --- the spawn's factories (gf_script_spawn.cpp) -----------------------------------
    FSceneScriptComponent* SpawnScene(FSceneScriptEntitySnap const* Snap);
    void ApplySpecialMarker(FSceneScriptEntitySnap const* Snap, FSceneScriptComponent* Obj);
    FSceneScriptComponent* SpawnMarker(FSceneScriptEntitySnap const* Snap);
    FSceneScriptComponent* SpawnMonitor(FSceneScriptEntitySnap const* Snap);
    void ApplySpecialCamera(FSceneScriptEntitySnap const* Snap, FSceneScriptComponent* Obj,
                            float const* CameraProps4);
    FSceneScriptComponent* SpawnCamera(FSceneScriptEntitySnap const* Snap);
    FSceneScriptComponent* SpawnCameraReference(FSceneScriptEntitySnap const* Snap);
    FSceneScriptComponent* SpawnCompound(int Index, FSceneScriptEntitySnap const* Snap);
    FSceneScriptComponent* SpawnDeformable(int Index, FSceneScriptEntitySnap const* Snap);
    FSceneScriptComponent* SpawnSound(int Index, FSceneScriptEntitySnap const* Snap);
    FSceneScriptComponent* SpawnPlayerEngineSound(FSceneScriptEntitySnap const* Snap);
    FSceneScriptComponent* SpawnSoundReference(FSceneScriptEntitySnap const* Snap);
    FSceneScriptComponent* SpawnLight(FSceneScriptEntitySnap const* Snap);
    FSceneScriptComponent* SpawnLightReference(FSceneScriptEntitySnap const* Snap);
    FSceneScriptComponent* SpawnPsys(FSceneScriptEntitySnap const* Snap);
    FSceneScriptComponent* SpawnPlayerEnginePsys(FSceneScriptEntitySnap const* Snap);
    // (thiscall, RET 0x1c) -- what a COMPOUND entity drives.
    void ResolveCompoundTarget(int Index, FRoomObject** OutObject, long* OutInstance,
                               void** OutAnimScript, Csys* OutOffset, int* OutCategory,
                               unsigned char* bOutUseScene);
    // (thiscall, RET 8) -- the character a DEFORMABLE entity drives.
    FCharacterObject* ResolveCharacter(FSceneScriptEntitySnap const* Snap, char const* Actor);

    Csys Frame;                       // +0x00 identity; every SceneScript object's base ctor copies it
    FRoomRuntime* Room;               // +0x30 the room this script plays in
    FGfScriptSequence* Sequence;      // +0x34 the sequence that runs it
    FGfMirrorDatabase* Mirror;        // +0x38 the sequence's +0x114 (the routine 0x00454ff6)
    IScriptEngine* Engine;            // +0x3c
    int Duration;                     // +0x40 IScriptEngine slot 17
    unsigned char bPaused;            // +0x44 the routine; SceneScript_Sound slot 3 skips while set
    unsigned char bTornDown;          // +0x45 the routine ran
    unsigned char Pad46[2];
    FSceneScriptComponent** Spawned;        // +0x48 one SceneScript object per entity, or null
    int SpawnedCount;                 // +0x4c
    int BindMode;                     // +0x50 how a DEFORMABLE's actor is found (the routine)
    unsigned char bLoop;              // +0x54 IScriptEngine slot 20
    unsigned char Field_0x55;         // +0x55 TODO: meaning not recovered; handed to
                                      //       CharacterObject the routine by the spawn and to
                                      // by SceneScript_Deformable's slot 14
    unsigned char bApplySpatial;      // +0x56 1: the spawn writes each entity's initial pose
    unsigned char Pad57;
    int Priority;                     // +0x58 camera pool priority ("priority" of the SCENE)
    unsigned char bPropsToRoom;       // +0x5c a spawned prop goes to the room, not the sequence
    unsigned char bFade;              // +0x5d fade to black at the end
    unsigned char bFadeStarted;       // +0x5e
    unsigned char bFinished;          // +0x5f
    unsigned char bSoundsUseFade;     // +0x60 handed to every SceneScript_Sound's +0x84
    unsigned char bPlayerActor;       // +0x61 a DEFORMABLE bound the player
    unsigned char Pad62[2];
    Csys PlayerCsys;                  // +0x64 that DEFORMABLE's character frame (the routine)
    float PlayerField94;              // +0x94 TODO: the character's slot 4 (+0x10) float
    void* PlayerDeformInstance;       // +0x98 the routine
    FCharacterObject* PlayerCharacter; // +0x9c
    char* Name;                       // +0xa0 malloc'd (the routine)
    char* ScriptFile;                 // +0xa4 malloc'd (the routine)
    std::list<FGfScriptSoundEvent> SoundEvents; // +0xa8 (head +0xac, size +0xb0)
};

// Gf\MirrorDatabase (0xc bytes at ScriptSequence +0x114): character objects by name -- the
// characters a sequence plays with (RoomControl the routine adds the room's, 0x0044aaf7).
// A list of {object, VC6 std::string}.
class FGfMirrorDatabase
{
public:
    // the empty list.
    FGfMirrorDatabase();
    // every node freed, the list gone.
    ~FGfMirrorDatabase();

    // (thiscall, RET 8) -- append {Object, Name}; a null Name is "".
    void Add(FCharacterObject* Object, char const* ObjName);
    // the first object, or null.
    FCharacterObject* First() const;
    // (thiscall, RET 4) -- the first object whose name matches (stricmp), or null.
    FCharacterObject* Find(char const* ObjName) const;

    struct FEntry
    {
        FCharacterObject* Object; // node +0x08
        std::string Name;         // node +0x0c
    };

    std::list<FEntry> Entries;    // +0x04 head, +0x08 size
};

// One script slot of a sequence (0x40 bytes; ctor the branch, dtor the branch).
struct FGfScriptEntry
{
    FGfScriptEntry();
    ~FGfScriptEntry();

    // (thiscall, RET 8) -- a new Script bound to Room / Seq, framed by Frame. A
    // second run reloads the engine from the file first.
    FGfScript* Create(FRoomRuntime* InRoom, FGfScriptSequence* InSeq);

    FGfScript* Script;       // +0x00
    IScriptEngine* Engine;   // +0x04
    char* ScriptFile;        // +0x08 malloc'd
    Csys Frame;              // +0x0c
    int Kind;                // +0x3c 0 plain, 1 / 4 / 6 dialog kinds, 5 traffic (the routine)
};

struct FGfScriptSequence
{
    // .
    FGfScriptSequence();
    // stopped first unless already (the routine(1)); entries, names, the
    // pending object, props and characters deleted.
    ~FGfScriptSequence();

    // (thiscall, RET 4) -- +0x120 = malloc copy of InName.
    void SetName(char const* InName);
    // (thiscall, RET 0x10) -- append a script: file, kind, frame; the engine loaded
    // from the file when none is given. No capacity check (baseline).
    void AddScript(char const* File, int InKind, Csys const* InFrame, IScriptEngine* InEngine);
    // (thiscall, RET 0x14) -- +0xc8..+0xd0 and the two text blocks.
    void SetDialog(int A, int B, int C, FmtStr const& TextA, FmtStr const& TextB);
    // play: the setup (the routine), the first script, active.
    void Play();
    // (thiscall, RET 4) -- start entry Index.
    void StartEntry(int Index);
    // the room camera saved, the scripts' animations / audio flags gathered.
    void Setup();
    // (thiscall, RET 4) -- once: location change, player / camera / audio restored,
    // animations released, callback.
    void StopPlayback(unsigned char bArg);
    // (thiscall) -- the multiplayer group offer was answered (IClient slot 114):
    // cleared; with the offer dialog up it is closed and the script resumes.
    void OnMissionOfferAnswered();
    // (thiscall, RET 8) -- open the kind-1 offer: NN_OfferDialog
    // "ScriptSequence_OfferDialog" into +0x108 with the type / text of +0xc8, a single button
    // unless a Decline branch (kind 3) follows. bHasAcceptBranch is not read.
    void ShowOfferDialog(unsigned char bHasAcceptBranch, unsigned char bHasDeclineBranch);
    // (cdecl) -- the offer's answer: IServer::MissionResponse, then the dialog
    // closed and the script resumed (in multiplayer group offers only once the server answered).
    static void __cdecl OnOfferAnswer(void* Context, int Answer);
    // (cdecl) -- the info dialog (kind 4) closed: +0x10d = 1, the dialog deleted,
    // the script resumed.
    static void __cdecl OnInfoDialogClosed(void* Context, int Answer);
    // (thiscall, RET 8) -- append {Prop, Name}. Caller: the routine (0x0044e82e).
    void AddProp(FObjectProp* Prop, char const* PropName);
    // every prop deleted, the list emptied. Caller: the routine (0x0044ed58).
    void ClearProps();
    // (thiscall, RET 8) -- the first prop of Props whose engine instance (RoomObject
    // slot 6) is Instance gets bDetached (ObjectProp +0x48); none, nothing.
    void SetPropDetached(void* Instance, unsigned char bDetached);
    // (thiscall, RET 8) -- append {Character, Name}. Caller: the routine.
    void AddCharacter(FCharacterObject* Character, char const* CharName);
    // (thiscall, RET 4) -- the first character named CharName (stricmp), or null
    // (null for a null name).
    FCharacterObject* FindCharacter(char const* CharName) const;
    // (thiscall, RET 8) -- how many entries ahead the next one of Kinds is.
    int FindNextOfKinds(int const* Kinds, int KindCount) const;
    // the entry that follows the current one.
    int NextEntryIndex() const;
    // (thiscall, RET 4) -- advance by Ms; true once the sequence is done.
    bool Update(int Ms);
    // the current script restarted (the routine), playing again.
    void RestartCurrent();
    // (thiscall, RET 4) -- while playing, the routine on the current script.
    void AdvanceCurrent(float Scale);

    struct FProp
    {
        FObjectProp* Prop;   // node +0x08
        std::string Name;    // node +0x0c
    };
    struct FCharacter
    {
        FCharacterObject* Character; // node +0x08
        std::string Name;            // node +0x0c
    };

    typedef void(__cdecl* FStopCallback)(void* A, void* B, unsigned char bArg);

    int BindMode;                  // +0x00 -> FGfScript +0x50
    unsigned char bLoop;           // +0x04 -> FGfScript +0x54
    unsigned char Field_0x05;      // +0x05 -> FGfScript +0x55
    unsigned char bLetterbox;      // +0x06 1: letterbox / cursor / hotspot lock around playback
    unsigned char bFade;           // +0x07 -> FGfScript +0x5d
    unsigned char bApplySpatial;   // +0x08 1 -> FGfScript +0x56
    unsigned char bRestoreCamera;  // +0x09 1: the room camera restored at the stop
    unsigned char bDuckAmbient;    // +0x0a the room ambience faded out while playing
    unsigned char bDuckMusic;      // +0x0b the room music faded out while playing
    unsigned char bDuckSlow;       // +0x0c the slow (config) fade instead of an instant one
    unsigned char bSoundsUseFade;  // +0x0d -> FGfScript +0x60; the routine's refresh gate
    unsigned char bRestorePlayer;  // +0x0e 1: the player character's pose restored at the stop
    unsigned char bHoldTime;       // +0x0f 1: the routine at each script start
    int Priority;                  // +0x10 -> FGfScript +0x58
    unsigned char bPropsToRoom;    // +0x14 -> FGfScript +0x5c
    unsigned char Field_0x15;      // +0x15 1 TODO: gates +0x2e with +0x16 (0x00455085, 0x00456866)
    unsigned char Field_0x16;      // +0x16 TODO: see +0x15
    unsigned char Pad17;
    FStopCallback Callback;        // +0x18 called at the stop with (+0x1c, +0x20, bArg)
    void* CallbackA;               // +0x1c
    void* CallbackB;               // +0x20
    unsigned RtcId;                // +0x24 non-zero: IServer::RTCDone(session, it) at the stop
                                   //       (slot 69, +0x114); the routine sets ScriptBehavior
                                   //       +0x25c
    unsigned Location;             // +0x28 the location to move to at the stop, 0 = none
    unsigned char bStopped;        // +0x2c 1 until Setup, 1 again after StopPlayback
    unsigned char bPlaying;        // +0x2d Play / RestartCurrent
    unsigned char bSkip;           // +0x2e run the current script out
    unsigned char bFindable;       // +0x2f the routine's flag; the routine searches only these
    unsigned char bPlayerSaved;    // +0x30 the player character's pose below was saved
    unsigned char Pad31[3];
    Csys PlayerCsys;               // +0x34 FGfScript +0x64
    float PlayerField64;           // +0x64 FGfScript +0x94
    void* PlayerDeformInstance;    // +0x68 FGfScript +0x98
    FCharacterObject* PlayerCharacter; // +0x6c FGfScript +0x9c
    float CameraPos[3];            // +0x70 the room camera at Setup
    float CameraOrient[9];         // +0x7c
    float CameraHalfFovX;          // +0xa0
    float CameraZNear;             // +0xa4
    float CameraZFar;              // +0xa8
    unsigned char bHasAmbient;     // +0xac a script plays BackgroundAmbient
    unsigned char bHasMusic;       // +0xad a script plays BackgroundMusic
    unsigned char PadAE[2];
    FRoomRuntime* Room;            // +0xb0
    FGfScriptEntry* Entries;       // +0xb4 new[0x20]
    int Capacity;                  // +0xb8 0x20
    int Count;                     // +0xbc
    int Current;                   // +0xc0 -1 = none
    unsigned char bActive;         // +0xc4
    unsigned char PadC5[3];
    int OfferType;                 // +0xc8 the routine's first argument: 1..4 pick the
                                   //       NN_OfferDialog type and text (the routine); 3 is
                                   //       the multiplayer group offer (the routine /
                                   // )
    int OfferArg;                  // +0xcc IServer::MissionResponse argument 0 (the routine)
    int OfferMissionId;            // +0xd0 IServer::MissionResponse MissionId (the routine)
    FmtStr DialogTextA;            // +0xd4 the offer text of types 1, 3, 4
    FmtStr DialogTextB;            // +0xec the offer text of type 2; the info text (kind 4)
    unsigned char bOfferShown;     // +0x104 the kind-1 NN_OfferDialog is up
    unsigned char bOfferAnswered;  // +0x105 the routine sent the answer
    unsigned char bOfferAccepted;  // +0x106 the answer was Accept
    unsigned char Pad107;
    NnOfferDialog* OfferDialog;    // +0x108 the routine's "ScriptSequence_OfferDialog"
    unsigned char bInfoShown;      // +0x10c the kind-4 NN_OfferDialog is up
    unsigned char bInfoClosed;     // +0x10d the routine ran
    unsigned char Pad10E[2];
    NnOfferDialog* InfoDialog;     // +0x110 "ScriptSequence_InfoDialog" (StartEntry kind 4)
    FGfMirrorDatabase Mirror;      // +0x114
    char* Name;                    // +0x120
    std::list<FCharacter> Characters; // +0x124 (head +0x128, size +0x12c)
    std::list<FProp> Props;        // +0x130 (head +0x134, size +0x138)

private:
    FGfScriptSequence(FGfScriptSequence const&) = delete;
    FGfScriptSequence& operator=(FGfScriptSequence const&) = delete;
};

// The object at the field (sequence half; the fade half is sound_config.cpp). Its Event::
// Handler face (vtable 0x005ce114, one slot the branch) takes KeyMap id 0xf.
class FGfScriptManager : public EventHandler
{
public:
    // (static init the branch) is the implicit constructor: the EventHandler base,
    // the empty list; the fade half it zeroes (+0x10..+0x20) is sound_config.cpp's GAudioFade.
    FGfScriptManager() = default;
    // (the atexit the branch) -- every sequence still listed deleted (the routine
    // + operator delete), then the list and the EventHandler base (the routine).
    ~FGfScriptManager();

    // (thiscall, RET 8) -- a new sequence for Room with 0x20 empty entries,
    // appended.
    FGfScriptSequence* CreateSequence(FRoomRuntime* Room, unsigned char bFindable);
    // (thiscall, RET 4) -- every sequence advanced (the finished ones deleted),
    // then the Gf fade advanced by Ms * 0.001 and, when it moved, identity slot 21 on every
    // spawned object of the current script of each sequence with +0x0d.
    void Update(int Ms);
    // (thiscall, RET 8) -- the routine on the current script of every playing
    // sequence of Room. Caller: the room draw the routine (0x0044b8c8).
    void DrawEffects(FRoomRuntime* Room, ICamera* Camera);
    // (thiscall, RET 8) -- the first created (not reference) entity of Type named
    // EntityName among the current scripts of the findable sequences, or null.
    FSceneScriptComponent* FindCreatedEntity(int Type, char const* EntityName);
    // (vtable slot 0) -- event 0xf: every letterboxed sequence is marked to skip
    // (when playing and +0x15), and the routine runs when there was one; true then.
    bool OnEvent(unsigned MsgId) override;
    // false while a letterboxed sequence without +0x15 exists.
    bool CanSkipAll() const;
    // (thiscall, RET 4) -- Sequence deleted and taken out, if listed.
    void DestroySequence(FGfScriptSequence* Sequence);
    // every sequence deleted. Caller: RoomControl the routine (0x0044945c).
    void DestroyAll();
    // register id 0xf. Callers: the base enter the routine (0x0055893d) and
    // the intro 0x00573990.
    void RegisterEvents();
    // every sequence deleted, id 0xf unregistered. Callers: the base leave
    // (0x00558a6c) and the menu 0x0057405d / 0x005740ee / 0x00574a9a.
    void Shutdown();

    std::list<FGfScriptSequence*> Sequences; // +0x08 head, +0x0c size
};

extern FGfScriptManager G_GfScriptManager;

// the field, set by the option callback the branch (value < 0.5). Read by
// (0x00455075) for the skip latch.
bool GfScriptSequenceSkipLatch();
// (cdecl, float) -- the field = Value < 0.5 (`TEST AH,1` + JZ: not less keeps 0).
void GfScriptSequenceSetSkipOption(float Value);

// (cdecl) -- Levels times NeuronRuntime::Engine slot 53 (the parent instance) up from
// Instance; Levels not positive returns Instance itself. Caller: SceneScript_Deformable's IK start
// (0x0045248f) with the descriptor's count_to_root.
void* GfEngineInstanceAncestor(void* Instance, int Levels);
