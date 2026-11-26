// Author: Giperion.

#pragma once

// The random-mission controller objects (implementation notes, TU map, lane CNT-RM):
//   TU47 (06f4b0d0-06f4d220)  FRandomMissionObject, the base of every random mission
//                             controller; bodies in RandomMissionDB.cpp.
//   TU27 (06eef3d0-06ef04d0)  FDestroyMission, the "DestroyMission" controller that
//                             CreateInstance builds (the routine 06eeed28); bodies in
//                             DestroyMission.cpp.

// baseline layout: the TU38 mission object (0x1b0 bytes, FMissionObject) followed by a Common
// StateMachineGroup at +0x1b0 (its StateMachine vtable at +0x1b0, its MessageHandler vtable at
// +0x1c0, 0x34 bytes). The port expresses that as the second base class. baseline has no RTTI in
// Content.dll; the class names are the port's.

#include "MissionObject.h"
#include "RandomMission.h"      // FRandomMissionData, TRmRef
#include "common_net.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

struct FContentPlayerState;
struct FRandomMissionData;
struct FSystemInfo;

// (TU27, thiscall on the player state): the state has a ship object (+0x10).
// Callers DestroyMission 06eef922 / 06eef972 and TU61 06f8f99a.
bool RmPlayerHasShip(FContentPlayerState const* State);

// ---------------------------------------------------------------------------------------
// TU47 -- random mission object base (baseline 0x240 bytes). Ctor the routine (RET 8), dtor
// , deleting dtor the routine. Vtables: 06fbb288 (45 slots), StateMachine side
// (slot 2 = the routine), MessageHandler side 06fbb258. Only DestroyMission
// derives from it (its ctor is the only caller of 06f4b0d0).
// ---------------------------------------------------------------------------------------
class FRandomMissionObject : public FMissionObject, public StateMachineGroup
{
public:
    // (RET 8): InData is the controller's create parms (the TU27 mission).
    FRandomMissionObject(unsigned int const* InControllerId, FRandomMissionData* InData);
    // (called by the deleting dtor the routine).
    ~FRandomMissionObject() override;

    // ---- TU38 slots this class replaces ----
    // Slot 2, the branch (RET 4): the base record (the routine); a record whose slot 2 is
    // set gets slot 1(0).
    FMissionPlayer* GetOrCreatePlayer(unsigned int InPlayerId) override;
    // Slot 18, the routine (RET 4): rebuild the player map / vector from the new player's group.
    void SetPlayer(unsigned int InPlayerId) override;
    // Slot 21, the branch (RET 8): the message goes to the state machine group as an
    // SMControllerEvent; the TU38 body runs from Slot2.
    void HandleMissionMessage(pub::Controller::EMessage MessageId, void const* MessageParms) override;
    // Slot 22 is 06f4cd40, a JMP to the TU38 body the routine: no override.
    // Slot 23, the routine: victory music, completed statistics, reputation, then the TU38 body.
    void OnMissionSucceed() override;

    // ---- slots 29..44 ----
    // Slot 29, the branch: &+0x1fc.
    virtual std::vector<FContentPlayerState*>* GetPlayerStates();
    // Slot 30, the branch: &+0x20c.
    virtual std::map<unsigned int, FContentPlayerState*>* GetPlayerStateMap();
    // Slot 31, the branch (RET 8): the type 8 "other players" objective of Player.
    virtual bool UpdatePlayerCountObjective(FMissionPlayer* Player, bool* bChanged);
    // Slots 32..34, the branch / the branch / the branch: --, ++ and read +0x220.
    virtual int DecrementPlayerCount();
    virtual int IncrementPlayerCount();
    virtual int GetPlayerCount();
    // Slot 35, the routine (RET 4): the mission failed (Reason 1 = aborted).
    virtual void FailMission(int Reason);
    // Slot 36, the routine (RET 8): InPlayerId leaves the mission.
    virtual void RemoveMissionPlayer(unsigned int InPlayerId, int Reason);
    // Slot 37, the routine: slot 38 for every player.
    virtual void RefreshAllObjectives();
    // Slot 38, the branch (RET 4): slots 40, 39 and (multiplayer) 31, then resend.
    virtual void RefreshObjectives(unsigned int InPlayerId);
    // Slot 39, the branch (RET 8): the type 2 "return to space" objective of a docked player.
    virtual bool UpdateDockedObjective(FMissionPlayer* Player, bool* bChanged);
    // Slot 40, the routine (RET 8): the path objectives to the mission's system (slot 8).
    virtual bool UpdatePathObjectives(FMissionPlayer* Player, bool* bChanged);
    // Slot 41, the branch (RET 0x14): reputation change towards Group, once per player name
    // and group unless bForce.
    virtual void ChangePlayerReputation(unsigned int InPlayerId, unsigned int Group, float Delta,
                                        bool bForce, bool bEmpathy);
    // Slot 42, the branch (RET 0xc): reputation change with empathy, always.
    virtual void ChangePlayerReputationAlways(unsigned int InPlayerId, unsigned int Group,
                                              float Delta);
    // Slot 43, the routine (RET 0xc): slot 41 (bForce false) for every player of the map.
    virtual void ChangeGroupReputation(unsigned int Group, float Delta, bool bEmpathy);
    // Slot 44, the routine (RET 8): slot 42 for every player of the map.
    virtual void ChangeGroupReputationAlways(unsigned int Group, float Delta);

    // Oracle instrumentation (P3.7b, OracleDumpRmSession; not a slot, no game module calls it):
    // InPlayerId mapped, listed and named as SetPlayer does for each member of the player's group,
    // which a player without a server slot does not have.
    void OracleJoinPlayer(unsigned int InPlayerId);

protected:
    // StateMachine slot 2, the routine (RET 8): the "any state" handler of the controller
    // messages. Called directly by the DestroyMission override (06eef740) with State -1.
    bool Slot2(SMEventData const* Event, int const& State) override;

    // (RET 8): the +0xd0 mission flags of a mapped player for SystemId.
    void UpdatePlayerSystemFlags(unsigned int InPlayerId, unsigned int SystemId);
    // (RET 4): message 5 for a mapped player (rejoin).
    void RejoinPlayer(unsigned int InPlayerId);
    // (RET 4): message 0x24 (group left).
    void OnPlayerLeftGroup(unsigned int const* InPlayerId);
    // (RET 4): message 0xa (a group member joins).
    void AddGroupMember(unsigned int InPlayerId);
    // (RET 8): a +0x1e4 entry at or after the player's name holds Group.
    bool HasReputationEntry(unsigned int InPlayerId, unsigned int Group);
    // The unique-name insert inlined in the routine and the routine (06f4c4c8-06f4c5b3 /
    // ): +0x220 grows while fewer than 8 names are listed. The name
    // (the routine) is the caller's local: baseline frees it at the end of each SetPlayer loop
    // pass (06f4c5be) but only at the very end of AddGroupMember (06f4ccea).
    void AddPlayerName(std::wstring const& Name);

    // +0x1e4 multimap (ctor the routine with the multi flag 1): player name -> group whose
    // reputation slot 41 already changed.
    std::multimap<std::wstring, unsigned int> ReputationChanges;
    // +0x1f8: the controller's create parms, one reference in a holder (AddRef in the ctor
    // ; the holder dtor the routine runs after +0x1fc, inlined at 06f4b54a-06f4b565).
    TRmRef<FRandomMissionData> Data;
    // +0x1fc: the player states of the map below, in key order (rebuilt on every change).
    std::vector<FContentPlayerState*> PlayerStates;
    // +0x20c map (ctor the routine): player id -> player state.
    std::map<unsigned int, FContentPlayerState*> PlayerStateMap;
    // +0x220: players counted for the type 8 objective (see AddPlayerName).
    int PlayerCount = 0;
    // +0x224: the names seen (vector of wstring, elements destroyed by the routine).
    std::vector<std::wstring> PlayerNames;
    // +0x234: seconds since the start (Slot2, message 0x20); -1.0f once it reached the
    // mission's +0x48 (360.0f), after which the +0xd0 system flags are no longer updated.
    // Ctor 0.
    float ElapsedTime = 0.0f;
    // +0x23c: fail reason stored by the DestroyMission state machine (06eefe9d), ctor 0.
    int FailReason = 0;
};

// ---------------------------------------------------------------------------------------
// TU27 -- the "DestroyMission" controller (baseline 0x29c bytes). Ctor the routine (RET 8),
// dtor the routine, deleting dtor the routine. Vtables: 06fb7658 (46 slots; replaces 1, 8,
// 9, 12, 22, 23, 35, 36 and adds 45), StateMachine side 06fb7638 (slot 2 = the branch),
// MessageHandler side 06fb7624.
// ---------------------------------------------------------------------------------------
class FDestroyMission : public FRandomMissionObject
{
public:
    // (RET 8).
    FDestroyMission(unsigned int const* InControllerId, FRandomMissionData* InData);
    // .
    ~FDestroyMission() override;

    // Slot 8, the branch: the first runtime machine's slot 8, else the system id of +0x244.
    int Slot08() override;
    // Slot 9, the branch: the jump preference of the mission (+0x240 +0x50).
    int Slot09() override;
    // Slot 12: the branch (XOR AL,AL; RET) where the TU47 table has the branch.
    bool Slot12() override;
    // Slot 22, the routine: slot 7 of every +0x28c machine, then the TU38 body.
    void EndMission() override;
    // Slot 23, the branch: +0x10 = the mission reward, then the TU47 body.
    void OnMissionSucceed() override;
    // Slot 35, the routine (RET 4): the reason text to the first player, then the TU47 body.
    void FailMission(int Reason) override;
    // Slot 36, the routine (RET 8): the reason text to the player, then the TU47 body.
    void RemoveMissionPlayer(unsigned int InPlayerId, int Reason) override;
    // Slot 45, the branch (RET 4): the resource id of a fail reason (0..14), else 1.
    virtual unsigned int GetReasonIds(int Reason);

    // Oracle instrumentation (P3.7d, OracleDumpRmSession; not a slot, no game module calls it):
    // what a PlayerSave {Client, File} writes of the mission, without the message's two
    // single-player gates, which a multiplayer session cannot pass.
    void OracleSave(unsigned int Client, FILE* File);

protected:
    // StateMachine slot 2, the branch (RET 8): states 0 (brief), 1 (launch the runtime
    // machines), 2 (running), 3 (succeeded), 4 (failed) and the "any state" messages.
    bool Slot2(SMEventData const* Event, int const& State) override;

    // a runtime machine per offer of the mission, added to the group; the new
    // handler ids go to +0x26c.
    void CreateRuntimeMachines();

    // +0x240: one reference in a holder (AddRef in the ctor 06eef411; the holder dtor
    // runs after the +0x248 list, inlined at 06eef693-06eef6ad).
    TRmRef<FRandomMissionData> MissionData;
    FSystemInfo* DestSystem = nullptr;                  // +0x244 FindSystemInfo(&DestSys)
    // +0x248: the owned objective list (the routine, then the derived vptr 06fb478c at
    // ); its +0x10 Owner (+0x258) = 0 at 06eef44e.
    FMissionOwnedObjectiveList Objectives;
    // +0x25c: set by message the field (06eef8af). baseline's ctor leaves it as heap
    // garbage; the port starts it at 0. TODO: meaning from its readers.
    unsigned char Field_0x25c = 0;
    std::vector<unsigned int> PendingMachines;          // +0x26c ids not started yet
    std::vector<unsigned int> StartingMachines;         // +0x27c ids sent the field
    std::vector<unsigned int> RunningMachines;          // +0x28c ids that answered the field
};
