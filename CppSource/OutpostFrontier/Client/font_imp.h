// Author: Giperion.

#pragma once

// FontImp -- baseline `.?AUFontImp@@` (Font.cpp, vtable module storage, 0x170 bytes). The one
// IFont implementation: a GDI face on its own memory DC, re-realized at a resolution-dependent
// pixel height whenever the layout generation the field moves. Every font lives in the list
// , filled from fonts.ini [TrueType] sections by the routine and searched by
// nickname with the routine.

#include "common_tstring.h"
#include "font.h"

#include <windows.h>

#include <map>

struct IFileSystem;

// The by-value font description the routine reads (its only builder is the routine,
// , which clears all three flags).
struct FFontDesc
{
    char const* Face;  // +0x00 GDI face name
    float FixedHeight; // +0x04 fixed_height (fraction of the screen height)
    bool bBold;        // +0x08 lfWeight 700 instead of 400
    bool bItalic;      // +0x09 lfItalic
    bool bUnderline;   // +0x0a lfUnderline
};

class FontImp : public IFont
{
public:
    // the three strings empty, the scale cache empty, HFONT / realized height /
    // DC / fixed_height / +0xd4 / generation zero. The LOGFONTA and TEXTMETRICA are left as they
    // are (the routine and the routine write them before any read).
    FontImp();
    // is the member-wise copy (three 0x44-byte strings, the scalars, both GDI
    // records, the handles and a deep copy of the cache tree) -- the implicit copy constructor.
    // The list insert the routine copies the stack temporary of the routine into its node.
    FontImp(FontImp const& Other) = default;
    FontImp& operator=(FontImp const& Other) = delete;
    // the HFONT and DC are only forgotten, never deleted (baseline leaks both), the
    // generation is zeroed and the cache tree is freed. Not virtual: baseline's vtable has no
    // destructor slot (11 entries), the list destroys FontImp by value.
    ~FontImp();

    // (RET 4): the id takes the nickname only while it is still empty, the name
    // always takes it (both are the TString<64> assignment).
    void SetName(char const* Nickname);
    // (RET 4): builds the LOGFONTA from the description, creates the DC, runs the
    // autoscale prelude and snapshots tmHeight into +0xd4. Always returns true.
    bool Init(FFontDesc const& Desc);

    char const* Slot00() override;
    char const* GetName() override;
    float GetFixedHeight() override;
    HFONT__* Slot03() override;
    LOGFONTA const* GetLogFont() override;
    float GetGlyphAdvance(wchar_t Ch) override;
    int GetLineHeight() override;
    int Slot07() override;
    void MeasureString(int* OutWH, wchar_t const* Text, int Len) override;
    int ProbeString(int MaxWidth, wchar_t const* Text, int Len) override;
    int GetCharWidths(wchar_t const* Text, int Len, int* OutWidths) override;

private:
    // (RET 4): (re)create the face at Height into +0x118, select it into the DC and
    // read TEXTMETRICA; false (and an FDUMP) when CreateFontIndirectA fails.
    bool Realize(int Height);
    // (RET 8): realize at Height, measure the calibration pangram, return
    // width / Width (FILD qword / FIDIV, kept on the FPU stack).
    double MeasurePangramRatio(int Height, int Width);
    // (RET 0x10): step the pixel height from round(Fixed * ScreenH) one pixel at a
    // time until the pangram-width ratio crosses Target; returns the accumulated step.
    int SearchScaleDelta(float Fixed, int ScreenW, int ScreenH, float Target);
    // the cached pixel delta for the current surface width, searched and cached
    // on a miss with the target pangramWidth(fixed*600) * 0.00125f. baseline inlines the same body
    // into the routine, the branch, the branch and the branch.
    int ComputeScaleDelta();
    // The variant inlined into the branch, the branch and the branch: the same cache, but
    // the target is the routine(round(fixed*600), 800) and the search is the out-of-line
    // .
    int ComputeScaleDeltaByRatio();
    // The prelude at the head of every face slot: when +0x158 differs from the field, take
    // the new generation, base = GetFixedHeight() * (float), delta from one of the
    // two searches above, realize at round(delta + base).
    void RefreshScale(bool bRatioTarget);

    TString<64> Name;                 // +0x04 nickname (GetName)
    TString<64> Id;                   // +0x48 "%s,%d" id (Slot00)
    TString<64> Face;                 // +0x8c GDI face name
    float FixedHeight;                // +0xd0
    int InitLineHeight;               // +0xd4 tmHeight snapshot of the routine (Slot07)
    HDC Dc;                           // +0xd8 CreateCompatibleDC(0)
    LOGFONTA LogFont;                 // +0xdc
    HFONT Font;                       // +0x118
    int RealizedHeight;               // +0x11c lfHeight of the realized face
    TEXTMETRICA Metrics;              // +0x120
    int LayoutGeneration;             // +0x158 vs the field
    std::map<int, int> ScaleDeltaByWidth; // +0x15c surface width -> pixel delta
};

// (cdecl): read the [TrueType] sections of Path (resolved through Filesys) into
// the font list; returns the number of fonts whose the routine succeeded.
int LoadFonts(char const* Path, IFileSystem* Filesys);

// the first font in the list whose GetName() matches Nickname (NEURONCOMPONENTS stricmp),
// else null.
IFont* FindMenuFontImp(char const* Nickname);

// the font subsystem teardown from WinMain's shutdown (0x005b229a) -- the glyph
// scratch, the font list, the atlas page pool, every number face's glyph string and the screen
// quad batch cache.
void FontShutdown();
