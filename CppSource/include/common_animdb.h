// Author: Giperion.

#pragma once

#include "common_exports.h"

// AnimDB — Common.dll animation-registration table (common_animdb.cpp).
// baseline exports two registration families: Add(int)/Rem(int) for OutpostFrontier.exe (muzzle
// joint, bay, gates) and Add(VChannel*)/Rem(VChannel const*) for per-frame joint channels,
// plus Count/Shutdown/Update.

namespace AnimDB {

// baseline RTTI ".?AUVChannel@AnimDB@@": an abstract struct with no data and two __thiscall
// slots. AnimDB::Update calls slot 0 on every registered channel (module storage..0627def6:
// MOV ECX,[node+8] / MOV EAX,[ECX] / PUSH Dt / CALL [EAX]); Add(VChannel*) calls slot 1
// (module storage..0627d4e3 / 0627d601..0627d603 / 0627d62c..0627d62e: CALL [EDX+4]).
// No virtual destructor: baseline never deletes a channel through this type. Known derived
// classes: CEGun::JointChannel (Common, vftable module storage); OutpostFrontier.exe carries one
// more (its RTTI base list at module storage).
struct VChannel
{
    virtual void Tick(float Dt) = 0;   // slot 0 (+0x00)
    virtual int GetId() const = 0;     // slot 1 (+0x04): engine instance, -1 when unbound
};

COMMON_API void __cdecl Add(int PlaybackId);                 // ?Add@AnimDB@@YAXH@Z
COMMON_API void __cdecl Add(VChannel* Chan);                 // ?Add@AnimDB@@YAXPAUVChannel@1@@Z
COMMON_API void __cdecl Rem(int PlaybackId);                 // ?Rem@AnimDB@@YAXH@Z
COMMON_API void __cdecl Rem(VChannel const* Chan);           // ?Rem@AnimDB@@YAXPBUVChannel@1@@Z
COMMON_API unsigned int __cdecl Count();                     // ?Count@AnimDB@@YAIXZ
COMMON_API void __cdecl Shutdown();                          // ?Shutdown@AnimDB@@YAXXZ
COMMON_API void __cdecl Update(float Dt);                    // ?Update@AnimDB@@YAXM@Z

} // namespace AnimDB
