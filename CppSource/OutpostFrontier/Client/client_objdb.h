// Author: Giperion.

#pragma once

#include "common_cobjdb.h"
#include "common_iface.h"
#include "common_cobject.h"

#include <cstddef>
#include <list>

class FClientGameobj;

namespace Universe
{
    struct ISystem;
}

// Client-side IObjDB — baseline the field via the field (StarSys.cpp).
// Concrete type FIObjDB in client_objdb.cpp: ten ManagerKind buckets (the routine),
// GetNext the branch, FindById the branch, GetBucket → the field (built by the routine),
// GetZoneDb → the field (Universe::ISystem*).

// === Draw contract (neighbor renderer) ===
// baseline does NOT draw FSpaceSceneObject proxies. After the routine / Gameobj::create:
//   visual handle = IObjInspect::get_index()  (== CObject/EngineObject::EngineInstanceId @ +0x04)
//   world pose    = IObjInspect::get_position() / get_orientation()  (CObject WorldPosition @ +0x2c)
// Mesh is bound in create() via Root::create_instance → EngineInstanceId, then
// ICliObj+0x20 CliObjLoad(1) on the archetype companion. Enumerate live objects with
// GetClientObjDb() GetNext/Fill — same DB the scanner already uses.

IObjDB* GetClientObjDb();

// (cdecl): link Go into its ManagerKind bucket; a kind-2 Gameobj also goes into
// the large-solar vector (radius > 200, get_type bit 7 clear) and the spatial partition;
// bRemotePhysics appends a CRemotePhysicsSimulation block for its CShip.
void ClientObjDb_RegisterGameobj(FClientGameobj* Go, bool bRemotePhysics = false);

// (cdecl): drop Go's remote-physics block, unlink it from its bucket, and for a
// solar erase it from the large-solar vector and the partition. Does not delete Go.
void ClientObjDb_Unregister(FClientGameobj* Go);

// (cdecl): the per-frame Gameobj update -- buckets 1, 3, 4, 5, 6, then (while the
// partition exists) every solar within 10000 of the listener camera plus every large solar,
// then buckets 10, the eleventh list, 7, 8, 9.
void ClientObjDb_UpdateAll(float Dt);

// unregister and delete every Gameobj whose slot 86 (+0x158) reports it done.
void ClientObjDb_Reap();

// (cdecl): the Gameobj wrapping Obj, found in the bucket its ObjectClass selects.
FClientGameobj* ClientObjDb_FindByCObject(CObject const* Obj);

unsigned ClientObjDb_GetRegisteredCount();
unsigned ClientObjDb_GetSize();

bool ClientObjDb_TryGetWorldPosById(unsigned NicknameId, float OutPos[3]);
bool ClientObjDb_TryGetWorldPosByArchetypeId(unsigned ArchetypeId, float OutPos[3]);

// FreeSpatial(the field) and null it (StarSysLoadIni entry, 00553763).
void ClientObjDb_FreeSpatial();
// (cdecl) -- the star-system unload's ObjDB clear: every remote-physics block and
// every Gameobj not named in Keep (null = keep nothing) is removed and deleted, the large-solar
// vector emptied and the spatial partition freed.
void ClientObjDb_ClearExcept(std::list<FClientGameobj*> const* Keep);
// clear/create the "BSP" partition and bulk-add every solar whose get_type
// & 0xfdff (StarSysLoadIni exit, 00554280).
void ClientObjDb_RebuildSpatial();

unsigned ClientObjDb_GetSolarCount();
bool ClientObjDb_TryGetSolarWorldPosByIndex(unsigned Index, float OutPos[3]);

// (cdecl) -- first Gameobj of ManagerKind bucket *Kind (1..10), null for any
// other kind or an empty bucket. The rest of the chain follows FClientGameobj::ListNext.
FClientGameobj* ClientObjDb_GetKindHead(int const* Kind);
// The large-solar vector (the field, solars of radius > 200) as the draw pass
// (0x0051c549..0x0051c5ac) and the routine walk it. *It is a slot index standing in for
// baseline's element pointer. the routine: start at the first slot; the routine: advance one.
// Each returns the entry at the new slot, or null at the end.
FClientGameobj* ClientObjDb_LargeSolarFirst(size_t* It);
FClientGameobj* ClientObjDb_LargeSolarNext(size_t* It);
// (cdecl): Go is in the large-solar vector (the walk stops at a null entry).
bool ClientObjDb_IsLargeSolar(FClientGameobj const* Go);
// (cdecl): CBeam::move(Dt) on the CBeam of every bucket-3 Gameobj.
void ClientObjDb_MoveBeams(float Dt);
// (cdecl, caller 0x0054b05d): PhySys::GenerateCollisions for every live bucket-3
// beam, then every PhySys collision of the active system resolved on both sides (the routine)
// and delivered to both Gameobjs' slot 101 (+0x194).
void ClientObjDb_ProcessCollisions();
// (cdecl, caller 0x0054b16d): multiplayer only -- every remote-physics block
// (the field) advances its time and CRemotePhysicsSimulation by Dt; a ship CheckForSync
// flags is sent to the server (IServer::SPObjUpdate) and fed back through RemoteUpdate.
void ClientObjDb_UpdateRemotePhysics(float Dt);
// (cdecl) -- the Gameobj whose get_id is ObjectId: Kind 0 searches buckets
// 1, 4, 5, 6, 7, 2 in that order, any other Kind searches only that bucket. Null for an id
// with no bits under 0x3fffffff.
FClientGameobj* ClientObjDb_FindObject(unsigned ObjectId, int Kind);
// (cdecl) -- Kind 0: the ship (bucket 1) or else solar (bucket 2) whose
// CEqObj+0x104 equals VibeId; any other Kind: the get_id match in that bucket, as
// does.
FClientGameobj* ClientObjDb_FindEqObjByVibeId(unsigned VibeId, int Kind);
