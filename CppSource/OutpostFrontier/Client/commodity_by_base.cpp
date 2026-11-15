// Author: Giperion.

#include "commodity_by_base.h"

#include "character_select_equip.h" // GetPlayerVisitFlagsbaseline (the routine)
#include "globals.h"                // IsClientServerTickPaused (the routine)
#include "hud_command_dispatch.h"   // HudPostCommand (the routine)
#include "hud_target.h"             // HudIsMousePickBlocked (the routine)
#include "ids_rdl.h"                // AppendTextMaybeXmlRdl (the routine)
#include "ids_strings.h"            // GetIdsStringWWithRawFallback (the routine)
#include "input_manager.h"          // GetClientSoftwareCursor, GetClientMouseCursorDrawFlag
#include "misc_util.h"              // FormatGroupedNumber (the routine)
#include "nav_bar.h"                // NavBarHasServiceWindow (the routine), NavBarIsAnimating (the routine)
#include "nn_inventory.h"           // IsPlayerDockedbaseline (the routine)
#include "nn_modal.h"               // NnFloatBits, NnMainButtonHasActivePanel, G_bNnScreenUp
#include "nn_navmap.h"              // NavMapRequestFocus (the routine)
#include "nn_text_scratch.h"        // / the field / the field
#include "nn_ui_pulse.h"            // NnUiPulseBlendArgb (the routine), palette colours
#include "player_behavior.h"        // GetPlayerObjRW (the routine)
#include "space_camera.h"           // FSpaceCamera
#include "space_letterbox.h"        // IsLetterboxVisible (the routine)
#include "ui_scroll.h"
#include "win3d.h"                  // Win3DFindByName, Win3DDeleteTree, Win3DGetCamera
#include "win_factory.h"

#include "common_archetype.h"
#include "common_csys.h"            // Acquire3DMathEngine (the routine)
#include "common_goods.h"
#include "common_universe.h"
#include "x86math.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

std::vector<FCommodityByBaseRow> G_CommodityByBaseRows;
std::map<unsigned, std::wstring> G_CommodityByBaseBaseNames;
std::map<unsigned, std::wstring> G_CommodityByBaseSystemNames;

// WINFACTORY<NN_CommodityByBase> static (the branch -> the routine;
// Create the routine; atexit the branch -> the routine).
WINFACTORY<NnCommodityByBase> G_NnCommodityByBaseFactory("NN_CommodityByBase");

namespace
{
    // this TU's 3DMathEngine (the routine on first use, the branch).
    I3DMathEngine* G_CommodityByBaseMath = nullptr;

    char const kCommodityByBaseMesh[] = "Interface\\NeuroNet\\Inventory\\CHAR_infowindow.cmp";
    char const kCommodityByBaseCloseMesh[] = "Interface\\NeuroNet\\Inventory\\x.3db";

    // true when the window may not stay open: docked with a base-service window
    // or a moving NavBar, in space without a player ship, a "NeuroNetMissionMessage",
    // "NN_Trade" or "NN_Scanner" window, an NN_MainButton panel, a NeuroNet screen, a held
    // server tick, the letterbox, or the software cursor hidden.
    bool CommodityByBaseMustClose()
    {
        if (IsPlayerDockedbaseline())
        {
            if (NavBarHasServiceWindow())
                return true;
            if (NavBarIsAnimating())
                return true;
        }
        else if (GetPlayerObjRW() == nullptr)
        {
            return true;
        }
        if (Win3DFindByName("NeuroNetMissionMessage") != nullptr)
            return true;
        if (Win3DFindByName("NN_Trade") != nullptr)
            return true;
        if (Win3DFindByName("NN_Scanner") != nullptr)
            return true;
        if (NnMainButtonHasActivePanel())
            return true;
        if (G_bNnScreenUp)
            return true;
        if (IsClientServerTickPaused())
            return true;
        if (IsLetterboxVisible())
            return true;
        if (GetClientMouseCursorDrawFlag() == 0)
            return true;
        return false;
    }

    // L"???" -- what every name lookup below falls back to on a miss.
    wchar_t const kCommodityUnknownName[] = L"???";

    // The inlined `find(key)` + `== end ? L"???" : node+0x10` of the routine / the routine.
    wchar_t const* CommodityByBaseName(std::map<unsigned, std::wstring> const& Names,
                                       unsigned SpaceObjectId)
    {
        std::map<unsigned, std::wstring>::const_iterator Found = Names.find(SpaceObjectId);
        if (Found == Names.end())
            return kCommodityUnknownName;
        return Found->second.c_str();
    }

    void SwapCommodityByBaseRows(FCommodityByBaseRow& A, FCommodityByBaseRow& B)
    {
        FCommodityByBaseRow const Temp = A;
        A = B;
        B = Temp;
    }
}

bool IsCommodityByBaseListedName(int Ids)
{
    // 0x004a2314..0x004a235c, one compare each, in baseline's order.
    if (Ids == 0x3fc27 || Ids == 0x3fc2e || Ids == 0x3fc28 || Ids == 0xeb41 || Ids == 0 ||
        Ids == 0xeb3f || Ids == 0x300ea || Ids == 0x30052 || Ids == 0x3009c ||
        Ids == 0x3009d || Ids == 0x300bf)
        return false;
    return true;
}

void BuildCommodityByBaseRows(unsigned ArchId)
{
    // . 0046f6f7..0046f72e: the rows (the routine erase(first, last)), the base
    // names (the routine clear) and the system names (the routine erase(begin, end)).
    G_CommodityByBaseRows.clear();
    G_CommodityByBaseBaseNames.clear();
    G_CommodityByBaseSystemNames.clear();

    // every base of the universe.
    for (Universe::IBase* Base = Universe::GetFirstBase(); Base != nullptr;
         Base = Universe::GetNextBase())
    {
        // the base's space object (vt+0x5c) and its system (vt+0x10 ->
        // Universe::get_system); either missing skips the base.
        unsigned const SpaceObjectId = Base->SpaceObjectId;
        Universe::ISystem const* System = Universe::get_system(Base->SystemId);
        if (System == nullptr || SpaceObjectId == 0)
            continue;

        // the base's name ids (vt+8) must not be one of the unlisted ids.
        if (!IsCommodityByBaseListedName(Base->StridName))
            continue;

        // the player's visit flags for the space object (the routine,
        // nothing written on a miss), OR the object's own visit byte (vt+0x60); bit 4 (known)
        // is required.
        unsigned char VisitFlags = 0;
        GetPlayerVisitFlagsbaseline(SpaceObjectId, &VisitFlags, 0, 0);
        VisitFlags = static_cast<unsigned char>(VisitFlags | Base->SpaceObjectVisit);
        if ((VisitFlags & 4) == 0)
            continue;

        // the system's name (ids = ISystem slot 3, the routine into
        // with the field) under the space object, when not there yet.
        if (G_CommodityByBaseSystemNames.find(SpaceObjectId) == G_CommodityByBaseSystemNames.end())
        {
            GetIdsStringWWithRawFallback((unsigned)System->GetStridName(),
                                         G_NnTextScratchB, G_NnTextScratchCapacity);
            G_CommodityByBaseSystemNames.insert(
                std::pair<unsigned const, std::wstring>(SpaceObjectId, G_NnTextScratchB));
        }

        // the base's name (ids = vt+8) the same way.
        if (G_CommodityByBaseBaseNames.find(SpaceObjectId) == G_CommodityByBaseBaseNames.end())
        {
            GetIdsStringWWithRawFallback(static_cast<unsigned>(Base->StridName),
                                         G_NnTextScratchB, G_NnTextScratchCapacity);
            G_CommodityByBaseBaseNames.insert(
                std::pair<unsigned const, std::wstring>(SpaceObjectId, G_NnTextScratchB));
        }

        // the base's market (BaseData +0x10) by the base id (vt[0]); no
        // base data skips the base.
        BaseData* Data = BaseDataList_get()->get_base_data(Base->NicknameId);
        if (Data == nullptr)
            continue;
        std::map<unsigned, MarketGoodInfo> const* Market = &Data->Market;

        // the good's market entry gives the price (+0x04, ftol) and
        // "sells" = its +0x0c is non-zero. Without an entry the good list's price (+0x58,
        // ftol) is used and "sells" is false -- unless the archetype has no good at all, which
        // leaves price 0 and "sells" true.
        unsigned const GoodId = Arch2Good(ArchId);
        int Price = 0;
        bool bBaseSells = true;
        std::map<unsigned, MarketGoodInfo>::const_iterator Entry = Market->find(GoodId);
        if (Entry != Market->end())
        {
            Price = static_cast<int>(Entry->second.Price);
            if (Entry->second.MaxInventory == 0)
                bBaseSells = false;
        }
        else
        {
            GoodInfo const* Good = GoodList::find_by_archetype(ArchId);
            if (Good != nullptr)
            {
                Price = static_cast<int>(Good->Price);
                bBaseSells = false;
            }
        }

        // push_back {space object, price, sells} (the routine at end).
        FCommodityByBaseRow Row;
        Row.SpaceObjectId = SpaceObjectId;
        Row.Price = Price;
        Row.bBaseSells = bBaseSells;
        G_CommodityByBaseRows.push_back(Row);
    }
}

void SortCommodityByBaseRows(int Column)
{
    // . 0047004b..0047006b: the row count; every mode runs passes over the first
    // Count - 1 neighbours until a pass swaps nothing, and does nothing below two rows.
    int const Count = static_cast<int>(G_CommodityByBaseRows.size());
    int const Last = Count - 1;
    if (Column != 1 && Column != 2 && Column != 3 && Column != 4)
        return;
    if (Last <= 0)
        return;

    bool bSorted = false;
    while (!bSorted)
    {
        bSorted = true;
        for (int Index = 0; Index < Last; ++Index)
        {
            FCommodityByBaseRow& A = G_CommodityByBaseRows[Index];
            FCommodityByBaseRow& B = G_CommodityByBaseRows[Index + 1];
            bool bSwap = false;

            if (Column == 2)
            {
                // base name, ascending (_wcsicmp(A, B) > 0 swaps).
                wchar_t const* NameB = CommodityByBaseName(G_CommodityByBaseBaseNames, B.SpaceObjectId);
                wchar_t const* NameA = CommodityByBaseName(G_CommodityByBaseBaseNames, A.SpaceObjectId);
                if (_wcsicmp(NameA, NameB) > 0)
                    bSwap = true;
            }
            else if (Column == 1)
            {
                // price, highest first; equal prices by base name.
                if (A.Price > B.Price)
                {
                    bSwap = false;
                }
                else if (A.Price < B.Price)
                {
                    bSwap = true;
                }
                else
                {
                    wchar_t const* NameB = CommodityByBaseName(G_CommodityByBaseBaseNames, B.SpaceObjectId);
                    wchar_t const* NameA = CommodityByBaseName(G_CommodityByBaseBaseNames, A.SpaceObjectId);
                    if (_wcsicmp(NameA, NameB) > 0)
                        bSwap = true;
                }
            }
            else if (Column == 3)
            {
                // system name ascending; equal systems by price (highest
                // first), then by base name.
                wchar_t const* SystemB = CommodityByBaseName(G_CommodityByBaseSystemNames, B.SpaceObjectId);
                wchar_t const* SystemA = CommodityByBaseName(G_CommodityByBaseSystemNames, A.SpaceObjectId);
                int const SystemOrder = _wcsicmp(SystemA, SystemB);
                if (SystemOrder != 0)
                {
                    if (SystemOrder > 0)
                        bSwap = true;
                }
                else if (A.Price > B.Price)
                {
                    bSwap = false;
                }
                else if (A.Price < B.Price)
                {
                    bSwap = true;
                }
                else
                {
                    wchar_t const* NameB = CommodityByBaseName(G_CommodityByBaseBaseNames, B.SpaceObjectId);
                    wchar_t const* NameA = CommodityByBaseName(G_CommodityByBaseBaseNames, A.SpaceObjectId);
                    if (_wcsicmp(NameA, NameB) > 0)
                        bSwap = true;
                }
            }
            else
            {
                // selling bases first; within the same flag, price highest
                // first (A.Price >= B.Price keeps the order).
                if (A.bBaseSells && !B.bBaseSells)
                    bSwap = false;
                else if (!A.bBaseSells && B.bBaseSells)
                    bSwap = true;
                else if (A.Price < B.Price)
                    bSwap = true;
            }

            if (bSwap)
            {
                SwapCommodityByBaseRows(A, B);
                bSorted = false;
            }
        }
    }
}

void AppendCommodityTradeReport(RenderDisplayList* Card, unsigned ArchId)
{
    // . 004704d9..004704e1: rebuild the rows, sort by price.
    BuildCommodityByBaseRows(ArchId);
    SortCommodityByBaseRows(1);

    // one line per row, built in the field with the field as the
    // scratch for the number and the ids strings.
    int const Count = static_cast<int>(G_CommodityByBaseRows.size());
    for (int Index = 0; Index < Count; ++Index)
    {
        FCommodityByBaseRow const& Row = G_CommodityByBaseRows[Index];

        // "\n" (the field), the price with the ids 0x2140 prefix
        // (the routine(.., -1)), " " (the field).
        wcscpy(G_NnTextScratchB, L"\n");
        FormatGroupedNumber(G_NnTextScratchA, Row.Price, -1);
        wcscat(G_NnTextScratchB, G_NnTextScratchA);
        wcscat(G_NnTextScratchB, L" ");

        // the base name or "???", then ", " (the field).
        wcscat(G_NnTextScratchB,
               CommodityByBaseName(G_CommodityByBaseBaseNames, Row.SpaceObjectId));
        wcscat(G_NnTextScratchB, L", ");

        // the system name or "???" (the routine lower_bound + the key
        // compare = find).
        wcscat(G_NnTextScratchB,
               CommodityByBaseName(G_CommodityByBaseSystemNames, Row.SpaceObjectId));

        // ids 0x3e5 when the base sells the good.
        if (Row.bBaseSells)
        {
            GetIdsStringWWithRawFallback(0x3e5, G_NnTextScratchA, G_NnTextScratchCapacity);
            wcscat(G_NnTextScratchB, G_NnTextScratchA);
        }

        // ids 0x3dd (" (Best Known Price)") on the first row.
        if (Index == 0)
        {
            GetIdsStringWWithRawFallback(0x3dd, G_NnTextScratchA, G_NnTextScratchCapacity);
            wcscat(G_NnTextScratchB, G_NnTextScratchA);
        }

        // the routine(line, wcslen(line), Card, 0).
        AppendTextMaybeXmlRdl(G_NnTextScratchB, static_cast<unsigned>(wcslen(G_NnTextScratchB)),
                              *Card, false);
    }
}

NnCommodityByBase::NnCommodityByBase()
{
    // 004707c9..004707e7: the routine and the three vptrs. Nothing else is
    // written before Create.
}

NnCommodityByBase::~NnCommodityByBase()
{
    // the routine on the +0x32c face, then the routine -- the EventHandler and
    // WinControl destructors, which C++ runs in that order.
}

void NnCommodityByBase::Close()
{
    // e.g. 0046f595..0046f5bf: the routine on every child (next sibling read first), slot 10,
    // then slot 0 with 1.
    Win3D* Child = ChildHead;
    while (Child != nullptr)
    {
        Win3D* const Next = Child->NextSibling;
        Win3DDeleteTree(Child);
        Child = Next;
    }
    Destroy();
    delete this;
}

bool NnCommodityByBase::Create(Win3D* /*NewParent*/, void* Data)
{
    // (RET 8). 0046ef9d..0046efa9: the archetype, first row 0, no sort column.
    ArchId = static_cast<unsigned>(reinterpret_cast<uintptr_t>(Data));
    FirstRow = 0;
    SortColumn = 0;

    // slot 38 SetMesh, then the half extents from the mesh (the routine).
    SetMesh(kCommodityByBaseMesh, 0);
    WinControlHalfExtentsFromMesh(HalfExtents, &MeshHolder);

    // the title.
    float const TitlePosition[3] = {NnFloatBits(0xbed58106), NnFloatBits(0x3e841893), 0.0f};
    WinControl* const Title = CreateTextControl(
        "CommodityByBaseTitle", "PageHeader", TitlePosition, nullptr, NnFloatBits(0x3f5645a2),
        NnFloatBits(0x3d75c28f), NnFloatBits(0x3fc00000), 0, 1, 0, 1);

    // ids 0x3d5, then " - " (the field) and the equipment's name when
    // the archetype is equipment.
    GetIdsStringWWithRawFallback(0x3d5, G_NnTextScratchB, G_NnTextScratchCapacity);
    if (ArchId != 0)
    {
        Archetype::Equipment* const Equip = Archetype::GetEquipment(ArchId);
        if (Equip != nullptr)
        {
            wcscat(G_NnTextScratchB, L" - ");
            GetIdsStringWWithRawFallback(static_cast<unsigned>(Equip->IdsName), G_NnTextScratchA,
                                         G_NnTextScratchCapacity);
            wcscat(G_NnTextScratchB, G_NnTextScratchA);
        }
    }

    // upper case (the routine, the delay-loaded CharUpperW), then the
    // title's text (0x1d); the title is not null-tested.
    CharUpperW(G_NnTextScratchB);
    Title->SetProperty(0x1d, reinterpret_cast<uintptr_t>(G_NnTextScratchB), 0);

    // the close button -> +0x334.
    {
        float const Position[3] = {NnFloatBits(0x3ecd4fdf), NnFloatBits(0x3e72b021), 0.0f};
        float const Extents[3] = {NnFloatBits(0x3c9374bc), NnFloatBits(0x3c9374bc), 0.0f};
        CloseButton = CreateButtonControl("CommodityByBaseCloseButton", kCommodityByBaseCloseMesh,
                                          nullptr, "ui_select_item", nullptr, Position, nullptr,
                                          Extents, 0, 0, 0, 0, 0x409, 0, 0);
    }

    // the four column titles, each then SetProperty(0x24, 1, 0) without a
    // null test.
    {
        float const Position[3] = {NnFloatBits(0xbec7ae14), NnFloatBits(0x3e428f5c), 0.0f};
        PriceTitle = CreateTextControl("CommodityByBasePriceTitle", "SubHeader", Position, nullptr,
                                       NnFloatBits(0x3dcccccd), NnFloatBits(0x3cf5c28f),
                                       NnFloatBits(0x3ff33333), 0x3d6, 0, 0, 1);
        PriceTitle->SetProperty(0x24, 1, 0);
    }
    {
        float const Position[3] = {NnFloatBits(0xbdcac083), NnFloatBits(0x3e428f5c), 0.0f};
        BaseTitle = CreateTextControl("CommodityByBaseBaseTitle", "SubHeader", Position, nullptr,
                                      NnFloatBits(0x3e851eb8), NnFloatBits(0x3cf5c28f),
                                      NnFloatBits(0x3ff33333), 0x3cc, 0, 0, 1);
        BaseTitle->SetProperty(0x24, 1, 0);
    }
    {
        float const Position[3] = {NnFloatBits(0x3e343958), NnFloatBits(0x3e428f5c), 0.0f};
        SystemTitle = CreateTextControl("CommodityByBaseSystemTitle", "SubHeader", Position,
                                        nullptr, NnFloatBits(0x3e4ccccd), NnFloatBits(0x3cf5c28f),
                                        NnFloatBits(0x3ff33333), 0x3cd, 0, 0, 1);
        SystemTitle->SetProperty(0x24, 1, 0);
    }
    {
        float const Position[3] = {NnFloatBits(0xbe8b4396), NnFloatBits(0x3e428f5c), 0.0f};
        BuySellTitle = CreateTextControl("CommodityByBaseBuySellTitle", "SubHeader", Position,
                                         nullptr, NnFloatBits(0x3dcccccd), NnFloatBits(0x3cf5c28f),
                                         NnFloatBits(0x3ff33333), 0x3d8, 0, 0, 1);
        BuySellTitle->SetProperty(0x24, 1, 0);
    }

    // 13 lines. Y = the field - i * (FILD / FMUL / FSUBR,
    // one rounding at the FST); the product and the difference are exact in double, so the
    // port's one rounding gives the same float. Each control gets SetProperty(0x24, 1, 0).
    float const LineTop = NnFloatBits(0x3e1cac08);   // 0.153
    float const LineStep = NnFloatBits(0x3cf5c28f);  // 0.03
    for (int Line = 0; Line < 13; ++Line)
    {
        float const Y = static_cast<float>(static_cast<double>(LineTop) -
                                           static_cast<double>(Line) *
                                               static_cast<double>(LineStep));
        char Name[0xf0]; // [ESP+0x44]
        FCommodityByBaseRowControls& Controls = Rows[Line];

        sprintf(Name, "CommodityByBasePrice%d", Line);
        float const PricePosition[3] = {NnFloatBits(0xbec7ae14), Y, 0.0f};
        Controls.Price = CreateTextControl(Name, "ItemName", PricePosition, nullptr,
                                           NnFloatBits(0x3dcccccd), NnFloatBits(0x3cf5c28f),
                                           NnFloatBits(0x3ff33333), 0, 0, 0, 1);
        Controls.Price->SetProperty(0x24, 1, 0);

        sprintf(Name, "CommodityByBuySellPrice%d", Line);
        float const BuySellPosition[3] = {NnFloatBits(0xbe8b4396), Y, 0.0f};
        Controls.BuySell = CreateTextControl(Name, "ItemName", BuySellPosition, nullptr,
                                             NnFloatBits(0x3dcccccd), NnFloatBits(0x3cf5c28f),
                                             NnFloatBits(0x3ff33333), 0, 0, 0, 1);
        Controls.BuySell->SetProperty(0x24, 1, 0);

        sprintf(Name, "CommodityByBaseBase%d", Line);
        float const BasePosition[3] = {NnFloatBits(0xbdcac083), Y, 0.0f};
        Controls.Base = CreateTextControl(Name, "ItemName", BasePosition, nullptr,
                                          NnFloatBits(0x3e851eb8), NnFloatBits(0x3cf5c28f),
                                          NnFloatBits(0x3ff33333), 0, 0, 0, 1);
        Controls.Base->SetProperty(0x24, 1, 0);

        sprintf(Name, "CommodityByBaseSystem%d", Line);
        float const SystemPosition[3] = {NnFloatBits(0x3e343958), Y, 0.0f};
        Controls.System = CreateTextControl(Name, "ItemName", SystemPosition, nullptr,
                                            NnFloatBits(0x3e4ccccd), NnFloatBits(0x3cf5c28f),
                                            NnFloatBits(0x3ff33333), 0, 0, 0, 1);
        Controls.System->SetProperty(0x24, 1, 0);
    }

    // the scroll bar (the routine: range 0..5, page 3, vertical).
    {
        float const Position[3] = {NnFloatBits(0x3ec83127), NnFloatBits(0x3e6d9168),
                                   NnFloatBits(0xbfa4dd2f)};
        Scroll = CreateUIScroll("CommodityByBaseScroll", Position, NnFloatBits(0x3c9ba5e3),
                                NnFloatBits(0x3ecfdf3b), 0, 5, 3, 0);
    }

    // the rows for the archetype, sorted by price, then shown.
    BuildCommodityByBaseRows(ArchId);
    if (SortColumn != 1)
    {
        SortColumn = 1;
        SortCommodityByBaseRows(1);
    }
    RefreshRows();

    // event 0xf, slot 31 SetOffset(0, 0x3d9ba5e3, 0xbfa4dd2f), slot 5
    // SetPosition(+0x60).
    RegisterEvent(0xf);
    SetOffset(0.0f, NnFloatBits(0x3d9ba5e3), NnFloatBits(0xbfa4dd2f));
    SetPosition(Position);

    // the routine(+0x04, 0) -- the window's current parent and no data,
    // not Create's arguments.
    return Win3D::Create(Parent, nullptr);
}

void NnCommodityByBase::RefreshRows()
{
    // . 0046fa83..0046fab4: the row count.
    int const Count = static_cast<int>(G_CommodityByBaseRows.size());

    // line i shows row FirstRow + i (signed compare with the count).
    int Row = FirstRow;
    for (int Line = 0; Line < 13; ++Line, ++Row)
    {
        FCommodityByBaseRowControls& Controls = Rows[Line];
        if (Row >= Count)
        {
            // past the end -- all four hidden.
            Controls.Base->Flags = static_cast<unsigned char>(Controls.Base->Flags & 0xfc);
            Controls.Price->Flags = static_cast<unsigned char>(Controls.Price->Flags & 0xfc);
            Controls.System->Flags = static_cast<unsigned char>(Controls.System->Flags & 0xfc);
            Controls.BuySell->Flags = static_cast<unsigned char>(Controls.BuySell->Flags & 0xfc);
            continue;
        }

        // all four shown.
        Controls.Base->Flags = static_cast<unsigned char>(Controls.Base->Flags | 3);
        Controls.Price->Flags = static_cast<unsigned char>(Controls.Price->Flags | 3);
        Controls.System->Flags = static_cast<unsigned char>(Controls.System->Flags | 3);
        Controls.BuySell->Flags = static_cast<unsigned char>(Controls.BuySell->Flags | 3);

        FCommodityByBaseRow const& Data = G_CommodityByBaseRows[Row];

        // the price (the routine(the field, price, -1), text 0x1d).
        FormatGroupedNumber(G_NnTextScratchB, Data.Price, -1);
        Controls.Price->SetProperty(0x1d, reinterpret_cast<uintptr_t>(G_NnTextScratchB), 0);

        // ids 0x3d9 when the base sells the good, 0x3da otherwise (0x1e).
        unsigned const SellsIds = Data.bBaseSells ? 0x3d9u : 0x3dau;
        Controls.BuySell->SetProperty(0x1e, SellsIds, 0);

        // the base name or "???".
        Controls.Base->SetProperty(
            0x1d,
            reinterpret_cast<uintptr_t>(
                CommodityByBaseName(G_CommodityByBaseBaseNames, Data.SpaceObjectId)),
            0);

        // the system name or "???" (the routine lower_bound + the key
        // compare = find).
        Controls.System->SetProperty(
            0x1d,
            reinterpret_cast<uintptr_t>(
                CommodityByBaseName(G_CommodityByBaseSystemNames, Data.SpaceObjectId)),
            0);
    }

    // the scroll bar -- hidden (property 2 = 1, flag bit 1 off) while the
    // rows fit; otherwise shown (property 2 = 0, flag bit 1 on) with the range 0x37
    // (0, Count - 13 + 2).
    int const Extra = Count - 13;
    if (Extra <= 0)
    {
        Scroll->SetProperty(2, 1, 0);
        Scroll->Flags = static_cast<unsigned char>(Scroll->Flags & 0xfd);
        return;
    }
    Scroll->SetProperty(2, 0, 0);
    Scroll->Flags = static_cast<unsigned char>(Scroll->Flags | 2);
    Scroll->SetProperty(0x37, 0, static_cast<uintptr_t>(Extra + 2));
}

intptr_t NnCommodityByBase::SetProperty(int Msg, uintptr_t A, uintptr_t B)
{
    // (RET 0xc). 0046f554..0046f58a: 0x3e from the scroll bar moves the first row;
    // 0x3e from anything else and 0x26 are clicks; every other message returns 0.
    if (Msg != 0x26)
    {
        if (Msg != 0x3e)
            return 0;
        if (A == reinterpret_cast<uintptr_t>(Scroll))
        {
            FirstRow = static_cast<int>(B);
            RefreshRows();
            return 1;
        }
    }

    // the close button -- close, then command 0x66 (the routine).
    if (A == reinterpret_cast<uintptr_t>(CloseButton))
    {
        Close();
        HudPostCommand(0x66, 0);
        return 1;
    }

    // a column title sorts by its column (unless it already does) and
    // redraws; these return 0.
    int Column = 0;
    if (A == reinterpret_cast<uintptr_t>(PriceTitle))
        Column = 1;
    else if (A == reinterpret_cast<uintptr_t>(BaseTitle))
        Column = 2;
    else if (A == reinterpret_cast<uintptr_t>(SystemTitle))
        Column = 3;
    else if (A == reinterpret_cast<uintptr_t>(BuySellTitle))
        Column = 4;
    if (Column != 0)
    {
        if (SortColumn != Column)
        {
            SortColumn = Column;
            SortCommodityByBaseRows(Column);
        }
        RefreshRows();
        return 0;
    }

    // any control of line i -- close, then the routine(the space object of
    // row FirstRow + i, 0).
    for (int Line = 0; Line < 13; ++Line)
    {
        FCommodityByBaseRowControls const& Controls = Rows[Line];
        if (A == reinterpret_cast<uintptr_t>(Controls.Base) ||
            A == reinterpret_cast<uintptr_t>(Controls.Price) ||
            A == reinterpret_cast<uintptr_t>(Controls.System) ||
            A == reinterpret_cast<uintptr_t>(Controls.BuySell))
        {
            // Id = rows[FirstRow + i].SpaceObjectId, read before the close
            // (no bounds test, as in baseline).
            unsigned const Id = G_CommodityByBaseRows[FirstRow + Line].SpaceObjectId;
            Close();
            // the routine(Id, 0) -- the system or system object named Id
            // becomes the nav map's pending focus.
            NavMapRequestFocus(Id, 0);
            return 1;
        }
    }
    return 0;
}

void NnCommodityByBase::Update()
{
    // . 0046fd27..0046fd68: close when the window may not stand (the `this` test
    // at 0x0046fd30 cannot fail).
    if (CommodityByBaseMustClose())
    {
        Close();
        return;
    }

    // the children (the routine).
    Win3D::Update();

    // the cursor ray -- the cursor pixel onto the near plane of the Win3D
    // camera the field, through a copy of its orientation the field (this TU's lazy
    // 3DMathEngine the field, slot +0x2c), scaled by 1.0f / length.
    FSpaceCamera* const Cam = Win3DGetCamera();
    int CursorX = 0;
    int CursorY = 0;
    GetClientSoftwareCursor(&CursorX, &CursorY, nullptr); // / the field
    FVector Local;
    Local.X = static_cast<float>((static_cast<double>(CursorX) - Cam->ScreenCenterX) /
                                 Cam->XScale * Cam->ZNear);
    Local.Y = static_cast<float>((static_cast<double>(CursorY) - Cam->ScreenCenterY) /
                                 Cam->YScale * Cam->ZNear);
    Local.Z = -Cam->ZNear;
    FMatrix Orientation;
    memcpy(&Orientation, Cam->World, sizeof(Orientation));
    if (G_CommodityByBaseMath == nullptr)
        G_CommodityByBaseMath = Acquire3DMathEngine();
    FVector World;
    G_CommodityByBaseMath->TransformMatrix(World, Orientation, Local);
    double const Inv = 1.0 / sqrt(static_cast<double>(World.X) * World.X +
                                  static_cast<double>(World.Y) * World.Y +
                                  static_cast<double>(World.Z) * World.Z);
    float Ray[3];
    Ray[0] = static_cast<float>(World.X * Inv);
    Ray[1] = static_cast<float>(World.Y * Inv);
    Ray[2] = static_cast<float>(World.Z * Inv);

    // the column titles (slot 0 of the local table is skipped). A visible
    // title is the idle colour, the sorted column's the amber the field, and one under the
    // mouse the pulse (the routine); then SetProperty(0x33, &colour, 1).
    WinControl* const Titles[5] = {nullptr, PriceTitle, BaseTitle, SystemTitle, BuySellTitle};
    for (int Column = 1; Column < 5; ++Column)
    {
        WinControl* const Title = Titles[Column];
        if (Title == nullptr || (Title->Flags & kWin3DVisible) == 0)
            continue;
        unsigned long Color = G_NnPaletteArgb_679bb0;
        if (SortColumn == Column)
        {
            Color = G_NnPaletteArgb_679ba4;
        }
        else if (Title->HitTest(Ray))
        {
            unsigned long Pulse = 0;
            NnUiPulseBlendArgb(&Pulse);
            Color = Pulse;
        }
        Title->SetProperty(0x33, reinterpret_cast<uintptr_t>(&Color), 1);
    }

    // a visible line (tested on its base name) is the idle colour, or the
    // pulse when the mouse is over any of its four controls; all four get the colour.
    for (int Line = 0; Line < 13; ++Line)
    {
        FCommodityByBaseRowControls const& Controls = Rows[Line];
        if (Controls.Base == nullptr || (Controls.Base->Flags & kWin3DVisible) == 0)
            continue;
        unsigned long Color = G_NnPaletteArgb_679bb0;
        if (Controls.Base->HitTest(Ray) || Controls.BuySell->HitTest(Ray) ||
            Controls.Price->HitTest(Ray) || Controls.System->HitTest(Ray))
        {
            unsigned long Pulse = 0;
            NnUiPulseBlendArgb(&Pulse);
            Color = Pulse;
        }
        Controls.Base->SetProperty(0x33, reinterpret_cast<uintptr_t>(&Color), 1);
        Controls.BuySell->SetProperty(0x33, reinterpret_cast<uintptr_t>(&Color), 1);
        Controls.Price->SetProperty(0x33, reinterpret_cast<uintptr_t>(&Color), 1);
        Controls.System->SetProperty(0x33, reinterpret_cast<uintptr_t>(&Color), 1);
    }
}

bool NnCommodityByBase::OnEvent(unsigned MsgId)
{
    // (RET 4) on the +0x32c face.
    if (HudIsMousePickBlocked())
        return false;
    if (MsgId != 0xf)
        return false;
    // the owner (this - 0x32c, whose null test cannot fail) closes.
    Close();
    return true;
}
