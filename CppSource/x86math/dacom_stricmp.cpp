// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

// Isolated TU: import DACOM's stricmp without CRT <cstring> macros remapping the symbol.
// Retail x86math.dll IAT: DACOM.DLL!stricmp (export alias of DacomStricmp).

extern "C" __declspec(dllimport) int __cdecl stricmp(const char* Left, const char* Right);

int CallDacomStricmp(const char* Left, const char* Right)
{
    return stricmp(Left, Right);
}
