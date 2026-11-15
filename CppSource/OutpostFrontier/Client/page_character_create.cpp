// Author: Giperion.

#include "page_character_create.h"

#include "chat_init.h"            // ChatGetBaseName (the routine), ChatGetShipArchName (the routine)
#include "engine_character.h"     // FEngineCharacterBody
#include "globals.h"              // GetClientFrameTimeCapped (the routine)
#include "ids_rdl.h"              // RdlClear, LoadIdsIntoRdl (the routine), RdlRemoveLastParagraph
#include "intro_page_util.h"
#include "login_mgr.h"            // G_LoginMgr (the field)
#include "menu_ui_init.h"         // PlayUiSoundSlot (the routine), PlayOrRewindUiSoundSlot (5645c0)
#include "nn_error.h"             // NnErrorRunModal (the routine)
#include "page_character_select.h" // G_CharacterCreateSelection (the field)
#include "space_camera.h"         // AsCamera, SpaceCameraSetHalfFovX (the routine)
#include "space_scene_obj_draw.h" // SpaceCameraSetNearFar (the routine)
#include "tooltip_hover.h"        // FToolTipHoverInfo
#include "ui_rich_text_win.h"
#include "ui_text_edit.h"
#include "ui_text_msg_button.h"
#include "ui_win_deform.h"
#include "ui_win_object.h"
#include "win3d.h"                // Win3DGetCamera (the routine), Win3DGetMouseRay (the routine)
#include "win3d_immediate.h"      // Win3DVec3Add (the routine)

#include "common_id.h"            // CreateID, ID_String
#include "common_newchar.h"       // NewChar::TheDB, FNewCharPackage, FNewCharPilot
#include "common_server_types.h"  // SCreateCharacterInfo
#include "common_text.h"
#include "common_universe.h"      // Archetype::GetShipID
#include "neuroncomponents.h"                // FDUMP

#include <stdint.h>               // uintptr_t
#include <string.h>
#include <wchar.h>                // wcscmp, wcscpy
#include <sstream>
#include <string>
#include <vector>

using namespace IntroPageUtil;

namespace
{
    // .
    PageCharacterCreate G_PageCharacterCreate(&G_CharacterCreateSelection.Info);

    // .rdata 0x005e1b1c -- the FDUMP source name.
    char const kCharCreateCpp[] = "Client\\Interface\\CharCreate.cpp";

    // .rdata 0x005e1b7c.
    wchar_t const kHigeneko[] = L"higeneko";
    // .data 0x00614344 -- the same word in hiragana (U+3072 U+3052 U+306D U+3053).
    wchar_t const kHigenekoKana[] = L"\x3072\x3052\x306d\x3053";

    // .data 0x00614338 -- the characters the name box refuses. Create passes a count of 5, so
    // the terminator is in the set too.
    wchar_t const kNameFilter[] = L" \t\r\n";

    // .rdata 0x005cef90 -- the name box's prefix text.
    wchar_t const kSpace[] = L" ";

    // The eight caption loads of Create (0x00560423..0x005605eb): the list emptied (the routine
    // over begin..end), the IDS text loaded (the routine, its result not tested) and the
    // trailing paragraph cut (the routine) whatever the load did.
    void LoadIdsAlwaysCut(RenderDisplayList& Rdl, unsigned Ids)
    {
        RdlClear(Rdl);
        LoadIdsIntoRdl(Ids, Rdl);
        RdlRemoveLastParagraph(Rdl);
    }

    void AppendStyle(UiRichTextWin* Win, unsigned short Style)
    {
        StyleNode const Node(Style);
        Win->AppendNode(Node);
    }

    void AppendJustify(UiRichTextWin* Win, TextJustify Mode)
    {
        JustifyNode const Node(Mode);
        Win->AppendNode(Node);
    }

    void AppendRef(UiRichTextWin* Win, RenderDisplayList& Rdl)
    {
        RDLRefNode const Node(Rdl);
        Win->AppendNode(Node);
    }

    // ParagraphNode's vftable written into a stack slot and handed to the routine.
    void AppendParagraph(UiRichTextWin* Win)
    {
        ParagraphNode const Node;
        Win->AppendNode(Node);
    }

    void SetButtonOffset(UiTextMsgButton* Button, float const* Offset)
    {
        // WinControl slot 42 (+0xa8) SetProperty(6, offset, 0).
        Button->SetProperty(6, reinterpret_cast<uintptr_t>(Offset), 0);
    }
}

PageCharacterCreate& GetPageCharacterCreate()
{
    return G_PageCharacterCreate;
}

// ---------------------------------------------------------------------------------------------

PageCharacterCreate::PageCharacterCreate(SCreateCharacterInfo* NewSelection)
    : UIPage::MasterWin(Desc.GetPageData())
    , Result(0)
    , Selection(NewSelection)
    , SelectionFactionIndex(&G_CharacterCreateSelection.FactionIndex)
    , SlideT(0.0)
    , bLeaving(0)
    , LeaveDirection(0)
    , DataFrameHome{0.0f, 0.0f, 0.0f}
    , DataFrame(nullptr)
    , BaseCostume(nullptr)
    , Back(nullptr)
    , SelectShip(nullptr)
    , CharacterName(nullptr)
    , StartingDesc(nullptr)
    , Title(nullptr)
    , NameLabel(nullptr)
{
    // the routine(the routine(+0x8c)), the routine on +0x8c, +0xb8 = the
    // argument, +0xbc = the argument + 0x40 (the pick's faction index, which is what
    // SelectionFactionIndex points at), the lists +0xc0 / +0xcc, +0x100..+0x11c = 0, the lists
    // +0x120..+0x174, +0xb4 = 0, the three vftables. +0xd8..+0xfc are not written; the static
    // object starts zeroed, which is what the initialisers above reproduce.
}

PageCharacterCreate::~PageCharacterCreate()
{
    // the vftables back, the lists +0x174..+0x120 (the routine), +0x11c..+0x100
    // = 0, the lists +0xcc / +0xc0 (inlined), the routine on +0x8c, the routine -- the member
    // destructors in reverse declaration order, then the base.
}

int PageCharacterCreate::GetResult() const
{
    // .
    return Result;
}

bool PageCharacterCreate::Load(char const* FullPath)
{
    // the routine on +0x8c; true -> slot 9 (0, 0), MOV AL,1.
    if (!Desc.LoadFromFile(FullPath))
        return false;
    Create(nullptr, nullptr);
    return true;
}

bool PageCharacterCreate::HasResult() const
{
    // .
    return Result != 0;
}

void PageCharacterCreate::Destroy()
{
    // the routine(0xf) on the +0x80 face, then JMP the routine.
    UnregisterEvent(0xf);
    UIPage::MasterWin::Destroy();
}

bool PageCharacterCreate::Create(Win3D* NewParent, void* /*Payload*/)
{
    // . MasterWin::Create (the routine) with no payload; its result is not tested.
    UIPage::MasterWin::Create(NewParent, nullptr);
    Result = 0;

    // the widgets through the IUIPage face (slot 4), in baseline's order.
    DataFrame = static_cast<UiWinObject*>(FindWidget("DataFrame"));
    BaseCostume = static_cast<UiWinDeform*>(FindWidget("BaseCostume"));
    Back = static_cast<UiTextMsgButton*>(FindWidget("Back"));
    SelectShip = static_cast<UiTextMsgButton*>(FindWidget("SelectShip"));
    CharacterName = static_cast<UiTextEdit*>(FindWidget("CharacterName"));
    StartingDesc = static_cast<UiRichTextWin*>(FindWidget("StartingDesc"));
    Title = static_cast<UiRichTextWin*>(FindWidget("Title"));
    NameLabel = static_cast<UiRichTextWin*>(FindWidget("NameLabel"));

    RegisterEvent(0xf); // the routine

    // the ToolTip IDS of three controls (slot 46 +0xb8, the record's
    // +0x04).
    CharacterName->GetToolTipHoverInfo()->IdsAlt = 0xad8;
    Back->GetToolTipHoverInfo()->IdsAlt = 0xadc;
    SelectShip->GetToolTipHoverInfo()->IdsAlt = 0xadd;

    // the captions.
    LoadIdsAlwaysCut(BackText, 0x60029);
    LoadIdsAlwaysCut(Text60028, 0x60028);
    LoadIdsAlwaysCut(NameLabelText, 0x6006c);
    LoadIdsAlwaysCut(BaseLabelText, 0x6006b);
    LoadIdsAlwaysCut(ShipLabelText, 0x6006e);
    LoadIdsAlwaysCut(Text6006a, 0x6006a);
    LoadIdsAlwaysCut(MoneyLabelText, 0x60063);
    LoadIdsAlwaysCut(TitleText, 0x60034);

    // NameLabel (not emptied first) -- centred, style 0x9ca4, its caption.
    AppendJustify(NameLabel, TextJustifyCenter);
    AppendStyle(NameLabel, 0x9ca4);
    AppendRef(NameLabel, NameLabelText);

    // Title emptied (the routine), style 0x1df5, its caption.
    Title->ClearContent();
    AppendStyle(Title, 0x1df5);
    AppendRef(Title, TitleText);

    // the preview body's pose -- Z -6.0f (0xc0c00000), height -0.9f
    // (0xbf666666).
    BaseCostume->PoseZ = -6.0f;
    BaseCostume->PoseHeight = -0.9f;

    // the preview camera's half horizontal fov scaled from the scene
    // camera's (the field) by the ratio of the viewport widths (its own through slot 5 +0x14,
    // the scene's the field - the field): FILD width, FMUL fov, FIDIV scene width,
    // FSTP float -> the routine. Then the routine(1.0f, 100.0f).
    FSpaceCamera* const Scene = Win3DGetCamera();
    int const SceneWidth = Scene->Viewport[2] - Scene->Viewport[0];
    FSpaceCamera* const Preview = &BaseCostume->Camera;
    int const* const PreviewRect = AsCamera(Preview)->GetViewport();
    int const PreviewWidth = PreviewRect[2] - PreviewRect[0];
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
    // Port-only Hor+: the scene's +0x98 is its angle on a 4:3 surface, so the width it spans
    // is the scene width over the Hor+ factor.
    SpaceCameraSetHalfFovX(
        Preview, static_cast<float>(static_cast<double>(PreviewWidth) *
                                    static_cast<double>(Scene->HalfFovXDeg) /
                                    (static_cast<double>(SceneWidth) / SpaceCameraHorPlusScale(Scene))));
#else
    SpaceCameraSetHalfFovX(
        Preview, static_cast<float>(static_cast<double>(PreviewWidth) *
                                    static_cast<double>(Scene->HalfFovXDeg) /
                                    static_cast<double>(SceneWidth)));
#endif
    SpaceCameraSetNearFar(Preview, 1.0f, 100.0f);

    // Back's text list emptied (the routine on +0x4b4), a reference to
    // BackText cloned into it (the routine, the first clone freed by the routine), then
    // .
    RdlClear(Back->TextBodyList);
    {
        RDLRefNode const Node(BackText);
        AppendNodeClone(Back->TextBodyList, Node);
    }
    Back->ApplyCaptionLayout();

    // the routine(1).
    StartingDesc->SetAutoScrollbar(true);

    // the routine(1).
    ShowSelection(true);

    // the name box -- focus (the routine), at most 0x17 characters
    // (the routine), " \t\r\n" and its terminator refused (the routine(.., 5)), a fixed style
    // (the routine(1)).
    CharacterName->Activate();
    CharacterName->SetMaxLength(0x17);
    CharacterName->AddFilterChars(kNameFilter, 5);
    CharacterName->SetFixedStyle(true);

    // StyleNode(0x9ca4) cloned into the fixed-style list (+0x4fc).
    {
        StyleNode const Node(0x9ca4);
        AppendNodeClone(CharacterName->FixedStyle, Node);
    }
    // JustifyNode(1) cloned into the prefix list (+0x4dc).
    {
        JustifyNode const Node(TextJustifyCenter);
        AppendNodeClone(CharacterName->Prefix, Node);
    }
    // TextNode(L" ", -1) cloned into the prefix list.
    {
        TextNode const Node(kSpace, -1);
        AppendNodeClone(CharacterName->Prefix, Node);
    }

    // a frame in the chrome colour (the routine(1), the routine
    // (&the field)), alignment 2 (the routine), the routine(0).
    CharacterName->SetDrawFrame(true);
    CharacterName->SetFrameColor(ChromeColor());
    CharacterName->SetAlignMode(2);
    CharacterName->SetLayoutFlag(false);

    // slide from -1.0 (0xbff00000), no leave, the frame's home.
    SlideT = -1.0;
    LeaveDirection = 0;
    bLeaving = 0;
    DataFrameHome[0] = DataFrame->Position[0];
    DataFrameHome[1] = DataFrame->Position[1];
    DataFrameHome[2] = DataFrame->Position[2];

    // five controls hidden until the slide ends.
    HideWin3D(CharacterName);
    HideWin3D(BaseCostume);
    HideWin3D(NameLabel);
    HideWin3D(StartingDesc);
    HideWin3D(Title);

    // the routine(1) on both buttons.
    Back->SetSkipUpload(true);
    SelectShip->SetSkipUpload(true);
    return true;
}

void PageCharacterCreate::ShowSelection(bool bWithCostume)
{
    // (RET 4).
    // the pick's name into the name box (the routine, the routine
    // with no attributes).
    CharacterName->ClearText();
    CharacterName->SetText(Selection->Name, nullptr);

    // StartingDesc emptied; the base caption and the home base's name
    // (the routine on the pick's +0x34).
    StartingDesc->ClearContent();
    AppendStyle(StartingDesc, 0x4433);
    AppendRef(StartingDesc, BaseLabelText);
    AppendParagraph(StartingDesc);
    AppendStyle(StartingDesc, 0x9ca4);
    AppendJustify(StartingDesc, TextJustifyCenter);
    AppendRef(StartingDesc, *ChatGetBaseName(Selection->Field_0x34));
    AppendParagraph(StartingDesc);
    AppendParagraph(StartingDesc);

    // NewChar::TheDB slot 1 (+0x04) with the pick's package.
    FNewCharPackage const* const Package = NewChar::TheDB->FindPackage(Selection->Field_0x38);
    if (Package != nullptr)
    {
        // the money caption, then the package's money through a
        // basic_stringstream<unsigned short> (operator<< and ends, str() into a TextNode(.., -1)).
        AppendStyle(StartingDesc, 0x4433);
        AppendRef(StartingDesc, MoneyLabelText);
        AppendParagraph(StartingDesc);
        AppendStyle(StartingDesc, 0x9ca4);
        AppendJustify(StartingDesc, TextJustifyCenter);
        {
            std::wstringstream Money;
            Money << Package->Money << std::ends;
            {
                std::wstring const Text = Money.str();
                TextNode const Node(Text.c_str(), -1);
                StartingDesc->AppendNode(Node);
            }
            AppendParagraph(StartingDesc);
            AppendParagraph(StartingDesc);
        }

        // the ship caption and the ship's name (CreateID on the
        // package's ship string, Archetype::GetShipID, the routine).
        AppendStyle(StartingDesc, 0x4433);
        AppendRef(StartingDesc, ShipLabelText);
        AppendParagraph(StartingDesc);
        AppendStyle(StartingDesc, 0x9ca4);
        AppendJustify(StartingDesc, TextJustifyCenter);
        ID_String ShipNickname;
        ShipNickname.Id = CreateID(Package->Ship.c_str());
        AppendRef(StartingDesc, *ChatGetShipArchName(Archetype::GetShipID(ShipNickname)));
        AppendParagraph(StartingDesc);
        AppendParagraph(StartingDesc);

        // the package description list emptied (the routine per node)
        // and loaded from strid_desc (the record's +0x08); a failed load ends the text here.
        RdlClear(PackageDescText);
        if (LoadIdsIntoRdl(static_cast<unsigned>(Package->StridDesc), PackageDescText))
        {
            // style 0x9ca4, left-justified, the description.
            AppendStyle(StartingDesc, 0x9ca4);
            AppendJustify(StartingDesc, TextJustifyLeft);
            AppendRef(StartingDesc, PackageDescText);
        }
    }

    // the body only when asked.
    if (!bWithCostume)
        return;

    // NewChar::TheDB slot 2 (+0x08) with the pick's pilot; none ->
    // FDUMP (0x100002, line 0x19d).
    FNewCharPilot const* const Pilot = NewChar::TheDB->FindPilot(Selection->Field_0x3c);
    if (Pilot == nullptr)
    {
        FDUMP(0x100002, "%s(%d) : WARNING:General:%s", kCharCreateCpp, 0x19d,
              "Failed to retrieve pilot information.\n");
        return;
    }

    // the pilot's body costume (record +0x04, its c_str -- [EBP+8] or
    // _Nullstr) on BaseCostume (the routine).
    BaseCostume->SetCostume(Pilot->Body.c_str());

    // every body.anim script (record +0x14, 0x10-byte strings, each
    // through its c_str) started (the routine with 0, 0, 1.0f, 2).
    FEngineCharacterBody& Body = BaseCostume->Body;
    for (std::vector<std::string>::const_iterator It = Pilot->BodyAnims.begin();
         It != Pilot->BodyAnims.end(); ++It)
    {
        Body.PlayAnimation(It->c_str(), 0, 0.0f, 1.0f, 2);
    }

    // 0.1f (0x3dcccccd) of animation in steps of at most 0.2f
    // (the field; FCOMP, TEST AH,5 / JP -> 0.2f when Remaining >= 0.2f or unordered). The
    // step body is the routine's (with a deformable: MixerWalk and IEngine slot 27 on the root
    // bone the routine with Step * AnimRate, the playback slots emptied when the routine finds
    // none running). Remaining -= Step (FST float), FCOMP double 0.0, TEST AH,0x41 / JZ -> loop
    // while the difference is > 0.
    float Remaining = 0.1f;
    for (;;)
    {
        float const Step = (Remaining < 0.2f) ? Remaining : 0.2f;
        Body.Advance(Step);
        double const Left = static_cast<double>(Remaining) - static_cast<double>(Step);
        Remaining = static_cast<float>(Left);
        if (!(Left > 0.0))
            break;
    }

    // the routine's body (slot 9, the used slots emptied).
    Body.StopAnimations();

    // the body.anim scripts started again.
    for (std::vector<std::string>::const_iterator It = Pilot->BodyAnims.begin();
         It != Pilot->BodyAnims.end(); ++It)
    {
        Body.PlayAnimation(It->c_str(), 0, 0.0f, 1.0f, 2);
    }
}

void PageCharacterCreate::ReadNameIntoSelection()
{
    // (pick, 0x18): the typed name into the pick.
    CharacterName->GetText(Selection->Name, 0x18);
    // wcscmp with L"higeneko": equal -> wcscpy the hiragana over it, and the box refilled
    // (the routine, the routine(pick, 0)).
    if (wcscmp(Selection->Name, kHigeneko) == 0)
    {
        wcscpy(Selection->Name, kHigenekoKana);
        CharacterName->ClearText();
        CharacterName->SetText(Selection->Name, nullptr);
    }
}

void PageCharacterCreate::Confirm()
{
    // . 0x00561563..0x005615c4.
    ReadNameIntoSelection();

    // an empty name does nothing.
    if (Selection->Name[0] == L'\0')
        return;

    // the routine on the field.
    int const Code = G_LoginMgr.CreateNewCharacter(*Selection);
    if (Code == 1)
    {
        // created -- direction 1, then the leave (tail call).
        LeaveDirection = 1;
        bLeaving = 1;
        BeginLeave();
        return;
    }
    if (Code == 4)
    {
        // refused -- NN_Error with IDS 0x738, the page stays.
        NnErrorRunModal(0x738, false, nullptr);
        return;
    }
    // anything else -- direction 2, then the leave (tail call).
    LeaveDirection = 2;
    bLeaving = 1;
    BeginLeave();
}

bool PageCharacterCreate::Slot23(unsigned Msg, unsigned A, unsigned /*B*/)
{
    // (RET 0xc).
    bool bBack = false;
    bool bConfirm = false;
    if (Msg == 0x3004)
    {
        // a button click -- the routine(0), the name read back.
        PlayOrRewindUiSoundSlot(0);
        ReadNameIntoSelection();
        // 0x300c Back, 0x300d SelectShip, anything else false.
        bBack = (A == 0x300c);
        bConfirm = (A == 0x300d);
    }
    else
    {
        // 0xf003 back, 0xf004 confirm, anything else false.
        bBack = (Msg == 0xf003);
        bConfirm = (Msg == 0xf004);
    }

    if (bBack)
    {
        // direction -1, the routine, true.
        LeaveDirection = -1;
        BeginLeave();
        return true;
    }
    if (bConfirm)
    {
        // the routine, true.
        Confirm();
        return true;
    }
    return false;
}

bool PageCharacterCreate::OnEvent(unsigned MsgId)
{
    // (RET 4, this = the +0x80 face): 0xf -> +0xe4 = -1 (the face's +0x64),
    // , true; anything else JMP the routine (XOR AL,AL / RET 4).
    if (MsgId == 0xf)
    {
        LeaveDirection = -1;
        BeginLeave();
        return true;
    }
    return false;
}

bool PageCharacterCreate::MouseEvent(FWin3DMouseEvent const* Event)
{
    // (RET 4): a press (Field1 == 2) drops the name box's focus (the routine(0))
    // and gives it back when the mouse ray (the routine) hits the box (slot 4). Always false.
    if (Event->Field1 == 2)
    {
        CharacterName->Deactivate(false);
        float Ray[3];
        if (CharacterName->HitTest(Win3DGetMouseRay(Ray)))
            CharacterName->Activate();
    }
    return false;
}

void PageCharacterCreate::BeginLeave()
{
    // .
    bLeaving = 1;
    SlideT = 0.0;

    // five controls hidden.
    HideWin3D(CharacterName);
    HideWin3D(BaseCostume);
    HideWin3D(NameLabel);
    HideWin3D(StartingDesc);
    HideWin3D(Title);

    // both buttons stop taking clicks (+0x498).
    Back->bInteractive = 0;
    SelectShip->bInteractive = 0;

    // the routine(1) on both buttons, the routine(0) on the name box
    // and both buttons.
    Back->SetSkipUpload(true);
    SelectShip->SetSkipUpload(true);
    CharacterName->SetToolTipEnabled(false);
    Back->SetToolTipEnabled(false);
    SelectShip->SetToolTipEnabled(false);

    // the routine(0x1b).
    PlayUiSoundSlot(0x1b);
}

void PageCharacterCreate::Update()
{
    // .
    // SelectShip's bit 0 follows whether the name box's text list
    // (+0x4e8) holds anything (RenderDisplayList::get_element_count), and its +0x498 copies it.
    unsigned char const OldFlags = SelectShip->Flags;
    unsigned char const bHasName = (CharacterName->TextDisplay.get_element_count() != 0) ? 1 : 0;
    SelectShip->Flags = static_cast<unsigned char>((OldFlags & 0xfe) | bHasName);
    SelectShip->bInteractive = static_cast<unsigned char>(SelectShip->Flags & 1);

    // FCOMP 1.0 (the field), TEST AH,0x41 / JP -> skip the slide-in
    // when SlideT > 1.0 or unordered; the fall-through (<= 1.0) goes to the leave when +0xe0 is
    // set.
    if (SlideT <= 1.0 && bLeaving == 0)
    {
        // the frame time (the routine); FCOM 0.5 (the field),
        // TEST AH,5 / JP -> no advance when >= 0.5 or unordered; below 0.5 SlideT += Dt * 1.5
        // (the field).
        double const Dt = GetClientFrameTimeCapped();
        if (Dt < 0.5)
            SlideT = Dt * 1.5 + SlideT;

        // FCOMP 0.0, TEST AH,5 / JP -> arrived when SlideT >= 0.0 or
        // unordered; below 0.0 (fall-through) the controls slide.
        if (SlideT < 0.0)
        {
            // Back by {SlideT, 0, 0}, SelectShip by {-SlideT, 0, 0}
            // (slot 42 property 6), DataFrame at home + {0, SlideT, 0} (the routine, slot 5).
            float const BackOffset[3] = {static_cast<float>(SlideT), 0.0f, 0.0f};
            SetButtonOffset(Back, BackOffset);
            float const ShipOffset[3] = {static_cast<float>(-SlideT), 0.0f, 0.0f};
            SetButtonOffset(SelectShip, ShipOffset);
            float const FrameOffset[3] = {0.0f, static_cast<float>(SlideT), 0.0f};
            float FramePosition[3];
            Win3DVec3Add(FramePosition, DataFrameHome, FrameOffset);
            DataFrame->SetPosition(FramePosition);
        }
        else
        {
            // both buttons back to {0, 0, 0}, the frame home, the five
            // controls shown, the routine(0) on both buttons, the routine(1) on the name box
            // and both buttons, SlideT = 2.0 (0x40000000).
            float const Zero[3] = {0.0f, 0.0f, 0.0f};
            SetButtonOffset(Back, Zero);
            SetButtonOffset(SelectShip, Zero);
            DataFrame->SetPosition(DataFrameHome);
            ShowWin3D(CharacterName);
            ShowWin3D(BaseCostume);
            ShowWin3D(NameLabel);
            ShowWin3D(StartingDesc);
            ShowWin3D(Title);
            Back->SetSkipUpload(false);
            SelectShip->SetSkipUpload(false);
            CharacterName->SetToolTipEnabled(true);
            Back->SetToolTipEnabled(true);
            SelectShip->SetToolTipEnabled(true);
            SlideT = 2.0;
        }

        // FCOMP -0.8 (the field), TEST AH,0x41 / JNZ -> nothing at or
        // below -0.8 (or unordered); FCOMP -0.1 (the field), TEST AH,5 / JP -> nothing at or
        // above -0.1; strictly between, the routine(0x1b).
        if (SlideT > -0.8 && SlideT < -0.1)
            PlayUiSoundSlot(0x1b);
    }

    // the leave runs while +0xe0 is set and a direction is chosen.
    if (bLeaving != 0 && LeaveDirection != 0)
    {
        // SlideT = SlideT - Dt * 1.5 (FSUBR), FST double.
        double const Dt = GetClientFrameTimeCapped();
        SlideT = SlideT - Dt * 1.5;
        float const Slide = static_cast<float>(SlideT);

        // Back by {SlideT, 0, 0}, SelectShip by {-SlideT, 0, 0}.
        float const BackOffset[3] = {Slide, 0.0f, 0.0f};
        SetButtonOffset(Back, BackOffset);
        float const ShipOffset[3] = {static_cast<float>(-SlideT), 0.0f, 0.0f};
        SetButtonOffset(SelectShip, ShipOffset);

        // DataFrame at {home x, SlideT + home y, home z} (slot 5).
        float const FramePosition[3] = {DataFrameHome[0], Slide + DataFrameHome[1],
                                        DataFrameHome[2]};
        DataFrame->SetPosition(FramePosition);

        // FCOMP -0.9 (the field), TEST AH,0x41 / JP -> still sliding
        // when SlideT > -0.9 or unordered; at or below -0.9 (fall-through) the leave ends.
        if (SlideT <= -0.9)
        {
            // CMP 3 / JA: -1 -> state 8, other values above 3 -> none;
            // 3 -> NN_Error 0x738 then state 8; 1 -> state 8; 2 -> state 5. Then +0xe0 = 0.
            unsigned const Direction = static_cast<unsigned>(LeaveDirection);
            if (Direction > 3)
            {
                if (LeaveDirection == -1)
                    Result = 8;
            }
            else if (Direction == 3)
            {
                NnErrorRunModal(0x738, false, nullptr);
                Result = 8;
            }
            else if (Direction == 1)
            {
                Result = 8;
            }
            else if (Direction == 2)
            {
                Result = 5;
            }
            bLeaving = 0;
        }
    }

    // Win3D's slot 11 body inlined (per child: bit0 ? slot 11 : 12).
    Win3D::Update();
}

void PageCharacterCreate::Draw()
{
    // JMP the routine.
    Win3D::Draw();
}
