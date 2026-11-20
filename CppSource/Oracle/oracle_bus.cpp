// Oracle suite of work package P3.1 (Oracle.exe neuron-server): the controller registry of
// Server.dll (Server/IServerImpl.cpp, the controller lists, guard, destroy queue, heartbeats
// and timers) driven by scripted controllers, decision record 0006 point 10. The Rust test
// crates/neuron-server/tests/controller_bus.rs runs the same scripts on
// neuron-server::controller and renders the same lines.
//
// The registry runs without a started server: pub::Controller::Create, Destroy,
// SetHeartbeatInterval, _SendMessage and pub::SetTimer are Server.dll's exports, and the two
// oracle exports OracleBusTick (the heartbeat walk, then timer firing, as ElapseTime runs them)
// and OracleBusShutdown (Server_ControllerCleanup) drive the rest. The controllers are
// SimClientStub.dll's scripted instances ("Scripted:<name>"), whose constructor, messages and
// destructor come back to Script() below. Controller ids come from Common's CFLIDMaker, which
// nothing else in this mode uses, so they count from 1.
//
// Input: crates/neuron-server/tests/fixtures/oracle/controller_bus.list, one directive per
// line, fields separated by spaces, '#' lines are comments:
//   case <name> [seed]                     -- a new case: srand(seed, default 1), rules and
//                                             names cleared; the registry carries over
//   on <inst> <event> <filter> <limit> <action...>
//        a rule of controller <inst>: event ctor | dtor | heartbeat | timer | destroyed | <msg>,
//        filter * or the tag (a test message), p1 (timer) or name (destroyed); limit 0 for
//        always, else the number of times it may fire. Matching rules fire in file order.
//   actions, in rules and at the top level:
//        send <ref> <msg> <tag> | broadcast <msg> <tag> | create <name> <priority>
//        | destroy <ref> | heartbeat <ref> <interval> | timer <ref> <p1> <p2> <timeout>
//        | fail (ctor rules: CreateInstance returns null)
//   top level only: tick <dt> | shutdown | state
//   ref: a name, self (rules), ghost (an id never given out) or all (0, the broadcast id).
// Test messages carry a pointer to their 32-bit tag. Floats are parsed with strtod and
// rounded to float.
//
// Output: controller_bus.jsonl, one record per line, every record {"case","seq","depth",...}:
//   delivery  "to","flid","msg" and "tag" | "elapsed" (hex bits) | "p1","p2" | "id"
//   ctor/dtor "ctor"|"dtor" (name), "flid"
//   op        "op" and its operands, before it runs; "ret" with the result after create
//             ("flid"), send, heartbeat and timer ("r")
//   state     "state": [{"name","flid","prio","interval","phase","deferred"}, ...] in list
//             order (OracleSimControllerHeartbeats)

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "oracle_support.h"

#include "common_server_controller.h"
#include "SimClientStub/sim_client_stub.h"

using namespace oracle;

typedef void(__cdecl* FOracleBusHeartbeatFn)(void* Context, unsigned int Flid, unsigned int Priority,
                                              char const* Module, float Interval, float Phase,
                                              bool bDeferred);
extern "C" __declspec(dllimport) void __cdecl OracleSimControllerHeartbeats(FOracleBusHeartbeatFn Fn,
                                                                            void* Context);
extern "C" __declspec(dllimport) void __cdecl OracleBusTick(float Dt);
extern "C" __declspec(dllimport) void __cdecl OracleBusShutdown();
extern "C" __declspec(dllimport) unsigned int __cdecl OracleAccountCrc(unsigned char const* Bytes,
                                                                       unsigned int ByteCount);
extern "C" __declspec(dllimport) void __cdecl OracleAccountDirName(char* Out, wchar_t const* AccountName);

namespace
{
    unsigned int const kGhostFlid = 0x3ffffff0u;
    int const kHeartbeat = static_cast<int>(pub::Controller::EMessage::Heartbeat);
    int const kTimerExpired = static_cast<int>(pub::Controller::EMessage::TimerExpired);
    int const kControllerDestroyed = static_cast<int>(pub::Controller::EMessage::ControllerDestroyed);

    struct FRule
    {
        std::string Inst;
        std::string Event;
        std::string Filter;
        unsigned int Limit = 0;
        unsigned int Fired = 0;
        std::vector<std::string> Action;
    };

    Suite* GOut = nullptr;
    std::string GStubPath;
    std::string GCase;
    unsigned int GSeq = 0;
    unsigned int GDepth = 0;
    std::vector<FRule> GRules;
    std::map<std::string, unsigned int> GFlids;
    std::map<unsigned int, std::string> GNames;

    std::vector<std::string> Tokens(std::string const& Line)
    {
        std::vector<std::string> Out;
        size_t Index = 0;
        while (Index < Line.size())
        {
            while (Index < Line.size() && (Line[Index] == ' ' || Line[Index] == '\t'))
                ++Index;
            size_t const Start = Index;
            while (Index < Line.size() && Line[Index] != ' ' && Line[Index] != '\t')
                ++Index;
            if (Index > Start)
                Out.push_back(Line.substr(Start, Index - Start));
        }
        return Out;
    }

    unsigned int ParseUint(std::string const& Text)
    {
        return static_cast<unsigned int>(strtoul(Text.c_str(), nullptr, 0));
    }

    float ParseFloat(std::string const& Text)
    {
        return static_cast<float>(strtod(Text.c_str(), nullptr));
    }

    std::string Head()
    {
        return Format("{\"case\":\"%s\",\"seq\":%u,\"depth\":%u", GCase.c_str(), ++GSeq, GDepth);
    }

    std::string NameOf(unsigned int Flid)
    {
        std::map<unsigned int, std::string>::const_iterator const It = GNames.find(Flid);
        return It == GNames.end() ? std::string("?") : It->second;
    }

    unsigned int Resolve(std::string const& Ref, unsigned int Self)
    {
        if (Ref == "self")
            return Self;
        if (Ref == "all")
            return 0;
        if (Ref == "ghost")
            return kGhostFlid;
        std::map<std::string, unsigned int>::const_iterator const It = GFlids.find(Ref);
        return It == GFlids.end() ? kGhostFlid : It->second;
    }

    pub::Controller::PRIORITY ParsePriority(std::string const& Word)
    {
        if (Word == "HIGHEST")
            return pub::Controller::HIGHEST;
        if (Word == "ABOVE_NORMAL")
            return pub::Controller::ABOVE_NORMAL;
        if (Word == "NORMAL")
            return pub::Controller::NORMAL;
        if (Word == "BELOW_NORMAL")
            return pub::Controller::BELOW_NORMAL;
        return pub::Controller::LOWEST;
    }

    void Bad(std::vector<std::string> const& A)
    {
        std::string Text;
        for (size_t Index = 0; Index < A.size(); ++Index)
            Text += (Index ? " " : "") + A[Index];
        fprintf(stderr, "controller_bus.list: bad directive %s\n", Text.c_str());
        GFailed = true;
    }

    // Runs one action. Returns false for "fail".
    bool Act(std::vector<std::string> const& A, unsigned int Self)
    {
        std::string const& Op = A[0];
        if (Op == "fail" && A.size() == 1)
        {
            GOut->Line(Head() + ",\"op\":\"fail\"}");
            return false;
        }
        if (Op == "send" && A.size() == 4)
        {
            unsigned int const To = Resolve(A[1], Self);
            unsigned int Tag = ParseUint(A[3]);
            int const Msg = static_cast<int>(ParseUint(A[2]));
            GOut->Line(Head() + Format(",\"op\":\"send\",\"to\":\"%s\",\"msg\":%d,\"tag\":%u}",
                                       A[1].c_str(), Msg, Tag));
            int const R = pub::Controller::_SendMessage(To, static_cast<pub::Controller::EMessage>(Msg), &Tag);
            GOut->Line(Head() + Format(",\"ret\":\"send\",\"r\":%d}", R));
        }
        else if (Op == "broadcast" && A.size() == 3)
        {
            unsigned int Tag = ParseUint(A[2]);
            int const Msg = static_cast<int>(ParseUint(A[1]));
            GOut->Line(Head() + Format(",\"op\":\"broadcast\",\"msg\":%d,\"tag\":%u}", Msg, Tag));
            unsigned int const All = 0;
            pub::Controller::_SendMessage(All, static_cast<pub::Controller::EMessage>(Msg), &Tag);
        }
        else if (Op == "create" && A.size() == 3)
        {
            GOut->Line(Head() + Format(",\"op\":\"create\",\"name\":\"%s\",\"prio\":\"%s\"}",
                                       A[1].c_str(), A[2].c_str()));
            std::string const Instance = "Scripted:" + A[1];
            unsigned int const Flid =
                pub::Controller::Create(GStubPath.c_str(), Instance.c_str(), nullptr, ParsePriority(A[2]));
            if (Flid != 0)
            {
                GFlids[A[1]] = Flid;
                GNames[Flid] = A[1];
            }
            GOut->Line(Head() + Format(",\"ret\":\"create\",\"flid\":%u}", Flid));
        }
        else if (Op == "destroy" && A.size() == 2)
        {
            GOut->Line(Head() + Format(",\"op\":\"destroy\",\"to\":\"%s\"}", A[1].c_str()));
            pub::Controller::Destroy(Resolve(A[1], Self));
        }
        else if (Op == "heartbeat" && A.size() == 3)
        {
            float const Interval = ParseFloat(A[2]);
            GOut->Line(Head() + Format(",\"op\":\"heartbeat\",\"to\":\"%s\",\"interval\":\"%08x\"}",
                                       A[1].c_str(), FloatBits(Interval)));
            unsigned int const To = Resolve(A[1], Self);
            int const R = pub::Controller::SetHeartbeatInterval(To, Interval);
            GOut->Line(Head() + Format(",\"ret\":\"heartbeat\",\"r\":%d}", R));
        }
        else if (Op == "timer" && A.size() == 5)
        {
            pub::Controller::TimerExpired Timer;
            Timer.Param1 = ParseUint(A[2]);
            Timer.Param2 = ParseUint(A[3]);
            float const Timeout = ParseFloat(A[4]);
            GOut->Line(Head() + Format(",\"op\":\"timer\",\"to\":\"%s\",\"p1\":%u,\"p2\":%u,\"timeout\":\"%08x\"}",
                                       A[1].c_str(), Timer.Param1, Timer.Param2, FloatBits(Timeout)));
            unsigned int const To = Resolve(A[1], Self);
            int const R = pub::SetTimer(To, Timer, Timeout);
            GOut->Line(Head() + Format(",\"ret\":\"timer\",\"r\":%d}", R));
        }
        else
        {
            Bad(A);
        }
        return true;
    }

    bool Matches(FRule const& Rule, std::string const& Name, std::string const& Event, std::string const& Filter)
    {
        if (Rule.Inst != Name || Rule.Event != Event)
            return false;
        if (Rule.Filter != "*" && Rule.Filter != Filter)
            return false;
        return Rule.Limit == 0 || Rule.Fired < Rule.Limit;
    }

    // Fires the matching rules in order; false when one of them is "fail".
    bool RunRules(unsigned int Flid, std::string const& Name, std::string const& Event, std::string const& Filter)
    {
        bool bKeep = true;
        for (size_t Index = 0; Index < GRules.size(); ++Index)
        {
            if (!Matches(GRules[Index], Name, Event, Filter))
                continue;
            ++GRules[Index].Fired;
            // Copied: an action may not change the rules, but keep the walk independent of them.
            std::vector<std::string> const Action = GRules[Index].Action;
            if (!Act(Action, Flid))
                bKeep = false;
        }
        return bKeep;
    }

    int __cdecl Script(unsigned int Flid, char const* Name, int Event, void const* Data)
    {
        ++GDepth;
        std::string const Short = Name;
        int Result = 1;
        if (Event == SIM_CLIENT_STUB_SCRIPT_CTOR)
        {
            GFlids[Short] = Flid;
            GNames[Flid] = Short;
            GOut->Line(Head() + Format(",\"ctor\":\"%s\",\"flid\":%u}", Short.c_str(), Flid));
            Result = RunRules(Flid, Short, "ctor", "*") ? 1 : 0;
        }
        else if (Event == SIM_CLIENT_STUB_SCRIPT_DTOR)
        {
            GOut->Line(Head() + Format(",\"dtor\":\"%s\",\"flid\":%u}", Short.c_str(), Flid));
            RunRules(Flid, Short, "dtor", "*");
        }
        else if (Event == kHeartbeat)
        {
            float Elapsed;
            memcpy(&Elapsed, Data, sizeof(Elapsed));
            GOut->Line(Head() + Format(",\"to\":\"%s\",\"flid\":%u,\"msg\":%d,\"elapsed\":\"%08x\"}",
                                       Short.c_str(), Flid, Event, FloatBits(Elapsed)));
            RunRules(Flid, Short, "heartbeat", "*");
        }
        else if (Event == kTimerExpired)
        {
            pub::Controller::TimerExpired Timer;
            memcpy(&Timer, Data, sizeof(Timer));
            GOut->Line(Head() + Format(",\"to\":\"%s\",\"flid\":%u,\"msg\":%d,\"p1\":%u,\"p2\":%u}",
                                       Short.c_str(), Flid, Event, Timer.Param1, Timer.Param2));
            RunRules(Flid, Short, "timer", Format("%u", Timer.Param1));
        }
        else if (Event == kControllerDestroyed)
        {
            unsigned int Destroyed;
            memcpy(&Destroyed, Data, sizeof(Destroyed));
            GOut->Line(Head() + Format(",\"to\":\"%s\",\"flid\":%u,\"msg\":%d,\"id\":%u}",
                                       Short.c_str(), Flid, Event, Destroyed));
            RunRules(Flid, Short, "destroyed", NameOf(Destroyed));
        }
        else
        {
            unsigned int Tag;
            memcpy(&Tag, Data, sizeof(Tag));
            GOut->Line(Head() + Format(",\"to\":\"%s\",\"flid\":%u,\"msg\":%d,\"tag\":%u}",
                                       Short.c_str(), Flid, Event, Tag));
            RunRules(Flid, Short, Format("%d", Event), Format("%u", Tag));
        }
        --GDepth;
        return Result;
    }

    struct FStateWriter
    {
        std::string Text;
        bool bFirst = true;
    };

    void __cdecl WriteState(void* Context, unsigned int Flid, unsigned int Priority, char const* /*Module*/,
                            float Interval, float Phase, bool bDeferred)
    {
        FStateWriter& W = *static_cast<FStateWriter*>(Context);
        W.Text += W.bFirst ? "" : ",";
        W.bFirst = false;
        W.Text += Format("{\"name\":\"%s\",\"flid\":%u,\"prio\":%u,\"interval\":\"%08x\",\"phase\":\"%08x\","
                         "\"deferred\":%s}",
                         NameOf(Flid).c_str(), Flid, Priority, FloatBits(Interval), FloatBits(Phase),
                         bDeferred ? "true" : "false");
    }

    void TopLevel(std::vector<std::string> const& A)
    {
        std::string const& Op = A[0];
        if (Op == "tick" && A.size() == 2)
        {
            float const Dt = ParseFloat(A[1]);
            GOut->Line(Head() + Format(",\"op\":\"tick\",\"dt\":\"%08x\"}", FloatBits(Dt)));
            OracleBusTick(Dt);
        }
        else if (Op == "shutdown" && A.size() == 1)
        {
            GOut->Line(Head() + ",\"op\":\"shutdown\"}");
            OracleBusShutdown();
        }
        else if (Op == "state" && A.size() == 1)
        {
            FStateWriter W;
            OracleSimControllerHeartbeats(&WriteState, &W);
            GOut->Line(Head() + ",\"state\":[" + W.Text + "]}");
        }
        else
        {
            Act(A, 0);
        }
    }

    std::string ExeFolder()
    {
        char Path[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, Path, MAX_PATH);
        std::string Folder = Path;
        size_t const Slash = Folder.find_last_of("\\/");
        return Slash == std::string::npos ? std::string(".\\") : Folder.substr(0, Slash + 1);
    }
}

void RunControllerBusSuite()
{
    GStubPath = ExeFolder() + "SimClientStub.dll";
    // Held for the whole suite, so the stub's state survives the last controller's
    // FreeLibrary.
    HMODULE const Stub = LoadLibraryA(GStubPath.c_str());
    if (Stub == nullptr)
    {
        fprintf(stderr, "cannot load %s\n", GStubPath.c_str());
        GFailed = true;
        return;
    }
    typedef void(__cdecl * FSetScript)(FSimClientStubScript);
    FSetScript const SetScript = (FSetScript)GetProcAddress(Stub, "SimClientStubSetScript");
    if (SetScript == nullptr)
    {
        fprintf(stderr, "SimClientStub.dll lacks SimClientStubSetScript\n");
        GFailed = true;
        return;
    }
    SetScript(&Script);

    Suite Out("controller_bus.jsonl");
    GOut = &Out;
    for (std::string const& Line : InputLines("controller_bus.list"))
    {
        std::vector<std::string> const A = Tokens(Line);
        if (A.empty())
            continue;
        if (A[0] == "case" && (A.size() == 2 || A.size() == 3))
        {
            GCase = A[1];
            GSeq = 0;
            GRules.clear();
            GFlids.clear();
            GNames.clear();
            srand(A.size() == 3 ? ParseUint(A[2]) : 1u);
        }
        else if (A[0] == "on" && A.size() >= 6)
        {
            FRule Rule;
            Rule.Inst = A[1];
            Rule.Event = A[2];
            Rule.Filter = A[3];
            Rule.Limit = ParseUint(A[4]);
            Rule.Action.assign(A.begin() + 5, A.end());
            GRules.push_back(Rule);
        }
        else
        {
            TopLevel(A);
        }
    }
    GOut = nullptr;
    SetScript(nullptr);
}

// P3.1: the account suite, over crates/neuron-server/tests/fixtures/oracle/account.list:
//   crc <hex bytes>|.         -> {"in","crc"}         (Server_AccountCrc)
//   name <hex UTF-16LE>|.     -> {"in","dir","key"}   (Server_AccountDirName; the name-file key,
//                                                     the low 16 bits of the CRC of
//                                                     L"AKL!8167fGBA0712", server_classes.cpp)
void RunAccountSuite()
{
    Suite Out("account.jsonl");
    for (std::string const& Line : InputLines("account.list"))
    {
        std::vector<std::string> const A = Tokens(Line);
        if (A.empty())
            continue;
        std::string const Field = A.size() == 2 && A[1] != "." ? A[1] : std::string();
        Bytes const Data = Unhex(Field);
        size_t const Length = Data.empty() ? 0 : Data.size() - 1;   // Unhex adds a NUL
        if (A[0] == "crc" && A.size() == 2)
        {
            unsigned int const Crc = OracleAccountCrc(Data.data(), static_cast<unsigned int>(Length));
            Out.Line(Format("{\"in\":\"%s\",\"crc\":\"%08x\"}", A[1].c_str(), Crc));
        }
        else if (A[0] == "name" && A.size() == 2)
        {
            std::wstring Name;
            for (size_t Index = 0; Index + 1 < Length; Index += 2)
                Name.push_back(static_cast<wchar_t>(Data[Index] | (Data[Index + 1] << 8)));
            char Dir[64] = {};
            OracleAccountDirName(Dir, Name.c_str());
            wchar_t const KeySource[] = L"AKL!8167fGBA0712";
            unsigned short const Key = static_cast<unsigned short>(OracleAccountCrc(
                reinterpret_cast<unsigned char const*>(KeySource),
                static_cast<unsigned int>(wcslen(KeySource) * sizeof(wchar_t))));
            Out.Line(Format("{\"in\":\"%s\",\"dir\":\"%s\",\"key\":\"%04x\"}", A[1].c_str(), Dir, Key));
        }
        else
        {
            fprintf(stderr, "account.list: bad line %s\n", Line.c_str());
            GFailed = true;
        }
    }
}
