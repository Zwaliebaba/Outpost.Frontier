// Author: Giperion.

// CTradeEquipListLine / CTradeEquipList -- see trade_equip_list.h. baseline OutpostFrontier.exe
// .

#include "trade_equip_list.h"

#include "cargo_bar_control.h"
#include "character_select_equip.h" // GetPlayerEquipDescListbaseline (the routine), credits (the routine)
#include "client_cliobj.h"          // Archetype::CliObjMaterialPaths (companion +0x20)
#include "nn_button_control.h"
#include "star_sphere.h"            // Win3DIdentityRotation3x3 (the routine)
#include "win_factory.h"

#include <stdint.h>
#include <stdio.h>
#include <wchar.h>

char G_TradeWidgetName[0x400];
int G_TradeWidgetNameCounter = 0;    // (.bss)

namespace
{
    // L"%d".
    wchar_t const* const kCountFormat = L"%d";

    // s_equipment\models\commodities\nn_icons\COMMOD_credits.3db_005d6134.
    char const* const kCreditsIconMesh = "equipment\\models\\commodities\\nn_icons\\COMMOD_credits.3db";
    // s_interface\multiplayer\trade\TRADE_item_view.3db_005d6068.
    char const* const kItemViewMesh = "interface\\multiplayer\\trade\\TRADE_item_view.3db";
    // s_..._TRADE_plyr_item_info.3db_005d60a8.
    char const* const kItemInfoMesh = "interface\\multiplayer\\trade\\TRADE_plyr_item_info.3db";
    // s_..._TRADE_question.3db_005d60ec.
    char const* const kQuestionMesh = "interface\\multiplayer\\trade\\TRADE_question.3db";
    // s_..._TRADE_down_arrow.3db_005d6170 / _up_arrow_005d61a4 / _scrollhandle_005d61d4.
    char const* const kDownArrowMesh = "interface\\multiplayer\\trade\\TRADE_down_arrow.3db";
    char const* const kUpArrowMesh = "interface\\multiplayer\\trade\\TRADE_up_arrow.3db";
    char const* const kScrollHandleMesh = "interface\\multiplayer\\trade\\TRADE_scrollhandle.3db";
    // s_..._TRADE_inventoryscrollbar.3db_005d6214 / _transscrollbar.3db_005d6250.
    char const* const kInventoryScrollBarMesh =
        "interface\\multiplayer\\trade\\TRADE_inventoryscrollbar.3db";
    char const* const kTransferScrollBarMesh =
        "interface\\multiplayer\\trade\\TRADE_transscrollbar.3db";

    // The "merge one bit" idiom baseline uses on the ButtonControl flag bytes:
    // ((Bits ^ Old) & Mask) ^ Old.
    unsigned char MergeBits(unsigned char Old, unsigned char Bits, unsigned char Mask)
    {
        return static_cast<unsigned char>(((Bits ^ Old) & Mask) ^ Old);
    }

    uintptr_t PointerArg(void const* Pointer)
    {
        return reinterpret_cast<uintptr_t>(Pointer);
    }

    int PointerField(void const* Pointer)
    {
        return static_cast<int>(reinterpret_cast<intptr_t>(Pointer));
    }

    // baseline walks the circular VC6 list: from the first node, Steps times to ->next, where the
    // head sentinel sits after the last item and before the first. Position Size is the head.
    size_t CircularListPosition(size_t Size, int Steps)
    {
        if (Steps <= 0)
            return 0;
        return static_cast<size_t>(Steps) % (Size + 1);
    }

    // The node at a CircularListPosition result: Position forward steps from the first node.
    // Position Size is the head sentinel, end() here.
    std::list<EquipDesc>::iterator ListNodeAt(std::list<EquipDesc>& List, size_t Position)
    {
        std::list<EquipDesc>::iterator It = List.begin();
        for (size_t Step = 0; Step < Position; ++Step)
            ++It;
        return It;
    }
}

void FormatTradeWidgetName(char const* Base)
{
    // s_%s-%06d_005d601c, inlined at every widget of the trade window.
    sprintf(G_TradeWidgetName, "%s-%06d", Base, G_TradeWidgetNameCounter);
    ++G_TradeWidgetNameCounter;
}

void GetTradeItemVisuals(EquipDesc const* Item, CacheString* OutMesh, CacheString* OutMaterial,
                         int* OutIdsName, int* OutIdsInfo, bool bShopArchetype)
{
    // (cdecl).
    GoodInfo const* const Good = GoodList::find_by_archetype(Item->ArchId);
    OutMesh->clear();
    OutMaterial->clear();

    if (Good == nullptr)
    {
        // .
        Archetype::Equipment* const Arch = Archetype::GetEquipment(Item->ArchId);
        if (Arch == nullptr)
            return;
        OutMesh->Value = Arch->DaArchetype.Value;
        OutMaterial->clear();
        Archetype::ICliObj* const Companion = Arch->Companion;
        if (Companion != nullptr)
        {
            Archetype::FCliVc6Vector<Archetype::FCliMaterialPathPair> const* const Paths =
                Archetype::CliObjMaterialPaths(Companion);
            if (Paths->Begin != nullptr && Paths->End - Paths->Begin != 0)
                OutMaterial->Value = Paths->Begin->Path;
        }
        *OutIdsName = Arch->IdsName;
        *OutIdsInfo = Arch->IdsInfo;
        return;
    }

    // the icon, unless the caller asks for the shop model.
    if (Good->ItemIcon.Value != nullptr && !bShopArchetype)
    {
        OutMesh->Value = Good->ItemIcon.Value;
    }
    else
    {
        // .
        OutMesh->Value = Good->ShopArchetype.Value;
        OutMaterial->clear();
        Archetype::Equipment* const Arch = Archetype::GetEquipment(Item->ArchId);
        if (Good->MaterialLibrary.Value != nullptr)
        {
            OutMaterial->Value = Good->MaterialLibrary.Value;
        }
        else if (Arch != nullptr)
        {
            Archetype::ICliObj* const Companion = Arch->Companion;
            if (Companion != nullptr)
            {
                Archetype::FCliVc6Vector<Archetype::FCliMaterialPathPair> const* const Paths =
                    Archetype::CliObjMaterialPaths(Companion);
                if (Paths->Begin != nullptr && Paths->End - Paths->Begin != 0)
                    OutMaterial->Value = Paths->Begin->Path;
            }
        }
    }

    // .
    *OutIdsName = Good->IdsName;
    *OutIdsInfo = Good->IdsInfo;
}

// ---------------------------------------------------------------------------------------------
// CTradeEquipListLine
// ---------------------------------------------------------------------------------------------

CTradeEquipListLine::CTradeEquipListLine(CTradeEquipList* NewOwner)
    : Owner(NewOwner)
    , Item(nullptr)
{
    // Inlined at 0x004bf257..0x004bf260 (vftable 0x005d6028, +0x04 owner, +0x24 = 0). The
    // widget pointers are set by Init; +0x28 is left as allocated.
}

CTradeEquipListLine::~CTradeEquipListLine()
{
    // the vftable is reset and, with the delete flag, the row freed. The widgets
    // are children of the parent control and are not touched.
}

void CTradeEquipListLine::Init(WinControl* Parent, float const* Position)
{
    // . Base = Position moved 0.029 right (the field).
    float Base[3];
    Base[0] = Position[0] + 0.029f;
    Base[1] = Position[1];
    Base[2] = Position[2];
    float Zero[3];
    float At[3];

    if (Owner != nullptr && Owner->bInfoCard != 0)
    {
        // the "?" button 0.119 right and 0.021 up of Base
        // (the field, the field), created hidden and above everything else.
        At[0] = Base[0] + 0.119f;
        At[1] = Base[1] + 0.021f;
        At[2] = Position[2];
        Zero[0] = 0.0f;
        Zero[1] = 0.0f;
        Zero[2] = 0.0f;
        FormatTradeWidgetName("InfoCard");
        InfoCard = static_cast<NnButtonControl*>(Parent->CreateButtonControl(
            G_TradeWidgetName, kQuestionMesh, kQuestionMesh, nullptr, nullptr, At, nullptr, Zero,
            0, 0, 0, 1, 0, 0, 0));
        InfoCard->Field2f0 = PointerField(this);
        WinControlHalfExtentsFromMesh(InfoCard->HalfExtents, &InfoCard->MeshHolder);
        InfoCard->HitPriority = 1000;
        InfoCard->SetProperty(1, 1, 0);
    }
    else
    {
        InfoCard = nullptr;
    }

    // the info frame at Base.
    Zero[0] = 0.0f;
    Zero[1] = 0.0f;
    Zero[2] = 0.0f;
    FormatTradeWidgetName("InfoFrame");
    InfoFrame = static_cast<NnButtonControl*>(Parent->CreateButtonControl(
        G_TradeWidgetName, kItemInfoMesh, kItemInfoMesh, nullptr, nullptr, Base, nullptr, Zero, 1,
        0, 0, 1, 0, 0, 0));
    InfoFrame->Field2f0 = PointerField(this);
    WinControlHalfExtentsFromMesh(InfoFrame->HalfExtents, &InfoFrame->MeshHolder);

    // the item frame 0.175 left of Base ("333>" read as a float,
    // ), always drawn tinted (+0x337).
    At[0] = Base[0] - 0.175f;
    At[1] = Base[1];
    At[2] = Base[2];
    Zero[0] = 0.0f;
    Zero[1] = 0.0f;
    Zero[2] = 0.0f;
    FormatTradeWidgetName("IconFrame");
    IconFrame = static_cast<NnButtonControl*>(Parent->CreateButtonControl(
        G_TradeWidgetName, kItemViewMesh, kItemViewMesh, nullptr, nullptr, At, nullptr, Zero, 1,
        0, 0, 1, 0, 0, 0));
    IconFrame->Field2f0 = PointerField(this);
    IconFrame->SetByte337(1);
    WinControlHalfExtentsFromMesh(IconFrame->HalfExtents, &IconFrame->MeshHolder);

    // the icon at the same place, a StaticControl; zero half extents
    // and +0x338 vector (the stack slots 0x004bea6d..0x004bea95 clear), +0x418 = 100.
    float IconAt[3];
    IconAt[0] = Base[0] - 0.175f;
    IconAt[1] = Base[1];
    IconAt[2] = Base[2];
    Zero[0] = 0.0f;
    Zero[1] = 0.0f;
    Zero[2] = 0.0f;
    At[0] = 0.0f;
    At[1] = 0.0f;
    At[2] = 0.0f;
    FormatTradeWidgetName("IconMesh");
    IconMesh = Parent->CreateStaticControl(G_TradeWidgetName, kItemViewMesh, IconAt, nullptr, At, 0,
                                           0, 0, 1, 0, 1, 1, 100, 0, 0, Zero);
    IconMesh->Field2ec = PointerField(IconMesh);
    IconMesh->Field2f0 = PointerField(this);
    WinControlHalfExtentsFromMesh(IconMesh->HalfExtents, &IconMesh->MeshHolder);
    InfoFrame->Field2ec = PointerField(IconMesh);

    // the name, 0.01 left of Base (the field), style "ItemName".
    IconAt[0] = Base[0] - 0.01f;
    IconAt[1] = Base[1];
    IconAt[2] = Base[2];
    FormatTradeWidgetName("ItemName");
    ItemName = Parent->CreateTextControl(G_TradeWidgetName, "ItemName", IconAt, nullptr, 0.23f,
                                         0.066f, 1.5f, 0, 0, 0, 0);

    // the count, 0.03 right and 0.01 down of Base (the field,
    // ), TextControl +0x358 = 2.
    IconAt[0] = Base[0] + 0.03f;
    IconAt[1] = Base[1] - 0.01f;
    IconAt[2] = Base[2];
    FormatTradeWidgetName("ItemCount");
    ItemCount = Parent->CreateTextControl(G_TradeWidgetName, "ItemName", IconAt, nullptr, 0.19f,
                                          0.025f, 1.5f, 0, 2, 0, 0);

    // the health bar straight from the factory (no Init): a horizontal
    // green bar 0.0264 x 0.0032, 0.176 left ("X94>" read as a float, 0x005d6030) and 0.0272
    // down (the field) of Base, full (100 of 100), no border (property 0x19 with 0x10000).
    FormatTradeWidgetName("CargoBar");
    HealthBar = static_cast<CargoBarControl*>(
        WinFactoryCreate(G_TradeWidgetName, "CargoBarControl", Parent, nullptr));
    HealthBar->HalfExtents[0] = 0.0264f;
    HealthBar->HalfExtents[1] = 0.0032f;
    HealthBar->HalfExtents[2] = 0.0f;
    HealthBar->bHorizontal = 1;
    HealthBar->Offset[0] = Base[0] - 0.176f;
    HealthBar->Offset[1] = Base[1] - 0.0272f;
    HealthBar->Offset[2] = Base[2];
    HealthBar->Capacity = 100.0f;
    HealthBar->Used = 100.0f;
    HealthBar->Color[0] = 0x00;
    HealthBar->Color[1] = 0xc8;
    HealthBar->Color[2] = 0x00;
    HealthBar->Color[3] = 0xff;
    HealthBar->SetProperty(0x19, 0x10000, 0);
}

void CTradeEquipListLine::Hide()
{
    // .
    IconFrame->SetProperty(1, 1, 0);
    IconMesh->SetProperty(1, 1, 0);
    InfoFrame->SetProperty(1, 1, 0);
    HealthBar->SetProperty(1, 1, 0);
    ItemName->SetProperty(1, 1, 0);
    ItemCount->SetProperty(1, 1, 0);
    if (InfoCard != nullptr)
        InfoCard->SetProperty(1, 1, 0);
}

void CTradeEquipListLine::Show()
{
    // . The info card is hidden, not shown.
    IconFrame->SetProperty(1, 0, 0);
    IconFrame->SetProperty(2, 0, 0);
    IconMesh->SetProperty(1, 0, 0);
    IconMesh->SetProperty(2, 0, 0);
    InfoFrame->SetProperty(1, 0, 0);
    HealthBar->SetProperty(1, 0, 0);
    ItemName->SetProperty(1, 0, 0);
    ItemCount->SetProperty(1, 0, 0);
    if (InfoCard != nullptr)
        InfoCard->SetProperty(1, 1, 0);
}

void CTradeEquipListLine::SetHighlight(unsigned char bOn)
{
    // bits 2 and 3 of +0x331 on the item frame, then on the info frame.
    unsigned char const Bit2 = static_cast<unsigned char>(bOn << 2);
    unsigned char const Bit3 = static_cast<unsigned char>(bOn << 3);
    IconFrame->SetFlags331(MergeBits(IconFrame->GetFlags331(), Bit2, 0x04));
    IconFrame->SetFlags331(MergeBits(IconFrame->GetFlags331(), Bit3, 0x08));
    InfoFrame->SetFlags331(MergeBits(InfoFrame->GetFlags331(), Bit2, 0x04));
    InfoFrame->SetFlags331(MergeBits(InfoFrame->GetFlags331(), Bit3, 0x08));
}

void CTradeEquipListLine::SetItem(EquipDesc* NewItem)
{
    // .
    Show();

    if (NewItem->ArchId == 0)
    {
        // credits -- the credits icon, IDS 0x476, no info card, an empty
        // health bar.
        IconMesh->SetProperty(4, PointerArg(kCreditsIconMesh), 0);
        ItemName->SetProperty(0x1e, 0x476, 0);
        if (InfoCard != nullptr)
            InfoCard->SetProperty(1, 1, 0);
        HealthBar->Used = 0.0f;
    }
    else
    {
        // .
        CacheString Mesh;
        CacheString Material;
        Mesh.clear();
        Material.clear();
        int IdsName = 0;
        int IdsInfo;
        GetTradeItemVisuals(NewItem, &Mesh, &Material, &IdsName, &IdsInfo, false);
        if (Mesh.Value != nullptr)
            IconMesh->SetProperty(4, PointerArg(Mesh.Value), PointerArg(Material.Value));
        ItemName->SetProperty(0x1e, static_cast<uintptr_t>(IdsName), 0);
        // FLD health, FMUL double 100.0 (the field), ftol, FILD, FSTP float.
        HealthBar->Used = static_cast<float>(
            static_cast<int>(static_cast<double>(NewItem->Health) * 100.0));
    }

    // the count.
    wchar_t CountText[58];
    swprintf(CountText, 58, kCountFormat, NewItem->Count);
    ItemCount->SetProperty(0x1d, PointerArg(CountText), 0);

    // . Byte +0 of the entry is the trade selection: its bit 0 becomes
    // bit 6 of +0x330 on both frames; any non-zero value lifts the row to draw layer 2, else a
    // pressed (+0x330 bit 0) item frame lifts it to 1. The name text keeps its layer.
    Item = NewItem;
    unsigned char const Selection = static_cast<unsigned char>(NewItem->Cached & 0xff);
    unsigned char const Bit6 = static_cast<unsigned char>(Selection << 6);
    IconFrame->SetFlags330(MergeBits(IconFrame->GetFlags330(), Bit6, 0x40));
    InfoFrame->SetFlags330(MergeBits(InfoFrame->GetFlags330(), Bit6, 0x40));

    unsigned Layer;
    if (Selection != 0)
        Layer = 2;
    else if ((IconFrame->GetFlags330() & 0x01) != 0)
        Layer = 1;
    else
        Layer = 0;
    IconFrame->DrawLayer = Layer;
    IconMesh->DrawLayer = Layer;
    InfoFrame->DrawLayer = Layer;
    HealthBar->DrawLayer = Layer;
    ItemCount->DrawLayer = Layer;
    if (InfoCard != nullptr)
        InfoCard->DrawLayer = Layer;
    bInUse = 1;
}

// ---------------------------------------------------------------------------------------------
// CTradeEquipList
// ---------------------------------------------------------------------------------------------

CTradeEquipList::CTradeEquipList(WinControl* NewParent, char const* RowsHp,
                                 char const* ScrollBackHp, char const* ScrollUpHp,
                                 char const* ScrollDownHp, char const* ThumbTopHp,
                                 char const* ThumbBottomHp, int NewVisibleRows,
                                 unsigned char bNewInfoCard, unsigned char bNewHighlight)
    : ScrollBar(nullptr)
    , Parent(NewParent)
    , TopIndex(0)
    , VisibleRows(NewVisibleRows)
    , SelectedItem(nullptr)
    , bInfoCard(bNewInfoCard)
    , bSelectMode(0)
    , bHighlight(bNewHighlight)
    , CargoVolume(0.0)
{
    // . Lines, Items (list _Buynode), CreditsTemplate (EquipDesc ctor) first; then
    // +0x0c, +0x5c, vftable, +0x2c, +0x30, +0x5d, +0x60/+0x64, +0x5e, the template's count 0
    // and +0x58. +0x04 and +0x34 are not written.
    CreditsTemplate.set_count(0);

    // the first row at the translation of the parent's RowsHp
    // hardpoint (slot 36; an identity transform when it has none).
    float Xform[12];
    Win3DIdentityRotation3x3(Xform);
    Xform[9] = 0.0f;
    Xform[10] = 0.0f;
    Xform[11] = 0.0f;
    Parent->GetHardpointTransform(RowsHp, Xform);
    float RowAt[3];
    RowAt[0] = Xform[9];
    RowAt[1] = Xform[10];
    RowAt[2] = Xform[11];

    CTradeEquipListLine* Line = new CTradeEquipListLine(this);
    Line->Init(Parent, RowAt);
    Lines.insert(Lines.end(), Line);

    // the row pitch is the first info frame's full height.
    float RowHeight;
    if (Line->InfoFrame != nullptr)
        RowHeight = Line->InfoFrame->HalfExtents[1] + Line->InfoFrame->HalfExtents[1];
    else
        RowHeight = 0.0f;

    // the remaining rows, each one pitch lower.
    if (VisibleRows > 1)
    {
        for (int Row = VisibleRows - 1; Row != 0; --Row)
        {
            RowAt[1] = RowAt[1] - RowHeight;
            Line = new CTradeEquipListLine(this);
            Line->Init(Parent, RowAt);
            Lines.insert(Lines.end(), Line);
        }
    }

    // the scroll bar. Four rows is the transfer list's bar, anything
    // else the inventory's.
    char const* const BackMesh = (VisibleRows == 4) ? kTransferScrollBarMesh : kInventoryScrollBarMesh;
    float ThumbMouse[3] = {0.01f, 0.018f, 0.0f};
    float ThumbOffset1[3] = {0.0f, 0.0f, 0.0f};
    float ThumbOffset0[3] = {0.0f, 0.0f, 0.0f};
    float PrevMouse[3] = {0.012f, 0.009f, 0.0f};
    float PrevOffset[3] = {0.0f, 0.0f, 0.0f};
    float NextMouse[3] = {0.012f, 0.009f, 0.0f};
    float NextOffset[3] = {0.0f, 0.0f, 0.0f};
    float BackMouse[3] = {0.01f, 0.14f, 0.0f};
    float BackOffset[3] = {0.0f, 0.0f, 0.0f};
    FormatTradeWidgetName("ScrollBar");
    ScrollBar = Parent->CreateScrollControl(
        G_TradeWidgetName, BackMesh, BackOffset, ScrollBackHp, BackMouse, true, kDownArrowMesh,
        NextOffset, ScrollDownHp, NextMouse, false, kUpArrowMesh, PrevOffset, ScrollUpHp,
        PrevMouse, false, kScrollHandleMesh, ThumbOffset0, ThumbOffset1, ThumbTopHp,
        ThumbBottomHp, ThumbMouse, false, 0, 0, VisibleRows, false, false);
    ScrollBar->Field2f0 = PointerField(this);

    Refresh(-1);
}

CTradeEquipList::~CTradeEquipList()
{
    // every row through its deleting destructor, the row vector emptied; the
    // item list (erase + head) and then the row vector's buffer go with the members.
    for (size_t Index = 0; Index < Lines.size(); ++Index)
    {
        if (Lines[Index] != nullptr)
            delete Lines[Index];
    }
    Lines.clear();
}

int CTradeEquipList::SetMoney(int Count)
{
    // .
    EquipDesc* Credits = Items.traverse_equipment_type(0, nullptr);
    if (Credits != nullptr)
    {
        if (Count == 0)
        {
            // every entry with the credits entry's id goes. baseline
            // keeps comparing against the freed node; its id is read once here.
            unsigned short const CreditsId = Credits->Id;
            std::list<EquipDesc>::iterator It = Items.Items.begin();
            while (It != Items.Items.end())
            {
                if (It->Id == CreditsId)
                    It = Items.Items.erase(It);
                else
                    ++It;
            }
        }
        else
        {
            Credits->set_count(Count);
        }
    }
    else if (Count != 0)
    {
        // a copy of the template at the end (the routine), found
        // again and given the count.
        Items.Items.push_back(CreditsTemplate);
        Credits = Items.traverse_equipment_type(0, nullptr);
        if (Credits != nullptr)
            Credits->set_count(Count);
    }

    // .
    CreditsTemplate.set_count(Count);
    Refresh(-1);
    return Count;
}

void CTradeEquipList::Rebuild()
{
    // .
    EquipDescList& PlayerList = GetPlayerEquipDescListbaseline();
    Items.Items.clear(); // list::erase(begin, end)

    if (GetPlayerCreditsbaseline() != 0)
    {
        CreditsTemplate.set_count(static_cast<int>(GetPlayerCreditsbaseline()));
        Items.Items.push_back(CreditsTemplate); // list::insert(end, template)
    }

    // cargo only -- nothing mounted (+0x0c), nothing from a mission
    // (+0x18).
    for (std::list<EquipDesc>::const_iterator It = PlayerList.Items.begin();
         It != PlayerList.Items.end(); ++It)
    {
        EquipDesc const& Desc = *It;
        if (Desc.bEquipped || Desc.bMission)
            continue;
        Items.Items.push_back(Desc);
    }

    Refresh(-1);

    // scroll range [0, max(0, size - rows)].
    int Range = static_cast<int>(Items.Items.size()) - VisibleRows;
    if (Range < 0)
        Range = 0;
    ScrollBar->SetProperty(0x37, 0, static_cast<uintptr_t>(Range));
}

void CTradeEquipList::Refresh(int Index)
{
    // .
    size_t const Size = Items.Items.size();

    if (Index < 0)
    {
        // from the scroll position, one item per row.
        std::list<EquipDesc>::iterator Node =
            ListNodeAt(Items.Items, CircularListPosition(Size, TopIndex));
        size_t Row = 0;
        if (Row == Lines.size())
            return;
        while (Node != Items.Items.end())
        {
            CTradeEquipListLine* const Line = Lines[Row];
            Line->SetItem(&*Node);
            if (bSelectMode != 0)
            {
                // only the frame, icon and bar; credits greyed out.
                Line->Hide();
                Line->IconFrame->SetProperty(1, 0, 0);
                Line->IconMesh->SetProperty(1, 0, 0);
                Line->HealthBar->SetProperty(1, 0, 0);
                if (Line->Item != nullptr && Line->Item->ArchId == 0)
                {
                    Line->IconFrame->SetProperty(2, 1, 0);
                    Line->IconMesh->SetProperty(2, 1, 0);
                }
                Line->SetHighlight(1);
            }
            else
            {
                // the routine inlined with +0x5e.
                Line->SetHighlight(bHighlight);
            }
            ++Node;
            ++Row;
            if (Row == Lines.size())
                break;
        }

        // the rows past the end of the list are blanked and hidden.
        for (; Row < Lines.size(); ++Row)
        {
            CTradeEquipListLine* const Line = Lines[Row];
            Line->IconMesh->SetProperty(4, PointerArg(kItemViewMesh), 0);
            Line->ItemName->SetProperty(0x1e, 0, 0);
            Line->ItemCount->SetProperty(0x1e, 0, 0);
            Line->bInUse = 0;
            Line->Hide();
        }
        return;
    }

    // one item, when its row is on screen. Every baseline caller (all 21
    // XREFs of the routine) passes -1, so this path never runs; an Index at or past the end
    // would reach the list's head node in baseline.
    int const Row = Index - TopIndex;
    if (Row >= VisibleRows || Row < 0)
        return;
    std::list<EquipDesc>::iterator const Node =
        ListNodeAt(Items.Items, CircularListPosition(Size, Index));
    // is the vector's "invalid vector<T> subscript" throw -- at() here.
    Lines.at(static_cast<size_t>(Row))->SetItem(&*Node);
    if (bSelectMode != 0)
    {
        CTradeEquipListLine* const Line = Lines.at(static_cast<size_t>(Row));
        Line->Hide();
        Line->IconFrame->SetProperty(1, 0, 0);
        Line->IconMesh->SetProperty(1, 0, 0);
        Line->HealthBar->SetProperty(1, 0, 0);
        if (Line->Item != nullptr && Line->Item->ArchId == 0)
        {
            Line->IconFrame->SetProperty(2, 1, 0);
            Line->IconMesh->SetProperty(2, 1, 0);
        }
        Line->SetHighlight(1);
        return;
    }
    Lines.at(static_cast<size_t>(Row))->SetHighlight(bHighlight);
}
