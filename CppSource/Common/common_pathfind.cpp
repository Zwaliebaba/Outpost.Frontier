// Author: Giperion.

// Common.dll -- PathfindManager (common_pathfind.h): the per-ship AI path / obstacle state
// IBehaviorManager owns at +0x0C (baseline run 0x062db3b0-0x062de160), together with the
// user-zone probe pair the routine / the routine and the two oriented-box helpers
// / the routine that the probe and the zone shapes (common_zone.cpp) share.

// Split out of common_aisteer.cpp and common_stubs.cpp (CMN-SPLIT stage 1, 2026-10-14);
// the bodies are unchanged.

#include "common_pathfind.h"
#include "common_geometry_internal.h" // ClampPointToBoxSurface, MathTransposeTransformVia3DMathEngine
#include "common_aisteer.h"
#include "common_behavior.h"      // GetIfacePositionOrZero
#include "common_cequip.h"        // IObject::ThrustEquipEngine (PathfindManager ctor)
#include "common_cobjdb.h"        // IObjDB::GetZoneDb (SetPathfindObjectDb, the routine)
#include "common_csys.h"          // Acquire3DMathEngine, Matrix
#include "common_iface.h"         // IBehaviorManager, IObjRW, ObjRwFromWatchable
#include "common_misc.h"          // FastInvSqrt, SUBOBJ_ID_NONE
#include "common_systemmanager.h" // SystemManager (Owner->Sys)
#include "common_universe.h"      // Universe::ISystem::GetSphereProperties (PathfindManager::Update's zone probe), GetId
#include "x86math.h"              // I3DMathEngine::TransposeTransform (vtable +0x38)
#include "common_aisteer_internal.h" // CmnAiSteer::PathfindOwnerObject

#include <cmath>
#include <cstdlib>
#include <cstring>

using CmnAiSteer::PathfindOwnerObject;

// ============================================================================
// PathfindManager (common_pathfind.h)
// ============================================================================

void PathfindManager::ObstacleReaction::ResetState()
{
    // (Common), all 9 instructions, in baseline's order.
    State = 3;
    LastObstacleTag = 0;
    Heading.z = 0.0f;   // [ECX+0x10]
    Heading.y = 0.0f;   // [ECX+0x0c]
    Heading.x = 0.0f;   // [ECX+0x08]
    bInReaction = false;
    bCloseEncounter = false;
}

PathfindManager::PathfindManager(IBehaviorManager* NewOwner)
    // Members are built in declaration order, which is baseline's order for every member with
    // a constructor: Zone (the routine, module storage), DeflectionSlots (empty element
    // ctor, module storage), Watcher1C8 (inline zero, module storage), Watcher218
    // (the routine with a null object == the default BaseWatcher, module storage),
    // Avoidance (module storage-062db4db).
{
    // (Common), all 224 instructions traced. The stores
    // below follow the listing's order; the member constructors above have already run.
    bUserZoneActive = false;                         // (before Zone's ctor in baseline)
    for (int Index = 0; Index < 8; ++Index)          // REP STOSD x8
    {
        RecentObstacleIds[Index] = 0;
    }
    NextDeflectionSlot = 0;
    Reaction.ResetState();                           // the routine(this+0x16c)

    Watcher1C8.set(nullptr);                         // BaseWatcher::set(0)
    Timer1E0 = 0.0f;
    Period1E4 = 1.0f;
    Timer1E8 = 0.0f;
    Period1EC = 1.0f;
    Timer20C = 0.0f;
    Period210 = 1.0f;

    Field_214 = 0;
    Watcher218.set(nullptr);                         // BaseWatcher::set(0)
    Field_220.z = 0.0f;
    Field_220.y = 0.0f;
    Field_220.x = 0.0f;
    Field_22C.z = 0.0f;
    Field_22C.y = 0.0f;
    Field_22C.x = 0.0f;
    Field_238 = false;
    Field_23C = 1.0f;
    Field_240 = 0;

    CachedObstacleId = 0;
    bHasCachedObstacle = false;

    Owner = NewOwner;
    Field_1F0 = 0;
    Flag245 = 1;
    Field_248 = -1;
    Field_24C = 1;
    Field_250 = 0x7f0000;
    Field_254 = 1;
    Field_064 = 0;
    Watcher1C8.set(nullptr);                         // `if ([+0x1c8]) set_pointer(0)`
    Field_1D0.z = 0.0f;
    Field_1D0.y = 0.0f;
    Field_1D0.x = 0.0f;
    Field_1DC = 0;
    Field_000 = 0;
    MaxEngineSpeed = 80.0f;                          // (0x42a00000)
    Field_1F8 = 0.0f;
    Field_1FC = 0.0f;

    // the owner object's get_type into OwnerType (result dropped).
    PathfindOwnerObject(Owner)->get_type(OwnerType);

    Avoidance.AttachOwner(Owner);                    // the routine(this+0x268, Owner)

    Avoidance.PhysSystemId = 0;
    CachedTargetPosition.z = 0.0f;
    CachedTargetPosition.y = 0.0f;
    CachedTargetPosition.x = 0.0f;
    Period1E4 = 1.0f;

    // three phase offsets, each `(float)rand() * (1/32768)
    // (the field == 0x38000000) * Period`, rand() called in this order.
    Timer1E0 = static_cast<float>(std::rand()) * 3.0517578e-05f * Period1E4;
    Period1EC = 0.5f;                                // (0x3f000000)
    Timer1E8 = static_cast<float>(std::rand()) * 3.0517578e-05f * Period1EC;
    Period210 = 0.1f;                                // (0x3dcccccd)
    Timer20C = static_cast<float>(std::rand()) * 3.0517578e-05f * Period210;

    Field_244 = false;
    Field_004 = 0;
    Field_255 = false;
    Field_258 = 0;
    Field_25C = 0;
    BlendTimer = 0.0f;
    bBlendEnabled = false;

    // get_speed(1.0f, ThrustEquipEngine) (+0xd8; pushes 1 then 1.0f). FST keeps
    // it in MaxEngineSpeed; FCOMP -1.0f (the field == 0xbf800000) / TEST AH,0x44 / JP:
    // the jump to the two distance queries is taken on even C3|C2 parity -- not equal, or
    // unordered. Only an exact -1.0f zeroes both distances.
    float const Speed = PathfindOwnerObject(Owner)->get_speed(1.0f, IObject::ThrustEquipEngine);
    MaxEngineSpeed = Speed;
    if (Speed == -1.0f)
    {
        Field_1F8 = 0.0f;
        Field_1FC = 0.0f;
    }
    else
    {
        // / 062db700: get_distance_travelled (+0xd0) with pushes
        // (Engine, 1000.0f (the field), Speed or 300.0f (0x43960000), 0) -- the last
        // push, 0, is the first argument.
        Field_1F8 = PathfindOwnerObject(Owner)->get_distance_travelled(
            0.0f, Speed, 1000.0f, IObject::ThrustEquipEngine);
        Field_1FC = PathfindOwnerObject(Owner)->get_distance_travelled(
            0.0f, 300.0f, 1000.0f, IObject::ThrustEquipEngine);
    }

    BlendStart.x = 0.0f;
    BlendStart.y = 1.0f;                             // (EBP == 0x3f800000)
    BlendStart.z = 0.0f;
}

// , Common (21 insns), thiscall, `RET 4`. Sole caller is
// ConnectBehaviorManagerObjectDb (the routine, module storage).
void SetPathfindObjectDb(PathfindManager* Self, IObjDB* ObjectDb)
{
    if (ObjectDb == nullptr)
    {
        Self->Avoidance.PhysSystemId = 0;
        return;
    }

    // `[ObjectDb]->+0x08` (IObjDB::GetZoneDb), then slot 0 of the
    // object it returns -- the whole pair runs twice and only the second result is stored.
    // The zone database is the system's Universe::ISystem; its slot 0 is the system id (the
    // same chain CheckLauncherLineOfFireCached walks).
    Universe::ISystem const* ZoneDb = ObjectDb->GetZoneDb();
    (void)ZoneDb->GetId();
    ZoneDb = ObjectDb->GetZoneDb();
    Self->Avoidance.PhysSystemId = ZoneDb->GetId();
}

void PathfindManager::clear_user_zone()
{
    // (Common): `MOV byte ptr [this+0x8],0; RET`.
    bUserZoneActive = false;
}

void PathfindManager::submit_user_zone(UserZone const& NewZone)
{
    // (Common): byte [this+0x8] = 1, then
    // `LEA EDI,[this+0xc]; MOV ECX,0x16; REP MOVSD` -- the 0x58-byte zone copied whole.
    bUserZoneActive = true;
    std::memcpy(&Zone, &NewZone, sizeof(UserZone));
}

bool PathfindManager::get_user_zone(UserZone& OutZone)
{
    // (Common): when byte [this+0x8] is set, the 0x16-dword
    // copy from this+0xc into OutZone and true; otherwise OutZone is untouched and false.
    if (bUserZoneActive)
    {
        std::memcpy(&OutZone, &Zone, sizeof(UserZone));
        return true;
    }
    return false;
}

void PathfindManager::SetCruiseLock(int Value, unsigned int Mask)
{
    // (Common), all 108 instructions traced.
    unsigned char const Byte = static_cast<unsigned char>(Value);   // MOV CL,byte
    if ((Mask & 2) != 0)                                            // TEST AL,0x2 / JZ
        Field_254 = Byte;
    unsigned int const Bit0 = Mask & 1;                             // AND EAX,0x1
    if (Bit0 != 0)
        Field_24C = Byte;
    if (Byte != 0)                                                  // CMP CL,BL / JNZ
        return;

    // bit 0 given and Field_214 bit 0 set -> the routine on the owner's
    // ThrustManager (IBehaviorManager+0x08 -> SystemManager, +0x18) with a zeroed stack Vector.
    if (Bit0 != 0 && (Field_214 & 1) != 0)
    {
        Vector Zero;
        Zero.x = 0.0f;
        Zero.y = 0.0f;
        Zero.z = 0.0f;
        Owner->Sys->Thrust.SetNudgeDirection(Zero);
    }

    // the +0x214 record is assigned from a default-constructed
    // temporary. The temporary's watcher is zeroed and BaseWatcher::set(0) on it is a no-op,
    // so its two `if (watched) set_pointer(0)` branches (062dd472, 062dd556) are never
    // taken. The assignment, field by field:
    Field_214 = 0;
    // the watcher half -- EDX = source target (0 here), `CMP EDX,[ECX];
    // JZ; set_pointer(EDX)`, i.e. BaseWatcher::set(nullptr).
    Watcher218.set(nullptr);
    Field_220.x = 0.0f;
    Field_220.y = 0.0f;
    Field_220.z = 0.0f;
    Field_22C.x = 0.0f;
    Field_22C.y = 0.0f;
    Field_22C.z = 0.0f;
    Field_238 = false;
    Field_23C = 1.0f;                                               // (0x3f800000)
    Field_240 = 0;
}

void PathfindManager::Update(float Dt)
{
    // (Common), all 173 instructions traced. Clock sums
    // stay on the x87 stack for the compare (double here); the stored timers are floats.
    if ((OwnerType & 0x7f0000) == 0)
    {
        return;
    }

    // EBX = the owner's CurrentBehaviorIndex, read before the probe.
    // Timer1E0 += Dt; the unrounded sum against Period1E4 (TEST AH,0x41 / JNZ skips <=, ==
    // and unordered).
    int const BehaviorIndex = Owner->CurrentBehaviorIndex;
    double const ZoneClock = static_cast<double>(Dt) + Timer1E0;
    float const ZoneClockStored = static_cast<float>(ZoneClock);
    Timer1E0 = ZoneClockStored;
    if (ZoneClock > Period1E4)
    {
        // fmod of the stored clock.
        Timer1E0 = static_cast<float>(std::fmod(static_cast<double>(ZoneClockStored),
                                                static_cast<double>(Period1E4)));

        // zone properties of a 20.0f sphere around the owner --
        // get_position (slot 0), then ObjectDb->GetZoneDb() (+0x08), Universe::ISystem
        // slot 8 (+0x20) on {position, 20.0f}.
        Vector const& Position = PathfindOwnerObject(Owner)->get_position();
        Universe::ISystem const* const System = Owner->ObjectDb->GetZoneDb();
        Geometry::Sphere Probe;
        Probe.Center = Position;
        Probe.Radius = 20.0f;
        unsigned const ZoneProps = System->GetSphereProperties(&Probe);

        // Field_004 = the properties, Field_000 = 1 when any of 0x7000 is
        // set (TEST AH,0x70), | 8 while the avoidance holds a contact.
        Field_004 = static_cast<int>(ZoneProps);
        Field_000 = 0;
        if ((ZoneProps & 0x7000) != 0)
        {
            Field_000 = 1;
        }
        if (Avoidance.HasContact == 1)
        {
            Field_000 |= 8;
        }
    }

    // behaviours 6, 8, 0x13 and none drop the obstacle state -- the five
    // stores of the routine inlined in its order, then the routine on +0x16c.
    if (BehaviorIndex == 6 || BehaviorIndex == 8 || BehaviorIndex == 0x13 || BehaviorIndex == -1)
    {
        Avoidance.ResetContact();
        Reaction.ResetState();
        return;
    }

    // EBX = Field_214 before the refresh; Timer20C against Period210.
    int const PreviousRecord = Field_214;
    double const RecordClock = static_cast<double>(Dt) + Timer20C;
    float const RecordClockStored = static_cast<float>(RecordClock);
    Timer20C = RecordClockStored;
    if (RecordClock > Period210)
    {
        Timer20C = static_cast<float>(std::fmod(static_cast<double>(RecordClockStored),
                                                static_cast<double>(Period210)));
        RefreshAvoidance();                                         // the routine
    }

    // a live nudge record is blended into the ThrustManager
    // (the routine) when Field_244 allows; one that has just ended hands it a zero vector
    // (the routine).
    if ((Field_214 & 1) != 0)
    {
        if (Field_244)
        {
            Owner->Sys->Thrust.BlendNudgeDirection(Field_220, Dt);
        }
    }
    else if ((PreviousRecord & 1) != 0)
    {
        Vector Zero;
        Zero.x = 0.0f;
        Zero.y = 0.0f;
        Zero.z = 0.0f;
        Owner->Sys->Thrust.SetNudgeDirection(Zero);
    }


    if (Flag245 == 0 || Field_254 == 0)
    {
        return;
    }

    UpdateHeadingBlend(Dt);                                         // the routine

    // the avoidance runs on its own clock and is handed its period as
    // its Dt; the period becomes 0.1f (the field) while it is active or has a contact,
    // 0.5f otherwise.
    double const AvoidClock = static_cast<double>(Dt) + Timer1E8;
    float const AvoidClockStored = static_cast<float>(AvoidClock);
    Timer1E8 = AvoidClockStored;
    if (AvoidClock > Period1EC)
    {
        Timer1E8 = static_cast<float>(std::fmod(static_cast<double>(AvoidClockStored),
                                                static_cast<double>(Period1EC)));
        Avoidance.Update(Period1EC);                                // the routine
        if (Avoidance.Active != 0 || Avoidance.HasContact != 0)
        {
            Period1EC = 0.1f;
        }
        else
        {
            Period1EC = 0.5f;
        }
    }

    // is_player (+0xbc) on the owner object, result dropped; no null test.
    (void)PathfindOwnerObject(Owner)->is_player();
}

void PathfindManager::RefreshAvoidance()
{
    // (Common), all 155 instructions traced.
    // the record cleared, then SelectAvoidancePoint into +0x22c / +0x23c.
    Watcher218.set(nullptr);                                        // `if (watched) set_pointer(0)`
    Field_214 = 0;
    Field_22C.z = 0.0f;
    Field_22C.y = 0.0f;
    Field_22C.x = 0.0f;
    Field_220.z = 0.0f;
    Field_220.y = 0.0f;
    Field_220.x = 0.0f;
    Field_23C = 1.0f;
    Field_240 = 0;
    int Kind;
    if (SelectAvoidancePoint(&Field_22C, &Field_23C, &Kind) == 0)
    {
        // a point record.
        Field_238 = Avoidance.HasContact;
        Watcher218.set(nullptr);
        Field_214 = 2;
        Field_240 = Kind;
        // get_center_of_mass (slot 5, +0x14) into a local that is never read.
        (void)PathfindOwnerObject(Owner)->get_center_of_mass();
        return;
    }

    // only owner types in 0x130000 look for an obstacle to nudge around.
    if ((OwnerType & 0x130000) == 0)
    {
        return;
    }
    IObjRW* Obstacle = nullptr;
    Vector Direction;
    if (SelectObstacleDirection(&Direction, &Obstacle, static_cast<unsigned>(Field_064)) != 0)
    {
        return;
    }

    // watch the obstacle (its Watchable at +8, 0 for none); Field_240 = 4.
    Watcher218.set(Obstacle != nullptr ? static_cast<Watchable const*>(Obstacle) : nullptr);
    Field_240 = 4;

    // the owner's cruise state (is_cruise_active, slot 24 +0x60) and the
    // watched obstacle's type (get_type, +0x88) when one is watched.
    bool bCruise = false;
    PathfindOwnerObject(Owner)->is_cruise_active(bCruise);
    unsigned ObstacleType = 0;
    if (Watcher218.target() != nullptr
        && ObjRwFromWatchable(const_cast<Watchable*>(Watcher218.target())) != nullptr)
    {
        ObjRwFromWatchable(const_cast<Watchable*>(Watcher218.target()))->get_type(ObstacleType);
    }

    // Field_244 = !cruise || obstacle type in 0x7f0000; when clear, the
    // direction goes straight to the ThrustManager (the routine). Field_220 = direction,
    // Field_214 = 1.
    SystemManager* const Sys = Owner->Sys;
    bool const bBlend = (!bCruise || (ObstacleType & 0x7f0000) != 0);
    Field_244 = bBlend;
    if (!bBlend)
    {
        Sys->Thrust.SetNudgeDirection(Direction);
    }
    Field_220 = Direction;
    Field_214 = 1;
}

int PathfindManager::SelectAvoidancePoint(Vector* OutPoint, float* OutScale, int* OutKind)
{
    // (Common), all 208 instructions traced.
    if (Flag245 == 0 || Field_254 == 0)
    {
        return 2;
    }

    // EDI = the owner object; the user-zone exit direction first.
    IObjRW* const Ship = PathfindOwnerObject(Owner);
    Vector ExitDirection;
    if (GetUserZoneApproachDirection(&ExitDirection) == 0)
    {
        // kind 2, the point 100 units along the exit direction from the
        // owner (get_orientation +0x10, folded with get_transform). *OutScale untouched.
        *OutKind = 2;
        Field_25C = 2;
        Transform const& Xform = Ship->get_transform();
        Vector const Reach{ ExitDirection.x * 100.0f, ExitDirection.y * 100.0f, ExitDirection.z * 100.0f };
        *OutPoint = Vector{ Reach.x + Xform.Translation.x, Reach.y + Xform.Translation.y,
                            Reach.z + Xform.Translation.z };
        return 0;
    }

    if (Reaction.State != 3)
    {
        // kind 3, the point 100 units along the reaction heading;
        // x and y are summed unrounded, z is rounded first (FSTP to a local).
        IObjRW* const Self = PathfindOwnerObject(Owner);
        Transform const& Xform = Self->get_transform();
        double const ReachX = static_cast<double>(Reaction.Heading.x) * 100.0f;
        double const ReachY = static_cast<double>(Reaction.Heading.y) * 100.0f;
        float const ReachZ = static_cast<float>(static_cast<double>(Reaction.Heading.z) * 100.0f);
        OutPoint->x = static_cast<float>(ReachX + Xform.Translation.x);
        OutPoint->y = static_cast<float>(ReachY + Xform.Translation.y);
        OutPoint->z = ReachZ + Xform.Translation.z;

        // the routine(owner, point, 0, 0) into *OutScale.
        *OutScale = static_cast<float>(EstimateTurnCircleApproachCost(Self, *OutPoint, 0.0f, 0.0f));

        // in a close encounter, a heading less than 0.93f (the field;
        // TEST AH,0x5 / JP skips >=, unordered) off the owner's forward (-column 2) zeroes it.
        if (Reaction.bCloseEncounter)
        {
            float const ForwardX = -Xform.Rotation.E[0][2];
            double const Dot = (static_cast<double>(-Xform.Rotation.E[2][2]) * Reaction.Heading.z
                                + static_cast<double>(-Xform.Rotation.E[1][2]) * Reaction.Heading.y)
                               + static_cast<double>(ForwardX) * Reaction.Heading.x;
            if (Dot < 0.93f)
            {
                *OutScale = 0.0f;
            }
        }

        // kind 3; a point within 1 of the origin ((x*x + y*y) + z*z <=
        // 1.0, double, TEST AH,0x41 / JNZ) still answers 2.
        *OutKind = 3;
        Field_25C = 3;
        double const LengthSq = static_cast<double>(OutPoint->x) * OutPoint->x
                                + static_cast<double>(OutPoint->y) * OutPoint->y
                                + static_cast<double>(OutPoint->z) * OutPoint->z;
        if (!(LengthSq > 1.0))
        {
            return 2;
        }
        return 0;
    }

    // the avoidance point, unless its contact is the excluded Field_064.
    Vector Point;
    unsigned ObjectId;
    if (Avoidance.GetAvoidPoint(&Point, &ObjectId) && ObjectId != static_cast<unsigned>(Field_064))
    {
        *OutPoint = Point;
        *OutScale = Avoidance.GetCloseRateScale();                 // the routine
        *OutKind = 1;
        Field_25C = 1;
        return 0;
    }
    *OutKind = 0;
    Field_25C = 0;
    return 2;
}

int PathfindManager::SelectObstacleDirection(Vector* OutDirection, IObjRW** OutObstacle, unsigned ExcludeId)
{
    // (Common), all 89 instructions traced.
    if (Flag245 == 0 || Field_24C == 0)
    {
        return 2;
    }

    // zero the direction, Field_255 = 0, then the closest obstacle on
    // the SystemManager's timer table (the routine) excluding ExcludeId, type mask
    // Field_248.
    SystemManager* const Sys = Owner->Sys;
    OutDirection->z = 0.0f;
    OutDirection->y = 0.0f;
    OutDirection->x = 0.0f;
    Field_255 = false;
    IObjInspect* const Obstacle =
        FindClosestObstacleToAvoid(&Sys->Timers, ExcludeId, static_cast<unsigned>(Field_248));
    if (Obstacle != nullptr)
    {
        // the shape-biased direction around it (the routine on
        // Sys+0x14fc); Field_258 = its get_id (+0x20).
        Vector Direction;
        *OutDirection = *ComputeShapeBiasedAvoidDirection(&Direction, &Sys->SelfTarget, Obstacle);
        *OutObstacle = static_cast<IObjRW*>(Obstacle);
        Field_255 = true;
        Field_258 = static_cast<int>(Obstacle->get_id());
        return 0;
    }

    // otherwise the avoidance direction and its contact, looked up by id
    // in the object database (IObjDB::FindById, +0x04).
    unsigned ObjectId = 0;
    if (Avoidance.GetAvoidDirection(OutDirection, &ObjectId))
    {
        *OutObstacle = Owner->ObjectDb->FindById(&ObjectId);
        Field_255 = true;
        Field_258 = static_cast<int>(ObjectId);
        return 0;
    }
    return 2;
}

// ============================================================================
// THE AI PROXIMITY-VOLUME PROBE FAMILY -- items 1674 (the routine), 1673
// (the routine), 1672 (the routine), 1686 (the routine).
// PORTED FOR REAL 2026-09-17 (claude:common-dnt-audit), from the raw listing
// only: the analysis pseudo-C for the routine is self-evidently broken (item
// 1674's own note records the uninitialized-array-write artifact) and the
// tracker notes for 1672/1673/1686 all pre-date the receiver being resolved.

// WHY THEY ARE PORTABLE NOW -- BOTH STATED BLOCKERS WERE WRONG.
//   (a) Items 1672/1686 claimed "owning embedded sub-object (element+0xc within
//       the still-unbuilt 0x88-stride AI-list-element) is a Wave-4/FULLPORT-
//       DEFERRED[PUB] prerequisite". That reads a "+0xc" out of the wrong
//       object. The receiver is NOT inside the 0x88-stride list element -- it
//       cannot be, since these functions dereference `this+0x1c0`, which is
//       past the end of a 0x88-byte record. Traced from the raw bytes at
// 's own call site:
//         Common  MOV EAX,[ESI+0x4]     ; SystemManager
//                                       MOV ECX,[EAX+0x4]     ; IBehaviorManager
//                                       MOV EDX,[ECX+0xc]     ; <-- the receiver
//         Common         MOV [ESP+0x18],EDX    ; frame+0x18
//         Common  LEA ECX,[ESP+0x60] / PUSH ECX /
//                                       MOV ECX,[ESP+0x1c]   ; == frame+0x18
//                                       CALL the routine
//         Common  ... MOV ECX,[ESP+0x20] ; == frame+0x18
//                                       CALL the routine
//       (ESP is at the frame base at both call sites: SUB ESP,0x11c + 4 pushes
//       == 0x12c, which is exactly the bias analysis's own `local_114+0x12c`
//       annotation at 254080 uses, and the only intervening pushes are the
//       out-param ones counted above.) So BOTH calls take the same receiver,
//       `*(void**)(IBehaviorManager + 0x0c)`. Independently corroborated by a
//       sibling lane: common_dodgeidea.cpp (split out of common_aisteer.cpp) records
//       `thunk_FUN_062ddcd0(ECX = *(Manager+0xc))` for the SteeringTask
//       siblings' own call sites, derived without reference to this cluster.
//       IBehaviorManager is ported (common_iface.h), so the receiver is
//       reachable today; nothing about the 0x88-stride element is involved.
//   (b) The "0x062d2000-0x062ea000 DO-NOT-TOUCH range" cited against them is
//       retired -- see the cluster header comment above for the census.

// THE RECEIVER'S LAYOUT, as established by these four bodies plus the two
// call sites. The receiver `*(IBehaviorManager+0x0c)` is the
// PathfindManager (common_pathfind.h: IBehaviorManager::Pathfind; +0x08 is its
// bUserZoneActive, +0x0C its UserZone, +0x1C0 its Owner), so the two probes are
// PathfindManager members (CMN-SPLIT 2026-10-14). The bodies still address the
// fields as byte offsets on `this`; the offsets below match common_pathfind.h:
//   +0x008 bool   Active        -- probe disabled when clear
//   +0x00C int    Mode          -- 0 = point+radius sphere, 1 = oriented box,
//                                  2 = live-interface target, >=3 = inert
//   +0x010 int    Invert        -- dword, tested as a whole (not a byte)
//   +0x014 Vector TargetPoint   -- mode 0's target; mode 2 caches into it
//   +0x020 float  Radius        -- mode 0's sphere radius / mode 0's distance bias
//   +0x024 Matrix BoxRotation  \
//   +0x048 Vector BoxCenter     |- a 0x3C-byte oriented box, MATRIX-FIRST order
//   +0x054 Vector BoxHalfExtent/   (deliberately NOT common_ini.h's OrientedBox,
//                                   which puts Center first -- see below)
//   +0x060 Zone.TargetKey      -- an object id; its ADDRESS goes to IObjDB::FindById
//   +0x1C0 IBehaviorManager* Owner -- Owner+0x0F0 is a Watchable* (object at -8),
//                                 Owner+0x0EC is IBehaviorManager::ObjectDb (IObjDB*)
// The box's field order is pinned by the routine's own reads: it is called
// with ECX = This+0x24 (Common, 229312, 225974) and reads
// [ECX+0x30]/[ECX+0x34]/[ECX+0x38], i.e. This+0x54..0x5C, as the half-extents;
// while the routine is called with This+0x48 as the point to subtract
// (Common, 225957) and the routine with This+0x24 as the
// matrix. Matrix at +0x00, Center at +0x24, HalfExtent at +0x30, total 0x3C.

// Shared leaf helpers used below and named in the raw listing:
// (Out, A, B)  -- __cdecl Vec3Sub: *Out = *A - *B.
// (Out, ByVal) -- __thiscall Vec3Assign (Vector passed by value
//                               in 12 bytes of stack).
// Both are trivial and are written out inline here rather than called through
// a forward declaration, matching this file's existing practice for them.
// ============================================================================

// Common (item 1674, the routine, 25 insns). __cdecl
// (Vector* Out, Matrix const* M, Vector const* Src) -- three plain stack args,
// no ECX, and the caller cleans (Common / 225969 clean 0x18 for
// this call plus the preceding Vec3Sub together).

// Body: lazily fetches the 3DMathEngine singleton into its own static cache
// (.data:the field, initialised from the routine == Acquire3DMathEngine --
// baseline keeps one such cache slot per call site; the field is another),
// then invokes engine vtable slot +0x38 as
//   slot(Engine, &Scratch, M, Src)
// with callee cleanup (there is no ADD ESP after the CALL at 226272, and the
// subsequent [ESP+0x10] read at 226273 only resolves to `Out` if 16 bytes were
// popped by the callee -- i.e. the slot is __stdcall, which is what
// NEURONCOMPONENTS_API/x86math.h already declares it as), and finally copies the three
// floats out of Scratch into *Out. Slot +0x38 is I3DMathEngine::
// TransposeTransform (x86math.h: +0x0C Inverse, +0x10 Scale, +0x14 Det,
// +0x18 MulQuat, +0x1C MulTransform, +0x20 MulMatrix, +0x24 TransformQuat,
// +0x28 TransformXform, +0x2C TransformMatrix, +0x30 Rotate, +0x34
// InverseRotate, +0x38 TransposeTransform) -- i.e. this rotates a world-space
// delta into the box's local frame, which is exactly how both callers use it.

// The bounce through `Scratch` is baseline's, not an artifact: the engine writes
// into its own stack temp and only then is it copied to *Out, so aliasing
// between Out and Src is harmless. Reproduced faithfully.

// LINKAGE 2026-09-24 (claude-code:w6-zone): no longer file-static -- the zone shapes in
// common_zone.cpp call the routine directly (06339812, 06339be1, 06339cf2 and more), so it
// is declared in common_geometry_internal.h (formerly common_stubs.h). The null guard that stood here is gone: baseline has none
// (226259's TEST is only the lazy-init gate of the engine cache).
void MathTransposeTransformVia3DMathEngine(Vector* Out, Matrix const* M, Vector const* Src)
{
    I3DMathEngine* const Engine = Acquire3DMathEngine();

    Vector Scratch;
    Engine->TransposeTransform(*reinterpret_cast<FVector*>(&Scratch),
                               *reinterpret_cast<FMatrix const*>(M),
                               *reinterpret_cast<FVector const*>(Src));
    *Out = Scratch;
}

// Common (item 1673, the routine, 138 insns).
// `bool __thiscall(OrientedBox* Box, Vector const* Src, Vector* Dst)`, RET 8.
// `Box` here is the probe object's +0x24 sub-object, so the half-extents it
// reads at Box+0x30/+0x34/+0x38 are the probe's +0x54/+0x58/+0x5C.

// Per axis i (226103-226179): Overshoot[i] = fabs(Src[i]) - HalfExtent[i]; if
// Overshoot[i] >= 0 then Dst[i] = _copysign(HalfExtent[i], Src[i]) and the
// "was outside" flag is latched, otherwise Dst[i] = Src[i] verbatim. The
// FCOMP/TEST AH,0x1/JNZ trio at 226110-226113 tests C0 only, so the >= 0 case
// (equality included) is the clamping one.

// If no axis was outside, the point is strictly inside the box and baseline does
// NOT leave it alone: it snaps the single axis with the LARGEST Overshoot
// (i.e. the nearest face, all three values being negative) onto that face
// (226180-226234). The comparison chain is exactly
//   if (Ox >= Oy) { if (Ox >= Oz) snap X; else snap Z; }
//   else          { if (Oy >= Oz) snap Y; else snap Z; }
// (226183/226188 and 226208 each TEST AH,0x1, i.e. branch away only on a
// strict <). That interior case still returns FALSE -- every one of the three
// interior exits loads AL from the latched flag, which is 0 on this path
// (226197, 226217, 226232), never from a fresh 1. So the return value means
// "the input was outside the box", while Dst always comes back sitting exactly
// on the box surface. Both callers rely on that pair of facts.

// LINKAGE 2026-09-24 (claude-code:w6-zone): no longer file-static; the zone box shape
// (common_zone.cpp) calls the routine directly. Declared in common_geometry_internal.h
// (formerly common_stubs.h).
bool ClampPointToBoxSurface(unsigned char* Box, Vector const* Src, Vector* Dst)
{
    float const* const HalfExtent = reinterpret_cast<float const*>(Box + 0x30);
    float const* const In = reinterpret_cast<float const*>(Src);
    float* const OutAxis = reinterpret_cast<float*>(Dst);

    float Overshoot[3];
    bool bWasOutside = false;

    for (int Axis = 0; Axis < 3; ++Axis)
    {
        Overshoot[Axis] = std::fabs(In[Axis]) - HalfExtent[Axis];
        if (Overshoot[Axis] >= 0.0f)
        {
            OutAxis[Axis] = static_cast<float>(_copysign(HalfExtent[Axis], In[Axis]));
            bWasOutside = true;
        }
        else
        {
            OutAxis[Axis] = In[Axis];
        }
    }

    if (!bWasOutside)
    {
        int NearestAxis = 2;
        if (Overshoot[0] >= Overshoot[1])
        {
            if (Overshoot[0] >= Overshoot[2])
                NearestAxis = 0;
        }
        else if (Overshoot[1] >= Overshoot[2])
        {
            NearestAxis = 1;
        }
        OutAxis[NearestAxis] = static_cast<float>(_copysign(HalfExtent[NearestAxis], In[NearestAxis]));
    }

    return bWasOutside;
}

// Both user-zone probes below start the same way (Common and
// 229197-229210): the owner position is GetIfacePositionOrZero (the routine) of the
// watched object behind Owner+0xF0 (PathfindOwnerObject: raw ? raw - 8 : 0). Mode 2's
// "target interface" at Owner+0xEC is IBehaviorManager::ObjectDb: slot +0x04 is
// IObjDB::FindById(&Zone.TargetKey) (no ADD ESP after the CALL -- thiscall, callee
// pops), and slot 0 of the found object is IObjInspect::get_position.

// Common (item 1672, the routine, 161 insns).
// baseline signature `bool __thiscall(This, Vector const* ProbePoint, float*
// /*unused*/)` -- RET 8, and the single call site
// (Common) pushes &<a float scratch at frame+0x50> first
// and This+0xC's Vector second, so the low/first argument is the point and the
// high/second one is a float out-param the callee never touches. The dead
// second argument is dropped rather than carried as a fake parameter.
// PathfindManager::UserZoneContainsPoint (formerly AiVolumeProbeContainsPoint,
// a file-static over the raw receiver; baseline 0x062db830).

// Asks "is ProbePoint inside this probe's volume?".
//   * !Active -> true unconditionally (226062-226066: AL=1). A disabled probe
//     admits everything; that is baseline, not a stub.
//   * The owner position is fetched first, on every mode (225899-225912).
//   * Mode 0 (225 226018-226025): the volume is the sphere at TargetPoint.
//   * Mode 1 (225952-226006): oriented box. Local = TransposeTransform(
//     BoxRotation, ProbePoint - BoxCenter); Clamped = ClampPointToBoxSurface(
//     Local); SignedSq = |Local - Clamped|^2, negated when the point was NOT
//     outside (226000-226001, TEST BL,BL / JNZ skips the FCHS). Inside iff
//     SignedSq < 0 -- the TEST AH,0x5 / JNP at 226005-226006 branches to the
//     "inside" label on odd parity of C0|C2, i.e. on C0 alone, i.e. on
//     ST0 < 0.0f. When the point is interior the clamp still moves one axis,
//     so SignedSq is strictly negative and the test is not degenerate.
//   * Mode 2 (225922-225951): resolves the live target through the owner's
//     interface and then falls into the SAME sphere test as mode 0 using that
//     target (225951 jumps straight to the branch). A missing interface or a
//     missing target returns false outright (225926, 225937).
//   * Mode >= 3 (225921): no test at all, result stays false.
//   * Finally, when Invert is non-zero the result is flipped
//     (226010-226017 vs 226055-226061).
bool PathfindManager::UserZoneContainsPoint(Vector const* ProbePoint)
{
    if (!bUserZoneActive)                                   // byte [+0x08]
        return true;

    Vector OwnerPos;
    GetIfacePositionOrZero(&OwnerPos, PathfindOwnerObject(Owner));

    bool bInside = false;

    // Set by mode 0 and mode 2; consumed by the shared sphere test.
    Vector SphereTarget;
    bool bHaveSphereTarget = false;

    if (Zone.Mode == 0)                                     // [+0x0C]
    {
        SphereTarget = Zone.TargetPoint;                    // [+0x14]
        bHaveSphereTarget = true;
    }
    else if (Zone.Mode == 1)
    {
        // takes the box as the 0x3C-byte block at +0x24 (rotation, then
        // centre at +0x24, half extents at +0x30 of that block).
        unsigned char* const Box = reinterpret_cast<unsigned char*>(&Zone.BoxRotation);
        Vector const* const BoxCenter = &Zone.BoxCenter;    // [+0x48]

        Vector Delta;
        Delta.x = ProbePoint->x - BoxCenter->x;
        Delta.y = ProbePoint->y - BoxCenter->y;
        Delta.z = ProbePoint->z - BoxCenter->z;

        Vector Local;
        MathTransposeTransformVia3DMathEngine(&Local, &Zone.BoxRotation, &Delta);

        Vector Clamped;
        bool const bWasOutside = ClampPointToBoxSurface(Box, &Local, &Clamped);

        // the three differences are FSTP'd to float (Vec3Assign);
        // (Dy*Dy + Dz*Dz) + Dx*Dx stays unrounded on the x87 stack.
        float const Dx = Local.x - Clamped.x;
        float const Dy = Local.y - Clamped.y;
        float const Dz = Local.z - Clamped.z;
        double SignedSq = ((double)Dy * Dy + (double)Dz * Dz) + (double)Dx * Dx;
        if (!bWasOutside)
            SignedSq = -SignedSq;

        bInside = (SignedSq < 0.0f);
    }
    else if (Zone.Mode == 2)
    {
        // Owner->ObjectDb (+0xEC) FindById (slot +0x04) on
        // &Zone.TargetKey (+0x60), then the found object's get_position (slot 0).
        IObjDB* const ObjectDb = Owner->ObjectDb;
        if (ObjectDb == nullptr)
            return false;

        IObjRW* const Found = ObjectDb->FindById(&Zone.TargetKey);
        if (Found == nullptr)
            return false;

        SphereTarget = Found->get_position();
        bHaveSphereTarget = true;
    }

    if (bHaveSphereTarget)
    {
        // the differences stay on the x87 stack (unrounded).
        double const Dx = (double)OwnerPos.x - SphereTarget.x;
        double const Dy = (double)OwnerPos.y - SphereTarget.y;
        double const Dz = (double)OwnerPos.z - SphereTarget.z;
        double const Radius = Zone.Radius;                  // [+0x20]
        // (dz*dz + dy*dy) + dx*dx and Radius*Radius, both unrounded;
        // FCOMPP (ST0 = R*R) with TEST AH,0x41 / JNZ to "outside" on not-greater or
        // unordered, so equality counts as outside -- a strict R^2 > DistSq is required.
        bInside = (Radius * Radius > (Dz * Dz + Dy * Dy) + Dx * Dx);
    }

    if (Zone.Invert != 0)                                   // [+0x10]
        return !bInside;
    return bInside;
}

// Common (item 1686, the routine, 283 insns).
// `int __thiscall(This, Vector* OutDir)`, RET 4 (OutDir at entry+4, read back
// at 229444/229459 as [ESP+0x58] with the frame 0x54 deep).
// Returns 0 on success with a unit approach direction written to *OutDir, or 2
// ("not applicable") on every rejection. analysis exposes it as
// `thunk_FUN_062ddcd0`; the four other callers (Common, 221278,
// 222312, 228962) reach it through that thunk.

// Constants, read out of .rdata rather than assumed:
// = 00 00 00 00                   -> 0.0f
// = 00 00 80 3F                   -> 1.0f
// = eight zero bytes              -> 0.0 (double)

//   * !Active -> 2 (229193-229196).
//   * Owner position first, same as the containment probe (229197-229210).
//   * Mode 0 (229379-229434): Dist = |OwnerPos - TargetPoint|; the gate value
//     is Dist - Radius (229399, FSUB [EDI+0xC] with EDI = This+0x14); the
//     direction is normalize(TargetPoint - OwnerPos).
//   * Mode 1 (229242-229378): oriented box. baseline runs the SAME
//     Vec3Sub/TransposeTransform/ClampPointToBoxSurface/Vec3Sub chain TWICE --
//     once at 229245-229283 purely to test the sign, and again verbatim at
//     229294-229336 to take the square root -- with identical inputs both
//     times. Reproduced as one computation plus a comment rather than a
//     literal double evaluation, since the second run cannot differ.
//     The gate value is 0.0f when the (signed) squared distance is not > 0.0
//     (229291: the JP at 229290 is taken on even parity of C0|C3, i.e. when
//     neither is set, i.e. when ST0 > 0.0 -- so the sqrt branch runs on
//     strictly positive only), otherwise sqrt of it. NOTE the asymmetry with
//     mode 0: here the probe point fed to the box is the OWNER's own position
//     (229296 passes local_48, the position filled at 229210), not an external
//     point -- this function asks "how far am *I* from the volume", which is
//     why it takes no point argument at all. The direction is
//     normalize(BoxCenter - OwnerPos) (229338-229375, reading This+0x48/+0x4C/
//     +0x50 == BoxCenter).
//   * Mode 2 (229220-229241): refreshes This->TargetPoint from the owner's
//     interface -- note this WRITES the cache, unlike the containment probe --
//     then joins mode 0's path at the branch. Missing interface or missing
//     target -> 2.
//   * Mode >= 3 (229219): jumps straight to the gate with the 0.0f that was
//     loaded at 229211 still in ST0, and 0.0 fails both polarities below, so
//     an out-of-range mode always returns 2. OutDir is left untouched in that
//     case (baseline would have written the raw owner position; it never gets
//     there).
//   * The gate (229435-229478): compare the value against 0.0.
//       Invert == 0: reject on C0|C3 via TEST AH,0x41/JNZ, i.e. require
//                    Value > 0.0, and write *OutDir = Dir.
//       Invert != 0: TEST AH,0x5/JP rejects on EVEN parity of C0|C2, i.e. when
//                    C0 is clear, i.e. require Value < 0.0, and write
//                    *OutDir = -Dir (three FCHS at 229460/229465/229470).

// PathfindManager::GetUserZoneApproachDirection (formerly the free function
// AiVolumeProbeApproachDirection(This, OutDir), baseline 0x062ddcd0 + thunk
// ; CMN-SPLIT 2026-10-14). Callers: SelectAvoidancePoint,
// DodgeIdea's reroll (common_dodgeidea.cpp), BreakAwayIdea (common_breakawayidea.cpp) / the
// idea (DodgeMissileIdea, common_dodgemissileidea.cpp) and TargetManager::UpdateAttackTargetOverride.
int PathfindManager::GetUserZoneApproachDirection(Vector* OutDir)
{
    if (!bUserZoneActive)                                   // byte [+0x08]
        return 2;

    Vector OwnerPos;
    GetIfacePositionOrZero(&OwnerPos, PathfindOwnerObject(Owner));

    int const Mode = Zone.Mode;                             // [+0x0C]

    // FLD 0.0f -- the gate value lives on the x87 stack (double here) until
    // the final FCOMP at 062ddfb7.
    double GateValue = 0.0f;
    Vector Dir;
    Dir.x = 0.0f;
    Dir.y = 0.0f;
    Dir.z = 0.0f;

    if (Mode == 2)
    {
        // Owner->ObjectDb (+0xEC) FindById (slot +0x04) on
        // &Zone.TargetKey (+0x60); the found object's get_position (slot 0) is written
        // into the cached TargetPoint (+0x14).
        IObjDB* const ObjectDb = Owner->ObjectDb;
        if (ObjectDb == nullptr)
            return 2;

        IObjRW* const Found = ObjectDb->FindById(&Zone.TargetKey);
        if (Found == nullptr)
            return 2;

        Zone.TargetPoint = Found->get_position();
    }

    if (Mode == 0 || Mode == 2)
    {
        Vector const* const Target = &Zone.TargetPoint;

        // V = OwnerPos - Target through the routine (float
        // stores); gate = FSQRT((Vy*Vy + Vz*Vz) + Vx*Vx) - Radius, left unrounded.
        float const Bx = OwnerPos.x - Target->x;
        float const By = OwnerPos.y - Target->y;
        float const Bz = OwnerPos.z - Target->z;
        float const Radius = Zone.Radius;                   // [+0x20]
        GateValue = std::sqrt(((double)By * By + (double)Bz * Bz) + (double)Bx * Bx) - Radius;

        // Dir = Target - OwnerPos (float stores), then
        // 1 / FSQRT((Dz*Dz + Dy*Dy) + Dx*Dx) unrounded (FDIVR 1.0f, no zero guard),
        // each component FSTP'd.
        Dir.x = Target->x - OwnerPos.x;
        Dir.y = Target->y - OwnerPos.y;
        Dir.z = Target->z - OwnerPos.z;
        double const InvLen = 1.0 / std::sqrt(((double)Dir.z * Dir.z + (double)Dir.y * Dir.y) + (double)Dir.x * Dir.x);
        Dir.x = (float)(Dir.x * InvLen);
        Dir.y = (float)(Dir.y * InvLen);
        Dir.z = (float)(Dir.z * InvLen);
    }
    else if (Mode == 1)
    {
        unsigned char* const Box = reinterpret_cast<unsigned char*>(&Zone.BoxRotation);   // [+0x24]
        Vector const* const BoxCenter = &Zone.BoxCenter;    // [+0x48]

        Vector Delta;
        Delta.x = OwnerPos.x - BoxCenter->x;
        Delta.y = OwnerPos.y - BoxCenter->y;
        Delta.z = OwnerPos.z - BoxCenter->z;

        Vector Local;
        MathTransposeTransformVia3DMathEngine(&Local, &Zone.BoxRotation, &Delta);

        Vector Clamped;
        bool const bWasOutside = ClampPointToBoxSurface(Box, &Local, &Clamped);

        // D = Local - Clamped (the routine, float stores);
        // (Dy*Dy + Dz*Dz) + Dx*Dx unrounded, negated when the point was inside.
        float const Dx = Local.x - Clamped.x;
        float const Dy = Local.y - Clamped.y;
        float const Dz = Local.z - Clamped.z;
        double SignedSq = ((double)Dy * Dy + (double)Dz * Dz) + (double)Dx * Dx;
        if (!bWasOutside)
            SignedSq = -SignedSq;

        // `FCOMP 0.0; TEST AH,0x41; JP` takes the sqrt arm on greater
        // (or unordered -- a NaN gate is rejected below either way); otherwise the float
        // gate local is 0. baseline's duplicate second evaluation of everything above
        // (062dddf8-062dde68) is bit-identical to the first; only its FSQRT, FSTP'd to
        // float, is kept here.
        GateValue = (SignedSq > 0.0f) ? (float)std::sqrt(SignedSq) : 0.0f;

        // Dir = BoxCenter - OwnerPos (Vec3Assign, float stores), then
        // 1 / FSQRT((Dx*Dx + Dy*Dy) + Dz*Dz) unrounded (no zero guard), FSTP each.
        Dir.x = BoxCenter->x - OwnerPos.x;
        Dir.y = BoxCenter->y - OwnerPos.y;
        Dir.z = BoxCenter->z - OwnerPos.z;
        double const InvLen = 1.0 / std::sqrt(((double)Dir.x * Dir.x + (double)Dir.y * Dir.y) + (double)Dir.z * Dir.z);
        Dir.x = (float)(Dir.x * InvLen);
        Dir.y = (float)(Dir.y * InvLen);
        Dir.z = (float)(Dir.z * InvLen);
    }

    if (Zone.Invert == 0)                                   // [+0x10]
    {
        if (!(GateValue > 0.0f))
            return 2;
        *OutDir = Dir;
        return 0;
    }

    if (!(GateValue < 0.0f))
        return 2;
    OutDir->x = -Dir.x;
    OutDir->y = -Dir.y;
    OutDir->z = -Dir.z;
    return 0;
}

