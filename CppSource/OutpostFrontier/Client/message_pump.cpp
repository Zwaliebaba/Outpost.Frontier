// Author: Giperion.

#include "globals.h"
#include "input_manager.h"
#include "menu_keymap_config.h" // KeyMapRouteInputMessage (the routine)

#include "imeui.h"

#include <windows.h>

// (module storage-005b0c20), cdecl(int* ExitCode): drain the queue with
// PeekMessage(PM_NOREMOVE) + GetMessage. A hot key IMEFocus claims, or a message the menu
// accelerator router (the routine) consumes, skips Translate/Dispatch. WM_QUIT stores its
// wParam through ExitCode (no null check, 0x005b0c18) and returns false.
bool PumpMessages(int* exit_code)
{
    // the routine drains the DirectInput mouse queue first.
    MouseDispatchEvents();
    MSG Msg;
    while (PeekMessageA(&Msg, nullptr, 0, 0, PM_NOREMOVE))
    {
        if (!GetMessageA(&Msg, nullptr, 0, 0))
        {
            *exit_code = static_cast<int>(Msg.wParam);
            return false;
        }

        IMEFocus* const Ime = IMEFocus::GetSingleton();
        if ((Ime == nullptr || !Ime->IgnoreHotKey(&Msg)) &&
            !KeyMapRouteInputMessage(Msg.message, static_cast<unsigned>(Msg.wParam),
                                     static_cast<unsigned>(Msg.lParam)))
        {
            TranslateMessage(&Msg);
            DispatchMessageA(&Msg);
        }
    }
    return true;
}
