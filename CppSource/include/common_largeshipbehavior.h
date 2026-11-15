// Author: Giperion.

#pragma once

#include "common_exports.h"
#include "common_behavior.h"  // Behavior
#include "common_idea.h"      // MoveIdea, FaceIdea
#include "common_iface.h"     // IObjRW, IObjInspect, ObjInspectOf, ObjRwFromWatchable

#include <cstddef>

// ============================================================================
// LargeShipBehavior -- AI "large ship (move)" state (Behavior family)
// ============================================================================

// RTTI identity (hand-decoded directly from Common raw bytes, NOT from
// analysis's .c analysis -- see "ground truth precedence" in this marathon's
// methodology). Already on file as implementation tracker item 6612 (address 0x0639ff24): matches
// the dispatch-table row a prior functional-analysis pass had already named
// "Large Ship (move)" -- RTTI now confirms the real baseline class name.

//   RTTICompleteObjectLocator @ module storage (offset field = 0, i.e. the primary --
//   and only -- vtable installs at object offset 0)
//     -> pTypeDescriptor, name ".?AVLargeShipBehavior@@"
//     -> pClassHierarchyDescriptor, numBaseClasses = 2, adjacent CHD sibling is
//        GuideBehavior (see common_guidebehavior.h; its own COL sits immediately before
//        this class's own leading vtable pointer word in .rdata, confirmed by reading the
//        raw listing straight across 0x0639feb4-0x0639ff8c)

// Single-inheritance direct child of Behavior -- same shape as FleeBehavior/CruiseBehavior,
// no secondary vtable.

// ----------------------------------------------------------------------------
// Constructor: the routine (Common), fully read end to end this pass
// (RET at 206931, right before GuideBehavior's own ctor begins at 206896... n.b. the two
// ctors sit back-to-back in .text). Calls the shared base-init field-initialising the routine (it writes +0x04..+0x29, see Behavior in common_behavior.h),
// default-constructs a MoveIdea and a FaceIdea (SubObjectA @+0x30 via the routine, 0x78
// bytes, ends +0xa8; SubObjectB @+0xa8 via the routine, 0xc0 bytes, ends +0x168 -- RTTI-
// identified, common_idea.h -- the SAME pair CruiseBehavior/LaunchBehavior/LootBehavior's own
// SubObjectA/B use), installs the vtable, sets +0x2c = 0x44bb8000 (exactly 1500.0f, verified
// bit-for-bit -- a distance/speed threshold default), and zeroes +0x168/+0x16c. No other
// fields are touched by the ctor (confirmed -- the function's only RET is immediately after
// the +0x16c zero-write).
// ============================================================================

class LargeShipBehavior : public Behavior
{
public:
    // baseline's ctor argument is the behavior type id (11, "Large Ship (move)"), pushed by the factory out of
    // the descriptor's +0x04 -- see Behavior::GetTypeId() in common_behavior.h.
    explicit LargeShipBehavior(int InTypeId);
    ~LargeShipBehavior() override;

    // ------------------------------------------------------------------
    // Vtable (module storage, 27 slots, 0x0639ff24-0x0639ff8c inclusive -- byte distance to
    // the next .rdata blob (the field) confirms this bound; byte_size=108 (108/4=27)
    // matches exactly, no correction needed here (contrast GuideBehavior's own vtable, whose
    // reported byte_size needed a slot-count correction -- see common_guidebehavior.h).

    //   0  (0x00) the routine        real (non-thunk) dtor wrapper -> the routine; ported for
    //                                 real (unwatches SubObjectB.Watcher/SubObjectA.Watcher, see below)
    //   1  (0x04) the branch        ported for real: SetContext()
    //   2  (0x08) CEquip::ConnectServerEquip      ICF fold, not LargeShipBehavior-specific
    //   3  (0x0c) the branch        ported for real: Engage()
    //   4  (0x10) the routine        ported for real: Disengage()
    //   5  (0x14) the branch        ported for real: SetModeFromFlags()
    //   6  (0x18) the branch        trivial ICF-shared "return 2" stub (same 14-way fold
    //                                 documented in common_idlebehavior.h's own slot 6), skipped
    //   7  (0x1c) the branch        ported IN FULL (2026-09-17): Update()
    //   8  (0x20) Exclusion::AsteroidExclusionZon..  ICF fold
    //   9  (0x24) IObjInspectImpl::cobject          ICF fold
    //  10  (0x28) the branch        trivial (2 insns), skipped
    //  11  (0x2c) CEquip::DisableController        ICF fold
    //  12-15 (0x30-0x3c) MetaList<GoodDesc>::operator[]  ICF fold (x4)
    //  16  (0x40) the routine        family-shared "audio cue" helper, not reachable from any
    //                                 confirmed LargeShipBehavior call site this pass traced -- FULLPORT-DEFERRED
    //  17  (0x44) the routine        ditto
    //  18  (0x48) MetaList<GoodDesc>::operator[]     ICF fold
    //  19  (0x4c) Archetype::AttachedEquipment::i..  ICF fold
    //  20  (0x50) the branch        trivial, same target as slot 10, skipped
    //  21-22 (0x54-0x58) MetaList<GoodDesc>::operator[]  ICF fold (x2)
    //  23  (0x5c) the branch        trivial (2 insns), skipped
    //  24  (0x60) RDLNode::operator_class_AttribS..  ICF fold
    //  25  (0x64) CEShield::GetEquipDesc            ICF fold
    //  26  (0x68) RDLNode::operator_class_AttribS..  ICF fold (same target as slot 24)

    // CORRECTED this pass: an earlier draft table had slots 24-26 as
    // CEShield/CEShield/RDLNode (a transcription slip); re-verified directly against the raw
    // .rdata listing (Common) -- the real order is
    // RDLNode/CEShield/RDLNode, matching the family-standard tail shape every other 27-slot
    // sibling (FleeBehavior/LaunchBehavior/LootBehavior/GuideBehavior) also has.
    // ------------------------------------------------------------------

    // Own vtable slot 7 (offset 0x1c), Update/dispatch entry point. Common
    // (the branch), `SUB ESP,0xd4` (212 bytes of locals), 684 instructions, NO analysis
    // inspect -- hand-inspected from the raw listing only, per this marathon's ground-
    // truth-precedence rule. By far the largest routine in either class this pass ported.

    // ENTRY GUARD, 2026-09-17 (claude:common-largeship-itl) -- NO LONGER A GAP. The note that
    // used to stand here said the `Handle = the routine(NavHelperPtr24+0x142c)` fetch "is not
    // reproduced ... this port simply proceeds as if Handle were always non-null, i.e. it runs
    // MORE of the function than baseline would". That deviation is now removed: the routine is
    // item 1891 and its 23 instructions decode cleanly (see ResolveNavHelperTarget above), so
    // the guard is real code and this port no longer over-runs the state machine. Handle is
    // the nav-helper's current AI target (an IObjRW*), and it is also the second argument
    // baseline hands to the immediate-threat probe -- which is itself now real code, see below.

    // Field168/Field16c were RE-TYPED (2026-09-17) from a placeholder `int` to `float` -- they
    // are genuine cooldown timers, not flags/counters. Field170 (a Vector at +0x170) is the
    // collision-avoidance steering goal the scan block computes.

    // THE DENSE INNER BODY IS NO LONGER DEFERRED (2026-09-17, claude:common-buzz-largeship).
    // Common, gated by `Field168 < 0.0f`, had stood FULLPORT-DEFERRED "AS
    // A WHOLE -- NOT decomposed into C++" through four passes, on the grounds that "every single
    // value in the entire block (the collection's contents, the ship's own position, the
    // falloff weights) is either opaque or fabricatable-only". Every clause of that was checked
    // against the tracker and the raw listing this pass, and none of it holds:

    //   * "iterates an asteroid/dust-field collection (the routine(NavHelperPtr24+0x34))".
    // is item 1800, `ready_unverified` as BuildAiTargetList
    //     (common_iustategraph.h), and the collection is not asteroids -- it is the AI SCANNER
    //     CANDIDATE LIST, a std::vector<FAiTarget> embedded in the SystemManager timer table.
    //     Its 0x88-byte stride is visible in this very function (`ADD ESI,0x88` at
    // / 062d0459 / 062d088a).
    //   * "fetches this ship's own position via an unidentified NavHelperPtr24-nested
    //     accessor". module storage-062d01b5 is the three-step idiom common_iustategraph.cpp
    //     already uses: SystemManager::Watcher (+0x08) -> -8 Watchable adjustment ->
    //     IObjInspect::get_position() (slot 0). Nothing unidentified.
    //   * "each per-element flag gate FULLPORT-DEFERRED itself". They are ordinary tests on
    //     FAiTarget::CachedFlags behind the struct's own bFlagsValid guard.
    //   * "would still require writing FABRICATED arithmetic connecting them". Nothing was
    //     fabricated -- every value is an FAiTarget field, an IObjInspect slot call, or
    //     arithmetic over those two.

    // WHAT THE BLOCK ACTUALLY DOES, now that it is compiled code (full instruction-level
    // derivation in the .cpp):
    //   Pass 1 (module storage-062d025d) averages the world positions of every candidate whose
    //     cached attitude is <= -0.6f (the field) -- the hostile contacts' centre of mass.
    //   Pass 2 (module storage-062d0553), which runs ONLY when Field16c has run down this tick,
    //     averages the positions of every candidate of type 0x80000/0x100000/0x200000 within
    //     1500 units, and offsets the goal 500 units AWAY from that crowd's bearing.
    //   The goal is then clamped to within +/-50 units of our own altitude, abandoned if it is
    //     already within 100 units, and finally
    //   Pass 3 (module storage-062d0892) sweeps a ray from us to the goal against every
    //     candidate's (radius + 500) sphere via RaySphereIntersect (item 1712) and pulls the
    //     goal back to the nearest hit -- or latches "blocked" and commits both ideas instead
    //     if we are already inside somebody's envelope.
    // So Field170 (+0x170, a Vector) is a COLLISION-AVOIDANCE STEERING GOAL, and the two
    // sub-objects are pointed at it: SubObjectA.SetRange3c(250.0f)/SetMode(0)/SetScale40(0.6f)/
    // SetPosition(Field170) and SubObjectB.SetPosition(Field170).

    // A LIVE CONTROL-FLOW BUG WAS FIXED HERE. The byte written by the Field16c clamp at
    // /062d00b6 (`[ESP+0x27]`) was recorded here as "confirmed, by re-reading the
    // complete raw analysis range, never read again anywhere in this function". It is read,
    // at module storage, spelled `[ESP+0x2b]` -- `PUSH EBP` at module storage shifts every later
    // ESP-relative displacement in the scan branch by four. It gates the whole of pass 2 onward
    // (module storage `JZ the branch`).

    // NAVHELPERPTR24 IS A SystemManager*, on two independent offsets that common_systemmanager.h
    // already static_asserts: its +0x08 is read as a BaseWatcher (offsetof(SystemManager,
    // Watcher) == 0x08) and its +0x34 is passed to BuildAiTargetList, whose parameter type IS
    // SystemManager+0x34 (offsetof(SystemManager, Timers) == 0x34). The field keeps its `void*`
    // declaration so the sibling methods reading its +0x142c/+0x12c need not change this wave;
    // Update() casts once, against those asserts.

    // Outer SHAPE (entry guard; the "immediate threat" early-return branch; the two independent
    // Field16c/Field168 cooldown-timer gates; the shared tail) was already real, and every x87
    // FCOM/FNSTSW/TEST-AH polarity in it was independently re-derived from the raw bytes
    // (the field = 0.0 double, the field = 0.0f, the field = 5.0f, the field =
    // 30.0f, all bit-exact). The scan block's own constants were read the same way this pass:
    // = -0.6f, the field = 0.6f, the field = 250.0f, the field = 500.0f,
    // = 500.0f, the field = 1500.0f, the field = 100.0f, the field =
    // 50.0f, the field = 1.5f, the field = 0.5f, the field = 1.0f.

    // ONE RECORDED SIGNATURE DISCREPANCY, in a file this lane does not own: baseline passes
    // RaySphereIntersect's radius BY ADDRESS (`LEA EDX,[ESP+0x30]` at module storage; the callee
    // does `MOV EAX,[ESP+0x10]; FLD [EAX]` at module storage), but common_aisteer.h declares the
    // parameter as a plain `float`. The port is internally consistent and computes the same
    // value; the baseline signature is `float const&`. Reported, not edited.
    // ------------------------------------------------------------------

    // Slots 1/3/4/5 are overrides since 2026-10-08 (claude-code:cmn-ai5); until then they
    // were plain members the manager's Behavior* dispatch never reached.

    // Slot 1, the branch (RET 0xc).
    bool SetContext(void* Owner, void* Manager, void* Sys) override;

    // Slot 3, the branch.
    void Slot3() override;

    // Slot 4, the routine.
    void Slot4() override;

    // Own vtable slot 5 (offset 0x14). Common (the branch), 33
    // instructions. CLOSED 2026-09-17 (claude:common-largeship-itl) -- every instruction is
    // now real code. Both of this method's FULLPORT-DEFERRED stubs were stale:
    // ThreatSourcePtr14's vtbl+0x88 is IObjInspect::get_type(uint&) and the routine is fully
    // decoded (see ResolveNavHelperTarget). baseline:
    //   uint Type = 0; ThreatSourcePtr14->get_type(Type);       //
    //   int Mode = 2;
    //   if (Type & 0x80000)                                      //
    //       if (ResolveNavHelperTarget(NavHelperPtr24 + 0x142c)) Mode = 1;
    //   if (Flag29) { ModeSelector0xc = 3; return 3; }
    //   ModeSelector0xc = Mode; return Mode;
    // Note the 0x80000 test is on get_type's OUT-PARAMETER, not on its int return value --
    // baseline discards the return entirely (`TEST dword ptr [ESP+8], 0x80000`).
    int Slot5(float Value) override;

    // Own vtable slot 7 (offset 0x1c), Update/dispatch entry point. See the class comment
    // above for the full writeup.
    int Update();

    // baseline's per-tick entry point is vtable slot 7 (+0x1c), not a non-virtual member:
    // IBehaviorManager::UpdateBehaviorTick writes the accumulated tick delta and then
    // dispatches through it -- Common module storage-062d336a,
    //     FLD float ptr [ESI + 0xd4]   /   FSTP float ptr [EDI + 0x1c]
    //     CALL dword ptr [EAX + 0x1c]  ; vtable +0x1c IS Slot7
    // EDI there is a bare Behavior* out of the dispatch table, so Update() below was
    // unreachable through a Behavior* until this override existed. Base Behavior::Slot7 is
    // _purecall in baseline; see common_behavior.h. Same wiring as TrailBehavior.
    int Slot7() override { return Update(); }

private:
    // ------------------------------------------------------------------
    // 2026-09-17 (claude:common-largeship-itl): FIVE of the eight stand-ins that used to
    // live here were STALE -- the baseline address each one cited had been ported by another
    // pass since this file was written, and nobody came back to rewire the call site. They
    // are gone; their call sites now make the real call. Recorded here so the next pass does
    // not re-litigate them:
    //   SetApproachOrientationAngle -> module storage is MoveIdea::SetAngle38(float)
    //       (common_idea.h). The analysis label `TextRenderContext::set_origin_h` is an
    //       /OPT:ICF fold belonging to an unrelated, separately-ported class -- exactly this
    //       marathon's lesson #44. Engage() now calls SubObjectA.SetAngle38(pi) for real.
    //   SetApproachFlagA            -> module storage is MoveIdea::SetEnabled(bool)
    //       (common_idea.h). The old comment's own hedge ("a separate investigation
    //       tentatively identifies this as FormationMoveFx::SetEnabled") was right; the
    //       identification is now settled. SetContext() calls SubObjectA.SetEnabled(true).
    //   RefreshApproachDistance     -> the routine is ComputeWeaponSlotStat (item 1846,
    //       ready_unverified, common_aisteer.h). Engage() calls it for real.
    //   QueryThreatFlags            -> ThreatSourcePtr14's vtbl+0x88 is IObjInspect::get_type
    //       (uint& OutType), slot 34 of the baseline-order table rebuilt 2026-09-16 in
    //       common_iface.h. ThreatSourcePtr14 was never an unidentified class: SetContext()
    //       in this very file already reads get_id() off it through ObjInspectOf().
    //   CheckNavHelperCondition     -> the routine (item 1891) is fully decoded; see
    //       ResolveNavHelperTarget() below, which replaces it with the real body.
    // NotifyDisengageEffect is also gone: the routine was settled 2026-09-17 as Behavior's
    // OWN vtable slot 4 (item 1534), and its Flag29 = 0 half needs nothing unidentified, so
    // Disengage() now performs it directly.
    // ------------------------------------------------------------------

    // (item 1891) used to be ported here as a private static,
    // ResolveNavHelperTarget(NavHelperPtr24 + 0x142c), with a null guard baseline does not
    // have. It is TargetManager::GetTarget (common_iface.h) -- NavHelperPtr24 is the
    // SystemManager, +0x142c its Targeting member -- and both call sites now use it.

    // The static SetCruiseLock / ReleaseSharedState stubs are gone (2026-10-08): the routine
    // is Behavior::SetCruiseLock (a forwarder to PathfindManager::SetCruiseLock) and
    // is ~Behavior. The static stub had been hiding Behavior::SetCruiseLock.

    // ProbeImmediateThreat IS GONE (2026-09-17, claude:common-buzz-largeship). It was the last
    // FULLPORT-DEFERRED stand-in in Update(), and its stated blocker -- "a ~0xd4-byte ON-STACK
    // scratch object ... a memoizing object inspection cache belonging to the still-unported
    // ~0x45c-byte AI maneuver/docking executor (items 1274/1470/1283) ... handed to
    // at a DIFFERENT base (ESP+0x58, i.e. eight bytes lower), so the two bases are
    // two embedded sub-objects of one cache record whose layout this port does not have a type
    // for" -- was an arithmetic slip, not a missing type. module storage and module storage
    // PUSH two arguments BEFORE module storage's `LEA ECX,[ESP+0x60]`, so that LEA and
    // 's `LEA ECX,[ESP+0x58]` name the SAME address. There is one record, and
    // with the single base every offset it uses is FAiTarget's (common_aisteer.h): +0x08
    // CachedFlags, +0x0C CachedPos, +0x7D bFlagsValid, +0x84 Iface. The probe is now three
    // lines of real code inside Update(). See the .cpp for the full writeup.

    // NOTE: ReleaseSubObject()/TickSubObject()/ConstructSubObjectA()/ConstructSubObjectB()
    // (SubObjectA/B's own vtbl slot 4 "release"/Commit() and vtbl slot 1 "tick"/Update(float)
    // stand-ins, plus their FULLPORT-DEFERRED sub-object constructors) were REMOVED this pass:
    // SubObjectA/SubObjectB are now RTTI-identified as MoveIdea/FaceIdea (common_idea.h) and
    // construct/tick/release for real via their own real methods -- see Disengage()/Update()
    // in the .cpp, which now call SubObjectA.Commit()/SubObjectB.Commit() and
    // SubObjectA.Update(DeltaTime1c)/SubObjectB.Update(DeltaTime1c) directly.

public:
    // ------------------------------------------------------------------
    // Confirmed fields, ascending absolute-offset order. Everything else is Opaque padding.
    // Struct sized to the largest confirmed field (Field170 @ +0x170, ends +0x17c) -- see
    // Update()'s own class-comment writeup for how Field168/Field16c/Field170 were confirmed
    // (and Field168/Field16c re-typed from a placeholder `int` to `float`) this pass.
    // ------------------------------------------------------------------
    int TypeId;  // +0x04. The type id baseline's shared base ctor the routine parks here, taken
                 // from this class's own ctor argument -- see Behavior::GetTypeId() (common_behavior.h)
                 // for the twenty-one-factory evidence.
    unsigned CachedHandle08;  // retyped 2026-09-16: this is IObjInspect::get_id()'s result,
                              // an object id, not a handle. baseline slot 8 is `?get_id@IObjInspectImpl@@UBE?BIXZ`.       // +0x08. Set by SetContext() via the shared the routine effect.
    int ModeSelector0xc;        // +0x0c. Written by Engage() (=1) and SetModeFromFlags() (1/2/3).
    int UpdateResult10;         // +0x10. Update()'s own tri-state result (1/2/3-shaped, see Update()).
    void* ThreatSourcePtr14;    // +0x14. Set by SetContext() (Param1); vcall receiver for
                                 // SetModeFromFlags()'s own vtbl+0x88 query.
    unsigned char Opaque18[0x1c - 0x18];
    float DeltaTime1c;           // +0x1c. Zeroed by the shared the routine reset effect inside
                                  // Engage() (CONFIRMED: the routine is the SAME shared routine
                                  // every other sibling's own ResetTickState()/Engage() calls,
                                  // re-verified this pass by reading its own body directly,
                                  // Common -- it unconditionally zeroes a
                                  // dword at [this+0x1c] regardless of caller). Not touched by
                                  // the ctor itself.
    // +0x20. Set by SetContext() (Param2).

    // LEFT `void*` DELIBERATELY. SEARCHED AND NOT FOUND, 2026-09-17
    // (claude:common-behavior-fields) -- a recorded negative result, not an unexamined field.
    // The lead is real: TrailBehavior's own +0x20 is PROVEN `IBehaviorManager*`
    // (module storage `MOV ECX,dword ptr [EBP + 0x20]` into
    // `CALL IBehaviorManager::get_camera_level_status`), and FaceBehavior, GotoBehavior,
    // GuideBehavior and DelayBehavior were typed this same pass from their own such sites. The
    // shared writer the routine does NOT carry it across (fourteen SetContext() callers).
    // WHAT WAS SEARCHED: every instruction of LargeShipBehavior's own methods
    // (module storage ctor, 062cfea0, 062cfec0, 062cff40, 062cffa0, 062cfff0, 062d0050,
    // , 062d09e0 -- module storage..062d0a10) scanned for `[reg + 0x20]`. Every hit is
    // ESP-relative stack scratch or a vtable dispatch on a DIFFERENT object
    // (`CALL dword ptr [EDX + 0x20]` at module storage, 062d02f8, 062d06af, 062d06ba -- slot 8
    // on an owner interface, not a field read). No `MOV reg,[this+0x20]`, no
    // `PUSH dword ptr [this+0x20]`, no `[[this+0x20]+disp]` chain: this class writes the field
    // in SetContext() and never reads it back except through the shared the routine tail.
    // WHAT WOULD CLOSE IT: a site that loads +0x20 into ECX ahead of a call, or walks it to a
    // confirmed IBehaviorManager offset (+0x08 Sys, +0xc0 CurrentBehaviorIndex, +0xc4,
    // +0x110 ContentCbPtr, +0x118/+0x11c, +0x164 TurnSensitivity, +0x174 dispatch table).
    void* BehaviorMgrPtr20;
    SystemManager* NavHelperPtr24; // +0x24. Set by SetContext() (Param3); read by
                                 // SetModeFromFlags()/Engage() at its own +0x142c/+0x12c.
    unsigned char Flags28;      // +0x28. Cleared bit 0x1 by Engage() (shared reset effect).
    unsigned char Flag29;       // +0x29. Read by SetModeFromFlags().
    unsigned char Pad2a[0x2c - 0x2a];

    float Field2c;               // +0x2c. Set to 1500.0f by ctor (bit-exact, verified). The
                                  // Engage() now REFRESHES this for real (2026-09-17) from
                                  // ComputeWeaponSlotStat's OutAverage out-parameter, when that
                                  // call reports success. No longer a documented gap.
    MoveIdea SubObjectA;             // +0x30, ends +0xa8. RTTI-identified as MoveIdea (ctor
                                     // , common_idea.h) -- was an opaque
                                     // `unsigned char[0x78]` byte-blob prior to this pass.
                                     // sizeof(MoveIdea) == 0x78 confirmed exactly matches this
                                     // span. Real calls wired: SetContext()'s own vtbl[0] call,
                                     // Disengage()'s own vtbl[0x10] (Commit()) call, and
                                     // Update()'s own vtbl[0x4] (Update(float)) tick call.
    FaceIdea SubObjectB;             // +0xa8, ends +0x168. RTTI-identified as FaceIdea (ctor
                                     // , common_idea.h) -- was an opaque
                                     // `unsigned char[0xc0]` byte-blob prior to this pass.
                                     // sizeof(FaceIdea) == 0xc0 confirmed exactly matches this
                                     // span. Same real calls wired as SubObjectA above.

    // +0x168. Ctor-zeroed. Right after SubObjectB ends (+0xa8+0xc0), not inside it. RE-TYPED
    // this pass from a placeholder `int` to `float`: Update() (the branch) reads/writes it
    // with FLD/FSUB/FST, not integer ops -- it's a cooldown timer, decremented by DeltaTime1c
    // every tick, gating the (FULLPORT-DEFERRED) avoidance-field scan: once it goes negative
    // the scan runs and resets it to 5.0f (the field, bit-exact).
    float Field168;
    // +0x16c. Ctor-zeroed. RE-TYPED this pass from `int` to `float`, same reasoning as
    // Field168: a SEPARATE cooldown timer, decremented by DeltaTime1c every tick and clamped
    // to 0.0f once it goes <= 0.0 (compared against the field, confirmed bit-exact 0.0
    // double); Update()'s "immediate threat" early-return branch also sets it to 30.0f
    // (the field, bit-exact).
    float Field16c;
    // +0x170 (ends +0x17c). Written by Update()'s avoidance-scan block (Common
    // 207230-207831), which is fully ported as of 2026-09-17. It is the COLLISION-AVOIDANCE
    // STEERING GOAL: the hostile contacts' centre of mass, pushed 500 units away from the
    // nearby-traffic crowd's bearing, clamped to within +/-50 units of our own altitude, and
    // finally pulled back to the first thing a ray from us to it actually hits. Both sub-objects
    // are aimed at it (SubObjectA.SetPosition / SubObjectB.SetPosition). Not read by any OTHER
    // method here (SetContext/Engage/Disengage/SetModeFromFlags never touch it).
    Vector Field170;
};

static_assert(offsetof(LargeShipBehavior, CachedHandle08) == 0x08, "LargeShipBehavior::CachedHandle08 offset");
static_assert(offsetof(LargeShipBehavior, ModeSelector0xc) == 0x0c, "LargeShipBehavior::ModeSelector0xc offset");
static_assert(offsetof(LargeShipBehavior, UpdateResult10) == 0x10, "LargeShipBehavior::UpdateResult10 offset");
static_assert(offsetof(LargeShipBehavior, ThreatSourcePtr14) == 0x14, "LargeShipBehavior::ThreatSourcePtr14 offset");
static_assert(offsetof(LargeShipBehavior, DeltaTime1c) == 0x1c, "LargeShipBehavior::DeltaTime1c offset");
static_assert(offsetof(LargeShipBehavior, BehaviorMgrPtr20) == 0x20, "LargeShipBehavior::BehaviorMgrPtr20 offset");
static_assert(offsetof(LargeShipBehavior, NavHelperPtr24) == 0x24, "LargeShipBehavior::NavHelperPtr24 offset");
static_assert(offsetof(LargeShipBehavior, Flags28) == 0x28, "LargeShipBehavior::Flags28 offset");
static_assert(offsetof(LargeShipBehavior, Flag29) == 0x29, "LargeShipBehavior::Flag29 offset");
static_assert(offsetof(LargeShipBehavior, Field2c) == 0x2c, "LargeShipBehavior::Field2c offset");
static_assert(offsetof(LargeShipBehavior, SubObjectA) == 0x30, "LargeShipBehavior::SubObjectA offset");
static_assert(offsetof(LargeShipBehavior, SubObjectB) == 0xa8, "LargeShipBehavior::SubObjectB offset");
static_assert(offsetof(LargeShipBehavior, Field168) == 0x168, "LargeShipBehavior::Field168 offset");
static_assert(offsetof(LargeShipBehavior, Field16c) == 0x16c, "LargeShipBehavior::Field16c offset");
static_assert(offsetof(LargeShipBehavior, Field170) == 0x170, "LargeShipBehavior::Field170 offset");
static_assert(sizeof(LargeShipBehavior) >= 0x17c, "LargeShipBehavior minimum size (largest field actually read/written)");

// baseline object size, read off the factory body that allocates this class (`PUSH 0x180`
// immediately before `CALL operator_new`). This is an UPPER bound on purpose: the existing
// `>=` asserts in this family prove the model reaches its furthest known field, but they
// cannot catch the opposite error -- a field declared PAST the end of the baseline object.
// That is exactly what happened to FormationBehavior, which carried a block of DockBehavior
// fields up to 0x470 behind a `>= 0x470` assert while baseline allocates 0x2b8. An upper bound
// cannot fail on an incomplete model, so it is safe to assert even where the port is short.
static_assert(sizeof(LargeShipBehavior) <= 0x180, "LargeShipBehavior must not exceed its baseline size 0x180");
