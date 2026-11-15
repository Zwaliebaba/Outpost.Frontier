// Author: Giperion.

// Content.dll TU46 (06f46940-06f4a780): per-player content state.

// TU46's own 16 static-ID initializers (the field..the field),
// before the TU's other dynamic initializers (the player-state map the field).
#include "ContentStaticIds.h"

#include "ContentPlayerState.h"
#include "BaseDB.h"
#include "ContentInternal.h"      // GetContentGameTime
#include "SystemInfo.h"
#include "common_id.h"
#include "common_newchar.h"       // pub::StateGraph::get_state_graph
#include "common_server.h"
#include "common_server_controller.h"

#include <algorithm>
#include <memory>

namespace
{
    // map<PlayerId, player state*> (node key +0xc, value +0x10). Built by
    // the dynamic initializer the branch (tree ctor the routine, atexit the routine).
    // The atexit dtor the routine only frees the nodes (_Erase the routine, then the head
    // and Nil): a state still in the map at unload is never deleted. Logout (GameSupport
    // msg 8) and the routine are the only places that delete a state.
    struct FContentPlayerStateMap : public std::map<unsigned int, std::unique_ptr<FContentPlayerState>>
    {
        ~FContentPlayerStateMap()
        {
            for (iterator It = begin(); It != end(); ++It)
                It->second.release();
        }
    };

    FContentPlayerStateMap GContentPlayerStates;

    // "RELAYSEVEN_system", re-read by the routine on every call.
    unsigned int GFp7SystemId = 0;

    // Inline find-then-insert-at-end of the routine / the routine
    // (the routine / the routine).
    void AppendUnique(std::vector<unsigned int>& Values, unsigned int Value)
    {
        for (int Index = 0; Index < static_cast<int>(Values.size()); ++Index)
        {
            if (Values[Index] == Value)
                return;
        }
        Values.push_back(Value);
    }
}

// (06eb6be0-06eb6bf6).
FContentPlayerStateBase::FContentPlayerStateBase()
    : Reputation(-1)
{
}

// (06eb6bb0-06eb6bd1): copy +8 to a local, GetAffiliation(local, out),
// return out. baseline does not initialise out; the Server export always writes it
// (0xffffffff on failure).
unsigned int FContentPlayerStateBase::GetAffiliation() const
{
    int Handle = Reputation;
    unsigned int Affiliation;
    pub::Reputation::GetAffiliation(Handle, Affiliation);
    return Affiliation;
}

// (06f33f80-06f33f83): MOV EAX,[ECX+8]; RET.
int FContentPlayerStateBase::GetReputation() const
{
    return Reputation;
}

// (06f479a0-06f479a7): DEC [this+4]; return it.
unsigned int FContentPlayerState::Release()
{
    --ReferenceCount;
    return ReferenceCount;
}

// (06f46b90-06f46d2f): zero/empty members, +0x21 and +0x34 = 1,
// +0xd4 = -1.0f (the field), +0xe0 = 1, then pub::Player::GetRep into +8.
FContentPlayerState::FContentPlayerState(unsigned int InPlayerId)
    : PlayerId(InPlayerId)
    , bCanDock(1)
    , bCanTradelane(1)
    , ValueD0(0)
    , ValueD4(-1.0f)
    , ValueD8(0)
    , MissionNumber(0)
    , NextVendorOfferId(1)
    , ValueE4(0)
    , ValueE8(0)
{
    pub::Player::GetRep(PlayerId, Reputation);
}

// (06f471d0-06f472b6).
FContentPlayerShip::FContentPlayerShip(FContentPlayerShipParams const* Params)
    : FContentShipBase(Params)
{
    SpaceObjId = Params->ShipId;

    // SetPersonalityParams with only the state graph set.
    pub::AI::SetPersonalityParams Op;
    Op.bSetPersonality = 0;
    Op.StateGraphId = pub::StateGraph::get_state_graph("PLAYER", pub::StateGraph::LEADER);
    Op.ContentCallbackObject = nullptr;
    Op.DirectiveCallbackObject = nullptr;
    pub::AI::SubmitState(SpaceObjId, &Op);
}

// (06f472e0-06f47425): the +0x180 list, then the TU50 members and the TU20
// base (the routine) -- member and base dtors in the port.
FContentPlayerShip::~FContentPlayerShip()
{
}

// (06f47430-06f47580).
void FContentPlayerShip::OnTargetChanged(unsigned int const* NewTargetId)
{
    TargetId = *NewTargetId;
    unsigned int Type;
    pub::SpaceObj::GetType(*NewTargetId, Type);

    // the player whose ship object carries this ship's id.
    FContentPlayerState* Owner = FindContentPlayerStateByShip(&SpaceObjId);
    if (Owner == nullptr || (Type & 0x80000) == 0)
        return;

    // the hole and its jump target, each once, into holes_visited.
    std::vector<unsigned int>& Holes = Owner->Stats.HolesVisited;
    unsigned int Hole = *NewTargetId;
    if (std::find(Holes.begin(), Holes.end(), Hole) == Holes.end())
        Holes.insert(Holes.end(), Hole);
    unsigned int JumpSystem = 0;
    unsigned int JumpObject = 0;
    pub::SpaceObj::GetJumpTarget(*NewTargetId, JumpSystem, JumpObject);
    if (std::find(Holes.begin(), Holes.end(), JumpObject) == Holes.end())
        Holes.insert(Holes.end(), JumpObject);
}

// (06f47880-06f478a3).
void FContentPlayerState::SetShipObject(FContentPlayerShip* NewShip)
{
    if (ShipObject != nullptr)
        delete ShipObject;
    ShipObject = NewShip;
}

// .
FSystemInfo* FContentPlayerState::GetSystemRecord() const
{
    return SystemRecord;
}

// (06f47980-06f47983).
FContentBase* FContentPlayerState::GetBaseRecord() const
{
    return BaseRecord;
}

// (06f478b0-06f478bb).
unsigned int FContentPlayerState::GetSystemId() const
{
    if (SystemRecord == nullptr)
        return 0;
    return SystemRecord->SystemId;
}

// (06f481e0-06f481ed).
unsigned int FContentPlayerState::GetShipId() const
{
    if (ShipObject == nullptr)
        return 0;
    return ShipObject->SpaceObjId;
}

// (06f47af0-06f47b85, RET 4). The local starts at 0; with a ship it becomes
// the ship's position (the routine), else GetBasePosition(&system id, &base id, &local)
// fills it (its result is ignored).
Vector* FContentPlayerState::GetPosition(Vector* Out) const
{
    Vector Position;
    Position.x = 0.0f;
    Position.y = 0.0f;
    Position.z = 0.0f;
    if (ShipObject != nullptr)
    {
        ShipObject->GetPosition(&Position);
    }
    else
    {
        unsigned int const BaseId = BaseRecord != nullptr ? BaseRecord->BaseId : 0;
        unsigned int const SystemId = SystemRecord != nullptr ? SystemRecord->SystemId : 0;
        pub::GF::GetBasePosition(SystemId, BaseId, Position);
    }
    *Out = Position;
    return Out;
}

// (06f478d0-06f47971).
void FContentPlayerState::OnSystemChange(unsigned int SystemId)
{
    SystemRecord = FindSystemInfo(&SystemId);
    if (ShipObject != nullptr)
    {
        ShipObject->SetSystem(SystemRecord);
    }

    if ((SystemId & 0x3fffffffu) == 0)
        return;

    pub::GetSystemID(GFp7SystemId, "RELAYSEVEN_system");
    if (SystemId == GFp7SystemId)
        return;

    AppendUnique(Stats.SystemsVisited, SystemId);
}

// (06f479b0-06f47a2d).
void FContentPlayerState::OnBaseChange(unsigned int BaseId)
{
    BaseRecord = FindContentBaseInfo(&BaseId);
    if ((BaseId & 0x3fffffffu) == 0)
        return;

    AppendUnique(Stats.BasesVisited, BaseId);
}

// .
void FContentPlayerState::SetLocation(unsigned int InLocationId)
{
    LocationId = InLocationId;
}

// (06f49030-06f49082).
void FContentPlayerState::UpdateTime(float DeltaTime)
{
    Stats.TotalTimePlayed = DeltaTime + Stats.TotalTimePlayed;

    // FCOMP -1.0f; TEST AH,0x44; JNP -> return only when equal (NaN goes on).
    if (ValueD4 == -1.0f)
        return;

    // FST keeps the difference; FCOMP 0.0; TEST AH,0x41; JP -> return when
    // greater (or unordered). Less or equal resets the countdown and bit 2.
    ValueD4 = ValueD4 - DeltaTime;
    if (ValueD4 <= 0.0f)
    {
        ValueD4 = -1.0f;
        ValueD0 &= 0xfffffffbu;
    }
}

// (06f481f0-06f483cb).
void FContentPlayerState::Save(FILE* File)
{
    std::vector<unsigned int> Gates;
    CollectContentGateLocks(PlayerId, Gates);

    fprintf(File, "[%s]\n", "mPlayer");
    for (int Index = 0; Index < static_cast<int>(Gates.size()); ++Index)
        fprintf(File, "%s = %u\n", "locked_gate", Gates[Index]);

    for (int Index = 0; Index < static_cast<int>(Vnpcs.size()); ++Index)
    {
        FVnpcRecord const& Record = Vnpcs[Index];
        // baseline passes the uninitialized 0x40 stack buffer on a lookup miss too.
        char BaseNickname[0x40];
        pub::GetBaseNickname(BaseNickname, 0x40, Record.Values[0]);
        ID_String VnpcId;
        VnpcId.Id = Record.Values[1];
        fprintf(File, "%s = %u, %u, %u, %u ; %s, %s\n", "vnpc", Record.Values[0],
                Record.Values[1], Record.Values[2], Record.Values[3], BaseNickname,
                VnpcId.get_string());
    }

    for (int Index = 0; Index < static_cast<int>(Rumors.size()); ++Index)
        fprintf(File, "%s = %u, %u\n", "rumor", Rumors[Index].Values[0], Rumors[Index].Values[1]);

    fprintf(File, "%s = %d\n", "can_dock", bCanDock != 0 ? 1 : 0);
    for (int Index = 0; Index < static_cast<int>(DockExceptions.size()); ++Index)
        fprintf(File, "%s = %u\n", "dock_exception", DockExceptions[Index]);

    fprintf(File, "%s = %d\n", "can_tl", bCanTradelane != 0 ? 1 : 0);
    for (int Index = 0; Index < static_cast<int>(TradelaneExceptions.size()); ++Index)
    {
        fprintf(File, "%s = %u, %u\n", "tlr_exception", TradelaneExceptions[Index].Values[0],
                TradelaneExceptions[Index].Values[1]);
    }

    SaveContentPlayerStats(Stats, File);
}

namespace
{
    // / the routine / the routine body: map::find, value or 0.
    int FindMissionCount(std::map<unsigned char, int> const& Counts, unsigned char MissionType)
    {
        std::map<unsigned char, int>::const_iterator Entry = Counts.find(MissionType);
        if (Entry == Counts.end())
            return 0;
        return Entry->second;
    }
}

// (06f49210-06f49261): player +0x5c map.
int FContentPlayerState::GetRandomMissionsCompleted(unsigned char MissionType) const
{
    return FindMissionCount(Stats.RandomMissionsCompleted, MissionType);
}

// (06f49270-06f492c1): player +0x70 map.
int FContentPlayerState::GetRandomMissionsAborted(unsigned char MissionType) const
{
    return FindMissionCount(Stats.RandomMissionsAborted, MissionType);
}

// (06f492d0-06f49327): player +0x84 map.
int FContentPlayerState::GetRandomMissionsFailed(unsigned char MissionType) const
{
    return FindMissionCount(Stats.RandomMissionsFailed, MissionType);
}

// (end - begin) / 4 of the vector at +0xb0, 0 when begin is null.
int FContentPlayerState::GetBasesVisitedCount() const
{
    return static_cast<int>(Stats.BasesVisited.size());
}

// vector at +0xc0.
int FContentPlayerState::GetHolesVisitedCount() const
{
    return static_cast<int>(Stats.HolesVisited.size());
}

// vector at +0xa0.
int FContentPlayerState::GetSystemsVisitedCount() const
{
    return static_cast<int>(Stats.SystemsVisited.size());
}

// FLD float +0x98; JMP ftol.
int FContentPlayerState::GetTotalCashEarned() const
{
    return static_cast<int>(Stats.TotalCashEarned);
}

// FLD float +0x9c; JMP ftol.
int FContentPlayerState::GetTotalTimePlayed() const
{
    return static_cast<int>(Stats.TotalTimePlayed);
}

// size of the map at +0x48.
unsigned int FContentPlayerState::GetShipTypesKilledCount() const
{
    return static_cast<unsigned int>(Stats.ShipTypesKilled.size());
}

// (06f493c0-06f49434): in-order walk, {key, value} per entry.
void FContentPlayerState::CopyShipTypesKilled(unsigned int* Out) const
{
    for (std::map<unsigned int, int>::const_iterator It = Stats.ShipTypesKilled.begin();
         It != Stats.ShipTypesKilled.end(); ++It)
    {
        Out[0] = It->first;
        Out[1] = static_cast<unsigned int>(It->second);
        Out += 2;
    }
}

// (06f48d80-06f48da7): linear scan of the vector at +0x100..+0x104.
unsigned int FContentPlayerState::FindRumor(unsigned int Id) const
{
    for (std::vector<FRumorRecord>::const_iterator It = Rumors.begin(); It != Rumors.end(); ++It)
    {
        if (It->Values[0] == Id)
            return It->Values[1];
    }
    return 0;
}

// (06f48db0-06f48fa5, RET 4). The not-found path is the inlined VC6
// vector insert of one {Id, 1} at end().
void FContentPlayerState::AddRumor(unsigned int Id)
{
    for (std::vector<FRumorRecord>::iterator It = Rumors.begin(); It != Rumors.end(); ++It)
    {
        if (It->Values[0] == Id)
        {
            ++It->Values[1];
            return;
        }
    }
    FRumorRecord Record;
    Record.Values[0] = Id;
    Record.Values[1] = 1;
    Rumors.push_back(Record);
}

// (06f48ca0-06f48d7c, RET 8). Both searches compare Values[0] and Values[1]
// (the first through the routine), so they find the same record.
void FContentPlayerState::RecordVnpcTalk(unsigned int BaseId, unsigned int NpcId)
{
    if (NpcId == 0)
        return;
    std::vector<FVnpcRecord>::iterator It = Vnpcs.begin();
    for (; It != Vnpcs.end(); ++It)
    {
        if (It->Values[0] == BaseId && It->Values[1] == NpcId)
            break;
    }
    if (It == Vnpcs.end() || It->Values[2] == 0)
    {
        // the routine (insert one at end).
        FVnpcRecord Record;
        Record.Values[0] = BaseId;
        Record.Values[1] = NpcId;
        Record.Values[2] = 1;
        Record.Values[3] = 0;
        Vnpcs.push_back(Record);
        return;
    }
    if (It->Values[3] != 1)
        It->Values[3] = 0;
    ++It->Values[2];
}

// (06f48c50-06f48c5f): MOV EAX,[ECX+0xe0]; LEA EDX,[EAX+1]; MOV [ECX+0xe0],EDX.
unsigned int FContentPlayerState::AllocateVendorOfferId()
{
    unsigned int const Id = NextVendorOfferId;
    NextVendorOfferId = Id + 1;
    return Id;
}

// (06f48070-06f480bb). The cash local is not initialised in baseline
// (pub::Player::InspectCash writes it first on every path); the asset value starts at 0.0.
// FILD cash, FADD float asset value: the sum stays on the FPU (double).
double FContentPlayerState::GetWorth() const
{
    int Cash = 0;
    pub::Player::InspectCash(PlayerId, Cash);
    float AssetValue = 0.0f;
    pub::Player::GetAssetValue(PlayerId, AssetValue);
    return static_cast<double>(Cash) + static_cast<double>(AssetValue);
}

// (06f487d0-06f488ab). The members are looked up in the player-state map
// (the field, the routine find); a missing entry or a null state is skipped.
unsigned int FContentPlayerState::GetGroupLiveMission() const
{
    if (ValueD8 != 0)
        return ValueD8;
    std::vector<unsigned int> Members;
    pub::Player::GetGroupMembers(PlayerId, Members);
    for (std::vector<unsigned int>::const_iterator It = Members.begin(); It != Members.end(); ++It)
    {
        FContentPlayerState* Member = FindContentPlayerState(*It);
        if (Member != nullptr && Member->ValueD8 != 0)
            return Member->ValueD8;
    }
    return 0;
}

// (06f495f0-06f49777).
void SaveContentPlayerStats(FContentPlayerStats const& Stats, FILE* File)
{
    for (std::map<unsigned int, int>::const_iterator It = Stats.ShipTypesKilled.begin();
         It != Stats.ShipTypesKilled.end(); ++It)
        fprintf(File, "ship_type_killed = %u, %d\n", It->first, It->second);

    for (std::map<unsigned char, int>::const_iterator It = Stats.RandomMissionsCompleted.begin();
         It != Stats.RandomMissionsCompleted.end(); ++It)
        fprintf(File, "rm_completed = %u, %d\n", static_cast<unsigned int>(It->first), It->second);

    for (std::map<unsigned char, int>::const_iterator It = Stats.RandomMissionsAborted.begin();
         It != Stats.RandomMissionsAborted.end(); ++It)
        fprintf(File, "rm_aborted = %u, %d\n", static_cast<unsigned int>(It->first), It->second);

    for (std::map<unsigned char, int>::const_iterator It = Stats.RandomMissionsFailed.begin();
         It != Stats.RandomMissionsFailed.end(); ++It)
        fprintf(File, "rm_failed = %u, %d\n", static_cast<unsigned int>(It->first), It->second);

    fprintf(File, "total_cash_earned = %f\n", static_cast<double>(Stats.TotalCashEarned));
    fprintf(File, "total_time_played = %f\n", static_cast<double>(Stats.TotalTimePlayed));

    for (int Index = 0; Index < static_cast<int>(Stats.SystemsVisited.size()); ++Index)
        fprintf(File, "sys_visited = %u\n", Stats.SystemsVisited[Index]);
    for (int Index = 0; Index < static_cast<int>(Stats.BasesVisited.size()); ++Index)
        fprintf(File, "base_visited = %u\n", Stats.BasesVisited[Index]);
    for (int Index = 0; Index < static_cast<int>(Stats.HolesVisited.size()); ++Index)
        fprintf(File, "holes_visited = %u\n", Stats.HolesVisited[Index]);
}

// (06f49b00-06f49b43).
FContentPlayerState* FindContentPlayerState(unsigned int PlayerId)
{
    std::map<unsigned int, std::unique_ptr<FContentPlayerState>>::iterator Entry =
        GContentPlayerStates.lower_bound(PlayerId);
    if (Entry == GContentPlayerStates.end() || PlayerId < Entry->first)
        return nullptr;
    return Entry->second.get();
}

// (06f47ba0-06f47d06, RET 8): +0x21 = bCan; the exception vector emptied, then
// assigned from *Exceptions when that is non-null, non-empty and not the same vector.
void FContentPlayerState::SetDockAccess(unsigned char bCan, std::vector<unsigned int> const* Exceptions)
{
    bCanDock = bCan;
    DockExceptions.clear();
    if (Exceptions == nullptr || Exceptions->empty() || Exceptions == &DockExceptions)
        return;
    DockExceptions = *Exceptions;
}

// (06f47d60-06f47ed1, RET 8): the same on +0x34 / +0x38 (8-byte elements).
void FContentPlayerState::SetTradelaneAccess(unsigned char bCan,
                                             std::vector<FTlrException> const* Exceptions)
{
    bCanTradelane = bCan;
    TradelaneExceptions.clear();
    if (Exceptions == nullptr || Exceptions->empty() || Exceptions == &TradelaneExceptions)
        return;
    TradelaneExceptions = *Exceptions;
}

// (06f47ee0-06f4800f, RET 8; added by CNT-MS-OBJ12 for TU55). The default is
// !can_tl. With a system record and a non-empty exception list, the lane of RingA is read
// (the routine into a local vector - EH state 0 - result not tested; an empty lane returns at
// once). For every ring of that lane and every exception pair whose first ring is both that
// ring and RingA: RingB being either end of the pair, or a ring of the lane reached while the
// toggle (flipped at both pair ends, in lane order) is set, gives can_tl ? 1 : 0; a full scan
// without such a hit gives 2. The last pair processed decides.
int FContentPlayerState::CheckTradelaneRoute(unsigned int RingA, unsigned int RingB)
{
    int Result = bCanTradelane == 0 ? 1 : 0;
    if (SystemRecord == nullptr || TradelaneExceptions.empty())
        return Result;

    std::vector<FTradeLaneRing> Rings;
    SystemRecord->GetTradeLaneRingsByRing(&RingA, &Rings);
    for (std::vector<FTradeLaneRing>::const_iterator Lane = Rings.begin(); Lane != Rings.end(); ++Lane)
    {
        for (std::vector<FTlrException>::const_iterator Pair = TradelaneExceptions.begin();
             Pair != TradelaneExceptions.end(); ++Pair)
        {
            if (Lane->ObjectId != Pair->Values[0] || RingA != Pair->Values[0])
                continue;
            if (RingB == Pair->Values[0] || RingB == Pair->Values[1])
            {
                Result = bCanTradelane != 0 ? 1 : 0;
                continue;
            }
            // the scan over the whole lane.
            bool bInside = false;
            bool bHit = false;
            for (std::vector<FTradeLaneRing>::const_iterator Ring = Rings.begin(); Ring != Rings.end(); ++Ring)
            {
                if (Ring->ObjectId == Pair->Values[0] || Ring->ObjectId == Pair->Values[1])
                    bInside = !bInside;
                if (Ring->ObjectId == RingB && bInside)
                {
                    bHit = true;
                    break;
                }
            }
            Result = bHit ? (bCanTradelane != 0 ? 1 : 0) : 2;
        }
    }
    return Result;
}

// (06f49c30-06f49c42): the first state in key order (head->left), or null.
FContentPlayerState* GetFirstContentPlayerState()
{
    if (GContentPlayerStates.empty())
        return nullptr;
    return GContentPlayerStates.begin()->second.get();
}

// (06f49bc0-06f49c25): in-order walk, first ship-id match.
FContentPlayerState* FindContentPlayerStateByShip(unsigned int const* ShipId)
{
    for (std::map<unsigned int, std::unique_ptr<FContentPlayerState>>::iterator It =
             GContentPlayerStates.begin();
         It != GContentPlayerStates.end(); ++It)
    {
        FContentPlayerShip* Ship = It->second->ShipObject;
        if (Ship != nullptr && Ship->SpaceObjId == *ShipId)
            return It->second.get();
    }
    return nullptr;
}

// (06f49b50-06f49bb1): in-order walk of the field, the first value whose
// +0xd8 equals *ControllerId (no null check on the value).
FContentPlayerState* FindContentPlayerStateByLiveMission(unsigned int const* ControllerId)
{
    for (std::map<unsigned int, std::unique_ptr<FContentPlayerState>>::iterator It =
             GContentPlayerStates.begin();
         It != GContentPlayerStates.end(); ++It)
    {
        if (It->second->ValueD8 == *ControllerId)
            return It->second.get();
    }
    return nullptr;
}

std::map<unsigned int, std::unique_ptr<FContentPlayerState>>& GetContentPlayerStates()
{
    return GContentPlayerStates;
}

// (06f47d10-06f47d59, RET 4): with can_dock (+0x21) the answer is true unless
// the target is in the exception list (+0x28..+0x2c); without it, false unless it is.
bool FContentPlayerState::CanDockAt(unsigned int DockTargetId) const
{
    bool bListed = false;
    for (int Index = 0; Index < static_cast<int>(DockExceptions.size()); ++Index)
    {
        if (DockExceptions[Index] == DockTargetId)
        {
            bListed = true;
            break;
        }
    }
    if (bCanDock != 0)
        return !bListed;
    return bListed;
}

// (06f002e0-06f003d6) lookup/erase/new/insert part.
FContentPlayerState* ReplaceContentPlayerState(unsigned int PlayerId)
{
    // lower_bound + exact key; on a hit the old state goes first
    // (06f00336-06f00341, null-tested vtbl+8(1)) and the node is unlinked after (the routine).
    std::map<unsigned int, std::unique_ptr<FContentPlayerState>>::iterator Entry =
        GContentPlayerStates.lower_bound(PlayerId);
    if (Entry != GContentPlayerStates.end() && !(PlayerId < Entry->first))
    {
        Entry->second.reset();
        GContentPlayerStates.erase(Entry);
    }
    // new 0x10c + the routine (EH state 0 frees the block when the
    // ctor throws), insert the routine; the caller gets the inserted node's value.
    std::unique_ptr<FContentPlayerState> State(new FContentPlayerState(PlayerId));
    std::pair<std::map<unsigned int, std::unique_ptr<FContentPlayerState>>::iterator, bool> Inserted =
        GContentPlayerStates.emplace(PlayerId, std::move(State));
    return Inserted.first->second.get();
}

// GameSupport message 8 (06effb82-06effe6x): the routine lower_bound + exact key
// check; on a hit the state is deleted through vtbl[+8](1) and the node erased
// (inlined tree erase with the routine / the routine rotations).
void RemoveContentPlayerState(unsigned int PlayerId)
{
    std::map<unsigned int, std::unique_ptr<FContentPlayerState>>::iterator Entry =
        GContentPlayerStates.lower_bound(PlayerId);
    if (Entry == GContentPlayerStates.end() || PlayerId < Entry->first)
        return;
    // the state goes first (null-tested), the node is unlinked after.
    Entry->second.reset();
    GContentPlayerStates.erase(Entry);
}

// (06f48030-06f4804b, RET 4): the result of AdjustCash is dropped.
void FContentPlayerState::AdjustCash(int Amount)
{
    unsigned int const Id = PlayerId;
    pub::Player::AdjustCash(Id, Amount);
}

// (06f48050-06f48062, RET 4).
void FContentPlayerState::RewardGroup(int Amount)
{
    pub::Player::RewardGroup(PlayerId, Amount);
}

// (06f480c0-06f480df): the size local starts at 1; GetGroupSize's result code is
// ignored.
unsigned int FContentPlayerState::GetGroupSize() const
{
    unsigned int Size = 1;
    pub::Player::GetGroupSize(PlayerId, Size);
    return Size;
}

// MOV EAX,[ECX+0xd8].
unsigned int FContentPlayerState::GetLiveMission() const
{
    return ValueD8;
}

namespace
{
    // 's leave-mission step for one state (06f48aeb-06f48b10 per group member,
    // for the player alone): +0xd8 = 0, message 0x100b {0, player id} to
    // controller 0 (the compiler reuses a zeroed parameter slot as the const uint& id), then
    // pub::Player::SetMsnID(player, +0xd8, player, 0, 0).
    void LeaveLiveMission(FContentPlayerState* State)
    {
        State->ValueD8 = 0;
        unsigned int const Message[2] = {0, State->PlayerId};
        unsigned int const NoController = 0;
        pub::Controller::_SendMessage(NoController, pub::Controller::EMessage::PlayerMissionChanged, Message);
        pub::Player::SetMsnID(State->PlayerId, State->ValueD8, State->PlayerId, false, 0);
    }
}

// (06f48a20-06f48b93, RET 0xc). The first argument is never read.
void FContentPlayerState::EndLiveMission(unsigned int Unused, int VnpcState, bool bGroup)
{
    (void)Unused;
    // records (0x10) of +0xec whose +0xc is 1.
    for (int Index = 0; Index < static_cast<int>(Vnpcs.size()); ++Index)
    {
        if (Vnpcs[Index].Values[3] == 1)
            Vnpcs[Index].Values[3] = static_cast<unsigned int>(VnpcState);
    }
    if (!bGroup)
    {
        LeaveLiveMission(this);
        return;
    }
    // every group member with an entry in the field (the routine find,
    // end or null value skipped), in member order.
    std::vector<unsigned int> Members;
    pub::Player::GetGroupMembers(PlayerId, Members);
    for (int Index = 0; Index < static_cast<int>(Members.size()); ++Index)
    {
        FContentPlayerState* Member = FindContentPlayerState(Members[Index]);
        if (Member == nullptr)
            continue;
        LeaveLiveMission(Member);
    }
}

// GameSupport message 0x20 (06efec8a-06efecd3): the routine on every state in key order.
void UpdateAllContentPlayerTimes(float DeltaTime)
{
    for (std::map<unsigned int, std::unique_ptr<FContentPlayerState>>::iterator It =
             GContentPlayerStates.begin();
         It != GContentPlayerStates.end(); ++It)
        It->second->UpdateTime(DeltaTime);
}

// ---- Random-mission support (added by CNT-RM8 for the TU47 mission object) ----

// (06f471a0-06f471a6).
unsigned int FContentPlayerState::GetFlags() const
{
    return ValueD0;
}

// (06f471b0-06f471ba, RET 4).
void FContentPlayerState::SetFlags(unsigned int Flags)
{
    ValueD0 = Flags;
}

// (06f471c0-06f471ca, RET 4).
void FContentPlayerState::SetCountdown(float Countdown)
{
    ValueD4 = Countdown;
}

// (06f480e0-06f4818d, RET 4): the name goes into a local first (GetName's result
// code is ignored), then *Out is built from it.
std::wstring* FContentPlayerState::GetName(std::wstring* Out) const
{
    std::wstring Name;
    pub::Player::GetName(PlayerId, Name);
    *Out = Name;
    return Out;
}

// (06f48190-06f481bc, RET 4): the local answer starts false; the call's result
// code is ignored.
bool FContentPlayerState::IsGroupMember(unsigned int OtherPlayerId) const
{
    bool bMember = false;
    if (PlayerId == OtherPlayerId)
        return true;
    pub::Player::IsGroupMember(PlayerId, OtherPlayerId, bMember);
    return bMember;
}

// (06f481c0-06f481d2, RET 4).
void FContentPlayerState::GetGroupMembers(std::vector<unsigned int>* Out) const
{
    pub::Player::GetGroupMembers(PlayerId, *Out);
}

// (06f48ba0-06f48c0a, RET 0x10). The message goes to controller 0 (the zeroed
// ControllerId argument slot is reused as the const uint& target).
void FContentPlayerState::JoinMission(unsigned int const* ControllerId, unsigned int SubId,
                                      bool bNotify, unsigned int NotifyArg)
{
    ValueD8 = *ControllerId;
    unsigned int const Message[2] = {*ControllerId, PlayerId};
    unsigned int const NoController = 0;
    pub::Controller::_SendMessage(NoController, pub::Controller::EMessage::PlayerMissionChanged, Message);
    if (SubId == 0)
        SubId = PlayerId;
    pub::Player::SetMsnID(PlayerId, ValueD8, SubId, bNotify, NotifyArg);
}

// (06f49090-06f49100, RET 4): find (the routine); absent -> insert {type, 1}
// (the routine), else ++count.
void FContentPlayerState::CountRandomMissionCompleted(unsigned char MissionType)
{
    std::map<unsigned char, int>::iterator Entry = Stats.RandomMissionsCompleted.find(MissionType);
    if (Entry == Stats.RandomMissionsCompleted.end())
        Stats.RandomMissionsCompleted.insert(std::pair<unsigned char const, int>(MissionType, 1));
    else
        ++Entry->second;
}

// (06f49110-06f49180, RET 4): the same on +0x70.
void FContentPlayerState::CountRandomMissionAborted(unsigned char MissionType)
{
    std::map<unsigned char, int>::iterator Entry = Stats.RandomMissionsAborted.find(MissionType);
    if (Entry == Stats.RandomMissionsAborted.end())
        Stats.RandomMissionsAborted.insert(std::pair<unsigned char const, int>(MissionType, 1));
    else
        ++Entry->second;
}

// (06f49190-06f49206, RET 4): the same on +0x84.
void FContentPlayerState::CountRandomMissionFailed(unsigned char MissionType)
{
    std::map<unsigned char, int>::iterator Entry = Stats.RandomMissionsFailed.find(MissionType);
    if (Entry == Stats.RandomMissionsFailed.end())
        Stats.RandomMissionsFailed.insert(std::pair<unsigned char const, int>(MissionType, 1));
    else
        ++Entry->second;
}

// (06f488b0-06f48a12, RET 0x14). The vnpc search (+0xf0..+0xf4, 0x10 stride)
// stops at the first {ControllerId, Vnpc->Id} and stores 1 at its +0xc; not finding one it
// stores 1 at +0xc of the end pointer (past the last record) -- the port writes only a found
// record. Group members (pub::Player::GetGroupMembers into a local vector) are looked up in
// (the routine find); a found non-null state: +0xd8 = ControllerId, message
// 0x100b {ControllerId, member id} to controller 0 (the Unused slot zeroed), SetMsnID(member,
// +0xd8, this player's id or else the member's, bNotify, NotifyArg).
void FContentPlayerState::JoinMissionAsGroup(unsigned int ControllerId, unsigned int VnpcOwner,
                                             ID_String const* Vnpc, bool bNotify,
                                             unsigned int NotifyArg)
{
    // param 2 (EDI) is only the vnpc key. baseline writes the flag at the
    // found record or, when none matches, at end() (past the vector); the port writes only a
    // found record.
    if (VnpcOwner != 0 && !Vnpc->IsEmpty())
    {
        for (std::vector<FVnpcRecord>::iterator It = Vnpcs.begin(); It != Vnpcs.end(); ++It)
        {
            if (It->Values[0] == VnpcOwner && It->Values[1] == Vnpc->Id)
            {
                It->Values[3] = 1;
                break;
            }
        }
    }

    std::vector<unsigned int> Members;
    pub::Player::GetGroupMembers(PlayerId, Members);
    for (std::vector<unsigned int>::iterator It = Members.begin(); It != Members.end(); ++It)
    {
        std::map<unsigned int, std::unique_ptr<FContentPlayerState>>::iterator Entry =
            GContentPlayerStates.find(*It);
        if (Entry == GContentPlayerStates.end() || Entry->second == nullptr)
            continue;
        FContentPlayerState* Member = Entry->second.get();
        Member->ValueD8 = ControllerId;
        unsigned int const Message[2] = {ControllerId, Member->PlayerId};
        unsigned int const NoController = 0;
        pub::Controller::_SendMessage(NoController, pub::Controller::EMessage::PlayerMissionChanged, Message);
        unsigned int SubId = PlayerId;
        if (SubId == 0)
            SubId = Member->PlayerId;
        pub::Player::SetMsnID(Member->PlayerId, Member->ValueD8, SubId, bNotify, NotifyArg);
    }
}

// (06f48770-06f487b6, RET 0xc).
bool FContentPlayerState::FindVnpcValue3(unsigned int BaseId, unsigned int const* NpcId,
                                         unsigned int* OutValue) const
{
    for (std::vector<FVnpcRecord>::const_iterator It = Vnpcs.begin(); It != Vnpcs.end(); ++It)
    {
        if (It->Values[0] == BaseId && It->Values[1] == *NpcId)
        {
            *OutValue = It->Values[3];
            return true;
        }
    }
    return false;
}

// (06f48c60-06f48c9d, RET 8).
unsigned int FContentPlayerState::GetVnpcValue2(unsigned int BaseId, unsigned int NpcId) const
{
    for (std::vector<FVnpcRecord>::const_iterator It = Vnpcs.begin(); It != Vnpcs.end(); ++It)
    {
        if (It->Values[0] == BaseId && It->Values[1] == NpcId)
            return It->Values[2];
    }
    return 0;
}

// (06f48c20-06f48c2a, RET 4).
void FContentPlayerState::SetValueE4(unsigned int Value)
{
    ValueE4 = Value;
}

// (06f48c40-06f48c4a, RET 4).
void FContentPlayerState::SetValueE8(unsigned int Value)
{
    ValueE8 = Value;
}

// (06f48c10-06f48c16; added by CNT-AI6).
unsigned int FContentPlayerState::GetValueE4() const
{
    return ValueE4;
}

// (06f48c30-06f48c36; added by CNT-AI6).
unsigned int FContentPlayerState::GetValueE8() const
{
    return ValueE8;
}

// (06f48fb0-06f49020, RET 4): lower_bound on +0x48 (the routine) plus the key
// test is a find; absent -> insert {arch, 1} (the routine), else ++count.
void FContentPlayerState::CountShipTypeKilled(unsigned int ShipArchId)
{
    std::map<unsigned int, int>::iterator Entry = Stats.ShipTypesKilled.find(ShipArchId);
    if (Entry == Stats.ShipTypesKilled.end())
        Stats.ShipTypesKilled.insert(std::pair<unsigned int const, int>(ShipArchId, 1));
    else
        ++Entry->second;
}

// (06f49f90-06f49fc6): FindSystemInfo, then its player vector (the routine =
// &+0xb8): a null first pointer or a zero size -> false.
bool IsContentSystemOccupied(unsigned int SystemId)
{
    FSystemInfo* System = FindSystemInfo(&SystemId);
    return !System->Players.empty();
}

// (06f47850-06f4786a).
void FContentPlayerShip::ResetHostileState()
{
    bInCombat = 0;
    CalmSince = static_cast<double>(-1.0f);
    HostileContactCount = 0;
}

// (06f47840-06f47846): FLD double [ECX+0x178].
double FContentPlayerShip::GetCalmSince() const
{
    return CalmSince;
}

// (06f47870-06f47876): MOV EAX,[ECX+0x18c].
int FContentPlayerShip::GetHostileContactCount() const
{
    return HostileContactCount;
}

// (06f47590-06f4782f, RET 4).
//   - Every contact's TimeSinceSeen += DeltaTime.
//   - For each space object of the ship's system (+0xdc; system not null-tested): the
//     difference of positions (the routine, components stored as float), squared length
//     y*y + z*z + x*x (x87; double here) compared with 2500^2 (the field; FCOMP, TEST
//     AH,0x41, JP skips greater / unordered).
//       * A contact of it already listed (first match): in single player only while the
//         object's attitude towards the ship (GetAttitude(object rep, ship rep); reputations
// of both handles) is not above -0.6 (the field; TEST AH,0x41, JZ skips
//         greater) -- TimeNear += DeltaTime, TimeSinceSeen = 0.
//       * Not listed: attitude <= -0.6 (JP skips greater / unordered) -> appended as {object,
//         DeltaTime, 0} and ++HostileContactCount.
//   - Contacts whose TimeSinceSeen is not 0.0 (the field; TEST AH,0x44, JNP keeps equal)
//     are erased.
//   - None left: if bInCombat, it clears, CalmSince = the game time (the routine),
//     HostileContactCount = 0 and the system (when set) unmutes its NPC ships
//     (the routine(1)). Some left and bInCombat clear: it is set, CalmSince = -1.0 and the
//     system mutes them (the routine(0)).
void FContentPlayerShip::UpdateHostileContacts(float DeltaTime)
{
    for (std::list<FHostileContact>::iterator It = HostileContacts.begin();
         It != HostileContacts.end(); ++It)
    {
        It->TimeSinceSeen = It->TimeSinceSeen + DeltaTime;
    }

    FSystemInfo* ShipSystem = GetSystem();
    Vector Position;
    GetPosition(&Position);
    for (std::list<FContentSpaceObj*>::iterator ObjIt = ShipSystem->SpaceObjs.begin();
         ObjIt != ShipSystem->SpaceObjs.end(); ++ObjIt)
    {
        FContentSpaceObj* Object = *ObjIt;
        Vector ObjectPosition;
        Object->GetPosition(&ObjectPosition);
        Vector const Delta = SubtractVectors(ObjectPosition, Position);
        double DistanceSquare = static_cast<double>(Delta.y) * Delta.y +
                                static_cast<double>(Delta.z) * Delta.z +
                                static_cast<double>(Delta.x) * Delta.x;
        if (!(DistanceSquare <= 6250000.0f))
            continue;

        std::list<FHostileContact>::iterator Contact = HostileContacts.begin();
        for (; Contact != HostileContacts.end(); ++Contact)
        {
            if (Contact->Object == Object)
                break;
        }

        if (Contact != HostileContacts.end())
        {
            if (pub::SinglePlayer())
            {
                int ShipRep = ReputationHandle->GetReputation();
                int ObjectRep = Object->ReputationHandle->GetReputation();
                float Attitude = 0.0f;
                pub::Reputation::GetAttitude(ObjectRep, ShipRep, Attitude);
                if (Attitude > -0.6f)
                    continue;
            }
            Contact->TimeNear = DeltaTime + Contact->TimeNear;
            Contact->TimeSinceSeen = 0.0f;
            continue;
        }

        int ShipRep = ReputationHandle->GetReputation();
        int ObjectRep = Object->ReputationHandle->GetReputation();
        float Attitude = 0.0f;
        pub::Reputation::GetAttitude(ObjectRep, ShipRep, Attitude);
        if (!(Attitude <= -0.6f))
            continue;
        FHostileContact NewContact;
        NewContact.Object = Object;
        NewContact.TimeNear = DeltaTime;
        NewContact.TimeSinceSeen = 0.0f;
        HostileContacts.push_back(NewContact);
        ++HostileContactCount;
    }

    std::list<FHostileContact>::iterator It = HostileContacts.begin();
    while (It != HostileContacts.end())
    {
        if (It->TimeSinceSeen == 0.0f)
            ++It;
        else
            It = HostileContacts.erase(It);
    }

    if (HostileContacts.empty())
    {
        if (bInCombat == 0)
            return;
        bInCombat = 0;
        CalmSince = GetContentGameTime();
        HostileContactCount = 0;
        FSystemInfo* CalmSystem = GetSystem();
        if (CalmSystem != nullptr)
            CalmSystem->SetNpcShipsField1b1(true);
        return;
    }

    if (bInCombat != 0)
        return;
    bInCombat = 1;
    CalmSince = static_cast<double>(-1.0f);
    FSystemInfo* HostileSystem = GetSystem();
    if (HostileSystem != nullptr)
        HostileSystem->SetNpcShipsField1b1(false);
}
