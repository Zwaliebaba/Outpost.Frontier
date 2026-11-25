// Oracle instrumentation (design/migration-plan.md, Phase 0; work package P3.5c1, decision record
// 0008 question 9 and its Verification table): the Tier B mission trace, what the trigger engine
// does while Mission_01a runs its opening, for the Rust comparison
// (crates/game-logic/tests/mission_trace.rs). A new export, not part of the reference DLL's
// interface: only Oracle.exe sim calls it, for the session file CppSource/Oracle/mission_trace.txt
// (`dump mission_trace`), after the multiplayer start-up and the scenario's system load, so a game
// run never reaches it.
//
// The mission is made as OracleDumpMissionScripts makes its missions (the mission dialogs emptied,
// the CRT generator seeded with OracleTraceSeed, CreateInstance on a controller id no controller
// has, so the bus delivers it nothing) for a synthetic player 7 registered with
// ReplaceContentPlayerState in RELAYSEVEN_system, with no ship. The session hands it, through
// IDLL::HandleMessage, MissionStart at tick 0, then at every tick up to OracleTraceTicks the steps
// of OracleTraceSteps for that tick and a heartbeat of 1.0f / 30.0f. GOracleMissionTraceHook
// (MissionScript.h) writes the engine's records; the session writes a `message` record before
// each message and a `final` record at the end (the active list and the pending activations).
// The mission is then destroyed (DestroyInstance), the clamps its actions set are put back,
// player 7 is removed and the hook cleared.
//
// OracleDumpMissionTrace(Dir) writes <Dir>\mission.jsonl, one JSON object per line, in the
// format of game_logic::content::mission::trace:
//   {"rec":"message","tick":T,"msg":M}
//   {"rec":"activate","tick":T,"trigger":I}
//   {"rec":"fire","tick":T,"trigger":I,"event":E}
//   {"rec":"action","tick":T,"trigger":I,"index":N}
//   {"rec":"timer","tick":T,"trigger":I,"index":N}
//   {"rec":"rearm","tick":T,"trigger":I}
//   {"rec":"deactivate","tick":T,"trigger":I}
//   {"rec":"state","tick":T,"from":P,"to":D}
//   {"rec":"final","tick":T,"active":[I,...],"pending":[I,...]}
// and returns the number of records, -1 when the file cannot be opened or the mission is not
// made.

#include "content.h"
#include "ContentInternal.h"
#include "ContentPlayerState.h"
#include "MissionAct.h"
#include "MissionDialogs.h"
#include "MissionScript.h"
#include "Populators.h"
#include "SystemInfo.h"
#include "common_id.h"
#include "common_server.h"
#include "common_server_controller.h"

#include <cstdio>
#include <cstdlib>
#include <list>
#include <map>
#include <string>

namespace
{
    using pub::Controller::EMessage;

    unsigned int const OracleTraceSeed = 0x4f524143u;
    unsigned int const OracleTracePlayerId = 7u;
    unsigned int const OracleTraceController = 0x00ff0001u;
    char const* const OracleTraceSystem = "RELAYSEVEN_system";
    unsigned int const OracleTraceTicks = 900u;

    // One step of the session: the tick it comes at (before the tick's heartbeat), its kind and,
    // for a comm or a base, a nickname.
    enum EOracleTraceStep
    {
        OracleTraceMissionStart,
        OracleTraceSpaceEnter,
        OracleTraceCommComplete,
        OracleTraceSpaceExit,
        OracleTraceBaseEnter,
    };

    struct FOracleTraceStep
    {
        unsigned int Tick;
        EOracleTraceStep Kind;
        char const* Name;
    };

    // The session (the Rust test's STEPS, in the same order).
    FOracleTraceStep const OracleTraceSteps[] = {
        {0u, OracleTraceMissionStart, nullptr},
        {2u, OracleTraceSpaceEnter, nullptr},
        {200u, OracleTraceCommComplete, "DX_M01_0000_FREEPORT_7_BRIDGE"},
        {260u, OracleTraceCommComplete, "DX_M01_0005_Samura_SA_14"},
        {320u, OracleTraceCommComplete, "DX_M01_0010_FREEPORT_7_BRIDGE"},
        {600u, OracleTraceSpaceExit, nullptr},
        {620u, OracleTraceBaseEnter, "he01_01_Base"},
    };

    // The message parameter blocks the steps send (common_server_controller.h).
    struct FOracleSpaceEnter
    {
        unsigned int SystemId;
        unsigned int PlayerId;
        unsigned int LaunchObject;
        int LaunchKind;
    };

    struct FOracleCommComplete
    {
        unsigned int Listener;
        unsigned int SystemId;
        unsigned int Speaker;
        unsigned int Line;
        unsigned int Result;
    };

    struct FOracleSystemPlayer
    {
        unsigned int Id;
        unsigned int PlayerId;
    };

    FILE* GOracleTraceFile = nullptr;
    FMissionScript const* GOracleTraceScript = nullptr;
    unsigned int GOracleTraceTick = 0;
    int GOracleTraceRecords = 0;

    // The place of Action in its trigger's list.
    unsigned int OracleTraceActionIndex(FMissionScript const* Script, Act const* Action)
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

    void __cdecl OracleTraceHook(int Kind, FMissionScript const* Script, unsigned int TriggerId,
                                 unsigned int A, int B, Act const* Action)
    {
        if (GOracleTraceFile == nullptr || Script != GOracleTraceScript)
            return;
        FILE* const Out = GOracleTraceFile;
        unsigned int const Tick = GOracleTraceTick;
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
                         TriggerId, OracleTraceActionIndex(Script, Action));
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
        ++GOracleTraceRecords;
    }

    // The session's `message` record, then IDLL::HandleMessage.
    void OracleTraceSend(IDLL* Instance, EMessage Message, void const* Parms)
    {
        std::fprintf(GOracleTraceFile, "{\"rec\":\"message\",\"tick\":%u,\"msg\":%u}\n", GOracleTraceTick,
                     static_cast<unsigned int>(Message));
        ++GOracleTraceRecords;
        Instance->HandleMessage(Message, Parms);
    }

    void OracleTraceStep(IDLL* Instance, FOracleTraceStep const& Step, unsigned int SystemId)
    {
        switch (Step.Kind)
        {
        case OracleTraceMissionStart:
            OracleTraceSend(Instance, EMessage::MissionStart, nullptr);
            break;
        case OracleTraceSpaceEnter:
        {
            FOracleSpaceEnter Parms = {SystemId, OracleTracePlayerId, 0u, 0};
            OracleTraceSend(Instance, EMessage::SpaceEnter, &Parms);
            break;
        }
        case OracleTraceCommComplete:
        {
            FOracleCommComplete Parms = {0u, SystemId, 0u, CreateID(Step.Name), 0u};
            OracleTraceSend(Instance, EMessage::CommComplete, &Parms);
            break;
        }
        case OracleTraceSpaceExit:
        {
            FOracleSystemPlayer Parms = {SystemId, OracleTracePlayerId};
            OracleTraceSend(Instance, EMessage::SpaceExit, &Parms);
            break;
        }
        case OracleTraceBaseEnter:
        {
            unsigned int BaseId = 0;
            pub::GetBaseID(BaseId, Step.Name);
            FOracleSystemPlayer Parms = {BaseId, OracleTracePlayerId};
            OracleTraceSend(Instance, EMessage::BaseEnter, &Parms);
            break;
        }
        }
    }
}

extern "C" __declspec(dllexport) int __cdecl OracleDumpMissionTrace(char const* Dir)
{
    if (Dir == nullptr)
        return -1;
    std::string const Path = std::string(Dir) + "\\mission.jsonl";
    FILE* const File = std::fopen(Path.c_str(), "wb");
    if (File == nullptr)
        return -1;

    // RegisterOracleTracePlayer: player 7 in RELAYSEVEN_system, no ship.
    FContentPlayerState* const State = ReplaceContentPlayerState(OracleTracePlayerId);
    unsigned int SystemId = 0;
    pub::GetSystemID(SystemId, OracleTraceSystem);
    State->SystemRecord = FindSystemInfo(&SystemId);

    // The clamps the opening's actions set, put back at the end.
    unsigned char const Attack = GSpacePopAttackClamp;
    unsigned char const Encounter = SpacePopGetEncounterEnabled();
    unsigned char const Scan = GMissionScanClamp;
    bool const bGcs = ContentIsGcsClampEnabled();
    unsigned char const Hostile = SpacePopGetHostileClamp();
    unsigned char const Population = SpacePopGetPopulationEnabled();

    // MakeOracleTraceMission.
    GMissionDialogs.clear();
    std::srand(OracleTraceSeed);
    FStoryMissionCreateParms Parms;
    Parms.PlayerId = OracleTracePlayerId;
    Parms.StoryEvents = nullptr;
    IDLL* const Instance = CreateInstance(OracleTraceController, "Mission_01a", &Parms);
    FMissionScript* const Script = dynamic_cast<FMissionScript*>(Instance);
    int Result = -1;
    if (Script != nullptr)
    {
        GOracleTraceFile = File;
        GOracleTraceScript = Script;
        GOracleTraceRecords = 0;
        GOracleMissionTraceHook = &OracleTraceHook;
        float const Frame = 1.0f / 30.0f;
        for (unsigned int Tick = 0; Tick <= OracleTraceTicks; ++Tick)
        {
            GOracleTraceTick = Tick;
            for (size_t Index = 0; Index < sizeof(OracleTraceSteps) / sizeof(OracleTraceSteps[0]); ++Index)
            {
                if (OracleTraceSteps[Index].Tick == Tick)
                    OracleTraceStep(Instance, OracleTraceSteps[Index], SystemId);
            }
            if (Tick > 0)
            {
                float Delta = Frame;
                OracleTraceSend(Instance, EMessage::Heartbeat, &Delta);
            }
        }
        // The `final` record: the active list in order, the pending activations' targets.
        std::fprintf(File, "{\"rec\":\"final\",\"tick\":%u,\"active\":[", GOracleTraceTick);
        std::list<FTriggerMap::iterator>::const_iterator ActiveIt = Script->ActiveTriggers.begin();
        for (bool bFirst = true; ActiveIt != Script->ActiveTriggers.end(); ++ActiveIt, bFirst = false)
            std::fprintf(File, "%s%u", bFirst ? "" : ",", (*ActiveIt)->first);
        std::fprintf(File, "],\"pending\":[");
        std::map<unsigned int, unsigned int>::const_iterator PendingIt = Script->PendingActivations.begin();
        for (bool bFirst = true; PendingIt != Script->PendingActivations.end(); ++PendingIt, bFirst = false)
            std::fprintf(File, "%s%u", bFirst ? "" : ",", PendingIt->first);
        std::fprintf(File, "]}\n");
        ++GOracleTraceRecords;
        Result = GOracleTraceRecords;
        GOracleMissionTraceHook = nullptr;
        GOracleTraceFile = nullptr;
        GOracleTraceScript = nullptr;
    }
    std::fclose(File);
    if (Instance != nullptr)
        DestroyInstance(Instance);

    SpacePopSetAttackClamp(Attack);
    SpacePopSetEncounterEnabled(Encounter);
    GMissionScanClamp = Scan;
    ContentSetGcsClampEnabled(bGcs ? 1 : 0);
    SpacePopSetHostileClamp(Hostile);
    SpacePopSetPopulationEnabled(Population);
    SpacePopClearExclusionSpheres();
    GMissionDialogs.clear();
    RemoveContentPlayerState(OracleTracePlayerId);
    return Result;
}
