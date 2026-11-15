// Author: Giperion.

#pragma once

// -------------------------------------------------------------------------------------------
// The player's weapon groups -- the static object the field (0x2c bytes, no vftable).
// XC the branch: ctor the routine, then atexit(the branch -> dtor the routine).

// A group is an id and a list of launcher hardpoint names. The server keeps the groups as text
// -- one "wg = %d, %s\n" line per hardpoint, then "\n" (SendToServer, IServer::SetWeaponGroup)
// -- and hands the text back through the client callback the branch (ReadFromServer).

// Callers in baseline:
//   SetShip            HUD_Status::SetTarget 0x004db0fe / 0x004db110
//   FindGroupIndex     0x004db695 (HUD_Status)
//   StoreActive        HUD_Status 0x004dba3e (the routine), 0x004dc425 .. (OnEvent)
//   GetGroupEquipIds   0x004dc639, 0x004dccbb (HUD_Status)
//   Activate           0x004db6b9, 0x004dc43b .. (HUD_Status OnEvent)
//   ReadFromServer     the branch (client callback, vftable 0x005e67e0)
//   GetCurrent         0x004dba33, 0x004dc41a .. (HUD_Status)
//   SetCurrent         0x004db6b2, 0x004dc430 .. (HUD_Status)
//   Clear              0x00483c98 (NeuroNet inventory)
//   AddHardpoint       0x0047ebfa, 0x0047faad (NeuroNet inventory)
//   RemoveHardpoint    0x0047eeda, 0x0047f92e (NeuroNet inventory)
// -------------------------------------------------------------------------------------------

#include "common_goods.h"

#include <string>
#include <vector>

struct CObject;
struct IObjRW;

// One group (baseline operator new(0x14) at 0x004f55df / 0x004f6002; dtor the routine, scalar
// deleting dtor the routine).
struct FWeaponGroup
{
    std::vector<std::string> Hardpoints; // +0x00 (baseline VC6 vector of 0x10-byte strings)
    int Id;                              // +0x10
};

class FWeaponGroups
{
public:
    // everything zero / empty.
    FWeaponGroups();
    // delete every group, then the members (text, list, watcher).
    ~FWeaponGroups();

    // (thiscall, RET 4) -- watch Ship's Watchable (IObjRW + 8; null stays null),
    // set_pointer only when it changes.
    void SetShip(IObjRW* NewShip);

    // (RET 4) -- the index in Groups of the group with this id, -1 if none.
    int FindGroupIndex(int Id) const;

    // (RET 4) -- make group Id (created when missing, emptied otherwise) hold the
    // hardpoints of the watched ship's active launchers, then SendToServer.
    void StoreActive(int Id);

    // (RET 0xc) -- the equipment ids of the watched ship's launchers named in group
    // Id into OutIds; returns how many. MaxIds is only tested after a name mismatch.
    int GetGroupEquipIds(int Id, unsigned short* OutIds, int MaxIds);

    // (RET 4) -- toggle (command 0x19) every launcher whose active state differs
    // from its membership in group Id, re-aiming the guns that end up active; with no such
    // group every active launcher is toggled off.
    void Activate(int Id);

    // rebuild Text and send it with IServer::SetWeaponGroup.
    void SendToServer();

    // (RET 8) -- replace the groups with the ones parsed out of the server's text.
    void ReadFromServer(char const* Data, int Size);

    // / the routine (RET 4) -- +0x28.
    int GetCurrent() const;
    void SetCurrent(int Group);

    // delete every group, then SendToServer.
    void Clear();

    // (RET 8) -- append Hardpoint to group Id unless the group is missing or
    // already names it (NEURONCOMPONENTS stricmp); SendToServer only when appended.
    void AddHardpoint(int Id, char const* Hardpoint);

    // (RET 4) -- drop Hardpoint from the groups whose ids are 0 .. Groups.size()-1,
    // then SendToServer.
    void RemoveHardpoint(char const* Hardpoint);

    BaseWatcher Ship;                  // +0x00 the ship whose launchers are grouped
    std::vector<FWeaponGroup*> Groups; // +0x08 (baseline VC6 vector)
    std::string Text;                  // +0x18 the last text sent
    int Current;                       // +0x28

private:
    // The chain baseline inlines at 0x004f564a.., 0x004f5814.., 0x004f596a.., 0x004f5a5d..:
    // watched, watched - 8 (IObjRW), + 0xc (IObjInspectImpl's CBase), CBase::Object -- a null
    // anywhere gives null.
    CObject* GetShipObject() const;

    // Delete every group and empty Groups (0x004f5e87..0x004f5ea9, 0x004f6196..0x004f61ee).
    void DeleteGroups();

    FWeaponGroups(FWeaponGroups const&) = delete;
    FWeaponGroups& operator=(FWeaponGroups const&) = delete;
};


extern FWeaponGroups G_WeaponGroups;
