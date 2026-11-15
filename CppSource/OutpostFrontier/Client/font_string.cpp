// Author: Giperion.

#include "font_string.h"
#include "msvcrt_numeric_format.h"

#include "font_atlas.h"
#include "font_imp.h"
#include "game_options.h"
#include "globals.h"
#include "screen_quad_state.h"
#include "win3d_engine_submit.h" // GetWin3DSceneCamera (the field)

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <map>
#include <new>
#include <vector>
#include <windows.h>

// ---------------------------------------------------------------------------
// baseline mirrors (Font.cpp BSS)
// ---------------------------------------------------------------------------

// .data initial value 1 (every cached layout starts at 0, so the first use always
// rebuilds). Its only writer is the rendermgr bump at the routine (INC, wrapping 0 back to 1).
int G_FontLayoutGeneration = 1;

// — NumberString paint kill switch (XC 0).
unsigned char G_NumberStringPaintDisable = 0;

namespace
{

// Defined further down in this file's second anonymous block; FontStringChild::Paint uses both.
FFontAtlasPage* FontStringChildEnsurePages(FontStringChild* Glyph, unsigned char bSkipSizeCheck);
unsigned NumberPackColor(unsigned char Alpha, unsigned Source);

// VC6 std::map<unsigned, NumberFace*>: node key at +0xc compared unsigned
// (SETC at 0046d6f7, JNC at 00432263), value at +0x10. Built by XC thunk the branch
// (the routine) and torn down by its atexit thunk the branch, which frees the tree nodes
// only -- a face still referenced at exit is not deleted.
std::map<unsigned, FNumberFace*> G_NumberFaceMap;

wchar_t* AllocWideBytes(unsigned ByteCount)
{
    return static_cast<wchar_t*>(std::malloc(ByteCount));
}

wchar_t* ReallocWide(wchar_t* P, unsigned ByteCount)
{
    return static_cast<wchar_t*>(std::realloc(P, ByteCount));
}

void FreeWide(wchar_t* P)
{
    std::free(P);
}

// (cdecl: font, text, width, lines, &count, max lines) -- greedy word-wrap via
// IFont::GetGlyphAdvance (+0x14). baseline's lines are one array of 8-byte {wchar_t*, capacity}
// elements; the port passes the two halves as parallel arrays. Nothing is null-tested.
int FontStringWordWrap(IFont* Font, wchar_t const* Text, float MaxWidth,
                       wchar_t** LinePtrs, unsigned* LineCaps, int* OutCount, int MaxLines)
{
    // / 0x004183a5: the length (signed); none -> count 0.
    int Remain = static_cast<int>(wcslen(Text));
    int LineCount = 0;
    if (Remain <= 0)
    {
        *OutCount = LineCount;
        return LineCount;
    }
    wchar_t const* Cursor = Text;
    wchar_t LineBuf[0x1000]; // [ESP+0x2c], 0x2000 bytes

    // Every line is measured and consumed even past MaxLines; only the store is skipped.
    do
    {
        // per line the full width, not done, first chunk, the buffer.
        wchar_t* Out = LineBuf;
        bool bLineDone = false;
        bool bFirstChunk = true;
        float WidthLeft = MaxWidth;
        do
        {
            // one chunk up to a break. The advance comes back on the
            // FPU; a NUL pops it uncounted; CR/LF and space/tab are counted and break; any other
            // character is counted while chunk + advance (unrounded) is not greater than the
            // width left (TEST AH,0x41 / JNP: less or equal goes on), else overflow.
            int Taken = 0;
            float Accum = 0.0f;
            int BreakKind = -1; // 0 space/tab, 1 CR/LF, 2 NUL, 3 overflow
            bool bHitBreak = false;
            do
            {
                wchar_t const Ch = Cursor[Taken];
                float const Adv = Font->GetGlyphAdvance(Ch);
                if (Ch == L'\0')
                {
                    BreakKind = 2;
                    break;
                }
                if (Ch == L'\r' || Ch == L'\n')
                {
                    BreakKind = 1;
                    bHitBreak = true;
                }
                else if (Ch == L' ' || Ch == L'\t')
                {
                    BreakKind = 0;
                    bHitBreak = true;
                }
                else if (!(static_cast<double>(Accum) + static_cast<double>(Adv) <=
                           static_cast<double>(WidthLeft)))
                {
                    BreakKind = 3;
                    break;
                }
                ++Taken;
                Accum = static_cast<float>(static_cast<double>(Adv) + static_cast<double>(Accum));
            } while (!bHitBreak);

            // an overflow chunk after the first is left for the next
            // line; otherwise its characters minus CR/LF are copied, the width and the length
            // reduced and the buffer terminated.
            if (bFirstChunk || BreakKind != 3)
            {
                for (int Left = Taken; Left > 0; --Left)
                {
                    wchar_t const Ch = *Cursor;
                    if (Ch != L'\r' && Ch != L'\n')
                    {
                        *Out = Ch;
                        ++Out;
                    }
                    ++Cursor;
                }
                WidthLeft = static_cast<float>(static_cast<double>(WidthLeft) -
                                               static_cast<double>(Accum));
                Remain = Remain - Taken;
                *Out = L'\0';
            }
            // .
            if (BreakKind != 0)
            {
                bLineDone = true;
            }
            bFirstChunk = false;
        } while (!bLineDone);

        // stored only below MaxLines (signed JGE): the element grown to
        // length + 1 when short (realloc, or malloc with its first character cleared), then
        // wcsncpy and the terminator.
        if (LineCount < MaxLines)
        {
            unsigned const Len = static_cast<unsigned>(wcslen(LineBuf));
            unsigned const Need = Len + 1u;
            if (Need > LineCaps[LineCount])
            {
                if (LinePtrs[LineCount] != nullptr)
                {
                    LinePtrs[LineCount] = ReallocWide(LinePtrs[LineCount], Need * 2u);
                }
                else
                {
                    LinePtrs[LineCount] = AllocWideBytes(Need * 2u);
                    LinePtrs[LineCount][0] = L'\0';
                }
                LineCaps[LineCount] = Need;
            }
            wcsncpy(LinePtrs[LineCount], LineBuf, Len);
            LinePtrs[LineCount][Len] = L'\0';
            ++LineCount;
        }
    } while (Remain > 0); // , JG

    *OutCount = LineCount;
    return LineCount;
}

// (cdecl: Dest, Source) -- a child slot's copy constructor run in place: a null
// Dest (the placement new's own test, 0x00418f2e) does nothing; otherwise the child pointer
// and the kind byte are copied and a non-null child AddRef'd (vt+0, 0x00418f4a..0x00418f52).
void ChildSlotCopyConstruct(FFontStringChildSlot* Dest, FFontStringChildSlot const& Source)
{
    if (Dest == nullptr)
        return;
    Dest->Child = Source.Child;
    Dest->Kind = Source.Kind;
    if (Dest->Child != nullptr)
        Dest->Child->AddRef();
}

// (thiscall on the child vector at Font::String +0xc, RET 0xc: Where, Count,
// Value) -- VC6 vector<child slot>::insert(Where, Count, Value). Both callers pass Where =
// last (0x00414740 PUSH [EDI+8], 0x00415766 PUSH [EBX+8]), so the element moves around Where
// copy nothing and only these steps are left:
//  - room for Count ((end - last) / 8, unsigned JNC at 0x0041862e): Count copies of Value
//    built at last (the routine, 0x004187c8..0x004187d6), last += Count.
//  - no room: the new capacity is Count, or the size when a buffer exists and the size is
//    larger, plus the size (0x00418634..0x00418655; negative -> 0, 0x0041865d); the old
//    slots, then the Count copies are copy-constructed into operator new(cap * 8)
//    (0x00418686..0x004186c2); every old slot is destroyed (a non-null child's vt+4,
// ) and the old buffer freed; end = new + cap, last = new + Count
//    (no old buffer) or new + old size + Count, first = new.
void ChildVectorInsertAtEnd(FFontString* Self, unsigned Count, FFontStringChildSlot const& Value)
{
    FFontStringChildSlot* const OldFirst = Self->ChildBegin;
    FFontStringChildSlot* const OldLast = Self->ChildEnd;
    unsigned const Room = static_cast<unsigned>(Self->ChildCap - OldLast);
    if (Room < Count)
    {
        unsigned const Size = (OldFirst != nullptr) ? static_cast<unsigned>(OldLast - OldFirst) : 0u;
        unsigned Grow = Count;
        if (OldFirst != nullptr && Grow < Size)
            Grow = Size;
        int NewCap = static_cast<int>(Grow + Size);
        if (NewCap < 0)
            NewCap = 0;
        FFontStringChildSlot* const NewBuf = static_cast<FFontStringChildSlot*>(
            operator new[](static_cast<unsigned>(NewCap) * sizeof(FFontStringChildSlot)));
        FFontStringChildSlot* Out = NewBuf;
        for (FFontStringChildSlot* It = OldFirst; It != OldLast; ++It, ++Out)
            ChildSlotCopyConstruct(Out, *It);
        for (unsigned Index = 0; Index < Count; ++Index, ++Out)
            ChildSlotCopyConstruct(Out, Value);
        for (FFontStringChildSlot* It = OldFirst; It != OldLast; ++It)
        {
            if (It->Child != nullptr)
                It->Child->Release();
        }
        operator delete[](OldFirst);
        Self->ChildCap = NewBuf + NewCap;
        Self->ChildEnd = (OldFirst == nullptr) ? NewBuf + Count : NewBuf + Size + Count;
        Self->ChildBegin = NewBuf;
        return;
    }
    for (unsigned Index = 0; Index < Count; ++Index)
        ChildSlotCopyConstruct(OldLast + Index, Value);
    Self->ChildEnd = OldLast + Count;
}

// Glyph-list free — DestroyTexture pages (baseline returns to the field pool).
void FreeGlyphList(FontStringChild* Child)
{
    if (Child->GlyphHead_ != nullptr)
    {
        FontAtlasFreePages(Child->GlyphHead_);
    }
}

void ReleaseChildSlot(FFontStringChildSlot& Slot)
{
    if (Slot.Child == nullptr)
    {
        return;
    }
    Slot.Child->Release();
    Slot.Child = nullptr;
}

// A child slot held by value on the stack: the argument the routine copies from. Built as
// {child, kind} with the child AddRef'd when non-null; the routine is its destructor (a
// non-null child's vt+4, tail-jumped), which baseline reaches through the EH unwind funclets
// Unwind@005b8826 ([EBP-0x14] in the routine) and Unwind@005b8899 ([EBP-0x201c] in
// ) and runs inline on the normal path (0x00414750..0x00414762). A local of this
// type gives both routes.
struct FFontStringChildTemp
{
    FFontStringChildTemp(FontStringElement* InChild, unsigned char InKind)
    {
        Slot.Child = InChild;
        Slot.Kind = InKind;
        Slot.Pad[0] = 0;
        Slot.Pad[1] = 0;
        Slot.Pad[2] = 0;
        if (Slot.Child != nullptr)
            Slot.Child->AddRef();
    }

    ~FFontStringChildTemp()
    {
        // MOV ECX,[ECX]; TEST; JZ; JMP [vt+4].
        ReleaseChildSlot(Slot);
    }

    FFontStringChildTemp(FFontStringChildTemp const&) = delete;
    FFontStringChildTemp& operator=(FFontStringChildTemp const&) = delete;

    FFontStringChildSlot Slot;
};

} // namespace

// ---------------------------------------------------------------------------
// The {Text, Capacity} wide-string head (FHudWideText)
// ---------------------------------------------------------------------------

void WideTextReserve(FHudWideText* Self, unsigned Need)
{
    // (module storage-004142e8): CMP Need,[+4] / JBE -- nothing to do when it fits.
    if (Need <= Self->Capacity)
        return;
    if (Self->Text != nullptr)
    {
        // realloc(Text, Need * 2); the result is stored unchecked.
        Self->Text = static_cast<wchar_t*>(realloc(Self->Text, Need * 2));
        Self->Capacity = Need;
        return;
    }
    // malloc(Need * 2), first char 0.
    Self->Text = static_cast<wchar_t*>(malloc(Need * 2));
    Self->Text[0] = 0;
    Self->Capacity = Need;
}

void WideTextClear(FHudWideText* Self)
{
    // (module storage-00414291).
    if (Self->Text != nullptr && Self->Text[0] != 0)
        Self->Text[0] = 0;
}

void WideTextFree(FHudWideText* Self)
{
    // (module storage-00444e41).
    if (Self->Text != nullptr)
    {
        free(Self->Text);
        Self->Text = nullptr;
        Self->Capacity = 0;
    }
}

// ---------------------------------------------------------------------------
// FontStringChild — `.?AUFontString@@` size 0x2c (the branch)
// ---------------------------------------------------------------------------

FontStringChild::FontStringChild()
    : Text_(nullptr)
    , Capacity_(0)
    , Font_(nullptr)
    , RefCount_(1)
    , GlyphHead_(nullptr)
    , CachedW_(0)
    , CachedH_(0)
    , LayoutGen_(0)
    , Color_(0xff080808u)
    , bShadow_(1) // (1) from the routine (0x0041480f)
{
    Pad29_[0] = 0;
    Pad29_[1] = 0;
    Pad29_[2] = 0;
}

FontStringChild::~FontStringChild()
{
    FreeGlyphList(this);
    FreeWide(Text_);
    Text_ = nullptr;
    Capacity_ = 0;
}

int FontStringChild::AddRef()
{

    ++RefCount_;
    return RefCount_;
}

int FontStringChild::Release()
{
    // → the routine cleanup + operator_delete
    int const Next = --RefCount_;
    if (Next < 1)
    {
        delete this;
        return Next;
    }
    return Next;
}

void FontStringChild::BindFont(IFont* Font)
{
    // — on change: free glyph list, LayoutGen_=0, store Font_
    if (Font_ == Font)
    {
        return;
    }
    FreeGlyphList(this);
    LayoutGen_ = 0;
    Font_ = Font;
}

IFont* FontStringChild::GetFont() const
{
    // (mis-tagged CGunWrapper::GetConnection in analysis)
    return Font_;
}

wchar_t const* FontStringChild::GetText() const
{
    // MOV EAX,[ECX+4] (shared by ICF with the vtable at 0x005cacf8).
    return Text_;
}

void FontStringChild::SetText(wchar_t const* Text)
{
    // (RET 4). 0x0041492a..0x0041495d: the same pointer, or wcscmp(new, old) with
    // a null on either side read as the field (L"") equal -> nothing.
    wchar_t const* Incoming = Text;
    if (Text_ == Incoming)
    {
        return;
    }
    wchar_t const* Left = Incoming != nullptr ? Incoming : L"";
    wchar_t const* Right = Text_ != nullptr ? Text_ : L"";
    if (wcscmp(Left, Right) == 0)
    {
        return;
    }

    // the page grid (+0x14) back to the the field pool, +0x14 = 0.
    FreeGlyphList(this);

    if (Incoming == nullptr)
    {
        // a non-empty text gets [0] = 0; +0x20 = 0.
        if (Text_ != nullptr && Text_[0] != L'\0')
        {
            Text_[0] = L'\0';
        }
        LayoutGen_ = 0;
        return;
    }

    // wcslen; Len + 1 above the capacity (unsigned JBE) -> realloc an
    // existing buffer or malloc a new one with [0] = 0 (neither result tested), capacity =
    // Len + 1; wcsncpy(Len), [Len] = 0, +0x20 = 0.
    size_t const Len = wcslen(Incoming);
    unsigned const Need = static_cast<unsigned>(Len + 1u);
    if (Need > Capacity_)
    {
        if (Text_ != nullptr)
        {
            Text_ = ReallocWide(Text_, Need * sizeof(wchar_t));
        }
        else
        {
            Text_ = AllocWideBytes(Need * sizeof(wchar_t));
            Text_[0] = L'\0';
        }
        Capacity_ = Need;
    }
    wcsncpy(Text_, Incoming, Len);
    Text_[Len] = L'\0';
    LayoutGen_ = 0;
}

int* FontStringChild::GetSize(int* OutWH)
{
    // (RET 4; returns OutWH in EAX, which the routine reads through).
    // a current generation (+0x20 == the field) returns the cached size.
    // the generation is stamped before the font test, so a null font (+0x0c) also
    // just returns the cached size.
    if (LayoutGen_ != G_FontLayoutGeneration)
    {
        LayoutGen_ = G_FontLayoutGeneration;
        if (Font_ != nullptr)
        {
            // IFont vt+0x20 (MeasureString) with the text pointer as it is
            // (FontImp measures a null string as 0 x 0) and -1.
            int Measure[2];
            Font_->MeasureString(Measure, Text_, -1);
            // a changed width or height sends the page grid (+0x14) back
            // to the the field pool (inlined the routine, 00414a8f..00414c26) and stores
            // the new size.
            if (Measure[0] != CachedW_ || Measure[1] != CachedH_)
            {
                FreeGlyphList(this);
                CachedW_ = Measure[0];
                CachedH_ = Measure[1];
            }
        }
    }
    // the pair out, EAX = OutWH.
    OutWH[0] = CachedW_;
    OutWH[1] = CachedH_;
    return OutWH;
}

void FontStringChild::SetColor(unsigned const* Argb)
{
    // [+0x24] = *Argb (00414838), the argument slot overwritten with 1 and a
    // tail jump through vt+0x20 (00414843).
    Color_ = *Argb;
    SetShadow(1);
}

void FontStringChild::SetShadow(unsigned char Value)
{
    // [+0x28] = the byte argument, RET 4.
    bShadow_ = Value;
}

void FontStringChild::Paint(void* Origin, void* DrawCtx, void* DrawArg)
{
    // (module storage-00413532, RET 0xc; the range sits outside the analysis function
    // inventory -- see implementation notes). baseline's second and third arguments are floats, the
    // left and right trim of the string box (list_box.cpp passes its overflow as the third);
    // the port's void* slots carry their bits.
    float TrimLeft = 0.0f;
    float TrimRight = 0.0f;
    std::memcpy(&TrimLeft, &DrawCtx, sizeof(float));
    std::memcpy(&TrimRight, &DrawArg, sizeof(float));
    FFontStringDrawOrigin const* const Org = static_cast<FFontStringDrawOrigin const*>(Origin);

    // the routine(8) "TEXT", a null or empty text -- nothing.
    if (GetGameOption(kGameOptText) == 0)
        return;
    if (Text_ == nullptr || Text_[0] == L'\0')
        return;

    // the shadow offset -- 2 above an 800-wide surface (the field,
    // signed CMP / SETG), else 1; FILD / FSTP float.
    float const Scale =
        static_cast<float>(static_cast<int>(G_RenderSurfaceWidth) > 0x320 ? 2 : 1);
    // floor(x), floor(y) stored as floats.
    float OriginX = static_cast<float>(std::floor(static_cast<double>(Org->X)));
    float const OriginY = static_cast<float>(std::floor(static_cast<double>(Org->Y)));
    // this->vt+0x18 into {w, h}.
    int Size[2];
    GetSize(Size);

    // the horizontal window starts as [0, the field - 1]; the trims
    // go through floor + _ftol.
    int ClipLeft = 0;
    int ClipRight = static_cast<int>(G_RenderSurfaceWidth) - 1;
    int const TrimLeftInt = static_cast<int>(std::floor(static_cast<double>(TrimLeft)));
    int const TrimRightInt = static_cast<int>(std::floor(static_cast<double>(TrimRight)));
    // JG on the right trim, JLE on the left -- either one positive.
    if (TrimRightInt > 0 || TrimLeftInt > 0)
    {
        // right = (int)(w + (x - left) - right), w FILD as a
        // zero-extended qword, clamped to [0, the field - 1].
        double const Shifted = static_cast<double>(OriginX) - static_cast<double>(TrimLeftInt);
        ClipRight = static_cast<int>(static_cast<double>(static_cast<unsigned>(Size[0])) +
                                     Shifted - static_cast<double>(TrimRightInt));
        if (ClipRight < 0)
            ClipRight = 0;
        if (ClipRight > static_cast<int>(G_RenderSurfaceWidth) - 1)
            ClipRight = static_cast<int>(G_RenderSurfaceWidth) - 1;
        // with a positive left trim the window starts at the old x
        // (_ftol, at least 0) and the string moves left by the trim (x - left stored as float).
        if (TrimLeftInt > 0)
        {
            ClipLeft = static_cast<int>(OriginX);
            OriginX = static_cast<float>(Shifted);
            if (ClipLeft < 0)
                ClipLeft = 0;
        }
    }

    // the routine(the field).
    ScreenQuadSetMaterialKey(FontAtlasMaterialKey());
    // a stale generation returns the page grid to the the field
    // pool, an empty grid is rasterised with the routine(1); still empty -- nothing.
    FFontAtlasPage* Row = FontStringChildEnsurePages(this, 1);
    if (Row == nullptr)
        return;

    // the window as floats (FILD / FSTP).
    float const ClipLeftF = static_cast<float>(ClipLeft);
    float const ClipRightF = static_cast<float>(ClipRight);
    for (; Row != nullptr; Row = Row->NextY)
    {
        for (FFontAtlasPage* Col = Row; Col != nullptr; Col = Col->NextX)
        {
            // the page quad at floor(origin) + page offset - 0.5
            // (the field), the offsets FILD as zero-extended qwords.
            float const X0 = static_cast<float>(
                static_cast<double>(static_cast<unsigned>(Col->SrcX)) + OriginX - 0.5);
            float const Y0 = static_cast<float>(
                static_cast<double>(static_cast<unsigned>(Col->SrcY)) + OriginY - 0.5);
            // UVs {0, 0} .. {UsedW / PageW, UsedH / PageH} (FIDIV by
            // the signed page size).
            float U0 = 0.0f;
            float V0 = 0.0f;
            float U1 = static_cast<float>(static_cast<double>(Col->UsedW) /
                                          static_cast<double>(static_cast<int>(Col->PageW)));
            float V1 = static_cast<float>(static_cast<double>(Col->UsedH) /
                                          static_cast<double>(static_cast<int>(Col->PageH)));
            // the routine(frame, 2, 0) for every page, before any
            // clipping. baseline writes only the frame's texture dword; the four rect dwords are
            // stack contents. The records carry their own UVs, so the port passes zeros there.
            unsigned long Frame[5] = {
                static_cast<unsigned long>(reinterpret_cast<uintptr_t>(Col->Texture)), 0ul, 0ul,
                0ul, 0ul};
            ScreenQuadBeginBatchFrame(Frame, 2, false);
            // the far corner, and the shadow quad offset by the scale
            // from the unclipped corners.
            float ClipX0 = X0;
            float ClipY0 = Y0;
            float X1 = static_cast<float>(static_cast<double>(Col->UsedW) + X0);
            float Y1 = static_cast<float>(static_cast<double>(Col->UsedH) + Y0);
            float ShadowX0 = static_cast<float>(static_cast<double>(X0) + Scale);
            float ShadowY0 = static_cast<float>(static_cast<double>(Y0) + Scale);
            float ShadowX1 = static_cast<float>(static_cast<double>(X1) + Scale);
            float ShadowY1 = static_cast<float>(static_cast<double>(Y1) + Scale);
            // the routine on x against the window, span (x1 - x0) + 1;
            // a page clipped away goes on to the next one.
            if (ScreenQuadClip1D(&ClipX0, &X1, &U0, &U1, ClipLeftF, ClipRightF,
                                 static_cast<float>(static_cast<double>(X1) - X0 + 1.0)) == 0)
                continue;
            // on y against [0, the field - 1]; a page clipped away
            // here ends the whole paint (JE 0x00413528).
            if (ScreenQuadClip1D(&ClipY0, &Y1, &V0, &V1, 0.0f,
                                 static_cast<float>(static_cast<int>(G_RenderSurfaceHeight) - 1),
                                 static_cast<float>(static_cast<double>(Y1) - ClipY0 + 1.0)) == 0)
                return;

            // with +0x28 set, the shadow quad first -- its own UVs,
            // clipped on x against the window and on y against the surface; colour = the tint's
            // alpha (the field) over this string's colour bytes (+0x24, +0x25, +0x26).
            if (bShadow_ != 0)
            {
                float ShadowU0 = 0.0f;
                float ShadowV0 = 0.0f;
                float ShadowU1 = static_cast<float>(
                    static_cast<double>(Col->UsedW) /
                    static_cast<double>(static_cast<int>(Col->PageW)));
                float ShadowV1 = static_cast<float>(
                    static_cast<double>(Col->UsedH) /
                    static_cast<double>(static_cast<int>(Col->PageH)));
                if (ScreenQuadClip1D(&ShadowX0, &ShadowX1, &ShadowU0, &ShadowU1, ClipLeftF,
                                     ClipRightF,
                                     static_cast<float>(static_cast<double>(ShadowX1) -
                                                        ShadowX0 + 1.0)) != 0 &&
                    ScreenQuadClip1D(&ShadowY0, &ShadowY1, &ShadowV0, &ShadowV1, 0.0f,
                                     static_cast<float>(static_cast<int>(G_RenderSurfaceHeight) - 1),
                                     static_cast<float>(static_cast<double>(ShadowY1) -
                                                        ShadowY0 + 1.0)) != 0)
                {
                    unsigned const Color = NumberPackColor(
                        static_cast<unsigned char>(FontAtlasDrawColor() >> 24), Color_);
                    FScreenQuadRecord Record;
                    Record.X0 = ShadowX0;
                    Record.Y0 = ShadowY0;
                    Record.X1 = ShadowX1;
                    Record.Y1 = ShadowY1;
                    Record.DepthA = 1.0f;
                    Record.DepthB = 1.0f;
                    Record.U0 = ShadowU0;
                    Record.V0 = ShadowV0;
                    Record.U1 = ShadowU1;
                    Record.V1 = ShadowV1;
                    Record.ColorBits = Color;
                    ScreenQuadAppendRecord(Record);
                }
            }

            // the page quad; colour = the tint the field..77 as
            // A (0x77), then bytes 0x74, 0x75, 0x76.
            unsigned const Tint = FontAtlasDrawColor();
            unsigned const Color = NumberPackColor(static_cast<unsigned char>(Tint >> 24), Tint);
            FScreenQuadRecord Record;
            Record.X0 = ClipX0;
            Record.Y0 = ClipY0;
            Record.X1 = X1;
            Record.Y1 = Y1;
            Record.DepthA = 1.0f;
            Record.DepthB = 1.0f;
            Record.U0 = U0;
            Record.V0 = V0;
            Record.U1 = U1;
            Record.V1 = V1;
            Record.ColorBits = Color;
            ScreenQuadAppendRecord(Record);
        }
    }
}

// ---------------------------------------------------------------------------
// Font::String host
// ---------------------------------------------------------------------------

void FontStringConstruct(FFontString* Self)
{

    Self->Text = nullptr;
    Self->Capacity = 0;
    Self->Field08 = 0x7fff;
    Self->Field0C = static_cast<unsigned char>(reinterpret_cast<uintptr_t>(Self) >> 24);
    Self->Pad0D[0] = 0;
    Self->Pad0D[1] = 0;
    Self->Pad0D[2] = 0;
    Self->ChildBegin = nullptr;
    Self->ChildEnd = nullptr;
    Self->ChildCap = nullptr;
    Self->FontIface = nullptr;
    Self->ExtentW = 0;
    Self->ExtentH = 0;
    Self->LayoutGen = 0;
    Self->ScaleKey = 0x7fff;
    Self->bMultiLine = 0;
    Self->Pad31[0] = 0;
    Self->Pad31[1] = 0;
    Self->Pad31[2] = 0;
}

void FontStringClearContents(FFontString* Self)
{
    // — free text; Release children; ChildEnd=Begin; LayoutGen=0.
    // Does NOT free the child vector buffer or clear ExtentW/H.
    if (Self->Text != nullptr)
    {
        FreeWide(Self->Text);
        Self->Text = nullptr;
        Self->Capacity = 0;
    }
    if (Self->ChildBegin != nullptr)
    {
        FFontStringChildSlot* It = Self->ChildBegin;
        FFontStringChildSlot* End = Self->ChildEnd;
        while (It != End)
        {
            if (It->Child != nullptr)
            {
                ReleaseChildSlot(*It);
            }
            ++It;
        }
        Self->ChildEnd = Self->ChildBegin;
    }
    Self->LayoutGen = 0;
}

// (vector<child slot>::_Destroy; thiscall on the vector, RET 8, 0x00445dc0..
// ): for each 8-byte slot in [First, Last), a non-null child gets vt+4 -- Release on
// both kinds. The slots themselves are not cleared.
void FontStringChildSlotsDestroy(FFontStringChildSlot* First, FFontStringChildSlot* Last)
{
    FFontStringChildSlot* It = First;
    for (; It != Last; ++It)
    {
        if (It->Child == nullptr)
            continue;
        It->Child->Release();
    }
}

// (the child vector's destructor; ECX = Font::String + 0xc, 0x00445d70..
// ): the slots in [first, last) released (the routine inlined), operator delete
// (the routine) on first, then first / last / end zeroed.
void FontStringChildVectorDestroy(FFontString* Self)
{
    FontStringChildSlotsDestroy(Self->ChildBegin, Self->ChildEnd);
    operator delete[](Self->ChildBegin);
    Self->ChildBegin = nullptr;
    Self->ChildEnd = nullptr;
    Self->ChildCap = nullptr;
}

// (Font::String::~String, 0x00444e50..0x00444ede): the destructor body is
// ; then the members in reverse -- the child vector at +0x0c (the routine
// inlined) and the wide-text buffer at +0x00 (the routine inlined: free when non-null, then
// +0x00 / +0x04 zeroed).
void FontStringDestroy(FFontString* Self)
{
    FontStringClearContents(Self);
    FontStringChildVectorDestroy(Self);
    if (Self->Text != nullptr)
    {
        FreeWide(Self->Text);
        Self->Text = nullptr;
        Self->Capacity = 0;
    }
}

void FontStringSetFont(FFontString* Self, IFont* Font)
{
    // (thiscall, RET 4): nothing when +0x1c already holds Font (0x004ea677 JZ);
    // otherwise store it, and for every index below the size re-read each pass (buffer test
    // , unsigned JNC 0x004ea693) the child's vt+8 with +0x1c re-read (0x004ea69a) --
    // no null test, no look at the kind byte -- then +0x28 = 0.
    if (Self->FontIface == Font)
        return;
    Self->FontIface = Font;
    for (unsigned Index = 0;
         Self->ChildBegin != nullptr &&
         Index < static_cast<unsigned>(Self->ChildEnd - Self->ChildBegin);
         ++Index)
    {
        Self->ChildBegin[Index].Child->BindFont(Self->FontIface);
    }
    Self->LayoutGen = 0;
}

void FontStringSetText(FFontString* Self, wchar_t const* Text)
{

    if (Self->Text == Text)
    {
        return;
    }
    wchar_t const* Left = Text != nullptr ? Text : L"";
    wchar_t const* Right = Self->Text != nullptr ? Self->Text : L"";
    if (wcscmp(Left, Right) == 0)
    {
        return;
    }

    // a non-empty old text is freed, then the child vector is erased
    // whole (the inlined erase(begin, end): nothing to shift, vt+4 on every non-null child from
    // begin to the old end, end = begin) and +0x28 = 0. No null test on begin -- an empty
    // vector's begin == end makes the loop run zero times.
    if (Self->Text != nullptr && Self->Text[0] != L'\0')
    {
        FreeWide(Self->Text);
        Self->Text = nullptr;
        Self->Capacity = 0;
        FFontStringChildSlot* It = Self->ChildBegin;
        FFontStringChildSlot* End = Self->ChildEnd;
        while (It != End)
        {
            if (It->Child != nullptr)
            {
                ReleaseChildSlot(*It);
            }
            ++It;
        }
        Self->ChildEnd = Self->ChildBegin;
        Self->LayoutGen = 0;
    }

    if (Text != nullptr)
    {
        size_t const Len = wcslen(Text);
        unsigned const Need = static_cast<unsigned>(Len + 1u);
        if (Need > Self->Capacity)
        {
            // realloc an existing buffer, otherwise malloc and write the
            // terminator -- neither result is null-tested.
            if (Self->Text == nullptr)
            {
                Self->Text = AllocWideBytes(Need * sizeof(wchar_t));
                Self->Text[0] = L'\0';
            }
            else
            {
                Self->Text = ReallocWide(Self->Text, Need * sizeof(wchar_t));
            }
            Self->Capacity = Need;
        }

        wcsncpy(Self->Text, Text, Len);
        Self->Text[Len] = L'\0';
        Self->LayoutGen = 0;
        return;
    }

    if (Self->Text != nullptr && Self->Text[0] != L'\0')
    {
        Self->Text[0] = L'\0';
    }
    Self->LayoutGen = 0;
}

void FontStringSetScale(FFontString* Self, float Param1, float Param2)
{

    //   param2 = abs(param2); if (param2 < 0.001) param2 = 1.0;
    //   key = floor( (the field/the field) * (the field/param2) * param1 + 0.5 )
    // and the field are not font globals: they are the XScale (+0x78) and ZNear
    // (+0xa4) of the Win3D scene camera the field, read straight out of it.
    // FCOM double 0.001 / TEST AH,5 / JP -- replaced only on an ordered "less"
    // (no float lies in [0.001, 0.001f), so the float compare is the same test).
    FSpaceCamera const* const Cam = GetWin3DSceneCamera();
    float Depth = std::fabs(Param2);
    if (Depth < 0.001f)
    {
        Depth = 1.0f;
    }
    double const Term =
        (static_cast<double>(Cam->XScale) / static_cast<double>(Cam->ZNear)) *
        (static_cast<double>(Cam->ZNear) / static_cast<double>(Depth)) *
        static_cast<double>(Param1) +
        0.5;
    int const Key = static_cast<int>(std::floor(Term));
    if (Key != Self->ScaleKey)
    {
        Self->ScaleKey = Key;
        Self->LayoutGen = 0;
    }
}

// (thiscall, RET 8: Index, Kind) -- the child at Index of the requested kind,
// created when missing. Returns the element; its callers go through its virtual slots.
FontStringElement* FontStringEnsureChild(FFontString* Self, unsigned Index, unsigned char Kind)
{
    FontStringElement* Result = nullptr;
    bool bReuse = false;

    // a slot at Index (buffer present, Index < size, unsigned JNC).
    if (Self->ChildBegin != nullptr && Index < static_cast<unsigned>(Self->ChildEnd - Self->ChildBegin))
    {
        FFontStringChildSlot& Slot = Self->ChildBegin[Index];
        if (Slot.Kind == Kind)
        {
            // the same kind with a child -> reuse it.
            Result = Slot.Child;
            bReuse = Result != nullptr;
        }
        else
        {
            // another kind -> its vt+4 (Release, no null test) and the
            // pointer cleared.
            Slot.Child->Release();
            Slot.Child = nullptr;
        }
    }

    if (!bReuse)
    {
        // Kind != 0 -> operator new(0x34) + the routine (NumberString);
        // Kind 0 -> operator new(0x2c) + the routine (FontString). 0x004146f0..0x00414702: the
        // new child's vt+8 with +0x1c.
        if (Kind != 0)
            Result = new NumberStringChild();
        else
            Result = new FontStringChild();
        Result->BindFont(Self->FontIface);
    }

    // a slot at Index (size re-read) takes the pointer and nothing else
    // happens -- the kind byte of a replaced child of the other kind stays as it was.
    if (Self->ChildBegin != nullptr && Index < static_cast<unsigned>(Self->ChildEnd - Self->ChildBegin))
    {
        Self->ChildBegin[Index].Child = Result;
        return Result;
    }

    // otherwise one slot is appended at the END whatever Index is: a
    // temporary {child, kind} (AddRef), the routine(last, 1, &temp) (copy AddRef), the
    // temporary's destructor the routine (Release), then the child's own vt+4 (Release, no
    // null test) -- the vector keeps the single reference the constructor gave.
    {
        FFontStringChildTemp Temp(Result, Kind);
        ChildVectorInsertAtEnd(Self, 1u, Temp.Slot);
    }
    Result->Release();
    return Result;
}

NumberStringChild* FontStringEnsureNumberChild(FFontString* Self, unsigned Index)
{
    // with kind 1 (the caller at 0x00415a14), whose result gets vt+0x30 -- a slot
    // only a NumberString has. The element is a NumberString whenever the routine reaches this:
    // a stale kind byte 1 over a FontString needs the FontString children of a non-empty text
    // (the routine builds none for an empty one) to outlive that text, and every path that
    // empties it (the routine, the routine, the routine) releases the children first.
    return static_cast<NumberStringChild*>(FontStringEnsureChild(Self, Index, 1));
}

void FontStringRebuild(FFontString* Self)
{
    // (thiscall). 0x004155df..0x00415607: the layout generation stamped; nothing
    // more without text, with an empty one or without the font (+0x1c) -- the extents are left.
    Self->LayoutGen = G_FontLayoutGeneration;

    wchar_t const* Text = Self->Text;
    if (Text == nullptr || Text[0] == L'\0' || Self->FontIface == nullptr)
    {
        return;
    }

    int LineCount = 0; // [ESP+0x10]

    if (Self->bMultiLine != 0)
    {
        // 0x400 {line, capacity} elements zeroed (the routine through
        // the EH vector constructor), max lines = Field08 capped at 0x400 (unsigned, JC), the
        // width is the unsigned ScaleKey (+0x2c) through a qword FILD, FSTP float.
        unsigned MaxLines = static_cast<unsigned>(Self->Field08);
        if (MaxLines >= 0x400u)
        {
            MaxLines = 0x400u;
        }
        wchar_t* LinePtrs[0x400];
        unsigned LineCaps[0x400];
        for (int I = 0; I < 0x400; ++I)
        {
            LinePtrs[I] = nullptr;
            LineCaps[I] = 0u;
        }
        float const WrapWidth =
            static_cast<float>(static_cast<double>(static_cast<unsigned>(Self->ScaleKey)));
        FontStringWordWrap(Self->FontIface, Text, WrapWidth, LinePtrs, LineCaps, &LineCount,
                           static_cast<int>(MaxLines));

        // per line its child (the routine(i, 0)), the line's trailing
        // spaces cut (never below one character), then the child's vt+0x14 SetText.
        for (int I = 0; I < LineCount; ++I)
        {
            FontStringElement* const Child =
                FontStringEnsureChild(Self, static_cast<unsigned>(I), 0);
            wchar_t* const Line = LinePtrs[I];
            size_t Len = wcslen(Line);
            while (Len > 1u && Line[Len - 1u] == L' ')
            {
                --Len;
                Line[Len] = L'\0';
            }
            Child->SetText(Line);
        }

        // the element destructor (the routine) on all 0x400.
        for (int I = 0; I < 0x400; ++I)
        {
            if (LinePtrs[I] != nullptr)
            {
                FreeWide(LinePtrs[I]);
                LinePtrs[I] = nullptr;
                LineCaps[I] = 0u;
            }
        }
    }
    else
    {
        // IFont slot 9 (+0x24)(ScaleKey, text, -1); positive -> one line,
        // child 0 (the routine(0, 0)) given the whole text.
        if (Self->FontIface->ProbeString(Self->ScaleKey, Text, -1) > 0)
        {
            LineCount = 1;
            FontStringEnsureChild(Self, 0, 0)->SetText(Self->Text);
        }
    }

    // the child vector resized to the line count -- short: default
    // {0, 0} slots appended (the routine); long: the surplus slots destroyed (the routine, the
    // child's vt+4) and the end pulled back.
    // The {0, 0} value is a stack temporary (0x00415719..0x0041571d, EH state 1 at 0x0041572a,
    // unwind Unwind@005b8899 -> the routine); its destructor runs inline at 0x004157cc..
    // .
    {
        FFontStringChildTemp EmptySlot(nullptr, 0);
        unsigned const Wanted = static_cast<unsigned>(LineCount);
        unsigned const Used = (Self->ChildBegin == nullptr)
                                  ? 0u
                                  : static_cast<unsigned>(Self->ChildEnd - Self->ChildBegin);
        if (Used < Wanted)
        {
            // the routine(last, Wanted - size, &temp).
            ChildVectorInsertAtEnd(Self, Wanted - Used, EmptySlot.Slot);
        }
        else if (Self->ChildBegin != nullptr && Wanted < Used)
        {
            // the erase copy loop runs over [last, last) (CMP ECX,ECX);
            // (0) on each surplus slot; last = first + Wanted.
            for (unsigned I = Wanted; I < Used; ++I)
            {
                ReleaseChildSlot(Self->ChildBegin[I]);
            }
            Self->ChildEnd = Self->ChildBegin + Wanted;
        }
    }

    // the extents -- over every slot (the size re-read each pass, no
    // test of the child) its vt+0x18 size: width kept when greater (unsigned, JBE), heights
    // summed.
    Self->ExtentW = 0;
    Self->ExtentH = 0;
    for (unsigned I = 0; Self->ChildBegin != nullptr &&
                         I < static_cast<unsigned>(Self->ChildEnd - Self->ChildBegin);
         ++I)
    {
        int Size[2] = {0, 0};
        Self->ChildBegin[I].Child->GetSize(Size);
        if (static_cast<unsigned>(Self->ExtentW) < static_cast<unsigned>(Size[0]))
        {
            Self->ExtentW = Size[0];
        }
        Self->ExtentH += Size[1];
    }
}

int* FontStringGetExtent(FFontString* Self, int* OutWH)
{
    // (thiscall, RET 4): a stale +0x28 rebuilds (the routine); then {+0x20, +0x24}
    // into OutWH (no null test) and EAX = OutWH.
    if (Self->LayoutGen != G_FontLayoutGeneration)
    {
        FontStringRebuild(Self);
    }
    OutWH[0] = Self->ExtentW;
    OutWH[1] = Self->ExtentH;
    return OutWH;
}

int FontStringChildCount(FFontString* Self)
{
    // (thiscall): the layout rebuilt when stale (+0x28 against the field,
    // ), then 0 without a child array, else (ChildEnd - ChildBegin) >> 3.
    if (Self->LayoutGen != G_FontLayoutGeneration)
    {
        FontStringRebuild(Self);
    }
    if (Self->ChildBegin == nullptr)
    {
        return 0;
    }
    return static_cast<int>(Self->ChildEnd - Self->ChildBegin);
}

namespace
{

// (thiscall ECX = the draw origin, RET 0xc): the child's size, the box alignment,
// then the child's vt+0x24 with a local origin.
void DrawChildAligned(FFontStringDrawOrigin const* Origin, FontStringElement* Child,
                      void* DrawCtx, void* DrawArg)
{
    // vt+0x18 into the local {w, h}.
    int Size[2] = {0, 0};
    Child->GetSize(Size);

    FFontStringDrawOrigin Local;
    // [+0x10] 1 -> (width - (unsigned)w) * 0.5 (the field) + x;
    // 2 -> width + x - (int)w (FISUB); anything else copies x.
    unsigned const HMode = Origin->Field10;
    if (HMode == 1u)
    {
        double const Span =
            static_cast<double>(Origin->Scale) - static_cast<double>(static_cast<unsigned>(Size[0]));
        Local.X = static_cast<float>(Span * 0.5 + static_cast<double>(Origin->X));
    }
    else if (HMode == 2u)
    {
        Local.X = static_cast<float>(static_cast<double>(Origin->Scale) +
                                     static_cast<double>(Origin->X) -
                                     static_cast<double>(Size[0]));
    }
    else
    {
        Local.X = Origin->X;
    }
    // the same on y with [+0x14] and the height at +0xc.
    unsigned const VMode = Origin->Field14;
    if (VMode == 1u)
    {
        double const Span =
            static_cast<double>(Origin->Height) - static_cast<double>(static_cast<unsigned>(Size[1]));
        Local.Y = static_cast<float>(Span * 0.5 + static_cast<double>(Origin->Y));
    }
    else if (VMode == 2u)
    {
        Local.Y = static_cast<float>(static_cast<double>(Origin->Height) +
                                     static_cast<double>(Origin->Y) -
                                     static_cast<double>(Size[1]));
    }
    else
    {
        Local.Y = Origin->Y;
    }
    // The local origin is {x, y} followed on the stack by the {w, h} pair; the child paints
    // read only x and y.
    std::memcpy(&Local.Scale, &Size[0], sizeof(float));
    std::memcpy(&Local.Height, &Size[1], sizeof(float));
    Local.Field10 = 0;
    Local.Field14 = 0;

    // vt+0x24(&local, DrawCtx, DrawArg).
    Child->Paint(&Local, DrawCtx, DrawArg);
}

} // namespace

void FontStringDrawChildren(FFontStringDrawOrigin* Origin, FFontString* Self, void* DrawCtx,
                            void* DrawArg)
{
    // — thiscall ECX = Origin, args (Font::String*, DrawCtx, DrawArg), RET 0xc.
    // a null string draws nothing.
    if (Self == nullptr)
    {
        return;
    }
    // the origin's x / y are kept (as dwords) to restore every pass.
    float const SavedX = Origin->X;
    float const SavedY = Origin->Y;

    // ftol(width) (truncation) is the wrap key; a change drops the layout.
    int const ScaleKey = static_cast<int>(Origin->Scale);
    if (ScaleKey != Self->ScaleKey)
    {
        Self->ScaleKey = ScaleKey;
        Self->LayoutGen = 0;
    }
    if (Self->LayoutGen != G_FontLayoutGeneration)
    {
        FontStringRebuild(Self);
    }

    // the count is taken once; each pass re-checks the vector bounds and a
    // null child (00414430..0041444a).
    int Count = 0;
    if (Self->ChildBegin != nullptr)
    {
        Count = static_cast<int>(Self->ChildEnd - Self->ChildBegin);
    }
    for (int I = 0; I < Count; ++I)
    {
        if (Self->ChildBegin == nullptr ||
            static_cast<unsigned>(I) >= static_cast<unsigned>(Self->ChildEnd - Self->ChildBegin))
        {
            continue;
        }
        FontStringElement* const Child = Self->ChildBegin[I].Child;
        if (Child == nullptr)
        {
            continue;
        }
        // vt+0x18; line I sits at y + h * I (IMUL, FILD of the zero-extended
        // product, FADD the saved y); x is restored.
        int Size[2] = {0, 0};
        Child->GetSize(Size);
        unsigned const LineOffset = static_cast<unsigned>(Size[1] * I);
        Origin->X = SavedX;
        Origin->Y = static_cast<float>(static_cast<double>(LineOffset) + static_cast<double>(SavedY));
        // the routine(child, DrawCtx, DrawArg) on this origin.
        DrawChildAligned(Origin, Child, DrawCtx, DrawArg);
    }
}

void FontStringSetNumberText(FFontString* Self, char const* Format)
{
    // (thiscall, RET 8; the first argument is never read).
    // only a non-empty wide text is dropped -- free, text / capacity 0, the
    // children released in place (vt+4 on each non-null one) and the vector emptied, +0x28 = 0.
    if (Self->Text != nullptr && Self->Text[0] != L'\0')
    {
        FreeWide(Self->Text);
        Self->Text = nullptr;
        Self->Capacity = 0;
        for (FFontStringChildSlot* It = Self->ChildBegin; It != Self->ChildEnd; ++It)
        {
            if (It->Child != nullptr)
            {
                ReleaseChildSlot(*It);
            }
        }
        Self->ChildEnd = Self->ChildBegin;
        Self->LayoutGen = 0;
    }
    // the routine(0, 1), then its vt+0x30 (cdecl) with Format.
    NumberStringChild* const Child = FontStringEnsureNumberChild(Self, 0);
    Child->Printf(Format);
    // the extents from every child's vt+0x18 -- the widest width (unsigned
    // JBE) and the summed height. The children are not null-checked.
    Self->ExtentW = 0;
    Self->ExtentH = 0;
    for (unsigned Index = 0;
         Self->ChildBegin != nullptr &&
         Index < static_cast<unsigned>(Self->ChildEnd - Self->ChildBegin);
         ++Index)
    {
        int Size[2] = {0, 0};
        Self->ChildBegin[Index].Child->GetSize(Size);
        if (static_cast<unsigned>(Size[0]) > static_cast<unsigned>(Self->ExtentW))
        {
            Self->ExtentW = Size[0];
        }
        Self->ExtentH += Size[1];
    }
}

// ---------------------------------------------------------------------------
// NumberString — `.?AUFontNumberString@@` size 0x34 (the branch)
// ---------------------------------------------------------------------------

namespace
{

// / the field -- {text, capacity} built by XC thunk the branch from
// u"-.0123456789MK%/" (005c8c90): capacity = wcslen + 1 = 17, freed by its atexit thunk
// . Nothing else writes it, so between the two it is this constant pair.
wchar_t const G_NumberGlyphText[] = L"-.0123456789MK%/";
unsigned const G_NumberGlyphTextCapacity = 17u;

// [0x100] / the field -- the byte -> glyph index map Printf builds on first use:
// zeroed, then map["-.0123456789MK%/"[i]] = i for i < 0x11 (the field, the NUL included, so
// map[0] = 16). Any other byte maps to 0, the '-' glyph.
unsigned char G_NumberGlyphMap[0x100];
unsigned char G_bNumberGlyphMapBuilt = 0;
char const G_NumberGlyphChars[] = "-.0123456789MK%/";

// (thiscall on a {wchar_t*, capacity} pair, RET 8): a null source clears a
// non-empty string; otherwise Len (-1: wcslen) characters are copied after growing the buffer
// to Len + 1 (realloc, or malloc with the first character zeroed), and [Len] = 0.
void AssignWideCount(wchar_t*& Text, unsigned& Capacity, wchar_t const* Source, unsigned Len)
{
    if (Source == nullptr)
    {
        if (Text != nullptr && Text[0] != L'\0')
        {
            Text[0] = L'\0';
        }
        return;
    }
    if (Len == 0xffffffffu)
    {
        Len = static_cast<unsigned>(wcslen(Source));
    }
    unsigned const Need = Len + 1u;
    if (Need > Capacity)
    {
        if (Text != nullptr)
        {
            Text = ReallocWide(Text, Need * 2u);
        }
        else
        {
            Text = AllocWideBytes(Need * 2u);
            Text[0] = L'\0';
        }
        Capacity = Need;
    }
    wcsncpy(Text, Source, Len);
    Text[Len] = L'\0';
}

// (thiscall on a FontString, RET 4): with a stale generation (+0x20) the page
// grid at +0x14 goes back to the the field pool (the walk inlined at 004152fe..00415496);
// then an empty grid is rasterised (the routine with the argument). Returns the grid. The
// generation itself is not stamped here.
FFontAtlasPage* FontStringChildEnsurePages(FontStringChild* Glyph, unsigned char bSkipSizeCheck)
{
    if (Glyph->LayoutGen_ != G_FontLayoutGeneration && Glyph->GlyphHead_ != nullptr)
    {
        FontAtlasFreePages(Glyph->GlyphHead_);
    }
    if (Glyph->GlyphHead_ == nullptr)
    {
        FontAtlasRasterize(Glyph, bSkipSizeCheck);
    }
    return Glyph->GlyphHead_;
}

// The glyph FontString of a face, as the three sites build it (0041380d..0041386f,
// , 00414094..00414139): the routine, vt+8 with the face key, the routine
// with the field / the field (so 17 characters, capacity 18); then the stale-page drop
// and the routine(0) (the routine's body, inlined at the first two sites), and a grid that
// is still empty releases the FontString again.
void NumberFaceCreateGlyph(FNumberFace* Face)
{
    FontStringChild* const Glyph = new FontStringChild();
    Face->Glyph = Glyph;
    Glyph->BindFont(Face->FontKey);
    AssignWideCount(Glyph->Text_, Glyph->Capacity_, G_NumberGlyphText, G_NumberGlyphTextCapacity);
    if (FontStringChildEnsurePages(Glyph, 0) == nullptr)
    {
        Glyph->Release();
        Face->Glyph = nullptr;
    }
}

// The advance / texture table of a face from its glyph FontString and its first page, as
// (004141c0..00414263) and the branch (004151fa..00415293) both compute it:
// IFont slot 10 over the glyph text with the text's capacity (18) as the count, straight into
// face+8; then for the 17 glyphs a running float sum of the advances, U1 = sum / page width,
// U0 = the previous U1 (0 first), V0 = 0, V1 = used height / page height.
// baseline lets the widths past the 17th run on into Uv[0..], which the loop then overwrites
// field by field; the port collects them in a buffer of the capacity and keeps the first 17,
// which leaves the face bytes identical.
void NumberFaceBuildTable(FNumberFace* Face, FontStringChild* Glyph, FFontAtlasPage* Page)
{
    IFont* const Font = Glyph->GetFont();         // vt+0xc
    wchar_t const* const Text = Glyph->GetText(); // vt+0x10
    std::vector<int> Widths(Glyph->Capacity_ > 17u ? Glyph->Capacity_ : 17u, 0);
    Font->GetCharWidths(Text, static_cast<int>(Glyph->Capacity_), Widths.data());
    for (unsigned Index = 0; Index < 17u && Index < Glyph->Capacity_; ++Index)
    {
        Face->Advance[Index] = Widths[Index];
    }

    // FILD qword (zero-extended +0x1c) / FIDIV dword +8: stays on the FPU stack for the loop.
    double const VRatio = static_cast<double>(Page->UsedH) /
                          static_cast<double>(static_cast<int>(Page->PageH));
    float Previous = 0.0f;
    float Sum = 0.0f;
    for (unsigned Index = 0; Index < 17u; ++Index)
    {
        Sum = static_cast<float>(static_cast<double>(Face->Advance[Index]) +
                                 static_cast<double>(Sum));
        Face->Uv[Index].V0 = 0.0f;
        float const U1 =
            static_cast<float>(static_cast<double>(Sum) / static_cast<double>(Page->PageW));
        Face->Uv[Index].U0 = Previous;
        Face->Uv[Index].U1 = U1;
        Face->Uv[Index].V1 = static_cast<float>(VRatio);
        Previous = U1;
    }
}

// (thiscall on the face) -- rebuild the advance / texture table when the face's
// generation is stale.
void NumberFaceRebuild(FNumberFace* Face)
{
    if (Face->LayoutGen == G_FontLayoutGeneration)
    {
        return;
    }
    if (Face->Glyph == nullptr)
    {
        // the text copy is the routine + wcsncpy inline, the page check
        // the the routine body inline -- the same steps as NumberFaceCreateGlyph.
        NumberFaceCreateGlyph(Face);
    }
    FontStringChild* const Glyph = Face->Glyph;
    if (Glyph == nullptr)
    {
        return;
    }
    // the generation is stamped before the pages are looked at.
    Face->LayoutGen = G_FontLayoutGeneration;
    // stale pages back to the pool (the routine per node), an empty grid
    // rasterised with 0.
    FFontAtlasPage* const Page = FontStringChildEnsurePages(Glyph, 0);
    if (Page == nullptr)
    {
        return;
    }
    NumberFaceBuildTable(Face, Glyph, Page);
}

// (Last, 1, &Glyph) -- VC6 vector<char>::insert at the end: with no spare
// capacity a new buffer of size + max(1, size) (1 for an empty vector), the old bytes copied,
// the old buffer freed; else in place.
void NumberGlyphAppend(NumberStringChild* Self, unsigned char Glyph)
{
    unsigned const Spare = static_cast<unsigned>(Self->GlyphEnd_ - Self->GlyphLast_);
    if (Spare < 1u)
    {
        unsigned const Size = Self->GlyphFirst_ == nullptr
                                  ? 0u
                                  : static_cast<unsigned>(Self->GlyphLast_ - Self->GlyphFirst_);
        unsigned Grow = 1u;
        if (Self->GlyphFirst_ != nullptr && Size > 1u)
        {
            Grow = Size;
        }
        unsigned const NewCapacity = Size + Grow;
        unsigned char* const NewFirst = static_cast<unsigned char*>(::operator new(NewCapacity));
        unsigned char* Out = NewFirst;
        for (unsigned char* In = Self->GlyphFirst_; In != Self->GlyphLast_; ++In)
        {
            *Out = *In;
            ++Out;
        }
        *Out = Glyph;
        ::operator delete(Self->GlyphFirst_);
        Self->GlyphFirst_ = NewFirst;
        Self->GlyphLast_ = NewFirst + Size + 1u;
        Self->GlyphEnd_ = NewFirst + NewCapacity;
        return;
    }
    *Self->GlyphLast_ = Glyph;
    ++Self->GlyphLast_;
}

// ARGB assembled the way the branch does it (00413b8f..00413bb5, 00413ccf..00413d01):
// alpha from the fourth byte of the global tint the field, then bytes 0, 1, 2 of Source
// from high to low -- the red and blue bytes of a D3DCOLOR trade places.
unsigned NumberPackColor(unsigned char Alpha, unsigned Source)
{
    unsigned const B0 = Source & 0xffu;
    unsigned const B1 = (Source >> 8) & 0xffu;
    unsigned const B2 = (Source >> 16) & 0xffu;
    return (static_cast<unsigned>(Alpha) << 24) | (B0 << 16) | (B1 << 8) | B2;
}

} // namespace

void WideTextAssign(FHudWideText* Self, wchar_t const* Source, unsigned Len)
{
    // on a free-standing {Text, Capacity} head (PageCharacterSelectSingle's
    // ShowDetails 0x0056c016).
    AssignWideCount(Self->Text, Self->Capacity, Source, Len);
}

NumberStringChild::NumberStringChild()
    : Alloc04_(0)
    , GlyphFirst_(nullptr)
    , GlyphLast_(nullptr)
    , GlyphEnd_(nullptr)
    , Font_(nullptr)
    , RefCount_(1)
    , Color_(0xff080808u)
    , bShadow_(0)
    , LayoutGen_(0)
    , Width_(0)
    , Height_(0)
    , FaceCache_(nullptr)
{
    // +4 = a stack byte (the allocator), +8..+0x14 = 0, +0x18 = 1, +0x1c bytes 0
    // then 0xff080808, +0x28 / +0x2c / +0x30 / +0x24 = 0, the vtable, then the routine(1)
    // through its vtable cell (00414d8b).
    Pad05_[0] = 0;
    Pad05_[1] = 0;
    Pad05_[2] = 0;
    Pad21_[0] = 0;
    Pad21_[1] = 0;
    Pad21_[2] = 0;
    SetShadow(1);
}

NumberStringChild::~NumberStringChild()
{
    // release the face first (00415517..00415588), then operator delete the
    // glyph vector at +8 and zero +8/+0xc/+0x10 (0041558d..004155a3).
    if (FaceCache_ != nullptr)
    {
        NumberFaceRelease(static_cast<FNumberFace*>(FaceCache_));
        FaceCache_ = nullptr;
    }
    ::operator delete(GlyphFirst_);
    GlyphFirst_ = nullptr;
    GlyphLast_ = nullptr;
    GlyphEnd_ = nullptr;
}

int NumberStringChild::AddRef()
{

    ++RefCount_;
    return RefCount_;
}

int NumberStringChild::Release()
{
    // the decremented count is returned either way; below 1 the routine and
    // operator delete.
    int const Remaining = --RefCount_;
    if (Remaining < 1)
    {
        delete this;
    }
    return Remaining;
}

void NumberStringChild::BindFont(IFont* Font)
{
    // (RET 4) — nothing when +0x14 already holds it (00414fb8 JZ); otherwise store
    // it and release the face at +0x30 (the routine cache) when there is one.
    if (Font_ == Font)
    {
        return;
    }
    Font_ = Font;
    if (FaceCache_ != nullptr)
    {
        NumberFaceRelease(static_cast<FNumberFace*>(FaceCache_));
        FaceCache_ = nullptr;
    }
    // baseline zeroes +0x24 on every change, face or not. (+0x20 is a byte flag set
    // through the routine, not a generation.)
    LayoutGen_ = 0;
}

IFont* NumberStringChild::GetFont() const
{
    // MOV EAX,[ECX+0x14].
    return Font_;
}

wchar_t const* NumberStringChild::GetText() const
{
    // MOV EAX,the field -- the shared empty wide string.
    return L"";
}

void NumberStringChild::SetText(wchar_t const* Text)
{
    // RET 4 (an ICF-folded empty body shared with 20 other slots).
    (void)Text;
}

int* NumberStringChild::GetSize(int* OutWH)
{
    // (RET 4).
    // cached while +0x24 matches the generation; else stamp it and zero the
    // two extents.
    if (LayoutGen_ != G_FontLayoutGeneration)
    {
        LayoutGen_ = G_FontLayoutGeneration;
        Width_ = 0;
        Height_ = 0;
        // the face (the routine with the key).
        if (FaceCache_ == nullptr)
        {
            FaceCache_ = NumberFaceGetOrCreate(Font_);
        }
        FNumberFace* const Face = static_cast<FNumberFace*>(FaceCache_);
        if (Face != nullptr)
        {
            // its glyph FontString on first use.
            if (Face->Glyph == nullptr)
            {
                NumberFaceCreateGlyph(Face);
            }
            FontStringChild* const Glyph = Face->Glyph;
            if (Glyph != nullptr)
            {
                // the height is the glyph FontString's (its vt+0x18).
                int GlyphSize[2] = {0, 0};
                Glyph->GetSize(GlyphSize);
                Height_ = GlyphSize[1];
                // the advance of every glyph byte; a face gone stale meanwhile
                // gets its table rebuilt first (the routine's steps inlined, with
                // for the pages and the glyph rebuilt when it went away).
                for (unsigned char* It = GlyphFirst_; It != GlyphLast_; ++It)
                {
                    unsigned const Index = *It;
                    if (Face->LayoutGen != G_FontLayoutGeneration)
                    {
                        if (Face->Glyph == nullptr)
                        {
                            NumberFaceCreateGlyph(Face);
                        }
                        FontStringChild* const Current = Face->Glyph;
                        if (Current != nullptr)
                        {
                            Face->LayoutGen = G_FontLayoutGeneration;
                            FFontAtlasPage* const Page = FontStringChildEnsurePages(Current, 0);
                            if (Page != nullptr)
                            {
                                NumberFaceBuildTable(Face, Current, Page);
                            }
                        }
                    }
                    // Width_ += face->Advance[glyph].
                    Width_ += Face->Advance[Index];
                }
            }
        }
    }
    // {+0x28, +0x2c} out; the pointer is returned.
    OutWH[0] = Width_;
    OutWH[1] = Height_;
    return OutWH;
}

void NumberStringChild::SetColor(unsigned const* Argb)
{
    // [+0x1c] = *Argb, the argument slot overwritten with 1 and a tail jump
    // through vt+0x20.
    Color_ = *Argb;
    SetShadow(1);
}

void NumberStringChild::SetShadow(unsigned char Value)
{
    // .
    bShadow_ = Value;
}

void NumberStringChild::FormatFloat(float Value, int Decimals, char Suffix)
{
    // (RET 0xc): the format "%.<Decimals>f<Suffix>" into a 0xc-byte local (count
    // 10), then slot 12 with the value widened to double (FSTP double).
    char Format[0xc];
    _snprintf(Format, 10, "%%.%df%c", Decimals, static_cast<int>(Suffix));
    Printf(Format, static_cast<double>(Value));
}

void NumberStringChild::FormatInt(int Value, char Suffix)
{
    // (RET 8): slot 12 with "%d%c" (the field), the suffix sign-extended.
    Printf("%d%c", Value, static_cast<int>(Suffix));
}

void NumberStringChild::Printf(char const* Format, ...)
{
    // (cdecl through vt+0x30).
    // _vsnprintf into a 0x20-byte local.
    // The port keeps one more byte and a NUL there: when the output does not fit, baseline gets
    // -1 and an unterminated buffer, reserves new(0) with End = First - 1, and reads the stack
    // past the buffer -- heap and stack corruption that no baseline caller reaches (their formats
    // are a number and a suffix). That path is the port's only divergence here.
    char Buffer[0x21];
    va_list Args;
    va_start(Args, Format);
    int const Count = MsvcrtNumericVsnprintf(Buffer, 0x20, Format, Args);
    va_end(Args);
    Buffer[0x20] = '\0';

    // reserve(Count) -- a capacity below it gets a new block of Count bytes
    // (the routine copies the old bytes one by one), the old block freed.
    unsigned const Capacity =
        GlyphFirst_ == nullptr ? 0u : static_cast<unsigned>(GlyphEnd_ - GlyphFirst_);
    if (Count >= 0 && Capacity < static_cast<unsigned>(Count))
    {
        unsigned char* const NewFirst =
            static_cast<unsigned char*>(::operator new(static_cast<size_t>(Count)));
        unsigned char* Out = NewFirst;
        for (unsigned char* In = GlyphFirst_; In != GlyphLast_; ++In)
        {
            *Out = *In;
            ++Out;
        }
        unsigned const Size =
            GlyphFirst_ == nullptr ? 0u : static_cast<unsigned>(GlyphLast_ - GlyphFirst_);
        ::operator delete(GlyphFirst_);
        GlyphEnd_ = NewFirst + Count;
        GlyphLast_ = NewFirst + Size;
        GlyphFirst_ = NewFirst;
    }

    // erase(begin, end).
    if (GlyphFirst_ != nullptr)
    {
        GlyphLast_ = GlyphFirst_;
    }

    // the glyph map, built once.
    if (G_bNumberGlyphMapBuilt == 0)
    {
        G_bNumberGlyphMapBuilt = 1;
        std::memset(G_NumberGlyphMap, 0, sizeof(G_NumberGlyphMap));
        for (unsigned Index = 0; Index < 0x11u; ++Index)
        {
            G_NumberGlyphMap[static_cast<unsigned char>(G_NumberGlyphChars[Index])] =
                static_cast<unsigned char>(Index);
        }
    }

    // every byte of the output through the map, appended with the routine.
    // baseline indexes the map with the byte sign-extended (MOVSX), so a byte >= 0x80 would read
    // the 0x80 bytes before the field; vsnprintf of the callers' formats only yields ASCII.
    for (char const* It = Buffer; *It != '\0'; ++It)
    {
        NumberGlyphAppend(this, G_NumberGlyphMap[static_cast<unsigned char>(*It)]);
    }

    // +0x24 = 0, the size cache is stale.
    LayoutGen_ = 0;
}

void NumberStringChild::Paint(void* Origin, void* DrawCtx, void* DrawArg)
{
    // (RET 0xc) -- only the origin is read; the two trailing arguments are not.
    (void)DrawCtx;
    (void)DrawArg;
    // the kill switch the field, a null or empty glyph vector.
    if (G_NumberStringPaintDisable != 0)
    {
        return;
    }
    if (GlyphFirst_ == nullptr || GlyphLast_ == GlyphFirst_)
    {
        return;
    }
    // the face.
    if (FaceCache_ == nullptr)
    {
        FaceCache_ = NumberFaceGetOrCreate(Font_);
    }
    FNumberFace* const Face = static_cast<FNumberFace*>(FaceCache_);
    if (Face == nullptr)
    {
        return;
    }
    // its glyph FontString on first use (released again when it cannot be
    // rasterised).
    if (Face->Glyph == nullptr)
    {
        NumberFaceCreateGlyph(Face);
    }
    FontStringChild* const Glyph = Face->Glyph;
    if (Glyph == nullptr)
    {
        return;
    }
    // stale pages dropped, an empty grid rasterised with 0 (no release on
    // failure here); no page -- nothing drawn.
    FFontAtlasPage* const Page = FontStringChildEnsurePages(Glyph, 0);
    if (Page == nullptr)
    {
        return;
    }

    // the routine(the field).
    ScreenQuadSetMaterialKey(FontAtlasMaterialKey());
    // the shadow offset / pixel scale -- 2 above an 800-wide surface
    // (signed CMP / SETG), else 1.
    int const ScaleInt = static_cast<int>(G_RenderSurfaceWidth) > 0x320 ? 2 : 1;
    float const Scale = static_cast<float>(ScaleInt);
    // floor(x) - 0.5, floor(y) - 0.5 (the field, a double).
    FFontStringDrawOrigin const* const Org = static_cast<FFontStringDrawOrigin const*>(Origin);
    float const X0 = static_cast<float>(std::floor(static_cast<double>(Org->X)) - 0.5);
    float const Y0 = static_cast<float>(std::floor(static_cast<double>(Org->Y)) - 0.5);
    // the routine(frame, 2, 1) with the page texture in the frame's first
    // dword. baseline leaves the frame's four rect dwords unwritten (stack contents); the records
    // below carry their own UVs, so the port passes zeros there.
    unsigned long Frame[5] = {static_cast<unsigned long>(reinterpret_cast<uintptr_t>(Page->Texture)),
                              0ul, 0ul, 0ul, 0ul};
    ScreenQuadBeginBatchFrame(Frame, 2, true);
    // the routine on a stale face.
    if (Face->LayoutGen != G_FontLayoutGeneration)
    {
        NumberFaceRebuild(Face);
    }
    // this->vt+0x18.
    int Size[2] = {0, 0};
    GetSize(Size);

    // the string box {X0, Y0} .. {X0 + w + s, Y0 + h + s} (w, h FILD as
    // zero-extended qwords), with the UV rect {0, glyph 0's V0, 1, glyph 0's V1}.
    float const X1 = static_cast<float>(
        static_cast<double>(static_cast<unsigned>(Size[0])) + X0 + Scale);
    float const Y1 = static_cast<float>(
        static_cast<double>(static_cast<unsigned>(Size[1])) + Y0 + Scale);
    float ClipX0 = X0;
    float ClipY0 = Y0;
    float ClipX1 = X1;
    float ClipY1 = Y1;
    float ClipU0 = 0.0f;
    float ClipV0 = Face->Uv[0].V0;
    float ClipU1 = 1.0f;
    float ClipV1 = Face->Uv[0].V1;
    int const ScreenW = static_cast<int>(G_RenderSurfaceWidth);
    int const ScreenH = static_cast<int>(G_RenderSurfaceHeight);
    // the routine on y against [0, the field - 1], span (y1 - y0) + 1.
    if (ScreenQuadClip1D(&ClipY0, &ClipY1, &ClipV0, &ClipV1, 0.0f,
                         static_cast<float>(ScreenH - 1),
                         static_cast<float>(static_cast<double>(Y1) - Y0 + 1.0)) == 0)
    {
        return;
    }
    // the same on x against [0, the field - 1].
    if (ScreenQuadClip1D(&ClipX0, &ClipX1, &ClipU0, &ClipU1, 0.0f,
                         static_cast<float>(ScreenW - 1),
                         static_cast<float>(static_cast<double>(X1) - X0 + 1.0)) == 0)
    {
        return;
    }

    // one glyph per byte, the pen starting at the unclipped X0.
    float PenX = X0;
    for (unsigned char* It = GlyphFirst_; It != GlyphLast_; ++It)
    {
        unsigned const Index = *It;
        // the routine on a stale face, before and after the advance read.
        if (Face->LayoutGen != G_FontLayoutGeneration)
        {
            NumberFaceRebuild(Face);
        }
        float const Advance = static_cast<float>(Face->Advance[Index]);
        if (Face->LayoutGen != G_FontLayoutGeneration)
        {
            NumberFaceRebuild(Face);
        }
        float const GlyphX0 = PenX;
        PenX = static_cast<float>(static_cast<double>(Advance) + PenX);
        FNumberGlyphUv const& Uv = Face->Uv[Index];

        // a glyph starting left of the clipped box is skipped (TEST AH,5 /
        // JNP: ordered less only).
        if (GlyphX0 < ClipX0)
        {
            continue;
        }
        // one ending at or past the clipped right edge ends the string
        // (TEST AH,1 / JZ: only an ordered greater-or-equal leaves; unordered keeps drawing).
        if (PenX >= ClipX1)
        {
            return;
        }

        // with +0x20 set, the shadow quad offset by the scale: x from the
        // glyph, y from the clipped box top + scale down to the clipped box bottom, clipped on x
        // again; colour = the tint's alpha over this string's colour bytes.
        if (bShadow_ != 0)
        {
            float ShadowX0 = static_cast<float>(static_cast<double>(GlyphX0) + Scale);
            float const ShadowY0 = static_cast<float>(static_cast<double>(ClipY0) + Scale);
            float ShadowX1 = static_cast<float>(static_cast<double>(PenX) + Scale);
            float ShadowU0 = Uv.U0;
            float ShadowU1 = Uv.U1;
            if (ScreenQuadClip1D(&ShadowX0, &ShadowX1, &ShadowU0, &ShadowU1, 0.0f,
                                 static_cast<float>(ScreenW - 1),
                                 static_cast<float>(static_cast<double>(ShadowX1) - ShadowX0 + 1.0)) != 0)
            {
                unsigned const Tint = FontAtlasDrawColor();
                unsigned const Color =
                    NumberPackColor(static_cast<unsigned char>(Tint >> 24), Color_);
                FScreenQuadRecord Record;
                Record.X0 = ShadowX0;
                Record.Y0 = ShadowY0;
                Record.X1 = ShadowX1;
                Record.Y1 = ClipY1;
                Record.DepthA = 1.0f;
                Record.DepthB = 1.0f;
                Record.U0 = ShadowU0;
                Record.V0 = ClipV0;
                Record.U1 = ShadowU1;
                Record.V1 = ClipV1;
                Record.ColorBits = Color;
                ScreenQuadAppendRecord(Record);
            }
        }

        // the glyph quad from the clipped box top to its bottom - scale,
        // skipped when that is above the top (TEST AH,1 / JNZ: less or unordered); colour =
        // the tint (the field..77) with the same byte order.
        float const GlyphY1 = static_cast<float>(static_cast<double>(ClipY1) - Scale);
        if (!(GlyphY1 >= ClipY0))
        {
            continue;
        }
        unsigned const Tint = FontAtlasDrawColor();
        unsigned const Color = NumberPackColor(static_cast<unsigned char>(Tint >> 24), Tint);
        FScreenQuadRecord Record;
        Record.X0 = GlyphX0;
        Record.Y0 = ClipY0;
        Record.X1 = PenX;
        Record.Y1 = GlyphY1;
        Record.DepthA = 1.0f;
        Record.DepthB = 1.0f;
        Record.U0 = Uv.U0;
        Record.V0 = ClipV0;
        Record.U1 = Uv.U1;
        Record.V1 = ClipV1;
        Record.ColorBits = Color;
        ScreenQuadAppendRecord(Record);
    }
}

FNumberFace* NumberFaceGetOrCreate(IFont* FontKey)
{
    // (baseline takes the key by pointer). A zero key returns null (0041371e).
    if (FontKey == nullptr)
    {
        return nullptr;
    }
    // insert (key, null) or land on the existing node; the value slot is node+0x10.
    FNumberFace*& Slot =
        G_NumberFaceMap
            .insert(std::map<unsigned, FNumberFace*>::value_type(
                static_cast<unsigned>(reinterpret_cast<uintptr_t>(FontKey)), nullptr))
            .first->second;
    if (Slot != nullptr)
    {
        // bump +0x160 and hand back the cached face.
        ++Slot->RefCount;
        return Slot;
    }
    // operator new(0x164); only +0, +4, +0x15c and +0x160 are written. A
    // failed allocation stores null into the node (00413783).
    FNumberFace* Face = new (std::nothrow) FNumberFace;
    if (Face != nullptr)
    {
        Face->Glyph = nullptr;
        Face->LayoutGen = 0;
        Face->FontKey = FontKey;
        Face->RefCount = 1;
    }
    Slot = Face;
    return Face;
}

void NumberFaceRelease(FNumberFace* Face)
{
    // The sequence the routine (00414fcc..00415034) and the NumberString dtor
    // (00415524..00415588) both inline: find by +0x15c (the routine), drop +0x160, clear and
    // delete the face at zero (the routine + operator delete), then erase the node
    // (the routine) when it was found and the count reached zero. Callers test the face first.
    std::map<unsigned, FNumberFace*>::iterator Found =
        G_NumberFaceMap.find(static_cast<unsigned>(reinterpret_cast<uintptr_t>(Face->FontKey)));
    int const Remaining = --Face->RefCount;
    if (Remaining <= 0)
    {
        // .
        if (Face->Glyph != nullptr)
        {
            Face->Glyph->Release();
            Face->Glyph = nullptr;
        }
        Face->LayoutGen = 0;
        delete Face;
    }
    if (Found != G_NumberFaceMap.end() && Remaining <= 0)
    {
        G_NumberFaceMap.erase(Found);
    }
}

void NumberFaceResetAll()
{
    // 004180d3..00418116: in-order walk from begin() (the routine is ++it); a
    // null value is skipped, otherwise the routine inline.
    std::map<unsigned, FNumberFace*>::iterator It = G_NumberFaceMap.begin();
    for (; It != G_NumberFaceMap.end(); ++It)
    {
        FNumberFace* const Face = It->second;
        if (Face == nullptr)
        {
            continue;
        }
        if (Face->Glyph != nullptr)
        {
            Face->Glyph->Release();
            Face->Glyph = nullptr;
        }
        Face->LayoutGen = 0;
    }
}
