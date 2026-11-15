// Author: Giperion.

#pragma once

// Content.dll TU42, the NPC name part (06f31650-06f32000): the name request, the name
// builder over the TU48 faction name tables (FactionProps.h), and the reputation handle an
// NPC ship gets from the name (vtable 06fb940c). Callers: TU29 encounter spawn (06ef4d2b..),
// TU31 (06efbf99..), TU38 mission ship spawn the routine, TU60 (06f836a7..), TU61 (06f91ac9).
// The rest of TU42 (npcships.ini, [NPC] blocks, name value readers) is NpcShips.h.

#include "ContentPlayerState.h"   // FContentPlayerStateBase
#include "common_text.h"          // FmtStr

struct FMsnNpc;                   // NpcShips.h

// The name request read by BuildNpcName (0x20 bytes). the routine is its default ctor;
// (thiscall, RET 0x14; only caller TU29 06ef4d2b) sets the first five dwords
// and leaves +0x14..+0x1c to the caller. Both return this.
struct FNpcNameRequest
{
    // +0, +4, +8 = 0, +0xc = 1, +0x10 = 0, byte +0x14 = 0, +0x18 = 0, +0x1c = 0.
    FNpcNameRequest();
    // +0 = InNameIndex, +0xc = InFormationSize, +0x10 = InEscortNumber,
    // +4 = InDesig, +8 = InDesigNumber (the argument order of the baseline call). +0x14..+0x1c
    // are left unwritten, as in baseline.
    FNpcNameRequest(int InNameIndex, int InFormationSize, int InEscortNumber, int InDesig,
                    int InDesigNumber);

    // +0x00: index into FFactionNames::NameIds; non-zero builds the ship-type name
    // (FFactionNames::GetNameId), zero the pilot name.
    int NameIndex;
    int Desig;                  // +0x04 formation designation; 0 picks one (PickFormationDesig)
    int DesigNumber;            // +0x08 its number (used with a non-zero Desig)
    // +0x0c: the number of ships in the formation. The only non-default writer, TU29
    // NameFormationBlocks (06ef4d13 / 06ef4d22 -> the routine), passes the block list's size;
    // the default ctor sets 1. 1 (a lone ship) selects the 0x3f17 / 0x3f19 / 0x3f18 / 0x3f1a
    // patterns in BuildNpcName.
    int FormationSize;
    // +0x10: the ship's index in its formation, 0 = the leader. TU29 increments it per block
    // (06ef4e0d). For a fighter / freighter that is not alone, 0 selects 0x3f20 / 0x3f21 and a
    // non-zero value 0x3f1d / 0x3f1e, the latter appending it as the escort number:
    // UiText.dll 0x3f1e = "%s0 %d0 %s1 %s2" with %s0 = 0x3f1f "Escort".
    int EscortNumber;
    unsigned char Field_0x14;   // +0x14 TODO: written only by the routine; no reader in TU42
    int RankLevel;              // +0x18 FFactionNames::GetRankDesig; non-zero -> 0x3f22
    unsigned int ShipArchId;    // +0x1c pub::GetType / pub::GetShipArchSTRID
};

// The name built by BuildNpcName and turned into a reputation by CreateNpcNameReputation
// (0x38 bytes on the callers' stacks: two inline FmtStr ctors the routine, +0x30 = 0,
// +0x34 = -1; e.g. 06f15f56-06f15f7a).
struct FNpcNameParams
{
    FmtStr First;                       // +0x00 the Reputation::Alloc name
    FmtStr Second;                      // +0x18 the Reputation::Alloc nickname
    // +0x30: 1 male / 2 female (FFactionProps::PickGender, FVoiceProperties roles); passed to
    // FFactionNames::PickPilotName and FFactionProps::PickSpaceCostume.
    int Gender = 0;
    unsigned int Affiliation = 0xffffffffu;   // +0x34 written by BuildNpcName
};

// The reputation handle of a named NPC (0xc bytes, vtable 06fb940c {06eacd40 AddRef,
// Release, 06f316a0 sdd}). Its ctor is inlined in the routine / the routine:
// the base ctor the routine, the vtable, the reputation allocated from the name.
class FNpcNameReputation : public FContentPlayerStateBase
{
public:
    // The ctor inlined in the routine (06f3194c-06f31980) and the routine (06f31e2f-
    // ), inside the new-expression's EH states (base dtor the routine, operator
    // delete): base ctor the routine (its Params argument is not read), vptr 06fb940c,
    // pub::Reputation::Alloc(+8, Params->First, Params->Second),
    // pub::Reputation::SetAffiliation(+8, Params->Affiliation), ++GNpcNameReputationCount.
    explicit FNpcNameReputation(FNpcNameParams const* Params);
    // (called by the sdd 06f316a0): the name's pool entries are returned to the
    // faction name tables (queued, not immediate), the reputation freed, the live count
    // (the field) decremented, then the base dtor the routine.
    ~FNpcNameReputation() override;

    // The three readers of GCS the routine (06eeb3b3 / 06eeb430 / 06eeb4f4). Each gets the
    // reputation's name (pub::Reputation::GetName into two local FmtStr) and reads it back.
    // (thiscall, RET 4): the formation designation of the name's first part
    // (the the routine body: 1 and false for a pattern without one).
    bool GetFormationDesig(int* Out) const;
    // (thiscall, RET 4): the designation's number (the the routine body: -1 and
    // false for a pattern without one).
    bool GetFormationDesigNumber(int* Out) const;
    // (thiscall, RET 4): the name is fetched but not read; *Out = rand() * 5 /
    // 32768 + 1 (the the routine body); false.
    bool RollNameValue(int* Out) const;
};

// the number of live FNpcNameReputation objects (written only).
extern int GNpcNameReputationCount;

// (cdecl): GetFactionProps(&Affiliation)->PickGender(&Params->Gender); the
// faction record is not null-tested.
bool __cdecl PickNpcNameGender(unsigned int Affiliation, FNpcNameParams* Params);

// (cdecl): Out->Affiliation = Affiliation, then Out->First / Out->Second from the
// faction's name tables (GetFactionNames, not null-tested) by Request's fields; the picked
// names are reserved. Always true.
bool __cdecl BuildNpcName(unsigned int Affiliation, FNpcNameRequest const* Request,
                          FNpcNameParams* Out, bool bNamedLeader);

// (cdecl): a new FNpcNameReputation, its reputation allocated from Params'
// names with Params->Affiliation; *Out = it; true when it was created.
bool __cdecl CreateNpcNameReputation(FNpcNameParams const* Params, FContentPlayerStateBase** Out);

// (cdecl): the same from a mission [NPC] block: an empty name and the nickname
// mad lib Npc->IndividualName, affiliation Npc->Affiliation.
bool __cdecl CreateMsnNpcReputation(FMsnNpc const* Npc, FContentPlayerStateBase** Out);
