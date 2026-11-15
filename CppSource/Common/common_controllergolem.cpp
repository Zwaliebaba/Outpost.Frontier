// Author: Giperion.

#include "common_physys.h"

#include "common_archetype.h"
#include "common_cobject.h"
#include "common_cobjdb.h"   // IObjDB::GetZoneDb (PhySys::GenerateCollisions)
#include "common_id.h"
#include "common_universe.h" // Universe::ISystem (PhySys::GenerateCollisions)
#include "common_net.h"  // IsMPServer -- CRemotePhysicsSimulation::tracef (module storage)
#include "common_phyarch.h"
#include "common_misc.h"     // SinglePlayer
#include "common_server.h"

#include "neuroncomponents.h"
#include "NeuronRuntime.h"
#include "x86math.h"  // I3DMathEngine::MatrixToQuaternion, FQuaternion, FMatrix
#include "../IVP/ivp_collision/ivp_collision.h" // IVP_Collision::get_objects (PhyCollisionStateManager)
#include "../IVP/ivp_physics/ivp_core.h"
#include "../IVP/ivp_physics/ivp_real_object.h"
#include "../IVP/ivp_physics/ivp_environment.h"
#include "../IVP/ivp_physics/ivp_controller.h"
#include "../IVP/ivp_physics/ivp_material.h"
#include "../IVP/ivp_physics/ivp_listener.h"
#include "../IVP/ivp_physics/ivp_mindist_settings.h"
#include "../IVP/ivp_physics/ivp_mindist.h" // IVP_Synapse_Real (PhySys_CalcClosestPointOnSynapse)
#include "common_csys.h"  // Acquire3DMathEngine
// PhySys::CalcClosestPointOnSynapse (the routine, near GetClosestPhantomIntruderPos
// below) walks baseline's compact half-edge mesh directly -- IVP_Compact_Edge /
// IVP_Compact_Triangle / IVP_Compact_Ledge and IVP_U_Float_Point.
#include "../IVP/ivp_compact_builder/ivp_compact_ledge.h"
#include "../IVP/ivp_compact_builder/ivp_surface_builder_pointsoup.h" // Cleanup, the box phantom's hull
#include "../IVP/ivp_compact_builder/ivp_surface_builder_ledge_soup.h" // the box phantom's surface
#include "../IVP/ivp_physics/ivp_surman_polygon.h" // PhySys::PhySurfaceManager's base
#include "../IVP/ivp_collision/ivp_ray_solver.h"   // PhySys::RaySolver_MaskedSurface's base
#include "../IVP/ivp_collision/ivp_sphere_query.h" // PhysicalSystem::FindSphereCollisions (06350740)

#include <assert.h>
#include <float.h>   // FLT_MAX (PhySys::GetClosestPhantomIntruderPos, the field)
#include <math.h>
#include <new>
#include <stdarg.h>  // va_list -- CRemotePhysicsSimulation::tracef is __cdecl varargs
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include <mmsystem.h>  // timeGetTime (PhysicalSystem::Update, module storage); winmm is linked by common_playeraccount.cpp


// Win32 macro conflict: PhySys::GetCurrentTime vs windows.h GetCurrentTime()
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include "common_physys_internal.h"

using namespace CmnPhySys;

// ============================================================================
// FLControllerGolem -- the force controller at CObject+0x60
// ============================================================================

// CLASS NAME IS RTTI-CONFIRMED, not inferred. The leaf vftable stamped over the object at
// is module storage; its Complete Object Locator pointer sits at [0639c1c8-4]
// = module storage -> 063a70e0; that COL's +0xC type-descriptor pointer (module storage) is
// , whose name field is ".?AVFLControllerGolem@@" (Common).
// implementation tracker items 2729-2743 all call this cluster "PhySys::Golem" -- that name predates
// anyone reading the COL and is simply wrong. Corrected on those items by this pass.

// VTABLE (8 slots, module storage-0639c1e4). The slot indices are settled by a symbol, not by
// guesswork: PhySys::Controller's own vftable at module storage carries the exported symbol
// ?get_controller_priority@Controller@PhySys@@... at module storage, which is 0639b60c+0x14,
// i.e. SLOT 5. Both tables are the same IVP_Controller shape:
//   [0] +0x00  core_is_going_to_be_deleted_event(IVP_Core*)
//              golem: the branch -- `TEST ECX,ECX; MOV EAX,[ECX]; PUSH 1; CALL [EAX+0x18]`,
//              i.e. this->vftable[6](1) == "scalar deleting destructor, do free". Delete-self.
//   [1] +0x04  the branch, inherited verbatim from PhySys::Controller's slot 1 (FLD of a
//              fixed .rdata float). Not overridden, not ported.
//   [2] +0x08  get_associated_controlled_cores() -- golem: the branch, `MOV EAX,the field;
//              RET`, a fixed global vector. IVP_Controller_Independent's (ivp_controller.cpp).
//   [3] +0x0c  golem: the branch (overrides a RET 0x8 no-op that PhySys::Controller inherits
//              ICF-folded as JustifyNode::Execute). Rebases/normalises +0x64/+0x68 by an
//              IVP_Time; ported 2026-09-25 as IVP_Controller_Golem::reset_time below.
//   [4] +0x10  do_simulation_controller -- golem: the branch, 345 instructions, the actual
//              per-tick force/torque integrator. PORTED 2026-09-17 by lane
//              claude:w19-integrator as FLControllerGolem::do_simulation_controller below;
//              the whole module storage-0634357b range was read instruction by instruction.
//              What is still missing is only the DISPATCH -- see that method's own comment.
//   [5] +0x14  get_controller_priority() -- golem: the branch, `MOV EAX,0x1f4; RET` == 500,
//              IVP_Controller_Motion's (ivp_controller.cpp); slot [0] is Motion's too.
//              (PhySys::Controller's own returns 0x5dc == 1500 == IVP_CP_MOTION.)
//   [6] +0x18  scalar deleting destructor -- golem: the routine.
//   [7] +0x1c  golem-only. The intermediate base's table (module storage) has _purecall
//              (the branch) here, so that base is abstract and FLControllerGolem is the
//              concrete leaf implementing it (the branch). IDENTIFIED 2026-09-17: it is the
//              integrator's "give up and teleport" escape hatch, called from slot 4 at
// and module storage. the branch is 5 instructions --
//              `MOV EAX,[ESP+4]; PUSH EAX; CALL the routine; XOR EAX,EAX; RET 8`, i.e. it
//              forwards only its FIRST argument (the IVP_Event_Sim*) and drops the second
//              (the 0/1 flag slot 4 passes), then returns 0. the routine
//              (module storage-06343142) ignores that forwarded argument entirely and is
//              `the routine(ECX=this->Body, param_1=&this->CurrentTargetQuat,
//              param_2=&this->CurrentTargetPosition, param_3=1)` -- IVP's
//              IVP_Real_Object::beam_object_to_new_position. Ported as beam_to_target below,
//              minus that one unported callee.

// NOTE FOR WHOEVER OWNS src/IVP/ivp_physics/ivp_controller.h (outside this lane's scope): that
// header places get_controller_priority at slot 1 and flags a conflict with common_physys.h's
// claim of slot 5. The vftable dump above settles it in favour of slot 5 -- the symbol
// ?get_controller_priority@Controller@PhySys@@ is literally at 0639b60c+0x14. Its slot-2
// get_associated_controlled_cores reading is correct; only its slot-1 guess is wrong.

// FLControllerGolemParms itself is declared in common_physys_internal.h (PhySys::RCBegin builds
// one too).

// + the routine: the two-stage default constructor of the tuning struct.
void CmnPhySys::FLControllerGolemParms_Init(FLControllerGolemParms& Parms)
{
    // (module storage-06342c7a): REP STOSD of 9 dwords over +0x00..+0x23 first,
    // then the non-zero defaults.
    Parms.ForceFactor = 0.8f;
    Parms.DampFactor = 1.0f;
    Parms.TorqueFactor = 0.8f;
    Parms.AngularDampFactor = 1.0f;
    Parms.MaxTranslationForce[0] = 1.0e6f;
    Parms.MaxTranslationForce[1] = 1.0e6f;
    Parms.MaxTranslationForce[2] = 1.0e6f;
    Parms.Field_0x1c = 0.0f;
    Parms.MaxTorque = 1.0e6f;

    // (module storage-06343041).
    Parms.MaxDeltaPosition = 10.0f;
    Parms.DeltaOrientation = 1.5707964f;
    Parms.Field_0x2c = 10.0f;
    Parms.Field_0x30 = 1.0e20f;
    Parms.Field_0x34 = 5.0f;
}

// IVP_Event_Sim (the per-PSI context every controller's slot 4, do_simulation_controller,
// receives) is declared in src/IVP/ivp_physics/ivp_controller.h next to IVP_Controller, with
// the layout evidence (the routine builds it, the routine dispatches it).
static_assert(offsetof(IVP_Event_Sim, i_delta_time) == 0x04, "IVP_Event_Sim+0x04");
static_assert(offsetof(IVP_Event_Sim, environment) == 0x08, "IVP_Event_Sim+0x08");

// IVP_Controller_Golem is DECLARED in src/IVP/ivp_physics/ivp_controller.h since 2026-09-24
// (claude-code:w6-rpc). Until then this file declared the whole thing as FLControllerGolem,
// one class; baseline has two -- the abstract IVP_Controller_Golem (vftable module storage,
// slot 7 _purecall) and the leaf FLControllerGolem (module storage, slot 7 the branch) --
// and a second leaf, CRemotePhysicsController (module storage), built on the same base. The
// split is what lets CRemotePhysicsController (src/include/common_server.h) exist at all.
// Since 2026-09-25 (claude-code:h-ivp) the chain above the golem is baseline's too:
// IVP_Controller_Golem : IVP_Controller_Motion : IVP_Controller_Independent : IVP_Controller
// (CHDs 063a70d0 / 063a6af8). Motion owns +0x04..+0x5c and its constructor/destructor are
// / the routine (src/IVP/ivp_physics/ivp_controller.cpp); the golem layer owns
// +0x60..+0xcc. Offsets below are BASELINE offsets and they hold exactly in this port too:
// IVP_Controller and IVP_Controller_Independent are vtable-only (no data members), so the
// vptr occupies +0x00 and the first member lands at +0x04, just as in baseline -- hence the
// real offsetof static_asserts at the bottom.

// FIELD NOTES (the declarations themselves are in ivp_controller.h):

// +0x04 / +0x14 belong to IVP_Controller_Motion (vftable 063a53b4). the routine
// zeroes +0x04/+0x08/+0x0c and +0x14/+0x18/+0x1c and sets +0x20 to 1.0f -- i.e. a zero
// vector and an identity quaternion. Motion's own slot 4 (the branch) servoes to them as
// they stand; the golem's slot 4 recomputes them first (below).

// NAMED 2026-09-17 (they were Field_0x04 / Field_0x14, "name them when slot 4 is ported").
// Slot 4 is ported now and it both WRITES and READS them every tick: it recomputes them at
// (+0x4/+0x8/+0xc = TargetPosition + dt*TargetSpeed) and
// (+0x14 = TargetQuat, or the slerp toward TargetRotQuat), then
// reads them back at module storage/0634321e/0634322c (position error) and
// /0634343b-06343457 (orientation error and the sign dot product). They are
// the target pose EXTRAPOLATED to this instant, which is why they are separate from the
// TargetPosition/TargetQuat the setters store.
// DEVIATION: baseline never initialises +0x10 (the IVP_U_Point hesse/pad slot) at all -- see
// the write list at module storage-06342cb7, which skips it. Zeroed here.
//   IVP_U_Point CurrentTargetPosition;  // +0x04
//   IVP_U_Quat CurrentTargetQuat;       // +0x14

//   IVP_U_Point MaxTranslationForce;   // +0x24  <- parms+0x10..+0x18 (x,y,z); pad +0x30 unwritten
//   IVP_U_Point MaxTorque;             // +0x34  <- parms+0x20 broadcast to x,y,z; pad +0x40 unwritten
//   float ForceFactor;                 // +0x44  <- parms+0x00
//   float DampFactor;                  // +0x48  <- parms+0x04
//   float TorqueFactor;                // +0x4c  <- parms+0x08
//   float AngularDampFactor;           // +0x50  <- parms+0x0c
//   IVP_Environment* Environment;      // +0x54  <- Body->environment   (IVP_Object+0x18)
//   IVP_Real_Object* Body;             // +0x58  <- the body itself
//   IVP_Core* Core;                    // +0x5c  <- Body->core          (IVP_Real_Object+0x98)

// +0x60. RETYPED 2026-09-17 from `void* Field_0x60` ("zeroed by the ctor, never read by
// ported code"). It is a FLOAT and slot 4 reads it every tick:
// FLD  [ESI+0x60]
// FMUL [.rdata:the field]      ; raw bytes 66 66 66 3F == 0.9f
// FADD ST0,ST1                    ; ST1 = |position error|^2
// FSTP [ESI+0x60]
// -- an exponentially-decaying running sum of the squared position error. The ctor's zero
// (module storage) is a float 0.0f, which has the same bits as the null pointer it used to
// be typed as, so nothing else changes.
// OPEN TODO: nothing in baseline's golem cluster READS this accumulator back within the
// traced range (module storage-0634357b writes it and never reads it again; the setters
// and both constructors do not touch it). Its consumer is presumably outside this cluster
// and was not found this pass -- kept faithfully rather than optimised away.
//   float SmoothedPositionErrorSq;     // +0x60

// +0x64 / +0x6c. baseline copies an IVP_Time here as EIGHT RAW BYTES, two dwords, with no
// float arithmetic at all (module storage-0634358c: `MOV EDX,[EAX]; MOV [ECX+0x64],EDX;
// MOV EAX,[EAX+4]; MOV [ECX+0x68],EAX`).

// IMPORTANT FOR WHOEVER PORTS THE INTEGRATOR (vftable slot 4, the branch): that function
// reads the pair as TWO SEPARATE FLOATS, not as one double. module storage-06343185 is
//     FLD [env_time_lo]; FSUB [ESI+0x64]; FADD [env_time_hi]; FSUB [ESI+0x68]
// i.e. (lo - this->+0x64) + (hi - this->+0x68) -- IVP's split coarse/fine time
// representation, summed to recover the elapsed interval. So baseline's IVP_Time is a float
// pair, NOT the `double Value` that common_physys.h currently models it as.

// RESOLVED 2026-09-17 by lane claude:w19-integrator: IVP_Time is now declared as
// that float pair (see common_physys.h for the evidence, and for which half of it actually
// discriminates -- the "GetCurrentTime copies two raw dwords" observation does NOT, since
// a blind 8-byte copy is also what returning a struct{double} looks like; the golem's x87
// FSUB pair and this very +0x64 offset are what settle it). The old alignment hazard is
// gone with it: a float pair is 4-byte aligned and sits at +0x64 happily.

// Kept as plain `float[2]` rather than an IVP_Time member anyway, so that the
// offsets this class is asserted against cannot be perturbed by anyone re-editing that
// struct again. [0] is time_base, [1] is time_offset, matching ::IVP_Time
// (src/IVP/ivp_physics/ivp_core.h) field for field.
//   float TargetPositionTime[2];       // +0x64  set by set_target_position
//   float TargetQuatTime[2];           // +0x6c  set by set_target_quaternion
//   float InvRotDeltaTime;             // +0x74  1.0f default, else 1.0f/DeltaTime
//   int bHasTargetRotQuat;             // +0x78  0/1
//   IVP_U_Quat TargetQuat;             // +0x7c  identity at construction
//   IVP_U_Quat TargetRotQuat;          // +0x8c  identity at construction
//   IVP_U_Point TargetPosition;        // +0x9c  zeroed; pad +0xa8 unwritten
//   IVP_U_Point TargetSpeed;           // +0xac  zeroed; pad +0xb8 unwritten
//   float MaxDeltaPosition;            // +0xbc  <- parms+0x24
//   float DeltaOrientation;            // +0xc0  <- parms+0x28
//   float Field_0xc4;                  // +0xc4  <- parms+0x2c (10.0f in practice)
//   float Field_0xc8;                  // +0xc8  <- parms+0x30 (1e20f in practice)
//   float Field_0xcc;                  // +0xcc  <- parms+0x34 (5.0f in practice)

// The leaf. baseline: vftable module storage stamped at module storage right after
// returns (the leaf constructor is inlined into the routine); scalar deleting
// destructor the routine, whose body is a jump into the routine (module storage) -- the
// leaf destructor adds nothing of its own.
class FLControllerGolem : public IVP_Controller_Golem
{
public:
    FLControllerGolem(IVP_Real_Object* InBody, FLControllerGolemParms const& Parms)
        : IVP_Controller_Golem(InBody, Parms)
    {
    }

    virtual ~FLControllerGolem() {}

    // (module storage-0628895c), baseline vftable slot 7.
    virtual int beam_to_target(IVP_Event_Sim const* EventSim, int Reason) override;
};

static_assert(sizeof(IVP_Time) == 8, "IVP_Time is the 8 bytes baseline copies verbatim");
static_assert(sizeof(FLControllerGolem) == 0xd0, "FLControllerGolem is operator_new(0xd0) at module storage");
static_assert(offsetof(IVP_Controller_Motion, CurrentTargetPosition) == 0x04, "golem+0x04");
static_assert(offsetof(IVP_Controller_Motion, CurrentTargetQuat) == 0x14, "golem+0x14");
static_assert(offsetof(IVP_Controller_Motion, MaxTranslationForce) == 0x24, "golem+0x24");
static_assert(offsetof(IVP_Controller_Motion, MaxTorque) == 0x34, "golem+0x34");
static_assert(offsetof(IVP_Controller_Motion, ForceFactor) == 0x44, "golem+0x44");
static_assert(offsetof(IVP_Controller_Motion, AngularDampFactor) == 0x50, "golem+0x50");
static_assert(offsetof(IVP_Controller_Motion, Environment) == 0x54, "golem+0x54");
static_assert(offsetof(IVP_Controller_Motion, Body) == 0x58, "golem+0x58");
static_assert(offsetof(IVP_Controller_Motion, Core) == 0x5c, "golem+0x5c");
static_assert(offsetof(IVP_Controller_Golem, SmoothedPositionErrorSq) == 0x60, "golem+0x60");
static_assert(offsetof(IVP_Controller_Golem, TargetPositionTime) == 0x64, "golem+0x64");
static_assert(offsetof(IVP_Controller_Golem, TargetQuatTime) == 0x6c, "golem+0x6c");
static_assert(offsetof(IVP_Controller_Golem, InvRotDeltaTime) == 0x74, "golem+0x74");
static_assert(offsetof(IVP_Controller_Golem, bHasTargetRotQuat) == 0x78, "golem+0x78");
static_assert(offsetof(IVP_Controller_Golem, TargetQuat) == 0x7c, "golem+0x7c");
static_assert(offsetof(IVP_Controller_Golem, TargetRotQuat) == 0x8c, "golem+0x8c");
static_assert(offsetof(IVP_Controller_Golem, TargetPosition) == 0x9c, "golem+0x9c");
static_assert(offsetof(IVP_Controller_Golem, TargetSpeed) == 0xac, "golem+0xac");
static_assert(offsetof(IVP_Controller_Golem, MaxDeltaPosition) == 0xbc, "golem+0xbc");
static_assert(offsetof(IVP_Controller_Golem, Field_0xcc) == 0xcc, "golem+0xcc");

// Slot 2 (the branch, the shared the field vector) is IVP_Controller_Independent's since
// 2026-09-25 -- see ivp_controller.h/.cpp; the golem no longer declares it.

// (IVP_Controller_Manager::ensure_core_to_be_in_simulation) and the routine
// (IVP_Environment::add_revive_core) are IVP_Controller_Manager_Ensure_Core_To_Be_In_Simulation
// / IVP_Environment_Enqueue_Core in src/IVP/ivp_physics/ivp_environment.cpp.

// (module storage-063430fb, RET 0x8). Its first act is to hand BOTH of its
// arguments, unchanged, to the IVP_Controller_Motion constructor the routine
// (module storage-0634305e: `EAX = param_1; EDI = param_2; PUSH EDI; PUSH EAX; CALL`) -- the
// base-class initialiser below; the routine's share is ported as IVP_Controller_Motion's
// constructor in src/IVP/ivp_physics/ivp_controller.cpp. Then it stamps its own vftable
// (module storage, the compiler's job here) and fills +0x60..+0xcc.
IVP_Controller_Golem::IVP_Controller_Golem(IVP_Real_Object* InBody, FLControllerGolemParms const& Parms)
    : IVP_Controller_Motion(InBody, Parms)
{
    // ---- the routine's own share (module storage-063430f2) ----
    SmoothedPositionErrorSq = 0.0f;          // (a dword 0; see the field's note)
    InvRotDeltaTime = 1.0f;                  // (0x3f800000)
    bHasTargetRotQuat = 0;

    TargetQuat.x = 0.0f;
    TargetQuat.y = 0.0f;
    TargetQuat.z = 0.0f;
    TargetQuat.w = 1.0f;
    TargetRotQuat.x = 0.0f;
    TargetRotQuat.y = 0.0f;
    TargetRotQuat.z = 0.0f;
    TargetRotQuat.w = 1.0f;

    // Very strange, but baseline: the fourth floats (+0xa8, +0xb8) are not written; neither
    // does the port.
    TargetPosition.set(0.0f, 0.0f, 0.0f);    // /063430ac/063430b2
    TargetSpeed.set(0.0f, 0.0f, 0.0f);       // /063430be/063430c4

    MaxDeltaPosition = Parms.MaxDeltaPosition;
    DeltaOrientation = Parms.DeltaOrientation;
    Field_0xc4 = Parms.Field_0x2c;
    Field_0xc8 = Parms.Field_0x30;
    Field_0xcc = Parms.Field_0x34;

    // Very strange, but baseline: +0x64..+0x70 (the two IVP_Time stamps) are not written here;
    // every creator primes them right away with a set_target_position / set_target_quaternion
    // pair (module storage / 062890b1, RCBegin likewise). The port does not write them either.
}

// (module storage-06343126): `MOV [ECX],063a53dc; JMP the routine`. It stamps
// this level's vftable (the compiler's job) and tail-chains the IVP_Controller_Motion
// destructor the routine, which is where IVP_Core::detach_controller lives
// (ivp_controller.cpp). The golem layer destroys nothing of its own, so the body is empty and
// the base destructor call the compiler emits IS the JMP.
// Its scalar deleting wrapper is the routine (063a53dc[6]); the leaves use their own,
// (FLControllerGolem) and the routine (CRemotePhysicsController).
IVP_Controller_Golem::~IVP_Controller_Golem()
{
}

// Vftable slot 3, the branch (module storage-0634366b, RET 0x8) -- 063a53dc[3], inherited
// unchanged by FLControllerGolem (0639c1d4) and CRemotePhysicsController (0639ba44). Its one
// stack argument is an IVP_Time by value (see IVP_Controller::reset_time).
// It moves set_target_position's time stamp (+0x64/+0x68) back by Delta and re-normalises the
// split pair so that the fractional half ends up in [0, 1]:
// FLD [+0x64]; FSUB [ESP+4]; FSTP [+0x64]
// FLD [+0x68]; FSUB [ESP+8]; FST  [+0x68]
// FCOMP 1.0f; `TEST AH,0x41` + JNZ -- C3|C0 (<= or unordered) skips, so
//                      the carry loop runs on STRICTLY GREATER
// +0x64 += 1.0f; +0x68 -= 1.0f (FST); FCOMP 1.0f; same test, JZ back
// FLD [+0x68]; FCOMP 0.0f; `TEST AH,0x1` + JZ -- C0 (less or unordered)
//                      enters the borrow loop, hence !(x >= 0) rather than x < 0
// +0x64 -= 1.0f; +0x68 += 1.0f (FST); FCOMP 0.0f; same test, JNZ back
// = 1.0f and the field = 0.0f (raw .rdata bytes 00 00 80 3F / 00 00 00 00).
// TargetQuatTime (+0x6c/+0x70) is NOT touched -- only the position stamp is rebased.
// Precision: the compares at 06343604 / 06343629 / 0634365e test the x87 register FST has
// just stored from, i.e. the unrounded difference (double here); the entry compare of the
// borrow loop (06343636 FLD [+0x68]) re-loads the stored float.
void IVP_Controller_Golem::reset_time(IVP_Time Delta)
{
    TargetPositionTime[0] -= Delta.time_base;

    double Offset = (double)TargetPositionTime[1] - (double)Delta.time_offset;
    TargetPositionTime[1] = (float)Offset;                                     // FST

    while (Offset > 1.0)                                       // / 06343629-06343634
    {
        TargetPositionTime[0] += 1.0f;
        Offset = (double)TargetPositionTime[1] - 1.0;
        TargetPositionTime[1] = (float)Offset;                 // FST
    }

    if (!(TargetPositionTime[1] >= 0.0f))                      // (stored float)
    {
        do
        {
            TargetPositionTime[0] -= 1.0f;
            Offset = (double)TargetPositionTime[1] + 1.0;
            TargetPositionTime[1] = (float)Offset;             // FST
        } while (!(Offset >= 0.0));                            // (C0: less/unordered)
    }
}

// (module storage-063435e9, __thiscall, RET 0xc). Argument order comes from the
// call site, decoded last-push-first at module storage-0628909c: param_1 = the position triple
// at S+0x5c, param_2 = the triple explicitly zeroed at module storage-06289094, param_3 = the
// 8-byte time at S+0x20. The AnimDB::Update call site (module storage) matches.
void IVP_Controller_Golem::set_target_position(Vector const& InTargetPosition,
                                               Vector const& InTargetSpeed,
                                               IVP_Time const& Time)
{
    // an 8-byte verbatim copy, two dwords, no float arithmetic.
    // Now a field-for-field copy rather than a memcpy: IVP_Time is a float pair with
    // the same member order (see ivp_core.h), so this moves exactly
    // the same bytes baseline's two MOVs do, without relying on layout coincidence.
    TargetPositionTime[0] = Time.time_base;
    TargetPositionTime[1] = Time.time_offset;

    // . Same x87 trap as the constructor: FLD param+0x4 then FLD
    // param+0x0, so the first FSTP (to +0x9c) receives x and the second (to +0xa0) receives y;
    // z travels as an integer move through EDX.
    TargetPosition.set(InTargetPosition.x, InTargetPosition.y, InTargetPosition.z);

    // , identical shape into +0xac/+0xb0/+0xb4.
    TargetSpeed.set(InTargetSpeed.x, InTargetSpeed.y, InTargetSpeed.z);

    // the routine(this->Core), whose body is
    // `ECX = Core->Environment(+0xc); PUSH Core; CALL the routine` == "enqueue this core for
    // wakeup". The `MOV ECX,[EAX+0x90]` at module storage (EAX = this->Environment) is a dead
    // load -- the routine overwrites ECX from its own argument before using it.
    // No test on Core (module storage `MOV EDX,[ECX+0x5c]; ... PUSH EDX; CALL`); the
    // `Core != nullptr` guard that stood here was removed 2026-09-25 (claude-code:h-ivp).
    IVP_Controller_Manager_Ensure_Core_To_Be_In_Simulation(Core);
}

// (module storage-063436f1, __thiscall, RET 0x10). Call site module storage-
// param_1 = the re-ordered quaternion, param_2 = the same IVP_Time the position call
// got, param_3 = 0, param_4 = 0. AnimDB::Update (module storage) passes the same shape.
void IVP_Controller_Golem::set_target_quaternion(float const InTargetQuat[4],
                                                 IVP_Time const& Time,
                                                 float const* InTargetRotQuatOrNull,
                                                 float DeltaTime)
{
    // a straight 4-dword copy into +0x7c..+0x88. The source is in
    // IVP's (X,Y,Z,W) order, which is why it lands directly on IVP_U_Quat's x/y/z/w.
    TargetQuat.x = InTargetQuat[0];
    TargetQuat.y = InTargetQuat[1];
    TargetQuat.z = InTargetQuat[2];
    TargetQuat.w = InTargetQuat[3];

    // . The branch is on param_3 (the optional rotation-speed
    // quaternion), and the divisor is param_4, read off the stack as a float by the FDIV at
    // . the field is 1.0f -- the raw .rdata bytes at Common-
    // 568656 are 00 00 80 3F, i.e. 0x3F800000. (Items 2732/2740/2742 at one point guessed
    // 2*PI here; 2743's own note already corrected that, and this pass re-read the bytes.)
    if (InTargetRotQuatOrNull != nullptr)
    {
        TargetRotQuat.x = InTargetRotQuatOrNull[0];
        TargetRotQuat.y = InTargetRotQuatOrNull[1];
        TargetRotQuat.z = InTargetRotQuatOrNull[2];
        TargetRotQuat.w = InTargetRotQuatOrNull[3];
        bHasTargetRotQuat = 1;
        InvRotDeltaTime = 1.0f / DeltaTime;  // /0634369e/063436c3
    }
    else
    {
        bHasTargetRotQuat = 0;
        // baseline does NOT touch +0x74 on this path -- it keeps whatever the previous call or
        // the constructor's 1.0f left there. Deliberately not assigned here either.
    }

    // the time copy happens AFTER the branch, into +0x6c/+0x70 --
    // a different pair from set_target_position's +0x64/+0x68, so the two stamps are
    // independent even though both call sites pass the same IVP_Time.
    TargetQuatTime[0] = Time.time_base;
    TargetQuatTime[1] = Time.time_offset;

    // same the routine(this->Core) tail as set_target_position,
    // including the same dead `MOV ECX,[EAX+0x90]` load, and the same absence of a Core test
    // (guard removed 2026-09-25, claude-code:h-ivp).
    IVP_Controller_Manager_Ensure_Core_To_Be_In_Simulation(Core);
}

// ============================================================================
// baseline vftable slot 4, do_simulation_controller.
// ============================================================================
// PORTED 2026-09-17 by lane claude:w19-integrator. VERIFIED RANGE: the entire body,
// through module storage inclusive (Common, 345
// instructions counting the four epilogue tails), read instruction by instruction with the x87
// stack tracked across every FLD/FSTP/FXCH/FCOMPP. Every branch below cites the address that
// proves it.

// WHAT IT IS. A critically-damped PD servo that drags the golem's body onto an extrapolated
// target pose, expressed entirely as direct velocity edits on the core (baseline never touches
// Core->SpeedChange/RotSpeedChange here -- it writes Core->Speed at module storage/063433b9/
// and Core->RotSpeed at module storage/0634355f/0634356f). Per axis:
//     Speed    += (ForceFactor*posErr + DampFactor*dt*(targetVel - vel)) / dt
//     RotSpeed += TorqueFactor*rotErr/dt - DampFactor*RotSpeed
// with both increments clamped, per axis, to MaxTranslationForce / MaxTorque expressed as real
// forces and torques (see the two clamp loops), and with a hard bail-out to a teleport when
// either error grows past MaxDeltaPosition / DeltaOrientation.

// EVERY x87 GATE IN THIS FUNCTION, mask and jump named together:
// FCOMPP / module storage `TEST AH,0x41` + JNZ. 0x41 is C3|C0, i.e.
//     equal-or-less. FCOMPP compared ST0=|posErr|^2 against ST1=MaxDeltaPosition^2, so the JNZ
//     (the taken branch) is "error within tolerance, keep going" and the FALL-THROUGH is
//     STRICTLY GREATER -- that is the teleport path at module storage. Ported as `>`, which
//     also reproduces baseline on unordered operands (an unordered compare sets C3|C2|C0, so the
//     JNZ fires and baseline keeps going; `>` is false on NaN and likewise keeps going).
// `TEST AH,0x1` + JNZ, after module storage FCOMP MaxTranslationForce[i].
//     0x1 is C0 alone = STRICTLY LESS, and the JNZ skips the clamp. So the clamp runs on
//     >= (ported as `>=`, false on NaN, matching the JNZ-taken/skip behaviour).
// `TEST AH,0x1` + JZ, after module storage FCOMP 0.0f on the raw (unsigned)
//     delta. C0 = delta < 0; the JZ skips the FCHS, so the result is negated exactly when the
//     delta was negative -- a copysign, not a comparison against the bound.
// `TEST AH,0x1` + JZ, after module storage FCOMPP. Note the operands are the
//     REVERSE way round from the position gate: ST0 = DeltaOrientation^2 and ST1 =
//     |rotErr|^2, so C0 means DeltaOrientation^2 < |rotErr|^2 and the JZ (C0 clear) is the
//     keep-going branch. Fall-through into module storage is the teleport.
// `TEST AH,0x1` + JZ, after module storage FCOMP 0.0f on the quaternion dot
//     product. C0 = dot < 0. The JZ -- i.e. dot >= 0 -- lands on the branch, and the branch
//     is the branch that starts with FCHS. So the torque scale is NEGATED when the dot is
//     non-negative, not when it is negative; the two arms are otherwise instruction-identical
//     (module storage-06343499 vs module storage-063434c3).
// `TEST AH,0x1` + JNZ, after module storage FCOMP MaxTorque[i] against an
//     FABS'd COPY (module storage FLD ST0 duplicates first, so the signed original survives in
//     ST0 for the copysign two instructions later). Same shape as the force clamp.
// `TEST AH,0x1` + JZ, after module storage FCOMP 0.0f -- the torque copysign.

// RAW .rdata CONSTANTS, read as bytes (Common, 568687-568690):
// = 00 00 00 00 -> 0.0f. Used as the copysign pivot at module storage,
// and module storage. Not a tolerance; literally zero.
// = 66 66 66 3F -> 0x3F666666 -> 0.9f (0.899999976f). The decay factor of the
//     squared-position-error accumulator at module storage. NOT 0.66 and not the "66h f" the
//     analysis listing's inline annotation prints -- that annotation is showing the first byte
//     as an ASCII character, not the value.
// Neither is a float-widened-to-double case; both are read with `FMUL float ptr` /
// `FCOMP float ptr` (32-bit operands), so there is no widening to check for here.

// FPU-STACK TRAPS THAT CHANGE THE ANSWER, recorded because each one was a real ambiguity:
// . `FLD ST0` then `FLD ST1` keep TWO live copies of the elapsed time
//     while the y and z components are computed, so the third component (x) at module storage
//     multiplies the ORIGINAL dt, not a partial result. The stores come out reversed relative
//     to the loads: FSTP +0x4 receives x and FSTP +0x8 receives y, while z travelled through
//     EAX as an integer move (module storage/063431b0).
// . Same reversal for the position error: the z component is spilled
//     to a stack scratch and moved as an integer, then the two remaining x87 values pop into
//     x and then y.
// . `FLD ST0; FMUL ST1; FXCH; FXCH ST2; FCOMPP` -- the FMUL squares
//     MaxDeltaPosition, and the two FXCHs exist only to put the squared error into ST0 before
//     FCOMPP. Getting the operand order wrong here inverts the teleport test.
// and module storage-06343540. Both clamp loops keep a value live in
//     ST0/ST1 ACROSS iterations (dt^-2 in the first, nothing in the second) and both walk their
//     arrays DOWNWARD, z to x (SUB EBX,4 / SUB ECX,4 / SUB EDX,4 with a DEC counter from 3).
// /063434db. `SUB EBP,ESI` and `MOV ESI,3` destroy `this` before the torque
//     clamp loop even starts; baseline reaches Core's per-axis inertia through the biased
//     addressing `[ECX + EBP]` where EBP == Core - this. That is a register-allocation trick,
//     not a second object, and it is why MaxTorque (this+0x34) and RotInertia (Core+0x14) walk
//     in lockstep at a fixed 0x20 byte separation (module storage reads [ECX+EBP-0x20]).

// WHAT IS STILL NOT WIRED (one thing, and it is not in this file):
//   Nothing in this port DISPATCHES baseline vftable slot 4. The base slot is now declared with
//   its real signature (IVP_Controller::do_simulation_controller, ivp_controller.h) and this
//   body overrides it, but the baseline caller -- the per-PSI controller loop the routine at
// has no ported counterpart. So this body is correct and
//   complete but unreachable until that loop is ported.
void IVP_Controller_Golem::do_simulation_controller(IVP_Event_Sim* EventSim,
                                                    IVP_U_Vector<IVP_Core>* AffectedCores)
{
    // RET 0x8 -- two stack arguments. The second (the affected-core list the
    // dispatch loop hands every controller, module storage `PUSH EAX` with EAX = element+4) is
    // never read anywhere in this body.
    (void)AffectedCores;

    // baseline tests none of EventSim / environment / Body / Core. A port-only null guard stood
    // here, justified by constructor null-guards that were themselves removed on 2026-09-24;
    // removed 2026-09-25 (claude-code:h-ivp). The constructor always sets Body and Core.

    float const Dt = EventSim->delta_time;     // etc, `FMUL [EDI]`
    float const IDt = EventSim->i_delta_time;  // , spilled to a scratch slot
    ::IVP_Time const& Now = EventSim->environment->current_time;

    // Precision: everything baseline keeps on the x87 stack is held in double below; a value is
    // rounded to float only where baseline FSTPs it to a float slot.

    // ---- 1. Extrapolate the stored target pose to this instant ----
    // FLD time_base, FSUB [+0x64], FADD time_offset, FSUB [+0x68] --
    // ((base - stamp0) + offset) - stamp1, never stored. The two halves of the stamp are
    // subtracted separately, which is what proves IVP_Time is a float pair (see the IVP_Time
    // note in common_physys.h).
    double const PosElapsed = (((double)(Now.time_base) - TargetPositionTime[0])
                               + Now.time_offset) - TargetPositionTime[1];

    // CurrentTargetPosition = PosElapsed*TargetSpeed + TargetPosition,
    // one FSTP float per component (z first, then x, then y).
    CurrentTargetPosition.k[2] = (float)(PosElapsed * TargetSpeed.k[2] + TargetPosition.k[2]);
    CurrentTargetPosition.k[0] = (float)(PosElapsed * TargetSpeed.k[0] + TargetPosition.k[0]);
    CurrentTargetPosition.k[1] = (float)(PosElapsed * TargetSpeed.k[1] + TargetPosition.k[1]);

    // .
    if (bHasTargetRotQuat == 0)
    {
        // a flat four-dword copy of +0x7c into +0x14.
        CurrentTargetQuat = TargetQuat;
    }
    else
    {
        // . The SECOND stamp (+0x6c/+0x70) is differenced the same way,
        // then scaled by InvRotDeltaTime to give the interpolation parameter. The `PUSH ECX`
        // at module storage is stack allocation for that float argument, not an argument
        // itself -- ECX still holds &CurrentTargetQuat and is the thiscall receiver.
        // FLD base, FSUB [+0x6c], FADD offset, FSUB [+0x70], FMUL [+0x74], FSTP float (the
        // argument slot).
        double const QuatElapsed = (((double)(Now.time_base) - TargetQuatTime[0])
                                    + Now.time_offset) - TargetQuatTime[1];
        CurrentTargetQuat.set_interpolate_quat(&TargetQuat, &TargetRotQuat,
                                               (float)(QuatElapsed * InvRotDeltaTime));
    }

    // ---- 2. Position error, and the teleport escape hatch ----
    // . Core+0x124/+0x128/+0x12c is MWorldFCore.vv -- the ADD ECX,0xf4
    // at module storage rebases the pointer onto the matrix, so the later [ECX+0x30]/[ECX+0x38]
    // are the matrix's own translation row, not separate fields.
    IVP_U_Point Delta;
    Delta.k[0] = CurrentTargetPosition.k[0] - Core->MWorldFCore.vv.k[0];
    Delta.k[1] = CurrentTargetPosition.k[1] - Core->MWorldFCore.vv.k[1];
    Delta.k[2] = CurrentTargetPosition.k[2] - Core->MWorldFCore.vv.k[2];

    // . `TEST DH,0xc` tests bits 0x0C00 of the dword at Body+0x78 (DH is
    // byte 1 of EDX), i.e. two adjacent IVP_Real_Object flag bits. When they are clear the body
    // is offset from its core's origin, so the core-to-object shift is rotated into world space
    // (the routine == IVP_U_Matrix3::vmult) and taken out of the error.
    if ((Body->flags & 0x0C00u) == 0)
    {
        IVP_U_Point ShiftWorld;
        Core->MWorldFCore.vmult(&Body->shift_core_f_object, &ShiftWorld);
        Delta.subtract(&Delta, &ShiftWorld);  // , this = v1 - v2
    }

    // . Written out rather than calling quad_length() because baseline
    // sums (y^2 + z^2) + x^2 and the generic helper sums x^2 + y^2 + z^2 -- same value in
    // exact arithmetic, different rounding.
    // The sum stays on the x87 stack (never stored).
    double const DeltaQuadLength =
        ((double)(Delta.k[1]) * Delta.k[1] + (double)(Delta.k[2]) * Delta.k[2])
        + (double)(Delta.k[0]) * Delta.k[0];

    // FLD [+0x60], FMUL float 0.9f (the field), FADD ST0,ST1, FSTP
    // float. See SmoothedPositionErrorSq's declaration for the raw bytes of the 0.9f.
    SmoothedPositionErrorSq = (float)(
        (double)(SmoothedPositionErrorSq) * (double)(0.9f) + DeltaQuadLength);

    // MaxDeltaPosition squared on the stack, FCOMPP with ST0 = the
    // quad length; strictly greater -- see the gate table above.
    if (DeltaQuadLength > (double)(MaxDeltaPosition) * MaxDeltaPosition)
    {
        beam_to_target(EventSim, 0);  // slot 7, module storage pushes 0 then the event
        return;
    }

    // ---- 3. Linear servo ----
    // Delta *= ForceFactor. The z component again detours through an
    // integer move (module storage/063432d7/063432dd).
    Delta.k[0] *= ForceFactor;
    Delta.k[1] *= ForceFactor;
    Delta.k[2] *= ForceFactor;

    // then module storage-0634331c. Two add_multiple calls
    // (the routine, `this = v1 + v2*f`), the second with the scale negated by an FCHS at
    // i.e. the velocity error (target minus actual), not the target velocity
    // alone. baseline recomputes DampFactor*dt for each; it is the same value both times.
    // The scale is FSTPed to a float argument; inside the routine each component is
    // FLD Scale, FMUL B, FADD A, one FSTP float.
    float const DampDt = DampFactor * Dt;
    Delta.k[0] = (float)((double)(DampDt) * TargetSpeed.k[0] + Delta.k[0]);
    Delta.k[1] = (float)((double)(DampDt) * TargetSpeed.k[1] + Delta.k[1]);
    Delta.k[2] = (float)((double)(DampDt) * TargetSpeed.k[2] + Delta.k[2]);

    float const NegDampDt = -(DampFactor * Dt);  // FMUL, FCHS, FSTP float
    Delta.k[0] = (float)((double)(NegDampDt) * Core->Speed.x + Delta.k[0]);
    Delta.k[1] = (float)((double)(NegDampDt) * Core->Speed.y + Delta.k[1]);
    Delta.k[2] = (float)((double)(NegDampDt) * Core->Speed.z + Delta.k[2]);

    // . Per-axis force clamp, walked z->y->x. Delta is about to become a
    // velocity increment of Delta*IDt, so the force it represents is Mass*Delta*IDt^2; the
    // clamp value is the exact inverse relation, InvMass*Dt^2*bound, which is what confirms
    // that Core+0x20/+0x40 are Mass/InvMass and that IVP_Event_Sim+0x00/+0x04 are dt and 1/dt.
    {
        // IDt squared stays on the x87 stack across the whole loop.
        double const IDtSquared = (double)(IDt) * IDt;
        for (int Axis = 2; Axis >= 0; --Axis)
        {
            // FLD Mass, FMUL float Delta, FMUL ST1, FABS, FCOMP float
            // bound, TEST AH,0x1 + JNZ skips on less (and unordered); the clamp runs on >=.
            double const AxisForce = fabs((double)(Core->Mass) * Delta.k[Axis] * IDtSquared);
            if (AxisForce >= MaxTranslationForce.k[Axis])
            {
                // ((InvMass * dt) * dt) * bound, negated when the
                // delta is below 0.0f (FCOMP, TEST AH,0x1 + JZ skips the FCHS only on C0
                // clear, so the FCHS side also takes unordered -- hence !(>=)), FSTP float.
                double Clamped = (double)Core->InvMass * Dt * Dt * MaxTranslationForce.k[Axis];
                if (!(Delta.k[Axis] >= 0.0f))
                    Clamped = -Clamped;
                Delta.k[Axis] = (float)(Clamped);
            }
        }
    }

    // . The whole correction lands as a direct velocity edit:
    // FLD Delta, FMUL float IDt, FADD Speed, FSTP float (z, then x, then y).
    Core->Speed.z = (float)((double)(Delta.k[2]) * IDt + Core->Speed.z);
    Core->Speed.x = (float)((double)(Delta.k[0]) * IDt + Core->Speed.x);
    Core->Speed.y = (float)((double)(Delta.k[1]) * IDt + Core->Speed.y);

    // ---- 4. Orientation error, and its own teleport escape hatch ----
    // . conj(target) * current, then the small-angle "2*asin of each
    // xyz" reduction to a rotation vector. the routine / the routine / the routine.
    IVP_U_Quat InvTargetQuat;
    InvTargetQuat.set_invert_unit(&CurrentTargetQuat);

    IVP_U_Quat RelativeQuat;
    RelativeQuat.set_mult_quat(&InvTargetQuat, &Core->QWorldFCoreNext);

    IVP_U_Point RelativeQuatXYZ;
    RelativeQuatXYZ.set(RelativeQuat.x, RelativeQuat.y, RelativeQuat.z);
    IVP_U_Point RotError;
    IVP_DoubleAsinVector(&RelativeQuatXYZ, &RotError);

    // . baseline sums (z^2 + y^2) + x^2 here -- note this is a DIFFERENT
    // summation order from the position error above, which started with y.
    double const RotQuadLength =
        ((double)(RotError.k[2]) * RotError.k[2] + (double)(RotError.k[1]) * RotError.k[1])
        + (double)(RotError.k[0]) * RotError.k[0];

    // FCOMPP with ST0 = DeltaOrientation^2, ST1 = the quad length;
    // TEST AH,0x1 + JZ keeps going on C0 clear (DeltaOrientation^2 >= length). C0 is also set
    // on unordered, so a NaN length teleports too -- hence !(>=), not <.
    if (!((double)(DeltaOrientation) * DeltaOrientation >= RotQuadLength))
    {
        beam_to_target(EventSim, 1);  // pushes 1 here, 0 at the position gate
        return;
    }

    // ---- 5. Angular servo ----
    // . The quaternion dot product picks which of the two double-cover
    // representations of the relative rotation the asin reduction landed on.
    double const QuatDot =
        (((double)(Core->QWorldFCoreNext.w) * CurrentTargetQuat.w
          + (double)(Core->QWorldFCoreNext.z) * CurrentTargetQuat.z)
         + (double)(Core->QWorldFCoreNext.y) * CurrentTargetQuat.y)
        + (double)(CurrentTargetQuat.x) * Core->QWorldFCoreNext.x;

    // FLD IDt, FMUL float TorqueFactor -- the scale stays on the
    // stack. FCOMP 0.0f on the dot, TEST AH,0x1 + JZ: negated on dot >= 0 (C0 clear), NOT on
    // dot < 0 or unordered -- see the gate table.
    double RotScale = (double)(IDt) * TorqueFactor;
    if (QuatDot >= 0.0)
        RotScale = -RotScale;

    // Each product FSTP float (z, then x, then y).
    RotError.k[2] = (float)(RotError.k[2] * RotScale);
    RotError.k[0] = (float)(RotScale * RotError.k[0]);
    RotError.k[1] = (float)(RotError.k[1] * RotScale);

    // FLD DampFactor, FCHS, then per axis FLD ST0, FMUL float
    // RotSpeed, FADD float RotError, FSTP float. Note this uses DampFactor (+0x48), the SAME
    // field the linear servo uses -- AngularDampFactor (+0x50) is never read by this function.
    // That is baseline's behaviour, not an omission here; +0x50 is written by the constructor
    // (module storage/06342d08) and read nowhere in module storage-0634357b.
    double const NegDamp = -(double)(DampFactor);
    float RotDelta[3];
    RotDelta[0] = (float)(NegDamp * Core->RotSpeed.x + RotError.k[0]);
    RotDelta[1] = (float)(NegDamp * Core->RotSpeed.y + RotError.k[1]);
    RotDelta[2] = (float)(NegDamp * Core->RotSpeed.z + RotError.k[2]);

    // . The torque mirror of the force clamp: RotInertia*IDt*delta is
    // the torque, InvRotInertia*Dt*bound the inverse. Per-axis inertia here, unlike the scalar
    // Mass/InvMass above.
    {
        float const RotInertiaAxis[3] = {Core->RotInertia.x, Core->RotInertia.y,
                                         Core->RotInertia.z};
        float const InvRotInertiaAxis[3] = {Core->InvRotInertia.x, Core->InvRotInertia.y,
                                            Core->InvRotInertia.z};
        for (int Axis = 2; Axis >= 0; --Axis)
        {
            // (IDt * RotInertia) * RotDelta on the stack, FLD ST0,
            // FABS, FCOMP float bound, TEST AH,0x1 + JNZ skips on less (and unordered).
            double const AxisTorque = (double)(IDt) * RotInertiaAxis[Axis] * RotDelta[Axis];
            if (fabs(AxisTorque) >= MaxTorque.k[Axis])
            {
                // ((InvRotInertia * dt) * bound), FCHS when the SIGNED
                // torque is below 0.0f (not the delta) or unordered (TEST AH,0x1 + JZ at
                // /0634352f skips the FCHS only on C0 clear), FSTP float.
                double Clamped = (double)InvRotInertiaAxis[Axis] * Dt * MaxTorque.k[Axis];
                if (!(AxisTorque >= 0.0))
                    Clamped = -Clamped;
                RotDelta[Axis] = (float)(Clamped);
            }
        }
    }

    // .
    Core->RotSpeed.x = RotDelta[0] + Core->RotSpeed.x;
    Core->RotSpeed.y = RotDelta[1] + Core->RotSpeed.y;
    Core->RotSpeed.z = RotDelta[2] + Core->RotSpeed.z;
}

// (module storage-0628895c), baseline vftable slot 7 -- the integrator's escape
// hatch. Five instructions:
//     MOV EAX,[ESP+0x4] ; PUSH EAX ; CALL the routine ; XOR EAX,EAX ; RET 0x8
// so the 0/1 the integrator passes as the SECOND argument is read by nobody, and the return
// value is a hard zero regardless of what the callee did. the routine
// (module storage-06343142) then ignores the argument it was handed and is only:
//     PUSH 1 ; LEA EAX,[ECX+0x4] ; LEA EDX,[ECX+0x14] ; MOV ECX,[ECX+0x58] ; PUSH EAX ;
//     PUSH EDX ; CALL the routine ; RET 0x4
// Last push first: param_1 = &CurrentTargetQuat (+0x14), param_2 = &CurrentTargetPosition
// (+0x4), param_3 = 1, receiver = this->Body. That is IVP's
// IVP_Real_Object::beam_object_to_new_position(quat, position, optimize_for_repeated_calls) --
// a hard teleport of the body onto the extrapolated target pose, which is exactly the right
// behaviour for "the servo has fallen too far behind to catch up with force".

// is IVP_Controller_Golem::beam_to_current_target, because
// CRemotePhysicsController calls it from three more places (module storage, module storage,
// ); the routine is IVP_Real_Object::beam_object_to_new_position.
void IVP_Controller_Golem::beam_to_current_target(IVP_Event_Sim const* EventSim)
{
    (void)EventSim;  // RET 0x4 pops it; module storage-06343142 never reads [ESP+0x4]

    // PUSH 1 / 06343132 LEA EAX,[ECX+0x4] / 06343135 LEA EDX,[ECX+0x14] /
    // MOV ECX,[ECX+0x58] / 0634313d CALL the routine (args: quat +0x14, pos +0x4, 1).
    Body->beam_object_to_new_position(&CurrentTargetQuat, &CurrentTargetPosition, 1);
}

int FLControllerGolem::beam_to_target(IVP_Event_Sim const* EventSim, int Reason)
{
    (void)Reason;    // reads only [ESP+0x4]; the 0/1 flag is dropped on the floor

    beam_to_current_target(EventSim);

    return 0;  // XOR EAX,EAX
}

// ---- port-local entry points declared in common_physys.h ----
// They stand for the direct __thiscall CALLs at module storage / 0627e2d9 (AnimDB::Update,
// ECX = EDI = [CObject+0x60]); no test of their own -- the caller's `CMP [ESI+0x60],0; JZ`
// at 0627e1f9-0627e1fc is the only one.

void __cdecl PhySys::GolemSetTargetPosition(::FLControllerGolem* Golem,
                                            Vector const& TargetPosition,
                                            Vector const& TargetSpeed,
                                            IVP_Time const& Time)
{
    Golem->set_target_position(TargetPosition, TargetSpeed, Time);
}

void __cdecl PhySys::GolemSetTargetQuaternion(::FLControllerGolem* Golem,
                                              float const TargetQuat[4],
                                              IVP_Time const& Time,
                                              float const* TargetRotQuatOrNull,
                                              float DeltaTime)
{
    Golem->set_target_quaternion(TargetQuat, Time, TargetRotQuatOrNull, DeltaTime);
}

// ============================================================================
// PhySys::BeamBodyToWorldPose -- the routine
// (module storage .. module storage inclusive; 191 instructions / 676 bytes;
//  Common; implementation tracker Common.dll item 2746)

// Read end to end from the first byte (SUB ESP,0x158) to the last (RET at
// ) before a line of this was written.

// WHAT IT IS. baseline hands it a body, a desired ABSOLUTE world orientation
// (quaternion) and world position, and it makes the body sit at that pose --
// not by moving the body's physical core, but by recomputing the body's fixed
// offset WITHIN that core (IVP_Real_Object::q_core_f_object /
// shift_core_f_object, both written by ApplyLocalCsys). That is exactly what
// an animated sub-part welded into a parent ship's core needs: the core keeps
// integrating, the part is re-seated inside it every frame. It then pays the
// bookkeeping cost of having moved: the hull manager's outer margin is bumped,
// the narrow-phase cache slot is orphaned, the mindist caches are told to
// recompute, and any hull-pool listeners whose key the bump invalidated are
// drained.

// ARGUMENT DECODE (__cdecl, last push is the first argument). Verified at both
// of the DLL's two call sites rather than assumed:
//   * (AnimDB::Update, TransformState==2 arm):
//       push EBX(=0); push &Position; push &Quat; push [ESI+0x54]; push EBX(=0)
//     -> (0, CObject::BodyPtr, &Quat, &Position, 0), ADD ESP,0x14.
//   * (inside the routine):
//       push 0; push &local_10; push &local_48; push ESI; push 0
//     -> (0, ESI, &local_48, &local_10, 0), ADD ESP,0x14.
// So param_1 is the constant 0 at EVERY call site in this DLL, and param_5 is
// never read by the body at all.

// .rdata CONSTANTS, read as raw bytes (analysis's inline "= 0Ah"-style comments
// are one byte of the adjacent object and are wrong here):
// Common  00 00 00 00 -> 0.0f
// Common  00 00 20 41 -> 10.0f
// Common  0a d7 23 3c -> 0.01f

// RECEIVERS, proven not guessed:
//   * EDI := param_2 at module storage, and stays the body for the rest of the
//     function. [EDI+0x18] is IVP_Object::environment, [EDI+0x98] is
//     IVP_Real_Object::core, [EDI] is its vtable.
//   * ESI := EDI+0x44 at module storage, i.e. &IVP_Real_Object::hull_manager.
//     Settled independently: the tail's `MOV ECX,ESI; CALL the routine`
//     (module storage) targets IVP_Hull_Manager::DrainAndShift (implementation tracker
//     item 3169, already ported in ivp_real_object.h/.cpp), so ESI is an
//     IVP_Hull_Manager*, and every [ESI+..] offset used here (+0x00, +0x08,
//     +0x0c, +0x10, +0x18, +0x1c, +0x24, +0x28, +0x2e) lands on a field that
//     class already declares.
//   * `CALL dword ptr [EDX+0x8]` at module storage is vtable slot 2 =
//     IVP_Real_Object::ApplyLocalCsys (module storage slot 2 == the branch;
//     both concrete leaves, IVP_Polygon/063a5824 and IVP_Ball/063a585c,
//     inherit it verbatim), so the dispatch is non-virtual in practice.

// MATRIX HELPERS. Both take their output as an explicit pointer, as in baseline:
//   * == IVP_U_Matrix::mmult4(mb, result): ECX = left operand,
//     param_1 = right operand, param_2 = OUTPUT.
//   * == IVP_U_Matrix::mimult4(mb, result): ECX = the matrix
//     being inverted, param_1 = the target, param_2 = OUTPUT.

// TWO NAMED OPEN RESIDUALS, both flagged inline below: the ReferenceBody
// branch and the pair of mindist-invalidation broadcasts. Neither is accepted
// as final.
// ============================================================================
bool __cdecl PhySys::BeamBodyToWorldPose(IVP_Real_Object* ReferenceBodyOrNull, IVP_Real_Object* Object,
                                         float const WorldQuat[4], Vector const& WorldPosition)
{

    // ---- module storage-06343d3e: the reference/base transform (local_100) ----
    // baseline builds a 0x40-byte IVP_U_Matrix here via the routine (the generic
    // "placement-construct N elements of stride S" helper, implementation tracker item
    // 550, called with base/stride=0x10/count=3/ctor=Csys::Csys). Csys::Csys is
    // , `MOV EAX,this; RET` -- it writes nothing, so all four of
    // this function's the routine calls are no-ops that merely reserve stack.
    // They are not reproduced; the C++ locals below occupy the same role.
    IVP_U_Matrix LocalBase;
    if (ReferenceBodyOrNull != nullptr)
    {
        // the reference body's narrow-phase cache slot
        // (field_40), allocated through the routine when missing and refreshed by
        // when the signed flags byte is < 8 (`SHL EDX,0x18; CMP
        // EDX,0x8000000; JGE skip`) and Env->sim_step_count > Slot->LastRefreshTick
        // (JLE skip) -- the library's get_cache_object_no_lock -- then REP MOVSD of
        // 0x10 dwords from Slot+0x1c (WorldTransform) into this local.
        IVP_NarrowPhaseCacheSlot* const Slot = ReferenceBodyOrNull->get_cache_object_no_lock();
        memcpy(&LocalBase, &Slot->WorldTransform, sizeof(IVP_U_Matrix));
    }
    else
    {
        // the routine == IVP_U_Matrix::init (identity).
        LocalBase.init();
    }

    // ---- module storage-06343d8d: the requested world pose (local_140) ----
    // == IVP_U_Quat::set_matrix, receiver = param_3.
    IVP_U_Matrix WorldFromObject;
    ((IVP_U_Quat const*)WorldQuat)->set_matrix(&WorldFromObject);   // 4 floats, IVP (x,y,z,w) order
    // the three floats of param_4 are stored into
    // local_140+0x30/+0x34/+0x38, i.e. that same matrix's translation. analysis
    // names those three slots local_110/local_10c/local_108 because they sit
    // inside local_140's own 0x40-byte extent; they are not separate variables.
    WorldFromObject.vv.set(WorldPosition.x, WorldPosition.y, WorldPosition.z);

    // ---- module storage-06343da4: local_80 = local_100 * local_140 ----
    IVP_U_Matrix Combined;
    LocalBase.mmult4(&WorldFromObject, &Combined);

    // ---- module storage-06343e5f: the core's own extrapolated world pose ----
    IVP_Core* const Core = Object->core;
    IVP_Environment* const Env = Object->environment;

    // . Both halves of the split-precision clock are
    // differenced against IVP_Core::TimeOfLastPsi (+0x68/+0x6c) in one x87 chain,
    // ((tb - psi_tb) + to) - psi_to, multiplied by IVP_Core::IDeltaTime (+0x70) on
    // the stack and stored as a float only then (FSTP float [ESP]).
    float const Alpha = (float)(
        (((double)(Env->current_time.time_base) - Core->TimeOfLastPsi.time_base)
         + Env->current_time.time_offset - Core->TimeOfLastPsi.time_offset)
        * Core->IDeltaTime);

    // the same difference again, without the FMUL, stored as a
    // float for the position extrapolation below.
    float const DTime = (float)(
        ((double)(Env->current_time.time_base) - Core->TimeOfLastPsi.time_base)
        + Env->current_time.time_offset - Core->TimeOfLastPsi.time_offset);

    // == IVP_U_Quat::set_interpolate_quat(q0, q1, alpha); the
    // pushed float slot is the `PUSH ECX` at module storage being overwritten
    // by `FSTP [ESP]` at :06343e18, the usual reserve-then-store idiom.
    IVP_U_Quat InterpQuat;
    InterpQuat.set_interpolate_quat(&Core->QWorldFCore, &Core->QWorldFCoreNext, Alpha);

    IVP_U_Matrix CoreWorld;
    InterpQuat.set_matrix(&CoreWorld);

    // receiver is local_c0+0x30 == CoreWorld.vv.
    // == PhySys_Vec3AddMul (implementation tracker item 772).
    PhySys_Vec3AddMul((Vector&)CoreWorld.vv, Core->PosWorldFCore, Core->DeltaWorldFCore, DTime);

    // ---- module storage-06343ea2: re-seat the body inside its core ----
    // local_40 = inverse(Combined) * CoreWorld, i.e. the core's frame expressed
    // in the object's frame -- which is precisely what ApplyLocalCsys expects
    // (it inverts its argument back and stores the result into
    // shift_core_f_object / q_core_f_object).
    IVP_U_Matrix ObjectFromCore;
    Combined.mimult4(&CoreWorld, &ObjectFromCore); // at module storage
    Object->ApplyLocalCsys(&ObjectFromCore);

    // ---- module storage-06343ec3: widen the hull margin ----
    // Write order below matches baseline's interleaving exactly.
    IVP_Hull_Manager& Hull = Object->hull_manager;
    // FLD [EDI+0x5c] / FADD float the field (0.01f): one extended sum, rounded at the
    // FST / FSTP float stores.
    float const NewOuter = (float)((double)(Hull.AccumulatedDeltaA) + 0.01f);
    Hull.OuterGradient = 0.0f;         // , [ESI+0x08]
    Hull.AccumulatedDeltaA = NewOuter; // , FST  [ESI+0x18]
    Hull.InnerGradient = 0.0f;         // , [ESI+0x0c]
    Hull.OuterValueAtLastEdit = NewOuter; // , FSTP [ESI+0x10]

    // ---- module storage..06343ecb: the routine(Body) (cdecl) ----
    IVP_Real_Object_Free_Cache_Object(Object);

    // ---- module storage/module storage: the two mindist broadcasts ----
    // `MOV ECX,EDI; CALL the routine` then `MOV ECX,EDI; CALL the routine`, both
    // __thiscall on the body.
    Object->recalc_exact_mindists();
    Object->recalc_invalid_mindists();

    // ---- module storage-06343f2f: drain hull-pool listeners ----
    // baseline's guard is `EDI=100` pre-loaded, then per iteration
    // `MOV EAX,EDI; DEC EDI; TEST EAX,EAX; JL exit` -- it tests the value from
    // BEFORE the decrement, so the loop body runs at most 102 times.
    int Guard = 100;
    // FLD [ESI+0x28] / FSUB [ESI+0x18] / FST float [ESP+8]: the difference is stored as a
    // float (passed to the listener) and compared unrounded (ExcessExact: a tiny negative
    // difference rounds to -0.0f, which would compare differently).
    double ExcessExact = (double)Hull.PoolKeyThreshold - Hull.AccumulatedDeltaA;
    float Excess = (float)ExcessExact;
    // FCOMP against the field (= 0.0f) with `TEST AH,0x1` + `JZ` at
    // / `JNZ` back at 06343f2f: AH bit 0 is C0, which FCOMP sets
    // when st0 < operand AND when unordered, so the loop CONTINUES while the
    // unrounded difference is negative or NaN.
    while (!(ExcessExact >= 0.0))
    {
        // the live entry's payload (Entry+0xc, an
        // IVP_Listener_Hull) gets slot 1, hull_limit_exceeded_event(hull, excess).
        IVP_Hull_Manager::Entry& E = Hull.PoolEntries[Hull.PoolLiveIndex];
        ((IVP_Listener_Hull*)E.Payload)->hull_limit_exceeded_event(&Hull, Excess);

        int const GuardBeforeDecrement = Guard;
        Guard -= 1;
        if (GuardBeforeDecrement < 0)
            break;

        ExcessExact = (double)Hull.PoolKeyThreshold - Hull.AccumulatedDeltaA;
        Excess = (float)ExcessExact;
    }

    // ---- module storage-06343f53: periodic rebase ----
    // `FLD [ESI]; FILD dword [ESI+0x1c]; FCOMPP; TEST AH,0x1; JZ skip`: FCOMPP
    // compares the freshly-loaded integer (st0) against LastEditTimeHi (st1);
    // C0 is set on "less" AND on "unordered", so the body runs when the cached
    // integer has fallen behind the timestamp or the timestamp is NaN.
    if (!((double)(int32_t)Hull.TimeOfNextReset >= (double)Hull.LastEditTimeHi))
    {
        Hull.DrainAndShift();
        // FLD [ESI] / FADD float the field (10.0f) / ftol: the unrounded sum,
        // truncated toward zero, stored at 06343f53.
        Hull.TimeOfNextReset = (uint32_t)(int32_t)((double)Hull.LastEditTimeHi + 10.0f);
    }

    return true;  // MOV EAX,1 -- constant, never inspected
}

// (module storage-062890cd, __cdecl, two args). baseline's per-child-part physics
// finalize. Its ONE call site is module storage inside the routine (implementation tracker item 735),
// the loop that spawns a child CObject per Archetype::Root::get_phy_part entry; the caller
// above that (module storage-06288a9e) is the archetype part walk that also picks
// GOLEM_CHILD_* vs DEFAULT_* mass/damping off the same CreateParms::Type at
// (PhySys_BuildInstanceIVP_Real / PhySys::BuildIVP_Real below).
// baseline only ever reaches here for a freshly created child part: its mode-2 arm welds
// Object's body into GetRoot(Object)'s body.
void __cdecl PhySys::AttachGolemController(CObject* Object, CreateParms const& Parms)
{
    // `MOV EAX,[EBX+0x18]; DEC EAX; JZ mode1; DEC EAX; JNZ out`.
    // CreateParms+0x18 == 1 selects the golem, == 2 the weld, anything else returns.
    if (Parms.Type == 1)
    {
        // the mode is stamped on the object BEFORE the controller exists.
        Object->PhysicsControlMode = 1;

        // then 06288eef-06288f7a: default-construct the tuning struct, then
        // overwrite it from the GOLEM_* globals. The stack-slot mapping was recovered from the
        // local_44..local_1c chain (local_44 is the struct base, each later local one dword
        // further in) and is cross-checked by the defaults: parms+0x24's default 10.0f equals
        // the shipped GOLEM_MAX_DELTA_POSITION and parms+0x28's PI/2 equals the shipped
        // GOLEM_DELTA_ORIENTATION, so the two independent sources agree.
        FLControllerGolemParms GolemParms;
        FLControllerGolemParms_Init(GolemParms);

        GolemParms.ForceFactor = PhySys::GOLEM_FORCE_FACTOR;
        GolemParms.DampFactor = PhySys::GOLEM_DAMP_FACTOR;
        GolemParms.TorqueFactor = PhySys::GOLEM_TORQUE_FACTOR;
        GolemParms.AngularDampFactor = PhySys::GOLEM_ANGULAR_DAMP_FACTOR;
        GolemParms.MaxTranslationForce[0] = PhySys::GOLEM_MAX_TRANSLATION_FORCE.x;
        GolemParms.MaxTranslationForce[1] = PhySys::GOLEM_MAX_TRANSLATION_FORCE.y;
        GolemParms.MaxTranslationForce[2] = PhySys::GOLEM_MAX_TRANSLATION_FORCE.z;
        GolemParms.MaxTorque = PhySys::GOLEM_MAX_TORQUE;
        GolemParms.MaxDeltaPosition = PhySys::GOLEM_MAX_DELTA_POSITION;
        GolemParms.DeltaOrientation = PhySys::GOLEM_DELTA_ORIENTATION;

        // DEVIATION (baseline bug, deliberately not reproduced): module storage copies
        // local_98 into parms+0x1c, and local_98 is never written anywhere in the routine --
        // its analysis XREF list (Common) shows a single READ and no write. baseline
        // therefore seeds parms+0x1c with whatever junk is on the stack. Nothing ever reads
        // parms+0x1c (neither the routine nor the routine touch it), so the value is inert;
        // the port keeps the constructor's 0.0f.

        // operator_new(0xd0), construct (the body is read only after
        // the null test, module storage), then stamp the leaf vftable. MSVC6's operator new
        // returns null on failure: baseline stores the null (06288fb6/06288fbd) and then calls
        // set_target_position on it below -- a deterministic crash, reproduced with nothrow.
        FLControllerGolem* const Golem = new (std::nothrow) FLControllerGolem(Object->BodyPtr, GolemParms);
        Object->GolemController = Golem;

        // the routine find(CreateParms+0x14) in the systems map, then
        // the found system's Environment->current_time (env +0xf8 / +0xfc) -- what
        // PhySys::GetCurrentTime returns. There is no miss test: a miss reads through the map
        // head node, as GetCurrentTime does (PhySys_FindSystem).
        IVP_Time const Now = PhySys::GetCurrentTime(Parms.CollisionGroupId);

        // the lazily cached I3DMathEngine singleton (the field,
        // filled by the routine) and its vtable+0x50, MatrixToQuaternion, fed Object+0x08.
        I3DMathEngine* const Math = Acquire3DMathEngine();

        FQuaternion MathQuat = {0.0f, 0.0f, 0.0f, 0.0f};
        Math->MatrixToQuaternion(MathQuat, (FMatrix const&)Object->Orientation);

        // baseline rotates the four dwords by one slot,
        // dst[0..3] = src[1], src[2], src[3], src[0]. FQuaternion is (W,X,Y,Z), so the buffer
        // handed to set_target_quaternion is (X,Y,Z,W) -- IVP's order, not a copy.
        float const IvpQuat[4] = {MathQuat.X, MathQuat.Y, MathQuat.Z, MathQuat.W};

        // zeroes the speed triple immediately before the call.
        Vector const ZeroSpeed = {0.0f, 0.0f, 0.0f};

        Golem->set_target_position(Object->WorldPosition, ZeroSpeed, Now);
        Golem->set_target_quaternion(IvpQuat, Now, nullptr, 0.0f);
        return;
    }

    if (Parms.Type != 2)
        return;

    // same stamp-before-work ordering as mode 1.
    Object->PhysicsControlMode = 2;

    // GetRoot(Object), then
    // (Root->BodyPtr, Object->BodyPtr, -1.0f)
    // (__cdecl; last push first, so EDX = Root's body is param_1 and ECX = Object's body is
    // param_2; the 0xbf800000 pushed first is -1.0f. The `ADD ESP,0x10` covers those three
    // pushes plus the PUSH ESI that fed the __cdecl GetRoot call.)
    CObject* const Root = GetRoot(Object);
    IVP_Merge_Object_Into_Core(Root->BodyPtr, Object->BodyPtr, -1.0f);
}

