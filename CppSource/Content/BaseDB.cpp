// Author: Giperion.

// Content.dll TU17 (06ea7750-06eb2670): the mbases.ini database. See BaseDB.h. The TU22
// (06ed3d90-06ed4220) GF_NPC record parsers are in BaseNpcRecords.cpp.

// TU17's own 16 static-ID initializers (the field..the field),
// before the GContentBases initializer 06ea7950.
#include "ContentStaticIds.h"

#include "BaseDB.h"
#include "BaseSupport.h"         // FMissionOfferAcceptMsg, RefreshMissionVendorOffers
#include "ContentInternal.h"     // OpenContentDataIni
#include "ContentPlayerState.h"
#include "FactionProps.h"        // GetFactionHostileGroups (TU48 the routine)
#include "GenericScripts.h"      // PickFidgetScript
#include "LootProps.h"           // CreateBarMissionOffer (TU35 the routine)
#include "RandomMission.h"       // FRandomMissionLoader
#include "RandomMissionGen.h"    // RoundRmMoney (TU44 the routine)
#include "RefHandle.h"           // TRefHandle (the routine)
#include "RtcScripts.h"          // FCharacterEncounter (TU23)
#include "SystemInfo.h"          // FindSystemInfo
#include "common_iface.h"        // CharacterBehaviorDescription
#include "common_goods.h"        // CharacterDescription
#include "common_id.h"
#include "common_ini.h"
#include "common_server.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace
{
    // static init / atexit thunks at the head of TU17.
    std::map<unsigned int, FContentBase> GContentBases;

    // 06ea7fed-06ea80a4 (and the same code for value 2): x87 clamp of a
    // "misn" difficulty to [0, 100] (the field = 0.0, the field = 100.0).
    // FCOMP 0 vs value, TEST AH,0x41 / JNZ: 0 <= value (or unordered) keeps the value,
    // otherwise 0; that result FCOMP 100, TEST AH,5 / JP: not below 100 (or unordered)
    // -> 100; else the value again, clamped below at 0 the same way.
    float ClampMissionDifficulty(float Value)
    {
        float Low = (Value < 0.0f) ? 0.0f : Value;
        if (!(Low < 100.0f))
            return 100.0f;
        return (Value < 0.0f) ? 0.0f : Value;
    }
}

// ---------------------------------------------------------------------------------------
// TU17 -- GF_NPC
// ---------------------------------------------------------------------------------------

// (06ea7f60-06ea8360).
bool __cdecl LoadBaseNpc(ID_String* OutNickname, FBaseNpc* Npc, INI_Reader* Reader,
                         unsigned int BaseId)
{
    while (Reader->read_value())
    {
        if (Npc->Desc.ParseValue(Reader, OutNickname))
            continue;

        if (Reader->is_value("misn"))
        {
            FBaseNpcMission Mission;
            Mission.Type = Reader->get_value_string(0);
            Mission.MinDifficulty = ClampMissionDifficulty(Reader->get_value_float(1));
            Mission.MaxDifficulty = ClampMissionDifficulty(Reader->get_value_float(2));
            Npc->Missions.push_back(Mission);
        }
        else if (Reader->is_value("bribe"))
        {
            FBaseNpcBribe Bribe;
            if (Bribe.Parse(Reader))
                Npc->Bribes.push_back(Bribe);
        }
        else if (Reader->is_value("know"))
        {
            FBaseNpcKnow Know;
            if (Know.Parse(Reader))
                Npc->Knows.push_back(Know);
        }
        else if (Reader->is_value("rumor") || Reader->is_value("rumor_type2"))
        {
            // the whole 0x2c record is zeroed first.
            FBaseNpcRumor Rumor;
            memset(&Rumor, 0, sizeof(Rumor));
            if (Rumor.Parse(Reader))
                Npc->Rumors.push_back(Rumor);
        }
        else if (Reader->is_value("room"))
        {
            unsigned int RoomId = pub::GF::FindLocation(BaseId, Reader->get_value_string());
            Npc->Rooms.push_back(RoomId);
        }
    }

    if (OutNickname->IsEmpty())
        return false;
    return Npc->Desc.IsComplete();
}

// ---------------------------------------------------------------------------------------
// TU17 -- fixtures
// ---------------------------------------------------------------------------------------

// 06eaabe9-06eaac12.
FBaseFixture::FBaseFixture()
    : Npc(nullptr), Placement(0)
{
    Fidget.Length = 0;
    Fidget.Data[0] = '\0';
    Role.Length = 0;
    Role.Data[0] = '\0';
}

// The copy inside the FBaseRoom copy-construct the routine (06eb1bf1-06eb1cae): an empty
// list, the inlined the routine (nothing to release), Other's pointers copied, AddRef each.
FBaseFixtureList::FBaseFixtureList(FBaseFixtureList const& Other)
    : Fixtures(Other.Fixtures)
{
    for (std::list<FBaseFixture*>::iterator It = Fixtures.begin(); It != Fixtures.end(); ++It)
        (*It)->AddRef();
}

// (ICF-shared with FMissionDirectiveList::operator=; callers here: the
// vector<FBaseRoom>::insert element assignment 06eb0716/06eb0798/06eb07ce and Unload
// ): Release every own element and erase them, then -- unless self-assigned --
// copy Other's pointers, then AddRef every element now held. A self-assignment therefore
// leaves the list empty, as in baseline.
FBaseFixtureList& FBaseFixtureList::operator=(FBaseFixtureList const& Other)
{
    Clear();
    if (this != &Other)
        Fixtures = Other.Fixtures;
    for (std::list<FBaseFixture*>::iterator It = Fixtures.begin(); It != Fixtures.end(); ++It)
        (*It)->AddRef();
    return *this;
}

// Release every element, then free the nodes.
FBaseFixtureList::~FBaseFixtureList()
{
    Clear();
}

void FBaseFixtureList::Add(FBaseFixture* Fixture)
{
    Fixtures.push_back(Fixture);
    Fixture->AddRef();
}

// .
void FBaseFixtureList::Clear()
{
    for (std::list<FBaseFixture*>::iterator It = Fixtures.begin(); It != Fixtures.end(); ++It)
        (*It)->Release();
    Fixtures.clear();
}

// ---------------------------------------------------------------------------------------
// TU17 -- MBase
// ---------------------------------------------------------------------------------------

bool GBaseMissionTypesInSinglePlayer = true;
bool GBaseMissionTypesInMultiplayer = true;

// (06ea8f80-06ea90de).
FContentBase::FContentBase(unsigned int const* InBaseId)
    : BaseId(*InBaseId)
{
}

// (06eab350-06eab735).
void FContentBase::LoadFactions(INI_Reader* Reader)
{
    while (Reader->read_value())
    {
        if (Reader->is_value("local_faction"))
            pub::Reputation::GetReputationGroup(LocalFaction, Reader->get_value_string());
        else if (Reader->is_value("diff"))
            Diff = Reader->get_value_int(0);
        else
            read_comm_ini_data(*Reader);                // vtbl[+8]
    }

    if (!Reader->read_header())
        return;
    do
    {
        if (Reader->is_header("BaseFaction"))
        {
            FBaseFaction Faction;
            float Weight = -1.0f;
            while (Reader->read_value())
            {
                if (Reader->is_value("faction"))
                {
                    pub::Reputation::GetReputationGroup(Faction.Group, Reader->get_value_string());
                }
                else if (Reader->is_value("weight"))
                {
                    Weight = Reader->get_value_float(0);
                }
                else if (Reader->is_value("mission_type"))
                {
                    FBaseMissionType MissionType;
                    MissionType.Type = "DestroyMission";
                    MissionType.MinDifficulty = Reader->get_value_float(1);
                    MissionType.MaxDifficulty = Reader->get_value_float(2);
                    float TypeWeight = 1.0f;
                    if (!Reader->is_value_empty(3))
                        TypeWeight = Reader->get_value_float(3);
                    bool bEnabled = pub::SinglePlayer() ? GBaseMissionTypesInSinglePlayer
                                                        : GBaseMissionTypesInMultiplayer;
                    if (bEnabled)
                    {
                        MissionType.Weight = TypeWeight;
                        Faction.MissionTypes.push_back(MissionType);
                    }
                }
                else if (Reader->is_value("npc"))
                {
                    // unique insert (find, push_back when absent).
                    unsigned int NpcId = CreateID(Reader->get_value_string());
                    std::vector<unsigned int>::iterator It = Faction.Npcs.begin();
                    for (; It != Faction.Npcs.end(); ++It)
                    {
                        if (*It == NpcId)
                            break;
                    }
                    if (It == Faction.Npcs.end())
                        Faction.Npcs.push_back(NpcId);
                }
            }
            // a temporary element {group, npcs copy (EH state 1), mission
            // types copy (state 2), Weight} -- the local's own +0x24 is never written -- goes
            // to the routine insert(end, 1, &temp) (state 3), then the temporary (states 3,
            // 0) and the local (state 4) are destroyed.
            FBaseFaction Element(Faction);
            Element.Weight = Weight;
            Factions.push_back(Element);
        }
        else if (Reader->is_header("mBase"))
        {
            return;
        }
    } while (Reader->read_header());
}

// (06eaa820-06eaaffb).
void FContentBase::Load(INI_Reader* Reader)
{
    int PreviousCount = LoadCount;
    ++LoadCount;
    if (PreviousCount != 0)
        return;

    INI_Reader OwnReader;
    INI_Reader* Current = Reader;
    if (Current == nullptr)
    {
        // find this base's [MBase] in missions\mBases.ini; the next
        // read_header then starts on the section after its nickname line.
        unsigned int Id = BaseId;
        Current = &OwnReader;
        if (OpenContentDataIni("missions\\mBases.ini", &OwnReader))
        {
            char Nickname[0x40];
            pub::GetBaseNickname(Nickname, 0x40, Id);
            bool bFound = false;
            while (!bFound && OwnReader.read_header())
            {
                if (!OwnReader.is_header("MBase") || !OwnReader.read_value())
                    continue;
                do
                {
                    if (OwnReader.is_value("nickname") &&
                        _stricmp(Nickname, OwnReader.get_value_string()) == 0)
                    {
                        bFound = true;
                        break;
                    }
                } while (OwnReader.read_value());
            }
        }
    }

    while (Current->read_header())
    {
        if (Current->is_header("MVendor"))
        {
            while (Current->read_value())
            {
                if (Current->is_value("num_offers"))
                {
                    VendorOffersMin = Current->get_value_int(0);
                    VendorOffersMax = Current->get_value_int(1);
                }
            }
        }
        else if (Current->is_header("GF_NPC"))
        {
            FBaseNpc Npc;
            ID_String Nickname;
            Nickname.clear();
            LoadBaseNpc(&Nickname, &Npc, Current, BaseId);
            Npcs.insert(std::pair<unsigned int const, FBaseNpc>(Nickname.Id, Npc));
        }
        else if (Current->is_header("mRoom"))
        {
            FBaseRoom Room;
            while (Current->read_value())
            {
                if (Current->is_value("nickname"))
                {
                    Room.RoomId = pub::GF::FindLocation(BaseId, Current->get_value_string());
                }
                else if (Current->is_value("character_density"))
                {
                    Room.CharacterDensity = Current->get_value_int(0);
                }
                else if (Current->is_value("fixture"))
                {
                    FBaseFixture* Fixture = new FBaseFixture;
                    unsigned int NpcId = CreateID(Current->get_value_string(0));
                    std::map<unsigned int, FBaseNpc>::iterator NpcIt = Npcs.find(NpcId);
                    if (NpcIt != Npcs.end())
                    {
                        Fixture->Npc = &NpcIt->second;
                        if (pub::GF::GetCharacterPlacementByName(Room.RoomId,
                                Current->get_value_string(1), Fixture->Placement) == 0)
                        {
                            Fixture->Fidget = Current->get_value_string(2);
                            Fixture->Role = Current->get_value_string(3);
                            // for the roles "trader", "shipdealer" and
                            // "equipment" baseline computes Npc->Missions.size() (or the
                            // address of Npc->Bribes when it is empty) into a local that
                            // overwrites before any read -- no effect.
                            Room.Fixtures.Add(Fixture);
                        }
                    }
                    Fixture->Release();
                }
            }
            if ((Room.RoomId & 0x3fffffff) != 0)
                Rooms.push_back(Room);
        }
        else if (Current->is_header("mBase"))
        {
            break;
        }
    }

    // every [BaseFaction] npc takes the faction as its affiliation.
    // baseline writes through the lower_bound result unconditionally (+0x4c of the node); a
    // miss lands at +0x4c of the map's head node -- a full 0xa8 node (the routine, built by
    // 06ea906c) whose value storage is never constructed or read -- so the
    // write has no observable effect. std::map has no head-node value, so the port skips it.
    for (std::vector<FBaseFaction>::iterator FactionIt = Factions.begin();
         FactionIt != Factions.end(); ++FactionIt)
    {
        for (std::vector<unsigned int>::iterator NpcId = FactionIt->Npcs.begin();
             NpcId != FactionIt->Npcs.end(); ++NpcId)
        {
            std::map<unsigned int, FBaseNpc>::iterator NpcIt = Npcs.find(*NpcId);
            if (NpcIt != Npcs.end())
                NpcIt->second.Desc.Affiliation = FactionIt->Group;
        }
    }
}

// (06eab1c0-06eab34f). Each container goes through VC6 erase(begin, end)
// (the whole-tree fast path or node by node; for the vector a self-copy of nothing and the
// element dtors the routine) -- clear() here.
void FContentBase::Unload()
{
    if (--LoadCount != 0)
        return;
    RoomStates.clear();
    Rooms.clear();
    Npcs.clear();
}

// (06eac110-06eac15b).
FBaseRoomState* FContentBase::FindRoomState(unsigned int const* RoomId)
{
    std::map<unsigned int, FBaseRoomState>::iterator It = RoomStates.find(*RoomId);
    if (It == RoomStates.end())
        return nullptr;
    return &It->second;
}

// (06eac0c0-06eac103): lower_bound + exact key check.
FContentBase* FindContentBaseInfo(unsigned int const* BaseId)
{
    std::map<unsigned int, FContentBase>::iterator It = GContentBases.find(*BaseId);
    if (It == GContentBases.end())
        return nullptr;
    return &It->second;
}

// (06eab770-06eab78d, RET 4): linear search of +0x58 (0x28 stride) by +0.
FBaseFaction* FContentBase::FindFaction(unsigned int Group)
{
    for (std::vector<FBaseFaction>::iterator It = Factions.begin(); It != Factions.end(); ++It)
    {
        if (It->Group == Group)
            return &*It;
    }
    return nullptr;
}

// (06eab790-06eabb65).
void __cdecl BuildContentBases()
{
    unsigned int BaseId = 0;
    while (pub::NextBaseID(BaseId))
    {
        FContentBase Base(&BaseId);
        GContentBases.insert(std::pair<unsigned int const, FContentBase>(BaseId, Base));
    }

    INI_Reader Reader;
    if (OpenContentDataIni("missions\\mBases.ini", &Reader))
        Reader.read_header();
    while (!Reader.is_end())
    {
        if (!Reader.is_header("mBase"))
            break;
        Reader.read_value();
        unsigned int Id;
        if (pub::GetBaseID(Id, Reader.get_value_string()) == 0)
        {
            std::map<unsigned int, FContentBase>::iterator It = GContentBases.find(Id);
            if (It != GContentBases.end())
            {
                It->second.LoadFactions(&Reader);
                continue;
            }
        }
        Reader.read_header();
    }
}

// (06eabf40-06eac082).
void __cdecl LoadDynBaseInfo(INI_Reader* Reader)
{
    GContentBases.clear();
    BuildContentBases();

    while (Reader->read_header())
    {
        if (!Reader->is_header("DynBaseInfo") || !Reader->read_value())
            continue;
        do
        {
            if (Reader->is_value("nickname"))
            {
                // the base is looked up and the result discarded.
                unsigned int Id;
                pub::GetBaseID(Id, Reader->get_value_string());
                GContentBases.find(Id);
                break;
            }
        } while (Reader->read_value());
    }
}

// (06eabe80-06eabf38).
void __cdecl UnloadContentBases()
{
    GContentBases.clear();
}

// (06eac090-06eac0b8): iterates the whole table with an empty body.
void __cdecl SaveContentBases(FILE* /*File*/)
{
    for (std::map<unsigned int, FContentBase>::iterator It = GContentBases.begin();
         It != GContentBases.end(); ++It)
    {
    }
}

// ---------------------------------------------------------------------------------------
// TU17 -- bar characters
// ---------------------------------------------------------------------------------------

int GBarCharacterCount = 0;
std::map<int, FContentBase*> GBarSceneBases;

namespace
{
    // (.data, 1; XREF[2] 06ea965b R / 06ea9667 W): the next bar scene id,
    // post-incremented by FContentBase::CreateBarScene only.
    int GBarSceneNextId = 1;

    // {kind, weight} of the routine's local choice vector (8 bytes, reserve 5).
    struct FBarOfferChoice
    {
        int Kind;
        float Weight;
    };

    // {rumor, weight} of the routine case 3 (8 bytes).
    struct FBarRumorChoice
    {
        FBaseNpcRumor const* Rumor;
        float Weight;
    };

    // (and its inlined copy at 06ea85a8-06ea863e). The weights are summed on
    // the x87 stack (extended; double here), and TEST AH,0x44 / JNP returns end() only for
    // a sum equal to 0. The pick is float(rand() * 2^-15 (the field) * float(sum)); the
    // first element whose running sum is not below it (TEST AH,0x41 / JNP: pick < sum or
    // equal) is taken, end() when none is.
    // OutSlot, when given, receives the float the inlined copy leaves in its stack slot
    // [ESP+0x1c] (the sum, then the pick): the routine reuses that slot as the ship id of
    // pub::Player::GetShipID without clearing it.
    template <typename TChoice>
    typename std::vector<TChoice>::const_iterator PickWeighted(std::vector<TChoice> const& Choices,
                                                               float* OutSlot = nullptr)
    {
        double Sum = 0.0;
        for (typename std::vector<TChoice>::const_iterator It = Choices.begin();
             It != Choices.end(); ++It)
        {
            Sum += It->Weight;
        }
        float const SumF = static_cast<float>(Sum);
        if (OutSlot != nullptr)
            *OutSlot = SumF;                    // FST
        if (Sum == 0.0)
            return Choices.end();

        float const Pick = static_cast<float>(static_cast<double>(rand()) *
                                              static_cast<double>(1.0f / 32768.0f) * SumF);
        if (OutSlot != nullptr)
            *OutSlot = Pick;                    // FSTP
        double Running = 0.0;
        for (typename std::vector<TChoice>::const_iterator It = Choices.begin();
             It != Choices.end(); ++It)
        {
            Running += It->Weight;
            if (Pick <= Running)
                return It;
        }
        return Choices.end();
    }

    // The random-element idiom of the routine (06ea8655-06ea86b3, 06ea8814-06ea8860,
    // ): an empty vector gives end(); a vector of one is taken without
    // calling rand; otherwise index rand() * size / 0x8000 (CDQ, AND 0x7fff, SAR 15).
    template <typename T>
    typename std::vector<T>::const_iterator PickRandom(std::vector<T> const& Values)
    {
        if (Values.empty())
            return Values.end();
        int Count = static_cast<int>(Values.size());
        if (Count - 1 == 0)
            return Values.begin();
        return Values.begin() + (rand() * Count) / 0x8000;
    }

    // TU48 the routine (06f50ea0-06f50ec9, cdecl): Base minus the feeling of Reputation
    // towards Group; pub::Reputation::GetReputation (IAT 06fb35e8) leaves 0 on failure.
    // Kept here until TU48 (CNT-GCS, wave 2) gets its own file.
    float GetFeelingShortfall(int Reputation, unsigned int Group, float Base)
    {
        float Feeling = 0.0f;
        pub::Reputation::GetReputation(Reputation, Group, Feeling);
        return Base - Feeling;
    }
}

// (06eacd40-06eacd47). FContentRefObject (ContentRefObject.h) is defined
// here, in the TU that holds the baseline body.
unsigned int FContentRefObject::AddRef()
{
    ++ReferenceCount;
    return ReferenceCount;
}

// (06eacd50-06eacd66).
unsigned int FContentRefObject::Release()
{
    unsigned int Count = --ReferenceCount;
    if (Count == 0)
        delete this;
    return Count;
}

void FBarOffer::ReleaseObject()
{
    if ((Kind == BAR_OFFER_MISSION || Kind == BAR_OFFER_BRIBE) && Object != nullptr)
        Object->Release();
}

FBarOffer::FBarOffer(FBarOffer const& Other)
    : Kind(Other.Kind), Object(Other.Object)
{
    if ((Kind == BAR_OFFER_MISSION || Kind == BAR_OFFER_BRIBE) && Object != nullptr)
        Object->AddRef();
}

// (06ea8cd0-06ea8d39). No self-assignment test in baseline.
FBarOffer& FBarOffer::operator=(FBarOffer const& Other)
{
    ReleaseObject();
    Object = nullptr;
    Kind = BAR_OFFER_NONE;
    Kind = Other.Kind;
    Object = Other.Object;
    if ((Kind == BAR_OFFER_MISSION || Kind == BAR_OFFER_BRIBE) && Object != nullptr)
        Object->AddRef();
    return *this;
}

// with the routine (06eb2060-06eb20c3).
FBarMissionOffer::FBarMissionOffer(FBarMissionOfferData const* InData)
{
    Data.Loader = InData->Loader;
    Data.Mission = InData->Mission;
    Data.Type = InData->Type.Data;
}

// (06ea7b90-06ea7bef).
FBarMissionOffer::~FBarMissionOffer()
{
    if (Data.Loader != nullptr)
    {
        Data.Loader->Slot05(Data.Mission);
        Data.Loader->Release();
        Data.Loader = nullptr;
    }
}

// (06ea7c30-06ea7c78).
FBarOffer::~FBarOffer()
{
    ReleaseObject();
    Object = nullptr;
    Kind = BAR_OFFER_NONE;
}

// (06ea7bf0-06ea7c28).
FBarCharacterDesc::FBarCharacterDesc()
{
    PlayerId = 0;
    Appearance.PartIds[0] = 0;
    Appearance.PartIds[1] = 0;
    Appearance.PartIds[2] = 0;
    Appearance.PartIds[3] = 0;
    Appearance.AccessoryCount = 0;
    Placement = -1;
    Role.Length = 0;
    Role.Data[0] = '\0';
    Fidget.Length = 0;
    Fidget.Data[0] = '\0';
    Nickname.clear();
}

// 06ea9361-06ea93af.
FBarCharacter::FBarCharacter()
    : UseCount(1), Behavior(0xffffffff), CharacterId(0), RoomId(0), Affiliation(0xffffffff),
      Placement(-1), PlayerId(0), Gender(-1), Voice(0), TalkCount(0)
{
    Role.Length = 0;
    Role.Data[0] = '\0';
    Nickname.clear();
    ++GBarCharacterCount;
}

// (06eb15b0-06eb16ea; the list node construct, a null node skips it): the
// members copied, then 06eb16e4 INC the field -- the copy counts like the ctor.
FBarCharacter::FBarCharacter(FBarCharacter const& Other)
    : UseCount(Other.UseCount), Behavior(Other.Behavior), Field_0x08(Other.Field_0x08),
      CharacterId(Other.CharacterId), RoomId(Other.RoomId), Affiliation(Other.Affiliation),
      Placement(Other.Placement), Role(Other.Role), PlayerId(Other.PlayerId),
      Gender(Other.Gender), Voice(Other.Voice), Nickname(Other.Nickname),
      TalkCount(Other.TalkCount), Offer(Other.Offer)
{
    ++GBarCharacterCount;
}

// (06ea7c80-06ea7cd8).
FBarCharacter::~FBarCharacter()
{
    --GBarCharacterCount;
}

// (06ea7d20-06ea7ed4).
void FBarCharacter::Create(FBarCharacterDesc const* Desc)
{
    // Local CharacterDescription at [ESP+0x10]: +0x38 0, +0x3c -1, +0x40 0, +0x44 -1,
    // +0x48 0, +0x4c 0, +0x50 0; the accessory slots past the count are not written in
    // baseline (stack garbage), zero here.
    CharacterDescription Character = {};
    Character.RoomId = Desc->RoomId;
    Character.Cost.PartIds[0] = Desc->Appearance.PartIds[0];
    Character.Cost.PartIds[1] = Desc->Appearance.PartIds[1];
    Character.Cost.PartIds[2] = Desc->Appearance.PartIds[2];
    Character.Cost.PartIds[3] = Desc->Appearance.PartIds[3];
    Character.Cost.AccessoryCount = Desc->Appearance.AccessoryCount;
    for (int Index = 0; Index < static_cast<int>(Desc->Appearance.AccessoryCount); ++Index)
        Character.Cost.AccessoryIds[Index] = Desc->Appearance.AccessoryIds[Index];
    Character.FactionId = 0xffffffff;
    Character.BehaviorId = 0xffffffff;

    pub::GF::GetCostumeSkeletonGender(Character.Cost, Gender);

    // +0x38 takes the individual name, +0x3c the affiliation.
    Character.NameIds = static_cast<unsigned int>(Desc->IndividualName);
    Character.FactionId = Desc->Affiliation;

    if (Desc->Fidget.Length == 0)
    {
        // the posture goes to the stack slot of the Desc argument and
        // the call's result is ignored -- a failed lookup leaves the pointer bits.
        unsigned long Posture = static_cast<unsigned long>(reinterpret_cast<uintptr_t>(Desc));
        pub::GF::GetCharacterPlacementPosture(Desc->RoomId, Desc->Placement, Posture);
        Character.Script = PickFidgetScript(Gender, static_cast<int>(Posture));
    }
    else
    {
        Character.Script = Desc->Fidget.Data;
    }

    Character.BehaviorId = 0xffffffff;
    if (Desc->Placement != -1)
        Character.Location = pub::GF::GetCharacterPlacementName(Desc->RoomId, Desc->Placement);
    else
        Character.Location = nullptr;
    // +0x4c takes the voice, +0x50 (session filter) the player.
    Character.VoiceId = Desc->Voice;
    Character.Filter = Desc->PlayerId;
    CharacterId = pub::GF::CharacterCreate(Character);

    PlayerId = Desc->PlayerId;
    Affiliation = Desc->Affiliation;
    RoomId = Desc->RoomId;
    Placement = Desc->Placement;
    Role = Desc->Role.Data;
    Nickname = Desc->Nickname;
    Voice = Desc->Voice;
}

// (06ea7ce0-06ea7d11).
void FBarCharacter::SetBehavior(unsigned long NewBehavior)
{
    if (Behavior != 0xffffffff)
        pub::GF::CharacterBehaviorDestroy(&Behavior);
    pub::GF::CharacterSetBehavior(CharacterId, NewBehavior);
    Behavior = NewBehavior;
}

// (06ea7ef0-06ea7f59).
void FBarCharacter::SetOffer(FBarOffer const* NewOffer)
{
    Offer = *NewOffer;
}

// (06ea8d50-06ea8f75, RET 8). Kind 3 (rumor) and 0 return true untouched.
bool FBarOffer::Accept(FContentPlayerState* State, FBarCharacter* Character)
{
    switch (Kind)
    {
    case BAR_OFFER_KNOW:
    {
        // the cash goes into the State argument slot; unsigned JB.
        int Cash = static_cast<int>(reinterpret_cast<intptr_t>(State));
        pub::Player::InspectCash(State->PlayerId, Cash);
        if (static_cast<unsigned int>(Cash) < static_cast<unsigned int>(Know->Price))
            return false;
        pub::Player::AdjustCash(State->PlayerId, -Know->Price);
        State->AddRumor(static_cast<unsigned int>(Know->Field_0x04));
        return true;
    }

    case BAR_OFFER_MISSION:
    {
        // . EH state 0 is the zeroed FmtStr local, never used.
        FBarMissionOffer* MissionOffer = static_cast<FBarMissionOffer*>(Object);
        FBarMissionOfferData* Data = &MissionOffer->Data;
        FmtStr Unused;
        FindContentPlayerState(State->PlayerId);                            // result unused
        if (State->GetGroupLiveMission() == 0)
        {
            unsigned int Controller = pub::Controller::Create(
                "content.dll", Data->Type.Data, Data->Mission, pub::Controller::ABOVE_NORMAL);
            State->JoinMissionAsGroup(Controller, State->GetBaseRecord()->BaseId,
                                      &Character->Nickname, false, 0);
            FMissionOfferAcceptMsg Msg;
            Msg.OfferId = Controller;
            Msg.Kind = 0;
            Msg.bAccepted = true;
            Msg.PlayerId = State->PlayerId;
            pub::Controller::_SendMessage(Controller, pub::Controller::EMessage::MissionResponse, &Msg);
        }
        return true;
    }

    case BAR_OFFER_BRIBE:
    {
        // the cash goes into the Character argument slot.
        FBarBribeOffer* Bribe = static_cast<FBarBribeOffer*>(Object);
        int Cash = static_cast<int>(reinterpret_cast<intptr_t>(Character));
        pub::Player::InspectCash(State->PlayerId, Cash);
        if (static_cast<unsigned int>(Cash) < static_cast<unsigned int>(Bribe->Price))
            return false;
        pub::Player::AdjustCash(State->PlayerId, -Bribe->Price);
        ApplyFactionReputationChange(State->GetReputation(), Bribe->Discount, Bribe->Group,
                                     true);
        RefreshMissionVendorOffers(State);
        return true;
    }

    default:
        return true;
    }
}

// (06eac160-06eac18b).
bool FBaseRoomState::HasLock(unsigned int PlayerId, int Placement) const
{
    for (std::list<FBarPlacementLock>::const_iterator It = Locks.begin(); It != Locks.end(); ++It)
    {
        if (It->PlayerId == PlayerId && It->Placement == Placement)
            return true;
    }
    return false;
}

// (06eac190-06eac1bb).
bool FBaseRoomState::HasCharacter(unsigned int PlayerId, int Placement) const
{
    for (FBarCharacterList::const_iterator It = Characters.begin(); It != Characters.end(); ++It)
    {
        if (It->PlayerId == PlayerId && It->Placement == Placement)
            return true;
    }
    return false;
}

// (06eac1c0-06eac23d).
void FBaseRoomState::AddLock(unsigned int PlayerId, int Placement)
{
    for (std::list<FBarPlacementLock>::iterator It = Locks.begin(); It != Locks.end(); ++It)
    {
        if (It->PlayerId == PlayerId && It->Placement == Placement)
        {
            ++It->Count;
            return;
        }
    }
    FBarPlacementLock Lock;
    Lock.PlayerId = PlayerId;
    Lock.Placement = Placement;
    Lock.Count = 1;
    Locks.push_back(Lock);
}

// (06eac240-06eac293).
bool FBaseRoomState::FindCharacter(TString<32> const* Role, unsigned int PlayerId,
                                   FBarCharacterList::iterator* OutCharacter)
{
    for (FBarCharacterList::iterator It = Characters.begin(); It != Characters.end(); ++It)
    {
        if (_stricmp(Role->Data, It->Role.Data) == 0 && PlayerId == It->PlayerId)
        {
            *OutCharacter = It;
            return true;
        }
    }
    return false;
}

// (06eac2a0-06eac2bd).
int FBaseRoomState::CountCharacters(unsigned int PlayerId) const
{
    int Count = 0;
    for (FBarCharacterList::const_iterator It = Characters.begin(); It != Characters.end(); ++It)
    {
        if (It->PlayerId == PlayerId)
            ++Count;
    }
    return Count;
}

// (06ea9340-06ea94b4).
FBarCharacterList::iterator FContentBase::AddBarCharacter(FBarCharacterDesc const* Desc)
{
    FBarCharacter Character;
    Character.Create(Desc);
    // insert {room, empty state} (the routine / the routine /
    // ); an existing entry is kept.
    std::map<unsigned int, FBaseRoomState>::iterator State =
        RoomStates.insert(std::pair<unsigned int const, FBaseRoomState>(Desc->RoomId,
                                                                        FBaseRoomState())).first;
    // the routine inserts before begin().
    State->second.Characters.push_front(Character);
    return State->second.Characters.begin();
}

// (06ea94c0-06ea95d9).
void FContentBase::RemoveBarCharacter(int /*Unused*/, FBarCharacterList::iterator Character,
                                      std::vector<FRtcScriptRole> const* KeepRoles)
{
    if (--Character->UseCount != 0)
        return;

    if (KeepRoles != nullptr)
    {
        // inline strcmp (case-sensitive) of each element's Role.Data
        // (+4, stride 0x3c) with the character's Role.
        for (std::vector<FRtcScriptRole>::const_iterator Keep = KeepRoles->begin();
             Keep != KeepRoles->end(); ++Keep)
        {
            if (strcmp(Keep->Role.Data, Character->Role.Data) == 0)
                return;
        }
    }

    // the room's state; baseline does not test the find.
    FBaseRoomState& State = RoomStates.find(Character->RoomId)->second;
    if (Character->Offer.Kind != BAR_OFFER_NONE)
    {
        if (Character->Behavior != 0xffffffff)
            pub::GF::CharacterBehaviorDestroy(&Character->Behavior);
        pub::GF::CharacterSetBehavior(Character->CharacterId, 0xffffffff);
        Character->Behavior = 0xffffffff;
    }
    unsigned int CharacterId = Character->CharacterId;
    pub::GF::CharacterDestroy(&CharacterId);
    State.Characters.erase(Character);
}

// (06eac2f0-06eac4ca).
void FBarScene::Release(std::vector<FRtcScriptRole> const* KeepRoles)
{
    bool bKeepFirst = false;
    if (KeepRoles != nullptr && KeepRoles->begin() != KeepRoles->end())
    {
        // the first character is read without an empty test (baseline
        // dereferences Characters' begin), then an inline strcmp of its Role with each
        // Role.Data (+4, stride 0x3c) until one matches.
        FBarCharacterList::iterator First = *Characters.begin();
        for (std::vector<FRtcScriptRole>::const_iterator Keep = KeepRoles->begin();
             Keep != KeepRoles->end(); ++Keep)
        {
            if (strcmp(First->Role.Data, Keep->Role.Data) == 0)
            {
                bKeepFirst = true;
                break;
            }
        }
    }

    // the first character loses the scene's behavior.
    if (Characters.begin() != Characters.end() && !bKeepFirst)
    {
        FBarCharacter& First = **Characters.begin();
        if (First.Behavior != 0xffffffff)
            pub::GF::CharacterBehaviorDestroy(&First.Behavior);
        pub::GF::CharacterSetBehavior(First.CharacterId, 0xffffffff);
        First.Behavior = 0xffffffff;
    }

    // the routine(SceneId, character, KeepRoles) for each.
    for (std::vector<FBarCharacterList::iterator>::iterator It = Characters.begin();
         It != Characters.end(); ++It)
        Base->RemoveBarCharacter(SceneId, *It, KeepRoles);
    Characters.clear();

    // inline find of the room (the routine); a missing room gives null,
    // which the lock loop below dereferences without a test.
    std::map<unsigned int, FBaseRoomState>::iterator Room = Base->RoomStates.find(RoomId);
    FBaseRoomState* State = Room != Base->RoomStates.end() ? &Room->second : nullptr;

    // one count off the lock {PlayerId, placement}; erased at zero.
    for (std::vector<int>::iterator Placement = Placements.begin();
         Placement != Placements.end(); ++Placement)
    {
        for (std::list<FBarPlacementLock>::iterator Lock = State->Locks.begin();
             Lock != State->Locks.end(); ++Lock)
        {
            if (Lock->PlayerId == PlayerId && Lock->Placement == *Placement)
            {
                if (--Lock->Count == 0)
                    State->Locks.erase(Lock);
                break;
            }
        }
    }
    Placements.clear();
}

// (06ea9640-06ea9f35). EH FuncInfo at the branch: the scene's two vectors,
// the behavior description's two FmtStr, the room-state temporaries, the character and the
// offer temporaries -- all RAII locals here.
int FContentBase::CreateBarScene(FCharacterEncounter const* Script, unsigned int PlayerId,
                                 unsigned int ControllerId)
{
    int const SceneId = GBarSceneNextId++;

    FBarScene Scene;                                                        // [ESP+0x30]
    Scene.Base = this;
    Scene.SceneId = SceneId;
    Scene.PlayerId = PlayerId;
    Scene.RoomId = Script->LocationId;
    // {SceneId, this} (the routine, unique insert; the result is dropped).
    GBarSceneBases.insert(std::pair<int const, FContentBase*>(SceneId, this));

    CharacterBehaviorDescription Behavior;
    Behavior.Field05 = static_cast<unsigned char>(Script->bAutoplay);
    Behavior.Field00 = static_cast<int>(Script->LocationId);
    Behavior.Field258 = Script->SequenceId;
    Behavior.Field06 = 0;
    Behavior.Field10 = 0;
    Behavior.Field04 = 1;

    // greet..offer (1, 0, 2, 3) paired with 0, then decision 1,
    // accept 2, decline 3 -- seven inlined copies of the routine.
    AppendBehaviorText(&Behavior, Script->FindActionText(1), 0);
    AppendBehaviorText(&Behavior, Script->FindActionText(0), 0);
    AppendBehaviorText(&Behavior, Script->FindActionText(2), 0);
    AppendBehaviorText(&Behavior, Script->FindActionText(3), 0);
    AppendBehaviorText(&Behavior, Script->FindActionText(4), 1);
    AppendBehaviorText(&Behavior, Script->FindActionText(5), 2);
    AppendBehaviorText(&Behavior, Script->FindActionText(6), 3);

    Behavior.Field0c = Script->StartRoomLocationId;
    Behavior.IdArrayCount = 0;
    Behavior.Field08 = Script->RelocateLocationId;
    Behavior.NameFmtStr.begin_mad_lib(static_cast<unsigned int>(Script->MissionTextId));
    Behavior.NameFmtStr.end_mad_lib();
    // 3 with an offer text (pointer test only), 5 without.
    Behavior.Field21c = Script->FindActionText(3) != nullptr ? 3 : 5;
    Behavior.Filter = PlayerId;
    Behavior.Field220 = ControllerId;
    Behavior.Field224 = 0;

    // insert {room, empty state} (the routine / the routine /
    // ); an existing entry is kept.
    FBaseRoomState* RoomState =
        &RoomStates.insert(std::pair<unsigned int const, FBaseRoomState>(Script->LocationId,
                                                                         FBaseRoomState()))
             .first->second;

    // capacity test (the routine) and reallocation.
    Scene.Placements.reserve(Script->ReservedSpots.size());
    for (std::vector<int>::const_iterator Spot = Script->ReservedSpots.begin();
         Spot != Script->ReservedSpots.end(); ++Spot)
    {
        RoomState->AddLock(PlayerId, *Spot);
        Scene.Placements.push_back(*Spot);
    }

    // the roles from the last to the first.
    int const RoleCount = static_cast<int>(Script->Roles.size());
    for (int Index = RoleCount - 1; Index >= 0; --Index)
    {
        FRtcScriptRole const* Role = &Script->Roles[Index];
        FBarCharacterList::iterator Character;
        bool bFound = false;
        // FBaseRoomState::FindCharacter (the routine) inlined.
        if (Role->Role.Length != 0)
            bFound = RoomState->FindCharacter(&Role->Role, PlayerId, &Character);

        if (bFound)
        {
            // .
            ++Character->UseCount;
            Behavior.StringArray[Index] = Character->Role.Data;
            Behavior.IdArray[Index] = Character->CharacterId;
        }
        else
        {
            // the description from the role's specific NPC record.
            FBarCharacterDesc Desc;                                         // inlined
            FNpcCharacterDesc const* Npc = Role->Character;
            Desc.PlayerId = PlayerId;
            Desc.IndividualName = Npc->IndividualName;
            Desc.Affiliation = Npc->Affiliation;
            Desc.Appearance.PartIds[0] = Npc->Appearance.PartIds[0];
            Desc.Appearance.PartIds[1] = Npc->Appearance.PartIds[1];
            Desc.Appearance.PartIds[2] = Npc->Appearance.PartIds[2];
            Desc.Appearance.PartIds[3] = Npc->Appearance.PartIds[3];
            Desc.Appearance.AccessoryCount = Npc->Appearance.AccessoryCount;
            for (int Accessory = 0;
                 Accessory < static_cast<int>(Desc.Appearance.AccessoryCount); ++Accessory)
            {
                Desc.Appearance.AccessoryIds[Accessory] = Npc->Appearance.AccessoryIds[Accessory];
            }
            Desc.RoomId = Script->LocationId;
            Desc.Voice = Npc->Voice;
            Desc.Placement = Role->Spot;
            Desc.Role = Role->Role.Data;
            Desc.Fidget = Role->Fidget.c_str();

            // the routine's body inlined -- create, then put it at the
            // front of the room's list (the routine operator[], the routine).
            FBarCharacter NewCharacter;
            NewCharacter.Create(&Desc);
            Behavior.StringArray[Index] = Role->Role.Data;
            Behavior.IdArray[Index] = NewCharacter.CharacterId;
            FBaseRoomState& State = RoomStates[Script->LocationId];
            State.Characters.push_front(NewCharacter);
            Character = State.Characters.begin();
        }

        ++Behavior.IdArrayCount;
        if (Index == 0)
        {
            // the scene's behavior goes to the first role's character,
            // whose offer is dropped (the routine from a kind-0 temporary).
            unsigned long const Handle = pub::GF::CharacterBehaviorCreate(Behavior);
            Character->Offer = FBarOffer();
            Character->SetBehavior(Handle);                                 // inlined
        }
        else if (Character->Offer.Kind == BAR_OFFER_NONE)
        {
            // .
            Character->SetBehavior(0xffffffff);                             // inlined
        }
        // the routine insert(begin, 1, &Character).
        Scene.Characters.insert(Scene.Characters.begin(), Character);
    }

    // the routine insert(end) with the the routine copy.
    BarScenes.push_back(Scene);
    return SceneId;
}

// (06ea9fc0-06eaa0e7).
void FContentBase::RemoveBarScene(int SceneId, std::vector<FRtcScriptRole> const* KeepRoles)
{
    for (std::list<FBarScene>::iterator Scene = BarScenes.begin(); Scene != BarScenes.end();
         ++Scene)
    {
        if (Scene->SceneId == SceneId)
        {
            Scene->Release(KeepRoles);
            BarScenes.erase(Scene);             // unlink, the routine, free, --size
            break;
        }
    }
    // equal_range of the key; clear() when the range is the whole map,
    // else erase element by element -- std::map::erase(key).
    GBarSceneBases.erase(SceneId);
}

// (06ea95e0-06ea9639).
bool FContentBase::FindBarCharacter(FBarCharacterList::iterator* OutCharacter,
                                    FBaseRoomState** OutRoomState, unsigned int CharacterId)
{
    for (std::map<unsigned int, FBaseRoomState>::iterator State = RoomStates.begin();
         State != RoomStates.end(); ++State)
    {
        for (FBarCharacterList::iterator It = State->second.Characters.begin();
             It != State->second.Characters.end(); ++It)
        {
            if (It->CharacterId == CharacterId)
            {
                *OutCharacter = It;
                *OutRoomState = &State->second;
                return true;
            }
        }
    }
    return false;
}

// (06ea9310-06ea9339).
FSystemInfo* FContentBase::GetSystemInfo() const
{
    unsigned int SystemId;
    if (pub::GetSystem(SystemId, BaseId) != 0)
        return nullptr;
    return FindSystemInfo(&SystemId);
}

namespace
{
    // 's local enumerator (vtable slot 0 the branch, RET 4): {+4 base,
    // +8 &ids name, +0xc &position, +0x10 &nickname id}.
    struct FBaseDockObjectFinder : public pub::System::SysObjEnumerator
    {
        unsigned int BaseId = 0;
        int* OutIdsName = nullptr;
        Vector* OutPosition = nullptr;
        unsigned int* OutNickname = nullptr;

        // (06eac560-06eac5a3): dock_with (+0x58) != base -> go on; else
        // nickname id (pub::GetNicknameId of the record's name), ids name (+0x30),
        // position (+0x20), stop.
        bool Slot00(pub::System::FSysObj const* Object) override
        {
            if (Object->DockWith != BaseId)
                return true;
            *OutNickname = pub::GetNicknameId(Object->Nickname);
            *OutIdsName = Object->IdsName;
            *OutPosition = Object->Pos;
            return false;
        }
    };
}

// (06eac4d0-06eac54e).
unsigned int FContentBase::FindDockObject(Vector* OutPosition) const
{
    unsigned int Nickname = 0;
    int IdsName = 0;
    FBaseDockObjectFinder Finder;
    Finder.BaseId = BaseId;
    Finder.OutIdsName = &IdsName;
    Finder.OutPosition = OutPosition;
    Finder.OutNickname = &Nickname;

    // pub::GetSystem into the argument slot, then the routine; a failed
    // lookup gives a null SystemInfo whose +0x48 is read anyway (baseline does not test it).
    unsigned int SystemId;
    FSystemInfo* System = nullptr;
    if (pub::GetSystem(SystemId, BaseId) == 0)
        System = FindSystemInfo(&SystemId);
    pub::System::EnumerateObjects(System->SystemId, Finder);
    return Nickname;
}

// (06eac2c0-06eac2e7).
int FContentBase::GetRoomDensity(unsigned int RoomId) const
{
    for (std::vector<FBaseRoom>::const_iterator It = Rooms.begin(); It != Rooms.end(); ++It)
    {
        if (It->RoomId == RoomId)
            return It->CharacterDensity;
    }
    return 0;
}

// (06eaa0f0-06eaa24c, RET 4). The count local is unset in baseline when the
// first Market call fails; the port starts it at 0. Count > 0: a temporary id buffer
// (operator new(count * 4)), the out vector reserved to count (06eaa15c-06eaa1d7), one
// insert(end, 1, {id, 1}) per id, the buffer freed.
void FContentBase::GetCommoditiesForSale(std::vector<FBaseCommodity>* Out) const
{
    int Count = 0;
    pub::Market::GetNumCommoditiesForSale(BaseId, &Count);
    if (Count == 0)
    {
        Out->clear();
        return;
    }
    std::vector<unsigned int> Goods(static_cast<size_t>(Count));
    pub::Market::GetCommoditiesForSale(BaseId, Goods.data(), &Count);
    if (Out->capacity() < static_cast<size_t>(Count))
        Out->reserve(static_cast<size_t>(Count));
    for (int Index = 0; Index < Count; ++Index)
    {
        FBaseCommodity const Entry = {Goods[Index], 1};
        Out->insert(Out->end(), 1, Entry);
    }
}

// (06eaa250-06eaa3cd, RET 4): as GetCommoditiesForSale over the goods in
// demand; the second dword is pub::Market::GetMinInventory(BaseId, good) (its out local is
// unset in baseline on a failure; the port starts it at 0).
void FContentBase::GetCommoditiesInDemand(std::vector<FBaseCommodity>* Out) const
{
    int Count = 0;
    pub::Market::GetNumCommoditiesInDemand(BaseId, &Count);
    if (Count == 0)
    {
        Out->clear();
        return;
    }
    std::vector<unsigned int> Goods(static_cast<size_t>(Count));
    pub::Market::GetCommoditiesInDemand(BaseId, Goods.data(), &Count);
    if (Out->capacity() < static_cast<size_t>(Count))
        Out->reserve(static_cast<size_t>(Count));
    for (int Index = 0; Index < Count; ++Index)
    {
        int MinInventory = 0;
        pub::Market::GetMinInventory(BaseId, Goods[Index], MinInventory);
        FBaseCommodity const Entry = {Goods[Index], static_cast<unsigned int>(MinInventory)};
        Out->insert(Out->end(), 1, Entry);
    }
}

// (06eaa3e0-06eaa77f).
void FContentBase::OnPlayerEnter(unsigned int PlayerId)
{
    for (std::vector<FBaseRoom>::iterator Room = Rooms.begin(); Room != Rooms.end(); ++Room)
    {
        std::list<FBaseFixture*>& Fixtures = Room->Fixtures.Fixtures;
        for (std::list<FBaseFixture*>::iterator Node = Fixtures.begin(); Node != Fixtures.end();
             ++Node)
        {
            // a counted local reference, EH state 0 of FuncInfo 06fbde58
            // (the routine on [esp+0x10]); released at the end of the iteration by
            // (06eaa69d, skip paths) or vtbl[+4] (06eaa723-06eaa73b).
            TRefHandle<FBaseFixture> Fixture;
            Fixture = *Node;
            if (Fixture != nullptr)
                Fixture->AddRef();

            bool bSkip = false;
            std::map<unsigned int, FBaseRoomState>::iterator StateIt =
                RoomStates.find(Room->RoomId);
            if (StateIt != RoomStates.end())
            {
                FBaseRoomState& State = StateIt->second;
                // a lock on (player, placement) -> nothing.
                if (State.HasLock(PlayerId, Fixture->Placement))
                {
                    bSkip = true;
                }
                else
                {
                    // , 06eaa671-06eaa689: the player's character in this
                    // role is reused: one more use, one more visit reference.
                    for (FBarCharacterList::iterator It = State.Characters.begin();
                         It != State.Characters.end(); ++It)
                    {
                        if (_stricmp(Fixture->Role.Data, It->Role.Data) == 0 &&
                            PlayerId == It->PlayerId)
                        {
                            ++It->UseCount;
                            State.CharacterRefs.push_back(It);
                            bSkip = true;
                            break;
                        }
                    }
                }
            }

            if (!bSkip)
            {
                // the character description from the fixture's GF_NPC.
                FBaseNpc const* Npc = Fixture->Npc;
                FBarCharacterDesc Desc;
                Desc.PlayerId = PlayerId;
                Desc.Affiliation = Npc->Desc.Affiliation;
                Desc.IndividualName = Npc->Desc.IndividualName;
                Desc.Appearance.PartIds[0] = Npc->Desc.Appearance.PartIds[0];
                Desc.Appearance.PartIds[1] = Npc->Desc.Appearance.PartIds[1];
                Desc.Appearance.PartIds[2] = Npc->Desc.Appearance.PartIds[2];
                Desc.Appearance.PartIds[3] = Npc->Desc.Appearance.PartIds[3];
                Desc.Appearance.AccessoryCount = Npc->Desc.Appearance.AccessoryCount;
                for (int Index = 0; Index < static_cast<int>(Desc.Appearance.AccessoryCount);
                     ++Index)
                {
                    Desc.Appearance.AccessoryIds[Index] = Npc->Desc.Appearance.AccessoryIds[Index];
                }
                Desc.Voice = Npc->Desc.Voice;
                Desc.RoomId = Room->RoomId;
                Desc.Placement = Fixture->Placement;
                Desc.Role = Fixture->Role.Data;
                Desc.Fidget = Fixture->Fidget.Data;
                Desc.Nickname = Npc->Desc.Nickname;

                FBarCharacterList::iterator Character = AddBarCharacter(&Desc);
                // the state is looked up again; baseline does not test the
                // result (AddBarCharacter has just created it).
                RoomStates.find(Room->RoomId)->second.CharacterRefs.push_back(Character);

                FBarOffer Offer;
                Npc->ChooseOffer(&Offer, FindContentPlayerState(PlayerId));
                Character->Offer = Offer;
                StartBarCharacterScript(Character);
            }
            // The handle releases the fixture here.
        }
    }
}

// (06eaa790-06eaa814).
void FContentBase::OnPlayerExit(unsigned int PlayerId)
{
    for (std::map<unsigned int, FBaseRoomState>::iterator State = RoomStates.begin();
         State != RoomStates.end(); ++State)
    {
        std::list<FBarCharacterList::iterator>& Refs = State->second.CharacterRefs;
        std::list<FBarCharacterList::iterator>::iterator Ref = Refs.begin();
        while (Ref != Refs.end())
        {
            if ((*Ref)->PlayerId == PlayerId)
            {
                RemoveBarCharacter(-1, *Ref, nullptr);
                Ref = Refs.erase(Ref);
            }
            else
            {
                ++Ref;
            }
        }
    }
}

// (06ea8370-06ea8ca9).
void FBaseNpc::ChooseOffer(FBarOffer* Out, FContentPlayerState* Player) const
{
    Out->Kind = BAR_OFFER_NONE;
    std::vector<FBarOfferChoice> Choices;
    Choices.reserve(5);

    // a mission when the game mode allows mission_type (the MP flag is
    // tested outside single player, the SP flag inside it).
    bool bMissionsEnabled;
    if (!pub::SinglePlayer() && GBaseMissionTypesInMultiplayer)
        bMissionsEnabled = true;
    else
        bMissionsEnabled = pub::SinglePlayer() && GBaseMissionTypesInSinglePlayer;
    if (bMissionsEnabled && !Missions.empty())
    {
        FBarOfferChoice Choice;
        Choice.Kind = BAR_OFFER_MISSION;
        Choice.Weight = 30.0f;
        Choices.push_back(Choice);
    }

    // the player's feeling towards this character's affiliation.
    float Feeling = 0.0f;
    int Reputation = Player->GetReputation();
    pub::Reputation::GetReputation(Reputation, Desc.Affiliation, Feeling);

    for (std::vector<FBaseNpcBribe>::const_iterator It = Bribes.begin(); It != Bribes.end(); ++It)
    {
        if (It->IsOffered(Player))
        {
            FBarOfferChoice Choice;
            Choice.Kind = BAR_OFFER_BRIBE;
            Choice.Weight = 20.0f;
            Choices.push_back(Choice);
            break;
        }
    }

    for (std::vector<FBaseNpcKnow>::const_iterator It = Knows.begin(); It != Knows.end(); ++It)
    {
        if (It->IsOffered(Player, Feeling))
        {
            FBarOfferChoice Choice;
            Choice.Kind = BAR_OFFER_KNOW;
            Choice.Weight = 15.0f;
            Choices.push_back(Choice);
            break;
        }
    }

    // the best rumor weight (unsigned JBE), FILD as int.
    unsigned int BestRumor = 0;
    for (std::vector<FBaseNpcRumor>::const_iterator It = Rumors.begin(); It != Rumors.end(); ++It)
    {
        unsigned int Weight = static_cast<unsigned int>(It->GetWeight(Player, Feeling));
        if (Weight > BestRumor)
            BestRumor = Weight;
    }
    if (BestRumor != 0)
    {
        FBarOfferChoice Choice;
        Choice.Kind = BAR_OFFER_RUMOR;
        Choice.Weight = static_cast<float>(static_cast<int>(BestRumor));
        Choices.insert(Choices.end(), 1, Choice);
    }

    if (Choices.empty())
        return;

    // the weights are all positive and the pick is below their sum, so
    // an element is always found; baseline would read *end() otherwise.
    float PickSlot = 0.0f;
    std::vector<FBarOfferChoice>::const_iterator Chosen = PickWeighted(Choices, &PickSlot);
    if (Chosen == Choices.end())
        return;

    switch (Chosen->Kind)
    {
    case BAR_OFFER_MISSION:
    {
        // .
        std::vector<FBaseNpcMission>::const_iterator Mission = PickRandom(Missions);
        unsigned int PlayerId = Player->PlayerId;
        // the ship id lives in the pick's slot; a failed query leaves its bits.
        unsigned int ShipId;
        memcpy(&ShipId, &PickSlot, sizeof(ShipId));
        pub::Player::GetShipID(PlayerId, ShipId);
        if (Mission == Missions.end() || ShipId == 0)
            break;

        // .
        FBarMissionOfferData OfferData;
        if (CreateBarMissionOffer(&*Mission, Player, Player->GetBaseRecord()->BaseId,
                                  Desc.Affiliation, &OfferData))
        {
            Out->Kind = BAR_OFFER_MISSION;
            Out->Object = new FBarMissionOffer(&OfferData);
        }
        else
        {
            // baseline does not test the loader (the routine stores 0 only when new fails).
            OfferData.Loader->Slot05(OfferData.Mission);
            OfferData.Loader->Release();
        }
        break;
    }
    case BAR_OFFER_BRIBE:
    {
        // a random offered bribe of a local copy (not offered ones are
        // erased and the pick repeats).
        std::vector<FBaseNpcBribe> Candidates(Bribes);
        std::vector<FBaseNpcBribe>::const_iterator Pick = PickRandom(Candidates);
        while (!Pick->IsOffered(Player))
        {
            Candidates.erase(Candidates.begin() + (Pick - Candidates.cbegin()));
            Pick = PickRandom(Candidates);
        }

        FBarBribeOffer* Bribe = new FBarBribeOffer;
        Out->Object = Bribe;
        Bribe->Discount = GetFeelingShortfall(Player->GetReputation(), Pick->Group, 0.6f);
        // FILD qword {cost, 0} * discount * 10.0 (the field), ftol.
        double const Cost = static_cast<double>(static_cast<unsigned int>(Pick->Cost));
        Bribe->Price = static_cast<int>(Cost * Bribe->Discount * 10.0);
        Bribe->Price = static_cast<int>(RoundRmMoney(static_cast<unsigned int>(Bribe->Price)));
        Bribe->Group = Pick->Group;
        Bribe->TextIds = Pick->TextIds;
        // the routine(object +0xc, object +0x10, 3).
        GetFactionHostileGroups(Bribe->Group, Bribe->Ids, 3);
        Out->Kind = BAR_OFFER_BRIBE;
        break;
    }
    case BAR_OFFER_KNOW:
    {
        // the same repeat-until-offered pick over a copy of Knows, then
        // the first element of Knows equal to it (REPE CMPSD, 4 dwords).
        std::vector<FBaseNpcKnow> Candidates(Knows);
        std::vector<FBaseNpcKnow>::const_iterator Pick = PickRandom(Candidates);
        while (!Pick->IsOffered(Player, Feeling))
        {
            Candidates.erase(Candidates.begin() + (Pick - Candidates.cbegin()));
            Pick = PickRandom(Candidates);
        }
        std::vector<FBaseNpcKnow>::const_iterator Match = Knows.begin();
        for (; Match != Knows.end(); ++Match)
        {
            if (memcmp(&*Match, &*Pick, sizeof(FBaseNpcKnow)) == 0)
                break;
        }
        // the scan's pointer as is (end() when nothing matched; the pick is a copy
        // of a Knows element, so a match always exists).
        Out->Know = Knows.data() + (Match - Knows.begin());
        Out->Kind = BAR_OFFER_KNOW;
        break;
    }
    case BAR_OFFER_RUMOR:
    {
        // every rumor with a weight, then the routine.
        std::vector<FBarRumorChoice> RumorChoices;
        RumorChoices.reserve(Rumors.size());
        for (std::vector<FBaseNpcRumor>::const_iterator It = Rumors.begin(); It != Rumors.end();
             ++It)
        {
            int Weight = It->GetWeight(Player, Feeling);
            if (Weight != 0)
            {
                FBarRumorChoice Choice;
                Choice.Rumor = &*It;
                // FILD qword {weight, 0}.
                Choice.Weight = static_cast<float>(static_cast<double>(static_cast<unsigned int>(Weight)));
                RumorChoices.insert(RumorChoices.end(), Choice);
            }
        }
        std::vector<FBarRumorChoice>::const_iterator Rumor = PickWeighted(RumorChoices);
        Out->Rumor = (Rumor != RumorChoices.end()) ? Rumor->Rumor : nullptr;
        Out->Kind = BAR_OFFER_RUMOR;
        break;
    }
    default:
        break;
    }
}
