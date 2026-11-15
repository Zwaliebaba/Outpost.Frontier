// Author: Giperion.

#pragma once

// baseline LoadingScreen (RTTI .?AVLoadingScreen@@, vftable 0x005e448c) and its
// LoadingProgressBar (.?AVLoadingProgressBar@@, vftable 0x005e44a4), 0x00578b70..0x005792a6.
// One static instance, the field (static init the branch, atexit the branch), which
// InitializeEngine registers with Common.dll (RegisterLoadingScreen, the routine) so every
// RefreshLoadingProgress lands in slot 0.

#include "common_ini.h" // ILoadingScreen
#include "wincontrol_base.h"

struct Image;

// 0x384 bytes (operator new(0x384) in the routine): a WinControl with a "LoadingProgressText"
// TextControl and the FRONT_loadingbar.cmp StaticControl whose material animation the phase
// drives.
class LoadingProgressBar : public WinControl
{
public:
    // The ctor inlined in the routine (0x00578d94..0x00578dd3): WinControl's ctor, +0x338 =
    // 3.0f, +0x380 = 0, vptrs, +0x33c = 0, the field = 0, Flags bit0 cleared, +0x32c = 0,
    // +0x334 = 0. +0x330 is not written.
    LoadingProgressBar();
    // (the routine deleting): vptrs, then WinControl's dtor the routine.
    ~LoadingProgressBar() override;

    // slot 9 the routine (RET 8)
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 11 the branch
    void Update() override;
    // slot 12 the branch
    void UpdateInactive() override;
    // slot 13 the routine (a bare RET): the bar never draws through the Win3D tree; the loading
    // screen's Update draws its two children itself.
    void Draw() override;

    unsigned char Field32c;   // +0x32c -- ctor 0; no other reader or writer in the class
    unsigned char Pad32d[3];  // +0x32d
    float Phase;              // +0x330 -- advanced by dt modulo Period (the routine)
    float Fraction;           // +0x334 -- min(dt, 1.0), handed to the routine
    float Period;             // +0x338 -- ctor 3.0f
    WinControl* Text;         // +0x33c -- "LoadingProgressText"
    wchar_t Caption[0x20];    // +0x340 -- ids 0x7c
    WinControl* Bar;          // +0x380 -- "LoadingProgressBar" StaticControl

private:
    LoadingProgressBar(LoadingProgressBar const&) = delete;
    LoadingProgressBar& operator=(LoadingProgressBar const&) = delete;
};

static_assert(sizeof(void*) != 4 || sizeof(LoadingProgressBar) == 0x384,
              "baseline LoadingProgressBar is 0x384 bytes");

// .
class LoadingScreen : public ILoadingScreen
{
public:
    // (static init): vptr, Screen = 0, bBeginEnd = 1, the field = 0, bClear = 1,
    // bShowBar = 1, bDisabled = 0, ProgressBar = 0.
    LoadingScreen();
    // (atexit): vptr, then the screen's deleting dtor when there is one. Not
    // virtual: the vftable 0x005e448c has only the two slots below.
    ~LoadingScreen();

    // slot 0 the routine -- RefreshLoadingProgress's entry.
    void Refresh() override;
    // slot 1 the branch (ILoadingScreen +0x04, name unrecoverable): the field = 1,
    // Update(-1.0f) -- the body the routine (LoadingScreenStart) runs non-virtually.
    void Slot01() override;

    // (thiscall, RET 4).
    void Update(float DeltaSeconds);
    // build the progress bar once.
    void CreateProgressBar();

    // The vptr is +0x00.
    Image* Screen;                   // +0x04 the field
    LoadingProgressBar* ProgressBar; // +0x08 the field
    unsigned char Pad0c[4];          // +0x0c the field -- no reference in the PE
    long long ResetTicks;            // +0x10 the field (the routine)
    long long LastUpdateTicks;       // +0x18 the field (the routine)
    unsigned char bBeginEnd;         // +0x20 the field -- Update brackets its own scene
    unsigned char bClear;            // +0x21 the field -- clear when there is no Screen
    unsigned char bShowBar;          // +0x22 the field
    unsigned char bDisabled;         // +0x23 the field -- set by the routine

private:
    LoadingScreen(LoadingScreen const&) = delete;
    LoadingScreen& operator=(LoadingScreen const&) = delete;
};

// the bar is running (the routine / slot 1 set it, the routine and the bar's
// ctor clear it). A separate .data byte, not a field of the field.
extern unsigned char G_LoadingScreenStarted;
// / the field -- Refresh's counts of updates run / skipped as too soon.
extern int G_LoadingScreenUpdateCount;
extern int G_LoadingScreenSkipCount;

LoadingScreen& GetLoadingScreen();

// CreateProgressBar on the field. Caller: IntroFocus 0x00574325.
void LoadingScreenCreateProgressBar();
// RegisterLoadingScreen(&the field). Caller: InitializeEngine 0x005b1a06.
void LoadingScreenRegister();
// reset: ResetTicks = now, the field = 0, bClear = 0, bShowBar = 1.
void LoadingScreenReset();
// delete the screen and the bar's tree, then bDisabled = 1. Caller: WinMain
// shutdown 0x005b2195.
void LoadingScreenShutdown();
// (cdecl) -- replace the screen with a new Image and Load(Path) it; false when the
// Image could not be allocated. Caller: InitializeEngine 0x005b1a0c.
bool LoadingScreenSetImage(char const* Path);
// delete the screen (Screen = 0). Caller: IntroFocus 0x0057432a.
void LoadingScreenReleaseImage();
// (cdecl) -- bClear = bValue.
void LoadingScreenSetClear(unsigned char bValue);
// the field = 1, Update(-1.0f) on the field.
void LoadingScreenStart();
