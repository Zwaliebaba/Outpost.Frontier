// Author: Giperion.

// Server.dll Mine wrapper -- baseline module storage-06cf3c6f plus the two damage overrides
// at module storage / 06cf4240 that its vtable (module storage) shares with Guided.
// The class is declared in src/include/server_gameobject.h.

// Layout (baseline offsets): Projectile/GameObject +0x00..+0x5f, PhySys::Controller +0x60
// (vtable module storage), PhySys::CObjectCB +0x70 (vtable module storage), then the
// Mine's own fields +0x74..+0x9f. The spawner (StarSystem the routine) allocates 0xa0.

#include "../include/server_gameobject.h"
#include "../include/server_sysdb.h"   // StarSystem::explode
#include "../include/common_misc.h"    // SUBOBJ_ID_*, DamageList
#include "../include/common_id.h"      // ID_String::IsEmpty
#include "../include/NeuronRuntime.h"          // NeuronRuntime::Engine (Mine::create, IEngine +0x6c)
#include "../IVP/ivp_physics/ivp_core.h"        // IVP_Core::Speed (+0xa4)
#include "../include/common_ivp_real_object.h" // IVP_Real_Object::core (+0x98)
#include "../include/common_ivp_controller.h"  // IVP_Event_Sim::delta_time

#include <cfloat>
#include <cmath>

// ---------------------------------------------------------------------------
// (module storage, RET 0).
// ---------------------------------------------------------------------------
Mine::Mine()
    : Phantom(nullptr)
    , IntruderCount(0)
    , bSeeking(false)
    , SeekTimer(FLT_MAX)
    , bTriggered(false)
    , TriggerObjId(0)
{
    // the routine (GameObject's constructor), the Projectile vtables
    // (inlined, module storage-06cf3361), PhySys::Controller's constructor on +0x60
    // (module storage), then:
    // +0x74 = 0          module storage  +0x84 = 0
    // +0x78 = 0          module storage  +0x88 = 0
    // byte +0x80 = 0     module storage  +0x8c = 0
    // byte +0x94 = 0     module storage  +0x98 = 0
    // +0x90 = 0x7f7fffff (FLT_MAX)
    // +0x7c and +0x9c are not touched.
    Target.x = 0.0f;
    Target.y = 0.0f;
    Target.z = 0.0f;
}

// ---------------------------------------------------------------------------
// (module storage), entered on the CBase sub-object (+0xc).
// ---------------------------------------------------------------------------
Mine::~Mine()
{
    // PhySys::DestroyPhantom(+0x74) -- no null test -- then
    // +0x74 = 0. PhySys::Controller's destructor (module storage) and GameObject's
    // (the routine, module storage) follow as the base-class destructors.
    PhySys::DestroyPhantom(Phantom);
    Phantom = nullptr;
}

// (module storage): `MOV EAX,0x4000000 / RET`.
unsigned int Mine::get_type_bits() const
{
    return 0x4000000u;
}

CMine* Mine::cmine() const
{
    CObject* const Obj = Object;
    if (Obj == nullptr || (static_cast<unsigned>(Obj->ObjectClass) & 0xfu) != 0xfu)
        return nullptr;
    return static_cast<CMine*>(Obj);
}

// ---------------------------------------------------------------------------
// (module storage, RET 4) -- vtable +0x164.
// ---------------------------------------------------------------------------
bool Mine::create(Archetype::Root* Arch)
{
    // CObject::Alloc(0xf) into CBase::Object.
    Object = CObject::Alloc(CObject::ClassMine);

    // Archetype::Root::create_instance(Object) -- before open, so
    // the port's CSimple::open scaffold finds a valid instance and does nothing.
    Arch->create_instance(static_cast<IEngineInstance*>(Object));

    // NeuronRuntime::Engine vt+0x6c, UpdateInstance(Object+4, 0, 0).
    NeuronRuntime::Engine->UpdateInstance(reinterpret_cast<void*>(Object->EngineInstanceId), 0, nullptr);

    // open(Arch) (vt+0x4c) on the (ObjectClass & 3) == 3
    // narrowing, not null-tested.
    CObject* Narrowed = Object;
    if (Narrowed == nullptr || (static_cast<unsigned>(Narrowed->ObjectClass) & 3u) != 3u)
        Narrowed = nullptr;
    Narrowed->open(Arch);

    // CBase::advise(true).
    advise(true);

    // get_index() (vt+0x1c) != -1.
    return get_index() != -1;
}

// `XOR AL,AL / RET 4` -- the Mine vtable's +0x168.
bool Mine::find(unsigned int const& Id)
{
    (void)Id;
    return false;
}

// ---------------------------------------------------------------------------
// (module storage, RET 0).
// ---------------------------------------------------------------------------
void Mine::arm()
{
    // the archetype through the raw CBase::Object (no narrowing).
    // A seeking mine (SeekDist +0x88 > 0) registers its controller on the object and
    // uses SeekDist as the phantom radius; otherwise a mine with a DetonationDist (+0x84)
    // > 0 still takes SeekDist (+0x88, module storage) as the radius; neither -> return.
    Archetype::Mine const* const Arch =
        static_cast<Archetype::Mine const*>(Object->get_archetype());
    float Radius;
    if (Arch->SeekDist > 0.0f)
    {
        Radius = Arch->SeekDist;
        PhySys::Controller::Register(static_cast<PhySys::Controller*>(this), Object);
    }
    else
    {
        if (!(Arch->DetonationDist > 0.0f))
            return;
        Radius = Arch->SeekDist;
    }

    // FCOMP 0.0f, TEST AH,0x44 / JNP -- a radius of exactly zero
    // makes no phantom.
    if (Radius == 0.0f)
        return;

    // PhantomParms on the stack -- system id (the routine, the
    // system's nickname id), +0x04 = 0, +0x05 = 1, empty name, this object's position,
    // identity orientation, listener = the CObjectCB sub-object (+0x70).
    PhySys::PhantomParms Parms;
    Parms.SystemId = OwnerSystem->SystemInfo->NicknameId;
    Parms.Flag04 = 0;
    Parms.Flag05 = 1;
    Parms.Name[0] = '\0';
    Parms.Position = get_position();
    Parms.Orientation = Matrix(1.0f, 0.0f, 0.0f,
                               0.0f, 1.0f, 0.0f,
                               0.0f, 0.0f, 1.0f);
    Parms.Listener = static_cast<PhySys::CObjectCB*>(this);

    // CreatePhantom(Parms, Radius, +0x74); module storage AttachPhantom.
    PhySys::CreatePhantom(Parms, Radius, Phantom);
    PhySys::AttachPhantom(Object, Phantom);
}

// ---------------------------------------------------------------------------
// (module storage, RET 4) -- vtable +0x16c.
// ---------------------------------------------------------------------------
bool Mine::update(float Dt)
{
    // GameObject's base update; false is returned as is.
    if (!GameObject::update(Dt))
        return false;

    // the CMine's own update (vt+0x50) with UPDATE_DEFAULT; the
    // narrowing is not null-tested.
    CMine* const Cm = cmine();
    Cm->update(Dt, CObject::UPDATE_DEFAULT);

    // FCOMP 0.0f, TEST AH,1 -- a spent lifetime (CProjectile+0xe8
    // below zero; C0 is also set for an unordered compare) destroys the mine quietly:
    // destroy(0, 0).
    if (!(Cm->Lifetime >= 0.0f))
    {
        destroy(0, 0);
    }
    else
    {
        // switch the phantom's collisions on if they are off.
        if (!PhySys::GetCollisionStatePhantom(Phantom))
            PhySys::SetCollisionStatePhantom(Phantom, true);

        if (IntruderCount != 0)
        {
            // the owner is exempt only while the owner-safe time
            // runs.
            unsigned int Exempt = 0;
            if (Cm->is_owner_safe())
                Exempt = Cm->OwnerId;

            // the closest intruder of mask 0x7ffdf8 (not Exempt);
            // its position lands in +0x84.
            CSimple* Closest = nullptr;
            PhySys::GetClosestPhantomIntruderPos(Phantom, 0x7ffdf8u, Exempt, Closest, Target);
            if (Closest != nullptr)
            {
                // DetonationDist squared, stored as a float.
                Archetype::Mine const* const Arch =
                    static_cast<Archetype::Mine const*>(Object->get_archetype());
                float const DetonationSq = static_cast<float>(
                    static_cast<double>(Arch->DetonationDist) *
                    static_cast<double>(Arch->DetonationDist));

                // (dz*dz + dx*dx) + dy*dy on the FPU against it;
                // TEST AH,5 / JP -- strictly inside detonates.
                Vector const& Pos = get_position();
                double const Dx = static_cast<double>(Target.x) - static_cast<double>(Pos.x);
                double const Dy = static_cast<double>(Target.y) - static_cast<double>(Pos.y);
                double const Dz = static_cast<double>(Target.z) - static_cast<double>(Pos.z);
                double const DistSq = (Dz * Dz + Dx * Dx) + Dy * Dy;
                if (DistSq < static_cast<double>(DetonationSq))
                {
                    // destroy(1, the intruder's id, CSimple+0xb0).
                    destroy(1, Closest->Id);
                }
                else
                {
                    // wake the controller up and seek; the timer
                    // sum stays on the FPU (FST) for the compare against 1.0f.
                    wakeup();
                    bSeeking = true;
                    double const NewTimer =
                        static_cast<double>(Dt) + static_cast<double>(SeekTimer);
                    SeekTimer = static_cast<float>(NewTimer);
                    if (NewTimer > 1.0)
                        SeekTimer = 0.0f;
                }
            }
            else
            {
                // nobody close -- stop seeking.
                bSeeking = false;
                Target.z = 0.0f;
                Target.y = 0.0f;
                Target.x = 0.0f;
            }
        }
        else
        {
            // empty phantom -- stop seeking; a touch latched by
            // added() detonates now: destroy(1, TriggerObjId).
            bSeeking = false;
            Target.z = 0.0f;
            Target.y = 0.0f;
            Target.x = 0.0f;
            if (bTriggered)
                destroy(1, TriggerObjId);
        }
    }

    // true unless destroyed.
    return !is_destroyed();
}

// ---------------------------------------------------------------------------
// (module storage, RET 8) -- vtable +0x158.
// ---------------------------------------------------------------------------
bool Mine::destroy(int Kind, unsigned int ObjId)
{
    // GameObject's destroy (the routine); its result is not used.
    GameObject::destroy(Kind, ObjId);

    // only a real kill blows up, and only with an explosion
    // archetype that has a radius and some effect (the double 0.0001 at module storage).
    if (Kind == 1)
    {
        Archetype::Root* const Arch = Object->get_archetype();
        if (!Arch->ExplosionArch.IsEmpty())
        {
            Archetype::Explosion* const Explosion = Archetype::GetExplosion(Arch->ExplosionArch);
            double const Epsilon = 0.0001;
            if (Explosion != nullptr &&
                static_cast<double>(Explosion->Radius) > Epsilon &&
                (static_cast<double>(Explosion->Impulse) > Epsilon ||
                 static_cast<double>(Explosion->HullDamage) > Epsilon ||
                 static_cast<double>(Explosion->EnergyDamage) > Epsilon))
            {
                // {own id, the CMine's owner (+0xe4, 0xf narrowing,
                // not null-tested), cause 7 (after a first store of 4), position, explosion,
                // no skipped collision group} -> StarSystem::explode (the routine).
                ExplosionDamageEvent Blast;
                Blast.ExplodingObjId = get_id();
                Blast.InflictorId = cmine()->OwnerId;
                Blast.Cause = static_cast<DamageCause>(7);
                Blast.Position = get_position();
                Blast.Explosion = Explosion;
                Blast.SkipCollisionGroup = nullptr;
                OwnerSystem->explode(Blast);
            }
        }
    }

    // always true.
    return true;
}

// ---------------------------------------------------------------------------
// (module storage, RET 4) -- CObjectCB slot 0 on the +0x70 sub-object.
// ---------------------------------------------------------------------------
void Mine::added(CObject* Obj)
{
    // SeekDist (+0x88) > 0.0f picks the seeking arm.
    Archetype::Mine const* const Arch =
        static_cast<Archetype::Mine const*>(Object->get_archetype());
    if (Arch->SeekDist > 0.0f)
    {
        // a CSimple root (GetRoot, (ObjectClass & 3) == 3) counts
        // as an intruder.
        CObject* const Root = GetRoot(Obj);
        if (Root != nullptr && (static_cast<unsigned>(Root->ObjectClass) & 3u) == 3u)
            IntruderCount = IntruderCount + 1;
        return;
    }

    // a non-seeking mine latches the first CSimple root that
    // touches it ...
    CObject* const Root = GetRoot(Obj);
    if (Root == nullptr || (static_cast<unsigned>(Root->ObjectClass) & 3u) != 3u)
        return;
    CSimple* const Toucher = static_cast<CSimple*>(Root);

    // ... unless it is the owner (CMine+0xe4, narrowing not
    // null-tested) while the owner-safe time still runs.
    CMine* const Cm = cmine();
    if (Toucher->Id == Cm->OwnerId && Cm->is_owner_safe())
        return;

    // .
    bTriggered = true;
    TriggerObjId = Toucher->Id;
}

// ---------------------------------------------------------------------------
// (module storage, RET 4) -- CObjectCB slot 1 on the +0x70 sub-object.
// ---------------------------------------------------------------------------
void Mine::deleted(CObject* Obj)
{
    // only a seeking mine counts; a CSimple root leaving takes
    // one off.
    Archetype::Mine const* const Arch =
        static_cast<Archetype::Mine const*>(Object->get_archetype());
    if (!(Arch->SeekDist > 0.0f))
        return;
    CObject* const Root = GetRoot(Obj);
    if (Root != nullptr && (static_cast<unsigned>(Root->ObjectClass) & 3u) == 3u)
        IntruderCount = IntruderCount - 1;
}

// ---------------------------------------------------------------------------
// (module storage, RET 8) -- PhySys::Controller slot 4 on +0x60.
// ---------------------------------------------------------------------------
void Mine::do_simulation_controller(IVP_Event_Sim* EventSim, IVP_U_Vector<IVP_Core>* CoreList)
{
    (void)CoreList;

    // nothing unless update found a target.
    if (!bSeeking)
        return;

    // velocity (vt+4), position (vt[0]); the direction to the
    // target, each component stored as a float (z first).
    Vector const Velocity = get_velocity();
    Vector const& Pos = get_position();
    Vector Dir;
    Dir.z = static_cast<float>(static_cast<double>(Target.z) - static_cast<double>(Pos.z));
    Dir.y = static_cast<float>(static_cast<double>(Target.y) - static_cast<double>(Pos.y));
    Dir.x = static_cast<float>(static_cast<double>(Target.x) - static_cast<double>(Pos.x));

    // normalised by 1.0f / FSQRT((z*z + y*y) + x*x) -- no zero test.
    double const LengthSq = (static_cast<double>(Dir.z) * static_cast<double>(Dir.z) +
                             static_cast<double>(Dir.y) * static_cast<double>(Dir.y)) +
                            static_cast<double>(Dir.x) * static_cast<double>(Dir.x);
    double const Inverse = 1.0 / std::sqrt(LengthSq);
    Dir.x = static_cast<float>(static_cast<double>(Dir.x) * Inverse);
    Dir.y = static_cast<float>(static_cast<double>(Dir.y) * Inverse);
    Dir.z = static_cast<float>(static_cast<double>(Dir.z) * Inverse);

    // the speed already along it, (z*vz + vy*y) + vx*x, as a float.
    float const Speed = static_cast<float>(
        (static_cast<double>(Dir.z) * static_cast<double>(Velocity.z) +
         static_cast<double>(Velocity.y) * static_cast<double>(Dir.y)) +
        static_cast<double>(Velocity.x) * static_cast<double>(Dir.x));

    // what is missing to TopSpeed (+0x90); stored as a float (FST)
    // but the unrounded difference is tested -- nothing to do unless positive.
    Archetype::Mine const* const Arch =
        static_cast<Archetype::Mine const*>(Object->get_archetype());
    double const Missing = static_cast<double>(Arch->TopSpeed) - static_cast<double>(Speed);
    float Delta = static_cast<float>(Missing);
    if (!(Missing > 0.0))
        return;

    // capped at TopSpeed itself.
    if (!(Arch->TopSpeed > Delta))
        Delta = Arch->TopSpeed;

    // and, unless Acceleration (+0x8c) is exactly -1.0f
    // (module storage; TEST AH,0x44 / JNP), at Acceleration * dt.
    float const Dt = EventSim->delta_time;
    if (!(Arch->Acceleration == -1.0f))
    {
        double const Cap = static_cast<double>(Arch->Acceleration) * static_cast<double>(Dt);
        if (static_cast<double>(Delta) > Cap)
            Delta = static_cast<float>(Cap);
    }

    // the push, drag-compensated:
    //   K = (Delta + Speed) * ((LinearDrag +0x80 / Mass +0x20) * dt) + Delta
    double const DragDt = (static_cast<double>(Arch->LinearDrag) / static_cast<double>(Arch->Mass)) *
                          static_cast<double>(Dt);
    double const K = (static_cast<double>(Delta) + static_cast<double>(Speed)) * DragDt +
                     static_cast<double>(Delta);

    // straight into the physics core's linear velocity
    // (CObject+0x54 body, +0x98 core, +0xa4 speed), no null tests.
    IVP_Real_Object* const Body = static_cast<IVP_Real_Object*>(Object->BodyPtr);
    Vector& CoreSpeed = Body->core->Speed;
    CoreSpeed.x = static_cast<float>(static_cast<double>(Dir.x) * K + static_cast<double>(CoreSpeed.x));
    CoreSpeed.y = static_cast<float>(static_cast<double>(Dir.y) * K + static_cast<double>(CoreSpeed.y));
    CoreSpeed.z = static_cast<float>(static_cast<double>(Dir.z) * K + static_cast<double>(CoreSpeed.z));
}

// ---------------------------------------------------------------------------
// (module storage, RET 8) -- vtable +0x17c (Mine and Guided).
// Any sub-object hit destroys the projectile: byte +0xc of the DamageList = 1.
// ---------------------------------------------------------------------------
void Mine::damage_sub_obj(FSubObjDamage const& Hit, DamageList& Damage)
{
    (void)Hit;
    Damage.bDestroyed = true;
}

// ---------------------------------------------------------------------------
// (module storage, RET 8) -- vtable +0x180 (Mine and Guided).
// ---------------------------------------------------------------------------
void Mine::munition_hit(FMunitionImpact const& Hit, DamageList& Damage)
{
    // a seeker munition (Munition+0x90 != 0) always kills.
    if (Hit.Munition->Seeker != 0)
    {
        Damage.bDestroyed = true;
        return;
    }
    // otherwise GameObject's body (JMP the routine).
    GameObject::munition_hit(Hit, Damage);
}
