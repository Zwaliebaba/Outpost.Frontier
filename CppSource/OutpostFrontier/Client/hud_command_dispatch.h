// Author: Giperion.

#pragma once

// baseline Client/Interface/KeyMap.cpp — HUD / KeyCmd message post + handler tree.

// Event::Handler is the single canonical command receiver.
class EventHandler;
using FHudCmdHandler = EventHandler;

// the record of the event being dispatched (seven dwords). the routine writes
// the id at +0; the poster fills the rest before posting. Readers reach it through
// (HudCommandRecord). Field meanings as the Win3D input handler the routine and
// the key dispatcher the routine read them:
struct FHudCmdRecord
{
    unsigned MsgId;          // +0x00
    unsigned Key;            // +0x04 -- key code (event 1)
    unsigned Modifiers;      // +0x08 -- the field: Shift 1 / Ctrl 4 / Alt 0x10 (the routine)
    unsigned Field0C;        // +0x0c -- the field: raw window message (the routine)
    unsigned Arg10;          // +0x10 -- the field: wParam (event 2 first argument)
    unsigned Arg14;          // +0x14 -- the field: lParam (event 2 second argument)
    unsigned char Byte18;    // +0x18 -- the field: key down (the routine) / set by the routine
    unsigned char Byte19;    // +0x19 -- the field: auto-repeat key event (the routine)
    unsigned char Byte1A;    // +0x1a -- mouse events: second flag (the routine 0059d184)
    unsigned char Byte1B;    // +0x1b
};

// &the field.
FHudCmdRecord* HudCommandRecord();

// (msgId, bSkipMods): if bSkipMods==0 capture Shift/Ctrl/Alt into
// and set the field=1; then the routine(msgId, 0). The tail is a JMP, so the
// result is the routine's (HUD_Target's SetProperty 0x004e1b04 / 0x004e1b8e returns its low byte).
int HudPostCommand(unsigned MsgId, int bSkipMods);

// (msgId, mode): walk the field handler tree; call handler vt[0].
int HudDispatchCommand(unsigned MsgId, int Mode);

// — register Handler for MsgId into the field multimap (after existing entries
// with the same id; duplicates allowed).
void HudRegisterCommandHandler(FHudCmdHandler* Handler, unsigned MsgId);

// — erase Handler's entries for one MsgId (the tree is kept even if it empties).
void HudUnregisterCommand(FHudCmdHandler* Handler, unsigned MsgId);

// — erase every entry whose handler == Handler; free the tree once it is empty.
void HudUnregisterCommandHandler(FHudCmdHandler* Handler);

// — cancel stack the field: skip the first Skip entries holding Handler and
// unlink every later one.
void HudRemoveCancelHandler(FHudCmdHandler* Handler, int Skip);

// — HudRemoveCancelHandler(Handler, Skip), then push Handler on the cancel stack;
// USER_CANCEL (0xf) in mode 1 pops it and calls its slot 0 with 0x10 (the routine tail).
void HudPushCancelHandler(FHudCmdHandler* Handler, int Skip);

// — Event::Handler base destructor body: HudPopInputFocus, erase every
// registration (freeing an emptied tree), HudRemoveCancelHandler(Handler, 0).
void HudDestroyEventHandler(FHudCmdHandler* Handler);

// — make Handler the Mode-1 filter (the field); a filter already in place is
// pushed onto the field. Modal NeuroNet dialogs call it when they open.
void HudPushInputFocus(FHudCmdHandler* Handler);

// — drop Handler: if it is the active filter the last pushed one comes back,
// otherwise it is unlinked from the stack. An emptied stack is freed.
void HudPopInputFocus(FHudCmdHandler* Handler);

// — the active Mode-1 filter (the field).
FHudCmdHandler* HudGetInputFocus();

// The 0x28-byte command record the routine routes. HUD_Button's click (the branch) zeroes
// ten dwords, sets +0x26 = 1 and the id at +0; the other posters (0x0049407d, 0x004cef01 ...)
// fill the target fields as well.
struct FSpaceCommand
{
    int Id;                        // +0x00 -- 0 ends routing after the camera's first look
    unsigned char Pad04[0x0c];
    unsigned ObjectId;             // +0x10
    unsigned SubObjectId;          // +0x14
    unsigned char Pad18[0x0e];
    unsigned char Byte26;          // +0x26
    unsigned char Pad27;
};
static_assert(sizeof(FSpaceCommand) == 0x28, "baseline command record is 0x28 bytes");

// (cdecl, one argument) -- route a command through the in-flight camera
// controller. False when no controller exists (the field); otherwise the active camera
// (the field) sees it through its slot 0x70, a record whose id is then 0 is consumed, slot
// 0x74 may consume it, and ids above 0xe go on to the watched client ship's slot 0x1cc. True
// whenever the controller exists and the id is above 0xe or a camera consumed it.
bool SpaceSendCommand(FSpaceCommand* Command);
