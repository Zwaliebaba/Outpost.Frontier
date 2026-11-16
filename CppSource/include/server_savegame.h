// Author: Giperion.

#pragma once

// Server.dll's character-file entry points. RENAMED AND DE-EXPORTED 2026-09-17
// (wave 17): this used to be common_server_savegame.h and these functions used
// to be COMMON_API, i.e. Common.dll exports, even though every caller is in
// Server.dll. Nothing here crosses a DLL boundary, so nothing here is exported.

// REWRITTEN 2026-09-24 (claude:srv-save). The save/load family is now the
// baseline functions themselves, one port per listing function:

// PlayerData_LoadSaveRecord       the character-file parser
// PlayerData_LoadSavedCharacter   login scan: one file -> record
// PlayerData_ReloadSavedCharacter the post-save reload
// PlayerData_SaveGame             named save / "SaveXXXX" search
// PlayerData_MissionSave          save + copy to AutoSave.fl
// PlayerData_AutoSave             the autosave
// PlayerData_SkipNextAutosave     the one-shot skip latch
// PlayerData_CreateNewCharacter   new-character template expansion
// PlayerData_SelectCharacter      head + MP front (server_classes.cpp)
//                 PlayerData_LoadSelectedCharacter its tail (module storage ..)
//                 PlayerData_ApplySavedCharacter  its record -> slot copy
// PlayerData_SendVisitedState     IClient +0x170
// PlayerData_SendMissionLog       IClient +0x184 (+ the routine)
//                 (module storage .. module storage)

// The implementation is src/Server/server_savegame.cpp; the writer they all
// end in is PlayerData_SerializeToFile (baseline the routine, server_pub.cpp).

#include "common_server_playerdb.h"

// (thiscall on the PlayerData, `RET 8`). Parses the
// character file at Path into Record. Returns true when a [Player] section was
// seen (the byte set at module storage).
bool __cdecl PlayerData_LoadSaveRecord(PlayerData& Slot, char const* Path,
                                       PlayerData::FSavedCharacter& Record);

// (thiscall, `RET 8`): parse <Dir>\<FileName> into
// Slot.saved_characters[FileName]; on failure warn and erase the entry.
bool __cdecl PlayerData_LoadSavedCharacter(PlayerData& Slot, char const* Dir,
                                           char const* FileName);

// (thiscall, `RET 4`): re-read a file the slot has just
// written into its own record container.
void __cdecl PlayerData_ReloadSavedCharacter(PlayerData& Slot, char const* FileName);

// (thiscall, `RET 0xc`). Name "" picks a free "Save%04x".
bool __cdecl PlayerData_SaveGame(PlayerData& Slot, char const* Name,
                                 wchar_t const* Description, unsigned int DescripStrId);

// (thiscall, `RET 4`). Returns CopyFileA's result, or false
// when the save itself failed.
bool __cdecl PlayerData_MissionSave(PlayerData& Slot, unsigned int DescripStrId);

// (thiscall, `RET`).
bool __cdecl PlayerData_AutoSave(PlayerData& Slot);

// `MOV byte ptr [ECX + 0x340],0x1 / RET`.
void __cdecl PlayerData_SkipNextAutosave(PlayerData& Slot);

// The record -> live slot copy of baseline the routine (module storage ..
// ), including the rep-vibe rebuild. SessionId is the worker's
// own stack argument (module storage `MOV ECX,[ESP + 0x192c]`).
void __cdecl PlayerData_ApplySavedCharacter(PlayerData& Slot,
                                            PlayerData::FSavedCharacter const& Record,
                                            unsigned int SessionId);

// (thiscall, `RET 4`): the visit map packed as {count,
// {u32 id, u8 flags}...} into an operator-new block sent by IClient +0x170.
void __cdecl PlayerData_SendVisitedState(PlayerData& Slot, unsigned int SessionId);

// (thiscall, `RET 4`): the mission log flattened by
// AppendMissionLogData into an operator-new block sent by IClient +0x184.
void __cdecl PlayerData_SendMissionLog(PlayerData& Slot, unsigned int SessionId);

// The tail of baseline the routine (module storage .. module storage), which the
// SP and MP paths share: re-open the record's file, the SP [Time] / feelings
// load (the routine), the record copy, the visited / weapon-group / mission-log
// / interface packets and controller event 5. Returns true (module storage).
bool __cdecl PlayerData_LoadSelectedCharacter(PlayerData& Slot,
                                              PlayerData::FSavedCharacter const& Record,
                                              unsigned int SessionId);

// (thiscall, `RET 4`), called only by
// PlayerDB::create_new_character (module storage): expands the exe-side
// NewCharacter.fl / MPNewCharacter.fl template into
// <account dir>\<the routine(Info.Name)>.fl as plain text, and on success
// reloads it into Slot.saved_characters through the routine.
bool __cdecl PlayerData_CreateNewCharacter(PlayerData& Slot, SCreateCharacterInfo const& Info);

// (thiscall, `RET 4`) -- MP only: leave the world
// (system, base, trade, events 0x3a / 8 / 7, group logout, player-list
// removal) and clear +0x64. Defined in server_classes.cpp, next to logout.
void __cdecl PlayerData_DeselectCharacter(PlayerData& Slot, unsigned int SessionId);

// (thiscall, `RET 8`), IServerImpl::CharacterSelect's worker:
// find the record (miss -> false); in MP leave the world if a character is
// selected, drop from the player lists when the id changes, assign +0x64,
// IClient +0x1c0 and the reputation / player-list exchange with every session;
// then the shared tail PlayerData_LoadSelectedCharacter. Defined in
// server_classes.cpp, next to PlayerData_DeselectCharacter.
bool __cdecl PlayerData_SelectCharacter(PlayerData& Slot, CHARACTER_ID const& Id,
                                        unsigned int SessionId);

// (thiscall, `RET 4`), IServerImpl::CharacterInfoReq's
// worker (module storage): in MP with a selected character, autosave and leave
// the world; then one CCharacterSet::CharacterInfo per saved character, sorted
// Restart.fl first, AutoSave.fl next, then newest first; the failed-save
// counter moves into Out.SaveFailureCount. Defined in server_classes.cpp.
class CCharacterSet;
void __cdecl PlayerData_BuildCharacterSet(PlayerData& Slot, CCharacterSet& Out);

// (thiscall, `RET 8`), IServerImpl::DestroyCharacter's
// worker: deselect if selected, find the record, refuse Restart.fl, drop the
// character from Players' id map and its account, unlink the file, erase the
// record, FDUMP in MP. Defined in server_classes.cpp.
bool __cdecl PlayerData_DestroyCharacter(PlayerData& Slot, CHARACTER_ID const& Id,
                                         unsigned int SessionId);

// server_pub.cpp. baseline the routine -- the vibe-id bitmap allocator
// calls before Reputation::Vibe::Alloc (module storage).
void Server_VibeID_Alloc(unsigned int* OutId);
