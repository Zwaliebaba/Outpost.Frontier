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
// Remote Correction (RC) -- CRemotePhysicsController
// ============================================================================

// PORTED 2026-09-24 (claude-code:w6-rpc). Until then RCBegin allocated a memset-zeroed
// 0x3c0 block with two hand-made "mock" vtables and RCSet/RCEnd called into it by raw slot
// index. The class is declared in src/include/common_server.h (it has to be public: Server.dll
// names it in the __RTDynamicCast at module storage); its RTTI, both vftables and its layout
// are documented there. Everything below is read off Common.

// is `ECX = Core->Environment; PUSH Core; CALL the routine` -- "enqueue the
// core for wakeup" (IVP_Controller_Manager_Ensure_Core_To_Be_In_Simulation). All three controller methods
// begin or end with it on this->Core (+0x5c); the preceding `MOV ECX,[... + 0x90]` loads of
// Environment+0x90 are dead, as in the golem setters.

// Inlined into PhySys::RCBegin, module storage .. module storage:
// the routine(this, Body, &Parms)        IVP_Controller_Golem base
// CRemotePhysicsSimulation::CRemotePhysicsSimulation(this + 0xd0)
// [this]       = module storage          (compiler: final vptrs before the body)
// [this+0xd0]  = module storage
// the routine(this+0xd0, &Pos, &Orient, 0.0)  -- THIS class's RemoteUpdate,
//             called directly (PUSH 0 / PUSH 0 are the two halves of the double 0.0)
// +0x348 = Vel (three dwords), +0x3b8 = Body, +0x388 = 0
// +0x348 is the simulation base's +0x278 (the velocity CRemotePhysicsSimulation::Doit
// publishes and PhySys::GetVelocity reads back); +0x388 is its +0x2b8 m_SampleCount, reset
// AFTER RemoteUpdate so the seed sample does not count.
CRemotePhysicsController::CRemotePhysicsController(IVP_Real_Object* InBody,
                                                   FLControllerGolemParms const& Parms,
                                                   Vector const& Position,
                                                   Quaternion const& Orientation,
                                                   Vector const& Velocity)
    : IVP_Controller_Golem(InBody, Parms)
    , CRemotePhysicsSimulation()
{
    CRemotePhysicsController::RemoteUpdate(Position, Orientation, 0.0);

    m_Velocity = Velocity;                               // , this+0x348
    RealObject = InBody;                                 // , this+0x3b8
    m_SampleCount = 0;                                   // , this+0x388
}

// (module storage-06285afa): re-stamp both vptrs (06285ac3/06285ac9), destroy
// the CRemotePhysicsSimulation base at +0xd0 (06285ad7), then the routine on this
// (06285ae6) -- the IVP_Controller_Golem destructor. That is exactly the compiler's
// reverse-order base destruction, so the body is empty.
// (slot 6) is the scalar deleting destructor around it (flag & 1 ->
// = operator delete) and the branch (`SUB ECX,0xd0; JMP the routine`) is the
// this-adjusting thunk in the CRemotePhysicsSimulation-side table; both are emitted by the
// compiler from this declaration.
CRemotePhysicsController::~CRemotePhysicsController()
{
}

// (module storage-062923d1, RET 0x10) -- slot 1 of module storage, entered
// with ECX = this + 0xd0.
// CRemotePhysicsSimulation::RemoteUpdate(Pos, Orient, Timestamp),
//                      a direct (non-virtual) call with the same four dwords
// the routine([ECX-0x74]) -- this+0xd0-0x74 = this+0x5c, Core
void CRemotePhysicsController::RemoteUpdate(Vector const& Position,
                                            Quaternion const& Orientation,
                                            double Timestamp)
{
    CRemotePhysicsSimulation::RemoteUpdate(Position, Orientation, Timestamp);
    IVP_Controller_Manager_Ensure_Core_To_Be_In_Simulation(Core);
}

// (module storage-06292561, RET 0x8) -- slot 4 of module storage, overriding
// IVP_Controller_Golem's PD servo (the branch). It does not servo: it runs the dead
// reckoning one PSI forward, copies the result into the golem's target fields, keeps the
// golem's error accumulator up to date, and then BEAMS the body onto the target every time.
void CRemotePhysicsController::do_simulation_controller(IVP_Event_Sim* EventSim,
                                                        IVP_U_Vector<IVP_Core>* AffectedCores)
{
    (void)AffectedCores;  // [ESP+0x30] after the prologue; never read

    IVP_Controller_Manager_Ensure_Core_To_Be_In_Simulation(Core);

    // FLD float [EventSim] (delta_time), FSTP double, Doit on +0xd0.
    CRemotePhysicsSimulation::Doit((double)EventSim->delta_time);

    // +0x04 CurrentTargetPosition <- this+0x37c/+0x380/+0x384
    // (= simulation +0x2ac, the position DRV's projected position Doit mirrors there).
    // KNOWN baseline UB: the three floats go through a stack temporary built as an
    // IVP_U_Point and all FOUR dwords are copied, so +0x10 (hesse) receives [ESP+0x24], a
    // stack slot nothing in this function writes. The port leaves +0x10 as it is.
    CurrentTargetPosition.k[0] = m_ProjectedPosition.x;
    CurrentTargetPosition.k[1] = m_ProjectedPosition.y;
    CurrentTargetPosition.k[2] = m_ProjectedPosition.z;

    // +0xac TargetSpeed <- this+0x348/+0x34c/+0x350 (simulation
    // +0x278, the velocity). Same uninitialised fourth dword into +0xb8 (KNOWN baseline UB),
    // not reproduced.
    TargetSpeed.k[0] = m_Velocity.x;
    TargetSpeed.k[1] = m_Velocity.y;
    TargetSpeed.k[2] = m_Velocity.z;

    // +0x14 CurrentTargetQuat <- the simulation's +0x29c
    // quaternion, rotated by one slot: x = [+0x370], y = [+0x374], z = [+0x378],
    // w = [+0x36c]. +0x29c is (W,X,Y,Z), written by MatrixToQuaternion in Doit;
    // IVP_U_Quat is (x,y,z,w).
    CurrentTargetQuat.x = m_Orientation.x;
    CurrentTargetQuat.y = m_Orientation.y;
    CurrentTargetQuat.z = m_Orientation.z;
    CurrentTargetQuat.w = m_Orientation.w;

    // Delta = CurrentTargetPosition - Core->MWorldFCore.vv, the
    // three differences each stored to a float (dx at [ESP+0x8], dy [+0xc], dz [+0x10]).
    IVP_U_Point Delta;
    Delta.k[0] = CurrentTargetPosition.k[0] - Core->MWorldFCore.vv.k[0];
    Delta.k[1] = CurrentTargetPosition.k[1] - Core->MWorldFCore.vv.k[1];
    Delta.k[2] = CurrentTargetPosition.k[2] - Core->MWorldFCore.vv.k[2];

    // `TEST DH,0xc` on Body+0x78; clear -> rotate the body's
    // shift_core_f_object (+0x30) into world space (the routine, ECX = &Core->MWorldFCore)
    // and take it out (the routine, this = v1 - v2). Same block as the golem's
    // .
    if ((Body->flags & 0x0C00u) == 0)
    {
        IVP_U_Point ShiftWorld;
        Core->MWorldFCore.vmult(&Body->shift_core_f_object, &ShiftWorld);
        Delta.subtract(&Delta, &ShiftWorld);
    }

    // (dy*dy + dz*dz) + dx*dx on the x87 stack, never stored, then
    // + [this+0x60] * 0.9f (.rdata:the field, bytes 66 66 66 3f) and FSTP float. The
    // unstored intermediates are held in double, as CalcPositionError does.
    double const DeltaQuadLength =
        ((double)Delta.k[1] * Delta.k[1] + (double)Delta.k[2] * Delta.k[2]) + (double)Delta.k[0] * Delta.k[0];
    SmoothedPositionErrorSq = (float)(DeltaQuadLength + (double)SmoothedPositionErrorSq * (double)0.9f);

    // PUSH EDI (the event) / module storage CALL the routine with ECX = this.
    beam_to_current_target(EventSim);
}

// (module storage-062925b2, RET 0x8) -- slot 7 of module storage, the pure
// slot of IVP_Controller_Golem. Wakes the core, then beams for Reason 0 (module storage) and
// Reason 1 (module storage), nothing for anything else; returns 0 on every path
// (062925af / 0629259d XOR EAX,EAX).
int CRemotePhysicsController::beam_to_target(IVP_Event_Sim const* EventSim, int Reason)
{
    IVP_Controller_Manager_Ensure_Core_To_Be_In_Simulation(Core);

    switch (Reason)                             // SUB EAX,0 / JZ, DEC / JNZ
    {
    case 0:
        beam_to_current_target(EventSim);
        break;
    case 1:
        beam_to_current_target(EventSim);
        return 0;
    default:
        break;
    }
    return 0;
}

// PhySys::RCBegin, module storage-06285a5c (89 instructions). Param 5 (angular velocity)
// has no reference anywhere in the body. Object is not null-tested: both arms dereference
// it (module storage / module storage).
void __cdecl PhySys::RCBegin(CSimple* Object, Vector const& Pos, Vector const& Vel,
                             Quaternion const& Orient, Vector const& AngVel)
{
    (void)AngVel;

    // the routine on the stack block (which itself starts with
    // ) -- FLControllerGolemParms_Init is both.
    FLControllerGolemParms Parms;
    FLControllerGolemParms_Init(Parms);

    // . A stack IVP_U_Point {1e12f, 1e12f, 1e12f, ?} (0x5368d4a5 =
    // 1.0e12f) is copied four dwords into parms+0x10..+0x1c.
    // KNOWN baseline UB: the fourth dword, parms+0x1c, is that temporary's hesse slot
    // ([ESP+local_48], read at module storage and never written). Nothing reads parms+0x1c
    // (see the same note in AttachGolemController); the port keeps the 0.0f from the init.
    Parms.MaxTranslationForce[0] = 1.0e12f;
    Parms.MaxTranslationForce[1] = 1.0e12f;
    Parms.MaxTranslationForce[2] = 1.0e12f;
    Parms.MaxDeltaPosition = PhySys::RC_MAX_DELTA_POSITION;        // /06285993
    Parms.DeltaOrientation = PhySys::RC_MAX_DELTA_ORIENTATION;     // /06285997
    Parms.ForceFactor = 1.0f;
    Parms.DampFactor = 1.0f;
    Parms.TorqueFactor = 1.0f;
    Parms.AngularDampFactor = 1.0f;
    Parms.MaxTorque = 1.0e12f;

    // operator new(0x3c0) and baseline's own null test -- a failed
    // allocation stores 0 at +0x60 (module storage) and returns. `nothrow` gives the same
    // null instead of a throw, and skips construction on it as baseline's JZ does.
    IVP_Real_Object* const Body = Object->BodyPtr;
    CRemotePhysicsController* const Controller =
        new (std::nothrow) CRemotePhysicsController(Body, Parms, Pos, Orient, Vel);

    // `MOV [EBP+0x60],ESI` -- the controller's IVP_Controller subobject,
    // which is at offset 0 (the implicit upcast keeps a null a null).
    IVP_Controller* const AsController = Controller;
    Object->GolemController = AsController;
}

// PhySys::RCSet, module storage-06285b2e. No dynamic cast here: whatever is at +0x60 is
// taken to be a CRemotePhysicsController, and its CRemotePhysicsSimulation side (+0xd0,
// ) gets slot 1 -- RemoteUpdate -- with (Pos, Orient, Time). Object itself is
// not null-tested (module storage).
void __cdecl PhySys::RCSet(CSimple* Object, Vector const& Pos, Quaternion const& Orient, double Time)
{
    IVP_Controller* const Rc = Object->GolemController;
    if (Rc != nullptr)
    {
        CRemotePhysicsSimulation* const Sim =
            static_cast<CRemotePhysicsController*>(Rc);            // this + 0xd0
        Sim->RemoteUpdate(Pos, Orient, Time);                     // CALL [EDX+0x4]
    }
}

// PhySys::RCEnd, module storage-06285b4b: vtbl[6](1) on the +0x60 controller -- the
// virtual deleting destructor -- then +0x60 = 0. Object is not null-tested (module storage).
void __cdecl PhySys::RCEnd(CSimple* Object)
{
    IVP_Controller* const Rc = Object->GolemController;
    if (Rc != nullptr)
    {
        delete Rc;
        Object->GolemController = nullptr;
    }
}

// CRemotePhysicsSimulation's eight members that baseline compiled into Common.dll,
// Server.dll AND OutpostFrontier.exe -- GetSimulationTime, CalcPositionError,
// GetSampleCount, GetLatestUpdate, GetSample, SetLogging, the copy constructor and
// operator= -- are defined inline in src/include/common_server.h, the one place
// all three modules compile them from (each exports its own copy, as baseline's do).
// Only the seven Common-only members live in this file, below.

// ============================================================================
// CRemotePhysicsSimulation -- the seven members Common.dll exports and Server.dll
// does not. Derived instruction by instruction from Common;
// Common is NOT usable for this class (it drops the hidden by-value-return
// and out-matrix pointers on the routine/the routine/the math-engine vcalls and
// papers over the gap with a bogus CONCAT44 cast).

// .. module storage   tracef         50 instructions
// .. module storage   ctor           50 instructions
// .. module storage   dtor            6 instructions
// .. module storage   Reset          38 instructions
// .. module storage   RemoteUpdate  131 instructions
// .. module storage   Doit          265 instructions
// .. module storage   CheckForSync  156 instructions

// The instruction counts above exclude the 0x90 padding after each RET; the
// ranges are first instruction .. RET instruction.

// Four helper leaves recur throughout. All are trivial, so they are expanded by
// hand below rather than wrapped:
// __thiscall Vector* (Vector* Out, float X, float Y, float Z)
//                 -- three stores, RET 0xC. This port already has it as
//                 Vec3Assign (common_ini.h), which is what the code below calls.
// __cdecl void (Vector* Out, Vector const& A, Vector const& B)
//                 -- Out = A - B, RET (caller-cleaned).
// __thiscall float (Vector const* This) -- |*This|.
// the lazily created "3DMathEngine" NEURONCOMPONENTS singleton cached in
//                 .data:the field; this port already has it as
//                 Acquire3DMathEngine() (common_csys.h).

// Every floating-point literal below was decoded from the listing's own data
// bytes. analysis's inline "= XXh" annotation prints ONE byte -- often the first
// byte of an adjacent string -- and is worthless for these:
// 00 00 00 3F                  -> float  0.5f
//                    (the 3F is the '?' that starts the string at 0639c13f)
// AC C5 27 37                  -> float  1e-5f   (0x3727C5AC)
// 00 00 80 3F                  -> float  1.0f
// 00 00 00 00 00 00 F0 3F      -> double 1.0
// 00 00 00 00 00 00 24 40      -> double 10.0
// FC A9 F1 D2 4D 62 60 3F      -> double 0.002
// 00 00 00 00 00 40 7F 40      -> double 500.0
//   .data :063eb390  00 00 00 00 00 00 00 40      -> double 2.0
//   .data :063eb398  00 00 00 00 00 00 E0 3F      -> double 0.5
// The two string-mode literals tracef passes to fopen are likewise two bytes
// long, not one: module storage is "wt" (77 74 00) and module storage is
// "a+" (61 2B 00).
// ============================================================================

// and module storage, the two sync tolerances CheckForSync scales
// by its distance-derived BlendScale. They live in .data rather than .rdata, so
// baseline could in principle have a writer for them, but list_references and a
// raw grep of the listing both show exactly one reference each and it is a read
// (module storage and module storage, both inside CheckForSync). Their baseline
// image values are the bytes above -- 2.0 and 0.5 -- NOT zero.
// TODO(open): if a cross-module writer for either symbol ever turns up (they are
// not exported by name, so it would have to be an ordinal/address poke), this
// pair has to become a real settable global instead of a file-scope constant.
static double G_RemoteSyncPositionTolerance = 2.0;
static double G_RemoteSyncOrientTolerance   = 0.5;

// .data:the field -- the c:\OutpostServerTrace.txt handle tracef keeps open for the
// life of the process. File-scope, not function-local, to mirror baseline's global.
static FILE* G_RemotePhysTraceFile = nullptr;

// ?tracef@CRemotePhysicsSimulation@@QAAXPBDZZ, ordinal 3754.
// __cdecl varargs, so `this` arrives as the FIRST stack argument
// (module storage `MOV EAX,dword ptr [ESP + 0x4]`, then
// `MOV CL,byte ptr [EAX + 0x2e0]` == m_bLogging). MSVC gives a
// varargs member function __cdecl automatically, which is what the QAAX..ZZ
// mangling encodes, so the declaration in common_server.h already matches.

// The buffer is 0x400 bytes (module storage `SUB ESP,0x400`). The header comment
// on this member is right about the two destinations but wrong about both fopen
// modes; the raw .rdata bytes (see the table above) say "a+" and "wt".
void CRemotePhysicsSimulation::tracef(char const* Format, ...)
{
    // /module storage -- everything below is gated on m_bLogging.
    if (!m_bLogging)
        return;

    char Buffer[0x400];
    va_list Args;
    va_start(Args, Format);
    vsprintf(Buffer, Format, Args);
    va_end(Args);

    // CALL IsMPServer / TEST AL,AL / JNZ.
    if (!IsMPServer())
    {
        // .. module storage: one fopen/fprintf/fclose per call.
        // baseline hands fopen's result straight to fprintf and fclose with no null
        // check (module storage -> module storage -> module storage), and neither
        // does the port. tracef has NO caller anywhere in baseline -- no CALL to
        // in Common.dll, and neither Server.dll nor OutpostFrontier.exe
        // imports ?tracef@ -- nor in this tree; it exists only as an export.
        FILE* File = fopen("c:\\client.updates", "a+");
        fprintf(File, "%s", Buffer);   // == "%s"
        fclose(File);
        return;
    }

    // .. module storage: MP server path. The handle is opened once
    // and never closed; every call appends and flushes through it. The fputs
    // stream is the value fopen just returned (module storage `PUSH EAX`), the
    // fflush stream is re-read from the global (module storage) -- the same value.
    if (G_RemotePhysTraceFile == nullptr)
        G_RemotePhysTraceFile = fopen("c:\\OutpostServerTrace.txt", "wt");
    fputs(Buffer, G_RemotePhysTraceFile);
    fflush(G_RemotePhysTraceFile);
}

// ??0CRemotePhysicsSimulation@@QAE@XZ, ordinal 180.

// baseline builds the three CDeadReckonedVector sub-objects by hand: for each one
// it stores .rdata:CDeadReckonedVector::`vftable' into the sub-object's first
// dword and then calls CDeadReckonedVector::Reset (module storage/module storage,
// /module storage, module storage/module storage). That pair IS the
// inlined CDeadReckonedVector default constructor -- this port's own is literally
// `{ Reset(); }` (common_deadreckonedvector.cpp) plus the compiler's vptr store -- so the
// compiler's member construction expresses it exactly.

// The whole tail, module storage .. module storage, is instruction-for-instruction
// identical to Reset's body at module storage .. module storage (same four Vector
// zeroings through the same three stack temporaries, same six dword stores, same
// high half). Expressed here as a call to Reset rather than duplicated.

// baseline writes NOTHING at +0x29c (the Quaternion Doit rebuilds), nothing at
// +0x2bc/+0x2d0, and -- see below -- nothing at +0x2e0 either.
CRemotePhysicsSimulation::CRemotePhysicsSimulation()
{
    // baseline's constructor never touches +0x2e0, and neither does this one. The
    // full write set of module storage..06291d42 is: the three sub-objects, the
    // four Vectors at +0x278/+0x284/+0x290/+0x2ac, the dwords at +0x2b8/+0x2c0/
    // +0x2c4/+0x2c8/+0x2cc/+0x2d8/+0x2dc, and this's own vptr (module storage).
    // m_bLogging keeps whatever the allocation held until SetLogging writes it.
    // Its only reader is tracef, which nothing calls (see tracef above); the
    // copy constructor and operator= copy the byte as baseline's do.

    Reset();
}

// Copy constructor (Common 0x235b0 ord 179) and operator= (0x23b30 ord 742): inline
// in src/include/common_server.h, shared with Server.dll and OutpostFrontier.exe.


// ??1CRemotePhysicsSimulation@@UAE@XZ, ordinal 571. Six
// instructions: one MOV EAX to hold the constant, four vptr stores, one RET.
// The stores are this's own (module storage -> module storage, the 2-slot
// table) and the three sub-objects' (module storage/module storage/
// , all three to .rdata:CDeadReckonedVector::`vftable' at
// ) -- i.e. the three inlined, empty CDeadReckonedVector destructors.
// There is no other cleanup: the class owns no heap memory.

// The compiler emits this's own vptr restore and destroys the three members in
// reverse order with direct (non-virtual) calls to ~CDeadReckonedVector -- baseline's
// dtor likewise only WRITES the sub-objects' vptrs, never dispatches through them.
CRemotePhysicsSimulation::~CRemotePhysicsSimulation()
{
}

// ?Reset@CRemotePhysicsSimulation@@QAEXXZ, ordinal 1936.

// Re-primes only the trailing scratch fields. It does NOT touch the three
// sub-objects, does NOT touch the +0x29c Quaternion, and does NOT touch
// m_bLogging -- so Reset()ing a logging object leaves it logging.

// The four Vector zeroings go through three stack temporaries in baseline
// (module storage .. module storage) purely because the source wrote something
// like `m_Foo = Vector(0,0,0)` four times over and the compiler CSE'd the three
// zero constants; the observable effect is twelve zero floats.
void CRemotePhysicsSimulation::Reset()
{
    // +0x278 the velocity Doit publishes, +0x284 its value as of the last
    // RemoteUpdate ("blend from"), +0x290 RemoteUpdate's projection for it
    // ("blend to"), +0x2ac the mirror of the position DRV's ProjectedPosition.
    // +0x29c (the Quaternion between +0x290 and +0x2ac) is deliberately skipped.
    Vec3Assign(&m_Velocity, 0.0f, 0.0f, 0.0f);
    Vec3Assign(&m_BlendFromVelocity, 0.0f, 0.0f, 0.0f);
    Vec3Assign(&m_BlendToVelocity, 0.0f, 0.0f, 0.0f);
    Vec3Assign(&m_ProjectedPosition, 0.0f, 0.0f, 0.0f);

    m_SampleCount = 0;
    // /module storage zero the blend window that RemoteUpdate arms and
    // Doit compares against; +0x2d0 is left alone.
    m_ConvergeWindow = 0.0;
    m_SimulationTime = 0.0;                                 // /dee
    // stores 0 at +0x2d8 and module storage stores 0x40140000 at
    // +0x2dc -- together the double 5.0.
    m_SyncBackoff = 5.0;
}

// vtable slot 1 (module storage),
// ?RemoteUpdate@CRemotePhysicsSimulation@@UAEXABVVector@@ABVQuaternion@@N@Z,
// ordinal 1931. `RET 0x10` at module storage pins the arity at two references
// plus a double.

// This is the write half of the simulation: the only thing that ever feeds the
// three dead-reckoners a network sample. The position goes in verbatim; the
// orientation goes in as the first two COLUMNS of the rotation matrix
// I3DMathEngine::QuaternionToMatrix (vtable +0x54, module storage) builds from
// the sample quaternion. Resolving those column reads needs the interleaved
// pushes accounted for: with S = ESP after the three register saves, the first
// group is module storage [ESP+0x34], module storage [ESP+0x40] and
// [ESP+0x50] -- but `PUSH EBX` at module storage sits between the
// second and the third, so the real addresses are S+0x34, S+0x40 and S+0x4C, i.e.
// matrix elements [0][0], [1][0], [2][0] at stride 0x0C. The second group
// (module storage/module storage/module storage with `PUSH EBX` at
// ) resolves the same way to S+0x38, S+0x44, S+0x50 -- [0][1],
// [1][1], [2][1]. Two distinct vectors, not one, and neither is a velocity.
void CRemotePhysicsSimulation::RemoteUpdate(Vector const& Position,
                                            Quaternion const& Orientation,
                                            double Timestamp)
{
    // .. module storage. baseline leaves the 36-byte out matrix
    // uninitialised and never checks the lazily created singleton for null
    // (module storage CALL the routine, stored, then dereferenced at 06291e34);
    // neither does the port. Quaternion and FQuaternion are both (W,X,Y,Z).
    FMatrix Basis;
    I3DMathEngine* const Math = Acquire3DMathEngine();
    Math->QuaternionToMatrix(Basis, (FQuaternion const&)Orientation);

    m_PositionDRV.AddSample(Position, Timestamp);

    Vector AxisX;                                           // column 0
    AxisX.x = Basis.D[0][0];
    AxisX.y = Basis.D[1][0];
    AxisX.z = Basis.D[2][0];
    m_OrientAxisXDRV.AddSample(AxisX, Timestamp);

    Vector AxisY;                                           // column 1
    AxisY.x = Basis.D[0][1];
    AxisY.y = Basis.D[1][1];
    AxisY.z = Basis.D[2][1];
    m_OrientAxisYDRV.AddSample(AxisY, Timestamp);

    ++m_SampleCount;

    // .. module storage -- three dwords, +0x278 -> +0x284.
    m_BlendFromVelocity = m_Velocity;

    // .. module storage. Every field below is read AFTER AddSample
    // re-fitted the quadratic, so this is the new fit. The window is read at two
    // different precisions on purpose: module storage `FLD double ptr [ESP+0x10]`
    // keeps the double for the first multiply, while module storage
    // `FST float ptr [ESP+0x1c]` spills a float copy that module storage reloads
    // for the second. Reproduced literally rather than folded into one multiply.
    double const WindowD = m_PositionDRV.ConvergeWindow;    // this+0xC0 == DRV+0xB8
    float const WindowF = (float)WindowD;

    Vector const& Accel = m_PositionDRV.Acceleration;       // DRV+0x9C
    Vector const& Velocity = m_PositionDRV.Velocity;        // DRV+0x78

    // .rdata:the field == 0.5f (module storage/module storage/module storage).
    // Each product is an FSTP float argument to the routine, so float here.
    float const HalfAccelX = Accel.x * 0.5f;
    float const HalfAccelY = Accel.y * 0.5f;
    float const HalfAccelZ = Accel.z * 0.5f;

    // .. module storage -- FMUL by the double window on the
    // stack, then FSTP float as the next the routine's arguments.
    float const ScaledX = (float)(HalfAccelX * WindowD);
    float const ScaledY = (float)(HalfAccelY * WindowD);
    float const ScaledZ = (float)(HalfAccelZ * WindowD);

    // .. module storage, term by term. The three products by
    // WindowF are formed together, but they do NOT narrow alike:
    //   y: module storage/68 FMUL, then FSTP float [ESP+0x2c] -- rounded to float,
    //      reloaded at 06291f85 and FADD Velocity.y (DRV+0x7c);
    //   z: module storage/72 FMUL, then FSTP float [ESP+0x30] -- rounded to float,
    //      reloaded at 06291f90 and FADD Velocity.z (DRV+0x80);
    //   x: module storage/62 FMUL stays on the x87 stack and is added to
    //      Velocity.x (06291f78 FADD float [EDI+0x78]) UNROUNDED.
    // Each sum is then FSTP float. So x keeps the product in double.
    float const ProductY = ScaledY * WindowF;
    float const ProductZ = ScaledZ * WindowF;
    Vec3Assign(&m_BlendToVelocity,
               (float)((double)ScaledX * WindowF + Velocity.x),
               ProductY + Velocity.y,
               ProductZ + Velocity.z);

    // /module storage re-read DRV+0xB8 and store it at +0x2c8 --
    // the window Doit's blend test compares the simulation clock against.
    m_ConvergeWindow = m_PositionDRV.ConvergeWindow;
    // /module storage -- both halves of the double zeroed.
    m_SimulationTime = 0.0;
}

// ?Doit@CRemotePhysicsSimulation@@QAEXN@Z, ordinal 1381.
// `RET 0x8`, one double argument. Ends at module storage; the next function in
// the listing, the routine, is the Vector lerp helper this one calls, and three
// more unrelated functions (the routine, the branch, the routine) sit
// between that and CheckForSync at module storage -- the 550-line gap in the
// listing is five functions, not one.

// Four steps: advance the clock and the three dead-reckoners; renormalize both
// orientation dead-reckoners' projected AND tracked vectors (dead-reckoning a
// unit vector does not keep it unit); rebuild an orthonormal basis from the two
// projected axes and hand it to MatrixToQuaternion; publish a position.
void CRemotePhysicsSimulation::Doit(double Dt)
{
    m_SimulationTime += Dt;                    // / module storage
    m_PositionDRV.Doit(Dt);
    m_OrientAxisXDRV.Doit(Dt);
    m_OrientAxisYDRV.Doit(Dt);

    // .. module storage: four copies of the same inlined body --
    // CALL the routine (|v|), FCOM module storage (1e-5f), FNSTSW/TEST AH,0x41,
    // JNZ past the divide. TEST AH,0x41 masks C0|C3, so the JNZ is taken for
    // less-than, equal and unordered; the fall-through that actually divides is
    // reached only when the length is STRICTLY GREATER than 1e-5f; the length and
    // the FDIVR double-1.0 reciprocal stay on the x87 stack and only the three
    // products are FSTP float. the routine is the same (x^2+y^2)+z^2 / FSQRT
    // sequence as module storage, so this is instruction-for-instruction
    // CDeadReckonedVector::Normalize{Projected,Tracked}Position (common_deadreckonedvector.cpp,
    // double Len / InvLen), and they are called here.
    // Targets in `this` coordinates: +0x138, +0x144, +0x208, +0x214.
    m_OrientAxisXDRV.NormalizeProjectedPosition();
    m_OrientAxisXDRV.NormalizeTrackedPosition();
    m_OrientAxisYDRV.NormalizeProjectedPosition();
    m_OrientAxisYDRV.NormalizeTrackedPosition();

    // .. module storage -- Gram-Schmidt, then a cross product.
    Vector const& U = m_OrientAxisXDRV.ProjectedPosition;   // this+0x138
    Vector const& W = m_OrientAxisYDRV.ProjectedPosition;   // this+0x208

    // .. module storage, in that operand order (z, then y, then x):
    // (Wz*Uz + Wy*Uy) + Wx*Ux, left on the x87 stack -- it is never stored, so
    // it is a double here, not a float.
    double const Dot = ((double)W.z * U.z + (double)W.y * U.y) + (double)W.x * U.x;

    // .. module storage -- U*Dot, each product FSTP float as an
    // argument of the routine, which writes it into the slot at S2+0x30.
    Vector Proj;
    Vec3Assign(&Proj, (float)(U.x * Dot), (float)(U.y * Dot), (float)(U.z * Dot));

    // the routine(out, W, Proj): Out = A - B, each difference
    // FSTP float (module storage..06288821); the LAST push (module storage) is
    // the out pointer.
    Vector Perp;
    Vec3Assign(&Perp, W.x - Proj.x, W.y - Proj.y, W.z - Proj.z);

    // .. module storage. (Px^2 + Py^2) + Pz^2, FSQRT, then FDIVR
    // float 1.0f (module storage) -- the length and its reciprocal both stay on
    // the x87 stack; only the three products are FSTP float. baseline guards the
    // four normalizations above with an epsilon but NOT this one -- matched as-is.
    double const InvPerpLen =
        1.0f / sqrt(((double)Perp.x * Perp.x + (double)Perp.y * Perp.y) + (double)Perp.z * Perp.z);
    Vector V;
    Vec3Assign(&V, (float)(Perp.x * InvPerpLen), (float)(Perp.y * InvPerpLen),
               (float)(Perp.z * InvPerpLen));

    // .. module storage -- cross(U, V), written into the SAME slot
    // the Gram-Schmidt residual occupied (`LEA ECX,[ESP+0x30]` at module storage,
    // the out pointer for the the routine at module storage). It is NOT a dead
    // store: module storage/module storage/module storage read those three floats
    // straight back out and module storage/module storage/module storage place
    // them in the matrix's third column. Component order follows the three
    // FSUBP pairs literally (FSUBP ST1,ST0 == first product minus second). Both
    // products and their difference stay on the x87 stack; only the FSTP float
    // argument store narrows, so each component is one double expression here.
    Vector Cross;
    Vec3Assign(&Cross,
               (float)((double)V.z * U.y - (double)U.z * V.y),
               (float)((double)U.z * V.x - (double)V.z * U.x),
               (float)((double)V.y * U.x - (double)U.y * V.x));

    // .. module storage fill nine dwords at S2+0x3c..S2+0x5f:
    //   S2+0x3c=U.x  S2+0x40=V.x  S2+0x44=Cross.x
    //   S2+0x48=U.y  S2+0x4c=V.y  S2+0x50=Cross.y
    //   S2+0x54=U.z  S2+0x58=V.z  S2+0x5c=Cross.z
    // Row-major 3x3 whose COLUMNS are U, V and U x V -- a proper right-handed
    // orthonormal frame, which is what makes MatrixToQuaternion meaningful.
    FMatrix const Basis = {{{U.x, V.x, Cross.x},
                            {U.y, V.y, Cross.y},
                            {U.z, V.z, Cross.z}}};

    // `CALL dword ptr [EDX + 0x50]` -- MatrixToQuaternion, writing
    // directly into this+0x29c (EDI, loaded at module storage). The engine is
    // created lazily at module storage and never tested for null. Quaternion and
    // FQuaternion are both (W,X,Y,Z).
    I3DMathEngine* const Math = Acquire3DMathEngine();
    Math->MatrixToQuaternion((FQuaternion&)m_Orientation, Basis);

    // .. module storage -- this+0x2ac = PositionDRV.ProjectedPosition
    // (this+0x68 == +0x08 + 0x60).
    m_ProjectedPosition = m_PositionDRV.ProjectedPosition;

    // FLD this+0x2c0 / module storage FCOMP this+0x2c8, then
    // TEST AH,0x5 with JP at module storage. Mask 0x5 is C0|C2:
    // greater and equal both leave it zero (even parity -> JP taken), only
    // less-than sets exactly one bit (odd parity -> JP not taken). So the JP
    // target -- ProjectVelocity -- is the clock >= window case, and the
    // fall-through lerp is the strictly-inside-the-window case.
    // An unordered compare sets C0|C2 (even parity), so a NaN takes the JP to
    // ProjectVelocity, as `<` does.
    Vector Published;
    if (m_SimulationTime < m_ConvergeWindow)
    {
        // .. module storage -- the routine is
        // Out = (To - From) * t + From, with t spilled as a FLOAT at
        // even though the divide is done in double. Inside the
        // helper (module storage .. module storage) each component is FLD To,
        // FSUB From, FMUL t, FADD From on the x87 stack and only then FSTP
        // float -- one rounding per component, so double until the store.
        float const T = (float)(m_SimulationTime / m_ConvergeWindow);
        Vector const& From = m_BlendFromVelocity;
        Vector const& To = m_BlendToVelocity;
        Vec3Assign(&Published,
                   (float)(((double)To.x - From.x) * T + From.x),
                   (float)(((double)To.y - From.y) * T + From.y),
                   (float)(((double)To.z - From.z) * T + From.z));
    }
    else
    {
        // note the argument is m_SimulationTime, not Dt.
        Published = m_PositionDRV.ProjectVelocity(m_SimulationTime);
    }
    m_Velocity = Published;
}

// ?CheckForSync@CRemotePhysicsSimulation@@QAE_NABVVector@@0ABVQuaternion@@@Z,
// ordinal 1263. `RET 0xC`, three references, bool in AL.

// Answers "should the caller force a resync of this object?". Four independent
// reasons to say yes, in this order:
//   1. the simulation clock has outgrown the backoff window;
//   2. the position error exceeds 2.0 * BlendScale;
//   3. the world-X basis error exceeds 0.5 * BlendScale;
//   4. the world-Y basis error exceeds 0.5 * BlendScale.
// Reason 1 doubles the backoff and returns immediately WITHOUT reseeding it;
// reasons 2-4 reseed it to 5.0. That asymmetry is baseline's, not a transcription
// slip: module storage `FADD ST0,ST0` then `RET 0xC` at module storage never
// reaches the module storage/module storage pair that writes 5.0.
bool CRemotePhysicsSimulation::CheckForSync(Vector const& Param1,
                                            Vector const& Param2,
                                            Quaternion const& Param3)
{
    // FLD +0x2c0 / module storage FCOMP +0x2d8, then TEST AH,0x41
    // (C0|C3) with JNZ at module storage. The JNZ covers less-than, equal and
    // unordered, so the fall-through -- the branch that doubles and returns true
    // -- is reached only when the clock is STRICTLY GREATER than the backoff.
    if (m_SimulationTime > m_SyncBackoff)
    {
        m_SyncBackoff += m_SyncBackoff;
        return true;
    }

    // .. module storage -- the tolerance scale. Param1 and Param2
    // are two independently sampled positions for the same object; the further
    // apart they already are, the looser every threshold below becomes.
    //   BlendScale = 1.0
    //   if (|P1 - P2| > 500.0)  BlendScale = |P1 - P2| * 0.002   (== 1.0 at 500)
    //   if (BlendScale > 10.0)  BlendScale = 10.0                (saturates at 5000)
    // Both comparisons are TEST AH,0x41 + JNZ (module storage, module storage),
    // so both are strictly-greater; at exactly 500.0 the scale stays 1.0 either
    // way, and at exactly 10.0 the clamp does not fire.
    // The 1.0 seed is the split-dword store at module storage/module storage
    // (0 / 0x3ff00000) and the 10.0 clamp the pair at module storage/module storage
    // (0 / 0x40240000).
    double BlendScale = 1.0;
    {
        // baseline keeps these three differences in x87 registers at extended
        // precision (module storage .. module storage); an SSE2 build cannot, so
        // the port accumulates in double, which is the closest available.
        double const Dx = (double)Param1.x - Param2.x;
        double const Dy = (double)Param1.y - Param2.y;
        double const Dz = (double)Param1.z - Param2.z;
        // Summation order is baseline's, not x-y-z: module storage/06292620 square
        // Dz (ST0), module storage/06292624 square Dx (ST3), FADDP, then
        // /0629262a square Dy (ST2) and FADDP -- (Dz^2 + Dx^2) + Dy^2.
        double const SampleDist = sqrt((Dz * Dz + Dx * Dx) + Dy * Dy);
        if (SampleDist > 500.0)
        {
            BlendScale = SampleDist * 0.002;
            if (BlendScale > 10.0)
                BlendScale = 10.0;
        }
    }

    // .. module storage -- position error against the position
    // dead-reckoner's TrackedPosition (this+0x74 == +0x08 + 0x6c, the same field
    // CalcPositionError above reads). the routine rounds the three differences
    // to float before the magnitude is taken (FSTP float), so these are floats.
    bool bForceSync = false;                       // `XOR BL,BL`
    {
        Vector const& Tracked = m_PositionDRV.TrackedPosition;
        float const Ex = Param2.x - Tracked.x;
        float const Ey = Param2.y - Tracked.y;
        float const Ez = Param2.z - Tracked.z;
        // writes the difference at S+0x18 (x), +0x1c (y), +0x20 (z);
        // squares +0x1c, module storage squares +0x20, FADDP, then
        // squares +0x18 -- (Ey^2 + Ez^2) + Ex^2.
        double const PosError = sqrt(((double)Ey * Ey + (double)Ez * Ez) + (double)Ex * Ex);
        // FLD module storage (2.0) / module storage FMUL BlendScale
        // / module storage FCOMPP compares (2.0*BlendScale) against PosError, and
        // TEST AH,0x5 + JNP takes the resync branch on odd parity,
        // i.e. C0 alone, i.e. 2.0*BlendScale < PosError. Equality does not resync.
        if (PosError > G_RemoteSyncPositionTolerance * BlendScale)
            bForceSync = true;
    }

    if (!bForceSync)
    {
        // .. module storage -- QuaternionToMatrix (vtable +0x54)
        // on the caller's orientation, into an uninitialised 36-byte stack
        // matrix. The engine is created lazily (module storage) and never
        // tested for null.
        FMatrix Basis;
        I3DMathEngine* const Math = Acquire3DMathEngine();
        Math->QuaternionToMatrix(Basis, (FQuaternion const&)Param3);

        // FLD module storage (0.5) / module storage FMUL BlendScale.
        // The same product is reused for both axis tests -- module storage and
        // are two FCOMP ST1 against the one value left on the x87
        // stack, never recomputed.
        double const AxisThreshold = G_RemoteSyncOrientTolerance * BlendScale;

        // Column 0 of the matrix (elements [0][0], [1][0], [2][0], read at
        // /module storage/module storage) against the world-X
        // dead-reckoner's TrackedPosition, this+0x144 == +0xd8 + 0x6c.
        Vector const& AxisXTracked = m_OrientAxisXDRV.TrackedPosition;
        float const Xx = Basis.D[0][0] - AxisXTracked.x;
        float const Xy = Basis.D[1][0] - AxisXTracked.y;
        float const Xz = Basis.D[2][0] - AxisXTracked.z;
        // (module storage) stores the difference at S+0x18/+0x1c/+0x20;
        // squares +0x1c, module storage squares +0x20, FADDP, then
        // squares +0x18 -- (Xy^2 + Xz^2) + Xx^2.
        double const AxisXError = sqrt(((double)Xy * Xy + (double)Xz * Xz) + (double)Xx * Xx);
        // TEST AH,0x41 + JNZ at module storage continues to the
        // second axis on less-or-equal; the fall-through jumps to the resync
        // block, so the resync condition is strictly-greater.
        if (AxisXError > AxisThreshold)
        {
            bForceSync = true;
        }
        else
        {
            // Column 1 ([0][1], [1][1], [2][1], read at module storage/
            // /module storage) against the world-Y dead-reckoner's
            // TrackedPosition, this+0x214 == +0x1a8 + 0x6c.
            Vector const& AxisYTracked = m_OrientAxisYDRV.TrackedPosition;
            // No the routine on this axis: only Yx is spilled as a float
            // (module storage FSTP float [ESP+0x18]); Yy (module storage) and Yz
            // (module storage) stay on the x87 stack, so they are doubles here.
            // squares Yz, module storage squares Yy, FADDP,
            // squares the spilled Yx -- (Yz^2 + Yy^2) + Yx^2.
            float const Yx = Basis.D[0][1] - AxisYTracked.x;
            double const Yy = (double)Basis.D[1][1] - AxisYTracked.y;
            double const Yz = (double)Basis.D[2][1] - AxisYTracked.z;
            double const AxisYError = sqrt((Yz * Yz + Yy * Yy) + (double)Yx * Yx);
            // TEST AH,0x41 + JNZ at module storage returns BL
            // (still false) on less-or-equal.
            if (AxisYError > AxisThreshold)
                bForceSync = true;
        }
    }

    if (bForceSync)
    {
        // /module storage -- 0 and 0x40140000, the double 5.0.
        m_SyncBackoff = 5.0;
    }
    return bForceSync;   // `MOV AL,BL`
}

