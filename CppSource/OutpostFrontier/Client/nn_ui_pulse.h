// Author: Giperion.

#pragma once

// NeuroNet UI pulse colour (the routine) + phase helpers.

// baseline globals:
// phase accumulator (float seconds)
// direction byte (0/1)
// idle cyan   — the branch → bytes 87,C3,E0,FF
// pulse amber — the branch → bytes F5,EA,52,FF
// mid grey    — lazy 0xFFB9B9B9 inside the routine

// = phase / 0.3; the routine = direction; the routine = reset
// (dir=0, phase=0.3f*0.5f = 0.15). Advance is the head of the routine (phase += dt, wrap
// every 0.3s flipping dir) — not the ToolTip/scene walk that follows.

// — reset pulse on hover/focus enter (the routine).
void NnUiPulseReset();

// Pulse-advance slice of the routine (the field / the field only). baseline passes the
// frame delta as a double (FADD double ptr [ESP+4]); Win3DUpdateAll is the only caller.
void NnUiPulseAdvance(double DeltaSeconds);

// — write blended ARGB into *OutArgb.
void NnUiPulseBlendArgb(unsigned long* OutArgb);

// (cdecl) — the same pulse around a caller colour: BaseArgb takes the idle
// colour's place and a grey made from the Base/amber midpoint takes the mid grey's place.
// 14 baseline callers (UIScroll arrows 005961a3 / 00596323, draw 005965d7, HUD, ...).
void NnUiPulseBlendFromArgb(unsigned long* OutArgb, unsigned long BaseArgb);

// (cdecl) -- the palette colour of a reputation Attitude: at or below -0.6 red
// (the field), at or below -0.2 white (the field), at or above 0.6 green
// (the field), otherwise white (two branches, both the field). Callers: the nav map's
// zone shading (0x0049ce39) and 0x004a82c9.
void NnReputationColorArgb(unsigned long* OutArgb, float Attitude);

// Idle palette colour the field (the branch).
unsigned long NnUiPulseIdleArgb();

// the pulse phase over its period, the field / 0.3f (the field).
float NnUiPulsePhaseNorm();

// the pulse direction byte the field.
unsigned char NnUiPulseDirection();

// the HUD chrome RGBA bytes 87,C3,E0,C0. Its only writer in the PE is the
// static initializer the branch; ~20 sites read it (ToolTip LineLoop, WinControl::Draw
// which scales each byte by 1/255 into the mesh holder's tint, HUD panels).
extern unsigned char G_HudChromeColorRgba[4];

// NeuroNet UI palette -- one 4-byte colour per static initializer, all in this TU's XC run
// (the branch .. the branch). Memory bytes are R,G,B,A, so the little-endian dword is
// 0xAABBGGRR -- which is how most baseline readers load them (MOV r32). None of them has a
// writer in the PE besides its initializer. the branch and the branch seed two more
// slots that nothing reads; they are not declared.
extern unsigned long G_NnPaletteArgb_679bb0; // 87,C3,E0,FF  idle cyan
extern unsigned long G_NnPaletteArgb_679bac; // 3B,BF,1D,FF
extern unsigned long G_NnPaletteArgb_679ba8; // BF,1D,1D,FF
extern unsigned long G_NnPaletteArgb_679ba4; // F5,EA,52,FF  pulse amber
extern unsigned long G_NnPaletteArgb_679b9c; // 88,00,C2,FF
extern unsigned long G_NnPaletteArgb_679b98; // 48,48,E0,FF
extern unsigned long G_NnPaletteArgb_679b94; // 80,80,80,FF  grey
extern unsigned long G_NnPaletteArgb_679b90; // A8,FB,C5,FF
extern unsigned long G_NnPaletteArgb_679b88; // FF,7E,FD,FF
extern unsigned long G_NnPaletteArgb_679b84; // FF,FF,FF,FF  white
extern unsigned long G_NnPaletteArgb_679b80; // CE,5A,FF,FF
