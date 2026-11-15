// Author: Giperion.

// OutpostFrontier.exe fuse database and fuse-action classes -- baseline module storage-0052c7f1.
// See client_fuse.h for what is and is not ported.

#include "client_fuse.h"

#include "cli_projectile_fx.h" // CliEffectListRemove (the routine), CliEffectCompanionClear
#include "client_explosion.h" // Action_Explode::Create (the routine)
#include "client_gameobj.h"   // FClientGameobj, ExplosionArchAddRef / Release
#include "client_objdb.h"     // ClientObjDb_GetKindHead (the routine)
#include "common_goods.h"     // FindHardpoint_OS / _PS / HardpointToWorld / HardpointInfo
#include "common_physys.h"    // PhySys::LinearImpulse
#include "particle_effect.h"  // CliCreateEffectOneShot / CliCreateEffectWorldOneShot(On)
#include "vis_effect_arch.h"  // FindEffectArch / EffectArchAddRef / EffectArchRelease
#include "common_ini.h"
#include "NeuronRuntime.h"
#include "engine/sceneruntime.h"   // IEngine::GetInstanceTransform (vt+0xa0)
#include "x86math.h"          // I3DMathEngine

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{
    // this file's 3DMathEngine, the routine on first use (0x0052b1bd).
    I3DMathEngine* G_FuseMath = nullptr;

    I3DMathEngine* FuseMath()
    {
        if (G_FuseMath == nullptr)
            G_FuseMath = Acquire3DMathEngine();
        return G_FuseMath;
    }

    // pi/180 as a float (0x3c8efa35).
    float const kDegToRad = 0.01745329238474369f;

    // double 0.0001: ClientImpulseAction::Trigger's minimum radius / force
    // and its "on top of the point" distance.
    double const kImpulseMinimum = 0.0001;

    // StartEffectAction::Trigger's "ROOT" hardpoint name, StringAlloc'd (not
    // temporary) by the static initializer the branch.
    char* G_FuseRootHardpoint = StringAlloc("ROOT", false);

    // One pass of the row rotation StartEffectAction::Slot08 runs for "ori_offset"
    // (module storage-0052b847 and 0052b84b-0052b88b). The angle is value * DEG in x87 double
    // precision; its cosine is stored to a float temporary, its sine stays on the FPU stack.
    // For each column: A = cos*A - sin*B, B = sin*A + cos*B (both from the old A and B).
    void RotateRowPair(float* RowA, float* RowB, float Degrees)
    {
        double const Angle = static_cast<double>(Degrees) * static_cast<double>(kDegToRad);
        float const Cos = static_cast<float>(std::cos(Angle));
        double const Sin = std::sin(Angle);
        for (int Column = 0; Column < 3; ++Column)
        {
            float const A = RowA[Column];
            float const B = RowB[Column];
            RowA[Column] = static_cast<float>(static_cast<double>(Cos) * static_cast<double>(A) -
                                              Sin * static_cast<double>(B));
            RowB[Column] = static_cast<float>(Sin * static_cast<double>(A) +
                                              static_cast<double>(Cos) * static_cast<double>(B));
        }
    }
}

// ---------------------------------------------------------------------------
// ClientFuse
// ---------------------------------------------------------------------------

ClientFuse::ClientFuse(ID_String const& Nickname, float Lifetime)
    : Fuse(Nickname, Lifetime)
    , Host(nullptr)
{
}

ClientFuse::ClientFuse(ClientFuse const& Other)
    : Fuse(Other)
    , Host(Other.Host)
{
}

ClientFuse::~ClientFuse()
{
    // Fuse::~Fuse, then operator delete when bit 0 of the flag is set.
}

bool ClientFuse::Read(INI_Reader& Reader)
{
    // .
    if (!ReadFuseValues(Reader))
        return false;
    if (!Reader.read_header())
        return true;

    for (;;)
    {
        ClientFuseAction* Action = nullptr;
        if (Reader.is_header("start_explosion"))
            Action = new StartExplosionAction();
        else if (Reader.is_header("start_cam_particles"))
            Action = new StartCameraParticlesAction();
        else if (Reader.is_header("start_effect"))
            Action = new StartEffectAction();
        else if (Reader.is_header("impulse"))
            Action = new ClientImpulseAction();
        else if (Reader.is_header("ignite_fuse"))
            Action = new ClientIgniteFuseAction();
        else if (Reader.is_header("fuse"))
            return true; // the next fuse starts here; the caller reads it (0052a0bd)

        if (Action != nullptr)
        {
            // vt+8; a failed read returns false and leaks the action (baseline).
            if (!Action->Slot08(Reader))
                return false;
            // FLD [action+4] -> key, the routine multimap insert.
            Actions.insert(std::multimap<float, FuseAction*>::value_type(
                Action->GetTriggerTime(), Action));
            // vt+0x24.
            if (Action->ShouldRandomize())
                bNeedsRandomize = true;
        }

        if (!Reader.read_header())
            return true;
    }
}

void ClientFuse::BindHost(FClientGameobj* NewHost)
{
    // +0x34 = host, then every action's +0x24 = host (walks the action tree).
    Host = NewHost;
    for (std::multimap<float, FuseAction*>::iterator It = Actions.begin(); It != Actions.end(); ++It)
        static_cast<ClientFuseAction*>(It->second)->Host = NewHost;
}

// ---------------------------------------------------------------------------
// ClientFuseAction
// ---------------------------------------------------------------------------

ClientFuseAction::ClientFuseAction()
    : Host(nullptr)
{
}

ClientFuseAction::~ClientFuseAction()
{
}

void ClientFuseAction::UnTriggerAndSettle(unsigned short SubObjId)
{
    // find SubObjId, erase it (shift the tail down, _Last -= 2); then when the
    // vector has no storage or is empty, clear bTriggered.
    std::vector<unsigned short>::iterator It =
        std::find(TriggeredSubObjs.begin(), TriggeredSubObjs.end(), SubObjId);
    if (It != TriggeredSubObjs.end())
        TriggeredSubObjs.erase(It);
    if (TriggeredSubObjs.empty())
        bTriggered = false;
}

// ---------------------------------------------------------------------------
// StartExplosionAction
// ---------------------------------------------------------------------------

StartExplosionAction::StartExplosionAction()
    : ExplosionArch(nullptr)
{
    // / 0052a151-0052a171.
    Explosion.clear();
    Hardpoint.clear();
    PosOffset.x = 0.0f;
    PosOffset.y = 0.0f;
    PosOffset.z = 0.0f;
}

FuseAction* StartExplosionAction::Slot04() const
{
    // new 0x40 object, then its slot 12 copies *this.
    StartExplosionAction* Copy = new StartExplosionAction();
    Copy->Slot30(*this);
    return Copy;
}

bool StartExplosionAction::Slot08(INI_Reader& Reader)
{
    // .
    while (Reader.read_value())
    {
        if (ReadFuseActionValue(Reader))
            continue;
        if (Reader.is_value("explosion"))
        {
            Explosion.Id = CreateID(Reader.get_value_string());
        }
        else if (Reader.is_value("hardpoint"))
        {
            Hardpoint.Value = StringAlloc(Reader.get_value_string(), false);
        }
        else if (Reader.is_value("pos_offset"))
        {
            float const X = Reader.get_value_float(0);
            float const Y = Reader.get_value_float(1);
            float const Z = Reader.get_value_float(2);
            PosOffset.x = X;
            PosOffset.y = Y;
            PosOffset.z = Z;
        }
    }
    return true;
}

int StartExplosionAction::Load()
{
    // Archetype::GetExplosion(+0x28) into +0x3c; a found one is AddRef'd
    // (the routine); then the tail JMP to FuseAction::Load.
    ExplosionArch = Archetype::GetExplosion(Explosion);
    if (ExplosionArch != nullptr)
        ExplosionArchAddRef(ExplosionArch);
    return FuseAction::Load();
}

int StartExplosionAction::UnLoad()
{
    // +0x3c released when set (the routine, the pointer is left as it is); then
    // the tail JMP to FuseAction::UnLoad.
    if (ExplosionArch != nullptr)
        ExplosionArchRelease(ExplosionArch);
    return FuseAction::UnLoad();
}

void StartExplosionAction::Trigger(unsigned SourceId, unsigned short SubObjId)
{
    // (RET 8). 0x0052b156..0x0052b15e: slot 6 (IsTriggered()) true skips to the
    // tail.
    if (!IsTriggered())
    {
        // the explosion looked up again by name (not +0x3c); only one
        // whose process (+0x14) is 2 starts.
        Archetype::Explosion* const Arch = Archetype::GetExplosion(Explosion);
        if (Arch != nullptr && Arch->Process == 2)
        {
            // the host's hardpoint in object space (FindHardpoint_OS
            // on its engine index, vt+0x1c).
            long HpHandle = 0;
            HardpointInfo Info;
            if (FindHardpoint_OS(static_cast<long>(Host->get_index()), Hardpoint.Value, HpHandle,
                                 Info))
            {
                // into world space on the handle it came back with.
                HardpointToWorld(HpHandle, Info);

                // pos_offset turned by the hardpoint's orientation
                // (the field vt+0x2c TransformMatrix), added to its position.
                FVector Turned;
                FuseMath()->TransformMatrix(Turned, reinterpret_cast<FMatrix const&>(Info.Orient),
                                            reinterpret_cast<FVector const&>(PosOffset));
                Vector Pos;
                Pos.x = Turned.X + Info.Position.x;
                Pos.y = Turned.Y + Info.Position.y;
                Pos.z = Turned.Z + Info.Position.z;

                // the routine(Arch, Host, &Pos); a made action goes
                // on the host's action list (+0x20).
                Action_Explode* const Explode = Action_Explode::Create(Arch, Host, &Pos);
                if (Explode != nullptr)
                    Host->Actions.add(Explode);
            }
        }
    }

    // FuseAction::Trigger's body, inlined.
    FuseAction::Trigger(SourceId, SubObjId);
}

void StartExplosionAction::UnTrigger(unsigned short SubObjId)
{
    UnTriggerAndSettle(SubObjId);
}

void StartExplosionAction::Slot30(StartExplosionAction const& Other)
{
    // CopyArchProperties, +0x28, +0x2c, +0x30..+0x38. +0x3c is not copied.
    FuseAction::CopyArchProperties(Other);
    Explosion = Other.Explosion;
    Hardpoint = Other.Hardpoint;
    PosOffset = Other.PosOffset;
}

// ---------------------------------------------------------------------------
// StartCameraParticlesAction
// ---------------------------------------------------------------------------

StartCameraParticlesAction::StartCameraParticlesAction()
    : EffectArch(nullptr)
{
    // / 0052a211-0052a21f.
    Effect.clear();
}

FuseAction* StartCameraParticlesAction::Slot04() const
{
    // .
    StartCameraParticlesAction* Copy = new StartCameraParticlesAction();
    Copy->Slot30(*this);
    return Copy;
}

bool StartCameraParticlesAction::Slot08(INI_Reader& Reader)
{
    // .
    while (Reader.read_value())
    {
        if (ReadFuseActionValue(Reader))
            continue;
        if (Reader.is_value("effect"))
            Effect.Id = CreateID(Reader.get_value_string());
    }
    return true;
}

int StartCameraParticlesAction::Load()
{
    // the routine(+0x28) into +0x2c; a found arch gets the routine(0); then the
    // tail JMP to FuseAction::Load.
    EffectArch = FindEffectArch(&Effect.Id);
    if (EffectArch != nullptr)
        EffectArchAddRef(EffectArch, 0);
    return FuseAction::Load();
}

int StartCameraParticlesAction::UnLoad()
{
    // +0x2c released (the routine) when set -- the pointer is not cleared; then
    // the tail JMP to FuseAction::UnLoad.
    if (EffectArch != nullptr)
        EffectArchRelease(EffectArch);
    return FuseAction::UnLoad();
}

void StartCameraParticlesAction::Trigger(unsigned SourceId, unsigned short SubObjId)
{
    // (RET 8). 0x0052b377..0x0052b37f: slot 6 (IsTriggered()) true skips to the
    // tail.
    if (!IsTriggered())
    {
        // a zero Vector on the stack (Pos3), then the host's vt+0x10
        // (IObjInspect::get_orientation, folded with get_transform: the object's whole
        // Transform), called before the id is tested.
        float const Pos3[3] = {0.0f, 0.0f, 0.0f};
        Matrix const& HostOrient = Host->get_orientation();

        // a non-empty +0x28 is looked up again (the routine, not +0x2c)
        // and handed to the routine on the field with no companion out.
        if (!Effect.IsEmpty())
        {
            CliCreateEffectWorldOneShotOn(&G_CliEffectFactoryCameraParticles,
                                          FindEffectArch(&Effect.Id),
                                          reinterpret_cast<FTransform const*>(&HostOrient), Pos3,
                                          nullptr);
        }
    }

    // FuseAction::Trigger's body, inlined.
    FuseAction::Trigger(SourceId, SubObjId);
}

void StartCameraParticlesAction::UnTrigger(unsigned short SubObjId)
{
    UnTriggerAndSettle(SubObjId);
}

void StartCameraParticlesAction::Slot30(StartCameraParticlesAction const& Other)
{
    // CopyArchProperties, +0x28.
    FuseAction::CopyArchProperties(Other);
    Effect = Other.Effect;
}

// ---------------------------------------------------------------------------
// StartEffectAction
// ---------------------------------------------------------------------------

StartEffectAction::StartEffectAction()
    : bAttached(false)
    , EffectArch(nullptr)
{
    // / 0052a2e1-0052a311: the companion by the routine(0,0,0,0).
    Effect.clear();
    Companion[0] = nullptr;
    Companion[1] = nullptr;
    Companion[2] = nullptr;
    Companion[3] = nullptr;
}

StartEffectAction::~StartEffectAction()
{
    // . 0x0052b534..0x0052b56c and again 0x0052b56f..0x0052b5a2 (EH state 2 -> 1:
    // the class body's release, then the companion member's destructor): particle +0x4c
    // (the routine), sound +0x48 (vt+8), light +0x50 (the routine), each zeroed, then +0x44
    // zeroed -- the the routine sequence. The second pass finds everything null. Then the
    // hardpoint vector is freed (0x0052b5a5) and ~FuseAction runs (0x0052b5b7).
    CliEffectCompanionClear(Companion);
    CliEffectCompanionClear(Companion);
}

FuseAction* StartEffectAction::Slot04() const
{
    // .
    StartEffectAction* Copy = new StartEffectAction();
    Copy->Slot30(*this);
    return Copy;
}

bool StartEffectAction::Slot08(INI_Reader& Reader)
{
    // .
    while (Reader.read_value())
    {
        if (ReadFuseActionValue(Reader))
            continue;
        if (Reader.is_value("effect"))
        {
            Effect.Id = CreateID(Reader.get_value_string());
        }
        else if (Reader.is_value("attached"))
        {
            bAttached = Reader.get_value_bool(0);
        }
        else if (Reader.is_value("hardpoint"))
        {
            // {cleared CacheString, identity matrix (the routine), zero
            // offset}, the hardpoint name StringAlloc'd, inserted at end (the routine).
            FStartEffectHardpoint Entry;
            Entry.Hardpoint.clear();
            Entry.Orientation = Matrix(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
            Entry.PosOffset.x = 0.0f;
            Entry.PosOffset.y = 0.0f;
            Entry.PosOffset.z = 0.0f;
            Entry.Hardpoint.Value = StringAlloc(Reader.get_value_string(), false);
            Hardpoints.insert(Hardpoints.end(), 1, Entry);
        }
        else if (Reader.is_value("pos_offset"))
        {
            if (Hardpoints.empty())
            {
                Reader.log_link("StartEffectAction::Read() pos_offset precedes any hardpoint.  Ignored.");
            }
            else
            {
                float const X = Reader.get_value_float(0);
                float const Y = Reader.get_value_float(1);
                float const Z = Reader.get_value_float(2);
                FStartEffectHardpoint& Last = Hardpoints.back();
                Last.PosOffset.x = X;
                Last.PosOffset.y = Y;
                Last.PosOffset.z = Z;
            }
        }
        else if (Reader.is_value("ori_offset"))
        {
            if (Hardpoints.empty())
            {
                Reader.log_link("StartEffectAction::Read() ori_offset precedes any hardpoint.  Ignored.");
            }
            else
            {
                // . Value 2 builds a rotation about Y; value 0 then rotates
                // rows 1/2, value 1 rows 0/1. The Y angle's cosine comes from the unrounded
                // product, its sine from the product stored as a float (FST, FLD, FSIN).
                float const RotRows12 = Reader.get_value_float(0);
                float const RotRows01 = Reader.get_value_float(1);
                double const AngleY =
                    static_cast<double>(Reader.get_value_float(2)) * static_cast<double>(kDegToRad);
                float const AngleYStored = static_cast<float>(AngleY);
                Matrix& M = Hardpoints.back().Orientation;
                double const CosY = std::cos(AngleY);
                M.E[0][1] = 0.0f;
                M.E[1][1] = 1.0f;
                M.E[1][2] = 0.0f;
                M.E[2][1] = 0.0f;
                M.E[1][0] = 0.0f;
                double const SinY = std::sin(static_cast<double>(AngleYStored));
                M.E[0][0] = static_cast<float>(CosY);
                M.E[0][2] = static_cast<float>(SinY);
                M.E[2][0] = static_cast<float>(-SinY);
                M.E[2][2] = static_cast<float>(CosY);
                RotateRowPair(M.E[1], M.E[2], RotRows12);
                RotateRowPair(M.E[0], M.E[1], RotRows01);
            }
        }
    }
    return true;
}

int StartEffectAction::Load()
{
    // the routine(+0x28) into +0x40; a found arch gets the routine(0); then the
    // tail JMP to FuseAction::Load.
    EffectArch = FindEffectArch(&Effect.Id);
    if (EffectArch != nullptr)
        EffectArchAddRef(EffectArch, 0);
    return FuseAction::Load();
}

int StartEffectAction::UnLoad()
{
    // +0x40 released (the routine) when set -- the pointer is not cleared; then
    // the tail JMP to FuseAction::UnLoad.
    if (EffectArch != nullptr)
        EffectArchRelease(EffectArch);
    return FuseAction::UnLoad();
}

void StartEffectAction::Trigger(unsigned SourceId, unsigned short SubObjId)
{
    // (RET 8). 0x0052bb0b..0x0052bb14: slot 6 (IsTriggered()) true skips to the
    // tail.
    if (!IsTriggered())
    {
        // engine index -1, the local transform identity (the routine)
        // with a zero position, nothing found yet (BL).
        long EngineIndex = -1;
        FTransform Local;
        reinterpret_cast<Matrix&>(Local.Rotation) =
            Matrix(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
        Local.Translation.X = 0.0f;
        Local.Translation.Y = 0.0f;
        Local.Translation.Z = 0.0f;
        bool bFound = false;

        // no hardpoints (no storage, or a zero count) -> the host's root.
        int const Count = static_cast<int>(Hardpoints.size());
        if (Count == 0)
        {
            // .
            EngineIndex = Host->get_index();
            bFound = true;
        }
        else
        {
            // start = rand() * count / 32768 (IMUL, then CDQ / AND 0x7fff
            // / ADD / SAR 15 -- a signed division rounding toward zero).
            int const Start = (std::rand() * Count) / 32768;
            int Index = Start;
            for (;;)
            {
                FStartEffectHardpoint const& Entry = Hardpoints[Index];
                // NEURONCOMPONENTS stricmp against the "ROOT" string (the field).
                if (_stricmp(Entry.Hardpoint.Value, G_FuseRootHardpoint) == 0) // NEURONCOMPONENTS stricmp
                {
                    // , 0x0052bce7: the host's own index and the entry's
                    // orientation + offset copied as the local transform (0xc dwords from +4).
                    EngineIndex = Host->get_index();
                    reinterpret_cast<Matrix&>(Local.Rotation) = Entry.Orientation;
                    Local.Translation = reinterpret_cast<FVector const&>(Entry.PosOffset);
                    bFound = true;
                }
                else
                {
                    // FindHardpoint_OS on the host's index; the handle it
                    // returns becomes the engine index.
                    HardpointInfo Info;
                    if (FindHardpoint_OS(Host->get_index(), Entry.Hardpoint.Value, EngineIndex,
                                         Info))
                    {
                        // the offset turned by the hardpoint's
                        // orientation (vt+0x2c TransformMatrix), plus its position, through
                        // Vector(x,y,z) (the routine) into the local position.
                        FVector Turned;
                        FuseMath()->TransformMatrix(Turned,
                                                    reinterpret_cast<FMatrix const&>(Info.Orient),
                                                    reinterpret_cast<FVector const&>(Entry.PosOffset));
                        Local.Translation.X = Turned.X + Info.Position.x;
                        Local.Translation.Y = Turned.Y + Info.Position.y;
                        Local.Translation.Z = Turned.Z + Info.Position.z;
                        // rotation = hardpoint orientation * entry
                        // orientation (vt+0x20 MulMatrix), copied (9 dwords) into the local one.
                        FMatrix Rotation;
                        FuseMath()->MulMatrix(Rotation,
                                              reinterpret_cast<FMatrix const&>(Info.Orient),
                                              reinterpret_cast<FMatrix const&>(Entry.Orientation));
                        Local.Rotation = Rotation;
                        bFound = true;
                    }
                }

                // next index, wrapping to 0 at the count. Back at the start
                // with nothing found goes straight to the tail; otherwise a found entry ends the
                // search.
                ++Index;
                if (Index == Count)
                    Index = 0;
                if (Index == Start)
                {
                    if (!bFound)
                    {
                        FuseAction::Trigger(SourceId, SubObjId);
                        return;
                    }
                    break;
                }
                if (bFound)
                    break;
            }
        }

        if (bAttached && EngineIndex != -1)
        {
            // attached -- a non-empty +0x28 is looked up again
            // (the routine, not +0x40) and started on the host's effect list through
            // (the field) with the companion at +0x44; the result is unused.
            if (!Effect.IsEmpty())
            {
                CliCreateEffectOneShot(FindEffectArch(&Effect.Id), Host,
                                       static_cast<int>(EngineIndex), &Local, Companion);
            }
        }
        else if (bFound)
        {
            // world transform = the engine instance's (NeuronRuntime::Engine
            // vt+0xa0) * local (vt+0x1c MulTransform), copied (0xc dwords).
            float* const InstanceRaw = NeuronRuntime::Engine->GetInstanceTransform(
                reinterpret_cast<void*>(static_cast<intptr_t>(EngineIndex)));
            FTransform World;
            FuseMath()->MulTransform(World, *reinterpret_cast<FTransform const*>(InstanceRaw),
                                     Local);
            // the host's velocity (vt+4), taken before the id test.
            Vector const Velocity = Host->get_velocity();
            // a non-empty id looked up again and started as a world
            // one-shot (the routine on the field) moving with the host, no companion out.
            if (!Effect.IsEmpty())
            {
                CliCreateEffectWorldOneShot(FindEffectArch(&Effect.Id), &World, &Velocity.x,
                                            nullptr);
            }
        }
    }

    // FuseAction::Trigger's body, inlined.
    FuseAction::Trigger(SourceId, SubObjId);
}

void StartEffectAction::UnTrigger(unsigned short SubObjId)
{
    // (RET 4). 0x0052be95..0x0052bed1: find SubObjId, erase it (tail shifted down,
    // _Last -= 2).
    std::vector<unsigned short>::iterator It =
        std::find(TriggeredSubObjs.begin(), TriggeredSubObjs.end(), SubObjId);
    if (It != TriggeredSubObjs.end())
        TriggeredSubObjs.erase(It);
    // only once the list has no storage or is empty.
    if (!TriggeredSubObjs.empty())
        return;
    // the companion taken off the host's effect list (the routine);
    // when it was not on it, released right here -- particle +0x4c (the routine), sound +0x48
    // (vt+8), light +0x50 (the routine), then +0x44 zeroed (the the routine sequence).
    if (!CliEffectListRemove(&Host->EffectList, Companion))
        CliEffectCompanionClear(Companion);
    // .
    bTriggered = false;
}

void StartEffectAction::Slot30(StartEffectAction const& Other)
{
    // CopyArchProperties, +0x28, +0x2c, then the inlined vector::operator=
    // (skipped for self-assignment; elements are copied bitwise, the routine).
    FuseAction::CopyArchProperties(Other);
    Effect = Other.Effect;
    bAttached = Other.bAttached;
    if (&Hardpoints != &Other.Hardpoints)
        Hardpoints = Other.Hardpoints;
}

// ---------------------------------------------------------------------------
// ClientImpulseAction
// ---------------------------------------------------------------------------

ClientImpulseAction::ClientImpulseAction()
    : Force(0.0f)
    , Radius(0.0f)
{
    // / 0052a3c1-0052a3db.
    Hardpoint.clear();
    PosOffset.x = 0.0f;
    PosOffset.y = 0.0f;
    PosOffset.z = 0.0f;
}

FuseAction* ClientImpulseAction::Slot04() const
{
    // .
    ClientImpulseAction* Copy = new ClientImpulseAction();
    Copy->Slot30(*this);
    return Copy;
}

bool ClientImpulseAction::Slot08(INI_Reader& Reader)
{
    // .
    while (Reader.read_value())
    {
        if (ReadFuseActionValue(Reader))
            continue;
        if (Reader.is_value("hardpoint"))
        {
            Hardpoint.Value = StringAlloc(Reader.get_value_string(), false);
        }
        else if (Reader.is_value("force"))
        {
            Force = Reader.get_value_float(0);
        }
        else if (Reader.is_value("radius"))
        {
            Radius = Reader.get_value_float(0);
        }
        else if (Reader.is_value("pos_offset"))
        {
            // values read 2, 1, 0.
            float const Z = Reader.get_value_float(2);
            float const Y = Reader.get_value_float(1);
            float const X = Reader.get_value_float(0);
            PosOffset.x = X;
            PosOffset.y = Y;
            PosOffset.z = Z;
        }
    }
    return true;
}

void ClientImpulseAction::Trigger(unsigned SourceId, unsigned short SubObjId)
{
    // (RET 8). 0x0052c0c6..0x0052c0ce: slot 6 (IsTriggered()) true skips to the
    // tail.
    if (!IsTriggered())
    {
        // FindHardpoint_PS (import 0x005c65e8, not _OS) on the host's
        // index, the out handle starting at -1; not found goes to the tail.
        long HpHandle = -1;
        HardpointInfo Info;
        if (FindHardpoint_PS(Host->get_index(), Hardpoint.Value, HpHandle, Info))
        {
            // into world space on the host's index (not the handle).
            HardpointToWorld(Host->get_index(), Info);

            // pos_offset through vt+0x38 (TransposeTransform) with the
            // hardpoint's orientation, plus its position, through Vector(x,y,z) (the routine).
            FVector Turned;
            FuseMath()->TransposeTransform(Turned, reinterpret_cast<FMatrix const&>(Info.Orient),
                                           reinterpret_cast<FVector const&>(PosOffset));
            Vector Pos;
            Pos.x = Turned.X + Info.Position.x;
            Pos.y = Turned.Y + Info.Position.y;
            Pos.z = Turned.Z + Info.Position.z;

            // radius, then force, against 1e-4 (double the field);
            // TEST AH,0x41 / JNZ -- on to the tail when either is <= it (or unordered).
            if (static_cast<double>(Radius) > kImpulseMinimum &&
                static_cast<double>(Force) > kImpulseMinimum)
            {
                // the ObjDB kind-10 bucket (the routine).
                int Kind = 10;
                FClientGameobj* Go = ClientObjDb_GetKindHead(&Kind);
                while (Go != nullptr)
                {
                    // the object at +0x10 when its class has bit 0 set,
                    // else null -- and no null test before 0x0052c1da reads it (baseline).
                    CObject* Obj = Go->Object;
                    if (Obj == nullptr || (static_cast<unsigned>(Obj->ObjectClass) & 1u) != 1u)
                        Obj = nullptr;

                    // the offset from the impulse point, each component
                    // stored as a float; its length from x87 sums of the float squares, stored
                    // as a float.
                    float const Dz = Obj->WorldPosition.z - Pos.z;
                    float const Dy = Obj->WorldPosition.y - Pos.y;
                    float const Dx = Obj->WorldPosition.x - Pos.x;
                    Vector Impulse;
                    Impulse.x = Dx;
                    Impulse.y = Dy;
                    Impulse.z = Dz;
                    float const Distance = static_cast<float>(
                        std::sqrt(static_cast<double>(Dx) * static_cast<double>(Dx) +
                                  static_cast<double>(Dy) * static_cast<double>(Dy) +
                                  static_cast<double>(Dz) * static_cast<double>(Dz)));

                    // force against 0.0f (the field), TEST AH,0x41 /
                    // JNZ -- skipped when <= 0. 0x0052c241..0x0052c24d: distance against the
                    // radius, TEST AH,5 / JP -- skipped unless strictly less.
                    if (Force > 0.0f && Distance < Radius)
                    {
                        // TEST AH,5 / JP -- a distance below 1e-4 pushes
                        // straight up by the force.
                        if (static_cast<double>(Distance) < kImpulseMinimum)
                        {
                            // .
                            Impulse.x = 0.0f;
                            Impulse.y = Force;
                            Impulse.z = 0.0f;
                        }
                        else
                        {
                            // (1.0 (double the field) - d / radius) *
                            // force / d, kept on the FPU stack, times each stored component.
                            double const Scale =
                                (1.0 - static_cast<double>(Distance) / static_cast<double>(Radius)) *
                                static_cast<double>(Force) / static_cast<double>(Distance);
                            Impulse.x = static_cast<float>(static_cast<double>(Dx) * Scale);
                            Impulse.y = static_cast<float>(static_cast<double>(Dy) * Scale);
                            Impulse.z = static_cast<float>(static_cast<double>(Dz) * Scale);
                        }
                        // only an object with a physics body (+0x54).
                        if (Obj->BodyPtr != nullptr)
                            PhySys::LinearImpulse(Obj, Impulse, 1.0f);
                    }

                    // the bucket link at +0x14.
                    Go = Go->ListNext;
                }
            }
        }
    }

    // FuseAction::Trigger's body, inlined.
    FuseAction::Trigger(SourceId, SubObjId);
}

void ClientImpulseAction::Slot30(ClientImpulseAction const& Other)
{
    // CopyArchProperties, +0x2c, +0x30, +0x34..+0x3c. The hardpoint at +0x28 is
    // NOT copied, so a clone keeps the cleared CacheString of its constructor (baseline).
    FuseAction::CopyArchProperties(Other);
    Force = Other.Force;
    Radius = Other.Radius;
    PosOffset = Other.PosOffset;
}

// ---------------------------------------------------------------------------
// ClientIgniteFuseAction
// ---------------------------------------------------------------------------

ClientIgniteFuseAction::ClientIgniteFuseAction()
    : bFuseLoaded(false)
{
    // / 0052a491-0052a49f.
    FuseId.clear();
}

FuseAction* ClientIgniteFuseAction::Slot04() const
{
    // .
    ClientIgniteFuseAction* Copy = new ClientIgniteFuseAction();
    Copy->Slot30(*this);
    return Copy;
}

bool ClientIgniteFuseAction::Slot08(INI_Reader& Reader)
{
    // .
    while (Reader.read_value())
    {
        if (ReadFuseActionValue(Reader))
            continue;
        if (Reader.is_value("fuse"))
            FuseId.Id = CreateID(Reader.get_value_string());
    }
    return true;
}

int ClientIgniteFuseAction::Load()
{
    // first time only, Fuse::Load on the target fuse in the exe database; always
    // FuseAction::Load, whose result is returned.
    if (!bFuseLoaded)
    {
        bFuseLoaded = true;
        ClientFuse* Target = ClientFuseDB::Find(FuseId.Id);
        if (Target != nullptr)
            Target->Load();
    }
    return FuseAction::Load();
}

int ClientIgniteFuseAction::UnLoad()
{
    // mirror of Load.
    if (bFuseLoaded)
    {
        bFuseLoaded = false;
        ClientFuse* Target = ClientFuseDB::Find(FuseId.Id);
        if (Target != nullptr)
            Target->UnLoad();
    }
    return FuseAction::UnLoad();
}

void ClientIgniteFuseAction::Slot30(ClientIgniteFuseAction const& Other)
{
    // (same body as StartCameraParticlesAction's): CopyArchProperties, +0x28.
    // bFuseLoaded is not copied.
    FuseAction::CopyArchProperties(Other);
    FuseId = Other.FuseId;
}

// ---------------------------------------------------------------------------
// ClientFuseDB
// ---------------------------------------------------------------------------

// . Its dynamic initializer (the branch -> the routine tree ctor, atexit
// tree dtor) is the std::map's own construction/destruction.
std::map<unsigned, ClientFuse*> ClientFuseDB::Fuses;

std::map<unsigned, ClientFuse*>& ClientFuseDB::Get()
{
    // `MOV EAX,the field`.
    return Fuses;
}

ClientFuse* ClientFuseDB::Find(unsigned FuseId)
{
    std::map<unsigned, ClientFuse*>::iterator It = Fuses.find(FuseId);
    if (It == Fuses.end())
        return nullptr;
    return It->second;
}

void ClientFuseDB::Clear()
{
    // virtual-delete every non-null prototype, then erase all nodes.
    for (std::map<unsigned, ClientFuse*>::iterator It = Fuses.begin(); It != Fuses.end(); ++It)
    {
        if (It->second != nullptr)
            delete It->second;
    }
    Fuses.clear();
}

void ClientFuseDB::LoadAll()
{
    // one the routine per path in FuseDB::m_FuseINIFiles (node+0xc = the
    // TString's characters).
    for (std::list<TString<260>>::iterator It = m_FuseINIFiles.begin(); It != m_FuseINIFiles.end(); ++It)
        LoadFile(It->Data);
}

void ClientFuseDB::LoadFile(char const* Path)
{
    // .
    INI_Reader Reader;
    if (!Reader.open(Path, false))
        return;
    Reader.read_header(); // result unused (0052a827)
    while (!Reader.is_end())
    {
        if (Reader.is_header("fuse"))
        {
            ClientFuse* NewFuse = new ClientFuse(ID_String{CreateID("")}, -1.0f);
            // A failed Read, or a nickname already in the map, leaks NewFuse (baseline).
            if (NewFuse->Read(Reader))
                Fuses.insert(std::map<unsigned, ClientFuse*>::value_type(NewFuse->Name().Id, NewFuse));
        }
        else
        {
            Reader.read_header();
        }
    }
}
