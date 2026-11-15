// Author: Giperion.

#pragma once

// Content.dll TU24 (06ed5390-06ed5a70): solar NPCs — the [Object] entries of a system ini
// that carry a pilot / behavior / reputation. SystemInfo's full load (TU58 the routine)
// fills an FSolarNpcDesc per [Object] with the routine, constructs an FSolarNpc from it
// (the routine) and appends it to FSystemInfo +0x88 (vector of 0x548-byte records).

#include "NpcBase.h"
#include "ContentPlayerState.h"   // FContentPlayerStateBase
#include "common_net.h"           // CommReferrable
#include "common_pub.h"           // pub::AI::Personality, pub::AI::ContentCallback

class INI_Reader;

// Parameter block of one [Object] (baseline 0x470 bytes, built inline by TU58 06f748ef-
// ): CommReferrable at +0 (final vtable 06fbbf30 = the three CommReferrable import
// thunks, no overrides), the space object parameters at +0x48 (the routine), then the
// members below. Its only cleanup is the parameters' handle release (TU58 06f74a3d).
struct FSolarNpcDesc : public CommReferrable, public FContentSpaceObjParams
{
    int StateGraph = -1;                     // +0x8c "behavior" (pub::StateGraph::get_state_graph)
    pub::AI::Personality Personality;        // +0x90 "pilot"
    bool bHasPersonality = false;            // +0x468 set when "pilot" resolved
    unsigned int ReputationGroup = 0xffffffffu; // +0x46c "reputation" (GetReputationGroup)
};

// (cdecl): one key of an [Object] section. Returns false only when no key
// matched (the CommReferrable slot 2 fallback included).
bool __cdecl ParseSolarNpcKey(INI_Reader* Reader, FSolarNpcDesc* Desc);

// The reputation handle a solar NPC creates for itself (the routine 06ed5865-06ed58b1):
// base ctor the routine, own vtable 06fb61a0 {AddRef, Release, sdd 06ed58e0 -> base dtor
// }. No members of its own. The ctor (inlined) runs GetReputation and
// SetAffiliation itself: they sit in EH state 2 of FuncInfo 06fbf748 (base dtor 06fab223, then
// operator delete 06fab218), so a throw from either frees the block.
struct FSolarNpcReputation : public FContentPlayerStateBase
{
    // vptr 06fb61a0; pub::Reputation::GetReputation(+8, Nickname);
    // pub::Reputation::SetAffiliation(+8, Group).
    FSolarNpcReputation(char const* Nickname, unsigned int const& Group);
    ~FSolarNpcReputation() override = default;
};

// baseline 0x548 bytes, vtable 06fb61e0 {sdd 06ed5910, slot 1 06ed59f0}; CommReferrable at
// +0x118 (vtable 06fb61d4, no overrides); ContentCallback at +0x160 (vtable 06fb61ac: the
// Common inline slots, combat_state_notification overridden by 06ed5a00).
class FSolarNpc : public FContentSpaceObj, public CommReferrable, public pub::AI::ContentCallback
{
public:
    // (RET 4).
    explicit FSolarNpc(FSolarNpcDesc const* Desc);
    // sdd 06ed5910 -> the routine (the TU20 dtor only).
    ~FSolarNpc() override = default;

    // Slot 1, 06ed59f0 (bytes 8a 81 64 01 00 00 c3, not inspected by analysis):
    // MOV AL,[ECX+0x164]; RET. TODO: slot name unknown (FContentSpaceObj +0x04).
    bool Slot04() override;

    // ContentCallback +0x1c, 06ed5a00 (RET 0xc).
    void combat_state_notification(CSNType Type, uint Param1, uint Param2) override;

    // delete the reputation handle through its slot 2, clear +8.
    void DeleteReputationHandle();
    // (RET 4): submit the personality state to pub::AI.
    void SubmitPersonalityState(bool bSubmit);

    // +0x164: 0 from the ctor; combat_state_notification stores (Type == 0).
    // TODO: meaning of the CSN type values unknown.
    bool Field_0x164 = false;
    int StateGraph;                          // +0x168
    pub::AI::Personality Personality;        // +0x16c
    bool bHasPersonality;                    // +0x544
};
