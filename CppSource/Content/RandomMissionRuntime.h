// Author: Giperion.

#pragma once

#include "RandomMissionDB.h"
#include "common_ini.h"                 // Vector
#include "common_net.h"
#include "common_text.h"

#include <vector>

class INI_Reader;

// Random-mission runtime (implementation notes, TU map, lane CNT-RM):
//   TU57 06f70dd0-06f71fc0  StateMachine / MessageHandler derivatives for TU61
//   TU61 06f84060-06f94d10  random-mission runtime objects
// Ported so far: the parametrised text object used by the vignette tree (failure_text /
// reward_text / objective_text) and the three .data name tables TU61/TU63 share.

// ---------------------------------------------------------------------------------------
// .data name tables ({name, value} rows, FRmNamedValue from RandomMissionDB.h).
// ---------------------------------------------------------------------------------------

// .data PTR_s_FSTRING_PARAM_NONE_06fce868, 15 rows (loop bound 0x78): FSTRING_PARAM_NONE 0,
// OFFER_GROUP 1, HOSTILE_GROUP 2, BIG_SOLAR 3, OTHER_SOLAR 4, CRITICAL_LOOT 5,
// TARGET_FULL_NAME 6, REWARD_MONEY 7, FACTION_REWARD 8, FACTION_HIT 9,
// TIME_TO_RETURN_TO_BATTLE 10, MISSION_DIFFICULTY 11, OFFER_BASE 12, TARGET_ZONE 13,
// TARGET_SYSTEM 14. Readers: the routine, the routine.
extern FRmNamedValue const GRmTextParamNames[15];

// .data the field, 17 rows (loop bound 0x88): comm_sequence trigger names.
// Reader: the routine.
extern FRmNamedValue const GRmCommTriggerNames[17];

// .data the field, 12 rows (loop bound 0x60): comm_sequence speaker/target names.
// Reader: the routine.
extern FRmNamedValue const GRmCommTargetNames[12];

// ---------------------------------------------------------------------------------------
// TU61 state machine message types. The TU61 static initialisers the branch ...
// (one per global, in this order) each inline
// StateMachineMessage::GenerateUniqueMessageType() (INC [s_unique_message_type_generator],
// store the new value). Names come from the DestroyMission handler (the branch) where it
// reads them; the others keep their address.
// ---------------------------------------------------------------------------------------
extern unsigned int const GRmMsgStartMachine;       // DestroyMission -> a runtime machine
extern unsigned int const GRmMsgMachineStarted;     // machine -> mission, now running
extern unsigned int const GRmMsgMachineSucceeded;   // machine done, next one may start
extern unsigned int const GRmMsgMachineFailed;      // machine failed (reason at +0x1c)
extern unsigned int const GRmMsgRefreshObjectives;  // mission +0x25c = 1, objectives refreshed
extern unsigned int const GRmMsgType06fd3ba8;
extern unsigned int const GRmMsgType06fd3ba4;
extern unsigned int const GRmMsgType06fd3ba0;
extern unsigned int const GRmMsgType06fd3b9c;
extern unsigned int const GRmMsgType06fd3b98;
extern unsigned int const GRmMsgType06fd3b94;
extern unsigned int const GRmMsgType06fd3b90;
extern unsigned int const GRmMsgType06fd3b8c;
extern unsigned int const GRmMsgType06fd3b88;
extern unsigned int const GRmMsgType06fd3b84;
extern unsigned int const GRmMsgType06fd3b80;
extern unsigned int const GRmMsgType06fd3b7c;
extern unsigned int const GRmMsgType06fd3b78;
extern unsigned int const GRmMsgType06fd3b74;
extern unsigned int const GRmMsgPlayerLeft;         // DestroyMission, controller message 9
extern unsigned int const GRmMsgType06fd3b6c;

// =======================================================================================
// TU57 (06f70dd0-06f71fc0, RandomMissionTriggers.cpp): the trigger state machines and the
// machine group the TU61 runtime machine (the routine) is built on. Every user is in TU61
// (the routine, the routine).
// =======================================================================================

struct FContentPlayerState;         // ContentPlayerState.h

// (static init the branch): the "trigger fired" message type.
extern unsigned int const GRmMsgTriggerFired;

// TU57 -- trigger fired (baseline 0x28 bytes, vtable 06fbbde4: [0] the routine, the deleting
// dtor shared by the TU61 messages, [1] clone the routine). Built on the stack by
// FRmTrigger::Fire (06f71283-06f712a5) and sent to the trigger's owner.
class FRmTriggerFiredMessage : public StateMachineMessage
{
public:
    FRmTriggerFiredMessage(unsigned int InCookie, bool bInFinal, unsigned int InActivator);
    // operator new(0x28) (StateMachineMessage::operator new), the exported copy
    // ctor, +0x1c / +0x20 / +0x24; a failed new returns null.
    StateMachineMessage* clone() const override;

    unsigned int Cookie;            // +0x1c the trigger's Cookie
    bool bFinal;                    // +0x20 the trigger will not fire again
    unsigned int Activator;         // +0x24 the trigger's Activator
};

// TU57 -- trigger (baseline 0x40 bytes; vtable 06fbbda4, 11 slots: [0] scalar deleting dtor
// (dtor the routine: ~MessageHandler, ~StateMachine), [1] process (import),
// [2] Slot2 the branch, [3] handle_state_change (import), [4]..[10] below; MessageHandler
// side 06fbbdd0 ([0] the branch = this - 0x10 -> the routine, [1]..[4] imports)).
// States: 0 idle, 1 armed (the subclass's condition, slot 6), 2 fired, 3 waiting to re-arm,
// 4 done. Slot 6 is purecall here: TU57's three subclasses and TU61's (06fbca40, 06fbca84)
// supply it.
class FRmTrigger : public StateMachineMessageHandler
{
public:
    // (RET 0x10): StateMachine, MessageHandler(the field) at +0x10, +0x1c =
    // InOwnerId, +0x38 = InCookie, +0x24 = bInOneShot, +0x20 = 0, +0x28 = InRearmDelay.
    FRmTrigger(unsigned int InCookie, unsigned int InOwnerId, bool bInOneShot,
               float InRearmDelay);

    // (RET 8): State 0..4 -> slots 5..9 with (Event, State); always false.
    bool Slot2(SMEventData const* Event, int const& State) override;

    virtual void SetActivator(unsigned int InActivator);            // [4] the routine
    virtual bool OnIdle(SMEventData const* Event, int const& State);    // [5] the branch
    virtual bool OnArmed(SMEventData const* Event, int const& State) = 0; // [6] purecall
    virtual bool OnFired(SMEventData const* Event, int const& State);   // [7] the branch
    virtual bool OnRearmWait(SMEventData const* Event, int const& State); // [8] the routine
    virtual bool OnDone(SMEventData const* Event, int const& State);    // [9] the branch
    virtual void Fire();                                            // [10] the branch

    unsigned int OwnerId;           // +0x1c handler id the fired message goes to
    unsigned int Activator = 0;     // +0x20 SetActivator (a ship id from the subclasses)
    bool bOneShot;                  // +0x24 fired -> state 4 instead of re-arming
    float RearmDelay;               // +0x28 > 0: wait this long (state 3) before re-arming
    double LastFireTime;            // +0x30 the game time of the last Fire (only read after it)
    unsigned int Cookie;            // +0x38 echoed in the fired message
};

// TU57 -- fires when a player ship of System comes within Radius of Center (baseline 0x58,
// vtable 06fbbdec, [6] = the branch; MessageHandler side 06fbbe18). ctor the routine.
class FRmProximityTrigger : public FRmTrigger
{
public:
    // (RET 0x18): the base inline with bOneShot 1 and RearmDelay 1.0, +0x40 =
    // *InCenter, +0x4c = InRadius * InRadius, +0x50 = InSystemId, +0x54 = InPlayers.
    FRmProximityTrigger(unsigned int InCookie, unsigned int InOwnerId, unsigned int InSystemId,
                        Vector const* InCenter, float InRadius,
                        std::vector<FContentPlayerState*> const* InPlayers);
    bool OnArmed(SMEventData const* Event, int const& State) override;

    Vector Center;                  // +0x40
    float RadiusSq;                 // +0x4c
    unsigned int SystemId;          // +0x50
    std::vector<FContentPlayerState*> const* Players;  // +0x54 (the TU61 machine's list)
};

// TU57 -- fires Delay seconds after it is armed (baseline 0x48, vtable 06fbbe2c, [6] =
// ; MessageHandler side 06fbbe58). ctor the routine.
class FRmTimerTrigger : public FRmTrigger
{
public:
    // (RET 0xc): the base inline with bOneShot 1 and RearmDelay 1.0, +0x40 =
    // InDelay.
    FRmTimerTrigger(unsigned int InCookie, unsigned int InOwnerId, float InDelay);
    bool OnArmed(SMEventData const* Event, int const& State) override;

    float Delay;                    // +0x40
};

// TU57 -- fires once no player ship of System is within Radius of Center; the activator is
// the last such ship closer than 1e10 squared units (baseline 0x58, vtable 06fbbe6c, [6] =
// ; MessageHandler side 06fbbe98). ctor the routine (same layout as
// FRmProximityTrigger).
class FRmProximityLeaveTrigger : public FRmTrigger
{
public:
    // (RET 0x18): as the routine.
    FRmProximityLeaveTrigger(unsigned int InCookie, unsigned int InOwnerId,
                             unsigned int InSystemId, Vector const* InCenter, float InRadius,
                             std::vector<FContentPlayerState*> const* InPlayers);
    bool OnArmed(SMEventData const* Event, int const& State) override;

    Vector Center;                  // +0x40
    float RadiusSq;                 // +0x4c
    unsigned int SystemId;          // +0x50
    std::vector<FContentPlayerState*> const* Players;  // +0x54
};

// TU57 -- machine group (baseline 0x54 bytes; vtable 06fbbec0: [0] scalar deleting dtor
// (dtor the routine), [1] process (import), [2] purecall, [3]
// handle_state_change the branch, [4]..[6] the StateMachineGroup add / remove imports;
// MessageHandler side 06fbbeac ([0] the branch = this - 0x10 -> the routine)). Base of
// the TU61 runtime machine (ctor the routine 06f89a26). Member machines are kept by handler
// id: Machines live as long as the group, StateMachines only until the next state change.
class FRmMachineGroup : public StateMachineGroup
{
public:
    // (RET 4): StateMachineGroup(InRouter), both id vectors empty.
    explicit FRmMachineGroup(IMessageRouter* InRouter);
    // every id of Machines, then of StateMachines, goes to
    // remove_state_machine(unsigned) (front first); then ~StateMachineGroup.
    ~FRmMachineGroup() override;

    // (RET 4): add_state_machine(Machine), then its handler id (+0x10 get_id)
    // appended to StateMachines.
    void AddStateMachine(StateMachineMessageHandler* Machine);
    // (RET 4): the same into Machines.
    void AddMachine(StateMachineMessageHandler* Machine);
    // (RET 4): remove_state_machine(Id); then Id is removed from StateMachines,
    // or, when it is not there, from Machines (std::remove + erase, the routine).
    void RemoveMachine(unsigned int Id);

protected:
    // while a change is pending: slot 2 with an exit event (type 2), every
    // StateMachines id removed, the new state, slot 2 with an enter event (type 1).
    void handle_state_change() override;

    std::vector<unsigned int> Machines;         // +0x34 (alloc byte) / +0x38..+0x40
    std::vector<unsigned int> StateMachines;    // +0x44 (alloc byte) / +0x48..+0x50
};

// ---------------------------------------------------------------------------------------
// TU61 messages with one dword after the StateMachineMessage (0x20 bytes). Every one of
// these classes shares the deleting dtor the routine (the routine resets the vptr to
// Content's copy of the StateMachineMessage table 06fb7730, then
// StateMachineMessage::operator delete); clone = operator new(0x20) (the exported
// StateMachineMessage::operator new), the exported copy ctor and the +0x1c copy.
// ---------------------------------------------------------------------------------------

// Vtable 06fbc81c: ctor the routine (RET 4), clone the routine. Sent by a runtime machine
// when it fails; DestroyMission keeps the reason (06eefe9a) for slot 35.
class FRmMachineFailedMessage : public StateMachineMessage
{
public:
    explicit FRmMachineFailedMessage(int InReason);
    StateMachineMessage* clone() const override;

    int Reason;                                     // +0x1c
};

// Vtable 06fbc824: ctor the routine (RET 4), clone the routine. DestroyMission sends it to
// every running machine on controller message 9 (06eef9d0-06eefa13).
class FRmPlayerLeftMessage : public StateMachineMessage
{
public:
    explicit FRmPlayerLeftMessage(unsigned int InPlayerId);
    StateMachineMessage* clone() const override;

    unsigned int PlayerId;                          // +0x1c
};

// ---------------------------------------------------------------------------------------
// TU61 -- text with parameters (baseline 0x30 bytes, vtable 06fbc700):
//   [0] scalar deleting dtor the routine (dtor the routine), [1] Parse the routine,
//   [2] Build the branch, [3] IsEmpty the branch.
// Derived TU61 text classes (vtables 06fbc6d8 / 06fbc710 / 06fbc73c / 06fbc770) extend this
// four-slot head.
// ---------------------------------------------------------------------------------------
class IRmTextParamSource;           // RandomMissionOffer.h

struct FRmParamText
{
    FmtStr Text;                    // +0x04 built by slot 2
    int StrId;                      // +0x1c
    std::vector<int> Params;        // +0x20 (alloc byte) / +0x24..+0x2c GRmTextParamNames values

    FRmParamText();
    virtual ~FRmParamText();
    virtual void Parse(INI_Reader* Reader, unsigned int Index);
    // Slot 2, the branch (RET 4): builds Text once (mad lib StrId, Source slot 0 per
    // parameter) and returns &Text.
    virtual FmtStr* Build(IRmTextParamSource* Source);
    virtual bool IsEmpty() const;
};

// ---------------------------------------------------------------------------------------
// TU61 -- objective text (baseline 0x48 bytes, vtable 06fbc6d8): FRmParamText and six slots.
// The runtime machine keeps the objectives of its offer components (FRmObjectiveMap) in its
// +0x7c vector and turns each into a mission objective through slot 6. Derived vtables
// (11 slots; ctors the routine from a text, the routine) and, below it, 06fbc73c
// (13 slots, FRmObjectiveType3) and 06fbc770 (12 slots, FRmObjectiveType4).
// ---------------------------------------------------------------------------------------
class FMissionObject;               // MissionObject.h
class FMissionObjective;            // MissionObject.h
class FRmRuntimeMachine;            // RandomMissionMachine.h

class FRmObjective : public FRmParamText
{
public:
    // The base part of the derived ctors (the routine 06f90a72-06f90acd, the routine
    // ): the text empty, +0x30 = 0x3a, the rest 0.
    FRmObjective() = default;

    // Slot 4, the routine: new 0x48 member copy (FmtStr copy ctor, the parameter vector,
    // +0x30..+0x44), vtable 06fbc6d8; null when new fails.
    virtual FRmObjective* Clone() const;
    // Slot 5, the branch (ICF): 1.
    virtual int GetType() const;
    // Slot 6, the branch (RET 8): a type 7 mission objective (CreateMissionObjective with
    // Mission) whose Type is ObjectiveType and whose text is slot 2 built with Machine's
    // text-parameter source (+0x54; null without a machine); bHidden 0.
    virtual FMissionObjective* CreateMissionObjective(FMissionObject* InMission,
                                                      FRmRuntimeMachine* Machine);
    // Slot 7, the branch (ICF: XOR AL,AL; RET 4): no position.
    virtual bool GetPosition(Vector* Out);
    // Slot 8, the branch (RET 0xc): nothing.
    virtual void SetPosition(Vector InPosition);
    // Slot 9, the branch (ICF): no system.
    virtual bool GetSystemId(unsigned int* Out);

    // +0x30: the mission objective's Type (pub::Player::MissionObjective, 0x3a by default).
    int ObjectiveType = 0x3a;
    int Key = 0;                    // +0x34 the FRmObjectiveMap key (the routine writes it)
    Vector Position = {};           // +0x38 TODO: reader (only the copies touch it)
    unsigned char Field_0x44 = 0;   // +0x44 TODO: meaning
};

// ---------------------------------------------------------------------------------------
// TU61 -- objective with a nav marker (baseline 0x5c bytes, vtable 06fbc710; GetType 2). The
// marker (+0x48 system, +0x4c value, +0x50 position = FmtStr::NavMarker) is appended to the
// text by slot 2. Ctors the routine (from a text) and the routine (empty); neither writes
// the marker.
// ---------------------------------------------------------------------------------------
class FRmObjectiveType2 : public FRmObjective
{
public:
    // the FRmObjective defaults, vtable 06fbc710.
    FRmObjectiveType2() = default;
    // (RET 4): the defaults, then StrId and Params of Source (the vector
    // assigned unless it is the same object).
    explicit FRmObjectiveType2(FRmParamText const* Source);

    // Slot 2, the branch (RET 4): FRmParamText::Build, and when the text was rebuilt the
    // marker appended (FmtStr::append_nav_marker) after end_mad_lib.
    FmtStr* Build(IRmTextParamSource* Source) override;
    // Slot 4, the routine: new 0x5c member copy.
    FRmObjective* Clone() const override;
    // Slot 5, the branch (ICF): 2.
    int GetType() const override;
    // Slot 7, the branch (RET 4): *Out = MarkerPosition; true.
    bool GetPosition(Vector* Out) override;
    // Slot 8, the branch (RET 0xc): MarkerPosition = InPosition.
    void SetPosition(Vector InPosition) override;
    // Slot 9, the branch (RET 4): *Out = MarkerSystemId; true.
    bool GetSystemId(unsigned int* Out) override;
    // Slot 10, the branch (RET 0x14): the three marker fields.
    virtual void SetMarker(unsigned int InSystemId, int InValue, Vector InPosition);

    // +0x48..+0x5b: not written by the ctors (heap garbage in baseline); the port starts at 0.
    unsigned int MarkerSystemId = 0;    // +0x48 NavMarker word 0
    int MarkerValue = 0;                // +0x4c NavMarker word 1 (0x44d from the generator)
    Vector MarkerPosition = {};         // +0x50 NavMarker words 2..4
};

// ---------------------------------------------------------------------------------------
// TU61 -- objective that marks objects for the players (baseline 0x70 bytes, vtable 06fbc73c;
// GetType 3). ctor the routine (from a text), dtor the routine (deleting the routine).
// ---------------------------------------------------------------------------------------
struct FContentPlayerState;         // ContentPlayerState.h
class FRmRecordList;                // RandomMissionMachine.h

class FRmObjectiveType3 : public FRmObjectiveType2
{
public:
    // (RET 4): as FRmObjectiveType2's, the id vector empty; +0x6c not written
    // (the port starts it null).
    explicit FRmObjectiveType3(FRmParamText const* Source);
    // every id unmarked (pub::Player::MarkObj 0) for every player of Players,
    // then the members and the base.
    ~FRmObjectiveType3() override;

    // Slot 2 of 06fbc73c is FRmParamText's the branch (no nav marker), not
    // FRmObjectiveType2's 06f860a0.
    FmtStr* Build(IRmTextParamSource* Source) override
    {
        return FRmParamText::Build(Source);
    }
    // Slot 4, the routine: new 0x70 member copy.
    FRmObjective* Clone() const override;
    // Slot 5, the branch (ICF): 3.
    int GetType() const override;
    // Slot 11, the branch (RET 0x18): the marker (value 0), InPlayers kept, the text reset;
    // the old ids unmarked for InPlayers, Ids = *InIds (unless the same vector), the new ids
    // marked (MarkObj 1).
    virtual void SetTargets(std::vector<unsigned int> const* InIds,
                            std::vector<FContentPlayerState*>* InPlayers, unsigned int InSystemId,
                            Vector InPosition);
    // Slot 12, the branch (RET 0x1c): slot 11 with the ids of Records' records that are
    // alive (State 2) and have a bit of Mask in Flags.
    virtual void SetTargetRecords(FRmRecordList* Records,
                                  std::vector<FContentPlayerState*>* InPlayers,
                                  unsigned int Mask, unsigned int InSystemId, Vector InPosition);

    std::vector<unsigned int> Ids;      // +0x5c (alloc byte) / +0x60..+0x68 marked objects
    // +0x6c: the players the ids are marked for (the runtime machine's mission players).
    std::vector<FContentPlayerState*>* Players = nullptr;
};

// ---------------------------------------------------------------------------------------
// TU61 -- countdown objective (baseline 0x60 bytes, vtable 06fbc770, 12 slots; GetType 4): the
// "return to the combat area" timer of the vignette machine (the routine 06f8cd07-06f8cd33:
// operator new(0x60), the routine, the 06fbc770 store, +0x30 = 0xda). No ctor or dtor of its
// own: slot 0 is FRmParamText's deleting dtor the routine.
// ---------------------------------------------------------------------------------------
class FRmObjectiveType4 : public FRmObjectiveType2
{
public:
    // Slot 4 of 06fbc770 is FRmObjective's the routine (not FRmObjectiveType2's 06f85790): a
    // clone is a plain 0x48 FRmObjective (GetType 1).
    FRmObjective* Clone() const override
    {
        return FRmObjective::Clone();
    }
    // Slot 2, the branch (RET 4): built once -- the mad lib of StrId and the parameters,
    // end_mad_lib, the marker (append_nav_marker of +0x48) only when the position is away from
    // the origin, then append_int(Seconds).
    FmtStr* Build(IRmTextParamSource* Source) override;
    // Slot 5, the branch (ICF): 4.
    int GetType() const override;
    // Slot 7, the branch (RET 4): the marker position, only when it is away from the origin.
    bool GetPosition(Vector* Out) override;
    // Slot 11, the branch (RET 0x18): the text reset (FmtStr::destroy, six dwords zeroed);
    // the marker (InSystemId, InValue, InPosition) only for a position away from the origin;
    // Seconds always.
    virtual void SetCountdown(int InSeconds, unsigned int InSystemId, int InValue,
                              Vector InPosition);

    // +0x5c: the seconds shown (append_int). Not written by the construction (heap garbage in
    // baseline); the port starts it at 0.
    int Seconds = 0;
};
