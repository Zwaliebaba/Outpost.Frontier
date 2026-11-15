// Author: Giperion.

#pragma once

// baseline HandleConnectError (module storage, export ordinal 69): stores the DirectPlay
// connect HRESULT in module storage -- `MOV EAX,[ESP+4]; MOV [0067ea50],EAX; RET`, so
// __cdecl with one 4-byte argument. Its only caller is baseline remoteserver.dll, which
// imports it by name (IAT module storage) from the CDPClient::OnConnect override at
// whenever the HRESULT is non-zero. Undecorated export, hence extern "C".
extern "C" __declspec(dllexport) void __cdecl HandleConnectError(long Result);

// WaitForServerResponse (module storage) is declared in login_mgr.h next to the globals it
// polls and defined in connect_handshake.cpp.
