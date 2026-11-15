// Author: Giperion.

// Content.dll TU48 (06f4ed80-06f55e20): faction properties, faction name tables, faction
// goods and reputation empathy. Lane CNT-GCS (implementation notes, TU map).

// Listing: DLLS\BIN\Content. The std::map / std::vector instantiations baseline emits
// for the four tables (map ctors the routine / the routine / the routine, _Tidy
// / the branch / the branch, insert the routine / the routine /
// , _Erase the routine / the routine / the routine, erase the routine /
// / the routine, node helpers 06f53960-06f55310, vector insert the routine,
// find the routine) are the std containers of FactionProps.h.

#include "FactionProps.h"

#include "ContentInternal.h"    // OpenContentDataIni, GContentNextUniqueId
#include "ContentSort.h"        // ContentSort (VC6 std::sort)
#include "ContentStaticIds.h"
#include "NpcShips.h"           // ParseSpaceCostume
#include "RandomMissionGen.h"   // FindVoiceProperties, FVoiceProperties
#include "common_ini.h"
#include "common_server.h"
#include "common_server_controller.h"   // pub::SetTimer, pub::Controller::TimerExpired

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <utility>

// the routine (vector ctor) + atexit the routine (vector dtor the routine).
std::vector<unsigned int> GFactionGroups;
// map ctor the routine + atexit the routine.
FFactionPropsMap GFactionProps;
// map ctor the routine + atexit the routine.
FFactionNamesMap GFactionNames;
// map ctor the routine + atexit the routine.
FFactionEmpathyMap GFactionEmpathy;

namespace
{
    // / 06f4efe0 / 06f4f000 (_initterm, in this order): three timer message ids
    // drawn from the module id generator the field, stored in the field (pilot
    // names, sender the routine) / the field (large ship names, the routine) /
    // (formation designations, the routine); all read by the routine.
    unsigned int const GFactionNameMsgPilot = GContentNextUniqueId++;
    unsigned int const GFactionNameMsgLargeShip = GContentNextUniqueId++;
    unsigned int const GFactionNameMsgFormation = GContentNextUniqueId++;

    // The random draw of TU48: rand() * Count, then the signed division by 32768 of the
    // CDQ / AND 0x7fff / ADD / SAR 0xf idiom.
    int FactionRandIndex(int Count)
    {
        return (rand() * Count) / 32768;
    }

    // / the inlined the routine loops: find by the pair only.
    std::vector<FFactionNamePairUse>::iterator FindPairUse(std::vector<FFactionNamePairUse>* Uses,
                                                           int First, int Second)
    {
        std::vector<FFactionNamePairUse>::iterator It = Uses->begin();
        while (It != Uses->end() && !(It->First == First && It->Second == Second))
            ++It;
        return It;
    }

    // / the inlined name loops: find by the name.
    std::vector<FFactionNameUse>::iterator FindNameUse(std::vector<FFactionNameUse>* Uses, int Name)
    {
        std::vector<FFactionNameUse>::iterator It = Uses->begin();
        while (It != Uses->end() && It->Name != Name)
            ++It;
        return It;
    }

    // / the routine / the routine bodies: one more use of the pair, or a
    // new {First, Second, 1} at the end (the routine / the routine).
    void AddPairUse(std::vector<FFactionNamePairUse>* Uses, int First, int Second)
    {
        std::vector<FFactionNamePairUse>::iterator It = FindPairUse(Uses, First, Second);
        if (It != Uses->end())
        {
            ++It->UseCount;
            return;
        }
        FFactionNamePairUse Use;
        Use.First = First;
        Use.Second = Second;
        Use.UseCount = 1;
        Uses->insert(Uses->end(), 1, Use);
    }

    // / the routine paths of the routine: one more use, or {Name, 1} at the
    // end (the routine).
    void AddNameUse(std::vector<FFactionNameUse>* Uses, int Name)
    {
        std::vector<FFactionNameUse>::iterator It = FindNameUse(Uses, Name);
        if (It != Uses->end())
        {
            ++It->UseCount;
            return;
        }
        FFactionNameUse Use;
        Use.Name = Name;
        Use.UseCount = 1;
        Uses->insert(Uses->end(), 1, Use);
    }

    // (thiscall, RET 8) and the immediate paths of the routine /
    // one use less, *OutCount = the new count when given, the entry erased
    // at zero (the routine); a miss sets *OutCount = 0 and answers false.
    bool ReleasePairUse(std::vector<FFactionNamePairUse>* Uses, int First, int Second,
                        int* OutCount)
    {
        std::vector<FFactionNamePairUse>::iterator It = FindPairUse(Uses, First, Second);
        if (It == Uses->end())
        {
            if (OutCount != nullptr)
                *OutCount = 0;
            return false;
        }
        --It->UseCount;
        if (OutCount != nullptr)
            *OutCount = It->UseCount;
        if (It->UseCount == 0)
            Uses->erase(It);
        return true;
    }

    // (thiscall, RET 8) and the immediate path of the routine: the same on
    // a single name.
    bool ReleaseNameUse(std::vector<FFactionNameUse>* Uses, int Name, int* OutCount)
    {
        std::vector<FFactionNameUse>::iterator It = FindNameUse(Uses, Name);
        if (It == Uses->end())
        {
            if (OutCount != nullptr)
                *OutCount = 0;
            return false;
        }
        --It->UseCount;
        if (OutCount != nullptr)
            *OutCount = It->UseCount;
        if (It->UseCount == 0)
            Uses->erase(It);
        return true;
    }

    // The predicate of the empathy-rate sort the routine / the routine (FLD a.rate,
    // FCOMP b.rate, TEST AH,5: a.Rate < b.Rate).
    bool EmpathyRateLess(FFactionEmpathyRate const& A, FFactionEmpathyRate const& B)
    {
        return A.Rate < B.Rate;
    }

    // The three release timers: pub::SetTimer(&SpaceSupport id the field,
    // {MessageId, Group}, 30.0f) (the field / the field / the field).
    void SetFactionNameTimer(unsigned int MessageId, unsigned int Group)
    {
        pub::Controller::TimerExpired Timer;
        Timer.Param1 = MessageId;
        Timer.Param2 = Group;
        pub::SetTimer(GSpaceSupportControllerId, Timer, 30.0f);
    }

    // (vtable 06fbb7a4, RET 4): appends GroupId to GFactionGroups when it is
    // not there yet; the second linear find that follows (06f50196-06f501b9) has no
    // effect. Always answers 1.
    struct FFactionGroupCollector : public pub::Reputation::Enumerator
    {
        char Execute(unsigned int GroupId) override
        {
            if (std::find(GFactionGroups.begin(), GFactionGroups.end(), GroupId) ==
                GFactionGroups.end())
            {
                GFactionGroups.insert(GFactionGroups.end(), 1, GroupId);
            }
            return 1;
        }
    };

    // (vtable 06fbb928, RET 4): ++Count, answers 1.
    struct FFactionGroupCounter : public pub::Reputation::Enumerator
    {
        int Count = 0;              // +0x04

        char Execute(unsigned int GroupId) override
        {
            (void)GroupId;
            ++Count;
            return 1;
        }
    };

    // The reputation clamp of the routine: min(Value, 1.0f - 0.1) first, then the
    // lower bound -1.0f + 0.1 (x87, the field / the field are float, the field
    // is double 0.1). Sum is the unrounded x87 sum, SumF the float the listing stores.
    float ClampFactionReputation(double Sum)
    {
        double const Upper = static_cast<double>(1.0f) - 0.1;
        double const Lower = static_cast<double>(-1.0f) + 0.1;
        float const SumF = static_cast<float>(Sum);

        // Upper < Sum (unrounded) -> Upper, else the stored float.
        double const Candidate = (Upper < Sum) ? Upper : static_cast<double>(SumF);
        // (TEST AH,0x41 + JNZ: Candidate <= Lower -> Lower).
        if (Candidate <= Lower)
            return static_cast<float>(Lower);
        // Upper < SumF -> Upper, else SumF.
        if (Upper < static_cast<double>(SumF))
            return static_cast<float>(Upper);
        return SumF;
    }
}

// ---------------------------------------------------------------------------------------
// FFactionProps
// ---------------------------------------------------------------------------------------

// (06f4f060-06f4f1a6).
FFactionProps::FFactionProps()
    : CommReferrable()
    , JumpPreference(FactionJumpAny)
    , ScanChance(1.0f)
    , bScanAnnounce(true)
    , Legality(FactionLegalityUnset)
    , bSingularNickname(true)
    , McCostume(0)
{
}

// (06f4f3d0-06f4f429).
bool FFactionProps::GetFormation(unsigned int const* Key, unsigned int* Out) const
{
    std::map<unsigned int, unsigned int>::const_iterator It = Formations.find(*Key);
    if (It == Formations.end())
        return false;
    *Out = It->second;
    return true;
}

// (06f4f430-06f4f611).
bool FFactionProps::PickSpaceCostume(int Gender, Costume* Out) const
{
    // the index list is reserved to the costume count first.
    std::vector<unsigned int> Matches;
    if (!SpaceCostumes.empty())
        Matches.reserve(SpaceCostumes.size());

    // the out-gender slot is the argument's own stack slot in baseline.
    for (unsigned int Index = 0; Index < SpaceCostumes.size(); ++Index)
    {
        int SkeletonGender = Gender;
        pub::GF::GetCostumeSkeletonGender(SpaceCostumes[Index], SkeletonGender);
        if (Gender == SkeletonGender)
            Matches.insert(Matches.end(), 1, Index);
    }

    // .
    if (Matches.empty())
        return false;

    int const MatchCount = static_cast<int>(Matches.size());
    int const Pick = (rand() * MatchCount) / 32768;
    Costume const& Source = SpaceCostumes[Matches[Pick]];
    Out->PartIds[0] = Source.PartIds[0];
    Out->PartIds[1] = Source.PartIds[1];
    Out->PartIds[2] = Source.PartIds[2];
    Out->PartIds[3] = Source.PartIds[3];
    Out->AccessoryCount = Source.AccessoryCount;
    for (int Accessory = 0; Accessory < static_cast<int>(Out->AccessoryCount); ++Accessory)
        Out->AccessoryIds[Accessory] = Source.AccessoryIds[Accessory];
    return true;
}

// (06f4f620-06f4f7b7).
bool FFactionProps::PickVoice(unsigned int Roles, unsigned int* Out) const
{
    // a copy of the voice list.
    std::vector<unsigned int> Candidates(Voices.begin(), Voices.end());

    while (!Candidates.empty())
    {
        // one candidate draws index 0 without calling rand().
        int const Count = static_cast<int>(Candidates.size());
        int Pick = 0;
        if (Count != 1)
            Pick = (rand() * Count) / 32768;

        FVoiceProperties* Props = FindVoiceProperties(&Candidates[Pick]);
        if (Props != nullptr && Props->SupportsRoles(Roles))
        {
            *Out = Candidates[Pick];
            return true;
        }
        // erase the candidate.
        Candidates.erase(Candidates.begin() + Pick);
    }
    return false;
}

// (06f4f7c0-06f4f860).
bool FFactionProps::PickGender(int* Out) const
{
    unsigned int MaleCount = 0;
    for (std::vector<unsigned int>::const_iterator It = Voices.begin(); It != Voices.end(); ++It)
    {
        FVoiceProperties* Props = FindVoiceProperties(&*It);
        if (Props != nullptr && Props->SupportsRoles(1))
            ++MaleCount;
    }

    if (Voices.empty())
        return false;

    // CMP + SBB: an unsigned draw below MaleCount -> 1, else 2.
    int const Count = static_cast<int>(Voices.size());
    unsigned int const Draw = static_cast<unsigned int>((rand() * Count) / 32768);
    *Out = (Draw < MaleCount) ? 1 : 2;
    return true;
}

// ---------------------------------------------------------------------------------------
// FFactionNames
// ---------------------------------------------------------------------------------------

FFactionNameList::FFactionNameList()
    : Count(0)
{
}

void FFactionNameList::AddRange(INI_Reader* Reader)
{
    FFactionNameRange Range;
    Range.First = Reader->get_value_int(0);
    if (Reader->is_value_empty(1))
        Range.Last = Range.First;
    else
        Range.Last = Reader->get_value_int(1);
    Ranges.insert(Ranges.end(), 1, Range);
    Count += Range.Last - Range.First + 1;
}

// (06f4f020-06f4f04b): unsigned compares (JA / JBE).
int FFactionNameList::GetName(unsigned int Index) const
{
    std::vector<FFactionNameRange>::const_iterator Range = Ranges.begin();
    unsigned int Size = static_cast<unsigned int>(Range->Last - Range->First + 1);
    while (Size <= Index)
    {
        Index -= Size;
        ++Range;
        Size = static_cast<unsigned int>(Range->Last - Range->First + 1);
    }
    return Range->First + static_cast<int>(Index);
}

// (06f4f050-06f4f059).
bool FFactionNameList::IsEmpty() const
{
    return Count == 0;
}

// (06f50ff0-06f5115c). Group (+0x00) is not written.
FFactionNames::FFactionNames()
    : LargeShipDesig(0)
    , ShortGroupName(0)
{
    RankDesig[0] = 0;
    RankDesig[1] = 0;
    RankDesig[2] = 0;
    for (int Index = 0; Index < 8; ++Index)
        NameIds[Index] = 0;
    RankThreshold[0] = 0;
    RankThreshold[1] = 0;
}

// (06f52230-06f5269d).
bool FFactionNames::ReadIniValue(INI_Reader* Reader)
{
    if (Reader->is_value("affiliation"))
    {
        pub::Reputation::GetReputationGroup(Group, Reader->get_value_string());
        pub::Reputation::GetShortGroupName(Group, ShortGroupName);
        return false;
    }
    if (Reader->is_value("firstname"))
        return false;
    if (Reader->is_value("firstname_male"))
    {
        MaleFirstNames.AddRange(Reader);
        return true;
    }
    if (Reader->is_value("firstname_female"))
    {
        FemaleFirstNames.AddRange(Reader);
        return true;
    }
    if (Reader->is_value("lastname"))
    {
        LastNames.AddRange(Reader);
        return true;
    }
    if (Reader->is_value("formation_desig"))
    {
        FormationDesigs.AddRange(Reader);
        return true;
    }
    if (Reader->is_value("large_ship_names"))
    {
        // the same append, inlined as vector::insert(end, 1, range).
        LargeShipNames.AddRange(Reader);
        return true;
    }
    if (Reader->is_value("large_ship_desig"))
    {
        LargeShipDesig = Reader->get_value_int(0);
        return false;
    }
    if (Reader->is_value("rank_desig"))
    {
        for (int Index = 0; Index < 3; ++Index)
            RankDesig[Index] = static_cast<unsigned int>(Reader->get_value_int(Index));
        RankThreshold[0] = Reader->get_value_int(3);
        RankThreshold[1] = Reader->get_value_int(4);
    }
    return false;
}

// .
int FFactionNames::GetLargeShipDesig() const
{
    return LargeShipDesig;
}

// .
unsigned int FFactionNames::GetShortGroupName() const
{
    return ShortGroupName;
}

// .
unsigned int FFactionNames::GetNameId(int Index) const
{
    return NameIds[Index];
}

// (signed compares, JGE).
unsigned int FFactionNames::GetRankDesig(int const* Level) const
{
    if (*Level < RankThreshold[0])
        return RankDesig[0];
    if (*Level < RankThreshold[1])
        return RankDesig[1];
    return RankDesig[2];
}

// (06f51520-06f51919).
bool FFactionNames::PickPilotName(int* OutFirst, int* OutLast, int Gender, bool bReserve)
{
    FFactionNameList const* FirstNames = (Gender == 1) ? &MaleFirstNames : &FemaleFirstNames;
    int const FirstCount = FirstNames->Count;
    int const LastCount = LastNames.Count;
    int First = 0;                                                  // EBX
    int Last = 0;                                                   // EBP
    bool bResult;

    // unsigned compare of the use count with the pair count.
    if (static_cast<unsigned int>(PilotNameUses.size()) <
        static_cast<unsigned int>(LastCount * FirstCount))
    {
        if (OutFirst != nullptr && OutLast != nullptr)
        {
            // . The inner walk compares the counter before its
            // increment, so a first name whose last names are all taken ends with the
            // counter at LastCount + 1 and is accepted like a free pair.
            int Outer = 0;
            int const FirstStart = FactionRandIndex(FirstCount);
            if (FirstCount > 0)
            {
                for (;;)
                {
                    First = FirstNames->GetName(
                        static_cast<unsigned int>((FirstStart + Outer) % FirstCount));
                    int Inner = 0;
                    int const LastStart = FactionRandIndex(LastCount);
                    for (;;)
                    {
                        Last = LastNames.GetName(
                            static_cast<unsigned int>((LastStart + Inner) % LastCount));
                        if (FindPairUse(&PilotNameUses, First, Last) == PilotNameUses.end())
                            break;
                        int const Previous = Inner;
                        ++Inner;
                        if (!(Previous < LastCount))
                            break;
                    }
                    if (Inner != LastCount)
                        break;
                    ++Outer;
                    if (!(Outer < FirstCount))
                        break;
                }
            }
            bResult = Outer < FirstCount;
        }
        else if (OutFirst == nullptr && OutLast != nullptr)
        {
            // last name only, key {1, last}.
            First = 1;
            int Inner = 0;
            int const Start = FactionRandIndex(LastCount);
            for (;;)
            {
                Last = LastNames.GetName(static_cast<unsigned int>((Start + Inner) % LastCount));
                if (FindPairUse(&PilotNameUses, 1, Last) == PilotNameUses.end())
                    break;
                int const Previous = Inner;
                ++Inner;
                if (!(Previous < LastCount))
                    break;
            }
            bResult = Inner != LastCount;
        }
        else
        {
            // first name only, key {first, 1}; the draw and the modulus
            // use the last-name count.
            Last = 1;
            int Inner = 0;
            int const Start = FactionRandIndex(LastCount);
            for (;;)
            {
                First = FirstNames->GetName(
                    static_cast<unsigned int>((Start + Inner) % LastCount));
                if (FindPairUse(&PilotNameUses, First, 1) == PilotNameUses.end())
                    break;
                int const Previous = Inner;
                ++Inner;
                if (!(Previous < LastCount))
                    break;
            }
            bResult = Inner != LastCount;
        }
    }
    else
    {
        // every pair is in use.
        if (OutFirst != nullptr)
            First = FirstNames->GetName(static_cast<unsigned int>(FactionRandIndex(FirstCount)));
        else
            First = 1;
        if (OutLast != nullptr)
            Last = LastNames.GetName(static_cast<unsigned int>(FactionRandIndex(LastCount)));
        else
            First = 1;                                      // writes EBX, not EBP
        bResult = false;
    }

    // .
    if (bReserve)
        AddPairUse(&PilotNameUses, First, Last);
    if (OutFirst != nullptr)
        *OutFirst = First;
    if (OutLast != nullptr)
        *OutLast = Last;
    return bResult;
}

// (06f51920-06f51b75).
bool FFactionNames::PickFormationDesig(int* OutDesig, int* OutNumber, bool bReserve)
{
    int const Count = FormationDesigs.Count;

    if (static_cast<unsigned int>(FormationDesigUses.size()) < static_cast<unsigned int>(Count * 20))
    {
        // same walk as the pilot names, 20 numbers per designation.
        int Outer = 0;
        int const Start = FactionRandIndex(Count);
        if (Count > 0)
        {
            for (;;)
            {
                *OutDesig = FormationDesigs.GetName(static_cast<unsigned int>((Start + Outer) % Count));
                int Inner = 0;
                int const NumberStart = (rand() * 20) / 32768;
                for (;;)
                {
                    int const Number = (NumberStart + Inner) % 20;
                    *OutNumber = Number;
                    if (FindPairUse(&FormationDesigUses, *OutDesig, Number) == FormationDesigUses.end())
                        break;
                    int const Previous = Inner;
                    ++Inner;
                    if (!(Previous < 20))
                        break;
                }
                if (Inner != 20)
                    break;
                ++Outer;
                if (!(Outer < Count))
                    break;
            }
        }
        // the routine.
        if (bReserve)
            AddPairUse(&FormationDesigUses, *OutDesig, *OutNumber);
        return true;
    }

    // .
    *OutDesig = FormationDesigs.GetName(static_cast<unsigned int>(FactionRandIndex(Count)));
    int const Number = (rand() * 20) / 32768;
    *OutNumber = Number;
    if (bReserve)
        AddPairUse(&FormationDesigUses, *OutDesig, Number);
    return false;
}

// (06f51b80-06f51da2).
bool FFactionNames::PickLargeShipName(int* OutName, bool bReserve)
{
    int const Count = LargeShipNames.Count;
    int Name;

    if (static_cast<unsigned int>(LargeShipNameUses.size()) < static_cast<unsigned int>(Count))
    {
        // the first free name from a random start.
        int Draw = FactionRandIndex(Count);
        for (;;)
        {
            Name = LargeShipNames.GetName(static_cast<unsigned int>(Draw % Count));
            ++Draw;
            if (FindNameUse(&LargeShipNameUses, Name) == LargeShipNameUses.end())
                break;
        }
        if (bReserve)
            AddNameUse(&LargeShipNameUses, Name);
        *OutName = Name;
        return true;
    }

    // .
    Name = LargeShipNames.GetName(static_cast<unsigned int>(FactionRandIndex(Count)));
    if (bReserve)
        AddNameUse(&LargeShipNameUses, Name);
    *OutName = Name;
    return false;
}

// (06f51db0-06f51ed5).
void FFactionNames::AddFormationDesigUse(int Desig, int Number)
{
    AddPairUse(&FormationDesigUses, Desig, Number);
}

// (06f51ee0-06f51fd0).
void FFactionNames::ReleasePilotName(int First, int Last, bool bImmediate)
{
    if (bImmediate)
    {
        ReleasePairUse(&PilotNameUses, First, Last, nullptr);
        return;
    }
    FFactionNamePair Pending;
    Pending.First = First;
    Pending.Second = Last;
    PendingPilotNames.push_back(Pending);
    SetFactionNameTimer(GFactionNameMsgPilot, Group);
}

// (06f51fe0-06f520be).
void FFactionNames::ReleaseLargeShipName(int Name, bool bImmediate)
{
    if (bImmediate)
    {
        ReleaseNameUse(&LargeShipNameUses, Name, nullptr);
        return;
    }
    PendingLargeShipNames.push_back(Name);
    SetFactionNameTimer(GFactionNameMsgLargeShip, Group);
}

// (06f520d0-06f521c0).
void FFactionNames::ReleaseFormationDesig(int Desig, int Number, bool bImmediate)
{
    if (bImmediate)
    {
        ReleasePairUse(&FormationDesigUses, Desig, Number, nullptr);
        return;
    }
    FFactionNamePair Pending;
    Pending.First = Desig;
    Pending.Second = Number;
    PendingFormationDesigs.push_back(Pending);
    SetFactionNameTimer(GFactionNameMsgFormation, Group);
}

// (06f513c0-06f5151c).
void __cdecl HandleFactionNameTimer(pub::Controller::TimerExpired const* Timer)
{
    unsigned int const Group = Timer->Param2;
    if (Timer->Param1 == GFactionNameMsgPilot)
    {
        FFactionNames* Names = GetFactionNames(&Group);
        FFactionNamePair const Front = Names->PendingPilotNames.front();
        Names->ReleasePilotName(Front.First, Front.Second, true);
        Names->PendingPilotNames.pop_front();
    }
    else if (Timer->Param1 == GFactionNameMsgLargeShip)
    {
        FFactionNames* Names = GetFactionNames(&Group);
        int const Front = Names->PendingLargeShipNames.front();
        ReleaseNameUse(&Names->LargeShipNameUses, Front, nullptr);
        Names->PendingLargeShipNames.pop_front();
    }
    else if (Timer->Param1 == GFactionNameMsgFormation)
    {
        FFactionNames* Names = GetFactionNames(&Group);
        FFactionNamePair const Front = Names->PendingFormationDesigs.front();
        ReleasePairUse(&Names->FormationDesigUses, Front.First, Front.Second, nullptr);
        Names->PendingFormationDesigs.pop_front();
    }
}

// ---------------------------------------------------------------------------------------
// Loaders
// ---------------------------------------------------------------------------------------

// (06f4f870-06f50133).
void __cdecl LoadFactionProps(INI_Reader* Reader)
{
    // Frame locals that outlive a section: the affiliation [ESP+0x20], the good id
    // [ESP+0x3c] and the voice id [ESP+0x2c]. baseline never initialises them; the port
    // starts them at 0.
    unsigned int Group = 0;
    unsigned int Good = 0;
    unsigned int Voice = 0;

    while (Reader->read_header())
    {
        if (!Reader->is_header("FactionProps"))
            continue;

        // [ESP+0x48] props, [ESP+0x270] names.
        FFactionProps Props;
        FFactionNames Names;

        while (Reader->read_value())
        {
            if (Reader->is_value("affiliation"))
            {
                pub::Reputation::GetReputationGroup(Group, Reader->get_value_string());
                Names.ReadIniValue(Reader);                              // result ignored
            }
            else if (Reader->is_value("scan_announce"))
            {
                Props.bScanAnnounce = Reader->get_value_bool(0);
            }
            else if (Reader->is_value("formation"))
            {
                // inserted only when the key is new.
                unsigned int const Key = CreateID(Reader->get_value_string(0));
                unsigned int const Value = CreateID(Reader->get_value_string(1));
                if (Props.Formations.find(Key) == Props.Formations.end())
                    Props.Formations.insert(std::make_pair(Key, Value));
            }
            else if (Reader->is_value("scan_chance"))
            {
                Props.ScanChance = Reader->get_value_float(0);
            }
            else if (Reader->is_value("npc_ship"))
            {
                unsigned int const Arch = CreateID(Reader->get_value_string());
                Props.NpcShips.insert(Props.NpcShips.end(), 1, Arch);
            }
            else if (Reader->is_value("scan_for_cargo"))
            {
                // {archetype, level}; appended when not present.
                FFactionScanCargo Cargo;
                Cargo.Archetype = 0;
                Cargo.Level = 0;
                pub::GetGoodID(Good, Reader->get_value_string(0));
                pub::SpaceObj::GetArchetypeID(Good, Cargo.Archetype);
                int const Level = Reader->get_value_int(1);
                if (Level == 0)
                    Cargo.Level = 0;
                else if (Level == 1)
                    Cargo.Level = 1;
                else if (Level == 2)
                    Cargo.Level = 2;

                std::vector<FFactionScanCargo>::iterator It = Props.ScanForCargo.begin();
                while (It != Props.ScanForCargo.end() &&
                       !(It->Archetype == Cargo.Archetype && It->Level == Cargo.Level))
                {
                    ++It;
                }
                if (It == Props.ScanForCargo.end())
                    Props.ScanForCargo.insert(Props.ScanForCargo.end(), Cargo);
            }
            else if (Props.CommReferrable::read_comm_ini_data(*Reader))
            {
                // msg_id_prefix, a direct (non-virtual) import call.
            }
            else if (Reader->is_value("mc_costume"))
            {
                pub::GetCostumeID(Props.McCostume, Reader->get_value_string(0));
            }
            else if (Reader->is_value("space_costume"))
            {
                // head, body, both hands and the count cleared; the
                // accessory slots are not.
                Costume Appearance;
                Appearance.PartIds[0] = 0;
                Appearance.PartIds[1] = 0;
                Appearance.PartIds[2] = 0;
                Appearance.PartIds[3] = 0;
                Appearance.AccessoryCount = 0;
                ParseSpaceCostume(&Appearance, Reader);
                Props.SpaceCostumes.insert(Props.SpaceCostumes.end(), 1, Appearance);
            }
            else if (Reader->is_value("voice"))
            {
                pub::GetVoiceID(Voice, Reader->get_value_string());
                Props.Voices.insert(Props.Voices.end(), 1, Voice);
            }
            else if (Names.ReadIniValue(Reader))
            {
                // .
            }
            else if (Reader->is_value("jump_preference"))
            {
                // the value is copied into a TString<0x20> (null ->
                // empty, the routine) and compared byte-wise (REPE CMPSB, 9 bytes) with
                // "jumpgate", then "jumphole"; anything else is 0. A copy truncated to 31
                // characters can never equal either word, so comparing the value itself
                // is the same test.
                char const* Value = Reader->get_value_string(0);
                if (Value != nullptr && strcmp(Value, "jumpgate") == 0)
                    Props.JumpPreference = FactionJumpGate;
                else if (Value != nullptr && strcmp(Value, "jumphole") == 0)
                    Props.JumpPreference = FactionJumpHole;
                else
                    Props.JumpPreference = FactionJumpAny;
            }
            else if (Reader->is_value("assassinate_target"))
            {
                unsigned int const Target = CreateID(Reader->get_value_string(0));
                Props.AssassinateTargets.insert(Props.AssassinateTargets.end(), 1, Target);
            }
            else if (Reader->is_value("Legality"))
            {
                if (_stricmp(Reader->get_value_string(0), "lawful") == 0)
                    Props.Legality = FactionLegalityLawful;
                else if (_stricmp(Reader->get_value_string(0), "unlawful") == 0)
                    Props.Legality = FactionLegalityUnlawful;
            }
            else if (Reader->is_value("nickname_plurality"))
            {
                if (_stricmp(Reader->get_value_string(0), "singular") == 0)
                    Props.bSingularNickname = true;
                else if (_stricmp(Reader->get_value_string(0), "plural") == 0)
                    Props.bSingularNickname = false;
            }
        }

        // legality name ids.
        if (Props.Legality == FactionLegalityLawful || Props.Legality == FactionLegalityUnlawful)
        {
            unsigned int const LegalityId =
                (Props.Legality == FactionLegalityLawful) ? 0x4074u : 0x4075u;
            Names.NameIds[7] = LegalityId;
            Names.NameIds[3] = LegalityId;
            Names.NameIds[2] = 0x4076u;
            Names.NameIds[4] = LegalityId;
        }

        // both tables keyed by the affiliation; an existing key keeps
        // its entry (_Tree::insert).
        GFactionNames.insert(std::make_pair(Group, Names));
        GFactionProps.insert(std::make_pair(Group, Props));
    }

    // .
    FFactionGroupCollector Collector;
    pub::Reputation::EnumerateGroups(Collector);
}

// (06f503d0-06f5064d).
void __cdecl LoadFactionGoods(INI_Reader* Reader)
{
    // [ESP+0x10]: the good id outlives the section; baseline never initialises it.
    unsigned int Good = 0;

    while (Reader->read_header())
    {
        if (!Reader->is_header("FactionGood"))
            continue;

        FFactionProps* Props = nullptr;
        unsigned int Group = 0xFFFFFFFFu;
        while (Reader->read_value())
        {
            if (Reader->is_value("faction"))
            {
                pub::Reputation::GetReputationGroup(Group, Reader->get_value_string(0));
                FFactionPropsMap::iterator It = GFactionProps.find(Group);
                if (It != GFactionProps.end())
                    Props = &It->second;
                else
                    Props = nullptr;
            }
            else if (Reader->is_value("MarketGood"))
            {
                // no null check on the props (baseline faults without a
                // faction line).
                pub::GetGoodID(Good, Reader->get_value_string(0));
                Props->MarketGoods.insert(Props->MarketGoods.end(), 1, Good);
            }
        }
    }
}

// (06f50880-06f50c3a).
void __cdecl LoadFactionEmpathy(char const* RelativePath)
{
    INI_Reader Reader;
    if (!OpenContentDataIni(RelativePath, &Reader))
        return;

    // the group count reserves every section's rate list.
    FFactionGroupCounter Counter;
    pub::Reputation::EnumerateGroups(Counter);

    // Frame locals of the empathy_rate line; baseline never initialises them.
    FFactionEmpathyRate Rate;
    Rate.Group = 0;
    Rate.Rate = 0.0f;

    while (Reader.read_header())
    {
        if (!Reader.is_header("RepChangeEffects"))
            continue;

        FFactionEmpathy Empathy;
        Empathy.EventRates[0] = 0.0f;
        Empathy.EventRates[1] = 0.0f;
        Empathy.EventRates[2] = 0.0f;
        Empathy.EventRates[3] = 0.0f;
        Empathy.EmpathyRates.reserve(static_cast<unsigned int>(Counter.Count));
        unsigned int Group = 0xFFFFFFFFu;

        while (Reader.read_value())
        {
            if (Reader.is_value("empathy_rate"))
            {
                pub::Reputation::GetReputationGroup(Rate.Group, Reader.get_value_string(0));
                Rate.Rate = Reader.get_value_float(1);
                Empathy.EmpathyRates.insert(Empathy.EmpathyRates.end(), 1, Rate);
            }
            else if (Reader.is_value("group"))
            {
                pub::Reputation::GetReputationGroup(Group, Reader.get_value_string());
            }
            else if (Reader.is_value("event"))
            {
                char const* Event = Reader.get_value_string(0);
                int Index;
                if (_stricmp(Event, "object_destruction") == 0)
                    Index = FactionEventObjectDestruction;
                else if (_stricmp(Event, "random_mission_success") == 0)
                    Index = FactionEventRandomMissionSuccess;
                else if (_stricmp(Event, "random_mission_failure") == 0)
                    Index = FactionEventRandomMissionFailure;
                else if (_stricmp(Event, "random_mission_abortion") == 0)
                    Index = FactionEventRandomMissionAbortion;
                else
                    continue;
                Empathy.EventRates[Index] = Reader.get_value_float(1);
            }
        }

        // the routine sorts the rates ascending by Rate. baseline's
        // algorithm is kept because it fixes the order of equal rates, which
        // GetFactionHostileGroups exposes.
        if (!Empathy.EmpathyRates.empty())
        {
            FFactionEmpathyRate* First = &Empathy.EmpathyRates[0];
            ContentSort(First, First + Empathy.EmpathyRates.size(), &EmpathyRateLess);
        }

        GFactionEmpathy.insert(std::make_pair(Group, Empathy));
    }
}

// (06f506a0-06f50758): _Tidy of the props map (clear() or a node-wise erase
// of [begin, end), the same result).
void __cdecl UnloadFactionProps()
{
    GFactionProps.clear();
}

// (06f50ed0-06f50f88).
void __cdecl UnloadFactionEmpathy()
{
    GFactionEmpathy.clear();
}

// ---------------------------------------------------------------------------------------
// Lookups
// ---------------------------------------------------------------------------------------

// (06f50650-06f50693).
FFactionProps* __cdecl GetFactionProps(unsigned int const* Group)
{
    FFactionPropsMap::iterator It = GFactionProps.find(*Group);
    if (It == GFactionProps.end())
        return nullptr;
    return &It->second;
}

// (06f51370-06f513b3).
FFactionNames* __cdecl GetFactionNames(unsigned int const* Group)
{
    FFactionNamesMap::iterator It = GFactionNames.find(*Group);
    if (It == GFactionNames.end())
        return nullptr;
    return &It->second;
}

// .
std::vector<unsigned int>* __cdecl GetFactionGroups()
{
    return &GFactionGroups;
}

// ---------------------------------------------------------------------------------------
// Reputation empathy
// ---------------------------------------------------------------------------------------

// (06f50c50-06f50e0a).
void __cdecl ApplyFactionReputationChange(int VibeHandle, float Delta, unsigned int Group,
                                          bool bEmpathy)
{
    // (TEST AH,0x44 + JNP: equal -> out).
    if (Delta == 0.0f)
        return;
    if (Group == 0xFFFFFFFFu)
        return;

    // . The out value is zeroed once; later reads reuse it.
    float Reputation = 0.0f;
    pub::Reputation::GetReputation(VibeHandle, Group, Reputation);
    float Clamped = ClampFactionReputation(static_cast<double>(Reputation) +
                                           static_cast<double>(Delta));
    pub::Reputation::SetReputation(VibeHandle, Group, Clamped);

    if (!bEmpathy)
        return;

    // every rate of Group's empathy list.
    FFactionEmpathyMap::iterator Entry = GFactionEmpathy.find(Group);
    if (Entry == GFactionEmpathy.end())
        return;
    for (std::vector<FFactionEmpathyRate>::iterator It = Entry->second.EmpathyRates.begin();
         It != Entry->second.EmpathyRates.end(); ++It)
    {
        pub::Reputation::GetReputation(VibeHandle, It->Group, Reputation);
        // FLD Delta, FMUL rate, FADD rep.
        double const Sum = static_cast<double>(Delta) * static_cast<double>(It->Rate) +
                           static_cast<double>(Reputation);
        Clamped = ClampFactionReputation(Sum);
        pub::Reputation::SetReputation(VibeHandle, It->Group, Clamped);
    }
}

// (06f50e10-06f50e97).
void __cdecl GetFactionHostileGroups(unsigned int Group, unsigned int* Out, unsigned int Count)
{
    FFactionEmpathyMap::iterator Entry = GFactionEmpathy.find(Group);
    if (Entry == GFactionEmpathy.end())
        return;

    // the list position only moves past a negative rate, so the first
    // rate at or above zero ends the run.
    std::vector<FFactionEmpathyRate>::iterator It = Entry->second.EmpathyRates.begin();
    for (unsigned int Index = 0; Index < Count; ++Index)
    {
        if (It != Entry->second.EmpathyRates.end() && It->Rate < 0.0f)
        {
            Out[Index] = It->Group;
            ++It;
        }
        else
        {
            Out[Index] = 0xFFFFFFFFu;
        }
    }
}

// (06f50f90-06f50fe2).
float __cdecl GetFactionEventRate(unsigned int Group, int Event)
{
    FFactionEmpathyMap::iterator Entry = GFactionEmpathy.find(Group);
    if (Entry == GFactionEmpathy.end())
        return 0.0f;
    return Entry->second.EventRates[Event];
}
