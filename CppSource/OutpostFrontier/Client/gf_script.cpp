// Author: Giperion.

#include "gf_script.h"

#include "character_info.h" // GetCharacterInfoDb (the routine)
#include "engine_light.h"
#include "game_options.h"
#include "room_camera.h"
#include "win3d_immediate.h"

#include "NeuronRuntime.h"
#include "common_newchar.h"
#include "engine/sceneruntime.h"
#include "render/IRenderPipeline.h"
#include "scenescript_iface.h"

#include <math.h>
#include <stddef.h>
#include <string.h>
#include <cstring>

static_assert(sizeof(FGfCameraSlot) == 0x40, "pool slot stride 0x40");
static_assert(offsetof(FGfCameraSlot, Orient) == 0x10, "slot orientation +0x10");
static_assert(offsetof(FGfCameraSlot, HalfFovXDeg) == 0x34, "slot fov +0x34");
static_assert(offsetof(FGfCameraPool, Priority) == 0x4000, "pool priorities +0x4000");
static_assert(offsetof(FGfCameraPool, bCanBeActive) == 0x4400, "pool flags +0x4400");
static_assert(offsetof(FGfCameraPool, bOccupied) == 0x4500, "pool occupancy +0x4500");
static_assert(offsetof(FGfCameraPool, ActiveIndex) == 0x4600, "pool active +0x4600");

namespace
{
    // The slot defaults every writer of the pool uses: 0x00450377..0x004503f7,
    // and 0x0045069d..0x004506db.
    void ResetSlot(FGfCameraSlot& Slot)
    {
        Slot.Camera = nullptr;
        Slot.Pos[0] = 0.0f;
        Slot.Pos[1] = 0.0f;
        Slot.Pos[2] = 0.0f;
        int Index = 0;
        for (Index = 0; Index < 9; ++Index)
            Slot.Orient[Index] = 0.0f;
        Slot.Orient[0] = 1.0f;
        Slot.Orient[4] = 1.0f;
        Slot.Orient[8] = 1.0f;
        Slot.HalfFovXDeg = 35.0f;
        Slot.ZNear = 0.25f;
        Slot.ZFar = 1000000.0f;
    }

    // / the field.
    const int kGfLightRegistrySize = 0x40;
    FGfLightWrapper* G_GfLightRegistry[kGfLightRegistrySize] = {};
    unsigned char G_GfLightEnabled[kGfLightRegistrySize] = {};
}

// baseline builds this object in the branch from the _initterm table (.data 0x0060f328) and
// resets it again in the branch at exit; a static object with a ctor and a dtor is that pair.
FGfCameraPool G_GfCameraPool;

FGfCameraPool::FGfCameraPool()
{
    Reset();
}

FGfCameraPool::~FGfCameraPool()
{
    // runs the slot loop twice (defaults first, then defaults with the priority
    // and byte arrays); the second pass alone leaves the same state.
    Reset();
}

void FGfCameraPool::Reset()
{
    int Index = 0;
    for (Index = 0; Index < 0x100; ++Index)
    {
        ResetSlot(Slots[Index]);
        Priority[Index] = -1;
        bCanBeActive[Index] = 0;
        bOccupied[Index] = 0;
    }
    ActiveIndex = -1;
}

int FGfCameraPool::Allocate(int SlotPriority, FRoomCamera* Camera, unsigned char bSlotCanBeActive)
{
    // .
    int Slot = 0;
    for (Slot = 0; Slot < 0x100; ++Slot)
    {
        if (bOccupied[Slot] == 0)
            break;
    }
    if (Slot == 0x100)
        return -1;

    ResetSlot(Slots[Slot]);
    Slots[Slot].Camera = Camera;
    Priority[Slot] = SlotPriority;
    bCanBeActive[Slot] = bSlotCanBeActive;
    bOccupied[Slot] = 1;

    // an empty pool compares against -0x100. Signed JLE.
    int ActivePriority = -0x100;
    if (ActiveIndex != -1)
        ActivePriority = Priority[ActiveIndex];
    if (bSlotCanBeActive != 0 && SlotPriority > ActivePriority)
        ActiveIndex = Slot;
    return Slot;
}

void FGfCameraPool::Free(int Slot)
{
    // . No range check, as baseline.
    const bool bWasActive = Slot == ActiveIndex;
    ResetSlot(Slots[Slot]);
    Priority[Slot] = -1;
    bCanBeActive[Slot] = 0;
    bOccupied[Slot] = 0;
    if (!bWasActive)
        return;

    int Best = -1;
    int BestPriority = -0x100;
    int Index = 0;
    for (Index = 0; Index < 0x100; ++Index)
    {
        if (bOccupied[Index] == 0 || bCanBeActive[Index] == 0)
            continue;
        if (Priority[Index] > BestPriority)
        {
            BestPriority = Priority[Index];
            Best = Index;
        }
    }
    ActiveIndex = Best;
    if (Best == -1)
        return;

    // the routine / cc0 / ce0 / e00 / f10.
    FGfCameraSlot& Next = Slots[Best];
    RoomCameraSetPosition(Next.Camera, Next.Pos);
    RoomCameraSetOrientation(Next.Camera, Next.Orient);
    RoomCameraSetHalfFovX(Next.Camera, Next.HalfFovXDeg);
    RoomCameraSetZNear(Next.Camera, Next.ZNear);
    RoomCameraSetZFar(Next.Camera, Next.ZFar);
}

int GfLightRegistryCount()
{
    // .
    return kGfLightRegistrySize;
}

FGfLightWrapper* GfLightRegistryGet(int Index)
{
    // CMP byte, 1 / JNZ -- only an enable byte of exactly 1 returns the slot.
    if (G_GfLightEnabled[Index] != 1)
        return nullptr;
    return G_GfLightRegistry[Index];
}

int GfLightRegistryAdd(FGfLightWrapper* Wrapper, bool bOn)
{
    // , 0x004530a2..0x004530d9: SETNZ on the props' On dword, then the first
    // null slot of the field.
    int Index = 0;
    for (Index = 0; Index < kGfLightRegistrySize; ++Index)
    {
        if (G_GfLightRegistry[Index] == nullptr)
        {
            G_GfLightRegistry[Index] = Wrapper;
            G_GfLightEnabled[Index] = bOn ? 1 : 0;
            return Index;
        }
    }
    return -1;
}

void GfLightRegistrySetEnabled(int Index, bool bOn)
{
    // SceneScript_Light's D3D-light slot 3, the branch 0x0045331c..0x00453325: SETNZ into
    // [Index]. The caller tests the index for -1 first.
    G_GfLightEnabled[Index] = bOn ? 1 : 0;
}

bool GfLightRegistryIsEnabled(int Index)
{
    // SceneScript_Light's D3D-light slot 6, the branch 0x00453406: MOVZX of the field[Index].
    // The byte only ever holds 0 or 1 (both writers use SETNZ).
    return G_GfLightEnabled[Index] != 0;
}

void GfLightRegistryRemove(int Index)
{
    // , 0x00453100..0x0045310f. No range check, as baseline.
    G_GfLightRegistry[Index] = nullptr;
    G_GfLightEnabled[Index] = 0;
}

FGfLightWrapper* GfLightWrapperCreate(float const* Frame12, FD3dLight8 const* Source,
                                      float const* Direction3, unsigned char Group)
{
    // 0x00556fed..0x00557009: operator new(8), both fields zeroed.
    FGfLightWrapper* Wrapper = new FGfLightWrapper;
    Wrapper->Light = nullptr;
    Wrapper->Group = 0;
    // the 0xc8 light, its source, position and look-at orientation,
    // then the enable through its slot 1 -- all of it is EngineLightCreate.
    Wrapper->Light = EngineLightCreate(Frame12, Source, Direction3);
    // the group byte.
    Wrapper->Group = Group;
    return Wrapper;
}

void GfLightWrapperDestroy(FGfLightWrapper** Field)
{
    // .
    FGfLightWrapper* Wrapper = *Field;
    if (Wrapper == nullptr)
        return;
    if (Wrapper->Light != nullptr)
        EngineLightDestroy(Wrapper->Light);
    Wrapper->Light = nullptr;
    Wrapper->Group = 0;
    delete Wrapper;
    *Field = nullptr;
}

void GfSetSceneAmbient(float R, float G, float B)
{
    // . 0x00556f21..0x00556f4a: the routine(0x18) == 0 -> 0xff, 0xff, 0xff.
    unsigned char Rgb[3];
    if (GetGameOption(kGameOptLighting) == 0)
    {
        Rgb[0] = 0xff;
        Rgb[1] = 0xff;
        Rgb[2] = 0xff;
        EngineLightSetAmbientRgbBytes(Rgb);
        return;
    }
    // per channel FMUL 255.0 (.rdata 0x005c89c0), FADD 0.5
    // (.rdata 0x005c75e8), floor, _ftol, low byte kept.
    Rgb[0] = static_cast<unsigned char>(
        static_cast<int>(floor(static_cast<double>(R) * 255.0 + 0.5)));
    Rgb[1] = static_cast<unsigned char>(
        static_cast<int>(floor(static_cast<double>(G) * 255.0 + 0.5)));
    Rgb[2] = static_cast<unsigned char>(
        static_cast<int>(floor(static_cast<double>(B) * 255.0 + 0.5)));
    EngineLightSetAmbientRgbBytes(Rgb);
}

bool GfLightScore(FD3dLight8 const* D3d, float const* LightPos3, float const* Dir3,
                  float const* Pos3, float /*Radius*/, float* OutScore)
{
    // (cdecl, six arguments; the fifth is never read). The x87 sequence is
    // modelled at the 53-bit precision control MSVC runs with: every FSTP float is a float
    // rounding below, every value left on the stack stays a double.
    if (D3d->Type == 3)
    {
        // directional -- score 3.0 (0x40400000), always taken.
        *OutScore = 3.0f;
        return true;
    }

    // Range^2 against 0.0f (the field); FCOM + TEST AH,0x44 + JP
    // falls through only on equality -> score 0, not taken.
    double const RangeSq = static_cast<double>(D3d->Range) * static_cast<double>(D3d->Range);
    if (RangeSq == 0.0)
    {
        *OutScore = 0.0f;
        return false;
    }

    // d = Pos - LightPos, dx / dy stored as floats, dz kept on the
    // stack (FST) and squared against its stored float; sum (dz*dz + dx*dx) + dy*dy.
    float const Dx = static_cast<float>(static_cast<double>(Pos3[0]) - LightPos3[0]);
    float const Dy = static_cast<float>(static_cast<double>(Pos3[1]) - LightPos3[1]);
    double const DzWide = static_cast<double>(Pos3[2]) - LightPos3[2];
    float const Dz = static_cast<float>(DzWide);
    double const DistSq =
        (DzWide * Dz + static_cast<double>(Dx) * Dx) + static_cast<double>(Dy) * Dy;

    // 2.0f (the field). The CMP ECX,1 at 0x00558734 still holds at 0x00558793:
    // a point light keeps it.
    double Base = 2.0;
    if (D3d->Type != 1)
    {
        // the cone test runs unless DistSq < 1e-5 (the field).
        if (!(DistSq < 1e-5))
        {
            // cos of the angle = ((Dz*dz + Dy*dy) + dx*Dx) / sqrt.
            double const Dot = (static_cast<double>(Dir3[2]) * Dz +
                                static_cast<double>(Dir3[1]) * Dy) +
                               static_cast<double>(Dx) * Dir3[0];
            double const CosAngle = Dot / sqrt(DistSq);
            float const CosAngleStored = static_cast<float>(CosAngle); // FST [param_1]
            // cos(Phi * 0.5f) (the field); FCOMPP + TEST AH,0x41
            // + JP keeps 2.0 only when the angle is strictly inside the half cone.
            double const CosHalfPhi = cos(static_cast<double>(D3d->Phi) * 0.5);
            if (!(CosAngle > CosHalfPhi))
                Base = CosAngleStored;
        }
    }

    // Base - DistSq / Range^2, stored as a float.
    *OutScore = static_cast<float>(Base - DistSq / RangeSq);
    return true;
}

void GfLightsSelectNearest(FGfLightWrapper* const* List, int Count, float const* Pos3,
                           float Radius, unsigned char Group, int MaxCount,
                           FEngineLight** OutLights, float* OutScores, int* InOutCount)
{
    // (cdecl). 0x00557182..0x005571ab: clear the first *InOutCount entries of
    // both arrays, then the count.
    int Index = 0;
    for (Index = 0; Index < *InOutCount; ++Index)
    {
        OutLights[Index] = nullptr;
        OutScores[Index] = 0.0f;
    }
    *InOutCount = 0;

    // .
    for (Index = 0; Index < Count; ++Index)
    {
        FGfLightWrapper* Wrapper = List[Index];
        if (Wrapper == nullptr)
            continue;
        FEngineLight* Light = Wrapper->Light;
        if (Light == nullptr)
            continue;
        if (Wrapper->Group != Group)
            continue;

        // the routine(dir, 0, 0), then the routine(light +0xc,
        // light +0x98, dir, pos, radius, &score).
        float Dir[3];
        float* DirOut = EngineLightComputeDirection(Light, Dir, nullptr, nullptr);
        float Score = 0.0f;
        if (!GfLightScore(&Light->D3d, Light->Position, DirOut, Pos3, Radius, &Score))
            continue;

        // FCOMP + TEST AH,5 + JP -- walk on while Score < the entry,
        // so the arrays run from the highest score down.
        int const Used = *InOutCount;
        int Slot = 0;
        while (Slot < Used && Score < OutScores[Slot])
            ++Slot;
        // shift the tail up one place.
        int Move = 0;
        for (Move = Used; Move > Slot; --Move)
        {
            OutLights[Move] = OutLights[Move - 1];
            OutScores[Move] = OutScores[Move - 1];
        }
        // .
        OutLights[Slot] = Light;
        OutScores[Slot] = Score;
        ++*InOutCount;
    }

    // the count is cut to MaxCount only at the end.
    if (*InOutCount > MaxCount)
        *InOutCount = MaxCount;
}

void GfLightsApplyNearest(FGfLightWrapper* const* List, int Count, void* Instance,
                          unsigned char Group, ICamera* Camera)
{
    // (cdecl). 0x005572b6..0x005572c8: Engine slot 36 (+0x90) position.
    float const* Pos = NeuronRuntime::Engine->GetInstancePosition(Instance);
    // radius 10000.0f (the field), at most 4 lights, 0x40-entry
    // arrays on the stack with the in/out count seeded to 0x40.
    FEngineLight* Lights[0x40];
    float Scores[0x40];
    int Selected = 0x40;
    float const PosCopy[3] = {Pos[0], Pos[1], Pos[2]};
    GfLightsSelectNearest(List, Count, PosCopy, 10000.0f, Group, 4, Lights, Scores, &Selected);
    // the routine(lights, count, camera, 0).
    EngineLightApplyList(Lights, static_cast<unsigned>(Selected), Camera, nullptr);
}

// ---------------------------------------------------------------------------
// the room fog state.
// ---------------------------------------------------------------------------

FGfFogState G_GfFog = {0, 0, 0.0f, 0.0f, 0.0f, 0, 0};

void GfFogReset()
{
    // (and the static initializer the branch, same seven stores).
    G_GfFog.bOn = 0;
    G_GfFog.Color = 0;
    G_GfFog.Start = 0.0f;
    G_GfFog.End = 0.0f;
    G_GfFog.Density = 0.0f;
    G_GfFog.TableMode = 0;
    G_GfFog.VertexMode = 0;
}

void GfFogSetOn(unsigned char bOn)
{
    // RtcSlider::is_enabled("fog") is always asked first; on only when both.
    bool const bSlider = RtcSlider::is_enabled("fog");
    G_GfFog.bOn = (bOn != 0 && bSlider) ? 1 : 0;
}

bool GfFogIsActive()
{
    // (the branch writes the same answer through its out byte).
    if (G_GfFog.bOn == 0)
        return false;
    if (GetGameOption(kGameOptEffectsGroup) == 0)
        return false;
    if (GetGameOption(kGameOptNebula) == 0)
        return false;
    if (G_GfFog.TableMode == 0)
        return true;
    // Pipeline slot 8 (+0x20) ability 0xd into a zeroed dword; the
    // call's result is not looked at.
    unsigned long Ability = 0;
    NeuronRuntime::Pipeline->QueryDeviceAbility(0xd, &Ability);
    return Ability != 0;
}

void GfFogSetColor(int const* Rgb3)
{
    // the low byte of each int, 0x00RRGGBB.
    unsigned const R = static_cast<unsigned>(Rgb3[0]) & 0xffu;
    unsigned const G = static_cast<unsigned>(Rgb3[1]) & 0xffu;
    unsigned const B = static_cast<unsigned>(Rgb3[2]) & 0xffu;
    G_GfFog.Color = (((R << 8) | G) << 8) | B;
}

void GfFogGetColor(int* Rgb3)
{
    // .
    Rgb3[0] = static_cast<int>((G_GfFog.Color >> 16) & 0xffu);
    Rgb3[1] = static_cast<int>((G_GfFog.Color >> 8) & 0xffu);
    Rgb3[2] = static_cast<int>(G_GfFog.Color & 0xffu);
}

void GfFogSetRange(float Start, float End, float Density)
{
    // .
    GfFogSetRangeValue(&Start, &End, &Density);
}

void GfFogSetRangeValue(const float* Start, const float* End, const float* Density)
{
    // loads all three DWORDs before publishing any global.
    unsigned int RangeWords[3];
    std::memcpy(&RangeWords[0], Start, sizeof(RangeWords[0]));
    std::memcpy(&RangeWords[1], End, sizeof(RangeWords[1]));
    std::memcpy(&RangeWords[2], Density, sizeof(RangeWords[2]));
    std::memcpy(&G_GfFog.Start, &RangeWords[0], sizeof(RangeWords[0]));
    std::memcpy(&G_GfFog.End, &RangeWords[1], sizeof(RangeWords[1]));
    std::memcpy(&G_GfFog.Density, &RangeWords[2], sizeof(RangeWords[2]));
}

void GfFogGetRange(float* Start, float* End, float* Density)
{
    // .
    *Start = G_GfFog.Start;
    *End = G_GfFog.End;
    *Density = G_GfFog.Density;
}

void GfFogSetMode(unsigned char bTable, unsigned Mode)
{
    // the mode goes to the table slot or the vertex slot, the other is zeroed.
    if (bTable != 0)
    {
        G_GfFog.TableMode = Mode;
        G_GfFog.VertexMode = 0;
        return;
    }
    G_GfFog.TableMode = 0;
    G_GfFog.VertexMode = Mode;
}

void GfFogGetMode(unsigned char* bTable, unsigned* Mode)
{
    // a non-zero vertex mode wins.
    if (G_GfFog.VertexMode != 0)
    {
        *bTable = 0;
        *Mode = G_GfFog.VertexMode;
        return;
    }
    *bTable = 1;
    *Mode = G_GfFog.TableMode;
}

void GfFogApply()
{
    // -> the routine(this = the field). The gate is the routine's, inlined.
    if (!GfFogIsActive())
    {
        // the routine(0xb60).
        Win3DDisable(kWin3DFog);
        return;
    }
    // D3DRS 0x1c FOGENABLE, 0x22 FOGCOLOR, 0x24 START, 0x25 END,
    // 0x26 DENSITY, 0x23 FOGTABLEMODE, 0x8c FOGVERTEXMODE.
    IRenderPipeline* Pipe = NeuronRuntime::Pipeline;
    int Bits = 0;
    Pipe->SetRenderState(D3DRS_FOGENABLE, 1);
    Pipe->SetRenderState(D3DRS_FOGCOLOR, static_cast<int>(G_GfFog.Color));
    memcpy(&Bits, &G_GfFog.Start, sizeof(Bits));
    Pipe->SetRenderState(D3DRS_FOGSTART, Bits);
    memcpy(&Bits, &G_GfFog.End, sizeof(Bits));
    Pipe->SetRenderState(D3DRS_FOGEND, Bits);
    memcpy(&Bits, &G_GfFog.Density, sizeof(Bits));
    Pipe->SetRenderState(D3DRS_FOGDENSITY, Bits);
    Pipe->SetRenderState(D3DRS_FOGTABLEMODE, static_cast<int>(G_GfFog.TableMode));
    Pipe->SetRenderState(D3DRS_FOGVERTEXMODE, static_cast<int>(G_GfFog.VertexMode));
}

void GfScriptCharacterAnimations(IScriptEngine* Script, bool bAcquire)
{
    // . 0x0044fa96: a null script does nothing.
    if (Script == nullptr)
        return;
    // slot 14 (+0x38) the event count; not positive -> done.
    int const Count = static_cast<int>(Script->Slot14EventCount());
    for (int Index = 0; Index < Count; ++Index)
    {
        // slot 15 (+0x3c) the event's descriptor; START_MOTION only.
        FSceneScriptMotionEventDesc const* const Desc =
            static_cast<FSceneScriptMotionEventDesc const*>(Script->Slot15EventByIndex(Index));
        if (Desc->Common.Type != kSceneScriptTypeStartMotion)
            continue;
        // the routine (the database) then the routine (queue) or
        // (release) with the animation at +0x3c.
        if (bAcquire)
            GetCharacterInfoDb()->QueueScript(Desc->Animation);
        else
            GetCharacterInfoDb()->ReleaseScriptByName(Desc->Animation);
    }
}

void GfScriptFindBackgroundAudio(IScriptEngine* Script, unsigned char* OutAmbient,
                                 unsigned char* OutMusic)
{
    // . 0x0044faf0..0x0044fb03: both outs cleared first, each when non-null.
    if (OutAmbient != nullptr)
        *OutAmbient = 0;
    if (OutMusic != nullptr)
        *OutMusic = 0;
    if (Script == nullptr)
        return;
    // slot 6 (+0x18) the entity count.
    int const Count = static_cast<int>(Script->Slot6EntityCount());
    for (int Index = 0; Index < Count; ++Index)
    {
        // slot 9 (+0x24) the entity descriptor; SOUND only.
        FSceneScriptEntitySnap const* const Entity =
            static_cast<FSceneScriptEntitySnap const*>(Script->Slot9Entity(Index));
        if (Entity->Type != kSceneScriptEntitySound)
            continue;
        // NEURONCOMPONENTS stricmp of the template (+0x3c) against
        // "BackgroundAmbient" (-> OutAmbient) and then "BackgroundMusic" (-> OutMusic).
        unsigned char* Out = nullptr;
        if (_stricmp(Entity->TemplateName, "BackgroundAmbient") == 0)
            Out = OutAmbient;
        else if (_stricmp(Entity->TemplateName, "BackgroundMusic") == 0)
            Out = OutMusic;
        else
            continue;
        if (Out != nullptr)
            *Out = 1;
    }
}

void GfScriptTouchEntities(IScriptEngine* Script, bool bAcquire)
{
    // slot 6 count, slot 9 per entity, nothing kept. bAcquire is passed by both
    // callers and never read by baseline.
    (void)bAcquire;
    if (Script == nullptr)
        return;
    int const Count = static_cast<int>(Script->Slot6EntityCount());
    for (int Index = 0; Index < Count; ++Index)
        Script->Slot9Entity(Index);
}
