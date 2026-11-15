// Author: Giperion.

#pragma once

// baseline server-module plumbing (source "network\ServerConnect.cpp" per the FDUMP in
// ) and the start / stop of the server connection.
// LoadServerModule        the routine  GetLoadedServer
// GetLoadedChatServer     the routine  FreeServerModule
// StartServer             the routine  ShutdownServerConnection
// The loaded interfaces live in module storage / 0067ed5c; the ones the client talks to are
// G_Server / G_ChatServer (module storage / 0067ecd4, login_mgr.h).

struct IServer;

IServer* LoadServerModule(char const* DllName);
IServer* GetLoadedServer();
void* GetLoadedChatServer();
void FreeServerModule();

bool StartServer(char const* IniPath);
void ShutdownServerConnection();

// Accessors for the port's callers that still take untyped pointers (G_Server / G_ChatServer).
void* GetLocalServerInterface();
void* GetLocalChatServerInterface();

// The server half of baseline the routine (module storage-005b2970): when G_Server is set and
// does not hold the local server, Update (+0x2c) then ElapseTime (+0x30).
void TickLocalServer(float DeltaSeconds);
