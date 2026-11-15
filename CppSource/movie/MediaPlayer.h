// Author: Giperion.

#pragma once

#include <windows.h>

struct FApproach2Player;
struct IGraphBuilder;
struct IMediaEventEx;
struct IMediaControl;
struct IBasicVideo;
struct IVideoWindow;
struct IUnknown;

// Global state variables (BSS)
extern int g_stateFlag;           // 0=not initialized, 1=active, 2=fullscreen
extern FApproach2Player* g_playerObject;
extern IGraphBuilder* g_graphBuilder;
extern IMediaEventEx* g_mediaEventEx;
extern IMediaControl* g_mediaControl;
extern IBasicVideo* g_basicVideo;
extern IVideoWindow* g_videoWindow;
extern int g_initSuccess;         // initialization success flag
extern unsigned char g_playing;   // playback active flag
extern HWND g_hwndParent;         // parent window handle
extern WNDPROC g_originalWndProc; // original WindowProc for subclassing
extern IUnknown* g_serviceProvider;
extern HMODULE g_movieModule;

// Exported functions (7 C++ style exports with decorated names)
namespace MediaPlayer
{
    // Approach enum inside namespace to match baseline decoration
    enum Approach
    {
        ApproachStandard = 0
    };

    __declspec(dllexport) void __cdecl Startup();
    __declspec(dllexport) bool __cdecl UpdateMovie();
    __declspec(dllexport) void __cdecl StopMovie();
    __declspec(dllexport) long __cdecl PlayMovie(HWND hwnd, bool fullscreen, const char* path);
    __declspec(dllexport) void __cdecl FreeMovie();
    __declspec(dllexport) void __cdecl Shutdown();
    __declspec(dllexport) void __cdecl SetApproach(Approach approach);
}
