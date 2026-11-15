// Author: Giperion.

#pragma once

// Content.dll TU42 (06f31190-06f33300): NPC ship archetypes (missions\npcships.ini,
// missions\npcships_test.ini, mission-ini [NPCShipArch]), NPC character
// descriptions (missions\specific_npc.ini [NPC], mbases.ini [GF_NPC]), mission
// [NPC] blocks and the space_costume value.

// Listing: DLLS\BIN\Content. Every declaration names the baseline address
// it ports.

#include "common_id.h"
#include "common_reputation.h"

#include <map>
#include <vector>

class INI_Reader;

// Value of an npcships map node (node+0x10, 0x28 bytes), assembled at
// from the locals the routine fills.
struct FNpcShipArch
{
    ID_String Nickname;                   // +0x00 nickname (also the map key)
    unsigned int ShipArchetype;           // +0x04 ship_archetype, pub::GetShipID
    unsigned int Loadout;                 // +0x08 loadout, pub::GetLoadoutID
    ID_String Pilot;                      // +0x0C pilot
    int StateGraph;                       // +0x10 state_graph, pub::StateGraph::get_state_graph
    std::vector<unsigned int> NpcClasses; // +0x14 npc_class ids, unique, file order
    int Level;                            // +0x24 level

    // the per-section defaults.
    FNpcShipArch();

    // (thiscall): return this->Level (+0x24).
    int GetLevel() const;
};

typedef std::map<unsigned int, FNpcShipArch> FNpcShipArchMap;

// the npcships table. Constructed by the _initterm thunk
// (the routine), destroyed by the atexit thunk the routine.
extern FNpcShipArchMap GNpcShipArchs;

// (cdecl): walk every header of an opened ini; each [NPCShipArch]
// section is read into one FNpcShipArch and inserted under its nickname id
// (unique insert: a duplicate nickname keeps the first entry). Called with
// &GNpcShipArchs by LoadContentGameTables (06efe4c1, 06efe4e8) and with a
// mission's own map by 06f18910.
void __cdecl LoadNpcShipArchs(INI_Reader* Reader, FNpcShipArchMap* Map);

// (cdecl): GNpcShipArchs.erase(begin, end). UnloadContentGameTables
// .
void __cdecl ClearNpcShipArchs();

// (cdecl): Map->find(*Id); &value, or null when absent.
FNpcShipArch* __cdecl FindNpcShipArch(unsigned int const* Id, FNpcShipArchMap* Map);

// (cdecl): the first entry, in key order, whose ShipArchetype is
// ShipArchetype; &value, or null.
FNpcShipArch* __cdecl FindNpcShipArchByShip(unsigned int ShipArchetype, FNpcShipArchMap* Map);

// (cdecl): the space_costume value "head, body, accessory..." of the
// current INI_Reader line: head = GetBodyPart(v0, 1), body = GetBodyPart(v1, 0),
// then GetAccessory(v2..) appended at OutCostume->AccessoryIds[AccessoryCount++]
// (the count is not reset). true when both head and body resolved.
bool __cdecl ParseSpaceCostume(Costume* OutCostume, INI_Reader* Reader);

// 0x44-byte NPC character description: a [NPC] block of missions\specific_npc.ini
// (StoryLauncher the routine) or a [GF_NPC] block of a base's mbases.ini
// (the routine area).
struct FNpcCharacterDesc
{
    Costume Appearance;        // +0x00 head/body/lefthand/righthand, accessories
    unsigned int Voice;        // +0x34 voice, pub::GetVoiceID
    int IndividualName;        // +0x38 individual_name
    unsigned int Affiliation;  // +0x3C affiliation, pub::Reputation::GetReputationGroup
    ID_String Nickname;        // +0x40 nickname

    // (thiscall): head/body/lefthand/righthand, AccessoryCount,
    // Voice, IndividualName = 0; Affiliation = -1; Nickname.clear().
    FNpcCharacterDesc();

    // (thiscall, RET 8): one key of the current line. true when the
    // key is one of base_appr/head/body/lefthand/righthand/accessory/nickname/
    // voice/individual_name/affiliation, false otherwise. nickname also writes
    // OutNickname->Id.
    bool ParseValue(INI_Reader* Reader, ID_String* OutNickname);

    // (thiscall): body, Voice and IndividualName all non-zero.
    bool IsComplete() const;
};

typedef std::map<unsigned int, FNpcCharacterDesc> FNpcCharacterDescMap;

// the specific NPC table filled by StoryLauncher (the routine,
// missions\specific_npc.ini) and read by TU23 (06ed4b0e, 06ed4ba0). Constructed
// by the _initterm thunk the branch (the routine), destroyed by the atexit
// thunk the routine.
extern FNpcCharacterDescMap GSpecificNpcs;

// (cdecl): ParseValue for every remaining value of the current
// section, then true when OutNickname is not empty and Desc->IsComplete().
bool __cdecl LoadNpcCharacterDesc(ID_String* OutNickname, FNpcCharacterDesc* Desc, INI_Reader* Reader);

// 0x4c-byte mission [NPC] block (mission ini, the routine 06eb8709/06eb8726;
// random mission runtime 06f863a2).
struct FMsnNpc
{
    unsigned int Affiliation;  // +0x00 affiliation, pub::Reputation::GetReputationGroup
    int IndividualName;        // +0x04 individual_name
    ID_String NpcShipArch;     // +0x08 npc_ship_arch
    ID_String Field_0x0C;      // +0x0C cleared by the ctor; not written by the parser.
                               // TODO(CNT-AI): meaning from its readers (TU21/TU61).
    Costume Appearance;        // +0x10 space_costume
    unsigned int Voice;        // +0x44 voice, pub::GetVoiceID
    ID_String Nickname;        // +0x48 nickname

    // (thiscall): Affiliation = -1; IndividualName = 0; NpcShipArch
    // and Field_0x0C cleared; head/body/lefthand/righthand, AccessoryCount and
    // Voice = 0; Nickname cleared.
    FMsnNpc();
};

// NPC name helpers. pub::Reputation::GetName fills two FmtStr (First, Second)
// whose mad-lib ids 0x3f17..0x3f24 select the name pattern; these pick the
// appended values (FmtStr::Val::Data of the Nth node) out of them. All cdecl.
struct FmtStr;

// OutA = OutB = 1; by Second->MadLibId: 0x3f18/0x3f21/0x3f22 ->
// OutA = node2, OutB = node3; 0x3f23 -> OutA = node1, OutB = node2; 0x3f1e ->
// OutB = node4; true for those ids, false otherwise. First is not read.
bool __cdecl GetSecondNameValues(FmtStr const* First, FmtStr const* Second, int* OutA, int* OutB);

// *Out = 1; Second->MadLibId 0x3f1a/0x3f1c -> *Out = node3, true;
// otherwise false. First is not read.
bool __cdecl GetSecondNameThirdValue(FmtStr const* First, FmtStr const* Second, int* Out);

// *Out = 1; First->MadLibId 0x3f17/0x3f19/0x3f1b/0x3f1d/0x3f20 ->
// *Out = node3; 0x3f24 -> *Out = node2; true for those, false otherwise.
bool __cdecl GetFirstNameValue(FmtStr const* First, FmtStr const* Second, int* Out);

// *Out = -1; First->MadLibId 0x3f17/0x3f19/0x3f1b/0x3f1d/0x3f20 ->
// *Out = node4; 0x3f24 -> *Out = node3; true for those, false otherwise.
bool __cdecl GetFirstNameLastValue(FmtStr const* First, FmtStr const* Second, int* Out);

// *Out = rand() * 5 / 32768 + 1 (1..5); returns false.
bool __cdecl RollNpcNameValue(FmtStr const* First, FmtStr const* Second, int* Out);

// (cdecl): read every remaining value of the current [NPC] section
// into Npc (Voice reset to 0 first). true when a nickname was read
// (!OutNickname->IsEmpty()).
bool __cdecl LoadMsnNpc(ID_String* OutNickname, FMsnNpc* Npc, INI_Reader* Reader);
