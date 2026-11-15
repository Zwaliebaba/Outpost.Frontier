// Author: Giperion.

#pragma once

// The ToolTip hover record (0x64 bytes): packed on the stack by WinControl slot 34
// (the routine), ButtonControl the routine and WireButton the routine, embedded in every
// BaseRichTextWin at +0x40c (slot 46 the routine returns it), and consumed by the ToolTip
// through the routine -> the routine.

#include "font_string.h"

#include <stddef.h>

struct FToolTipHoverInfo;

// (the record's destructor, ~FToolTipHoverInfo): members in reverse -- the
// Font::String at +0x10 (the routine), then the caption buffer at +0x08 (the routine: free,
// +0x08 / +0x0c zeroed). Defined in cargo_bar_control.cpp.
void ToolTipHoverDestroy(FToolTipHoverInfo* Info);

// Hover blob packed by the routine / the routine and consumed by the routine.
// Layout matches stack locals relative to the pointer passed to the routine.

// RAII holder (AGENTS.md RE rule 13): every baseline site that builds one constructs the caption
// buffer (+0x08 / +0x0c zeroed) and then the Font::String (the routine), and tears the record
// down through the routine (or its inlined body) on the normal and the unwind path -- e.g.
// WinControl slot 34 the routine: construct 0x005a054e..0x005a055e, inlined teardown
// ; FuncInfo_005fed48 unwinds state 0 with the routine on the caption
// (Unwind@005c4f60) and state 1 with the routine on the whole record (Unwind@005c4f58).
// The rest of the record is written by each site (the routine or inline stores); baseline leaves
// it as stack garbage until then, the port starts it at zero.
struct FToolTipHoverInfo
{
    FToolTipHoverInfo()
        : Ids(0), IdsAlt(0), CaptionW(nullptr), CaptionCapacity(0), AuxField(0),
          WorldPos{0.0f, 0.0f, 0.0f}, HalfExtents{0.0f, 0.0f, 0.0f}, bLive(0)
    {
    }
    // . The Font::String member dtor that follows finds it already empty.
    ~FToolTipHoverInfo() { ToolTipHoverDestroy(this); }
    FToolTipHoverInfo(FToolTipHoverInfo const&) = delete;
    FToolTipHoverInfo& operator=(FToolTipHoverInfo const&) = delete;

    int Ids;                     // +0x00  LabelIds
    int IdsAlt;                  // +0x04  IDS used by the routine (via +0xf8)
    wchar_t* CaptionW;           // +0x08  null → resolve via IdsAlt
    unsigned CaptionCapacity;    // +0x0c  wchar count of +0x08 (the routine grows it)
    // +0x10 Font::String -- BaseRichTextWin's embedded copy (+0x40c) constructs it with
    // and tears it down in its dtor (0x0057969d..0x005796b6).
    FFontString Text;            // +0x10 .. +0x43
    int AuxField;                // +0x44  KeyCmd NicknameId (the routine)
    float WorldPos[3];           // +0x48
    float HalfExtents[3];        // +0x54
    unsigned char bLive;         // +0x60
};

static_assert(offsetof(FToolTipHoverInfo, AuxField) == 0x44, "hover +0x44");
static_assert(offsetof(FToolTipHoverInfo, WorldPos) == 0x48, "hover +0x48");
static_assert(offsetof(FToolTipHoverInfo, bLive) == 0x60, "hover +0x60");
static_assert(offsetof(FToolTipHoverInfo, Text) == 0x10, "hover +0x10 Font::String");
static_assert(sizeof(FToolTipHoverInfo) == 0x64, "BaseRichTextWin +0x40c..+0x470");

// guarded forward to the ToolTip singleton's UpdateFromHover.
void NnToolTipFeedHover(FToolTipHoverInfo const* Info);

// (thiscall on the record) -- Ids -1, IdsAlt 0, AuxField 0, the caption emptied
// when it has a non-empty buffer, bLive 0. WorldPos / HalfExtents / Text are untouched.
// Callers: NeuroNetNavMap::Update (4 sites), 0x004b66f1, UiFastTable (0x0058d677 and siblings).
void ToolTipHoverReset(FToolTipHoverInfo* Info);
