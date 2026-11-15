// Author: Giperion.

/*
** SceneScript uses the same Lua 3.2 parser/code emitter as luaprofile.  The old
** stub was added when only baseline bytecode was loaded, but it made the
** documented text-save/reload path fail and left the parser partial items
** without an implementation.  Keep one source of the Lua implementation;
** this translation unit supplies it to SceneScript with SceneScript's own Lua headers and
** runtime objects, without importing any VC6 ABI or STL layout.
*/
#include "../../luaprofile/lua32/src/lparser.c"
