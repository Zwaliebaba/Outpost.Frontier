// Author: Giperion.

#pragma once

// baseline CharacterSelect → spawn scratch globals (the field EquipDescList cluster,
// ship archetype the field, health the field, system ids, etc.).
// Extractors are called from the routine after CharacterSelect completes; SpawnPlayerShip
// (the routine) reads the same globals when building CShip::CreateParms.

#include "common_archetype.h"
#include "common_goods.h"
#include "server_characterset.h"

#include <list>

// --- Getters (baseline the routine / 4060 / 3f00 / 3de0 / 4650 / 4670 / 4f70 / 4830) ---
unsigned GetPlayerShipArchetypeIdbaseline();          // → the field
EquipDescList& GetPlayerEquipDescListbaseline();      // → the field
unsigned GetPlayerEquipRevisionbaseline();            // → the field
unsigned GetPlayerCollisionRevisionbaseline();        // → the field
// (cdecl) -- set the hardpoint / count / health / equipped state of the player's
// item Id in the field (moving it to another hardpoint re-adds it); false when there is no
// such item. Bumps the field.
bool ModifyPlayerEquipbaseline(unsigned short Id, CacheString const* Hardpoint, int Count,
                             float Health, unsigned char bEquipped);
// (cdecl) -- the first entry of the field with this archetype id whose
// bMission (+0x18) is clear; null when there is none. Callers: 0x0048af5f, 0x004c0533,
// , 0x004dc77b, 0x004dc7ea, HUD_Status 0x004de2fd / 0x004de34f, 0x0052d464.
EquipDesc* FindPlayerNonMissionEquipbaseline(unsigned ArchId);
// (cdecl; the first argument is never read) -- remove Count of the player's item
// Id from the field (EquipDescList::remove_equipment_item), bumping the field; false when
// there is no such item, then the ObjectShip unmount notify (the routine / the routine).
// Callers: 0x0043760f, the launcher fire 0x0052d494, 0x0053beda..0x0053e070 (six).
bool RemovePlayerEquipbaseline(unsigned char UnusedFlag, unsigned short Id, int Count);
// (cdecl; the first argument is never read) -- the field++, add Desc to
// (combined when arch_is_combinable), then the GF ObjectShip mount notify
// (the routine / the routine). Callers: IClient slot 26 (0x004375ea), slot 86 (0x0053e081).
void AddPlayerEquipbaseline(unsigned char UnusedFlag, EquipDesc const* Desc);
std::list<CollisionGroupDesc>& GetPlayerCollisionGroupListbaseline();
unsigned const* GetPlayerRepVibeScratchbaseline();    // → &the field
int GetPlayerRepVibeScratchCountbaseline();           // count at the field
unsigned const* GetPlayerRepVibeScratchTailbaseline(); // up to 8 rank/accessory dwords after header
unsigned GetPlayerSpawnScratchScalarbaseline();       // → the field
int GetPlayerSpawnSystemIdbaseline();                 // → the field
float GetPlayerHealthScalebaseline();                 // → the field
int GetPlayerRankLevelbaseline();                     // → the field
void SetPlayerRankLevelbaseline(int Level);           // → the field
unsigned GetPlayerCreditsbaseline();                  // → the field
unsigned GetPlayerStarSystemIdbaseline();             // → the field
unsigned GetPlayerBaseIdbaseline();                   // (the routine)
unsigned GetPlayerFactionScratchbaseline();           // (the routine)

// --- Setters / sync (CharacterSelect field extractors) ---
void SetPlayerShipArchetypeIdbaseline(unsigned ShipArchId);
void SyncPlayerEquipDescListbaseline(EquipDescList const& Source);
void SyncPlayerCollisionGroupListbaseline(std::list<CollisionGroupDesc> const& Source);
void SetPlayerRepRankBufferbaseline(unsigned const* SourceHeader4, int Count,
                                  unsigned const* TailDwords);
void SetPlayerRepVibeScratchbaseline(unsigned const* SourceHeader4, int Count,
                                   unsigned const* TailDwords);
void SetPlayerSpawnScratchScalarbaseline(unsigned Value);
void SetPlayerSpawnSystemIdbaseline(int SystemId);
void SetPlayerHealthScalebaseline(float Scale);
void SetPlayerCreditsbaseline(unsigned Credits, int bFromCharacterSelect);
void SetPlayerNumKillsbaseline(unsigned Kills);           // → the field
void SetPlayerMissionSuccessesbaseline(unsigned Successes); // → the field
void SetPlayerMissionFailuresbaseline(unsigned Failures); // → the field
void SetPlayerStarSystemIdbaseline(unsigned SystemId);    // (visit/notify stubbed)
void SetPlayerBaseIdbaseline(unsigned BaseId);            // (core assign only)
void SetPlayerFactionScratchbaseline(unsigned Value);

// The player ship's last position (the field, 3 floats) and orientation (the field,
// 9 floats), refreshed every space frame from the watched ship (0x0054b192 / 0x0054b1b0).
float const* GetPlayerLastPositionbaseline();
void SetPlayerLastPositionbaseline(float const* Position);
float const* GetPlayerLastOrientationbaseline();
void SetPlayerLastOrientationbaseline(float const* Orientation);

// Zero-init a CSimple::CreateParms-sized scratch (the routine). Prefer CShip::CreateParms
// default members for new code; kept for listing parity with the routine.
void InitCreateParmsScratchbaseline(void* ParmsBytes);

// Apply CharacterSelect response fields (the routine extract tail) from one record
// of the character list (src/include/server_characterset.h). Returns false if
// CharInfo is null. Does NOT call IServer::CharacterSelect / wait.
bool ApplyCharacterSelectEquipFields(CCharacterSet::CharacterInfo const* CharInfo);

// IClient slot 13 (ch2 0x03 FLPACKET_SERVER_CHARACTERINFO) on the
// connect context the field: mark the list as arrived (+0x1d4), append a deep
// copy of every record to the context's list (+0x1b4) and keep the set's
// SaveFailureCount (+0x1cc).
void CharacterSelect_ReceiveCharacterInfo(CCharacterSet const& Set);

// +0x1b4, the list the routine fills and the routine / the routine /
// walk.
OwnerList<CCharacterSet::CharacterInfo>& CharacterSelect_GetConnectCharacters();

// after the completion flag (CLoginMgr::OnCharacterSelectResponse sets it):
// spawn system id, Reputation::Vibe::SetClientID (multiplayer only), two scratch dwords.
void NotifyCharacterSelectComplete(unsigned const Response4[4]);

// the name of the selected character (wcscpy'd by the routine; read by
// EnterState case 19 through the routine, and by 0x004c0842 / 0x004d328d / 0x0054b7a7).
wchar_t* GetPlayerCharacterNamebaseline();

// The player's visit map the field (system / object id -> visit flags).
// (cdecl, four arguments: every one of the 33 call sites pushes four and cleans
// 0x10) -- the id's flags into *OutFlags; false (nothing written) when the id has no entry.
// Arg3 / Arg4 are not read by the body; the callers pass (0, 0), zones (1, 0) -- 0x0049398d,
// , 0x00498b82, 0x0049b71b.., 0x004a1c45 -- and 0x004a8bb3 / 0x0051cfea (0, 1).
// Their meaning is not recovered.
bool GetPlayerVisitFlagsbaseline(unsigned Id, unsigned char* OutFlags, unsigned Arg3,
                               unsigned Arg4);
// (cdecl) -- unless the entry already carries every bit of Flags, store Flags as
// the entry's value (a store, not an OR) and, with bNotifyServer, send IServer::SetVisitedState
// (vt+0x11c) one {count 1, Id, Flags} record.
void SetPlayerVisitFlagsbaseline(unsigned Id, unsigned char Flags, bool bNotifyServer);
// The map clear the routine / the routine inline (not a baseline function of its own).
void ClearPlayerVisitMapbaseline();
// The two stores the routine inlines (0x004c4a8a / 0x004c4a94): the field = 0 and
// = 0 -- plain stores, not the routine / the routine. Not a baseline function.
void ClearPlayerShipArchAndHealthbaseline();
// (cdecl) -- the health of the player's collision group Id in the field, then
// ++ (also when there is no such group). Caller: IClient slot 63 (0x0053bf3a).
void SetPlayerCollisionGroupHealthbaseline(unsigned short Id, float Health);
