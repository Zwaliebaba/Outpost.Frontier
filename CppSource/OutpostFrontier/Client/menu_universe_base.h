// Author: Giperion.

#pragma once

// baseline Gf\LocationController.cpp -- the singleton the field: the base / room registry and
// the base and location changes (enter, leave, load, unload) every client state goes through.

class RoomData;

// — LocationController base/room registry, built once after DataConfig. baseline
// reaches it through the menu pre-init chain: singleton the routine, vtable slot 8
// (the branch) → the routine → the routine(the field).
void BuildClientLocationMaps();
void ClearClientLocationMaps();
// 's lookup through this+0x0c (record[0] = RoomData*). Null when the id is unknown.
RoomData* LookupClientLocationRoom(unsigned LocationId);
// this+0x20, keyed by BaseData::BaseId. Returns BaseData*; an unknown id FDUMPs
// and reads through null, as baseline.
void* LookupClientLocationBase(unsigned BaseId);
unsigned GetClientLocationBaseCount();
unsigned GetClientLocationRoomCount();

// LocationController field accessors (the routine/a5a0/a630/a860/ -- a510 and b630 are
// GetClientLocationBaseMarket / GetClientCurrentLocationBaseData in client_base_market.h;
// a8d0/aaf0/ab60). Runtime/State pointers are opaque (FLocationBaseRuntime/State are file-local
// types); they are null until the base (the routine) / room (the routine) is loaded.
void* GetClientLocationBaseRuntime(unsigned BaseId);
// again, typed: the runtime's +0x00 MissionVendorProperties cache
// (mirror_property_bucket.h), which is what NN_MissionVendor walks.
template <class T> struct TMirrorPropertyBucket;
class MissionVendorProperties;
TMirrorPropertyBucket<MissionVendorProperties>* GetClientLocationBaseMissionOffers(unsigned BaseId);
// — the runtime's +0x18 NewsBroadcastProperties cache, which NN_News walks.
class NewsBroadcastProperties;
TMirrorPropertyBucket<NewsBroadcastProperties>* GetClientLocationBaseNews(unsigned BaseId);
void SetClientLocationBaseStartLocation(unsigned BaseId, unsigned LocationId);
void UpsertClientLocationBaseMissionOffer(MissionVendorProperties const& Offer);
void EraseClientLocationBaseMissionOffer(unsigned BaseId, unsigned long Key);
void UpsertClientLocationBaseNews(NewsBroadcastProperties const& News);
void EraseClientLocationBaseNews(unsigned BaseId, unsigned long Key);
void SetClientLocationBaseMissionListComplete(unsigned BaseId); // (+0x14)
void SetClientLocationBaseNewsListComplete(unsigned BaseId);    // (+0x2c)
void* GetClientLocationRoomState(unsigned LocationId);
// The room state's MirrorDatabase buckets (the 0x48-byte object the routine allocates at room
// entry +0x04). Callers: IClient slots 30..37 (client_iclient.cpp).
class CharacterProperties;
class ScriptBehavior;
class AmbientScriptProperties;
void UpsertClientLocationRoomCharacter(CharacterProperties const& Character);
void SetClientLocationRoomCharactersComplete(unsigned LocationId);
void UpsertClientLocationRoomScriptBehavior(ScriptBehavior const& Behavior);
void EraseClientLocationRoomScriptBehavior(unsigned LocationId, unsigned long Key);
void SetClientLocationRoomScriptBehaviorsComplete(unsigned LocationId);
void UpsertClientLocationRoomAmbientScript(AmbientScriptProperties const& Script);
void EraseClientLocationRoomAmbientScript(unsigned LocationId, unsigned long Key);
void SetClientLocationRoomAmbientScriptsComplete(unsigned LocationId);
// (typed) / the routine -- the room state's +0x00 / +0x18 buckets. Caller:
// RoomControl the routine.
TMirrorPropertyBucket<CharacterProperties>* GetClientLocationRoomCharacters(unsigned LocationId);
TMirrorPropertyBucket<ScriptBehavior>* GetClientLocationRoomScriptBehaviors(unsigned LocationId);
// the room state's +0x30 bucket. Caller: RoomControl the routine.
TMirrorPropertyBucket<AmbientScriptProperties>* GetClientLocationRoomAmbientScripts(
    unsigned LocationId);
// (RET 4) -- the location's RoomData is loaded (RoomData +0x08). Caller: the Gf
// script sequence stop the routine (0x004554f7).
bool IsClientLocationRoomLoaded(unsigned LocationId);
void* GetClientCurrentLocationRuntime();
void UnloadClientLocationRoomState(unsigned LocationId);

// LocationController (the field) base and location changes. Every IServer notification
// (BaseEnter / BaseExit / LocationEnter / LocationExit / *InfoRequest) goes to the field
// with the session the field when both the server exists and this+0x5a is clear.
// IntroFocus writes +0x5a = 1 before entering an intro base (0x00573919), then clears it
// when leaving that base (0x0057409b / 0x0057412c / 0x00574ad8).
void SetClientLocationNoServerNotify(bool bDisabled);

// +0x34..+0x44, +0x48, +0x4c, +0x50 and +0x59 zeroed.
void ResetClientLocationController();
// (RET 4) -- count a queued change's timer up; once it reaches the delay, change
// to the queued location (the routine), run its audio (the routine) when asked, clear the
// queue. Caller: the Gameflow tick the routine.
void TickClientLocationController(float DeltaSeconds);
// (RET 0xc) -- queue a location change after Delay seconds.
void QueueClientLocationChange(unsigned Location, float Delay, bool bWithAudio);
// (RET 4) -- enter BaseId: BaseEnter, load it at a zero count, count + 1, then
// its start location (the server's, runtime+0x48, else BaseData's) through the routine and
// . Caller: the routine (no location given).
void EnterClientLocationBase(unsigned BaseId);
// (RET 4) -- the base half of the routine alone. Caller: the routine (a
// location given).
void SetClientLocationBase(unsigned BaseId);
// (RET 4) -- move to Location: across bases (leave the room, unload/exit the old
// base, enter/load the new one, the routine / the routine / the routine) or within one
// (leave the room), then the new location's records (+0x3c / +0x40), LocationEnter and the room
// loaded at a zero count, count + 1. +0x59 is set for the length of the call.
void ChangeClientLocation(unsigned Location);
// (RET 4) -- the location's music and ambience (RoomData +0xcc..+0xd5) and the Gf
// audio fade. Callers outside the controller: the routine (0x00558997, a location given).
void ApplyClientLocationAudio(unsigned LocationId);
// 0x0055896f -- the field (the controller's this+0x58) = 1: the next
// keeps a non-looping music / ambience, then clears the byte.
void SetClientLocationKeepAudio();
// leave the current location and base (LocationExit / BaseExit, rooms and base
// unloaded when their counts reach 0). Caller: the routine (and the menu 0x00574053 /
// / 0x00574a90).
void LeaveClientLocationBase();
// draw the current location: +0x40's runtime through the routine. Caller: the
// base frame the routine.
void DrawClientLocation();
unsigned GetClientLocationCurrentBase();  // this+0x34
// (thiscall on the field) -- this+0x54, the loaded base's solar object id
// (the routine). Callers (unported): 0x00440819 / 0x0044103e / 0x004411f9.
unsigned GetClientLocationBaseSolarId();
// (thiscall on the field) -- the strid_name of the base the player is in: the
// this+0x34 base looked up (the routine inlined, FDUMP on a miss), its BaseData's id through
// Universe::get_base, then IBase slot 2 (+0x08, [+0x0c] = strid_name). 0 when the entry has no
// BaseData or the universe has no such base. Callers: NN_ArrivalText 0x0045aeb4, the dealer
// .
int GetClientCurrentBaseStridName();
unsigned GetClientLocationCurrentRoom();  // this+0x38
// this+0x59 (the field): zeroed by the ctor (0x00438eef) and the routine (0x0043921a),
// raised for the length of the routine (0x0043b6bf / 0x0043b963) and set by the base enter
// (0x005588bc) right after its the routine. Readers outside the controller
// address it absolutely: the routine (0x0045a75b), NN_MainButton 0x0045cb09, NN_Pause
// (0x004a75de).
bool IsClientLocationInTransition();
// 0x005588bc -- MOV byte [the field],1.
void SetClientLocationInTransition();

// — the current room's camera, or null when no room is entered. The object
// answers vtable slots 3 (position) and 4 (orientation), which is all the routine asks of it.
void* GetClientRoomCamera();
