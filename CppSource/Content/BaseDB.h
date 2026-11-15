// Author: Giperion.

#pragma once

// Content.dll TU17 (06ea7750-06eb2670): the mbases.ini database -- one MBase record per
// base (map the field, key = base id), each with its [BaseFaction] list, [GF_NPC]
// bar characters, [mRoom] rooms and fixtures, and [MVendor] offer counts. TU22
// (06ed3d90-06ed4220, BaseNpcRecords.cpp) holds the parsers of the GF_NPC "bribe" /
// "know" / "rumor" records kept by those characters. Lane CNT-GS (implementation notes,
// TU map).

#include "common_ini.h"      // Vector
#include "common_net.h"      // CommReferrable
#include "common_tstring.h"  // TString
#include "ContentRefObject.h" // FContentRefObject (vtable 06fb43f8)
#include "NpcShips.h"        // FNpcCharacterDesc

#include <cstdio>
#include <list>
#include <map>
#include <string>
#include <vector>

class INI_Reader;
struct FContentBase;
struct ID_String;
struct FContentPlayerStateBase;
struct FContentPlayerState;
struct FBarOffer;
struct FBarCharacter;
struct FCharacterEncounter;         // RtcScripts.h (TU23)
struct FSystemInfo;
class FRandomMissionLoader;         // RandomMission.h (TU47)
struct FRandomMissionData;          // RandomMission.h (TU27)

// ---------------------------------------------------------------------------------------
// TU22 -- GF_NPC records.
// ---------------------------------------------------------------------------------------

// "bribe = group, cost, value2" (0xc bytes; vector at GF_NPC +0x54, inserted by
// ).
struct FBaseNpcBribe
{
    int Cost;                   // +0x00 value 1; written before the zero test
    unsigned int Group;         // +0x04 value 0, pub::Reputation::GetReputationGroup
    int TextIds;                // +0x08 value 2: copied into FBarBribeOffer::TextIds
                                // (the routine), the mad-lib of the offer text.

    // (thiscall, RET 4): false when the group is unknown or the cost is 0.
    bool Parse(INI_Reader* Reader);
    // (thiscall, RET 4): the player's feeling towards Group (0 when the
    // query fails) is below 0.6 (the field).
    bool IsOffered(FContentPlayerStateBase const* Player) const;
};

// "know = v0, v1, v2, level" (0x10 bytes; vector at GF_NPC +0x64, the routine).
struct FBaseNpcKnow
{
    int Price;                  // +0x00 value 2: compared with the player's cash and
                                // appended as an int (TU34 the routine, the routine)
    int Field_0x04;             // +0x04 value 1 (an ids number): the rumor record key, and
                                // the string of mad-lib 0x3efe (the routine)
    int Field_0x08;             // +0x08 value 0 (an ids number): the string of mad-lib
                                // 0x3efd (the routine). TODO: names from BaseSupport.
    unsigned int RepLevel;      // +0x0c value 3, index into GBaseNpcRepThresholds; picks
                                // the generic script 0x26/0x26/0x27/0x28 (the routine)

    // (thiscall, RET 4): true when RepLevel < 4 (unsigned).
    bool Parse(INI_Reader* Reader);
    // (thiscall, RET 8): Feeling reaches the level's threshold and the
    // player has no rumor record for Field_0x04.
    bool IsOffered(FContentPlayerState const* Player, float Feeling) const;
};

// "rumor" / "rumor_type2 = from stage, to stage, level, ids..." (0x2c bytes; vector at
// GF_NPC +0x74, the routine). The caller zeroes the whole record before Parse.
struct FBaseNpcRumor
{
    int FromStage;              // +0x00 value 0, GContentStageNames lookup
    int ToStage;                // +0x04 value 1, GContentStageNames lookup
    int RepLevel;               // +0x08 value 2, index into GBaseNpcRepThresholds
    int Ids[8];                 // +0x0c values 3.. until the first empty value

    // (thiscall, RET 4): always true.
    bool Parse(INI_Reader* Reader);
    // (thiscall, RET 8): 0 unless the player's mission number lies in
    // [FromStage, ToStage], Feeling reaches the level's threshold and the player has no
    // rumor record for Ids[0]; then 0x40 for a rumor bounded on both ends
    // (FromStage > 0 and ToStage < 0x2a), else (RepLevel + 1) * 5.
    int GetWeight(FContentPlayerState const* Player, float Feeling) const;
};

// the feeling thresholds indexed by the know / rumor level
// (read by the routine and the routine).
extern float const GBaseNpcRepThresholds[4];

// "misn = type, min, max" (0x24 bytes; vector at GF_NPC +0x44, the routine).
struct FBaseNpcMission
{
    float MinDifficulty;        // +0x00 value 1, clamped to [0, 100]
    float MaxDifficulty;        // +0x04 value 2, clamped to [0, 100]
    TString<24> Type;           // +0x08 value 0
};

// One [GF_NPC] block of a base (0x94 bytes, the value of MBase +0x84). Built on the stack
// by the routine (the routine + five vector ctors), copied by the routine, destroyed by
// (member-wise).
struct FBaseNpc
{
    FNpcCharacterDesc Desc;                     // +0x00 body/head/voice/affiliation...
    std::vector<FBaseNpcMission> Missions;      // +0x44 "misn"
    std::vector<FBaseNpcBribe> Bribes;          // +0x54 "bribe"
    std::vector<FBaseNpcKnow> Knows;            // +0x64 "know"
    std::vector<FBaseNpcRumor> Rumors;          // +0x74 "rumor", "rumor_type2"
    std::vector<unsigned int> Rooms;            // +0x84 "room", location ids

    // (thiscall, RET 8): pick what this character offers Player -- a
    // mission, a bribe, a rumor or a piece of knowledge, weighted 30 / 20 / max rumor
    // weight / 15. Out->Kind stays 0 when nothing applies.
    void ChooseOffer(FBarOffer* Out, FContentPlayerState* Player) const;
};

// (cdecl): every remaining value of the current [GF_NPC] section into Npc.
// true when a nickname was read and Npc->Desc.IsComplete(); the routine ignores it.
bool __cdecl LoadBaseNpc(ID_String* OutNickname, FBaseNpc* Npc, INI_Reader* Reader,
                         unsigned int BaseId);

// ---------------------------------------------------------------------------------------
// TU17 -- rooms and fixtures.
// ---------------------------------------------------------------------------------------

// "fixture = npc, placement, fidget, role" of an [mRoom] (new 0x78 in the routine,
// vtable 06fb4498 {the routine AddRef, the routine Release, the routine deleting dtor}:
// the FContentRefObject slots, no dtor of its own).
struct FBaseFixture : public FContentRefObject
{
    FBaseFixture();

    FBaseNpc* Npc;                  // +0x08 value 0, the owning base's GF_NPC
    int Placement;                  // +0x0c value 1, pub::GF::GetCharacterPlacementByName
    TString<64> Fidget;             // +0x10 value 2 (a .thn script in the data)
    TString<32> Role;               // +0x54 value 3 ("bartender", "trader"...)
};

// The fixture list of a room: list<FBaseFixture*> holding one reference per element.
// Clear = the routine (Release every element, then erase), dtor = the routine, the
// copy inside the room copy the routine AddRefs the copied elements.
class FBaseFixtureList
{
public:
    FBaseFixtureList() = default;
    FBaseFixtureList(FBaseFixtureList const& Other);
    FBaseFixtureList& operator=(FBaseFixtureList const& Other);
    ~FBaseFixtureList();

    // insert at end() plus the caller's AddRef (the routine 06eaad47).
    void Add(FBaseFixture* Fixture);
    void Clear();

    std::list<FBaseFixture*> Fixtures;
};

// One [mRoom] (0x14 bytes; vector at MBase +0xa4, inserted by the routine).
struct FBaseRoom
{
    unsigned int RoomId = 0;            // +0x00 "nickname", pub::GF::FindLocation
    int CharacterDensity = 0;           // +0x04 "character_density"
    FBaseFixtureList Fixtures;          // +0x08 "fixture"
};

// ---------------------------------------------------------------------------------------
// TU17 -- factions.
// ---------------------------------------------------------------------------------------

// "mission_type = type, min, max[, weight]" of a [BaseFaction] (0x28 bytes, vector at
// faction +0x14, the routine). The type is always "DestroyMission": value 0 is not read.
struct FBaseMissionType
{
    float MinDifficulty;        // +0x00 value 1
    float MaxDifficulty;        // +0x04 value 2
    TString<24> Type;           // +0x08 "DestroyMission"
    float Weight;               // +0x24 value 3, 1.0 when empty
};

// One [BaseFaction] (0x28 bytes; vector at MBase +0x58, the routine; element dtor
// ).
struct FBaseFaction
{
    unsigned int Group = 0xffffffff;                // +0x00 "faction"
    std::vector<unsigned int> Npcs;                 // +0x04 "npc", unique CreateID ids
    std::vector<FBaseMissionType> MissionTypes;     // +0x14 "mission_type"
    float Weight = 0.0f;                            // +0x24 "weight", -1.0 when absent
};

// / the field: .data bytes initialised to 1 and never written; a
// [BaseFaction] mission_type is kept only when the flag of the current game mode is set
// (the routine, the routine).
extern bool GBaseMissionTypesInSinglePlayer;
extern bool GBaseMissionTypesInMultiplayer;

// ---------------------------------------------------------------------------------------
// TU17 -- the MBase record.
// ---------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------
// TU17 -- bar characters (the people standing in a room while players are in it).
// ---------------------------------------------------------------------------------------

// The ref-counted offer objects a bar character carries derive from FContentRefObject
// (ContentRefObject.h, vtable 06fb43f8); the derived dtors reset the vptr to it
// (the routine 06ea7bde).

// Bribe offer (new 0x24 in the routine 06ea893f, vtable 06fb443c: AddRef, Release,
// empty deleting dtor).
class FBarBribeOffer : public FContentRefObject
{
public:
    int Price = 0;                  // +0x08 the routine(ftol(cost * Discount * 10))
    unsigned int Group = 0;         // +0x0c the bribe's reputation group
    unsigned int Ids[3] = {};       // +0x10 GetFactionHostileGroups (TU48 the routine)
    float Discount = 0.0f;          // +0x1c the routine(rep, group, 0.6) = 0.6 - feeling
    int TextIds = 0;                // +0x20 FBaseNpcBribe::TextIds: the mad-lib id of the
                                    // offer text (TU34 the routine begin_mad_lib)
};

// A generated random mission and the loader that made it (0x20 bytes): the local of
// at [ESP+0x54] (zeroed at 06ea86ef), filled by TU35 the routine, then the
// body of FBarMissionOffer.
struct FBarMissionOfferData
{
    FRandomMissionLoader* Loader = nullptr;     // +0x00 the routine
    FRandomMissionData* Mission = nullptr;      // +0x04 FRandomMissionLoader::CreateMission
    TString<24> Type;                           // +0x08 the GF_NPC "misn" type

    FBarMissionOfferData()
    {
        Type.Length = 0;
        Type.Data[0] = '\0';
    }
};

// Mission offer (new 0x2c in the routine 06ea8727, vtable 06fb43ec: AddRef, Release,
// deleting dtor over the routine).
class FBarMissionOffer : public FContentRefObject
{
public:
    // (inlined): count 1, Data zeroed, then the routine copies Loader,
    // Mission and Type (strlen clamped to 23). The loader's reference moves in; nothing is
    // AddRef'd.
    explicit FBarMissionOffer(FBarMissionOfferData const* InData);
    // a held loader gets slot 5 (the mission), Release, and is cleared.
    ~FBarMissionOffer() override;

    FBarMissionOfferData Data;                  // +0x08
};

// {kind, object} held by a bar character (8 bytes). Kind 1 = FBarBribeOffer, 2 =
// FBarMissionOffer, 3 = FBaseNpcRumor const*, 4 = FBaseNpcKnow const*; only kinds 1 and 2
// are ref-counted.
enum EBarOfferKind
{
    BAR_OFFER_NONE = 0,
    BAR_OFFER_BRIBE = 1,
    BAR_OFFER_MISSION = 2,
    BAR_OFFER_RUMOR = 3,
    BAR_OFFER_KNOW = 4,
};

struct FBarOffer
{
    int Kind = BAR_OFFER_NONE;                  // +0x00
    union
    {
        FContentRefObject* Object;              // kinds 1, 2
        FBaseNpcRumor const* Rumor;             // kind 3
        FBaseNpcKnow const* Know;               // kind 4
    };                                          // +0x04

    FBarOffer() : Object(nullptr) {}
    // 06eb16b7-06eb16e2 (the copy inside the character copy).
    FBarOffer(FBarOffer const& Other);
    // (thiscall, RET 4): release the old object, copy, AddRef the new one.
    FBarOffer& operator=(FBarOffer const& Other);
    // release, then Kind = 0, Object = 0.
    ~FBarOffer();

    // The shared "release kinds 1 and 2" step of the routine / the routine.
    void ReleaseObject();

    // (thiscall, RET 8; caller BaseSupport msg 0x2c 06eb52c2): the player
    // accepts the offer of Character. Knowledge / bribe: false when the cash is short, else
    // pay (knowledge adds the rumor, a bribe moves the reputation and rebuilds the vendor
    // offers); mission: start it unless the group already runs one; true otherwise.
    bool Accept(FContentPlayerState* State, FBarCharacter* Character);
};

// Input of FContentBase::AddBarCharacter (0xb8 bytes). Ctor the routine; the routine and
// the BasePop builder fill it from a GF_NPC.
struct FBarCharacterDesc
{
    FBarCharacterDesc();

    unsigned int PlayerId;              // +0x00 0, session filter of the character
    unsigned int Affiliation;           // +0x04 not initialised by the ctor
    int IndividualName;                 // +0x08 not initialised by the ctor
    Costume Appearance;                 // +0x0c parts and count zeroed by the ctor
    unsigned int Voice;                 // +0x40 not initialised by the ctor
    unsigned int RoomId;                // +0x44 not initialised by the ctor
    int Placement;                      // +0x48 -1
    TString<32> Role;                   // +0x4c empty
    TString<64> Fidget;                 // +0x70 empty
    ID_String Nickname;                 // +0xb4 cleared
};

// A bar character (0x5c bytes; value of FBaseRoomState::Characters, node 0x64). Built on
// the stack by the routine, copied into the list by the routine, destroyed by
// .
struct FBarCharacter
{
    // 06ea9361-06ea93af (inlined ctor, ++GBarCharacterCount).
    FBarCharacter();
    // member-wise, then ++GBarCharacterCount (06eb16e4).
    FBarCharacter(FBarCharacter const& Other);
    FBarCharacter& operator=(FBarCharacter const& Other) = delete;
    // --GBarCharacterCount, then the offer.
    ~FBarCharacter();

    // (thiscall, RET 4): pub::GF::CharacterCreate from Desc.
    void Create(FBarCharacterDesc const* Desc);
    // (thiscall, RET 4): destroy the old behavior, attach the new one.
    void SetBehavior(unsigned long NewBehavior);
    // (thiscall, RET 4): Offer = NewOffer.
    void SetOffer(FBarOffer const* NewOffer);
    // INC [ECX+0x50] (caller BaseSupport msg 0x1b 06eb4c4d).
    void CountTalk() { ++TalkCount; }

    int UseCount;                  // +0x00 1; one per player visit holding it
    unsigned long Behavior;         // +0x04 -1, pub::GF::CharacterBehaviorCreate handle
    int Field_0x08;                 // +0x08 not written by the ctor or Create.
                                    // TODO: find its writer/readers.
    unsigned int CharacterId;       // +0x0c 0, pub::GF::CharacterCreate
    unsigned int RoomId;            // +0x10 0
    unsigned int Affiliation;       // +0x14 -1
    int Placement;                  // +0x18 -1
    TString<32> Role;               // +0x1c empty
    unsigned int PlayerId;          // +0x40 0
    int Gender;                     // +0x44 -1, pub::GF::GetCostumeSkeletonGender
    unsigned int Voice;             // +0x48 0
    ID_String Nickname;             // +0x4c cleared
    int TalkCount;                  // +0x50 0; ++ by CountTalk (BaseSupport msg 0x1b);
                                    // non-zero picks the bartender scripts 0xd/0xe + 0x15
                                    // (TU34 the routine)
    FBarOffer Offer;                // +0x54
};

typedef std::list<FBarCharacter> FBarCharacterList;

// incremented by the inlined ctor (the routine, the routine) and by the list
// copy the routine (06eb16e4), decremented by the routine. Never read.
extern int GBarCharacterCount;

// (static init the branch, _Multi = 0, atexit the routine): scene id -> the
// base holding the scene. the routine inserts (06ea96dd), the routine erases; no reader
// in the baseline listing (XREF[10]: init/atexit, 06ea96b9, 06eaa025-06eaa0cf).
extern std::map<int, FContentBase*> GBarSceneBases;

// FBaseRoomState::Locks element (0xc, node 0x14): the routine adds / counts.
struct FBarPlacementLock
{
    unsigned int PlayerId;          // +0x00
    int Placement;                  // +0x04
    int Count;                      // +0x08 1 on insert, ++ on a repeat
};

// MBase +0x70 value (0x24 bytes): per-room bar state. Ctor the routine, copy
// , dtor the routine.
struct FBaseRoomState
{
    std::list<FBarPlacementLock> Locks;                     // +0x00
    FBarCharacterList Characters;                           // +0x0c
    std::list<FBarCharacterList::iterator> CharacterRefs;   // +0x18 one per visit

    // (thiscall, RET 8): a Locks entry for (PlayerId, Placement).
    bool HasLock(unsigned int PlayerId, int Placement) const;
    // (thiscall, RET 8): a character of PlayerId on Placement.
    bool HasCharacter(unsigned int PlayerId, int Placement) const;
    // (thiscall, RET 8): ++Count of the entry, or push_back {.., .., 1}.
    void AddLock(unsigned int PlayerId, int Placement);
    // (thiscall, RET 0xc): the character of PlayerId whose Role matches
    // Role->Data (stricmp).
    bool FindCharacter(TString<32> const* Role, unsigned int PlayerId,
                       FBarCharacterList::iterator* OutCharacter);
    // (thiscall, RET 4): characters of PlayerId.
    int CountCharacters(unsigned int PlayerId) const;
};

// Element of the character encounter's +0x10 vector (0x3c bytes; FCharacterEncounter,
// RtcScripts.h, TU23 loader the routine 06ed4a51-06ed4ca7). The bar scene creator
// reads every field below; the routine / the routine compare only Role.Data
// (+4) of their KeepRoles vectors of it.
struct FRtcScriptRole
{
    TString<32> Role;               // +0x00 empty (Length 0) = no fixed character
    // +0x24: the GSpecificNpcs value of the "actor" / "npc" nickname (CreateID); the scene
    // creator copies its costume into the character description (06ea9b6e-06ea9cb4).
    FNpcCharacterDesc const* Character;
    int Spot;                       // +0x28 GetCharacterPlacementByName, -1 initially
    std::string Fidget;             // +0x2c VC6 string, c_str() at 06ea9ce1
};

// MBase +0x98 list element (0x30 bytes, node alloc 0x38): one bar scene -- the characters an
// RTC script put into a room for one player, and the placements it locked there. Created by
// FContentBase::CreateBarScene (the routine), released by the routine and destroyed by
// (the two vector destructors, member-wise here).
struct FBarScene
{
    FContentBase* Base;                                 // +0x00
    int SceneId;                                        // +0x04 the field++
    unsigned int PlayerId;                              // +0x08
    unsigned int RoomId;                                // +0x0c
    std::vector<int> Placements;                        // +0x10 one AddLock each
    std::vector<FBarCharacterList::iterator> Characters; // +0x20 one use each

    // (thiscall, RET 4): unless the first character plays one of KeepRoles,
    // detach its behavior; drop one use of every character (the routine) and one count of
    // every placement lock.
    void Release(std::vector<FRtcScriptRole> const* KeepRoles);
};

// Element of the market lists FContentBase::GetCommoditiesForSale / GetCommoditiesInDemand
// build (8 bytes; the VC6 insert the routine copies both dwords). Consumer: the TU44
// cargo generator the routine (Cargo.cpp).
struct FBaseCommodity
{
    unsigned int GoodId;        // +0x00
    // +0x04: 1 for a good for sale; pub::Market::GetMinInventory for a good in demand.
    unsigned int Count;
};

// MBase, the value of the field (0xb4 bytes). Derives from CommReferrable without
// slots of its own (vtable 06fb4484 = the three CommReferrable import thunks). Ctor
// , copy the routine, destruction the routine (pair dtor), all member-wise.
struct FContentBase : public CommReferrable
{
    // (thiscall, RET 4).
    explicit FContentBase(unsigned int const* InBaseId);

    unsigned int BaseId;                                // +0x48
    int Diff = 10;                                      // +0x4c [mBase] "diff"
    int LoadCount = 0;                                  // +0x50 the routine runs once
    unsigned int LocalFaction = 0xffffffff;             // +0x54 [mBase] "local_faction"
    std::vector<FBaseFaction> Factions;                 // +0x58 [BaseFaction]
    int VendorOffersMin = 0;                            // +0x68 [MVendor] num_offers[0]
    int VendorOffersMax = 0;                            // +0x6c [MVendor] num_offers[1]
    std::map<unsigned int, FBaseRoomState> RoomStates;  // +0x70 key = room id
    std::map<unsigned int, FBaseNpc> Npcs;              // +0x84 [GF_NPC], key = nickname id
    std::list<FBarScene> BarScenes;                     // +0x98 CreateBarScene appends
    std::vector<FBaseRoom> Rooms;                       // +0xa4 [mRoom]

    // (thiscall, RET 4): the rest of this base's mbases.ini section
    // ([MVendor], [GF_NPC], [mRoom] up to the next [mBase]), once per base.
    void Load(INI_Reader* Reader);
    // the last of the Load references drops the room states, the rooms and
    // the GF_NPC records (BaseSupport 06eb4dd8).
    void Unload();
    // (thiscall, RET 4): the [mBase] values and the [BaseFaction] sections
    // that follow it.
    void LoadFactions(INI_Reader* Reader);
    // (thiscall, RET 4): RoomStates.find(*RoomId) -> &value, or null.
    FBaseRoomState* FindRoomState(unsigned int const* RoomId);

    // (thiscall, RET 8): create the character, put it at the front of its
    // room's list (the room state is created on demand) and return it.
    FBarCharacterList::iterator AddBarCharacter(FBarCharacterDesc const* Desc);
    // (thiscall, RET 0xc): drop one use of Character; at zero -- unless its
    // Role equals the Data of one of KeepRoles -- destroy it and erase it from its room.
    // Unused is never read.
    void RemoveBarCharacter(int Unused, FBarCharacterList::iterator Character,
                            std::vector<FRtcScriptRole> const* KeepRoles);
    // (thiscall, RET 0xc; caller TU21 FMissionScript::StartRtcScript
    // ): a new scene id (the field++) registered in GBarSceneBases; the
    // script's room gets a lock per reserved spot and one character per role (an existing
    // one of the same Role and PlayerId is reused), the first of which gets a behavior built
    // from the script (pub::GF::CharacterBehaviorCreate, the third argument its +0x220) while
    // the others without an offer lose theirs; the scene is appended to BarScenes. Returns
    // the scene id.
    int CreateBarScene(FCharacterEncounter const* Script, unsigned int PlayerId,
                       unsigned int ControllerId);
    // (thiscall, RET 8): release and erase the first scene SceneId (callers
    // TU21 06eb8bbe, 06eb9f35, 06eba62b, 06eba9db), then erase SceneId from
    // GBarSceneBases whether or not a scene was found.
    void RemoveBarScene(int SceneId, std::vector<FRtcScriptRole> const* KeepRoles);
    // (thiscall, RET 0xc): the character with this pub::GF id, and its room.
    bool FindBarCharacter(FBarCharacterList::iterator* OutCharacter,
                          FBaseRoomState** OutRoomState, unsigned int CharacterId);
    // SystemInfo of the base's system (pub::GetSystem), null on failure.
    FSystemInfo* GetSystemInfo() const;
    // (thiscall, RET 4): GameSupport msg 1 -- one character per room fixture
    // for this player, each with an offer and a script.
    void OnPlayerEnter(unsigned int PlayerId);
    // (thiscall, RET 4): GameSupport msg 2 -- drop the player's visits.
    void OnPlayerExit(unsigned int PlayerId);
    // (thiscall, RET 4): nickname id and position of the first object of the
    // base's system whose dock_with is this base (0 when none). Callers TU27 06ef1cff,
    // TU61 06f8fbb4.
    unsigned int FindDockObject(Vector* OutPosition) const;
    // (thiscall, RET 4): character_density of a room, 0 when not listed.
    int GetRoomDensity(unsigned int RoomId) const;
    // (thiscall, RET 4; callers TU44 06f3612e / 06f365fc / 06f369f8; added by
    // CNT-POP7): Out gets {good, 1} for every commodity pub::Market sells at BaseId; with none,
    // Out is emptied (otherwise appended to).
    void GetCommoditiesForSale(std::vector<FBaseCommodity>* Out) const;
    // (thiscall, RET 4; callers TU44 06f361f0 / 06f3639b / 06f36a92; added by
    // CNT-POP7): Out gets {good, pub::Market::GetMinInventory} for every commodity in demand
    // at BaseId; with none, Out is emptied (otherwise appended to).
    void GetCommoditiesInDemand(std::vector<FBaseCommodity>* Out) const;
    // (thiscall, RET 4; callers TU53 06f6cb51, TU37 06f06260 / 06f06300): the
    // first [BaseFaction] of Group, or null.
    FBaseFaction* FindFaction(unsigned int Group);
};

// (cdecl): the field.find(*BaseId) -> &value, or null.
FContentBase* FindContentBaseInfo(unsigned int const* BaseId);
// (cdecl): one record per pub::NextBaseID base, then missions\mBases.ini.
void __cdecl BuildContentBases();
// (cdecl): GameSupport message 0x40 -- rebuild the table, then the
// [DynBaseInfo] sections of the save.
void __cdecl LoadDynBaseInfo(INI_Reader* Reader);
// (cdecl): empty the table (UnloadContentGameTables).
void __cdecl UnloadContentBases();
// (cdecl): GameSupport message 0x41 -- walks the table and writes nothing.
void __cdecl SaveContentBases(FILE* File);
