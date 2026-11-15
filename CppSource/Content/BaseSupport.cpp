// Author: Giperion.

// Content.dll TU19 (06eb3ec0-06eb6c00): the BaseSupport controller.
// CreateInstance("BaseSupport") -> new 8 -> the routine; vtable 06fb4618
// (slot 0 the branch HandleMessage, slot 1 the routine deleting dtor).

// TU19's own 16 static-ID initializers (the field..the field),
// before GNewsItems (the branch, _initterm 06fcd548) and GBaseVisits (the branch,
// ).
#include "ContentStaticIds.h"

#include "BaseSupport.h"

#include "ContentInternal.h"
#include "ContentPlayerState.h"
#include "ContentStageNames.h"
#include "GenericScripts.h"     // StartBarCharacterScript (the routine)
#include "LootProps.h"          // BuildMissionVendorOffers / ClearMissionVendorOffers (TU35)
#include "Populators.h"         // BasePopHasPlayer (the routine)
#include "RandomMission.h"      // FRandomMissionLoader::CanAccept / Release
#include "common_ini.h"
#include "common_server.h"
#include "common_server_controller.h"
#include "common_text.h"        // FmtStr
#include "common_tstring.h"

#include <algorithm>
#include <cstring>
#include <intrin.h>             // __debugbreak -- the two baseline INT3s of msg 2
#include <new>

// ---------------------------------------------------------------------------------------
// News (missions\news.ini)
// ---------------------------------------------------------------------------------------

std::vector<FNewsItem> GNewsItems;

namespace
{
    // (06eb5f50-06eb5fa6): FmtStr::destroy then zero all 0x18 bytes.
    void ResetNewsFmtStr(FmtStr& Str)
    {
        Str.destroy();
        Str.MadLibId = 0;
        memset(Str.ValueIndex, 0, sizeof(Str.ValueIndex));
    }

    void ResetNewsDescription(NewsBroadcastDescription& Desc)
    {
        Desc.BaseId = 0;
        Desc.bFlag1 = false;
        Desc.bFlag2 = false;
        Desc.Category = 0;
        ResetNewsFmtStr(Desc.Headline);
        ResetNewsFmtStr(Desc.Text);
        ResetNewsFmtStr(Desc.Audio);
        Desc.LogoTexture = nullptr;
        Desc.AudioName = nullptr;
        Desc.Filter = 0;
    }

    // The 43-entry stage table walk inlined twice in the routine (06eb418e, 06eb41da):
    // null names are skipped, the first stricmp match wins, a miss leaves Value alone.
    void LookupNewsStage(char const* Name, int& Value)
    {
        for (int Index = 0; Index < 43; ++Index)
        {
            if (GContentStageNames[Index].Name != nullptr &&
                _stricmp(Name, GContentStageNames[Index].Name) == 0)
            {
                Value = GContentStageNames[Index].Value;
                return;
            }
        }
    }

    // (06eb40c0-06eb4430): the values of one [NewsItem] section.
    void __cdecl LoadNewsItemValues(INI_Reader* Reader, FNewsItem* Item)
    {
        // [ESP+0x14]: one stage slot shared by both "rank" lookups and kept across
        // values. baseline never initialises it; it is read uninitialised only when the
        // first rank name is unknown (every rank in news.ini is a table name).
        int StageValue = 0;

        while (Reader->read_value())
        {
            if (Reader->is_value("base"))
            {
                unsigned int BaseId;
                pub::GetBaseID(BaseId, Reader->get_value_string());
                // insert-unique (the routine at end); the returned
                // pair<iterator, bool> is dropped.
                if (std::find(Item->Bases.begin(), Item->Bases.end(), BaseId) == Item->Bases.end())
                    Item->Bases.insert(Item->Bases.end(), BaseId);
            }
            else if (Reader->is_value("rank"))
            {
                LookupNewsStage(Reader->get_value_string(0), StageValue);
                Item->FromStage = StageValue;
                LookupNewsStage(Reader->get_value_string(1), StageValue);
                Item->ToStage = StageValue;
            }
            else if (Reader->is_value("breaking"))
            {
                Item->Desc.bFlag1 = true;
            }
            else if (Reader->is_value("autoselect"))
            {
                Item->Desc.bFlag2 = true;
            }
            else if (Reader->is_value("icon"))
            {
                // Each compare re-reads the value; no match leaves Category alone.
                if (_stricmp(Reader->get_value_string(), "critical") == 0)
                    Item->Desc.Category = 1;
                else if (_stricmp(Reader->get_value_string(), "world") == 0)
                    Item->Desc.Category = 2;
                else if (_stricmp(Reader->get_value_string(), "mission") == 0)
                    Item->Desc.Category = 3;
                else if (_stricmp(Reader->get_value_string(), "system") == 0)
                    Item->Desc.Category = 4;
                else if (_stricmp(Reader->get_value_string(), "faction") == 0)
                    Item->Desc.Category = 5;
                else if (_stricmp(Reader->get_value_string(), "universe") == 0)
                    Item->Desc.Category = 6;
            }
            else if (Reader->is_value("category"))
            {
                Item->Desc.Headline.begin_mad_lib(Reader->get_value_int(0));
                Item->Desc.Headline.end_mad_lib();
            }
            else if (Reader->is_value("headline"))
            {
                Item->Desc.Text.begin_mad_lib(Reader->get_value_int(0));
                Item->Desc.Text.end_mad_lib();
            }
            else if (Reader->is_value("text"))
            {
                FmtStr& Text = Item->Desc.Audio;
                Text.begin_mad_lib(Reader->get_value_int(0));
                for (unsigned int Index = 1; !Reader->is_value_empty(Index); ++Index)
                    Text.append_string(Reader->get_value_int(Index));
                Text.end_mad_lib();
            }
            else if (Reader->is_value("logo"))
            {
                char* Logo = StringAlloc(Reader->get_value_string(), false);
                Item->Logo.Value = Logo;
                Item->Desc.LogoTexture = Logo;
            }
            else if (Reader->is_value("audio"))
            {
                char* Audio = StringAlloc(Reader->get_value_string(), false);
                Item->Audio.Value = Audio;
                Item->Desc.AudioName = Audio;
            }
        }
    }
}

// (inlined in the loader): vector empty, both CacheStrings cleared,
// the three FmtStrs default-constructed (the routine) then the routine, stages 0..0x2a.
FNewsItem::FNewsItem()
    : FromStage(0), ToStage(0x2a)
{
    Audio.clear();
    Logo.clear();
    ResetNewsDescription(Desc);
}

// (06eb4680-06eb471f): destroy the three FmtStrs, then their destructors
// and the vector. The StringAlloc'd strings are not freed.
FNewsItem::~FNewsItem()
{
    Desc.Headline.destroy();
    Desc.Text.destroy();
    Desc.Audio.destroy();
}

// (06eb44c0-06eb4677).
void __cdecl LoadNewsItems(INI_Reader* Reader)
{
    while (Reader->read_header())
    {
        if (Reader->is_header("NewsItem"))
        {
            FNewsItem Item;
            LoadNewsItemValues(Reader, &Item);
            GNewsItems.insert(GNewsItems.end(), 1, Item);
        }
    }
}

// ---------------------------------------------------------------------------------------
// BaseSupport
// ---------------------------------------------------------------------------------------

namespace
{
    struct FBaseSupport : IDLL
    {
        unsigned int ControllerId;                            // +4

        explicit FBaseSupport(unsigned int const& InControllerId);
        ~FBaseSupport() override;
        void HandleMessage(pub::Controller::EMessage MessageId, void const* Parms) override;
    };

    // (06eb4ad0-06eb4b28). No heartbeat is registered.
    FBaseSupport::FBaseSupport(unsigned int const& InControllerId)
        : ControllerId(InControllerId)
    {
        LoadGenericScripts("scripts\\gcs\\genericScripts.ini");
    }

    // (06eb4b50-06eb4b95).
    FBaseSupport::~FBaseSupport()
    {
        UnloadGenericScripts();
    }

    // The inlined "start the script of every character in the player's room" walk of
    // msg 0x1b 06eb4cb7-06eb4d22, 0x29 06eb4f7a-06eb4fe2, 0x100b
    // , the 0x2c tail 06eb531d-06eb5360. GetBaseRecord (the routine)
    // and the room (the routine on the routine = +0x14) are null-tested; the iterator
    // is advanced after the call (the routine postfix ++ at 06eb4d14 / 06eb4fd4 /
    // , MOV ESI,[ESI] at 06eb5356), and end() is re-read each pass.
    void StartRoomCharacterScripts(FContentPlayerState* State)
    {
        FContentBase* Base = State->GetBaseRecord();
        if (Base == nullptr)
            return;
        unsigned int RoomId = State->LocationId;
        FBaseRoomState* Room = Base->FindRoomState(&RoomId);
        if (Room == nullptr)
            return;
        for (FBarCharacterList::iterator It = Room->Characters.begin();
             It != Room->Characters.end(); ++It)
            StartBarCharacterScript(It);
    }

    // (06eb4a00-06eb4ac2, cdecl; caller msg 0x1b 06eb4caa): a rumor was told.
    // Every other character of the room offering the same rumor (Ids[0], the routine)
    // loses its offer; each of them, the teller included, restarts its script. Base is not
    // null-tested, the room is.
    void __cdecl OnRumorTold(FContentPlayerState* State, FBarCharacterList::iterator Teller)
    {
        FContentBase* Base = State->GetBaseRecord();
        unsigned int RoomId = State->LocationId;
        FBaseRoomState* Room = Base->FindRoomState(&RoomId);
        if (Room == nullptr)
            return;
        int const RumorId = Teller->Offer.Rumor->Ids[0];
        for (FBarCharacterList::iterator It = Room->Characters.begin();
             It != Room->Characters.end(); ++It)
        {
            if (It->Offer.Kind != BAR_OFFER_RUMOR || It->Offer.Rumor->Ids[0] != RumorId)
                continue;
            if (It != Teller)
            {
                // an empty offer local (EH state 0, dtor the routine).
                FBarOffer NoOffer;
                It->SetOffer(&NoOffer);
            }
            StartBarCharacterScript(It);
        }
    }

    // (06eb4ba0-06eb537a, RET 8): messages 1, 2, 0x1b, 0x29, 0x2c, 0x100b
    // (JG / JE on 0x29, then DEC / DEC / SUB 0x19; 0x2c and 0x100b above).
    void FBaseSupport::HandleMessage(pub::Controller::EMessage MessageId, void const* Parms)
    {
        switch (MessageId)
        {
        case pub::Controller::EMessage::BaseEnter:
        {
            // a player entered base Msg[0].
            unsigned int const* Msg = static_cast<unsigned int const*>(Parms);
            FContentPlayerState* State = FindContentPlayerState(Msg[1]);
            FContentBase* Base = FindContentBaseInfo(&Msg[0]);
            // [ESP+0x14]: left unset by baseline when the query fails.
            unsigned int ShipId = 0;
            pub::Player::GetShipID(Msg[1], ShipId);
            // [ESP+0x44] vector (the routine, EH state 0) + [ESP+0x54] list
            // (the routine, EH state 1).
            FBaseVisit Visit;
            if (ShipId != 0)
                BuildMissionVendorOffers(Base, &Visit, State);
            // no null test on Base or State.
            unsigned int const BaseId = Base->BaseId;
            for (std::vector<FNewsItem>::iterator Item = GNewsItems.begin();
                 Item != GNewsItems.end(); ++Item)
            {
                // (find), then signed JL / JG on the stage bounds.
                if (std::find(Item->Bases.begin(), Item->Bases.end(), BaseId) == Item->Bases.end())
                    continue;
                int const Stage = static_cast<int>(State->MissionNumber);
                if (Stage < Item->FromStage || Stage > Item->ToStage)
                    continue;
                Item->Desc.BaseId = BaseId;
                unsigned long const Handle = pub::GF::NewsBroadcastCreate(Item->Desc);
                Visit.NewsHandles.push_back(Handle);
            }
            // pair copy (the routine, the offers are copied without a
            // reference) and insert-unique (the routine); a second record for the player is
            // dropped.
            GBaseVisits.insert(std::pair<unsigned int const, FBaseVisit>(Msg[1], Visit));
            break;
        }

        case pub::Controller::EMessage::BaseExit:
        {
            // a player left base Msg[0].
            unsigned int const* Msg = static_cast<unsigned int const*>(Parms);
            FindContentPlayerState(Msg[1]);                                   // result unused
            FContentBase* Base = FindContentBaseInfo(&Msg[0]);
            std::map<unsigned int, FBaseVisit>::iterator Visit = GBaseVisits.find(Msg[1]);
            if (Visit == GBaseVisits.end())
            {
                // JZ -> 06eb4dbc: INT3 (an assertion), then on to the Base test.
                __debugbreak();
            }
            else
            {
                for (std::vector<unsigned long>::iterator Handle =
                         Visit->second.NewsHandles.begin();
                     Handle != Visit->second.NewsHandles.end(); ++Handle)
                    pub::GF::NewsBroadcastDestroy(&*Handle);
                Visit->second.NewsHandles.clear();
                ClearMissionVendorOffers(&Visit->second);
                GBaseVisits.erase(Visit);
            }
            if (Base == nullptr)
                break;
            // the routine true -> INT3 (an assertion: BasePop must
            // have let the player go); false jumps over it to 06eb4dd6.
            if (BasePopHasPlayer(Msg[1]))
                __debugbreak();
            Base->Unload();
            break;
        }

        case pub::Controller::EMessage::GFObjSelect:
        {
            // the player talked to bar character Msg[0].
            unsigned int const* Msg = static_cast<unsigned int const*>(Parms);
            FContentPlayerState* State = FindContentPlayerState(Msg[1]);
            if (State == nullptr)
                break;
            FContentBase* Base = State->GetBaseRecord();
            if (Base == nullptr)
                break;
            FBarCharacterList::iterator Character;
            FBaseRoomState* Room = nullptr;
            if (!Base->FindBarCharacter(&Character, &Room, Msg[0]))
                break;
            State->RecordVnpcTalk(Base->BaseId, Character->Nickname.Id);
            Character->CountTalk();
            if (Character->Offer.Kind != BAR_OFFER_RUMOR)
            {
                StartRoomCharacterScripts(State);
                break;
            }
            // .
            State->AddRumor(static_cast<unsigned int>(Character->Offer.Rumor->Ids[0]));
            // {player, TString<32> role} (the routine copies the role) to controller 0
            // (the field, a constant 0).
            struct FRumorToldMsg
            {
                unsigned int PlayerId;
                TString<32> Role;
            } RumorMsg;
            RumorMsg.PlayerId = Msg[1];
            RumorMsg.Role.Length = 0;
            RumorMsg.Role.Data[0] = '\0';
            RumorMsg.Role = Character->Role.Data;
            unsigned int const NoController = 0;
            pub::Controller::_SendMessage(NoController, pub::Controller::EMessage::RumorHeard, &RumorMsg);
            OnRumorTold(State, Character);
            break;
        }

        case pub::Controller::EMessage::LocationEnter:
        case pub::Controller::EMessage::PlayerMissionChanged:
        {
            // / 06eb4ffb-06eb5082 (identical): the player's room restarts.
            unsigned int const* Msg = static_cast<unsigned int const*>(Parms);
            FContentPlayerState* State = FindContentPlayerState(Msg[1]);
            if (State == nullptr)
                break;
            StartRoomCharacterScripts(State);
            break;
        }

        case pub::Controller::EMessage::MissionResponse:
        {
            // a mission offer was answered.
            FMissionOfferAcceptMsg const* Msg = static_cast<FMissionOfferAcceptMsg const*>(Parms);
            if (!Msg->bAccepted)
                break;
            FContentPlayerState* State = FindContentPlayerState(Msg->PlayerId);
            if (Msg->Kind == 2)
            {
                // a vendor offer. The visit lookup is not tested for
                // end(), nor State for null.
                std::map<unsigned int, FBaseVisit>::iterator Visit =
                    GBaseVisits.find(Msg->PlayerId);
                std::list<FBaseVendorOffer>& Offers = Visit->second.VendorOffers;
                std::list<FBaseVendorOffer>::iterator Offer = Offers.begin();
                for (; Offer != Offers.end(); ++Offer)
                {
                    if (Offer->OfferId == Msg->OfferId)
                        break;
                }
                if (Offer != Offers.end())
                {
                    FmtStr Reason;                                             // EH state 4
                    bool bAccept = false;
                    if (State->GetGroupLiveMission() == 0)
                    {
                        // Loader slot 7 (+0x1c); the code goes to [ESP+0x3c].
                        int Code = 0;
                        bAccept = Offer->Data.Loader->CanAccept(Offer->Data.Mission, &Reason,
                                                                 &Code);
                        if (bAccept)
                        {
                            unsigned int Controller = pub::Controller::Create(
                                "content.dll", Offer->Data.Type.Data, Offer->Data.Mission,
                                pub::Controller::ABOVE_NORMAL);
                            ID_String NoVnpc;
                            NoVnpc.clear();
                            State->JoinMissionAsGroup(Controller, 0, &NoVnpc, false, 0);
                            FMissionOfferAcceptMsg Forward = *Msg;
                            pub::Controller::_SendMessage(Controller, pub::Controller::EMessage::MissionResponse, &Forward);
                        }
                    }
                    else
                    {
                        // .
                        Reason.begin_mad_lib(0x730);
                        Reason.end_mad_lib();
                    }
                    pub::GF::MissionVendorAcceptance(Offer->VendorHandle, bAccept, Reason,
                                                     Msg->PlayerId);
                    if (bAccept)
                    {
                        // no slot 5 here, only Release.
                        pub::GF::MissionVendorOfferDestroy(&Offer->VendorHandle);
                        Offer->Data.Loader->Release();
                        Offers.erase(Offer);
                    }
                }
            }
            else if (Msg->Kind == 1)
            {
                // a bar character's offer. Base and room are not
                // null-tested here.
                FContentBase* Base = State->GetBaseRecord();
                unsigned int RoomId = State->LocationId;
                FBaseRoomState* Room = Base->FindRoomState(&RoomId);
                for (FBarCharacterList::iterator It = Room->Characters.begin();
                     It != Room->Characters.end(); ++It)
                {
                    if (It->CharacterId != Msg->OfferId)
                        continue;
                    if (It->Offer.Accept(State, &*It))
                    {
                        It->SetBehavior(0xffffffffu);
                        FBarOffer NoOffer;                                     // EH state 5
                        It->SetOffer(&NoOffer);
                    }
                }
            }
            // .
            if (State == nullptr)
                break;
            StartRoomCharacterScripts(State);
            break;
        }

        default:
            break;
        }
    }
}

std::map<unsigned int, FBaseVisit> GBaseVisits;

// (06eb55c0-06eb562a): lower_bound the routine + "not below" test = find.
void __cdecl RefreshMissionVendorOffers(FContentPlayerState* State)
{
    std::map<unsigned int, FBaseVisit>::iterator Visit = GBaseVisits.find(State->PlayerId);
    if (Visit == GBaseVisits.end())
        return;
    ClearMissionVendorOffers(&Visit->second);
    BuildMissionVendorOffers(State->GetBaseRecord(), &Visit->second, State);
}

IDLL* CreateBaseSupport(char const* /*Name*/, unsigned int ControllerId, void const* /*Parms*/)
{
    return new (std::nothrow) FBaseSupport(ControllerId);
}
