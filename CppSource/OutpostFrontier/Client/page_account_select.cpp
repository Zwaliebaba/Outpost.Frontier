// Author: Giperion.

#include "page_account_select.h"

#include "globals.h"          // GetClientFrameTimeCapped (the routine)
#include "ids_rdl.h"          // LoadIdsIntoRdl (the routine), RdlAppendNode, RdlClear
#include "menu_ui_init.h"     // PlayUiSoundSlot (the routine), PlayOrRewindUiSoundSlot
#include "nn_error.h"         // NnErrorRunModal (the routine)
#include "nn_ui_pulse.h"      // G_HudChromeColorRgba (the field)
#include "tooltip_hover.h"    // FToolTipHoverInfo
#include "ui_rich_text_win.h"
#include "ui_text_edit.h"
#include "ui_text_msg_button.h"
#include "win3d_immediate.h"  // Win3DVec3Add (the routine)

#include "common_net.h"       // CPlayerAccount
#include "common_text.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <vector>

namespace
{
    // .
    PageAccountSelect G_PageAccountSelect;

    // u"0123456789-ABCDEF" (0x00614c74), handed to the routine with a count of 0x12 -- the
    // seventeen characters and the terminator.
    wchar_t const kAccountIdChars[] = L"0123456789-ABCDEF";

    // The idiom the routine spells at every RDL append: the node cloned (slot +0x1c), the
    // clone pushed by the routine (whose cell clones it again), the first clone deleted
    // (the routine / the inline slot-0 call).
    void AppendNodeClone(RenderDisplayList& Rdl, RDLNode const& Node)
    {
        RDLNode* const Temp = Node.Clone();
        RdlAppendNode(Rdl, Temp->Clone());
        delete Temp;
    }

    // / 0x00572990..0x005729d3: the list emptied (the routine over
    // begin..end), StyleNode(0x9ca4) appended, the IDS text loaded and its trailing paragraph
    // stripped when the load worked.
    void LoadStyledIds(RenderDisplayList& Rdl, unsigned Ids)
    {
        RdlClear(Rdl);
        {
            StyleNode const Style(0x9ca4);
            AppendNodeClone(Rdl, Style);
        }
        if (LoadIdsIntoRdl(Ids, Rdl))
            RdlRemoveLastParagraph(Rdl);
    }

    void HideWin3D(Win3D* Object)
    {
        Object->Flags = static_cast<unsigned char>(Object->Flags & 0xfc);
    }

    void ShowWin3D(Win3D* Object)
    {
        Object->Flags = static_cast<unsigned char>(Object->Flags | 3);
    }
}

PageAccountSelect::PageAccountSelect()
    : UIPage::MasterWin(Desc.GetPageData())
    , Result(0)
    , SlideT(0.0)
    , bLeaving(0)
    , LeaveDirection(0)
    , BoxHome{0.0f, 0.0f, 0.0f}
    , AccountId(nullptr)
    , SelectServer(nullptr)
    , Back(nullptr)
    , Explain(nullptr)
    , AccountBox(nullptr)
{
    // the routine(the routine(+0x8c)) -- the page data pointer is taken before
    // the descriptor is built, as here -- then the routine on +0x8c, +0xe0..+0xf0 = 0, the two
    // RenderDisplayLists (+0xf4 / +0x100: allocator byte, a fresh 0xc-byte sentinel, size 0),
    // +0xb4 = 0 and the three vftables. +0xb8..+0xdc are not written; the static object starts
    // zeroed, which is what the initialisers above reproduce.
}

PageAccountSelect::~PageAccountSelect()
{
    // vftables back, +0x100 then +0xf4 emptied and their sentinels freed,
    // +0xe0..+0xf0 = 0, the routine on +0x8c, the routine -- the member destructors in reverse
    // declaration order, then the base.
}

bool PageAccountSelect::Create(Win3D* NewParent, void* /*Payload*/)
{
    // . MasterWin::Create (the routine) with no payload; false goes straight back.
    if (!UIPage::MasterWin::Create(NewParent, nullptr))
        return false;
    Result = 0;

    // the five widgets through the IUIPage face (slot 4).
    AccountId = static_cast<UiTextEdit*>(FindWidget("AccountID"));
    SelectServer = static_cast<UiTextMsgButton*>(FindWidget("SelectServer"));
    Back = static_cast<UiTextMsgButton*>(FindWidget("Back"));
    Explain = static_cast<UiRichTextWin*>(FindWidget("Explain"));
    AccountBox = FindWidget("AccountBoxShort");

    // slot 46 (+0xb8) on each, +4 of the record it returns.
    AccountId->GetToolTipHoverInfo()->IdsAlt = 0xaca;
    SelectServer->GetToolTipHoverInfo()->IdsAlt = 0xace;
    Back->GetToolTipHoverInfo()->IdsAlt = 0xac6;

    // the routine(0xf) on the Event::Handler face.
    RegisterEvent(0xf);

    LoadStyledIds(ExplainText, 0x60005);
    LoadStyledIds(Text601eb, 0x601eb);

    // the key field -- fixed style, no IME, the style for the fixed
    // list, a centred prefix of one space, 0x28 characters at most, the chrome frame, the
    // hex-and-dash allow list shown upper case, '-' for anything refused.
    AccountId->SetFixedStyle(true);   // (1)
    AccountId->SetImeEnabled(false);  // (0)
    {
        StyleNode const Style(0x9ca4);
        AppendNodeClone(AccountId->FixedStyle, Style); // +0x4fc
    }
    {
        JustifyNode const Justify(TextJustifyCenter);  // JustifyNode(1)
        AppendNodeClone(AccountId->Prefix, Justify);    // +0x4dc
    }
    {
        TextNode const Space(L" ", -1);
        AppendNodeClone(AccountId->Prefix, Space);
    }
    AccountId->SetMaxLength(0x28);
    AccountId->SetDrawFrame(true);
    AccountId->SetFrameColor(reinterpret_cast<unsigned long const*>(G_HudChromeColorRgba));
    AccountId->SetAlignMode(2);
    AccountId->AddFilterChars(kAccountIdChars, 0x12);
    AccountId->SetReplacementChar(L'-');
    AccountId->SetCaseMode(1);
    AccountId->SetFilterIsAllowList(true);

    // the explanation shows the +0xf4 list by reference.
    Explain->ClearContent();
    {
        RDLRefNode Ref(ExplainText);
        Explain->AppendNode(Ref);
    }
    Explain->SetAutoScrollbar(true); // (1)

    // a machine key generated by CPlayerAccount goes into the field,
    // widened with mbstowcs over strlen + 1 characters (baseline's buffer is _alloca'd). The
    // frame slot for the key is 0x28 bytes and a key of 0x28 characters puts its terminator in
    // the next local; the port's array holds it.
    char TextKey[0x28 + 1];
    if (CPlayerAccount::GenerateTextKey(TextKey))
    {
        size_t const Count = strlen(TextKey) + 1;
        std::vector<wchar_t> Wide(Count);
        mbstowcs(Wide.data(), TextKey, Count);
        AccountId->SetText(Wide.data(), nullptr); // (text, 0)
    }
    AccountId->Activate();

    // the slide starts at -1.5, the buttons' surfaces frozen, the key
    // field and the explanation hidden until the slide ends, the box's home position kept.
    SlideT = -1.5;
    bLeaving = 0;
    LeaveDirection = 0;
    Back->SetSkipUpload(true);          // (1)
    SelectServer->SetSkipUpload(true);
    HideWin3D(AccountId);
    HideWin3D(Explain);
    memcpy(BoxHome, AccountBox->Position, sizeof(BoxHome));
    return true;
}

void PageAccountSelect::Destroy()
{
    // the routine(0xf) on the Event::Handler face, MasterWin::Destroy
    // (the routine), then the five widget pointers cleared.
    UnregisterEvent(0xf);
    UIPage::MasterWin::Destroy();
    AccountId = nullptr;
    SelectServer = nullptr;
    Back = nullptr;
    Explain = nullptr;
    AccountBox = nullptr;
}

void PageAccountSelect::Update()
{
    // . 0x00572cc6..0x00572cd7: FCOMP 1.0, TEST AH,0x41 / JP -- the slide-in runs
    // unless SlideT > 1.0 (or unordered).
    if (!(SlideT > 1.0) && bLeaving == 0)
    {
        // the frame time, capped at 0.5 (FCOM 0.5, TEST AH,0x41 / JNZ
        // keeps it when not greater).
        double Dt = GetClientFrameTimeCapped();
        if (Dt > 0.5)
            Dt = 0.5;
        SlideT = Dt * 1.5 + SlideT;

        // FCOM 0.0, TEST AH,5 / JP -- still sliding while SlideT < 0.
        if (SlideT < 0.0)
        {
            // Back offset (t, 0, 0), SelectServer (-t, 0, 0) through
            // slot 42 property 6, the box lifted by t (the routine) through slot 5.
            float const T = static_cast<float>(SlideT);
            float BackOffset[3] = {T, 0.0f, 0.0f};
            float ServerOffset[3] = {static_cast<float>(-SlideT), 0.0f, 0.0f};
            float const Lift[3] = {0.0f, T, 0.0f};
            Back->SetProperty(6, reinterpret_cast<uintptr_t>(BackOffset), 0);
            SelectServer->SetProperty(6, reinterpret_cast<uintptr_t>(ServerOffset), 0);
            float BoxPosition[3];
            Win3DVec3Add(BoxPosition, BoxHome, Lift);
            AccountBox->SetPosition(BoxPosition);
        }
        else
        {
            // everything in place and live, SlideT parked at 2.0.
            float ZeroOffset[3] = {0.0f, 0.0f, 0.0f};
            Back->SetProperty(6, reinterpret_cast<uintptr_t>(ZeroOffset), 0);
            SelectServer->SetProperty(6, reinterpret_cast<uintptr_t>(ZeroOffset), 0);
            AccountBox->SetPosition(BoxHome);
            ShowWin3D(Explain);
            ShowWin3D(AccountId);
            Back->SetSkipUpload(false);
            SelectServer->SetSkipUpload(false);
            AccountId->SetToolTipEnabled(true); // (1)
            SelectServer->SetToolTipEnabled(true);
            Back->SetToolTipEnabled(true);
            SlideT = 2.0;
        }

        // the slide sound while -0.8 < SlideT < -0.1 (FCOMP -0.8,
        // TEST AH,0x41 / JNZ; FCOMP -0.1, TEST AH,5 / JP).
        if (SlideT > -0.8 && SlideT < -0.1)
            PlayUiSoundSlot(0x1b); // (0x1b)
    }

    // leaving -- SlideT runs down from 0 at 1.5 per second (no cap).
    if (bLeaving != 0 && LeaveDirection != 0)
    {
        SlideT = SlideT - GetClientFrameTimeCapped() * 1.5;
        float const T = static_cast<float>(SlideT);
        float BackOffset[3] = {T, 0.0f, 0.0f};
        float ServerOffset[3] = {static_cast<float>(-SlideT), 0.0f, 0.0f};
        Back->SetProperty(6, reinterpret_cast<uintptr_t>(BackOffset), 0);
        SelectServer->SetProperty(6, reinterpret_cast<uintptr_t>(ServerOffset), 0);
        // the routine(home.x, t + home.y, home.z) through slot 5.
        float BoxPosition[3] = {BoxHome[0], T + BoxHome[1], BoxHome[2]};
        AccountBox->SetPosition(BoxPosition);

        // FCOMP -0.9, TEST AH,0x41 / JP -- done once SlideT <= -0.9.
        if (SlideT <= -0.9)
        {
            HideWin3D(Back);
            HideWin3D(Explain);
            HideWin3D(SelectServer);
            HideWin3D(AccountId);
            HideWin3D(AccountBox);
            if (LeaveDirection == 1)
                Result = 5;
            else if (LeaveDirection == -1)
                Result = 1;
            bLeaving = 0;
        }
    }

    // Win3D's child walk (the routine inlined).
    Win3D::Update();
}

bool PageAccountSelect::Slot23(unsigned Msg, unsigned A, unsigned /*B*/)
{
    // .
    if (Msg == 0x3004)
    {
        PlayOrRewindUiSoundSlot(0); // (0)
        if (A == 0x3e9) // "Back" (0x005730fd)
        {
            BeginLeave(-1);
            return true;
        }
        if (A != 0x3ea) // "SelectServer" (0x00573053)
            return false;
        return SubmitTypedKey();
    }
    if (Msg == 0xf003)
    {
        BeginLeave(-1);
        return true;
    }
    if (Msg == 0xf004)
        return SubmitTypedKey();
    return false;
}

bool PageAccountSelect::SubmitTypedKey()
{
    // / 0x005731af..0x00573227: the typed key, lower-cased (towlower
    // until the terminator), narrowed with wcstombs over wcslen + 1 characters (baseline's
    // buffer is _alloca'd), and handed to CPlayerAccount::SetTextKey.
    wchar_t Typed[0x2a];
    AccountId->GetText(Typed, 0x29);
    for (wchar_t* Ch = Typed; *Ch != 0; ++Ch)
        *Ch = static_cast<wchar_t>(towlower(*Ch));
    size_t const Count = wcslen(Typed) + 1;
    std::vector<char> Narrow(Count);
    wcstombs(Narrow.data(), Typed, Count);
    if (!CPlayerAccount::SetTextKey(Narrow.data()))
    {
        // the "invalid key" error, run modally; the page stays.
        NnErrorRunModal(0x76a, false, nullptr); // (0x76a, 0, 0)
        LeaveDirection = 0;
        return true;
    }
    BeginLeave(1);
    return true;
}

bool PageAccountSelect::OnEvent(unsigned MsgId)
{
    // (this = the +0x80 face): event 0xf leaves back to the main menu; anything
    // else is the routine (false).
    if (MsgId != 0xf)
        return UIPage::MasterWin::OnEvent(MsgId);
    BeginLeave(-1);
    return true;
}

void PageAccountSelect::SetToolTipsEnabled(bool bEnabled)
{
    // .
    AccountId->SetToolTipEnabled(bEnabled);
    SelectServer->SetToolTipEnabled(bEnabled);
    Back->SetToolTipEnabled(bEnabled);
}

void PageAccountSelect::BeginLeave(int Direction)
{
    bLeaving = 1;
    SlideT = 0.0;
    Back->bInteractive = 0;          // +0x498
    SelectServer->bInteractive = 0;
    Back->SetSkipUpload(true);       // (1)
    SelectServer->SetSkipUpload(true);
    HideWin3D(AccountId);
    HideWin3D(Explain);
    PlayUiSoundSlot(0x1b);           // (0x1b)
    SetToolTipsEnabled(false);       // (0), inline at 0x00573167 / 0x0057340e
    LeaveDirection = Direction;
}

PageAccountSelect& GetPageAccountSelect()
{
    return G_PageAccountSelect;
}
