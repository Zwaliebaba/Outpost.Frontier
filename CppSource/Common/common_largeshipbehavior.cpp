// Author: Giperion.

#include "common_largeshipbehavior.h"
#include "common_aisteer.h"        // ComputeWeaponSlotStat (item 1846), FAiTarget, GetCachedRange, RaySphereIntersect
#include "common_iustategraph.h"   // BuildAiTargetList / GetAiTargetList (item 1800)
#include "common_systemmanager.h"  // SystemManager, FAiTargetVector
#include "common_ini.h"            // Vec3Assign (the routine, item 763)
#include "common_aisteer_internal.h" // CmnAiSteer::InlineFastLength

#include <cmath>  // sqrtf, for the two places baseline uses a real FSQRT rather than the bit-hack

// LargeShipBehavior -- see common_largeshipbehavior.h for the full RTTI/layout writeup this
// pass relied on. Ported from Common (raw analysis is authoritative in
// this module over analysis's Common analysis -- see this marathon's "ground truth
// precedence" rule).

// (Common) -- constructor. Calls the shared base-init
// field-initialising the routine (it writes +0x04..+0x29, see Behavior in common_behavior.h) first, then default-constructs a MoveIdea and a FaceIdea for real
// (SubObjectA/SubObjectB, RTTI-identified, common_idea.h), sets Field2c, and zeroes
// Field168/Field16c.
LargeShipBehavior::LargeShipBehavior(int InTypeId)
    : Behavior()
    , TypeId(InTypeId)
    , CachedHandle08(0)
    , ModeSelector0xc(0)
    , UpdateResult10(0)
    , ThreatSourcePtr14(nullptr)
    , Opaque18{}
    , DeltaTime1c(0.0f)
    , BehaviorMgrPtr20(nullptr)
    , NavHelperPtr24(nullptr)
    , Flags28(0)
    , Flag29(0)
    , Pad2a{}
    , Field2c(1500.0f)
    , SubObjectA()
    , SubObjectB()
    , Field168(0.0f)
    , Field16c(0.0f)
    , Field170{}
{
    ConstructBase(InTypeId);   // Behavior's own field initialisation
    // baseline default-constructs SubObjectA/SubObjectB here (the routine @ SubObjectA,
    // @ SubObjectB) -- now real MoveIdea()/FaceIdea() constructor calls via the
    // member-initializer list above (common_idea.h), not a deferred stub.
}

// Real (non-thunk) destructor path. the routine (Common) is the
// scalar-deleting-dtor thunk: it calls the routine (the REAL cleanup body, 206959-206996)
// then conditionally frees. the routine clears two BaseWatcher-shaped sub-fields at +0xbc
// and +0x44 -- +0xbc is SubObjectB's own +0x14 (SubObjectB.Watcher) and +0x44 is SubObjectA's
// own +0x14 (SubObjectA.Watcher), now real MoveIdea/FaceIdea members (common_idea.h) rather
// than opaque byte ranges -- before tail-calling the shared the routine(this,-1) effect.
// baseline order confirmed from the raw bytes (Common): SubObjectB's own
// Watcher is unwatched first, then SubObjectA's.
LargeShipBehavior::~LargeShipBehavior()
{
    SubObjectB.Watcher.set(nullptr);
    SubObjectA.Watcher.set(nullptr);
    // Then the routine == ~Behavior (vptr reset only), which the compiler emits.
}

// Own vtable slot 1 (offset 0x04). Common (the branch), 45
// instructions -- same two-sub-object call shape as LaunchBehavior::SetContext
// (common_launchbehavior.cpp).
bool LargeShipBehavior::SetContext(void* Owner, void* Manager, void* Sys)
{
    // BL = the routine(Owner, Manager, Sys). baseline does NOT test BL before the
    // Idea calls below -- they run with whatever +0x14/+0x24 hold -- and returns BL at the end.
    // (The old port returned false early, skipping them.)
    bool const bBound = BindContext(Owner, Manager, Sys);

    // MoveIdea slot 0 with (this->+0x14, this->+0x24, Manager arg).
    // Return value discarded.
    SubObjectA.SetContext(ThreatSourcePtr14, NavHelperPtr24, Manager);

    // baseline: `CALL the routine(&SubObjectA, 1)`. Correctly observed to be a direct CALL and
    // not a vtable dispatch -- but that is exactly what MoveIdea's setter family IS: baseline
    // calls every one of them non-virtually by raw address (common_idea.h's "/OPT:ICF trap"
    // section). module storage is MoveIdea::SetEnabled(bool), writing a single byte to
    // MoveIdea+0x58. Real call as of 2026-09-17; the stub is gone.
    SubObjectA.SetEnabled(true);

    // baseline: SubObjectB's own vtbl slot 0 (SetContext), SAME arg shape as SubObjectA's own
    // call above (Commoncff7a-062cff8f).
    SubObjectB.SetContext(ThreatSourcePtr14, NavHelperPtr24, Manager);

    return bBound;                                                  // MOV AL,BL
}

// Slot 3, the branch (module storage-062cffea).
void LargeShipBehavior::Slot3()
{
    Behavior::Slot3();                                              // the routine

    // baseline (module storage-062cffb3): `PUSH 0x40490fdb (pi); LEA ECX,[ESI+0x30];
    // CALL TextRenderContext::set_origin_h`. ESI+0x30 is SubObjectA, and module storage is
    // MoveIdea::SetAngle38(float) -- the analysis symbol is an /OPT:ICF fold owned by an
    // unrelated, separately-ported class (common_idea.h's fold table names it explicitly).
    // Real call as of 2026-09-17; the SetApproachOrientationAngle stub is gone. The literal is
    // the exact bit pattern 0x40490fdb, i.e. this restores MoveIdea's own ctor default of pi.
    SubObjectA.SetAngle38(3.14159274f);

    // the routine(0, 3) == Behavior::SetCruiseLock (the manager's PathfindManager).
    SetCruiseLock(0, 3);

    // baseline (module storage-062cffe3): `LEA EAX,[ESP+0x8]; PUSH EAX; LEA ECX,[ESP+0x8];
    // PUSH ECX; ECX = NavHelperPtr24 + 0x12c; CALL the routine; TEST AL,AL; JZ skip;
    // Field2c = [ESP+0x4]`. the routine is item 1846,
    // SystemManagerGunGroupManager::ComputeGunRangeStats(OutAverage, OutMax). Push order re-derived from the raw bytes: EAX (&OutMax, the higher local) is pushed
    // first, ECX (&OutAverage, the lower local) second, so OutAverage is argument 1; the value
    // copied into Field2c afterwards is that same lower local, i.e. the AVERAGE, not the max.
    // Real call as of 2026-09-17.
    float OutAverage = 0.0f;
    float OutMax = 0.0f;
    if (NavHelperPtr24->GunGroups.ComputeGunRangeStats(&OutAverage, &OutMax))
        Field2c = OutAverage;
}

// Own vtable slot 4 (offset 0x10). Common (the routine), 13
// instructions, fully self-contained aside from the two "release" sub-object vcalls and the
// shared tail effect.
void LargeShipBehavior::Slot4()
{
    // baseline: `CALL [SubObjectB_vtbl+0x10]()` then `CALL [SubObjectA_vtbl+0x10]()` (Common
    // ) -- confirmed THISCALL, zero explicit args, vtable BYTE offset +0x10 ==
    // slot index 4 == BaseIdea::Commit() (common_idea.h). Same family convention as
    // LaunchBehavior::Disengage()'s own SubObjectA/B usage (baseline calls SubObjectB first, then
    // SubObjectA -- order preserved here).
    SubObjectB.Commit();
    SubObjectA.Commit();

    // JMP the routine == Behavior::Slot4 (PathfindManager::Field_064 = 0, if any;
    // Flag29 = 0). The old port did only the Flag29 half.
    Behavior::Slot4();
}

// Own vtable slot 5 (offset 0x14). Common (the branch), 36
// instructions.
int LargeShipBehavior::Slot5(float Value)
{
    (void)Value;   // RET 4, argument unread
    int Mode = 2;

    // baseline (module storage-062d000a): `ECX = ThreatSourcePtr14; EAX = [ECX]; LEA EDX,[ESP+8];
    // PUSH EDX; CALL [EAX+0x88]; TEST dword ptr [ESP+8], 0x80000`. CLOSED 2026-09-17: vtbl+0x88
    // is IObjInspect::get_type(uint& OutType), slot 34 of the baseline-order table rebuilt in
    // common_iface.h. ThreatSourcePtr14 was never an unidentified class -- SetContext() above
    // already reads get_id() off this very field through ObjInspectOf(). Note the mask is
    // tested against the OUT-PARAMETER; get_type's int return value is discarded by baseline.
    unsigned ObjectType = 0;
    ObjInspectOf(ThreatSourcePtr14)->get_type(ObjectType);
    if ((ObjectType & 0x80000) != 0)
    {
        // baseline (module storage-062d0024): `ECX = NavHelperPtr24 + 0x142c;
        // CALL the routine; TEST EAX,EAX`. CLOSED 2026-09-17 -- see ResolveNavHelperTarget().
        if (NavHelperPtr24->Targeting.GetTarget() != nullptr)
            Mode = 1;
    }

    if (Flag29 != 0)
    {
        ModeSelector0xc = 3;
        return 3;
    }
    ModeSelector0xc = Mode;
    return Mode;
}

// (item 1891) is TargetManager::GetTarget (common_targetmanager.cpp); the private
// ResolveNavHelperTarget copy that stood here, with a null guard baseline lacks, is gone.


// ============================================================================
// Update()'s FAiTarget helpers.

// Update() manipulates FAiTarget records (common_aisteer.h, 0x88 bytes) through
// three shapes baseline either calls by raw address or inlines. All three belong
// on FAiTarget itself, but common_aisteer.h is another lane's file this wave, so
// they are reproduced here with internal linkage -- nothing new is published.
// See this file's tracker note (item 1469) for the request to promote them.

// THE "UNMODELLABLE SCRATCH RECORD" WAS A STACK-OFFSET MISTAKE. The disposition
// this pass replaced said Update()'s on-stack scratch was "a memoizing object
// inspection cache of the unported ~0x45c-byte AI maneuver/docking executor
// whose layout this port has no type for", handed to two helpers "at a DIFFERENT
// base, ESP+0x58 vs ESP+0x60 -- two embedded sub-objects of one cache record".
// There is one record, not two, and it is an ordinary FAiTarget:
//   * /062d0085 PUSH the two arguments BEFORE module storage's
//     `LEA ECX,[ESP+0x60]`, so that LEA resolves to the same address as
// 's `LEA ECX,[ESP+0x58]`. The prior trace read the LEA
//     against the pre-push ESP and so saw two bases eight bytes apart.
//   * With the single base, every offset is FAiTarget's, exactly:
//     [ESP+0x60] = +0x08 CachedFlags (get_type's out-param AND the 0x280100
//     test), [ESP+0xd5] = +0x7D bFlagsValid, [ESP+0xdc] = +0x84 Iface,
//     [ESP+0x64..0x6c] = +0x0C CachedPos.
//   * The scan block below then walks BuildAiTargetList's vector of the same
//     record with a literal `ADD ESI,0x88` stride and the same +0x08/+0x0C/
//     +0x18/+0x7D/+0x7E/+0x84 accesses.
// ============================================================================

// (FAiTarget::Init: clears the four cache guards +0x7C..+0x7F, stores
// AttitudeRefIface +0x80 and Iface +0x84; argument order from this file's call site, PUSH
// Handle, PUSH ThreatSourcePtr14) and the routine (FAiTarget::RefreshPos: guard +0x7E raised
// BEFORE get_physical_radius) are the FAiTarget members in common_aitarget.cpp.

// The lazily-cached type-flags fetch baseline INLINES at six sites in Update()
// (module storage, 062d0307, 062d0331, 062d0353, 062d06c9, 062d06f3). Same
// shape as FAiTarget::RefreshPos, through slot 34 get_type(unsigned&). baseline sets
// the guard AFTER the call here (module storage then 062d00da) -- reproduced.
static void RefreshAiTargetFlags(FAiTarget& Self)
{
    if (Self.bFlagsValid != 0)
    {
        return;
    }
    Self.Iface->get_type(Self.CachedFlags);   // no null test, as baseline
    Self.bFlagsValid = 1;
}

// , and again per-iteration at module storage-062d02e3
// and module storage-062d06a3: `EAX = *(void**)(Sys + 8); Self = EAX ? EAX - 8
// : nullptr`. SystemManager+0x08 is BaseWatcher Watcher and the -8 is the
// documented Watchable-sub-object adjustment (Watchable is IObjRW's third base,
// at IObjRW+8), so this is the owning ship's own IObjInspect. baseline re-tests
// the adjusted pointer against zero as well; that second test can only fire on
// a literal 0x8 sentinel, so it folds into the one guard -- same reduction
// common_iustategraph.cpp's RecoverObjInspect makes.
static IObjInspect const* OwnerObjInspectOf(SystemManager* Sys)
{
    Watchable const* const Watched = Sys->Watcher.target();
    if (Watched == nullptr)
    {
        return nullptr;
    }
    unsigned char* const Raw = reinterpret_cast<unsigned char*>(const_cast<Watchable*>(Watched)) - 8;
    return static_cast<IObjInspect const*>(reinterpret_cast<IObjRW*>(Raw));
}

// baseline inlines the 0x5f3759df fast length three times in Update() (062d03b2-062d0403,
// , 062d07e1-062d080b): the squared length stays on the x87 stack, only its
// FST float image feeds the magic, and the Newton step ((y*y*S)*0.5f, 1.5f - that, * y) and the
// final * S run on the unrounded S -- CmnAiSteer::InlineFastLength, kept in double.
static double FastLengthFromSquared(double LengthSq)
{
    return CmnAiSteer::InlineFastLength(LengthSq);
}

// ----------------------------------------------------------------------------
// Own vtable slot 7 (offset 0x1c), Update/dispatch entry point. Common
// 207132-207857 (the branch), 684 instructions, NO analysis inspect -- hand-
// inspected from the raw listing only.

// CLOSED 2026-09-17 (claude:common-buzz-largeship). Every instruction in the
// range is now compiled code; the "dense avoidance-scan block left FULLPORT-
// DEFERRED as one unit" disposition that stood here through four passes rested
// on three claims and all three were stale or wrong:
//   (1) "the routine (item 1800) is still unported, and every value the block
//       computes is built from it." Item 1800 has been `ready_unverified` since
//       earlier the same day as BuildAiTargetList (common_iustategraph.h): it
//       returns the SystemManager timer table's embedded std::vector<FAiTarget>.
//   (2) "the block fetches this ship's own position via an unidentified
//       NavHelperPtr24-nested accessor." It does not -- module storage-062d01b5
//       is `SystemManager::Watcher (+0x08) -> -8 Watchable adjust ->
//       IObjInspect::get_position()`, the same three-step idiom
//       common_iustategraph.cpp already uses.
//   (3) "an on-stack cache record whose layout this port has no type for."
//       A stack-offset mistake -- see the FAiTarget::Init note above.
// Nothing had to be fabricated: every value in the block is an FAiTarget field,
// an IObjInspect slot call, or arithmetic over the two.

// NAVHELPERPTR24 IS A SystemManager*. Two independent offsets settle it and
// both are asserted in common_systemmanager.h rather than assumed here:
// reads its +0x08 as a BaseWatcher (offsetof(SystemManager,
// Watcher) == 0x08) and module storage passes its +0x34 to BuildAiTargetList,
// whose parameter type IS SystemManager+0x34 (offsetof(SystemManager, Timers)
// == 0x34, and common_iustategraph.h's GetAiTargetList is literally
// `&Sys->Timers`). The field keeps its `void*` declaration so the sibling
// methods that read its +0x142c/+0x12c need not be rewritten this wave; the
// cast is made here, once, against those two asserts.

// A LIVE CONTROL-FLOW BUG WAS FIXED. The byte at `[ESP+0x27]` written by the
// Field16c clamp was dismissed here as "confirmed, by re-reading the complete
// raw analysis range, never read again anywhere in this function". It IS
// read -- at module storage, spelled `[ESP+0x2b]`, because `PUSH EBP` at
// shifts every later ESP-relative displacement in the scan
// branch by four. It gates the whole second half of the scan
// (module storage `JZ the branch`): when Field16c had not yet run down to
// zero this tick, baseline averages the hostile-contact positions and stops,
// committing both ideas instead of steering. Modelled for real below as
// bField16cExpired.
// ----------------------------------------------------------------------------
int LargeShipBehavior::Update()
{
    // baseline: module storage-062d0080.
    IObjRW* const Handle = NavHelperPtr24->Targeting.GetTarget();
    if (Handle == nullptr)
    {
        UpdateResult10 = 2;
        return 2;
    }

    // baseline: module storage-062d008a. The scratch FAiTarget lives at
    // [ESP+0x58]; AttitudeRefIface is the behaviour's own threat source and
    // Iface is the nav-helper's current target.
    // Zero-initialised where baseline leaves the record's unused slices
    // indeterminate -- the same deliberate, documented deviation item 1800's
    // own port makes for this struct. Every field the code below reads is set
    // by FAiTarget::Init or by one of the two lazy refreshes.
    FAiTarget Scratch = {};
    Scratch.Init(ObjInspectOf(ThreatSourcePtr14), ObjInspectOf(Handle));

    // baseline: module storage-062d00b9. Field16c is a cooldown timer decremented
    // every tick and clamped at zero (compared against the field, confirmed
    // bit-exact 0.0 double). The clamp also raises the local flag the scan
    // block below consumes -- see this function's header comment.
    bool bField16cExpired = false;
    Field16c -= DeltaTime1c;
    if (Field16c <= 0.0f)
    {
        Field16c = 0.0f;
        bField16cExpired = true;
    }

    // baseline: module storage-062d00ea. 0x280100 is an ordinary object-type mask.
    RefreshAiTargetFlags(Scratch);
    if ((Scratch.CachedFlags & 0x280100) != 0)
    {
        // baseline: module storage-062d0170. Steer straight at the threat and
        // hold that decision for 30 seconds.
        Scratch.RefreshPos();
        Vector const ThreatPos = Scratch.CachedPos;

        SubObjectA.SetRange3c(500.0f);  // , bit-exact
        SubObjectA.SetMode(0);
        SubObjectA.SetScale40(0.6f);    // , bit-exact
        SubObjectB.SetPosition(ThreatPos);
        SubObjectA.SetPosition(ThreatPos);

        Field16c = 30.0f;  // , bit-exact
        UpdateResult10 = 1;
        return 1;  // baseline's own literal early RET at module storage -- this
                   // path does NOT reach the shared tail's tick calls
    }

    // baseline: module storage-062d018c. Field168 is a SEPARATE cooldown gating
    // the avoidance scan (compared against the field, bit-exact 0.0f).
    Field168 -= DeltaTime1c;
    if (Field168 < 0.0f)
    {
        // baseline: module storage-062d019e.
        Field168 = 5.0f;  // , bit-exact

        SystemManager* const Sys = NavHelperPtr24;

        // baseline: module storage-062d01cb. The owning ship's own world position.
        // `raw ? raw - 8 : 0`, get_position (slot 0) with no null test, as baseline.
        Vector const SelfPos = OwnerObjInspectOf(Sys)->get_position();

        // baseline: module storage-062d01f8. BuildAiTargetList rebuilds (or
        // reuses, within one IBehaviorManager tick) the scanner's candidate
        // list -- see common_iustategraph.h.
        Vector HostileCentre = {0.0f, 0.0f, 0.0f};
        int HostileCount = 0;
        FAiTargetVector* const List = BuildAiTargetList(GetAiTargetList(Sys));

        // ---- Pass 1, module storage-062d025d: sum the positions of every
        // ---- candidate whose cached attitude is hostile enough.
        for (FAiTarget* Elem = List->Begin; Elem != List->End; ++Elem)
        {
            float Attitude = 0.0f;
            float AttitudeHigh = 0.0f;
            GetCachedRange(Elem, &Attitude, &AttitudeHigh);

            // `FCOMP the field(-0.6f); TEST AH,0x41; JP skip` -- the mask
            // leaves one bit set for both "less" and "equal", so the body runs
            // on <=, and baseline's own threshold here is -0.6f (not the -0.2f
            // this cluster's other consumer uses). Iface is dereferenced with no
            // null test, as baseline (062d0222).
            if (Attitude <= -0.6f)
            {
                // baseline dispatches slot +0x10 (get_orientation); slots 3 and 4
                // are one folded address in baseline's table (common_iface.h),
                // and what it reads off the result is Transform+0x24, so the
                // Transform-typed spelling is the accurate one here.
                Vector const& Pos = Elem->Iface->get_transform().Translation;
                HostileCentre.x += Pos.x;
                HostileCentre.y += Pos.y;
                HostileCentre.z += Pos.z;
                ++HostileCount;
            }
        }

        bool bBlocked = false;  // byte [ESP+0x17], module storage
        if (HostileCount > 0)
        {
            // baseline: module storage-062d029e. Average the contacts: FILD count, FDIVR 1.0f,
            // and the unrounded reciprocal multiplies each sum (one float rounding each).
            double const InvCount = 1.0 / static_cast<double>(HostileCount);
            HostileCentre.x = static_cast<float>(HostileCentre.x * InvCount);
            HostileCentre.y = static_cast<float>(HostileCentre.y * InvCount);
            HostileCentre.z = static_cast<float>(HostileCentre.z * InvCount);

            // baseline: module storage. The gate the prior pass called dead.
            if (bField16cExpired)
            {
                // ---- Pass 2, module storage-062d0461: average the positions of
                // ---- every nearby candidate of one of three exact types.
                Vector NearbyCentre = {0.0f, 0.0f, 0.0f};
                int NearbyCount = 0;
                for (FAiTarget* Elem = List->Begin; Elem != List->End; ++Elem)
                {
                    // `raw ? raw - 8 : 0`; neither pointer is tested.
                    IObjInspect const* const Owner = OwnerObjInspectOf(Sys);
                    // baseline: module storage-062d0301, candidate id first.
                    unsigned const CandId = Elem->Iface->get_id();
                    unsigned const SelfId = Owner->get_id();
                    if (CandId == SelfId)
                    {
                        continue;
                    }

                    // baseline: module storage-062d0373. Three exact-equality
                    // tests on the whole flags dword (CMP, not TEST), each
                    // preceded by its own copy of the lazy refresh.
                    RefreshAiTargetFlags(*Elem);
                    if (Elem->CachedFlags != 0x80000)
                    {
                        RefreshAiTargetFlags(*Elem);
                        if (Elem->CachedFlags != 0x100000)
                        {
                            RefreshAiTargetFlags(*Elem);
                            if (Elem->CachedFlags != 0x200000)
                            {
                                continue;
                            }
                        }
                    }

                    // baseline: module storage-062d03ad.
                    Vector const& Pos = Elem->Iface->get_transform().Translation;
                    Vector Delta = {};
                    Vec3Assign(&Delta, Pos.x - SelfPos.x, Pos.y - SelfPos.y, Pos.z - SelfPos.z);

                    // baseline: module storage-062d040c. (X*X + Z*Z) + Y*Y on the FPU; TEST AH,0x5
                    // + JP skips on >= or unordered.
                    double const DistSq = (static_cast<double>(Delta.x) * Delta.x
                                           + static_cast<double>(Delta.z) * Delta.z)
                                        + static_cast<double>(Delta.y) * Delta.y;
                    if (!(FastLengthFromSquared(DistSq) < 1500.0))  // , bit-exact
                    {
                        continue;
                    }

                    ++NearbyCount;  // baseline increments before the refresh
                    Elem->RefreshPos();
                    NearbyCentre.x += Elem->CachedPos.x;
                    NearbyCentre.y += Elem->CachedPos.y;
                    NearbyCentre.z += Elem->CachedPos.z;
                }

                if (NearbyCount > 1)
                {
                    // baseline: module storage-062d0553. Push away from the crowd:
                    // aim 500 units along the reverse of the crowd's bearing.
                    // the unrounded 1/n times each sum, minus SelfPos, one
                    // float rounding per component (FSTP into Vec3Assign's arguments).
                    double const InvNearby = 1.0 / static_cast<double>(NearbyCount);
                    Vector AwayDir = {};
                    Vec3Assign(&AwayDir,
                               static_cast<float>(NearbyCentre.x * InvNearby - SelfPos.x),
                               static_cast<float>(NearbyCentre.y * InvNearby - SelfPos.y),
                               static_cast<float>(NearbyCentre.z * InvNearby - SelfPos.z));

                    // a real FSQRT of (x*x + z*z) + y*y, FDIVR 1.0f. x*inv stays on
                    // the FPU and is scaled by 500 there; y*inv and z*inv are FSTP'd (into AwayDir)
                    // before their own * 500.
                    double const InvLen = 1.0 / std::sqrt((static_cast<double>(AwayDir.x) * AwayDir.x
                                                           + static_cast<double>(AwayDir.z) * AwayDir.z)
                                                          + static_cast<double>(AwayDir.y) * AwayDir.y);
                    double const PushX = (AwayDir.x * InvLen) * 500.0;             // , bit-exact
                    float const NormY = static_cast<float>(AwayDir.y * InvLen);
                    float const NormZ = static_cast<float>(AwayDir.z * InvLen);
                    double const PushY = static_cast<double>(NormY) * 500.0;
                    double const PushZ = static_cast<double>(NormZ) * 500.0;
                    // HostileCentre - push, one rounding each.
                    Vector Goal = {};
                    Vec3Assign(&Goal,
                               static_cast<float>(HostileCentre.x - PushX),
                               static_cast<float>(HostileCentre.y - PushY),
                               static_cast<float>(HostileCentre.z - PushZ));
                    Field170 = Goal;
                }
                else
                {
                    // baseline: module storage-062d056f.
                    Field170 = HostileCentre;
                }

                // baseline: module storage-062d05b2. Hold the goal within +/-50
                // units of our own altitude (the field = 50.0f, bit-exact).
                // Two one-sided FCOM/FSTP pairs, i.e. a clamp.
                // The sums stay on the FPU for the FCOM: 062d0584 TEST AH,0x5 + JP stores only on
                // strictly-less, 062d05a5 TEST AH,0x41 + JNZ only on strictly-greater.
                double const Ceiling = static_cast<double>(SelfPos.y) + 50.0;
                if (Ceiling < Field170.y)
                {
                    Field170.y = static_cast<float>(Ceiling);
                }
                double const Floor = static_cast<double>(SelfPos.y) - 50.0;
                if (Floor > Field170.y)
                {
                    Field170.y = static_cast<float>(Floor);
                }

                // baseline: module storage-062d0643. If the goal is already close
                // enough, spend no further effort this tick and fall straight
                // through to the shared tail.
                Vector ToGoal = {};
                Vec3Assign(&ToGoal,
                           Field170.x - SelfPos.x,
                           Field170.y - SelfPos.y,
                           Field170.z - SelfPos.z);
                // (x*x + z*z) + y*y on the FPU; TEST AH,0x5 + JNP leaves on strictly-less.
                double const ToGoalSq = (static_cast<double>(ToGoal.x) * ToGoal.x
                                         + static_cast<double>(ToGoal.z) * ToGoal.z)
                                      + static_cast<double>(ToGoal.y) * ToGoal.y;
                if (FastLengthFromSquared(ToGoalSq) < 100.0)  // , bit-exact
                {
                    SubObjectB.Update(DeltaTime1c);
                    SubObjectA.Update(DeltaTime1c);
                    UpdateResult10 = 1;
                    return 1;
                }

                // baseline: module storage-062d068c. Re-aim the ray at the goal
                // and reset the running best hit distance.
                Vector RayDir = {};
                Vec3Assign(&RayDir,
                           Field170.x - SelfPos.x,
                           Field170.y - SelfPos.y,
                           Field170.z - SelfPos.z);
                float BestHit = 0.0f;

                // ---- Pass 3, module storage-062d0892: sweep the ray to the
                // ---- goal against every candidate and keep the nearest hit.
                for (FAiTarget* Elem = List->Begin; Elem != List->End; ++Elem)
                {
                    // `raw ? raw - 8 : 0`; neither pointer is tested.
                    IObjInspect const* const Owner = OwnerObjInspectOf(Sys);
                    // baseline: module storage-062d06c3 -- OWN id first here,
                    // the reverse of pass 2's order. Reproduced as written.
                    unsigned const SelfId = Owner->get_id();
                    unsigned const CandId = Elem->Iface->get_id();
                    if (CandId == SelfId)
                    {
                        continue;
                    }

                    // baseline: module storage-062d0713. A mask test this time,
                    // with an exact-equality fallback.
                    RefreshAiTargetFlags(*Elem);
                    if ((Elem->CachedFlags & 0xfdff) == 0)
                    {
                        RefreshAiTargetFlags(*Elem);
                        if (Elem->CachedFlags != 0x200000)
                        {
                            continue;
                        }
                    }

                    // baseline: module storage-062d0760. Renormalise the ray in
                    // place every qualifying iteration (idempotent after the
                    // first; reproduced rather than hoisted).
                    // (x*x + y*y) + z*z, FSQRT, FDIVR 1.0f; the unrounded reciprocal scales each
                    // component, one float rounding each.
                    double const RayInv = 1.0 / std::sqrt((static_cast<double>(RayDir.x) * RayDir.x
                                                           + static_cast<double>(RayDir.y) * RayDir.y)
                                                          + static_cast<double>(RayDir.z) * RayDir.z);
                    RayDir.x = static_cast<float>(RayDir.x * RayInv);
                    RayDir.y = static_cast<float>(RayDir.y * RayInv);
                    RayDir.z = static_cast<float>(RayDir.z * RayInv);

                    // baseline: module storage-062d0794.
                    Elem->RefreshPos();
                    float const Limit = Elem->CachedRadius + 500.0f;  // , bit-exact
                    float Hit = 0.0f;
                    Elem->RefreshPos();  // baseline's own second, redundant refresh

                    // baseline: module storage-062d0818. Already inside the
                    // candidate's clearance envelope -> latch "blocked" and
                    // stop steering this tick.
                    // the differences are never stored (all three stay on the
                    // FPU), summed (z*z + y*y) + x*x; TEST AH,0x5 + JP: only strictly-less blocks.
                    double const DX = static_cast<double>(SelfPos.x) - Elem->CachedPos.x;
                    double const DY = static_cast<double>(SelfPos.y) - Elem->CachedPos.y;
                    double const DZ = static_cast<double>(SelfPos.z) - Elem->CachedPos.z;
                    if (FastLengthFromSquared((DZ * DZ + DY * DY) + DX * DX) < Limit)
                    {
                        bBlocked = true;
                        continue;
                    }

                    // baseline: module storage-062d0881. the routine is item
                    // 1712, RaySphereIntersect (common_aisteer.h). NOTE: baseline
                    // passes the radius BY ADDRESS (`LEA EDX,[ESP+0x30]` at
                    // ; the callee does `MOV EAX,[ESP+0x10]; FLD
                    // [EAX]` at module storage). That header declares the
                    // parameter as a plain `float`, so the port is internally
                    // consistent and computes the same thing, but the baseline
                    // signature is `float const&`. Reported, not edited -- see
                    // this item's tracker note.
                    Elem->RefreshPos();  // baseline's own third refresh
                    if (!RaySphereIntersect(SelfPos, RayDir, Elem->CachedPos, Limit, &Hit))
                    {
                        continue;
                    }
                    // `TEST AH,0x44; JNP` is the equality idiom: take the hit
                    // outright the first time, then only when it is nearer.
                    if (BestHit == 0.0f || Hit < BestHit)
                    {
                        BestHit = Hit;
                    }
                }

                // baseline: module storage-062d08fa. Pull the goal back to the
                // first thing the ray actually hits.
                // x and y products stay on the FPU until the add; z's is
                // FSTP'd first.
                if (BestHit != 0.0f)
                {
                    Field170.x = static_cast<float>(static_cast<double>(RayDir.x) * BestHit + SelfPos.x);
                    Field170.y = static_cast<float>(static_cast<double>(RayDir.y) * BestHit + SelfPos.y);
                    float const ScaledZ = static_cast<float>(static_cast<double>(RayDir.z) * BestHit);
                    Field170.z = static_cast<float>(static_cast<double>(ScaledZ) + SelfPos.z);
                }

                // baseline: module storage-062d0958. the branch is also the
                // direct destination of module storage (candidate list empty),
                // which is why baseline re-tests bBlocked at module storage even
                // though the module storage test has already sent every
                // blocked tick to the commit path. Reproduced as written.
                if (!bBlocked)
                {
                    SubObjectA.SetRange3c(250.0f);  // , bit-exact
                    SubObjectA.SetMode(0);
                    SubObjectA.SetScale40(0.6f);    // , bit-exact
                    SubObjectB.SetPosition(Field170);
                    SubObjectA.SetPosition(Field170);
                    Field16c = 30.0f;               // , bit-exact
                }

                if (bBlocked)
                {
                    // baseline: module storage-062d0971, slot 4 == BaseIdea::Commit.
                    SubObjectA.Commit();
                    SubObjectB.Commit();
                }
            }
            else
            {
                // baseline: the module storage branch into the branch.
                SubObjectA.Commit();
                SubObjectB.Commit();
            }
        }
        else
        {
            // baseline: the module storage branch into the branch.
            SubObjectA.Commit();
            SubObjectB.Commit();
        }
    }

    // baseline: module storage-062d09a4 (the branch, the shared tail). Ticks
    // SubObjectB then SubObjectA through slot 1 == BaseIdea::Update(float);
    // both return values are discarded (EAX is overwritten with the literal 1
    // at module storage).
    SubObjectB.Update(DeltaTime1c);
    SubObjectA.Update(DeltaTime1c);

    UpdateResult10 = 1;
    return 1;
}

// ----------------------------------------------------------------------------
// , formerly a FULLPORT-DEFERRED stand-in that always
// returned false. Its stated blocker -- an on-stack cache record with no type --
// was a stack-offset mistake (see the FAiTarget::Init note above), so the probe
// is now three lines of real code inside Update() itself and this function is
// gone. Left as a tombstone so the next pass does not go looking for it.
// ----------------------------------------------------------------------------
