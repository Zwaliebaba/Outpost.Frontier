// Author: Giperion.

#include "MissionScript.h"
#include "ContentStaticIds.h"   // TU21's 16 static-init thunks 06eb7360-06eb7540
#include "ContentDefaultId.h"   // TU21's "default" thunk 06eb7560
#include "BaseDB.h"
#include "ContentInternal.h"
#include "ContentPlayerState.h"
#include "Encounters.h"
#include "MissionAct.h"
#include "MissionCnd.h"
#include "MissionDialogs.h"
#include "MissionObjList.h"     // CreateObjListDirective, FObjListAvoidance (the routine)
#include "RefHandle.h"          // TRefHandle
#include "BehaviorCmds.h"       // FContentShipBase::SetDirectives
#include "DockQueues.h"
#include "NpcShips.h"
#include "PilotPersonalities.h" // GetPilotPersonality (the routine)
#include "Populators.h"
#include "RandomMissionDB.h"
#include "RtcScripts.h"
#include "SystemInfo.h"
#include "common_goods.h"
#include "common_id.h"
#include "common_iface.h"
#include "common_ini.h"
#include "common_server.h"
#include "common_server_controller.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string.h>
#include <windows.h>

I3DMathEngine* GTriggerMathEngine = nullptr;

// (see MissionScript.h).
unsigned char GMissionScanClamp = 0;

// stored 0 by the TU21 ctor (06eb7799); the listing has no reader (XREF[1] W).
unsigned int GMissionScriptDat06fcf018 = 0;

namespace
{
    int const MissionStateFail = G_MissionStateFail;
    int const MissionStateSucceed = G_MissionStateSucceed;
    int const MissionStateNoState = G_MissionStateNoState;
}

// =======================================================================================
// baseline trigger engine (TU21). See MissionScript.h.
// =======================================================================================

namespace
{
    template <class T>
    Cnd* NewTriggerCondition(FMissionScript* Mission)
    {
        return new T(Mission);
    }

    template <class T>
    Act* NewTriggerAction(FMissionScript* Mission)
    {
        return new T(Mission);
    }

    // One Cnd_* key of the routine: the class and the message ids pushed into trigger +0x18
    // after Parse (MOV [ESP+0x10],id before the shared tails the branch / the branch /
    // / the branch, or the inline the routine / the routine pushes).
    // The controller message ids (common_server_controller.h), used throughout this file.
    using pub::Controller::EMessage;

    struct FCndFactoryEntry
    {
        char const* Name;
        Cnd* (*Create)(FMissionScript* Mission);
        int EventCount;
        EMessage EventIds[3];   // past EventCount: unused (value-initialized)
    };

    // In the order of the is_value chain (06ebb890-06ebce72).
    FCndFactoryEntry const GCndFactory[] = {
        {"Cnd_BaseEnter", &NewTriggerCondition<CndBaseEnter>, 1, {EMessage::BaseEnter}},
        {"Cnd_BaseExit", &NewTriggerCondition<CndBaseExit>, 1, {EMessage::BaseExit}},
        {"Cnd_CargoScanned", &NewTriggerCondition<CndCargoScanned>, 1, {EMessage::CargoScanned}},
        {"Cnd_CharSelect", &NewTriggerCondition<CndCharSelect>, 1, {EMessage::GFObjSelect}},
        {"Cnd_CommComplete", &NewTriggerCondition<CndCommComplete>, 1, {EMessage::CommComplete}},
        {"Cnd_CmpToPlane", &NewTriggerCondition<CndCmpToPlane>, 1, {EMessage::Heartbeat}},
        {"Cnd_Destroyed", &NewTriggerCondition<CndDestroyed>, 1, {EMessage::ObjectDestroyed}},
        {"Cnd_DistCircle", &NewTriggerCondition<CndDistCircle>, 1, {EMessage::Heartbeat}},
        {"Cnd_DistShip", &NewTriggerCondition<CndDistShip>, 1, {EMessage::Heartbeat}},
        {"Cnd_DistVec", &NewTriggerCondition<CndDistVec>, 1, {EMessage::Heartbeat}},
        {"Cnd_DistVecLbl", &NewTriggerCondition<CndDistVecLbl>, 1, {EMessage::Heartbeat}},
        {"Cnd_EncLaunched", &NewTriggerCondition<CndEncLaunched>, 1, {EMessage::EncounterLaunched}},
        {"Cnd_HasMsn", &NewTriggerCondition<CndHasMsn>, 1, {EMessage::Heartbeat}},
        {"Cnd_HealthDec", &NewTriggerCondition<CndHealthDec>, 1, {EMessage::ObjectDamaged}},
        {"Cnd_InSpace", &NewTriggerCondition<CndInSpace>, 1, {EMessage::Heartbeat}},
        {"Cnd_InTradelane", &NewTriggerCondition<CndInTradelane>, 1, {EMessage::Heartbeat}},
        {"Cnd_InZone", &NewTriggerCondition<CndInZone>, 1, {EMessage::Heartbeat}},
        {"Cnd_JumpgateAct", &NewTriggerCondition<CndJumpgateAct>, 1, {EMessage::JumpgateAct}},
        {"Cnd_JumpInComplete", &NewTriggerCondition<CndJumpInComplete>, 1, {EMessage::JumpInComplete}},
        {"Cnd_LaunchComplete", &NewTriggerCondition<CndLaunchComplete>, 1, {EMessage::LaunchComplete}},
        {"Cnd_LocEnter", &NewTriggerCondition<CndLocEnter>, 1, {EMessage::LocationEnter}},
        {"Cnd_LocExit", &NewTriggerCondition<CndLocExit>, 1, {EMessage::LocationExit}},
        {"Cnd_LootAcquired", &NewTriggerCondition<CndLootAcquired>, 1, {EMessage::LootAcquired}},
        {"Cnd_MsnResponse", &NewTriggerCondition<CndMsnResponse>, 1, {EMessage::MissionResponse}},
        {"Cnd_NPCSystemEnter", &NewTriggerCondition<CndNpcSystemEnter>, 3,
         {EMessage::NpcSystemEnter, EMessage::Heartbeat, EMessage::NpcShipCreated}},
        {"Cnd_NPCSystemExit", &NewTriggerCondition<CndNpcSystemExit>, 1, {EMessage::NpcSystemExit}},
        {"Cnd_PlayerLaunch", &NewTriggerCondition<CndPlayerLaunch>, 1, {EMessage::PlayerLaunch}},
        {"Cnd_PlayerManeuver", &NewTriggerCondition<CndPlayerManeuver>, 1, {EMessage::PlayerManeuver}},
        {"Cnd_PopUpDialog", &NewTriggerCondition<CndPopUpDialog>, 1, {EMessage::PopUpDialog}},
        {"Cnd_ProjHit", &NewTriggerCondition<CndProjHit>, 1, {EMessage::ObjectDamaged}},
        {"Cnd_ProjHitShipToLbl", &NewTriggerCondition<CndProjHitShipToLbl>, 1, {EMessage::ObjectDamaged}},
        {"Cnd_RTCDone", &NewTriggerCondition<CndRtcDone>, 1, {EMessage::RtcDone}},
        {"Cnd_RumorHeard", &NewTriggerCondition<CndRumorHeard>, 1, {EMessage::RumorHeard}},
        {"Cnd_SpaceEnter", &NewTriggerCondition<CndSpaceEnter>, 1, {EMessage::SpaceEnter}},
        {"Cnd_SpaceExit", &NewTriggerCondition<CndSpaceExit>, 1, {EMessage::SpaceExit}},
        {"Cnd_SystemEnter", &NewTriggerCondition<CndSystemEnter>, 1, {EMessage::SystemEnter}},
        {"Cnd_SystemExit", &NewTriggerCondition<CndSystemExit>, 1, {EMessage::SystemExit}},
        {"Cnd_TetherBroke", &NewTriggerCondition<CndTetherBroke>, 1, {EMessage::TetherObject}},
        {"Cnd_Timer", &NewTriggerCondition<CndTimer>, 1, {EMessage::Heartbeat}},
        {"Cnd_TLEntered", &NewTriggerCondition<CndTlEntered>, 1, {EMessage::TradelaneEnter}},
        {"Cnd_TLExited", &NewTriggerCondition<CndTlExited>, 1, {EMessage::TradelaneExit}},
        {"Cnd_True", &NewTriggerCondition<CndTrue>, 1, {EMessage::Heartbeat}},
        {"Cnd_WatchTrigger", &NewTriggerCondition<CndWatchTrigger>, 1, {EMessage::Heartbeat}},
        {"Cnd_WatchVibe", &NewTriggerCondition<CndWatchVibe>, 1, {EMessage::Heartbeat}},
    };

    // One Act_* key of the routine (new, ctor, Parse, push_back into trigger +0x08 with
    // , the routine with trigger +0x2c).
    struct FActFactoryEntry
    {
        char const* Name;
        Act* (*Create)(FMissionScript* Mission);
    };

    // In the order of the is_value chain (06ebceea-06ebf59c).
    FActFactoryEntry const GActFactory[] = {
        {"Act_ActTrig", &NewTriggerAction<ActActTrig>},
        {"Act_AddAmbient", &NewTriggerAction<ActAddAmbient>},
        {"Act_AddRTC", &NewTriggerAction<ActAddRtc>},
        {"Act_AdjAcct", &NewTriggerAction<ActAdjAcct>},
        {"Act_AdjHealth", &NewTriggerAction<ActAdjHealth>},
        {"Act_CallSceneScript", &NewTriggerAction<ActCallSceneScript>},
        {"Act_ChangeState", &NewTriggerAction<ActChangeState>},
        {"Act_Cloak", &NewTriggerAction<ActCloak>},
        {"Act_DeactTrig", &NewTriggerAction<ActDeactTrig>},
        {"Act_DebugMsg", &NewTriggerAction<ActDebugMsg>},
        {"Act_Destroy", &NewTriggerAction<ActDestroy>},
        {"Act_DisableEnc", &NewTriggerAction<ActDisableEnc>},
        {"Act_DisableFriendlyFire", &NewTriggerAction<ActDisableFriendlyFire>},
        {"Act_DisableTradelane", &NewTriggerAction<ActDisableTradelane>},
        {"Act_DockRequest", &NewTriggerAction<ActDockRequest>},
        {"Act_EnableEnc", &NewTriggerAction<ActEnableEnc>},
        {"Act_EnableManeuver", &NewTriggerAction<ActEnableManeuver>},
        {"Act_EtherComm", &NewTriggerAction<ActEtherComm>},
        {"Act_ForceLand", &NewTriggerAction<ActForceLand>},
        {"Act_GCSClamp", &NewTriggerAction<ActGcsClamp>},
        {"Act_GiveMB", &NewTriggerAction<ActGiveMb>},
        {"Act_GiveNNObjs", &NewTriggerAction<ActGiveNnObjs>},
        {"Act_GiveObjList", &NewTriggerAction<ActGiveObjList>},
        {"Act_HostileClamp", &NewTriggerAction<ActHostileClamp>},
        {"Act_Invulnerable", &NewTriggerAction<ActInvulnerable>},
        {"Act_Jumper", &NewTriggerAction<ActJumper>},
        {"Act_LightFuse", &NewTriggerAction<ActLightFuse>},
        {"Act_LockDock", &NewTriggerAction<ActLockDock>},
        {"Act_LockManeuvers", &NewTriggerAction<ActLockManeuvers>},
        {"Act_MarkObj", &NewTriggerAction<ActMarkObj>},
        {"Act_MovePlayer", &NewTriggerAction<ActMovePlayer>},
        {"Act_NagClamp", &NewTriggerAction<ActNagClamp>},
        {"Act_NagDistLeaving", &NewTriggerAction<ActNagDistLeaving>},
        {"Act_NagDistTowards", &NewTriggerAction<ActNagDistTowards>},
        {"Act_NagGreet", &NewTriggerAction<ActNagGreet>},
        {"Act_NagOff", &NewTriggerAction<ActNagOff>},
        {"Act_NNPath", &NewTriggerAction<ActNnPath>},
        {"Act_NNIds", &NewTriggerAction<ActNnIds>},
        {"Act_PlayerCanDock", &NewTriggerAction<ActPlayerCanDock>},
        {"Act_PlayerCanTradelane", &NewTriggerAction<ActPlayerCanTradelane>},
        {"Act_PlayerEnemyClamp", &NewTriggerAction<ActPlayerEnemyClamp>},
        {"Act_PlayerForm", &NewTriggerAction<ActPlayerForm>},
        {"Act_PlayMusic", &NewTriggerAction<ActPlayMusic>},
        {"Act_PlayNN", &NewTriggerAction<ActPlayNn>},
        {"Act_PlaySoundEffect", &NewTriggerAction<ActPlaySoundEffect>},
        {"Act_PilotParams", &NewTriggerAction<ActPilotParams>},
        {"Act_PobjIdle", &NewTriggerAction<ActPobjIdle>},
        {"Act_PopUpDialog", &NewTriggerAction<ActPopUpDialog>},
        {"Act_SetPriority", &NewTriggerAction<ActSetPriority>},
        {"Act_RandomPop", &NewTriggerAction<ActRandomPop>},
        {"Act_RandomPopSphere", &NewTriggerAction<ActRandomPopSphere>},
        {"Act_RelocateForm", &NewTriggerAction<ActRelocateForm>},
        {"Act_RelocateShip", &NewTriggerAction<ActRelocateShip>},
        {"Act_RemoveAmbient", &NewTriggerAction<ActRemoveAmbient>},
        {"Act_RemoveCargo", &NewTriggerAction<ActRemoveCargo>},
        {"Act_RemoveRTC", &NewTriggerAction<ActRemoveRtc>},
        {"Act_RepChangeRequest", &NewTriggerAction<ActRepChangeRequest>},
        {"Act_RevertCam", &NewTriggerAction<ActRevertCam>},
        {"Act_RpopAttClamp", &NewTriggerAction<ActRpopAttClamp>},
        {"Act_RpopTLAttacksEnabled", &NewTriggerAction<ActRpopTlAttacksEnabled>},
        {"Act_Save", &NewTriggerAction<ActSave>},
        {"Act_SendComm", &NewTriggerAction<ActSendComm>},
        {"Act_SetFlee", &NewTriggerAction<ActSetFlee>},
        {"Act_SetInitialPlayerPos", &NewTriggerAction<ActSetInitialPlayerPos>},
        {"Act_SetLifeTime", &NewTriggerAction<ActSetLifeTime>},
        {"Act_SetNNHidden", &NewTriggerAction<ActSetNnHidden>},
        {"Act_SetNNObj", &NewTriggerAction<ActSetNnObj>},
        {"Act_SetNNState", &NewTriggerAction<ActSetNnState>},
        {"Act_SetOffer", &NewTriggerAction<ActSetOffer>},
        {"Act_SetOrient", &NewTriggerAction<ActSetOrient>},
        {"Act_SetRep", &NewTriggerAction<ActSetRep>},
        {"Act_SetShipAndLoadout", &NewTriggerAction<ActSetShipAndLoadout>},
        {"Act_SetTitle", &NewTriggerAction<ActSetTitle>},
        {"Act_SetVibe", &NewTriggerAction<ActSetVibe>},
        {"Act_SetVibeLbl", &NewTriggerAction<ActSetVibeLbl>},
        {"Act_SetVibeLblToShip", &NewTriggerAction<ActSetVibeLblToShip>},
        {"Act_SetVibeShipToLbl", &NewTriggerAction<ActSetVibeShipToLbl>},
        {"Act_SetVibeOfferBaseHack", &NewTriggerAction<ActSetVibeOfferBaseHack>},
        {"Act_SpawnFormation", &NewTriggerAction<ActSpawnFormation>},
        {"Act_SpawnLoot", &NewTriggerAction<ActSpawnLoot>},
        {"Act_SpawnShip", &NewTriggerAction<ActSpawnShip>},
        {"Act_SpawnShipRel", &NewTriggerAction<ActSpawnShipRel>},
        {"Act_SpawnSolar", &NewTriggerAction<ActSpawnSolar>},
        {"Act_StartDialog", &NewTriggerAction<ActStartDialog>},
        {"Act_StaticCam", &NewTriggerAction<ActStaticCam>},
    };
}

// (thiscall on the trigger, RET 4). Every value of the rest of the [Trigger]
// section: a Cnd_* key replaces +0x04 (the previous condition is not deleted), is parsed,
// pushes its message ids into +0x18 and gets the trigger id (+0x2c) at +0x08; an Act_* key is
// parsed, appended to +0x08 and gets the trigger id; "InitState" = ACTIVE 0 / INACTIVE 1 /
// COMPLETE 2 (stricmp, anything else leaves the state); "repeatable" true sets +0x28 (false
// leaves it); "system" is tested and ignored.
void LoadTrigger(FTrigger* Trigger, INI_Reader* Reader)
{
    while (Reader->read_value())
    {
        bool bMatched = false;
        for (int Index = 0; Index < static_cast<int>(sizeof(GCndFactory) / sizeof(GCndFactory[0]));
             ++Index)
        {
            FCndFactoryEntry const& Entry = GCndFactory[Index];
            if (!Reader->is_value(Entry.Name))
                continue;
            Cnd* const Condition = Entry.Create(Trigger->Mission);
            Trigger->Condition = Condition;
            Condition->Parse(*Reader);
            for (int EventIndex = 0; EventIndex < Entry.EventCount; ++EventIndex)
                Trigger->EventIds.push_back(Entry.EventIds[EventIndex]);
            Trigger->Condition->SetTriggerId(Trigger->Id);
            bMatched = true;
            break;
        }
        if (bMatched)
            continue;
        for (int Index = 0; Index < static_cast<int>(sizeof(GActFactory) / sizeof(GActFactory[0]));
             ++Index)
        {
            FActFactoryEntry const& Entry = GActFactory[Index];
            if (!Reader->is_value(Entry.Name))
                continue;
            Act* const Action = Entry.Create(Trigger->Mission);
            Action->Parse(*Reader);
            Trigger->Actions.push_back(Action);
            Action->SetTriggerId(Trigger->Id);
            bMatched = true;
            break;
        }
        if (bMatched)
            continue;
        if (Reader->is_value("InitState"))
        {
            char const* const StateName = Reader->get_value_string(0);
            if (_stricmp("ACTIVE", StateName) == 0)
                Trigger->State = G_TriggerStateActive;
            else if (_stricmp("INACTIVE", StateName) == 0)
                Trigger->State = G_TriggerStateInactive;
            else if (_stricmp("COMPLETE", StateName) == 0)
                Trigger->State = G_TriggerStateComplete;
            continue;
        }
        if (Reader->is_value("repeatable"))
        {
            if (Reader->get_value_bool(0))
                Trigger->bRepeatable = 1;
            continue;
        }
        // is_value("system"); the result is not used.
        Reader->is_value("system");
    }
}

// [Trigger] branch (06eb8527-06eb86f0), after is_header("Trigger").
void LoadMissionTriggerSection(FMissionScript* Script, INI_Reader* Reader)
{
    // the local ID_String, cleared.
    unsigned int TriggerId = 0;
    while (Reader->read_value())
    {
        if (Reader->is_value("nickname"))
        {
            TriggerId = CreateID(Reader->get_value_string(0));
            // the routine (find); a known nickname is skipped.
            if (Script->TriggerMap.find(TriggerId) != Script->TriggerMap.end())
                continue;
            // the stack trigger (State INACTIVE, no condition, empty
            // lists, mission = this, repeatable 0, id).
            FTrigger Trigger;
            Trigger.State = G_TriggerStateInactive;
            Trigger.Condition = nullptr;
            Trigger.Mission = Script;
            Trigger.bRepeatable = 0;
            Trigger.Id = TriggerId;
            LoadTrigger(&Trigger, Reader);
            // {id, copy} (the routine) inserted into +0x1b0 (the routine);
            // the copies share the condition and action pointers.
            Script->TriggerMap.insert(FTriggerMap::value_type(TriggerId, Trigger));
            // an InitState ACTIVE trigger also goes into +0x1e4 as {id, id}.
            if (Trigger.State == G_TriggerStateActive)
                Script->InitiallyActive.insert(
                    std::map<unsigned int, unsigned int>::value_type(TriggerId, TriggerId));
            continue;
        }
        // "State" goes through vtbl+0x68 and the result is dropped.
        if (Reader->is_value("State"))
            static_cast<void>(Script->MissionStateFromName(Reader->get_value_string(0)));
    }
}

// .
void RearmTrigger(FTrigger* Trigger)
{
    Trigger->State = G_TriggerStateActive;
    if (Trigger->Condition != nullptr)
        Trigger->Condition->OnActivate();
}

// vtbl[0](1) on +0x04 when set, then on every non-null list value.
void DeleteTriggerContents(FTrigger* Trigger)
{
    if (Trigger->Condition != nullptr)
        delete Trigger->Condition;
    std::list<Act*>::iterator It = Trigger->Actions.begin();
    for (; It != Trigger->Actions.end(); ++It)
    {
        if (*It != nullptr)
            delete *It;
    }
}

// .
void ActivateMissionTrigger(FMissionScript* Script, unsigned int const* TriggerId)
{
    FTriggerMap::iterator Found = Script->TriggerMap.find(*TriggerId);
    if (Found == Script->TriggerMap.end())
        return;

    // the whole active list is walked (no early exit); any node whose
    // key equals the id means the trigger is already active.
    bool bAlreadyActive = false;
    std::list<FTriggerMap::iterator>::iterator ActiveIt = Script->ActiveTriggers.begin();
    for (; ActiveIt != Script->ActiveTriggers.end(); ++ActiveIt)
    {
        if ((*ActiveIt)->first == *TriggerId)
            bAlreadyActive = true;
    }
    if (bAlreadyActive)
        return;

    FTrigger& Trigger = Found->second;
    Trigger.State = G_TriggerStateActive;
    if (Trigger.Condition != nullptr)
        Trigger.Condition->OnActivate();

    // re-read State; the condition is called without a null check (-1, 0). -1 is
    // no message id: the activation probe, cast into the message-id type Matches takes.
    if (Trigger.State == G_TriggerStateActive &&
        Trigger.Condition->Matches(static_cast<EMessage>(-1), nullptr))
    {
        Trigger.State = G_TriggerStateComplete;
        std::list<Act*>::iterator ActIt = Trigger.Actions.begin();
        for (; ActIt != Trigger.Actions.end(); ++ActIt)
            (*ActIt)->Execute();
        // inline copy of the routine.
        if (Trigger.bRepeatable != 0)
        {
            Trigger.State = G_TriggerStateActive;
            if (Trigger.Condition != nullptr)
                Trigger.Condition->OnActivate();
        }
    }

    // push_back the map node unless the trigger completed.
    if (Trigger.State != G_TriggerStateComplete)
        Script->ActiveTriggers.push_back(Found);
}

// .
void DeactivateMissionTrigger(FMissionScript* Script, unsigned int const* TriggerId)
{
    // first active node with this key whose trigger is not COMPLETE:
    // State = INACTIVE, node erased (--size).
    std::list<FTriggerMap::iterator>::iterator ActiveIt = Script->ActiveTriggers.begin();
    for (; ActiveIt != Script->ActiveTriggers.end(); ++ActiveIt)
    {
        FTrigger& Trigger = (*ActiveIt)->second;
        if ((*ActiveIt)->first == *TriggerId && Trigger.State != G_TriggerStateComplete)
        {
            Trigger.State = G_TriggerStateInactive;
            Script->ActiveTriggers.erase(ActiveIt);
            break;
        }
    }

    // erase every pending activation keyed by this id.
    std::map<unsigned int, unsigned int>::iterator PendingIt = Script->PendingActivations.begin();
    while (PendingIt != Script->PendingActivations.end())
    {
        if (PendingIt->first == *TriggerId)
            PendingIt = Script->PendingActivations.erase(PendingIt);
        else
            ++PendingIt;
    }
}

// . *EventId is the message id the caller (the routine) stored in its local.
void DispatchMissionTriggerEvent(FMissionScript* Script, EMessage const* EventId,
                                 void const* EventParms)
{
    std::list<FTriggerMap::iterator>::iterator ActiveIt = Script->ActiveTriggers.begin();
    while (ActiveIt != Script->ActiveTriggers.end())
    {
        FTrigger& Trigger = (*ActiveIt)->second;

        // linear scan of the trigger's event vector.
        bool bListens = false;
        std::vector<EMessage>::iterator EventIt = Trigger.EventIds.begin();
        for (; EventIt != Trigger.EventIds.end(); ++EventIt)
        {
            if (*EventIt == *EventId)
            {
                bListens = true;
                break;
            }
        }

        // only ACTIVE triggers are evaluated; others are skipped, never erased.
        if (bListens && Trigger.State == G_TriggerStateActive)
        {
            if (Trigger.Condition->Matches(*EventId, EventParms))
            {
                Trigger.State = G_TriggerStateComplete;
                std::list<Act*>::iterator ActIt = Trigger.Actions.begin();
                for (; ActIt != Trigger.Actions.end(); ++ActIt)
                    (*ActIt)->Execute();
                if (Trigger.bRepeatable != 0)
                    RearmTrigger(&Trigger);
            }
            // a completed trigger leaves the active list.
            if (Trigger.State == G_TriggerStateComplete)
            {
                ActiveIt = Script->ActiveTriggers.erase(ActiveIt);
                continue;
            }
        }
        ++ActiveIt;
    }

    // drain the pending map front to back. baseline also looks the
    // source id (node+0x10) up in the trigger map (the routine @ 06eb9b63) and discards the
    // result; that lookup has no side effect and is not reproduced.
    while (!Script->PendingActivations.empty())
    {
        std::map<unsigned int, unsigned int>::iterator First = Script->PendingActivations.begin();
        unsigned int TargetId = First->first;
        Script->PendingActivations.erase(First);
        ActivateMissionTrigger(Script, &TargetId);
    }
    // the routine erase(begin, end).
    Script->PendingActivations.clear();
}

// .
void TickMissionTimers(FMissionScript* Script, float DeltaSeconds)
{
    std::list<FMissionTimer>::iterator It = Script->Timers.begin();
    while (It != Script->Timers.end())
    {
        It->Remaining = It->Remaining - DeltaSeconds;
        // FCOMP the field (0.0) / TEST AH,0x41 / JP skip: runs when Remaining <= 0.
        if (It->Remaining <= 0.0f)
        {
            if (It->Action != nullptr)
                It->Action->Execute();
            It = Script->Timers.erase(It);
            continue;
        }
        ++It;
    }
}

// stricmp against the field "FAIL", "SUCCEED", "NO_STATE"; miss -> EBX = -1.
int FMissionScript::MissionStateFromName(char const* Name)
{
    if (_stricmp(Name, "FAIL") == 0)
        return MissionStateFail;
    if (_stricmp(Name, "SUCCEED") == 0)
        return MissionStateSucceed;
    if (_stricmp(Name, "NO_STATE") == 0)
        return MissionStateNoState;
    return -1;
}

// CMP byte [+0x2b0],1 / JNZ store; TEST State / JZ skip.
void FMissionScript::SetDesiredMissionState(int State, unsigned int MadLibId)
{
    if (Field_0x2b0 == 1 && State == MissionStateFail)
        return;
    MissionState = State;
    Field_0x2e8 = MadLibId;
}

// case 0x1000 (06eb94ec-06eb9541): in-order walk of mission+0x1e4, activating
// each key.
void ActivateInitialMissionTriggers(FMissionScript* Script)
{
    std::map<unsigned int, unsigned int>::iterator It = Script->InitiallyActive.begin();
    for (; It != Script->InitiallyActive.end(); ++It)
        ActivateMissionTrigger(Script, &It->first);
}

namespace
{
    // The script's +0x10 (Roles._First) passed as FindCharacter's role: the first role's
    // TString at +0x00; baseline passes the raw pointer, null for an empty vector.
    TString<32> const* GetFirstRtcRole(FCharacterEncounter const* Script)
    {
        if (Script->Roles.empty())
            return nullptr;
        return &Script->Roles.front().Role;
    }
}

// .
void RemoveMissionRtc(FMissionScript* Script, TString<0x80> const* Path)
{
    std::list<FStoryMissionListNode_0x2f4>::iterator It = Script->List_0x2f4.begin();
    for (; It != Script->List_0x2f4.end(); ++It)
    {
        // the inlined TString == (node path against Path->Data).
        if (strcmp(It->Path, Path->Data) != 0)
            continue;
        if (FindContentPlayerState(Script->PlayerId) == nullptr)
            continue;
        // the player's base (the routine), the room state of the
        // mission's location (the routine on +0x308), the script (+0x90), the player gate
        // again, then FBaseRoomState::FindCharacter(script +0x10, player) and
        // FBarCharacter::SetBehavior(-1). Any miss skips to the +0x94 store.
        FContentBase* const Base = Script->PlayerState->GetBaseRecord();
        if (Base != nullptr)
        {
            FBaseRoomState* const Room = Base->FindRoomState(&Script->CurrentLocationId);
            if (Room != nullptr && It->Field_0x90 != nullptr &&
                FindContentPlayerState(Script->PlayerId) != nullptr)
            {
                FBarCharacterList::iterator Character;
                if (Room->FindCharacter(GetFirstRtcRole(It->Field_0x90), Script->PlayerId,
                                        &Character))
                {
                    Character->SetBehavior(static_cast<unsigned long>(-1));
                }
            }
        }
        It->Field_0x94 = 1;
    }
}

// .
void MoveMissionPlayer(FMissionScript* Script, Vector const* Position, float RadiusSq)
{
    if (FindContentPlayerState(Script->PlayerId) == nullptr)
        return;
    FContentPlayerShip* const Ship = Script->PlayerState->ShipObject;
    if (Ship == nullptr)
        return;
    // the routine (FContentSpaceObj::GetPosition); each delta is stored as a float.
    Vector ShipPosition;
    Ship->GetPosition(&ShipPosition);
    float const DeltaX = ShipPosition.x - Position->x;
    float const DeltaY = ShipPosition.y - Position->y;
    float const DeltaZ = ShipPosition.z - Position->z;
    // (y*y + z*z) + x*x on the FPU, FCOMP RadiusSq, TEST AH,0x41 / JNZ
    // skip: only a distance strictly beyond the radius (and not unordered) moves the ship.
    double const DistanceSq = static_cast<double>(DeltaY) * DeltaY
                              + static_cast<double>(DeltaZ) * DeltaZ
                              + static_cast<double>(DeltaX) * DeltaX;
    if (!(DistanceSq > static_cast<double>(RadiusSq)))
        return;
    // the ship's orientation (GetLocation again), the player's system
    // (the routine +0x48, no null check), then the routine(&ship, &system, Position,
    // &orientation).
    Vector CurrentPosition;
    Matrix CurrentOrientation;
    pub::SpaceObj::GetLocation(Ship->SpaceObjId, CurrentPosition, CurrentOrientation);
    unsigned int const SystemId = Script->PlayerState->GetSystemRecord()->SystemId;
    unsigned int const ShipId = Script->PlayerState->ShipObject->SpaceObjId;
    RelocateShip(&ShipId, &SystemId, Position, &CurrentOrientation);
}

// =======================================================================================
// FMissionScript (TU21 script mission, vtable 06fb4710)
// =======================================================================================

// (06eb75a0-06eb780a). The TU38 base ctor, the containers (06eb75d9-06eb7788),
// then 06eb778d-06eb77f1: vtable, +0x2b0 = 0, the field = 0, +0x2bc = 0, +0x2a8 = +0x2ac = 2,
// +0x2c8 = +0x2ec = 0, the routine(parms+4) (the slot 18 body, called directly),
// +0x2a4 = parms+8, the routine(+0xc) (the slot 2 body, called directly), +0x30c = 0,
// +0x2e8 = 1. +0x2b4/+0x2b8/+0x2c0/+0x2c4 and +0x308 are left to the mission ctors.
FMissionScript::FMissionScript(unsigned int const* InControllerId,
                               FStoryMissionCreateParms const* Parms)
    : FMissionObject(InControllerId, Parms)
    , StoryEvents(nullptr)
    , PreviousMissionState(G_MissionStateNoState)
    , MissionState(G_MissionStateNoState)
    , Field_0x2b0(0)
    , CampaignSlotCurrent(0)
    , CampaignSlotSucceed(0)
    , StoryCue(0)
    , Field_0x2c0(0)
    , Field_0x2c4(0)
    , Field_0x2c8(0)
    , bRepPopUpShown(0)
    , bRepPopUpArmed(0)
    , RepPopUpSystemId(0)
    , Field_0x2e8(1)
    , Field_0x2ec(0)
{
    GMissionScriptDat06fcf018 = 0;
    FMissionObject::SetPlayer(Parms->PlayerId);
    StoryEvents = Parms->StoryEvents;
    FMissionObject::GetOrCreatePlayer(PlayerId);
    ElapsedTime = 0.0f;
}

// (the members after the explicit loops are destroyed by their own dtors, then
// the TU38 base the routine).
FMissionScript::~FMissionScript()
{
    // the active list, then every trigger's condition and actions.
    ActiveTriggers.clear();
    FTriggerMap::iterator TriggerIt = TriggerMap.begin();
    while (TriggerIt != TriggerMap.end())
    {
        DeleteTriggerContents(&TriggerIt->second);
        TriggerIt = TriggerMap.erase(TriggerIt);
    }
    TriggerMap.clear();
    // the routine.
    ReleaseRtcScripts();
    // vtbl[0](1) on every non-null +0x204 value, erase (the routine).
    std::map<unsigned int, FMissionNag*>::iterator NagIt = Nags.begin();
    while (NagIt != Nags.end())
    {
        if (NagIt->second != nullptr)
            delete NagIt->second;
        NagIt = Nags.erase(NagIt);
    }
    // +0x2fc erase(begin, end).
    FriendlyFireOffLabels.clear();
    // every ambient scene with a handle is destroyed, then erased.
    std::list<FMissionAmbientScene>::iterator SceneIt = AmbientScenes.begin();
    for (; SceneIt != AmbientScenes.end(); ++SceneIt)
    {
        if (SceneIt->Handle != -1)
        {
            unsigned long Handle = static_cast<unsigned long>(SceneIt->Handle);
            pub::GF::AmbientScriptDestroy(&Handle);
            SceneIt->Handle = -1;
        }
    }
    AmbientScenes.clear();
}

// .
void FMissionScript::ReleaseRtcScripts()
{
    // the player's base, null without a live player state.
    FContentBase* Base = nullptr;
    if (FindContentPlayerState(PlayerId) != nullptr)
        Base = PlayerState->GetBaseRecord();
    std::list<FStoryMissionListNode_0x2f4>::iterator It = List_0x2f4.begin();
    for (; It != List_0x2f4.end(); ++It)
    {
        if (It->Field_0x90 == nullptr)
            continue;
        if (Base != nullptr && It->Field_0x8c != -1)
        {
            // the routine(Base, Field_0x8c, 0).
            Base->RemoveBarScene(It->Field_0x8c, nullptr);
            It->Field_0x8c = -1;
        }
        // the routine (the member-wise teardown) + free of the script.
        delete It->Field_0x90;
        It->Field_0x90 = nullptr;
    }
    // unlink and free every node.
    List_0x2f4.clear();
}

// (06eb8a20-06eb8b6d, thiscall, RET 4).
void FMissionScript::StartRtcScript(FStoryMissionListNode_0x2f4* Rtc)
{
    // operator new 0x54 + the routine.
    FCharacterEncounter* Script = new FCharacterEncounter();
    // the player state (the routine on +0xc) and its base (the routine).
    if (FindContentPlayerState(PlayerId) != nullptr)
    {
        FContentBase* Base = PlayerState->GetBaseRecord();
        // the routine(Rtc path), then the script's base against the
        // player's (MBase +0x48).
        if (Base != nullptr && Script->Load(Rtc->Path) && Base->BaseId == Script->BaseId)
        {
            // a mission text and no live mission (the routine & 0x3fffffff
            // zero) -> +0x2c = +0x34.
            if (Script->MissionTextId != 0 && (PlayerState->GetLiveMission() & 0x3fffffff) == 0)
                Script->MissionTextId = Script->LoadedMissionTextId;
            // +0x40 = the routine().
            Script->SequenceId = AllocateFormationId();
            // Rtc +0x84 = the routine(Base, Script, +0xc, +0x4), then
            // Rtc +0x88 = Script.
            Rtc->Field_0x8c = Base->CreateBarScene(Script, PlayerId, ControllerId);
            Rtc->Field_0x90 = Script;
            return;
        }
    }
    // the routine inlined + free.
    delete Script;
}

// (06eb8d10-06eb8eeb, thiscall, plain RET).
void FMissionScript::RevertStaticCam()
{
    // no live player -> nothing.
    if (FindContentPlayerState(PlayerId) == nullptr)
        return;
    // +0x2c8 = 0, pub::SpaceObj::SetInvincible(ship id, 0, 1, 0) (the id is
    // not masked here), +0x218 = 0.
    Field_0x2c8 = 0;
    pub::SpaceObj::SetInvincible(PlayerState->GetShipId(), false, true, 0.0f);
    NagClamp = 0;
    // the routine(npc, 0) over the +0xa4 NPC map, in order.
    std::map<unsigned int, FContentNpcShip*>::iterator NpcIt = NpcObjects.begin();
    for (; NpcIt != NpcObjects.end(); ++NpcIt)
        NpcIt->second->SetField1b1(false);
    // with a player ship object (+0x10): a local directive list holding one
    // new Avoidance directive (the routine type 0, the routine(1), pushed and AddRef'ed),
    // (&list, 0); then the local reference is released and the list's elements
    // released and erased.
    FContentPlayerShip* const Ship = PlayerState->ShipObject;
    if (Ship != nullptr)
    {
        FMissionDirectiveList Directives;
        TRefHandle<FObjListDirective> Directive;
        CreateObjListDirective(0, &Directive.Pointer);
        static_cast<FObjListAvoidance*>(Directive.Pointer)->SetAvoidance(true);
        Directives.Items.push_back(Directive.Pointer);
        Directive.Pointer->AddRef();
        Ship->SetDirectives(&Directives.Items, 0);
    }
    // the routine(0).
    SpacePopSetGcsClampEnabled(0);
}

void FMissionScript::SetChapter(int SlotCurrent, int SlotSucceed, unsigned int Value2c0,
                                unsigned int Value2c4)
{
    CampaignSlotCurrent = SlotCurrent;
    CampaignSlotSucceed = SlotSucceed;
    Field_0x2c0 = Value2c0;
    Field_0x2c4 = Value2c4;
}

void FMissionScript::LoadMissionIni(char const* IniPath)
{
    INI_Reader Reader;
    OpenContentDataIni(IniPath, &Reader);
    LoadMission(&Reader);
    Reader.close();
}

// CMP byte [+0x290] (NnPath.bActive); zero -> JMP the routine.
void FMissionScript::UpdateObjectiveTimer(EMessage MessageId, void const* MessageParms)
{
    if (NnPath.bActive != 0)
        return;
    FMissionObject::UpdateObjectiveTimer(MessageId, MessageParms);
}

// the routine(&0x100f, Arg).
void FMissionScript::Slot28(void const* Parms)
{
    EMessage EventId = EMessage::NpcSystemEnter;
    DispatchMissionTriggerEvent(this, &EventId, Parms);
}

// (06eb9aa0-06eb9aeb). Desired (+0x2ac) equal to Previous (+0x2a8) -> nothing;
// else Previous = Desired; 0 (FAIL) -> tail-jump vtbl[+0x60]; 1 (SUCCEED) -> vtbl[+0x5c], then
// StoryEvents(+0x2a4)->vtbl[0](3, +0x2b8, +4) if the facet is set (TEST ECX / JZ 06eb9ad5).
void FMissionScript::ApplyDesiredMissionState()
{
    int Desired = MissionState;
    if (Desired == PreviousMissionState)
        return;
    PreviousMissionState = Desired;
    if (Desired == G_MissionStateFail)
    {
        OnMissionFail();
        return;
    }
    if (Desired != G_MissionStateSucceed)
        return;
    OnMissionSucceed();
    if (StoryEvents != nullptr)
        StoryEvents->OnMissionEvent(3, static_cast<unsigned int>(CampaignSlotSucceed), ControllerId);
}

// (06eb9760-06eb9861): with a madlib id (+0x2e8 != 0) and a live player state:
// 1 -> 0x4cf, FmtStr madlib, DisplayMissionMessage(PlayerId, fmt, 1, true), then
// (+8, 7) and the routine(this); always the routine(this) last.
void FMissionScript::OnMissionSucceed()
{
    if (Field_0x2e8 != 0 && FindContentPlayerState(PlayerId) != nullptr)
    {
        if (Field_0x2e8 == 1)
            Field_0x2e8 = 0x4cf;
        FmtStr MissionMsg;
        MissionMsg.begin_mad_lib(Field_0x2e8);
        MissionMsg.end_mad_lib();
        pub::Player::DisplayMissionMessage(PlayerId, MissionMsg, MISSION_MSG_TYPE_1, true);
        // the routine on the cached player state (+0x08) with type 7.
        PlayerState->CountRandomMissionCompleted(7);
        DestroyMissionObjects();
    }
    FMissionObject::OnMissionSucceed();
}

// (06eb9870-06eb999d).
void FMissionScript::OnMissionFail()
{
    // madlib 1 -> 0x412.
    if (Field_0x2e8 == 1)
        Field_0x2e8 = 0x412;
    // DisplayMissionMessage(&PlayerId, madlib, type 0, true).
    FmtStr MissionMsg;
    MissionMsg.begin_mad_lib(Field_0x2e8);
    MissionMsg.end_mad_lib();
    pub::Player::DisplayMissionMessage(PlayerId, MissionMsg, MISSION_MSG_TYPE_0, true);
    // in-order walk of the +0xa4 NPC map; the routine(npc, 1).
    std::map<unsigned int, FContentNpcShip*>::iterator It = NpcObjects.begin();
    for (; It != NpcObjects.end(); ++It)
        It->second->SetField1b1(true);
    // the routine.
    DestroyMissionObjects();
    FMissionObject::OnMissionFail();
}

// .
void FMissionScript::EndMission()
{
    // vtbl[0](1) on every non-null +0x204 value, erase (the routine).
    std::map<unsigned int, FMissionNag*>::iterator NagIt = Nags.begin();
    while (NagIt != Nags.end())
    {
        if (NagIt->second != nullptr)
            delete NagIt->second;
        NagIt = Nags.erase(NagIt);
    }
    // +0x1f8 erase(begin, end) (the actions stay owned by their triggers).
    Timers.clear();
    // a player without a ship gets its maneuvers unlocked (the ship id is
    // re-read and passed, TEST EAX,0x3fffffff / JNZ skip).
    if ((PlayerState->GetShipId() & 0x3fffffff) == 0)
        pub::AI::lock_maneuvers(PlayerState->GetShipId(), false);
    // every active trigger's condition gets slot 4 (+0x10).
    std::list<FTriggerMap::iterator>::iterator ActiveIt = ActiveTriggers.begin();
    for (; ActiveIt != ActiveTriggers.end(); ++ActiveIt)
        (*ActiveIt)->second.Condition->Slot10();
    // the routine.
    ReleaseRtcScripts();
    // +0x2fc erase(begin, end).
    FriendlyFireOffLabels.clear();
    // +0x98 (the random encounters) erase(begin, end).
    RandEncs.clear();
    // erase(begin, end) of the global dialog map the field (TU28).
    GMissionDialogs.erase(GMissionDialogs.begin(), GMissionDialogs.end());
    // the SpacePop clamps back to their defaults.
    SpacePopSetAttackClamp(0);
    GMissionScanClamp = 0;
    SpacePopSetEncounterEnabled(1);
    SpacePopSetPopulationEnabled(1);
    SpacePopClearExclusionSpheres();
    SpacePopSetHostileClamp(1);
    // a live player regains docking and trade lanes, the music stops.
    if (FindContentPlayerState(PlayerId) != nullptr)
    {
        PlayerState->SetDockAccess(1, nullptr);
        PlayerState->SetTradelaneAccess(1, nullptr);
        pub::Audio::CancelMusic(PlayerId);
    }
    // +0x14c, +0x158 and +0x140 emptied.
    Invulnerables.clear();
    DecloakedNames.clear();
    MarkObjNames.clear();
    // the routine, the TU38 slot 22 body.
    FMissionObject::EndMission();
}

// (thiscall on A, RET 4).
bool IsSameCostume(Costume const* A, Costume const* B)
{
    if (A->PartIds[0] != B->PartIds[0] || A->PartIds[1] != B->PartIds[1] ||
        A->PartIds[2] != B->PartIds[2] || A->PartIds[3] != B->PartIds[3])
        return false;
    if (A->AccessoryCount != B->AccessoryCount)
        return false;
    // signed count (JLE), accessories compared up to it.
    int const Count = static_cast<int>(A->AccessoryCount);
    for (int Index = 0; Index < Count; ++Index)
    {
        if (A->AccessoryIds[Index] != B->AccessoryIds[Index])
            return false;
    }
    return true;
}

// The unique push_back into mission +0x31c inlined by the routine (06eb8836-06eb88f4) and
// the Act_* comm senders: the routine over the vector, the routine at the end when absent.
void AddDialogCostume(FMissionScript* Script, Costume const* SpeakerCostume)
{
    std::vector<Costume>::iterator It = Script->DialogCostumes.begin();
    for (; It != Script->DialogCostumes.end(); ++It)
    {
        if (IsSameCostume(&*It, SpeakerCostume))
            return;
    }
    Script->DialogCostumes.push_back(*SpeakerCostume);
}

// (06eb84c0-06eb8931).
void FMissionScript::LoadMission(INI_Reader* Reader)
{
    // the routine(Reader), the TU38 slot 25 body.
    FMissionObject::LoadMission(Reader);
    // .
    Reader->reset();
    while (Reader->read_header())
    {
        if (Reader->is_header("Trigger"))
        {
            LoadMissionTriggerSection(this, Reader);
            continue;
        }
        // = "NPC".
        if (Reader->is_header("NPC"))
        {
            // the routine on a stack FMsnNpc, a cleared ID_String,
            // (&id, &npc, Reader), then {id, npc (0x13 dwords)} into the +0x184 map
            // (the routine, unique insert).
            FMsnNpc Npc;
            ID_String Nickname;
            Nickname.clear();
            LoadMsnNpc(&Nickname, &Npc, Reader);
            MissionNpcs.insert(std::map<unsigned int, FMsnNpc>::value_type(Nickname.Id, Npc));
            continue;
        }
        if (Reader->is_header("Dialog"))
        {
            // every "line" value whose second string is "Player" (stricmp):
            // its first string (TString<0x30>, cut to 0x2f) -> the routine (the costume of that
            // mission NPC or solar) -> unique push_back into +0x31c (the routine compare,
            // insert).
            while (Reader->read_value())
            {
                if (!Reader->is_value("line"))
                    continue;
                if (_stricmp(Reader->get_value_string(1), "Player") != 0)
                    continue;
                TString<0x30> Speaker;
                Speaker.Length = 0;
                Speaker.Data[0] = '\0';
                Speaker = Reader->get_value_string(0);
                Costume* const SpeakerCostume = GetMissionCostume(&Speaker);
                if (SpeakerCostume == nullptr)
                    continue;
                AddDialogCostume(this, SpeakerCostume);
            }
        }
    }
}

// (06eb8f50-06eb9628). The switch tables: 06eb9630 (byte index 06eb96ac,
// messages 1..0x3c) and 06eb96e8 (byte index 06eb9714, messages 0x1000..0x1011). Every case
// ends in either the branch (the TU38 body the routine, then the tail) or caseD_7 (the tail
// only); the tail (06eb90ef-06eb9144) runs the NN path and the nag objects unless the mission
// is ending, then always vtbl[+0x78].
void FMissionScript::HandleMissionMessage(EMessage MessageId, void const* MessageParms)
{
    // nothing at all without a live player state.
    if (FindContentPlayerState(PlayerId) == nullptr)
        return;

    unsigned int const* Parms = static_cast<unsigned int const*>(MessageParms);
    // The case's message id (local_c1c) handed to the routine.
    EMessage EventId = MessageId;
    bool bBaseBody = true;

    switch (MessageId)
    {
    case EMessage::BaseEnter:
        // parms {base, player}.
        if (Parms[1] == PlayerId)
            OnBaseEnter(MessageParms);
        break;
    case EMessage::BaseExit:
        // .
        if (Parms[1] == PlayerId)
            OnBaseExit(MessageParms);
        break;
    case EMessage::CargoScanned:
    case EMessage::CommComplete:
    case EMessage::ObjectDamaged:
    case EMessage::TradelaneEnter:
    case EMessage::TradelaneExit:
    case EMessage::RtcDone:
    case EMessage::JumpInComplete:
    case EMessage::LaunchComplete:
    case EMessage::PlayerManeuver:
    case EMessage::JumpgateAct:
    case EMessage::NpcSystemExit:
    case EMessage::EncounterLaunched:
    case EMessage::RumorHeard:
    case EMessage::TetherObject:
        // / 06eb94af / 06eb90d1 / 06eb9167 / 06eb917a / 06eb9286 / 06eb9154 /
        // / 06eb9353 / 06eb9546 / 06eb956c / 06eb95eb / 06eb94d9 / 06eb9559.
        DispatchMissionTriggerEvent(this, &EventId, MessageParms);
        break;
    case EMessage::PlayerSave:
    {
        // the save {player, FILE*}; skips the TU38 body when the gate fails.
        bBaseBody = false;
        if (Parms[0] != PlayerId || !pub::SinglePlayer())
            break;
        FILE* File = reinterpret_cast<FILE*>(static_cast<uintptr_t>(Parms[1]));
        fprintf(File, "\n[MissionState]\n");
        SaveMissionState(File);
        FMissionObject::HandleMissionMessage(MessageId, MessageParms);
        fprintf(File, "\n[BStoryMissionDone]\n");
        bTriggerSavePending = 0;
        break;
    }
    case EMessage::PlayerLoad:
    {
        // the load {player, INI_Reader*}.
        PlayerState = FindContentPlayerState(PlayerId);
        if (PlayerState == nullptr)
        {
            bPendingDestroy = 1;
            SetPlayer(0);
        }
        else if (Parms[0] == PlayerId)
        {
            INI_Reader* Reader = reinterpret_cast<INI_Reader*>(static_cast<uintptr_t>(Parms[1]));
            INI_Reader::State SavedState;
            Reader->get_state(SavedState);
            Reader->reset();
            LoadMissionState(Reader);
            Reader->set_state(SavedState);
        }
        // the TU38 body only while a player state is live.
        if (FindContentPlayerState(PlayerId) == nullptr)
            bBaseBody = false;
        break;
    }
    case EMessage::PlayerLaunch:
        // .
        if (Parms[0] == PlayerId)
            DispatchMissionTriggerEvent(this, &EventId, MessageParms);
        break;
    case EMessage::PlayerLogin:
    case EMessage::SystemLoaded:
    case EMessage::LoadGame:
        bBaseBody = false;
        break;
    case EMessage::PlayerLogout:
    {
        // drop every trigger.
        ActiveTriggers.clear();
        FTriggerMap::iterator TriggerIt = TriggerMap.begin();
        for (; TriggerIt != TriggerMap.end(); ++TriggerIt)
            DeleteTriggerContents(&TriggerIt->second);
        TriggerMap.clear();
        PlayerState = FindContentPlayerState(PlayerId);
        if (PlayerState == nullptr)
        {
            bPendingDestroy = 1;
            SetPlayer(0);
            bBaseBody = false;
        }
        break;
    }
    case EMessage::LootAcquired:
    {
        // the trigger event, then word parms+8 into the loot record's
        // +0xf8 (06eb927a).
        DispatchMissionTriggerEvent(this, &EventId, MessageParms);
        FMissionLoot* Found = FindLootById(Parms);
        if (Found != nullptr)
            Found->SubObjId = *reinterpret_cast<unsigned short const*>(
                static_cast<unsigned char const*>(MessageParms) + 8);
        break;
    }
    case EMessage::GFObjSelect:
        // .
        if (PlayerId == Parms[1])
            OnCharacterSelect(MessageParms);
        break;
    case EMessage::Heartbeat:
    {
        // +0x30c += dt, the delayed actions, event 0x20 with a local copy
        // of dt, event 0x1011 with no parms, then the TU38 body with the real parms.
        float DeltaSeconds = *static_cast<float const*>(MessageParms);
        ElapsedTime = DeltaSeconds + ElapsedTime;
        TickMissionTimers(this, DeltaSeconds);
        DispatchMissionTriggerEvent(this, &EventId, &DeltaSeconds);
        EventId = EMessage::TetherObject;
        DispatchMissionTriggerEvent(this, &EventId, nullptr);
        break;
    }
    case EMessage::AbortMission:
        // the TU38 body first, then the routine; tail only.
        FMissionObject::HandleMissionMessage(MessageId, MessageParms);
        OnMessage24(MessageParms);
        bBaseBody = false;
        break;
    case EMessage::LocationEnter:
        // the location enter on the player's base is remembered.
        if (PlayerId == Parms[1] && FindContentPlayerState(PlayerId) != nullptr)
        {
            if (PlayerState->GetBaseRecord() != nullptr)
                CurrentLocationId = Parms[0];
            DispatchMissionTriggerEvent(this, &EventId, MessageParms);
        }
        break;
    case EMessage::LocationExit:
        // .
        if (PlayerId == Parms[1])
            OnLocationExit(MessageParms);
        break;
    case EMessage::MissionResponse:
        // .
        if (Parms[3] == PlayerId && Parms[0] == ControllerId)
            OnOfferResponse(MessageParms);
        break;
    case EMessage::PopUpDialog:
        // the answer to the reputation pop-up.
        if (Parms[0] == PlayerId)
        {
            if (bRepPopUpShown != 0)
            {
                if ((Parms[1] & 1) != 0)
                {
                    int PlayerRep = -1;
                    pub::Player::GetRep(PlayerId, PlayerRep);
                    std::vector<unsigned int>::iterator GroupIt = RepPopUpGroups.begin();
                    for (; GroupIt != RepPopUpGroups.end(); ++GroupIt)
                        pub::Reputation::SetReputation(PlayerRep, *GroupIt, 0.6f);
                    bRepPopUpArmed = 0;
                }
                bRepPopUpShown = 0;
            }
            DispatchMissionTriggerEvent(this, &EventId, MessageParms);
        }
        break;
    case EMessage::SpaceEnter:
        // the TU38 body first; tail only.
        FMissionObject::HandleMissionMessage(MessageId, MessageParms);
        if (Parms[1] == PlayerId)
            OnSpaceEnter(MessageParms);
        bBaseBody = false;
        break;
    case EMessage::SpaceExit:
        // the TU38 body first; tail only.
        FMissionObject::HandleMissionMessage(MessageId, MessageParms);
        if (Parms[1] == PlayerId)
            DispatchMissionTriggerEvent(this, &EventId, MessageParms);
        bBaseBody = false;
        break;
    case EMessage::ObjectDestroyed:
        // .
        OnObjectDestroyed(MessageParms);
        break;
    case EMessage::SystemExit:
        // .
        if (Parms[2] == PlayerId)
            DispatchMissionTriggerEvent(this, &EventId, MessageParms);
        break;
    case EMessage::SystemEnter:
        // the TU38 body first; tail only.
        FMissionObject::HandleMissionMessage(MessageId, MessageParms);
        OnSystemEnter(MessageParms);
        OnSystemEnterPopUp(MessageParms);
        bBaseBody = false;
        break;
    case EMessage::MissionStart:
        // ; tail only.
        ActivateInitialMissionTriggers(this);
        bBaseBody = false;
        break;
    case EMessage::DamageReputation:
        // ; tail only.
        if (FindContentPlayerState(PlayerId) != nullptr
            && PlayerState->GetLiveMission() == ControllerId)
            OnMessage1005(MessageParms);
        bBaseBody = false;
        break;
    case EMessage::Msg0x1006:
        // .
        if (Parms[1] == 0)
        {
            if (Parms[2] == 1)
                Field_0x2e8 = 0x5462;
            else
                Field_0x2e8 = Parms[2];
        }
        break;
    case EMessage::PlayerMissionChanged:
        // .
        OnMessage100b(MessageParms);
        break;
    case EMessage::NpcShipCreated:
    {
        // an NPC of this mission was created.
        FContentNpcShip* Npc = *static_cast<FContentNpcShip* const*>(MessageParms);
        if (Npc->ControllerId == ControllerId)
        {
            RegisterNpc(MessageParms);
            SubmitNpcPersonality(Npc, 0);
            DispatchMissionTriggerEvent(this, &EventId, MessageParms);
        }
        break;
    }
    default:
        // caseD_9 (0x9, 0xa, 0xc, 0xe..0x15, 0x19..0x1f, 0x21, 0x23, 0x25, 0x27, 0x2b, 0x2d,
        // 0x2e, 0x31..0x35, 0x38, 0x1001..0x1004, 0x1008, 0x1009, 0x100e, 0x100f, and
        // everything outside the two tables).
        break;
    }

    if (bBaseBody)
        FMissionObject::HandleMissionMessage(MessageId, MessageParms);

    // the tail.
    if (bPendingDestroy == 0)
    {
        if (Field_0x2ec == 0 && NnPath.bActive != 0
            && (PlayerState->GetLiveMission() == ControllerId
                || (PlayerState->GetLiveMission() & 0x3fffffff) == 0))
            NnPath.HandleMessage(MessageId, MessageParms);
        // the routine(+0x204, msg, parms).
        DispatchNagMessage(this, MessageId, MessageParms);
    }
    // vtbl[+0x78].
    ApplyDesiredMissionState();
}

// .
void FMissionScript::OnBaseEnter(void const* MessageParms)
{
    unsigned int const* Parms = static_cast<unsigned int const*>(MessageParms);
    // every RTC script not yet started is started.
    std::list<FStoryMissionListNode_0x2f4>::iterator ScriptIt = List_0x2f4.begin();
    for (; ScriptIt != List_0x2f4.end(); ++ScriptIt)
    {
        if (ScriptIt->Field_0x8c == -1)
            StartRtcScript(&*ScriptIt);
    }
    // every ambient scene of this base is created.
    std::list<FMissionAmbientScene>::iterator SceneIt = AmbientScenes.begin();
    for (; SceneIt != AmbientScenes.end(); ++SceneIt)
    {
        if (SceneIt->BaseId != Parms[0])
            continue;
        AmbientScriptDescription Desc;
        Desc.RoomId = SceneIt->RoomId;
        Desc.ScriptId = 1;
        Desc.Flags = 0;
        Desc.Name = SceneIt->Path.Data;
        Desc.Filter = PlayerId;
        SceneIt->Handle = static_cast<int>(pub::GF::AmbientScriptCreate(Desc));
    }
    // .
    if (Field_0x2ec != 0)
    {
        RenumberAllObjectives();
        Field_0x2ec = 0;
    }
    else if (NnPath.bActive != 0)
    {
        NnPath.Rebuild();
        NnPath.SendObjectives(false, false);
    }
    // .
    EMessage EventId = EMessage::BaseEnter;
    DispatchMissionTriggerEvent(this, &EventId, MessageParms);
}

// (06eb9ef0-06eba005).
void FMissionScript::OnBaseExit(void const* MessageParms)
{
    // the base record (the routine on *parms); the scripts are only walked
    // with one.
    FContentBase* Base = FindContentBaseInfo(static_cast<unsigned int const*>(MessageParms));
    if (Base != nullptr)
    {
        std::list<FStoryMissionListNode_0x2f4>::iterator It = List_0x2f4.begin();
        while (It != List_0x2f4.end())
        {
            if (It->Field_0x90 != nullptr)
            {
                // the routine(Base, Field_0x8c, 0), -1 not tested.
                Base->RemoveBarScene(It->Field_0x8c, nullptr);
                It->Field_0x8c = -1;
                // the routine + free of the script.
                delete It->Field_0x90;
                It->Field_0x90 = nullptr;
            }
            // a finished script's node is unlinked.
            if (It->Field_0x94 != 0)
                It = List_0x2f4.erase(It);
            else
                ++It;
        }
    }
    // every ambient scene's handle is destroyed.
    std::list<FMissionAmbientScene>::iterator SceneIt = AmbientScenes.begin();
    for (; SceneIt != AmbientScenes.end(); ++SceneIt)
    {
        if (SceneIt->Handle != -1)
        {
            unsigned long Handle = static_cast<unsigned long>(SceneIt->Handle);
            pub::GF::AmbientScriptDestroy(&Handle);
            SceneIt->Handle = -1;
        }
    }
    EMessage EventId = EMessage::BaseExit;
    DispatchMissionTriggerEvent(this, &EventId, MessageParms);
}

// (06eba350-06eba41f).
void FMissionScript::OnCharacterSelect(void const* MessageParms)
{
    std::list<FStoryMissionListNode_0x2f4>::iterator It = List_0x2f4.begin();
    for (; It != List_0x2f4.end(); ++It)
    {
        if (It->Field_0x94 != 0 || It->Field_0x95 != 0)
            continue;
        if (FindContentPlayerState(PlayerId) == nullptr)
            continue;
        if (It->Field_0x90 == nullptr)
            continue;
        // the player's base, its room state of +0x308,
        // FindCharacter(script +0x10 role, PlayerId); a character whose id (+0x0c) equals
        // *parms gets SetBehavior(-1) and the node +0x94 = 1.
        FContentBase* const Base = PlayerState->GetBaseRecord();
        if (Base == nullptr)
            continue;
        FBaseRoomState* const Room = Base->FindRoomState(&CurrentLocationId);
        if (Room == nullptr)
            continue;
        FBarCharacterList::iterator Character;
        if (!Room->FindCharacter(GetFirstRtcRole(It->Field_0x90), PlayerId, &Character))
            continue;
        if (Character->CharacterId != *static_cast<unsigned int const*>(MessageParms))
            continue;
        Character->SetBehavior(static_cast<unsigned long>(-1));
        It->Field_0x94 = 1;
    }
    EMessage EventId = EMessage::GFObjSelect;
    DispatchMissionTriggerEvent(this, &EventId, MessageParms);
}

// (06eba530-06eba719).
void FMissionScript::OnLocationExit(void const* MessageParms)
{
    if (FindContentPlayerState(PlayerId) == nullptr)
        return;
    FContentBase* Base = PlayerState->GetBaseRecord();
    if (Base != nullptr)
    {
        // the roles (script +0x10 vector<0x3c>) of the unfinished scripts
        // of this location (script +0 == *parms), appended one by one (the routine).
        unsigned int const LocationId = *static_cast<unsigned int const*>(MessageParms);
        std::vector<FRtcScriptRole> KeepRoles;
        std::list<FStoryMissionListNode_0x2f4>::iterator It = List_0x2f4.begin();
        for (; It != List_0x2f4.end(); ++It)
        {
            if (It->Field_0x94 != 0 || It->Field_0x90 == nullptr)
                continue;
            if (It->Field_0x90->LocationId != LocationId)
                continue;
            std::vector<FRtcScriptRole>::const_iterator Role = It->Field_0x90->Roles.begin();
            for (; Role != It->Field_0x90->Roles.end(); ++Role)
                KeepRoles.push_back(*Role);
        }
        // every finished script (+0x94 == 1) with an object is released
        // through the routine(Base, +0x8c, &KeepRoles), freed and its node unlinked.
        It = List_0x2f4.begin();
        while (It != List_0x2f4.end())
        {
            if (It->Field_0x94 != 1 || It->Field_0x90 == nullptr)
            {
                ++It;
                continue;
            }
            Base->RemoveBarScene(It->Field_0x8c, &KeepRoles);
            It->Field_0x8c = -1;
            delete It->Field_0x90;
            It->Field_0x90 = nullptr;
            It = List_0x2f4.erase(It);
        }
        // KeepRoles destroyed.
    }
    EMessage EventId = EMessage::LocationExit;
    DispatchMissionTriggerEvent(this, &EventId, MessageParms);
}

// (06eba030-06eba1e2).
void FMissionScript::OnOfferResponse(void const* MessageParms)
{
    EMessage EventId = EMessage::MissionResponse;
    unsigned char const* ParmsBytes = static_cast<unsigned char const*>(MessageParms);
    if (ParmsBytes[8] == 0 || FindContentPlayerState(PlayerId) == nullptr)
    {
        // .
        DispatchMissionTriggerEvent(this, &EventId, MessageParms);
        return;
    }
    // an empty FmtStr shown with type 10, broadcast.
    {
        FmtStr EmptyMsg;
        pub::Player::DisplayMissionMessage(PlayerId, EmptyMsg,
                                           static_cast<MissionMessageType>(0xa), true);
    }
    DispatchMissionTriggerEvent(this, &EventId, MessageParms);
    // .
    if (StoryEvents != nullptr)
        StoryEvents->OnMissionEvent(1, static_cast<unsigned int>(CampaignSlotSucceed), ControllerId);
    // another live mission of the player is destroyed and left.
    unsigned int const LiveMission = PlayerState->GetLiveMission();
    if ((LiveMission & 0x3fffffff) != 0 && LiveMission != ControllerId)
    {
        pub::Controller::Destroy(LiveMission);
        PlayerState->EndLiveMission(LiveMission, 4, true);
    }
    // .
    bRepPopUpShown = 0;
    bRepPopUpArmed = 0;
    RepPopUpSystemId = 0;
    RepPopUpGroups.clear();
    // .
    SpacePopSetAttackClamp(1);
    SpacePopSetEncounterEnabled(0);
    GMissionScanClamp = 1;
    // a cleared ID_String, the routine(PlayerState; +4, EBX = 0, &id, 1,
    // +0x2c4) -- the group joins this mission. Arguments in baseline's positional order.
    ID_String NoVnpc;
    NoVnpc.clear();
    PlayerState->JoinMissionAsGroup(ControllerId, 0, &NoVnpc, true, Field_0x2c4);
}

// (06eba720-06eba859).
void FMissionScript::OnSpaceEnter(void const* MessageParms)
{
    unsigned int const* Parms = static_cast<unsigned int const*>(MessageParms);
    if (Parms[1] != PlayerId)
        return;
    // a CostumeHint per dialog costume, the costume assigned through
    // Costume's inline operator= (part ids, count, then `count` accessory ids).
    std::vector<Costume>::iterator It = DialogCostumes.begin();
    for (; It != DialogCostumes.end(); ++It)
    {
        CostumeHint Hint;
        Hint.Cost.PartIds[0] = It->PartIds[0];
        Hint.Cost.PartIds[1] = It->PartIds[1];
        Hint.Cost.PartIds[2] = It->PartIds[2];
        Hint.Cost.PartIds[3] = It->PartIds[3];
        Hint.Cost.AccessoryCount = It->AccessoryCount;
        for (int Index = 0; Index < static_cast<int>(It->AccessoryCount); ++Index)
            Hint.Cost.AccessoryIds[Index] = It->AccessoryIds[Index];
        pub::Player::LoadHint(PlayerId, &Hint);
    }
    EMessage EventId = EMessage::SpaceEnter;
    DispatchMissionTriggerEvent(this, &EventId, MessageParms);
    // .
    if (NnPath.bActive == 0)
    {
        RenumberAllObjectives();
    }
    else
    {
        NnPath.Rebuild();
        HideAllObjectives();
        NnPath.SendObjectives(false, false);
    }
    Field_0x2ec = 0;
}

// (06ebaae0-06ebab81).
void FMissionScript::OnObjectDestroyed(void const* MessageParms)
{
    unsigned int const* Parms = static_cast<unsigned int const*>(MessageParms);
    EMessage EventId = EMessage::ObjectDestroyed;
    DispatchMissionTriggerEvent(this, &EventId, MessageParms);
    // the first friendly-fire label naming the destroyed object is dropped.
    std::list<TString<0x30>>::iterator It = FriendlyFireOffLabels.begin();
    for (; It != FriendlyFireOffLabels.end(); ++It)
    {
        if (GetLabelObjectId(&*It) == Parms[0])
        {
            FriendlyFireOffLabels.erase(It);
            break;
        }
    }
    // the routine(PlayerId) false, a live player state and no ship ->
    // +0x19 = 1.
    if (!IsPlayerDead(PlayerId) && FindContentPlayerState(PlayerId) != nullptr
        && PlayerState->ShipObject == nullptr)
        bPendingDestroy = 1;
}

// (06eba860-06eba8e9).
void FMissionScript::OnSystemEnter(void const* MessageParms)
{
    unsigned int const* Parms = static_cast<unsigned int const*>(MessageParms);
    if (Parms[2] != PlayerId || FindContentPlayerState(PlayerId) == nullptr)
        return;
    EMessage EventId = EMessage::SystemEnter;
    DispatchMissionTriggerEvent(this, &EventId, MessageParms);
    FContentPlayerShip* const Ship = PlayerState->ShipObject;
    if (Ship == nullptr)
        return;
    // event 0x100f with {ship id, parms[0], parms[3] or 0}.
    unsigned int NpcEnterParms[3];
    NpcEnterParms[0] = Ship->SpaceObjId;
    NpcEnterParms[1] = Parms[0];
    NpcEnterParms[2] = (Parms[1] & 0x3fffffff) != 0 ? Parms[3] : 0;
    EventId = EMessage::NpcSystemEnter;
    DispatchMissionTriggerEvent(this, &EventId, NpcEnterParms);
}

// (06eba1f0-06eba343).
void FMissionScript::OnSystemEnterPopUp(void const* MessageParms)
{
    unsigned int const* Parms = static_cast<unsigned int const*>(MessageParms);
    if (bRepPopUpArmed == 0 || Parms[0] != RepPopUpSystemId || Parms[2] != PlayerId)
        return;
    FmtStr Title;
    Title.begin_mad_lib(0x54c4);
    Title.end_mad_lib();
    FmtStr Text;
    Text.begin_mad_lib(0x54c5);
    Text.end_mad_lib();
    pub::Player::PopUpDialog(PlayerId, Title, Text, 5);
    bRepPopUpShown = 1;
}

// (06eb9cd0-06eb9dba).
void FMissionScript::OnMessage24(void const* MessageParms)
{
    if (FindContentPlayerState(PlayerId) == nullptr)
        return;
    if (static_cast<unsigned int const*>(MessageParms)[0] != PlayerId)
        return;
    if (PlayerState->GetLiveMission() != ControllerId)
        return;
    FmtStr MissionMsg;
    MissionMsg.begin_mad_lib(Field_0x2e8);
    MissionMsg.end_mad_lib();
    pub::Player::DisplayMissionMessage(PlayerId, MissionMsg, MISSION_MSG_TYPE_0, true);
    bPendingDestroy = 1;
}

// (06eb99a0-06eb9a97): parms {kind, ship, victim}.
void FMissionScript::OnMessage1005(void const* MessageParms)
{
    unsigned int const* Parms = static_cast<unsigned int const*>(MessageParms);
    if (FindContentPlayerState(PlayerId) == nullptr)
        return;
    FContentPlayerShip* const Ship = PlayerState->ShipObject;
    if (Ship == nullptr)
        return;
    if (Parms[1] != Ship->SpaceObjId || Parms[0] != 1)
        return;
    // a victim named by a friendly-fire label is excused.
    std::list<TString<0x30>>::iterator It = FriendlyFireOffLabels.begin();
    for (; It != FriendlyFireOffLabels.end(); ++It)
    {
        if (GetLabelObjectId(&*It) == Parms[2])
            return;
    }
    int VictimRep = 0;
    int ShipRep;
    float Attitude = 0.0f;
    pub::SpaceObj::GetRep(Parms[2], VictimRep);
    pub::SpaceObj::GetRep(Parms[1], ShipRep);
    pub::Reputation::GetAttitude(VictimRep, ShipRep, Attitude);
    // FCOMP the field (0.2f), TEST AH,0x41 / JNZ: fall-through (fail
    // with 0x5460) only when Attitude > 0.2.
    if (Attitude > 0.2f)
    {
        SetDesiredMissionState(G_MissionStateFail, 0x5460);
        return;
    }
    // FCOMP the field (-0.2f), TEST AH,0x1 / JNZ skip: fail with 0x5463
    // when Attitude >= -0.2 (not below, not unordered).
    if (Attitude >= -0.2f)
        SetDesiredMissionState(G_MissionStateFail, 0x5463);
}

// (06eba8f0-06ebaacf).
void FMissionScript::OnMessage100b(void const* MessageParms)
{
    unsigned int const* Parms = static_cast<unsigned int const*>(MessageParms);
    if (FindContentPlayerState(PlayerId) != nullptr && PlayerState->ShipObject != nullptr
        && Parms[0] != ControllerId)
    {
        // a local record {"", handle -1, no script, 0, 0}.
        FStoryMissionListNode_0x2f4 Restart;
        // the first node with a script whose mission text (+0x2c) is set,
        // not finished, while the player has a base.
        std::list<FStoryMissionListNode_0x2f4>::iterator It = List_0x2f4.begin();
        for (; It != List_0x2f4.end(); ++It)
        {
            if (It->Field_0x90 == nullptr || It->Field_0x90->MissionTextId == 0 ||
                It->Field_0x94 != 0)
            {
                continue;
            }
            FContentBase* const Base = PlayerState->GetBaseRecord();
            if (Base == nullptr)
                continue;
            // its path (the routine, max 0x80) and +0x95.
            size_t PathLength = strlen(It->Path);
            if (PathLength >= 0x80)
                PathLength = 0x7f;
            memcpy(Restart.Path, It->Path, PathLength);
            Restart.Path[PathLength] = '\0';
            Restart.PathLength = static_cast<unsigned int>(PathLength);
            Restart.Field_0x95 = It->Field_0x95;
            // the routine(Base, +0x8c, 0), the routine + free, unlink.
            Base->RemoveBarScene(It->Field_0x8c, nullptr);
            It->Field_0x8c = -1;
            delete It->Field_0x90;
            It->Field_0x90 = nullptr;
            List_0x2f4.erase(It);
            break;
        }
        // a copied path restarts the script and appends the record.
        if (Restart.PathLength != 0)
        {
            StartRtcScript(&Restart);
            List_0x2f4.push_back(Restart);
        }
    }
    // with *parms == 0 the player's first objective is resent with its
    // type or-ed with 0x30, the record flagged (slot 1) around the send.
    if (Parms[0] != 0)
        return;
    FMissionPlayer* Player = FindPlayer(PlayerId);
    if (Player == nullptr)
        return;
    FMissionObjective* First = Player->Objectives.GetFront();
    unsigned int const SavedType = First->Objective.Type;
    First->Objective.Type = SavedType | 0x30;
    Player->SetField25(true);
    Player->SendObjectives();
    Player->SetField25(false);
    First->Objective.Type = SavedType;
}

// (06ebabb0-06ebae00).
void FMissionScript::SaveMissionState(FILE* File)
{
    // NEG/SBB/INC of (live mission - this id) -> 1 when equal.
    fprintf(File, "mission_accepted = %d\n", PlayerState->GetLiveMission() == ControllerId ? 1 : 0);
    // the field (GSpacePopAttackClamp) CMP AL,1.
    fprintf(File, GSpacePopAttackClamp == 1 ? "att_clamp = true\n" : "att_clamp = false\n");
    fprintf(File, SpacePopGetEncounterEnabled() == 1 ? "tradelane_attacks = true\n"
                                                      : "tradelane_attacks = false\n");
    fprintf(File, GMissionScanClamp == 1 ? "scan_clamp = true\n" : "scan_clamp = false\n");
    fprintf(File, ContentIsGcsClampEnabled() ? "gcs_clamp = true\n" : "gcs_clamp = false\n");
    fprintf(File, SpacePopGetHostileClamp() == 1 ? "hostile_clamp = true\n"
                                                  : "hostile_clamp = false \n");
    fprintf(File, SpacePopGetPopulationEnabled() != 0 ? "random_pop = true\n"
                                                       : "random_pop = false\n");
    // the madlib ids of the TU38 FmtStrs +0x68 and +0x50.
    fprintf(File, "msn_offer = %d\n", Field_0x68.MadLibId);
    fprintf(File, "msn_title = %d\n", Field_0x50.MadLibId);
    if (NnPath.bActive != 0)
        NnPath.Save(File);
    if (bTriggerSavePending != 0)
    {
        // .
        fprintf(File, "\n[TriggerSave]\n");
        fprintf(File, "trigger = %u\n", SaveTriggerId);
        return;
    }
    // every unfinished script.
    std::list<FStoryMissionListNode_0x2f4>::iterator ScriptIt = List_0x2f4.begin();
    for (; ScriptIt != List_0x2f4.end(); ++ScriptIt)
    {
        if (ScriptIt->Field_0x94 != 0)
            continue;
        fprintf(File, "rtc = %s, %s\n", ScriptIt->Path, ScriptIt->Field_0x95 != 0 ? "true" : "false");
    }
    std::list<FMissionAmbientScene>::iterator SceneIt = AmbientScenes.begin();
    for (; SceneIt != AmbientScenes.end(); ++SceneIt)
        fprintf(File, "ambi_scene = %s, %u, %u\n", SceneIt->Path.Data, SceneIt->RoomId,
                SceneIt->BaseId);
    std::list<TString<0x30>>::iterator LabelIt = FriendlyFireOffLabels.begin();
    for (; LabelIt != FriendlyFireOffLabels.end(); ++LabelIt)
        fprintf(File, "friendly_fire_excused = %s\n", LabelIt->Data);
    fprintf(File, "story_cue = %d\n", StoryCue);
    // a [TriggerSave] per active trigger, its condition's slot 5 (+0x14).
    std::list<FTriggerMap::iterator>::iterator ActiveIt = ActiveTriggers.begin();
    for (; ActiveIt != ActiveTriggers.end(); ++ActiveIt)
    {
        fprintf(File, "\n[TriggerSave]\n");
        fprintf(File, "trigger = %u\n", (*ActiveIt)->first);
        (*ActiveIt)->second.Condition->Save(File);
    }
}

// (06ebae10-06ebb52f). Four local flags: [MissionState] seen (the [NNObjective]
// gate), the RTC list released, the active triggers dropped, the objectives cleared. The
// header loop ends at [BStoryMissionDone]; any other header is skipped.
void FMissionScript::LoadMissionState(INI_Reader* Reader)
{
    bool bMissionStateSeen = false;
    bool bRtcReleased = false;
    bool bTriggersDropped = false;
    bool bObjectivesCleared = false;
    while (Reader->read_header())
    {
        if (Reader->is_header("MissionState"))
        {
            // .
            Field_0x2ec = 1;
            bMissionStateSeen = true;
            while (Reader->read_value())
            {
                if (Reader->is_value("rtc"))
                {
                    // the first "rtc" releases the started scripts; each one
                    // is appended (the routine) with handle -1, no script, not finished,
                    // +0x95 = b1.
                    if (!bRtcReleased)
                        ReleaseRtcScripts();
                    FStoryMissionListNode_0x2f4 Script;
                    TString<0x80> Path;
                    Path = Reader->get_value_string(0);
                    Script.PathLength = Path.Length;
                    memcpy(Script.Path, Path.Data, Path.Length + 1);
                    Script.Field_0x95 = Reader->get_value_bool(1) ? 1 : 0;
                    List_0x2f4.push_back(Script);
                    bRtcReleased = true;
                    continue;
                }
                if (Reader->is_value("ambi_scene"))
                {
                    // path s0, room i1, base i2, handle -1, appended to
                    // +0x310. The AmbientScriptDescription the listing fills on the stack
                    // (06ebaff7-06ebb03d) is never passed anywhere.
                    FMissionAmbientScene Scene;
                    Scene.Path.Length = 0;
                    Scene.Path.Data[0] = '\0';
                    Scene.Handle = -1;
                    Scene.RoomId = 0;
                    Scene.BaseId = 0;
                    Scene.Path = Reader->get_value_string(0);
                    Scene.RoomId = static_cast<unsigned int>(Reader->get_value_int(1));
                    Scene.BaseId = static_cast<unsigned int>(Reader->get_value_int(2));
                    AmbientScenes.push_back(Scene);
                    continue;
                }
                if (Reader->is_value("friendly_fire_excused"))
                {
                    // s0 as TString<0x30>, appended to +0x2fc.
                    TString<0x30> Label;
                    Label = Reader->get_value_string(0);
                    FriendlyFireOffLabels.push_back(Label);
                    continue;
                }
                if (Reader->is_value("mission_accepted"))
                {
                    // .
                    if (Reader->get_value_bool(0) != true)
                        continue;
                    if (FindContentPlayerState(PlayerId) == nullptr)
                        continue;
                    ID_String ObjectiveIds;
                    ObjectiveIds.clear();
                    // the routine(PlayerState; +4, 0, &id, 0, 0), in
                    // baseline's positional order.
                    PlayerState->JoinMissionAsGroup(ControllerId, 0, &ObjectiveIds, false, 0);
                    if (StoryEvents != nullptr)
                        StoryEvents->OnMissionEvent(1, static_cast<unsigned int>(CampaignSlotSucceed),
                                                    ControllerId);
                    continue;
                }
                if (Reader->is_value("att_clamp"))
                {
                    // the field in place.
                    GSpacePopAttackClamp = Reader->get_value_bool(0) ? 1 : 0;
                    continue;
                }
                if (Reader->is_value("tradelane_attacks"))
                {
                    SpacePopSetEncounterEnabled(Reader->get_value_bool(0) ? 1 : 0);
                    continue;
                }
                if (Reader->is_value("scan_clamp"))
                {
                    GMissionScanClamp = Reader->get_value_bool(0) ? 1 : 0;
                    continue;
                }
                if (Reader->is_value("gcs_clamp"))
                {
                    SpacePopSetGcsClampEnabled(Reader->get_value_bool(0) ? 1 : 0);
                    continue;
                }
                if (Reader->is_value("random_pop"))
                {
                    SpacePopSetPopulationEnabled(Reader->get_value_bool(0) ? 1 : 0);
                    continue;
                }
                if (Reader->is_value("hostile_clamp"))
                {
                    SpacePopSetHostileClamp(Reader->get_value_bool(0) ? 1 : 0);
                    continue;
                }
                // a non-zero id rebuilds +0x68 / +0x50 (FmtStr::destroy, the
                // 6 dwords zeroed, begin_mad_lib(i0 read again), end_mad_lib).
                FmtStr* MadLib = nullptr;
                if (Reader->is_value("msn_offer"))
                {
                    if (Reader->get_value_int(0) != 0)
                        MadLib = &Field_0x68;
                }
                else if (Reader->is_value("msn_title"))
                {
                    if (Reader->get_value_int(0) != 0)
                        MadLib = &Field_0x50;
                }
                else if (Reader->is_value("nnpath"))
                {
                    // the routine, then the routine(+0x21c, Reader, this).
                    HideAllObjectives();
                    NnPath.Load(Reader, this);
                    continue;
                }
                if (MadLib != nullptr)
                {
                    MadLib->destroy();
                    MadLib->Head = nullptr;
                    MadLib->MadLibId = 0;
                    for (int Index = 0; Index < FmtStr::KindCount; ++Index)
                        MadLib->ValueIndex[Index] = 0;
                    MadLib->begin_mad_lib(static_cast<unsigned int>(Reader->get_value_int(0)));
                    MadLib->end_mad_lib();
                }
            }
            continue;
        }
        if (Reader->is_header("TriggerSave"))
        {
            while (Reader->read_value())
            {
                if (!Reader->is_value("trigger"))
                    continue;
                // the first "trigger" drops the active list (the routine per
                // node, the triggers' states untouched), +0x1e4 and +0x1d0.
                if (!bTriggersDropped)
                {
                    ActiveTriggers.clear();
                    InitiallyActive.clear();
                    PendingActivations.clear();
                }
                // {i0, i0} unique into +0x1e4 (activated by message 0x1000).
                // The key is the second read, the value the first.
                unsigned int const FirstRead = static_cast<unsigned int>(Reader->get_value_int(0));
                unsigned int const SecondRead = static_cast<unsigned int>(Reader->get_value_int(0));
                InitiallyActive.insert(
                    std::map<unsigned int, unsigned int>::value_type(SecondRead, FirstRead));
                // every trigger with id i0 (read a third time): its condition's
                // slot 6 (+0x18, Load) on the reader.
                unsigned int const TriggerId = static_cast<unsigned int>(Reader->get_value_int(0));
                FTriggerMap::iterator It = TriggerMap.begin();
                for (; It != TriggerMap.end(); ++It)
                {
                    if (It->first == TriggerId)
                        It->second.Condition->Load(Reader);
                }
                bTriggersDropped = true;
            }
            continue;
        }
        if (Reader->is_header("NNObjective") && bMissionStateSeen)
        {
            // the first one clears every objective (the routine); each is
            // loaded into slot 6's (+0x18) objective list (+8) by the routine.
            if (!bObjectivesCleared)
            {
                ClearObjectives();
                bObjectivesCleared = true;
            }
            LoadNNObjective(Reader, this, &GetFirstPlayer()->Objectives);
            continue;
        }
        if (Reader->is_header("BStoryMissionDone"))
            return;
    }
}

// (06ebb620-06ebb7ed). stdcall (npc, mode), no this; case 0x1010 passes
// (*parms, 0).
void FMissionScript::SubmitNpcPersonality(FContentNpcShip* Npc, int Mode)
{
    // CMP EBP,0 / JZ return.
    if (Npc == nullptr)
        return;
    unsigned int const Flid = Npc->SpaceObjId;
    pub::AI::Personality LocalPersonality;
    if (!pub::AI::get_personality(Flid, LocalPersonality))
        return;
    if (Mode == 0)
    {
        // .
        LocalPersonality.JobParams.Field_00 = 0;
        LocalPersonality.JobParams.SceneToughnessThreshold = 4;
        LocalPersonality.JobParams.LootPreference = 0;
        LocalPersonality.JobParams.FleeSceneThreatStyle = 4;
        LocalPersonality.JobParams.FleeWhenHullDamagedPercent = 0;
        LocalPersonality.JobParams.bFleeWhenLeaderFleesStyle = 0;
        LocalPersonality.JobParams.bFleeNoWeaponsStyle = 0;
    }
    else if (Mode == 1)
    {
        // a second Personality from the ship's own pilot (npc +0x1cc) or,
        // when that is empty, from the pilot of its npcships entry (the routine on
        // ship arch +0x138, value +0xc); on success its job fields replace the live ones.
        pub::AI::Personality PilotPersonality;
        ID_String PilotId = Npc->Field_0x1cc;
        bool bLoaded = false;
        if (PilotId.IsEmpty())
        {
            FNpcShipArch* const Arch = FindNpcShipArchByShip(Npc->ShipArchId, &GNpcShipArchs);
            if (Arch != nullptr)
                bLoaded = GetPilotPersonality(&Arch->Pilot.Id, &PilotPersonality);
        }
        else
        {
            bLoaded = GetPilotPersonality(&PilotId.Id, &PilotPersonality);
        }
        if (bLoaded)
        {
            // .
            LocalPersonality.JobParams.FleeSceneThreatStyle = PilotPersonality.JobParams.FleeSceneThreatStyle;
            LocalPersonality.JobParams.FleeWhenHullDamagedPercent = PilotPersonality.JobParams.FleeWhenHullDamagedPercent;
            LocalPersonality.JobParams.bFleeWhenLeaderFleesStyle = PilotPersonality.JobParams.bFleeWhenLeaderFleesStyle;
            LocalPersonality.JobParams.bFleeNoWeaponsStyle = PilotPersonality.JobParams.bFleeNoWeaponsStyle;
            LocalPersonality.JobParams.SceneToughnessThreshold = PilotPersonality.JobParams.SceneToughnessThreshold;
            LocalPersonality.JobParams.Field_00 = PilotPersonality.JobParams.Field_00;
            LocalPersonality.JobParams.LootPreference = PilotPersonality.JobParams.LootPreference;
        }
    }
    // SetPersonalityParams + SubmitState.
    pub::AI::SetPersonalityParams Params;
    Params.bSetPersonality = 1;
    Params.Value = LocalPersonality;
    Params.StateGraphId = pub::AI::get_state_graph_id(Flid);
    // npc +0x174 (ContentCallback) and +0x118 (DirectiveCallback).
    Params.ContentCallbackObject = static_cast<pub::AI::ContentCallback*>(Npc);
    Params.DirectiveCallbackObject = static_cast<pub::AI::DirectiveCallback*>(Npc);
    pub::AI::SubmitState(Flid, &Params);
}
