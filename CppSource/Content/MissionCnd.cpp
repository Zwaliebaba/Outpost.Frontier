// Author: Giperion.

#include "MissionCnd.h"
#include "BaseDB.h"
#include "ContentInternal.h"
#include "ContentPlayerState.h"
#include "SystemInfo.h"
#include "MissionAct.h"
#include "MissionScript.h"
#include "NpcShip.h"            // FContentNpcShip : FContentShipBase (Cnd_InTradelane)
#include "BehaviorCmds.h"       // FContentShipBase::HasField120
#include "RtcScripts.h"
#include "common_id.h"
#include "common_iface.h"
#include "common_ini.h"
#include "common_server.h"

#include <cmath>
#include <cstdlib>
#include <string.h>

namespace
{
    // (raw 00 00 00 38) = 2^-15: rand() scale in Cnd_Timer::Parse.
    float const GRandUnitScale = 1.0f / 32768.0f;

    // baseline tests an ID_String for "set" with TEST id,0x3fffffff.
    unsigned int const GIdValueMask = 0x3fffffff;

    // (TU21 initializer the branch): CreateID(null), the default of the
    // Cnd_DistVecLbl and Cnd_LootAcquired label fields.
    unsigned int const GNullId = CreateID(nullptr);

    // (TU21 initializers 06ec4200-06ec4290): the state names
    // Cnd_WatchTrigger compares against.
    unsigned int const GIdStateActive = CreateID("ACTIVE");
    unsigned int const GIdStateInactive = CreateID("INACTIVE");
    unsigned int const GIdStateComplete = CreateID("COMPLETE");
    unsigned int const GIdStateOn = CreateID("ON");
    unsigned int const GIdStateOff = CreateID("OFF");

    // (&mission+0x0c) followed by the mission's cached player state (+0x08,
    // ): null when the lookup fails, else the cache.
    FContentPlayerState* GetMissionPlayerState(FMissionScript* Mission)
    {
        if (FindContentPlayerState(Mission->PlayerId) == nullptr)
            return nullptr;
        return Mission->PlayerState;
    }

    // player+0x18 system record ? record+0x48 : 0.
    unsigned int GetPlayerSystemId(FContentPlayerState* Player)
    {
        if (Player->SystemRecord == nullptr)
            return 0;
        return Player->SystemRecord->SystemId;
    }

    // player+0x10 ship ? ship+0x04 : 0.
    unsigned int GetPlayerShipId(FContentPlayerState* Player)
    {
        if (Player->ShipObject == nullptr)
            return 0;
        return Player->ShipObject->SpaceObjId;
    }

    // Shared Parse of Cnd_BaseEnter/Cnd_BaseExit (the routine): s0 into a stack
    // TString<0x20>; unless it is "ANY" (4-byte compare with the field, case-sensitive),
    // pub::GetBaseID(BaseId, s0).
    void ParseBaseCondition(INI_Reader& Reader, unsigned int& BaseId)
    {
        TString<0x20> Name;
        Name.Length = 0;
        Name.Data[0] = '\0';
        Name = Reader.get_value_string(0);
        if (strcmp(Name.Data, "ANY") != 0)
            pub::GetBaseID(BaseId, Name.Data);
    }

    // Shared Matches the branch (Cnd_BaseEnter/Exit, Cnd_SpaceEnter/Exit): null parms ->
    // false; an unset id matches anything, otherwise *parms must equal it.
    bool MatchesIdOrAny(unsigned int Id, void const* EventParms)
    {
        if (EventParms == nullptr)
            return false;
        if ((Id & GIdValueMask) == 0)
            return true;
        return *static_cast<unsigned int const*>(EventParms) == Id;
    }

    // Shared Parse of Cnd_LocEnter/Cnd_LocExit (the routine).
    void ParseLocationCondition(INI_Reader& Reader, unsigned int& LocationId, unsigned int& BaseId)
    {
        pub::GetBaseID(BaseId, Reader.get_value_string(1));
        LocationId = pub::GF::FindLocation(BaseId, Reader.get_value_string(0));
    }

    // Shared Matches of Cnd_LocEnter/Cnd_LocExit (the routine): null parms / the routine /
    // no base record -> false; base record +0x48 == BaseId and *parms == LocationId.
    bool MatchesPlayerLocation(FMissionScript* Mission, unsigned int LocationId,
                               unsigned int BaseId, void const* EventParms)
    {
        if (EventParms == nullptr)
            return false;
        if (FindContentPlayerState(Mission->PlayerId) == nullptr)
            return false;
        FContentBase* const Base = Mission->PlayerState->BaseRecord;
        if (Base == nullptr)
            return false;
        if (Base->BaseId != BaseId)
            return false;
        return LocationId == *static_cast<unsigned int const*>(EventParms);
    }

    // Shared Parse of Cnd_CharSelect/Cnd_RumorHeard (the routine).
    void ParseCharacterCondition(INI_Reader& Reader, TString<0x20>& Name, unsigned int& BaseId,
                                 unsigned int& LocationId)
    {
        Name = Reader.get_value_string(0);
        pub::GetBaseID(BaseId, Reader.get_value_string(2));
        LocationId = pub::GF::FindLocation(BaseId, Reader.get_value_string(1));
    }

    // Shared Parse of Cnd_SpaceEnter/Cnd_SpaceExit (the routine): s0 into a stack
    // TString<0x40>; when slot 0 is present and is not "no_params" (10-byte compare),
    // pub::GetSystemID(SystemId, s0).
    void ParseSpaceCondition(INI_Reader& Reader, unsigned int& SystemId)
    {
        TString<0x40> Name;
        Name.Data[0] = '\0';
        Name = Reader.get_value_string(0);
        if (Reader.is_value_empty(0))
            return;
        if (strcmp(Name.Data, "no_params") == 0)
            return;
        pub::GetSystemID(SystemId, Reader.get_value_string(0));
    }

    // TString ctor idiom: dword 0 / byte 0, then "" (the routine / the routine).
    template <unsigned N>
    void InitEmpty(TString<N>& Value)
    {
        Value.Length = 0;
        Value.Data[0] = '\0';
    }

    // x87 square-if-positive idioms of the distance parsers.
    // FCOM 0 / FST / TEST AH,0x41 / JNZ skip: squared only when Value > 0.
    float SquareIfGreater(float Value)
    {
        if (Value > 0.0f)
            return Value * Value;
        return Value;
    }

    // FCOM 0 / FST / TEST AH,0x41 / JNP skip: skipped when exactly one of C0/C3 is set
    // (less or equal); squared when greater or unordered.
    float SquareUnlessLessOrEqual(float Value)
    {
        if (!(Value <= 0.0f))
            return Value * Value;
        return Value;
    }

    // FCOM 0 / FST / TEST AH,0x5 / JNP skip: skipped when exactly one of C0/C2 is set
    // (less); squared when greater, equal or unordered.
    float SquareUnlessLess(float Value)
    {
        if (!(Value < 0.0f))
            return Value * Value;
        return Value;
    }

    // TICK_AWAY 2 / TICK_TOWARDS 1 / TICK_ALWAYS 0 (Cnd_DistShip / Cnd_DistVec): a missing or
    // unknown slot leaves the mode unchanged.
    void ParseTickMode(INI_Reader& Reader, unsigned int Slot, int& TickMode)
    {
        if (Reader.is_value_empty(Slot))
            return;
        if (_stricmp("TICK_AWAY", Reader.get_value_string(Slot)) == 0)
        {
            TickMode = 2;
            return;
        }
        if (_stricmp("TICK_TOWARDS", Reader.get_value_string(Slot)) == 0)
        {
            TickMode = 1;
            return;
        }
        if (_stricmp("TICK_ALWAYS", Reader.get_value_string(Slot)) == 0)
            TickMode = 0;
    }

    // Shared tail of Cnd_DistShip / Cnd_DistVec Matches (06ec0a5e-06ec0af7,
    // ). DistanceSq is the float the x87 sum was stored to.
    bool TickDistanceCondition(float DistanceSq, float LimitSq, unsigned char bInside,
                               float& TickTimer, float TickInterval, int TickMode,
                               float& LastDistanceSq, float DeltaSeconds)
    {
        // inside: TEST AH,0x5 / JNP go on (less); outside: TEST AH,0x41 / JNZ reset (not
        // greater). The other outcomes (and unordered) reload the timer and fail.
        bool const bInRange = (bInside != 0) ? (DistanceSq < LimitSq) : (DistanceSq > LimitSq);
        if (!bInRange)
        {
            TickTimer = TickInterval;
            return false;
        }
        double const Delta = static_cast<double>(DistanceSq) - static_cast<double>(LastDistanceSq);
        LastDistanceSq = DistanceSq;
        bool bTick = false;
        if (TickMode == 0)
            bTick = true;
        // FCOM 0 / TEST AH,0x41 / JP skip: TICK_TOWARDS ticks when the distance did not grow.
        else if (Delta <= 0.0 && TickMode == 1)
            bTick = true;
        // FCOMP 0 / TEST AH,0x1 / JNZ skip: TICK_AWAY ticks when it did not shrink.
        else if (Delta >= 0.0 && TickMode == 2)
            bTick = true;
        if (bTick)
            TickTimer -= DeltaSeconds;
        // FCOMP 0 / TEST AH,0x41 / JP false: done when the timer is <= 0.
        return TickTimer <= 0.0f;
    }

    // (TU38): non-empty and starts with a digit or '-'.
    bool IsNumericToken(char const* Text)
    {
        if (Text == nullptr)
            return false;
        char const First = Text[0];
        if (First == '\0')
            return false;
        if (First >= '0' && First <= '9')
            return true;
        return First == '-';
    }
}

// Base ctor as inlined in the routine / the Cnd_* ctors: +0x04 = trigger+0x14 (mission),
// ID_String::clear on +0x08, vptr 06fb565c.
Cnd::Cnd(FMissionScript* InMission)
    : Mission(InMission)
    , TriggerId(0)
{
}

// restore base vptr, operator delete when (flags & 1).
Cnd::~Cnd()
{
}

// RET.
void Cnd::OnActivate()
{
}

// RET.
void Cnd::Slot10()
{
}

// RET 4.
void Cnd::Save(FILE* /*File*/)
{
}

// RET 4.
void Cnd::Load(INI_Reader* /*Reader*/)
{
}

// MOV [ECX+8],arg / RET 4.
void Cnd::SetTriggerId(unsigned int Id)
{
    TriggerId = Id;
}

// ---------------------------------------------------------------------------------------
// Cnd_BaseEnter / Cnd_BaseExit
// ---------------------------------------------------------------------------------------

// base fields, vptr 06fb5770, +0x0c = 0.
CndBaseEnter::CndBaseEnter(FMissionScript* InMission)
    : Cnd(InMission)
    , BaseId(0)
{
}

// .
void CndBaseEnter::Parse(INI_Reader& Reader)
{
    ParseBaseCondition(Reader, BaseId);
}

// parms = {base id, ...}.
bool CndBaseEnter::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    return MatchesIdOrAny(BaseId, EventParms);
}

// vptr 06fb5744.
CndBaseExit::CndBaseExit(FMissionScript* InMission)
    : Cnd(InMission)
    , BaseId(0)
{
}

// .
void CndBaseExit::Parse(INI_Reader& Reader)
{
    ParseBaseCondition(Reader, BaseId);
}

// .
bool CndBaseExit::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    return MatchesIdOrAny(BaseId, EventParms);
}

// ---------------------------------------------------------------------------------------
// Cnd_CargoScanned
// ---------------------------------------------------------------------------------------

// .
CndCargoScanned::CndCargoScanned(FMissionScript* InMission)
    : Cnd(InMission)
    , Field_0x74(0)
    , Field_0x78(0)
{
    InitEmpty(ScannerLabel);
    InitEmpty(ScannedLabel);
}

// only when slots 0 and 1 are both present.
void CndCargoScanned::Parse(INI_Reader& Reader)
{
    if (Reader.is_value_empty(0))
        return;
    if (Reader.is_value_empty(1))
        return;
    ScannerLabel = Reader.get_value_string(0);
    ScannedLabel = Reader.get_value_string(1);
}

// parms = {scanner id, scanned id}. A label "none" (5-byte compare with
// ) is not resolved; unresolved ids are looked up lazily (the routine). Both
// set: both must match; only the scanner set: it must match; scanner unset: false.
bool CndCargoScanned::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    if (EventParms == nullptr)
        return false;
    bool bScannerMissing = false;
    if (strcmp(ScannerLabel.Data, "none") != 0 && (Field_0x74 & GIdValueMask) == 0)
    {
        Field_0x74 = Mission->GetLabelShipId(&ScannerLabel);
        if ((Field_0x74 & GIdValueMask) == 0)
            bScannerMissing = true;
    }
    if (strcmp(ScannedLabel.Data, "none") != 0 && (Field_0x78 & GIdValueMask) == 0)
    {
        Field_0x78 = Mission->GetLabelShipId(&ScannedLabel);
        if ((Field_0x78 & GIdValueMask) == 0)
            return false;
    }
    if (bScannerMissing)
        return false;
    unsigned int const* Parms = static_cast<unsigned int const*>(EventParms);
    bool const bScannerMatch = Field_0x74 == Parms[0];
    bool const bScannedMatch = Field_0x78 == Parms[1];
    if ((Field_0x74 & GIdValueMask) != 0 && (Field_0x78 & GIdValueMask) != 0)
        return bScannerMatch && bScannedMatch;
    if ((Field_0x74 & GIdValueMask) == 0)
        return false;
    return bScannerMatch;
}

// resolve each label that is not "none" (the routine).
void CndCargoScanned::OnActivate()
{
    if (strcmp(ScannerLabel.Data, "none") != 0)
        Field_0x74 = Mission->GetLabelShipId(&ScannerLabel);
    if (strcmp(ScannedLabel.Data, "none") != 0)
        Field_0x78 = Mission->GetLabelShipId(&ScannedLabel);
}

// ---------------------------------------------------------------------------------------
// Cnd_CharSelect
// ---------------------------------------------------------------------------------------

// .
CndCharSelect::CndCharSelect(FMissionScript* InMission)
    : Cnd(InMission)
    , LocationId(0)
    , BaseId(0)
{
    InitEmpty(Character);
}

// .
void CndCharSelect::Parse(INI_Reader& Reader)
{
    ParseCharacterCondition(Reader, Character, BaseId, LocationId);
}

// (06ebfac0-06ebfb51, RET 8): the player's base record (+0x48 base id) and the
// mission's current location (+0x308), then the base database (the routine room state,
// character lookup).
bool CndCharSelect::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    // null parms or no live player -> false.
    if (EventParms == nullptr)
        return false;
    if (FindContentPlayerState(Mission->PlayerId) == nullptr)
        return false;
    // the player's base record +0x48 against +0x34 (no null test here, as
    // in baseline; the second the routine below is the tested one).
    if (Mission->PlayerState->GetBaseRecord()->BaseId != BaseId)
        return false;
    // +0x0c against the mission's current location (+0x308).
    if (LocationId != Mission->CurrentLocationId)
        return false;
    FContentBase* const Base = Mission->PlayerState->GetBaseRecord();
    if (Base == nullptr)
        return false;
    // the routine room state, the routine(&+0x10, player, &it), the
    // character's id (node +0x14) against *parms.
    FBaseRoomState* const Room = Base->FindRoomState(&Mission->CurrentLocationId);
    if (Room == nullptr)
        return false;
    FBarCharacterList::iterator Found;
    if (!Room->FindCharacter(&Character, Mission->PlayerId, &Found))
        return false;
    return Found->CharacterId == *static_cast<unsigned int const*>(EventParms);
}

// ---------------------------------------------------------------------------------------
// Cnd_CommComplete
// ---------------------------------------------------------------------------------------

// .
CndCommComplete::CndCommComplete(FMissionScript* InMission)
    : Cnd(InMission)
    , CommId(0)
    , SourceObjId(0)
{
    InitEmpty(SourceLabel);
}

// .
void CndCommComplete::Parse(INI_Reader& Reader)
{
    CommId = pub::GetNicknameId(Reader.get_value_string(0));
    if (!Reader.is_value_empty(1))
        SourceLabel = Reader.get_value_string(1);
}

// parms+0x0c = comm id, parms+0x08 = sender id.
bool CndCommComplete::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    if (EventParms == nullptr)
        return false;
    unsigned int const* Parms = static_cast<unsigned int const*>(EventParms);
    if (SourceLabel.Length == 0)
        return Parms[3] == CommId;
    SourceObjId = Mission->GetLabelShipId(&SourceLabel);
    if ((SourceObjId & GIdValueMask) == 0)
        return false;
    if (Parms[3] != CommId)
        return false;
    return Parms[2] == SourceObjId;
}

// +0x44 = the routine(+0x0c) when the label is set.
void CndCommComplete::OnActivate()
{
    if (SourceLabel.Length != 0)
        SourceObjId = Mission->GetLabelShipId(&SourceLabel);
}

// ---------------------------------------------------------------------------------------
// Cnd_CmpToPlane
// ---------------------------------------------------------------------------------------

// .
CndCmpToPlane::CndCmpToPlane(FMissionScript* InMission)
    : Cnd(InMission)
    , PlaneA(0.0f)
    , PlaneB(0.0f)
    , PlaneC(0.0f)
    , PlaneD(0.0f)
    , Side(0)
    , ObjectId(0)
{
    InitEmpty(ObjectLabel);
}

// .
void CndCmpToPlane::Parse(INI_Reader& Reader)
{
    if (_stricmp("inside", Reader.get_value_string(0)) == 0)
        Side = 0;
    else if (_stricmp("outside", Reader.get_value_string(0)) == 0)
        Side = 1;
    ObjectLabel = Reader.get_value_string(1);
    PlaneA = Reader.get_value_float(2);
    PlaneB = Reader.get_value_float(3);
    PlaneC = Reader.get_value_float(4);
    PlaneD = Reader.get_value_float(5);
}

// player in space with a system record, object resolved (lazy
// ), then the plane value A*x + B*y + C*z + D: below 0 matches "inside" (0),
// above 0 matches "outside" (1).
bool CndCmpToPlane::Matches(pub::Controller::EMessage /*EventId*/, void const* /*EventParms*/)
{
    FContentPlayerState* Player = GetMissionPlayerState(Mission);
    if (Player == nullptr)
        return false;
    if (Mission->PlayerState->ShipObject == nullptr)
        return false;
    if (Mission->PlayerState->GetSystemRecord() == nullptr)
        return false;
    if ((ObjectId & GIdValueMask) == 0)
        ObjectId = Mission->GetLabelShipId(&ObjectLabel);
    if ((ObjectId & GIdValueMask) == 0)
        return false;
    Vector Position;
    Matrix Orientation;
    if (pub::SpaceObj::GetLocation(ObjectId, Position, Orientation) != 0)
        return false;
    float const Value = Position.z * PlaneC + Position.y * PlaneB + Position.x * PlaneA + PlaneD;
    // FLD 0 / FCOMP v / TEST AH,0x41 / JNZ: fall-through only when 0 > v.
    if (0.0f > Value && Side == 0)
        return true;
    // FLD 0 / FCOMP v / TEST AH,0x5 / JP false: continue only when 0 < v.
    if (0.0f < Value)
        return Side == 1;
    return false;
}

// +0x54 = the routine(+0x0c).
void CndCmpToPlane::OnActivate()
{
    ObjectId = Mission->GetLabelShipId(&ObjectLabel);
}

// ---------------------------------------------------------------------------------------
// Cnd_Destroyed
// ---------------------------------------------------------------------------------------

// +0x14 ID_String, +0x18 TString, +0x4c = 0, +0x50 = 0x3e6, +0x10 = EBX
// (-1 at this point of the factory).
CndDestroyed::CndDestroyed(FMissionScript* InMission)
    : Cnd(InMission)
    , Count(0)
    , Needed(-1)
    , Label(0)
    , KillerObjId(0)
    , Mode(0x3e6)
{
    InitEmpty(KillerLabel);
}

// .
void CndDestroyed::Parse(INI_Reader& Reader)
{
    Label = CreateID(Reader.get_value_string(0));
    // CMP EAX,-1 / JL skip: the count is taken only when >= -1.
    if (!Reader.is_value_empty(1) && Reader.get_value_int(1) >= -1)
        Needed = Reader.get_value_int(1);
    if (!Reader.is_value_empty(2))
    {
        char const* ModeName = Reader.get_value_string(2);
        if (_stricmp(ModeName, "EXPLODE") == 0)
            Mode = 1;
        else if (_stricmp(ModeName, "SILENT") == 0)
            Mode = 0;
        else if (_stricmp(ModeName, "ALL") == 0)
            Mode = 0x3e7;
        else if (_stricmp(ModeName, "ALL_IGNORE_LANDING") == 0)
            Mode = 0x3e6;
    }
    if (!Reader.is_value_empty(3))
        KillerLabel = Reader.get_value_string(3);
}

// . Parms: +0 destroyed object id, +4 killer id, +0x10 destroy type.
bool CndDestroyed::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    if (EventParms == nullptr)
        return false;
    if (FindContentPlayerState(Mission->PlayerId) == nullptr)
        return false;
    unsigned int const* Parms = static_cast<unsigned int const*>(EventParms);
    // a different destroy type only passes for ALL, or for
    // ALL_IGNORE_LANDING while the player has a ship.
    if (static_cast<int>(Parms[4]) != Mode && Mode != 0x3e7)
    {
        if (Mission->PlayerState->ShipObject == nullptr)
            return false;
        if (Mode != 0x3e6)
            return false;
    }
    if (!Mission->HasObjectLabel(Parms[0], &Label))
        return false;
    if (KillerLabel.Length != 0 && (KillerObjId & GIdValueMask) == 0)
        KillerObjId = Mission->GetLabelShipId(&KillerLabel);
    if ((KillerObjId & GIdValueMask) != 0 && KillerObjId != Parms[1])
        return false;
    if (Needed == -1)
    {
        // equal_range of the label in the mission +0xec multimap; true
        // unless more than one entry is left.
        return Mission->FormationMembers.count(Label) <= 1;
    }
    ++Count;
    return Count == Needed;
}

// +0x0c = 0; +0x4c = the routine(+0x18) when the killer label is set.
void CndDestroyed::OnActivate()
{
    Count = 0;
    if (KillerLabel.Length != 0)
        KillerObjId = Mission->GetLabelShipId(&KillerLabel);
}

// .
void CndDestroyed::Save(FILE* File)
{
    fprintf(File, "Cnd_Destroyed = %d\n", Count);
}

// .
void CndDestroyed::Load(INI_Reader* Reader)
{
    while (Reader->read_value())
    {
        if (Reader->is_value("Cnd_Destroyed"))
            Count = Reader->get_value_int(0);
    }
}

// ---------------------------------------------------------------------------------------
// Cnd_DistCircle
// ---------------------------------------------------------------------------------------

// +0x0c/+0x40 zeroed (no "" assignment), +0x74 = 0.
CndDistCircle::CndDistCircle(FMissionScript* InMission)
    : Cnd(InMission)
    , CircleObjId(0)
    , Radius(0.0f)
    , PlaneOffset(0.0f)
{
    InitEmpty(ObjectLabel);
    InitEmpty(CircleObj);
    LastPosition.x = LastPosition.y = LastPosition.z = 0.0f;
    CircleCenter.x = CircleCenter.y = CircleCenter.z = 0.0f;
    Normal.x = Normal.y = Normal.z = 0.0f;
}

// .
void CndDistCircle::Parse(INI_Reader& Reader)
{
    ObjectLabel = Reader.get_value_string(0);
    CircleObj = Reader.get_value_string(1);
}

// true when the object's move since the last tick crosses the circle's plane
// in the axis direction, within Radius of the circle centre.
bool CndDistCircle::Matches(pub::Controller::EMessage /*EventId*/, void const* /*EventParms*/)
{
    unsigned int const ObjectId = Mission->GetLabelShipId(&ObjectLabel);
    if (FindContentPlayerState(Mission->PlayerId) == nullptr)
        return false;
    if (Mission->PlayerState->ShipObject == nullptr)
        return false;
    if (Mission->PlayerState->GetSystemRecord() == nullptr)
        return false;
    if ((ObjectId & GIdValueMask) == 0)
        return false;
    // baseline copies its stack vector into +0x78 even when GetLocation fails (06ec06fc); the
    // port starts it at zero instead of an indeterminate value.
    Vector Position;
    Position.x = Position.y = Position.z = 0.0f;
    Matrix Orientation;
    bool bResult = false;
    if (pub::SpaceObj::GetLocation(ObjectId, Position, Orientation) == 0)
    {
        double const Dx = static_cast<double>(Position.x) - LastPosition.x;
        double const Dy = static_cast<double>(Position.y) - LastPosition.y;
        double const Dz = static_cast<double>(Position.z) - LastPosition.z;
        // TEST AH,0x44 / JNP out: a move parallel to the plane fails.
        double const Denom = Dy * Normal.y + Dz * Normal.z + Dx * Normal.x;
        if (!(Denom == 0.0))
        {
            double const Param = (static_cast<double>(Position.y) * Normal.y +
                                  static_cast<double>(Position.z) * Normal.z +
                                  static_cast<double>(Position.x) * Normal.x + PlaneOffset) /
                                 Denom;
            // the move direction, normalised (dz goes through a float).
            float const DzFloat = static_cast<float>(Dz);
            double const Length = sqrt(Dz * DzFloat + Dx * Dx + Dy * Dy);
            float const InvLength = static_cast<float>(1.0 / Length);
            float const DirX = static_cast<float>(Dx * InvLength);
            float const DirY = static_cast<float>(Dy * InvLength);
            float const DirZ = static_cast<float>(static_cast<double>(DzFloat) * InvLength);
            // 0 < Param < 1.
            if (0.0 < Param && Param < 1.0)
            {
                double const Along = static_cast<double>(DirZ) * Normal.z +
                                     static_cast<double>(DirY) * Normal.y +
                                     static_cast<double>(DirX) * Normal.x;
                if (Along > 0.0)
                {
                    double const Cx = static_cast<double>(CircleCenter.x) - Position.x;
                    double const Cy = static_cast<double>(CircleCenter.y) - Position.y;
                    double const Cz = static_cast<double>(CircleCenter.z) - Position.z;
                    if (sqrt(Cz * Cz + Cx * Cx + Cy * Cy) < Radius)
                        bResult = true;
                }
            }
        }
    }
    LastPosition = Position;
    return bResult;
}

// .
void CndDistCircle::OnActivate()
{
    CircleObjId = pub::GetNicknameId(CircleObj.Data);
    Matrix Orientation;
    pub::SpaceObj::GetLocation(CircleObjId, CircleCenter, Orientation);
    FVector Forward;
    Forward.X = 0.0f;
    Forward.Y = 0.0f;
    Forward.Z = -1.0f;
    if (GTriggerMathEngine == nullptr)
        GTriggerMathEngine = ContentAcquire3DMathEngine();
    // Engine vtbl +0x2c (TransformMatrix) into a local, copied to +0x90.
    FVector Axis;
    GTriggerMathEngine->TransformMatrix(Axis, reinterpret_cast<FMatrix const&>(Orientation),
                                        Forward);
    Normal.x = Axis.X;
    Normal.y = Axis.Y;
    Normal.z = Axis.Z;
    // (x*x + y*y) + z*z, 1/sqrt kept on the x87 stack.
    double const InvLength =
        1.0 / sqrt(static_cast<double>(Normal.x) * Normal.x +
                   static_cast<double>(Normal.y) * Normal.y +
                   static_cast<double>(Normal.z) * Normal.z);
    Normal.x = static_cast<float>(InvLength * Normal.x);
    Normal.y = static_cast<float>(InvLength * Normal.y);
    Normal.z = static_cast<float>(InvLength * Normal.z);
    PlaneOffset = static_cast<float>(-(static_cast<double>(CircleCenter.z) * Normal.z +
                                       static_cast<double>(CircleCenter.y) * Normal.y +
                                       static_cast<double>(CircleCenter.x) * Normal.x));
    // GetRadius(&+0x74, &+0x9c, &+0x84) overwrites +0x84 after the plane offset.
    pub::SpaceObj::GetRadius(CircleObjId, Radius, CircleCenter);
    unsigned int const ObjectId = Mission->GetLabelShipId(&ObjectLabel);
    if ((ObjectId & GIdValueMask) != 0)
    {
        Matrix ObjectOrientation;
        pub::SpaceObj::GetLocation(ObjectId, LastPosition, ObjectOrientation);
    }
}

// ---------------------------------------------------------------------------------------
// Cnd_DistShip
// ---------------------------------------------------------------------------------------

// .
CndDistShip::CndDistShip(FMissionScript* InMission)
    : Cnd(InMission)
    , DistanceSq(0.0f)
    , bInside(0)
    , TickTimer(0.0f)
    , TickInterval(0.0f)
    , TickMode(0)
    , LastDistanceSq(0.0f)
    , ObjectAId(0)
    , ObjectBId(0)
{
    InitEmpty(ObjectA);
    InitEmpty(ObjectB);
}

// .
void CndDistShip::Parse(INI_Reader& Reader)
{
    char const* SideName = Reader.get_value_string(0);
    if (_stricmp(SideName, "inside") == 0)
        bInside = 1;
    else if (_stricmp(SideName, "outside") == 0)
        bInside = 0;
    ObjectA = Reader.get_value_string(1);
    ObjectB = Reader.get_value_string(2);
    DistanceSq = SquareIfGreater(Reader.get_value_float(3));
    // FCOMP 0 / TEST AH,0x41 / JNZ skip: taken only when > 0.
    if (!Reader.is_value_empty(4) && Reader.get_value_float(4) > 0.0f)
        TickInterval = Reader.get_value_float(4);
    ParseTickMode(Reader, 5, TickMode);
}

// . Parms: the frame time (float). Both ids are re-resolved every tick.
bool CndDistShip::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    if (EventParms == nullptr)
        return false;
    if (FindContentPlayerState(Mission->PlayerId) == nullptr)
        return false;
    if (Mission->PlayerState->ShipObject == nullptr)
        return false;
    if (Mission->PlayerState->GetSystemRecord() == nullptr)
        return false;
    ObjectAId = Mission->GetLabelObjectId(&ObjectA);
    if ((ObjectAId & GIdValueMask) == 0)
        return false;
    Vector PositionA;
    Matrix Orientation;
    if (pub::SpaceObj::GetLocation(ObjectAId, PositionA, Orientation) != 0)
        return false;
    ObjectBId = Mission->GetLabelObjectId(&ObjectB);
    if ((ObjectBId & GIdValueMask) == 0)
        return false;
    Vector PositionB;
    if (pub::SpaceObj::GetLocation(ObjectBId, PositionB, Orientation) != 0)
        return false;
    // both objects must be in the same system (results ignored).
    unsigned int SystemA = 0;
    unsigned int SystemB = 0;
    pub::SpaceObj::GetSystem(ObjectAId, SystemA);
    pub::SpaceObj::GetSystem(ObjectBId, SystemB);
    if (SystemA != SystemB)
        return false;
    // (dz*dz + dy*dy) + dx*dx on the x87 stack, stored as float.
    double const Dx = static_cast<double>(PositionA.x) - static_cast<double>(PositionB.x);
    double const Dy = static_cast<double>(PositionA.y) - static_cast<double>(PositionB.y);
    double const Dz = static_cast<double>(PositionA.z) - static_cast<double>(PositionB.z);
    float const CurrentSq = static_cast<float>(Dz * Dz + Dy * Dy + Dx * Dx);
    return TickDistanceCondition(CurrentSq, DistanceSq, bInside, TickTimer, TickInterval,
                                 TickMode, LastDistanceSq,
                                 *static_cast<float const*>(EventParms));
}

// +0x8c/+0x90 = the routine(+0x0c/+0x40) when the labels are set;
// +0x7c = +0x80, +0x88 = 0.
void CndDistShip::OnActivate()
{
    if (ObjectA.Length != 0)
        ObjectAId = Mission->GetLabelObjectId(&ObjectA);
    if (ObjectB.Length != 0)
        ObjectBId = Mission->GetLabelObjectId(&ObjectB);
    TickTimer = TickInterval;
    LastDistanceSq = 0.0f;
}

// .
void CndDistShip::Save(FILE* File)
{
    fprintf(File, "Cnd_DistShip = %f, %f\n", static_cast<double>(TickTimer),
            static_cast<double>(LastDistanceSq));
}

// .
void CndDistShip::Load(INI_Reader* Reader)
{
    while (Reader->read_value())
    {
        if (Reader->is_value("Cnd_DistShip"))
        {
            TickTimer = Reader->get_value_float(0);
            LastDistanceSq = Reader->get_value_float(1);
        }
    }
}

// ---------------------------------------------------------------------------------------
// Cnd_DistVec
// ---------------------------------------------------------------------------------------

// +0x18 TString, +0x64/+0x58/+0x5c = 0, +0x50 byte 0.
CndDistVec::CndDistVec(FMissionScript* InMission)
    : Cnd(InMission)
    , PositionX(0.0f)
    , PositionY(0.0f)
    , PositionZ(0.0f)
    , DistanceSq(0.0f)
    , bInside(0)
    , TickTimer(0.0f)
    , TickInterval(0.0f)
    , TickMode(0)
    , LastDistanceSq(0.0f)
    , ObjectId(0)
{
    InitEmpty(ObjectLabel);
}

// .
void CndDistVec::Parse(INI_Reader& Reader)
{
    char const* SideName = Reader.get_value_string(0);
    if (_stricmp(SideName, "inside") == 0)
        bInside = 1;
    else if (_stricmp(SideName, "outside") == 0)
        bInside = 0;
    ObjectLabel = Reader.get_value_string(1);
    PositionX = Reader.get_value_float(2);
    PositionY = Reader.get_value_float(3);
    PositionZ = Reader.get_value_float(4);
    DistanceSq = SquareUnlessLessOrEqual(Reader.get_value_float(5));
    // FCOMP 0 / TEST AH,0x1 / JNZ skip: taken when not less (and ordered).
    if (!Reader.is_value_empty(6) && Reader.get_value_float(6) >= 0.0f)
        TickInterval = Reader.get_value_float(6);
    ParseTickMode(Reader, 7, TickMode);
}

// . Parms: the frame time (float). The id is re-resolved every tick with
// (OnActivate uses the routine).
bool CndDistVec::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    if (EventParms == nullptr)
        return false;
    if (FindContentPlayerState(Mission->PlayerId) == nullptr)
        return false;
    if (Mission->PlayerState->ShipObject == nullptr)
        return false;
    if (Mission->PlayerState->GetSystemRecord() == nullptr)
        return false;
    ObjectId = Mission->GetLabelObjectId(&ObjectLabel);
    if ((ObjectId & GIdValueMask) == 0)
        return false;
    Vector Position;
    Matrix Orientation;
    if (pub::SpaceObj::GetLocation(ObjectId, Position, Orientation) != 0)
        return false;
    // (dz*dz + dx*dx) + dy*dy on the x87 stack, stored as float.
    double const Dx = static_cast<double>(Position.x) - static_cast<double>(PositionX);
    double const Dy = static_cast<double>(Position.y) - static_cast<double>(PositionY);
    double const Dz = static_cast<double>(Position.z) - static_cast<double>(PositionZ);
    float const CurrentSq = static_cast<float>(Dz * Dz + Dx * Dx + Dy * Dy);
    return TickDistanceCondition(CurrentSq, DistanceSq, bInside, TickTimer, TickInterval,
                                 TickMode, LastDistanceSq,
                                 *static_cast<float const*>(EventParms));
}

// +0x64 = the routine(+0x18) when the label is set; +0x54 = +0x58,
// +0x60 = 0.
void CndDistVec::OnActivate()
{
    if (ObjectLabel.Length != 0)
        ObjectId = Mission->GetLabelShipId(&ObjectLabel);
    TickTimer = TickInterval;
    LastDistanceSq = 0.0f;
}

// .
void CndDistVec::Save(FILE* File)
{
    fprintf(File, "Cnd_DistVec = %f, %f\n", static_cast<double>(TickTimer),
            static_cast<double>(LastDistanceSq));
}

// .
void CndDistVec::Load(INI_Reader* Reader)
{
    while (Reader->read_value())
    {
        if (Reader->is_value("Cnd_DistVec"))
        {
            TickTimer = Reader->get_value_float(0);
            LastDistanceSq = Reader->get_value_float(1);
        }
    }
}

// ---------------------------------------------------------------------------------------
// Cnd_DistVecLbl
// ---------------------------------------------------------------------------------------

// +0x18 = the field, +0x20 = 0, +0x28 = 0.
CndDistVecLbl::CndDistVecLbl(FMissionScript* InMission)
    : Cnd(InMission)
    , PositionX(0.0f)
    , PositionY(0.0f)
    , PositionZ(0.0f)
    , Label(GNullId)
    , DistanceSq(0.0f)
    , bInside(0)
    , bAny(0)
    , TickTimer(0.0f)
    , TickInterval(0.0f)
{
}

// .
void CndDistVecLbl::Parse(INI_Reader& Reader)
{
    char const* SideName = Reader.get_value_string(0);
    if (_stricmp(SideName, "inside") == 0)
        bInside = 1;
    else if (_stricmp(SideName, "outside") == 0)
        bInside = 0;
    bAny = (_stricmp(Reader.get_value_string(1), "any") == 0) ? 1 : 0;
    Label = CreateID(Reader.get_value_string(2));
    PositionX = Reader.get_value_float(3);
    PositionY = Reader.get_value_float(4);
    PositionZ = Reader.get_value_float(5);
    DistanceSq = SquareUnlessLessOrEqual(Reader.get_value_float(6));
    if (!Reader.is_value_empty(7) && Reader.get_value_float(7) >= 0.0f)
        TickInterval = Reader.get_value_float(7);
}

// . Parms: the frame time (float).
bool CndDistVecLbl::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    if (EventParms == nullptr)
        return false;
    if (FindContentPlayerState(Mission->PlayerId) == nullptr)
        return false;
    if (Mission->PlayerState->ShipObject == nullptr)
        return false;
    if (Mission->PlayerState->GetSystemRecord() == nullptr)
        return false;
    float const DeltaSeconds = *static_cast<float const*>(EventParms);
    if (bAny != 0)
        return MatchesAny(DeltaSeconds);
    return MatchesAll(DeltaSeconds);
}

// walks equal_range(+0x18) of the mission +0xec multimap (the routine /
// , ++ the routine). Each object whose location is known either reloads the
// timer (out of range) or ticks it; the first tick that reaches <= 0 wins.
bool CndDistVecLbl::MatchesAny(float DeltaSeconds)
{
    typedef std::multimap<unsigned int, unsigned int>::iterator FMemberIterator;
    FMemberIterator It = Mission->FormationMembers.lower_bound(Label);
    while (It != Mission->FormationMembers.upper_bound(Label))
    {
        Vector Position;
        Matrix Orientation;
        if (pub::SpaceObj::GetLocation(It->second, Position, Orientation) == 0)
        {
            // (dz*dz + dx*dx) + dy*dy, kept on the x87 stack.
            double const Dx = static_cast<double>(Position.x) - static_cast<double>(PositionX);
            double const Dy = static_cast<double>(Position.y) - static_cast<double>(PositionY);
            double const Dz = static_cast<double>(Position.z) - static_cast<double>(PositionZ);
            double const CurrentSq = Dz * Dz + Dx * Dx + Dy * Dy;
            bool const bInRange = (bInside != 0) ? (CurrentSq < DistanceSq) : (CurrentSq > DistanceSq);
            if (bInRange)
            {
                TickTimer -= DeltaSeconds;
                if (TickTimer <= 0.0f)
                    return true;
            }
            else
            {
                TickTimer = TickInterval;
            }
        }
        ++It;
    }
    return false;
}

// counts the objects in range, then the whole equal_range; ticks only when
// the two counts agree (an empty range ticks), else reloads the timer.
bool CndDistVecLbl::MatchesAll(float DeltaSeconds)
{
    typedef std::multimap<unsigned int, unsigned int>::iterator FMemberIterator;
    int InRange = 0;
    FMemberIterator It = Mission->FormationMembers.lower_bound(Label);
    while (It != Mission->FormationMembers.upper_bound(Label))
    {
        Vector Position;
        Matrix Orientation;
        if (pub::SpaceObj::GetLocation(It->second, Position, Orientation) == 0)
        {
            // (dz*dz + dy*dy) + dx*dx, kept on the x87 stack.
            double const Dx = static_cast<double>(Position.x) - static_cast<double>(PositionX);
            double const Dy = static_cast<double>(Position.y) - static_cast<double>(PositionY);
            double const Dz = static_cast<double>(Position.z) - static_cast<double>(PositionZ);
            double const CurrentSq = Dz * Dz + Dy * Dy + Dx * Dx;
            bool const bInRange = (bInside != 0) ? (CurrentSq < DistanceSq) : (CurrentSq > DistanceSq);
            if (bInRange)
                ++InRange;
        }
        ++It;
    }
    int const Total = static_cast<int>(Mission->FormationMembers.count(Label));
    if (InRange != Total)
    {
        TickTimer = TickInterval;
        return false;
    }
    TickTimer -= DeltaSeconds;
    return TickTimer <= 0.0f;
}

// +0x24 = +0x28.
void CndDistVecLbl::OnActivate()
{
    TickTimer = TickInterval;
}

// .
void CndDistVecLbl::Save(FILE* File)
{
    fprintf(File, "Cnd_DistVecLbl = %f\n", static_cast<double>(TickTimer));
}

// .
void CndDistVecLbl::Load(INI_Reader* Reader)
{
    while (Reader->read_value())
    {
        if (Reader->is_value("Cnd_DistVecLbl"))
            TickTimer = Reader->get_value_float(0);
    }
}

// ---------------------------------------------------------------------------------------
// Cnd_EncLaunched
// ---------------------------------------------------------------------------------------

// +0x0c ID_String.
CndEncLaunched::CndEncLaunched(FMissionScript* InMission)
    : Cnd(InMission)
    , EncounterId(0)
{
}

// (shared with Act_ActTrig / Act_DeactTrig / Act_NagOff).
void CndEncLaunched::Parse(INI_Reader& Reader)
{
    EncounterId = CreateID(Reader.get_value_string(0));
}

// true when the id is in the launched list.
bool CndEncLaunched::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    if (EventParms == nullptr)
        return false;
    std::list<unsigned int> const* Launched = static_cast<std::list<unsigned int> const*>(EventParms);
    std::list<unsigned int>::const_iterator It = Launched->begin();
    for (; It != Launched->end(); ++It)
    {
        if (*It == EncounterId)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------
// Cnd_HasMsn
// ---------------------------------------------------------------------------------------

// base fields only.
CndHasMsn::CndHasMsn(FMissionScript* InMission)
    : Cnd(InMission)
    , Wanted(0)
{
}

// s0 into a stack TString<0x40>; "yes" (4-byte compare) -> 0, "no" (3-byte)
// -> 1; anything else leaves the field.
void CndHasMsn::Parse(INI_Reader& Reader)
{
    TString<0x40> Answer;
    Answer.Data[0] = '\0';
    Answer = Reader.get_value_string(0);
    if (strcmp(Answer.Data, "yes") == 0)
    {
        Wanted = 0;
        return;
    }
    if (strcmp(Answer.Data, "no") == 0)
        Wanted = 1;
}

// the player's +0xd8 (the routine, live mission controller) set -> "yes"
// matches, unset -> "no" matches.
bool CndHasMsn::Matches(pub::Controller::EMessage /*EventId*/, void const* /*EventParms*/)
{
    FContentPlayerState* Player = GetMissionPlayerState(Mission);
    if (Player == nullptr)
        return false;
    if ((Player->ValueD8 & GIdValueMask) != 0)
        return Wanted == 0;
    return Wanted == 1;
}

// ---------------------------------------------------------------------------------------
// Cnd_HealthDec
// ---------------------------------------------------------------------------------------

// .
CndHealthDec::CndHealthDec(FMissionScript* InMission)
    : Cnd(InMission)
    , Health(0.0f)
    , ObjectId(0)
    , AttackerId(0)
{
    InitEmpty(ObjectLabel);
    InitEmpty(AttackerLabel);
}

// .
void CndHealthDec::Parse(INI_Reader& Reader)
{
    ObjectLabel = Reader.get_value_string(0);
    Health = Reader.get_value_float(1);
    if (!Reader.is_value_empty(2))
        AttackerLabel = Reader.get_value_string(2);
}

// . With parms (damage message 0xd: +0x00 object, +0x08 attacker, +0x0c/+0x10
// must be 0, float +0x20 = new relative health): lazy the routine of both labels, then the
// object must match and the health must be <= +0x74 (TEST AH,0x41 / JP: less or equal).
// Without parms (activation): only for an object label without an attacker label, with the
// player in space, pub::SpaceObj::GetRelativeHealth <= +0x74.
bool CndHealthDec::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    if (EventParms == nullptr)
    {
        if (ObjectLabel.Length == 0)
            return false;
        if (AttackerLabel.Length != 0)
            return false;
        FContentPlayerState* Player = GetMissionPlayerState(Mission);
        if (Player == nullptr)
            return false;
        if (Player->ShipObject == nullptr)
            return false;
        ObjectId = Mission->GetLabelObjectId(&ObjectLabel);
        if ((ObjectId & GIdValueMask) == 0)
            return false;
        float Relative = 0.0f;
        pub::SpaceObj::GetRelativeHealth(ObjectId, Relative);
        return Relative < Health || Relative == Health;
    }

    bool bObjectMissing = false;
    if ((ObjectId & GIdValueMask) == 0 && ObjectLabel.Length != 0)
    {
        ObjectId = Mission->GetLabelObjectId(&ObjectLabel);
        if ((ObjectId & GIdValueMask) == 0)
            bObjectMissing = true;
    }
    if ((AttackerId & GIdValueMask) == 0 && AttackerLabel.Length != 0)
    {
        AttackerId = Mission->GetLabelObjectId(&AttackerLabel);
        if ((AttackerId & GIdValueMask) == 0)
            return false;
    }
    if (bObjectMissing)
        return false;
    unsigned int const* Parms = static_cast<unsigned int const*>(EventParms);
    if (ObjectId != Parms[0])
        return false;
    if (Parms[3] != 0)
        return false;
    if (Parms[4] != 0)
        return false;
    float const NewHealth = *reinterpret_cast<float const*>(Parms + 8);
    if (!(NewHealth < Health || NewHealth == Health))
        return false;
    if (AttackerLabel.Length == 0)
        return true;
    return AttackerId == Parms[2];
}

// +0x78/+0x7c = the routine of the set labels.
void CndHealthDec::OnActivate()
{
    if (ObjectLabel.Length != 0)
        ObjectId = Mission->GetLabelObjectId(&ObjectLabel);
    if (AttackerLabel.Length != 0)
        AttackerId = Mission->GetLabelObjectId(&AttackerLabel);
}

// ---------------------------------------------------------------------------------------
// Cnd_InSpace
// ---------------------------------------------------------------------------------------

// +0x0c byte = 1.
CndInSpace::CndInSpace(FMissionScript* InMission)
    : Cnd(InMission)
    , bInSpace(1)
{
}

// +0x0c = get_value_bool(0) (shared with several Act_* parsers).
void CndInSpace::Parse(INI_Reader& Reader)
{
    bInSpace = Reader.get_value_bool(0) ? 1 : 0;
}

// "in space" means the player state holds a ship object (+0x10).
bool CndInSpace::Matches(pub::Controller::EMessage /*EventId*/, void const* /*EventParms*/)
{
    FContentPlayerState* Player = GetMissionPlayerState(Mission);
    if (Player == nullptr)
        return false;
    if (bInSpace != 0)
        return Player->ShipObject != nullptr;
    return Player->ShipObject == nullptr;
}

// ---------------------------------------------------------------------------------------
// Cnd_InTradelane
// ---------------------------------------------------------------------------------------

// +0x0c/+0x10 zeroed, +0x40 = 1.
CndInTradelane::CndInTradelane(FMissionScript* InMission)
    : Cnd(InMission)
    , bInLane(1)
{
    InitEmpty(ObjectLabel);
}

// .
void CndInTradelane::Parse(INI_Reader& Reader)
{
    bInLane = Reader.get_value_bool(0) ? 1 : 0;
    ObjectLabel = Reader.get_value_string(1);
}

// (06ec27c0-06ec2842). Live player with a ship object; the tested object is that
// ship for "Player" (stricmp), else the routine (FindNpcByName). True when the routine
// (HasField120, called twice as in baseline) agrees with +0x40.
bool CndInTradelane::Matches(pub::Controller::EMessage /*EventId*/, void const* /*EventParms*/)
{
    if (FindContentPlayerState(Mission->PlayerId) == nullptr)
        return false;
    if (Mission->PlayerState->ShipObject == nullptr)
        return false;
    FContentShipBase* Object = nullptr;
    if (_stricmp("Player", ObjectLabel.Data) == 0)
        Object = Mission->PlayerState->ShipObject;
    else
        Object = Mission->FindNpcByName(&ObjectLabel);
    if (Object == nullptr)
        return false;
    if (Object->HasField120() && bInLane != 0)
        return true;
    if (Object->HasField120())
        return false;
    return bInLane == 0;
}

// ---------------------------------------------------------------------------------------
// Cnd_InZone
// ---------------------------------------------------------------------------------------

// .
CndInZone::CndInZone(FMissionScript* InMission)
    : Cnd(InMission)
    , bInZone(1)
    , ZoneFlags(0)
    , ObjectId(0)
{
    InitEmpty(ObjectLabel);
}

// .
void CndInZone::Parse(INI_Reader& Reader)
{
    bInZone = Reader.get_value_bool(0) ? 1 : 0;
    ObjectLabel = Reader.get_value_string(1);
    if (Reader.is_value_empty(2))
        return;
    if (_stricmp("OBJECT_ZONE", Reader.get_value_string(2)) == 0)
    {
        ZoneFlags = 0x7fc0;
        return;
    }
    if (_stricmp("EXCLUSION_ZONE", Reader.get_value_string(2)) == 0)
    {
        ZoneFlags = 0x30000;
        return;
    }
    if (_stricmp("NEBULA_ZONE", Reader.get_value_string(2)) == 0)
    {
        ZoneFlags = 0x8000;
        return;
    }
    if (_stricmp("DUST_ZONE", Reader.get_value_string(2)) == 0)
        ZoneFlags = 0x200000;
}

// (06ec2980-06ec2a83, RET 8): +0x48 = the routine(+0x14), then
// pub::System::InZones / pub::Zone::GetProperties against +0x10.
bool CndInZone::Matches(pub::Controller::EMessage /*EventId*/, void const* /*EventParms*/)
{
    if (FindContentPlayerState(Mission->PlayerId) == nullptr)
        return false;
    if (Mission->PlayerState->ShipObject == nullptr)
        return false;
    ObjectId = Mission->GetLabelShipId(&ObjectLabel);
    if ((ObjectId & GIdValueMask) == 0)
        return false;
    FSystemInfo* const SystemRecord = Mission->PlayerState->GetSystemRecord();
    if (SystemRecord == nullptr)
        return false;
    unsigned int const SystemId = SystemRecord->SystemId;
    Vector Position;
    Matrix Orientation;
    if (pub::SpaceObj::GetLocation(ObjectId, Position, Orientation) != 0)
        return false;
    // at most 8 zones around the point (radius 0).
    unsigned int Zones[8];
    unsigned int ZoneCount;
    pub::System::InZones(SystemId, Position, 0.0f, Zones, 8, ZoneCount);
    bool bMatched = false;
    for (unsigned int Index = 0; Index < ZoneCount; ++Index)
    {
        unsigned long Properties;
        pub::Zone::GetProperties(Zones[Index], Properties);
        if ((ZoneFlags & Properties) != 0)
            bMatched = true;
    }
    // .
    if (bInZone != 0)
        return bMatched;
    if (!bMatched)
        return true;
    return ZoneFlags == 0;
}

// +0x48 = the routine(+0x14).
void CndInZone::OnActivate()
{
    ObjectId = Mission->GetLabelObjectId(&ObjectLabel);
}

// ---------------------------------------------------------------------------------------
// Cnd_JumpgateAct
// ---------------------------------------------------------------------------------------

// .
CndJumpgateAct::CndJumpgateAct(FMissionScript* InMission)
    : Cnd(InMission)
    , GateId(0)
{
    InitEmpty(Jumpgate);
}

// .
void CndJumpgateAct::Parse(INI_Reader& Reader)
{
    Jumpgate = Reader.get_value_string(0);
    GateId = pub::GetNicknameId(Jumpgate.Data);
}

// parms = {gate id, ...}.
bool CndJumpgateAct::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    if (EventParms == nullptr)
        return false;
    return *static_cast<unsigned int const*>(EventParms) == GateId;
}

// ---------------------------------------------------------------------------------------
// Cnd_JumpInComplete
// ---------------------------------------------------------------------------------------

// .
CndJumpInComplete::CndJumpInComplete(FMissionScript* InMission)
    : Cnd(InMission)
    , SystemId(0)
{
}

// .
void CndJumpInComplete::Parse(INI_Reader& Reader)
{
    if (_stricmp(Reader.get_value_string(0), "ANY") != 0)
        pub::GetSystemID(SystemId, Reader.get_value_string(0));
}

// parms = {system id, ship id}.
bool CndJumpInComplete::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    if (EventParms == nullptr)
        return false;
    unsigned int const* Parms = static_cast<unsigned int const*>(EventParms);
    if (Parms[0] != SystemId && (SystemId & GIdValueMask) != 0)
        return false;
    FContentPlayerState* Player = GetMissionPlayerState(Mission);
    if (Player == nullptr)
        return false;
    return Parms[1] == GetPlayerShipId(Player);
}

// ---------------------------------------------------------------------------------------
// Cnd_LaunchComplete
// ---------------------------------------------------------------------------------------

// .
CndLaunchComplete::CndLaunchComplete(FMissionScript* InMission)
    : Cnd(InMission)
    , BaseId(0)
    , ShipId(0)
{
    InitEmpty(BaseNickname);
    InitEmpty(ShipLabel);
}

// .
void CndLaunchComplete::Parse(INI_Reader& Reader)
{
    ShipLabel = Reader.get_value_string(0);
    if (Reader.is_value_empty(1))
        return;
    BaseNickname = Reader.get_value_string(1);
    BaseId = pub::GetNicknameId(BaseNickname.Data);
}

// parms = {base id, ship id}; lazy the routine(+0x40); an unset base id
// matches any base.
bool CndLaunchComplete::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    if (EventParms == nullptr)
        return false;
    if ((ShipId & GIdValueMask) == 0)
        ShipId = Mission->GetLabelShipId(&ShipLabel);
    if ((ShipId & GIdValueMask) == 0)
        return false;
    unsigned int const* Parms = static_cast<unsigned int const*>(EventParms);
    if (Parms[1] != ShipId)
        return false;
    if ((BaseId & GIdValueMask) != 0 && Parms[0] != BaseId)
        return false;
    return true;
}

// +0x78 = the routine(+0x40).
void CndLaunchComplete::OnActivate()
{
    ShipId = Mission->GetLabelShipId(&ShipLabel);
}

// ---------------------------------------------------------------------------------------
// Cnd_LocEnter / Cnd_LocExit
// ---------------------------------------------------------------------------------------

// .
CndLocEnter::CndLocEnter(FMissionScript* InMission)
    : Cnd(InMission)
    , LocationId(0)
    , BaseId(0)
{
}

// .
void CndLocEnter::Parse(INI_Reader& Reader)
{
    ParseLocationCondition(Reader, LocationId, BaseId);
}

// (shared with Cnd_LocExit): the player's base record (the routine = player
// +0x1c) must be BaseId, and *parms the location.
bool CndLocEnter::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    return MatchesPlayerLocation(Mission, LocationId, BaseId, EventParms);
}

// .
CndLocExit::CndLocExit(FMissionScript* InMission)
    : Cnd(InMission)
    , LocationId(0)
    , BaseId(0)
{
}

// .
void CndLocExit::Parse(INI_Reader& Reader)
{
    ParseLocationCondition(Reader, LocationId, BaseId);
}

// (shared with Cnd_LocEnter).
bool CndLocExit::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    return MatchesPlayerLocation(Mission, LocationId, BaseId, EventParms);
}

// ---------------------------------------------------------------------------------------
// Cnd_LootAcquired
// ---------------------------------------------------------------------------------------

// +0x0c = the field, +0x10/+0x14 zeroed, +0x48 = -1.
CndLootAcquired::CndLootAcquired(FMissionScript* InMission)
    : Cnd(InMission)
    , LootLabel(GNullId)
    , Count(0)
    , Needed(-1)
{
    InitEmpty(ShipLabel);
}

// .
void CndLootAcquired::Parse(INI_Reader& Reader)
{
    LootLabel = CreateID(Reader.get_value_string(0));
    ShipLabel = Reader.get_value_string(1);
    if (!Reader.is_value_empty(2))
        Needed = Reader.get_value_int(2);
}

// . Parms: +0 loot object id, +4 id of the ship that picked it up.
bool CndLootAcquired::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    if (EventParms == nullptr)
        return false;
    unsigned int const* Parms = static_cast<unsigned int const*>(EventParms);
    unsigned int const ShipId = Mission->GetLabelShipId(&ShipLabel);
    if (Mission->HasObjectLabel(Parms[0], &LootLabel) && ShipId == Parms[1])
        ++Count;
    // with no explicit count, done once every entry of the label in the
    // mission +0xec multimap is picked up.
    if (ShipId == Parms[1] && Needed == -1 &&
        Count == static_cast<int>(Mission->FormationMembers.count(LootLabel)))
        return true;
    return Needed == Count;
}

// +0x44 = 0.
void CndLootAcquired::OnActivate()
{
    Count = 0;
}

// .
void CndLootAcquired::Save(FILE* File)
{
    fprintf(File, "Cnd_LootAcquired = %d\n", Count);
}

// .
void CndLootAcquired::Load(INI_Reader* Reader)
{
    while (Reader->read_value())
    {
        if (Reader->is_value("Cnd_LootAcquired"))
            Count = Reader->get_value_int(0);
    }
}

// ---------------------------------------------------------------------------------------
// Cnd_MsnResponse
// ---------------------------------------------------------------------------------------

// base fields only.
CndMsnResponse::CndMsnResponse(FMissionScript* InMission)
    : Cnd(InMission)
    , Response(0)
{
}

// .
void CndMsnResponse::Parse(INI_Reader& Reader)
{
    if (_stricmp(Reader.get_value_string(0), "accept") == 0)
    {
        Response = 0;
        return;
    }
    if (_stricmp(Reader.get_value_string(0), "reject") == 0)
        Response = 1;
}

// parms byte +0x08 = accepted.
bool CndMsnResponse::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    if (EventParms == nullptr)
        return false;
    unsigned char const bAccepted = static_cast<unsigned char const*>(EventParms)[8];
    if (bAccepted != 0)
        return Response == 0;
    return Response == 1;
}

// ---------------------------------------------------------------------------------------
// Cnd_NPCSystemEnter / Cnd_NPCSystemExit
// ---------------------------------------------------------------------------------------

// three empty lists, +0x30..+0x44 zeroed, vptr 06fb59e4.
CndNpcSystemEnter::CndNpcSystemEnter(FMissionScript* InMission)
    : Cnd(InMission)
    , SystemId(0)
    , bAnySystem(0)
    , bDelayRunning(0)
    , DelayStart(0.0f)
    , DelayElapsed(0.0f)
    , Delay(0.0f)
    , bLoadedSameSystem(0)
{
}

// .
void CndNpcSystemEnter::Parse(INI_Reader& Reader)
{
    if (_stricmp(Reader.get_value_string(0), "ANY") == 0)
        bAnySystem = 1;
    else
        pub::GetSystemID(SystemId, Reader.get_value_string(0));

    // slots 1..0x31; "Player" is skipped here and appended below.
    int Slot = 1;
    while (!Reader.is_value_empty(static_cast<unsigned int>(Slot)) && Slot < 0x32)
    {
        if (_stricmp(Reader.get_value_string(static_cast<unsigned int>(Slot)), "Player") != 0)
        {
            FNpcSystemEntry Entry;
            Entry.Label.Data[0] = '\0';
            Entry.Label = Reader.get_value_string(static_cast<unsigned int>(Slot));
            Entry.SystemId = 0;
            Ships.push_back(Entry);
        }
        ++Slot;
    }

    FNpcSystemEntry PlayerEntry;
    PlayerEntry.Label.Data[0] = '\0';
    PlayerEntry.Label = "Player";
    PlayerEntry.SystemId = 0;
    Ships.push_back(PlayerEntry);

    // every system id of the parsed list = 0.
    std::list<FNpcSystemEntry>::iterator It = Ships.begin();
    for (; It != Ships.end(); ++It)
        It->SystemId = 0;
}

// . Events: 0x100f (parms: ship id, system id, +8 object entered), 0x1010 (NPC
// created, parms: object*), 0x20 (tick).
bool CndNpcSystemEnter::Matches(pub::Controller::EMessage EventId, void const* EventParms)
{
    if (EventId == pub::Controller::EMessage::NpcSystemEnter)
    {
        unsigned int const* Parms = static_cast<unsigned int const*>(EventParms);
        bool bResult = true;
        std::list<FNpcSystemEntry>::iterator It = Pending.begin();
        while (It != Pending.end())
        {
            unsigned int const ShipId = Mission->GetLabelShipId(&It->Label);
            if ((ShipId & GIdValueMask) == 0)
            {
                // no ship; forget the label unless loaded NPCs are awaited.
                if (!Awaited.empty())
                {
                    bResult = false;
                    ++It;
                    continue;
                }
                It = Pending.erase(It);
                continue;
            }
            unsigned int const EnteredSystem = Parms[1];
            if (ShipId == Parms[0])
            {
                if ((EnteredSystem & GIdValueMask) != 0 &&
                    (EnteredSystem == SystemId || bAnySystem != 0))
                    It->SystemId = EnteredSystem;
                else
                    bResult = false;
            }
            else if (It->SystemId != EnteredSystem)
            {
                bResult = false;
            }
            ++It;
        }
        if (!bResult)
            return false;
        // the player's own ship entering through a gate or hole
        // (object type bit 0x800 or 0x40) starts a 10 s delay instead.
        if (FindContentPlayerState(Mission->PlayerId) == nullptr)
            return true;
        FContentPlayerShip* const Ship = Mission->PlayerState->ShipObject;
        if (Ship == nullptr)
            return true;
        if (Ship->SpaceObjId != Parms[0])
            return true;
        if ((Parms[2] & GIdValueMask) == 0)
            return true;
        unsigned int Type;
        pub::SpaceObj::GetType(Parms[2], Type);
        if ((Type & 0x800) == 0 && (Type & 0x40) == 0)
            return true;
        bDelayRunning = 1;
        DelayStart = Mission->ElapsedTime;
        DelayElapsed = 0.0f;
        Delay = 10.0f;
        return false;
    }
    if (EventId == pub::Controller::EMessage::NpcShipCreated)
    {
        if (Awaited.empty())
            return false;
        if (GetPlayerSystemId(Mission->PlayerState) != SystemId && bAnySystem == 0)
            return false;
        FContentNpcShip const* const Npc =
            *static_cast<FContentNpcShip* const*>(EventParms);
        std::list<FNpcSystemEntry>::iterator It = Awaited.begin();
        for (; It != Awaited.end(); ++It)
        {
            if (strcmp(It->Label.Data, Npc->Nickname.Data) == 0)
            {
                Awaited.erase(It);
                break;
            }
        }
        if (Awaited.empty())
        {
            bDelayRunning = 1;
            DelayStart = Mission->ElapsedTime;
            DelayElapsed = 0.0f;
            Delay = 2.0f;
            return false;
        }
        // only "Player" left (7-byte compare) -> drop it and start.
        if (Awaited.size() == 1 && memcmp(Awaited.front().Label.Data, "Player", 7) == 0)
        {
            Awaited.clear();
            bDelayRunning = 1;
            DelayStart = Mission->ElapsedTime;
            DelayElapsed = 0.0f;
            Delay = 2.0f;
        }
        return false;
    }
    if (EventId == pub::Controller::EMessage::Heartbeat && bDelayRunning != 0)
    {
        // FST +0x3c / FCOMP +0x40 / TEST AH,0x1 / JNZ false: done when the unrounded
        // difference is not less than the delay.
        double const Elapsed =
            static_cast<double>(Mission->ElapsedTime) - static_cast<double>(DelayStart);
        DelayElapsed = static_cast<float>(Elapsed);
        return !(Elapsed < Delay);
    }
    return false;
}

// +0x24 = +0x0c (list assignment).
void CndNpcSystemEnter::OnActivate()
{
    Pending = Ships;
}

// no trailing newline in baseline.
void CndNpcSystemEnter::Save(FILE* File)
{
    fprintf(File, "Cnd_NPCSystemEnter = ");
    std::list<FNpcSystemEntry>::iterator It = Pending.begin();
    for (; It != Pending.end(); ++It)
        fprintf(File, "%s,%u, ", It->Label.Data, It->SystemId);
}

// rebuilds +0x24 from "label, system" pairs (at most 0x32 slots), clears
// +0x44 when the system ids differ, then copies +0x24 into +0x18.
void CndNpcSystemEnter::Load(INI_Reader* Reader)
{
    unsigned int FirstSystem = 0;
    bLoadedSameSystem = 1;
    while (Reader->read_value())
    {
        if (!Reader->is_value("Cnd_NPCSystemEnter"))
            continue;
        Pending.clear();
        int Slot = 0;
        if (!Reader->is_value_empty(0))
        {
            while (Slot < 0x32)
            {
                FNpcSystemEntry Entry;
                Entry.Label.Length = 0;
                Entry.Label.Data[0] = '\0';
                Entry.Label = Reader->get_value_string(static_cast<unsigned int>(Slot));
                Entry.SystemId =
                    static_cast<unsigned int>(Reader->get_value_int(static_cast<unsigned int>(Slot + 1)));
                Pending.push_back(Entry);
                if ((FirstSystem & GIdValueMask) == 0)
                    FirstSystem = static_cast<unsigned int>(
                        Reader->get_value_int(static_cast<unsigned int>(Slot + 1)));
                if (FirstSystem !=
                    static_cast<unsigned int>(Reader->get_value_int(static_cast<unsigned int>(Slot + 1))))
                    bLoadedSameSystem = 0;
                Slot += 2;
                if (Reader->is_value_empty(static_cast<unsigned int>(Slot)))
                    break;
            }
        }
        Awaited = Pending;
    }
}

// three empty lists (the routine), +0x30 = 0, +0x34 = 0.
CndNpcSystemExit::CndNpcSystemExit(FMissionScript* InMission)
    : Cnd(InMission)
    , SystemId(0)
    , bAnySystem(0)
{
}

// like Cnd_NPCSystemEnter's but every label (including "Player") is kept and
// nothing is appended.
void CndNpcSystemExit::Parse(INI_Reader& Reader)
{
    if (_stricmp(Reader.get_value_string(0), "ANY") == 0)
        bAnySystem = 1;
    else
        pub::GetSystemID(SystemId, Reader.get_value_string(0));

    int Slot = 1;
    while (!Reader.is_value_empty(static_cast<unsigned int>(Slot)) && Slot < 0x32)
    {
        FNpcSystemEntry Entry;
        Entry.Label.Data[0] = '\0';
        Entry.Label = Reader.get_value_string(static_cast<unsigned int>(Slot));
        Entry.SystemId = 0;
        Ships.push_back(Entry);
        ++Slot;
    }

    std::list<FNpcSystemEntry>::iterator It = Ships.begin();
    for (; It != Ships.end(); ++It)
        It->SystemId = 0;
}

// . Parms: +0 ship id, +4 system id.
bool CndNpcSystemExit::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    if (EventParms == nullptr)
        return false;
    unsigned int const* Parms = static_cast<unsigned int const*>(EventParms);
    bool bResult = true;
    std::list<FNpcSystemEntry>::iterator It = Pending.begin();
    while (It != Pending.end())
    {
        unsigned int const ShipId = Mission->GetLabelShipId(&It->Label);
        if ((ShipId & GIdValueMask) == 0)
        {
            // no ship; forget the label unless loaded NPCs are awaited.
            if (!Awaited.empty())
            {
                bResult = false;
                ++It;
                continue;
            }
            It = Pending.erase(It);
            continue;
        }
        // strike the first awaited entry with the same label.
        std::list<FNpcSystemEntry>::iterator AwaitedIt = Awaited.begin();
        for (; AwaitedIt != Awaited.end(); ++AwaitedIt)
        {
            if (strcmp(It->Label.Data, AwaitedIt->Label.Data) == 0)
            {
                Awaited.erase(AwaitedIt);
                break;
            }
        }
        unsigned int const LeftSystem = Parms[1];
        if (ShipId == Parms[0])
        {
            if ((LeftSystem & GIdValueMask) != 0 && (LeftSystem == SystemId || bAnySystem != 0))
                It->SystemId = LeftSystem;
        }
        else if (It->SystemId != LeftSystem)
        {
            bResult = false;
        }
        ++It;
    }
    return bResult;
}

// (Cnd_NPCSystemEnter's body).
void CndNpcSystemExit::OnActivate()
{
    Pending = Ships;
}

// .
void CndNpcSystemExit::Save(FILE* File)
{
    fprintf(File, "Cnd_NPCSystemExit = ");
    std::list<FNpcSystemEntry>::iterator It = Pending.begin();
    for (; It != Pending.end(); ++It)
        fprintf(File, "%s,%u, ", It->Label.Data, It->SystemId);
}

// rebuilds +0x24 from "label, system" pairs while the next label (read into
// a TString<0x40>) is non-empty, system ids through get_value_float + ftol; then copies
// +0x24 into +0x18.
void CndNpcSystemExit::Load(INI_Reader* Reader)
{
    while (Reader->read_value())
    {
        if (!Reader->is_value("Cnd_NPCSystemExit"))
            continue;
        Pending.clear();
        TString<0x40> Next;
        Next.Length = 0;
        Next.Data[0] = '\0';
        Next = Reader->get_value_string(0);
        int Slot = 0;
        while (Next.Length != 0 && Slot < 0x32)
        {
            FNpcSystemEntry Entry;
            Entry.Label.Length = 0;
            Entry.Label.Data[0] = '\0';
            Entry.Label = Reader->get_value_string(static_cast<unsigned int>(Slot));
            // ftol: 64-bit truncation, low dword kept.
            Entry.SystemId = static_cast<unsigned int>(static_cast<long long>(
                Reader->get_value_float(static_cast<unsigned int>(Slot + 1))));
            Pending.push_back(Entry);
            Slot += 2;
            Next = Reader->get_value_string(static_cast<unsigned int>(Slot));
        }
        Awaited = Pending;
    }
}

// ---------------------------------------------------------------------------------------
// Cnd_PlayerLaunch
// ---------------------------------------------------------------------------------------

// .
CndPlayerLaunch::CndPlayerLaunch(FMissionScript* InMission)
    : Cnd(InMission)
{
}

// RET 4.
void CndPlayerLaunch::Parse(INI_Reader& /*Reader*/)
{
}

// *parms == mission player id (+0x0c).
bool CndPlayerLaunch::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    if (EventParms == nullptr)
        return false;
    return *static_cast<unsigned int const*>(EventParms) == Mission->PlayerId;
}

// ---------------------------------------------------------------------------------------
// Cnd_PlayerManeuver
// ---------------------------------------------------------------------------------------

// .
CndPlayerManeuver::CndPlayerManeuver(FMissionScript* InMission)
    : Cnd(InMission)
    , BehaviorId(0)
    , bInverse(0)
    , TargetId(0)
{
    InitEmpty(TargetLabel);
}

// .
void CndPlayerManeuver::Parse(INI_Reader& Reader)
{
    BehaviorId = BehaviorTypes::get_behavior_id(Reader.get_value_string(0));
    if (Reader.is_value_empty(1))
        return;
    TargetLabel = Reader.get_value_string(1);
    if (strstr(TargetLabel.Data, "inverse") != nullptr)
        bInverse = 1;
}

// the player ship runs behaviour +0x0c against the wanted target; "inverse"
// negates the result, including every early out.
bool CndPlayerManeuver::Matches(pub::Controller::EMessage /*EventId*/, void const* /*EventParms*/)
{
    bool bResult = false;
    if (FindContentPlayerState(Mission->PlayerId) != nullptr)
    {
        unsigned int const ShipId = GetPlayerShipId(Mission->PlayerState);
        int const Behavior = pub::AI::get_behavior_id(ShipId);
        unsigned int Target = 0;
        pub::SpaceObj::GetTarget(ShipId, Target);
        if (Behavior == BehaviorId)
        {
            if (Behavior == 0xa)
            {
                // formation behaviour; the targeted NPC and the player
                // ship must share the space-object field +0x154.
                FContentPlayerShip* const Ship = Mission->PlayerState->ShipObject;
                FContentNpcShip* const Npc = Mission->FindNpcById(&Target);
                if (Npc != nullptr && Ship != nullptr)
                    bResult = Npc->FormationId == Ship->FormationId;
            }
            else if (TargetId == 0 || Target == TargetId)
            {
                bResult = true;
            }
        }
    }
    if (bInverse != 0)
        return !bResult;
    return bResult;
}

// +0x48 = the routine(+0x10) when the label is set.
void CndPlayerManeuver::OnActivate()
{
    if (TargetLabel.Length != 0)
        TargetId = Mission->GetLabelShipId(&TargetLabel);
}

// ---------------------------------------------------------------------------------------
// Cnd_PopUpDialog
// ---------------------------------------------------------------------------------------

// .
CndPopUpDialog::CndPopUpDialog(FMissionScript* InMission)
    : Cnd(InMission)
    , ButtonMask(0)
{
}

// s0 into a stack TString<0x40>, stricmp against the button names.
void CndPopUpDialog::Parse(INI_Reader& Reader)
{
    TString<0x40> Button;
    Button.Data[0] = '\0';
    Button = Reader.get_value_string(0);
    if (_stricmp(Button.Data, "YES") == 0)
    {
        ButtonMask = 1;
        return;
    }
    if (_stricmp(Button.Data, "NO") == 0)
    {
        ButtonMask = 2;
        return;
    }
    if (_stricmp(Button.Data, "LATER") == 0)
    {
        ButtonMask = 4;
        return;
    }
    if (_stricmp(Button.Data, "CLOSE") == 0)
        ButtonMask = 8;
}

// parms+0x04 = pressed button bits.
bool CndPopUpDialog::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    if (EventParms == nullptr)
        return false;
    return (static_cast<unsigned int const*>(EventParms)[1] & ButtonMask) != 0;
}

// ---------------------------------------------------------------------------------------
// Cnd_ProjHit
// ---------------------------------------------------------------------------------------

// .
CndProjHit::CndProjHit(FMissionScript* InMission)
    : Cnd(InMission)
    , Needed(1)
    , TargetId(0)
    , ShooterId(0)
    , Count(0)
{
    InitEmpty(TargetLabel);
    InitEmpty(ShooterLabel);
}

// .
void CndProjHit::Parse(INI_Reader& Reader)
{
    TargetLabel = Reader.get_value_string(0);
    Needed = Reader.get_value_int(1);
    if (!Reader.is_value_empty(2))
        ShooterLabel = Reader.get_value_string(2);
}

// parms = {target id, ?, shooter id}; lazy the routine of both labels; an
// unset shooter matches any; each hit counts, true once the count reaches +0x74 (JGE).
bool CndProjHit::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    if (EventParms == nullptr)
        return false;
    if ((TargetId & GIdValueMask) == 0)
        TargetId = Mission->GetLabelObjectId(&TargetLabel);
    if (ShooterLabel.Length != 0 && (ShooterId & GIdValueMask) == 0)
        ShooterId = Mission->GetLabelObjectId(&ShooterLabel);
    if ((TargetId & GIdValueMask) == 0)
        return false;
    unsigned int const* Parms = static_cast<unsigned int const*>(EventParms);
    if (Parms[0] != TargetId)
        return false;
    if ((ShooterId & GIdValueMask) != 0 && Parms[2] != ShooterId)
        return false;
    ++Count;
    return Count >= Needed;
}

// +0x78 = the routine(+0x0c), +0x7c = the routine(+0x40) when set;
// +0x80 = 0.
void CndProjHit::OnActivate()
{
    TargetId = Mission->GetLabelObjectId(&TargetLabel);
    if (ShooterLabel.Length != 0)
        ShooterId = Mission->GetLabelObjectId(&ShooterLabel);
    Count = 0;
}

// .
void CndProjHit::Save(FILE* File)
{
    fprintf(File, "Cnd_ProjHit = %d\n", Count);
}

// .
void CndProjHit::Load(INI_Reader* Reader)
{
    while (Reader->read_value())
    {
        if (Reader->is_value("Cnd_ProjHit"))
            Count = Reader->get_value_int(0);
    }
}

// ---------------------------------------------------------------------------------------
// Cnd_ProjHitShipToLbl
// ---------------------------------------------------------------------------------------

// .
CndProjHitShipToLbl::CndProjHitShipToLbl(FMissionScript* InMission)
    : Cnd(InMission)
    , TargetLabel(0)
    , Needed(1)
    , ShooterId(0)
    , Count(0)
{
    InitEmpty(ShooterLabel);
}

// .
void CndProjHitShipToLbl::Parse(INI_Reader& Reader)
{
    TargetLabel = CreateID(Reader.get_value_string(0));
    Needed = Reader.get_value_int(1);
    if (!Reader.is_value_empty(2))
        ShooterLabel = Reader.get_value_string(2);
}

// . Parms: +0 hit object id, +8 shooter id.
bool CndProjHitShipToLbl::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    if (EventParms == nullptr)
        return false;
    unsigned int const* Parms = static_cast<unsigned int const*>(EventParms);
    if (ShooterLabel.Length != 0)
        ShooterId = Mission->GetLabelObjectId(&ShooterLabel);
    if (!Mission->HasObjectLabel(Parms[0], &TargetLabel))
        return false;
    if (ShooterLabel.Length != 0 && (ShooterId & GIdValueMask) == 0)
        ShooterId = Mission->GetLabelObjectId(&ShooterLabel);
    // a set shooter id must equal parms+8.
    if ((ShooterId & GIdValueMask) != 0 && Parms[2] != ShooterId)
        return false;
    ++Count;
    return Count >= Needed;
}

// +0x48 = the routine(+0x10) when set; +0x4c = 0.
void CndProjHitShipToLbl::OnActivate()
{
    if (ShooterLabel.Length != 0)
        ShooterId = Mission->GetLabelObjectId(&ShooterLabel);
    Count = 0;
}

// .
void CndProjHitShipToLbl::Save(FILE* File)
{
    fprintf(File, "Cnd_ProjHitShipToLbl = %d\n", Count);
}

// .
void CndProjHitShipToLbl::Load(INI_Reader* Reader)
{
    while (Reader->read_value())
    {
        if (Reader->is_value("Cnd_ProjHitShipToLbl"))
            Count = Reader->get_value_int(0);
    }
}

// ---------------------------------------------------------------------------------------
// Cnd_RTCDone
// ---------------------------------------------------------------------------------------

// +0x0c = 0, +0x10 = 0 (no "" assignment).
CndRtcDone::CndRtcDone(FMissionScript* InMission)
    : Cnd(InMission)
{
    InitEmpty(RtcFile);
}

// clamp 0x7f.
void CndRtcDone::Parse(INI_Reader& Reader)
{
    RtcFile = Reader.get_value_string(0);
}

// null parms -> false; for each node of mission+0x2f4: object (node+0x90) set,
// its +0x40 == parms[1] and stricmp(node path +0x0c, +0x10) == 0 -> true; end -> false.
bool CndRtcDone::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    if (EventParms == nullptr)
        return false;
    unsigned int const* Parms = static_cast<unsigned int const*>(EventParms);
    std::list<FStoryMissionListNode_0x2f4>::iterator It = Mission->List_0x2f4.begin();
    while (It != Mission->List_0x2f4.end())
    {
        FCharacterEncounter* Script = It->Field_0x90;
        if (Script != nullptr && Parms[1] == Script->SequenceId &&
            _stricmp(It->Path, RtcFile.Data) == 0)
        {
            return true;
        }
        ++It;
    }
    return false;
}

// ---------------------------------------------------------------------------------------
// Cnd_RumorHeard
// ---------------------------------------------------------------------------------------

// .
CndRumorHeard::CndRumorHeard(FMissionScript* InMission)
    : Cnd(InMission)
    , LocationId(0)
    , BaseId(0)
{
    InitEmpty(Rumor);
}

// (Cnd_CharSelect's parser).
void CndRumorHeard::Parse(INI_Reader& Reader)
{
    ParseCharacterCondition(Reader, Rumor, BaseId, LocationId);
}

// parms = {player id, ?, char name[...] at +0x08}; the location must be the
// mission's current one (+0x308, written by the routine case 0x29).
bool CndRumorHeard::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    if (EventParms == nullptr)
        return false;
    unsigned int const* Parms = static_cast<unsigned int const*>(EventParms);
    if (Mission->PlayerId != Parms[0])
        return false;
    if (LocationId != Mission->CurrentLocationId)
        return false;
    char const* HeardName = reinterpret_cast<char const*>(Parms + 2);
    return _stricmp(Rumor.Data, HeardName) == 0;
}

// ---------------------------------------------------------------------------------------
// Cnd_SpaceEnter / Cnd_SpaceExit
// ---------------------------------------------------------------------------------------

// .
CndSpaceEnter::CndSpaceEnter(FMissionScript* InMission)
    : Cnd(InMission)
    , SystemId(0)
{
}

// .
void CndSpaceEnter::Parse(INI_Reader& Reader)
{
    ParseSpaceCondition(Reader, SystemId);
}

// .
bool CndSpaceEnter::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    return MatchesIdOrAny(SystemId, EventParms);
}

// .
CndSpaceExit::CndSpaceExit(FMissionScript* InMission)
    : Cnd(InMission)
    , SystemId(0)
{
}

// .
void CndSpaceExit::Parse(INI_Reader& Reader)
{
    ParseSpaceCondition(Reader, SystemId);
}

// .
bool CndSpaceExit::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    return MatchesIdOrAny(SystemId, EventParms);
}

// ---------------------------------------------------------------------------------------
// Cnd_SystemEnter / Cnd_SystemExit
// ---------------------------------------------------------------------------------------

// .
CndSystemEnter::CndSystemEnter(FMissionScript* InMission)
    : Cnd(InMission)
    , FromSystemId(0)
    , SystemId(0)
{
}

// .
void CndSystemEnter::Parse(INI_Reader& Reader)
{
    if (_stricmp(Reader.get_value_string(0), "ANY") != 0)
        pub::GetSystemID(SystemId, Reader.get_value_string(0));
    if (Reader.is_value_empty(1))
        return;
    if (_stricmp(Reader.get_value_string(1), "ANY") == 0)
        return;
    pub::GetSystemID(FromSystemId, Reader.get_value_string(1));
}

// with parms {system id, from system id} both unset-or-equal; without parms
// (trigger activation) the player's current system (the routine on the mission's cached
// state, no lookup gate in baseline) must equal +0x10.
bool CndSystemEnter::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    // null parms (activation) -> the routine on mission+8 directly, no
    // lookup gate.
    if (EventParms == nullptr)
        return GetPlayerSystemId(Mission->PlayerState) == SystemId;
    unsigned int const* Parms = static_cast<unsigned int const*>(EventParms);
    if (Parms[0] != SystemId && (SystemId & GIdValueMask) != 0)
        return false;
    if (Parms[1] != FromSystemId && (FromSystemId & GIdValueMask) != 0)
        return false;
    return true;
}

// .
CndSystemExit::CndSystemExit(FMissionScript* InMission)
    : Cnd(InMission)
    , SystemId(0)
    , ToSystemId(0)
{
}

// .
void CndSystemExit::Parse(INI_Reader& Reader)
{
    if (_stricmp(Reader.get_value_string(0), "ANY") != 0)
        pub::GetSystemID(SystemId, Reader.get_value_string(0));
    if (Reader.is_value_empty(1))
        return;
    if (_stricmp(Reader.get_value_string(1), "ANY") == 0)
        return;
    pub::GetSystemID(ToSystemId, Reader.get_value_string(1));
}

// parms = {system id, destination system id}.
bool CndSystemExit::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    if (EventParms == nullptr)
        return false;
    unsigned int const* Parms = static_cast<unsigned int const*>(EventParms);
    if (Parms[0] != SystemId && (SystemId & GIdValueMask) != 0)
        return false;
    if ((ToSystemId & GIdValueMask) == 0)
        return true;
    return Parms[1] == ToSystemId;
}

// ---------------------------------------------------------------------------------------
// Cnd_TetherBroke
// ---------------------------------------------------------------------------------------

// .
CndTetherBroke::CndTetherBroke(FMissionScript* InMission)
    : Cnd(InMission)
    , ShipAId(0)
    , ShipBId(0)
    , Field_0x7c(0)
    , DistanceSq(0.0f)
    , Field_0x84(0.0f)
    , Field_0x88(0.0f)
    , Field_0x8c(0.0f)
    , Field_0x90(0.0f)
    , Field_0x94(0.0f)
{
    InitEmpty(ShipA);
    InitEmpty(ShipB);
}

// .
void CndTetherBroke::Parse(INI_Reader& Reader)
{
    ShipA = Reader.get_value_string(0);
    ShipB = Reader.get_value_string(1);
    DistanceSq = SquareIfGreater(Reader.get_value_float(2));
    Field_0x8c = Reader.get_value_float(3);
    if (!Reader.is_value_empty(4))
        Field_0x84 = SquareUnlessLess(Reader.get_value_float(4));
}

// . Parms (optional): +0 ship A, +4 ship B, +0xc byte "tether released".
bool CndTetherBroke::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    if (FindContentPlayerState(Mission->PlayerId) == nullptr)
        return false;
    if (Mission->PlayerState->ShipObject == nullptr)
        return false;
    if (Mission->PlayerState->GetSystemRecord() == nullptr)
        return false;
    ShipAId = Mission->GetLabelShipId(&ShipA);
    ShipBId = Mission->GetLabelShipId(&ShipB);
    if ((ShipAId & GIdValueMask) == 0 || (ShipBId & GIdValueMask) == 0)
        return false;
    float const Distance = static_cast<float>(GetObjectDistanceSq(&ShipAId, &ShipBId));
    if (EventParms != nullptr)
    {
        unsigned int const* Parms = static_cast<unsigned int const*>(EventParms);
        if (Parms[0] == ShipAId && Parms[1] == ShipBId)
            Field_0x7c = (static_cast<unsigned char const*>(EventParms)[0xc] == 0) ? 1 : 0;
    }
    if (Field_0x7c == 0)
    {
        Field_0x90 = 0.0f;
        return false;
    }
    // FCOMP / TEST AH,0x41 / JP out: only while the limit is <= the distance.
    if (!(DistanceSq <= Distance))
        return false;
    double const Now = Mission->ElapsedTime;
    // TEST AH,0x44 / JP: no step on the first check (+0x94 == 0).
    float TimeStep = 0.0f;
    if (!(Field_0x94 == 0.0f))
        TimeStep = static_cast<float>(Now - Field_0x94);
    Field_0x94 = static_cast<float>(Now);
    double DistanceStep = 0.0;
    if (!(Field_0x88 == 0.0f))
        DistanceStep = static_cast<double>(Distance) - Field_0x88;
    Field_0x88 = Distance;
    // within the inner radius (+0x84 > distance) only a non-shrinking
    // distance counts; unordered compares fail.
    if (!(Field_0x84 <= Distance))
    {
        if (!(Field_0x84 > Distance))
            return false;
        if (!(DistanceStep >= 0.0))
            return false;
    }
    // FADD / FST +0x90 / FCOMP +0x8c / TEST AH,0x1 / JNZ false: done when not less.
    double const Accumulated = static_cast<double>(TimeStep) + Field_0x90;
    Field_0x90 = static_cast<float>(Accumulated);
    return !(Accumulated < Field_0x8c);
}

// .
double __cdecl GetObjectDistanceSq(unsigned int const* IdA, unsigned int const* IdB)
{
    Vector PositionA;
    Vector PositionB;
    Matrix Orientation;
    if (pub::SpaceObj::GetLocation(*IdA, PositionA, Orientation) != 0)
        return -1.0f;
    if (pub::SpaceObj::GetLocation(*IdB, PositionB, Orientation) != 0)
        return -1.0f;
    // (dz*dz + dy*dy) + dx*dx on the x87 stack.
    double const Dx = static_cast<double>(PositionA.x) - PositionB.x;
    double const Dy = static_cast<double>(PositionA.y) - PositionB.y;
    double const Dz = static_cast<double>(PositionA.z) - PositionB.z;
    return Dz * Dz + Dy * Dy + Dx * Dx;
}

// +0x90/+0x88/+0x94 = 0; +0x74/+0x78 = the routine of both labels.
void CndTetherBroke::OnActivate()
{
    Field_0x90 = 0.0f;
    Field_0x88 = 0.0f;
    Field_0x94 = 0.0f;
    ShipAId = Mission->GetLabelShipId(&ShipA);
    ShipBId = Mission->GetLabelShipId(&ShipB);
}

// .
void CndTetherBroke::Save(FILE* File)
{
    fprintf(File, "Cnd_TetherBroke = %f, %f\n", static_cast<double>(Field_0x90),
            static_cast<double>(Field_0x88));
}

// .
void CndTetherBroke::Load(INI_Reader* Reader)
{
    while (Reader->read_value())
    {
        if (Reader->is_value("Cnd_TetherBroke"))
        {
            Field_0x90 = Reader->get_value_float(0);
            Field_0x88 = Reader->get_value_float(1);
        }
    }
}

// ---------------------------------------------------------------------------------------
// Cnd_Timer
// ---------------------------------------------------------------------------------------

// base fields, vptr 06fb5884, byte +0x14 = 1. +0x0c/+0x10 are not written by
// the ctor (Parse writes +0x10, OnActivate writes +0x0c before the first Matches).
CndTimer::CndTimer(FMissionScript* InMission)
    : Cnd(InMission)
    , Elapsed(0.0f)
    , Duration(0.0f)
    , bRunOnBase(1)
{
}

// .
void CndTimer::Parse(INI_Reader& Reader)
{
    Duration = Reader.get_value_float(0);
    if (!Reader.is_value_empty(1))
    {
        float const Upper = Reader.get_value_float(1);
        float const Lower = Duration;
        // FILD rand * ; (Upper - Lower) * that; + Lower.
        float const RandUnit = static_cast<float>(rand()) * GRandUnitScale;
        Duration = (Upper - Lower) * RandUnit + Lower;
    }
    if (!Reader.is_value_empty(2))
    {
        if (_stricmp(Reader.get_value_string(2), "RUN_ON_BASE") == 0)
        {
            bRunOnBase = 1;
            return;
        }
        if (_stricmp(Reader.get_value_string(2), "RUN_IN_SPACE_ONLY") == 0)
            bRunOnBase = 0;
    }
}

// . EventParms is the float* delta the tick message (0x20) carries. +0x10 of
// the player state is its ship object: null (docked) counts only when bRunOnBase.
bool CndTimer::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    FContentPlayerState* Player = GetMissionPlayerState(Mission);
    if (Player == nullptr)
        return false;
    if (Player->ShipObject == nullptr && bRunOnBase == 0)
        return false;
    float const* Delta = static_cast<float const*>(EventParms);
    if (Delta != nullptr)
        Elapsed = Elapsed + *Delta;
    // FCOMP [+0x10] / TEST AH,1 / JZ -> true when not (Elapsed < Duration).
    return !(Elapsed < Duration);
}

// +0x0c = 0.
void CndTimer::OnActivate()
{
    Elapsed = 0.0f;
}

// fprintf(File, "Cnd_Timer = %f\n", (double)+0x0c).
void CndTimer::Save(FILE* File)
{
    fprintf(File, "Cnd_Timer = %f\n", static_cast<double>(Elapsed));
}

// while read_value: is_value("Cnd_Timer") -> +0x0c = get_value_float(0).
void CndTimer::Load(INI_Reader* Reader)
{
    while (Reader->read_value())
    {
        if (Reader->is_value("Cnd_Timer"))
            Elapsed = Reader->get_value_float(0);
    }
}

// ---------------------------------------------------------------------------------------
// Cnd_TLEntered / Cnd_TLExited
// ---------------------------------------------------------------------------------------

// three TStrings = "", +0xa8/+0xac/+0xb0 = 0, vptr 06fb58d0.
CndTlEntered::CndTlEntered(FMissionScript* InMission)
    : Cnd(InMission)
    , ShipId(0)
    , RingAId(0)
    , RingBId(0)
{
    InitEmpty(ShipLabel);
    InitEmpty(RingA);
    InitEmpty(RingB);
}

// .
void CndTlEntered::Parse(INI_Reader& Reader)
{
    ShipLabel = Reader.get_value_string(0);
    RingA = Reader.get_value_string(1);
    RingB = Reader.get_value_string(2);
    RingAId = pub::GetNicknameId(RingA.Data);
    RingBId = pub::GetNicknameId(RingB.Data);
}

// parms = {ship id, ring A id, ring B id}; the ship is re-resolved every call.
bool CndTlEntered::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    if (EventParms == nullptr)
        return false;
    ShipId = Mission->GetLabelShipId(&ShipLabel);
    if ((ShipId & GIdValueMask) == 0)
        return false;
    unsigned int const* Parms = static_cast<unsigned int const*>(EventParms);
    if (Parms[0] != ShipId)
        return false;
    if (Parms[1] != RingAId)
        return false;
    return Parms[2] == RingBId;
}

// +0xa8 = the routine(+0x0c).
void CndTlEntered::OnActivate()
{
    ShipId = Mission->GetLabelShipId(&ShipLabel);
}

// +0x0c/+0x40 = "", +0x74 only zeroed, +0xa8/+0xb0/+0xac = 0, vptr 06fb58ec.
CndTlExited::CndTlExited(FMissionScript* InMission)
    : Cnd(InMission)
    , ShipId(0)
    , RingBId(0)
    , RingAId(0)
{
    InitEmpty(ShipLabel);
    InitEmpty(RingA);
    InitEmpty(RingB);
}

// .
void CndTlExited::Parse(INI_Reader& Reader)
{
    ShipLabel = Reader.get_value_string(0);
    RingA = Reader.get_value_string(1);
    if (_stricmp(RingA.Data, "ANY") != 0)
        RingAId = pub::GetNicknameId(RingA.Data);
    if (Reader.is_value_empty(2))
        return;
    RingB = Reader.get_value_string(2);
    RingBId = pub::GetNicknameId(RingB.Data);
}

// parms = {ship id, +0x04 vs +0xac, +0x08 vs +0xb0}; an unset ring id matches
// any ring.
bool CndTlExited::Matches(pub::Controller::EMessage /*EventId*/, void const* EventParms)
{
    if (EventParms == nullptr)
        return false;
    ShipId = Mission->GetLabelShipId(&ShipLabel);
    if ((ShipId & GIdValueMask) == 0)
        return false;
    unsigned int const* Parms = static_cast<unsigned int const*>(EventParms);
    if (Parms[0] != ShipId)
        return false;
    if ((RingAId & GIdValueMask) != 0 && Parms[2] != RingAId)
        return false;
    if ((RingBId & GIdValueMask) != 0 && Parms[1] != RingBId)
        return false;
    return true;
}

// (Cnd_TLEntered's): +0xa8 = the routine(+0x0c).
void CndTlExited::OnActivate()
{
    ShipId = Mission->GetLabelShipId(&ShipLabel);
}

// ---------------------------------------------------------------------------------------
// Cnd_True
// ---------------------------------------------------------------------------------------

// base fields, vptr 06fb5908.
CndTrue::CndTrue(FMissionScript* InMission)
    : Cnd(InMission)
{
}

// RET 4.
void CndTrue::Parse(INI_Reader& /*Reader*/)
{
}

// MOV AL,1 / RET 8.
bool CndTrue::Matches(pub::Controller::EMessage /*EventId*/, void const* /*EventParms*/)
{
    return true;
}

// ---------------------------------------------------------------------------------------
// Cnd_WatchTrigger
// ---------------------------------------------------------------------------------------

// +0x0c/+0x10 ID_String, vptr 06fb5a40.
CndWatchTrigger::CndWatchTrigger(FMissionScript* InMission)
    : Cnd(InMission)
    , WatchedTriggerId(0)
    , WantedState(0)
{
}

// .
void CndWatchTrigger::Parse(INI_Reader& Reader)
{
    WatchedTriggerId = CreateID(Reader.get_value_string(0));
    WantedState = CreateID(Reader.get_value_string(1));
}

// the routine find in mission+0x1b0; unknown trigger -> false. ON matches
// ACTIVE, OFF matches INACTIVE or COMPLETE.
bool CndWatchTrigger::Matches(pub::Controller::EMessage /*EventId*/, void const* /*EventParms*/)
{
    FTriggerMap::iterator Found = Mission->TriggerMap.find(WatchedTriggerId);
    if (Found == Mission->TriggerMap.end())
        return false;
    int const State = Found->second.State;
    if (WantedState == GIdStateActive && State == G_TriggerStateActive)
        return true;
    if (WantedState == GIdStateInactive && State == G_TriggerStateInactive)
        return true;
    if (WantedState == GIdStateComplete && State == G_TriggerStateComplete)
        return true;
    if (WantedState == GIdStateOn && State == G_TriggerStateActive)
        return true;
    if (WantedState == GIdStateOff)
        return State == G_TriggerStateInactive || State == G_TriggerStateComplete;
    return false;
}

// ---------------------------------------------------------------------------------------
// Cnd_WatchVibe
// ---------------------------------------------------------------------------------------

// +0x10/+0x44 = "", +0x78/+0x7c/+0x0c/+0x80 = 0, vptr 06fb5a5c.
CndWatchVibe::CndWatchVibe(FMissionScript* InMission)
    : Cnd(InMission)
    , Compare(0)
    , ObjectAId(0)
    , ObjectBId(0)
    , Value(0.0f)
{
    InitEmpty(ObjectA);
    InitEmpty(ObjectB);
}

// .
void CndWatchVibe::Parse(INI_Reader& Reader)
{
    ObjectA = Reader.get_value_string(0);
    ObjectB = Reader.get_value_string(1);
    if (IsNumericToken(Reader.get_value_string(2)))
    {
        Value = Reader.get_value_float(2);
    }
    else
    {
        // get_indexed_value(2) into a by-value TString<0x40>.
        TString<0x40> RepName;
        RepName.Length = 0;
        RepName.Data[0] = '\0';
        RepName = Reader.get_indexed_value(2);
        Value = GetRepValueFromName(Mission, RepName);
    }
    if (Reader.is_value_empty(3))
        return;
    if (_stricmp(Reader.get_value_string(3), "lt") == 0)
    {
        Compare = 0;
        return;
    }
    if (_stricmp(Reader.get_value_string(3), "gt") == 0)
    {
        Compare = 1;
        return;
    }
    if (_stricmp(Reader.get_value_string(3), "eq") == 0)
        Compare = 2;
}

// lazy the routine of both labels; attitude of A's vibe towards B's
// (pub::SpaceObj::GetRep x2, pub::Reputation::GetAttitude) compared with +0x80:
// "lt" (TEST AH,0x41 / JP) less or equal, "gt" (TEST AH,1 / JNZ) not less and ordered,
// "eq" (TEST AH,0x44 / JP) equal.
bool CndWatchVibe::Matches(pub::Controller::EMessage /*EventId*/, void const* /*EventParms*/)
{
    if ((ObjectAId & GIdValueMask) == 0)
        ObjectAId = Mission->GetLabelObjectId(&ObjectA);
    if ((ObjectBId & GIdValueMask) == 0)
        ObjectBId = Mission->GetLabelObjectId(&ObjectB);
    if ((ObjectAId & GIdValueMask) == 0)
        return false;
    if ((ObjectBId & GIdValueMask) == 0)
        return false;
    int RepA = 0;
    int RepB = 0;
    float Attitude = 0.0f;
    pub::SpaceObj::GetRep(ObjectAId, RepA);
    pub::SpaceObj::GetRep(ObjectBId, RepB);
    pub::Reputation::GetAttitude(RepA, RepB, Attitude);
    if (Compare == 0)
        return Attitude < Value || Attitude == Value;
    if (Compare == 1)
        return Attitude >= Value;
    if (Compare == 2)
        return Attitude == Value;
    return false;
}

// +0x78/+0x7c = the routine(+0x10/+0x44).
void CndWatchVibe::OnActivate()
{
    ObjectAId = Mission->GetLabelObjectId(&ObjectA);
    ObjectBId = Mission->GetLabelObjectId(&ObjectB);
}
