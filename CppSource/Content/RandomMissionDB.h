// Author: Giperion.

#pragma once

#include "x86math.h"

#include <list>
#include <map>
#include <vector>

class INI_Reader;

// Random-mission static databases (implementation notes, TU map, lane CNT-RM):
//   TU47 06f4a780-06f4ed80  rank -> difficulty table (missions\RankDiff.ini)
//   TU51 06f57cb0-06f5a2e0  ship classes (missions\shipclasses.ini), per-ship mission
//                           properties (ships\shiparch.ini), solar formations
//                           (RandomMissions\SolarFormations.ini)
// Every loader takes an already opened INI_Reader, exactly as baseline: the caller runs
// OpenContentDataIni (the routine) and closes the reader afterwards.

// ---------------------------------------------------------------------------------------
// The the routine holder: baseline's releasing-handle dtor for objects whose ref-counted
// interface sits at +4 -- the random mission (FRmMissionDataBase) and the TU61 offer: if set,
// (p + 4)->Release(), then null. Users: the TU47 mission object's +0x1f8 (Unwind@06faefe5 /
// ), the DestroyMission +0x240 (Unwind@06fabe08 / 06fabe68), the offer local of
// (Unwind@06fac000). Holders released through the routine (interface at +0,
// e.g. an [ObjList] directive) use the shared TRefHandle (RefHandle.h) instead.
// RmRefAddRef / RmRefRelease overloads next to each held type (RandomMission.h) pick its +4
// interface (found by argument-dependent lookup at instantiation).
// ---------------------------------------------------------------------------------------
template <typename T>
class TRmRef
{
public:
    TRmRef() = default;
    // Store, then AddRef when non-null (the member inits 06f4b144-06f4b152 and
    // ).
    explicit TRmRef(T* InObject)
        : Object(InObject)
    {
        if (Object != nullptr)
            RmRefAddRef(Object);
    }
    // Copy + AddRef when set: the by-value holder argument of the routine (06f9ec46-06f9ec61),
    // released by the callee.
    TRmRef(TRmRef const& Other)
        : Object(Other.Object)
    {
        if (Object != nullptr)
            RmRefAddRef(Object);
    }
    TRmRef& operator=(TRmRef const&) = delete;
    // .
    ~TRmRef()
    {
        Reset();
    }

    // called directly (e.g. the routine 06ef1c85 on the loader's mission).
    void Reset()
    {
        if (Object != nullptr)
        {
            RmRefRelease(Object);
            Object = nullptr;
        }
    }

    // The holder's address as an out-parameter: the callee stores a pointer whose reference
    // the holder then owns (no AddRef), e.g. BuildOffer(&Query, &Offer, ...) in the routine.
    T** GetAddress()
    {
        return &Object;
    }

    T* Get() const
    {
        return Object;
    }

    T* operator->() const
    {
        return Object;
    }

private:
    T* Object = nullptr;
};

// {name, value} row of the baseline .data name tables (PTR_s_big_solar_06fce748 and the
// other 4-entry tables of TU51/TU62).
struct FRmNamedValue
{
    char const* Name;
    int Value;
};

// .data PTR_s_big_solar_06fce748: big_solar 0, defensive_solar 1, killable_neutral 2,
// background_solar 3. One baseline object, shared by TU51 and TU62.
extern FRmNamedValue const GRmSolarTypes[4];

// ---------------------------------------------------------------------------------------
// TU47 -- rank -> difficulty (the field: VC6 vector of 8-byte {int, float} records,
// static init the branch, atexit the routine).
// ---------------------------------------------------------------------------------------
struct FRmRankDifficulty
{
    int Stage;          // +0x0 story stage number (GContentStageNames value)
    float Difficulty;   // +0x4 clamped to [0, 100]
};

extern std::vector<FRmRankDifficulty> GRmRankDifficulties;

void __cdecl LoadRankDifficulties(INI_Reader* Reader);
void __cdecl ClearRankDifficulties();

struct FContentPlayerState;

// a random difficulty in [MinDifficulty, MaxDifficulty] for Player's offer
// (TU35 the routine). Multiplayer: the top may grow by 1.3689^(2|4|6) and is clamped to
// [0, 100]. Single player: narrowed to the window around the difficulty of the player's
// stage (MissionNumber in GRmRankDifficulties); 0.0 when the window lies at or below
// MinDifficulty.
float __cdecl PickRmMissionDifficulty(FContentPlayerState const* Player, float MinDifficulty,
                                      float MaxDifficulty);

// ---------------------------------------------------------------------------------------
// TU47 -- systems loaded for random missions. the field/the field: std::list of
// system ids (head, size), static init at 06f4d540, atexit at 06f4d570. Each listed system
// holds one FSystemInfo::Load(false) reference until ReleaseRmDestSystems.
// ---------------------------------------------------------------------------------------
extern std::list<unsigned int> GRmDestSystems;

void __cdecl RegisterRmDestSystem(unsigned int SystemId);
void __cdecl ReleaseRmDestSystems();

// ---------------------------------------------------------------------------------------
// TU49 -- random-mission bonus loot (RandomMissions\RMLootInfo.ini).
// map<reputation group id (0xffffffff = "all"), records>, static init
// , atexit the routine. the field: [Global] BonusLootDropChance.
// ---------------------------------------------------------------------------------------
struct FRmBonusLootEntry            // baseline 0x18 bytes
{
    unsigned int Archetype;     // +0x00 pub::GetEquipmentID
    int NumToDropMin;           // +0x04
    int NumToDropMax;           // +0x08 = min when the second value is empty
    float DifficultyMin;        // +0x0c
    float DifficultyMax;        // +0x10
    float Weight;               // +0x14 get_value_int converted to float
};

extern std::map<unsigned int, std::vector<FRmBonusLootEntry>> GRmBonusLoot;
extern float GRmBonusLootDropChance;

void __cdecl LoadRmLootInfo(INI_Reader* Reader);
void __cdecl LoadRmBonusLootSection(INI_Reader* Reader);
float __cdecl GetRmBonusLootDropChance();
FRmBonusLootEntry const* __cdecl PickRmBonusLoot(unsigned int Faction, float Difficulty);

// ---------------------------------------------------------------------------------------
// TU51 -- ship classes (the field: map<class id, member ids>, static init
// , atexit the routine).
// ---------------------------------------------------------------------------------------
extern std::map<unsigned int, std::vector<unsigned int>> GRmShipClasses;

void __cdecl LoadShipClasses(INI_Reader* Reader);
void __cdecl ClearShipClasses();
std::vector<unsigned int>* __cdecl FindShipClass(unsigned int const* ShipClassId);
bool __cdecl FindShipClassByMembers(unsigned int* OutShipClassId,
                                    std::vector<unsigned int> const& Members);

// ---------------------------------------------------------------------------------------
// TU51 -- ship mission properties (the field: map<ship arch id, property ids>,
// load-once flag the field, static init the branch, atexit the routine).
// ---------------------------------------------------------------------------------------
extern std::map<unsigned int, std::vector<unsigned int>> GRmShipMissionProperties;

void __cdecl LoadShipMissionProperties(INI_Reader* Reader);
bool __cdecl GetShipMissionProperties(unsigned int ShipArchId,
                                      std::vector<unsigned int>* InOutProperties);

// ---------------------------------------------------------------------------------------
// TU51 -- solar formations (the field: map<nickname id, formation>, static init
// , atexit the routine).
// ---------------------------------------------------------------------------------------
struct FRmSolarFormationSlot    // baseline 0x38 bytes
{
    unsigned char bMandatory;   // +0x00 "mandatory" = 1, "optional" = 0
    FMatrix Orientation;        // +0x04 identity unless a quaternion follows the position
    FVector Position;           // +0x28
    int SolarType;              // +0x34 index in {big_solar, defensive_solar,
                                //       killable_neutral, background_solar}
};

struct FRmSolarFormation        // map value, baseline 0x14 bytes
{
    unsigned int Nickname;      // +0x00
    int Type;                   // +0x04 index in {big_solar_formation,
                                //       battle_background_formation,
                                //       defensive_outpost_formation, loot_group_formation},
                                //       -1 when "type" is absent or unknown
    std::vector<FRmSolarFormationSlot> Slots;   // +0x08 (alloc byte) / +0x0c..+0x14

    void CollectOptionalSlotTypes(std::vector<int>* InOutTypes) const;
    void CollectMandatorySlotTypes(std::vector<int>* InOutTypes) const;
    int GetSlotCount() const;
    FRmSolarFormationSlot* GetSlot(unsigned int Index);
};

std::map<unsigned int, FRmSolarFormation>* __cdecl GetSolarFormations();
void __cdecl LoadSolarFormations(INI_Reader* Reader);
