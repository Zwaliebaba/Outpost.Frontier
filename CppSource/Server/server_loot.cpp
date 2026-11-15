// Author: Giperion.

// Server.dll Loot wrapper -- baseline Loot.cpp ("Server\Space\Loot.cpp",
// ), module storage-06cf2c03. The class is declared in
// src/include/server_gameobject.h; its constructor, find, get_type_bits and cloot live in
// server_gameobject.cpp.

#include "../include/server_gameobject.h"
#include "../include/server_sysdb.h"   // StarSystem::explode / find_object, StarSystem_DamageObject
#include "../include/common_misc.h"    // LOOT_UNSEEN_*, SUBOBJ_ID_ROOT / SUBOBJ_ID_POWER
#include "../include/common_id.h"      // ID_String::IsEmpty / get_string
#include "../include/neuroncomponents.h"          // FDUMP
#include "../include/NeuronRuntime.h"          // NeuronRuntime::Engine (Loot::create, IEngine +0x6c)

#include <cstdlib>
#include <list>
#include <vector>

// Server_ControllerDispatch (common_server_controller.h, through server_sysdb.h ->
// common_server.h): pub::Controller::_SendMessage's dispatch core; baseline the routine is
// `_SendMessage(0 /* broadcast */, MsgId, MsgData)`.

namespace
{
// the __FILE__ every Loot.cpp FDUMP passes.
char const kLootCppPath[] = "Server\\Space\\Loot.cpp";
} // namespace

// ---------------------------------------------------------------------------
// (module storage, RET 4) -- vtable +0x164.
// ---------------------------------------------------------------------------
bool Loot::create(Archetype::Root* Arch)
{
    // CObject::Alloc(0x803); CBase::Object keeps the result as is.
    Object = CObject::Alloc(CObject::ClassLoot);

    // the (ObjectClass & 0x803) == 0x803 narrowing is applied only to
    // the pointer open() (vt+0x4c) is called on; no null test before the call.
    CObject* Narrowed = Object;
    if (Narrowed == nullptr ||
        (static_cast<unsigned>(Narrowed->ObjectClass) & 0x803u) != 0x803u)
    {
        Narrowed = nullptr;
    }
    Narrowed->open(Arch);

    // CBase::advise(true).
    advise(true);

    // Archetype::Root::create_instance(the raw CBase::Object) on the
    // same Arch (EDI); the return value is dropped -- the engine stores the id at CObject+4
    // through IEngineInstance::initialize_instance. Unlike Ship/Solar, Loot makes the
    // instance after open and advise.
    Arch->create_instance(static_cast<IEngineInstance*>(Object));

    // NeuronRuntime::Engine vt+0x6c, IEngine::UpdateInstance(the object's
    // engine instance, CObject+4, 0, 0); no null test on the engine.
    NeuronRuntime::Engine->UpdateInstance(reinterpret_cast<void*>(Object->EngineInstanceId), 0, nullptr);

    // IObjInspect vt+0x1c get_index, SETNZ against -1.
    return get_index() != -1;
}

// ---------------------------------------------------------------------------
// (module storage, RET 4) -- vtable +0x16c.
// ---------------------------------------------------------------------------
bool Loot::update(float Dt)
{
    // CALL the routine (GameObject's base update). A false answer is returned
    // as is (module storage -- AL still holds it).
    if (!GameObject::update(Dt))
        return false;

    // the CObject's own update, as a dumb (client-driven) object when
    // it belongs to a player. The owner is read through the CSimple narrowing (no null test on
    // the result, `XOR EAX,EAX` then `[EAX+0xb4]`); the call goes through the raw CBase::Object.
    unsigned int const Mode = (csimple()->OwnerPlayer != 0) ? CObject::UPDATE_DUMB
                                                            : CObject::UPDATE_DEFAULT;
    Object->update(Dt, Mode);

    update_decay(Dt); // , result ignored

    // destroyed meanwhile -> false.
    if (is_destroyed())
        return false;

    // CALL [EAX+0x19c] (is_invincible): invincible loot never times out.
    if (!is_invincible())
    {
        // both timers move; the scan timer's sum stays on the FPU
        // stack (FST, not FSTP) and it is that unrounded value the compare below reads.
        double const NewCheckTimer = static_cast<double>(Dt) + static_cast<double>(CheckTimer);
        CheckTimer = static_cast<float>(NewCheckTimer);
        UnseenLife = UnseenLife - Dt;
        // FCOMP [module storage] (3.0f), TEST AH,0x41 / JNZ --
        // scan only once more than three seconds have passed.
        if (NewCheckTimer > 3.0)
        {
            // RadiusSq = LOOT_UNSEEN_RADIUS squared, stored as a float;
            // the IObjDB cursor {0, 0, 0x7f0000} walks the system's ships.
            float const RadiusSq = LOOT_UNSEEN_RADIUS * LOOT_UNSEEN_RADIUS;
            unsigned int Cursor[3];
            Cursor[0] = 0;
            Cursor[1] = 0;
            Cursor[2] = 0x7f0000u;
            IObjRW* Other = OwnerSystem->GetNext(Cursor);
            while (Other != nullptr)
            {
                // CALL [EDX+0xbc] (IObjInspect::is_player).
                if (Other->is_player())
                {
                    // d = other - this, (dx*dx + dz*dz) + dy*dy in x87
                    // (double) precision, FCOMP against the float RadiusSq; TEST AH,5 / JP --
                    // strictly inside the radius resets the unseen life.
                    Vector const& Mine = get_position();
                    Vector const& Theirs = Other->get_position();
                    double const Dx = static_cast<double>(Theirs.x) - static_cast<double>(Mine.x);
                    double const Dy = static_cast<double>(Theirs.y) - static_cast<double>(Mine.y);
                    double const Dz = static_cast<double>(Theirs.z) - static_cast<double>(Mine.z);
                    double const DistSq = (Dx * Dx + Dz * Dz) + Dy * Dy;
                    if (DistSq < static_cast<double>(RadiusSq))
                        UnseenLife = LOOT_UNSEEN_LIFE_TIME;
                }
                Other = OwnerSystem->GetNext(Cursor);
            }

            // FCOMP 0.0f, TEST AH,5 / JP -- below zero destroys the
            // loot (destroy(0, own id), module storage) and leaves the scan timer running;
            // otherwise the timer restarts (module storage).
            if (UnseenLife < 0.0f)
                destroy(0, get_id());
            else
                CheckTimer = 0.0f;
        }
    }

    // NEG / SBB / INC -- true unless destroyed.
    return !is_destroyed();
}

// ---------------------------------------------------------------------------
// (module storage, RET 8) -- vtable +0x158.
// ---------------------------------------------------------------------------
bool Loot::destroy(int Kind, unsigned int ObjId)
{
    // controller message 0x39 with an empty drop list; the id is read
    // first (module storage), then the system id (module storage the routine -- the
    // Universe system's own id), then the position (module storage vt[0]).
    FObjectDestroyedEvent Event;
    Event.ObjId = get_id();
    Event.KillerId = ObjId;
    Event.SystemId = OwnerSystem->SystemInfo->NicknameId;
    Event.OwnerPlayer = 0;
    Event.Kind = Kind;
    Event.Position = get_position();
    Server_ControllerDispatch(0, pub::Controller::EMessage::ObjectDestroyed, &Event);

    // CALL the routine -- GameObject's destroy; its result is not used.
    GameObject::destroy(Kind, ObjId);

    // a real kill also blows the crate up and spills its cargo.
    if (Kind == 1)
    {
        explode_on_destroy();
        split_stack();        // (module storage)
    }

    // the vector is freed; always true.
    return true;
}

// ---------------------------------------------------------------------------
// (module storage, RET 4) -- vtable +0x188.
// ---------------------------------------------------------------------------
void Loot::apply_damage_list(DamageList& Damage)
{
    // re-entry is reported, not refused. baseline evaluates the cause
    // string first, then the archetype nickname (CObject::get_archetype on the CLoot
    // narrowing, Root+0x08 ID_String::get_string), then the id; level 0x100001, line 0xa1.
    if (is_in_damage_callback())
    {
        char const* const CauseName = DamageList::DmgCauseToString(Damage.Cause);
        char const* const ArchName = cloot()->get_archetype()->Nickname.get_string();
        unsigned int const Id = get_id();
        FDUMP(0x100001,
              "%s(%d) : *** ERROR: ID(0x%X) Arch(%s): Tried to apply damage of type %s within "
              "another damage callback.",
              kLootCppPath, 0xa1, Id, ArchName, CauseName);
    }

    set_in_damage_callback(true);

    // an empty list skips the entries AND the system broadcast.
    if (Damage.Count != 0)
    {
        for (std::list<DamageEntry>::iterator It = Damage.Entries.begin();
             It != Damage.Entries.end(); ++It)
        {
            DamageEntry const& Entry = *It;
            if (Entry.SubObjId == SUBOBJ_ID_ROOT)
            {
                // CALL [EDX+0x1b4] (apply_root_damage).
                apply_root_damage(Entry);
            }
            else if (Entry.HitPts > 0.0f)
            {
                // FCOMP 0.0f, TEST AH,0x41 / JNZ -- a positive value
                // becomes the contents' hit points (CLoot+0xf4, no null test).
                cloot()->ContentsHitPts = Entry.HitPts;
            }
            else
            {
                // one unit is gone (CLoot+0xf0 - 1) and the next one
                // starts at the contents archetype's full hit points (ContentsArch+0x1c).
                cloot()->Units = cloot()->Units - 1;
                cloot()->ContentsHitPts = cloot()->ContentsArch->HitPts;
            }
        }
        // the routine (OwnerSystem, this, &Damage).
        StarSystem_DamageObject(OwnerSystem, this, &Damage);
    }

    // a destroying list kills the loot unless the invincibility gate
    // (+0x1b8) blocks it or it is already dead; the inflictor goes on as destroy's ObjId.
    if (Damage.bDestroyed && !is_damage_blocked(Damage) && !is_destroyed())
        destroy(1, Damage.InflictorId);

    set_in_damage_callback(false);
}

// ---------------------------------------------------------------------------
// (module storage, RET 4).
// ---------------------------------------------------------------------------
bool Loot::update_decay(float Dt)
{
    // only commodities decay (ContentsArch vt+0x10 get_class_type ==
    // 0xa); the CLoot narrowing is not null-tested.
    Archetype::Equipment const* const Contents = cloot()->ContentsArch;
    if (Contents->get_class_type() != Archetype::AClassCommodity)
        return true;

    // Commodity::get_decay_per_second * Dt; FCOM 0.0f, TEST AH,0x41 /
    // JNZ -- nothing decays unless the amount is positive.
    // The product and the sum below stay on the FPU stack (FMUL, FADD, then FST -- not FSTP),
    // so both compares read the unrounded x87 values.
    double const Decay =
        static_cast<double>(static_cast<Archetype::Commodity const*>(cloot()->ContentsArch)
                                ->get_decay_per_second()) *
        static_cast<double>(Dt);
    if (!(Decay > 0.0))
        return true;

    // accumulate; a whole unit's worth (ContentsArch+0x1c, strictly
    // greater) takes a unit off.
    double const NewAccum = Decay + static_cast<double>(DecayAccum);
    DecayAccum = static_cast<float>(NewAccum);
    if (!(NewAccum > static_cast<double>(cloot()->ContentsArch->HitPts)))
        return true;

    // the last unit destroys the loot: destroy(1, own id), false.
    if (cloot()->Units == 1)
    {
        destroy(1, get_id());
        return false;
    }

    // a cause-0x14 DamageList (inflictor 0/0 -- the inlined
    // constructor) with one SUBOBJ_ID_POWER entry {0.0f, None}, applied through the object's own
    // +0x188, which turns a non-positive entry into "one unit fewer".
    {
        DamageList Damage;
        Damage.Cause = static_cast<DamageCause>(0x14);
        Damage.add_damage_entry(SUBOBJ_ID_POWER, 0.0f, DamageEntry::SubObjFate::None);
        apply_damage_list(Damage);
    }

    // the spent unit's hit points come off the accumulator.
    DecayAccum = DecayAccum - cloot()->ContentsArch->HitPts;
    return true;
}

// ---------------------------------------------------------------------------
// (module storage, RET 0).
// ---------------------------------------------------------------------------
void Loot::explode_on_destroy()
{
    // the loot's own archetype (CObject::get_archetype on the CLoot
    // narrowing), its explosion_arch (Root+0x24); an empty id means no blast.
    Archetype::Root* const Arch = cloot()->get_archetype();
    ID_String const& ExplosionId = Arch->ExplosionArch;
    if (ExplosionId.IsEmpty())
        return;

    // Archetype::GetExplosion; a missing entry means no blast.
    Archetype::Explosion* const Explosion = Archetype::GetExplosion(ExplosionId);
    if (Explosion == nullptr)
        return;

    // FCOMP double [module storage] (0.0001), TEST AH,0x41 each --
    // the blast needs a radius and at least one of impulse, hull damage or energy damage.
    double const Epsilon = 0.0001;
    if (!(static_cast<double>(Explosion->Radius) > Epsilon))
        return;
    if (!(static_cast<double>(Explosion->Impulse) > Epsilon) &&
        !(static_cast<double>(Explosion->HullDamage) > Epsilon) &&
        !(static_cast<double>(Explosion->EnergyDamage) > Epsilon))
    {
        return;
    }

    // {own id, inflictor 0, cause 4, position, explosion, no skipped
    // collision group} -> StarSystem::explode (the routine).
    ExplosionDamageEvent Blast;
    Blast.InflictorId = 0;
    Blast.SkipCollisionGroup = nullptr;
    Blast.ExplodingObjId = get_id();
    Blast.Cause = static_cast<DamageCause>(4);
    Blast.Position = get_position();
    Blast.Explosion = Explosion;
    OwnerSystem->explode(Blast);
}

// ---------------------------------------------------------------------------
// (module storage, RET 0).
// ---------------------------------------------------------------------------
void Loot::split_stack()
{
    // only a CargoPod crate splits (CObject::get_archetype on the CLoot
    // narrowing, Root vt+0x10 get_class_type == 9).
    if (cloot()->get_archetype()->get_class_type() != Archetype::AClassCargoPod)
        return;

    // Units (CLoot+0xf0) against the contents' UnitsPerContainer
    // (ContentsArch+0x64), unsigned (JBE) -- nothing to split unless strictly more.
    unsigned int Remaining = cloot()->Units;
    unsigned int const PerContainer =
        static_cast<unsigned int>(cloot()->ContentsArch->UnitsPerContainer);
    if (!(PerContainer < Remaining))
        return;

    // an empty std::vector of 0x6c-byte records, then the id maker.
    std::vector<FLootRecord> Records;
    CFLIDMaker* const Maker = CFLIDMaker::GetInstance();

    // / 06cf2fd6-06cf2fda: while units remain, at most five records
    // (CMP EAX,4 / JG on the pre-increment index).
    int Index = 0;
    while (Remaining != 0 && Index <= 4)
    {
        Index = Index + 1;

        // the routine inserts a template at the end -- the first eight
        // dwords zero (so no player to mark, +0x18, and no name id, +0x1c), +0x68 = 1 and
        // +0x69 = 0; every other field is written below.
        FLootRecord Template = {};
        Template.Field_0x68 = 1;
        Template.bMission = 0;
        Records.push_back(Template);
        FLootRecord& Record = Records.back();

        // the maker's dynamic counter, post-incremented, 30 bits.
        Record.Id = Maker->AllocateObjectID();
        // the contents archetype (CLoot+0xec) and the current unit's hit
        // points (CLoot+0xf4).
        Record.ContentsArch = cloot()->ContentsArch;
        Record.ContentsHitPts = cloot()->ContentsHitPts;
        // a full container, or what is left.
        Record.Count = static_cast<int>((Remaining > PerContainer) ? PerContainer : Remaining);
        // the crate is the contents' loot appearance (Equipment
        // vt+0x1c), at that archetype's hit points (Root+0x1c).
        Archetype::Equipment* const Appearance =
            const_cast<Archetype::Equipment*>(Record.ContentsArch)->get_loot_appearance();
        Record.Arch = Appearance;
        Record.HitPts = Appearance->HitPts;
        // this crate's position (vt[0]) and rotation (vt+0xc
        // get_transform, its first nine dwords, REP MOVSD ECX=9).
        Record.Position = get_position();
        Record.Orientation = get_transform().Rotation;
        // and its velocity (vt+4, by value).
        Record.Velocity = get_velocity();

        // a random push. rand() * 2^-15 (module storage) is kept as a
        // double, doubled (FADD ST0,ST0), times random_on_unit_sphere -- each component stored as
        // a float -- then times 10.0f (module storage) and added to the velocity.
        int const SpeedRoll = rand();
        double const Speed = static_cast<double>(SpeedRoll) * (1.0 / 32768.0);
        Vector const Direction = random_on_unit_sphere();
        double const Speed2 = Speed + Speed;
        Vector Push;
        Push.x = static_cast<float>(Speed2 * static_cast<double>(Direction.x));
        Push.y = static_cast<float>(Speed2 * static_cast<double>(Direction.y));
        Push.z = static_cast<float>(Speed2 * static_cast<double>(Direction.z));
        Vector Kick;
        Kick.x = static_cast<float>(static_cast<double>(Push.x) * 10.0);
        Kick.y = static_cast<float>(static_cast<double>(Push.y) * 10.0);
        Kick.z = static_cast<float>(static_cast<double>(Push.z) * 10.0);
        Record.Velocity.x = static_cast<float>(static_cast<double>(Kick.x) +
                                               static_cast<double>(Record.Velocity.x));
        Record.Velocity.y = static_cast<float>(static_cast<double>(Kick.y) +
                                               static_cast<double>(Record.Velocity.y));
        Record.Velocity.z = static_cast<float>(static_cast<double>(Kick.z) +
                                               static_cast<double>(Record.Velocity.z));

        // spin about the crate's first column (E[0][0], E[1][0],
        // E[2][0]) scaled by rand() * 2^-15 * 0.8 - 0.4 (module storage / 06d656a8, doubles);
        // the scale is stored as a float.
        Matrix const& Rot = Record.Orientation;
        int const SpinRollA = rand();
        float const SpinA = static_cast<float>(
            static_cast<double>(SpinRollA) * (1.0 / 32768.0) * 0.8 - 0.4);
        Vector SpinX;
        SpinX.x = static_cast<float>(static_cast<double>(Rot.E[0][0]) * static_cast<double>(SpinA));
        SpinX.y = static_cast<float>(static_cast<double>(Rot.E[1][0]) * static_cast<double>(SpinA));
        SpinX.z = static_cast<float>(static_cast<double>(Rot.E[2][0]) * static_cast<double>(SpinA));

        // the same about the third column (E[0][2], E[1][2], E[2][2]).
        int const SpinRollB = rand();
        float const SpinB = static_cast<float>(
            static_cast<double>(SpinRollB) * (1.0 / 32768.0) * 0.8 - 0.4);
        Vector SpinZ;
        SpinZ.x = static_cast<float>(static_cast<double>(Rot.E[0][2]) * static_cast<double>(SpinB));
        SpinZ.y = static_cast<float>(static_cast<double>(Rot.E[1][2]) * static_cast<double>(SpinB));
        SpinZ.z = static_cast<float>(static_cast<double>(Rot.E[2][2]) * static_cast<double>(SpinB));

        // the sum is the angular velocity (+0x5c).
        Record.AngularVelocity.x =
            static_cast<float>(static_cast<double>(SpinX.x) + static_cast<double>(SpinZ.x));
        Record.AngularVelocity.y =
            static_cast<float>(static_cast<double>(SpinX.y) + static_cast<double>(SpinZ.y));
        Record.AngularVelocity.z =
            static_cast<float>(static_cast<double>(SpinX.z) + static_cast<double>(SpinZ.z));

        // those units are spoken for.
        Remaining = Remaining - static_cast<unsigned int>(Record.Count);
    }

    // a non-empty vector goes to the system's loot spawner
    // (the routine, ECX = OwnerSystem +0x28) as mode 2 with this crate's id (vt+0x20), which
    // baseline reads only after the emptiness test.
    if (!Records.empty())
    {
        int const Count = static_cast<int>(Records.size());
        OwnerSystem->create_loot(get_id(), 2, &Records[0], Count);
    }
    // the vector's storage is freed (the routine, operator delete).
}
