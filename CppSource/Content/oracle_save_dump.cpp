// Oracle instrumentation (P3.5b, decision record 0008 point 10 and its Verification table, Gate
// G3): the player-save sections of the story missions and of Story as Content.dll writes them,
// and as it restores the saves the Rust port wrote, for the Rust comparison
// (crates/game-logic/tests/mission_saves.rs). Two new exports, not part of the reference DLL's
// interface: only Oracle.exe sim calls them, for the session files
// CppSource/Oracle/mission_saves.txt (`dump mission_saves`) and
// CppSource/Oracle/mission_save_loads.txt (`dump mission_save_loads`), right after the
// multiplayer start-up, so a game run never reaches them.
//
// Every mission is made as OracleDumpMissionScripts makes it (the mission dialogs emptied, the
// CRT generator seeded with OracleSaveSeed, CreateInstance for a synthetic player 7 registered
// with ReplaceContentPlayerState in he01, on a controller id no controller has), and destroyed
// (DestroyInstance) when done. Player 7 is removed at the end.
//
// The save is the body of FMissionScript's PlayerSave case without the message's gates (the
// session's server is a multiplayer one, and the gates are the run time's): "\n[MissionState]\n",
// SaveMissionState, then unless a trigger save is pending SaveMission and SaveRandEncs (there is
// no ship, so no snapshot), then "\n[BStoryMissionDone]\n"; the pending flag is cleared. The load
// is the body of the PlayerLoad case without the gates: LoadMissionState from the reader's start,
// then LoadMissionSave. The six clamps a save writes are put back after each mission.
//
// The three states each mission is written in (design/sim-records.md):
//   constructed  right after the constructor;
//   advanced     every trigger with a condition made active, in id order, its condition's
//                saved values set from a table by its place k; the script's story
//                cue, NN path, cut scenes, ambient scene and friendly-fire labels, the mission
//                object's respawn records, solars, marks, invulnerables, decloaked names and
//                attitudes, two random encounters' states and the six clamps set to fixed
//                values (Advance below);
//   pending      a trigger save pending for the first trigger.
//
// OracleDumpMissionSaves(Dir) writes <Dir>\save_<mission>_<state>.ini (binary: the "\n" the
// format strings write) and <Dir>\story_constructed.ini (Story's PlayerSave right after its
// constructor, for player 7).
//
// OracleLoadMissionSaves(Dir) reads the port's saves from <Dir>\rust\: for each mission and each
// of the states constructed, advanced, pending, restored and raw, save_<mission>_<state>.ini is
// loaded into a newly made mission (the generator seeded again before the load) and
// <Dir>\loaded_<mission>_<state>.txt written: the restored state (DumpLoaded) then the save of
// it; then one Story loads story_<n>.ini, n = 0 to 5, in order, and writes
// <Dir>\story_loaded_<n>.txt: its save and the player's mission number.
//
// Both return the number of files written, -1 when a file cannot be opened or a mission or the
// Story is not made.

#include "content.h"
#include "ContentInternal.h"
#include "ContentPlayerState.h"
#include "Guides.h"
#include "MissionCnd.h"
#include "MissionDialogs.h"
#include "MissionObject.h"
#include "MissionScript.h"
#include "Populators.h"
#include "RandomEncounters.h"
#include "SystemInfo.h"
#include "common_id.h"
#include "common_ini.h"
#include "common_server.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <string>
#include <vector>

namespace
{
    // The seed of every mission's parse and load (the harness's default seed).
    unsigned int const OracleSaveSeed = 0x4f524143u;

    // The synthetic player, the first mission controller id and the Story's (no controller has
    // these ids).
    unsigned int const OracleSavePlayerId = 7u;
    unsigned int const OracleSaveControllerBase = 0x00ff0001u;
    unsigned int const OracleSaveStoryController = 0x00ff0100u;

    char const* const OracleSaveMissions[] = {"Mission_01a", "Mission_01b", "Mission_02", "Mission_03",
                                              "Mission_04",  "Mission_05",  "Mission_06", "Mission_07",
                                              "Mission_08",  "Mission_09",  "Mission_10", "Mission_11",
                                              "Mission_12",  "Mission_13"};
    int const OracleSaveMissionCount = 14;

    // The saved float values of the advanced state: exact float literals, no arithmetic.
    float const OracleSaveFloats[12] = {0.0078125f, 0.0234375f, 0.1f,    123.456f, -0.6f,       31476.25f,
                                        3.4028235e38f, 1e-7f,  2500.0f, 0.33333334f, 16777216.0f, -0.0f};

    // The systems of the NPC system conditions' pending ships in the advanced state.
    unsigned int const OracleSaveSystems[3] = {7u, 16777217u, 4000000007u};

    // Story saves the port writes for OracleLoadMissionSaves.
    int const OracleSaveStoryCount = 6;

    float OracleSaveFloat(int Index)
    {
        return OracleSaveFloats[Index % 12];
    }

    // Story's message parameter blocks (Story.cpp:45-55).
    struct FOracleStorySave
    {
        unsigned int PlayerId;
        FILE* File;
    };

    struct FOracleStoryLoad
    {
        unsigned int PlayerId;
        INI_Reader* Reader;
    };

    struct FOracleClamps
    {
        unsigned char Attack;
        unsigned char Encounter;
        unsigned char Scan;
        bool bGcs;
        unsigned char Hostile;
        unsigned char Population;
    };

    FOracleClamps GetOracleClamps()
    {
        FOracleClamps Out;
        Out.Attack = GSpacePopAttackClamp;
        Out.Encounter = SpacePopGetEncounterEnabled();
        Out.Scan = GMissionScanClamp;
        Out.bGcs = ContentIsGcsClampEnabled();
        Out.Hostile = SpacePopGetHostileClamp();
        Out.Population = SpacePopGetPopulationEnabled();
        return Out;
    }

    void SetOracleClamps(FOracleClamps const& In)
    {
        SpacePopSetAttackClamp(In.Attack);
        SpacePopSetEncounterEnabled(In.Encounter);
        GMissionScanClamp = In.Scan;
        ContentSetGcsClampEnabled(In.bGcs ? 1 : 0);
        SpacePopSetHostileClamp(In.Hostile);
        SpacePopSetPopulationEnabled(In.Population);
    }

    void OracleSaveHex(FILE* Out, char const* Data)
    {
        if (Data == nullptr)
            return;
        for (size_t Index = 0; Data[Index] != '\0'; ++Index)
            std::fprintf(Out, "%02x", static_cast<unsigned char>(Data[Index]));
    }

    // The draws since the seeding, as OracleDumpMissionScripts counts them.
    int OracleSaveCountDraws()
    {
        int Probe[4];
        for (int Index = 0; Index < 4; ++Index)
            Probe[Index] = std::rand();
        std::srand(OracleSaveSeed);
        std::vector<int> Sequence;
        Sequence.reserve(0x10004);
        for (int Index = 0; Index < 0x10004; ++Index)
            Sequence.push_back(std::rand());
        for (int Draws = 0; Draws <= 0x10000; ++Draws)
        {
            if (Sequence[Draws] == Probe[0] && Sequence[Draws + 1] == Probe[1] &&
                Sequence[Draws + 2] == Probe[2] && Sequence[Draws + 3] == Probe[3])
                return Draws;
        }
        return -1;
    }

    // The mission of index Index, made as OracleDumpMissionScripts makes it.
    FMissionScript* MakeOracleMission(int Index, IDLL** OutInstance)
    {
        GMissionDialogs.clear();
        std::srand(OracleSaveSeed);
        FStoryMissionCreateParms Parms;
        Parms.PlayerId = OracleSavePlayerId;
        Parms.StoryEvents = nullptr;
        IDLL* const Instance = CreateInstance(OracleSaveControllerBase + static_cast<unsigned int>(Index),
                                              OracleSaveMissions[Index], &Parms);
        *OutInstance = Instance;
        FMissionScript* const Script = dynamic_cast<FMissionScript*>(Instance);
        if (Script == nullptr)
            return nullptr;
        return Script;
    }

    // The advanced state's values of the condition of the k-th active trigger.
    void AdvanceCondition(Cnd* Condition, int K)
    {
        if (CndDestroyed* const C = dynamic_cast<CndDestroyed*>(Condition))
            C->Count = K + 1;
        else if (CndLootAcquired* const C = dynamic_cast<CndLootAcquired*>(Condition))
            C->Count = K + 1;
        else if (CndProjHit* const C = dynamic_cast<CndProjHit*>(Condition))
            C->Count = K + 1;
        else if (CndProjHitShipToLbl* const C = dynamic_cast<CndProjHitShipToLbl*>(Condition))
            C->Count = K + 1;
        else if (CndDistShip* const C = dynamic_cast<CndDistShip*>(Condition))
        {
            C->TickTimer = OracleSaveFloat(K);
            C->LastDistanceSq = OracleSaveFloat(K + 1);
        }
        else if (CndDistVec* const C = dynamic_cast<CndDistVec*>(Condition))
        {
            C->TickTimer = OracleSaveFloat(K);
            C->LastDistanceSq = OracleSaveFloat(K + 1);
        }
        else if (CndDistVecLbl* const C = dynamic_cast<CndDistVecLbl*>(Condition))
            C->TickTimer = OracleSaveFloat(K + 2);
        else if (CndTetherBroke* const C = dynamic_cast<CndTetherBroke*>(Condition))
        {
            C->Field_0x90 = OracleSaveFloat(K + 3);
            C->Field_0x88 = OracleSaveFloat(K + 4);
        }
        else if (CndTimer* const C = dynamic_cast<CndTimer*>(Condition))
            C->Elapsed = OracleSaveFloat(K + 5);
        else if (CndNpcSystemEnter* const C = dynamic_cast<CndNpcSystemEnter*>(Condition))
        {
            C->Pending = C->Ships;
            int Index = 0;
            for (std::list<FNpcSystemEntry>::iterator It = C->Pending.begin(); It != C->Pending.end(); ++It)
                It->SystemId = OracleSaveSystems[(K + Index++) % 3];
        }
        else if (CndNpcSystemExit* const C = dynamic_cast<CndNpcSystemExit*>(Condition))
        {
            C->Pending = C->Ships;
            int Index = 0;
            for (std::list<FNpcSystemEntry>::iterator It = C->Pending.begin(); It != C->Pending.end(); ++It)
                It->SystemId = OracleSaveSystems[(K + Index++) % 3];
        }
    }

    // The advanced state (design/sim-records.md, "mission saves").
    void Advance(FMissionScript* S)
    {
        // The triggers: every one with a condition, in id order.
        int K = 0;
        for (FTriggerMap::iterator Trigger = S->TriggerMap.begin(); Trigger != S->TriggerMap.end(); ++Trigger)
        {
            if (Trigger->second.Condition == nullptr)
                continue;
            S->ActiveTriggers.push_back(Trigger);
            AdvanceCondition(Trigger->second.Condition, K);
            ++K;
        }

        // The script.
        S->StoryCue = 7;
        S->NnPath.bActive = 1;
        S->NnPath.TravelIds = 1234;
        S->NnPath.TargetIds = -5;
        S->NnPath.Position.x = 1.5f;
        S->NnPath.Position.y = -2.25f;
        S->NnPath.Position.z = 100000.125f;
        S->NnPath.Label = "oracle_nn_target";
        S->NnPath.SystemId = 4000000007u;
        char const* const RtcPaths[3] = {"missions\\oracle\\rtc_a.thn", "missions\\oracle\\rtc_b.thn",
                                         "missions\\oracle\\rtc_c.thn"};
        unsigned char const RtcFinished[3] = {0, 1, 0};
        unsigned char const Rtc95[3] = {1, 0, 0};
        for (int Index = 0; Index < 3; ++Index)
        {
            FStoryMissionListNode_0x2f4 Script;
            std::strcpy(Script.Path, RtcPaths[Index]);
            Script.PathLength = static_cast<unsigned int>(std::strlen(RtcPaths[Index]));
            Script.Field_0x94 = RtcFinished[Index];
            Script.Field_0x95 = Rtc95[Index];
            S->List_0x2f4.push_back(Script);
        }
        FMissionAmbientScene Scene;
        Scene.Path = "scripts\\oracle\\ambi.thn";
        Scene.Handle = -1;
        Scene.RoomId = 0x1234u;
        Scene.BaseId = 4000000007u;
        S->AmbientScenes.push_back(Scene);
        TString<0x30> Label;
        Label = "oracle_ff_a";
        S->FriendlyFireOffLabels.push_back(Label);
        Label = "oracle_ff_b";
        S->FriendlyFireOffLabels.push_back(Label);

        // The respawn ship records: a copy of the first [MsnShip] record with its first ship (a
        // position, a priority, hit points and the first object list's directives), and a copy
        // of the last with its first ship arriving from an object (the first meta behaviour's
        // first object as its guide).
        if (!S->MissionShips.empty())
        {
            FMissionShipGroup Front = S->MissionShips.front();
            std::vector<FMissionShip> FrontShips;
            if (!Front.Ships.empty())
                FrontShips.push_back(Front.Ships.front());
            Front.Ships = FrontShips;
            Front.ArrivalObjId = 0;
            Front.Position.x = -31476.25f;
            Front.Position.y = 150.0f;
            Front.Position.z = 0.1f;
            if (!Front.Ships.empty())
            {
                FMissionShip& Ship = Front.Ships.front();
                Ship.Priority = 3;
                Ship.HitPoints = 450;
                if (!S->ObjLists.empty())
                    Ship.Directives = S->ObjLists.begin()->second;
            }
            S->RespawnShips.push_back(Front);

            FMissionShipGroup Back = S->MissionShips.back();
            std::vector<FMissionShip> BackShips;
            if (!Back.Ships.empty())
                BackShips.push_back(Back.Ships.front());
            Back.Ships = BackShips;
            Back.ArrivalObjId = 0x40000123u;
            if (!Back.Ships.empty())
            {
                FMissionShip& Ship = Back.Ships.front();
                Ship.Priority = -2;
                if (!S->MetaBehaviors.empty() && !S->MetaBehaviors.begin()->second.Items.empty())
                    CreateGuide(S->MetaBehaviors.begin()->second.Items.front(), &Ship.Guide.Pointer);
            }
            S->RespawnShips.push_back(Back);
        }

        // One random-encounter respawn record.
        {
            FMissionRandEncRespawn Record;
            Record.EncounterId.Id = S->RandEncs.empty() ? CreateID("oracle_enc") : S->RandEncs.front().NicknameId.Id;
            Record.Count = 2;
            Record.Priority = 3;
            Record.HitPoints = 450;
            Record.Placement.Translation.X = 100.5f;
            Record.Placement.Translation.Y = -0.0f;
            Record.Placement.Translation.Z = 3000000.0f;
            FRandEncShipChoice First;
            First.ShipClasses.push_back(0x11u);
            First.ShipClasses.push_back(0x22u);
            First.Weight = 1.0f;
            Record.Choices.push_back(First);
            FRandEncShipChoice Second;
            Second.Weight = 1.0f;
            Record.Choices.push_back(Second);
            TString<0x30> Name;
            Name = "oracle_rand_a";
            Record.Names.push_back(Name);
            Name = "oracle_rand_b";
            Record.Names.push_back(Name);
            if (!S->ObjLists.empty())
                Record.Directives = S->ObjLists.begin()->second;
            S->RandEncRespawns.push_back(Record);
        }

        // The mission object's lists.
        TString<0x30> Name;
        Name = "oracle_solar";
        S->RespawnSolars.push_back(Name);
        Name = "oracle_mark_a";
        S->MarkObjNames.push_back(Name);
        Name = "oracle_mark_b";
        S->MarkObjNames.push_back(Name);
        Name = "oracle_cloak";
        S->DecloakedNames.push_back(Name);
        FMissionInvulnerable Invulnerable;
        Invulnerable.Nickname = "oracle_inv_a";
        Invulnerable.Duration = 12.5f;
        Invulnerable.bParam3 = 1;
        S->Invulnerables.push_back(Invulnerable);
        Invulnerable.Nickname = "oracle_inv_b";
        Invulnerable.Duration = 0.0234375f;
        Invulnerable.bParam3 = 0;
        S->Invulnerables.push_back(Invulnerable);
        FMissionVibeInfo Vibe = {};
        Vibe.ObjectA = "oracle_vibe_a";
        Vibe.ObjectB = "Player";
        Vibe.RepA = -1;
        Vibe.RepB = -1;
        Vibe.Attitude = -0.6f;
        S->VibeInfos.push_back(Vibe);
        FMissionVibeInfo Second = {};
        Second.ObjectB = "oracle_vibe_b";
        Second.RepA = 17;
        Second.RepB = -1;
        Second.Attitude = 0.1f;
        S->VibeInfos.push_back(Second);
        FMissionVibeInfo Third = {};
        Third.ObjectA = "oracle_vibe_c";
        Third.RepA = -1;
        Third.RepB = static_cast<int>(4000000007u);
        Third.Attitude = -0.0f;
        S->VibeInfos.push_back(Third);

        // Two random encounters' states.
        std::list<FMissionRandEnc>::iterator Enc = S->RandEncs.begin();
        if (Enc != S->RandEncs.end())
        {
            Enc->bEnabled = 1;
            Enc->NpcCount = 3;
            ++Enc;
        }
        if (Enc != S->RandEncs.end())
        {
            Enc->bEnabled = 0;
            Enc->NpcCount = 5;
        }

        // The clamps.
        SpacePopSetAttackClamp(1);
        SpacePopSetEncounterEnabled(0);
        GMissionScanClamp = 1;
        ContentSetGcsClampEnabled(1);
        SpacePopSetHostileClamp(0);
        SpacePopSetPopulationEnabled(0);
    }

    // The PlayerSave body without its gates.
    void WriteOracleSave(FMissionScript* S, FILE* File)
    {
        std::fprintf(File, "\n[MissionState]\n");
        S->SaveMissionState(File);
        if (S->bTriggerSavePending == 0)
        {
            S->SaveMission(File);
            S->SaveRandEncs(File);
        }
        std::fprintf(File, "\n[BStoryMissionDone]\n");
        S->bTriggerSavePending = 0;
    }

    // The restored state the save does not show, then "=== save" and the save of it.
    void DumpLoaded(FILE* Out, FMissionScript* S, int Draws)
    {
        std::fprintf(Out, "script field_0x2ec=%u initially_active=", static_cast<unsigned int>(S->Field_0x2ec));
        for (std::map<unsigned int, unsigned int>::iterator It = S->InitiallyActive.begin();
             It != S->InitiallyActive.end(); ++It)
            std::fprintf(Out, "%s%u:%u", It == S->InitiallyActive.begin() ? "" : ",", It->first, It->second);
        std::fprintf(Out, " active=%u pending_activations=%u save_pending=%u save_trigger=%u nn_state=%d draws=%d\n",
                     static_cast<unsigned int>(S->ActiveTriggers.size()),
                     static_cast<unsigned int>(S->PendingActivations.size()),
                     static_cast<unsigned int>(S->bTriggerSavePending), S->SaveTriggerId, S->NnPath.State, Draws);
        for (std::map<unsigned int, unsigned int>::iterator It = S->InitiallyActive.begin();
             It != S->InitiallyActive.end(); ++It)
        {
            FTriggerMap::iterator Trigger = S->TriggerMap.find(It->first);
            if (Trigger == S->TriggerMap.end() || Trigger->second.Condition == nullptr)
                continue;
            std::fprintf(Out, "cnd trigger=%u text=", It->first);
            Trigger->second.Condition->Save(Out);
            std::fprintf(Out, "|\n");
        }
        unsigned int Group = 0;
        for (std::list<FMissionShipGroup>::iterator It = S->RespawnShips.begin(); It != S->RespawnShips.end();
             ++It, ++Group)
        {
            for (size_t Index = 0; Index < It->Ships.size(); ++Index)
            {
                FMissionShip const& Ship = It->Ships[Index];
                std::fprintf(Out, "ship group=%u index=%u init_objectives=%u init_meta=%u guide=%u directives=%u\n",
                             Group, static_cast<unsigned int>(Index), Ship.InitObjectives.Id,
                             Ship.InitMetaBehavior.Id, Ship.Guide.Pointer != nullptr ? 1u : 0u,
                             static_cast<unsigned int>(Ship.Directives.Items.size()));
            }
        }
        std::fprintf(Out, "solar_reps=");
        for (size_t Index = 0; Index < S->SolarReps.size(); ++Index)
            std::fprintf(Out, "%s%d", Index == 0 ? "" : ",", S->SolarReps[Index]);
        std::fprintf(Out, "\n");
        for (std::list<FMissionRandEnc>::iterator It = S->RandEncs.begin(); It != S->RandEncs.end(); ++It)
        {
            std::fprintf(Out, "rand_enc nickname=");
            OracleSaveHex(Out, It->Nickname.Data);
            std::fprintf(Out, " enabled=%u npc_count=%u params=%u\n", static_cast<unsigned int>(It->bEnabled),
                         It->NpcCount, It->Params.Pointer != nullptr ? 1u : 0u);
        }
        std::fprintf(Out, "=== save\n");
        WriteOracleSave(S, Out);
    }

    // Player 7 in he01, as OracleDumpMissionScripts registers it.
    void RegisterOraclePlayer()
    {
        FContentPlayerState* const State = ReplaceContentPlayerState(OracleSavePlayerId);
        unsigned int He01 = 0;
        pub::GetSystemID(He01, "he01");
        State->SystemRecord = FindSystemInfo(&He01);
    }

    std::string OracleSavePath(std::string const& Dir, std::string const& Name)
    {
        return Dir + "\\" + Name;
    }

    IDLL* MakeOracleStory()
    {
        pub::Controller::FStoryCreateParms Parms;
        Parms.PlayerId = OracleSavePlayerId;
        return CreateInstance(OracleSaveStoryController, "Story", &Parms);
    }

    void WriteOracleStory(IDLL* Story, FILE* File)
    {
        FOracleStorySave Message;
        Message.PlayerId = OracleSavePlayerId;
        Message.File = File;
        Story->HandleMessage(pub::Controller::EMessage::PlayerSave, &Message);
    }
}

// The C++ writer: the fourteen missions in their three states, and Story after its constructor.
extern "C" __declspec(dllexport) int __cdecl OracleDumpMissionSaves(char const* Dir)
{
    if (Dir == nullptr)
        return -1;
    std::string const Root(Dir);
    char const* const States[3] = {"constructed", "advanced", "pending"};
    RegisterOraclePlayer();
    FOracleClamps const StartClamps = GetOracleClamps();
    int Files = 0;
    for (int Index = 0; Index < OracleSaveMissionCount && Files >= 0; ++Index)
    {
        for (int State = 0; State < 3; ++State)
        {
            IDLL* Instance = nullptr;
            FMissionScript* const Script = MakeOracleMission(Index, &Instance);
            if (Script == nullptr)
            {
                Files = -1;
                break;
            }
            if (State == 1)
                Advance(Script);
            if (State == 2)
            {
                Script->bTriggerSavePending = 1;
                Script->SaveTriggerId = Script->TriggerMap.empty() ? 0u : Script->TriggerMap.begin()->first;
            }
            std::string const Path =
                OracleSavePath(Root, std::string("save_") + OracleSaveMissions[Index] + "_" + States[State] + ".ini");
            FILE* const File = std::fopen(Path.c_str(), "wb");
            if (File == nullptr)
                Files = -1;
            else
            {
                WriteOracleSave(Script, File);
                std::fclose(File);
                ++Files;
            }
            SetOracleClamps(StartClamps);
            DestroyInstance(Instance);
            if (Files < 0)
                break;
        }
    }
    if (Files >= 0)
    {
        IDLL* const Story = MakeOracleStory();
        FILE* const File = std::fopen(OracleSavePath(Root, "story_constructed.ini").c_str(), "wb");
        if (Story == nullptr || File == nullptr)
            Files = -1;
        else
        {
            WriteOracleStory(Story, File);
            ++Files;
        }
        if (File != nullptr)
            std::fclose(File);
        if (Story != nullptr)
            DestroyInstance(Story);
    }
    GMissionDialogs.clear();
    RemoveContentPlayerState(OracleSavePlayerId);
    return Files;
}

// The C++ reader of the port's saves in <Dir>\rust\.
extern "C" __declspec(dllexport) int __cdecl OracleLoadMissionSaves(char const* Dir)
{
    if (Dir == nullptr)
        return -1;
    std::string const Root(Dir);
    std::string const Rust = Root + "\\rust";
    char const* const States[5] = {"constructed", "advanced", "pending", "restored", "raw"};
    RegisterOraclePlayer();
    FOracleClamps const StartClamps = GetOracleClamps();
    int Files = 0;
    for (int Index = 0; Index < OracleSaveMissionCount && Files >= 0; ++Index)
    {
        for (int State = 0; State < 5; ++State)
        {
            std::string const Name = std::string(OracleSaveMissions[Index]) + "_" + States[State];
            IDLL* Instance = nullptr;
            FMissionScript* const Script = MakeOracleMission(Index, &Instance);
            INI_Reader Reader;
            FILE* Out = nullptr;
            if (Script != nullptr && Reader.open(OracleSavePath(Rust, "save_" + Name + ".ini").c_str(), false))
                Out = std::fopen(OracleSavePath(Root, "loaded_" + Name + ".txt").c_str(), "wb");
            if (Out == nullptr)
                Files = -1;
            else
            {
                std::srand(OracleSaveSeed);
                Reader.reset();
                Script->LoadMissionState(&Reader);
                Script->LoadMissionSave(&Reader);
                int const Draws = OracleSaveCountDraws();
                DumpLoaded(Out, Script, Draws);
                std::fclose(Out);
                ++Files;
            }
            Reader.close();
            SetOracleClamps(StartClamps);
            if (Instance != nullptr)
                DestroyInstance(Instance);
            if (Files < 0)
                break;
        }
    }
    IDLL* const Story = Files >= 0 ? MakeOracleStory() : nullptr;
    if (Files >= 0 && Story == nullptr)
        Files = -1;
    for (int Index = 0; Index < OracleSaveStoryCount && Files >= 0; ++Index)
    {
        char Name[0x40];
        std::snprintf(Name, sizeof(Name), "story_%d.ini", Index);
        INI_Reader Reader;
        FILE* Out = nullptr;
        if (Reader.open(OracleSavePath(Rust, Name).c_str(), false))
        {
            std::snprintf(Name, sizeof(Name), "story_loaded_%d.txt", Index);
            Out = std::fopen(OracleSavePath(Root, Name).c_str(), "wb");
        }
        if (Out == nullptr)
            Files = -1;
        else
        {
            FOracleStoryLoad Message;
            Message.PlayerId = OracleSavePlayerId;
            Message.Reader = &Reader;
            Story->HandleMessage(pub::Controller::EMessage::PlayerLoad, &Message);
            WriteOracleStory(Story, Out);
            FContentPlayerState* const State = FindContentPlayerState(OracleSavePlayerId);
            std::fprintf(Out, "mission_number=%u\n", State != nullptr ? State->MissionNumber : 0xffffffffu);
            std::fclose(Out);
            ++Files;
        }
        Reader.close();
    }
    if (Story != nullptr)
        DestroyInstance(Story);
    GMissionDialogs.clear();
    RemoveContentPlayerState(OracleSavePlayerId);
    return Files;
}
