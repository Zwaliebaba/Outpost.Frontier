// Author: Giperion.

#include "nn_modal.h"

#include "character_select_equip.h" // GetPlayerBaseIdbaseline (the field), GetPlayerStarSystemIdbaseline
#include "client_base_market.h" // GetClientCurrentLocationBaseData (the routine)
#include "globals.h"       // SetClientServerTickPause (the routine), GetClientFrameTimeCapped
#include "hud_comm.h"      // HudCommIsWindowOpen (the routine)
#include "ids_strings.h"   // GetIdsStringWWithRawFallback (the routine)
#include "input_manager.h" // Get/SetClientMouseCursorDrawFlag (the routine / the routine)
#include "menu_universe_base.h" // GetClientCurrentBaseStridName (the routine)
#include "infocard_map.h"  // InfocardMapCreate / InfocardMapDestroy (the routine / the routine)
#include "knowledge_map.h" // KnowledgeMapCreate / KnowledgeMapDestroy (the routine / the routine)
#include "cargo_bar_control.h"
#include "font_string.h"   // FontStringConstruct (the routine)
#include "menu_ui_widgets.h"
#include "nn_rollover.h"   // NnRolloverShowHoverGlobal (the routine)
#include "tooltip_hover.h" // FToolTipHoverInfo, ToolTipHoverReset (the routine)
#include "nav_bar.h"       // NavBarHasServiceWindow (the routine)
#include "nn_arrival_text.h"
#include "nn_inventory.h"  // IsPlayerDockedbaseline (the routine)
#include "nn_main_button.h"
#include "nn_messaging.h"  // NnVoiceInit (the routine)
#include "nn_trade.h"      // G_NnTrade (the field)
#include "nn_story_star.h" // G_MissionObjectiveWindow (the field), StoryStarReset
#include "nn_text_control.h"
#include "nn_text_scratch.h" // G_NnTextScratchA/B, G_NnTextScratchCapacity
#include "nn_ui_pulse.h"   // G_NnPaletteArgb_679b90 (the field)
#include "player_behavior.h" // OnMouseFlightRawMessage (the routine)
#include "room_control.h"  // FRoomRuntime, GetRoomRuntimeRoomId (runtime +0x38)
#include "space_letterbox.h" // IsLetterboxVisible (the routine)
#include "win3d.h"         // Win3DDeleteTree (the routine)
#include "win_factory.h"   // WinFactoryCreate (the routine)

#include "common_goods.h"  // BaseData::get_base_id
#include "common_id.h"     // CreateID, MakeLocationID
#include "common_universe.h"

#include <stdio.h>
#include <wchar.h>
#include <windows.h>

bool G_bNnScreenUp = false;
NN_MainButton* G_NnMainButton = nullptr;
NnArrivalText* G_NnArrivalText = nullptr;

// WINFACTORY<NN_ArrivalText> static (the branch -> the routine; Create
// ; atexit the branch -> the routine).
WINFACTORY<NnArrivalText> G_NnArrivalTextFactory("NN_ArrivalText");

namespace
{
    // set by the static initializer the branch to
    // CreateID("click_on_characters_and_buttons"); only reader the routine (0x0045a37e).
    unsigned const G_NnVoiceClickOnCharactersId = CreateID("click_on_characters_and_buttons");

    // ("NN_ArrivalText", "NN_ArrivalText", 0, 0) when the field is null, then the
    // pointer re-read -- the head every cdecl entry point spells out (0x0045b2d0.., 0x0045b310..,
    // .., 0x0045b430.., 0x0045b490..).
    NnArrivalText* EnsureNnArrivalText()
    {
        if (G_NnArrivalText == nullptr)
            WinFactoryCreate("NN_ArrivalText", "NN_ArrivalText", nullptr, nullptr);
        return G_NnArrivalText;
    }

    // The strid_name of the player's system: Universe::get_system(the routine()) and its slot 3
    // (+0x0c), or 0 when there is no such system -- 0x0045aebd..0x0045aed7, 0x0045b004..
    // , 0x0045b1d4..0x0045b1ee.
    int PlayerSystemStridName()
    {
        Universe::ISystem const* const System =
            Universe::get_system(GetPlayerStarSystemIdbaseline());
        if (System == nullptr)
            return 0;
        return System->GetStridName();
    }

    // .rdata 0x005ce928: the typing time per character ShowBaseArrival reserves.
    constexpr float kArrivalSecondsPerChar = 0.06f;
    // Each Show* reads its format (ids 0x55c / 0x55d / 0x55e) with a count of 0x80 into a stack
    // buffer of 0x80 BYTES (0x40 wchars) -- a baseline overrun for a format of 64 characters or
    // more. The port's buffer holds the 0x80 wchars the count allows; the formats are short.
    constexpr int kArrivalFormatChars = 0x80;
}

NnArrivalText::NnArrivalText()
{
    // 0x0045b569..0x0045b583: the routine, vptrs, the field = this, +0x338 = 0.
    G_NnArrivalText = this;
    bWaitForLetterbox = 0;
}

NnArrivalText::~NnArrivalText()
{
    // the vptrs, the field = 0, JMP the routine (the WinControl dtor).
    G_NnArrivalText = nullptr;
}

bool NnArrivalText::Create(Win3D* NewParent, void* Data)
{
    // (RET 8).
    (void)NewParent;

    // the routine "ArrivalText" / "PageHeader" at (-0.45f, 0.24f, 0),
    // no hardpoint, 0.95f x 0.06f, 1.4f, ids 0, 0, 0, last byte 1 -> +0x32c.
    float const CaptionPosition[3] = {NnFloatBits(0xbee66666), NnFloatBits(0x3e75c28f), 0.0f};
    Caption = static_cast<NnTextControl*>(CreateTextControl(
        "ArrivalText", "PageHeader", CaptionPosition, nullptr, NnFloatBits(0x3f733333),
        NnFloatBits(0x3d75c28f), NnFloatBits(0x3fb33333), 0, 0, 0, 1));

    // the caption's slot 42 (0x33, &the field, 1) -- the colour
    // override. No null test on the control.
    Caption->SetProperty(0x33, reinterpret_cast<uintptr_t>(&G_NnPaletteArgb_679b90), 1);
    // the caption hidden (flags &= 0xfc).
    Caption->Flags = static_cast<unsigned char>(Caption->Flags & 0xfc);

    // slot 31 SetOffset(0, 0, -1.0f), the vector passed by value.
    SetOffset(0.0f, 0.0f, -1.0f);
    // slot 5 SetPosition(this + 0x60).
    SetPosition(Position);

    // / 0x0045ac96
    HoldTime = 2.0f;
    Delay = 4.0f;

    // the routine with [this + 4] (the window's current parent, not
    // NewParent) and Data, SETNZ.
    return Win3D::Create(GetParent(), Data);
}

void NnArrivalText::DestroySelf()
{
    Win3D* Child = GetChildHead();
    while (Child != nullptr)
    {
        Win3D* const Next = Child->GetNextSibling();
        Win3DDeleteTree(Child);
        Child = Next;
    }
    Destroy();
    delete this;
}

void NnArrivalText::Update()
{
    // . 0x0045acc8..0x0045ad07: the routine (NN_MainButton has a panel open) tears
    // the caption down.
    if (NnMainButtonHasActivePanel())
    {
        DestroySelf();
        return;
    }

    // waiting for the letterbox. While it is up (the routine) the
    // caption stays hidden and nothing else runs; once it is down a delay at or below zero (FCOMP
    // 0.0f, TEST AH,0x41 / JP skips -- NaN skips) shows it.
    if (bWaitForLetterbox != 0)
    {
        if (IsLetterboxVisible())
        {
            Caption->Flags = static_cast<unsigned char>(Caption->Flags & 0xfc);
            return;
        }
        if (Delay <= 0.0f)
            Caption->Flags = static_cast<unsigned char>(Caption->Flags | 3);
    }

    // the caption's visible bit is sampled here. A caption that is
    // showing with its delay spent goes away as soon as the mission-objective banner
    // (the field, MissionObjectiveWindow) or HUD_Comm's window (the routine) is up.
    bool const bWasVisible = ((Caption->Flags >> 1) & 1) != 0;
    if (Delay <= 0.0f && bWasVisible)
    {
        if (G_MissionObjectiveWindow != nullptr || HudCommIsWindowOpen())
        {
            DestroySelf();
            return;
        }
    }

    // the routine, the base Update.
    Win3D::Update();

    if ((Caption->Flags & 2) == 0)
    {
        // hidden -- unless one of those windows is up, the delay runs
        // down by the capped frame time (in double, stored as float) and a negative delay
        // (TEST AH,5 / JP skips) shows the caption.
        if (G_MissionObjectiveWindow == nullptr && !HudCommIsWindowOpen())
        {
            Delay = static_cast<float>(static_cast<double>(Delay) - GetClientFrameTimeCapped());
            if (Delay < 0.0f)
                Caption->Flags = static_cast<unsigned char>(Caption->Flags | 3);
        }
    }
    else if (Caption->bTypewriter == 0)
    {
        // showing and fully typed (+0x362 clear) -- the hold time runs
        // down the same way; a negative one tears the caption down.
        HoldTime = static_cast<float>(static_cast<double>(HoldTime) - GetClientFrameTimeCapped());
        if (HoldTime < 0.0f)
        {
            DestroySelf();
            return;
        }
    }

    // a caption that became visible this frame starts typing
    // (property 0x50 with 1 and 60 ms per character).
    if (!bWasVisible && (Caption->Flags & 2) != 0)
        Caption->SetProperty(0x50, 1, 0x3c);
}

void NnArrivalText::ShowBaseArrival(float ArrivalTime)
{
    // (RET 4). 0x0045ae9b..0x0045aea2: the routine -- only docked.
    if (!IsPlayerDockedbaseline())
        return;

    bWaitForLetterbox = 0;

    // the routine on the field (the base's strid_name), then the
    // system's; either one zero returns.
    int const BaseIds = GetClientCurrentBaseStridName();
    int const SystemIds = PlayerSystemStridName();
    if (BaseIds == 0 || SystemIds == 0)
        return;

    // ids 0x55c is the format; base and system names into the two halves
    // of scratch A; swprintf into scratch B; CharUpperW.
    wchar_t Format[kArrivalFormatChars];
    GetIdsStringWWithRawFallback(0x55c, Format, kArrivalFormatChars);
    int const Half = G_NnTextScratchCapacity / 2;
    wchar_t* const SecondHalf = G_NnTextScratchA + Half;
    GetIdsStringWWithRawFallback(static_cast<unsigned>(BaseIds), G_NnTextScratchA, Half);
    GetIdsStringWWithRawFallback(static_cast<unsigned>(SystemIds), SecondHalf, Half);
    _swprintf(G_NnTextScratchB, Format, G_NnTextScratchA, SecondHalf);
    CharUpperW(G_NnTextScratchB);

    // the caption's property 0x1d (the text).
    Caption->SetProperty(0x1d, reinterpret_cast<uintptr_t>(G_NnTextScratchB), 0);
    int const Length = static_cast<int>(wcslen(G_NnTextScratchB));

    // only a positive ArrivalTime (TEST AH,0x41 / JNZ skips) sets the
    // delay so typing (0.06f a character) plus the hold ends with it; a negative result (TEST
    // AH,5 / JP skips) is clamped to 0.
    if (!(ArrivalTime > 0.0f))
        return;
    Delay = ArrivalTime - (static_cast<float>(Length) * kArrivalSecondsPerChar + HoldTime);
    if (Delay < 0.0f)
        Delay = 0.0f;
}

void NnArrivalText::ShowSystemArrival(bool bResetDelay)
{
    // (RET 4). 0x0045affb
    bWaitForLetterbox = 1;

    int const SystemIds = PlayerSystemStridName();

    // the region caption by the strid_name's place in 0x30001..0x30018
    // (byte table 0x0045b1a0 over the jump table 0x0045b190).
    unsigned const Index = static_cast<unsigned>(SystemIds) - 0x30001u;
    unsigned RegionIds = 0;
    if (Index <= 4)
        RegionIds = 0x4e1;
    else if (Index <= 10)
        RegionIds = 0x4e2;
    else if (Index <= 16)
        RegionIds = 0x4e4;
    else if (Index <= 0x17)
        RegionIds = 0x4e3;


    if (SystemIds == 0)
        return;

    wchar_t Format[kArrivalFormatChars];
    if (RegionIds != 0)
    {
        // ids 0x55e, the system and the region into the two halves.
        GetIdsStringWWithRawFallback(0x55e, Format, kArrivalFormatChars);
        int const Half = G_NnTextScratchCapacity / 2;
        wchar_t* const SecondHalf = G_NnTextScratchA + Half;
        GetIdsStringWWithRawFallback(static_cast<unsigned>(SystemIds), G_NnTextScratchA, Half);
        GetIdsStringWWithRawFallback(RegionIds, SecondHalf, Half);
        _swprintf(G_NnTextScratchB, Format, G_NnTextScratchA, SecondHalf);
        CharUpperW(G_NnTextScratchB);
        Caption->SetProperty(0x1d, reinterpret_cast<uintptr_t>(G_NnTextScratchB), 0);
    }
    else
    {
        // ids 0x55d with the system alone; the delay becomes 2.0f.
        GetIdsStringWWithRawFallback(0x55d, Format, kArrivalFormatChars);
        GetIdsStringWWithRawFallback(static_cast<unsigned>(SystemIds), G_NnTextScratchA,
                                     G_NnTextScratchCapacity / 2);
        _swprintf(G_NnTextScratchB, Format, G_NnTextScratchA);
        CharUpperW(G_NnTextScratchB);
        Caption->SetProperty(0x1d, reinterpret_cast<uintptr_t>(G_NnTextScratchB), 0);
        Delay = 2.0f;
    }


    if (bResetDelay)
        Delay = 2.0f;
}

void NnArrivalText::ShowNamedArrival(unsigned NameIds)
{
    // (RET 4). 0x0045b1cb
    bWaitForLetterbox = 0;

    int const SystemIds = PlayerSystemStridName();

    if (NameIds == 0 || SystemIds == 0)
        return;

    // ids 0x55c, NameIds and the system into the two halves, the text,
    // delay 2.0f.
    wchar_t Format[kArrivalFormatChars];
    GetIdsStringWWithRawFallback(0x55c, Format, kArrivalFormatChars);
    int const Half = G_NnTextScratchCapacity / 2;
    wchar_t* const SecondHalf = G_NnTextScratchA + Half;
    GetIdsStringWWithRawFallback(NameIds, G_NnTextScratchA, Half);
    GetIdsStringWWithRawFallback(static_cast<unsigned>(SystemIds), SecondHalf, Half);
    _swprintf(G_NnTextScratchB, Format, G_NnTextScratchA, SecondHalf);
    CharUpperW(G_NnTextScratchB);
    Caption->SetProperty(0x1d, reinterpret_cast<uintptr_t>(G_NnTextScratchB), 0);
    Delay = 2.0f;
}

bool NnArrivalText::IsCaptionVisible() const
{
    // MOV EAX,[ECX+0x32c] / MOV AL,[EAX+0x6c] / SHR AL,1 / AND AL,1.
    return ((Caption->Flags >> 1) & 1) != 0;
}

void NnArrivalTextShow()
{
    // (cdecl): ensure, then the routine(0) (0x0045b338..0x0045b33a).
    NnArrivalText* const Self = EnsureNnArrivalText();
    if (Self == nullptr)
        return;
    Self->ShowSystemArrival(false);
}

void NnArrivalTextShowBase(float ArrivalTime)
{
    // (cdecl): ensure, then the routine(ArrivalTime).
    NnArrivalText* const Self = EnsureNnArrivalText();
    if (Self == nullptr)
        return;
    Self->ShowBaseArrival(ArrivalTime);
}

void NnArrivalTextShowNamed(unsigned NameIds)
{
    // (cdecl): ensure, then the routine(NameIds).
    NnArrivalText* const Self = EnsureNnArrivalText();
    if (Self == nullptr)
        return;
    Self->ShowNamedArrival(NameIds);
}

void NnArrivalTextDestroy()
{
    // (cdecl): no window -> nothing; else the teardown.
    NnArrivalText* const Self = G_NnArrivalText;
    if (Self == nullptr)
        return;
    Self->DestroySelf();
}

void NnArrivalTextHurry()
{
    // (cdecl). 0x0045b3cb..0x0045b3fc: a delay above 0 (TEST AH,0x41 / JNZ goes to
    // the teardown -- NaN too) is cut to 1.0f when above it; otherwise the window goes.
    NnArrivalText* const Self = G_NnArrivalText;
    if (Self == nullptr)
        return;
    if (Self->Delay > 0.0f)
    {
        if (Self->Delay > 1.0f)
            Self->Delay = 1.0f;
        return;
    }
    Self->DestroySelf();
}

void NnArrivalTextShowRaw(float Delay, uintptr_t Arg)
{
    // (cdecl): ensure; +0x334 = Delay (dword copy), +0x338 = 0, then the caption's
    // slot 42 (0x1e, Arg, 0).
    NnArrivalText* const Self = EnsureNnArrivalText();
    if (Self == nullptr)
        return;
    Self->Delay = Delay;
    Self->bWaitForLetterbox = 0;
    Self->Caption->SetProperty(0x1e, Arg, 0);
}

void NnArrivalTextShowSystemNow()
{
    // (cdecl): ensure, then the routine(1).
    NnArrivalText* const Self = EnsureNnArrivalText();
    if (Self == nullptr)
        return;
    Self->ShowSystemArrival(true);
}

void NnMainButtonOnModalOpen()
{
    // MOV ECX,[the field] / TEST / JZ RET / JMP the routine.
    if (G_NnMainButton == nullptr)
        return;
    G_NnMainButton->ClosePanel();
}

void NnMainButtonSetPanelOrigin(int PanelId, float const* Origin)
{
    // MOV ECX,[the field] / TEST / JZ RET / the routine(PanelId, Origin).
    if (G_NnMainButton == nullptr)
        return;
    G_NnMainButton->SetPanelOrigin(PanelId, Origin);
}

WinControl* NnMainButtonGetPanelButton(int PanelId)
{
    // null -> XOR EAX,EAX; else the routine(PanelId).
    if (G_NnMainButton == nullptr)
        return nullptr;
    return G_NnMainButton->GetPanelButton(PanelId);
}

bool NnMainButtonHasActivePanel()
{
    // null -> XOR AL,AL; else JMP the routine (`[ECX + 0x3a8] != 0`).
    if (G_NnMainButton == nullptr)
        return false;
    return G_NnMainButton->HasActivePanel();
}

WinControl* G_NnMissionMessage = nullptr;

void NnMissionMessageDestroy()
{
    // with a NeuroNetMissionMessage window, the field = 0, the routine on every
    // child (next sibling read first), slot 10 Destroy, slot 0 with 1. The pointer is not
    // cleared here; the window's dtor (0x0048a46c) does that.
    WinControl* const Window = G_NnMissionMessage;
    if (Window == nullptr)
        return;
    G_bNnScreenUp = false;
    Win3DDeleteTree(Window);
}

unsigned char G_bNnStoryStarKeepPending = 0;

void NnSystemInit(bool bCreateMainButton)
{
    // (cdecl). Nothing when NN_MainButton already exists.
    if (G_NnMainButton != nullptr)
        return;
    G_bNnStoryStarKeepPending = 0;
    // the routine -- the KnowledgeMap singleton the field
    // (interface\KnowledgeMap.ini).
    KnowledgeMapCreate();
    // the routine -- the InfocardMap singleton the field
    // (interface\InfocardMap.ini).
    InfocardMapCreate();
    // the routine("NN_MainButton", "NN_MainButton", 0, 0).
    if (bCreateMainButton)
        WinFactoryCreate("NN_MainButton", "NN_MainButton", nullptr, nullptr);
    // JMP the routine.
    NnVoiceInit();
}

void NnSystemShutdown()
{
    // (cdecl).
    // NN_MainButton slot 10 Destroy, then (re-read) slot 0 with 1.
    if (G_NnMainButton != nullptr)
    {
        G_NnMainButton->Destroy();
        if (G_NnMainButton != nullptr)
            delete G_NnMainButton;
    }
    // the trade window's slot 42 (0xef02, 0, 0).
    if (G_NnTrade != nullptr)
        G_NnTrade->SetProperty(0xef02, 0, 0);
    // the routine -- free the KnowledgeMap.
    KnowledgeMapDestroy();
    // the routine -- free the InfocardMap.
    InfocardMapDestroy();
    // the routine.
    NnArrivalTextDestroy();
    // the routine -- the story star reset (the the field pending FmtStr list
    // unless the field, then the the field banner).
    StoryStarReset();
    // JMP the routine.
    NnMissionMessageDestroy();
}

void NnMainButtonOnUnclaimedMouse(unsigned Msg, unsigned WParam, unsigned LParam)
{
    // (cdecl, 3 args).
    (void)WParam;
    (void)LParam;

    // the routine() and the field == 0.
    if (GetClientMouseCursorDrawFlag() == 0)
        return;
    if (G_bNnScreenUp)
        return;

    // the routine -- `(the field & 0x3fffffff) != 0`, the player is docked.
    // In space the message goes to the routine(Msg, WParam, LParam) (0x0045a424).
    if ((GetPlayerBaseIdbaseline() & 0x3fffffffu) == 0)
    {
        OnMouseFlightRawMessage(Msg);
        return;
    }

    // docked, and no NN_MainButton -> nothing.
    if (G_NnMainButton == nullptr)
        return;
    // the routine -- a NavBar with one of the service windows up.
    if (NavBarHasServiceWindow())
        return;
    // the routine -- the NN_MainButton has a panel open.
    if (G_NnMainButton->HasActivePanel())
        return;

    // a hover record -- caption null, the Font::String constructed
    // (the routine), IdsAlt 0, Ids -1, +0x44 0, the caption cleared (the routine), +0x60 0 --
    // then Ids = IdsAlt = 0x45e.
    FToolTipHoverInfo Record; // caption null + the routine: the record's constructor
    ToolTipHoverReset(&Record);
    Record.Ids = 0x45e;
    Record.IdsAlt = 0x45e;
    // in the base's "Cityscape" room (the current room runtime's id
    // +0x38 against MakeLocationID(BaseData's id, "Cityscape"); the routine / the routine,
    // both null-tested) the ids are 0x3a0 instead.
    FRoomRuntime* const Runtime = static_cast<FRoomRuntime*>(GetClientCurrentLocationRuntime());
    BaseData* const Base = GetClientCurrentLocationBaseData();
    if (Runtime != nullptr && Base != nullptr &&
        GetRoomRuntimeRoomId(Runtime) == MakeLocationID(Base->get_base_id(), "Cityscape"))
    {
        Record.Ids = 0x3a0;
        Record.IdsAlt = 0x3a0;
    }

    // the routine(&record, 8.0f) -- the rollover shows the record for
    // eight seconds.
    NnRolloverShowHoverGlobal(&Record, NnFloatBits(0x41000000));
    // the routine(the field, 0) -- the voice line.
    NnVoiceEnqueue(G_NnVoiceClickOnCharactersId, 0);
    // the record's dtor inlined (the routine) -- ~FToolTipHoverInfo.
}

unsigned char NnModalPauseGame()
{
    // 004a7671 PUSH 1 / CALL the routine; 004a7678 CALL the routine, MOV BL,AL;
    // PUSH 1 / CALL the routine; 004a7689 MOV AL,BL.
    SetClientServerTickPause(true);
    unsigned char const bPrevious = GetClientMouseCursorDrawFlag();
    SetClientMouseCursorDrawFlag(1);
    return bPrevious;
}

void NnModalResumeGame(unsigned char bCursorDrawFlag)
{
    // 004a7690 PUSH 0 / CALL the routine; 004a76a2 JMP the routine with the
    // argument slot rewritten to param_1.
    SetClientServerTickPause(false);
    SetClientMouseCursorDrawFlag(bCursorDrawFlag);
}
