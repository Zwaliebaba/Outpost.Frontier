// Oracle instrumentation (design/migration-plan.md, Phase 0; work package P3.5c2, decision record
// 0008 question 9 and its Verification table): the Tier B mission session with a ship, Mission_01a's
// space part run on the bus for a scenario's player, for the Rust comparison
// (crates/game-logic/tests/mission_session.rs). New exports, not part of the reference DLL's
// interface: only Oracle.exe sim calls them, for a scenario's `mission <name> <session>` line
// (CppSource/Oracle/mission_session.txt), so a game run never reaches them.
//
// OracleSimMissionBegin(Dir, Name, Session, Ship, ShipArch, System), after the scenario's objects
// are spawned and its players joined: the session's player gets a Content player state in the
// system's record with the session's ship as its ship object (made as GameSupport's OnPlayerLaunch
// makes it, with the scenario's archetype for pub::Player::GetShipID, which refuses the harness's
// player: GetShipMissionProperties of the archetype, `fighter` or `freighter` by its type, the
// PLAYER state graph), the mission is made as Story makes one (pub::Controller::Create of
// "Content.dll" Name with FStoryMissionCreateParms {Session, no story}, ABOVE_NORMAL, then
// MissionStart through pub::Controller::_SendMessage), and GOracleMissionTraceHook (MissionScript.h)
// writes the mission's trigger engine into <Dir>\mission.jsonl. It returns the controller id, 0 on
// failure. The session's scripted messages (OracleSessionSteps) go to the mission alone through
// _SendMessage at the start of their tick's frame (OracleSimMissionFrame); every delivery the bus
// makes to the mission is written as a `message` record before its handler runs
// (OracleSimMissionDeliver, from the harness's dispatch hook); after each frame the mission's live
// NPC ships and spawned solars and loot are written to <Dir>\mission_objects.jsonl
// (OracleSimMissionFrameEnd); OracleSimMissionEnd writes the `final` record (or `gone` when the bus
// has torn the mission down) and closes both files.
//
// mission.jsonl holds the records of oracle_mission_trace.cpp (P3.5c1), and
//   {"rec":"gone","tick":T}
// in place of `final` when the mission no longer exists. mission_objects.jsonl holds
//   {"rec":"obj","tick":T,"kind":"npc"|"solar"|"loot","name":"<nickname>","id":I,"pos":[X,Y,Z]}
// with each coordinate's float bits as an unsigned number, for each object whose GetLocation
// succeeds, NPCs in the mission's map order, then the solars and the loot in their list order.

#include "content.h"
#include "ContentInternal.h"
#include "ContentPlayerState.h"
#include "MissionAct.h"
#include "MissionDialogs.h"
#include "MissionObject.h"
#include "MissionScript.h"
#include "NpcShip.h"
#include "RandomMissionDB.h"
#include "SystemInfo.h"
#include "common_id.h"
#include "common_server.h"
#include "common_server_controller.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <string>
#include <vector>

namespace
{
    using pub::Controller::EMessage;

    // One scripted message of the session: the tick it comes at (before the tick's frame), its
    // kind and, for a comm, the line's nickname.
    enum EOracleSessionStep
    {
        OracleSessionSpaceEnter,
        OracleSessionCommComplete,
    };

    struct FOracleSessionStep
    {
        unsigned int Tick;
        EOracleSessionStep Kind;
        char const* Name;
    };

    // The session (the Rust test's STEPS, in the same order): the player's SpaceEnter, then the
    // three scripted comms of P3.5c1's opening.
    FOracleSessionStep const OracleSessionSteps[] = {
        {2u, OracleSessionSpaceEnter, nullptr},
        {200u, OracleSessionCommComplete, "DX_M01_0000_FREEPORT_7_BRIDGE"},
        {260u, OracleSessionCommComplete, "DX_M01_0005_Samura_SA_14"},
        {320u, OracleSessionCommComplete, "DX_M01_0010_FREEPORT_7_BRIDGE"},
    };

    struct FOracleSessionSpaceEnter
    {
        unsigned int SystemId;
        unsigned int PlayerId;
        unsigned int LaunchObject;
        int LaunchKind;
    };

    struct FOracleSessionCommComplete
    {
        unsigned int Listener;
        unsigned int SystemId;
        unsigned int Speaker;
        unsigned int Line;
        unsigned int Result;
    };

    FILE* GSessionTrace = nullptr;
    FILE* GSessionObjects = nullptr;
    unsigned int GSessionController = 0;
    unsigned int GSessionPlayer = 0;
    unsigned int GSessionSystem = 0;
    unsigned int GSessionTick = 0;
    int GSessionRecords = 0;
    // The mission, as the hook first sees it (its MissionStart activates its initial triggers).
    FMissionScript const* GSessionScript = nullptr;

    unsigned int FloatBits(float Value)
    {
        unsigned int Bits;
        std::memcpy(&Bits, &Value, sizeof(Bits));
        return Bits;
    }

    // The place of Action in its trigger's list (as oracle_mission_trace.cpp).
    unsigned int SessionActionIndex(FMissionScript const* Script, Act const* Action)
    {
        FTriggerMap::const_iterator Found = Script->TriggerMap.find(Action->TriggerId);
        if (Found == Script->TriggerMap.end())
            return 0xffffffffu;
        unsigned int Index = 0;
        std::list<Act*>::const_iterator It = Found->second.Actions.begin();
        for (; It != Found->second.Actions.end(); ++It, ++Index)
        {
            if (*It == Action)
                return Index;
        }
        return 0xffffffffu;
    }

    void __cdecl SessionHook(int Kind, FMissionScript const* Script, unsigned int TriggerId, unsigned int A,
                             int B, Act const* Action)
    {
        if (GSessionTrace == nullptr || Script == nullptr || Script->ControllerId != GSessionController)
            return;
        if (GSessionScript == nullptr)
            GSessionScript = Script;
        FILE* const Out = GSessionTrace;
        unsigned int const Tick = GSessionTick;
        switch (Kind)
        {
        case OracleMissionTraceActivate:
            std::fprintf(Out, "{\"rec\":\"activate\",\"tick\":%u,\"trigger\":%u}\n", Tick, TriggerId);
            break;
        case OracleMissionTraceFire:
            std::fprintf(Out, "{\"rec\":\"fire\",\"tick\":%u,\"trigger\":%u,\"event\":%u}\n", Tick,
                         TriggerId, A);
            break;
        case OracleMissionTraceAction:
            std::fprintf(Out, "{\"rec\":\"action\",\"tick\":%u,\"trigger\":%u,\"index\":%u}\n", Tick,
                         TriggerId, A);
            break;
        case OracleMissionTraceTimer:
            std::fprintf(Out, "{\"rec\":\"timer\",\"tick\":%u,\"trigger\":%u,\"index\":%u}\n", Tick,
                         TriggerId, SessionActionIndex(Script, Action));
            break;
        case OracleMissionTraceRearm:
            std::fprintf(Out, "{\"rec\":\"rearm\",\"tick\":%u,\"trigger\":%u}\n", Tick, TriggerId);
            break;
        case OracleMissionTraceDeactivate:
            std::fprintf(Out, "{\"rec\":\"deactivate\",\"tick\":%u,\"trigger\":%u}\n", Tick, TriggerId);
            break;
        case OracleMissionTraceState:
            std::fprintf(Out, "{\"rec\":\"state\",\"tick\":%u,\"from\":%d,\"to\":%d}\n", Tick,
                         static_cast<int>(A), B);
            break;
        default:
            return;
        }
        ++GSessionRecords;
    }

    void SessionSend(EMessage Message, void const* Parms)
    {
        pub::Controller::_SendMessage(GSessionController, Message, Parms);
    }

    void WriteObject(char const* Kind, char const* Name, unsigned int ObjId)
    {
        Vector Position;
        Matrix Orientation;
        if (pub::SpaceObj::GetLocation(ObjId, Position, Orientation) != 0)
            return;
        std::fprintf(GSessionObjects,
                     "{\"rec\":\"obj\",\"tick\":%u,\"kind\":\"%s\",\"name\":\"%s\",\"id\":%u,\"pos\":[%u,%u,%u]}\n",
                     GSessionTick, Kind, Name, ObjId, FloatBits(Position.x), FloatBits(Position.y),
                     FloatBits(Position.z));
    }
}

extern "C" __declspec(dllexport) unsigned int __cdecl OracleSimMissionBegin(char const* Dir, char const* Name,
                                                                           unsigned int SessionId,
                                                                           unsigned int ShipId,
                                                                           unsigned int ShipArch,
                                                                           unsigned int SystemId)
{
    if (Dir == nullptr || Name == nullptr)
        return 0;
    std::string const TracePath = std::string(Dir) + "\\mission.jsonl";
    std::string const ObjectsPath = std::string(Dir) + "\\mission_objects.jsonl";
    GSessionTrace = std::fopen(TracePath.c_str(), "wb");
    GSessionObjects = std::fopen(ObjectsPath.c_str(), "wb");
    if (GSessionTrace == nullptr || GSessionObjects == nullptr)
        return 0;

    // The player's state in the system's record, its ship object as OnPlayerLaunch makes it.
    FContentPlayerState* const State = ReplaceContentPlayerState(SessionId);
    State->SystemRecord = FindSystemInfo(&SystemId);
    FContentPlayerShipParams Params;
    Params.ShipId = ShipId;
    Params.ShipArchId = ShipArch;
    GetShipMissionProperties(ShipArch, &Params.ClassIds);
    if (Params.ReputationHandle != State)
    {
        if (Params.ReputationHandle != nullptr)
            Params.ReputationHandle->Release();
        Params.ReputationHandle = State;
        State->AddRef();
    }
    Params.System = State->GetSystemRecord();
    unsigned int Type = 0;
    pub::SpaceObj::GetType(ShipId, Type);
    unsigned int ClassId = 0;
    if ((Type & 0x10000) != 0)
        ClassId = CreateID("fighter");
    else if ((Type & 0x20000) != 0)
        ClassId = CreateID("freighter");
    if (ClassId != 0 && std::find(Params.ClassIds.begin(), Params.ClassIds.end(), ClassId) == Params.ClassIds.end())
        Params.ClassIds.insert(Params.ClassIds.end(), ClassId);
    State->SetShipObject(new FContentPlayerShip(&Params));

    // The mission, as Story makes it; the hook is set first, so MissionStart is traced.
    GSessionPlayer = SessionId;
    GSessionSystem = SystemId;
    GSessionTick = 0;
    GSessionRecords = 0;
    GSessionScript = nullptr;
    GMissionDialogs.clear();
    FStoryMissionCreateParms CreateParms;
    CreateParms.PlayerId = SessionId;
    CreateParms.StoryEvents = nullptr;
    GOracleMissionTraceHook = &SessionHook;
    GSessionController = 0xffffffffu;
    unsigned int const Controller =
        pub::Controller::Create("Content.dll", Name, &CreateParms, pub::Controller::ABOVE_NORMAL);
    GSessionController = Controller;
    if (Controller == 0)
    {
        GOracleMissionTraceHook = nullptr;
        return 0;
    }
    SessionSend(EMessage::MissionStart, nullptr);
    return Controller;
}

// The start of a frame: the session's tick, then its scripted messages.
extern "C" __declspec(dllexport) void __cdecl OracleSimMissionFrame(unsigned int Tick)
{
    if (GSessionTrace == nullptr)
        return;
    GSessionTick = Tick;
    for (size_t Index = 0; Index < sizeof(OracleSessionSteps) / sizeof(OracleSessionSteps[0]); ++Index)
    {
        FOracleSessionStep const& Step = OracleSessionSteps[Index];
        if (Step.Tick != Tick)
            continue;
        if (Step.Kind == OracleSessionSpaceEnter)
        {
            FOracleSessionSpaceEnter Parms = {GSessionSystem, GSessionPlayer, 0u, 0};
            SessionSend(EMessage::SpaceEnter, &Parms);
        }
        else
        {
            FOracleSessionCommComplete Parms = {0u, GSessionSystem, 0u, CreateID(Step.Name), 0u};
            SessionSend(EMessage::CommComplete, &Parms);
        }
    }
}

// A delivery of the bus to the mission, before its handler (the harness's dispatch hook).
extern "C" __declspec(dllexport) void __cdecl OracleSimMissionDeliver(unsigned int Tick, unsigned int Flid,
                                                                     int MsgId)
{
    if (GSessionTrace == nullptr || Flid != GSessionController)
        return;
    std::fprintf(GSessionTrace, "{\"rec\":\"message\",\"tick\":%u,\"msg\":%u}\n", Tick,
                 static_cast<unsigned int>(MsgId));
    ++GSessionRecords;
}

// The end of a frame: the mission's objects, while the bus still has the mission.
extern "C" __declspec(dllexport) void __cdecl OracleSimMissionFrameEnd(unsigned int Tick, bool bAlive)
{
    if (GSessionObjects == nullptr || !bAlive || GSessionScript == nullptr)
        return;
    GSessionTick = Tick;
    std::map<unsigned int, FContentNpcShip*>::const_iterator Npc = GSessionScript->NpcObjects.begin();
    for (; Npc != GSessionScript->NpcObjects.end(); ++Npc)
    {
        if (Npc->second != nullptr)
            WriteObject("npc", Npc->second->Nickname.Data, Npc->first);
    }
    std::list<FMissionSolar>::const_iterator Solar = GSessionScript->MissionSolars.begin();
    for (; Solar != GSessionScript->MissionSolars.end(); ++Solar)
    {
        if (Solar->bSpawned == 1)
            WriteObject("solar", Solar->Nickname.Data, Solar->SpaceObjId);
    }
    std::list<FMissionLoot>::const_iterator Loot = GSessionScript->MissionLoots.begin();
    for (; Loot != GSessionScript->MissionLoots.end(); ++Loot)
    {
        if (Loot->bSpawned == 1)
            WriteObject("loot", Loot->Nickname.Data, Loot->SpaceObjId);
    }
}

// The session's end: `final` (the active list and the pending activations) while the bus has the
// mission, else `gone`; both files closed, the hook cleared. The number of trace records.
extern "C" __declspec(dllexport) int __cdecl OracleSimMissionEnd(unsigned int Tick, bool bAlive)
{
    if (GSessionTrace == nullptr)
        return -1;
    FILE* const File = GSessionTrace;
    if (bAlive && GSessionScript != nullptr)
    {
        std::fprintf(File, "{\"rec\":\"final\",\"tick\":%u,\"active\":[", Tick);
        std::list<FTriggerMap::iterator>::const_iterator ActiveIt = GSessionScript->ActiveTriggers.begin();
        for (bool bFirst = true; ActiveIt != GSessionScript->ActiveTriggers.end(); ++ActiveIt, bFirst = false)
            std::fprintf(File, "%s%u", bFirst ? "" : ",", (*ActiveIt)->first);
        std::fprintf(File, "],\"pending\":[");
        std::map<unsigned int, unsigned int>::const_iterator PendingIt =
            GSessionScript->PendingActivations.begin();
        for (bool bFirst = true; PendingIt != GSessionScript->PendingActivations.end(); ++PendingIt, bFirst = false)
            std::fprintf(File, "%s%u", bFirst ? "" : ",", PendingIt->first);
        std::fprintf(File, "]}\n");
    }
    else
    {
        std::fprintf(File, "{\"rec\":\"gone\",\"tick\":%u}\n", Tick);
    }
    ++GSessionRecords;
    GOracleMissionTraceHook = nullptr;
    std::fclose(File);
    GSessionTrace = nullptr;
    if (GSessionObjects != nullptr)
        std::fclose(GSessionObjects);
    GSessionObjects = nullptr;
    GSessionScript = nullptr;
    return GSessionRecords;
}
