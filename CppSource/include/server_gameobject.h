// Author: Giperion.

#pragma once

// ===========================================================================
// Server.dll's read/write object wrappers: GameObject / EqObj / Ship / Solar /
// Loot.

// These are NOT the simulated objects. The simulated objects are Common.dll's
// CObject family (CShip, CSolar, CLoot, ...). What Server.dll owns is a thin
// wrapper that implements IObjRW over one of them, and that wrapper is what the
// whole pub::SpaceObj:: export family hands around: every one of those exports
// resolves a FLID to one of these, then dispatches through its vtable.

// The hierarchy is RTTI, not inference. Server's ClassHierarchy-
// Descriptors spell it out:
//   Loot  base array module storage = { Loot, GameObject, IObjInspectImpl,
//                                        IObjRW, IObjInspect, IObjAffect,
//                                        Watchable, CBase }
//   Ship  base array module storage = { Ship, EqObj, GameObject,
//                                        IObjInspectImpl, ... }
//   Solar base array module storage = { Solar, EqObj, GameObject,
//                                        IObjInspectImpl, ... }
// i.e. GameObject : IObjInspectImpl, EqObj : GameObject, Ship/Solar : EqObj,
// Loot : GameObject. IObjInspectImpl is Common.dll's (common_iface.h) and
// already implements all 85 IObjInspect slots by delegating to the CObject it
// holds in its CBase sub-object at +0x10 -- which is exactly why baseline can get
// away with such a thin wrapper.

// The constructors confirm the same layout byte for byte:
//   GameObject::GameObject  module storage  -- IObjRW + IObjInspectImpl inlined
//                                              (+0x00/+0x04 interface vtables,
//                                              +0x08 Watchable, +0x0c CBase
//                                              vtable, +0x10 CObject*), then its
//                                              own members +0x14..+0x5f, then
//                                              its own three vtables.
//   EqObj::EqObj            module storage  -- GameObject then +0x60..+0x8b.
//   Ship::Ship              module storage  -- operator new(200 == 0xc8), EqObj
//                                              then +0x90..+0xc7.

// VTABLES (re-checked slot by slot 2026-09-25, claude-code:g-vt1):
//   * The extension vtable is declared in baseline slot ORDER: GameObject +0x154 .. +0x1b8
//     (111 slots) and EqObj +0x1bc .. +0x24c (148 slots), each with its baseline offset and
//     base body next to it. The absolute slot numbers are baseline's too: IObjInspect's 85
//     slots come first, and neither IObjInspectImpl's nor GameObject's virtual destructor
//     adds a primary slot -- it overrides CBase's (the +0x0c sub-object's only slot), the
//     same place baseline keeps it (module storage -> the routine).
//   * IObjAffect (the +0x04 sub-object): GameObject's own table (module storage) is a row
//     of baseline error stubs and is ported as such; EqObj's six overrides and Ship's own are
//     ported below; Solar keeps EqObj's.
// ===========================================================================

#include <list>
#include <vector>

#include "common_iface.h"
#include "common_cobject.h"
#include "common_archetype.h"
// EquipDescList (Ship::Jettison), EquipDesc. DamageList (common_misc.h)
// for get_damage_list below; common_cobject.h already pulls it in, but say so.
#include "common_goods.h"
#include "common_misc.h"
#include "server_exports.h"

class StarSystem;   // server_sysdb.h -- baseline GameObject+0x28 points at one
class GameObject;
class ServerEquip;  // Server.dll's per-equipment listener (EqObj+0x70 list); defined below EqObj
// ServerFuse (Fuse + 8 bytes, vtable module storage) is held BY VALUE in
// EqObj's +0x7c list, so its full definition is needed here.
#include "server_fuse.h"

// ---------------------------------------------------------------------------
// Argument records of the GameObject/EqObj extension slots. baseline passes each
// of these by pointer; the layouts are read off the base bodies that consume
// them and the call sites that build them (cited per field). Server-internal:
// none of them crosses a DLL boundary.
// ---------------------------------------------------------------------------

// +0x194 set_invincible's command, 0x0c bytes -- the node value of the
// GameObject+0x2c list (node 0x14 = 8 + 0x0c). module storage reads +0 (the key,
// compared per node at module storage), +4/+5 (bytes, copied into +0x38/+0x39)
// and +8 (float, into +0x3c, reduced with a minimum at module storage).
// Ship the routine (module storage) passes {Type 1, bProtect 1, bFromPlayers 1,
// MaxHpLoss 0.0f} (module storage / 06d01130 / 06d01134 / 06d0113b).
struct FInvincibility
{
    unsigned int Type;          // +0x00  list key
    unsigned char bProtect;     // +0x04  blocks non-player damage   -> GameObject+0x38
    unsigned char bFromPlayers; // +0x05  blocks player damage too    -> GameObject+0x39
    unsigned char Pad06[2];
    float MaxHpLoss;            // +0x08  fraction of max hp that may still be lost -> +0x3c
};
static_assert(sizeof(FInvincibility) == 0x0c, "FInvincibility is the 0x0c list value");

// +0x17c damage_sub_obj's record. Built by StarSystem the routine
// (module storage..06d0d7ca): +0 = the inflictor's id, +4 = the damaged
// sub-object, +8 = the damage, +0xc = the routine's own third (byte) argument.
// Read by module storage (+4) / module storage (+8) and EqObj module storage /
// .
struct FSubObjDamage
{
    unsigned int InflictorId;   // +0x00
    unsigned short SubObjId;    // +0x04
    unsigned short Pad06;
    float Damage;               // +0x08
    unsigned char Field_0x0c;   // +0x0c  a byte flag; meaning not yet derived
    unsigned char Pad0d[3];
};

// +0x180 munition_hit's record. module storage reads the sub-object at +8 and
// /06ceeb66 the munition archetype at +4 (its +0x80 HullDamage).
struct FMunitionImpact
{
    unsigned int ShooterId;                  // +0x00
    Archetype::Munition const* Munition;     // +0x04
    unsigned short SubObjId;                 // +0x08
    unsigned short Pad0a;
    Vector HitPos;                           // +0x0c
};

// +0x184 explosion_hit's record (and +0x1fc..+0x208). module storage reads the
// position at +0xc and the Explosion archetype at +0x18 (Radius +4, HullDamage
// +0xc); EqObj's bodies read the inflictor at +4 and the cause at +8.
struct ExplosionDamageEvent
{
    unsigned int ExplodingObjId;             // +0x00
    unsigned int InflictorId;                // +0x04
    DamageCause Cause;                       // +0x08
    Vector Position;                         // +0x0c
    Archetype::Explosion const* Explosion;   // +0x18
    // +0x1c -- a collision-group name the blast skips. StarSystem's explosion
    // (the routine) reads it per candidate: module storage `MOV ECX,[ESP+0x188]`
    // (the record) / module storage `MOV ECX,[ECX+0x1c]`, and when both it and
    // PhySys::GetCollisionGroup(candidate) are non-null and NEURONCOMPONENTS stricmp says
    // equal (module storage), the candidate is not collected. The MedMine blast
    // stores 0 here (module storage `MOV [ESP+0x40],EDI`, record base S+0x24).
    char const* SkipCollisionGroup;          // +0x1c
};

// +0x1a4 on_collision's record, 0x30 bytes, built twice per PhySys collision by
// the system collision pass (module storage..06ce3610 for side A,
// for side B): each side gets its own sub-object, the
// other side's wrapper and sub-object, the shared contact point, and the two
// vectors at +0x18/+0x24 NEGATED for side B (FCHS at module storage..06ce367b).
struct FCollisionEvent
{
    unsigned short SubObjId;                 // +0x00
    unsigned short Pad02;
    GameObject* Other;                       // +0x04
    unsigned short OtherSubObjId;            // +0x08
    unsigned short Pad0a;
    Vector Point;                            // +0x0c  same for both sides
    Vector Field_0x18;                       // +0x18  negated for side B
    Vector Field_0x24;                       // +0x24  negated for side B
};
static_assert(sizeof(FCollisionEvent) == 0x30, "FCollisionEvent 0x30");

// One loot object to create, 0x6c bytes -- the element of the std::vector that
// EqObj::build_loot (+0x244, module storage) / build_equip_loot (+0x248,
// ) fill and StarSystem the routine consumes (the callers divide
// the byte span by 0x6c: `MOV EAX,0x4bda12f7 / IMUL / SAR EDX,5`).
// Field sources: module storage..06cec4b7 and module storage..06cec219.
struct FLootRecord
{
    unsigned int Id;                                 // +0x00  CFLIDMaker id & 0x3fffffff
    Archetype::Equipment const* Arch;                // +0x04  the loot object's archetype
    float HitPts;                                    // +0x08
    Archetype::Equipment const* ContentsArch;        // +0x0c  what it carries (itself unless a cargo pod)
    float ContentsHitPts;                            // +0x10
    int Count;                                       // +0x14
    unsigned int Field_0x18;                         // +0x18  zeroed by build_loot; meaning not derived
    unsigned int Field_0x1c;                         // +0x1c  zeroed by build_loot; meaning not derived
    Vector Velocity;                                 // +0x20
    Vector Position;                                 // +0x2c
    Matrix Orientation;                              // +0x38  (9 dwords, copied with REP MOVSD ECX=9)
    Vector AngularVelocity;                          // +0x5c
    unsigned char Field_0x68;                        // +0x68  build_loot stores 1 (module storage)
    unsigned char bMission;                          // +0x69  EquipDesc+0x18 / CEquip vt+0x14
    unsigned char Pad6a[2];
};
static_assert(sizeof(FLootRecord) == 0x6c, "FLootRecord is the 0x6c-byte loot element");

// Controller message 0x39 (the routine, the port's Server_ControllerDispatch
// broadcast): "this object was destroyed". Built on the stack by EqObj::destroy
// (module storage-06ce8339) and Loot::destroy (module storage-06cf26fc); the
// dispatcher only forwards the pointer.
struct FObjectDestroyedEvent
{
    unsigned int ObjId;                      // +0x00  get_id()
    unsigned int KillerId;                   // +0x04  destroy()'s second argument
    unsigned int SystemId;                   // +0x08  the routine(OwnerSystem)
    unsigned int OwnerPlayer;                // +0x0c  EqObj: CSimple+0xb4; Loot: 0
    int Kind;                                // +0x10  destroy()'s first argument
    Vector Position;                         // +0x14  get_position()
    // +0x20 (VC6 vector: allocator byte, first/last/end). EqObj fills it through
    // collect_droppable_equip (+0x1f4) when Kind == 1; Loot leaves it empty.
    std::vector<EquipDesc> DroppedEquip;
};

// Controller message 0xd (the routine): "a sub-object's hit points were
// applied". Built on the stack by EqObj's four apply_*_entry slots
// (root module storage..06ceb603, equip module storage..06ceb940, power
// , group module storage..06cebb9b): nine dwords
// zeroed, then filled per field below.
struct FSubObjDamageEvent
{
    unsigned int ObjId;                      // +0x00  get_id()
    unsigned int SystemId;                   // +0x04  the routine(OwnerSystem)
    unsigned int InflictorId;                // +0x08  DamageList+0x14
    unsigned int EquipType;                  // +0x0c  CEquip+0x14 (equip), 0x40000 (power), 0 otherwise
    unsigned int CollisionGroupType;         // +0x10  CArchGroup::GetCollisionGroupType (group), 0 otherwise
    DamageCause Cause;                       // +0x14  DamageList+0x10
    float Damage;                            // +0x18  hit points before - Entry.HitPts
    float RelativeDamage;                    // +0x1c  Damage / max hit points
    float RelativeHealth;                    // +0x20  Entry.HitPts / max hit points
};
static_assert(sizeof(FSubObjDamageEvent) == 0x24, "FSubObjDamageEvent is nine dwords");

// Controller message 0x13 (the routine): "this equipment is about to be
// dropped as loot". Built by EqObj::destroy_equip (module storage..06cea109)
// and EqObj::destroy_cargo (module storage..06ce9da2) for fate 4 only: the four
// dwords, then CEquip vt+0x28 GetEquipDesc into Desc. After the dispatch baseline
// copies Desc back out (REP MOVSD, 8 dwords) and loots THAT copy, so a handler
// may rewrite it.
struct FEquipLootEvent
{
    unsigned int SystemId;                   // +0x00
    unsigned int InflictorId;                // +0x04
    unsigned int ObjId;                      // +0x08
    DamageCause Cause;                       // +0x0c
    EquipDesc Desc;                          // +0x10
};
static_assert(sizeof(FEquipLootEvent) == 0x30, "FEquipLootEvent is 0x30 bytes");

// Controller message 0x38 (the routine), raised the first time a death fuse is
// lit: by EqObj::light_fuse (module storage..06cec66a) and EqObj::destroy
// (module storage..06ce812a). The handler may write a lifetime into +0x0c; the
// caller reads it back after the dispatch.
struct FDeathFuseEvent
{
    unsigned int SystemId;                   // +0x00
    unsigned int ObjId;                      // +0x04
    unsigned int ObjType;                    // +0x08  IObjInspect slot 34 get_type
    float Lifetime;                          // +0x0c  zeroed before the dispatch
};
static_assert(sizeof(FDeathFuseEvent) == 0x10, "FDeathFuseEvent is four dwords");

// ---------------------------------------------------------------------------
// GameObject -- the base wrapper. module storage.
// ---------------------------------------------------------------------------
class GameObject : public IObjInspectImpl
{
public:
    GameObject();
    virtual ~GameObject();

    // -- Server.dll's own extension of the IObjInspect vtable ---------------

    // DECLARATION ORDER IS BASELINE SLOT ORDER (2026-09-24, claude-code:w5-server-06ce).
    // Every virtual below is declared in the order of GameObject's own vtable
    // (module storage family; GameObject ends at +0x1b8 = 111 slots, EqObj
    // continues to +0x24c = 148 slots, Solar adds +0x250). The byte offset in each
    // `// +0xNNN` comment is the baseline slot; the address is the base body. The
    // absolute slot numbers match baseline's as well (IObjInspect's 85 first; the
    // virtual destructor overrides CBase's slot and adds none here -- see the file
    // header). Do not insert a virtual between two of these without a baseline slot
    // for it.

    // -- The four slots baseline puts between IObjInspect's last (+0x150) and
    //    create() (+0x164). -------------------------------------------------

    // IDENTIFIED 2026-09-17 (Server.dll lane, wave 8). The header comment above
    // called these "four unidentified GameObject slots"; they are not
    // unidentified any more. Ship's vtable is at module storage, anchored
    // twice and independently: create() sits at +0x164 (module storage ->
    // , this file's own citation) and get_invincible_flag() at
    // +0x198 (module storage -> module storage, likewise). Slot +0x150 is
    // , `JMP dword ptr [->COMMON.DLL::IObjInspectImpl::...]`,
    // i.e. an import thunk -- which is exactly what IObjInspect's last slot
    // (cobject(), implemented in Common.dll) has to look like from here, and is
    // the third confirmation of the base address.

    // All thirteen Server.dll vtables that share this layout were dumped and
    // compared, which is what separates GameObject's own body from an override:
    //   +0x154  base module storage;  overridden 5x (CounterMeasure module storage,
    // , module storage, module storage, module storage)
    //   +0x158  base module storage;  overridden 6x (EqObj module storage, ...)
    //   +0x15c  base module storage;  overridden by EqObj/Ship/Solar
    //           (module storage, which tail-jumps back into the base body)
    //   +0x160  module storage in ALL THIRTEEN -- never overridden.

    // +0x154. Base module storage:
    //     MOV EAX,[ECX+0x10] / <the standard (ObjectClass & 3) == 3 guard> /
    //     MOV EAX,[EAX+0xe0] / RET
    // i.e. `return cobj()->TypeBits;` -- CSimple+0xe0, which common_cobject.h
    // already names TypeBits ("arch ObjectType after open"). Distinct from
    // IObjInspect::get_type(uint&) at slot 34.
    virtual unsigned int get_type_bits() const;

    // +0x158. Base module storage, RET 8 -- two arguments, always returns true:
    //     cobj()->set_hit_pts(0.0f);        ; PUSH 0 / CALL [CObjectVt + 0x98]
    //     StarSystem_OnObjectDestroyed(OwnerSystem, this, Kind);   ; 06d0b160
    //     this->destroy_notify();           ; CALL [own vtable + 0x15c]
    //     return true;
    // The CObject vtable offsets are read off CSimple's own table
    // (??_7CSimple@@6B@ at module storage in Common.dll): +0x98 is
    // == CSimple::set_hit_pts, +0x94 is module storage ==
    // CSimple::disconnect. Only the FIRST argument reaches the base body;
    // baseline compares it against 1, both inside module storage (06d0b1ea
    // `CMP EBP,0x1`) and in EqObj's override (06ce80d8 `CMP EDI,0x1`). The
    // second argument is the object's own id: StarSystem::remove_player builds
    // it with `CALL [EDI+0x20]` (IObjInspect::get_id) and pushes it second
    // (module storage..06d087d8).
    virtual bool destroy(int Kind, unsigned int ObjId);

    // +0x15c. Base module storage, RET 0:
    // c6464001  MOV byte ptr [ESI+0x40],0x1   ; bDestroyed
    //     if (SinglePlayer() && cobj()->OwnerPlayer != 0) return;  ; CSimple+0xb4
    //     cobj()->disconnect(OwnerSystem);          ; CALL [CObjectVt + 0x94]
    // EqObj/Ship/Solar override it (module storage) to first drain the
    // equipment list at EqObj+0x64 through module storage and then
    // `JMP the routine` straight into this base body.
    virtual void destroy_notify();

    // +0x160. `MOV AL,[ECX+0x40]; RET` at module storage. The predicate
    // StarSystem::remove_player tests before destroying an object
    // (module storage `CALL [EAX+0x160]` / `TEST AL,AL` / skip when set).
    virtual bool is_destroyed() const;

    // baseline slot +0x164 (Ship module storage, Solar module storage, Loot
    // , CounterMeasure module storage): COMMON.DLL::CObject::Alloc
    // with the subclass's literal ObjectClass into CBase::Object,
    // Archetype::Root::create_instance(Object), NeuronRuntime::Engine vtable +0x6c,
    // CObject::open(Arch) and CBase::advise(true). The ORDER differs per
    // subclass (Ship advises before open, Solar opens first, Loot opens first and
    // creates the instance last) and so does the result (Ship/Solar:
    // Object != nullptr; Loot/CounterMeasure: get_index() != -1).
    // GameObject's and EqObj's own entries are _purecall (module storage).
    virtual bool create(Archetype::Root* Arch) = 0;

    // baseline slot +0x168 (Ship module storage): COMMON.DLL::CObject::Find(Id,
    // ObjectClass) instead of Alloc, then the same CBase::advise(true).
    // _purecall in GameObject/EqObj like +0x164.
    virtual bool find(unsigned int const& Id) = 0;

    // baseline slot +0x16c (Ship module storage -> module storage). Called from
    // during StarSystem physics. Base body is the routine:
    // return !is_destroyed(). EqObj overrides with module storage, Ship with
    // .
    virtual bool update(float Dt);

    // +0x170 -- base module storage (RET 8); Ship overrides with module storage.
    // OwnerSystem = Sys; when Parms is given, CObject::remake_physical(*Parms,
    // 0.0f) (CObject vt +0x78); outside single player an owned object starts
    // remote-controlled physics (PhySys::RCBegin); then CSimple::connect(Sys)
    // (vt +0x90) unless it is a player's object in single player.
    virtual void enter_system(StarSystem* Sys, PhySys::CreateParms const* Parms);

    // +0x174 -- base module storage (RET 4). The inverse of enter_system: the
    // system is told first (the routine -> +0x178 on every object), RCEnd,
    // then OutParms (when given) is filled from the live physics body, the body
    // is unmade (CObject vt +0x74), CSimple::disconnect, OwnerSystem = nullptr.
    virtual void leave_system(PhySys::CreateParms* OutParms);

    // +0x178 -- base module storage (RET 4). Called by StarSystem the routine on
    // every object of the system when ObjId leaves it. A CEqObj forwards a
    // type-3 notification record to its IBehaviorManager (vt slot 0).
    virtual void on_object_left_system(unsigned int ObjId);

    // +0x17c -- base module storage (RET 8); EqObj module storage, Solar
    // . Hull part: damage_hull(Hit.Damage) when Hit.SubObjId is
    // SUBOBJ_ID_ROOT or SUBOBJ_ID_NONE.
    virtual void damage_sub_obj(FSubObjDamage const& Hit, DamageList& Damage);

    // +0x180 -- base module storage (RET 8); EqObj module storage, Solar
    // . Hull part: damage_hull(Munition::HullDamage) for a hit on
    // the root / no sub-object.
    virtual void munition_hit(FMunitionImpact const& Hit, DamageList& Damage);

    // +0x184 -- base module storage (RET 8); EqObj module storage. Hull damage
    // from an explosion, in three distance bands of the blast radius.
    virtual void explosion_hit(ExplosionDamageEvent const& Blast, DamageList& Damage);

    // +0x188 -- base module storage (RET 4); EqObj/Solar module storage, Ship
    // . Applies a finished DamageList: root entries through
    // apply_root_damage (+0x1b4), the system broadcast (StarSystem_DamageObject,
    // ), and destroy(1, InflictorId) when the list says destroyed.
    virtual void apply_damage_list(DamageList& Damage);

    // +0x18c -- module storage `MOV AL,[ECX+0x41]; RET`. Never overridden.
    virtual bool is_in_damage_callback() const;

    // +0x190 -- module storage (RET 4). Sets the +0x41 latch; clearing it
    // flushes whatever PendingDamage collected meanwhile through +0x188.
    virtual void set_in_damage_callback(bool bIn);

    // +0x194 -- module storage (RET 4). Adds or replaces one invincibility
    // command (keyed by Type) in the +0x2c list and recomputes +0x38/+0x39/+0x3c.
    virtual void set_invincible(FInvincibility const& Cmd);

    // baseline slot +0x198 -- `MOV AL,[ECX+0x39]; RET` at module storage.
    virtual unsigned char get_invincible_flag() const;
    // baseline slot +0x19c -- `MOV AL,[ECX+0x38]; RET` at module storage.
    virtual unsigned char is_invincible() const;
    // baseline slot +0x1a0 -- `FLD [ECX+0x3c]; RET` at module storage.
    virtual float get_invincible_timer() const;

    // +0x1a4 -- module storage (RET 4). Object-vs-object collision: relative
    // speed -> Collision::CalcCollisionDamage, handed to the system
    // (StarSystem the routine), which damages the OTHER object.
    virtual void on_collision(FCollisionEvent const& Hit);

    // baseline slot +0x1a8. ADDED 2026-09-17 (wave 14) because the per-frame
    // observer update needs it: module storage is
    // 6800000040      PUSH 0x40000000
    // 8d442470        LEA  EAX,[ESP + 0x70]
    // 50              PUSH EAX
    // ff92a8010000    CALL dword ptr [EDX + 0x1a8]
    // on a stack DamageList that the caller then tests for emptiness.

    // CORRECTION, and the reason the second argument is a float here: 0x40000000
    // is NOT a bitmask. Ship's override for this slot is module storage ->
    // (Ship's vtable base is module storage, anchored by
    // create() at +0x164 -> module storage and get_invincible_timer() at
    // +0x1a0 -> module storage), and that body uses the argument as
    // ff5244          CALL dword ptr [EDX + 0x44]  ; CEquip status
    // d85c2434        FCOMP float ptr [ESP + 0x34] ; <- the arg
    // f6c405          TEST  AH,0x5
    // 7a9d            JP    the branch           ; >= -> skip
    // i.e. a FLOAT comparison against each equip's status. 0x40000000 read as a
    // float is 2.0f, so "everything, whatever its status" -- a health ceiling,
    // not a mask. The tracker's earlier note on the routine called it "mask
    // "; that reading is wrong and this comment supersedes it.

    // PORTED 2026-09-17 (wave 15). The base body is the routine
    // (module storage), which ten of the thirteen wrapper vtables carry
    // unchanged; EqObj/Ship/Solar override it with module storage. Both are in
    // src/Server/server_gameobject.cpp with their byte trails.

    // CORRECTION to the sketch that used to stand here: it said the non-
    // destroyed arm records "the equip's own status", i.e. the +0x44 value the
    // comparison uses. It does not. module storage `ff5244` (GetRelativeHealth)
    // is the COMPARISON and module storage `ff503c` (GetHitPoints) is the value
    // that goes into the entry -- two different accessors, named by
    // ??_7CETractor@@6B@ at module storage and module storage.
    virtual void get_damage_list(DamageList& Out, float StatusCeiling) const;

    // +0x1ac -- module storage (RET 4). Queues a root entry of
    // get_max_hit_pts() * Relative in PendingDamage (destroyed when <= 0) and,
    // unless already inside a damage callback, applies it at once.
    virtual void set_relative_health(float Relative);

    // +0x1b0 -- base module storage (RET 8); EqObj/Ship/Solar module storage
    // (difficulty scale and armor first). Adds the new root hit points to
    // Damage, honouring the invincibility floor; true when the hull reaches 0.
    virtual bool damage_hull(float Amount, DamageList& Damage);

    // +0x1b4 -- module storage (RET 4): csimple()->set_hit_pts(Entry.HitPts).
    virtual void apply_root_damage(DamageEntry const& Entry);

    // +0x1b8 -- module storage (RET 4). The invincibility gate: causes >= 0x10
    // are never blocked; otherwise a player inflictor is checked against +0x39,
    // everything else against +0x38.
    virtual bool is_damage_blocked(DamageList const& Damage) const;

    // Bind an already-existing CObject to this wrapper. baseline does exactly this in
    // its find() slot (module storage) after COMMON.DLL::CObject::Find hands one back:
    // store it in CBase::Object, then CBase::advise(true). Named separately here
    // because the callers that need it in this port already hold the CObject (the
    // single-player system loader walks CObject::FindFirst) rather than an id.
    void attach(CObject* Obj);

    // The CObject this wrapper reads and writes, with baseline's own guard: every
    // accessor in the family loads CBase::Object and nulls it unless
    // (ObjectClass & 3) == 3 (see module storage, module storage and a dozen
    // others). Non-virtual on purpose -- baseline inlines the guard everywhere.
    CObject* cobj() const;
    CSimple* csimple() const;

    // -----------------------------------------------------------------
    // baseline +0x14 .. +0x1F -- the "marked by" player list. DECODED
    // 2026-09-17 (wave 13); it used to be one of the "four VC6 list heads
    // this port does not model".

    // The constructor builds it (raw bytes, module storage):
    // 6a0c     PUSH 0xc                 ; node size
    // 884e14   MOV  byte [ESI+0x14],CL  ; std::allocator
    // CALL MSVCRT.DLL::operator_new
    // 8900     MOV  [EAX],EAX
    // 894004   MOV  [EAX+4],EAX         ; self-linked sentinel
    // 894618   MOV  [ESI+0x18],EAX      ; _Head
    // 895e1c   MOV  [ESI+0x1c],EBX      ; _Size = 0
    // so the MSVC6 shape is {allocator +0x14, _Head +0x18, _Size +0x1C} over
    // 0x0C-byte nodes {next, prev, unsigned int} -- and the element really is
    // a bare dword, because the routine's own `operator new(0xc)` writes only
    // node[2].

    // What the dword IS comes from baseline's own export name:
    // pub::Player::MarkObj (?MarkObj@Player@pub@@YAHIIH@Z, module storage)
    // resolves the object id and calls the routine(obj, player_id, mark_type)
    // -- so the list holds PLAYER IDS, and the predicate the routine answers
    // "has that player marked this object?".

    // (module storage) is mark_for_player() below: it searches the
    // list for the id, push_backs it when MarkType == 1 and it is absent,
    // erases it when MarkType == 0 and it is present, and keeps _Size in step
    // both ways.
    // (module storage) is is_marked_by(): fourteen instructions,
    // `MOV ECX,[ECX+0x18]` / `CMP [EAX+8],EDX` / `SETNZ AL`.

    // The answer is a wire field in at least three payloads:
    // 884208        MOV byte [EDX + 0x8],AL    ; CREATELOOT +0x08
    // 888424dc000000 MOV byte [ESP + 0xdc],AL  ; CREATESHIP +0xA8
    // 8885a8000000  MOV byte [EBP + 0xa8],AL   ; CREATESOLAR +0xA8
    // The CREATESOLAR site is the cleanest of the three and is worth reading in
    // full, because the two dwords it writes are the two payload fields this
    // wave chased: module storage loads the observer's player id off node+0x40,
    // asks this predicate, module storage stores the answer,
    // is `MOV EAX,[EDX + EAX*1 + 0xffffff00]` (Players +
    // id*0x418 - 0x100, i.e. PlayerData::rep_vibe), module storage stores THAT
    // at payload +0xb0, and module storage finally calls the notifier.

    // A real container, because baseline has one. The port uses std::list rather
    // than reproducing the intrusive node by hand: nothing here is a raw heap
    // block the way Observer's node is, so there is no reason to.
    std::list<unsigned int> MarkedByPlayers;

    // (module storage, `RET 0x8`). MarkType is baseline's `param_2`,
    // and baseline tests it against the literals 1 and 0 rather than for
    // non-zero: `module storage  83fa01  CMP EDX,0x1` adds,
    // `module storage  85d2  TEST EDX,EDX` removes, and every other value falls
    // through to the branch having done nothing at all. The port keeps the int
    // and keeps that three-way split -- do not collapse it to a bool.
    void mark_for_player(unsigned int PlayerId, int MarkType);

    // (module storage, `RET 0x4`).
    bool is_marked_by(unsigned int PlayerId) const;

    // GameObject's own data members, from the constructor at module storage
    // (every member it writes, 2026-09-24) and the accessors above.

    // baseline +0x20/+0x24 -- a DOUBLE. The ctor zeroes both dwords
    // (module storage/06cee3da); the only writer is EqObj::apply_damage_list,
    // `CALL [->Timing::read]` / `FSTP double ptr [ESI+0x20]` (module storage /
    // ) while the hull's relative health is above 0; readers are
    // EqObj::update (module storage `FSUB double ptr [EBX+0x20]`) and StarSystem
    // (module storage).
    double LastDamageTime;

    // baseline +0x2c/+0x30/+0x34 -- VC6 std::list of 0x14-byte nodes (0x0c of
    // value): the active invincibility commands, one per Type. Built by the ctor
    // (`PUSH 0x14` / operator new), maintained only by set_invincible (+0x194),
    // freed by the dtor through the routine.
    std::list<FInvincibility> Invincibilities;

    // +0x38/+0x39/+0x3c are the reduction of Invincibilities that
    // set_invincible (+0x194) recomputes: "any command blocks non-player damage",
    // "any command blocks player damage", and the minimum over the commands of
    // the fraction of max hit points the object may still LOSE (the floor is
    // 1 - that fraction, see damage_hull). The historical name InvincibleTimer
    // is kept because other lanes' code spells it; it is NOT a timer.
    unsigned char bInvincible;      // baseline +0x38, ctor zeroes it
    unsigned char bInvincibleFlag;  // baseline +0x39, ctor zeroes it
    float InvincibleTimer;          // baseline +0x3c, ctor stores 1.0f -- max hp-loss fraction

    // baseline +0x40. Written only by destroy_notify() (module storage) and read
    // only by is_destroyed() (module storage); a one-way latch.
    unsigned char bDestroyed;

    // baseline +0x41 -- read by is_in_damage_callback (+0x18c), written by
    // set_in_damage_callback (+0x190).
    unsigned char bInDamageCallback;

    // baseline +0x44..+0x5f -- a DamageList by value (0x1c bytes). The ctor
    // constructs it and then stores 0x18 into its Cause (+0x54, module storage `MOV dword ptr [ESI+0x54],0x18`);
    // set_relative_health (+0x1ac) queues into it and set_in_damage_callback
    // (+0x190) drains it.
    DamageList PendingDamage;

    // baseline +0x28. The StarSystem this object lives in. Two independent reads
    // prove what it is:
    //   * takes it in ECX and immediately walks `[ECX+0xc]` to
    //     `[ECX+0xc]`'s sentinel -- StarSystem's player list (server_sysdb.h).
    //   * hands it to CSimple::disconnect, whose parameter is an
    //     IObjDB*. baseline's StarSystem is an IObjDB (RTTI .?AVStarSystem@@,
    //     single inheritance), and so is the port's (server_sysdb.h);
    //     destroy_notify() makes that call.
    StarSystem* OwnerSystem;

    // -- IObjAffect: GameObject's own table (module storage), every entry a baseline error
    //    stub (-3 / -8 / 0, see server_gameobject.cpp). Ship and EqObj override most of
    //    them (EqObj's six below, Ship's in its own class). ----------------------------
    int set_throttle(float Throttle) override;
    int set_axis_throttle(Vector const& AxisThrottle) override;
    int set_nudge_vec(Vector const& NudgeVec) override;
    int set_strafe_dir(StrafeDir Dir) override;
    int set_formation_leader(IObjRW* Leader) override;
    int set_formation_offset(Vector const& Offset) override;
    int add_formation_follower(IObjRW* Follower) override;
    int remove_formation_follower(IObjRW* Follower) override;
    int get_formation_follower_count() override;
    int set_follow_leader(IObjRW* Leader) override;
    int set_follow_offset(Vector const& Offset) override;
    int add_follow_follower(IObjRW* Follower) override;
    int remove_follow_follower(IObjRW* Follower) override;
    int slot_34(unsigned short const* EquipIds, unsigned Count, Vector const* TargetPos,
                int SourceClient) override;
    int slot_38(unsigned short EquipId, int Count, int SourceClient) override;
    int slot_3c(unsigned short const* EquipId, std::vector<IObjRW*> const& Targets,
                int SourceClient) override;
    int slot_40(unsigned short const* EquipId, IObjRW* Target, int SourceClient) override;
    int slot_44(unsigned short const* EquipId, IObjRW* Target, unsigned short SubTarget,
                int SourceClient) override;
    int set_target(IObjRW* Target, unsigned short SubTarget, int SourceClient) override;
    int slot_4c(unsigned short const* EquipId, bool bActivate, int SourceClient) override;
    int slot_50(bool bEngage, bool bArg2, int SourceClient) override;
    int slot_54(bool bActivate, int SourceClient) override;
    int slot_58(unsigned short EquipId, unsigned Count, int SourceClient) override;
    int slot_5c(int RequestType, unsigned const* TargetId, unsigned const* SecondId,
                int bFlag, RequestCB* Callback) override;
    int slot_60(int RequestType, unsigned const* TargetId, int Arg3) override;
    int go_tradelane(IObjInspect const* EntranceRing, IObjInspect const* ExitRing,
                     bool bFastExit, float Speed) override;
    int stop_tradelane() override;
    int slot_6c(int ArgA, int ArgB) override;
    int slot_70(unsigned Id) override;
    int slot_74(unsigned Arg) override;

protected:
    // Ship::create's body (module storage). baseline has no such shared function;
    // Solar::create and Loot::create order the same calls differently (see
    // create() above), so they spell their own sequence out.
    bool CreateOfClass(CObject::Class ObjectClass, Archetype::Root* Arch);
    bool FindOfClass(CObject::Class ObjectClass, unsigned int const& Id);
};

// ---------------------------------------------------------------------------
// EqObj -- module storage. Adds no modelled state (see the header comment).
// ---------------------------------------------------------------------------
class EqObj : public GameObject
{
public:
    EqObj();
    virtual ~EqObj();

    // baseline slot +0x1a8 again -- module storage, the equipment-aware override.
    // It is installed in exactly three vtables and this class is the common
    // ancestor of all three: EqObj's own (module storage + 0x1a8 ==
    // ), Ship's (module storage + 0x1a8 == module storage) and
    // Solar's (module storage + 0x1a8 == module storage). Every other wrapper
    // keeps the base the routine.

    // NOT an ABI change: the slot already exists on GameObject, this only
    // overrides it.
    virtual void get_damage_list(DamageList& Out, float StatusCeiling) const;

    CEqObj* ceqobj() const;

    // (module storage, RET 4). GameObject::update's gate
    // (the routine), the weapons-offline repair pass, the dock-bay tick
    // (the routine), every ServerEquip's update, the cargo-decay timer (+0x6c,
    // decay_cargo every 1.0 s), Fuse::Burn on every lit fuse; true while alive.
    bool update(float Dt) override;

    // -- GameObject slots EqObj overrides (vtable module storage) -----------
    // +0x38 (IObjInspect slot 14, module storage; Ship 06d67154 and Solar
    // keep it) module storage: `MOV EAX,[ECX]; JMP [EAX+0x1ec]` --
    // is_death_fuse_burning.
    bool is_dying() const override;

    // -- IObjAffect overrides (EqObj's table module storage; Ship module storage
    //    and Solar keep these six entries). `this` is the +4 sub-object in
    //    baseline; the listing's [this+N] are EqObj+N+4. ----------------------
    // slot 13 (+0x34) module storage (RET 0x10): fire Count equips by id.
    int slot_34(unsigned short const* EquipIds, unsigned Count, Vector const* TargetPos,
                int SourceClient) override;
    // slot 15 (+0x3c) module storage (RET 0xc): tractor every object of Targets.
    int slot_3c(unsigned short const* EquipId, std::vector<IObjRW*> const& Targets,
                int SourceClient) override;
    // slot 16 (+0x40) module storage (RET 0xc): tractor one object.
    int slot_40(unsigned short const* EquipId, IObjRW* Target, int SourceClient) override;
    // slot 17 (+0x44) module storage (RET 0x10): CEGun::SetTarget on one gun.
    int slot_44(unsigned short const* EquipId, IObjRW* Target, unsigned short SubTarget,
                int SourceClient) override;
    // slot 19 (+0x4c) module storage (RET 0xc): (de)activate one equip.
    int slot_4c(unsigned short const* EquipId, bool bActivate, int SourceClient) override;
    // slot 22 (+0x58) module storage (RET 0xc): use Count of a repair kit /
    // shield battery cargo stack ("EqObj::use_item()" in its FDUMP).
    int slot_58(unsigned short EquipId, unsigned Count, int SourceClient) override;
    // (RET 0xc, non-virtual): slot_58's repair-kit branch; mends
    // the hull and returns how many kits that took. EquipId is not read.
    int use_repair_kits(unsigned short EquipId, Archetype::RepairKit const* Kit, int Count);
    // (RET 0xc, non-virtual): slot_58's shield-battery branch;
    // recharges the weakest shield (type 0x100) and returns how many
    // batteries that took. EquipId is not read.
    int use_shield_batteries(unsigned short EquipId, Archetype::ShieldBattery const* Battery,
                             int Count);
    bool destroy(int Kind, unsigned int ObjId) override;                 // +0x158 module storage
    void destroy_notify() override;                                      // +0x15c module storage
    void damage_sub_obj(FSubObjDamage const& Hit, DamageList& Damage) override;      // +0x17c module storage
    void munition_hit(FMunitionImpact const& Hit, DamageList& Damage) override;      // +0x180 module storage
    void explosion_hit(ExplosionDamageEvent const& Blast, DamageList& Damage) override; // +0x184 module storage
    void apply_damage_list(DamageList& Damage) override;                 // +0x188 module storage
    bool damage_hull(float Amount, DamageList& Damage) override;         // +0x1b0 module storage

    // -- EqObj's own extension slots, +0x1bc .. +0x24c, in baseline order -----
    // baseline bodies are identical in EqObj's, Ship's and Solar's tables except
    // where noted (Ship +0x1e4 module storage and +0x1f4 module storage, Solar
    // +0x1fc module storage and its own +0x250 module storage).

    // +0x1bc module storage (RET 0). rebuild_server_equip (+0x1dc), then lights
    // every archetype fuse (Archetype::EqObj +0x74 list) whose hit-point
    // threshold the hull has reached, then every arch group's FuseIgnitions.
    virtual void check_fuses();
    // +0x1c0 module storage (RET 0xc). damage_hull(Damage) -- with bCanKill only
    // for Damage >= 0, without it only when the hull stays above 0.
    virtual void inflict_hull_damage(float Damage, bool bCanKill, DamageList& Out);
    // +0x1c4 module storage (RET 0x10). By arch-group NAME (StringAlloc +
    // CArchGroupManager::FindByName); false when there is no such group.
    virtual bool inflict_group_damage(char const* GroupName, float Damage, bool bCanKill,
                                      DamageList& Out);
    // +0x1c8 module storage (RET 0xc). destroy_group(group, Fate, Out, true).
    virtual bool destroy_group_by_name(char const* GroupName, DamageEntry::SubObjFate Fate,
                                       DamageList& Out);
    // +0x1cc module storage (RET 0x10). By HARDPOINT (CEquipManager::FindByHardpoint).
    virtual bool inflict_equip_damage(char const* Hardpoint, float Damage, bool bCanKill,
                                      DamageList& Out);
    // +0x1d0 module storage (RET 0xc). destroy_equip(equip, Fate, Out).
    virtual bool destroy_equip_by_hardpoint(char const* Hardpoint, DamageEntry::SubObjFate Fate,
                                            DamageList& Out);
    // +0x1d4 module storage (RET 8). Every cargo item goes out as loot through
    // build_loot (+0x244) and StarSystem the routine; Hardpoint (may be null)
    // becomes the loot's origin hardpoint.
    virtual bool dump_cargo(char const* Hardpoint, DamageList& Out);
    // +0x1d8 module storage (RET 4). damage_shield(shield, -1.0f, Out) for every
    // shield (CEquipTraverser 0x100); always true.
    virtual bool drain_shields(DamageList& Out);
    // +0x1dc module storage (RET 0). clear_server_equip, then one ServerEquip
    // (the routine) per CEquip.
    virtual void rebuild_server_equip();
    // +0x1e0 module storage (RET 0). Deletes every ServerEquip (their vtable
    // slot 1) and empties the +0x70 list.
    virtual void clear_server_equip();
    // +0x1e4 module storage (RET 0x14); Ship overrides with module storage.
    // Re-ignites the burning copy of FuseId if there is one; otherwise copies the
    // prototype out of ServerFuseDB into Fuses, binds this object as owner,
    // raises controller event 0x38 the first time a death fuse is lit, applies
    // the Lifetime override and ignites it. Broadcasts through StarSystem
    // when the ignition took. Returns whether a fuse was found.
    virtual bool light_fuse(unsigned int SourceId, ID_String const& FuseId,
                            unsigned short SubObjId, float AtT, float Lifetime);
    // +0x1e8 module storage (RET 0xc). Fuse::UnBurn on the burning copy of FuseId,
    // broadcast through the routine when it took. Returns whether it was found.
    virtual bool unlight_fuse(ID_String const& FuseId, unsigned short SubObjId, float AtT);
    // +0x1ec module storage (RET 0). Any death fuse (Fuse+0xc) not yet spent.
    virtual bool is_death_fuse_burning();
    // +0x1f0 module storage (RET 0). The archetype's explosion_arch (Root+0x24),
    // as a cause-4 ExplosionDamageEvent through StarSystem the routine.
    virtual void explode();
    // +0x1f4 module storage (RET 4); Ship overrides with module storage. Appends
    // the EquipDesc of every intact, lootable, non-cargo-pod equip.
    virtual void collect_droppable_equip(std::vector<EquipDesc>& Out);
    // +0x1f8 module storage (RET 4). At most six build_loot records' worth of
    // Descs, created as loot through StarSystem the routine.
    virtual void drop_loot(std::vector<EquipDesc> const& Descs);
    // +0x1fc .. +0x208: the four explosion passes explosion_hit (+0x184) runs --
    // shield first (+0x204; true == absorbed, the rest is skipped), then
    // equipment, arch groups and power.
    // +0x1fc is the HULL pass (CEqObj::get_explosion_dmg_bounding_sphere, then
    // damage_hull); +0x200 walks the attached equipment (CEquipTraverser 0x1ee0,
    // damage_equip) and then every arch group (damage_group).
    virtual void explosion_hit_hull(ExplosionDamageEvent const& Blast, DamageList& Out);    // +0x1fc module storage (Solar module storage)
    virtual void explosion_hit_subobjs(ExplosionDamageEvent const& Blast, DamageList& Out); // +0x200 module storage
    virtual bool explosion_hit_shield(ExplosionDamageEvent const& Blast, DamageList& Out);  // +0x204 module storage
    virtual void explosion_hit_power(ExplosionDamageEvent const& Blast, DamageList& Out);   // +0x208 module storage
    // +0x20c module storage (RET 0xc). A munition hitting the shield bubble.
    virtual void munition_hit_shield(CEShield* Shield, Archetype::Munition const* Munition,
                                     DamageList& Out);
    // +0x210 module storage (RET 0xc). Damage one equip (not a shield). Returns
    // true only when the equip was destroyed (and destroy_equip +0x224 ran):
    // `MOV AL,1` at module storage, `XOR AL,AL` on every other path (module storage);
    // Ship the routine tests it (module storage TEST AL,AL).
    virtual bool damage_equip(CEquip* Equip, float Damage, DamageList& Out);
    // +0x214 module storage (RET 0xc). Damage the shield; Damage < 0 drains it.
    // Returns true when the new hit points cross the shield's OfflineThreshold
    // (CEShield+0x74) in either direction (module storage .. 06cea9d6).
    virtual bool damage_shield(CEShield* Shield, float Damage, DamageList& Out);
    // +0x218 module storage (RET 0xc). Damage one arch group. Returns true only
    // when the group was destroyed (`MOV EAX,1` at module storage; `XOR AL,AL` /
    // `XOR EAX,EAX` elsewhere); Ship the routine tests it (module storage).
    virtual bool damage_group(CArchGroup* Group, float Damage, DamageList& Out);
    // +0x21c module storage (RET 8). Energy (power) damage; true when the new
    // power entry is <= 0 (`MOV EAX,1` at module storage).
    virtual bool damage_power(float Damage, DamageList& Out);
    // +0x220 module storage (RET 0x10).
    virtual void destroy_group(CArchGroup* Group, DamageEntry::SubObjFate Fate, DamageList& Out,
                               bool bWithChildren);
    // +0x224 module storage (RET 0xc). Destroys one equip; fates 4/5 (loot)
    // spawn it as loot, cargo pods (type 0x800) go through destroy_cargo.
    virtual void destroy_equip(CEquip* Equip, DamageEntry::SubObjFate Fate, DamageList& Out);
    // +0x228 module storage (RET 0xc).
    virtual void destroy_cargo(CEquip* Equip, DamageEntry::SubObjFate Fate, DamageList& Out);
    // +0x22c module storage (RET 8). CArchGroup::LinkedEquipmentType's equipment
    // takes LinkedEquipmentDamage * its max hit points (damage_equip); linked
    // SHIELDS (0x100) only FDUMP "Implement me!" (line 0x91b).
    virtual void damage_linked_equipment(CArchGroup* Group, DamageList& Out);
    // +0x230 module storage (RET 8). Everything mounted inside the group dies
    // with it: external equipment (0x1fe3, not lights / attached FX) through
    // destroy_equip(fate 1), child groups through destroy_group(fate 1, false).
    virtual void destroy_group_contents(CArchGroup* Group, DamageList& Out);
    // +0x234 .. +0x240 -- apply_damage_list's per-entry dispatch (module storage
    // .. 06ceb463): root, equip, power, arch group. Pushed as (Entry, List), so
    // the List is the FIRST parameter.
    virtual void apply_root_entry(DamageList const& Damage, DamageEntry const& Entry);   // +0x234 module storage
    virtual void apply_equip_entry(DamageList const& Damage, DamageEntry const& Entry);  // +0x238 module storage
    virtual void apply_power_entry(DamageList const& Damage, DamageEntry const& Entry);  // +0x23c module storage
    // Group entries are written: baseline clears Entry+8 (the fate) at module storage
    // while a death fuse burns and the cause is < 0x10.
    virtual void apply_group_entry(DamageList const& Damage, DamageEntry& Entry);        // +0x240 module storage
    // +0x244 module storage (RET 8). Appends the loot record(s) one EquipDesc
    // turns into; returns how many.
    virtual int build_loot(EquipDesc const& Desc, std::vector<FLootRecord>& Out);
    // +0x248 module storage (RET 8). One attached equip -> one loot record
    // (fresh FLID, position/orientation from the engine instance, explosive
    // impulse and a random spin); returns true.
    virtual bool build_equip_loot(CAttachedEquip const* Equip, FLootRecord& Out);
    // +0x24c module storage (RET 4). Cargo decay: every cargo stack with a count
    // loses GetDecayDamagePerSecond * Elapsed, gated on the object not being a
    // solar, CEquipManager::DecayingCargoCount > 0 and the owning player having no
    // open trade (PlayerData+0x3cc). update() calls it with the +0x6c timer.
    virtual void decay_cargo(float Elapsed);

    // (module storage, RET 4, non-virtual). One ServerEquip for the
    // equip with this id, by type; other types get none.
    void create_server_equip(unsigned short EquipId);
    // (module storage, RET 4, non-virtual). The dock-bay tick over
    // DockQueue; update()'s only callee of its kind.
    void process_dock_queue(float Dt);
    // (module storage, RET 4, non-virtual). True when a DockQueue
    // node carries this ship id. Sole caller IServerImpl::SPRequestInvincibility
    // (module storage).
    bool is_ship_queued(unsigned int ShipId) const;
    // (module storage, RET 8, non-virtual). ceqobj() through the
    // (& 0x103) guard -- the null it can yield is dereferenced -- tail-jumping
    // into CEqObj::activate (vt +0xb4, Common module storage). Sole caller
    // StarSystem::activate_object (module storage).
    ObjActivateResult activate(bool bOn, unsigned int DockIndex);
    // (module storage, RET 8, non-virtual). Appends {Port, ShipId}
    // to DockQueue and registers this object with its system
    // (StarSystem::add_to_list_d4, module storage). Sole caller
    // pub::SpaceObj::Dock (module storage).
    void queue_dock_request(int Port, unsigned int ShipId);
    // (module storage, RET 8, non-virtual). Erases the first
    // DockQueue node carrying ShipId whose port matches (any port when Port < 0),
    // then StarSystem::remove_from_list_d4 (module storage); false when no node
    // matched. Sole caller pub::SpaceObj::Dock (module storage).
    bool dequeue_dock_request(int Port, unsigned int ShipId);

    // -- EqObj's own members, ctor module storage / dtor module storage -----
    // +0x60/+0x64/+0x68 -- VC6 std::list of 0x10-byte nodes (8 + {int Port;
    // unsigned ShipId}): ships queued on this object's docking points. Drained
    // through StarSystem module storage by destroy_notify (module storage) and
    // cleared first thing in the dtor.
    struct FDockRequest
    {
        int Port;
        unsigned int ShipId;
    };
    std::list<FDockRequest> DockQueue;
    // +0x6c -- a FLOAT: update() adds Dt (module storage FLD/FADD/FST) and, past
    // 1.0f, hands it to decay_cargo and zeroes it (module storage). Ctor zeroes it.
    float CargoDecayTimer;
    // +0x70/+0x74/+0x78 -- VC6 std::list of 0x0c-byte nodes: one ServerEquip*
    // per CEquip (rebuild_server_equip / clear_server_equip).
    std::list<ServerEquip*> ServerEquips;
    // +0x7c/+0x80/+0x84 -- VC6 std::list of 0x44-byte nodes (8 + sizeof
    // ServerFuse): the fuses lit on this object (light_fuse).
    std::list<ServerFuse> Fuses;
    // +0x88 -- set by light_fuse (module storage) the first time a fuse with
    // ServerFuse::bHasDestroyRoot is lit; gates controller event 0x38.
    unsigned char bDeathFuseLit;
};

// ---------------------------------------------------------------------------
// ServerEquip -- Server.dll's per-equipment listener (RTTI .?AUServerEquip@@,
// vtable module storage: notify module storage, scalar deleting dtor
// , update module storage). One per CEquip of an EqObj, made by
// EqObj::create_server_equip (the routine) and held in EqObj+0x70. The
// constructor (module storage, RET 8) stores the equip at +4 and the owning
// EqObj at +8 and registers itself as the equip's server listener
// (CEquip::ConnectServerEquip). Five subclasses, each with its own vtable:
//   SrvTractor            module storage  new(0x0c)  notify module storage
//   SrvRepairDroid        module storage  new(0x14)  update module storage
//   SrvThruster           module storage  new(0x0c)  notify module storage
//   SrvMineDropper        module storage  new(0x18)  slots 3..5
//   SrvCounterMeasureDropper module storage new(0x18) notify + slots 3..5
//   SrvGun                module storage  new(0x18)  slots 3..7
// ---------------------------------------------------------------------------
class ServerEquip : public INotify
{
public:
    // (RET 8): Equip at +4, Owner at +8, then
    // Equip->ConnectServerEquip(this) -- no null test on the equip.
    ServerEquip(CEquip* InEquip, EqObj* InOwner);
    // (the body behind the scalar deleting dtor module storage and
    // ): CEquip::NotifyDisconnecting(this) while an equip is held.
    virtual ~ServerEquip();

    // slot 0 -- module storage (RET 8). Event 0xa (the equip is being destroyed):
    // NotifyDisconnecting(this) and forget the equip.
    void notify(Event Code, void* Param) override;
    // slot 2 -- module storage (RET 4): `Equip != 0`; Dt is not read.
    virtual bool update(float Dt);

    CEquip* Equip;        // +0x04
    EqObj* Owner;         // +0x08
};

// notify module storage (RET 8).
class SrvThruster : public ServerEquip
{
public:
    SrvThruster(CEquip* InEquip, EqObj* InOwner);
    void notify(Event Code, void* Param) override;
};

// notify module storage (StarSystem lane's range, W5-C).
class SrvTractor : public ServerEquip
{
public:
    SrvTractor(CEquip* InEquip, EqObj* InOwner);
    void notify(Event Code, void* Param) override;
};

// update module storage (RET 4), target pick module storage.
class SrvRepairDroid : public ServerEquip
{
public:
    SrvRepairDroid(CEquip* InEquip, EqObj* InOwner);
    bool update(float Dt) override;

    // (RET 0, non-virtual): the most damaged attached, non-shield
    // equip or arch group of the droid's owner (strictly below 1.0 relative
    // health); SUBOBJ_ID_NONE when everything is whole.
    unsigned short pick_target() const;

    float RepairTimer;             // +0x0c  the factory stores 0 (module storage)
    unsigned short TargetId;       // +0x10  SUBOBJ_ID_NONE from the factory (module storage)
};

// The three launcher wrappers share +0x0c/+0x10 and three more slots.
class SrvLauncher : public ServerEquip
{
public:
    SrvLauncher(CEquip* InEquip, EqObj* InOwner);

    // slot 3 (+0x0c): CELauncher::Fire(Target) must answer 9, then the shot is
    // paid for (ConsumeFireResources), counted and launched. Mine module storage,
    // countermeasure module storage, gun module storage.
    virtual bool fire(Vector const& Target) = 0;
    // slot 4 (+0x10): fire straight along the barrels -- slot 3 at
    // GetAvgBarrelPosWS + GetAvgBarrelDirWS * (1e10 / |dir|). Mine and
    // countermeasure share module storage (the argument is not read); the gun's
    // also announces the shot to the system.
    virtual bool fire_forward(int SourceClient);
    // slot 5 (+0x14): the shot without CELauncher::Fire -- use_ammo
    // (the routine) and, when it succeeds, the launch. Mine module storage,
    // countermeasure module storage, gun module storage.
    virtual bool fire_unchecked(Vector const& Target) = 0;

    // (RET 4, non-virtual; the Vector is not read). Refuses a
    // destroyed equip or owner; bumps FireCount; outside single player needs
    // CELauncher::AmmoNeedsMet and calls ConsumeFireResources; and when the
    // owner is a player's current ship and the projectile needs ammo, removes one
    // round from that player's cargo (PlayerData_RemoveCargo).
    bool use_ammo(Vector const& Target);

    int FireCount;                 // +0x0c  the factory stores 0
    // +0x10 -- a DOUBLE: Timing::read of the last damage, written by
    // EqObj::apply_equip_entry (module storage) and read by EqObj::update's
    // weapons-offline repair (module storage). The factory never writes it
    // (operator new garbage in baseline); the port zeroes it.
    double LastDamageTime;
};

// (RTTI .?AUSrvMineDropper@@).
class SrvMineDropper : public SrvLauncher
{
public:
    SrvMineDropper(CEquip* InEquip, EqObj* InOwner);
    bool fire(Vector const& Target) override;
    bool fire_unchecked(Vector const& Target) override;
    // (RET 4): one StarSystem::create_mine per launch record.
    void launch(Vector const& Target);
};

// (RTTI .?AUSrvCounterMeasureDropper@@).
class SrvCounterMeasureDropper : public SrvLauncher
{
public:
    SrvCounterMeasureDropper(CEquip* InEquip, EqObj* InOwner);
    // (RET 8): event 0x19 on an AI-owned dropper fires it through
    // the owner's IObjAffect slot +0x34; event 0xa as the base.
    void notify(Event Code, void* Param) override;
    bool fire(Vector const& Target) override;
    bool fire_unchecked(Vector const& Target) override;
    // (RET 4): one StarSystem::create_counter_measure per record.
    void launch(Vector const& Target);
};

// (RTTI .?AUSrvGun@@), eight slots.
class SrvGun : public SrvLauncher
{
public:
    SrvGun(CEquip* InEquip, EqObj* InOwner);
    bool fire(Vector const& Target) override;
    bool fire_forward(int SourceClient) override;
    bool fire_unchecked(Vector const& Target) override;
    // slot 6 (+0x18) module storage (module storage): one StarSystem::create_bullet
    // per launch record. fire (module storage) calls it for an unguided munition
    // outside single player.
    virtual void launch_bullets(Vector const& Target);
    // slot 7 (+0x1c) module storage (module storage): one StarSystem::create_guided
    // per record (with the lock target); fire / fire_unchecked call it when
    // Munition::Seeker (+0x90) is non-zero (module storage / module storage).
    virtual void launch_guided(Vector const& Target);
};

// The launch records the dropper/gun wrappers hand to StarSystem. Built on the
// stack by module storage (mines), module storage (countermeasures) and
// (guided); read by the routine / the routine / the routine.
struct FServerLaunchRecord
{
    unsigned int OwnerPlayer;                // +0x00  owner's CSimple+0xb4
    unsigned int OwnerId;                    // +0x04  owner's get_id()
    unsigned short EquipId;                  // +0x08
    unsigned short Pad0a;
    Archetype::Equipment const* Arch;        // +0x0c  Mine / CounterMeasure / Munition
    Vector Position;                         // +0x10  ProjLaunchInfo+0x00
    Matrix Orientation;                      // +0x1c  ProjLaunchInfo+0x0c
    Vector Velocity;                         // +0x40  ProjLaunchInfo+0x30
    unsigned int TargetId;                   // +0x4c  guided only: LockTarget's CSimple+0xb0, or 0
    unsigned short TargetSubId;              // +0x50  guided only: ProjLaunchInfo+0x40
    unsigned short Pad52;
};

// SrvGun::launch_bullets' record (module storage .. 06cee2b2), for the routine.
struct FServerBulletRecord
{
    unsigned int OwnerId;                    // +0x00
    Archetype::Munition const* Arch;         // +0x04
    unsigned short EquipId;                  // +0x08
    unsigned short Pad0a;
    Vector Position;                         // +0x0c
    Matrix Orientation;                      // +0x18
    Vector Velocity;                         // +0x3c
};

// ---------------------------------------------------------------------------
// Ship -- module storage, operator new(0xc8). create() = CObject::Alloc(0x503).
// ---------------------------------------------------------------------------
class Ship : public EqObj
{
public:
    Ship();
    virtual ~Ship();

    bool create(Archetype::Root* Arch) override;
    bool find(unsigned int const& Id) override;

    // (module storage) — Ship vtable +0x16c.
    bool update(float Dt) override;

    // Ship's override of IObjInspect::get_data (main
    // vtable slot 42, +0xA8): `*OutData = [this+0xb4]; return 0` (RET 0x4).
    // The base IObjInspectImpl::get_data returns null and -3.
    int get_data(void const*& OutData) const override;

    // -- GameObject / EqObj slots Ship overrides (vtable module storage) ----
    // +0x170 module storage (RET 8): GameObject::enter_system (the routine,
    // called directly), then DeathSequenceObjId = 0.
    void enter_system(StarSystem* Sys, PhySys::CreateParms const* Parms) override;
    // +0x188 module storage (RET 4): a cause-6 list on a ship nobody owns
    // first drops out of cruise (IObjAffect slot_50(false, true, 0)), then
    // EqObj::apply_damage_list (the routine, direct).
    void apply_damage_list(DamageList& Damage) override;
    // +0x1e4 module storage (RET 0x14): EqObj::light_fuse (the routine,
    // direct); a lit DEATH_COMM_FUSE is then told to the behaviors of this
    // ship's formation followers (CShip +0x1d4) and follow followers (+0x1f8).
    bool light_fuse(unsigned int SourceId, ID_String const& FuseId,
                    unsigned short SubObjId, float AtT, float Lifetime) override;
    // +0x1f4 module storage (RET 4): EqObj::collect_droppable_equip
    // (the routine, direct), then every EquipDesc still in the jettison queue.
    void collect_droppable_equip(std::vector<EquipDesc>& Out) override;

    // -- IObjAffect overrides (Ship's table module storage, ctor
    // ). Slots 13, 15, 16, 17, 19 and 22 are EqObj's bodies
    //    (module storage / 06ce6d80 / 06ce6ba0 / 06ce6f60 / 06ce70b0 /
    // ) and slot 27 is GameObject's; everything else is here. ----
    int set_throttle(float Throttle) override;                                   //  0 module storage
    int set_axis_throttle(Vector const& AxisThrottle) override;                  //  1 module storage
    int set_nudge_vec(Vector const& NudgeVec) override;                          //  2 module storage
    int set_strafe_dir(StrafeDir Dir) override;                                  //  3 module storage
    int set_formation_leader(IObjRW* Leader) override;                           //  4 module storage
    int set_formation_offset(Vector const& Offset) override;                     //  5 module storage
    int add_formation_follower(IObjRW* Follower) override;                       //  6 module storage
    int remove_formation_follower(IObjRW* Follower) override;                    //  7 module storage
    int get_formation_follower_count() override;                                 //  8 module storage
    int set_follow_leader(IObjRW* Leader) override;                              //  9 module storage
    int set_follow_offset(Vector const& Offset) override;                        // 10 module storage
    int add_follow_follower(IObjRW* Follower) override;                          // 11 module storage
    int remove_follow_follower(IObjRW* Follower) override;                       // 12 module storage
    int slot_38(unsigned short EquipId, int Count, int SourceClient) override;   // 14 module storage
    int set_target(IObjRW* Target, unsigned short SubTarget, int SourceClient) override; // 18 module storage
    int slot_50(bool bEngage, bool bArg2, int SourceClient) override;            // 20 module storage
    int slot_54(bool bActivate, int SourceClient) override;                      // 21 module storage
    int slot_5c(int RequestType, unsigned const* TargetId, unsigned const* SecondId,
                int bFlag, RequestCB* Callback) override;                        // 23 module storage
    int slot_60(int RequestType, unsigned const* TargetId, int Arg3) override;   // 24 module storage
    int go_tradelane(IObjInspect const* EntranceRing, IObjInspect const* ExitRing,
                     bool bFastExit, float Speed) override;                      // 25 module storage
    int stop_tradelane() override;                                               // 26 module storage
    int slot_70(unsigned Id) override;                                          // 28 module storage
    int slot_74(unsigned Arg) override;                                          // 29 module storage

    // (module storage, RET 0xc) -- the answer to the
    // request slot_5c raised. Callers: pub::SpaceObj's three response exports
    // (module storage / module storage / module storage), which pass
    // RequestType 0, 1 and 2 -- the same kinds slot_5c raises controller
    // events 0x12 / 0x19 / 0x14 for. With a PendingRequest callback its
    // OnResult gets the three arguments and a false answer clears the request;
    // without one the owner's client is told through IClient +0x14c (SendServerRequestReturned,
    // REQUEST_RETURNED) and Field_0x98 is re-armed for (0, 3).
    void request_returned(int RequestType, int Response, int Arg3);

    CShip* cship() const;

    // Ship::allocate_equip_id -- baseline the routine (module storage), a plain
    // non-virtual thiscall with no stack arguments. The name is baseline's own:
    // the failure path FDUMPs "Ship::allocate_equip_id (): Playe..." from
    // Server\... line 0x1e5 (module storage).
    // MOV EAX,[ECX + 0x10] / the (ObjectClass & 3) == 3 guard
    // MOV EDX,[EAX + 0xb4]        ; CSimple::OwnerPlayer
    // JNZ -> the FDUMP, then return *SUBOBJ_ID_NONE
    // ADD ECX,0xc4
    // JMP [->COMMON.DLL::SubObjectID::EquipIdMaker::CreateEquipID]
    // i.e. an object that belongs to a PLAYER must take its ids from that
    // player's own PlayerData::equip_id_maker (+0x3C8) instead; only unowned
    // objects mint them here. StarSystem::add_cargo is the single caller.
    unsigned short allocate_equip_id();

    // The 8-byte record the routine reads: a word (the EquipIdMaker state)
    // at +0 and a dword at +4. Its only builder is StarSystem the routine,
    // which fills a stack copy from its create request just before the call:
    // MOV DX,word ptr [EBP + 0xe0]  -> [ESP+0x24]
    // MOV EAX,dword ptr [EBP + 0xe4] -> [ESP+0x28]
    // LEA EAX,[ESP + 0x24] / PUSH EAX / CALL the routine
    struct FEquipState
    {
        SubObjectID::EquipIdMaker EquipIds;   // +0x00
        void const* Data;                     // +0x04  -> Field_0xB4
    };

    // (module storage, RET 4). A DIRECT call of EqObj's
    // +0x1bc body the routine (check_fuses; Ship does not override that
    // slot), then the two fields of State. Only caller: the routine
    // (module storage), right after enter_system (+0x170).
    void set_equip_state(FEquipState const& State);

    // (module storage, RET 0). The one-shot
    // invincibility grant behind the Field_0xB8 latch (see that member).
    // Callers: module storage (after a player ship is built) and
    // StarSystem the routine (module storage, after CShip::launch).
    void apply_launch_invincibility();

    // (module storage, RET 4) -- the zone-damage tick
    // Ship::update runs last on the live path (module storage).
    void tick_zone_damage(float Dt);

    // (module storage, RET 8) -- spreads a damage ratio
    // over hull, equipment and arch groups; true when any of them reported
    // destruction. Only caller: tick_zone_damage (module storage).
    bool apply_damage_ratio(float Ratio, DamageList& Damage);

    // (module storage, RET 4) -- the jettison tick
    // Ship::update runs first on the live path (module storage). Owns
    // Jettison/JettisonTimer/PendingBayCloses.
    void tick_jettison(float Dt);

    // (module storage, RET 0xc) -- where a jettisoned item
    // leaves the ship: the bay hardpoints' transform and the direction from
    // bay surface to bay exterior, or the ship's own transform and its -Y
    // axis without them; the direction is scaled to a randomised
    // JETTISONED_CARGO_VELOCITY and the ship's velocity is added. Only caller:
    // tick_jettison (module storage).
    void compute_jettison_launch(Vector* OutPos, Matrix* OutOrient, Vector* OutVel);

    // -----------------------------------------------------------------
    // Ship's OWN block, +0x90 .. +0xC7. Decoded 2026-09-17 (wave 9) off the
    // constructor at module storage, which writes every one of these in order
    // and nothing else; the EqObj constructor it chains to (module storage)
    // stops at `MOV byte ptr [ESI + 0x88],BL` (module storage), so the base
    // really does end at 0x8C/0x90 and this block really is Ship's.

    // MOV  [ESI + 0x90],EBX(0)
    // MOV  [ESI + 0x94],EBX
    // MOV  byte [ESI + 0x98],BL
    // MOV  [ESI + 0x9c],EBX
    // MOV  byte [ESI + 0xa0],AL   <- see the CAUTION below
    // PUSH 0x28 / operator new; [EAX]=EAX; [EAX+4]=EAX
    // MOV  [ESI + 0xa4],EAX       ; _Head of a std::list
    // MOV  [ESI + 0xa8],EBX       ; its _Size
    // MOV  [ESI + 0xac],EBX
    // MOV  [ESI + 0xb0],EBX
    //   (+0xb4 is NEVER written by the constructor)
    // MOV  byte [ESI + 0xb8],BL
    // MOV  [ESI + 0xbc],EBX
    // MOV  [ESI + 0xc0],EBX
    // /module storage  LEA ECX,[ESI + 0xc4] + a direct call to
    //                   COMMON.DLL::SubObjectID::EquipIdMaker::EquipIdMaker
    // 0xC4 + 4 = 0xC8 = exactly what module storage's operator new asks for.

    // CAUTION on +0xA0: `MOV AL,[ESP + 0x0b]` at module storage reads an
    // UNINITIALISED local, not an argument (Ship::Ship takes none). It is
    // MSVC6's 1-byte std::allocator member, copy-initialised from an
    // `allocator()` temporary -- the same artefact as Observer+0x08.
    // Do not read it as data. The triple {+0xA0, +0xA4, +0xA8} is one
    // std::list whose nodes are 0x28 bytes (0x08 header + a 0x20 value).

    // DECODED 2026-09-17 (wave 13). The 2026-09-17 (wave 9) note said "what
    // each field MEANS is still open"; it is not any more, and FOUR of them
    // were typed wrong -- +0x94, +0xAC, +0xBC and +0xC0 are FLOATS, read with
    // FLD/FST/FSUB, not `unsigned int`. +0x9C is a pointer, not an int.

    // The readers, in order of how much they settle:

    //  * Ship::update, baseline's vtable slot at module storage ->
    // . `if (Field_0x90 == 0)` is the live path -- it runs
    //    CObject::update (module storage, with CObject::NO_SCANNER for an
    //    unowned ship and CObject::UPDATE_DUMB for a player's outside single
    //    player), then the jettison tick and the damage tick. When +0x90 is
    //    non-zero the ship is running its DEATH SEQUENCE instead:
    // single player: wait for CShip+0x214, then
    // the routine and +0x90 = 0
    // multiplayer: bail while CShip+0x2bd is set,
    // FLD [ESI+0x94] / FSUB dt / FST [ESI+0x94], and when
    //                      the result is <= 0.0 re-enable collisions
    //                      (module storage), the routine, +0x94 = 0, +0x90 = 0.
    //  * the system-level destroy handler at module storage is what ARMS it:
    //    `MOV [EBP+0x90],EAX` right after `CALL [EDX+0x20]`, i.e. the id of the
    //    object whose destruction started the sequence, and then
    // / module storage store the delay --
    // == 0.0f normally, module storage == 0x41400000 ==
    //    12.0f when get_type_bits() has bit 0x40 or bit 0x800 set.
    //  * (module storage), reached from module storage /
    // / module storage with a leading 0, 1 and 2, owns +0x98
    //    and +0x9C: when +0x9C is null it answers the client directly through
    //    IClient +0x14c at module storage (SendServerRequestReturned, ch2 op 0x44
    //    FLPACKET_SERVER_REQUEST_RETURNED)
    //    and sets +0x98 to (kind == 0 && status == 3); when +0x9C is set it
    //    calls THAT object's vtable slot 0 with the same three arguments and
    //    clears both on a false answer (module storage / module storage).
    // reads +0x98 back.
    //  * (module storage), the jettison tick, owns +0xA0..+0xB0:
    //    +0xAC counts down by dt and is re-armed from
    //    COMMON.DLL::JETTISON_CLEARANCE_TIME after every ejection, +0xB0 is how
    //    many CShip::close_bay calls are still owed and is drained one per tick
    //    once the queue empties (module storage).
    //  * (module storage), the damage tick, owns +0xBC/+0xC0:
    //    +0xBC accumulates `fuse rate * dt` (module storage FADD /
    // FSTP) and +0xC0 accumulates dt (module storage FADD /
    // FST). Once +0xBC is non-zero and +0xC0 > 1.0 the
    //    accumulated damage is divided by the hull's own vtable +0xd4 value and,
    //    above 1%, applied through the routine; +0xBC is then zeroed
    //    (module storage). Nothing in the module ever resets +0xC0.

    // These offsets are BASELINE's. This port's GameObject base is not
    // byte-compatible with baseline's, so the members below reproduce the ORDER
    // and the types, not the absolute addresses. Names for what baseline leaves
    // anonymous are the port's own, as bShipGone is in server_sysdb.h.
    unsigned int DeathSequenceObjId;      // +0x90  an object id; 0 == the live path
    float DeathSequenceTimer;             // +0x94  seconds; armed to 12.0f or 0.0f
    // +0x98 -- deliberately left anonymous. the routine sets it to
    // `(param_1 == 0 && param_2 == 3)`:
    // 85ff          TEST EDI,EDI          ; EDI == param_1
    // c68698...00   MOV  byte [ESI+0x98],0
    // 750c          JNZ  the branch
    // 83fd03        CMP  EBP,0x3          ; EBP == param_2
    // 7507          JNZ  the branch
    // c68698...01   MOV  byte [ESI+0x98],1
    // and module storage reads it back. So the CONDITION is known but not what
    // the two literals mean. A Field_0xNN with a confirmed primitive type is
    // what this port allows in that situation; do not name it on a guess.
    unsigned char Field_0x98;
    IObjAffect::RequestCB* PendingRequest; // +0x9C  slot_5c stores it; request_returned calls its OnResult (vtable slot 0)
    // +0xA0..+0xA8 -- the jettison queue, a COMMON.DLL EquipDescList (a VC6
    // std::list<EquipDesc>: allocator +0xA0, head +0xA4, size +0xA8, 0x28-byte nodes
    // with the EquipDesc at +0x08). The type is proved by the unwind funclets:
    // Unwind@06d60ecb (Ship::Ship, ECX = this + 0xa0) and Unwind@06d60ef8
    // (~Ship, ECX = CBase + 0x94 = this + 0xa0) both JMP to the import
    // COMMON.DLL::EquipDescList::~EquipDescList (module storage). Ship::Ship builds it
    // inline (module storage..06d00ee2: operator new(0x28) head, size 0) before the
    // EquipIdMaker at +0xC4; ~Ship destroys it after its body (module storage..06d01016)
    // and before ~EqObj -- a member destroyed by the compiler, as here.
    EquipDescList Jettison;
    float JettisonTimer;                  // +0xAC  JETTISON_CLEARANCE_TIME countdown
    int PendingBayCloses;                 // +0xB0
    // +0xB4 -- the constructor leaves it alone; the routine (module storage)
    // is the only writer, copying dword 1 of the same two-field record whose
    // word 0 seeds EquipIds (module storage). Its only reader is get_data
    // (module storage, main vtable slot 42 +0xA8), which hands it out as the
    // IObjInspect `void const*` data pointer -- hence the type.
    void const* Field_0xB4;
    // +0xB8 -- a ONE-SHOT latch, not a toggle. the routine (module storage,
    // called from module storage and module storage) is its whole story:
    // 8a81b8000000  MOV  AL,byte ptr [ECX + 0xb8]
    // 752c          JNZ  the branch      ; already done
    // b801000000    MOV  EAX,0x1
    // 8881b8000000  MOV  byte ptr [ECX + 0xb8],AL
    // ff9094010000  CALL dword ptr [EAX + 0x194]
    // i.e. "fire the +0x194 slot exactly once per ship", with a 0x0C-byte
    // FInvincibility built on the stack: dword 1 (module storage), byte 1
    // (module storage), byte 1 (module storage), float 0.0f (module storage)
    // -- not {1, 0, 0} as this note used to say. ECX is the Ship BASE: the
    // vtable it calls through is `[ECX]`, at +0x00.
    unsigned char Field_0xB8;
    float AccumulatedDamage;              // +0xBC
    float AccumulatedDamageTime;          // +0xC0

    // baseline Ship+0xC4, the last dword of the 0xC8 bytes Ship::Ship allocates
    // (module storage, `operator new(200)`).
    SubObjectID::EquipIdMaker EquipIds;   // +0xC4
};

// ---------------------------------------------------------------------------
// Solar -- create() = CObject::Alloc(0x303), module storage.
// ---------------------------------------------------------------------------
class Solar : public EqObj
{
public:
    Solar();
    // (the body; module storage is the deleting destructor):
    // re-stamps Solar's three vtables, calls Solar's own +0x15c slot
    // (`CALL [module storage]` = EqObj::destroy_notify, module storage) on
    // this, then ~EqObj (the routine).
    virtual ~Solar();

    bool create(Archetype::Root* Arch) override;                                  // +0x164 module storage
    bool find(unsigned int const& Id) override;                                   // +0x168 module storage `XOR AL,AL / RET 4`

    // (module storage, RET 4; the name is the port's). Wraps a
    // CObject that already exists: CBase::Object = Obj, CObject::AddRef(Obj),
    // CBase::advise(true), return Object != 0. Only caller: StarSystem::load's
    // single-player walk the routine (module storage).
    bool attach_existing(CObject* Obj);

    // -- Solar's overrides in its main vtable module storage ---------------
    bool update(float Dt) override;                                              // +0x16c module storage
    bool destroy(int Kind, unsigned int ObjId) override;                         // +0x158 module storage
    void damage_sub_obj(FSubObjDamage const& Hit, DamageList& Damage) override;  // +0x17c module storage
    void munition_hit(FMunitionImpact const& Hit, DamageList& Damage) override;  // +0x180 module storage
    void explosion_hit_hull(ExplosionDamageEvent const& Blast, DamageList& Out) override; // +0x1fc module storage

    // +0x250 module storage (RET 0xc) -- Solar's own slot, the only one past
    // EqObj's +0x24c. The name is baseline's: its error path FDUMPs "Invalid type
    // in ChooseSystemArrivalLocation()" from Server\Space\
    // Solar.cpp line 0xeb (module storage). Callers: SystemSwitchOutComplete
    // (module storage) and pub::SpaceObj::JumpIn (module storage), both on the
    // arrival solar, both pushing the arriving object first -- which this body
    // never reads.
    virtual void ChooseSystemArrivalLocation(GameObject* Arriving, Vector& OutPos, Matrix& OutOrient);

    CSolar* csolar() const;
};

// ---------------------------------------------------------------------------
// Loot -- create() = CObject::Alloc(0x803), module storage: narrows the fresh
// CObject with (ObjectClass & 0x803) == 0x803 only for the open() call, then
// CBase::advise(true), Archetype::Root::create_instance, NeuronRuntime::Engine +0x6c,
// and returns get_index() != -1 (see Loot::create for the two port gaps).
// ---------------------------------------------------------------------------
class Loot : public GameObject
{
public:
    Loot();
    virtual ~Loot();

    bool create(Archetype::Root* Arch) override;
    bool find(unsigned int const& Id) override;

    // +0x154 override, module storage (Loot vtable module storage slot 85 ->
    // ): `MOV EAX,0x10000000 / RET` -- a constant, NOT the base
    // body's cobj()->TypeBits.
    unsigned int get_type_bits() const override;

    // Loot's own overrides (vtable module storage): +0x158 the routine, +0x16c
    // , +0x188 the routine. Bodies in src/Server/server_loot.cpp.
    bool destroy(int Kind, unsigned int ObjId) override;
    bool update(float Dt) override;
    void apply_damage_list(DamageList& Damage) override;

    // (module storage, RET 4) -- commodity decay, called only by
    // update (module storage). False when the last unit decayed and the loot was
    // destroyed, true otherwise; the caller ignores it.
    bool update_decay(float Dt);
    // (module storage, RET 0) -- the archetype's explosion_arch as a
    // cause-4 blast through StarSystem::explode; destroy(1, ...) only.
    void explode_on_destroy();
    // (module storage, RET 0) -- a CargoPod whose contents exceed
    // UnitsPerContainer splits into at most five new loot records, spawned
    // through StarSystem::create_loot (mode 2); destroy(1, ...) only.
    void split_stack();

    CLoot* cloot() const;

    // The three dwords the baseline spawner's inlined constructor zeroes after
    // GameObject's (module storage-06d08bb9); operator new(0x70) at module storage.
    float CheckTimer;   // +0x60  seconds since the last unseen-radius scan (every 3 s)
    float UnseenLife;   // +0x64  counts down; reset to LOOT_UNSEEN_LIFE_TIME near a player
    float DecayAccum;   // +0x68  decayed hit points not yet taken off a unit
};

// ---------------------------------------------------------------------------
// Projectile -- RTTI ".?AUProjectile@@" (module storage). The abstract base of
// Mine, Munition/Guided, CounterMeasure and Bullet (their base arrays at
// / 06d6c73c list it right after the leaf). It adds nothing:
// its own vtables (module storage / 06d66254 / 06d6624c, stored by the inlined
// constructor in every leaf's, e.g. module storage-06cf3361) are GameObject's
// slot for slot (create/find still _purecall module storage), and every leaf
// destructor goes straight into GameObject's (the routine, module storage).
// ---------------------------------------------------------------------------
class Projectile : public GameObject
{
};

// ---------------------------------------------------------------------------
// Mine -- RTTI ".?AUMine@@" (module storage), operator new(0xa0) in the system's
// mine spawner (module storage). Base array module storage: Projectile (and its
// GameObject chain), PhySys::Controller at +0x60 (COL offset 0x60, vtable
// ) and PhySys::CObjectCB at +0x70 (COL module storage, offset
// 0x70, vtable module storage = {the routine, the routine}).
// Bodies in src/Server/server_mine.cpp. Primary vtable module storage; against
// GameObject's it overrides +0x154 (06cf3400), +0x158 (06cf3870), +0x164
// (06cf34e0), +0x168 (06d02cb0), +0x16c (06cf36a0), +0x17c (06cf4230) and +0x180
// (06cf4240) -- nothing else.
// ---------------------------------------------------------------------------
class Mine : public Projectile, public PhySys::Controller, public PhySys::CObjectCB
{
public:
    // (module storage).
    Mine();
    // (module storage), entered on the CBase sub-object; the routine /
    // are its deleting forms.
    virtual ~Mine();

    unsigned int get_type_bits() const override;
    bool destroy(int Kind, unsigned int ObjId) override;
    bool create(Archetype::Root* Arch) override;
    bool find(unsigned int const& Id) override;
    bool update(float Dt) override;
    void damage_sub_obj(FSubObjDamage const& Hit, DamageList& Damage) override;
    void munition_hit(FMunitionImpact const& Hit, DamageList& Damage) override;

    // PhySys::CObjectCB (+0x70): the routine / the routine.
    void added(CObject* Obj) override;
    void deleted(CObject* Obj) override;

    // (module storage, RET 0). The proximity phantom and, for a seeking
    // mine, the controller registration. Only caller: the spawner, right after
    // enter_system (module storage). The name is the port's.
    void arm();

    // CMine narrowing: (ObjectClass & 0xf) == 0xf, as module storage-06cf36d2.
    CMine* cmine() const;

protected:
    // PhySys::Controller slot 4, the branch (RET 8): accelerate toward the target.
    void do_simulation_controller(IVP_Event_Sim* EventSim,
                                  IVP_U_Vector<IVP_Core>* CoreList) override;

public:
    void* Phantom;                  // +0x74  PhySys phantom handle (CreatePhantom)
    unsigned int IntruderCount;     // +0x78  objects inside the phantom (Slot00 ++ / Slot04 --)
    unsigned int Field_0x7c;        // +0x7c  never written or read by Mine's bodies
    bool bSeeking;                  // +0x80  update sets it while a target is in range
    Vector Target;                  // +0x84  closest intruder position (GetClosestPhantomIntruderPos)
    float SeekTimer;                // +0x90  constructor FLT_MAX; update adds Dt, zeroed past 1.0
    bool bTriggered;                // +0x94  Slot00 latch: a hostile ship touched the phantom
    unsigned int TriggerObjId;      // +0x98  that ship's id (CSimple+0xb0)
    unsigned int Field_0x9c;        // +0x9c  never written or read by Mine's bodies
};

// ---------------------------------------------------------------------------
// Munition -- RTTI ".?AUMunition@@" (module storage). The shared base of Guided and
// Bullet; its base array (module storage) is Munition, Projectile and GameObject's
// chain. Like Projectile it adds nothing: its own vtables (module storage /
// / 06d66720, stored by Guided's constructor at module storage-06cf3d11)
// end at +0x1b8 (module storage) with GameObject's bodies and _purecall for
// create/find. Guided and Bullet each add their OWN slot +0x1bc, with different
// arities (Guided RET 0, Bullet RET 8), so it is not a Munition virtual.
// ---------------------------------------------------------------------------
class Munition : public Projectile
{
};

// ---------------------------------------------------------------------------
// Guided -- RTTI ".?AUGuided@@" (module storage), a guided missile; operator new(0x80)
// in the system's missile spawner (module storage) and constructed by the routine.
// Base array module storage: Guided, Munition, Projectile, GameObject's chain, then
// PhySys::Controller at +0x60 (vtable module storage) and PhySys::CObjectCB at +0x70
// (vtable module storage = {the branch, module storage `RET 4`}).
// Primary vtable module storage; against GameObject's it overrides +0x154
// (06cf3d70), +0x158 (06cf4450), +0x164 (06cf3e50), +0x168 (06cf3ed0), +0x16c
// (06cf3f10), +0x17c (06cf4230), +0x180 (06cf4240), +0x1a4 (06cf4270) and adds
// +0x1bc (06cf4310); IObjAffect table module storage overrides +0x48 set_target
// (06cf4c10). Bodies in src/Server/server_guided.cpp.
// ---------------------------------------------------------------------------
class Guided : public Munition, public PhySys::Controller, public PhySys::CObjectCB
{
public:
    // (module storage).
    Guided();
    // (module storage), entered on the CBase sub-object; the routine /
    // are its deleting forms.
    virtual ~Guided();

    unsigned int get_type_bits() const override;
    bool destroy(int Kind, unsigned int ObjId) override;
    bool create(Archetype::Root* Arch) override;
    bool find(unsigned int const& Id) override;
    bool update(float Dt) override;
    void damage_sub_obj(FSubObjDamage const& Hit, DamageList& Damage) override;
    void munition_hit(FMunitionImpact const& Hit, DamageList& Damage) override;
    void on_collision(FCollisionEvent const& Hit) override;

    // +0x1bc, the branch (RET 0). Registers the controller and builds the proximity
    // phantom. Only caller: the missile spawner, right after enter_system
    // (module storage `CALL [EAX+0x1bc]`). The name is the port's.
    virtual void arm();

    // IObjAffect +0x48, the branch (RET 0xc).
    int set_target(IObjRW* Target, unsigned short SubTarget, int SourceClient) override;

    // PhySys::CObjectCB (+0x70): the branch / module storage (`RET 4`).
    void added(CObject* Obj) override;
    void deleted(CObject* Obj) override;

    // CGuided narrowing: (ObjectClass & 0xa7) == 0xa7, as module storage-06cf3f41.
    CGuided* cguided() const;

protected:
    // PhySys::Controller slot 4, the branch (RET 8): the motor's thrust.
    void do_simulation_controller(IVP_Event_Sim* EventSim,
                                  IVP_U_Vector<IVP_Core>* CoreList) override;

    // (module storage, RET 8): turn toward the target (proportional
    // navigation on the line of sight) and keep the velocity on the nose. Only caller:
    // update (module storage). The name is the port's.
    void seek(Vector const& TargetPos, Vector const& TargetVel);

public:
    void* Phantom;                  // +0x74  PhySys phantom handle (CreatePhantom)
    bool bTriggered;                // +0x78  Slot00 latch: something touched the phantom
    unsigned int TriggerObjId;      // +0x7c  its root's id (0 for a non-CSimple root)
};

// ---------------------------------------------------------------------------
// Bullet -- RTTI ".?AUBullet@@" (module storage), a gun round (CBeam, class 0x67).
// operator new(0x60) in the system's bullet spawner the routine (module storage),
// which inlines the constructor: GameObject's, then the three vtables
// (module storage-06d09a2b); no field of its own. Base array module storage:
// Bullet, Munition, Projectile, GameObject's chain. Primary vtable module storage;
// against GameObject's it overrides +0x154 (module storage), +0x164 (06cf4d40), +0x168
// (06d02cb0), +0x16c (06cf4dc0), +0x17c/+0x180/+0x184 (module storage `RET 8`), +0x1a4
// (06cf4e40) and adds +0x1bc (module storage `RET 8`). Its CBase slot 0 is the
// shared the routine. Bodies in src/Server/server_guided.cpp.
// ---------------------------------------------------------------------------
class Bullet : public Munition
{
public:
    unsigned int get_type_bits() const override;
    bool create(Archetype::Root* Arch) override;
    bool find(unsigned int const& Id) override;
    bool update(float Dt) override;
    void damage_sub_obj(FSubObjDamage const& Hit, DamageList& Damage) override;
    void munition_hit(FMunitionImpact const& Hit, DamageList& Damage) override;
    void explosion_hit(ExplosionDamageEvent const& Blast, DamageList& Damage) override;
    void on_collision(FCollisionEvent const& Hit) override;

    // +0x1bc, module storage `RET 8`: Bullet's own two-argument slot, an empty body. No
    // call through a Bullet's +0x1bc exists in Server.dll; name and argument types are
    // not recoverable from the empty body.
    virtual void Slot1bc(unsigned int Arg1, unsigned int Arg2);

    // CBeam narrowing: (ObjectClass & 0x67) == 0x67, as module storage-06cf4d87.
    CBeam* cbeam() const;
};

// ---------------------------------------------------------------------------
// CounterMeasure -- RTTI ".?AUCounterMeasure@@" (module storage), a dropped decoy
// (CCounterMeasure, class 0x17). operator new(0x60) in the system's spawner
// (module storage .. 06d09310), constructed by the routine: GameObject's
// constructor, then the three vtables (module storage .. 06ce6185); no field of its
// own. Base array (COL at module storage): CounterMeasure, Projectile, GameObject's
// chain. Primary vtable module storage (ends at +0x1b8); against GameObject's it
// overrides +0x154 (06ce61f0), +0x158 (06ce63f0), +0x164 (06ce6200), +0x168
// (06d02cb0, Mine's), +0x16c (06ce6280) and puts module storage in BOTH +0x17c and
// +0x180. Its IObjAffect table (module storage) is Bullet's entry for entry
// (GameObject's bodies) and its CBase slot 0 is the shared the routine.
// Bodies in src/Server/server_countermeasure.cpp.
// ---------------------------------------------------------------------------
class CounterMeasure : public Projectile
{
public:
    // (module storage, RET 0).
    CounterMeasure();

    unsigned int get_type_bits() const override;
    bool destroy(int Kind, unsigned int ObjId) override;
    bool create(Archetype::Root* Arch) override;
    bool find(unsigned int const& Id) override;
    bool update(float Dt) override;
    void damage_sub_obj(FSubObjDamage const& Hit, DamageList& Damage) override;
    void munition_hit(FMunitionImpact const& Hit, DamageList& Damage) override;

    // (module storage, RET 0). The decoy pass: every guided missile
    // of another owner inside the archetype's Range may be retargeted onto this
    // countermeasure (DiversionPctg). Only caller: the spawner, right after
    // enter_system (module storage). The name is the port's.
    void divert_missiles();

    // CCounterMeasure narrowing: (ObjectClass & 0x17) == 0x17, as module storage-06ce62a7.
    CCounterMeasure* ccountermeasure() const;
};

// ---------------------------------------------------------------------------
// SrvAsteroid::MedAsteroid / SrvAsteroid::MedMine -- the server objects that
// stand in for a live asteroid / mine of an active asteroid-field cube. RTTI
// ".?AVMedAsteroid@SrvAsteroid@@" (module storage) and
// ".?AVMedMine@SrvAsteroid@@" (module storage). Created only by the StarSystem
// factories the routine / the routine on behalf of
// SrvAsteroid::FSrvAsteroidCube::apply_marker (module storage) and kept in the
// system's +0xc0 block (StarSystem::ListC0). Bodies in server_classes.cpp.

// Their vtables were dumped slot by slot against GameObject's (module storage
// main, module storage IObjAffect, module storage CBase). Everything not
// overridden below is GameObject's own entry:
//   main +0x164 create   module storage  `XOR AL,AL / RET 4` (both)
//   main +0x168 find     module storage  (both)
//   main +0x16c update   MedMine only, module storage (MedAsteroid keeps
//                        GameObject's the routine)
//   main +0x17c/+0x180/+0x184  module storage `RET 8` (both) -- the ICF body
//                        the listing labels IServerImpl::Connect
//   main +0x188          MedAsteroid only, module storage `RET 4` (labelled
//                        CAccount::AppendCharacterNames); MedMine keeps
//                        GameObject's the routine
//   main +0x1bc          MedMine only: a NEW slot (112 entries against
//                        GameObject's 111), module storage `RET 8`
//   CBase slot 0         the scalar deleting destructors module storage /

// The IObjInspect part (+0x000..+0x160) and the whole IObjAffect table are
// GameObject's in both, entry for entry.
// ---------------------------------------------------------------------------
namespace SrvAsteroid {

// operator new(0x68) at module storage.
class MedAsteroid : public GameObject
{
public:
    // (module storage): GameObject's constructor, then
    // +0x64 = 0 and byte +0x60 = 0.
    MedAsteroid();
    // (module storage), entered on the CBase sub-object: frees
    // +0x64 when it is set, then GameObject's destructor (module storage JMP).
    virtual ~MedAsteroid();

    bool create(Archetype::Root* Arch) override;
    bool find(unsigned int const& Id) override;
    void damage_sub_obj(FSubObjDamage const& Hit, DamageList& Damage) override;
    void munition_hit(FMunitionImpact const& Hit, DamageList& Damage) override;
    void explosion_hit(ExplosionDamageEvent const& Blast, DamageList& Damage) override;
    void apply_damage_list(DamageList& Damage) override;

    // +0x60 / +0x64. The constructor clears both, the destructor frees +0x64
    // (the routine, operator delete) when it is non-null, and nothing else in
    // Server.dll writes either: no MedAsteroid slot differs from GameObject's
    // except the ones listed above, none of which touches them. Their meaning
    // is therefore not recoverable from this binary; the types are the ones the
    // constructor and destructor use (a byte, and a heap pointer).
    unsigned char Field_0x60;
    void* Field_0x64;
};

// operator new(0x88) at module storage. A second base, PhySys::CObjectCB, sits
// at +0x60 (COL module storage, offset 0x60); its vtable module storage holds
// the two phantom callbacks.
class MedMine : public GameObject, public PhySys::CObjectCB
{
public:
    // (module storage): GameObject's constructor, +0x80 = +0x7c =
    // 1.0f, the CObjectCB vtable at +0x60, +0x78 = 0. The phantom (+0x64), the
    // intruder count (+0x68), the direction (+0x6c) and +0x84 are left alone
    // until arm() fills them.
    MedMine();
    // (module storage): PhySys::DestroyPhantom(+0x64) -- no null
    // test -- then +0x64 = 0, then GameObject's destructor.
    virtual ~MedMine();

    bool create(Archetype::Root* Arch) override;
    bool find(unsigned int const& Id) override;
    bool update(float Dt) override;
    void damage_sub_obj(FSubObjDamage const& Hit, DamageList& Damage) override;
    void munition_hit(FMunitionImpact const& Hit, DamageList& Damage) override;
    void explosion_hit(ExplosionDamageEvent const& Blast, DamageList& Damage) override;

    // +0x1bc, module storage `RET 8`: MedMine's own two-argument slot, the 112th entry of
    // its main vtable (module storage, right before the float data at module storage);
    // MedAsteroid's table ends at +0x1b8. An empty body, like Bullet's own +0x1bc. No call
    // through a MedMine's +0x1bc exists in Server.dll; name and argument types are not
    // recoverable from the empty body.
    virtual void Slot1bc(unsigned int Arg1, unsigned int Arg2);

    // PhySys::CObjectCB. added = the branch, deleted = the branch.
    void added(CObject* Obj) override;
    void deleted(CObject* Obj) override;

    // (module storage, thiscall, RET 8): (radius, system id).
    // Only caller apply_marker (module storage), right after the factory.
    void arm(float DetectRadius, unsigned int SystemId);

    void* Phantom;                                // +0x64  PhySys phantom handle
    unsigned int IntruderCount;                   // +0x68  ships inside (unsigned: JBE at module storage)
    Vector Direction;                             // +0x6c  unit vector mine -> last ship that entered
    Archetype::Explosion const* Explosion;        // +0x78  GetExplosion(arch ExplosionArch)
    float RechargeTimer;                          // +0x7c
    float RechargeTime;                           // +0x80  Archetype::Asteroid::RechargeTime
    float ExplosionOffset;                        // +0x84  Archetype::Asteroid::ExplosionOffset
};

} // namespace SrvAsteroid

// ---------------------------------------------------------------------------
// The dying-ship list -- baseline's file-static std::list<Ship*> in ship.cpp:
// allocator byte .data:the field, _Head .data:the field, _Size
// .data:the field, 0x0C-byte nodes {Next, Prev, Ship*}.
// CRT static initialiser: sentinel = operator new(0xc),
//                 self-linked, size 0, atexit(the routine)
// atexit teardown: free every node, then the sentinel; zero
//                 head and size
// push_back(Ship*) -- Ship::update's death-sequence arm
//                 (module storage / module storage)
// remove(Ship*) -- every matching node; Ship::~Ship
//                 (module storage)
// Drained by Server_PhysicsUpdate (the routine, module storage .. 06d0026b):
// controller event 0x25 {ship id, DeathSequenceObjId, system id} per ship,
// then the list is emptied.
// A global std::list IS that initialiser/teardown pair: the compiler emits the
// same construct-then-atexit(destroy) sequence for it.
// ---------------------------------------------------------------------------
extern std::list<Ship*> GDyingShips;
void Ship_QueueDying(Ship* DyingShip);
void Ship_ForgetDying(Ship* DyingShip);

// ---------------------------------------------------------------------------
// Objects between star systems. baseline keeps them in a global std::list at
// (allocator byte) / the field (head node) / the field
// (size); a node is 0x40 bytes: next, prev, then this 0x38-byte element.
// CRT init the branch builds it and registers the routine (its destructor)
// with atexit. The system-transfer path (module storage) parks an object here
// when the target system is null; the lookup below searches it between the
// cached system and SysDB::SysMap. Defined in src/Server/server_classes.cpp.
// ---------------------------------------------------------------------------
struct FInTransitObject
{
    PhySys::CreateParms Parms;   // +0x00  the physics state leave_system handed back
    GameObject* Obj;             // +0x34
};
static_assert(offsetof(FInTransitObject, Obj) == 0x34, "FInTransitObject: Obj at +0x34");
extern std::list<FInTransitObject> GInTransitObjects;

// (cdecl): the first entry holding Obj is taken out -- its Parms
// copied to OutParms, the node freed -- and the lookup cache's watcher is
// cleared when it watches Obj. False when Obj is not in the list. Callers:
// , module storage (system transfer).
bool Server_TakeInTransit(GameObject* Obj, PhySys::CreateParms& OutParms);

// (cdecl): as above without the copy, then controller message 0x39
// (FObjectDestroyedEvent, only ObjId set) and the wrapper is deleted. False when
// Obj is not in the list. Only caller: pub::SpaceObj::Destroy (module storage).
bool Server_DestroyInTransit(GameObject* Obj);

// (cdecl): push_back {Parms, Obj}. Only caller: module storage.
void Server_PushInTransit(GameObject* Obj, PhySys::CreateParms const& Parms);

// (cdecl, no arguments; only caller the routine at module storage): the
// pub::Phantom event pump -- controller events 0x2d / 0x2e for every id a phantom saw
// enter / leave since the last tick. Defined in server_pub.cpp next to the phantom record.
void Server_PumpPhantomEvents();

// ---------------------------------------------------------------------------
// The object lookup by id. Server-internal: baseline Server.dll exports neither.
// ---------------------------------------------------------------------------
// (cdecl, 21 call sites): the one-entry cache (GLookupCacheObject /
// GLookupCacheSystem, server_sysdb.h), then the cached system's find_object,
// then GInTransitObjects, then every other system in SysDB::SysMap. OutSys is
// the system the object lives in, null for an in-transit object. On success
// the cache is re-pointed at the result.
bool Server_FindObject(unsigned int const& Id, GameObject*& OutObj, StarSystem*& OutSys);

// (cdecl, 13 call sites): Server_FindObject, then false -- with
// OutObj reset to null, OutSys left as the lookup wrote it -- when the object's
// own +0x160 (is_destroyed) answers true.
bool Server_FindLiveObject(unsigned int Id, GameObject*& OutObj, StarSystem*& OutSys);

// Server_FindObject for the callers that only keep the object: baseline passes a
// dummy for OutSys at those sites. A false return always leaves OutObj null,
// so the object pointer carries the whole answer. Server_LookupObject
// (server_cobject.h) is the same with the result flattened to void*. Defined in
// src/Server/server_pub.cpp.
GameObject* Server_LookupGameObject(unsigned int ObjId);

// (module storage), __cdecl(unsigned int Flid): the lookup
// above, then null when the object's own +0x160 (is_destroyed) answers true.
// Defined in src/Server/server_pub.cpp.
GameObject* Server_LookupLiveObject(unsigned int Flid);

// ---------------------------------------------------------------------------
// The launch placement for a player leaving a base (ServerSpace.cpp). baseline
// (module storage, cdecl, 5 args, 380 insns); only caller
// IServerImpl::PlayerLaunch (module storage). baseline name not recovered.
// OutObjId gets the object the ship launches from (0 when none), OutKind the
// launch_pos index (-1 when the ship is not launched from an object), OutPos /
// OutOrient the launch transform. Clears Players[PlayerId].exited_base_id when
// the player was in a base. Defined in src/Server/server_classes.cpp.
// ---------------------------------------------------------------------------
class Quaternion;
void Server_GetLaunchPosition(unsigned int PlayerId, unsigned int* OutObjId, int* OutKind,
                              Vector* OutPos, Quaternion* OutOrient);

// the free-terminal mailbox pub::ReportFreeTerminal writes and
// Server_GetLaunchPosition reads back after controller event 0x32. Defined in
// src/Server/server_pub.cpp.
extern int GReportedFreeTerminal;
