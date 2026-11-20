// Oracle instrumentation (work package P2.3, design/reference-build.md section 10).
//
// SimClientStub.dll's writer, its controller and its exports; see sim_client_stub.h.

#include "sim_client_stub.h"

#include "content.h"   // IDLL, CreateInstance / DestroyInstance, pub::Controller::EMessage
#include "rpc_iclient.h"

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <string>

namespace
{
    FILE* GIClientFile = nullptr;
    FILE* GControllerFile = nullptr;
    unsigned GTick = 0;
    unsigned GCallSeq = 0;
    unsigned GMessageSeq = 0;
    FSimClientStubSink GSink = nullptr;
    CRITICAL_SECTION GLock;
    bool GbLockReady = false;

    void WriteLine(FILE* File, std::string const& Line)
    {
        if (File == nullptr)
            return;
        if (GbLockReady)
            EnterCriticalSection(&GLock);
        fputs(Line.c_str(), File);
        fputc('\n', File);
        fflush(File);
        if (GbLockReady)
            LeaveCriticalSection(&GLock);
    }

    FILE* OpenOutput(std::string const& Folder, char const* Name)
    {
        std::string Path = Folder;
        if (!Path.empty() && Path[Path.size() - 1] != '\\' && Path[Path.size() - 1] != '/')
            Path += '\\';
        Path += Name;
        return fopen(Path.c_str(), "wb");
    }

    // The payloads of the messages a scenario turns into events, read only as far as the
    // sender fills them (pub::Controller::EMessage comments, server_gameobject.h).
    void DecodeMessage(FOracleJson& J, pub::Controller::EMessage MessageId, void const* Data)
    {
        if (Data == nullptr)
        {
            J.Null("a");
            return;
        }
        unsigned int const* const Words = static_cast<unsigned int const*>(Data);
        J.BeginObject("a");
        switch (MessageId)
        {
        case pub::Controller::EMessage::DockComplete:        // {ship, dock, system}
            J.Uint("ship", Words[0]);
            J.Uint("dock", Words[1]);
            J.Uint("system", Words[2]);
            break;
        case pub::Controller::EMessage::DockRequest:         // {ship, target, system, byte}
        case pub::Controller::EMessage::DockCancel:          // {ship, target, arg}
            J.Uint("ship", Words[0]);
            J.Uint("target", Words[1]);
            J.Uint("w2", Words[2]);
            break;
        case pub::Controller::EMessage::ObjectDestroyed:     // FObjectDestroyedEvent +0x00..+0x1f
        {
            J.Uint("id", Words[0]);
            J.Uint("killer", Words[1]);
            J.Uint("system", Words[2]);
            J.Uint("owner", Words[3]);
            J.Int("kind", static_cast<int>(Words[4]));
            float Position[3];
            memcpy(Position, Words + 5, sizeof(Position));
            J.FloatArray("pos", Position, 3);
            break;
        }
        case pub::Controller::EMessage::ObjectDamaged:       // FSubObjDamageEvent / FSolarHitEvent: id first
        case pub::Controller::EMessage::SystemLoaded:        // &system
        case pub::Controller::EMessage::SystemUnloaded:      // &system
        case pub::Controller::EMessage::ControllerDestroyed: // &flid
            J.Uint("w0", Words[0]);
            break;
        case pub::Controller::EMessage::Msg0x25:             // {ship, object, system}
            J.Uint("w0", Words[0]);
            J.Uint("w1", Words[1]);
            J.Uint("w2", Words[2]);
            break;
        case pub::Controller::EMessage::Heartbeat:           // float elapsed
        {
            float Elapsed;
            memcpy(&Elapsed, Data, sizeof(Elapsed));
            J.Float("elapsed", Elapsed);
            break;
        }
        default:
            J.Str("data", "present");
            break;
        }
        J.EndObject();
    }

    // The controller every scenario ship names (ShipInfo.ControllerId).
    struct FSimController final : public IDLL
    {
        explicit FSimController(unsigned int InFlid) : Flid(InFlid) {}

        void HandleMessage(pub::Controller::EMessage MessageId, void const* Parms) override
        {
            FOracleJson J;
            J.Str("rec", "msg");
            J.Uint("tick", GTick);
            J.Uint("seq", ++GMessageSeq);
            J.Uint("flid", Flid);
            J.Int("msg", static_cast<int>(MessageId));
            DecodeMessage(J, MessageId, Parms);
            WriteLine(GControllerFile, J.Finish());
            if (GSink != nullptr)
                GSink(static_cast<int>(MessageId), Parms);
        }

        unsigned int Flid;
    };

    // P3.1, the controller_bus suite (oracle_bus.cpp): a controller made from an instance
    // name "Scripted:<name>". Its constructor, every message and its destructor go to the
    // harness's script callback; the constructor's answer decides whether CreateInstance
    // returns the instance.
    FSimClientStubScript GScript = nullptr;

    struct FScriptedController final : public IDLL
    {
        FScriptedController(unsigned int InFlid, char const* InName) : Flid(InFlid), Name(InName) {}

        ~FScriptedController() override
        {
            if (GScript != nullptr)
                GScript(Flid, Name.c_str(), SIM_CLIENT_STUB_SCRIPT_DTOR, nullptr);
        }

        void HandleMessage(pub::Controller::EMessage MessageId, void const* Parms) override
        {
            if (GScript != nullptr)
                GScript(Flid, Name.c_str(), static_cast<int>(MessageId), Parms);
        }

        unsigned int Flid;
        std::string Name;
    };

    char const GScriptedPrefix[] = "Scripted:";
}

// IDLL's special members: content.h declares them out of line and Content.dll exports its
// own; this module has its own copies, as every controller DLL does.
IDLL::IDLL() {}
IDLL::IDLL(IDLL const& /*Other*/) {}
IDLL& IDLL::operator=(IDLL const& /*Other*/)
{
    return *this;
}
IDLL::~IDLL() {}

// rpc_iclient.h declares IClient's special members out of line; each module that implements
// the class defines them (the class has no data).
IClient::IClient() {}
IClient::IClient(const IClient& /*Other*/) {}
IClient& IClient::operator=(const IClient& /*Other*/)
{
    return *this;
}

unsigned SimStubCallBegin(FOracleJson& J, int Slot, char const* Method)
{
    unsigned const Seq = ++GCallSeq;
    J.Uint("tick", GTick);
    J.Uint("seq", Seq);
    J.Int("slot", Slot);
    J.Str("m", Method);
    J.BeginObject("a");
    return Seq;
}

void SimStubReturnBegin(FOracleJson& J, int Slot, char const* Method, unsigned Seq)
{
    J.Uint("tick", GTick);
    J.Uint("seq", Seq);
    J.Int("slot", Slot);
    J.Str("m", Method);
}

void SimStubCallEnd(FOracleJson& J)
{
    WriteLine(GIClientFile, J.Finish());
}

void SimStubPresence(FOracleJson& J, char const* Name, bool bPresent)
{
    if (bPresent)
        J.Str(Name, "present");
    else
        J.Null(Name);
}

void SimStubUintArray(FOracleJson& J, char const* Name, unsigned int const* Values, int Count)
{
    if (Values == nullptr)
    {
        J.Null(Name);
        return;
    }
    J.BeginArray(Name);
    for (int Index = 0; Index < Count; ++Index)
        J.Uint(nullptr, Values[Index]);
    J.EndArray();
}

// GetClientInterface is declared dllexport by rpc_iclient.h; the others are exported by
// SimClientStub.def.
extern "C" __declspec(dllexport) IClient* GetClientInterface()
{
    return SimStubClient();
}

extern "C" IChatClient* GetChatClientInterface()
{
    return SimStubChatClient();
}

extern "C" IDLL* __cdecl CreateInstance(unsigned int ControllerId, char const* Name, void const* /*Parms*/)
{
    size_t const PrefixLength = sizeof(GScriptedPrefix) - 1;
    if (Name != nullptr && strncmp(Name, GScriptedPrefix, PrefixLength) == 0)
    {
        char const* const Short = Name + PrefixLength;
        // The constructor's rules run before the instance exists, as a Content constructor's
        // pub:: calls do; a "fail" rule makes CreateInstance return null.
        bool const bKeep = GScript == nullptr ||
                           GScript(ControllerId, Short, SIM_CLIENT_STUB_SCRIPT_CTOR, nullptr) != 0;
        if (!bKeep)
            return nullptr;
        return new FScriptedController(ControllerId, Short);
    }
    return new FSimController(ControllerId);
}

extern "C" void __cdecl DestroyInstance(IDLL* Instance)
{
    delete Instance;
}

extern "C" void __cdecl SimClientStubConfigure(char const* OutputFolder)
{
    if (!GbLockReady)
    {
        InitializeCriticalSection(&GLock);
        GbLockReady = true;
    }
    if (GIClientFile != nullptr)
        fclose(GIClientFile);
    if (GControllerFile != nullptr)
        fclose(GControllerFile);
    GIClientFile = nullptr;
    GControllerFile = nullptr;
    if (OutputFolder == nullptr)
        return;
    GIClientFile = OpenOutput(OutputFolder, "iclient.jsonl");
    GControllerFile = OpenOutput(OutputFolder, "controller.jsonl");
}

extern "C" void __cdecl SimClientStubSetTick(unsigned int Tick)
{
    GTick = Tick;
}

extern "C" void __cdecl SimClientStubSetSink(FSimClientStubSink Sink)
{
    GSink = Sink;
}

extern "C" void __cdecl SimClientStubSetScript(FSimClientStubScript Script)
{
    GScript = Script;
}

BOOL WINAPI DllMain(HINSTANCE /*Instance*/, DWORD /*Reason*/, LPVOID /*Reserved*/)
{
    return TRUE;
}
