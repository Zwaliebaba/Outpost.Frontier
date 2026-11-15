// Author: Giperion.

#include "gf_script_object.h"

#include "NeuronRuntime.h"
#include "character_info.h"
#include "common_goods.h" // SceneProgramLoad / SceneProgramDestroy
#include "common_ini.h"   // RefreshLoadingProgress
#include "common_misc.h"  // malloc_strcpy / malloc_strfree, SpaceflightLocationID
#include "common_server.h" // IServer::RTCDone / MissionResponse
#include "neuroncomponents.h"
#include "fade_to_black.h"
#include "gameflow.h"
#include "gf_character_object.h" // FCharacterObject (slot 3, deleting dtor)
#include "gf_script.h"
#include "gf_scenescript_component.h"
#include "gf_scenescript_sound.h"
#include "input_manager.h"
#include "knowledge_map.h"  // KnowledgeMapLookupFmtStr (the routine)
#include "login_mgr.h"      // G_Server (the field)
#include "nn_navmap.h"      // NavMapRequestFocus (the routine)
#include "nn_offer_dialog.h"
#include "win_factory.h"    // WinFactoryCreate (the routine)
#include "menu_sound_arch.h"
#include "menu_universe_base.h"
#include "nn_arrival_text.h"
#include "object_prop.h"
#include "player_spawn.h"   // GetPlayerConnectionId (the field)
#include "room_camera.h"
#include "room_control.h"
#include "sound_archetype.h"
#include "sound_config.h"
#include "soundmanager.h"
#include "space_letterbox.h"
#include "scenescript_iface.h"

#include <math.h>
#include <string.h>

FGfScriptManager G_GfScriptManager;

namespace
{
    // .
    bool G_bGfScriptSequenceSkipLatch = false;
}

// (the option callback registered at 0x005b58c7). 0x004548f4: cleared first, then
// set when Value < 0.5 (the field; `TEST AH,1` + JZ leaves it clear only for not-less: an
// unordered compare sets C0, so a NaN sets it too).
void GfScriptSequenceSetSkipOption(float Value)
{
    G_bGfScriptSequenceSkipLatch = false;
    if (!(static_cast<double>(Value) >= 0.5))
        G_bGfScriptSequenceSkipLatch = true;
}

// .
bool GfScriptSequenceSkipLatch()
{
    return G_bGfScriptSequenceSkipLatch;
}

namespace
{
    char const* const GScriptSequenceFile = "Client\\Gf\\ScriptSequence.cpp";

    // The fixed-rate ducking of the routine / the routine: the field (-20 dB) and
    // (2.5 dB per second); 0x501502f9 (1e10) is "the ducking config".
    float const kSequenceDuckDb = -20.0f;
    float const kSequenceDuckRate = 2.5f;
    float const kUseDuckingConfig = 1.0e10f;

    void SetIdentityCsys(Csys& Out)
    {
        Out.Pos.x = 0.0f;
        Out.Pos.y = 0.0f;
        Out.Pos.z = 0.0f;
        int Row = 0;
        for (Row = 0; Row < 3; ++Row)
        {
            int Col = 0;
            for (Col = 0; Col < 3; ++Col)
                Out.Orient.E[Row][Col] = Row == Col ? 1.0f : 0.0f;
        }
    }

    // The play key the routine takes (six dwords; flags byte at +4: bit 0 loop, bit 1 spatial,
    // bit 2 streamer).
    struct FScriptSoundKey
    {
        unsigned Offset;
        unsigned Flags;
        unsigned Rest[4];
    };

    unsigned char MakeKeyFlags(unsigned char bStreamer, unsigned char bSpatial,
                               unsigned char bLoop)
    {
        // the three XOR/AND inserts into a byte whose low three bits
        // were cleared (AND AL,0xf8).
        unsigned char Flags = 0;
        Flags = static_cast<unsigned char>(Flags ^ (((bStreamer << 2) ^ Flags) & 4));
        Flags = static_cast<unsigned char>(Flags ^ (((bSpatial << 1) ^ Flags) & 2));
        Flags = static_cast<unsigned char>(Flags ^ ((bLoop ^ Flags) & 1));
        return Flags;
    }
}

// =========================================================================================
// FGfScript
// =========================================================================================

// . 0x0044eab0..0x0044eaf7: the +0xa8 list's sentinel, size 0; 0x0044eafc
// . +0x34, +0x5c and +0x60 are not written by either (the routine / the routine
// write them before anything reads them); the port zeroes them here for C++ hygiene only.
FGfScript::FGfScript()
    : Sequence(nullptr),
      bPropsToRoom(0),
      bSoundsUseFade(0)
{
    Init();
}

// . 0x0044eb40..0x0044eb6d: every spawned object deleted through identity slot 13
// (the scalar deleting dtor, flag 1) and its entry zeroed; 0x0044eb6f..0x0044eb78 the table
// freed, +0x48 = 0; 0x0044eb7b..0x0044ebd7 every event node freed, the sentinel too.
FGfScript::~FGfScript()
{
    int Index = 0;
    for (Index = 0; Index < SpawnedCount; ++Index)
    {
        if (Spawned[Index] != nullptr)
            delete Spawned[Index];
        Spawned[Index] = nullptr;
    }
    delete[] Spawned;
    Spawned = nullptr;
    SoundEvents.clear();
}

// . Frame and PlayerCsys to identity; +0x30, +0x38..+0x40, +0x48, +0x4c, +0x50,
// +0x58, +0x94, +0x98, +0x9c, +0xa0, +0xa4 = 0; bytes +0x44, +0x45, +0x54, +0x55, +0x5d, +0x5e,
// +0x5f, +0x61 = 0 and +0x56 = 1. +0x34, +0x5c and +0x60 are not touched.
void FGfScript::Init()
{
    SetIdentityCsys(Frame);
    Room = nullptr;
    Mirror = nullptr;
    Engine = nullptr;
    Duration = 0;
    bPaused = 0;
    bTornDown = 0;
    Spawned = nullptr;
    SpawnedCount = 0;
    bFinished = 0;
    bPlayerActor = 0;
    PlayerCharacter = nullptr;
    SetIdentityCsys(PlayerCsys);
    PlayerField94 = 0.0f;
    PlayerDeformInstance = nullptr;
    BindMode = 0;
    bLoop = 0;
    Field_0x55 = 0;
    bApplySpatial = 1;
    Priority = 0;
    bFade = 0;
    bFadeStarted = 0;
    Name = nullptr;
    ScriptFile = nullptr;
}

// (RET 8).
void FGfScript::SetRoom(FRoomRuntime* InRoom, FGfScriptSequence* InSequence)
{
    Room = InRoom;
    Sequence = InSequence;
}

// . 0x0044ed03..0x0044ed08: once. 0x0044ed0b the routine; 0x0044ed10..0x0044ed4c:
// each non-null spawned object slot 19 (+0x4c) SetScene(0), slot 14 (+0x38) Stop, then
// IScriptEngine slot 13 Detach(i) -- the engine is not tested for null. 0x0044ed4e..0x0044ed7f:
// +0x3c = 0, the sequence's the routine, +0x38 = 0, both names freed, +0x45 = 1. The spawned
// objects themselves stay (the dtor deletes them).
void FGfScript::Teardown()
{
    if (bTornDown != 0)
        return;
    StopScriptSounds();
    int Index = 0;
    for (Index = 0; Index < SpawnedCount; ++Index)
    {
        FSceneScriptComponent* const Object = Spawned[Index];
        if (Object == nullptr)
            continue;
        Object->SetScene(nullptr);
        Spawned[Index]->Stop();
        Engine->Slot13Detach(Index);
    }
    Engine = nullptr;
    Sequence->ClearProps();
    Mirror = nullptr;
    malloc_strfree(&Name);
    malloc_strfree(&ScriptFile);
    bTornDown = 1;
}

// (RET 8). No engine: false. 0x0044f284..0x0044f294: IScriptEngine slot 16 the
// event; only START_SOUND (3). 0x0044f296..0x0044f2b4: Out +0x08 = StartMs, +0x0c = StartMs +
// DurationMs, +0x10 = event Flags bit 3. 0x0044f2b7..0x0044f2d7: slot 9 the event's entity
// (+0x1c); only a SOUND (6) that is not a reference; Out +0x11 = entity Flags bit 1.
// not "PlayerShipSound" and spawned: Out +0x00 = its +0x7c archetype,
// +0x04 = the object; true. Every early exit leaves the rest of Out as it was.
bool FGfScript::CollectSoundEvent(int EventId, FGfScriptSoundEvent* Out)
{
    if (Engine == nullptr)
        return false;
    FSceneScriptEventDesc const* const Event =
        static_cast<FSceneScriptEventDesc const*>(Engine->Slot16EventById(EventId));
    if (Event->Type != kSceneScriptTypeStartSound)
        return false;
    Out->StartMs = Event->StartMs;
    Out->EndMs = Event->StartMs + Event->DurationMs;
    Out->bLoop = static_cast<unsigned char>((static_cast<unsigned>(Event->Flags) >> 3) & 1);
    int const EntityIndex = Event->PrimaryIndex;
    FSceneScriptEntitySnap const* const Snap =
        static_cast<FSceneScriptEntitySnap const*>(Engine->Slot9Entity(EntityIndex));
    if (Snap->Type != kSceneScriptEntitySound)
        return false;
    if ((Snap->Flags & 1) != 0)
        return false;
    Out->bSpatial = static_cast<unsigned char>((static_cast<unsigned>(Snap->Flags) >> 1) & 1);
    if (_stricmp(Snap->TemplateName, "PlayerShipSound") == 0)
        return false;
    FSceneScriptComponent* const Object = Spawned[EntityIndex];
    if (Object == nullptr)
        return false;
    FSceneScriptSound* const Sound = static_cast<FSceneScriptSound*>(Object);
    Out->Arch = Sound->Arch;
    Out->Sound = Sound;
    return true;
}

// . No engine: nothing. 0x0044f328..0x0044f3f6: for each of the slot 14 events,
// slot 15 the event, a zeroed five-word local (+0x10 / +0x11 bytes cleared) filled by
// with the event's +0x14 id; accepted -> a new node {the five words, EventId,
// Handle 0, bStarted 0} appended.
void FGfScript::CollectSoundEvents()
{
    if (Engine == nullptr)
        return;
    int const Count = static_cast<int>(Engine->Slot14EventCount());
    int Index = 0;
    for (Index = 0; Index < Count; ++Index)
    {
        FSceneScriptEventDesc const* const Event =
            static_cast<FSceneScriptEventDesc const*>(Engine->Slot15EventByIndex(Index));
        FGfScriptSoundEvent Local;
        memset(&Local, 0, sizeof(Local));
        if (!CollectSoundEvent(Event->UniqueId, &Local))
            continue;
        Local.EventId = Event->UniqueId;
        Local.Handle = nullptr;
        Local.bStarted = 0;
        SoundEvents.push_back(Local);
    }
}

// . Every node not started: with an archetype, a non-streamer is acquired (slot 3,
// +0x0c); a streamer is started through the routine with offset 0 and {streamer, spatial,
// loop} and bAltStart 1, the handle into the node. Started either way (also without one).
void FGfScript::StartScriptSounds()
{
    std::list<FGfScriptSoundEvent>::iterator It = SoundEvents.begin();
    for (; It != SoundEvents.end(); ++It)
    {
        FGfScriptSoundEvent& Node = *It;
        if (Node.bStarted != 0)
            continue;
        Archetype::ISound* const Arch = static_cast<Archetype::ISound*>(Node.Arch);
        if (Arch != nullptr)
        {
            if (Arch->IsStreamer() == 0)
            {
                Arch->Acquire();
            }
            else
            {
                FScriptSoundKey Key;
                memset(&Key, 0, sizeof(Key));
                Key.Flags = MakeKeyFlags(Arch->IsStreamer(), Node.bSpatial, Node.bLoop);
                void* Handle = nullptr;
                PlayClientSoundArch(Arch, reinterpret_cast<unsigned*>(&Key), &Handle, 1);
                Node.Handle = Handle;
            }
        }
        Node.bStarted = 1;
    }
}

// . The clock (IScriptEngine slot 19; the engine is not tested), then over the nodes
// whose SceneScript_Sound answers the routine: the largest EndMs - clock above 0.
int FGfScript::GetSoundTailMs()
{
    int Tail = 0;
    int const Clock = Engine->Slot19GetClock();
    std::list<FGfScriptSoundEvent>::iterator It = SoundEvents.begin();
    for (; It != SoundEvents.end(); ++It)
    {
        if (It->Sound == nullptr)
            continue;
        if (!It->Sound->IsHoldingScript())
            continue;
        int const Remaining = It->EndMs - Clock;
        if (Remaining > Tail)
            Tail = Remaining;
    }
    return Tail;
}

// . Every started node: a non-streamer archetype released (slot 4, +0x10); a
// streamer's handle, when held, stopped (slot 24, +0x60), released (+0x08) and cleared. Not
// started any more (also without an archetype).
void FGfScript::StopScriptSounds()
{
    std::list<FGfScriptSoundEvent>::iterator It = SoundEvents.begin();
    for (; It != SoundEvents.end(); ++It)
    {
        FGfScriptSoundEvent& Node = *It;
        if (Node.bStarted == 0)
            continue;
        Archetype::ISound* const Arch = static_cast<Archetype::ISound*>(Node.Arch);
        if (Arch != nullptr)
        {
            if (Arch->IsStreamer() == 0)
            {
                Arch->Release();
            }
            else if (Node.Handle != nullptr)
            {
                ISoundSource* const Handle = static_cast<ISoundSource*>(Node.Handle);
                Handle->Stop();
                Handle->Release();
                Node.Handle = nullptr;
            }
        }
        Node.bStarted = 0;
    }
}

// (RET 4). A null handle, nothing; otherwise the first node holding it is unlinked
// and freed.
void FGfScript::RemoveSoundEvent(void* Handle)
{
    if (Handle == nullptr)
        return;
    std::list<FGfScriptSoundEvent>::iterator It = SoundEvents.begin();
    for (; It != SoundEvents.end(); ++It)
    {
        if (It->Handle == Handle)
        {
            SoundEvents.erase(It);
            return;
        }
    }
}

// (RET 8). 0x0044f625..0x0044f642: the first node of EventId. 0x0044f6ea..
// held handle -> the routine(Arch, Offset) on it (result dropped), bStarted = 0,
// the handle answered. Otherwise 0x0044f644..0x0044f6d9: the routine into a local and, when
// accepted, the routine with {Offset, streamer / spatial / loop} and bAltStart 0 -- the new
// source is answered (null when the routine refused), not stored in any node.
void* FGfScript::PlayStreamedSoundEvent(int EventId, int Offset)
{
    void* Result = nullptr;
    std::list<FGfScriptSoundEvent>::iterator It = SoundEvents.begin();
    for (; It != SoundEvents.end(); ++It)
    {
        if (It->EventId != EventId)
            continue;
        if (It->Handle != nullptr)
        {
            ISoundSource* const Handle = static_cast<ISoundSource*>(It->Handle);
            RestartClientStreamedSound(Handle, It->Arch, Offset);
            It->bStarted = 0;
            return Handle;
        }
        break;
    }

    FGfScriptSoundEvent Local;
    memset(&Local, 0, sizeof(Local));
    if (CollectSoundEvent(EventId, &Local))
    {
        Archetype::ISound* const Arch = static_cast<Archetype::ISound*>(Local.Arch);
        FScriptSoundKey Key;
        memset(&Key, 0, sizeof(Key));
        Key.Flags = MakeKeyFlags(Arch->IsStreamer(), Local.bSpatial, Local.bLoop);
        Key.Offset = static_cast<unsigned>(Offset);
        PlayClientSoundArch(Arch, reinterpret_cast<unsigned*>(&Key), &Result, 0);
    }
    return Result;
}

// (RET 0x2c). The arguments into +0x3c, +0x50, +0x54, +0x55, +0x56, +0x58, +0x5c,
// +0x5d, +0x60; +0x5e = 0; malloc_strcpy of the name into +0xa0 (not freed first) and of the file
// into +0xa4 (freed first). With an engine: +0x40 = slot 17, the routine, the routine,
// , then slot 20 (thiscall) SetLoop(+0x54), slot 22 Start(0), slot 26 Tick(0).
void FGfScript::Start(IScriptEngine* InEngine, char const* InScriptFile, int InBindMode,
                      unsigned char bInLoop, unsigned char bInField55,
                      unsigned char bInApplySpatial, int InPriority, unsigned char bInFade,
                      char const* InName, unsigned char bInPropsToRoom,
                      unsigned char bInSoundsUseFade)
{
    Engine = InEngine;
    BindMode = InBindMode;
    Field_0x55 = bInField55;
    bApplySpatial = bInApplySpatial;
    bLoop = bInLoop;
    bPropsToRoom = bInPropsToRoom;
    bFade = bInFade;
    Priority = InPriority;
    bFadeStarted = 0;
    bSoundsUseFade = bInSoundsUseFade;
    malloc_strcpy(&Name, InName);
    malloc_strfree(&ScriptFile);
    malloc_strcpy(&ScriptFile, InScriptFile);
    if (Engine == nullptr)
        return;
    Duration = Engine->Slot17GetDuration();
    Spawn();
    CollectSoundEvents();
    StartScriptSounds();
    Engine->Slot20SetLoop(bLoop);
    Engine->Slot22Start(0);
    Engine->Slot26Tick(0);
}

// (RET 4). No engine: finished. Not finished yet:
//   running (+0x44 clear) -- 0x0044f817..0x0044f845: slot 26 Tick(the routine() + Ms), the
//     clock (slot 19); not looping and the clock at or past +0x40 (unsigned compare, JC)
//     finishes. 0x0044f849..0x0044f8ce: with +0x5d and no fade yet, the remaining seconds
//     (Duration - clock as an unsigned 64-bit FILD, * 0.001, stored as float): only when they
//     are not above the fade duration the field (`TEST AH,0x41` + JP skips greater or
//     unordered) -- at or below 0 (`TEST AH,0x41` + JNZ) the routine(1.0), else
// (1.0, 1.0 / seconds); fade started.
//   paused (+0x44 set) -- 0x0044f8d0..0x0044f901: slot 26 Tick(Duration - clock); with +0x5d
// (1.0) and fade started; finished, +0x44 cleared.
// finished -> slot 26 Tick(100) and, with +0x5d, the routine(1.0) and
// fade started. Answers +0x5f.
bool FGfScript::Update(int Ms)
{
    if (Engine == nullptr)
    {
        bFinished = 1;
        return bFinished != 0;
    }
    if (bFinished == 0)
    {
        if (bPaused == 0)
        {
            Engine->Slot26Tick(GetSoundTailMs() + Ms);
            int const Clock = Engine->Slot19GetClock();
            if (bLoop == 0 && static_cast<unsigned>(Clock) >= static_cast<unsigned>(Duration))
                bFinished = 1;
            if (bFade != 0 && bFadeStarted == 0)
            {
                unsigned const RemainingMs =
                    static_cast<unsigned>(Duration) - static_cast<unsigned>(Clock);
                // FILD / FMUL 0.001 / FST float [ESP+0x10] / FCOMP: the first compare is on the
                // unrounded product still on the FPU stack; the float copy feeds the rest.
                double const SecondsExact = static_cast<double>(RemainingMs) * 0.001;
                float const Seconds = static_cast<float>(SecondsExact);
                if (!(SecondsExact > static_cast<double>(GetFadeDuration())))
                {
                    if (Seconds > 0.0f)
                        SetFadeTarget(1.0f, static_cast<float>(1.0 / static_cast<double>(Seconds)));
                    else
                        SetFadeImmediate(1.0f);
                    bFadeStarted = 1;
                }
            }
        }
        else
        {
            Engine->Slot26Tick(Duration - Engine->Slot19GetClock());
            if (bFade != 0)
            {
                SetFadeImmediate(1.0f);
                bFadeStarted = 1;
            }
            bFinished = 1;
            bPaused = 0;
        }
    }
    if (bFinished != 0)
    {
        Engine->Slot26Tick(100);
        if (bFade != 0)
        {
            SetFadeImmediate(1.0f);
            bFadeStarted = 1;
        }
    }
    return bFinished != 0;
}

// . With an engine: slot 23 Stop, slot 22 Start(0).
void FGfScript::Restart()
{
    if (Engine == nullptr)
        return;
    Engine->Slot23Stop();
    Engine->Slot22Start(0);
}

// (RET 4). Identity slot 18 (+0x48) on every non-null spawned object.
void FGfScript::DrawEffects(ICamera* Camera)
{
    int Index = 0;
    for (Index = 0; Index < SpawnedCount; ++Index)
    {
        if (Spawned[Index] != nullptr)
            Spawned[Index]->DrawEffects(Camera);
    }
}

// (thiscall, RET 8). 0x0044f9a3..0x0044f9bb: Spawned[Index] slot 17 (+0x44), then
// Sequence (+0x34) the routine(instance, bDetached). No null check on the entity or the
// sequence -- the callers only get here for an index the routine / the routine just used.
void FGfScript::SetEntityPropDetached(int Index, unsigned char bDetached)
{
    void* Instance = Spawned[Index]->GetEngineInstance();
    Sequence->SetPropDetached(Instance, bDetached);
}

// .
void FGfScript::Pause()
{
    bPaused = 1;
}

// (RET 4). 0x0044f9e6..0x0044f9f4: the remaining ms (Duration - slot 19); none,
// nothing. 0x0044f9f6..0x0044fa37: ftol(floor(remaining (unsigned) * Scale + 0.5)), one more
// unless Scale < 1.0 (`TEST AH,1` + JNZ: less or unordered keeps it); the routine with that.
void FGfScript::Advance(float Scale)
{
    unsigned const RemainingMs =
        static_cast<unsigned>(Duration) - static_cast<unsigned>(Engine->Slot19GetClock());
    if (RemainingMs == 0)
        return;
    double const Scaled =
        static_cast<double>(RemainingMs) * static_cast<double>(Scale) + 0.5;
    int Ms = static_cast<int>(floor(Scaled));
    if (!(static_cast<double>(Scale) < 1.0))
        ++Ms;
    Update(Ms);
}

// =========================================================================================
// FGfMirrorDatabase
// =========================================================================================

// .
FGfMirrorDatabase::FGfMirrorDatabase()
{
}

// .
FGfMirrorDatabase::~FGfMirrorDatabase()
{
    Entries.clear();
}

// (RET 8). A null name is "" (the field's empty tail); the node goes before the
// sentinel.
void FGfMirrorDatabase::Add(FCharacterObject* Object, char const* ObjName)
{
    FEntry Entry;
    Entry.Object = Object;
    Entry.Name = ObjName != nullptr ? ObjName : "";
    Entries.push_back(Entry);
}

// . head->next != head ? node +0x08 : 0.
FCharacterObject* FGfMirrorDatabase::First() const
{
    if (Entries.empty())
        return nullptr;
    return Entries.front().Object;
}

// (RET 4). stricmp of each node's c_str (null storage reads as "") with ObjName.
FCharacterObject* FGfMirrorDatabase::Find(char const* ObjName) const
{
    std::list<FEntry>::const_iterator It = Entries.begin();
    for (; It != Entries.end(); ++It)
    {
        if (_stricmp(It->Name.c_str(), ObjName) == 0)
            return It->Object;
    }
    return nullptr;
}

// =========================================================================================
// FGfScriptEntry
// =========================================================================================

// . Script, Engine, File = 0; Frame = origin, identity; Kind = 0.
FGfScriptEntry::FGfScriptEntry()
    : Script(nullptr),
      Engine(nullptr),
      ScriptFile(nullptr),
      Kind(0)
{
    SetIdentityCsys(Frame);
}

// . A script is torn down (the routine) first; the file freed, the engine
// destroyed (SceneProgramDestroy), the script deleted (the routine + free); everything back to
// the ctor's values.
FGfScriptEntry::~FGfScriptEntry()
{
    if (Script != nullptr)
        Script->Teardown();
    malloc_strfree(&ScriptFile);
    SceneProgramDestroy(&Engine);
    if (Script != nullptr)
        delete Script;
    Script = nullptr;
    Engine = nullptr;
    ScriptFile = nullptr;
    SetIdentityCsys(Frame);
    Kind = 0;
}

// (RET 8). 0x00454839..0x00454876: a script already there -> SceneProgramDestroy(
// &Engine), the script deleted, Script = 0, Engine = SceneProgramLoad(ScriptFile). Then
// operator new(0xb4) + the routine into +0x00, the routine,
// (Room, Seq), the entry's frame copied over the script's (REP MOVSD 0xc).
FGfScript* FGfScriptEntry::Create(FRoomRuntime* InRoom, FGfScriptSequence* InSeq)
{
    if (Script != nullptr)
    {
        SceneProgramDestroy(&Engine);
        delete Script;
        Script = nullptr;
        Engine = SceneProgramLoad(ScriptFile);
    }
    Script = new FGfScript();
    Script->Init();
    Script->SetRoom(InRoom, InSeq);
    Script->Frame = Frame;
    return Script;
}

// =========================================================================================
// FGfScriptSequence
// =========================================================================================

// . The two FmtStr blocks, the mirror database, the two lists (sentinels of 0x1c);
// then 0x00454a19..0x00454b2d the scalar defaults: +0x00 = 0, +0x06 / +0x08 / +0x09 / +0x0e /
// +0x0f / +0x15 / +0x2c = 1, the other flag bytes 0, +0x10 / +0x18..+0x28 = 0, the player frame
// origin / identity (the routine), +0x64 / +0x6c = 0 (+0x68 is not written), +0xb0..+0xbc = 0,
// +0xc0 = -1, +0xc4 = 0, +0xc8..+0xd0 = 0, both FmtStr destroyed and zeroed, +0x104..+0x110 and
// +0x120 = 0. 0x00454b33..0x00454b99: both lists emptied again (they are already empty).
FGfScriptSequence::FGfScriptSequence()
    : BindMode(0),
      bLoop(0),
      Field_0x05(0),
      bLetterbox(1),
      bFade(0),
      bApplySpatial(1),
      bRestoreCamera(1),
      bDuckAmbient(0),
      bDuckMusic(0),
      bDuckSlow(0),
      bSoundsUseFade(0),
      bRestorePlayer(1),
      bHoldTime(1),
      Priority(0),
      bPropsToRoom(0),
      Field_0x15(1),
      Field_0x16(0),
      Callback(nullptr),
      CallbackA(nullptr),
      CallbackB(nullptr),
      RtcId(0),
      Location(0),
      bStopped(1),
      bPlaying(0),
      bSkip(0),
      bFindable(0),
      bPlayerSaved(0),
      PlayerField64(0.0f),
      PlayerDeformInstance(nullptr),
      PlayerCharacter(nullptr),
      CameraHalfFovX(0.0f),
      CameraZNear(0.0f),
      CameraZFar(0.0f),
      bHasAmbient(0),
      bHasMusic(0),
      Room(nullptr),
      Entries(nullptr),
      Capacity(0),
      Count(0),
      Current(-1),
      bActive(0),
      OfferType(0),
      OfferArg(0),
      OfferMissionId(0),
      bOfferShown(0),
      bOfferAnswered(0),
      bOfferAccepted(0),
      OfferDialog(nullptr),
      bInfoShown(0),
      bInfoClosed(0),
      InfoDialog(nullptr),
      Name(nullptr)
{
    SetIdentityCsys(PlayerCsys);
    int Index = 0;
    for (Index = 0; Index < 3; ++Index)
        CameraPos[Index] = 0.0f;
    for (Index = 0; Index < 9; ++Index)
        CameraOrient[Index] = 0.0f;
}

// . 0x00454bd2..0x00454be5: not stopped -> the routine(1). 0x00454bea..
// the entries (vector delete with the branch), +0xb4 = 0. Both FmtStr destroyed;
// +0x108 deleted through its slot 0 (flag 1) and zeroed; +0x120 freed. 0x00454c58..0x00454cb8:
// every prop, then every character, deleted through RoomObject slot 0 (flag 1) and its node
// value zeroed; both lists emptied and their sentinels freed; the mirror database cleared
// (the routine).
FGfScriptSequence::~FGfScriptSequence()
{
    if (bStopped == 0)
        StopPlayback(1);
    delete[] Entries;
    Entries = nullptr;
    DialogTextA.destroy();
    DialogTextB.destroy();
    // the offer dialog through its slot 0 with 1, then +0x108 = 0.
    if (OfferDialog != nullptr)
        delete OfferDialog;
    OfferDialog = nullptr;
    malloc_strfree(&Name);
    std::list<FProp>::iterator PropIt = Props.begin();
    for (; PropIt != Props.end(); ++PropIt)
    {
        if (PropIt->Prop != nullptr)
            delete PropIt->Prop;
        PropIt->Prop = nullptr;
    }
    Props.clear();
    std::list<FCharacter>::iterator CharIt = Characters.begin();
    for (; CharIt != Characters.end(); ++CharIt)
    {
        if (CharIt->Character != nullptr)
            delete CharIt->Character;
        CharIt->Character = nullptr;
    }
    Characters.clear();
}

// (RET 4). malloc_strfree(+0x120), malloc_strcpy(+0x120, InName).
void FGfScriptSequence::SetName(char const* InName)
{
    malloc_strfree(&Name);
    malloc_strcpy(&Name, InName);
}

// (RET 0x10). Entry Count: malloc_strcpy of File into +0x08 (the old string is
// not freed), +0x3c = Kind, the frame copied (REP MOVSD 0xc); Engine null -> SceneProgramLoad(
// the copied file). +0x04 = the engine, the field = its slot 17 (the Gameflow arrival
// caption's delay), Count + 1. No check against +0xb8 (baseline: a 0x21st script writes past
// the 0x20 entries -- no caller adds that many).
void FGfScriptSequence::AddScript(char const* File, int InKind, Csys const* InFrame,
                                  IScriptEngine* InEngine)
{
    FGfScriptEntry& Entry = Entries[Count];
    malloc_strcpy(&Entry.ScriptFile, File);
    Entry.Kind = InKind;
    Entry.Frame = *InFrame;
    IScriptEngine* Engine = InEngine;
    if (Engine == nullptr)
        Engine = SceneProgramLoad(Entry.ScriptFile);
    Entry.Engine = Engine;
    SetGameflowArrivalDelayMs(static_cast<unsigned>(Engine->Slot17GetDuration()));
    ++Count;
}

// (RET 0x14). +0xc8 / +0xcc / +0xd0, then FmtStr::operator= into +0xd4 and +0xec.
void FGfScriptSequence::SetDialog(int A, int B, int C, FmtStr const& TextA, FmtStr const& TextB)
{
    OfferType = A;
    OfferArg = B;
    OfferMissionId = C;
    DialogTextA = TextA;
    DialogTextB = TextB;
}

// . RefreshLoadingProgress, +0x2d = 1, the routine, the routine(0), +0xc4 = 1.
void FGfScriptSequence::Play()
{
    RefreshLoadingProgress();
    bPlaying = 1;
    Setup();
    StartEntry(0);
    bActive = 1;
}

// (RET 4).
// RefreshLoadingProgress; +0xc0 = Index; entry Index's the routine(
//     Room, this); the new script's +0x38 = &+0x114.
// the fade flag is +0x07, forced on when +0x28 (a location to move to)
//     is set and the routine runs past the last entry; the routine with the entry's engine and
//     file and +0x00, +0x04, +0x05, +0x08, +0x10, that flag, +0x120, +0x14, +0x0d.
// +0x16 && the field (the branch's latch) && +0x2d && +0x15 ->
//     +0x2e = 1.
// +0x0f -> the routine.
// the player not saved yet and the script bound one (+0x61): saved --
//     its frame (+0x64 -> +0x34), +0x94 -> +0x64, +0x98 -> +0x68, +0x9c -> +0x6c.
// kind 1 -- the offer dialog the routine; kind 4 -- the info box.
void FGfScriptSequence::StartEntry(int Index)
{
    RefreshLoadingProgress();
    Current = Index;
    FGfScriptEntry& Entry = Entries[Index];
    Entry.Create(Room, this);
    Entries[Current].Script->Mirror = &Mirror;

    unsigned char bScriptFade = bFade;
    if (Location != 0 && NextEntryIndex() >= Count)
        bScriptFade = 1;

    FGfScriptEntry& Started = Entries[Current];
    Started.Script->Start(Started.Engine, Started.ScriptFile, BindMode, bLoop, Field_0x05,
                          bApplySpatial, Priority, bScriptFade, Name, bPropsToRoom,
                          bSoundsUseFade);

    if (Field_0x16 != 0 && GfScriptSequenceSkipLatch() && bPlaying != 0 && Field_0x15 != 0)
        bSkip = 1;
    if (bHoldTime != 0)
        HoldGameflowTime();

    FGfScript* const Script = Entries[Current].Script;
    if (bPlayerSaved == 0 && Script->bPlayerActor != 0)
    {
        bPlayerSaved = 1;
        PlayerCsys = Script->PlayerCsys;
        PlayerField64 = Script->PlayerField94;
        PlayerDeformInstance = Script->PlayerDeformInstance;
        PlayerCharacter = Script->PlayerCharacter;
    }

    // a kind-1 entry (an offer). The entries after it are scanned while
    // their kinds are 2, 3, 4 or 6 -- 2 and 4 mark an Accept branch, 3 a Decline branch, 6
    // neither; any other kind ends the scan. No branch found (or no entry after it) counts as
    // both. Unless the offer is already up (+0x104): the routine, then with +0x06 the
    // letterbox out (the routine(0, 3)) and the software cursor on (the routine(1));
    // +0x104 = 1, +0x105 = +0x106 = 0.
    if (Entries[Current].Kind == 1)
    {
        unsigned char bAcceptBranch = 0;
        unsigned char bDeclineBranch = 0;
        int Next = Current + 1;
        if (Next < Count)
        {
            for (; Next < Count; ++Next)
            {
                int const NextKind = Entries[Next].Kind;
                if (NextKind == 2 || NextKind == 4)
                    bAcceptBranch = 1;
                else if (NextKind == 3)
                    bDeclineBranch = 1;
                else if (NextKind != 6)
                    break;
            }
            if (bAcceptBranch == 0 && bDeclineBranch == 0)
            {
                bAcceptBranch = 1;
                bDeclineBranch = 1;
            }
        }
        else
        {
            bAcceptBranch = 1;
            bDeclineBranch = 1;
        }
        if (bOfferShown == 0)
        {
            ShowOfferDialog(bAcceptBranch, bDeclineBranch);
            if (bLetterbox != 0)
            {
                SetLetterboxTarget(0.0f, 3.0f);
                SetClientMouseCursorDrawFlag(1);
            }
            bOfferShown = 1;
            bOfferAnswered = 0;
            bOfferAccepted = 0;
        }
    }

    // a kind-4 entry (an info box), unless one is already up (+0x10c):
    // the nav map focused on the place the info text names (the routine; the routine with
    // bZone = flag bit 0x20), NN_OfferDialog "ScriptSequence_InfoDialog" into +0x110 opened
    // with type 4, the +0xec text, a single button and the branch; with +0x06 the letterbox
    // out and the software cursor on; +0x10c = 1, +0x10d = 0.
    if (Entries[Current].Kind == 4 && bInfoShown == 0)
    {
        unsigned Id = 0;
        unsigned char Flags = 0;
        if (KnowledgeMapLookupFmtStr(&DialogTextB, &Id, &Flags))
            NavMapRequestFocus(Id, (Flags & 0x20) != 0 ? 1 : 0);
        InfoDialog = static_cast<NnOfferDialog*>(
            WinFactoryCreate("ScriptSequence_InfoDialog", "NN_OfferDialog", nullptr, nullptr));
        InfoDialog->Open(4, &DialogTextB, true, &FGfScriptSequence::OnInfoDialogClosed, this);
        if (bLetterbox != 0)
        {
            SetLetterboxTarget(0.0f, 3.0f);
            SetClientMouseCursorDrawFlag(1);
        }
        bInfoShown = 1;
        bInfoClosed = 0;
    }
}

// .
// the room camera (+0xb0 -> +0x500): slot 4 (+0x10) the transform
//     (orientation, then position at +0x24) -> +0x70 position, +0x7c orientation; slot 6
//     (+0x18) -> +0xa0, slot 8 (+0x20) -> +0xa4, slot 9 (+0x24) -> +0xa8.
// +0xac = +0xad = 0; per entry the routine(engine, 1),
// (engine, &ambient, &music), the routine(engine, 1); either found sets its
//     byte.
// RefreshLoadingProgress; CharacterInfo the routine.
// +0x0a without a script ambience -- the room ambience ducks: with
//     +0x0c at the fixed rate (the routine(3, -20, 2.5), Gf fade target -20 at 2.5 dB/s), else
//     by the ducking config (the routine(3, 1e10, 1e10), Gf fade to down_by over down_time).
// +0x0b without a script music -- the routine the same way.
// +0x06 -> letterbox in (the routine(1.0, 3.0)), hotspots locked
//     (the routine), software cursor off (the routine(0)).
//   +0x2c = 0.
void FGfScriptSequence::Setup()
{
    RefreshLoadingProgress();
    ICamera* const Camera = GetRoomRuntimeCamera(Room);
    float const* const Transform = Camera->GetOrientation();
    int Index = 0;
    for (Index = 0; Index < 3; ++Index)
        CameraPos[Index] = Transform[9 + Index];
    for (Index = 0; Index < 9; ++Index)
        CameraOrient[Index] = Transform[Index];
    CameraHalfFovX = Camera->GetHalfFovXDeg();
    CameraZNear = Camera->GetZNear();
    CameraZFar = Camera->GetZFar();

    bHasAmbient = 0;
    bHasMusic = 0;
    for (Index = 0; Index < Count; ++Index)
    {
        IScriptEngine* const Engine = Entries[Index].Engine;
        unsigned char bAmbient = 0;
        unsigned char bMusic = 0;
        GfScriptCharacterAnimations(Engine, true);
        GfScriptFindBackgroundAudio(Engine, &bAmbient, &bMusic);
        GfScriptTouchEntities(Engine, true);
        if (bAmbient != 0)
            bHasAmbient = 1;
        if (bMusic != 0)
            bHasMusic = 1;
    }

    RefreshLoadingProgress();
    GetCharacterInfoDb()->FlushQueuedScripts();

    if (bDuckAmbient != 0 && bHasAmbient == 0)
    {
        if (bDuckSlow != 0)
        {
            FadeClientBackgroundAmbient(3, kSequenceDuckDb, kSequenceDuckRate);
            SetClientAudioFadeTargetRate(kSequenceDuckDb, kSequenceDuckRate);
        }
        else
        {
            FadeClientBackgroundAmbient(3, kUseDuckingConfig, kUseDuckingConfig);
            float DownBy = 0.0f;
            float DownTime = 0.0f;
            GetClientDuckingConfig(&DownBy, nullptr, nullptr);
            GetClientDuckingConfig(nullptr, &DownTime, nullptr);
            // target down_by; rate |down_by - current| / down_time, or
            // 1e6 when down_time is not above 0 -- the routine's body with those seconds.
            SetClientAudioFade(DownBy, DownTime > 0.0f ? DownTime : 0.0f);
        }
    }
    if (bDuckMusic != 0 && bHasMusic == 0)
    {
        if (bDuckSlow != 0)
            FadeClientBackgroundMusic(3, kSequenceDuckDb, kSequenceDuckRate);
        else
            FadeClientBackgroundMusic(3, kUseDuckingConfig, kUseDuckingConfig);
    }
    if (bLetterbox != 0)
    {
        SetLetterboxTarget(1.0f, 3.0f);
        LockHotspotInput();
        SetClientMouseCursorDrawFlag(0);
    }
    bStopped = 0;
}

// (RET 4). Once (+0x2c).
// a location to move to (+0x28): the spaceflight location ->
// ; else the room loaded (the routine) -> its audio (the routine) and a
//     change queued without audio (the routine(loc, 0, 0)), otherwise queued with audio
//     (the routine(loc, 0, 1)). Then the routine(1.0), the routine; "moved" is remembered.
// +0x0e, not moved, the player saved -> NeuronRuntime::Deform slot 19 (+0x4c)
//     / slot 20 (+0x50) with +0x68 and the frame's position / orientation, the character's
//     slot 3 (+0x0c) with +0x64.
// +0x06, not moved -> letterbox out (the routine(0, 3)), hotspots
//     unlocked (the routine), software cursor on (the routine(1)), the room's the routine.
// not moved: +0x0a -> ambience back to 0 dB (fixed rate with +0x0c,
//     else over down_time), Gf fade target 0; +0x0b -> music back to 0 dB.
// +0x09, not moved -> the room camera restored (the routine /
// / the routine / the routine with +0x7c/+0x70, +0xa0, +0xa4, +0xa8).
// per entry the routine(engine, 0), the routine(engine, 0).
//   +0x2c = 1; +0x24 -> IServer slot 69 (+0x114)(session, +0x24); +0x18 -> the callback.
void FGfScriptSequence::StopPlayback(unsigned char bArg)
{
    if (bStopped != 0)
        return;
    RefreshLoadingProgress();

    bool bMoved = false;
    if (Location != 0)
    {
        if (Location == SpaceflightLocationID.Id)
        {
            RequestGameflowMissionBriefing();
        }
        else if (IsClientLocationRoomLoaded(Location))
        {
            ApplyClientLocationAudio(Location);
            QueueClientLocationChange(Location, 0.0f, false);
        }
        else
        {
            QueueClientLocationChange(Location, 0.0f, true);
        }
        SetFadeImmediate(1.0f);
        HoldGameflowTime();
        bMoved = true;
    }

    if (bRestorePlayer != 0 && !bMoved && bPlayerSaved != 0)
    {
        // NeuronRuntime::Deform slot 19 (+0x4c)(+0x68, &+0x34 position) and
        // slot 20 (+0x50)(+0x68, &+0x40 orientation), then the character's (+0x6c) slot 3
        // (+0x0c) with +0x64. Results unused; the character is not null-tested.
        NeuronRuntime::Deform->SetPosePos(PlayerDeformInstance, &PlayerCsys.Pos.x);
        NeuronRuntime::Deform->SetPoseOri(PlayerDeformInstance, &PlayerCsys.Orient);
        PlayerCharacter->SetFloat1C(PlayerField64);
    }

    if (bLetterbox != 0 && !bMoved)
    {
        SetLetterboxTarget(0.0f, 3.0f);
        UnlockHotspotInput();
        SetClientMouseCursorDrawFlag(1);
        RoomControlRequestCharacterScripts(Room);
    }

    bool const bRestoreAmbient = bDuckAmbient != 0 && !bMoved;
    bool const bRestoreMusic = bDuckMusic != 0 && !bMoved;
    if (bRestoreAmbient)
    {
        if (bDuckSlow != 0)
        {
            FadeClientBackgroundAmbient(3, 0.0f, kSequenceDuckRate);
            SetClientAudioFadeTargetRate(0.0f, kSequenceDuckRate);
        }
        else
        {
            FadeClientBackgroundAmbient(3, 0.0f, kUseDuckingConfig);
            float DownTime = 0.0f;
            GetClientDuckingConfig(nullptr, &DownTime, nullptr);
            SetClientAudioFade(0.0f, DownTime > 0.0f ? DownTime : 0.0f);
        }
    }
    if (bRestoreMusic)
    {
        if (bDuckSlow != 0)
            FadeClientBackgroundMusic(3, 0.0f, kSequenceDuckRate);
        else
            FadeClientBackgroundMusic(3, 0.0f, kUseDuckingConfig);
    }

    if (bRestoreCamera != 0 && !bMoved)
    {
        FRoomCamera* const Camera = GetRoomRuntimeRoomCamera(Room);
        float Transform[12];
        int Index = 0;
        for (Index = 0; Index < 9; ++Index)
            Transform[Index] = CameraOrient[Index];
        for (Index = 0; Index < 3; ++Index)
            Transform[9 + Index] = CameraPos[Index];
        RoomCameraSetTransform(Camera, Transform);
        RoomCameraSetHalfFovX(Camera, CameraHalfFovX);
        RoomCameraSetZNear(Camera, CameraZNear);
        RoomCameraSetZFar(Camera, CameraZFar);
    }

    int Index = 0;
    for (Index = 0; Index < Count; ++Index)
    {
        IScriptEngine* const Engine = Entries[Index].Engine;
        GfScriptTouchEntities(Engine, false);
        GfScriptCharacterAnimations(Engine, false);
    }

    bStopped = 1;
    // +0x24 non-zero -> IServer slot 69 RTCDone(the field, +0x24)
    // on the field (not null-tested).
    if (RtcId != 0)
        G_Server->RTCDone(GetPlayerConnectionId(), RtcId);
    if (Callback != nullptr)
        Callback(CallbackA, CallbackB, bArg);
}

// (thiscall). Caller: IClient slot 114 (the branch, 0x0053e38d) on the field.
void FGfScriptSequence::OnMissionOfferAnswered()
{
    // the field = 0.
    SetMissionOfferSequencebaseline(nullptr);
    // with the offer up (+0x104): +0x108 through its slot 0 with 1 and
    // zeroed; +0x06 -> letterbox in (the routine(1.0, 3.0)), software cursor off
    // (the routine(0)); the current script run out (the routine); +0x104 = 0.
    if (bOfferShown == 0)
        return;
    if (OfferDialog != nullptr)
        delete OfferDialog;
    OfferDialog = nullptr;
    if (bLetterbox != 0)
    {
        SetLetterboxTarget(1.0f, 3.0f);
        SetClientMouseCursorDrawFlag(0);
    }
    Entries[Current].Script->Pause();
    bOfferShown = 0;
}

// (thiscall, RET 8).
void FGfScriptSequence::ShowOfferDialog(unsigned char bHasAcceptBranch,
                                        unsigned char bHasDeclineBranch)
{
    (void)bHasAcceptBranch;
    // the routine("ScriptSequence_OfferDialog", "NN_OfferDialog", 0, 0)
    // -> +0x108 (not null-tested).
    OfferDialog = static_cast<NnOfferDialog*>(
        WinFactoryCreate("ScriptSequence_OfferDialog", "NN_OfferDialog", nullptr, nullptr));

    // the dialog type and text by +0xc8: 1 -> type 1, +0xd4; 2 -> type
    // 2, +0xec; 3 -> type 0, +0xd4; 4 -> type 3, +0xd4; anything else -> type 0, +0xd4 and a
    // WARNING FDUMP (ScriptSequence.cpp line 0x38b).
    FmtStr Text;
    int Type = 0;
    if (OfferType == 1)
    {
        Type = 1;
        Text = DialogTextA;
    }
    else if (OfferType == 2)
    {
        Type = 2;
        Text = DialogTextB;
    }
    else if (OfferType == 3)
    {
        Type = 0;
        Text = DialogTextA;
    }
    else if (OfferType == 4)
    {
        Type = 3;
        Text = DialogTextA;
    }
    else
    {
        Text = DialogTextA;
        FDUMP(0x100002, "%s(%d) : *** WARNING: Invalid offer type %d\n", GScriptSequenceFile,
              0x38b, OfferType);
    }

    // the routine(type, &text, no Decline branch, the branch, this).
    OfferDialog->Open(Type, &Text, bHasDeclineBranch == 0, &FGfScriptSequence::OnOfferAnswer,
                      this);
    // the local FmtStr destroyed.
}

// (cdecl).
void __cdecl FGfScriptSequence::OnOfferAnswer(void* Context, int Answer)
{
    FGfScriptSequence* const Seq = static_cast<FGfScriptSequence*>(Context);
    if (Answer == 0)
    {
        // Accept, first answer (+0x105 clear): +0x105 = +0x106 = 1.
        // Unless this is a multiplayer group offer (type 3) the server already answered:
        // multiplayer type 3 -> the field = this; IServer slot 43 MissionResponse(+0xcc,
        // +0xd0, +0x106, the field) on the field (not null-tested).
        if (Seq->bOfferAnswered == 0)
        {
            Seq->bOfferAnswered = 1;
            Seq->bOfferAccepted = 1;
            if (SinglePlayer() || Seq->OfferType != 3 || GetMissionOfferAnsweredbaseline() == 0)
            {
                if (!SinglePlayer() && Seq->OfferType == 3)
                    SetMissionOfferSequencebaseline(Seq);
                G_Server->MissionResponse(static_cast<unsigned>(Seq->OfferArg),
                                          static_cast<unsigned long>(Seq->OfferMissionId),
                                          Seq->bOfferAccepted != 0, GetPlayerConnectionId());
            }
        }
        // a multiplayer group offer the server has not answered yet
        // stays open (the routine closes it later); otherwise on to the close.
        if (!SinglePlayer() && Seq->OfferType == 3 && GetMissionOfferAnsweredbaseline() == 0)
            return;
    }
    else if (Answer == 1)
    {
        // Decline / Close, first answer: +0x105 = 1, +0x106 = 0,
        // MissionResponse(+0xcc, +0xd0, 0, the field).
        if (Seq->bOfferAnswered == 0)
        {
            Seq->bOfferAnswered = 1;
            Seq->bOfferAccepted = 0;
            G_Server->MissionResponse(static_cast<unsigned>(Seq->OfferArg),
                                      static_cast<unsigned long>(Seq->OfferMissionId), false,
                                      GetPlayerConnectionId());
        }
    }
    else
    {
        return;
    }

    // with the offer up (+0x104): +0x108 through its slot 0 with 1 and
    // zeroed; +0x06 -> letterbox in (the routine(1.0, 3.0)), software cursor off
    // (the routine(0)); the current script run out (the routine); +0x104 = 0.
    if (Seq->bOfferShown == 0)
        return;
    if (Seq->OfferDialog != nullptr)
        delete Seq->OfferDialog;
    Seq->OfferDialog = nullptr;
    if (Seq->bLetterbox != 0)
    {
        SetLetterboxTarget(1.0f, 3.0f);
        SetClientMouseCursorDrawFlag(0);
    }
    Seq->Entries[Seq->Current].Script->Pause();
    Seq->bOfferShown = 0;
}

// (cdecl). Answer is not read.
void __cdecl FGfScriptSequence::OnInfoDialogClosed(void* Context, int Answer)
{
    (void)Answer;
    FGfScriptSequence* const Seq = static_cast<FGfScriptSequence*>(Context);
    // +0x10d = 1; with the info box up (+0x10c): +0x110 through its
    // slot 0 with 1 and zeroed; +0x06 -> letterbox in, software cursor off; the current script
    // run out (the routine); +0x10c = 0.
    Seq->bInfoClosed = 1;
    if (Seq->bInfoShown == 0)
        return;
    if (Seq->InfoDialog != nullptr)
        delete Seq->InfoDialog;
    Seq->InfoDialog = nullptr;
    if (Seq->bLetterbox != 0)
    {
        SetLetterboxTarget(1.0f, 3.0f);
        SetClientMouseCursorDrawFlag(0);
    }
    Seq->Entries[Seq->Current].Script->Pause();
    Seq->bInfoShown = 0;
}

// (RET 8). A null name is ""; the node {Prop, std::string} before the sentinel.
void FGfScriptSequence::AddProp(FObjectProp* Prop, char const* PropName)
{
    FProp Entry;
    Entry.Prop = Prop;
    Entry.Name = PropName != nullptr ? PropName : "";
    Props.push_back(Entry);
}

// . 0x00455ca4..0x00455ccc: every node's prop deleted through its slot 0 (flag 1)
// and zeroed; 0x00455cce..0x00455d13: every node unlinked, its string freed (the routine) and
// the node freed.
void FGfScriptSequence::ClearProps()
{
    std::list<FProp>::iterator It = Props.begin();
    for (; It != Props.end(); ++It)
    {
        if (It->Prop != nullptr)
            delete It->Prop;
        It->Prop = nullptr;
    }
    Props.clear();
}

// (thiscall, RET 8). 0x00455d25..0x00455d58: walk the list at +0x134 from its first
// node; the first prop whose RoomObject slot 6 (+0x18) answers Instance gets the byte at +0x48;
// the walk stops there. An empty list, or no match, writes nothing.
void FGfScriptSequence::SetPropDetached(void* Instance, unsigned char bDetached)
{
    std::list<FProp>::iterator It = Props.begin();
    for (; It != Props.end(); ++It)
    {
        if (It->Prop->GetEngineInstance() == Instance)
        {
            It->Prop->bDetached = bDetached;
            return;
        }
    }
}

// (RET 8). the routine's body on the +0x124 list.
void FGfScriptSequence::AddCharacter(FCharacterObject* Character, char const* CharName)
{
    FCharacter Entry;
    Entry.Character = Character;
    Entry.Name = CharName != nullptr ? CharName : "";
    Characters.push_back(Entry);
}

// (RET 4). Null name: null. stricmp(node c_str (null storage -> ""), name) == 0.
FCharacterObject* FGfScriptSequence::FindCharacter(char const* CharName) const
{
    if (CharName == nullptr)
        return nullptr;
    std::list<FCharacter>::const_iterator It = Characters.begin();
    for (; It != Characters.end(); ++It)
    {
        if (_stricmp(It->Name.c_str(), CharName) == 0)
            return It->Character;
    }
    return nullptr;
}

// (RET 8). Span = Count - Current - 1; the first offset 1..Span whose entry kind is
// in Kinds, else Span + 1 (also when Span < 1).
int FGfScriptSequence::FindNextOfKinds(int const* Kinds, int KindCount) const
{
    int const Span = Count - Current - 1;
    int Offset = 1;
    if (Span < 1)
        return Span + 1;
    for (Offset = 1; Offset <= Span; ++Offset)
    {
        int const Kind = Entries[Current + Offset].Kind;
        int KindIndex = 0;
        for (KindIndex = 0; KindIndex < KindCount; ++KindIndex)
        {
            if (Kind == Kinds[KindIndex])
                return Offset;
        }
    }
    return Span + 1;
}

// . By the current entry's kind: 0 -> +1; 5 -> +0 (the traffic script repeats);
// 1 -> with +0x105: +0x106 set -> single player, or +0xc8 != 3 -> the next of {0, 2, 4}; else
//   (multiplayer, +0xc8 == 3) the field set -> the field == the field ? the next of
//   {0, 2, 4} : the next of {0, 6}, clear -> +0; +0x106 clear -> the next of {0, 3};
//   +0x105 clear -> +0;
// 2 -> the next of {0, 2, 4}; 3 -> the next of {0, 3}; 4 -> +0x10d ? the next of {0, 2, 4} : +0;
// 6 -> the next of {0, 6}; anything else -> +0.
int FGfScriptSequence::NextEntryIndex() const
{
    int const Kind = Entries[Current].Kind;
    int Step = 0;
    if (Kind == 0)
        return Current + 1;
    if (Kind == 1)
    {
        if (bOfferAnswered != 0)
        {
            if (bOfferAccepted != 0)
            {
                // a multiplayer group offer (type 3) waits for the
                // server's answer (the field; +0 until then), then follows the Accept branch
                // when this player is the acceptor (the field == the field) and the kind-6
                // branch otherwise. Every other offer takes the Accept branch.
                if (!SinglePlayer() && OfferType == 3)
                {
                    if (GetMissionOfferAnsweredbaseline() != 0)
                    {
                        if (GetMissionOfferAcceptorIdbaseline() == GetPlayerConnectionId())
                        {
                            int const Kinds[3] = {0, 2, 4};
                            Step = FindNextOfKinds(Kinds, 3);
                        }
                        else
                        {
                            int const Kinds[2] = {0, 6};
                            Step = FindNextOfKinds(Kinds, 2);
                        }
                    }
                }
                else
                {
                    int const Kinds[3] = {0, 2, 4};
                    Step = FindNextOfKinds(Kinds, 3);
                }
            }
            else
            {
                int const Kinds[2] = {0, 3};
                Step = FindNextOfKinds(Kinds, 2);
            }
        }
    }
    else if (Kind == 2)
    {
        int const Kinds[3] = {0, 2, 4};
        Step = FindNextOfKinds(Kinds, 3);
    }
    else if (Kind == 3)
    {
        int const Kinds[2] = {0, 3};
        Step = FindNextOfKinds(Kinds, 2);
    }
    else if (Kind == 4)
    {
        if (bInfoClosed != 0)
        {
            int const Kinds[3] = {0, 2, 4};
            Step = FindNextOfKinds(Kinds, 3);
        }
    }
    else if (Kind == 6)
    {
        int const Kinds[2] = {0, 6};
        Step = FindNextOfKinds(Kinds, 2);
    }
    return Current + Step;
}

// (RET 4). Not playing: false. Not active: true. 0x00456170..0x0045622b: while
// +0x2e (skip): the current script paused, updated by 0, torn down; the next entry (past the
// end: +0xc0 = -1, done); else kinds 1 / 4 clear +0x2e, and the entry starts. Done -> the stop
// below. 0x00456235..0x00456288: the current script updated by Ms; finished -> torn down, the
// next entry started, or past the end the stop: the routine(0), +0xc0 = -1, +0xc4 = 0. Answers
// !+0xc4.
bool FGfScriptSequence::Update(int Ms)
{
    if (bPlaying == 0)
        return false;
    if (bActive == 0)
        return true;

    bool bDone = false;
    if (bSkip != 0)
    {
        while (bSkip != 0)
        {
            if (bDone)
                break;
            Entries[Current].Script->Pause();
            Entries[Current].Script->Update(0);
            if (Entries[Current].Script != nullptr)
                Entries[Current].Script->Teardown();
            bDone = false;
            Current = NextEntryIndex();
            if (Current >= Count)
            {
                Current = -1;
                bDone = true;
            }
            if (!bDone)
            {
                int const Kind = Entries[Current].Kind;
                if (Kind == 1 || Kind == 4)
                    bSkip = 0;
                StartEntry(Current);
            }
        }
        if (!bDone)
        {
            // Fall through to the normal update (0x00456231 TEST BL,BL; JNZ taken only when
            // done).
        }
    }

    if (!bDone)
    {
        if (!Entries[Current].Script->Update(Ms))
            return bActive == 0;
        if (Entries[Current].Script != nullptr)
            Entries[Current].Script->Teardown();
        int const Next = NextEntryIndex();
        Current = Next;
        if (Next < Count)
        {
            StartEntry(Next);
            return bActive == 0;
        }
        Current = -1;
    }

    StopPlayback(0);
    Current = -1;
    bActive = 0;
    return bActive == 0;
}

// . the routine on the current script, +0x2d = 1. Caller: 0x0044529e.
void FGfScriptSequence::RestartCurrent()
{
    Entries[Current].Script->Restart();
    bPlaying = 1;
}

// (RET 4). While playing: a tail jump to the routine on the current script.
void FGfScriptSequence::AdvanceCurrent(float Scale)
{
    if (bPlaying == 0)
        return;
    Entries[Current].Script->Advance(Scale);
}

// =========================================================================================
// FGfScriptManager
// =========================================================================================

// (RET 8). operator new(0x13c) + the routine; 0x20 entries (operator new(0x804),
// count 0x20 at [-4], the branch each) into +0xb4; +0xb8 = 0x20, +0xbc = 0, +0xb0 = Room,
// +0x2f = bFindable; appended; size + 1.
FGfScriptSequence* FGfScriptManager::CreateSequence(FRoomRuntime* Room, unsigned char bFindable)
{
    FGfScriptSequence* const Sequence = new FGfScriptSequence();
    Sequence->Entries = new FGfScriptEntry[0x20];
    Sequence->Capacity = 0x20;
    Sequence->Count = 0;
    Sequence->Room = Room;
    Sequence->bFindable = bFindable;
    Sequences.push_back(Sequence);
    return Sequence;
}

// (RET 4). 0x00456655..0x004566bb: every sequence the routine(Ms); a finished one
// is deleted (the routine + free) and its node erased. 0x004566bd..0x004566d4: the routine
// (the Gf fade) with (float)(Ms * 0.001); when it moved, 0x004566d6..0x0045672c: every sequence
// with +0x0d and a current script: identity slot 21 (+0x54) on each non-null spawned object.
void FGfScriptManager::Update(int Ms)
{
    std::list<FGfScriptSequence*>::iterator It = Sequences.begin();
    while (It != Sequences.end())
    {
        FGfScriptSequence* const Sequence = *It;
        if (Sequence->Update(Ms))
        {
            delete Sequence;
            It = Sequences.erase(It);
        }
        else
        {
            ++It;
        }
    }

    float const Seconds = static_cast<float>(static_cast<double>(Ms) * 0.001);
    if (!TickClientAudioFade(Seconds))
        return;
    for (It = Sequences.begin(); It != Sequences.end(); ++It)
    {
        FGfScriptSequence* const Sequence = *It;
        if (Sequence->bSoundsUseFade == 0)
            continue;
        if (Sequence->Current == -1)
            continue;
        FGfScript* const Script = Sequence->Entries[Sequence->Current].Script;
        if (Script == nullptr)
            continue;
        int Index = 0;
        for (Index = 0; Index < Script->SpawnedCount; ++Index)
        {
            if (Script->Spawned[Index] != nullptr)
                Script->Spawned[Index]->RefreshSound();
        }
    }
}

// (RET 8). Every sequence of Room (+0xb0) that plays (+0x2d): the routine(Camera)
// on its current script (no -1 test: a playing sequence has one).
void FGfScriptManager::DrawEffects(FRoomRuntime* Room, ICamera* Camera)
{
    std::list<FGfScriptSequence*>::iterator It = Sequences.begin();
    for (; It != Sequences.end(); ++It)
    {
        FGfScriptSequence* const Sequence = *It;
        if (Sequence->Room != Room)
            continue;
        if (Sequence->bPlaying == 0)
            continue;
        Sequence->Entries[Sequence->Current].Script->DrawEffects(Camera);
    }
}

// (RET 8). The findable (+0x2f) sequences with a current script: each non-null
// spawned object's the routine(Type, Name); the first match answers.
FSceneScriptComponent* FGfScriptManager::FindCreatedEntity(int Type, char const* EntityName)
{
    std::list<FGfScriptSequence*>::iterator It = Sequences.begin();
    for (; It != Sequences.end(); ++It)
    {
        FGfScriptSequence* const Sequence = *It;
        if (Sequence->bFindable == 0)
            continue;
        if (Sequence->Current == -1)
            continue;
        FGfScript* const Script = Sequence->Entries[Sequence->Current].Script;
        if (Script == nullptr)
            continue;
        int Index = 0;
        for (Index = 0; Index < Script->SpawnedCount; ++Index)
        {
            FSceneScriptComponent* const Object = Script->Spawned[Index];
            if (Object == nullptr)
                continue;
            if (Object->MatchesEntity(Type, EntityName))
                return Object;
        }
    }
    return nullptr;
}

// (RET 4). Id 0xf only: over the sequences (the list the field), each with +0x06
// counts; when it also plays (+0x2d) and has +0x15, +0x2e = 1. Any counted -> the routine,
// true. Otherwise false.
bool FGfScriptManager::OnEvent(unsigned MsgId)
{
    if (MsgId != 0xf)
        return false;
    bool bAny = false;
    std::list<FGfScriptSequence*>::iterator It = Sequences.begin();
    for (; It != Sequences.end(); ++It)
    {
        FGfScriptSequence* const Sequence = *It;
        if (Sequence->bLetterbox == 0)
            continue;
        if (Sequence->bPlaying != 0 && Sequence->Field_0x15 != 0)
            Sequence->bSkip = 1;
        bAny = true;
    }
    if (!bAny)
        return false;
    NnArrivalTextHurry();
    return true;
}

// . False at the first sequence with +0x06 and without +0x15; true otherwise.
bool FGfScriptManager::CanSkipAll() const
{
    std::list<FGfScriptSequence*>::const_iterator It = Sequences.begin();
    for (; It != Sequences.end(); ++It)
    {
        if ((*It)->bLetterbox != 0 && (*It)->Field_0x15 == 0)
            return false;
    }
    return true;
}

// (RET 4). The node holding Sequence: the sequence deleted, the node erased.
void FGfScriptManager::DestroySequence(FGfScriptSequence* Sequence)
{
    std::list<FGfScriptSequence*>::iterator It = Sequences.begin();
    for (; It != Sequences.end(); ++It)
    {
        if (*It == Sequence)
        {
            if (Sequence != nullptr)
                delete Sequence;
            Sequences.erase(It);
            return;
        }
    }
}

// (0x004563d0..0x004564cb). First pass: for each node, a non-null payload gets
// + operator delete, then the node is unlinked and freed. A second erase pass over
// whatever is left (nothing -- the first pass already took every node) and the head free are the
// std::list destructor; the routine is ~EventHandler.
FGfScriptManager::~FGfScriptManager()
{
    std::list<FGfScriptSequence*>::iterator It = Sequences.begin();
    while (It != Sequences.end())
    {
        if (*It != nullptr)
            delete *It;
        It = Sequences.erase(It);
    }
}

// . Every sequence deleted, every node erased.
void FGfScriptManager::DestroyAll()
{
    std::list<FGfScriptSequence*>::iterator It = Sequences.begin();
    while (It != Sequences.end())
    {
        if (*It != nullptr)
            delete *It;
        It = Sequences.erase(It);
    }
}

// the routine(0xf) on the handler face.
void FGfScriptManager::RegisterEvents()
{
    RegisterEvent(0xf);
}

// the routine's body, then the routine(0xf).
void FGfScriptManager::Shutdown()
{
    DestroyAll();
    UnregisterEvent(0xf);
}

// =========================================================================================

// (cdecl). 0x0044fa69 `TEST ESI,ESI; JLE` -- no step when Levels <= 0; otherwise
// Levels times EAX = NeuronRuntime::Engine slot 53 (+0xd4)(EAX). No check for a -1 instance on the way
// (the one caller tests the result afterwards, 0x004524a4).
void* GfEngineInstanceAncestor(void* Instance, int Levels)
{
    void* Current = Instance;
    if (Levels > 0)
    {
        int Remaining = Levels;
        do
        {
            Current = NeuronRuntime::Engine->GetInstanceParent(Current);
            --Remaining;
        } while (Remaining != 0);
    }
    return Current;
}
