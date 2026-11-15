// Author: Giperion.

#pragma once

// baseline PixelFormat helpers (0x005b7000..0x005b70d7) and their two .data tables. Every
// BigImage descriptor, texture-format query and display-mode record in the exe identifies a
// pixel format by the small integer id these tables index.

// 0x16 D3DFORMAT codes, indexed by id (the routine returns the array,
// maps a D3DFORMAT back to its id).
// 15 descriptors of 0x4c bytes, indexed by id (the routine).

// The only code references to either table are those accessors (module storage / 005b7039 /
// ); nothing in the PE writes them after load.

// One colour channel of a descriptor.
struct FPixelFormatChannel
{
    unsigned long Bits;  // +0x00
    unsigned long Mask;  // +0x04
    unsigned long Shift; // +0x08
    unsigned long Loss;  // +0x0c -- 8 - Bits
};

// entry.
struct FPixelFormatDesc
{
    unsigned long Id;             // +0x00
    unsigned long Bpp;            // +0x04
    FPixelFormatChannel Red;      // +0x08
    FPixelFormatChannel Green;    // +0x18
    FPixelFormatChannel Blue;     // +0x28
    FPixelFormatChannel Alpha;    // +0x38
    unsigned long HasAlpha;       // +0x48 -- the routine reads its low byte
};

static_assert(sizeof(FPixelFormatDesc) == 0x4c, "baseline PixelFormat descriptor is 0x4c bytes");

// the field, the 0x16-entry D3DFORMAT array.
unsigned long* PixelFormatGetD3DFormats();

// (cdecl) -- the id whose D3DFORMAT equals D3DFormat, or 0 when none does. Every
// caller seeds a format id with the routine(0), i.e. id 0.
int PixelFormatFromD3D(unsigned long D3DFormat);

// (cdecl) -- the descriptor of Id; an Id above 14 (signed compare) gives entry 0.
FPixelFormatDesc* PixelFormatGetDesc(int Id);

// (thiscall on the id, RET 0x14) -- the first of the first nine descriptors with
// this Bpp, HasAlpha == (AlphaBits != 0), and matching channel widths. Stores its id in *OutId
// and returns true; otherwise stores the routine(0) and returns false.

// baseline quirk kept: the blue width is compared against the descriptor's RED width
// (module storage CMP EDX,ECX with ECX still [EAX+8]), not its blue width.
bool PixelFormatFind(unsigned long* OutId, unsigned long Bpp, unsigned long RedBits,
                     unsigned long GreenBits, unsigned long BlueBits, unsigned long AlphaBits);
