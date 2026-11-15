// Author: Giperion.

// Server.dll CounterMeasure wrapper -- baseline module storage-06ce64fb, plus the shared
// damage override module storage its vtable (module storage) puts in two slots.
// The class is declared in src/include/server_gameobject.h.

// Layout (baseline offsets): Projectile/GameObject +0x00..+0x5f and nothing else; the
// spawner (StarSystem the routine) allocates 0x60.

#include "../include/server_gameobject.h"
#include "../include/server_sysdb.h"   // StarSystem::explode / find_object
#include "../include/common_misc.h"    // SUBOBJ_ID_NONE, DamageList
#include "../include/common_id.h"      // ID_String::IsEmpty
#include "../include/common_physys.h"  // PhySys::FindSphereCollisions
#include "../include/NeuronRuntime.h"          // NeuronRuntime::Engine (CounterMeasure::create, IEngine +0x6c)

#include <cstdlib>   // rand

// .data:the field -- the file-static hit buffer divert_missiles hands to
// PhySys::FindSphereCollisions (module storage pushes its address with a
// maximum of 0x1e) and then reads by index (module storage).
static CObject* GCounterMeasureHits[0x1e];

// ---------------------------------------------------------------------------
// (module storage, RET 0).
// ---------------------------------------------------------------------------
CounterMeasure::CounterMeasure()
{
    // the routine (GameObject's constructor), then the three
    // CounterMeasure vtables (module storage / 06ce617e / 06ce6185). No field
    // of its own is written.
}

// (module storage): `MOV EAX,0x8000000 / RET`.
unsigned int CounterMeasure::get_type_bits() const
{
    return 0x8000000u;
}

CCounterMeasure* CounterMeasure::ccountermeasure() const
{
    CObject* const Obj = Object;
    if (Obj == nullptr || (static_cast<unsigned>(Obj->ObjectClass) & 0x17u) != 0x17u)
        return nullptr;
    return static_cast<CCounterMeasure*>(Obj);
}

// ---------------------------------------------------------------------------
// (module storage, RET 4) -- vtable +0x164.
// ---------------------------------------------------------------------------
bool CounterMeasure::create(Archetype::Root* Arch)
{
    // CObject::Alloc(0x17) into CBase::Object.
    Object = CObject::Alloc(CObject::ClassCounterMeasure);

    // Archetype::Root::create_instance(Object) (import
    // ).
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

// `XOR AL,AL / RET 4` -- the CounterMeasure vtable's +0x168.
bool CounterMeasure::find(unsigned int const& Id)
{
    (void)Id;
    return false;
}

// ---------------------------------------------------------------------------
// (module storage, RET 4) -- vtable +0x16c.
// ---------------------------------------------------------------------------
bool CounterMeasure::update(float Dt)
{
    // GameObject's base update (the routine); false is
    // returned as is.
    if (!GameObject::update(Dt))
        return false;

    // the CCounterMeasure's own update (vt+0x50) with
    // UPDATE_DEFAULT; the narrowing is not null-tested.
    CCounterMeasure* const Cm = ccountermeasure();
    Cm->update(Dt, CObject::UPDATE_DEFAULT);

    // FCOMP 0.0f (module storage), TEST AH,1 -- a spent
    // lifetime (CProjectile+0xe8 below zero, or unordered) destroys it quietly:
    // destroy(0, 0) through vtable +0x158.
    if (!(Cm->Lifetime >= 0.0f))
        destroy(0, 0);

    // `NEG AL / SBB EAX,EAX / INC EAX` -- true unless destroyed.
    return !is_destroyed();
}

// ---------------------------------------------------------------------------
// (module storage, RET 0).
// ---------------------------------------------------------------------------
void CounterMeasure::divert_missiles()
{
    // FindSphereCollisions(the system id (the routine),
    // get_position() (vt[0]), the archetype's Range (+0x84, read through the raw
    // CBase::Object), the file-static buffer, 0x1e).
    Archetype::CounterMeasure const* const Arch =
        static_cast<Archetype::CounterMeasure const*>(Object->get_archetype());
    int const Count = PhySys::FindSphereCollisions(OwnerSystem->SystemInfo->NicknameId,
                                                   get_position(), Arch->Range,
                                                   GCounterMeasureHits, 0x1e);

    // for i in [0, Count) -- JLE skips a non-positive count.
    for (int Index = 0; Index < Count; ++Index)
    {
        // a non-null hit whose class passes (& 0xa7) == 0xa7
        // (a CGuided).
        CObject* const Hit = GCounterMeasureHits[Index];
        if (Hit == nullptr || (static_cast<unsigned>(Hit->ObjectClass) & 0xa7u) != 0xa7u)
            continue;

        // not a missile of this countermeasure's own owner
        // (CProjectile+0xe4 on both; this side's narrowing is not null-tested).
        CCounterMeasure* const Cm = ccountermeasure();
        if (static_cast<CProjectile*>(Hit)->OwnerId == Cm->OwnerId)
            continue;

        // the missile's server object, by its CSimple id
        // (+0xb0), through the system (the routine).
        GameObject* const Missile = OwnerSystem->find_object(static_cast<CSimple*>(Hit)->Id);
        if (Missile == nullptr)
            continue;

        // `rand() * 100` divided by 0x8000 with the
        // signed-division rounding fix-up (CDQ / AND EDX,0x7fff / ADD / SAR 0xf).
        int const Roll = (std::rand() * 100) / 0x8000;

        // JG -- a roll above DiversionPctg (+0x88, the
        // archetype fetched again through the raw CBase::Object) keeps the target.
        Archetype::CounterMeasure const* const RollArch =
            static_cast<Archetype::CounterMeasure const*>(Object->get_archetype());
        if (Roll > RollArch->DiversionPctg)
            continue;

        // the missile's IObjAffect (+4) vt+0x48,
        // set_target(this, SUBOBJ_ID_NONE, 0).
        static_cast<IObjAffect*>(Missile)->set_target(static_cast<IObjRW*>(this),
                                                      SUBOBJ_ID_NONE, 0);
    }
}

// ---------------------------------------------------------------------------
// (module storage, RET 8) -- vtable +0x158.
// ---------------------------------------------------------------------------
bool CounterMeasure::destroy(int Kind, unsigned int ObjId)
{
    // GameObject's destroy (the routine); its result is not used.
    GameObject::destroy(Kind, ObjId);

    // only a real kill blows up, and only with an explosion
    // archetype that has a radius and some effect (the double 0.0001 at module storage;
    // each test is FCOMP / TEST AH,0x41, i.e. strictly greater).
    if (Kind == 1)
    {
        // the archetype through the raw CBase::Object.
        if (!Object->get_archetype()->ExplosionArch.IsEmpty())
        {
            // fetched again for Archetype::GetExplosion.
            Archetype::Explosion* const Explosion =
                Archetype::GetExplosion(Object->get_archetype()->ExplosionArch);
            double const Epsilon = 0.0001;
            if (Explosion != nullptr &&
                static_cast<double>(Explosion->Radius) > Epsilon &&
                (static_cast<double>(Explosion->Impulse) > Epsilon ||
                 static_cast<double>(Explosion->HullDamage) > Epsilon ||
                 static_cast<double>(Explosion->EnergyDamage) > Epsilon))
            {
                // {own id (vt+0x20), the raw object's
                // CProjectile owner (+0xe4, no narrowing), cause 4 (stored twice),
                // position (vt[0]), explosion, no skipped collision group} ->
                // StarSystem::explode (the routine).
                ExplosionDamageEvent Blast;
                Blast.ExplodingObjId = get_id();
                Blast.InflictorId = static_cast<CProjectile*>(Object)->OwnerId;
                Blast.Cause = static_cast<DamageCause>(4);
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
// (module storage, RET 8) -- vtable +0x17c AND +0x180 here.
// `MOV EAX,[ESP+8] / MOV byte [EAX+0xc],1`: any hit destroys it.
// ---------------------------------------------------------------------------
void CounterMeasure::damage_sub_obj(FSubObjDamage const& Hit, DamageList& Damage)
{
    (void)Hit;
    Damage.bDestroyed = true;
}

void CounterMeasure::munition_hit(FMunitionImpact const& Hit, DamageList& Damage)
{
    (void)Hit;
    Damage.bDestroyed = true;
}
