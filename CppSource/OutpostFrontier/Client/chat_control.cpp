// Author: Giperion.

// ChatControl -- the NeuroNet chat panel (see chat_control.h for the object).

// factory init       the routine ctor       the routine dtor (the routine)
// broadcast callback the routine FillBroadcast
// private callback   the routine RenderPrivateRow
// ..the branch the eight qsort comparators (tables 0x006115a8 / 0x006115b8)
// FillPrivate        the routine Destroy    the branch Create
// Slot23             the branch SetProperty
// Update             the routine RefreshCounts
// ..the branch the layout floats (the field..the field)

#include "chat_control.h"

#include "chat_msg_win.h"          // MsgWin (HistoryDisplay, the field)
#include "chat_text_input.h"       // ChatTextInputDisplay ("CTID")
#include "ids_rdl.h"               // LoadIdsTextIntoRdl, LoadIdsIntoRdl, RdlRemoveLastParagraph, RdlAppendNode, RdlClear
#include "menu_ui_widgets.h"
#include "nn_group_control.h"      // NnGroupControl::GroupMembers (the field +0x340)
#include "nn_modal.h"              // NnMainButtonOnModalOpen (the routine)
#include "nn_ui_pulse.h"           // G_HudChromeColorRgba (the field)
#include "player_spawn.h"          // GetPlayerConnectionId (the field)
#include "rich_text_win.h"
#include "tooltip_hover.h"         // FToolTipHoverInfo
#include "ui_page_desc.h"          // FUiPayloadTable
#include "win3d.h"                 // Win3DReparent (the routine), Win3DFindByName (the routine)
#include "win_factory.h"

#include <stdlib.h>
#include <wchar.h>

#include <vector>

// WINFACTORY<ChatControl> (the branch -> the routine("ChatControl"),
// atexit the branch; create the routine: operator_new(0x3d0) + the routine, then Create).
WINFACTORY<ChatControl> G_ChatControlFactory("ChatControl");

namespace
{
    // The CRT initializers the branch..the branch, in table order. Only the field and
    // ..the field are read (by Create); the rest are written by their initializer
    // and read by nothing but the next initializer in the listing.
    float G_ChatHistoryWidth = 0.46f;
    float G_ChatUnused4d4 = 0.8f * -0.5f;
    float G_ChatUnused4d0 = G_ChatUnused4d4;
    float G_ChatUnused4cc = static_cast<float>(static_cast<double>(0.6f) * 0.7);
    float G_ChatUnused4c8 = -0.45f;
    float G_ChatUnused4c4 = static_cast<float>(static_cast<double>(0.46f) +
                                               static_cast<double>(-0.05f) -
                                               static_cast<double>(0.35f));
    float G_ChatUnused4c0 = G_ChatUnused4c4;
    float G_ChatUnused4bc = static_cast<float>(0.2 - static_cast<double>(G_ChatUnused4cc * 0.5f));
    float G_ChatBroadcastWidth = 0.35f;
    float G_ChatBroadcastX = -0.45f;
    float G_ChatPrivateWidth = 0.35f;
    float G_ChatPrivateX = -0.45f;
    float G_ChatPrivateY = static_cast<float>(static_cast<double>(0.215f) - 0.075 -
                                              static_cast<double>(0.03f));

    // the column header IDS, in column order.
    unsigned const kChatHeaderIds[4] = {0x6002eu, 0x6002du, 0x6002fu, 0x60030u};
    // the private list's first four column edges.
    float const kChatPrivateColumnEdges[4] = {0.0f, 0.086143f, 0.163329f, 0.234f};
    // u"  "
    wchar_t const kChatGap[] = L"  ";

    // The clone-into-list idiom (the routine with a fresh clone, first clone deleted; the inline
    // + the routine insertion is the same thing).
    void AppendNodeClone(RenderDisplayList& List, RDLNode const& Node)
    {
        RDLNode* const Temp = Node.Clone();
        RdlAppendNode(List, Temp->Clone());
        delete Temp;
    }

    // (guard the field bit 0, atexit IDPMsgHandler::OnDisconnect -- an empty
    // body): the zero record both lists fall back to when a name has no player info.
    FChatPlayerInfo* ChatZeroPlayerInfo()
    {
        static FChatPlayerInfo Zero = {0u, 0u, 0u};
        return &Zero;
    }

    // The inlined the routine on the table's +0x18 map: the value, or the zero record.
    FChatPlayerInfo* ChatControlInfoFor(FChatChannelTable* Table, unsigned Key)
    {
        FChatPlayerInfo* const Found = Table->FindPlayerInfo(CHAT_ID(Key));
        return Found != nullptr ? Found : ChatZeroPlayerInfo();
    }

    bool ChatKeyIsChannel(unsigned Key)
    {
        return ((Key >> 16) & 0xffu) != 0;
    }

    // The qsort comparators. Entries are FChatControlEntry.
    int __cdecl ChatCompareNameAsc(void const* A, void const* B)
    {

        FChatControlEntry const* const Ea = static_cast<FChatControlEntry const*>(A);
        FChatControlEntry const* const Eb = static_cast<FChatControlEntry const*>(B);
        return lex_compare(*Ea->Name, *Eb->Name, false);
    }

    int __cdecl ChatCompareField0Asc(void const* A, void const* B)
    {

        FChatControlEntry const* const Ea = static_cast<FChatControlEntry const*>(A);
        FChatControlEntry const* const Eb = static_cast<FChatControlEntry const*>(B);
        return static_cast<int>(Ea->Info->Field_0x0 - Eb->Info->Field_0x0);
    }

    int __cdecl ChatCompareRankAsc(void const* A, void const* B)
    {

        FChatControlEntry const* const Ea = static_cast<FChatControlEntry const*>(A);
        FChatControlEntry const* const Eb = static_cast<FChatControlEntry const*>(B);
        return static_cast<int>(Ea->Info->Rank - Eb->Info->Rank);
    }

    int __cdecl ChatCompareSystemAsc(void const* A, void const* B)
    {
        // the routine on the field for B first, then A.
        FChatControlEntry const* const Ea = static_cast<FChatControlEntry const*>(A);
        FChatControlEntry const* const Eb = static_cast<FChatControlEntry const*>(B);
        RenderDisplayList* const NameB = ChatGetSystemName(Eb->Info->Field_0x8);
        RenderDisplayList* const NameA = ChatGetSystemName(Ea->Info->Field_0x8);
        return lex_compare(*NameA, *NameB, false);
    }

    int __cdecl ChatCompareNameDesc(void const* A, void const* B)
    {

        FChatControlEntry const* const Ea = static_cast<FChatControlEntry const*>(A);
        FChatControlEntry const* const Eb = static_cast<FChatControlEntry const*>(B);
        return -lex_compare(*Ea->Name, *Eb->Name, false);
    }

    int __cdecl ChatCompareField0Desc(void const* A, void const* B)
    {

        FChatControlEntry const* const Ea = static_cast<FChatControlEntry const*>(A);
        FChatControlEntry const* const Eb = static_cast<FChatControlEntry const*>(B);
        return static_cast<int>(Eb->Info->Field_0x0 - Ea->Info->Field_0x0);
    }

    int __cdecl ChatCompareRankDesc(void const* A, void const* B)
    {

        FChatControlEntry const* const Ea = static_cast<FChatControlEntry const*>(A);
        FChatControlEntry const* const Eb = static_cast<FChatControlEntry const*>(B);
        return static_cast<int>(Eb->Info->Rank - Ea->Info->Rank);
    }

    int __cdecl ChatCompareSystemDesc(void const* A, void const* B)
    {

        FChatControlEntry const* const Ea = static_cast<FChatControlEntry const*>(A);
        FChatControlEntry const* const Eb = static_cast<FChatControlEntry const*>(B);
        RenderDisplayList* const NameB = ChatGetSystemName(Eb->Info->Field_0x8);
        RenderDisplayList* const NameA = ChatGetSystemName(Ea->Info->Field_0x8);
        return -lex_compare(*NameA, *NameB, false);
    }

    typedef int(__cdecl* FChatCompareFn)(void const*, void const*);

    // (direction byte non-zero) / the branch (zero), by sort column.
    FChatCompareFn const kChatCompareAsc[4] = {ChatCompareNameAsc, ChatCompareField0Asc,
                                               ChatCompareRankAsc, ChatCompareSystemAsc};
    FChatCompareFn const kChatCompareDesc[4] = {ChatCompareNameDesc, ChatCompareField0Desc,
                                                ChatCompareRankDesc, ChatCompareSystemDesc};

    // (cdecl)
    void ChatControlBroadcastCallback(void* Context, int FirstItem, unsigned VisibleRows)
    {
        if (Context != nullptr)
            static_cast<ChatControl*>(Context)->FillBroadcast(FirstItem, VisibleRows);
    }

    // (cdecl)
    void ChatControlPrivateCallback(void* Context, int FirstItem, unsigned VisibleRows)
    {
        if (Context != nullptr)
            static_cast<ChatControl*>(Context)->FillPrivate(FirstItem, VisibleRows);
    }

    // The sequence every refresh site spells out inline: the routine, then the routine and
    // with each list's the routine and its +0x4a0 vector size.
    void ChatControlRefreshAll(ChatControl* Control)
    {
        Control->RefreshCounts();
        Control->FillPrivate(Control->PrivateList->GetFirstItem(),
                             static_cast<unsigned>(Control->PrivateList->RowEdges.size()));
        Control->FillBroadcast(Control->BroadcastList->GetFirstItem(),
                               static_cast<unsigned>(Control->BroadcastList->RowEdges.size()));
    }

    // Create's row payload loop (0x004678e3..0x00467975 / 0x00467ab0..0x00467b42): a fresh
    // RowData (count 1) assigned to the row (AddRef), then the local reference dropped.
    void ChatControlSeedRowPayloads(UiScrollTable* List, unsigned RowCount)
    {
        for (unsigned Row = 0; Row < RowCount; ++Row)
        {
            FUiFastTableRow* const TableRow = List->GetRow(Row);
            if (TableRow == nullptr)
                continue;
            UiFastTableRefPtr Local;
            Local.Object = new ChatControl::RowData();
            TableRow->Payload = Local;
        }
    }
}

// ---------------------------------------------------------------------------------------------

ChatControl::ChatControl()
    : Field_0x32c(0)
    , TableGeneration(0)
    , GroupColorImage(nullptr)
    , SystemColorImage(nullptr)
    , InactiveColorImage(nullptr)
    , UpPipImage(nullptr)
    , DownPipImage(nullptr)
    , SortColumn(1)
    , Table(nullptr)
    , HistoryDisplay(nullptr)
    , HistoryTitle(nullptr)
    , BroadcastTitle(nullptr)
    , PrivateTitle(nullptr)
    , BroadcastList(nullptr)
    , PrivateList(nullptr)
    , BtnCCClose(nullptr)
{
    // WinControl, the four RDLs and the header array, the eight control pointers
    // and +0x32c / +0x330 zeroed, +0x3a8 = 1, the +0x3ac dword = 0. The five image pointers
    // (+0x394..+0x3a4) are not written by baseline; the port starts them null.
    SortAscending[0] = 0;
    SortAscending[1] = 0;
    SortAscending[2] = 0;
    SortAscending[3] = 0;
}

ChatControl::~ChatControl()
{
    // the header array, then +0x358 / +0x34c / +0x340 / +0x334, then WinControl's
    // dtor (the routine) -- member destruction order.
}

bool ChatControl::Create(Win3D* NewParent, void* Data)
{
    // the routine(NewParent, 0); Data is not passed on.
    (void)Data;
    if (!Win3D::Create(NewParent, nullptr))
        return false;


    Offset[0] = 0.0f;
    Offset[1] = 0.056f;
    Offset[2] = 0.0f;
    // slot 38 (+0x98), then +0x2d2 = 1.
    SetMesh("Interface\\NeuroNet\\Chat\\CHAT_background.cmp", 0);
    Field2d2 = 1;

    // the titles, each a StyleNode(0xfaa9) then the IDS idiom; "None"
    // without a style.
    {
        StyleNode Style(0xfaa9);
        AppendNodeClone(BroadcastTitleText, Style);
    }
    LoadIdsTextIntoRdl(0x6002cu, BroadcastTitleText, L"DEBUG - Broadcast Channels");
    {
        StyleNode Style(0xfaa9);
        AppendNodeClone(HistoryTitleText, Style);
    }
    LoadIdsTextIntoRdl(0x60032u, HistoryTitleText, L"DEBUG - Chat History");
    {
        StyleNode Style(0xfaa9);
        AppendNodeClone(PrivateTitleText, Style);
    }
    LoadIdsTextIntoRdl(0x60033u, PrivateTitleText, L"DEBUG - Private Channels");
    LoadIdsTextIntoRdl(0x60054u, NoneText, L"DEBUG - None");

    // the headers -- the routine + the routine, no debug fallback.
    for (unsigned Column = 0; Column < 4; ++Column)
    {
        if (LoadIdsIntoRdl(kChatHeaderIds[Column], Headers[Column]))
            RdlRemoveLastParagraph(Headers[Column]);
    }


    HistoryDisplay = static_cast<MsgWin*>(WinFactoryCreate("HistoryDisplay", "MsgWin", this, nullptr));
    if (HistoryDisplay != nullptr)
    {
        float const AttachOffset[3] = {-0.05f, 0.215f, 0.0f};  // / the field
        HistoryDisplay->SetAttachOffset(AttachOffset);
        HistoryDisplay->SetNormSize(G_ChatHistoryWidth, 0.335f);  // / the field
        HistoryDisplay->SetAutoScrollbar(true);
        HistoryDisplay->SnapshotGeneration = 0;
        HistoryDisplay->Snapshot = G_ChatAllMessages;
        HistoryDisplay->SetFrameColor(reinterpret_cast<unsigned long const*>(G_HudChromeColorRgba));
        HistoryDisplay->SetDrawFrame(false);
        HistoryDisplay->SetKeepScrolledToEnd(true);
        HistoryDisplay->GetToolTipHoverInfo()->IdsAlt = 0x1b7c;
        HistoryDisplay->SetToolTipEnabled(true);
    }


    {
        float const Position[3] = {0.44f, 0.23f, 0.0f};     // / 0x3e6b851f
        float const Half[3] = {0.02f, 0.02f, 0.0f};
        BtnCCClose = CreateButtonControl("BtnCCClose", "Interface\\NeuroNet\\Inventory\\X.3db",
                                         nullptr, "ui_select_item", nullptr, Position, nullptr,
                                         Half, 0, 0, 0, 0, 0x409, 0, 0);
    }

    // BroadcastList, 0.35 x 0.05, 2 rows, 1 column.
    {
        FUiPayloadTable Payload;
        Payload.Right = G_ChatBroadcastWidth;
        Payload.Bottom = 0.05f;
        Payload.Int5 = 2;
        Payload.Int6 = 1;
        BroadcastList = static_cast<UiScrollTable*>(
            WinFactoryCreate("BroadcastList", "UIScrollTable", this, &Payload));
    }
    if (BroadcastList != nullptr)
    {
        float const AttachOffset[3] = {G_ChatBroadcastX, 0.215f, 0.0f};   // / the field
        BroadcastList->SetAttachOffset(AttachOffset);
        BroadcastList->SetFrameColor(reinterpret_cast<unsigned long const*>(G_HudChromeColorRgba));
        BroadcastList->SetResizable(false, false);
        BroadcastList->SetLayoutFlag(true);
        BroadcastList->SetBorders(false, false, false, true);
        BroadcastList->SetRowHighlights(false);
        BroadcastList->SetTipMode(2);
        BroadcastList->SetToolTipEnabled(true);
        BroadcastList->SetScrollBarGap(BroadcastList->GetScrollBarGap() * 4.0f);
        BroadcastList->SetCallback(ChatControlBroadcastCallback, this);
        ChatControlSeedRowPayloads(BroadcastList, 2);
    }

    // PrivateList, 0.35 x 0.235, 11 rows, 4 columns.
    {
        FUiPayloadTable Payload;
        Payload.Right = G_ChatPrivateWidth;
        Payload.Bottom = 0.235f;
        Payload.Int5 = 0xb;
        Payload.Int6 = 4;
        PrivateList = static_cast<UiScrollTable*>(
            WinFactoryCreate("PrivateList", "UIScrollTable", this, &Payload));
    }
    if (PrivateList != nullptr)
    {
        float const AttachOffset[3] = {G_ChatPrivateX, G_ChatPrivateY, 0.0f};   // / the field
        PrivateList->SetAttachOffset(AttachOffset);
        PrivateList->SetFrameColor(reinterpret_cast<unsigned long const*>(G_HudChromeColorRgba));
        PrivateList->SetResizable(false, false);
        PrivateList->SetLayoutFlag(true);
        PrivateList->SetDrawColumnLines(false);
        PrivateList->SetBorders(false, false, false, true);
        PrivateList->SetRowHighlights(false);
        PrivateList->SetCellMargins(2, 0);
        PrivateList->SetTipMode(0);
        PrivateList->GetToolTipHoverInfo()->IdsAlt = 0x1b74;
        PrivateList->SetToolTipEnabled(true);
        PrivateList->SetScrollBarGap(PrivateList->GetScrollBarGap() * 4.0f);
        PrivateList->SetCallback(ChatControlPrivateCallback, this);
        ChatControlSeedRowPayloads(PrivateList, 0xb);

        // the routine, the first four column edges from the field,
        // . baseline writes the four floats without a size test.
        FUiFastTableEdges Edges;
        PrivateList->GetEdges(&Edges);
        for (unsigned Column = 0; Column < 4; ++Column)
            Edges.Cols[Column] = kChatPrivateColumnEdges[Column];
        PrivateList->SetEdges(&Edges);
    }

    // the images, through the private list's TRC (no null test).
    UpPipImage = PrivateList->GetDeviceTRC()->create_image("interface\\intro\\images\\uppip.bmp");
    DownPipImage = PrivateList->GetDeviceTRC()->create_image("interface\\intro\\images\\DownPip.bmp");
    GroupColorImage =
        PrivateList->GetDeviceTRC()->create_image("interface\\intro\\images\\GroupColor.bmp");
    SystemColorImage =
        PrivateList->GetDeviceTRC()->create_image("interface\\intro\\images\\SystemColor.bmp");
    InactiveColorImage =
        PrivateList->GetDeviceTRC()->create_image("interface\\intro\\images\\InactiveColor.bmp");

    // the three titles, 0.35 x 0.03, 0.03 above their panels.
    {
        float const AttachOffset[3] = {G_ChatBroadcastX, 0.03f + 0.215f, 0.0f};
        BroadcastTitle = CreateRichTextWin("BroadcastTitle", 0.35f, 0.03f, AttachOffset, false);
        RDLRefNode Ref(BroadcastTitleText);
        BroadcastTitle->AppendNode(Ref);
    }
    {
        float const AttachOffset[3] = {-0.05f, 0.03f + 0.215f, 0.0f};
        HistoryTitle = CreateRichTextWin("HistoryTitle", 0.35f, 0.03f, AttachOffset, false);
        RDLRefNode Ref(HistoryTitleText);
        HistoryTitle->AppendNode(Ref);
    }
    {
        float const AttachOffset[3] = {G_ChatPrivateX, 0.03f + G_ChatPrivateY, 0.0f};
        PrivateTitle = CreateRichTextWin("PrivateTitle", 0.35f, 0.03f, AttachOffset, false);
        RDLRefNode Ref(PrivateTitleText);
        PrivateTitle->AppendNode(Ref);
    }

    // bind the table when it exists and fill both lists ...
    if (G_ChatChannelTable != nullptr)
    {
        Table = G_ChatChannelTable;
        ChatControlRefreshAll(this);
    }
    // ... and then once more unconditionally.
    ChatControlRefreshAll(this);


    WinControlHalfExtentsFromMesh(HalfExtents, &MeshHolder);
    return true;
}

void ChatControl::Destroy()
{
    // DeviceTRC slot 11 (+0x2c) destroy_image, the TRC fetched each time.
    PrivateList->GetDeviceTRC()->destroy_image(UpPipImage);
    PrivateList->GetDeviceTRC()->destroy_image(DownPipImage);
    PrivateList->GetDeviceTRC()->destroy_image(GroupColorImage);
    PrivateList->GetDeviceTRC()->destroy_image(SystemColorImage);
    PrivateList->GetDeviceTRC()->destroy_image(InactiveColorImage);
    WinControl::Destroy();  // tail jump the routine
}

void ChatControl::Update()
{

    if (Table != nullptr && TableGeneration != Table->Generation)
    {
        ChatControlRefreshAll(this);
        TableGeneration = Table->Generation;
    }
    Win3D::Update();    // tail jump the routine
}

void ChatControl::RefreshCounts()
{
    // None is skipped; byte 2 set counts a channel, clear a player.
    unsigned Players = 0;
    unsigned Channels = 0;
    if (Table != nullptr)
    {
        for (FChatChannelTable::FNameMap::const_iterator It = Table->Names.begin();
             It != Table->Names.end(); ++It)
        {
            unsigned const Key = It->first;
            if (Key == G_ChatIdNone.Id)
                continue;
            if (ChatKeyIsChannel(Key))
                ++Channels;
            else
                ++Players;
        }
    }
    PrivateList->SetItemCount(Players * 2, 1);
    BroadcastList->SetItemCount(Channels, 0);
}

void ChatControl::FillBroadcast(int FirstItem, unsigned VisibleRows)
{

    if (Table == nullptr)
        return;

    CHAT_ID const Current = G_ChatInputTarget;     // , read once

    // the channel entries (byte 2 set, not None), alloca'd in baseline.
    std::vector<FChatControlEntry> Entries(Table->Names.size());
    int Count = 0;
    for (FChatChannelTable::FNameMap::const_iterator It = Table->Names.begin();
         It != Table->Names.end(); ++It)
    {
        unsigned const Key = It->first;
        if (!ChatKeyIsChannel(Key) || Key == G_ChatIdNone.Id)
            continue;
        FChatControlEntry& Entry = Entries[static_cast<size_t>(Count)];
        Entry.Key = Key;
        Entry.Name = It->second;
        Entry.Info = ChatControlInfoFor(Table, Key);
        ++Count;
    }


    BroadcastList->ClearSelection();

    unsigned Row = 0;
    if (FirstItem < Count)
    {
        do
        {
            // out of visible rows ends the whole function.
            if (Row >= VisibleRows)
                return;

            FChatControlEntry const& Entry =
                Entries[static_cast<size_t>(FirstItem) + static_cast<size_t>(Row)];
            FUiFastTableCell* Cell = nullptr;
            if (BroadcastList->GetCell(Row, 0, &Cell))
            {
                unsigned char bEnabled = 1;
                RenderDisplayList& Text = Cell->Source;
                RdlClear(Text);
                {
                    JustifyNode Justify(TextJustifyLeft);
                    AppendNodeClone(Text, Justify);
                }

                unsigned TipIds = 0;
                if (Entry.Key == Current.Id)
                {
                    StyleNode Style(0x3343);
                    AppendNodeClone(Text, Style);
                }
                else
                {
                    {
                        StyleNode Style(0x8283);
                        AppendNodeClone(Text, Style);
                    }
                    if (Entry.Key == G_ChatIdChannel1.Id)
                    {
                        StyleNode Style(0xd801);
                        AppendNodeClone(Text, Style);
                    }
                    else if (Entry.Key == G_ChatIdChannel2.Id)
                    {
                        StyleNode Style(0x1d9d);
                        AppendNodeClone(Text, Style);
                    }
                    else if (Entry.Key == G_ChatIdChannel3.Id)
                    {
                        {
                            StyleNode Style(0xd801);
                            AppendNodeClone(Text, Style);
                        }
                        // the group channel is live only with group members.
                        if (G_ChatGroupControl != nullptr)
                            bEnabled = G_ChatGroupControl->GroupMembers.empty() ? 0 : 1;
                    }
                    else if (Entry.Key == G_ChatIdNone.Id)
                    {
                        StyleNode Style(0x73f7);
                        AppendNodeClone(Text, Style);
                    }
                    else if (Entry.Key == G_ChatIdChannel0.Id)
                    {
                        StyleNode Style(0xd681);
                        AppendNodeClone(Text, Style);
                    }
                    else
                    {
                        StyleNode Style(0x8283);
                        AppendNodeClone(Text, Style);
                    }
                }

                // the row tooltip.
                if (Entry.Key == G_ChatIdChannel1.Id)
                    TipIds = 0x1b76;
                else if (Entry.Key == G_ChatIdChannel2.Id)
                    TipIds = 0x1b77;
                else if (Entry.Key == G_ChatIdChannel3.Id)
                    TipIds = 0x1b78;
                else if (Entry.Key == G_ChatIdNone.Id)
                    TipIds = 0x1b8c;
                FToolTipHoverInfo* const Tip = BroadcastList->GetRowTip(Row);
                if (Tip != nullptr)
                    Tip->IdsAlt = static_cast<int>(TipIds);

                if (!bEnabled)
                {
                    StyleNode Style(0xbb80);
                    AppendNodeClone(Text, Style);
                }
                {
                    TextNode Gap(kChatGap, -1);
                    AppendNodeClone(Text, Gap);
                }
                if (bEnabled)
                {
                    if (Entry.Key == G_ChatIdChannel1.Id)
                    {
                        ImageNode Image(SystemColorImage);
                        AppendNodeClone(Text, Image);
                    }
                    else if (Entry.Key == G_ChatIdChannel3.Id)
                    {
                        ImageNode Image(GroupColorImage);
                        AppendNodeClone(Text, Image);
                    }
                }
                else
                {
                    ImageNode Image(InactiveColorImage);
                    AppendNodeClone(Text, Image);
                }
                {
                    TextNode Gap(kChatGap, -1);
                    AppendNodeClone(Text, Gap);
                }
                ChatAppendRdlCopy(Text, *Entry.Name);


                FUiFastTableRow* const TableRow = BroadcastList->GetRow(Row);
                TableRow->bHoverable = bEnabled;
                TableRow->bSelectable = bEnabled;
                TableRow->bRowLine = 1;
                static_cast<RowData*>(TableRow->Payload.Object)->ChatId = CHAT_ID(Entry.Key);
                if (Entry.Key == Current.Id)
                    BroadcastList->SelectRow(Row);
                BroadcastList->InvalidateCell(Row, 0);
            }
            ++Row;
        } while (FirstItem + static_cast<int>(Row) < Count);
    }

    // the rows past the last channel.
    for (; Row < VisibleRows; ++Row)
    {
        BroadcastList->ClearCell(Row, 0);
        FUiFastTableRow* const TableRow = BroadcastList->GetRow(Row);
        TableRow->bHoverable = 0;
        TableRow->bSelectable = 0;
        TableRow->bRowLine = 0;
    }
}

void ChatControl::RenderPrivateRow(unsigned Row, FChatControlEntry const* Entry, bool bEnabled)
{

    CHAT_ID const Current = G_ChatInputTarget;     // , read once
    unsigned char const bEnabledByte = bEnabled ? 1 : 0;
    FUiFastTableCell* Cell = nullptr;

    // the name cell, spanning the row.
    if (PrivateList->GetCell(Row, 0, &Cell))
    {
        RenderDisplayList& Text = Cell->Source;
        RdlClear(Text);
        {
            JustifyNode Justify(TextJustifyLeft);
            AppendNodeClone(Text, Justify);
        }
        {
            StyleNode Style(static_cast<unsigned short>(Entry->Key == Current.Id ? 0x3343 : 0x69f5));
            AppendNodeClone(Text, Style);
        }
        if (!bEnabled)
        {
            StyleNode Style(0xbb80);
            AppendNodeClone(Text, Style);
        }
        ChatAppendRdlCopy(Text, *Entry->Name);
        PrivateList->SetCellSpanToEnd(Row, 0, true);
        PrivateList->InvalidateCell(Row, 0);
    }

    for (unsigned Column = 1; Column < 4; ++Column)
    {
        if (PrivateList->GetCell(Row, Column, &Cell))
            RdlClear(Cell->Source);
    }


    {
        FUiFastTableRow* const TableRow = PrivateList->GetRow(Row);
        TableRow->bHoverable = bEnabledByte;
        TableRow->bSelectable = bEnabledByte;
        TableRow->bRowLine = 1;
        TableRow->GroupId = Row;
        static_cast<RowData*>(TableRow->Payload.Object)->ChatId = CHAT_ID(Entry->Key);
        if (Entry->Key == Current.Id)
            PrivateList->SelectRow(Row);
    }

    // the detail row below.
    unsigned const Detail = Row + 1;
    FUiFastTableRow* const DetailRow = PrivateList->GetRow(Detail);
    if (DetailRow == nullptr)
        return;

    unsigned short const DetailStyle =
        static_cast<unsigned short>(Entry->Key == Current.Id ? 0x3dbc : 0x5203);
    wchar_t Buffer[64];

    // column 1 -- Field_0x0, or the "None" text when zero.
    if (PrivateList->GetCell(Detail, 1, &Cell))
    {
        RenderDisplayList& Text = Cell->Source;
        RdlClear(Text);
        {
            JustifyNode Justify(TextJustifyCenter);
            AppendNodeClone(Text, Justify);
        }
        {
            StyleNode Style(DetailStyle);
            AppendNodeClone(Text, Style);
        }
        if (Entry->Info->Field_0x0 != 0)
        {
            swprintf(Buffer, sizeof(Buffer) / sizeof(Buffer[0]), L"%u", Entry->Info->Field_0x0);
            TextNode Value(Buffer, -1);
            AppendNodeClone(Text, Value);
        }
        else
        {
            RDLRefNode None(NoneText);
            AppendNodeClone(Text, None);
        }
        PrivateList->SetCellSpanToEnd(Detail, 1, false);
        PrivateList->InvalidateCell(Detail, 1);
    }
    // column 2 -- the rank.
    if (PrivateList->GetCell(Detail, 2, &Cell))
    {
        RenderDisplayList& Text = Cell->Source;
        RdlClear(Text);
        {
            JustifyNode Justify(TextJustifyCenter);
            AppendNodeClone(Text, Justify);
        }
        {
            StyleNode Style(DetailStyle);
            AppendNodeClone(Text, Style);
        }
        swprintf(Buffer, sizeof(Buffer) / sizeof(Buffer[0]), L"%u", Entry->Info->Rank);
        {
            TextNode Value(Buffer, -1);
            AppendNodeClone(Text, Value);
        }
        PrivateList->SetCellSpanToEnd(Detail, 2, false);
        PrivateList->InvalidateCell(Detail, 2);
    }
    // column 3 -- the system name (the the routine lookup inlined,
    // only while the the field cache exists).
    if (PrivateList->GetCell(Detail, 3, &Cell))
    {
        RenderDisplayList& Text = Cell->Source;
        RdlClear(Text);
        {
            JustifyNode Justify(TextJustifyCenter);
            AppendNodeClone(Text, Justify);
        }
        {
            StyleNode Style(DetailStyle);
            AppendNodeClone(Text, Style);
        }
        if (G_ChatNameCache70 != nullptr)
        {
            RDLRefNode System(*ChatGetSystemName(Entry->Info->Field_0x8));
            AppendNodeClone(Text, System);
        }
        PrivateList->SetCellSpanToEnd(Detail, 3, false);
        PrivateList->InvalidateCell(Detail, 3);
    }


    DetailRow->bHoverable = bEnabledByte;
    DetailRow->bSelectable = bEnabledByte;
    DetailRow->bRowLine = 0;
    DetailRow->GroupId = Row;
    static_cast<RowData*>(DetailRow->Payload.Object)->ChatId = CHAT_ID(Entry->Key);
}

void ChatControl::FillPrivate(int FirstItem, unsigned VisibleRows)
{

    if (Table == nullptr)
        return;

    // the local player's key {the field word, 0, 0}; the group size
    // read from the field +0x350 without a null test.
    unsigned const OwnKey = GetPlayerConnectionId() & 0xffffu;
    unsigned const GroupSize = static_cast<unsigned>(G_ChatGroupControl->GroupMembers.size());

    // baseline alloca(Names.size() * 12) and places the others from index GroupSize + 1, which can
    // run past that block when the group set holds ids the name map lacks; the port sizes the
    // buffer for the worst case and zero-fills it.
    std::vector<FChatControlEntry> Entries(Table->Names.size() + GroupSize + 1);
    for (size_t Index = 0; Index < Entries.size(); ++Index)
    {
        Entries[Index].Key = 0;
        Entries[Index].Name = nullptr;
        Entries[Index].Info = nullptr;
    }

    // own entry at [0], group members from [1], the rest after the
    // group block.
    int OwnCount = 0;
    int GroupCount = 0;
    int OtherCount = 0;
    for (FChatChannelTable::FNameMap::const_iterator It = Table->Names.begin();
         It != Table->Names.end(); ++It)
    {
        unsigned const Key = It->first;
        if (ChatKeyIsChannel(Key) || Key == G_ChatIdNone.Id)
            continue;
        FChatControlEntry* Entry = nullptr;
        if (Key == OwnKey)
        {
            Entry = &Entries[0];
            ++OwnCount;
        }
        else if (G_ChatGroupControl->GroupMembers.find(Key) != G_ChatGroupControl->GroupMembers.end())
        {
            Entry = &Entries[1 + static_cast<size_t>(GroupCount)];
            ++GroupCount;
        }
        else
        {
            Entry = &Entries[static_cast<size_t>(GroupSize) + static_cast<size_t>(OtherCount) + 1];
            ++OtherCount;
        }
        Entry->Key = Key;
        Entry->Name = It->second;
        Entry->Info = ChatControlInfoFor(Table, Key);
    }
    int const Total = OwnCount + GroupCount + OtherCount;

    // sort the group block and the rest separately.
    FChatCompareFn const Compare =
        SortAscending[SortColumn] != 0 ? kChatCompareAsc[SortColumn] : kChatCompareDesc[SortColumn];
    if (GroupCount != 0)
        qsort(&Entries[1], static_cast<size_t>(GroupCount), sizeof(FChatControlEntry), Compare);
    if (OtherCount != 0)
        qsort(&Entries[static_cast<size_t>(GroupCount) + 1], static_cast<size_t>(OtherCount),
              sizeof(FChatControlEntry), Compare);

    // the header row (no InvalidateCell in baseline).
    for (unsigned Column = 0; Column < 4; ++Column)
    {
        FUiFastTableCell* Cell = nullptr;
        if (!PrivateList->GetCell(0, Column, &Cell))
            continue;
        RenderDisplayList& Text = Cell->Source;
        RdlClear(Text);
        {
            JustifyNode Justify(TextJustifyCenter);
            AppendNodeClone(Text, Justify);
        }
        bool const bSorted = Column == SortColumn;
        {
            StyleNode Style(static_cast<unsigned short>(bSorted ? 0xda80 : 0x3df7));
            AppendNodeClone(Text, Style);
        }
        {
            RDLRefNode Header(Headers[Column]);
            AppendNodeClone(Text, Header);
        }
        if (bSorted)
        {
            ImageNode Pip(SortAscending[Column] != 0 ? DownPipImage : UpPipImage);
            AppendNodeClone(Text, Pip);
        }
    }


    {
        FUiFastTableRow* const HeaderRow = PrivateList->GetRow(0);
        HeaderRow->bHoverable = 0;
        HeaderRow->bSelectable = 0;
    }
    PrivateList->ClearSelection();

    // two table rows per entry from FirstItem / 2.
    unsigned Index = static_cast<unsigned>(FirstItem) >> 1;
    unsigned Row = 1;
    if (static_cast<int>(Index) < Total)
    {
        do
        {
            if (PrivateList->RowEdges.empty() || Row >= PrivateList->RowEdges.size())
                break;
            FChatControlEntry const* const Entry = &Entries[Index];
            RenderPrivateRow(Row, Entry, Entry->Key != OwnKey);
            ++Index;
            Row += 2;
        } while (static_cast<int>(Index) < Total);
    }

    // the rows left, the first of them keeping its row line.
    unsigned char bFirst = 1;
    for (; Row < VisibleRows; ++Row)
    {
        for (unsigned Column = 0; Column < 4; ++Column)
            PrivateList->ClearCell(Row, Column);
        FUiFastTableRow* const TableRow = PrivateList->GetRow(Row);
        TableRow->bHoverable = 0;
        TableRow->bSelectable = 0;
        TableRow->bRowLine = bFirst;
        bFirst = 0;
    }

    // "Private Channels (N)".
    PrivateTitle->ClearContent();
    {
        RDLRefNode Ref(PrivateTitleText);
        PrivateTitle->AppendNode(Ref);
    }
    wchar_t Buffer[64];
    swprintf(Buffer, sizeof(Buffer) / sizeof(Buffer[0]), L" (%d)", Total);   // u" (%d)" 0x005cfb88
    {
        TextNode Count(Buffer, -1);
        PrivateTitle->AppendNode(Count);
    }
}

bool ChatControl::Slot23(unsigned Msg, unsigned A, unsigned B)
{

    if (Msg == 0xef01)
    {
        // slot 5 with the parent's +0x60.
        if (Parent != nullptr)
            SetPosition(Parent->Position);
        ChatControlRefreshAll(this);
        Flags = static_cast<unsigned char>(Flags | 0x03);
        // slot 11, the routine, the routine -- no null test.
        HistoryDisplay->Update();
        HistoryDisplay->RebuildLayoutIfDirty();
        HistoryDisplay->ScrollToEnd();
        if (G_ChatMsgWin != nullptr)
            G_ChatMsgWin->Flags = static_cast<unsigned char>(G_ChatMsgWin->Flags & 0xfc);
        return true;
    }
    if (Msg == 0xef02)
    {

        if (Parent != nullptr)
            Win3DReparent(this, nullptr);
        Flags = static_cast<unsigned char>(Flags & 0xfc);
        if (G_bChatMsgWinWanted)
        {
            if (G_ChatMsgWin != nullptr)
                G_ChatMsgWin->Flags = static_cast<unsigned char>(G_ChatMsgWin->Flags | 0x03);
        }
        else if (G_ChatMsgWin != nullptr)
        {
            G_ChatMsgWin->Flags = static_cast<unsigned char>(G_ChatMsgWin->Flags & 0xfc);
        }
        return true;
    }

    UiScrollTable* const List = reinterpret_cast<UiScrollTable*>(static_cast<uintptr_t>(A));
    bool const bOwnList = List == BroadcastList || List == PrivateList;

    if (Msg == 0x300d && bOwnList)
    {
        // the selected row's target, or the system channel.
        CHAT_ID Target = G_ChatIdChannel1;
        unsigned Selected = 0;
        if (List->GetSelectedRow(&Selected))
        {
            UiFastTableRefCounter* const Payload = List->GetRow(Selected)->Payload.Object;
            if (Payload != nullptr)
                Target = static_cast<RowData*>(Payload)->ChatId;
        }
        if (Target.Id != G_ChatInputTarget.Id)
        {
            G_ChatInputTarget = Target;
            Win3D* const Found = Win3DFindByName("CTID");   // , name 0x005cf04c
            if (Found != nullptr)
            {
                // (&target) + the routine on the input line.
                ChatTextInputDisplay* const Line = static_cast<ChatTextInputDisplay*>(Found);
                Line->SetTarget(Target);
                Line->RebuildDisplay();
            }
            ChatControlRefreshAll(this);
        }
        return true;
    }
    if (Msg == 0xf001 && bOwnList)
    {
        // a header click on the private list -- same column flips the direction,
        // another column becomes the sort column.
        if (List != PrivateList)
            return true;
        unsigned const Column = B & 0xffffu;
        if ((B & 0xffff0000u) != 0 || Column >= 4)
            return true;
        if (Column == SortColumn)
            SortAscending[Column] = SortAscending[Column] == 0 ? 1 : 0;
        else
            SortColumn = Column;
        ChatControlRefreshAll(this);
        return true;
    }
    if (Msg == 0xf002 && bOwnList)
        return true;

    return WinControl::Slot23(Msg, A, B);
}

intptr_t ChatControl::SetProperty(int Msg, uintptr_t A, uintptr_t B)
{

    if (Msg == 0x26 && reinterpret_cast<WinControl*>(A) == BtnCCClose)
    {
        NnMainButtonOnModalOpen();
        return 1;
    }
    return WinControl::SetProperty(Msg, A, B);
}
