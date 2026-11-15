// Author: Giperion.

// Content.dll lane CNT-POP: encounters and the static game tables.
//   TU31 tail 06efc9b0-06efe100  missions\formations.ini database
//   TU32      06efe100-06efe8e0  LoadContentGameTables / UnloadContentGameTables

#include "Encounters.h"
#include "BaseSupport.h"
#include "BaseDB.h"
#include "GenericScripts.h"
#include "Voices.h"
#include "LootProps.h"

#include "ContentInternal.h"
#include "ContentPlayerState.h"
#include "ContentStaticIds.h"   // TU30 06ef8d40-06ef8f20 and TU32 06efe100-06efe2e0 static-init thunks
#include "DockQueues.h"         // CountNpcShipsNear (TU53 the routine)
#include "FactionProps.h"
#include "PilotPersonalities.h"
#include "NpcShips.h"
#include "Populators.h"         // FSpawnRequest
#include "RandomMissionDB.h"
#include "RandomMissionGen.h"
#include "SystemInfo.h"
#include "common_id.h"
#include "common_ini.h"
#include "common_server.h"      // pub::SinglePlayer

#include <cstdlib>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------------------
// TU31 tail -- formations
// ---------------------------------------------------------------------------------------

// constructs the map, the routine (atexit) destroys it.
std::map<unsigned int, FFormation> GFormations;

namespace
{
    // this TU's math engine, created on first use (the routine).
    I3DMathEngine* GFormationMath = nullptr;

    I3DMathEngine* GetFormationMath()
    {
        if (GFormationMath == nullptr)
            GFormationMath = ContentAcquire3DMathEngine();
        return GFormationMath;
    }
}

// (06efc7a0-06efc7c3).
void SetMatrixIdentity(FMatrix* Matrix)
{
    Matrix->D[2][2] = 1.0f;
    Matrix->D[1][1] = 1.0f;
    Matrix->D[0][0] = 1.0f;
    Matrix->D[2][1] = 0.0f;
    Matrix->D[2][0] = 0.0f;
    Matrix->D[1][2] = 0.0f;
    Matrix->D[1][0] = 0.0f;
    Matrix->D[0][2] = 0.0f;
    Matrix->D[0][1] = 0.0f;
}

// (06efcac0-06efcae0).
int FFormation::CountPlayerSlots() const
{
    int Count = 0;
    for (std::vector<FFormationSlot>::const_iterator It = Slots.begin(); It != Slots.end(); ++It)
    {
        if (It->bPlayerSlot)
            ++Count;
    }
    return Count;
}

// (06efcaf0-06efcb10).
int FFormation::CountShipSlots() const
{
    int Count = 0;
    for (std::vector<FFormationSlot>::const_iterator It = Slots.begin(); It != Slots.end(); ++It)
    {
        if (!It->bPlayerSlot)
            ++Count;
    }
    return Count;
}

// (06efcb20-06efcbd9, RET 8: hidden return pointer + Index).
FVector FFormation::GetSlotPosition(unsigned int Index) const
{
    FVector Result;
    if (Index < Slots.size())
    {
        FVector Rotated;
        GetFormationMath()->TransformMatrix(Rotated, Orientation, Slots[Index].Position);
        Result.X = Rotated.X + Position.X;
        Result.Y = Rotated.Y + Position.Y;
        Result.Z = Rotated.Z + Position.Z;
        return Result;
    }
    Result.X = 0.0f;
    Result.Y = 0.0f;
    Result.Z = 0.0f;
    return Result;
}

// (06efcbe0-06efccc4, RET 0xc).
bool FFormation::GetSlotTransform(unsigned int Index, FVector* OutPosition,
                                  FMatrix* OutOrientation) const
{
    if (Index >= Slots.size())
        return false;

    FFormationSlot const& Slot = Slots[Index];
    FVector Rotated;
    GetFormationMath()->TransformMatrix(Rotated, Orientation, Slot.Position);
    FVector World;
    World.X = Rotated.X + Position.X;
    World.Y = Rotated.Y + Position.Y;
    World.Z = Rotated.Z + Position.Z;
    *OutPosition = World;

    FMatrix Combined;
    GetFormationMath()->MulMatrix(Combined, Orientation, Slot.Orientation);
    *OutOrientation = Combined;
    return true;
}

// (06efccd0-06efccde).
bool FFormation::IsPlayerSlot(int Index) const
{
    return Slots.data()[Index].bPlayerSlot;
}

// (06efccf0-06efd02c): one [Formation] block; the caller already matched
// the header. Values: nickname, pos (ship slot, module storage), pl_pos (player slot).
void __cdecl LoadFormation(INI_Reader* Reader)
{
    FFormation Formation;
    Formation.Nickname.clear();
    Formation.Position.X = 0.0f;                       // (0, 0, 0)
    Formation.Position.Y = 0.0f;
    Formation.Position.Z = 0.0f;
    SetMatrixIdentity(&Formation.Orientation);

    ID_String Key;
    Key.clear();

    // the orientation copied into every slot.
    FMatrix SlotOrientation;
    SetMatrixIdentity(&SlotOrientation);

    while (Reader->read_value())
    {
        if (Reader->is_value("nickname"))
        {
            unsigned int Id = CreateID(Reader->get_value_string());
            Key.Id = Id;
            Formation.Nickname.Id = Id;
        }
        else if (Reader->is_value("pos"))
        {
            FFormationSlot Slot;
            Slot.Position.X = Reader->get_value_float(0);
            Slot.Position.Y = Reader->get_value_float(1);
            Slot.Position.Z = Reader->get_value_float(2);
            Slot.Orientation = SlotOrientation;
            Slot.bPlayerSlot = false;
            Formation.Slots.insert(Formation.Slots.end(), Slot);
        }
        else if (Reader->is_value("pl_pos"))
        {
            FFormationSlot Slot;
            Slot.Position.X = Reader->get_value_float(0);
            Slot.Position.Y = Reader->get_value_float(1);
            Slot.Position.Z = Reader->get_value_float(2);
            Slot.Orientation = SlotOrientation;
            Slot.bPlayerSlot = true;
            Formation.Slots.insert(Formation.Slots.end(), 1, Slot);
        }
    }

    // pair copy, map insert (an existing key keeps its formation).
    GFormations.insert(std::pair<unsigned int const, FFormation>(Key.Id, Formation));
}

// (06efd070-06efd198): empty every formation's slots, then the map.
void __cdecl ClearFormations()
{
    for (std::map<unsigned int, FFormation>::iterator It = GFormations.begin();
         It != GFormations.end(); ++It)
    {
        It->second.Slots.clear();
    }
    GFormations.clear();
}

// (06efd1a0-06efd1e3): lower_bound + exact key; null on a miss.
FFormation* __cdecl FindFormation(unsigned int const* FormationId)
{
    std::map<unsigned int, FFormation>::iterator It = GFormations.find(*FormationId);
    if (It == GFormations.end())
        return nullptr;
    return &It->second;
}

// ---------------------------------------------------------------------------------------
// TU30 -- the zone population block (SystemInfo.h). The ctor the routine is the member
// initialisers of FZonePopulation.
// ---------------------------------------------------------------------------------------

// (06ef9000-06ef93b1, RET 0xc).
bool FZonePopulation::ParseValue(INI_Reader* Reader, FSystemInfo* InSystem, FSystemZone* Zone)
{
    System = InSystem;
    if (Reader->is_value("encounter"))
    {
        // "encounter = id, level, chance".
        unsigned int const EncounterId = CreateID(Reader->get_value_string(0));
        int const Level = static_cast<int>(Reader->get_value_float(1));     // ftol
        float const Chance = Reader->get_value_float(2);

        // the system's parameters for that id, bound to this zone and
        // tagged with the level.
        FEncounterParameters Parameters;
        InSystem->GetEncounterParameters(EncounterId, &Parameters);
        Parameters.ZoneId = Zone->ZoneId;
        Parameters.ApplyDifficulty(Level);

        // parameters without faction weights take the "faction =
        // name, weight" lines that follow; the first other line is put back (set_state).
        if (Parameters.Factions.Items.empty())
        {
            weighted_vector<unsigned int> Factions;
            for (;;)
            {
                INI_Reader::State Saved;
                Reader->get_state(Saved);
                if (!Reader->read_value())
                    break;
                if (!Reader->is_value("faction"))
                {
                    Reader->set_state(Saved);
                    break;
                }
                unsigned int Faction;
                pub::Reputation::GetReputationGroup(Faction, Reader->get_value_string(0));
                float const Weight = Reader->get_value_float(1);
                Factions.Items.insert(Factions.Items.end(), 1,
                                      std::pair<unsigned int, float>(Faction, Weight));
            }
            Parameters.Factions = Factions;
        }

        // the copy (the routine) with its chance goes at the end.
        FZoneEncounter Encounter(Parameters, Chance);
        Encounters.insert(Encounters.end(), 1, Encounter);
    }
    else if (Reader->is_value("density_restriction"))
    {
        // push_back.
        FDensityRestriction Restriction;
        Restriction.Type.clear();
        Restriction.MaxCount = Reader->get_value_int(0);
        Restriction.Type.Id = CreateID(Reader->get_value_string(1));
        DensityRestrictions.push_back(Restriction);
    }
    else if (Reader->is_value("population_additive"))
    {
        bPopulationAdditive = Reader->get_value_bool(0);
    }
    else if (Reader->is_value("tradelane_attack"))
    {
        // FMUL double [the field] = 0.01, FSTP float.
        TradelaneAttack = static_cast<float>(static_cast<double>(Reader->get_value_float(0)) * 0.01);
    }
    else if (Reader->is_value("max_battle_size"))
    {
        MaxBattleSize = Reader->get_value_int(0);
    }
    else if (Reader->is_value("relief_time"))
    {
        // TEST AH,5 + JP on RepopTime vs 0.0 -- a negative repop time
        // zeroes the relief time just read.
        ReliefTime = Reader->get_value_float(0);
        if (RepopTime < 0.0f)
            ReliefTime = 0.0f;
    }
    else
    {
        return false;
    }
    bHasPopulationKeys = true;
    return true;
}

// (06ef9dd0-06ef9e07).
bool FZonePopulation::Validate()
{
    // every encounter must refresh at time 0.
    for (std::vector<FZoneEncounter>::iterator It = Encounters.begin(); It != Encounters.end(); ++It)
    {
        if (!It->Refresh(0.0f))
        {
            bValid = false;
            return false;
        }
    }
    bValid = true;
    return true;
}

// TU30 static initialisers.
float GEncounterDensityRadiusSP = 3750.0f;
// FLD 850.0f (the field), FADD ST0,ST0, FADD 7500.0f (the field).
float GEncounterDensityRadiusMP = static_cast<float>(850.0 + 850.0 + 7500.0);
float GEncounterRestrictionCone = 3750.0f;
float GEncounterPlacementRange = 2700.0f;

namespace
{
    // the routine writes -0.01f (06ef994a) and then the clamped chance
    // (06ef9a10); nothing in Content reads it.
    float GZoneSpawnChance;

    typedef std::vector<std::pair<unsigned int, float>> FEncounterPool;

    // / the routine 06ef95ec-06ef96cd: every encounter refreshes at *Time; the
    // ones that fail are erased (bValid drops when the list becomes empty), the others spawn
    // their starred formations. Returns the ships created.
    int RefreshAndSpawnStarred(FZonePopulation* Population, FSpawnRequest const* Request,
                               float const* Time)
    {
        int Created = 0;
        std::vector<FZoneEncounter>::iterator It = Population->Encounters.begin();
        while (It != Population->Encounters.end())
        {
            if (It->Refresh(*Time))
            {
                bool const bStarred = true;
                Created += It->Spawn(Request, &bStarred, &Population->DensityRestrictions);
                ++It;
                continue;
            }
            It = Population->Encounters.erase(It);                          // / the routine
            if (Population->Encounters.empty())
                Population->bValid = false;
        }
        return Created;
    }

    // {encounter index, chance} for every encounter (capacity = count).
    void BuildEncounterPool(FZonePopulation const* Population, FEncounterPool* Pool)
    {
        Pool->reserve(Population->Encounters.size());
        for (std::vector<FZoneEncounter>::size_type Index = 0; Index < Population->Encounters.size(); ++Index)
        {
            Pool->insert(Pool->end(), 1,
                         std::pair<unsigned int, float>(static_cast<unsigned int>(Index),
                                                        Population->Encounters[Index].Chance));
        }
    }

    // a weighted pick; *bEmpty when the weights sum to 0 (no rand then).
    // The sum and the running total stay on the x87 stack; the sum and the scaled roll are
    // stored as floats. Returns Pool->size() when nothing is picked.
    FEncounterPool::size_type PickFromPool(FEncounterPool const* Pool, bool* bEmpty)
    {
        double Sum = 0.0;
        for (FEncounterPool::const_iterator It = Pool->begin(); It != Pool->end(); ++It)
            Sum += static_cast<double>(It->second);
        float const SumStored = static_cast<float>(Sum);
        // TEST AH,0x44 + JNP: only an exactly zero sum stops the draw.
        *bEmpty = Sum == 0.0;
        if (*bEmpty)
            return Pool->size();
        float const Roll = static_cast<float>(static_cast<double>(rand()) *
                                              static_cast<double>(1.0f / 32768.0f) *
                                              static_cast<double>(SumStored));
        double Running = 0.0;
        for (FEncounterPool::size_type Index = 0; Index < Pool->size(); ++Index)
        {
            Running += static_cast<double>((*Pool)[Index].second);
            // TEST AH,0x41 + JNP: taken when the roll is below or equal to the running total.
            if (static_cast<double>(Roll) <= Running)
                return Index;
        }
        return Pool->size();
    }
}

// (06ef95d0-06ef9937, RET 0x10).
int FZonePopulation::Populate(FSpawnRequest const* Request, float const* Time,
                              float const* DensityScale, unsigned int const* MaxDensity)
{
    int const StarredCount = RefreshAndSpawnStarred(this, Request, Time);

    int DrawnCount = 0;
    if (!RollSpawnChance(&Request->Placement, DensityScale, MaxDensity))
        return StarredCount + DrawnCount;

    FEncounterPool Pool;
    BuildEncounterPool(this, &Pool);
    for (;;)
    {
        bool bEmpty;
        FEncounterPool::size_type const Picked = PickFromPool(&Pool, &bEmpty);
        // the loop ends on no pick or once an encounter has spawned (the
        // roll of that extra round is still drawn).
        if (bEmpty || Picked == Pool.size() || DrawnCount != 0)
            break;
        bool const bStarred = false;
        DrawnCount = Encounters[Pool[Picked].first].Spawn(Request, &bStarred, &DensityRestrictions);
        Pool.erase(Pool.begin() + Picked);
    }
    return StarredCount + DrawnCount;
}

// (06ef9940-06ef9a52, RET 0xc).
bool FZonePopulation::RollSpawnChance(FSpaceObjPlacement const* Placement,
                                      float const* DensityScale, unsigned int const* MaxDensity)
{
    GZoneSpawnChance = -0.01f;
    unsigned int Limit = *MaxDensity;
    if (Limit == 0xffffffffu)
        Limit = static_cast<unsigned int>(Density);
    float const* Radius = pub::SinglePlayer() ? &GEncounterDensityRadiusSP : &GEncounterDensityRadiusMP;
    int const Ships = CountNpcShipsNear(System, Placement, Radius, &GEncounterRestrictionCone,
                                        nullptr);
    if (static_cast<unsigned int>(Ships) >= Limit)                          // JNC
        return false;

    // FILD qword Limit * Scale - FISUB Ships, * 1/3 (the field), over
    // RepopTime / Scale * 1/3 (FDIVP ST1,ST0).
    double const Third = static_cast<double>(1.0f / 3.0f);
    double const Numerator = (static_cast<double>(Limit) * static_cast<double>(*DensityScale) -
                              static_cast<double>(Ships)) * Third;
    double const Denominator = static_cast<double>(RepopTime) / static_cast<double>(*DensityScale) * Third;
    float Chance = static_cast<float>(Numerator / Denominator);
    if (Chance > 0.0f)
    {
        if (!(Chance < 1.0f))
            Chance = 1.0f;
    }
    else
    {
        Chance = 0.0f;
    }
    GZoneSpawnChance = Chance;
    // TEST AH,5 + JP -- a roll at or above the chance fails.
    return static_cast<double>(rand()) * static_cast<double>(1.0f / 32768.0f) <
           static_cast<double>(Chance);
}

// (06ef9a60-06ef9dbd, RET 0xc).
int FZonePopulation::PopulateUpTo(FSpawnRequest const* Request, float const* Time,
                                  unsigned int const* MaxCount)
{
    int const StarredCount = RefreshAndSpawnStarred(this, Request, Time);

    int DrawnCount = 0;
    FEncounterPool Pool;
    BuildEncounterPool(this, &Pool);
    for (;;)
    {
        bool bEmpty;
        FEncounterPool::size_type const Picked = PickFromPool(&Pool, &bEmpty);
        if (bEmpty || Picked == Pool.size())
            break;
        // JNC -- the total compares unsigned.
        if (static_cast<unsigned int>(DrawnCount + StarredCount) >= *MaxCount)
            break;
        FZoneEncounter& Encounter = Encounters[Pool[Picked].first];
        Encounter.Refresh(*Time);                                           // result ignored
        bool const bStarred = false;
        int const Created = Encounter.Spawn(Request, &bStarred, &DensityRestrictions);
        if (Created != 0)
        {
            // a successful encounter stays in the pool.
            DrawnCount += Created;
            continue;
        }
        Pool.erase(Pool.begin() + Picked);
    }
    return StarredCount + DrawnCount;
}

// ---------------------------------------------------------------------------------------
// TU32
// ---------------------------------------------------------------------------------------

// (06efe300-06efe86c). One INI_Reader for every file; the result of
// OpenContentDataIni (the routine) is never tested, the loader runs regardless.
void __cdecl LoadContentGameTables()
{
    INI_Reader Reader;

    OpenContentDataIni("audio\\voices_space_male.ini", &Reader);
    LoadVoiceLines(&Reader);                            // the routine
    Reader.reset();
    LoadVoiceProperties(&Reader);                       // the routine
    Reader.close();

    OpenContentDataIni("audio\\voices_space_female.ini", &Reader);
    LoadVoiceLines(&Reader);                            // the routine
    Reader.reset();
    LoadVoiceProperties(&Reader);                       // the routine
    Reader.close();

    OpenContentDataIni("audio\\voices_recognizable.ini", &Reader);
    LoadVoiceLines(&Reader);                            // the routine
    Reader.reset();
    LoadVoiceProperties(&Reader);                       // the routine
    Reader.close();

    LoadFactionEmpathy("missions\\empathy.ini");        // the routine

    OpenContentDataIni("missions\\voice_properties.ini", &Reader);
    LoadVoiceProperties(&Reader);                       // the routine
    Reader.close();

    OpenContentDataIni("missions\\formations.ini", &Reader);
    while (Reader.read_header())
    {
        if (Reader.is_header("Formation"))
            LoadFormation(&Reader);
    }
    Reader.close();

    OpenContentDataIni("missions\\pilots_population.ini", &Reader);
    LoadPilotPersonalities(&Reader);
    Reader.close();

    OpenContentDataIni("missions\\pilots_story.ini", &Reader);
    LoadPilotPersonalities(&Reader);
    Reader.close();

    OpenContentDataIni("missions\\npcships.ini", &Reader);
    // the routine(&Reader, &the field).
    LoadNpcShipArchs(&Reader, &GNpcShipArchs);
    Reader.close();

    OpenContentDataIni("missions\\npcships_test.ini", &Reader);
    // the routine(&Reader, &the field).
    LoadNpcShipArchs(&Reader, &GNpcShipArchs);
    Reader.close();

    OpenContentDataIni("missions\\faction_prop.ini", &Reader);
    LoadFactionProps(&Reader);                          // the routine
    Reader.close();

    OpenContentDataIni("equipment\\commodities_per_faction.ini", &Reader);
    LoadFactionGoods(&Reader);                          // the routine
    Reader.close();

    OpenContentDataIni("missions\\shipclasses.ini", &Reader);
    LoadShipClasses(&Reader);
    Reader.close();

    OpenContentDataIni("ships\\shiparch.ini", &Reader);
    LoadShipMissionProperties(&Reader);
    Reader.close();

    OpenContentDataIni("missions\\lootprops.ini", &Reader);
    // the routine.
    LoadLootProps(&Reader);
    Reader.reset();
    // the routine.
    LoadPhantomLoot(&Reader);
    Reader.close();

    LoadGcsExclusions("scripts\\GCS\\GCSExcl.ini");      // the routine

    OpenContentDataIni("missions\\PTough.ini", &Reader);
    LoadPlayerToughnessScale(&Reader);
    Reader.close();

    OpenContentDataIni("missions\\RankDiff.ini", &Reader);
    LoadRankDifficulties(&Reader);
    Reader.close();

    OpenContentDataIni("missions\\news.ini", &Reader);
    // the routine.
    LoadNewsItems(&Reader);
    Reader.close();

    // the inlined INI_Reader destructor.
}

// (06efe870-06efe898, tail JMP into the routine).
void __cdecl UnloadContentGameTables()
{
    ClearRankDifficulties();
    UnloadContentBases();                               // the routine
    ClearSystemInfos();
    ClearShipClasses();
    UnloadFactionEmpathy();                             // the routine
    UnloadFactionProps();                               // the routine
    // the routine.
    ClearNpcShipArchs();
    ClearPilotPersonalities();
    ClearFormations();
}
