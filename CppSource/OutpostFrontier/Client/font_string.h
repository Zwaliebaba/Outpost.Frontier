// Author: Giperion.

#pragma once

// FFontString — the 0x34 text holder (Font.cpp) embedded at WinControl+0x7c (ToolTip factory
// LEA ESI+0x7c → the routine). It has no vtable and so no RTTI; the port has long called it
// "Font::String", but baseline's `.?AUString@Font@@` is the abstract element its children derive
// from (FontStringElement below).

// NOT LayoutRichText / RenderDisplayList / TextNode / DeviceTRC (RichTextPane RDL path).
// Children: `.?AUFontString@@` (0x2c) / `.?AUFontNumberString@@` (0x34).
// Measure provider: IFont* at +0x1c (FontImp). See epic:font-string.

#include "font.h"
#include "font_atlas.h"

#include <stddef.h>

// The two-word heap wide string the caption builders fill: text and capacity in wide chars
// (terminator included), the head baseline's Font::String and the HUD_Status caption (+0x5d0)
// share, and the low-machine warning text of InitializeEngine (the routine). The owner frees
// Text; the builders only grow it (the routine or the same code inline: realloc when there is
// a buffer, else malloc with a zero first char, then Capacity = need).
struct FHudWideText
{
    wchar_t* Text;          // +0x00
    unsigned Capacity;      // +0x04
};

// (thiscall, RET 4) -- grow to Need wide chars when Capacity is smaller: realloc
// an existing buffer (Capacity = Need), else malloc Need * 2 bytes, Text[0] = 0, Capacity = Need.
void WideTextReserve(FHudWideText* Self, unsigned Need);
// (thiscall) -- a non-null, non-empty Text gets Text[0] = 0.
void WideTextClear(FHudWideText* Self);
// (thiscall) -- a non-null Text is freed and both words zeroed.
void WideTextFree(FHudWideText* Self);
// (thiscall, RET 8) -- a null Source clears a non-empty Text; otherwise Len
// (0xffffffff: wcslen) characters copied after growing the buffer to Len + 1, and [Len] = 0.
void WideTextAssign(FHudWideText* Self, wchar_t const* Source, unsigned Len);

// baseline `.?AUString@Font@@` -- the abstract element both concrete strings derive from (RTTI:
// FontString's hierarchy the field = {FontString, Font::String}; FontNumberString's
// = {FontNumberString, Font::NumberString, Font::String}). It has no data and no
// destructor slot: the last Release deletes the object non-virtually (the routine ->
// , the routine -> the routine). Every baseline reader of a child slot calls these
// ten slots virtually; the slot's kind byte is read only by the routine's reuse test.
class FontStringElement
{
public:
    // +0x00 AddRef (the branch / the branch)
    virtual int AddRef() = 0;
    // +0x04 Release (the routine / the routine)
    virtual int Release() = 0;
    // +0x08 the font (the branch / the routine)
    virtual void BindFont(IFont* Font) = 0;
    // +0x0c (the branch / the branch)
    virtual IFont* GetFont() const = 0;
    // +0x10 (the branch / the branch)
    virtual wchar_t const* GetText() const = 0;
    // +0x14 (the branch / the branch)
    virtual void SetText(wchar_t const* Text) = 0;
    // +0x18 RET 4, returns OutWH (the branch / the branch)
    virtual int* GetSize(int* OutWH) = 0;
    // +0x1c the shadow colour, then slot 8 with 1 (the branch / the branch)
    virtual void SetColor(unsigned const* Argb) = 0;
    // +0x20 the shadow flag (the routine / the routine)
    virtual void SetShadow(unsigned char Value) = 0;
    // +0x24 RET 0xc (the branch / the branch)
    virtual void Paint(void* Origin, void* DrawCtx, void* DrawArg) = 0;

protected:
    FontStringElement() = default;
    ~FontStringElement() = default;
};

struct FFontStringChildSlot
{
    FontStringElement* Child; // +0x00
    // +0x04 0 = FontString (0x2c), nonzero = NumberString (0x34). Written only when a slot is
    // appended (the routine 0x00414729..0x00414745, the copy the routine) and read only by
    // 's reuse test; an in-range replacement leaves it as it was (0x0041471b).
    unsigned char Kind;
    unsigned char Pad[3];
};

static_assert(sizeof(FFontStringChildSlot) == 8, "child slot 8 bytes");

// baseline `.?AUFontString@@` — size 0x2c, vtable the branch.
class FontStringChild final : public FontStringElement
{
public:
    FontStringChild();
    // reached only from Release.
    ~FontStringChild();

    // +0x00 the branch AddRef
    int AddRef() override;
    // +0x04 the routine Release
    int Release() override;
    // +0x08 the branch — bind IFont* into +0x0c
    void BindFont(IFont* Font) override;
    // +0x0c — return Font_ (misnamed GetConnection in analysis)
    IFont* GetFont() const override;
    // +0x10 the branch — return Text_ (+0x04). The NumberString face build reads the glyph
    // string through it (the routine 0x004141e1, the branch 0x00415216).
    wchar_t const* GetText() const override;
    // +0x14 the branch SetText
    void SetText(wchar_t const* Text) override;
    // +0x18 the branch GetSize → Out[0]=W Out[1]=H (needs IFont::MeasureString); EAX = OutWH.
    int* GetSize(int* OutWH) override;
    // +0x1c the branch — Color_ (+0x24) = *Argb, then a tail jump into slot 8 with 1.
    void SetColor(unsigned const* Argb) override;
    // +0x20 the routine — bShadow_ (+0x28) = Value.
    void SetShadow(unsigned char Value) override;
    // +0x24 the branch (RET 0xc) — the atlas pages as screen quads. baseline's second and third
    // arguments are floats (left / right trim of the string box); the void* slots carry their bits.
    void Paint(void* Origin, void* DrawCtx, void* DrawArg) override;

    wchar_t* Text_;     // +0x04
    unsigned Capacity_; // +0x08
    IFont* Font_;       // +0x0c
    int RefCount_;      // +0x10
    FFontAtlasPage* GlyphHead_; // +0x14 atlas page grid (the routine)
    int CachedW_;       // +0x18
    int CachedH_;       // +0x1c
    int LayoutGen_;     // +0x20  vs G_FontLayoutGeneration
    unsigned Color_;    // +0x24  shadow colour, 0xff080808
    unsigned char bShadow_; // +0x28 draw the shadow quad (Paint 0x00413225)
    unsigned char Pad29_[3];
};

static_assert(sizeof(FontStringChild) == 0x2c, "FontString child 0x2c");

// baseline `.?AUFontNumberString@@` — size 0x34, ctor the routine, one vtable of 13 slots at
// (0x005c8d08 is slot 8 of it, not a second vtable). baseline derives it from the
// interface `.?AUNumberString@Font@@` (slots 10..12 below), itself a Font::String; the port folds
// that interface into this class, whose only user is FontStringSetNumberText. The text is a VC6
// vector<char> of glyph indices (0..16 into "-.0123456789MK%/" plus NUL), drawn one quad per
// glyph out of a per-font face atlas (FNumberFace).
class NumberStringChild final : public FontStringElement
{
public:
    NumberStringChild();
    // reached only from Release.
    ~NumberStringChild();

    // +0x00 the branch — ++RefCount_.
    int AddRef() override;
    // +0x04 the routine — --RefCount_; at < 1 the routine + operator delete.
    int Release() override;
    // +0x08 the routine — the face key; a change drops the face.
    void BindFont(IFont* Font) override;
    // +0x0c the branch — return Font_ (+0x14).
    IFont* GetFont() const override;
    // +0x10 the branch — return the field (the shared empty wide string).
    wchar_t const* GetText() const override;
    // +0x14 the branch — RET 4, the text slot does nothing on a NumberString.
    void SetText(wchar_t const* Text) override;
    // +0x18 the branch — {Width_ (sum of glyph advances), Height_ (face glyph height)}.
    int* GetSize(int* OutWH) override;
    // +0x1c the branch — Color_ = *Argb, then a tail jump into slot 8 with 1.
    void SetColor(unsigned const* Argb) override;
    // +0x20 the routine — bShadow_ = Value (the ctor calls it with 1).
    void SetShadow(unsigned char Value) override;
    // +0x24 the branch (RET 0xc) — per-glyph quads into the screen-quad batcher.
    void Paint(void* Origin, void* DrawCtx, void* DrawArg) override;
    // +0x28 the branch (RET 0xc) — _snprintf("%%.%df%c") then slot 12 with (double)Value.
    virtual void FormatFloat(float Value, int Decimals, char Suffix);
    // +0x2c the branch (RET 8) — slot 12 with "%d%c" (the field).
    virtual void FormatInt(int Value, char Suffix);
    // +0x30 the branch (cdecl) — _vsnprintf into 0x20 bytes, then the glyph vector rebuilt
    // through the the field map; LayoutGen_ = 0.
    virtual void Printf(char const* Format, ...);

    unsigned char Alloc04_;      // +0x04 the vector's allocator byte (ctor copies a stack byte)
    unsigned char Pad05_[3];
    unsigned char* GlyphFirst_;  // +0x08 vector<char> begin
    unsigned char* GlyphLast_;   // +0x0c end
    unsigned char* GlyphEnd_;    // +0x10 capacity end
    IFont* Font_;                // +0x14 the face key
    int RefCount_;               // +0x18
    unsigned Color_;             // +0x1c shadow colour, ctor 0xff080808
    unsigned char bShadow_;      // +0x20 draw the shadow quad (the routine)
    unsigned char Pad21_[3];
    int LayoutGen_;              // +0x24 vs G_FontLayoutGeneration (GetSize cache)
    int Width_;                  // +0x28 sum of advances
    int Height_;                 // +0x2c face glyph height
    void* FaceCache_;            // +0x30 the routine face* (FNumberFace*)
};

static_assert(offsetof(NumberStringChild, GlyphFirst_) == 0x08, "NumberString glyphs +0x08");
static_assert(offsetof(NumberStringChild, Font_) == 0x14, "NumberString key +0x14");
static_assert(offsetof(NumberStringChild, RefCount_) == 0x18, "NumberString RefCount +0x18");
static_assert(offsetof(NumberStringChild, bShadow_) == 0x20, "NumberString shadow +0x20");
static_assert(offsetof(NumberStringChild, LayoutGen_) == 0x24, "NumberString gen +0x24");
static_assert(offsetof(NumberStringChild, FaceCache_) == 0x30, "NumberString Face +0x30");
static_assert(sizeof(NumberStringChild) == 0x34, "NumberString 0x34");

// One glyph's texture rect in the face atlas (the routine 00414228..00414263).
struct FNumberGlyphUv
{
    float U0; // +0x00 previous glyph's U1 (0 for the first)
    float V0; // +0x04 always 0
    float U1; // +0x08 running advance sum / page width
    float V1; // +0x0c page used height / page height
};
static_assert(sizeof(FNumberGlyphUv) == 0x10, "glyph uv 0x10");

// — refcounted 0x164 face keyed by the NumberString's font (+0x14).
struct FNumberFace
{
    FontStringChild* Glyph;   // +0x00 FontString holding "-.0123456789MK%/" (the routine)
    int LayoutGen;            // +0x04 zeroed by the routine (new face) and the routine (clear)
    int Advance[17];          // +0x08 IFont slot 10 widths, one per glyph index
    FNumberGlyphUv Uv[17];    // +0x4c
    IFont* FontKey;           // +0x15c
    int RefCount;             // +0x160
};
static_assert(offsetof(FNumberFace, Advance) == 0x08, "face advances +0x08");
static_assert(offsetof(FNumberFace, Uv) == 0x4c, "face uv +0x4c");
static_assert(offsetof(FNumberFace, FontKey) == 0x15c, "face FontKey");
static_assert(offsetof(FNumberFace, RefCount) == 0x160, "face RefCount");
static_assert(sizeof(FNumberFace) == 0x164, "face 0x164");

FNumberFace* NumberFaceGetOrCreate(IFont* FontKey);
void NumberFaceRelease(FNumberFace* Face);
// The walk over the field inlined in the routine (004180d3..00418116): for every face,
// (release +0, zero +4). Faces and nodes stay.
void NumberFaceResetAll();

struct FFontString;
void FontStringConstruct(FFontString* Self);
void FontStringDestroy(FFontString* Self);

// Font::String owns its text buffer and its child slots, so it is a RAII holder (AGENTS.md RE
// rule 13): baseline constructs it with the routine and tears it down with the routine on both
// the normal and the unwind path of every frame and object that holds one (e.g. the hover
// records' Unwind@ funclets around the routine / the routine, TextWin's member dtor
// ). The baseline class has no copy: every copy site is a hand-written
// member walk (UIFastTable the routine).
struct FFontString
{

    FFontString() { FontStringConstruct(this); }

    ~FFontString() { FontStringDestroy(this); }
    FFontString(FFontString const&) = delete;
    FFontString& operator=(FFontString const&) = delete;

    wchar_t* Text;              // +0x00
    unsigned Capacity;          // +0x04
    int Field08;                // +0x08  sentinel 0x7fff
    unsigned char Field0C;      // +0x0c
    unsigned char Pad0D[3];
    FFontStringChildSlot* ChildBegin; // +0x10
    FFontStringChildSlot* ChildEnd;   // +0x14
    FFontStringChildSlot* ChildCap;   // +0x18
    IFont* FontIface;           // +0x1c
    int ExtentW;                // +0x20
    int ExtentH;                // +0x24
    int LayoutGen;              // +0x28
    int ScaleKey;               // +0x2c
    unsigned char bMultiLine;   // +0x30
    unsigned char Pad31[3];
};

static_assert(offsetof(FFontString, FontIface) == 0x1c, "Font::String +0x1c");
static_assert(sizeof(FFontString) == 0x34, "Font::String size 0x34");

void FontStringClearContents(FFontString* Self);
// Release on every non-null child in [First, Last); the slots stay as they are.
void FontStringChildSlotsDestroy(FFontStringChildSlot* First, FFontStringChildSlot* Last);
// the child vector's destructor: children released, buffer freed, zeroed.
void FontStringChildVectorDestroy(FFontString* Self);
// Font::String's destructor (~FFontString): the routine, the child vector's
// destructor, the text buffer freed. It leaves the string empty, so running it again is a no-op.
void FontStringSetText(FFontString* Self, wchar_t const* Text);
void FontStringSetScale(FFontString* Self, float Param1, float Param2);
void FontStringSetFont(FFontString* Self, IFont* Font);
// (thiscall, RET 8; the first argument is always 0 and never read) — when the
// wide text is non-empty it is freed and every child released (the the routine body, gated);
// then the NumberString child at 0 (the routine(0, 1)) gets Format through slot 12 (so it
// is a printf format), and ExtentW / ExtentH are rebuilt as max width / summed height over the
// children's slot 6. Sole caller: TextControl the routine.
void FontStringSetNumberText(FFontString* Self, char const* Format);
void FontStringRebuild(FFontString* Self);
int* FontStringGetExtent(FFontString* Self, int* OutWH);
// the child (line) count after a stale layout is rebuilt; 0 without children.
// Caller: NN_News RefreshRows (0x004a53be), which moves a one-line headline down.
int FontStringChildCount(FFontString* Self);

// thiscall: ECX = draw origin (not Font::String).
// Paint (00412e50) FLD [Origin] / [Origin+4] — both floats.
struct FFontStringDrawOrigin
{
    float X;     // +0x00
    float Y;     // +0x04
    float Scale; // +0x08 box width: ftol -> ScaleKey (004143e7); the routine aligns X in it
    float Height; // +0x0c box height; the routine aligns Y in it
    unsigned Field10; // +0x10 horizontal mode (the routine): 1 centre, 2 right, else left
    unsigned Field14; // +0x14 vertical mode: 1 centre, 2 bottom, else top
};

void FontStringDrawChildren(FFontStringDrawOrigin* Origin, FFontString* Self, void* DrawCtx,
                            void* DrawArg);

// — find-or-create child at Index (Kind 0 = FontString).
FontStringElement* FontStringEnsureChild(FFontString* Self, unsigned Index, unsigned char Kind);
NumberStringChild* FontStringEnsureNumberChild(FFontString* Self, unsigned Index);

extern int G_FontLayoutGeneration;
