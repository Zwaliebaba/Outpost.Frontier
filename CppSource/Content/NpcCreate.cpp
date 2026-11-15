// Author: Giperion.

// Content.dll TU24 (06ed5390-06ed5a70): solar NPCs (see NpcCreate.h).

#include "NpcCreate.h"
#include "BaseDB.h"               // FindContentBaseInfo (TU17 the routine)
#include "PilotPersonalities.h"   // GetPilotPersonality (TU45 the routine)
#include "SystemInfo.h"           // FSystemInfo::SystemId
#include "ContentStaticIds.h"     // TU24's 16 static-init thunks 06ed5390-06ed5570
#include "common_id.h"
#include "common_ini.h"
#include "common_newchar.h"       // pub::StateGraph::get_state_graph
#include "common_server.h"

// (06ed5590-06ed571f). Result starts true and is cleared only when no key
// matched:
//   "nickname": Desc->Nickname = get_value_string() (TString<0x30>: null -> empty,
//               else strlen clamped to 0x2f).
//   "base":     only while Desc->Base is null: GetBaseID(local, get_value_string()),
//               Desc->Base = the routine(&local).
//   "reputation": GetReputationGroup(Desc->ReputationGroup, get_value_string()).
//   otherwise Desc's CommReferrable slot 2 (read_comm_ini_data) first; when it returns
//   false: "pilot": GetPilotPersonality(CreateID(value), &Desc->Personality) == true ->
//               bHasPersonality = true; "behavior": StateGraph = get_state_graph(value, 0).
bool __cdecl ParseSolarNpcKey(INI_Reader* Reader, FSolarNpcDesc* Desc)
{
    bool bResult = true;
    if (Reader->is_value("nickname"))
    {
        Desc->Nickname = Reader->get_value_string();
        return bResult;
    }
    if (Reader->is_value("base"))
    {
        if (Desc->Base != nullptr)
            return bResult;
        unsigned int BaseId;
        pub::GetBaseID(BaseId, Reader->get_value_string());
        Desc->Base = FindContentBaseInfo(&BaseId);
        return bResult;
    }
    if (Reader->is_value("reputation"))
    {
        pub::Reputation::GetReputationGroup(Desc->ReputationGroup, Reader->get_value_string());
        return bResult;
    }
    if (Desc->read_comm_ini_data(*Reader))
        return bResult;
    if (Reader->is_value("pilot"))
    {
        unsigned int PilotId = CreateID(Reader->get_value_string());
        if (GetPilotPersonality(&PilotId, &Desc->Personality))
            Desc->bHasPersonality = true;
        return bResult;
    }
    if (Reader->is_value("behavior"))
    {
        Desc->StateGraph = pub::StateGraph::get_state_graph(Reader->get_value_string(),
                                                            pub::StateGraph::LEADER);
        return bResult;
    }
    bResult = false;
    return bResult;
}

// The ctor inlined at 06ed5883-06ed58ae (inside the new-expression of the routine): base
// (its unused argument is the Group local), vptr 06fb61a0, then the reputation
// of the solar's nickname and its affiliation -- both under EH state 2 (06fab223 base dtor,
// operator delete).
FSolarNpcReputation::FSolarNpcReputation(char const* Nickname, unsigned int const& Group)
{
    pub::Reputation::GetReputation(Reputation, Nickname);
    pub::Reputation::SetAffiliation(Reputation, Group);
}

// (06ed5720-06ed58d8). Order: TU20 ctor with &Desc's parameters (null-checked
// base conversion); CommReferrable default (+0x11c = 0, +0x120 = 0); ContentCallback vptr;
// +0x164 = 0; +0x168 = Desc->StateGraph; Personality ctor; final vptrs; +0x544 =
// Desc->bHasPersonality and, when set, the 0x3d8-byte personality copy; SpaceObjId =
// GetNicknameId(Nickname.Data); MsgIdPrefix = Desc's prefix (TString<0x40> assign, clamp
// 0x3f). When (SpaceObjId & 0x3fffffff) != 0 and Desc->ReputationGroup != -1: a new
// FSolarNpcReputation takes +8 (the raw copy from the parameters is overwritten, not
// released), its Reputation = GetReputation(Nickname.Data), then SetAffiliation(its
// Reputation, ReputationGroup). Otherwise +8 = 0 (even when the parameters carried one).
FSolarNpc::FSolarNpc(FSolarNpcDesc const* Desc)
    : FContentSpaceObj(Desc)
    , StateGraph(Desc->StateGraph)
    , bHasPersonality(Desc->bHasPersonality)
{
    if (bHasPersonality)
        Personality = Desc->Personality;

    SpaceObjId = pub::GetNicknameId(Nickname.Data);
    MsgIdPrefix = Desc->MsgIdPrefix.Data;

    if ((SpaceObjId & 0x3fffffff) != 0 && Desc->ReputationGroup != 0xffffffffu)
    {
        unsigned int Group = Desc->ReputationGroup;
        ReputationHandle = new FSolarNpcReputation(Nickname.Data, Group);
    }
    else
    {
        ReputationHandle = nullptr;
    }
}

// .
bool FSolarNpc::Slot04()
{
    return Field_0x164;
}

// (06ed5a00-06ed5a60), ContentCallback +0x1c. Message block (0x10, stack
// ) = {Type, Param1, Param2, the routine(this - 0x160)->SystemId};
// +0x164 = (Type == 0); the controller id local (the reused Type slot) = 0; then
// pub::Controller::_SendMessage(&0, 0x1001, &block).
void FSolarNpc::combat_state_notification(CSNType Type, uint Param1, uint Param2)
{
    FSpaceObjCombatStateMsg Msg;
    Msg.Type = static_cast<unsigned int>(Type);
    Msg.Param1 = Param1;
    Msg.Param2 = Param2;
    Msg.SystemId = GetSystem()->SystemId;
    Field_0x164 = (Type == 0);
    unsigned int Controller = 0;
    pub::Controller::_SendMessage(Controller, pub::Controller::EMessage::CombatState, &Msg);
}

// (06ed5940-06ed5959): +8 ? vtbl[+8](1) (scalar deleting dtor), +8 = 0.
void FSolarNpc::DeleteReputationHandle()
{
    if (ReputationHandle != nullptr)
        delete ReputationHandle;
    ReputationHandle = nullptr;
}

// (06ed5960-06ed59e6): only when +8 is set, bSubmit, and StateGraph != -1:
// SetPersonalityParams P (default ctor); P+0xc = StateGraph, P+0x10 = &ContentCallback
// subobject (+0x160), P+0x14 = 0, P+0x18 = bHasPersonality; the personality copied in when
// set; pub::AI::SubmitState(SpaceObjId, &P) (result dropped).
void FSolarNpc::SubmitPersonalityState(bool bSubmit)
{
    if (ReputationHandle == nullptr || !bSubmit || StateGraph == -1)
        return;
    pub::AI::SetPersonalityParams Params;
    Params.StateGraphId = StateGraph;
    Params.ContentCallbackObject = static_cast<pub::AI::ContentCallback*>(this);
    Params.DirectiveCallbackObject = nullptr;
    Params.bSetPersonality = bHasPersonality ? 1 : 0;
    if (bHasPersonality)
        Params.Value = Personality;
    pub::AI::SubmitState(SpaceObjId, &Params);
}
