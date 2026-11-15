// Author: Giperion.

// Content.dll TU25 (06ed5a70-06eeea30): GCS handler set, conversation manager and
// admission tables. Lane CNT-GCS (implementation notes, TU map).

// Listing: DLLS\BIN\Content. The std::map / std::list / std::vector instantiations
// baseline emits for these members (map find the routine / the routine, insert
// / the routine / the routine, operator[] the routine / the routine,
// erase the routine / the routine / the routine, list erase the routine, list insert
// , vector insert the routine / the routine / the routine, ctors
// / the routine / the routine) are the std containers of Gcs.h.

#include "Gcs.h"

#include "BaseDB.h"             // FindContentBaseInfo (the routine)
#include "BehaviorCmds.h"       // FContentShipBase
#include "ContentInternal.h"    // GContentNextUniqueId, GSpaceSupportControllerId, game time
#include "ContentPlayerState.h" // FindContentPlayerStateByShip (the routine)
#include "DockQueues.h"         // FGcsLaunchEvent, CollectFormationMembers (the routine)
#include "ContentSort.h"        // ContentSort (the VC6 std::sort of the routine)
#include "ContentStaticIds.h"   // TU25's 16 static ids (06ed5a70-06ed5c50)
#include "FactionProps.h"       // GetFactionProps (the routine)
#include "Guides.h"             // FContentGuide::GetTargets (slot +0x34)
#include "GuidesInternal.h"     // FTradeGuide slots +0x4c / +0x50
#include "NpcNames.h"           // FNpcNameReputation (TU42 name values)
#include "NpcShip.h"            // FContentNpcShip
#include "RandomMissionGen.h"   // FindVoiceProperties (the routine)
#include "RandomMissionOffer.h" // FRmConversationBase (the routine's argument)
#include "SystemInfo.h"         // FindSystemInfo (the routine), FSystemInfo, SendSpaceComm
#include "Voices.h"             // FindVoiceLineDuration (the routine)
#include "common_server.h"
#include "common_server_controller.h"   // pub::SetTimer, pub::Controller

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

// TU25's static initializers run in this order after the 16 ids of ContentStaticIds.h
// (06ed5a70-06ed5c50); the atexit destructors run in reverse.
namespace
{
    // the field, recent comms per category (ctor the routine, atexit 06ed5ca0:
    // guard bit 1 of the field, _Tidy with the node lists destroyed).
    std::map<int, std::list<FGcsRecentComm>> GGcsRecentComms;
    // the field, minimum interval per category (ctor the routine, atexit
    // guard bit 1 of the field, _Tidy the routine).
    std::map<int, float> GGcsCategoryIntervals;
    // the field, chance per category (ctor the routine, atexit 06ed5ed0: bit 1
    // of the field, the routine).
    std::map<int, float> GGcsCategoryChances;
    // the field, categories open to non-SpacePop listeners (ctor the routine,
    // atexit 06ed5f20: bit 2 of the field, _Tidy the routine).
    std::map<int, bool> GGcsListenerCategories;
    // the field, the same for speakers (ctor the routine, atexit 06ed6030: bit 1
    // of the field, the routine).
    std::map<int, bool> GGcsSpeakerCategories;

    // the recent-hail timer id, the field++.
    unsigned int const GGcsDockHailTimerId = GContentNextUniqueId++;

    // the field (ctor the routine, atexit the routine frees the nodes only;
    // the handlers belong to GGcsHandlers).
    std::map<int, FGcsHandler*> GGcsHandlerMap;

    // (.data bytes 00 00 00 00 00 00 28 40): 12.0; the routine sets it.
    double GGcsDefaultInterval = 12.0;
}

// TU53's static initializers (they run after TU25's in baseline).
// the routine + atexit 06f5c9a0 (the routine).
FGcsConversationManager GGcsConversations;
FGcsConversationManager* GGcsConversationManager = nullptr;
// the routine + atexit 06f5c9e0 (the routine).
FGcsHandlerSet GGcsHandlers;

// =======================================================================================
// Voice line names
// =======================================================================================

void FormatTString64(TString<0x40>* Out, char const* Format, ...)
{
    // . 06eec2bb-06eec2c6: vsprintf into the 0x40-byte local, unbounded as in
    // baseline (no caller formats more than 63 characters). 06eec2cc-06eec30d: strlen, CMP 0x40 /
    // JC else 0x3f, copy, Data[len] = 0, Length = len - TString::operator= (its null branch is
    // unreachable: Buffer is never null).
    char Buffer[0x40];
    va_list Args;
    va_start(Args, Format);
    vsprintf(Buffer, Format, Args);
    va_end(Args);
    *Out = Buffer;
}

int __cdecl PickVoiceLine(char const* Name, char const* Suffix, unsigned int VoiceId,
                          TString<0x40>* Out)
{
    // (06eeb010-06eeb100). 06eeb01a-06eeb037: "<Name>_%02d%s".
    TString<0x40> Format;
    Format.Length = 0;
    Format.Data[0] = '\0';
    FormatTString64(&Format, "%s_%%02d%%s", Name);
    int Result = -2;
    FVoiceProperties* const Voice = FindVoiceProperties(&VoiceId);
    if (Voice != nullptr)
    {
        unsigned int const NameId = pub::GetNicknameId(Name);
        int const Count = Voice->GetPermutationCount(&NameId);
        if (Count > 0)
        {
            // n as float and as double; cos(rand() * (2^-15) *
            // ((double)(float)(pi/2))) * (n - the field (1.0f)), subtracted
            // from n, + the field (0.5), floor, ftol.
            float const CountFloat = static_cast<float>(Count);
            double const CountDouble = static_cast<double>(Count);
            double const Spread = cos(static_cast<double>(rand()) *
                                      static_cast<double>(1.0f / 32768.0f) * 1.5707963705062866);
            double const Pick = floor(CountDouble -
                                      Spread * static_cast<double>(CountFloat - 1.0f) + 0.5);
            FormatTString64(Out, Format.Data, static_cast<int>(Pick), Suffix);
            return 0;
        }
        Result = -3;
    }
    Out->Length = 0;
    Out->Data[0] = '\0';
    return Result;
}

TString<0x40> __cdecl MakeVoiceLineName(char const* Name, char const* Suffix,
                                        unsigned int VoiceId)
{
    // the body of the routine (same format "%s_%%02d%%s", the routine /
    // lookups and the same x87 pick) with the result returned instead of a
    // status; both failure paths leave the zero-initialised local (06eeaf2d-06eeaf39).
    TString<0x40> Result;
    PickVoiceLine(Name, Suffix, VoiceId, &Result);
    return Result;
}

void AppendTString64(TString<0x40>* Str, char const* Tail)
{
    // length clamped below 0x40 - Length (CMP / JC, else 0x3f - Length).
    if (Tail == nullptr)
        return;
    unsigned int const Room = 0x40 - Str->Length;
    unsigned int Count = static_cast<unsigned int>(strlen(Tail));
    if (Count >= Room)
        Count = Room - 1;
    memcpy(Str->Data + Str->Length, Tail, Count);
    Str->Data[Str->Length + Count] = '\0';
    Str->Length += Count;
}

TString<0x40> MakeTString64(char const* Str)
{
    // {0, ""} first, then the clamped copy (TString::operator= semantics).
    TString<0x40> Result;
    Result.Length = 0;
    Result.Data[0] = '\0';
    Result = Str;
    return Result;
}

int __cdecl GetBaseMsgIdPrefix(unsigned int BaseId, TString<0x40>* Out)
{
    // . The null test of the prefix's chars (06eeb134) cannot fire.
    FContentBase* const Base = FindContentBaseInfo(&BaseId);
    if (Base == nullptr)
        return -4;
    *Out = Base->GetMsgIdPrefix().Data;
    return 0;
}

int __cdecl GetFactionMsgIdPrefix(unsigned int FactionId, TString<0x40>* Out)
{
    // the routine with GetFactionProps (the routine).
    FFactionProps* const Props = GetFactionProps(&FactionId);
    if (Props == nullptr)
        return -4;
    *Out = Props->GetMsgIdPrefix().Data;
    return 0;
}

TString<0x40> __cdecl MakeObjectReferenceName(int Kind, FContentSpaceObj* Object,
                                              char const* Suffix)
{
    // . Object->Slot04() (vt[+4]) is true for an NPC ship; its reputation
    // handle (the routine) is the NPC's name reputation.
    TString<0x40> Result;
    Result.Length = 0;
    Result.Data[0] = '\0';
    switch (Kind)
    {
    case 0:
    {
        // / 06eeb2f6-06eeb391: by the guide type (2 trader; 3, 4, 7
        // patrol; 5, 6 and the rest nothing), plural with formation wingmen, then the
        // legality of the ship's faction ("_leg" the field for lawful, else "_ill"
        // ). the routine copies, clamped to the room left.
        if (!Object->Slot04())
            return Result;
        char const* Name = nullptr;
        switch (static_cast<FContentNpcShip*>(Object)->GetGuideType())
        {
        case 2:
            Name = HasFormationWingmen(Object) ? "gcs_dhail_mbid_traders"
                                               : "gcs_dhail_mbid_trader";
            break;
        case 3:
        case 4:
        case 7:
            Name = HasFormationWingmen(Object) ? "gcs_dhail_mbid_patrollers"
                                               : "gcs_dhail_mbid_patrol";
            break;
        default:
            return Result;
        }
        Result = Name;
        unsigned int Affiliation = Object->ReputationHandle->GetAffiliation();
        FFactionProps* const Props = GetFactionProps(&Affiliation);
        AppendTString64(&Result, Props->Legality == FactionLegalityLawful ? "_leg" : "_ill");
        return Result;
    }
    case 1:
    {
        // .
        if (Object->Slot04())
        {
            int Value = 0;
            if (!static_cast<FNpcNameReputation*>(Object->ReputationHandle)
                     ->GetFormationDesig(&Value))
                return Result;
            // ADD EDX,0xfffcfb51 (Value - 0x304af).
            FormatTString64(&Result, "gcs_refer_formationdesig_%02d", Value - 0x304af);
            return Result;
        }
        Result = "gcs_refer_formationdesig_01";
        return Result;
    }
    case 2:
    {
        // .
        if (Object->Slot04())
        {
            int Value = 0;
            if (!static_cast<FNpcNameReputation*>(Object->ReputationHandle)
                     ->GetFormationDesigNumber(&Value))
                return Result;
            if (Suffix != nullptr)
                FormatTString64(&Result, "gcs_misc_number_%d%s", Value, Suffix);
            else
                FormatTString64(&Result, "gcs_misc_number_%d", Value);
            return Result;
        }
        // a player: (PlayerId - 1) % 20 + 1 (unsigned DIV); the state is
        // not null-tested.
        unsigned int ShipId = Object->SpaceObjId;
        FContentPlayerState* const Player = FindContentPlayerStateByShip(&ShipId);
        FormatTString64(&Result, "gcs_misc_number_%d", (Player->PlayerId - 1) % 20 + 1);
        return Result;
    }
    case 3:
    {
        // the NPC's rolled value (the call's result is not read); a
        // player gets 1 (the routine is called, its result dropped).
        if (Object->Slot04())
        {
            int Value = 0;
            static_cast<FNpcNameReputation*>(Object->ReputationHandle)->RollNameValue(&Value);
            FormatTString64(&Result, "gcs_misc_number_%d-", Value);
            return Result;
        }
        unsigned int ShipId = Object->SpaceObjId;
        FindContentPlayerStateByShip(&ShipId);
        FormatTString64(&Result, "gcs_misc_number_%d-", 1);
        return Result;
    }
    default:
        return Result;
    }
}

TString<0x40> __cdecl MakeObjectLocationName(unsigned int SpaceObjId)
{
    // .
    TString<0x40> Result;
    Result.Length = 0;
    Result.Data[0] = '\0';
    unsigned int BaseId = 0;
    pub::SpaceObj::GetDockingTarget(SpaceObjId, BaseId);
    if (BaseId == 0)
    {
        // a jump gate / hole: its target system's prefix.
        unsigned int Type = 0;
        pub::SpaceObj::GetType(SpaceObjId, Type);
        if ((Type & 0xc40) == 0)
            return Result;
        unsigned int TargetSystem = 0;
        unsigned int TargetGate = 0;
        pub::SpaceObj::GetJumpTarget(SpaceObjId, TargetSystem, TargetGate);
        pub::TranslateSystemToMsgIdPrefix(TargetSystem, Result);
    }
    else
    {
        // the the routine body inlined (no base -> empty).
        FContentBase* const Base = FindContentBaseInfo(&BaseId);
        if (Base != nullptr)
            Result = Base->GetMsgIdPrefix().Data;
    }
    // "-" appended (the the routine body inlined).
    AppendTString64(&Result, "-");
    return Result;
}

bool __cdecl HasFormationWingmen(FContentSpaceObj* Ship)
{
    // the routine into a local vector, size > 1.
    std::vector<FContentShipBase*> Members;
    CollectFormationMembers(&Members, static_cast<FContentShipBase*>(Ship)->FormationId);
    return Members.size() > 1;
}

void __cdecl MakeNumberNames(int Number, char const* Suffix, std::vector<TString<0x40>>* Out)
{
    // . 06eeb598-06eeb62b: room for three names.
    if (Out->capacity() < 3)
        Out->reserve(3);
    TString<0x40> Name;
    if (Number == 0)
    {
        // insert(end).
        Name.Length = 0;
        Name.Data[0] = '\0';
        FormatTString64(&Name, "gcs_misc_number_%d%s", 0, Suffix);
        Out->insert(Out->end(), 1, Name);
        return;
    }
    // IDIV by 10 (remainder EDX = the digit); the first (least
    // significant) digit gets Suffix appended with the the routine clamp; insert(begin).
    bool bFirst = true;
    while (Number > 0)
    {
        int const Digit = Number % 10;
        Number /= 10;
        Name.Length = 0;
        Name.Data[0] = '\0';
        FormatTString64(&Name, "gcs_misc_number_%d", Digit);
        if (bFirst)
        {
            if (Suffix != nullptr)
                AppendTString64(&Name, Suffix);
            bFirst = false;
        }
        Out->insert(Out->begin(), 1, Name);
    }
}

TString<0x40> __cdecl MakeDockApproachName(unsigned int DockType, char const* Suffix)
{
    // jump table over DockType - 1.
    TString<0x40> Result;
    Result.Length = 0;
    Result.Data[0] = '\0';
    switch (DockType)
    {
    case 1:
        FormatTString64(&Result, "gcs_dockrequest_todock%s", Suffix);
        break;
    case 3:
    case 4:
    case 5:
        FormatTString64(&Result, "gcs_dockrequest_tomoor%s", Suffix);
        break;
    case 6:
        FormatTString64(&Result, "gcs_dockrequest_toland%s", Suffix);
        break;
    default:
        break;
    }
    return Result;
}

TString<0x40> __cdecl MakeFleeReasonName(int Reason, unsigned int VoiceId)
{
    // (06eec030-06eec0bd): SUB 0 / SUB 2 / DEC over Reason; each branch is
    // (name, "-" the field, VoiceId) into the hidden result.
    switch (Reason)
    {
    case 0:
        return MakeVoiceLineName("gcs_combat_fleereason_tough", "-", VoiceId);
    case 2:
        return MakeVoiceLineName("gcs_combat_fleereason_damaged", "-", VoiceId);
    case 3:
        return MakeVoiceLineName("gcs_combat_fleereason_noweapons", "-", VoiceId);
    default:
        return MakeVoiceLineName("gcs_combat_fleereason_other", "-", VoiceId);
    }
}

bool __cdecl IsSpaceObjIdMultipleOf5(FContentSpaceObj* Object)
{
    // (06eeb900-06eeb917): +0x04 DIV 5, remainder NEG / SBB / INC -> AL.
    return Object->SpaceObjId % 5 == 0;
}

// =======================================================================================
// Lines
// =======================================================================================

FGcsLine::FGcsLine(FSystemInfo* InSystem, unsigned int InSpeaker, unsigned int InListener)
    : Speaker(InSpeaker)
    , Listener(InListener)
    , VoiceId(0)
    , CostumeId(0)
    , System(InSystem)
    , Field_0x28(0)
    , Field_0x2c(0)
{
}

FGcsLine::~FGcsLine()
{
    // vptr 06fb6254, the text buffer freed.
}

bool FGcsLine::CanPlay()
{
    // .
    return Field_0x28 != 0 && System != nullptr && System->FindShipOrSolar(Speaker) != nullptr &&
           !TextIds.empty();
}

void FGcsLine::Play()
{
    // . 06ed6a4b-06ed6a7e: the texts are copied into a 16-dword stack array
    // ([esp+0x60]..[esp+0xa0] above the 0x34-byte costume at [esp+0x2c]); baseline has no bound
    // and no line ctor adds more than eleven texts (the largest, FGcsLineScanAnnounce
    // , reserves eleven).
    unsigned int Texts[16];
    int const TextCount = static_cast<int>(TextIds.size());
    for (int Index = 0; Index < TextCount; ++Index)
        Texts[Index] = TextIds[Index];
    if (CostumeId != 0)
    {
        // head, body, both hands and the accessory count zeroed.
        Costume CommCostume;
        CommCostume.PartIds[0] = 0;
        CommCostume.PartIds[1] = 0;
        CommCostume.PartIds[2] = 0;
        CommCostume.PartIds[3] = 0;
        CommCostume.AccessoryCount = 0;
        pub::GF::GetCostumeByID(CostumeId, CommCostume);
        SendSpaceComm(System, Speaker, Listener, Texts, &TextCount, VoiceId, Field_0x2c,
                      &CommCostume, Field_0x28, 0.5f, true);
    }
    else
    {
        // .
        SendSpaceComm(System, Speaker, Listener, Texts, &TextCount, VoiceId, Field_0x2c,
                      nullptr, Field_0x28, 0.5f, true);
    }

    // outside single player the manager's delayed messages get
    // {system, speaker, listener, last text, 0, duration + the field (1.25f)}
    // (vector::insert(end, 1, entry) inlined; float store at 06ed6bb9).
    if (pub::SinglePlayer())
        return;
    FGcsDelayedMessage Entry;
    Entry.Delay = static_cast<float>(static_cast<double>(GetDuration()) + 1.25);
    Entry.Field_0x00 = System->SystemId;
    Entry.Field_0x04 = Speaker;
    Entry.Field_0x08 = Listener;
    Entry.Field_0x0c = TextIds.back();
    Entry.Elapsed = 0.0f;
    GGcsConversations.DelayedMessages.insert(GGcsConversations.DelayedMessages.end(), 1, Entry);
}

bool FGcsLine::IsDone(void const* Parms)
{
    // {+0 listener, +4 system id, +8 speaker, +0xc last text}.
    unsigned int const* Words = static_cast<unsigned int const*>(Parms);
    return System->SystemId == Words[1] && Listener == Words[0] && Speaker == Words[2] &&
           TextIds.back() == Words[3];
}

float FGcsLine::GetDuration()
{
    // . The speaker's voice when +0x0c is 0 (into a local, the field keeps 0).
    unsigned int Voice = VoiceId;
    if (Voice == 0)
        pub::SpaceObj::GetVoiceID(Speaker, Voice);
    float Sum = 0.0f;
    for (std::vector<unsigned int>::iterator It = TextIds.begin(); It != TextIds.end(); ++It)
    {
        // FCOM the field (-1.0f), TEST AH,0x44 / JNP: C3 alone (equal)
        // leaves with the field (5.0f); otherwise FADD + FSTP float.
        float const Duration = FindVoiceLineDuration(&Voice, &*It);
        if (Duration == -1.0f)
            return 5.0f;
        Sum = static_cast<float>(static_cast<double>(Duration) + static_cast<double>(Sum));
    }
    return Sum;
}

void FGcsLine::AddText(TString<0x40> Name)
{
    // vector::insert(end, 1, id) (the reallocation inlined).
    unsigned int const TextId = pub::GetNicknameId(Name.Data);
    TextIds.insert(TextIds.end(), 1, TextId);
}

bool FGcsLine::HasVoiceRoles(unsigned int Roles)
{
    pub::SpaceObj::GetVoiceID(Speaker, VoiceId);
    FVoiceProperties* const Voice = FindVoiceProperties(&VoiceId);
    if (Voice == nullptr || !Voice->SupportsRoles(Roles))
    {
        System = nullptr;
        return false;
    }
    return true;
}

void FGcsLine::PushText(char const* Name)
{
    unsigned int const TextId = pub::GetNicknameId(Name);
    TextIds.insert(TextIds.end(), 1, TextId);
}

void FGcsLine::BuildSingleVoiceLine(unsigned int Roles, char const* Name)
{
    // E.g. the routine: 06edc1fd-06edc222 the voice test, 06edc228-06edc23c rand() / 32768
    // (CDQ / AND 0x7fff / ADD / SAR 0xf), 06edc242 +0x28, 06edc24e-06edc2bb room for one,
    // the name copied (clamp 0x3f), 06edc303-06edc347 the routine with "-",
    // GetNicknameId, the routine insert(end, 1, id).
    if (!HasVoiceRoles(Roles))
        return;
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a39;
    TextIds.reserve(1);
    PushText(MakeVoiceLineName(MakeTString64(Name).Data, "-", VoiceId).Data);
}

// =======================================================================================
// Player list line (vtable 06fb623c)
// =======================================================================================

FGcsPlayerListLine::FGcsPlayerListLine(FSystemInfo* InSystem)
    : FGcsCloneableLine(InSystem, 0, 0)
{
    // (RET 4): +0x04..+0x10 = 0, +0x18..+0x20 = 0, +0x24 = the argument (its low
    // byte also lands in the allocator bytes +0x14 / +0x30), +0x28 / +0x2c = 0, +0x34..+0x3c
    // = 0, vptr 06fb623c.
}

bool FGcsPlayerListLine::CanPlay()
{
    // . 06ed6714-06ed673c: the first player id whose state (the routine) has a
    // ship object (+0x10); none -> false.
    for (std::vector<unsigned int>::iterator It = PlayerIds.begin(); It != PlayerIds.end(); ++It)
    {
        FContentPlayerState* const State = FindContentPlayerState(*It);
        if (State == nullptr || State->ShipObject == nullptr)
            continue;
        // a speaker for which ExistsAndAlive is non-zero refuses.
        if (Speaker != 0 && pub::SpaceObj::ExistsAndAlive(Speaker) != 0)
            return false;
        // SETNZ on the text count.
        return !TextIds.empty();
    }
    return false;
}

void FGcsPlayerListLine::Play()
{
    // . 06ed620b-06ed623e: the texts copied once into the same 16-dword stack
    // array as FGcsLine::Play ([esp+0x60]..[esp+0xa0], no bound).
    unsigned int Texts[16];
    int const CopyCount = static_cast<int>(TextIds.size());
    for (int Index = 0; Index < CopyCount; ++Index)
        Texts[Index] = TextIds[Index];

    // the costume is fetched once; without an id no costume is passed.
    Costume CommCostume;
    Costume const* CommCostumePtr = nullptr;
    if (CostumeId != 0)
    {
        CommCostume.PartIds[0] = 0;
        CommCostume.PartIds[1] = 0;
        CommCostume.PartIds[2] = 0;
        CommCostume.PartIds[3] = 0;
        CommCostume.AccessoryCount = 0;
        pub::GF::GetCostumeByID(CostumeId, CommCostume);
        CommCostumePtr = &CommCostume;
    }

    // every player with a ship: +0x24 = its system record (the routine),
    // then SendSpaceComm(+0x24, +0x04, its ship (the routine), texts, &count, +0x0c, +0x2c,
    // costume, +0x28, 0, false). EDI keeps the last player's state, cleared when that state
    // is missing or has no ship.
    FContentPlayerState* LastState = nullptr;
    for (std::vector<unsigned int>::iterator It = PlayerIds.begin(); It != PlayerIds.end(); ++It)
    {
        LastState = FindContentPlayerState(*It);
        if (LastState == nullptr)
            continue;
        if (LastState->ShipObject == nullptr)
        {
            LastState = nullptr;
            continue;
        }
        System = LastState->GetSystemRecord();
        int const TextCount = static_cast<int>(TextIds.size());
        SendSpaceComm(System, Speaker, LastState->GetShipId(), Texts, &TextCount, VoiceId,
                      Field_0x2c, CommCostumePtr, Field_0x28, 0.0f, false);
    }

    // outside single player, with a last player, the manager's delayed
    // messages get {system id (+0x24)->+0x48, speaker, that player's ship, last text, 0,
    // vt[+0x10]() + the field (1.25f)} (vector::insert(end, 1, entry) inlined).
    if (pub::SinglePlayer() || LastState == nullptr)
        return;
    FGcsDelayedMessage Entry;
    Entry.Delay = static_cast<float>(static_cast<double>(GetDuration()) + 1.25);
    Entry.Field_0x08 = LastState->GetShipId();
    Entry.Field_0x00 = System->SystemId;
    Entry.Field_0x04 = Speaker;
    Entry.Field_0x0c = TextIds.back();
    Entry.Elapsed = 0.0f;
    GGcsConversations.DelayedMessages.insert(GGcsConversations.DelayedMessages.end(), 1, Entry);
}

bool FGcsPlayerListLine::IsDone(void const* Parms)
{
    // (RET 4): Parms {+0 listener, +4 system id, +8 speaker, +0xc last text}.
    // The ship lookup (the routine on &word 0) is repeated for every player id.
    unsigned int const* Words = static_cast<unsigned int const*>(Parms);
    for (std::vector<unsigned int>::iterator It = PlayerIds.begin(); It != PlayerIds.end(); ++It)
    {
        FContentPlayerState* const State = FindContentPlayerStateByShip(&Words[0]);
        if (State != nullptr && State->PlayerId == *It && Speaker == Words[2] &&
            TextIds.back() == Words[3])
            return true;
    }
    return false;
}

FGcsCloneableLine* FGcsPlayerListLine::Clone()
{
    // new 0x40, the base fields and both vectors copied (capacity = size),
    // vptr 06fb6268 then 06fb623c; EH state 0 frees the block, state 1 runs the base dtor.
    return new FGcsPlayerListLine(*this);
}

namespace
{
    // The inline faction step of the line ctors (e.g. 06ed9466-06ed94a4): the ship's faction
    // prefix (the routine of GetAffiliation(ship +8)), "gcs_refer_faction_player" when the
    // faction has no record, then "_short" (the routine).
    TString<0x40> MakeShortFactionName(FContentSpaceObj* Ship)
    {
        TString<0x40> Name;
        Name.Length = 0;
        Name.Data[0] = '\0';
        if (GetFactionMsgIdPrefix(Ship->ReputationHandle->GetAffiliation(), &Name) != 0)
            Name = "gcs_refer_faction_player";
        AppendTString64(&Name, "_short");
        return Name;
    }

    // The inline singular / plural step of the guide lines (e.g. 06eda8cb-06eda9ab): the
    // speaker ship's formation group has more than one member (the the routine body
    // inlined, SETA) -> "_p" (the field), else "_s" (the field); "%s%s%s" with
    // (""); then the routine of that with Suffix.
    TString<0x40> MakeFormationVoiceLineName(FContentSpaceObj* Ship, char const* Name,
                                             char const* Suffix, unsigned int VoiceId)
    {
        bool const bPlural = HasFormationWingmen(Ship);
        TString<0x40> Format;
        Format.Length = 0;
        Format.Data[0] = '\0';
        FormatTString64(&Format, "%s%s%s", Name, bPlural ? "_p" : "_s", "");
        return MakeVoiceLineName(Format.Data, Suffix, VoiceId);
    }

    // The guide of an NPC ship (the routine; the callers only reach it for ships whose guide
    // type they tested).
    FContentGuide* GetShipGuide(FContentSpaceObj* Ship)
    {
        return static_cast<FContentNpcShip*>(Ship)->GetGuide();
    }

    // The inline system step of the jump gate lines (e.g. 06ed8a0f-06ed8a4f): the system's
    // prefix (pub::TranslateSystemToMsgIdPrefix, status ignored) + "-" (the routine into the
    // room left).
    TString<0x40> MakeSystemPrefixName(unsigned int SystemId)
    {
        TString<0x40> Name;
        Name.Length = 0;
        Name.Data[0] = '\0';
        pub::TranslateSystemToMsgIdPrefix(SystemId, Name);
        AppendTString64(&Name, "-");
        return Name;
    }
}

// ---------------------------------------------------------------------------------------
// Line classes
// ---------------------------------------------------------------------------------------

FGcsLineLaunchClearance::FGcsLineLaunchClearance(FSystemInfo* InSystem, unsigned int InSpeaker,
                                                 unsigned int InListener,
                                                 unsigned int TerminalType)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // . 06ed93a2-06ed93ca: role 0x40 (dockrequest_dock).
    if (!HasVoiceRoles(0x40))
        return;
    // the listener's ship (the routine, not null-tested); rand() / 32768
    // (CDQ / AND 0x7fff / ADD / SAR 0xf) is always 0 for the CRT's 15-bit rand.
    FContentSpaceObj* const Ship = System->FindShip(Listener);
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a41;
    TextIds.reserve(9);

    // the ship's faction prefix + "_short".
    PushText(MakeShortFactionName(Ship).Data);
    // designation, number, dash, number-, "this is".
    PushText(MakeObjectReferenceName(1, Ship, nullptr).Data);
    PushText(MakeObjectReferenceName(2, Ship, nullptr).Data);
    PushText("gcs_misc_dash");
    PushText(MakeObjectReferenceName(3, Ship, nullptr).Data);
    PushText("gcs_misc_thisis+");

    // the dock's own place (the the routine body inlined: a base with an
    // unknown record still gets the "-", neither a base nor a gate leaves the name empty and
    // its id is still added).
    PushText(MakeObjectLocationName(Speaker).Data);

    // the clearance by terminal type (1 berth; 3, 4, 5 moor; 6 ring; any
    // other leaves the name empty), its voice line (the routine with "-").
    char const* Clearance = "";
    switch (TerminalType)
    {
    case 1:
        Clearance = "gcs_docklaunch_clear_berth";
        break;
    case 3:
    case 4:
    case 5:
        Clearance = "gcs_docklaunch_clear_moor";
        break;
    case 6:
        Clearance = "gcs_docklaunch_clear_ring";
        break;
    default:
        break;
    }
    PushText(MakeVoiceLineName(Clearance, "-", VoiceId).Data);

    // .
    AddText(MakeVoiceLineName(MakeTString64("gcs_misc_wellwish").Data, "-", VoiceId));
}

FGcsLineAck::FGcsLineAck(FSystemInfo* InSystem, unsigned int InSpeaker,
                         unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // role 0x200 (misc_chatter).
    if (!HasVoiceRoles(0x200))
        return;
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a39;
    TextIds.reserve(1);
    // the routine insert(end, 1, id).
    PushText(MakeVoiceLineName("gcs_misc_ack", "-", VoiceId).Data);
}

FGcsLineDockRequest::FGcsLineDockRequest(FSystemInfo* InSystem, unsigned int InSpeaker,
                                         unsigned int InListener, unsigned int DockType)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // role 0x80 (dockrequest_docker).
    if (!HasVoiceRoles(0x80))
        return;
    // The speaker's own ship (not null-tested); the dock's lookup (the routine) is unused.
    FContentSpaceObj* const Ship = System->FindShip(Speaker);
    System->FindShipOrSolar(Listener);
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a42;
    TextIds.reserve(10);

    PushText("gcs_misc_thisis+");
    PushText(MakeShortFactionName(Ship).Data);
    PushText(MakeObjectReferenceName(1, Ship, nullptr).Data);
    PushText(MakeObjectReferenceName(2, Ship, nullptr).Data);
    PushText("gcs_misc_dash");
    // kind 2 again (not 3).
    PushText(MakeObjectReferenceName(2, Ship, nullptr).Data);
    PushText("gcs_misc_to");
    // the dock's place (the the routine body inlined).
    PushText(MakeObjectLocationName(Listener).Data);
    // "gcs_dockrequest_request" with "+" (the field).
    PushText(MakeVoiceLineName("gcs_dockrequest_request", "+", VoiceId).Data);
    // the approach with "-" through AddText (empty for another type).
    AddText(MakeDockApproachName(DockType, "-"));
}

FGcsLineDockHostile::FGcsLineDockHostile(FSystemInfo* InSystem, unsigned int InSpeaker,
                                         unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06ed72e0-06ed74c3): role 0x40.
    if (!HasVoiceRoles(0x40))
        return;
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a39;
    TextIds.reserve(1);
    // the name copied into a local TString (clamp 0x3f) first.
    PushText(MakeVoiceLineName("gcs_dockrequest_responsehostile", "-", VoiceId).Data);
}

FGcsLineDockGranted::FGcsLineDockGranted(FSystemInfo* InSystem, unsigned int InSpeaker,
                                         unsigned int InListener, unsigned int DockType,
                                         int DockIndex)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // role 0x40; the ship's lookup (the routine) is unused. No rand() test.
    if (!HasVoiceRoles(0x40))
        return;
    System->FindShip(Listener);
    if (DockType == 6)
    {
        // landing.
        Field_0x28 = 0x4a3e;
        TextIds.reserve(6);
        PushText(MakeVoiceLineName("gcs_misc_ack", "-", VoiceId).Data);
        PushText("gcs_dockrequest_yourrequest+");
        PushText("gcs_dockrequest_toland");
        PushText(MakeVoiceLineName("gcs_dockrequest_granted", "-", VoiceId).Data);
        PushText(MakeVoiceLineName("gcs_dockrequest_proceed", "+", VoiceId).Data);
        PushText("gcs_dockrequest_toland-");
        return;
    }
    if (DockIndex <= 0x14)
    {
        // a terminal up to 20, its number (DockIndex + 1) at the end.
        Field_0x28 = 0x4a3f;
        TextIds.reserve(7);
        PushText(MakeVoiceLineName("gcs_misc_ack", "-", VoiceId).Data);
        PushText("gcs_dockrequest_yourrequest+");
        PushText(MakeDockApproachName(DockType, "").Data);          // ""
        PushText(MakeVoiceLineName("gcs_dockrequest_granted", "-", VoiceId).Data);
        PushText(MakeVoiceLineName("gcs_dockrequest_proceed", "+", VoiceId).Data);
        PushText(MakeDockApproachName(DockType, "_number").Data);
        TString<0x40> Number;
        Number.Length = 0;
        Number.Data[0] = '\0';
        FormatTString64(&Number, "gcs_misc_number_%d%s", DockIndex + 1, "-");
        PushText(Number.Data);
        return;
    }
    // a terminal above 20, its number digit by digit (two digits).
    Field_0x28 = 0x4a40;
    TextIds.reserve(8);
    PushText(MakeVoiceLineName("gcs_misc_ack", "-", VoiceId).Data);
    PushText("gcs_dockrequest_yourrequest+");
    PushText(MakeDockApproachName(DockType, "").Data);
    AddText(MakeVoiceLineName(MakeTString64("gcs_dockrequest_granted").Data, "-", VoiceId));
    AddText(MakeVoiceLineName(MakeTString64("gcs_dockrequest_proceed").Data, "+", VoiceId));
    AddText(MakeDockApproachName(DockType, "_number"));
    // a local vector (the routine / the routine); its first two names
    // are read without a size test (DockIndex + 1 > 21 has two digits or more).
    std::vector<TString<0x40>> Digits;
    MakeNumberNames(DockIndex + 1, "-", &Digits);
    AddText(Digits[0]);
    AddText(Digits[1]);
}

FGcsLineDockStandby::FGcsLineDockStandby(FSystemInfo* InSystem, unsigned int InSpeaker,
                                         unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // role 0x40.
    if (!HasVoiceRoles(0x40))
        return;
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3c;
    TextIds.reserve(4);
    PushText(MakeVoiceLineName("gcs_misc_ack", "-", VoiceId).Data);
    PushText(MakeVoiceLineName("gcs_dockrequest_standby", "-", VoiceId).Data);
    PushText(MakeVoiceLineName("gcs_dockrequest_delayedreason", "-", VoiceId).Data);
    PushText(MakeVoiceLineName("gcs_dockrequest_willbecleared", "-", VoiceId).Data);
}

FGcsLineDockDenied::FGcsLineDockDenied(FSystemInfo* InSystem, unsigned int InSpeaker,
                                       unsigned int InListener, unsigned int DockType)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // role 0x40.
    if (!HasVoiceRoles(0x40))
        return;
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3d;
    TextIds.reserve(5);
    PushText(MakeVoiceLineName("gcs_misc_ack", "-", VoiceId).Data);
    PushText("gcs_dockrequest_yourrequest+");
    PushText(MakeDockApproachName(DockType, "").Data);              // ""
    PushText(MakeVoiceLineName("gcs_dockrequest_denied", "-", VoiceId).Data);
    PushText(MakeVoiceLineName("gcs_dockrequest_nofit", "-", VoiceId).Data);
}

FGcsLineDockNowCleared::FGcsLineDockNowCleared(FSystemInfo* InSystem, unsigned int InSpeaker,
                                               unsigned int InListener, unsigned int DockType)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // role 0x40; the ship (the routine, not null-tested).
    if (!HasVoiceRoles(0x40))
        return;
    FContentSpaceObj* const Ship = System->FindShip(Listener);
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3f;
    TextIds.reserve(7);
    PushText(MakeShortFactionName(Ship).Data);
    PushText(MakeObjectReferenceName(1, Ship, nullptr).Data);
    PushText(MakeObjectReferenceName(2, Ship, nullptr).Data);
    PushText("gcs_misc_dash");
    PushText(MakeObjectReferenceName(3, Ship, nullptr).Data);
    PushText(MakeVoiceLineName("gcs_dockrequest_nowcleared", "+", VoiceId).Data);
    AddText(MakeDockApproachName(DockType, "-"));
}

FGcsLineJumpGateOpen::FGcsLineJumpGateOpen(FSystemInfo* InSystem, unsigned int InSpeaker,
                                           unsigned int InListener, unsigned int TargetSystem)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // role 0x80.
    if (!HasVoiceRoles(0x80))
        return;
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3b;
    TextIds.reserve(3);
    PushText(MakeVoiceLineName("gcs_dockrequest_jumpgateopen", "-", VoiceId).Data);
    PushText(MakeVoiceLineName("gcs_dockrequest_goingto", "+", VoiceId).Data);
    PushText(MakeSystemPrefixName(TargetSystem).Data);
}

FGcsLineJumpGateDelayed::FGcsLineJumpGateDelayed(FSystemInfo* InSystem, unsigned int InSpeaker,
                                                 unsigned int InListener,
                                                 unsigned int TargetSystem)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // role 0x80.
    if (!HasVoiceRoles(0x80))
        return;
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3a;
    TextIds.reserve(2);
    PushText(MakeVoiceLineName("gcs_dockrequest_jumpgatedelayed", "-", VoiceId).Data);
    PushText(MakeSystemPrefixName(TargetSystem).Data);
}

FGcsLineJumpGateOpenAfterDelay::FGcsLineJumpGateOpenAfterDelay(FSystemInfo* InSystem,
                                                               unsigned int InSpeaker,
                                                               unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06ed8d20-06ed8f0b): the listing of the routine but for the vtable, role
    // 0x80 and the name.
    BuildSingleVoiceLine(0x80, "gcs_dockrequest_jumpgateopenafterdelay");
}

FGcsLineTradeLaneStart::FGcsLineTradeLaneStart(FSystemInfo* InSystem, unsigned int InSpeaker,
                                               unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06ed8f10-06ed90fb): the listing of the routine but for the vtable, role
    // 0x80 and the name.
    BuildSingleVoiceLine(0x80, "gcs_dockrequest_tradelanestart");
}

FGcsLineThisOurStop::FGcsLineThisOurStop(FSystemInfo* InSystem, unsigned int InSpeaker,
                                         unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // role 0x80.
    if (!HasVoiceRoles(0x80))
        return;
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3a;
    TextIds.reserve(2);
    PushText(MakeVoiceLineName("gcs_dockrequest_thisourstop", "-", VoiceId).Data);
    PushText(MakeVoiceLineName("gcs_dockrequest_thankshelp", "-", VoiceId).Data);
}

FGcsLineDockHail::FGcsLineDockHail(FSystemInfo* InSystem, unsigned int InSpeaker,
                                   unsigned int InListener, float Attitude)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // role 0x20 (hail_dock); the ship (the routine, not null-tested).
    if (!HasVoiceRoles(0x20))
        return;
    FContentShipBase* const Ship = static_cast<FContentShipBase*>(System->FindShip(Listener));

    // FCOMP the field (-0.6f), TEST AH,0x41 / JNZ: C0 or C3 (at or
    // below, or unordered) refuses; no rand() test on that side.
    if (!(Attitude > -0.6f))
    {
        // .
        Field_0x28 = 0x4a3c;
        TextIds.reserve(4);
        PushText("gcs_dockhail_dock_incoming+");
        TString<0x40> ShipPrefix;
        ShipPrefix.Length = 0;
        ShipPrefix.Data[0] = '\0';
        pub::TranslateShipToMsgIdPrefix(Ship->ShipArchId, ShipPrefix);
        AddText(ShipPrefix);
        AddText(MakeObjectReferenceName(0, Ship, nullptr));
        AddText(MakeVoiceLineName(MakeTString64("gcs_dhail_nopermission").Data, "-", VoiceId));
        return;
    }

    // (rand() * 2) / 32768 (SHL 1, CDQ, AND 0x7fff, ADD, SAR 0xf).
    int const Pick = (rand() * 2) / 32768;
    if (Pick == 1)
    {
        // .
        Field_0x28 = 0x4a3f;
        TextIds.reserve(7);
        PushText("gcs_dockhail_dock_incoming+");
        TString<0x40> ShipPrefix;
        ShipPrefix.Length = 0;
        ShipPrefix.Data[0] = '\0';
        pub::TranslateShipToMsgIdPrefix(Ship->ShipArchId, ShipPrefix);
        PushText(ShipPrefix.Data);
        PushText(MakeObjectReferenceName(0, Ship, nullptr).Data);
        PushText("gcs_misc_thisis+");
        PushText(MakeObjectLocationName(Speaker).Data);             // inlined
        PushText(MakeVoiceLineName("gcs_dockhail_dock_hailmethod", "-", VoiceId).Data);
        PushText(MakeVoiceLineName("gcs_dockhail_dock_idreq", "-", VoiceId).Data);
        return;
    }
    if (Pick != 0)
        return;
    // .
    Field_0x28 = 0x4a3e;
    TextIds.reserve(6);
    PushText("gcs_misc_thisis+");
    PushText(MakeObjectLocationName(Speaker).Data);                 // inlined
    PushText("gcs_misc_to");
    TString<0x40> ShipPrefix;
    ShipPrefix.Length = 0;
    ShipPrefix.Data[0] = '\0';
    pub::TranslateShipToMsgIdPrefix(Ship->ShipArchId, ShipPrefix);
    PushText(ShipPrefix.Data);
    PushText(MakeObjectReferenceName(0, Ship, nullptr).Data);
    PushText(MakeVoiceLineName("gcs_dockhail_dock_idreq", "-", VoiceId).Data);
}

FGcsLineDockHailReceived::FGcsLineDockHailReceived(FSystemInfo* InSystem, unsigned int InSpeaker,
                                                   unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // role 0x20; the ship (not null-tested); no rand() test.
    if (!HasVoiceRoles(0x20))
        return;
    FContentSpaceObj* const Ship = System->FindShip(Listener);
    Field_0x28 = 0x4a3d;
    TextIds.reserve(5);
    PushText(MakeVoiceLineName("gcs_dockhail_dock_transreceived", "+", VoiceId).Data);
    PushText(MakeShortFactionName(Ship).Data);
    PushText(MakeObjectReferenceName(1, Ship, nullptr).Data);
    PushText(MakeObjectReferenceName(2, Ship, "-").Data);
    PushText(MakeVoiceLineName("gcs_dockhail_dock_whereheaded", "+", VoiceId).Data);
}

FGcsLineHaileeDataSubmit::FGcsLineHaileeDataSubmit(FSystemInfo* InSystem, unsigned int InSpeaker,
                                                   unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // role 0x10 (hail_hailee); the speaker's own ship.
    if (!HasVoiceRoles(0x10))
        return;
    FContentSpaceObj* const Ship = System->FindShip(Speaker);
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3d;
    TextIds.reserve(5);
    PushText("gcs_misc_thisis+");
    // GetFactionProps (the routine) directly: null ->
    // "gcs_refer_faction_player", else its prefix; + "_short" (the the routine outcome).
    PushText(MakeShortFactionName(Ship).Data);
    PushText(MakeObjectReferenceName(1, Ship, nullptr).Data);
    PushText(MakeObjectReferenceName(2, Ship, "-").Data);
    PushText(MakeVoiceLineName("gcs_dockhail_hailee_datasubmit", "-", VoiceId).Data);
}

FGcsLineHaileeAngry::FGcsLineHaileeAngry(FSystemInfo* InSystem, unsigned int InSpeaker,
                                         unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06eda5a0-06eda783): the listing of the routine but for the vtable, role
    // 0x10 and the name.
    BuildSingleVoiceLine(0x10, "gcs_dhail_angry_response");
}

FGcsLineGoingTo::FGcsLineGoingTo(FSystemInfo* InSystem, unsigned int InSpeaker,
                                 unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // role 0x10; the speaker's ship.
    if (!HasVoiceRoles(0x10))
        return;
    FContentSpaceObj* const Ship = System->FindShip(Speaker);
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3a;
    TextIds.reserve(2);
    PushText(MakeFormationVoiceLineName(Ship, "gcs_misc_goingto", "+", VoiceId).Data);
    // the guide's second target (slot +0x34), its place.
    unsigned int TargetId1 = 0;
    unsigned int TargetId2 = 0;
    FContentBase* TargetBase1 = nullptr;
    FContentBase* TargetBase2 = nullptr;
    GetShipGuide(Ship)->GetTargets(&TargetId1, &TargetId2, &TargetBase1, &TargetBase2);
    PushText(MakeObjectLocationName(TargetId2).Data);
}

FGcsLinePatrolIdHome::FGcsLinePatrolIdHome(FSystemInfo* InSystem, unsigned int InSpeaker,
                                           unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // role 0x10; the speaker's ship.
    if (!HasVoiceRoles(0x10))
        return;
    FContentSpaceObj* const Ship = System->FindShip(Speaker);
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3a;
    TextIds.reserve(2);
    PushText(MakeFormationVoiceLineName(Ship, "gcs_patrol_idhome", "+", VoiceId).Data);
    // The guide's first target (slot +0x34), its place.
    unsigned int TargetId1 = 0;
    unsigned int TargetId2 = 0;
    FContentBase* TargetBase1 = nullptr;
    FContentBase* TargetBase2 = nullptr;
    GetShipGuide(Ship)->GetTargets(&TargetId1, &TargetId2, &TargetBase1, &TargetBase2);
    PushText(MakeObjectLocationName(TargetId1).Data);
}

FGcsLinePatrolStatus::FGcsLinePatrolStatus(FSystemInfo* InSystem, unsigned int InSpeaker,
                                           unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06edae70-06edb253). 06edaee8-06edaf0a: role 0x10.
    if (!HasVoiceRoles(0x10))
        return;
    // the speaker's own ship (the routine, not null-tested), its patrol
    // guide (the routine; the callers only get here for guide type 3), slot +0x4c
    // (the branch: the route's points to go). No rand() test in either branch.
    FContentSpaceObj* const Ship = System->FindShip(Speaker);
    int const PointsToGo = static_cast<FPatrolGuide*>(GetShipGuide(Ship))->GetPathPointsToGo();
    if (PointsToGo == 0)
    {
        // +0x28 = 0x4a39, room for one; "gcs_patrol_finished" (the routine
        // copy) + "_p" when the formation has more than one member (the routine into a local,
        // SETA), else "_s", "%s%s%s" with "", then the routine with "-" and the routine.
        Field_0x28 = 0x4a39;
        TextIds.reserve(1);
        PushText(MakeFormationVoiceLineName(Ship, "gcs_patrol_finished", "-", VoiceId).Data);
        return;
    }
    // +0x28 = 0x4a3b, room for three.
    Field_0x28 = 0x4a3b;
    TextIds.reserve(3);
    // the formation (the routine) with more than one member speaks as
    // "we", else as "I"; "only" for a single point.
    char const* Have = nullptr;
    if (HasFormationWingmen(Ship))
        Have = PointsToGo == 1 ? "gcs_patrol_wehaveonly+" : "gcs_patrol_wehave+";
    else
        Have = PointsToGo == 1 ? "gcs_patrol_ihaveonly+" : "gcs_patrol_ihave+";
    PushText(Have);
    // "gcs_misc_number_%d%s" with "" (the routine).
    TString<0x40> Number;
    Number.Length = 0;
    Number.Data[0] = '\0';
    FormatTString64(&Number, "gcs_misc_number_%d%s", PointsToGo, "");
    PushText(Number.Data);
    // .
    PushText(PointsToGo == 1 ? "gcs_patrol_morewptogo-" : "gcs_patrol_morewpstogo-");
}

FGcsLineTradeStatus::FGcsLineTradeStatus(FSystemInfo* InSystem, unsigned int InSpeaker,
                                         unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // role 0x10; the speaker's ship.
    if (!HasVoiceRoles(0x10))
        return;
    FContentSpaceObj* const Ship = System->FindShip(Speaker);
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3c;
    TextIds.reserve(4);

    // the trade guide's slot +0x4c (its trade good) ...
    FTradeGuide* const Guide = static_cast<FTradeGuide*>(GetShipGuide(Ship));
    unsigned int const TradeGood = Guide->GetTradeGood();
    char const* Preposition = nullptr;
    TString<0x40> GoodPrefix;
    if (TradeGood != 0)
    {
        PushText(MakeFormationVoiceLineName(Ship, "gcs_trade_carrying", "+", VoiceId).Data);
        GoodPrefix.Length = 0;
        GoodPrefix.Data[0] = '\0';
        pub::TranslateGoodToMsgIdPrefix(TradeGood, GoodPrefix);
        PushText(GoodPrefix.Data);
        Preposition = "gcs_misc_to";
    }
    else
    {
        // ... else slot +0x50 (its return good); none -> System = null
        // (06edb6f1).
        unsigned int const ReturnGood = Guide->GetReturnGood();
        if (ReturnGood == 0)
        {
            System = nullptr;
            return;
        }
        PushText(MakeFormationVoiceLineName(Ship, "gcs_trade_wanttoget", "+", VoiceId).Data);
        GoodPrefix.Length = 0;
        GoodPrefix.Data[0] = '\0';
        pub::TranslateGoodToMsgIdPrefix(ReturnGood, GoodPrefix);
        PushText(GoodPrefix.Data);
        Preposition = "gcs_misc_from";
    }
    PushText(Preposition);

    // the guide's second target (slot +0x34); the routine + the routine.
    unsigned int TargetId1 = 0;
    unsigned int TargetId2 = 0;
    FContentBase* TargetBase1 = nullptr;
    FContentBase* TargetBase2 = nullptr;
    GetShipGuide(Ship)->GetTargets(&TargetId1, &TargetId2, &TargetBase1, &TargetBase2);
    AddText(MakeObjectLocationName(TargetId2));
}

FGcsLineTradeIdHome::FGcsLineTradeIdHome(FSystemInfo* InSystem, unsigned int InSpeaker,
                                         unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // role 0x10; the speaker's ship.
    if (!HasVoiceRoles(0x10))
        return;
    FContentSpaceObj* const Ship = System->FindShip(Speaker);
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3a;
    TextIds.reserve(2);
    PushText(MakeFormationVoiceLineName(Ship, "gcs_trade_idhome", "+", VoiceId).Data);
    // The guide's first target (slot +0x34), its place.
    unsigned int TargetId1 = 0;
    unsigned int TargetId2 = 0;
    FContentBase* TargetBase1 = nullptr;
    FContentBase* TargetBase2 = nullptr;
    GetShipGuide(Ship)->GetTargets(&TargetId1, &TargetId2, &TargetBase1, &TargetBase2);
    PushText(MakeObjectLocationName(TargetId1).Data);
}

FGcsLinePlayerHailBlowOff::FGcsLinePlayerHailBlowOff(FSystemInfo* InSystem,
                                                     unsigned int InSpeaker,
                                                     unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06ee11c0-06ee13a3). 06ee123d-06ee125f: role 0x10.
    if (!HasVoiceRoles(0x10))
        return;
    // .
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a39;
    TextIds.reserve(1);
    // the name copied into a local TString (clamp 0x3f); 06ee1338-06ee137f:
    // with "-" (the field), GetNicknameId, the routine insert(end, 1, id).
    PushText(MakeVoiceLineName(MakeTString64("gcs_phail_blowoff").Data, "-", VoiceId).Data);
}

FGcsLinePlayerHailCombatBlowOff::FGcsLinePlayerHailCombatBlowOff(FSystemInfo* InSystem,
                                                                 unsigned int InSpeaker,
                                                                 unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06ee13b0-06ee1593): the routine with its own name.
    if (!HasVoiceRoles(0x10))
        return;
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a39;
    TextIds.reserve(1);
    // .
    PushText(MakeVoiceLineName(MakeTString64("gcs_phail_combatblowoff").Data, "-", VoiceId).Data);
}

FGcsLinePlayerHailIdentify::FGcsLinePlayerHailIdentify(FSystemInfo* InSystem,
                                                       unsigned int InSpeaker,
                                                       unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06ee15a0-06ee182a). 06ee161d-06ee163f: role 0x10.
    if (!HasVoiceRoles(0x10))
        return;
    // the speaker's own ship (the routine, not null-tested).
    FContentSpaceObj* const Ship = System->FindShip(Speaker);
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3c;
    TextIds.reserve(4);
    // .
    PushText("gcs_misc_thisis+");
    // GetFactionProps (the routine) of the handle's affiliation
    // (the routine) directly: null -> "gcs_refer_faction_player", else its prefix
    // (CommReferrable slot 1, the routine); + "_short" (the routine) -- the
    // MakeShortFactionName outcome.
    PushText(MakeShortFactionName(Ship).Data);
    // the routine(1, ship, null).
    PushText(MakeObjectReferenceName(1, Ship, nullptr).Data);
    // the routine(2, ship, "-").
    PushText(MakeObjectReferenceName(2, Ship, "-").Data);
}

// ---------------------------------------------------------------------------------------
// Combat lines (FGcsCombatConversation)
// ---------------------------------------------------------------------------------------

namespace
{
    // The voice test of the routine / the routine (e.g. 06edbfdd-06edc052): GetVoiceID into
    // +0x0c, the voice's record, the speaker's type (its status is not tested; baseline leaves
    // the local uninitialised on failure, the port 0), then role 0x4000 for a ship (type &
    // 0x7f0000), 0x800 for anything else. Without the record or the role System = null.
    bool HasCombatVoiceRoles(FGcsLine* Line)
    {
        pub::SpaceObj::GetVoiceID(Line->Speaker, Line->VoiceId);
        FVoiceProperties* const Voice = FindVoiceProperties(&Line->VoiceId);
        unsigned int Type = 0;
        pub::SpaceObj::GetType(Line->Speaker, Type);
        unsigned int const Roles = (Type & 0x7f0000) != 0 ? 0x4000 : 0x800;
        if (Voice == nullptr || !Voice->SupportsRoles(Roles))
        {
            Line->System = nullptr;
            return false;
        }
        return true;
    }
}

FGcsLineCombatDeathSympathy::FGcsLineCombatDeathSympathy(FSystemInfo* InSystem,
                                                         unsigned int InSpeaker,
                                                         unsigned int Unused)
    : FGcsLine(InSystem, InSpeaker, 0)
{
    // (06edba90-06edbc6f): 06edbac2-06edbacb +0x08 = 0 (the third argument is
    // never read); then the one-name body with role 0x4000.
    (void)Unused;
    BuildSingleVoiceLine(0x4000, "gcs_combat_deathsympathy");
}

FGcsLineCombatTakingLead::FGcsLineCombatTakingLead(FSystemInfo* InSystem, unsigned int InSpeaker,
                                                   unsigned int Unused)
    : FGcsLine(InSystem, InSpeaker, 0)
{
    // (06edbc80-06edbf52). 06edbcb6-06edbcbb `XOR EDI,EDI ; MOV [EBX+0x8],EDI`:
    // +0x08 = 0, the third argument is never read. 06edbcf6-06edbd18: role 0x4.
    (void)Unused;
    if (!HasVoiceRoles(0x4))
        return;
    // the speaker's own ship (the routine, not null-tested).
    FContentSpaceObj* const Ship = System->FindShip(Speaker);
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3e;
    TextIds.reserve(6);
    // (the routine / the routine appends).
    PushText("gcs_misc_thisis+");
    PushText(MakeObjectReferenceName(1, Ship, nullptr).Data);
    PushText(MakeObjectReferenceName(2, Ship, nullptr).Data);
    PushText("gcs_misc_dash");
    PushText(MakeObjectReferenceName(3, Ship, nullptr).Data);
    // the name copied (the routine), the routine with "-".
    PushText(MakeVoiceLineName(MakeTString64("gcs_combat_takinglead").Data, "-", VoiceId).Data);
}

FGcsLineCombatScream::FGcsLineCombatScream(FSystemInfo* InSystem, unsigned int InSpeaker,
                                           unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06edbf60-06edc16f). 06edbfdd-06edc052: the role by the speaker's type.
    if (!HasCombatVoiceRoles(this))
        return;
    // the one-name body.
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a39;
    TextIds.reserve(1);
    PushText(MakeVoiceLineName(MakeTString64("gcs_combat_scream").Data, "-", VoiceId).Data);
}

FGcsLineCombatBrag::FGcsLineCombatBrag(FSystemInfo* InSystem, unsigned int InSpeaker,
                                       unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06edc180-06edc36b).
    BuildSingleVoiceLine(0x4000, "gcs_combat_brag");
}

FGcsLineCombatTakingDamage::FGcsLineCombatTakingDamage(FSystemInfo* InSystem,
                                                       unsigned int InSpeaker,
                                                       unsigned int InListener, float Health)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06edc370-06edc5c2). 06edc3ed-06edc462: the role by the speaker's type.
    if (!HasCombatVoiceRoles(this))
        return;
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a39;
    TextIds.reserve(1);
    // FLD Health, FCOMP double the field (0.49), TEST AH,5 / JP: C0
    // alone (below) -> "_worst"; the name copied with the routine (room 0x40).
    char const* const Name = static_cast<double>(Health) < 0.49
                                 ? "gcs_combat_taking_damage_worst"
                                 : "gcs_combat_taking_damage";
    PushText(MakeVoiceLineName(MakeTString64(Name).Data, "-", VoiceId).Data);
}

FGcsLineCombatInflictingDamage::FGcsLineCombatInflictingDamage(FSystemInfo* InSystem,
                                                               unsigned int InSpeaker,
                                                               unsigned int InListener,
                                                               float Health)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06edc5d0-06edc7e1): role 0x4000.
    if (!HasVoiceRoles(0x4000))
        return;
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a39;
    TextIds.reserve(1);
    // FCOMP double the field (0.33), TEST AH,5 / JP: C0 alone (below)
    // -> "_worst".
    char const* const Name = static_cast<double>(Health) < 0.33
                                 ? "gcs_combat_inflicting_damage_worst"
                                 : "gcs_combat_inflicting_damage";
    PushText(MakeVoiceLineName(MakeTString64(Name).Data, "-", VoiceId).Data);
}

FGcsLineCombatInSights::FGcsLineCombatInSights(FSystemInfo* InSystem, unsigned int InSpeaker,
                                               unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06edc800-06edc9eb).
    BuildSingleVoiceLine(0x4000, "gcs_combat_insights");
}

FGcsLineCombatHasInSights::FGcsLineCombatHasInSights(FSystemInfo* InSystem,
                                                     unsigned int InSpeaker,
                                                     unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06edc9f0-06edcbdb).
    BuildSingleVoiceLine(0x4000, "gcs_combat_hasinsights");
}

FGcsLineCombatRejoiningForm::FGcsLineCombatRejoiningForm(FSystemInfo* InSystem,
                                                         unsigned int InSpeaker,
                                                         unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06edcbe0-06edcdcb).
    BuildSingleVoiceLine(0x4000, "gcs_combat_rejoiningform");
}

FGcsLineCombatOrderDisengage::FGcsLineCombatOrderDisengage(FSystemInfo* InSystem,
                                                           unsigned int InSpeaker,
                                                           unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06edcdd0-06edcff3): role 0x4000.
    if (!HasVoiceRoles(0x4000))
        return;
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3a;
    TextIds.reserve(2);
    // / 06edcf5e-06edcfcd: each name copied (the routine), with "-".
    PushText(MakeVoiceLineName(MakeTString64("gcs_combat_order_disengage").Data, "-",
                               VoiceId).Data);
    PushText(MakeVoiceLineName(MakeTString64("gcs_combat_announce_allclear").Data, "-",
                               VoiceId).Data);
}

FGcsLineCombatAllClear::FGcsLineCombatAllClear(FSystemInfo* InSystem, unsigned int InSpeaker,
                                               unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06edd000-06edd1eb).
    BuildSingleVoiceLine(0x4000, "gcs_combat_announce_allclear");
}

FGcsLineCombatResumeTrade::FGcsLineCombatResumeTrade(FSystemInfo* InSystem,
                                                     unsigned int InSpeaker,
                                                     unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06edd1f0-06edd4f9). 06edd26d-06edd292: role 0xc004.
    if (!HasVoiceRoles(0xc004))
        return;
    // the speaker's own ship (not null-tested).
    FContentSpaceObj* const Ship = System->FindShip(Speaker);
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3b;
    TextIds.reserve(3);
    // the name copied (the routine) with "+" (the field).
    PushText(MakeVoiceLineName(MakeTString64("gcs_combat_resume_trade").Data, "+",
                               VoiceId).Data);
    PushText("gcs_misc_to");
    // the guide's slot +0x34; 06edd3f0 reads the first out-parameter.
    // its place (the the routine body inlined).
    unsigned int TargetId1 = 0;
    unsigned int TargetId2 = 0;
    FContentBase* TargetBase1 = nullptr;
    FContentBase* TargetBase2 = nullptr;
    GetShipGuide(Ship)->GetTargets(&TargetId1, &TargetId2, &TargetBase1, &TargetBase2);
    PushText(MakeObjectLocationName(TargetId1).Data);
}

FGcsLineCombatResumePatrol::FGcsLineCombatResumePatrol(FSystemInfo* InSystem,
                                                       unsigned int InSpeaker,
                                                       unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06edd500-06edd6e3).
    BuildSingleVoiceLine(0x4, "gcs_combat_resume_patrol");
}

FGcsLineCombatEngaging::FGcsLineCombatEngaging(FSystemInfo* InSystem, unsigned int InSpeaker,
                                               unsigned int InTarget)
    : FGcsLine(InSystem, InSpeaker, 0)
{
    // (06edd6f0-06edda17). 06edd726-06edd72b `XOR EDI,EDI ; MOV [EBX+0x8],EDI`:
    // the listener is 0, NOT the target -- SendComm then goes to receiver 0 and the client
    // plays it as chatter on channel B (no talking head, HUD_Comm 004ce26c..004ce2b9).
    // InTarget ([ESP+0x1d0], 06edd7a0) is read only for the target's ship below.
    // role 0x4.
    if (!HasVoiceRoles(0x4))
        return;
    // the speaker's ship (result unused), then the target's; no target
    // ship -> System = null (MOV [EBX+0x24],EAX with EAX 0).
    System->FindShip(Speaker);
    FContentSpaceObj* const Target = System->FindShip(InTarget);
    if (Target == nullptr)
    {
        System = nullptr;
        return;
    }
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3c;
    TextIds.reserve(4);
    // the name copied (the routine) with "+".
    PushText(MakeVoiceLineName(MakeTString64("gcs_combat_engaging").Data, "+", VoiceId).Data);
    // the target's faction (the routine of the routine) + "_short";
    // a last text id of 0 -> System = null.
    PushText(MakeShortFactionName(Target).Data);
    if (TextIds.back() == 0)
    {
        System = nullptr;
        return;
    }
    // .
    PushText(MakeObjectReferenceName(1, Target, nullptr).Data);
    if (TextIds.back() == 0)
    {
        System = nullptr;
        return;
    }
    // .
    PushText(MakeObjectReferenceName(2, Target, "-").Data);
    if (TextIds.back() == 0)
        System = nullptr;
}

FGcsLineCombatNoEngaging::FGcsLineCombatNoEngaging(FSystemInfo* InSystem,
                                                   unsigned int InSpeaker,
                                                   unsigned int InListener, int Reason)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06edda20-06eddbff): role 0x4.
    if (!HasVoiceRoles(0x4))
        return;
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3a;
    TextIds.reserve(2);
    // the name copied (the routine) with "+".
    PushText(MakeVoiceLineName(MakeTString64("gcs_combat_noengaging").Data, "+", VoiceId).Data);
    // CMP Reason,2 / JNZ.
    PushText(Reason == 2 ? "gcs_combat_noengagereason_tootough_01-"
                         : "gcs_combat_noengagereason_toofar_01-");
}

FGcsLineCombatAskEngage::FGcsLineCombatAskEngage(FSystemInfo* InSystem, unsigned int InSpeaker,
                                                 unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06eddc10-06edddf3).
    BuildSingleVoiceLine(0x4, "gcs_combat_askengage");
}

FGcsLineCombatOrderEngage::FGcsLineCombatOrderEngage(FSystemInfo* InSystem,
                                                     unsigned int InSpeaker,
                                                     unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06edde00-06eddfeb).
    BuildSingleVoiceLine(0x4000, "gcs_combat_order_engage");
}

FGcsLineCombatOrderNoEngage::FGcsLineCombatOrderNoEngage(FSystemInfo* InSystem,
                                                         unsigned int InSpeaker,
                                                         unsigned int InListener, int Reason)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06eddff0-06ede1cf): the routine with "gcs_combat_order_noengage" and
    // "-".
    if (!HasVoiceRoles(0x4))
        return;
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3a;
    TextIds.reserve(2);
    PushText(MakeVoiceLineName(MakeTString64("gcs_combat_order_noengage").Data, "-",
                               VoiceId).Data);
    // .
    PushText(Reason == 2 ? "gcs_combat_noengagereason_tootough_01-"
                         : "gcs_combat_noengagereason_toofar_01-");
}

FGcsLineCombatEnemySighted::FGcsLineCombatEnemySighted(FSystemInfo* InSystem,
                                                       unsigned int InSpeaker,
                                                       unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06ede1e0-06ede3cb).
    BuildSingleVoiceLine(0x4000, "gcs_combat_announce_enemysighted");
}

FGcsLineCombatAskForHelp::FGcsLineCombatAskForHelp(FSystemInfo* InSystem,
                                                   unsigned int InSpeaker,
                                                   unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06ede3d0-06ede6b6). 06ede44d-06ede46f: role 0x4.
    if (!HasVoiceRoles(0x4))
        return;
    // the listener's ship (not null-tested).
    FContentSpaceObj* const Ship = System->FindShip(Listener);
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3c;
    TextIds.reserve(4);
    // GetFactionProps directly (null -> "gcs_refer_faction_player") +
    // "_short" -- the MakeShortFactionName outcome.
    PushText(MakeShortFactionName(Ship).Data);
    // / 06ede5da-06ede61f.
    PushText(MakeObjectReferenceName(1, Ship, nullptr).Data);
    PushText(MakeObjectReferenceName(2, Ship, "-").Data);
    // the name copied (the routine) with "-".
    PushText(MakeVoiceLineName(MakeTString64("gcs_combat_askforhelp").Data, "-", VoiceId).Data);
}

FGcsLineCombatComingIn::FGcsLineCombatComingIn(FSystemInfo* InSystem, unsigned int InSpeaker,
                                               unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06ede6c0-06ede9a9). 06ede73d-06ede75f: role 0x4.
    if (!HasVoiceRoles(0x4))
        return;
    // the speaker's own ship (not null-tested).
    FContentSpaceObj* const Ship = System->FindShip(Speaker);
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3a;
    TextIds.reserve(2);
    // "gcs_misc_ack" copied (the routine) with "-".
    PushText(MakeVoiceLineName(MakeTString64("gcs_misc_ack").Data, "-", VoiceId).Data);
    // the the routine body inlined (SETA), "%s%s%s" with _p / _s and "",
    // with "-".
    PushText(MakeFormationVoiceLineName(Ship, "gcs_combat_comingin", "-", VoiceId).Data);
}

FGcsLineCombatOfferHelp::FGcsLineCombatOfferHelp(FSystemInfo* InSystem, unsigned int InSpeaker,
                                                 unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06ede9b0-06ededc6). 06edea30-06edea52: role 0x4.
    if (!HasVoiceRoles(0x4))
        return;
    // both ships (not null-tested).
    FContentSpaceObj* const SpeakerShip = System->FindShip(Speaker);
    FContentSpaceObj* const ListenerShip = System->FindShip(Listener);
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a41;
    TextIds.reserve(9);
    // (the routine appends).
    PushText("gcs_misc_thisis+");
    PushText(MakeShortFactionName(SpeakerShip).Data);
    PushText(MakeObjectReferenceName(1, SpeakerShip, nullptr).Data);
    PushText(MakeObjectReferenceName(2, SpeakerShip, nullptr).Data);
    PushText("gcs_misc_to");
    // .
    PushText(MakeShortFactionName(ListenerShip).Data);
    PushText(MakeObjectReferenceName(1, ListenerShip, nullptr).Data);
    PushText(MakeObjectReferenceName(2, ListenerShip, "-").Data);
    // the name copied (the routine) with "-".
    PushText(MakeVoiceLineName(MakeTString64("gcs_combat_offerhelp").Data, "-", VoiceId).Data);
}

FGcsLineCombatRetreatTowards::FGcsLineCombatRetreatTowards(FSystemInfo* InSystem,
                                                           unsigned int InSpeaker,
                                                           unsigned int InListener, int Reason)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06ededd0-06edf159). 06edee4d-06edee72: role 0xc004.
    if (!HasVoiceRoles(0xc004))
        return;
    // the speaker's own ship (not null-tested).
    FContentSpaceObj* const Ship = System->FindShip(Speaker);
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3c;
    TextIds.reserve(4);
    // (the routine copy) / 06edef80-06edeff6 (the routine copy), "+".
    PushText(MakeVoiceLineName(MakeTString64("gcs_misc_attentionallwingmen").Data, "+",
                               VoiceId).Data);
    PushText(MakeVoiceLineName(MakeTString64("gcs_combat_order_retreattowards").Data, "+",
                               VoiceId).Data);
    // the guide's first target (slot +0x34, first out-parameter), its
    // place (the routine + "-").
    unsigned int TargetId1 = 0;
    unsigned int TargetId2 = 0;
    FContentBase* TargetBase1 = nullptr;
    FContentBase* TargetBase2 = nullptr;
    GetShipGuide(Ship)->GetTargets(&TargetId1, &TargetId2, &TargetBase1, &TargetBase2);
    PushText(MakeObjectLocationName(TargetId1).Data);
    // the routine(Reason, +0x0c).
    PushText(MakeFleeReasonName(Reason, VoiceId).Data);
}

FGcsLineCombatAnnounceFlee::FGcsLineCombatAnnounceFlee(FSystemInfo* InSystem,
                                                       unsigned int InSpeaker,
                                                       unsigned int InListener, int Reason)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06edf160-06edf3bd): role 0x4.
    if (!HasVoiceRoles(0x4))
        return;
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3a;
    TextIds.reserve(2);
    // the name copied (the routine) with "-".
    PushText(MakeVoiceLineName(MakeTString64("gcs_combat_announce_flee").Data, "-",
                               VoiceId).Data);
    // the the routine body inlined.
    PushText(MakeFleeReasonName(Reason, VoiceId).Data);
}

FGcsLineCombatFriendlyFire::FGcsLineCombatFriendlyFire(FSystemInfo* InSystem,
                                                       unsigned int InSpeaker,
                                                       unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06edf3c0-06edf5ab).
    BuildSingleVoiceLine(0x4000, "gcs_combat_announce_ff");
}

FGcsLineCombatDrift::FGcsLineCombatDrift(FSystemInfo* InSystem, unsigned int InSpeaker,
                                         unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06edf5b0-06edf79b).
    BuildSingleVoiceLine(0x4000, "gcs_combat_announce_combatdrift");
}

FGcsLineStationAllClear::FGcsLineStationAllClear(FSystemInfo* InSystem, unsigned int InSpeaker,
                                                 unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06ee1830-06ee1b4c). 06ee18ad-06ee18d2: role 0x800.
    if (!HasVoiceRoles(0x800))
        return;
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3c;
    TextIds.reserve(4);
    // the name copied (the routine) with "-".
    PushText(MakeVoiceLineName(MakeTString64("gcs_combat_attentionallunits").Data, "-",
                               VoiceId).Data);
    PushText("gcs_misc_thisis+");
    // the station's place (the routine + "-").
    PushText(MakeObjectLocationName(Speaker).Data);
    // .
    PushText(MakeVoiceLineName(MakeTString64("gcs_combat_announce_allclear").Data, "-",
                               VoiceId).Data);
}

FGcsLineStationEngage::FGcsLineStationEngage(FSystemInfo* InSystem, unsigned int InSpeaker,
                                             unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06ee1b50-06ee1f3a). 06ee1bc8-06ee1bed: role 0x800.
    if (!HasVoiceRoles(0x800))
        return;
    // the station's place first; an empty one ends the ctor (System
    // kept, no text).
    TString<0x40> const Place = MakeObjectLocationName(Speaker);
    if (Place.Length == 0)
        return;
    // (rand() * 2) / 32768.
    int const Pick = (rand() * 2) / 32768;
    if (Pick == 1)
    {
        // room for four (the routine capacity test).
        Field_0x28 = 0x4a3c;
        TextIds.reserve(4);
        PushText(MakeVoiceLineName(MakeTString64("gcs_combat_attentionallunits").Data, "-",
                                   VoiceId).Data);
        PushText("gcs_misc_thisis+");
        PushText(Place.Data);
        PushText(MakeVoiceLineName(MakeTString64("gcs_combat_announce_engage").Data, "-",
                                   VoiceId).Data);
        return;
    }
    if (Pick != 0)
        return;
    // .
    Field_0x28 = 0x4a39;
    TextIds.reserve(1);
    PushText(MakeVoiceLineName(MakeTString64("gcs_combat_manturrets").Data, "-", VoiceId).Data);
}

// ---------------------------------------------------------------------------------------
// Scan lines (FGcsScanConversation)
// ---------------------------------------------------------------------------------------

FGcsLineScanAnnounce::FGcsLineScanAnnounce(FSystemInfo* InSystem, unsigned int InSpeaker,
                                           unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06ee5c20-06ee6089). 06ee5ca0-06ee5cc2: role 0x8.
    if (!HasVoiceRoles(0x8))
        return;
    // both ships (the routine, not null-tested).
    FContentSpaceObj* const SpeakerShip = System->FindShip(Speaker);
    FContentSpaceObj* const ListenerShip = System->FindShip(Listener);
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a43;
    // room for eleven (the routine capacity test).
    TextIds.reserve(11);
    // (the routine appends).
    PushText("gcs_misc_thisis+");
    PushText(MakeShortFactionName(SpeakerShip).Data);
    PushText(MakeObjectReferenceName(1, SpeakerShip, nullptr).Data);
    PushText(MakeObjectReferenceName(2, SpeakerShip, nullptr).Data);
    PushText("gcs_misc_to");
    // .
    PushText(MakeShortFactionName(ListenerShip).Data);
    PushText(MakeObjectReferenceName(1, ListenerShip, nullptr).Data);
    PushText(MakeObjectReferenceName(2, ListenerShip, nullptr).Data);
    PushText("gcs_misc_dash");
    PushText(MakeObjectReferenceName(3, ListenerShip, nullptr).Data);
    // the name copied (the routine) with "-", the routine.
    PushText(MakeVoiceLineName(MakeTString64("gcs_scan_announce").Data, "-", VoiceId).Data);
}

FGcsLineScanNothingFound::FGcsLineScanNothingFound(FSystemInfo* InSystem,
                                                   unsigned int InSpeaker,
                                                   unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06ee6090-06ee6273).
    BuildSingleVoiceLine(0x8, "gcs_scan_nothingfound");
}

FGcsLineScanOrderDrop::FGcsLineScanOrderDrop(FSystemInfo* InSystem, unsigned int InSpeaker,
                                             unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06ee6280-06ee6463).
    BuildSingleVoiceLine(0x8, "gcs_scan_orderdrop");
}

FGcsLineScanRefuse::FGcsLineScanRefuse(FSystemInfo* InSystem, unsigned int InSpeaker,
                                       unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06ee6470-06ee6653).
    BuildSingleVoiceLine(0x8, "gcs_scan_refuse");
}

FGcsLineScanComply::FGcsLineScanComply(FSystemInfo* InSystem, unsigned int InSpeaker,
                                       unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06ee6660-06ee6843).
    BuildSingleVoiceLine(0x8, "gcs_scan_comply");
}

FGcsLineScanAngry::FGcsLineScanAngry(FSystemInfo* InSystem, unsigned int InSpeaker,
                                     unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06ee6850-06ee6a33).
    BuildSingleVoiceLine(0x8, "gcs_scan_angry");
}

FGcsLineScanHappy::FGcsLineScanHappy(FSystemInfo* InSystem, unsigned int InSpeaker,
                                     unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06ee6a40-06ee6c23).
    BuildSingleVoiceLine(0x8, "gcs_scan_happy");
}

FGcsLineScanLootDestroyed::FGcsLineScanLootDestroyed(FSystemInfo* InSystem,
                                                     unsigned int InSpeaker,
                                                     unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06ee6c30-06ee6e13).
    BuildSingleVoiceLine(0x8, "gcs_scan_lootdestroyed");
}

// ---------------------------------------------------------------------------------------
// Fidget lines (FGcsFidgetConversation)
// ---------------------------------------------------------------------------------------

FGcsLineFidgetTradeRumor::FGcsLineFidgetTradeRumor(FSystemInfo* InSystem, unsigned int InSpeaker,
                                                   unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06edf950-06edfb3b; the listing of the routine but for the vtable, role
    // 0x400 and the name).
    BuildSingleVoiceLine(0x400, "gcs_fidget_traderumor");
}

FGcsLineFidgetTradeIHear::FGcsLineFidgetTradeIHear(FSystemInfo* InSystem, unsigned int InSpeaker,
                                                   unsigned int InListener, unsigned int Good)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06edfb40-06edfdd4). 06edfbbd-06edfbe2: role 0x400.
    if (!HasVoiceRoles(0x400))
        return;
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3b;
    // room for three.
    TextIds.reserve(3);
    // the name copied (the routine, 0x40), the routine with "+".
    PushText(MakeVoiceLineName(MakeTString64("gcs_fidget_trade_ihear").Data, "+", VoiceId).Data);
    // the good's prefix (status ignored).
    TString<0x40> GoodPrefix;
    GoodPrefix.Length = 0;
    GoodPrefix.Data[0] = '\0';
    pub::TranslateGoodToMsgIdPrefix(Good, GoodPrefix);
    PushText(GoodPrefix.Data);
    // .
    PushText(MakeVoiceLineName(MakeTString64("gcs_fidget_talkcargo").Data, "-", VoiceId).Data);
}

FGcsLineFidgetTradeSimple::FGcsLineFidgetTradeSimple(FSystemInfo* InSystem,
                                                     unsigned int InSpeaker,
                                                     unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06edfde0-06edffcb; one-name template).
    BuildSingleVoiceLine(0x400, "gcs_fidget_trade_simple");
}

FGcsLineFidgetStatusQuery::FGcsLineFidgetStatusQuery(FSystemInfo* InSystem,
                                                     unsigned int InSpeaker,
                                                     unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06edffd0-06ee038d). 06ee0048-06ee006d: role 0x400.
    if (!HasVoiceRoles(0x400))
        return;
    // the speaker's own ship (the routine, not null-tested).
    FContentSpaceObj* const Ship = System->FindShip(Speaker);
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a40;
    // room for eight.
    TextIds.reserve(8);
    // the routine copy, the routine with "+".
    PushText(MakeVoiceLineName(MakeTString64("gcs_misc_attentionallwingmen").Data, "+", VoiceId)
                 .Data);
    // .
    PushText("gcs_misc_thisis+");
    PushText(MakeShortFactionName(Ship).Data);
    // .
    PushText(MakeObjectReferenceName(1, Ship, nullptr).Data);
    PushText(MakeObjectReferenceName(2, Ship, nullptr).Data);
    PushText("gcs_misc_dash");
    PushText(MakeObjectReferenceName(3, Ship, nullptr).Data);
    // .
    PushText(MakeVoiceLineName(MakeTString64("gcs_fidget_whatstatus").Data, "-", VoiceId).Data);
}

FGcsLineFidgetStatusReport::FGcsLineFidgetStatusReport(FSystemInfo* InSystem,
                                                       unsigned int InSpeaker,
                                                       unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06ee0390-06ee06c4). 06ee040d-06ee0432: role 0x400.
    if (!HasVoiceRoles(0x400))
        return;
    // the speaker's own ship (the routine, not null-tested).
    FContentSpaceObj* const Ship = System->FindShip(Speaker);
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3e;
    // room for six.
    TextIds.reserve(6);
    // .
    PushText("gcs_misc_thisis+");
    PushText(MakeObjectReferenceName(1, Ship, nullptr).Data);
    PushText(MakeObjectReferenceName(2, Ship, nullptr).Data);
    PushText("gcs_misc_dash");
    PushText(MakeObjectReferenceName(3, Ship, nullptr).Data);
    // the ship's relative health (status not tested) into the local that
    // held the ship pointer ([ESP+0x10]), so a failing call leaves the pointer's bits.
    unsigned int const ShipBits =
        static_cast<unsigned int>(reinterpret_cast<std::uintptr_t>(Ship));
    float Health = 0.0f;
    std::memcpy(&Health, &ShipBits, sizeof(Health));
    pub::SpaceObj::GetRelativeHealth(Ship->SpaceObjId, Health);
    // FCOMP double the field (0.3) / the field (0.75) /
    // (1.0), TEST AH,5 / JP: C0 alone (below) picks the name; unordered falls
    // through to "none".
    char const* Name = "gcs_fidget_saystatus_damagenone";
    if (static_cast<double>(Health) < 0.3)
        Name = "gcs_fidget_saystatus_damagelarge";
    else if (static_cast<double>(Health) < 0.75)
        Name = "gcs_fidget_saystatus_damagemedium";
    else if (static_cast<double>(Health) < 1.0)
        Name = "gcs_fidget_saystatus_damagesmall";
    // the routine with "-", the routine.
    PushText(MakeVoiceLineName(Name, "-", VoiceId).Data);
}

FGcsLineFidgetPatrolRumor::FGcsLineFidgetPatrolRumor(FSystemInfo* InSystem,
                                                     unsigned int InSpeaker,
                                                     unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06ee06d0-06ee08bb; one-name template).
    BuildSingleVoiceLine(0x400, "gcs_fidget_patrolrumor");
}

FGcsLineFidgetPatrolSimple::FGcsLineFidgetPatrolSimple(FSystemInfo* InSystem,
                                                       unsigned int InSpeaker,
                                                       unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06ee08c0-06ee0aab; one-name template).
    BuildSingleVoiceLine(0x400, "gcs_fidget_patrolsimple");
}

FGcsLineFidgetWaypointQuery::FGcsLineFidgetWaypointQuery(FSystemInfo* InSystem,
                                                         unsigned int InSpeaker,
                                                         unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06ee0ab0-06ee0e0c). 06ee0b33-06ee0b47: the speaker's own ship
    // (the routine, not null-tested) is looked up between GetVoiceID and the voice record;
    // both are plain lookups, so the port asks after the voice test. 06ee0b4b-06ee0b69:
    // role 0x400.
    if (!HasVoiceRoles(0x400))
        return;
    FContentSpaceObj* const Ship = System->FindShip(Speaker);
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3f;
    // room for seven.
    TextIds.reserve(7);
    // .
    PushText("gcs_misc_thisis+");
    PushText(MakeShortFactionName(Ship).Data);
    PushText(MakeObjectReferenceName(1, Ship, nullptr).Data);
    PushText(MakeObjectReferenceName(2, Ship, nullptr).Data);
    PushText("gcs_misc_dash");
    PushText(MakeObjectReferenceName(3, Ship, nullptr).Data);
    // .
    PushText(MakeVoiceLineName(MakeTString64("gcs_fidget_patrol_wpremainquery").Data, "-",
                               VoiceId)
                 .Data);
}

FGcsLineFidgetPatrolStatus::FGcsLineFidgetPatrolStatus(FSystemInfo* InSystem,
                                                       unsigned int InSpeaker,
                                                       unsigned int InListener)
    : FGcsLine(InSystem, InSpeaker, InListener)
{
    // (06ee0e10-06ee11af). 06ee0e93-06ee0ea7: the speaker's own ship
    // (the routine, not null-tested) between GetVoiceID and the voice record (plain
    // lookups; the port asks after the voice test). 06ee0eab-06ee0ec9: role 0x400.
    if (!HasVoiceRoles(0x400))
        return;
    FContentSpaceObj* const Ship = System->FindShip(Speaker);
    // the ship's patrol guide (the routine), slot +0x4c (the branch: the
    // route's points to go).
    int const PointsToGo = static_cast<FPatrolGuide*>(GetShipGuide(Ship))->GetPathPointsToGo();
    if (PointsToGo == 0)
    {
        // no rand() test; +0x28 = 0x4a39, room for one; "gcs_patrol_
        // finished" (the routine copy) + "_p" when the formation has more than one member
        // (the routine into a local, SETA), else "_s", formatted "%s%s%s" with "", then
        // with "-" and the routine.
        Field_0x28 = 0x4a39;
        TextIds.reserve(1);
        PushText(MakeFormationVoiceLineName(Ship, "gcs_patrol_finished", "-", VoiceId).Data);
        return;
    }
    // .
    if (rand() / 32768 != 0)
        return;
    Field_0x28 = 0x4a3b;
    // room for three.
    TextIds.reserve(3);
    // .
    PushText(PointsToGo == 1 ? "gcs_patrol_wehaveonly+" : "gcs_patrol_wehave+");
    // "gcs_misc_number_%d%s" with "" (the routine).
    TString<0x40> Number;
    Number.Length = 0;
    Number.Data[0] = '\0';
    FormatTString64(&Number, "gcs_misc_number_%d%s", PointsToGo, "");
    PushText(Number.Data);
    // .
    PushText(PointsToGo == 1 ? "gcs_patrol_morewptogo-" : "gcs_patrol_morewpstogo-");
}

// =======================================================================================
// Conversations
// =======================================================================================

FGcsConversation::~FGcsConversation()
{
    // delete the front line and erase it until none is left.
    while (!Lines.empty())
    {
        if (Lines.front() != nullptr)
            delete Lines.front();
        Lines.erase(Lines.begin());
    }
}

void FGcsConversation::Start()
{
    // .
    if (!Lines.empty())
        Lines.front()->Play();
}

bool FGcsConversation::OnMessage(void const* Parms)
{
    // .
    for (std::vector<FGcsLine*>::iterator It = Lines.begin(); It != Lines.end(); ++It)
    {
        if (!(*It)->IsDone(Parms))
            continue;
        if (*It != nullptr)
            delete *It;
        It = Lines.erase(It);
        if (It != Lines.end() && (*It)->CanPlay())
        {
            (*It)->Play();
            return true;
        }
        bFinished = true;
        return true;
    }
    return false;
}

bool FGcsConversation::IsFinished()
{
    // .
    return bFinished;
}

bool FGcsConversation::AddLine(FGcsLine* Line)
{
    // The inline step of the conversation ctors (e.g. 06ee89a5-06ee89bd / 06ee8a39-06ee8a45):
    // vt[+4] true -> the routine insert(end, 1, &line); false -> vt[0](1) when non-null.
    if (Line->CanPlay())
    {
        Lines.insert(Lines.end(), 1, Line);
        return true;
    }
    if (Line != nullptr)
        delete Line;
    return false;
}

// ---------------------------------------------------------------------------------------
// Conversation classes
// ---------------------------------------------------------------------------------------

FGcsLaunchConversation::FGcsLaunchConversation(FGcsLaunchEvent const* Event)
    : System(Event->System)
    , Speaker(Event->Speaker)
    , Listener(Event->Listener)
    , DockIndex(Event->DockIndex)
    , TerminalType(Event->TerminalType)
{
    // . 06ee88c8-06ee893b: room for two lines.
    Lines.reserve(2);
    // the dock's clearance; failing, System = null and nothing else.
    if (!AddLine(new FGcsLineLaunchClearance(System, Speaker, Listener, TerminalType)))
    {
        System = nullptr;
        return;
    }
    // a ship that is no player's acknowledges; failing, System = null.
    if (FindContentPlayerStateByShip(&Listener) != nullptr)
        return;
    if (!AddLine(new FGcsLineAck(System, Listener, Speaker)))
        System = nullptr;
}

bool FGcsLaunchConversation::IsValid()
{
    // .
    return System != nullptr;
}

FGcsPlayerHailConversation::FGcsPlayerHailConversation(FGcsPlayerObjectEvent const* Event)
    : System(Event->System)
    , Player(Event->Player)
    , Object(Event->Object)
{
    // (06ee4cd0-06ee55e4). 06ee4d13-06ee4d79: room for two lines.
    Lines.reserve(2);

    // the object's formation leader (the routine, not null-tested), the
    // object itself when its formation is -1.
    FContentShipBase* Leader = static_cast<FContentShipBase*>(Object);
    if (Leader->FormationId != 0xffffffffu)
        Leader = FindFormationLeader(Leader->FormationId);

    // the leader's attitude towards the player (the routine of the
    // leader's handle and of the player state); FCOMP the field (-0.6f), TEST AH,1 / JNZ:
    // C0 (below, or unordered) -> no line.
    int LeaderRep = Leader->ReputationHandle->GetReputation();
    int PlayerRep = Player->GetReputation();
    float Attitude = 0.0f;
    pub::Reputation::GetAttitude(LeaderRep, PlayerRep, Attitude);
    if (!(Attitude >= -0.6f))
        return;
    // only a SpacePop ship answers (the routine).
    FContentNpcShip* const Ship = static_cast<FContentNpcShip*>(Leader);
    if (!Ship->IsSpacePopShip())
        return;

    // The player's ship id as the lines' listener: +0x10 -> +0x04 read directly by the
    // blow-off paths (06ee4e29-06ee4e33, 06ee4eca-06ee4ed0), the routine by the identify path.
    FGcsLine* Line = nullptr;
    if (Leader->Slot08())
    {
        // vt[+8] (busy).
        Line = new FGcsLinePlayerHailCombatBlowOff(System, Leader->SpaceObjId,
                                                   Player->ShipObject->SpaceObjId);
    }
    else
    {
        // the same player again (+0x1a0) within the field (15.0) s of
        // its last hail (the routine of +0x1a8 and the routine); FCOMP, TEST AH,5 / JP:
        // C0 alone (below) -> the blow-off.
        bool bBlowOff = false;
        if (Player == Ship->LastHailPlayer)
        {
            double const Now = GetContentGameTime();
            double const LastHail = Ship->LastHailTime;
            bBlowOff = GetContentTimeDistance(&LastHail, &Now) < 15.0;
        }
        // / 06ee4eec-06ee4ef4: the routine either way.
        Ship->SetLastHailPlayer(Player);
        if (bBlowOff)
        {
            // .
            Line = new FGcsLinePlayerHailBlowOff(System, Leader->SpaceObjId,
                                                 Player->ShipObject->SpaceObjId);
        }
        else
        {
            // the leader names itself; failing, deleted and System = null
            // (06ee55b7-06ee55c9).
            if (!AddLine(new FGcsLinePlayerHailIdentify(System, Leader->SpaceObjId,
                                                        Player->GetShipId())))
            {
                System = nullptr;
                return;
            }

            // by the leader's guide type (the routine); the lines speak
            // to the player's ship (+0x10 -> +0x04).
            unsigned int const PlayerShip = Player->ShipObject->SpaceObjId;
            switch (Ship->GetGuideType())
            {
            case 2:
            {
                // trade home; 06ee54ec-06ee554e: what it trades, failing
                // that (deleted, System kept) 06ee5561-06ee55af where it is going.
                if (!AddLine(new FGcsLineTradeIdHome(System, Leader->SpaceObjId, PlayerShip)))
                {
                    System = nullptr;
                    return;
                }
                FGcsLine* const Status =
                    new FGcsLineTradeStatus(System, Leader->SpaceObjId, PlayerShip);
                if (Status->CanPlay())
                {
                    Lines.insert(Lines.end(), 1, Status);
                    return;
                }
                delete Status;
                Line = new FGcsLineGoingTo(System, Leader->SpaceObjId, PlayerShip);
                break;
            }
            case 3:
                // patrol home; 06ee5050-06ee509e: where it is going
                // (06ee50a4-06ee5203 insert(end) inlined); 06ee5207-06ee5255: its progress
                // (06ee525b-06ee5467 insert(end) inlined; failing 06ee546c: deleted, System =
                // null).
                if (!AddLine(new FGcsLinePatrolIdHome(System, Leader->SpaceObjId, PlayerShip)) ||
                    !AddLine(new FGcsLineGoingTo(System, Leader->SpaceObjId, PlayerShip)))
                {
                    System = nullptr;
                    return;
                }
                Line = new FGcsLinePatrolStatus(System, Leader->SpaceObjId, PlayerShip);
                break;
            case 4:
                // wander: home only.
                Line = new FGcsLinePatrolIdHome(System, Leader->SpaceObjId, PlayerShip);
                break;
            default:
                return;
            }
        }
    }

    // (and the inline tails of the guide cases): a line that can play is
    // appended; otherwise it is deleted and System = null (06ee55bb-06ee55c9).
    if (!AddLine(Line))
        System = nullptr;
}

bool FGcsPlayerHailConversation::IsValid()
{
    // .
    return System != nullptr;
}

namespace
{
    // The inline "formation has wingmen" test of the combat cases (e.g. 06ee3078-06ee30d0):
    // into a local vector, size > 1 (SETA), the vector freed.
    bool HasFormationMembers(FContentShipBase* Ship)
    {
        std::vector<FContentShipBase*> Members;
        CollectFormationMembers(&Members, Ship->FormationId);
        return Members.size() > 1;
    }

    // The inline wingman test (e.g. 06ee30d6-06ee30e9): slot +0x14c != 0 and formation != -1.
    bool IsFormationWingman(FContentShipBase* Ship)
    {
        return Ship->FormationSlot != 0 && Ship->FormationId != 0xffffffffu;
    }
}

FGcsCombatConversation::FGcsCombatConversation(FGcsCombatEvent const* Event)
    : System(Event->System)
{
    // (06ee2c20-06ee4c90). 06ee2c3f-06ee2c71: the base part (no room reserved),
    // +0x18 = event +0x04, vtable 06fb72d4. 06ee2c82-06ee2c95: the event's type - 6 over the
    // jump table 06ee4c94 (6..0x10; anything else nothing).
    unsigned int const ObjectA = Event->ObjectA;
    unsigned int const ObjectB = Event->ObjectB;
    switch (Event->GetType())
    {
    case 6:
    {
        // both types, 0 when GetType fails (B first).
        float const Health = static_cast<FGcsCombatDamageEvent const*>(Event)->Health;
        unsigned int TypeB = 0;
        if (pub::SpaceObj::GetType(ObjectB, TypeB) != 0)
            TypeB = 0;
        unsigned int TypeA = 0;
        if (pub::SpaceObj::GetType(ObjectA, TypeA) != 0)
            TypeA = 0;
        FGcsLine* Line = nullptr;
        if ((TypeA & 0x7f0000) != 0)
        {
            // a ship hit a ship: (rand() * 2) / 32768 picks who speaks.
            if ((TypeB & 0x7f0000) == 0)
                return;
            if ((rand() * 2) / 32768 != 0)
                Line = new FGcsLineCombatInflictingDamage(System, ObjectA, 0, Health);
            else
                Line = new FGcsLineCombatTakingDamage(System, ObjectB, 0, Health);
        }
        else
        {
            // the attacker is no ship: type & 0xfdff and a ship victim.
            if ((TypeA & 0xfdff) == 0)
                return;
            if ((TypeB & 0x7f0000) == 0)
                return;
            Line = new FGcsLineCombatTakingDamage(System, ObjectB, 0, Health);
        }
        // .
        AddLineOrClear(Line);
        return;
    }
    case 7:
    {
        // the killer's and the victim's ships (the routine).
        FContentSpaceObj* const ShipA = System->FindShip(ObjectA);
        FContentSpaceObj* const ShipB = System->FindShip(ObjectB);
        bool bBrag = false;
        if (ShipB == nullptr)
        {
            // .
            if (ShipA == nullptr)
            {
                System = nullptr;
                return;
            }
            bBrag = true;
        }
        else if (ShipA != nullptr)
        {
            // (rand() * 2) / 32768: 1 brags, 0 falls through.
            int const Pick = (rand() * 2) / 32768;
            if (Pick == 1)
            {
                bBrag = true;
            }
            else if (Pick != 0)
            {
                System = nullptr;
                return;
            }
        }
        FGcsLine* Line = nullptr;
        if (bBrag)
        {
            // .
            Line = new FGcsLineCombatBrag(System, ObjectA, 0);
        }
        else if ((rand() * 2) / 32768 != 0)
        {
            // .
            Line = new FGcsLineCombatScream(System, ObjectB, 0);
        }
        else
        {
            // the victim's formation leader (the routine) mourns it; the
            // victim leading screams itself.
            FContentShipBase* const Leader =
                static_cast<FContentShipBase*>(ShipB)->GetFormationLeader();
            if (Leader->SpaceObjId == ObjectB)
                Line = new FGcsLineCombatScream(System, ObjectB, 0);
            else
                Line = new FGcsLineCombatDeathSympathy(System, Leader->SpaceObjId, ObjectB);
        }
        // / 06ee3acd.
        AddLineOrClear(Line);
        return;
    }
    case 8:
    {
        // who is in whose sights.
        FContentSpaceObj* const ObjA = System->FindSpaceObj(ObjectA);
        FContentSpaceObj* const ObjB = System->FindSpaceObj(ObjectB);
        FGcsLine* Line = nullptr;
        if (ObjA == nullptr)
            Line = new FGcsLineCombatInSights(System, ObjectB, 0);
        else if (ObjB == nullptr)
            Line = new FGcsLineCombatHasInSights(System, ObjectA, 0);
        else if ((rand() * 2) / 32768 != 0)
            Line = new FGcsLineCombatHasInSights(System, ObjectA, 0);
        else
            Line = new FGcsLineCombatInSights(System, ObjectB, 0);
        AddLineOrClear(Line);
        return;
    }
    case 9:
    {
        // a ship engages ObjectB.
        FContentSpaceObj* const Object = System->FindSpaceObj(ObjectA);
        if (Object == nullptr)
        {
            // no such object here (a station): its place name (the
            // body inlined) must not be empty.
            if (MakeObjectLocationName(ObjectA).Length == 0)
            {
                System = nullptr;
                return;
            }
            AddLineOrClear(new FGcsLineStationEngage(System, ObjectA, 0));
            return;
        }
        FContentShipBase* const Ship = static_cast<FContentShipBase*>(Object);
        // a lone ship says nothing.
        if (!HasFormationMembers(Ship))
            return;
        if (IsFormationWingman(Ship))
        {
            // a wingman engages unless its leader's id is a multiple of 5.
            if (Ship->GetFormationLeader()->SpaceObjId % 5 == 0)
                return;
            AddLineOrClear(new FGcsLineCombatEngaging(System, Ship->SpaceObjId, ObjectB));
            return;
        }
        if (Ship->SpaceObjId % 5 != 0)
        {
            // .
            AddLineOrClear(new FGcsLineCombatEngaging(System, Ship->SpaceObjId, ObjectB));
            return;
        }
        // the leader announces the enemy (or a wingman asks to engage).
        if (!AddEnemySightedLine(Ship))
            return;
        // the order.
        if (!AddLineOrClear(new FGcsLineCombatOrderEngage(System, Ship->SpaceObjId, 0)))
            return;
        // on the rand pick the wingmen acknowledge.
        if ((rand() * 2) / 32768 == 0)
            return;
        AddFormationAcks(Ship);
        return;
    }
    case 0xa:
    {
        // the fight is over.
        FContentSpaceObj* const Object = System->FindSpaceObj(ObjectA);
        if (Object == nullptr)
        {
            // a station: its place name must not be empty.
            if (MakeObjectLocationName(ObjectA).Length == 0)
            {
                System = nullptr;
                return;
            }
            AddLineOrClear(new FGcsLineStationAllClear(System, ObjectA, 0));
            return;
        }
        FContentShipBase* const Ship = static_cast<FContentShipBase*>(Object);
        // a lone ship announces all clear (06ee40cc-06ee40fe).
        if (!HasFormationMembers(Ship))
        {
            AddLineOrClear(new FGcsLineCombatAllClear(System, Ship->SpaceObjId, 0));
            return;
        }
        if (IsFormationWingman(Ship))
        {
            // unless the leader's id is a multiple of 5, reason 3 rejoins
            // the formation, any other announces all clear.
            if (Ship->GetFormationLeader()->SpaceObjId % 5 == 0)
                return;
            int const Reason = static_cast<FGcsCombatOverEvent const*>(Event)->Reason;
            if (Reason == 3)
                AddLineOrClear(new FGcsLineCombatRejoiningForm(System, Ship->SpaceObjId, 0));
            else
                AddLineOrClear(new FGcsLineCombatAllClear(System, Ship->SpaceObjId, 0));
            return;
        }
        // / 06ee3fe2-06ee4038: a leader that is no SpacePop ship or whose
        // id is no multiple of 5 announces all clear.
        if (!static_cast<FContentNpcShip*>(Object)->IsSpacePopShip() ||
            Ship->SpaceObjId % 5 != 0)
        {
            AddLineOrClear(new FGcsLineCombatAllClear(System, Ship->SpaceObjId, 0));
            return;
        }
        // the order to disengage.
        if (!AddLineOrClear(new FGcsLineCombatOrderDisengage(System, Ship->SpaceObjId, 0)))
            return;
        // by the guide type (the routine): 2 resumes trade, 3 and 4
        // patrol, the rest nothing.
        int const GuideType = static_cast<FContentNpcShip*>(Object)->GetGuideType();
        if (GuideType == 2)
        {
            if (!AddLineOrClear(new FGcsLineCombatResumeTrade(System, Ship->SpaceObjId, 0)))
                return;
        }
        else if (GuideType > 2 && GuideType <= 4)
        {
            if (!AddLineOrClear(new FGcsLineCombatResumePatrol(System, Ship->SpaceObjId, 0)))
                return;
        }
        // .
        if ((rand() * 2) / 32768 == 0)
            return;
        AddFormationAcks(Ship);
        return;
    }
    case 0xb:
    {
        // a ship does not engage.
        FContentSpaceObj* const Object = System->FindSpaceObj(ObjectA);
        if (Object == nullptr)
            return;
        FContentShipBase* const Ship = static_cast<FContentShipBase*>(Object);
        // .
        if (!HasFormationMembers(Ship))
            return;
        int const Reason = static_cast<FGcsCombatNoEngageEvent const*>(Event)->Reason;
        bool bOrders = false;
        if (IsFormationWingman(Ship))
        {
            // .
            if (Ship->GetFormationLeader()->SpaceObjId % 5 == 0)
                return;
        }
        else
        {
            // .
            bOrders = Ship->SpaceObjId % 5 == 0;
        }
        if (!bOrders)
        {
            // / 06ee3bb8-06ee3c92: the enemy, then why not.
            if (!AddLineOrClear(new FGcsLineCombatEnemySighted(System, Ship->SpaceObjId, 0)))
                return;
            AddLineOrClear(new FGcsLineCombatNoEngaging(System, Ship->SpaceObjId, 0, Reason));
            return;
        }
        // the leader announces the enemy (or a wingman asks to engage).
        if (!AddEnemySightedLine(Ship))
            return;
        // the order not to engage.
        if (!AddLineOrClear(
                new FGcsLineCombatOrderNoEngage(System, Ship->SpaceObjId, 0, Reason)))
        {
            return;
        }
        // .
        if ((rand() * 2) / 32768 == 0)
            return;
        AddFormationAcks(Ship);
        return;
    }
    case 0xc:
    {
        // two NPC ships (slot 1), A no wingman.
        FContentSpaceObj* const ObjA = System->FindSpaceObj(ObjectA);
        if (ObjA == nullptr || !ObjA->Slot04())
            return;
        if (IsFormationWingman(static_cast<FContentShipBase*>(ObjA)))
            return;
        FContentSpaceObj* const ObjB = System->FindSpaceObj(ObjectB);
        if (ObjB == nullptr || !ObjB->Slot04())
            return;
        // .
        if ((rand() * 2) / 32768 != 0)
        {
            // A offers B its help.
            AddLineOrClear(new FGcsLineCombatOfferHelp(System, ObjectA, ObjectB));
            return;
        }
        // B asks A for help, A comes in.
        if (!AddLineOrClear(new FGcsLineCombatAskForHelp(System, ObjectB, ObjectA)))
            return;
        AddLineOrClear(new FGcsLineCombatComingIn(System, ObjectA, ObjectB));
        return;
    }
    case 0xd:
        // .
        AddLineOrClear(new FGcsLineCombatDrift(System, ObjectA, 0));
        return;
    case 0xe:
    {
        // a ship flees (the object is not null-tested).
        FContentShipBase* const Ship =
            static_cast<FContentShipBase*>(System->FindSpaceObj(ObjectA));
        int const Reason = static_cast<FGcsCombatFleeEvent const*>(Event)->Reason;
        // alone it announces its flight (id through the routine).
        if (!HasFormationMembers(Ship))
        {
            AddLineOrClear(new FGcsLineCombatAnnounceFlee(System, Ship->SpaceObjId, 0, Reason));
            return;
        }
        if (IsFormationWingman(Ship))
        {
            // the routine of the leader.
            if (IsSpaceObjIdMultipleOf5(Ship->GetFormationLeader()))
                return;
            AddLineOrClear(new FGcsLineCombatAnnounceFlee(System, Ship->SpaceObjId, 0, Reason));
            return;
        }
        // / 06ee4927-06ee4b1f.
        if (!static_cast<FContentNpcShip*>(Ship)->IsSpacePopShip() ||
            Ship->SpaceObjId % 5 != 0)
        {
            AddLineOrClear(new FGcsLineCombatAnnounceFlee(System, Ship->SpaceObjId, 0, Reason));
            return;
        }
        // the leader orders the retreat.
        if (!AddLineOrClear(
                new FGcsLineCombatRetreatTowards(System, Ship->SpaceObjId, 0, Reason)))
        {
            return;
        }
        // .
        if ((rand() * 2) / 32768 == 0)
            return;
        AddFormationAcks(Ship);
        return;
    }
    case 0xf:
        // friendly fire: B speaks to A.
        if (System->FindSpaceObj(ObjectB) == nullptr)
            return;
        AddLineOrClear(new FGcsLineCombatFriendlyFire(System, ObjectB, ObjectA));
        return;
    case 0x10:
        // a ship takes the lead.
        AddLineOrClear(new FGcsLineCombatTakingLead(System, ObjectA, 0));
        return;
    default:
        return;
    }
}

bool FGcsCombatConversation::IsValid()
{
    // .
    return System != nullptr;
}

bool FGcsCombatConversation::AddLineOrClear(FGcsLine* Line)
{
    // vt[+4] true -> the routine (or the inline / the routine push_back) insert(end);
    // false -> vt[0](1) when non-null, +0x18 = 0.
    if (Line->CanPlay())
    {
        Lines.insert(Lines.end(), 1, Line);
        return true;
    }
    if (Line != nullptr)
        delete Line;
    System = nullptr;
    return false;
}

void FGcsCombatConversation::AddFormationAcks(FContentShipBase* Ship)
{
    // E.g. 06ee338c-06ee347f: a fresh local vector (freed on every exit); the members from
    // the second on (ESI = begin + 4). The earlier size > 1 test covers the first member.
    std::vector<FContentShipBase*> Members;
    CollectFormationMembers(&Members, Ship->FormationId);
    for (std::size_t Index = 1; Index < Members.size(); ++Index)
    {
        unsigned int MemberId = Members[Index]->SpaceObjId;
        if (FindContentPlayerStateByShip(&MemberId) != nullptr)
            continue;
        if (!AddLineOrClear(new FGcsLineAck(System, Members[Index]->SpaceObjId,
                                            Ship->SpaceObjId)))
        {
            return;
        }
    }
}

bool FGcsCombatConversation::AddEnemySightedLine(FContentShipBase* Ship)
{
    // E.g. 06ee3103-06ee3119 (case 9) / 06ee3763-06ee3779 (case 0xb): (rand() * 2) / 32768.
    if ((rand() * 2) / 32768 == 0)
    {
        // the leader announces the enemy.
        return AddLineOrClear(new FGcsLineCombatEnemySighted(System, Ship->SpaceObjId, 0));
    }
    // the first member after the first that is no player's asks to engage
    // (06ee3197-06ee31c0); without one the leader announces the enemy (06ee3226-06ee324f).
    std::vector<FContentShipBase*> Members;
    CollectFormationMembers(&Members, Ship->FormationId);
    FContentShipBase* Asker = nullptr;
    for (std::size_t Index = 1; Index < Members.size() && Asker == nullptr; ++Index)
    {
        unsigned int MemberId = Members[Index]->SpaceObjId;
        if (FindContentPlayerStateByShip(&MemberId) == nullptr)
            Asker = Members[Index];
    }
    FGcsLine* Line = nullptr;
    if (Asker != nullptr)
        Line = new FGcsLineCombatAskEngage(System, Asker->SpaceObjId, 0);
    else
        Line = new FGcsLineCombatEnemySighted(System, Ship->SpaceObjId, 0);
    return AddLineOrClear(Line);
}

FGcsScanConversation::FGcsScanConversation(FGcsScanEvent const* Event)
    : System(Event->System)
    , Speaker(Event->Speaker)
    , Listener(Event->Listener)
{
    // (06ee55f0-06ee5bf2). 06ee5630-06ee569f: room for two lines. 06ee56a2-06ee56b8:
    // +0x18 / +0x1c / +0x20 = event +0x04 / +0x0c / +0x10. 06ee56be: the line local starts at
    // 0. 06ee56c6-06ee56d4: the type - 0x11 over the jump table 06ee5bf8 (above 7: the tail).
    Lines.reserve(2);
    FGcsLine* Line = nullptr;
    switch (Event->GetType())
    {
    case 0x11:
        // .
        Line = new FGcsLineScanAnnounce(System, Speaker, Listener);
        break;
    case 0x12:
        // .
        Line = new FGcsLineScanNothingFound(System, Speaker, Listener);
        break;
    case 0x13:
        // .
        Line = new FGcsLineScanOrderDrop(System, Speaker, Listener);
        break;
    case 0x14:
    {
        // a player's ship (the routine of +0x20) gets the happy line only.
        if (FindContentPlayerStateByShip(&Listener) != nullptr)
        {
            // -> 06ee5974.
            Line = new FGcsLineScanHappy(System, Speaker, Listener);
            break;
        }
        // the scanned NPC complies (06ee57d6-06ee57e0: speaker +0x20,
        // listener +0x1c); a line that cannot play goes to the tail (06ee5a5d), which deletes
        // it.
        Line = new FGcsLineScanComply(System, Listener, Speaker);
        if (!Line->CanPlay())
            break;
        // insert(end) inlined.
        Lines.insert(Lines.end(), 1, Line);
        // .
        Line = new FGcsLineScanHappy(System, Speaker, Listener);
        break;
    }
    case 0x15:
        // only a scanned NPC refuses (06ee59c0-06ee59ca: speaker +0x20,
        // listener +0x1c); a player one reaches the tail with no line.
        if (FindContentPlayerStateByShip(&Listener) != nullptr)
            break;
        Line = new FGcsLineScanRefuse(System, Listener, Speaker);
        break;
    case 0x16:
        // .
        Line = new FGcsLineScanAngry(System, Speaker, Listener);
        break;
    case 0x18:
        // the listener argument is the event's +0x14 (the loot taker).
        Line = new FGcsLineScanLootDestroyed(System, Speaker, Event->LootTakerId);
        break;
    default:
        // Type 0x17 and anything outside 0x11..0x18 (06ee5a5d).
        break;
    }

    // slot 1 of the line (baseline calls through the null local for 0x17
    // and 0x15 with a player listener); true -> insert(end) inlined; false -> slot 0(1) when
    // non-null, +0x18 = 0.
    if (Line->CanPlay())
    {
        Lines.insert(Lines.end(), 1, Line);
        return;
    }
    if (Line != nullptr)
        delete Line;
    System = nullptr;
}

bool FGcsScanConversation::IsValid()
{
    // .
    return System != nullptr;
}

namespace
{
    // (cdecl; callers only the routine: 06ee7c48, 06ee7f65, 06ee8494): for each
    // SystemInfo of the map (the routine) its space objects (+0xdc) whose +0x154 is
    // FormationId are appended (the routine) -- the routine without the player walk -- then
    // sorted by the formation slot +0x14c (signed `<`) with the inlined VC6 std::sort
    // (insertion sort up to 16, else _Sort with the routine / the routine / the routine).
    // FormationId -1 does nothing.
    void CollectSystemFormationMembers(std::vector<FContentShipBase*>* Out,
                                       unsigned int FormationId)
    {
        if (FormationId == 0xffffffffu)
            return;

        std::map<unsigned int, FSystemInfo>* Systems = GetSystemInfos();
        for (std::map<unsigned int, FSystemInfo>::iterator SystemIt = Systems->begin();
             SystemIt != Systems->end(); ++SystemIt)
        {
            std::list<FContentSpaceObj*>& Objects = SystemIt->second.SpaceObjs;
            for (std::list<FContentSpaceObj*>::iterator ObjIt = Objects.begin();
                 ObjIt != Objects.end(); ++ObjIt)
            {
                FContentShipBase* const Member = static_cast<FContentShipBase*>(*ObjIt);
                if (Member->FormationId == FormationId)
                    Out->insert(Out->end(), 1, Member);
            }
        }

        ContentSort(Out->data(), Out->data() + Out->size(),
                    [](FContentShipBase* A, FContentShipBase* B)
                    {
                        return static_cast<int>(A->FormationSlot) < static_cast<int>(B->FormationSlot);
                    });
    }
}

FGcsFidgetConversation::FGcsFidgetConversation(FGcsFidgetEvent const* Event)
    : System(Event->System)
    , Ship(Event->Ship)
    , Kind(Event->Kind)
{
    // (06ee7b00-06ee8866). 06ee7b40-06ee7ba2: room for one line. 06ee7ba5-06ee7bba:
    // +0x18 / +0x1c / +0x20 = event +0x04 / +0x08 / +0x0c.
    Lines.reserve(1);

    // / 06ee7ee1-06ee7eeb / 06ee8237-06ee8241: the ship's guide (the routine,
    // not null-tested) and its type (slot +0x28, called again for every comparison; a plain
    // getter). Any type but 2, 3, 4 queues nothing (06ee8854).
    int const GuideType = GetShipGuide(Ship)->GetType();
    unsigned int const ShipId = Ship->SpaceObjId;
    FGcsLine* Line = nullptr;
    if (GuideType == 2)
    {
        // Trade.
        switch (Kind)
        {
        case 0:
        {
            // the trade guide's good (the routine again, slot +0x4c); a
            // good and (rand() * 2) / 32768 == 0 (SHL 1, CDQ / AND 0x7fff / ADD / SAR 0xf, NEG /
            // SBB / INC) -> what it heard, else the simple line.
            unsigned int const Good = static_cast<FTradeGuide*>(GetShipGuide(Ship))->GetTradeGood();
            if (Good != 0 && (rand() * 2) / 32768 == 0)
                Line = new FGcsLineFidgetTradeIHear(System, ShipId, 0, Good);
            else
                Line = new FGcsLineFidgetTradeSimple(System, ShipId, 0);
            break;
        }
        case 1:
        {
            // the formation (the routine of +0x154) with more than one
            // member -> the status query; else the simple line (06ee7da9-06ee7e35). The members
            // are freed after the line is appended or deleted.
            std::vector<FContentShipBase*> Members;
            CollectSystemFormationMembers(&Members,
                                          static_cast<FContentShipBase*>(Ship)->FormationId);
            if (Members.size() > 1)
            {
                AddStatusQuery(Members);
                return;
            }
            if (!AddLine(new FGcsLineFidgetTradeSimple(System, ShipId, 0)))
                System = nullptr;
            return;
        }
        case 2:
            // .
            Line = new FGcsLineFidgetTradeRumor(System, ShipId, 0);
            break;
        default:
            return;
        }
    }
    else if (GuideType == 3)
    {
        // Patrol.
        switch (Kind)
        {
        case 0:
            // .
            Line = new FGcsLineFidgetPatrolSimple(System, ShipId, 0);
            break;
        case 1:
        {
            // the formation (the routine) with more than one member.
            std::vector<FContentShipBase*> Members;
            CollectSystemFormationMembers(&Members,
                                          static_cast<FContentShipBase*>(Ship)->FormationId);
            if (Members.size() <= 1)
            {
                // .
                if (!AddLine(new FGcsLineFidgetPatrolSimple(System, ShipId, 0)))
                    System = nullptr;
                return;
            }
            // (rand() * 2) / 32768 != 0 -> the status query (06ee7fad-06ee80c6).
            if ((rand() * 2) / 32768 != 0)
            {
                AddStatusQuery(Members);
                return;
            }
            // the second member asks how many waypoints are left, the
            // leader answers it.
            unsigned int const WingmanId = Members[1]->SpaceObjId;
            if (!AddLine(new FGcsLineFidgetWaypointQuery(System, WingmanId, ShipId)) ||
                !AddLine(new FGcsLineFidgetPatrolStatus(System, ShipId, WingmanId)))
            {
                System = nullptr;
            }
            return;
        }
        case 2:
            // .
            Line = new FGcsLineFidgetPatrolRumor(System, ShipId, 0);
            break;
        default:
            return;
        }
    }
    else if (GuideType == 4)
    {
        // Wander.
        switch (Kind)
        {
        case 0:
            // .
            Line = new FGcsLineFidgetPatrolSimple(System, ShipId, 0);
            break;
        case 1:
        {
            // the formation (the routine) with more than one member -> the
            // status query (06ee84c0-06ee85eb); else the simple line (06ee85f0-06ee87e4,
            // insert(end) inlined).
            std::vector<FContentShipBase*> Members;
            CollectSystemFormationMembers(&Members,
                                          static_cast<FContentShipBase*>(Ship)->FormationId);
            if (Members.size() > 1)
            {
                AddStatusQuery(Members);
                return;
            }
            if (!AddLine(new FGcsLineFidgetPatrolSimple(System, ShipId, 0)))
                System = nullptr;
            return;
        }
        case 2:
            // (insert(end) inlined).
            Line = new FGcsLineFidgetPatrolRumor(System, ShipId, 0);
            break;
        default:
            return;
        }
    }
    else
    {
        return;
    }

    // a line that can play is appended (the routine); otherwise it is
    // deleted and System = null.
    if (!AddLine(Line))
        System = nullptr;
}

bool FGcsFidgetConversation::IsValid()
{
    // .
    return System != nullptr;
}

bool FGcsFidgetConversation::AddStatusQuery(std::vector<FContentShipBase*> const& Members)
{
    // E.g. 06ee7c72-06ee7d44: the leader's query (speaker the ship, listener 0).
    unsigned int const ShipId = Ship->SpaceObjId;
    if (!AddLine(new FGcsLineFidgetStatusQuery(System, ShipId, 0)))
    {
        System = nullptr;
        return false;
    }
    // every member after the first answers the leader (06ee7d49-06ee7d80:
    // the first that cannot play is deleted and System = null).
    for (std::vector<FContentShipBase*>::const_iterator It = Members.begin() + 1;
         It != Members.end(); ++It)
    {
        if (!AddLine(new FGcsLineFidgetStatusReport(System, (*It)->SpaceObjId, ShipId)))
        {
            System = nullptr;
            return false;
        }
    }
    return true;
}

FGcsDockConversation::FGcsDockConversation(FGcsDockGrantedEvent const* Event)
    : System(Event->System)
    , Speaker(Event->Speaker)
    , Listener(Event->Listener)
    , DockIndex(Event->DockIndex)
    , DockType(Event->DockType)
    , Response(Event->Response)
    , bWaitedInQueue(Event->bWaitedInQueue)
{
    // . 06ee6ea4-06ee6f1d: room for two lines.
    Lines.reserve(2);
    unsigned int TargetType = 0;
    pub::SpaceObj::GetType(Speaker, TargetType);

    if (TargetType == 0x400)
        return;

    if (TargetType == 0x40 || TargetType == 0x80 || TargetType == 0x800)
    {
        // (0x80) / 06ee7912-06ee7a9a (0x40, 0x800): only the leader
        // (slot +0x14c 0, or no group) of a SpacePop formation with wingmen speaks.
        FContentSpaceObj* const Object = System->FindSpaceObj(Listener);
        if (Object == nullptr)
            return;
        FContentNpcShip* const Ship = static_cast<FContentNpcShip*>(Object);
        if (!Ship->IsSpacePopShip())
            return;
        // / 06ee7936-06ee7990: the the routine body inlined (EH states
        // 1 / 3): the vector is freed right after the size test, before any line is built.
        if (!HasFormationWingmen(Object))
            return;
        if (Ship->FormationSlot != 0 && Ship->FormationId != 0xffffffffu)
            return;

        FGcsLine* Line = nullptr;
        if (TargetType == 0x80)
        {
            Line = new FGcsLineTradeLaneStart(System, Listener, 0);
        }
        else
        {
            // the gate's target system; response 4 (granted) opens it
            // (after the delay line when the ship waited), any other delays it.
            unsigned int TargetSystem = 0;
            unsigned int TargetObject = 0;
            pub::SpaceObj::GetJumpTarget(Speaker, TargetSystem, TargetObject);
            if (Response == 4)
            {
                if (bWaitedInQueue)
                {
                    if (!AddLine(new FGcsLineJumpGateOpenAfterDelay(System, Listener, 0)))
                    {
                        System = nullptr;
                        return;
                    }
                }
                Line = new FGcsLineJumpGateOpen(System, Listener, 0, TargetSystem);
            }
            else
            {
                Line = new FGcsLineJumpGateDelayed(System, Listener, 0, TargetSystem);
            }
        }
        // .
        if (!AddLine(Line))
            System = nullptr;
        return;
    }

    // any other target (a dock).
    FContentSpaceObj* const Object = System->FindSpaceObj(Listener);
    if (bWaitedInQueue)
    {
        // the ship waited in the queue.
        if (!AddLine(new FGcsLineDockNowCleared(System, Speaker, Listener, DockType)))
            System = nullptr;
        return;
    }

    if (Object != nullptr && static_cast<FContentNpcShip*>(Object)->IsSpacePopShip())
    {
        // a SpacePop ship asks; a formation leader with wingmen first
        // thanks its escort.
        FContentNpcShip* const Ship = static_cast<FContentNpcShip*>(Object);
        // the the routine body inlined (EH state 8), freed before the
        // line new-expressions.
        bool const bWingmen = HasFormationWingmen(Object);
        if (bWingmen &&(Ship->FormationSlot == 0 || Ship->FormationId == 0xffffffffu))
        {
            if (!AddLine(new FGcsLineThisOurStop(System, Listener, 0)))
            {
                System = nullptr;
                return;
            }
        }
        if (!AddLine(new FGcsLineDockRequest(System, Listener, Speaker, DockType)))
        {
            System = nullptr;
            return;
        }
    }

    // the dock's answer by Response (jump table 06ee7ae0); 1 and anything
    // above 4 answer nothing.
    FGcsLine* Answer = nullptr;
    switch (Response)
    {
    case 0:
        Answer = new FGcsLineDockHostile(System, Speaker, Listener);
        break;
    case 2:
        Answer = new FGcsLineDockDenied(System, Speaker, Listener, DockType);
        break;
    case 3:
        Answer = new FGcsLineDockStandby(System, Speaker, Listener);
        break;
    case 4:
        Answer = new FGcsLineDockGranted(System, Speaker, Listener, DockType, DockIndex);
        break;
    default:
        return;
    }
    // a line that cannot play is deleted and clears System.
    if (!AddLine(Answer))
        System = nullptr;
}

FGcsDockHailConversation::FGcsDockHailConversation(FGcsObjectEvent const* Event)
    : System(Event->System)
    , Speaker(Event->Speaker)
    , Listener(Event->Listener)
{
    // . 06ee2040-06ee20b9: room for four lines.
    Lines.reserve(4);

    // the station's attitude towards the ship. A station without a
    // reputation handle (+8) gets no line; the object is not null-tested.
    int ShipRep = 0;
    pub::SpaceObj::GetRep(Listener, ShipRep);
    FContentSpaceObj* const Station = System->FindShipOrSolar(Speaker);
    if (Station->ReputationHandle == nullptr)
        return;
    int StationRep = Station->ReputationHandle->GetReputation();
    float Attitude = 0.0f;
    pub::Reputation::GetAttitude(StationRep, ShipRep, Attitude);

    // the hail.
    if (!AddLine(new FGcsLineDockHail(System, Speaker, Listener, Attitude)))
    {
        System = nullptr;
        return;
    }

    // only a SpacePop ship answers (the routine then the routine, the
    // object not null-tested).
    FContentNpcShip* const Ship = static_cast<FContentNpcShip*>(System->FindSpaceObj(Listener));
    if (!Ship->IsSpacePopShip())
        return;

    // FCOMP the field (-0.6f), TEST AH,0x41 / JNZ: at or below (or
    // unordered) -> the angry answer (06ee2980).
    if (!(Attitude > -0.6f))
    {
        if (!AddLine(new FGcsLineHaileeAngry(System, Listener, Speaker)))
            System = nullptr;
        return;
    }

    // the ship's data, the station's acknowledgement.
    if (!AddLine(new FGcsLineHaileeDataSubmit(System, Listener, Speaker)))
    {
        System = nullptr;
        return;
    }
    if (!AddLine(new FGcsLineDockHailReceived(System, Speaker, Listener)))
    {
        System = nullptr;
        return;
    }

    // by the ship's guide type (the routine).
    switch (Ship->GetGuideType())
    {
    case 2:
    {
        // Trade: home, then what it trades (failing that, where it is going).
        if (!AddLine(new FGcsLineTradeIdHome(System, Listener, Speaker)))
        {
            System = nullptr;
            return;
        }
        FGcsLine* const Status = new FGcsLineTradeStatus(System, Listener, Speaker);
        if (Status->CanPlay())
        {
            Lines.insert(Lines.end(), 1, Status);
            return;
        }
        delete Status;
        if (!AddLine(new FGcsLineGoingTo(System, Listener, Speaker)))
            System = nullptr;
        return;
    }
    case 3:
        // Patrol: home, where it is going, its progress.
        if (!AddLine(new FGcsLinePatrolIdHome(System, Listener, Speaker)) ||
            !AddLine(new FGcsLineGoingTo(System, Listener, Speaker)) ||
            !AddLine(new FGcsLinePatrolStatus(System, Listener, Speaker)))
        {
            System = nullptr;
        }
        return;
    case 4:
        // Wander: home, where it is going.
        if (!AddLine(new FGcsLinePatrolIdHome(System, Listener, Speaker)) ||
            !AddLine(new FGcsLineGoingTo(System, Listener, Speaker)))
        {
            System = nullptr;
        }
        return;
    default:
        return;
    }
}

bool FGcsDockHailConversation::IsValid()
{
    // .
    return System != nullptr && System->FindShip(Listener) != nullptr &&
           System->FindShipOrSolar(Speaker) != nullptr;
}

bool FGcsDockConversation::IsValid()
{
    // .
    return System != nullptr && System->FindShip(Listener) != nullptr &&
           System->FindShipOrSolar(Speaker) != nullptr;
}

// =======================================================================================
// Events
// =======================================================================================

int FGcsDockHailEvent::GetType() const
{
    // .
    return 1;
}

int FGcsPlayerObjectEvent::GetType() const
{
    // .
    return 5;
}

int FGcsCombatEvent::GetType() const
{
    // (ICF `MOV EAX,[ECX+0x10]; RET`, slot 0 of 06fb74c4..06fb74d4).
    return Type;
}

int FGcsScanEvent::GetType() const
{
    // (ICF `MOV EAX,[ECX+8]; RET`, slot 0 of 06fb857c).
    return Type;
}

int FGcsFidgetEvent::GetType() const
{
    // .
    return 0x19;
}

// =======================================================================================
// Conversation manager
// =======================================================================================

FGcsConversationManagerInstance::FGcsConversationManagerInstance()
{
    // Inlined at 06ee8a8b-06ee8a92: MOV EAX,ESI (the derived this); MOV [the field],EAX.
    GGcsConversationManager = static_cast<FGcsConversationManager*>(this);
}

FGcsConversationManagerInstance::~FGcsConversationManagerInstance()
{
    // (inlined at 06ee8bbe).
    GGcsConversationManager = nullptr;
}

FGcsConversationManager::FGcsConversationManager()
{
    // base first (06ee8a92), then +4 = 1, the list (head allocated) and the
    // empty vector (in-class initialisers / member ctors); vptr 06fb7484.
}

FGcsConversationManager::~FGcsConversationManager()
{
    // each conversation deleted and its node unlinked, then the delayed
    // messages freed and the list emptied.
    std::list<FGcsConversation*>::iterator It = Conversations.begin();
    while (It != Conversations.end())
    {
        if (*It != nullptr)
            delete *It;
        It = Conversations.erase(It);
    }
    // DelayedMessages then Conversations are destroyed (unwind states 2
    // and 1), then the base clears the field (state 0).
}

void FGcsConversationManager::QueueMessage(unsigned int const* A, unsigned int const* B,
                                           unsigned int const* C, unsigned int const* D,
                                           float const* Delay)
{
    // vector::insert(end, 1, entry) (reallocation the routine /
    // / the routine).
    FGcsDelayedMessage Entry;
    Entry.Field_0x00 = *A;
    Entry.Field_0x04 = *B;
    Entry.Field_0x08 = *C;
    Entry.Field_0x0c = *D;
    Entry.Elapsed = 0.0f;
    Entry.Delay = *Delay;
    DelayedMessages.insert(DelayedMessages.end(), 1, Entry);
}

bool FGcsConversationManager::Add(FRmConversationBase* Conversation)
{
    // (RET 4). 06ee9143-06ee9149: +4 clear -> false.
    if (!bEnabled)
        return false;
    // vt[+0x14] (Clone) of the argument, then the clone's vt[+8] (IsValid);
    // the clone is not null-tested.
    FRmConversationBase* const Clone = Conversation->Clone();
    if (!Clone->IsValid())
    {
        // vt[0](1).
        delete Clone;
        return false;
    }
    // vt[+4] (Start), then a node with the clone before the list head
    // (push_back), ++size.
    Clone->Start();
    Conversations.push_back(Clone);
    return true;
}

void FGcsConversationManager::OnMessage(void const* Parms)
{
    // .
    if (!bEnabled)
        return;
    for (std::list<FGcsConversation*>::iterator It = Conversations.begin();
         It != Conversations.end(); ++It)
    {
        if (!(*It)->OnMessage(Parms))
            continue;
        if ((*It)->IsFinished())
        {
            if (*It != nullptr)
                delete *It;
            Conversations.erase(It);
        }
        return;
    }
}

void FGcsConversationManager::Update(float DeltaTime)
{
    // .
    if (pub::SinglePlayer())
        return;

    std::vector<FGcsSentMessage> Due;
    std::vector<FGcsDelayedMessage>::iterator It = DelayedMessages.begin();
    while (It != DelayedMessages.end())
    {
        // FLD dt / FADD +0x10 / FST +0x10 / FCOMP +0x14, TEST AH,1 +
        // JNZ: C0 (sum < delay) keeps waiting; the unrounded sum is compared.
        double const Sum = static_cast<double>(DeltaTime) + static_cast<double>(It->Elapsed);
        It->Elapsed = static_cast<float>(Sum);
        if (Sum < static_cast<double>(It->Delay))
        {
            ++It;
            continue;
        }
        // words {+8, +0, +4, +0xc}; the fifth is not written.
        FGcsSentMessage Message;
        Message.Words[0] = It->Field_0x08;
        Message.Words[1] = It->Field_0x00;
        Message.Words[2] = It->Field_0x04;
        Message.Words[3] = It->Field_0x0c;
        Message.Words[4] = 0;
        Due.insert(Due.end(), 1, Message);
        It = DelayedMessages.erase(It);
    }

    // .
    for (std::vector<FGcsSentMessage>::iterator Send = Due.begin(); Send != Due.end(); ++Send)
        pub::Controller::_SendMessage(GSpaceSupportControllerId, pub::Controller::EMessage::CommComplete,
                                      &*Send);
}

float FGcsConversationManager::QueueConversation(FGcsEvent const* Event)
{
    // . The result is 0.0f (the field) whenever no conversation is queued.
    if (!bEnabled)
        return 0.0f;

    int const Type = Event->GetType();
    FGcsConversation* Conversation = nullptr;
    switch (Type)
    {
    case 1:
    case 2:
    case 3:
    {
        // / 06ee8e15-06ee8e3f / 06ee8e6f-06ee8e99: the admission test
        // on {System, +8, +0xc}; type 2 records both objects, types 1 and 3 the speaker
        // only.
        FGcsObjectEvent const* ObjectEvent = static_cast<FGcsObjectEvent const*>(Event);
        if (!IsGcsCommAllowed(Type, ObjectEvent->System, ObjectEvent->Speaker,
                              ObjectEvent->Listener))
            return 0.0f;
        RecordGcsComm(Type, ObjectEvent->System, ObjectEvent->Speaker,
                      Type == 2 ? ObjectEvent->Listener : 0);
        if (Type == 3)
        {
            // .
            Conversation =
                new FGcsLaunchConversation(static_cast<FGcsLaunchEvent const*>(Event));
        }
        else if (Type == 2)
        {
            // .
            Conversation =
                new FGcsDockConversation(static_cast<FGcsDockGrantedEvent const*>(Event));
        }
        else
        {
            // .
            Conversation = new FGcsDockHailConversation(ObjectEvent);
        }
        break;
    }
    case 5:
        // no admission test.
        Conversation = new FGcsPlayerHailConversation(
            static_cast<FGcsPlayerObjectEvent const*>(Event));
        break;
    case 6:
    case 7:
    case 8:
    case 9:
    case 0xa:
    case 0xb:
    case 0xc:
    case 0xd:
    case 0xe:
    case 0xf:
    case 0x10:
        // / 06ee8f76-06ee8f98 (type 7): no admission test here (FGcsHandler1
        // tests and records before queueing).
        Conversation =
            new FGcsCombatConversation(static_cast<FGcsCombatEvent const*>(Event));
        break;
    case 0x11:
    case 0x12:
    case 0x13:
    case 0x14:
    case 0x15:
    case 0x16:
    case 0x17:
    case 0x18:
        // (the valid / invalid tail 06ee9040-06ee90f8 is the common one's).
        Conversation = new FGcsScanConversation(static_cast<FGcsScanEvent const*>(Event));
        break;
    case 0x19:
        // caseD_19 06ee907d-06ee90a1 (its own copy of the tail, 06ee90a5-06ee90f8).
        Conversation = new FGcsFidgetConversation(static_cast<FGcsFidgetEvent const*>(Event));
        break;
    default:
        // Type 4 and anything outside 1..0x19.
        return 0.0f;
    }

    // (06ee9040-06ee90f8 for 0x11..0x19): an invalid conversation is
    // deleted; a valid one is started and appended.
    if (!Conversation->IsValid())
    {
        delete Conversation;
        return 0.0f;
    }
    Conversation->Start();
    Conversations.push_back(Conversation);
    if (Type == 7)
    {
        // the first line's +0x10 is the result (no FLD 1.0).
        return Conversations.back()->Lines.front()->GetDuration();
    }
    return 1.0f;
}

// =======================================================================================
// Handlers
// =======================================================================================

FGcsHandler* __cdecl GetGcsHandler(int const* Type)
{
    // find, else create and insert. baseline reads the value back through a
    // second insert of {*Type, 0}, which returns the existing node.
    switch (*Type)
    {
    case 0:
    case 1:
    case 2:
        break;
    default:
        return nullptr;
    }

    std::map<int, FGcsHandler*>::iterator It = GGcsHandlerMap.find(*Type);
    if (It != GGcsHandlerMap.end())
        return It->second;

    FGcsHandler* Handler = nullptr;
    if (*Type == 2)
        Handler = new FGcsHandler2();            // new 4, vtable 06fb74ac
    else if (*Type == 1)
        Handler = new FGcsHandler1();            // new 4, vtable 06fb74a0
    else
        Handler = new FGcsDockHailHandler();     // new 0x24, vtable 06fb7488
    GGcsHandlerMap.insert(std::make_pair(*Type, static_cast<FGcsHandler*>(nullptr)))
        .first->second = Handler;
    return Handler;
}

FGcsHandlerSet::~FGcsHandlerSet()
{
    // .
    while (!Handlers.empty())
    {
        if (Handlers.front() != nullptr)
            delete Handlers.front();
        Handlers.erase(Handlers.begin());
    }
}

void FGcsHandlerSet::HandleMessage(pub::Controller::EMessage MessageId, void const* Parms)
{
    // .
    if (MessageId != pub::Controller::EMessage::Hail)
    {
        for (std::vector<FGcsHandler*>::iterator It = Handlers.begin(); It != Handlers.end();
             ++It)
        {
            (*It)->HandleMessage(MessageId, Parms);
        }
        return;
    }

    // {ship, object, system}.
    unsigned int const* Words = static_cast<unsigned int const*>(Parms);
    FGcsPlayerObjectEvent Event;
    Event.System = FindSystemInfo(&Words[2]);
    Event.Player = FindContentPlayerStateByShip(&Words[0]);
    Event.Object = Event.System->FindSpaceObj(Words[1]);
    if (Event.Player != nullptr && Event.Object != nullptr)
        GGcsConversations.QueueConversation(&Event);
}

void FGcsHandlerSet::AddHandler(int const* Type)
{
    // vector::insert(end, 1, GetGcsHandler(Type)).
    FGcsHandler* const Handler = GetGcsHandler(Type);
    Handlers.insert(Handlers.end(), 1, Handler);
}

// ---------------------------------------------------------------------------------------
// Dock hail handler
// ---------------------------------------------------------------------------------------

FGcsDockHailHandler::~FGcsDockHailHandler()
{
    // OnSystemUnloaded(&the field (0)); the list and the map follow.
    unsigned int const AllSystems = 0;
    OnSystemUnloaded(&AllSystems);
}

int FGcsDockHailHandler::GetType() const
{
    // XOR EAX,EAX.
    return 0;
}

void FGcsDockHailHandler::SuppressHail(unsigned int const* Ship, void* const* Phantom,
                                       int Delay)
{
    // .
    RecentHails.push_back(std::make_pair(*Phantom, *Ship));
    pub::Controller::TimerExpired Timer;
    Timer.Param1 = GGcsDockHailTimerId;
    Timer.Param2 = static_cast<unsigned int>(reinterpret_cast<std::uintptr_t>(&RecentHails.back()));
    pub::SetTimer(GSpaceSupportControllerId, Timer, static_cast<float>(Delay));
}

void FGcsDockHailHandler::HandleMessage(pub::Controller::EMessage MessageId, void const* Parms)
{
    using pub::Controller::EMessage;
    // id - 0x2d over a byte table (0x2d, 0x3c, 0x3d, 0x3e; the rest nothing).
    switch (MessageId)
    {
    case EMessage::TimerExpired:
    {
        // our timer drops the oldest recent hail.
        pub::Controller::TimerExpired const* Timer =
            static_cast<pub::Controller::TimerExpired const*>(Parms);
        if (Timer->Param1 == GGcsDockHailTimerId)
            RecentHails.erase(RecentHails.begin());
        return;
    }
    case EMessage::SystemLoaded:
        OnSystemLoaded(static_cast<unsigned int const*>(Parms));
        return;
    case EMessage::SystemUnloaded:
        OnSystemUnloaded(static_cast<unsigned int const*>(Parms));
        return;
    case EMessage::PhantomEnter:
        break;
    default:
        return;
    }

    // a ship touched a phantom.
    FGcsPhantomTouch const* Touch = static_cast<FGcsPhantomTouch const*>(Parms);
    std::map<unsigned int, std::map<void*, unsigned int>>::iterator SystemIt =
        Phantoms.find(Touch->System);
    if (SystemIt == Phantoms.end())
        return;

    // a recent hail of this ship by this phantom (or by any) wins.
    for (std::list<std::pair<void*, unsigned int>>::iterator It = RecentHails.begin();
         It != RecentHails.end(); ++It)
    {
        if ((It->first == Touch->Phantom || It->first == nullptr) && It->second == Touch->Ship)
            return;
    }

    std::map<void*, unsigned int>::iterator PhantomIt = SystemIt->second.find(Touch->Phantom);
    if (PhantomIt == SystemIt->second.end())
        return;

    // remembered for 30 s (0x41f00000).
    RecentHails.push_back(std::make_pair(Touch->Phantom, Touch->Ship));
    pub::Controller::TimerExpired Timer;
    Timer.Param1 = GGcsDockHailTimerId;
    Timer.Param2 = static_cast<unsigned int>(reinterpret_cast<std::uintptr_t>(&RecentHails.back()));
    pub::SetTimer(GSpaceSupportControllerId, Timer, 30.0f);

    // only a SpacePop NPC ship outside a formation group is hailed
    // (neither the system record nor the object is tested beyond this).
    FGcsDockHailEvent Event;
    Event.System = FindSystemInfo(&Touch->System);
    FContentSpaceObj* const Object = Event.System->FindSpaceObj(Touch->Ship);
    if (Object == nullptr)
        return;
    FContentNpcShip* const Ship = static_cast<FContentNpcShip*>(Object);
    if (Ship->FormationSlot != 0 && Ship->FormationId != 0xffffffffu)
        return;
    if (!Ship->IsSpacePopShip())
        return;
    Event.Speaker = PhantomIt->second;
    Event.Listener = Touch->Ship;
    GGcsConversations.QueueConversation(&Event);
}

void FGcsDockHailHandler::OnSystemLoaded(unsigned int const* SystemId)
{
    // every station / docking object of the system (pub::System::ScanObjects,
    // 0x40 at most, radius -1, mask 0x200100).
    unsigned int Objects[0x40];
    unsigned int Count = 0;
    Vector Center;
    Center.x = 0.0f;
    Center.y = 0.0f;
    Center.z = 0.0f;
    pub::System::ScanObjects(*SystemId, Objects, 0x40, Center, -1.0f, 0x200100, Count);
    FindSystemInfo(SystemId);   // result unused

    for (unsigned int Index = 0; Index < Count; ++Index)
    {
        // the type must carry 0x200100 and not 2; the docking target's
        // base record must have voice data (CommReferrable slot 0).
        unsigned int Type = 0;
        pub::SpaceObj::GetType(Objects[Index], Type);
        if ((Type & 0x200100) == 0 || (Type & 2) != 0)
            continue;
        unsigned int BaseId = 0;
        pub::SpaceObj::GetDockingTarget(Objects[Index], BaseId);
        FContentBase* const Base = FindContentBaseInfo(&BaseId);
        if (Base == nullptr || !Base->HasVoiceInfo())
            continue;

        // a 2500 m phantom (flags 0x7f0000) at the object, activated.
        Vector Position;
        Matrix Orientation;
        pub::SpaceObj::GetLocation(Objects[Index], Position, Orientation);
        void* Phantom = nullptr;
        pub::Phantom::Create(*SystemId, 2500.0f, Position, 0x7f0000, Phantom);
        pub::Phantom::SetActive(Phantom, true);

        // Phantoms[*SystemId] gets {Phantom -> object}; an existing key
        // gets the new object.
        Phantoms[*SystemId].insert(std::make_pair(Phantom, 0u)).first->second = Objects[Index];
    }
}

void FGcsDockHailHandler::OnSystemUnloaded(unsigned int const* SystemId)
{
    // .
    if (*SystemId != 0)
    {
        // the system's phantoms destroyed; its (empty) entry stays.
        std::map<unsigned int, std::map<void*, unsigned int>>::iterator SystemIt =
            Phantoms.find(*SystemId);
        if (SystemIt == Phantoms.end())
            return;
        std::map<void*, unsigned int>::iterator It = SystemIt->second.begin();
        while (It != SystemIt->second.end())
        {
            pub::Phantom::Destroy(It->first);
            It = SystemIt->second.erase(It);
        }
        return;
    }

    // every system.
    std::map<unsigned int, std::map<void*, unsigned int>>::iterator SystemIt = Phantoms.begin();
    while (SystemIt != Phantoms.end())
    {
        std::map<void*, unsigned int>::iterator It = SystemIt->second.begin();
        while (It != SystemIt->second.end())
        {
            pub::Phantom::Destroy(It->first);
            It = SystemIt->second.erase(It);
        }
        SystemIt = Phantoms.erase(SystemIt);
    }
}

// ---------------------------------------------------------------------------------------
// Handlers 1 and 2
// ---------------------------------------------------------------------------------------

void FGcsHandler1::HandleMessage(pub::Controller::EMessage MessageId, void const* Parms)
{
    using pub::Controller::EMessage;
    // (06eea160-06eea821): the combat messages become combat events (types
    // 6..0x10, built on the stack) once IsGcsCommAllowed passes; each is recorded
    // (RecordGcsComm) and queued. Every other message id does nothing.
    unsigned int const* const Words = static_cast<unsigned int const*>(Parms);
    // Two locals live in the Parms argument slot ([ESP+0x40]): GetType (0x1001) and
    // GetRelativeHealth (0xd) write there, so a failing call leaves the pointer's bits.
    unsigned int const ParmsBits =
        static_cast<unsigned int>(reinterpret_cast<std::uintptr_t>(Parms));
    switch (MessageId)
    {
    case EMessage::DeathFuseLit:
    {
        // {system, ship, ?, out float}: a ship died -- the victim's side
        // speaks (type 7, A = 0); the conversation's result goes to word 3.
        FSystemInfo* const System = FindSystemInfo(&Words[0]);
        if (System->FindShip(Words[1]) == nullptr)
            return;
        if (!IsGcsCommAllowed(7, System, 0, Words[1]))
            return;
        FGcsCombatEvent Event;
        Event.System = System;
        Event.ObjectA = 0;
        Event.ObjectB = Words[1];
        Event.Type = 7;
        float const Duration = GGcsConversations.QueueConversation(&Event);
        // FSTP float [ESI+0xc] into the sender's parameter block.
        *reinterpret_cast<float*>(const_cast<unsigned int*>(&Words[3])) = Duration;
        RecordGcsComm(7, System, 0, Words[1]);
        return;
    }
    case EMessage::ObjectDamaged:
    {
        // {victim, system, attacker, ?, ?, kind, amount}: damage.
        // FCOMP 0.0f, TEST AH,0x41 / JNZ: only an amount above 0 (ordered).
        float const Amount = *reinterpret_cast<float const*>(&Words[6]);
        if (!(Amount > 0.0f))
            return;
        int const Kind = static_cast<int>(Words[5]);
        if (Kind < 2 || (Kind > 5 && Kind != 7))
            return;
        FSystemInfo* const System = FindSystemInfo(&Words[1]);
        unsigned int VictimType = 0;
        if (pub::SpaceObj::GetType(Words[0], VictimType) != 0)
            return;
        if (!IsGcsCommAllowed(6, System, Words[2], Words[0]))
            return;
        // the victim's relative health (status not tested) below 1.0
        // (FCOMP the field, TEST AH,5 / JP: C0 alone).
        float Health = 0.0f;
        std::memcpy(&Health, &ParmsBits, sizeof(Health));
        pub::SpaceObj::GetRelativeHealth(Words[0], Health);
        if (!(Health < 1.0f))
            return;
        RecordGcsComm(6, System, Words[2], Words[0]);
        FGcsCombatDamageEvent Event;
        Event.System = System;
        Event.ObjectA = Words[2];
        Event.ObjectB = Words[0];
        Event.Type = 6;
        Event.Health = Health;
        GGcsConversations.QueueConversation(&Event);
        return;
    }
    case EMessage::ObjectDestroyed:
    {
        // {victim, killer, system, ?, flag}: with flag 1 the killer brags
        // (type 7, B = 0) -- unless it is a player whose group feelings towards the NPC
        // victim's faction are at least -0.2 (the field; FCOMP, TEST AH,1 / JZ).
        if (Words[4] != 1)
            return;
        FSystemInfo* const System = FindSystemInfo(&Words[2]);
        FContentSpaceObj* const Killer = System->FindShip(Words[1]);
        FContentSpaceObj* const Victim = System->FindShip(Words[0]);
        if (Killer == nullptr)
            return;
        if (Victim != nullptr)
        {
            unsigned int KillerId = Killer->SpaceObjId;
            unsigned int VictimId = Victim->SpaceObjId;
            if (FindContentPlayerStateByShip(&KillerId) != nullptr &&
                FindContentPlayerStateByShip(&VictimId) == nullptr)
            {
                // the out value starts at 0 (EAX of the lookup).
                float Feelings = 0.0f;
                unsigned int const VictimGroup = Victim->ReputationHandle->GetAffiliation();
                int const KillerRep = Killer->ReputationHandle->GetReputation();
                pub::Reputation::GetGroupFeelingsTowards(KillerRep, VictimGroup, Feelings);
                if (Feelings >= -0.2f)
                    return;
            }
        }
        if (!IsGcsCommAllowed(7, System, Words[1], Words[0]))
            return;
        FGcsCombatEvent Event;
        Event.System = System;
        Event.ObjectA = Words[1];
        Event.ObjectB = 0;
        Event.Type = 7;
        GGcsConversations.QueueConversation(&Event);
        RecordGcsComm(7, System, Words[1], Words[0]);
        return;
    }
    case EMessage::CombatState:
    {
        // {kind, A, B, system}: 0 engage (9), 1 / 2 no engage (0xb), 3 / 4
        // combat over (0xa); anything else nothing.
        FSystemInfo* const System = FindSystemInfo(&Words[3]);
        switch (Words[0])
        {
        case 0:
        {
            // .
            if (!IsGcsCommAllowed(9, System, Words[1], Words[2]))
                return;
            RecordGcsComm(9, System, Words[1], Words[2]);
            FGcsCombatEvent Event;
            Event.System = System;
            Event.ObjectA = Words[1];
            Event.ObjectB = Words[2];
            Event.Type = 9;
            GGcsConversations.QueueConversation(&Event);
            return;
        }
        case 1:
        case 2:
        {
            // only a ship (type & 0x7f0000; GetType's status not tested).
            if (!IsGcsCommAllowed(0xb, System, Words[1], Words[2]))
                return;
            unsigned int Type = ParmsBits;
            pub::SpaceObj::GetType(Words[1], Type);
            if ((Type & 0x7f0000) == 0)
                return;
            RecordGcsComm(0xb, System, Words[1], Words[2]);
            FGcsCombatNoEngageEvent Event;
            Event.System = System;
            Event.ObjectA = Words[1];
            Event.ObjectB = Words[2];
            Event.Type = 0xb;
            Event.Reason = static_cast<int>(Words[0]);
            GGcsConversations.QueueConversation(&Event);
            return;
        }
        case 3:
        case 4:
        {
            // .
            if (!IsGcsCommAllowed(0xa, System, Words[1], Words[2]))
                return;
            RecordGcsComm(0xa, System, Words[1], Words[2]);
            FGcsCombatOverEvent Event;
            Event.System = System;
            Event.ObjectA = Words[1];
            Event.ObjectB = Words[2];
            Event.Type = 0xa;
            Event.Reason = static_cast<int>(Words[0]);
            GGcsConversations.QueueConversation(&Event);
            return;
        }
        default:
            return;
        }
    }
    case EMessage::CombatDrift:
    {
        // {ship, system}: combat drift (0xd).
        FSystemInfo* const System = FindSystemInfo(&Words[1]);
        if (!IsGcsCommAllowed(0xd, System, Words[0], 0))
            return;
        RecordGcsComm(0xd, System, Words[0], 0);
        FGcsCombatEvent Event;
        Event.System = System;
        Event.ObjectA = Words[0];
        Event.ObjectB = 0;
        Event.Type = 0xd;
        GGcsConversations.QueueConversation(&Event);
        return;
    }
    // id - 0x1004 over the byte table 06eea84c into the jump table
    // 0x1004 -> [0] 06eea6a9, 0x1005 -> [1] 06eea713, 0x1009 -> [2] 06eea779,
    // 0x100e -> [3] 06eea643, every other index -> [4] (nothing). 0x1003 alone is the JZ at
    // to 06eea5e5.
    case EMessage::NpcTurnedHostile:
    case EMessage::ShipInSights:
    {
        // (0x1003: 0xc, help -- the routine) / 06eea643-06eea67b (0x100e:
        // 8, in sights -- ship_in_sights_event 06f34000), then 06eea683-06eea6a4:
        // {A, B, system}, all ids.
        int const Type = MessageId == EMessage::NpcTurnedHostile ? 0xc : 8;
        FSystemInfo* const System = FindSystemInfo(&Words[2]);
        if (!IsGcsCommAllowed(Type, System, Words[0], Words[1]))
            return;
        RecordGcsComm(Type, System, Words[0], Words[1]);
        FGcsCombatEvent Event;
        Event.System = System;
        Event.ObjectA = Words[0];
        Event.ObjectB = Words[1];
        Event.Type = Type;
        GGcsConversations.QueueConversation(&Event);
        return;
    }
    case EMessage::FleeRequest:
    {
        // {ship, reason, system}: flight (0xe) -- request_flee_destination
        // .
        FSystemInfo* const System = FindSystemInfo(&Words[2]);
        if (!IsGcsCommAllowed(0xe, System, Words[0], 0))
            return;
        RecordGcsComm(0xe, System, Words[0], 0);
        FGcsCombatFleeEvent Event;
        Event.System = System;
        Event.ObjectA = Words[0];
        Event.ObjectB = 0;
        Event.Type = 0xe;
        Event.Reason = static_cast<int>(Words[1]);
        GGcsConversations.QueueConversation(&Event);
        return;
    }
    case EMessage::DamageReputation:
    {
        // {0, A, B, system}: friendly fire (0xf) -- the routine's damage
        // reputation message; a non-zero word 0 does nothing.
        if (Words[0] != 0)
            return;
        FSystemInfo* const System = FindSystemInfo(&Words[3]);
        if (!IsGcsCommAllowed(0xf, System, Words[1], Words[2]))
            return;
        RecordGcsComm(0xf, System, Words[1], Words[2]);
        FGcsCombatEvent Event;
        Event.System = System;
        Event.ObjectA = Words[1];
        Event.ObjectB = Words[2];
        Event.Type = 0xf;
        GGcsConversations.QueueConversation(&Event);
        return;
    }
    case EMessage::FormationLeaderChanged:
    {
        // {new leader, old leader} as ship objects (06f5e7eb-06f5e802): a
        // SpacePop ship with wingmen takes the lead (0x10); its system from the object
        // (the routine).
        FContentShipBase* const* const Ships = static_cast<FContentShipBase* const*>(Parms);
        FSystemInfo* const System = Ships[0]->GetSystem();
        if (!static_cast<FContentNpcShip*>(Ships[0])->IsSpacePopShip())
            return;
        if (!HasFormationWingmen(Ships[0]))
            return;
        if (!IsGcsCommAllowed(0x10, System, Ships[0]->SpaceObjId, Ships[1]->SpaceObjId))
            return;
        RecordGcsComm(0x10, System, Ships[0]->SpaceObjId, Ships[1]->SpaceObjId);
        FGcsCombatEvent Event;
        Event.System = System;
        Event.ObjectA = Ships[0]->SpaceObjId;
        Event.ObjectB = Ships[1]->SpaceObjId;
        Event.Type = 0x10;
        GGcsConversations.QueueConversation(&Event);
        return;
    }
    default:
        return;
    }
}

int FGcsHandler1::GetType() const
{
    // .
    return 1;
}

namespace
{
    // The squared length the fidget handler sums on the x87 stack (06eeab7a-06eeab9a /
    // ): z*z + y*y + x*x of a float difference (kept here in double).
    double FidgetLengthSquared(Vector const& Difference)
    {
        return static_cast<double>(Difference.z) * Difference.z +
               static_cast<double>(Difference.y) * Difference.y +
               static_cast<double>(Difference.x) * Difference.x;
    }

    // FILD rand() / FMUL the field (2^-15) / FCOMP float Chance, TEST AH,5 / JP: true when
    // C0 alone is set (below).
    bool IsRandBelow(float Chance)
    {
        double const Roll = static_cast<double>(rand()) * (1.0 / 32768.0);
        return Roll < static_cast<double>(Chance);
    }
}

// (static initializer 06eea880: 0x4abebc20).
float GGcsFidgetRangeSquared = 6250000.0f;

void FGcsHandler2::HandleMessage(pub::Controller::EMessage MessageId, void const* Parms)
{
    // (06eea890-06eeaedb). Parms is never read.
    (void)Parms;
    if (MessageId != pub::Controller::EMessage::Heartbeat)
        return;

    // the players handled this tick (a local vector, the routine find).
    std::vector<FContentPlayerState*> Handled;
    std::map<unsigned int, std::unique_ptr<FContentPlayerState>>& Players =
        GetContentPlayerStates();
    for (std::map<unsigned int, std::unique_ptr<FContentPlayerState>>::iterator PlayerIt =
             Players.begin();
         PlayerIt != Players.end(); ++PlayerIt)
    {
        // a player with a ship not handled yet.
        FContentPlayerState* const Player = PlayerIt->second.get();
        if (Player->ShipObject == nullptr)
            continue;
        if (std::find(Handled.begin(), Handled.end(), Player) != Handled.end())
            continue;
        // the field (1/120).
        if (!IsRandBelow(0.0083333338f))
            continue;
        // the admission test {0x19, the ship's system, 0, the ship}.
        FContentPlayerShip* const PlayerShip = Player->ShipObject;
        if (!IsGcsCommAllowed(0x19, PlayerShip->GetSystem(), 0, PlayerShip->SpaceObjId))
            continue;

        // the system's space objects copied into a local list (the routine
        // head; the assignment inlined with the routine / the routine).
        FSystemInfo* const System = PlayerShip->GetSystem();
        std::list<FContentSpaceObj*> Objects;
        Objects = System->SpaceObjs;

        // the candidates -- SpacePop ships (the routine) that lead their
        // formation (+0x14c == 0) or fly without one (+0x154 == -1) and are not busy (slot
        // +8); room for all of them first (the routine capacity test).
        std::vector<FContentShipBase*> Candidates;
        Candidates.reserve(Objects.size());
        for (std::list<FContentSpaceObj*>::iterator ObjIt = Objects.begin(); ObjIt != Objects.end();
             ++ObjIt)
        {
            if (!static_cast<FContentNpcShip*>(*ObjIt)->IsSpacePopShip())
                continue;
            FContentShipBase* const Candidate = static_cast<FContentShipBase*>(*ObjIt);
            if (Candidate->FormationSlot != 0 && Candidate->FormationId != 0xffffffffu)
                continue;
            if (Candidate->Slot08())
                continue;
            Candidates.insert(Candidates.end(), 1, Candidate);
        }

        // the player's position; no speaker yet (+0x30 local = 0).
        Vector PlayerPosition;
        PlayerShip->GetPosition(&PlayerPosition);
        FContentShipBase* Speaker = nullptr;
        // random candidates until one speaks or none is left.
        while (!Candidates.empty())
        {
            // (rand() * n) / 32768 (IMUL, CDQ / AND 0x7fff / ADD / SAR 0xf).
            int const Count = static_cast<int>(Candidates.size());
            int const Index = (rand() * Count) / 32768;
            std::vector<FContentShipBase*>::iterator Pick = Candidates.begin() + Index;
            Vector PickPosition;
            (*Pick)->GetPosition(&PickPosition);
            // within 2500 m of the player (TEST AH,5 / JP: C0 alone).
            // the routine (SubtractVectors) for the difference.
            if (FidgetLengthSquared(SubtractVectors(PlayerPosition, PickPosition)) <
                static_cast<double>(GGcsFidgetRangeSquared))
            {
                // no member of its formation (the routine) is busy. The
                // members (EH state 3) are freed at 06eeac18-06eeac20, before the flag is
                // tested and before the players walk below.
                bool bBusy = false;
                {
                    std::vector<FContentShipBase*> Members;
                    CollectFormationMembers(&Members, (*Pick)->FormationId);
                    for (std::vector<FContentShipBase*>::iterator Member = Members.begin();
                         Member != Members.end(); ++Member)
                    {
                        if ((*Member)->Slot08())
                        {
                            bBusy = true;
                            break;
                        }
                    }
                }
                if (!bBusy)
                {
                    // it speaks; every player whose ship is within 2500 m of
                    // it counts as handled (the redundant second find at 06eeace8 and its pair
                    // result are not reproduced).
                    Speaker = *Pick;
                    for (std::map<unsigned int, std::unique_ptr<FContentPlayerState>>::iterator
                             OtherIt = Players.begin();
                         OtherIt != Players.end(); ++OtherIt)
                    {
                        FContentPlayerState* const Other = OtherIt->second.get();
                        if (std::find(Handled.begin(), Handled.end(), Other) != Handled.end())
                            continue;
                        if (Other->ShipObject == nullptr)
                            continue;
                        Vector OtherPosition;
                        Other->ShipObject->GetPosition(&OtherPosition);
                        // here the difference is inline (FSUB / FSTP float
                        // per component, then the Vector ctor the routine).
                        Vector Difference;
                        Difference.x = OtherPosition.x - PickPosition.x;
                        Difference.y = OtherPosition.y - PickPosition.y;
                        Difference.z = OtherPosition.z - PickPosition.z;
                        if (!(FidgetLengthSquared(Difference) <
                              static_cast<double>(GGcsFidgetRangeSquared)))
                            continue;
                        Handled.insert(Handled.end(), 1, Other);
                    }
                    break;
                }
            }
            // too far or busy -- the pick is erased.
            Candidates.erase(Pick);
        }

        // nobody speaks.
        if (Speaker == nullptr)
            continue;

        // recorded as {0x19, the player ship's system, speaker, player ship}.
        RecordGcsComm(0x19, PlayerShip->GetSystem(), Speaker->SpaceObjId, PlayerShip->SpaceObjId);
        // the event on the stack; Kind 2 below the field (1/30), else 1
        // below the field (0.2) of a second roll, else 0. The result is dropped (FSTP ST0).
        FGcsFidgetEvent Event;
        Event.System = System;
        Event.Ship = Speaker;
        Event.Kind = 0;
        if (IsRandBelow(0.033333335f))
        {
            Event.Kind = 2;
        }
        else
        {
            Event.Kind = 1;
            if (!IsRandBelow(0.2f))
                Event.Kind = 0;
        }
        GGcsConversations.QueueConversation(&Event);
    }
}

int FGcsHandler2::GetType() const
{
    // .
    return 2;
}

// =======================================================================================
// Admission
// =======================================================================================

void __cdecl SetGcsDefaultInterval(double Interval)
{
    // .
    GGcsDefaultInterval = Interval;
}

void __cdecl SetGcsCategoryInterval(int Category, float Interval)
{
    // _Tree::insert (unique keys; an existing key is kept).
    GGcsCategoryIntervals.insert(std::make_pair(Category, Interval));
}

void __cdecl SetGcsCategoryChance(int Category, float Chance)
{
    // .
    GGcsCategoryChances.insert(std::make_pair(Category, Chance));
}

void __cdecl EnableGcsSpeakerCategory(int Category)
{
    // .
    GGcsSpeakerCategories.insert(std::make_pair(Category, true));
}

void __cdecl EnableGcsListenerCategory(int Category)
{
    // .
    GGcsListenerCategories.insert(std::make_pair(Category, true));
}

bool __cdecl IsGcsCommAllowed(int Category, FSystemInfo* System, unsigned int Speaker,
                              unsigned int Listener)
{
    // . 06eebd19: the field == 0 (gcs_clamp on) refuses everything.
    if (ContentIsGcsClampEnabled())
        return false;

    // a speaker / listener that is not a SpacePop ship needs its
    // category opened by EnableGcsSpeakerCategory / EnableGcsListenerCategory.
    FContentSpaceObj* Object = System->FindSpaceObj(Speaker);
    if (Object != nullptr && !static_cast<FContentNpcShip*>(Object)->IsSpacePopShip() &&
        GGcsSpeakerCategories.find(Category) == GGcsSpeakerCategories.end())
    {
        return false;
    }
    Object = System->FindSpaceObj(Listener);
    if (Object != nullptr && !static_cast<FContentNpcShip*>(Object)->IsSpacePopShip() &&
        GGcsListenerCategories.find(Category) == GGcsListenerCategories.end())
    {
        return false;
    }

    // categories 1..4 need the speaker (ship or solar NPC) with slot 1
    // false; a player listener is always allowed.
    if (Category > 0 && Category <= 4)
    {
        FContentSpaceObj* const SpeakerObject = System->FindShipOrSolar(Speaker);
        if (SpeakerObject == nullptr)
            return false;
        if (SpeakerObject->Slot04())                                        // vt[+4]
            return false;
        if (FindContentPlayerStateByShip(&Listener) != nullptr)
            return true;
    }

    // .
    double const Now = GetContentGameTime();
    Vector Position;
    Matrix Orientation;
    if (pub::SpaceObj::GetLocation(Speaker != 0 ? Speaker : Listener, Position, Orientation) != 0)
        return false;

    // the newest remembered comm of the category within 10 km
    // (squared distance below the field = 1e8; TEST AH,5 / JNP: C0 only = below).
    std::list<FGcsRecentComm>& Recent = GGcsRecentComms[Category];
    std::list<FGcsRecentComm>::iterator Near = Recent.begin();
    for (; Near != Recent.end(); ++Near)
    {
        float const DeltaX = Near->Position.x - Position.x;
        float const DeltaY = Near->Position.y - Position.y;
        float const DeltaZ = Near->Position.z - Position.z;
        double const LengthSquared = static_cast<double>(DeltaZ) * DeltaZ +
                                     static_cast<double>(DeltaY) * DeltaY +
                                     static_cast<double>(DeltaX) * DeltaX;
        if (LengthSquared < 100000000.0)
            break;
    }

    float Chance = 1.0f;
    if (Near != Recent.end())
    {
        // too soon after it (interval above the time since) refuses;
        // TEST AH,0x41 / JZ: neither C0 nor C3 = interval greater.
        double const RecentTime = static_cast<double>(Near->Time);
        std::map<int, float>::iterator Interval = GGcsCategoryIntervals.find(Category);
        float const MinInterval = Interval != GGcsCategoryIntervals.end()
                                      ? Interval->second
                                      : static_cast<float>(GGcsDefaultInterval);
        double const Since = GetContentTimeDistance(&Now, &RecentTime);
        if (static_cast<double>(MinInterval) > Since)
            return false;
    }
    else
    {
        // an interval of exactly -1 (the field) turns the category
        // off (TEST AH,0x44 / JNP: C3 only = equal).
        std::map<int, float>::iterator Interval = GGcsCategoryIntervals.find(Category);
        double const MinInterval = Interval != GGcsCategoryIntervals.end()
                                       ? static_cast<double>(Interval->second)
                                       : GGcsDefaultInterval;
        if (MinInterval == -1.0)
            return false;
    }

    // rand() * 2^-15 below the category's chance (default 1.0);
    // TEST AH,5 / JP: C0 only = below.
    std::map<int, float>::iterator ChanceIt = GGcsCategoryChances.find(Category);
    if (ChanceIt != GGcsCategoryChances.end())
        Chance = ChanceIt->second;
    double const Roll = static_cast<double>(rand()) * static_cast<double>(1.0f / 32768.0f);
    return Roll < static_cast<double>(Chance);
}

void __cdecl RecordGcsComm(int Category, FSystemInfo* System, unsigned int Speaker,
                           unsigned int Listener)
{
    // . The position starts at -0.1 (0xbdcccccd) and keeps it when GetLocation
    // fails (the result is not tested).
    FGcsRecentComm Comm;
    Comm.System = System;
    Comm.Time = static_cast<float>(GetContentGameTime());
    Comm.Position.x = -0.1f;
    Comm.Position.y = -0.1f;
    Comm.Position.z = -0.1f;
    Comm.Speaker = Speaker;
    Comm.Listener = Listener;
    Comm.Field_0x1c = -1;
    Matrix Orientation;
    pub::SpaceObj::GetLocation(Speaker != 0 ? Speaker : Listener, Comm.Position, Orientation);

    // at most 10 per category: the oldest (back) goes, the new one is
    // pushed to the front.
    std::list<FGcsRecentComm>& Recent = GGcsRecentComms[Category];
    if (Recent.size() == 10)
        Recent.pop_back();
    Recent.push_front(Comm);
}
