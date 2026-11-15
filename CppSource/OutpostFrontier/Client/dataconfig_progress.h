// Author: Giperion.

#pragma once

#include <windows.h>

// (Client\main.cpp) -- the client's own pass over OutpostFrontier.ini after
// DataConfig::startup: the [Data] keys Common.dll leaves to the client (fonts, rich_fonts,
// sounds, voices, effects, effect_shapes, debris, intro, HUD, igraph), the [Resources] DLLs,
// then audio\SoundCFG.ini. Also the routine, the intro.ini [Images] reader it calls.
// DataConfig::startup's progress object is not here: it is InitializeEngine's DataConfigPump
// (engine_init.cpp).

// One intro.ini string field: {length, text}, stride 0x48 in baseline. the routine hands
// a capacity of 0x40 and stores the returned length in front of the text; a null
// value stores length 0 and a zero first byte. Each field's {length, text[0]} pair is zeroed by
// its own static initializer (the branch .. the branch).
struct FIntroPathString
{
    unsigned Length;  // +0x00
    char Text[0x44];  // +0x04  (the routine capacity 0x40)
};
static_assert(sizeof(FIntroPathString) == 0x48, "intro.ini string field stride is 0x48");

// ([Images] of intro.ini). baseline keeps these as separate .data globals; the port
// groups them. ScreenType/ScreenExit/ScreenReset/Screen1-4 are write-only in the PE: each has
// exactly three references (the parse store, the static-init zero, the atexit clear) and no
// reader, so nothing in the port may consume them either.
struct FIntroConfigState
{
    FIntroPathString TgaPath;     // / the field  "TgaPath"
    FIntroPathString ScreenType;  // / the field  "ScreenType"
    FIntroPathString ScreenExit;  // / the field  "ScreenExit"
    FIntroPathString ScreenReset; // / the field  "ScreenReset"
    FIntroPathString Screen[4];   // / c020 / bfd8 / bf90  "Screen1".."Screen4"
    // / the field -- after a successful parse 00564e5c copies
    // "FRONT_OutpostFrontierlogo.tga" (bounded at 0x3f chars) here; the title screen pushes it at
    // .
    FIntroPathString Logo;
    // (rand() * 3) >> 15 after a successful parse (00564e9c); 005738xx picks
    // Intro1_Base / Intro2_Base / Intro3_Base from it.
    int IntroBasePick;
    // PE init 1; only a "bDrawScripts" key rewrites it (00564e20).
    bool bDrawScripts;
};

FIntroConfigState const* GetIntroConfigState();

// , cdecl(ini path); InitializeEngine 0x005b1caa passes the field. Always true.
bool LoadClientIniData(char const* IniPath);
