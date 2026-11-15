// Author: Giperion.

#pragma once

#include "common_exports.h"
#include "common_id.h"

// IObjDB — client inspect object database.

// Client (OutpostFrontier.exe): static the field via the field
//   (RTTI Object::IObjDBImpl, vtable module storage). Port: GetClientObjDb()
//   in OutpostFrontier Client/client_objdb.cpp.
// Server: baseline StarSystem (RTTI .?AVStarSystem@@, hierarchy StarSystem ->
//   IObjDB, single inheritance) is the per-system IObjDB. Its constructor
//   stores this base's vtable (module storage) first and its own
//   (module storage) last. Port: class StarSystem in server_sysdb.h.

// There is no CObjDB class in Common.dll. CleanUpDatabases @ RVA 0xa4870 is a
// 5-byte JMP to Reputation::Vibe::FreeDynamic (see common_reputation).
// CSimple::connect only stores the pointer; objects enter the client DB through
// / Gameobj registration (ClientObjDb_Register).

struct IObjRW;
namespace Universe { struct ISystem; }

// baseline Object::IObjDBImpl vtable (module storage) — four __thiscall slots.
// Common CEScanner / CAsteroidSystem / CSimple call these through raw void**.
struct IObjDB
{
    // +0x00 the branch — walk type-bucket lists; IterState =
    //   {Cursor*, ListHead*, Mask}. Mask bits select which lists to visit.
    virtual IObjRW* __thiscall GetNext(void* IterState) = 0;

    // +0x04 the branch — find Gameobj by object id (arg = unsigned* Id).
    virtual IObjRW* __thiscall FindById(unsigned const* ObjectId) = 0;

    // +0x08 the branch → JMP the routine: return the field
    // (Universe::ISystem* set by EnterStarSystem the routine).
    virtual Universe::ISystem* __thiscall GetZoneDb() = 0;

    // +0x0c the branch — if (TypeMask & 0xfdff) return the field else 0.
    // = NewSpatial("BSP") ISpatialPartition*; Fill = vtable +0x1c.

    // NOT pure. IObjDB's own vtable (Server.dll module storage, COL
    // -> .?AUIObjDB@@) holds _purecall (module storage) in
    // slots 0..2 and a real body in slot 3: the branch,
    // 33c0    XOR EAX,EAX
    // c20400  RET 0x4
    // i.e. the base answers "no spatial bucket" for any mask. Both known
    // implementations (Object::IObjDBImpl in OutpostFrontier.exe, StarSystem in
    // Server.dll) override it.
    virtual void* __thiscall GetBucket(unsigned TypeMask)
    {
        (void)TypeMask;
        return nullptr;
    }
};

class INI_Reader;

// baseline ?FindSolarRep@SolarReps@@YAHABUID_String@@@Z (module storage): a global-namespace
// SolarReps, taking the nickname as an ID_String. Returns Id.Id when Reputation::Vibe::Verify
// accepts it, else -1.
namespace SolarReps
{
    COMMON_API int __cdecl FindSolarRep(ID_String const& Id);
}

namespace Reputation
{
    namespace SolarReps
    {
        // — walk system INI [Object] sections and Vibe::Alloc each nickname
        // (the routine / the routine). baseline runs this once per System file= open inside
        // LoadUniverseIni, before Zone/Object universe parse (then INI_Reader::reset).
        // Internal to Common.dll: baseline exports no symbol for it.
        void __cdecl LoadObjectVibesFromSystemIni(INI_Reader& Reader);
    }
}
