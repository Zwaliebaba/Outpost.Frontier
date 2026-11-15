// Author: Giperion.

#include "common_lootbehavior.h"

#include "common_aisteer.h"        // FAiTarget
#include "common_archetype.h"      // Archetype::Ship::HoldSize, Archetype::Root::get_class_type
#include "common_cequip.h"         // ScanList
#include "common_cobject.h"        // CObject, CShip, CLoot, CEqObj::get_behavior_interface
#include "common_iface.h"          // IObjInspect, IObjAffect, IObjRW, IBehaviorManager, EquipmentVal
#include "common_misc.h"           // SUBOBJ_ID_NONE
#include "common_systemmanager.h"  // SystemManager -- what NavHelper points at

#include <cstring>

// LootBehavior -- see common_lootbehavior.h. Every method re-ported from
// Common-10-08 (claude-code:cmn-ai5).

// (item 1798) is SystemManagerTimerTable::FindVerifiedObject.

namespace
{
    IObjRW* WatchedObject(BaseWatcher const& Watcher)
    {
        return ObjRwFromWatchable(const_cast<Watchable*>(Watcher.target()));
    }

    // `MOV EAX,[Sys+8]; ... LEA ECX,[EAX-8]` -- the owner ship, null arm included.
    IObjRW* OwnShip(SystemManager* Sys)
    {
        return WatchedObject(Sys->Watcher);
    }

    // The inlined `cobject(); (+0x4c & Mask) == Mask ? obj : 0` filter.
    CObject const* CObjectOfClass(IObjInspect const* Obj, unsigned Mask)
    {
        CObject const* const C = Obj->cobject();
        if (C != nullptr && (static_cast<unsigned>(C->ObjectClass) & Mask) == Mask)
            return C;
        return nullptr;
    }

    // The cargo-hold check inlined at module storage-062ce8fe and 062ceb9e-062cebf8: the own
    // ship's CShip (null when it is not one -- baseline then faults on [ECX+0x88]); with a
    // positive hold the free fraction, else 0.0; true when that exceeds 0.2 (double
    // , `TEST AH,0x41; JZ`).
    bool OwnHoldHasRoom(SystemManager* Sys)
    {
        CShip const* const Ship = static_cast<CShip const*>(CObjectOfClass(OwnShip(Sys), CObject::ClassShip));
        Archetype::Ship const* const Arch = static_cast<Archetype::Ship const*>(Ship->Archetype);
        double Fraction = 0.0;                                      // FLD the field
        if (static_cast<double>(Arch->HoldSize) > 0.0)              // FCOMP double 0.0; `TEST AH,0x41; JNZ`
            Fraction = static_cast<double>(Ship->get_cargo_hold_remaining()) / Arch->HoldSize;
        return Fraction > 0.2;
    }

    // The 0x5f3759df distance at module storage-062cead9 / 062cec90-062cecd0: (dz*dz + dx*dx)
    // + dy*dy on the x87 stack, FST for the bit pattern, DistSq * s * (1.5 - 0.5 * s*s*DistSq).
    double FastDistance(Vector const& A, Vector const& B)
    {
        double const Dx = static_cast<double>(A.x) - B.x;          // FLD; FSUB -- kept on the stack
        double const Dy = static_cast<double>(A.y) - B.y;
        double const Dz = static_cast<double>(A.z) - B.z;
        double const DistSq = (Dz * Dz + Dx * Dx) + Dy * Dy;
        float const DistSqF = static_cast<float>(DistSq);
        int Bits;
        std::memcpy(&Bits, &DistSqF, sizeof(Bits));
        int const SeedBits = 0x5f3759df - (Bits >> 1);
        float Seed;
        std::memcpy(&Seed, &SeedBits, sizeof(Seed));
        return ((1.5f - ((static_cast<double>(Seed) * Seed) * DistSq) * 0.5f) * Seed) * DistSq;
    }

    // (module storage-062ece39, RET 4): `this` is the SystemManager+0x1498
    // tractor record {Owner1498, Watcher149C, Unknown14A4 = tractor equipment id}. Point the
    // tractor at Target through the owner ship's IObjAffect::slot_40(&Id, Target, -1).
    bool TractorActivate(SystemManager* Sys, IObjRW* Target)
    {
        if (Target == nullptr)
            return false;
        unsigned short const* const Id = reinterpret_cast<unsigned short const*>(&Sys->Unknown14A4);
        if (*Id == SUBOBJ_ID_NONE)
            return false;
        IObjRW* const Current = WatchedObject(Sys->Watcher149C);
        if (Current != nullptr && Current->get_id() == Target->get_id())
            return false;
        Sys->Watcher149C.set(reinterpret_cast<Watchable const*>(reinterpret_cast<char*>(Target) + 8));
        IObjRW* const Owner = WatchedObject(Sys->Owner1498->Watcher);
        int const Result = static_cast<IObjAffect*>(Owner)->slot_40(Id, WatchedObject(Sys->Watcher149C), -1);
        return Result == 0;
    }

    // (module storage-062ecea3, RET 0): release the tractor through
    // IObjAffect::slot_4c(&Id, false, -1); true when that returned 0 (NEG/SBB/INC).
    bool TractorRelease(SystemManager* Sys)
    {
        if (WatchedObject(Sys->Watcher149C) == nullptr)
            return false;
        unsigned short const* const Id = reinterpret_cast<unsigned short const*>(&Sys->Unknown14A4);
        if (*Id == SUBOBJ_ID_NONE)
            return false;
        Sys->Watcher149C.set(nullptr);
        IObjRW* const Owner = WatchedObject(Sys->Owner1498->Watcher);
        return static_cast<IObjAffect*>(Owner)->slot_4c(Id, false, -1) == 0;
    }
}

// ----------------------------------------------------------------------------
// (module storage-062ce48f).
// ----------------------------------------------------------------------------
LootBehavior::LootBehavior(int InTypeId)
    : Behavior()
    , TypeId(InTypeId)
    , CachedHandle08(0)
    , ModeSelector0xc(0)
    , UpdateResult10(0)
    , OwnerContext(nullptr)
    , Opaque18{}
    , DeltaTime(0.0f)
    , BehaviorMgrPtr(nullptr)
    , NavHelper(nullptr)
    , Flags28(0)
    , Flag29(0)
    , Pad2a{}
    , Move()                        // the routine
    , Face()                        // the routine
    , bHasTractor164(0)             // not written by baseline's ctor; SetContext writes it
    , Pad165{}
    , TractorReach168(0.0f)         // not written by baseline's ctor; SetContext writes it
    , Flag16c(1)
    , Pad16d{}
    , Field170(13.0f)               // 0x41500000
    , Target{}                      // watcher zeroed, 062ce47b bDirected = 0
{
    ConstructBase(InTypeId);        // the routine
}

// (the routine is the scalar-deleting thunk).
LootBehavior::~LootBehavior()
{
}

// ----------------------------------------------------------------------------
// Slot 1, the branch (module storage-062ce653).
// ----------------------------------------------------------------------------
bool LootBehavior::SetContext(void* Owner, void* Manager, void* Sys)
{
    bool const bOk = BindContext(Owner, Manager, Sys);              // the routine
    if (bOk)
    {
        Move.SetContext(OwnerContext, NavHelper, Manager);
        Face.SetContext(OwnerContext, NavHelper, Manager);
    }

    // runs whether or not the binder accepted the context.
    Target.Watcher.set(nullptr);
    Target.bDirected = 0;
    bHasTractor164 = 0;
    TractorReach168 = 0.0f;

    // baseline reuses its own argument slots: the second one becomes the
    // equipment id (seeded SUBOBJ_ID_NONE) and the first the EquipmentVal value in front of
    // it, i.e. one EquipmentVal{Value, EquipId}.
    EquipmentVal Tractor;
    Tractor.Value = 0.0f;
    Tractor.EquipId = SUBOBJ_ID_NONE;
    Tractor.Pad = 0;
    IObjRW* const Ship = OwnShip(NavHelper);
    if (Ship->find_equipment(&Tractor.EquipId, 1, 0x100000) == 0)  // [+0x114], tractor class
    {
        bHasTractor164 = 1;
        if (OwnShip(NavHelper)->get_equipment_val(Tractor, Tractor.EquipId,
                                                  static_cast<EquipmentValType>(5), 0.0f) == 0)   // [+0x11c]
        {
            // FMUL double 0.75 (module storage), FSTP float.
            TractorReach168 = static_cast<float>(static_cast<double>(Tractor.Value) * 0.75);
        }
    }
    return bOk;
}

// ----------------------------------------------------------------------------
// Slot 3, the branch: `JMP the routine`.
// ----------------------------------------------------------------------------
void LootBehavior::Slot3()
{
    Behavior::Slot3();
}

// ----------------------------------------------------------------------------
// Slot 4, the routine (module storage-062ce6aa).
// ----------------------------------------------------------------------------
void LootBehavior::Slot4()
{
    Target.Watcher.set(nullptr);
    Target.bDirected = 0;
    Move.Commit();
    Face.Commit();
    (void)TractorRelease(NavHelper);                                // the routine(Sys+0x1498)
    Behavior::Slot4();                                              // JMP the routine
}

// ----------------------------------------------------------------------------
// Slot 5, the routine (module storage-062ce73a).
// ----------------------------------------------------------------------------
int LootBehavior::Slot5(float Unused)
{
    (void)Unused;
    // only with a tractor and the TargetManager in attack state 2.
    if (bHasTractor164 != 0 && NavHelper->Targeting.AttackState == 2)
    {
        if (WatchedObject(Target.Watcher) == nullptr)
            (void)AcquireLootTarget(&Target);                       // the routine
        if (WatchedObject(Target.Watcher) != nullptr)
        {
            ModeSelector0xc = Flag29 ? 3 : 0;
            return ModeSelector0xc;
        }
    }
    ModeSelector0xc = Flag29 ? 3 : 2;
    return ModeSelector0xc;
}

// ----------------------------------------------------------------------------
// (module storage-062ce85e, RET 4).
// ----------------------------------------------------------------------------
bool LootBehavior::IsLootUnclaimed(IObjRW* Loot)
{
    // the loot's CLoot (null when it is not one -- baseline then reads
    // [0+0xf0]); more than one unit is never contested.
    CLoot const* const LootObj = static_cast<CLoot const*>(CObjectOfClass(Loot, CObject::ClassLoot));
    if (LootObj->Units > 1)                                         // `CMP [EAX+0xf0],1; JA`
        return true;

    // every ship-class object in the owner's scan list (the routine).
    ScanList const* List = nullptr;
    (void)NavHelper->Timers.GetOwnScanList(List, 0x7f0000);
    if (List == nullptr)
        return false;

    for (int Index = 0; static_cast<unsigned>(Index) < static_cast<unsigned>(List->Count); ++Index)   // `JBE` / `JC`
    {
        IObjRW* const Other = WatchedObject(List->Entries[Index]);
        if (Other == OwnShip(NavHelper))
            continue;
        unsigned OtherType = 0;                                     // not pre-set in baseline
        Other->get_type(OtherType);                                 // [+0x88], result unread
        if ((OtherType & 0x7f0000u) == 0)
            continue;
        CObject const* const OtherShip = CObjectOfClass(Other, CObject::ClassShip);
        if (OtherShip == nullptr)
            continue;
        IBehaviorManager* const Manager =
            const_cast<CEqObj*>(static_cast<CEqObj const*>(OtherShip))->get_behavior_interface();
        if (Manager == nullptr)
            continue;
        if (Manager->CurrentBehaviorIndex != 0x10)                  // currently looting
            continue;
        // its LootBehavior (dispatch slot 16, +0x1b4) chases this loot.
        LootBehavior const* const OtherLoot = static_cast<LootBehavior const*>(Manager->BehaviorDispatchTable[16]);
        if (WatchedObject(OtherLoot->Target.Watcher) == Loot)
            return false;
    }
    return true;
}

// ----------------------------------------------------------------------------
// (module storage-062ceb40, RET 4).
// ----------------------------------------------------------------------------
bool LootBehavior::AcquireLootTarget(FLootTarget* Slot)
{
    SystemManager* const Sys = NavHelper;
    pub::AI::Personality::JobStruct const& Job = Sys->PersonalityData.JobParams;
    if (Job.LootPreference == 0)                                    // [Sys+0x18fc]: loot flags
        return false;
    if (!OwnHoldHasRoom(Sys))
        return false;

    // the own position (ExternalIface transform slot 4, +0x24 = Position).
    Vector const& OwnPos = Sys->SelfTarget.Iface->get_transform().Translation;   // [+0x10] +0x24

    IObjRW* Best = nullptr;                                         // [ESP+0x20]
    float BestDistance = 3.40282347e+38f;
    ScanList const* List = nullptr;
    (void)Sys->Timers.GetOwnScanList(List, 0x10000000);   // , loot class
    if (List == nullptr)
        return false;

    for (int Index = 0; static_cast<unsigned>(Index) < static_cast<unsigned>(List->Count); ++Index)
    {
        IObjRW* const Candidate = WatchedObject(List->Entries[Index]);
        unsigned CandidateType = 0;                                 // not pre-set in baseline
        Candidate->get_type(CandidateType);                         // [+0x88]
        if ((CandidateType & 0x10000000u) == 0)
            continue;

        CLoot const* const Loot = static_cast<CLoot const*>(CObjectOfClass(Candidate, CObject::ClassLoot));
        if (Loot->bCanAiTractor == 0)                               // [+0xfc]
            break;

        int const Kind = static_cast<int>(Loot->ContentsArch->get_class_type());   // [+0x10]
        if (Kind == 0xa)
        {
            // commodities need loot flag bit 0 and, when the personality
            // lists goods (+0x1904 count, +0x1908 ids), the candidate's good among them.
            if ((Job.LootPreference & 1) == 0)
                break;
            int const ListCount = Job.ScanForCargoCount;
            if (ListCount > 0)
            {
                unsigned GoodId = 0;
                if (Candidate->get_good_id(GoodId) == 0)            // [+0x24]
                {
                    int GoodIndex = 0;
                    while (Job.ScanForCargo[GoodIndex] != GoodId)
                    {
                        ++GoodIndex;
                        if (GoodIndex >= ListCount)
                            goto Done;                              // JMP the branch
                    }
                }
            }
        }
        else if (Kind == 0x1a || Kind == 0x1b)
        {
            if ((Job.LootPreference & 4) == 0)
                break;
        }

        // .
        if (Loot->Units == 0)                                       // [+0xf0]
            break;
        if (!IsLootUnclaimed(Candidate))                            // the routine
            continue;

        // the nearest wins (`FCOM best; TEST AH,0x5; JP` keeps >=).
        Vector const& LootPos = Candidate->get_transform().Translation;   // [+0x10] +0x24
        double const Distance = FastDistance(LootPos, OwnPos);
        if (Distance < BestDistance)
        {
            BestDistance = static_cast<float>(Distance);
            Best = Candidate;
        }
    }

Done:
    // the slot watches the winner (or nothing), undirected.
    Slot->Watcher.set(Best != nullptr ? reinterpret_cast<Watchable const*>(reinterpret_cast<char*>(Best) + 8)
                                      : nullptr);
    Slot->bDirected = 0;
    return Best != nullptr;
}

// ----------------------------------------------------------------------------
// Slot 7, the branch (module storage-062cedc7).
// ----------------------------------------------------------------------------
int LootBehavior::Slot7()
{
    if (OwnerContext == nullptr)
    {
        UpdateResult10 = 3;
        return 3;
    }
    IObjRW* const Loot = WatchedObject(Target.Watcher);             // EBX
    if (Loot == nullptr)
    {
        UpdateResult10 = 2;
        return 2;
    }

    SystemManager* const Sys = NavHelper;
    if (!OwnHoldHasRoom(Sys))
    {
        Target.Watcher.set(nullptr);
        Target.bDirected = 0;
        UpdateResult10 = 2;
        return 2;
    }

    // the FAiTarget type refresh (get_type, then +0x7d = 1).
    FAiTarget* const Nav = &Sys->SelfTarget;
    if (Nav->bFlagsValid == 0)
    {
        Nav->Iface->get_type(Nav->CachedFlags);
        Nav->bFlagsValid = 1;
    }
    // `AND ECX,0x10000; OR ECX,0x20000; JNZ the branch` -- always taken.
    // The fall-through at 062cec59-062ced2a (a FastDistance gate on TractorReach168, then
    // Move.Commit and the routine, or the release exit) is unreachable in baseline and is
    // written below behind the same constant test.
    if ((((Nav->CachedFlags & 0x10000u) | 0x20000u)) == 0)
    {
        IObjRW* const Ship = OwnShip(Sys);
        double const Distance = FastDistance(Ship->get_position(), Loot->get_position());   // [EBP] / [EBX] slot 0
        if (Distance < TractorReach168)                             // `TEST AH,0x5; JP`
        {
            Move.Commit();
            if (!TractorActivate(Sys, Loot))
            {
                UpdateResult10 = 1;
                return 1;
            }
        }
        Target.Watcher.set(nullptr);
        Target.bDirected = 0;
        UpdateResult10 = 2;
        return 2;
    }

    // fly within tractor reach, then face it and tractor it in.
    Move.SetWatchObject(Loot);                                      // the routine
    Move.SetEnabled(true);                                          // the routine
    Move.SetRange3c(TractorReach168);                               // module storage
    if (Move.Update(DeltaTime) == 1)
    {
        Move.Commit();
        Face.SetWatchObject(Loot);                                  // the routine
        if (Face.Update(DeltaTime) == 1 && TractorActivate(Sys, Loot))   // / 062ced8d
            return 2;                                               // +0x10 not written
    }
    else
    {
        Face.SetWatchObject(Loot);
        Face.Update(DeltaTime);
    }
    UpdateResult10 = 1;
    return 1;
}

// ----------------------------------------------------------------------------
// Slot 10, the routine (module storage-062cee3f).
// ----------------------------------------------------------------------------
bool LootBehavior::Slot10(void* DirectiveOp)
{
    unsigned char* const Op = static_cast<unsigned char*>(DirectiveOp);
    void* const Found = NavHelper->Timers.FindVerifiedObject((unsigned const*)(Op + 0xc));
    Target.Watcher.set(Found != nullptr ? reinterpret_cast<Watchable const*>(static_cast<char*>(Found) + 8)
                                        : nullptr);
    if (WatchedObject(Target.Watcher) != nullptr)
    {
        Target.bDirected = 1;
        Flags28 = static_cast<unsigned char>(Flags28 | 0x2);
        return true;
    }
    Flags28 = static_cast<unsigned char>(Flags28 & 0xfd);
    Flag16c = Op[0x10];
    return false;
}
