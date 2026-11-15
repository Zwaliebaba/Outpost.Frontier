// Author: Giperion.

#include "nn_help.h"

#include "best_path.h"              // BestPathGetWaypoint (the routine), FBestPathWaypoint
#include "character_select_equip.h" // GetPlayerStarSystemIdbaseline (the routine),
                                    // GetPlayerBaseIdbaseline (*the routine),
                                    // GetPlayerVisitFlagsbaseline (the routine)
#include "client_base_market.h"     // ClientMarketFill (the routine),
                                    // ClientMarketCollectCategory (the routine),
                                    // ClientMarketGetPrice (the routine)
#include "client_gameobj.h"         // FClientGameobj
#include "client_objdb.h"           // ClientObjDb_FindObject (the routine)
#include "hud_status_core.h"        // HudSampleShieldHitPoints (the routine)
#include "hud_target.h"             // HudObjectVisitFlags (the routine), HudObjectTypeIds
                                    // (the routine), GetObjectIdsName (the routine),
                                    // GetShipPilotName (the routine), NavMapSectorName
                                    // (the routine), FHudWideText
#include "hud_waypoint.h"           // GetHudWaypoint (the field), GetWaypointEntry
                                    // (the routine)
#include "ids_rdl.h"                // AppendTextMaybeXmlRdl (the routine), LoadIdsIntoRdl
                                    // (the routine), RdlAppendNode
#include "ids_strings.h"            // GetIdsStringWWithRawFallback (the routine)
#include "infocard_map.h"           // InfocardMapFind (the routine)
#include "menu_universe_base.h"     // GetClientCurrentBaseStridName (the routine)
#include "misc_util.h"              // FormatGroupedNumber (the routine),
                                    // FmtStrSprintf (the routine)
#include "mission_created_solars.h" // LoadMissionCreatedSolars (the routine)
#include "nn_inventory.h"           // IsPlayerDockedbaseline (the routine)
#include "nn_modal.h"               // NnMainButtonOnModalOpen (the routine)
#include "nn_navmap.h"              // NavMapZoneAt (the routine)
#include "nn_text_scratch.h"        // G_NnTextScratchA / B, G_NnTextScratchCapacity
#include "player_behavior.h"        // GetPlayerObjRW (the routine)
#include "rich_text_win.h"
#include "star_sys.h"               // GetAttitudeClass (the routine)
#include "win3d.h"                  // Win3DReparent (the routine), Win3DDeleteTree (the routine)
#include "win_factory.h"

#include "common_archetype.h"       // Archetype::GetEquipment / GetShip
#include "common_cobject.h"         // CObject, CSimple, CEqObj, CShip, CLoot
#include "common_goods.h"           // GoodList, GoodInfo, BaseDataList_get, Good2Arch
#include "common_iface.h"           // IObjRW, IObjInspectImpl
#include "common_id.h"              // CreateID
#include "common_reputation.h"      // Reputation::Vibe::GetAffiliation / GetInfocard,
                                    // Reputation::get_info_card
#include "common_text.h"            // RenderDisplayList, StyleNode
#include "common_universe.h"        // Universe::get_system, ISystem, IZone, FSystemObject

#include <list>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <wchar.h>
#include <windows.h>                // CharUpperW (the routine)

// WINFACTORY<NN_Help> static (the branch -> the routine; Create
// ; atexit the branch -> the routine).
WINFACTORY<NnHelp> G_NnHelpFactory("NN_Help");

namespace
{
    char const kInfoWindowMesh[] = "Interface\\NeuroNet\\Inventory\\CHAR_infowindow.cmp";
    char const kCloseButtonMesh[] = "Interface\\NeuroNet\\Inventory\\x.3db";

    wchar_t const kLineBreak[] = L"\n";
    wchar_t const kDoubleLineBreak[] = L"\n\n";
    wchar_t const kEmptyParagraph[] = L"<RDL><TEXT><PARA/></TEXT></RDL>";
    wchar_t const kTitleTail[] = L"\n    ";
    wchar_t const kItemTail[] = L"\n     ";
    wchar_t const kPriceLead[] = L" ";

    // / the field -- the id of the ship's target, kept by Create between its
    // head (0x00474c00) and its text (0x00474d23); nothing else reads or writes them.
    unsigned G_NnHelpPendingTargetId = 0;
    unsigned char G_bNnHelpPendingTarget = 0;

    // Not a baseline function: the append every text builder here spells out -- a string into
    // the second scratch buffer (wcscpy), then the routine with its wcslen and no text pointer.
    void AppendScratchB(RenderDisplayList& Card)
    {
        AppendTextMaybeXmlRdl(G_NnTextScratchB, static_cast<unsigned>(wcslen(G_NnTextScratchB)),
                              Card, false);
    }

    void AppendText(RenderDisplayList& Card, wchar_t const* Text)
    {
        wcscpy(G_NnTextScratchB, Text);
        AppendScratchB(Card);
    }

    void AppendStyle(RenderDisplayList& Card, unsigned short StyleId)
    {
        // StyleNode ctor, clone (vt+0x1c), the clone into the list (the routine or the same
        // push_back inline), the StyleNode dtor.
        StyleNode Style(StyleId);
        RdlAppendNode(Card, Style.Clone());
    }

    // Not a baseline function: the header the routine writes before each of its four lists
    // (e.g. 0x00476177..0x0047628f) -- style 0xf64, a line break, the title ids, the title tail
    // and style 0x9ca4.
    void AppendSectionTitle(RenderDisplayList& Card, unsigned TitleIds)
    {
        AppendStyle(Card, 0xf64);
        AppendText(Card, kLineBreak);
        LoadIdsIntoRdl(TitleIds, Card);
        AppendText(Card, kTitleTail);
        AppendStyle(Card, 0x9ca4);
    }

    // Not a baseline function: one priced line (0x00476564..0x004765b9 and 0x004767f3..
    // ) -- the name, then " " + the grouped price (ids 0x2140 prefix) + the item tail.
    void AppendPricedItem(RenderDisplayList& Card, unsigned NameIds, float Price)
    {
        LoadIdsIntoRdl(NameIds, Card);
        FormatGroupedNumber(G_NnTextScratchA, static_cast<int>(Price), -1); // ftol
        wcscpy(G_NnTextScratchB, kPriceLead);
        wcscat(G_NnTextScratchB, G_NnTextScratchA);
        wcscat(G_NnTextScratchB, kItemTail);
        AppendScratchB(Card);
    }

    // The ids string of Ids into Out (Chars wide characters), Out emptied when there is none
    // (e.g. 0x004755eb..0x004755f7: the routine, zero -> first character 0).
    void IdsIntoOrEmpty(unsigned Ids, wchar_t* Out, int Chars)
    {
        if (GetIdsStringWWithRawFallback(Ids, Out, Chars) == 0)
            Out[0] = L'\0';
    }

    // The "%d%\n" lines (u"%d%\n" at 0x005d0a48, MSVCRT swprintf): the legacy printf state
    // machine drops a '%' followed by a character that is no conversion and writes that
    // character, so baseline prints the number and a line break -- no percent sign. The port's
    // CRT rejects "%\n" as an invalid conversion, so the same output is spelled "%d\n".
    void FormatPercentLine(wchar_t* Out, int Value)
    {
        _swprintf(Out, L"%d\n", Value);
    }

    // 's 8-byte record: the entry and its index into the name buffer.
    struct FNnHelpNameSortRecord
    {
        FClientMarketGood const* Entry; // +0
        int NameIndex;                  // +4
    };
}

void NnHelpSortGoodsByName(std::list<FClientMarketGood const*>* Goods)
{
    // (cdecl). 0x00475df1..0x00475ed8: a vector of Count 8-byte records and one of
    // Count 0x40-wchar names, Count = the list's size (+8). The vectors' resize is the STL
    // / the routine / the routine and the routine / the routine / the routine.
    int const Count = static_cast<int>(Goods->size());
    std::vector<FNnHelpNameSortRecord> Records(static_cast<size_t>(Count));
    std::vector<wchar_t> Names(static_cast<size_t>(Count) * 0x40);

    // per node, in list order -- the record {entry, i} and the name:
    // (0x40) of GetEquipment(Good2Arch(good))'s ids_name, empty without one.
    int Index = 0;
    for (std::list<FClientMarketGood const*>::iterator It = Goods->begin(); It != Goods->end();
         ++It, ++Index)
    {
        FClientMarketGood const* const Entry = *It;
        Records[Index].Entry = Entry;
        Records[Index].NameIndex = Index;
        Archetype::Equipment const* const Equip =
            Archetype::GetEquipment(Good2Arch(Entry->Info.GoodId));
        wchar_t* const Name = &Names[static_cast<size_t>(Index) * 0x40];
        if (Equip == nullptr)
            Name[0] = L'\0';
        else
            GetIdsStringWWithRawFallback(static_cast<unsigned>(Equip->IdsName), Name, 0x40);
    }

    // bubble passes over the Count - 1 neighbour pairs (none when
    // Count - 1 <= 0) until one makes no swap; a pair swaps when _wcsicmp(first, second) > 0.
    bool bSorted = false;
    while (!bSorted)
    {
        bSorted = true;
        for (int Pair = 0; Pair < Count - 1; ++Pair)
        {
            FNnHelpNameSortRecord& A = Records[Pair];
            FNnHelpNameSortRecord& B = Records[Pair + 1];
            if (_wcsicmp(&Names[static_cast<size_t>(A.NameIndex) * 0x40],
                         &Names[static_cast<size_t>(B.NameIndex) * 0x40]) > 0)
            {
                FNnHelpNameSortRecord const Saved = A;
                A = B;
                B = Saved;
                bSorted = false;
            }
        }
    }

    // every node erased (the routine), then one node per record
    // appended in order (the routine / the routine); 0x0047603e..: both buffers freed.
    Goods->clear();
    for (int Out = 0; Out < Count; ++Out)
        Goods->push_back(Records[Out].Entry);
}

void NnHelpAppendBaseGoods(RenderDisplayList* Card, unsigned BaseId, unsigned char bCommodities,
                           unsigned char bEquipment, unsigned char bShips)
{
    // (cdecl). 0x0047608e..0x004760af: nothing at all without a section.
    if (bCommodities == 0 && bEquipment == 0 && bShips == 0)
        return;


    AppendStyle(*Card, 0x9ca4);

    // a market of the base's goods, priced with its variance (the
    // BaseData is used untested, as in baseline).
    BaseData* const Base = BaseDataList_get()->get_base_data(BaseId);
    FClientMarket Market; // ; the routine at 0x00476bd0
    ClientMarketFill(&Market, Base->PriceVariance, Base);

    // ships for sale (category 3) -- the package's hull good's ship.
    if (bShips != 0)
    {
        AppendSectionTitle(*Card, 0x669);
        std::list<FClientMarketGood const*> Goods;
        ClientMarketCollectCategory(&Market, &Goods, 3);
        int Listed = 0;
        for (std::list<FClientMarketGood const*>::iterator It = Goods.begin(); It != Goods.end();
             ++It)
        {
            // GoodList::find_by_id(good)->HullGoodId (+0x90) ->
            // find_by_id -> ShipArchId (+0x54) -> GetShip -> ids_name (+0x14); no null tests.
            GoodInfo const* const Package = GoodList::find_by_id((*It)->Info.GoodId);
            GoodInfo const* const Hull = GoodList::find_by_id(Package->HullGoodId);
            Archetype::Ship const* const Ship = Archetype::GetShip(Hull->ShipArchId);
            LoadIdsIntoRdl(static_cast<unsigned>(Ship->IdsName), *Card);
            AppendText(*Card, kItemTail);
            ++Listed;
        }
        if (Listed == 0)
            LoadIdsIntoRdl(0x67b, *Card);
    }


    if (bCommodities != 0)
    {
        // the base's stocked commodities (category 0), by name, that sell
        // here at or below their good_buy price (GoodInfo +0x5c).
        AppendSectionTitle(*Card, 0x668);
        std::list<FClientMarketGood const*> Commodities;
        ClientMarketCollectCategory(&Market, &Commodities, 0);
        NnHelpSortGoodsByName(&Commodities);
        int Listed = 0;
        for (std::list<FClientMarketGood const*>::iterator It = Commodities.begin();
             It != Commodities.end(); ++It)
        {
            FClientMarketGood const* const Entry = *It;
            if (Entry->Stock <= 0) // +0x1c, JLE
                continue;
            unsigned const Arch = Good2Arch(Entry->Info.GoodId);
            Archetype::Equipment const* const Equip = Archetype::GetEquipment(Arch);
            GoodInfo const* const Info = GoodList::find_by_archetype(Arch);
            float const Price = ClientMarketGetPrice(&Market, Info->Id); // +0x48
            // FCOMP [+0x5c], TEST AH,0x41, JP -- skipped when the price
            // is greater (C3 = C0 = 0) or unordered (both set); listed when less or equal.
            if (!(Price <= Info->GoodBuyPrice))
                continue;
            AppendPricedItem(*Card, static_cast<unsigned>(Equip->IdsName), Price);
            ++Listed;
        }
        if (Listed == 0)
            LoadIdsIntoRdl(0x67b, *Card);

        // every good of GoodList (a copy of its list, the routine /
        // ) with an archetype whose price here is at or above its good_sell price
        // (GoodInfo +0x64).
        AppendSectionTitle(*Card, 0x667);
        std::list<GoodInfo*> const* const AllGoods = GoodList_get()->get_list();
        std::list<GoodInfo*> Candidates(AllGoods->begin(), AllGoods->end());
        Listed = 0;
        for (std::list<GoodInfo*>::iterator It = Candidates.begin(); It != Candidates.end(); ++It)
        {
            GoodInfo const* const Good = *It;
            if (Good->ArchId == 0) // +0x50, JBE
                continue;
            Archetype::Equipment const* const Equip = Archetype::GetEquipment(Good->ArchId);
            GoodInfo const* const Info = GoodList::find_by_archetype(Good->ArchId);
            float const Price = ClientMarketGetPrice(&Market, Info->Id);
            // FCOMP [+0x64], TEST AH,0x1, JNZ -- skipped when the price
            // is less (C0) or unordered (C0 set too); listed when greater or equal.
            if (!(Price >= Info->GoodSellPrice))
                continue;
            AppendPricedItem(*Card, static_cast<unsigned>(Equip->IdsName), Price);
            ++Listed;
        }
        if (Listed == 0)
            LoadIdsIntoRdl(0x67b, *Card);
        // both lists destroyed.
    }

    // the base's stocked equipment (category 1), by name, without
    // ammunition (Equipment vt+0x18 get_hp_type 3).
    if (bEquipment != 0)
    {
        AppendSectionTitle(*Card, 0x66a);
        std::list<FClientMarketGood const*> Goods;
        ClientMarketCollectCategory(&Market, &Goods, 1);
        NnHelpSortGoodsByName(&Goods);
        int Listed = 0;
        for (std::list<FClientMarketGood const*>::iterator It = Goods.begin(); It != Goods.end();
             ++It)
        {
            FClientMarketGood const* const Entry = *It;
            if (Entry->Stock <= 0)
                continue;
            Archetype::Equipment const* const Equip =
                Archetype::GetEquipment(Good2Arch(Entry->Info.GoodId));
            if (static_cast<int>(Equip->get_hp_type()) == 3)
                continue;
            LoadIdsIntoRdl(static_cast<unsigned>(Equip->IdsName), *Card);
            AppendText(*Card, kItemTail);
            ++Listed;
        }
        if (Listed == 0)
            LoadIdsIntoRdl(0x67b, *Card);
    }
}

bool NnHelp::Create(Win3D* NewParent, void* Data)
{
    // (RET 8). 0x00474bab..0x00474c05: in space, the id of the ship's target is
    // kept for the text below (the player's gameobj -> +0xc CBase -> +4 CObject, a CShip).
    if (G_bNnHelpPendingTarget == 0 && !IsPlayerDockedbaseline())
    {
        FClientGameobj* const Player = static_cast<FClientGameobj*>(GetPlayerObjRW());
        if (Player != nullptr)
        {
            CBase* const PlayerBase = static_cast<IObjInspectImpl*>(Player);
            if (PlayerBase != nullptr)
            {
                CObject* const PlayerObject = PlayerBase->cobject();
                if (PlayerObject != nullptr &&
                    (static_cast<unsigned>(PlayerObject->ObjectClass) & 0x503u) == 0x503u)
                {
                    IObjRW* const Target = static_cast<CShip*>(PlayerObject)->get_target();
                    if (Target != nullptr)
                    {
                        G_NnHelpPendingTargetId = Target->get_id(); // vt+0x20
                        G_bNnHelpPendingTarget = 1;
                    }
                }
            }
        }
    }

    // the window mesh (slot 38) and the hit box from it.
    SetMesh(kInfoWindowMesh, 0);
    WinControlHalfExtentsFromMesh(HalfExtents, &MeshHolder);


    {
        float const Position[3] = {0.397f, 0.238f, 0.0f};
        float const Half[3] = {0.018f, 0.018f, 0.0f};
        CloseButton = CreateButtonControl("PlayerInfoCloseButton", kCloseButtonMesh, nullptr,
                                          "ui_select_item", nullptr, Position, nullptr, Half, 0, 0,
                                          0, 0, 0x409, 0, 0);
    }

    {
        float const Position[3] = {-0.418f, 0.256f, 0.0f};
        Title = CreateTextControl("PlayerInfoTitle", "PageHeader", Position, nullptr, 0.836f, 0.03f,
                                  1.65f, 0x389, 1, 0, 1);
    }

    {
        float const Position[3] = {-0.409f, 0.208f, 0.0f};
        Text = CreateRichTextWin("PlayerInfoText", 0.795f, 0.455f, Position, true);
    }

    if (G_bNnHelpPendingTarget != 0)
    {
        // the target, looked up again by id, fills the window.
        FClientGameobj* const Found = ClientObjDb_FindObject(G_NnHelpPendingTargetId, 0);
        if (Found != nullptr)
            ShowObjectInfo(Found);
        G_NnHelpPendingTargetId = 0;
        G_bNnHelpPendingTarget = 0;
    }
    else
    {
        // the strid_name of the player's system (ISystem slot 3).
        int SystemName = 0; // [ESP+0x24]
        Universe::ISystem const* const System = Universe::get_system(GetPlayerStarSystemIdbaseline());
        if (System != nullptr)
            SystemName = System->GetStridName();


        RenderDisplayList Card; // [ESP+0x18]
        AppendStyle(Card, 0x9ca4);

        if (IsPlayerDockedbaseline())
        {
            // the base's strid_name; nothing more without it or the
            // system's.
            int const BaseName = GetClientCurrentBaseStridName();
            if (BaseName != 0 && SystemName != 0)
            {
                // ids 0x55c formatted with the base name and the system
                // name, each loaded into one half of the first scratch buffer.
                wchar_t Format[0x80]; // [ESP+0x158]
                GetIdsStringWWithRawFallback(0x55c, Format, 0x80);
                int const Half = G_NnTextScratchCapacity / 2;
                wchar_t* const SecondHalf = &G_NnTextScratchA[Half];
                GetIdsStringWWithRawFallback(static_cast<unsigned>(BaseName), G_NnTextScratchA,
                                             Half);
                GetIdsStringWWithRawFallback(static_cast<unsigned>(SystemName), SecondHalf, Half);
                _swprintf(G_NnTextScratchB, Format, G_NnTextScratchA, SecondHalf);
                AppendScratchB(Card);

                // the system object that is the player's base.
                unsigned char VisitFlags = 0; // [ESP+0x12]
                unsigned const BaseId = GetPlayerBaseIdbaseline(); // [ESP+0x24], *the routine
                int IdsInfo = 0;                                 // ESI
                bool bShow = false;
                Universe::FSystemObject Object; // [ESP+0x40], shared by both passes
                unsigned const Count = (unsigned)System->GetObjectCount();

                // The visit bits of a matching object: its "visit" byte OR the visit-map flags of
                // its nickname (the routine(id, &flags, 0, 0)). baseline ORs the stack byte
                // [ESP+0x13] whatever the call did; when the nickname has no visit-map entry
                // nothing is written there and baseline ORs a stale stack byte -- the port ORs 0.
                // Returns whether the object carries an ids_info (the show path).
                auto TakeMatch = [&VisitFlags, &IdsInfo](Universe::FSystemObject const& Match)
                {
                    VisitFlags = Match.Visit; // +0x114
                    IdsInfo = Match.IdsInfo;  // +0x58
                    unsigned char MapFlags = 0;
                    GetPlayerVisitFlagsbaseline(CreateID(Match.Nickname.Data), &MapFlags, 0, 0);
                    VisitFlags = static_cast<unsigned char>(VisitFlags | MapFlags);
                    return IdsInfo != 0;
                };

                // first pass, an object whose "base" is BaseId (+0x88).
                // A match without an ids_info ends the pass and falls into the second one.
                for (unsigned Index = 0; Index < Count; ++Index)
                {
                    if (System->GetSystemObject(Index, &Object) && Object.BaseId == BaseId)
                    {
                        bShow = TakeMatch(Object);
                        break;
                    }
                }
                // second pass, an object that docks with BaseId (+0x80).
                if (!bShow)
                {
                    for (unsigned Index = 0; Index < Count; ++Index)
                    {
                        if (System->GetSystemObject(Index, &Object) && Object.DockWith == BaseId)
                        {
                            bShow = TakeMatch(Object);
                            break;
                        }
                    }
                }
                // a mission-created base's ids_info (the visit bits as
                // the passes left them); nothing without one.
                if (!bShow)
                {
                    LoadMissionCreatedSolars();
                    int const SolarCount = static_cast<int>(G_MissionCreatedSolars.size());
                    for (int Index = 0; Index < SolarCount; ++Index)
                    {
                        if (G_MissionCreatedSolars[Index].BaseId == BaseId)
                        {
                            IdsInfo = G_MissionCreatedSolars[Index].IdsInfo;
                            bShow = IdsInfo != 0;
                            break;
                        }
                    }
                }

                if (bShow)
                {
                    // two line breaks, the infocard, then the infocard
                    // map's entry of it.
                    AppendText(Card, kDoubleLineBreak);
                    LoadIdsIntoRdl(static_cast<unsigned>(IdsInfo), Card);
                    FInfocardMapEntry const* Entry = nullptr; // [ESP+0x14]
                    unsigned char bCommodities = 0;           // [ESP+0x28]
                    unsigned char bEquipment = 0;             // [ESP+0x2c]
                    unsigned char bShips = 0;                 // [ESP+0x38]
                    if (InfocardMapFind(static_cast<unsigned>(IdsInfo), &Entry))
                    {
                        // a section only when the visit bit and the
                        // entry's value for it are both set.
                        if ((VisitFlags & 4) != 0 && Entry->Field_0x08 != 0)
                            bCommodities = 1;
                        if ((VisitFlags & 8) != 0 && Entry->Field_0x04 != 0)
                            bEquipment = 1;
                        if ((VisitFlags & 0x10) != 0 && Entry->Field_0x0c != 0)
                            bShips = 1;
                        NnHelpAppendBaseGoods(&Card, BaseId, bCommodities, bEquipment, bShips);

                        // the entry's extra infocard after an empty
                        // paragraph.
                        if ((VisitFlags & 2) != 0 && Entry->ExtraIds != 0)
                        {
                            AppendText(Card, kEmptyParagraph);
                            LoadIdsIntoRdl(static_cast<unsigned>(Entry->ExtraIds), Card);
                        }
                    }
                }
            }
        }
        else
        {
            // in space -- the system's name, ids 0x561 and the sector,
            // then the zone the player is in (named, any visit state).
            IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW());
            if (Player != nullptr)
            {
                int const Capacity = G_NnTextScratchCapacity;
                GetIdsStringWWithRawFallback(static_cast<unsigned>(SystemName), G_NnTextScratchA,
                                             Capacity);
                wcscpy(G_NnTextScratchB, G_NnTextScratchA);
                GetIdsStringWWithRawFallback(0x561, G_NnTextScratchA, Capacity);
                wcscat(G_NnTextScratchB, G_NnTextScratchA);
                // baseline also pushes the capacity as a third argument;
                // reads only the first two.
                NavMapSectorName(&Player->get_position(), G_NnTextScratchA);
                wcscat(G_NnTextScratchB, G_NnTextScratchA);
                AppendScratchB(Card);

                // the zone (the routine(system, pos, 1, 0, 0)).
                Universe::IZone const* const Zone = NavMapZoneAt(
                    System, reinterpret_cast<float const*>(&Player->get_position()), 1, 0, 0);
                if (Zone != nullptr)
                {
                    // its name (vt+0x84) after two line breaks, then its
                    // ids_info (vt+0x88) after an empty paragraph.
                    AppendText(Card, kDoubleLineBreak);
                    LoadIdsIntoRdl(static_cast<unsigned>(Zone->GetName()), Card);
                    AppendText(Card, kEmptyParagraph);
                    LoadIdsIntoRdl(static_cast<unsigned>(Zone->GetIdsInfo()), Card);
                }
            }
        }

        // the text replaced by the card, the card destroyed.
        Text->ClearContent();
        Text->AppendContent(Card);
    }

    // slot 31 with {0, 0, 0}, slot 5 with the position, Win3D's Create.
    SetOffset(0.0f, 0.0f, 0.0f);
    SetPosition(Position);
    return Win3D::Create(NewParent, Data);
}

bool NnHelp::Slot23(unsigned Msg, unsigned A, unsigned B)
{
    // (RET 0xc).
    if (Msg != 0xef02)
        return WinControl::Slot23(Msg, A, B);

    // out of the parent (no parent test), then every child deleted (the
    // next sibling read first), slot 10 and slot 0 with 1.
    Win3DReparent(this, nullptr);
    Win3D* Child = ChildHead;
    while (Child != nullptr)
    {
        Win3D* const Next = Child->NextSibling;
        Win3DDeleteTree(Child);
        Child = Next;
    }
    Destroy();
    delete this;
    return true;
}

intptr_t NnHelp::SetProperty(int Msg, uintptr_t A, uintptr_t B)
{
    // (RET 0xc).
    (void)B;
    if (Msg == 0x26 && A == reinterpret_cast<uintptr_t>(CloseButton))
    {
        NnMainButtonOnModalOpen();
        return 1;
    }
    return 0;
}

void NnHelp::ShowObjectInfo(IObjRW* Object)
{
    // (RET 4). 0x004754c6..0x004754f0: a CSimple ((class & 3) == 3) or nothing.
    if (Object == nullptr)
        return;
    CObject const* const Target = Object->cobject(); // vt+0x150
    if (Target == nullptr)
        return;
    if ((static_cast<unsigned>(Target->ObjectClass) & 3u) != 3u)
        return;
    CSimple const* const Simple = static_cast<CSimple const*>(Target);


    unsigned const TypeBits = Simple->TypeBits; // [ESP+0x24], +0xe0
    unsigned InfoIds = 0;                       // [ESP+0x20]
    RenderDisplayList Card;                     // [ESP+0x28]
    AppendStyle(Card, 0x9ca4);


    unsigned char const Visit = HudObjectVisitFlags(Target); // [ESP+0x17]
    G_NnTextScratchB[0] = L'\0';
    wchar_t Name[0x80]; // [ESP+0x48]

    if ((Visit & 1) == 0)
    {
        // never visited -- the type caption as the title (upper case)
        // and ids 0xd6ef as the text.
        IdsIntoOrEmpty(HudObjectTypeIds(Simple), Name, 0x80);
        wcscat(G_NnTextScratchB, Name);
        CharUpperW(G_NnTextScratchB);
        Title->SetProperty(0x1d, reinterpret_cast<uintptr_t>(G_NnTextScratchB), 0);
        GetIdsStringWWithRawFallback(0xd6ef, G_NnTextScratchB, G_NnTextScratchCapacity);
        AppendScratchB(Card);
    }
    else
    {
        int const Capacity = G_NnTextScratchCapacity;

        // the name as the title (upper case).
        IdsIntoOrEmpty(GetObjectIdsName(Target), Name, 0x80);
        wcscat(G_NnTextScratchB, Name);
        CharUpperW(G_NnTextScratchB);
        Title->SetProperty(0x1d, reinterpret_cast<uintptr_t>(G_NnTextScratchB), 0);
        G_NnTextScratchB[0] = L'\0';

        // the pilot (ids 0x454).
        IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW()); // EBP
        FHudWideText Pilot = {nullptr, 0};                             // [ESP+0x38]
        GetShipPilotName(Target, &Pilot);
        if (Pilot.Text != nullptr && Pilot.Text[0] != L'\0')
        {
            GetIdsStringWWithRawFallback(0x454, G_NnTextScratchA, Capacity);
            wcscat(G_NnTextScratchB, G_NnTextScratchA);
            wcscat(G_NnTextScratchB, Pilot.Text);
            wcscat(G_NnTextScratchB, kLineBreak);
        }

        // the attitude towards the player (ids 0x455, then 0x466
        // hostile / 0x467 neutral / 0x468 friendly).
        if (Player != nullptr && (TypeBits & 0x7f0100u) != 0)
        {
            int const Attitude = GetAttitudeClass(Player, Object);
            GetIdsStringWWithRawFallback(0x455, G_NnTextScratchA, Capacity);
            wcscat(G_NnTextScratchB, G_NnTextScratchA);
            unsigned AttitudeIds = 0;
            if (Attitude == -1)
                AttitudeIds = 0x466;
            else if (Attitude == 0)
                AttitudeIds = 0x467;
            else if (Attitude == 1)
                AttitudeIds = 0x468;
            if (AttitudeIds != 0)
            {
                GetIdsStringWWithRawFallback(AttitudeIds, G_NnTextScratchA, Capacity);
                wcscat(G_NnTextScratchB, G_NnTextScratchA);
            }
            wcscat(G_NnTextScratchB, kLineBreak);
        }


        if ((TypeBits & 0x7f0000u) != 0)
        {
            // the hull (ids 0x457): ftol(hit / max * 100). The x87
            // divide and multiply are carried in double here.
            float const MaxHitPts = Simple->get_max_hit_pts(); // FSTP float [ESP+0x40]
            int const Hull = static_cast<int>(static_cast<double>(Simple->get_hit_pts()) /
                                              static_cast<double>(MaxHitPts) * 100.0);
            GetIdsStringWWithRawFallback(0x457, G_NnTextScratchA, Capacity);
            wcscat(G_NnTextScratchB, G_NnTextScratchA);
            FormatPercentLine(G_NnTextScratchA, Hull);
            wcscat(G_NnTextScratchB, G_NnTextScratchA);

            // a CEqObj's shield (ids 0x464) when its capacity is above 0.
            if ((static_cast<unsigned>(Target->ObjectClass) & 0x103u) == 0x103u)
            {
                float ShieldMax = 0.0f; // [ESP+0x10]
                float ShieldCur = 0.0f; // [ESP+0x18]
                HudSampleShieldHitPoints(
                    const_cast<CEqObj*>(static_cast<CEqObj const*>(Target)), &ShieldMax, &ShieldCur,
                    nullptr);
                // FCOMP 0.0, TEST AH,0x41, JNZ -- skipped when not greater (or unordered).
                if (ShieldMax > 0.0f)
                {
                    int const Shield = static_cast<int>(static_cast<double>(ShieldCur) /
                                                        static_cast<double>(ShieldMax) * 100.0);
                    GetIdsStringWWithRawFallback(0x464, G_NnTextScratchA, Capacity);
                    wcscat(G_NnTextScratchB, G_NnTextScratchA);
                    FormatPercentLine(G_NnTextScratchA, Shield);
                    wcscat(G_NnTextScratchB, G_NnTextScratchA);
                }
            }

            // a ship's group (ids 0x456 + CShip::get_group_name), and the
            // affiliation's infocard kept for the text.
            if ((static_cast<unsigned>(Target->ObjectClass) & 0x503u) == 0x503u)
            {
                CShip const* const Ship = static_cast<CShip const*>(Target);
                unsigned const Group = Ship->get_group_name();
                if (Group != 0)
                {
                    GetIdsStringWWithRawFallback(0x456, G_NnTextScratchA, Capacity);
                    wcscat(G_NnTextScratchB, G_NnTextScratchA);
                    wchar_t GroupName[0x80]; // [ESP+0x148]
                    IdsIntoOrEmpty(Group, GroupName, 0x80);
                    wcscat(G_NnTextScratchB, GroupName);
                    wcscat(G_NnTextScratchB, kLineBreak);

                    int const Vibe = static_cast<int>(Ship->VibeId); // [ESP+0x50], +0x104
                    unsigned Affiliation = 0;                        // [ESP+0x44]
                    Reputation::Vibe::GetAffiliation(Vibe, Affiliation, false);
                    unsigned const AffiliationCard = Reputation::get_info_card(Affiliation);
                    if (AffiliationCard != 0)
                    {
                        wcscat(G_NnTextScratchB, kLineBreak);
                        InfoIds = AffiliationCard;
                    }
                }
            }
        }

        // a waypoint -- the player's own is numbered (ids 0x465), a
        // route's shows its marker.
        if ((TypeBits & 0x200u) != 0)
        {
            unsigned char bUserWaypoint = 0; // [ESP+0x1f]
            int WaypointIndex = 0;           // [ESP+0x18]
            // ECX = the field, used untested as in baseline.
            if (GetHudWaypoint()->GetWaypointEntry(&bUserWaypoint, &WaypointIndex))
            {
                if (bUserWaypoint != 0)
                {
                    GetIdsStringWWithRawFallback(0x465, G_NnTextScratchA, Capacity);
                    wcscat(G_NnTextScratchB, G_NnTextScratchA);
                    FormatPercentLine(G_NnTextScratchA, WaypointIndex + 1);
                    wcscat(G_NnTextScratchB, G_NnTextScratchA);
                }
                else
                {
                    FBestPathWaypoint* const Waypoint = BestPathGetWaypoint(WaypointIndex);
                    if (Waypoint != nullptr)
                    {
                        FmtStrSprintf(&Waypoint->Marker, G_NnTextScratchA, Capacity);
                        wcscat(G_NnTextScratchB, G_NnTextScratchA);
                        wcscat(G_NnTextScratchB, kLineBreak);
                    }
                }
            }
        }

        // loot -- the text so far becomes the title, and the infocard
        // is the contents' good's (+0x8c), or the contents archetype's ids_info (+0x18).
        if ((TypeBits & 0x10000000u) != 0)
        {
            Title->SetProperty(0x1d, reinterpret_cast<uintptr_t>(G_NnTextScratchB), 0);
            CObject const* const Loot = Object->cobject();
            if (Loot != nullptr && (static_cast<unsigned>(Loot->ObjectClass) & 0x803u) == 0x803u)
            {
                Archetype::Equipment const* const Contents =
                    static_cast<CLoot const*>(Loot)->ContentsArch; // +0xec
                if (Contents != nullptr)
                {
                    GoodInfo const* const Good = GoodList::find_by_archetype(Contents->Nickname.Id);
                    if (Good != nullptr)
                        InfoIds = static_cast<unsigned>(Good->IdsInfo);
                    else
                        InfoIds = static_cast<unsigned>(Contents->IdsInfo);
                }
            }
        }

        // the collected lines, then the kept infocard.
        if (G_NnTextScratchB[0] != L'\0')
            AppendScratchB(Card);
        if (InfoIds != 0)
            LoadIdsIntoRdl(InfoIds, Card);

        // a CEqObj's own infocard (Reputation::Vibe::GetInfocard), and
        // with an infocard map entry and a dock base the base goods sections by the visit bits.
        CObject const* const Owner = Object->cobject();
        if (Owner != nullptr && (static_cast<unsigned>(Owner->ObjectClass) & 0x103u) == 0x103u)
        {
            CEqObj const* const EqObj = static_cast<CEqObj const*>(Owner);
            int const Vibe = static_cast<int>(EqObj->VibeId); // [ESP+0x34]
            unsigned Infocard = 0;                            // [ESP+0x10]
            Reputation::Vibe::GetInfocard(Vibe, Infocard);
            if (Infocard != 0)
            {
                LoadIdsIntoRdl(Infocard, Card);
                AppendText(Card, kLineBreak);
                unsigned const DockBase = EqObj->DockingBaseId; // ESI, +0x160
                FInfocardMapEntry const* Entry = nullptr;      // [ESP+0x34]
                if (InfocardMapFind(Infocard, &Entry) && DockBase != 0)
                {
                    // the visit bits alone pick the sections here.
                    unsigned char const bCommodities = (Visit & 4) != 0 ? 1 : 0; // [ESP+0x20]
                    unsigned char const bEquipment = (Visit & 8) != 0 ? 1 : 0;   // [ESP+0x24]
                    unsigned char const bShips = (Visit & 0x10) != 0 ? 1 : 0;    // [ESP+0x18]
                    NnHelpAppendBaseGoods(&Card, DockBase, bCommodities, bEquipment, bShips);
                    AppendText(Card, kLineBreak);
                    if ((Visit & 2) != 0 && Entry->ExtraIds != 0)
                    {
                        LoadIdsIntoRdl(static_cast<unsigned>(Entry->ExtraIds), Card);
                        AppendText(Card, kLineBreak);
                    }
                }
            }
        }

        // the pilot name's buffer (CRT free).
        if (Pilot.Text != nullptr)
            free(Pilot.Text);
    }

    // the text replaced by the card and scrolled to the top.
    Text->ClearContent();
    Text->AppendContent(Card);
    Text->ScrollToTop();
}
