// Author: Giperion.

// OutpostFrontier.exe's exported client facades: the IClientImpl object `Client` and its two getters.
// The IClient slot bodies live in client_iclient.cpp / client_iclient_space.cpp (see
// OutpostFrontierClient.h); baseline Server / rpclocal / remoteserver reach them through these exports.

#include "OutpostFrontierClient.h"
// MD5Hash: nothing in this file calls it. baseline OutpostFrontier.exe compiled md5hash.h
// with the class dllexport and so exports its own copy constructor (ord 13,
// ), operator= (ord 24, module storage), GetStatePtr (ord 47,
// ) and ??_7 (ord 34, module storage); including the header in this
// exports translation unit is what emits them (MD5HASH_API = dllexport under
// OUTPOST_FRONTIER_EXPORTS). Every other member resolves to NeuronRuntime.lib's import thunks.
#include "md5hash.h"
#include "oracle_mode.h" // Oracle instrumentation (P0.5)

// IClient special members — same trivial bodies Server/rpclocal define for their
// own link of this abstract base (exe does not import Server's copies).
IClient::IClient() {}
IClient::IClient(const IClient& Other) { (void)Other; }
IClient& IClient::operator=(const IClient& Other)
{
    (void)Other;
    return *this;
}

// baseline .data:Client (0x0067ea64), exported as the undecorated data name `Client`
// (ordinal 66) -- extern "C" for that reason, which is also why it cannot carry the G
// prefix. baseline remoteserver.dll imports it (IAT, name hint 0x41). The static initializer
// is IClientImpl's constructor (client_iclient.cpp).
IClientImpl Client;

// (ordinal 68): `MOV EAX,Client; RET`.
extern "C" __declspec(dllexport) IClient* GetClientInterface()
{
    // Oracle instrumentation (P0.5): the logging proxy under `+oracle`, else &Client.
    return OracleWrapClientInterface(static_cast<IClient*>(&Client));
}

// (ordinal 67): `MOV EAX,Client+4; RET` -- the IChatClient
// sub-object of Client.
extern "C" __declspec(dllexport) IChatClient* GetChatClientInterface()
{
    // Oracle instrumentation (P0.5): the logging proxy under `+oracle`, else Client+4.
    return OracleWrapChatClientInterface(static_cast<IChatClient*>(&Client));
}
