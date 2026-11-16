// Author: Giperion.

#pragma once

#include <windows.h>

// Client BSS / globals recovered from OutpostFrontier.exe WinMain (the routine).

// the exe's bounded string copy: copies min(strlen(Src), Capacity - 1) chars and
// a terminator into Dest and returns the count copied. A null Src returns 0 and leaves Dest
// untouched (callers that can see a null clear the buffer themselves). Shared by the options
// object below and many loaders across the exe.
unsigned CopyBoundedString(char* Dest, unsigned Capacity, const char* Src);

// The {length, text[0x104]} path buffer the options object keeps its strings in. the routine
// fills Text and the caller stores the returned length in front of it; the accessor baseline calls
// (ICF-folded with MD5Hash::GetStatePtr, module storage `LEA EAX,[ECX+4]`) returns Text.
struct FClientPathString
{
    unsigned Length;  // +0x00
    char Text[0x104]; // +0x04

    const char* c_str() const { return Text; }
};
static_assert(sizeof(FClientPathString) == 0x108, "FClientPathString stride is 0x108");

// The options object at the field (this of the routine). Its first 0x28 bytes are the
// ten-dword render mode the routine hands to DX8RENDERER create_buffers as the out-blob (DX8RENDERER FDx8RendererMode:
// Field00, Field04, Width, Height, Format, WantedZ, WantedStencil, Interval, BufferCount,
// SwapExtra, Fullscreen); the rest is the routine's OutpostFrontier.ini state. Field defaults are
// the stores at 0x00562225..0x00562265.
struct DisplaySettings
{
    int mode_field00;   // +0x000  -1
    int refresh_rate;   // +0x004  0x3c
    int width;          // +0x008  0x400
    int height;         // +0x00c  0x300
    int color_bpp;      // +0x010  -1   (DX8RENDERER Format)
    int depth_bpp;      // +0x014  -1   (DX8RENDERER WantedZ)
    int stencil_bpp;    // +0x018  0    (DX8RENDERER WantedStencil)
    int interval;       // +0x01c  1
    int buffer_count;   // +0x020  2
    char border;        // +0x024  1    (the routine "B")
    char fullscreen;    // +0x025  1    (the routine "F"/"W"; read as the field)
    char pad26[2];
    // +0x028 .. +0x140: the debug option table the routine reads (the field); written only
    // by the scatter loop at 0x005621c0. See game_options.h.
    int options[0x46];
    FClientPathString ini_path;       // +0x140  "OutpostFrontier.ini" (text is the field)
    FClientPathString data_path;      // +0x248  ".\\data" / [OutpostFrontier] data path (the field)
    FClientPathString movie_files[3]; // +0x350  [Startup] movie_file, first three only
    FClientPathString gate_tunnels;   // +0x668  [Data] gate_tunnels
    FClientPathString jump_effect;    // +0x770  [Data] jump_effect
    char field_878;                   // +0x878  (the field) cleared by the routine through `this`;
                                      //         read once, by ClientWndProc @0x005b2667 -- modelled
                                      //         there as window.cpp's kActivateOverrideByte (always 0)
    char quick_flag;                  // +0x879  "+Q"/"-Q": set to 1, never read anywhere in the PE
    char pad87a[2];
};
static_assert(sizeof(DisplaySettings) == 0x87c, "options object ends after +0x879");

// WinMain's command-line block (local_5e4, 0x244 bytes of WinMain's frame). the routine zeroes
// Count; the routine copies lpCmdLine into Buffer and splits it in place on " \t", honouring
// double quotes, appending each token to Argv. baseline bounds neither the token count nor the
// copied length, and neither does the port.
struct FClientCommandLine
{
    unsigned Count;     // +0x000
    char* Argv[16];     // +0x004
    char Buffer[0x200]; // +0x044

    FClientCommandLine();
    void Parse(const char* CommandLine);
};
static_assert(sizeof(FClientCommandLine) == 0x244, "WinMain local_5e4 is 0x244 bytes");

struct IEnvironment;

struct ClientGlobals
{
    HINSTANCE instance;
    HWND window;
    HMODULE resources;
    unsigned int packed_version;
    IEnvironment* current_state; // — the current client state (client_environment.h)
};

extern DisplaySettings g_display;
extern ClientGlobals g_client;

// "+X"/"-X" (the routine's second command-line loop); WinMain and the frame
// bracket (the routine / the routine) gate the routine on it.
extern bool G_bFpExceptionTrap;

// the "WinDebug" root window: InitializeEngine stores
// ("WinDebug", "WinDebug", 0, 0) (0x005b1d73), the atexit teardown deletes its tree
// (0x005b223c..0x005b226f, the routine inlined). No WinFactory in the exe registers the class
// "WinDebug" (the only copy of the string, 0x005e6a8c, is referenced by those two pushes alone),
// so the lookup finds nothing and the root is always null in baseline.
struct Win3D;
extern Win3D* G_WinDebugRoot;

// thiscall on the options object at the field, argument WinMain's command line.
void LoadStartupConfig(DisplaySettings* Options, const FClientCommandLine* CommandLine);
// thiscall on a render mode block (the first 0x28 bytes of the DisplaySettings
// layout): applies "+B/-B", "+F/-F", "+W", "+M<size>". True if any of those letters was seen.
bool ApplyDisplayCmdlineOverrides(DisplaySettings* Mode, const FClientCommandLine* CommandLine);

HWND CreateGameWindow(const char* class_name, DisplaySettings* display, const char* title);
// record the surface size/mode and restyle + resize the window for it.
void ApplyGameWindowLayout(HWND Hwnd, bool bFullscreen, unsigned Width, unsigned Height);

// RenderMgr.cpp state.
extern HWND G_RenderMgrWindow;          // the game window (the routine)
extern unsigned G_RenderSurfaceWidth;   // surface size last laid out (0x280)
extern unsigned G_RenderSurfaceHeight;  // (0x1e0)
extern bool G_bRenderSurfaceFullscreen;
extern bool G_bRenderMgrShuttingDown;   // the routine re-entry latch
extern float G_RenderGamma;             // .data 1.3f; "gamma_ramp" option + 1.0
extern int G_bRenderDither;             // .data 1; "DitherControl" option >= 0.3
extern int G_RenderMipFilter;           // .data 1; "MipFilterControlCallback"
// .data 1. Cleared by the routine on entry (0x00424a09) and by the routine when
// a fullscreen surface is re-activated (0x00424c7c); set at the end of a successful
// (0x00424c4f); the routine stores (SwapBuffers == 0) into it (0x00425158).
extern bool G_bRenderPostCreate;
// this frame's BeginScene succeeded. Written only by the routine (which returns
// it); the routine reads it.
extern bool G_bRenderFrameBegun;
// .data 0x35, never written: the FPU precision the renderer wants (0x35 = 53-bit,
// 0x18 = 24-bit). the field -- .data 0x18, the precision the routine found in the control word
// after create_buffers (0x00424b6c). the routine puts the found one in place for the frame,
// puts the wanted one back.
extern int G_FpuPrecisionWanted;
extern int G_FpuPrecisionFound;

// QI the pipeline for IGammaControl; with it, SetGammaEnable(fullscreen) and
// SetGammaFunction(7, G_RenderGamma, 0, 1.0f, 0). True when the function call succeeded.
bool RenderMgrApplyGamma();
// The apply slots (vtable slot 1) of the three RenderMgr.cpp perf options.
void ApplyGammaRampOption(float Value);
void ApplyDitherControlOption(float Value);
void ApplyMipFilterControlOption(float Value);

// release the render device. With bRestoreWindowedMode (the fatal-error and
// crash paths) a fullscreen session is first switched back to a windowed surface.
void ShutdownRenderMgr(bool bRestoreWindowedMode);
// create the device buffers for the render mode in the first 0x28 bytes of Mode;
// the pipeline writes the mode it actually got to Out (the options object at every baseline call).
bool RenderMgrCreateBuffers(HWND Hwnd, const DisplaySettings* Mode, DisplaySettings* Out);
// (cdecl) -- try Mode (its first 0x28 bytes); a windowed mode then retries Format
// 32/24/16, then the options object itself, then a 640x480 window. Callers: InitializeEngine
// (engine_init.cpp) and NN_Preferences::TryApplyVideoMode 0x004b1ce4.
bool SetRenderModeWithFallback(HWND Hwnd, const DisplaySettings* Mode);
LRESULT CALLBACK ClientWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

// — WinMain only presents a frame while this is set (0x5b0fe8). WM_ACTIVATEAPP
// clears it when a fullscreen single-player session loses focus.
bool ClientPresentAllowed();
// — latched by the routine on WM_ACTIVATEAPP.
bool ClientAppIsActive();
// the field && the field && the field. Callers: the loading screen's
// Update (0x00578f8b) and WndProc's SC_CLOSE (0x005b2727).
bool RenderMgrFrameReady();
// / the field — WinMain arms the quit screen once InitializeEngine returns and
// replays a close that arrived earlier.
void ClientNoteEngineReady();
// (0) — raise NN_Exit (nn_exit.h NnExitOpen).
void RequestClientQuitDialog();

bool PumpMessages(int* exit_code);
void UpdateClientTiming();
void TickClientFrameTimer();       // on its own
double GetClientAccumulatedTime();
void SeedClientAccumulatedTime(double Seconds); // -> _DAT_0067ecd8
float GetClientFrameDeltaSeconds();
// The baseline frame-timer accessors around the routine (0042d650..0042d6e0).
double GetClientFrameTimeCapped();      // — 0.0 while paused
double GetClientFrameTimeRaw();         // — 0.0 while paused
double GetClientElapsedTime();          // — not pause-gated
unsigned GetClientFrameCount();
unsigned long GetClientFrameTimestampMs();
void PushClientTimingPause();
void PopClientTimingPause();
void SetClientServerTickPause(bool bPause); // — the field
bool ShouldSkipLocalServerTick();       // — SP && the field > 0
bool IsClientServerTickPaused();        // — the field > 0 (signed, no SP test)
void SetClientFpsCap(float Fps);
bool InitializeEngine(const FClientCommandLine* CommandLine);
void OpenSpewLog();
// — developer FPU-exception-trap toggle, "+X"/"-X"-gated (G_bFpExceptionTrap).
// baseline calls this every frame (Enable=false in TryBeginFrame/the routine, Enable=true at the end
// of FinishPresentFrame's first half/the routine), not just once at WinMain startup.
void SetFpuExceptionTrap(bool Enable);
