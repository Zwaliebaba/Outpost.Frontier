// Author: Giperion.

// Server.dll Guided (missile) and Bullet (gun round) wrappers -- baseline module storage-06cf4ecf,
// minus the Vector helpers the compiler emitted out of line (the routine .. the routine,
// inlined here) and the two damage bodies Guided shares with Mine (module storage /
// , see server_mine.cpp). The classes are declared in src/include/server_gameobject.h.

// Guided layout (baseline offsets): Munition/Projectile/GameObject +0x00..+0x5f,
// PhySys::Controller +0x60 (vtable module storage), PhySys::CObjectCB +0x70 (vtable
// ), then +0x74..+0x7f. The spawner (StarSystem the routine) allocates 0x80.
// Bullet has no field of its own (operator new(0x60) in the routine).

#include "../include/server_gameobject.h"
#include "../include/server_sysdb.h"       // StarSystem::explode / munition_collision / find_object
#include "../include/server_createinfo.h"  // Server_Acquire3DMathEngine (the routine)
#include "../include/x86math.h"            // I3DMathEngine::TransformMatrix
#include "../include/common_misc.h"        // SUBOBJ_ID_ROOT, DamageList
#include "../include/common_id.h"          // ID_String::IsEmpty
#include "../include/common_csys.h"        // Matrix, Transform (get_transform)
#include "../include/NeuronRuntime.h"              // NeuronRuntime::Engine (create, IEngine +0x6c)
#include "../include/common_ivp_controller.h" // IVP_Event_Sim::delta_time

#include <cmath>
#include <cstdio>

// .data:the field -- Guided.cpp's own lazily fetched math engine (the routine).
static I3DMathEngine* GGuidedMathEngine = nullptr;

// ---------------------------------------------------------------------------
// (module storage, cdecl, hidden return slot): Matrix * Vector through
// I3DMathEngine slot 11, TransformMatrix (vt+0x2c, module storage). The engine is fetched
// once (the routine) and never null-tested; the result is copied out of a stack temporary.
// ---------------------------------------------------------------------------
static Vector Guided_TransformMatrix(Matrix const& M, Vector const& V)
{
    if (GGuidedMathEngine == nullptr)
        GGuidedMathEngine = Server_Acquire3DMathEngine();
    FVector Tmp;
    GGuidedMathEngine->TransformMatrix(Tmp, reinterpret_cast<FMatrix const&>(M),
                                       reinterpret_cast<FVector const&>(V));
    Vector Out;
    Out.x = Tmp.X;
    Out.y = Tmp.Y;
    Out.z = Tmp.Z;
    return Out;
}

// ===========================================================================
// Guided
// ===========================================================================

// ---------------------------------------------------------------------------
// (module storage, RET 0).
// ---------------------------------------------------------------------------
Guided::Guided()
    : Phantom(nullptr)
    , bTriggered(false)
    , TriggerObjId(0)
{
    // the routine (GameObject's constructor), the Munition vtables
    // (module storage-06cf3d11), PhySys::Controller's constructor on +0x60
    // (module storage), then module storage +0x74 = 0, module storage byte +0x78 = 0,
    // +0x7c = 0, and the Guided vtables.
}

// ---------------------------------------------------------------------------
// (module storage), entered on the CBase sub-object (+0xc).
// ---------------------------------------------------------------------------
Guided::~Guided()
{
    // `CALL [module storage]` -- the +0x15c entry of Guided's own table,
    // i.e. destroy_notify as seen from ~Guided, which is GameObject's (the routine).
    GameObject::destroy_notify();

    // PhySys::DestroyPhantom(+0x74) -- no null test -- then
    // +0x74 = 0. PhySys::Controller's destructor (module storage) and GameObject's
    // (the routine, module storage) follow as the base-class destructors.
    PhySys::DestroyPhantom(Phantom);
    Phantom = nullptr;
}

// (module storage): `MOV EAX,0x1000000 / RET`.
unsigned int Guided::get_type_bits() const
{
    return 0x1000000u;
}

CGuided* Guided::cguided() const
{
    CObject* const Obj = Object;
    if (Obj == nullptr || (static_cast<unsigned>(Obj->ObjectClass) & 0xa7u) != 0xa7u)
        return nullptr;
    return static_cast<CGuided*>(Obj);
}

// ---------------------------------------------------------------------------
// (module storage, RET 4) -- vtable +0x164.
// ---------------------------------------------------------------------------
bool Guided::create(Archetype::Root* Arch)
{
    // CObject::Alloc(0xa7) into CBase::Object.
    Object = CObject::Alloc(CObject::ClassGuided);

    // Archetype::Root::create_instance(Object) -- before open.
    Arch->create_instance(static_cast<IEngineInstance*>(Object));

    // NeuronRuntime::Engine vt+0x6c, UpdateInstance(Object+4, 0, 0).
    NeuronRuntime::Engine->UpdateInstance(reinterpret_cast<void*>(Object->EngineInstanceId), 0, nullptr);

    // open(Arch) (vt+0x4c) on the (ObjectClass & 0xa7) == 0xa7
    // narrowing, not null-tested.
    CObject* Narrowed = Object;
    if (Narrowed == nullptr || (static_cast<unsigned>(Narrowed->ObjectClass) & 0xa7u) != 0xa7u)
        Narrowed = nullptr;
    Narrowed->open(Arch);

    // CBase::advise(true).
    advise(true);

    // get_index() (vt+0x1c) != -1.
    return get_index() != -1;
}

// ---------------------------------------------------------------------------
// (module storage, RET 4) -- vtable +0x168.
// ---------------------------------------------------------------------------
bool Guided::find(unsigned int const& Id)
{
    // CObject::Find(Id, 0xa7) into CBase::Object, then
    // CBase::advise(true); true when something was found.
    Object = CObject::Find(Id, CObject::ClassGuided);
    advise(true);
    return Object != nullptr;
}

// ---------------------------------------------------------------------------
// (module storage, RET 0) -- vtable +0x1bc.
// ---------------------------------------------------------------------------
void Guided::arm()
{
    // PhySys::Controller::Register(this ? this+0x60 : 0, Object).
    PhySys::Controller::Register(static_cast<PhySys::Controller*>(this), Object);

    // nothing more without a detonation distance
    // (Munition+0xa4, squared; TEST AH,0x41 -- only strictly positive goes on).
    Archetype::Munition const* const Arch =
        static_cast<Archetype::Munition const*>(Object->get_archetype());
    if (!(Arch->DetonationDistSq > 0.0f))
        return;

    // PhantomParms on the stack -- +0x00 = 0, +0x04/+0x05/+0x06
    // bytes = 0, position 0, identity orientation, listener 0.
    PhySys::PhantomParms Parms;
    Parms.SystemId = 0;
    Parms.Flag04 = 0;
    Parms.Flag05 = 0;
    Parms.Name[0] = '\0';
    Parms.Position.x = 0.0f;
    Parms.Position.y = 0.0f;
    Parms.Position.z = 0.0f;
    Parms.Orientation = Matrix(1.0f, 0.0f, 0.0f,
                               0.0f, 1.0f, 0.0f,
                               0.0f, 0.0f, 1.0f);
    Parms.Listener = nullptr;

    // the system id (the routine, the system's nickname id).
    Parms.SystemId = OwnerSystem->SystemInfo->NicknameId;

    // sprintf(Name, PhySys::GetCollisionGroup(Object)) -- the
    // group name is the format string, exactly as baseline passes it.
    std::sprintf(Parms.Name, PhySys::GetCollisionGroup(Object));

    // this object's position (vt[0]), the CObjectCB sub-object
    // (+0x70) as listener, +0x05 = 1.
    Parms.Position = get_position();
    Parms.Listener = static_cast<PhySys::CObjectCB*>(this);
    Parms.Flag05 = 1;

    // radius = FSQRT(DetonationDistSq) stored as a float;
    // CreatePhantom(Parms, radius, +0x74). module storage AttachPhantom(Object, +0x74).
    float const Radius = static_cast<float>(std::sqrt(static_cast<double>(Arch->DetonationDistSq)));
    PhySys::CreatePhantom(Parms, Radius, Phantom);
    PhySys::AttachPhantom(Object, Phantom);
}

// ---------------------------------------------------------------------------
// (module storage, RET 4) -- vtable +0x16c.
// ---------------------------------------------------------------------------
bool Guided::update(float Dt)
{
    // GameObject's base update; false is returned as is.
    if (!GameObject::update(Dt))
        return false;

    // the CGuided's own update (vt+0x50) with UPDATE_DEFAULT; the
    // narrowing is not null-tested.
    CGuided* const Cg = cguided();
    Cg->update(Dt, CObject::UPDATE_DEFAULT);

    // FCOMP 0.0f, TEST AH,1 -- a spent lifetime (CProjectile+0xe8
    // below zero, or unordered) is a kill: destroy(1, 0) (module storage-06cf4208).
    if (!(Cg->Lifetime >= 0.0f))
    {
        destroy(1, 0);
        return !is_destroyed();
    }

    // the phantom's collisions go on first; once they are on,
    // a touch latched by added() detonates: destroy(1, TriggerObjId).
    if (!PhySys::GetCollisionStatePhantom(Phantom))
        PhySys::SetCollisionStatePhantom(Phantom, true);
    else if (bTriggered)
        destroy(1, TriggerObjId);

    // PhySys::Controller::wakeup on +0x60.
    wakeup();

    // only a lock-on seeker (Munition+0x90 == 2; baseline reads it
    // twice: zero, then minus two) steers.
    Archetype::Munition const* const Arch =
        static_cast<Archetype::Munition const*>(Object->get_archetype());
    if (Arch->Seeker == 0)
        return !is_destroyed();
    if (Arch->Seeker != 2)
        return !is_destroyed();

    // ... and only with a target (CGuided+0xf0, the watched
    // IObjRW+8; the narrowing is not null-tested).
    if (cguided()->get_target() == nullptr)
        return !is_destroyed();

    // the aim point, into [ESP+0xc]. The target is read again;
    // a sub-targeted CEqObj ((ObjectClass & 0x103) == 0x103, sub-target not
    // SUBOBJ_ID_ROOT) gives its sub-object's centre of mass (CEqObj vt+0xe0); otherwise
    // -- or when that call fails -- the target's centre of mass (IObjInspect vt+0x14).
    // baseline leaves the slot unwritten when the second read finds no target, which the
    // first read just ruled out; the port starts it at zero.
    Vector TargetPos;
    TargetPos.x = 0.0f;
    TargetPos.y = 0.0f;
    TargetPos.z = 0.0f;
    IObjRW* const PosTarget = cguided()->get_target();
    if (PosTarget != nullptr)
    {
        bool bHavePos = false;
        CObject const* const TargetObj = PosTarget->cobject();                     // vt+0x150
        if (TargetObj != nullptr &&
            (static_cast<unsigned>(TargetObj->ObjectClass) & 0x103u) == 0x103u)
        {
            unsigned short const SubTarget = cguided()->SubTarget;
            if (SUBOBJ_ID_ROOT != SubTarget)
            {
                bHavePos = static_cast<CEqObj const*>(TargetObj)->get_sub_obj_center_of_mass(
                    cguided()->SubTarget, TargetPos);
            }
        }
        if (!bHavePos)
            TargetPos = PosTarget->get_center_of_mass();
    }

    // CGuided::seeker_can_see(aim point).
    if (cguided()->seeker_can_see(TargetPos))
    {
        // the target's velocity, into [ESP+0x18] -- the same
        // shape: a sub-targeted CEqObj's sub-object velocity (CEqObj vt+0xdc), otherwise
        // IObjInspect::get_velocity (vt+4).
        Vector TargetVel;
        TargetVel.x = 0.0f;
        TargetVel.y = 0.0f;
        TargetVel.z = 0.0f;
        IObjRW* const VelTarget = cguided()->get_target();
        if (VelTarget != nullptr)
        {
            bool bHaveVel = false;
            CObject const* const TargetObj = VelTarget->cobject();
            if (TargetObj != nullptr &&
                (static_cast<unsigned>(TargetObj->ObjectClass) & 0x103u) == 0x103u)
            {
                unsigned short const SubTarget = cguided()->SubTarget;
                if (SUBOBJ_ID_ROOT != SubTarget)
                {
                    bHaveVel = static_cast<CEqObj const*>(TargetObj)->get_sub_obj_velocity(
                        cguided()->SubTarget, TargetVel);
                }
            }
            if (!bHaveVel)
                TargetVel = VelTarget->get_velocity();
        }

        // the routine(aim point, velocity).
        seek(TargetPos, TargetVel);
    }
    else
    {
        // lost it -- IObjAffect (+4) vt+0x48, this Guided's own
        // set_target(nullptr, SUBOBJ_ID_ROOT, 0).
        set_target(nullptr, SUBOBJ_ID_ROOT, 0);
    }

    // true unless destroyed.
    return !is_destroyed();
}

// ---------------------------------------------------------------------------
// (module storage, RET 8). F below is ESP after `PUSH ESI`; every
// out-of-line Vector helper (the routine subtract, the routine scale, the routine
// length, the routine negate, the routine add, the routine construct) stores floats,
// so each intermediate below is rounded to float where baseline stores it.
// ---------------------------------------------------------------------------
void Guided::seek(Vector const& TargetPos, Vector const& TargetVel)
{
    // F+0x20 = TargetPos - position (vt[0]), the routine.
    Vector const& MyPos = get_position();
    Vector RelPos;
    RelPos.x = static_cast<float>(static_cast<double>(TargetPos.x) - static_cast<double>(MyPos.x));
    RelPos.y = static_cast<float>(static_cast<double>(TargetPos.y) - static_cast<double>(MyPos.y));
    RelPos.z = static_cast<float>(static_cast<double>(TargetPos.z) - static_cast<double>(MyPos.z));

    // F+0x38 = TargetVel - velocity (vt+4), the routine.
    Vector const MyVel = get_velocity();
    Vector RelVel;
    RelVel.x = static_cast<float>(static_cast<double>(TargetVel.x) - static_cast<double>(MyVel.x));
    RelVel.y = static_cast<float>(static_cast<double>(TargetVel.y) - static_cast<double>(MyVel.y));
    RelVel.z = static_cast<float>(static_cast<double>(TargetVel.z) - static_cast<double>(MyVel.z));

    // (vz*pz + vy*py) + vx*px against 0.0f, TEST AH,5 / JP --
    // the turn happens only while the target closes in (strictly negative).
    double const Closing =
        (static_cast<double>(RelVel.z) * static_cast<double>(RelPos.z) +
         static_cast<double>(RelVel.y) * static_cast<double>(RelPos.y)) +
        static_cast<double>(RelVel.x) * static_cast<double>(RelPos.x);
    if (Closing < 0.0)
    {
        // F+0x44 = RelPos x RelVel (the routine, z then y then x).
        Vector Cross;
        Cross.z = static_cast<float>(static_cast<double>(RelVel.y) * static_cast<double>(RelPos.x) -
                                     static_cast<double>(RelPos.y) * static_cast<double>(RelVel.x));
        Cross.y = static_cast<float>(static_cast<double>(RelPos.z) * static_cast<double>(RelVel.x) -
                                     static_cast<double>(RelVel.z) * static_cast<double>(RelPos.x));
        Cross.x = static_cast<float>(static_cast<double>(RelVel.z) * static_cast<double>(RelPos.y) -
                                     static_cast<double>(RelPos.z) * static_cast<double>(RelVel.y));

        // |RelPos|^2 = (z*z + y*y) + x*x; FCOM 0.0001f
        // (module storage), TEST AH,1 -- below it (or unordered) no turn.
        double const DistSq =
            (static_cast<double>(RelPos.z) * static_cast<double>(RelPos.z) +
             static_cast<double>(RelPos.y) * static_cast<double>(RelPos.y)) +
            static_cast<double>(RelPos.x) * static_cast<double>(RelPos.x);
        if (DistSq >= static_cast<double>(0.0001f))
        {
            // F+0x68 = Cross * float(1.0 / |RelPos|^2)
            // (module storage double 1.0, FDIVR), the routine -- the line-of-sight rate.
            float const InvDistSq = static_cast<float>(1.0 / DistSq);
            Vector Omega;
            Omega.x = static_cast<float>(static_cast<double>(InvDistSq) * static_cast<double>(Cross.x));
            Omega.y = static_cast<float>(static_cast<double>(InvDistSq) * static_cast<double>(Cross.y));
            Omega.z = static_cast<float>(static_cast<double>(InvDistSq) * static_cast<double>(Cross.z));

            // F+0x8 = Omega * 3.0f (module storage), the routine.
            Vector Desired;
            Desired.x = static_cast<float>(3.0 * static_cast<double>(Omega.x));
            Desired.y = static_cast<float>(3.0 * static_cast<double>(Omega.y));
            Desired.z = static_cast<float>(3.0 * static_cast<double>(Omega.z));

            // |Desired| = FSQRT((y*y + z*z) + x*x), stored float.
            float const Magnitude = static_cast<float>(std::sqrt(
                (static_cast<double>(Desired.y) * static_cast<double>(Desired.y) +
                 static_cast<double>(Desired.z) * static_cast<double>(Desired.z)) +
                static_cast<double>(Desired.x) * static_cast<double>(Desired.x)));

            // against MaxAngularVelocity (Munition+0xa0),
            // TEST AH,0x41 -- only a strictly larger rate is clamped.
            Archetype::Munition const* const Arch =
                static_cast<Archetype::Munition const*>(Object->get_archetype());
            if (Magnitude > Arch->MaxAngularVelocity)
            {
                // normalise -- the routine's (x*x + y*y) + z*z,
                // 1.0f / that length kept on the FPU, each component stored.
                double const Length = std::sqrt(
                    (static_cast<double>(Desired.x) * static_cast<double>(Desired.x) +
                     static_cast<double>(Desired.y) * static_cast<double>(Desired.y)) +
                    static_cast<double>(Desired.z) * static_cast<double>(Desired.z));
                double const Inverse = 1.0 / Length;
                Desired.x = static_cast<float>(static_cast<double>(Desired.x) * Inverse);
                Desired.y = static_cast<float>(static_cast<double>(Desired.y) * Inverse);
                Desired.z = static_cast<float>(static_cast<double>(Desired.z) * Inverse);

                // times MaxAngularVelocity (the routine into
                // F+0x14, copied back to F+0x8).
                double const MaxRate = static_cast<double>(Arch->MaxAngularVelocity);
                Desired.x = static_cast<float>(MaxRate * static_cast<double>(Desired.x));
                Desired.y = static_cast<float>(MaxRate * static_cast<double>(Desired.y));
                Desired.z = static_cast<float>(MaxRate * static_cast<double>(Desired.z));
            }

            // F+0x5c = -PhySys::GetAngularVelocityOS(Object)
            // (the routine).
            Vector const AngularVelocity = PhySys::GetAngularVelocityOS(Object);
            Vector NegAngular;
            NegAngular.x = -AngularVelocity.x;
            NegAngular.y = -AngularVelocity.y;
            NegAngular.z = -AngularVelocity.z;

            // F+0x74 = the transpose of the object's orientation
            // (CObject+0x08, the routine filled column by column).
            Matrix const& Orientation = Object->Orientation;
            Matrix const Transposed(Orientation.E[0][0], Orientation.E[1][0], Orientation.E[2][0],
                                    Orientation.E[0][1], Orientation.E[1][1], Orientation.E[2][1],
                                    Orientation.E[0][2], Orientation.E[1][2], Orientation.E[2][2]);

            // F+0x14 = Transposed * Desired (the routine) -- the
            // wanted rate in object space.
            Vector const LocalRate = Guided_TransformMatrix(Transposed, Desired);

            // F+0x50 = -current + wanted (the routine), added to the
            // object-space angular velocity: the rate becomes the wanted one.
            Vector Change;
            Change.x = static_cast<float>(static_cast<double>(NegAngular.x) + static_cast<double>(LocalRate.x));
            Change.y = static_cast<float>(static_cast<double>(NegAngular.y) + static_cast<double>(LocalRate.y));
            Change.z = static_cast<float>(static_cast<double>(NegAngular.z) + static_cast<double>(LocalRate.z));
            PhySys::AddToAngularVelocityOS(Object, Change);
        }
    }

    // F+0x2c = velocity (vt+4); F+0x14 = minus the third column of
    // get_transform (vt+0xc) -- the nose.
    Vector const Velocity = get_velocity();
    Transform const& Xform = get_transform();
    Vector Nose;
    Nose.z = -Xform.Rotation.E[2][2];
    Nose.y = -Xform.Rotation.E[1][2];
    Nose.x = -Xform.Rotation.E[0][2];

    // (nz*vz + ny*vy) + vx*nx against the double 0.0
    // (module storage), TEST AH,0x41 -- nothing unless flying nose first.
    double const Along =
        (static_cast<double>(Nose.z) * static_cast<double>(Velocity.z) +
         static_cast<double>(Nose.y) * static_cast<double>(Velocity.y)) +
        static_cast<double>(Velocity.x) * static_cast<double>(Nose.x);
    if (!(Along > 0.0))
        return;

    // |velocity| = FSQRT((z*z + y*y) + x*x), stored float.
    float const Speed = static_cast<float>(std::sqrt(
        (static_cast<double>(Velocity.z) * static_cast<double>(Velocity.z) +
         static_cast<double>(Velocity.y) * static_cast<double>(Velocity.y)) +
        static_cast<double>(Velocity.x) * static_cast<double>(Velocity.x)));

    // the third column again (vt+0xc), negated by the routine.
    Transform const& Xform2 = get_transform();
    Vector Forward;
    Forward.x = -Xform2.Rotation.E[0][2];
    Forward.y = -Xform2.Rotation.E[1][2];
    Forward.z = -Xform2.Rotation.E[2][2];

    // F+0x14 = Forward * Speed (the routine).
    Vector Wanted;
    Wanted.z = static_cast<float>(static_cast<double>(Forward.z) * static_cast<double>(Speed));
    Wanted.y = static_cast<float>(static_cast<double>(Forward.y) * static_cast<double>(Speed));
    Wanted.x = static_cast<float>(static_cast<double>(Forward.x) * static_cast<double>(Speed));

    // F+0x44 = -velocity.
    Vector NegVelocity;
    NegVelocity.z = -Velocity.z;
    NegVelocity.y = -Velocity.y;
    NegVelocity.x = -Velocity.x;

    // F+0x50 = Wanted + NegVelocity -> PhySys::AddToVelocity:
    // the whole speed is put back on the nose.
    Vector Change;
    Change.z = static_cast<float>(static_cast<double>(Wanted.z) + static_cast<double>(NegVelocity.z));
    Change.y = static_cast<float>(static_cast<double>(Wanted.y) + static_cast<double>(NegVelocity.y));
    Change.x = static_cast<float>(static_cast<double>(Wanted.x) + static_cast<double>(NegVelocity.x));
    PhySys::AddToVelocity(Object, Change);
}

// ---------------------------------------------------------------------------
// (module storage, RET 8) -- vtable +0x158.
// ---------------------------------------------------------------------------
bool Guided::destroy(int Kind, unsigned int ObjId)
{
    // GameObject's destroy (the routine); its result is not used.
    GameObject::destroy(Kind, ObjId);

    // only a real kill, and not one by a countermeasure
    // (StarSystem the routine with type mask 0x8000000 finds the killer there).
    if (Kind != 1)
        return true;
    if (OwnerSystem->find_object(ObjId, 0x8000000u) != nullptr)
        return true;

    // an explosion archetype with a radius and some effect (the
    // double 0.0001 at module storage).
    Archetype::Root* const Arch = Object->get_archetype();
    if (Arch->ExplosionArch.IsEmpty())
        return true;
    Archetype::Explosion* const Explosion = Archetype::GetExplosion(Arch->ExplosionArch);
    if (Explosion == nullptr)
        return true;
    double const Epsilon = 0.0001;
    if (!(static_cast<double>(Explosion->Radius) > Epsilon))
        return true;
    if (!(static_cast<double>(Explosion->Impulse) > Epsilon) &&
        !(static_cast<double>(Explosion->HullDamage) > Epsilon) &&
        !(static_cast<double>(Explosion->EnergyDamage) > Epsilon))
        return true;

    // {own id, the CGuided's owner (+0xe4, 0xa7 narrowing, not
    // null-tested), cause 6 for a cruise disruptor (Munition+0xac) else 5 (after a first
    // store of 4), position, explosion, no skipped collision group} -> StarSystem::explode.
    ExplosionDamageEvent Blast;
    Blast.ExplodingObjId = get_id();
    Blast.InflictorId = cguided()->OwnerId;
    Archetype::Munition const* const MunitionArch =
        static_cast<Archetype::Munition const*>(Object->get_archetype());
    Blast.Cause = static_cast<DamageCause>(MunitionArch->bCruiseDisruptor ? 6 : 5);
    Blast.Position = get_position();
    Blast.Explosion = Explosion;
    Blast.SkipCollisionGroup = nullptr;
    OwnerSystem->explode(Blast);

    // always true.
    return true;
}

// (RET 8) -- Guided's +0x17c, the body Mine::damage_sub_obj ports.
void Guided::damage_sub_obj(FSubObjDamage const& Hit, DamageList& Damage)
{
    (void)Hit;
    Damage.bDestroyed = true;
}

// (RET 8) -- Guided's +0x180, the body Mine::munition_hit ports.
void Guided::munition_hit(FMunitionImpact const& Hit, DamageList& Damage)
{
    if (Hit.Munition->Seeker != 0)
    {
        Damage.bDestroyed = true;
        return;
    }
    GameObject::munition_hit(Hit, Damage);
}

// ---------------------------------------------------------------------------
// (module storage, RET 4) -- vtable +0x1a4.
// ---------------------------------------------------------------------------
void Guided::on_collision(FCollisionEvent const& Hit)
{
    // nothing for an object without an id (vt+0x20).
    if (Hit.Other->get_id() == 0)
        return;

    // the CGuided (0xa7 narrowing), used later, not null-tested.
    CGuided* const Cg = cguided();

    // a player-owned object (its CSimple+0xb4 under the & 3
    // narrowing, not null-tested) handles the hit on its own client.
    if (Hit.Other->csimple()->OwnerPlayer != 0)
        return;

    // the record at [ESP+0xc] -- munition archetype id
    // (get_archetype()+0x08 on the raw Object), owner (CGuided+0xe4), the object hit,
    // the sub-object hit (Hit+8) -> StarSystem::munition_collision (the routine).
    // baseline never writes the record's +0x10..+0x1b (hit position); the port zeroes it.
    SSPMunitionCollisionInfo Info = {};
    Info.Field_0x00 = Object->get_archetype()->Nickname.Id;
    Info.Field_0x04 = Cg->OwnerId;
    Info.ObjectId = Hit.Other->get_id();
    Info.Field_0x0c = Hit.OtherSubObjId;
    OwnerSystem->munition_collision(Info);
}

// ---------------------------------------------------------------------------
// (module storage, RET 4) -- CObjectCB slot 0 on the +0x70 sub-object.
// ---------------------------------------------------------------------------
void Guided::added(CObject* Obj)
{
    // the id of the root, when the root is a CSimple; else 0.
    CObject* const Root = GetRoot(Obj);
    unsigned int Id = 0;
    if (Root != nullptr && (static_cast<unsigned>(Root->ObjectClass) & 3u) == 3u)
        Id = static_cast<CSimple*>(Root)->Id;

    // the owner is exempt while its owner-safe time runs (the
    // CGuided narrowing is not null-tested).
    CGuided* const Cg = cguided();
    if (Id != 0 && Id == Cg->OwnerId && Cg->is_owner_safe())
        return;

    // .
    bTriggered = true;
    TriggerObjId = Id;
}

// `RET 4` -- CObjectCB slot 1 of Guided's table: nothing.
void Guided::deleted(CObject* Obj)
{
    (void)Obj;
}

// ---------------------------------------------------------------------------
// (module storage, RET 8) -- PhySys::Controller slot 4 on +0x60.
// ---------------------------------------------------------------------------
void Guided::do_simulation_controller(IVP_Event_Sim* EventSim, IVP_U_Vector<IVP_Core>* CoreList)
{
    (void)CoreList;

    // only while the motor burns -- MotorData (CGuided+0xfc)
    // set, SeekerElapsed (+0x100) not below Delay (+8), and Delay + Lifetime (+0x10) not
    // below SeekerElapsed (both TEST AH,1: an unordered compare also stops it).
    CGuided* const Cg = cguided();
    Archetype::MotorData const* const Motor = Cg->MotorArch;
    if (Motor == nullptr)
        return;
    if (!(Cg->SeekerElapsed >= Motor->Delay))
        return;
    double const BurnEnd = static_cast<double>(Motor->Lifetime) + static_cast<double>(Motor->Delay);
    if (!(BurnEnd >= static_cast<double>(Cg->SeekerElapsed)))
        return;

    // the motor again (re-narrowed), and the third column of
    // get_transform (vt+0xc).
    Archetype::MotorData const* const Motor2 = cguided()->MotorArch;
    Transform const& Xform = get_transform();
    Vector Axis;
    Axis.x = Xform.Rotation.E[0][2];
    Axis.y = Xform.Rotation.E[1][2];
    Axis.z = Xform.Rotation.E[2][2];

    // PhySys::Controller::push(Axis, -((mass * Accel) * dt))
    // with get_mass (vt+0x30), MotorData+0xc and IVP_Event_Sim+0; stored as a float.
    float const Mass = get_mass();
    float const Amount = static_cast<float>(
        -((static_cast<double>(Mass) * static_cast<double>(Motor2->Accel)) *
          static_cast<double>(EventSim->delta_time)));
    push(Axis, Amount);
}

// ---------------------------------------------------------------------------
// (module storage, RET 0xc) -- IObjAffect +0x48 on the +4 sub-object.
// ---------------------------------------------------------------------------
int Guided::set_target(IObjRW* Target, unsigned short SubTarget, int SourceClient)
{
    // a source of -1 is taken as 0.
    if (SourceClient == -1)
        SourceClient = 0;

    // a munition that does not seek (Munition+0x90 == 0 on the
    // CGuided's archetype, narrowing not null-tested) refuses: -3.
    if (cguided()->munitionarch()->Seeker == 0)
        return -3;

    // in single player a change from a client is only relayed.
    bool bChanged = false;
    if (!SinglePlayer() || SourceClient == 0)
    {
        // a different target or sub-target is taken (CGuided's
        // own set_target / set_sub_target, both gated on the seeker, inlined in baseline).
        CGuided* const Cg = cguided();
        if (Cg->get_target() != Target || Cg->SubTarget != SubTarget)
        {
            bChanged = true;
            Cg->set_target(Target);
            Cg->set_sub_target(SubTarget);
        }

        // nothing to tell when it came from nobody and changed
        // nothing.
        if (SourceClient == 0 && !bChanged)
            return 0;
    }

    // StarSystem the routine -- IClient XSetTarget for this
    // object (get_id, vt+0x20), sub-object SUBOBJ_ID_ROOT, the new target pair.
    unsigned int const ObjId = get_id();
    OwnerSystem->notify_set_target(ObjId, SUBOBJ_ID_ROOT, Target, SubTarget, SourceClient);

    // .
    return 0;
}

// ===========================================================================
// Bullet
// ===========================================================================

// `MOV EAX,0x2000000 / RET`.
unsigned int Bullet::get_type_bits() const
{
    return 0x2000000u;
}

CBeam* Bullet::cbeam() const
{
    CObject* const Obj = Object;
    if (Obj == nullptr || (static_cast<unsigned>(Obj->ObjectClass) & 0x67u) != 0x67u)
        return nullptr;
    return static_cast<CBeam*>(Obj);
}

// ---------------------------------------------------------------------------
// (module storage, RET 4) -- vtable +0x164.
// ---------------------------------------------------------------------------
bool Bullet::create(Archetype::Root* Arch)
{
    // CObject::Alloc(0x67) into CBase::Object.
    Object = CObject::Alloc(CObject::ClassBeam);

    // Archetype::Root::create_instance(Object) -- before open.
    Arch->create_instance(static_cast<IEngineInstance*>(Object));

    // NeuronRuntime::Engine vt+0x6c, UpdateInstance(Object+4, 0, 0).
    NeuronRuntime::Engine->UpdateInstance(reinterpret_cast<void*>(Object->EngineInstanceId), 0, nullptr);

    // open(Arch) (vt+0x4c) on the (ObjectClass & 0x67) == 0x67
    // narrowing, not null-tested.
    CObject* Narrowed = Object;
    if (Narrowed == nullptr || (static_cast<unsigned>(Narrowed->ObjectClass) & 0x67u) != 0x67u)
        Narrowed = nullptr;
    Narrowed->open(Arch);

    // CBase::advise(true).
    advise(true);

    // get_index() (vt+0x1c) != -1.
    return get_index() != -1;
}

// `XOR AL,AL / RET 4` -- the Bullet vtable's +0x168.
bool Bullet::find(unsigned int const& Id)
{
    (void)Id;
    return false;
}

// ---------------------------------------------------------------------------
// (module storage, RET 4) -- vtable +0x16c.
// ---------------------------------------------------------------------------
bool Bullet::update(float Dt)
{
    // GameObject's base update; false is returned as is.
    if (!GameObject::update(Dt))
        return false;

    // the CBeam's own update (vt+0x50) with UPDATE_DEFAULT; the
    // narrowing is not null-tested.
    CBeam* const Cb = cbeam();
    Cb->update(Dt, CObject::UPDATE_DEFAULT);

    // FCOMP 0.0f, TEST AH,1 -- a spent lifetime (CProjectile+0xe8
    // below zero, or unordered) removes it quietly: destroy(0, 0).
    if (!(Cb->Lifetime >= 0.0f))
        destroy(0, 0);

    // true unless destroyed.
    return !is_destroyed();
}

// `RET 8` -- Bullet's +0x17c: nothing.
void Bullet::damage_sub_obj(FSubObjDamage const& Hit, DamageList& Damage)
{
    (void)Hit;
    (void)Damage;
}

// `RET 8` -- Bullet's +0x180: nothing.
void Bullet::munition_hit(FMunitionImpact const& Hit, DamageList& Damage)
{
    (void)Hit;
    (void)Damage;
}

// `RET 8` -- Bullet's +0x184: nothing.
void Bullet::explosion_hit(ExplosionDamageEvent const& Blast, DamageList& Damage)
{
    (void)Blast;
    (void)Damage;
}

// `RET 8` -- Bullet's own +0x1bc: nothing.
void Bullet::Slot1bc(unsigned int Arg1, unsigned int Arg2)
{
    (void)Arg1;
    (void)Arg2;
}

// ---------------------------------------------------------------------------
// (module storage, RET 4) -- vtable +0x1a4.
// ---------------------------------------------------------------------------
void Bullet::on_collision(FCollisionEvent const& Hit)
{
    // the record at [ESP+8] -- munition archetype id
    // (get_archetype()+0x08 on the raw Object), owner (CBeam+0xe4 under the 0x67
    // narrowing, not null-tested), the object hit (vt+0x20), the sub-object hit (Hit+8)
    // -> StarSystem::munition_collision (the routine). baseline never writes the record's
    // +0x10..+0x1b (hit position); the port zeroes it.
    CBeam* const Cb = cbeam();
    SSPMunitionCollisionInfo Info = {};
    Info.Field_0x00 = Object->get_archetype()->Nickname.Id;
    Info.Field_0x04 = Cb->OwnerId;
    Info.ObjectId = Hit.Other->get_id();
    Info.Field_0x0c = Hit.OtherSubObjId;
    OwnerSystem->munition_collision(Info);

    // the round is spent -- destroy(1, the object hit).
    destroy(1, Hit.Other->get_id());
}
