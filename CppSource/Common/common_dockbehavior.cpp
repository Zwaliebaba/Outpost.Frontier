// Author: Giperion.

#include "common_dockbehavior.h"
#include "common_misc.h"   // SinglePlayer() -- UpdateApproachTail()'s case 0xb calls it
#include "common_systemmanager.h"  // SystemManagerGunGroupManager::SetShieldActive -- the routine
#include "common_gotobehavior.h"   // ComputeFormationFollowerEnvelope / ProxWatcherComputeSpeedCap
#include "common_aisteer.h"        // FindObjectDockingAtBase (the routine) -- CheckClearanceProgress
#include "common_csys.h"           // Transform, Acquire3DMathEngine -- SetContext's hpmount offset
#include "x86math.h"               // I3DMathEngine::TransposeTransform, FVector, FMatrix
#include "common_cobject.h"        // CObject, CShip::request_exit_tradelane -- Disengage
                                   // -- AdvancePreamble's two speed-cap sources
                           // directly (Commonc5025)

#include <cmath>
#include <cstdint>
#include <cstdlib>   // rand -- EnterState(2)'s spread
#include <cstring>

// baseline pushes float parameters to the MoveIdea/FaceIdea setters as raw dword
// bit patterns (`PUSH 0x3eb2b8c3`), so the call sites below reinterpret rather
// than convert -- the same convention common_idea.cpp uses.
static float BitsToFloat(unsigned int Bits)
{
    float Result;
    std::memcpy(&Result, &Bits, sizeof(Result));
    return Result;
}

// The "-8 header" idiom, used at five sites inside UpdateApproachTail()/EnterState():
// a BaseWatcher's watched payload pointer is the middle of an allocation whose object
// header sits eight bytes earlier, and baseline reaches the object by subtracting 8 and
// treating null either side as "no object". baseline writes it as
// `MOV EAX,[ESI+0x324]; TEST EAX,EAX; JZ ...; ADD EAX,-8`.
static void* HeaderBlockFromWatched(Watchable const* Watched)
{
    if (Watched == nullptr)
        return nullptr;
    return const_cast<void*>(
        static_cast<void const*>(reinterpret_cast<char const*>(Watched) - 8));
}

// DockBehavior -- see common_dockbehavior.h for the full RTTI/layout writeup
// this pass relied on. Ported from Common (raw analysis
// is authoritative in this module over analysis's Common analysis --
// see this marathon's "ground truth precedence" rule, and item 1278's own
// prior notes about a analysis register-reuse bug in this exact neighborhood).

// ----------------------------------------------------------------------------
// (Common), thiscall, RET 4 -- constructor.
// the routine(TypeId)               Behavior's field initialisation
// the routine on +0x30              MoveIdea        SubObjectA
// the routine on +0xa8              FaceIdea        SubObjectB
// the routine on +0x168             SplineMoveIdea  SubObjectC
// the routine on +0x2dc             AfterburnerIdea GotoDirective.Idea
// DirectiveDockOp ctor on +0x2f4     DockOp
// +0x324 = +0x328 = 0                Watcher324
// ICRSplineSegment ctor on +0x348    Spline
// +0x450 = +0x454 = 0                ProxWatcher450.Watcher
// +0x45c = +0x458 = 0                ProxWatcher450.Unknown0c / Unknown08
// +0x320 = 0, +0x32c = 0, byte +0x39b = 0
// both vtables (implicit here).
// Nothing else is written: GotoDirective.NavPilotPtr (+0x2f0) is first stored by
// RecomputeApproach (module storage), the remaining fields by SetContext / EnterState /
// RefreshApproachState. Those members are therefore left default-initialised, as in
// baseline's operator new block.
DockBehaviorFields::DockBehaviorFields(int InTypeId)
{
    ConstructBase(InTypeId);   // the routine -- writes TypeId (+0x04) among the rest
}

DockBehavior::DockBehavior(int InTypeId)
    : DockBehaviorFields(InTypeId)
    , SubObjectA()
    , SubObjectB()
    , SubObjectC()
    , DockOp()
    , ModeState320(0)
    , Watcher324()
    , StateFlags32c(0)
    , Spline()
    , Flag39b(0)
{
    // ProxWatcher450's watcher dwords are zeroed by its own default
    // ctor; the two floats after it are zeroed as raw dwords.
    ProxWatcher450.Unknown0c = 0.0f;
    ProxWatcher450.Unknown08 = 0.0f;
}

// (Common), the destructor proper; the routine
// (item 1267) is the scalar-deleting wrapper. baseline is MSVC's member teardown in
// reverse declaration order, each step an inlined ~BaseWatcher (`if (Watched)
// set_pointer(0)`): +0x450 ProxWatcher450.Watcher, +0x324 Watcher324, then
// SubObjectC's ~SplineMoveIdea (+0x208 Face.Watcher, +0x190 Move.Watcher -- the same
// body as the out-of-line the routine), +0xbc SubObjectB.Watcher, +0x44
// SubObjectA.Watcher, and finally the routine (~Behavior). The compiler-generated
// member and base destructors reproduce exactly that sequence, so the body is empty.
DockBehavior::~DockBehavior()
{
}

// Own vtable slot 5 (offset 0x14). Common (the branch).
// All 10 instructions: EAX = (byte +0x28 & 2) ? 1 : 2; byte +0x29 -> 3; [+0xc] = EAX; the
// mode is also the return value (EAX), RET 4 (the manager's float timer is not read).
int DockBehavior::SetModeFromFlags()
{
    int Mode = (Flags28 & 0x2) ? 1 : 2;
    if (Flag29 != 0)
        Mode = 3;
    ModeSelector0xc = Mode;
    return Mode;
}

// Vtable slots 13/14 (the branch / the branch, 7 instructions each):
// `MOV ECX,[ECX+0x2f0]; TEST ECX,ECX; JZ ret; ...; JMP [[ECX]+0x34 / +0x38]`.
void DockBehavior::set_throttle(float Throttle)
{
    Behavior* const Pilot = static_cast<Behavior*>(GotoDirective.NavPilotPtr);
    if (Pilot != nullptr)
        Pilot->set_throttle(Throttle);
}

void DockBehavior::set_cruise(bool bOn)
{
    Behavior* const Pilot = static_cast<Behavior*>(GotoDirective.NavPilotPtr);
    if (Pilot != nullptr)
        Pilot->set_cruise(bOn);
}

// Vtable slots 16/17 (the branch / the branch): `MOV EAX,[ECX+0x2f0]; TEST EAX,EAX;
// JZ ret; JMP the routine / the routine` -- Behavior's own bodies on this object.
void DockBehavior::set_afterburner(bool bOn)
{
    if (GotoDirective.NavPilotPtr != nullptr)
        Behavior::set_afterburner(bOn);
}

void DockBehavior::slide_strafe_burst(StrafeDir Dir)
{
    if (GotoDirective.NavPilotPtr != nullptr)
        Behavior::slide_strafe_burst(Dir);
}

// Own vtable slot 11 (offset 0x2c). Common (the branch).
bool DockBehavior::IsClearanceState() const
{
    int Mode = ModeState320;
    return Mode != 6 && Mode != 7 && Mode != 8 && Mode != 10;
}

// Own vtable slot 24/26 (offsets 0x60/0x68). Common
// (the branch).
void* DockBehavior::GetWatchedRawBlock() const
{
    char const* Watched = reinterpret_cast<char const*>(Watcher324.target());
    if (Watched == nullptr)
        return nullptr;
    return const_cast<char*>(Watched) - 8;
}

// Own vtable slot 7 (offset 0x1c), Update/dispatch entry point.
// Common (the branch), 141 instructions.
int DockBehavior::Update()
{
    // `TEST byte ptr [ESI+0x28],1` -> result 2.
    if (Flags28 & 0x1)
    {
        UpdateResult10 = 2;
        return 2;
    }

    // / the branch: no owner, no watched object, or a watched pointer
    // of exactly 8 -> result 3.
    void const* Watched = Watcher324.target();
    if (ThreatSourcePtr14 == nullptr || Watched == nullptr ||
        reinterpret_cast<char const*>(Watched) - 8 == nullptr)
    {
        UpdateResult10 = 3;
        return 3;
    }

    IObjRW* const Owner = static_cast<IObjRW*>(ThreatSourcePtr14);

    // for a player owner (+0xbc is_player) with StateFlags32c bit 7
    // (`MOV AL,[+0x32c]; TEST AL,AL; JNS` skips), read the docking target's
    // get_status(float&) (+0x40) into a local preset to 1.0f. FCOMP 0.0f with
    // TEST AH,0x41 + JZ: strictly greater clears Flag39b; otherwise (not greater or
    // unordered) the first time only, Flag39b = 1 and the owner's IObjAffect
    // slot_6c(1, 0).
    if (Owner->is_player() && static_cast<signed char>(StateFlags32c & 0xff) < 0)
    {
        IObjRW* const Target = ObjRwFromWatchable(const_cast<Watchable*>(Watcher324.target()));
        float Health = 1.0f;
        Target->get_status(Health);
        if (Health > 0.0f)
        {
            Flag39b = 0;
        }
        else if (Flag39b == 0)
        {
            Flag39b = 1;
            static_cast<IObjAffect*>(Owner)->slot_6c(1, 0);
        }
    }

    // |target velocity|^2, (x*x + y*y) + z*z on the x87 stack,
    // against 0.3f (the field); TEST AH,0x41 + JNZ: not greater goes on to the
    // approach tail.
    {
        IObjRW* const Target = ObjRwFromWatchable(const_cast<Watchable*>(Watcher324.target()));
        Vector const Velocity = Target->get_velocity();
        double const SpeedSq = (static_cast<double>(Velocity.x) * Velocity.x +
                                static_cast<double>(Velocity.y) * Velocity.y) +
                               static_cast<double>(Velocity.z) * Velocity.z;
        if (SpeedSq > static_cast<double>(0.3f))
        {
            // the target is moving -- release both Ideas, result 1.
            SubObjectB.Commit();          // [+0xa8]+0x10
            SubObjectA.SetField44(0);     // on +0x30
            SubObjectA.Commit();          // [+0x30]+0x10
            UpdateResult10 = 1;
            return 1;
        }
    }

    // the proximity watcher at +0x450 against the target's
    // get_position() (+0x00), re-read for each call.
    unsigned char* const ProxWatcher = reinterpret_cast<unsigned char*>(&ProxWatcher450);
    ProxWatcherRecomputeZoneFlags(
        ProxWatcher, Field24,
        ObjRwFromWatchable(const_cast<Watchable*>(Watcher324.target()))->get_position());
    ProxWatcherNotifyZoneChange(
        ProxWatcher, Field24, Field20,
        ObjRwFromWatchable(const_cast<Watchable*>(Watcher324.target()))->get_position());

    // the routine with `this`; its EAX is the return value.
    return UpdateApproachTail();
}

// (Common), all 13 instructions: ESI = this+0x2f4,
// `MOV ECX,0xb; REP MOVSD` into the argument, then `MOV AL,[EAX+0x28]; SHR AL,1; AND AL,1`.
// The copy is a raw dword copy (pub::AI::DirectiveDockOp's operator= is deleted, as in
// baseline, so the vptr travels with the bytes exactly as REP MOVSD carries it).
bool DockBehavior::CopyDockOp(pub::AI::DirectiveDockOp* Out) const
{
    std::memcpy(static_cast<void*>(Out), static_cast<void const*>(&DockOp), 0xb * 4);
    return ((Flags28 >> 1) & 1) != 0;
}

// IObjAffect::RequestCB override -- own single secondary-vtable slot.
// Common (the branch), 66 instructions. Re-read in
// full this pass (2026-09-11) -- now faithfully translated, including the
// shared-tail EnterState() calls (see the class-level comment's 2026-09-11
// follow-up: this is the 2nd of now 5 confirmed direct DockBehavior call
// sites into the routine).
bool DockBehavior::OnResult(int Param1, int Param2, int Param3)
{
    // outer_this = ESI-0x2c in baseline (ESI is the RequestCB-adjusted `this`
    // the secondary-vtable thunk passes in); `this` here already IS
    // outer_this since we're a plain member function, not the raw thunk.
    if (Param1 == 0)
    {
        // `CMP dword[ESP+0x14],0x3; JZ ...; MOV BL,1; RET 0xc` -- no side
        // effect, just an early true.
        if (Param2 == 3)
            return true;

        // baseline: `MOV [ESI+0x410],ECX` == outer+0x43c = Field43c = Param3.
        Field43c = Param3;

        // baseline: `MOV EAX,[ESI+0x2f8]` == outer+0x324 = Watcher324's raw
        // Watched pointer, then the usual "-8 header, null either way is
        // invalid" test.
        void const* Watched = Watcher324.target();
        bool const bInvalidBlock =
            (Watched == nullptr) || (reinterpret_cast<char const*>(Watched) - 8 == nullptr);

        if (bInvalidBlock || Param3 == -1)
        {
            EnterState(0xd);
            Flag39a = 0; // baseline: `MOV byte[ESI+0x36e],BL` == outer+0x39a.
            return false;
        }

        // on the outer object.
        RefreshApproachState();

        // baseline: `TEST [ESI+0x300],0x840` == outer+0x32c = StateFlags32c.
        if (StateFlags32c & 0x840)
            EnterState(6);
        else
            EnterState(2);
        return false;
    }

    if (Param1 == 2)
    {
        if (Param2 == 4)
        {
            if (Param3 == -1)
            {
                EnterState(0xd);
                Flag39a = 0;
                return false;
            }
            // baseline: `MOV [ESI+0x3dc],0x41800000` == outer+0x408 = 16.0f.
            Field408 = 16.0f;
            EnterState(6);
            return false;
        }
        if (Param2 == 1)
        {
            EnterState(0xd);
            Flag39a = 0;
            return false;
        }
        return false;
    }

    // Any other Param1 value: baseline falls straight through to `RET 0xc`
    // with AL=0 and no side effects.
    return false;
}

// ----------------------------------------------------------------------------
// Newly ported this pass (2026-09-11) -- see common_dockbehavior.h's
// class-level comment (2026-09-11 follow-up) for the evidence trail these
// rely on.
// ----------------------------------------------------------------------------

// Own vtable slot 1 (offset 0x04). Common (the branch).
bool DockBehavior::SetContext(void* Param1, void* Param2, void* Param3)
{
    // baseline: `CALL the routine(this, Param1, Param2, Param3)` -- the shared
    // Behavior-family context initializer (see DockBehaviorFields'
    // ThreatSourcePtr14/Field20/Field24 comments). Ported inline here since
    // it's a small (24-instruction), fully-decoded leaf with no cross-cutting
    // dependency of its own:
    //   if (Param1 == nullptr || Param3 == nullptr) return false;  // baseline's 2nd null check is on P3
    //   ThreatSourcePtr14 = Param1;
    //   CachedHandle08 = Param1->vtbl[0x20](Param1);   // a getter call
    //   Field24 = Param3;
    //   Field20 = Param2;
    //   return true;
    // NOTE: parameter-to-field mapping confirmed directly from the routine's
    // raw analysis (not just call-site order) -- P2 and P3 land on
    // Field20/Field24 in the OPPOSITE order a naive "P1/P2/P3 -> +0x14/+0x20/
    // +0x24" reading would suggest (this function's own P3 stack slot is read
    // BEFORE its P2 slot, into Field24 first).
    // 2026-10-08: the shared binder is Behavior::BindContext now (common_behavior.cpp).
    bool const bOk = BindContext(Param1, Param2, Param3);
    if (!bOk)
        return false;                   // , AL == 0

    // baseline: clear the "hpmount lookup failed last tick"-shaped flags before
    // the (deferred) recompute branch.
    AbortFlag398 = 0;
    Flag39b = 0;

    // WIRED FOR REAL (2026-09-11, d285:common-port): baseline calls SubObjectA/
    // B's own vtable slot 0 here -- re-read directly from Common
    // 187744-187774 byte-for-byte: both calls push the SAME 3 arguments,
    // `(ThreatSourcePtr14, Field24, Param2)` -- Param2 is this function's own
    // raw incoming 2nd parameter (captured into EDI at function entry, BEFORE
    // the the routine call above, and never reloaded from +0x20/Field20
    // afterward; Field20 holds the identical value since the routine sets
    // Field20=Param2, but baseline's own register-level source really is the
    // still-live Param2, not a reload of the field). Argument order confirmed
    // by push order (ECX=ThreatSourcePtr14 pushed last = 1st param, matching
    // SetContext(InObject, InHandle, InContext)).
    SubObjectA.SetContext(ThreatSourcePtr14, Field24, Param2);
    SubObjectA.SetMode(0);                                          // the routine(0)
    GotoDirective.Idea.SetContext(ThreatSourcePtr14, Field24, Param2); // (+0x2dc slot 0)
    SubObjectB.SetContext(ThreatSourcePtr14, Field24, Param2);

    // WIRED FOR REAL 2026-09-17 (claude:abi-pass50): Common
    // 187778-187784 (`module storage MOV EDX,[ESI+0x168]`, `062c3250 LEA
    // ECX,[ESI+0x168]`, `062c3257 CALL dword ptr [EDX]`) -- SubObjectC's own
    // vtable slot 0, pushing the SAME three arguments as SubObjectA/SubObjectB
    // above. SubObjectC is a SplineMoveIdea (common_idea.h), so slot 0 is
    // BaseIdea::SetContext and this is now a typed call.
    SubObjectC.SetContext(ThreatSourcePtr14, Field24, Param2);

    // CORRECTED 2026-09-11 (d285:common-port): re-reading Common
    // 187783-187789 byte-for-byte while wiring the calls above -- this call
    // site actually zeroes the 3 unconfirmed dwords at +0x444/+0x448/+0x44c
    // (Opaque444), NOT SubStateValue440(+0x440)/Field330(+0x330) as the prior
    // pass's port claimed (no instruction anywhere in this function touches
    // either of those two fields). Fixed here; SubStateValue440/Field330
    // remain untouched by SetContext(), matching their own field comments
    // ("Cleared by RecomputeApproach()/Disengage()" -- not SetContext()).
    // (Three zero dwords == a zero Vector; Field444 since 2026-10-08.)
    Field444.x = 0.0f;
    Field444.y = 0.0f;
    Field444.z = 0.0f;
    Flags28 = static_cast<unsigned char>(Flags28 & ~0x2);

    // owner->get_hardpoint("hpmount", &Pos, nullptr) (IObjInspect +0xc0).
    // A non-zero result (no such hardpoint) zeroes the +0x430 offset (the branch).
    Vector HpPos;   // [ESP+0x18], left uninitialised by baseline
    if (ObjInspectOf(ThreatSourcePtr14)->get_hardpoint("hpmount", &HpPos, nullptr) != 0)
    {
        unsigned int* const Offset430 = reinterpret_cast<unsigned int*>(Opaque430);
        Offset430[0] = 0;
        Offset430[1] = 0;
        Offset430[2] = 0;
        return bOk;
    }

    // the SystemManager's physical-radius cache (+0x14fc block), the same
    // inlined refresh LaunchBehavior::EnsureNestedNavPositionCached reproduces.
    SystemManager* const Sys = static_cast<SystemManager*>(Field24);
    if (Sys->SelfTarget.bPosValid == 0)
    {
        Sys->SelfTarget.bPosValid = 1;
        Sys->SelfTarget.Iface->get_physical_radius(Sys->SelfTarget.CachedRadius,
                                                              Sys->SelfTarget.CachedPos);
    }

    // Delta = hardpoint - cached centre (each FSTP float), stored to
    // +0x430 and kept in a local.
    Vector Delta;
    Delta.x = HpPos.x - Sys->SelfTarget.CachedPos.x;
    Delta.y = HpPos.y - Sys->SelfTarget.CachedPos.y;
    Delta.z = HpPos.z - Sys->SelfTarget.CachedPos.z;
    std::memcpy(Opaque430, &Delta, sizeof(Delta));

    // Matrix = ExternalIface->get_transform() (+0xc, rotation at offset 0);
    // I3DMathEngine (lazily cached the field via the routine)->TransposeTransform(
    // &Delta, Matrix, +0x430) -- Dst is the local, Source is +0x430 -- then the local is
    // copied back into +0x430.
    Transform const& OwnerXform = Sys->SelfTarget.Iface->get_transform();
    I3DMathEngine* const Math = Acquire3DMathEngine();
    Vector Source;
    std::memcpy(&Source, Opaque430, sizeof(Source));
    Math->TransposeTransform(reinterpret_cast<FVector&>(Delta),
                             reinterpret_cast<FMatrix const&>(OwnerXform.Rotation),
                             reinterpret_cast<FVector const&>(Source));
    std::memcpy(Opaque430, &Delta, sizeof(Delta));
    return bOk;                                                      // AL = saved byte
}

// Own vtable slot 3 (offset 0x0c). Common (the branch).
void DockBehavior::RecomputeApproach()
{
    // baseline: `MOV [this+0x399],0; MOV [this+0x39a],0; MOV [this+0x39b],0;
    // MOV [this+0x2f0],0` -- clears Flag399/Flag39a/Flag39b and (a second,
    // independent write -- NOT through GotoDirective.NavPilotPtr's own
    // release sequence, just a raw zero) GotoDirective.NavPilotPtr.
    Flag399 = 0;
    Flag39a = 0;
    Flag39b = 0;
    GotoDirective.NavPilotPtr = nullptr;

    // `CALL the routine` -- Behavior's own slot 3 body.
    Behavior::Slot3();

    void const* Watched = Watcher324.target();
    bool const bInvalidBlock =
        (Watched == nullptr) || (reinterpret_cast<char const*>(Watched) - 8 == nullptr);
    if (bInvalidBlock)
    {
        EnterState(0xd);
        return;
    }

    Field43c = -1;
    CombatFlag344 = 0;                                               // (byte)

    // the routine(owner, 1.0f, 1) (get_speed, Engine) is FSTP'd to
    // a float stack slot and passed to the routine(owner, speed) (the coast distance at
    // 1000.0f); the result times 1.4f (the field == 0x3fb33333) is FSTP'd to Field33c.
    {
        float const Speed = Behavior_InvokeManagerSlotD8(ObjInspectOf(ThreatSourcePtr14), 1.0f,
                                                         IObject::ThrustEquipEngine);
        Field33c = static_cast<float>(
            static_cast<double>(Behavior_QueryFixedFalloffEffect(ThreatSourcePtr14, Speed)) * 1.4f);
    }

    // the routine(this, 1, 3) -- Behavior::SetCruiseLock.
    SetCruiseLock(1, 3);

    // `MOV EAX,[EBP+0x32c]; TEST AL,AL; JNS the branch` -- the
    // block below runs when bit 7 of the low byte is SET (the previous port had the two
    // branches swapped).
    if (static_cast<signed char>(StateFlags32c) < 0)
    {
        // owner->get_lane_start(watched object, &TargetShipPtr388,
        // &DestPoint3cc, &Facing38c) (IObjInspect +0x138; the object is target - 8, or null).
        IObjInspect const* const WatchedObj = ObjInspectOf(Watched != nullptr
            ? static_cast<void const*>(reinterpret_cast<char const*>(Watched) - 8)
            : nullptr);
        ObjInspectOf(ThreatSourcePtr14)->get_lane_start(
            WatchedObj, *static_cast<IObjInspect const**>(static_cast<void*>(&TargetShipPtr388)),
            DestPoint3cc, Facing38c);
        // LookMatrixYup(Facing38c) copied (9 dwords) into OrientMatrix3d8.
        OrientMatrix3d8 = LookMatrixYup(Facing38c);
        // the routine(&SubObjectA, object->get_id()) where the object is
        // Watcher324's target - 8 re-read; on a null target baseline calls through a null
        // pointer (062c3829 XOR ECX,ECX / MOV EDX,[ECX]) -- reproduced literally.
        Watchable const* const Watched2 = Watcher324.target();
        void const* const Obj2 = Watched2 != nullptr
            ? static_cast<void const*>(reinterpret_cast<char const*>(Watched2) - 8)
            : nullptr;
        SubObjectA.PublishTargetId(ObjInspectOf(Obj2)->get_id());
    }
    else if ((StateFlags32c & 0x40) != 0) // `TEST AL,0x40`
    {
        // `MOV dword ptr [EBP + 0x43c],EBX` with EBX held at 0
        // by the function's entry `XOR EBX,EBX` (module storage). This write
        // was missing from the port: Field43c is set to -1 unconditionally
        // earlier (module storage, `MOV [EBP+0x43c],0xffffffff`), and baseline
        // resets it to 0 again here, on this branch only, right before the
        // call below. Found 2026-09-17 while wiring this method's closing gate.
        Field43c = 0;

        RefreshApproachState();                                      // the routine
    }

    // WIRED FOR REAL (2026-09-17, claude:common-orchestrator). This block used
    // to be FULLPORT-DEFERRED on the claim that "ThreatSourcePtr14's class is
    // still unidentified", and that claim is now stale: ThreatSourcePtr14 is an
    // IObjRW. Four independent vcall offsets in this very function agree on it,
    // which is what makes it an identification and not a guess --
    // CALL [EAX+0xbc]  -> IObjInspect slot 47, is_player()
    // CALL [EDX+0x20]  -> IObjInspect slot  8, get_id()
    //   (deferred above)  vtbl[0x138]    -> IObjInspect slot 78
    // CALL [EDX+0x6c] on LEA ECX,[EAX+4]
    //                                    -> IObjAffect  slot 27 on the +4
    //                                       sub-object, whose own catalog note
    //                                       already records it being called on
    //                                       "the IObjAffect sub-object of a
    //                                       watched IObjRW".
    // IObjInspect at +0 and IObjAffect at +4 is exactly IObjRW's RTTI-proven
    // base layout (see the ClassHierarchyDescriptor note in common_iface.h).

    // The gate constant is likewise no longer opaque. the field is not a
    // datum in the image at all -- it is built at startup by the CRT
    // initializer the branch, registered at module storage:
    // FLD  float ptr [.rdata:the field]
    // FMUL float ptr [.rdata:the field]
    // FSTP float ptr [.data:the field]
    // and the field's four bytes are 00 40 1C 46 = 0x461C4000 = 10000.0f.
    // So the gate is a squared 10000.0f range test, compared against a squared
    // distance -- baseline never takes the square root.

    // Polarity, read from the listing rather than assumed: module storage
    // `FCOMP [the field]` / `TEST AH,0x5` / `JNP the branch`. TEST AH,0x5
    // masks C0|C2; JNP is taken when exactly one of them is set, i.e. C0=1 with
    // C2=0, which is the ordered strictly-less-than case. the branch pushes 1
    // and the branch's fallthrough pushes 0xd, so CLOSER than the range
    // continues the approach and FARTHER aborts it. (Note this is the 0x5/JNP
    // less-than idiom, NOT the 0x44/JP equality idiom this module has been
    // bitten by before.)
    float const kApproachRangeSq = 10000.0f * 10000.0f;   // == 1e8f exactly

    int NextState = 1;                                       // `PUSH 1`
    IObjInspect const* const ThreatObject = ObjInspectOf(ThreatSourcePtr14);
    if (ThreatObject->is_player())                           // / 062c3859 JZ
    {
        // Watcher324 re-read, object = target - 8 (or null, no test);
        // owner->get_position() first, then the object's (both IObjInspect slot 0).
        Watchable const* const Watched3 = Watcher324.target();
        void const* const Obj3 = Watched3 != nullptr
            ? static_cast<void const*>(reinterpret_cast<char const*>(Watched3) - 8)
            : nullptr;
        Vector const& ThreatPos = ThreatObject->get_position();
        Vector const& WatchedPos = ObjInspectOf(Obj3)->get_position();

        // (object - owner) componentwise as float subtractions on the
        // FPU, then (dx*dx + dy*dy) + dz*dz kept on the stack (double here).
        double const Dx = static_cast<double>(WatchedPos.x) - ThreatPos.x;
        double const Dy = static_cast<double>(WatchedPos.y) - ThreatPos.y;
        double const Dz = static_cast<double>(WatchedPos.z) - ThreatPos.z;
        double const DistSq = Dx * Dx + Dy * Dy + Dz * Dz;
        if (!(DistSq < kApproachRangeSq))                    // JNP: less -> 1
        {
            // owner's IObjAffect (+4) slot 0x6c(0, 0), then PUSH 0xd.
            ObjAffectOf(ThreatSourcePtr14)->slot_6c(0, 0);
            NextState = 0xd;
        }
    }
    EnterState(NextState);

    // the routine(&SubObjectA, &zero) -- Offset4c = 0 -- and
    // SetAngle38(pi) (0x40490fdb) on the same MoveIdea.
    Vector Zero;
    Zero.x = 0.0f;
    Zero.y = 0.0f;
    Zero.z = 0.0f;
    SubObjectA.Offset4c = Zero;
    SubObjectA.SetAngle38(3.14159274f);
    // MaxApproachSpeed46c = the field (1000.0f).
    MaxApproachSpeed46c = 1000.0f;
}

// Own vtable slot 4 (offset 0x10). Common (the branch).
// FULLY RE-READ AND CORRECTED 2026-09-11 (d285:common-port) while wiring the
// SubObjectA/B Commit() calls -- see the 3-point CORRECTED note on this
// method's declaration in common_dockbehavior.h for the prior pass's bugs
// this rewrite fixes (missing ModeState320==0 gate/alt-tail, inverted
// StateFlags32c branches, GotoWatcherPtr's spurious null-write).
void DockBehavior::Disengage()
{
    // baseline: `CMP [this+0x320],0; JZ the branch` -- the WHOLE
    // lock_maneuvers/NavPilot-release block and everything past it is gated
    // on ModeState320 != 0. When it's already 0, baseline takes a short,
    // completely separate tail instead (reproduced in the early-return below).
    if (ModeState320 == 0)
    {
        if (ApproachClearedFlag334 == 0)
            return;

        // (byte +0x334 set): +0x440 = 0, +0x330 = -1, byte +0x334 = 0, then the
        // owner's IObjAffect slot 0x60 (0, &id, 0) with Watcher324's object id.
        SubStateValue440 = 0;
        Field330 = -1;
        ApproachClearedFlag334 = 0;
        CancelOwnerRequest(0);
        return;
    }

    // IBehaviorManager::lock_maneuvers(false) on Field20.
    static_cast<IBehaviorManager*>(Field20)->lock_maneuvers(false);

    // with a Goto pilot, its Behavior slot 4, the routine on it, and
    // the pointer is cleared.
    if (GotoDirective.NavPilotPtr != nullptr)
    {
        static_cast<Behavior*>(GotoDirective.NavPilotPtr)->Slot4();
        Behavior_ReleaseDirectiveTarget(GotoDirective.NavPilotPtr);
        GotoDirective.NavPilotPtr = nullptr;
    }

    // `MOV AL,[+0x32c]; TEST AL,AL; JNS the branch`.
    if (static_cast<signed char>(StateFlags32c) < 0)
    {
        SubObjectA.SetField44(2);                                    // the routine

        // AbortFlag398 set and Flag399 clear -> the owner's CObject,
        // reduced to a CShip when (+0x4c & 0x503) == 0x503 (else null), and
        // CShip::request_exit_tradelane on it (thiscall on a possibly-null pointer, as
        // baseline).
        if (AbortFlag398 != 0 && Flag399 == 0)
        {
            CObject const* const Obj = ObjInspectOf(ThreatSourcePtr14)->cobject(); // +0x150
            CShip* Ship = nullptr;
            if (Obj != nullptr &&
                (*reinterpret_cast<unsigned int const*>(reinterpret_cast<char const*>(Obj) + 0x4c) &
                 0x503) == 0x503)
                Ship = const_cast<CShip*>(static_cast<CShip const*>(Obj));
            Ship->request_exit_tradelane();
        }

        // Flag39a set -> owner IObjAffect slot 0x60 (2, &id, 0).
        if (Flag39a != 0)
            CancelOwnerRequest(2);
    }
    else
    {
        // level 2 when the SystemManager's external interface is a player
        // and ModeState320 == 1, else 0.
        SystemManager* const Sys = static_cast<SystemManager*>(Field24);
        int const Level = (Sys->SelfTarget.Iface->is_player() && ModeState320 == 1) ? 2 : 0;
        SubObjectA.SetField44(Level);                                // the routine

        // unless Flags28 bit 0 is set, the approach reset and the
        // owner's IObjAffect slot 0x60 (0, &id, 0).
        if ((Flags28 & 0x1) == 0)
        {
            SubStateValue440 = 0;
            Field330 = -1;
            ApproachClearedFlag334 = 0;
            CancelOwnerRequest(0);
        }
    }

    // , the shared tail.
    Flag39a = 0;
    AbortFlag398 = 0;
    GotoDirective.Idea.Commit();                                     // (+0x2dc slot 4)
    SubObjectB.Commit();                                             // (+0xa8)
    SubObjectA.Commit();                                             // (+0x30)
    SetCruiseLock(1, 3);                                             // the routine
    ((SystemManager*)Field24)->GunGroups.SetShieldActive(1);         // the routine
    Behavior::Slot4();                                               // the routine
    ModeState320 = 0;
}

// The three `IObjAffect slot 0x60` sites in Disengage (062c39ae-062c39f9,
// , 062c3b1a-062c3b64) share one shape: Id = Watcher324's object
// (target - 8; a null target with a non-null -8 is impossible, and on the re-read baseline
// calls through a null pointer when the target is gone -- reproduced literally) ->get_id,
// or 0 when the target or its -8 is null; then the owner's IObjAffect (+4) slot 0x60
// (RequestType, &Id, 0).
void DockBehavior::CancelOwnerRequest(int RequestType)
{
    unsigned int Id = 0;
    Watchable const* const Watched = Watcher324.target();
    if (Watched != nullptr && reinterpret_cast<char const*>(Watched) - 8 != nullptr)
    {
        Watchable const* const Again = Watcher324.target();
        void const* const Obj = Again != nullptr
            ? static_cast<void const*>(reinterpret_cast<char const*>(Again) - 8)
            : nullptr;
        Id = ObjInspectOf(Obj)->get_id();
    }
    ObjAffectOf(ThreatSourcePtr14)->slot_60(RequestType, &Id, 0);
}

// Own vtable slot 6 (offset 0x18). Common (the branch).
// (Common), vtable slot 6 (evaluate), RET 4 -- the
// manager's context dword is not read. All 100 instructions traced. +0x14 is the owner
// object (the binder the routine stores it), read as an IObjInspect.
int DockBehavior::CheckClearanceProgress()
{
    if (AbortFlag398 != 0)
        return 1;

    // the owner's target into a zeroed local; none -> 2.
    IObjRW* Candidate = nullptr;
    ObjInspectOf(ThreatSourcePtr14)->get_target(Candidate);          // +0x74
    if (Candidate == nullptr)
        return 2;

    unsigned int Type = 0;   // baseline leaves [ESP+0x10] unset; get_type always writes it
    Candidate->get_type(Type);                                       // +0x88 (062c3638)
    if ((Type & 0x600de0) != 0)
    {
        if ((Type & 0x8c0) == 0)                                     // JNZ -> checks
        {
            if ((Type & 0x120) == 0)
                return 2;
            if (!Candidate->has_dock_hardpoints())                   // +0xc4 (062c365f)
                return 2;
        }
    }
    else
    {
        // `TEST AL,0x3` on the type's low byte.
        if ((Type & 0x3) == 0)
            return 2;
        unsigned int Base = 0;
        Candidate->get_base(Base);                                   // +0x8c
        if (Base == 0)
            return 2;
        // the routine(NavHelper+0x34, Candidate); zero -> 2.
        if (static_cast<SystemManager*>(Field24)->Timers.FindObjectDockingAtBase(Candidate) == nullptr)
            return 2;
    }

    // get_status into a local seeded with 1.0f; FCOMP 0.0f (the field) /
    // TEST AH,0x41 / JNZ -> 2 on less, equal or unordered, i.e. go on only when > 0.
    float Health = 1.0f;
    Candidate->get_status(Health);                                   // +0x40
    if (!(Health > 0.0f))
        return 2;

    // |velocity|^2 = (x*x + y*y) + z*z on the FPU stack; FCOMP 0.3f
    // (the field == 0x3e99999a) / TEST AH,0x5 / JNP -> 1 exactly on "less".
    Vector const Velocity = Candidate->get_velocity();               // +0x04
    double const SpeedSq = static_cast<double>(Velocity.x) * Velocity.x +
                           static_cast<double>(Velocity.y) * Velocity.y +
                           static_cast<double>(Velocity.z) * Velocity.z;
    return (SpeedSq < 0.3f) ? 1 : 2;
}

// Own vtable slot 12 (offset 0x30). Common (the branch).
// All 81 instructions traced. NavPilotPtr (+0x2f0) is the manager's dispatch-table Goto
// behavior (EnterState case 1 loads it from IBehaviorManager+0x17c), so its +0x30 / +0x28
// calls are Behavior::set_direction / Behavior::Slot10.
void DockBehavior::SubmitGotoDirective(Vector const& ReferencePoint)
{
    Behavior* const Pilot = static_cast<Behavior*>(GotoDirective.NavPilotPtr);
    if (Pilot == nullptr)
        return;

    // |Dir|^2 = (x*x + y*y) + z*z on the FPU; FCOMP 0.0f / TEST AH,0x44 /
    // JP. C3|C2: only "equal" gives odd parity (no jump); greater, less and unordered jump
    // to the branch, which forwards the direction to the pilot unchanged.
    double const MagSq = static_cast<double>(ReferencePoint.x) * ReferencePoint.x +
                         static_cast<double>(ReferencePoint.y) * ReferencePoint.y +
                         static_cast<double>(ReferencePoint.z) * ReferencePoint.z;
    if (!(MagSq == 0.0))
    {
        Pilot->set_direction(ReferencePoint);                        // [EAX+0x30]
        return;
    }

    // A zero direction: send the pilot a goto directive instead.
    pub::AI::DirectiveGotoOp Op;
    Op.Field_50 = 1000.0f;                       // .data:the field (0x447a0000)
    Op.Field_58 = 1;
    // the ProxWatcher450 object (target - 8) -> get_id, else 0.
    void* const ProxObj = HeaderBlockFromWatched(ProxWatcher450.Watcher.target());
    if (ProxObj != nullptr)
        Op.Field_5C = ObjInspectOf(ProxObj)->get_id();               // [EDX+0x20]
    else
        Op.Field_5C = 0;
    Op.Field_60 = ProxWatcher450.Unknown08;
    Op.Field_64 = ProxWatcher450.Unknown0c;

    if ((StateFlags32c & 0xc0) != 0)                                 // byte test
    {
        Op.Field_0C = 1;
        Op.Destination = DestPoint3cc;
    }
    else
    {
        // Field_0C = 0, Field_1C = Watcher324's object (target - 8, no null
        // test on the result) ->get_id.
        Op.Field_0C = 0;
        Watchable const* const Watched = Watcher324.target();
        void const* const Obj = Watched != nullptr
            ? static_cast<void const*>(reinterpret_cast<char const*>(Watched) - 8)
            : nullptr;
        Op.Field_1C = static_cast<int32_t>(ObjInspectOf(Obj)->get_id());
    }

    Pilot->Slot10(&Op);                                              // [EAX+0x28]
}

// Own vtable slot 22 (offset 0x58). Common (the branch).
// All 51 instructions traced. The one argument is a two-dword message {TypeCode, Payload}.
void DockBehavior::OnWatchedObjectDestroyed(void* Message)
{
    int const TypeCode = *static_cast<int const*>(Message);
    unsigned char const* const Payload =
        *reinterpret_cast<unsigned char const* const*>(static_cast<char const*>(Message) + 4);

    if (TypeCode == 4)
    {
        // no null test on the payload. Byte 0 must be clear, the owner's
        // id must equal Payload+0xc and Watcher324's object's id (target - 8, no null test on
        // the result) must equal Payload+0x10.
        if (Payload[0] != 0)
            return;
        if (ObjInspectOf(ThreatSourcePtr14)->get_id() !=
            *reinterpret_cast<unsigned int const*>(Payload + 0xc))
            return;
        Watchable const* const Watched = Watcher324.target();
        void const* const Obj = Watched != nullptr
            ? static_cast<void const*>(reinterpret_cast<char const*>(Watched) - 8)
            : nullptr;
        if (ObjInspectOf(Obj)->get_id() != *reinterpret_cast<unsigned int const*>(Payload + 0x10))
            return;
        Flag399 = 1;
        EnterState(0xc);
        return;
    }

    if (TypeCode == 3)
    {
        // a null payload is ignored; otherwise the SystemManager's external
        // interface id must equal Payload+8. No Flag399 store on this path.
        if (Payload == nullptr)
            return;
        SystemManager* const Sys = static_cast<SystemManager*>(Field24);
        if (*reinterpret_cast<unsigned int const*>(Payload + 0x8) !=
            Sys->SelfTarget.Iface->get_id())
            return;
        EnterState(0xc);
    }
    // Any other TypeCode: the branch, RET 4.
}

// Common / Common the routine, implementation tracker
// item 1279. NOT a vtable slot anywhere in the module; a plain non-virtual member of
// this class, called with an unadjusted `this` from RecomputeApproach()
// (module storage / 062c38c4), OnWatchedObjectDestroyed() and OnResult()
// (module storage..062c552e).

// MOVED HERE IN FULL 2026-09-17 (claude:common-dockmove). This function used to exist
// twice in the port: a two-case stub here, and the complete ten-case translation on
// FormationBehavior in common_behavior.cpp. There was never a reason for two copies --
// FormationBehavior's object is 0x2b8 bytes and this body writes as far as +0x46c, so
// it was never that class's. The complete translation is below, unchanged apart from
// field renames onto this class's own members. The "extending EnterState() to the
// remaining cases is a documented follow-up" note that stood here is discharged.

// The full translation was re-derived from the raw listing on 2026-09-17 by
// claude:common-formation, which fixed five things every earlier pass had wrong:
//   * the +0x2dc pre-switch call was a double deref; baseline loads the vtable from
//     [this+0x2dc] and passes `this+0x2dc` itself (062c3d5e-062c3d6a).
//   * case 1 set the DirectiveGotoOp's +0x50 to 0.0f "from a global tunable"; the
//     tunable (.data:the field) is 1000.0f (062c3dad), and baseline also fills
//     +0xc/+0x10..0x18 or +0x1c, +0x58, +0x5c, +0x60 and +0x64, then clears
//     NavPilotPtr[0x36c] after submitting (062c3e6f).
//   * case 4 called the disengage/threat-refresh pair; baseline resets the MoveIdea and
//     the FaceIdea (062c419a-062c41b8).
//   * cases 2/6/7/9 drove the wrong vectors and dropped every DeviceTRC::bind_to_dc
//     radius and every MaxApproachSpeed46c write. The radii are 64.0f / 10.0f /
//     15.0f / 25.0f and Field408*the field; the timeouts are 10.0f (0x41200000) and
//     18.0f (.data:the field), never the generic 1000.0f those branches were left at.
//   * cases 0xc/0xd dropped the Flags28 |= 1 / own-slot-0x10 / Flags28 &= ~2 sequence
//     that precedes the the routine call (062c44be-062c44ea).
void DockBehavior::EnterState(int NewState)
{
    if (NewState != 1 && ModeState320 == 1)
    {
        // the Goto pilot's Behavior slot 4, no null test in baseline.
        static_cast<Behavior*>(GotoDirective.NavPilotPtr)->Slot4();
        // the routine with ECX == NavPilotPtr's value.
        Behavior_ReleaseDirectiveTarget(GotoDirective.NavPilotPtr);
        GotoDirective.NavPilotPtr = nullptr;
        // ThrustManager::engage_cruise(false) on Field24 + 0x18.
        static_cast<SystemManager*>(Field24)->Thrust.engage_cruise(false);
        // the embedded AfterburnerIdea at +0x2dc, BaseIdea slot 4.
        GotoDirective.Idea.Commit();
    }

    // .rdata:the field == 0x447a0000 == 1000.0f, unconditional.
    MaxApproachSpeed46c = 1000.0f;
    ModeState320 = NewState;

    switch (NewState)
    {
    case 1:
    {
        // caseD_1 at 062c3d95: +0x17c of the manager is BehaviorDispatchTable[2], the Goto
        // behavior. baseline reads it with no null test (the old port had one).
        GotoDirective.NavPilotPtr = static_cast<IBehaviorManager*>(Field20)->BehaviorDispatchTable[2];

        pub::AI::DirectiveGotoOp Op;
        // .data:the field == 0x447a0000 == 1000.0f.
        Op.Field_50 = 1000.0f;
        Op.Field_58 = 1;                              // , BL

        // the ProxWatcher450 handle, "-8 header", slot 0x20.
        void* ProxObj = HeaderBlockFromWatched(ProxWatcher450.Watcher.target());
        if (ProxObj != nullptr)
        {
            Op.Field_5C = ((IObjRW*)ProxObj)->get_id();
        }
        else
        {
            Op.Field_5C = 0;
        }

        // / 062c3df8: +0x458 is ProxWatcher450.Unknown08, read as a float;
        // (this+0x450, 0) is the far threshold unscaled (PUSH EBP == 0).
        Op.Field_60 = ProxWatcher450.Unknown08;
        Op.Field_64 = QueryHandleRange(&ProxWatcher450, 0);

        if ((StateFlags32c & 0xc0) != 0)
        {
            // Field_0C = 1, Destination <- the Vector at +0x3cc.
            Op.Field_0C = 1;
            Op.Destination = DestPoint3cc;
        }
        else
        {
            // Field_0C = 0, Field_1C <- Watcher324's block, slot 0x20.
            Op.Field_0C = 0;
            // get_id() (+0x20) on `raw ? raw - 8 : 0` with no test, as baseline.
            Op.Field_1C = static_cast<int32_t>(
                ObjRwFromWatchable(const_cast<Watchable*>(Watcher324.target()))->get_id());
        }

        // the Goto behavior's slot 10 with the op, its slot 3, then its
        // +0x36c byte cleared -- all on [this+0x2f0] with no null test (the old port had one).
        GotoBehavior* const Pilot = static_cast<GotoBehavior*>(
            static_cast<Behavior*>(GotoDirective.NavPilotPtr));
        Pilot->Slot10(&Op);                                          // vtbl+0x28
        Pilot->Slot3();                                              // vtbl+0x0c
        Pilot->Flag36c = 0;
        return;
    }
    case 2:
    {
        // caseD_2 at 062c3e80.
        SetCruiseLock(1, 3);   // (this, EBX == 1, 3)

        if ((StateFlags32c & 0xc0) != 0)
        {
            // , ported 2026-10-08 (claude-code:cmn-ai5) from the x87 sequence.
            // Each FSTP dword is a float below; everything kept on the FPU stack is double.
            NavHelperRefreshLeadPoint();
            SystemManager* const Sys = static_cast<SystemManager*>(Field24);

            // D = own cached centre - DestPoint3cc, each FSTP float.
            Vector Delta;
            Delta.x = static_cast<float>(static_cast<double>(Sys->SelfTarget.CachedPos.x) - DestPoint3cc.x);
            Delta.y = static_cast<float>(static_cast<double>(Sys->SelfTarget.CachedPos.y) - DestPoint3cc.y);
            Delta.z = static_cast<float>(static_cast<double>(Sys->SelfTarget.CachedPos.z) - DestPoint3cc.z);

            // the watched object's get_transform() (+0x0c) -- baseline calls it
            // on `raw ? raw - 8 : 0` without a test -- rotation column 0, then the routine
            // (Behavior_GetIfaceMaxScalarAttribute) on the same object.
            IObjRW* const Watched = ObjRwFromWatchable(const_cast<Watchable*>(Watcher324.target()));
            Matrix const& Rot0 = Watched->get_transform().Rotation;
            float const Col0x = Rot0.E[0][0];
            float const Col0y = Rot0.E[1][0];
            float const Col0z = Rot0.E[2][0];
            double const Size = Behavior_GetIfaceMaxScalarAttribute(Watched);

            // Offset = column0 * Size * 2 * (1/3) (the field == 0x3eaaaaab).
            // x stays on the FPU throughout; y and z are FSTP'd after the multiply by Size,
            // and z once more after the doubling.
            double const OneThird = 0.3333333432674408;
            Vector Offset;
            Offset.x = static_cast<float>((static_cast<double>(Col0x) * Size * 2.0) * OneThird);
            float const ScaledY = static_cast<float>(static_cast<double>(Col0y) * Size);
            Offset.y = static_cast<float>((static_cast<double>(ScaledY) * 2.0) * OneThird);
            float const ScaledZ = static_cast<float>(static_cast<double>(Col0z) * Size);
            float const DoubledZ = static_cast<float>(static_cast<double>(ScaledZ) * 2.0);
            Offset.z = static_cast<float>(static_cast<double>(DoubledZ) * OneThird);

            // ((Oy*Dy + Ox*Dx) + Oz*Dz) against 0.0f; TEST AH,0x41 + JP
            // keeps the offset on greater or unordered, otherwise it is negated.
            double const Facing = static_cast<double>(Offset.y) * Delta.y
                                + static_cast<double>(Offset.x) * Delta.x
                                + static_cast<double>(Offset.z) * Delta.z;
            if (Facing <= 0.0)
            {
                Offset.x = -Offset.x;
                Offset.y = -Offset.y;
                Offset.z = -Offset.z;
            }

            // Spread = lerp(-128, 128, rand() * (1/32768)) (the field ==
            // 128.0f, the field == 1/32768f); the rand() product is stored as a double,
            // the result as a float: ((A - (-A)) * t) + (-A).
            float const SpreadHalf = 128.0f;
            float const SpreadLow = -SpreadHalf;
            double const RandUnit = static_cast<double>(rand()) * 3.0517578125e-05;
            float const Spread = static_cast<float>(
                (static_cast<double>(SpreadHalf) - SpreadLow) * RandUnit + SpreadLow);

            // rotation column 1 of a second get_transform() call, times Spread,
            // plus Offset. x goes through a float temporary first; z stays on the FPU.
            Matrix const& Rot1 = Watched->get_transform().Rotation;
            float const Col1x = Rot1.E[0][1];
            float const Col1y = Rot1.E[1][1];
            float const Col1z = Rot1.E[2][1];
            float const SpreadX = static_cast<float>(static_cast<double>(Col1x) * Spread);
            float const JitterX = static_cast<float>(static_cast<double>(SpreadX) + Offset.x);
            float const JitterY = static_cast<float>(static_cast<double>(Col1y) * Spread + Offset.y);
            double const JitterZ = static_cast<double>(Col1z) * Spread + Offset.z;

            // InterceptPoint39c = DestPoint3cc + (64 - 200) * Facing38c +
            // jitter (the field == 64.0f, the field == 200.0f). Facing x stays on the FPU,
            // y and z are FSTP'd after the multiply, z once more after adding Dest.z.
            double const Back = 64.0 - 200.0;
            double const BackX = Back * Facing38c.x;
            float const BackY = static_cast<float>(Back * Facing38c.y);
            float const BackZ = static_cast<float>(Back * Facing38c.z);
            double const BaseX = BackX + DestPoint3cc.x;
            double const BaseY = static_cast<double>(BackY) + DestPoint3cc.y;
            float const BaseZ = static_cast<float>(static_cast<double>(BackZ) + DestPoint3cc.z);
            InterceptPoint39c.x = static_cast<float>(BaseX + JitterX);
            InterceptPoint39c.y = static_cast<float>(BaseY + JitterY);
            InterceptPoint39c.z = static_cast<float>(static_cast<double>(BaseZ) + JitterZ);

            // Field338 from the watched object's size -- times 0.5f
            // (the field) when the type's 0x80 bit is set (JNS on the low byte), times 1/3
            // when only 0x40 is; neither leaves it alone.
            unsigned int const TypeBits = StateFlags32c;
            if ((TypeBits & 0x80) != 0)
                Field338 = static_cast<float>(
                    static_cast<double>(Behavior_GetIfaceMaxScalarAttribute(Watched)) * 0.5);
            else if ((TypeBits & 0x40) != 0)
                Field338 = static_cast<float>(
                    static_cast<double>(Behavior_GetIfaceMaxScalarAttribute(Watched)) * OneThird);
        }

        // the unconditional tail, reached from both branches.
        // Receiver is `LEA EDI,[ESI+0x30]` for everything but the the routine.
        SubObjectA.SetMode(0);                           // the routine
        SubObjectA.SetRange3c(64.0f);                    // bind_to_dc fold, .rdata:the field == 0x42800000
        SubObjectA.SetPosition(InterceptPoint39c);       // the routine
        SubObjectB.SetPosition(InterceptPoint39c);       // the routine
        SubObjectA.SetField44(0);                        // the routine
        return;
    }
    case 4:
        // caseD_4 at 062c419a -- three calls, all on the sub-objects. The two
        // `CALL [<vtbl>+0x10]` are BaseIdea slot 4 == Commit() on each.
        SubObjectA.SetField44(0);                        // the routine
        SubObjectA.Commit();                             // [EAX+0x10], ECX == this+0x30
        SubObjectB.Commit();                             // [EDX+0x10], ECX == this+0xa8
        return;
    case 6:
        // caseD_6 at 062c41c8. `TEST AL,0xc0` then `TEST AH,8` on the same dword.
        if ((StateFlags32c & 0xc0) != 0)
        {
            SubObjectB.SetAngleWeight8c(BitsToFloat(0x3ec90fdbu)); // the routine, pi/8
            SubObjectB.SetPosition(DestPoint3cc);           // the routine
            SubObjectA.SetPosition(DestPoint3cc);           // the routine
            SubObjectA.SetRange3c(10.0f);                   // bind_to_dc fold, 0x41200000
            SubObjectA.SetField44(2);                       // the routine
            MaxApproachSpeed46c = 10.0f;
            return;
        }
        if ((StateFlags32c & 0x800) != 0)
            break;                                          // -> default

        // tail. the routine is FaceIdea::SetBasis (nine dwords) on Orient3a8.
        SubObjectB.SetBasis(Orient3a8);                     // the routine
        SubObjectA.SetPosition(InterceptPoint39c);          // the routine
        SubObjectA.SetRange3c(10.0f);                       // bind_to_dc fold, .data:the field
        SubObjectA.SetMode(1);                              // the routine, EBX == 1
        MaxApproachSpeed46c = 18.0f;                   // .data:the field == 0x41900000
        // copies three dwords into receiver+0x4c == MoveIdea::Offset4c
        // (module storage-062d5a97); it is NOT one of the tagged target setters.
        SubObjectA.Offset4c = *static_cast<Vector const*>(RawAt(0x430)); // the routine
        return;
    case 7:
        // caseD_7 at 062c4291.
        SetCruiseLock(0, 3);
        SubObjectA.SetField44(2);                           // the routine
        SubObjectA.Offset4c = *static_cast<Vector const*>(RawAt(0x430)); // the routine
        if (CombatFlag344 != 0)
        {
            SubObjectB.SetPosition(SavedPosition3fc);       // the routine
            SubObjectA.SetMode(0);                          // the routine
            SubObjectA.SetPosition(SavedPosition3fc);       // the routine
            SubObjectA.SetRange3c(25.0f);                   // bind_to_dc fold, .data:the field
            return;
        }
        // .
        SubObjectB.SetBasis(OrientMatrix3d8);               // the routine
        SubObjectA.SetPosition(DestPoint3cc);               // the routine
        SubObjectA.SetRange3c(15.0f);                       // bind_to_dc fold, .data:the field
        SubObjectA.SetMode(1);                              // the routine
        MaxApproachSpeed46c = 18.0f;                   // .data:the field
        return;
    case 8:
        // caseD_8 at 062c4345. the routine's receiver is the SplineMoveIdea
        // at +0x168 (062c4358), not the behavior.
        BuildFinalApproachSpline();                         // the routine
        SetCruiseLock(0, 3);
        SubObjectC.SetField158(2);                          // the routine
        return;
    case 9:
    {
        // caseD_9 at 062c4370.
        SetCruiseLock(0, 3);
        SubObjectA.SetPosition(SavedPosition3fc);           // the routine
        SubObjectA.Offset4c = *static_cast<Vector const*>(RawAt(0x430)); // the routine
        // FLD float Field408, FMUL double the field (0.6), FSTP dword as
        // the bind_to_dc argument -- one rounding of the double product (the old port
        // multiplied two floats).
        SubObjectA.SetRange3c(static_cast<float>(
            static_cast<double>(Field408) * kEnterState9RadiusScale));

        if ((StateFlags32c & 0xc0) != 0)
        {
            SubObjectB.SetDirection(Facing38c);             // the routine
            SubObjectA.SetField44(2);                       // the routine
            return;
        }
        if ((StateFlags32c & 0x800) != 0)
        {
            // 's taken branch. Every receiver here is `MOV ECX,EDI`
            // with EDI == this+0x30 (LEA at 062c4380), i.e. SubObjectA.
            SubObjectA.SetMode(0);                          // the routine
            // writes MoveIdea+0x59, NOT the +0x58 SetEnabled()
            // writes -- two adjacent bytes, two different one-instruction
            // functions ten bytes apart. See MoveIdea::SetField59.
            SubObjectA.SetField59(0);                       // the routine
            SubObjectA.SetAngle38(BitsToFloat(0x3eb2b8c3u)); // set_origin_h fold
            SubObjectA.SetField44(2);                       // the routine
            SubObjectB.SetPosition(SavedPosition3fc);       // the routine
            return;
        }
        // . Field20 is IBehaviorManager::lock_maneuvers' receiver -- item
        // 1277 calls that function on it BY LINKED SYMBOL NAME, which is what settled
        // the field's type. Stored as void*, so the cast is explicit here.
        // baseline has no null test on Field20 here (the old port did).
        static_cast<IBehaviorManager*>(Field20)->lock_maneuvers(true);
        SubObjectA.SetField44(0);                           // the routine
        if (CombatFlag344 != 0)
        {
            SubObjectB.SetPosition(SavedPosition3fc);       // the routine
            SubObjectA.SetMode(0);                          // the routine
            return;
        }
        // . +0x40c's width is unproven, same as +0x3a8 -- raw reach,
        // but the routine copies nine dwords, so the span is a 3x3 Matrix.
        SubObjectB.SetBasis(*static_cast<Matrix const*>(RawAt(0x40c))); // the routine
        SubObjectA.SetMode(1);                              // the routine
        MaxApproachSpeed46c = 18.0f;                   // .data:the field
        if ((StateFlags32c & 0x20) != 0)
            break;                                          // -> default
        // the routine = Field24->GunGroups.SetShieldActive, 0 here (the
        // Disengage site pushes 1).
        ((SystemManager*)Field24)->GunGroups.SetShieldActive(0);
        return;
    }
    case 0xb:
        // caseD_b at 062c44ab: `MOV dword ptr [ESI+0x340],EBP` with EBP == 0.
        StateTimer340 = 0.0f;
        return;
    case 0xc:
        // caseD_c at 062c44d1.
        Flags28 = static_cast<unsigned char>(Flags28 | 1);
        Disengage();                                        // own vtbl slot 4 (offset 0x10)
        Flags28 = static_cast<unsigned char>(Flags28 & 0xfd);
        static_cast<IBehaviorManager*>(Field20)->SetDirectiveResult(7, 0);
        return;
    case 0xd:
        // caseD_d at 062c44be -- same sequence, result 2.
        Flags28 = static_cast<unsigned char>(Flags28 | 1);
        Disengage();
        Flags28 = static_cast<unsigned char>(Flags28 & 0xfd);
        static_cast<IBehaviorManager*>(Field20)->SetDirectiveResult(7, 2);
        return;
    default:
        // caseD_3/caseD_5/caseD_a and anything outside 1..0xd: nothing beyond the
        // MaxApproachSpeed46c/ModeState320 writes above.
        break;
    }
}

// ----------------------------------------------------------------------------
// FULLPORT-DEFERRED sub-object constructors -- see common_dockbehavior.h's
// class-level comment and the GotoDirective field comment.
// SubObjectA/B (the routine/the routine) are IDENTIFIED as real MoveIdea/
// FaceIdea (2026-09-11, d285:common-port) and SubObjectC (the routine) as a
// real SplineMoveIdea (2026-09-17, claude:abi-pass50) -- none of the three
// needs a stand-in any more, see the ctor above. GotoDirective's embedded object
// (the routine) is an AfterburnerIdea member since 2026-10-08.

// ----------------------------------------------------------------------------
// FULLPORT-DEFERRED stand-ins shared (by name/role, not by literal code) with
// FormationBehavior's own equivalents in common_behavior.cpp -- see each
// call site above for the baseline behavior each one stands in for.
// ----------------------------------------------------------------------------
// ReleaseGotoDirective is GONE (2026-09-17, claude:orch-dockpreamble). It was an
// empty `static void ReleaseGotoDirective()` -- and `static` was the reason it could
// not be written: baseline's the routine is a __thiscall whose first instruction is
// `MOV AL,[ECX+0x28]`, and at both of this class's call sites ECX is the value of
// GotoDirective.NavPilotPtr rather than `this`. It is five instructions and is now
// Behavior_ReleaseDirectiveTarget (common_behavior.h/.cpp), shared with the two
// GuideBehavior sites that had the same body stubbed under a second guessed name.
// SetCruiseLock (the routine) is Behavior::SetCruiseLock now (common_behavior.cpp).
// SetAlertLevel is GONE (2026-09-17, claude:w21-stubsweep). It was a guessed
// name for the routine, whose receiver is SubObjectA at every one of this
// class's eleven call sites, and which is MoveIdea::SetField44(int) --
// a real, ported one-line method in common_idea.h. Every site now calls it.
// is IBehaviorManager::SetDirectiveResult (common_behaviormanager.cpp), called on Field20.

// (Common, module storage-062c53e7), 207 instructions, all traced.
void DockBehavior::RefreshApproachState()
{
    // Field43c < -1 (signed JL) or no Watcher324 object -> return.
    if (Field43c < -1)
        return;
    Watchable const* const Watched = Watcher324.target();
    if (Watched == nullptr || reinterpret_cast<char const*>(Watched) - 8 == nullptr)
        return;

    // object->get_dock_hardpoints(Field43c, &SubStateValue440, &A, &B,
    // &C, &Field408) (IObjInspect +0xc8; the object is re-read as target - 8).
    IObjInspect const* const Obj = ObjInspectOf(reinterpret_cast<char const*>(Watched) - 8);
    Transform XformA;   // [ESP+0x40] local_90 -- left uninitialised by baseline
    Transform XformB;   // [ESP+0x70] local_60
    Transform XformC;   // [ESP+0xa0] local_30
    Obj->get_dock_hardpoints(Field43c, reinterpret_cast<TERMINAL_TYPE*>(&SubStateValue440),
                             &XformA, &XformB, &XformC, &Field408);
    SavedPosition3fc = XformA.Translation;
    Orient40c = XformA.Rotation;
    DestPoint3cc = XformB.Translation;
    OrientMatrix3d8 = XformB.Rotation;
    InterceptPoint39c = XformC.Translation;
    Orient3a8 = XformC.Rotation;

    CombatFlag344 = 0;
    if ((StateFlags32c & 0x200100) == 0)                             // / 062c51fb
    {
        // D1 = DestPoint3cc - InterceptPoint39c, D2 = SavedPosition3fc -
        // DestPoint3cc (float subtractions, Vec3Assign).
        Vector D1;
        D1.x = DestPoint3cc.x - InterceptPoint39c.x;
        D1.y = DestPoint3cc.y - InterceptPoint39c.y;
        D1.z = DestPoint3cc.z - InterceptPoint39c.z;
        Vector D2;
        D2.x = SavedPosition3fc.x - DestPoint3cc.x;
        D2.y = SavedPosition3fc.y - DestPoint3cc.y;
        D2.z = SavedPosition3fc.z - DestPoint3cc.z;

        // acos(ClampedDot3(normalize(D1), normalize(D2))) (D2 normalized
        // first); FCOMP the field (0x3dc90fdb == float pi / 32) / TEST AH,0x5 / JP --
        // the block below runs only when the angle is strictly smaller.
        Vector N2;
        VecNormalize3(&D2, &N2);
        Vector N1;
        VecNormalize3(&D1, &N1);
        double const Angle = std::acos(ClampedDot3(N1, N2));
        if (Angle < 3.14159274f / 32.0f)
        {
            // S = (colZ(Orient40c) + colZ(OrientMatrix3d8)) +
            // colZ(Orient3a8), each sum FSTP'd to float through Vec3Assign.
            Vector Sum1;
            Sum1.x = Orient40c.E[0][2] + OrientMatrix3d8.E[0][2];
            Sum1.y = Orient40c.E[1][2] + OrientMatrix3d8.E[1][2];
            Sum1.z = Orient40c.E[2][2] + OrientMatrix3d8.E[2][2];
            Vector Sum2;
            Sum2.x = Sum1.x + Orient3a8.E[0][2];
            Sum2.y = Sum1.y + Orient3a8.E[1][2];
            Sum2.z = Sum1.z + Orient3a8.E[2][2];
            // ClampedDot3(D2, -Sum2) (D2 un-normalized); FCOMP 0.9999f
            // (the field == 0x3f7ff972) / TEST AH,0x41 / JNZ -- strictly greater sets
            // the flag.
            Vector NegSum;
            NegSum.x = -Sum2.x;
            NegSum.y = -Sum2.y;
            NegSum.z = -Sum2.z;
            if (ClampedDot3(D2, NegSum) > 0.9999f)
                CombatFlag344 = 1;
        }
    }

    // with StateFlags32c bit 6, Facing38c = normalize(SavedPosition3fc -
    // DestPoint3cc).
    if ((StateFlags32c & 0x40) != 0)
    {
        Vector D3;
        D3.x = SavedPosition3fc.x - DestPoint3cc.x;
        D3.y = SavedPosition3fc.y - DestPoint3cc.y;
        D3.z = SavedPosition3fc.z - DestPoint3cc.z;
        Vector N3;
        VecNormalize3(&D3, &N3);
        Facing38c = N3;
    }
}

// ----------------------------------------------------------------------------
// Common / Common,
// implementation tracker item 1285. Update()'s non-virtual tail; see the declaration in
// common_dockbehavior.h for the ownership proof and the move history.

// MOVED HERE 2026-09-17 (claude:common-dockmove) from common_behavior.cpp, where it
// was hosted on FormationBehavior. The body is the 2026-09-17 re-derivation by
// claude:common-formation, unchanged apart from renaming its fields onto this class's
// own members -- it already addressed this class's sub-object offsets through raw
// accessors, which are now the real typed members SubObjectA/B/C.

// Each of these was wrong in the port before that re-derivation, and is now what the
// baseline bytes say:
//   * case 2's give-up gate tested only `OwnerContext == nullptr`; baseline tests
//     `TargetShipPtr388 == 0 && !ObjInspect(ThreatSourcePtr14)->slot_bc() &&
//     (char)StateFlags32c < 0` (062c4a2c-062c4a4b), and derefs the context
//     unconditionally.
//   * 's watcher result was allowed to fall into the "enter hold" tail
//     even when it was not 1; baseline goes to the default case (062c4c2f).
//   * case 3 wrote the +0x2a8 flag; baseline writes +0x39a (062c4cb2), and the tail is
//     an IObjAffect slot-0x5c call with a mode of 2 (only when the StateFlags32c sign
//     bit is set) or 0, not a behavior-done notification.
//   * case 8 drove the MoveFx sub-object; baseline drives the SplineMoveIdea at +0x168
//     for the tick, the reset and the pi/16 angle read alike (062c4dad-062c4de7).
//   * case 9's clearance test was inverted: baseline enters state 0xd when the
//     IObjAffect slot-0x64 query returns NON-zero (062c4f44).
//   * case 0xb's threshold is 3.0f, or .rdata:the field == 10.0f (bytes 00 00 20 41)
//     on the not-single-player, player-owner branch (062c5011 / 062c503d).
int DockBehavior::UpdateApproachTail()
{
    // baseline keeps the eventual result in a stack local ([ESP+0x14], preset to
    // EBX == 1) and stores it into UpdateResult10@0x10 on every exit.
    UpdateResult10 = 1;
    AdvancePreamble();  // , ECX == this

    float const DeltaTime = GetTickDelta();   // Behavior's own +0x1c, see common_behavior.h

    if (ModeState320 == 1)
    {
        // the Goto behavior's +0x1c tick delta = ours, then its slot 7
        // (vtable +0x1c); anything but 2 leaves through the default case. baseline tests
        // NavPilotPtr for null nowhere (the old port did, 2026-10-08).
        Behavior* const Pilot = static_cast<Behavior*>(GotoDirective.NavPilotPtr);
        *reinterpret_cast<float*>(reinterpret_cast<char*>(Pilot) + 0x1c) = DeltaTime;
        if (Pilot->Slot7() != 2)
            return UpdateResult10;

        if ((StateFlags32c & 0xc0) != 0)
        {
            EnterState(2);
            UpdateResult10 = 1;
            return UpdateResult10;
        }
        // baseline: goto the branch (shared with case 2's "else" tail)
        EnterState(3);
        UpdateResult10 = 1;
        return UpdateResult10;
    }

    bool bAborted = false;
    if (ModeState320 != 0xc)
    {
        // get_status(float& Health) (+0x40) into a local preset to 1.0f;
        // FCOMP 0.0f with TEST AH,0x41 + JNZ aborts on not-greater OR unordered. baseline calls
        // it on `raw ? raw - 8 : 0` with no test, and so does the port.
        float Health = 1.0f;
        ObjRwFromWatchable(const_cast<Watchable*>(Watcher324.target()))->get_status(Health);
        if (!(Health > 0.0f))
            bAborted = true;
    }

    {
        void* ProxWatched = const_cast<void*>(
            static_cast<void const*>(ProxWatcher450.Watcher.target()));
        if (bAborted ||
            ((ProxWatched != nullptr) && (ProxWatched != reinterpret_cast<void*>(8)) &&
             ((ProxWatcher450.Flags14 & 2) == 0)))
        {
            // `LEA EDI,[ESI+0x30]` is the receiver for all three.
            SubObjectA.SetField44(0);     // the routine
            SubObjectA.Commit();          // [EDI]+0x10, BaseIdea slot 4
            SubObjectB.Commit();          // [EAX]+0x10 on this+0xa8
            UpdateResult10 = 1;
            return UpdateResult10;
        }
    }

    if (!CheckCanProceed())
    {
        Disengage();            // own vtbl slot 4 (offset 0x10), the branch
        RecomputeApproach();    // own vtbl slot 3 (offset 0x0c), the branch
        UpdateResult10 = 1;
        return UpdateResult10;
    }

    switch (ModeState320)
    {
    case 2:
    {
        // . All three conditions, in baseline's order; ThreatSourcePtr14 is
        // dereferenced with no null test (the old port had one, removed 2026-10-08).
        if ((TargetShipPtr388 == nullptr) &&
            !ObjInspectOf(ThreatSourcePtr14)->is_player() &&                 // vtbl+0xbc
            (static_cast<char>(StateFlags32c) < 0))
        {
            EnterState(0xc);
            UpdateResult10 = 1;
            return UpdateResult10;
        }

        bool bPastDock = false;
        if ((StateFlags32c & 0xc0) != 0)
        {
            // (ported 2026-10-08; the dot was never evaluated before, so
            // the dot < 0 exit to the branch was missing): radius-cache refresh, D = cached
            // centre - DestPoint3cc through Vec3Assign (FSTP float each), then
            // ((Dz*Fz + Dy*Fy) + Dx*Fx) on the FPU against 0.0f with TEST AH,0x5 + JP: >= 0 or
            // unordered falls to the branch.
            NavHelperRefreshLeadPoint();
            SystemManager* const Sys = static_cast<SystemManager*>(Field24);
            Vector Delta;
            Delta.x = Sys->SelfTarget.CachedPos.x - DestPoint3cc.x;
            Delta.y = Sys->SelfTarget.CachedPos.y - DestPoint3cc.y;
            Delta.z = Sys->SelfTarget.CachedPos.z - DestPoint3cc.z;
            double const Along = static_cast<double>(Delta.z) * Facing38c.z
                               + static_cast<double>(Delta.y) * Facing38c.y
                               + static_cast<double>(Delta.x) * Facing38c.x;
            // FCOMP 200.0f (the field), TEST AH,0x41 + JNZ to
            // on not-greater -- always taken for a negative value, so
            // is unreachable in baseline (same shape as CheckCanProceed).
            bPastDock = Along < 0.0;
        }

        if (!bPastDock)
        {
            // . The FaceIdea ticks first, its result discarded; the MoveIdea's
            // result decides between the tail and the default case.
            (void)SubObjectB.Update(DeltaTime);
            if (SubObjectA.Update(DeltaTime) != 1)
                break;   // JZ switchD_062c4a25::caseD_4
        }

        // .
        EnterState(((StateFlags32c & 0x8c0) == 0) ? 6 : 3);
        UpdateResult10 = 1;
        return UpdateResult10;
    }
    case 3:
    {
        // . baseline's EBP (the slot-0x5c mode argument) and the [ESP+0x1c]
        // out-slot both start at 0 and are only filled in on the sign-bit branch.
        int SlotMode = 0;
        int TargetQuery = 0;
        if (static_cast<char>(StateFlags32c) < 0)
        {
            // SavedPosition3fc@0x3fc <- DestPoint3cc@0x3cc, copied
            // dword by dword.
            SavedPosition3fc = DestPoint3cc;
            // TargetShipPtr388's get_id (IObjInspect slot 8, +0x20), no
            // null test.
            TargetQuery = static_cast<int>(ObjInspectOf(TargetShipPtr388)->get_id());
            SlotMode = 2;
        }
        if (ApproachClearedFlag334 != 0)
        {
            // -> the branch, shared with case 2's hold exit.
            EnterState(6);
            UpdateResult10 = 1;
            return UpdateResult10;
        }

        EnterState(4);
        Flag39a = 1;   // .

        // the docking object's get_id (+0x20) on `W ? W - 8 : 0`, no null
        // test.
        int const EngineQuery = static_cast<int>(
            ObjInspectOf(HeaderBlockFromWatched(Watcher324.target()))->get_id());

        // five-argument call on ThreatSourcePtr14's IObjAffect
        // sub-object (`LEA ECX,[EAX+4]`), slot 0x5c, with `this+0x2c` last -- which is
        // this object's own IObjAffect::RequestCB base sub-object, so the cast below is
        // the typed equivalent of baseline's LEA. Pushes, right to left: EDI (the
        // RequestCB), 0, &[ESP+0x1c] (TargetQuery), &[ESP+0x20] (EngineQuery), EBP
        // (SlotMode). Slot 23's shape is fixed by Server.dll's Ship body module storage
        // (RET 0x14; both id pointers are dereferenced as dwords) -- see common_iface.h.
        // The result in EAX is not read (062c4cef reloads EAX from the stack). No null
        // test on ThreatSourcePtr14 (the port's former guard is gone).
        (void)ObjAffectOf(ThreatSourcePtr14)->slot_5c(
            SlotMode, reinterpret_cast<unsigned const*>(&EngineQuery),
            reinterpret_cast<unsigned const*>(&TargetQuery), 0,
            static_cast<IObjAffect::RequestCB*>(this));
        UpdateResult10 = 1;
        return UpdateResult10;
    }
    case 6:
        // `TEST AH,8` on the dword at +0x32c, i.e. bit 0x800.
        if ((StateFlags32c & 0x800) != 0)
        {
            EnterState(9);          // , shared with case 8
            UpdateResult10 = 1;
            return UpdateResult10;
        }
        {
            int MoveResult = SubObjectA.Update(DeltaTime);
            int FaceResult = SubObjectB.Update(DeltaTime);
            if (((StateFlags32c & 0xc0) != 0) && (FaceResult == 1))
            {
                EnterState(8);
                UpdateResult10 = 1;
                return UpdateResult10;
            }
            if (MoveResult != 1)
                break;              // JNZ default
            EnterState((CombatFlag344 != 0) ? 9 : 7);
            UpdateResult10 = 1;
            return UpdateResult10;
        }
    case 7:
        // . Face first, then MoveIdea; both results but only the second one is
        // tested. Falls into case 10's tail either way.
        (void)SubObjectB.Update(DeltaTime);
        if (SubObjectA.Update(DeltaTime) == 1)
            EnterState(9);
        [[fallthrough]];
    case 10:
        // caseD_a: baseline stores EBP, which is 0 on every path reaching here.
        UpdateResult10 = 0;
        return UpdateResult10;
    case 8:
    {
        // . The receiver is `LEA EDI,[ESI+0x168]` -- SubObjectC, this class's
        // SplineMoveIdea -- for the tick, the reset and the angle read alike.
        if (SubObjectC.Update(DeltaTime) == 1)
        {
            SubObjectC.Commit();              // , vtbl slot 0x10 == BaseIdea slot 4
            EnterState(9);                    // falls into the branch
            UpdateResult10 = 1;
            return UpdateResult10;
        }
        // the routine (SplineMoveIdea::GetFaceAngle) on the SAME +0x168 receiver,
        // compared against .rdata:the field == 0x3e490fdb == 0.19634955f (pi/16).
        // `TEST AH,5; JNP` leaves for the default case only when strictly less; unordered
        // builds the spline.
        if (!(SubObjectC.GetFaceAngle() < 0.19634955f))
        {
            BuildFinalApproachSpline();       // , ECX == this
            UpdateResult10 = 1;
            return UpdateResult10;
        }
        break;
    }
    case 9:
    {
        // caseD_9 at 062c4e4c. baseline spills EBP (== 0) into the result slot first, so
        // every exit from this case that does not set it again returns 0.
        UpdateResult10 = 0;
        (void)SubObjectB.Update(DeltaTime);
        int MoveResult = SubObjectA.Update(DeltaTime);

        if (static_cast<char>(StateFlags32c) < 0)
        {
            if (MoveResult != 1)
            {
                // SavedPosition3fc + Facing38c * .rdata:the field, which
                // is 0xc1c80000 == -25.0f, hence the subtraction.
                // x and y products stay on the FPU until the add; z's is FSTP'd first.
                Vector RetreatPoint;
                RetreatPoint.x = static_cast<float>(static_cast<double>(Facing38c.x) * -25.0 + SavedPosition3fc.x);
                RetreatPoint.y = static_cast<float>(static_cast<double>(Facing38c.y) * -25.0 + SavedPosition3fc.y);
                float const BackZ = static_cast<float>(static_cast<double>(Facing38c.z) * -25.0);
                RetreatPoint.z = static_cast<float>(static_cast<double>(BackZ) + SavedPosition3fc.z);
                // the routine(&RetreatPoint, ThreatSourcePtr14, 0.0f) ==
                // IsPointOutsideHeadingCone (was the always-true RunCollisionProbe stub).
                if (!IsPointOutsideHeadingCone(RetreatPoint, ObjInspectOf(ThreatSourcePtr14), 0.0f))
                    break;          // JZ default
            }

            // . Both `CALL [<vtbl>+0x10]` == BaseIdea slot 4, Commit().
            SubObjectA.Commit();
            SubObjectB.Commit();
            // ThreatSourcePtr14's IObjAffect sub-object, slot 18
            // set_target(0, SUBOBJ_ID_NONE, -1). WIRED 2026-09-24: this used to go through
            // the empty NotifyBehaviorDone stub, whose other two call sites (the routine)
            // are a different function.
            (void)ObjAffectOf(ThreatSourcePtr14)->set_target(nullptr, SUBOBJ_ID_NONE, -1);

            // IObjAffect slot 25, go_tradelane(<-8 header of the
            // watched block>, TargetShipPtr388, false, 0.0f) -- the two `PUSH 0` are the
            // bool and the float. baseline enters state 0xd when this returns NON-zero
            // (062c4f44 JNZ). WIRED 2026-09-24: this used to call the RunCollisionProbe
            // stub (the routine's stand-in), which always returned true.
            void* EngineObj = HeaderBlockFromWatched(Watcher324.target());
            if (ObjAffectOf(ThreatSourcePtr14)->go_tradelane(
                    static_cast<IObjInspect const*>(EngineObj),
                    static_cast<IObjInspect const*>(TargetShipPtr388), false, 0.0f) != 0)
            {
                EnterState(0xd);
                return UpdateResult10;
            }
            // == IBehaviorManager::QueueNotification (item 1561, 133
            // instructions, ported in common_behaviormanager.cpp). WIRED FOR REAL 2026-09-17
            // (claude:w21-stubsweep) -- it used to run through an empty
            // `FormationBehavior::ReleaseDockPoint(void*, int, int) {}` stub, whose
            // guessed signature had folded the RECEIVER into the first parameter and
            // lost the real first argument entirely.
            // MOV  ECX,[ESI+0x14]   <- pushed as arg 1
            // PUSH EAX (== 0)       <- arg 3, Duration
            // PUSH EAX (== 0)       <- arg 2, Extra
            // PUSH ECX
            // MOV  ECX,[ESI+0x20]   <- the RECEIVER, Field20
            // CALL the routine
            static_cast<IBehaviorManager*>(Field20)->QueueNotification(
                static_cast<IObjRW*>(ThreatSourcePtr14), 0, 0.0f);
            SubObjectA.SetField44(2);   // the routine
            EnterState(10);
            AbortFlag398 = 1;       // +0x398
            Flag39a = 1;            // +0x39a
            return UpdateResult10;
        }

        // .
        if (MoveResult != 1)
            break;                  // JNZ default
        if (SubStateValue440 != 3 && SubStateValue440 != 4 && SubStateValue440 != 5)
        {
            EnterState(0xb);
            return UpdateResult10;
        }
        // . Both `CALL [<vtbl>+0x10]` == BaseIdea slot 4, Commit().
        SubObjectA.Commit();
        SubObjectB.Commit();
        // the same IObjAffect::set_target(0, SUBOBJ_ID_NONE, -1).
        (void)ObjAffectOf(ThreatSourcePtr14)->set_target(nullptr, SUBOBJ_ID_NONE, -1);
        EnterState(0xc);
        return UpdateResult10;
    }
    case 0xb:
    {
        // caseD_b at 062c500e.
        StateTimer340 += DeltaTime;
        float Threshold = 3.0f;     // , immediate 0x40400000
        if (!SinglePlayer() && ObjInspectOf(ThreatSourcePtr14)->is_player())
        {
            // .rdata:the field = 00 00 20 41 = 10.0f. (The listing shows only
            // its first two bytes before a mis-sliced string at 0639e886; a 2026-09-17 note
            // read that as 0.0f.) No null test on ThreatSourcePtr14.
            Threshold = 10.0f;
        }
        // FCOMP StateTimer340, `TEST AH,5; JP` leaves for the default case
        // unless Threshold is strictly less than StateTimer340.
        if (Threshold < StateTimer340)
        {
            Disengage();            // own vtbl slot 4 (offset 0x10)
            RecomputeApproach();    // own vtbl slot 3 (offset 0x0c)
            UpdateResult10 = 1;
            return UpdateResult10;
        }
        break;
    }
    case 0xc:
    case 0xd:
        UpdateResult10 = 2;
        break;
    default:
        break;
    }

    return UpdateResult10;
}

// ----------------------------------------------------------------------------
// FULLPORT-DEFERRED stand-ins that came here 2026-09-17 with
// UpdateApproachTail()/EnterState(). Each stands in for a real baseline callee that is
// not ported yet; the call site, its order and its arguments are faithful.
// ----------------------------------------------------------------------------

// (module storage-062c4759), ported 2026-10-08 (claude-code:cmn-ai5); it was a
// stub returning true. A jump table on ModeState320 - 2 (062c454a, 7 entries):
//   mode 2       -> the range test (caseD_0, 062c4710)
//   modes 3, 6   -> caseD_1 (062c4599)
//   mode 8       -> caseD_6 (062c4551): range test only when Flag39a or ApproachClearedFlag334
//   modes 4/5/7 and anything else (JA) -> true
// The range test (the branch / 062c4710): the routine (IsWithinInterceptRangeOfIface) on
// SystemManager+0x14fc and Watcher324's object with float(1000.0f (.data the field) * 1.05
// (double the field)).
bool DockBehavior::CheckCanProceed()
{
    SystemManager* const Sys = static_cast<SystemManager*>(Field24);
    switch (ModeState320)
    {
    case 3:
    case 6:
    {
        if ((StateFlags32c & 0xc0) == 0)
            break;                                                   // -> range test
        // cache refresh (the routine), D = centre - DestPoint3cc
        // (the routine, one float subtraction per component).
        NavHelperRefreshLeadPoint();
        Vector Delta;
        Delta.x = Sys->SelfTarget.CachedPos.x - DestPoint3cc.x;
        Delta.y = Sys->SelfTarget.CachedPos.y - DestPoint3cc.y;
        Delta.z = Sys->SelfTarget.CachedPos.z - DestPoint3cc.z;
        // ((z*Fz + y*Fy) + x*Fx) on the FPU; FCOM 0.0f, TEST AH,0x5 + JP
        // returns false on >= 0 or unordered.
        double const Along = static_cast<double>(Delta.z) * Facing38c.z
                           + static_cast<double>(Delta.y) * Facing38c.y
                           + static_cast<double>(Delta.x) * Facing38c.x;
        if (!(Along < 0.0))
            return false;
        // FCOMP 200.0f (the field), TEST AH,0x41 + JNZ -> range test on
        // not-greater. Along is negative here, so that jump is always taken and the block
        // (a second cache refresh, -Facing38c, the routine / the routine /
        // ) is unreachable in baseline; it is not reproduced.
        break;
    }
    case 8:
        if (Flag39a == 0 && ApproachClearedFlag334 == 0)
            return false;                                            // -> the branch
        break;
    case 2:
        break;                                                       // caseD_0 062c4710
    default:
        return true;                                                 // caseD_2 / JA
    }

    // The range test (the branch and 062c4710-062c4746).
    IObjRW* const Watched = ObjRwFromWatchable(const_cast<Watchable*>(Watcher324.target()));
    float const Range = static_cast<float>(1000.0 * 1.05);           // FLD f the field, FMUL double
    return IsWithinInterceptRangeOfIface(&Sys->SelfTarget, Watched, Range);
}

// ----------------------------------------------------------------------------
// (item 1284), Common, module storage through
// the `RET` at module storage, 76 instructions, read end to end this pass.

// PORTED FOR REAL 2026-09-17 (claude:orch-dockpreamble). It had been
// `static void DockBehavior::AdvancePreamble() {}`, and `static` was itself the
// defect rather than a side effect of one: baseline's second instruction is
// `MOV ESI,ECX` and the body then reads SEVEN of this class's fields
// (+0x14, +0x24, +0x30, +0xa8, +0x168, +0x450, +0x46c). A receiverless
// declaration cannot express any of that, so the empty body was structurally
// locked in and no amount of grepping for stubs would have suggested otherwise.
// The one call site, UpdateApproachTail()'s first line, runs every tick.

// THE OLD COMMENT'S READING WAS WRONG, not merely incomplete. It said the
// function "reads MaxApproachSpeed46c and .rdata:the field, i.e. it is the
// timeout countdown". Nothing here counts down. The function computes this tick's
// LINEAR and ANGULAR speed caps and hands them to the three Idea sub-objects:

//   LinearCap  = MaxApproachSpeed46c                      ; 062c47fc
//   AngularCap = the field == 0x447a0000 == 1000.0f    ; 062c47f6
//   if (ComputeFormationFollowerEnvelope(Field24 + 0x1364, &Envelope))   ; 062c4839
//   {
//       LinearCap  = min(LinearCap,  Envelope.Speed);        ; 062c4842-062c4855
//       AngularCap = min(AngularCap, Envelope.AngularSpeed); ; 062c4859-062c486c
//   }
//   if (HeaderBlockFromWatched(ProxWatcher450) != null)                  ; 062c4870-062c4885
//       LinearCap = min(LinearCap, ProxWatcherComputeSpeedCap(...));     ; 062c4887-062c48be
//   SubObjectA.SetThrottleScale(LinearCap);    ; 062c48c7  LEA ECX,[ESI+0x30]
//   SubObjectB.SetThrottleScale(AngularCap);   ; 062c48d4  LEA ECX,[ESI+0xa8]
//   SubObjectC.SetThrottleScale(LinearCap);    ; 062c48e0  LEA ECX,[ESI+0x168]

// That is also why the +0x46c field is renamed MaxApproachSpeed46c this pass:
// this is its only reader in the DLL, and it uses the value as a speed.

// BOTH MIN-CLAMPS ARE `TEST AH,0x5` + `JNP` (062c484c/062c484f and
// /062c4866). Per this module's x87 table the JNP fires on strictly-less,
// and the jump SKIPS the assignment, so the incumbent is kept only when it is
// already the smaller -- i.e. `x = min(x, y)`. Naming the mask without the jump
// would invert both of these, which is exactly how this cluster has produced real
// defects before.

// THE PROXIMITY CAP IS CALLED TWICE IN BASELINE (062c4898 and 062c48b9) with
// byte-identical receivers and arguments -- the register shuffle before each push
// differs, the pushed values do not. That is MSVC's `x = min(x, f(...))` when f
// returns in ST0: `FCOMP` POPS the value it compares, so the compiler has nothing
// left to store and recomputes it. Both calls are reproduced (2026-10-08 cmn-ai10):
// the compare uses the unrounded first result, the store the rounded second one.
// The gate on it is `TEST AH,0x41` + `JZ` at 062c48a3: ZF is set when neither C3
// (equal) nor C0 (less) survives the mask, i.e. when the returned cap is strictly
// GREATER than the incumbent -- and that is the case that jumps past the store.
void DockBehavior::AdvancePreamble()
{
    // the angular cap starts at this module's standing "no limit"
    // sentinel, the same the field EnterState() seeds MaxApproachSpeed46c with.
    float LinearCap = MaxApproachSpeed46c;
    float AngularCap = 1000.0f;

    // baseline zeroes the out-struct's first four dwords and lets
    // the callee write the trailing byte, so Flag10 is deliberately left alone.
    FormationFollowerEnvelope Envelope;
    Envelope.Speed = 0.0f;
    Envelope.Throttle = 0.0f;
    Envelope.AngularSpeed = 0.0f;
    Envelope.AngularThrottle = 0.0f;

    // ECX == Field24 + 0x1364, the GunConvergenceState blob.
    // This caller reads only +0x00 and +0x08; GotoBehavior::Update reads the other
    // three.
    if (((SystemManager*)Field24)->GunConvergenceStateBytes.ComputeFormationFollowerEnvelope(&Envelope))
    {
        if (!(LinearCap < Envelope.Speed))
            LinearCap = Envelope.Speed;
        if (!(AngularCap < Envelope.AngularSpeed))
            AngularCap = Envelope.AngularSpeed;
    }

    // the same "-8 header" idiom as the five sites above --
    // `MOV EAX,[ESI+0x450]; TEST EAX,EAX; JZ; ADD EAX,-8; TEST EAX,EAX; JZ`.
    if (HeaderBlockFromWatched(ProxWatcher450.Watcher.target()) != nullptr)
    {
        // PUSH 0x3f800000 (1.0f), PUSH 1, PUSH [ESI+0x24],
        // PUSH [ESI+0x14]. Last push is the first argument, so OwnerContext comes
        // from +0x14 and NavHelper from +0x24 -- matching the declaration in
        // common_gotobehavior.h rather than the offset order. baseline calls it TWICE
        // (an inlined min): 062c4898 FCOMPs the unrounded result against LinearCap,
        // `TEST AH,0x41; JZ` skips only when it is strictly greater, and 062c48b9
        // calls again with the same arguments and FSTPs that result.
        if (!(ProxWatcherComputeSpeedCap(reinterpret_cast<unsigned char*>(&ProxWatcher450),
                                         ThreatSourcePtr14, Field24, 1, 1.0f) >
              static_cast<double>(LinearCap)))
        {
            LinearCap = static_cast<float>(ProxWatcherComputeSpeedCap(
                reinterpret_cast<unsigned char*>(&ProxWatcher450), ThreatSourcePtr14, Field24, 1, 1.0f));
        }
    }

    // . The linear cap goes to BOTH move-style Ideas; only the
    // FaceIdea gets the angular one.
    SubObjectA.SetThrottleScale(LinearCap);
    SubObjectB.SetThrottleScale(AngularCap);
    SubObjectC.SetThrottleScale(LinearCap);
}

// (module storage-062c5a6d), ported 2026-10-08 (claude-code:cmn-ai5). Builds the
// final-approach Catmull-Rom segment at +0x348 and gives it to SubObjectC. Every component is
// one float operation stored through the routine (Vec3Assign) or the routine (Vector add),
// so plain float arithmetic matches; only the two lengths stay on the FPU through FSQRT and
// the scale, and are kept in double.
void DockBehavior::BuildFinalApproachSpline()
{
    SystemManager* const Sys = static_cast<SystemManager*>(Field24);

    // FormationOffset starts zero. When the routine on Sys+0x1364 finds no
    // formation leader, it is the owner's get_formation_offset (+0xac) rotated into world space
    // by the 3DMathEngine's TransformMatrix (+0x2c) with ExternalIface's get_transform().
    Vector FormationOffset;
    FormationOffset.x = 0.0f;
    FormationOffset.y = 0.0f;
    FormationOffset.z = 0.0f;
    // the routine: null when the +0xc5 gate byte is set, else the +0xc watcher's
    // object (raw - 8, null when raw is 0).
    bool const bHasLeader = Sys->GunConvergenceStateBytes.GetFormationLeader() != nullptr;
    if (!bHasLeader)
    {
        ObjInspectOf(ThreatSourcePtr14)->get_formation_offset(FormationOffset);
        Transform const& ShipXform = Sys->SelfTarget.Iface->get_transform();
        Vector Rotated;
        Acquire3DMathEngine()->TransformMatrix(reinterpret_cast<FVector&>(Rotated),
                                               reinterpret_cast<FMatrix const&>(ShipXform),
                                               reinterpret_cast<FVector const&>(FormationOffset));
        FormationOffset = Rotated;
    }

    // refresh the cached centre (the routine), P1 = centre + offset.
    NavHelperRefreshLeadPoint();
    Spline.P1.x = Sys->SelfTarget.CachedPos.x + FormationOffset.x;
    Spline.P1.y = Sys->SelfTarget.CachedPos.y + FormationOffset.y;
    Spline.P1.z = Sys->SelfTarget.CachedPos.z + FormationOffset.z;

    // P2 = (DestPoint3cc + Field444) + offset.
    Vector DockPoint;
    DockPoint.x = DestPoint3cc.x + Field444.x;
    DockPoint.y = DestPoint3cc.y + Field444.y;
    DockPoint.z = DestPoint3cc.z + Field444.z;
    Spline.P2.x = DockPoint.x + FormationOffset.x;
    Spline.P2.y = DockPoint.y + FormationOffset.y;
    Spline.P2.z = DockPoint.z + FormationOffset.z;

    // |P2 - P1| twice (the same difference is rebuilt), summed on the FPU
    // as (z*z + y*y) + x*x, FSQRT, times 0.5f (the field) and 2.5f (the field).
    Vector Span;
    Span.x = Spline.P2.x - Spline.P1.x;
    Span.y = Spline.P2.y - Spline.P1.y;
    Span.z = Spline.P2.z - Spline.P1.z;
    double const SpanLength = std::sqrt(static_cast<double>(Span.z) * Span.z
                                        + static_cast<double>(Span.y) * Span.y
                                        + static_cast<double>(Span.x) * Span.x);
    float const LeadIn = static_cast<float>(SpanLength * 0.5);
    float const LeadOut = static_cast<float>(SpanLength * 2.5);

    // the ship's forward axis, rotation column 2 of ExternalIface's
    // get_transform(), through the routine (negate) and back (FCHS) -- an exact identity.
    Transform const& Xform = Sys->SelfTarget.Iface->get_transform();
    Vector Forward;
    Forward.x = Xform.Rotation.E[0][2];
    Forward.y = Xform.Rotation.E[1][2];
    Forward.z = Xform.Rotation.E[2][2];

    // P0 = (P1 + Forward * LeadIn) + offset.
    Vector Ahead;
    Ahead.x = Forward.x * LeadIn;
    Ahead.y = Forward.y * LeadIn;
    Ahead.z = Forward.z * LeadIn;
    Vector Start;
    Start.x = Ahead.x + Spline.P1.x;
    Start.y = Ahead.y + Spline.P1.y;
    Start.z = Ahead.z + Spline.P1.z;
    Spline.P0.x = Start.x + FormationOffset.x;
    Spline.P0.y = Start.y + FormationOffset.y;
    Spline.P0.z = Start.z + FormationOffset.z;

    // P3 = (P2 + Facing38c * LeadOut) + offset.
    Vector Behind;
    Behind.x = LeadOut * Facing38c.x;
    Behind.y = LeadOut * Facing38c.y;
    Behind.z = LeadOut * Facing38c.z;
    Vector End;
    End.x = Behind.x + Spline.P2.x;
    End.y = Behind.y + Spline.P2.y;
    End.z = Behind.z + Spline.P2.z;
    Spline.P3.x = End.x + FormationOffset.x;
    Spline.P3.y = End.y + FormationOffset.y;
    Spline.P3.z = End.z + FormationOffset.z;

    Spline.calculate_arclength();
    SubObjectC.SetSpline(&Spline);                                   // the routine
}

// Commonc4a72-062c4a98 (identical copies at 062c4b0e and 062c3e97).
// The SystemManager's physical-radius cache: +0x157a (Flags1578[2], == +0x14fc + 0x7e) set
// once, then ExternalIface->get_physical_radius (+0x2c) into CachedPhysicalRadius (+0x1514)
// and CachedPhysicalCenter (+0x1508). baseline tests neither Field24 nor ExternalIface; the two
// "DEVIATION" null guards that stood here are gone (2026-10-08).
void DockBehavior::NavHelperRefreshLeadPoint()
{
    SystemManager* const Sys = static_cast<SystemManager*>(Field24);
    if (Sys->SelfTarget.bPosValid != 0)
        return;
    Sys->SelfTarget.bPosValid = 1;
    Sys->SelfTarget.Iface->get_physical_radius(Sys->SelfTarget.CachedRadius,
                                                          Sys->SelfTarget.CachedPos);
}

// ---- The sub-object stand-ins are GONE (2026-09-17, claude:w21-stubsweep) ----

// EvaluateWatcher(SubObject, Delta) was a guessed name for the `CALL [<vtbl>+0x4]`
// this body makes nine times. Offset 0x4 is BaseIdea slot 1 == Update(float), and
// all three receivers (+0x30 MoveIdea, +0xa8 FaceIdea, +0x168 SplineMoveIdea) are
// typed members of this class with real, ported overrides. The stub returned the
// literal 1, so every `== 1` progress test in UpdateApproachTail() took the
// "manoeuvre complete" arm unconditionally. Call sites now dispatch for real.

// ResetWatcher(SubObject) was the same mistake at `CALL [<vtbl>+0x10]`, i.e.
// BaseIdea slot 4 == Commit(). Seven call sites, all now real.

// SetOffsetVector/SetMoveIdeaMode/SetMoveIdeaRadius/SetMoveIdeaEnabled/
// SetMoveIdeaAngle were five guessed names covering EIGHT distinct baseline
// functions, every one of them already ported in common_idea.h:
// -> MoveIdea::SetPosition       the routine -> FaceIdea::SetPosition
// -> MoveIdea::SetMode           the routine -> FaceIdea::SetDirection
// -> MoveIdea::SetRange3c      the routine -> FaceIdea::SetBasis
// -> MoveIdea::SetAngle38      the routine -> MoveIdea::Offset4c store
// The "SetMoveIdeaEnabled" name was wrong twice over: module storage writes
// MoveIdea+0x59 (SetField59), not the +0x58 byte SetEnabled writes.
// The names are deleted rather than kept, so nothing preserves the guess.

// .rdata:the field == 0x3fe3333333333333 == 0.6 (a double in baseline, and here).
double const DockBehavior::kEnterState9RadiusScale = 0.6;
// (module storage-062c2ade, RET 4), receiver the proximity watcher: FLD its
// far threshold (+0x0c) and, when the byte argument is set, FMUL its cruise ratio (+0x10).
// The caller FSTPs the result to a float, so one float rounding is the whole precision.
// (Was a stub returning 0.0f, 2026-10-08.)
float DockBehavior::QueryHandleRange(void* HandleField, int Mode)
{
    DockProximityWatcher const* const Watcher = static_cast<DockProximityWatcher const*>(HandleField);
    if (static_cast<unsigned char>(Mode) != 0)
        return static_cast<float>(static_cast<double>(Watcher->Unknown0c) * Watcher->Unknown10);
    return Watcher->Unknown0c;
}
// PlayApproachWarning (the routine) and RequestBehaviorSwitch (the routine) are
// GONE too (2026-09-17, claude:w21-stubsweep). Both were guessed names, and both
// targets are real: the routine is FaceIdea::SetAngleWeight8c (item 1632) and
// is SplineMoveIdea::SetField158 (item 1654) -- common_idea.h exposes
// both fields and both setters now, so the two call sites dispatch for real.

// (item 1798): the checked id lookup through the IObjDB at SystemManager+0x34,
// SystemManagerTimerTable::FindVerifiedObject (common_sysmgr_timertable.cpp).

// Slot 10, the routine (module storage-062c3587, RET 4). The argument is a
// pub::AI::DirectiveDockOp. Always returns true.
bool DockBehavior::Slot10(void* DirectiveOp)
{
    SystemManagerTimerTable* const ObjDb = &static_cast<SystemManager*>(Field24)->Timers;
    // REP MOVSD of 0xb dwords -- the whole op, vptr included.
    std::memcpy(static_cast<void*>(&DockOp), DirectiveOp, sizeof(DockOp));
    ModeState320 = 0;
    StateFlags32c = 0;
    // Watcher324 = the object DockOp.DockObjectId names.
    IObjRW* const Dock = static_cast<IObjRW*>(
        ObjDb->FindVerifiedObject((unsigned const*)&DockOp.DockObjectId));
    Watcher324.set(Dock != nullptr ? static_cast<Watchable const*>(Dock) : nullptr);
    IObjRW* const Watched = ObjRwFromWatchable(const_cast<Watchable*>(Watcher324.target()));
    if (Watched == nullptr)
    {
        // .
        Flags28 = static_cast<unsigned char>((Flags28 & 0xfd) | 0x1);
        return true;
    }

    // an object docking at that base replaces it (the routine), only when
    // one is found and it differs.
    IObjRW* const Docking = ObjDb->FindObjectDockingAtBase(Watched);
    if (Docking != nullptr)
        Watcher324.set(static_cast<Watchable const*>(Docking));

    // +0x32c = the routine(the watched object).
    StateFlags32c = GetObjectTypeOrZero(
        ObjRwFromWatchable(const_cast<Watchable*>(Watcher324.target())));
    // +0x388 = the object DockOp.Field_10 names (stored raw).
    TargetShipPtr388 = ObjDb->FindVerifiedObject((unsigned const*)&DockOp.Field_10);
    ApproachClearedFlag334 = DockOp.Field_14;
    Field408 = -1.0f;
    if (DockOp.Field_14 != 0)
    {
        Field330 = DockOp.DockPortIndex;
        Field43c = DockOp.DockPortIndex;
        RefreshApproachState();                                     // the routine
    }
    else
    {
        SubStateValue440 = 0;
        Field330 = -1;
    }

    // the proximity watcher on the object DockOp.Field_1C names, with the
    // op's +0x20/+0x24 floats; its flag words cleared.
    IObjRW* const Prox = static_cast<IObjRW*>(
        ObjDb->FindVerifiedObject((unsigned const*)&DockOp.Field_1C));
    ProxWatcher450.Unknown08 = DockOp.Field_20;
    ProxWatcher450.Unknown0c = DockOp.Field_24;
    ProxWatcher450.Watcher.set(Prox != nullptr ? static_cast<Watchable const*>(Prox) : nullptr);
    ProxWatcher450.Flags14 = 0;
    ProxWatcher450.SavedFlags18 = 0;
    IObjRW* const ProxObj = ObjRwFromWatchable(const_cast<Watchable*>(ProxWatcher450.Watcher.target()));
    if (ProxObj != nullptr)
    {
        // op+0x28 as a float against 0.0 (double the field); TEST
        // AH,0x41 + JNZ -> not greater takes the speed-ratio branch.
        float const Ratio = *reinterpret_cast<float const*>(&DockOp.Field_28);
        if (Ratio > 0.0)
        {
            ProxWatcher450.Unknown10 = Ratio;
        }
        else
        {
            // own ship (SystemManager::Watcher target - 8) get_speed(1.0f, 4),
            // rounded to float, divided by the prox object's get_speed(1.0f, 1) (FDIVR).
            SystemManager* const Sys = static_cast<SystemManager*>(Field24);
            IObjRW* const Self = ObjRwFromWatchable(const_cast<Watchable*>(Sys->Watcher.target()));
            float const OwnSpeed = Behavior_InvokeManagerSlotD8(
                ObjInspectOf(Self), 1.0f, IObject::ThrustEquipCruise);      // PUSH 0x4
            double const ProxSpeed = Behavior_InvokeManagerSlotD8(
                ObjInspectOf(ProxObj), 1.0f, IObject::ThrustEquipEngine);   // PUSH 0x1
            ProxWatcher450.Unknown10 = static_cast<float>(OwnSpeed / ProxSpeed);
        }
        // TEST AH,0x5 + JP keeps the value on >= 1.0f or unordered.
        if (ProxWatcher450.Unknown10 < 1.0f)
            ProxWatcher450.Unknown10 = 1.0f;
    }
    // .
    Flags28 = static_cast<unsigned char>((Flags28 & 0xfe) | 0x2);
    return true;
}

// Slot 21, the routine (module storage-062c2fd5, RET 4): release Watcher324 and then the
// proximity watcher when their object's id equals the argument (the object id the manager's
// loop at module storage passes).
void DockBehavior::Slot21(void* Arg)
{
    unsigned int const LeftId = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(Arg));
    IObjRW* const Watched = ObjRwFromWatchable(const_cast<Watchable*>(Watcher324.target()));
    if (Watched != nullptr && Watched->get_id() == LeftId)          // slot 0x20
        Watcher324.set(nullptr);
    IObjRW* const Prox = ObjRwFromWatchable(const_cast<Watchable*>(ProxWatcher450.Watcher.target()));
    if (Prox != nullptr && Prox->get_id() == LeftId)                // slot 0x20
        ProxWatcher450.Watcher.set(nullptr);
}

// DockBehavior::SetDestPointOffset -- formerly the routine (the dissolved common_stubs.cpp), Common
// `MOV EAX,[param_1]; MOV [this+0x444],EAX` x3, RET 4 -- the three dwords of the
// argument copied into Field444. Its single call site (module storage, BaseFollowerBehavior's
// vtable slot 0x10 the routine) passes ECX = *(ESI+0x278) = BaseFollowerBehavior::DockDelegate,
// which the routine loads from IBehaviorManager::BehaviorDispatchTable[7] (+0x190, behaviour
// type 7 "Dock", GDescDock) -- a DockBehavior; and 0x062c50c0 sits inside DockBehavior's own
// address run (0x062c2ad0-0x062c56d0). That settles the receiver the old stub comment left open.
void DockBehavior::SetDestPointOffset(Vector const& Offset)
{
    Field444 = Offset;
}
