// Author: Giperion.

#pragma once

// baseline Client\cmn\mouse.cpp -- the DirectInput mouse, its event queue and
// the software cursor.

// Three pieces of baseline state:

//   * The mouse object at the field (0x74 bytes, constructed lazily behind the field bit 0
//     by the routine, destroyed through the atexit thunk the branch). It owns the DirectInput
//     interface (+0x0c, the field), the SysMouse device (+0x10, the field), a flags byte
//     (+0x14, the field), the buffered-data array (+0x24 count / +0x28 data) and a std::list of
//     0x24-byte input events (+0x04 head / +0x08 size) that the per-frame poll fills and the
//     message pump drains. The device is created once, behind the field.
//   * The software cursor position the field / the field / the field (x, y, wheel
//     accumulator) and the button bits the field. While the object is active (flags bit 0)
//     the position is driven by DirectInput deltas scaled by the surface width, not by
//     GetCursorPos; GetCursorPos is only read when the object (re)activates.
//   * The named cursors: up to 0x14 records from mouse.ini [Cursor] (the field count,
// array), the one in use (the field), the "DEFAULT" alias (TString<32> at
// ) and the cursor shapes in the ShapeDB the field (G_MouseShapeDB). The
//     cursor is drawn at the very end of the routine by the routine.

// Flags byte (+0x14):
//   0x01 active (the routine(1)) -- the device is polled and owns the cursor
//   0x02 absolute axis mode (base captured at acquire); never set in the PE, branches kept
//   0x04 acquired and the last read succeeded
//   0x08 cooperative level set to DISCL_EXCLUSIVE | DISCL_FOREGROUND
//   0x10 clamp the cursor to the surface instead of releasing the mouse when it leaves it
//   0x20 the device needs Poll
//   0x40 buffered mode (GetDeviceData into the +0x28 array)

#include "hud_command_dispatch.h" // FHudCmdRecord (the event layout's first 0x1c bytes)

#include <stddef.h>

// One queued input event (a 0x2c-byte list node minus the two links). The first 0x1c bytes are
// exactly what the routine copies into the key-map record the field.
struct FMouseEvent
{
    FHudCmdRecord Record;       // +0x00  Key +0x04 (-1 - button, -9 wheel), Byte18 down,
                                //        Byte19 repeat (always 0), Byte1A double click
    unsigned char bMove;        // +0x1c  a cursor-move event (the routine)
    unsigned char Pad1d[3];
    unsigned ButtonIndex;       // +0x20  0..7 buttons, 8 wheel
};

static_assert(sizeof(FHudCmdRecord) == 0x1c, "module state copies seven dwords");
static_assert(offsetof(FMouseEvent, bMove) == 0x1c, "event +0x1c move flag");
static_assert(offsetof(FMouseEvent, ButtonIndex) == 0x20, "event +0x20 button index");
static_assert(sizeof(FMouseEvent) == 0x24, "module state copies nine dwords");

// WM_MOUSEMOVE: with input focus (the field), make sure the device exists and
// re-activate the object (the routine(1) inlined): the cursor jumps to the OS pointer.
void ClientInputOnMouseMove();
// WM_SETFOCUS: the same, unconditionally, then the field = 1.
void ClientInputOnFocusGained();
// WM_KILLFOCUS: deactivate (the routine(0) inlined) and the field = 0.
void ClientInputOnFocusLost();

// (bEnable, Width, Height, bFullscreen) -- called by the routine (device buffers
// created) and the routine (WM_ACTIVATEAPP). Enable needs bFullscreen or the window to hold the
// focus; bFullscreen becomes flags bit 0x10 (clamp instead of release). Width and Height are
// not read.
void MouseSetEnabled(bool bEnable, int Width, int Height, bool bFullscreen);

// drain the event queue: move events post key-map command 5 (the routine),
// button / wheel events go through the routine, and an unclaimed left press reaches
// (0x201, 0, 0). Called first in every message-pump loop.
void MouseDispatchEvents();

// the per-frame mouse update and cursor draw (gated by the field): poll the
// device, move the cursor, queue move / button / wheel events, then draw the current cursor.
// The last call of the routine (Win3DDrawAll) and of the NN_Error modal loop.
void MouseFrameUpdate();

// load the mouse.ini shapes (the routine) and cursors (the routine), set
// and select "DEFAULT". Called by the routine.
void MouseInitCursors();
// release the device and the DirectInput interface, free the cursors, clear the
// shape DB and the field. Called by the routine.
void MouseShutdown();

// (Name, bSetDefault) -- select the cursor called Name ("DEFAULT" is the name stored
// by the last bSetDefault call). With bSetDefault the name (or its absence) is stored first.
// Returns baseline's EAX, which no caller reads.
void MouseSetCursor(char const* Name, bool bSetDefault);
// whether the cursor in use is "Arrow".
bool MouseIsArrowCursor();

// .
bool ClientInputIsActive();
// / the field / the field.
void GetClientSoftwareCursor(int* OutX, int* OutY, int* OutZ);
void SetClientSoftwareCursor(int X, int Y);
// the wheel delta of the last the routine; read by the routine (event 4) and
// .
int GetClientMouseWheel();
// button bits (bit n = button n held), kept by the routine / the routine;
// read by 0x004957f8, 0x004b9331, 0x004bde7e, the routine, 0x0051a84c and 0x00578545.
unsigned GetClientMouseButtons();
// perf option "mouse_speed": the field = clamp(Value, 0, 1) * 1.75 + 0.25.
void ApplyMouseSpeedOption(float Value);
// the axis multiplier the routine / the routine apply.
float GetClientMouseSpeed();
// Mouse object +0x40 (the field) -- GetDoubleClickTime() latched when the device is created.
unsigned GetClientDoubleClickTime();

// / the routine / the routine -- the software-cursor draw latch.
unsigned char GetClientMouseCursorDrawFlag();
void SetClientMouseCursorDrawFlag(unsigned char Value);
