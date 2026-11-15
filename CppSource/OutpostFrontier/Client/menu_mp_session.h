// Author: Giperion.

#pragma once

struct FServerInfo;

// (cdecl, one argument): true when the server record's +0x28 and +0x98 are both
// non-zero. Sole caller: the routine 0x005b210e with &the field, the login manager's
// current server (CLoginMgr +0x94, login_mgr.h).
bool MenuMpSessionIsReady(FServerInfo const* Server);
