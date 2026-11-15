// Author: Giperion.

#include "nn_mission_message.h"

#include "character_select_equip.h" // GetPlayerEquipDescListbaseline (the routine),
                                    // FindPlayerNonMissionEquipbaseline (the routine)
#include "client_gameobj.h"         // FClientGameobj
#include "client_objdb.h"           // ClientObjDb_FindObject (the routine)
#include "globals.h"                // GetClientFrameTimeCapped (the routine)
#include "hud_target.h"             // GetShipPilotName (the routine), FHudWideText
#include "ids_rdl.h"                // RdlAppendNode, AppendTextMaybeXmlRdl (the routine)
#include "input_manager.h"          // Get/SetClientMouseCursorDrawFlag, MouseSetCursor
#include "login_mgr.h"              // G_Server (the field)
#include "main_command_handler.h"   // SetClientQuitRequested (the routine)
#include "misc_util.h"              // FmtStrSprintf (the routine)
#include "nav_bar.h"                // NavBarCloseService (the routine), NavBarHasServiceWindow (the routine)
#include "nn_inventory.h"           // IsPlayerDockedbaseline (the routine)
#include "nn_main_button.h"         // NN_MainButton::ResetPanel (the routine)
#include "nn_modal.h"               // G_NnMissionMessage, NnMissionMessageDestroy, G_bNnScreenUp,
                                    // NnModalPauseGame / ResumeGame, NnMainButtonOnModalOpen,
                                    // NnMainButtonHasActivePanel, NnFloatBits
#include "nn_text_scratch.h"        // G_NnTextScratchB (the field), the field
#include "player_behavior.h"        // GetPlayerObjRW (the routine)
#include "player_spawn.h"           // GetPlayerConnectionId (the field)
#include "rich_text_win.h"
#include "star_sys.h"               // GetAttitudeClass (the routine)
#include "win3d.h"
#include "win_factory.h"

#include "common_cobject.h"         // CObject::ObjectClass
#include "common_goods.h"           // Arch2Good / Good2Arch, EquipDesc, CBase
#include "common_iface.h"           // IObjRW, IObjAffect::slot_38
#include "common_server.h"          // IServer::PopUpDialog (slot 74)

#include <list>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>
#include <windows.h>                // CharUpperW (the routine)

// ---------------------------------------------------------------------------------------------
// The builders' state. Only NN_Messaging.cpp touches these (raw XREFs: the field 0x0048a1ab /
// / 0x0048af4b; the field 0x0048a1a6 / 0x0048aae0 / 0x0048ae78; the field
// and the four bit tests; the field 0x0048a2e5 / 0x0048a402 / 0x0048a4ce /
// ).

namespace
{
    unsigned G_NnMissionMessageGood = 0;      // the scanned good (kind 4)
    unsigned G_NnMissionMessageScanner = 0;   // the scanning object's id
    unsigned G_NnMissionMessageButtons = 0;   // kind 6: bit 0 yes, 1 no,
                                              // 2 later, 3 ok
    int G_NnMissionMessageKind = 0;           // 4 or 6, read by Create

    // (static init the branch, atexit the branch ~FmtStr) -- kind 6's title.
    FmtStr G_NnMissionMessageTitle;
    // (static init the branch, atexit the branch ~FmtStr) -- the text.
    FmtStr G_NnMissionMessageText;

    int const kKindScanNotice = 4;
    int const kKindPopUp = 6;

    char const kFrameMesh[] = "Interface\\NeuroNet\\Inventory\\CHAR_infowindow.cmp";
    char const kCloseMesh[] = "Interface\\NeuroNet\\Inventory\\x.3db";
    char const kSelectSound[] = "ui_select_item";
    char const kButtonTextStyle[] = "ButtonText";
}

// -> the routine ("NeuroNetMissionMessage"); atexit the branch -> the routine.
WINFACTORY<NeuroNetMissionMessage> G_NeuroNetMissionMessageFactory("NeuroNetMissionMessage");

// ---------------------------------------------------------------------------------------------
// The builders.

void NnMissionMessageShowScanNotice(unsigned ScannerId, unsigned GoodId)
{
    // (cdecl).

    G_NnMissionMessageScanner = ScannerId;
    G_NnMissionMessageGood = GoodId;

    FmtStr Notice;   // [ESP+0x1c]
    int Count = 0;   // [ESP+0x44]

    // a copy of the player's equipment list (the std::list copy ctor
    // through the IAT), searched for the first item whose archetype's good is GoodId; its count
    // is kept. The copy lives until the end of the function (0x0048a2f4..0x0048a34d).
    std::list<EquipDesc> const Cargo(GetPlayerEquipDescListbaseline().Items);
    for (std::list<EquipDesc>::const_iterator It = Cargo.begin(); It != Cargo.end(); ++It)
    {
        if (Arch2Good(It->ArchId) == GoodId)
        {
            Count = It->Count;
            break;
        }
    }

    // mad-lib 0x51e {the good, ids 0x3ca, the count, ids 0x3cb}. The
    // good goes in by reference to the field itself.
    Notice.begin_mad_lib(0x51e);
    Notice.append_good(G_NnMissionMessageGood);
    Notice.append_string(0x3ca);
    Notice.append_int(static_cast<unsigned>(Count));
    Notice.append_string(0x3cb);
    Notice.end_mad_lib();

    // the routine's body inline -- any open message goes.
    NnMissionMessageDestroy();

    // the text, kind 4, and the window.
    G_NnMissionMessageText = Notice;
    G_NnMissionMessageKind = kKindScanNotice;
    WinFactoryCreate("NeuroNetMissionMessage", "NeuroNetMissionMessage", nullptr, nullptr);
}

void NnMissionMessagePopUp(FmtStr const* Title, FmtStr const* Text, unsigned Buttons)
{
    // (cdecl).

    G_NnMissionMessageTitle = *Title;
    G_NnMissionMessageButtons = Buttons;
    // NN_MainButton's open panel dropped at once.
    if (G_NnMainButton != nullptr)
        G_NnMainButton->ResetPanel();
    // the routine's body inline.
    NnMissionMessageDestroy();

    G_NnMissionMessageText = *Text;
    G_NnMissionMessageKind = kKindPopUp;
    WinFactoryCreate("NeuroNetMissionMessage", "NeuroNetMissionMessage", nullptr, nullptr);
}

// ---------------------------------------------------------------------------------------------
// NeuroNetMissionMessage

NeuroNetMissionMessage::NeuroNetMissionMessage()
    : SavedCursorDrawFlag(0)
    , CloseButton(nullptr)
    , Title(nullptr)
    , Text(nullptr)
    , Field_0x33c(0)
    , DropCargoButton(nullptr)
    , DropCargoText(nullptr)
    , CancelButton(nullptr)
    , CancelText(nullptr)
    , YesButton(nullptr)
    , NoButton(nullptr)
    , LaterButton(nullptr)
    , OkButton(nullptr)
    , Kind(0)
    , Elapsed(0.0f)
    , bPausedGame(0)
{
    // Inlined into the routine (0x0048b1f9..0x0048b21d): WinControl ctor (the routine), the
    // vptrs, the field = this, +0x364 = 0 (a dword store), +0x368 = 0. Port-only: every
    // other field is zeroed too -- baseline leaves +0x32c..+0x360 as operator_new left them, and
    // only Create writes the controls its kind uses (SetProperty compares the clicked control
    // against all of them).
    Pad32d[0] = 0;
    Pad32d[1] = 0;
    Pad32d[2] = 0;
    Pad369[0] = 0;
    Pad369[1] = 0;
    Pad369[2] = 0;
    G_NnMissionMessage = this;
}

NeuroNetMissionMessage::~NeuroNetMissionMessage()
{
    // the vptrs, the field = 0; a game this window paused resumes with the
    // cursor flag it saved (the routine); then the WinControl dtor (the routine).
    G_NnMissionMessage = nullptr;
    if (bPausedGame != 0)
    {
        bPausedGame = 0;
        NnModalResumeGame(SavedCursorDrawFlag);
    }
}

bool NeuroNetMissionMessage::Create(Win3D* NewParent, void* Data)
{
    // (RET 8).
    (void)NewParent;

    // the kind; the frame mesh (slot 38) and its half extents
    // (the routine).
    Kind = G_NnMissionMessageKind;
    SetMesh(kFrameMesh, 0);
    WinControlHalfExtentsFromMesh(HalfExtents, &MeshHolder);

    // the pop-up needs the cursor. In space the game also pauses
    // (the routine, undone by the dtor).
    if (Kind == kKindPopUp)
    {
        if (!IsPlayerDockedbaseline())
        {
            SavedCursorDrawFlag = NnModalPauseGame();
            bPausedGame = 1;
        }
        G_bNnScreenUp = true;
        SetClientMouseCursorDrawFlag(1);       // (1)
        MouseSetCursor("Arrow", false);        // ("Arrow", 0)
    }


    DrawSubLayer = 6;
    DrawLayer = 1;

    // every kind but the pop-up has the close box.
    float const ClosePosition[3] = {NnFloatBits(0x3ecd4fdf), NnFloatBits(0x3e72b021), 0.0f};
    float const CloseExtents[3] = {NnFloatBits(0x3c9374bc), NnFloatBits(0x3c9374bc), 0.0f};
    if (Kind != kKindPopUp)
    {
        CloseButton = CreateButtonControl("MissionCloseButton", kCloseMesh, nullptr, kSelectSound,
                                          nullptr, ClosePosition, nullptr, CloseExtents, 0, 0, 0,
                                          0, 0x409, 0, 0);
    }

    // the title -- ids 0x4cf for kinds 3 and 1, none for the scan
    // notice, 0x412 otherwise.
    unsigned TitleIds = 0x412;
    if (Kind == 3 || Kind == 1)
        TitleIds = 0x4cf;
    if (Kind == kKindScanNotice)
        TitleIds = 0;
    float const TitlePosition[3] = {NnFloatBits(0xbed58106), NnFloatBits(0x3e841893), 0.0f};
    Title = CreateTextControl("MissionMessageTitle", "PageHeader", TitlePosition, nullptr,
                              NnFloatBits(0x3f5645a2), NnFloatBits(0x3d75c28f), 1.5f, TitleIds,
                              1, 0, 1);

    // The answer buttons share one size; each caption sits below its button.
    float const AnswerExtents[3] = {NnFloatBits(0x3dcccccd), NnFloatBits(0x3cd4fdf4), 0.0f};
    float const CaptionHeight = NnFloatBits(0x3cf5c28f);
    float const CaptionArg6 = NnFloatBits(0x3ff33333);

    if (Kind == kKindPopUp)
    {
        // the server's title, formatted into the field, upper-cased
        // (the routine -> the routine, the delay-loaded CharUpperW) and set as the title
        // text (property 0x1d).
        FmtStrSprintf(&G_NnMissionMessageTitle, G_NnTextScratchB, G_NnTextScratchCapacity);
        CharUpperW(G_NnTextScratchB);
        Title->SetProperty(0x1d, reinterpret_cast<uintptr_t>(G_NnTextScratchB), 0);

        // bit 0 -- Yes.
        if ((G_NnMissionMessageButtons & 1) != 0)
        {
            float const YesPosition[3] = {NnFloatBits(0xbe6b851f), NnFloatBits(0xbe560419), 0.0f};
            YesButton = CreateWireButtonControl("MMYesButton", kSelectSound, YesPosition,
                                                AnswerExtents, 0, 0, 0x335, 0, 0);
            float const CaptionPosition[3] = {NnFloatBits(0xbea8f5c3), NnFloatBits(0xbe3f7cee), 0.0f};
            CreateTextControl("MMYesText", kButtonTextStyle, CaptionPosition, nullptr,
                              NnFloatBits(0x3e4ac083), CaptionHeight, CaptionArg6, 0x4dc, 1, 0, 1);
        }
        // bit 1 -- No.
        if ((G_NnMissionMessageButtons & 2) != 0)
        {
            float const NoPosition[3] = {0.0f, NnFloatBits(0xbe560419), 0.0f};
            NoButton = CreateWireButtonControl("MMNoButton", kSelectSound, NoPosition, AnswerExtents,
                                               0, 0, 0x336, 0, 0);
            float const CaptionPosition[3] = {NnFloatBits(0xbdc8b439), NnFloatBits(0xbe3f7cee), 0.0f};
            CreateTextControl("MMNoText", kButtonTextStyle, CaptionPosition, nullptr,
                              NnFloatBits(0x3e4ac083), CaptionHeight, CaptionArg6, 0x4dd, 1, 0, 1);
        }
        // bit 2 -- Later.
        if ((G_NnMissionMessageButtons & 4) != 0)
        {
            float const LaterPosition[3] = {NnFloatBits(0x3e6b851f), NnFloatBits(0xbe560419), 0.0f};
            LaterButton = CreateWireButtonControl("MMLaterButton", kSelectSound, LaterPosition,
                                                  AnswerExtents, 0, 0, 0x337, 0, 0);
            float const CaptionPosition[3] = {NnFloatBits(0x3e072b02), NnFloatBits(0xbe3f7cee), 0.0f};
            CreateTextControl("MMLaterText", kButtonTextStyle, CaptionPosition, nullptr,
                              NnFloatBits(0x3e4ac083), CaptionHeight, CaptionArg6, 0x622, 1, 0,
                              1);
        }
        // bit 3 -- Ok, together with the close box after all.
        if ((G_NnMissionMessageButtons & 8) != 0)
        {
            CloseButton = CreateButtonControl("MissionCloseButton", kCloseMesh, nullptr,
                                              kSelectSound, nullptr, ClosePosition, nullptr,
                                              CloseExtents, 0, 0, 0, 0, 0x409, 0, 0);
            float const OkPosition[3] = {NnFloatBits(0xbc449ba6), NnFloatBits(0xbe560419), 0.0f};
            OkButton = CreateWireButtonControl("MMOkButton", kSelectSound, OkPosition, AnswerExtents,
                                               0, 0, 0, 0, 0);
            float const CaptionPosition[3] = {NnFloatBits(0xbddf3b64), NnFloatBits(0xbe3f7cee), 0.0f};
            CreateTextControl("MMOkText", kButtonTextStyle, CaptionPosition, nullptr,
                              NnFloatBits(0x3e47ae14), CaptionHeight, CaptionArg6, 0x411, 1, 0, 1);
        }
    }

    if (Kind == kKindScanNotice)
    {
        // Drop Cargo and Cancel.
        float const Extents[3] = {NnFloatBits(0x3e083127), NnFloatBits(0x3cd4fdf4), 0.0f};
        float const DropPosition[3] = {NnFloatBits(0xbe3c6a7f), NnFloatBits(0xbe560419), 0.0f};
        DropCargoButton = CreateWireButtonControl("DropCargoButton", kSelectSound, DropPosition,
                                                  Extents, 0, 0, 0x338, 0, 0);
        float const DropTextPosition[3] = {NnFloatBits(0xbea04189), NnFloatBits(0xbe3f7cee), 0.0f};
        DropCargoText = CreateTextControl("DropCargoText", kButtonTextStyle, DropTextPosition,
                                          nullptr, NnFloatBits(0x3e851eb8), CaptionHeight,
                                          CaptionArg6, 0x51f, 1, 0, 1);
        float const CancelPosition[3] = {NnFloatBits(0x3e3851ec), NnFloatBits(0xbe560419), 0.0f};
        CancelButton = CreateWireButtonControl("DCCancelButton", kSelectSound, CancelPosition,
                                               Extents, 0, 0, 0x63e, 0, 0);
        float const CancelTextPosition[3] = {NnFloatBits(0x3d4ccccd), NnFloatBits(0xbe3f7cee), 0.0f};
        CancelText = CreateTextControl("DCCancelText", kButtonTextStyle, CancelTextPosition,
                                       nullptr, NnFloatBits(0x3e851eb8), CaptionHeight,
                                       CaptionArg6, 0x63d, 1, 0, 1);

        // when the scanner is still there and is a ship-class object
        // ((class & 3) == 3), its pilot name becomes the title (property 0x1d).
        FClientGameobj* const Scanner = ClientObjDb_FindObject(G_NnMissionMessageScanner, 0);
        if (Scanner != nullptr)
        {
            CBase const* const Base = Scanner;
            CObject const* const Object = Base->Object;
            if (Object != nullptr && (static_cast<unsigned>(Object->ObjectClass) & 3u) == 3u)
            {
                // [ESP+0x18], an EH-tracked local (state at [ESP+0x44]): its buffer is freed on
                // the way out (0x0048ab5d..0x0048ab70) and by the unwind funclet.
                struct FPilotName
                {
                    FHudWideText Name;
                    ~FPilotName()
                    {
                        if (Name.Text != nullptr)
                            free(Name.Text);
                    }
                };
                FPilotName Pilot = {{nullptr, 0}};
                GetShipPilotName(Object, &Pilot.Name);
                if (Pilot.Name.Text != nullptr && Pilot.Name.Text[0] != 0)
                {
                    wcscpy(G_NnTextScratchB, Pilot.Name.Text);
                    Title->SetProperty(0x1d, reinterpret_cast<uintptr_t>(G_NnTextScratchB), 0);
                }
            }
        }
    }

    // the message body.
    float const TextOffset[3] = {NnFloatBits(0xbec7ae14), NnFloatBits(0x3e8d4fdf),
                                 NnFloatBits(0xbfa4dd2f)};
    Text = CreateRichTextWin("MissionMessageText", NnFloatBits(0x3f451eb8),
                             NnFloatBits(0x3ebd70a4), TextOffset, true);

    // the text formatted into the field and read into a display list
    // after a StyleNode 0x9ca4 (the clone-into-list idiom: a clone of the node, the list takes
    // a clone of that, the first clone is deleted).
    FmtStrSprintf(&G_NnMissionMessageText, G_NnTextScratchB, G_NnTextScratchCapacity);
    RenderDisplayList Body; // [ESP+0x30]
    {
        StyleNode const Style(0x9ca4);
        RDLNode* const Temp = Style.Clone();
        RdlAppendNode(Body, Temp->Clone());
        delete Temp;
    }
    AppendTextMaybeXmlRdl(G_NnTextScratchB, static_cast<unsigned>(wcslen(G_NnTextScratchB)),
                          Body, false);

    // the body text starts at the top with the new content.
    Text->SetScrollOrigin(0, 0);
    Text->ClearContent();
    Text->AppendContent(Body);

    // slot 31 SetOffset(0, 0.076, -1.288), slot 5 SetPosition on its
    // own +0x60.
    SetOffset(0.0f, NnFloatBits(0x3d9ba5e3), NnFloatBits(0xbfa4dd2f));
    SetPosition(Position);

    // NN_MainButton's panel closes (the routine), the NavBar
    // collapses (the routine(1)).
    NnMainButtonOnModalOpen();
    NavBarCloseService(true);

    // the routine with the window's OWN +0x04 parent, not the one
    // Create was given (the factory is always called with null, so it is a root window); the
    // result is returned once the display list is freed (0x0048ad25..0x0048adce).
    return Win3D::Create(Parent, Data);
}

void NeuroNetMissionMessage::Update()
{
    // .
    // the routine (the Win3D child walk).
    Win3D::Update();

    // unless this window paused the game, another NeuroNet panel or a
    // base-service window takes it down -- a pop-up answers "later" (4) first.
    if (bPausedGame == 0 &&
        (NnMainButtonHasActivePanel() || NavBarHasServiceWindow())) // / the routine
    {
        if (Kind == kKindPopUp)
            G_Server->PopUpDialog(GetPlayerConnectionId(), 4);
        Win3DDeleteTree(this); // children, slot 10, slot 0 with 1
        return;
    }

    // the scan notice counts its 20 seconds. A scanner that is gone,
    // not a ship-class object, or hostile to the player (the routine == -1) sets the count to
    // 20 at once, so any running frame time closes it.
    if (Kind != kKindScanNotice)
        return;
    IObjRW const* const Player = static_cast<IObjRW const*>(GetPlayerObjRW());
    FClientGameobj* const Scanner = ClientObjDb_FindObject(G_NnMissionMessageScanner, 0);
    bool bScannerOk = false;
    if (Scanner != nullptr && Player != nullptr)
    {
        CBase const* const Base = Scanner;
        CObject const* const Object = Base->Object;
        if (Object != nullptr && (static_cast<unsigned>(Object->ObjectClass) & 3u) == 3u &&
            GetAttitudeClass(Player, Scanner) != -1)
        {
            bScannerOk = true;
        }
    }
    if (!bScannerOk)
        Elapsed = 20.0f;

    // FADD / FST keeps the double sum on the stack for the compare
    // against 20.0f (the field); TEST AH,0x41 + JNZ skips when the sum is not greater.
    double const Sum = GetClientFrameTimeCapped() + static_cast<double>(Elapsed);
    Elapsed = static_cast<float>(Sum);
    if (Sum > 20.0)
        Win3DDeleteTree(this);
}

void NeuroNetMissionMessage::Draw()
{
    // drawn only while the cursor is drawn (the routine).
    if (GetClientMouseCursorDrawFlag() != 0)
        WinControl::Draw(); // JMP the routine
}

intptr_t NeuroNetMissionMessage::SetProperty(int Msg, uintptr_t A, uintptr_t B)
{
    // (RET 0xc). Nothing reaches WinControl's slot 42.
    (void)B;
    if (Msg != 0x26)
        return 0;

    WinControl* const Clicked = reinterpret_cast<WinControl*>(A);
    if (Clicked == DropCargoButton)
    {
        // jettison all of the scanned good -- the player's first
        // non-mission item of its archetype, when it is not mounted: IObjAffect slot 14
        // (+0x38) with (id, count, the field).
        unsigned const Arch = Good2Arch(G_NnMissionMessageGood);
        if (Arch != 0)
        {
            EquipDesc* const Desc = FindPlayerNonMissionEquipbaseline(Arch);
            IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW());
            if (Desc != nullptr && !Desc->bEquipped && Player != nullptr)
            {
                IObjAffect* const Affect = Player;
                Affect->slot_38(Desc->Id, Desc->Count, static_cast<int>(GetPlayerConnectionId()));
            }
        }
    }
    else if (Clicked == CancelButton)
    {
        // just close.
    }
    else if (Clicked == YesButton)
    {

        G_Server->PopUpDialog(GetPlayerConnectionId(), 1);
        G_bNnScreenUp = false;
    }
    else if (Clicked == NoButton)
    {

        G_Server->PopUpDialog(GetPlayerConnectionId(), 2);
        G_bNnScreenUp = false;
    }
    else if (Clicked == LaterButton)
    {

        G_Server->PopUpDialog(GetPlayerConnectionId(), 4);
        G_bNnScreenUp = false;
    }
    else if (Clicked == CloseButton || Clicked == OkButton)
    {
        // the kind is read from the field, not +0x360. A pop-up
        // answers "close" (8); kinds 0 and 5 (no builder sets either) end the game loop
        // (the routine).
        int const CurrentKind = G_NnMissionMessageKind;
        G_bNnScreenUp = false;
        if (CurrentKind == kKindPopUp)
        {
            G_Server->PopUpDialog(GetPlayerConnectionId(), 8);
            G_bNnScreenUp = false;
        }
        else if (CurrentKind == 0 || CurrentKind == 5)
        {
            SetClientQuitRequested(true); // the field = 1
        }
    }
    else
    {

        return 0;
    }

    // children (the routine each), slot 10, slot 0 with 1; handled.
    Win3DDeleteTree(this);
    return 1;
}
