// Oracle instrumentation (design/migration-plan.md, Phase 0; work package P3.7b, decision record
// 0008 points 3 and 10): the Tier B router and machine trace of a vendor random mission, for the
// Rust comparison (crates/game-logic/tests/rm_session.rs). A new export, not part of the reference
// DLL's interface: only Oracle.exe sim calls it, for the session file CppSource/Oracle/rm_session.txt
// (`dump rm_session`), right after the multiplayer start-up, so a game run never reaches it.
//
// The synthetic player 7 is registered with ReplaceContentPlayerState in he01_01_Base's system,
// with no ship and no server slot, and the base's rooms are loaded in Server.dll's base database,
// as OracleDumpBaseSession does. The export makes a BaseSupport of its own (the generic scripts
// emptied first) and runs these steps, each from std::srand(OracleRmSeed + step):
//   0 base_enter      BaseEnter {base, 7} (GameSupport's case, then the BaseSupport);
//   1 vendor_offers   RefreshMissionVendorOffers;
//   2 accept_vendor   MissionResponse {the first vendor offer's id, 2, accepted, 7}: BaseSupport
//                     creates the DestroyMission controller on the bus (CreateDestroyMission notes
//                     it, OracleRmNoteMission);
//   3 join            the mission's FRandomMissionObject::OracleJoinPlayer(7): player 7 mapped,
//                     listed and named as SetPlayer does for each member of a group, which a player
//                     without a server slot does not have;
//   4.. tick          OracleRmTicks times: the game time + 1 s (OracleAddContentGameTime, as
//                     GameSupport's heartbeat adds it), Heartbeat {1.0} to the mission through the
//                     bus (_SendMessage), then the bus's timers fired with 1 s (Server.dll's
//                     OracleBusFireTimers), which hands the router's timers to SpaceSupport;
//   abort             AbortMission {7, mission} to the mission through the bus;
//   teardown          Server.dll's OracleBusTick(0): the queued controllers torn down, the
//                     mission's destructor among them.
// At the end the hooks are off, the BaseSupport destroyed, the scripts read again and player 7
// removed.
//
// OracleDumpRmSession(Dir) writes <Dir>\rm.jsonl, one JSON object per line, ids unsigned decimal,
// a float as its bits in lower-case hexadecimal, in the format of
// game_logic::content::random_mission::runtime::trace and the Rust test's records:
//   {"rec":"step","step":N,"name":"X"}
//   {"rec":"message","step":N,"msg":M}
//   {"rec":"register"|"unregister","step":N,"handler":H}
//   {"rec":"route","step":N,"type":"T","sender":A,"receiver":B,"state":D,"time":"%08x"}
//   {"rec":"deliver","step":N,"type":"T","sender":A,"receiver":B,"found":0|1}
//   {"rec":"sm","step":N,"machine":H,"kind":"K","event":1|2,"state":S}
//   {"rec":"mission","step":N,"event":"E","value":V}
//   {"rec":"end","step":N,"draws":D,"flags":F,"live":L,"completed":[[t,c],..],"aborted":[..],
//    "failed":[..]}
// and returns the number of records, -1 when the file cannot be opened, the base is unknown, no
// mission was made or Server.dll lacks its exports.

#include "content.h"
#include "BaseDB.h"             // FindContentBaseInfo, FContentBase
#include "BaseSupport.h"        // GBaseVisits, RefreshMissionVendorOffers, FMissionOfferAcceptMsg
#include "ContentInternal.h"    // OracleAddContentGameTime
#include "ContentPlayerState.h" // ReplaceContentPlayerState, RemoveContentPlayerState
#include "GenericScripts.h"     // LoadGenericScripts, UnloadGenericScripts
#include "RandomMissionObject.h" // FDestroyMission, FRandomMissionObject::OracleJoinPlayer
#include "RandomMissionRuntime.h" // the message types
#include "SystemInfo.h"         // FindSystemInfo
#include "common_misc.h"        // SMM_TIMEOUT, SMM_CHANGE_STATE
#include "common_net.h"
#include "common_server.h"
#include "common_server_controller.h"
#include "oracle_rm_trace.h"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace
{
    using pub::Controller::EMessage;

    unsigned int const OracleRmSeed = 0x4f524143u;
    unsigned int const OracleRmPlayerId = 7u;
    unsigned int const OracleRmSupportController = 0x00ff0003u;
    int const OracleRmTicks = 12;
    char const* const OracleRmBaseName = "he01_01_Base";

    typedef void(__cdecl* FOracleBaseDbLoadBase)(unsigned int BaseId);
    typedef void(__cdecl* FOracleBusTick)(float Dt);

    struct FOracleRmBasePlayer
    {
        unsigned int Id;
        unsigned int PlayerId;
    };

    struct FOracleRmSession
    {
        FILE* File = nullptr;
        unsigned int Step = 0;
        int Records = 0;
        IDLL* Support = nullptr;
    };

    FOracleRmSession GOracleRm;
    FDestroyMission* GOracleRmMission = nullptr;
    unsigned int GOracleRmMissionId = 0;

    // A message type's trace name: the C++ global's without its GRmMsg prefix.
    char const* OracleRmTypeName(unsigned int Type, char* Buffer)
    {
        struct FName
        {
            unsigned int const* Type;
            char const* Name;
        };
        FName const Names[] = {
            {&GRmMsgStartMachine, "StartMachine"},
            {&GRmMsgMachineStarted, "MachineStarted"},
            {&GRmMsgMachineSucceeded, "MachineSucceeded"},
            {&GRmMsgMachineFailed, "MachineFailed"},
            {&GRmMsgRefreshObjectives, "RefreshObjectives"},
            {&GRmMsgType06fd3ba8, "Type06fd3ba8"},
            {&GRmMsgType06fd3ba4, "Type06fd3ba4"},
            {&GRmMsgType06fd3ba0, "Type06fd3ba0"},
            {&GRmMsgType06fd3b9c, "Type06fd3b9c"},
            {&GRmMsgType06fd3b98, "Type06fd3b98"},
            {&GRmMsgType06fd3b94, "Type06fd3b94"},
            {&GRmMsgType06fd3b90, "Type06fd3b90"},
            {&GRmMsgType06fd3b8c, "Type06fd3b8c"},
            {&GRmMsgType06fd3b88, "Type06fd3b88"},
            {&GRmMsgType06fd3b84, "Type06fd3b84"},
            {&GRmMsgType06fd3b80, "Type06fd3b80"},
            {&GRmMsgType06fd3b7c, "Type06fd3b7c"},
            {&GRmMsgType06fd3b78, "Type06fd3b78"},
            {&GRmMsgType06fd3b74, "Type06fd3b74"},
            {&GRmMsgPlayerLeft, "PlayerLeft"},
            {&GRmMsgType06fd3b6c, "Type06fd3b6c"},
            {&GRmMsgTriggerFired, "TriggerFired"},
        };
        if (Type == SMM_TIMEOUT)
            return "Timeout";
        if (Type == SMM_CHANGE_STATE)
            return "ChangeState";
        for (size_t Index = 0; Index < sizeof(Names) / sizeof(Names[0]); ++Index)
        {
            if (*Names[Index].Type == Type)
                return Names[Index].Name;
        }
        std::sprintf(Buffer, "?%u", Type);
        return Buffer;
    }

    unsigned int OracleRmBits(float Value)
    {
        unsigned int Bits = 0;
        std::memcpy(&Bits, &Value, sizeof(Bits));
        return Bits;
    }

    // The draws since std::srand(Seed), as OracleDumpBaseSession counts them.
    int OracleRmCountDraws(unsigned int Seed)
    {
        int Probe[4];
        for (int Index = 0; Index < 4; ++Index)
            Probe[Index] = std::rand();
        std::srand(Seed);
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

    void OracleRmBegin(unsigned int Step, char const* Name)
    {
        GOracleRm.Step = Step;
        std::fprintf(GOracleRm.File, "{\"rec\":\"step\",\"step\":%u,\"name\":\"%s\"}\n", Step, Name);
        ++GOracleRm.Records;
        std::srand(OracleRmSeed + Step);
    }

    void OracleRmMessage(EMessage Message)
    {
        std::fprintf(GOracleRm.File, "{\"rec\":\"message\",\"step\":%u,\"msg\":%u}\n", GOracleRm.Step,
                     static_cast<unsigned int>(Message));
        ++GOracleRm.Records;
    }

    void OracleRmCounts(FILE* Out, char const* Name, std::map<unsigned char, int> const& Counts)
    {
        std::fprintf(Out, ",\"%s\":[", Name);
        bool bFirst = true;
        for (std::map<unsigned char, int>::const_iterator It = Counts.begin(); It != Counts.end();
             ++It, bFirst = false)
            std::fprintf(Out, "%s[%u,%d]", bFirst ? "" : ",", static_cast<unsigned int>(It->first), It->second);
        std::fprintf(Out, "]");
    }

    // The step's `end` record.
    void OracleRmEnd()
    {
        FILE* const Out = GOracleRm.File;
        int const Draws = OracleRmCountDraws(OracleRmSeed + GOracleRm.Step);
        FContentPlayerState const* const State = FindContentPlayerState(OracleRmPlayerId);
        std::fprintf(Out, "{\"rec\":\"end\",\"step\":%u,\"draws\":%d,\"flags\":%u,\"live\":%u", GOracleRm.Step,
                     Draws, State != nullptr ? State->ValueD0 : 0u, State != nullptr ? State->ValueD8 : 0u);
        std::map<unsigned char, int> const Empty;
        OracleRmCounts(Out, "completed", State != nullptr ? State->Stats.RandomMissionsCompleted : Empty);
        OracleRmCounts(Out, "aborted", State != nullptr ? State->Stats.RandomMissionsAborted : Empty);
        OracleRmCounts(Out, "failed", State != nullptr ? State->Stats.RandomMissionsFailed : Empty);
        std::fprintf(Out, "}\n");
        ++GOracleRm.Records;
    }

    // GameSupport's case for a base message, as OracleDumpBaseSession runs it, then the
    // BaseSupport's.
    void OracleRmBaseSend(EMessage Message, void const* Parms)
    {
        OracleRmMessage(Message);
        FOracleRmBasePlayer const* const Msg = static_cast<FOracleRmBasePlayer const*>(Parms);
        if (Message == EMessage::BaseEnter)
        {
            FindContentPlayerState(Msg->PlayerId)->OnBaseChange(Msg->Id);
            FContentBase* const Base = FindContentBaseInfo(&Msg->Id);
            if (Base != nullptr)
            {
                Base->Load(nullptr);
                Base->OnPlayerEnter(Msg->PlayerId);
            }
        }
        GOracleRm.Support->HandleMessage(Message, Parms);
    }
}

void OracleRmTraceHandler(char const* Op, unsigned int HandlerId)
{
    if (GOracleRm.File == nullptr)
        return;
    std::fprintf(GOracleRm.File, "{\"rec\":\"%s\",\"step\":%u,\"handler\":%u}\n", Op, GOracleRm.Step, HandlerId);
    ++GOracleRm.Records;
}

void OracleRmTraceRoute(StateMachineMessage const* Message)
{
    if (GOracleRm.File == nullptr)
        return;
    char Buffer[16];
    std::fprintf(GOracleRm.File,
                 "{\"rec\":\"route\",\"step\":%u,\"type\":\"%s\",\"sender\":%u,\"receiver\":%u,\"state\":%d,\"time\":\"%08x\"}\n",
                 GOracleRm.Step, OracleRmTypeName(Message->MessageType, Buffer), Message->Sender, Message->Receiver,
                 Message->DeliveryState, OracleRmBits(Message->DeliveryTime));
    ++GOracleRm.Records;
}

void OracleRmTraceDeliver(StateMachineMessage const* Message, bool bFound)
{
    if (GOracleRm.File == nullptr)
        return;
    char Buffer[16];
    std::fprintf(GOracleRm.File,
                 "{\"rec\":\"deliver\",\"step\":%u,\"type\":\"%s\",\"sender\":%u,\"receiver\":%u,\"found\":%u}\n",
                 GOracleRm.Step, OracleRmTypeName(Message->MessageType, Buffer), Message->Sender, Message->Receiver,
                 bFound ? 1u : 0u);
    ++GOracleRm.Records;
}

void OracleRmTraceSm(unsigned int MachineId, char const* Kind, SMEventData const* Event, int State)
{
    if (GOracleRm.File == nullptr || State < 0)
        return;
    int const Type = static_cast<int>(Event->get_event_type());
    if (Type != 1 && Type != 2)
        return;
    std::fprintf(GOracleRm.File, "{\"rec\":\"sm\",\"step\":%u,\"machine\":%u,\"kind\":\"%s\",\"event\":%d,\"state\":%d}\n",
                 GOracleRm.Step, MachineId, Kind, Type, State);
    ++GOracleRm.Records;
}

void OracleRmTraceMission(char const* Event, long long Value)
{
    if (GOracleRm.File == nullptr)
        return;
    std::fprintf(GOracleRm.File, "{\"rec\":\"mission\",\"step\":%u,\"event\":\"%s\",\"value\":%lld}\n", GOracleRm.Step,
                 Event, Value);
    ++GOracleRm.Records;
}

void OracleRmNoteMission(FDestroyMission* Mission, unsigned int ControllerId)
{
    GOracleRmMission = Mission;
    GOracleRmMissionId = ControllerId;
}

extern "C" __declspec(dllexport) int __cdecl OracleDumpRmSession(char const* Dir)
{
    if (Dir == nullptr)
        return -1;
    HMODULE const Server = GetModuleHandleA("server.dll");
    FOracleBaseDbLoadBase const LoadBase =
        Server != nullptr ? (FOracleBaseDbLoadBase)GetProcAddress(Server, "OracleBaseDbLoadBase") : nullptr;
    FOracleBusTick const FireTimers =
        Server != nullptr ? (FOracleBusTick)GetProcAddress(Server, "OracleBusFireTimers") : nullptr;
    FOracleBusTick const BusTick = Server != nullptr ? (FOracleBusTick)GetProcAddress(Server, "OracleBusTick") : nullptr;
    if (LoadBase == nullptr || FireTimers == nullptr || BusTick == nullptr)
        return -1;
    unsigned int BaseId = 0;
    pub::GetBaseID(BaseId, OracleRmBaseName);
    FContentBase* const Base = FindContentBaseInfo(&BaseId);
    if (Base == nullptr)
        return -1;
    std::string const Path = std::string(Dir) + "\\rm.jsonl";
    FILE* const File = std::fopen(Path.c_str(), "wb");
    if (File == nullptr)
        return -1;

    // Player 7 in the base's system, no ship.
    FContentPlayerState* const State = ReplaceContentPlayerState(OracleRmPlayerId);
    unsigned int SystemId = 0;
    if (pub::GetSystem(SystemId, BaseId) == 0)
        State->SystemRecord = FindSystemInfo(&SystemId);

    UnloadGenericScripts();
    IDLL* const Support = CreateInstance(OracleRmSupportController, "BaseSupport", nullptr);
    GOracleRm = FOracleRmSession();
    GOracleRm.Support = Support;
    GOracleRmMission = nullptr;
    GOracleRmMissionId = 0;
    if (Support == nullptr)
    {
        std::fclose(File);
        LoadGenericScripts("scripts\\gcs\\genericScripts.ini");
        RemoveContentPlayerState(OracleRmPlayerId);
        return -1;
    }
    LoadBase(BaseId);
    // The hooks record from here on.
    GOracleRm.File = File;

    unsigned int Step = 0;
    FOracleRmBasePlayer Parms = {BaseId, OracleRmPlayerId};
    OracleRmBegin(Step, "base_enter");
    OracleRmBaseSend(EMessage::BaseEnter, &Parms);
    OracleRmEnd();

    OracleRmBegin(++Step, "vendor_offers");
    RefreshMissionVendorOffers(State);
    OracleRmEnd();

    FMissionOfferAcceptMsg Accept;
    std::memset(&Accept, 0, sizeof(Accept));
    std::map<unsigned int, FBaseVisit>::const_iterator Visit = GBaseVisits.find(OracleRmPlayerId);
    Accept.OfferId =
        Visit != GBaseVisits.end() && !Visit->second.VendorOffers.empty() ? Visit->second.VendorOffers.front().OfferId : 0u;
    Accept.Kind = 2;
    Accept.bAccepted = 1;
    Accept.PlayerId = OracleRmPlayerId;
    OracleRmBegin(++Step, "accept_vendor");
    OracleRmBaseSend(EMessage::MissionResponse, &Accept);
    OracleRmEnd();

    int Records = -1;
    if (GOracleRmMission != nullptr)
    {
        OracleRmBegin(++Step, "join");
        GOracleRmMission->OracleJoinPlayer(OracleRmPlayerId);
        OracleRmEnd();

        unsigned int const Mission = GOracleRmMissionId;
        for (int Tick = 0; Tick < OracleRmTicks; ++Tick)
        {
            OracleRmBegin(++Step, "tick");
            OracleAddContentGameTime(1.0);
            float const Dt = 1.0f;
            OracleRmMessage(EMessage::Heartbeat);
            pub::Controller::_SendMessage(Mission, EMessage::Heartbeat, &Dt);
            FireTimers(1.0f);
            OracleRmEnd();
        }

        OracleRmBegin(++Step, "abort");
        unsigned int const Abort[2] = {OracleRmPlayerId, Mission};
        OracleRmMessage(EMessage::AbortMission);
        pub::Controller::_SendMessage(Mission, EMessage::AbortMission, Abort);
        OracleRmEnd();

        OracleRmBegin(++Step, "teardown");
        BusTick(0.0f);
        OracleRmEnd();
        Records = GOracleRm.Records;
    }

    GOracleRm.File = nullptr;
    GOracleRm = FOracleRmSession();
    GOracleRmMission = nullptr;
    GOracleRmMissionId = 0;
    std::fclose(File);
    DestroyInstance(Support);
    LoadGenericScripts("scripts\\gcs\\genericScripts.ini");
    RemoveContentPlayerState(OracleRmPlayerId);
    return Records;
}
