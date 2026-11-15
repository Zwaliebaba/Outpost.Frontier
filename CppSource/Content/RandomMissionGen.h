// Author: Giperion.

#pragma once

#include "RandomMissionDB.h"        // TRmRef (the routine's by-value offer holder)
#include "RandomMissionRuntime.h"
#include "common_id.h"
#include "common_server.h"

#include <cstdio>
#include <list>
#include <map>
#include <vector>

class INI_Reader;
struct FmtStr;
struct FRmSolarFormation;
struct FSystemInfo;

// Random-mission generator (implementation notes, TU map, lane CNT-RM): TU63 06f97360-06fa75c0.
// Ported: the generator root object (the field: ctor the routine = all RandomMissions
// table loads, dtor the routine), the vignette node tree (VignetteParams.ini load
// , link the routine, difficulty windows the routine), NpcRankToDiff /
// Diff2Money tables, the "all groups" enumerator, the voice-properties database, the solar
// formation pick, the offer walk (offer node, tree paths, path filters) and the building of
// the TU61 offer (BuildOffer, BuildOfferFromPath, the filters' CreateOffer; the offer
// classes are RandomMissionOffer.h).

// TU63 -- random pick among the solar formations (the field) whose mandatory slot types
// are all in SpawnTypes and whose mandatory + optional types cover SpawnTypes. Callers:
// (06f9ad97), the routine (06f9bf98).
FRmSolarFormation* __cdecl PickSolarFormation(std::vector<int> const* SpawnTypes);

// TU63 the routine (cdecl): the zone shown as TARGET_ZONE for Position in System -- of the
// named zones within 1.0 of it (pub::System::InZones, at most 0x14) the one with the smallest
// radius; with none, the navmap grid cell (row + column * 8 + 1). Caller the branch.
unsigned int __cdecl GetRmZoneAtPosition(FSystemInfo const* System, Vector const* Position);

// ---------------------------------------------------------------------------------------
// TU63 -- NPC rank -> difficulty per formation size (RandomMissions\NpcRankToDiff.ini).
// map<NPC rank, difficulties>, static init the branch, atexit the routine.
// ---------------------------------------------------------------------------------------
extern std::map<int, std::vector<float>> GRmNpcRankToDifficulty;

void __cdecl LoadNpcRankToDifficulty(INI_Reader* Reader);

// ---------------------------------------------------------------------------------------
// TU63 -- difficulty -> money (RandomMissions\Diff2Money.ini, read inline by the
// generator ctor). the field: map<difficulty, money>, static init the branch,
// atexit the routine; unique insert the routine.
// ---------------------------------------------------------------------------------------
extern std::map<float, int> GRmDiff2Money;

unsigned int __cdecl RmDifficultyToMoney(float Difficulty);

// TU44 the routine: keep the two leading decimal digits of an amount above 100 and round
// the third to 0 or 5 in the second place's unit (called by the routine).
unsigned int __cdecl RoundRmMoney(unsigned int Amount);

// ---------------------------------------------------------------------------------------
// TU63 -- every reputation group, collected once for "offer_group = all" /
// "hostile_group = all" (the routine). Object the field (vtable 06fbcd4c, 0x18 bytes),
// ctor the routine (static init the branch), dtor the routine (atexit the routine);
// the ctor publishes its address in the field, the dtor clears it.
// ---------------------------------------------------------------------------------------
struct FRmAllGroupsEnumerator : public pub::Reputation::Enumerator
{
    bool bEnumerated;                           // +0x04 set by the caller after EnumerateGroups
    std::vector<unsigned int> Groups;           // +0x08 (alloc byte) / +0x0c..+0x14

    FRmAllGroupsEnumerator();
    ~FRmAllGroupsEnumerator();
    char Execute(unsigned int GroupId) override;
};

extern FRmAllGroupsEnumerator* GRmAllGroups;

// ---------------------------------------------------------------------------------------
// TU63 -- difficulty window (baseline 0xc bytes, vtable 06fbd3f0):
//   [0] scalar deleting dtor the branch, [1] IsInRange the branch,
//   [2] Overlaps the branch, [3] Parse the branch, [4] Set the branch.
// ctor(min, max) the routine, copy ctor the routine.
// ---------------------------------------------------------------------------------------
struct FRmDifficultyWindow
{
    float Min;                                  // +0x04
    float Max;                                  // +0x08

    FRmDifficultyWindow();                      // inline at the routine (0, 0)
    FRmDifficultyWindow(float InMin, float InMax);
    virtual ~FRmDifficultyWindow();
    virtual bool IsInRange(float Difficulty) const;
    virtual bool Overlaps(FRmDifficultyWindow const* Other) const;
    virtual void Parse(INI_Reader* Reader);
    virtual void Set(float InMin, float InMax);

    bool Equals(FRmDifficultyWindow const* Other) const;
    void Add(FRmDifficultyWindow const* Other);
};

// TU63 -- list of difficulty windows (baseline 0x10 bytes, vtable 06fbd50c: [0] scalar
// deleting dtor the routine / dtor the routine; ctor the routine). The list nodes hold
// FRmDifficultyWindow by value (0x14-byte nodes, element dtor through its vtable).
struct FRmDifficultyWindowList
{
    std::list<FRmDifficultyWindow> Windows;     // +0x04 (alloc byte) / +0x08 head, +0x0c size

    virtual ~FRmDifficultyWindowList();
    void AddUnique(FRmDifficultyWindow const* Window);
    void PushBack(FRmDifficultyWindow const* Window);
};

// TU63 -- one "offer_text" line entry (baseline 0x18 bytes).
struct FRmOfferTextEntry
{
    int StrId;                                  // +0x00
    int Form;                                   // +0x04 singular 1, plural 2, default 3
    std::vector<int> Params;                    // +0x08 (alloc byte) / +0x0c..+0x14
                                                //       GRmTextParamNames values

    FRmOfferTextEntry();
};

// TU63 -- "offer_text" value (baseline 0x18 bytes, vtable 06fbcf08: [0] Parse the routine,
// [1] IsEmpty the branch, [2] Build the routine). No virtual dtor: the owning vector
// runs the routine -> the routine.
class IRmTextParamSource;                       // RandomMissionOffer.h (the TU61 offers)
struct FRmOfferText
{
    bool bAppend;                               // +0x04 "append" 1, "replace" 0
    std::vector<FRmOfferTextEntry> Entries;     // +0x08 (alloc byte) / +0x0c..+0x14

    FRmOfferText();
    virtual void Parse(INI_Reader* Reader);
    virtual bool IsEmpty() const;
    // Slot 2, the routine (RET 0x10): per entry whose Form passes (see the .cpp), a local
    // FmtStr is built (begin_mad_lib(StrId), Source slot 0 per parameter, end_mad_lib) and
    // appended to *Out (FmtStr::append_fmt_str). The two flags are the offer group's and the
    // hostile group's FFactionProps::bSingularNickname (the only caller, the routine
    // / 06f9e42a-06f9e445).
    virtual void Build(bool bOfferGroupSingular, bool bHostileGroupSingular, FmtStr* Out,
                       IRmTextParamSource* Source);
};

// TU63 -- "objective_text" values (baseline 0x18 bytes, vtable 06fbd510: [0] Parse
// ). map<objective, text> at +0x04 (ctor the routine, insert the routine,
// signed key compare).
struct FRmObjectiveText
{
    std::map<int, FRmParamText> Texts;          // +0x04

    virtual char Parse(INI_Reader* Reader, unsigned int Index);
};

// TU63 -- one line of a comm sequence (baseline 0x48 bytes; vector insert the routine).
struct FRmCommLine
{
    int Speaker;                                // +0x00 GRmCommTargetNames value, default 1
    unsigned int Length;                        // +0x04
    char Text[0x40];                            // +0x08 at most 0x3f characters

    FRmCommLine();
};

// TU63 -- "comm_sequence" value (baseline 0x24 bytes; element dtor the routine ->
// ).
struct FRmCommSequence
{
    int Trigger;                                // +0x00 GRmCommTriggerNames value
    int Target;                                 // +0x04 GRmCommTargetNames value, default 1
    float Field_0x08;                           // +0x08 value 2 TODO: meaning unknown
    // +0x0c / +0x10 values 3 / 4: the conversation's delay is MinDelay + (MaxDelay - MinDelay)
    // * rand() / 32768 (the routine 06f9e82c-06f9e864).
    float MinDelay;                             // +0x0c
    float MaxDelay;                             // +0x10
    std::vector<FRmCommLine> Lines;             // +0x14 (alloc byte) / +0x18..+0x20

    FRmCommSequence();
    void Parse(INI_Reader* Reader);
};

// TU63 -- per-node parameters (baseline 0xd0 bytes, vtable 06fbd554: [0] scalar deleting
// dtor the routine / dtor the routine, [1] ParseKey the routine). ctor the routine,
// copy ctor the routine (member-wise).
struct FRmVignetteParams
{
    float Weight;                               // +0x04 0 in the ctor; 0 -> 1.0 after a node
    std::vector<FRmOfferText> OfferTexts;       // +0x08 (alloc byte) / +0x0c..+0x14
    FRmObjectiveText ObjectiveText;             // +0x18
    FRmParamText FailureText;                   // +0x30
    FRmParamText RewardText;                    // +0x60
    FRmDifficultyWindow Difficulty;             // +0x90
    std::vector<unsigned int> OfferGroups;      // +0x9c (alloc byte) / +0xa0..+0xa8
    std::vector<unsigned int> HostileGroups;    // +0xac (alloc byte) / +0xb0..+0xb8
    std::vector<FRmCommSequence> CommSequences; // +0xbc (alloc byte) / +0xc0..+0xc8
    unsigned int AllowableZoneTypes;            // +0xcc open 1, field 2, exclusion 4;
                                                //       default 7

    FRmVignetteParams();
    virtual ~FRmVignetteParams();
    virtual char ParseKey(INI_Reader* Reader);
};

// TU63 -- one [DecisionNode] / [DocumentationNode] / [DataNode] of VignetteParams.ini
// (baseline 0x114 bytes, vtable 06fbd550: [0] Load the branch -- no virtual dtor).
// Default ctor inline at the routine / the routine; copy ctor the routine.
struct FRmVignetteNode
{
    ID_String Nickname;                         // +0x04 CreateID("nickname"/"documentation")
    unsigned int NodeId;                        // +0x08 -1
    std::vector<unsigned int> Parents;          // +0x0c (alloc byte) / +0x10..+0x18
                                                //       filled by the routine
    std::vector<unsigned int> Children;         // +0x1c (alloc byte) / +0x20..+0x28
    bool bImplemented;                          // +0x2c 1
    FRmDifficultyWindowList Windows;            // +0x30 filled by the routine
    FRmVignetteParams Params;                   // +0x40
    float FirstChildChance;                     // +0x110 -1.0: chance of the first child
                                                //       on a path (the routine); -1 means
                                                //       1 / child count. No loader writes it.

    FRmVignetteNode();
    virtual void Load(INI_Reader* Reader);
    bool ParseCommonKey(INI_Reader* Reader);
};

// ---------------------------------------------------------------------------------------
// TU63 -- offer request (baseline 0x34 bytes, ctor the routine). Filled by the random
// mission (the routine) and checked by FRandomMissionGenerator::BuildOffer.
// ---------------------------------------------------------------------------------------
struct FRmOfferQuery
{
    int OfferGroup;                             // +0x00 -1
    unsigned int EnemyGroup;                    // +0x04 -1
    int Reward;                                 // +0x08 0 (signed compare in 06f9f714)
    float RepReward;                            // +0x0c 0
    float RepHit;                               // +0x10 0
    unsigned int OfferBase;                     // +0x14 0
    float Difficulty;                           // +0x18 the field (0.0)
    unsigned int ObjectiveZone;                 // +0x1c 0
    Vector ObjectivePos;                        // +0x20 0
    unsigned int PlayerId;                      // +0x2c 0
    unsigned int DestSys;                       // +0x30 0

    FRmOfferQuery();
};

// ---------------------------------------------------------------------------------------
// TU63 -- one decision path from the root of the vignette tree (baseline 0x14 bytes, vtable
// [0] Save the branch, [1] Load the routine; built inline at 06ef0658,
// , 06fa4ff5). One byte per decision: 1 = first child, 0 = second child. The random
// mission keeps one at +0x68 and saves it as "tree_path = TFFT...".
// ---------------------------------------------------------------------------------------
struct FRmTreePath
{
    std::vector<char> Bits;                     // +0x04 (alloc byte) / +0x08..+0x10

    virtual void Save(FILE* File);
    virtual void Load(INI_Reader* Reader);
};

// TU61 random-mission offers (RandomMissionOffer.h): the base offer and the vignette offer
// the destroy-solars and assassinate filters create.
class FRmOffer;
class FRmVignetteOffer;
class FRmSolarComponent;
class FRmShipComponent;
class FRmTargetShipComponent;
struct FRmKillableSolar;
struct FRmSolarSpawn;

// TU63 -- the formation size a ship block gets (the three dwords the routine writes at the
// block's +0x08).
struct FRmShipSize
{
    float Difficulty;                           // +0x00 the NpcRankToDiff entry of Count
    int Level;                                  // +0x04 the NPC level
    int Count;                                  // +0x08 the number of ships; 0 when none
};

// TU63 -- a ship block (0x20-byte stack record of the routine / the routine), read by
// . Field meanings are open: the names say what writes and reads them.
struct FRmShipBlock
{
    unsigned char Field_0x00;                   // -> ship group +0x00 (pk flag)
    // non-zero -> the first ship's +0xd8 = 2, the others' 0, and the first
    // ship's +0xdc = 2 (its random name off, +0xd4 = the caller's last argument).
    unsigned char Field_0x01;
    unsigned char Field_0x02;                   // +0xd8 = 2 for every ship; pk flag
    unsigned char Field_0x03;                   // -> ship group +0x02
    // non-zero -> the first ship carries a critical loot (the routine of
    // Field_0x14); 0 -> a bonus loot when the drop chance roll succeeds.
    unsigned char Field_0x04;
    FRmShipSize Size;                           // +0x08 the routine
    int Field_0x14;                             // critical loot (the routine)
    int Field_0x18;                             // -> ship group +0x04
    int Field_0x1c;                             // -> ship group +0x08
};

// TU63 the routine (cdecl): the formation size for Difficulty among the class_fighter
// npc_ship archs of Faction -- per NPC level 1..25 the NpcRankToDiff count whose difficulty
// is closest to Difficulty, then a weighted pick by count (single-player weights
// , multiplayer the field). Nothing picked: only Out->Count = 0.
void __cdecl PickRmShipFormationSize(FRmShipSize* Out, float Difficulty, unsigned int Faction);

struct FRmShipGroup;
struct FRmLootSpawn;
struct FNpcNameParams;

// TU63 the routine (cdecl): Size.Count ships of GroupId into Group->Ships, at most one per
// slot of the faction's "fighters" formation -- a class_fighter / "D<level>" npc_ship arch,
// named "Vignette_ship_%X". The first ship gets Loot (when given) and the First* values, the
// others the Other* values. bMatchLevel rejects a level 0 or an arch more than one level off.
bool __cdecl BuildRmShips(FRmShipGroup* Group, unsigned int GroupId, FRmShipSize Size,
                          int FirstField_0xd8, int OtherField_0xd8, int FirstField_0xdc,
                          int OtherField_0xdc, FRmLootSpawn const* Loot, FNpcNameParams* TargetName,
                          bool bMatchLevel);

// TU63 the routine (cdecl): a new ship group of the block's size in Component (the offer
// group's when bFriendly, else the hostile group's) at *Position, with an optional loot for
// its first ship; the result of the routine.
bool __cdecl BuildRmShipGroup(FRmOfferQuery const* Query, FRmShipComponent* Component,
                              FRmShipBlock const* Block, Vector const* Position,
                              bool bFriendly, FNpcNameParams* TargetName);

// ---------------------------------------------------------------------------------------
// TU63 -- path filter. FRandomMissionGenerator::BuildOfferFromPath (the routine) starts
// with a base filter, feeds it every node of the chosen path and swaps it for the object
// Specialize returns once a mission type was recognised; the last filter creates the
// offer. Filters have no virtual destructor: baseline frees them with plain operator delete
// (the routine), so every filter class stays trivially destructible.
// Slots: [0] Slot00, [1] HasMissionType, [2] ApplyNode, [3] Specialize, [4] CreateOffer,
// [5] SupportsTextParam.
// ---------------------------------------------------------------------------------------
struct FRmPathFilter                            // baseline 0x0c, vtable 06fbcf6c
{
    bool bRandomMission;                        // +0x04 node "random_mission"
    bool bDestroySolars;                        // +0x05 node "destroy_solars_mission"
    bool bAssassinate;                          // +0x06 node "assassinate_mission"
    bool bDestroyShips;                         // +0x07 node "destroy_ships_mission"
    bool bLootItem;                             // +0x08 node "loot_item_mission"

    FRmPathFilter();                            // inline at 06f9e9e0
    virtual int Slot00() const;                 // 0. TODO: name unknown
    virtual bool HasMissionType() const;
    virtual bool ApplyNode(FRmVignetteNode const* Node, char Bit);
    virtual FRmPathFilter* Specialize() const;
    virtual bool CreateOffer(FRmOfferQuery const* Query, FRmOffer** OutOffer);
    virtual bool SupportsTextParam(int Param) const;
};

// Destroy-solars filter (baseline 0x1c, vtable 06fbd034; built by the routine with the base
// flags copied and its own flags cleared). Each flag is set by the path node of that name.
struct FRmDestroySolarsFilter : FRmPathFilter
{
    bool bBigSolar;                             // +0x0c "big_solar"
    bool bPkBigSolar;                           // +0x0d "pk_big_solar"
    bool bDefensiveSolars;                      // +0x0e "defensive_solars"
    bool bPkDefensiveSolars;                    // +0x0f "pk_defensive_solars"
    bool bCavalry;                              // +0x10 "cavalry"
    bool bHostileShips;                         // +0x11 "hostile_ships"
    bool bPkHostileShips;                       // +0x12 "pk_hostile_ships"
    bool bHostileShipsAtInstallation;           // +0x13 "hostile_ships_at_installation"
    bool bHostileShipsAfter30S;                 // +0x14 "hostile_ships_after_30_s"
    bool bHostileWaves;                         // +0x15 "hostile_waves"
    bool bFriendlyShips;                        // +0x16 "friendly_ships"
    bool bFriendlyShipsAtInstallation;          // +0x17 "friendly_ships_at_installation"
    bool bFriendlyShipsAfter30S;                // +0x18 "friendly_ships_after_30_s"
    bool bFriendlyWaves;                        // +0x19 "friendly_waves"

    FRmDestroySolarsFilter();                   // inline at 06f9a2aa / 06f9a6aa
    bool HasMissionType() const override;       // bCavalry
    bool ApplyNode(FRmVignetteNode const* Node, char Bit) override;
    FRmPathFilter* Specialize() const override;
    bool CreateOffer(FRmOfferQuery const* Query, FRmOffer** OutOffer) override;
    bool SupportsTextParam(int Param) const override;
    // Slot 6 (+0x18), new in this class (vtables 06fbd034 and 06fbd164, raw dword after
    // slot 5): the branch = MOV EAX,1; RET. TODO: name -- no +0x18 call on a filter found
    // yet in TU63 (06f9a700 / 06f9ac80 / 06f9af40 / 06f9e1f0 checked).
    virtual int Slot06() const;

    // (thiscall, RET 8): the solars of the offer -- position, activation, the
    // solar formation and a KillableSolars entry per solar type; false when either is
    // missing.
    bool FillSolarSpawns(FRmOfferQuery const* Query, FRmSolarComponent* Solars) const;
    // (thiscall, RET 0x18): one solar per matching formation slot and entry
    // until the entries' scaled difficulties reach Difficulty.
    void SpawnSolars(FRmOfferQuery const* Query, unsigned int EnemyGroup, float Difficulty,
                     FRmSolarFormation* Formation,
                     std::vector<std::pair<FRmKillableSolar*, float>> const* Entries,
                     std::list<FRmSolarSpawn>* OutSolars) const;
};

// Destroy-solars filter after a "cavalry" node (baseline 0x1c, vtable 06fbd164; built by
// with every flag cleared): slots 1/2 are the routine / the routine (always
// false), so no later node changes it; 3/4/5 are the destroy-solars ones.
struct FRmCavalryFilter : FRmDestroySolarsFilter
{
    bool HasMissionType() const override;
    bool ApplyNode(FRmVignetteNode const* Node, char Bit) override;
};

// Assassinate filter (baseline 0x28, vtable 06fbd01c; built by the routine with the base
// flags copied, ctor the routine clears +0x0c..+0x26). Flags[i] is set by the path node
// named GRmAssassinateNodeNames[i] (the branch).
struct FRmAssassinateFilter : FRmPathFilter
{
    bool Flags[27];                             // +0x0c..+0x26

    FRmAssassinateFilter();
    int Slot00() const override;                // Flags[0] ? 2 : 3
    bool HasMissionType() const override;
    bool ApplyNode(FRmVignetteNode const* Node, char Bit) override;
    FRmPathFilter* Specialize() const override; // null
    bool CreateOffer(FRmOfferQuery const* Query, FRmOffer** OutOffer) override;
    bool SupportsTextParam(int Param) const override;

    // (thiscall, RET 0x10): the target's ship groups at a random point of a ring
    // around the objective (pre-battle or main-battle wave, the target group itself carrying
    // TargetName); +0x50 / +0x54 of the component are set at the end.
    bool FillTargetShips(FRmOfferQuery const* Query, FRmTargetShipComponent* TargetShips,
                         float Share, FNpcNameParams* TargetName) const;
    // (thiscall, RET 0x14): the defensive solars (the routine) and the hostile /
    // friendly ship groups of the main battle at the objective.
    bool FillShipGroups(FRmOfferQuery const* Query, FRmShipComponent* Ships, float Share,
                        FRmSolarComponent* Solars, FNpcNameParams* TargetName) const;
    // (thiscall, RET 8): 1..3 defensive solars (type 1) at the objective; false
    // without a formation or a KillableSolars entry.
    bool FillDefensiveSolars(FRmOfferQuery const* Query, FRmSolarComponent* Solars) const;
    // (thiscall, RET 0x18): FRmDestroySolarsFilter::SpawnSolars (the routine)
    // without the pk modes -- every solar's +0x4d8 is 0.
    void SpawnSolars(FRmOfferQuery const* Query, unsigned int EnemyGroup, float Difficulty,
                     FRmSolarFormation* Formation,
                     std::vector<std::pair<FRmKillableSolar*, float>> const* Entries,
                     std::list<FRmSolarSpawn>* OutSolars) const;
};

extern char const* const GRmAssassinateNodeNames[27];

// Destroy-ships filter (baseline 0x0c, vtable 06fbd004) and loot-item filter (baseline 0x0c,
// vtable 06fbcfec): both built by the routine with every flag cleared. Slots 1 and 2 are
// / the routine (always false); 0, 3, 4 and 5 are the base ones. Their
// offers are created by the base CreateOffer, i.e. never.
struct FRmDestroyShipsFilter : FRmPathFilter
{
    bool HasMissionType() const override;
    bool ApplyNode(FRmVignetteNode const* Node, char Bit) override;
};

struct FRmLootItemFilter : FRmPathFilter
{
    bool HasMissionType() const override;
    bool ApplyNode(FRmVignetteNode const* Node, char Bit) override;
};

// ---------------------------------------------------------------------------------------
// TU63 -- the random-mission generator root. One static instance the field (static init
// , atexit the branch); its address is published in the field.
// vtable 06fbd4fc: [0] scalar deleting dtor the routine, [1] Load the routine
// (VignetteParams.ini), [2] Unload the branch, [3] BuildOffer the routine (build an
// offer; caller the routine at 06ef0a5c).
// ---------------------------------------------------------------------------------------
struct FRandomMissionGenerator;

// TU63 -- the generator's publishing sub-object (no storage). Its construction is the first
// thing the routine does (06f9d57a: the field = this, before any member); its destructor
// (the field = 0) is inlined last into the routine
// (06f9da41, after the members) and called by the unwind funclets 06fb19e9 / 06fb1a72 when
// the ctor or dtor throws.
struct FRmGeneratorRegistration
{
    explicit FRmGeneratorRegistration(FRandomMissionGenerator* Generator);
    ~FRmGeneratorRegistration();
};

struct FRandomMissionGenerator : private FRmGeneratorRegistration
{
    bool bLoaded;                               // +0x04 set at the end of Load
    bool bLinked;                               // +0x05 set at the end of the routine
    unsigned int RootNodeId;                    // +0x08 -1 in ctor/dtor; the routine
    std::vector<unsigned int> LeafNodeIds;      // +0x0c (alloc byte) / +0x10..+0x18
                                                //       implemented nodes without children
    std::map<unsigned int, FRmVignetteNode> Nodes; // +0x1c map<node id, node>
                                                //       (insert the routine, unsigned keys)

    FRandomMissionGenerator();
    virtual ~FRandomMissionGenerator();         // / sdtor the routine
    virtual void Load(INI_Reader* Reader);
    // Slot 2, the branch: +0x04 / +0x05 = 0, RootNodeId = -1, the leaf ids and the node map
    // emptied (the map's inline clear: the routine subtree erase with the node dtor
    // , or the one-by-one erase the routine path).
    virtual void Unload();
    // Slot 3, the routine (called through the vtable by the routine at 06ef0a5c).
    virtual bool BuildOffer(FRmOfferQuery const* Query, FRmOffer** OutOffer,
                            FRmTreePath* OutPath);

    void Link();
    void DisableChildren(std::vector<unsigned int> const* ChildIds);
    void PropagateDifficultyWindows(unsigned int NodeId);

    // Offer construction helpers of BuildOffer (slot 3 above).
    unsigned int PickOfferNode(FRmOfferQuery const* Query);
    bool ChooseTreePath(FRmTreePath* OutPath, unsigned int OfferNodeId, float Difficulty,
                        int OfferGroup);
    void CollectTreePaths(std::vector<FRmTreePath>* OutPaths, unsigned int NodeId,
                          FRmDifficultyWindow Window, int OfferGroup);
    FRmTreePath* PickTreePath(std::vector<FRmTreePath>* Paths);
    bool BuildOfferFromPath(FRmTreePath const* Path, FRmOfferQuery const* Query,
                            FRmOffer** OutOffer);
    // (RET 8): the comm sequences of every node on Path go into the vignette
    // offer's General component; the holder argument is released on return.
    void BuildCommSequences(FRmTreePath const* Path, TRmRef<FRmOffer> Offer);
    // (RET 0x14; caller the routine 06f9ec23): the offer text, the reward and
    // failure texts and the objective texts of every node on Path, then the objectives
    // (the routine). Source is the caller's *OutOffer read before the call.
    void BuildOfferTexts(FRmOfferQuery const* Query, FRmTreePath const* Path,
                         FRmPathFilter* Filter, FRmOffer* Source, FRmOffer** OutOffer);
    // (RET 0x10; caller the routine 06f9e524): one TU61 objective per objective
    // text into the vignette offer's component maps. this and Filter are not read.
    void BuildObjectives(FRmOfferQuery const* Query, FRmOffer** OutOffer,
                         FRmObjectiveText const* Texts, FRmPathFilter* Filter);
};

extern FRandomMissionGenerator* GRandomMissionGenerator;

// ---------------------------------------------------------------------------------------
// TU63 -- voice properties (missions\voice_properties.ini and the audio\voices_*.ini files).
// vector of 0x1c-byte records, static init the branch, atexit the routine
// (record dtor the routine).
// ---------------------------------------------------------------------------------------
struct FVoiceProperties                             // baseline 0x1c bytes
{
    unsigned int VoiceId;                           // +0x00 pub::GetVoiceID("voice")
    unsigned int Flags;                             // +0x04 gender: male 1, female 2;
                                                    //       supports_roles: combat_lev_1 0x4000,
                                                    //       combat_lev_2 0x4, combat_lev_3 0xc004,
                                                    //       scan 0x8, hail_hailee 0x10,
                                                    //       hail_dock 0x20, dockrequest_dock 0x40,
                                                    //       dockrequest_docker 0x80, behavior 0x100,
                                                    //       misc_chatter 0x200, fidget 0x400,
                                                    //       combat_dock 0x800,
                                                    //       random_mission_ship 0x1000,
                                                    //       mission_commission 0x2000
    std::map<unsigned int, int> PermutationCounts;  // +0x08 nickname id -> permutation count

    int GetPermutationCount(unsigned int const* NicknameId) const;
    bool SupportsRoles(unsigned int Mask) const;
};

extern std::vector<FVoiceProperties> GVoiceProperties;

void __cdecl LoadVoiceProperties(INI_Reader* Reader);
FVoiceProperties* __cdecl FindVoiceProperties(unsigned int const* VoiceId);
