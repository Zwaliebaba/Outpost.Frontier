// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

#pragma once

// Retail connect paths (all cdecl, all on the static CLoginMgr):
//   FUN_005a8770 -> ConnectToServer   StartServer + CLoginMgr::Connect, error dialog on failure
//   FUN_005a8840 -> LoginToServer     CLoginMgr::Login (single player retries with account
//                                     creation), error dialog on failure
//   FUN_005a8960 -> ConnectAndLogin   both
bool ConnectToServer(char const* IniPath);
bool LoginToServer();
bool ConnectAndLogin(char const* IniPath);
