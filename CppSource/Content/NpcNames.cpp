// Author: Giperion.

// Content.dll TU42, the NPC name part (06f31650-06f32000). See NpcNames.h.

// Listing: DLLS\BIN\Content. The mad-lib ids 0x3f17..0x3f24 are the name patterns the
// client formats; the values appended here are what the routine / the routine /
// / the routine (NpcShips.h) read back out of the two FmtStr.

#include "NpcNames.h"

#include "FactionProps.h"
#include "NpcShips.h"            // FMsnNpc, the name value readers
#include "common_server.h"       // pub::Reputation, pub::GetType, pub::GetShipArchSTRID

#include <cstdint>

int GNpcNameReputationCount = 0;

// (06f31650-06f3166f).
FNpcNameRequest::FNpcNameRequest()
    : NameIndex(0)
    , Desig(0)
    , DesigNumber(0)
    , FormationSize(1)
    , EscortNumber(0)
    , Field_0x14(0)
    , RankLevel(0)
    , ShipArchId(0)
{
}

// (06f31670-06f31694). +0x14..+0x1c are left unwritten, as in baseline (the only
// caller, TU29 06ef4d48 / 06ef4d53, writes +0x18 and +0x1c before every BuildNpcName;
// +0x14 has no reader).
FNpcNameRequest::FNpcNameRequest(int InNameIndex, int InFormationSize, int InEscortNumber,
                                 int InDesig, int InDesigNumber)
    : NameIndex(InNameIndex)
    , Desig(InDesig)
    , DesigNumber(InDesigNumber)
    , FormationSize(InFormationSize)
    , EscortNumber(InEscortNumber)
{
}

// Inlined in the routine (06f3194c-06f31980) and the routine (06f31e2f-06f31e68).
FNpcNameReputation::FNpcNameReputation(FNpcNameParams const* Params)
{
    pub::Reputation::Alloc(Reputation, Params->First, Params->Second);
    pub::Reputation::SetAffiliation(Reputation, Params->Affiliation);
    ++GNpcNameReputationCount;
}

// (06f316c0-06f318f2).
FNpcNameReputation::~FNpcNameReputation()
{
    int PilotFirst;
    int PilotLast;
    int LargeShipName = 1;
    int Desig = 1;
    int DesigNumber;
    {
        // two empty FmtStr locals, pub::Reputation::GetName(+8, First,
        // Second).
        FmtStr First;
        FmtStr Second;
        pub::Reputation::GetName(Reputation, First, Second);
        // the routine -> the pilot first / last name.
        GetSecondNameValues(&First, &Second, &PilotFirst, &PilotLast);
        // Second 0x3f1a / 0x3f1c -> the large ship name, node 3 (the
        // body inlined).
        if (Second.MadLibId == 0x3f1a || Second.MadLibId == 0x3f1c)
            LargeShipName = (int)Second.Head->Next->Next->Value;
        // the switch table 06f318f4 / 06f31900 (the the routine body
        // inlined): the formation designation.
        ::GetFirstNameValue(&First, &Second, &Desig);
        // the routine -> the designation number; the routine rolls a value
        // the destructor never reads (the rand() call stays).
        GetFirstNameLastValue(&First, &Second, &DesigNumber);
        int Unused;
        RollNpcNameValue(&First, &Second, &Unused);
        // ~FmtStr Second, ~FmtStr First.
    }

    // the faction name tables of the affiliation (the routine ->
    // , not null-tested); each used entry queued for release (bImmediate 0).
    unsigned int Affiliation = GetAffiliation();
    FFactionNames* Names = GetFactionNames(&Affiliation);
    if (PilotFirst != 1 || PilotLast != 1)
        Names->ReleasePilotName(PilotFirst, PilotLast, false);
    if (LargeShipName != 1)
        Names->ReleaseLargeShipName(LargeShipName, false);
    if (Desig != 1)
        Names->ReleaseFormationDesig(Desig, DesigNumber, false);

    // pub::Reputation::Free when a reputation was allocated.
    if (Reputation != -1)
        pub::Reputation::Free(Reputation);
    // .
    --GNpcNameReputationCount;
    // the base dtor the routine (vptr reset only) follows.
}

// (06f31ec0-06f31fff).
bool FNpcNameReputation::GetFormationDesig(int* Out) const
{
    FmtStr First;
    FmtStr Second;
    pub::Reputation::GetName(Reputation, First, Second);
    // *Out = 1, then the switch table 06f32004 / 06f32010 (the the routine
    // body inlined).
    return ::GetFirstNameValue(&First, &Second, Out);
}

// (06f32020-06f32162).
bool FNpcNameReputation::GetFormationDesigNumber(int* Out) const
{
    FmtStr First;
    FmtStr Second;
    pub::Reputation::GetName(Reputation, First, Second);
    // *Out = -1, then the switch table 06f32168 / 06f32174 (the the routine
    // body inlined: node 4, 0x3f24 node 3).
    return GetFirstNameLastValue(&First, &Second, Out);
}

// (06f32190-06f32298).
bool FNpcNameReputation::RollNameValue(int* Out) const
{
    FmtStr First;
    FmtStr Second;
    pub::Reputation::GetName(Reputation, First, Second);
    // rand() * 5, signed / 32768 (CDQ, AND 0x7fff, SAR 0xf), + 1.
    return RollNpcNameValue(&First, &Second, Out);
}

// (06f319b0-06f319cc).
bool __cdecl PickNpcNameGender(unsigned int Affiliation, FNpcNameParams* Params)
{
    return GetFactionProps(&Affiliation)->PickGender(&Params->Gender);
}

// (06f319d0-06f31d3b).
bool __cdecl BuildNpcName(unsigned int Affiliation, FNpcNameRequest const* Request,
                          FNpcNameParams* Out, bool bNamedLeader)
{
    // .
    Out->Affiliation = Affiliation;
    FFactionNames* Names = GetFactionNames(&Affiliation);

    if (Request->NameIndex != 0)
    {
        // the arch type tested against 0x30000 (TEST + SETNZ) = FIGHTER
        // 0x10000 | FREIGHTER 0x20000 (Common's object type name table 063ec8a0).
        unsigned int ShipType;
        pub::GetType(Request->ShipArchId, ShipType);
        bool const bFighterOrFreighter = (ShipType & 0x30000) != 0;

        // the first name's pattern.
        unsigned int FirstId;
        if (Request->FormationSize == 1)
            FirstId = bFighterOrFreighter ? 0x3f17 : 0x3f19;
        else if (bFighterOrFreighter)
            FirstId = Request->EscortNumber == 0 ? 0x3f20 : 0x3f1d;
        else
            FirstId = 0x3f1b;
        Out->First.begin_mad_lib(FirstId);
        // short group name, the type name.
        Out->First.append_string(Names->GetShortGroupName());
        Out->First.append_string(Names->GetNameId(Request->NameIndex));
        // the formation designation - the request's own (counted as used)
        // or a new reserved one.
        int Desig = Request->Desig;
        int DesigNumber = Request->DesigNumber;
        if (Desig != 0)
            Names->AddFormationDesigUse(Desig, DesigNumber);
        else
            Names->PickFormationDesig(&Desig, &DesigNumber, true);
        // .
        Out->First.append_string(static_cast<unsigned int>(Desig));
        Out->First.append_int(static_cast<unsigned int>(DesigNumber));
        Out->First.end_mad_lib();

        if (bFighterOrFreighter)
        {
            // the second name's pattern.
            unsigned int SecondId;
            if (Request->FormationSize == 1)
                SecondId = 0x3f18;
            else if (Request->EscortNumber == 0)
                SecondId = 0x3f21;
            else
                SecondId = 0x3f1e;
            Out->Second.begin_mad_lib(SecondId);
            int PilotFirst = 0;
            int PilotLast = 0;
            if (Out->Second.MadLibId != 0x3f1e)
            {
                // rank designation, then a reserved pilot first + last name.
                Out->Second.append_string(Names->GetRankDesig(&Request->RankLevel));
                Names->PickPilotName(&PilotFirst, &PilotLast, Out->Gender, true);
                Out->Second.append_string(static_cast<unsigned int>(PilotFirst));
                Out->Second.append_string(static_cast<unsigned int>(PilotLast));
                Out->Second.end_mad_lib();
                return true;
            }
            // "Escort" (0x3f1f), the escort number, rank designation, a
            // reserved last name only (null OutFirst).
            Out->Second.append_string(0x3f1f);
            Out->Second.append_int(static_cast<unsigned int>(Request->EscortNumber));
            Out->Second.append_string(Names->GetRankDesig(&Request->RankLevel));
            Names->PickPilotName(nullptr, &PilotLast, Out->Gender, true);
            Out->Second.append_string(static_cast<unsigned int>(PilotLast));
            Out->Second.end_mad_lib();
            return true;
        }

        // any other ship type: 0x3f1a / 0x3f1c, the ship arch's name, the
        // faction's large ship designation, a reserved large ship name.
        Out->Second.begin_mad_lib(Request->FormationSize == 1 ? 0x3f1a : 0x3f1c);
        unsigned int ShipName;
        pub::GetShipArchSTRID(Request->ShipArchId, ShipName);
        Out->Second.append_string(ShipName);
        Out->Second.append_string(static_cast<unsigned int>(Names->GetLargeShipDesig()));
        int LargeShipName;
        Names->PickLargeShipName(&LargeShipName, true);
        Out->Second.append_string(static_cast<unsigned int>(LargeShipName));
        Out->Second.end_mad_lib();
        return true;
    }

    // a formation leader also gets 0x3f24 with the short group name and a
    // designation.
    if (bNamedLeader)
    {
        Out->First.begin_mad_lib(0x3f24);
        Out->First.append_string(Names->GetShortGroupName());
        int Desig = Request->Desig;
        int DesigNumber = Request->DesigNumber;
        if (Desig != 0)
            Names->AddFormationDesigUse(Desig, DesigNumber);
        else
            Names->PickFormationDesig(&Desig, &DesigNumber, true);
        Out->First.append_string(static_cast<unsigned int>(Desig));
        Out->First.append_int(static_cast<unsigned int>(DesigNumber));
        Out->First.end_mad_lib();
    }

    // 0x3f22 with the rank designation, or 0x3f23; then a reserved pilot
    // first + last name.
    if (Request->RankLevel != 0)
    {
        Out->Second.begin_mad_lib(0x3f22);
        Out->Second.append_string(Names->GetRankDesig(&Request->RankLevel));
    }
    else
    {
        Out->Second.begin_mad_lib(0x3f23);
    }
    int PilotFirst = 0;
    int PilotLast = 0;
    Names->PickPilotName(&PilotFirst, &PilotLast, Out->Gender, true);
    Out->Second.append_string(static_cast<unsigned int>(PilotFirst));
    Out->Second.append_string(static_cast<unsigned int>(PilotLast));
    Out->Second.end_mad_lib();
    return true;
}

// (06f31910-06f319a6).
bool __cdecl CreateNpcNameReputation(FNpcNameParams const* Params, FContentPlayerStateBase** Out)
{
    // operator new(0xc) and the inlined ctor (Alloc, SetAffiliation, ++count)
    // under EH states 0/1 (06fae9c0 delete, 06fae9cb base dtor).
    FNpcNameReputation* Object = new FNpcNameReputation(Params);
    *Out = Object;
    return Object != nullptr;
}

// (06f31d40-06f31eb1).
bool __cdecl CreateMsnNpcReputation(FMsnNpc const* Npc, FContentPlayerStateBase** Out)
{
    // a local name block (two inline FmtStr ctors, +0x30 = 0, +0x34 = -1;
    // EH state 0 = its dtor the routine): an empty first name, the second name the mad lib
    // Npc->IndividualName, Affiliation = Npc->Affiliation.
    FNpcNameParams Name;
    Name.Second.begin_mad_lib(static_cast<unsigned int>(Npc->IndividualName));
    Name.Second.end_mad_lib();
    Name.Affiliation = Npc->Affiliation;

    // operator new(0xc) and the inlined ctor with &Name (EH states 1/2).
    FNpcNameReputation* Object = new FNpcNameReputation(&Name);
    *Out = Object;
    return Object != nullptr;
    // the block's dtor - ~FmtStr Second, ~FmtStr First (EH state 3).
}
