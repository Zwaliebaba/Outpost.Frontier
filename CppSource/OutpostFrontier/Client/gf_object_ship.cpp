// Author: Giperion.

#include "gf_object_ship.h"

#include "character_select_equip.h" // GetPlayerEquipDescListbaseline (the routine) & co.
#include "client_cliobj.h"          // CliObjMaterialPaths, CliEngine*, CliObjRecomputeLodPickExplicit
#include "fade_to_black.h"          // SetFadeImmediate (the routine), SetFadeTarget (the routine)
#include "game_options.h"           // GetGameOption (the routine)
#include "gameflow.h"               // / the routine
#include "gf_script_object.h"       // G_GfScriptManager (the field)
#include "menu_sound_arch.h"        // LookupClientSoundArch (the routine), PlayClientSoundArch
#include "misc_util.h"              // MiscUtilLoadEngineInstance (the routine)
#include "vis_effect_arch.h"        // FindEffectArch (the routine)

#include "NeuronRuntime.h"
#include "particlefx.h"
#include "common_archetype.h"
#include "common_cobject.h"   // CompoundInstanceList
#include "common_goods.h"
#include "common_ini.h"       // StringAlloc
#include "common_misc.h"      // malloc_strcpy / malloc_strfree
#include "common_newchar.h"   // RtcSlider
#include "neuroncomponents.h"
#include "engine/sceneruntime.h"
#include "soundmanager.h"
#include "x86math.h"

#include <list>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace
{
    // the ObjectShip singleton. Written by RebuildModel (0x00447219), cleared by
    // the destructor (0x004470e4).
    FObjectShip* G_GfObjectShip = nullptr;

    // ObjectShip.cpp's 3DMathEngine, the routine on first use (the routine,
    // , the routine).
    I3DMathEngine* G_ObjectShipMath = nullptr;
    // the second one, the routine's.
    I3DMathEngine* G_ObjectShipMountMath = nullptr;

    // .rdata 0x005cbdbc.
    char const kObjectShipCpp[] = "Client\\Gf\\ObjectShip.cpp";

    I3DMathEngine* ObjectShipMath()
    {
        if (G_ObjectShipMath == nullptr)
            G_ObjectShipMath = Acquire3DMathEngine();
        return G_ObjectShipMath;
    }

    I3DMathEngine* ObjectShipMountMath()
    {
        if (G_ObjectShipMountMath == nullptr)
            G_ObjectShipMountMath = Acquire3DMathEngine();
        return G_ObjectShipMountMath;
    }

    // IFxEffect slots 7 / 8 still use the existing Dt argument adapter.
    void* FloatArg(float Value)
    {
        void* Out = nullptr;
        memcpy(&Out, &Value, sizeof(Value));
        return Out;
    }

    // The inverse frame the ObjectShip functions build inline: the routine (R^T P), the routine
    // (negated), the routine with the nine elements pushed transposed, the routine.
    void ShipInverseFrame(FTransform& Out, FMatrix const& R, FVector const& P)
    {
        FVector Rotated;
        ShipTransposeRotate(Rotated, R, P);
        FVector Negated;
        Negated.X = -Rotated.X;
        Negated.Y = -Rotated.Y;
        Negated.Z = -Rotated.Z;
        int Row = 0;
        for (Row = 0; Row < 3; ++Row)
        {
            int Col = 0;
            for (Col = 0; Col < 3; ++Col)
                Out.Rotation.D[Row][Col] = R.D[Col][Row];
        }
        Out.Translation = Negated;
    }

    void ToFMatrix(FMatrix& Out, Matrix const& In)
    {
        memcpy(&Out, &In, sizeof(Out));
    }

    void ToFVector(FVector& Out, Vector const& In)
    {
        Out.X = In.x;
        Out.Y = In.y;
        Out.Z = In.z;
    }

    // (cdecl Out, Format, ...) -- vsprintf into a 0x100-byte local, then
    // Out->Value = StringAlloc(Buffer, false). The previous value is not freed (baseline).
    void FormatHardpointName(CacheString* Out, char const* Format, ...)
    {
        char Buffer[0x100];
        va_list Args;
        va_start(Args, Format);
        vsprintf(Buffer, Format, Args);
        va_end(Args);
        Out->Value = StringAlloc(Buffer, false);
    }

    // (cdecl) -- the routine(Handle, 0, 0) on the companion unless Handle is -1.
    void RecomputeShipCompanionLod(Archetype::ICliObj* Companion, long Handle)
    {
        if (Handle == -1)
            return;
        Archetype::CliObjRecomputeLodPickExplicit(Companion, Handle, nullptr, false);
    }

    // The sound play key the routine builds on the stack (0x18 bytes, the routine's layout:
    // offset, flags, the position, +0x14).
    struct FEngineSoundKey
    {
        unsigned Offset;    // +0x00 0
        unsigned Flags;     // +0x04 low byte (garbage & 0xf8) | 3: looping, spatial
        float Pos[3];       // +0x08 the hardpoint instance's position
        unsigned Field14;   // +0x14 0
    };
}

// (cdecl Out, M, V) -- 0x00447be0..0x00447bf1 the field tested and filled
// (the routine), TransposeTransform (slot +0x38) into a scratch, copied to *Out.
void ShipTransposeRotate(FVector& Out, FMatrix const& M, FVector const& V)
{
    FVector Scratch;
    ObjectShipMath()->TransposeTransform(Scratch, M, V);
    Out = Scratch;
}

// ---------------------------------------------------------------------------------------
// The description
// ---------------------------------------------------------------------------------------

FObjectShipDesc::FObjectShipDesc()
    : Ship(nullptr), LaunchScript(nullptr), bShadow(0), Float1C(0.0f), Equipment(nullptr)
{
    // +0x00 = 0, +0x04..+0x0c = 0, the orientation identity (+0x10 / +0x20 /
    // +0x30 = 1.0f), +0x34 = 0, +0x38 = 0, +0x3c = 0, +0x40 = 0.
    Pos.x = 0.0f;
    Pos.y = 0.0f;
    Pos.z = 0.0f;
    MatrixSetIdentity(Orient);
}

FObjectShipDesc::~FObjectShipDesc()
{
    // malloc_strfree(&+0x34).
    malloc_strfree(&LaunchScript);
}

// ---------------------------------------------------------------------------------------
// Engine flame effects (+0xa0)
// ---------------------------------------------------------------------------------------

FObjectShipEngineFx::FObjectShipEngineFx()
    : Instance(kRoomObjectNoInstance), bStopping(0), Lifetime(0.0f)
{
    // +0x00 = +0x04 = 0, +0x08 = -1, +0x3c = 0, +0x40 = 0. The frame (+0x0c..
    // +0x3b) is not written.
    Particles.Effect = nullptr;
    Particles.Vis = nullptr;
}

FObjectShipEngineFx::~FObjectShipEngineFx()
{
    // the routine, +0x08 = -1, +0x3c = 0, +0x40 = 0.
    Particles.Destroy();
    Instance = kRoomObjectNoInstance;
    bStopping = 0;
    Lifetime = 0.0f;
}

void FObjectShipEngineFx::Start(unsigned ArchId, float InLifetime, void* InInstance,
                                Vector const& InPos, Matrix const& InOrient)
{
    // . 0x00446ca5: the routine.
    Particles.Destroy();
    // +0x18 = the orientation, +0x08 = the instance, +0x0c = the
    // position, +0x3c = 0, +0x40 = 0.
    Orient = InOrient;
    Instance = InInstance;
    Pos = InPos;
    bStopping = 0;
    Lifetime = 0.0f;

    // the archetype's class through slot 4 (no null test).
    Archetype::Equipment* const Equip = Archetype::GetEquipment(ArchId);
    if (Equip->get_class_type() == Archetype::AClassEngine)
    {
        // the companion's +0x9c flame_effect, unless empty.
        ID_String const* const Flame = Archetype::CliEngineFlameEffect(Equip->Companion);
        if (!Flame->IsEmpty())
        {
            // the routine on it.
            FEffectArch* const Arch = FindEffectArch(&Flame->Id);
            if (Arch != nullptr)
            {
                // the routine with EffectArch +0x08; on success
                // +0x3c = 0, +0x40 = InLifetime and the routine(0).
                if (Particles.Create(Arch->VisEffect))
                {
                    bStopping = 0;
                    Lifetime = InLifetime;
                    Update(0.0f);
                    return;
                }
            }
            else
            {
                // .
                FDUMP(0x100001,
                      "%s(%d) : *** ERROR: Cannot load engine flame effect ID=%u, name=%s\n",
                      kObjectShipCpp, 0x84, Flame->Id, Flame->get_string());
            }
        }
    }
    // .
    Update(0.0f);
}

void FObjectShipEngineFx::Update(float Delta)
{
    // . 0x00446d99..0x00446d9c.
    if (Particles.Effect == nullptr)
        return;

    // the effect's frame = the instance's transform (Engine slot 40)
    // times the local frame (the field MulTransform, slot +0x1c), copied (the routine).
    FTransform Local;
    ToFMatrix(Local.Rotation, Orient);
    ToFVector(Local.Translation, Pos);
    FTransform const* const InstanceXform =
        reinterpret_cast<FTransform const*>(NeuronRuntime::Engine->GetInstanceTransform(Instance));
    FTransform Product;
    ObjectShipMath()->MulTransform(Product, *InstanceXform, Local);
    FTransform Xform = Product;

    // slot 3 (0, &frame), slot 7 (Delta), slot 8 (Delta).
    Particles.Effect->Slot3(nullptr, &Xform);
    Particles.Effect->Slot7(FloatArg(Delta));
    Particles.Effect->Slot8(FloatArg(Delta));

    if (bStopping == 0)
    {
        // FCOMP 0.0; TEST AH,1; JNZ -> less (or unordered): endless.
        if (!(static_cast<double>(Lifetime) >= 0.0))
            return;
        // FSUB, FST (the float stored, the extended value compared);
        // TEST AH,0x41; JP -> greater or unordered: still counting.
        double const Remaining = static_cast<double>(Lifetime) - static_cast<double>(Delta);
        Lifetime = static_cast<float>(Remaining);
        if (Remaining > 0.0 || Remaining != Remaining)
            return;
        // slot 13 (1) on the effect, +0x3c = 1, +0x40 = 0.
        if (Particles.Effect != nullptr)
            Particles.Effect->Slot13(reinterpret_cast<void*>(1));
        bStopping = 1;
        Lifetime = 0.0f;
        return;
    }

    // stopping -- slot 11 (4); AL set -> the routine.
    GENRESULT const Done = Particles.Effect->Slot11(reinterpret_cast<void*>(4));
    if ((static_cast<unsigned>(Done) & 0xffu) != 0)
        Particles.Destroy();
}

// ---------------------------------------------------------------------------------------
// Engine sounds (+0xa4 / +0xa8)
// ---------------------------------------------------------------------------------------

FObjectShipEngineSound::FObjectShipEngineSound()
    : Instance(kRoomObjectNoInstance), Source(nullptr)
{
    // .
}

FObjectShipEngineSound::~FObjectShipEngineSound()
{
    // slot 34 (+0x88) with 0 on a source, then -1 / 0.
    if (Source != nullptr)
        Source->SetLooping(0);
    Instance = kRoomObjectNoInstance;
    Source = nullptr;
}

// ---------------------------------------------------------------------------------------
// ObjectShip
// ---------------------------------------------------------------------------------------

FObjectShip* GfObjectShip()
{
    // . MOV EAX,[the field]; RET.
    return G_GfObjectShip;
}

FObjectShip::FObjectShip(FRoomRuntime* InOwner, FObjectShipDesc const& Desc)
    : FRoomObject(InOwner),
      Ship(nullptr),
      LaunchScript(nullptr),
      bShadow(0),
      Equipment(nullptr),
      Instance(kRoomObjectNoInstance),
      EngineFx(nullptr),
      LoopSounds(nullptr),
      RumbleSounds(nullptr),
      ScriptState(0)
{
    // . 0x00446f14: the routine (the base); 0x00446f24: the routine on +0x74 (the
    // member); 0x00446f36..0x00446f42: +0x30 / +0x64 / +0x68 / +0x6c = 0, +0x70 = -1.
    // the routine on +0x74.
    Visuals.ResetFields();
    // +0xa0..+0xac = 0 (above); 0x00446f6d: the routine, which writes
    // +0x34 / +0x40 (the ctor leaves them).
    Load(Desc);
}

FObjectShip::~FObjectShip()
{
    // . 0x00446fd8..0x0044701d: the +0x6c list's nodes erased, its head node and
    // the list itself freed.
    if (Equipment != nullptr)
        delete Equipment;
    // .
    malloc_strfree(&LaunchScript);
    // the routine on every hardpoint of +0x74.
    int Index = 0;
    for (Index = 0; Index < Visuals.Count; ++Index)
        Visuals.Detach(Index);
    // the three arrays (the routine / the routine per element).
    delete[] EngineFx;
    delete[] LoopSounds;
    delete[] RumbleSounds;
    // the three pointers cleared, the routine on +0x74.
    EngineFx = nullptr;
    LoopSounds = nullptr;
    RumbleSounds = nullptr;
    Visuals.Clear();
    // Engine slot 24 on a live model (+0x70 keeps its value).
    if (Instance != kRoomObjectNoInstance)
        NeuronRuntime::Engine->DestroyInstance(Instance);
    // the field = 0, whichever ship this is.
    G_GfObjectShip = nullptr;
    // / 0x004470fe: the routine on +0x74 and the routine -- the member and base
    // destructors.
}

bool FObjectShip::IsVisible()
{
    // XOR AL,AL.
    return false;
}

void FObjectShip::Load(FObjectShipDesc const& Desc)
{
    // . 0x0044713a..0x00447165: +0x30 = the archetype, +0x34 the position, +0x40
    // the orientation.
    Ship = Desc.Ship;
    Pos = Desc.Pos;
    Orient = Desc.Orient;
    // malloc_strcpy into +0x64, +0x68 = the byte.
    malloc_strcpy(&LaunchScript, Desc.LaunchScript);
    bShadow = Desc.bShadow;
    // slot 3 with the float.
    SetFloat1C(Desc.Float1C);
    // operator new(0xc) with a fresh head, then the Common list
    // operator= from the description's list.
    Equipment = new EquipDescList();
    Equipment->Items = Desc.Equipment->Items;
    // +0xac = 0, the routine.
    ScriptState = 0;
    RebuildModel();
    // .
    RenderMode = 0;
    LightGroup = 3;
    Layer = 0;
    Flag14 = 0;
}

void FObjectShip::RebuildModel()
{
    // . 0x00447219.
    G_GfObjectShip = this;
    // the routine on every hardpoint of +0x74.
    int Index = 0;
    for (Index = 0; Index < Visuals.Count; ++Index)
        Visuals.Detach(Index);
    // the three arrays deleted and cleared.
    delete[] EngineFx;
    delete[] LoopSounds;
    delete[] RumbleSounds;
    EngineFx = nullptr;
    LoopSounds = nullptr;
    RumbleSounds = nullptr;
    // the routine on +0x74.
    Visuals.Clear();
    // Engine slot 24 on a live model, +0x70 = -1.
    if (Instance != kRoomObjectNoInstance)
    {
        NeuronRuntime::Engine->DestroyInstance(Instance);
        Instance = kRoomObjectNoInstance;
    }

    // the routine on the archetype's mesh with the companion's
    // material list, the material entries into RoomObject +0x20 and the handle into a local
    // (-1 first).
    Archetype::ICliObj* const Companion = Ship->Companion;
    long Handle = -1;
    Instance = MiscUtilLoadEngineInstance(Ship->DaArchetype.Value, NeuronRuntime::Data, nullptr, nullptr,
                                          Archetype::CliObjMaterialPaths(Companion),
                                          &MaterialEntries, &Handle);
    if (Instance != kRoomObjectNoInstance)
    {
        // the routine.
        GfShipApplyCollisionDamage(Instance);
        // the routine with {+0x34, +0x40} as a Csys.
        Csys Frame;
        Frame.Pos = Pos;
        Frame.Orient = Orient;
        GfShipPlaceOnMount(Instance, &Frame);
        // +0x13 = the companion's envmap material is not empty.
        bEnvMap = Archetype::CliObjEnvmapMaterial(Companion)[0] != '\0' ? 1 : 0;
        // the routine on +0x74 with the model.
        Visuals.Populate(static_cast<long>(reinterpret_cast<intptr_t>(Instance)));
        // with "ship_loadouts", the routine and the routine on +0x74
        // with the +0x6c list.
        if (RtcSlider::is_enabled("ship_loadouts"))
        {
            Visuals.AttachAll(Equipment);
            Visuals.AddFlashLights(Equipment);
        }
        // the routine.
        BuildHardpointArrays();
    }
    // the routine (companion, handle) on both paths.
    RecomputeShipCompanionLod(Companion, Handle);
}

void FObjectShip::BuildHardpointArrays()
{
    // . 0x004476ac..0x004476fd: operator new(count * 0x44 + 4), the count stored,
    // on each (the routine) -> +0xa0.
    int const HpCount = Visuals.Count;
    EngineFx = new FObjectShipEngineFx[HpCount];
    // the same with 8-byte the branch elements -> +0xa4, +0xa8.
    LoopSounds = new FObjectShipEngineSound[HpCount];
    RumbleSounds = new FObjectShipEngineSound[HpCount];
    // each sound bound to its hardpoint's instance (get_instance twice).
    int Index = 0;
    for (Index = 0; Index < HpCount; ++Index)
    {
        LoopSounds[Index].Instance =
            reinterpret_cast<void*>(static_cast<intptr_t>(Visuals.Summary->get_instance(Index)));
        RumbleSounds[Index].Instance =
            reinterpret_cast<void*>(static_cast<intptr_t>(Visuals.Summary->get_instance(Index)));
    }
}

void FObjectShip::Update(double DeltaSeconds)
{
    // . 0x004473a5..0x004473c5: the [OBJECTS] group and SHIPS options.
    if (GetGameOption(kGameOptObjectsGroup) == 0)
        return;
    if (GetGameOption(kGameOptShips) == 0)
        return;

    float const Delta = static_cast<float>(DeltaSeconds);
    int Index = 0;
    for (Index = 0; Index < Visuals.Count; ++Index)
    {
        // a bound loop sound follows its instance (Engine slot 36 into
        // a local, source slot 35).
        FObjectShipEngineSound& Loop = LoopSounds[Index];
        if (Loop.Instance != kRoomObjectNoInstance && Loop.Source != nullptr)
        {
            float const* const At = NeuronRuntime::Engine->GetInstancePosition(Loop.Instance);
            float Where[3] = {At[0], At[1], At[2]};
            Loop.Source->Slot35(Where);
        }
        // the same for the rumble.
        FObjectShipEngineSound& Rumble = RumbleSounds[Index];
        if (Rumble.Instance != kRoomObjectNoInstance && Rumble.Source != nullptr)
        {
            float const* const At = NeuronRuntime::Engine->GetInstancePosition(Rumble.Instance);
            float Where[3] = {At[0], At[1], At[2]};
            Rumble.Source->Slot35(Where);
        }
        // the routine on the flame effect.
        EngineFx[Index].Update(Delta);
    }

    // Anim +0x6c (model, Delta).
    NeuronRuntime::Anim->MixerWalk(static_cast<unsigned long>(reinterpret_cast<uintptr_t>(Instance)), Delta);
    // Engine +0x6c (model, 0, Delta).
    NeuronRuntime::Engine->UpdateInstance(Instance, 0, AsOpaqueDword(Delta));
    // the routine on +0x74 while +0x14.
    if (Flag14 != 0)
        Visuals.UpdateFlashLights(Delta);
}

void* FObjectShip::GetEngineInstance()
{
    // MOV EAX,[ECX+0x70].
    return Instance;
}

void FObjectShip::DrawOpaque(ICamera* Camera)
{
    // . 0x00447501..0x0044752b: [OBJECTS], SHIPS, and not LIKE_PETAL.
    if (GetGameOption(kGameOptObjectsGroup) == 0)
        return;
    if (GetGameOption(kGameOptShips) == 0)
        return;
    if (GetGameOption(kGameOptLikePetal) != 0)
        return;
    // .
    if (Instance == kRoomObjectNoInstance)
        return;
    if (Camera == nullptr)
        return;
    // with +0x68, the routine (model, slot 4's float, 0).
    if (bShadow != 0)
    {
        float const Height = GetFloat1C();
        unsigned long HeightBits = 0;
        memcpy(&HeightBits, &Height, sizeof(HeightBits));
        RoomControlDrawShadow(Owner, static_cast<unsigned long>(reinterpret_cast<uintptr_t>(Instance)),
                              HeightBits, 0);
    }
    // the routine.
    RoomControlDrawObject(Owner, this);
}

void FObjectShip::DrawTranslucent(ICamera* Camera)
{
    // [OBJECTS], SHIPS, +0x14 -> the routine on +0x74.
    if (GetGameOption(kGameOptObjectsGroup) == 0)
        return;
    if (GetGameOption(kGameOptShips) == 0)
        return;
    if (Flag14 == 0)
        return;
    Visuals.DrawFlashLights(Camera);
}

void FObjectShip::DrawEffects(ICamera* Camera)
{
    // [OBJECTS], SHIPS, [EFFECTS], PARTICLES -> the routine on every flame.
    if (GetGameOption(kGameOptObjectsGroup) == 0)
        return;
    if (GetGameOption(kGameOptShips) == 0)
        return;
    if (GetGameOption(kGameOptEffectsGroup) == 0)
        return;
    if (GetGameOption(kGameOptParticles) == 0)
        return;
    int Index = 0;
    for (Index = 0; Index < Visuals.Count; ++Index)
        EngineFx[Index].Particles.Draw(Camera);
}

void FObjectShip::StartEngineEffects()
{
    // . 0x00447d7d..0x00447dc8: the first item of the player's list (the routine)
    // whose archetype is an engine.
    EquipDescList& List = GetPlayerEquipDescListbaseline();
    std::list<EquipDesc>::iterator It = List.Items.begin();
    for (; It != List.Items.end(); ++It)
    {
        Archetype::Equipment* const Equip = Archetype::GetEquipment(It->get_arch_id());
        if (Equip != nullptr && Equip->get_class_type() == Archetype::AClassEngine)
            break;
    }
    if (It == List.Items.end())
        return;

    // the ship's NumExhaustNozzles (+0xfc), a cleared CacheString.
    unsigned const ArchId = It->get_arch_id();
    int const Nozzles = Ship->NumExhaustNozzles;
    CacheString HpName;
    HpName.clear();
    int Number = 0;
    for (Number = 1; Number <= Nozzles; ++Number)
    {
        // "HpEngine0<n>" (the routine), found on the summary.
        FormatHardpointName(&HpName, "HpEngine0%d", Number);
        int const Index = Visuals.Summary->find_name(HpName.Value);
        if (Index == -1)
            continue;
        // the routine on +0xa0[index] with the item, -1.0f, the
        // hardpoint's instance, position and orientation.
        HardpointInfo const& Info = Visuals.Summary->get_info(Index);
        void* const HpInstance =
            reinterpret_cast<void*>(static_cast<intptr_t>(Visuals.Summary->get_instance(Index)));
        EngineFx[Index].Start(ArchId, -1.0f, HpInstance, Info.Position, Info.Orient);
    }
}

void FObjectShip::StopEngineEffects()
{
    // slot 13 (1) on each live effect; +0x3c = 1, +0x40 = 0 on every element.
    int Index = 0;
    for (Index = 0; Index < Visuals.Count; ++Index)
    {
        FObjectShipEngineFx& Fx = EngineFx[Index];
        if (Fx.Particles.Effect != nullptr)
            Fx.Particles.Effect->Slot13(reinterpret_cast<void*>(1));
        Fx.bStopping = 1;
        Fx.Lifetime = 0.0f;
    }
}

void FObjectShip::StartEngineSound()
{
    // . Unlike StartEngineEffects, EVERY engine of the player's list is walked
    // (0x00448188..0x00448196 continues with the next node); a later engine overwrites the
    // sources of an earlier one without releasing them.
    EquipDescList& List = GetPlayerEquipDescListbaseline();
    std::list<EquipDesc>::iterator It = List.Items.begin();
    for (; It != List.Items.end(); ++It)
    {
        // .
        Archetype::Equipment* const Equip = Archetype::GetEquipment(It->get_arch_id());
        if (Equip == nullptr)
            continue;
        if (Equip->get_class_type() != Archetype::AClassEngine)
            continue;

        // the companion, the ship's NumExhaustNozzles.
        Archetype::ICliObj* const Companion = Equip->Companion;
        int const Nozzles = Ship->NumExhaustNozzles;
        CacheString HpName;
        HpName.clear();
        int Number = 0;
        for (Number = 1; Number <= Nozzles; ++Number)
        {
            // .
            FormatHardpointName(&HpName, "HpEngine0%d", Number);
            int const Index = Visuals.Summary->find_name(HpName.Value);
            if (Index == -1)
                continue;

            // character_loop_sound (+0x78) on +0xa4[index] --
            // with the key {0, loop | spatial, the instance's position (Engine
            // slot 36), 0} and the element's source as the handle. baseline keeps bits 3..7 of
            // the flag byte from an unwritten stack slot (AND CL,0xf8); the port writes 0.
            FObjectShipEngineSound& Loop = LoopSounds[Index];
            float const* const LoopAt = NeuronRuntime::Engine->GetInstancePosition(Loop.Instance);
            FEngineSoundKey LoopKey;
            LoopKey.Offset = 0;
            LoopKey.Flags = 3;
            LoopKey.Pos[0] = LoopAt[0];
            LoopKey.Pos[1] = LoopAt[1];
            LoopKey.Pos[2] = LoopAt[2];
            LoopKey.Field14 = 0;
            PlayClientSoundArch(LookupClientSoundArch(Archetype::CliEngineCharacterLoopSound(Companion)),
                                reinterpret_cast<unsigned*>(&LoopKey),
                                reinterpret_cast<void**>(&Loop.Source), 0);

            // rumble_sound (+0x60) on +0xa8[index], the same way.
            FObjectShipEngineSound& Rumble = RumbleSounds[Index];
            float const* const RumbleAt = NeuronRuntime::Engine->GetInstancePosition(Rumble.Instance);
            FEngineSoundKey RumbleKey;
            RumbleKey.Offset = 0;
            RumbleKey.Flags = 3;
            RumbleKey.Pos[0] = RumbleAt[0];
            RumbleKey.Pos[1] = RumbleAt[1];
            RumbleKey.Pos[2] = RumbleAt[2];
            RumbleKey.Field14 = 0;
            PlayClientSoundArch(LookupClientSoundArch(Archetype::CliEngineRumbleSound(Companion)),
                                reinterpret_cast<unsigned*>(&RumbleKey),
                                reinterpret_cast<void**>(&Rumble.Source), 0);
        }
    }
}

void FObjectShip::StopEngineSound()
{
    // slot 34 (+0x88) with 0 on each source of +0xa4 and +0xa8.
    int Index = 0;
    for (Index = 0; Index < Visuals.Count; ++Index)
    {
        if (LoopSounds[Index].Source != nullptr)
            LoopSounds[Index].Source->SetLooping(0);
        if (RumbleSounds[Index].Source != nullptr)
            RumbleSounds[Index].Source->SetLooping(0);
    }
}

void FObjectShip::SetEngineSoundAttenuation(float Attenuation)
{
    // slot 32 (+0x80) with the value on each source of +0xa4 and +0xa8.
    int Index = 0;
    for (Index = 0; Index < Visuals.Count; ++Index)
    {
        if (LoopSounds[Index].Source != nullptr)
            LoopSounds[Index].Source->SetAttenuation(Attenuation);
        if (RumbleSounds[Index].Source != nullptr)
            RumbleSounds[Index].Source->SetAttenuation(Attenuation);
    }
}

void FObjectShip::SetEquipment(EquipDescList const* List)
{
    // . 0x0044855c..0x0044859d: the old list's nodes erased, its head and the
    // list freed.
    if (Equipment != nullptr)
        delete Equipment;
    // operator new(0xc) with a fresh head (+0x6c), then the VC6
    // element-wise assign from List (existing nodes overwritten, surplus erased, the rest
    // inserted) -- std::list assignment.
    Equipment = new EquipDescList();
    Equipment->Items = List->Items;
}

void FObjectShip::SetShipArchetype(unsigned ArchId)
{
    // +0x30 = Archetype::GetShip(ArchId).
    Ship = Archetype::GetShip(ArchId);
}

void FObjectShip::MountEquipment(int bCombined, unsigned ArchId, unsigned short /*Id*/,
                                 CacheString const* Hardpoint, int /*Count*/, float /*Health*/)
{
    // . 0x004477d0..0x004477e1: the cargo bay, by pointer.
    if (Hardpoint->Value == EquipDesc::CARGO_BAY_HP_NAME.Value)
        return;
    // .
    int const Index = Visuals.Summary->find_name(Hardpoint->Value);
    if (Index == -1)
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: missing hardpoint (%s)", kObjectShipCpp, 0x275,
              Hardpoint->Value);
        return;
    }
    // an item that was not folded into an existing one.
    if (bCombined == 0)
        Visuals.Attach(Index, Archetype::GetEquipment(ArchId));
}

void FObjectShip::UnmountEquipment(int RemoveResult, unsigned /*Arg2*/, unsigned /*Arg3*/,
                                   CacheString const* Hardpoint, unsigned /*Arg5*/)
{
    // . 0x00447850..0x00447862.
    if (Hardpoint->Value == EquipDesc::CARGO_BAY_HP_NAME.Value)
        return;
    // .
    int const Index = Visuals.Summary->find_name(Hardpoint->Value);
    if (Index == -1)
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: missing hardpoint (%s)", kObjectShipCpp, 0x295,
              Hardpoint->Value);
        return;
    }
    // only when the item was erased.
    if (RemoveResult == 0)
        Visuals.Detach(Index);
}

void FObjectShip::PlayLaunchScript()
{
    // . 0x00447cd7..0x00447cdc, 0x00447d5d..0x00447d62: no launch script -> JMP
    // .
    if (LaunchScript == nullptr)
    {
        RequestGameflowMissionBriefing();
        return;
    }
    // a sequence for the room (the routine(owner, 0)), the script
    // added with an identity frame (Csys ctor + set_identity) and no engine (the routine).
    FGfScriptSequence* const Sequence = G_GfScriptManager.CreateSequence(Owner, 0);
    Csys Frame;
    Frame.set_identity();
    Sequence->AddScript(LaunchScript, 0, &Frame, nullptr);
    // .
    Sequence->BindMode = 0;
    Sequence->bLetterbox = 1;
    Sequence->bFade = 1;
    Sequence->bApplySpatial = 1;
    Sequence->bRestoreCamera = 0;
    Sequence->Priority = 1;
    Sequence->Field_0x16 = 1;
    Sequence->Callback = &FObjectShip::OnScriptStopped;
    Sequence->CallbackA = this;
    Sequence->CallbackB = nullptr;
    // the routine.
    Sequence->Play();
    // +0xac = 1, JMP the routine.
    ScriptState = 1;
    RequestGameflowStateFive();
}

void FObjectShip::PlayLandingScript(char const* Script)
{
    // . 0x00447c35..0x00447c3c: the routine(1.0f).
    SetFadeImmediate(1.0f);
    // the routine(0, 1.0 / the field) -- FDIVR in double, stored
    // as a float over the argument slot.
    SetFadeTarget(0.0f, static_cast<float>(1.0 / static_cast<double>(GetFadeDuration())));
    // the sequence, the script with an identity frame.
    FGfScriptSequence* const Sequence = G_GfScriptManager.CreateSequence(Owner, 0);
    Csys Frame;
    Frame.set_identity();
    Sequence->AddScript(Script, 0, &Frame, nullptr);
    // .
    Sequence->BindMode = 0;
    Sequence->bLetterbox = 1;
    Sequence->bApplySpatial = 1;
    Sequence->bRestoreCamera = 1;
    Sequence->Priority = 1;
    Sequence->Callback = &FObjectShip::OnScriptStopped;
    Sequence->CallbackA = this;
    Sequence->CallbackB = nullptr;
    Sequence->bPropsToRoom = 1;
    // the routine, +0xac = 2.
    Sequence->Play();
    ScriptState = 2;
}

void __cdecl FObjectShip::OnScriptStopped(void* ShipPtr, void* /*Unused*/, unsigned char /*bArg*/)
{
    // .
    FObjectShip* const Ship = static_cast<FObjectShip*>(ShipPtr);
    if (Ship->ScriptState == 0)
        return;
    if (Ship->ScriptState == 1)
    {
        // JMP the routine (+0xac is left at 1).
        RequestGameflowMissionBriefing();
        return;
    }
    if (Ship->ScriptState == 2)
        Ship->ScriptState = 0;
}

// ---------------------------------------------------------------------------------------
// DealerShipObject
// ---------------------------------------------------------------------------------------

FDealerShipDesc::FDealerShipDesc()
    : Ship(nullptr), bShadow(0), Height(0.0f), Loadout(nullptr)
{
    // +0x00 = 0, +0x04..+0x0c = 0, the orientation identity (+0x10 / +0x20 /
    // +0x30 = 1.0f), +0x34 = 0, +0x38 = 0, +0x3c = 0 (the three stack stores are dead).
    Pos.x = 0.0f;
    Pos.y = 0.0f;
    Pos.z = 0.0f;
    MatrixSetIdentity(Orient);
}

FDealerShipObject::FDealerShipObject(FRoomRuntime* InOwner, FDealerShipDesc const& Desc)
    : FRoomObject(InOwner), Instance(kRoomObjectNoInstance), bShadow(0)
{
    // . 0x004437f3: the routine (the base); 0x00443805: the routine on +0x38 (the
    // member); 0x00443817..0x0044381e: +0x30 = -1, +0x34 = 0; 0x00443822: the routine.
    Visuals.ResetFields();
    // the routine.
    Load(Desc);
}

FDealerShipObject::~FDealerShipObject()
{
    // . 0x004438d1: the routine on +0x38.
    Visuals.Clear();
    // Engine slot 24 on a live model.
    if (Instance != kRoomObjectNoInstance)
        NeuronRuntime::Engine->DestroyInstance(Instance);
    // / 0x00443903: the routine and the routine -- the member and base dtors.
}

bool FDealerShipObject::IsVisible()
{
    // XOR AL,AL.
    return false;
}

void FDealerShipObject::Load(FDealerShipDesc const& Desc)
{
    // . 0x00443928..0x00443931: +0x34 = the byte.
    bShadow = Desc.bShadow;
    // slot 3 with Height - 1.6f (FSUB float the field, FSTP float).
    SetFloat1C(static_cast<float>(static_cast<double>(Desc.Height) - static_cast<double>(1.6f)));

    // the routine on the ship's mesh with the companion's material
    // list, the material entries into RoomObject +0x20 and the handle into a local (-1 first).
    Archetype::ICliObj* const Companion = Desc.Ship->Companion;
    long Handle = -1;
    Instance = MiscUtilLoadEngineInstance(Desc.Ship->DaArchetype.Value, NeuronRuntime::Data, nullptr,
                                          nullptr, Archetype::CliObjMaterialPaths(Companion),
                                          &MaterialEntries, &Handle);
    if (Instance != kRoomObjectNoInstance)
    {
        // the routine with {Pos, Orient} as a Csys.
        Csys Frame;
        Frame.Pos = Desc.Pos;
        Frame.Orient = Desc.Orient;
        GfShipPlaceOnMount(Instance, &Frame);
        // +0x13 = the companion's envmap material is not empty.
        bEnvMap = Archetype::CliObjEnvmapMaterial(Companion)[0] != '\0' ? 1 : 0;
        // the routine with the model, the routine with the loadout
        // (no "ship_loadouts" test and no flashlights here, unlike the player's ship).
        Visuals.Populate(static_cast<long>(reinterpret_cast<intptr_t>(Instance)));
        Visuals.AttachAll(Desc.Loadout);
    }
    // +0x15 = 3, +0x16 = 0, +0x18 = 0, the routine (companion,
    // handle) on both paths.
    LightGroup = 3;
    Layer = 0;
    RenderMode = 0;
    RecomputeShipCompanionLod(Companion, Handle);
}

void FDealerShipObject::Update(double DeltaSeconds)
{
    // Engine slot 27 (+0x30, 0, (float)dt). No option test.
    NeuronRuntime::Engine->UpdateInstance(Instance, 0, AsOpaqueDword(static_cast<float>(DeltaSeconds)));
}

void* FDealerShipObject::GetEngineInstance()
{
    // MOV EAX,[ECX+0x30].
    return Instance;
}

void FDealerShipObject::DrawOpaque(ICamera* /*Camera*/)
{
    // . No option, model or camera test. +0x34 -> the routine (model, slot 4's
    // float, 0).
    if (bShadow != 0)
    {
        float const Height = GetFloat1C();
        unsigned long HeightBits = 0;
        memcpy(&HeightBits, &Height, sizeof(HeightBits));
        RoomControlDrawShadow(Owner, static_cast<unsigned long>(reinterpret_cast<uintptr_t>(Instance)),
                              HeightBits, 0);
    }
    // the routine.
    RoomControlDrawObject(Owner, this);
}

// ---------------------------------------------------------------------------------------
// Free functions
// ---------------------------------------------------------------------------------------

void GfShipApplyCollisionDamage(void* InstancePtr)
{
    // . 0x004482cc..0x004482da: the player's ship archetype (the routine).
    long const Instance = static_cast<long>(reinterpret_cast<intptr_t>(InstancePtr));
    Archetype::Ship* const Ship = Archetype::GetShip(GetPlayerShipArchetypeIdbaseline());
    // the player's collision groups (the routine) and every instance
    // of the compound.
    std::list<CollisionGroupDesc>& Groups = GetPlayerCollisionGroupListbaseline();
    int PartCount = 0;
    long* Parts = nullptr;
    CompoundInstanceList(Instance, &PartCount, &Parts);

    // a destroyed group (health == 0.0f: TEST AH,0x44 + JP skips the
    // not-equal and unordered cases) removes every part whose engine name (Engine slot 26, ""
    // for none) is the group's obj (NEURONCOMPONENTS stricmp) -- Engine slot 24, the slot set to -1.
    std::list<CollisionGroupDesc>::iterator It = Groups.begin();
    for (; It != Groups.end(); ++It)
    {
        if (!(It->Health == 0.0f))
            continue;
        int PartIndex = 0;
        for (PartIndex = 0; PartIndex < PartCount; ++PartIndex)
        {
            void* const Part = reinterpret_cast<void*>(static_cast<intptr_t>(Parts[PartIndex]));
            FEngineName NameOut;
            char const* Name = NeuronRuntime::Engine->GetInstanceName(&NameOut, Part)->Text;
            if (Name == nullptr)
                Name = "";
            Archetype::CollisionGroup const* const Group = Ship->get_group_by_id(It->Id);
            if (Name == nullptr || Group == nullptr)
                continue;
            if (_stricmp(Name, Group->ObjName) != 0) // NEURONCOMPONENTS stricmp
                continue;
            NeuronRuntime::Engine->DestroyInstance(Part);
            Parts[PartIndex] = -1;
        }
    }

    // a HardpointSummary of the model.
    HardpointSummary Summary;
    Summary.analyze_instance(Instance);
    // each destroyed group's dmg_obj meshes: the archetype
    // (GetSimple) loaded through the routine (no scripts, entries released), its
    // "DpConnect" connected to the dmg_hp hardpoint (Hardpoint slot 6 on the summary's
    // instance of that name, unchecked).
    for (It = Groups.begin(); It != Groups.end(); ++It)
    {
        if (!(It->Health == 0.0f))
            continue;
        Archetype::CollisionGroup const* const Group = Ship->get_group_by_id(It->Id);
        std::vector<Archetype::DamageObjInfo>::const_iterator Dmg = Group->DmgObjs.Items->begin();
        for (; Dmg != Group->DmgObjs.Items->end(); ++Dmg)
        {
            char const* const HpName = Dmg->Hardpoint.Value;
            Archetype::Root* const Arch = Archetype::GetSimple(Dmg->Arch.Id);
            void* const Child = MiscUtilLoadEngineInstance(
                Arch->DaArchetype.Value, NeuronRuntime::Data, nullptr, nullptr,
                Archetype::CliObjMaterialPaths(Arch->Companion), nullptr, nullptr);
            FHardpointKey ChildKey;
            ChildKey.Name = "DpConnect";
            ChildKey.Crc = NEURONCOMPONENTS_CRC::GetCRC32("DpConnect");
            FHardpointKey ParentKey;
            ParentKey.Name = HpName;
            ParentKey.Crc = NEURONCOMPONENTS_CRC::GetCRC32(HpName);
            long const Parent = Summary.find_name_instance(HpName);
            NeuronRuntime::Hardpoint->ConnectHardpoints(reinterpret_cast<void*>(static_cast<intptr_t>(Parent)),
                                                &ParentKey, Child, &ChildKey);
        }
    }

    // destroy, the instance array freed, ~HardpointSummary.
    Summary.destroy();
    ::operator delete(Parts);
}

void GfShipPlaceOnMount(void* Instance, Csys const* Frame)
{
    // . 0x00448c46..0x00448c90: the model's archetype (Engine slot 25) asked for
    // "HpMount" (Hardpoint slot 3); AL clear -> position 0, orientation identity.
    void* const ArchHandle = NeuronRuntime::Engine->GetInstanceArchetype(Instance);
    FHardpointKey Key;
    Key.Name = "HpMount";
    Key.Crc = NEURONCOMPONENTS_CRC::GetCRC32("HpMount");
    HardpointInfo Mount;
    if ((NeuronRuntime::Hardpoint->GetHardpoint(ArchHandle, &Key, &Mount) & 0xffu) == 0)
    {
        Mount.Position.x = 0.0f;
        Mount.Position.y = 0.0f;
        Mount.Position.z = 0.0f;
        MatrixSetIdentity(Mount.Orient);
    }
    // Engine slot 15 on the archetype handle.
    NeuronRuntime::Engine->ReleaseHandle(ArchHandle);

    // the mount's inverse frame.
    FMatrix MountRotation;
    ToFMatrix(MountRotation, Mount.Orient);
    FVector MountPos;
    ToFVector(MountPos, Mount.Position);
    FTransform Inverse;
    ShipInverseFrame(Inverse, MountRotation, MountPos);

    // the field MulTransform (Frame as a transform, the inverse).
    FTransform FrameXform;
    ToFMatrix(FrameXform.Rotation, Frame->Orient);
    ToFVector(FrameXform.Translation, Frame->Pos);
    FTransform Placed;
    ObjectShipMountMath()->MulTransform(Placed, FrameXform, Inverse);

    // Engine slot 39 with a copy, then slot 27 (model, 0, 0).
    FTransform Copy = Placed;
    NeuronRuntime::Engine->SetInstanceMatrix(Instance, &Copy);
    NeuronRuntime::Engine->UpdateInstance(Instance, 0, nullptr);
}

Csys GfShipMountOffset(HardpointSummary const* Summary, char const* HpName, void* RootInstance)
{
    // . 0x004478c0..0x0044791c: unknown hardpoint -> {0, identity (the routine)}.
    Csys Out;
    int const Index = Summary->find_name(HpName);
    if (Index == -1)
    {
        Out.Pos.x = 0.0f;
        Out.Pos.y = 0.0f;
        Out.Pos.z = 0.0f;
        MatrixSetIdentity(Out.Orient);
        return Out;
    }

    // the hardpoint's instance and its 0x48-byte record; its local
    // frame {orientation, position} kept.
    void* const HpInstance =
        reinterpret_cast<void*>(static_cast<intptr_t>(Summary->get_instance(Index)));
    HardpointInfo const Info = Summary->get_info(Index);
    FMatrix LocalRotation;
    ToFMatrix(LocalRotation, Info.Orient);
    FVector LocalPos;
    ToFVector(LocalPos, Info.Position);

    // Engine slot 40 on the hardpoint's instance, then on the root.
    FTransform const* const HpXform =
        reinterpret_cast<FTransform const*>(NeuronRuntime::Engine->GetInstanceTransform(HpInstance));
    FTransform const* const RootXform =
        reinterpret_cast<FTransform const*>(NeuronRuntime::Engine->GetInstanceTransform(RootInstance));

    // the instance transform's inverse, times the root's
    // (the field MulTransform(Out, inverse, root)), copied (the routine).
    FTransform HpInverse;
    ShipInverseFrame(HpInverse, HpXform->Rotation, HpXform->Translation);
    FTransform Relative;
    ObjectShipMath()->MulTransform(Relative, HpInverse, *RootXform);
    FTransform RelativeCopy = Relative;

    // the local frame's inverse, times that.
    FTransform LocalInverse;
    ShipInverseFrame(LocalInverse, LocalRotation, LocalPos);
    FTransform Result;
    ObjectShipMath()->MulTransform(Result, LocalInverse, RelativeCopy);

    // {translation, rotation} out as a Csys.
    Out.Pos.x = Result.Translation.X;
    Out.Pos.y = Result.Translation.Y;
    Out.Pos.z = Result.Translation.Z;
    memcpy(&Out.Orient, &Result.Rotation, sizeof(Out.Orient));
    return Out;
}
