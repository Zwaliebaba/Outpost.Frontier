// Author: Giperion.

// TU23 (06ed4220-06ed5390): the [CharacterEncounter] RTC script object. See RtcScripts.h.
// Lane CNT-MS-TRIG (implementation notes).

#include "RtcScripts.h"

#include "ContentInternal.h"    // OpenContentDataIni (the routine)
#include "ContentStaticIds.h"   // TU23's 16 static-init thunks 06ed4220-06ed4400
#include "NpcShips.h"           // GSpecificNpcs (the field)
#include "common_id.h"          // CreateID
#include "common_ini.h"         // INI_Reader
#include "common_server.h"      // pub::GetBaseID, pub::GF::*

#include <cstring>
#include <stdlib.h>

// (06ed4420-06ed446b). +0x00 / +0x04 are left unset, as in baseline.
FCharacterEncounter::FCharacterEncounter()
    : bAutoplay(false)
    , MissionTextId(0)
    , NoOfferTextId(0)
    , LoadedMissionTextId(0)
    , RelocateLocationId(0)
    , StartRoomLocationId(0)
    , SequenceId(0)
{
}

// (06ed4470-06ed4494): walk +0x48..+0x4c in 0x48 steps, the first element whose
// +0x00 equals Type -> &element+8 (Text.Data); none -> 0.
char const* FCharacterEncounter::FindActionText(int Type) const
{
    for (std::vector<FCharacterEncounterAction>::const_iterator It = Actions.begin();
         It != Actions.end(); ++It)
    {
        if (It->Type == Type)
            return It->Text.Data;
    }
    return nullptr;
}

// (06ed44a0-06ed5075, thiscall, RET 4). The local INI_Reader is closed by its
// destructor (the inlined teardown at 06ed4e95-06ed505d); AL = the the routine result.
bool FCharacterEncounter::Load(char const* Path)
{
    INI_Reader Reader;
    bool const bOpened = OpenContentDataIni(Path, &Reader);
    if (!bOpened || !Reader.read_header())
        return bOpened;
    do
    {
        if (Reader.is_header("CharacterEncounter"))
        {
            if (!Reader.read_value())
                continue;
            do
            {
                // the action record's text is emptied on every line; its
                // type is written only by the action keys below.
                FCharacterEncounterAction Action;
                Action.Text.Length = 0;
                Action.Text.Data[0] = '\0';
                bool bAction = true;
                if (Reader.is_value("Location"))
                {
                    // base (max 0x20), room (max 0x40), GetBaseID into +4,
                    // FindLocation(+4, room) into +0.
                    TString<0x20> BaseName;
                    BaseName = Reader.get_value_string(0);
                    TString<0x40> RoomName;
                    RoomName = Reader.get_value_string(1);
                    pub::GetBaseID(BaseId, BaseName.Data);
                    LocationId = pub::GF::FindLocation(BaseId, RoomName.Data);
                    bAction = false;
                }
                else if (Reader.is_value("action"))
                    Action.Type = 2;
                else if (Reader.is_value("walkup"))
                    Action.Type = 1;
                else if (Reader.is_value("greet"))
                    Action.Type = 0;
                else if (Reader.is_value("offer"))
                    Action.Type = 3;
                else if (Reader.is_value("decision"))
                    Action.Type = 4;
                else if (Reader.is_value("accept"))
                    Action.Type = 5;
                else if (Reader.is_value("decline") || Reader.is_value("reject"))
                    Action.Type = 6;
                else
                {
                    bAction = false;
                    if (Reader.is_value("autoplay"))
                    {
                        bAutoplay = true;
                    }
                    else if (Reader.is_value("mission_text_id"))
                    {
                        // two reads of value 0, the first into +0x34.
                        LoadedMissionTextId = Reader.get_value_int(0);
                        MissionTextId = Reader.get_value_int(0);
                    }
                    else if (Reader.is_value("nooffer_text_id"))
                    {
                        NoOfferTextId = Reader.get_value_int(0);
                    }
                    else if (Reader.is_value("relocate_player"))
                    {
                        // .
                        TString<0x40> RoomName;
                        RoomName = Reader.get_value_string(0);
                        if (_stricmp(RoomName.Data, "Spaceflight") == 0)
                        {
                            RelocateLocationId = pub::GF::GetSpaceflightLocation();
                        }
                        else
                        {
                            unsigned int RoomBaseId = BaseId;
                            if (!Reader.is_value_empty(1))
                                pub::GetBaseID(RoomBaseId, Reader.get_value_string(1));
                            RelocateLocationId = pub::GF::FindLocation(RoomBaseId, RoomName.Data);
                        }
                    }
                    else if (Reader.is_value("start_room"))
                    {
                        // .
                        TString<0x40> RoomName;
                        RoomName = Reader.get_value_string(0);
                        unsigned int RoomBaseId = BaseId;
                        if (!Reader.is_value_empty(1))
                            pub::GetBaseID(RoomBaseId, Reader.get_value_string(1));
                        StartRoomLocationId = pub::GF::FindLocation(RoomBaseId, RoomName.Data);
                    }
                }
                if (bAction)
                {
                    // the text is the line's first value (max 0x40), the
                    // record is appended at +0x4c (the routine).
                    Action.Text = Reader.get_value_string();
                    Actions.push_back(Action);
                }
            } while (Reader.read_value());
        }
        else if (Reader.is_header("Reserve"))
        {
            // every spot whose placement resolves in +0x00 is appended at
            // +0x24 (the routine).
            if (!Reader.read_value())
                continue;
            int Spot;
            do
            {
                if (Reader.is_value("spot") &&
                    pub::GF::GetCharacterPlacementByName(LocationId, Reader.get_value_string(),
                                                         Spot) == 0)
                {
                    ReservedSpots.push_back(Spot);
                }
            } while (Reader.read_value());
        }
        else if (Reader.is_header("Char"))
        {
            // role {"" , no character, spot -1, fidget ""}.
            FRtcScriptRole Role;
            Role.Role.Length = 0;
            Role.Role.Data[0] = '\0';
            Role.Character = nullptr;
            Role.Spot = -1;
            if (Reader.read_value())
            {
                do
                {
                    if (Reader.is_value("actor"))
                    {
                        // the role name (max 0x20); unless a character is
                        // already set, the specific NPC of the same name (map find, the field).
                        Role.Role = Reader.get_value_string();
                        if (Role.Character == nullptr)
                        {
                            unsigned int const Key = CreateID(Reader.get_value_string());
                            FNpcCharacterDescMap::const_iterator It = GSpecificNpcs.find(Key);
                            if (It != GSpecificNpcs.end())
                                Role.Character = &It->second;
                        }
                    }
                    else if (Reader.is_value("spot"))
                    {
                        // the result is not tested.
                        pub::GF::GetCharacterPlacementByName(LocationId, Reader.get_value_string(),
                                                             Role.Spot);
                    }
                    else if (Reader.is_value("npc"))
                    {
                        // the specific NPC, replacing any earlier one.
                        unsigned int const Key = CreateID(Reader.get_value_string());
                        FNpcCharacterDescMap::const_iterator It = GSpecificNpcs.find(Key);
                        if (It != GSpecificNpcs.end())
                            Role.Character = &It->second;
                    }
                    else if (Reader.is_value("fidget"))
                    {
                        // the inlined VC6 string assign.
                        Role.Fidget = Reader.get_value_string();
                    }
                } while (Reader.read_value());
            }
            // the inlined vector insert at +0x14 (end).
            Roles.push_back(Role);
        }
    } while (Reader.read_header());
    return bOpened;
}
