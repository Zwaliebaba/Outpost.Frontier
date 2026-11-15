// Author: Giperion.

#include "page_character_select_single.h"

#include "font_string.h"       // FHudWideText, WideTextAssign (the routine)
#include "globals.h"           // GetClientFrameTimeCapped (the routine)
#include "ids_rdl.h"           // AppendTextAsRdl (the routine), AppendTextMaybeXmlRdl (the routine)
#include "ids_strings.h"       // GetIdsStringWWithRawFallback (the routine)
#include "intro_page_util.h"
#include "loading_screen.h"    // LoadingScreenSetClear / LoadingScreenStart (the routine / 290)
#include "login_mgr.h"         // G_LoginMgr (the field)
#include "menu_ui_init.h"      // PlayUiSoundSlot (the routine), PlayOrRewindUiSoundSlot (5645c0)
#include "nn_confirm.h"        // NnConfirmOpenFormatted (the routine)
#include "nn_error.h"          // NnErrorRunModal (the routine)
#include "nn_load_save.h"      // NnLoadSaveEntryText (the routine)
#include "ui_list_box.h"
#include "ui_rich_text_win.h"
#include "ui_scroll.h"
#include "ui_text_msg_button.h"

#include "common_archetype.h"  // Archetype::GetShip
#include "common_misc.h"       // filetime_to_wstring
#include "common_text.h"
#include "common_tstring.h"    // TString<32> (the routine)
#include "common_universe.h"   // Universe::get_system

#include <stdint.h>            // intptr_t
#include <stdlib.h>            // free
#include <string.h>            // memcmp, memcpy
#include <wchar.h>             // swprintf, wcslen
#include <string>
#include <vector>

using namespace IntroPageUtil;

namespace
{
    // .
    PageCharacterSelectSingle G_PageCharacterSelectSingle;

    // L"-", the text of an empty value window.
    wchar_t const kDashText[] = L"-";

    // The UIListBox rows carry the CharacterInfo* as their data word (0x0056b667 / 0x0056b685).
    int InfoToItemData(CCharacterSet::CharacterInfo* Info)
    {
        return static_cast<int>(reinterpret_cast<intptr_t>(Info));
    }

    CCharacterSet::CharacterInfo* ItemDataToInfo(int Data)
    {
        return reinterpret_cast<CCharacterSet::CharacterInfo*>(static_cast<intptr_t>(Data));
    }

    void AppendStyle(UiRichTextWin* Win, unsigned short Style)
    {
        StyleNode const Node(Style);
        Win->AppendNode(Node);
    }

    void AppendRef(UiRichTextWin* Win, RenderDisplayList& Rdl)
    {
        RDLRefNode const Node(Rdl);
        Win->AppendNode(Node);
    }

    // The {Text, Capacity} local of ShowDetails (0x0056bfc8..0x0056c0c2): free()d on the way out
    // (0x0056c0bc, and by the EH state 6 unwind).
    struct FScopedWideText : FHudWideText
    {
        FScopedWideText()
        {
            Text = nullptr;
            Capacity = 0;
        }
        ~FScopedWideText()
        {
            if (Text != nullptr)
                free(Text);
        }
        FScopedWideText(FScopedWideText const&) = delete;
        FScopedWideText& operator=(FScopedWideText const&) = delete;
    };

    // The x87 row loop over the three "bImg" statics (0x0056cf1c..0x0056cf76 and
    // ): EDI from 2 down to 0 against +0xf4, +0xf8, +0xfc; the offset
    // V = EDI * 0.1 (the field) + SlideT. FCOM 0.0 (the field), TEST AH,0x41 / JNZ: at
    // or below 0.0 (or unordered) the image goes to {V, 0, 0}; above 0.0 (fall-through) to
    // {0, 0, 0} (all three from the 0.0f the field still on the stack). Slot 5 SetPosition.
    void PlaceButtonImages(Win3D* const* Images, double SlideT)
    {
        int Row = 2;
        for (int Index = 0; Index < 3; ++Index, --Row)
        {
            double const V = static_cast<double>(Row) * 0.1 + SlideT;
            float Position[3];
            if (V > 0.0)
            {
                Position[0] = 0.0f;
                Position[1] = 0.0f;
                Position[2] = 0.0f;
            }
            else
            {
                Position[0] = static_cast<float>(V);
                Position[1] = 0.0f;
                Position[2] = 0.0f;
            }
            Images[Index]->SetPosition(Position);
        }
    }
}

// ---------------------------------------------------------------------------------------------

PageCharacterSelectSingle::PageCharacterSelectSingle()
    : UIPage::MasterWin(Desc.GetPageData())
    , Result(0)
    , CharacterMoney(nullptr)
    , CharacterRank(nullptr)
    , CharacterLocation(nullptr)
    , CharacterShip(nullptr)
    , LoadCharacter(nullptr)
    , DeleteCharacter(nullptr)
    , MainMenu(nullptr)
    , CharScroll(nullptr)
    , CharSelDesc(nullptr)
    , CharSelTime(nullptr)
    , MoneyLabel(nullptr)
    , RankLabel(nullptr)
    , LocationLabel(nullptr)
    , ShipLabel(nullptr)
    , ScreenTitle(nullptr)
    , ButtonImages{nullptr, nullptr, nullptr}
    , BigBox(nullptr)
    , SmallBox(nullptr)
    , SlideT(1.0)
    , bLeaving(0)
    , LeaveDirection(-1)
    , bDeletePending(0)
    , bCharacterChosen(0)
    , bLoadRequested(0)
    , bIntroBaseLeft(0)
{
    // the routine(the routine(+0x8c)), the routine on +0x8c, +0xb8..+0xf0 = 0,
    // +0x118's first byte 0, the five lists +0x31c..+0x34c, the vector +0x358, +0x368 = +0x369 =
    // 0, +0xb4 = 0, +0x108 = 1.0, +0x110 = 0, the vftables, +0x114 = -1. The statics
    // +0xf4..+0x104 and the bytes +0x318 / +0x319 are not written; the static object starts
    // zeroed, which is what the initialisers above reproduce.
}

PageCharacterSelectSingle::~PageCharacterSelectSingle()
{
    // vftables back, the vector (the routine over its rows, then freed), the
    // lists +0x34c..+0x31c (the routine), +0xf0..+0xb8 = 0, the routine on +0x8c, the routine
    // -- the member destructors in reverse declaration order, then the base.
}

void PageCharacterSelectSingle::Destroy()
{
    // . Hidden first.
    Flags = static_cast<unsigned char>(Flags & 0xfc);

    // every child -- the next one read first, its class name copied
    // into a TString<32> (the routine), slot 10, then deleted (slot 0 with 1) when the copy's
    // first 14 bytes are "StaticControl" with its terminator (REPE CMPSB, ECX = 0xe).
    Win3D* Child = ChildHead;
    while (Child != nullptr)
    {
        Win3D* const Next = Child->NextSibling;
        TString<32> ClassCopy;
        ClassCopy.Length = 0;
        ClassCopy.Data[0] = '\0';
        ClassCopy = Child->ClassName;
        Child->Destroy();
        if (memcmp(ClassCopy.Data, "StaticControl", 14) == 0)
            delete Child;
        Child = Next;
    }

    UIPage::MasterWin::Destroy();
    UnregisterEvent(0xf);         // (0xf) on the +0x80 face

    // .
    CharacterShip = nullptr;
    LoadCharacter = nullptr;
    DeleteCharacter = nullptr;
    MainMenu = nullptr;
    CharacterMoney = nullptr;
    CharacterRank = nullptr;
    CharacterLocation = nullptr;
    CharScroll = nullptr;
    CharSelDesc = nullptr;
    CharSelTime = nullptr;
    MoneyLabel = nullptr;
    RankLabel = nullptr;
    LocationLabel = nullptr;
    ShipLabel = nullptr;
    ScreenTitle = nullptr;
}

bool PageCharacterSelectSingle::Create(Win3D* NewParent, void* /*Payload*/)
{
    // . MasterWin::Create (the routine) with no payload; its result is not tested.
    UIPage::MasterWin::Create(NewParent, nullptr);
    Result = 0;

    // the widgets through the IUIPage face (slot 4), in baseline's order.
    CharacterShip = static_cast<UiRichTextWin*>(FindWidget("CharacterShip"));
    LoadCharacter = static_cast<UiTextMsgButton*>(FindWidget("LoadCharacter"));
    DeleteCharacter = static_cast<UiTextMsgButton*>(FindWidget("DeleteCharacter"));
    MainMenu = static_cast<UiTextMsgButton*>(FindWidget("MainMenu"));
    CharacterMoney = static_cast<UiRichTextWin*>(FindWidget("CharacterMoney"));
    CharacterRank = static_cast<UiRichTextWin*>(FindWidget("CharacterRank"));
    CharacterLocation = static_cast<UiRichTextWin*>(FindWidget("CharacterLocation"));
    CharScroll = static_cast<UiScroll*>(FindWidget("CharScroll"));
    CharSelDesc = static_cast<UiListBox*>(FindWidget("CharSelDesc"));
    CharSelTime = static_cast<UiListBox*>(FindWidget("CharSelTime"));
    MoneyLabel = static_cast<UiRichTextWin*>(FindWidget("MoneyLabel"));
    RankLabel = static_cast<UiRichTextWin*>(FindWidget("RankLabel"));
    LocationLabel = static_cast<UiRichTextWin*>(FindWidget("LocationLabel"));
    ShipLabel = static_cast<UiRichTextWin*>(FindWidget("ShipLabel"));
    ScreenTitle = static_cast<UiRichTextWin*>(FindWidget("ScreenTitle"));

    RegisterEvent(0xf); // the routine

    // the scroll bar's chrome colours, a page of 20, step 1.
    CharScroll->SetArrowColors(ChromeColor(), ChromeColor());
    CharScroll->SetBarColors(ChromeColor(), ChromeColor());
    CharScroll->SetPage(20.0f);                               // (0x41a00000)
    CharScroll->SetStep(1.0f);                                // (0x3f800000)

    // the three buttons draw no frame (the routine(0)).
    LoadCharacter->SetDrawFrame(false);
    DeleteCharacter->SetDrawFrame(false);
    MainMenu->SetDrawFrame(false);

    // .
    bDeletePending = 0;
    bCharacterChosen = 0;
    SlideT = -2.0;

    // the faction records (CLoginMgr +0x1d8) rebuilt (the routine).
    G_LoginMgr.ReloadFactions();

    // the roster refreshed (the routine); only a 1 fills the lists.
    if (G_LoginMgr.RequestCharacterInfo() == 1)
    {
        // both lists emptied, page 20 / step 1, the bar shown.
        CharSelDesc->ItemCount = 0;
        CharSelTime->ItemCount = 0;
        CharScroll->SetPage(20.0f);
        CharScroll->SetStep(1.0f);
        ShowWin3D(CharScroll);

        // the roster count (the routine on the field), the vector
        // emptied (the routine) and reserved (the routine); one row per record that is not
        // the restart save (+0x200), appended (the routine) and added to both lists
        // (the routine) with the record as the data word.
        int Added = 0;
        unsigned const Count = G_LoginMgr.Characters.count();
        Entries.clear();
        Entries.reserve(Count);
        for (unsigned Index = 0; Index < Count; ++Index)
        {
            CCharacterSet::CharacterInfo* const Info =
                G_LoginMgr.CharacterAt(static_cast<int>(Index));
            if (Info == nullptr || Info->bIsRestart != 0)
                continue;
            FCharSelectSingleEntry Entry;
            Entry.Info = Info;
            wchar_t const* const EntryText = NnLoadSaveEntryText(Info);
            Entry.Name.assign(EntryText, wcslen(EntryText));
            wchar_t const* const TimeText =
                filetime_to_wstring(*reinterpret_cast<_FILETIME const*>(&Info->TimestampLow));
            Entry.Time.assign(TimeText, wcslen(TimeText));
            Entries.push_back(Entry);
            FCharSelectSingleEntry const& Last = Entries.back();
            CharSelDesc->AddItem(Last.Name.c_str(), InfoToItemData(Last.Info));
            CharSelTime->AddItem(Last.Time.c_str(), InfoToItemData(Last.Info));
            ++Added;
        } // Entry destroyed (the routine)

        // the range 0..rows added (FILD), page 20, step 1, value 0.
        CharScroll->SetRange(0.0f, static_cast<float>(Added));
        CharScroll->SetPage(20.0f);
        CharScroll->SetStep(1.0f);
        CharScroll->SetValue(0.0f);
        UpdateScrollVisibility();

        // both lists from the bar's value (ftol).
        CharSelDesc->FirstVisible = static_cast<int>(CharScroll->GetValue());
        CharSelTime->FirstVisible = static_cast<int>(CharScroll->GetValue());
    }

    // the ship window emptied, the three buttons shown.
    CharacterShip->ClearContent();
    LoadCharacter->Flags = static_cast<unsigned char>(LoadCharacter->Flags | 3);
    DeleteCharacter->Flags = static_cast<unsigned char>(DeleteCharacter->Flags | 3);
    MainMenu->Flags = static_cast<unsigned char>(MainMenu->Flags | 3);

    // no frame, row rules on, for both lists.
    CharSelDesc->bDrawFrame = 0;
    CharSelDesc->bDrawRowRules = 1;
    CharSelTime->bDrawFrame = 0;
    CharSelTime->bDrawRowRules = 1;

    // the routine with EBP, 0 on both paths.
    ShowDetails(nullptr);

    // all fifteen widgets hidden.
    HideWin3D(CharacterMoney);
    HideWin3D(CharacterRank);
    HideWin3D(CharacterLocation);
    HideWin3D(CharacterShip);
    HideWin3D(LoadCharacter);
    HideWin3D(DeleteCharacter);
    HideWin3D(MainMenu);
    HideWin3D(CharScroll);
    HideWin3D(CharSelDesc);
    HideWin3D(CharSelTime);
    HideWin3D(MoneyLabel);
    HideWin3D(RankLabel);
    HideWin3D(LocationLabel);
    HideWin3D(ShipLabel);
    HideWin3D(ScreenTitle);

    // the five captions (the routine, then the routine; no
    // trailing-paragraph cut).
    LoadIds(MoneyLabelText, 0x594);
    LoadIds(RankLabelText, 0x5ec);
    LoadIds(LocationLabelText, 0x593);
    LoadIds(ShipLabelText, 0x591);
    LoadIds(TitleText, 0x4f5);

    // each label emptied, a style, then a reference to its caption.
    MoneyLabel->ClearContent();
    AppendStyle(MoneyLabel, 0x5724);
    AppendRef(MoneyLabel, MoneyLabelText);
    RankLabel->ClearContent();
    AppendStyle(RankLabel, 0x5724);
    AppendRef(RankLabel, RankLabelText);
    LocationLabel->ClearContent();
    AppendStyle(LocationLabel, 0x5724);
    AppendRef(LocationLabel, LocationLabelText);
    ShipLabel->ClearContent();
    AppendStyle(ShipLabel, 0x5724);
    AppendRef(ShipLabel, ShipLabelText);
    ScreenTitle->ClearContent();
    AppendStyle(ScreenTitle, 0x1df5);
    AppendRef(ScreenTitle, TitleText);

    // L"-" (the routine) into a local list, copied into the four
    // value windows behind the same style.
    {
        RenderDisplayList Dash;
        AppendTextMaybeXmlRdl(kDashText, static_cast<unsigned>(wcslen(kDashText)), Dash, false);
        CharacterShip->ClearContent();
        AppendStyle(CharacterShip, 0x5724);
        CharacterShip->AppendContent(Dash);
        CharacterMoney->ClearContent();
        AppendStyle(CharacterMoney, 0x5724);
        CharacterMoney->AppendContent(Dash);
        CharacterRank->ClearContent();
        AppendStyle(CharacterRank, 0x5724);
        CharacterRank->AppendContent(Dash);
        CharacterLocation->ClearContent();
        AppendStyle(CharacterLocation, 0x5724);
        CharacterLocation->AppendContent(Dash);

        // twenty visible rows in both lists.
        CharSelDesc->VisibleRows = 0x14;
        CharSelTime->VisibleRows = 0x14;
    } // the local list's nodes and head freed
    return true;
}

void PageCharacterSelectSingle::UpdateScrollVisibility()
{
    // CharSelDesc's count - 20 >= 1 (JGE) shows the bar, else hidden.
    if (CharSelDesc->ItemCount - 0x14 >= 1)
        CharScroll->Flags = static_cast<unsigned char>(CharScroll->Flags | 3);
    else
        CharScroll->Flags = static_cast<unsigned char>(CharScroll->Flags & 0xfc);
}

void PageCharacterSelectSingle::ShowDetails(CCharacterSet::CharacterInfo const* Info)
{
    // (RET 4, 0x2244-byte frame).
    if (Info == nullptr)
    {
        // "-" (the routine) into the ship, money, rank and location
        // windows; Load and Delete inactive and not interactive.
        RenderDisplayList Dash;
        AppendTextMaybeXmlRdl(kDashText, static_cast<unsigned>(wcslen(kDashText)), Dash, false);
        CharacterShip->ClearContent();
        AppendStyle(CharacterShip, 0x5724);
        CharacterShip->AppendContent(Dash);
        CharacterMoney->ClearContent();
        AppendStyle(CharacterMoney, 0x5724);
        CharacterMoney->AppendContent(Dash);
        CharacterRank->ClearContent();
        AppendStyle(CharacterRank, 0x5724);
        CharacterRank->AppendContent(Dash);
        CharacterLocation->ClearContent();
        AppendStyle(CharacterLocation, 0x5724);
        CharacterLocation->AppendContent(Dash);
        LoadCharacter->Flags = static_cast<unsigned char>(LoadCharacter->Flags & 0xfe);
        DeleteCharacter->Flags = static_cast<unsigned char>(DeleteCharacter->Flags & 0xfe);
        LoadCharacter->bInteractive = 0;
        DeleteCharacter->bInteractive = 0;
        return;
    }

    // the money (+0x234) through L"%d" (0x005d0ec4) into a 0x80-wide
    // buffer, then into its own list (the routine / the routine) behind the style.
    wchar_t Number[0x80];
    swprintf(Number, 0x80, L"%d", Info->Money);
    CharacterMoney->ClearContent();
    AppendStyle(CharacterMoney, 0x5724);
    RenderDisplayList MoneyText;
    AppendTextAsRdl(Number, static_cast<unsigned>(wcslen(Number)), MoneyText, false);
    CharacterMoney->AppendContent(MoneyText);

    // the rank (+0x270), the same way.
    swprintf(Number, 0x80, L"%d", Info->Rank);
    CharacterRank->ClearContent();
    AppendStyle(CharacterRank, 0x5724);
    RenderDisplayList RankText;
    AppendTextAsRdl(Number, static_cast<unsigned>(wcslen(Number)), RankText, false);
    CharacterRank->AppendContent(RankText);

    // the ship's name -- only with a ship archetype (+0x230) that
    // resolves: its ids_name (+0x14) through the routine into 0x80 wide chars (the first char
    // zeroed on a miss), then its own list behind the style.
    CharacterShip->ClearContent();
    if (Info->ShipArchetype != 0)
    {
        Archetype::Ship const* const Ship = Archetype::GetShip(Info->ShipArchetype);
        if (Ship != nullptr)
        {
            wchar_t ShipName[0x80];
            if (GetIdsStringWWithRawFallback(static_cast<unsigned>(Ship->IdsName), ShipName,
                                             0x80) == 0)
                ShipName[0] = L'\0';
            AppendStyle(CharacterShip, 0x5724);
            RenderDisplayList ShipText;
            AppendTextAsRdl(ShipName, static_cast<unsigned>(wcslen(ShipName)), ShipText, false);
            CharacterShip->AppendContent(ShipText);
        }
    }

    // the system's name -- ISystem slot 3 (strid_name) through
    // into 0x1000 wide chars, into a {Text, Capacity} (the routine: "IDS?" on a
    // miss, else the copied length), then its own list behind the style; the text freed.
    Universe::ISystem const* const System = Universe::get_system(Info->SystemId);
    CharacterLocation->ClearContent();
    if (System != nullptr)
    {
        FScopedWideText SystemName;
        wchar_t Buffer[0x1000];
        unsigned const Length = GetIdsStringWWithRawFallback(
            (unsigned)System->GetStridName(), Buffer, 0x1000);
        if (Length == 0)
            WideTextAssign(&SystemName, L"IDS?", 0xffffffffu);
        else
            WideTextAssign(&SystemName, Buffer, Length);
        AppendStyle(CharacterLocation, 0x5724);
        RenderDisplayList LocationText;
        AppendTextAsRdl(SystemName.Text, static_cast<unsigned>(wcslen(SystemName.Text)),
                        LocationText, false);
        CharacterLocation->AppendContent(LocationText);
    }

    // Load and Delete active and interactive.
    LoadCharacter->Flags = static_cast<unsigned char>(LoadCharacter->Flags | 1);
    DeleteCharacter->Flags = static_cast<unsigned char>(DeleteCharacter->Flags | 1);
    LoadCharacter->bInteractive = 1;
    DeleteCharacter->bInteractive = 1;
} // RankText, then MoneyText (0x0056c0ff, 0x0056c104..0x0056c336)

void PageCharacterSelectSingle::BeginLeave(int Direction)
{
    // Every widget hidden, the three "bImg" statics shown, the lists not selectable, the three
    // buttons not interactive, the direction, then (0x0056caac) leaving from 0.0 with the slide
    // sound.
    HideWin3D(CharacterMoney);
    HideWin3D(CharacterRank);
    HideWin3D(CharacterLocation);
    HideWin3D(CharacterShip);
    HideWin3D(LoadCharacter);
    HideWin3D(DeleteCharacter);
    HideWin3D(MainMenu);
    HideWin3D(CharScroll);
    HideWin3D(CharSelDesc);
    HideWin3D(CharSelTime);
    HideWin3D(MoneyLabel);
    HideWin3D(RankLabel);
    HideWin3D(LocationLabel);
    HideWin3D(ShipLabel);
    HideWin3D(ScreenTitle);
    ShowWin3D(ButtonImages[0]);
    ShowWin3D(ButtonImages[1]);
    ShowWin3D(ButtonImages[2]);
    CharSelDesc->bSelectable = 0;
    CharSelTime->bSelectable = 0;
    LoadCharacter->bInteractive = 0;
    DeleteCharacter->bInteractive = 0;
    MainMenu->bInteractive = 0;
    LeaveDirection = Direction;
    bLeaving = 1;
    SlideT = 0.0;
    PlayUiSoundSlot(0x1b);
}

bool PageCharacterSelectSingle::Slot23(unsigned Msg, unsigned A, unsigned B)
{
    // .
    if (Msg == 0x3004)
    {
        PlayOrRewindUiSoundSlot(0); // (0)
        if (A == 0x3003)
        {
            // load -- nothing while leaving.
            if (bLeaving != 0)
                return true;
            BeginLeave(1);
            return true;
        }
        if (A == 0x300c)
        {
            // main menu.
            if (bLeaving != 0)
                return true;
            BeginLeave(0);
            return true;
        }
        if (A == 0x3002)
        {
            // delete.
            if (bDeletePending != 0)
            {
                // the confirm box said yes -- everything live again.
                bDeletePending = 0;
                CharScroll->Flags = static_cast<unsigned char>(CharScroll->Flags | 1);
                CharSelDesc->Flags = static_cast<unsigned char>(CharSelDesc->Flags | 1);
                CharSelTime->Flags = static_cast<unsigned char>(CharSelTime->Flags | 1);
                MainMenu->Flags = static_cast<unsigned char>(MainMenu->Flags | 1);
                LoadCharacter->Flags = static_cast<unsigned char>(LoadCharacter->Flags | 1);
                DeleteCharacter->Flags = static_cast<unsigned char>(DeleteCharacter->Flags | 1);
                MainMenu->bInteractive = 1;
                LoadCharacter->bInteractive = 1;
                DeleteCharacter->bInteractive = 1;
                if (CharSelDesc->SelectedIndex == -1)
                    return true;

                // the selected record's CHARACTER_ID copied raw (REP
                // MOVSD, 0x80 dwords), the rows dropped (the routine), both lists emptied,
                // the bar's range 0..0.
                CCharacterSet::CharacterInfo const* const Victim =
                    ItemDataToInfo(CharSelDesc->GetItemData(CharSelDesc->SelectedIndex));
                CHARACTER_ID VictimId;
                memcpy(VictimId.Buffer, Victim->Id.Buffer, sizeof(VictimId.Buffer));
                Entries.clear();
                CharSelDesc->ItemCount = 0;
                CharSelTime->ItemCount = 0;
                CharScroll->SetRange(0.0f, 0.0f);

                int const Destroyed = G_LoginMgr.DestroyCharacter(VictimId);
                if (Destroyed == 1)
                {
                    // no selection (CMP count,-1 / JLE), the range's
                    // top one lower (the routine, FSUB 1.0f the field), the details
                    // emptied, the lists refilled.
                    if (CharSelDesc->ItemCount > -1)
                        CharSelDesc->SelectedIndex = -1;
                    if (CharSelTime->ItemCount > -1)
                        CharSelTime->SelectedIndex = -1;
                    float Min = 0.0f;
                    float Max = 0.0f;
                    CharScroll->GetRange(&Min, &Max);
                    CharScroll->SetRange(Min, static_cast<float>(Max - 1.0f));
                    ShowDetails(nullptr);
                    RefillLists();
                    return true;
                }
                if (Destroyed != 2)
                {
                    // the delete failed -- NN_Error 0x73b, the slide
                    // reset to -2.0, the faction list and the roster fetched again.
                    NnErrorRunModal(0x73b, false, nullptr);
                    SlideT = -2.0;
                    G_LoginMgr.ReloadFactions(); // the routine
                    int const Relisted = G_LoginMgr.RequestCharacterInfo();
                    if (Relisted == 1)
                    {
                        RefillLists();
                        return true;
                    }
                    if (Relisted != 2)
                        NnErrorRunModal(0x73c, false, nullptr);
                }
                Result = 5;
                RefillLists();
                return true;
            }

            // first press -- the confirm box over the page; everything inactive
            // until it answers (0x3006).
            bDeletePending = 1;
            CharScroll->Flags = static_cast<unsigned char>(CharScroll->Flags & 0xfe);
            CharSelDesc->Flags = static_cast<unsigned char>(CharSelDesc->Flags & 0xfe);
            CharSelTime->Flags = static_cast<unsigned char>(CharSelTime->Flags & 0xfe);
            MainMenu->Flags = static_cast<unsigned char>(MainMenu->Flags & 0xfe);
            LoadCharacter->Flags = static_cast<unsigned char>(LoadCharacter->Flags & 0xfe);
            DeleteCharacter->Flags = static_cast<unsigned char>(DeleteCharacter->Flags & 0xfe);
            MainMenu->bInteractive = 0;
            LoadCharacter->bInteractive = 0;
            DeleteCharacter->bInteractive = 0;
            if (CharSelDesc->SelectedIndex == -1)
                return true;
            CCharacterSet::CharacterInfo const* const Selected =
                ItemDataToInfo(CharSelDesc->GetItemData(CharSelDesc->SelectedIndex));
            NnConfirmOpenFormatted(this, NnLoadSaveEntryText(Selected), nullptr);
            return true;
        }
        if (A == 0xf003)
        {
            // the confirm box said no -- everything live again; false either way.
            if (bDeletePending != 0)
            {
                bDeletePending = 0;
                CharScroll->Flags = static_cast<unsigned char>(CharScroll->Flags | 1);
                CharSelDesc->Flags = static_cast<unsigned char>(CharSelDesc->Flags | 1);
                CharSelTime->Flags = static_cast<unsigned char>(CharSelTime->Flags | 1);
                MainMenu->Flags = static_cast<unsigned char>(MainMenu->Flags | 1);
                LoadCharacter->Flags = static_cast<unsigned char>(LoadCharacter->Flags | 1);
                DeleteCharacter->Flags = static_cast<unsigned char>(DeleteCharacter->Flags | 1);
                MainMenu->bInteractive = 1;
                LoadCharacter->bInteractive = 1;
                DeleteCharacter->bInteractive = 1;
            }
            return false;
        }
        return false;
    }

    if (Msg == 0xf001)
    {
        // a click on a row (B, from ListBox::MouseEvent) selects it in
        // both lists when -1 <= B < count; the details follow; false.
        int const Row = static_cast<int>(B);
        if (Row >= -1 && Row < CharSelDesc->ItemCount)
            CharSelDesc->SelectedIndex = Row;
        if (Row >= -1 && Row < CharSelTime->ItemCount)
            CharSelTime->SelectedIndex = Row;
        ShowDetails(ItemDataToInfo(CharSelDesc->GetItemData(Row)));
        return false;
    }

    if (Msg == 0x3006)
    {
        // the confirm box's answer, back through slot 23.
        if (A == 0)
        {
            Slot23(0x3004, 0xf003, 0);
            return true;
        }
        if (A == 1)
        {
            Slot23(0x3004, 0x3002, 0);
            return true;
        }
        return true;
    }

    if (Msg == 0xf002)
    {
        // a double click loads the selection -- not without one, not while leaving.
        if (CharSelDesc->SelectedIndex == -1)
            return true;
        if (bLeaving != 0)
            return true;
        BeginLeave(1);
        return true;
    }

    if (Msg == 0xf005)
    {
        // the bar moved -- both lists from its value (ftol).
        CharSelDesc->FirstVisible = static_cast<int>(CharScroll->GetValue());
        CharSelTime->FirstVisible = static_cast<int>(CharScroll->GetValue());
        return true;
    }
    return false;
}

void PageCharacterSelectSingle::RefillLists()
{
    // . 0x0056cb5d..0x0056cba9: both lists emptied, page 20 / step 1, the bar shown.
    CharSelDesc->ItemCount = 0;
    CharSelTime->ItemCount = 0;
    CharScroll->SetPage(20.0f);
    CharScroll->SetStep(1.0f);
    ShowWin3D(CharScroll);

    // the roster count (the routine), the rows dropped (the routine)
    // and the storage grown to the count when smaller (the reserve inline).
    int Added = 0;
    unsigned const Count = G_LoginMgr.Characters.count();
    Entries.clear();
    if (Entries.capacity() < Count)
        Entries.reserve(Count);

    // as Create's loop (the row's strings built empty, then assigned).
    for (unsigned Index = 0; Index < Count; ++Index)
    {
        CCharacterSet::CharacterInfo* const Info = G_LoginMgr.CharacterAt(static_cast<int>(Index));
        if (Info == nullptr || Info->bIsRestart != 0)
            continue;
        FCharSelectSingleEntry Entry;
        Entry.Info = Info;
        wchar_t const* const EntryText = NnLoadSaveEntryText(Info);
        Entry.Name.assign(EntryText, wcslen(EntryText));
        wchar_t const* const TimeText =
            filetime_to_wstring(*reinterpret_cast<_FILETIME const*>(&Info->TimestampLow));
        Entry.Time.assign(TimeText, wcslen(TimeText));
        Entries.push_back(Entry);
        FCharSelectSingleEntry const& Last = Entries.back();
        CharSelDesc->AddItem(Last.Name.c_str(), InfoToItemData(Last.Info));
        CharSelTime->AddItem(Last.Time.c_str(), InfoToItemData(Last.Info));
        ++Added;
    }

    // range 0..rows added (FILD), page 20, step 1, value 0.
    CharScroll->SetRange(0.0f, static_cast<float>(Added));
    CharScroll->SetPage(20.0f);
    CharScroll->SetStep(1.0f);
    CharScroll->SetValue(0.0f);
    // the routine's body inline.
    UpdateScrollVisibility();
    // both lists from the bar's value (ftol).
    CharSelDesc->FirstVisible = static_cast<int>(CharScroll->GetValue());
    CharSelTime->FirstVisible = static_cast<int>(CharScroll->GetValue());
}

void PageCharacterSelectSingle::Update()
{
    // . Win3D's child walk (next read before each call, the routine inlined).
    Win3D::Update();

    // the row under the cursor in either list (CharSelTime's wins)
    // pulses in both, when -1 <= row < that list's count.
    int Hover = -1;
    if (CharSelDesc->HoverIndex != -1)
        Hover = CharSelDesc->HoverIndex;
    if (CharSelTime->HoverIndex != -1)
        Hover = CharSelTime->HoverIndex;
    if (Hover >= -1 && Hover < CharSelDesc->ItemCount)
        CharSelDesc->Field80 = Hover;
    if (Hover >= -1 && Hover < CharSelTime->ItemCount)
        CharSelTime->Field80 = Hover;

    // FCOMP 1.0 (the field), TEST AH,5 / JP -- the jump (not below
    // 1.0, or unordered) goes to the branch, which slides out only while leaving; the
    // fall-through (below 1.0) slides out while leaving (JNZ the branch), else slides in.
    bool const bBelowOne = SlideT < 1.0;
    if (bLeaving == 0 && bBelowOne)
    {
        // the frame time capped at 0.5 (FCOM 0.5 the field, TEST
        // AH,0x41 / JNZ keeps it when not greater, the fall-through loads 0.5), 1.3 per second
        // (the field).
        double Dt = GetClientFrameTimeCapped();
        if (Dt > 0.5)
            Dt = 0.5;
        SlideT = Dt * 1.3 + SlideT;

        PlaceButtonImages(ButtonImages, SlideT);

        // FCOMP 0.0, TEST AH,0x41 / JNZ the branch -- at or below 0.0
        // (or unordered) the boxes follow SlideT; above 0.0 (fall-through) everything arrives.
        if (SlideT > 0.0)
        {
            // both boxes home, the widgets shown (the bar only when
            // needed, the routine's test inline), the three "bImg" hidden, SlideT parked at 2.0.
            float const Home[3] = {0.0f, 0.0f, 0.0f};
            BigBox->SetPosition(Home);
            SmallBox->SetPosition(Home);
            ShowWin3D(CharacterMoney);
            ShowWin3D(CharacterRank);
            ShowWin3D(CharacterLocation);
            ShowWin3D(CharacterShip);
            ShowWin3D(LoadCharacter);
            ShowWin3D(DeleteCharacter);
            ShowWin3D(MainMenu);
            if (CharSelDesc->ItemCount - 0x14 >= 1)
                ShowWin3D(CharScroll);
            else
                HideWin3D(CharScroll);
            ShowWin3D(CharSelDesc);
            ShowWin3D(CharSelTime);
            ShowWin3D(MoneyLabel);
            ShowWin3D(RankLabel);
            ShowWin3D(LocationLabel);
            ShowWin3D(ShipLabel);
            ShowWin3D(ScreenTitle);
            HideWin3D(ButtonImages[0]);
            HideWin3D(ButtonImages[1]);
            HideWin3D(ButtonImages[2]);
            SlideT = 2.0;
        }
        else
        {
            // bigBox at {-1.5 * SlideT, 0, 0} (the field), smallBox at
            // {0, SlideT, 0}.
            float const BigPosition[3] = {static_cast<float>(SlideT * -1.5), 0.0f, 0.0f};
            BigBox->SetPosition(BigPosition);
            float const SmallPosition[3] = {0.0f, static_cast<float>(SlideT), 0.0f};
            SmallBox->SetPosition(SmallPosition);
        }

        // the slide sound while -0.5 < SlideT < -0.1 (FCOMP -0.5
        // , TEST AH,0x41 / JNZ skips at or below; FCOMP -0.1 the field, TEST AH,5
        // / JP skips when not below).
        if (SlideT > -0.5 && SlideT < -0.1)
            PlayUiSoundSlot(0x1b); // (0x1b)
    }
    else if (bLeaving != 0)
    {
        // SlideT runs down at 1.5 per second (the field, no cap; FSUBR).
        SlideT = SlideT - GetClientFrameTimeCapped() * 1.5;

        PlaceButtonImages(ButtonImages, SlideT);

        // FCOMP -0.5, TEST AH,5 / JP the branch -- not below -0.5 (or
        // unordered) the boxes follow; below -0.5 (fall-through) the leave ends.
        if (SlideT < -0.5)
        {
            // the three "bImg" hidden, the leave over; direction 1 asks
            // IntroFocus to leave the intro base (+0x368), anything else is state 1; SlideT back
            // to -2.0.
            HideWin3D(ButtonImages[0]);
            HideWin3D(ButtonImages[1]);
            HideWin3D(ButtonImages[2]);
            bLeaving = 0;
            if (LeaveDirection == 1)
                bLoadRequested = 1;
            else
                Result = 1;
            SlideT = -2.0;
        }
        else
        {
            // as the branch.
            float const BigPosition[3] = {static_cast<float>(SlideT * -1.5), 0.0f, 0.0f};
            BigBox->SetPosition(BigPosition);
            float const SmallPosition[3] = {0.0f, static_cast<float>(SlideT), 0.0f};
            SmallBox->SetPosition(SmallPosition);
        }
    }

    // once IntroFocus has left the intro base (+0x369), the selected
    // row's record becomes the chosen character (loading screen first), state 0xc.
    if (bIntroBaseLeft == 0)
        return;
    bIntroBaseLeft = 0;
    bLoadRequested = 0;
    if (bCharacterChosen != 0)
        return;
    LoadingScreenSetClear(1); // (1)
    LoadingScreenStart();
    CCharacterSet::CharacterInfo const* const Chosen =
        ItemDataToInfo(CharSelDesc->GetItemData(CharSelDesc->SelectedIndex));
    if (Chosen == nullptr)
        return;
    bCharacterChosen = 1;
    CHARACTER_ID Local; // REP MOVSD, 0x80 dwords, then CHARACTER_ID::operator=
    memcpy(Local.Buffer, Chosen->Id.Buffer, sizeof(Local.Buffer));
    SelectedCharacter = Local;
    Result = 0xc;
}

bool PageCharacterSelectSingle::OnEvent(unsigned MsgId)
{
    // (this = the +0x80 face): event 0xf outside a leave -> state 1 (face +0x34 =
    // page +0xb4); anything else is the routine (false).
    if (MsgId == 0xf && bLeaving == 0)
    {
        Result = 1;
        return true;
    }
    return UIPage::MasterWin::OnEvent(MsgId);
}

PageCharacterSelectSingle& GetPageCharacterSelectSingle()
{
    return G_PageCharacterSelectSingle;
}
