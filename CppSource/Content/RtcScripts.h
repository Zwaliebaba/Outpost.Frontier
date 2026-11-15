// Author: Giperion.

#pragma once

// TU23 (06ed4220-06ed5390): the [CharacterEncounter] RTC script of a bar scene -- the object a
// mission's Act_AddRTC / Mission_01b / OnBaseEnter start (FMissionScript::StartRtcScript,
// ) and hand to the base's scene creator the routine.

#include "BaseDB.h"          // FRtcScriptRole
#include "common_tstring.h"  // TString

#include <vector>

// FCharacterEncounter::Actions element (0x48 bytes; VC6 vector<>::insert the routine).
struct FCharacterEncounterAction
{
    // +0x00 the key of the [CharacterEncounter] line: greet 0, walkup 1, action 2, offer 3,
    // decision 4, accept 5, decline / reject 6.
    int Type;
    TString<0x40> Text;     // +0x04 its value (the routine, max 0x40)
};

// The 0x54-byte RTC script object (operator new 0x54 at 06eb8a3e). Ctor the routine, loader
// , member-wise teardown the routine (+0x44, +0x1c, the role strings, +0x0c --
// reverse declaration order, the implicit destructor here).
struct FCharacterEncounter
{
    // +0x00 [CharacterEncounter] Location: pub::GF::FindLocation(BaseId, room). Not set by
    // the ctor (06ed4420 starts at +0x08).
    unsigned int LocationId;
    // +0x04 [CharacterEncounter] Location: pub::GetBaseID(base). Not set by the ctor either;
    // also the default base of relocate_player / start_room.
    unsigned int BaseId;
    bool bAutoplay;                                 // +0x08 autoplay
    std::vector<FRtcScriptRole> Roles;              // +0x0c one per [Char] section
    std::vector<int> ReservedSpots;                 // +0x1c [Reserve] spot placements
    // +0x2c mission_text_id (the second get_value_int(0)); the routine re-copies +0x34 into
    // it when the player has no live mission.
    int MissionTextId;
    int NoOfferTextId;                              // +0x30 nooffer_text_id
    int LoadedMissionTextId;                        // +0x34 mission_text_id (the first read)
    // +0x38 relocate_player: pub::GF::GetSpaceflightLocation() for "Spaceflight", otherwise
    // FindLocation(base, room).
    unsigned int RelocateLocationId;
    unsigned int StartRoomLocationId;               // +0x3c start_room: FindLocation(base, room)
    // +0x40 = the routine() (06eb8acc-06eb8ad5); Cnd_RTCDone compares it with parms[1].
    unsigned int SequenceId;
    std::vector<FCharacterEncounterAction> Actions; // +0x44 greet / walkup / action / ...

    // (thiscall): +0x08 = 0, the three vectors empty, +0x2c..+0x40 = 0.
    FCharacterEncounter();

    // (thiscall, RET 4): the routine (OpenContentDataIni) then every
    // [CharacterEncounter] / [Reserve] / [Char] section. Returns whether the file opened.
    bool Load(char const* Path);

    // (thiscall, RET 4): the Text.Data of the first action of Type, or null.
    char const* FindActionText(int Type) const;
};
