// Author: Giperion.

#include "RandomMissionGen.h"

#include "ContentInternal.h"
#include "ContentStaticIds.h"
#include "RandomEncounters.h"
#include "RandomMissionDB.h"
#include "Cargo.h"              // GetFactionShipLineArchs (TU44 the routine)
#include "EncounterParams.h"    // FEncounterShip
#include "Encounters.h"         // FindFormation (the routine)
#include "FactionProps.h"       // GetFactionProps, GetFactionNames
#include "NpcNames.h"           // FNpcNameRequest, BuildNpcName
#include "NpcShips.h"           // FNpcShipArch
#include "PilotPersonalities.h" // GetPilotPersonality (the routine)
#include "RandomMission.h"      // TRmRef overloads for the offer
#include "RandomMissionOffer.h"
#include "SystemInfo.h"         // FSystemInfo::SystemId (GetRmZoneAtPosition)
#include "common_ini.h"
#include "common_misc.h"        // random_inside_ring (the routine)
#include "common_server.h"
#include "common_weighted_vector.h"
#include "msvcrt_compat.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <list>
#include <memory>
#include <stdlib.h>
#include <string.h>

// =======================================================================================
// TU63 -- solar formation pick
// =======================================================================================

// (06f97d90-06f9800b). Walks the field (the routine) in key order. Per
// formation the mandatory (the routine) and optional (the routine) slot types are
// collected; the formation is a candidate when every mandatory type is in SpawnTypes
// (the routine != end) and every SpawnTypes entry is mandatory or optional. The 0x40th
// candidate ends the walk at once (06f97ee7). No candidate -> null; otherwise
// candidates[(rand() * count) / 0x8000] (IMUL, CDQ, AND EDX,0x7fff, ADD, SAR 0xf).
FRmSolarFormation* __cdecl PickSolarFormation(std::vector<int> const* SpawnTypes)
{
    FRmSolarFormation* Candidates[0x40];
    int CandidateCount = 0;

    std::map<unsigned int, FRmSolarFormation>* Formations = GetSolarFormations();
    std::map<unsigned int, FRmSolarFormation>::iterator It = Formations->begin();
    if (It == Formations->end())
        return nullptr;

    for (; It != Formations->end(); ++It)
    {
        FRmSolarFormation* Formation = &It->second;
        std::vector<int> Mandatory;
        std::vector<int> Optional;
        Formation->CollectMandatorySlotTypes(&Mandatory);
        Formation->CollectOptionalSlotTypes(&Optional);

        bool bMatches = true;
        for (std::vector<int>::const_iterator Type = Mandatory.begin(); Type != Mandatory.end();
             ++Type)
        {
            if (std::find(SpawnTypes->begin(), SpawnTypes->end(), *Type) == SpawnTypes->end())
            {
                bMatches = false;
                break;
            }
        }
        if (bMatches)
        {
            for (std::vector<int>::const_iterator Type = SpawnTypes->begin();
                 Type != SpawnTypes->end(); ++Type)
            {
                if (std::find(Mandatory.begin(), Mandatory.end(), *Type) != Mandatory.end())
                    continue;
                if (std::find(Optional.begin(), Optional.end(), *Type) == Optional.end())
                {
                    bMatches = false;
                    break;
                }
            }
        }
        if (!bMatches)
            continue;

        Candidates[CandidateCount] = Formation;
        ++CandidateCount;
        if (CandidateCount == 0x40)
            break;
    }

    if (CandidateCount <= 0)
        return nullptr;

    int const Product = rand() * CandidateCount;
    int const Index = (Product + ((Product >> 31) & 0x7fff)) >> 15;
    return Candidates[Index];
}

// (06f98010-06f98267, cdecl). pub::System::InZones(System +0x48, *Position,
// 1.0, 0x14 slots); a vector of {zone, radius} reserved for the count (06f980a4-06f98123);
// every zone whose pub::Zone::GetName succeeds with a non-zero name adds {zone,
// pub::Zone::GetRadius(zone)} (the routine). Best radius starts at -1.0 (0xbf800000): a
// pair is taken when its radius is below it (TEST AH,0x5 + JNP: C0 alone) or the best is
// still -1.0 (the field; TEST AH,0x44 + JP skips unless C3 alone). A found zone is
// returned; otherwise the navmap cell: Size = 133333.0 (the field) / GetNavMapScale,
// Cell = Size * 0.25 (the field), both stored as float; column = ftol(floor((x + Size) /
// Cell)), row = ftol(floor((z + Size) / Cell)) (x87, the quotient stored as double for
// floor); result row + column * 8 + 1.
unsigned int __cdecl GetRmZoneAtPosition(FSystemInfo const* System, Vector const* Position)
{
    Vector const Where = *Position;
    unsigned int Zones[0x14];
    unsigned int ZoneCount = 0;
    pub::System::InZones(System->SystemId, Where, 1.0f, Zones, 0x14, ZoneCount);

    std::vector<std::pair<unsigned int, float>> Named;
    if (ZoneCount > 0)
        Named.reserve(ZoneCount);
    for (unsigned int Index = 0; Index < ZoneCount; ++Index)
    {
        unsigned int NameId = 0;
        if (pub::Zone::GetName(Zones[Index], NameId) == 0 && NameId != 0)
        {
            float const Radius = pub::Zone::GetRadius(Zones[Index]);
            Named.insert(Named.end(), 1, std::pair<unsigned int, float>(Zones[Index], Radius));
        }
    }

    unsigned int BestZone = 0;
    float BestRadius = -1.0f;
    for (std::vector<std::pair<unsigned int, float>>::const_iterator It = Named.begin();
         It != Named.end(); ++It)
    {
        if (It->second < BestRadius || BestRadius == -1.0f)
        {
            BestRadius = It->second;
            BestZone = It->first;
        }
    }
    if (BestZone != 0)
        return BestZone;

    float Scale = 0.0f;
    pub::GetNavMapScale(System->SystemId, Scale);
    float const Size = 133333.0f / Scale;
    float const Cell = Size * 0.25f;
    int const Column = static_cast<int>(
        std::floor((static_cast<double>(Where.x) + static_cast<double>(Size)) /
                   static_cast<double>(Cell)));
    int const Row = static_cast<int>(
        std::floor((static_cast<double>(Where.z) + static_cast<double>(Size)) /
                   static_cast<double>(Cell)));
    return static_cast<unsigned int>(Row + Column * 8 + 1);
}

// =======================================================================================
// TU63 -- NpcRankToDiff / Diff2Money
// =======================================================================================

std::map<int, std::vector<float>> GRmNpcRankToDifficulty;
std::map<float, int> GRmDiff2Money;

// [RankAndFormationSizeToDifficulty] "NPCRank = rank, diff1, diff2, ...".
// Each line inserts {rank, empty vector} (the routine, an existing rank is kept), looks
// the rank up again (the routine) and appends every value from index 1 on (the routine).
void __cdecl LoadNpcRankToDifficulty(INI_Reader* Reader)
{
    while (Reader->read_header())
    {
        if (!Reader->is_header("RankAndFormationSizeToDifficulty") || !Reader->read_value())
            continue;
        do
        {
            if (Reader->is_value("NPCRank"))
            {
                int Rank = Reader->get_value_int(0);
                GRmNpcRankToDifficulty.insert(std::make_pair(Rank, std::vector<float>()));
                std::vector<float>& Difficulties = GRmNpcRankToDifficulty[Rank];
                for (unsigned int Index = 1; !Reader->is_value_empty(Index); ++Index)
                    Difficulties.insert(Difficulties.end(), Reader->get_value_float(Index));
            }
        } while (Reader->read_value());
    }
}

// walks the field in key order to the first difficulty greater than
// Difficulty (FCOMP + TEST AH,0x41 + JZ: key > Difficulty), then interpolates between the
// previous entry and that one on the x87 stack and truncates (ftol) before the routine:
//   lo.money + (Difficulty - lo.key) / (hi.key - lo.key) * (hi.money - lo.money)
// with the money difference an int (FIMUL) and lo.money added as an int (FIADD).
unsigned int __cdecl RmDifficultyToMoney(float Difficulty)
{
    std::map<float, int>::iterator Hi = GRmDiff2Money.begin();
    while (Hi != GRmDiff2Money.end() && !(Hi->first > Difficulty))
        ++Hi;

    if (Hi == GRmDiff2Money.end())
    {
        // baseline returns the map head node's value slot (+0x10), which the VC6
        // tree never constructs. TODO: no defined value exists; 0 is returned instead.
        return 0;
    }

    // the routine (iterator decrement, no nil check on the upward walk). From
    // begin() baseline steps onto the head node and reads its unconstructed key/value; only
    // when begin() is the root does the walk come back to begin() itself (lo == hi: x/0,
    // FIMUL 0 -> NaN, ftol -> 0). TODO: undefined case (needs Difficulty below the first
    // Diff2Money key, 0 in DATA); the port interpolates from {0, 0}.
    float LoKey = 0.0f;
    int LoMoney = 0;
    if (Hi != GRmDiff2Money.begin())
    {
        std::map<float, int>::iterator Lo = Hi;
        --Lo;
        LoKey = Lo->first;
        LoMoney = Lo->second;
    }

    double Fraction = (static_cast<double>(Difficulty) - static_cast<double>(LoKey)) /
                      (static_cast<double>(Hi->first) - static_cast<double>(LoKey));
    int Money = static_cast<int>(Fraction * static_cast<double>(Hi->second - LoMoney) +
                                 static_cast<double>(LoMoney));
    return RoundRmMoney(static_cast<unsigned int>(Money));
}

// TU44 the routine (inspect-derived): for Amount > 100, split the decimal digits
// (least significant first), keep the two leading ones and, when the leading digit is
// below 5, add 5 (third digit >= 5) or 0 in the unit below the second digit.
unsigned int __cdecl RoundRmMoney(unsigned int Amount)
{
    if (Amount > 100)
    {
        unsigned int Digits[16];
        unsigned int Power = 1;
        int Count = 0;
        for (; Amount != 0; Amount = Amount / 10)
        {
            Power = Power * 10;
            Digits[Count] = Amount % 10;
            ++Count;
        }
        unsigned int Second = (Power / 10) / 10;
        Amount = Digits[Count - 1] * (Power / 10) + Digits[Count - 2] * Second;
        if (Digits[Count - 1] < 5)
        {
            unsigned int Third = Digits[Count - 3] < 5 ? 0u : 5u;
            Amount = Amount + Third * (Second / 10);
        }
    }
    return Amount;
}

// =======================================================================================
// TU63 -- all reputation groups (the field)
// =======================================================================================

FRmAllGroupsEnumerator* GRmAllGroups = nullptr;


FRmAllGroupsEnumerator::FRmAllGroupsEnumerator()
    : bEnumerated(false)
{
    GRmAllGroups = this;
}

// frees the vector and clears the field.
FRmAllGroupsEnumerator::~FRmAllGroupsEnumerator()
{
    GRmAllGroups = nullptr;
}

// linear find; an absent group is appended (the routine). Always 1.
char FRmAllGroupsEnumerator::Execute(unsigned int GroupId)
{
    std::vector<unsigned int>::iterator It = Groups.begin();
    for (; It != Groups.end(); ++It)
    {
        if (*It == GroupId)
            break;
    }
    if (It == Groups.end())
        Groups.insert(Groups.end(), GroupId);
    return 1;
}

static FRmAllGroupsEnumerator GRmAllGroupsInstance;

// =======================================================================================
// TU63 -- generator root (the field)
// =======================================================================================

FRandomMissionGenerator* GRandomMissionGenerator = nullptr;

// published before the members are built.
FRmGeneratorRegistration::FRmGeneratorRegistration(FRandomMissionGenerator* Generator)
{
    GRandomMissionGenerator = Generator;
}

// (also inline at 06f9da41, after the generator's members).
FRmGeneratorRegistration::~FRmGeneratorRegistration()
{
    GRandomMissionGenerator = nullptr;
}

// . Every table is opened with the routine and closed only when it opened;
// the reader is the ctor's stack INI_Reader (EH states 3..6 = its dtor).
FRandomMissionGenerator::FRandomMissionGenerator()
    : FRmGeneratorRegistration(this)
    , bLoaded(false)
    , bLinked(false)
    , RootNodeId(0xffffffffu)
{
    INI_Reader Reader;
    if (OpenContentDataIni("RandomMissions\\NpcRankToDiff.ini", &Reader))
    {
        LoadNpcRankToDifficulty(&Reader);
        Reader.close();
    }

    // [Diff2Money] "Diff2Money = difficulty, money" -> the routine
    // (unique insert, value 0 float, value 1 int).
    if (OpenContentDataIni("RandomMissions\\Diff2Money.ini", &Reader))
    {
        while (Reader.read_header())
        {
            if (!Reader.is_header("Diff2Money") || !Reader.read_value())
                continue;
            do
            {
                if (Reader.is_value("Diff2Money"))
                {
                    float Difficulty = Reader.get_value_float(0);
                    int Money = Reader.get_value_int(1);
                    GRmDiff2Money.insert(std::make_pair(Difficulty, Money));
                }
            } while (Reader.read_value());
        }
        Reader.close();
    }

    if (OpenContentDataIni("RandomMissions\\RMLootInfo.ini", &Reader))
    {
        LoadRmLootInfo(&Reader);
        Reader.close();
    }
    if (OpenContentDataIni("RandomMissions\\SolarFormations.ini", &Reader))
    {
        LoadSolarFormations(&Reader);
        Reader.close();
    }
    if (OpenContentDataIni("RandomMissions\\KillableSolars.ini", &Reader))
    {
        LoadKillableSolars(&Reader);
        Reader.close();
    }
    if (OpenContentDataIni("RandomMissions\\VignetteCriticalLoot.ini", &Reader))
    {
        LoadVignetteCriticalLoot(&Reader);
        Reader.close();
    }
    if (OpenContentDataIni("RandomMissions\\VignetteParams.ini", &Reader))
    {
        // this->Load(&Reader), the routine called directly (not through the
        // vtable).
        Load(&Reader);
        Reader.close();
    }
}

// . When loaded: bLoaded/bLinked = 0, RootNodeId = -1, the +0x10 vector and
// the +0x1c map are emptied; then the members are destroyed and the field is cleared
// (the registration base, the routine).
FRandomMissionGenerator::~FRandomMissionGenerator()
{
    if (bLoaded)
    {
        bLoaded = false;
        bLinked = false;
        RootNodeId = 0xffffffffu;
        LeafNodeIds.clear();
        Nodes.clear();
    }
}

// (vtable slot 2, no arguments): unconditionally +0x04 / +0x05 = 0, +0x08 = -1,
// the leaf id vector's end reset to its begin (inline erase), the node map emptied (size
// non-zero and begin == leftmost: the subtree erase the routine + node dtor the routine +
// head relink; otherwise the one-by-one erase loop the routine / the routine -- both are
// clear()).
void FRandomMissionGenerator::Unload()
{
    bLoaded = false;
    bLinked = false;
    RootNodeId = 0xffffffffu;
    LeafNodeIds.clear();
    Nodes.clear();
}

// (vtable slot 1, RET 4). Every header builds a stack node; the three node
// headers read their values exactly like the node's own Load (06f9ed95-06f9ee5b is an
// inline copy of the branch, weight fix included); any other header skips straight to
// the insert (06f9ee16 -> 06f9ee63), so it still adds a node keyed by NodeId (-1 unless a
// node_id was read). Insert the routine is unique: an existing key keeps its node.
void FRandomMissionGenerator::Load(INI_Reader* Reader)
{
    while (Reader->read_header())
    {
        FRmVignetteNode Node;
        if (Reader->is_header("DecisionNode") || Reader->is_header("DocumentationNode") ||
            Reader->is_header("DataNode"))
        {
            Node.Load(Reader);
        }
        Nodes.insert(std::make_pair(Node.NodeId, Node));
    }
    Link();
    bLoaded = true;
}

// . Runs once (bLinked).
//  1. For every node, every child id gets the parent's NodeId appended to its Parents --
//     through the routine (map operator[]), so a child id with no node of its own gets a
//     default node (NodeId -1) inserted here.
//  2. RootNodeId = the smallest key, then follow Parents[0] until a node has no parents.
//  3. Every child of a node that is not implemented is disabled together with its subtree
//     (the routine), in map order.
//  4. Implemented nodes without children are appended to LeafNodeIds (their NodeId).
//  5. the routine(RootNodeId).
// After step 1 every child id is a key, so the finds of steps 3/5 always hit.
void FRandomMissionGenerator::Link()
{
    if (bLinked)
        return;

    for (std::map<unsigned int, FRmVignetteNode>::iterator It = Nodes.begin();
         It != Nodes.end(); ++It)
    {
        FRmVignetteNode& Parent = It->second;
        for (unsigned int Index = 0; Index < Parent.Children.size(); ++Index)
        {
            FRmVignetteNode& Child = Nodes[Parent.Children[Index]];
            Child.Parents.insert(Child.Parents.end(), Parent.NodeId);
        }
    }

    // head->left = begin.
    RootNodeId = Nodes.begin()->first;
    for (;;)
    {
        FRmVignetteNode& Node = Nodes.find(RootNodeId)->second;
        if (Node.Parents.empty())
            break;
        RootNodeId = Node.Parents[0];
    }

    for (std::map<unsigned int, FRmVignetteNode>::iterator It = Nodes.begin();
         It != Nodes.end(); ++It)
    {
        FRmVignetteNode& Node = It->second;
        if (Node.bImplemented)
            continue;
        for (unsigned int Index = 0; Index < Node.Children.size(); ++Index)
        {
            FRmVignetteNode& Child = Nodes.find(Node.Children[Index])->second;
            if (Child.bImplemented)
            {
                Child.bImplemented = false;
                DisableChildren(&Child.Children);
            }
        }
    }

    for (std::map<unsigned int, FRmVignetteNode>::iterator It = Nodes.begin();
         It != Nodes.end(); ++It)
    {
        FRmVignetteNode& Node = It->second;
        if (Node.bImplemented && Node.Children.empty())
            LeafNodeIds.insert(LeafNodeIds.end(), Node.NodeId);
    }

    PropagateDifficultyWindows(RootNodeId);
    bLinked = true;
}

// (RET 4): every implemented child is disabled and its own children follow
// recursively. The lookup is lower_bound the routine + key check, i.e. find.
void FRandomMissionGenerator::DisableChildren(std::vector<unsigned int> const* ChildIds)
{
    for (unsigned int Index = 0; Index < ChildIds->size(); ++Index)
    {
        FRmVignetteNode& Child = Nodes.find((*ChildIds)[Index])->second;
        if (Child.bImplemented)
        {
            Child.bImplemented = false;
            DisableChildren(&Child.Children);
        }
    }
}

// (RET 4). A node whose window list is still empty gets its own difficulty
// window (06f9db05-06f9db38: the inline equal-window search of the empty list never
// matches, then the routine). Each child then receives, uniquely (the routine), a copy of
// every window of this node shifted by the child's own window (the routine), or just the
// child's window when this node has none; the recursion goes into the child's NodeId
// (map node +0x18), not the child id. Children created by Link's operator[] carry NodeId
// -1 there; baseline then searches -1 (and reads the map head node when no key -1 exists).
void FRandomMissionGenerator::PropagateDifficultyWindows(unsigned int NodeId)
{
    FRmVignetteNode& Node = Nodes.find(NodeId)->second;
    if (Node.Windows.Windows.empty())
    {
        bool bFound = false;
        for (std::list<FRmDifficultyWindow>::iterator It = Node.Windows.Windows.begin();
             It != Node.Windows.Windows.end(); ++It)
        {
            if (It->Equals(&Node.Params.Difficulty))
            {
                bFound = true;
                break;
            }
        }
        if (!bFound)
            Node.Windows.PushBack(&Node.Params.Difficulty);
    }

    for (unsigned int Index = 0; Index < Node.Children.size(); ++Index)
    {
        // lower_bound the routine + key check (find).
        FRmVignetteNode& Child = Nodes.find(Node.Children[Index])->second;

        FRmDifficultyWindowList Shifted;                                // copy
        Shifted.Windows = Node.Windows.Windows;
        if (Shifted.Windows.empty())
        {
            Shifted.AddUnique(&Child.Params.Difficulty);
        }
        else
        {
            for (std::list<FRmDifficultyWindow>::iterator It = Shifted.Windows.begin();
                 It != Shifted.Windows.end(); ++It)
            {
                It->Add(&Child.Params.Difficulty);
            }
        }
        for (std::list<FRmDifficultyWindow>::iterator It = Shifted.Windows.begin();
             It != Shifted.Windows.end(); ++It)
        {
            Child.Windows.AddUnique(&*It);
        }

        PropagateDifficultyWindows(Child.NodeId);
    }
}

// =======================================================================================
// TU63 -- vignette node pieces
// =======================================================================================

FRmDifficultyWindow::FRmDifficultyWindow()
    : Min(0.0f)
    , Max(0.0f)
{
}


FRmDifficultyWindow::FRmDifficultyWindow(float InMin, float InMax)
    : Min(InMin)
    , Max(InMax)
{
}

// (sdtor): no members to release.
FRmDifficultyWindow::~FRmDifficultyWindow()
{
}

// Min <= Difficulty (TEST AH,0x41 + JP: jump = Min > Difficulty or
// unordered -> 0) and Difficulty <= Max (same idiom).
bool FRmDifficultyWindow::IsInRange(float Difficulty) const
{
    return Min <= Difficulty && Difficulty <= Max;
}

// (RET 4): this contains Other->Min, or this contains Other->Max, or Other
// contains this->Min -- all three through vtable slot 1.
bool FRmDifficultyWindow::Overlaps(FRmDifficultyWindow const* Other) const
{
    if (IsInRange(Other->Min))
        return true;
    if (IsInRange(Other->Max))
        return true;
    if (Other->IsInRange(Min))
        return true;
    return false;
}

// (RET 4): "difficulty = min, max".
void FRmDifficultyWindow::Parse(INI_Reader* Reader)
{
    Min = Reader->get_value_float(0);
    Max = Reader->get_value_float(1);
}

// (RET 8)
void FRmDifficultyWindow::Set(float InMin, float InMax)
{
    Min = InMin;
    Max = InMax;
}

// (RET 4): Other->Min == Min and Other->Max == Max (TEST AH,0x44 + JP:
// jump = not equal or unordered -> 0).
bool FRmDifficultyWindow::Equals(FRmDifficultyWindow const* Other) const
{
    return Other->Min == Min && Other->Max == Max;
}

// (RET 4): Min += Other->Min, Max += Other->Max.
void FRmDifficultyWindow::Add(FRmDifficultyWindow const* Other)
{
    Min = Other->Min + Min;
    Max = Other->Max + Max;
}

// erases every node (element dtor through its vtable), frees the head.
FRmDifficultyWindowList::~FRmDifficultyWindowList()
{
}

// (RET 4): appends a copy unless an element with equal Min and Max exists.
void FRmDifficultyWindowList::AddUnique(FRmDifficultyWindow const* Window)
{
    for (std::list<FRmDifficultyWindow>::iterator It = Windows.begin(); It != Windows.end();
         ++It)
    {
        if (Window->Min == It->Min && Window->Max == It->Max)
            return;
    }
    Windows.push_back(*Window);
}

// (RET 4): appends a copy.
void FRmDifficultyWindowList::PushBack(FRmDifficultyWindow const* Window)
{
    Windows.push_back(*Window);
}

FRmOfferTextEntry::FRmOfferTextEntry()
    : StrId(0)
    , Form(3)
{
}

// Inline at the routine 06f99917-06f99947: bAppend 0, empty entries.
FRmOfferText::FRmOfferText()
    : bAppend(false)
{
}

// (RET 4). Value 0: "append" / "replace" (NEURONCOMPONENTS stricmp; anything else keeps
// bAppend). From value 1 on, entries are built token by token: a new entry {0, 3, []}
// starts whenever the previous one was closed. A token with atoi != 0 is the entry's
// StrId, followed by parameter names looked up in GRmTextParamNames; the first name not in
// the table (or the end of the values) closes the entry, and that token is read again as
// the first token of the next entry. A token with atoi == 0 is "singular" (Form 1) /
// "plural" (Form 2) / ignored, and leaves the entry open.
void FRmOfferText::Parse(INI_Reader* Reader)
{
    char const* Mode = Reader->get_value_string(0);
    if (_stricmp(Mode, "append") == 0)
        bAppend = true;
    else if (_stricmp(Mode, "replace") == 0)
        bAppend = false;

    unsigned int Index = 1;
    bool bNewEntry = true;
    FRmOfferTextEntry* Entry = nullptr;
    while (!Reader->is_value_empty(Index))
    {
        if (bNewEntry)
        {
            bNewEntry = false;
            Entries.insert(Entries.end(), FRmOfferTextEntry());
            Entry = &Entries.back();
        }

        char const* Token = Reader->get_value_string(Index);
        if (MsvcrtAtoi(Token) != 0)
        {
            Entry->StrId = Reader->get_value_int(Index);
            ++Index;
            while (!Reader->is_value_empty(Index))
            {
                char const* Name = Reader->get_value_string(Index);
                int Row = 0;
                for (; Row < 15; ++Row)
                {
                    if (GRmTextParamNames[Row].Name != nullptr &&
                        _stricmp(Name, GRmTextParamNames[Row].Name) == 0)
                        break;
                }
                if (Row == 15)
                    break;
                Entry->Params.insert(Entry->Params.end(), GRmTextParamNames[Row].Value);
                ++Index;
            }
            bNewEntry = true;
        }
        else
        {
            if (_stricmp(Token, "singular") == 0)
                Entry->Form = 1;
            else if (_stricmp(Token, "plural") == 0)
                Entry->Form = 2;
            ++Index;
        }
    }
}

// no entries.
bool FRmOfferText::IsEmpty() const
{
    return Entries.empty();
}

// (06f993c0-06f9955c, RET 0x10). Per entry (+0x0c..+0x10, stride 0x18), by Form
// (+0x04): 1 (06f99452) builds when Params holds 1 and bFlag1 is set, else when Params holds
// 2 and bFlag2 is set; 2 (06f9940b) builds when Params holds 1 and bFlag1 is clear, else
// when Params holds 2 and bFlag2 is clear; 3 always builds; any other Form is skipped. A
// build (06f9949b-06f99538): local FmtStr (inline zeroing), begin_mad_lib(StrId), Source
// slot 0 (GetTextParam) for each parameter in order, end_mad_lib, Out->append_fmt_str, the
// local destroyed (EH state 0). bFlag1 / bFlag2 are bOfferGroupSingular /
// bHostileGroupSingular.
void FRmOfferText::Build(bool bOfferGroupSingular, bool bHostileGroupSingular, FmtStr* Out,
                         IRmTextParamSource* Source)
{
    bool const bFlag1 = bOfferGroupSingular;
    bool const bFlag2 = bHostileGroupSingular;
    for (std::vector<FRmOfferTextEntry>::iterator Entry = Entries.begin(); Entry != Entries.end();
         ++Entry)
    {
        bool const bHas1 = std::find(Entry->Params.begin(), Entry->Params.end(), 1) !=
                           Entry->Params.end();
        bool bBuild = false;
        if (Entry->Form == 1)
        {
            if (bHas1 && bFlag1)
                bBuild = true;
            else if (std::find(Entry->Params.begin(), Entry->Params.end(), 2) !=
                     Entry->Params.end())
                bBuild = bFlag2;
        }
        else if (Entry->Form == 2)
        {
            if (bHas1 && !bFlag1)
                bBuild = true;
            else if (std::find(Entry->Params.begin(), Entry->Params.end(), 2) !=
                     Entry->Params.end())
                bBuild = !bFlag2;
        }
        else if (Entry->Form == 3)
        {
            bBuild = true;
        }
        if (!bBuild)
            continue;

        FmtStr Part;
        Part.begin_mad_lib(static_cast<unsigned int>(Entry->StrId));
        for (std::vector<int>::iterator Param = Entry->Params.begin();
             Param != Entry->Params.end(); ++Param)
            Source->GetTextParam(*Param, &Part);
        Part.end_mad_lib();
        Out->append_fmt_str(Part);
    }
}

// (RET 8). Value Index names the objective; the text is inserted uniquely
// (the routine) as an empty FRmParamText and then parsed from Index + 1 (vtable slot 1),
// so a repeated objective re-parses the existing text. An unknown name uses the stack
// slot holding this ([ESP+0x14] = the saved ECX) as the key. Always returns 0.
char FRmObjectiveText::Parse(INI_Reader* Reader, unsigned int Index)
{
    char const* Name = Reader->get_value_string(Index);
    int Objective = 0;
    if (_stricmp(Name, "guide_to_main_battle") == 0)
        Objective = 2;
    else if (_stricmp(Name, "guide_to_pre_battle") == 0)
        Objective = 1;
    else if (_stricmp(Name, "guide_to_main_from_pre") == 0)
        Objective = 3;
    else if (_stricmp(Name, "kill_big_solar") == 0)
        Objective = 4;
    else if (_stricmp(Name, "kill_other_solars") == 0)
        Objective = 5;
    else if (_stricmp(Name, "pb_kill_ships") == 0)
        Objective = 0x13;
    else if (_stricmp(Name, "pb_kill_big_ships") == 0)
        Objective = 0x14;
    else if (_stricmp(Name, "pb_dont_kill_ships") == 0)
        Objective = 0x15;
    else if (_stricmp(Name, "mb_kill_ships") == 0)
        Objective = 0x16;
    else if (_stricmp(Name, "mb_kill_big_ships") == 0)
        Objective = 0x17;
    else if (_stricmp(Name, "mb_dont_kill_ships") == 0)
        Objective = 0x18;
    else if (_stricmp(Name, "return_to_battle") == 0)
        Objective = 9;
    else if (_stricmp(Name, "destroy_loot") == 0)
        Objective = 0xa;
    else if (_stricmp(Name, "tractor_loot") == 0)
        Objective = 0xb;
    else if (_stricmp(Name, "return_loot") == 0)
        Objective = 0xc;
    else if (_stricmp(Name, "jettison_loot") == 0)
        Objective = 0xd;
    else
        Objective = static_cast<int>(reinterpret_cast<intptr_t>(this));

    std::pair<std::map<int, FRmParamText>::iterator, bool> Inserted =
        Texts.insert(std::make_pair(Objective, FRmParamText()));
    Inserted.first->second.Parse(Reader, Index + 1);
    return 0;
}

// Inline at the routine 06f98f23-06f98f41: speaker 1, length 0, empty text.
FRmCommLine::FRmCommLine()
    : Speaker(1)
    , Length(0)
{
    Text[0] = '\0';
}

// Inline at the routine 06f99b79-06f99ba4: {0, 1, 0, 0, 0, []}.
FRmCommSequence::FRmCommSequence()
    : Trigger(0)
    , Target(1)
    , Field_0x08(0.0f)
    , MinDelay(0.0f)
    , MaxDelay(0.0f)
{
}

// (RET 4): "comm_sequence = trigger, target, f, f, f, {speaker, text}...".
// Names are NEURONCOMPONENTS stricmp lookups (null rows skipped, no match keeps the default). For the
// triggers 13..16 a value 4 above 5.0 is clamped to 5.0 and value 3 to at most 5.0
// (06f98ece: TEST AH,0x41 + JNZ = value 4 <= 5.0 or unordered -> no clamp; 06f98eed:
// TEST AH,0x05 + JP = value 3 >= 5.0 or unordered -> 5.0). Line texts keep at most 0x3f
// characters.
void FRmCommSequence::Parse(INI_Reader* Reader)
{
    char const* TriggerName = Reader->get_value_string(0);
    for (int Row = 0; Row < 17; ++Row)
    {
        if (GRmCommTriggerNames[Row].Name != nullptr &&
            _stricmp(TriggerName, GRmCommTriggerNames[Row].Name) == 0)
        {
            Trigger = GRmCommTriggerNames[Row].Value;
            break;
        }
    }
    char const* TargetName = Reader->get_value_string(1);
    for (int Row = 0; Row < 12; ++Row)
    {
        if (GRmCommTargetNames[Row].Name != nullptr &&
            _stricmp(TargetName, GRmCommTargetNames[Row].Name) == 0)
        {
            Target = GRmCommTargetNames[Row].Value;
            break;
        }
    }
    Field_0x08 = Reader->get_value_float(2);
    MinDelay = Reader->get_value_float(3);
    MaxDelay = Reader->get_value_float(4);
    if (Trigger == 0xd || Trigger == 0xe || Trigger == 0xf || Trigger == 0x10)
    {
        if (MaxDelay > 5.0f)
        {
            MaxDelay = 5.0f;
            if (!(MinDelay < 5.0f))
                MinDelay = 5.0f;
        }
    }

    unsigned int Index = 5;
    while (!Reader->is_value_empty(Index))
    {
        Lines.insert(Lines.end(), FRmCommLine());
        FRmCommLine& Line = Lines.back();
        char const* SpeakerName = Reader->get_value_string(Index);
        ++Index;
        for (int Row = 0; Row < 12; ++Row)
        {
            if (GRmCommTargetNames[Row].Name != nullptr &&
                _stricmp(SpeakerName, GRmCommTargetNames[Row].Name) == 0)
            {
                Line.Speaker = GRmCommTargetNames[Row].Value;
                break;
            }
        }
        char const* LineText = Reader->get_value_string(Index);
        ++Index;
        if (LineText == nullptr)
        {
            Line.Length = 0;
            Line.Text[0] = '\0';
        }
        else
        {
            unsigned int Length = static_cast<unsigned int>(strlen(LineText));
            if (Length >= 0x40)
                Length = 0x3f;
            memcpy(Line.Text, LineText, Length);
            Line.Text[Length] = '\0';
            Line.Length = Length;
        }
    }
}


FRmVignetteParams::FRmVignetteParams()
    : Weight(0.0f)
    , AllowableZoneTypes(7)
{
}


FRmVignetteParams::~FRmVignetteParams()
{
}

// "offer_group" / "hostile_group" (06f99a17-06f99ab5 / 06f99acb-06f99b65):
// value 0 "all" (NEURONCOMPONENTS stricmp) enumerates every group once into the field and assigns
// its vector (the routine); otherwise every value's reputation group is inserted uniquely
// (the routine).
static void ParseRmReputationGroups(INI_Reader* Reader, std::vector<unsigned int>* Groups)
{
    if (_stricmp(Reader->get_value_string(0), "all") == 0)
    {
        FRmAllGroupsEnumerator* AllGroups = GRmAllGroups;
        if (!AllGroups->bEnumerated)
        {
            pub::Reputation::EnumerateGroups(*AllGroups);
            AllGroups->bEnumerated = true;
        }
        *Groups = AllGroups->Groups;
        return;
    }
    // [ESP+0x6c] (the dead Reader argument slot): one out-slot for every value. Its initial
    // value is never read: Server pub::Reputation::GetReputationGroup writes it on both paths
    // (06d5f714 the id, 06d5f722 -1).
    unsigned int GroupId = 0;
    for (unsigned int Index = 0; !Reader->is_value_empty(Index); ++Index)
    {
        pub::Reputation::GetReputationGroup(GroupId, Reader->get_value_string(Index));
        RmInsertUnique(Groups, GroupId);
    }
}

// (vtable slot 1, RET 4): returns 1 when the key was one of this block's,
// 0 otherwise (06f99caa).
char FRmVignetteParams::ParseKey(INI_Reader* Reader)
{
    if (Reader->is_value("weight"))
    {
        Weight = Reader->get_value_float(0);
        return 1;
    }
    if (Reader->is_value("offer_text"))
    {
        // appends a {bAppend 0, []} element, then its slot 0 parses.
        OfferTexts.insert(OfferTexts.end(), FRmOfferText());
        OfferTexts.back().Parse(Reader);
        return 1;
    }
    if (Reader->is_value("objective_text"))
    {
        ObjectiveText.Parse(Reader, 0);
        return 1;
    }
    if (Reader->is_value("failure_text"))
    {
        FailureText.Parse(Reader, 0);
        return 1;
    }
    if (Reader->is_value("reward_text"))
    {
        RewardText.Parse(Reader, 0);
        return 1;
    }
    if (Reader->is_value("difficulty"))
    {
        Difficulty.Parse(Reader);
        return 1;
    }
    if (Reader->is_value("offer_group"))
    {
        ParseRmReputationGroups(Reader, &OfferGroups);
        return 1;
    }
    if (Reader->is_value("hostile_group"))
    {
        ParseRmReputationGroups(Reader, &HostileGroups);
        return 1;
    }
    if (Reader->is_value("comm_sequence"))
    {
        // appends a default element, then the routine parses it.
        CommSequences.insert(CommSequences.end(), FRmCommSequence());
        CommSequences.back().Parse(Reader);
        return 1;
    }
    if (Reader->is_value("allowable_zone_types"))
    {
        AllowableZoneTypes = 0;
        for (unsigned int Index = 0; !Reader->is_value_empty(Index); ++Index)
        {
            if (_stricmp(Reader->get_value_string(Index), "open") == 0)
                AllowableZoneTypes = AllowableZoneTypes | 0x1;
            else if (_stricmp(Reader->get_value_string(Index), "field") == 0)
                AllowableZoneTypes = AllowableZoneTypes | 0x2;
            else if (_stricmp(Reader->get_value_string(Index), "exclusion") == 0)
                AllowableZoneTypes = AllowableZoneTypes | 0x4;
        }
        return 1;
    }
    return 0;
}

// Inline at the routine 06f9ed00-06f9ed73 and the routine: ID_String::clear on +0x04,
// NodeId -1, empty vectors, implemented 1, window list the routine, params the routine,
// +0x110 = -1.0f (0xbf800000).
FRmVignetteNode::FRmVignetteNode()
    : NodeId(0xffffffffu)
    , bImplemented(true)
    , FirstChildChance(-1.0f)
{
    Nickname.clear();
}

// (vtable slot 0, RET 4): every value goes to the common keys first, then to
// the parameter block (vtable slot 1); a weight of exactly 0 becomes 1.0 (TEST AH,0x44 +
// JP: jump = not equal or unordered).
void FRmVignetteNode::Load(INI_Reader* Reader)
{
    while (Reader->read_value())
    {
        if (!ParseCommonKey(Reader))
            Params.ParseKey(Reader);
    }
    if (Params.Weight == 0.0f)
        Params.Weight = 1.0f;
}

// (RET 4): node_id / child_node (appended) / implemented / nickname or
// documentation (CreateID of value 0). 1 when the key was handled.
bool FRmVignetteNode::ParseCommonKey(INI_Reader* Reader)
{
    if (Reader->is_value("node_id"))
    {
        NodeId = static_cast<unsigned int>(Reader->get_value_int(0));
        return true;
    }
    if (Reader->is_value("child_node"))
    {
        unsigned int ChildId = static_cast<unsigned int>(Reader->get_value_int(0));
        Children.insert(Children.end(), ChildId);
        return true;
    }
    if (Reader->is_value("implemented"))
    {
        bImplemented = Reader->get_value_bool(0);
        return true;
    }
    if (Reader->is_value("nickname") || Reader->is_value("documentation"))
    {
        Nickname.Id = CreateID(Reader->get_value_string());
        return true;
    }
    return false;
}

static FRandomMissionGenerator GRandomMissionGeneratorInstance;

// =======================================================================================
// TU63 -- offer request, tree path, path filters
// =======================================================================================

// .
FRmOfferQuery::FRmOfferQuery()
    : OfferGroup(-1)
    , EnemyGroup(0xffffffffu)
    , Reward(0)
    , RepReward(0.0f)
    , RepHit(0.0f)
    , OfferBase(0)
    , Difficulty(0.0f)
    , ObjectiveZone(0)
    , PlayerId(0)
    , DestSys(0)
{
    ObjectivePos.x = 0.0f;
    ObjectivePos.y = 0.0f;
    ObjectivePos.z = 0.0f;
}

// (RET 4): one character per decision into a 0x400-byte stack buffer ('F' for
// 0, 'T' otherwise: SETZ / DEC / AND 0xe / ADD 0x46), no length check, then
// fprintf(File, "tree_path = %s\n").
void FRmTreePath::Save(FILE* File)
{
    char Text[0x400];
    unsigned int Length = 0;
    for (std::vector<char>::const_iterator It = Bits.begin(); It != Bits.end(); ++It)
    {
        Text[Length] = (*It == 0) ? 'F' : 'T';
        ++Length;
    }
    Text[Length] = '\0';
    fprintf(File, "tree_path = %s\n", Text);
}

// (RET 4): only for "tree_path". Clears the bits, copies value 0 (at most
// 0x3ff characters; a null string reads as empty) and appends one bit per character,
// 1 for 'T' and 0 for anything else.
void FRmTreePath::Load(INI_Reader* Reader)
{
    if (!Reader->is_value("tree_path"))
        return;
    Bits.clear();

    char Text[0x400];
    unsigned int Length = 0;
    if (Reader->is_value("tree_path"))
    {
        char const* Value = Reader->get_value_string(0);
        if (Value != nullptr)
        {
            Length = static_cast<unsigned int>(strlen(Value));
            if (Length >= 0x400)
                Length = 0x3ff;
            memcpy(Text, Value, Length);
            Text[Length] = '\0';
        }
    }
    for (unsigned int Index = 0; Index < Length; ++Index)
        Bits.insert(Bits.end(), static_cast<char>(Text[Index] == 'T' ? 1 : 0));
}

// Inline at 06f9e9e0: the five mission flags cleared.
FRmPathFilter::FRmPathFilter()
    : bRandomMission(false)
    , bDestroySolars(false)
    , bAssassinate(false)
    , bDestroyShips(false)
    , bLootItem(false)
{
}

// .
int FRmPathFilter::Slot00() const
{
    return 0;
}

// any of +0x05..+0x08 (random_mission at +0x04 does not count).
bool FRmPathFilter::HasMissionType() const
{
    return bDestroySolars || bAssassinate || bDestroyShips || bLootItem;
}

// (RET 8): only a taken branch (Bit != 0) is looked at. The node nickname is
// compared with CreateID of each mission name in turn; a match sets that flag and returns
// 1. No match: ID_String::IsEmpty on the nickname (result unused), 0.
bool FRmPathFilter::ApplyNode(FRmVignetteNode const* Node, char Bit)
{
    if (Bit == 0)
        return false;
    ID_String const& Nickname = Node->Nickname;
    if (Nickname.Id == CreateID("random_mission"))
    {
        bRandomMission = true;
        return true;
    }
    if (Nickname.Id == CreateID("destroy_solars_mission"))
    {
        bDestroySolars = true;
        return true;
    }
    if (Nickname.Id == CreateID("assassinate_mission"))
    {
        bAssassinate = true;
        return true;
    }
    if (Nickname.Id == CreateID("destroy_ships_mission"))
    {
        bDestroyShips = true;
        return true;
    }
    if (Nickname.Id == CreateID("loot_item_mission"))
    {
        bLootItem = true;
        return true;
    }
    Nickname.IsEmpty();
    return false;
}

// the first set flag of destroy_solars / assassinate / destroy_ships /
// loot_item picks the new filter. Destroy-solars (0x1c) and assassinate (0x28, ctor
// ) copy +0x04..+0x08 and clear their own flags; destroy-ships and loot-item
// (0x0c) start with every flag cleared. None set: null.
FRmPathFilter* FRmPathFilter::Specialize() const
{
    if (bDestroySolars)
    {
        FRmDestroySolarsFilter* Filter = new FRmDestroySolarsFilter();
        Filter->bRandomMission = bRandomMission;
        Filter->bDestroySolars = bDestroySolars;
        Filter->bAssassinate = bAssassinate;
        Filter->bDestroyShips = bDestroyShips;
        Filter->bLootItem = bLootItem;
        return Filter;
    }
    if (bAssassinate)
    {
        FRmAssassinateFilter* Filter = new FRmAssassinateFilter();
        Filter->bRandomMission = bRandomMission;
        Filter->bDestroySolars = bDestroySolars;
        Filter->bAssassinate = bAssassinate;
        Filter->bDestroyShips = bDestroyShips;
        Filter->bLootItem = bLootItem;
        return Filter;
    }
    if (bDestroyShips)
        return new FRmDestroyShipsFilter();
    if (bLootItem)
        return new FRmLootItemFilter();
    return nullptr;
}

// switch on Param - 1 through the byte table at 06f9a458 -- parameters 1..3
// (OFFER_GROUP, HOSTILE_GROUP, BIG_SOLAR) and 9..13 (FACTION_HIT .. TARGET_ZONE) are
// supported; anything else (unsigned index above 0xc included) is not.
bool FRmPathFilter::SupportsTextParam(int Param) const
{
    unsigned int const Index = static_cast<unsigned int>(Param) - 1u;
    if (Index > 0xcu)
        return false;
    return Index <= 2u || Index >= 8u;
}

// Inline at 06f9a2aa / 06f9a6aa: every destroy-solars flag cleared.
FRmDestroySolarsFilter::FRmDestroySolarsFilter()
    : bBigSolar(false)
    , bPkBigSolar(false)
    , bDefensiveSolars(false)
    , bPkDefensiveSolars(false)
    , bCavalry(false)
    , bHostileShips(false)
    , bPkHostileShips(false)
    , bHostileShipsAtInstallation(false)
    , bHostileShipsAfter30S(false)
    , bHostileWaves(false)
    , bFriendlyShips(false)
    , bFriendlyShipsAtInstallation(false)
    , bFriendlyShipsAfter30S(false)
    , bFriendlyWaves(false)
{
}

// MOV AL,[ECX+0x10].
bool FRmDestroySolarsFilter::HasMissionType() const
{
    return bCavalry;
}

// (RET 8): Bit 0 -> 0. Otherwise the nickname is compared with CreateID of
// the fourteen flag names in field order (+0x0c..+0x19); a match sets that flag and
// returns 1. "branch" returns 1 without a flag. No match: ID_String::IsEmpty (result
// unused), 0.
bool FRmDestroySolarsFilter::ApplyNode(FRmVignetteNode const* Node, char Bit)
{
    static char const* const Names[14] = {
        "big_solar", "pk_big_solar", "defensive_solars", "pk_defensive_solars", "cavalry",
        "hostile_ships", "pk_hostile_ships", "hostile_ships_at_installation",
        "hostile_ships_after_30_s", "hostile_waves", "friendly_ships",
        "friendly_ships_at_installation", "friendly_ships_after_30_s", "friendly_waves",
    };
    bool* const Flags[14] = {
        &bBigSolar, &bPkBigSolar, &bDefensiveSolars, &bPkDefensiveSolars, &bCavalry,
        &bHostileShips, &bPkHostileShips, &bHostileShipsAtInstallation,
        &bHostileShipsAfter30S, &bHostileWaves, &bFriendlyShips,
        &bFriendlyShipsAtInstallation, &bFriendlyShipsAfter30S, &bFriendlyWaves,
    };

    if (Bit == 0)
        return false;
    ID_String const& Nickname = Node->Nickname;
    for (int Index = 0; Index < 14; ++Index)
    {
        if (Nickname.Id == CreateID(Names[Index]))
        {
            *Flags[Index] = true;
            return true;
        }
    }
    if (Nickname.Id == CreateID("branch"))
        return true;
    Nickname.IsEmpty();
    return false;
}

// a "cavalry" node swaps in the cavalry filter with every flag cleared
// (base flags included); otherwise null.
FRmPathFilter* FRmDestroySolarsFilter::Specialize() const
{
    if (!bCavalry)
        return nullptr;
    return new FRmCavalryFilter();
}

// switch on Param - 1 (byte table 06f9ac50, 0x18 entries): 1 -> false,
// 4 -> +0x0d, 5 -> +0x0f, 22 -> +0x12, 24 -> !+0x12; every other parameter falls into the
// base table (second switch at 06f9ac1c, byte table 06f9ac70).
bool FRmDestroySolarsFilter::SupportsTextParam(int Param) const
{
    switch (Param)
    {
    case 1:
        return false;
    case 4:
        return bPkBigSolar;
    case 5:
        return bPkDefensiveSolars;
    case 22:
        return bPkHostileShips;
    case 24:
        return !bPkHostileShips;
    default:
        return FRmPathFilter::SupportsTextParam(Param);
    }
}

// Slot 6 of 06fbd034 / 06fbd164, the branch (ICF: MOV EAX,1; RET).
int FRmDestroySolarsFilter::Slot06() const
{
    return 1;
}

// .
bool FRmCavalryFilter::HasMissionType() const
{
    return false;
}

// (RET 8).
bool FRmCavalryFilter::ApplyNode(FRmVignetteNode const* /*Node*/, char /*Bit*/)
{
    return false;
}

// .
bool FRmDestroyShipsFilter::HasMissionType() const
{
    return false;
}

// (RET 8).
bool FRmDestroyShipsFilter::ApplyNode(FRmVignetteNode const* /*Node*/, char /*Bit*/)
{
    return false;
}

// .
bool FRmLootItemFilter::HasMissionType() const
{
    return false;
}

// (RET 8).
bool FRmLootItemFilter::ApplyNode(FRmVignetteNode const* /*Node*/, char /*Bit*/)
{
    return false;
}

// Node names of the branch in flag order (+0x0c..+0x26).
char const* const GRmAssassinateNodeNames[27] = {
    "assassinate_ship", "assassinate_solar", "target_drops_critical_loot", "bring_back_loot",
    "tractor_in_loot", "destroy_loot", "target_spawns_with_wave", "pre_battle",
    "target_spawns_at_pre_battle", "pre_battle_non_target_wave", "pre_battle_wave_runs",
    "pk_pre_battle_wave", "target_runs_to_main_battle", "main_battle",
    "defensive_solars_at_main_battle", "target_spawns_at_main_battle",
    "main_battle_non_target_wave", "wave_just_after_main_battle_starts",
    "continuous_reenforcements", "pk_all_ships", "pre_battle_wave_1", "pre_battle_wave_2",
    "main_battle_wave_1", "main_battle_wave_2", "friendly_ships_at_main_battle",
    "friendly_ships_after_30_s", "friendly_ships_come_in_to_whoop_up",
};

// +0x0c..+0x26 cleared.
FRmAssassinateFilter::FRmAssassinateFilter()
{
    for (int Index = 0; Index < 27; ++Index)
        Flags[Index] = false;
}

// MOV AL,[ECX+0xc]; NEG; SBB EAX,EAX; ADD 3 -> 2 when assassinate_ship,
// else 3.
int FRmAssassinateFilter::Slot00() const
{
    return Flags[0] ? 2 : 3;
}

// .
bool FRmAssassinateFilter::HasMissionType() const
{
    return false;
}

// (RET 8): same shape as the branch over GRmAssassinateNodeNames, then
// "branch" (1, no flag), then ID_String::IsEmpty (result unused) and 0.
bool FRmAssassinateFilter::ApplyNode(FRmVignetteNode const* Node, char Bit)
{
    if (Bit == 0)
        return false;
    ID_String const& Nickname = Node->Nickname;
    for (int Index = 0; Index < 27; ++Index)
    {
        if (Nickname.Id == CreateID(GRmAssassinateNodeNames[Index]))
        {
            Flags[Index] = true;
            return true;
        }
    }
    if (Nickname.Id == CreateID("branch"))
        return true;
    Nickname.IsEmpty();
    return false;
}

// (slot 3): null.
FRmPathFilter* FRmAssassinateFilter::Specialize() const
{
    return nullptr;
}

// switch on Param - 1 (byte table 06f9bac4, 0x18 entries): 1, 3, 19 ->
// pre_battle (+0x13); 20 -> target_spawns_at_pre_battle (+0x14); 4, 5, 21, 24 -> false;
// 22 -> target_spawns_at_main_battle && main_battle_non_target_wave (+0x1b, +0x1c);
// 23 -> +0x1b; the rest falls into the base table (06f9bae4).
bool FRmAssassinateFilter::SupportsTextParam(int Param) const
{
    switch (Param)
    {
    case 1:
    case 3:
    case 19:
        return Flags[7];
    case 20:
        return Flags[8];
    case 4:
    case 5:
    case 21:
    case 24:
        return false;
    case 22:
        return Flags[15] && Flags[16];
    case 23:
        return Flags[15];
    default:
        return FRmPathFilter::SupportsTextParam(Param);
    }
}

// =======================================================================================
// TU63 -- offer node and tree path choice
// =======================================================================================

namespace
{
    // {path, weight} entry of the tree path pick (8 bytes, vector insert the routine; the
    // local is a plain vector, unwind the routine at Unwind@06fb19a0).
    struct FRmWeightedPath
    {
        FRmTreePath* Path;
        float Weight;
    };
}

// (RET 4). Every leaf whose offer_group list holds Query->OfferGroup and
// whose hostile_group list holds Query->EnemyGroup (the routine) and one of whose
// difficulty windows contains Query->Difficulty (vtable slot 1) becomes a {node id,
// weight} candidate of a Common weighted_vector<unsigned int> local (unwind
// Unwind@06fb1950 = the ??1?$weighted_vector@I@@ import; insert the routine). The leaf
// lookup is the routine without an end check, as baseline. An empty candidate vector
// (begin null or size 0, 06f9cc4e / 06f9cc5f) gives -1. The weighted pick 06f9cc61-06f9ccd2
// is weighted_vector::Pick inlined: Target = rand() * (1/32768) * sum, the first candidate
// whose running sum is >= Target wins (TEST AH,0x41 + JNP: Target <= sum); a zero sum
// (TEST AH,0x44 + JNP) and a run past the last candidate both give end(), and baseline then
// returns the dword just after the last candidate (06f9ccd2, MOV ESI,[ECX]).
unsigned int FRandomMissionGenerator::PickOfferNode(FRmOfferQuery const* Query)
{
    weighted_vector<unsigned int> Candidates;
    unsigned int const OfferGroup = static_cast<unsigned int>(Query->OfferGroup);
    for (std::vector<unsigned int>::const_iterator LeafIt = LeafNodeIds.begin();
         LeafIt != LeafNodeIds.end(); ++LeafIt)
    {
        FRmVignetteNode const& Node = Nodes.find(*LeafIt)->second;
        std::vector<unsigned int> const& OfferGroups = Node.Params.OfferGroups;
        if (std::find(OfferGroups.begin(), OfferGroups.end(), OfferGroup) == OfferGroups.end())
            continue;
        std::vector<unsigned int> const& HostileGroups = Node.Params.HostileGroups;
        if (std::find(HostileGroups.begin(), HostileGroups.end(), Query->EnemyGroup) ==
            HostileGroups.end())
            continue;
        float const Difficulty = Query->Difficulty;
        for (std::list<FRmDifficultyWindow>::const_iterator Window = Node.Windows.Windows.begin();
             Window != Node.Windows.Windows.end(); ++Window)
        {
            if (Window->IsInRange(Difficulty))
            {
                Candidates.Items.insert(Candidates.Items.end(), 1,
                                        std::pair<unsigned int, float>(Node.NodeId,
                                                                       Node.Params.Weight));
                break;
            }
        }
    }

    if (Candidates.Items.empty())
        return 0xffffffffu;

    std::pair<unsigned int, float> const* Picked = Candidates.Pick();
    if (Picked != Candidates.Items.data() + Candidates.Items.size())
        return Picked->first;
    // TODO: baseline returns the dword after the last candidate (undefined); a zero sum is
    // unreachable while every node weight is forced non-zero (the routine), a run past the
    // end only through float rounding of Target against the running sum.
    return 0xffffffffu;
}

// (RET 0x18; Window by value). The decision bits of the current branch live in
// the function-static list the field (guard the field bit 0, atexit the routine).
// A node without parents (the routine == 0) appends a new path (vtable 06fb7750) holding a
// copy of that list. Otherwise, for every parent: bit = (parent's first child == NodeId);
// the parent is taken when one of its windows overlaps Window (vtable slot 2) and its
// offer_group list is empty or holds OfferGroup; the bit is pushed in front, the window is
// shifted by the parent's difficulty (min - parent max, max - parent min) and the parent
// recursed into; the bit is popped afterwards. Node lookups have no end check, as baseline.
void FRandomMissionGenerator::CollectTreePaths(std::vector<FRmTreePath>* OutPaths,
                                               unsigned int NodeId, FRmDifficultyWindow Window,
                                               int OfferGroup)
{
    static std::list<char> Choices;

    FRmVignetteNode const& Node = Nodes.find(NodeId)->second;
    if (Node.Parents.empty())
    {
        OutPaths->insert(OutPaths->end(), FRmTreePath());
        std::vector<char>& Bits = OutPaths->back().Bits;
        for (std::list<char>::const_iterator It = Choices.begin(); It != Choices.end(); ++It)
            Bits.insert(Bits.end(), *It);
        return;
    }

    for (std::vector<unsigned int>::const_iterator ParentIt = Node.Parents.begin();
         ParentIt != Node.Parents.end(); ++ParentIt)
    {
        FRmVignetteNode const& Parent = Nodes.find(*ParentIt)->second;
        char const Bit = (Parent.Children.front() == NodeId) ? 1 : 0;

        std::list<FRmDifficultyWindow>::const_iterator ParentWindow =
            Parent.Windows.Windows.begin();
        while (ParentWindow != Parent.Windows.Windows.end() && !ParentWindow->Overlaps(&Window))
            ++ParentWindow;
        if (ParentWindow == Parent.Windows.Windows.end())
            continue;

        std::vector<unsigned int> const& OfferGroups = Parent.Params.OfferGroups;
        if (!OfferGroups.empty() &&
            std::find(OfferGroups.begin(), OfferGroups.end(),
                      static_cast<unsigned int>(OfferGroup)) == OfferGroups.end())
            continue;

        Choices.insert(Choices.begin(), Bit);
        FRmDifficultyWindow const Shifted(Window.Min - Parent.Params.Difficulty.Max,
                                          Window.Max - Parent.Params.Difficulty.Min);
        CollectTreePaths(OutPaths, Parent.NodeId, Shifted, OfferGroup);
        Choices.erase(Choices.begin());
    }
}

// (RET 4). Each path starts at weight 1.0 and walks from RootNodeId: bit 1
// multiplies by the node's first-child chance and goes to Children[0], any other bit
// multiplies by (1 - chance) and goes to Children[1]. A chance of exactly -1.0
// (CMP 0xbf800000) means 1 / child count (FILD qword, FDIVR 1.0). Products are rounded to
// float (FSTP) at every step. Weighted pick as in the routine; returns the path.
FRmTreePath* FRandomMissionGenerator::PickTreePath(std::vector<FRmTreePath>* Paths)
{
    std::vector<FRmWeightedPath> Weighted;
    for (std::vector<FRmTreePath>::iterator PathIt = Paths->begin(); PathIt != Paths->end();
         ++PathIt)
    {
        FRmWeightedPath Entry;
        Entry.Path = &*PathIt;
        Entry.Weight = 1.0f;
        Weighted.insert(Weighted.end(), Entry);
        float& Weight = Weighted.back().Weight;

        unsigned int CurrentId = RootNodeId;
        for (std::vector<char>::const_iterator BitIt = PathIt->Bits.begin();
             BitIt != PathIt->Bits.end(); ++BitIt)
        {
            FRmVignetteNode const& Node = Nodes.find(CurrentId)->second;
            bool const bUniform = (Node.FirstChildChance == -1.0f);
            double const Chance =
                bUniform ? 1.0 / static_cast<double>(Node.Children.size())
                         : static_cast<double>(Node.FirstChildChance);
            if (*BitIt == 1)
            {
                Weight = static_cast<float>(Chance * Weight);
                CurrentId = Node.Children[0];
            }
            else
            {
                Weight = static_cast<float>((1.0 - Chance) * Weight);
                CurrentId = Node.Children[1];
            }
        }
    }

    double Sum = 0.0;
    for (std::vector<FRmWeightedPath>::const_iterator It = Weighted.begin();
         It != Weighted.end(); ++It)
        Sum += It->Weight;
    float const Total = static_cast<float>(Sum);
    if (Sum == 0.0)
    {
        // TODO: baseline returns the dword after the last entry (undefined), which its caller
        // then dereferences.
        return nullptr;
    }

    float const Target = static_cast<float>(
        static_cast<double>(rand()) * static_cast<double>(3.0517578e-05f) * Total);
    double Accum = 0.0;
    for (std::vector<FRmWeightedPath>::const_iterator It = Weighted.begin();
         It != Weighted.end(); ++It)
    {
        Accum += It->Weight;
        if (static_cast<double>(Target) <= Accum)
            return It->Path;
    }
    // TODO: baseline returns the dword after the last entry (undefined).
    return nullptr;
}

// (RET 0x10). Collects every path from OfferNodeId up to the root through
// windows overlapping [Difficulty, Difficulty] (the routine), then copies the weighted
// pick into OutPath (vector assignment, skipped when source and target are the same).
// Returns 1 as soon as at least one path exists.
bool FRandomMissionGenerator::ChooseTreePath(FRmTreePath* OutPath, unsigned int OfferNodeId,
                                             float Difficulty, int OfferGroup)
{
    bool bChosen = false;
    std::vector<FRmTreePath> Paths;
    CollectTreePaths(&Paths, OfferNodeId, FRmDifficultyWindow(Difficulty, Difficulty),
                     OfferGroup);
    if (!Paths.empty())
    {
        bChosen = true;
        FRmTreePath* Picked = PickTreePath(&Paths);
        if (&OutPath->Bits != &Picked->Bits)
            OutPath->Bits = Picked->Bits;
    }
    return bChosen;
}

// =======================================================================================
// TU63 -- voice properties
// =======================================================================================

std::vector<FVoiceProperties> GVoiceProperties;

// [mVoiceProp] sections; called by LoadContentGameTables (the routine) for
// audio\voices_space_male.ini, voices_space_female.ini, voices_recognizable.ini and
// missions\voice_properties.ini.
void __cdecl LoadVoiceProperties(INI_Reader* Reader)
{
    // [ESP+0x1c] -- GetVoiceID out-slot, one frame slot for all sections.
    unsigned int VoiceId = 0;

    while (Reader->read_header())
    {
        if (!Reader->is_header("mVoiceProp"))
            continue;

        // EBP: the record being filled -- an existing element of the field, or a heap
        // record (operator new 0x1c) that is copied in at the end of the section and freed.
        FVoiceProperties* Record = nullptr;
        std::unique_ptr<FVoiceProperties> Fresh;
        bool bExisting = false;                     // [ESP+0x12]

        while (Reader->read_value())
        {
            if (Reader->is_value("voice"))
            {
                pub::GetVoiceID(VoiceId, Reader->get_value_string());
                // linear search by VoiceId (+0x00).
                FVoiceProperties* Found = nullptr;
                for (std::vector<FVoiceProperties>::iterator It = GVoiceProperties.begin();
                     It != GVoiceProperties.end(); ++It)
                {
                    if (It->VoiceId == VoiceId)
                    {
                        Found = &*It;
                        break;
                    }
                }
                if (Found != nullptr)
                {
                    bExisting = true;
                    Record = Found;
                }
                else
                {
                    // new record, +0/+4 zero, empty map.
                    Fresh.reset(new FVoiceProperties());
                    Fresh->VoiceId = 0;
                    Fresh->Flags = 0;
                    Record = Fresh.get();
                }
                Record->VoiceId = VoiceId;
            }
            else if (Reader->is_value("gender"))
            {
                // NEURONCOMPONENTS stricmp for "male", then a case-sensitive REPE CMPSB for "female".
                char const* Value = Reader->get_value_string();
                if (_stricmp(Value, "male") == 0)
                    Record->Flags |= 0x1;
                else if (strcmp(Value, "female") == 0)
                    Record->Flags |= 0x2;
            }
            else if (Reader->is_value("supports_roles"))
            {
                // every value compared case-sensitively (REPE CMPSB over
                // the literal including its terminator).
                for (unsigned int Index = 0; !Reader->is_value_empty(Index); ++Index)
                {
                    char const* Role = Reader->get_value_string(Index);
                    if (strcmp(Role, "combat_lev_1") == 0)
                        Record->Flags |= 0x4000;
                    else if (strcmp(Role, "combat_lev_2") == 0)
                        Record->Flags |= 0x4;
                    else if (strcmp(Role, "combat_lev_3") == 0)
                        Record->Flags |= 0xc004;
                    else if (strcmp(Role, "scan") == 0)
                        Record->Flags |= 0x8;
                    else if (strcmp(Role, "hail_hailee") == 0)
                        Record->Flags |= 0x10;
                    else if (strcmp(Role, "hail_dock") == 0)
                        Record->Flags |= 0x20;
                    else if (strcmp(Role, "dockrequest_dock") == 0)
                        Record->Flags |= 0x40;
                    else if (strcmp(Role, "dockrequest_docker") == 0)
                        Record->Flags |= 0x80;
                    else if (strcmp(Role, "behavior") == 0)
                        Record->Flags |= 0x100;
                    else if (strcmp(Role, "misc_chatter") == 0)
                        Record->Flags |= 0x200;
                    else if (strcmp(Role, "fidget") == 0)
                        Record->Flags |= 0x400;
                    else if (strcmp(Role, "combat_dock") == 0)
                        Record->Flags |= 0x800;
                    else if (strcmp(Role, "random_mission_ship") == 0)
                        Record->Flags |= 0x1000;
                    else if (strcmp(Role, "mission_commission") == 0)
                        Record->Flags |= 0x2000;
                }
            }
            else if (Reader->is_value("permutation_count"))
            {
                // unique insert into the record's map (existing key kept).
                unsigned int Key = pub::GetNicknameId(Reader->get_value_string(0));
                int Count = Reader->get_value_int(1);
                Record->PermutationCounts.insert(std::make_pair(Key, Count));
            }
        }

        // a section that did not name an existing voice appends a copy of
        // its record (the routine(end, 1, EBP), EBP dereferenced unconditionally), then the
        // heap record is destroyed.
        if (!bExisting)
            GVoiceProperties.insert(GVoiceProperties.end(), *Record);
    }
}

// (thiscall on the record) -- lower_bound on the map at +8 (the routine), exact
// key -> mapped count, else 0. Callers 06eeaf71 / 06eeb068 (GCS).
int FVoiceProperties::GetPermutationCount(unsigned int const* NicknameId) const
{
    std::map<unsigned int, int>::const_iterator It = PermutationCounts.find(*NicknameId);
    if (It == PermutationCounts.end())
        return 0;
    return It->second;
}

// (thiscall on the record) -- (Flags & Mask) == Mask (AND/SUB/NEG/SBB/INC).
// 21 callers (GCS TU25, TU38, TU48).
bool FVoiceProperties::SupportsRoles(unsigned int Mask) const
{
    return (Flags & Mask) == Mask;
}

// linear search of the field by voice id; null on a miss. 21 callers
// (GCS TU25, TU38, TU48).
FVoiceProperties* __cdecl FindVoiceProperties(unsigned int const* VoiceId)
{
    for (std::vector<FVoiceProperties>::iterator It = GVoiceProperties.begin();
         It != GVoiceProperties.end(); ++It)
    {
        if (It->VoiceId == *VoiceId)
            return &*It;
    }
    return nullptr;
}

// =======================================================================================
// TU63 -- building the TU61 offer (RandomMissionOffer.h)
// =======================================================================================

namespace
{
    // .rdata the field (single player) and the field (multiplayer), indexed by the
    // formation size: nine floats each, stored back to back (06f98a48 / 06f98a51).
    float const GRmFormationSizeWeights[18] = {
        0.0f, 0.0f, 3.0f, 9.0f, 6.0f, 2.0f, 1.0f, 1.0f, 1.0f,
        0.0f, 0.0f, 2.0f, 3.0f, 9.0f, 6.0f, 4.0f, 2.0f, 1.0f,
    };

    // The 0x10-byte candidate of the routine (vector insert the routine).
    struct FRmFormationSizeChoice
    {
        FRmShipSize Size;
        float Weight;
    };

    // the counter of the "Vignette_solar_%X" nicknames (the routine,
    // ).
    unsigned int GRmVignetteSolarCounter = 0;
    // the .data buffer both solar spawners format the nickname into (0x20 bytes
    // up to the field).
    char GRmVignetteSolarName[0x20];

    // .rdata 06fbcbf8: the pilots of vignette solars, 0x18 bytes apart (06f9b141-06f9b14c).
    char const* const GRmSolarPilotNames[4] = {
        "pilot_rm_solar_easiest",
        "pilot_rm_solar_easy",
        "pilot_rm_solar_hard",
        "pilot_rm_solar_hardest",
    };

    // (x87): Difficulty -> offer +0x2c. At or above the field (FCOM +
    // TEST AH,0x5 + JP: not less, NaN included) ln(D) * + the field (FLDLN2 /
    // FYL2X); below it D * + (D^3 * - D^2 * ) -
    // . A negative result becomes 0 (TEST AH,0x5 + JP keeps >= 0); floor(x +
    // ) is truncated by ftol, and a rank below 1 becomes 1 (TEST AH,0x41 + JNZ
    // keeps 1 <= rank).
    float RemapRmOfferDifficulty(float Difficulty)
    {
        double const LogThreshold = 0.900376;       // 3feccfe1'54434e33
        double const LogScale = 6.3693;             // 40197a29'c779a6b5
        double const LogOffset = 13.668;            // 402b5604'189374bc
        double const CubeScale = 15.193;            // 402e62d0'e5604189
        double const SquareScale = 28.236;          // 403c3c6a'7ef9db23
        double const LinearScale = 28.682;          // 403cae97'8d4fdf3b
        double const CubicOffset = 1.0113;          // 3ff02e48'e8a71de7

        double const D = static_cast<double>(Difficulty);
        double Mapped = 0.0;
        if (!(D < LogThreshold))
            Mapped = std::log(D) * LogScale + LogOffset;
        else
            Mapped = D * LinearScale + (D * D * D * CubeScale - D * D * SquareScale) - CubicOffset;
        if (Mapped < 0.0)
            Mapped = 0.0;
        int const Rounded = static_cast<int>(std::floor(Mapped + 0.5));
        float Rank = static_cast<float>(Rounded);
        if (1.0f > Rank)
            Rank = 1.0f;
        return Rank;
    }
}

// (06f98780-06f98bc5, cdecl). The candidate classes are the one id
// CreateID("class_fighter") (a fresh vector, the routine find + the routine insert);
// lists the faction's archs that fit it. Every arch level (+0x24) that is not 0
// is flagged (a 0x1a-byte array, no bound check, as baseline). Per flagged level 1..25 the
// NpcRankToDiff vector (map operator[], the routine) is walked for the first difficulty
// above Difficulty (TEST AH,0x41 + JZ); at index 0 or when none is above, the level gets no
// count; otherwise the nearer neighbour wins (B = d[i] - D against A = D - d[i-1]: TEST
// AH,0x5 + JNP keeps i when B < A, else i - 1) and the count is that index + 1. Per level
// 1..25 with a count >= 1: {d[count - 1], level, count, weight[count]} (weights by
// pub::SinglePlayer). Weighted pick as weighted_vector::Pick (rand() * 2^-15 * sum, first
// running sum >= target); a zero sum, an empty vector or no pick set only Out->Count = 0.
// Every level 1..25 gets count -1 before its flag test (06f98899), so a level without a
// count is skipped by the second loop (JL 1). The class-id local is a bare vector<uint> in
// baseline (ctor the routine, dtor the routine); the routine reads only its +0x00 vector,
// which is FEncounterShip::ShipClasses here. The weight index is the count (at most 8 with
// DATA\RANDOMMISSIONS\npcranktodiff.ini's eight values per rank; baseline does not bound it).
void __cdecl PickRmShipFormationSize(FRmShipSize* Out, float Difficulty, unsigned int Faction)
{
    FEncounterShip Line;
    Line.ShipClasses.push_back(CreateID("class_fighter"));
    std::list<FNpcShipArch*> Archs;
    GetFactionShipLineArchs(Faction, &Line, &Archs);

    unsigned char bLevels[0x1a] = {};
    for (std::list<FNpcShipArch*>::const_iterator It = Archs.begin(); It != Archs.end(); ++It)
    {
        int const Level = (*It)->Level;
        if (Level != 0)
            bLevels[Level] = 1;
    }

    int Counts[0x1a];
    for (int Level = 1; Level <= 0x19; ++Level)
    {
        Counts[Level] = -1;
        if (bLevels[Level] == 0)
            continue;
        std::vector<float> const& Diffs = GRmNpcRankToDifficulty[Level];
        for (unsigned int Index = 0; Index < Diffs.size(); ++Index)
        {
            if (Diffs[Index] > Difficulty)
            {
                if (Index != 0)
                {
                    double const Below = static_cast<double>(Difficulty) - Diffs[Index - 1];
                    double const Above = static_cast<double>(Diffs[Index]) - Difficulty;
                    unsigned int const Picked = (Above < Below) ? Index : Index - 1;
                    Counts[Level] = static_cast<int>(Picked) + 1;
                }
                break;
            }
        }
    }

    std::vector<FRmFormationSizeChoice> Choices;
    float const* const Weights =
        pub::SinglePlayer() ? &GRmFormationSizeWeights[0] : &GRmFormationSizeWeights[9];
    for (int Level = 1; Level <= 0x19; ++Level)
    {
        int const Count = Counts[Level];
        if (Count < 1)
            continue;
        FRmFormationSizeChoice Choice;
        Choice.Size.Difficulty = GRmNpcRankToDifficulty[Level][Count - 1];
        Choice.Size.Level = Level;
        Choice.Size.Count = Count;
        Choice.Weight = Weights[Count];
        Choices.insert(Choices.end(), 1, Choice);
    }

    if (!Choices.empty())
    {
        float Sum = 0.0f;
        for (unsigned int Index = 0; Index < Choices.size(); ++Index)
            Sum += Choices[Index].Weight;
        if (Sum != 0.0f)
        {
            float const Target = static_cast<float>(rand()) * (1.0f / 32768.0f) * Sum;
            float Running = 0.0f;
            for (unsigned int Index = 0; Index < Choices.size(); ++Index)
            {
                Running += Choices[Index].Weight;
                if (Target <= Running)
                {
                    *Out = Choices[Index].Size;
                    return;
                }
            }
        }
    }
    Out->Count = 0;
}

namespace
{
    // the counter of the "Vignette_ship_%X" nicknames (the routine).
    unsigned int GRmVignetteShipCounter = 0;
    // the .data buffer the routine formats the nickname into (0x20 bytes: the
    // TU's static-ID globals follow at the field..the field, written by the thunks
    // ).
    char GRmVignetteShipName[0x20];
}

// (06f97580-06f979af, cdecl, 12 dwords). Count 0 -> false (06f975a7).
// GetFactionProps(&GroupId) (unchecked) ->GetFormation(&CreateID("fighters"), &Group +0x80)
// (the routine, result ignored); the slot budget = FindFormation(&Group +0x80) (the routine,
// unchecked) slot count (0 with no slot vector; 0x34 per slot). A 0x10-byte class-id vector
// local (only the ShipClasses vector of FEncounterShip is read by the routine) gets
// CreateID("class_fighter") and CreateID of "D%d" with the level (the field; level 0 copies
// "D1", the field) into an 8-byte stack buffer, each inserted when absent (the routine find,
// insert). the routine(GroupId, &classes, -1) null -> false. bMatchLevel: level 0
// -> false, |arch +0x24 - level| >= 2 (CDQ/XOR/SUB abs) -> false. The ship local (the routine,
// EH state 1): +0x50 = 1, Npc +0x88 = GroupId, Npc +0x90 = arch +0x00. Per ship 0..Count-1 while
// the slot budget (post-decrement) was > 0: the first ship appends *Loot (the routine) when
// Loot is set, +0xd8 = FirstField_0xd8, +0xdc = FirstField_0xdc and, for 2, +0x50 = 0 and +0xd4
// = TargetName; later ships clear the loot vector (the routine / the routine), +0xd8 =
// OtherField_0xd8, +0xdc = OtherField_0xdc, +0x50 = 1, +0xd4 = 0. Then sprintf(the field,
// "Vignette_ship_%X", the field++), the nickname assigned (the routine, 0x30), the ship
// appended to Group +0x84 (the routine). The ship local and the class vector are destroyed;
// true.
bool __cdecl BuildRmShips(FRmShipGroup* Group, unsigned int GroupId, FRmShipSize Size,
                          int FirstField_0xd8, int OtherField_0xd8, int FirstField_0xdc,
                          int OtherField_0xdc, FRmLootSpawn const* Loot, FNpcNameParams* TargetName,
                          bool bMatchLevel)
{
    if (Size.Count == 0)
        return false;

    unsigned int FormationType = CreateID("fighters");
    GetFactionProps(&GroupId)->GetFormation(&FormationType, &Group->Formation.Id);
    int SlotsLeft = static_cast<int>(FindFormation(&Group->Formation.Id)->Slots.size());

    FEncounterShip Classes;
    std::vector<unsigned int>& ClassIds = Classes.ShipClasses;
    unsigned int ClassId = CreateID("class_fighter");
    if (std::find(ClassIds.begin(), ClassIds.end(), ClassId) == ClassIds.end())
        ClassIds.insert(ClassIds.end(), ClassId);

    char LevelClass[8];
    if (Size.Level == 0)
        std::strcpy(LevelClass, "D1");
    else
        std::sprintf(LevelClass, "D%d", Size.Level);
    ClassId = CreateID(LevelClass);
    if (std::find(ClassIds.begin(), ClassIds.end(), ClassId) == ClassIds.end())
        ClassIds.insert(ClassIds.end(), ClassId);

    FNpcShipArch* const Arch = PickFactionShipLineArch(GroupId, &Classes, -1);
    if (Arch == nullptr)
        return false;
    if (bMatchLevel)
    {
        if (Size.Level == 0)
            return false;
        if (std::abs(Arch->Level - Size.Level) >= 2)
            return false;
    }

    FRmShipSpawn Ship;
    bool const bHasLoot = (Loot != nullptr);
    Ship.bRandomName = 1;
    Ship.Npc.Affiliation = GroupId;
    Ship.Npc.NpcShipArch.Id = Arch->Nickname.Id;
    bool bFirst = true;
    for (int Index = 0; Index < Size.Count; ++Index)
    {
        int const Slots = SlotsLeft;
        --SlotsLeft;
        if (Slots <= 0)
            break;

        if (bFirst)
        {
            bFirst = false;
            if (bHasLoot)
                Ship.Loot.insert(Ship.Loot.end(), 1, *Loot);
            Ship.Field_0xd8 = FirstField_0xd8;
            Ship.Field_0xdc = FirstField_0xdc;
            if (FirstField_0xdc == 2)
            {
                Ship.bRandomName = 0;
                Ship.TargetName = TargetName;
            }
        }
        else
        {
            Ship.Loot.clear();
            Ship.Field_0xd8 = OtherField_0xd8;
            Ship.Field_0xdc = OtherField_0xdc;
            Ship.bRandomName = 1;
            Ship.TargetName = nullptr;
        }

        std::sprintf(GRmVignetteShipName, "Vignette_ship_%X", GRmVignetteShipCounter);
        ++GRmVignetteShipCounter;
        Ship.Nickname = GRmVignetteShipName;
        Group->Ships.insert(Group->Ships.end(), 1, Ship);
    }
    return true;
}

// (06f979b0-06f97d89, cdecl, 6 dwords). Component +0x0c = 1; the group is the
// offer group (Query +0x00) when bFriendly, else the hostile group (+0x04). A default ship group
// (the routine temp, EH state 0) is appended to Component +0x34 (the routine) and filled:
// +0x00 = block +0x00, +0x02 = block +0x03, +0x04 = block +0x18, +0x08 = block +0x1c, +0x34 =
// *Position, +0x40 = 0.9f (0x3f666666, friendly) / -0.9f (the field), +0x0c =
// (&DestSys). Ship values (+0xd8): first / others 0 / 0; block +0x02 -> 2 / 2;
// then block +0x01 -> 2 / 0. Loot: block +0x04 set, or else the bonus drop chance
// (the routine) above rand() * (1/32768) stored as float (FCOMP + TEST AH,0x41 + JNZ: not
// above -> none). The loot local (inline FRmLootSpawn template, EH state 1) when looted: Info
// +0x60 = 0; critical (block +0x04): the routine(group, block +0x14), +0xfc = 2, +0x104 = the
// record, +0x100 = block +0x14, archetype = record +0x0c, units 1; else the routine(Query
// +0x04, Query +0x18) (unchecked), +0xfc = 1, +0x100 / +0x104 = 0, archetype = record +0x00,
// units = (rand() * (max - min + 1)) / 0x8000 + min; then +0xe4 = 1, system = DestSys,
// +0xd8..+0xe0 = 0, hit points scale = rand() * (1/32768) * 0.7f (the field, double) +
// 0.3f (the field, double) stored as float. the routine(group, group id, block size,
// first, others, block +0x01 ? 2 : 1, 1, the loot or null, TargetName, !bFriendly).
bool __cdecl BuildRmShipGroup(FRmOfferQuery const* Query, FRmShipComponent* Component,
                              FRmShipBlock const* Block, Vector const* Position,
                              bool bFriendly, FNpcNameParams* TargetName)
{
    Component->bActive = true;
    unsigned int const GroupId =
        bFriendly ? static_cast<unsigned int>(Query->OfferGroup) : Query->EnemyGroup;

    Component->Groups.insert(Component->Groups.end(), 1, FRmShipGroup());
    FRmShipGroup* const Group = &Component->Groups.back();
    Group->Field_0x00 = Block->Field_0x00;
    Group->Field_0x02 = Block->Field_0x03;
    Group->Field_0x04 = Block->Field_0x18;
    Group->Field_0x08 = Block->Field_0x1c;
    Group->Position = *Position;
    Group->Field_0x40 = bFriendly ? 0.9f : -0.9f;
    Group->System = FindSystemInfo(&Query->DestSys);

    int FirstField_0xd8 = 0;
    int OtherField_0xd8 = 0;
    if (Block->Field_0x02 != 0)
    {
        FirstField_0xd8 = 2;
        OtherField_0xd8 = 2;
    }
    if (Block->Field_0x01 != 0)
    {
        FirstField_0xd8 = 2;
        OtherField_0xd8 = 0;
    }

    bool bLoot = true;
    if (Block->Field_0x04 == 0)
    {
        float const Roll = static_cast<float>(static_cast<double>(rand()) *
                                              static_cast<double>(1.0f / 32768.0f));
        bLoot = GetRmBonusLootDropChance() > Roll;
    }

    FRmLootSpawn Loot;
    FRmLootSpawn const* LootArg = nullptr;
    if (bLoot)
    {
        Loot.Info.bCanAiTractor = 0;
        if (Block->Field_0x04 != 0)
        {
            FRmCriticalLoot* const Critical =
                PickCriticalLoot(GroupId, static_cast<unsigned int>(Block->Field_0x14));
            Loot.Kind = 2;
            Loot.CriticalLoot = Critical;
            Loot.Field_0x100 = Block->Field_0x14;
            Loot.Info.EquipArchID = Critical->ArchetypeId;
            Loot.Info.Units = 1;
        }
        else
        {
            FRmBonusLootEntry const* const Bonus =
                PickRmBonusLoot(Query->EnemyGroup, Query->Difficulty);
            Loot.Kind = 1;
            Loot.CriticalLoot = nullptr;
            Loot.Field_0x100 = 0;
            Loot.Info.EquipArchID = Bonus->Archetype;
            int const Min = Bonus->NumToDropMin;
            Loot.Info.Units = static_cast<unsigned int>(
                (rand() * (Bonus->NumToDropMax - Min + 1)) / 0x8000 + Min);
        }
        Loot.bRelPos = 1;
        Loot.Info.SystemID = Query->DestSys;
        Loot.RelPosOffset.x = 0.0f;
        Loot.RelPosOffset.y = 0.0f;
        Loot.RelPosOffset.z = 0.0f;
        Loot.Info.HitPtsScale = static_cast<float>(
            static_cast<double>(rand()) * static_cast<double>(1.0f / 32768.0f) *
                static_cast<double>(0.7f) +
            static_cast<double>(0.3f));
        LootArg = &Loot;
    }

    int const FirstField_0xdc = (Block->Field_0x01 != 0) ? 2 : 1;
    return BuildRmShips(Group, GroupId, Block->Size, FirstField_0xd8, OtherField_0xd8,
                        FirstField_0xdc, 1, LootArg, TargetName, !bFriendly);
}

// (RET 8): a held offer is released through its interface at +0x04 and the
// pointer cleared; the base filter never creates an offer.
bool FRmPathFilter::CreateOffer(FRmOfferQuery const* /*Query*/, FRmOffer** OutOffer)
{
    if (*OutOffer != nullptr)
    {
        (*OutOffer)->Release();
        *OutOffer = nullptr;
    }
    return false;
}

// (06f9a700-06f9abd3, RET 8). new 0x218 + the routine(1) into a local holder
// (AddRef, then the new reference dropped: 06f9a763-06f9a77e); General active (+0xc4), type
// 1 (+0x54); the solars (the routine, its result is the first success value); the ship
// component's position = the objective position (06f9a7a4-06f9a7d5). Shares (x87): Divisor
// = at_installation + after_30_s + waves * 0.5 (the field), at least 1.0 (TEST AH,0x41 +
// JNZ keeps it when 1.0 <= it); HostileShare = 0.6 (the field) / Divisor * Difficulty,
// FriendlyShare = HostileShare * 0.5, both stored as float. Then one ship block per flag:
// hostile at installation (+0x13: block +0/+2 = pk_hostile_ships), friendly at installation
// (+0x17: +0x18/+0x1c = 1), hostile after 30 s (+0x14: +0/+2 = pk flag; +0x18/+0x1c = 3/1
// with at_installation, else 1/8), friendly after 30 s (+0x18: +0x18/+0x1c = 1/9), hostile
// waves (+0x15: +2 = pk flag; +0x18/+0x1c = 3/1 when (at_installation or after_30_s) and
// rand() is odd, else 1/0xf0). Each block's size comes from the routine (hostile: the
// enemy group and HostileShare; friendly: the offer group and FriendlyShare; a friendly block
// with no count gets count 1 and, with level 0, level 1 -- on a copy of the block). Last the
// offer goes to *OutOffer (the old one released, the new one AddRef'ed when they differ) and
// the holder is released. Each block goes to the routine (Query, the ship component, the
// block, the position local, friendly, 0) and its result is ANDed into the success byte.
bool FRmDestroySolarsFilter::CreateOffer(FRmOfferQuery const* Query, FRmOffer** OutOffer)
{
    FRmVignetteOffer* const NewOffer = new FRmVignetteOffer(1);
    TRmRef<FRmVignetteOffer> Holder(NewOffer);
    NewOffer->Release();
    FRmVignetteOffer* const Offer = Holder.Get();

    Offer->General.bActive = true;
    Offer->Type = 1;
    bool bSuccess = FillSolarSpawns(Query, &Offer->Solars);
    Vector const Position = Query->ObjectivePos;
    Offer->Ships.Position = Position;

    double Divisor =
        static_cast<double>(static_cast<int>(bHostileShipsAfter30S) +
                            static_cast<int>(bHostileShipsAtInstallation)) +
        static_cast<double>(static_cast<int>(bHostileWaves)) * 0.5;
    if (!(1.0 <= Divisor))
        Divisor = 1.0;
    float const HostileShare =
        static_cast<float>(static_cast<double>(0.6f) / Divisor * Query->Difficulty);
    float const FriendlyShare = HostileShare * 0.5f;

    if (bHostileShipsAtInstallation)
    {
        FRmShipBlock Block = {};
        Block.Field_0x00 = bPkHostileShips;
        Block.Field_0x02 = bPkHostileShips;
        Block.Size.Difficulty = 0.0f;
        Block.Field_0x1c = -1;
        PickRmShipFormationSize(&Block.Size, HostileShare, Query->EnemyGroup);
        bool const bBuilt =
            BuildRmShipGroup(Query, &Offer->Ships, &Block, &Position, false, nullptr);
        bSuccess = bSuccess && bBuilt;
    }

    if (bFriendlyShipsAtInstallation)
    {
        FRmShipBlock Block = {};
        Block.Size.Difficulty = 0.0f;
        Block.Field_0x18 = 1;
        Block.Field_0x1c = 1;
        PickRmShipFormationSize(&Block.Size, FriendlyShare, static_cast<unsigned int>(Query->OfferGroup));
        FRmShipBlock Friendly = Block;
        if (Block.Size.Count == 0)
        {
            if (Block.Size.Level == 0)
                Friendly.Size.Level = 1;
            Friendly.Size.Count = 1;
        }
        bool const bBuilt =
            BuildRmShipGroup(Query, &Offer->Ships, &Friendly, &Position, true, nullptr);
        bSuccess = bSuccess && bBuilt;
    }

    if (bHostileShipsAfter30S)
    {
        FRmShipBlock Block = {};
        Block.Field_0x00 = bPkHostileShips;
        Block.Field_0x02 = bPkHostileShips;
        Block.Size.Difficulty = 0.0f;
        if (bHostileShipsAtInstallation)
        {
            Block.Field_0x18 = 3;
            Block.Field_0x1c = 1;
        }
        else
        {
            Block.Field_0x18 = 1;
            Block.Field_0x1c = 8;
        }
        PickRmShipFormationSize(&Block.Size, HostileShare, Query->EnemyGroup);
        bool const bBuilt =
            BuildRmShipGroup(Query, &Offer->Ships, &Block, &Position, false, nullptr);
        bSuccess = bSuccess && bBuilt;
    }

    if (bFriendlyShipsAfter30S)
    {
        FRmShipBlock Block = {};
        Block.Size.Difficulty = 0.0f;
        Block.Field_0x18 = 1;
        Block.Field_0x1c = 9;
        PickRmShipFormationSize(&Block.Size, FriendlyShare, static_cast<unsigned int>(Query->OfferGroup));
        FRmShipBlock Friendly = Block;
        if (Block.Size.Count == 0)
        {
            if (Block.Size.Level == 0)
                Friendly.Size.Level = 1;
            Friendly.Size.Count = 1;
        }
        bool const bBuilt =
            BuildRmShipGroup(Query, &Offer->Ships, &Friendly, &Position, true, nullptr);
        bSuccess = bSuccess && bBuilt;
    }

    if (bHostileWaves)
    {
        FRmShipBlock Block = {};
        Block.Field_0x02 = bPkHostileShips;
        Block.Size.Difficulty = 0.0f;
        if ((bHostileShipsAtInstallation || bHostileShipsAfter30S) && (rand() % 2) != 0)
        {
            Block.Field_0x18 = 3;
            Block.Field_0x1c = 1;
        }
        else
        {
            Block.Field_0x18 = 1;
            Block.Field_0x1c = 0xf0;
        }
        PickRmShipFormationSize(&Block.Size, HostileShare, Query->EnemyGroup);
        bool const bBuilt =
            BuildRmShipGroup(Query, &Offer->Ships, &Block, &Position, false, nullptr);
        bSuccess = bSuccess && bBuilt;
    }

    if (*OutOffer != Offer)
    {
        if (*OutOffer != nullptr)
        {
            (*OutOffer)->Release();
            *OutOffer = nullptr;
        }
        *OutOffer = Offer;
        Offer->AddRef();
    }
    return bSuccess;
}

// (06f9ac80-06f9af37, thiscall RET 8). The component's position = the
// objective position, active. The solar types: 0 when big_solar (+0xc), 1 when
// defensive_solars (+0xe) (the routine find + insert). PickSolarFormation (the routine)
// null -> false. pub::System::GetNestedProperties(DestSys, position). Per type the difficulty
// is scaled (each product stored as float): type 0 by 0.4 (the field) when
// hostile_ships_at_installation and by 0.6 (the field) when defensive_solars; type 1 by
// 0.4 when hostile_ships_at_installation and by 0.39999998 (the field) when big_solar;
// PickKillableSolar(enemy group, type, the nested properties, scaled) null -> false;
// {entry, scaled} appended (the routine). Then SpawnSolars(Query, EnemyGroup, Difficulty,
// formation, the entries, &Solars->Solars) and true.
bool FRmDestroySolarsFilter::FillSolarSpawns(FRmOfferQuery const* Query,
                                             FRmSolarComponent* Solars) const
{
    Solars->Position = Query->ObjectivePos;
    Solars->bActive = true;

    std::vector<int> Types;
    if (bBigSolar)
    {
        if (std::find(Types.begin(), Types.end(), 0) == Types.end())
            Types.push_back(0);
    }
    if (bDefensiveSolars)
    {
        if (std::find(Types.begin(), Types.end(), 1) == Types.end())
            Types.push_back(1);
    }

    FRmSolarFormation* const Formation = PickSolarFormation(&Types);
    if (Formation == nullptr)
        return false;

    unsigned long NestedProps = 0;
    pub::System::GetNestedProperties(Query->DestSys, Query->ObjectivePos, NestedProps);

    std::vector<std::pair<FRmKillableSolar*, float>> Entries;
    for (std::vector<int>::const_iterator Type = Types.begin(); Type != Types.end(); ++Type)
    {
        float Scaled = Query->Difficulty;
        if (*Type == 0)
        {
            if (bHostileShipsAtInstallation)
                Scaled = Scaled * 0.4f;
            if (bDefensiveSolars)
                Scaled = Scaled * 0.6f;
        }
        else if (*Type == 1)
        {
            if (bHostileShipsAtInstallation)
                Scaled = Scaled * 0.4f;
            if (bBigSolar)
                Scaled = Scaled * 0.39999998f;
        }
        FRmKillableSolar* const Entry = PickKillableSolar(
            Query->EnemyGroup, *Type, static_cast<unsigned int>(NestedProps), Scaled);
        if (Entry == nullptr)
            return false;
        Entries.insert(Entries.end(), 1, std::pair<FRmKillableSolar*, float>(Entry, Scaled));
    }

    SpawnSolars(Query, Query->EnemyGroup, Query->Difficulty, Formation, &Entries,
                &Solars->Solars);
    return true;
}

// (06f9af40-06f9b388, thiscall RET 0x18). For every entry {solar, scaled} in
// order: the mode is 2 when the solar's type is 0 and pk_big_solar (+0xd), or 1 and
// pk_defensive_solars (+0xf), else 0. For each formation slot of the solar's type, while
// nothing of this entry was spawned yet or the committed difficulty is below Difficulty: a
// solar (the routine) named "Vignette_solar_%X" (the field++, at most 0x2f characters)
// of EnemyGroup, the slot's orientation (9 dwords) at the slot position + the objective
// position, the entry's archetype / loadout; fraction = (scaled - min) / (max - min) capped
// at 0.999 (0x3f7fbe77, TEST AH,0x1 + JNZ keeps the lower); pilot
// pilot_rm_solar_*[ftol(choices * fraction)] (CreateID, the routine into the personality),
// +0x4c8 = -0.9 (the field), bPilot 1, DestSys, mode, hit points ftol((max - min) *
// fraction + min), type, entry; the entry's used difficulty += scaled, then the solar is
// appended (operator new 0x500 + the routine copy). A slot loop ends once used + committed
// reach Difficulty; committed += used per entry.
void FRmDestroySolarsFilter::SpawnSolars(
    FRmOfferQuery const* Query, unsigned int EnemyGroup, float Difficulty,
    FRmSolarFormation* Formation,
    std::vector<std::pair<FRmKillableSolar*, float>> const* Entries,
    std::list<FRmSolarSpawn>* OutSolars) const
{
    int const SlotCount = Formation->GetSlotCount();
    float Committed = 0.0f;
    for (std::vector<std::pair<FRmKillableSolar*, float>>::const_iterator Entry =
             Entries->begin();
         Entry != Entries->end(); ++Entry)
    {
        FRmKillableSolar const* const Arch = Entry->first;
        float const Scaled = Entry->second;

        int Mode = 0;
        if (Arch->SolarType == 0)
        {
            if (bPkBigSolar)
                Mode = 2;
        }
        else if (Arch->SolarType == 1)
        {
            if (bPkDefensiveSolars)
                Mode = 2;
        }

        float Used = 0.0f;
        for (int SlotIndex = 0; SlotIndex < SlotCount; ++SlotIndex)
        {
            FRmSolarFormationSlot const* const Slot =
                Formation->GetSlot(static_cast<unsigned int>(SlotIndex));
            if (Slot->SolarType == Arch->SolarType && (Used == 0.0f || Committed < Difficulty))
            {
                FRmSolarSpawn Solar;

                std::sprintf(GRmVignetteSolarName, "Vignette_solar_%X", GRmVignetteSolarCounter);
                ++GRmVignetteSolarCounter;
                unsigned int Length =
                    static_cast<unsigned int>(std::strlen(GRmVignetteSolarName));
                if (Length > 0x2f)
                    Length = 0x2f;
                std::memcpy(Solar.Nickname.Data, GRmVignetteSolarName, Length);
                Solar.Nickname.Data[Length] = '\0';
                Solar.Nickname.Length = Length;

                Solar.Faction = EnemyGroup;
                Solar.Orientation = Slot->Orientation;
                Solar.Position.x = Slot->Position.X + Query->ObjectivePos.x;
                Solar.Position.y = Slot->Position.Y + Query->ObjectivePos.y;
                Solar.Position.z = Slot->Position.Z + Query->ObjectivePos.z;
                Solar.ArchetypeId = Arch->ArchetypeId;
                Solar.LoadoutId = Arch->LoadoutId;

                unsigned int const FractionCapBits = 0x3f7fbe77u;
                float FractionCap = 0.0f;
                std::memcpy(&FractionCap, &FractionCapBits, sizeof(FractionCap));
                float Fraction = (Scaled - Arch->DifficultyMin) /
                                 (Arch->DifficultyMax - Arch->DifficultyMin);
                if (!(Fraction < FractionCap))
                    Fraction = FractionCap;
                int const PilotIndex = static_cast<int>(
                    static_cast<double>(Arch->PilotChoices) * Fraction);
                unsigned int PilotId = CreateID(GRmSolarPilotNames[PilotIndex]);
                GetPilotPersonality(&PilotId, &Solar.Personality);

                Solar.Field_0x4c8 = -0.9f;
                Solar.bPilot = 1;
                Solar.DestSys = Query->DestSys;
                Solar.ModeFlag = Mode;
                Solar.Hitpoints = static_cast<int>(
                    (static_cast<double>(Arch->HitpointsMax) - Arch->HitpointsMin) * Fraction +
                    Arch->HitpointsMin);
                // the local's (empty) loot vector cleared.
                Solar.Loot.clear();
                Solar.SolarType = Arch->SolarType;
                Solar.Arch = Arch;
                Used = Scaled + Used;
                OutSolars->push_back(Solar);
            }
            if (Used + Committed >= Difficulty)
                break;
        }
        Committed = Used + Committed;
    }
}

// (06f9b790-06f9ba45, RET 8). new 0x218 + the routine(Flags[0] ? 2 : 3)
// (NEG/SBB/ADD 3) into a local holder (AddRef, the new reference dropped); success starts
// true; General active; type Flags[0] ? (Flags[3] ? 4 : Flags[4] ? 5 : Flags[5] ? 3 : 2) : 0;
// a copy of the query whose difficulty is scaled by 0.9 (the field) when Flags[2] and
// Flags[3]. With Flags[0]: the enemy group's male first names (GetFactionNames(&group)+0x18,
// empty test) must exist, the target name's gender = 1 and BuildNpcName(group, a
// default request, &TargetName, true) must succeed -- else the holder is released and false.
// Then the target count (Flags[7]: Flags[0x13] ? Flags[0x15] + 1 : Flags[9] + Flags[8]; plus
// Flags[0x13] ? Flags[0x12] + 2 * Flags[0x17] : Flags[0x11] + Flags[0x12] + Flags[0x10] +
// Flags[0xf]) splits the difficulty (difficulty / ((count + 1.0) * 0.5) when non-zero, else
// 0) between the routine (the target ships, only with Flags[7]; its result replaces the
// success byte) and the routine (ships and solars, ANDed); then *OutOffer takes the offer as
// in the routine and the holder is released.
bool FRmAssassinateFilter::CreateOffer(FRmOfferQuery const* Query, FRmOffer** OutOffer)
{
    FRmVignetteOffer* const NewOffer = new FRmVignetteOffer(Flags[0] ? 2 : 3);
    TRmRef<FRmVignetteOffer> Holder(NewOffer);
    NewOffer->Release();
    FRmVignetteOffer* const Offer = Holder.Get();

    Offer->General.bActive = true;
    if (Flags[0])
    {
        if (Flags[3])
            Offer->Type = 4;
        else if (Flags[4])
            Offer->Type = 5;
        else if (Flags[5])
            Offer->Type = 3;
        else
            Offer->Type = 2;
    }
    else
    {
        Offer->Type = 0;
    }

    FRmOfferQuery Scaled = *Query;
    if (Flags[2] && Flags[3])
        Scaled.Difficulty = Scaled.Difficulty * 0.9f;

    if (Flags[0])
    {
        FFactionNames* const Names = GetFactionNames(&Scaled.EnemyGroup);
        if (Names->MaleFirstNames.IsEmpty())
            return false;
        Offer->TargetName.Gender = 1;
        FNpcNameRequest Request;
        if (!BuildNpcName(Scaled.EnemyGroup, &Request, &Offer->TargetName, true))
            return false;
    }

    // the target count and the difficulty share (FILD, FADD 1.0, FMUL 0.5,
    // FDIVR the scaled difficulty, FSTP float); 0 without a count.
    int Count = 0;
    if (Flags[7])
        Count = Flags[19] ? static_cast<int>(Flags[21]) + 1
                          : static_cast<int>(Flags[9]) + static_cast<int>(Flags[8]);
    int const MainBattle =
        Flags[19] ? static_cast<int>(Flags[18]) + static_cast<int>(Flags[23]) * 2
                  : static_cast<int>(Flags[17]) + static_cast<int>(Flags[18]) +
                        static_cast<int>(Flags[16]) + static_cast<int>(Flags[15]);
    Count = Count + MainBattle;
    float Share = 0.0f;
    if (Count != 0)
        Share = static_cast<float>(static_cast<double>(Scaled.Difficulty) /
                                   ((static_cast<double>(Count) + 1.0) * 0.5));

    // the target ships replace the success byte; the ship groups are ANDed.
    bool bSuccess = true;
    if (Flags[7])
        bSuccess = FillTargetShips(&Scaled, &Offer->TargetShips, Share, &Offer->TargetName);
    bool const bShips =
        FillShipGroups(&Scaled, &Offer->Ships, Share, &Offer->Solars, &Offer->TargetName);
    bSuccess = bSuccess && bShips;

    // *OutOffer takes the offer (the old one released, the new one
    // AddRef'ed when they differ); the holder is released on return.
    if (*OutOffer != Offer)
    {
        if (*OutOffer != nullptr)
        {
            (*OutOffer)->Release();
            *OutOffer = nullptr;
        }
        *OutOffer = Offer;
        Offer->AddRef();
    }
    return bSuccess;
}

namespace
{
    // The inline block templates of the routine / the routine: the five flag bytes, the size
    // zeroed (difficulty the field 0.0), +0x14 = 0 and the two ints.
    void ResetRmShipBlock(FRmShipBlock* Block, unsigned char Field00, unsigned char Field01,
                          unsigned char Field02, unsigned char Field03, unsigned char Field04,
                          int Field18, int Field1c)
    {
        Block->Field_0x00 = Field00;
        Block->Field_0x01 = Field01;
        Block->Field_0x02 = Field02;
        Block->Field_0x03 = Field03;
        Block->Field_0x04 = Field04;
        Block->Size.Difficulty = 0.0f;
        Block->Size.Level = 0;
        Block->Size.Count = 0;
        Block->Field_0x14 = 0;
        Block->Field_0x18 = Field18;
        Block->Field_0x1c = Field1c;
    }

    // The friendly block fix of the routine (as the routine): a copy of the block; with no
    // count, level 0 becomes 1 and the count 1.
    FRmShipBlock CopyFriendlyRmShipBlock(FRmShipBlock const* Block)
    {
        FRmShipBlock Copy = *Block;
        if (Block->Size.Count == 0)
        {
            if (Block->Size.Level == 0)
                Copy.Size.Level = 1;
            Copy.Size.Count = 1;
        }
        return Copy;
    }

    // The critical loot kind of a target block (06f9c31b-06f9c349 / 06f9c913-06f9c93d):
    // bring_back_loot 3, else tractor_in_loot 2, else destroy_loot 1, else 0.
    int GetRmCriticalLootKind(bool const* Flags)
    {
        if (Flags[3])
            return 3;
        if (Flags[4])
            return 2;
        return Flags[5] ? 1 : 0;
    }
}

// (06f9c100-06f9c406, thiscall RET 0x10). +0x50 / +0x54 start 0 / -1. The
// position = objective + random_inside_ring(6000.0 (the field), 6000.0 - 500.0
// (the field), 300.0) (float adds), also written to TargetShips +0x44. With pk_all_ships
// (+0x1f): pre_battle_wave_runs (+0x16) sets 2 / 1; a block {1,0,1,0,0, +0x18 0, +0x1c -1}
// sized for Share against the hostile group gives the success byte (the routine, not
// friendly, no name); pre_battle_wave_2 (+0x21) re-sizes the same block with +0x18 / +0x1c =
// 3 / 1 and ANDs a second group. Otherwise: pre_battle_non_target_wave (+0x15) builds the same
// first block (and, without target_spawns_at_pre_battle (+0x14), +0x50 = pre_battle_wave_runs
// ? 2 (+0x54 1) : 0); target_spawns_at_pre_battle builds the target's block {1,1,0,1,
// target_drops_critical_loot, +0x14 the critical kind, +0x1c -1, +0x18: 3 / 1 when the
// non-target wave ran and its last group has more than one ship (0x88888889 / 0xf0), 2 / 8
// when it ran with fewer, else 0}, +0x50 / +0x54 = 3 / 0x63, sized for Share * 1.5
// (the field) and built with TargetName. The two ints are stored last.
bool FRmAssassinateFilter::FillTargetShips(FRmOfferQuery const* Query,
                                           FRmTargetShipComponent* TargetShips, float Share,
                                           FNpcNameParams* TargetName) const
{
    int Field50 = 0;
    int Field54 = -1;
    bool bSuccess = true;

    Vector const Offset = random_inside_ring(6000.0f, 6000.0f - 500.0f, 300.0f);
    Vector Position;
    Position.x = Query->ObjectivePos.x + Offset.x;
    Position.y = Query->ObjectivePos.y + Offset.y;
    Position.z = Query->ObjectivePos.z + Offset.z;
    TargetShips->Position = Position;

    FRmShipBlock Block;
    if (Flags[19])
    {
        if (Flags[10])
        {
            Field50 = 2;
            Field54 = 1;
        }
        ResetRmShipBlock(&Block, 1, 0, 1, 0, 0, 0, -1);
        PickRmShipFormationSize(&Block.Size, Share, Query->EnemyGroup);
        bSuccess = BuildRmShipGroup(Query, TargetShips, &Block, &Position, false, nullptr);
        if (Flags[21])
        {
            Block.Field_0x18 = 3;
            Block.Field_0x1c = 1;
            PickRmShipFormationSize(&Block.Size, Share, Query->EnemyGroup);
            bool const bBuilt =
                BuildRmShipGroup(Query, TargetShips, &Block, &Position, false, nullptr);
            bSuccess = bSuccess && bBuilt;
        }
    }
    else
    {
        if (Flags[9])
        {
            ResetRmShipBlock(&Block, 1, 0, 1, 0, 0, 0, -1);
            if (!Flags[8])
            {
                if (Flags[10])
                {
                    Field50 = 2;
                    Field54 = 1;
                }
                else
                {
                    Field50 = 0;
                }
            }
            PickRmShipFormationSize(&Block.Size, Share, Query->EnemyGroup);
            bSuccess = BuildRmShipGroup(Query, TargetShips, &Block, &Position, false, nullptr);
        }
        if (Flags[8])
        {
            ResetRmShipBlock(&Block, 1, 1, 0, 1, Flags[2] ? 1 : 0, 0, -1);
            if (Flags[2])
                Block.Field_0x14 = GetRmCriticalLootKind(Flags);
            if (Flags[9])
            {
                std::vector<FRmShipSpawn> const& Ships = TargetShips->Groups.back().Ships;
                if (Ships.size() > 1)
                {
                    Block.Field_0x18 = 3;
                    Block.Field_0x1c = 1;
                }
                else
                {
                    Block.Field_0x18 = 2;
                    Block.Field_0x1c = 8;
                }
            }
            else
            {
                Block.Field_0x18 = 0;
            }
            Field50 = 3;
            Field54 = 0x63;
            PickRmShipFormationSize(&Block.Size, Share * 1.5f, Query->EnemyGroup);
            bool const bBuilt =
                BuildRmShipGroup(Query, TargetShips, &Block, &Position, false, TargetName);
            bSuccess = bSuccess && bBuilt;
        }
    }

    TargetShips->Field_0x50 = Field50;
    TargetShips->Field_0x54 = Field54;
    return bSuccess;
}

// (06f9c410-06f9caaf, thiscall RET 0x14). defensive_solars_at_main_battle
// (+0x1a) -> the success byte = the routine(Query, Solars). Ships +0x44 = the objective
// position (also the position local); FriendlyShare = Share * 0.5 (the field, float).
// With pk_all_ships (+0x1f) -- hostile block {1,0,1,0,0, 0 / -1}, friendly block {0,0,0,0,0,
// 2 / 1}, both sized from zero once -- in order: main_battle_wave_1 (+0x22) a hostile group
// (Share, hostile group); friendly_ships_at_main_battle (+0x24) a friendly group (FriendlyShare,
// offer group, the count fix on a copy); main_battle_wave_2 (+0x23) a hostile group with 3 / 1
// after wave 1, else 1 / 8; friendly_ships_after_30_s (+0x25) a friendly group with 1 / 0x3c;
// then continuous_reenforcements (+0x1e): hostile {0,.,1,.} 2 / 0xf0 and return; else
// friendly_ships_come_in_to_whoop_up (+0x26): friendly 2 / 9, then two more with +0x1c 0x19 on
// the same size, and return. Without pk_all_ships: main_battle_non_target_wave (+0x1c) a
// hostile group {t,0,t,0,0, 0 / -1} with t = target_spawns_at_main_battle; that flag (+0x1b)
// the target's group {1,1,0,1, target_drops_critical_loot, +0x14 the critical kind; 2 / 0x2d
// with the non-target wave, else 0 / -1} sized for Share * 1.5 (x 1.3, the field, without
// the wave) and built with TargetName; wave_just_after_main_battle_starts (+0x1d) {0..} 2 /
// 0xf0; continuous_reenforcements {0..} 2 / 0x78. Every group result is ANDed in; the size
// picks reuse each block, so a failed pick keeps the previous level.
bool FRmAssassinateFilter::FillShipGroups(FRmOfferQuery const* Query, FRmShipComponent* Ships,
                                          float Share, FRmSolarComponent* Solars,
                                          FNpcNameParams* TargetName) const
{
    Vector const Position = Query->ObjectivePos;
    bool bSuccess = true;
    if (Flags[14])
        bSuccess = FillDefensiveSolars(Query, Solars);
    Ships->Position = Query->ObjectivePos;
    float FriendlyShare = Share * 0.5f;
    unsigned int const OfferGroup = static_cast<unsigned int>(Query->OfferGroup);

    FRmShipBlock Hostile;
    FRmShipBlock Friendly;
    if (Flags[19])
    {
        ResetRmShipBlock(&Hostile, 1, 0, 1, 0, 0, 0, -1);
        ResetRmShipBlock(&Friendly, 0, 0, 0, 0, 0, 2, 1);
        if (Flags[22])
        {
            PickRmShipFormationSize(&Hostile.Size, Share, Query->EnemyGroup);
            bool const bBuilt =
                BuildRmShipGroup(Query, Ships, &Hostile, &Position, false, nullptr);
            bSuccess = bSuccess && bBuilt;
        }
        if (Flags[24])
        {
            PickRmShipFormationSize(&Friendly.Size, FriendlyShare, OfferGroup);
            FRmShipBlock const Copy = CopyFriendlyRmShipBlock(&Friendly);
            bool const bBuilt = BuildRmShipGroup(Query, Ships, &Copy, &Position, true, nullptr);
            bSuccess = bSuccess && bBuilt;
        }
        if (Flags[23])
        {
            if (Flags[22])
            {
                Hostile.Field_0x18 = 3;
                Hostile.Field_0x1c = 1;
            }
            else
            {
                Hostile.Field_0x18 = 1;
                Hostile.Field_0x1c = 8;
            }
            PickRmShipFormationSize(&Hostile.Size, Share, Query->EnemyGroup);
            bool const bBuilt =
                BuildRmShipGroup(Query, Ships, &Hostile, &Position, false, nullptr);
            bSuccess = bSuccess && bBuilt;
        }
        if (Flags[25])
        {
            Friendly.Field_0x18 = 1;
            Friendly.Field_0x1c = 0x3c;
            PickRmShipFormationSize(&Friendly.Size, FriendlyShare, OfferGroup);
            FRmShipBlock const Copy = CopyFriendlyRmShipBlock(&Friendly);
            bool const bBuilt = BuildRmShipGroup(Query, Ships, &Copy, &Position, true, nullptr);
            bSuccess = bSuccess && bBuilt;
        }
        if (Flags[18])
        {
            Hostile.Field_0x00 = 0;
            Hostile.Field_0x02 = 1;
            Hostile.Field_0x18 = 2;
            Hostile.Field_0x1c = 0xf0;
            PickRmShipFormationSize(&Hostile.Size, Share, Query->EnemyGroup);
            bool const bBuilt =
                BuildRmShipGroup(Query, Ships, &Hostile, &Position, false, nullptr);
            return bSuccess && bBuilt;
        }
        if (Flags[26])
        {
            Friendly.Field_0x18 = 2;
            Friendly.Field_0x1c = 9;
            PickRmShipFormationSize(&Friendly.Size, FriendlyShare, OfferGroup);
            for (int Wave = 0; Wave < 3; ++Wave)
            {
                if (Wave != 0)
                    Friendly.Field_0x1c = 0x19;
                FRmShipBlock const Copy = CopyFriendlyRmShipBlock(&Friendly);
                bool const bBuilt =
                    BuildRmShipGroup(Query, Ships, &Copy, &Position, true, nullptr);
                bSuccess = bSuccess && bBuilt;
            }
        }
        return bSuccess;
    }

    if (Flags[16])
    {
        unsigned char const bTarget = Flags[15] ? 1 : 0;
        ResetRmShipBlock(&Hostile, bTarget, 0, bTarget, 0, 0, 0, -1);
        PickRmShipFormationSize(&Hostile.Size, Share, Query->EnemyGroup);
        bool const bBuilt = BuildRmShipGroup(Query, Ships, &Hostile, &Position, false, nullptr);
        bSuccess = bSuccess && bBuilt;
    }
    if (Flags[15])
    {
        ResetRmShipBlock(&Hostile, 1, 1, 0, 1, 0, 0, -1);
        if (Flags[16])
        {
            Hostile.Field_0x18 = 2;
            Hostile.Field_0x1c = 0x2d;
        }
        else
        {
            Hostile.Field_0x18 = 0;
        }
        Hostile.Field_0x04 = Flags[2] ? 1 : 0;
        if (Flags[2])
            Hostile.Field_0x14 = GetRmCriticalLootKind(Flags);
        FriendlyShare = Share * 1.5f;
        if (!Flags[16])
            FriendlyShare = FriendlyShare * 1.3f;
        PickRmShipFormationSize(&Hostile.Size, FriendlyShare, Query->EnemyGroup);
        bool const bBuilt =
            BuildRmShipGroup(Query, Ships, &Hostile, &Position, false, TargetName);
        bSuccess = bSuccess && bBuilt;
    }
    if (Flags[17])
    {
        ResetRmShipBlock(&Hostile, 0, 0, 0, 0, 0, 2, 0xf0);
        PickRmShipFormationSize(&Hostile.Size, Share, Query->EnemyGroup);
        bool const bBuilt = BuildRmShipGroup(Query, Ships, &Hostile, &Position, false, nullptr);
        bSuccess = bSuccess && bBuilt;
    }
    if (Flags[18])
    {
        ResetRmShipBlock(&Hostile, 0, 0, 0, 0, 0, 2, 0x78);
        PickRmShipFormationSize(&Hostile.Size, Share, Query->EnemyGroup);
        bool const bBuilt = BuildRmShipGroup(Query, Ships, &Hostile, &Position, false, nullptr);
        bSuccess = bSuccess && bBuilt;
    }
    return bSuccess;
}

// (06f9bee0-06f9c0fd, thiscall RET 8). Count = (rand() * 3) / 0x8000 + 1;
// Solars +0x0c = 1, +0x34 = the objective position. Types {1} (the routine find +
// insert); PickSolarFormation null -> false. GetNestedProperties(DestSys,
// position); Total = Difficulty * 0.25 (the field, float); PerSolar = Total / Count
// (FILD, FDIVR, float); PickKillableSolar(hostile group, 1, props, PerSolar) null -> false;
// {entry, PerSolar} appended (the routine); the routine(Query, hostile group, Total,
// formation, entries, Solars +0x40); true. The two vectors are locals (EH states 0 / 1).
bool FRmAssassinateFilter::FillDefensiveSolars(FRmOfferQuery const* Query,
                                               FRmSolarComponent* Solars) const
{
    int const Count = (rand() * 3) / 0x8000 + 1;
    Solars->bActive = true;
    Solars->Position = Query->ObjectivePos;

    std::vector<int> Types;
    int const Type = 1;
    if (std::find(Types.begin(), Types.end(), Type) == Types.end())
        Types.insert(Types.end(), Type);

    FRmSolarFormation* const Formation = PickSolarFormation(&Types);
    if (Formation == nullptr)
        return false;

    unsigned long NestedProps = 0;
    pub::System::GetNestedProperties(Query->DestSys, Query->ObjectivePos, NestedProps);

    std::vector<std::pair<FRmKillableSolar*, float>> Entries;
    float const Total = Query->Difficulty * 0.25f;
    float const PerSolar =
        static_cast<float>(static_cast<double>(Total) / static_cast<double>(Count));
    FRmKillableSolar* const Entry = PickKillableSolar(
        Query->EnemyGroup, 1, static_cast<unsigned int>(NestedProps), PerSolar);
    if (Entry == nullptr)
        return false;
    Entries.insert(Entries.end(), 1, std::pair<FRmKillableSolar*, float>(Entry, PerSolar));

    SpawnSolars(Query, Query->EnemyGroup, Total, Formation, &Entries, &Solars->Solars);
    return true;
}

// (06f9bb00-06f9bed8, thiscall RET 0x18): the routine instruction for
// instruction except that no pk mode is computed -- each solar's +0x4d8 is 0. The nickname
// goes through the field and the field as there; the solar local's empty loot vector is
// cleared before the append (06f9bd5c-06f9bd90), a no-op on the fresh local.
void FRmAssassinateFilter::SpawnSolars(
    FRmOfferQuery const* Query, unsigned int EnemyGroup, float Difficulty,
    FRmSolarFormation* Formation,
    std::vector<std::pair<FRmKillableSolar*, float>> const* Entries,
    std::list<FRmSolarSpawn>* OutSolars) const
{
    int const SlotCount = Formation->GetSlotCount();
    float Committed = 0.0f;
    for (std::vector<std::pair<FRmKillableSolar*, float>>::const_iterator Entry =
             Entries->begin();
         Entry != Entries->end(); ++Entry)
    {
        FRmKillableSolar const* const Arch = Entry->first;
        float const Scaled = Entry->second;

        float Used = 0.0f;
        for (int SlotIndex = 0; SlotIndex < SlotCount; ++SlotIndex)
        {
            FRmSolarFormationSlot const* const Slot =
                Formation->GetSlot(static_cast<unsigned int>(SlotIndex));
            if (Slot->SolarType == Arch->SolarType && (Used == 0.0f || Committed < Difficulty))
            {
                FRmSolarSpawn Solar;

                std::sprintf(GRmVignetteSolarName, "Vignette_solar_%X", GRmVignetteSolarCounter);
                ++GRmVignetteSolarCounter;
                unsigned int Length =
                    static_cast<unsigned int>(std::strlen(GRmVignetteSolarName));
                if (Length > 0x2f)
                    Length = 0x2f;
                std::memcpy(Solar.Nickname.Data, GRmVignetteSolarName, Length);
                Solar.Nickname.Data[Length] = '\0';
                Solar.Nickname.Length = Length;

                Solar.Faction = EnemyGroup;
                Solar.Orientation = Slot->Orientation;
                Solar.Position.x = Slot->Position.X + Query->ObjectivePos.x;
                Solar.Position.y = Slot->Position.Y + Query->ObjectivePos.y;
                Solar.Position.z = Slot->Position.Z + Query->ObjectivePos.z;
                Solar.ArchetypeId = Arch->ArchetypeId;
                Solar.LoadoutId = Arch->LoadoutId;

                unsigned int const FractionCapBits = 0x3f7fbe77u;
                float FractionCap = 0.0f;
                std::memcpy(&FractionCap, &FractionCapBits, sizeof(FractionCap));
                float Fraction = (Scaled - Arch->DifficultyMin) /
                                 (Arch->DifficultyMax - Arch->DifficultyMin);
                if (!(Fraction < FractionCap))
                    Fraction = FractionCap;
                int const PilotIndex = static_cast<int>(
                    static_cast<double>(Arch->PilotChoices) * Fraction);
                unsigned int PilotId = CreateID(GRmSolarPilotNames[PilotIndex]);
                GetPilotPersonality(&PilotId, &Solar.Personality);

                Solar.Field_0x4c8 = -0.9f;
                Solar.bPilot = 1;
                Solar.DestSys = Query->DestSys;
                Solar.ModeFlag = 0;
                Solar.Hitpoints = static_cast<int>(
                    (static_cast<double>(Arch->HitpointsMax) - Arch->HitpointsMin) * Fraction +
                    Arch->HitpointsMin);
                Solar.Loot.clear();
                Solar.SolarType = Arch->SolarType;
                Solar.Arch = Arch;
                Used = Scaled + Used;
                OutSolars->push_back(Solar);
            }
            if (Used + Committed >= Difficulty)
                break;
        }
        Committed = Used + Committed;
    }
}

// (RET 0xc). the routine(DestSys) first, then a held offer is released
// ((Offer + 4)->Release) and cleared. The query must have OfferGroup and EnemyGroup set,
// Reward > 0 (signed), RepReward > 0 (TEST AH,0x41 + JNZ fails on <= and NaN), RepHit < 0
// (TEST AH,0x5 + JP fails on >= and NaN), OfferBase, 0 <= Difficulty (TEST AH,0x1 + JNZ)
// and Difficulty <= 100 (TEST AH,0x41 + JP fails only above), ObjectiveZone, a non-zero
// |ObjectivePos|^2 (unrounded x87 sum, TEST AH,0x44 + JNP fails only on == 0), PlayerId
// and DestSys. Then the routine (-1 fails), the routine and the routine, whose result
// is returned.
bool FRandomMissionGenerator::BuildOffer(FRmOfferQuery const* Query, FRmOffer** OutOffer,
                                         FRmTreePath* OutPath)
{
    RegisterRmDestSystem(Query->DestSys);
    if (*OutOffer != nullptr)
    {
        (*OutOffer)->Release();
        *OutOffer = nullptr;
    }

    if (Query->OfferGroup == -1)
        return false;
    if (Query->EnemyGroup == 0xffffffffu)
        return false;
    if (Query->Reward <= 0)
        return false;
    if (!(Query->RepReward > 0.0f))
        return false;
    if (!(Query->RepHit < 0.0f))
        return false;
    if (Query->OfferBase == 0)
        return false;
    if (Query->Difficulty < 0.0f || std::isnan(Query->Difficulty))
        return false;
    if (Query->Difficulty > 100.0f)
        return false;
    if (Query->ObjectiveZone == 0)
        return false;
    {
        double const X = Query->ObjectivePos.x;
        double const Y = Query->ObjectivePos.y;
        double const Z = Query->ObjectivePos.z;
        if (X * X + Y * Y + Z * Z == 0.0)
            return false;
    }
    if (Query->PlayerId == 0)
        return false;
    if (Query->DestSys == 0)
        return false;

    unsigned int const OfferNodeId = PickOfferNode(Query);
    if (OfferNodeId == 0xffffffffu)
        return false;
    if (!ChooseTreePath(OutPath, OfferNodeId, Query->Difficulty, Query->OfferGroup))
        return false;
    return BuildOfferFromPath(OutPath, Query, OutOffer);
}

// (RET 0xc). A base path filter (new 0xc, vtable 06fbcf6c) walks the path from
// RootNodeId: a node whose allowable_zone_types misses the objective zone's vignette type
// (the routine -> the routine -> zone+0x14, both unchecked as baseline) fails. Each node
// goes to ApplyNode(node, bit); when that and HasMissionType are both true the filter is
// replaced by Specialize() (the old one freed with plain operator delete). Bit 1 steps to
// Children[0], anything else to Children[1]. The last filter's CreateOffer must succeed with
// a non-null offer (06f9eaf5-06f9eb08); the offer then gets (06f9eb0e-06f9ec09) the system,
// the groups, the remapped difficulty, reward, reputation hit and reward, the offer base,
// the offer group's mc_costume (GetFactionProps, not null-tested) and the "mc_leg_m01"
// voice; the texts and objectives (the routine); for machine types 1..3 the comm sequences
// (the routine).
bool FRandomMissionGenerator::BuildOfferFromPath(FRmTreePath const* Path,
                                                 FRmOfferQuery const* Query,
                                                 FRmOffer** OutOffer)
{
    FRmPathFilter* Filter = new FRmPathFilter();
    FSystemInfo* System = FindSystemInfo(&Query->DestSys);
    unsigned int const ZoneTypes = System->FindZone(Query->ObjectiveZone)->VignetteType;

    bool bBuilt = false;
    FRmVignetteNode const* Node = &Nodes.find(RootNodeId)->second;
    std::vector<char>::const_iterator BitIt = Path->Bits.begin();
    for (; BitIt != Path->Bits.end(); ++BitIt)
    {
        if ((Node->Params.AllowableZoneTypes & ZoneTypes) == 0)
            break;
        char const Bit = *BitIt;
        if (Filter->ApplyNode(Node, Bit) && Filter->HasMissionType())
        {
            FRmPathFilter* Specialized = Filter->Specialize();
            ::operator delete(static_cast<void*>(Filter));
            Filter = Specialized;
        }
        unsigned int const NextId = (Bit == 1) ? Node->Children[0] : Node->Children[1];
        Node = &Nodes.find(NextId)->second;
    }

    if (BitIt == Path->Bits.end() && Filter->CreateOffer(Query, OutOffer) &&
        *OutOffer != nullptr)
    {
        FRmOffer* const Offer = *OutOffer;
        Offer->System = FindSystemInfo(&Query->DestSys);
        Offer->OfferGroup = Query->OfferGroup;
        Offer->HostileGroup = Query->EnemyGroup;
        Offer->Difficulty = RemapRmOfferDifficulty(Query->Difficulty);
        Offer->Reward = Query->Reward;
        Offer->RepHit = Query->RepHit;
        Offer->RepReward = Query->RepReward;
        Offer->OfferBase = Query->OfferBase;
        unsigned int const OfferGroup = static_cast<unsigned int>(Query->OfferGroup);
        Offer->McCostume = GetFactionProps(&OfferGroup)->McCostume;
        pub::GetVoiceID(Offer->VoiceId, "mc_leg_m01");
        // the routine(Query, Path, the filter, *OutOffer, OutOffer).
        BuildOfferTexts(Query, Path, Filter, *OutOffer, OutOffer);
        // *OutOffer read again; machine types 1..3 (signed JLE 0 / JG 3) --
        // a local holder (AddRef), its by-value copy (AddRef) for the routine, which releases
        // it; then the local is released.
        FRmOffer* const Built = *OutOffer;
        if (Built->MachineType > 0 && Built->MachineType <= 3)
        {
            TRmRef<FRmOffer> Holder(Built);
            BuildCommSequences(Path, Holder);
        }
        bBuilt = true;
    }

    ::operator delete(static_cast<void*>(Filter));
    return bBuilt;
}

// (06f9e1f0-06f9e593, RET 0x14; caller the routine 06f9ec23). Text is
// (*OutOffer)+0x3c. First the bSingularNickname bytes (FFactionProps +0x98, the routine) of
// the offer group (Query +0x00) and the hostile group (Query +0x04); the local objective text
// (vtable 06fbd510, map ctor the routine, EH state 0) and Text->begin_mad_lib(0x50001). From
// the root node (the routine find, unchecked) down Path, per node:
//  1. 06f9e2b5-06f9e36e: every objective text of the node (+0x40 params +0x18, map size
//     +0x6c, head +0x60) is inserted uniquely into the local map as {key, empty text}
//     (the routine temp, the routine pair copy, insert the routine, both temps destroyed
//     by the routine, EH states 2 / 1) and the mapped text is then assigned member-wise
//     (FmtStr::operator=, +0x1c, vector assign the routine) -- a later node wins.
//  2. 06f9e374-06f9e445: with offer texts (+0x48 params +0x08, non-null begin and a non-zero
//     count), one is picked: index 0 for a single entry, else (rand() * count) / 0x8000 (IMUL,
//     CDQ, AND 0x7fff, SAR 0xf). the routine (the count) is tested again and its zero branch
//     would take the end pointer -- unreachable after the count test. A "replace" text (+0x04
//     zero) first resets Text (FmtStr::destroy, six dwords zeroed by REP STOSD,
//     begin_mad_lib(0x50001)); then its slot 2 (Build) appends with the two singular flags,
//     Text and Source.
//  3. 06f9e448-06f9e4ae: a reward text (+0xa0) and a failure text (+0x70) that are not empty
//     (slot 3) are assigned to the offer's +0x58 / +0x88 (FmtStr::operator=, +0x1c, vector
//     assign).
//  4. 06f9e4b3-06f9e4fe: no children ends the walk; otherwise the path byte 1 steps to
//     Children[0], anything else to Children[1] (the routine find, unchecked), and the path
//     pointer advances (no end check, as the routine).
// Then the routine(Query, OutOffer, &local, Filter) (06f9e503-06f9e524), Text->end_mad_lib
// and the local map freed (erase the routine, the head and nil nodes).
void FRandomMissionGenerator::BuildOfferTexts(FRmOfferQuery const* Query,
                                              FRmTreePath const* Path, FRmPathFilter* Filter,
                                              FRmOffer* Source, FRmOffer** OutOffer)
{
    FRmOffer* const Offer = *OutOffer;
    FmtStr* const Text = &Offer->Text;
    unsigned int const OfferGroup = static_cast<unsigned int>(Query->OfferGroup);
    bool const bOfferGroupSingular = GetFactionProps(&OfferGroup)->bSingularNickname;
    bool const bHostileGroupSingular = GetFactionProps(&Query->EnemyGroup)->bSingularNickname;

    FRmVignetteNode* Node = &Nodes.find(RootNodeId)->second;
    FRmObjectiveText Objectives;
    Text->begin_mad_lib(0x50001);

    std::vector<char>::const_iterator BitIt = Path->Bits.begin();
    for (;;)
    {
        std::map<int, FRmParamText> const& NodeTexts = Node->Params.ObjectiveText.Texts;
        for (std::map<int, FRmParamText>::const_iterator It = NodeTexts.begin();
             It != NodeTexts.end(); ++It)
        {
            std::pair<std::map<int, FRmParamText>::iterator, bool> Inserted =
                Objectives.Texts.insert(std::make_pair(It->first, FRmParamText()));
            Inserted.first->second = It->second;
        }

        std::vector<FRmOfferText>& OfferTexts = Node->Params.OfferTexts;
        if (!OfferTexts.empty())
        {
            int const Count = static_cast<int>(OfferTexts.size());
            int Index = 0;
            if (Count - 1 != 0)
                Index = (rand() * Count) / 0x8000;
            FRmOfferText* const Chosen = &OfferTexts[0] + static_cast<unsigned int>(Index);
            if (!Chosen->bAppend)
            {
                Text->destroy();
                Text->Head = nullptr;
                Text->MadLibId = 0;
                for (int Kind = 0; Kind < FmtStr::KindCount; ++Kind)
                    Text->ValueIndex[Kind] = 0;
                Text->begin_mad_lib(0x50001);
            }
            Chosen->Build(bOfferGroupSingular, bHostileGroupSingular, Text, Source);
        }

        if (!Node->Params.RewardText.IsEmpty())
            Offer->RewardText = Node->Params.RewardText;
        if (!Node->Params.FailureText.IsEmpty())
            Offer->FailureText = Node->Params.FailureText;

        if (Node->Children.empty())
            break;
        unsigned int const NextId = (*BitIt == 1) ? Node->Children[0] : Node->Children[1];
        Node = &Nodes.find(NextId)->second;
        ++BitIt;
    }

    BuildObjectives(Query, OutOffer, &Objectives, Filter);
    Text->end_mad_lib();
}

// (06f9dee0-06f9e1a9, RET 0x10). A local holder takes *OutOffer (the routine on
// the empty holder, AddRef; EH state 0, released at 06f9e182). The four component objective
// maps are read off the holder unchecked: General +0xd4, Solars +0x160, TargetShips +0x108,
// Ships +0x1ac. Per text in key order (head +8, the routine):
//  - keys 1..3 (guide_to_*): new 0x5c the routine(&text) (EH state 1), then its slot 10
//    (Query->DestSys, 0x44d, Query->ObjectivePos by value); any other key: new 0x70
// (&text) (EH state 2).
//  - the map: switch on key - 1 (byte table 06f9e1c0, jump table 06f9e1ac): 1, 2, 9..13 ->
//    General; 4, 5 -> Solars; 3, 19..21 -> TargetShips; 22..24 -> Ships; every other key keeps
//    ECX 0 (06f9e01f) and the insert then reads through a null map, as baseline.
//  - the key: 19 / 22 -> 6, 20 / 23 -> 7, 21 / 24 -> 8 (second switch 06f9e04a, table
// ); else unchanged.
//  - unique insert {key, objective} (inline the routine path, 06f9e06c-06f9e154); the result
//    is dropped, so an objective whose key is already present is leaked, as baseline.
void FRandomMissionGenerator::BuildObjectives(FRmOfferQuery const* Query, FRmOffer** OutOffer,
                                              FRmObjectiveText const* Texts,
                                              FRmPathFilter* /*Filter*/)
{
    TRmRef<FRmOffer> Holder(*OutOffer);
    FRmVignetteOffer* const Offer = static_cast<FRmVignetteOffer*>(Holder.Get());
    FRmObjectiveMap* const GeneralMap = &Offer->General.Objectives;
    FRmObjectiveMap* const SolarMap = &Offer->Solars.Objectives;
    FRmObjectiveMap* const TargetMap = &Offer->TargetShips.Objectives;
    FRmObjectiveMap* const ShipMap = &Offer->Ships.Objectives;

    for (std::map<int, FRmParamText>::const_iterator It = Texts->Texts.begin();
         It != Texts->Texts.end(); ++It)
    {
        int Key = It->first;
        FRmParamText* Objective = nullptr;
        if (Key == 2 || Key == 1 || Key == 3)
        {
            FRmObjectiveType2* const Guide = new FRmObjectiveType2(&It->second);
            Guide->SetMarker(Query->DestSys, 0x44d, Query->ObjectivePos);
            Objective = Guide;
        }
        else
        {
            Objective = new FRmObjectiveType3(&It->second);
        }

        FRmObjectiveMap* Map = nullptr;
        switch (Key)
        {
        case 1:
        case 2:
        case 9:
        case 10:
        case 11:
        case 12:
        case 13:
            Map = GeneralMap;
            break;
        case 4:
        case 5:
            Map = SolarMap;
            break;
        case 3:
        case 19:
        case 20:
        case 21:
            Map = TargetMap;
            break;
        case 22:
        case 23:
        case 24:
            Map = ShipMap;
            break;
        default:
            break;
        }

        switch (Key)
        {
        case 19:
        case 22:
            Key = 6;
            break;
        case 20:
        case 23:
            Key = 7;
            break;
        case 21:
        case 24:
            Key = 8;
            break;
        default:
            break;
        }

        Map->Objectives.insert(std::make_pair(Key, Objective));
    }
}

// (06f9e770-06f9e9a7, RET 8; caller the routine 06f9ec66). From the root node
// (the routine find, unchecked) down Path: per comm sequence of the node (+0x40 params
// +0xc0, 0x24 each) a local conversation (the routine) is appended by copy to the vignette
// offer's General list (+0xc8, the routine -> the routine) and destroyed (the routine);
// the list's last element (the `&node->value != null` test at 06f9e819 is the compiler's)
// gets slot 7 = the trigger, slot 11 = MinDelay + (MaxDelay - MinDelay) * rand() / 32768
// (x87: FILD, FMUL the field, FLD max, FSUB min, FMULP, FADD min, FSTP float), and per
// {speaker, text} a new FRmLine (+0x44 the sequence's target, +0x40 the speaker, the text
// through the TString assign the routine; the `!= -8` test at 06f9e89d is again the
// compiler's) appended to its lines (the routine). A node with children steps to
// Children[0] when the path byte is 1, else Children[1] (the routine lower bound; a missing
// id would read the end node, as the port's find), and the path pointer advances. At the end
// the holder argument is released (06f9e980-06f9e992, also the unwind state 0).
void FRandomMissionGenerator::BuildCommSequences(FRmTreePath const* Path, TRmRef<FRmOffer> Offer)
{
    FRmVignetteOffer* const Vignette = static_cast<FRmVignetteOffer*>(Offer.Get());
    FRmVignetteNode const* Node = &Nodes.find(RootNodeId)->second;
    std::vector<char>::const_iterator BitIt = Path->Bits.begin();
    for (;;)
    {
        std::vector<FRmCommSequence> const& Sequences = Node->Params.CommSequences;
        for (std::vector<FRmCommSequence>::const_iterator Sequence = Sequences.begin();
             Sequence != Sequences.end(); ++Sequence)
        {
            {
                FRmConversation Local;
                Vignette->General.Conversations.insert(Vignette->General.Conversations.end(),
                                                       Local);
            }
            FRmConversation& Conversation = Vignette->General.Conversations.back();
            Conversation.SetTrigger(Sequence->Trigger);
            double const Fraction =
                static_cast<double>(std::rand()) * static_cast<double>(3.0517578125e-05f);
            double const Delay =
                (static_cast<double>(Sequence->MaxDelay) - static_cast<double>(Sequence->MinDelay)) *
                    Fraction +
                static_cast<double>(Sequence->MinDelay);
            Conversation.SetDelay(static_cast<float>(Delay));
            for (std::vector<FRmCommLine>::const_iterator CommLine = Sequence->Lines.begin();
                 CommLine != Sequence->Lines.end(); ++CommLine)
            {
                FRmLine* const Line = new FRmLine();
                Line->TargetKind = Sequence->Target;
                Line->SpeakerKind = CommLine->Speaker;
                Line->Text = CommLine->Text;
                FGcsLine* const Appended = Line;
                Conversation.Lines.insert(Conversation.Lines.end(), 1, Appended);
            }
        }
        if (Node->Children.empty())
            break;
        unsigned int const NextId = (*BitIt == 1) ? Node->Children[0] : Node->Children[1];
        Node = &Nodes.find(NextId)->second;
        ++BitIt;
    }
}
