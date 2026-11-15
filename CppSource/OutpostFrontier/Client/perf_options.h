// Author: Giperion.

#pragma once

// PerfOptions.cpp (baseline source path Client\PerfOptions.cpp).

// baseline keeps a process-wide registry of "perf options": named float knobs registered from
// static initializers (the routine), persisted to <user data>\PerfOptions.ini and applied
// through per-option callbacks. Entry points:

// register            the routine  find by name
// find by IDS id      the routine  read one of the 7 shared float slots
// load + apply        the routine  save
// apply everything changed since the last apply
// clear the registry  the routine  max_submitted_sounds accessor

// Call order in baseline InitializeEngine (the routine @ 005b168e):
//   LoadPerfOptions("PerfOptions.ini", &display)
//   if (!ok) { the routine(); ApplyChangedPerfOptions(); }
//   SavePerfOptions("PerfOptions.ini")

struct FPerfDisplaySettings
{
    int Width;      // baseline display struct +0x08
    int Height;     // +0x0c
    int ColorBpp;   // +0x10
    int DepthBpp;   // +0x14
};

// baseline option record is 0x1c bytes: vtable, name, IDS name, IDS info, value, applied,
// callback. The port drops the vtable: baseline's generic class (vtable 005d6d50) forwards
// slot 1 to the +0x18 cdecl callback, and every other option's slot 1 is that option's own
// apply body, so one function pointer covers both. Slot 0 is a no-op for every option in
// the PE, so the "value parsed" hook is dropped too. See implementation notes, PerfOptions.
struct FPerfOption
{
    char const* Name;           // +0x04
    int         IdsName;        // +0x08
    int         IdsInfo;        // +0x0c
    float       Value;          // +0x10  last parsed / assigned
    float       Applied;        // +0x14  last applied; -1 means "never" (baseline 0xbf800000)
    void        (*Apply)(float);
};

// / the routine — exact name match, as in baseline (byte compare, not _stricmp).
FPerfOption* FindPerfOption(char const* Name);
FPerfOption* FindPerfOptionByIds(int IdsName);
unsigned GetPerfOptionCount();
FPerfOption* GetPerfOptionAt(unsigned Index);

// — the seven shared floats at the field that some applies write into.
float GetPerfOptionSlot(int Index);

// — apply every option whose Value drifted from Applied.
void ApplyChangedPerfOptions();

// / the routine. FileName is a bare name; both prepend GetUserDataPath().
// Save takes only the name and always writes [Display] from the options object g_display
// (the field..bd4).
bool LoadPerfOptions(char const* FileName, FPerfDisplaySettings* Display);
void SavePerfOptions(char const* FileName);

// The first-run seed the routine is NnPreferences::SeedGraphicsDefaults (nn_preferences.h):
// optlist walk on a bare NN_Preferences, this probe, then the routine's row cascade.
// the machine's graphics score in {0.25, 0.5, 0.6, 0.75, 1.0}.
float ProbeMachineGraphicsLevel();

bool GetSkipMachineWarnings();
void SetSkipMachineWarnings(bool bSkip);

// (the field) — the mixer's voice budget, default 32.
int GetMaxSubmittedSounds();

// Simple globals the option callbacks write; kept here until their consumers are ported.
bool GetPerfEnableTooltips();
bool GetPerfEnableRollover();
bool GetPerfCockpit();
bool GetPerfAutohideManeuverBar();
bool GetPerfSkipTransitionScripts();
bool GetPerfAdvancedGraphics();
int  GetPerfGeneralGraphics();
int  GetPerfDefaultGraphics();
float GetPerfSpaceDustDetail();
float GetPerfGenericEffectPerf();
