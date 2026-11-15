// Author: Giperion.

/*
** luaprofile: baseline the routine alloc 0x7C, the routine stack 0x800.
** TObject 16 × STACK_UNIT 128 = 0x800; last = base+0x7f0.
** Not PUC-Rio — layout checks only.
*/

#include "lobject.h"
#include "lstate.h"

typedef char LuaStateSizeCheck[(sizeof(struct lua_State) == 0x7C) ? 1 : -1];
typedef char LuaTObjectSizeCheck[(sizeof(TObject) == 16) ? 1 : -1];
