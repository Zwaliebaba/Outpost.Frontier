// Author: Giperion.

#pragma once

// The pause/resume pair every NeuroNet modal dialog (NN_Exit, NN_NetworkOperation, NN_Error,
// BlockingDialog) brackets itself with. NN_Prompt does not: neither its ctor (the routine), dtor
// (the routine), Create (the routine) nor any close path (the branch / the branch /
// ) calls them. baseline keeps them next to NN_Pause (0x004a7670 / 0x004a7690); both
// are cdecl.

// the routine(1) (SetClientServerTickPause), then remember the software-cursor
// draw flag (the routine), force it on (the routine(1)) and return the remembered value. The
// dialogs keep the result in a byte of their own and hand it back to NnModalResumeGame.
unsigned char NnModalPauseGame();

// the routine(0), then the routine(bCursorDrawFlag) (a tail jump).
void NnModalResumeGame(unsigned char bCursorDrawFlag);

// a byte the NeuroNet windows raise while one of them holds the screen (written
// 1 at 0x0045c921, 0x0048a537, 0x004a6a31, 0x004a98fe; 0 at 0x0045c914, 0x0048a13b,
// .., 0x004af057, and the space teardown 0x0054af3c = DestroySpaceState) and readers test (0x0043e748.., 0x0045a297 in the routine,
// , 0x0046fcf3, NN_Pause 0x004a75c0). The Ship's jump states also write it (1 at
// entering state 1, 0 at 0x00543851 leaving state 5 -- FClientShipGameobj::
// SetJumpState); of the writers listed above only the space teardown is ported so far.
extern bool G_bNnScreenUp;

// The NN_MainButton singleton, the field (RTTI .?AVNN_MainButton@@; class in
// nn_main_button.h). Written by its ctor the routine and cleared by its dtor the routine;
// NnSystemInit makes it, NnSystemShutdown deletes it.
class NN_MainButton;
class WinControl;
extern NN_MainButton* G_NnMainButton;

// (cdecl) -- the NeuroNet system: when NN_MainButton does not exist yet,
// = 0, the KnowledgeMap (the routine) and InfocardMap (the routine) singletons,
// NN_MainButton through the WinFactory when bCreateMainButton, and the NN voice init (tail JMP
// ). Callers: GF's first frame 0x004372fe, the MissionBriefing state 0x0043d434,
// the space init 0x0054ab7c.
void NnSystemInit(bool bCreateMainButton);

// (cdecl) -- NN_MainButton Destroy + delete, the trade window's 0xef02, the
// KnowledgeMap / InfocardMap frees, NN_ArrivalText teardown, the story star reset
// (the routine) and the NeuroNetMissionMessage teardown (tail JMP the routine). Callers: GF
// Destroy 0x004372c8, the MissionBriefing state 0x0043d337 / 0x0043d52d, the space teardown
// , the client teardown 0x005b2232.
void NnSystemShutdown();

// the NeuroNetMissionMessage window (ctor 0x0048b20d sets it, dtor 0x0048a46c
// clears it). The class is NeuroNetMissionMessage (nn_mission_message.h).
extern WinControl* G_NnMissionMessage;

// (cdecl) -- with a NeuroNetMissionMessage window: the field = 0 and the
// window's subtree deleted (the routine per child, slot 10, deleting dtor).
void NnMissionMessageDestroy();

// zeroed by the routine, set at 0x004bc1ff (the story star's button
// dispatcher); while set the routine keeps the story star's pending list. Read at 0x004b9bb0,
// , 0x004b9dd7, 0x004ba46f, 0x004bcc4e -- none ported yet.
extern unsigned char G_bNnStoryStarKeepPending;

// (cdecl) -- null when there is no NN_MainButton, else the routine(PanelId): the
// icon button of that panel. Caller: NN_Inventory 0x00477ae9.
WinControl* NnMainButtonGetPanelButton(int PanelId);

// The NN_ArrivalText singleton, the field (written by its ctor, 0x0045b57d, and zeroed by its
// dtor at 0x0045abbd). Class and WINFACTORY in nn_arrival_text.h / nn_modal.cpp.
class NnArrivalText;
extern NnArrivalText* G_NnArrivalText;

// (cdecl) -- make the NN_ArrivalText window through the WinFactory when there is
// none, then the routine(0) on it (the system arrival caption). Caller: the Ship's jump state
// 5 for the player's character (0x00543ad4). The other entry points are in nn_arrival_text.h.
void NnArrivalTextShow();

// `if (the field) the routine(the field)` (NN_MainButton::ClosePanel). Called by the NeuroNet windows'
// Create bodies (NN_Exit 0x00471825, 0x004a6bac, ...) before they build their controls.
void NnMainButtonOnModalOpen();

// (cdecl) -- `if (the field) the routine(the field, PanelId, Origin)`:
// NN_MainButton stores the three floats as the origin of panel PanelId (0x63..0x6a map to
// slots 0..6 at +0x444 + 12 * slot). Called by NN_Inventory's dtor (0x00477e4d) and
// .
void NnMainButtonSetPanelOrigin(int PanelId, float const* Origin);

// `the field != 0 && the routine(the field)`; the routine is
// `[this + 0x3a8] != 0`, the panel the button has open (the routine switches +0x3ac over the
// NeuroNet panel names).
bool NnMainButtonHasActivePanel();

// (Msg, WParam, LParam) -- the mouse message nobody else claimed (the routine hands
// it WM_LBUTTONDOWN for an unclaimed left press). With the cursor drawn and no NeuroNet screen up:
// in space it goes to the mouse-flight hook the routine; docked, NN_MainButton handles it.
void NnMainButtonOnUnclaimedMouse(unsigned Msg, unsigned WParam, unsigned LParam);

// The dialogs' Create bodies push their layout floats as raw immediates (e.g. PUSH 0x3cf5c28f);
// this turns such an immediate back into the float it encodes, bit for bit.
inline float NnFloatBits(unsigned Bits)
{
    union
    {
        unsigned U;
        float F;
    } Value;
    Value.U = Bits;
    return Value.F;
}
