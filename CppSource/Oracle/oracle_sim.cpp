// Oracle.exe sim, the scenario and replay oracle (work package P2.3,
// design/adr/0003-simulation-oracle.md; usage and scenario format in
// design/reference-build.md section 10; record layouts in design/sim-records.md).
//
//   Oracle.exe sim <data root> <scenario file> <out folder> [--runs N] [--seed S]
//
// One run starts a headless multiplayer server in this process the way OutpostServer.exe
// does (NeuronRuntime::Startup with NEURONCOMPONENTSsrv.ini, the data path, SetIsMPServer,
// IServerImpl::Startup with Content.dll's controllers), with SimClientStub.dll as its
// client interface and as the scenario ships' controller. It loads the scenario system,
// spawns the scenario objects, then runs the given number of server frames with a fixed
// frame time, a fixed clock and a fixed rand() seed, applying the scenario's commands and
// recording each frame through the hooks of include/oracle_sim_hooks.h. Its output is made
// from the game data and must not be committed.

#include <windows.h>
#include <objbase.h>   // CoInitialize

#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "oracle_support.h"
#include "oracle_sim.h"
#include "../OutpostFrontier/Client/oracle_json.h"

#include "neuroncomponents.h"          // FDUMP
#include "neuronruntime_core.h"        // NeuronRuntime::Startup / OpenData, CriticalWarningFn
#include "common_archetype.h"          // Archetype::Ship / Engine
#include "common_cequip.h"             // CEquipTraverser, CEGun, CEEngine
#include "common_cobjdb.h"             // IObjDB::FindById
#include "common_cobject.h"            // CObject, CSimple, CEqObj, CShip
#include "common_csys.h"               // Matrix
#include "common_iface.h"              // IObjRW
#include "common_id.h"                 // CreateID
#include "common_ini.h"                // INI_Reader, Vector
#include "common_misc.h"               // DamageList
#include "common_net.h"                // SetIsMPServer
#include "common_newchar.h"            // pub::StateGraph
#include "common_pub.h"                // pub::AI ops
#include "common_server.h"             // pub::AI::SubmitState / SubmitDirective, pub::SpaceObj
#include "common_server_controller.h"  // pub::Controller::Create
#include "common_server_types.h"       // SStartupInfo
#include "common_timing.h"             // Timing::UpdateGlobalTime
#include "common_universe.h"           // Universe::IZone::GetDragModifier (psi.jsonl "ctl")
#include "server_cobject.h"            // pub::SpaceObj::ShipInfo / Create
#include "server_iserverimpl.h"        // IServerImpl (the `Server` export is looked up at run time)
#include "oracle_sim_hooks.h"
#include "oracle_sim_ai.h"
#include "oracle_sim_rand.h"
#include "oracle_sim_fault.h"
#include "oracle_sim_fields.h"
#include "oracle_sim_env.h"
#include "../include/common_ivp_controller.h" // IVP_Controller_Golem (unit_cores.jsonl)

// Server.dll, IServerImpl.cpp (Oracle instrumentation): StarSystem::load on one system.
extern "C" __declspec(dllimport) bool __cdecl OracleSimLoadSystem(unsigned int SystemId);
// Server.dll, IServerImpl.cpp (Oracle instrumentation, P3.3): every system loaded and its
// solars written as JSON lines (decision record 0007 point 11).
extern "C" __declspec(dllimport) int __cdecl OracleDumpSystemLoads(char const* Path);
// Server.dll, IServerImpl.cpp (Oracle instrumentation, P3.3): a scenario's `player` joins the
// system with the ship the harness made (decision record 0007 point 11).
extern "C" __declspec(dllimport) bool __cdecl OracleSimAddPlayer(unsigned int SessionId, unsigned int SystemId,
                                                                 unsigned int ShipId);
// Server.dll's other Oracle export: every controller's heartbeat (session.jsonl `heartbeats`).
typedef void(__cdecl* FOracleSimHeartbeatFn)(void* Context, unsigned int Flid, unsigned int Priority,
                                              char const* Module, float Interval, float Phase,
                                              bool bDeferred);
extern "C" __declspec(dllimport) void __cdecl OracleSimControllerHeartbeats(FOracleSimHeartbeatFn Fn,
                                                                            void* Context);

namespace
{
    using oracle::Format;

    // The record format version written into session.jsonl's start record.
    unsigned int const kSimVersion = 2;
    // OutpostServer.exe's server-thread control word (SetupServerFpu without the unmask
    // option): every exception masked, 53-bit precision, round to nearest.
    unsigned int const kServerFpuWord = 0x9001f;
    unsigned int const kServerFpuMask = 0xfffff;

    // ------------------------------------------------------------------------------------
    // Scenario
    // ------------------------------------------------------------------------------------

    struct FObjectDesc
    {
        std::string Name;
        bool bShip = true;
        std::string Archetype;   // ship archetype nickname
        std::string Loadout;     // loadout nickname, empty for none
        std::string Nickname;    // solar: the system object's nickname
        double Pos[3] = {0.0, 0.0, 0.0};
        double Orient[9] = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
        double Vel[3] = {0.0, 0.0, 0.0};
        double AngVel[3] = {0.0, 0.0, 0.0};
        std::string StateGraph;  // empty: no behaviour
        std::string Pilot;       // P2.5: the pilot personality's nickname, empty for the default
        std::string Faction;     // P2.5: the reputation group of the ship's vibe, empty for none
        int HitPts = -1;
        bool bPsi = false;
        bool bEscort = false;    // P2.5 escort: the state graph's ESCORT variant (with `ai`)
        // P3.6c: `content`, the ship is Content's NPC ship (FSystemInfo::CreateNpcShip through
        // Content.dll's OracleSimCreateNpcShip), its manager's directive and content callback;
        // `flee x y z distance` gives it that flee position (FContentNpcShip::SetFleePosition).
        bool bContent = false;
        bool bContentFlee = false;
        double ContentFlee[3] = {0.0, 0.0, 0.0};
        double ContentFleeDistance = 0.0;
        int Line = 0;
    };

    struct FCommand
    {
        unsigned int Tick = 0;
        std::string Object;
        std::vector<std::string> Args;
        std::string Text;
        int Line = 0;
        size_t Order = 0;
    };

    // P3.3 (decision record 0007 point 11): `player <session> <ship>`, a session that joins the
    // scenario's system after the objects are spawned, its observer following the ship.
    struct FPlayerDesc
    {
        unsigned int Session = 0;
        std::string Object;
        int Line = 0;
    };

    struct FScenario
    {
        std::string Path;
        std::string System;
        unsigned int Ticks = 0;
        double Frame = 1.0 / 30.0;
        bool bSpStepping = false;
        bool bSpCollisions = false;
        // `comms off`: Content's comm system (GCS) refuses every comm, so it draws no rand()
        // (the reproducibility work, owner 2026-11-19).
        bool bCommsOff = false;
        // `dump system_loads`: after the server starts and the scenario's system loads, every
        // system is loaded and its solars written (system_loads.jsonl); no scenario runs (P3.3).
        bool bDumpSystemLoads = false;
        // `dump replication`: the packets of the replication comparison are written to
        // replication.jsonl from the players' join on (SimClientStub, P3.3).
        bool bDumpReplication = false;
        // `dump content_tables`: right after the server starts, Content.dll's static tables
        // (LoadContentGameTables, run by the GameSupport the start-up creates) are written as
        // content_<table>.txt through its OracleDumpContentTables; no system loads and no
        // scenario runs (P3.4b, decision record 0008 point 5).
        bool bDumpContentTables = false;
        // `dump content_systems`: with `dump system_loads`, after every system has loaded,
        // Content.dll's system records, gate locks and a synthetic [mPlayer] are written as
        // content_<table>.txt through its OracleDumpContentSystems (P3.4d, decision record
        // 0008 point 10).
        bool bDumpContentSystems = false;
        // `dump mission_scripts`: right after the server starts, Content.dll makes each of the
        // fourteen story missions for a synthetic player and writes what its parser left as
        // mission_<table>.txt through its OracleDumpMissionScripts; no system loads and no
        // scenario runs (P3.5a, decision record 0008 point 10).
        bool bDumpMissionScripts = false;
        // `dump mission_saves`: right after the server starts, Content.dll writes the player-save
        // sections of the fourteen story missions in three states, and Story's, through its
        // OracleDumpMissionSaves; `dump mission_save_loads`: it loads the port's saves from the
        // out folder's `rust` folder and writes what it restored, through its
        // OracleLoadMissionSaves. No system loads and no scenario runs (P3.5b, decision record
        // 0008 point 10, Gate G3).
        bool bDumpMissionSaves = false;
        bool bDumpMissionSaveLoads = false;
        // `dump random_missions`: right after the server starts, Content.dll writes its
        // random-mission tables and vignette tree and the offers its generator builds for a fixed
        // set of bases, factions, difficulties and seeds as rm_<table>.txt through its
        // OracleDumpRandomMissions; no system loads and no scenario runs (P3.7a, decision record
        // 0008 point 8, Gate G3).
        bool bDumpRandomMissions = false;
        // `dump random_mission_saves`: right after the server starts, Content.dll makes random
        // missions through a fixed set of bases' vendor and bar offers, writes their
        // [RandomMission] save sections and loads each again, through its
        // OracleDumpRandomMissionSaves; `dump random_mission_save_loads`: it loads the port's
        // saves from the out folder's `rust` folder and writes the rebuilt offers, through its
        // OracleLoadRandomMissionSaves. No system loads and no scenario runs (P3.7c, decision
        // record 0008 points 8 and 10, Gate G3).
        bool bDumpRandomMissionSaves = false;
        bool bDumpRandomMissionSaveLoads = false;
        // `dump mission_trace`: after the server starts and the scenario's system loads,
        // Content.dll runs Mission_01a's opening for a synthetic player and writes what its trigger
        // engine did as mission.jsonl through its OracleDumpMissionTrace; no scenario runs (P3.5c1,
        // decision record 0008 question 9).
        bool bDumpMissionTrace = false;
        unsigned int Seed = 0x4f524143u;
        double ClockStart = 1000.0;
        std::vector<FObjectDesc> Objects;
        std::vector<FCommand> Commands;
        std::vector<FPlayerDesc> Players;
        // P3.6d: `population <ship>`, the ship becomes a Content player's (player 1) after the
        // objects are spawned, and SpacePop is created and hears its launch
        // (Content.dll's OracleSimPopulate); the server's frames then run SpacePop's heartbeat.
        std::string PopulationShip;
        int PopulationLine = 0;
    };

    std::string Lower(std::string Text)
    {
        for (char& Ch : Text)
        {
            if (Ch >= 'A' && Ch <= 'Z')
                Ch = (char)(Ch - 'A' + 'a');
        }
        return Text;
    }

    std::vector<std::string> Tokens(std::string const& Line)
    {
        std::vector<std::string> Out;
        std::string Current;
        for (char Ch : Line)
        {
            if (Ch == '#')
                break;
            if (Ch == ' ' || Ch == '\t' || Ch == '\r' || Ch == '\n' || Ch == ',')
            {
                if (!Current.empty())
                    Out.push_back(Current);
                Current.clear();
            }
            else
            {
                Current += Ch;
            }
        }
        if (!Current.empty())
            Out.push_back(Current);
        return Out;
    }

    bool ParseDouble(std::string const& Text, double& Out)
    {
        if (Text.empty())
            return false;
        char* End = nullptr;
        Out = strtod(Text.c_str(), &End);
        return End != nullptr && *End == '\0';
    }

    bool ParseUnsigned(std::string const& Text, unsigned long& Out)
    {
        if (Text.empty() || Text[0] == '-')
            return false;
        char* End = nullptr;
        Out = strtoul(Text.c_str(), &End, 0);
        return End != nullptr && *End == '\0';
    }

    // "<first>", "<first>..<last>" or "<first>..<last>/<step>".
    bool ParseTicks(std::string const& Text, unsigned long& First, unsigned long& Last, unsigned long& Step)
    {
        std::string Range = Text;
        Step = 1;
        size_t const Slash = Range.find('/');
        if (Slash != std::string::npos)
        {
            if (!ParseUnsigned(Range.substr(Slash + 1), Step) || Step == 0)
                return false;
            Range.resize(Slash);
        }
        size_t const Dots = Range.find("..");
        if (Dots == std::string::npos)
        {
            if (!ParseUnsigned(Range, First))
                return false;
            Last = First;
            return true;
        }
        return ParseUnsigned(Range.substr(0, Dots), First) && ParseUnsigned(Range.substr(Dots + 2), Last) &&
               First <= Last;
    }

    bool ParseVector(std::vector<std::string> const& T, size_t At, double* Out)
    {
        if (At + 3 > T.size())
            return false;
        for (size_t Index = 0; Index < 3; ++Index)
        {
            if (!ParseDouble(T[At + Index], Out[Index]))
                return false;
        }
        return true;
    }

    // Rotation about +Y by Degrees, evaluated in double: 0 faces -Z, 90 faces -X.
    void HeadingMatrix(double Degrees, double* Out)
    {
        double const Radians = Degrees * 3.14159265358979323846 / 180.0;
        double const C = cos(Radians);
        double const S = sin(Radians);
        double const M[9] = {C, 0.0, S, 0.0, 1.0, 0.0, -S, 0.0, C};
        memcpy(Out, M, sizeof(M));
    }

    // The object options after `ship ... x y z` or `solar <name> <nickname>`.
    bool ParseObjectOptions(std::vector<std::string> const& T, size_t At, FObjectDesc& Object, std::string& Error)
    {
        while (At < T.size())
        {
            std::string const Key = Lower(T[At]);
            if (Key == "psi")
            {
                Object.bPsi = true;
                ++At;
            }
            else if (Key == "heading" && At + 1 < T.size())
            {
                double Degrees = 0.0;
                if (!ParseDouble(T[At + 1], Degrees))
                    return Error = "bad heading", false;
                HeadingMatrix(Degrees, Object.Orient);
                At += 2;
            }
            else if (Key == "orient" && At + 9 < T.size())
            {
                for (size_t Index = 0; Index < 9; ++Index)
                {
                    if (!ParseDouble(T[At + 1 + Index], Object.Orient[Index]))
                        return Error = "bad orient", false;
                }
                At += 10;
            }
            else if (Key == "vel" && ParseVector(T, At + 1, Object.Vel))
            {
                At += 4;
            }
            else if (Key == "angvel" && ParseVector(T, At + 1, Object.AngVel))
            {
                At += 4;
            }
            else if (Key == "ai" && At + 1 < T.size())
            {
                Object.StateGraph = T[At + 1];
                At += 2;
            }
            else if (Key == "escort")
            {
                // P2.5 escort: a wingman's graph, as Content's ResetFormationState gives one
                // (pub::StateGraph::get_state_graph(graph, ESCORT)).
                Object.bEscort = true;
                ++At;
            }
            else if (Key == "pilot" && At + 1 < T.size())
            {
                Object.Pilot = T[At + 1];
                At += 2;
            }
            else if (Key == "faction" && At + 1 < T.size())
            {
                Object.Faction = T[At + 1];
                At += 2;
            }
            else if (Key == "hp" && At + 1 < T.size())
            {
                double Value = 0.0;
                if (!ParseDouble(T[At + 1], Value))
                    return Error = "bad hp", false;
                Object.HitPts = (int)Value;
                At += 2;
            }
            else if (Key == "content")
            {
                Object.bContent = true;
                ++At;
            }
            else if (Key == "flee" && At + 4 < T.size() && ParseVector(T, At + 1, Object.ContentFlee))
            {
                if (!ParseDouble(T[At + 4], Object.ContentFleeDistance))
                    return Error = "bad flee distance", false;
                Object.bContentFlee = true;
                At += 5;
            }
            else
            {
                Error = "unknown or incomplete option '" + T[At] + "'";
                return false;
            }
        }
        if (Object.bContent && (Object.StateGraph.empty() || Object.Faction.empty() || Object.bEscort))
        {
            // FContentNpcShip reads its faction's properties without a null test.
            Error = "content needs ai and faction, and takes no escort";
            return false;
        }
        if (Object.bContentFlee && !Object.bContent)
        {
            Error = "flee needs content";
            return false;
        }
        if (!Object.bShip &&
            (!Object.StateGraph.empty() || !Object.Pilot.empty() || !Object.Faction.empty() || Object.HitPts != -1 ||
             Object.bEscort))
        {
            Error = "a solar takes only psi";
            return false;
        }
        if (!Object.Pilot.empty() && Object.StateGraph.empty())
        {
            Error = "pilot needs ai";
            return false;
        }
        if (Object.bEscort && Object.StateGraph.empty())
        {
            Error = "escort needs ai";
            return false;
        }
        return true;
    }

    FObjectDesc const* FindDesc(FScenario const& Scenario, std::string const& Name)
    {
        for (FObjectDesc const& Object : Scenario.Objects)
        {
            if (Object.Name == Name)
                return &Object;
        }
        return nullptr;
    }

    bool LoadScenario(std::string const& Path, FScenario& Out, std::string& Error)
    {
        oracle::Bytes Data;
        if (!oracle::ReadWholeFile(Path, Data))
        {
            Error = "cannot read " + Path;
            return false;
        }
        Out.Path = Path;
        std::string Text(Data.begin(), Data.end());
        size_t Start = 0;
        int LineNumber = 0;
        bool bTicks = false;
        while (Start <= Text.size())
        {
            size_t End = Text.find('\n', Start);
            if (End == std::string::npos)
                End = Text.size();
            std::string const Line = Text.substr(Start, End - Start);
            Start = End + 1;
            ++LineNumber;
            std::vector<std::string> const T = Tokens(Line);
            if (T.empty())
                continue;
            std::string const Key = Lower(T[0]);
            std::string const Where = Format("line %d: ", LineNumber);
            unsigned long Value = 0;
            double Number = 0.0;
            if (Key == "system" && T.size() == 2)
            {
                Out.System = T[1];
            }
            else if (Key == "ticks" && T.size() == 2 && ParseUnsigned(T[1], Value) && Value > 0)
            {
                Out.Ticks = (unsigned int)Value;
                bTicks = true;
            }
            else if (Key == "frame" && T.size() == 2 && ParseDouble(T[1], Number) && Number > 0.0)
            {
                Out.Frame = Number;
            }
            else if (Key == "stepping" && T.size() == 2 && (Lower(T[1]) == "sp" || Lower(T[1]) == "mp"))
            {
                Out.bSpStepping = Lower(T[1]) == "sp";
            }
            else if (Key == "collisions" && T.size() == 2 && (Lower(T[1]) == "sp" || Lower(T[1]) == "mp"))
            {
                Out.bSpCollisions = Lower(T[1]) == "sp";
            }
            else if (Key == "comms" && T.size() == 2 && (Lower(T[1]) == "on" || Lower(T[1]) == "off"))
            {
                Out.bCommsOff = Lower(T[1]) == "off";
            }
            else if (Key == "dump" && T.size() == 2 && Lower(T[1]) == "system_loads")
            {
                Out.bDumpSystemLoads = true;
            }
            else if (Key == "dump" && T.size() == 2 && Lower(T[1]) == "replication")
            {
                Out.bDumpReplication = true;
            }
            else if (Key == "player" && T.size() == 3)
            {
                // The session (1 to the server's 16 slots) and a ship named before the line.
                FPlayerDesc Player;
                Player.Line = LineNumber;
                Player.Object = T[2];
                if (!ParseUnsigned(T[1], Value) || Value == 0 || Value > 16)
                    return Error = Where + "bad session '" + T[1] + "' (1 to 16)", false;
                Player.Session = (unsigned int)Value;
                FObjectDesc const* const Ship = FindDesc(Out, Player.Object);
                if (Ship == nullptr || !Ship->bShip)
                    return Error = Where + "a player needs a ship named before it: " + Player.Object, false;
                for (FPlayerDesc const& Other : Out.Players)
                {
                    if (Other.Session == Player.Session || Other.Object == Player.Object)
                        return Error = Where + "duplicate player", false;
                }
                Out.Players.push_back(Player);
            }
            else if (Key == "population" && T.size() == 2)
            {
                FObjectDesc const* const Ship = FindDesc(Out, T[1]);
                if (Ship == nullptr || !Ship->bShip)
                    return Error = Where + "population needs a ship named before it: " + T[1], false;
                Out.PopulationShip = T[1];
                Out.PopulationLine = LineNumber;
            }
            else if (Key == "dump" && T.size() == 2 && Lower(T[1]) == "content_tables")
            {
                Out.bDumpContentTables = true;
            }
            else if (Key == "dump" && T.size() == 2 && Lower(T[1]) == "content_systems")
            {
                Out.bDumpContentSystems = true;
            }
            else if (Key == "dump" && T.size() == 2 && Lower(T[1]) == "mission_scripts")
            {
                Out.bDumpMissionScripts = true;
            }
            else if (Key == "dump" && T.size() == 2 && Lower(T[1]) == "mission_saves")
            {
                Out.bDumpMissionSaves = true;
            }
            else if (Key == "dump" && T.size() == 2 && Lower(T[1]) == "mission_save_loads")
            {
                Out.bDumpMissionSaveLoads = true;
            }
            else if (Key == "dump" && T.size() == 2 && Lower(T[1]) == "random_missions")
            {
                Out.bDumpRandomMissions = true;
            }
            else if (Key == "dump" && T.size() == 2 && Lower(T[1]) == "random_mission_saves")
            {
                Out.bDumpRandomMissionSaves = true;
            }
            else if (Key == "dump" && T.size() == 2 && Lower(T[1]) == "random_mission_save_loads")
            {
                Out.bDumpRandomMissionSaveLoads = true;
            }
            else if (Key == "dump" && T.size() == 2 && Lower(T[1]) == "mission_trace")
            {
                Out.bDumpMissionTrace = true;
            }
            else if (Key == "seed" && T.size() == 2 && ParseUnsigned(T[1], Value))
            {
                Out.Seed = (unsigned int)Value;
            }
            else if (Key == "clock" && T.size() == 2 && ParseDouble(T[1], Number))
            {
                Out.ClockStart = Number;
            }
            else if (Key == "ship" && T.size() >= 7)
            {
                FObjectDesc Object;
                Object.Line = LineNumber;
                Object.Name = T[1];
                Object.Archetype = T[2];
                Object.Loadout = T[3] == "-" ? std::string() : T[3];
                if (!ParseVector(T, 4, Object.Pos))
                    return Error = Where + "bad position", false;
                if (!ParseObjectOptions(T, 7, Object, Error))
                    return Error = Where + Error, false;
                if (FindDesc(Out, Object.Name) != nullptr)
                    return Error = Where + "duplicate object " + Object.Name, false;
                Out.Objects.push_back(Object);
            }
            else if (Key == "solar" && T.size() >= 3)
            {
                FObjectDesc Object;
                Object.Line = LineNumber;
                Object.bShip = false;
                Object.Name = T[1];
                Object.Nickname = T[2];
                if (!ParseObjectOptions(T, 3, Object, Error))
                    return Error = Where + Error, false;
                if (FindDesc(Out, Object.Name) != nullptr)
                    return Error = Where + "duplicate object " + Object.Name, false;
                Out.Objects.push_back(Object);
            }
            else if (Key == "at" && T.size() >= 4)
            {
                unsigned long First = 0;
                unsigned long Last = 0;
                unsigned long Step = 1;
                if (!ParseTicks(T[1], First, Last, Step) || First == 0)
                    return Error = Where + "bad tick range '" + T[1] + "' (ticks count from 1)", false;
                if (FindDesc(Out, T[2]) == nullptr)
                    return Error = Where + "unknown object " + T[2], false;
                std::string Joined;
                for (size_t Index = 3; Index < T.size(); ++Index)
                    Joined += (Index > 3 ? " " : "") + T[Index];
                for (unsigned long Tick = First; Tick <= Last; Tick += Step)
                {
                    FCommand Command;
                    Command.Tick = (unsigned int)Tick;
                    Command.Object = T[2];
                    Command.Args.assign(T.begin() + 3, T.end());
                    Command.Args[0] = Lower(Command.Args[0]);
                    Command.Text = Joined;
                    Command.Line = LineNumber;
                    Command.Order = Out.Commands.size();
                    Out.Commands.push_back(Command);
                }
            }
            else
            {
                return Error = Where + "cannot read '" + Line + "'", false;
            }
        }
        if (Out.System.empty() || !bTicks)
            return Error = "a scenario needs `system` and `ticks`", false;
        // A dump session needs no object: every `dump` line but `replication`, which records
        // the objects the session spawns (P3.6a: `content_systems` joins the list; P3.7a:
        // `random_missions`; P3.7c: `random_mission_saves` and `random_mission_save_loads`; P3.5c1:
        // `mission_trace`).
        bool const bObjectlessDump = Out.bDumpSystemLoads || Out.bDumpContentTables ||
                                     Out.bDumpContentSystems || Out.bDumpMissionScripts ||
                                     Out.bDumpMissionSaves || Out.bDumpMissionSaveLoads ||
                                     Out.bDumpRandomMissions || Out.bDumpRandomMissionSaves ||
                                     Out.bDumpRandomMissionSaveLoads || Out.bDumpMissionTrace;
        if (Out.Objects.empty() && !bObjectlessDump)
            return Error = "a scenario needs at least one object", false;
        std::stable_sort(Out.Commands.begin(), Out.Commands.end(),
                         [](FCommand const& A, FCommand const& B) { return A.Tick < B.Tick; });
        return true;
    }

    // ------------------------------------------------------------------------------------
    // Run state and writers
    // ------------------------------------------------------------------------------------

    enum EFile
    {
        FileSession,
        FileTick,
        FileEvents,
        FilePsi,
        FileCalls,
        FileMetrics,
        FileDump,
        FileDispatch,
        kFileCount
    };

    char const* const kFileNames[kFileCount] = {"session.jsonl", "tick.jsonl", "events.jsonl", "psi.jsonl",
                                                "calls.jsonl",   "metrics.txt", "fdump.txt", "dispatch.jsonl"};

    struct FSimObject
    {
        FObjectDesc const* Desc = nullptr;
        unsigned int Id = 0;
        CObject::Class Class = CObject::ClassShip;
    };

    FILE* GFiles[kFileCount] = {};
    FScenario GScenario;
    std::vector<FSimObject> GObjects;
    unsigned int GTick = 0;
    unsigned int GPsiSeq = 0;
    std::vector<float> GPsiDt;      // this tick's PSI lengths
    std::vector<int> GPsiCheck;     // the movement-check countdown at the start of each
    std::vector<unsigned> GPsiSeed; // and IVP_FastRandomFloat's seed
    unsigned int GBehaviorSeq = 0;
    unsigned int GOpenBehavior = 0;  // the behaviour tick in progress, 0 outside one
    bool GbScenarioCommand = false;  // the harness itself is issuing a command

    // One psi.jsonl record being built (design/sim-records.md), one per GObjects entry: the
    // core is resolved before the PSI's simulation units, the record opened at its unit's
    // Process_One_Psi_Step entry, the pushes appended while that unit runs, and the record
    // written after the next-PSI matrices.
    enum EPsiState
    {
        PsiIdle,      // not recorded in this PSI (yet)
        PsiInUnit,    // its unit is running: pushes are recorded
        PsiUnitDone   // its unit has run: waiting for the state after calc_next_PSI_matrix
    };

    struct FPsiRecord
    {
        CObject* Object = nullptr;   // a reference held from OnPsi phase 0 to phase 1
        IVP_Core* Core = nullptr;
        EPsiState State = PsiIdle;
        FOracleJson J;
    };

    std::vector<FPsiRecord> GPsiRecords;            // parallel to GObjects
    // P2.5 (the missile): the psi records of the guided munitions whose units run in this
    // PSI (obj "guided", no ctl), and every guided munition seen in a unit, for the
    // per-tick `guided` records of tick.jsonl until it is gone.
    std::vector<FPsiRecord> GGuidedPsi;
    std::set<unsigned int> GGuidedIds;
    IVP_Simulation_Unit* GPsiUnit = nullptr;        // the unit running with a record open
    std::vector<std::string> GPsiNoted;             // objects already noted as not recordable
    std::map<unsigned int, unsigned int> GLastTarget;  // P2.5: ship id -> its target id at the last tick
    void(__cdecl* GStubSetTick)(unsigned int) = nullptr;

    void Write(EFile File, std::string const& Line)
    {
        if (GFiles[File] == nullptr)
            return;
        fputs(Line.c_str(), GFiles[File]);
        fputc('\n', GFiles[File]);
    }

    void Write(EFile File, FOracleJson& J)
    {
        Write(File, J.Finish());
    }

    void FlushAll()
    {
        for (FILE* File : GFiles)
        {
            if (File != nullptr)
                fflush(File);
        }
    }

    void CloseAll()
    {
        for (FILE*& File : GFiles)
        {
            if (File != nullptr)
                fclose(File);
            File = nullptr;
        }
    }

    // A session record with "rec" and a message; also on stderr.
    void SessionNote(char const* Rec, std::string const& Text)
    {
        FOracleJson J;
        J.Str("rec", Rec);
        J.Uint("tick", GTick);
        J.Str("text", Text.c_str());
        Write(FileSession, J);
        fflush(GFiles[FileSession]);
        fprintf(stderr, "sim %s: %s\n", Rec, Text.c_str());
    }

    void __cdecl SimDump(int Flags, char const* Pattern, ...)
    {
        if (GFiles[FileDump] == nullptr || Pattern == nullptr)
            return;
        char Buffer[0x1000];
        va_list Args;
        va_start(Args, Pattern);
        _vsnprintf_s(Buffer, sizeof(Buffer), _TRUNCATE, Pattern, Args);
        va_end(Args);
        size_t Length = strlen(Buffer);
        while (Length > 0 && (Buffer[Length - 1] == '\n' || Buffer[Length - 1] == '\r'))
            Buffer[--Length] = '\0';
        fprintf(GFiles[FileDump], "%u %08x %s\n", GTick, (unsigned int)Flags, Buffer);
    }

    bool __cdecl SimCriticalWarning(unsigned int Id, char const* Text)
    {
        SessionNote("critical_warning", Format("%u %s", Id, Text != nullptr ? Text : ""));
        return true;
    }

    FSimObject const* FindObject(unsigned int Id)
    {
        for (FSimObject const& Object : GObjects)
        {
            if (Object.Id == Id && Id != 0)
                return &Object;
        }
        return nullptr;
    }

    FSimObject const* FindObject(std::string const& Name)
    {
        for (FSimObject const& Object : GObjects)
        {
            if (Object.Desc->Name == Name)
                return &Object;
        }
        return nullptr;
    }

    // P2.4 (asteroids.jsonl `probe`): the positions of the scenario's live ships.
    std::vector<Vector> ShipPositions();

    // The scenario name of an id, or "#<id>" for any other object.
    std::string NameOf(unsigned int Id)
    {
        FSimObject const* const Object = FindObject(Id);
        return Object != nullptr ? Object->Desc->Name : Format("#%u", Id);
    }

    void IdAndName(FOracleJson& J, char const* IdKey, char const* NameKey, unsigned int Id)
    {
        J.Uint(IdKey, Id);
        FSimObject const* const Object = FindObject(Id);
        if (Object != nullptr)
            J.Str(NameKey, Object->Desc->Name.c_str());
        else
            J.Null(NameKey);
    }

    void VectorJ(FOracleJson& J, char const* Name, Vector const& Value)
    {
        float const Values[3] = {Value.x, Value.y, Value.z};
        J.FloatArray(Name, Values, 3);
    }

    void MatrixJ(FOracleJson& J, char const* Name, Matrix const& Value)
    {
        J.FloatArray(Name, &Value.E[0][0], 9);
    }

    void Metric(char const* Kind, std::string const& Object)
    {
        Write(FileMetrics, Format("E %s %s %u", Kind, Object.c_str(), GTick));
    }

    // The server object behind a scenario object: the system's IObjRW, null once it is gone.
    IObjRW* FindRw(FSimObject const& Object)
    {
        unsigned int Id = Object.Id;
        CObject* const Found = CObject::Find(Id, Object.Class);
        if (Found == nullptr)
            return nullptr;
        IObjDB* const Db = static_cast<CSimple*>(Found)->ObjectDb;
        Found->Release();
        return Db != nullptr ? Db->FindById(&Id) : nullptr;
    }

    std::vector<Vector> ShipPositions()
    {
        std::vector<Vector> Out;
        for (FSimObject const& Object : GObjects)
        {
            if (!Object.Desc->bShip)
                continue;
            IObjRW* const Rw = FindRw(Object);
            if (Rw != nullptr)
                Out.push_back(Rw->get_position());
        }
        return Out;
    }

    // ------------------------------------------------------------------------------------
    // Hook callbacks
    // ------------------------------------------------------------------------------------

    // The name of one controller of a ship's simulation unit (psi.jsonl "ctl.order"): the
    // ship's own, its steering, nudge and strafe engines, its engine and its thrusters by id;
    // "other" for anything else in the unit (IVP's standard controller included).
    std::string ControllerName(CShip* Ship, void const* Controller)
    {
        auto Is = [Controller](PhySys::Controller const* C) {
            return static_cast<void const*>(static_cast<IVP_Controller const*>(C)) == Controller;
        };
        if (Is(Ship))
            return "ship";
        if (Is(&Ship->Steering))
            return "steering";
        if (Is(&Ship->NudgeEngine))
            return "nudge";
        if (Is(&Ship->StrafeEngine))
            return "strafe";
        CEquipTraverser Walk(static_cast<int>(0x20000 | EquipTypeThruster), true);
        for (CEquip* Equip = Ship->EquipManager.Traverse(Walk); Equip != nullptr;
             Equip = Ship->EquipManager.Traverse(Walk))
        {
            if (CEEngine* const Engine = CEEngine::cast(Equip))
            {
                if (Is(static_cast<CPhysControllerEquip const*>(Engine)))
                    return "engine";
            }
            else if (CEThruster* const Thruster = CEThruster::cast(Equip))
            {
                if (Is(static_cast<CPhysControllerEquip const*>(Thruster)))
                    return Format("thruster %u", (unsigned)Equip->GetID());
            }
        }
        return "other";
    }

    // The flight controllers' inputs of a ship (psi.jsonl "ctl").
    void ControllerInputs(FOracleJson& J, CShip* Ship, IVP_Simulation_Unit* Unit)
    {
        J.BeginObject("ctl");
        J.Float("throttle", Ship->Throttle);
        J.FloatArray("axis", &Ship->Steering.AxisThrottle.x, 3);
        J.Bool("steering_active", Ship->Steering.bActive);
        J.FloatArray("nudge", &Ship->NudgeEngine.CurrentNudge.x, 3);
        J.Bool("nudge_active", Ship->NudgeEngine.bActive);
        J.Int("strafe", (int)Ship->StrafeEngine.CurrentStrafe);
        J.Float("thrust_power", Ship->ThrustPower);
        J.Float("max_thrust_power", Ship->MaxThrustPower);
        J.Float("angular_drag_factor", Ship->AngularDragFactor);
        CEquip* const EngineEquip = Ship->EquipManager.FindFirst(0x20000u);
        CEEngine* const Engine = EngineEquip != nullptr ? CEEngine::cast(EngineEquip) : nullptr;
        if (Engine != nullptr)
        {
            J.BeginObject("engine");
            J.Bool("cruise", Engine->bCruiseActive != 0);
            J.Float("cruise_charge_elapsed", Engine->CruiseChargeTimeElapsed);
            J.Float("cruise_charge_rate", Engine->CruiseChargeRate);
            J.Float("last_wake_throttle", Engine->LastWakeThrottle);
            Archetype::Engine const* const Arch = Engine->EngineArch();
            if (Arch != nullptr)
            {
                J.Float("max_force", Arch->ThrustMultiplier);
                J.Float("linear_drag", Arch->LinearDrag);
                J.Float("power_usage", Arch->PowerDrawFactor);
                J.Float("cruise_power_usage", Arch->CruisePowerUsage);
                J.Float("cruise_charge_time", Arch->CruiseChargeTime);
                J.Float("reverse_fraction", Arch->ReverseFraction);
            }
            // P2.4: the engine controller's gates (CEEngine::do_simulation_controller).
            J.Bool("active", Engine->IsActive());
            J.Bool("functioning", Engine->IsFunctioning());
            J.Bool("controller", Engine->IsControllerEnabled());
            J.EndObject();
        }
        else
        {
            J.Null("engine");
        }
        Archetype::Root* const Root = Ship->get_archetype();
        if (Root != nullptr)
        {
            Archetype::Ship const* const Arch = static_cast<Archetype::Ship const*>(Root);
            J.BeginObject("arch");
            J.Float("linear_drag", Arch->LinearDrag);
            float const AngularDrag[3] = {Arch->AngularDragX, Arch->AngularDragY, Arch->AngularDragZ};
            J.FloatArray("angular_drag", AngularDrag, 3);
            float const Torque[3] = {Arch->SteeringTorqueX, Arch->SteeringTorqueY, Arch->SteeringTorqueZ};
            J.FloatArray("steering_torque", Torque, 3);
            J.Float("nudge_force", Arch->NudgeForce);
            J.Float("strafe_force", Arch->StrafeForce);
            J.Float("strafe_power_usage", Arch->StrafePowerUsage);
            J.EndObject();
        }
        else
        {
            J.Null("arch");
        }
        // P2.4 (game-sim's controller replay): what else the controllers read -- the owner's
        // power (CEEngine), its orientation (every controller's frame), the zone drag of
        // get_linear_drag (cruise), whether steering, nudge and strafe are registered, and the
        // thrusters (CEThruster) with their gates.
        J.Float("power", Ship->Power);
        J.Float("max_power", Ship->MaxPower);
        MatrixJ(J, "orient", Ship->Orientation);
        if (Ship->ZoneAtA4 != nullptr)
            J.Float("zone_drag", Ship->ZoneAtA4->GetDragModifier());
        else
            J.Null("zone_drag");
        J.Bool("steering_enabled", Ship->Steering.IsControllerEnabled());
        J.Bool("nudge_enabled", Ship->NudgeEngine.IsControllerEnabled());
        J.Bool("strafe_enabled", Ship->StrafeEngine.IsControllerEnabled());
        J.BeginArray("thrusters");
        {
            CEquipTraverser Walk(EquipTypeThruster, true);
            for (CEquip* Equip = Ship->EquipManager.Traverse(Walk); Equip != nullptr;
                 Equip = Ship->EquipManager.Traverse(Walk))
            {
                CEThruster* const Thruster = CEThruster::cast(Equip);
                if (Thruster == nullptr)
                    continue;
                Archetype::Thruster const* const ThrusterArch = Thruster->ThrusterArch();
                J.BeginObject(nullptr);
                J.Uint("id", Equip->GetID());
                J.Bool("functioning", Thruster->IsFunctioning());
                J.Bool("controller", Thruster->IsControllerEnabled());
                J.Float("power_usage", ThrusterArch->PowerUsage);
                J.Float("max_force", ThrusterArch->MaxThrust);
                J.EndObject();
            }
        }
        J.EndArray();
        // P2.5: the unit's controllers in the order its PSI runs them (the bucket order, which a
        // merge or split of the unit rebuilds).
        J.BeginArray("order");
        {
            void const* Controllers[32];
            const int Count = OracleSimUnitControllers(Unit, Controllers, 32);
            for (int Index = 0; Index < Count; ++Index)
                J.Str(nullptr, ControllerName(Ship, Controllers[Index]).c_str());
        }
        J.EndArray();
        J.EndObject();
    }

    // A session note, once per object, for a psi object whose PSIs cannot be replayed.
    void PsiNote(FSimObject const& Object, char const* Why)
    {
        if (std::find(GPsiNoted.begin(), GPsiNoted.end(), Object.Desc->Name) != GPsiNoted.end())
            return;
        GPsiNoted.push_back(Object.Desc->Name);
        SessionNote("note", Format("psi %s: %s; its PSIs are not recorded", Object.Desc->Name.c_str(), Why));
    }

    // A core state of a psi record ("in" or "out").
    void CoreJ(FOracleJson& J, char const* Name, FOracleSimCore const& C)
    {
        J.BeginObject(Name);
        J.Uint("flags", C.Flags);
        J.Uint("movement_state", C.MovementState);
        J.Uint("unit_state", C.UnitState);
        J.Float("upper_limit_radius", C.UpperLimitRadius);
        J.Float("max_surface_deviation", C.SpeedLimit);
        J.Float("mass", C.Mass);
        J.Float("inv_mass", C.InvMass);
        J.FloatArray("inertia", C.RotInertia, 3);
        J.FloatArray("inv_inertia", C.InvRotInertia, 3);
        J.FloatArray("rot_speed_damp", C.RotSpeedDamp, 3);
        J.Float("speed_damp", C.SpeedDamp);
        J.Float("angular_limit", C.DAngularLimitFactor);
        if (C.bSpinClipping != 0)
            J.FloatArray("spin_clip", C.SpinClipping, 3);
        else
            J.Null("spin_clip");
        J.FloatArray("time_of_last_psi", C.TimeOfLastPsi, 2);
        J.Float("i_delta_time", C.IDeltaTime);
        J.FloatArray("rot_speed_change", C.RotSpeedChange, 3);
        J.FloatArray("speed_change", C.SpeedChange, 3);
        J.FloatArray("rot_speed", C.RotSpeed, 3);
        J.FloatArray("speed", C.Speed, 3);
        J.FloatArray("pos", C.Pos, 3);
        J.FloatArray("delta_world", C.DeltaWorld, 3);
        J.FloatArray("q", C.Quat, 4);
        J.FloatArray("q_next", C.QuatNext, 4);
        J.FloatArray("m", C.Matrix, 12);
        J.FloatArray("rot_axis", C.RotAxis, 3);
        J.Float("current_speed", C.Margin);
        J.Float("abs_omega", C.MaxSurfaceDeviation);
        J.Float("surface_rot_speed", C.BoundingRadius);
        J.FloatArray("time_of_last_motion", C.TimeOfLastMotion, 2);
        J.FloatArray("time_of_last_long_motion", C.TimeOfLastLongMotion, 2);
        J.FloatArray("q_last_motion", C.QLastMotion, 4);
        J.FloatArray("q_last_long_motion", C.QLastLongMotion, 4);
        J.FloatArray("pos_last_motion", C.PosLastMotion, 3);
        J.FloatArray("pos_last_long_motion", C.PosLastLongMotion, 3);
        J.EndObject();
    }

    void ResetPsiRecords()
    {
        for (FPsiRecord& Record : GPsiRecords)
        {
            if (Record.Object != nullptr)
                Record.Object->Release();
            Record.Object = nullptr;
            Record.Core = nullptr;
            Record.State = PsiIdle;
            Record.J = FOracleJson();
        }
        GGuidedPsi.clear();
        GPsiUnit = nullptr;
    }

    // Phase 0: count the PSI and resolve the cores of the psi objects. Phase 1 (after
    // commit_all_calc_next_PSI_matrix): the state after calc_next_PSI_matrix closes each
    // record whose unit ran in this PSI.
    void __cdecl OnPsi(IVP_Environment* Environment, int Phase)
    {
        if (Phase == 0)
        {
            ++GPsiSeq;
            GPsiDt.push_back(OracleSimEnvDeltaTime(Environment));
            {
                FOracleSimEnv Env;
                OracleSimReadEnv(Environment, Env);
                GPsiCheck.push_back(Env.MovementCheck);
                GPsiSeed.push_back(Env.IvpSeed);
            }
            ResetPsiRecords();
            GPsiRecords.resize(GObjects.size());
            for (size_t Index = 0; Index < GObjects.size(); ++Index)
            {
                FSimObject const& Object = GObjects[Index];
                if (!Object.Desc->bPsi)
                    continue;
                unsigned int Id = Object.Id;
                CObject* const Found = CObject::Find(Id, Object.Class);
                if (Found == nullptr)
                    continue;
                IVP_Core* const Core = OracleSimCoreOf(Found);
                if (Core == nullptr || Found->GolemController != nullptr)
                {
                    // A golem or remote-correction controller beams its body; the scenario
                    // tests cover those.
                    if (Core != nullptr)
                        PsiNote(Object, "it has a golem or remote-correction controller");
                    Found->Release();
                    continue;
                }
                GPsiRecords[Index].Object = Found;
                GPsiRecords[Index].Core = Core;
            }
            return;
        }

        for (FPsiRecord& Record : GPsiRecords)
        {
            if (Record.State != PsiUnitDone)
                continue;
            FOracleSimCore Out;
            OracleSimReadCore(Record.Core, Out);
            CoreJ(Record.J, "out", Out);
            Write(FilePsi, Record.J);
        }
        for (FPsiRecord& Record : GGuidedPsi)
        {
            if (Record.State != PsiUnitDone)
                continue;
            FOracleSimCore Out;
            OracleSimReadCore(Record.Core, Out);
            CoreJ(Record.J, "out", Out);
            Write(FilePsi, Record.J);
        }
        ResetPsiRecords();
    }

    // Phase 0 opens the record of each psi core in the unit: the environment's values, the
    // controller inputs and the core state before the unit's PSI. Phase 1 closes its pushes
    // and adds the movement check.
    // units.jsonl: the simulation units of the first kUnitPsis PSIs in the order they ran,
    // each with its cores' objects and whether the environment's movement-check countdown
    // came to it (which unit classifies its motion depends on how many units share it).
    const unsigned int kUnitPsis = 120;
    FILE* GUnitFile = nullptr;
    unsigned int GUnitPsi = 0;
    FOracleJson* GUnitLine = nullptr;

    // Writes the PSI's line in progress; with bClose the file is closed (the run ends or
    // the last recorded PSI is over).
    void UnitFlush(bool bClose)
    {
        if (GUnitFile == nullptr)
            return;
        if (GUnitLine != nullptr)
        {
            GUnitLine->EndArray();
            fputs(GUnitLine->Finish().c_str(), GUnitFile);
            fputc('\n', GUnitFile);
            delete GUnitLine;
            GUnitLine = nullptr;
        }
        fflush(GUnitFile);
        if (bClose)
        {
            fclose(GUnitFile);
            GUnitFile = nullptr;
        }
    }

    // unit_cores.jsonl: the first core of every unit of the first kUnitCorePsis PSIs at the
    // end of the unit's PSI (before calc_next_PSI_matrix), in the order the units ran, and
    // in the first PSI also at its start: the state of the units no scenario object owns
    // (spinning solars, the free parts of stations), for scenario_replay's comparison.
    const unsigned int kUnitCorePsis = 12;
    FILE* GUnitCoreFile = nullptr;
    unsigned int GUnitCorePsi = 0;
    unsigned int GUnitCoreIndex = 0;
    FOracleSimCore GUnitCoreIn;

    void UnitCoreRecord(IVP_Simulation_Unit* Unit, int Phase, bool bMovementCheck)
    {
        if (GUnitCoreFile == nullptr)
            return;
        if (GPsiSeq > kUnitCorePsis)
        {
            fclose(GUnitCoreFile);
            GUnitCoreFile = nullptr;
            return;
        }
        if (GUnitCorePsi != GPsiSeq)
        {
            GUnitCorePsi = GPsiSeq;
            GUnitCoreIndex = 0;
        }
        CObject* const Object = OracleSimUnitCoreObject(Unit, 0);
        IVP_Core* const Core = Object != nullptr ? OracleSimCoreOf(Object) : nullptr;
        if (Phase == 0)
        {
            OracleSimReadCore(Core, GUnitCoreIn);
            return;
        }
        FOracleSimCore Out;
        OracleSimReadCore(Core, Out);
        bool const bSimple = Object != nullptr && ((unsigned)Object->ObjectClass & 3u) == 3u;
        FOracleJson J;
        J.Str("rec", "unit_core");
        J.Uint("tick", GTick);
        J.Uint("psi", GPsiSeq);
        J.Uint("i", GUnitCoreIndex);
        J.Uint("id", bSimple ? static_cast<CSimple*>(Object)->Id : 0);
        J.Uint("class", Object != nullptr ? (unsigned)Object->ObjectClass : 0);
        J.Bool("check", bMovementCheck);
        if (GPsiSeq == 1)
            CoreJ(J, "in", GUnitCoreIn);
        CoreJ(J, "out", Out);
        // A free part's object (class 1): its frame and its golem's targets.
        if (Object != nullptr && (unsigned)Object->ObjectClass == 1u)
        {
            J.FloatArray("orient", &Object->Orientation.E[0][0], 9);
            float const Pos[3] = {Object->WorldPosition.x, Object->WorldPosition.y, Object->WorldPosition.z};
            J.FloatArray("pos", Pos, 3);
            IVP_Controller_Golem const* const Golem =
                static_cast<IVP_Controller_Golem const*>(Object->GolemController);
            if (Golem != nullptr)
            {
                float const Target[4] = {Golem->TargetQuat.x, Golem->TargetQuat.y, Golem->TargetQuat.z,
                                         Golem->TargetQuat.w};
                J.FloatArray("golem_q", Target, 4);
                float const Current[4] = {Golem->CurrentTargetQuat.x, Golem->CurrentTargetQuat.y,
                                          Golem->CurrentTargetQuat.z, Golem->CurrentTargetQuat.w};
                J.FloatArray("golem_current_q", Current, 4);
            }
        }
        fputs(J.Finish().c_str(), GUnitCoreFile);
        fputc('\n', GUnitCoreFile);
        fflush(GUnitCoreFile);
        ++GUnitCoreIndex;
    }

    void UnitRecord(IVP_Simulation_Unit* Unit, int Phase, bool bMovementCheck)
    {
        UnitCoreRecord(Unit, Phase, bMovementCheck);
        if (GUnitFile == nullptr)
            return;
        if (GPsiSeq > kUnitPsis)
        {
            UnitFlush(true);
            return;
        }
        if (GUnitPsi != GPsiSeq)
        {
            UnitFlush(false);
            GUnitPsi = GPsiSeq;
            GUnitLine = new FOracleJson();
            GUnitLine->Str("rec", "units");
            GUnitLine->Uint("tick", GTick);
            GUnitLine->Uint("psi", GPsiSeq);
            GUnitLine->BeginArray("units");
        }
        if (Phase == 0)
        {
            GUnitLine->BeginArray(nullptr);
            int const Count = OracleSimUnitCoreCount(Unit);
            for (int Index = 0; Index < Count; ++Index)
            {
                CObject* const Object = OracleSimUnitCoreObject(Unit, Index);
                bool const bSimple =
                    Object != nullptr && ((unsigned)Object->ObjectClass & 3u) == 3u;
                GUnitLine->Uint(nullptr, bSimple ? static_cast<CSimple*>(Object)->Id : 0);
                GUnitLine->Uint(nullptr, Object != nullptr ? (unsigned)Object->ObjectClass : 0);
            }
        }
        else
        {
            GUnitLine->Bool(nullptr, bMovementCheck);
            GUnitLine->EndArray();
        }
    }

    void __cdecl OnSimUnitPsi(IVP_Environment* Environment, IVP_Simulation_Unit* Unit, int Phase,
                              bool bMovementCheck)
    {
        UnitRecord(Unit, Phase, bMovementCheck);
        if (Phase != 0)
        {
            for (FPsiRecord& Record : GPsiRecords)
            {
                if (Record.State != PsiInUnit)
                    continue;
                Record.J.EndArray();
                Record.J.Bool("check", bMovementCheck);
                Record.State = PsiUnitDone;
            }
            for (FPsiRecord& Record : GGuidedPsi)
            {
                if (Record.State != PsiInUnit)
                    continue;
                Record.J.EndArray();
                Record.J.Bool("check", bMovementCheck);
                Record.State = PsiUnitDone;
            }
            GPsiUnit = nullptr;
            return;
        }

        // P2.5: a guided munition's unit (one core, its first object of class 0xa7) opens a
        // record of its own, with no controller inputs.
        if (OracleSimUnitCoreCount(Unit) == 1)
        {
            CObject* const First = OracleSimUnitCoreObject(Unit, 0);
            IVP_Core* const Core = First != nullptr ? OracleSimCoreOf(First) : nullptr;
            if (Core != nullptr && (unsigned)First->ObjectClass == (unsigned)CObject::ClassGuided)
            {
                unsigned int const Id = static_cast<CSimple*>(First)->Id;
                GGuidedIds.insert(Id);
                FOracleSimEnv Env;
                OracleSimReadEnv(Environment, Env);
                FOracleSimCore In;
                OracleSimReadCore(Core, In);
                GGuidedPsi.emplace_back();
                FPsiRecord& Record = GGuidedPsi.back();
                Record.Core = Core;
                FOracleJson& J = Record.J;
                J.Str("rec", "psi");
                J.Uint("tick", GTick);
                J.Uint("psi", GPsiSeq);
                J.Str("obj", "guided");
                J.Uint("id", Id);
                J.Float("dt", Env.DeltaTime);
                J.Float("idt", Env.InvDeltaTime);
                J.FloatArray("time", Env.Time, 2);
                float const Limits[2] = {Env.MaxVelocity, Env.MaxAngularVelocityPerPsi};
                J.FloatArray("limits", Limits, 2);
                CoreJ(J, "in", In);
                J.BeginArray("pushes");
                Record.State = PsiInUnit;
                GPsiUnit = Unit;
            }
        }

        for (size_t Index = 0; Index < GPsiRecords.size(); ++Index)
        {
            FPsiRecord& Record = GPsiRecords[Index];
            if (Record.Core == nullptr || Record.State != PsiIdle || OracleSimUnitOf(Record.Core) != Unit)
                continue;
            FSimObject const& Object = GObjects[Index];
            if (OracleSimUnitCoreCount(Unit) != 1)
            {
                // The replay steps one core per unit.
                PsiNote(Object, "its simulation unit holds more than one core");
                continue;
            }
            FOracleSimEnv Env;
            OracleSimReadEnv(Environment, Env);
            FOracleSimCore In;
            OracleSimReadCore(Record.Core, In);
            FOracleJson& J = Record.J;
            J.Str("rec", "psi");
            J.Uint("tick", GTick);
            J.Uint("psi", GPsiSeq);
            J.Str("obj", Object.Desc->Name.c_str());
            J.Uint("id", Object.Id);
            J.Float("dt", Env.DeltaTime);
            J.Float("idt", Env.InvDeltaTime);
            J.FloatArray("time", Env.Time, 2);
            float const Limits[2] = {Env.MaxVelocity, Env.MaxAngularVelocityPerPsi};
            J.FloatArray("limits", Limits, 2);
            if (Object.Class == CObject::ClassShip)
                ControllerInputs(J, static_cast<CShip*>(Record.Object), Unit);
            CoreJ(J, "in", In);
            J.BeginArray("pushes");
            Record.State = PsiInUnit;
            GPsiUnit = Unit;
        }
    }

    char const* PushKindName(int Kind)
    {
        switch (Kind)
        {
        case OracleSimPushLinear:
            return "linear";
        case OracleSimPushAngularWs:
            return "angular_ws";
        case OracleSimPushAngularCs:
            return "angular_cs";
        default:
            return "unknown";
        }
    }

    // A push into a core whose unit is running with its record open; pushes at any other
    // time are not part of a record.
    void __cdecl OnCorePush(IVP_Core* Core, int Kind, Vector const* Direction, float Magnitude)
    {
        if (GPsiUnit == nullptr)
            return;
        for (std::vector<FPsiRecord>* List : {&GPsiRecords, &GGuidedPsi})
        {
            for (FPsiRecord& Record : *List)
            {
                if (Record.State != PsiInUnit || Record.Core != Core)
                    continue;
                FOracleJson& J = Record.J;
                J.BeginArray(nullptr);
                J.Str(nullptr, PushKindName(Kind));
                J.Float(nullptr, Direction->x);
                J.Float(nullptr, Direction->y);
                J.Float(nullptr, Direction->z);
                J.Float(nullptr, Magnitude);
                J.EndArray();
            }
        }
    }

    void __cdecl OnCollisionDamage(unsigned int SourceId, unsigned short SourceSubObj, unsigned int TargetId,
                                   unsigned short TargetSubObj, Vector const& OwnVelocity,
                                   Vector const& OtherVelocity, float Speed, float OtherMass, float OwnMass,
                                   float Damage)
    {
        FOracleJson J;
        J.Str("rec", "collision_damage");
        J.Uint("tick", GTick);
        IdAndName(J, "source", "source_obj", SourceId);
        J.Uint("source_sub", SourceSubObj);
        IdAndName(J, "target", "target_obj", TargetId);
        J.Uint("target_sub", TargetSubObj);
        VectorJ(J, "own_vel", OwnVelocity);
        VectorJ(J, "other_vel", OtherVelocity);
        J.Float("speed", Speed);
        J.Float("other_mass", OtherMass);
        J.Float("own_mass", OwnMass);
        J.Float("damage", Damage);
        Write(FileCalls, J);

        FOracleJson E;
        E.Str("rec", "collision");
        E.Uint("tick", GTick);
        IdAndName(E, "a", "a_obj", SourceId);
        IdAndName(E, "b", "b_obj", TargetId);
        E.Float("damage", Damage);
        Write(FileEvents, E);
        Metric("collision", NameOf(SourceId));
    }

    // P2.4 (game-sim's damage replay): what EqObj::munition_hit read of its target, before
    // the list is applied -- the hull, power, owner and armour, the sub-object that was hit,
    // and for a shield its threshold and the weapon modifier -- and the munition's damage.
    void MunitionHitState(FOracleJson& J, unsigned int TargetId, unsigned int MunitionArchId,
                          unsigned short SubObjId)
    {
        unsigned int Id = TargetId;
        CObject* Found = CObject::Find(Id, CObject::ClassShip);
        if (Found == nullptr)
            Found = CObject::Find(Id, CObject::ClassSolar);
        Archetype::Equipment const* const MunitionRoot = Archetype::GetEquipment(MunitionArchId);
        Archetype::Munition const* const Munition = static_cast<Archetype::Munition const*>(MunitionRoot);
        J.BeginObject("state");
        if (Munition != nullptr)
        {
            J.Float("hull_damage", Munition->HullDamage);
            J.Float("energy_damage", Munition->EnergyDamage);
            J.Uint("weapon_type", Munition->WeaponType.Id);
        }
        if (Found != nullptr)
        {
            CEqObj* const Eq = static_cast<CEqObj*>(Found);
            J.Float("hp", Eq->HitPts);
            J.Float("hp_max", Eq->Archetype->HitPts);
            J.Float("power", Eq->Power);
            J.Float("power_max", Eq->MaxPower);
            J.Uint("owner", Eq->OwnerPlayer);
            CEquip* const Armor = Eq->EquipManager.FindFirst(EquipTypeArmor);
            if (Armor != nullptr)
                J.Float("armor", static_cast<CEArmor*>(Armor)->ArmorArch()->HitPtsScale);
            else
                J.Null("armor");
            CArchGroup* const Group = Eq->ArchGroups.FindByID(SubObjId);
            CEquip* const Equip = Eq->EquipManager.FindByID(SubObjId);
            if (Group != nullptr)
            {
                J.Str("sub_kind", "group");
                J.Float("sub_hp", Group->GetHitPoints());
                J.Double("sub_hp_max", Group->GetMaxHitPoints());
                J.Int("sub_state", Group->State);
                J.Bool("sub_proxy", Group->IsRootHealthProxy());
                J.Uint("sub_linked_type", Group->LinkedEquipmentType());
            }
            else if (Equip != nullptr)
            {
                J.Str("sub_kind", Equip->GetType() == EquipTypeShield ? "shield" : "equip");
                J.Float("sub_hp", Equip->GetHitPoints());
                J.Double("sub_hp_max", Equip->GetMaxHitPoints());
                J.Uint("sub_type", Equip->GetType());
                J.Bool("sub_destroyed", Equip->IsDestroyed());
                if (Equip->GetType() == EquipTypeShield)
                {
                    CEShield* const Shield = static_cast<CEShield*>(Equip);
                    J.Float("shield_offline_threshold", Shield->OfflineThreshold);
                    if (Munition != nullptr)
                        J.Double("shield_weapon_mod", Shield->GetWeaponModifier(Munition->WeaponType));
                }
            }
            else
            {
                J.Str("sub_kind", "root");
            }
            Found->Release();
        }
        J.EndObject();
    }

    void __cdecl OnMunitionHit(unsigned int TargetId, unsigned int ShooterId, unsigned int MunitionArchId,
                               unsigned short SubObjId, Vector const& HitPos, DamageList const& Damage)
    {
        FOracleJson J;
        J.Str("rec", "munition_hit");
        J.Uint("tick", GTick);
        IdAndName(J, "target", "target_obj", TargetId);
        IdAndName(J, "shooter", "shooter_obj", ShooterId);
        J.Uint("munition", MunitionArchId);
        J.Uint("sub", SubObjId);
        VectorJ(J, "hit_pos", HitPos);
        J.BeginObject("damage");
        J.Int("cause", (int)Damage.Cause);
        J.Uint("inflictor", Damage.InflictorId);
        J.Uint("inflictor_owner", Damage.InflictorOwner);
        J.Bool("destroyed", Damage.bDestroyed);
        J.Int("count", Damage.Count);
        J.BeginArray("entries");
        for (std::list<DamageEntry>::const_iterator It = Damage.Entries.begin(); It != Damage.Entries.end(); ++It)
        {
            J.BeginArray(nullptr);
            J.Uint(nullptr, It->SubObjId);
            J.Float(nullptr, It->HitPts);
            J.Int(nullptr, (int)It->Fate);
            J.EndArray();
        }
        J.EndArray();
        J.EndObject();
        MunitionHitState(J, TargetId, MunitionArchId, SubObjId);
        Write(FileCalls, J);

        FOracleJson E;
        E.Str("rec", "munition_hit");
        E.Uint("tick", GTick);
        IdAndName(E, "target", "target_obj", TargetId);
        IdAndName(E, "shooter", "shooter_obj", ShooterId);
        E.Uint("munition", MunitionArchId);
        E.Uint("sub", SubObjId);
        Write(FileEvents, E);
        Metric("munition_hit", NameOf(TargetId));
    }

    // The object state behaviour records carry (IObjInspect of the server object).
    void InspectState(FOracleJson& J, IObjRW* Object)
    {
        VectorJ(J, "pos", Object->get_position());
        MatrixJ(J, "orient", Object->get_orientation());
        VectorJ(J, "vel", Object->get_velocity());
        VectorJ(J, "angvel", Object->get_angular_velocity());
        float Throttle = 0.0f;
        Object->get_throttle(Throttle);
        J.Float("throttle", Throttle);
        Vector Axis = {0.0f, 0.0f, 0.0f};
        Object->get_axis_throttle(Axis);
        VectorJ(J, "axis", Axis);
        Vector Nudge = {0.0f, 0.0f, 0.0f};
        Object->get_nudge_vec(Nudge);
        VectorJ(J, "nudge", Nudge);
        StrafeDir Strafe = StrafeNone;
        Object->get_strafe_dir(Strafe);
        J.Int("strafe", (int)Strafe);
        bool bCruise = false;
        Object->is_cruise_active(bCruise);
        J.Bool("cruise", bCruise);
    }

    void BehaviorFields(FOracleJson& J, FOracleSimBehavior const& Behavior)
    {
        J.Int("behavior", Behavior.BehaviorIndex);
        J.Int("state_graph", Behavior.StateGraphId);
        J.BeginArray("directive");
        for (unsigned int Word : Behavior.Directive)
            J.Uint(nullptr, Word);
        J.EndArray();
    }

    void __cdecl OnBehaviorTick(IBehaviorManager const* Manager, float Dt, int Phase)
    {
        FOracleSimBehavior Behavior;
        OracleSimReadBehavior(Manager, Behavior);
        FOracleJson J;
        if (Phase == 0)
        {
            GOpenBehavior = ++GBehaviorSeq;
            J.Str("rec", "behavior_in");
            J.Uint("tick", GTick);
            J.Uint("seq", GOpenBehavior);
            IdAndName(J, "id", "obj", Behavior.ObjectId);
            J.Float("dt", Dt);
            BehaviorFields(J, Behavior);
            J.Bool("suppressed", Behavior.bSuppressed != 0);
            if (Behavior.Object != nullptr)
                InspectState(J, Behavior.Object);
        }
        else
        {
            J.Str("rec", "behavior_out");
            J.Uint("tick", GTick);
            J.Uint("seq", GOpenBehavior);
            IdAndName(J, "id", "obj", Behavior.ObjectId);
            BehaviorFields(J, Behavior);
            GOpenBehavior = 0;
        }
        Write(FileCalls, J);
    }

    char const* AffectName(int Slot)
    {
        switch (Slot)
        {
        case OracleSimAffectThrottle:
            return "set_throttle";
        case OracleSimAffectAxisThrottle:
            return "set_axis_throttle";
        case OracleSimAffectNudge:
            return "set_nudge_vec";
        case OracleSimAffectStrafe:
            return "set_strafe_dir";
        case OracleSimAffectFire:
            return "fire";
        case OracleSimAffectTractor:
            return "tractor";
        case OracleSimAffectGunTarget:
            return "gun_target";
        case OracleSimAffectTarget:
            return "set_target";
        case OracleSimAffectEquip:
            return "activate_equip";
        case OracleSimAffectCruise:
            return "engage_cruise";
        case OracleSimAffectThrusters:
            return "set_thrusters";
        case OracleSimAffectRequest:
            return "request";
        case OracleSimAffectCancelRequest:
            return "cancel_request";
        case OracleSimAffectGoTradeLane:
            return "go_tradelane";
        case OracleSimAffectStopTradeLane:
            return "stop_tradelane";
        default:
            return "unknown";
        }
    }

    void __cdecl OnAffect(unsigned int ObjectId, int Slot, int IntArg, float const* Values, int Count)
    {
        FOracleJson J;
        J.Str("rec", "affect");
        J.Uint("tick", GTick);
        J.Uint("beh", GOpenBehavior);
        J.Str("src", GbScenarioCommand ? "scenario" : (GOpenBehavior != 0 ? "behavior" : "other"));
        IdAndName(J, "id", "obj", ObjectId);
        J.Int("slot", Slot);
        J.Str("m", AffectName(Slot));
        J.Int("i", IntArg);
        // The instrumented calls in progress around this one (FOracleSimAffectScope): 0 for a
        // call the caller made, 1 or more for one an IObjAffect body made.
        J.Int("depth", GOracleSimHooks.AffectDepth > 0 ? GOracleSimHooks.AffectDepth - 1 : 0);
        if (Values != nullptr)
            J.FloatArray("v", Values, (unsigned)Count);
        else
            J.Null("v");
        Write(FileCalls, J);
    }

    // P3.4a, the dispatch trace (dispatch.jsonl, design/sim-records.md; record 0006 point 10):
    // every delivery of Server.dll's controller registry, before the handler runs, numbered in
    // order from the start-up on.
    unsigned int GDispatchSeq = 0;

    void __cdecl OnControllerDeliver(unsigned int Flid, int MsgId, void const* MsgData, int Path, int Depth)
    {
        static char const* const kPaths[] = {"broadcast", "send", "heartbeat"};
        FOracleJson J;
        J.Str("rec", "deliver");
        J.Uint("tick", GTick);
        J.Uint("seq", GDispatchSeq++);
        J.Int("depth", Depth);
        J.Uint("flid", Flid);
        J.Int("msg", MsgId);
        J.Str("path", Path >= 0 && Path < 3 ? kPaths[Path] : "?");
        if (MsgData != nullptr)
        {
            if (MsgId == (int)pub::Controller::EMessage::Heartbeat)
            {
                J.Float("elapsed", *static_cast<float const*>(MsgData));
            }
            else if (MsgId == (int)pub::Controller::EMessage::TimerExpired)
            {
                unsigned int const* const Words = static_cast<unsigned int const*>(MsgData);
                J.Uint("p1", Words[0]);
                J.Uint("p2", Words[1]);
            }
        }
        Write(FileDispatch, J);
    }

    // SimClientStub's controller hands every controller message here.
    void __cdecl OnControllerMessage(int MessageId, void const* Data)
    {
        if (Data == nullptr)
            return;
        unsigned int const* const Words = static_cast<unsigned int const*>(Data);
        if (MessageId == (int)pub::Controller::EMessage::DockComplete)
        {
            FOracleJson J;
            J.Str("rec", "dock");
            J.Uint("tick", GTick);
            IdAndName(J, "ship", "ship_obj", Words[0]);
            IdAndName(J, "dock", "dock_obj", Words[1]);
            J.Uint("system", Words[2]);
            Write(FileEvents, J);
            Metric("dock", NameOf(Words[0]));
        }
        else if (MessageId == (int)pub::Controller::EMessage::ObjectDestroyed)
        {
            float Position[3];
            memcpy(Position, Words + 5, sizeof(Position));
            FOracleJson J;
            J.Str("rec", "destroyed");
            J.Uint("tick", GTick);
            IdAndName(J, "id", "obj", Words[0]);
            IdAndName(J, "killer", "killer_obj", Words[1]);
            J.Int("kind", (int)Words[4]);
            J.FloatArray("pos", Position, 3);
            Write(FileEvents, J);
            Metric("destroyed", NameOf(Words[0]));
        }
    }

    // ------------------------------------------------------------------------------------
    // Per-tick record
    // ------------------------------------------------------------------------------------

    void RecordTick(unsigned int X87, unsigned int Sse)
    {
        // P2.4: the asteroid cubes the frame made or changed (asteroids.jsonl).
        oracle::OracleSimFieldsTick();
        FOracleJson F;
        F.Str("rec", "frame");
        F.Uint("tick", GTick);
        F.Double("clock", GOracleSimHooks.ClockSeconds);
        F.Str("x87_cw", Format("%08x", X87).c_str());
        F.Str("sse_cw", Format("%08x", Sse).c_str());
        F.Uint("psi_count", (unsigned)GPsiDt.size());
        F.FloatArray("psi_dt", GPsiDt.empty() ? nullptr : GPsiDt.data(), (unsigned)GPsiDt.size());
        F.BeginArray("psi_mcheck");
        for (int Value : GPsiCheck)
            F.Int(nullptr, Value);
        F.EndArray();
        F.BeginArray("psi_ivp_seed");
        for (unsigned Value : GPsiSeed)
            F.Uint(nullptr, Value);
        F.EndArray();
        Write(FileTick, F);

        for (FSimObject const& Object : GObjects)
        {
            FOracleJson J;
            J.Str("rec", "object");
            J.Uint("tick", GTick);
            J.Str("obj", Object.Desc->Name.c_str());
            J.Uint("id", Object.Id);
            IObjRW* const Rw = FindRw(Object);
            J.Bool("alive", Rw != nullptr);
            double Values[22] = {};
            if (Rw != nullptr)
            {
                Vector const Pos = Rw->get_position();
                Matrix const Orient = Rw->get_orientation();
                Vector const Vel = Rw->get_velocity();
                Vector const AngVel = Rw->get_angular_velocity();
                float HitPts = 0.0f;
                float MaxHitPts = 0.0f;
                Rw->get_status(HitPts, MaxHitPts);
                VectorJ(J, "pos", Pos);
                MatrixJ(J, "orient", Orient);
                VectorJ(J, "vel", Vel);
                VectorJ(J, "angvel", AngVel);
                J.Float("hp", HitPts);
                J.Float("hp_max", MaxHitPts);
                // P2.4 (scenario report): the power and the first shield of an equipped object
                // (CEqObj::Power, MaxPower; CEShield's hit points, maximum and bOnline), `null`
                // without a shield.
                {
                    unsigned int Id = Object.Id;
                    CObject* const Found = CObject::Find(Id, Object.Class);
                    if (Found != nullptr)
                    {
                        CEqObj* const Eq = static_cast<CEqObj*>(Found);
                        J.Float("power", Eq->Power);
                        J.Float("power_max", Eq->MaxPower);
                        CEquip* const ShieldEquip = Eq->EquipManager.FindFirst(EquipTypeShield);
                        CEShield* const Shield = ShieldEquip != nullptr ? CEShield::cast(ShieldEquip) : nullptr;
                        if (Shield != nullptr)
                        {
                            J.BeginObject("shield");
                            J.Uint("id", ShieldEquip->GetID());
                            J.Float("hp", Shield->GetHitPoints());
                            J.Float("hp_max", Shield->GetMaxHitPoints());
                            J.Bool("online", Shield->bOnline != 0);
                            J.Bool("functioning", Shield->IsFunctioning());
                            J.EndObject();
                        }
                        else
                        {
                            J.Null("shield");
                        }
                        Found->Release();
                    }
                }
                if (Object.Class == CObject::ClassShip)
                {
                    float Throttle = 0.0f;
                    Rw->get_throttle(Throttle);
                    J.Float("throttle", Throttle);
                    Vector Axis = {0.0f, 0.0f, 0.0f};
                    Rw->get_axis_throttle(Axis);
                    VectorJ(J, "axis", Axis);
                    Vector Nudge = {0.0f, 0.0f, 0.0f};
                    Rw->get_nudge_vec(Nudge);
                    VectorJ(J, "nudge", Nudge);
                    StrafeDir Strafe = StrafeNone;
                    Rw->get_strafe_dir(Strafe);
                    J.Int("strafe", (int)Strafe);
                    bool bCruise = false;
                    Rw->is_cruise_active(bCruise);
                    J.Bool("cruise", bCruise);
                    FOracleSimBehavior Behavior;
                    OracleSimReadBehavior(OracleSimBehaviorOf(const_cast<CObject*>(Rw->cobject())), Behavior);
                    BehaviorFields(J, Behavior);
                    // P2.5 (the strafe's bullets): each gun's turret joints, [id, yaw current,
                    // yaw desired, pitch current, pitch desired, target point age, yaw bound, pitch bound,
                    // lock target id].
                    {
                        unsigned int GunId = Object.Id;
                        CObject* const GunOwner = CObject::Find(GunId, CObject::ClassShip);
                        J.BeginArray("guns");
                        if (GunOwner != nullptr)
                        {
                            CEqObj* const Eq = static_cast<CEqObj*>(GunOwner);
                            CEquipTraverser Walk(EquipTypeGun);
                            for (CEquip* Equip = Eq->EquipManager.Traverse(Walk); Equip != nullptr;
                                 Equip = Eq->EquipManager.Traverse(Walk))
                            {
                                CEGun const* const Gun = CEGun::cast(Equip);
                                if (Gun == nullptr)
                                    continue;
                                J.BeginArray(nullptr);
                                J.Uint(nullptr, Equip->GetID());
                                J.Float(nullptr, Gun->Yaw.Current);
                                J.Float(nullptr, Gun->Yaw.Desired);
                                J.Float(nullptr, Gun->Pitch.Current);
                                J.Float(nullptr, Gun->Pitch.Desired);
                                J.Float(nullptr, Gun->TargetPointAge);
                                J.Int(nullptr, Gun->Yaw.EngineIndex != -1 ? 1 : 0);
                                J.Int(nullptr, Gun->Pitch.EngineIndex != -1 ? 1 : 0);
                                CSimple* Lock = nullptr;
                                unsigned short LockSub = 0;
                                Gun->GetTarget(Lock, LockSub);
                                J.Uint(nullptr, Lock != nullptr ? Lock->get_id() : 0u);
                                J.EndArray();
                            }
                            GunOwner->Release();
                        }
                        J.EndArray();
                    }
                    // P2.5 (the dogfight): the ship's target (IObjInspect::get_target), and a
                    // `target` event when it changed since the last tick.
                    IObjRW* Target = nullptr;
                    unsigned int const TargetId =
                        (Rw->get_target(Target) == 0 && Target != nullptr) ? Target->get_id() : 0u;
                    if (TargetId != 0)
                    {
                        IdAndName(J, "target", "target_obj", TargetId);
                    }
                    else
                    {
                        J.Null("target");
                        J.Null("target_obj");
                    }
                    unsigned int& Last = GLastTarget[Object.Id];
                    if (Last != TargetId)
                    {
                        FOracleJson E;
                        E.Str("rec", "target");
                        E.Uint("tick", GTick);
                        IdAndName(E, "id", "obj", Object.Id);
                        if (TargetId != 0)
                        {
                            IdAndName(E, "target", "target_obj", TargetId);
                            Metric("target", Object.Desc->Name);
                        }
                        else
                        {
                            E.Null("target");
                            E.Null("target_obj");
                        }
                        Write(FileEvents, E);
                        Last = TargetId;
                    }
                }
                Values[0] = Pos.x;
                Values[1] = Pos.y;
                Values[2] = Pos.z;
                for (int Index = 0; Index < 9; ++Index)
                    Values[3 + Index] = Orient.E[Index / 3][Index % 3];
                Values[12] = Vel.x;
                Values[13] = Vel.y;
                Values[14] = Vel.z;
                Values[15] = AngVel.x;
                Values[16] = AngVel.y;
                Values[17] = AngVel.z;
                Values[18] = HitPts;
            }
            Write(FileTick, J);

            std::string Line = Format("T %u %s %d", GTick, Object.Desc->Name.c_str(), Rw != nullptr ? 1 : 0);
            for (int Index = 0; Index < 19; ++Index)
                Line += Format(" %.17g", Values[Index]);
            Write(FileMetrics, Line);
        }

        // P2.5 (the missile): every guided munition seen in a simulation unit, after the
        // frame: its CGuided state, then `alive` false once and it is dropped.
        std::vector<unsigned int> Gone;
        for (unsigned int const GuidedId : GGuidedIds)
        {
            FOracleJson J;
            J.Str("rec", "guided");
            J.Uint("tick", GTick);
            J.Uint("id", GuidedId);
            unsigned int Id = GuidedId;
            CObject* const Found = CObject::Find(Id, CObject::ClassGuided);
            J.Bool("alive", Found != nullptr);
            if (Found == nullptr)
            {
                Gone.push_back(GuidedId);
                Write(FileTick, J);
                continue;
            }
            CGuided* const Guided = static_cast<CGuided*>(Found);
            J.Uint("owner", Guided->OwnerId);
            VectorJ(J, "pos", Guided->WorldPosition);
            MatrixJ(J, "orient", Guided->Orientation);
            VectorJ(J, "vel", Guided->get_velocity());
            VectorJ(J, "angvel", Guided->get_angular_velocity());
            J.Float("lifetime", Guided->Lifetime);
            J.Float("seeker_elapsed", Guided->SeekerElapsed);
            IObjRW* const Target = Guided->get_target();
            if (Target != nullptr)
                IdAndName(J, "target", "target_obj", Target->get_id());
            else
            {
                J.Null("target");
                J.Null("target_obj");
            }
            J.Uint("sub_target", Guided->SubTarget);
            Found->Release();
            Write(FileTick, J);
        }
        for (unsigned int const GuidedId : Gone)
            GGuidedIds.erase(GuidedId);
        GPsiDt.clear();
        GPsiCheck.clear();
        GPsiSeed.clear();
    }

    // ------------------------------------------------------------------------------------
    // Spawning and commands
    // ------------------------------------------------------------------------------------

    Matrix ToMatrix(double const* Values)
    {
        Matrix Out;
        for (int Index = 0; Index < 9; ++Index)
            Out.E[Index / 3][Index % 3] = (float)Values[Index];
        return Out;
    }

    Vector ToVector(double const* Values)
    {
        Vector Out = {(float)Values[0], (float)Values[1], (float)Values[2]};
        return Out;
    }

    // P2.5: the `faction` option. A vibe made as Content makes an NPC's reputation
    // (pub::Reputation::Alloc with empty names, then SetAffiliation to the group), so that
    // the ship's attitude towards another ship is the groups' feeling in initialworld.ini.
    // Returns false when the group is unknown; Vibe stays -1 without the option.
    bool AllocFactionVibe(FObjectDesc const& Desc, FOracleJson& J, int& Vibe)
    {
        Vibe = -1;
        if (Desc.Faction.empty())
            return true;
        unsigned int Group = 0;
        int const GroupResult = pub::Reputation::GetReputationGroup(Group, Desc.Faction.c_str());
        J.Str("faction", Desc.Faction.c_str());
        J.Int("faction_result", GroupResult);
        if (GroupResult != 0)
            return false;
        FmtStr Name;
        FmtStr Nickname;
        int const AllocResult = pub::Reputation::Alloc(Vibe, Name, Nickname);
        int const AffiliationResult = pub::Reputation::SetAffiliation(Vibe, Group);
        J.Uint("faction_id", Group);
        J.Int("vibe", Vibe);
        J.Int("vibe_alloc", AllocResult);
        J.Int("vibe_affiliation", AffiliationResult);
        return AllocResult == 0 && AffiliationResult == 0;
    }

    // P3.6c: a `content` ship, made by Content.dll's OracleSimCreateNpcShip
    // (oracle_content_npc.cpp): FSystemInfo::CreateNpcShip with the ship's archetype, loadout,
    // place, hit points, state graph, pilot personality and the vibe of its `faction`, owned by
    // SpaceSupport. The constructor submits the state with the personality and both callbacks
    // itself, so no SubmitState follows here.
    bool SpawnContentShip(FObjectDesc const& Desc, unsigned int SystemId, int Vibe, FOracleJson& J,
                          FSimObject& Object)
    {
        typedef unsigned int(__cdecl * FCreateNpcShip)(
            unsigned int, unsigned int, unsigned int, Vector const*, Matrix const*, int, int,
            pub::AI::Personality const*, int, bool, Vector const*, float);
        typedef bool(__cdecl * FPilotPersonality)(char const*, pub::AI::Personality*);
        HMODULE const Content = GetModuleHandleA("Content.dll");
        FCreateNpcShip const Create =
            Content != nullptr ? (FCreateNpcShip)GetProcAddress(Content, "OracleSimCreateNpcShip") : nullptr;
        J.Str("kind", "ship");
        J.Bool("content", true);
        J.Str("arch", Desc.Archetype.c_str());
        J.Str("loadout", Desc.Loadout.c_str());
        if (Create == nullptr)
        {
            J.Str("error", "Content.dll lacks OracleSimCreateNpcShip");
            return false;
        }
        pub::AI::Personality Personality;
        bool bPilot = true;
        if (!Desc.Pilot.empty())
        {
            FPilotPersonality const PilotPersonality =
                (FPilotPersonality)GetProcAddress(Content, "OracleSimPilotPersonality");
            bPilot = PilotPersonality != nullptr && PilotPersonality(Desc.Pilot.c_str(), &Personality);
            J.Str("pilot", Desc.Pilot.c_str());
            J.Bool("pilot_found", bPilot);
        }
        int const StateGraphId =
            pub::StateGraph::get_state_graph(Desc.StateGraph.c_str(), pub::StateGraph::LEADER);
        J.Str("state_graph", Desc.StateGraph.c_str());
        J.Int("state_graph_id", StateGraphId);
        Vector const Position = ToVector(Desc.Pos);
        Matrix const Orientation = ToMatrix(Desc.Orient);
        Vector const FleePosition = ToVector(Desc.ContentFlee);
        unsigned int const Id = Create(SystemId, CreateID(Desc.Archetype.c_str()),
                                       Desc.Loadout.empty() ? 0u : CreateID(Desc.Loadout.c_str()),
                                       &Position, &Orientation, Desc.HitPts, StateGraphId,
                                       &Personality, Vibe,
                                       Desc.bContentFlee, &FleePosition,
                                       (float)Desc.ContentFleeDistance);
        Object.Id = Id;
        J.Uint("id", Id);
        if (Desc.bContentFlee)
        {
            J.Bool("flee", true);
            J.Float("flee_distance", (float)Desc.ContentFleeDistance);
        }
        return bPilot && Id != 0;
    }

    bool SpawnObject(FObjectDesc const& Desc, unsigned int SystemId, unsigned int ControllerId)
    {
        FSimObject Object;
        Object.Desc = &Desc;
        FOracleJson J;
        J.Str("rec", "spawn");
        J.Str("obj", Desc.Name.c_str());
        bool bOk = false;
        if (Desc.bShip)
        {
            Object.Class = CObject::ClassShip;
            int Vibe = -1;
            if (!AllocFactionVibe(Desc, J, Vibe))
            {
                Write(FileSession, J);
                return false;
            }
            if (Desc.bContent)
            {
                bOk = SpawnContentShip(Desc, SystemId, Vibe, J, Object);
                Write(FileSession, J);
                if (bOk)
                    GObjects.push_back(Object);
                return bOk;
            }
            pub::SpaceObj::ShipInfo Info;
            Info.ControllerId = ControllerId;
            Info.SystemID = SystemId;
            Info.ShipArchID = CreateID(Desc.Archetype.c_str());
            Info.Position = ToVector(Desc.Pos);
            Info.AngularVelocity = ToVector(Desc.AngVel);
            Info.Velocity = ToVector(Desc.Vel);
            Info.Orientation = ToMatrix(Desc.Orient);
            Info.Field_0x54 = 0;
            Info.LoadoutID = Desc.Loadout.empty() ? 0u : CreateID(Desc.Loadout.c_str());
            Info.CostumeHead = 0;
            Info.CostumeBody = 0;
            Info.CostumeLeftHand = 0;
            Info.CostumeRightHand = 0;
            for (int Index = 0; Index < 8; ++Index)
                Info.CostumeAccessory[Index] = 0;
            Info.CostumeAccessoryCount = 0;
            Info.VibeGroupId = Vibe;
            Info.VoiceId = 0;
            Info.DockingBaseId = 0;
            Info.HitPts = Desc.HitPts;
            Info.Field_0xb4 = 0;
            Info.Field_0xb8 = 0;
            Info.ReputationRank = 0;
            unsigned int Id = 0;
            int const Result = pub::SpaceObj::Create(Id, Info);
            Object.Id = Id;
            J.Str("kind", "ship");
            J.Str("arch", Desc.Archetype.c_str());
            J.Uint("arch_id", Info.ShipArchID);
            J.Str("loadout", Desc.Loadout.c_str());
            J.Uint("loadout_id", Info.LoadoutID);
            J.Int("result", Result);
            J.Uint("id", Id);
            bOk = Result == 0 && Id != 0;
            // P2.5 (the transport's collision geometry): the ship's part map and each
            // attached item's attachment (non-physical with its hardpoint CRC and collision
            // state, physical, or none) and whether it is connected.
            if (bOk)
            {
                unsigned int FindId = Id;
                CObject* const Spawned = CObject::Find(FindId, CObject::ClassShip);
                if (Spawned != nullptr)
                {
                    J.BeginArray("part_map");
                    for (FPartMapEntry const& Entry : Spawned->PartMap)
                    {
                        J.BeginArray(nullptr);
                        J.Uint(nullptr, Entry.PartId);
                        J.Int(nullptr, (int)Entry.EngineInstanceId);
                        J.EndArray();
                    }
                    J.EndArray();
                    J.BeginArray("attached");
                    CEqObj* const Eq = static_cast<CEqObj*>(Spawned);
                    CEquipTraverser Walk(-1);
                    for (CEquip* Equip = Eq->EquipManager.Traverse(Walk); Equip != nullptr;
                         Equip = Eq->EquipManager.Traverse(Walk))
                    {
                        CAttachedEquip* const Attached = CAttachedEquip::cast(Equip);
                        if (Attached == nullptr)
                            continue;
                        J.BeginObject(nullptr);
                        J.Uint("id", Equip->GetID());
                        J.Bool("connected", Attached->IsConnected());
                        CNonPhysAttachment* const NonPhys = dynamic_cast<CNonPhysAttachment*>(Attached->Attachment);
                        CPhysAttachment* const Phys = dynamic_cast<CPhysAttachment*>(Attached->Attachment);
                        if (NonPhys != nullptr)
                        {
                            J.Str("kind", "non_physical");
                            J.Uint("hp_crc", NonPhys->HpCrc);
                            J.Bool("collisions", NonPhys->bCollisions != 0);
                        }
                        else if (Phys != nullptr)
                        {
                            J.Str("kind", "physical");
                        }
                        else
                        {
                            J.Str("kind", "none");
                        }
                        J.EndObject();
                    }
                    J.EndArray();
                    Spawned->Release();
                }
            }
            if (bOk && !Desc.StateGraph.empty())
            {
                pub::AI::SetPersonalityParams Params;
                Params.StateGraphId = pub::StateGraph::get_state_graph(
                    Desc.StateGraph.c_str(), Desc.bEscort ? pub::StateGraph::ESCORT : pub::StateGraph::LEADER);
                Params.ContentCallbackObject = nullptr;
                Params.DirectiveCallbackObject = nullptr;
                Params.bSetPersonality = 0;
                J.Str("state_graph", Desc.StateGraph.c_str());
                J.Int("state_graph_id", Params.StateGraphId);
                if (Desc.bEscort)
                    J.Bool("escort", true);
                if (!Desc.Pilot.empty())
                {
                    // P2.5: the personality Content gives an NPC ship of an npcships.ini entry
                    // (NpcShip's GetPilotPersonalityWithJob without a job): the default
                    // personality filled from the [Pilot] of missions\pilots_*.ini, through
                    // Content.dll's Oracle export.
                    typedef bool(__cdecl * FPilotPersonality)(char const*, pub::AI::Personality*);
                    HMODULE const Content = GetModuleHandleA("Content.dll");
                    FPilotPersonality const PilotPersonality =
                        Content != nullptr
                            ? (FPilotPersonality)GetProcAddress(Content, "OracleSimPilotPersonality")
                            : nullptr;
                    bool const bPilot =
                        PilotPersonality != nullptr && PilotPersonality(Desc.Pilot.c_str(), &Params.Value);
                    J.Str("pilot", Desc.Pilot.c_str());
                    J.Bool("pilot_found", bPilot);
                    Params.bSetPersonality = bPilot ? 1 : 0;
                    bOk = bPilot;
                }
                J.Int("submit_state", (int)pub::AI::SubmitState(Id, &Params));
            }
        }
        else
        {
            Object.Class = CObject::ClassSolar;
            unsigned int Id = CreateID(Desc.Nickname.c_str());
            CObject* const Found = CObject::Find(Id, CObject::ClassSolar);
            if (Found != nullptr)
                Found->Release();
            Object.Id = Id;
            J.Str("kind", "solar");
            J.Str("nickname", Desc.Nickname.c_str());
            J.Uint("id", Id);
            J.Bool("found", Found != nullptr);
            bOk = Found != nullptr;
        }
        Write(FileSession, J);
        if (bOk)
            GObjects.push_back(Object);
        return bOk;
    }

    // P3.3 (decision record 0007 point 11): each `player` of the scenario joins its system, in
    // file order, after the objects are spawned (Server.dll's OracleSimAddPlayer: the slot's
    // system and ship, then StarSystem::add_player, whose packets are tick 0's). One `player`
    // record each in session.jsonl. False when a ship is missing or a join is refused.
    bool AddPlayers(unsigned int SystemId)
    {
        bool bOk = true;
        for (FPlayerDesc const& Player : GScenario.Players)
        {
            FSimObject const* const Object = FindObject(Player.Object);
            unsigned int const ShipId = Object != nullptr ? Object->Id : 0u;
            bool const bJoined = ShipId != 0u && OracleSimAddPlayer(Player.Session, SystemId, ShipId);
            FOracleJson J;
            J.Str("rec", "player");
            J.Uint("tick", GTick);
            J.Int("line", Player.Line);
            J.Uint("session", Player.Session);
            J.Str("obj", Player.Object.c_str());
            J.Uint("id", ShipId);
            J.Bool("joined", bJoined);
            Write(FileSession, J);
            bOk = bJoined && bOk;
        }
        return bOk;
    }

    // P3.6d: `population <ship>` through Content.dll's OracleSimPopulate
    // (oracle_content_population.cpp): player 1 with the ship, then SpacePop and its launch.
    bool Populate(unsigned int SystemId)
    {
        typedef unsigned int(__cdecl * FPopulate)(unsigned int, unsigned int, unsigned int, unsigned int);
        HMODULE const Content = GetModuleHandleA("Content.dll");
        FPopulate const PopulateFn =
            Content != nullptr ? (FPopulate)GetProcAddress(Content, "OracleSimPopulate") : nullptr;
        FSimObject const* const Object = FindObject(GScenario.PopulationShip);
        unsigned int const ShipId = Object != nullptr ? Object->Id : 0u;
        unsigned int const ShipArch =
            Object != nullptr && Object->Desc != nullptr ? CreateID(Object->Desc->Archetype.c_str()) : 0u;
        unsigned int const SpacePop =
            PopulateFn != nullptr && ShipId != 0u ? PopulateFn(1u, SystemId, ShipId, ShipArch) : 0u;
        FOracleJson J;
        J.Str("rec", "population");
        J.Uint("tick", GTick);
        J.Int("line", GScenario.PopulationLine);
        J.Str("obj", GScenario.PopulationShip.c_str());
        J.Uint("id", ShipId);
        J.Uint("space_pop", SpacePop);
        if (PopulateFn == nullptr)
            J.Str("error", "Content.dll lacks OracleSimPopulate");
        Write(FileSession, J);
        return SpacePop != 0u;
    }

    int SubmitScenarioDirective(unsigned int Id, pub::AI::BaseOp& Op)
    {
        // A current directive blocks a new one (pub::AI::SubmitDirective returns 2), so each
        // scenario directive cancels the one before.
        pub::AI::DirectiveCancelOp Cancel;
        pub::AI::SubmitDirective(Id, &Cancel);
        return (int)pub::AI::SubmitDirective(Id, &Op);
    }

    bool CommandVector(FCommand const& Command, size_t At, Vector& Out)
    {
        double Values[3];
        if (!ParseVector(Command.Args, At, Values))
            return false;
        Out = ToVector(Values);
        return true;
    }

    // Runs one command; Result is what the called function returned, Note a failure reason.
    bool ExecuteCommand(FCommand const& Command, int& Result, std::string& Note)
    {
        Result = 0;
        FSimObject const* const Object = FindObject(Command.Object);
        if (Object == nullptr)
            return Note = "object was not spawned", false;
        IObjRW* const Rw = FindRw(*Object);
        if (Rw == nullptr)
            return Note = "object is gone", false;
        IObjAffect* const Affect = static_cast<IObjAffect*>(Rw);
        std::vector<std::string> const& A = Command.Args;
        std::string const& Verb = A[0];
        auto Target = [&](size_t At) -> FSimObject const* {
            return At < A.size() ? FindObject(A[At]) : nullptr;
        };

        if (Verb == "throttle" && A.size() == 2)
        {
            double Value = 0.0;
            if (!ParseDouble(A[1], Value))
                return Note = "bad throttle", false;
            Result = Affect->set_throttle((float)Value);
            return true;
        }
        if ((Verb == "axis" || Verb == "nudge") && A.size() == 4)
        {
            Vector Value;
            if (!CommandVector(Command, 1, Value))
                return Note = "bad vector", false;
            Result = Verb == "axis" ? Affect->set_axis_throttle(Value) : Affect->set_nudge_vec(Value);
            return true;
        }
        if (Verb == "strafe" && A.size() == 2)
        {
            static char const* const Names[5] = {"none", "left", "right", "up", "down"};
            for (int Index = 0; Index < 5; ++Index)
            {
                if (Lower(A[1]) == Names[Index])
                {
                    Result = Affect->set_strafe_dir((StrafeDir)Index);
                    return true;
                }
            }
            return Note = "bad strafe direction", false;
        }
        if (Verb == "fire" && A.size() <= 2)
        {
            Vector TargetPos;
            if (A.size() == 2)
            {
                FSimObject const* const Aim = Target(1);
                IObjRW* const AimRw = Aim != nullptr ? FindRw(*Aim) : nullptr;
                if (AimRw == nullptr)
                    return Note = "no target", false;
                TargetPos = AimRw->get_position();
            }
            else
            {
                // 1000 m straight ahead (-Z of the ship).
                Vector const Pos = Rw->get_position();
                Matrix const Orient = Rw->get_orientation();
                TargetPos.x = (float)(Pos.x - 1000.0 * Orient.E[0][2]);
                TargetPos.y = (float)(Pos.y - 1000.0 * Orient.E[1][2]);
                TargetPos.z = (float)(Pos.z - 1000.0 * Orient.E[2][2]);
            }
            std::vector<unsigned short> Guns;
            unsigned int Id = Object->Id;
            CObject* const Found = CObject::Find(Id, CObject::ClassShip);
            if (Found != nullptr)
            {
                CEqObj* const Eq = static_cast<CEqObj*>(Found);
                CEquipTraverser Walk(-1);
                for (CEquip* Equip = Eq->EquipManager.Traverse(Walk); Equip != nullptr;
                     Equip = Eq->EquipManager.Traverse(Walk))
                {
                    // Unguided guns only: a missile's seeker would add its own steering.
                    CEGun const* const Gun = CEGun::cast(Equip);
                    if (Gun != nullptr && !Gun->is_guided() && !Equip->IsDestroyed())
                        Guns.push_back(Equip->GetID());
                }
                Found->Release();
            }
            if (Guns.empty())
                return Note = "no guns", false;
            Result = Affect->slot_34(Guns.data(), (unsigned)Guns.size(), &TargetPos, -1);
            return true;
        }
        if (Verb == "target" && A.size() == 2)
        {
            IObjRW* AimRw = nullptr;
            if (Lower(A[1]) != "none")
            {
                FSimObject const* const Aim = Target(1);
                AimRw = Aim != nullptr ? FindRw(*Aim) : nullptr;
                if (AimRw == nullptr)
                    return Note = "no target", false;
            }
            Result = Affect->set_target(AimRw, 0, -1);
            return true;
        }
        if (Verb == "goto" && A.size() >= 4)
        {
            pub::AI::DirectiveGotoOp Op;
            Op.Field_0C = 1;   // a point (Content's GotoVec)
            if (!CommandVector(Command, 1, Op.Destination))
                return Note = "bad point", false;
            Op.Field_50 = 100.0f;   // arrival range
            Op.Field_58 = 0;
            Op.Field_5A = 0;        // cruise
            Op.Field_5B = 1;        // no cruise
            for (size_t At = 4; At < A.size(); ++At)
            {
                double Value = 0.0;
                if (Lower(A[At]) == "range" && At + 1 < A.size() && ParseDouble(A[At + 1], Value))
                {
                    Op.Field_50 = (float)Value;
                    ++At;
                }
                else if (Lower(A[At]) == "cruise")
                {
                    Op.Field_5A = 1;
                    Op.Field_5B = 0;
                }
                else
                {
                    return Note = "bad goto option " + A[At], false;
                }
            }
            Result = SubmitScenarioDirective(Object->Id, Op);
            return true;
        }
        if (Verb == "face" && A.size() == 4)
        {
            pub::AI::DirectiveFaceOp Op;
            Op.Mode = 2;   // FacePoint
            if (!CommandVector(Command, 1, Op.FacePoint))
                return Note = "bad point", false;
            Result = SubmitScenarioDirective(Object->Id, Op);
            return true;
        }
        if ((Verb == "dock" || Verb == "ram" || Verb == "dock_request") && A.size() == 2)
        {
            FSimObject const* const Aim = Target(1);
            if (Aim == nullptr)
                return Note = "unknown target", false;
            if (Verb == "dock")
            {
                pub::AI::DirectiveDockOp Op;
                Op.DockObjectId = Aim->Id;
                Result = SubmitScenarioDirective(Object->Id, Op);
            }
            else if (Verb == "ram")
            {
                pub::AI::DirectiveRamOp Op;
                Op.TargetId = Aim->Id;
                Result = SubmitScenarioDirective(Object->Id, Op);
            }
            else
            {
                unsigned int const ShipId = Object->Id;
                unsigned int const TargetId = Aim->Id;
                Result = pub::SpaceObj::DockRequest(ShipId, TargetId);
            }
            return true;
        }
        if (Verb == "launch" && A.size() == 4)
        {
            // P2.5 launch: DirectiveLaunchOp {target, dock index, terminal type}, as Content's
            // LaunchQueuedShips gives a ship it places at a dock (FObjListLaunch::SetLaunch).
            FSimObject const* const Aim = Target(1);
            if (Aim == nullptr)
                return Note = "unknown target", false;
            double Index = 0.0;
            double Terminal = 0.0;
            if (!ParseDouble(A[2], Index) || !ParseDouble(A[3], Terminal))
                return Note = "bad launch index or terminal", false;
            pub::AI::DirectiveLaunchOp Op;
            Op.LaunchObjectId = Aim->Id;
            Op.Field_10 = (int32_t)Index;
            Op.Field_14 = (int32_t)Terminal;
            Result = SubmitScenarioDirective(Object->Id, Op);
            return true;
        }
        if (Verb == "tradelane" && (A.size() == 3 || A.size() == 4))
        {
            // P2.5 trade lanes: DirectiveInstantTradelaneOp {entrance ring, exit ring (none: 0),
            // speed (a float in the third word; default 0)}.
            FSimObject const* const Entrance = Target(1);
            if (Entrance == nullptr)
                return Note = "unknown entrance", false;
            unsigned int ExitId = 0;
            if (Lower(A[2]) != "none")
            {
                FSimObject const* const Exit = Target(2);
                if (Exit == nullptr)
                    return Note = "unknown exit", false;
                ExitId = Exit->Id;
            }
            double Speed = 0.0;
            if (A.size() == 4 && !ParseDouble(A[3], Speed))
                return Note = "bad speed", false;
            float const SpeedF = (float)Speed;
            pub::AI::DirectiveInstantTradelaneOp Op;
            Op.TradeLaneId = Entrance->Id;
            Op.Field_10 = (int32_t)ExitId;
            memcpy(&Op.Field_14, &SpeedF, sizeof(SpeedF));
            Result = SubmitScenarioDirective(Object->Id, Op);
            return true;
        }
        // P2.5 escort. `formation <leader|none> [x y z]`: pub::AI::update_formation_state(ship,
        // leader, offset), as Content's ResetFormationState makes a formation (the leader
        // first with itself as leader and no offset, then each wingman with its slot).
        if (Verb == "formation" && (A.size() == 2 || A.size() == 5))
        {
            unsigned int LeaderId = 0;
            if (Lower(A[1]) != "none")
            {
                FSimObject const* const Leader = Target(1);
                if (Leader == nullptr)
                    return Note = "unknown leader", false;
                LeaderId = Leader->Id;
            }
            Vector Offset;
            Offset.x = Offset.y = Offset.z = 0.0f;
            if (A.size() == 5 && !CommandVector(Command, 2, Offset))
                return Note = "bad offset", false;
            Result = (int)pub::AI::update_formation_state(Object->Id, LeaderId, Offset);
            return true;
        }
        // `follow <target> [distance d] [offset x y z] [range r]`: DirectiveFollowOp (distance
        // 150, offset 0, range 400 unless given).
        if (Verb == "follow" && A.size() >= 2)
        {
            FSimObject const* const Aim = Target(1);
            if (Aim == nullptr)
                return Note = "unknown target", false;
            pub::AI::DirectiveFollowOp Op;
            Op.TargetId = Aim->Id;
            for (size_t At = 2; At < A.size(); ++At)
            {
                double Value = 0.0;
                std::string const Key = Lower(A[At]);
                if (Key == "distance" && At + 1 < A.size() && ParseDouble(A[At + 1], Value))
                {
                    Op.FollowDistance = (float)Value;
                    ++At;
                }
                else if (Key == "range" && At + 1 < A.size() && ParseDouble(A[At + 1], Value))
                {
                    Op.Field_20 = (float)Value;
                    ++At;
                }
                else if (Key == "offset" && At + 3 < A.size())
                {
                    Vector Offset;
                    if (!CommandVector(Command, At + 1, Offset))
                        return Note = "bad offset", false;
                    Op.Field_14 = Offset.x;
                    Op.Field_18 = Offset.y;
                    Op.Field_1C = Offset.z;
                    At += 3;
                }
                else
                {
                    return Note = "bad follow option " + A[At], false;
                }
            }
            Result = SubmitScenarioDirective(Object->Id, Op);
            return true;
        }
        // `guide <escorted> <x y z> [range r] [distance d] [offset x y z]`: DirectiveGuideOp
        // to a point (its goto part as the goto command's: Field_0C 1, no cruise; range 100
        // unless given), the escorted ship in GotoParams.Field_5C and as the follow part's
        // target (distance 150, offset 0 unless given); the other fields keep their defaults.
        if (Verb == "guide" && A.size() >= 5)
        {
            FSimObject const* const Escorted = Target(1);
            if (Escorted == nullptr)
                return Note = "unknown escorted ship", false;
            pub::AI::DirectiveGuideOp Op;
            Op.GotoParams.Field_0C = 1;
            if (!CommandVector(Command, 2, Op.GotoParams.Destination))
                return Note = "bad point", false;
            Op.GotoParams.Field_50 = 100.0f;
            Op.GotoParams.Field_58 = 0;
            Op.GotoParams.Field_5A = 0;
            Op.GotoParams.Field_5B = 1;
            Op.GotoParams.Field_5C = Escorted->Id;
            Op.FollowParams.TargetId = Escorted->Id;
            for (size_t At = 5; At < A.size(); ++At)
            {
                double Value = 0.0;
                std::string const Key = Lower(A[At]);
                if (Key == "range" && At + 1 < A.size() && ParseDouble(A[At + 1], Value))
                {
                    Op.GotoParams.Field_50 = (float)Value;
                    ++At;
                }
                else if (Key == "distance" && At + 1 < A.size() && ParseDouble(A[At + 1], Value))
                {
                    Op.FollowParams.FollowDistance = (float)Value;
                    ++At;
                }
                else if (Key == "offset" && At + 3 < A.size())
                {
                    Vector Offset;
                    if (!CommandVector(Command, At + 1, Offset))
                        return Note = "bad offset", false;
                    Op.FollowParams.Field_14 = Offset.x;
                    Op.FollowParams.Field_18 = Offset.y;
                    Op.FollowParams.Field_1C = Offset.z;
                    At += 3;
                }
                else
                {
                    return Note = "bad guide option " + A[At], false;
                }
            }
            Result = SubmitScenarioDirective(Object->Id, Op);
            return true;
        }
        if (Verb == "idle" && A.size() == 1)
        {
            pub::AI::DirectiveIdleOp Op;
            Result = SubmitScenarioDirective(Object->Id, Op);
            return true;
        }
        if (Verb == "cancel" && A.size() == 1)
        {
            pub::AI::DirectiveCancelOp Op;
            Result = (int)pub::AI::SubmitDirective(Object->Id, &Op);
            return true;
        }
        Note = "unknown command";
        return false;
    }

    void RunCommands(size_t& Next)
    {
        while (Next < GScenario.Commands.size() && GScenario.Commands[Next].Tick == GTick)
        {
            FCommand const& Command = GScenario.Commands[Next++];
            int Result = 0;
            std::string Note;
            GbScenarioCommand = true;
            bool const bOk = ExecuteCommand(Command, Result, Note);
            GbScenarioCommand = false;
            FOracleJson J;
            J.Str("rec", "command");
            J.Uint("tick", GTick);
            J.Int("line", Command.Line);
            J.Str("obj", Command.Object.c_str());
            J.Str("cmd", Command.Text.c_str());
            J.Bool("ok", bOk);
            J.Int("result", Result);
            if (!bOk)
                J.Str("note", Note.c_str());
            Write(FileSession, J);
        }
    }

    // ------------------------------------------------------------------------------------
    // Start-up
    // ------------------------------------------------------------------------------------

    // The master's OutpostFrontier.ini with [Initial MP DLLs] reduced to Content.dll's
    // GameSupport, SpaceSupport and BaseSupport (the populators and test controllers would
    // spawn ships of their own) and its `path` pointing at this program's folder, where the
    // build puts Content.dll. Every other line is copied unchanged.
    bool WriteServerIni(std::string const& DataRoot, std::string const& ExeDir, std::string const& OutPath)
    {
        oracle::Bytes Data;
        if (!oracle::ReadWholeFile(DataRoot + "\\OutpostFrontier.ini", Data))
            return false;
        FILE* File = fopen(OutPath.c_str(), "wb");
        if (File == nullptr)
            return false;
        std::string const Text(Data.begin(), Data.end());
        bool bDlls = false;
        size_t Start = 0;
        while (Start < Text.size())
        {
            size_t End = Text.find('\n', Start);
            if (End == std::string::npos)
                End = Text.size();
            std::string Line = Text.substr(Start, End - Start);
            Start = End + 1;
            if (!Line.empty() && Line[Line.size() - 1] == '\r')
                Line.resize(Line.size() - 1);
            std::vector<std::string> const T = Tokens(Line);
            std::string const First = T.empty() ? std::string() : Lower(T[0]);
            if (!First.empty() && First[0] == '[')
                bDlls = Lower(Line).find("[initial mp dlls]") != std::string::npos;
            if (bDlls && First == "path")
            {
                Line = "path = " + ExeDir;
            }
            else if (bDlls && First == "dll")
            {
                // T: dll, =, Content.dll, <interface>, <priority>
                std::string const Interface = T.size() > 3 ? Lower(T[3]) : std::string();
                if (Interface != "gamesupport" && Interface != "spacesupport" && Interface != "basesupport")
                    Line = "; Oracle.exe sim leaves out: " + Line;
            }
            fprintf(File, "%s\r\n", Line.c_str());
        }
        fclose(File);
        return true;
    }

    // [OutpostFrontier] "Data path", as OutpostServer.exe's ReadOutpostFrontierIni reads it.
    bool OpenDataPath(std::string const& IniPath, std::string& DataPath)
    {
        INI_Reader Reader;
        if (!Reader.open(IniPath.c_str(), false))
            return false;
        bool bFound = false;
        while (Reader.read_header())
        {
            if (!Reader.is_header("OutpostFrontier"))
                continue;
            while (Reader.read_value())
            {
                if (Reader.is_value("Data path"))
                {
                    // P2.4: the data path is opened absolute (the current directory is the data
                    // root). The server's field load (SrvAsteroidSystem::load) roots the system
                    // file's path, which Universe::get_filename has already rooted, at the data
                    // path again (GetFileSysPath leaves only absolute paths alone): with the
                    // INI's relative ".\data" it opens ".\data\.\data\universe\..." and no
                    // asteroid field loads. Every other path resolves as before.
                    DataPath = Reader.get_value_string();
                    char Full[MAX_PATH] = {};
                    DWORD const Length = GetFullPathNameA(DataPath.c_str(), MAX_PATH, Full, nullptr);
                    std::string const Opened = (Length != 0 && Length < MAX_PATH) ? std::string(Full) : DataPath;
                    NeuronRuntime::OpenData(Opened.c_str());
                    bFound = true;
                }
            }
        }
        return bFound;
    }

    std::string ExeFolder()
    {
        char Path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, Path, MAX_PATH);
        std::string Folder = Path;
        size_t const Slash = Folder.find_last_of("\\/");
        return Slash == std::string::npos ? std::string(".\\") : Folder.substr(0, Slash + 1);
    }

    void ControlWords(unsigned int& X87, unsigned int& Sse)
    {
        X87 = 0;
        Sse = 0;
        __control87_2(0, 0, &X87, &Sse);
    }

    // The fault recorder's writer (oracle_sim_fault.cpp): one session.jsonl line, flushed.
    void WriteSessionLine(std::string const& Line)
    {
        Write(FileSession, Line);
        if (GFiles[FileSession] != nullptr)
            fflush(GFiles[FileSession]);
    }

    // One `heartbeats` entry (OracleSimControllerHeartbeats' callback).
    void __cdecl WriteHeartbeat(void* Context, unsigned int Flid, unsigned int Priority, char const* Module,
                                float Interval, float Phase, bool bDeferred)
    {
        FOracleJson& J = *static_cast<FOracleJson*>(Context);
        J.BeginObject(nullptr);
        J.Uint("flid", Flid);
        J.Uint("priority", Priority);
        J.Str("module", Module);
        J.Float("interval", Interval);
        J.Float("phase", Phase);
        J.Bool("deferred", bDeferred);
        J.EndObject();
    }

    // One frame of the run, called inside the fault recorder's __try (OracleSimGuardedCall).
    struct FFrameArgs
    {
        IServerImpl* Server = nullptr;
        size_t* NextCommand = nullptr;
        unsigned int X87 = 0;
        unsigned int Sse = 0;
    };

    void SimFrame(void* Arg)
    {
        FFrameArgs* const Args = static_cast<FFrameArgs*>(Arg);
        RunCommands(*Args->NextCommand);
        // OutpostServer.exe's frame (ServerThreadProc) with the fixed frame time.
        Args->Server->Update();
        Timing::UpdateGlobalTime(GScenario.Frame);
        Args->Server->ElapseTime((float)GScenario.Frame);
        RecordTick(Args->X87, Args->Sse);
    }

    // Ends the process: the server is not shut down (its teardown is not part of a scenario
    // and its static destructors are not needed), so the open files are closed first.
    void EndProcess(int Code)
    {
        FOracleJson J;
        J.Str("rec", "end");
        J.Uint("tick", GTick);
        J.Uint("psi_total", GPsiSeq);
        J.Int("code", Code);
        // The machine, compiler and runtime DLLs of the run (session.jsonl `env`; the
        // cross-runner reproducibility work, owner 2026-11-19), after everything the run
        // computes.
        oracle::OracleSimRunRandEnd();
        Write(FileSession, oracle::OracleSimEnvRecord());
        Write(FileSession, J);
        UnitFlush(true);
        CloseAll();
        fflush(stdout);
        fflush(stderr);
        TerminateProcess(GetCurrentProcess(), (UINT)Code);
    }
}

namespace oracle
{
    int RunSimScenario(std::string const& DataRoot, std::string const& ScenarioPath, std::string const& OutDir,
                       FSimOptions const& Options)
    {
        MakeParentDirectories(OutDir + "\\session.jsonl");
        for (int Index = 0; Index < kFileCount; ++Index)
        {
            std::string const Path = OutDir + "\\" + kFileNames[Index];
            GFiles[Index] = fopen(Path.c_str(), "wb");
            if (GFiles[Index] == nullptr)
            {
                fprintf(stderr, "sim: cannot write %s\n", Path.c_str());
                CloseAll();
                return 3;
            }
        }

        std::string Error;
        if (!LoadScenario(ScenarioPath, GScenario, Error))
        {
            SessionNote("error", "scenario " + ScenarioPath + ": " + Error);
            CloseAll();
            return 2;
        }
        // The seed of the seeding point (start-up step 5): --seed's, else the scenario's.
        unsigned int const Reseed = Options.bReseed ? Options.Reseed : GScenario.Seed;

        std::string const ExeDir = ExeFolder();
        std::string const StubPath = ExeDir + "SimClientStub.dll";
        std::string const IniPath = OutDir + "\\server.ini";

        {
            FOracleJson J;
            J.Str("rec", "start");
            J.Uint("version", kSimVersion);
            J.Str("scenario", ScenarioPath.c_str());
            J.Str("data_root", DataRoot.c_str());
            J.Str("system", GScenario.System.c_str());
            J.Uint("system_id", CreateID(GScenario.System.c_str()));
            J.Uint("ticks", GScenario.Ticks);
            J.Double("frame", GScenario.Frame);
            J.Str("stepping", GScenario.bSpStepping ? "sp" : "mp");
            J.Str("collisions", GScenario.bSpCollisions ? "sp" : "mp");
            J.Str("comms", GScenario.bCommsOff ? "off" : "on");
            J.Uint("seed", GScenario.Seed);
            J.Uint("reseed", Reseed);
            J.Double("clock", GScenario.ClockStart);
            J.Uint("objects", (unsigned)GScenario.Objects.size());
            J.Uint("commands", (unsigned)GScenario.Commands.size());
            Write(FileSession, J);
            FlushAll();
        }

        // Server.dll is delay-loaded (Oracle.vcxproj), so the other modes never load it; it is
        // loaded here, first, as a static import was before. Its data export `Server` cannot be
        // delay-loaded and is looked up by name; the function imports (pub::, the
        // OracleSimLoadSystem export) bind to this module on their first call.
        std::string const ServerPath = ExeDir + "Server.dll";
        HMODULE const ServerModule = LoadLibraryA(ServerPath.c_str());
        IServerImpl* const ServerImpl =
            ServerModule != nullptr ? reinterpret_cast<IServerImpl*>(GetProcAddress(ServerModule, "Server")) : nullptr;
        if (ServerImpl == nullptr)
        {
            SessionNote("error", "cannot load " + ServerPath + " or its Server export");
            CloseAll();
            return 3;
        }

        HMODULE const Stub = LoadLibraryA(StubPath.c_str());
        if (Stub == nullptr)
        {
            SessionNote("error", "cannot load " + StubPath);
            CloseAll();
            return 3;
        }
        typedef void(__cdecl * FConfigure)(char const*);
        typedef void(__cdecl * FSetSink)(void(__cdecl*)(int, void const*));
        FConfigure const Configure = (FConfigure)GetProcAddress(Stub, "SimClientStubConfigure");
        FSetSink const SetSink = (FSetSink)GetProcAddress(Stub, "SimClientStubSetSink");
        GStubSetTick = (void(__cdecl*)(unsigned int))GetProcAddress(Stub, "SimClientStubSetTick");
        if (Configure == nullptr || SetSink == nullptr || GStubSetTick == nullptr)
        {
            SessionNote("error", "SimClientStub.dll lacks its exports");
            CloseAll();
            return 3;
        }
        // P3.3: `dump replication`'s record (replication.jsonl), opened when the players join.
        typedef void(__cdecl * FRecordReplication)(char const*);
        FRecordReplication const RecordReplication =
            (FRecordReplication)GetProcAddress(Stub, "SimClientStubRecordReplication");
        if (GScenario.bDumpReplication && RecordReplication == nullptr)
        {
            SessionNote("error", "SimClientStub.dll lacks SimClientStubRecordReplication");
            CloseAll();
            return 3;
        }
        Configure(OutDir.c_str());

        if (!WriteServerIni(DataRoot, ExeDir, IniPath))
        {
            SessionNote("error", "cannot write " + IniPath + " from " + DataRoot + "\\OutpostFrontier.ini");
            CloseAll();
            return 3;
        }
        if (!SetCurrentDirectoryA(DataRoot.c_str()))
        {
            SessionNote("error", "cannot enter " + DataRoot);
            CloseAll();
            return 3;
        }

        // OutpostServer.exe's order: the server role first, then the engine and the data.
        FDUMP = SimDump;
        CriticalWarningFn = SimCriticalWarning;
        CoInitialize(nullptr);
        _control87(kServerFpuWord, kServerFpuMask);
        GOracleSimHooks.bFixedClock = true;
        // IServerImpl::Startup's srand takes the scenario's seed instead of timeGetTime(): the
        // draws before the harness's own srand set the controllers' heartbeat phases, and
        // through them Content's game time (the reproducibility work, owner 2026-11-19).
        GOracleSimHooks.StartupSeed = GScenario.Seed;
        GOracleSimHooks.FrameSeconds = GScenario.Frame;
        GOracleSimHooks.ClockSeconds = GScenario.ClockStart;
        // The dispatch trace from the start-up on (Server_LoadWorldState's LoadGame included).
        GOracleSimHooks.OnControllerDeliver = OnControllerDeliver;
        SetIsMPServer(true);
        if (!NeuronRuntime::Startup(nullptr, "NEURONCOMPONENTSsrv.ini"))
        {
            SessionNote("error", "NeuronRuntime::Startup failed");
            CloseAll();
            return 3;
        }
        std::string DataPath;
        if (!OpenDataPath(IniPath, DataPath))
        {
            SessionNote("error", "no [OutpostFrontier] data path in " + IniPath);
            CloseAll();
            return 3;
        }

        SStartupInfo Info;
        memset(&Info, 0, sizeof(Info));
        strncpy_s(Info.ModulePath, sizeof(Info.ModulePath), StubPath.c_str(), _TRUNCATE);
        strncpy_s(Info.IniPath, sizeof(Info.IniPath), IniPath.c_str(), _TRUNCATE);
        Info.MaxPlayers = 16;
        Info.Port = 0;
        Info.bAllowPvp = 0;
        Info.bAllowNewPlayers = 1;
        bool const bStarted = ServerImpl->Startup(Info);
        // The server is up from here on, so every exit goes through EndProcess.
        if (!bStarted)
        {
            SessionNote("error", "IServerImpl::Startup failed (DataConfig::startup)");
            EndProcess(3);
            return 3;
        }

        // `dump content_tables` (P3.4b): the tables as the start-up left them, through
        // Content.dll's Oracle export, then the end of the run.
        if (GScenario.bDumpContentTables)
        {
            typedef int(__cdecl * FDumpContentTables)(char const*);
            HMODULE const Content = GetModuleHandleA("Content.dll");
            FDumpContentTables const DumpContentTables =
                Content != nullptr ? (FDumpContentTables)GetProcAddress(Content, "OracleDumpContentTables")
                                   : nullptr;
            int const Lines = DumpContentTables != nullptr ? DumpContentTables(OutDir.c_str()) : -1;
            if (DumpContentTables == nullptr)
                SessionNote("error", "dump content_tables: Content.dll is not loaded or lacks OracleDumpContentTables");
            FOracleJson J;
            J.Str("rec", "content_tables");
            J.Int("lines", Lines);
            Write(FileSession, J);
            EndProcess(Lines < 0 ? 3 : 0);
            return Lines < 0 ? 3 : 0;
        }

        // `dump mission_scripts` (P3.5a): the story missions' parse after the start-up, through
        // Content.dll's Oracle export, then the end of the run.
        if (GScenario.bDumpMissionScripts)
        {
            typedef int(__cdecl * FDumpMissionScripts)(char const*);
            HMODULE const Content = GetModuleHandleA("Content.dll");
            FDumpMissionScripts const DumpMissionScripts =
                Content != nullptr ? (FDumpMissionScripts)GetProcAddress(Content, "OracleDumpMissionScripts")
                                   : nullptr;
            int const Lines = DumpMissionScripts != nullptr ? DumpMissionScripts(OutDir.c_str()) : -1;
            if (DumpMissionScripts == nullptr)
                SessionNote("error", "dump mission_scripts: Content.dll is not loaded or lacks OracleDumpMissionScripts");
            FOracleJson J;
            J.Str("rec", "mission_scripts");
            J.Int("lines", Lines);
            Write(FileSession, J);
            EndProcess(Lines < 0 ? 3 : 0);
            return Lines < 0 ? 3 : 0;
        }

        // `dump mission_saves` and `dump mission_save_loads` (P3.5b): the story missions' and
        // Story's save sections, and their restores of the port's saves, after the start-up,
        // through Content.dll's Oracle exports, then the end of the run.
        if (GScenario.bDumpMissionSaves || GScenario.bDumpMissionSaveLoads)
        {
            typedef int(__cdecl * FMissionSaves)(char const*);
            char const* const Export =
                GScenario.bDumpMissionSaves ? "OracleDumpMissionSaves" : "OracleLoadMissionSaves";
            HMODULE const Content = GetModuleHandleA("Content.dll");
            FMissionSaves const Saves =
                Content != nullptr ? (FMissionSaves)GetProcAddress(Content, Export) : nullptr;
            int const Files = Saves != nullptr ? Saves(OutDir.c_str()) : -1;
            if (Saves == nullptr)
                SessionNote("error", (std::string("dump mission saves: Content.dll is not loaded or lacks ") +
                                      Export).c_str());
            else if (Files < 0)
                SessionNote("error", (std::string(Export) + " failed: a file could not be opened or a "
                                                            "mission or Story could not be made").c_str());
            FOracleJson J;
            J.Str("rec", GScenario.bDumpMissionSaves ? "mission_saves" : "mission_save_loads");
            J.Int("files", Files);
            Write(FileSession, J);
            EndProcess(Files < 0 ? 3 : 0);
            return Files < 0 ? 3 : 0;
        }

        // `dump random_missions` (P3.7a): the random-mission tables, the vignette tree and the
        // offers built from a seed after the start-up, through Content.dll's Oracle export, then
        // the end of the run.
        if (GScenario.bDumpRandomMissions)
        {
            typedef int(__cdecl * FDumpRandomMissions)(char const*);
            HMODULE const Content = GetModuleHandleA("Content.dll");
            FDumpRandomMissions const DumpRandomMissions =
                Content != nullptr ? (FDumpRandomMissions)GetProcAddress(Content, "OracleDumpRandomMissions")
                                   : nullptr;
            int const Lines = DumpRandomMissions != nullptr ? DumpRandomMissions(OutDir.c_str()) : -1;
            if (DumpRandomMissions == nullptr)
                SessionNote("error", "dump random_missions: Content.dll is not loaded or lacks OracleDumpRandomMissions");
            else if (Lines < 0)
                SessionNote("error", "OracleDumpRandomMissions failed: a file could not be opened or the "
                                     "generator or a random-mission loader is missing");
            FOracleJson J;
            J.Str("rec", "random_missions");
            J.Int("lines", Lines);
            Write(FileSession, J);
            EndProcess(Lines < 0 ? 3 : 0);
            return Lines < 0 ? 3 : 0;
        }

        // `dump random_mission_saves` and `dump random_mission_save_loads` (P3.7c): the random
        // missions' [RandomMission] save sections and their restores, the port's saves' among
        // them, after the start-up, through Content.dll's Oracle exports, then the end of the run.
        if (GScenario.bDumpRandomMissionSaves || GScenario.bDumpRandomMissionSaveLoads)
        {
            typedef int(__cdecl * FRandomMissionSaves)(char const*);
            char const* const Export = GScenario.bDumpRandomMissionSaves ? "OracleDumpRandomMissionSaves"
                                                                         : "OracleLoadRandomMissionSaves";
            HMODULE const Content = GetModuleHandleA("Content.dll");
            FRandomMissionSaves const Saves =
                Content != nullptr ? (FRandomMissionSaves)GetProcAddress(Content, Export) : nullptr;
            int const Lines = Saves != nullptr ? Saves(OutDir.c_str()) : -1;
            if (Saves == nullptr)
                SessionNote("error", (std::string("dump random mission saves: Content.dll is not loaded or lacks ") +
                                      Export).c_str());
            else if (Lines < 0)
                SessionNote("error", (std::string(Export) + " failed: a file could not be opened").c_str());
            FOracleJson J;
            J.Str("rec", GScenario.bDumpRandomMissionSaves ? "random_mission_saves" : "random_mission_save_loads");
            J.Int("lines", Lines);
            Write(FileSession, J);
            EndProcess(Lines < 0 ? 3 : 0);
            return Lines < 0 ? 3 : 0;
        }

        // `comms off`: the GCS clamp through Content.dll's Oracle export, before the load.
        bool bCommsClamped = false;
        if (GScenario.bCommsOff)
        {
            typedef void(__cdecl * FSetGcsClamp)(bool);
            HMODULE const Content = GetModuleHandleA("Content.dll");
            FSetGcsClamp const SetGcsClamp =
                Content != nullptr ? (FSetGcsClamp)GetProcAddress(Content, "OracleSimSetGcsClamp") : nullptr;
            if (SetGcsClamp != nullptr)
            {
                SetGcsClamp(true);
                bCommsClamped = true;
            }
            else
                SessionNote("error", "comms off: Content.dll lacks OracleSimSetGcsClamp");
        }

        // The load-time rand() recorder (load_rand.jsonl): every draw from the seeding to the
        // first frame, with its caller.
        if (!OracleSimLoadRandBegin(OutDir, GScenario.Seed))
            SessionNote("note", "load_rand.jsonl cannot be written; load-time draws are not recorded");

        // rand() is per thread in the CRT; everything runs on this thread.
        srand(GScenario.Seed);

        OracleSimLoadRandPhase("controller");
        unsigned int const ControllerId =
            pub::Controller::Create(StubPath.c_str(), "OracleSim", nullptr, pub::Controller::LOWEST);
        unsigned int const SystemId = CreateID(GScenario.System.c_str());
        OracleSimLoadRandPhase("load");
        bool const bLoaded = OracleSimLoadSystem(SystemId);
        OracleSimLoadRandPhase("after_load");
        {
            FOracleJson J;
            J.Str("rec", "server");
            J.Str("data_path", DataPath.c_str());
            J.Uint("controller", ControllerId);
            J.Uint("system_id", SystemId);
            J.Bool("system_loaded", bLoaded);
            J.Uint("reseed", Reseed);
            J.Uint("startup_seed", GOracleSimHooks.StartupSeed);
            if (GScenario.bCommsOff)
                J.Bool("comms_clamped", bCommsClamped);
            Write(FileSession, J);
        }
        if (ControllerId == 0 || !bLoaded)
        {
            SessionNote("error", ControllerId == 0 ? "no controller" : "the system did not load");
            EndProcess(3);
            return 3;
        }

        if (GScenario.bDumpSystemLoads)
        {
            int const Lines = OracleDumpSystemLoads((OutDir + "\\system_loads.jsonl").c_str());
            FOracleJson J;
            J.Str("rec", "system_loads");
            J.Int("lines", Lines);
            Write(FileSession, J);
            // `dump content_systems` (P3.4d): every system has loaded, so SpaceSupport's
            // SystemLoaded has run each record's full load.
            int ContentLines = 0;
            if (GScenario.bDumpContentSystems)
            {
                typedef int(__cdecl * FDumpContentSystems)(char const*);
                HMODULE const Content = GetModuleHandleA("Content.dll");
                FDumpContentSystems const DumpContentSystems =
                    Content != nullptr
                        ? (FDumpContentSystems)GetProcAddress(Content, "OracleDumpContentSystems")
                        : nullptr;
                ContentLines = DumpContentSystems != nullptr ? DumpContentSystems(OutDir.c_str()) : -1;
                if (DumpContentSystems == nullptr)
                    SessionNote("error", "dump content_systems: Content.dll is not loaded or lacks OracleDumpContentSystems");
                FOracleJson C;
                C.Str("rec", "content_systems");
                C.Int("lines", ContentLines);
                Write(FileSession, C);
            }
            int const Code = Lines < 0 || ContentLines < 0 ? 3 : 0;
            EndProcess(Code);
            return Code;
        }

        // `dump mission_trace` (P3.5c1): the scenario's system has loaded, so the mission's spawns
        // have a system; Mission_01a's opening runs through Content.dll's Oracle export, then the
        // end of the run.
        if (GScenario.bDumpMissionTrace)
        {
            typedef int(__cdecl * FDumpMissionTrace)(char const*);
            HMODULE const Content = GetModuleHandleA("Content.dll");
            FDumpMissionTrace const DumpMissionTrace =
                Content != nullptr ? (FDumpMissionTrace)GetProcAddress(Content, "OracleDumpMissionTrace")
                                   : nullptr;
            int const Records = DumpMissionTrace != nullptr ? DumpMissionTrace(OutDir.c_str()) : -1;
            if (DumpMissionTrace == nullptr)
                SessionNote("error", "dump mission_trace: Content.dll is not loaded or lacks OracleDumpMissionTrace");
            else if (Records < 0)
                SessionNote("error", "OracleDumpMissionTrace failed: mission.jsonl could not be opened or "
                                     "Mission_01a could not be made");
            FOracleJson J;
            J.Str("rec", "mission_trace");
            J.Int("records", Records);
            Write(FileSession, J);
            EndProcess(Records < 0 ? 3 : 0);
            return Records < 0 ? 3 : 0;
        }

        // The seeding point of the comparison (owner, 2026-11-18): the system and Content's
        // population of it (SystemLoaded) have drawn their values; the scenario starts from
        // srand(seed) again, where the Rust world, which has no population yet (P3.6), seeds.
        // With --seed the seed is the ensemble's (owner, 2026-11-19).
        OracleSimLoadRandReseed(Reseed);
        srand(Reseed);

        bool bSpawned = true;
        for (FObjectDesc const& Desc : GScenario.Objects)
        {
            OracleSimLoadRandPhase("spawn " + Desc.Name);
            bSpawned = SpawnObject(Desc, SystemId, ControllerId) && bSpawned;
        }
        // P3.3: the replication record from here on, then the players' join (tick 0).
        if (bSpawned && GScenario.bDumpReplication)
            RecordReplication(OutDir.c_str());
        if (bSpawned && !GScenario.Players.empty())
        {
            OracleSimLoadRandPhase("players");
            if (!AddPlayers(SystemId))
            {
                SessionNote("error", "a player could not join the system");
                bSpawned = false;
            }
        }
        // P3.6d: the populated scenario's player and SpacePop, after the players' join.
        if (bSpawned && !GScenario.PopulationShip.empty())
        {
            OracleSimLoadRandPhase("population");
            if (!Populate(SystemId))
            {
                SessionNote("error", "the population could not be started");
                bSpawned = false;
            }
        }
        OracleSimLoadRandPhase("hooks");
        if (!bSpawned)
        {
            SessionNote("error", "an object could not be spawned or found");
            EndProcess(3);
            return 3;
        }

        GOracleSimHooks.StepSystemId = SystemId;
        GOracleSimHooks.bSinglePlayerStepping = GScenario.bSpStepping;
        GOracleSimHooks.bSinglePlayerCollisions = GScenario.bSpCollisions;
        GOracleSimHooks.OnPsi = OnPsi;
        GOracleSimHooks.OnSimUnitPsi = OnSimUnitPsi;
        GUnitFile = fopen((OutDir + "\\units.jsonl").c_str(), "wb");
        GUnitCoreFile = fopen((OutDir + "\\unit_cores.jsonl").c_str(), "wb");
        GOracleSimHooks.OnCorePush = OnCorePush;
        GOracleSimHooks.OnCollisionDamage = OnCollisionDamage;
        GOracleSimHooks.OnMunitionHit = OnMunitionHit;
        GOracleSimHooks.OnBehaviorTick = OnBehaviorTick;
        GOracleSimHooks.OnAffect = OnAffect;
        // P2.5: the behaviour-tick recorder (ai.jsonl, oracle_sim_ai.cpp) chains itself in
        // front of OnBehaviorTick, so it is started after the hooks above.
        if (!OracleSimAiStart(OutDir, &GTick, &NameOf))
            SessionNote("note", "ai.jsonl cannot be written; behaviour ticks are not recorded");
        // P2.4: the asteroid fields and the docking points (asteroids.jsonl, dock.jsonl).
        if (!OracleSimFieldsStart(OutDir, SystemId, &GTick, &NameOf, &ShipPositions))
            SessionNote("note", "asteroids.jsonl or dock.jsonl cannot be written; fields and docking points are not recorded");
        SetSink(OnControllerMessage);
        OracleSimLoadRandEnd();
        // The frame-time rand() recorder (run_rand.jsonl), only with OUTPOST_ORACLE_RUN_RAND set
        // (the cross-runner reproducibility work, owner 2026-11-19).
        {
            char Flag[8] = {};
            if (GetEnvironmentVariableA("OUTPOST_ORACLE_RUN_RAND", Flag, sizeof(Flag)) > 0)
                OracleSimRunRandBegin(OutDir, &GTick);
        }

        // Every controller's heartbeat interval and phase as frame 1 finds them.
        {
            FOracleJson J;
            J.Str("rec", "heartbeats");
            J.Uint("tick", 0);
            J.BeginArray("controllers");
            OracleSimControllerHeartbeats(&WriteHeartbeat, &J);
            J.EndArray();
            Write(FileSession, J);
        }

        // Tick 0: the state after spawning, before any frame.
        unsigned int X87 = 0;
        unsigned int Sse = 0;
        ControlWords(X87, Sse);
        RecordTick(X87, Sse);
        FlushAll();

        // The fault recorder: a structured exception in a frame is written to session.jsonl
        // (`fault`) and ends the run with exit code 4.
        OracleSimFaultStart(WriteSessionLine, &GTick);

        size_t NextCommand = 0;
        for (GTick = 1; GTick <= GScenario.Ticks; ++GTick)
        {
            // The precision IVP's point-soup builder may have left (decision T11), then the
            // server thread's word again.
            ControlWords(X87, Sse);
            _control87(kServerFpuWord, kServerFpuMask);
            GOracleSimHooks.ClockSeconds = GScenario.ClockStart + GTick * GScenario.Frame;
            GStubSetTick(GTick);
            FFrameArgs Args;
            Args.Server = ServerImpl;
            Args.NextCommand = &NextCommand;
            Args.X87 = X87;
            Args.Sse = Sse;
            if (!OracleSimGuardedCall(SimFrame, &Args))
            {
                FlushAll();
                SessionNote("error", "a structured exception in frame " + std::to_string(GTick) +
                                         " (the fault record)");
                EndProcess(4);
                return 4;
            }
            FlushAll();
        }
        GTick = GScenario.Ticks;
        EndProcess(0);
        return 0;
    }
}

int RunSimMode(int Argc, char** Argv)
{
    // The options after the paths, each once, in any order: --runs N (1 to 99), --seed S
    // (decimal or 0x hex, 32 bits).
    oracle::FSimOptions Options;
    bool bUsage = Argc < 5 || (Argc - 5) % 2 != 0;
    bool bRuns = false;
    for (int Index = 5; !bUsage && Index + 1 < Argc; Index += 2)
    {
        unsigned long Value = 0;
        bool const bParsed = ParseUnsigned(Argv[Index + 1], Value);
        if (strcmp(Argv[Index], "--runs") == 0 && !bRuns)
        {
            bUsage = !bParsed || Value == 0 || Value > 99;
            Options.Runs = (int)Value;
            bRuns = true;
        }
        else if (strcmp(Argv[Index], "--seed") == 0 && !Options.bReseed)
        {
            bUsage = !bParsed;
            Options.bReseed = true;
            Options.Reseed = (unsigned int)Value;
        }
        else
        {
            bUsage = true;
        }
    }
    if (bUsage)
    {
        fprintf(stderr, "usage: Oracle.exe sim <data root> <scenario file> <out folder> [--runs N] [--seed S]\n");
        return 2;
    }
    std::string Paths[3];
    for (int Index = 0; Index < 3; ++Index)
    {
        char Full[MAX_PATH] = {};
        if (GetFullPathNameA(Argv[2 + Index], MAX_PATH, Full, nullptr) == 0)
        {
            fprintf(stderr, "sim: bad path %s\n", Argv[2 + Index]);
            return 2;
        }
        Paths[Index] = Full;
        while (Paths[Index].size() > 3 && (Paths[Index].back() == '\\' || Paths[Index].back() == '/'))
            Paths[Index].pop_back();
    }
    if (Options.Runs > 0)
        return oracle::RunSimSpread(Paths[0], Paths[1], Paths[2], Options);
    return oracle::RunSimScenario(Paths[0], Paths[1], Paths[2], Options);
}
