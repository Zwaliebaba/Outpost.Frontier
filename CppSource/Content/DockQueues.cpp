// Author: Giperion.

// Content.dll TU53: the dock queues and the formation helpers (see DockQueues.h).

// TU54's own 16 static-ID initializers (the field..the field), before
// the TU's other dynamic initializers (the dock lists, the branch, the branch). The
// dock-type tests read the last four (GIdCanUseBerths .. GIdCanUseLargeMoors).
#include "ContentStaticIds.h"

#include "DockQueues.h"

#include "BaseDB.h"              // FindContentBaseInfo, FContentBase::FindFaction
#include "ContentInternal.h"
#include "ContentPlayerState.h"
#include "ContentSort.h"         // ContentSort (the VC6 std::sort of CollectFormationMembers)
#include "Encounters.h"          // FFormation::GetSlotPosition
#include "Guides.h"              // FContentGuide::Clone / Release (BreakFormationAtDock)
#include "MissionObjList.h"      // CreateObjListDirective, FObjListDelay, FObjListLaunch
#include "MissionObject.h"       // FMissionDirectiveList (the ref-owning list of BreakFormationAtDock)
#include "NpcShip.h"             // FContentNpcShipParams (GDockLaunchQueues), FContentNpcShip
#include "SystemInfo.h"
#include "common_id.h"           // CreateID
#include "common_newchar.h"      // pub::StateGraph::get_state_graph
#include "common_server.h"
#include "common_server_controller.h"   // pub::Controller::_SendMessage
#include "server_cobject.h"      // EQInfo, TerminalInfo, ActivateEquipment, GetTerminalInfo

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

std::list<FDockQueueEntry> GDockWaitingShips;
std::list<FDockQueueEntry> GDockingShips;
std::vector<FDockTerminal> GDockTerminals;
std::vector<FDockTerminal> GDockTerminalsApplied;
std::map<unsigned int, std::list<FContentNpcShipParams>> GDockLaunchQueues;

// the field = the field (-0.6f).
float GDockHostileAttitude = -0.6f;


namespace
{
    // this TU's math engine, created on first use (the routine).
    I3DMathEngine* GDockMathEngine = nullptr;

    bool ContainsClass(std::vector<unsigned int> const* ClassIds, unsigned int ClassId)
    {
        // std::find over the vector.
        return std::find(ClassIds->begin(), ClassIds->end(), ClassId) != ClassIds->end();
    }

    // The terminal-type switch of the routine (06f6c5fb-06f6c635 and 06f6c791-06f6c7d1):
    // types 1, 2, 6 berths, 3 small, 4 medium, 5 large moors need that class; 7 and 8 take
    // any ship; any other type takes none.
    bool DoesTerminalFitClasses(unsigned int TerminalType, std::vector<unsigned int> const* ClassIds)
    {
        switch (TerminalType)
        {
        case 1:
        case 2:
        case 6:
            return ContainsClass(ClassIds, GIdCanUseBerths);
        case 3:
            return ContainsClass(ClassIds, GIdCanUseSmallMoors);
        case 4:
            return ContainsClass(ClassIds, GIdCanUseMediumMoors);
        case 5:
            return ContainsClass(ClassIds, GIdCanUseLargeMoors);
        case 7:
        case 8:
            return true;
        default:
            return false;
        }
    }

    // The docking-list scan of the routine (06f6c637-06f6c660, 06f6c7d7-06f6c7fd).
    bool IsDockTerminalInUse(unsigned int DockTargetId, int DockIndex)
    {
        for (std::list<FDockQueueEntry>::iterator It = GDockingShips.begin();
             It != GDockingShips.end(); ++It)
        {
            if (It->DockIndex == DockIndex && It->DockTargetId == DockTargetId)
                return true;
        }
        return false;
    }

    // 's local vector (0x14 elements, the routine / the routine inserts).
    struct FDockKick
    {
        FContentShipBase* Ship;      // +0x00 null: cancel the docking by id
        unsigned int ShipId;         // +0x04 only written (and read) for a null Ship
        unsigned int DockTargetId;   // +0x08
        int DockIndex;               // +0x0c
        bool bRedock;                // +0x10
    };

    // / the routine: each component of the difference is stored as a float;
    // the squares are summed on the x87 stack (kept here in double).
    double DistanceSquared(Vector const& A, Vector const& B)
    {
        float X = A.x - B.x;
        float Y = A.y - B.y;
        float Z = A.z - B.z;
        return static_cast<double>(Z) * Z + static_cast<double>(Y) * Y + static_cast<double>(X) * X;
    }

    // The inverse-square-root idiom of the routine (06f6c09f-06f6c161, 06f6c1a2-06f6c1fa):
    // y = 0x5f3759df - (bits(StoredSquare) >> 1), one Newton step, times Square:
    // ((1.5 - y*y*Square*0.5) * y) * Square.
    double ApproximateSqrt(float StoredSquare, double Square)
    {
        int Bits;
        std::memcpy(&Bits, &StoredSquare, sizeof(Bits));
        Bits = 0x5f3759df - (Bits >> 1);
        float Y;
        std::memcpy(&Y, &Bits, sizeof(Y));
        double Step = 1.5 - static_cast<double>(Y) * Y * Square * 0.5;
        return Step * Y * Square;
    }
}

// (06f6e530-06f6e80a, cdecl; caller SpaceSupport msg 0x27 06f5dd8b), and the same
// terminal-vector update inlined in the routine (06f6c3c8-06f6c3ff, 06f6c43c-06f6c47e,
// ) and the routine (06f6e9f1-06f6ea34, 06f6eb53-06f6eb5e): the first
// GDockTerminals entry (the field) with this target and index only changes when its flag
// is clear (`if (!b) b = bOccupied`, so a set flag is never cleared here); an unlisted pair
// is appended {target, index, bOccupied} (06f6e560-06f6e7fd: the VC6 vector growth inlined,
// / the routine / the routine / the routine).
void __cdecl MarkDockTerminal(unsigned int DockTargetId, int DockIndex, bool bOccupied)
{
    for (std::vector<FDockTerminal>::iterator It = GDockTerminals.begin();
         It != GDockTerminals.end(); ++It)
    {
        if (It->DockTargetId == DockTargetId && It->DockIndex == DockIndex)
        {
            if (!It->bOccupied)
                It->bOccupied = bOccupied;
            return;
        }
    }
    FDockTerminal Terminal;
    Terminal.DockTargetId = DockTargetId;
    Terminal.DockIndex = DockIndex;
    Terminal.bOccupied = bOccupied;
    GDockTerminals.push_back(Terminal);
}

// (06f6bbf0-06f6bc17).
FDockQueueEntry::FDockQueueEntry()
    : SystemId(0)
    , ShipId(0)
    , DockTargetId(0)
    , DockIndex(-1)
    , bField_0x10(false)
    , Time(GetContentGameTime())
    , bField_0x20(false)
{
}

// (06f601f0-06f60219).
std::list<FDockQueueEntry>::iterator __cdecl FindDockQueueEntry(std::list<FDockQueueEntry>* List,
                                                                unsigned int ShipId)
{
    for (std::list<FDockQueueEntry>::iterator It = List->begin(); It != List->end(); ++It)
    {
        if (It->ShipId == ShipId)
            return It;
    }
    return List->end();
}

// (06f66220-06f66438). For each SystemInfo of the map (the routine) its space
// objects (+0xdc) whose +0x154 is FormationId, then every player state's ship (+0x10, when
// set) with the same test -- all appended (the routine). Then the whole vector is sorted by
// +0x14c (signed `<`, JL): baseline's inlined VC6 std::sort = ContentSort -- up to 16 elements
// the insertion sort 06f66322 (unguarded insert the routine); above, the first _Sort pass
// inlined (median the routine of first/mid/last-1, partition the routine, _Sort
// into the not-larger side, loop while > 16), then insertion the routine of the
// first 16 and the unguarded inserts 06f663f4. Ties keep baseline's order.
void __cdecl CollectFormationMembers(std::vector<FContentShipBase*>* Out, unsigned int FormationId)
{
    if (FormationId == 0xffffffffu)
        return;

    std::map<unsigned int, FSystemInfo>* Systems = GetSystemInfos();
    for (std::map<unsigned int, FSystemInfo>::iterator SystemIt = Systems->begin();
         SystemIt != Systems->end(); ++SystemIt)
    {
        std::list<FContentSpaceObj*>& Objects = SystemIt->second.SpaceObjs;
        for (std::list<FContentSpaceObj*>::iterator ObjIt = Objects.begin(); ObjIt != Objects.end();
             ++ObjIt)
        {
            FContentShipBase* Ship = static_cast<FContentShipBase*>(*ObjIt);
            if (Ship->FormationId == FormationId)
                Out->push_back(Ship);
        }
    }

    std::map<unsigned int, std::unique_ptr<FContentPlayerState>>& Players = GetContentPlayerStates();
    for (std::map<unsigned int, std::unique_ptr<FContentPlayerState>>::iterator PlayerIt =
             Players.begin();
         PlayerIt != Players.end(); ++PlayerIt)
    {
        FContentShipBase* Ship = PlayerIt->second->ShipObject;
        if (Ship != nullptr && Ship->FormationId == FormationId)
            Out->push_back(Ship);
    }

    ContentSort(Out->data(), Out->data() + Out->size(),
                [](FContentShipBase* A, FContentShipBase* B)
                {
                    return static_cast<int>(A->FormationSlot) < static_cast<int>(B->FormationSlot);
                });
}

// (06f67660-06f67733). The player walk sits inside the system loop: for each
// system its space objects, then all players. A match is +0x154 == FormationId with slot
// +0x14c == 0 or (+0x154 == -1).
FContentShipBase* __cdecl FindFormationLeader(unsigned int FormationId)
{
    std::map<unsigned int, FSystemInfo>* Systems = GetSystemInfos();
    for (std::map<unsigned int, FSystemInfo>::iterator SystemIt = Systems->begin();
         SystemIt != Systems->end(); ++SystemIt)
    {
        std::list<FContentSpaceObj*>& Objects = SystemIt->second.SpaceObjs;
        for (std::list<FContentSpaceObj*>::iterator ObjIt = Objects.begin(); ObjIt != Objects.end();
             ++ObjIt)
        {
            FContentShipBase* Ship = static_cast<FContentShipBase*>(*ObjIt);
            if (Ship->FormationId == FormationId &&
                (Ship->FormationSlot == 0 || Ship->FormationId == 0xffffffffu))
                return Ship;
        }

        std::map<unsigned int, std::unique_ptr<FContentPlayerState>>& Players = GetContentPlayerStates();
        for (std::map<unsigned int, std::unique_ptr<FContentPlayerState>>::iterator PlayerIt =
                 Players.begin();
             PlayerIt != Players.end(); ++PlayerIt)
        {
            FContentShipBase* Ship = PlayerIt->second->ShipObject;
            if (Ship != nullptr && Ship->FormationId == FormationId &&
                (Ship->FormationSlot == 0 || Ship->FormationId == 0xffffffffu))
                return Ship;
        }
    }
    return nullptr;
}

// (06f6ee40-06f6eeb2). The docking entry of the leader (first match); its
// terminal's type (GetTerminalInfo's result is not tested) 7 or 8 -> CancelShipDocking(ship,
// that entry's dock target, 0).
void __cdecl CancelDockingBehindLeader(FContentShipBase* Ship, unsigned int FormationId)
{
    FContentShipBase* Leader = FindFormationLeader(FormationId);
    if (Leader == nullptr)
        return;

    std::list<FDockQueueEntry>::iterator It = GDockingShips.begin();
    for (; It != GDockingShips.end(); ++It)
    {
        if (It->ShipId == Leader->SpaceObjId)
            break;
    }
    if (It == GDockingShips.end())
        return;

    unsigned int DockTargetId = It->DockTargetId;
    pub::SpaceObj::TerminalInfo Info;
    pub::SpaceObj::GetTerminalInfo(It->DockTargetId, It->DockIndex, Info);
    if (Info.Value == 7 || Info.Value == 8)
        CancelShipDocking(Ship->SpaceObjId, DockTargetId, false);
}

// (06f671a0-06f672c6). Offset: zero, or with a formation (+0x150) its slot
// position for +0x14c (the routine). SetPersonalityParams (Common ctor) carries only the
// state graph (+0xc). A follower (formation != -1, slot != 0, +0x154 != -1) whose leader is
// found gets the ESCORT graph and follows the leader; without a leader nothing is sent.
// Anyone else gets the LEADER graph and update_formation_state(ship, formation == -1 ? 0 :
// ship, offset).
void __cdecl ResetFormationState(FContentShipBase* Ship)
{
    unsigned int FormationId = Ship->FormationId;
    Vector Offset;
    Offset.x = 0.0f;
    Offset.y = 0.0f;
    Offset.z = 0.0f;
    if (Ship->Formation != nullptr)
    {
        FVector SlotPosition = Ship->Formation->GetSlotPosition(Ship->FormationSlot);
        Offset.x = SlotPosition.X;
        Offset.y = SlotPosition.Y;
        Offset.z = SlotPosition.Z;
    }

    pub::AI::SetPersonalityParams Params;
    if (FormationId != 0xffffffffu && Ship->FormationSlot != 0 && Ship->FormationId != 0xffffffffu)
    {
        FContentShipBase* Leader = FindFormationLeader(FormationId);
        if (Leader == nullptr)
            return;
        Params.StateGraphId = pub::StateGraph::get_state_graph(
            pub::AI::get_state_graph_id(Ship->SpaceObjId), pub::StateGraph::ESCORT);
        pub::AI::SubmitState(Ship->SpaceObjId, &Params);
        pub::AI::update_formation_state(Ship->SpaceObjId, Leader->SpaceObjId, Offset);
        return;
    }

    Params.StateGraphId = pub::StateGraph::get_state_graph(
        pub::AI::get_state_graph_id(Ship->SpaceObjId), pub::StateGraph::LEADER);
    pub::AI::SubmitState(Ship->SpaceObjId, &Params);
    unsigned int LeaderId = FormationId == 0xffffffffu ? 0 : Ship->SpaceObjId;
    pub::AI::update_formation_state(Ship->SpaceObjId, LeaderId, Offset);
}

// (06f67090-06f67198). The leader of the ship's formation is the ship: every
// member is looked up again in its own system (the routine + FindShip); a live one still in
// a formation gets CancelDockingBehindLeader(member, its formation) and SetFormation(-1, 0,
// null); every member then ResetFormationState. Anyone else in a formation:
// SetFormation(-1, -1, null) and ResetFormationState.
void __cdecl RemoveFromFormation(FContentShipBase* Ship)
{
    unsigned int FormationId = Ship->FormationId;
    if (FindFormationLeader(FormationId) == Ship)
    {
        std::vector<FContentShipBase*> Members;
        CollectFormationMembers(&Members, FormationId);
        for (int Index = 0; Index < static_cast<int>(Members.size()); ++Index)
        {
            FContentShipBase* Member = Members[Index];
            unsigned int MemberId = Member->SpaceObjId;
            FContentShipBase* Live =
                static_cast<FContentShipBase*>(Member->GetSystem()->FindShip(MemberId));
            if (Live != nullptr && Live->FormationId != 0xffffffffu)
            {
                CancelDockingBehindLeader(Live, Live->FormationId);
                Live->SetFormation(0xffffffffu, 0, nullptr);
            }
            ResetFormationState(Member);
        }
        return;
    }

    if (FormationId == 0xffffffffu)
        return;
    Ship->SetFormation(0xffffffffu, 0xffffffffu, nullptr);
    ResetFormationState(Ship);
}

// (06f66dc0-06f66eba). Members of Ship's formation (+0x154) other than Ship (by
// space object id): a player's ship (the routine, state +0x10 non-null) is pushed to the
// back of Ships; each of them then leaves the formation (the routine).
void __cdecl ReleaseFormationMembers(FContentShipBase* Ship, std::list<FContentShipBase*>* Ships)
{
    std::vector<FContentShipBase*> Members;
    CollectFormationMembers(&Members, Ship->FormationId);
    for (int Index = 0; Index < static_cast<int>(Members.size()); ++Index)
    {
        FContentShipBase* Member = Members[Index];
        unsigned int MemberId = Member->SpaceObjId;
        if (MemberId == Ship->SpaceObjId)
            continue;
        FContentPlayerState* Player = FindContentPlayerStateByShip(&MemberId);
        if (Player != nullptr && Player->ShipObject != nullptr)
            Ships->push_back(Player->ShipObject);
        RemoveFromFormation(Member);
    }
}

// (06f66ec0-06f67046). An empty list does nothing. The first ship, when in a
// formation and its slot 1 (vtbl +4, Slot04: true for NPC ships) says so, releases its
// formation's other members into the list. A new formation id (the routine); Slot = 0.
// Pass 1 over the list, NPC ships (no player state): skipped while Slot is a player slot
// (the routine, not advanced); with bReleaseLeaderFollowers a leader (+0x14c == 0) releases
// its formation's members first; SetFormation(id, Slot, Formation), ResetFormationState,
// ++Slot. Pass 2, player ships: unless Slot is already a player slot, Slot advances until it
// is one or reaches the slot count (0x34-byte slots); below the count SetFormation(id, Slot,
// Formation) and ResetFormationState (Slot is not advanced).
void __cdecl FormFormation(std::list<FContentShipBase*>* Ships, FFormation* Formation,
                           bool bReleaseLeaderFollowers)
{
    if (Ships->empty())
        return;

    FContentShipBase* First = Ships->front();
    if (First->FormationId != 0xffffffffu && First->Slot04())
        ReleaseFormationMembers(First, Ships);

    unsigned int FormationId = AllocateFormationId();
    int Slot = 0;
    for (std::list<FContentShipBase*>::iterator It = Ships->begin(); It != Ships->end(); ++It)
    {
        FContentShipBase* Ship = *It;
        unsigned int ShipId = Ship->SpaceObjId;
        if (FindContentPlayerStateByShip(&ShipId) != nullptr)
            continue;
        if (Formation->IsPlayerSlot(Slot))
            continue;
        if (bReleaseLeaderFollowers && Ship->FormationSlot == 0)
            ReleaseFormationMembers(Ship, Ships);
        Ship->SetFormation(FormationId, static_cast<unsigned int>(Slot), Formation);
        ResetFormationState(Ship);
        ++Slot;
    }

    for (std::list<FContentShipBase*>::iterator It = Ships->begin(); It != Ships->end(); ++It)
    {
        FContentShipBase* Ship = *It;
        unsigned int ShipId = Ship->SpaceObjId;
        if (FindContentPlayerStateByShip(&ShipId) == nullptr)
            continue;
        if (!Formation->IsPlayerSlot(Slot))
        {
            while (Slot < static_cast<int>(Formation->Slots.size()))
            {
                ++Slot;
                if (Formation->IsPlayerSlot(Slot))
                    break;
            }
        }
        if (Slot < static_cast<int>(Formation->Slots.size()))
        {
            Ship->SetFormation(FormationId, static_cast<unsigned int>(Slot), Formation);
            ResetFormationState(Ship);
        }
    }
}

// (06f6eca0-06f6ed02).
bool __cdecl GetShipDockingTarget(unsigned int const* ShipId, unsigned int* DockTargetId,
                                  int* DockIndex, unsigned int* OutTerminalType)
{
    for (std::list<FDockQueueEntry>::iterator It = GDockingShips.begin(); It != GDockingShips.end();
         ++It)
    {
        if (It->ShipId != *ShipId)
            continue;
        *DockIndex = It->DockIndex;
        *DockTargetId = It->DockTargetId;
        if (OutTerminalType != nullptr)
        {
            pub::SpaceObj::TerminalInfo Info;
            pub::SpaceObj::GetTerminalInfo(It->DockTargetId, *DockIndex, Info);
            *OutTerminalType = Info.Value;
        }
        return true;
    }
    return false;
}

// (06f6dff0-06f6e10d). EQInfo: baseline writes only Type (+0) and the flag byte
// (+4); the port zero-fills the rest (deviations.md, zeroed indeterminate stack bytes).
void __cdecl SetDockEquipment(unsigned int ShipId, bool bOn, bool bFormation, FSystemInfo* System)
{
    pub::SpaceObj::EQInfo Info;
    std::memset(&Info, 0, sizeof(Info));
    Info.Type = 3;
    Info.EquipId = bOn ? 1 : 0;

    if (!bFormation)
    {
        pub::SpaceObj::ActivateEquipment(ShipId, &Info);
        return;
    }

    FContentShipBase* Ship = static_cast<FContentShipBase*>(System->FindShip(ShipId));
    if (Ship == nullptr)
        return;

    std::vector<FContentShipBase*> Members;
    CollectFormationMembers(&Members, Ship->FormationId);
    if (Members.empty())
        Members.push_back(Ship);
    for (int Index = 0; Index < static_cast<int>(Members.size()); ++Index)
        pub::SpaceObj::ActivateEquipment(Members[Index]->SpaceObjId, &Info);
}

// (06f6e810-06f6e9cf). The system is looked up (the routine) at the first
// matching entry of either list and reused. Waiting entries of the ship (from the first
// match on): SetDockEquipment(ship, off, formation, system) and erase. A docking entry: the
// ship is answered Dock(ship, its dock target, -1, 5) once, then every docking entry of the
// ship goes through FinishDocking. With a system and bFormation, the ship (FindShip, not
// null-tested) and the other members of its formation are cancelled too (bFormation 0).
void __cdecl CancelShipDocking(unsigned int ShipId, unsigned int PassThrough, bool bFormation)
{
    FSystemInfo* System = nullptr;

    std::list<FDockQueueEntry>::iterator It = FindDockQueueEntry(&GDockWaitingShips, ShipId);
    while (It != GDockWaitingShips.end())
    {
        if (It->ShipId != ShipId)
        {
            ++It;
            continue;
        }
        if (System == nullptr)
            System = FindSystemInfo(&It->SystemId);
        SetDockEquipment(It->ShipId, false, true, System);
        It = GDockWaitingShips.erase(It);
    }

    It = FindDockQueueEntry(&GDockingShips, ShipId);
    if (It != GDockingShips.end())
    {
        pub::SpaceObj::Dock(ShipId, It->DockTargetId, -1, DOCK_HOST_RESPONSE_5);
        while (It != GDockingShips.end())
        {
            if (It->ShipId != ShipId)
            {
                ++It;
                continue;
            }
            if (System == nullptr)
                System = FindSystemInfo(&It->SystemId);
            FinishDocking(&It);
        }
    }

    if (System == nullptr || !bFormation)
        return;

    FContentShipBase* Ship = static_cast<FContentShipBase*>(System->FindShip(ShipId));
    std::vector<FContentShipBase*> Members;
    CollectFormationMembers(&Members, Ship->FormationId);
    for (int Index = 0; Index < static_cast<int>(Members.size()); ++Index)
    {
        unsigned int MemberId = Members[Index]->SpaceObjId;
        if (MemberId != ShipId)
            CancelShipDocking(MemberId, PassThrough, false);
    }
}

// (06f67740-06f6777a).
void __cdecl RelocateShip(unsigned int const* ShipId, unsigned int const* SystemId,
                          Vector const* Position, Matrix const* Orientation)
{
    pub::SpaceObj::Relocate(*ShipId, *SystemId, *Position, *Orientation);
    if (*SystemId == 0)
        CancelShipDocking(*ShipId, 0, false);
    UpdateDockQueues();
}

// (06f682c0-06f68356). The result local starts at 0xc0000000 (-2.0f); both GetRep
// results zero: result = Lo; `FCOMP Hi; FNSTSW; TEST AH,0x44; JNP` jumps (skips the random
// pick) only when Lo == Hi (C3 alone set, odd parity); unequal or unordered fall through to
// result = float(rand()) * (1/32768) * (Hi - Lo) + Lo (x87, stored as float);
// then pub::Reputation::SetAttitude(repA, repB, result).
float __cdecl SetObjectAttitude(unsigned int ObjA, unsigned int ObjB, float Lo, float Hi)
{
    float Result = -2.0f;
    int RepA;
    int const ResultA = pub::SpaceObj::GetRep(ObjA, RepA);
    int RepB;
    int const ResultB = pub::SpaceObj::GetRep(ObjB, RepB);
    if (ResultA != 0 || ResultB != 0)
        return Result;
    Result = Lo;
    if (!(Lo == Hi))
    {
        double const Random = static_cast<double>(rand()) * (1.0 / 32768.0);
        Result = static_cast<float>(Random * (static_cast<double>(Hi) - Lo) + Lo);
    }
    pub::Reputation::SetAttitude(RepA, RepB, Result);
    return Result;
}

// (06f6c250-06f6c577). Per docking entry:
//   - neither +0x20 nor +0x10 set and more than 90 s (the field = 90.0) since it entered
//     (the routine): its ship (FindShip in its system) is kicked -- gone: by id; a leader or
//     formation-less ship: cancel its directive and dock again; a follower: nothing;
//   - else at the docking point (the routine): its terminal is marked occupied;
//   - else with +0x10: FinishDocking (which advances the iterator);
//   - else its terminal is marked free.
// Then each kicked entry: no ship -> CancelShipDocking(id, dock target, 0); else
// DirectiveCancelOp and, when redocking, a DirectiveDockOp on the dock target.
void __cdecl UpdateDockQueues()
{
    std::vector<FDockKick> Kicks;

    std::list<FDockQueueEntry>::iterator It = GDockingShips.begin();
    while (It != GDockingShips.end())
    {
        FDockQueueEntry& Entry = *It;
        if (!Entry.bField_0x20 && !Entry.bField_0x10)
        {
            double Now = GetContentGameTime();
            // FCOMP 90.0; TEST AH,0x41; JNZ -> not greater falls to the dock-point
            // test, greater times out.
            if (GetContentTimeDistance(&Now, &Entry.Time) > 90.0)
            {
                FSystemInfo* System = FindSystemInfo(&Entry.SystemId);
                FContentShipBase* Ship =
                    static_cast<FContentShipBase*>(System->FindShip(Entry.ShipId));
                if (Ship == nullptr)
                {
                    FDockKick Kick;
                    Kick.Ship = nullptr;
                    Kick.ShipId = Entry.ShipId;
                    Kick.DockTargetId = Entry.DockTargetId;
                    Kick.DockIndex = Entry.DockIndex;
                    Kick.bRedock = false;
                    Kicks.push_back(Kick);
                }
                else if (Ship->FormationSlot == 0 || Ship->FormationId == 0xffffffffu)
                {
                    FDockKick Kick;
                    Kick.Ship = Ship;
                    Kick.ShipId = 0;   // +0x04 left unwritten by baseline (06f6c329-06f6c34d)
                    Kick.DockTargetId = Entry.DockTargetId;
                    Kick.DockIndex = Entry.DockIndex;
                    Kick.bRedock = true;
                    Kicks.push_back(Kick);
                }
                ++It;
                continue;
            }
        }

        if (IsShipAtDockPoint(Entry.SystemId, &Entry.ShipId, &Entry.DockTargetId, &Entry.DockIndex))
        {
            MarkDockTerminal(Entry.DockTargetId, Entry.DockIndex, true);
            ++It;
            continue;
        }
        if (Entry.bField_0x10)
        {
            FinishDocking(&It);
            continue;
        }
        MarkDockTerminal(Entry.DockTargetId, Entry.DockIndex, false);
        ++It;
    }

    for (int Index = 0; Index < static_cast<int>(Kicks.size()); ++Index)
    {
        FDockKick& Kick = Kicks[Index];
        if (Kick.Ship == nullptr)
        {
            CancelShipDocking(Kick.ShipId, Kick.DockTargetId, false);
            continue;
        }
        pub::AI::DirectiveCancelOp Cancel;
        pub::AI::SubmitDirective(Kick.Ship->SpaceObjId, &Cancel);
        if (Kick.bRedock)
        {
            pub::AI::DirectiveDockOp Dock;
            Dock.DockObjectId = Kick.DockTargetId;
            pub::AI::SubmitDirective(Kick.Ship->SpaceObjId, &Dock);
        }
    }
}

// (06f6e9d0-06f6ec7d). The launch queue of the dock target (map operator[],
// it inserts an empty one) is empty -> the terminal is marked free. The
// entry is erased and *It advanced (the routine). Then every waiting entry: dock target's
// type (GetType failing -> response 5); a docking ring's terminal is +0x10 != 0, else -1;
// TryStartDocking. 4: Dock(ship, target, index, 4) and the formation handling -- a target of
// type & 0xc40 (jump gate / airlock / jump hole) takes the followers along
// (DockFormationFollowers), else the routine(&system, &ship, 0, 1); then the entry is
// erased. 3: the entry stays. 0, 1, 2, 5: Dock(ship, target, -1, response) and erase.
void __cdecl FinishDocking(std::list<FDockQueueEntry>::iterator* It)
{
    FDockQueueEntry& Finished = **It;
    if (GDockLaunchQueues[Finished.DockTargetId].empty())
        MarkDockTerminal(Finished.DockTargetId, Finished.DockIndex, false);
    *It = GDockingShips.erase(*It);

    std::list<FDockQueueEntry>::iterator Wait = GDockWaitingShips.begin();
    while (Wait != GDockWaitingShips.end())
    {
        int DockIndex = -1;
        int Response = DOCK_HOST_RESPONSE_5;
        unsigned int TargetType = 0;
        if (pub::SpaceObj::GetType(Wait->DockTargetId, TargetType) == 0)
        {
            if ((TargetType & 0x20) != 0)
                DockIndex = Wait->bField_0x10 ? 1 : 0;
            Response = TryStartDocking(Wait->SystemId, &Wait->ShipId, &Wait->DockTargetId,
                                       &DockIndex, Wait->bField_0x20);
            if (Response == DOCK_HOST_RESPONSE_4)
            {
                // the radio chatter of a granted dock -- the type 2 event
                // (System not null-tested before FindShip) to the field (the routine);
                // its float result is dropped.
                FGcsDockGrantedEvent Event;
                Event.System = FindSystemInfo(&Wait->SystemId);
                if (DockIndex != -1)
                {
                    pub::SpaceObj::TerminalInfo Info;
                    pub::SpaceObj::GetTerminalInfo(Wait->DockTargetId, DockIndex, Info);
                    Event.DockType = Info.Value;
                }
                else
                {
                    FContentShipBase* Ship =
                        static_cast<FContentShipBase*>(Event.System->FindShip(Wait->ShipId));
                    Event.DockType = static_cast<unsigned int>(
                        GetDockTypeForClasses(&Wait->DockTargetId, &Ship->ClassIds));
                }
                Event.Speaker = Wait->DockTargetId;
                Event.Listener = Wait->ShipId;
                Event.DockIndex = DockIndex;
                Event.Response = DOCK_HOST_RESPONSE_4;
                Event.bWaitedInQueue = true;
                GGcsConversations.QueueConversation(&Event);
                unsigned int DockTargetType = 0;
                pub::SpaceObj::GetType(Wait->DockTargetId, DockTargetType);
                pub::SpaceObj::Dock(Wait->ShipId, Wait->DockTargetId, DockIndex, DOCK_HOST_RESPONSE_4);
                if ((DockTargetType & 0xc40) == 0)
                    BreakFormationAtDock(&Wait->SystemId, &Wait->ShipId, false, true);
                else
                {
                    DockFormationFollowers(&Wait->SystemId, &Wait->ShipId, &Wait->DockTargetId,
                                           &DockIndex);
                }
                Wait = GDockWaitingShips.erase(Wait);
                continue;
            }
        }

        switch (Response)
        {
        case 0:
        case 1:
        case 2:
        case 5:
            pub::SpaceObj::Dock(Wait->ShipId, Wait->DockTargetId, -1,
                                static_cast<DOCK_HOST_RESPONSE>(Response));
            Wait = GDockWaitingShips.erase(Wait);
            break;
        case 3:
            ++Wait;
            break;
        default:
            // JA caseD_4: baseline does not advance (TryStartDocking returns 1..5).
            break;
        }
    }
}

// (06f6d820-06f6d919). FindSystemInfo is not null-tested; an unknown ship -> 5.
int __cdecl TryStartDocking(unsigned int SystemId, unsigned int const* ShipId,
                            unsigned int const* DockTargetId, int* DockIndex, bool bField_0x20)
{
    FSystemInfo* System = FindSystemInfo(&SystemId);
    FContentShipBase* Ship = static_cast<FContentShipBase*>(System->FindShip(*ShipId));
    if (Ship == nullptr)
        return DOCK_HOST_RESPONSE_5;

    Vector Position;
    Ship->GetPosition(&Position);
    int ShipRep = Ship->ReputationHandle->GetReputation();
    int Response = CheckDockPermission(&Ship->ClassIds, &ShipRep, ShipId, &Position,
                                       DockTargetId, DockIndex);
    if (Response != DOCK_HOST_RESPONSE_4)
        return Response;

    FDockQueueEntry Entry;
    Entry.Time = GetContentGameTime();
    Entry.SystemId = SystemId;
    Entry.ShipId = Ship->SpaceObjId;
    Entry.DockTargetId = *DockTargetId;
    Entry.DockIndex = *DockIndex;
    Entry.bField_0x10 = false;
    Entry.bField_0x20 = bField_0x20;
    GDockingShips.push_back(Entry);
    return Response;
}

// (06f6c920-06f6cb17).
//   - A player's ship (the routine) asks CanDockAt (the routine), else allowed.
//   - Locked gate (IsGateLockedForShip) or a refused player -> the player gets
//     "dock_disallowed" when on a live mission and refused, else "info_access_denied"
//     (SendNNMessage); index -1; 1.
//   - Not a gate / airlock / hole (type & 0xc40): the attitude (1.0 when the ship's rep is
//     -1) of the target's rep (GetRep) towards the ship's; at or below the field (FCOMP;
//     TEST AH,0x41; JZ = greater passes) -> index -1, a player hears
//     "nnv_dock_denied_destination_hostile"; 0.
//   - DropPlayersLockedOutOfGate(ship, target); ChooseDockTerminal; not 4 -> that.
//   - 4: TerminalInfo of the chosen terminal (result unused); the target's motion
//     (GetMotion failing -> 4); |linear| or |angular| above 0.01 (the field) -> 3, else 4.
int __cdecl CheckDockPermission(std::vector<unsigned int> const* ClassIds, int const* ShipRep,
                                unsigned int const* ShipId, Vector const* Position,
                                unsigned int const* DockTargetId, int* DockIndex)
{
    FContentPlayerState* Player = FindContentPlayerStateByShip(ShipId);
    bool bPlayerMayDock = true;
    if (Player != nullptr)
        bPlayerMayDock = Player->CanDockAt(*DockTargetId);

    if (IsGateLockedForShip(*ShipId, *DockTargetId) || (Player != nullptr && !bPlayerMayDock))
    {
        if (Player != nullptr)
        {
            unsigned int MessageId;
            if (Player->GetLiveMission() != 0 && !bPlayerMayDock)
                MessageId = pub::GetNicknameId("dock_disallowed");
            else
                MessageId = pub::GetNicknameId("info_access_denied");
            pub::Player::SendNNMessage(Player->PlayerId, MessageId);
        }
        *DockIndex = -1;
        return DOCK_HOST_RESPONSE_1;
    }

    unsigned int TargetType = 0;
    pub::SpaceObj::GetType(*DockTargetId, TargetType);
    if ((TargetType & 0xc40) == 0)
    {
        float Attitude = 1.0f;
        if (*ShipRep != -1)
        {
            int TargetRep = 0;
            pub::SpaceObj::GetRep(*DockTargetId, TargetRep);
            pub::Reputation::GetAttitude(TargetRep, *ShipRep, Attitude);
        }
        if (!(Attitude > GDockHostileAttitude))
        {
            *DockIndex = -1;
            if (Player != nullptr)
                pub::Player::SendNNMessage(Player->PlayerId,
                                           pub::GetNicknameId("nnv_dock_denied_destination_hostile"));
            return DOCK_HOST_RESPONSE_0;
        }
    }

    DropPlayersLockedOutOfGate(ShipId, DockTargetId);
    int Response = ChooseDockTerminal(ClassIds, DockTargetId, Position, DockIndex);
    if (Response != DOCK_HOST_RESPONSE_4)
        return Response;

    pub::SpaceObj::TerminalInfo Info;
    pub::SpaceObj::GetTerminalInfo(*DockTargetId, *DockIndex, Info);
    Vector Linear;
    Vector Angular;
    if (pub::SpaceObj::GetMotion(*DockTargetId, Linear, Angular) != 0)
        return DOCK_HOST_RESPONSE_4;
    double LinearSpeed = std::sqrt(static_cast<double>(Linear.y) * Linear.y +
                                   static_cast<double>(Linear.z) * Linear.z +
                                   static_cast<double>(Linear.x) * Linear.x);
    if (LinearSpeed > 0.01)
        return DOCK_HOST_RESPONSE_3;
    double AngularSpeed = std::sqrt(static_cast<double>(Angular.y) * Angular.y +
                                    static_cast<double>(Angular.z) * Angular.z +
                                    static_cast<double>(Angular.x) * Angular.x);
    if (AngularSpeed > 0.01)
        return DOCK_HOST_RESPONSE_3;
    return DOCK_HOST_RESPONSE_4;
}

// (06f6c580-06f6c8db). Candidates (a local list, the routine / the routine):
//   - index -1: every terminal (GetTerminalInfo 0, 1, ... until it fails; failing at 0 -> 5)
//     that fits the classes and is not in the docking list; none fitting at all -> 2;
//   - a given index: its TerminalInfo (failing -> 5); not fitting -> 2; free -> the index.
// No candidate -> 3. Else 4 with the index: given -> itself (random pick of one); -1 with a
// Position -> the candidate whose hardpoint (GetHardpoint of the terminal's name, results
// not tested) is nearest (strictly smaller squared distance, from FLT_MAX; none -> -1);
// -1 without a Position -> a random candidate (rand() * n / 32768; no rand() for one).
// Every refusal also stores -1.
int __cdecl ChooseDockTerminal(std::vector<unsigned int> const* ClassIds,
                               unsigned int const* DockTargetId, Vector const* Position,
                               int* DockIndex)
{
    std::list<int> Candidates;
    pub::SpaceObj::TerminalInfo Info;

    if (*DockIndex == -1)
    {
        int Index = 0;
        if (pub::SpaceObj::GetTerminalInfo(*DockTargetId, 0, Info) != 0)
        {
            *DockIndex = -1;
            return DOCK_HOST_RESPONSE_5;
        }
        bool bAnyFits = false;
        do
        {
            if (DoesTerminalFitClasses(Info.Value, ClassIds))
            {
                bAnyFits = true;
                if (!IsDockTerminalInUse(*DockTargetId, Index))
                    Candidates.push_back(Index);
            }
            ++Index;
        } while (pub::SpaceObj::GetTerminalInfo(*DockTargetId, Index, Info) == 0);
        if (!bAnyFits)
        {
            *DockIndex = -1;
            return DOCK_HOST_RESPONSE_2;
        }
    }
    else
    {
        if (pub::SpaceObj::GetTerminalInfo(*DockTargetId, *DockIndex, Info) != 0)
        {
            *DockIndex = -1;
            return DOCK_HOST_RESPONSE_5;
        }
        if (!DoesTerminalFitClasses(Info.Value, ClassIds))
        {
            *DockIndex = -1;
            return DOCK_HOST_RESPONSE_2;
        }
        if (!IsDockTerminalInUse(*DockTargetId, *DockIndex))
            Candidates.push_back(*DockIndex);
    }

    if (Candidates.empty())
    {
        *DockIndex = -1;
        return DOCK_HOST_RESPONSE_3;
    }

    if (*DockIndex == -1 && Position != nullptr)
    {
        int BestIndex = -1;
        float BestDistance = FLT_MAX;
        // baseline keeps one local across the loop and overwrites it with the difference
        // (06f6c720-06f6c736). Common HardpointPosition (0630ef30) writes nothing when it
        // fails, so a failed GetHardpoint leaves the previous difference in it, as here.
        // Before the first call the baseline slot is never written (stack residue); the port
        // starts it at zero.
        Vector Work;
        Work.x = 0.0f;
        Work.y = 0.0f;
        Work.z = 0.0f;
        for (std::list<int>::iterator It = Candidates.begin(); It != Candidates.end(); ++It)
        {
            pub::SpaceObj::GetTerminalInfo(*DockTargetId, *It, Info);
            pub::SpaceObj::GetHardpoint(*DockTargetId, Info.Name, &Work, nullptr);
            Work.x = Work.x - Position->x;
            Work.y = Work.y - Position->y;
            Work.z = Work.z - Position->z;
            // (x*x + z*z) + y*y on the x87 stack.
            double Distance = static_cast<double>(Work.x) * Work.x +
                              static_cast<double>(Work.z) * Work.z +
                              static_cast<double>(Work.y) * Work.y;
            // FCOM best; TEST AH,5; JP -> not below keeps the best.
            if (Distance < BestDistance)
            {
                BestIndex = *It;
                BestDistance = static_cast<float>(Distance);
            }
        }
        *DockIndex = BestIndex;
        return DOCK_HOST_RESPONSE_4;
    }

    int Count = static_cast<int>(Candidates.size());
    int Pick = 0;
    if (Count - 1 != 0)
        Pick = rand() * Count / 32768;
    std::list<int>::iterator Chosen = Candidates.begin();
    for (; Pick != 0; --Pick)
        ++Chosen;
    *DockIndex = *Chosen;
    return DOCK_HOST_RESPONSE_4;
}

// (06f6bdd0-06f6c226). The terminal's hardpoint (GetTerminalInfo, then
// GetHardpoint with its orientation) and the ship's location (the ship must be known in the
// system; GetLocation); any failure -> false. By terminal type:
//   1, 3, 4, 5: |ship - hardpoint|^2 < 40000 (the field);
//   7: < 6250000 (the field); 8: < 250000 (the field); 2 and others: false;
//   6 (a berth): |dx| < 10 and |dz| < 10 (the field); else, with Far = the hardpoint
//     transform applied to (0, 0, -5000) (math engine slot +0x28), the ship lies on the
//     hardpoint-Far segment (| 5000 - (|ship - Far| + |ship - hardpoint|) | < 25, the field
//     / the field, lengths by the 0x5f3759df idiom); else the distance to the hardpoint is
//     below 1.85 (the field) times the target's radius (GetRadius).
// Every "< c" is FCOMP; TEST AH,5; JP (not below -> false).
bool __cdecl IsShipAtDockPoint(unsigned int SystemId, unsigned int const* ShipId,
                               unsigned int const* DockTargetId, int const* DockIndex)
{
    pub::SpaceObj::TerminalInfo Info;
    if (pub::SpaceObj::GetTerminalInfo(*DockTargetId, *DockIndex, Info) != 0)
        return false;
    Vector Hardpoint;
    Matrix HardpointOrientation;
    if (pub::SpaceObj::GetHardpoint(*DockTargetId, Info.Name, &Hardpoint, &HardpointOrientation) != 0)
        return false;
    FSystemInfo* System = FindSystemInfo(&SystemId);
    if (System->FindShip(*ShipId) == nullptr)
        return false;
    Vector ShipPosition;
    Matrix ShipOrientation;
    if (pub::SpaceObj::GetLocation(*ShipId, ShipPosition, ShipOrientation) != 0)
        return false;

    switch (Info.Value)
    {
    case 1:
    case 3:
    case 4:
    case 5:
    {
        // The one CALL of the routine here (06f6be9a); (z*z + y*y) + x*x on the x87 stack.
        Vector const Delta = SubtractVectors(ShipPosition, Hardpoint);
        return (static_cast<double>(Delta.z) * Delta.z + static_cast<double>(Delta.y) * Delta.y) +
                   static_cast<double>(Delta.x) * Delta.x < 40000.0f;
    }
    case 7:
        return DistanceSquared(ShipPosition, Hardpoint) < 6250000.0f;
    case 8:
        return DistanceSquared(ShipPosition, Hardpoint) < 250000.0f;
    case 6:
        break;
    default:
        return false;
    }

    if (std::fabs(static_cast<double>(Hardpoint.x) - ShipPosition.x) < 10.0 &&
        std::fabs(static_cast<double>(Hardpoint.z) - ShipPosition.z) < 10.0)
        return true;

    FTransform Transform;
    std::memcpy(&Transform.Rotation, &HardpointOrientation, sizeof(Transform.Rotation));
    Transform.Translation.X = Hardpoint.x;
    Transform.Translation.Y = Hardpoint.y;
    Transform.Translation.Z = Hardpoint.z;
    FVector Back;
    Back.X = 0.0f;
    Back.Y = 0.0f;
    Back.Z = -5000.0f;
    if (GDockMathEngine == nullptr)
        GDockMathEngine = ContentAcquire3DMathEngine();
    FVector FarPoint;
    GDockMathEngine->TransformXform(FarPoint, Transform, Back);
    Vector Far;
    Far.x = FarPoint.X;
    Far.y = FarPoint.Y;
    Far.z = FarPoint.Z;

    double ToFarSquare = DistanceSquared(ShipPosition, Far);
    float ToFarStored = static_cast<float>(ToFarSquare);
    double ToHardpointSquare = DistanceSquared(ShipPosition, Hardpoint);
    double ToHardpoint = ApproximateSqrt(static_cast<float>(ToHardpointSquare), ToHardpointSquare);
    double ToFar = ApproximateSqrt(ToFarStored, ToFarStored);
    if (std::fabs(5000.0f - (ToFar + ToHardpoint)) < 25.0)
        return true;

    float Radius = 0.0f;
    Vector Center;
    pub::SpaceObj::GetRadius(*DockTargetId, Radius, Center);
    // unlike the routine, the three differences stay on the x87 stack
    // (FSUB with no FSTP to float) before (z*z + y*y) + x*x.
    double const DeltaX = static_cast<double>(ShipPosition.x) - Hardpoint.x;
    double const DeltaY = static_cast<double>(ShipPosition.y) - Hardpoint.y;
    double const DeltaZ = static_cast<double>(ShipPosition.z) - Hardpoint.z;
    double DistanceSquare = DeltaZ * DeltaZ + DeltaY * DeltaY + DeltaX * DeltaX;
    double Distance = ApproximateSqrt(static_cast<float>(DistanceSquare), DistanceSquare);
    // FCOMPP of Radius * 1.85 against the distance; TEST AH,0x41; JNZ -> false.
    return static_cast<double>(Radius) * 1.85f > Distance;
}

// (06f6e420-06f6e52f). GetType's result is not tested; pub::SpaceObj::GetType
// zeroes the type before any check (Server 06cf8db5), so a failure reads 0.
int __cdecl GetDockTypeForClasses(unsigned int const* DockTargetId,
                                  std::vector<unsigned int> const* ClassIds)
{
    unsigned int TargetType = 0;
    pub::SpaceObj::GetType(*DockTargetId, TargetType);
    if ((TargetType & 0x840) != 0)
        return 7;
    if ((TargetType & 0x400) != 0)
        return 8;
    if ((TargetType & 0x20) != 0)
        return ContainsClass(ClassIds, GIdCanUseBerths) ? 6 : 0;
    if (ContainsClass(ClassIds, GIdCanUseBerths))
        return 1;
    if (ContainsClass(ClassIds, GIdCanUseSmallMoors))
        return 3;
    if (ContainsClass(ClassIds, GIdCanUseMediumMoors))
        return 4;
    return ContainsClass(ClassIds, GIdCanUseLargeMoors) ? 5 : 0;
}

// (06f6dd80-06f6def4). FindSystemInfo / FindShip are not null-tested. Every
// member after the first (the sorted leader) is appended to the docking list {system, member,
// target, index, +0x10 0, now, +0x20 0} (the routine copy) and told Dock(member, target,
// index, 4).
void __cdecl DockFormationFollowers(unsigned int const* SystemId, unsigned int const* ShipId,
                                    unsigned int const* DockTargetId, int const* DockIndex)
{
    FSystemInfo* System = FindSystemInfo(SystemId);
    FContentShipBase* Ship = static_cast<FContentShipBase*>(System->FindShip(*ShipId));
    std::vector<FContentShipBase*> Members;
    CollectFormationMembers(&Members, Ship->FormationId);
    for (int Index = 1; Index < static_cast<int>(Members.size()); ++Index)
    {
        FDockQueueEntry Entry;
        Entry.Time = GetContentGameTime();
        Entry.SystemId = *SystemId;
        Entry.ShipId = Members[Index]->SpaceObjId;
        Entry.DockTargetId = *DockTargetId;
        Entry.DockIndex = *DockIndex;
        Entry.bField_0x10 = false;
        Entry.bField_0x20 = false;
        GDockingShips.push_back(Entry);
        unsigned int MemberId = Members[Index]->SpaceObjId;
        pub::SpaceObj::Dock(MemberId, *DockTargetId, *DockIndex, DOCK_HOST_RESPONSE_4);
    }
}

// (06f6df00-06f6dfe6, cdecl). A player's ship (the routine) -> true at once.
// Otherwise the ship (the routine, not null-tested) gives its formation +0x154; the members
// (the routine into a local vector -- EH state 0 of FuncInfo 06fc5010, funclet 06fafe40) are
// walked in order and the first player's ship among them -> true; none -> false. The vector
// is freed on both exits (the routine).
bool __cdecl IsPlayerInShipFormation(FSystemInfo* System, unsigned int ShipId)
{
    if (FindContentPlayerStateByShip(&ShipId) != nullptr)
        return true;
    std::vector<FContentShipBase*> Members;
    FContentShipBase* Ship = static_cast<FContentShipBase*>(System->FindShip(ShipId));
    CollectFormationMembers(&Members, Ship->FormationId);
    for (std::vector<FContentShipBase*>::iterator It = Members.begin(); It != Members.end(); ++It)
    {
        unsigned int MemberId = (*It)->SpaceObjId;
        if (FindContentPlayerStateByShip(&MemberId) != nullptr)
            return true;
    }
    return false;
}

// (06f6ed10-06f6ee30, cdecl). Only a wingman: +0x14c != 0 and +0x154 != -1.
// The leader of its formation (the routine, not null-tested) is looked up by id in
// GDockingShips (the field, first match); none -> done. GetTerminalInfo(the entry's target,
// its index) (status not tested); Value 7 or 8 only: a docking entry {the ship's system
// (the routine()->+0x48), the ship, target, index, +0x10 0, now (the routine), +0x20 0} is
// appended (node new(0x30), size the field + 1), then Dock(ship, target, index, 4).
void __cdecl FollowLeaderIntoDock(FContentShipBase* Ship)
{
    if (Ship->FormationSlot == 0 || Ship->FormationId == 0xffffffffu)
        return;
    FContentShipBase* Leader = FindFormationLeader(Ship->FormationId);
    unsigned int const LeaderId = Leader->SpaceObjId;
    std::list<FDockQueueEntry>::iterator It = GDockingShips.begin();
    while (It != GDockingShips.end() && It->ShipId != LeaderId)
        ++It;
    if (It == GDockingShips.end())
        return;

    unsigned int DockTargetId = It->DockTargetId;
    int const DockIndex = It->DockIndex;
    pub::SpaceObj::TerminalInfo Info;
    pub::SpaceObj::GetTerminalInfo(It->DockTargetId, DockIndex, Info);
    if (Info.Value != 7 && Info.Value != 8)
        return;

    FDockQueueEntry Entry;
    Entry.Time = GetContentGameTime();
    Entry.SystemId = Ship->GetSystem()->SystemId;
    Entry.ShipId = Ship->SpaceObjId;
    Entry.DockTargetId = DockTargetId;
    Entry.DockIndex = DockIndex;
    Entry.bField_0x10 = false;
    Entry.bField_0x20 = false;
    GDockingShips.push_back(Entry);
    unsigned int ShipId = Ship->SpaceObjId;
    pub::SpaceObj::Dock(ShipId, DockTargetId, DockIndex, DOCK_HOST_RESPONSE_4);
}

// (06f66690-06f66c75, cdecl; EH FuncInfo of the branch). Leader / ship looked
// up in System (the routine; -4 / -3 when missing); both formation ids (+0x154) and both
// formation leaders (the routine) are read first, then the player states of both ships
// (the routine). The formations "multiplayer_freighter" and "freighter_basic" (the routine)
// must both exist (-8).
// Not multiplayer-with-a-player (06f667be-06f66a22): a formation-less leader founds one
// (AllocateFormationId, slot 0, freighter_basic, ResetFormationState); the ship already in it
// -> -1. The first member of the leader's formation (the routine; element 0 read without an
// empty test) must not be a wingman (-8). When that member is docking (GetShipDockingTarget)
// and the requesting ship is a player's: a gate locked for the player (the routine) or a
// target type with 0x120 -> -9 (the type local is the freighter_basic slot reused; GetType
// zeroes it even when it fails, Server 06cf8db5). Then the first slot of the member's formation (+0x150)
// that no member holds (+0x14c) and that suits the ship (a player's ship a player slot,
// , an NPC a non-player slot; the player lookup repeated per slot) is taken:
// SetFormation(member's formation, slot, formation) -> 0. None -> -5.
// Multiplayer with a player (06f66a23-06f66c75): both players' ships -> they must be grouped
// (the routine, -8); the same formation leader -> -1; "multiplayer_freighter" looked up again;
// a formation-less leader founds one with it. The joining ships: the ship's whole formation
// when it leads one (each member found again through its own system, out of its formation:
// CancelDockingBehindLeader, SetFormation(-1, 0, null)), else the ship alone (the routine).
// Each joining ship takes the first slot not held by a member of the leader's formation as
// collected before the loop (the list is not refreshed, so several joiners can get the same
// slot) -- SetFormation(leader's formation, slot, formation), ResetFormationState; a full
// formation (or none with slots) -> -5 at that joiner. Otherwise 0.
int __cdecl HandleFormationRequest(unsigned int const* ShipId, unsigned int const* LeaderId,
                                   FSystemInfo* System)
{
    FContentShipBase* Leader = static_cast<FContentShipBase*>(System->FindShip(*LeaderId));
    if (Leader == nullptr)
        return -4;
    FContentShipBase* Ship = static_cast<FContentShipBase*>(System->FindShip(*ShipId));
    if (Ship == nullptr)
        return -3;

    unsigned int LeaderFormation = Leader->FormationId;
    unsigned int const ShipFormation = Ship->FormationId;
    FContentShipBase* const ShipLeader = Ship->GetFormationLeader();
    FContentShipBase* const LeaderLeader = Leader->GetFormationLeader();
    FContentPlayerState* const ShipPlayer = FindContentPlayerStateByShip(ShipId);
    FContentPlayerState* const LeaderPlayer = FindContentPlayerStateByShip(LeaderId);
    bool const bPlayerRules =
        !pub::SinglePlayer() && (ShipPlayer != nullptr || LeaderPlayer != nullptr);

    unsigned int FormationName = CreateID("multiplayer_freighter");
    FFormation* const MultiplayerFreighter = FindFormation(&FormationName);
    FormationName = CreateID("freighter_basic");
    FFormation* const FreighterBasic = FindFormation(&FormationName);
    if (MultiplayerFreighter == nullptr || FreighterBasic == nullptr)
        return -8;

    if (!bPlayerRules)
    {
        if (LeaderFormation == 0xffffffffu)
        {
            LeaderFormation = AllocateFormationId();
            Leader->SetFormation(LeaderFormation, 0, FreighterBasic);
            ResetFormationState(Leader);
        }
        if (ShipFormation == LeaderFormation)
            return -1;

        std::vector<FContentShipBase*> Members;
        CollectFormationMembers(&Members, LeaderFormation);
        FContentShipBase* const First = *Members.data();     // no empty test
        if (First->FormationSlot != 0 && First->FormationId != 0xffffffffu)
            return -8;

        FContentPlayerState* const Player = FindContentPlayerStateByShip(ShipId);
        unsigned int DockTargetId = 0;
        unsigned int FirstId = First->SpaceObjId;
        int DockIndex = 0;
        GetShipDockingTarget(&FirstId, &DockTargetId, &DockIndex, nullptr);
        if ((DockTargetId & 0x3fffffff) != 0)
        {
            unsigned int TargetType = 0;
            pub::SpaceObj::GetType(DockTargetId, TargetType);
            if (Player != nullptr)
            {
                unsigned int PlayerId = Player->PlayerId;
                if (IsContentGateLocked(&PlayerId, &DockTargetId) || (TargetType & 0x120) != 0)
                    return -9;
            }
        }

        FFormation* const Formation = First->Formation;
        int const SlotCount = static_cast<int>(Formation->Slots.size());
        for (int Slot = 0; Slot < SlotCount; ++Slot)
        {
            bool bTaken = false;
            for (std::vector<FContentShipBase*>::iterator It = Members.begin(); It != Members.end();
                 ++It)
            {
                if ((*It)->FormationSlot == static_cast<unsigned int>(Slot))
                {
                    bTaken = true;
                    break;
                }
            }
            if (bTaken)
                continue;
            bool const bPlayerShip = FindContentPlayerStateByShip(ShipId) != nullptr;
            bool const bPlayerSlot = Formation->IsPlayerSlot(Slot);
            if (bPlayerShip != bPlayerSlot)
                continue;
            Ship->SetFormation(First->FormationId, static_cast<unsigned int>(Slot), Formation);
            return 0;
        }
        return -5;
    }

    if (ShipPlayer != nullptr && LeaderPlayer != nullptr &&
        !ShipPlayer->IsGroupMember(LeaderPlayer->PlayerId))
        return -8;
    if (ShipLeader != nullptr && ShipLeader == LeaderLeader)
        return -1;

    FormationName = CreateID("multiplayer_freighter");
    FFormation* const Formation = FindFormation(&FormationName);
    if (LeaderFormation == 0xffffffffu)
    {
        LeaderFormation = AllocateFormationId();
        Leader->SetFormation(LeaderFormation, 0, Formation);
        ResetFormationState(Leader);
    }

    std::vector<FContentShipBase*> Joining;
    if (ShipLeader == Ship && ShipFormation != 0xffffffffu)
    {
        CollectFormationMembers(&Joining, ShipFormation);
        for (std::vector<FContentShipBase*>::iterator It = Joining.begin(); It != Joining.end(); ++It)
        {
            unsigned int const MemberId = (*It)->SpaceObjId;
            FContentShipBase* Member =
                static_cast<FContentShipBase*>((*It)->GetSystem()->FindShip(MemberId));
            if (Member == nullptr || Member->FormationId == 0xffffffffu)
                continue;
            CancelDockingBehindLeader(Member, Member->FormationId);
            Member->SetFormation(0xffffffffu, 0, nullptr);
        }
    }
    else
    {
        Joining.push_back(Ship);
    }

    std::vector<FContentShipBase*> Members;
    CollectFormationMembers(&Members, LeaderFormation);
    int const SlotCount = static_cast<int>(Formation->Slots.size());
    for (std::vector<FContentShipBase*>::iterator Join = Joining.begin(); Join != Joining.end();
         ++Join)
    {
        int Slot = 0;
        if (SlotCount <= 0)
            return -5;
        for (;;)
        {
            bool bTaken = false;
            for (std::vector<FContentShipBase*>::iterator It = Members.begin(); It != Members.end();
                 ++It)
            {
                if ((*It)->FormationSlot == static_cast<unsigned int>(Slot))
                {
                    bTaken = true;
                    break;
                }
            }
            if (!bTaken)
                break;
            ++Slot;
            if (Slot >= SlotCount)
                return -5;
        }
        (*Join)->SetFormation(LeaderFormation, static_cast<unsigned int>(Slot), Formation);
        ResetFormationState(*Join);
    }
    return 0;
}

// (06f6e110-06f6e419, RET 4; SpaceSupport message 0x12 06f5e019).
// GetType(target) (status not tested; GetType zeroes the type before
//     any check, Server 06cf8db5) -> index 0 for a docking ring (& 0x20), else -1.
// Response = TryStartDocking(system, &ship, &target, &index, +0x0c);
//     System = FindSystemInfo(&system).
//   Response != 5 (06f6e173-06f6e20a): the type 2 GCS event {System, target, ship, index,
//     the terminal's TerminalInfo::Value (index != -1) or GetDockTypeForClasses(&target, the
//     ship's ClassIds) (index -1; FindShip not null-tested), Response, 0} to the field
//     (the routine, float result dropped).
//   4 (06f6e211-06f6e282): GetType(target) into the System local; Dock(ship, target, index, 4);
//     type & 0xc40 (gate / lane kinds) -> DockFormationFollowers(&system, &ship, &target,
//     &index); else BreakFormationAtDock(&system, &ship, false, (type & 0x120) != 0).
//   3 (06f6e28e-06f6e3d7): a waiting entry {system, ship, target, index, +0x10 0, now, +0x20
//     0} (the the routine init inlined) goes into GDockWaitingShips: a player's ship, a ship
//     with a player in its formation (the routine), or a space object whose +0x1b4 is 0 or 1
//     (the routine; read whatever the object is -- an NPC ship's CreationKind) is placed
//     behind the players already waiting (first node whose ship is no player's); any other
//     ship goes to the end. Dock(ship, target, -1, 3).
//   2, 1, 0, 5 (06f6e3d9-06f6e40d): Dock(ship, target, index, Response).
//   Returns Response.
int HandleDockRequest(FDockRequest const* Request)
{
    unsigned int TargetType = 0;
    pub::SpaceObj::GetType(Request->DockTargetId, TargetType);
    int DockIndex = (TargetType & 0x20) != 0 ? 0 : -1;
    int const Response = TryStartDocking(Request->SystemId, &Request->ShipId,
                                         &Request->DockTargetId, &DockIndex,
                                         Request->bField_0x0c);
    FSystemInfo* System = FindSystemInfo(&Request->SystemId);

    if (Response != DOCK_HOST_RESPONSE_5)
    {
        FGcsDockGrantedEvent Event;
        Event.System = System;
        if (DockIndex != -1)
        {
            pub::SpaceObj::TerminalInfo Info;
            pub::SpaceObj::GetTerminalInfo(Request->DockTargetId, DockIndex, Info);
            Event.DockType = Info.Value;
        }
        else
        {
            FContentShipBase* Ship =
                static_cast<FContentShipBase*>(System->FindShip(Request->ShipId));
            Event.DockType = static_cast<unsigned int>(
                GetDockTypeForClasses(&Request->DockTargetId, &Ship->ClassIds));
        }
        Event.Speaker = Request->DockTargetId;
        Event.Listener = Request->ShipId;
        Event.DockIndex = DockIndex;
        Event.Response = Response;
        GGcsConversations.QueueConversation(&Event);
    }

    if (Response == DOCK_HOST_RESPONSE_4)
    {
        unsigned int DockTargetType = 0;
        pub::SpaceObj::GetType(Request->DockTargetId, DockTargetType);
        pub::SpaceObj::Dock(Request->ShipId, Request->DockTargetId, DockIndex,
                            DOCK_HOST_RESPONSE_4);
        if ((DockTargetType & 0xc40) != 0)
        {
            DockFormationFollowers(&Request->SystemId, &Request->ShipId, &Request->DockTargetId,
                                   &DockIndex);
        }
        else
        {
            bool const bReleasePlayers = (DockTargetType & 0x120) != 0;
            BreakFormationAtDock(&Request->SystemId, &Request->ShipId, false,
                                 bReleasePlayers);
        }
        return Response;
    }

    if (Response == DOCK_HOST_RESPONSE_3)
    {
        FDockQueueEntry Entry;
        Entry.Time = GetContentGameTime();
        Entry.SystemId = Request->SystemId;
        Entry.ShipId = Request->ShipId;
        Entry.DockTargetId = Request->DockTargetId;
        Entry.DockIndex = DockIndex;
        Entry.bField_0x10 = false;
        Entry.bField_0x20 = false;

        bool bBehindPlayers = FindContentPlayerStateByShip(&Entry.ShipId) != nullptr ||
                              IsPlayerInShipFormation(System, Entry.ShipId);
        if (!bBehindPlayers)
        {
            FContentNpcShip* Object =
                static_cast<FContentNpcShip*>(System->FindSpaceObj(Entry.ShipId));
            bBehindPlayers = Object != nullptr &&
                             (Object->CreationKind == 0 || Object->CreationKind == 1);
        }
        std::list<FDockQueueEntry>::iterator Position = GDockWaitingShips.end();
        if (bBehindPlayers)
        {
            Position = GDockWaitingShips.begin();
            while (Position != GDockWaitingShips.end() &&
                   FindContentPlayerStateByShip(&Position->ShipId) != nullptr)
                ++Position;
        }
        GDockWaitingShips.insert(Position, Entry);
        pub::SpaceObj::Dock(Request->ShipId, Request->DockTargetId, -1, DOCK_HOST_RESPONSE_3);
        return Response;
    }

    if (Response == DOCK_HOST_RESPONSE_2 || Response == DOCK_HOST_RESPONSE_1 ||
        Response == DOCK_HOST_RESPONSE_0 || Response == DOCK_HOST_RESPONSE_5)
    {
        pub::SpaceObj::Dock(Request->ShipId, Request->DockTargetId, DockIndex,
                            static_cast<DOCK_HOST_RESPONSE>(Response));
    }
    return Response;
}

// (06f6d920-06f6dd7a). The system is not null-tested. The ship must be known and
// lead (slot +0x14c 0) or be formation-less (+0x154 -1); its formation (the routine) needs
// more than one member. Members after the first: in the system's space objects
// (the routine) -> the NPC list, else the ship object of the player whose ship it is
// (the routine, not null-tested) -> the player list (both the routine push_backs).
//   NPC members: RemoveFromFormation (the routine), FollowDirectivesOf(leader) (the routine);
//     a leader guide (the routine) is cloned (slot +0x0c), set on the member
//     (the routine(clone, 1, 0)) and the creating reference released (the member's guide,
//     slot +0x04).
//   Without either flag: done. Player members: RemoveFromFormation.
//   bRedirectPlayers (non-empty list), per player ship: the leader's directives copied
//     (the routine, each AddRef'ed); non-empty: a first directive of type 0x13 (slot +0x14)
//     gives a DirectiveDockOp copy of its op (+0x10c); the ship's docking entry, when found,
//     overrides DockObjectId / Field_14 = 1 / DockPortIndex; SubmitDirective. Empty list:
//     only with a docking entry -- a fresh DirectiveDockOp {Field_08 1, the entry's target,
//     Field_14 1, its index}. Then every copied directive is released.
// The leader's guide is read at +0x178 whatever the ship is; for a player's ship that is the
// low dword of CalmSince. The port reads a guide from NPC ships only (Slot04).
// Type 0x13 also names InstantTradelane directives (GetType ICF-shared): baseline copies their
// bytes (and the bytes past their end) as a DockOp; the port copies Dock directives only.
void __cdecl BreakFormationAtDock(unsigned int const* SystemId, unsigned int const* ShipId,
                                  bool bRedirectPlayers, bool bReleasePlayers)
{
    std::vector<FContentShipBase*> Members;
    FSystemInfo* System = FindSystemInfo(SystemId);
    FContentShipBase* Leader = static_cast<FContentShipBase*>(System->FindShip(*ShipId));
    if (Leader == nullptr)
        return;
    if (Leader->FormationSlot != 0 && Leader->FormationId != 0xffffffffu)
        return;
    CollectFormationMembers(&Members, Leader->FormationId);
    if (Members.size() <= 1)
        return;

    std::vector<FContentShipBase*> NpcMembers;
    std::vector<FContentShipBase*> PlayerMembers;
    for (int Index = 1; Index < static_cast<int>(Members.size()); ++Index)
    {
        FContentSpaceObj* Object = System->FindSpaceObj(Members[Index]->SpaceObjId);
        if (Object != nullptr)
        {
            NpcMembers.push_back(static_cast<FContentShipBase*>(Object));
            continue;
        }
        unsigned int MemberId = Members[Index]->SpaceObjId;
        PlayerMembers.push_back(FindContentPlayerStateByShip(&MemberId)->ShipObject);
    }

    for (int Index = 0; Index < static_cast<int>(NpcMembers.size()); ++Index)
    {
        FContentNpcShip* Member = static_cast<FContentNpcShip*>(NpcMembers[Index]);
        RemoveFromFormation(Member);
        Member->FollowDirectivesOf(Leader);
        FContentGuide* LeaderGuide =
            Leader->Slot04() ? static_cast<FContentNpcShip*>(Leader)->GetGuide() : nullptr;
        if (LeaderGuide == nullptr)
            continue;
        Member->SetGuide(LeaderGuide->Clone(), true, false);
        Member->GetGuide()->Release();
    }

    if (!bReleasePlayers && !bRedirectPlayers)
        return;
    for (int Index = 0; Index < static_cast<int>(PlayerMembers.size()); ++Index)
        RemoveFromFormation(PlayerMembers[Index]);
    if (!bRedirectPlayers || PlayerMembers.empty())
        return;

    for (int Index = 0; Index < static_cast<int>(PlayerMembers.size()); ++Index)
    {
        FContentShipBase* Player = PlayerMembers[Index];
        // EH state 3 of FuncInfo 06fc4f80: the copy lives in a list that owns its references
        // (dtor the routine: Release every element, then erase) -- FMissionDirectiveList.
        FMissionDirectiveList Directives;
        Directives.Items = Leader->CopyDirectives();
        std::list<FDockQueueEntry>::iterator Entry =
            FindDockQueueEntry(&GDockingShips, Player->SpaceObjId);
        if (!Directives.Items.empty())
        {
            FObjListDock* Dock = Directives.Items.front()->GetType() == 0x13
                                     ? dynamic_cast<FObjListDock*>(Directives.Items.front())
                                     : nullptr;
            if (Dock != nullptr)
            {
                pub::AI::DirectiveDockOp Op(Dock->Op);
                if (Entry != GDockingShips.end())
                {
                    Op.DockObjectId = Entry->DockTargetId;
                    Op.Field_14 = 1;
                    Op.DockPortIndex = Entry->DockIndex;
                }
                pub::AI::SubmitDirective(Player->SpaceObjId, &Op);
            }
        }
        else if (Entry != GDockingShips.end())
        {
            pub::AI::DirectiveDockOp Op;
            Op.Priority = 1;
            Op.DockObjectId = Entry->DockTargetId;
            Op.Field_14 = 1;
            Op.DockPortIndex = Entry->DockIndex;
            pub::AI::SubmitDirective(Player->SpaceObjId, &Op);
        }
        // The list's destructor releases the copied references.
    }
}

// (06f6d040-06f6d1b1). Ship id & 0x3fffffff == 0 -> nothing. The ship's system
// (GetSystem into a local preset to 0) must have a SystemInfo and know the ship, which must
// be in a formation; each member that is a player's ship and for whom the target is a locked
// gate (the IsContentGateLocked test inlined) leaves the formation.
void __cdecl DropPlayersLockedOutOfGate(unsigned int const* ShipId, unsigned int const* DockTargetId)
{
    if ((*ShipId & 0x3fffffffu) == 0)
        return;
    unsigned int SystemId = 0;
    pub::SpaceObj::GetSystem(*ShipId, SystemId);
    FSystemInfo* System = FindSystemInfo(&SystemId);
    if (System == nullptr)
        return;
    FContentShipBase* Ship = static_cast<FContentShipBase*>(System->FindShip(*ShipId));
    if (Ship == nullptr || Ship->FormationId == 0xffffffffu)
        return;

    std::vector<FContentShipBase*> Members;
    CollectFormationMembers(&Members, Ship->FormationId);
    for (int Index = 0; Index < static_cast<int>(Members.size()); ++Index)
    {
        unsigned int MemberId = Members[Index]->SpaceObjId;
        FContentPlayerState* Player = FindContentPlayerStateByShip(&MemberId);
        if (Player == nullptr)
            continue;
        if (IsContentGateLocked(&Player->PlayerId, DockTargetId))
            RemoveFromFormation(Members[Index]);
    }
}

// (06f67440-06f67630). Per object of System->SpaceObjs (list head +0xe0), taken
// as an NPC ship, skipped when its +0x5a8 byte is set:
//   - the squared distance to the placement's position (differences and sum on the x87
//     stack) not above *Radius squared (stored as float; TEST AH,0x41, JNP) -> counted;
//   - else only with *ConeRange > 0.0 (the field; JNZ skips not-greater): V = ship -
//     placement (the routine, float), |V| = |V|^2 * (|V|^2) not above *ConeRange
//     (JP skips greater); F = -(column 2 of the orientation) (the routine + the routine);
//     |F|^2, |V|^2, V.F and |V| stored as float, cos = V.F / (|F| * |V|); counted when
//     cos(0.6108652334660292 rad, double the field = 35 degrees) is not above it
//     (FCOMPP, TEST AH,0x41, JP).
//   - counted only with no ClassId or HasClassId(ClassId) (the routine).
int __cdecl CountNpcShipsNear(FSystemInfo* System, FSpaceObjPlacement const* Placement,
                              float const* Radius, float const* ConeRange,
                              unsigned int const* ClassId)
{
    float RadiusSquare = static_cast<float>(static_cast<double>(*Radius) * *Radius);
    int Count = 0;
    for (std::list<FContentSpaceObj*>::iterator It = System->SpaceObjs.begin();
         It != System->SpaceObjs.end(); ++It)
    {
        FContentNpcShip* Ship = static_cast<FContentNpcShip*>(*It);
        if (Ship->Field_0x5a8 != 0)
            continue;
        Vector Position;
        Ship->GetPosition(&Position);
        double DX = static_cast<double>(Placement->Position.x) - Position.x;
        double DY = static_cast<double>(Placement->Position.y) - Position.y;
        double DZ = static_cast<double>(Placement->Position.z) - Position.z;
        double DistanceSquare = DZ * DZ + DY * DY + DX * DX;
        if (!(DistanceSquare <= RadiusSquare))
        {
            if (!(*ConeRange > 0.0f))
                continue;
            Vector const V = SubtractVectors(Position, Placement->Position);
            float VX = V.x;
            float VY = V.y;
            float VZ = V.z;
            float VSquare = static_cast<float>(static_cast<double>(VZ) * VZ +
                                               static_cast<double>(VY) * VY +
                                               static_cast<double>(VX) * VX);
            if (!(FastReciprocalSqrt(VSquare) * VSquare <= *ConeRange))
                continue;
            float FX = -Placement->Orientation.E[0][2];
            float FY = -Placement->Orientation.E[1][2];
            float FZ = -Placement->Orientation.E[2][2];
            float FSquare = static_cast<float>(static_cast<double>(FZ) * FZ +
                                               static_cast<double>(FY) * FY +
                                               static_cast<double>(FX) * FX);
            float Dot = static_cast<float>(static_cast<double>(VZ) * FZ +
                                           static_cast<double>(VY) * FY +
                                           static_cast<double>(FX) * VX);
            float VLength = static_cast<float>(FastReciprocalSqrt(VSquare) * VSquare);
            double FLength = FastReciprocalSqrt(FSquare) * FSquare;
            double Cosine = Dot / (FLength * VLength);
            if (!(std::cos(0.6108652334660292) <= Cosine))
                continue;
        }
        if (ClassId != nullptr && !Ship->HasClassId(ClassId))
            continue;
        ++Count;
    }
    return Count;
}

// (06f6cb20-06f6cb5e). The docking target lands in the ObjectId argument slot
// (baseline reuses it as the out-parameter); pub::SpaceObj::GetDockingTarget zeroes it before
// any check (Server 06cf9889), so a failed call reads 0 -> false.
bool __cdecl IsGroupAtDockBase(unsigned int const* Group, unsigned int const* ObjectId)
{
    unsigned int BaseId = 0;
    pub::SpaceObj::GetDockingTarget(*ObjectId, BaseId);
    if (BaseId == 0)
        return false;
    FContentBase* Base = FindContentBaseInfo(&BaseId);
    return Base->FindFaction(*Group) != nullptr;
}

// (06f6cb90-06f6ce47). Two copies of one body that differ in the attitude:
//   - *ShipRep != -1: GetRep(target) and GetAttitude(target rep, *ShipRep) (06f6cbd7-06f6cbe8;
//     the inner "*ShipRep == -1" skip can never be taken there);
//   - *ShipRep == -1: 1.0 unless *Group != -1 and the target's affiliation (GetRep +
//     GetAffiliation) is not -1 -> the group/group GetReputation(affiliation, *Group).
// IsGateLockedForShip(*ShipId, *DockTargetId) first -> false; attitude not above -0.6
// (the field; TEST AH,0x41, JNZ) -> false. Then terminals 0, 1, ... (GetTerminalInfo
// failing -> false): types 1 / 2 / 6 need can_use_berths, 3 / 4 / 5 the moor classes (the
// class ids the field..06fd1ccc, std::find the routine or inline), 7 / 8 -> true at once,
// any other type fits nothing; the first fitting terminal -> true.
bool __cdecl CanUseDockTarget(std::vector<unsigned int> const* ClassIds, int const* ShipRep,
                              unsigned int const* Group, unsigned int const* ShipId,
                              unsigned int const* DockTargetId)
{
    if (IsGateLockedForShip(*ShipId, *DockTargetId))
        return false;

    // baseline keeps the out-values in argument slots it no longer needs, and GetRep's result is
    // not checked: a failed GetRep (it writes only on success, Server 06cf8bed) leaves the
    // slot's argument bits in place, and the next call reads them as the rep. GetAffiliation
    // always writes its out-value (-1 on failure, Server 06d5f400).
    float Attitude = 1.0f;
    if (*ShipRep != -1)
    {
        // the target rep lands in the ShipRep argument's slot.
        int TargetRep = static_cast<int>(reinterpret_cast<uintptr_t>(ShipRep));
        pub::SpaceObj::GetRep(*DockTargetId, TargetRep);
        pub::Reputation::GetAttitude(TargetRep, *ShipRep, Attitude);
    }
    else if (*Group != 0xffffffffu)
    {
        // the target rep lands in the Group argument's slot, 06f6cd0f: the
        // affiliation in the ShipId argument's slot.
        int TargetRep = static_cast<int>(reinterpret_cast<uintptr_t>(Group));
        pub::SpaceObj::GetRep(*DockTargetId, TargetRep);
        unsigned int Affiliation = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(ShipId));
        pub::Reputation::GetAffiliation(TargetRep, Affiliation);
        if (Affiliation != 0xffffffffu)
            pub::Reputation::GetReputation(Affiliation, *Group, Attitude);
    }
    if (!(Attitude > GDockHostileAttitude))
        return false;

    pub::SpaceObj::TerminalInfo Info;
    int Index = 0;
    if (pub::SpaceObj::GetTerminalInfo(*DockTargetId, Index, Info) != 0)
        return false;
    for (;;)
    {
        switch (Info.Value)
        {
        case 7:
        case 8:
            return true;
        default:
            if (DoesTerminalFitClasses(Info.Value, ClassIds))
                return true;
            break;
        }
        ++Index;
        if (pub::SpaceObj::GetTerminalInfo(*DockTargetId, Index, Info) != 0)
            return false;
    }
}

// (06f6bc20-06f6bdc6). For every pending state (in order), the first applied
// entry of the same target and index:
//   - found with a different flag: Activate(target, flag, index); 0 -> the applied flag
//     takes it, and a set flag queues the target;
//   - found with the same flag: nothing;
//   - none: Activate; 0 -> the state is appended (the routine), a set flag queues the target.
// Then GDockTerminals is emptied (the routine on the empty tail) and every queued target
// (a local vector<uint>, the routine) is announced: controller 0 gets 0x1007 with it.
void __cdecl ApplyDockTerminalStates()
{
    std::vector<unsigned int> OccupiedTargets;
    for (std::vector<FDockTerminal>::iterator Pending = GDockTerminals.begin();
         Pending != GDockTerminals.end(); ++Pending)
    {
        std::vector<FDockTerminal>::iterator Applied = GDockTerminalsApplied.begin();
        for (; Applied != GDockTerminalsApplied.end(); ++Applied)
        {
            if (Applied->DockTargetId == Pending->DockTargetId &&
                Applied->DockIndex == Pending->DockIndex)
                break;
        }

        if (Applied != GDockTerminalsApplied.end())
        {
            if (Applied->bOccupied == Pending->bOccupied)
                continue;
            if (pub::SpaceObj::Activate(Pending->DockTargetId, Pending->bOccupied,
                                        Pending->DockIndex) != 0)
                continue;
            Applied->bOccupied = Pending->bOccupied;
        }
        else
        {
            if (pub::SpaceObj::Activate(Pending->DockTargetId, Pending->bOccupied,
                                        Pending->DockIndex) != 0)
                continue;
            GDockTerminalsApplied.push_back(*Pending);
        }
        if (Pending->bOccupied)
            OccupiedTargets.push_back(Pending->DockTargetId);
    }
    GDockTerminals.clear();

    for (int Index = 0; Index < static_cast<int>(OccupiedTargets.size()); ++Index)
    {
        unsigned int const Controller = 0;
        unsigned int TargetId = OccupiedTargets[Index];
        pub::Controller::_SendMessage(Controller, pub::Controller::EMessage::JumpgateAct, &TargetId);
    }
}

// (ICF-shared `MOV EAX,2; RET`).
int FGcsDockGrantedEvent::GetType() const
{
    return 2;
}

// (`MOV EAX,3; RET`).
int FGcsLaunchEvent::GetType() const
{
    return 3;
}

// ---------------------------------------------------------------------------------------
// The dock launch queue (the field): the routine, the routine, the routine.
// ---------------------------------------------------------------------------------------

I3DMathEngine* GSpaceMathEngine = nullptr;

I3DMathEngine* GetSpaceMathEngine()
{
    if (GSpaceMathEngine == nullptr)
        GSpaceMathEngine = ContentAcquire3DMathEngine();
    return GSpaceMathEngine;
}

namespace
{
    FVector ToFVector(Vector const& Source)
    {
        FVector Out;
        Out.X = Source.x;
        Out.Y = Source.y;
        Out.Z = Source.z;
        return Out;
    }

    FMatrix ToFMatrix(Matrix const& Source)
    {
        FMatrix Out;
        std::memcpy(&Out, &Source, sizeof(Out));
        return Out;
    }

    // .rdata 06fbbb70: the ship id CheckDockPermission gets for a ship that does not exist
    // yet (0).
    unsigned int const GLaunchNoShipId = 0;

    // The directive insert of the routine (06f60892-06f60a9b, twice): the new directive is
    // put in front of the block's list (the routine at the routine's begin) and AddRef'ed
    // through a copy of the pointer (no null test). The creating reference belongs to the
    // caller's holder (FuncInfo 06fc4b28 states 0 / 1, TRefHandle), which releases it.
    void AddLeadingDirective(FContentNpcShipParams* Params, FObjListDirective* Directive)
    {
        Params->Directives.Items.push_front(Directive);
        Directive->AddRef();
    }

    // (06f603d0-06f60753, cdecl): where a ship launched from a terminal starts.
    //   - A docking ring (low byte of *TargetType & 0x20): Position = the hardpoint position
    //     plus (0, rand() * 1/32768 (the field) * 10000.0 (double the field) + 10000.0f
    //     (the field), 0) (the routine, each sum stored as float); nothing else.
    //   - Else Orientation = the hardpoint orientation; unless the terminal type is 3 / 4 / 5
    //     (moors) it is turned half a circle about its own column 1: quaternion (cos(pi/2),
    //     column 1 * (sin(pi/2) / |column 1|)) -- pi/2 is double the field, sin and the
    //     quotient stored as float, |.| the routine, the scaling the routine -- through
    //     QuaternionToMatrix (+0x54) and MulMatrix (+0x20) = rotation * orientation.
    //   - Then the ship arch's "HpMount" (pub::FindHardpoint, result not tested; baseline leaves
    //     the outputs as they were on failure, the port's start at zero): A = hardpoint +
    //     Orientation * mount (TransformMatrix +0x2c); MulMatrix(Orientation, mount
    //     orientation) is computed into a local that is never read (06f6061f); D = hardpoint -
    //     A; D' = transpose(Orientation) (the routine = Matrix(9 floats)) * D; Position =
    //     Orientation * D' + hardpoint. Every intermediate sum is stored as float.
    // |column 1| is summed on the x87 stack in extended precision; the port uses double.
    void PlaceLaunchedShip(FContentNpcShipParams* Params, unsigned int const* TargetType,
                           unsigned int TerminalType, Vector const* HardpointPosition,
                           Matrix const* HardpointOrientation)
    {
        if ((*TargetType & 0x20) != 0)
        {
            int Random = rand();
            Vector Offset;
            Offset.x = 0.0f;
            Offset.y = static_cast<float>(static_cast<double>(Random) *
                                              static_cast<double>(3.0517578125e-05f) * 10000.0 +
                                          static_cast<double>(10000.0f));
            Offset.z = 0.0f;
            Params->Position.x = HardpointPosition->x + Offset.x;
            Params->Position.y = HardpointPosition->y + Offset.y;
            Params->Position.z = HardpointPosition->z + Offset.z;
            return;
        }

        Params->Orientation = *HardpointOrientation;
        if (TerminalType != 3 && TerminalType != 4 && TerminalType != 5)
        {
            Vector Axis;
            Axis.x = Params->Orientation.E[0][1];
            Axis.y = Params->Orientation.E[1][1];
            Axis.z = Params->Orientation.E[2][1];
            float SinHalf = static_cast<float>(std::sin(1.5707963267948966));
            double Length = std::sqrt(static_cast<double>(Axis.x) * Axis.x +
                                      static_cast<double>(Axis.y) * Axis.y +
                                      static_cast<double>(Axis.z) * Axis.z);
            float Scale = static_cast<float>(SinHalf / Length);
            FQuaternion Quat;
            Quat.W = static_cast<float>(std::cos(1.5707963267948966));
            Quat.X = Scale * Axis.x;
            Quat.Y = Scale * Axis.y;
            Quat.Z = Scale * Axis.z;
            FMatrix Rotation;
            GetSpaceMathEngine()->QuaternionToMatrix(Rotation, Quat);
            FMatrix Current = ToFMatrix(Params->Orientation);
            FMatrix Product;
            GetSpaceMathEngine()->MulMatrix(Product, Rotation, Current);
            std::memcpy(&Params->Orientation, &Product, sizeof(Product));
        }

        Vector MountPosition;
        MountPosition.x = 0.0f;
        MountPosition.y = 0.0f;
        MountPosition.z = 0.0f;
        Matrix MountOrientation;
        std::memset(&MountOrientation, 0, sizeof(MountOrientation));
        pub::FindHardpoint("HpMount", Params->ShipArchId, MountPosition, MountOrientation);

        FMatrix Current = ToFMatrix(Params->Orientation);
        FVector Rotated;
        GetSpaceMathEngine()->TransformMatrix(Rotated, Current, ToFVector(MountPosition));
        Vector MountPoint;
        MountPoint.x = Rotated.X + HardpointPosition->x;
        MountPoint.y = Rotated.Y + HardpointPosition->y;
        MountPoint.z = Rotated.Z + HardpointPosition->z;

        FMatrix MountProduct;
        GetSpaceMathEngine()->MulMatrix(MountProduct, Current, ToFMatrix(MountOrientation));

        Vector Offset;
        Offset.x = HardpointPosition->x - MountPoint.x;
        Offset.y = HardpointPosition->y - MountPoint.y;
        Offset.z = HardpointPosition->z - MountPoint.z;

        Matrix Transposed(Params->Orientation.E[0][0], Params->Orientation.E[1][0],
                          Params->Orientation.E[2][0], Params->Orientation.E[0][1],
                          Params->Orientation.E[1][1], Params->Orientation.E[2][1],
                          Params->Orientation.E[0][2], Params->Orientation.E[1][2],
                          Params->Orientation.E[2][2]);
        FVector Local;
        GetSpaceMathEngine()->TransformMatrix(Local, ToFMatrix(Transposed), ToFVector(Offset));
        FVector Back;
        GetSpaceMathEngine()->TransformMatrix(Back, Current, Local);
        Params->Position.x = Back.X + HardpointPosition->x;
        Params->Position.y = Back.Y + HardpointPosition->y;
        Params->Position.z = Back.Z + HardpointPosition->z;
    }

    // The whole-queue erase of the routine: element by element from the front
    // (the routine at 06f60cac; the range erase the routine at 06f60945 / 06f60974 walks
    // the same way).
    void EraseLaunchQueue(std::list<FContentNpcShipParams>* Queue)
    {
        while (!Queue->empty())
            Queue->erase(Queue->begin());
    }
}

// (06f60340-06f603c6). The node is linked before the copy is built into it
// (the routine = the block's copy ctor).
bool __cdecl QueueDockLaunch(FContentNpcShipParams const* Params, unsigned int DockTarget,
                             FContentNpcShip** OutShip)
{
    GDockLaunchQueues[DockTarget].push_back(*Params);
    if (OutShip != nullptr)
        *OutShip = LaunchQueuedShips();
    else
        LaunchQueuedShips();
    return !GDockLaunchQueues[DockTarget].empty();
}

// (06f607e0-06f60d41). Per map node (dock target, the key's address is handed
// on) and per queued block:
//   - GetType of the target fails -> the whole queue is erased.
//   - index = block +0x110.
//   - A jump gate / hole / airlock (type & 0xc40): unless the block is a formation follower
//     (+0x48 != 0 and +0x4c != -1), a Delay directive (type 1) of 2.0 s leads its list.
//   - A base / docking ring (type & 0x600de0): a docking ring asks for terminal 1;
//     CheckDockPermission(+0x50 classes, the block's reputation, ship 0, no position,
//     target, &index) -- jump table 06f60d44:
//       0, 2: this block is erased;  5: the whole queue;
//       3: kept while +0x104 < 0.0f (the field; FCOMP, TEST AH,1, JNZ keeps), else the
//          whole queue;
//       4: TerminalInfo of the terminal and its hardpoint (results not tested), then as 1;
//       1 and above 5: PlaceLaunchedShip (the routine) with the terminal type, hardpoint
//          and orientation locals -- on these paths they still hold an earlier block's values
//          (function-scope locals; the port's start at zero). Unless +0x104 is -1.0f (integer
//          compare 0xbf800000) and the terminal type is 2..5, a Launch directive (type 0x15;
//          target, index, terminal type) leads the block's list.
//   - Any other type: straight to the creation.
// Creation (06f60a9e): +0x110 = index, +0x114 = target; new FContentNpcShip (0x5b0); the
// result becomes it when +0x104 >= 0.0f (TEST AH,1, JNZ skips); AddSpaceObj on the block's
// system; controller 0 gets 0x1010 with the ship; a gate type jumps it in (JumpIn); a base /
// ring type enters it in the docking list ({system, ship, target, index, +0x10 = 1}, ctor
// , inserted at the end by the routine) and launches it (pub::SpaceObj::Launch
// ..., 0) unless type & 0x100. The block is erased, and then baseline's loop increment runs on
// the iterator erase returned (06f60c8c: MOV EDI,[EBP]) -- the block after a created one is
// skipped this pass, and erasing the last one wraps to the queue's first block (the VC6
// list's head node links back to it).
// After the queue: empty -> its map node is erased (the routine), else the next node.
FContentNpcShip* __cdecl LaunchQueuedShips()
{
    FContentNpcShip* Result = nullptr;

    unsigned int TargetType = 0;
    int DockIndex = -1;
    pub::SpaceObj::TerminalInfo Info;
    std::memset(&Info, 0, sizeof(Info));
    Vector HardpointPosition;
    HardpointPosition.x = 0.0f;
    HardpointPosition.y = 0.0f;
    HardpointPosition.z = 0.0f;
    Matrix HardpointOrientation;
    std::memset(&HardpointOrientation, 0, sizeof(HardpointOrientation));

    std::map<unsigned int, std::list<FContentNpcShipParams>>::iterator QueueIt =
        GDockLaunchQueues.begin();
    while (QueueIt != GDockLaunchQueues.end())
    {
        unsigned int const* DockTargetId = &QueueIt->first;
        std::list<FContentNpcShipParams>& Queue = QueueIt->second;

        std::list<FContentNpcShipParams>::iterator It = Queue.begin();
        while (It != Queue.end())
        {
            if (pub::SpaceObj::GetType(*DockTargetId, TargetType) != 0)
            {
                EraseLaunchQueue(&Queue);
                break;
            }

            FContentNpcShipParams& Params = *It;
            DockIndex = Params.Field_0x110;

            if ((TargetType & 0xc40) != 0)
            {
                if (Params.Field_0x48 == 0 || Params.Field_0x4c == 0xffffffffu)
                {
                    TRefHandle<FObjListDirective> Directive;                  // EH state 0
                    CreateObjListDirective(1, &Directive.Pointer);
                    static_cast<FObjListDelay*>(Directive.Pointer)->SetDelay(2.0f);
                    AddLeadingDirective(&Params, Directive);
                }
            }
            else if ((TargetType & 0x600de0) != 0)
            {
                if ((TargetType & 0x20) != 0)
                    DockIndex = 1;
                int ShipRep = Params.ReputationHandle->GetReputation();
                int Response = CheckDockPermission(&Params.ClassIds, &ShipRep, &GLaunchNoShipId,
                                                   nullptr, DockTargetId, &DockIndex);
                if (Response == 0 || Response == 2)
                {
                    It = Queue.erase(It);                                     // (0)
                    continue;
                }
                if (Response == 5)
                {
                    EraseLaunchQueue(&Queue);
                    break;
                }
                if (Response == 3)
                {
                    if (Params.Field_0x104 >= 0.0f)
                    {
                        EraseLaunchQueue(&Queue);
                        break;
                    }
                    ++It;
                    continue;
                }
                if (Response == 4)
                {
                    pub::SpaceObj::GetTerminalInfo(*DockTargetId, DockIndex, Info);
                    pub::SpaceObj::GetHardpoint(*DockTargetId, Info.Name, &HardpointPosition,
                                                &HardpointOrientation);
                }

                PlaceLaunchedShip(&Params, &TargetType, Info.Value, &HardpointPosition,
                                  &HardpointOrientation);
                bool bOpenTerminal = !(Info.Value == 2 || Info.Value == 3 || Info.Value == 4 ||
                                       Info.Value == 5);
                if (Params.Field_0x104 != -1.0f || bOpenTerminal)
                {
                    TRefHandle<FObjListDirective> Directive;                  // EH state 1
                    CreateObjListDirective(0x15, &Directive.Pointer);
                    static_cast<FObjListLaunch*>(Directive.Pointer)->SetLaunch(
                        DockTargetId, DockIndex, static_cast<int>(Info.Value));
                    AddLeadingDirective(&Params, Directive);
                }
            }

            Params.Field_0x110 = DockIndex;
            Params.GuideTargetId = *DockTargetId;
            FContentNpcShip* Ship = new FContentNpcShip(&Params);
            if (Params.Field_0x104 >= 0.0f)
                Result = Ship;
            Params.System->AddSpaceObj(Ship);
            unsigned int const Controller = 0;
            FContentNpcShip* MsgShip = Ship;
            pub::Controller::_SendMessage(Controller, pub::Controller::EMessage::NpcShipCreated, &MsgShip);

            if ((TargetType & 0xc40) != 0)
            {
                unsigned int ShipId = Ship->SpaceObjId;
                pub::SpaceObj::JumpIn(ShipId, *DockTargetId);
            }
            else if ((TargetType & 0x600de0) != 0)
            {
                FDockQueueEntry Entry;
                Entry.SystemId = Params.System->SystemId;
                Entry.DockIndex = DockIndex;
                Entry.DockTargetId = *DockTargetId;
                Entry.ShipId = Ship->SpaceObjId;
                Entry.bField_0x10 = true;
                GDockingShips.push_back(Entry);

                if ((TargetType & 0x200100) != 0)
                {
                    // the dock hail handler (GetGcsHandler(&0),
                    // ) keeps quiet about this ship for 30 s (the routine with
                    // the phantom the field = 0, i.e. any).
                    int const HandlerType = 0;
                    void* const AnyPhantom = nullptr;
                    unsigned int ShipId = Ship->SpaceObjId;
                    static_cast<FGcsDockHailHandler*>(GetGcsHandler(&HandlerType))
                        ->SuppressHail(&ShipId, &AnyPhantom, 0x1e);
                }

                // the type 3 launch event to the field (the routine,
                // float result dropped). The info goes into the same local as the
                // permission path's (ESP+0x90), so a later block's case 1 sees it.
                FGcsLaunchEvent Event;
                Event.System = Params.System;
                pub::SpaceObj::GetTerminalInfo(Entry.DockTargetId, Entry.DockIndex, Info);
                Event.TerminalType = Info.Value;
                Event.Speaker = Entry.DockTargetId;
                Event.DockIndex = Entry.DockIndex;
                Event.Listener = Ship->SpaceObjId;
                GGcsConversations.QueueConversation(&Event);

                if ((TargetType & 0x100) == 0)
                {
                    unsigned int ShipId = Ship->SpaceObjId;
                    pub::SpaceObj::Launch(ShipId, *DockTargetId, 0);
                }
            }

            It = Queue.erase(It);
            if (It == Queue.end())
                It = Queue.begin();
            else
                ++It;
        }

        if (Queue.empty())
            QueueIt = GDockLaunchQueues.erase(QueueIt);
        else
            ++QueueIt;
    }
    return Result;
}

// (06f66c80-06f66db2). Pass 1 (06f66ca2-06f66ce8): slot counter from 0 over the
// list, the routine IsPlayerSlot -> the block is left alone, else +0x48 = slot, +0x44 =
// Formation, +0x4c = the new id (the routine); the counter advances for every block. Pass
// 2: the routine GetSlotTransform(+0x48, &pos, &orient) (result not tested; a rejected slot
// keeps the previous block's outputs, see below), then the engine the field:
// TransformXform (+0x28) of the slot position by *Placement -> +0xb4, MulMatrix (+0x20)
// (Placement's rotation, slot orientation) -> +0x90.
void __cdecl PlaceFormationBlocks(std::list<FContentNpcShipParams>* Blocks, FFormation* Formation,
                                  FTransform const* Placement)
{
    unsigned int FormationId = AllocateFormationId();
    int Slot = 0;
    for (std::list<FContentNpcShipParams>::iterator It = Blocks->begin(); It != Blocks->end();
         ++It)
    {
        if (!Formation->IsPlayerSlot(Slot))
        {
            It->Field_0x48 = static_cast<unsigned int>(Slot);
            It->Formation = Formation;
            It->Field_0x4c = FormationId;
        }
        ++Slot;
    }

    // the slot transform lands in two frame locals ([ESP+0x28] position,
    // [ESP+0x34] orientation) that only GetSlotTransform writes, so a block whose slot it
    // rejects (a player slot left at -1 by pass 1, or an index past the slots) gets the
    // previous block's transform. Declared once, outside the loop, for the same carry-over;
    // baseline's first values are indeterminate, the port's are zero.
    FVector SlotPosition;
    SlotPosition.X = 0.0f;
    SlotPosition.Y = 0.0f;
    SlotPosition.Z = 0.0f;
    FMatrix SlotOrientation;
    std::memset(&SlotOrientation, 0, sizeof(SlotOrientation));
    for (std::list<FContentNpcShipParams>::iterator It = Blocks->begin(); It != Blocks->end();
         ++It)
    {
        Formation->GetSlotTransform(It->Field_0x48, &SlotPosition, &SlotOrientation);

        FVector World;
        GetSpaceMathEngine()->TransformXform(World, *Placement, SlotPosition);
        It->Position.x = World.X;
        It->Position.y = World.Y;
        It->Position.z = World.Z;

        FMatrix Orientation;
        GetSpaceMathEngine()->MulMatrix(Orientation, Placement->Rotation, SlotOrientation);
        std::memcpy(&It->Orientation, &Orientation, sizeof(Orientation));
    }
}
