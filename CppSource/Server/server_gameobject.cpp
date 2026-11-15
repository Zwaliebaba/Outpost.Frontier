// Author: Giperion.

// ===========================================================================
// Server.dll's IObjRW wrapper family: GameObject / EqObj / Ship / Solar / Loot.
// See src/include/server_gameobject.h for the RTTI evidence behind the
// hierarchy and for the list of things this port does NOT model yet.
// ===========================================================================

#include "../include/server_gameobject.h"
#include "../include/common_timing.h"  // Timing::read
#include "../include/server_sysdb.h"   // StarSystem::on_object_destroyed (GameObject::destroy)
#include "../include/common_misc.h"    // SUBOBJ_ID_NONE (Ship::allocate_equip_id)
#include "../include/common_cequip.h"  // CEquipTraverser / CEquip (get_damage_list)
#include "../include/neuroncomponents.h"          // FDUMP (Ship::allocate_equip_id)
#include "../include/common_goods.h"   // FindHardpoint / JETTISONED_CARGO_VELOCITY / EquipDesc (Ship::tick_jettison)
#include "../include/common_id.h"      // CFLIDMaker (Ship::tick_jettison)
#include "../include/server_createinfo.h" // Server_Acquire3DMathEngine (the field)
#include "../include/x86math.h"          // I3DMathEngine::MatrixToQuaternion
#include "../include/common_physys.h"   // PhySys::RCBegin/RCEnd, Collision::CalcCollisionDamage
#include "../include/common_universe.h" // Universe::IZone (CSimple::ZoneAtA4)
#include <cmath>
#include <windows.h>
#include <cstdio>
#include <cstdlib>   // rand (Ship::tick_jettison / compute_jettison_launch)
#include "../include/rpc_iclient.h"   // IClient (Ship::request_returned / set_formation_*)
#include "../include/NeuronRuntime.h"         // NeuronRuntime::Engine (Ship::create / Solar::create, IEngine +0x6c)
#include "../include/common_ini.h"              // StringAlloc (EqObj::*_by_name / dump_cargo)
#include "../include/common_server.h"           // Players, Server_BroadcastGroupControl (EqObj::destroy)
#include "../include/common_server_playerdb.h"  // PlayerData_ShouldQueueVisit / PlayerData_QueueVisit
#include "../include/common_server_types.h"     // ServerStats (EqObj::destroy)
#include "../include/rpc_iclient_args.h"        // FServerBurnFuseInfo (EqObj::light_fuse)
#include <map>                                  // ServerFuseDB::Get (EqObj::light_fuse)
#include <cstring>                              // memcpy (EqObj::build_equip_loot)

// (IServerImpl.cpp) and the routine (pub::Controller::_SendMessage
// with a zero id; Server_ControllerDispatch, common_server_controller.h) -- the Ship
// IObjAffect bodies use both.
extern IClient* g_pClientInterface;
// (IServerImpl.cpp) -- EqObj::destroy bumps +0x10 (module storage).
extern ServerStats GServerStats;

// ---------------------------------------------------------------------------
// GameObject
// ---------------------------------------------------------------------------

// . The IObjRW / IObjInspectImpl prefix is constructed by the
// base-class initialiser; what is left is GameObject's own state, every store
// of which is here (2026-09-24):
// MarkedByPlayers (+0x14) -- empty list
// / 06cee3da   LastDamageTime (+0x20/+0x24) = 0
// OwnerSystem (+0x28) = 0
// Invincibilities (+0x2c) -- empty list
// / 06cee401   +0x38 / +0x39 = 0
// +0x3c = 1.0f (0x3f800000)
// / 06cee40e   bDestroyed (+0x40) / bInDamageCallback (+0x41) = 0
// PendingDamage (+0x44) -- DamageList, inline-built
// PendingDamage.Cause (+0x54) = 0x18
GameObject::GameObject()
    : LastDamageTime(0.0)
    , bInvincible(0)
    , bInvincibleFlag(0)
    , InvincibleTimer(1.0f)
    , bDestroyed(0)
    , bInDamageCallback(0)
    , OwnerSystem(nullptr)
{
    PendingDamage.Cause = static_cast<DamageCause>(0x18);
}

// (module storage .. module storage), entered with ECX on the CBase
// sub-object at +0x0c (the scalar deleting destructor the routine is the CBase
// vtable's slot 0, module storage). baseline order, raw bytes:
// re-stamp GameObject's three vtables
// 55              PUSH EBP                 ; EBP == 0
// ff158841d606    CALL [->CBase::advise]   ; advise(false)
// 8b4e04          MOV  ECX,[ESI + 0x4]     ; CBase::Object (+0x10)
// ff156443d606    CALL [->CObject::Release]; no null test
// clear + free the list at +0x44 (Common's
//                               std::list<DamageEntry>::erase, module storage)
// clear + free the list at +0x2c (the routine)
// clear + free the list at +0x14 (MarkedByPlayers)
// CALL [->CBase::~CBase]
// CALL [->Watchable::unwatch]            ; on this+0x08
// The reference released here is the one the wrapper took when it was bound:
// CObject::Alloc hands back a count of 1 (Common module storage `INC [EAX+0x98]`)
// and CObject::Find adds one (Common module storage `INC ECX`), so create() and
// find() both leave the wrapper owning exactly one reference -- and nothing but
// this destructor gives it back. The member lists and the IObjInspectImpl / CBase
// / Watchable bases are torn down by the compiler after this body, in the same
// order: the compiler destroys PendingDamage (+0x44), Invincibilities (+0x2c)
// and MarkedByPlayers (+0x14) in reverse declaration order, exactly baseline's.
GameObject::~GameObject()
{
    advise(false);
    Object->Release();
}

// (module storage, RET 4): return !is_destroyed(). The float
// argument is discarded (baseline pushes Dt then ignores it).
bool GameObject::update(float Dt)
{
    (void)Dt;
    return !is_destroyed();
}

// (module storage, `RET 0x8`). See the declaration for why the
// element is a player id.
void GameObject::mark_for_player(unsigned int PlayerId, int MarkType)
{
    // .. module storage -- the search runs FIRST, before either
    // branch looks at MarkType, and it stops on the first match
    // (module storage `CMP [EAX + 0x8],EDX`).
    std::list<unsigned int>::iterator It = MarkedByPlayers.begin();
    while (It != MarkedByPlayers.end() && *It != PlayerId)
        ++It;

    // 83fa01  CMP EDX,0x1 / module storage JNZ the branch, and
    // 85d2  TEST EDX,EDX / module storage JNZ the branch --
    // baseline compares against the literals 1 and 0, so any other MarkType falls
    // out at the branch having done nothing. Do not turn this into a bool.
    if (MarkType == 1) {
        // `CMP EAX,ECX` / module storage `JNZ the branch` --
        // only when it is NOT already in the list.
        if (It == MarkedByPlayers.end())
            MarkedByPlayers.push_back(PlayerId);   // PUSH 0xc / new
        return;
    }
    if (MarkType == 0) {
        // / module storage -- only when it IS present; then
        // .. module storage unlink, free, size--.
        if (It != MarkedByPlayers.end())
            MarkedByPlayers.erase(It);
    }
}

// (module storage, `RET 0x4`), fourteen instructions:
// 8b4918    MOV    ECX,[ECX + 0x18]     ; the list sentinel
// 395008    CMP    [EAX + 0x8],EDX      ; node value vs the id
// 3bc1      CMP    EAX,ECX
// 0f95c0    SETNZ  AL                   ; "not end()" == found
bool GameObject::is_marked_by(unsigned int PlayerId) const
{
    for (std::list<unsigned int>::const_iterator It = MarkedByPlayers.begin();
         It != MarkedByPlayers.end(); ++It) {
        if (*It == PlayerId)
            return true;
    }
    return false;
}

// baseline inlines this guard at every single call site: load CBase::Object and
// null it unless (ObjectClass & 3) == 3. module storage is the shortest
// example, module storage (pub::SpaceObj::GetCenterOfMass) the clearest.
CObject* GameObject::cobj() const
{
    CObject* Obj = Object;
    if (Obj == nullptr)
        return nullptr;
    if ((static_cast<unsigned>(Obj->ObjectClass) & 3u) != 3u)
        return nullptr;
    return Obj;
}

void GameObject::attach(CObject* Obj)
{
    Object = Obj;
    advise(true);
}

CSimple* GameObject::csimple() const
{
    return static_cast<CSimple*>(cobj());
}

unsigned char GameObject::get_invincible_flag() const
{
    return bInvincibleFlag;
}

unsigned char GameObject::is_invincible() const
{
    return bInvincible;
}

float GameObject::get_invincible_timer() const
{
    return InvincibleTimer;
}

// ---------------------------------------------------------------------------
// baseline slot +0x1a8 -- "give me a DamageList describing every sub-object of
// mine whose relative health is below StatusCeiling".

// (module storage, 27 instructions, `RET 0x8`) is the BASE body
// that ten of the thirteen wrapper classes carry unchanged; the EqObj family's
// three (EqObj module storage, Ship module storage, Solar module storage,
// each + 0x1a8) override it with module storage, which calls this one first.

// The base only ever reports the HULL:
// MOV  ESI,[ECX + 0x10]        ; CBase::Object
// AND  EAX,0x3 / CMP AL,0x3    ; the usual CSimple narrowing
// CALL [->COMMON.DLL::CSimple::get_relative_health]
// FCOMP float ptr [ESP + 0xc]  ; against StatusCeiling
// TEST AH,0x5 / JP  -> return
// CALL [->COMMON.DLL::CSimple::get_hit_pts]
// MOV  ECX,[->COMMON.DLL::SUBOBJ_ID_ROOT]
// CALL [->COMMON.DLL::DamageList::add_damage_entry]

// The x87 predicate, spelled out because it inverts if read loosely: FCOMP
// leaves C3 in AH bit 6, C2 in bit 2 and C0 in bit 0. `TEST AH,0x5` looks at
// C0 and C2 only, and JP jumps on EVEN parity of the masked byte. ST0 > mem
// gives AH&5 == 0 (even, jump); ST0 == mem gives AH&5 == 0 (even, jump);
// unordered gives AH&5 == 5, two bits set (even, jump); only ST0 < mem gives
// AH&5 == 1 (odd, fall through). So the entry is added ONLY when the health is
// strictly BELOW the ceiling -- which is why the observer's 2.0f means
// "everything".
void GameObject::get_damage_list(DamageList& Out, float StatusCeiling) const
{
    CSimple* const Simple = csimple();
    if (Simple == nullptr)
        return;
    if (!(Simple->get_relative_health() < StatusCeiling))
        return;
    // PUSH 0x0 -- the fate argument, pushed before the
    // get_hit_pts call whose float lands on top of it.
    Out.add_damage_entry(SUBOBJ_ID_ROOT, Simple->get_hit_pts(),
                         DamageEntry::SubObjFate::None);
}

// ---------------------------------------------------------------------------
// EqObj::get_damage_list -- module storage, 104 instructions, `RET 0x8`.

// Three vtables hold it and only three: module storage, module storage and
// , i.e. base + 0x1a8 for EqObj (module storage, stamped at
// inside EqObj::EqObj at module storage), Ship
// (module storage, module storage) and Solar (module storage,
// ). Declaring it on EqObj reproduces exactly that set.

// Two traps in this body, both caught on the bytes:

//  1. The value COMPARED and the value RECORDED are different accessors. The
//     comparison is `module storage  ff5244  CALL dword ptr [EDX + 0x44]` and
//     the recorded number is `module storage  ff503c  CALL dword ptr [EAX + 0x3c]`.
//     CETractor's own vftable (module storage) names both: base + 0x44 at
// is CEquip::GetRelativeHealth and base + 0x3c at
// is CEquip::GetHitPoints. An earlier note on this item
//     said the entry carries "the equip's own status", i.e. the +0x44 value.
//     It does not.

//  2. The traverser is the TWO-argument overload. module storage calls through
// , whose decorated name is "??0CEquipTraverser@@QAE@H_N@Z"
//     -- (int, bool) -- with `PUSH 0x1` then `PUSH -0x1` at module storage /
// , so the arguments are (-1, true): every type, destroyed
//     items included. That is a different import from the one-argument
//     "??0CEquipTraverser@@QAE@H@Z" at module storage the create builders use.
//     And the manager call is the NON-const Traverse overload
//     (module storage, "?Traverse@CEquipManager@@QAEPAVCEquip@@...") where
// uses the const one at module storage.
// ---------------------------------------------------------------------------
void EqObj::get_damage_list(DamageList& Out, float StatusCeiling) const
{
    // CALL the routine -- a DIRECT call to the base body, with
    // the same two arguments re-pushed at module storage / module storage.
    GameObject::get_damage_list(Out, StatusCeiling);

    // .. module storage -- the inlined accessor: [this+0x10],
    // and zero unless (ObjectClass & 0x103) == 0x103. baseline has NO early exit
    // on the zero: module storage zeroes EAX and module storage still does
    // `LEA ECX,[EAX + 0xe4]` and calls CEquipManager::Traverse on it. Neither
    // does the port. The override sits only in EqObj's, Ship's and Solar's
    // vtables, whose wrappers always hold a CEqObj, so baseline never reaches that
    // path; a null here is an upstream defect and faults here as in baseline.
    CEqObj* const Owner = ceqobj();

    // CEquipTraverser(-1, true).
    CEquipTraverser Walk(-1, true);
    for (;;) {
        // CALL [->COMMON.DLL::CEquipManager::Traverse]
        CEquip* const Equip = Owner->EquipManager.Traverse(Walk);
        if (Equip == nullptr)
            break;

        // ff5208  CALL dword ptr [EDX + 0x8] -- CEquip vtable
        // +0x08 is IsDestroyed (module storage in ??_7CETractor@@6B@).
        if (Equip->IsDestroyed()) {
            // PUSH 0x3 / module storage PUSH 0x0 /
            // PUSH EAX -- so the arguments in source order are
            // (id, 0.0f, SubObjFate(3)). The pushed 0 is a float bit pattern,
            // not an int: the other arm at module storage FSTPs into the same
            // stack slot. SubObjFate(3) is HealthProxy in common_misc.h.
            Out.add_damage_entry(Equip->Id, 0.0f,
                                 DamageEntry::SubObjFate::HealthProxy);
            continue;
        }

        // CALL [EDX + 0x44]  ; CEquip::GetRelativeHealth
        // FCOMP float ptr [ESP + 0x34]  ; unrounded ST0 (double)
        // TEST AH,0x5 / JP -> skip unless ST0 < ceiling
        if (!(Equip->GetRelativeHealth() < StatusCeiling))
            continue;

        // MOV DI,word ptr [ESI + 0x8] -- the id is taken before
        // the call, as a word out of CEquip+0x08.
        Out.add_damage_entry(Equip->Id, Equip->GetHitPoints(),
                             DamageEntry::SubObjFate::None);
    }

    // MOV byte ptr [ESP + 0x14],0x1 -- the CArchGrpTraverser's
    // "at start" byte, then the identical second pass over the arch groups at
    // CEqObj+0x144.
    CArchGrpTraverser Groups;
    for (;;) {
        // CALL [->COMMON.DLL::CArchGroupManager::Traverse]
        CArchGroup* const Group = Owner->ArchGroups.Traverse(Groups);
        if (Group == nullptr)
            break;

        // CALL [->COMMON.DLL::CArchGroup::GetRelativeHealth]
        // FCOMP float ptr [ESP + 0x34]  ; the same ceiling
        if (!(Group->GetRelativeHealth() < StatusCeiling))
            continue;

        // MOV EDX,[ESI + 0x4] -- CArchGroup::Arch --
        // MOV DI,word ptr [EDX + 0x4] -- and its Id.
        // CALL [->COMMON.DLL::CArchGroup::GetHitPoints]
        Out.add_damage_entry(Group->GetID(), Group->GetHitPoints(),
                             DamageEntry::SubObjFate::None);
    }
}

// ---------------------------------------------------------------------------
// The four baseline slots between IObjInspect's last and create(). See the
// header for the vtable arithmetic that pins them down.
// ---------------------------------------------------------------------------

// +0x154 -- module storage
unsigned int GameObject::get_type_bits() const
{
    // the inlined (ObjectClass & 3) guard yields
    // null and `MOV EAX,[EAX+0xe0]` reads through it anyway: baseline has no
    // default value, so neither does the port (a null here is an upstream
    // defect and faults as in baseline).
    return csimple()->TypeBits;
}

// +0x158 -- module storage
bool GameObject::destroy(int Kind, unsigned int ObjId)
{
    // the (ObjectClass & 3) guard nulls the
    // pointer and the call goes through it anyway: no null test in baseline.
    // PUSH 0 / CALL [CObjectVt + 0x98] == CSimple::set_hit_pts.
    csimple()->set_hit_pts(0.0f);

    // CALL the routine with ECX = OwnerSystem, i.e.
    // StarSystem::on_object_destroyed(this, Kind). PORTED 2026-09-17 (wave 11);
    // all three PlayerData helpers it needs exist now
    // (PlayerData_TerminateTrade / _CancelTradeRequest / _SetRelativeHealth),
    // and the three fields the earlier note called unmodelled turned out to be
    // modelled already: +0x268 is `relative_health` (0x418 - 0x1b0), +0x3CC is
    // `trade_offer` and +0x3D4 is `inbound_trade_requests`.

    // baseline passes only the FIRST argument through; ObjId is this slot's own
    // second parameter and never leaves GameObject::destroy.
    // ECX = [ESI+0x28] with no null test.
    OwnerSystem->on_object_destroyed(this, Kind);
    (void)ObjId;

    // CALL [own vtable + 0x15c]
    destroy_notify();

    // MOV AL,1
    return true;
}

// +0x15c -- module storage
void GameObject::destroy_notify()
{
    // MOV byte ptr [ESI + 0x40],0x1
    bDestroyed = 1;

    // CALL COMMON.DLL::SinglePlayer / module storage
    // MOV ECX,[EAX+0xb4] -- CSimple::OwnerPlayer. In single player an object
    // that belongs to a player is left connected. The CSimple comes out of the
    // (ObjectClass & 3) == 3 guard (module storage .. 06cee693) and is read
    // without a null test.
    if (SinglePlayer())
    {
        if (csimple()->OwnerPlayer != 0)
            return;
    }

    // .. module storage -- the same guard again, then
    // CALL [CObjectVt + 0x94] == CSimple::disconnect, ECX = the CSimple and
    // the OwnerSystem (+0x28) pushed as the IObjDB*; no null test on either.
    csimple()->disconnect(OwnerSystem);
}

// +0x160 -- module storage
bool GameObject::is_destroyed() const
{
    return bDestroyed != 0;
}

// ---------------------------------------------------------------------------
// GameObject's extension slots +0x170 .. +0x1b8 (declaration order == baseline
// slot order, see server_gameobject.h). PORTED 2026-09-24
// (claude-code:w5-server-06ce) from the listing, one body per slot.
// ---------------------------------------------------------------------------

// .data:the field -- GameObj.cpp's own lazily fetched math engine
// (module storage `MOV EAX,[the field]` / JNZ / CALL the routine / store).
static I3DMathEngine* GGameObjectMathEngine = nullptr;

// +0x170 -- module storage (RET 8).
void GameObject::enter_system(StarSystem* Sys, PhySys::CreateParms const* Parms)
{
    // MOV [EDI+0x28],EAX
    OwnerSystem = Sys;

    // only when Parms is given: the RAW CBase
    // Object (no class guard), CObject vt +0x78 remake_physical(*Parms, 0.0f).
    if (Parms != nullptr)
        Object->remake_physical(*Parms, 0.0f);

    // outside single player an object that has an
    // owner player is put under remote correction. The owner test reads the
    // guarded CSimple without a null test (module storage).
    if (!SinglePlayer() && csimple()->OwnerPlayer != 0)
    {
        // LEA ESI,[EAX+0x8] -- the RAW Object's orientation;
        // the field's MatrixToQuaternion (+0x50).
        if (GGameObjectMathEngine == nullptr)
            GGameObjectMathEngine = Server_Acquire3DMathEngine();
        Quaternion Orientation;
        GGameObjectMathEngine->MatrixToQuaternion(reinterpret_cast<FQuaternion&>(Orientation),
                                                  reinterpret_cast<FMatrix const&>(Object->Orientation));

        // EBP = &RawObject->WorldPosition (+0x2c),
        // EBX = the guarded CSimple; the angular velocity (vt +0x58) is fetched
        // before the velocity (vt +0x54), both from the raw Object, then
        // RCBegin(cs, pos, vel, quat, angvel).
        CSimple* const Simple = csimple();
        Vector const AngularVelocity = Object->get_angular_velocity();
        Vector const Velocity = Object->get_velocity();
        PhySys::RCBegin(Simple, Object->WorldPosition, Velocity, Orientation, AngularVelocity);
    }

    // connect(Sys) (CSimple vt +0x90) unless this
    // is a player's object in single player. baseline's null-guard arm
    // (module storage) calls through a null pointer; there is no extra test.
    if (SinglePlayer() && csimple()->OwnerPlayer != 0)
        return;
    csimple()->connect(Sys);
}

// +0x174 -- module storage (RET 4).
void GameObject::leave_system(PhySys::CreateParms* OutParms)
{
    // EDI = the RAW Object (no guard) -- every physics read
    // below goes through it.
    CObject* const Raw = Object;

    // OwnerSystem->the routine(cs->Id): every
    // ship and solar of the system hears that this object is leaving (+0x178).
    OwnerSystem->notify_object_left(csimple()->Id);

    // RCEnd only outside single player and only
    // for an owned object.
    if (!SinglePlayer() && csimple()->OwnerPlayer != 0)
        PhySys::RCEnd(csimple());

    // the caller's CreateParms, from the live body.
    if (OutParms != nullptr)
    {
        OutParms->Mass = Raw->get_mass();                  // vt +0x6c
        OutParms->bHasInertia = 1;
        // BodyPtr (+0x54) picks the IVP moment of
        // inertia or a zero vector.
        Vector Inertia;
        if (Raw->BodyPtr != nullptr)
            Inertia = PhySys::GetMomentOfInertia(Raw);
        else
        {
            Inertia.x = 0.0f;
            Inertia.y = 0.0f;
            Inertia.z = 0.0f;
        }
        OutParms->Inertia = Inertia;
        OutParms->bEnableCollisions = PhySys::GetCollisionState(Raw) ? 1 : 0;
        PhySys::GetDamping(Raw, OutParms->LinearDamping,
                           *reinterpret_cast<Vector*>(OutParms->AngularDamping));
        // an unbounded byte copy of the collision
        // group name into +0x1e, terminator included (baseline's inlined strcpy).
        char const* Src = PhySys::GetCollisionGroup(Raw);
        char* Dst = OutParms->Name;
        char Ch;
        do
        {
            Ch = *Src++;
            *Dst++ = Ch;
        } while (Ch != 0);
    }

    // CALL [EDX+0x74] -- unmake_physical on the raw Object.
    Raw->unmake_physical();

    // disconnect(OwnerSystem) (vt +0x94) unless a
    // player's object in single player; OwnerSystem = null either way.
    if (!(SinglePlayer() && csimple()->OwnerPlayer != 0))
        csimple()->disconnect(OwnerSystem);
    OwnerSystem = nullptr;
}

// +0x178 -- module storage (RET 4).
void GameObject::on_object_left_system(unsigned int ObjId)
{
    // the raw Object narrowed with the CEqObj mask
    // 0x103; anything else does nothing.
    CObject* const Obj = Object;
    if (Obj == nullptr)
        return;
    if ((static_cast<unsigned>(Obj->ObjectClass) & 0x103u) != 0x103u)
        return;
    IBehaviorManager* const Behavior = static_cast<CEqObj*>(Obj)->get_behavior_interface();
    if (Behavior == nullptr)
        return;

    // a stack record {3, &Payload} with Payload
    // {0, 0, ObjId}, handed to IBehaviorManager's vtable slot 0 (Common
    // , the behavior manager's notification switch; case 3 is
    // "an object left the system"). The port's IBehaviorManager has the same
    // one-slot vtable; slot 0 is virtual Slot00.
    unsigned int Payload[3];
    Payload[0] = 0;
    Payload[1] = 0;
    Payload[2] = ObjId;
    FBehaviorManagerEvent Event;
    Event.Type = 3;
    Event.Data = Payload;
    Behavior->Slot00(&Event);
}

// +0x17c -- module storage (RET 8).
void GameObject::damage_sub_obj(FSubObjDamage const& Hit, DamageList& Damage)
{
    // Hit+4 against SUBOBJ_ID_ROOT, then NONE.
    if (Hit.SubObjId == SUBOBJ_ID_ROOT || Hit.SubObjId == SUBOBJ_ID_NONE)
        damage_hull(Hit.Damage, Damage);                   // vt +0x1b0
}

// +0x180 -- module storage (RET 8).
void GameObject::munition_hit(FMunitionImpact const& Hit, DamageList& Damage)
{
    // Hit+8 against ROOT, then NONE.
    if (Hit.SubObjId == SUBOBJ_ID_ROOT || Hit.SubObjId == SUBOBJ_ID_NONE)
        damage_hull(Hit.Munition->HullDamage, Damage);     // Munition+0x80
}

// +0x184 -- module storage (RET 8).
void GameObject::explosion_hit(ExplosionDamageEvent const& Blast, DamageList& Damage)
{
    // R2 = Radius^2 and the two inner bands
    // R2 * (4/9) (the field) and R2 * (1/9) (the field): the squares of
    // 2/3 and 1/3 of the radius.
    float const Radius = Blast.Explosion->Radius;
    float const Radius2 = Radius * Radius;
    float const Band2 = Radius2 * (4.0f / 9.0f);
    float const Band3 = Radius2 * (1.0f / 9.0f);

    // raw Object vt +0x64 get_physical_radius_r(Center).
    Vector Center;
    float const ObjRadius = Object->get_physical_radius_r(Center);

    // ((dz*dz + dx*dx) + dy*dy) - r*r. The three
    // differences and the result never leave the x87 stack (FSTP ST4 / FSTP ST0
    // at module storage .. 06ceebf5), so they are doubles here.
    double const Dx = static_cast<double>(Blast.Position.x) - Center.x;
    double const Dy = static_cast<double>(Blast.Position.y) - Center.y;
    double const Dz = static_cast<double>(Blast.Position.z) - Center.z;
    double Dist2 = ((Dz * Dz + Dx * Dx) + Dy * Dy) - static_cast<double>(ObjRadius) * ObjRadius;
    // FCOM 0.0 / TEST AH,0x41 / JZ -- kept only when > 0.
    if (!(Dist2 > 0.0))
        Dist2 = 0.0;

    // TEST AH,5 / JP skips on >= or unordered, so
    // each band is a strict "<".
    int Factor;
    if (Dist2 < Band3)
        Factor = 3;
    else if (Dist2 < Band2)
        Factor = 2;
    else if (Dist2 < Radius2)
        Factor = 1;
    else
        return;

    // ((FILD Factor * 1/3f (the field)) *
    // HullDamage) * Root::ExplosionResistance (+0x28), one FSTP dword at the
    // end, then vt +0x1b0.
    float const Resistance = Object->get_archetype()->ExplosionResistance;
    float const Amount = static_cast<float>(
        ((static_cast<double>(Factor) * (1.0f / 3.0f)) * Blast.Explosion->HullDamage) * Resistance);
    damage_hull(Amount, Damage);
}

// +0x188 -- module storage (RET 4).
void GameObject::apply_damage_list(DamageList& Damage)
{
    // re-entry is reported, not refused.
    if (is_in_damage_callback())
    {
        FDUMP(0x100001,
              "%s(%d) : *** ERROR: ID(0x%X) Arch(%s): Tried to apply damage of type %s within another damage callback.",
              "Server\\space\\Gameobj.cpp", 0x1cf,
              get_id(), Object->get_archetype()->Nickname.get_string(),
              DamageList::DmgCauseToString(Damage.Cause));
    }

    set_in_damage_callback(true);                          // vt +0x190

    // only a non-empty list: every ROOT entry goes
    // through vt +0x1b4, then the system broadcast (the routine).
    if (Damage.Count != 0)
    {
        for (std::list<DamageEntry>::iterator It = Damage.Entries.begin();
             It != Damage.Entries.end(); ++It)
        {
            if (It->SubObjId == SUBOBJ_ID_ROOT)
                apply_root_damage(*It);
        }
        StarSystem_DamageObject(OwnerSystem, this, &Damage);
    }


    if (Damage.bDestroyed && !is_destroyed() && !is_damage_blocked(Damage))
        destroy(1, Damage.InflictorId);

    set_in_damage_callback(false);
}

// +0x18c -- module storage `MOV AL,[ECX+0x41]; RET`.
bool GameObject::is_in_damage_callback() const
{
    return bInDamageCallback != 0;
}

// The flush set_in_damage_callback (+0x190, module storage .. 06cef092) and
// set_relative_health (+0x1ac, module storage .. 06cef31d) both inline: a fresh
// DamageList receives a copy of PendingDamage's entries (std::list::insert,
// which carries the size, i.e. Count) and its four scalars; PendingDamage's
// entries are then erased one by one (Count back to 0 -- bDestroyed is NOT
// reset), and the copy goes through apply_damage_list (vt +0x188).
static void GameObject_FlushPendingDamage(GameObject* Obj, DamageList& Pending)
{
    DamageList Local;
    Local.Entries.insert(Local.Entries.begin(), Pending.Entries.begin(), Pending.Entries.end());
    Local.Count = Pending.Count;
    Local.bDestroyed = Pending.bDestroyed;
    Local.Cause = Pending.Cause;
    Local.InflictorId = Pending.InflictorId;
    Local.InflictorOwner = Pending.InflictorOwner;
    Pending.Entries.clear();
    Pending.Count = 0;
    Obj->apply_damage_list(Local);
}

// +0x190 -- module storage (RET 4).
void GameObject::set_in_damage_callback(bool bIn)
{
    bInDamageCallback = bIn ? 1 : 0;
    // / 06ceefcd -- only when clearing, and only with a non-empty
    // PendingDamage (+0x4c, its Count).
    if (bIn || PendingDamage.Count == 0)
        return;
    GameObject_FlushPendingDamage(this, PendingDamage);
}

// +0x194 -- module storage (RET 4).
void GameObject::set_invincible(FInvincibility const& Cmd)
{
    // the command's own values start the reduction.
    bInvincible = Cmd.bProtect;
    bInvincibleFlag = Cmd.bFromPlayers;
    InvincibleTimer = Cmd.MaxHpLoss;

    // one pass over every node, no early exit.
    bool bNew = true;
    for (std::list<FInvincibility>::iterator It = Invincibilities.begin();
         It != Invincibilities.end(); ++It)
    {
        if (It->Type == Cmd.Type)
        {
            // the stored command is replaced and not
            // folded (its new values already are).
            *It = Cmd;
            bNew = false;
            continue;
        }
        // the two flags are OR-ed in.
        if (bInvincible == 0)
            bInvincible = It->bProtect;
        if (bInvincibleFlag == 0)
            bInvincibleFlag = It->bFromPlayers;
        // FCOMPP of node against current; TEST
        // AH,0x41 / JNZ takes the node's value on <= or unordered: the minimum.
        float const Current = InvincibleTimer;
        float const Node = It->MaxHpLoss;
        InvincibleTimer = (Node > Current) ? Current : Node;
    }

    // push_back of a Type not seen before.
    if (bNew)
        Invincibilities.push_back(Cmd);
}

// +0x1a4 -- module storage (RET 4).
void GameObject::on_collision(FCollisionEvent const& Hit)
{
    // nothing for an id-less partner or for a
    // partner that belongs to a player (its CSimple read without a null test).
    if (Hit.Other->get_id() == 0)
        return;
    if (Hit.Other->csimple()->OwnerPlayer != 0)
        return;

    // relative velocity through IObjInspect +4;
    // the partner's is fetched first.
    Vector const OtherVelocity = Hit.Other->get_velocity();
    Vector const OwnVelocity = get_velocity();
    float const Rx = OwnVelocity.x - OtherVelocity.x;
    float const Ry = OwnVelocity.y - OtherVelocity.y;
    float const Rz = OwnVelocity.z - OtherVelocity.z;

    // this object's mass (IObjInspect +0x30) is
    // the third argument, the partner's the second, sqrt((x*x + z*z) + y*y)
    // the first.
    float const OwnMass = get_mass();
    float const OtherMass = Hit.Other->get_mass();
    // the squares, their sum and FSQRT stay on
    // the x87 stack; one FSTP dword stores the speed.
    float const Speed = static_cast<float>(sqrt((static_cast<double>(Rx) * Rx
                                                 + static_cast<double>(Rz) * Rz)
                                                + static_cast<double>(Ry) * Ry));
    float const Amount = Collision::CalcCollisionDamage(Speed, OtherMass, OwnMass);

    // the record the routine reads.
    FCollisionDamage Record;
    Record.SourceId = get_id();
    Record.SourceSubObjId = Hit.SubObjId;
    Record.TargetId = Hit.Other->get_id();
    Record.TargetSubObjId = Hit.OtherSubObjId;
    Record.Pad0e = 0;
    Record.Damage = Amount;
    OwnerSystem->collision_damage(Record, Hit.Other);
}

// +0x1ac -- module storage (RET 4).
void GameObject::set_relative_health(float Relative)
{
    // a root entry of max * Relative, fate 0, into
    // PendingDamage.
    PendingDamage.add_damage_entry(SUBOBJ_ID_ROOT, csimple()->get_max_hit_pts() * Relative,
                                   DamageEntry::SubObjFate::None);
    // TEST AH,0x41 / JP skips on > or unordered.
    if (Relative <= 0.0f)
        PendingDamage.bDestroyed = true;
    // applied at once unless inside a callback.
    if (is_in_damage_callback())
        return;
    GameObject_FlushPendingDamage(this, PendingDamage);
}

// +0x1b0 -- module storage (RET 8).
bool GameObject::damage_hull(float Amount, DamageList& Damage)
{
    // TEST AH,0x44 / JNP: exactly zero does nothing.
    if (Amount == 0.0f)
        return false;

    // what the list already says, else the hull.
    float Current = Damage.get_hit_pts_left(SUBOBJ_ID_ROOT);
    if (Current < 0.0f)
        Current = csimple()->get_hit_pts();


    float NewHitPts = Current;
    if (Amount > 0.0f)
    {
        float const Left = Current - Amount;
        NewHitPts = (Left > 0.0f) ? Left : 0.0f;
    }
    else if (Amount < 0.0f)
    {
        // a negative amount zeroes the hull.
        NewHitPts = 0.0f;
    }

    // the invincibility floor: the hull may not go
    // below (1 - InvincibleTimer) of its maximum. Already below it -> unchanged;
    // crossing it -> clamped onto it.
    if (is_damage_blocked(Damage))
    {
        // Max (module storage) and Floor (module storage) are stored floats;
        // 1/Max (module storage .. 06ceed8e) stays on the x87 stack for both
        // products, so it is a double here.
        float const MaxHitPts = csimple()->get_max_hit_pts();
        float const Floor = 1.0f - get_invincible_timer();
        double const InvMax = 1.0 / static_cast<double>(MaxHitPts);
        if (Current * InvMax < Floor)
            NewHitPts = Current;
        else if (NewHitPts * InvMax < Floor)
            NewHitPts = Floor * MaxHitPts;
    }

    // TEST AH,0x44 / JNP: no change, nothing added.
    if (NewHitPts == Current)
        return false;

    Damage.add_damage_entry(SUBOBJ_ID_ROOT, NewHitPts, DamageEntry::SubObjFate::None);
    // TEST AH,0x41 / JP: <= 0 (ordered) destroys.
    if (NewHitPts <= 0.0f)
    {
        Damage.bDestroyed = true;
        return true;
    }
    return false;
}

// +0x1b4 -- module storage (RET 4): tail-jump into CSimple vt +0x98.
void GameObject::apply_root_damage(DamageEntry const& Entry)
{
    csimple()->set_hit_pts(Entry.HitPts);
}

// +0x1b8 -- module storage (RET 4).
bool GameObject::is_damage_blocked(DamageList const& Damage) const
{
    // CMP EDX,0x10 / JGE -- a signed compare of the cause.
    if (static_cast<int>(Damage.Cause) >= 0x10)
        return false;

    if (Damage.InflictorOwner != 0)
        return get_invincible_flag();
    return is_invincible();
}

// Ship::create's body (module storage). Only Ship uses it: Solar::create opens
// before it advises and Loot::create has a different order and return value, so
// those two spell their own sequence out below.
//     PUSH 0x503 / CALL COMMON.DLL::CObject::Alloc
//     MOV [ESI+0x10],EAX            ; CBase::Object = the new CObject
//     PUSH EAX / MOV ECX,EDI / CALL [module storage]
//                                   ; COMMON.DLL::Archetype::Root::create_instance(
//                                   ;   (IEngineInstance*)Object) -- ONE argument,
//                                   ;   so it is not Root::AddRef (no arguments);
//                                   ;   the import label at module storage names it
//     MOV EDX,[ESI+0x10] / MOV EDX,[EDX+4]        ; EngineObject::EngineInstanceId
//     ... CALL [NeuronRuntime::Engine vtable + 0x6c]      ; IEngine::UpdateInstance(inst,0,0)
//     PUSH 1 / LEA ECX,[ESI+0xc] / CALL COMMON.DLL::CBase::advise
//     MOV ECX,[ESI+0x10] / MOV EAX,[ECX] / PUSH EDI / CALL [EAX+0x4c]  ; CObject::open
//     return Object != nullptr
// No null checks anywhere: baseline dereferences Arch and Object unconditionally.
bool GameObject::CreateOfClass(CObject::Class ObjectClass, Archetype::Root* Arch)
{
    Object = CObject::Alloc(ObjectClass);

    Arch->create_instance(static_cast<IEngineInstance*>(Object));

    // NeuronRuntime::Engine vtable +0x6c (slot 27),
    // IEngine::UpdateInstance(Object->EngineInstanceId, 0, 0). Server.vcxproj
    // references NeuronRuntime (the same call Loot::create / Mine::create make).
    NeuronRuntime::Engine->UpdateInstance(reinterpret_cast<void*>(Object->EngineInstanceId), 0, nullptr);

    advise(true);

    Object->open(Arch);

    return Object != nullptr;
}

// Ship::find at module storage -- the same shape with CObject::Find instead of
// CObject::Alloc, no archetype and no engine touch.
bool GameObject::FindOfClass(CObject::Class ObjectClass, unsigned int const& Id)
{
    Object = CObject::Find(Id, ObjectClass);
    advise(true);
    return Object != nullptr;
}

// ---------------------------------------------------------------------------
// IObjAffect -- GameObject's own table, module storage (stored at +4 by the ctor,
// ). PORTED 2026-09-24 against that table: every GameObject entry is a
// one-instruction error stub, and eleven of the thirteen Server wrappers keep it slot for
// slot (the table dumps are in common_iface.h). The error codes are baseline's:
// MOV EAX,0xfffffffd / RET 4
// MOV EAX,0xfffffff8 / RET 4
// XOR EAX,EAX / RET          (ICF-folded with pub::Player::SetCostume)
// MOV EAX,0xfffffffd / RET 0x10
// MOV EAX,0xfffffffd / RET 0xc
// MOV EAX,0xfffffffd / RET 8
// MOV EAX,0xfffffffd / RET 0x14
// MOV EAX,0xfffffffd / RET
// The real bodies are Ship's (module storage) and EqObj/Solar's (module storage /
// ) overrides: EqObj::slot_34/3c/40/44/4c/58 (module storage..06ce7210,
// below) and the Ship items module storage..06d02ad0.
// ---------------------------------------------------------------------------
int GameObject::set_throttle(float) { return -3; }
int GameObject::set_axis_throttle(Vector const&) { return -3; }
int GameObject::set_nudge_vec(Vector const&) { return -3; }
int GameObject::set_strafe_dir(StrafeDir) { return -3; }
int GameObject::set_formation_leader(IObjRW*) { return -8; }
int GameObject::set_formation_offset(Vector const&) { return -8; }
int GameObject::add_formation_follower(IObjRW*) { return -8; }
int GameObject::remove_formation_follower(IObjRW*) { return -8; }
int GameObject::get_formation_follower_count() { return 0; }
int GameObject::set_follow_leader(IObjRW*) { return -8; }
int GameObject::set_follow_offset(Vector const&) { return -8; }
int GameObject::add_follow_follower(IObjRW*) { return -8; }
int GameObject::remove_follow_follower(IObjRW*) { return -8; }
int GameObject::slot_34(unsigned short const*, unsigned, Vector const*, int) { return -3; }
int GameObject::slot_38(unsigned short, int, int) { return -3; }
int GameObject::slot_3c(unsigned short const*, std::vector<IObjRW*> const&, int) { return -3; }
int GameObject::slot_40(unsigned short const*, IObjRW*, int) { return -3; }
int GameObject::slot_44(unsigned short const*, IObjRW*, unsigned short, int) { return -3; }
int GameObject::set_target(IObjRW*, unsigned short, int) { return -3; }
int GameObject::slot_4c(unsigned short const*, bool, int) { return -3; }
int GameObject::slot_50(bool, bool, int) { return -3; }
int GameObject::slot_54(bool, int) { return -3; }
int GameObject::slot_58(unsigned short, unsigned, int) { return -3; }
int GameObject::slot_5c(int, unsigned const*, unsigned const*, int, RequestCB*) { return -3; }
int GameObject::slot_60(int, unsigned const*, int) { return -3; }
int GameObject::go_tradelane(IObjInspect const*, IObjInspect const*, bool, float) { return -3; }
int GameObject::stop_tradelane() { return -3; }
int GameObject::slot_6c(int, int) { return -3; }
int GameObject::slot_70(unsigned) { return -3; }
int GameObject::slot_74(unsigned) { return -3; }

// ---------------------------------------------------------------------------
// EqObj -- module storage
// ---------------------------------------------------------------------------

// . GameObject's ctor (the routine), then the three list heads
// (+0x60 DockQueue, +0x70 ServerEquips, +0x7c Fuses -- operator new(0x10 / 0xc /
// 0x44) self-linked sentinels, sizes zeroed), +0x6c CargoDecayTimer = 0
// (module storage) and +0x88 bDeathFuseLit = 0 (module storage); then EqObj's
// three vtables (module storage / 06d651c4 / 06d651b8).
EqObj::EqObj()
    : CargoDecayTimer(0.0f)
    , bDeathFuseLit(0)
{
}

// (the body behind the deleting dtor module storage; ECX is the
// CBase sub-object at +0x0c). EqObj's vtables go back in first, so the
// clear_server_equip call below is EqObj's own. Order:
// every DockQueue node erased (the routine);
// clear_server_equip (vt +0x1e0);
// then the member destructors in reverse declaration order -- Fuses (+0x7c,
// per node), ServerEquips (+0x70), DockQueue (+0x60) -- and
// GameObject's destructor (the routine).
EqObj::~EqObj()
{
    DockQueue.clear();
    clear_server_equip();
}

// (module storage .. module storage, RET 4). PORTED 2026-09-24
// (claude-code:w5-server-06cf), all 15 instructions: ECX = [this + 0x64] (the
// DockQueue head); walk from head->next until back at the head, comparing the
// node's +0xc (FDockRequest::ShipId) with the argument; a hit returns 1, the
// end of the list 0.
bool EqObj::is_ship_queued(unsigned int ShipId) const
{
    for (std::list<FDockRequest>::const_iterator It = DockQueue.begin(); It != DockQueue.end(); ++It)
    {
        if (It->ShipId == ShipId)
            return true;
    }
    return false;
}

CEqObj* EqObj::ceqobj() const
{
    CObject* Obj = cobj();
    if (Obj == nullptr)
        return nullptr;
    // baseline's own narrowing test on this family is the ObjectClass mask, not
    // RTTI: Ship::create checks (ObjectClass & 0x503) == 0x503, Loot::create
    // (ObjectClass & 0x803) == 0x803. Everything CEqObj-derived carries 0x103.
    if ((static_cast<unsigned>(Obj->ObjectClass) & 0x103u) != 0x103u)
        return nullptr;
    return static_cast<CEqObj*>(Obj);
}

// (module storage, RET 0; only caller EqObj::destroy module storage):
// the CBase Object under the (ObjectClass & 3) == 3 guard, then CSimple+0xb4.
// baseline dereferences the null the guard produces; so does this.
static unsigned int EqObj_OwnerPlayer(EqObj const* Obj)
{
    return Obj->csimple()->OwnerPlayer;
}

// (module storage, RET 4).
bool EqObj::update(float Dt)
{
    // GameObject's gate (the routine, direct).
    if (!GameObject::update(Dt))
        return false;

    // the weapons-offline repair pass, only while
    // CEqObj+0x15c is set (ceqobj() is dereferenced without a null test).
    if (ceqobj()->bWeaponsOffline)
    {
        double const Now = Timing::read();               // / 06ce7e98
        DamageList Repair;                                // list ctor
        Repair.Cause = static_cast<DamageCause>(0x1a);

        // relative health <= 0.0 (TEST AH,0x41 /
        // JP skips > and unordered) and more than 60 s (the field, double)
        // since the last damage (TEST AH,0x41 / JNZ skips <= and unordered):
        // the root back at its max hit points.
        if (!(ceqobj()->get_relative_health() > 0.0) &&
            (Now - LastDamageTime) > 60.0)
        {
            Repair.add_damage_entry(SUBOBJ_ID_ROOT, ceqobj()->get_max_hit_pts(),
                                    DamageEntry::SubObjFate::None);
        }

        // every ServerEquip whose equip is a
        // disabled launcher (CEquip vt +0x10 IsDisabled) that has not been hit
        // for more than 60 s gets its max hit points back (vt +0x38).
        for (std::list<ServerEquip*>::iterator It = ServerEquips.begin();
             It != ServerEquips.end(); ++It)
        {
            CEquip* const Equip = (*It)->Equip;
            if (CELauncher::cast(Equip) == nullptr)
                continue;
            if (!Equip->IsDisabled())
                continue;
            double const Since = Now - static_cast<SrvLauncher*>(*It)->LastDamageTime;
            if (!(Since > 60.0))
                continue;
            Repair.add_damage_entry(Equip->Id, Equip->GetMaxHitPoints(), DamageEntry::SubObjFate::None);
        }

        // only a non-empty list (Count, +0x08).
        if (Repair.Count != 0)
            apply_damage_list(Repair);                    // vt +0x188
    }

    // CALL the routine -- the dock-bay tick.
    process_dock_queue(Dt);

    // every ServerEquip's update (slot 2, +0x08).
    for (std::list<ServerEquip*>::iterator It = ServerEquips.begin();
         It != ServerEquips.end(); ++It)
        (*It)->update(Dt);

    // FLD/FADD/FST into +0x6c, compared with 1.0f
    // (the field; TEST AH,0x41 / JNZ skips <= and unordered).
    CargoDecayTimer = Dt + CargoDecayTimer;
    if (CargoDecayTimer > 1.0f)
    {
        decay_cargo(CargoDecayTimer);                     // vt +0x24c
        CargoDecayTimer = 0.0f;
    }

    // Fuse::Burn on every lit fuse.
    for (std::list<ServerFuse>::iterator It = Fuses.begin(); It != Fuses.end(); ++It)
        It->Burn(Dt);

    // NEG/SBB/INC: !is_destroyed().
    return !is_destroyed();
}

// +0x158 -- module storage (RET 8).
bool EqObj::destroy(int Kind, unsigned int ObjId)
{
    // the first destroy only: latch +0x88.
    if (!bDeathFuseLit)
    {
        bDeathFuseLit = 1;

        if (csimple()->OwnerPlayer == 0)
        {
            // an AI object killed outright (kind 1) first
            // gets its death fuse, if the controller hands out a lifetime.
            if (Kind == 1)
            {
                // controller event 0x38.
                FDeathFuseEvent Ev;
                Ev.SystemId = 0;
                Ev.ObjId = 0;
                Ev.ObjType = 0;
                Ev.Lifetime = 0.0f;
                Ev.SystemId = OwnerSystem->SystemInfo->NicknameId;
                Ev.ObjId = get_id();                                    // vt +0x20
                get_type(Ev.ObjType);                                   // vt +0x88
                Server_ControllerDispatch(0, pub::Controller::EMessage::DeathFuseLit, &Ev);

                // Lifetime > 0.0f (TEST AH,0x41 /
                // JNZ skips <= and unordered).
                if (Ev.Lifetime > 0.0f)
                {
                    // light_fuse(ObjId,
                    // DEATH_COMM_FUSE, SUBOBJ_ID_NONE, 0.0f, Lifetime), vt +0x1e4.
                    if (light_fuse(ObjId, DEATH_COMM_FUSE, SUBOBJ_ID_NONE, 0.0f, Ev.Lifetime))
                    {
                        // ceqobj() (no null test on
                        // the narrowing) -> get_behavior_interface; with a manager,
                        // {7, &get_id()} through its slot 0 (Common the branch,
                        // the port's virtual IBehaviorManager::Slot00).
                        IBehaviorManager* const Behavior = ceqobj()->get_behavior_interface();
                        if (Behavior != nullptr)
                        {
                            unsigned int SelfId = get_id();         // vt +0x20
                            FBehaviorManagerEvent Event;
                            Event.Type = 7;
                            Event.Data = &SelfId;
                            Behavior->Slot00(&Event);
                        }

                        // once per DockQueue entry.
                        for (std::list<FDockRequest>::iterator It = DockQueue.begin();
                             It != DockQueue.end(); ++It)
                            OwnerSystem->remove_from_list_d4(this);

                        // CEqObj::disconnect(IObjRW*)
                        // (CEqObj vt +0xc0).
                        ceqobj()->disconnect(static_cast<IObjRW*>(this));
                        // XOR AL,AL -- not destroyed yet: the
                        // fuse will do it.
                        return false;
                    }
                }
            }
        }
        else if (!SinglePlayer() && Kind == 1 && is_player())
        {
            // a player's ship died in multiplayer:
            // control message 0x14 with the player's id to every active player
            // (the routine), and ServerStats+0x10 counts it (module storage).
            Server_BroadcastGroupControl(0x14, EqObj_OwnerPlayer(this));
            ++GServerStats.Field_0x10;
        }
    }

    // CSimple::set_hit_pts(0) (vt +0x98).
    csimple()->set_hit_pts(0.0f);

    // controller event 0x39.
    FObjectDestroyedEvent Ev;
    Ev.ObjId = get_id();
    Ev.KillerId = ObjId;
    Ev.SystemId = OwnerSystem->SystemInfo->NicknameId;
    Ev.OwnerPlayer = csimple()->OwnerPlayer;
    Ev.Kind = Kind;
    Ev.Position = get_position();                                  // vt +0x00
    if (Kind == 1)
        collect_droppable_equip(Ev.DroppedEquip);                  // vt +0x1f4
    Server_ControllerDispatch(0, pub::Controller::EMessage::ObjectDestroyed, &Ev);


    OwnerSystem->on_object_destroyed(this, Kind);
    destroy_notify();                                              // vt +0x15c


    if (Kind == 1)
    {
        explode();                                                 // vt +0x1f0
        if (SinglePlayer() || csimple()->OwnerPlayer == 0)
            drop_loot(Ev.DroppedEquip);                            // vt +0x1f8
    }
    return true;
}

// +0x15c -- module storage (RET 0).
void EqObj::destroy_notify()
{
    // StarSystem the routine(this), once per
    // DockQueue entry.
    for (std::list<FDockRequest>::iterator It = DockQueue.begin(); It != DockQueue.end(); ++It)
        OwnerSystem->remove_from_list_d4(this);

    // CEqObj::disconnect(this) (vt +0xc0) unless
    // this is a player's object in single player.
    if (!(SinglePlayer() && csimple()->OwnerPlayer != 0))
        ceqobj()->disconnect(static_cast<IObjRW*>(this));

    // JMP the routine
    GameObject::destroy_notify();
}

// +0x1bc -- module storage (RET 0).
void EqObj::check_fuses()
{
    rebuild_server_equip();                                        // vt +0x1dc

    // the archetype's fuse list (Archetype::EqObj
    // +0x74 list, node {+8 Fuse, +0xc Value1, +0x10 Value2}): hull hit points <=
    // Value2 (TEST AH,0x41 / JP skips > and unordered) lights the fuse at Value1.
    CEqObj* const Ce = ceqobj();
    std::vector<Archetype::FGroupFuse> const& ArchFuses = *Ce->eqobjarch()->Fuses;
    for (std::vector<Archetype::FGroupFuse>::const_iterator It = ArchFuses.begin();
         It != ArchFuses.end(); ++It)
    {
        if (Ce->get_hit_pts() > It->Value2)
            continue;
        light_fuse(0, It->Fuse, SUBOBJ_ID_ROOT, It->Value1, -1.0f); // vt +0x1e4
    }

    // every arch group's own FuseIgnitions, same
    // test on the group's hit points, sub-object = the group's id.
    CArchGrpTraverser Walk;
    CArchGroupManager& Groups = Ce->ArchGroups;
    for (CArchGroup* Group = Groups.Traverse(Walk); Group != nullptr; Group = Groups.Traverse(Walk))
    {
        std::vector<Archetype::FGroupFuse> const& GroupFuses = *Group->FuseIgnitions().Items;
        for (std::vector<Archetype::FGroupFuse>::const_iterator It = GroupFuses.begin();
             It != GroupFuses.end(); ++It)
        {
            if (Group->GetHitPoints() > It->Value2)
                continue;
            light_fuse(0, It->Fuse, Group->Arch->Id, It->Value1, -1.0f);
        }
    }
}

// +0x1e4 -- module storage (RET 0x14).
bool EqObj::light_fuse(unsigned int SourceId, ID_String const& FuseId, unsigned short SubObjId,
                       float AtT, float Lifetime)
{
    bool bFound = false;                                           // BL
    bool bIgnited = false;                                         // [ESP+0x13]

    // a copy of this fuse is already burning:
    // re-ignite it. The Lifetime argument is not applied here; baseline only
    // rewrites it to -1.0f (the field) unless it already is -1.0f, and that
    // value is what the broadcast below carries.
    for (std::list<ServerFuse>::iterator It = Fuses.begin(); It != Fuses.end() && !bFound; ++It)
    {
        if (It->Name().Id != FuseId.Id)
            continue;
        bFound = true;
        bIgnited = It->IgniteAt(SourceId, SubObjId, AtT);
        if (Lifetime != -1.0f)
            Lifetime = -1.0f;
    }

    if (!bFound)
    {
        // the prototype from ServerFuseDB
        // (the routine / the routine map find). baseline reads the value slot of
        // whatever node find() returns and tests it for null; the port reads it
        // only for a real node.
        std::map<unsigned, ServerFuse*>& Db = ServerFuseDB::Get();
        std::map<unsigned, ServerFuse*>::iterator Found = Db.find(FuseId.Id);
        ServerFuse* const Proto = (Found != Db.end()) ? Found->second : nullptr;
        if (Proto == nullptr)
        {

            FDUMP(0x100002, "%s(%d) : *** WARNING: Fuse(%s) not found in FuseDB. Aborting.",
                  "Server\\Space\\EqObj.cpp", 0xc73, FuseId.get_string());
        }
        else
        {
            bFound = true;
            bIgnited = true;                                       // [ESP+0x13] = 1

            // a stack copy, list insert at end()
            // (the routine), the stack copy destroyed.
            {
                ServerFuse Copy(*Proto);
                Fuses.push_back(Copy);
            }
            ServerFuse& Lit = Fuses.back();

            Lit.SetOwner(this);
            Lit.RandomizeActions();

            if (!bDeathFuseLit && Lit.bHasDestroyRoot)
            {
                // the first fuse that destroys the
                // root: controller event 0x38.
                bDeathFuseLit = 1;
                FDeathFuseEvent Ev;
                Ev.SystemId = 0;
                Ev.ObjId = 0;
                Ev.ObjType = 0;
                Ev.Lifetime = 0.0f;
                Ev.SystemId = OwnerSystem->SystemInfo->NicknameId;
                Ev.ObjId = get_id();
                get_type(Ev.ObjType);
                Server_ControllerDispatch(0, pub::Controller::EMessage::DeathFuseLit, &Ev);

                // the fuse's own lifetime below the
                // controller's (TEST AH,5 / JP skips >= and unordered): take the
                // controller's, and it replaces the Lifetime argument as well.
                if (Lit.Lifetime() < Ev.Lifetime)
                {
                    Lifetime = Ev.Lifetime;
                    Lit.OverrideLifetime(Ev.Lifetime);
                }
            }
            else if (Lifetime != -1.0f)
            {

                Lit.OverrideLifetime(Lifetime);
            }

            // the result is not kept.
            Lit.IgniteAt(SourceId, SubObjId, AtT);
        }
    }

    // tell the system (the routine).
    if (OwnerSystem != nullptr && bIgnited)
    {
        FServerBurnFuseInfo Info;
        Info.SourceId = SourceId;
        Info.ObjectId = get_id();
        Info.FuseId = FuseId.Id;
        Info.SubObjId = SubObjId;
        Info.Pad_0x0e = 0;
        Info.AtT = AtT;
        Info.Lifetime = Lifetime;
        Info.bLit = 1;
        Info.Pad_0x19[0] = 0;
        Info.Pad_0x19[1] = 0;
        Info.Pad_0x19[2] = 0;
        OwnerSystem->broadcast_fuse(Info);
    }
    return bFound;
}

// +0x1e8 -- module storage (RET 0xc).
bool EqObj::unlight_fuse(ID_String const& FuseId, unsigned short SubObjId, float AtT)
{
    // the first burning copy of FuseId only (the
    // loop ends once one matched; baseline keeps the flag in the SubObjId
    // argument's stack slot).
    bool bFound = false;
    for (std::list<ServerFuse>::iterator It = Fuses.begin(); It != Fuses.end() && !bFound; ++It)
    {
        if (It->Name().Id != FuseId.Id)
            continue;
        bFound = true;
        if (!It->UnBurn(SubObjId, AtT))
            continue;
        if (OwnerSystem == nullptr)
            continue;

        // {0, id, fuse, SubObjId, AtT, -1.0f, 0}.
        FServerBurnFuseInfo Info;
        Info.SourceId = 0;
        Info.ObjectId = get_id();
        Info.FuseId = FuseId.Id;
        Info.SubObjId = SubObjId;
        Info.Pad_0x0e = 0;
        Info.AtT = AtT;
        Info.Lifetime = -1.0f;
        Info.bLit = 0;
        Info.Pad_0x19[0] = 0;
        Info.Pad_0x19[1] = 0;
        Info.Pad_0x19[2] = 0;
        OwnerSystem->broadcast_fuse(Info);
    }
    return bFound;
}

// +0x1ec -- module storage (RET 0). A death fuse (Fuse+0x0c) that is not spent.
// IObjInspect slot 14 (+0x38) -- module storage, a two-instruction tail jump
// through vt +0x1ec. baseline's slot is not const-qualified; the const_cast only
// bridges the port's const IObjInspect declaration.
bool EqObj::is_dying() const
{
    return const_cast<EqObj*>(this)->is_death_fuse_burning();
}

bool EqObj::is_death_fuse_burning()
{
    for (std::list<ServerFuse>::iterator It = Fuses.begin(); It != Fuses.end(); ++It)
    {
        if (It->IsDeathFuse() && !It->IsSpent())
            return true;
    }
    return false;
}

// +0x188 -- module storage (RET 4).
void EqObj::apply_damage_list(DamageList& Damage)
{
    // re-entry is reported, not refused.
    if (is_in_damage_callback())
    {
        FDUMP(0x100001,
              "%s(%d) : *** ERROR: ID(0x%X) Arch(%s): Tried to apply damage of type %s within another damage callback.",
              "Server\\Space\\EqObj.cpp", 0x9d7,
              get_id(), Object->get_archetype()->Nickname.get_string(),
              DamageList::DmgCauseToString(Damage.Cause));
    }

    set_in_damage_callback(true);                                  // vt +0x190

    // the time of the last damage, while the hull
    // is still above 0.0 (the field; TEST AH,0x41 / JNZ skips <=).
    if (ceqobj()->get_relative_health() > 0.0)
        LastDamageTime = Timing::read();

    // a non-empty list only.
    if (Damage.Count != 0)
    {
        for (std::list<DamageEntry>::iterator It = Damage.Entries.begin();
             It != Damage.Entries.end(); ++It)
        {
            unsigned short const Id = It->SubObjId;
            if (Id == SUBOBJ_ID_ROOT)
                apply_root_entry(Damage, *It);                     // vt +0x234
            else if (Id == SUBOBJ_ID_POWER)
                apply_power_entry(Damage, *It);                    // vt +0x23c
            else if (SubObjectID::IsArchGroupID(Id))
                apply_group_entry(Damage, *It);                    // vt +0x240
            else if (SubObjectID::IsEquipID(Id))
                apply_equip_entry(Damage, *It);                    // vt +0x238
        }

        // outside single player the dead
        // sub-objects go at once: CEqObj::destroy_sub_objs(Damage, false)
        // (vt +0xfc) and CEquipManager::CleanUp.
        if (!SinglePlayer())
        {
            CEqObj* const Ce = ceqobj();
            Ce->destroy_sub_objs(Damage, false);
            Ce->EquipManager.CleanUp();
        }

        StarSystem_DamageObject(OwnerSystem, this, &Damage);
    }

    // destroyed, not blocked, no death fuse
    // holding a cause below 0x10, not already destroyed, and CEqObj+0x15c clear.
    if (Damage.bDestroyed &&
        !is_damage_blocked(Damage) &&                              // vt +0x1b8
        !(is_death_fuse_burning() && static_cast<int>(Damage.Cause) < 0x10) &&
        !is_destroyed() &&                                         // vt +0x160
        !ceqobj()->bWeaponsOffline)
    {
        destroy(1, Damage.InflictorId);                            // vt +0x158
    }

    set_in_damage_callback(false);                                 // vt +0x190
}

// The light/unlight pass apply_root_entry (module storage .. 06ceb785) and
// apply_group_entry (module storage .. 06cebcf6) share, over one fuse list:
// damage (hit points dropping from Before to After) lights every fuse whose
// threshold Value2 lies in [After, Before); healing (Before <= After) unlights
// every fuse whose threshold lies in [Before, After). Before is re-read for
// every node (the light_fuse / unlight_fuse calls may change it).
//   damage:  Before > After (TEST AH,0x41 / JNZ), Before > Value2 (same),
//            !(Value2 < After) (TEST AH,1 / JNZ) -> light_fuse(InflictorId,
//            Fuse, SubObjId, Value1, -1.0f), vt +0x1e4;
//   healing: Value2 < After (TEST AH,5 / JP), Before <= Value2 (TEST AH,0x41 /
//            JP) -> unlight_fuse(Fuse, SubObjId, Value1), vt +0x1e8.
template <typename FReadHitPts>
static void EqObj_SweepFuses(EqObj* Obj, std::vector<Archetype::FGroupFuse> const& List,
                             FReadHitPts ReadHitPts, float After, unsigned short SubObjId,
                             unsigned int InflictorId)
{
    for (std::vector<Archetype::FGroupFuse>::const_iterator It = List.begin(); It != List.end(); ++It)
    {
        if (ReadHitPts() > After)
        {
            if (!(ReadHitPts() > It->Value2))
                continue;
            // TEST AH,1 / JNZ skips "less" and unordered alike.
            if (!(It->Value2 >= After))
                continue;
            Obj->light_fuse(InflictorId, It->Fuse, SubObjId, It->Value1, -1.0f);
        }
        else
        {
            if (!(It->Value2 < After))
                continue;
            // TEST AH,0x41 / JP skips "greater" and unordered alike.
            if (!(ReadHitPts() <= It->Value2))
                continue;
            Obj->unlight_fuse(It->Fuse, SubObjId, It->Value1);
        }
    }
}

// The behavior-manager half of the four apply_*_entry slots: with a manager,
// the controller event 0xd record re-ordered into FBehaviorManagerEvent {0,
// &Record} and handed to its slot 0 (Common the branch, the port's
// virtual IBehaviorManager::Slot00). EquipType and CollisionGroupType are
// passed in because each site writes one of them from the event and zeroes the
// other with an immediate (root module storage / 06ceb668, equip module storage,
// group module storage, power module storage).
static void EqObj_NotifyBehaviorOfDamage(IBehaviorManager* Behavior, FSubObjDamageEvent const& Ev,
                                         unsigned int EquipType, unsigned int CollisionGroupType)
{
    if (Behavior == nullptr)
        return;
    FBehaviorDamageEvent Record;
    Record.SystemId = Ev.SystemId;
    Record.InflictorId = Ev.InflictorId;
    Record.ObjId = Ev.ObjId;
    Record.EquipType = EquipType;
    Record.CollisionGroupType = CollisionGroupType;
    Record.Cause = static_cast<int>(Ev.Cause);
    Record.Damage = Ev.Damage;
    Record.RelativeDamage = Ev.RelativeDamage;
    Record.RelativeHealth = Ev.RelativeHealth;
    FBehaviorManagerEvent Event;
    Event.Type = 0;
    Event.Data = &Record;
    Behavior->Slot00(&Event);
}

// +0x234 -- module storage (RET 8).
void EqObj::apply_root_entry(DamageList const& Damage, DamageEntry const& Entry)
{
    CEqObj* const Ce = ceqobj();

    // controller event 0xd.
    FSubObjDamageEvent Ev;
    Ev.ObjId = 0;
    Ev.SystemId = 0;
    Ev.InflictorId = 0;
    Ev.EquipType = 0;
    Ev.CollisionGroupType = 0;
    Ev.Cause = static_cast<DamageCause>(0);
    Ev.Damage = 0.0f;
    Ev.RelativeDamage = 0.0f;
    Ev.RelativeHealth = 0.0f;
    Ev.SystemId = OwnerSystem->SystemInfo->NicknameId;
    Ev.InflictorId = Damage.InflictorId;
    Ev.ObjId = get_id();
    Ev.Cause = Damage.Cause;
    Ev.Damage = Ce->get_hit_pts() - Entry.HitPts;
    // Root::HitPts (+0x1c), no zero test.
    Ev.RelativeDamage = Ev.Damage / Ce->Archetype->HitPts;
    Ev.RelativeHealth = Entry.HitPts / Ce->Archetype->HitPts;
    Server_ControllerDispatch(0, pub::Controller::EMessage::ObjectDamaged, &Ev);

    // the behavior manager hears {0, &Record},
    // Record = {SystemId, InflictorId, ObjId, EquipType 0, CollisionGroupType 0,
    // Cause, Damage, RelativeDamage, RelativeHealth}.
    EqObj_NotifyBehaviorOfDamage(Ce->get_behavior_interface(), Ev, 0, 0);

    // the archetype's fuses, when the hull's hit
    // points change (TEST AH,0x44 / JNP skips equal).
    if (Ce->get_hit_pts() != Entry.HitPts)
    {
        EqObj_SweepFuses(this, *ceqobj()->eqobjarch()->Fuses,
                         [Ce]() { return Ce->get_hit_pts(); },
                         Entry.HitPts, SUBOBJ_ID_ROOT, Damage.InflictorId);
    }

    // CALL the routine -- direct, not through the vtable.
    GameObject::apply_root_damage(Entry);
}

// +0x238 -- module storage (RET 8).
void EqObj::apply_equip_entry(DamageList const& Damage, DamageEntry const& Entry)
{
    CEqObj* const Ce = ceqobj();


    CEquip* const Equip = Ce->EquipManager.FindByID(Entry.SubObjId);
    if (Equip == nullptr || Equip->IsDestroyed())
        return;

    // a live launcher (relative health > 0.0,
    // TEST AH,0x41 / JNZ skips <=) stamps the time on its ServerEquip, found by
    // a second FindByID and a walk of the +0x70 list.
    if (Equip->GetRelativeHealth() > 0.0 && CELauncher::cast(Equip) != nullptr)
    {
        CEquip* const Again = ceqobj()->EquipManager.FindByID(Entry.SubObjId);
        for (std::list<ServerEquip*>::iterator It = ServerEquips.begin();
             It != ServerEquips.end(); ++It)
        {
            if ((*It)->Equip != Again)
                continue;
            SrvLauncher* const Launcher = static_cast<SrvLauncher*>(*It);
            if (Launcher != nullptr)
                Launcher->LastDamageTime = Timing::read();
            break;
        }
    }

    // controller event 0xd.
    FSubObjDamageEvent Ev;
    Ev.ObjId = 0;
    Ev.SystemId = 0;
    Ev.InflictorId = 0;
    Ev.EquipType = 0;
    Ev.CollisionGroupType = 0;
    Ev.Cause = static_cast<DamageCause>(0);
    Ev.Damage = 0.0f;
    Ev.RelativeDamage = 0.0f;
    Ev.RelativeHealth = 0.0f;
    Ev.SystemId = OwnerSystem->SystemInfo->NicknameId;
    Ev.InflictorId = Damage.InflictorId;
    Ev.ObjId = get_id();
    Ev.Cause = Damage.Cause;
    Ev.EquipType = Equip->Type;
    Ev.Damage = Equip->GetHitPoints() - Entry.HitPts;             // vt +0x3c
    float const Max = Equip->GetMaxHitPoints();                    // vt +0x38
    // TEST AH,0x44 / JNP: a zero max gives 0 for both ratios.
    if (Max != 0.0f)
        Ev.RelativeDamage = Ev.Damage / Max;
    else
        Ev.RelativeDamage = 0.0f;
    if (Max != 0.0f)
        Ev.RelativeHealth = Entry.HitPts / Max;
    else
        Ev.RelativeHealth = 0.0f;
    Server_ControllerDispatch(0, pub::Controller::EMessage::ObjectDamaged, &Ev);

    // behavior record as apply_root_entry's, with
    // EquipType (module storage) and CollisionGroupType 0.
    EqObj_NotifyBehaviorOfDamage(Ce->get_behavior_interface(), Ev, Ev.EquipType, 0);

    // a shield-battery list (cause 0x13) boosts
    // the shield instead of setting its hit points.
    if (static_cast<int>(Damage.Cause) == 0x13)
    {
        static_cast<CEShield*>(Equip)->ShieldBatteryBoost(Entry.HitPts);
        return;
    }

    // a cargo stack (type 0x10000, Count +0x28 > 1
    // unsigned) loses one unit when its entry reaches 0 (TEST AH,0x44 / JP
    // skips unequal), otherwise takes the hit points.
    if (Equip->Type == 0x10000 && static_cast<CECargo*>(Equip)->Count > 1)
    {
        if (Entry.HitPts == 0.0f)
        {
            static_cast<CECargo*>(Equip)->RemoveFromStack(1);
            return;
        }
        Equip->SetHitPoints(Entry.HitPts);                         // vt +0x40
        return;
    }

    Equip->SetHitPoints(Entry.HitPts);                             // vt +0x40
}

// +0x23c -- module storage (RET 8).
void EqObj::apply_power_entry(DamageList const& Damage, DamageEntry const& Entry)
{
    CEqObj* const Ce = ceqobj();

    // controller event 0xd; EquipType 0x40000,
    // the ratios over CEqObj::MaxPower (+0x19c) with no zero test (FDIVR).
    FSubObjDamageEvent Ev;
    Ev.ObjId = 0;
    Ev.SystemId = 0;
    Ev.InflictorId = 0;
    Ev.EquipType = 0;
    Ev.CollisionGroupType = 0;
    Ev.Cause = static_cast<DamageCause>(0);
    Ev.Damage = 0.0f;
    Ev.RelativeDamage = 0.0f;
    Ev.RelativeHealth = 0.0f;
    Ev.SystemId = OwnerSystem->SystemInfo->NicknameId;
    Ev.InflictorId = Damage.InflictorId;
    Ev.ObjId = get_id();
    Ev.EquipType = 0x40000;
    Ev.Cause = Damage.Cause;
    Ev.Damage = Ce->Power - Entry.HitPts;
    Ev.RelativeDamage = Ev.Damage / Ce->MaxPower;
    Ev.RelativeHealth = Entry.HitPts / Ce->MaxPower;
    Server_ControllerDispatch(0, pub::Controller::EMessage::ObjectDamaged, &Ev);

    // behavior record with EquipType 0x40000 (from
    // the event, module storage) and CollisionGroupType 0.
    EqObj_NotifyBehaviorOfDamage(Ce->get_behavior_interface(), Ev, Ev.EquipType, 0);

    Ce->Power = Entry.HitPts;
}

// +0x240 -- module storage (RET 8).
void EqObj::apply_group_entry(DamageList const& Damage, DamageEntry& Entry)
{
    CEqObj* const Ce = ceqobj();

    // no null test on the group.
    CArchGroup* const Group = Ce->ArchGroups.FindByID(Entry.SubObjId);

    // a group that dies empties the dock queue
    // (the routine, list erase(begin, end)); a persistent fate is reported.
    if (Entry.Fate != DamageEntry::SubObjFate::None)
    {
        if (ceqobj() != nullptr)
            DockQueue.clear();
        if (static_cast<int>(Entry.Fate) == 4)
        {
            FDUMP(0x100002, "%s(%d) : *** WARNING: %s",
                  "Server\\Space\\EqObj.cpp", 0xaf5,
                  "Persistent CArchGroups not supported.");
        }
    }

    // controller event 0xd.
    FSubObjDamageEvent Ev;
    Ev.ObjId = 0;
    Ev.SystemId = 0;
    Ev.InflictorId = 0;
    Ev.EquipType = 0;
    Ev.CollisionGroupType = 0;
    Ev.Cause = static_cast<DamageCause>(0);
    Ev.Damage = 0.0f;
    Ev.RelativeDamage = 0.0f;
    Ev.RelativeHealth = 0.0f;
    Ev.SystemId = OwnerSystem->SystemInfo->NicknameId;
    Ev.InflictorId = Damage.InflictorId;
    Ev.ObjId = get_id();
    Ev.CollisionGroupType = Group->GetCollisionGroupType();
    Ev.Cause = Damage.Cause;
    Ev.Damage = Group->GetHitPoints() - Entry.HitPts;
    // CArchGroup::GetMaxHitPoints stays in ST0 unrounded: both
    // zero tests (06cebb62 / 06cebb83 FCOM float 0.0, TEST AH,0x44 / JNP ->
    // zero only when equal) and both FDIV ST0,ST1 use it; only the two
    // quotients are stored as float (06cebb75 / 06cebb95).
    double const Max = Group->GetMaxHitPoints();
    if (Max != 0.0)
        Ev.RelativeDamage = (float)(Ev.Damage / Max);
    else
        Ev.RelativeDamage = 0.0f;
    if (Max != 0.0)
        Ev.RelativeHealth = (float)(Entry.HitPts / Max);
    else
        Ev.RelativeHealth = 0.0f;
    Server_ControllerDispatch(0, pub::Controller::EMessage::ObjectDamaged, &Ev);

    // behavior record with EquipType 0 and the
    // event's CollisionGroupType (module storage).
    EqObj_NotifyBehaviorOfDamage(Ce->get_behavior_interface(), Ev, 0, Ev.CollisionGroupType);

    // the group's fuses when its hit points change.
    if (Group->GetHitPoints() != Entry.HitPts)
    {
        EqObj_SweepFuses(this, *Group->FuseIgnitions().Items,
                         [Group]() { return Group->GetHitPoints(); },
                         Entry.HitPts, Group->Arch->Id, Damage.InflictorId);
    }

    // a burning death fuse keeps low-cause groups
    // attached: the entry's fate is cleared.
    if (is_death_fuse_burning() && static_cast<int>(Damage.Cause) < 0x10)
        Entry.Fate = DamageEntry::SubObjFate::None;

    Group->SetHitPoints(Entry.HitPts);
}

// ---------------------------------------------------------------------------
// EqObj helpers shared by the damage / loot slots below.
// ---------------------------------------------------------------------------

// The "is this object a connected player's CURRENT ship" test baseline inlines
// at every difficulty-scale site (damage_hull module storage .. 06cea412,
// damage_equip module storage .. 06cea5b7, damage_shield module storage ..
// , damage_group module storage .. 06ceabf8, damage_power
// ): get_id() (vt +0x20) first, then CSimple+0xb4
// through the (ObjectClass & 3) == 3 guard (the null it can produce is
// dereferenced, as in baseline), PlayerDB::is_valid's three conditions inlined
// (owner > 0, owner - 1 < MaxPlayers, PlayerData+0x348 != 0), then
// PlayerData+0x3e4 against the id. Null when any of them fails.
static PlayerData* EqObj_PlayerOfShip(EqObj const* Obj)
{
    unsigned int const Id = Obj->get_id();
    unsigned int const Owner = Obj->csimple()->OwnerPlayer;
    if (!Players.is_valid(Owner))
        return nullptr;
    PlayerData& Data = Players[Owner];
    if (Data.ship_object_id != Id)
        return nullptr;
    return &Data;
}

// The raw Object under the (ObjectClass & 0x303) == 0x303 test -- baseline's
// inlined CSolar narrowing at the loot-visit sites (module storage,
// , module storage, ...). Null for everything that is not a solar.
static CSolar* EqObj_Solar(EqObj const* Obj)
{
    CObject* const Raw = Obj->cobj();
    if (Raw == nullptr)
        return nullptr;
    if ((static_cast<unsigned>(Raw->ObjectClass) & 0x303u) != 0x303u)
        return nullptr;
    return static_cast<CSolar*>(Raw);
}

// .data:the field -- EqObj.cpp's own lazily fetched math engine
// (module storage / module storage `MOV EAX,[the field]` / JNZ / CALL
// / store).
static I3DMathEngine* GEqObjMathEngine = nullptr;

// The ServerEquip listening to Equip: EqObj+0x74 (ServerEquips' head) walked
// from head->next, node+8's +4 compared with the CEquip (module storage ..
// , module storage... in the siblings). The walk steps with
// (the list iterator's post-increment). End of list -> nullptr
// and the "not found" flag; baseline tells the two apart only by the jump taken.
static bool EqObj_FindServerEquip(std::list<ServerEquip*>& ServerEquips, CEquip const* Equip,
                                  ServerEquip*& Out)
{
    for (std::list<ServerEquip*>::iterator It = ServerEquips.begin(); It != ServerEquips.end(); ++It)
    {
        if ((*It)->Equip == Equip)
        {
            Out = *It;
            return true;
        }
    }
    Out = nullptr;
    return false;
}

// IObjAffect slot 13 (+0x34) -- the routine (module storage, RET 0x10).
// Every requested equip is looked up (CEquipManager::FindByID on CEqObj+0xe4)
// and handed to its ServerEquip, which baseline calls as a SrvLauncher through
// slots 3/4/5 whatever it really is. Unguided guns that fired are collected and
// announced to the system in one StarSystem::notify_fire_weapon.
int EqObj::slot_34(unsigned short const* EquipIds, unsigned Count, Vector const* TargetPos,
                   int SourceClient)
{
    // [this+0x24] (OwnerSystem) null or
    // is_destroyed (vt +0x160): -5.
    if (OwnerSystem == nullptr)
        return -5;
    if (is_destroyed())
        return -5;

    // the fired-id vector, Result = 0, the lock
    // target (+0x14) = 0, its sub-id (+0x28) = SUBOBJ_ID_NONE.
    std::vector<unsigned short> Fired;
    int Result = 0;
    CSimple* LockTarget = nullptr;
    unsigned short LockSubId = SUBOBJ_ID_NONE;

    for (unsigned Index = 0; Index < Count; ++Index)                // JC
    {
        // ceqobj() through the (& 0x103) guard,
        // no null test, then FindByID.
        CEquip* const Found = ceqobj()->EquipManager.FindByID(EquipIds[Index]);

        ServerEquip* Listener = nullptr;
        if (!EqObj_FindServerEquip(ServerEquips, Found, Listener) || Listener == nullptr)
        {
            // / 06ce6a9b -- no listener: -2, next id.
            Result = -2;
            continue;
        }

        // a listener whose equip is gone: next id.
        CEquip* const Equip = Listener->Equip;
        if (Equip == nullptr)
            continue;

        SrvLauncher* const Launcher = static_cast<SrvLauncher*>(Listener);
        bool bFired;
        if (SourceClient != -1 && SourceClient != 0)
        {
            // a client's shot: slot 5 fire_unchecked.
            bFired = Launcher->fire_unchecked(*TargetPos);
        }
        else
        {
            // CELauncher::cast (not null-tested);
            // with an aim point and a projectile without ForceGunOri (+0x79),
            // slot 3 fire(*TargetPos) and on to the fired test.
            CELauncher* const Cast = CELauncher::cast(Equip);
            if (TargetPos != nullptr &&
                !static_cast<Archetype::Projectile const*>(Cast->Projectile)->bForceGunOri)
            {
                bFired = Launcher->fire(*TargetPos);
            }
            else
            {
                // slot 4 fire_forward(SourceClient);
                // its answer is not read, next id.
                Launcher->fire_forward(SourceClient);
                continue;
            }
        }

        // only a gun that fired an unguided
        // munition is collected.
        if (!bFired)
            continue;
        CEGun* const Gun = CEGun::cast(Equip);
        if (Gun == nullptr)
            continue;
        if (Gun->MunitionArch()->Seeker != 0)
            continue;

        // vector<unsigned short>::insert(end, 1, id)
        // (Common import).
        Fired.insert(Fired.end(), 1, Gun->Id);

        // until one is found, CEGun::GetTarget.
        if (LockTarget == nullptr)
            Gun->GetTarget(LockTarget, LockSubId);
    }

    // nothing collected: straight to the cleanup.
    if (!Fired.empty())
    {
        // the aim point, read without a null test.
        Vector Aim = *TargetPos;

        // outside single player with a lock
        // target: its Transform (target+8, 12 dwords REP MOVSD) and the lazily
        // fetched EqObj.cpp math engine (the field), InverseTransform (+0x3c)
        // of *TargetPos into the target's frame.
        if (!SinglePlayer() && LockTarget != nullptr)
        {
            FTransform const TargetXform = *reinterpret_cast<FTransform const*>(&LockTarget->Orientation);
            if (GEqObjMathEngine == nullptr)
                GEqObjMathEngine = Server_Acquire3DMathEngine();
            FVector Local;
            GEqObjMathEngine->InverseTransform(Local, TargetXform,
                                               *reinterpret_cast<FVector const*>(TargetPos));
            Aim = reinterpret_cast<Vector const&>(Local);
        }

        // get_id (vt +0x20), then
        // OwnerSystem->the routine(&id, Fired, &Aim, SourceClient).
        unsigned int const ObjId = get_id();
        OwnerSystem->notify_fire_weapon(ObjId, Fired, Aim, SourceClient);
    }

    // the vector's buffer freed; Result.
    return Result;
}

// IObjRW::cobject() (vt +0x150) under the (ObjectClass & 0x803) == 0x803 test --
// the loot narrowing both tractor slots inline (module storage .. 06ce6c6d,
// ). Null when it is not loot.
static CLoot* EqObj_LootOf(IObjRW* Obj)
{
    CObject const* const Raw = Obj->cobject();
    if (Raw == nullptr)
        return nullptr;
    if ((static_cast<unsigned>(Raw->ObjectClass) & 0x803u) != 0x803u)
        return nullptr;
    return const_cast<CLoot*>(static_cast<CLoot const*>(Raw));
}

// IObjAffect slot 16 (+0x40) -- the routine (module storage, RET 0xc).
int EqObj::slot_40(unsigned short const* EquipId, IObjRW* Target, int SourceClient)
{
    // no system / destroyed: -5.
    if (OwnerSystem == nullptr)
        return -5;
    if (is_destroyed())
        return -5;

    // no target: -4.
    if (Target == nullptr)
        return -4;

    // Result (the Target slot, reused) = 0; an AI
    // request (-1) is treated as the server's own (0).
    int Result = 0;
    if (SourceClient == -1)
        SourceClient = 0;

    // single player lets a client's request
    // straight through to the broadcast; otherwise the tractor is driven here.
    if (!SinglePlayer() || SourceClient == 0)
    {
        // FindByID on ceqobj() (no null test).
        CEquip* const Found = ceqobj()->EquipManager.FindByID(*EquipId);
        if (Found == nullptr)
        {
            Result = -2;
        }
        else
        {
            // CETractor::AddTarget on the equip as
            // found (baseline does not cast it); any refusal is -5.
            if (static_cast<CETractor*>(Found)->AddTarget(EqObj_LootOf(Target)) != TractorOk)
                Result = -5;
        }

        // the server's own request stops here
        // unless the tractor took.
        if (SourceClient == 0 && Result != 0)
            return Result;
    }

    // XTractorObjects {get_id(), *EquipId,
    // {Target->get_id()}}: the id vector grown by the routine (insert at end, 1).
    XTractorObjects Info;
    Info.ObjectId = get_id();
    Info.TractorEquipId = *EquipId;
    Info.TargetIds.insert(Info.TargetIds.end(), 1, Target->get_id());

    // OwnerSystem->the routine(&SourceClient, &Info).
    unsigned int const SkipPlayerId = static_cast<unsigned int>(SourceClient);
    OwnerSystem->send_tractor_objects(SkipPlayerId, Info);

    // the buffer freed; Result.
    return Result;
}

// IObjAffect slot 15 (+0x3c) -- the routine (module storage, RET 0xc).
int EqObj::slot_3c(unsigned short const* EquipId, std::vector<IObjRW*> const& Targets,
                   int SourceClient)
{
    // no system / destroyed: -5.
    if (OwnerSystem == nullptr)
        return -5;
    if (is_destroyed())
        return -5;


    int Result = 0;
    if (SourceClient == -1)
        SourceClient = 0;


    if (!SinglePlayer() || SourceClient == 0)
    {
        // FindByID, then CETractor::cast.
        CETractor* const Tractor = CETractor::cast(ceqobj()->EquipManager.FindByID(*EquipId));
        if (Tractor == nullptr)
        {
            Result = -2;
        }
        else
        {
            // every non-null target through
            // AddTarget; the ones that took (0) are counted. None: -4.
            int Took = 0;
            for (std::vector<IObjRW*>::const_iterator It = Targets.begin(); It != Targets.end(); ++It)
            {
                if (*It == nullptr)
                    continue;
                if (Tractor->AddTarget(EqObj_LootOf(*It)) == TractorOk)
                    ++Took;
            }
            if (Took == 0)
                Result = -4;
        }


        if (SourceClient == 0 && Result != 0)
            return Result;
    }

    // XTractorObjects {get_id(), *EquipId, every
    // target's get_id()} -- here without a null test on the targets.
    XTractorObjects Info;
    Info.ObjectId = get_id();
    Info.TractorEquipId = *EquipId;
    for (std::vector<IObjRW*>::const_iterator It = Targets.begin(); It != Targets.end(); ++It)
        Info.TargetIds.insert(Info.TargetIds.end(), 1, (*It)->get_id());

    // OwnerSystem->the routine(&SourceClient, &Info).
    unsigned int const SkipPlayerId = static_cast<unsigned int>(SourceClient);
    OwnerSystem->send_tractor_objects(SkipPlayerId, Info);


    return Result;
}

// IObjAffect slot 17 (+0x44) -- the branch (module storage, RET 0x10).
int EqObj::slot_44(unsigned short const* EquipId, IObjRW* Target, unsigned short SubTarget,
                   int SourceClient)
{
    // no system / destroyed: -5.
    if (OwnerSystem == nullptr)
        return -5;
    if (is_destroyed())
        return -5;

    // Result lives in ESI from here on.
    int Result = 0;
    if (SourceClient == -1)
        SourceClient = 0;

    // BL, "the gun's target changed".
    bool bChanged = false;
    if (!SinglePlayer() || SourceClient == 0)
    {
        // FindByID on ceqobj().
        CEquip* const Found = ceqobj()->EquipManager.FindByID(*EquipId);
        if (Found == nullptr || Found->IsDestroyed())               // vt +0x08
        {
            Result = -2;
        }
        else
        {
            CEGun* const Gun = CEGun::cast(Found);
            if (Gun == nullptr)
            {
                Result = -4;
            }
            else
            {
                // Target->cobject() through the
                // (& 3) guard (null target -> null), CEGun::SetTarget.
                CSimple* Simple = nullptr;
                if (Target != nullptr)
                {
                    CObject const* const Raw = Target->cobject();
                    if (Raw != nullptr && (static_cast<unsigned>(Raw->ObjectClass) & 3u) == 3u)
                        Simple = const_cast<CSimple*>(static_cast<CSimple const*>(Raw));
                }
                bChanged = Gun->SetTarget(Simple, SubTarget);
            }
        }

        // the server's own request is only told
        // when the target really changed and nothing failed.
        if (SourceClient == 0 && (!bChanged || Result != 0))
            return Result;
    }

    // get_id(), then
    // OwnerSystem->the routine(&id, *EquipId, Target, &SubTarget, &SourceClient).
    unsigned int const ObjId = get_id();
    OwnerSystem->notify_set_target(ObjId, *EquipId, Target, SubTarget, SourceClient);
    return Result;
}

// IObjAffect slot 19 (+0x4c) -- the branch (module storage, RET 0xc).
int EqObj::slot_4c(unsigned short const* EquipId, bool bActivate, int SourceClient)
{
    // no system / destroyed: -5.
    if (OwnerSystem == nullptr)
        return -5;
    if (is_destroyed())
        return -5;


    int Result = 0;
    if (SourceClient == -1)
        SourceClient = 0;

    // [ESP+0xf], "the equip was switched".
    bool bChanged = false;
    if (!SinglePlayer() || SourceClient == 0)
    {
        // FindByID on ceqobj(); missing or
        // destroyed (vt +0x08): -2.
        CEquip* const Found = ceqobj()->EquipManager.FindByID(*EquipId);
        if (Found == nullptr || Found->IsDestroyed())
        {
            Result = -2;
        }
        else
        {
            bool bRefused = false;
            // switching an engine (type 0x20000)
            // off is refused (-5) while the ship strafes or has a thruster on;
            // the CShip is the (& 0x503) narrowing of the raw object, called
            // without a null test.
            if (!bActivate && Found->Type == 0x20000u)
            {
                CObject* const Raw = cobj();
                CShip* Ship = nullptr;
                if (Raw != nullptr && (static_cast<unsigned>(Raw->ObjectClass) & 0x503u) == 0x503u)
                    Ship = static_cast<CShip*>(Raw);
                if (Ship->get_strafe_dir() != 0 || Ship->any_thrusters_on())
                {
                    Result = -5;
                    bRefused = true;
                }
            }

            // IsActive (vt +0x04) already as asked:
            // nothing; otherwise Activate (vt +0x30) and remember it.
            if (!bRefused && Found->IsActive() != bActivate)
            {
                Found->Activate(bActivate);
                bChanged = true;
            }
        }


        if (SourceClient == 0 && (!bChanged || Result != 0))
            return Result;
    }

    // get_id(), then
    // OwnerSystem->the routine(&id, *EquipId, bActivate, &SourceClient).
    unsigned int const ObjId = get_id();
    OwnerSystem->notify_activate_equip(ObjId, *EquipId, bActivate, SourceClient);
    return Result;
}

// IObjAffect slot 22 (+0x58) -- the branch (module storage, RET 0xc).
// SourceClient is not read.
int EqObj::slot_58(unsigned short EquipId, unsigned Count, int SourceClient)
{
    (void)SourceClient;

    // no system / destroyed: -5.
    if (OwnerSystem == nullptr)
        return -5;
    if (is_destroyed())
        return -5;

    // EBP, the answer for an item that is neither kind.
    int Result = -5;

    // FindByID on ceqobj(), CECargo::cast.
    CECargo* const Cargo = CECargo::cast(ceqobj()->EquipManager.FindByID(EquipId));
    if (Cargo == nullptr)
    {

        FDUMP(0x100002, "%s(%d) : *** WARNING: %s", "Server\\Space\\EqObj.cpp",
              0x18c, "EqObj::use_item() couldn't find target item on Client request.");
        return -2;
    }

    // at most the stack's own count (unsigned).
    unsigned Used = Count;
    if (Cargo->Count < Used)
        Used = Cargo->Count;

    // get_class_type (vt +0x10) of the archetype
    // (+0x0c): repair kits and shield batteries; each answers how many it used,
    // and only a positive number is success.
    int const ClassType = static_cast<int>(Cargo->Arch->get_class_type());
    int Taken;
    if (ClassType == Archetype::AClassRepairKit)
    {
        Taken = use_repair_kits(EquipId, static_cast<Archetype::RepairKit const*>(Cargo->Arch),
                                static_cast<int>(Used));
    }
    else if (ClassType == Archetype::AClassShieldBattery)
    {
        Taken = use_shield_batteries(EquipId,
                                     static_cast<Archetype::ShieldBattery const*>(Cargo->Arch),
                                     static_cast<int>(Used));
    }
    else
    {
        return Result;
    }
    Result = Taken > 0 ? 0 : -5;
    if (Result != 0)
        return Result;

    // CECargo::RemoveFromStack, then
    // OwnerSystem->the routine(this, EquipId, Taken).
    Cargo->RemoveFromStack(static_cast<unsigned>(Taken));
    OwnerSystem->use_item(this, EquipId, Taken);
    return Result;
}

// (module storage, RET 0xc).
int EqObj::use_repair_kits(unsigned short EquipId, Archetype::RepairKit const* Kit, int Count)
{
    (void)EquipId;

    // a DamageList built inline (Buynode, count 0,
    // bDestroyed 0, cause 0x12, inflictor 0 / 0) that nothing ever fills; it is
    // only destroyed on the way out (module storage .. 06ce760d).
    DamageList Unused;
    Unused.Count = 0;
    Unused.bDestroyed = false;
    Unused.Cause = static_cast<DamageCause>(0x12);
    Unused.InflictorId = 0;
    Unused.InflictorOwner = 0;
    (void)Unused;

    int Taken = 0;                                                  // EBP

    // only a hull below full health (TEST AH,5 /
    // JP skips >= 1.0f and unordered) is mended: SUBOBJ_ID_ROOT, else NONE.
    unsigned short SubId = SUBOBJ_ID_NONE;
    if (ceqobj()->get_relative_health() < 1.0f)
        SubId = SUBOBJ_ID_ROOT;
    if (SubId == SUBOBJ_ID_NONE)
        return Taken;

    // the list that is applied: cause 0x12, this
    // object as inflictor, its CSimple::OwnerPlayer (the (& 3) guard's null
    // dereferenced) as the owner.
    DamageList Damage;
    Damage.Count = 0;
    Damage.bDestroyed = false;
    Damage.Cause = static_cast<DamageCause>(0x12);
    Damage.InflictorId = get_id();
    Damage.InflictorOwner = csimple()->OwnerPlayer;

    // the hull's own points through CSimple, any
    // other sub-object through CEqObj vt +0xe4 / +0xec.
    float HitPts;
    float MaxHitPts;
    if (SubId == SUBOBJ_ID_ROOT)
    {
        HitPts = ceqobj()->get_hit_pts();
        MaxHitPts = ceqobj()->get_max_hit_pts();
    }
    else
    {
        ceqobj()->get_sub_obj_hit_pts(SubId, HitPts);
        ceqobj()->get_sub_obj_max_hit_pts(SubId, MaxHitPts);
    }

    // kits needed = (max - hp) / Kit->HitPts on
    // the x87 stack, truncated by _ftol, rounded up when the remainder is above
    // 0.05f (TEST AH,0x41 / JNZ keeps <= and unordered).
    double const Needed = (static_cast<double>(MaxHitPts) - HitPts) / Kit->HitPts;
    Taken = static_cast<int>(Needed);
    if (Needed - static_cast<double>(Taken) > 0.05f)
        ++Taken;

    // no more than asked (signed).
    if (Count < Taken)
        Taken = Count;

    // mend: hp + Taken * Kit->HitPts, capped at the
    // max (TEST AH,5 / JP takes >= and unordered), into the list as a fate-0
    // entry, then apply_damage_list (vt +0x188).
    if (Taken > 0)
    {
        double const Mended = static_cast<double>(Taken) * Kit->HitPts + HitPts;
        float NewHitPts;
        if (Mended < static_cast<double>(MaxHitPts))
            NewHitPts = static_cast<float>(Mended);
        else
            NewHitPts = MaxHitPts;
        Damage.add_damage_entry(SubId, NewHitPts, DamageEntry::SubObjFate::None);
        apply_damage_list(Damage);
    }

    // both lists destroyed; the count.
    return Taken;
}

// (module storage, RET 0xc).
int EqObj::use_shield_batteries(unsigned short EquipId, Archetype::ShieldBattery const* Battery,
                                int Count)
{
    (void)EquipId;

    // CEquipTraverser(0x100) over the shields:
    // the one with the lowest relative health (vt +0x44) strictly below 1.0f
    // (TEST AH,5 / JP skips >= and unordered) whose max hit points (vt +0x38)
    // are above 0.0f (TEST AH,0x41 / JNZ skips <= and unordered).
    CEquipTraverser Walk(0x100);
    float Lowest = 1.0f;
    CEquip* Weakest = nullptr;
    for (;;)
    {
        CEquip* const Equip = ceqobj()->EquipManager.Traverse(Walk);
        if (Equip == nullptr)
            break;
        // FST float ptr [ESP + 0x10]   ; rounded copy kept
        // FCOMP float ptr [ESP + 0x14] ; compare the UNROUNDED ST0
        double const RelativeExact = Equip->GetRelativeHealth();
        float const Relative = (float)RelativeExact;
        if (!(RelativeExact < Lowest))
            continue;
        if (!(Equip->GetMaxHitPoints() > 0.0f))
            continue;
        Lowest = Relative;
        Weakest = Equip;
    }

    // nothing to charge: 0.
    if (Weakest == nullptr)
        return 0;

    // the list: cause 0x13, this object as the
    // inflictor, its CSimple::OwnerPlayer (guard null dereferenced) as the owner.
    DamageList Damage;
    Damage.Count = 0;
    Damage.bDestroyed = false;
    Damage.Cause = static_cast<DamageCause>(0x13);
    Damage.InflictorId = get_id();
    Damage.InflictorOwner = csimple()->OwnerPlayer;

    // GetHitPoints (vt +0x3c), GetMaxHitPoints (+0x38).
    float const HitPts = Weakest->GetHitPoints();
    float const MaxHitPts = Weakest->GetMaxHitPoints();

    // the same count as the repair kits'.
    double const Needed = (static_cast<double>(MaxHitPts) - HitPts) / Battery->HitPts;
    int Taken = static_cast<int>(Needed);
    if (Needed - static_cast<double>(Taken) > 0.05f)
        ++Taken;


    if (Count < Taken)
        Taken = Count;

    // charge the shield by its own id (+0x08).
    if (Taken > 0)
    {
        double const Charged = static_cast<double>(Taken) * Battery->HitPts + HitPts;
        float NewHitPts;
        if (Charged < static_cast<double>(MaxHitPts))
            NewHitPts = static_cast<float>(Charged);
        else
            NewHitPts = MaxHitPts;
        Damage.add_damage_entry(Weakest->Id, NewHitPts, DamageEntry::SubObjFate::None);
        apply_damage_list(Damage);
    }

    // the list destroyed; the count.
    return Taken;
}

// (module storage, RET 8). `MOV EAX,[ECX+0x10]` / the (& 0x103)
// test zeroes EAX on failure, then `MOV EDX,[EAX]` / `JMP [EDX+0xb4]` with both
// stack arguments passed through untouched -- no null test.
ObjActivateResult EqObj::activate(bool bOn, unsigned int DockIndex)
{
    return ceqobj()->activate(bOn, DockIndex);
}

// (module storage, RET 8).
void EqObj::queue_dock_request(int Port, unsigned int ShipId)
{
    // operator new(0x10) linked before the head
    // (push_back), node +8 = Port, +0xc = ShipId, ++[+0x68].
    FDockRequest Request;
    Request.Port = Port;
    Request.ShipId = ShipId;
    DockQueue.push_back(Request);

    // [this+0x28]->the routine(this).
    OwnerSystem->add_to_list_d4(this);
}

// (module storage, RET 8).
bool EqObj::dequeue_dock_request(int Port, unsigned int ShipId)
{
    // head->next onward: node+0xc == ShipId, then
    // `TEST EDX,EDX / JL` (Port < 0 matches any port) or node+8 == Port.
    for (std::list<FDockRequest>::iterator It = DockQueue.begin(); It != DockQueue.end(); ++It)
    {
        if (It->ShipId != ShipId)
            continue;
        if (Port >= 0 && It->Port != Port)
            continue;

        // unlink, operator delete (the routine),
        // --[+0x68].
        DockQueue.erase(It);

        // [this+0x28]->the routine(this); AL = 1.
        OwnerSystem->remove_from_list_d4(this);
        return true;
    }

    // no match: AL = 0, the system is not told.
    return false;
}

// (module storage, RET 4; Dt is not read). The dock-bay tick:
// every queued {port, ship} whose ship has reached the bay's docking sphere is
// handed on to the system, the rest wait.
void EqObj::process_dock_queue(float Dt)
{
    (void)Dt;

    // ceqobj() once, kept for the whole walk.
    CEqObj* const Ce = ceqobj();

    std::list<FDockRequest>::iterator It = DockQueue.begin();
    while (It != DockQueue.end())
    {
        // StarSystem the routine(ShipId, 0x7f0000).
        GameObject* const ShipObj = OwnerSystem->find_object(It->ShipId, 0x7f0000u);
        if (ShipObj == nullptr)
        {
            // the ship is gone: the node is
            // erased, the size dropped and the system's +0xd4 entry for this
            // bay removed (the routine).
            It = DockQueue.erase(It);
            OwnerSystem->remove_from_list_d4(this);
            continue;
        }

        // the archetype's docking sphere for this
        // port (Archetype::EqObj +0x88 array, 0x10-byte entries: Radius +0,
        // Type +0xc).
        int const Port = It->Port;
        Archetype::EqObj::DockingSphere const& Sphere = Ce->eqobjarch()->DockingSpheres[Port];

        // CEqObj::launch_pos(Pos, Orient, Port).
        Vector LaunchPos;
        Matrix LaunchOrient;
        Ce->launch_pos(LaunchPos, LaunchOrient, Port);

        // the ship's "hpmount" hardpoint in object
        // space (FindHardpoint_OS on the ship's engine index, IObjInspect slot 7),
        // copied into a Transform {Orient, Position}.
        long HpHandle = 0;
        HardpointInfo Hp;
        FindHardpoint_OS(ShipObj->get_index(), "hpmount", HpHandle, Hp);
        Transform HpXform;
        HpXform.Rotation = Hp.Orient;
        HpXform.Translation = Hp.Position;

        // IEngine slot 40 on the same index.
        float* const ShipXform =
            NeuronRuntime::Engine->GetInstanceTransform(reinterpret_cast<void*>(ShipObj->get_index()));

        // MulTransform(Mounted, ShipXform, HpXform)
        // through the lazily fetched math engine (slot 7). module storage is
        // , a no-op constructor (MOV EAX,ECX / RET 4).
        Transform Mounted;
        if (GEqObjMathEngine == nullptr)
            GEqObjMathEngine = Server_Acquire3DMathEngine();
        GEqObjMathEngine->MulTransform(reinterpret_cast<FTransform&>(Mounted),
                                       *reinterpret_cast<FTransform const*>(ShipXform),
                                       reinterpret_cast<FTransform const&>(HpXform));
        // the routine, a 12-dword copy.
        Transform const Copy = Mounted;

        // the mount point relative to the launch
        // position (the routine is Vector(x, y, z)).
        Vector const Delta = { Copy.Translation.x - LaunchPos.x,
                               Copy.Translation.y - LaunchPos.y,
                               Copy.Translation.z - LaunchPos.z };

        // Radius^2 against (dy^2 + dz^2) + dx^2;
        // TEST AH,0x41 / JNZ keeps the ship queued on <= and unordered.
        // Both sides stay on the x87 stack (FCOMPP), so both are doubles here.
        double const DistSq = (static_cast<double>(Delta.y) * Delta.y + static_cast<double>(Delta.z) * Delta.z) +
                              static_cast<double>(Delta.x) * Delta.x;
        if (!(static_cast<double>(Sphere.Radius) * Sphere.Radius > DistSq))
        {
            ++It;
            continue;
        }

        // this bay's own type bits (vt +0x154).
        unsigned int const Bits = get_type_bits();
        if (Bits == (Bits & 0x440u))
        {
            // StarSystem the routine.
            OwnerSystem->complete_dock(ShipObj, this);
        }
        else if ((Bits & 0x800u) != 0)
        {
            // TEST AH,0x8.
            OwnerSystem->complete_dock(ShipObj, this);
        }
        else
        {
            // mooring points 3/4/5 are not a
            // landing; StarSystem the routine(ship, this,
            // &ceqobj()->DockingPointBaseId (+0x164), &bLand). The CEqObj is
            // re-narrowed from the raw Object here.
            bool bLand = true;
            if (Sphere.Type == 3 || Sphere.Type == 4 || Sphere.Type == 5)
                bLand = false;
            CEqObj* const Bay = ceqobj();
            OwnerSystem->queue_dock(ShipObj, this, &Bay->DockingPointBaseId, &bLand);
        }

        // the node erased, the size dropped, the
        // system's +0xd4 entry removed.
        It = DockQueue.erase(It);
        OwnerSystem->remove_from_list_d4(this);
    }
}

// +0x1f0 -- module storage (RET 0).
void EqObj::explode()
{
    // ceqobj()->Archetype->ExplosionArch (+0x24).
    ID_String const& ExplosionArch = ceqobj()->Archetype->ExplosionArch;
    if (ExplosionArch.IsEmpty())
        return;

    Archetype::Explosion const* const Blast = Archetype::GetExplosion(ExplosionArch);
    if (Blast == nullptr)
        return;

    // every test against DOUBLE 0.0001
    // (the field), TEST AH,0x41: the radius must exceed it, and so must one
    // of impulse / hull damage / energy damage.
    if (!(static_cast<double>(Blast->Radius) > 0.0001))
        return;
    if (!(static_cast<double>(Blast->Impulse) > 0.0001) &&
        !(static_cast<double>(Blast->HullDamage) > 0.0001) &&
        !(static_cast<double>(Blast->EnergyDamage) > 0.0001))
        return;

    // {id, id, cause 4, position, blast,
    // PhySys::GetCollisionGroup(raw Object)}.
    ExplosionDamageEvent Event;
    Event.ExplodingObjId = 0;
    Event.InflictorId = 0;
    Event.Cause = static_cast<DamageCause>(4);
    Event.Explosion = nullptr;
    Event.SkipCollisionGroup = nullptr;
    Event.ExplodingObjId = get_id();
    Event.InflictorId = get_id();
    Event.Cause = static_cast<DamageCause>(4);
    Event.Position = get_position();
    Event.Explosion = Blast;
    Event.SkipCollisionGroup = PhySys::GetCollisionGroup(Object);

    // StarSystem the routine.
    OwnerSystem->explode(Event);
}

// +0x1f4 -- module storage (RET 4).
void EqObj::collect_droppable_equip(std::vector<EquipDesc>& Out)
{
    // / module storage -- ONE EquipDesc for the whole walk
    // (GetEquipDesc refills it), the default traverser.
    EquipDesc Desc;
    CEquipTraverser Walk;
    for (;;)
    {
        // ceqobj() re-narrowed every iteration.
        CEquip* const Equip = ceqobj()->EquipManager.Traverse(Walk);
        if (Equip == nullptr)
            break;
        if (Equip->IsDestroyed())                                  // vt +0x08
            continue;
        if (Equip->Type == 0x800)                                  // cargo pods stay
            continue;
        if (!Equip->IsLootable())                                  // vt +0x20
            continue;
        if (!Equip->GetEquipDesc(Desc))                            // vt +0x28
            continue;
        // the inlined VC6 vector insert at end().
        Out.push_back(Desc);
    }
}

// +0x1f8 -- module storage (RET 4).
void EqObj::drop_loot(std::vector<EquipDesc> const& Descs)
{
    // the record vector, reserved to Descs.size()
    // when that is non-zero.
    std::vector<FLootRecord> Loot;
    if (!Descs.empty())
        Loot.reserve(Descs.size());

    // build_loot (vt +0x244) per desc while fewer
    // than six records' worth were produced (CMP EBP,6 / JGE).
    int Total = 0;
    for (std::vector<EquipDesc>::const_iterator It = Descs.begin(); It != Descs.end(); ++It)
    {
        if (Total >= 6)
            break;
        Total += build_loot(*It, Loot);
    }

    // a non-empty vector goes out as loot
    // (StarSystem the routine, mode 2).
    if (!Loot.empty())
        OwnerSystem->create_loot(get_id(), 2, Loot.data(), static_cast<int>(Loot.size()));
}

// +0x1dc -- module storage (RET 0).
void EqObj::rebuild_server_equip()
{
    clear_server_equip();                                          // vt +0x1e0
    CEquipTraverser Walk;
    for (;;)
    {
        // ceqobj() re-narrowed every iteration.
        CEquip* const Equip = ceqobj()->EquipManager.Traverse(Walk);
        if (Equip == nullptr)
            break;
        create_server_equip(Equip->Id);
    }
}

// +0x1e0 -- module storage (RET 0).
void EqObj::clear_server_equip()
{
    // every held ServerEquip deleted through its
    // scalar deleting destructor (vt +0x04, argument 1) and the slot cleared.
    for (std::list<ServerEquip*>::iterator It = ServerEquips.begin(); It != ServerEquips.end(); ++It)
    {
        if (*It != nullptr)
            delete *It;
        *It = nullptr;
    }
    // every node erased.
    ServerEquips.clear();
}

// (module storage, RET 4).
void EqObj::create_server_equip(unsigned short EquipId)
{
    // no null test on the equip.
    CEquip* const Equip = ceqobj()->EquipManager.FindByID(EquipId);

    // and on -- one wrapper per type, each pushed at
    // the end of +0x70 (operator new, the routine + the subclass fields and
    // vtable inlined here; the subclass constructors below carry those stores).
    switch (Equip->Type)
    {
    case 0x80:       // new(0x18), +0x0c = 0, vt module storage
        ServerEquips.push_back(new SrvGun(Equip, this));
        break;
    case 0x40:       // new(0x18), +0x0c = 0, vt module storage
        ServerEquips.push_back(new SrvCounterMeasureDropper(Equip, this));
        break;
    case 0x20:       // ..           new(0x18), +0x0c = 0, vt module storage
        ServerEquips.push_back(new SrvMineDropper(Equip, this));
        break;
    case 0x400:      // ..           new(0x0c), vt module storage
        ServerEquips.push_back(new SrvThruster(Equip, this));
        break;
    case 0x200000:   // new(0x14), +0x0c = 0, +0x10 = NONE
        ServerEquips.push_back(new SrvRepairDroid(Equip, this));
        break;
    case 0x100000:   //                             new(0x0c), vt module storage
        ServerEquips.push_back(new SrvTractor(Equip, this));
        break;
    default:         // no wrapper
        break;
    }
}

// +0x1c0 -- module storage (RET 0xc).
void EqObj::inflict_hull_damage(float Damage, bool bCanKill, DamageList& Out)
{
    if (bCanKill)
    {
        // TEST AH,1 / JNZ skips < and unordered.
        if (Damage >= 0.0f)
            damage_hull(Damage, Out);                              // vt +0x1b0
        return;
    }
    // only while the hull stays above 0
    // (TEST AH,0x41 / JNZ skips <= and unordered).
    if (ceqobj()->get_hit_pts() - Damage > 0.0f)
        damage_hull(Damage, Out);
}

// +0x1c8 -- module storage (RET 0xc).
bool EqObj::destroy_group_by_name(char const* GroupName, DamageEntry::SubObjFate Fate, DamageList& Out)
{
    CacheString Name;
    Name.Value = StringAlloc(GroupName, false);
    CArchGroup* const Group = ceqobj()->ArchGroups.FindByName(Name);
    if (Group == nullptr)
        return false;
    destroy_group(Group, Fate, Out, true);                         // vt +0x220
    return true;
}

// +0x1c4 -- module storage (RET 0x10).
bool EqObj::inflict_group_damage(char const* GroupName, float Damage, bool bCanKill, DamageList& Out)
{
    CacheString Name;
    Name.Value = StringAlloc(GroupName, false);
    CArchGroup* const Group = ceqobj()->ArchGroups.FindByName(Name);
    if (Group == nullptr)
        return false;

    if (bCanKill)
    {
        // TEST BL(1),AH / JNZ skips < and unordered.
        if (Damage >= 0.0f)
            damage_group(Group, Damage, Out);                      // vt +0x218
        return true;
    }

    // the REMAINDER (hit points - Damage) is what
    // is passed on, and only while it stays above 0 (TEST AH,0x41 / JNZ).
    float const Remainder = Group->GetHitPoints() - Damage;
    if (Remainder > 0.0f)
        damage_group(Group, Remainder, Out);
    return true;
}

// +0x1d0 -- module storage (RET 0xc).
bool EqObj::destroy_equip_by_hardpoint(char const* Hardpoint, DamageEntry::SubObjFate Fate, DamageList& Out)
{
    CEqObj* const Ce = ceqobj();                                   // before StringAlloc
    CacheString Name;
    Name.Value = StringAlloc(Hardpoint, false);
    CExternalEquip* const Equip = Ce->EquipManager.FindByHardpoint(Name);
    if (Equip == nullptr)
        return false;
    destroy_equip(Equip, Fate, Out);                               // vt +0x224
    return true;
}

// +0x1cc -- module storage (RET 0x10).
bool EqObj::inflict_equip_damage(char const* Hardpoint, float Damage, bool bCanKill, DamageList& Out)
{
    CEqObj* const Ce = ceqobj();
    CacheString Name;
    Name.Value = StringAlloc(Hardpoint, false);
    CExternalEquip* const Equip = Ce->EquipManager.FindByHardpoint(Name);
    if (Equip == nullptr)
        return false;

    if (bCanKill)
    {
        // no sign test on this path.
        damage_equip(Equip, Damage, Out);                          // vt +0x210
        return true;
    }

    // the remainder, only while above 0.
    float const Remainder = Equip->GetHitPoints() - Damage;        // vt +0x3c
    if (Remainder > 0.0f)
        damage_equip(Equip, Remainder, Out);
    return true;
}

// +0x1d4 -- module storage (RET 8).
bool EqObj::dump_cargo(char const* Hardpoint, DamageList& Out)
{
    bool bDumped = false;                                          // [ESP+0x13]
    CEquipTraverser Walk(0x10000);
    for (;;)
    {
        // ceqobj() re-narrowed every iteration.
        CEquip* const Equip = ceqobj()->EquipManager.Traverse(Walk);
        if (Equip == nullptr)
            break;

        // a stack already held in a pod (CECargo+0x20) stays.
        if (static_cast<CECargo*>(Equip)->CargoPod != nullptr)
            continue;

        // a fresh EquipDesc per item.
        EquipDesc Desc;
        if (!Equip->GetEquipDesc(Desc))
            continue;

        // dumping at a solar for a valid inflicting
        // player (Out+0x18) asks PlayerData the routine first; a refusal skips
        // the item.
        unsigned int const Inflictor = Out.InflictorOwner;
        CSolar* const Solar = EqObj_Solar(this);
        if (Solar != nullptr && Players.is_valid(Inflictor))
        {
            if (!PlayerData_ShouldQueueVisit(Players[Inflictor], Solar->Id, Solar->GetVisitValue()))
                continue;
        }


        bDumped = true;
        if (Hardpoint != nullptr)
        {
            CacheString Hp;
            Hp.Value = StringAlloc(Hardpoint, false);
            Desc.set_hardpoint(Hp);
        }

        // build_loot (vt +0x244) into a fresh
        // vector; a non-zero count goes out through StarSystem the routine.
        std::vector<FLootRecord> Loot;
        if (build_loot(Desc, Loot) != 0)
            OwnerSystem->create_loot(get_id(), 2, Loot.data(), static_cast<int>(Loot.size()));

        // the stack itself is destroyed.
        Out.add_damage_entry(Equip->Id, 0.0f, DamageEntry::SubObjFate::Destroyed);

        // the visit is recorded (the routine),
        // with the owner re-read from Out+0x18.
        CSolar* const SolarAgain = EqObj_Solar(this);
        unsigned int const InflictorAgain = Out.InflictorOwner;
        if (SolarAgain != nullptr && Players.is_valid(InflictorAgain))
            PlayerData_QueueVisit(Players[InflictorAgain], SolarAgain->Id, SolarAgain->GetVisitValue());
    }
    return bDumped;
}

// +0x1d8 -- module storage (RET 4).
bool EqObj::drain_shields(DamageList& Out)
{
    CEquipTraverser Walk(0x100);
    for (;;)
    {
        CEquip* const Equip = ceqobj()->EquipManager.Traverse(Walk);   // re-narrowed each pass
        if (Equip == nullptr)
            break;
        damage_shield(static_cast<CEShield*>(Equip), -1.0f, Out);  // vt +0x214
    }
    return true;
}

// +0x17c -- module storage (RET 8).
void EqObj::damage_sub_obj(FSubObjDamage const& Hit, DamageList& Damage)
{
    // the base body (the routine) first, directly.
    GameObject::damage_sub_obj(Hit, Damage);

    if (Hit.SubObjId == SUBOBJ_ID_ROOT)
        return;

    if (SubObjectID::IsArchGroupID(Hit.SubObjId))
    {
        CArchGroup* const Group = ceqobj()->ArchGroups.FindByID(Hit.SubObjId);
        if (Group != nullptr)
            damage_group(Group, Hit.Damage, Damage);               // vt +0x218
        return;
    }

    if (SubObjectID::IsEquipID(Hit.SubObjId))
    {
        CEquip* const Equip = ceqobj()->EquipManager.FindByID(Hit.SubObjId);
        if (Equip == nullptr)
            return;
        if (Equip->Type == 0x100)
            damage_shield(static_cast<CEShield*>(Equip), Hit.Damage, Damage);   // vt +0x214
        else
            damage_equip(Equip, Hit.Damage, Damage);               // vt +0x210
    }
}

// +0x180 -- module storage (RET 8).
void EqObj::munition_hit(FMunitionImpact const& Hit, DamageList& Damage)
{
    // ceqobj() once, used for both lookups.
    CEqObj* const Ce = ceqobj();

    // the behavior manager hears {1, &Hit}
    // through its slot 0 when there is one (Common the branch, the port's
    // virtual IBehaviorManager::Slot00; the record is the caller's own, the
    // const_cast only fits FBehaviorManagerEvent::Data).
    IBehaviorManager* const Behavior = Ce->get_behavior_interface();
    if (Behavior != nullptr)
    {
        FBehaviorManagerEvent Event;
        Event.Type = 1;
        Event.Data = const_cast<FMunitionImpact*>(&Hit);
        Behavior->Slot00(&Event);
    }

    Archetype::Munition const* const Munition = Hit.Munition;

    if (Hit.SubObjId == SUBOBJ_ID_ROOT || Hit.SubObjId == SUBOBJ_ID_NONE)
    {

        damage_hull(Munition->HullDamage, Damage);                 // vt +0x1b0
    }
    else if (SubObjectID::IsArchGroupID(Hit.SubObjId))
    {
        CArchGroup* const Group = Ce->ArchGroups.FindByID(Hit.SubObjId);
        if (Group == nullptr)
            return;
        damage_group(Group, Munition->HullDamage, Damage);         // vt +0x218
    }
    else if (SubObjectID::IsEquipID(Hit.SubObjId))
    {
        CEquip* const Equip = Ce->EquipManager.FindByID(Hit.SubObjId);
        if (Equip == nullptr)
            return;
        if (Equip->Type == 0x100)
        {
            // the shield takes the whole hit; no
            // energy pass.
            munition_hit_shield(static_cast<CEShield*>(Equip), Munition, Damage);   // vt +0x20c
            return;
        }
        damage_equip(Equip, Munition->HullDamage, Damage);         // vt +0x210
    }
    else
    {
        return;
    }

    // the energy damage (Munition+0x84).
    damage_power(Munition->EnergyDamage, Damage);                  // vt +0x21c
}

// +0x20c -- module storage (RET 0xc).
void EqObj::munition_hit_shield(CEShield* Shield, Archetype::Munition const* Munition, DamageList& Out)
{
    // FLD Hull / FMUL factor / FADD Energy on the
    // FPU, ONE rounding at 06ce94d5 FSTP float; then 06ce94df FMUL float Total
    // on the unrounded CEShield::GetWeaponModifier (Munition+0x88) and 06ce94eb
    // FSTP float into damage_shield's argument.
    float const Total = (float)((double)Munition->HullDamage * ShieldEquipConsts::HULL_DAMAGE_FACTOR
                                + Munition->EnergyDamage);
    damage_shield(Shield, (float)(Shield->GetWeaponModifier(Munition->WeaponType) * Total), Out);   // vt +0x214
}

// +0x184 -- module storage (RET 8).
void EqObj::explosion_hit(ExplosionDamageEvent const& Blast, DamageList& Damage)
{
    // The shield pass first; a functioning shield absorbs the whole blast.
    if (explosion_hit_shield(Blast, Damage))                       // vt +0x204
        return;
    explosion_hit_hull(Blast, Damage);                             // vt +0x1fc
    explosion_hit_subobjs(Blast, Damage);                          // vt +0x200
    explosion_hit_power(Blast, Damage);                            // vt +0x208
}

// The distance bands the four explosion passes share (module storage ..
// and the same code in each pass): R^2 = (float)(Radius * Radius),
// Inner = (float)(R^2 * 1/9) (the field), Middle = (float)(R^2 * 4/9)
// (the field). The distance D (the squared centre distance less the target's
// squared radius) is floored at 0 (TEST AH,0x41 / JZ keeps >), then
// D < Inner -> 3, D < Middle -> 2, D < R^2 -> 1 (TEST AH,5 / JP each), else 0
// (the target is out of reach). baseline keeps D on the x87 stack (53-bit
// precision control) and never stores it, so the port carries it as a double;
// each damage amount is likewise formed on the x87 stack and rounded to float
// once, by the FSTP / the float push.
static int EqObj_ExplosionBand(double Distance, float RadiusSq, float Middle, float Inner)
{
    if (!(Distance > 0.0))
        Distance = 0.0;
    if (Distance < Inner)
        return 3;
    if (Distance < Middle)
        return 2;
    if (Distance < RadiusSq)
        return 1;
    return 0;
}

// +0x1fc -- module storage (RET 8).
void EqObj::explosion_hit_hull(ExplosionDamageEvent const& Blast, DamageList& Out)
{
    CEqObj* const Ce = ceqobj();

    float const RadiusSq = Blast.Explosion->Radius * Blast.Explosion->Radius;
    float const Middle = RadiusSq * (4.0f / 9.0f);
    float const Inner = RadiusSq * (1.0f / 9.0f);

    // CEqObj::get_explosion_dmg_bounding_sphere.
    float Radius = 0.0f;
    Vector Center;
    Ce->get_explosion_dmg_bounding_sphere(Radius, Center);

    // ((dz^2 + dx^2) + dy^2) - r^2.
    double const Dx = static_cast<double>(Blast.Position.x) - Center.x;
    double const Dy = static_cast<double>(Blast.Position.y) - Center.y;
    double const Dz = static_cast<double>(Blast.Position.z) - Center.z;
    double const Distance = ((Dz * Dz + Dx * Dx) + Dy * Dy) - static_cast<double>(Radius) * Radius;

    int const Band = EqObj_ExplosionBand(Distance, RadiusSq, Middle, Inner);
    if (Band == 0)
        return;

    // (float)(Band * 1/3 * HullDamage), times the
    // archetype's explosion_resistance (Root+0x28).
    float const Amount = static_cast<float>(static_cast<double>(Band) * (1.0f / 3.0f) *
                                            Blast.Explosion->HullDamage);
    damage_hull(Ce->get_archetype()->ExplosionResistance * Amount, Out);   // vt +0x1b0
}

// +0x200 -- module storage (RET 8).
void EqObj::explosion_hit_subobjs(ExplosionDamageEvent const& Blast, DamageList& Out)
{
    CEqObj* const Ce = ceqobj();

    float const RadiusSq = Blast.Explosion->Radius * Blast.Explosion->Radius;
    float const Middle = RadiusSq * (4.0f / 9.0f);
    float const Inner = RadiusSq * (1.0f / 9.0f);

    // the attached equipment (const traversal,
    // mask 0x1ee0; the manager is fixed before the loop).
    CEquipTraverser Walk(0x1ee0);
    CEquipManager const& Equipment = Ce->EquipManager;
    for (CEquip const* Equip = Equipment.Traverse(Walk); Equip != nullptr; Equip = Equipment.Traverse(Walk))
    {
        CAttachedEquip const* const Attached = static_cast<CAttachedEquip const*>(Equip);
        float Radius = 0.0f;
        Vector Center;
        Attached->GetRadius(Radius);                               // vt +0x7c
        Attached->GetCenterOfMass(Center);                         // vt +0x70

        // ((dz^2 + dy^2) + dx^2) - r^2.
        double const Dx = static_cast<double>(Blast.Position.x) - Center.x;
        double const Dy = static_cast<double>(Blast.Position.y) - Center.y;
        double const Dz = static_cast<double>(Blast.Position.z) - Center.z;
        double const Distance = ((Dz * Dz + Dy * Dy) + Dx * Dx) - static_cast<double>(Radius) * Radius;

        int const Band = EqObj_ExplosionBand(Distance, RadiusSq, Middle, Inner);
        if (Band == 0)
            continue;
        float const Amount = static_cast<float>(static_cast<double>(Band) * (1.0f / 3.0f) *
                                            Blast.Explosion->HullDamage);
        damage_equip(const_cast<CEquip*>(Equip),
                     Attached->AttachedEquipArch()->ExplosionResistance * Amount, Out);   // vt +0x210
    }

    // every arch group, the same formula.
    CArchGrpTraverser Groups;
    CArchGroupManager const& GroupManager = Ce->ArchGroups;
    for (CArchGroup const* Group = GroupManager.Traverse(Groups); Group != nullptr;
         Group = GroupManager.Traverse(Groups))
    {
        float Radius = 0.0f;
        Vector Center;
        Group->GetRadius(Radius);
        Group->GetCenterOfMass(Center);

        double const Dx = static_cast<double>(Blast.Position.x) - Center.x;
        double const Dy = static_cast<double>(Blast.Position.y) - Center.y;
        double const Dz = static_cast<double>(Blast.Position.z) - Center.z;
        double const Distance = ((Dz * Dz + Dy * Dy) + Dx * Dx) - static_cast<double>(Radius) * Radius;

        int const Band = EqObj_ExplosionBand(Distance, RadiusSq, Middle, Inner);
        if (Band == 0)
            continue;
        float const Amount = static_cast<float>(static_cast<double>(Band) * (1.0f / 3.0f) *
                                            Blast.Explosion->HullDamage);
        damage_group(const_cast<CArchGroup*>(Group), Group->ExplosionResistance() * Amount, Out);   // vt +0x218
    }
}

// +0x208 -- module storage (RET 8).
void EqObj::explosion_hit_power(ExplosionDamageEvent const& Blast, DamageList& Out)
{
    CEqObj* const Ce = ceqobj();

    // TEST AH,5 / JNP: out only on a negative
    // MaxPower (+0x19c); 0 and unordered go on.
    if (Ce->MaxPower < 0.0f)
        return;
    // no energy damage, nothing to do.
    if (Blast.Explosion->EnergyDamage == 0.0f)
        return;

    float const RadiusSq = Blast.Explosion->Radius * Blast.Explosion->Radius;
    float const Middle = RadiusSq * (4.0f / 9.0f);
    float const Inner = RadiusSq * (1.0f / 9.0f);

    // CObject vt +0x64, get_physical_radius_r(Center).
    Vector Center;
    float const Radius = Ce->get_physical_radius_r(Center);

    // ((dz^2 + dx^2) + dy^2) - r^2.
    double const Dx = static_cast<double>(Blast.Position.x) - Center.x;
    double const Dy = static_cast<double>(Blast.Position.y) - Center.y;
    double const Dz = static_cast<double>(Blast.Position.z) - Center.z;
    double const Distance = ((Dz * Dz + Dx * Dx) + Dy * Dy) - static_cast<double>(Radius) * Radius;

    int const Band = EqObj_ExplosionBand(Distance, RadiusSq, Middle, Inner);
    if (Band == 0)
        return;

    // ((Band * 1/3) * EnergyDamage) * resistance.
    float const Resistance = Ce->get_archetype()->ExplosionResistance;
    damage_power(static_cast<float>(static_cast<double>(Band) * (1.0f / 3.0f) *
                                    Blast.Explosion->EnergyDamage * Resistance), Out);   // vt +0x21c
}

// +0x204 -- module storage (RET 8). True when any functioning shield was met.
bool EqObj::explosion_hit_shield(ExplosionDamageEvent const& Blast, DamageList& Out)
{
    bool bShielded = false;                                        // [ESP+0x13]
    CEqObj* const Ce = ceqobj();

    float const RadiusSq = Blast.Explosion->Radius * Blast.Explosion->Radius;
    float const Middle = RadiusSq * (4.0f / 9.0f);
    float const Inner = RadiusSq * (1.0f / 9.0f);

    CEquipTraverser Walk(0x100);
    CEquipManager const& Equipment = Ce->EquipManager;
    for (CEquip const* Equip = Equipment.Traverse(Walk); Equip != nullptr; Equip = Equipment.Traverse(Walk))
    {
        // vt +0x0c IsFunctioning.
        if (!Equip->IsFunctioning())
            continue;
        bShielded = true;

        CAttachedEquip const* const Attached = static_cast<CAttachedEquip const*>(Equip);
        float Radius = 0.0f;
        Vector Center;
        Attached->GetRadius(Radius);                               // vt +0x7c
        Attached->GetCenterOfMass(Center);                         // vt +0x70

        // ((dz^2 + dx^2) + dy^2) - r^2.
        double const Dx = static_cast<double>(Blast.Position.x) - Center.x;
        double const Dy = static_cast<double>(Blast.Position.y) - Center.y;
        double const Dz = static_cast<double>(Blast.Position.z) - Center.z;
        double const Distance = ((Dz * Dz + Dx * Dx) + Dy * Dy) - static_cast<double>(Radius) * Radius;

        int const Band = EqObj_ExplosionBand(Distance, RadiusSq, Middle, Inner);
        if (Band == 0)
            continue;

        // (float)((Hull * HULL_DAMAGE_FACTOR +
        // Energy) * (Band * 1/3)), times the shield's explosion_resistance.
        float const Amount = static_cast<float>(
            (static_cast<double>(Blast.Explosion->HullDamage) * ShieldEquipConsts::HULL_DAMAGE_FACTOR +
             Blast.Explosion->EnergyDamage) *
            (static_cast<double>(Band) * (1.0f / 3.0f)));
        damage_shield(static_cast<CEShield*>(const_cast<CEquip*>(Equip)),
                      Attached->AttachedEquipArch()->ExplosionResistance * Amount, Out);   // vt +0x214
    }
    return bShielded;
}

// +0x228 -- module storage (RET 0xc).
void EqObj::destroy_cargo(CEquip* Equip, DamageEntry::SubObjFate Fate, DamageList& Out)
{
    // the stack in the pod (CECargoPod+0x4c).
    CECargo* const Cargo = static_cast<CECargoPod*>(Equip)->Cargo;
    if (Cargo != nullptr)
    {
        int const FateValue = static_cast<int>(Fate);
        if (FateValue != 4 && FateValue != 5)
        {
            if (Fate == DamageEntry::SubObjFate::Separated)
            {
                // a separated pod drops its stack
                // as loot (fate 4) through destroy_equip.
                destroy_equip(Cargo, static_cast<DamageEntry::SubObjFate>(4), Out);   // vt +0x224
            }
            else
            {

                Out.add_damage_entry(Cargo->Id, 0.0f, DamageEntry::SubObjFate::Destroyed);
            }
        }
        else
        {
            // at a solar, a valid inflicting player
            // (PlayerDB::is_valid, a real call here) that already has this visit
            // gets no loot.
            bool bRefused = false;
            unsigned int const Inflictor = Out.InflictorOwner;
            CSolar* const Solar = EqObj_Solar(this);
            if (Solar != nullptr && Players.is_valid(Inflictor))
            {
                if (!PlayerData_ShouldQueueVisit(Players[Inflictor], Solar->Id, Solar->GetVisitValue()))
                    bRefused = true;
            }

            if (!bRefused)
            {
                // fate 4 asks the controller (event
                // 0x13) and takes the desc it hands back; fate 5 reads it.
                EquipDesc Desc;
                if (static_cast<int>(Fate) == 4)
                {
                    FEquipLootEvent Event;
                    Event.SystemId = 0;
                    Event.InflictorId = 0;
                    Event.ObjId = 0;
                    Event.Cause = static_cast<DamageCause>(0);
                    Event.SystemId = OwnerSystem->SystemInfo->NicknameId;
                    Event.InflictorId = Out.InflictorId;
                    Event.ObjId = get_id();
                    Event.Cause = Out.Cause;
                    Cargo->GetEquipDesc(Event.Desc);                         // vt +0x28
                    Server_ControllerDispatch(0, pub::Controller::EMessage::EquipDropLoot, &Event);
                    Desc = Event.Desc;                                       // REP MOVSD 8
                }
                else
                {
                    Cargo->GetEquipDesc(Desc);
                }

                // nothing left (count <= 0, or
                // health not > 0): the pod is only separated.
                if (Desc.Count <= 0 || !(Desc.Health > 0.0f))
                {
                    Fate = DamageEntry::SubObjFate::Separated;
                }
                else
                {
                    // the record's first eight
                    // dwords zeroed, +0x68 = 1, +0x69 = 0, then build_equip_loot
                    // (vt +0x248) on the pod.
                    FLootRecord Record;
                    Record.Id = 0;
                    Record.Arch = nullptr;
                    Record.HitPts = 0.0f;
                    Record.ContentsArch = nullptr;
                    Record.ContentsHitPts = 0.0f;
                    Record.Count = 0;
                    Record.Field_0x18 = 0;
                    Record.Field_0x1c = 0;
                    Record.Field_0x68 = 1;
                    Record.bMission = 0;
                    if (build_equip_loot(static_cast<CAttachedEquip const*>(Equip), Record))
                    {
                        // the stack's health and
                        // count; cause 0x16 drops it in mode 1, anything else 2.
                        Record.ContentsHitPts = Desc.Health * Record.ContentsArch->HitPts;
                        Record.Count = Desc.Count;
                        int const Mode = (static_cast<int>(Out.Cause) == 0x16) ? 1 : 2;
                        OwnerSystem->create_loot(get_id(), Mode, &Record, 1);

                        // the visit recorded
                        // (the routine), owner re-read from Out+0x18.
                        CSolar* const SolarAgain = EqObj_Solar(this);
                        unsigned int const InflictorAgain = Out.InflictorOwner;
                        if (SolarAgain != nullptr && Players.is_valid(InflictorAgain))
                            PlayerData_QueueVisit(Players[InflictorAgain], SolarAgain->Id,
                                                  SolarAgain->GetVisitValue());
                    }
                }
            }

            // the stack is always gone.
            Out.add_damage_entry(Cargo->Id, 0.0f, DamageEntry::SubObjFate::Destroyed);
        }
    }

    // the pod's own entry with the (possibly
    // rewritten) fate.
    Out.add_damage_entry(Equip->Id, 0.0f, Fate);
}

// +0x224 -- module storage (RET 0xc).
void EqObj::destroy_equip(CEquip* Equip, DamageEntry::SubObjFate Fate, DamageList& Out)
{
    if (Equip->IsDestroyed())                                      // vt +0x08
        return;

    // a cargo pod goes through destroy_cargo.
    if (Equip->Type == 0x800)
    {
        destroy_cargo(Equip, Fate, Out);                           // vt +0x228
        return;
    }

    int const FateValue = static_cast<int>(Fate);
    if (FateValue == 4 || FateValue == 5)
    {
        // not lootable, or disabled: separated.
        if (!Equip->IsLootable() || Equip->IsDisabled())           // vt +0x20 / +0x10
        {
            Fate = DamageEntry::SubObjFate::Separated;
        }
        else
        {
            // the solar visit test (a real
            // PlayerDB::is_valid call).
            bool bRefused = false;
            unsigned int const Inflictor = Out.InflictorOwner;
            CSolar* const Solar = EqObj_Solar(this);
            if (Solar != nullptr && Players.is_valid(Inflictor))
            {
                if (!PlayerData_ShouldQueueVisit(Players[Inflictor], Solar->Id, Solar->GetVisitValue()))
                    bRefused = true;
            }

            if (bRefused)
            {
                Fate = DamageEntry::SubObjFate::Separated;
            }
            else
            {
                // the desc (event 0x13 for fate 4).
                EquipDesc Desc;
                if (static_cast<int>(Fate) == 4)
                {
                    FEquipLootEvent Event;
                    Event.SystemId = 0;
                    Event.InflictorId = 0;
                    Event.ObjId = 0;
                    Event.Cause = static_cast<DamageCause>(0);
                    Event.SystemId = OwnerSystem->SystemInfo->NicknameId;
                    Event.ObjId = get_id();
                    Event.InflictorId = Out.InflictorId;
                    Event.Cause = Out.Cause;
                    Equip->GetEquipDesc(Event.Desc);
                    Server_ControllerDispatch(0, pub::Controller::EMessage::EquipDropLoot, &Event);
                    Desc = Event.Desc;
                }
                else
                {
                    Equip->GetEquipDesc(Desc);
                }


                if (Desc.Count <= 0 || !(Desc.Health > 0.0f))
                {
                    Fate = DamageEntry::SubObjFate::Separated;
                }
                else
                {
                    CAttachedEquip const* const Attached = CAttachedEquip::cast(Equip);
                    if (Attached != nullptr)
                    {
                        // one record for the
                        // attached equip itself.
                        FLootRecord Record;
                        Record.Id = 0;
                        Record.Arch = nullptr;
                        Record.HitPts = 0.0f;
                        Record.ContentsArch = nullptr;
                        Record.ContentsHitPts = 0.0f;
                        Record.Count = 0;
                        Record.Field_0x18 = 0;
                        Record.Field_0x1c = 0;
                        Record.Field_0x68 = 1;
                        Record.bMission = 0;
                        if (build_equip_loot(Attached, Record))            // vt +0x248
                        {
                            // both hit-point
                            // fields take Health * ContentsArch hit points.
                            float const HitPts = Desc.Health * Record.ContentsArch->HitPts;
                            Record.ContentsHitPts = HitPts;
                            Record.HitPts = HitPts;
                            int const Mode = (static_cast<int>(Out.Cause) == 0x16) ? 1 : 2;
                            OwnerSystem->create_loot(get_id(), Mode, &Record, 1);
                        }
                    }
                    else
                    {
                        // internal equipment
                        // becomes build_loot's records (mode 2).
                        std::vector<FLootRecord> Loot;
                        if (build_loot(Desc, Loot) != 0)                   // vt +0x244
                            OwnerSystem->create_loot(get_id(), 2, Loot.data(),
                                                     static_cast<int>(Loot.size()));
                    }

                    // on both paths, whether or
                    // not a record was built, the visit is recorded.
                    CSolar* const SolarAgain = EqObj_Solar(this);
                    unsigned int const InflictorAgain = Out.InflictorOwner;
                    if (SolarAgain != nullptr && Players.is_valid(InflictorAgain))
                        PlayerData_QueueVisit(Players[InflictorAgain], SolarAgain->Id,
                                              SolarAgain->GetVisitValue());
                }
            }
        }
    }


    Out.add_damage_entry(Equip->Id, 0.0f, Fate);
}

// +0x1b0 -- module storage (RET 8).
bool EqObj::damage_hull(float Amount, DamageList& Damage)
{
    // only a positive amount is scaled
    // (TEST AH,0x41 / JNZ skips <= and unordered).
    if (Amount > 0.0f)
    {
        // a player's current ship takes the
        // player's difficulty scale (PlayerData+0x3c4).
        PlayerData* const Player = EqObj_PlayerOfShip(this);
        if (Player != nullptr)
            Amount = Player->difficulty_scale * Amount;

        // armour (type 0x1000000) divides by its
        // hit_pts_scale (Armor+0x70).
        CEquip* const Armor = ceqobj()->EquipManager.FindFirst(0x1000000u);
        if (Armor != nullptr)
            Amount = Amount / static_cast<CEArmor*>(Armor)->ArmorArch()->HitPtsScale;
    }

    // the base body (the routine), directly.
    return GameObject::damage_hull(Amount, Damage);
}

// +0x210 -- module storage (RET 0xc).
bool EqObj::damage_equip(CEquip* Equip, float Damage, DamageList& Out)
{
    // TEST AH,0x44 / JNP: zero damage does nothing.
    if (Damage == 0.0f)
        return false;
    if (Equip->IsDestroyed())                                      // vt +0x08
        return false;

    // what the list already holds for the equip,
    // else its hit points (TEST AH,5 / JP keeps >= and unordered).
    float Current = Out.get_hit_pts_left(Equip->Id);
    if (Current < 0.0f)
        Current = Equip->GetHitPoints();                           // vt +0x3c
    float NewHitPts = Current;

    if (Damage > 0.0f)
    {
        // an attached equip of an OWNED object
        // divides by PLAYER_ATTACHED_EQUIP_HIT_PTS_SCALE and, when this is the
        // owner's current ship, takes the difficulty scale. Unowned objects and
        // internal equipment take the damage as it is.
        double Applied = Damage;
        if (CAttachedEquip::cast(Equip) != nullptr && csimple()->OwnerPlayer != 0)
        {
            Damage = Damage / PLAYER_ATTACHED_EQUIP_HIT_PTS_SCALE;
            Applied = Damage;
            PlayerData* const Player = EqObj_PlayerOfShip(this);
            if (Player != nullptr)
                Applied = static_cast<double>(Player->difficulty_scale) * Damage;
        }
        // floored at 0 (TEST AH,0x41 / JNZ).
        double const Left = Current - Applied;  // adj stays on the x87 stack
        NewHitPts = (Left > 0.0) ? static_cast<float>(Left) : 0.0f;
    }
    else if (Damage < 0.0f)
    {
        // TEST AH,5 / JP: a negative amount
        // empties it.
        NewHitPts = 0.0f;
    }

    // a blocked list (vt +0x1b8) leaves the equip
    // no lower than (1 - invincible timer) of its max: F = 1 - vt +0x1a0; a
    // current relative health already below F keeps the current value; a new
    // one below F is raised to Max * F. The current ratio tests Max for zero,
    // the new one does not.
    if (is_damage_blocked(Out))
    {
        float const Floor = 1.0f - get_invincible_timer();
        float const Max = Equip->GetMaxHitPoints();                // vt +0x38
        double const RelCurrent = (Max != 0.0f) ? static_cast<double>(Current) / Max : 0.0;
        if (RelCurrent < Floor)
        {
            NewHitPts = Current;
        }
        else if (static_cast<double>(NewHitPts) / Max < Floor)
        {
            NewHitPts = Max * Floor;
        }
    }

    // unchanged: nothing to record.
    if (NewHitPts == Current)
        return false;

    // still alive: the new hit points.
    if (NewHitPts > 0.0f)
    {
        Out.add_damage_entry(Equip->Id, NewHitPts, DamageEntry::SubObjFate::None);
        return false;
    }

    // destroyed: separated (2), except a launcher
    // of a weapons-offline object (CEqObj+0x15c) outside single player, which
    // keeps fate 0.
    DamageEntry::SubObjFate DeathFate = DamageEntry::SubObjFate::Separated;
    if (!SinglePlayer() && ceqobj()->bWeaponsOffline && CELauncher::cast(Equip) != nullptr)
        DeathFate = DamageEntry::SubObjFate::None;
    destroy_equip(Equip, DeathFate, Out);                          // vt +0x224
    return true;
}

// +0x214 -- module storage (RET 0xc).
bool EqObj::damage_shield(CEShield* Shield, float Damage, DamageList& Out)
{
    if (Damage == 0.0f)
        return false;
    if (Shield->IsDestroyed())
        return false;

    float Current = Out.get_hit_pts_left(Shield->Id);
    if (Current < 0.0f)
        Current = Shield->GetHitPoints();                          // vt +0x3c
    float NewHitPts = Current;

    if (Damage > 0.0f)
    {
        // only the difficulty scale here.
        double Applied = Damage;
        PlayerData* const Player = EqObj_PlayerOfShip(this);
        if (Player != nullptr)
            Applied = static_cast<double>(Player->difficulty_scale) * Damage;
        double const Left = Current - Applied;  // adj stays on the x87 stack
        NewHitPts = (Left > 0.0) ? static_cast<float>(Left) : 0.0f;
    }
    else if (Damage < 0.0f)
    {
        NewHitPts = 0.0f;
    }

    // a blocked list: with no invincible timer
    // left (TEST AH,0x41 / JNZ, <= and unordered) the shield keeps its current
    // value; otherwise a new relative value below F = 1 - timer is either held
    // at the current value (when that one is below F too) or raised to Max * F
    // (GetMaxHitPoints re-read). Both ratios test Max for zero here.
    if (is_damage_blocked(Out))
    {
        float const Timer = get_invincible_timer();
        if (!(Timer > 0.0f))
        {
            NewHitPts = Current;
        }
        else
        {
            float const Max = Shield->GetMaxHitPoints();
            double const RelNew = (Max != 0.0f) ? static_cast<double>(NewHitPts) / Max : 0.0;  // kept on the x87 stack
            float const RelCurrent = (Max != 0.0f) ? Current / Max : 0.0f;  // stored (FSTP [ESP+0x10])
            float const Floor = 1.0f - Timer;
            if (RelNew < Floor)
            {
                if (RelCurrent < Floor)
                    NewHitPts = Current;
                else
                    NewHitPts = Shield->GetMaxHitPoints() * Floor;
            }
        }
    }

    if (NewHitPts == Current)
        return false;


    Out.add_damage_entry(Shield->Id, NewHitPts, DamageEntry::SubObjFate::None);

    // true when the change crosses the shield's
    // OfflineThreshold (+0x74): from above to strictly below, or from at/below
    // (or unordered) to strictly above. The old side is GetRelativeHealth
    // (vt +0x44), the new side New / Max (0 for a zero Max).
    float const Max = Shield->GetMaxHitPoints();
    float const RelNew = (Max != 0.0f) ? NewHitPts / Max : 0.0f;
    float const Threshold = Shield->OfflineThreshold;
    // CALL [EDX + 0x44] / 06cea9a9 FCOMP float [Threshold] --
    // the old ratio is compared unrounded, never stored.
    double const RelOld = Shield->GetRelativeHealth();
    if (RelOld > Threshold)
        return RelNew < Threshold;
    return RelNew > Threshold;
}

// +0x218 -- module storage (RET 0xc).
bool EqObj::damage_group(CArchGroup* Group, float Damage, DamageList& Out)
{
    if (Damage == 0.0f)
        return false;
    // killed (2) or destroyed (3) groups are done.
    if (Group->State == 2 || Group->State == 3)
        return false;
    // TEST AH,0x41 / JP: only a group with hit
    // points above 0 (or unordered) takes damage.
    if (!(Group->GetHitPoints() > 0.0f) && !(Group->GetHitPoints() != Group->GetHitPoints()))
        return false;

    unsigned short const GroupId = Group->Arch->Id;
    float Current = Out.get_hit_pts_left(GroupId);
    if (Current < 0.0f)
        Current = Group->GetHitPoints();
    float NewHitPts = Current;

    if (Damage > 0.0f)
    {
        // armour's hit_pts_scale divides first:
        // FLD [Arch+0x70] / 06ceab6a FDIVR Damage, and the quotient
        // stays on the FPU (no store before 06ceab9c / 06ceac29).
        double Scaled = Damage;
        CEquip* const Armor = ceqobj()->EquipManager.FindFirst(0x1000000u);
        if (Armor != nullptr)
            Scaled = (double)Damage / ((CEArmor*)Armor)->ArmorArch()->HitPtsScale;

        // an OWNED object's group then divides by
        // PLAYER_COLLISION_GROUP_HIT_PTS_SCALE (06ceab9c FDIV on the unrounded
        // quotient, 06ceaba2 the only FSTP float) and, on the owner's current
        // ship, takes the difficulty scale. An unowned object subtracts the
        // unrounded quotient directly (06ceab90 JZ -> 06ceac29 FSUBR).
        double Applied = Scaled;
        if (csimple()->OwnerPlayer != 0)
        {
            float const OwnedScaled = (float)(Scaled / PLAYER_COLLISION_GROUP_HIT_PTS_SCALE);
            Applied = OwnedScaled;
            PlayerData* const Player = EqObj_PlayerOfShip(this);
            if (Player != nullptr)
                Applied = (double)Player->difficulty_scale * OwnedScaled;
        }
        double const Left = Current - Applied;  // adj stays on the x87 stack
        NewHitPts = (Left > 0.0) ? static_cast<float>(Left) : 0.0f;
    }
    else if (Damage < 0.0f)
    {
        NewHitPts = 0.0f;
    }

    // the invincibility floor, as damage_equip's
    // but with Max tested for zero in both ratios.
    if (is_damage_blocked(Out))
    {
        float const Floor = 1.0f - get_invincible_timer();
        // CArchGroup::GetMaxHitPoints / 06ceac88 FST float: the
        // first zero test (06ceac8c) sees the unrounded ST0, the divisions and
        // the second zero test (06ceacbe) the stored float.
        double const MaxExact = Group->GetMaxHitPoints();
        float const Max = (float)MaxExact;
        double const RelCurrent = (MaxExact != 0.0) ? (double)Current / Max : 0.0;
        if (RelCurrent < Floor)
        {
            NewHitPts = Current;
        }
        else
        {
            double const RelNew = (Max != 0.0f) ? static_cast<double>(NewHitPts) / Max : 0.0;
            if (RelNew < Floor)
                NewHitPts = Max * Floor;
        }
    }

    bool bDestroyed = false;                                       // BL
    if (NewHitPts != Current)
    {
        // a burning death fuse keeps low-cause
        // damage from destroying the group outright.
        bool const bFuseHolds = is_death_fuse_burning() && static_cast<int>(Out.Cause) < 0x10;

        // `f6c441` TEST AH,0x41 / 06cead2e `7418` JZ -- only
        // New > 0 reaches the entry arm; <= 0 AND unordered fall through to
        // the destroy unless the fuse holds.
        if (!(NewHitPts > 0.0f) && !bFuseHolds)
        {
            // destroy_group(fate 2, with contents).
            destroy_group(Group, DamageEntry::SubObjFate::Separated, Out, true);   // vt +0x220
            bDestroyed = true;
        }
        else
        {
            // the entry, and "destroyed" when it
            // is at or below 0 (TEST AH,0x41 / JP: > and unordered are not).
            Out.add_damage_entry(GroupId, NewHitPts, DamageEntry::SubObjFate::None);
            bDestroyed = (NewHitPts <= 0.0f);
        }
    }

    // a root-health-proxy group passes the ORIGINAL
    // damage on to the hull; its result counts as well.
    if (Group->IsRootHealthProxy() && damage_hull(Damage, Out))   // vt +0x1b0
        return true;
    return bDestroyed;
}

// +0x21c -- module storage (RET 8).
bool EqObj::damage_power(float Damage, DamageList& Out)
{
    if (Damage == 0.0f)
        return false;

    // TEST AH,0x41 / JNP, twice in baseline: no
    // power plant (MaxPower <= 0, CEqObj+0x19c) takes no energy damage.
    CEqObj* const Ce = ceqobj();
    if (Ce->MaxPower <= 0.0f)
        return false;

    float Current = Out.get_hit_pts_left(SUBOBJ_ID_POWER);
    if (Current < 0.0f)
        Current = Ce->Power;                                       // +0x198
    float NewHitPts = Current;

    if (Damage > 0.0f)
    {
        double Applied = Damage;
        PlayerData* const Player = EqObj_PlayerOfShip(this);
        if (Player != nullptr)
            Applied = static_cast<double>(Player->difficulty_scale) * Damage;
        double const Left = Current - Applied;  // adj stays on the x87 stack
        NewHitPts = (Left > 0.0) ? static_cast<float>(Left) : 0.0f;
    }
    else if (Damage < 0.0f)
    {
        NewHitPts = 0.0f;
    }

    // the invincibility floor over MaxPower,
    // through its reciprocal (1 / Max, no zero test).
    if (is_damage_blocked(Out))
    {
        float const Max = Ce->MaxPower;
        float const Floor = 1.0f - get_invincible_timer();
        double const Inverse = 1.0 / Max;  // never stored
        if (Current * Inverse < Floor)
            NewHitPts = Current;
        else if (NewHitPts * Inverse < Floor)
            NewHitPts = Floor * Max;
    }

    if (NewHitPts == Current)
        return false;

    Out.add_damage_entry(SUBOBJ_ID_POWER, NewHitPts, DamageEntry::SubObjFate::None);
    // true when the power is at or below 0
    // (TEST AH,0x41 / JP: > and unordered answer false).
    return NewHitPts <= 0.0f;
}

// +0x220 -- module storage (RET 0x10).
void EqObj::destroy_group(CArchGroup* Group, DamageEntry::SubObjFate Fate, DamageList& Out,
                          bool bWithChildren)
{
    if (Group->State == 2 || Group->State == 3)
        return;

    // TEST AH,0x41 / JP: a group with hit points
    // left (or unordered) is destroyed regardless; an empty one only when the
    // fate changes.
    float const HitPts = Group->GetHitPoints();
    if (!(HitPts > 0.0f) && !(HitPts != HitPts) && Group->Fate == static_cast<int>(Fate))
        return;

    // a group that takes its parent with it only
    // marks the list destroyed.
    if (Group->DestroysParent())
    {
        Out.bDestroyed = true;
        return;
    }

    damage_linked_equipment(Group, Out);                           // vt +0x22c
    if (bWithChildren)
        destroy_group_contents(Group, Out);                        // vt +0x230

    Out.add_damage_entry(Group->Arch->Id, 0.0f, Fate);
}

// +0x22c -- module storage (RET 8).
void EqObj::damage_linked_equipment(CArchGroup* Group, DamageList& Out)
{
    unsigned int const LinkedType = Group->LinkedEquipmentType();
    if (LinkedType == 0)
        return;

    if (LinkedType == 0x100)
    {

        FDUMP(0x100001, "%s(%d) : *** ERROR: %s",
              "Server\\Space\\EqObj.cpp", 0x91b,
              "Implement me!  Linked shield damage!");
        return;
    }

    // every equip of that type takes
    // LinkedEquipmentDamage * its max hit points (const traversal; the manager
    // is fixed before the loop).
    CEquipTraverser Walk(static_cast<int>(LinkedType));
    CEquipManager const& Equipment = ceqobj()->EquipManager;
    for (CEquip const* Equip = Equipment.Traverse(Walk); Equip != nullptr; Equip = Equipment.Traverse(Walk))
    {
        float const Max = Equip->GetMaxHitPoints();                // vt +0x38
        damage_equip(const_cast<CEquip*>(Equip), Group->LinkedEquipmentDamage() * Max, Out);   // vt +0x210
    }
}

// +0x230 -- module storage (RET 8).
void EqObj::destroy_group_contents(CArchGroup* Group, DamageList& Out)
{
    CEqObj* const Ce = ceqobj();

    // external equipment (mask 0x1fe3) mounted
    // under the group's instance, except types 1 and 2, is destroyed (fate 1).
    CEquipTraverser Walk(0x1fe3);
    CEquipManager const& Equipment = Ce->EquipManager;
    for (CEquip const* Equip = Equipment.Traverse(Walk); Equip != nullptr; Equip = Equipment.Traverse(Walk))
    {
        // vt +0x64: CExternalEquip::GetParentConnector(false).
        long const Parent = static_cast<CExternalEquip const*>(Equip)->GetParentConnector(false);
        if (!Group->IsInstInGroup(Parent))
            continue;
        if (Equip->Type == 1 || Equip->Type == 2)
            continue;
        destroy_equip(const_cast<CEquip*>(Equip), DamageEntry::SubObjFate::Destroyed, Out);   // vt +0x224
    }

    // every OTHER group whose root instance lies in
    // this one is destroyed too (fate 1, without its own contents).
    CArchGrpTraverser Groups;
    CArchGroupManager const& GroupManager = Ce->ArchGroups;
    for (CArchGroup const* Child = GroupManager.Traverse(Groups); Child != nullptr;
         Child = GroupManager.Traverse(Groups))
    {
        if (Group->Arch->Id == Child->Arch->Id)
            continue;
        if (!Group->IsInstInGroup(Child->GetRootIndex()))
            continue;
        destroy_group(const_cast<CArchGroup*>(Child), DamageEntry::SubObjFate::Destroyed, Out, false);   // vt +0x220
    }
}

// +0x244 -- module storage (RET 8). The loot records one EquipDesc becomes;
// returns how many were appended.
int EqObj::build_loot(EquipDesc const& Desc, std::vector<FLootRecord>& Out)
{
    int Built = 0;                                                 // [ESP+0x18]

    Archetype::Equipment* const Arch = Archetype::GetEquipment(Desc.ArchId);
    if (Arch == nullptr)
        return 0;

    // the record: first eight dwords zeroed,
    // +0x68 = 1, the contents are the equip itself at Health * its hit points,
    // the loot object is get_loot_appearance (Equipment slot 7) at ITS hit
    // points, bMission from the desc.
    FLootRecord Record;
    Record.Id = 0;
    Record.Arch = nullptr;
    Record.HitPts = 0.0f;
    Record.Count = 0;
    Record.Field_0x18 = 0;
    Record.Field_0x1c = 0;
    Record.Field_0x68 = 1;
    Record.bMission = 0;
    Record.ContentsArch = Arch;
    Record.ContentsHitPts = Desc.Health * Arch->HitPts;
    Archetype::Equipment* const Appearance = Arch->get_loot_appearance();
    Record.Arch = Appearance;
    Record.HitPts = Appearance->HitPts;
    Record.bMission = Desc.bMission ? 1 : 0;

    // orientation (IObjInspect slot 3, the
    // transform's rotation) and position (slot 0) of this object.
    Record.Orientation = get_transform().Rotation;
    Record.Position = get_position();

    // mounted equipment starts at its own centre:
    // a cargo stack at its pod's (CECargo+0x20), anything external at its own
    // (vt +0x70). Otherwise (not found, or neither) the desc's hardpoint on this
    // object (HardpointPosition on the engine index, slot 7).
    if (!Desc.is_internal())
    {
        bool bPlaced = false;
        CEquip* const Equip = ceqobj()->EquipManager.FindByID(Desc.Id);
        if (Equip != nullptr)
        {
            CExternalEquip const* Mounted = nullptr;
            if (Equip->Type == 0x10000)
                Mounted = static_cast<CExternalEquip const*>(static_cast<CECargo*>(Equip)->CargoPod);
            else
                Mounted = CExternalEquip::cast(Equip);
            if (Mounted != nullptr)
            {
                Mounted->GetCenterOfMass(Record.Position);
                bPlaced = true;
            }
        }
        if (!bPlaced)
            HardpointPosition(get_index(), Desc.Hardpoint, &Record.Position, nullptr);
    }

    // the id maker, the count, and the stack size
    // (Equipment+0x64 units_per_container).
    CFLIDMaker* const Maker = CFLIDMaker::GetInstance();
    int Remaining = Desc.Count;
    int const PerContainer = Arch->UnitsPerContainer;
    if (Remaining <= 0)
        return Built;

    do
    {
        // at most six records unless it is a
        // mission item (CMP 5 / JG returns the count so far).
        if (!Desc.bMission && Built > 5)
            return Built;

        // a fresh object id (the inlined
        // AllocateObjectID: counter & 0x3fffffff, then ++counter); the count is
        // min(remaining, per container), compared UNSIGNED (JA).
        Record.Id = Maker->AllocateObjectID();
        Record.Count = (static_cast<unsigned>(Remaining) > static_cast<unsigned>(PerContainer))
                           ? PerContainer
                           : Remaining;

        // this object's velocity (slot 1).
        Record.Velocity = get_velocity();

        // a random push: Scale = rand() / 32768
        // (the field, kept as a double), a random unit vector times 2 * Scale
        // (each component rounded to float), times 15 (the field), added.
        double const Scale = static_cast<double>(rand()) * 3.0517578125e-05;
        Vector const Direction = random_on_unit_sphere();
        Vector const Push = { static_cast<float>((Scale + Scale) * Direction.x),
                              static_cast<float>((Scale + Scale) * Direction.y),
                              static_cast<float>((Scale + Scale) * Direction.z) };
        Vector const Kick = { Push.x * 15.0f, Push.y * 15.0f, Push.z * 15.0f };
        Record.Velocity.x = Kick.x + Record.Velocity.x;
        Record.Velocity.y = Record.Velocity.y + Kick.y;
        Record.Velocity.z = Record.Velocity.z + Kick.z;

        // a random spin about the orientation's
        // first and third columns: A, B = rand() / 32768 * 0.8 - 0.4 (doubles
        // / the field, each rounded to float);
        // AngularVelocity = col0 * A + col2 * B.
        float const SpinA = static_cast<float>(static_cast<double>(rand()) * 3.0517578125e-05 *
                                               0.800000011920929 - 0.4000000059604645);
        float const Col0X = Record.Orientation.E[0][0] * SpinA;
        float const Col0Y = Record.Orientation.E[1][0] * SpinA;
        float const Col0Z = Record.Orientation.E[2][0] * SpinA;
        float const SpinB = static_cast<float>(static_cast<double>(rand()) * 3.0517578125e-05 *
                                               0.800000011920929 - 0.4000000059604645);
        // the x and y products stay on the x87
        // stack before the add; only z's product is stored (FSTP [ESP+0xe0])
        // first.
        float const Col2Z = Record.Orientation.E[2][2] * SpinB;
        Record.AngularVelocity.x =
            static_cast<float>(static_cast<double>(Record.Orientation.E[0][2]) * SpinB + Col0X);
        Record.AngularVelocity.y =
            static_cast<float>(static_cast<double>(Record.Orientation.E[1][2]) * SpinB + Col0Y);
        Record.AngularVelocity.z = Col2Z + Col0Z;

        // the routine, vector insert(end, 1, Record).
        Out.push_back(Record);


        ++Built;
        Remaining -= Record.Count;
    } while (Remaining > 0);

    return Built;
}

// +0x248 -- module storage (RET 8). One attached equip -> one loot record.
bool EqObj::build_equip_loot(CAttachedEquip const* Equip, FLootRecord& Out)
{
    // a fresh object id, the equip's archetype
    // and hit points (vt +0x3c).
    Out.Id = CFLIDMaker::GetInstance()->AllocateObjectID();
    Out.Arch = Equip->Arch;
    float const HitPts = Equip->GetHitPoints();
    Out.HitPts = HitPts;

    if (Equip->Type == 0x800)
    {
        // a cargo pod carries its stack.
        CECargo* const Cargo = CECargoPod::cast(Equip)->Cargo;
        Out.ContentsArch = Cargo->Arch;
        Out.ContentsHitPts = Cargo->GetHitPoints();
        Out.Count = static_cast<int>(Cargo->Count);
    }
    else
    {

        Out.ContentsHitPts = HitPts;
        Out.ContentsArch = Out.Arch;
        Out.Count = 1;
    }

    // GetCenterOfMass (vt +0x70) into the
    // position; baseline then overwrites it with the engine's instance position.
    Equip->GetCenterOfMass(Out.Position);

    // the root instance (vt +0x84), its position
    // (IEngine slot 36) and orientation (slot 38).
    long const Instance = Equip->GetRootIndex();
    float const* const Position =
        NeuronRuntime::Engine->GetInstancePosition(reinterpret_cast<void*>(Instance));
    Out.Position.x = Position[0];
    Out.Position.y = Position[1];
    Out.Position.z = Position[2];
    float const* const Orientation =
        NeuronRuntime::Engine->GetInstanceOrientation(reinterpret_cast<void*>(Instance));
    memcpy(&Out.Orientation, Orientation, sizeof(Out.Orientation));

    // ComputeExplosiveImpulses(instance, A, B, C);
    // only the second out vector is used.
    Vector Unused1;
    Vector Impulse;
    Vector Unused2;
    ComputeExplosiveImpulses(Instance, Unused1, Impulse, Unused2);

    // scaled by child_impulse / mass (Attached
    // +0x78 / Root +0x20).
    Archetype::AttachedEquipment const* const Arch = Equip->AttachedEquipArch();
    // The quotient stays on the x87 stack; each component is rounded once.
    double const ImpulseScale = static_cast<double>(Arch->ChildImpulse) / Equip->AttachedEquipArch()->Mass;
    Impulse.x = static_cast<float>(Impulse.x * ImpulseScale);
    Impulse.y = static_cast<float>(Impulse.y * ImpulseScale);
    Impulse.z = static_cast<float>(Impulse.z * ImpulseScale);

    // times rand() / 32768 * 0.5 + 0.75 (doubles
    // / the field).
    double const Jitter = static_cast<double>(rand()) * 3.0517578125e-05 * 0.5 + 0.75;
    Impulse.x = static_cast<float>(Jitter * Impulse.x);
    Impulse.y = static_cast<float>(Jitter * Impulse.y);
    Impulse.z = static_cast<float>(Jitter * Impulse.z);

    // the equip's own velocity (vt +0x6c) plus
    // the impulse.
    Equip->GetVelocity(Out.Velocity);
    Out.Velocity.x = Impulse.x + Out.Velocity.x;
    Out.Velocity.y = Impulse.y + Out.Velocity.y;
    Out.Velocity.z = Impulse.z + Out.Velocity.z;

    // three rand() / 32768 values; the FIRST goes
    // to y, the second to x, the third to z.
    float const SpinY = static_cast<float>(rand()) * 3.0517578125e-05f;
    float const SpinX = static_cast<float>(rand()) * 3.0517578125e-05f;
    float const SpinZ = static_cast<float>(rand()) * 3.0517578125e-05f;
    Out.AngularVelocity.x = SpinX;
    Out.AngularVelocity.y = SpinY;
    Out.AngularVelocity.z = SpinZ;

    // normalised and scaled to rand() / 32768 * 3
    // + 2 (doubles the field / the field): factor = (float)(s / |v|).
    double const Spin = static_cast<double>(rand()) * 3.0517578125e-05 * 3.0 + 2.0;
    double const Length = sqrt(static_cast<double>(SpinX) * SpinX +
                               static_cast<double>(SpinY) * SpinY +
                               static_cast<double>(SpinZ) * SpinZ);
    float const Factor = static_cast<float>(Spin / Length);
    Out.AngularVelocity.x = Factor * Out.AngularVelocity.x;
    Out.AngularVelocity.y = Factor * Out.AngularVelocity.y;
    Out.AngularVelocity.z = Factor * Out.AngularVelocity.z;

    // vt +0x14 IsTemporary into +0x69.
    Out.bMission = Equip->IsTemporary() ? 1 : 0;
    return true;
}

// +0x24c -- module storage (RET 4).
void EqObj::decay_cargo(float Elapsed)
{
    // a cause-0x14 damage list.
    DamageList Decay;
    Decay.Cause = static_cast<DamageCause>(0x14);

    // not a solar (0x303), and the equipment
    // manager has decaying cargo (CEquipManager+0x1c > 0, signed). The CEqObj
    // null is dereferenced as in baseline.
    CEqObj* const Ce = ceqobj();
    bool bDecay = true;
    if (Ce != nullptr && (static_cast<unsigned>(Ce->ObjectClass) & 0x303u) == 0x303u)
        bDecay = false;
    if (!(Ce->EquipManager.DecayingCargoCount > 0))
        bDecay = false;

    // a valid owning player with an open trade
    // (PlayerData+0x3cc) freezes the decay.
    unsigned int const Owner = csimple()->OwnerPlayer;
    if (Players.is_valid(Owner) && Players[Owner].trade_offer != nullptr)
        return;
    if (!bDecay)
        return;

    // every cargo stack (non-const traversal).
    CEquipTraverser Walk(0x10000);
    for (CEquip* Equip = Ce->EquipManager.Traverse(Walk); Equip != nullptr; Equip = Ce->EquipManager.Traverse(Walk))
    {
        CECargo* const Cargo = static_cast<CECargo*>(Equip);
        if (Cargo->Count == 0)                                     // JBE: unsigned
            continue;

        // TEST AH,0x41 / JNZ: only a positive decay amount.
        float const Amount = Cargo->GetDecayDamagePerSecond() * Elapsed;
        if (!(Amount > 0.0f))
            continue;

        float NewHitPts = 0.0f;
        if (Cargo->GetHitPoints() - Amount > 0.0f)
        {
            // hit points re-read; an unequal-to-0
            // result (TEST AH,0x44 / JP) is a plain entry.
            NewHitPts = Cargo->GetHitPoints() - Amount;
            if (NewHitPts != 0.0f)
            {
                Decay.add_damage_entry(Cargo->Id, NewHitPts, DamageEntry::SubObjFate::None);
                continue;
            }
        }

        // a spent single item is destroyed; a
        // spent stack only records 0.
        if (Cargo->Count == 1)
            Decay.add_damage_entry(Cargo->Id, NewHitPts, DamageEntry::SubObjFate::Destroyed);
        else
            Decay.add_damage_entry(Cargo->Id, NewHitPts, DamageEntry::SubObjFate::None);
    }

    // vt +0x188.
    if (Decay.Count != 0)
        apply_damage_list(Decay);
}

// ---------------------------------------------------------------------------
// ServerEquip family -- module storage .. module storage
// ---------------------------------------------------------------------------

// .data:the field -- SrvGun::fire_forward's own lazily fetched math engine
// (module storage `MOV EAX,[the field]` / JNZ / CALL the routine / store).
static I3DMathEngine* GSrvGunMathEngine = nullptr;

// (RET 8): +4 = the equip, +8 = the owner, ServerEquip's vtable
// (module storage), then CEquip::ConnectServerEquip(this) on the equip with no
// null test.
ServerEquip::ServerEquip(CEquip* InEquip, EqObj* InOwner)
    : Equip(InEquip)
    , Owner(InOwner)
{
    Equip->ConnectServerEquip(this);
}

// (and the scalar deleting destructor module storage / the one
// every subclass table carries, module storage): the vtable reset, then
// CEquip::NotifyDisconnecting(this) while an equip is held.
ServerEquip::~ServerEquip()
{
    if (Equip != nullptr)
        Equip->NotifyDisconnecting(this);
}

// slot 0 -- module storage (RET 8). Only event 0xa (the equip is going away):
// NotifyDisconnecting(this) on the held equip (no null test) and forget it.
void ServerEquip::notify(Event Code, void* Param)
{
    (void)Param;
    if (Code != EquipDestroying)
        return;
    Equip->NotifyDisconnecting(this);
    Equip = nullptr;
}

// slot 2 -- module storage (RET 4): SETNZ on the held equip; Dt is not read.
bool ServerEquip::update(float Dt)
{
    (void)Dt;
    return Equip != nullptr;
}

// operator new(0x0c) + the routine + vtable module storage
// (module storage .. 06ce8b3c).
SrvThruster::SrvThruster(CEquip* InEquip, EqObj* InOwner)
    : ServerEquip(InEquip, InOwner)
{
}

// (RET 8).
void SrvThruster::notify(Event Code, void* Param)
{
    (void)Param;
    if (Code == Event_0xF)
    {
        // an AI-owned object (CSimple+0xb4 == 0,
        // read through the unguarded (ObjectClass & 3) narrowing) switches its
        // thrusters off: a tail jump into the owner's IObjAffect slot +0x54 with
        // both arguments rewritten to 0. A player's object ignores the event.
        if (Owner->csimple()->OwnerPlayer == 0)
            Owner->slot_54(false, 0);
        return;
    }
    if (Code == EquipDestroying)
    {
        // the base body, inlined.
        Equip->NotifyDisconnecting(this);
        Equip = nullptr;
    }
}

// operator new(0x0c) + the routine + vtable module storage. The notify body
// (module storage) lives with the StarSystem code.
SrvTractor::SrvTractor(CEquip* InEquip, EqObj* InOwner)
    : ServerEquip(InEquip, InOwner)
{
}

// operator new(0x14) + the routine, +0x0c = 0 (module storage), +0x10 =
// SUBOBJ_ID_NONE (module storage), vtable module storage.
SrvRepairDroid::SrvRepairDroid(CEquip* InEquip, EqObj* InOwner)
    : ServerEquip(InEquip, InOwner)
    , RepairTimer(0.0f)
    , TargetId(SUBOBJ_ID_NONE)
{
}

// (RET 4).
bool SrvRepairDroid::update(float Dt)
{
    // no equip: false.
    CEquip* const Droid = Equip;
    if (Droid == nullptr)
        return false;

    // an inactive droid (vt +0x04) does nothing.
    if (!Droid->IsActive())
        return true;

    // pick a target when there is none.
    if (TargetId == SUBOBJ_ID_NONE)
        TargetId = pick_target();
    if (TargetId == SUBOBJ_ID_NONE)
        return true;

    // FLD/FADD/FST into +0x0c; only past 2.0f
    // (the field; TEST AH,0x41 / JNZ skips <= and unordered). The timer keeps
    // counting otherwise.
    RepairTimer = Dt + RepairTimer;
    if (!(RepairTimer > 2.0f))
        return true;

    // the target's relative health through the
    // equip owner's CEqObj slot +0xf0 (get_sub_obj_relative_health); a failed
    // lookup, or a target already at 1.0f or above (TEST AH,5 / JP, the field;
    // unordered too), drops the target.
    float Relative = 0.0f;
    if (!Droid->Owner->get_sub_obj_relative_health(TargetId, Relative) || !(Relative < 1.0f))
    {
        TargetId = SUBOBJ_ID_NONE;
        RepairTimer = 0.0f;
        return true;
    }

    // a cause-0x11 list from this droid's owner:
    // inflictor = its get_id() (vt +0x20), owner = its CSimple+0xb4.
    DamageList Repair;
    Repair.Cause = static_cast<DamageCause>(0x11);
    Repair.InflictorId = Owner->get_id();
    Repair.InflictorOwner = Owner->csimple()->OwnerPlayer;

    // the target's hit points (slot +0xe4,
    // get_sub_obj_hit_pts); failing that the target is dropped.
    float Current = 0.0f;
    if (Droid->Owner->get_sub_obj_hit_pts(TargetId, Current))
    {
        // the droid pays for the repair out of its
        // own hit points: Amount = (float)(repair_rate (RepairDroid+0x70) *
        // timer), the droid's entry = hit points - Amount floored at 0
        // (TEST AH,0x41 / JNZ; GetHitPoints re-read for the store).
        float const Amount = static_cast<CERepairDroid*>(Droid)->RepairDroidArch()->RepairRate * RepairTimer;
        float DroidHitPts = 0.0f;
        if (Droid->GetHitPoints() - Amount > 0.0f)
            DroidHitPts = Droid->GetHitPoints() - Amount;
        Repair.add_damage_entry(Droid->Id, DroidHitPts, DamageEntry::SubObjFate::None);

        // the target's max: current / relative
        // when the relative health is above 0 (TEST AH,0x41 / JNZ), else slot
        // +0xec (get_sub_obj_max_hit_pts); 0 until then.
        float MaxHitPts = 0.0f;
        if (Relative > 0.0f)
            MaxHitPts = Current / Relative;
        else
            Droid->Owner->get_sub_obj_max_hit_pts(TargetId, MaxHitPts);

        // the target gains Amount, capped at its
        // max (TEST AH,5 / JP takes the max on >= and unordered).
        double const Repaired = static_cast<double>(Amount) + Current;
        float NewHitPts = MaxHitPts;
        if (Repaired < MaxHitPts)
            NewHitPts = static_cast<float>(Repaired);
        Repair.add_damage_entry(TargetId, NewHitPts, DamageEntry::SubObjFate::None);

        // the owner's apply_damage_list (vt +0x188).
        Owner->apply_damage_list(Repair);
    }
    else
    {
        TargetId = SUBOBJ_ID_NONE;
    }

    // the list torn down, the timer reset.
    RepairTimer = 0.0f;
    return true;
}

// (module storage, RET 0).
unsigned short SrvRepairDroid::pick_target() const
{
    float Lowest = 1.0f;
    unsigned short Best = SUBOBJ_ID_NONE;

    // the equip owner's (CEquip+0x04) attached,
    // non-shield equipment, const traversal with the default traverser: a
    // relative health (vt +0x44) strictly below the best so far (TEST AH,5 /
    // JP) wins.
    CEqObj* const Ce = Equip->Owner;
    CEquipTraverser Walk;
    CEquipManager const& Equipment = Ce->EquipManager;
    for (CEquip const* Item = Equipment.Traverse(Walk); Item != nullptr; Item = Equipment.Traverse(Walk))
    {
        if (CAttachedEquip::cast(Item) == nullptr)
            continue;
        if (Item->Type == 0x100)
            continue;
        // FCOM float [Lowest] on the unrounded ST0; only the
        // winner is rounded (06ced714 FSTP float).
        double const Relative = Item->GetRelativeHealth();
        if (Relative < Lowest)
        {
            Lowest = (float)Relative;
            Best = Item->Id;
        }
    }

    // then every arch group the same way.
    CArchGrpTraverser Groups;
    CArchGroupManager const& GroupManager = Ce->ArchGroups;
    for (CArchGroup const* Group = GroupManager.Traverse(Groups); Group != nullptr;
         Group = GroupManager.Traverse(Groups))
    {
        // FCOM / 06ced766 FSTP float, as above.
        double const Relative = Group->GetRelativeHealth();
        if (Relative < Lowest)
        {
            Lowest = (float)Relative;
            Best = Group->Arch->Id;
        }
    }
    return Best;
}

// The launcher wrappers' shared base. baseline has no constructor of its own:
// the factory stores +0x0c = 0 inline (module storage and siblings) and never
// writes +0x10 (operator new garbage); the port zeroes both.
SrvLauncher::SrvLauncher(CEquip* InEquip, EqObj* InOwner)
    : ServerEquip(InEquip, InOwner)
    , FireCount(0)
    , LastDamageTime(0.0)
{
}

// slot 4 -- module storage (RET 4), shared by the mine and countermeasure
// tables; SourceClient is not read. The barrels' average world position plus
// their average direction scaled to 1e10 (the field) over its length
// ((y^2 + z^2) + x^2 on the x87 stack), handed to slot 3.
bool SrvLauncher::fire_forward(int SourceClient)
{
    (void)SourceClient;
    CELauncher* const Launcher = static_cast<CELauncher*>(Equip);
    Vector const Position = Launcher->GetAvgBarrelPosWS();
    Vector Direction = Launcher->GetAvgBarrelDirWS();

    double const Scale = 10000000000.0 /
        sqrt((static_cast<double>(Direction.y) * Direction.y + static_cast<double>(Direction.z) * Direction.z) +
             static_cast<double>(Direction.x) * Direction.x);
    Direction.x = static_cast<float>(Direction.x * Scale);
    Direction.y = static_cast<float>(Direction.y * Scale);
    Direction.z = static_cast<float>(Direction.z * Scale);

    Vector const Target = { Position.x + Direction.x, Direction.y + Position.y, Direction.z + Position.z };
    return fire(Target);                                            // vt +0x0c
}

// (module storage, RET 4; the Vector is not read).
bool SrvLauncher::use_ammo(Vector const& Target)
{
    (void)Target;

    // a live equip (vt +0x08) on a live owner
    // (vt +0x160).
    if (Equip == nullptr || Equip->IsDestroyed())
        return false;
    if (Owner->is_destroyed())
        return false;

    ++FireCount;
    CELauncher* const Launcher = static_cast<CELauncher*>(Equip);

    // outside single player the ammunition must be
    // there (vt +0xa4 AmmoNeedsMet) and is paid for (vt +0x94).
    if (!SinglePlayer())
    {
        if (!Launcher->AmmoNeedsMet())
            return false;
        Launcher->ConsumeFireResources();
    }

    // the owner's player (CSimple+0xb4), its id
    // (vt +0x20), PlayerDB::is_valid inlined, and PlayerData+0x3e4 against the
    // id: a player's current ship with an ammunition-using projectile
    // (Projectile+0x78) loses one round of it from the player's cargo
    // (the routine: arch = the projectile's nickname, 1, 1.0f, no notify).
    unsigned int const OwnerPlayer = Owner->csimple()->OwnerPlayer;
    unsigned int const OwnerId = Owner->get_id();
    if (Players.is_valid(OwnerPlayer))
    {
        PlayerData& Player = Players[OwnerPlayer];
        if (Player.ship_object_id == OwnerId)
        {
            Archetype::Projectile const* const Projectile = Launcher->ProjectileArch();
            if (Projectile->bRequiresAmmo)
                PlayerData_RemoveCargo(Player, Projectile->Nickname.Id, 1, 1.0f, false);
        }
    }
    return true;
}

// operator new(0x18) + the routine, +0x0c = 0.
SrvMineDropper::SrvMineDropper(CEquip* InEquip, EqObj* InOwner)
    : SrvLauncher(InEquip, InOwner)
{
}

// slot 3 -- module storage (RET 4).
bool SrvMineDropper::fire(Vector const& Target)
{
    if (Equip == nullptr || Equip->IsDestroyed())
        return false;
    if (Owner->is_destroyed())
        return false;
    CELauncher* const Launcher = static_cast<CELauncher*>(Equip);
    // vt +0x90 CELauncher::Fire must answer 9 (FireOk).
    if (Launcher->Fire(Target) != FireOk)
        return false;
    Launcher->ConsumeFireResources();                              // vt +0x94
    ++FireCount;
    launch(Target);
    return true;
}

// slot 5 -- module storage (RET 4).
bool SrvMineDropper::fire_unchecked(Vector const& Target)
{
    if (!use_ammo(Target))
        return false;
    launch(Target);
    return true;
}

// Fills the per-shot part of a launch record from one ProjLaunchInfo
// (module storage .. 06cedaf4: three dwords, REP MOVSD x9, three dwords).
static void SrvLauncher_CopyLaunchInfo(FServerLaunchRecord& Record, ProjLaunchInfo const& Info)
{
    Record.Position = Info.Pos;
    Record.Orientation = Info.Orient;
    Record.Velocity = Info.Velocity;
}

// (module storage, RET 4).
void SrvMineDropper::launch(Vector const& Target)
{
    // CELauncher::ComputeLaunchInfo (vt +0x98).
    std::vector<ProjLaunchInfo> Shots;
    static_cast<CELauncher*>(Equip)->ComputeLaunchInfo(Shots, Target);
    if (Shots.empty())
        return;

    // the shared part: owner player, owner id, the
    // mine archetype (CEMineDropper::MineArch), the equip id. baseline leaves
    // +0x4c/+0x50 (guided-only) as stack garbage; the port value-initialises.
    FServerLaunchRecord Record = {};
    Record.EquipId = SUBOBJ_ID_NONE;
    Record.OwnerPlayer = Owner->csimple()->OwnerPlayer;
    Record.OwnerId = Owner->get_id();
    Record.Arch = static_cast<CEMineDropper*>(Equip)->MineArch();
    Record.EquipId = Equip->Id;

    // StarSystem the routine per shot, the
    // system re-read from the owner (+0x28) each time.
    for (std::vector<ProjLaunchInfo>::const_iterator It = Shots.begin(); It != Shots.end(); ++It)
    {
        SrvLauncher_CopyLaunchInfo(Record, *It);
        Owner->OwnerSystem->create_mine(Record);
    }
}

// operator new(0x18) + the routine, +0x0c = 0.
SrvCounterMeasureDropper::SrvCounterMeasureDropper(CEquip* InEquip, EqObj* InOwner)
    : SrvLauncher(InEquip, InOwner)
{
}

// slot 0 -- module storage (RET 8).
void SrvCounterMeasureDropper::notify(Event Code, void* Param)
{
    (void)Param;
    if (Code == Event_0x19)
    {
        // an AI owner (CSimple+0xb4 == 0) drops a
        // countermeasure through its own IObjAffect slot +0x34: this equip's id
        // (a dword on the stack), count 1, no target position, client -1.
        if (Owner->csimple()->OwnerPlayer != 0)
            return;
        unsigned short const EquipId = Equip->Id;
        Owner->slot_34(&EquipId, 1, nullptr, -1);
        return;
    }
    if (Code == EquipDestroying)
    {
        // the base body, inlined.
        Equip->NotifyDisconnecting(this);
        Equip = nullptr;
    }
}

// slot 3 -- module storage (RET 4).
bool SrvCounterMeasureDropper::fire(Vector const& Target)
{
    if (Equip == nullptr || Equip->IsDestroyed())
        return false;
    if (Owner->is_destroyed())
        return false;
    CELauncher* const Launcher = static_cast<CELauncher*>(Equip);
    if (Launcher->Fire(Target) != FireOk)
        return false;
    Launcher->ConsumeFireResources();
    ++FireCount;
    launch(Target);
    return true;
}

// slot 5 -- module storage (RET 4).
bool SrvCounterMeasureDropper::fire_unchecked(Vector const& Target)
{
    if (!use_ammo(Target))
        return false;
    launch(Target);
    return true;
}

// (module storage, RET 4). SrvMineDropper::launch with the
// countermeasure archetype (CECounterMeasureDropper::CounterMeasureArch) and
// StarSystem the routine.
void SrvCounterMeasureDropper::launch(Vector const& Target)
{
    std::vector<ProjLaunchInfo> Shots;
    static_cast<CELauncher*>(Equip)->ComputeLaunchInfo(Shots, Target);
    if (Shots.empty())
        return;

    FServerLaunchRecord Record = {};
    Record.EquipId = SUBOBJ_ID_NONE;
    Record.OwnerPlayer = Owner->csimple()->OwnerPlayer;
    Record.OwnerId = Owner->get_id();
    Record.Arch = static_cast<CECounterMeasureDropper*>(Equip)->CounterMeasureArch();
    Record.EquipId = Equip->Id;

    for (std::vector<ProjLaunchInfo>::const_iterator It = Shots.begin(); It != Shots.end(); ++It)
    {
        SrvLauncher_CopyLaunchInfo(Record, *It);
        Owner->OwnerSystem->create_counter_measure(Record);
    }
}

// operator new(0x18) + the routine, +0x0c = 0.
SrvGun::SrvGun(CEquip* InEquip, EqObj* InOwner)
    : SrvLauncher(InEquip, InOwner)
{
}

// slot 3 -- module storage (RET 4).
bool SrvGun::fire(Vector const& Target)
{
    if (Equip == nullptr || Equip->IsDestroyed())
        return false;
    if (Owner->is_destroyed())
        return false;
    CELauncher* const Launcher = static_cast<CELauncher*>(Equip);
    if (Launcher->Fire(Target) != FireOk)
        return false;
    Launcher->ConsumeFireResources();
    ++FireCount;

    // a seeking munition (Munition+0x90) goes out
    // as guided missiles (slot 7); anything else as bullets (slot 6), and only
    // outside single player.
    if (static_cast<CEGun*>(Equip)->MunitionArch()->Seeker != 0)
    {
        launch_guided(Target);                                      // vt +0x1c
        return true;
    }
    if (!SinglePlayer())
        launch_bullets(Target);                                     // vt +0x18
    return true;
}

// slot 4 -- module storage (RET 4).
bool SrvGun::fire_forward(int SourceClient)
{
    // the base aim ((z^2 + y^2) + x^2 here).
    CELauncher* const Launcher = static_cast<CELauncher*>(Equip);
    Vector const Position = Launcher->GetAvgBarrelPosWS();
    Vector Direction = Launcher->GetAvgBarrelDirWS();

    double const Scale = 10000000000.0 /
        sqrt((static_cast<double>(Direction.z) * Direction.z + static_cast<double>(Direction.y) * Direction.y) +
             static_cast<double>(Direction.x) * Direction.x);
    Direction.x = static_cast<float>(Direction.x * Scale);
    Direction.y = static_cast<float>(Direction.y * Scale);
    Direction.z = static_cast<float>(Direction.z * Scale);
    Vector Target = { Position.x + Direction.x, Direction.y + Position.y, Direction.z + Position.z };

    // slot 3; a refused shot answers false.
    bool const bFired = fire(Target);
    if (!bFired)
        return false;

    // no system, nothing to announce.
    StarSystem* const System = Owner->OwnerSystem;
    if (System == nullptr)
        return bFired;

    // outside single player, a locked target
    // (CEGun::GetTarget) turns the aim point into the target's frame: the math
    // engine's InverseTransform (slot +0x3c) over the target's Transform
    // (EngineObject+0x08 orientation, +0x2c position).
    if (!SinglePlayer())
    {
        CSimple* LockTarget = nullptr;
        unsigned short LockSubId = SUBOBJ_ID_NONE;
        static_cast<CEGun*>(Equip)->GetTarget(LockTarget, LockSubId);
        if (LockTarget != nullptr)
        {
            if (GSrvGunMathEngine == nullptr)
                GSrvGunMathEngine = Server_Acquire3DMathEngine();
            FVector Local;
            GSrvGunMathEngine->InverseTransform(
                Local, *reinterpret_cast<FTransform const*>(&LockTarget->Orientation),
                reinterpret_cast<FVector const&>(Target));
            Target = reinterpret_cast<Vector const&>(Local);
        }
    }

    // a one-element id vector (the inlined
    // vector<unsigned short>::insert(begin, 1, id)), the owner's id, and
    // StarSystem the routine.
    std::vector<unsigned short> EquipIds;
    EquipIds.insert(EquipIds.begin(), 1, Equip->Id);
    unsigned int const OwnerId = Owner->get_id();
    System->notify_fire_weapon(OwnerId, EquipIds, Target, SourceClient);
    return bFired;
}

// slot 5 -- module storage (RET 4). No bullets here: only a seeking munition
// is launched (slot 7) after use_ammo.
bool SrvGun::fire_unchecked(Vector const& Target)
{
    if (!use_ammo(Target))
        return false;
    if (static_cast<CEGun*>(Equip)->MunitionArch()->Seeker != 0)
        launch_guided(Target);                                      // vt +0x1c
    return true;
}

// slot 6 -- module storage (RET 4).
void SrvGun::launch_bullets(Vector const& Target)
{
    std::vector<ProjLaunchInfo> Shots;
    static_cast<CELauncher*>(Equip)->ComputeLaunchInfo(Shots, Target);   // vt +0x98

    // the system (+0x28) read once, the owner id,
    // the munition archetype, the equip id.
    StarSystem* const System = Owner->OwnerSystem;
    FServerBulletRecord Record = {};
    Record.OwnerId = Owner->get_id();
    Record.Arch = static_cast<CEGun*>(Equip)->MunitionArch();
    Record.EquipId = Equip->Id;

    // StarSystem the routine per shot.
    for (std::vector<ProjLaunchInfo>::const_iterator It = Shots.begin(); It != Shots.end(); ++It)
    {
        Record.Position = It->Pos;
        Record.Orientation = It->Orient;
        Record.Velocity = It->Velocity;
        System->create_bullet(Record);
    }
}

// slot 7 -- module storage (RET 4).
void SrvGun::launch_guided(Vector const& Target)
{
    std::vector<ProjLaunchInfo> Shots;
    static_cast<CELauncher*>(Equip)->ComputeLaunchInfo(Shots, Target);   // vt +0x98
    if (Shots.empty())
        return;

    // the shared part (+0x4c = 0, +0x50 = NONE
    // until the first shot fills them).
    FServerLaunchRecord Record = {};
    Record.EquipId = SUBOBJ_ID_NONE;
    Record.TargetId = 0;
    Record.TargetSubId = SUBOBJ_ID_NONE;
    Record.OwnerPlayer = Owner->csimple()->OwnerPlayer;
    Record.OwnerId = Owner->get_id();
    Record.Arch = static_cast<CEGun*>(Equip)->MunitionArch();
    Record.EquipId = Equip->Id;

    // per shot: the lock target's id (CSimple
    // +0xb0) or 0, its sub-object, and StarSystem the routine (system re-read
    // each time).
    for (std::vector<ProjLaunchInfo>::const_iterator It = Shots.begin(); It != Shots.end(); ++It)
    {
        SrvLauncher_CopyLaunchInfo(Record, *It);
        Record.TargetId = (It->LockTarget != nullptr) ? It->LockTarget->Id : 0;
        Record.TargetSubId = It->LockSubId;
        Owner->OwnerSystem->create_guided(Record);
    }
}

// ---------------------------------------------------------------------------
// Ship -- module storage / create module storage
// ---------------------------------------------------------------------------

// . baseline zeroes its whole own block, builds the EquipDescList at
// +0xA0..+0xA8 (the 0x28-byte self-linked head), and constructs the EquipIdMaker
// at +0xC4 last. See server_gameobject.h for the instruction map.
Ship::Ship()
{
    DeathSequenceObjId = 0;
    DeathSequenceTimer = 0.0f;
    Field_0x98 = 0;
    PendingRequest = nullptr;
    // .. module storage (allocator byte +0xA0, operator new(0x28)
    // self-linked head +0xA4, size +0xA8 = 0): the Jettison member's
    // EquipDescList constructor, run before this body.
    JettisonTimer = 0.0f;
    PendingBayCloses = 0;
    // +0xB4 is deliberately not touched: baseline's constructor never writes it
    // (there is no store between module storage and module storage). Zeroed
    // here anyway, because leaving a member indeterminate buys nothing.
    Field_0xB4 = nullptr;
    Field_0xB8 = 0;
    AccumulatedDamage = 0.0f;
    AccumulatedDamageTime = 0.0f;
    // EquipIds at +0xC4 constructs itself (module storage).
}

// . CAUTION when reading that listing: it is reached through the
// CBase vtable at Ship+0x0C, so its ECX is `object + 0x0C` and every offset in
// it is 0x0C LOW. `MOV EBX,[ESI+0x98]` at module storage is Ship+0xA4, the
// jettison list's sentinel, and `MOV ECX,[ESI+0x9c]` at module storage is
// Ship+0xA8, its size -- NOT the scalars this port calls Field_0x98 and
// PendingRequest. The three stores at module storage / module storage /
// are what prove the adjustment: they rewrite the same three
// vptrs the constructor stamps at module storage / module storage /
// , at +0x00, +0x04 and +0x0C of the object.
Ship::~Ship()
{
    // / module storage -- PUSH EDI (the Ship base) / CALL
    // drop this ship from the dying-ship list.
    Ship_ForgetDying(this);

    // .. module storage -- CALL [vtable + 0x15c] with the Ship
    // vtable just re-stamped, i.e. this class's own destroy_notify.
    destroy_notify();

    // .. module storage -- every node unlinked and freed, then
    // the head (the inlined EquipDescList destructor; Unwind@06d60ef8 runs the
    // imported one on the exceptional path). That is the Jettison member's
    // destructor, which runs after this body and before ~EqObj (module storage).
}

// ---------------------------------------------------------------------------
// The dying-ship list (server_gameobject.h). the routine / the routine are
// this definition's construction and atexit destruction.
// ---------------------------------------------------------------------------
std::list<Ship*> GDyingShips;

// (module storage .. 06d00172, cdecl, one argument): operator
// new(0xc), link before the sentinel (a push_back), node value = the ship,
// ++the field.
void Ship_QueueDying(Ship* DyingShip)
{
    GDyingShips.push_back(DyingShip);
}

// (module storage .. 06d001cb, cdecl, one argument): walk from
// the first node to the sentinel; every node whose value (+0x08) equals the
// ship is unlinked, freed and the size decremented -- the walk does not stop
// at the first match.
void Ship_ForgetDying(Ship* DyingShip)
{
    std::list<Ship*>::iterator It = GDyingShips.begin();
    while (It != GDyingShips.end()) {
        if (*It == DyingShip)
            It = GDyingShips.erase(It);
        else
            ++It;
    }
}

bool Ship::create(Archetype::Root* Arch)
{
    // PUSH 0x503.
    return CreateOfClass(CObject::ClassShip, Arch);
}

// (five instructions):
//   MOV EAX,[ECX+0xb4] / MOV ECX,[ESP+4] / MOV [ECX],EAX / XOR EAX,EAX / RET 4
int Ship::get_data(void const*& OutData) const
{
    OutData = Field_0xB4;
    return 0;
}

bool Ship::find(unsigned int const& Id)
{
    // PUSH 0x503.
    return FindOfClass(CObject::ClassShip, Id);
}

// . See the declaration for the instruction map.
unsigned short Ship::allocate_equip_id()
{
    // the (& 3) == 3 accessor may yield null
    // (module storage XOR EAX,EAX) and [EAX+0xb4] is read regardless -- baseline
    // has no null test here.
    if (csimple()->OwnerPlayer != 0)
    {
        // .. module storage -- strings read off the PE
        // (module storage / module storage), then *SUBOBJ_ID_NONE
        // (module storage). (2026-09-24, claude:srv-pdb: the old comment said
        // "the port has no FDUMP", which was never true.)
        FDUMP(0x100001, "%s(%d) : *** ERROR: %s",
              "Server\\space\\ship.cpp", 0x1e5,
              "Ship::allocate_equip_id (): Player tried to allocate id!");
        return SUBOBJ_ID_NONE;
    }
    // /module storage -- a TAIL JMP into
    // SubObjectID::EquipIdMaker::CreateEquipID on this + 0xc4.
    return EquipIds.CreateEquipID();
}

CShip* Ship::cship() const
{
    CObject* Obj = cobj();
    if (Obj == nullptr)
        return nullptr;
    if ((static_cast<unsigned>(Obj->ObjectClass) & 0x503u) != 0x503u)
        return nullptr;
    return static_cast<CShip*>(Obj);
}

// (module storage, RET 4), all ten instructions:
// MOV  ESI,ECX
// CALL the routine              ; EqObj +0x1bc body, DIRECT
// MOV  EAX,[ESP + 0x8]           ; &State
// MOV  CX,word ptr [EAX]
// MOV  word ptr [ESI + 0xc4],CX  ; EquipIds
// MOV  EDX,dword ptr [EAX + 0x4]
// MOV  dword ptr [ESI + 0xb4],EDX ; Field_0xB4
void Ship::set_equip_state(FEquipState const& State)
{
    EqObj::check_fuses();
    EquipIds = State.EquipIds;
    Field_0xB4 = State.Data;
}

// (module storage, RET 0):
// MOV  AL,[ECX + 0xb8] / TEST / JNZ -> return
// MOV  EAX,1
// MOV  byte [ECX + 0xb8],AL      ; latch
// MOV  [ESP],EAX                 ; Cmd.Type = 1
// MOV  byte [ESP + 4],AL         ; Cmd.bProtect = 1
// MOV  byte [ESP + 5],AL         ; Cmd.bFromPlayers = 1
// MOV  dword [ESP + 8],0         ; Cmd.MaxHpLoss = 0.0f
// CALL [EAX + 0x194]             ; set_invincible(&Cmd)
void Ship::apply_launch_invincibility()
{
    if (Field_0xB8 != 0)
        return;
    Field_0xB8 = 1;
    FInvincibility Cmd = {};
    Cmd.Type = 1u;
    Cmd.bProtect = true;
    Cmd.bFromPlayers = true;
    Cmd.MaxHpLoss = 0.0f;
    set_invincible(Cmd);
}

// (module storage, RET 4). EDI is the RAW CBase::Object
// (module storage `MOV EDI,[ESI+0x10]`), used with no class or null test.
void Ship::tick_zone_damage(float Dt)
{
    CObject* const Raw = Object;
    CSimple* const Simple = static_cast<CSimple*>(Raw);

    // CALL [EAX + 0x19c] (is_invincible) / JNZ -> return
    if (is_invincible())
        return;

    // MOV ECX,[EDI + 0xa4]  -- the zone CSimple::update_zones
    // cached (Common ZONE, vftable module storage): slot +0x5c
    // (module storage) -> module storage `FLD [ECX+0x60]; RET`, the zone's
    // damage rate.
    Universe::IZone* const Zone = Simple->ZoneAtA4;
    if (Zone != nullptr)
    {
        float const Rate = Zone->GetDamage();
        // FCOM 0.0f (module storage) / TEST AH,0x41 / JNZ:
        // skip when Rate <= 0.
        if (Rate > 0.0f)
        {
            // FMUL [EDI+0xa8] / module storage FMUL dt /
            // FADD [ESI+0xbc] / module storage FSTP [ESI+0xbc]
            AccumulatedDamage = Rate * Simple->ScannerInterference * Dt + AccumulatedDamage;
        }
    }

    // FLD dt / FADD [ESI+0xc0] / FST [ESI+0xc0]
    AccumulatedDamageTime = Dt + AccumulatedDamageTime;

    // FCOMP 0.0f / TEST AH,0x44 / JNP -> return when equal.
    if (AccumulatedDamage == 0.0f)
        return;
    // FCOMP 1.0f (module storage = 0x3f800000) /
    // TEST AH,0x41 / JNZ -> return when the time is <= 1.0.
    if (AccumulatedDamageTime <= 1.0f)
        return;

    // CALL [EAX + 0xd4] on the raw object -- Common's CShip
    // vtable (module storage + 0xd4 = module storage) holds
    // CEqObj::get_total_max_hit_pts there. module storage FDIVR [ESI+0xbc]:
    // accumulated / total. module storage FST to the dt slot.
    float const Ratio = AccumulatedDamage / static_cast<CEqObj*>(Raw)->get_total_max_hit_pts();
    // FCOMP 0.01f (module storage = 0x3c23d70a) /
    // TEST AH,0x41 / JNZ -> return when Ratio <= 0.01.
    if (Ratio <= 0.01f)
        return;

    // a DamageList built inline on the stack:
    // operator new(0x14) self-linked head (+0x04), +0x08 = 0, byte +0x0c = 0,
    // +0x14 = +0x18 = 0 and +0x10 (the cause) = 8. No DamageList constructor
    // or set_cause call: the field is stored directly. 8 has no name in
    // DamageCause (common_misc.h names 0..5 only).
    DamageList Damage;
    Damage.Cause = static_cast<DamageCause>(8);

    apply_damage_ratio(Ratio, Damage);             // CALL the routine
    apply_damage_list(Damage);                     // CALL [EDX + 0x188]
    AccumulatedDamage = 0.0f;                      // MOV [ESI+0xbc],EBX
    // the list's nodes and head are freed
    // (the routine == operator delete): ~DamageList.
}

// (module storage, RET 8). EDI = this, EBP = the RAW
// CBase::Object (module storage), kept in a local at module storage.
bool Ship::apply_damage_ratio(float Ratio, DamageList& Damage)
{
    CObject* const Raw = Object;

    // FLD Ratio / module storage FCOMP 1.0f (module storage) /
    // TEST AH,0x41 / JNZ the branch when Ratio <= 1.0.
    if (Ratio > 1.0f)
    {
        // CALL the routine -- `[ECX+0x10]`, nulled unless
        // (ObjectClass & 0x103) == 0x103, i.e. ceqobj(); module storage
        // CSimple::get_hit_pts on it, FSTP as the float argument;
        // CALL [EBX + 0x1b0] (damage_hull), result ignored.
        damage_hull(ceqobj()->get_hit_pts(), Damage);
        Damage.bDestroyed = true;                  // MOV byte [ESI+0xc],1
        return true;                               // MOV AL,1
    }

    // MOV byte [ESP+0x13],0 -- the result byte.
    bool bAnyDestroyed = false;

    // CSimple::get_max_hit_pts on the raw object (ECX = EBP),
    // FMUL Ratio, module storage CALL [EAX + 0x1b0].
    if (damage_hull(static_cast<CSimple*>(Raw)->get_max_hit_pts() * Ratio, Damage))
        bAnyDestroyed = true;

    // MOV AL,[ESI+0xc] / TEST / JNZ -- equipment pass only while
    // the list does not say destroyed.
    if (!Damage.bDestroyed)
    {
        // PUSH 0x1ee0 / CEquipTraverser::CEquipTraverser(int);
        // ADD EBP,0xe4 -- CEqObj::EquipManager.
        CEquipTraverser Walk(0x1ee0);
        CEquipManager& Equipment = static_cast<CEqObj*>(Raw)->EquipManager;
        for (CEquip* Equip = Equipment.Traverse(Walk); Equip != nullptr;
             Equip = Equipment.Traverse(Walk))
        {
            // CALL [EAX + 0x38] -- CEquip slot 14,
            // GetMaxHitPoints (common_cequip.h slot order); FMUL Ratio;
            // CALL [EDX + 0x210] (damage_equip) / TEST AL.
            if (damage_equip(Equip, Equip->GetMaxHitPoints() * Ratio, Damage))
                bAnyDestroyed = true;
        }
    }

    // re-reads byte +0xc of the list.
    if (!Damage.bDestroyed)
    {
        // ADD EBP,0x144 -- CEqObj::ArchGroups. The traverser is
        // one byte set to 1 inline (module storage MOV byte [ESP+0x1c],1),
        // which is what CArchGrpTraverser's constructor does (AtStart).
        CArchGrpTraverser Walk;
        CArchGroupManager& Groups = static_cast<CEqObj*>(Raw)->ArchGroups;
        for (CArchGroup* Group = Groups.Traverse(Walk); Group != nullptr;
             Group = Groups.Traverse(Walk))
        {
            // CArchGroup::GetMaxHitPoints / 06d01408 FMUL Ratio
            // on the unrounded ST0 / 06d01412 FSTP float argument /
            // CALL [EDX + 0x218] (damage_group) / TEST AL.
            if (damage_group(Group, (float)(Group->GetMaxHitPoints() * Ratio), Damage))
                bAnyDestroyed = true;
        }
    }

    return bAnyDestroyed;                          // MOV AL,[ESP+0x13]
}

// (module storage, RET 4). ESI = this; EBX = 0 throughout.
// Stack base S = ESP after the four register pushes of the eject path; the
// FLootRecord is built at S+0x2c.
void Ship::tick_jettison(float Dt)
{
    // FLD +0xac / FCOMP 0.0f, then the queue size
    // (+0xa8) decides the arm.
    if (Jettison.Items.size() == 0u)
    {
        // TEST AH,0x44 / JNP -> return when the timer is 0.
        if (JettisonTimer == 0.0f)
            return;
        // FLD / FSUB dt / FST, FCOMP 0.0f,
        // TEST AH,0x41 / JP -> return while it is still > 0 (or unordered).
        JettisonTimer = JettisonTimer - Dt;
        if (!(JettisonTimer <= 0.0f))
            return;
        // the owed bay closes, one
        // CShip::close_bay on the raw [ESI+0x10] each (its float result is
        // dropped, module storage FSTP ST0).
        while (PendingBayCloses > 0)
        {
            static_cast<CShip*>(Object)->close_bay();
            --PendingBayCloses;
        }
        JettisonTimer = 0.0f;
        return;
    }

    // TEST AH,0x44 / JNP -- only a running timer counts down;
    // clamps it to 0 once it is <= 0 (JP skips
    // the clamp while it is > 0 or unordered).
    if (JettisonTimer != 0.0f)
    {
        JettisonTimer = JettisonTimer - Dt;
        if (JettisonTimer <= 0.0f)
            JettisonTimer = 0.0f;
    }
    // FCOMP 0.0f / TEST AH,0x44 / JP -> return
    // unless the timer is exactly 0.
    if (JettisonTimer != 0.0f)
        return;
    // the raw CShip's BayState (+0x2ac) must be 2.
    if (static_cast<CShip*>(Object)->BayState != 2)
        return;

    // EDI = &head->Next->Value: the front item.
    EquipDesc& Desc = Jettison.Items.front();

    // +0x00 .. +0x1c zeroed, +0x68 = 1, +0x69 = 0.
    // The rest is filled below before anything reads it.
    FLootRecord Record;
    Record.Id = 0u;
    Record.Arch = nullptr;
    Record.HitPts = 0.0f;
    Record.ContentsArch = nullptr;
    Record.ContentsHitPts = 0.0f;
    Record.Count = 0;
    Record.Field_0x18 = 0u;
    Record.Field_0x1c = 0u;
    Record.Field_0x68 = 1;
    Record.bMission = 0;

    // CFLIDMaker::GetInstance, the id counter at
    // +4 masked with 0x3fffffff and post-incremented (inline
    // CFLIDMaker::AllocateObjectID).
    Record.Id = CFLIDMaker::GetInstance()->AllocateObjectID();

    // Archetype::GetEquipment(Desc +0x04).
    Archetype::Equipment* const Contents = Archetype::GetEquipment(Desc.ArchId);
    Record.ContentsArch = Contents;
    // FLD Desc +0x10 / FMUL Arch +0x1c / FSTP rec +0x10.
    Record.ContentsHitPts = Desc.Health * Contents->HitPts;
    // CALL [EDX + 0x1c] -- get_loot_appearance.
    Archetype::Equipment* const LootArch = Contents->get_loot_appearance();
    Record.Arch = LootArch;
    // same nickname -> the contents' hit points,
    // else the loot archetype's own (+0x1c).
    if (LootArch->Nickname.Id == Contents->Nickname.Id)
        Record.HitPts = Record.ContentsHitPts;
    else
        Record.HitPts = LootArch->HitPts;

    // MOV EBP,[EDI + 0x14] / module storage -> rec +0x14.
    int const Count = Desc.Count;
    Record.Count = Count;

    // the routine(&rec +0x2c, &rec +0x38, &rec +0x20).
    compute_jettison_launch(&Record.Position, &Record.Orientation, &Record.Velocity);

    // the spin: two rand() draws, each
    // rand() * 2^-15 (module storage = 0x38000000) * (double)0.8f
    // (module storage) - (double)0.4f (module storage), stored as a float;
    // the first scales the orientation's column 0, the second its column 2.
    Matrix const& M = Record.Orientation;
    float const A = static_cast<float>(
        static_cast<double>(static_cast<float>(rand()) * 3.0517578125e-05f) *
        static_cast<double>(0.8f) - static_cast<double>(0.4f));
    float const Ax = M.E[0][0] * A;
    float const Ay = M.E[1][0] * A;
    float const Az = M.E[2][0] * A;
    float const B = static_cast<float>(
        static_cast<double>(static_cast<float>(rand()) * 3.0517578125e-05f) *
        static_cast<double>(0.8f) - static_cast<double>(0.4f));
    // column 2 * B, plus the column-0 terms.
    Record.AngularVelocity.x = M.E[0][2] * B + Ax;
    Record.AngularVelocity.y = M.E[1][2] * B + Ay;
    Record.AngularVelocity.z = M.E[2][2] * B + Az;

    // CALL [EAX + 0x20] (get_id), then
    // on OwnerSystem (+0x28) with (id, 1, &rec, 1).
    OwnerSystem->create_loot(get_id(), 1, &Record, 1);

    // EquipDesc::set_count(count - rec.Count).
    Desc.set_count(Count - Record.Count);

    // CMP [EDI + 0x14],EBX / JNZ -- an emptied stack is dropped:
    // one more bay close owed (module storage), the front node unlinked and
    // freed (module storage .. 06d0182b), size - 1 (module storage): pop_front.
    if (Desc.Count == 0)
    {
        ++PendingBayCloses;
        Jettison.Items.erase(Jettison.Items.begin());
    }

    // COMMON.DLL::CShip::JETTISON_CLEARANCE_TIME.
    JettisonTimer = CShip::JETTISON_CLEARANCE_TIME;
}

// (module storage, RET 0xc). EBX = this; ESI = the raw
// CBase::Object's archetype (+0x88) read as Archetype::Ship, with no test.
// Stack base S = ESP after the three register pushes.
void Ship::compute_jettison_launch(Vector* OutPos, Matrix* OutOrient, Vector* OutVel)
{
    Archetype::Ship const* const Arch =
        static_cast<Archetype::Ship const*>(Object->Archetype);

    // both bay hardpoints must exist on the
    // engine instance (IObjInspect +0x1c get_index): HpBaySurface (+0xe0)
    // into S+0xc, HpBayExternal (+0xe4) into S+0x10.
    long Surface = 0;
    long External = 0;
    Vector Dir;
    if (FindHardpoint(get_index(), Arch->HpBaySurface.Value, Surface) &&
        FindHardpoint(get_index(), Arch->HpBayExternal.Value, External))
    {
        // HardpointPosition(Surface, name, OutPos, OutOrient)
        HardpointPosition(Surface, Arch->HpBaySurface.Value, OutPos, OutOrient);
        // HardpointPosition(External, name, &S+0x20, 0)
        Vector ExternalPos;
        HardpointPosition(External, Arch->HpBayExternal.Value, &ExternalPos, nullptr);
        // exterior minus surface.
        Dir.x = ExternalPos.x - OutPos->x;
        Dir.y = ExternalPos.y - OutPos->y;
        Dir.z = ExternalPos.z - OutPos->z;
    }
    else
    {
        // the raw object's position (+0x2c).
        *OutPos = Object->WorldPosition;
        // its orientation (+0x08), REP MOVSD 9.
        *OutOrient = Object->Orientation;
        // minus column 1 of that matrix
        // (+0x04 / +0x10 / +0x1c, each FCHS).
        Dir.x = -OutOrient->E[0][1];
        Dir.y = -OutOrient->E[1][1];
        Dir.z = -OutOrient->E[2][1];
    }

    // the direction goes into OutVel.
    *OutVel = Dir;

    // speed = (rand() * 2^-15 * 0.5 + 0.75)
    // (module storage / module storage, doubles) * JETTISONED_CARGO_VELOCITY.
    double const Speed =
        (static_cast<double>(static_cast<float>(rand()) * 3.0517578125e-05f) * 0.5 + 0.75) *
        static_cast<double>(JETTISONED_CARGO_VELOCITY);
    // |OutVel| = sqrt((x*x + y*y) + z*z); the scale
    // Speed / |OutVel| is stored as a float (module storage FSTP [ESP+0x34]).
    double const Length = sqrt(
        (static_cast<double>(OutVel->x) * OutVel->x + static_cast<double>(OutVel->y) * OutVel->y) +
        static_cast<double>(OutVel->z) * OutVel->z);
    float const Scale = static_cast<float>(Speed / Length);
    OutVel->x = Scale * OutVel->x;
    OutVel->y = Scale * OutVel->y;
    OutVel->z = Scale * OutVel->z;

    // CALL [EAX + 0x4] -- IObjInspect::get_velocity on this,
    // added component by component (module storage .. 06d01a05).
    Vector const Velocity = get_velocity();
    OutVel->x = Velocity.x + OutVel->x;
    OutVel->y = Velocity.y + OutVel->y;
    OutVel->z = Velocity.z + OutVel->z;
}

// (module storage) — Ship vtable +0x16c. Term-by-term vs listing.
bool Ship::update(float Dt)
{
    // CALL the routine (EqObj::update)
    if (!EqObj::update(Dt))
        return false;

    // MOV EAX,[ESI+0x90] / TEST / JNZ death-sequence
    if (DeathSequenceObjId == 0u)
    {
        // .. module storage -- ECX = [ESI+0x10] narrowed with
        // (ObjectClass & 3) == 3, then module storage reads CSimple::OwnerPlayer
        // (+0xb4) through the result with NO null test. The CObject::update call
        // itself goes through the raw [ESI+0x10] (ECX from module storage on the
        // unowned path, reloaded at module storage on the other).
        CObject* const Raw = Object;
        CSimple* Simple = nullptr;
        if (Raw != nullptr &&
            (static_cast<unsigned>(Raw->ObjectClass) & 0x3u) == 0x3u)
            Simple = static_cast<CSimple*>(Raw);

        if (Simple->OwnerPlayer == 0u)
        {
            // MOV EAX,[->CObject::NO_SCANNER] / module storage
            // CALL [EDX + 0x50] (CObject::update).
            Raw->update(Dt, CObject::NO_SCANNER);
        }
        else if (SinglePlayer() == 0)
        {
            // .. module storage -- CObject::UPDATE_DUMB.
            Object->update(Dt, CObject::UPDATE_DUMB);
        }
        // else: owned + single player -- module storage JNZ the branch skips
        // CObject::update.

        tick_jettison(Dt);
        tick_zone_damage(Dt);
        return true;
    }

    // Death-sequence arm (module storage ..). ECX = [ESI+0x10], the raw
    // CBase::Object (module storage), read without a class test or null test.
    CObject* const DeathObj = Object;
    if (SinglePlayer() != 0)
    {
        // MOV AL,[ECX+0x214] -- CShip::bJumpedOut, no class test.
        if (((CShip*)DeathObj)->bJumpedOut == 0)
            return true;
        // / module storage -- PUSH ESI / CALL the routine.
        Ship_QueueDying(this);
        DeathSequenceObjId = 0u;
        return true;
    }

    // MP (module storage): bail while CShip+0x2bd (bIsPlayer) is set.
    if (static_cast<CShip*>(DeathObj)->bIsPlayer != 0)
        return true;

    // FLD/FSUB/FST, FCOMP against 0.0f
    // (module storage), `TEST AH,0x41 / JP` -> leave unless <= 0.
    DeathSequenceTimer -= Dt;
    if (DeathSequenceTimer <= 0.0f)
    {
        // / module storage -- PUSH 1 / CALL
        // CObject::enable_collisions_r with ECX still [ESI+0x10].
        DeathObj->enable_collisions_r(true);
        Ship_QueueDying(this);
        DeathSequenceTimer = 0.0f;
        DeathSequenceObjId = 0u;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Ship's GameObject / EqObj overrides and its IObjAffect table
// (2026-09-24, claude-code:w5-server-06d0). In every IObjAffect body baseline
// runs with ECX = the IObjAffect sub-object (this + 4), so `[ECX+0xc]` is the
// raw CBase::Object (+0x10), `[ECX+0x24]` is OwnerSystem (+0x28) and
// `ECX - 4` is the wrapper; the port's `this` is the wrapper itself.
// ---------------------------------------------------------------------------

// (RET 8), all eleven instructions: the two arguments pushed
// again, CALL the routine (GameObject's +0x170 body, direct), then
// `MOV dword ptr [ESI+0x90],0` (module storage).
void Ship::enter_system(StarSystem* Sys, PhySys::CreateParms const* Parms)
{
    GameObject::enter_system(Sys, Parms);
    DeathSequenceObjId = 0u;
}

// (RET 4):
// CMP [EDI+0x10],6 / JNZ -> base      ; DamageList cause
// the (& 3) == 3 accessor, OwnerPlayer read
//                               through it unguarded; non-zero -> base
// PUSH 0 / PUSH 1 / PUSH 0, CALL [EDX+0x50] on
//                               this+4: IObjAffect slot_50(false, true, 0)
// CALL the routine (EqObj +0x188 body, direct)
void Ship::apply_damage_list(DamageList& Damage)
{
    if (Damage.Cause == static_cast<DamageCause>(6) && csimple()->OwnerPlayer == 0u)
        slot_50(false, true, 0);
    EqObj::apply_damage_list(Damage);
}

// (RET 0x14). EBP = this, EBX = the raw CBase::Object read as a
// CShip with no test (module storage).
// EqObj::light_fuse(SourceId, FuseId, SubObjId,
//                               AtT, Lifetime) (the routine, direct); its
//                               byte kept in the Lifetime slot (module storage)
//                               and returned on every path
// not lit -> return it
// FuseId.Id != DEATH_COMM_FUSE.Id -> return it
// CShip +0x1d4 .. +0x1d8, 8-byte entries whose
//                               first dword is a Watchable (IObjRW + 8)
// the same over CShip +0x1f8 .. +0x1fc
// Per entry: a null Watchable, or one whose IObjRW adjust is null, is skipped
// (module storage / 06d01afb); so is one whose is_player() (IObjInspect +0xbc)
// says true (module storage). Otherwise its cobject() (+0x150) narrowed with
// (ObjectClass & 0x503) == 0x503 gives CEqObj::get_behavior_interface -- the
// narrowing is NOT tested before the call (module storage) -- and a non-null
// behavior gets a {7, &<this ship's get_id()>} record through its vtable
// slot 0 (module storage .. 06d01b68): "my leader's death fuse is lit".
bool Ship::light_fuse(unsigned int SourceId, ID_String const& FuseId,
                      unsigned short SubObjId, float AtT, float Lifetime)
{
    bool const bLit = EqObj::light_fuse(SourceId, FuseId, SubObjId, AtT, Lifetime);
    if (!bLit)
        return bLit;
    if (FuseId.Id != DEATH_COMM_FUSE.Id)
        return bLit;

    CShip* const Raw = static_cast<CShip*>(Object);                  // EBX

    // The per-entry body of both loops (module storage .. 06d01b68 and
    // are the same code twice). *Entry is read again
    // for the cobject() call, and the ship's own id is fetched per follower.
    auto NotifyFollower = [this](FObjRWWatcher const& Entry)
    {
        // W = [slot]; W != 0 -> W - 8, tested for 0 (CShip's typed watcher, common_cobject.h).
        IObjRW* const Follower = Entry.get();
        if (Follower == nullptr)
            return;
        if (Follower->is_player())
            return;
        IObjRW* const FollowerAgain = Entry.get();
        CObject* const FollowerObj = const_cast<CObject*>(FollowerAgain->cobject());
        CEqObj* const FollowerEqObj =
            (FollowerObj != nullptr &&
             (static_cast<unsigned int>(FollowerObj->ObjectClass) & 0x503u) == 0x503u)
                ? static_cast<CEqObj*>(FollowerObj) : nullptr;
        IBehaviorManager* const Behavior = FollowerEqObj->get_behavior_interface();
        if (Behavior == nullptr)
            return;
        // {7, &get_id()} to IBehaviorManager
        // slot 0 (Common the branch): "my leader's death fuse is lit".
        unsigned int LeaderId = get_id();
        FBehaviorManagerEvent FuseEvent;
        FuseEvent.Type = 7;
        FuseEvent.Data = &LeaderId;
        Behavior->Slot00(&FuseEvent);
    };

    // CShip +0x1d4; the end (+0x1d8) is re-read
    // after every entry (module storage).
    for (std::vector<FObjRWWatcher>::const_iterator Entry = Raw->FormationFollowers.begin();
         Entry != Raw->FormationFollowers.end(); ++Entry)
        NotifyFollower(*Entry);
    // CShip +0x1f8, end +0x1fc re-read likewise.
    for (std::vector<FObjRWWatcher>::const_iterator Entry = Raw->FollowFollowers.begin();
         Entry != Raw->FollowFollowers.end(); ++Entry)
        NotifyFollower(*Entry);
    return bLit;
}

// (RET 4): CALL the routine (EqObj +0x1f4 body, direct), then
// for every node of the jettison queue (+0xa4, module storage .. 06d01e64) the
// inlined vector insert(end(), 1, node->Value) -- _Ufill / _Ucopy / the
// reallocation through operator new(n * 0x20) -- i.e. push_back.
void Ship::collect_droppable_equip(std::vector<EquipDesc>& Out)
{
    EqObj::collect_droppable_equip(Out);
    for (std::list<EquipDesc>::const_iterator It = Jettison.Items.begin();
         It != Jettison.Items.end(); ++It)
        Out.push_back(*It);
}

// (module storage, RET 0xc).
// PendingRequest (+0x9c) == 0 -> the client arm
// PendingRequest->OnResult(RequestType,
//                               Response, Arg3) (vtable slot 0)
// true -> return
// / 06d01eaf   PendingRequest = 0, Field_0x98 = 0
//   client arm:
// OwnerPlayer through the (& 3) == 3 accessor,
//                               unguarded, into a stack slot
// get_id()
// IClient +0x14c (SendServerRequestReturned) (&OwnerPlayer, id,
//                               RequestType, Response, Arg3)
// Field_0x98 = (RequestType == 0 && Response == 3)
void Ship::request_returned(int RequestType, int Response, int Arg3)
{
    if (PendingRequest != nullptr)
    {
        if (!PendingRequest->OnResult(RequestType, Response, Arg3))
        {
            PendingRequest = nullptr;
            Field_0x98 = 0;
        }
        return;
    }
    unsigned int OwnerPlayer = csimple()->OwnerPlayer;
    unsigned int const Id = get_id();
    g_pClientInterface->SendServerRequestReturned(OwnerPlayer, Id, RequestType, Response, Arg3);
    Field_0x98 = 0;
    if (RequestType == 0 && Response == 3)
        Field_0x98 = 1;
}

// CShip::set_throttle (IAT 06d645c4) on [ECX+0xc]; return 0.
int Ship::set_throttle(float Throttle)
{
    static_cast<CShip*>(Object)->set_throttle(Throttle);
    return 0;
}

// `ADD ECX,0x230` on [ECX+0xc], CSteering::SetAxisThrottle
// (IAT 06d645c8); return 0.
int Ship::set_axis_throttle(Vector const& AxisThrottle)
{
    static_cast<CShip*>(Object)->Steering.SetAxisThrottle(AxisThrottle);
    return 0;
}

// CShip::set_nudge_vec (IAT 06d645cc); return 0.
int Ship::set_nudge_vec(Vector const& NudgeVec)
{
    static_cast<CShip*>(Object)->set_nudge_vec(NudgeVec);
    return 0;
}

// (module storage, RET 4):
// CShip::get_strafe_dir() == Dir -> return 0
// CEquipManager::FindFirst(0x20000) on
//                               CEqObj +0xe4 -- the engine, used untested
// its IsActive() (vt +0x04); active -> skip
// IObjAffect slot 19 (+0x4c) with (&<its
//                               GetID() widened to a dword>, 1, 0)
// CShip::set_strafe_dir(Dir)
// Always returns 0.
int Ship::set_strafe_dir(StrafeDir Dir)
{
    CShip* const Raw = static_cast<CShip*>(Object);
    if (Raw->get_strafe_dir() == Dir)
        return 0;
    CEquip* const Engine = Raw->EquipManager.FindFirst(0x20000u);
    if (!Engine->IsActive())
    {
        unsigned short const EngineId = Engine->GetID();
        slot_4c(&EngineId, true, 0);
    }
    static_cast<CShip*>(Object)->set_strafe_dir(Dir);
    return 0;
}

// (module storage, RET 4).
// CShip +0x1b8 is a BaseWatcher: W = Leader ?
//                               Leader + 8 : 0; when W differs from the watched
//                               pointer, BaseWatcher::set_pointer(W)
// OwnerPlayer through the (& 3) == 3 accessor,
//                               unguarded; 0 -> return 0
// get_formation_offset(offset) (IObjInspect +0xac)
// Leader ? Leader->get_id() : 0
// IClient +0x1e4 (SendServerFormationUpdate) (owner, leader id,
//                               &offset) -- FORMATION_UPDATE
// CShip::FormationLeader is the BaseWatcher itself (common_cobject.h); BaseWatcher::set is
// the compare + set_pointer this body inlines.
int Ship::set_formation_leader(IObjRW* Leader)
{
    CShip* const Raw = static_cast<CShip*>(Object);
    Watchable const* const Watched = (Leader != nullptr) ? static_cast<Watchable const*>(Leader)
                                                         : nullptr;
    Raw->FormationLeader.set(Watched);

    unsigned int const Owner = csimple()->OwnerPlayer;
    if (Owner != 0u)
    {
        Vector Offset;
        get_formation_offset(Offset);
        unsigned int const LeaderId = (Leader != nullptr) ? Leader->get_id() : 0u;
        g_pClientInterface->SendServerFormationUpdate(Owner, LeaderId, Offset);
    }
    return 0;
}

// (RET 4).
// Offset copied into CShip +0x1c0 (FormationOffset)
//                               on the raw object, untested
// OwnerPlayer through the (& 3) == 3 accessor,
//                               unguarded; 0 -> return 0
// leader = 0; get_formation_leader(leader)
//                               (IObjInspect +0xb0)
// leader ? leader->get_id() : 0
// IClient +0x1e4 (SendServerFormationUpdate) (owner, leader id, &Offset)
int Ship::set_formation_offset(Vector const& Offset)
{
    static_cast<CShip*>(Object)->FormationOffset = Offset;
    unsigned int const Owner = csimple()->OwnerPlayer;
    if (Owner != 0u)
    {
        IObjRW* Leader = nullptr;
        get_formation_leader(Leader);
        unsigned int const LeaderId = (Leader != nullptr) ? Leader->get_id() : 0u;
        g_pClientInterface->SendServerFormationUpdate(Owner, LeaderId, Offset);
    }
    return 0;
}

// / 06d02a60 / 06d02a70 / 06d02ac0 / 06d02ad0 -- each is
// `MOV ECX,[ECX+0xc]` and a JMP into the CShip export, whose FORMATION_RTYPE
// (or count) is the slot's result.
int Ship::add_formation_follower(IObjRW* Follower)
{
    return static_cast<int>(static_cast<CShip*>(Object)->add_formation_follower(Follower));
}

int Ship::remove_formation_follower(IObjRW* Follower)
{
    return static_cast<int>(static_cast<CShip*>(Object)->remove_formation_follower(Follower));
}

int Ship::get_formation_follower_count()
{
    return static_cast<CShip*>(Object)->get_formation_follower_count();
}

int Ship::add_follow_follower(IObjRW* Follower)
{
    return static_cast<int>(static_cast<CShip*>(Object)->add_follow_follower(Follower));
}

int Ship::remove_follow_follower(IObjRW* Follower)
{
    return static_cast<int>(static_cast<CShip*>(Object)->remove_follow_follower(Follower));
}

// CShip::set_follow_leader (IAT 06d64608); return 0.
int Ship::set_follow_leader(IObjRW* Leader)
{
    static_cast<CShip*>(Object)->set_follow_leader(Leader);
    return 0;
}

// CShip::set_follow_offset (IAT 06d64604); return 0.
int Ship::set_follow_offset(Vector const& Offset)
{
    static_cast<CShip*>(Object)->set_follow_offset(Offset);
    return 0;
}

// (RET 0xc), the jettison request. EDI = this + 4.
// OwnerSystem == 0 or is_destroyed() -> -5
// CEquipManager::FindByID(EquipId) on the raw
//                               object's +0xe4, CECargo::cast; null -> -2
// Count = min(Count, cargo count) (unsigned JC)
// controller event 0x23 {get_id(), word EquipId,
//                               Arch2Good(cargo arch id), Count}
// the cargo's pod (+0x20):
//   pod:
// a DamageList built inline: empty list, not
//                               destroyed, cause 0x16, inflictor get_id(),
//                               inflictor owner = OwnerPlayer through the
//                               (& 3) == 3 accessor, unguarded
// destroy_equip(pod, fate 4, list) (+0x224)
// apply_damage_list(list) (+0x188)
// ~DamageList, return 0
//   no pod:
// EquipDesc ctor, cargo GetEquipDesc (vt +0x28);
//                               false -> -5
// EquipDesc::set_count(Count)
// operator new(0x28) node linked before the queue
//                               sentinel (+0xa4), its value copy-constructed
//                               (the routine), queue size + 1 (+0xa8)
// CShip::open_bay(), the ST0 result dropped (FSTP ST0)
// outside single player CECargo::RemoveFromStack(Count)
// OwnerSystem the routine(this, EquipId, Count)
//   return 0
int Ship::slot_38(unsigned short EquipId, int Count, int SourceClient)
{
    (void)SourceClient;   // baseline never reads the third argument
    if (OwnerSystem == nullptr)
        return -5;
    if (is_destroyed())
        return -5;

    CECargo* const Cargo =
        CECargo::cast(static_cast<CEqObj*>(Object)->EquipManager.FindByID(EquipId));
    if (Cargo == nullptr)
        return -2;

    unsigned int Amount = static_cast<unsigned int>(Count);
    if (!(Amount < Cargo->Count))
        Amount = Cargo->Count;

    struct FControllerMsg23
    {
        unsigned int ObjId;
        unsigned short EquipId;
        unsigned short Pad;       // not written by baseline
        unsigned int GoodId;
        unsigned int Count;
    } Msg;
    Msg.ObjId = get_id();
    Msg.EquipId = EquipId;
    Msg.Pad = 0;
    Msg.GoodId = Arch2Good(Cargo->EquipArch()->get_id());
    Msg.Count = Amount;
    Server_ControllerDispatch(0, pub::Controller::EMessage::CargoJettisoned, &Msg);

    CEquip* const Pod = Cargo->CargoPod;
    if (Pod != nullptr)
    {
        DamageList Damage;
        Damage.bDestroyed = false;
        Damage.Cause = static_cast<DamageCause>(0x16);
        Damage.InflictorId = get_id();
        Damage.InflictorOwner = csimple()->OwnerPlayer;
        destroy_equip(Pod, static_cast<DamageEntry::SubObjFate>(4), Damage);
        apply_damage_list(Damage);
        return 0;
    }

    EquipDesc Desc;
    if (!Cargo->GetEquipDesc(Desc))
        return -5;
    Desc.set_count(static_cast<int>(Amount));

    // operator new(0x28) node linked before the head (+0xa4), the value copied
    // (the routine), ++size (+0xa8): push_back.
    Jettison.Items.push_back(Desc);

    static_cast<CShip*>(Object)->open_bay();
    if (!SinglePlayer())
        Cargo->RemoveFromStack(Amount);
    OwnerSystem->notify_jettison(this, EquipId, static_cast<int>(Amount));
    return 0;
}

// (RET 0xc).
// OwnerSystem == 0 or is_destroyed() -> -5
// SourceClient -1 -> 0
// single player with a SourceClient -> straight
//                               to the notification
// CShip::get_target() != Target or
//                               get_sub_target() != SubTarget -> changed:
//                               CShip::set_target(Target), set_sub_target(SubTarget)
// no SourceClient and nothing changed -> return 0
// OwnerSystem the routine(&get_id(),
//                               &SUBOBJ_ID_NONE, Target, &SubTarget, &SourceClient)
// Returns 0.
int Ship::set_target(IObjRW* Target, unsigned short SubTarget, int SourceClient)
{
    if (OwnerSystem == nullptr)
        return -5;
    if (is_destroyed())
        return -5;
    if (SourceClient == -1)
        SourceClient = 0;

    if (!(SinglePlayer() && SourceClient != 0))
    {
        bool bChanged = false;
        CShip* const Raw = static_cast<CShip*>(Object);
        if (Raw->get_target() != Target || Raw->get_sub_target() != SubTarget)
        {
            bChanged = true;
            Raw->set_target(Target);
            Raw->set_sub_target(SubTarget);
        }
        if (SourceClient == 0 && !bChanged)
            return 0;
    }

    unsigned int const Id = get_id();
    OwnerSystem->notify_set_target(Id, SUBOBJ_ID_NONE, Target, SubTarget, SourceClient);
    return 0;
}

// (RET 0xc).
// OwnerSystem == 0 or is_destroyed() -> -5
// SourceClient -1 -> 0
// single player with a SourceClient -> notify
// CEEngine::cast(FindFirst(0x20000)), untested
// engine +0x54 (bCruiseActive) == bEngage -> no change
//   changed:
// changed = 1
// bEngage and no SourceClient: IObjAffect
//                               slot 21 (+0x54) (0, 0) -- thrusters off first
// CEEngine::EngageCruise(bEngage, bArg2)
// no SourceClient and nothing changed -> return 0
// OwnerSystem the routine(&get_id(), bEngage,
//                               bArg2, &SourceClient)
// Returns 0.
int Ship::slot_50(bool bEngage, bool bArg2, int SourceClient)
{
    if (OwnerSystem == nullptr)
        return -5;
    if (is_destroyed())
        return -5;
    if (SourceClient == -1)
        SourceClient = 0;

    if (!(SinglePlayer() && SourceClient != 0))
    {
        bool bChanged = false;
        CEEngine* const Engine =
            CEEngine::cast(static_cast<CEqObj*>(Object)->EquipManager.FindFirst(0x20000u));
        if (Engine->bCruiseActive != static_cast<unsigned char>(bEngage))
        {
            bChanged = true;
            if (bEngage && SourceClient == 0)
                slot_54(false, 0);
            Engine->EngageCruise(bEngage, bArg2);
        }
        if (SourceClient == 0 && !bChanged)
            return 0;
    }

    unsigned int const Id = get_id();
    OwnerSystem->notify_cruise(Id, bEngage, bArg2, SourceClient);
    return 0;
}

// (RET 8).
// OwnerSystem == 0 or is_destroyed() -> -5
// result = 0; SourceClient -1 -> 0
// single player with a SourceClient -> notify
// FindFirst(0x20000) (the engine, untested): its
//                               +0x54 byte (bCruiseActive) and IsActive()
// only with no SourceClient and bActivate:
// cruise on -> return -5
// engine inactive -> slot 19 (&<its id>, 1, 0)
// every 0x400 equip (CEquipTraverser): IsActive()
//                               != bActivate -> changed = 1, Activate(bActivate)
//                               (vt +0x30)
// no SourceClient and nothing changed -> result
// OwnerSystem the routine(&get_id(), bActivate,
//                               &SourceClient)
int Ship::slot_54(bool bActivate, int SourceClient)
{
    if (OwnerSystem == nullptr)
        return -5;
    if (is_destroyed())
        return -5;
    int const Result = 0;
    if (SourceClient == -1)
        SourceClient = 0;

    if (!(SinglePlayer() && SourceClient != 0))
    {
        bool bChanged = false;
        CEqObj* const Raw = static_cast<CEqObj*>(Object);
        CEquip* const Engine = Raw->EquipManager.FindFirst(0x20000u);
        unsigned char const bCruiseActive = static_cast<CEEngine*>(Engine)->bCruiseActive;
        bool const bEngineActive = Engine->IsActive();
        if (SourceClient == 0 && bActivate)
        {
            if (bCruiseActive != 0)
                return -5;
            if (!bEngineActive)
            {
                unsigned short const EngineId = Engine->GetID();
                slot_4c(&EngineId, true, 0);
            }
        }

        CEquipTraverser Walk(0x400);
        for (CEquip* Thruster = Raw->EquipManager.Traverse(Walk); Thruster != nullptr;
             Thruster = Raw->EquipManager.Traverse(Walk))
        {
            if (Thruster->IsActive() != bActivate)
            {
                bChanged = true;
                Thruster->Activate(bActivate);
            }
        }
        if (SourceClient == 0 && !bChanged)
            return Result;
    }

    unsigned int const Id = get_id();
    OwnerSystem->notify_thrusters(Id, bActivate, SourceClient);
    return Result;
}

// (RET 0x14).
// Field_0x98 already set -> return 0
// PendingRequest = Callback, Field_0x98 = 1 --
//                               both BEFORE the type is looked at
//   type 0 (module storage .. 06d026e7): controller event 0x12 {get_id(),
//     *TargetId, system id, byte (bFlag == 1)}; baseline zeroes +0..+0xc
//     (module storage..b1) but never writes bytes +0xd..+0xf, which stay
//     stack garbage -- the port zeroes them
//   type 1 (module storage .. 06d02727): 0x19 {get_id(), *TargetId, system id}
//   type 2 (module storage .. 06d02771): 0x14 {get_id(), *TargetId, *SecondId,
//     system id}
//   anything else (module storage): Field_0x98 = 0 -- PendingRequest keeps
//     the callback
// Every path returns 0. The system id is the routine, SystemInfo's slot 0.
int Ship::slot_5c(int RequestType, unsigned const* TargetId, unsigned const* SecondId,
                  int bFlag, RequestCB* Callback)
{
    if (Field_0x98 != 0)
        return 0;
    PendingRequest = Callback;
    Field_0x98 = 1;

    if (RequestType == 0)
    {
        unsigned int Msg[4] = { 0u, 0u, 0u, 0u };
        Msg[0] = get_id();
        Msg[1] = *TargetId;
        Msg[2] = OwnerSystem->SystemInfo->NicknameId;
        reinterpret_cast<unsigned char*>(&Msg[3])[0] = (bFlag == 1) ? 1u : 0u;
        Server_ControllerDispatch(0, pub::Controller::EMessage::DockRequest, Msg);
        return 0;
    }
    if (RequestType == 1)
    {
        unsigned int Msg[3];
        Msg[2] = OwnerSystem->SystemInfo->NicknameId;
        Msg[0] = get_id();
        Msg[1] = *TargetId;
        Server_ControllerDispatch(0, pub::Controller::EMessage::FormationRequest, Msg);
        return 0;
    }
    if (RequestType == 2)
    {
        unsigned int Msg[4];
        Msg[0] = get_id();
        Msg[1] = *TargetId;
        Msg[2] = *SecondId;
        Msg[3] = OwnerSystem->SystemInfo->NicknameId;
        Server_ControllerDispatch(0, pub::Controller::EMessage::TradelaneRequest, Msg);
        return 0;
    }
    Field_0x98 = 0;
    return 0;
}

// (RET 0xc; analysis splits it at module storage, the routine is
// the same body).
// / 06d027b5  PendingRequest = 0, Field_0x98 = 0
//   type 0 (module storage .. 06d027e3): controller event 0x10 {get_id(),
//     *TargetId, Arg3}
//   type 1 (module storage .. 06d02814): 0x1a {get_id(), *TargetId, system id}
//   type 2 (module storage .. 06d02843): 0x15 {get_id(), *TargetId, system id}
//   then every path (module storage .. 06d02870): set_invincible({2, 0, 0, 1.0f})
// Returns 0.
int Ship::slot_60(int RequestType, unsigned const* TargetId, int Arg3)
{
    PendingRequest = nullptr;
    Field_0x98 = 0;

    if (RequestType == 0)
    {
        unsigned int Msg[3];
        Msg[0] = get_id();
        Msg[1] = *TargetId;
        Msg[2] = static_cast<unsigned int>(Arg3);
        Server_ControllerDispatch(0, pub::Controller::EMessage::DockCancel, Msg);
    }
    else if (RequestType == 1)
    {
        unsigned int Msg[3];
        Msg[0] = get_id();
        Msg[1] = *TargetId;
        Msg[2] = OwnerSystem->SystemInfo->NicknameId;
        Server_ControllerDispatch(0, pub::Controller::EMessage::FormationLeave, Msg);
    }
    else if (RequestType == 2)
    {
        unsigned int Msg[3];
        Msg[0] = get_id();
        Msg[1] = *TargetId;
        Msg[2] = OwnerSystem->SystemInfo->NicknameId;
        Server_ControllerDispatch(0, pub::Controller::EMessage::TradelaneRelease, Msg);
    }

    FInvincibility Cmd = {};
    Cmd.Type = 2u;
    Cmd.bProtect = false;
    Cmd.bFromPlayers = false;
    Cmd.MaxHpLoss = 1.0f;
    set_invincible(Cmd);
    return 0;
}

// (RET 0x10).
// CShip::go_tradelane(EntranceRing, ExitRing,
//                               <this wrapper as IObjRW>, bFastExit, Speed)
// non-zero -> -5
// ExitRing ? ExitRing->get_id() : 0, then
//                               EntranceRing->get_id() (untested)
// OwnerSystem the routine(get_id(), entrance id,
//                               exit id, bFastExit, Speed)
// Returns 0.
int Ship::go_tradelane(IObjInspect const* EntranceRing, IObjInspect const* ExitRing,
                       bool bFastExit, float Speed)
{
    if (static_cast<CShip*>(Object)->go_tradelane(EntranceRing, ExitRing, this, bFastExit, Speed) != 0)
        return -5;
    unsigned int const ExitId = (ExitRing != nullptr) ? ExitRing->get_id() : 0u;
    unsigned int const EntranceId = EntranceRing->get_id();
    unsigned int const ShipId = get_id();
    OwnerSystem->notify_go_tradelane(ShipId, EntranceId, ExitId, bFastExit, Speed);
    return 0;
}

// (module storage, RET 0).
// CShip::stop_tradelane()
// CShip::get_tradelane_exit_ring()      -> third argument
// CShip::get_tradelane_entrance_ring()  -> second argument
// get_id()                               -> first argument
// OwnerSystem the routine
// Returns 0.
int Ship::stop_tradelane()
{
    CShip* const Raw = static_cast<CShip*>(Object);
    Raw->stop_tradelane();
    unsigned int const ExitId = Raw->get_tradelane_exit_ring();
    unsigned int const EntranceId = static_cast<CShip*>(Object)->get_tradelane_entrance_ring();
    unsigned int const ShipId = get_id();
    OwnerSystem->notify_stop_tradelane(ShipId, EntranceId, ExitId);
    return 0;
}

// (RET 4).
// Field_0xB8 clear -> return 0
// Field_0xB8 = 0
// set_invincible({1, 0, 0, 1.0f}) -- lifts the
//                               launch grant apply_launch_invincibility made
// controller event 0x28 {Id, get_id()}
// Returns 0.
int Ship::slot_70(unsigned Id)
{
    if (Field_0xB8 != 0)
    {
        Field_0xB8 = 0;
        FInvincibility Cmd = {};
        Cmd.Type = 1u;
        Cmd.bProtect = false;
        Cmd.bFromPlayers = false;
        Cmd.MaxHpLoss = 1.0f;
        set_invincible(Cmd);
        unsigned int Msg[2];
        Msg[0] = Id;
        Msg[1] = get_id();
        Server_ControllerDispatch(0, pub::Controller::EMessage::LaunchComplete, Msg);
    }
    return 0;
}

// (RET 4).
// controller event 0x26 {Arg, get_id()}
// set_invincible({2, 0, 0, 1.0f})
// Returns 0.
int Ship::slot_74(unsigned Arg)
{
    unsigned int Msg[2];
    Msg[0] = Arg;
    Msg[1] = get_id();
    Server_ControllerDispatch(0, pub::Controller::EMessage::JumpInComplete, Msg);
    FInvincibility Cmd = {};
    Cmd.Type = 2u;
    Cmd.bProtect = false;
    Cmd.bFromPlayers = false;
    Cmd.MaxHpLoss = 1.0f;
    set_invincible(Cmd);
    return 0;
}

// ---------------------------------------------------------------------------
// Solar -- create module storage
// ---------------------------------------------------------------------------

Solar::Solar()
{
}

Solar::~Solar()
{
    // CALL [module storage] -- Solar's +0x15c entry, which is
    // EqObj's destroy_notify (module storage) -- with the Solar vtables just
    // re-stamped; the virtual call made from this destructor resolves to the
    // same entry. ~EqObj (the routine) follows as the base destructor.
    destroy_notify();
}

// (module storage, RET 4), all 15 instructions:
// MOV [ESI+0x10],ECX            ; CBase::Object = Obj
// CALL COMMON.DLL::CObject::AddRef (ECX = Obj, untested)
// CBase::advise(true) on this + 0xc
// return Object != 0
bool Solar::attach_existing(CObject* Obj)
{
    Object = Obj;
    Obj->AddRef();
    advise(true);
    return Object != nullptr;
}

// +0x16c module storage (RET 4):
// CALL the routine (EqObj::update, direct); false -> return
//                   it (module storage)
// the (ObjectClass & 0x303) == 0x303 narrowing,
//                   no null test on the result
// CSolar vt +0x50 (CObject::update)(Dt,
//                   COMMON.DLL::CObject::UPDATE_DEFAULT); its result is dropped
// MOV AL,1
bool Solar::update(float Dt)
{
    if (!EqObj::update(Dt))
        return false;
    csolar()->update(Dt, CObject::UPDATE_DEFAULT);
    return true;
}

// +0x158 module storage (RET 8): EqObj::destroy (the routine, direct) with
// both arguments; false -> false. Otherwise the owning system (+0x28) drops the
// solar, the routine (module storage), and the answer is true.
bool Solar::destroy(int Kind, unsigned int ObjId)
{
    if (!EqObj::destroy(Kind, ObjId))
        return false;
    OwnerSystem->remove_solar(this);
    return true;
}

// +0x17c module storage (RET 8). A solar that is not destructible
// (CSolar +0x1a8 clear, read through the unguarded 0x303 narrowing) ignores a
// hit on its root (SUBOBJ_ID_ROOT: IAT 06d6401c, module storage .. 06d02d77);
// everything else is EqObj's (the routine, a tail JMP at module storage).
void Solar::damage_sub_obj(FSubObjDamage const& Hit, DamageList& Damage)
{
    if (!csolar()->bDestructible && Hit.SubObjId == SUBOBJ_ID_ROOT)
        return;
    EqObj::damage_sub_obj(Hit, Damage);
}

// The 0x24-byte record Solar's two root-hit overrides hand to controller event
// 0xd (module storage .. 06d02e5e / module storage .. 06d02fd5): nine dwords
// zeroed, then the solar's id, its system's id, the DamageList's inflictor
// (+0x14) and cause (+0x10), and 1.0f in the last one. +0x0c/+0x10/+0x18/
// +0x1c stay zero on both paths.
struct FSolarHitEvent
{
    unsigned int ObjId;        // +0x00  get_id()
    unsigned int SystemId;     // +0x04  the routine on OwnerSystem
    unsigned int InflictorId;  // +0x08  DamageList +0x14
    unsigned int Zero0c;       // +0x0c
    unsigned int Zero10;       // +0x10
    unsigned int Cause;        // +0x14  DamageList +0x10
    unsigned int Zero18;       // +0x18
    unsigned int Zero1c;       // +0x1c
    float Scale;               // +0x20  1.0f
};
static_assert(sizeof(FSolarHitEvent) == 0x24, "Solar root-hit event is 0x24 bytes");

// +0x180 module storage (RET 8).
// CSolar +0x1a8 (bDestructible) set -> EqObj
//                               body the routine and return
// a hit on a sub-object (Hit +8 !=
//                               SUBOBJ_ID_ROOT, IAT 06d6401c) -> the same EqObj body
//   root hit on an indestructible solar:
// FSolarHitEvent (system id read first, then
//                               the inflictor, then get_id()) -> controller
//                               event 0xd
// CEqObj::get_behavior_interface on the
//                               unguarded 0x303 narrowing
// non-null -> a type-0 behavior event whose
//                               payload is the same nine dwords reordered
//                               {system, inflictor, id, 0, 0, cause, 0, 0, 1.0f},
//                               through the behavior manager's vtable slot 0
// damage_power (+0x21c) with the munition's
//                               EnergyDamage (Archetype::Munition +0x84)
void Solar::munition_hit(FMunitionImpact const& Hit, DamageList& Damage)
{
    if (csolar()->bDestructible)
    {
        EqObj::munition_hit(Hit, Damage);
        return;
    }
    if (Hit.SubObjId != SUBOBJ_ID_ROOT)
    {
        EqObj::munition_hit(Hit, Damage);
        return;
    }

    FSolarHitEvent Event = {};
    Event.SystemId = OwnerSystem->SystemInfo->NicknameId;
    Event.InflictorId = Damage.InflictorId;
    Event.ObjId = get_id();
    Event.Cause = static_cast<unsigned int>(Damage.Cause);
    Event.Scale = 1.0f;
    Server_ControllerDispatch(0, pub::Controller::EMessage::ObjectDamaged, &Event);

    IBehaviorManager* const Behavior = static_cast<CEqObj*>(csolar())->get_behavior_interface();
    if (Behavior != nullptr)
    {

        // the 0xd record's nine dwords reordered into FBehaviorDamageEvent (common_iface.h):
        // {system, inflictor, id, 0, 0, cause, 0, 0, 1.0f}; event {0, &payload} to
        // IBehaviorManager::Slot00 (Common the branch).
        FBehaviorDamageEvent Payload;
        Payload.SystemId = Event.SystemId;
        Payload.InflictorId = Event.InflictorId;
        Payload.ObjId = Event.ObjId;
        Payload.Cause = static_cast<int>(Event.Cause);
        std::memcpy(&Payload.Damage, &Event.Zero18, sizeof(unsigned int));
        std::memcpy(&Payload.RelativeDamage, &Event.Zero1c, sizeof(unsigned int));
        Payload.RelativeHealth = Event.Scale;
        Payload.EquipType = 0u;
        Payload.CollisionGroupType = 0u;
        FBehaviorManagerEvent DamageEvent;
        DamageEvent.Type = 0;
        DamageEvent.Data = &Payload;
        Behavior->Slot00(&DamageEvent);
    }

    damage_power(Hit.Munition->EnergyDamage, Damage);
}

// +0x1fc module storage (RET 8). A destructible solar (CSolar +0x1a8) takes
// EqObj's hull pass (the routine, module storage). An indestructible one only
// reports the blast: the FSolarHitEvent above to controller event 0xd
// (module storage .. 06d02fdd, the system id read first) and the same type-0
// behavior event (module storage .. 06d03063) -- no damage at all. The Blast
// itself is not read on that path.
void Solar::explosion_hit_hull(ExplosionDamageEvent const& Blast, DamageList& Out)
{
    if (csolar()->bDestructible)
    {
        EqObj::explosion_hit_hull(Blast, Out);
        return;
    }

    FSolarHitEvent Event = {};
    Event.SystemId = OwnerSystem->SystemInfo->NicknameId;
    Event.InflictorId = Out.InflictorId;
    Event.ObjId = get_id();
    Event.Cause = static_cast<unsigned int>(Out.Cause);
    Event.Scale = 1.0f;
    Server_ControllerDispatch(0, pub::Controller::EMessage::ObjectDamaged, &Event);

    IBehaviorManager* const Behavior = static_cast<CEqObj*>(csolar())->get_behavior_interface();
    if (Behavior != nullptr)
    {

        // the 0xd record's nine dwords reordered into FBehaviorDamageEvent (common_iface.h):
        // {system, inflictor, id, 0, 0, cause, 0, 0, 1.0f}; event {0, &payload} to
        // IBehaviorManager::Slot00 (Common the branch).
        FBehaviorDamageEvent Payload;
        Payload.SystemId = Event.SystemId;
        Payload.InflictorId = Event.InflictorId;
        Payload.ObjId = Event.ObjId;
        Payload.Cause = static_cast<int>(Event.Cause);
        std::memcpy(&Payload.Damage, &Event.Zero18, sizeof(unsigned int));
        std::memcpy(&Payload.RelativeDamage, &Event.Zero1c, sizeof(unsigned int));
        Payload.RelativeHealth = Event.Scale;
        Payload.EquipType = 0u;
        Payload.CollisionGroupType = 0u;
        FBehaviorManagerEvent DamageEvent;
        DamageEvent.Type = 0;
        DamageEvent.Data = &Payload;
        Behavior->Slot00(&DamageEvent);
    }
}

// .data:the field -- Solar.cpp's own lazily fetched math engine
// (module storage `MOV EAX,[the field]` / JNZ / CALL the routine / store).
static I3DMathEngine* GSolarMathEngine = nullptr;

// +0x250 module storage (RET 0xc). The type bits (+0x154) pick the branch:
//   0x40 or 0x800 (module storage / 06d03087) -- somewhere on a 500-unit ring
//     350 units behind the solar:
// the 0x30 bytes vt +0x10 returns copied (the
//                                 slot is ICF-folded with get_transform, so it
//                                 is the whole Transform: rotation + position)
// a = rand() * (1/32768.0f) * 2pi (the double
// ); local {cos(a) * 500.0,
//                                 sin(a) * 500.0, -350.0f}
// the field's TransformXform (+0x28)
//                                 (out, transform, local)
// OutPos = out, OutOrient = the rotation
//   0x400 (module storage) -- in place, turned round:
// OutPos = get_position() (vt +0x00)
// OutOrient = get_transform()'s rotation (vt +0x0c)
// with c = cos(pi), s = sin(pi) (x87, the pi
//                                 double module storage), for each column i:
//                                 row0[i] = row0*c + s*row2, row2[i] = c*row2 - row0*s
//                                 (both from the old values) -- a half turn about
//                                 the local Y axis
//   anything else (module storage):
//     FDUMP(0x100001, "%s(%d) : *** ERROR: %s", Solar.cpp, 0xeb, ...);
//     OutPos = {35000, 0, 35000}, OutOrient = identity.
void Solar::ChooseSystemArrivalLocation(GameObject* Arriving, Vector& OutPos, Matrix& OutOrient)
{
    (void)Arriving;   // pushed by both callers, never read
    unsigned int const TypeBits = get_type_bits();

    if ((TypeBits & 0x40u) != 0 || (TypeBits & 0x800u) != 0)
    {
        Transform const Xform = reinterpret_cast<Transform const&>(get_orientation());
        int const Random = rand();
        double const Angle = static_cast<double>(static_cast<float>(Random) * 3.0517578125e-05f)
                           * 6.2831854820251465;
        Vector Local;
        Local.x = static_cast<float>(cos(Angle) * 500.0);
        Local.y = static_cast<float>(sin(Angle) * 500.0);
        Local.z = -350.0f;
        if (GSolarMathEngine == nullptr)
            GSolarMathEngine = Server_Acquire3DMathEngine();
        Vector Out;
        GSolarMathEngine->TransformXform(reinterpret_cast<FVector&>(Out),
                                         reinterpret_cast<FTransform const&>(Xform),
                                         reinterpret_cast<FVector const&>(Local));
        OutPos = Out;
        OutOrient = Xform.Rotation;
        return;
    }

    if ((TypeBits & 0x400u) != 0)
    {
        OutPos = get_position();
        OutOrient = get_transform().Rotation;
        double const Pi = 3.1415927410125732;
        double const C = cos(Pi);
        double const S = sin(Pi);
        for (int Column = 0; Column < 3; ++Column)
        {
            double const Row0 = OutOrient.E[0][Column];
            double const Row2 = OutOrient.E[2][Column];
            OutOrient.E[0][Column] = static_cast<float>(Row0 * C + S * Row2);
            OutOrient.E[2][Column] = static_cast<float>(C * Row2 - Row0 * S);
        }
        return;
    }

    FDUMP(0x100001, "%s(%d) : *** ERROR: %s",
          "Server\\Space\\Solar.cpp", 0xeb,
          "Invalid type in ChooseSystemArrivalLocation()");
    OutPos.x = 35000.0f;
    OutPos.y = 0.0f;
    OutPos.z = 35000.0f;
    OutOrient.E[0][0] = 1.0f; OutOrient.E[0][1] = 0.0f; OutOrient.E[0][2] = 0.0f;
    OutOrient.E[1][0] = 0.0f; OutOrient.E[1][1] = 1.0f; OutOrient.E[1][2] = 0.0f;
    OutOrient.E[2][0] = 0.0f; OutOrient.E[2][1] = 0.0f; OutOrient.E[2][2] = 1.0f;
}

bool Solar::create(Archetype::Root* Arch)
{
    // . Same steps as Ship::create (CreateOfClass) except that
    // CObject::open (module storage) runs BEFORE CBase::advise(true)
    // (module storage). No null checks, as in baseline.
    Object = CObject::Alloc(CObject::ClassSolar);                  // PUSH 0x303
    Arch->create_instance(static_cast<IEngineInstance*>(Object));
    // IEngine::UpdateInstance(instance, 0, 0).
    NeuronRuntime::Engine->UpdateInstance(reinterpret_cast<void*>(Object->EngineInstanceId), 0, nullptr);
    Object->open(Arch);
    advise(true);
    return Object != nullptr;
}

bool Solar::find(unsigned int const& Id)
{
    // Solar vtable module storage + 0x168 = module storage -> module storage
    // `XOR AL,AL / RET 4` (the ICF body Loot/Bullet/Mine's find and MedAsteroid's
    // create share): a Solar is never found by id. No CObject::Find, no advise --
    // single player wraps an existing CSolar through attach_existing (the routine)
    // instead. Ship's +0x168 (module storage) is the only EqObj-family find that
    // calls CObject::Find.
    (void)Id;
    return false;
}

CSolar* Solar::csolar() const
{
    CObject* Obj = cobj();
    if (Obj == nullptr)
        return nullptr;
    if ((static_cast<unsigned>(Obj->ObjectClass) & 0x303u) != 0x303u)
        return nullptr;
    return static_cast<CSolar*>(Obj);
}

// ---------------------------------------------------------------------------
// Loot -- create module storage
// ---------------------------------------------------------------------------

Loot::Loot()
    : CheckTimer(0.0f)
    , UnseenLife(0.0f)
    , DecayAccum(0.0f)
{
    // baseline has no Loot constructor of its own: the spawner the routine runs
    // GameObject's (module storage the routine), zeroes +0x60/+0x64/+0x68
    // (module storage-06d08bb9) and stores the three Loot vtables.
}

Loot::~Loot()
{
}

// Loot::create (module storage) lives in server_loot.cpp.

bool Loot::find(unsigned int const& Id)
{
    // Loot vtable module storage + 0x168 = module storage -> module storage
    // `XOR AL,AL / RET 4` (the ICF body Solar/MedAsteroid's create share): a Loot is never
    // found by id, it is only ever created. No CObject::Find, no advise.
    (void)Id;
    return false;
}

// `MOV EAX,0x10000000 / RET`.
unsigned int Loot::get_type_bits() const
{
    return 0x10000000u;
}

CLoot* Loot::cloot() const
{
    CObject* Obj = cobj();
    if (Obj == nullptr)
        return nullptr;
    if ((static_cast<unsigned>(Obj->ObjectClass) & 0x803u) != 0x803u)
        return nullptr;
    return static_cast<CLoot*>(Obj);
}
