// Author: Giperion.

// Common.dll -- TargetManager (common_iface.h): the AI target arbiter embedded in
// SystemManager at +0x142C (baseline run 0x062ed570-0x062eec50, vftable module storage).

// Split out of common_stubs.cpp and common_iface.cpp (CMN-SPLIT stage 1, 2026-10-14). The
// former free helpers whose `this` the listing proves to be the TargetManager --
// , the routine and the routine (UpdateAttackTargetOverride) -- are members
// now; their bodies are unchanged.

#include "common_iface.h"         // TargetManager, IBehaviorManager, IObjRW, ObjInspectOf
#include "common_systemmanager.h" // SystemManager
#include "common_aisteer.h"       // FAiTarget, ClampedDot3, GetCachedRange, GetIfacePositionOrZero
#include "common_aisteer_internal.h" // CmnAiSteer::InlineFastLength (baseline's inlined x87 fast length)
#include "common_iustategraph.h"  // BuildAiTargetList / GetAiTargetList / ResetAiTargetList
#include "common_pathfind.h"      // PathfindManager user-zone probes (the routine / the routine)
#include "common_cobject.h"       // CShip::get_targeted_enemy_count / get_formation_followers
#include "common_cobjdb.h"        // IObjDB -- TargetManager::GetScanResponse's FindById call
#include "common_misc.h"          // FastInvSqrt, SUBOBJ_ID_NONE
#include "common_universe.h"      // Universe::ISystem::Slot08, Geometry::Sphere

#include <cstdlib>
#include <cstring>

// ============================================================================
// AI-wingman/state-graph cluster -- sibling callees
// ============================================================================

// RE FINDING (2026-09-10, d206:ai-dispatch-1896, resolves the open question in
// src/include/common_iface.h): the routine's own "this" is NOT IBehaviorManager
// itself -- per Common, the routine is called with
// `this = (*(int*)(CallerThis+8)) + 0x142C`, i.e. it is a small embedded
// "wingman/attack-coordination" sub-object living at a fixed +0x142C offset
// inside some larger owner. Its own +4 field ("sysMgr" below) holds a
// back-pointer to that owner, whose own +4 field is in turn a back-pointer to
// the owning IBehaviorManager -- proven via Common
// (`EAX=[ESI+4]; ECX=[EAX+4]; CALL IBehaviorManager::get_current_directive`,
// i.e. the value two hops from "this" is passed as the literal `this` argument
// to a real, mangled-name IBehaviorManager method) and confirmed again by
// TargetManager::SetAttackState (062ee280, item 1898): `*(this+4)+4)+0x110` matches
// IBehaviorManager::ContentCbPtr (header +0x110) exactly.

// CONTRADICTION SETTLED (2026-09-17, claude:common-iustategraph, implementation tracker
// item 1896). This block and src/include/common_iface.h's own "RE NOTE" have
// disagreed since 2026-09-10 about what param_1 is. The raw listing settles it
// in THIS block's favour, and goes one step further than either note:
//   * The caller IS an IBehaviorManager -- but what it passes is
//     `IBehaviorManager+0x08` plus 0x142C, and common_iface.h's own
//     IBehaviorManager layout already documents +0x08 as `SystemManager*`.
//     So param_1 is `SystemManager + 0x142C`, i.e. SystemManager::Targeting
//     (TargetManager, common_systemmanager.h). common_iface.h's claim that param_1 is
//     IBehaviorManager itself is WRONG and should be deleted by whoever owns
//     that file; its supporting argument ("+0x54..+0x69 fall inside
//     IBehaviorManager's Opaque0c") is a coincidence -- those offsets land
//     just as neatly inside SystemManager's 0x6C-byte TargetManager.
//   * There is NO separate, unrecovered ">= 0x1958-byte IUStateGraph" class.
//     The intermediate object at `param_1+4` is the already-ported
//     SystemManager (sizeof 0x195C), and SystemManager+0x04 is
//     `IBehaviorManager* Owner`, which is exactly the two-hop chain the old
//     note described. Every "IUStateGraph field" this cluster touches is a
//     SystemManager field: +0x34 is the AI target list (see
//     common_iustategraph.h), +0x1364 is GunConvergenceStateBytes, +0x14FC is
//     the owner's own FAiTarget record, +0x1580 is ExternalIface, +0x1584 is
//     PersonalityData. The locals here are renamed `sysMgr` accordingly.

// Below are the sibling callees of the routine.

// THE "0x062d2000-0x062ea000 DO-NOT-TOUCH RANGE" IS DEAD -- AUDITED AND
// RETIRED 2026-09-17 (claude:common-dnt-audit). Every version of this comment
// block before today qualified these helpers by whether they sat above or
// below 0x062ea000. That qualifier meant nothing:
//   * The stated reason for the range was "a dense, still-unidentified AI-
//     dispatch/steering vtable interface family". Nothing in it is unidentified
//     any more. A implementation tracker census of the range on 2026-09-17 returns 400
//     function items of which 252 are ready_unverified, 6 ready_verified and 43
//     skipped -- 293 carry a ported_file. The "unidentified vtable family" is
//     Behavior/DockBehavior/EvadeBehavior/GuideBehavior, MoveIdea/FaceIdea/
//     DodgeIdea/StopIdea/BaseIdea, FormationMoveFx, IBehaviorManager and
//     GenericWeightedPick, all named, all ported, all inside the range.
//   * Two former exclusions from this very comment, the routine (0x062e7580)
//     and the routine (0x062e6a80), were already found stale on 2026-09-17:
//     they are BuildAiTargetList (item 1800) and GetCachedRange (item 1791).
//   * The last two names the range was still being invoked to protect,
// and the routine, are ported for real immediately below.
// Nothing here is gated on an address range any more; what is left is ordinary
// translation work, tracked per item.

// Common (implementation tracker item 1897, address 0x062ee240).
// REAL PORT (2026-09-10): pure lookup-table classify, no callee dependencies.
// Treats its argument as a 3-int severity vector (matches the routine's 3
// decoded outputs -- its only other caller besides the routine itself, which
// also calls it directly on IUStateGraph-derived fields). Table is Common.dll's
// own .rdata the field (byte-exact copy, Common): 3
// tiers of 3 ints each -- {0,0,0}, {1,0,0}, {2,1,1}; no semantic names
// recovered for the tiers themselves. Returns the index of the first tier that
// dominates (>= component-wise) the input vector, or 3 if none do.
// formerly the routine (extern "C" __fastcall); file-static, its only caller is
// TargetManager::UpdateAttackTargetOverride (module storage / 062ede97).
static int ClassifyZoneLevelTier(int const* severity)
{
    static int const kTiers[3][3] = { { 0, 0, 0 }, { 1, 0, 0 }, { 2, 1, 1 } };

    for (int tier = 0; tier < 3; ++tier)
    {
        bool dominates = true;
        for (int i = 0; i < 3; ++i)
        {
            if (kTiers[tier][i] < severity[i])
            {
                dominates = false;
            }
        }
        if (dominates)
        {
            return tier;
        }
    }
    return 3;
}

// Common / Common (implementation tracker
// item 1894, address 0x062ed670). REAL PORT (2026-09-10), analysis-verified
// instruction-by-instruction (Common) after an initial
// pass mis-collapsed two separate calls into one -- see the corrected
// breakdown below. `this_00` is baseline's ECX: the caller's own
// SystemManager+0x142C object. `probeSrc` is the first stack argument (the
// object whose position is probed), and `out3` is a pure OUT buffer of 3 ints.
// Sequence: (1) call `probeSrc->vtbl[0]()` to get a 3-float position; (2) walk
// `R = *(*(this_00+4)+0x10)` and call `R->vtbl[8/4]()` (0 args) to fetch a
// "classifier" object; (3) build a 4-float probe buffer [x,y,z,20.0]
// (0x41a00000) from step 1's position and call `classifier->vtbl[0x20/4]()`
// with that buffer (1 arg) to get a bitmask. If bit 0x40000 is set, fails
// (returns 0, leaves *out3 untouched). Otherwise zeroes out3 and decodes
// low/mid/high severity from bits 0x1/0x2/0x4 (out[0] = 1/2/3), 0x8/0x10/0x20
// (out[1] = 1/2/3), and bit 0x8000 (out[2] = 1) -- analysis
// (`TEST AH,AH; JNS`, Common) confirms the analysis's
// `(char)(uVar3>>8)<'\0'` really tests bit 0x8000, not 0x100 as a literal
// reading of the C might suggest; returns 1 on success.
// SIGNATURE CORRECTED 2026-09-17 (claude:common-attack-goto). The port had
// `the routine(unsigned char* this_00, int* out3)` and read the probe source
// as `*out3`, with `this_00+4` supplying the `R` hop. Both halves were wrong,
// and together they made the one live call site a guaranteed null dereference:
// baseline's own AmbientScriptInfo::clear zeroes that buffer immediately before
// the call, so `*out3` is always 0 there. The raw prologue
// (Common) settles it --
//     SUB ESP,0x10 / PUSH ESI / PUSH EDI
//     MOV EDI,ECX                 <- the REAL `this`, i.e. the caller's own
//                                    SystemManager+0x142C object
//     MOV ECX,[ESP+0x1c]          <- E+4, the FIRST STACK ARG
//     MOV EAX,[ECX] / CALL [EAX]  <- get_position() ON THAT ARG
//     MOV ECX,[EDI+4] / MOV ECX,[ECX+0x10]   <- the `R` hop is off ECX/`this`
// and `MOV ECX,[ESP+0x20]` at module storage (E+8) is where out3 really comes
// from. Both of its call sites push an interface pointer first and the 3-int
// buffer second (module storage-062ed9db and module storage-062ede8b), which
// is exactly this shape. Declared __thiscall-shaped with `This` explicit,
// since the port models baseline vtable receivers as data everywhere else.
// TargetManager::QueryZonePropertyLevels, formerly the free the routine(this_00, ...):
// baseline's ECX is the TargetManager itself (MOV EDI,ECX; [EDI+4] = Owner), so it is a member.
int TargetManager::QueryZonePropertyLevels(IObjInspect const* probeSrc, int* out3)
{
    // RETYPED 2026-10-09 (claude-code:cmn-bug16), same three calls, arities unchanged and
    // re-checked against module storage-062ed6b1: probeSrc's IObjInspect::get_position()
    // (slot 0, no args); Owner->ObjectDb (SystemManager+0x10) IObjDB::GetZoneDb() (+0x08,
    // no args); Universe::ISystem::Slot08 (+0x20, one pointer) on a stack
    // {position, 20.0f} sphere -- the zone-properties query common_pathfind.cpp
    // (062dd7be-062dd812) already spells the same way.
    Vector const& pos = probeSrc->get_position();

    Universe::ISystem const* const System = Owner->ObjectDb->GetZoneDb();

    Geometry::Sphere probe;
    probe.Center = pos;
    probe.Radius = 20.0f;

    unsigned const mask = System->GetSphereProperties(&probe);

    if ((mask & 0x40000u) != 0)
    {
        return 0;
    }

    out3[0] = 0;
    out3[1] = 0;
    out3[2] = 0;
    if (mask & 1u) out3[0] = 1;
    if (mask & 2u) out3[0] = 2;
    if (mask & 4u) out3[0] = 3;
    if (mask & 8u) out3[1] = 1;
    if (mask & 0x10u) out3[1] = 2;
    if (mask & 0x20u) out3[1] = 3;
    if (mask & 0x8000u) out3[2] = 1;
    return 1;
}

// TargetManager::SetAttackState (implementation tracker item 1898; baseline name not
// exported, named after what the listing does). Common /
// Common, thiscall `RET 0x4`. Five callers, every one
// `PUSH state; MOV ECX,<TargetManager>; CALL`: 062ea038 (ScanGunGroupTargets, 0 or 2),
// (2), 062edbcd (1), 062ee03f (0), 062ee206 (2) -- the last four in
// UpdateAttackTargetOverride. No-op when AttackState (+0x08) already equals NewState;
// otherwise stores it and, when the owning IBehaviorManager has a content callback
// (+0x110), reports the change through ContentCallback::combat_state_notification:
// state 0 -> (0, owner id, owner's target id or 0), state 1 -> (2, owner id, 0),
// state 2 -> (4, owner id, 0); any other state only stores. State 0 also refreshes
// AnchorPosition (+0x3C) from ExternalIface's frame, callback or not.

// RE-PORTED 2026-10-09 (claude-code:cmn-bug16), term by term against
// Common (module storage-062ee38c). The 2026-09-10 body (then a
// free function the routine over raw vtable casts) was wrong on three counts and
// crashed under RTC ("ESP was not properly saved"):
//   * It called the owner's IObjInspect slot +0x20 with an argument. That slot is
//     get_id() (`?get_id@IObjInspectImpl@@UBE?BIXZ`, module storage, bare `RET`). The
//     `PUSH 0x0` (062ee2c9 / 062ee2f5) and `PUSH EBX` (062ee351) before `CALL [EDX+0x20]`
//     are not get_id's: MSVC pushed the LAST argument of the following +0x1c call first.
//   * So ContentCallback slot +0x1c is combat_state_notification(CSNType, uint, uint)
//     (`?combat_state_notification@ContentCallback@AI@pub@@UAEXW4CSNType@123@II@Z`, base
//     body module storage `RET 0xc`) and takes THREE arguments: (code, owner id, 0) for
//     states 1/2 and (0, owner id, target id) for state 0. The old port passed two and
//     dropped the target id.
//   * The state-0 tail reads +0x24 off the slot-4 result (062ee374 `ADD EAX,0x24`), i.e.
//     the Translation of the Transform the folded get_orientation/get_transform body
//     returns; the old port read offset 0 (the rotation matrix).
// The owner receiver is resolved like TargetManager::GetTarget: Owner->Watcher's watched
// object minus 8, with an `XOR ECX,ECX` arm when nothing is watched (062ee2c3 / 062ee2ef /
// / 062ee34b) that baseline then dereferences -- no guard, so none here either.
void TargetManager::SetAttackState(int NewState)
{
    // unchanged state -> return.
    if (AttackState == NewState)
    {
        return;
    }

    // EAX = Owner (SystemManager), store the state, EDI = the owning
    // IBehaviorManager's content callback (+0x110).
    SystemManager* const Sys = Owner;
    AttackState = NewState;
    pub::AI::ContentCallback* const Listener = Sys->Owner->ContentCbPtr;

    switch (NewState)
    {
    case 0:
    {
        // no callback -> straight to the anchor refresh.
        if (Listener != nullptr)
        {
            // the owner's current target (IObjInspect::get_target, +0x74,
            // into a zeroed out-param), then its get_id() (+0x20) when non-null, else 0.
            unsigned int TargetId = 0;
            IObjRW* Target = nullptr;
            ObjRwFromWatchable(const_cast<Watchable*>(Sys->Watcher.target()))->get_target(Target);
            if (Target != nullptr)
            {
                TargetId = Target->get_id();
            }

            // the owner is re-resolved from [[this+4]+8], its get_id()
            // is argument 2, the target id argument 3.
            unsigned int const OwnerId = ObjRwFromWatchable(const_cast<Watchable*>(Sys->Watcher.target()))->get_id();
            Listener->combat_state_notification(pub::AI::ContentCallback::CSNType0, OwnerId, TargetId);
        }

        // [Owner+0x14fc+0x84] == ExternalIface; slot 4 (+0x10,
        // get_orientation) is the /OPT:ICF fold of slot 3 (get_transform), and baseline reads
        // the Vector at +0x24 of its result -- Transform::Translation. Spelled through slot 3
        // as common_obstacleavoidance.cpp (062e7a7d) already does for the identical idiom.
        // ExternalIface slot +0x10 (get_orientation -- the Matrix leading
        // the Transform; baseline adds 0x24 for the translation), copied into AnchorPosition.
        Transform const& Xform = (Transform const&)Sys->SelfTarget.Iface->get_orientation();
        AnchorPosition = Xform.Translation;
        break;
    }
    case 1:
        // combat_state_notification(2, owner id, 0), then return.
        if (Listener != nullptr)
        {
            unsigned int const OwnerId = ObjRwFromWatchable(const_cast<Watchable*>(Sys->Watcher.target()))->get_id();
            Listener->combat_state_notification(pub::AI::ContentCallback::CSNType2, OwnerId, 0);
        }
        break;
    case 2:
        // combat_state_notification(4, owner id, 0), then return.
        if (Listener != nullptr)
        {
            unsigned int const OwnerId = ObjRwFromWatchable(const_cast<Watchable*>(Sys->Watcher.target()))->get_id();
            Listener->combat_state_notification(pub::AI::ContentCallback::CSNType4, OwnerId, 0);
        }
        break;
    default:
        // any other state is stored and nothing else happens.
        break;
    }
}

// (item 1801) is ResetAiTargetList (common_iustategraph.cpp) and the routine
// (item 1899) is TargetManager::GetHostilePowerRatio (below, with the other TargetManager
// members). Both used to be raw-offset extern "C" stubs here; the second returned a
// constant 0.0f for the hostile half of the ratio.

// TargetManager's two enemy-count clamp bounds: the field (min) / the field (max),
// .data dwords initialised to 2. Written only by TargetManager::set_player_enemy_clamp
// (0x062ee5e8 / 0x062ee5ed); read only by the routine below (0x062ee89d min, 0x062ee864 max).
namespace
{
    int GTargetManagerEnemyClampMin = 2;
    int GTargetManagerEnemyClampMax = 2;
}

void TargetManager::set_player_enemy_clamp(int MinClamp, int MaxClamp)
{
    // EAX = param_1 -> [the field]; ECX = param_2 -> [the field]; RET 8.
    // `this` is not touched.
    GTargetManagerEnemyClampMin = MinClamp;
    GTargetManagerEnemyClampMax = MaxClamp;
}

int TargetManager::submit_forced_target(unsigned int TargetId)
{
    // a full list (count >= 8) -> 0. A target already in the list -> 0.
    // Otherwise append at ForcedTargets[count], count++, return 1.
    int const Count = NumForcedTargets;
    if (Count >= MaxForcedTargets)
        return 0;
    for (int Index = 0; Index < Count; ++Index)
    {
        if (ForcedTargets[Index] == TargetId)
            return 0;
    }
    ForcedTargets[Count] = TargetId;
    ++NumForcedTargets;
    return 1;
}

int TargetManager::remove_forced_target(unsigned int TargetId)
{
    // find TargetId among the first count entries; absent -> 0. Found at Index:
    // shift [Index+1 .. count-1] down by one (the loop re-reads count each pass, 0x062eec85),
    // then count--, return 1.
    int const Count = NumForcedTargets;
    int Index = 0;
    for (; Index < Count; ++Index)
    {
        if (ForcedTargets[Index] == TargetId)
            break;
    }
    if (Index >= Count)
        return 0;
    for (int Move = Index; Move < NumForcedTargets - 1; ++Move)
        ForcedTargets[Move] = ForcedTargets[Move + 1];
    --NumForcedTargets;
    return 1;
}

TargetManager::TargetManager()
{
    // (Common), all 13 instructions. The base
    // SubtargetEnumerator has no data, and the vfptr store (module storage) is the
    // compiler's. Nothing else is written: Owner, AttackState, AggressionRatio,
    // SubtargetDistanceSq, NumForcedTargets, ForcedTargets, AnchorPosition, bCommitted,
    // CommitTimer and SelectionPriority are left as the allocation found them.
    NumCandidatesA = 0;          // +0x54
    NumCandidatesB = 0;          // +0x58
    NumSelected = 0;             // +0x5c
    bSelectedIsPlayer = false;   // +0x60 (byte)
    TargetedEnemyCount = 0;      // +0x64
    bPlayerOverClampKept = false;    // +0x68 (byte)
    bPlayerOverClampDropped = false; // +0x69 (byte)
    SubtargetId = SUBOBJ_ID_NONE; // +0x14, `MOV CX,[SUBOBJ_ID_NONE]` (module storage)
}

// The six-instruction cache refresh baseline inlines twice in enumerate_equip
// (module storage-062eea1a and module storage-062eeb11), the same block FaceBehavior
// and the routine inline: latch SystemManager+0x157A, then refresh
// CachedPhysicalRadius / CachedPhysicalCenter through ExternalIface's
// get_physical_radius (IObjInspect +0x2C; the last push, &Radius, is the first argument).
static void TargetManager_EnsurePhysicalCacheFresh(SystemManager* Sys)
{
    if (Sys->SelfTarget.bPosValid != 0)
        return;
    Sys->SelfTarget.bPosValid = 1;
    Sys->SelfTarget.Iface->get_physical_radius(Sys->SelfTarget.CachedRadius,
                                                         Sys->SelfTarget.CachedPos);
}

bool TargetManager::enumerate_equip(unsigned short Id, unsigned Type)
{
    // (Common), vftable module storage slot 0,
    // RET 8. Every instruction traced:
    // FLD FLT_MAX (the field = 0x7f7fffff) -- the value for any Type other
    //             than 0x80 (EquipTypeGun) (CMP EAX,0x80 / JNZ the branch).
    //   Type == 0x80:
    // cache refresh (first copy)
    // Obj = Owner->Watcher target - 8 (0 when unset; no null test after:
    //               baseline dereferences it), Obj->get_target(Target) (+0x74), Target
    //               pre-zeroed in the arg-2 stack slot (062eea25)
    // SubCenter = 0; Target->get_sub_obj_center_of_mass(Id, SubCenter) (+0x18);
    //               the int result is dropped, and Target is not null-tested either
    // TargetPos = the routine(Target) (GetIfacePositionOrZero)
    // ToTarget = TargetPos - CachedPhysicalCenter; SubOffset = SubCenter -
    //               TargetPos; the routine(&SubOffset, &ToTarget) (ClampedDot3)
    // FCOMP 0.0f (the field), TEST AH,0x5 / JP: the jump (to FLT_MAX) is
    //               taken on even C0|C2 parity, i.e. Dot >= 0 or unordered; only Dot < 0 --
    //               the subtarget faces the owner -- goes on:
    // cache refresh (second copy), then Value = |TargetPos - CachedCenter|^2
    //               summed as (dz*dz + dy*dy) + dx*dx (062eeb29-062eeb37)
    // FCOM [this+0x10] / TEST AH,0x5 / JP: store only when Value is strictly
    //             below SubtargetDistanceSq (C0 set, C2 clear); then SubtargetDistanceSq =
    //             Value, SubtargetId = Id (MOV [EBP+0x14],BX).
    //   AL is cleared at 062eeb4c on both exits: the enumerator never stops the walk.
    // The value stays on the x87 stack to the FCOM at 062eeb47 and is rounded only by the
    // FSTP into SubtargetDistanceSq (CMN-S2 AIS: was float arithmetic).
    double Value = 3.4028235e+38f;
    if (Type == 0x80)
    {
        TargetManager_EnsurePhysicalCacheFresh(Owner);

        IObjRW* Target = nullptr;
        IObjRW* const Obj = ObjRwFromWatchable(const_cast<Watchable*>(Owner->Watcher.target()));
        Obj->get_target(Target);

        Vector SubCenter;
        SubCenter.x = 0.0f;
        SubCenter.y = 0.0f;
        SubCenter.z = 0.0f;
        Target->get_sub_obj_center_of_mass(Id, SubCenter);

        Vector TargetPos;
        GetIfacePositionOrZero(&TargetPos, Target);

        Vector ToTarget;
        ToTarget.x = TargetPos.x - Owner->SelfTarget.CachedPos.x;
        ToTarget.y = TargetPos.y - Owner->SelfTarget.CachedPos.y;
        ToTarget.z = TargetPos.z - Owner->SelfTarget.CachedPos.z;
        Vector SubOffset;
        SubOffset.x = SubCenter.x - TargetPos.x;
        SubOffset.y = SubCenter.y - TargetPos.y;
        SubOffset.z = SubCenter.z - TargetPos.z;

        if (ClampedDot3(SubOffset, ToTarget) < 0.0f)
        {
            TargetManager_EnsurePhysicalCacheFresh(Owner);
            // differences and the sum (dz*dz + dy*dy) + dx*dx unrounded.
            double const Dx = (double)(TargetPos.x) - Owner->SelfTarget.CachedPos.x;
            double const Dy = (double)(TargetPos.y) - Owner->SelfTarget.CachedPos.y;
            double const Dz = (double)(TargetPos.z) - Owner->SelfTarget.CachedPos.z;
            Value = (Dz * Dz + Dy * Dy) + Dx * Dx;
        }
    }

    // FCOM; TEST AH,0x5; JP -- only strictly less (ordered) is kept.
    if (Value < (double)(SubtargetDistanceSq))
    {
        SubtargetDistanceSq = (float)(Value);
        SubtargetId = Id;
    }
    return false;
}

bool TargetManager::enumerate_subpart(unsigned short Id, unsigned CollisionGroup)
{
    // `XOR AL,AL; RET 8` (ICF-folded), vftable module storage slot 1.
    (void)Id;
    (void)CollisionGroup;
    return false;
}

bool TargetManager::Attach(SystemManager* NewOwner)
{
    // (Common), RET 4, all 24 instructions. The three
    // refusal tests are NewOwner == 0, NewOwner->Watcher target == 0, and target - 8 == 0
    // (the last cannot fail for a non-null target but is baseline's own test).
    if (NewOwner == nullptr)
        return false;
    Watchable const* const Watched = NewOwner->Watcher.target();
    if (Watched == nullptr)
        return false;
    if (ObjRwFromWatchable(const_cast<Watchable*>(Watched)) == nullptr)
        return false;
    Owner = NewOwner;            // +0x04
    AttackState = 2;             // +0x08
    NumForcedTargets = 0;        // +0x18
    AggressionRatio = 0.5f;      // +0x0c (0x3f000000)
    AnchorPosition.z = 0.0f;     // +0x44
    AnchorPosition.y = 0.0f;     // +0x40
    AnchorPosition.x = 0.0f;     // +0x3c
    CommitTimer = 0.0f;          // +0x4c
    bCommitted = false;          // +0x48
    return true;
}

void TargetManager::UpdateCommitTimer(float Dt)
{
    // (Common), RET 4. FADD then FST keeps the sum in
    // CommitTimer before the compare; FCOMP 10.0f (the field) / TEST AH,0x41 / JNZ skips
    // the reset when the sum is below, equal to, or unordered with 10 -- so it lapses only on
    // a strictly greater value.
    if (!bCommitted)
        return;
    // FST stores the rounded sum, FCOMP 10.0f (the field) reads the
    // unrounded one; TEST AH,0x41 / JNZ keeps counting when <= or unordered.
    double const Sum = (double)(Dt) + CommitTimer;
    CommitTimer = (float)(Sum);
    if (Sum > 10.0)
    {
        CommitTimer = 0.0f;      // +0x4c
        AnchorPosition.z = 0.0f; // +0x44
        AnchorPosition.y = 0.0f; // +0x40
        AnchorPosition.x = 0.0f; // +0x3c
        bCommitted = false;      // +0x48
    }
}

IObjRW* TargetManager::GetTarget() const
{
    // (item 1891), Common, all 23 instructions:
    // EAX = [[this+4]+8]            Owner->Watcher's Watchable
    // [ESP] = 0                     the out-param, pre-zeroed
    // TEST EAX,EAX / JZ 062ed5c8    ECX = EAX - 8, or 0 when nothing is watched
    // CALL [[ECX]+0x74]             IObjInspect::get_target(IObjRW*&)
    // EAX = [ESP]                   the out-param is the result
    // The null arm is not a guard: it loads the vtable from address 0. Nothing is tested here
    // either.
    IObjRW* Target = nullptr;
    IObjRW* const Obj = ObjRwFromWatchable(const_cast<Watchable*>(Owner->Watcher.target()));
    Obj->get_target(Target);
    return Target;
}

unsigned short TargetManager::GetSubTarget() const
{
    // (item 1892), Common, all 24 instructions: the local
    // is seeded from SUBOBJ_ID_NONE (062ed5e1), the receiver is resolved exactly as in
    // GetTarget (null arm reads address 0), then IObjInspect::get_subtarget(unsigned short&)
    // (+0x78) and `MOV AX,[ESP+2]`.
    unsigned short SubTarget = SUBOBJ_ID_NONE;
    IObjRW* const Obj = ObjRwFromWatchable(const_cast<Watchable*>(Owner->Watcher.target()));
    Obj->get_subtarget(SubTarget);
    return SubTarget;
}

double TargetManager::GetHostilePowerRatio()
{
    // (item 1899), Common, 120 instructions. Frame after
    // the three pushes: [ESP+0x0c] Hostile, [ESP+0x10] Own, [ESP+0x14]/[0x18]/[0x1c] and
    // [0x24]/[0x28]/[0x2c] the three GetCachedRange out-pairs.

    // Hostile = 0.
    float Hostile = 0.0f;

    // the owner (Owner->Watcher's Watchable - 8, 0 when unset and then
    // dereferenced anyway) -> get_toughness() (+0x144), FILD, FSTP float into Own.
    IObjRW* const Self = ObjRwFromWatchable(const_cast<Watchable*>(Owner->Watcher.target()));
    float Own = (float)(Self->get_toughness());

    // the SystemManager+0x157A latch and the physical-radius refresh,
    // raised before the call, radius (+0x1514) pushed last so it is the first argument.
    TargetManager_EnsurePhysicalCacheFresh(Owner);

    // the routine(&Owner->Timers) -> the vector header; ESI walks Begin..End with a
    // 0x88 stride, End re-read on every pass (062ee4dd).
    FAiTargetVector* const List = BuildAiTargetList(GetAiTargetList(Owner));
    for (FAiTarget* Elem = List->Begin; Elem != List->End; ++Elem)
    {
        // the lazily cached type, raised AFTER the get_type call.
        if (Elem->bFlagsValid == 0)
        {
            Elem->Iface->get_type(Elem->CachedFlags);
            Elem->bFlagsValid = 1;
        }

        // TEST [EDI],0x7f8110 / JZ next.
        if ((Elem->CachedFlags & 0x7f8110) == 0)
            continue;

        // Three the routine calls, each into its own stack pair; the routine writes
        // (CachedAttitude, CachedReverseAttitude) through (param_1, param_2).
        // (&L24, &L14), compares L14 -- the reverse attitude
        // (&L18, &L28), compares L18 -- the attitude
        // (&L2C, &L1C), compares L1C -- the reverse attitude
        // The first two use `TEST AH,0x41 / JNP`, taken when exactly one of C0/C3 is set,
        // i.e. value <= -0.6f (the field) and ordered; either one goes to the hostile sum.
        // The third uses `TEST AH,0x1 / JNZ next`, so the own sum needs value >= 0.6f
        // (the field) and ordered.
        float Attitude = 0.0f;
        float Reverse = 0.0f;
        GetCachedRange(Elem, &Attitude, &Reverse);
        bool bHostile = (Reverse <= -0.6f);
        if (!bHostile)
        {
            GetCachedRange(Elem, &Attitude, &Reverse);
            bHostile = (Attitude <= -0.6f);
        }

        if (bHostile)
        {
            // FILD toughness; FADD [ESP+0xc]; FSTP float [ESP+0xc].
            Hostile = (float)((double)(Elem->Iface->get_toughness())
                                         + (double)(Hostile));
            continue;
        }

        GetCachedRange(Elem, &Attitude, &Reverse);
        if (Reverse >= 0.6f)
        {
            // FILD toughness; FADD [ESP+0x10]; FSTP float [ESP+0x10].
            Own = (float)((double)(Elem->Iface->get_toughness())
                                     + (double)(Own));
        }
    }

    // Total = Own + Hostile, stored as a float over Own's slot.
    float const Total = Own + Hostile;

    // 0.0f (the field) is left in ST0; Total is compared against the
    // DOUBLE 0.01 (the field, 0x3f847ae147ae147b), TEST AH,0x41 / JNZ keeps the 0.0f when
    // Total <= 0.01 or unordered; otherwise Hostile / Total replaces it.
    // FLD float Hostile; FDIV float Total; RET -- the quotient is returned
    // unrounded (double), never stored as a float here.
    if ((double)(Total) > 0.01)
        return (double)(Hostile) / Total;
    return 0.0;
}

pub::AI::ScanResponse TargetManager::GetScanResponse(uint Type)
{
    // (item 1904), Common, `RET 4`.
    // [[this+4]+0x10] is SystemManager::ObjectDb; `LEA EAX,[ESP+8]; PUSH EAX;
    //             CALL [EDX+4]` is IObjDB::FindById(&Type). EAX is not kept.
    Owner->ObjectDb->FindById(&Type);

    // the routine(&Owner->Timers): drop the cached AI target list.
    ResetAiTargetList(GetAiTargetList(Owner));

    // the routine(this), float in ST0.
    double const Ratio = GetHostilePowerRatio(); // unrounded ST0, compared below


    // [Owner+0x1900] == Personality+0x37C == JobParams.LootFleeThreshold, a TOUGHNESS_TYPE
    //   (its default 2 is the 0.65f entry). `CMP EAX,4; JA` is unsigned, so anything outside
    //   0..4 -- negatives included -- takes the 0.5f default. Constants read from the image:
    // 0.15f, 0639e0dc 0.35f, 0639e0d8 0.65f, 0639e0d4 0.85f, 0639a1d0 1.0f,
    // 0.5f. baseline inlines this switch rather than calling
    //   Personality::get_toughness_ratio, whose table is the same.
    float Threshold;
    switch (Owner->PersonalityData.JobParams.LootFleeThreshold)
    {
    case 0:
        Threshold = 0.15f;
        break;
    case 1:
        Threshold = 0.35f;
        break;
    case 2:
        Threshold = 0.65f;
        break;
    case 3:
        Threshold = 0.85f;
        break;
    case 4:
        Threshold = 1.0f;
        break;
    default:
        Threshold = 0.5f;
        break;
    }

    // FXCH; FCOMP threshold; TEST AH,0x1 / JNZ -> 1: C0 is set when Ratio is below
    //   the threshold or unordered with it, and only then is the answer 1.
    if (!(Ratio >= (double)(Threshold)))
        return (pub::AI::ScanResponse)(1);
    return (pub::AI::ScanResponse)(0);
}

// The job attack-order class-mask table -- Common.dll .rdata the field..the field
// (Common), byte-exact (re-read from EXE\Common.dll 2026-09-24: the
// first thirteen dwords are these values). Indexed by an attack-order record's class id;
// class 12 is the terminator. The last raw dword (the field) looks like a pointer, not a
// mask -- copied verbatim since no observed record indexes that far. Read by
// TargetManager::MatchesAttackOrder (the routine, module storage) and the routine
// (module storage); it used to be a function-local static of the latter.
static unsigned const GAttackOrderClassMasks[14] = {
    0x00010000u, 0x00020000u, 0x00100000u, 0x00040000u, 0x00080000u, 0x00200000u,
    0x00000080u, 0x00000040u, 0x00001000u, 0x00002000u, 0x0000cd3fu, 0xffffffffu,
    0x00000000u, 0x063a9df4u,
};

bool TargetManager::MatchesAttackOrder(FAiTarget* Candidate)
{
    // (item 1900), Common, `RET 4`.
    // EAX = Owner + 0x1834 (Personality JobParams.AttackPreferences): count
    //                      the 12-byte records up to the first whose class dword is 12.
    // signed `Index < Count` loop (JLE out when Count <= 0).
    // class re-read through [this+4] every pass, mask = table[class]
    //                      with no bounds test.
    // Candidate's lazily cached get_type (+0x88), guard raised AFTER.
    // `TEST mask,flags; JNZ` -> true; falling out of the loop -> false.
    // No bound on the count either: baseline walks past the 13 records until it meets class 12.
    pub::AI::Personality::JobStruct::FAttackPreference const* const Records =
        Owner->PersonalityData.JobParams.AttackPreferences;
    int Count = 0;
    while (Records[Count].ShipClass != 0xc)
        ++Count;

    for (int Index = 0; Index < Count; ++Index)
    {
        unsigned const Mask = GAttackOrderClassMasks[
            Owner->PersonalityData.JobParams.AttackPreferences[Index].ShipClass];

        if (Candidate->bFlagsValid == 0)
        {
            Candidate->Iface->get_type(Candidate->CachedFlags);
            Candidate->bFlagsValid = 1;
        }

        if ((Mask & Candidate->CachedFlags) != 0)
            return true;
    }
    return false;
}

// Common / Common (implementation tracker
// item 1901, address 0x062ee600). REAL PORT (2026-09-10): candidate/target
// arbitration -- walks a priority-tier chain at IUStateGraph+0x1834 (stride
// 0xc: class-id int at +0, threshold float at +4 -- analysis-confirmed via
// `FCOMP float ptr [EDI+0x4]` at Common, a direct memory float
// compare, not an int needing numeric conversion despite the analysis's
// `(float)piVar13[1]` cast syntax -- and output-priority int at +8),
// classifies each candidate by a type-mask table (the field, below), keeps
// the first non-empty tier's matches (fast-invsqrt distance vs. the tier's
// threshold), then prefers candidates whose CShip::get_targeted_enemy_count()
// falls outside [kEnemyClampMin, kEnemyClampMax] before falling back to a
// pseudo-random pick (with a `stickyTargetId` tie-break). Does not call
// /the routine/the routine/the routine -- no DO-NOT-TOUCH
// dependency, fully portable. The inspected C declared a bogus 65517-int
// unused array (`aiStackY_40040`) immediately before the real 16-int
// `aiStack_40` -- confirmed via the raw analysis
// (Common `SUB ESP,0x70`, a 112-byte frame nowhere near large
// enough for that phantom array) to be a analysis stack-frame-reconstruction
// artifact, exactly the risk item 1901's prior notes flagged; omitted here.
// FIXED 2026-10-14 (CMN-S2 AIS): the invented `< 16` store guard, the class-index
// clamp (module storage) and the owner null arm (module storage `XOR ECX,ECX` then
// dereferences) are gone; the distance runs in x87 precision with baseline's sum
// order; rand() is called before the clamp-pick return (module storage).

// TargetManager::SelectAttackCandidate, formerly the free the routine(this_, ...):
// its one caller passes ECX = the TargetManager (module storage, `MOV ECX,ESI`).
FAiTarget* TargetManager::SelectAttackCandidate(
    FAiTarget** Candidates, int CandidateCount, IObjRW const* StickyTarget, int* OutPriority)
{
    // TargetManager::set_player_enemy_clamp's [min,max] bounds (the field / the field,
    // both 2 until set_player_enemy_clamp runs) -- read live, as baseline does.
    int const kEnemyClampMin = GTargetManagerEnemyClampMin;
    int const kEnemyClampMax = GTargetManagerEnemyClampMax;

    *OutPriority = 0xf;

    // Sys+0x1834: the personality's JobParams.AttackPreferences, ended by ShipClass 12.
    pub::AI::Personality::JobStruct::FAttackPreference const* const Prefs =
        Owner->PersonalityData.JobParams.AttackPreferences;

    int tierCount = 0;
    while (Prefs[tierCount].ShipClass != 0xc)
    {
        ++tierCount;
    }

    FAiTarget* candidatesOut[16] = {};
    int matchCount = 0;

    for (int tier = 0; tier < tierCount; ++tier)
    {
        pub::AI::Personality::JobStruct::FAttackPreference const& Pref = Prefs[tier];
        // `MOV EAX,[EDI]; MOV EAX,[EAX*4 + the field]` -- no bounds test.
        // Very strange, but baseline: a class id past the table reads whatever follows it.
        unsigned const classMask = GAttackOrderClassMasks[Pref.ShipClass];

        matchCount = 0;
        for (int i = 0; i < CandidateCount; ++i)
        {
            FAiTarget* const cand = Candidates[i];

            if (cand->bFlagsValid == 0)
            {
                cand->Iface->get_type(cand->CachedFlags);
                cand->bFlagsValid = 1;
            }

            unsigned typeFlags = classMask & cand->CachedFlags;

            if (cand->Iface->is_player() != 0)
            {
                typeFlags = classMask & 0x30000u;
            }

            if (typeFlags == 0)
            {
                continue;
            }

            // the owner (Owner->Watcher's object - 8; the `XOR ECX,ECX` arm at
            // dereferences 0, no guard) answers get_position (slot 0); its three floats
            // are copied into the frame.
            Vector const OwnerPos =
                ObjRwFromWatchable(const_cast<Watchable*>(Owner->Watcher.target()))->get_position();

            // FAiTarget::RefreshPos inlined (guard raised before the call).
            if (cand->bPosValid == 0)
            {
                cand->bPosValid = 1;
                cand->Iface->get_physical_radius(cand->CachedRadius, cand->CachedPos);
            }

            // the three differences stay on the x87 stack (no float store),
            // the squared length is (dz*dz + dy*dy) + dx*dx, and the inlined 0x5f3759df length
            // runs on that unrounded value (CmnAiSteer::InlineFastLength).
            double const dx = (double)cand->CachedPos.x - OwnerPos.x;
            double const dy = (double)cand->CachedPos.y - OwnerPos.y;
            double const dz = (double)cand->CachedPos.z - OwnerPos.z;
            double const dist = CmnAiSteer::InlineFastLength((dz * dz + dy * dy) + dx * dx);

            // FCOMP [EDI+4]; TEST AH,0x41; JP skips greater and unordered, so
            // the record matches when dist <= its float range.
            if (dist <= (double)Pref.Range)
            {
                // stored with no bound. The array cannot overflow: the only caller
                // (module storage) passes at most 16 candidates (its own `CMP EAX,0x10; JGE` caps),
                // each candidate matches at most once, and only one tier's matches survive.
                candidatesOut[matchCount] = cand;
                ++matchCount;
                // The record's +8 dword (the weapon-type mask) is what baseline reports.
                *OutPriority = (int)Pref.WeaponFlags;
            }
        }

        if (matchCount > 0)
        {
            break;
        }
    }

    int bestIndex = -1;
    if (matchCount > 0)
    {
        for (int i = 0; i < matchCount; ++i)
        {
            IObjInspect const* const candIface = candidatesOut[i]->Iface;
            if (!candIface->is_player())
            {
                continue;
            }

            // Slot 0x150 is cobject(), declared as returning CObject const*. This site needs
            // the CShip it actually is -- the 0x503 class mask below is CObject::ObjectClass.
            CShip* ship = (CShip*)candIface->cobject();
            if (ship == nullptr || ((unsigned)ship->ObjectClass & 0x503u) != 0x503u)
            {
                continue;
            }

            int const enemyCount = ship->get_targeted_enemy_count();
            if (enemyCount > kEnemyClampMax)
            {
                if (matchCount < 2)
                {
                    bPlayerOverClampKept = true;
                }
                else
                {
                    std::memmove(&candidatesOut[i], &candidatesOut[i + 1],
                                 (size_t)(matchCount - (i + 1)) * sizeof(FAiTarget*));
                    --matchCount;
                    bPlayerOverClampDropped = true;
                }
            }

            if (enemyCount < kEnemyClampMin)
            {
                bestIndex = i;
                break;
            }
        }

        if (matchCount > 0)
        {
            // rand() runs BEFORE the bestIndex test, so a clamp-selected pick
            // still advances the CRT generator.
            int const r = rand();
            int const scaledIndex = (int)((r * matchCount + ((r * matchCount) >> 0x1f & 0x7fff)) >> 0xf);
            if (bestIndex != -1)
            {
                return candidatesOut[bestIndex];
            }
            int pickIndex = scaledIndex;

            if (StickyTarget != nullptr)
            {
                bool found = false;
                int j = 0;
                for (; j < matchCount; ++j)
                {
                    // The candidate's +0x84 Iface compared with the sticky object (IObjInspect
                    // is IObjRW's base at offset 0, so the pointer values are the same).
                    if (candidatesOut[j]->Iface == StickyTarget)
                    {
                        found = true;
                        break;
                    }
                }
                pickIndex = found ? j : scaledIndex;
            }

            return candidatesOut[pickIndex];
        }
    }

    return nullptr;
}

// Common (implementation tracker item 1896): per-tick "wingman
// attack coordination" dispatcher (see the cluster header comment above for
// the resolved this_/IUStateGraph/IBehaviorManager relationship). PARTIAL PORT
// (2026-09-10, d215:item1896-directive-branch -- closed piece (a) below, the
// last portable gap; wired up now that 7 of its 10 sibling callees are real).

// HISTORY CORRECTION, 2026-09-17 (claude:common-dnt-audit). Until today this
// header read: "One piece remains, and is permanently out of reach, not merely
// 'not yet done': (b) the two neighbor-scan loops ... both inside the standing
// DO-NOT-TOUCH address range 0x062d2000-0x062ea000. This is an external,
// permanent blocker, not a gap in this port -- there is nothing left to do here
// until that range is lifted elsewhere."
// Every clause of that is now false, and none of it was ever a real blocker:
//   * and the routine became BuildAiTargetList (item 1800) and
//     GetCachedRange (item 1791), and the FIRST neighbour loop
//     (Common) was wired for real on 2026-09-17.
//   * and the routine are ported for real immediately above, as
//     AiVolumeProbeApproachDirection / AiVolumeProbeContainsPoint. Their own
//     stated blocker (an "unbuilt 0x88-stride AI-list-element" owner) was a
//     misreading of the receiver, which is `*(IBehaviorManager+0x0c)` -- see
//     that block's header for the byte-level derivation.
//   * The address range itself is retired; see the cluster header comment.
// What is genuinely left is ordinary translation work on ONE loop, called out
// at its own site below.
// The directive-override branch (piece (a), baseline Common-
// 123105) is now fully wired using the real the routine -- see the inline
// comments below for the resolved control-flow (including the the branch
// fallthrough that skips the directive gate entirely) and the vtbl-call
// "this" idiom (owner0+4, not owner0) that a previous pass had left
// unresolved.
// CLOSED 2026-09-17 (claude:common-attack-goto): piece (b) -- the second
// neighbour-scan loop (Common) -- and the whole
// arbitration tail behind it (the branch through the branch) are now
// translated in full at their own sites below. The candidate-match count can
// leave 0, the routine is called for real, and both of baseline's own exit
// paths (a selected candidate, and "nothing selected") run.

// The sibling gap once recorded here is closed: the routine (item 1899, the aggression
// ratio read at Common) is TargetManager::GetHostilePowerRatio, ported in
// full on 2026-09-24, so this function's `aggressionRatio != 0.0f` early-return at
// Common can fire again.

// Shared tail used by the routine from TWO separate call sites (baseline
// literally jumps into the same code from both -- Common-
// 254249's the branch/the branch/the branch block has XREFs from both
// /062ed80d (piece (a)'s own commit branch) and 062ed997/062ed9a0
// (the already-committed/close-range section's commit path, Common
// 123147-123154)): re-reads the current owner's vtbl+0x74-reported target,
// and if non-null, notifies it through owner0+4's vtbl+0x48 -- a SECOND,
// embedded vtable sub-object 4 bytes into owner0, not owner0 itself (analysis-
// confirmed: same double-dereference vtbl-call idiom used everywhere else in
// this cluster, `**(code**)(OBJ+N)+SLOT`, just with N=4 instead of N=0 -- NOT
// "one pointer hop shorter" as an earlier pass suspected before this branch
// was actually ported).

// RETYPED 2026-10-09 (claude-code:cmn-bug16), Common / 254234-254264
// (module storage-062ed838, 062ed97e-062ed9b5): local_114's low word is set to
// SUBOBJ_ID_NONE (module storage, 0x0000 -- the port's common_misc.cpp constant is the
// same symbol and value now), local_118 = 0, the owner (Owner->Watcher's object - 8)
// answers get_target (+0x74) into local_118; a null target returns (the branch).
// Otherwise the owner is re-resolved and its IObjAffect sub-object (+4) gets
// set_target(+0x48, `RET 0xc`) with pushes (EBP == 0, local_114, -1): set_target(nullptr,
// SUBOBJ_ID_NONE, -1). local_114's high word (the BehaviorMgr+0xc seed) is pushed as part
// of the dword but SubTarget is an `unsigned short`, so it never reaches the callee.
// Neither owner resolution has a guard in baseline (the `XOR ECX,ECX` / `XOR EAX,EAX` arms
// at 062ed80f / 062ed9a5 dereference 0); the former owner0 null checks are gone.
static void NotifyOverrideCommitTail(SystemManager* Sys)
{
    IObjRW* OwnerTarget = nullptr;
    ObjRwFromWatchable(const_cast<Watchable*>(Sys->Watcher.target()))->get_target(OwnerTarget);
    if (OwnerTarget == nullptr)
    {
        return;
    }
    IObjRW* const Owner = ObjRwFromWatchable(const_cast<Watchable*>(Sys->Watcher.target()));
    static_cast<IObjAffect*>(Owner)->set_target(nullptr, SUBOBJ_ID_NONE, -1);
}

// Real identity (2026-09-11, d280:common-port; CONTRADICTION SETTLED
// 2026-09-17, claude:common-iustategraph). This function's param_1 used to be
// described two incompatible ways on the same day: common_iface.h's "RE NOTE"
// called it IBehaviorManager* itself, while this cluster's own header comment
// traced the call-site bytes (Common) to
// `this = (*(int*)(CallerThis+8)) + 0x142C`. The call-site version is the
// correct one, and the missing piece is that `CallerThis+8` is a field
// common_iface.h ALREADY documents: IBehaviorManager+0x08 is `SystemManager*`.
// So param_1 is `SystemManager + 0x142C` (SystemManager::Targeting), the
// caller really is an IBehaviorManager, and `param_1+4` (`sysMgr` below) is a
// back-pointer to that SystemManager -- not to any "IUStateGraph". See the
// cluster header comment above for the full settlement, including why the old
// note's "+0x54..+0x69 land inside IBehaviorManager's Opaque0c" argument does
// not distinguish the two hypotheses. common_iface.h's RE NOTE is left
// unedited only because that file belongs to another lane this wave; it is
// wrong and should be removed.
// No mangled/exported baseline name was recovered for this specific
// routine (unlike IBehaviorManager's other, individually-exported methods
// declared in common_iface.h) -- it reads as an internal, no-name-mangled
// per-tick AI helper: it (a) resolves/commits a
// directive-driven target override via the embedded sysMgr+0x1364
// sub-object, (b) falls back to an already-committed/close-range override
// notify, (c) decodes an AmbientScriptInfo tier and an aggression-ratio gate
// (the routine) that can short-circuit into a priority notify
// (TargetManager::SetAttackState, 062ee280), and (d) would otherwise gather nearby attack-target
// candidates and arbitrate them via the routine -- except that candidate
// gathering's own two neighbor-scan loops call into the standing
// DO-NOT-TOUCH range (the routine/the routine) and
// are therefore structurally unreachable from this port, so baseline's own
// "0 candidates found" tail is what always executes here (see the two
// TODO(implementation tracker item 1896) comments below for the exact citations).
// Named UpdateAttackTargetOverride to replace the bare baseline address with
// something that reflects this real, analysis-confirmed role.
// (The "structurally unreachable" remark above is history: both loops and the
// arbitration tail are wired, see below.)
// TargetManager::UpdateAttackTargetOverride, formerly the free extern "C"
// UpdateAttackTargetOverride(param_1) over `SystemManager+0x142C` (CMN-SPLIT A2).
void TargetManager::UpdateAttackTargetOverride()
{
    // Owner (+4) is the owning SystemManager, whose own +4 is the IBehaviorManager.
    SystemManager* const Sys = Owner;
    IBehaviorManager* behaviorMgr = Sys->Owner;
    // / 062ed7b8: IBehaviorManager+0x0C (the PathfindManager) is loaded at
    // entry into the frame dword the two user-zone probes use as their receiver.
    PathfindManager* const probe = behaviorMgr->Pathfind;
    // The owner ship: [Sys+8] with the -8 Watchable adjustment (0 for an empty watcher).
    IObjRW* const owner0 = ObjRwFromWatchable(const_cast<Watchable*>(Sys->Watcher.target()));
    // Sys+0x1584, the personality; JobParams is at +0x2A0 (Sys+0x1824).
    pub::AI::Personality::JobStruct const& Job = Sys->PersonalityData.JobParams;

    NumCandidatesA = 0;
    NumCandidatesB = 0;
    NumSelected = 0;
    bSelectedIsPlayer = false;
    TargetedEnemyCount = 0;
    bPlayerOverClampKept = false;
    bPlayerOverClampDropped = false;

    // --- Common (piece (a)): directive-override branch ---
    // REAL PORT (2026-09-10, d215:item1896-directive-branch).
    // Common / Common: local_114 is
    // unconditionally seeded from IBehaviorManager+0xc (opaque field, see
    // IBehaviorManager::Pathfind, the PathfindManager*) at function entry, before
    // the override gate below even runs; only its low 16 bits get overwritten
    // (with a "no sub-object id" sentinel) on the commit-notify path further
    // down. That path passes the dword as IObjAffect::set_target's `unsigned
    // short` SubTarget, so only the overwritten low word reaches the callee and
    // the seed itself is not modelled there (see NotifyOverrideCommitTail); the
    // same dword is the probe pointer read further down.

    // 's own "this" is NOT sysMgr itself but the small
    // embedded sub-object at sysMgr+0x1364 (Common:
    // `ECX = [ESI+4]; ECX += 0x1364; CALL the routine`) -- item 1869's
    // gate/override bytes (this_+0xc5, this_+0x14, this_+0xc) all live inside
    // that sub-object.
    IObjRW* const overrideTarget = Sys->GunConvergenceStateBytes.GetCurrentLeader(); // "local_d4", the routine

    bool skipDirectiveGate = false; // true when baseline's the branch falls into the branch directly
    // baseline's `local_f4` (frame+0x38), zeroed unconditionally at
    // and filled by the the branch probe below. It is read
    // again at module storage, deep in the arbitration tail, so it is hoisted
    // to function scope rather than living inside the probe's own block.
    IObjRW* directiveProbeTarget = nullptr;
    if (overrideTarget != nullptr && overrideTarget != owner0)
    {
        // (Sys+0x1364)+0xC1, the block's bLeaderIdleFormation byte.
        unsigned char const committedFlag = Sys->GunConvergenceStateBytes.bLeaderIdleFormation;
        if (committedFlag != 1)
        {
            // Common / Common
            // (the branch): validate the candidate override target. If it's
            // rejected (JobParams.bWaitForLeaderTarget, Sys+0x18f0, set and the leader
            // has no target), suppress this tick entirely. Otherwise baseline jumps
            // straight into the branch -- the already-committed/close-range
            // check below -- skipping the directive gate (the branch)
            // entirely; the override is not applied on this path either way.
            // `overrideTarget` is not reached through one of the four recognised
            // IObjRW-provenance idioms, but baseline compares it against `owner0` directly
            // above, so it is the same kind of pointer, and slot 0x74's shape matches
            // get_target exactly.
            overrideTarget->get_target(directiveProbeTarget);
            if (Job.bWaitForLeaderTarget == 1 && directiveProbeTarget == nullptr)
            {
                return;
            }
            skipDirectiveGate = true;
        }
        else
        {
            // Common / Common
            // (the branch/the branch tail): override already committed --
            // refresh-notify the current owner and return either way. The
            // notify call's "this" is owner0+4 (a SECOND, embedded vtable
            // sub-object 4 bytes into owner0), not owner0 itself -- analysis
            // confirms this is the SAME double-dereference vtbl-call idiom
            // used everywhere else in this cluster (`**(code**)(OBJ+N)+SLOT`),
            // just with N=4 instead of N=0 (NOT "one pointer hop shorter" as
            // a previous pass suspected -- that suspicion is now resolved).
            NotifyOverrideCommitTail(Sys);
            return;
        }
    }
    // else (overrideTarget == nullptr, or it already equals the current
    // owner): baseline falls straight into the directive gate below
    // (the branch), same as skipDirectiveGate == false.

    // --- Common (the branch): directive gate ---
    if (!skipDirectiveGate)
    {
        IDirectiveInfo const* directive = behaviorMgr->get_current_directive();
        unsigned char const* directiveBytes = reinterpret_cast<unsigned char const*>(directive);
        int const directiveKind = *reinterpret_cast<int const*>(directiveBytes);
        int const directiveArg = *reinterpret_cast<int const*>(directiveBytes + 4);
        int const directiveTag = *reinterpret_cast<int const*>(directiveBytes + 8);
        bool const isIdleDirective = (directiveKind == 1 && directiveArg == 0);
        if (directiveTag != 0x14 && directiveTag != 6 && isIdleDirective)
        {
            return;
        }
    }

    // --- Common (the branch): already-committed / close-range override ---
    if (bCommitted == 1)
    {
        return;
    }

    // the owner (Owner->Watcher's object - 8; the `XOR ECX,ECX` arm at
    // dereferences 0, no guard) answers IObjInspect::get_position() (slot 0,
    // `CALL [EDX]`, no arguments) and baseline copies the three floats into its frame
    // (+0x40/+0x44/+0x48) -- a snapshot, kept here as a Vector by value. Retyped
    // 2026-10-09 (claude-code:cmn-bug16) from a raw vt[0] cast plus invented null checks.
    Vector const selfPos =
        ObjRwFromWatchable(const_cast<Watchable*>(Sys->Watcher.target()))->get_position();

    bool committedThisTick = false;
    if (AttackState == 0)
    {
        // the three differences are stored as floats into Vector(x, y, z)
        // (the routine), the squared length is (x*x + z*z) + y*y on the x87 stack, and the
        // inlined 0x5f3759df length runs on that unrounded value.
        float const dx = AnchorPosition.x - selfPos.x;
        float const dy = AnchorPosition.y - selfPos.y;
        float const dz = AnchorPosition.z - selfPos.z;
        double const dist = CmnAiSteer::InlineFastLength(
            ((double)dx * dx + (double)dz * dz) + (double)dy * dy);

        // FCOMP [Sys+0x1830] (JobParams.Field_0C); TEST AH,0x1; JNZ skips
        // below-or-unordered.
        if (dist >= (double)Job.Field_0C)
        {
            // IBehaviorManager::ContentCbPtr (+0x110), when set, gets
            // ContentCallback::combat_drift_event() (slot +0x20, no arguments).
            pub::AI::ContentCallback* const Listener = behaviorMgr->ContentCbPtr;
            if (Listener != nullptr)
            {
                Listener->combat_drift_event();
            }
            bCommitted = true;
            SetAttackState(2);
            committedThisTick = true;
        }
    }

    // Common (label the branch, reached via goto):
    // baseline does not just `return` right after the notify+SetAttackState(2)
    // above -- it falls into the SAME shared tail piece (a) uses for its own
    // "override already committed" exit (see NotifyOverrideCommitTail above,
    // resolved via analysis as part of d215:item1896-directive-branch -- no
    // longer the ambiguous "one pointer hop shorter" call a previous pass
    // left unresolved).
    if (committedThisTick)
    {
        NotifyOverrideCommitTail(Sys);
        return;
    }

    // --- Common: AmbientScriptInfo flag decode ---
    // AmbientScriptInfo::clear (Common, a genuine
    // exported method) is a trivial 3-dword zero-fill on a local buffer; its
    // owning class is not modeled here, so its one observed effect is
    // reproduced inline rather than introducing a whole new header for a
    // 3-instruction method.
    int ambientFlags[3] = { 0, 0, 0 }; // AmbientScriptInfo::clear-equivalent
    // `LEA ECX,[ESP+0x2c]; PUSH ECX; PUSH EBX; MOV ECX,ESI; CALL the routine`
    // (module storage-062ed9dd): this TargetManager, the own ship's SelfTarget.Iface
    // (Sys+0x1580), the 3-int buffer.
    bool const decoded = QueryZonePropertyLevels(Sys->SelfTarget.Iface, ambientFlags) != 0;
    bool useAmbientTier = false;
    if (decoded)
    {
        int const ambientTier = ClassifyZoneLevelTier(ambientFlags);
        useAmbientTier = (ambientTier <= Job.FieldTargeting);   // Sys+0x18f8
    }
    // `useAmbientTier` (baseline: uStack_110._2_1_, the frame byte at +0x1e)
    // gates one filter inside the second neighbour-scan loop below; that loop
    // is wired for real as of 2026-09-17, so this is no longer dead.

    // --- Common (the branch-the branch): the first
    // "neighbour" scan loop. WIRED IN 2026-09-17 (claude:common-iustategraph).
    // The old TODO here called this loop unportable because it needs
    // /the routine; both are now real (BuildAiTargetList, item
    // 1800, and GetCachedRange, item 1791), and nothing in the loop touches
    // the the routine/the routine pair -- which the SECOND loop uses and
    // which are themselves ported now.

    // It walks the owner's AI target list (SystemManager+0x34) and, for every
    // hostile entry, bumps two counters and -- for anything both maximally
    // hostile and physically close -- tightens a hostility threshold that the
    // second loop consumes (that loop is wired for real as of 2026-09-17).
    // Nothing in THIS loop touches the the routine/the routine pair.
    float hostileThreshold = -0.6f;              // baseline's [ESP+0x20] seed, module storage
    // baseline keeps the vector pointer in the frame slot at +0x3c
    // (`MOV [ESP+0x3c],EBX` at module storage, reloaded at module storage) and
    // the SECOND loop walks the very same object, so it is hoisted here rather
    // than rebuilt. It is written before the emptiness test, so it is always
    // valid by the time the second loop runs.
    FAiTargetVector* savedTargets = nullptr;
    {
        FAiTargetList* const targetList = GetAiTargetList(Sys);
        FAiTargetVector* const targets = BuildAiTargetList(targetList);
        savedTargets = targets;

        // JobParams.Field_00 (Sys+0x1824) chooses a fraction of 5000.0f
        // (module storage). Cases 1 and 2 multiply by the doubles at
        // / 0639e0e8 (2/3 and 1/3 exactly, as
        // 55555555 / 0x3FD5555555555555); anything else keeps the
        // full 5000. The multiply happens in x87 double precision, so the
        // comparison below is done in double too.
        int const proximityTierSel = Job.Field_00;
        double proximityLimit = 5000.0;
        if (proximityTierSel == 1)
        {
            proximityLimit = 5000.0 * 0.66666666666666663;
        }
        else if (proximityTierSel == 2)
        {
            proximityLimit = 5000.0 * 0.33333333333333331;
        }

        for (FAiTarget* elem = targets->Begin; elem != targets->End; ++elem)
        {
            float attitude = 0.0f;
            float reverseAttitude = 0.0f;
            GetCachedRange(elem, &attitude, &reverseAttitude);

            // skip unless attitude <= -0.6f.
            if (!(attitude <= -0.6f))
            {
                continue;
            }
            ++NumCandidatesA;

            // skip unless attitude <= -1.0f (module storage),
            // i.e. only maximally hostile entries reach the proximity test.
            if (!(attitude <= -1.0f))
            {
                continue;
            }
            ++NumCandidatesB;

            // calls slot +0x10 (get_orientation), which baseline
            // ICF-folds with slot +0x0C (get_transform) -- the returned
            // pointer is the same Transform either way, and the loop reads its
            // +0x24/+0x28/+0x2c, i.e. Translation.
            // The slot called is +0x10 (get_orientation); the Matrix it returns is the
            // leading member of the object's Transform, and baseline reads +0x24..+0x2c past it.
            Vector const& elemPos = ((Transform const&)elem->Iface->get_orientation()).Translation;

            // baseline snapshots the owner's position into three stack slots at
            // and reads them back here; selfPos is that
            // snapshot. 062eda77-062edb01: the differences are stored as floats into
            // Vector(x, y, z) (the routine); squared length (x*x + z*z) + y*y and the
            // inlined fast length stay on the x87 stack.
            float const ndx = elemPos.x - selfPos.x;
            float const ndy = elemPos.y - selfPos.y;
            float const ndz = elemPos.z - selfPos.z;
            double const nDist = CmnAiSteer::InlineFastLength(
                ((double)ndx * ndx + (double)ndz * ndz) + (double)ndy * ndy);

            // FXCH; FCOMP; TEST AH,0x5; JP -- only strictly below passes.
            if (nDist < proximityLimit)
            {
                hostileThreshold = -1.0f;
            }
        }
    }
    // `hostileThreshold` is consumed by the second neighbour-scan loop below
    // (baseline's frame slot at +0x20), which is wired for real as of
    // 2026-09-17 -- so this is no longer a computed-but-unused value.

    // --- Common: aggression-ratio early return ---
    // Thresholds are baseline's own raw float bit patterns (Common
    // 123204-123219: 0x3e19999a/0x3eb33333/0x3f266666/0x3f59999a/0x3f800000,
    // default 0x3f000000), reproduced via bit-exact memcpy rather than
    // decimal float literals to avoid any rounding-equivalence doubt --
    // numerically these are 0.15/0.35/0.65/0.85/1.0, default 0.5.
    static unsigned const kAggressionThresholdBits[5] = {
        0x3e19999au, 0x3eb33333u, 0x3f266666u, 0x3f59999au, 0x3f800000u,
    };
    static unsigned const kAggressionThresholdDefaultBits = 0x3f000000u;
    int const aggressionTierSel = Job.SceneToughnessThreshold;   // Sys+0x1828
    unsigned const aggressionThresholdBits =
        (aggressionTierSel >= 0 && aggressionTierSel <= 4)
            ? kAggressionThresholdBits[aggressionTierSel]
            : kAggressionThresholdDefaultBits;
    float aggressionThreshold;
    std::memcpy(&aggressionThreshold, &aggressionThresholdBits, sizeof(float));

    // `MOV ECX,ESI; CALL the routine` -- ESI is this TargetManager.
    // the ratio stays in ST0; FST float stores the rounded copy, the two
    // compares (== 0.0f via TEST AH,0x44 / JNP, then > threshold via TEST AH,0x41 / JNZ)
    // read the unrounded value.
    double const aggressionRatio = GetHostilePowerRatio();
    AggressionRatio = (float)aggressionRatio;
    if (aggressionRatio != 0.0 && (double)aggressionThreshold < aggressionRatio)
    {
        SetAttackState(1);
        return;
    }

    // --- Common (module storage-062edf33): the SECOND
    // neighbour-scan loop. WIRED IN 2026-09-17 (claude:common-attack-goto).

    // This was the last FULLPORT-DEFERRED piece of item 1896, and the reason
    // it carried ("the per-candidate filter chain is still untranslated") was
    // accurate -- unlike the two reasons before it, both of which were false.
    // Nothing new had to be researched: the two AiVolumeProbe entry points
    // were already pinned by the previous pass, and every vtable slot the
    // chain dispatches through is a named IObjInspect method in
    // common_iface.h:
    //     +0x10  get_orientation()  (ICF-folded with get_transform)
    //     +0x20  get_id()
    //     +0x2c  get_physical_radius(float&, Vector&)
    //     +0x40  get_status(float& Health)
    //     +0x88  get_type(unsigned&)
    //     +0xbc  is_player()
    //     +0x150 cobject()
    // The receiver for both probe calls is the frame slot at +0x18, loaded
    // once at module storage from `*(*(*(this+4)+4)+0xc)` ==
    // `*(IBehaviorManager+0x0c)` -- the same frame dword (local_114) whose low
    // word the commit tails overwrite; read here as a pointer -- the
    // IBehaviorManager's PathfindManager.
    // (read once at entry, 062ed791 / 062ed7b8 -- see `probe` there)

    // one up-front probe query whose OutDir result is
    // discarded; only "did it answer at all" is kept. `NEG EAX; SBB AL,AL;
    // INC AL` is the compiler's `AL = (EAX == 0)` -- NEG sets CF exactly when
    // EAX is non-zero, so SBB gives 0/-1 and INC gives 1/0.
    Vector probeDirScratch = { 0.0f, 0.0f, 0.0f };           // frame+0x60
    bool const skipContainmentTest =
        (probe->GetUserZoneApproachDirection(&probeDirScratch) == 0); // , item 1686

    // frame+0x6c, 16 entries; baseline stores raw FAiTarget pointers and never
    // writes past 16 (the `CMP EAX,0x10 / JGE` guard is baseline's own, at BOTH
    // insertion points, and it drops the candidate rather than growing).
    FAiTarget* candidateSlots[16] = {};
    int matchCount = 0;                                       // frame+0x14, seeded 0 at module storage

    for (FAiTarget* cand = savedTargets->Begin; cand != savedTargets->End; ++cand)
    {
        // an unconditional side effect, not a filter
        // -- refresh the cached targeted-enemy count from any player-controlled
        // CShip in the list. The 0x503 mask on CShip+0x4c is the same field
        // tests (see its own loop above).
        if (cand->Iface->is_player())
        {
            CShip* const candShip =
                (CShip*)cand->Iface->cobject();
            if (candShip != nullptr && ((unsigned)candShip->ObjectClass & 0x503u) == 0x503u)
            {
                TargetedEnemyCount = candShip->get_targeted_enemy_count();
            }
        }

        // the "pinned id" list at this+0x18 (count)
        // and this+0x1c (dwords). A candidate whose id is on it is admitted
        // immediately -- and baseline then STILL runs the whole filter chain on
        // it below, so a pinned candidate that also passes can be inserted
        // twice. Reproduced as-is; the inner loop has no early break either.
        // the count is re-read on every pass.
        for (int pinned = 0; pinned < NumForcedTargets; ++pinned)
        {
            if (cand->Iface->get_id() != ForcedTargets[pinned])
            {
                continue;
            }
            if (matchCount < 16)
            {
                candidateSlots[matchCount] = cand;
                ++matchCount;
            }
        }

        // refresh the cached type flags on demand.
        if (cand->bFlagsValid == 0)
        {
            cand->Iface->get_type(cand->CachedFlags);
            cand->bFlagsValid = 1;
        }

        // anything whose flags survive the 0xFDFF
        // mask must also still be alive. `FCOMP` against the DOUBLE at
        // .rdata:the field, whose eight bytes are all zero (listing line
        // 558810) -- so the threshold is 0.0. `TEST AH,0x44; JNP` fires ONLY
        // on C3-alone, i.e. only on exact equality: a NaN or any non-zero
        // health passes.
        if ((cand->CachedFlags & 0xfdffu) != 0)
        {
            float health = 1.0f; // baseline seeds the out slot with 0x3f800000
            cand->Iface->get_status(health);
            if ((double)health == 0.0)
            {
                continue;
            }
        }

        // while JobParams.bAllowPlayerTargeting (Sys+0x1952) is
        // clear, player ships are excluded outright.
        if (Job.bAllowPlayerTargeting == 0 && cand->Iface->is_player())
        {
            continue;
        }

        // same hostility gate the first loop uses,
        // but against the threshold that loop may have tightened to -1.0f.
        // `TEST AH,0x41; JP` skips on greater-or-unordered.
        float candAttitude = 0.0f;
        float candReverseAttitude = 0.0f;
        GetCachedRange(cand, &candAttitude, &candReverseAttitude); // , item 1791
        if (!(candAttitude <= hostileThreshold))
        {
            continue;
        }

        // proximity to the owner, against the same
        // personality-scaled 5000.0f limit the first loop computes. baseline
        // accumulates (dz*dz + dy*dy) + dx*dx and inlines the fast length
        // (magic 0x5f3759df, 0.5f at module storage, 1.5f at module storage)
        // on the unrounded sum. The limit multiply happens in x87 double, so
        // the compare is done in double too. `TEST AH,0x5; JP` skips on
        // greater-or-equal-or-unordered: only strictly closer passes.
        // CORRECTED 2026-10-14 (CMN-S2 AIS): the slot is +0x10 (get_orientation, the Matrix
        // leading the Transform), the differences at 062edd22-062edd3e are never stored as
        // floats, and the length is the inlined idiom -- not FastInvSqrt (062df7f0), which
        // the port used to call and which rounds its argument to float.
        Vector const& candPos = ((Transform const&)cand->Iface->get_orientation()).Translation;
        double const cdx = (double)candPos.x - selfPos.x;
        double const cdy = (double)candPos.y - selfPos.y;
        double const cdz = (double)candPos.z - selfPos.z;
        double const candDist = CmnAiSteer::InlineFastLength((cdz * cdz + cdy * cdy) + cdx * cdx);

        int const proximityTierSel2 = Job.Field_00;   // Sys+0x1824
        double proximityLimit2 = 5000.0; // .rdata:the field == 0x459C4000
        if (proximityTierSel2 == 1)
        {
            proximityLimit2 = 5000.0 * 0.66666666666666663;
        }
        else if (proximityTierSel2 == 2)
        {
            proximityLimit2 = 5000.0 * 0.33333333333333331;
        }
        if (!(candDist < proximityLimit2))
        {
            continue;
        }

        // when a directive override target exists and
        // is not the owner itself, the candidate must also be within
        // JobParams.MaximumLeaderTargetDistance (Sys+0x18f4) of THAT object. `local_d4` is the frame slot at
        // +0x58, i.e. the the routine result the port already holds as
        // `overrideTarget`. Its position comes through vtable slot 0
        // (get_position) on the raw pointer -- baseline does NOT apply the -8
        // Watchable adjustment here, unlike the owner read one line earlier.
        // `TEST AH,0x41; JZ` skips when neither C0 nor C3 is set, i.e. when
        // the distance is strictly greater.
        if (overrideTarget != nullptr && overrideTarget != owner0)
        {
            // slot +0x10 again, then the override's get_position; the
            // differences stay unrounded, (dz*dz + dy*dy) + dx*dx, inlined fast length.
            Vector const& candPos2 = ((Transform const&)cand->Iface->get_orientation()).Translation;
            Vector const& ovrPos = overrideTarget->get_position();
            double const odx = (double)candPos2.x - ovrPos.x;
            double const ody = (double)candPos2.y - ovrPos.y;
            double const odz = (double)candPos2.z - ovrPos.z;
            double const ovrDist = CmnAiSteer::InlineFastLength((odz * odz + ody * ody) + odx * odx);
            if (ovrDist > (double)Job.MaximumLeaderTargetDistance)
            {
                continue;
            }
        }

        // the ambient-severity gate, run per
        // candidate this time (the earlier one ran once, on
        // SelfTarget.Iface, Sys+0x1580). baseline reuses the SAME 3-int buffer slot
        // at frame+0x2c and zeroes it inline instead of calling
        // AmbientScriptInfo::clear.
        if (useAmbientTier)
        {
            int candAmbientFlags[3] = { 0, 0, 0 };
            if (QueryZonePropertyLevels(cand->Iface, candAmbientFlags) == 0)
            {
                continue;
            }
            if (ClassifyZoneLevelTier(candAmbientFlags) > Job.FieldTargeting)   // Sys+0x18f8
            {
                continue;
            }
        }

        // the volume-containment test, skipped
        // wholesale when the up-front probe query returned 0.
        if (!skipContainmentTest)
        {
            if (cand->bPosValid == 0)
            {
                cand->Iface->get_physical_radius(cand->CachedRadius, cand->CachedPos);
                cand->bPosValid = 1;
            }
            float radiusScratch = cand->CachedRadius; // frame+0x50, the arg
                                                      // never reads
            // baseline repeats the bPosValid refresh verbatim at
            // . It is DEAD: the block above has already
            // set the byte to 1 on every path that reaches here. Two separately
            // inlined copies of the same accessor, the same duplication pattern
            // this function shows at module storage / module storage. Not
            // reproduced.
            if (!probe->UserZoneContainsPoint(&cand->CachedPos)) // , item 1672
            {
                continue;
            }
            (void)radiusScratch;
        }


        if (matchCount < 16)
        {
            candidateSlots[matchCount] = cand;
            ++matchCount;
        }
    }

    // --- Common (the branch): arbitration ---
    // WIRED IN 2026-09-17. This branch was unreachable while the loop above
    // was stubbed, and the port said so honestly; it is reachable now, so
    // leaving it out would have turned a documented gap into a wrong answer.
    NumSelected = matchCount;
    int selectionPriority = 0xf; // frame+0x4c, baseline's own seed
    if (matchCount > 0)
    {
        // the current target, fed to the routine as
        // its "sticky" candidate. baseline re-reads SystemManager+0x8 rather
        // than reusing the owner it resolved at function entry.
        // The `XOR ECX,ECX` arm at 062edf67 dereferences 0: no guard (CMN-S2 AIS fix).
        IObjRW* stickyTarget = nullptr;
        ObjRwFromWatchable(const_cast<Watchable*>(Owner->Watcher.target()))->get_target(stickyTarget);

        // push order is (&priority, sticky, count,
        // &candidates[0]), so the array is the first argument.
        FAiTarget* const selected = SelectAttackCandidate(candidateSlots, matchCount,
                                                          stickyTarget, &selectionPriority);
        if (selected != nullptr)
        {
            IObjInspect* const selectedIface = selected->Iface;


            if (selectedIface->is_player())
            {
                bSelectedIsPlayer = true;
            }

            SelectionPriority = selectionPriority;

            // commit the new target unless the owner
            // is already on it -- either literally the same interface pointer,
            // or the same object id when both sides are non-null.
            // SUBOBJ_ID_NONE is module storage and its two bytes are 00 00
            // (listing line 568142), i.e. 0.
            unsigned const subObjNone = 0; // SUBOBJ_ID_NONE
            // Owner re-resolved at 062edfc4-062edfe1 and 062ee006-062ee037; both `XOR` arms
            // dereference 0, no guard (CMN-S2 AIS fix).
            IObjRW* currentTarget = nullptr;
            ObjRwFromWatchable(const_cast<Watchable*>(Owner->Watcher.target()))->get_target(currentTarget);

            bool bAlreadyOnTarget = ((void*)currentTarget == (void*)selectedIface);
            if (!bAlreadyOnTarget && currentTarget != nullptr && selectedIface != nullptr)
            {
                // the current target's get_id runs first, then the pick's.
                unsigned int const CurrentId = currentTarget->get_id();
                bAlreadyOnTarget = (CurrentId == selectedIface->get_id());
            }
            if (!bAlreadyOnTarget)
            {
                // IObjAffect lives at IObjRW+4 (`LEA ECX,[EAX+4]`, 062ee030).
                (void)static_cast<IObjAffect*>(ObjRwFromWatchable(const_cast<Watchable*>(Owner->Watcher.target())))
                    ->set_target((IObjRW*)selectedIface, (unsigned short)subObjNone, -1);
            }

            // (module storage-062ee04f)
            SetAttackState(0);
            // JobParams.bForceAttackFormation (Sys+0x1958).
            if (Job.bForceAttackFormation == 0)
            {
                return;
            }

            if (skipDirectiveGate)
            {
                // the directive-override path also
                // re-points the override object itself, unless it is already
                // on the same target.
                // Every owner re-resolution below has baseline's dereferencing `XOR` arm
                // (062ee071, 062ee0ab, 062ee0f2): no guard (CMN-S2 AIS fix).
                IObjRW* ownerTargetD = nullptr;
                ObjRwFromWatchable(const_cast<Watchable*>(Owner->Watcher.target()))->get_target(ownerTargetD);
                // frame+0x38 is `local_f4`, the IObjRW* the the branch
                // directive probe wrote earlier; the port holds it as
                // `directiveProbeTarget`.
                if (directiveProbeTarget == ownerTargetD)
                {
                    return;
                }
                IObjRW* ownerTargetE = nullptr;
                ObjRwFromWatchable(const_cast<Watchable*>(Owner->Watcher.target()))->get_target(ownerTargetE);
                if (ownerTargetE == directiveProbeTarget)
                {
                    return;
                }
                if (ownerTargetE != nullptr && directiveProbeTarget != nullptr)
                {
                    // the owner's target's get_id first, then the probe's.
                    unsigned int const OwnerTargetId = ownerTargetE->get_id();
                    if (OwnerTargetId == directiveProbeTarget->get_id())
                    {
                        return;
                    }
                }
                (void)static_cast<IObjAffect*>(ObjRwFromWatchable(const_cast<Watchable*>(Owner->Watcher.target())))
                    ->set_target(directiveProbeTarget, (unsigned short)subObjNone, -1);
                return;
            }

            // no directive override -- push the new
            // target onto every non-player formation follower of the owner's
            // own ship instead.
            // the owner (dereferencing `XOR` arm, no guard) -> cobject().
            CShip* ownerShip = (CShip*)
                ObjRwFromWatchable(const_cast<Watchable*>(Owner->Watcher.target()))->cobject();
            if (ownerShip == nullptr || ((unsigned)ownerShip->ObjectClass & 0x503u) != 0x503u)
            {
                ownerShip = nullptr; // `XOR EAX,EAX`
            }
            IObjRW* followers[0x20] = { nullptr };
            // baseline calls CShip::get_formation_followers with ECX = the
            // value above -- 0 on the reject path, which faults inside the callee. Reproduced
            // (CMN-S2 AIS fix): the former `ownerShip ? ... : 0` guard was invented.
            unsigned const followerCount =
                (unsigned)ownerShip->get_formation_followers(followers, 0x20);
            // `JBE` / `JC` -- unsigned count and index.
            for (unsigned f = 0; f < followerCount; ++f)
            {
                IObjRW* const follower = followers[f];
                if (follower == nullptr || ObjInspectOf(follower)->is_player())
                {
                    continue;
                }
                (void)ObjAffectOf(follower)->set_target((IObjRW*)selectedIface,
                                                   (unsigned short)subObjNone, -1);
            }
            return;
        }
        // A null pick falls through to the branch below.
    }

    // --- Common (the branch): "nothing selected" tail.
    // CORRECTED 2026-09-17: the port used to write a hard 0 into this+0x5c and
    // then call get_target purely for its side effect, with a comment saying
    // baseline's follow-up "re-notifies the previous target when local_118 != 0,
    // unreachable while the candidate count is always 0". Both halves were
    // wrong. this+0x5c is the match count, written at module storage before
    // the branch, and the follow-up's condition is the get_target OUT
    // parameter at frame+0x18 -- not local_118 -- so it was always reachable:
    // when the owner still holds a target and nothing was selected this tick,
    // baseline CLEARS that target (slot_48 with a null object).
    {
        // Both owner resolutions (062ee1cc, 062ee1ef) dereference the `XOR` arm: no guard
        // (CMN-S2 AIS fix).
        IObjRW* previousTarget = nullptr;
        ObjRwFromWatchable(const_cast<Watchable*>(Owner->Watcher.target()))->get_target(previousTarget);
        if (previousTarget != nullptr)
        {
            (void)static_cast<IObjAffect*>(ObjRwFromWatchable(const_cast<Watchable*>(Owner->Watcher.target())))
                ->set_target(nullptr, SUBOBJ_ID_NONE, -1);
        }
    }
    SetAttackState(2);
}



// ---------------------------------------------------------------------------
// TargetManager::RetargetOwner -- the routine, Common module storage-062ee9c2
// (RET 8). Added 2026-10-08 (claude-code:cmn-ai4) for FaceBehavior::evaluate.
// ---------------------------------------------------------------------------
bool TargetManager::RetargetOwner(IObjRW* Target, unsigned short SubTarget)
{
    // [[this+4]+8] -8 (null arm), get_target (+0x74) into a zeroed local.
    IObjRW* Current = nullptr;
    IObjRW* const Ship = ObjRwFromWatchable(const_cast<Watchable*>(Owner->Watcher.target()));
    Ship->get_target(Current);

    if (Current == Target)
        return false;
    if (Current != nullptr && Target != nullptr &&
        Current->get_id() == Target->get_id())
        return true;

    // the ship re-resolved the same way, then `LEA ECX,[EAX+4]` -- its
    // IObjAffect sub-object -- and slot 0x48 with (Target, SubTarget, -1).
    IObjRW* const Again = ObjRwFromWatchable(const_cast<Watchable*>(Owner->Watcher.target()));
    static_cast<IObjAffect*>(Again)->set_target(Target, SubTarget, -1);
    return true;
}
