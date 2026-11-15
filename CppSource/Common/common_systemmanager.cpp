// Author: Giperion.

// Common.dll -- SystemManager (the per-ship AI system block behind IBehaviorManager::Sys):
// ctor/dtor, the +0x1364 record's destructor, the SystemManager-level helpers, the personality
// hook, InitSystemManager and UpdateSystemManager.
// CMN-SPLIT stage 1 (2026-10-14): the timer-table functions moved to common_sysmgr_timertable.cpp,
// the gun-group manager to common_sysmgr_gungroups.cpp, RepairManager and its helpers to
// common_repairmanager.cpp, ThrustManager to common_thrustmanager.cpp, all unchanged.

#include "common_systemmanager.h"
#include "common_systemmanager_internal.h" // CmnSysMgr:: -- WriteFloatAt / IsWatcherLive / WatchedObject
#include "common_aisteer.h"   // SustainedThresholdTimer, ConstructGunConvergenceState, RefreshWeaponFireControlConfig
#include "common_behavior.h"
#include "common_misc.h"      // SUBOBJ_ID_NONE
#include "common_iface.h"     // IBehaviorManager::Pathfind, IObjInspect, ObjRwFromWatchable
#include "common_pathfind.h"  // PathfindManager::IsCruiseUnobstructed (the routine)
#include "common_iustategraph.h" // ClearAiTargetVector, BuildAiTargetList (ScanGunGroupTargets)
#include "common_cobject.h"    // CEqObj::EquipManager / get_behavior_interface (ScanGunGroupTargets)
#include "common_cequip.h"     // CELauncher::cast / GetBarrelPosWS (ScanGunGroupTargets)
#include "common_cobjdb.h"     // IObjDB::FindById (AppendSystemManagerTimerListEntry)
#include "neuroncomponents.h"             // NEURONCOMPONENTS_API

#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <new>

using CmnSysMgr::WriteFloatAt;
using CmnSysMgr::IsWatcherLive;
using CmnSysMgr::WatchedObject;

// ============================================================================
// SystemManager
// ============================================================================
SystemManager::SystemManager()
    : Unknown0(0)
    , Owner(nullptr)
    , Watcher()
    , ObjectDb(nullptr)
    , Unknown14(0)
    , Owner1498(nullptr)
    , Watcher149C()
    , Unknown14A4(SUBOBJ_ID_NONE)
    , Repair()
    , PersonalityData()
{
    // Common 0x062e6260 (the routine), item 1779. Raw analysis Common
    // 241928-242031. baseline order: Watcher (+0x08) zeroed (module storage-062e6289),
    // Thrust.Owner (+0x18) = 0, Timers (the routine), GunGroups (the routine),
    // gun-convergence state (the routine), Targeting (the routine), the +0x1498 loot
    // sub-object's watcher and id (module storage-062e62f8), the RepairManager
    // (module storage-062e6394), SelfTarget's guards and interface pointers zeroed,
    // Personality ctor, then Owner/Unknown10/Unknown14 zeroed last (module storage-062e63e9).
    // Owner (+0x04) gets its real value from InitSystemManager (the routine), called by
    // IBehaviorManager's ctor right after this one.

    // Not written by baseline's ctor, zeroed here for determinism: Unknown0, Pad15, the rest
    // of the ThrustManager after its Owner dword (InitThrustManager fills it), Owner1498,
    // RepairManager::Owner/CheckTimer, and SelfTarget's cached slices (+0x14FC..+0x1577).
    std::memset(Pad15, 0, sizeof(Pad15));
    std::memset(&Thrust, 0, sizeof(Thrust));

    ConstructSystemManagerTimerTable(&Timers);
    ConstructSystemManagerGunGroupManager(&GunGroups);
    // +0x1364: GunConvergenceStateBytes is a real member; its ctor (the routine,
    // ) runs as a member initializer.

    // +0x142C: Targeting is a real TargetManager member; its own ctor (the routine,
    // ) runs as a member initializer.

    std::memset(Pad14A6, 0, sizeof(Pad14A6));

    // SelfTarget: baseline writes only the four guards (module storage-062e63c7) and the two
    // interface pointers (module storage and ExternalIface); the cached slices +0x00..+0x7B
    // are zeroed here for determinism.
    std::memset(&SelfTarget, 0, sizeof(SelfTarget));
}

// baseline has no out-of-line SystemManager destructor: the routine (IBehaviorManager's dtor)
// inlines it at module storage-062d265d, in this order, and then frees the block
// (the routine). Everything not listed (Thrust, GunGroups' arrays, TargetManager, the
// RepairManager, Personality) has no destructor work in baseline.

// The sub-objects with destructor work are RAII members (AGENTS RE rule 13: baseline's ctor
// unwind funclets call their out-of-line destructors the routine / the routine), so the
// members unwind in reverse declaration order, which is baseline's inlined order exactly:
// Watcher149C (module storage), the +0x1364 record (FGunConvergenceStateBlock, +0x14 then
// +0x0C, 062d25f5 / 062d2603), GunGroups' +0x08 target watcher (Sys+0x134, 062d2614),
// Timers (+0xE8 watcher, list, Targets buffer, 062d262c-062d2649), Watcher (+0x8, 062d2657).
SystemManager::~SystemManager()
{
}

// (item 1546). Commond27d0-062d282e: the BaseWatcher at +0x14,
// then the one at +0x0C, each set_pointer(0) when it watches something -- the member
// destructors of FollowLeaderWatcher and FormationLeaderWatcher, in that (reverse
// declaration) order.
FGunConvergenceStateBlock::~FGunConvergenceStateBlock()
{
}

// , item 1789. Common. "this" =
// &GunConvergenceStateBytes (the already-ported GunConvergenceState blob,
// common_aisteer.h -- its own two embedded BaseWatcher fields at Self+0x0C /
// Self+0x14 match this function's +0x1370/+0x1378 SystemManager-absolute
// offsets exactly: 0x1364+0x0C=0x1370, 0x1364+0x14=0x1378).

// Common module storage-062e6a0f, thiscall on the SystemManager, `RET 4`; the same
// block twice, for +0x1370 (062e6999-062e69cf) and then +0x1378 (062e69d4-062e6a0a):
//   Watched = [W]; 0 -> next; Watched - 8 == 0 -> next   (ADD EAX,-8; TEST; JZ)
//   ([W] ? [W] - 8 : 0)->get_id()  (IObjInspect slot 8, CALL [vt+0x20], thiscall)
//   != TargetId -> next
//   [W] == 0 -> next, else BaseWatcher::set_pointer(W, 0)
// The trailing `[W] != 0 -> set_pointer(0)` is BaseWatcher::set(nullptr).
void ClearGunConvergenceStateWatcherIfTarget(SystemManager* Self, int TargetId)
{
    BaseWatcher* const Watchers[2] =
    {
        &Self->GunConvergenceStateBytes.FormationLeaderWatcher, // Sys + 0x1370
        &Self->GunConvergenceStateBytes.FollowLeaderWatcher,    // Sys + 0x1378
    };

    for (int Index = 0; Index < 2; ++Index)
    {
        BaseWatcher* const W = Watchers[Index];
        IObjRW* const Watched = ObjRwFromWatchable(const_cast<Watchable*>(W->target()));
        if (Watched == nullptr)
            continue;
        if (static_cast<int>(Watched->get_id()) != TargetId)
            continue;
        W->set(nullptr);
    }
}

// , item 1787. Common / raw analysis
// Common.
void SetSystemManagerSharedListHead(SystemManager* Self, void* NewHead)
{
    Self->ObjectDb = static_cast<IObjDB*>(NewHead);
    Self->Timers.ObjectDb = static_cast<IObjDB*>(NewHead);

    // `MOV EAX,[EDX+0x38]; ADD EAX,8; MOV EAX,[EAX]` --
    // Timers.Owner is dereferenced with no null test (baseline has none), then the
    // watched pointer is tested for 0, for 8 (`ADD EAX,-8; TEST`) and NewHead for 0.
    SystemManager* Owner = Self->Timers.Owner;
    Watchable const* WatcherTarget = Owner->Watcher.target();
    if (WatcherTarget != nullptr && reinterpret_cast<std::uintptr_t>(WatcherTarget) != 8 && NewHead != nullptr)
    {
        // Absolute SystemManager+0x50/+0x48/+0x44 == Timers-relative
        // +0x1C/+0x14/+0x10: this is `bBuilt = 0; Targets.End = Targets.Begin;`,
        // i.e. empty the cached AI target list so the next
        // BuildAiTargetList rebuilds it from scratch. baseline inlines
        // vector::erase(begin, end) (module storage-062e6938): the 0x88-byte
        // element move loop runs from End to End (zero iterations), then End = Begin.
        Self->Timers.bBuilt = 0;
        Self->Timers.Targets.End = Self->Timers.Targets.Begin;
    }

    // `LEA ECX,[EDX+0x142c]; JMP 0x062ed730` -- a
    // TargetManager member taking NewHead whose body is the ICF-folded
    // `RET 4`. It writes nothing, so nothing is done here. (The port used to
    // memcpy NewHead over SystemManager+0x142C, which is Targeting's vfptr.)
}

// ============================================================================
// , item 1788 -- SystemManager's "personality changed" hook.
// Common, 15 instructions, `this` == ECX == the
// SystemManager. Sole caller module storage is
// IBehaviorManager::set_personality.
// ============================================================================
void NotifySystemManagerPersonalityChanged(SystemManager* Self)
{
    // LEA ECX,[ESI+0x12c]; CALL the routine
    (void)Self->GunGroups.RefreshFromPersonality();

    // LEA ECX,[ESI+0x34];  CALL the routine
    (void)RefreshSystemManagerTimerThresholdsFromPersonality(&Self->Timers);

    // LEA ECX,[ESI+0x14a8]; CALL the routine -- item 1880, on the
    // RepairManager (whose +0x04 Owner the routine writes at module storage).
    (void)RefreshRepairManagerFromPersonality(&Self->Repair);

    // the block's Owner (+0x136c) tested, then its
    // JobParams.bForceAttackFormation (+0x1958) copied to the block's +0xC3 (Sys+0x1427).
    // Item 1786's own tick does the byte-identical copy at module storage.
    SystemManager* const StateOwner = Self->GunConvergenceStateBytes.Owner;
    if (StateOwner != nullptr)
        Self->GunConvergenceStateBytes.bForceAttackFormation = StateOwner->PersonalityData.JobParams.bForceAttackFormation;
}

// ============================================================================
// SystemManager initialisation and per-tick update (items 1785, 1786, 1837).
// 2026-09-24, claude-code:w3-behaviormgr-sys.
// ============================================================================
// , item 1785. Raw analysis Common (185 insns), `RET 8`,
// ESI = this, EBX = &Watcher, EBP = 0. Every "if live" below is its own re-read of the
// watcher in baseline (module storage, 062e652f, 062e6592, 062e6613, 062e6633, 062e666b).
bool InitSystemManager(SystemManager* Self, IObjRW* Object, IBehaviorManager* Manager)
{
    // EAX = Object ? Object + 8 : 0; if (EAX != [EBX])
    // set_pointer(EAX) -- BaseWatcher::set on the object's Watchable base.
    Self->Watcher.set(Object != nullptr ? static_cast<Watchable const*>(Object) : nullptr);

    Self->Owner = Manager;

    // [ESI] = watched object's get_id() (IObjInspect slot 8). The
    // object is dereferenced without a test.
    Self->Unknown0 = static_cast<int>(static_cast<IObjInspect*>(WatchedObject(Self->Watcher))->get_id());

    (void)ArmSystemManagerTimerTable(&Self->Timers, Self);

    if (IsWatcherLive(Self->Watcher))
    {
        // , FAiTarget::Init inlined (the guards cleared 7d, 7c, 7e,
        // 7f). The attitude reference comes from Timers.Owner (just set by
        // ArmSystemManagerTimerTable, which succeeds on this branch), dereferenced blind.
        IObjRW* const Own = WatchedObject(Self->Watcher);
        IObjRW* const TimersOwnerObject = WatchedObject(Self->Timers.Owner->Watcher);
        Self->SelfTarget.Init(TimersOwnerObject, Own);
    }

    if (IsWatcherLive(Self->Watcher))
        InitThrustManager(&Self->Thrust, Self);

    if (IsWatcherLive(Self->Watcher))
    {
        // , EDI = &GunGroups.
        SystemManagerGunGroupManager& G = Self->GunGroups;
        G.Owner = Self;                                         // [EDI+4] = ESI
        G.GunCount = 0;
        G.Field_0x834 = -1;
        (void)G.ResolveHardpoints();
        for (int Index = 0; Index < G.GunCount; ++Index)        // count re-read each pass (module storage)
            G.RandomizeSlotTiming(&G.Guns[Index]);
        // ScanPeriod = 1.0f (the field); ScanTimer = rand()/32768 * that.
        float const Period = 1.0f;
        G.ScanPeriod = Period;
        G.ScanTimer = (float)rand() * (1.0f / 32768.0f) * Period;
    }

    if (IsWatcherLive(Self->Watcher))
    {
        // [ESI+0x136c] = ESI -- the +0x1364 block's Owner.
        Self->GunConvergenceStateBytes.Owner = Self;
    }

    (void)Self->Targeting.Attach(Self);

    if (IsWatcherLive(Self->Watcher))
    {

        Self->Owner1498 = Self;
        (void)static_cast<IObjInspect*>(WatchedObject(Self->Watcher))
            ->find_equipment(&Self->Unknown14A4, 1, 0x100000);
    }

    if (IsWatcherLive(Self->Watcher))
    {
        // , EAX = &Repair.
        Self->Repair.Owner = Self;
        Self->Repair.CheckTimer = 0.0f;                         // [EAX+0x50]
        Self->Repair.Hull.Count = 0;                            // [EAX+0x18]
        Self->Repair.Shield.Count = 0;                          // [EAX+0x3c]
        (void)static_cast<IObjInspect*>(WatchedObject(Self->Watcher))->enumerate_cargo(&Self->Repair);
    }

    // the routine(this + 0x1344, this) -- GunGroups+0x1218.
    (void)Self->GunGroups.CmDroppers.Bind(Self);

    return true;                                                // MOV AL,1
}

// , item 1786. Raw analysis Common (183 insns), `RET 4`,
// ESI = this, EBP = Dt from module storage on.
void UpdateSystemManager(SystemManager* Self, float Dt)
{
    // re-resolve the two object pointers and clear the four
    // per-tick cache latches while the watcher is live. Timers.Owner is dereferenced blind.
    if (IsWatcherLive(Self->Watcher))
    {
        IObjRW* const Own = WatchedObject(Self->Watcher);
        IObjRW* const TimersOwnerObject = WatchedObject(Self->Timers.Owner->Watcher);
        Self->SelfTarget.Init(TimersOwnerObject, Own);          // FAiTarget::Init inlined
    }

    // bBuilt = 0, then vector::erase(begin, end) on the AI target
    // list -- the routine's copy runs from End to End (no element moves), JustifyNode::Execute
    // is DestroyAiTargetRange, End = Begin. That is ClearAiTargetVector exactly.
    Self->Timers.bBuilt = 0;
    ClearAiTargetVector(&Self->Timers.Targets);

    Self->Targeting.UpdateCommitTimer(Dt);
    Self->GunGroups.UpdateTimers(Dt);
    Self->GunGroups.CmDroppers.Update(Dt);                      // (+0x1344)

    // Thrust.bCruiseUnobstructed = Thrust.Owner->Owner->Pathfind's
    // the cruise gate engage_cruise reads.
    Self->Thrust.bCruiseUnobstructed =Self->Thrust.Owner->Owner->Pathfind->IsCruiseUnobstructed() ? 1 : 0;

    // while the object is in cruise but the gate is closed,
    // disengage. The watched object is dereferenced blind.
    bool bCruise = false;
    (void)static_cast<IObjInspect*>(WatchedObject(Self->Thrust.Owner->Watcher))->is_cruise_active(bCruise);
    if (bCruise && Self->Thrust.bCruiseUnobstructed == 0)
        Self->Thrust.engage_cruise(false);

    UpdateSystemManagerTimerTable(&Self->Timers, Dt);

    // , EDI = &Repair. Every three seconds (strictly past 3.0f,
    // ; FCOMP + TEST AH,0x41 + JNZ) the cargo is re-tallied.
    RepairManager& Repair = Self->Repair;
    double const CheckTimer = static_cast<double>(Dt) + static_cast<double>(Repair.CheckTimer);
    Repair.CheckTimer = static_cast<float>(CheckTimer);
    if (CheckTimer > 3.0)
    {
        Repair.CheckTimer = 0.0f;
        Repair.Hull.Count = 0;
        Repair.Shield.Count = 0;
        (void)static_cast<IObjInspect*>(WatchedObject(Repair.Owner->Watcher))->enumerate_cargo(&Repair);
    }
    Repair.Hull.Update(Dt, WatchedObject(Repair.Owner->Watcher));
    Repair.Shield.Update(Dt, WatchedObject(Repair.Owner->Watcher));

    Self->GunConvergenceStateBytes.Update(Dt);

    // the personality-refresh latch. Unknown14 is set by
    // RefreshRepairManagerFromPersonality / RefreshWeaponFireControlConfig when they copied
    // another ship's parameters; it is consumed here once that borrowing no longer applies.
    if (Self->Unknown14 == 0)
        return;
    if (Self->SelfTarget.Iface->is_player())
        return;
    if (Self->PersonalityData.FormationParams.bLeaderMakesMeTougher == 0)   // byte +0x1820
        return;
    if (Self->GunConvergenceStateBytes.bLeaderSuppressed == 0)
    {
        // a live formation leader keeps the borrowed parameters.
        Watchable const* const Node = Self->GunConvergenceStateBytes.FormationLeaderWatcher.target();
        if (Node != nullptr && Node != (Watchable const*)8)
            return;
    }

    Self->Unknown14 = 0;
    Self->PersonalityData.FormationParams.bLeaderMakesMeTougher = 0;
    (void)Self->GunGroups.RefreshFromPersonality();
    (void)RefreshSystemManagerTimerThresholdsFromPersonality(&Self->Timers);
    (void)RefreshRepairManagerFromPersonality(&Self->Repair);

    // the same JobParams.bForceAttackFormation copy
    // NotifySystemManagerPersonalityChanged makes.
    SystemManager* const StateOwner = Self->GunConvergenceStateBytes.Owner;
    if (StateOwner != nullptr)
        Self->GunConvergenceStateBytes.bForceAttackFormation = StateOwner->PersonalityData.JobParams.bForceAttackFormation;
}
