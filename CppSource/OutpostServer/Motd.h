// Author: Giperion.

// The server's message of the day (baseline module storage, a std::wstring built empty by the
// static initializer module storage and destroyed by its atexit thunk module storage).
#pragma once

#include <string>

extern std::wstring GMotd;

// writes GMotd as raw UTF-16 (no BOM, no terminator) to
// <local user dir>\motd.dat through PlayerDB::MakeLocalUserPath.
void SaveMotd();
