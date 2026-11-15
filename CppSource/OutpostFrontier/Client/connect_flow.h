// Author: Giperion.

#pragma once

// baseline connect paths (all cdecl, all on the static CLoginMgr):
// -> ConnectToServer   StartServer + CLoginMgr::Connect, error dialog on failure
// -> LoginToServer     CLoginMgr::Login (single player retries with account
//                                     creation), error dialog on failure
// -> ConnectAndLogin   both
bool ConnectToServer(char const* IniPath);
bool LoginToServer();
bool ConnectAndLogin(char const* IniPath);
