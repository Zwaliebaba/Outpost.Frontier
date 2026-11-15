// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

// Keep DACOM's imported comparator separate from CRT's similarly named declaration.

extern "C" __declspec(dllimport) int __cdecl stricmp(const char* Left, const char* Right);

int CallDacomStricmp(const char* Left, const char* Right)
{
    return stricmp(Left, Right);
}
