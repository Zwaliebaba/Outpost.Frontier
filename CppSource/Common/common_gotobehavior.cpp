// Author: Giperion.

#include "common_gotobehavior.h"
#include "common_iface.h"   // IObjInspect / IObjRW, ObjInspectOf, ObjRwFromWatchable
#include "common_csys.h"          // Transform, Matrix, Acquire3DMathEngine
#include "common_systemmanager.h" // SystemManager: CachedPhysicalCenter/Radius, Flags1578, ExternalIface
#include "x86math.h"              // I3DMathEngine::TransformMatrix, FVector, FMatrix
#include "common_pathfind.h"      // PathfindManager::IsCruiseUnobstructed (the routine)

#include <cmath>
#include <cstdint>
#include <cstring> // std::memcpy -- the GotoOp copy in Slot10 (the branch)

// GotoBehavior -- see common_gotobehavior.h for the full RTTI/layout writeup
// this pass relied on. Ported from Common (raw
// analysis is authoritative in this module over analysis's Common
// analysis -- see this marathon's "ground truth precedence" rule).

// (Common) -- constructor. Calls the
// shared base-init helper the routine first (item 1529 -- NOT the
// REAL field-initialising base ctor: it writes +0x04..+0x29, see Behavior in common_behavior.h), default-constructs a
// MoveIdea, a FaceIdea and a SplineMoveIdea for real (SubObjectA/SubObjectB/
// SubObjectC, all three RTTI-identified, common_idea.h -- SubObjectC's class
// was resolved 2026-09-17 and its former FULLPORT-DEFERRED
// ConstructSubObjectC stand-in is gone), then the two REAL
// already-ported sub-objects (ICRSplineSegment, DirectiveGotoOp) plus the
// small the routine POD (its leading BaseWatcher modeled for real, see
// Watcher370), then explicitly zeroes/sets every remaining field baseline
// touches directly, and finally (implicitly, via this being a real C++
// object) installs the vtable -- baseline's own
// `MOV dword ptr [ESI], the routine` needs no manual equivalent.
// (The block above documents GotoBehavior::GotoBehavior, defined just below this base ctor.)

// forwards its own type-id argument to the routine at module storage.
GotoBehaviorFields::GotoBehaviorFields(int InTypeId)
    : TypeId(InTypeId)
{
    ConstructBase(InTypeId);   // Behavior's own field initialisation
}

GotoBehavior::GotoBehavior(int InTypeId)
    : GotoBehaviorFields(InTypeId)
    , SubObjectA()
    , SubObjectB()
    , SubObjectC()
    , Field2d8(0)
    , Field2dc(0)
    , Field2e0(0)
    , Field2e4(0)
    , Flag2e8(0)
    , Flag2e9(1)
    , Flag2ea(0)
    , Flag2eb(0)
    , Flag2ec(0)
    , Pad2ed{}
    , Field2f0(1.5f)
    , Field2f4(3)
    , Field2f8{}    // not written by baseline's ctor; value-initialised (Slot10/Update write it first)
    , Field304{}    // three zero dwords
    , Watcher310()
    , Spline()
    , Field358(200.0f)
    , Field35c(-1.0f)
    , Field360(1.0f)
    , Flag364(0)    // not written by baseline's ctor; Update writes it before any read
    , Flag365(1)
    , Flag366(0)
    , Flag367(0)    // not written by baseline's ctor; SetContext/Slot10 set it before any read
    , Flag368(0)
    , Flag369(0)
    , Flag36a(0)
    , Flag36b(0)
    , Flag36c(1)
    , Pad36d{}
    , Watcher370()
    , Field378(0.0f)
    , Field37c(0.0f)
    // Field380/Flags384/SavedFlags388 are the recovered tail of the shared
    // 0x1c-byte proximity-watcher struct (common_gotobehavior.h). The shared
    // ctor the routine zeroes only the FIRST FOUR DWORDS of it -- Watcher370's
    // two words plus Field378/Field37c -- and stops. baseline therefore leaves
    // these three uninitialized at construction, exactly as DockBehavior's own
    // ctor does at +0x460/+0x464/+0x468. They are zeroed here anyway: reading
    // an uninitialized member is UB in C++ (unlike x86, which just reads
    // whatever was in the block), and GotoBehavior::Update DOES read Flags384
    // (module storage) on a path reachable before the routine has necessarily
    // written it. Deliberate, documented divergence from baseline, not an
    // oversight.
    , Field380(0.0f)
    , Flags384(0)
    , SavedFlags388(0)
    , GotoOp()
{
    // baseline default-constructs SubObjectA/SubObjectB/SubObjectC here via
    // /the routine/the routine (ECX = &SubObjectA/&SubObjectB/
    // &SubObjectC) -- ALL THREE are now real MoveIdea()/FaceIdea()/
    // SplineMoveIdea() constructor calls via the member-initializer list above
    // (common_idea.h). The the routine gap closed 2026-09-17 when that ctor's
    // class was RTTI-identified as SplineMoveIdea; nothing is deferred here
    // any more.

    // Every other field write above (Field2d8.. through Flag36c, plus the
    // Watcher310/Watcher370 zero-init and the Field2f0/Field358/Field35c/
    // Field360/Flag365/Flag36c non-zero defaults) is baseline's own explicit
    // ctor body, fully reproduced via the member-initializer list -- nothing
    // further to do here.
}

// (Common) -- the real (non-thunk)
// destructor; the routine (item 1989, MSVC's scalar-deleting-destructor
// boilerplate) just calls this then conditionally frees the block.
GotoBehavior::~GotoBehavior()
{
    // baseline unwatches six BaseWatcher-shaped pointer fields via
    // BaseWatcher::set_pointer(nullptr), in this exact order: this+0x370,
    // this+0x310, this+0x204, this+0x18c, this+0xb8, this+0x40. ALL SIX are
    // now named and cleared for real as of 2026-09-17, since SubObjectC is a
    // real SplineMoveIdea (common_idea.h) rather than an opaque buffer:
    //   this+0x204 == SubObjectC(+0x164) + 0xa0 == SubObjectC.Face.Watcher
    //   this+0x18c == SubObjectC(+0x164) + 0x28 == SubObjectC.Move.Watcher
    //   this+0xb8  == SubObjectB(+0xa4)  + 0x14 == SubObjectB.Watcher
    //   this+0x40  == SubObjectA(+0x2c)  + 0x14 == SubObjectA.Watcher
    // The two SubObjectC offsets landing exactly on its embedded MoveIdea's
    // and FaceIdea's own Watcher members is independent structural
    // confirmation of SplineMoveIdea's layout, not an assumption.
    Watcher370.set(nullptr);
    Watcher310.set(nullptr);
    SubObjectC.Face.Watcher.set(nullptr);
    SubObjectC.Move.Watcher.set(nullptr);
    SubObjectB.Watcher.set(nullptr);
    SubObjectA.Watcher.set(nullptr);

    // 's tail the routine is ~Behavior, which the compiler chains after this body.
    // (The empty static ReleaseSharedState stand-in that stood here is gone, 2026-10-08.)
}

// ----------------------------------------------------------------------------
// Every own slot of GotoBehavior's vtable is a real override as of 2026-10-08
// (claude-code:cmn-ai5): see the slot list in common_gotobehavior.h. The extern "C" free
// functions that used to stand in for slots 0/1/3/4/5/6/12/14 over a raw `unsigned char*`
// (the routine / the routine / the branch / the routine / the routine / the branch /
// / the routine / the branch) are gone -- nothing dispatched to them.
// ----------------------------------------------------------------------------

// (The FULLPORT-DEFERRED `ConstructSubObjectC` stand-in that used to live here
// is gone as of 2026-09-17: the routine's class is RTTI-identified as
// SplineMoveIdea, so all three sub-objects now construct for real from the
// ctor's member-initializer list. DockBehavior/LootBehavior/LaunchBehavior
// carry a stand-in of the same NAME, but for a DIFFERENT baseline helper
// (the routine == AfterburnerIdea) -- those are a separate, still-open gap.)


// ============================================================================
// The shared 0x1c-byte proximity-watcher struct -- implementation tracker items
// 1257 / 1258 / 1259, PORTED IN FULL 2026-09-17 (claude:common-attack-goto).

// All three were on record as "DO-NOT-TOUCH DockBehavior vtable cluster", then
// (2026-09-17) as "untranslated FP geometry leaning on the still-unprocessed
// / the routine". Both reasons are dead:
//   * the DO-NOT-TOUCH scope was lifted 2026-09-11 and the enclosing address
//     range retired 2026-09-17 (item 1896's census);
//   * is item 1749, GetInterceptClearanceOfIface, and
// is item 1751, GetSignedRadialClearanceToPoint -- BOTH
//     ready_unverified (now in common_aiintercept.cpp) since 2026-09-17. Checked, not
//     assumed: `list_items` over 0x062e4b00-0x062e5000 returns both with a
//     ported_file.
// What was left was plain translation, done here from the raw listing
// (Common); analysis's Common was not
// used. Every FPU comparison below is annotated with the exact FNSTSW/TEST/Jcc
// pair it came from, because the polarity is not recoverable from the
// analysis.

// TWO SLOT IDENTIFICATIONS that make these readable, both from the baseline
// IObjInspect table rebuilt in common_iface.h:
//   vtable +0x00 == get_position()          (slot 0)   -- the routine
//   vtable +0x60 == is_cruise_active(bool&) (slot 24)  -- the routine
//   vtable +0x20 == get_id()                (slot 8)   -- the routine
// The +0x60 call is why the struct's +0x08/+0x0c thresholds get multiplied by
// its +0x10 field on some ticks and not others: +0x10 is a cruise multiplier,
// applied exactly while the watched object reports cruise engaged.
// ============================================================================

// (module storage-062c2aa5, RET 0x14, item 1254). Ported 2026-10-08
// (claude-code:cmn-ai5); its "blocked on the qhull cluster" note was stale -- the struct is
// the shared proximity watcher and its only caller is GotoBehavior::Slot10.
void ProxWatcherInit(unsigned char* Watcher, void* NavHelper, void* Target, float Near, float Far,
                     float CruiseRatio)
{
    BaseWatcher* const W = reinterpret_cast<BaseWatcher*>(Watcher);
    *reinterpret_cast<float*>(Watcher + 0x08) = Near;
    *reinterpret_cast<float*>(Watcher + 0x0c) = Far;
    // Target + 8 (or 0), set_pointer only when it differs.
    IObjRW* const TargetObj = static_cast<IObjRW*>(Target);
    W->set(TargetObj != nullptr ? static_cast<Watchable const*>(TargetObj) : nullptr);
    *reinterpret_cast<unsigned*>(Watcher + 0x14) = 0;
    *reinterpret_cast<unsigned*>(Watcher + 0x18) = 0;
    IObjRW* const Watched = ObjRwFromWatchable(const_cast<Watchable*>(W->target()));
    if (Watched == nullptr)
        return;                                                     // / 062c2a36
    float* const Ratio = reinterpret_cast<float*>(Watcher + 0x10);
    // CruiseRatio against double 0.0 (the field); TEST AH,0x41 + JNZ ->
    // not-greater or NaN takes the speed quotient.
    if (CruiseRatio > 0.0)
    {
        *Ratio = CruiseRatio;
    }
    else
    {
        // own ship (NavHelper's +0x08 Watcher target - 8) get_speed(1.0f, 4),
        // FSTP float; FDIVR by the watched object's get_speed(1.0f, 1); FSTP float.
        SystemManager* const Sys = static_cast<SystemManager*>(NavHelper);
        IObjRW* const Self = ObjRwFromWatchable(const_cast<Watchable*>(Sys->Watcher.target()));
        float const OwnSpeed = Behavior_InvokeManagerSlotD8(
            ObjInspectOf(Self), 1.0f, IObject::ThrustEquipCruise);
        double const WatchedSpeed = Behavior_InvokeManagerSlotD8(
            ObjInspectOf(Watched), 1.0f, IObject::ThrustEquipEngine);
        *Ratio = static_cast<float>(OwnSpeed / WatchedSpeed);
    }
    // TEST AH,0x5 + JP keeps >= 1.0f or unordered; strictly less -> 1.0f.
    if (*Ratio < 1.0f)
        *Ratio = 1.0f;
}

// (Common, 135/135 insns, item 1257).
void ProxWatcherRecomputeZoneFlags(unsigned char* Watcher, void* NavHelper, Vector const& RefPoint)
{
    // . baseline's second guard (`LEA ECX,[EAX-8]; TEST
    // ECX,ECX; JZ`) can only fire if the watched pointer is literally 8;
    // ObjRwFromWatchable's null return covers the first and, in practice, both.
    IObjRW* const Obj = ObjRwFromWatchable(*reinterpret_cast<void**>(Watcher + 0x00));
    if (Obj == nullptr)
        return;

    unsigned* const ZoneFlags = reinterpret_cast<unsigned*>(Watcher + 0x14);
    unsigned* const SavedFlags = reinterpret_cast<unsigned*>(Watcher + 0x18);

    // snapshot last tick's zones first; the
    // hysteresis bands below OR that snapshot back in.
    *SavedFlags = *ZoneFlags;

    // . Slot 0 is IObjInspect::get_position().
    Vector const& Pos = ObjInspectOf(Obj)->get_position();
    float const Dx = Pos.x - RefPoint.x;
    float const Dy = Pos.y - RefPoint.y;
    float const Dz = Pos.z - RefPoint.z;

    FAiTarget* const SteerElem = &((SystemManager*)NavHelper)->SelfTarget;
    // The ST0 return stays on the x87 stack until the FCOMPP below.
    double const RadialClearance = GetSignedRadialClearanceToPoint(SteerElem, RefPoint); // , item 1751

    // . baseline accumulates (z*z + x*x) + y*y from the
    // float-stored differences, then FSQRT, all unrounded, then `FXCH;
    // FCOMPP` -- so the comparison is RadialClearance against Distance, and
    // `TEST AH,0x41 / JNZ` SKIPS the write when C0|C3 is set, i.e. when
    // RadialClearance <= Distance or the compare is unordered.
    *ZoneFlags = 0;
    double const Distance = std::sqrt((static_cast<double>(Dz) * Dz + static_cast<double>(Dx) * Dx) +
                                      static_cast<double>(Dy) * Dy);
    if (RadialClearance > Distance)
        *ZoneFlags = 1;

    // . baseline re-reads the watcher slot before each of
    // the next two calls (separately-inlined accessors) and re-guards neither:
    // a null head gives a null receiver, as here.
    IObjRW* const ClearanceObj = ObjRwFromWatchable(*reinterpret_cast<void**>(Watcher + 0x00));
    // `FSTP float` (into the RefPoint argument's home slot) rounds the ST0 return.
    float const InterceptClearance = static_cast<float>(GetInterceptClearanceOfIface(SteerElem, ClearanceObj)); // , item 1749

    // . Slot +0x60 is is_cruise_active(bool&).
    IObjRW* const CruiseObj = ObjRwFromWatchable(*reinterpret_cast<void**>(Watcher + 0x00));
    bool bCruiseActive = false;
    ObjInspectOf(CruiseObj)->is_cruise_active(bCruiseActive);

    // Each limit is re-derived on the x87 stack before each compare and never stored:
    // FLD [+0x08/+0x0c], FMUL [+0x10] while cruising -- unrounded.
    float const CruiseScale = *reinterpret_cast<float*>(Watcher + 0x10);
    double const NearLimit = bCruiseActive
        ? static_cast<double>(*reinterpret_cast<float*>(Watcher + 0x08)) * CruiseScale
        : static_cast<double>(*reinterpret_cast<float*>(Watcher + 0x08));
    double const FarLimit = bCruiseActive
        ? static_cast<double>(*reinterpret_cast<float*>(Watcher + 0x0c)) * CruiseScale
        : static_cast<double>(*reinterpret_cast<float*>(Watcher + 0x0c));

    // .rdata:the field == 0x42480000 == 50.0f -- the half-width of the
    // hysteresis band around each limit (FSUBR / FADD on the stack).
    double const HysteresisBand = 50.0;
    double const Clearance = InterceptClearance;

    // . Each compare is `FCOMPP; FNSTSW AX; TEST
    // AH,0x41; JP <skip>`, so the FALLTHROUGH is "clearance <= limit" and the
    // jump covers greater-or-unordered. Inside the band the previous tick's
    // zone bits are OR'd back in, which is what makes this hysteresis rather
    // than a plain threshold.
    if (Clearance <= NearLimit - HysteresisBand)
        *ZoneFlags |= 2;
    else if (Clearance <= NearLimit + HysteresisBand)
        *ZoneFlags |= *SavedFlags;

    // . Same shape on the far limit -- note baseline
    // RETURNS immediately after setting bit 0x4 rather than falling into the
    // band test.
    if (Clearance <= FarLimit - HysteresisBand)
    {
        *ZoneFlags |= 4;
        return;
    }
    if (Clearance <= FarLimit + HysteresisBand)
        *ZoneFlags |= *SavedFlags;
}

// (Common, 99/99 insns, item 1258).
double ProxWatcherComputeSpeedCap(unsigned char* Watcher, void* OwnerContext, void* NavHelper,
                                  int Selector, float SelectorValue)
{
    // . `local_4` is pre-loaded with
    // .rdata:the field (0x00000000 == 0.0f) purely so the two early-return
    // paths have something on the FPU stack.
    IObjRW* const Obj = ObjRwFromWatchable(*reinterpret_cast<void**>(Watcher + 0x00));
    if (Obj == nullptr)
        return 0.0f;

    ThrustEquipType const ThrustType = static_cast<ThrustEquipType>(Selector);

    // . baseline reuses the param_3 home slot as float
    // scratch here (which is why analysis's inspect of this function is a
    // mess): the Value argument is SelectorValue when Selector == 1 and the
    // literal 1.0f otherwise, while the Selector passed on is the ORIGINAL
    // integer, still live in EDI.
    float const QueryValue = (Selector == 1) ? SelectorValue : 1.0f;
    // FSTP float into the SelectorValue argument's home slot.
    float const OwnerSpeed =
        Behavior_InvokeManagerSlotD8(ObjInspectOf(OwnerContext), QueryValue, ThrustType); // , item 1762

    unsigned const ZoneFlags = *reinterpret_cast<unsigned*>(Watcher + 0x14);
    double ZoneSpeed;
    if ((ZoneFlags & 3) != 0)
    {
        // . Two byte-identical arms MSVC failed to CSE
        // (one re-reads the watcher, one passes a hard zero); collapsed,
        // because Behavior_InvokeManagerSlotD8 itself returns 0.0f on a null
        // receiver -- item 1762's own confirmed null-fallback.
        ZoneSpeed = Behavior_InvokeManagerSlotD8(
            ObjInspectOf(ObjRwFromWatchable(*reinterpret_cast<void**>(Watcher + 0x00))), 1.0f, ThrustType);
    }
    else if ((ZoneFlags & 4) == 0)
    {
        // no zone at all, so the cap is the pre-loaded 0.0f.
        ZoneSpeed = 0.0f;
    }
    else
    {
        // . Linear blend of the clearance across the
        // struct's own two thresholds -- UNSCALED here, unlike
        // ProxWatcherRecomputeZoneFlags, which cruise-scales the same pair.
        float const NearThreshold = *reinterpret_cast<float*>(Watcher + 0x08);
        float const FarThreshold = *reinterpret_cast<float*>(Watcher + 0x0c);
        FAiTarget* const SteerElem = &((SystemManager*)NavHelper)->SelfTarget;
        // FSUB / FLD / FSUB / FDIVP / FSUBR all run on the x87
        // stack straight off the ST0 return; the only rounding is the final
        // `FSTP float` of Blend. Hence double for the clearance and the
        // threshold difference.
        double const Clearance = GetInterceptClearanceOfIface(SteerElem, Obj); // , item 1749
        float const Blend = static_cast<float>(
            1.0 - ((Clearance - NearThreshold) / (static_cast<double>(FarThreshold) - NearThreshold))); // FSUBR against the field (1.0f)

        // baseline multiplies the full-throttle speed by Blend TWICE, at
        // and module storage -- both reads resolve to the same
        // stack slot (ESP+0x24 before `ADD ESP,0xc`, ESP+0x18 after). This is
        // a square, not a transcription slip; re-checked against the raw bytes.
        // Both FMULs run on the stack (no rounding until the caller's store).
        float const FullSpeed = Behavior_InvokeManagerSlotD8(
            ObjInspectOf(ObjRwFromWatchable(*reinterpret_cast<void**>(Watcher + 0x00))), 1.0f, ThrustType);
        ZoneSpeed = (static_cast<double>(FullSpeed) * Blend) * Blend;

        // `FCOM the field; FNSTSW AX; TEST AH,0x5;
        // JP` -- the jump KEEPS the value and is taken on greater-or-equal or
        // unordered, so only a strictly-below-10.0f result is floored to zero.
        // == 0x41200000 == 10.0f (hand-decoded, listing line
        // 561713; it sits immediately before the " AUHH index not 0" string).
        if (ZoneSpeed < 10.0)
            ZoneSpeed = 0.0;
    }

    // `FCOM <OwnerSpeed>; TEST AH,0x41; JNZ` returns
    // the zone speed (still in ST0) when C0|C3 is set (zone <= owner, or
    // unordered) and the owner's rounded speed otherwise. Written as `!(a > b)`
    // rather than `a <= b` so the NaN case keeps baseline's answer.
    return !(ZoneSpeed > static_cast<double>(OwnerSpeed)) ? ZoneSpeed : static_cast<double>(OwnerSpeed);
}

// (Common, 60/60 insns, item 1259).
void ProxWatcherNotifyZoneChange(unsigned char* Watcher, void* NavHelper, void* BehaviorMgr,
                                 Vector const& RefPoint)
{

    IObjRW* const Obj = ObjRwFromWatchable(*reinterpret_cast<void**>(Watcher + 0x00));
    if (Obj == nullptr)
        return;

    // . Three recognised transitions between last
    // tick's snapshot (+0x18) and this tick's zones (+0x14); anything else
    // returns without notifying. baseline's third arm is reached by falling out
    // of the first, which is why the (Saved & 4) test appears twice.
    unsigned const Saved = *reinterpret_cast<unsigned*>(Watcher + 0x18);
    unsigned const ZoneFlags = *reinterpret_cast<unsigned*>(Watcher + 0x14);

    int Code;
    if ((Saved & 4) != 0 && (Saved & 2) == 0 && (ZoneFlags & 2) != 0)
        Code = 1;
    else if ((Saved & 4) == 0 && (ZoneFlags & 4) != 0)
        Code = 2;
    else if ((Saved & 4) != 0 && (ZoneFlags & 4) == 0)
        Code = 3;
    else
        return;

    // (and its two verbatim copies). Slot +0x20 is
    // IObjInspect::get_id(); `NavHelper + 0x1580` is SelfTarget.Iface, the own ship.
    unsigned const OwnerId = ((SystemManager*)NavHelper)->SelfTarget.Iface->get_id();

    // the same slot on the watched object itself.
    unsigned const WatchedId = ObjInspectOf(Obj)->get_id();

    // . The CALL at module storage does NOT clean the
    // two dwords pushed just before it -- that is what proves those two are
    // 's arguments and that slot +0x20 takes none. the routine is
    // IBehaviorManager::NotifyTetherEvent (common_behaviormanager.cpp); the result is dropped.
    (void)static_cast<IBehaviorManager*>(BehaviorMgr)->NotifyTetherEvent(
        OwnerId, WatchedId, static_cast<pub::AI::ContentCallback::TetherStatusType>(Code), RefPoint);
}

// ============================================================================
// ComputeFormationFollowerEnvelope, implementation tracker item 1855.
// 471/471 instructions, Common. PORTED IN FULL
// 2026-09-17 (claude:common-goto), from the raw listing only; analysis's
// Common body for this function is the `unaff_EBX`/`unaff_retaddr` mess
// that got it filed as `blocked` in the first place and was not consulted.
// See common_gotobehavior.h for the call shape, the receiver's identity, the
// out-struct field table, and the point-by-point retirement of the three
// stale blocker claims.

// FRAME MAP (needed to read the annotations below, and the whole reason the
// inspect is unusable). On entry ESP == E, [E] = return address, [E+4] = the
// single `Envelope*` argument. `SUB ESP,0xa4` then `PUSH EBX/EBP/ESI` puts the
// frame base at B == E-0xb0, so every `[ESP+k]` in the listing is B+(k-adjust)
// where `adjust` is however many argument dwords are live at that instruction.
// Resolved slots, all verified against every read AND every write:
//   B+0x0f  bool   bCruise            B+0x10  float MinFollowerSpeed
//   B+0x14  int    ThrustEquipType    B+0x18  float scratch (|Delta|, then Speed)
//   B+0x1c  float  MinFollowerAngularSpeed        B+0x20  float MaxProjection
//   B+0x24  float  MinAlignCos        B+0x28  Vector Delta
//   B+0x34  IObjRW** follower cursor  B+0x38  unsigned remaining count
//   B+0x3c  float  MaxDistance (WRITE-ONLY, see below)
//   B+0x40  Transform const* hoisted owner transform
//   B+0x44  Vector DesiredPos         B+0x50  Vector RotatedOffset
//   B+0x5c  Vector OwnerBack          B+0x74  Vector FormationOffset
//   B+0x80  Vector OwnerFacing        B+0x8c  Vector FollowerPos
//   B+0x98  Vector DeltaDir           B+0xa4  Vector FollowerFacing
// MSVC reuses three of them after their last read: B+0x10 becomes the angular
// target at module storage, B+0x14 becomes the acos() angle at module storage,
// and B+0x18 switches from the per-follower distance to the final speed at
// . That reuse is exactly what analysis could not follow.

// Every x87 comparison below carries the FNSTSW/TEST/Jcc it came from, because
// the polarity is NOT recoverable from the analysis:
//   `TEST AH,0x05; JP <skip>`  -> the jump is taken on "NOT less" (and on
//                                 unordered), so the fall-through is `a < b`.
//   `TEST AH,0x41; JNZ <skip>` -> the jump is taken on C0|C3, i.e. `a <= b` or
//                                 unordered, so the fall-through is `a > b`.
// Written as plain `<` / `>` on purpose: C++ gives false for either on a NaN
// operand, which is the same branch baseline takes.

// BASELINE CONSTANTS, all hand-decoded from the .rdata bytes (analysis prints no
// value for any of them):
// = 0x43960000 = 300.0f   non-cruise projection divisor
// = 0x447a0000 = 1000.0f  cruise projection divisor
// = 0xc1a00000 = -20.0f   "formation has any cohesion" gate
// = 0x41a00000 = 20.0f    squared-blend / tightness threshold
// = 0x3c23d70a = 0.01f    blend floor-to-zero epsilon
// = 0x00000000 = 0.0f     the family's null-receiver fallback
// = 0x3fe0000000000000 = 0.5  (double) speed/angular floor
// = 0x3fc999999999999a = 0.2  (double) wide-angle scale
// = 0x3fefd70a3d70a3d7 = 0.99 (double) alignment threshold
// The three doubles are kept as double literals rather than float ones: baseline
// really does widen the float operand and compare/multiply in the wider type,
// and 0.2 and 0.99 are not representable in float.
// ============================================================================

namespace
{
    // The receiver's +0x08 back-pointer to its owning SystemManager, then that
    // SystemManager's own +0x08 BaseWatcher, then the standard `-8` Watchable
    // downcast. baseline re-walks this chain from scratch at all seven sites that
    // need the owner (module storage, 062ebfa4, 062ebfcb, 062ec010, 062ec37d,
    // , 062ec494), so it is a function here rather than a hoisted
    // local. baseline's second guard at module storage-062ebf6e (`ADD EAX,-8;
    // TEST EAX,EAX; JZ`) can only fire when the Watchable pointer is literally
    // 8; ObjRwFromWatchable's null return covers the reachable case, and the
    // two checks are collapsed here exactly as item 1856's port collapsed the
    // identical triple-check.
    IObjRW* OwnerObjOf(SystemManager* Sys)
    {
        return ObjRwFromWatchable(*reinterpret_cast<void**>(reinterpret_cast<unsigned char*>(Sys) + 0x8));
    }

    // , repeated verbatim at module storage-062ec17b.
    // SystemManager's once-per-tick cache of
    // `ExternalIface->get_physical_radius(float& Radius, Vector& Center)`
    // (IObjInspect slot +0x2C), latched by Flags1578[2] (== +0x157a, spelled
    // `[Sys+0x14fc + 0x7e]` in the listing). Argument order is (Radius,
    // Center): baseline pushes `&[Elem+0xc]` first and `&[Elem+0x18]` second, so
    // Elem+0x18 == SystemManager+0x1514 == CachedPhysicalRadius is the FIRST
    // argument -- the same ordering common_aisteer.cpp's
    // RefreshCachedIfacePosition had to be corrected to on 2026-09-16.

    // baseline sets the latch and then dereferences ExternalIface with no null
    // check; both are reproduced as written.
    void RefreshCachedPhysicalExtents(SystemManager* Sys)
    {
        if (Sys->SelfTarget.bPosValid != 0)
            return;
        Sys->SelfTarget.bPosValid = 1;
        Sys->SelfTarget.Iface
            ->get_physical_radius(Sys->SelfTarget.CachedRadius, Sys->SelfTarget.CachedPos);
    }
}

bool FGunConvergenceStateBlock::ComputeFormationFollowerEnvelope(FormationFollowerEnvelope* Out)
{

    SystemManager* const Sys = Owner;
    if (OwnerObjOf(Sys) == nullptr)
        return false;

    // . Slot +0x60 is the one-argument
    // is_cruise_active(bool&) overload, not the (bool&,bool&) one at +0x5C.
    bool bCruise = false;
    ObjInspectOf(OwnerObjOf(Sys))->is_cruise_active(bCruise);

    // B+0x1c is pre-seeded with 0x7f7fffff (FLT_MAX) so the
    // per-follower minimum below has a sentinel; it is NOT a float constant
    // from .rdata, it is an immediate store.
    float MinFollowerAngularSpeed = 3.402823466e+38f;

    // . The two arms differ only in the selector they
    // pass and latch into B+0x14: 1 == ThrustEquipEngine when not cruising,
    // 4 == ThrustEquipCruise when cruising (common_cequip.h's IObject enum).
    ThrustEquipType const ThrustType = bCruise ? ThrustEquipCruise : ThrustEquipEngine;
    float MinFollowerSpeed =
        Behavior_InvokeManagerSlotD8(ObjInspectOf(OwnerObjOf(Sys)), 1.0f, ThrustType); // , item 1762

    // . baseline populates four of the five out fields
    // HERE, before the follower-count early-out below -- so a `false` return
    // from the count check still leaves the caller with this partial result.
    // That ordering is load-bearing and is reproduced exactly.
    Out->Speed = MinFollowerSpeed;
    Out->Throttle = 1.0f;
    Out->AngularThrottle = 1.0f;
    Out->Flag10 = 1;

    // . `cobject()` is IObjInspect slot +0x150 (84).
    // The 0x503 mask is this codebase's established "is really a CShip" test
    // (common_iobjinspectimpl.cpp's own bitmask table). baseline then calls
    // CShip::get_formation_followers NON-virtually with whatever EAX holds --
    // 0 on a mask miss (module storage) -- and the port does the same.
    CObject const* const Obj = ObjInspectOf(OwnerObjOf(Sys))->cobject();
    CShip* const Ship = (Obj != nullptr && (static_cast<unsigned>(Obj->ObjectClass) & 0x503u) == 0x503u)
        ? static_cast<CShip*>(const_cast<CObject*>(Obj))
        : nullptr;

    // +0x1c..+0x9c is the receiver's own embedded 32-entry scratch array -- the same one
    // item 1856's port fills.
    IObjRW** const Followers = FollowerScratch;
    unsigned const Count = static_cast<unsigned>(Ship->get_formation_followers(Followers, 0x20));

    // `TEST ESI,ESI; JBE` -- after a TEST, CF is
    // always clear, so JBE degenerates to JZ. Only an exactly-zero count is
    // rejected, which is why the count is modelled unsigned here (the same
    // baseline quirk item 1856's port preserved).
    if (Count == 0)
        return false;


    RefreshCachedPhysicalExtents(Sys);

    // . Slot +0x0C is get_transform(); baseline calls it
    // ONCE here and keeps the returned pointer in B+0x40 for the whole loop,
    // re-reading through it each iteration. Reproduced as a hoisted pointer,
    // not as a copy, so a mutation mid-loop would be seen the same way.
    IObjInspect const* const Ext = Sys->SelfTarget.Iface;
    Transform const* const HoistedXform = &Ext->get_transform();

    // . the routine (item 1772).
    Vector OwnerFacing = { 0.0f, 0.0f, 0.0f };
    GetNegatedIfaceDirection(&OwnerFacing, OwnerObjOf(Sys));

    // . -10000.0f (0xc61c4000) twice, 1.0f once.
    float MaxProjection = -10000.0f;
    float MaxDistance = -10000.0f;
    float MinAlignCos = 1.0f;


    for (unsigned Index = 0; Index < Count; ++Index)
    {
        IObjRW* const Follower = Followers[Index];
        if (Follower == nullptr)
            continue; // jumps straight to the increment

        // . Slot +0xD8 == get_speed(Throttle, Type);
        // the pushes are (0x3f800000, selector), so Throttle is the 1.0f.
        // `FCOM; TEST AH,0x5; JP` keeps the smaller: a running MINIMUM seeded
        // from the owner's own speed above.
        float const FollowerSpeed = ObjInspectOf(Follower)->get_speed(1.0f, ThrustType);
        if (FollowerSpeed < MinFollowerSpeed)
            MinFollowerSpeed = FollowerSpeed;

        // . Slot +0xF0 == get_angular_speed_XY(Speed,
        // MaxSpeed), inlined here rather than routed through the routine
        // (baseline devirtualises it because the null case cannot happen inside
        // the loop). Pushes are (0x0, 0x3f800000) -> (1.0f, 0.0f). Another
        // running minimum, against the FLT_MAX sentinel.
        float const FollowerAngularSpeed = ObjInspectOf(Follower)->get_angular_speed_XY(1.0f, 0.0f);
        if (FollowerAngularSpeed < MinFollowerAngularSpeed)
            MinFollowerAngularSpeed = FollowerAngularSpeed;

        // . Slot +0xAC == get_formation_offset(Vector&)
        // -- the follower's OWN desired station, in the leader's local frame.
        Vector FormationOffset = { 0.0f, 0.0f, 0.0f };
        ObjInspectOf(Follower)->get_formation_offset(FormationOffset);

        // the same cache refresh again, per follower.
        RefreshCachedPhysicalExtents(Sys);

        // . the field / the routine is the shared
        // 3DMathEngine singleton (Acquire3DMathEngine, common_csys.h); slot
        // +0x2C is TransformMatrix(FVector& Dst, FMatrix const&, FVector const&)
        // per src/include/x86math.h. baseline hands it the Transform pointer
        // where an FMatrix is expected, which is exactly "rotate by the
        // orientation, ignore the translation" -- a Transform opens with its
        // Matrix. The `this` pointer is pushed as the first stack argument
        // (PUSH EAX last), the NEURONCOMPONENTS calling convention.
        Vector RotatedOffset;
        Acquire3DMathEngine()->TransformMatrix(reinterpret_cast<FVector&>(RotatedOffset),
                                               reinterpret_cast<FMatrix const&>(HoistedXform->Rotation),
                                               reinterpret_cast<FVector const&>(FormationOffset));

        // . The three FADDs are against
        // [SteerElem+0xc/+0x10/+0x14] == SystemManager+0x1508..+0x1510 ==
        // CachedPhysicalCenter, the pair the refresh above maintains.
        Vector const DesiredPos = { RotatedOffset.x + Sys->SelfTarget.CachedPos.x,
                                    RotatedOffset.y + Sys->SelfTarget.CachedPos.y,
                                    RotatedOffset.z + Sys->SelfTarget.CachedPos.z };

        // . the routine (item 1771) returns its own
        // out-pointer in EAX, which is what the three FSUBs dereference.
        Vector FollowerPos = { 0.0f, 0.0f, 0.0f };
        GetIfacePositionOrZero(&FollowerPos, Follower);
        Vector const Delta = { DesiredPos.x - FollowerPos.x,
                               DesiredPos.y - FollowerPos.y,
                               DesiredPos.z - FollowerPos.z };

        // . A SECOND, fresh get_transform() call (baseline
        // does not reuse B+0x40 here), then the NEGATED third column
        // [+0x08, +0x14, +0x20] == E[0][2], E[1][2], E[2][2] -- the owner's
        // backward axis. Three separate FCHS instructions, one per component.
        Matrix const& R = Ext->get_transform().Rotation;
        Vector const OwnerBack = { -R.E[0][2], -R.E[1][2], -R.E[2][2] };

        // . the routine (item 1728) == VecNormalize3,
        // whose port spells its parameters (Src, Out) where baseline pushes
        // (Out, Src); the routine (item 1727) == ClampedDot3. baseline leaves
        // &OwnerBack on the stack from the normalize call and reuses that slot
        // as ClampedDot3's second argument -- the `ADD ESP,0x8` at
        // cleans only two of the three pushed dwords, which is
        // the proof that VecNormalize3 takes two arguments and not three.
        Vector DeltaDir;
        VecNormalize3(&Delta, &DeltaDir);
        double const Cosine = ClampedDot3(DeltaDir, OwnerBack);

        // (z*z + y*y) + x*x, FSQRT, FST to [B+0x18] (the
        // rounded distance), FMULP with the unrounded cosine -- the projection
        // stays on the stack.
        double const Distance = std::sqrt((static_cast<double>(Delta.z) * Delta.z +
                                           static_cast<double>(Delta.y) * Delta.y) +
                                          static_cast<double>(Delta.x) * Delta.x);
        float const DistanceRounded = static_cast<float>(Distance);
        double const Projection = Cosine * Distance;

        // FCOM the unrounded projection; `TEST AH,0x41;
        // JNZ` skips the store unless strictly greater -- a running MAXIMUM of the
        // signed longitudinal offset (positive == the follower is behind its station).
        if (Projection > static_cast<double>(MaxProjection))
            MaxProjection = static_cast<float>(Projection);

        // , same polarity, on the rounded distance.
        if (DistanceRounded > MaxDistance)
            MaxDistance = DistanceRounded;

        // . the routine again, this time on the
        // follower, then the routine(OwnerFacing, FollowerFacing) FCOM'd unrounded
        // (`TEST AH,0x5; JP`): a running MINIMUM of how well the formation is aligned.
        Vector FollowerFacing;
        GetNegatedIfaceDirection(&FollowerFacing, Follower);
        double const AlignCos = ClampedDot3(OwnerFacing, FollowerFacing);
        if (AlignCos < static_cast<double>(MinAlignCos))
            MinAlignCos = static_cast<float>(AlignCos);
    }

    // B+0x3c is written every iteration and never read again anywhere in the
    // function -- baseline computes the widest follower distance and throws it
    // away. Kept (and explicitly discarded) rather than deleted, because
    // dropping it would hide a real, if dead, baseline computation.
    (void)MaxDistance;

    // . The divisor is picked off the SAME cruise flag
    // sampled at the top, not re-queried.
    float const ProjectionDivisor = bCruise ? 1000.0f : 300.0f; // / the field

    // `FCOMP the field; TEST AH,0x41; JNZ the branch` -- the fallback
    // path is taken when MaxProjection <= -20.0f (or unordered), i.e. when
    // every follower is already well AHEAD of its station and there is nothing
    // for the leader to wait for.
    if (MaxProjection > -20.0f)
    {
        // . The blend stays on the x87 stack throughout.
        double Blend = static_cast<double>(MaxProjection) / ProjectionDivisor;
        // `FCOMP the field; TEST AH,0x5; JP the branch` -- the SQUARING
        // arm is the fall-through, so it runs only while MaxProjection is
        // below 20.0f; at or above it the linear ratio is used as-is.
        if (MaxProjection < 20.0f)
        {
            Blend = Blend * Blend;
            // `FCOM the field; TEST AH,0x5; JP` -- anything under 0.01f is
            // flattened to 0.0f (FSTP ST0 / FLD the field).
            if (Blend < static_cast<double>(0.01f))
                Blend = 0.0;
        }

        // . `FMUL; FSUBR` against the same slot, so this
        // is Speed - Blend*Speed, FSTP'd to [B+0x18]; then floored at half the
        // speed (`FMUL double 0.5`, FLD [B+0x18], FCOMP, `TEST AH,0x5; JP`: only a
        // rounded target below the unrounded floor takes the floor).
        float TargetSpeed = static_cast<float>(static_cast<double>(MinFollowerSpeed) -
                                               Blend * MinFollowerSpeed);
        double const SpeedFloor = static_cast<double>(MinFollowerSpeed) * 0.5;
        if (static_cast<double>(TargetSpeed) < SpeedFloor)
            TargetSpeed = static_cast<float>(SpeedFloor);
        Out->Speed = TargetSpeed;

        // . the routine (item 1765) == slot +0xD4 ==
        // get_projected_throttle(Speed, Type).
        Out->Throttle =
            Behavior_InvokeManagerSlotD4(ObjInspectOf(OwnerObjOf(Sys)), TargetSpeed, ThrustType);

        // . _CIacos on the worst alignment cosine; FST keeps
        // a rounded copy in [B+0x14] while the first compare uses the unrounded one.
        double const AngleExact = std::acos(static_cast<double>(MinAlignCos));
        float const Angle = static_cast<float>(AngleExact);

        // +0x9c / +0xa0 are the receiver's own two angle thresholds; the ctor (item 1853)
        // seeds them with 0.1f and 0.2f radians.
        float const NarrowAngle = FormationNarrowAngle;
        float const WideAngle = FormationWideAngle;

        float AngularTarget;
        if (AngleExact < static_cast<double>(NarrowAngle))
        {
            // the fall-through of `TEST AH,0x5; JP`,
            // i.e. strictly-below. Stores the 0x7f7fffff sentinel, which the
            // clamp below immediately turns into "no angular limit at all".
            AngularTarget = 3.402823466e+38f;
        }
        else if (Angle < WideAngle)
        {
            // . (WideAngle - Angle) * (MinAngular /
            // WideAngle), floored at half the minimum angular speed by the same
            // `FMUL double 0.5` + keep-larger shape used for the speed above.
            // NOTE the floor is not decorative and not a transcription slip:
            // this ramp PEAKS at exactly half the minimum (at Angle ==
            // NarrowAngle, with baseline's own 0.1/0.2 seeds) and falls from
            // there, so with those defaults the whole band collapses to the
            // constant floor. Reproduced literally; the thresholds are fields,
            // not constants, and other seeds make the ramp live.
            // On the stack: (Wide - rounded Angle) * (MinAngular / Wide), FSTP'd.
            AngularTarget = static_cast<float>(
                (static_cast<double>(WideAngle) - Angle) *
                (static_cast<double>(MinFollowerAngularSpeed) / WideAngle));
            double const AngularFloor = static_cast<double>(MinFollowerAngularSpeed) * 0.5;
            if (static_cast<double>(AngularTarget) < AngularFloor)
                AngularTarget = static_cast<float>(AngularFloor);
        }
        else
        {
            // `FMUL double 0.2` (the field).
            AngularTarget = static_cast<float>(static_cast<double>(MinFollowerAngularSpeed) * 0.2);
        }

        // `TEST AH,0x41; JNZ` skips the store unless
        // strictly greater, i.e. clamp DOWN to the slowest follower's turn rate.
        if (AngularTarget > MinFollowerAngularSpeed)
            AngularTarget = MinFollowerAngularSpeed;
        Out->AngularSpeed = AngularTarget;

        // . the routine (item 1766) == slot +0xF8.
        Out->AngularThrottle = Behavior_InvokeManagerSlotF8(ObjInspectOf(OwnerObjOf(Sys)), AngularTarget);
    }
    else
    {
        // -module storage. Nothing to hold back for: run the
        // owner's own full-throttle speed and turn rate straight into the
        // envelope. ESI is loaded with 0x3f800000 once and reused as both the
        // 1.0f literals AND the two stored 1.0f fields.
        Out->Speed = Behavior_InvokeManagerSlotD8(ObjInspectOf(OwnerObjOf(Sys)), 1.0f, ThrustType);
        Out->Throttle = 1.0f;
        // the routine (item 1763) for real, with
        // BOTH arguments 1.0f -- unlike the in-loop site, which passes
        // (1.0f, 0.0f).
        Out->AngularSpeed = Behavior_InvokeManagerSlotF0(ObjInspectOf(OwnerObjOf(Sys)), 1.0f, 1.0f);
        Out->AngularThrottle = 1.0f;
    }

    // . While cruising the flag keeps the 1 stored back
    // at module storage and nothing else is tested. Otherwise it becomes
    // "the formation is tight": `FCOMP the field; TEST AH,0x5; JP` demands
    // MaxProjection strictly below 20.0f, and `FCOMP the field; TEST
    // AH,0x41; JNZ` demands MinAlignCos strictly above 0.99. Either failing --
    // including either being NaN -- writes 0.
    if (!bCruise)
        Out->Flag10 = (MaxProjection < 20.0f && static_cast<double>(MinAlignCos) > 0.99) ? 1 : 0;

    // Both non-early-out tails (module storage / module storage) return AL = 1;
    // only the guard at the top and the zero-follower check return 0
    // (the branch).
    return true;
}

// ============================================================================
// MOVED HERE 2026-09-17 (claude:w16-behavior). Everything from this banner to the end
// of the file used to sit in src/Common/common_stubs.cpp, lines 8412-10865, as one
// contiguous GotoBehavior cluster: the vtable slot bodies (the routine / the routine /
// / the routine / the branch / the branch / the branch / the branch /
// CopyActiveGotoOperation / the routine), GotoBehavior::Update (the branch, item 1996),
// GotoBehavior::EnterState (the branch, item 2000), and the twelve file-static helpers
// they share (GotoMoveIdea/GotoFaceIdea/GotoSplineMoveIdea, the four FollowSibling_* and
// the six Follower* ones).

// The split was verified to be exact before it was made, in both directions, by scanning
// the range's identifiers against common_stubs.cpp's own out-of-range definitions:
//   * the ONLY symbol this cluster needed from the rest of common_stubs.cpp was the
//     file-static IsIdAmongOwnerFormationFollowers (the routine), which is now non-static
//     and lived in common_behavior.cpp (declared in common_behavior.h; since CMN-GC an
//     FGunConvergenceStateBlock member in common_gunconvergence.cpp) -- its real home,
//     since both its call sites load the same SystemManager+0x1364 receiver and it is a
//     member of neither behavior class;
//   * NOTHING outside the range referenced anything defined inside it.
// Only one new include was needed here: <cstring> for the std::memcpy state-block copies
// (common_behavior.h already arrives through common_gotobehavior.h). Not a single line of the bodies
// below was edited by the move -- they are byte-identical to what common_stubs.cpp held,
// so every "Common NNNNNN" citation in them still stands unaltered.

// TODO (out of the moving lane's scope): the extern "C" free functions below (no longer
// exported -- baseline exports none of them) are GotoBehavior vtable slots that still carry their raw LAB_/FUN_ names
// and `unsigned char* param_1` receivers. Now that they share a translation unit with the
// real class they should become real GotoBehavior members, the way the branch already
// became GotoBehavior::Update().
// ----------------------------------------------------------------------------

// Predecessor cluster to the branch -- Common/.txt 0x062fd780-0x062fda59,
// implementation tracker items 1989/1990/1991/1992/1993/1994.

// OWNING CLASS AND ITS THREE SUB-OBJECTS -- RESOLVED 2026-09-17, ABI pass 50
// (claude:abi-pass50). Everything below used to be written as "the concrete
// owning class is still not independently named/RTTI-confirmed ... most likely
// FollowBehavior", with the +0x2c sub-object called FormationMoveFx and the
// +0xa4/+0x164 ones left raw. All four of those claims were wrong, and the
// correction comes from one constructor, read straight out of the raw listing:

// (Common) is GotoBehavior's constructor
//   -- it is the function that installs .rdata:the routine at
//   `module storage MOV dword ptr [ESI],.rdata:the routine`, and every
//   item in this cluster is a slot of THAT table (1991 = 063a2288/slot 1,
//   1992 = 063a2290/slot 3, 1993 = 063a2294/slot 4, 1994 = 063a2298/slot 5).
//   The table has a real RTTI COL at [-4] (063a2280 -> 063a9f94); it is a
//   genuine C++ vtable, not one of the "0639f5dc-ish/063a2284-ish
//   function-pointer table" dead ends this file's older comments describe.
//   GotoBehavior is already fully modelled in common_gotobehavior.h.

//   The same constructor names all three embedded sub-objects by calling their
//   own constructors with an explicit LEA:
// LEA ECX,[ESI+0x2c];   CALL the routine  -> MoveIdea       (0x78, ends +0xa4)
// LEA ECX,[ESI+0xa4];   CALL the routine  -> FaceIdea       (0xc0, ends +0x164)
// LEA ECX,[ESI+0x164];  CALL the routine  -> SplineMoveIdea (0x174, ends +0x2d8)
//   All three are RTTI-identified BaseIdea siblings (common_idea.h); the sizes
//   chain exactly, and the next field the routine writes is +0x2d8.

//   CORRECTION, stated rather than silently overwritten: the +0x2c sub-object
//   is NOT FormationMoveFx. FormationMoveFx (this port's invented name for the
//   class whose ctor is the routine / vtable module storage, baseline RTTI name
//   MatchThrottle2Idea) is a DIFFERENT BaseIdea sibling; the routine has
//   exactly two call sites in the whole module (062ca381 and 062cb2ec) and
//   neither is this class. The reason the older reading looked convincing is
//   that every "evidence" call it cited is an /OPT:ICF fold of a one-line
//   setter shared by both siblings:
// = `MOV byte [ECX+0x58],AL; RET 4`  (MoveIdea::Field58)
// = `MOV dword [ECX+0x44],EAX; RET 4` (MoveIdea::Field44)
//     TextRenderContext::set_origin_h = `MOV [ECX+0x38],EAX; RET 4`
//   Identical code, so the linker gave them one address; that says nothing
//   about which class the receiver belongs to. Only the constructor does.

// The three sub-objects' dispatch sites in this cluster are now typed calls on
// MoveIdea/FaceIdea/SplineMoveIdea instead of `Vtable[N]` indexing. Slot 0 is
// BaseIdea::SetContext(Object, Handle, Context) and slot 0x10 is
// BaseIdea::Commit() in all three, so the old raw sites were at least reading
// in-range slots -- but see item 1991 below for an argument-order defect the
// typing exposed.

// These remain free functions taking `unsigned char* param_1` rather than
// GotoBehavior methods: GotoBehavior's own class (common_gotobehavior.h)
// deliberately does not reproduce vtable slots 3/4/5 (its GAP:AI-GOTO-STATE-
// MACHINE note), and moving them is a separate change. The sub-object casts
// below are exact, though -- offsets straight from GotoBehavior's own
// static_asserts.
// ============================================================================

// Common 0x062fd3e0 (the routine) is implementation tracker item 1984 -- see
// common_archetype.h: that one is UNRELATED to this class (it is ArchDB's own
// per-node value destructor, called only from item 1942). Two of THIS
// cluster's own items reuse the exact same "destroy, then maybe free(this)"
// wrapper SHAPE for a real owned game object instead -- traced individually,
// not assumed identical just because the shape matches.

// Slot 1, the branch (module storage-062fd910, RET 0xc). BL = the routine(Owner, Manager,
// Sys); only when it succeeds: Flag367 = 1, then each Idea's slot 0 with (+0x14, +0x24,
// Manager) -- MoveIdea, MoveIdea::SetEnabled(Flag367) (the routine), FaceIdea,
// SplineMoveIdea. Returns BL. (The old free function hard-wired the gate to false.)
bool GotoBehavior::SetContext(void* Owner, void* Manager, void* Sys)
{
    bool const bBound = BindContext(Owner, Manager, Sys);
    if (bBound)
    {
        Flag367 = 1;
        SubObjectA.SetContext(OwnerContextPtr14, NavHelperPtr24, Manager);
        SubObjectA.SetEnabled(Flag367);                             // (raw byte)
        SubObjectB.SetContext(OwnerContextPtr14, NavHelperPtr24, Manager);
        SubObjectC.SetContext(OwnerContextPtr14, NavHelperPtr24, Manager);
    }
    return bBound;
}

// Slot 3, the routine (module storage-062fd9be).
void GotoBehavior::Slot3()
{
    Behavior::Slot3();                                              // the routine
    Flag369 = 0;
    // OwnerContext->is_cruise_active(Flag368) (+0x60). baseline does not
    // clear Flag368 first (the old port did).
    int const CruiseResult =
        ObjInspectOf(OwnerContextPtr14)->is_cruise_active(reinterpret_cast<bool&>(Flag368));
    // mode 2 only when the flag came back set AND the call returned 0.
    SubObjectA.SetMode((Flag368 != 0 && CruiseResult == 0) ? 2 : 0);
    Flag365 = 1;
    Flag36c = 1;
    SubObjectA.SetAngle38(3.14159274f);                             // , 0x40490fdb
    Field2e4 = Field360;
    Flag2ea = 0;
    Flag2ec = 0;
    SetCruiseLock(1, 3);                                            // the routine
    // SystemManager::ExternalIface (+0x14fc+0x84) is_player() -> this
    // object's own slot 14 (vtable +0x38) with true.
    SystemManager* const Sys = static_cast<SystemManager*>(NavHelperPtr24);
    if (Sys->SelfTarget.Iface->is_player())
        set_cruise(true);
}

// Slot 4, the routine (module storage-062fda1b).
void GotoBehavior::Slot4()
{
    SubObjectB.Commit();                                            // vtbl+0x10
    // ExternalIface->is_player() and goto kind 0 -> MoveIdea::SetField44(2)
    // (the routine).
    SystemManager* const Sys = static_cast<SystemManager*>(NavHelperPtr24);
    if (Sys->SelfTarget.Iface->is_player() && Field2f4 == 0)
        SubObjectA.SetField44(2);
    SubObjectA.Commit();                                            // vtbl+0x10
    // the routine(NavHelper + 0x18, 0). The old port dropped this call.
    ((SystemManager*)NavHelperPtr24)->Thrust.SetStrafeDir(StrafeNone);
    Behavior::Slot4();                                              // JMP the routine
}

// Slot 5, the branch (module storage-062fda52, RET 4, argument unread). EAX = 2; Flags28
// bit 0x2 set -> (Flag29 ? 3 : 1), clear -> (Flag29 ? 3 : 2); stored to +0xc and returned
// (the old free function returned nothing).
int GotoBehavior::Slot5(float Value)
{
    (void)Value;
    int Result = 2;
    if ((Flags28 & 0x2) != 0)
        Result = (Flag29 != 0) ? 3 : 1;
    else if (Flag29 != 0)
        Result = 3;
    ModeSelector0xc = Result;
    return Result;
}

// Slot 6, the branch (module storage-062fdb74, RET 4, argument unread).
EvaluateResult GotoBehavior::evaluate(unsigned int Context)
{
    (void)Context;
    IObjInspect const* const Owner = ObjInspectOf(OwnerContextPtr14);
    IObjRW* Target = nullptr;
    Owner->get_target(Target);                                      // +0x74
    if (Target == nullptr)
        return EvalBlocked;                                         // -> the branch (1)

    // is_using_tradelane (+0x134) on the owner, then on the target, into
    // ONE byte baseline zeroes only before the first call.
    bool bInLane = false;
    Owner->is_using_tradelane(&bInLane);
    if (bInLane)
        return EvalSkip;                                            // 2
    Target->is_using_tradelane(&bInLane);
    if (bInLane)
        return EvalSkip;

    // get_type (+0x88). Only a ZERO return with a 0x7f0000 type bit reaches
    // get_behavior_id (+0x148); ids 0xe / 0x11 / 0x8 -> 2. (The old port took the id branch on
    // a NONZERO return too: `JNZ the branch` skips it.)
    unsigned int Type = 0;
    int const TypeResult = Target->get_type(Type);
    if (TypeResult == 0 && (Type & 0x7f0000) != 0)
    {
        int const BehaviorId = Target->get_behavior_id();
        if (BehaviorId == 0xe || BehaviorId == 0x11 || BehaviorId == 0x8)
            return EvalSkip;
    }
    if ((Type & 0x10000000) != 0)
        return EvalBlocked;                                         // -> 1

    // |velocity|^2 on the FPU stack, ((x*x + y*y) + z*z), FCOMP float 25.0f
    // (the field); TEST AH,0x41 + JZ -> strictly greater keeps 2, otherwise 1.
    Vector const Velocity = Target->get_velocity();                 // +0x04
    double const SpeedSq = static_cast<double>(Velocity.x) * Velocity.x
                         + static_cast<double>(Velocity.y) * Velocity.y
                         + static_cast<double>(Velocity.z) * Velocity.z;
    return (SpeedSq > 25.0) ? EvalSkip : EvalBlocked;
}

// Slot 21, the routine (module storage-062c6da5, RET 4): drop Watcher310 and then Watcher370
// when their object's get_id() (+0x20) equals the argument.
void GotoBehavior::Slot21(void* Arg)
{
    unsigned int const LeftId = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(Arg));
    IObjRW* const Dest = ObjRwFromWatchable(const_cast<Watchable*>(Watcher310.target()));
    if (Dest != nullptr && Dest->get_id() == LeftId)
        Watcher310.set(nullptr);
    IObjRW* const Prox = ObjRwFromWatchable(const_cast<Watchable*>(Watcher370.target()));
    if (Prox != nullptr && Prox->get_id() == LeftId)
        Watcher370.set(nullptr);
}

// Slot 24, the branch (module storage-062c6dc0): Watcher310's object (target - 8) or null.
IObjRW const* GotoBehavior::get_target()
{
    return ObjRwFromWatchable(const_cast<Watchable*>(Watcher310.target()));
}

// ============================================================================
// GotoBehavior::Update -- vtable slot 7, the branch (Common, 796
// instructions plus the the branch jump table). Re-ported 2026-10-08 (cmn-ai7) from the
// listing onto the modelled fields; the previous body (raw `param_1` offsets) dropped the
// / the routine results the later compares read, invented a 0.0f "no prior
// write" placeholder for them, built case 0's direction with a 0.0f x, and returned 2 instead
// of 3 on case 0's velocity exit.

// Frame: `SUB ESP,0x54` + EBX/EBP/ESI/EDI, E = ESP after the pushes:
//   E+0x11 byte  bCruise        -- the "fly in cruise mode" decision of the 0/1 tail
//   E+0x12 byte  bTargetCruise  -- the watched object's is_cruise_active
//   E+0x13 byte  owner is_cruise_active out-param (never read back)
//   E+0x14 int   Result         -- EBP = 3 at entry
//   E+0x18 float Throttle       -- Field360, overridden from Field2e4 / 1.0f for the player
//   E+0x1c float Clearance      -- 0 at entry; case 1 / case 0 store the the routine /
// result here, the tail stores the
// speed cap here.
// Both `switch` statements dispatch on Field2f4 (0 watch an object, 1 a point, 2 a spline).
// ============================================================================

namespace
{
    // `raw = [W]; raw ? raw - 8 : 0` -- the IObjRW behind a BaseWatcher. baseline's tests of the
    // result (`ADD EAX,-8; TEST EAX,EAX`) also treat a raw value of 8 as "nothing watched",
    // which ObjRwFromWatchable's null-or-minus-8 reproduces.
    IObjRW* GotoWatchedObject(BaseWatcher const& Watcher)
    {
        return ObjRwFromWatchable(const_cast<Watchable*>(Watcher.target()));
    }

    // The negated third column of a frame's rotation: FLD [+8] / [+0x14] / [+0x20], each FCHS'd
    // and `FSTP float` (module storage-062fdcf4 and module storage-062fe035).
    Vector NegatedColumn2(Matrix const& Rotation)
    {
        return Vector{ -Rotation.E[0][2], -Rotation.E[1][2], -Rotation.E[2][2] };
    }

    // (x*x + y*y) + z*z on the x87 stack: FLD [+8], FLD [+4], FLD [+0]; FLD ST0 / FMUL ST1;
    // FLD ST2 / FMUL ST3; FADDP; FLD ST3 / FMUL ST4; FADDP (module storage-062fdd75 and
    // ). Never stored, so it stays double.
    double LengthSquaredXyz(Vector const& V)
    {
        return (static_cast<double>(V.x) * V.x + static_cast<double>(V.y) * V.y) +
               static_cast<double>(V.z) * V.z;
    }
}

int GotoBehavior::Update()
{
    // a pending disengage request finishes the tick with 2.
    if ((Flags28 & 0x1) != 0)
    {
        UpdateResult10 = 2;
        return 2;
    }

    // -> the branch: no owner, or not engaged, finishes with 3.
    if (OwnerContextPtr14 == nullptr || (Flags28 & 0x2) == 0)
    {
        UpdateResult10 = 3;
        return 3;
    }

    IObjInspect const* const Owner = ObjInspectOf(OwnerContextPtr14);
    SystemManager* const Sys = static_cast<SystemManager*>(NavHelperPtr24);
    FAiTarget* const Elem = &Sys->SelfTarget;

    // Owner->is_cruise_active (slot +0x60) into E+0x13, which nothing reads.
    bool bOwnerCruise = false;
    int Result = 3;   // E+0x14, EBP
    Owner->is_cruise_active(bOwnerCruise);

    // the steering element's PosValid refill (flag set before the call).
    if (Elem->bPosValid == 0)
    {
        Elem->bPosValid = 1;
        Elem->Iface->get_physical_radius(Elem->CachedRadius, Elem->CachedPos);
    }

    // the lead point starts as our own cached position.
    Field2f8 = Elem->CachedPos;

    // Throttle = Field360; Clearance = 0; for the player ship Field2e4, or
    // 1.0f while Flag2e8 is set and Flag36b clear.
    float Throttle = Field360;
    float Clearance = 0.0f;
    if (Elem->Iface->is_player())
    {
        if (Flag2e8 != 0 && Flag36b == 0)
            Throttle = 1.0f;
        else
            Throttle = Field2e4;
    }

    // `SUB 0 / JZ`, `DEC / JZ`, `SUB 2 / JNZ`; value 3 stores EBP (3) into
    // Result, which already holds 3. Everything but 0 and 1 goes straight to the tail.
    if (Field2f4 == 1)
    {
        // Clearance = the routine(Elem, &Field304), `FSTP float`.
        Clearance = static_cast<float>(GetSignedRadialClearanceToPoint(Elem, Field304));

        if (Flag2ea != 0)
        {
            // Dir (E+0x20) = -column 2 of our own frame (slot +0x0c).
            Vector const Dir = NegatedColumn2(Elem->Iface->get_transform().Rotation);

            // PosValid refill again.
            if (Elem->bPosValid == 0)
            {
                Elem->bPosValid = 1;
                Elem->Iface->get_physical_radius(Elem->CachedRadius, Elem->CachedPos);
            }

            // Ahead (E+0x38) = CachedPos + Dir * 1750.0f (the field =
            // 00 c0 da 44). x and y round the product (`FSTP float` to E+0x2c / E+0x30) before
            // the FADD; z adds the unrounded product.
            float const ScaledX = Dir.x * 1750.0f;
            float const ScaledY = Dir.y * 1750.0f;
            Vector const Ahead{
                ScaledX + Elem->CachedPos.x,
                ScaledY + Elem->CachedPos.y,
                static_cast<float>(static_cast<double>(Dir.z) * 1750.0f + Elem->CachedPos.z) };

            // `FCOMP 0.0f; TEST AH,0x41; JNZ` -- a non-zero (strictly
            // positive) steering triple Field2d8 makes the FaceIdea face that axis
            // (the routine), otherwise it faces the point ahead (the routine).
            Vector const& Steer = *reinterpret_cast<Vector const*>(&Field2d8);
            if (LengthSquaredXyz(Steer) > 0.0)
                SubObjectB.SetAxis(Steer);
            else
                SubObjectB.SetPosition(Ahead);

            // the routine(2), the routine(&Dir) on the MoveIdea.
            SubObjectA.SetField44(2);
            SubObjectA.SetDirection(Dir);

            // `FLD Clearance; FCOMP Field358`, Flag366 = 1 stored between,
            // `TEST AH,0x5; JP the branch` -- only strictly-less falls through.
            Flag366 = 1;
            if (Clearance < Field358)
            {
                // arrived.
                Flags28 = static_cast<unsigned char>(Flags28 | 0x1);
                Flag368 = 0;
                Slot4();
                Flags28 = static_cast<unsigned char>(Flags28 & 0xfd);
                BehaviorMgrPtr20->SetDirectiveResult(2, 0);
                UpdateResult10 = 2;
                return 2;
            }
        }
        else
        {
            // FaceIdea faces the destination (the routine), MoveIdea moves to it
            // (the routine).
            SubObjectB.SetPosition(Field304);
            SubObjectA.SetPosition(Field304);
        }

        // the lead point is the destination.
        Field2f8 = Field304;
    }
    else if (Field2f4 == 0)
    {
        // .
        if (GotoWatchedObject(Watcher310) == nullptr)
        {
            // the watched object is gone.
            Flags28 = static_cast<unsigned char>(Flags28 | 0x1);
            Slot4();
            Flags28 = static_cast<unsigned char>(Flags28 & 0xfd);
            BehaviorMgrPtr20->SetDirectiveResult(2, 2);

            // for the player, IObjAffect slot +0x6c (3, 0) on the owner.
            if (Elem->Iface->is_player())
                ObjAffectOf(OwnerContextPtr14)->slot_6c(3, 0);

            // .
            Flags28 = static_cast<unsigned char>(Flags28 | 0x1);
            Flag368 = 0;
            Slot4();
            Flags28 = static_cast<unsigned char>(Flags28 & 0xfd);
            BehaviorMgrPtr20->SetDirectiveResult(2, 2);
            UpdateResult10 = Result;
            return Result;
        }

        // for the player, a watched object moving faster than 5 m/s ends the
        // goto. 062fdf10-062fdf43: get_velocity (slot +0x04, hidden return buffer E+0x44), its
        // (x*x+y*y)+z*z against 25.0f (the field = 00 00 c8 41), `TEST AH,0x41; JNZ` --
        // strictly greater falls through.
        if (Elem->Iface->is_player())
        {
            Vector const Velocity = GotoWatchedObject(Watcher310)->get_velocity();
            if (LengthSquaredXyz(Velocity) > 25.0)
            {
                ObjAffectOf(OwnerContextPtr14)->slot_6c(2, 0);
                Flags28 = static_cast<unsigned char>(Flags28 | 0x1);
                Flag368 = 0;
                Slot4();
                Flags28 = static_cast<unsigned char>(Flags28 & 0xfd);
                BehaviorMgrPtr20->SetDirectiveResult(2, 2);
                // `MOV [ESI+0x10],EBP; MOV EAX,EBP` -- 3.
                UpdateResult10 = Result;
                return Result;
            }
        }

        // the lead point is the watched object's position (the routine), and
        // Clearance = the routine(Elem, watched), `FSTP float`.
        Vector WatchedPos;
        GetIfacePositionOrZero(&WatchedPos, GotoWatchedObject(Watcher310));
        Field2f8 = WatchedPos;
        Clearance = static_cast<float>(GetInterceptClearanceOfIface(Elem, GotoWatchedObject(Watcher310)));

        if (Flag2ea != 0)
        {
            // Dir (E+0x38) = -column 2 of our own frame; the FaceIdea faces
            // the steering triple unconditionally (the routine), the MoveIdea flies along Dir.
            Vector const Dir = NegatedColumn2(Elem->Iface->get_transform().Rotation);
            SubObjectB.SetAxis(*reinterpret_cast<Vector const*>(&Field2d8));
            SubObjectA.SetDirection(Dir);

            // `FCOMP Field358; TEST AH,0x5; JP the branch`.
            if (Clearance < Field358)
            {
                Flags28 = static_cast<unsigned char>(Flags28 | 0x1);
                Flag368 = 0;
                Slot4();
                Flags28 = static_cast<unsigned char>(Flags28 & 0xfd);
                BehaviorMgrPtr20->SetDirectiveResult(2, 0);
                UpdateResult10 = 2;
                return 2;
            }
        }
        else
        {
            // both Ideas watch the object (the routine, the routine).
            SubObjectA.SetWatchObject(GotoWatchedObject(Watcher310));
            SubObjectB.SetWatchObject(GotoWatchedObject(Watcher310));
        }
    }

    // the routine / the routine on the proximity watcher at +0x370.
    unsigned char* const ProxWatcher = reinterpret_cast<unsigned char*>(&Watcher370);
    ProxWatcherRecomputeZoneFlags(ProxWatcher, NavHelperPtr24, Field2f8);
    ProxWatcherNotifyZoneChange(ProxWatcher, NavHelperPtr24, BehaviorMgrPtr20, Field2f8);

    if (Field2f4 == 2)
    {
        // the lead point is the spline's end point (Spline.P2 at +0x330);
        // (SplineMoveIdea::SetBankBlendCap) with Throttle.
        Field2f8 = Spline.P2;
        SubObjectC.SetBankBlendCap(Throttle);

        if (GotoWatchedObject(Watcher370) != nullptr)
        {
            // Clearance = the routine(Owner, Sys, 1, Field360) (`FSTP
            // float`); when Owner's get_speed(Throttle, engine) (the routine) is strictly
            // greater (`TEST AH,0x41; JNZ`), the routine with that cap.
            Clearance = static_cast<float>(
                ProxWatcherComputeSpeedCap(ProxWatcher, OwnerContextPtr14, NavHelperPtr24, 1, Field360));
            if (Behavior_InvokeManagerSlotD8(Owner, Throttle, ThrustEquipEngine) > Clearance)
                SubObjectC.SetThrottleScale(Clearance);
        }

        // SplineMoveIdea::Update (slot 1) with the tick delta.
        int const SplineResult = SubObjectC.Update(DeltaTime1c);
        if (SplineResult == 0)
        {
            // .
            Result = 1;
            UpdateResult10 = Result;
            return Result;
        }
        if (SplineResult != 1)
        {
            Result = 3;
        }
        else
        {
            // done with the spline -- keep going (1) while the watcher still
            // has an object, is outside zone 2 and Flag366 is clear, else the branch (2).
            if (GotoWatchedObject(Watcher370) != nullptr && (Flags384 & 0x2) == 0 && Flag366 == 0)
            {
                Result = 1;
                UpdateResult10 = Result;
                return Result;
            }
            Result = 2;
        }
    }
    else if (Field2f4 == 0 || Field2f4 == 1)
    {
        // the routine(Flag366 ? 2 : 0) -- `NEG DL; SBB EDX,EDX; AND EDX,2`.
        SubObjectA.SetField44(Flag366 != 0 ? 2 : 0);

        // once per goto (Flag365), Flag364 = Clearance > 1750.0f
        // (`TEST AH,0x41; JNZ` -> 0 on not-greater or unordered).
        if (Flag365 != 0)
        {
            Flag364 = (Clearance > 1750.0f) ? 1 : 0;
            Flag365 = 0;
        }

        // bCruise. For the player it is Flag364 only while Flag2e8 is set
        // (else Flag2e8 itself, i.e. 0); for AI ships Flag364. A non-zero value is then
        // confirmed by the pathfinder when there is one (the routine). Flag36a forces it on,
        // Flag36b forces it off.
        unsigned char bCruise;
        if (Elem->Iface->is_player())
        {
            bCruise = Flag2e8;
            if (Flag2e8 != 0)
                bCruise = Flag364;
        }
        else
        {
            bCruise = Flag364;
        }
        if (bCruise != 0 && BehaviorMgrPtr20->Pathfind != nullptr)
            bCruise = BehaviorMgrPtr20->Pathfind->IsCruiseUnobstructed() ? 1 : 0;
        if (Flag36a != 0)
            bCruise = 1;
        if (Flag36b != 0)
            bCruise = 0;

        // the formation envelope (the routine on Sys+0x1364, out-struct
        // E+0x50). baseline zeroes only its first four dwords; the flag byte is value-initialised
        // here as well (the callee writes it on every path that returns true).
        FormationFollowerEnvelope Envelope{};
        if (Sys->GunConvergenceStateBytes.ComputeFormationFollowerEnvelope(&Envelope))
        {
            // MoveIdea angle pi (`PUSH 0x40490fdb`), Throttle clamped down to
            // the envelope's (`TEST AH,0x5; JP` -> assign only when strictly less), MoveIdea
            // scale = Throttle.
            SubObjectA.SetAngle38(3.14159274f);
            if (Envelope.Throttle < Throttle)
                Throttle = Envelope.Throttle;
            SubObjectA.SetScale40(Throttle);

            // FaceIdea throttle scale from the envelope's angular speed
            // (the routine); a clear envelope flag cancels cruise.
            SubObjectB.SetThrottleScale(Envelope.AngularSpeed);
            if (Envelope.Flag10 == 0)
                bCruise = 0;
        }
        else
        {
            // the routine(1.0f) on the FaceIdea.
            SubObjectB.SetScale94(1.0f);
        }

        // bTargetCruise = the watched object's is_cruise_active (slot +0x60).
        unsigned char bTargetCruise = 0;
        if (GotoWatchedObject(Watcher370) != nullptr)
        {
            bool bWatchedCruise = false;
            GotoWatchedObject(Watcher370)->is_cruise_active(bWatchedCruise);
            bTargetCruise = bWatchedCruise ? 1 : 0;
        }

        // unless forced, a watched object that is not cruising and is not one of
        // our formation followers (the routine, the routine on Sys+0x1364) cancels cruise.
        if (Flag36a == 0 && GotoWatchedObject(Watcher370) != nullptr && bTargetCruise == 0)
        {
            if (!Sys->GunConvergenceStateBytes.IsIdAmongOwnerFormationFollowers(Watcher370.get_watched_id()))
                bCruise = 0;
        }

        // a change of decision switches the MoveIdea mode (the routine).
        if (bCruise != Flag368)
        {
            if (bCruise != 0)
            {
                SubObjectA.SetMode(2);
                Flag368 = 1;
            }
            else
            {
                // DeviceTRC::bind_to_dc fold == MoveIdea::SetRange3c.
                SubObjectA.SetRange3c(Field358);
                SubObjectA.SetMode(0);
                Flag368 = 0;
            }
        }

        // speed cap from the proximity watcher (the routine, selector 4 in
        // cruise else 1 -- `NEG AL; SBB EAX,EAX; AND EAX,3; INC EAX`), stored to Clearance.
        // When Owner's get_speed(Throttle, type) is strictly greater (`TEST AH,0x41; JNZ`),
        // Throttle = get_projected_throttle(cap, type) (the routine). The query type is
        // cruise only while both we and the watched object cruise.
        if (GotoWatchedObject(Watcher370) != nullptr)
        {
            Clearance = static_cast<float>(ProxWatcherComputeSpeedCap(
                ProxWatcher, OwnerContextPtr14, NavHelperPtr24, Flag368 != 0 ? 4 : 1, Field360));
            ThrustEquipType const QueryType =
                (Flag368 != 0 && bTargetCruise != 0) ? ThrustEquipCruise : ThrustEquipEngine;
            if (Behavior_InvokeManagerSlotD8(Owner, Throttle, QueryType) > Clearance)
                Throttle = Behavior_InvokeManagerSlotD4(Owner, Clearance, QueryType);
        }

        // FaceIdea::Update (slot 1) with the tick delta, kept in EBX.
        int const FaceResult = SubObjectB.Update(DeltaTime1c);

        // for the player, Flag2ec engages the cruise gate with -1.0f
        // (the routine) and Flag2e9 clear drops the registration flag (the routine) -- both
        // dispatch with index 0. Otherwise the MoveIdea gets the throttle (CAsteroidCube::
        // set_state fold == MoveIdea::SetScale40) and its own Update picks the index.
        unsigned int Index = 0;
        bool bRunMove = true;
        if (Elem->Iface->is_player())
        {
            if (Flag2ec != 0)
            {
                Sys->Thrust.SetThrottle(-1.0f, 0);
                bRunMove = false;
            }
            else if (Flag2e9 == 0)
            {
                Sys->Thrust.SetEngineActive(false);
                bRunMove = false;
            }
        }
        if (bRunMove)
        {
            // .
            SubObjectA.SetScale40(Throttle);
            Index = static_cast<unsigned int>(SubObjectA.Update(DeltaTime1c));
        }

        // `CMP EDI,3; JA the branch` (Result still 3), then the
        // table {062fe544, 062fe55b, 062fe5e2, 062fe5e2}.
        if (Index == 0)
        {
            // .
            Result = 1;
            UpdateResult10 = Result;
            return Result;
        }
        if (Index == 1)
        {
            // BL = (FaceResult == 1), forced on while Flag36c is clear. The player
            // keeps going (1) when neither cruise nor the face is pending.
            bool bFaceDone = (FaceResult == 1);
            if (Flag36c == 0)
                bFaceDone = true;
            if (Owner->is_player() && bCruise == 0 && !bFaceDone)
            {
                Result = 1;
                UpdateResult10 = Result;
                return Result;
            }

            // same keep-going test as the spline arm, else the branch (2).
            if (GotoWatchedObject(Watcher370) != nullptr && (Flags384 & 0x2) == 0 && Flag366 == 0)
            {
                Result = 1;
                UpdateResult10 = Result;
                return Result;
            }
            Result = 2;
        }
        else if (Index <= 3)
        {
            Result = 3;
        }
    }

    // finish -- request disengage, Slot4 (vtable +0x10), report to the manager:
    // SetDirectiveResult(2, 0) when Result is 2, (2, 2) otherwise.
    Flags28 = static_cast<unsigned char>(Flags28 | 0x1);
    Flag368 = 0;
    Slot4();
    Flags28 = static_cast<unsigned char>(Flags28 & 0xfd);
    if (Result == 2)
        BehaviorMgrPtr20->SetDirectiveResult(2, 0);
    else
        BehaviorMgrPtr20->SetDirectiveResult(2, 2);
    UpdateResult10 = Result;
    return Result;
}

// / the branch / the branch (tracker items 1997-1999) are internal labels of
// Update above -- jump-table targets of the branch sharing its frame -- not functions.

// (item 1798): the checked id lookup through the IObjDB at SystemManager+0x34,
// SystemManagerTimerTable::FindVerifiedObject (common_sysmgr_timertable.cpp).

// Slot 10, the branch (module storage-062fe938, RET 4): apply a pub::AI::DirectiveGotoOp.
// Re-ported 2026-10-08 (claude-code:cmn-ai5) from the listing onto the modelled fields; the
// previous body (EnterState, raw offsets) stubbed the routine to null, skipped
// / the routine / the routine, swapped the the field arms and routed
// / the routine into empty stubs.
bool GotoBehavior::Slot10(void* DirectiveOp)
{
    Flags28 = static_cast<unsigned char>(Flags28 & 0xfe);           // / 062fe691
    // REP MOVSD of 0x1c dwords, the whole op (vptr included) -> GotoOp.
    std::memcpy(static_cast<void*>(&GotoOp), DirectiveOp, sizeof(GotoOp));
    pub::AI::DirectiveGotoOp const& Op = *static_cast<pub::AI::DirectiveGotoOp const*>(DirectiveOp);
    SystemManagerTimerTable const* const ObjDb = &static_cast<SystemManager*>(NavHelperPtr24)->Timers;
    Field2f4 = Op.Field_0C;

    switch (Op.Field_0C)
    {
    case 0:
    {
        // watch the object Op.Field_1C names.
        IObjRW* const Dest = static_cast<IObjRW*>(
            ObjDb->FindVerifiedObject((unsigned const*)&GotoOp.Field_1C));
        Watcher310.set(Dest != nullptr ? static_cast<Watchable const*>(Dest) : nullptr);
        SubObjectA.SetRange3c(Field358);                            // (bind_to_dc fold)
        IObjRW* const Watched = ObjRwFromWatchable(const_cast<Watchable*>(Watcher310.target()));
        SubObjectA.SetWatchObject(Watched);                         // the routine
        SubObjectB.SetWatchObject(Watched);                         // the routine
        break;
    }
    case 1:
        // the destination point, into both Ideas.
        Field304 = Op.Destination;
        SubObjectA.SetPosition(Field304);                           // the routine
        SubObjectB.SetPosition(Field304);                           // the routine
        break;
    case 2:
        // the spline's four control points from op+0x38/+0x20/+0x2c/+0x44.
        Spline.P0 = *reinterpret_cast<Vector const*>(&Op.Field_38);
        Spline.P1 = *reinterpret_cast<Vector const*>(&Op.Field_20);
        Spline.P2 = *reinterpret_cast<Vector const*>(&Op.Field_2C);
        Spline.P3 = *reinterpret_cast<Vector const*>(&Op.Field_44);
        Spline.calculate_arclength();
        SubObjectC.SetSpline(&Spline);                              // the routine
        SubObjectC.SetWideMode(Op.Field_5A);                        // the routine (raw byte)
        Field2f8 = Spline.P2;
        break;
    default:
        // .
        Flags28 = static_cast<unsigned char>(Flags28 & 0xfd);
        return false;
    }

    // shared tail.
    Field358 = Op.Field_50;
    SubObjectA.SetRange3c(Field358);
    Field35c = Op.Field_54;
    // FCOMP double the field (1.0e-5, bytes f1 68 e3 88 b5 f8 e4 3e);
    // TEST AH,0x41 + JP is taken on greater OR unordered -> owner's get_projected_throttle
    // (the routine, slot 0xd4) of that speed; less-or-equal -> 1.0f. (The old port had the
    // two arms swapped and never made the call.)
    if (!(static_cast<double>(Op.Field_54) <= 1.0e-5))
        Field360 = Behavior_InvokeManagerSlotD4(ObjInspectOf(OwnerContextPtr14), Op.Field_54,
                                                IObject::ThrustEquipEngine);   // PUSH 0x1
    else
        Field360 = 1.0f;
    Flag367 = Op.Field_59;
    SubObjectA.SetEnabled(Flag367);                                 // the routine (raw byte)
    Flag366 = Op.Field_58;
    SubObjectA.SetField44(Op.Field_58 != 0 ? 2 : 0);                // /062fe892
    SubObjectC.SetField158(Op.Field_58 != 0 ? 2 : 0);               // the routine
    SetCruiseLock(Op.Field_6C == 0 ? 1 : 0, 3);                     // (SETZ)
    Flag36a = Op.Field_5A;
    Flag36b = Op.Field_5B;

    // the proximity watcher. Only a nonzero op+0x5c is looked up, and only
    // then are op+0x60/+0x64/+0x68 passed; otherwise all four arguments are zero.
    void* ProxTarget = nullptr;
    float ProxNear = 0.0f;
    float ProxFar = 0.0f;
    float ProxRatio = 0.0f;
    if (Op.Field_5C != 0)
    {
        ProxTarget = ObjDb->FindVerifiedObject((unsigned const*)&GotoOp.Field_5C);
        ProxNear = Op.Field_60;
        ProxFar = Op.Field_64;
        ProxRatio = *reinterpret_cast<float const*>(&Op.Field_68);
    }
    ProxWatcherInit(reinterpret_cast<unsigned char*>(&Watcher370), NavHelperPtr24, ProxTarget,
                    ProxNear, ProxFar, ProxRatio);                  // the routine

    Flags28 = static_cast<unsigned char>(Flags28 | 0x2);
    Flag2ea = 0;
    return true;
}

// Common / Common (implementation tracker item 2001,
// CopyActiveGotoOperation). Tiny getter/predicate pair for the same sibling class as the branch/
// /the branch above: copies the 0x70-byte state block back OUT of
// this+0x38c (the exact inverse of the branch's own IN-copy above) into a caller-
// supplied buffer, and returns bit 1 of Flags28 (this+0x28) as a bool.
// thiscall(this, void* OutBuffer).
extern "C" bool __fastcall CopyActiveGotoOperation(unsigned char* param_1, void* outBuffer)
{
    std::memcpy(outBuffer, param_1 + 0x38c, 0x70);
    return (param_1[0x28] & 0x2) != 0;
}

// baseline's two-sided clamp to [-1, 1] at module storage-062fea69 (and again for +0x2dc):
// FCOMP -1.0f with TEST AH,0x41 + JNZ sends not-greater AND unordered to -1.0f; then FCOMP
// 1.0f with TEST AH,0x5 + JP sends >= to 1.0f; otherwise the value stays.
static float GotoClampUnit(float Value)
{
    if (!(Value > -1.0f))
        return -1.0f;
    if (!(Value < 1.0f))
        return 1.0f;
    return Value;
}

// Slot 12, the routine (module storage-062fead1, RET 4).
void GotoBehavior::set_direction(Vector const& Dir)
{
    // goto kind 1 always proceeds; kind 0 only when |Dir|^2 (FPU, ((x*x +
    // y*y) + z*z)) is strictly greater than 0.0f (TEST AH,0x41 + JZ). Anything else clears
    // Flag2ea. (The old port proceeded on the NOT-greater side.)
    bool bProceed = false;
    if (Field2f4 == 1)
    {
        bProceed = true;
    }
    else if (Field2f4 == 0)
    {
        double const LengthSq = static_cast<double>(Dir.x) * Dir.x
                              + static_cast<double>(Dir.y) * Dir.y
                              + static_cast<double>(Dir.z) * Dir.z;
        bProceed = LengthSq > 0.0;
    }
    if (!bProceed)
    {
        Flag2ea = 0;
        return;
    }

    Flag2ea = 1;
    Field2d8 = -Dir.y;
    Field2e0 = 0.0f;                                                // (ECX == 0)
    Field2dc = -Dir.x;
    // factor kept on the FPU; FCOM double 0.0 with TEST AH,0x41 + JNZ ->
    // only a strictly positive factor scales; FDIVR gives 1/factor, each product FSTP dword.
    double const Factor = BehaviorMgrPtr20->get_turn_sensitivity_factor();
    if (Factor > 0.0)
    {
        double const InvFactor = 1.0 / Factor;
        Field2d8 = static_cast<float>(InvFactor * Field2d8);
        Field2dc = static_cast<float>(InvFactor * Field2dc);
    }
    Field2d8 = GotoClampUnit(Field2d8);
    Field2dc = GotoClampUnit(Field2dc);
    Field2e0 = -Dir.z;
}

// Slot 13, the branch (module storage-062feaf8, RET 4).
void GotoBehavior::set_throttle(float Throttle)
{
    Flag2eb = 1;
    Field2e4 = Throttle;
    Flag2e9 = 1;
}

// Slot 14, the branch (module storage-062feb5c, RET 4).
void GotoBehavior::set_cruise(bool bOn)
{
    if (bOn)
    {
        Flag2e8 = 1;                                                // (BL)
        Field2e4 = Field360;
        Flag2e9 = 1;
        return;
    }
    if (Flag2e8 != 0)
        Field2e4 = Field360;
    // ThrustManager::engage_cruise(NavHelper + 0x18, false). The old port
    // dropped this call as "ThrustManager not ported"; it is (common_systemmanager.h).
    static_cast<SystemManager*>(NavHelperPtr24)->Thrust.engage_cruise(false);
    Flag2e8 = 0;
    Flag2e9 = 1;
}

// Slot 18, the branch (module storage-062feb6a, RET 4).
void GotoBehavior::brake_reverse(bool bOn)
{
    Flag2ec = bOn ? 1 : 0;
}

