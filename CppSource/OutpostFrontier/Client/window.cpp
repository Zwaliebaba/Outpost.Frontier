// Author: Giperion.

#include "globals.h"
#include "imeui.h"
#include "input_manager.h"
#include "nn_error.h"
#include "nn_exit.h"
#include "nn_network_operation.h"

#include "NeuronRuntime.h"
#include "common_misc.h" // SinglePlayer
#include "render/IRenderPipeline.h"

#include <windows.h>

DisplaySettings g_display = {};
ClientGlobals g_client = {};

// RenderMgr.cpp state (baseline .data/.bss next to the functions below).
HWND G_RenderMgrWindow = nullptr;          // , written by the routine
unsigned G_RenderSurfaceWidth = 0x280;     // (initialised .data)
unsigned G_RenderSurfaceHeight = 0x1e0;    // (initialised .data)
bool G_bRenderSurfaceFullscreen = false;
bool G_bRenderMgrShuttingDown = false;     // , the routine's re-entry latch
bool G_bRenderPostCreate = true;           // (initialised .data 01h)
bool G_bRenderFrameBegun = false;          // (.data, no initialiser)

// (module storage-00424791), cdecl(ClassName, Display, Title). A zero width or height
// in the options object is replaced by 640/480 locally only -- baseline never writes it back. The
// window handle is also stored in RenderMgr's the field (0x00424788).
HWND CreateGameWindow(const char* class_name, DisplaySettings* display, const char* title)
{
    const HINSTANCE instance = GetModuleHandleA(nullptr);
    int width = display->width;
    if (width == 0)
        width = 0x280;
    int height = display->height;
    if (height == 0)
        height = 0x1e0;

    int x = 0;
    int y = 0;
    if (display->fullscreen == 0)
    {
        RECT rc = {0, 0, width, height};
        x = CW_USEDEFAULT;
        y = CW_USEDEFAULT;
        if (AdjustWindowRect(&rc, 0x2CA0000, FALSE))
            width = rc.right - rc.left;
        // the height is taken from the rect whether or not the call succeeded.
        height = rc.bottom - rc.top;
    }

    G_RenderMgrWindow = CreateWindowExA(0, class_name, title, 0x2CA0000, x, y, width, height,
                                        nullptr, nullptr, instance, nullptr);
    return G_RenderMgrWindow;
}

// (module storage-004249b8), cdecl(hwnd, fullscreen, width, height): records the
// surface size and mode, then restyles and resizes the window for it.
void ApplyGameWindowLayout(HWND Hwnd, bool bFullscreen, unsigned Width, unsigned Height)
{
    G_RenderSurfaceWidth = Width;
    G_RenderSurfaceHeight = Height;
    G_bRenderSurfaceFullscreen = bFullscreen;

    if (bFullscreen)
    {
        // never smaller than the desktop (unsigned compares, JAE).
        RECT Desktop;
        GetWindowRect(GetDesktopWindow(), &Desktop);
        if (Width < static_cast<unsigned>(Desktop.right))
            Width = static_cast<unsigned>(Desktop.right);
        if (Height < static_cast<unsigned>(Desktop.bottom))
            Height = static_cast<unsigned>(Desktop.bottom);

        SetWindowLongA(Hwnd, GWL_STYLE, static_cast<LONG>(0x80000000)); // WS_POPUP
        SetWindowPos(Hwnd, nullptr, 0, 0, 0, 0, 0x27);
        SetWindowPos(Hwnd, HWND_TOPMOST, 0, 0, static_cast<int>(Width), static_cast<int>(Height),
                     0x100);
        ShowWindow(Hwnd, SW_SHOWNORMAL);
        return;
    }

    // .
    SetWindowLongA(Hwnd, GWL_STYLE, 0x2ca0000);
    SetWindowPos(Hwnd, nullptr, 0, 0, 0, 0, 0x27);
    RECT Frame;
    Frame.left = 0;
    Frame.top = 0;
    Frame.right = static_cast<LONG>(Width);
    Frame.bottom = static_cast<LONG>(Height);
    // Argument evaluation order as baseline: ex-style, menu, style.
    const DWORD ExStyle = static_cast<DWORD>(GetWindowLongA(Hwnd, GWL_EXSTYLE));
    const BOOL bMenu = GetMenu(Hwnd) != nullptr ? TRUE : FALSE;
    const DWORD Style = static_cast<DWORD>(GetWindowLongA(Hwnd, GWL_STYLE));
    AdjustWindowRectEx(&Frame, Style, bMenu, ExStyle);
    SetWindowPos(Hwnd, HWND_NOTOPMOST, 0, 0, Frame.right - Frame.left, Frame.bottom - Frame.top,
                 0x102);
    ShowWindow(Hwnd, G_bRenderMgrShuttingDown ? SW_HIDE : SW_SHOWNORMAL);
    InvalidateRect(nullptr, nullptr, TRUE);
}

namespace
{
    void ReleaseRenderDeviceForDeactivate()
    {
        // NeuronRuntime::Pipeline slot 14 (+0x38) — what the branch calls before handing the
        // deactivation on to DefWindowProc.
        if (NeuronRuntime::Pipeline == nullptr)
            return;
        NeuronRuntime::Pipeline->DestroyGpuResources();
    }

    // state.

    // — the WinMain loop's present gate: `if (the field) the routine(dt);`
    // (0x5b0fe8). WM_ACTIVATEAPP is its only writer.
    bool G_bPresentAllowed = true;
    // — "application is active", latched by the routine.
    bool G_bAppActive = true;
    // / the field — WinMain sets the first once InitializeEngine returns, and
    // SC_CLOSE sets the second when it arrives before that.
    bool G_bQuitDialogReady = false;
    bool G_bQuitDialogPending = false;

    // is read exactly once in the whole PE (005b2667) and never written, so the
    // WM_ACTIVATEAPP branch it guards always takes the zero path. Kept as a named constant
    // rather than silently folded away.
    const bool kActivateOverrideByte = false;

    bool IsSinglePlayerSession()
    {
        // COMMON.DLL::SinglePlayer — baseline default is true.
        return SinglePlayer();
    }

    void OnActivateApp(bool bActive)
    {
        // . the field is the local "active" copy; the derived gate is
        // , which stays 1 whenever the app is not a fullscreen single-player
        // session (there is nothing to release then).
        const bool bFullscreen = g_display.fullscreen != 0;
        if (!IsSinglePlayerSession() || (kActivateOverrideByte && !bFullscreen))
            G_bPresentAllowed = true;
        else
            G_bPresentAllowed = bActive;

        // a fullscreen surface (the field, 0x00424c60) re-activated after it
        // was inactive clears the post-create flag the field; then the field latches the
        // new state and the routine(bActive, w, h, fullscreen) takes or releases the mouse.
        if (G_bRenderSurfaceFullscreen && !G_bAppActive && bActive)
            G_bRenderPostCreate = false;
        G_bAppActive = bActive;
        MouseSetEnabled(bActive, static_cast<int>(G_RenderSurfaceWidth),
                        static_cast<int>(G_RenderSurfaceHeight), G_bRenderSurfaceFullscreen);
    }

    bool OnSysCommandClose()
    {
        // the routine gates on app-active + post-create + buffers-ready.
        // Then TEST EAX after the routine (body = the routine("NN_Error") — nonzero
        // swallows) and the routine ("NN_NetworkOperation"). Then the routine NN_Exit.
        if (!RenderMgrFrameReady())
            return false;
        if (NnErrorFind() != nullptr)
            return false;
        if (NnNetworkOperationIsOpen())
            return false;
        if (!G_bQuitDialogReady)
        {
            G_bQuitDialogPending = true;
            return true;
        }
        RequestClientQuitDialog();
        return true;
    }
}

bool ClientPresentAllowed()
{
    return G_bPresentAllowed;
}

bool ClientAppIsActive()
{
    return G_bAppActive;
}

// (module storage-00424cd3): three byte tests, 1 or 0.
bool RenderMgrFrameReady()
{
    return G_bAppActive && G_bRenderPostCreate && G_bRenderFrameBegun;
}

void ClientNoteEngineReady()
{
    // WinMain 0x5b0fbe: the field = 1; if (the field) the routine(0);
    G_bQuitDialogReady = true;
    if (G_bQuitDialogPending)
    {
        G_bQuitDialogPending = false;
        RequestClientQuitDialog();
    }
}

void RequestClientQuitDialog()
{
    // / 005b0fc8: the routine(0) -- raise the NN_Exit confirmation. Its confirm paths
    // call DestroyWindow on the game window themselves.
    NnExitOpen(false);
}

LRESULT CALLBACK ClientWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
    // head. The IME gets first refusal on every message; a true return means it
    // consumed it, and the LRESULT comes back through the out parameter. baseline hands WndProc
    // the GLOBAL window handle (the field) rather than the hwnd it was called with, and
    // reuses the lparam argument slot as that out parameter (005b258f / 005b25ac).
    IMEFocus* const Ime = IMEFocus::GetSingleton();
    if (Ime != nullptr)
    {
        int Handled = 0;
        if (Ime->WndProc(g_client.window, msg, wparam, lparam, &Handled))
            return Handled;
    }

    switch (msg)
    {
    case WM_SETFOCUS:
        ClientInputOnFocusGained(); // caseD_7 — the routine
        break;
    case WM_KILLFOCUS:
        ClientInputOnFocusLost(); // caseD_8 — the routine
        break;
    case WM_ERASEBKGND:
        // caseD_14 — the routine returns the field; non-zero means "the engine owns the
        // surface, do not let GDI erase it".
        if (G_bRenderPostCreate)
            return 1;
        break;
    // WM_PAINT (0xF): baseline switch byte at 0x005b27b9 = 5 → caseD_3 → DefWindowProcA
    // (the branch). No BeginPaint/EndPaint / ValidateRect here — those were a port scaffold.
    case WM_ACTIVATEAPP:
        OnActivateApp(wparam == 1);
        if (!G_bAppActive && g_display.fullscreen != 0)
        {
            // NeuronRuntime::Pipeline slot 14 (+0x38) — release the device while minimised.
            ReleaseRenderDeviceForDeactivate();
        }
        break;
    case WM_NCHITTEST:
        // — a fullscreen window claims every pixel as client area.
        if (g_display.fullscreen != 0)
            return HTCLIENT;
        break;
    case WM_SYSKEYUP:
        // 0x105 — swallowed so Alt releases never reach DefWindowProc's menu handling.
        return 0;
    case WM_SYSCOMMAND:
        if (wparam == SC_CLOSE)
        {
            OnSysCommandClose();
            return 0;
        }
        if (wparam == SC_SCREENSAVE)
            return 0;
        break;
    case WM_MOUSEMOVE:
        // CMP ESI,0x200 -> the routine, then caseD_3 -> DefWindowProcA.
        // baseline's WndProc has no WM_LBUTTONDOWN / WM_LBUTTONUP case: buttons arrive through
        // the DirectInput queue (MouseDispatchEvents, the routine) and the Win3D handler.
        ClientInputOnMouseMove();
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcA(hwnd, msg, wparam, lparam);
    }
    return DefWindowProcA(hwnd, msg, wparam, lparam);
}
