// Oracle instrumentation (work package P2.3, design/reference-build.md section 10).

#pragma once

// SimClientStub.dll: the client-interface DLL and the controller of Oracle.exe sim
// (design/adr/0003-simulation-oracle.md point 1).
//
//   * IServerImpl::Startup loads it through SStartupInfo.ModulePath and takes its IClient and
//     IChatClient (GetClientInterface / GetChatClientInterface). Every call is accepted and
//     written to iclient.jsonl; nothing is forwarded (sim_client_stub_iclient.cpp, generated
//     by make_sim_client_stub.py).
//   * pub::Controller::Create loads it a second time (the same module) as a controller
//     (CreateInstance / DestroyInstance): the harness's ships name it as their controller, and
//     every controller message it receives is written to controller.jsonl and handed to the
//     harness's sink, which turns docks and deaths into events.
//
// Record layouts: design/sim-records.md. Until SimClientStubConfigure names the output
// folder nothing is written.

#include "../../OutpostFrontier/Client/oracle_json.h"

class IClient;
class IChatClient;

// The common head of an iclient.jsonl record, {"tick","seq","slot","m","a":{...}}; returns the
// call's seq. SimStubCallEnd closes the open objects and writes the line.
unsigned SimStubCallBegin(FOracleJson& J, int Slot, char const* Method);
// A non-void slot's answer: {"tick","seq","slot","m"}, then the caller adds "r".
void SimStubReturnBegin(FOracleJson& J, int Slot, char const* Method, unsigned Seq);
void SimStubCallEnd(FOracleJson& J);

// "present", or null for a null pointer.
void SimStubPresence(FOracleJson& J, char const* Name, bool bPresent);
void SimStubUintArray(FOracleJson& J, char const* Name, unsigned int const* Values, int Count);

// The one stub object (sim_client_stub_iclient.cpp).
IClient* SimStubClient();
IChatClient* SimStubChatClient();

// Exports for the harness (SimClientStub.def). The sink gets every controller message
// (pub::Controller::EMessage value and its payload) after it is recorded.
typedef void(__cdecl* FSimClientStubSink)(int MessageId, void const* Data);
extern "C" void __cdecl SimClientStubConfigure(char const* OutputFolder);
extern "C" void __cdecl SimClientStubSetTick(unsigned int Tick);
extern "C" void __cdecl SimClientStubSetSink(FSimClientStubSink Sink);
