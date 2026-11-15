// Author: Giperion.

#pragma once

#include "ContentRefObject.h"       // FContentRefObject: the ref-counted base, vtable 06fb43f8
#include "Gcs.h"                    // FGcsConversation, FGcsPlayerListLine (TU25)
#include "MissionObject.h"         // FMissionShip, FMissionLoot, FMissionSolar
#include "NpcNames.h"               // FNpcNameParams
#include "NpcShips.h"               // FMsnNpc
#include "RandomMissionRuntime.h"   // FRmParamText
#include "common_id.h"
#include "common_net.h"             // MessageHandler
#include "common_text.h"
#include "x86math.h"

#include <list>
#include <map>
#include <vector>

struct FRmCriticalLoot;             // RandomEncounters.h (TU62)
struct FRmKillableSolar;            // RandomEncounters.h (TU62)
struct FSystemInfo;
class FRandomMissionObject;         // RandomMissionObject.h
class FRmRuntimeMachine;            // RandomMissionMachine.h

// Random-mission offer (implementation notes, TU map, lane CNT-RM): the TU61 objects the offer tree of
// TU63 (FRandomMissionGenerator::BuildOffer) produces and the random mission keeps
// (FRandomMissionData::Offers). Two classes: the base offer FRmOffer (baseline 0xb8, ctor
// ) and the vignette offer FRmVignetteOffer (baseline 0x218, ctor the routine) that
// both mission-type filters (destroy solars the routine, assassinate the routine) create.
// The vignette offer holds four offer components -- MessageHandlers registered with the
// content message router (the field) as soon as the offer is built -- whose ship groups,
// solars and objectives the TU61 runtime machine (the routine, not ported) spawns.

// ---------------------------------------------------------------------------------------
// Source of the values of a parametrised text: FRmParamText slot 2 (the branch) and
// FRmOfferText slot 2 (the routine) call slot 0 once per parameter id (GRmTextParamNames
// values). The offers' primary vtables have this one slot and no destructor (06fbc908 = base
// offer the routine, 06fbc898 = vignette offer the branch).
// ---------------------------------------------------------------------------------------
class IRmTextParamSource
{
public:
    // Appends the value of Param to Out; false when this source has no such value.
    virtual bool GetTextParam(int Param, FmtStr* Out) = 0;

protected:
    ~IRmTextParamSource() = default;
};

// ---------------------------------------------------------------------------------------
// TU61 -- base offer (baseline 0xb8 bytes). Primary vtable 06fbc908 ([0] GetTextParam
// ); the ref-counted FContentRefObject at +0x04 (vtable 06fbc8fc: AddRef
// , Release the routine, deleting dtor the routine = the routine + free of
// this - 4), count 1 at +0x08. The dtor the routine resets the +0x04 vptr to 06fb43f8.
// ---------------------------------------------------------------------------------------
class FRmOffer : public IRmTextParamSource, public FContentRefObject
{
public:
    // (RET 4): +0x0c/+0x14/+0x18/+0x1c/+0x20 = 0, +0x10 = InMachineType,
    // +0x24/+0x28 = -1, the text and both parametrised texts empty, +0x54 = 0.
    explicit FRmOffer(int InMachineType);
    FRmOffer(FRmOffer const&) = delete;
    FRmOffer& operator=(FRmOffer const&) = delete;
    // FailureText, RewardText, Text (reverse member order).
    ~FRmOffer() override;

    // (RET 8).
    bool GetTextParam(int Param, FmtStr* Out) override;

    // +0x0c: the mission running the offer; FDestroyMission::CreateRuntimeMachines
    // (the routine 06ef0141) writes it right before the factory, the runtime machine copies
    // it to its +0x88 (the routine 06f89a5d).
    FRandomMissionObject* Mission = nullptr;
    // +0x10: the runtime machine the routine builds (1..3; anything else builds none):
    // destroy solars 1, assassinate 2 or 3 (the routine 06f9b7cf: NEG/SBB/ADD 3).
    int MachineType;
    FSystemInfo* System = nullptr;              // +0x14 the mission system (the routine)
    // +0x18: the offer group's FFactionProps::McCostume (the routine 06f9ebee-06f9ebfb).
    int McCostume = 0;
    unsigned int VoiceId = 0;                   // +0x1c pub::GetVoiceID("mc_leg_m01")
    unsigned int OfferBase = 0;                 // +0x20 OFFER_BASE
    int OfferGroup = -1;                        // +0x24 OFFER_GROUP
    unsigned int HostileGroup = 0xffffffffu;    // +0x28 HOSTILE_GROUP
    // +0x2c..+0x38 are not written by the ctor (heap garbage in baseline); BuildOfferFromPath
    // writes all four before the offer is handed out. The port starts them at 0.
    float Difficulty = 0.0f;                    // +0x2c MISSION_DIFFICULTY (remapped rank)
    int Reward = 0;                             // +0x30 REWARD_MONEY
    float RepHit = 0.0f;                        // +0x34 FACTION_HIT (x 100)
    float RepReward = 0.0f;                     // +0x38 FACTION_REWARD (x 100)
    FmtStr Text;                                // +0x3c the offer text (the routine)
    // +0x54: the mission type shown to the player (text 0x32fa + Type, rm_* statistics):
    // destroy solars 1; assassinate 0, 2, 3, 4 or 5.
    unsigned char Type = 0;
    FRmParamText RewardText;                    // +0x58 node reward_text (the routine)
    FRmParamText FailureText;                   // +0x88 node failure_text (the routine)
};

// ---------------------------------------------------------------------------------------
// Objectives of an offer component, keyed by objective id (baseline 0x18 bytes, vtable
// [0] scalar deleting dtor the routine; ctor the routine, dtor the routine).
// The values are owned: the dtor deletes each through its vtable slot 0 (06f89270-06f8927b)
// before the tree is freed. Filled by the routine (FRandomMissionGenerator, insert
// , signed keys) with the TU61 objectives FRmObjectiveType2 (0x5c, ctor
// ) and FRmObjectiveType3 (0x70, ctor the routine), RandomMissionRuntime.h.
// ---------------------------------------------------------------------------------------
class FRmObjectiveMap
{
public:
    FRmObjectiveMap() = default;
    FRmObjectiveMap(FRmObjectiveMap const&) = delete;
    FRmObjectiveMap& operator=(FRmObjectiveMap const&) = delete;
    virtual ~FRmObjectiveMap();

    std::map<int, FRmParamText*> Objectives;    // +0x04
};

// ---------------------------------------------------------------------------------------
// Comm sequences of an offer (VignetteParams.ini "comm_sequence", FRmCommSequence): one
// conversation per sequence, one line per {speaker, text} pair. the routine builds them into
// the vignette offer's General component; the component (the routine) sends a clone in
// message GRmMsgType06fd3b74 and the TU61 runtime machine hands it to the GCS conversation
// manager (the routine, which clones it once more through slot 5).
// ---------------------------------------------------------------------------------------

// The line bases are TU25 classes declared in Gcs.h (lane GCS): FGcsCloneableLine (vtable
// , FGcsLine + [5] Clone purecall) and FGcsPlayerListLine (vtable 06fb623c, 0x40
// bytes, PlayerIds at +0x34; ctor the routine). FGcsPlayerListLine's dtor the routine and
// scalar deleting dtor the routine are emitted in TU61 and ICF-shared with FRmLine (it adds
// nothing to destroy).

// TU61 -- one line of a comm sequence (baseline 0x90 bytes, vtable 06fbc648: [0] scalar deleting
// dtor the routine, [1] CanPlay the routine, [2]-[4] FGcsPlayerListLine's Play the branch,
// IsDone the routine, GetDuration the branch, [5] Clone the routine). ctor the routine
// (caller the routine).
class FRmLine : public FGcsPlayerListLine
{
public:
    // FGcsPlayerListLine(null) (the routine with 0), the kinds 0, the text
    // empty, +0x8c = 0.
    FRmLine();
    // FGcsPlayerListLine::CanPlay (the routine, a direct call) and +0x8c.
    bool CanPlay() override;
    // new 0x90 copy.
    FGcsCloneableLine* Clone() override;

    int SpeakerKind;                            // +0x40 FRmCommLine::Speaker
    int TargetKind;                             // +0x44 FRmCommSequence::Target
    TString<0x40> Text;                         // +0x48 FRmCommLine::Text
    // +0x8c: TODO: meaning; CanPlay needs it non-zero, the ctor and the routine leave it 0
    // (writer not found yet -- the runtime machine side).
    unsigned char Field_0x8c;
};

// TU61 -- conversation base with a copy (vtable 06fbc690: FGcsConversation's five slots with
// [2] IsValid still purecall, [5] Clone = purecall the branch, [6] CloneLines
// ). Stored only by the copy ctors (06f84ccf, 06fa3e7f) before the derived vptr.
// The GCS conversation manager's the routine takes this type (slots 2, 1 and 5).
class FRmConversationBase : public FGcsConversation
{
public:
    // Slot 5 (+0x14): a heap copy whose lines are copies too.
    virtual FRmConversationBase* Clone() = 0;
    // Slot 6 (+0x18), the routine: Lines is replaced by a vector of Clone() of each line
    // (the routine appends); the old pointers are not deleted (a fresh copy shares them).
    virtual void CloneLines();
};

// TU61 -- a comm sequence conversation (baseline 0x24 bytes, vtable 06fbc660: [0] scalar
// deleting dtor the routine (FGcsConversation's dtor thunk_FUN_06ee2b70 = 06f84d20), [1]
// Start the branch, [2] IsValid the routine, [3] OnMessage the routine, [4] IsFinished
// , [5] Clone the routine, [6] CloneLines the routine, [7] SetTrigger
// (ICF), [8] GetTrigger the branch, [9] MarkReady the branch, [10] GetDelay
// , [11] SetDelay the branch). ctor the routine (RET 4; its byte argument
// is the vector's allocator byte at +0x04). Kept by value in the offer components' Conversations
// list; the copy into the list (the routine) shares the line pointers, as baseline.
class FRmConversation : public FRmConversationBase
{
public:
    FRmConversation() = default;
    // MarkReady was called and every line CanPlay (slot 1).
    bool IsValid() override;
    // new 0x24 member copy, then its CloneLines (slot 6).
    FRmConversationBase* Clone() override;
    virtual void SetTrigger(int InTrigger);     // +0x20
    virtual int GetTrigger();                   // +0x20
    virtual void MarkReady();                   // +0x18 = 1
    virtual float GetDelay();                   // +0x1c
    virtual void SetDelay(float InDelay);       // +0x1c

    bool bReady = false;                        // +0x18
    float Delay = 0.0f;                         // +0x1c seconds (the routine sends after it)
    int Trigger = 0;                            // +0x20 FRmCommSequence::Trigger
};

// ---------------------------------------------------------------------------------------
// TU61 -- offer component (baseline 0x34 bytes, vtable 06fbc6ac: [0] scalar deleting dtor
// / dtor the routine, [1] receive_message the routine (RET 4, empty), [2]-[4]
// the MessageHandler send_* imports, [5] the routine, [6] the branch, [7] the branch).
// ctor the routine: MessageHandler(the field), +0x0c = 0, the list, the objective map.
// Slots 5-7 are called by the runtime machine (FRmVignetteMachine, the routine).
// ---------------------------------------------------------------------------------------
struct FContentPlayerState;         // ContentPlayerState.h

class FRmOfferComponent : public MessageHandler
{
public:
    FRmOfferComponent();
    FRmOfferComponent(FRmOfferComponent const&) = delete;
    FRmOfferComponent& operator=(FRmOfferComponent const&) = delete;
    ~FRmOfferComponent() override;

    void receive_message(StateMachineMessage* Msg) override;

    // Slot 5, the routine (RET 0xc): every conversation of Trigger is prepared (slot 6) and a
    // clone of it sent to Machine in FRmConversationMessage after its delay.
    virtual void SendConversations(int Trigger, FRmRuntimeMachine* Machine,
                                   FContentPlayerState* Player);
    // Slot 6, the branch (RET 0xc): MarkReady; per line the texts and players cleared, the
    // listeners and the speaker resolved (slot 7), the voice line name, +0x28 = 0x4a39 and
    // Field_0x8c = 1.
    virtual void PrepareConversation(FRmConversation* Conversation, FRmRuntimeMachine* Machine,
                                     FContentPlayerState* Player);
    // Slot 7, the branch (RET 0x10): Machine's ResolveLine (its slot 11).
    virtual bool ResolveLine(bool bSpeaker, FRmLine* Line, FRmRuntimeMachine* Machine,
                             FContentPlayerState* Player);

    bool bActive = false;                       // +0x0c set by the filter that fills it
    // +0x10: the comm sequence conversations (the routine appends to the vignette offer's
    // General component); the dtor runs each element's dtor (the routine 06f84eb8).
    std::list<FRmConversation> Conversations;
    FRmObjectiveMap Objectives;                 // +0x1c
};

// One loot object of a vignette (baseline 0x108 bytes: an FMissionLoot and three dwords; the
// dtor the routine is the FMissionLoot one -- only the +0xec label list needs freeing).
// Built by the routine (06f97869-06f97894: the routine / the routine).
struct FRmLootSpawn : public FMissionLoot
{
    // +0xfc: 2 marks the critical loot (the routine and the branch count its +0x94 units
    // and take its +0x104). TODO: the other values.
    int Kind = 0;
    int Field_0x100 = 0;                        // +0x100 TODO: meaning
    // +0x104: the critical loot record (the branch appends its +0x08 Name as a loot).
    FRmCriticalLoot const* CriticalLoot = nullptr;
};

// One ship of a vignette ship group (baseline 0xf0 bytes, vtable 06fbc7a0: [0] scalar deleting
// dtor the routine / dtor the routine). ctor the routine: the FMissionShip part (vtable
// first), the [NPC] block (the routine), +0xd4 = 0, +0xd8 = 2, +0xdc = 1, the loot
// vector empty. The dtor frees the loot, then the FMissionShip members.
class FRmShipSpawn : public FMissionShip
{
public:
    FRmShipSpawn();
    ~FRmShipSpawn() override;

    FMsnNpc Npc;                                // +0x88
    // +0xd4: the vignette offer's TargetName (+0x1e0) on the target ship, else null. Writer
    // 06f97852-06f97860 (first ship of a block with +0xdc == 2), whose argument is
    // &offer+0x1e0 only on the the routine path (06f9c3d2). the routine keeps the name
    // reserved while one ship carries it.
    FNpcNameParams* TargetName = nullptr;
    int Field_0xd8 = 2;                         // +0xd8 TODO: meaning
    int Field_0xdc = 1;                         // +0xdc TODO: meaning
    std::vector<FRmLootSpawn> Loot;             // +0xe0 (alloc byte) / +0xe4..+0xec
};

// One ship group of a vignette (baseline 0x98 bytes, ctor the routine, dtor the routine =
// the ship vector). Filled by the routine from a ship block and the routine.
struct FRmShipGroup
{
    FRmShipGroup();

    unsigned char Field_0x00 = 1;               // +0x00 ship block +0x00. TODO: meaning
    unsigned char Field_0x01 = 0;               // +0x01 TODO: meaning (read by the routine)
    unsigned char Field_0x02 = 0;               // +0x02 ship block +0x03. TODO: meaning
    int Field_0x04 = 0;                         // +0x04 ship block +0x18. TODO: meaning
    int Field_0x08 = -1;                        // +0x08 ship block +0x1c. TODO: meaning
    FSystemInfo* System = nullptr;              // +0x0c FindSystemInfo(query DestSys)
    FMatrix Orientation;                        // +0x10 identity
    Vector Position;                            // +0x34 the objective position
    float Field_0x40 = -2.0f;                   // +0x40 the field; 0.9 / -0.9. TODO: meaning
    TString<0x30> Name;                         // +0x44 (the routine builds it)
    int Field_0x78 = 0;                         // +0x78 TODO: meaning
    int Field_0x7c = -1;                        // +0x7c TODO: meaning
    ID_String Formation;                        // +0x80 CreateID("transport_helion")
    std::vector<FRmShipSpawn> Ships;            // +0x84 (alloc byte) / +0x88..+0x90
    unsigned int Serial;                        // +0x94 ++the field
};

// TU61 -- the field: the serial of the last ship group built (the routine).
extern unsigned int GRmShipGroupSerial;

// One solar of a vignette (baseline 0x500 bytes, ctor the routine: the FMissionSolar part as
// the [MsnSolar] template, then the fields below). Built by the routine. +0x4f8..+0x4ff are
// never written.
struct FRmSolarSpawn : public FMissionSolar
{
    FRmSolarSpawn() = default;

    // +0x4c8: the solar's attitude towards the players (the field -2.0 = none; the routine
    // -0.9). the routine sets it with pub::Reputation::SetAttitude and keeps it in the
    // record's Attitude.
    float Field_0x4c8 = -2.0f;
    FRmKillableSolar const* Arch = nullptr;     // +0x4cc KillableSolars entry
    int SolarType = -1;                         // +0x4d0 GRmSolarTypes value
    unsigned int DestSys = 0;                   // +0x4d4 the mission system
    // +0x4d8: 1 by default; the routine writes 2 for a solar the player must kill ("pk_"
    // flags), 0 otherwise. the routine: bit 0 makes the solar invincible; the value becomes
    // the record's Flags. TODO: exact meaning of 2.
    int ModeFlag = 1;
    // +0x4dc: SetInvincible's duration when ModeFlag bit 0 is set (the routine 06f92c3f, a
    // float pushed as a dword).
    float InvincibleDuration = 0.0f;
    // +0x4e0: SetInvincible's third argument (the routine 06f92c4e). TODO: meaning.
    unsigned char Field_0x4e0 = 0;
    int Hitpoints = -1;                         // +0x4e4
    std::vector<FRmLootSpawn> Loot;             // +0x4e8 (alloc byte) / +0x4ec..+0x4f4
};

// TU61 -- ship component (baseline 0x50 bytes, vtable 06fbc89c: [0] the routine / dtor
// , [7] the branch, the rest the component's). The vignette offer's +0x190:
// the hostile and friendly ship groups (the routine).
class FRmShipComponent : public FRmOfferComponent
{
public:
    FRmShipComponent() = default;               // inline at 06f88363-06f88386
    ~FRmShipComponent() override;

    // Slot 7, the branch (RET 0x10): listeners PLAYERS_IN_RANGE (8) / PLAYERS_OUT_OF_RANGE (9)
    // around Player's ship; everything else is the base slot's machine call.
    bool ResolveLine(bool bSpeaker, FRmLine* Line, FRmRuntimeMachine* Machine,
                     FContentPlayerState* Player) override;

    std::vector<FRmShipGroup> Groups;          // +0x34 (alloc byte) / +0x38..+0x40
    // +0x44: the objective position (06f9a7b0). Neither inline ctor writes it (06f882d1-06f88303,
    // heap garbage in baseline); the port starts it at 0.
    Vector Position = {};
};

// TU61 -- target ship component (baseline 0x58 bytes, vtable 06fbc8dc with every slot of
// , the dtor the routine included). Derives from the ship component: the layout is
// the same up to 0x50 and TU63 the routine takes an FRmShipComponent* for it (the routine
// / 06f9c2c8 / 06f9c3e5 pass &offer+0xec); the inline ctor 06f882d1-06f88303 elides
// the intermediate 06fbc89c store. The vignette offer's +0xec: the assassination target's
// group (the routine).
class FRmTargetShipComponent : public FRmShipComponent
{
public:
    FRmTargetShipComponent() = default;         // inline at 06f882d1-06f88303
    ~FRmTargetShipComponent() override;         // (ICF)

    int Field_0x50 = 0;                         // +0x50 TODO: meaning
    int Field_0x54 = -1;                        // +0x54 TODO: meaning
};

// TU61 -- solar component (baseline 0x4c bytes, vtable 06fbc8bc: [0] the routine / dtor
// , [7] the branch). The vignette offer's +0x144 (the routine).
class FRmSolarComponent : public FRmOfferComponent
{
public:
    FRmSolarComponent() = default;              // inline at 06f88306-06f8835d
    ~FRmSolarComponent() override;

    // Slot 7, the branch (RET 0x10): the ship component's slot with two differences: range
    // 8 keeps squared distance <= 1e8 (not <), and without Player (range 8 or 9) the machine
    // is asked for the listeners (bSpeaker false; the ship slot passes true).
    bool ResolveLine(bool bSpeaker, FRmLine* Line, FRmRuntimeMachine* Machine,
                     FContentPlayerState* Player) override;

    // +0x34: the objective position. The inline ctor (06f88306-06f8835d) does not write it
    // (heap garbage in baseline); the port starts it at 0.
    Vector Position = {};
    std::list<FRmSolarSpawn> Solars;           // +0x40 (alloc byte) / +0x44 head, +0x48 size
};

// ---------------------------------------------------------------------------------------
// TU61 -- vignette offer (baseline 0x218 bytes). Primary vtable 06fbc898 ([0] GetTextParam
// ); the ref-counted interface at +0x04 has vtable 06fbc88c (AddRef, Release,
// deleting dtor the routine = the routine + free of this - 4).
// ---------------------------------------------------------------------------------------
class FRmVignetteOffer : public FRmOffer
{
public:
    // (RET 4): the base offer, the four components, the target name.
    explicit FRmVignetteOffer(int InMachineType);
    // ReleaseTargetName (the routine), then the members in reverse order.
    ~FRmVignetteOffer() override;

    // (RET 8): the base offer's parameters first (the routine), then
    // BIG_SOLAR, OTHER_SOLAR, CRITICAL_LOOT, TARGET_FULL_NAME and TARGET_ZONE.
    bool GetTextParam(int Param, FmtStr* Out) override;

    // +0xb8: the comm sequences of the path (the routine fills Conversations; the routine
    // activates it). TODO: any other role.
    FRmOfferComponent General;
    FRmTargetShipComponent TargetShips;         // +0xec
    FRmSolarComponent Solars;                   // +0x144
    FRmShipComponent Ships;                     // +0x190
    FNpcNameParams TargetName;                  // +0x1e0 (the routine: the target's name)

private:
    // the target's name goes back to its faction's name pools unless a ship
    // still carries it.
    void ReleaseTargetName();
    // (thiscall, RET 0xc; this is not read): over Groups' ships, each critical
    // loot (Kind 2) sets *OutLoot and adds its units to *InOutCount; a ship's loot scan stops
    // once the count exceeds 1.
    void CountCriticalLoot(std::vector<FRmShipGroup> const* Groups,
                           FRmCriticalLoot const** OutLoot, int* InOutCount) const;
};
