// Author: Giperion.

// CProjectile, CMunition, CGuided (baseline CGuided.cpp) and CBeam (baseline CBeam.cpp).
#include "common_cobject.h"
#include "common_cobject_internal.h" // CmnCobject::RotateWorldToObjectSpace, GetObjectDbCollisionGroup

#include "common_physys.h"
#include "common_net.h"
#include "common_iface.h"

#include "NeuronRuntime.h"

#include <cmath>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using CmnCobject::GetObjectDbCollisionGroup;
using CmnCobject::RotateWorldToObjectSpace;

namespace
{
    const char kCBeamSource[] = "Common\\CBeam.cpp";
    const char kCGuidedSource[] = "Common\\CGuided.cpp";


    // CBeam::init_physics module storage..062ad1a2: IEngine +0x7c SetInstanceUserData(
    // instance, this) -- no test on the instance or on NeuronRuntime::Engine.
    void SetInstanceUserData(long Instance, CBeam* Beam)
    {
        // IEngine takes the instance handle as a pointer-sized value.
        NeuronRuntime::Engine->SetInstanceUserData((void*)Instance, Beam);
    }
}

CProjectile::CProjectile(Class ObjectClass)
    : CSimple(ObjectClass)
{
    // Common 0x4fa90. Zeros owner + owner_safe; lifetime left until init.
    OwnerId = 0;
    OwnerSafeTime = 0.0f;
}

CProjectile::~CProjectile()
{
}

void CProjectile::init(CreateParms const& Parms)
{
    // OwnerId (+0xe4) = Parms+0x5c; CSimple::init; then the
    // archetype's (+0x88, no null test) Lifetime (+0x70) and OwnerSafeTime (+0x74) are
    // copied to +0xe8 / +0xec as raw dwords.
    OwnerId = Parms.OwnerId;
    CSimple::init(Parms);
    Archetype::Projectile const* const Proj = projarch();
    Lifetime = Proj->Lifetime;
    OwnerSafeTime = Proj->OwnerSafeTime;
}

int CProjectile::update(float Dt, unsigned Flags)
{
    CObject::update(Dt, Flags);
    Lifetime -= Dt;
    if (OwnerSafeTime > 0.0f)
    {
        OwnerSafeTime -= Dt;
        if (OwnerSafeTime <= 0.0f)
        {
            OwnerSafeTime = 0.0f;
            expire_safe_time();
        }
    }
    return 0;
}

Archetype::Projectile const* CProjectile::projarch() const
{
    // `MOV EAX,[this+0x88]; RET` (ICF fold). No null check in baseline.
    return (Archetype::Projectile const*)Archetype;
}

void CProjectile::set_dead()
{
    Lifetime = -1.0f;
}

unsigned int const& CProjectile::get_owner() const
{
    return OwnerId;
}

bool CProjectile::is_alive() const
{
    // Common 0x26070.
    return Lifetime >= 0.0f;
}

bool CProjectile::is_owner_safe() const
{
    // . FCOMP 0.0, TEST AH,0x5 / JNP -> true when OwnerSafeTime < 0;
    // FCOMP 0.0, TEST AH,0x41 / JZ -> true when OwnerSafeTime > 0; otherwise false. So an
    // exact 0.0 and an unordered (NaN) value both give false.
    return OwnerSafeTime < 0.0f || OwnerSafeTime > 0.0f;
}

CMunition::CMunition(Class ObjectClass)
    : CProjectile(ObjectClass)
{
    // Common (0x06286380), nine instructions:
    //   MOV EAX,[ESP+4] / PUSH ESI / PUSH EAX / MOV ESI,ECX
    //   CALL CProjectile::CProjectile / MOV [ESI],&CMunition::`vftable'
    //   MOV EAX,ESI / POP ESI / RET 4
    // __thiscall (this in ECX, RET 4 for the single stack arg). No field of its own is
    // touched, here or in the copy ctor at 0x062863a0 -- the body is empty by construction.
}

CMunition::~CMunition()
{
    // ??1CMunition@@UAE@XZ lives at 0x06286c80, which is a single `JMP CSimple::~CSimple`
    // ICF-folded with ??1CBeam, ??1CCounterMeasure, ??1CLoot, ??1CMine and ??1CProjectile
    // (Common lists all six mangled names on the one address). A shared
    // address across six unrelated-by-inheritance classes proves nothing about the receiver;
    // what it does tell us is that the body is the empty base-chain teardown, so nothing to
    // do here beyond letting ~CProjectile run.
}

Archetype::Munition const* CBeam::munitionarch() const
{
    // `MOV EAX,[this+0x88]; RET` (ICF fold), same body as CMunition's.
    return (Archetype::Munition const*)Archetype;
}

Archetype::Munition const* CMunition::munitionarch() const
{
    // `MOV EAX,[this+0x88]; RET` -- CObject+0x88 is Archetype. Folded with
    // thirteen other arch accessors (Common), so the label analysis picked
    // (CAsteroid::asteroidarch) is borrowed, not the receiver. baseline has no null check;
    // unlike CMine::minearch below, this port keeps the baseline shape exactly.
    return (Archetype::Munition const*)Archetype;
}

CGuided::CGuided(Class ObjectClass)
    : CMunition(ObjectClass)
{
    // . TargetWatch (BaseWatcher, +0xf0/+0xf4) default-constructs to a
    // null/unlinked watcher. The alignment word +0xfa and SeekerElapsed (+0x100) are not written here
    // (init() zeroes SeekerElapsed).
    SubTarget = SUBOBJ_ID_NONE;
    MotorArch = nullptr;
}

CGuided::~CGuided()
{
    // Common 0x4cc40: baseline explicitly unlinks the watcher (`if (this+0xf0 != 0)
    // set_pointer(this+0xf0, nullptr)`) before CSimple::~CSimple runs. BaseWatcher's own
    // destructor (~BaseWatcher -> set(nullptr)) does exactly this unlink, and runs
    // automatically right here since TargetWatch is now an embedded BaseWatcher member --
    // no explicit body needed.
}

void CGuided::init(CreateParms const& Parms)
{
    // Common 0x4ccb0. baseline: pWVar2 = Target ? (Watchable*)((char*)Target+8) : nullptr --
    // the IObjRW -> Watchable base conversion (Watchable at +8, null stays null); then only
    // relinks the watcher if the target actually changed. BaseWatcher::set() already carries
    // that same de-dup guard (if (Target != Watched) set_pointer(Target)).
    CProjectile::init(Parms);
    Watchable const* NewTarget = Parms.Target;
    TargetWatch.set(NewTarget);
    SubTarget = Parms.SubTarget;
    SeekerElapsed = 0.0f;
    // archetype (+0x88) read with no null test; an empty Motor gives 0.
    Archetype::Munition const* const Mun = munitionarch();
    if (Mun->Motor.IsEmpty())
        MotorArch = nullptr;
    else
        MotorArch = Archetype::GetMotor(Mun->Motor.Id);
}

int CGuided::update(float Dt, unsigned Flags)
{
    SeekerElapsed += Dt;
    return CProjectile::update(Dt, Flags);
}

bool __cdecl CGuided::seeker_can_see(Vector const& From, Vector const& Target, Vector const& Forward,
                                     Archetype::Munition const* Arch)
{
    // . D = Target - From, each component stored to float
    // (062acd6b-062acd89). Every later term stays on the x87 stack (double here).
    float const Dx = Target.x - From.x;
    float const Dy = Target.y - From.y;
    float const Dz = Target.z - From.z;
    // |D|^2 = (Dy^2 + Dz^2) + Dx^2; 062acdb1-062acdb3: SeekerRange^2.
    double const DistSq = ((double)Dy * Dy + (double)Dz * Dz) + (double)Dx * Dx;
    double const Range = Arch->SeekerRange;
    // FCOMPP R^2 vs |D|^2, TEST AH,0x41 / JNZ -> false unless R^2 > |D|^2
    // (equal and unordered fail).
    if (!(Range * Range > DistSq))
        return false;
    // InvLen = 1.0 / FSQRT(|D|^2), unrounded.
    double const InvDist = 1.0 / sqrt(DistSq);
    // dot = ((Dz*InvLen)*F.z + (Dy*InvLen)*F.y) + (Dx*InvLen)*F.x.
    double const Dot = (Dz * InvDist * Forward.z + Dy * InvDist * Forward.y) + Dx * InvDist * Forward.x;
    // FCOMP SeekerFovCos (+0x9c), TEST AH,0x1 / JNZ -> false when the dot is
    // below it or unordered.
    return Dot >= Arch->SeekerFovCos;
}

bool CGuided::seeker_can_see(Vector const& Pos) const
{
    // (member overload; the static 4-arg overload at 062acd60 is
    // above). Forward is Orientation's column 2 negated (062ace23-062ace4b; negation is exact).
    float const ForwardX = -Orientation.E[0][2];
    float const ForwardY = -Orientation.E[1][2];
    float const ForwardZ = -Orientation.E[2][2];
    // Dx, Dy stored to float; Dz is stored too, but FST leaves the
    // unrounded difference on the stack and that is what the first square multiplies.
    float const Dx = Pos.x - WorldPosition.x;
    float const Dy = Pos.y - WorldPosition.y;
    double const DzWide = (double)Pos.z - WorldPosition.z;
    float const Dz = (float)DzWide;
    // |D|^2 = (DzWide*Dz + Dy^2) + Dx^2, unrounded.
    double const DistSq = (DzWide * Dz + (double)Dy * Dy) + (double)Dx * Dx;

    // Archetype (+0x88) read with no null test.
    Archetype::Munition const* const Mun = munitionarch();
    double const Range = Mun->SeekerRange;
    // FCOMPP |D|^2 vs R^2, TEST AH,0x5 / JP -> false unless |D|^2 < R^2
    // (equal and unordered fail).
    if (!(DistSq < Range * Range))
        return false;

    // InvLen = 1.0 / FSQRT(|D|^2), unrounded.
    double const InvDist = 1.0 / sqrt(DistSq);
    // dot = ((Dz*InvLen)*F.z + (Dy*InvLen)*F.y) + (Dx*InvLen)*F.x.
    double const Dot = (Dz * InvDist * ForwardZ + Dy * InvDist * ForwardY) + Dx * InvDist * ForwardX;
    // FCOMP SeekerFovCos (+0x9c), TEST AH,0x1 / JNZ -> false when below or
    // unordered.
    return Dot >= Mun->SeekerFovCos;
}

void CGuided::init_physics(Vector const& Velocity, Vector const& AngularVelocity)
{
    // . The parms start from the PhySys defaults (mass 1, linear /
    // angular damping) and are then overwritten: mass = archetype Mass (+0x20, no null test),
    // both dampings 0, Type 2, +0x1c = 0, bEnableCollisions 1, Name = _itoa(OwnerId +0xe4,
    // radix 24), +0x26 and +0x28..+0x30 = 0, system id from IObjDB. The bytes baseline leaves
    // unwritten (Name tail, +0x27) are zero here.
    PhySys::CreateParms Phy;
    memset(&Phy, 0, sizeof(Phy));
    Phy.Type = 2;
    Phy.bEnableCollisions = 1;
    _itoa((int)OwnerId, Phy.Name, 0x18);
    Phy.Mass = Archetype->Mass;
    Phy.CollisionGroupId = GetObjectDbCollisionGroup(ObjectDb);
    PhySys::BuildIVP_Real(this, Phy);
    PhySys::AddToVelocity(this, Velocity);
    // Common 0x4cf00: rotate world-space AngularVelocity into object space
    // via Orientation^T before adding it — see RotateWorldToObjectSpace above.
    PhySys::AddToAngularVelocityOS(this, RotateWorldToObjectSpace(Orientation, AngularVelocity));
}

void CGuided::expire_safe_time()
{
    // Common 0x4ca10
    PhySys::SetCollisionGroup(this, "");
}

IObjRW* CGuided::get_target() const
{
    // Common 0x26890. baseline ?get_target@CGuided@@QBEPAUIObjRW@@XZ: the Watchable
    // sits at IObjRW+8, so the owning IObjRW is 8 bytes before it (null stays null).
    Watchable const* Watched = TargetWatch.target();
    if (Watched == nullptr)
        return nullptr;
    return static_cast<IObjRW*>(const_cast<Watchable*>(Watched));
}

void CGuided::set_target(IObjRW* Target)
{
    // gated on archetype (+0x88, no null test) Seeker (+0x90) != 0;
    // W = Target ? Target + 8 : 0; W != [+0xf0] -> BaseWatcher::set_pointer(W) (tail jump).
    // BaseWatcher::set() carries the same de-dup test.
    if (!munitionarch()->is_guided())
        return;
    Watchable const* NewTarget = Target;   // IObjRW -> its Watchable base (+8), null stays null
    TargetWatch.set(NewTarget);
}

unsigned short CGuided::get_sub_target() const
{
    return SubTarget;
}

void CGuided::set_sub_target(unsigned short SubTargetId)
{
    // only a guided munition (archetype +0x88, no null test, Seeker
    // +0x90 != 0) stores the word into SubTarget (+0xf8).
    if (munitionarch()->is_guided())
        SubTarget = SubTargetId;
}

bool CGuided::motor_on() const
{
    // . No motor (+0xfc) -> 0. FLD Elapsed(+0x100) FCOMP Delay(+8);
    // TEST AH,0x1 / JNZ (C0: less, or unordered) -> 0. FLD Lifetime(+0x10) FADD Delay
    // (unrounded) FCOMP Elapsed; TEST AH,0x1 / JNZ (sum less, or unordered) -> 0. Else 1.
    if (MotorArch == nullptr)
        return false;
    if (!(SeekerElapsed >= MotorArch->Delay))
        return false;
    if (!((double)MotorArch->Lifetime + MotorArch->Delay >= SeekerElapsed))
        return false;
    return true;
}

Archetype::MotorData const* CGuided::get_motor_arch() const
{
    return MotorArch;
}

CBeam::CBeam(Class ObjectClass)
    : CMunition(ObjectClass)
{
    bMoved = 0;
}

CBeam::~CBeam()
{
}

void CBeam::init_physics(Vector const& Velocity, Vector const& AngularVelocity)
{
    // Commonad190: IEngine+0x7c SetUserData, itoa owner radix 0x18, copy vel.
    (void)AngularVelocity;
    SetInstanceUserData(EngineInstanceId, this);
    _itoa((int)OwnerId, CollisionGroup, 0x18);
    this->Velocity = Velocity;
}

void CBeam::expire_safe_time()
{
    CollisionGroup[0] = '\0';
}

Vector CBeam::get_velocity() const
{
    return Velocity;
}

float CBeam::get_mass() const
{
    return 0.0f;
}

void CBeam::move(float Dt)
{
    // . FCOMP 0.0, TEST AH,0x1 / JNZ: a negative or unordered
    // Lifetime returns.
    if (!(Lifetime >= 0.0f))
        return;
    if (bMoved == 0)
    {
        OriginPos = WorldPosition;
        LastPos = WorldPosition;
        bMoved = 1;
    }
    LastPos = WorldPosition;
    // Dt*V.x and Dt*V.y stay on the x87 stack and are added unrounded
    // (one rounding at FSTP); Dt*V.z is stored to a float local first (062ad129).
    float const StepZ = Dt * Velocity.z;
    WorldPosition.x = (float)((double)Dt * Velocity.x + WorldPosition.x);
    WorldPosition.y = (float)((double)Dt * Velocity.y + WorldPosition.y);
    WorldPosition.z = StepZ + WorldPosition.z;
}

void CBeam::set_velocity(Vector const& Vel)
{
    Velocity = Vel;
}

Vector const& CBeam::get_last_pos() const
{
    if (bMoved != 0)
        return LastPos;
    return WorldPosition;
}

char const* CBeam::get_collision_group() const
{
    return CollisionGroup;
}
