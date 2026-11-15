// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

/*
** Thorn uses the same Lua 3.2 parser/code emitter as luaprofile.  The old
** stub was added when only retail bytecode was loaded, but it made the
** documented text-save/reload path fail and left the parser partial items
** without an implementation.  Keep one source of the Lua implementation;
** this translation unit supplies it to Thorn with Thorn's own Lua headers and
** runtime objects, without importing any VC6 ABI or STL layout.
*/
#include "../../luaprofile/lua32/src/lparser.c"
