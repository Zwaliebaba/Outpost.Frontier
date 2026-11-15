// Author: Giperion.

#include "page_character_select.h"

#include "chat_init.h"         // ChatAppendRdlCopy, ChatGetServerMotd*, ChatGetShip/SystemName
#include "connect_flow.h"      // ConnectToServer (the routine)
#include "engine_light.h"      // ScaleVec3 (the routine)
#include "globals.h"           // g_display (the field), GetClientFrameTimeCapped
#include "ids_rdl.h"           // AppendTextAsRdl, LoadIdsIntoRdl, RdlClear, RdlRemoveLastParagraph
#include "intro_page_util.h"
#include "loading_screen.h"    // LoadingScreenSetClear / LoadingScreenStart (the routine / 290)
#include "login_mgr.h"         // G_LoginMgr (the field)
#include "menu_ui_init.h"      // PlayUiSoundSlot (the routine), PlayOrRewindUiSoundSlot (5645c0)
#include "nn_confirm.h"        // NnConfirmOpenFormatted (the routine)
#include "nn_error.h"          // NnErrorRunModal (the routine)
#include "reg_key_stack.h"     // FRegKeyStack
#include "server_connect.h"    // ShutdownServerConnection (the routine)
#include "tooltip_hover.h"     // FToolTipHoverInfo
#include "ui_fast_table.h"
#include "ui_rich_text_win.h"
#include "ui_scroll.h"
#include "ui_text_msg_button.h"
#include "win3d_immediate.h"   // Win3DVec3Add (the routine)

#include "common_text.h"

#include <stdint.h>            // uintptr_t
#include <stdlib.h>            // qsort
#include <string.h>            // memset
#include <wchar.h>             // _wcsicoll
#include <sstream>
#include <string>
#include <vector>

using namespace IntroPageUtil;

// .
CHARACTER_ID G_NoCharacterId;

// .
FCharacterCreateSelection G_CharacterCreateSelection;
// REP STOSD with ECX = 0x11 clears the field..the field, the index included.
static_assert(sizeof(FCharacterCreateSelection) == 0x44, "module state is 0x11 dwords");

namespace
{
    // .
    PageCharacterSelect G_PageCharacterSelect;

    char const kCharacterTableKey[] = "CharacterTable"; // s_ 0x005e29a4 (also the widget name)

    // the column captions (0x00567643).
    unsigned const kColumnHeaderIds[5] = {0xaf1, 0x6002f, 0xaf2, 0x6004a, 0x60049};
    // the header cells' tool tips (0x00567673).
    unsigned const kColumnHeaderTipIds[5] = {0xad2, 0x1b64, 0xad3, 0x1b65, 0x1b66};
    // the column edges Create writes when the registry has none (0x00567b70).
    float const kDefaultColumnEdges[5] = {0.0f, 0.228489995f, 0.316978991f, 0.413801998f,
                                          0.640625f};

    // -----------------------------------------------------------------------------------------
    // The ten qsort comparators (cdecl, on CharacterInfo* elements). Columns: 0 name, 1 rank
    // (+0x270), 2 money (+0x234), 3 ship (+0x230), 4 system (+0x260).

    CCharacterSet::CharacterInfo const* ElementOf(void const* Element)
    {
        return *static_cast<CCharacterSet::CharacterInfo* const*>(Element);
    }

    // tail call _wcsicoll(A name, B name).
    int __cdecl CompareNameUp(void const* A, void const* B)
    {
        return _wcsicoll(ElementOf(A)->Name.c_str(), ElementOf(B)->Name.c_str());
    }

    // A rank - B rank.
    int __cdecl CompareRankUp(void const* A, void const* B)
    {
        return static_cast<int>(ElementOf(A)->Rank - ElementOf(B)->Rank);
    }

    // A money - B money.
    int __cdecl CompareMoneyUp(void const* A, void const* B)
    {
        return ElementOf(A)->Money - ElementOf(B)->Money;
    }

    // lex_compare(ship name A, ship name B, false) (the routine).
    int __cdecl CompareShipUp(void const* A, void const* B)
    {
        RenderDisplayList const* const NameB = ChatGetShipArchName(ElementOf(B)->ShipArchetype);
        RenderDisplayList const* const NameA = ChatGetShipArchName(ElementOf(A)->ShipArchetype);
        return lex_compare(*NameA, *NameB, false);
    }

    // lex_compare(system name A, system name B, false) (the routine).
    int __cdecl CompareSystemUp(void const* A, void const* B)
    {
        RenderDisplayList const* const NameB = ChatGetSystemName(ElementOf(B)->SystemId);
        RenderDisplayList const* const NameA = ChatGetSystemName(ElementOf(A)->SystemId);
        return lex_compare(*NameA, *NameB, false);
    }

    // -_wcsicoll(A name, B name).
    int __cdecl CompareNameDown(void const* A, void const* B)
    {
        return -_wcsicoll(ElementOf(A)->Name.c_str(), ElementOf(B)->Name.c_str());
    }

    // B rank - A rank.
    int __cdecl CompareRankDown(void const* A, void const* B)
    {
        return static_cast<int>(ElementOf(B)->Rank - ElementOf(A)->Rank);
    }

    // B money - A money.
    int __cdecl CompareMoneyDown(void const* A, void const* B)
    {
        return ElementOf(B)->Money - ElementOf(A)->Money;
    }

    // -lex_compare(ship name A, ship name B, false).
    int __cdecl CompareShipDown(void const* A, void const* B)
    {
        RenderDisplayList const* const NameB = ChatGetShipArchName(ElementOf(B)->ShipArchetype);
        RenderDisplayList const* const NameA = ChatGetShipArchName(ElementOf(A)->ShipArchetype);
        return -lex_compare(*NameA, *NameB, false);
    }

    // -lex_compare(system name A, system name B, false).
    int __cdecl CompareSystemDown(void const* A, void const* B)
    {
        RenderDisplayList const* const NameB = ChatGetSystemName(ElementOf(B)->SystemId);
        RenderDisplayList const* const NameA = ChatGetSystemName(ElementOf(A)->SystemId);
        return -lex_compare(*NameA, *NameB, false);
    }

    typedef int(__cdecl* FCharacterCompare)(void const*, void const*);

    // used while bAscending[SortColumn] is set.
    FCharacterCompare const kAscendingCompare[5] = {CompareNameUp, CompareRankUp, CompareMoneyUp,
                                                    CompareShipUp, CompareSystemUp};
    // used while it is clear.
    FCharacterCompare const kDescendingCompare[5] = {CompareNameDown, CompareRankDown,
                                                     CompareMoneyDown, CompareShipDown,
                                                     CompareSystemDown};

    void AppendStyle(UiRichTextWin* Win, unsigned short Style)
    {
        StyleNode const Node(Style);
        Win->AppendNode(Node);
    }

    void AppendJustify(UiRichTextWin* Win, TextJustify Justify)
    {
        JustifyNode const Node(Justify);
        Win->AppendNode(Node);
    }

    void AppendRef(UiRichTextWin* Win, RenderDisplayList& Rdl)
    {
        RDLRefNode const Node(Rdl);
        Win->AppendNode(Node);
    }

    void AppendParagraph(UiRichTextWin* Win)
    {
        ParagraphNode const Node;
        Win->AppendNode(Node);
    }

    void SetButtonLive(UiTextMsgButton* Button, bool bLive)
    {
        if (bLive)
        {
            Button->Flags = static_cast<unsigned char>(Button->Flags | 1);
            Button->bInteractive = 1;
        }
        else
        {
            Button->Flags = static_cast<unsigned char>(Button->Flags & 0xfe);
            Button->bInteractive = 0;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// CharacterRowData

PageCharacterSelect::CharacterRowData::CharacterRowData(CHARACTER_ID const& Source)
{
    // count 1, the id's first byte 0 (its inline constructor), the
    // vftable, then CHARACTER_ID::operator=.
    Id = Source;
}

PageCharacterSelect::CharacterRowData::~CharacterRowData()
{
    // -> the routine: only the RefCounter vftable.
}

// ---------------------------------------------------------------------------------------------

PageCharacterSelect::PageCharacterSelect()
    : UIPage::MasterWin(Desc.GetPageData())
    , Result(0)
    , CharacterTable(nullptr)
    , CharacterScroll(nullptr)
    , LoadCharacter(nullptr)
    , NewCharacter(nullptr)
    , DeleteCharacter(nullptr)
    , Back(nullptr)
    , MainMenu(nullptr)
    , CharacterBox(nullptr)
    , MotdBox(nullptr)
    , ServerDetailsBox(nullptr)
    , Title(nullptr)
    , ServerDetails(nullptr)
    , Motd(nullptr)
    , MotdVersion(0)
    , Field_0x380(PackTextAttributes(0, 0, 0, 0, 0x52eaf5))
    , UpPip(nullptr)
    , DownPip(nullptr)
    , SortColumn(0)
    , bAscending{0, 0, 0, 0, 0}
    , bCharacterChosen(0)
    , FirstVisible(0)
    , CharacterBoxHome{0.0f, 0.0f, 0.0f}
    , MotdBoxHome{0.0f, 0.0f, 0.0f}
    , DetailsBoxHome{0.0f, 0.0f, 0.0f}
    , SlideT(0.0)
    , bLeaving(0)
    , LeaveDirection(0)
    , bLoadRequested(0)
    , bIntroBaseLeft(0)
{
    // the routine(the routine(+0x28c)), +0x8c's first byte 0, the routine on
    // +0x28c, +0x2b8..+0x2e8 = 0, the header array (the routine, 5 x 0xc), the seven lists
    // +0x32c..+0x374 (the routine), +0x380 = flags 0 / colour 0x52eaf5, +0x399's first byte 0,
    // the two edge vectors, the vftables, +0x5f8 = +0x5f9 = 0, +0x2b4 = +0x390 = 0,
    // +0x394..+0x398 = 0, +0x59c = +0x2ec = 0, then CHARACTER_ID::operator=(+0x8c,
    // ). The images, the homes and +0x5e8..+0x5f4 are not written; the static object
    // starts zeroed, which is what the initialisers above reproduce.
    CurrentCharacter = G_NoCharacterId;
}

PageCharacterSelect::~PageCharacterSelect()
{
    // vftables back, the edge vectors (the routine), the lists +0x374..+0x32c,
    // the header array (the routine), +0x2e8..+0x2b8 = 0, the routine on +0x28c, the routine
    // -- the member destructors in reverse declaration order, then the base.
}

bool PageCharacterSelect::SaveEdges()
{
    // HKCU (the stack's root), "...\\OutpostFrontier\\1.0" and "CharacterTable" created
    // with KEY_ALL_ACCESS (0x2001f), the edges written under the top key, the top popped and
    // closed, then every key left on the stack closed (HKCU included, as baseline).
    bool bSaved = false;
    FRegKeyStack Keys;
    if (Keys.Create(G_OutpostFrontierRegKey, 0x2001f) == nullptr)
        return false;
    if (Keys.Create(kCharacterTableKey, 0x2001f) == nullptr)
        return false;
    if (Edges.SaveToRegistry(&Keys))
        bSaved = true;
    Keys.PopClose();
    return bSaved;
}

void PageCharacterSelect::Destroy()
{
    // the routine(0xf) on the Event::Handler face.
    UnregisterEvent(0xf);
    if (CharacterTable != nullptr)
    {
        // the edges saved, the two arrows destroyed on the table's TRC.
        CharacterTable->GetEdges(&Edges);
        SaveEdges();
        CharacterTable->GetDeviceTRC()->destroy_image(UpPip);
        CharacterTable->GetDeviceTRC()->destroy_image(DownPip);
    }
    UIPage::MasterWin::Destroy();

    // .
    CharacterTable = nullptr;
    CharacterScroll = nullptr;
    LoadCharacter = nullptr;
    NewCharacter = nullptr;
    DeleteCharacter = nullptr;
    Back = nullptr;
    MainMenu = nullptr;
    CharacterBox = nullptr;
    MotdBox = nullptr;
    ServerDetailsBox = nullptr;
    Title = nullptr;
    ServerDetails = nullptr;
    Motd = nullptr;
}

bool PageCharacterSelect::Create(Win3D* NewParent, void* /*Payload*/)
{
    // . MasterWin::Create (the routine) with no payload; its result is not tested.
    UIPage::MasterWin::Create(NewParent, nullptr);
    Result = 0;

    // the widgets through the IUIPage face (slot 4).
    CharacterTable = static_cast<UiFastTable*>(FindWidget(kCharacterTableKey));
    CharacterScroll = static_cast<UiScroll*>(FindWidget("CharacterScroll"));
    LoadCharacter = static_cast<UiTextMsgButton*>(FindWidget("LoadCharacter"));
    NewCharacter = static_cast<UiTextMsgButton*>(FindWidget("NewCharacter"));
    DeleteCharacter = static_cast<UiTextMsgButton*>(FindWidget("DeleteCharacter"));
    Back = static_cast<UiTextMsgButton*>(FindWidget("Back"));
    MainMenu = static_cast<UiTextMsgButton*>(FindWidget("MainMenu"));
    CharacterBox = FindWidget("CharacterBox");
    MotdBox = FindWidget("MOTDBox");
    ServerDetailsBox = FindWidget("ServerDetailsBox");
    Title = static_cast<UiRichTextWin*>(FindWidget("Title"));
    ServerDetails = static_cast<UiRichTextWin*>(FindWidget("ServerDetails"));
    Motd = static_cast<UiRichTextWin*>(FindWidget("MOTD"));
    bCharacterChosen = 0;

    RegisterEvent(0xf); // the routine

    // one tip per cell, the table's tips on, the arrows on its TRC.
    CharacterTable->SetTipMode(3);           // (3)
    CharacterTable->SetToolTipEnabled(true); // (1)
    UpPip = CharacterTable->GetDeviceTRC()->create_image("interface\\intro\\images\\uppip.bmp");
    DownPip =
        CharacterTable->GetDeviceTRC()->create_image("interface\\intro\\images\\DownPip.bmp");

    // the five captions (the trailing paragraph cut without testing the
    // load), each column ascending, the header cells' tips.
    for (unsigned Column = 0; Column < 5; ++Column)
    {
        RdlClear(ColumnHeaders[Column]);
        LoadIdsIntoRdl(kColumnHeaderIds[Column], ColumnHeaders[Column]);
        RdlRemoveLastParagraph(ColumnHeaders[Column]);
        bAscending[Column] = 1;
        FToolTipHoverInfo* const Tip = CharacterTable->GetCellTip(0, Column);
        if (Tip != nullptr)
            Tip->IdsAlt = kColumnHeaderTipIds[Column];
    }

    // the lists, in baseline's order.
    LoadIds(Text729, 0x729);
    LoadIds(DetailsHeader, 0x744);
    LoadIds(DetailsPassword, 0x750);
    LoadIds(DetailsField95, 0x74f);
    LoadIdsCut(TitleText, 0x6004e);
    LoadIdsCut(MotdHeader, 0x60007);
    LoadIdsCut(DetailsRestricted, 0x6007c);

    // the table's look.
    CharacterTable->SetFrameColor(ChromeColor());          // (&the field)
    CharacterTable->SetGrabDistance(0.00400000019f);       // (0x3b83126f)
    CharacterTable->SetResizable(false, true);             // (0, 1)
    CharacterTable->SetLayoutFlag(true);                   // (1)
    CharacterTable->SetHeaderRows(1);                      // (1)
    CharacterTable->SetBorders(false, false, false, true); // (0, 0, 0, 1)
    CharacterTable->SetRowHighlights(false);               // (0)

    // the scroll bar -- chrome colours, range 0..rows - 1, page
    // rows - 1 (both through FILD of a zero-extended dword), step 1, value 0.
    CharacterScroll->SetArrowColors(ChromeColor(), ChromeColor());
    CharacterScroll->SetBarColors(ChromeColor(), ChromeColor());
    CharacterScroll->SetRange(
        0.0f,
        static_cast<float>(static_cast<unsigned>(EdgeCount(CharacterTable->RowEdges) - 1)));
    CharacterScroll->SetPage(
        static_cast<float>(static_cast<unsigned>(EdgeCount(CharacterTable->RowEdges) - 1)));
    CharacterScroll->SetStep(1.0f);
    CharacterScroll->SetValue(0.0f);
    ServerDetails->SetAutoScrollbar(true); // (1)
    Motd->SetAutoScrollbar(true);

    // the message of the day, the routine's body inline.
    MotdVersion = 0;
    RefreshMotd();

    // the saved edges from HKCU\...\CharacterTable (KEY_READ); on
    // success two keys popped and closed, on failure one, then the rest closed.
    bool bLoaded = false;
    {
        FRegKeyStack Keys;
        if (Keys.Open(G_OutpostFrontierRegKey, 0x20019) != nullptr &&
            Keys.Open(kCharacterTableKey, 0x20019) != nullptr)
        {
            if (Edges.LoadFromRegistry(&Keys))
                bLoaded = true;
            Keys.PopClose();
        }
        Keys.PopClose();
    }
    // nothing saved -> the table's own edges with the default column
    // edges written over the first five (no size check, as baseline).
    if (!bLoaded)
    {
        CharacterTable->GetEdges(&Edges);
        float* const Cols = Edges.Cols.data();
        for (int Index = 0; Index < 5; ++Index)
            Cols[Index] = kDefaultColumnEdges[Index];
    }
    CharacterTable->SetEdges(&Edges);

    // the title.
    Title->ClearContent();
    AppendStyle(Title, 0x1df5);
    AppendRef(Title, TitleText);

    SelectedCharacter = G_NoCharacterId;

    // with five column edges the header row gets its captions and
    // stops hovering.
    if (CharacterTable->ColEdges.size() == 5)
    {
        for (unsigned Column = 0; Column < 5; ++Column)
            SetCell(0, Column, &ColumnHeaders[Column], false, false);
        CharacterTable->GetRow(0)->bHoverable = 0;
    }

    // the server's details, from a copy of the current server
    // (the routine) that lives to the end of the block.
    {
        FServerInfo const Server = G_LoginMgr.GetCurrentServer();
        ServerDetails->ClearContent();
        AppendJustify(ServerDetails, TextJustifyCenter);
        AppendStyle(ServerDetails, 0x4433);
        AppendRef(ServerDetails, Text729);
        AppendParagraph(ServerDetails);
        AppendJustify(ServerDetails, TextJustifyLeft);
        AppendStyle(ServerDetails, 0x76ec);
        {
            TextNode const Name(Server.Name.c_str(), -1);
            ServerDetails->AppendNode(Name);
        }
        AppendParagraph(ServerDetails);
        AppendJustify(ServerDetails, TextJustifyCenter);
        AppendStyle(ServerDetails, 0x4433);
        AppendRef(ServerDetails, DetailsHeader);
        AppendParagraph(ServerDetails);
        AppendJustify(ServerDetails, TextJustifyLeft);
        AppendStyle(ServerDetails, 0x76ec);
        RenderDisplayList Description;
        AppendTextAsRdl(Server.Field_0x54.c_str(), static_cast<unsigned>(Server.Field_0x54.size()),
                        Description, false);
        ServerDetails->AppendContent(Description);
        AppendParagraph(ServerDetails);

        // the restrictions -- shown unless +0x95 is set and there is no
        // password. Here the style comes before the left justification (0x005680b8 / 0x005680e5).
        if (Server.Field_0x95 == 0 || Server.bPassword != 0)
        {
            AppendJustify(ServerDetails, TextJustifyCenter);
            AppendStyle(ServerDetails, 0x4433);
            AppendRef(ServerDetails, DetailsRestricted);
            AppendParagraph(ServerDetails);
            AppendStyle(ServerDetails, 0x76ec);
            AppendJustify(ServerDetails, TextJustifyLeft);
            if (Server.Field_0x95 == 0)
            {
                AppendRef(ServerDetails, DetailsField95);
                AppendParagraph(ServerDetails);
            }
            if (Server.bPassword != 0)
            {
                AppendRef(ServerDetails, DetailsPassword);
                AppendParagraph(ServerDetails);
            }
        }
    }

    // the faction records (CLoginMgr +0x1d8) rebuilt (the routine).
    G_LoginMgr.ReloadFactions();
    // the roster refreshed (the routine), the table on success.
    if (G_LoginMgr.RequestCharacterInfo() == 1)
        RebuildTable();

    // the three character buttons shown, then 0x005682d7..0x0056838e the
    // logic inline.
    NewCharacter->Flags = static_cast<unsigned char>(NewCharacter->Flags | 3);
    LoadCharacter->Flags = static_cast<unsigned char>(LoadCharacter->Flags | 3);
    DeleteCharacter->Flags = static_cast<unsigned char>(DeleteCharacter->Flags | 3);
    UpdateButtons();

    // the slide starts at -1.0, no leave, direction -1.
    SlideT = -1.0;
    bLeaving = 0;
    LeaveDirection = -1;

    // the five buttons skip their upload; 0x005683f0..0x00568421 the
    // text windows, the table and the scroll bar hidden.
    LoadCharacter->SetSkipUpload(true); // (1)
    NewCharacter->SetSkipUpload(true);
    DeleteCharacter->SetSkipUpload(true);
    Back->SetSkipUpload(true);
    MainMenu->SetSkipUpload(true);
    HideWin3D(ServerDetails);
    HideWin3D(Motd);
    HideWin3D(Title);
    HideWin3D(CharacterTable);
    HideWin3D(CharacterScroll);

    // the three boxes' positions (+0x60) as their homes.
    for (int Axis = 0; Axis < 3; ++Axis)
    {
        CharacterBoxHome[Axis] = CharacterBox->Position[Axis];
        MotdBoxHome[Axis] = MotdBox->Position[Axis];
        DetailsBoxHome[Axis] = ServerDetailsBox->Position[Axis];
    }
    return true;
}

void PageCharacterSelect::SetCell(unsigned Row, unsigned Column, RenderDisplayList const* Content,
                                  bool bEnabled, bool bSelected)
{
    // . the routine; false -> nothing.
    FUiFastTableCell* Cell = nullptr;
    if (!CharacterTable->GetCell(Row, Column, &Cell))
        return;
    RdlClear(Cell->Source); // over the cell's list

    bool bSortArrow = false;
    if (Row == 0)
    {
        // the header -- the sort column's style (and its arrow below),
        // centred.
        if (Column == SortColumn)
        {
            StyleNode const Style(0xd190);
            AppendNodeClone(Cell->Source, Style);
            bSortArrow = true;
        }
        else
        {
            StyleNode const Style(0xd824);
            AppendNodeClone(Cell->Source, Style);
        }
        JustifyNode const Justify(TextJustifyCenter);
        AppendNodeClone(Cell->Source, Justify);
    }
    else
    {
        // grey when not enabled, else selected / normal; column 0 left,
        // the rest centred.
        unsigned short const StyleId =
            !bEnabled ? static_cast<unsigned short>(0xbb80)
                      : (bSelected ? static_cast<unsigned short>(0x3343)
                                   : static_cast<unsigned short>(0x69f5));
        {
            StyleNode const Style(StyleId);
            AppendNodeClone(Cell->Source, Style);
        }
        JustifyNode const Justify(Column == 0 ? TextJustifyLeft : TextJustifyCenter);
        AppendNodeClone(Cell->Source, Justify);
    }

    // a clone of every Content node (the routine per node).
    ChatAppendRdlCopy(Cell->Source, *Content);

    // the arrow -- DownPip ascending, UpPip descending.
    if (bSortArrow)
    {
        ImageNode const Arrow(bAscending[Column] != 0 ? DownPip : UpPip);
        AppendNodeClone(Cell->Source, Arrow);
    }
    CharacterTable->InvalidateCell(Row, Column);
}

void PageCharacterSelect::RebuildTable()
{
    // .
    CharacterTable->ClearSelection();

    // the scroll range over the roster, a page of rows - 1;
    // FirstVisible from the scroll value; the bar shown only when the roster does not fit.
    int const CharacterCount = static_cast<int>(G_LoginMgr.Characters.count()); // inline walk
    unsigned const RowCount = static_cast<unsigned>(EdgeCount(CharacterTable->RowEdges));
    CharacterScroll->SetRange(0.0f, static_cast<float>(CharacterCount));
    int const VisibleRows = static_cast<int>(RowCount) - 1;
    CharacterScroll->SetPage(static_cast<float>(VisibleRows));
    CharacterScroll->SetStep(1.0f);
    FirstVisible = static_cast<int>(CharacterScroll->GetValue()); // ftol
    if (CharacterCount > VisibleRows)
        ShowWin3D(CharacterScroll);
    else
        HideWin3D(CharacterScroll);

    // the roster into an _alloca'd array (the routine per index; the
    // operator== against +0x399 after each fetch has no effect).
    std::vector<CCharacterSet::CharacterInfo*> Characters(static_cast<size_t>(CharacterCount));
    for (int Index = 0; Index < CharacterCount; ++Index)
        Characters[static_cast<size_t>(Index)] = G_LoginMgr.CharacterAt(Index);

    // sorted by the sort column, or no roster -> nothing selected.
    if (CharacterCount != 0)
    {
        FCharacterCompare const Compare = bAscending[SortColumn] != 0
                                              ? kAscendingCompare[SortColumn]
                                              : kDescendingCompare[SortColumn];
        qsort(Characters.data(), static_cast<size_t>(CharacterCount),
              sizeof(CCharacterSet::CharacterInfo*), Compare); // element size 4
    }
    else
    {
        SelectedCharacter = G_NoCharacterId;
    }

    // the visible characters from row 1; the selected one selected.
    unsigned Row = 1;
    for (int Index = FirstVisible; Index < CharacterCount; ++Index, ++Row)
    {
        if (Row >= RowCount)
            return;
        CCharacterSet::CharacterInfo* const Character = Characters[static_cast<size_t>(Index)];
        if (Character != nullptr)
        {
            FillRow(Row, Character);
            if (Character->Id == SelectedCharacter)
                CharacterTable->SelectRow(Row);
        }
    }

    // the rows left over emptied (their payload released inline).
    for (; Row < RowCount; ++Row)
    {
        for (unsigned Column = 0; Column < CharacterTable->ColEdges.size(); ++Column)
            CharacterTable->ClearCell(Row, Column);
        FUiFastTableRow* const RowRecord = CharacterTable->GetRow(Row);
        if (RowRecord != nullptr && RowRecord->Payload.Object != nullptr)
            RowRecord->Payload.Reset(); // , then +8 = 0
        CharacterTable->GetRow(Row)->bSelectable = 0;
        CharacterTable->GetRow(Row)->bHoverable = 0;
    }
}

void PageCharacterSelect::FillRow(unsigned Row, CCharacterSet::CharacterInfo const* Character)
{
    // . Every cell enabled; selected when it is +0x399.
    bool const bSelected = Character->Id == SelectedCharacter;

    // column 0, the name.
    {
        RenderDisplayList Text;
        TextNode const Node(Character->Name.c_str(), -1);
        AppendNodeClone(Text, Node);
        SetCell(Row, 0, &Text, true, bSelected);
    }

    // column 2, the money through a wide stringstream
    // (operator<<(int), ends).
    {
        std::wstringstream Stream;
        Stream << Character->Money << std::ends;
        std::wstring const Digits = Stream.str();
        RenderDisplayList Text;
        TextNode const Node(Digits.c_str(), -1);
        AppendNodeClone(Text, Node);
        SetCell(Row, 2, &Text, true, bSelected);
    }

    // column 1, the rank (operator<<(unsigned)).
    {
        std::wstringstream Stream;
        Stream << Character->Rank << std::ends;
        std::wstring const Digits = Stream.str();
        RenderDisplayList Text;
        TextNode const Node(Digits.c_str(), -1);
        AppendNodeClone(Text, Node);
        SetCell(Row, 1, &Text, true, bSelected);
    }

    // column 3 the ship's name, column 4 the system's.
    SetCell(Row, 3, ChatGetShipArchName(Character->ShipArchetype), true, bSelected);
    SetCell(Row, 4, ChatGetSystemName(Character->SystemId), true, bSelected);

    // the row live, the payload a CharacterRowData with the id
    // (the routine), the construction reference dropped (the routine).
    FUiFastTableRow* const RowRecord = CharacterTable->GetRow(Row);
    RowRecord->bSelectable = 1;
    RowRecord->bHoverable = 1;
    CharacterRowData* const Data = new CharacterRowData(Character->Id);
    RowRecord->Payload.Assign(Data);
    Data->Release();
}

bool PageCharacterSelect::Slot23(unsigned Msg, unsigned A, unsigned B)
{
    // .
    if (Msg == 0x3004)
    {
        PlayOrRewindUiSoundSlot(0); // (0)
        int Direction = 0;
        switch (A)
        {
        case 0x3e9:
            Direction = -1;
            break;
        case 0x3ea:
            Direction = -2;
            break;
        case 0x3001:
            Direction = 2;
            break;
        case 0x3003:
            Direction = 1;
            break;
        case 0x3002:
        {
            // delete -- the confirm box with the selected character's
            // name (the routine on the row's id).
            unsigned SelectedRow = A;
            if (CharacterTable->GetSelectedRow(&SelectedRow))
            {
                FUiFastTableRow* const RowRecord = CharacterTable->GetRow(SelectedRow);
                if (RowRecord->Payload.Object != nullptr)
                {
                    CharacterRowData const* const Data =
                        static_cast<CharacterRowData const*>(RowRecord->Payload.Object);
                    CCharacterSet::CharacterInfo const* const Character =
                        G_LoginMgr.FindCharacterById(Data->Id);
                    NnConfirmOpenFormatted(this, Character->Name.c_str(), nullptr);
                }
            }
            return true;
        }
        default:
            return false;
        }

        // The leave sequence each of the four spells inline (0x005696bd..0x00569797 etc.):
        // 's body with the routine for the tips, then the direction.
        bLeaving = 1;
        SlideT = 0.0;
        LoadCharacter->bInteractive = 0;
        NewCharacter->bInteractive = 0;
        DeleteCharacter->bInteractive = 0;
        Back->bInteractive = 0;
        MainMenu->bInteractive = 0;
        LoadCharacter->SetSkipUpload(true);
        NewCharacter->SetSkipUpload(true);
        DeleteCharacter->SetSkipUpload(true);
        Back->SetSkipUpload(true);
        MainMenu->SetSkipUpload(true);
        HideWin3D(ServerDetails);
        HideWin3D(Motd);
        HideWin3D(Title);
        HideWin3D(CharacterTable);
        HideWin3D(CharacterScroll);
        PlayUiSoundSlot(0x1b);
        DisableToolTips();
        LeaveDirection = Direction;
        return true;
    }

    if (Msg == 0x300d)
    {
        // a click on the table -- the selection moves to the clicked
        // row's character; the new and the old row redrawn.
        if (static_cast<uintptr_t>(A) != reinterpret_cast<uintptr_t>(CharacterTable))
            return false;
        CHARACTER_ID const OldSelection = SelectedCharacter; // REP MOVSD, 0x80 dwords
        SelectedCharacter = G_NoCharacterId;
        unsigned ClickedRow = 0;
        if (CharacterTable->GetSelectedRow(&ClickedRow))
        {
            FUiFastTableRow* const RowRecord = CharacterTable->GetRow(ClickedRow);
            if (RowRecord->Payload.Object != nullptr)
            {
                SelectedCharacter =
                    static_cast<CharacterRowData const*>(RowRecord->Payload.Object)->Id;

                // the new and the old character in the roster (the
                // walk stops once both are found).
                CCharacterSet::CharacterInfo* NewOne = nullptr;
                CCharacterSet::CharacterInfo* OldOne = nullptr;
                int const Count = static_cast<int>(G_LoginMgr.Characters.count());
                for (int Index = 0; Index < Count; ++Index)
                {
                    if (NewOne != nullptr && OldOne != nullptr)
                        break;
                    CCharacterSet::CharacterInfo* const Character = G_LoginMgr.CharacterAt(Index);
                    if (Character->Id == SelectedCharacter)
                        NewOne = Character;
                    if (Character->Id == OldSelection)
                        OldOne = Character;
                }
                if (NewOne != nullptr)
                    FillRow(ClickedRow, NewOne);
                // the old one's row (the first row from 1 whose payload
                // carries its id) redrawn unselected.
                if (OldOne != nullptr)
                {
                    for (unsigned Row = 1; Row < CharacterTable->RowEdges.size(); ++Row)
                    {
                        FUiFastTableRow* const Other = CharacterTable->GetRow(Row);
                        if (Other->Payload.Object != nullptr &&
                            static_cast<CharacterRowData const*>(Other->Payload.Object)->Id ==
                                OldSelection)
                        {
                            FillRow(Row, OldOne);
                            break;
                        }
                    }
                }
            }
        }
        UpdateButtons(); // , the routine's body inline
        return true;
    }

    if (Msg == 0x3006)
    {
        // the delete confirm's answer.
        if (A == 0)
        {
            Slot23(0x3004, 0xf003, 0); // slot 23 on itself (0x00569d96)
            return true;
        }
        if (A != 1)
            return true;
        unsigned SelectedRow = A;
        CharacterTable->GetSelectedRow(&SelectedRow); // result not tested (0x00569dbd)
        CHARACTER_ID const Victim =
            static_cast<CharacterRowData const*>(CharacterTable->GetRow(SelectedRow)->Payload.Object)
                ->Id;
        int const Destroyed = G_LoginMgr.DestroyCharacter(Victim);
        if (Destroyed == 1)
        {
            CharacterTable->ClearSelection();
            SelectedCharacter = G_NoCharacterId;
        }
        else if (Destroyed == 2)
        {
            Result = 5;
        }
        else
        {
            NnErrorRunModal(0x73b, false, nullptr);
            G_LoginMgr.ReloadFactions(); // the routine
            int const Relisted = G_LoginMgr.RequestCharacterInfo();
            if (Relisted != 1)
            {
                if (Relisted != 2)
                    NnErrorRunModal(0x73c, false, nullptr);
                Result = 5;
            }
        }
        RebuildTable();
        UpdateButtons();
        return true;
    }

    if (Msg == 0xf001)
    {
        // a header click -- the sort column or its direction, the
        // headers redrawn, the table rebuilt (the routine's body inline, the leftover rows
        // through the routine).
        if (static_cast<uintptr_t>(A) != reinterpret_cast<uintptr_t>(CharacterTable))
            return false;
        unsigned const Column = B & 0xffffu;
        if ((B & 0xffff0000u) != 0 || Column >= 5)
            return false;
        if (Column == SortColumn)
            bAscending[Column] = static_cast<unsigned char>(bAscending[Column] == 0);
        else
            SortColumn = Column;
        for (unsigned Header = 0; Header < 5; ++Header)
            SetCell(0, Header, &ColumnHeaders[Header], false, false);
        CharacterTable->GetRow(0)->bHoverable = 0;
        CharacterTable->ClearSelection();

        // the scroll bar over the roster (the routine / the routine).
        int const CharacterCount = static_cast<int>(G_LoginMgr.Characters.count());
        unsigned const RowCount = static_cast<unsigned>(EdgeCount(CharacterTable->RowEdges));
        CharacterScroll->SetRange(0.0f, static_cast<float>(CharacterCount));
        int const VisibleRows = static_cast<int>(RowCount) - 1;
        CharacterScroll->SetPage(static_cast<float>(VisibleRows));
        CharacterScroll->SetStep(1.0f);
        FirstVisible = static_cast<int>(CharacterScroll->GetValue()); // ftol
        if (CharacterCount > VisibleRows)
            ShowWin3D(CharacterScroll);
        else
            HideWin3D(CharacterScroll);

        // the roster gathered and sorted, or nothing selected.
        std::vector<CCharacterSet::CharacterInfo*> Characters(static_cast<size_t>(CharacterCount));
        for (int Index = 0; Index < CharacterCount; ++Index)
            Characters[static_cast<size_t>(Index)] = G_LoginMgr.CharacterAt(Index);
        if (CharacterCount != 0)
        {
            FCharacterCompare const Compare = bAscending[SortColumn] != 0
                                                  ? kAscendingCompare[SortColumn]
                                                  : kDescendingCompare[SortColumn];
            qsort(Characters.data(), static_cast<size_t>(CharacterCount),
                  sizeof(CCharacterSet::CharacterInfo*), Compare);
        }
        else
        {
            SelectedCharacter = G_NoCharacterId;
        }

        // the visible rows from 1.
        unsigned Row = 1;
        for (int Index = FirstVisible; Index < CharacterCount; ++Index, ++Row)
        {
            if (Row >= RowCount)
                return true;
            CCharacterSet::CharacterInfo* const Character = Characters[static_cast<size_t>(Index)];
            if (Character != nullptr)
            {
                FillRow(Row, Character);
                if (Character->Id == SelectedCharacter)
                    CharacterTable->SelectRow(Row);
            }
        }
        // the rest through the routine, then +1 and +0 cleared.
        for (; Row < RowCount; ++Row)
        {
            ClearRow(Row);
            CharacterTable->GetRow(Row)->bSelectable = 0;
            CharacterTable->GetRow(Row)->bHoverable = 0;
        }
        return true;
    }

    if (Msg == 0xf002)
    {
        // a double click on the table loads.
        if (static_cast<uintptr_t>(A) != reinterpret_cast<uintptr_t>(CharacterTable))
            return false;
        BeginLeave();
        LeaveDirection = 1;
        return true;
    }

    if (Msg == 0xf005)
    {
        RebuildTable();
        return true;
    }
    return false;
}

void PageCharacterSelect::DisableToolTips()
{
    // .
    LoadCharacter->SetToolTipEnabled(false);
    NewCharacter->SetToolTipEnabled(false);
    DeleteCharacter->SetToolTipEnabled(false);
    Back->SetToolTipEnabled(false);
    MainMenu->SetToolTipEnabled(false);
    ServerDetails->SetToolTipEnabled(false);
    Motd->SetToolTipEnabled(false);
}

void PageCharacterSelect::ClearRow(unsigned Row)
{
    // every cell (the column count read again each pass), then the payload
    // released inline (the routine's body) and cleared.
    for (unsigned Column = 0; Column < CharacterTable->ColEdges.size(); ++Column)
        CharacterTable->ClearCell(Row, Column);
    FUiFastTableRow* const RowRecord = CharacterTable->GetRow(Row);
    if (RowRecord != nullptr && RowRecord->Payload.Object != nullptr)
        RowRecord->Payload.Reset();
}

void PageCharacterSelect::UpdateButtons()
{
    // .
    bool const bNone = SelectedCharacter == G_NoCharacterId;
    SetButtonLive(LoadCharacter, !bNone);
    SetButtonLive(DeleteCharacter, !bNone);
    SetButtonLive(NewCharacter, G_LoginMgr.Characters.count() < 5);
}

bool PageCharacterSelect::OnEvent(unsigned MsgId)
{
    // (this = the +0x80 face): event 0xf reconnects unless a leave runs; anything
    // else is the routine (false).
    if (MsgId != 0xf)
        return UIPage::MasterWin::OnEvent(MsgId);
    if (bLeaving == 0)
    {
        ShutdownServerConnection();
        if (!ConnectToServer(g_display.ini_path.Text)) // (the field)
        {
            G_LoginMgr.Reset();
            Result = 4;         // , overwritten by the next store
        }
        Result = 5;
    }
    return true;
}

void PageCharacterSelect::Update()
{
    // . The message of the day, then Win3D's child walk (next read before each
    // call, the routine inlined).
    RefreshMotd();
    Win3D::Update();

    // FCOMP 1.0, TEST AH,5 / JP -- below 1.0 and not leaving slides in;
    // leaving (at any value) slides out.
    bool const bBelowOne = SlideT < 1.0;
    if (bBelowOne && bLeaving == 0)
    {
        // the frame time capped at 0.5 (FCOM 0.5, TEST AH,0x41 / JNZ
        // keeps it when not greater), 1.3 per second.
        double Dt = GetClientFrameTimeCapped();
        if (Dt > 0.5)
            Dt = 0.5;
        SlideT = Dt * 1.3 + SlideT;

        // FCOM 0.0, TEST AH,5 / JP (0x0056a3dc..0x0056a3ed): below 0 the boxes follow SlideT.
        if (SlideT < 0.0)
        {
            float const Along[3] = {static_cast<float>(SlideT), 0.0f, 0.0f};
            float const Against[3] = {static_cast<float>(-SlideT), 0.0f, 0.0f};
            float const Up[3] = {0.0f, static_cast<float>(SlideT), 0.0f};
            LoadCharacter->SetProperty(6, reinterpret_cast<uintptr_t>(Along), 0);
            NewCharacter->SetProperty(6, reinterpret_cast<uintptr_t>(Along), 0);
            DeleteCharacter->SetProperty(6, reinterpret_cast<uintptr_t>(Along), 0);
            Back->SetProperty(6, reinterpret_cast<uintptr_t>(Along), 0);
            MainMenu->SetProperty(6, reinterpret_cast<uintptr_t>(Along), 0);
            // CharacterBox at home + 1.2 * Against, MOTDBox at home +
            // Up, ServerDetailsBox at home + 1.2 * Along (the routine / the routine, slot 5).
            float Scaled[3];
            float Position[3];
            ScaleVec3(Scaled, Against, 1.20000005f);
            Win3DVec3Add(Position, CharacterBoxHome, Scaled);
            CharacterBox->SetPosition(Position);
            Win3DVec3Add(Position, MotdBoxHome, Up);
            MotdBox->SetPosition(Position);
            ScaleVec3(Scaled, Along, 1.20000005f);
            Win3DVec3Add(Position, DetailsBoxHome, Scaled);
            ServerDetailsBox->SetPosition(Position);
        }
        else if (SlideT > 0.0)
        {
            // (FCOMP 0.0, TEST AH,0x41 / JNZ -- past 0 only): everything
            // in place, shown and live, the tips back; SlideT parked at 2.0.
            float const ZeroOffset[3] = {0.0f, 0.0f, 0.0f};
            LoadCharacter->SetProperty(6, reinterpret_cast<uintptr_t>(ZeroOffset), 0);
            NewCharacter->SetProperty(6, reinterpret_cast<uintptr_t>(ZeroOffset), 0);
            DeleteCharacter->SetProperty(6, reinterpret_cast<uintptr_t>(ZeroOffset), 0);
            Back->SetProperty(6, reinterpret_cast<uintptr_t>(ZeroOffset), 0);
            MainMenu->SetProperty(6, reinterpret_cast<uintptr_t>(ZeroOffset), 0);
            CharacterBox->SetPosition(CharacterBoxHome);
            MotdBox->SetPosition(MotdBoxHome);
            ServerDetailsBox->SetPosition(DetailsBoxHome);
            LoadCharacter->SetSkipUpload(false); // (0)
            NewCharacter->SetSkipUpload(false);
            DeleteCharacter->SetSkipUpload(false);
            Back->SetSkipUpload(false);
            MainMenu->SetSkipUpload(false);
            ShowWin3D(ServerDetails);
            ShowWin3D(Motd);
            ShowWin3D(Title);
            ShowWin3D(CharacterTable);
            // the bar only when the roster does not fit.
            int const Count = static_cast<int>(G_LoginMgr.Characters.count());
            if (Count > EdgeCount(CharacterTable->RowEdges) - 1)
                ShowWin3D(CharacterScroll);
            else
                HideWin3D(CharacterScroll);
            LoadCharacter->SetToolTipEnabled(true); // (1), slot 46 +4
            LoadCharacter->GetToolTipHoverInfo()->IdsAlt = 0xad5;
            NewCharacter->SetToolTipEnabled(true);
            NewCharacter->GetToolTipHoverInfo()->IdsAlt = 0xad6;
            DeleteCharacter->SetToolTipEnabled(true);
            DeleteCharacter->GetToolTipHoverInfo()->IdsAlt = 0xad7;
            Back->SetToolTipEnabled(true);
            Back->GetToolTipHoverInfo()->IdsAlt = 0xad4;
            MainMenu->SetToolTipEnabled(true);
            MainMenu->GetToolTipHoverInfo()->IdsAlt = 0xac6;
            ServerDetails->SetToolTipEnabled(true);
            ServerDetails->GetToolTipHoverInfo()->IdsAlt = 0xac8;
            Motd->SetToolTipEnabled(true);
            Motd->GetToolTipHoverInfo()->IdsAlt = 0xae4;
            SlideT = 2.0;
        }

        // the slide sound while -0.5 < SlideT < -0.1 (FCOMP -0.5,
        // TEST AH,0x41 / JNZ; FCOMP -0.1, TEST AH,5 / JP).
        if (SlideT > -0.5 && SlideT < -0.1)
            PlayUiSoundSlot(0x1b); // (0x1b)
    }
    else if (bLeaving != 0)
    {
        // SlideT runs down at 1.5 per second (no cap).
        SlideT = SlideT - GetClientFrameTimeCapped() * 1.5;
        // FCOM -0.9, TEST AH,0x41 / JNZ: at or below -0.9 (or unordered) the leave ends.
        if (SlideT > -0.9)
        {
            float const Along[3] = {static_cast<float>(SlideT), 0.0f, 0.0f};
            float const Negated = static_cast<float>(-SlideT);
            LoadCharacter->SetProperty(6, reinterpret_cast<uintptr_t>(Along), 0);
            NewCharacter->SetProperty(6, reinterpret_cast<uintptr_t>(Along), 0);
            DeleteCharacter->SetProperty(6, reinterpret_cast<uintptr_t>(Along), 0);
            Back->SetProperty(6, reinterpret_cast<uintptr_t>(Along), 0);
            MainMenu->SetProperty(6, reinterpret_cast<uintptr_t>(Along), 0);
            // the same three positions built with the routine (float
            // sums): CharacterBox home + {-1.2 SlideT, 0, 0}, MOTDBox home + {0, SlideT, 0},
            // ServerDetailsBox home + 1.2 * Along.
            float const Out[3] = {Negated * 1.20000005f, 0.0f, 0.0f};
            float Position[3] = {Out[0] + CharacterBoxHome[0], Out[1] + CharacterBoxHome[1],
                                 Out[2] + CharacterBoxHome[2]};
            CharacterBox->SetPosition(Position);
            Position[0] = MotdBoxHome[0];
            Position[1] = Along[0] + MotdBoxHome[1];
            Position[2] = MotdBoxHome[2];
            MotdBox->SetPosition(Position);
            float const Right[3] = {Along[0] * 1.20000005f, Along[1] * 1.20000005f,
                                    Along[2] * 1.20000005f};
            Position[0] = Right[0] + DetailsBoxHome[0];
            Position[1] = Right[1] + DetailsBoxHome[1];
            Position[2] = Right[2] + DetailsBoxHome[2];
            ServerDetailsBox->SetPosition(Position);
        }
        else
        {
            // everything hidden, then the direction decides.
            HideWin3D(LoadCharacter);
            HideWin3D(NewCharacter);
            HideWin3D(DeleteCharacter);
            HideWin3D(Back);
            HideWin3D(MainMenu);
            HideWin3D(ServerDetails);
            HideWin3D(Motd);
            HideWin3D(Title);
            HideWin3D(CharacterBox);
            HideWin3D(MotdBox);
            HideWin3D(ServerDetailsBox);
            bLeaving = 0;
            if (LeaveDirection == 1)
            {
                bLoadRequested = 1;
            }
            else if (LeaveDirection == 2)
            {
                // the character-create pick the field -- 0x11 dwords
                // zeroed (REP STOSD), the faction index (the field) = EBX = 0, then from that
                // faction record (the routine, element stride 0x50) its id and the first key of
                // each set (the leftmost node's +0xc: head +0x18 / +0x2c / +0x40 -> [0] -> +0xc),
                // with no empty test.
                memset(&G_CharacterCreateSelection, 0, sizeof(G_CharacterCreateSelection));
                G_CharacterCreateSelection.FactionIndex = 0;
                FNewCharFaction const& Faction =
                    G_LoginMgr.GetFactions()[static_cast<size_t>(
                        G_CharacterCreateSelection.FactionIndex)];
                G_CharacterCreateSelection.Info.Field_0x30 = Faction.NicknameId;
                G_CharacterCreateSelection.Info.Field_0x34 = *Faction.Bases.begin();    // bd2c
                G_CharacterCreateSelection.Info.Field_0x38 = *Faction.Packages.begin(); // bd30
                G_CharacterCreateSelection.Info.Field_0x3c = *Faction.Pilots.begin();   // bd34
                Result = 0xe;
            }
            else if (LeaveDirection == -2)
            {
                ShutdownServerConnection();
                Result = 1;
            }
            else
            {
                ShutdownServerConnection();
                Result = 4;
            }
        }
    }

    // once IntroFocus has left the intro base (+0x5f9), the selected
    // row's character becomes the chosen one (loading screen first), state 0xc.
    if (bIntroBaseLeft == 0)
        return;
    bIntroBaseLeft = 0;
    bLoadRequested = 0;
    if (bCharacterChosen != 0)
        return;
    LoadingScreenSetClear(1); // (1)
    LoadingScreenStart();
    unsigned SelectedRow = 0;
    if (!CharacterTable->GetSelectedRow(&SelectedRow))
        return;
    FUiFastTableRow* const RowRecord = CharacterTable->GetRow(SelectedRow);
    if (RowRecord->Payload.Object == nullptr)
        return;
    bCharacterChosen = 1;
    SelectedCharacter = static_cast<CharacterRowData const*>(RowRecord->Payload.Object)->Id;
    Result = 0xc;
    if (SelectedCharacter != CurrentCharacter)
        ChatResetForConnect();
    CurrentCharacter = SelectedCharacter;
}

void PageCharacterSelect::RefreshMotd()
{
    // only when the server's message of the day changed.
    int const Version = ChatGetServerMotdVersion();
    if (Version == MotdVersion)
        return;
    MotdVersion = Version;
    Motd->ClearContent();
    AppendJustify(Motd, TextJustifyCenter);
    AppendStyle(Motd, 0x4433);
    AppendRef(Motd, MotdHeader);
    AppendParagraph(Motd);
    AppendJustify(Motd, TextJustifyLeft);
    AppendStyle(Motd, 0x76ec);
    Motd->AppendContent(*ChatGetServerMotdRdl());
}

void PageCharacterSelect::BeginLeave()
{
    // no direction; the caller sets it.
    bLeaving = 1;
    SlideT = 0.0;
    LoadCharacter->bInteractive = 0;
    NewCharacter->bInteractive = 0;
    DeleteCharacter->bInteractive = 0;
    Back->bInteractive = 0;
    MainMenu->bInteractive = 0;
    LoadCharacter->SetSkipUpload(true);
    NewCharacter->SetSkipUpload(true);
    DeleteCharacter->SetSkipUpload(true);
    Back->SetSkipUpload(true);
    MainMenu->SetSkipUpload(true);
    HideWin3D(ServerDetails);
    HideWin3D(Motd);
    HideWin3D(Title);
    HideWin3D(CharacterTable);
    HideWin3D(CharacterScroll);
    PlayUiSoundSlot(0x1b);
    DisableToolTips(); // spelled inline here (0x0056ae16..0x0056ae68)
}

PageCharacterSelect& GetPageCharacterSelect()
{
    return G_PageCharacterSelect;
}
