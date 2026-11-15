// Author: Giperion.

#include "font_atlas.h"

#include "font.h"
#include "font_imp.h"
#include "font_string.h"
#include "pixel_format.h"

#include "NeuronRuntime.h"
#include "neuroncomponents.h"
#include "render/IRenderPipeline.h"

#include <cstdlib>
#include <cstring>
#include <vector>
#include <wchar.h>

namespace
{

// ..6c — shared GDI scratch for glyph upload.
HBITMAP G_AtlasBitmap = nullptr;
HGDIOBJ G_AtlasPrevObj = nullptr;
unsigned short* G_AtlasBits = nullptr;
BITMAPINFO* G_AtlasBmi = nullptr;
HDC G_AtlasDc = nullptr;
// / the field are 0 in the PE; the routine (the scratch object's +0x14 / +0x18,
// thiscall on the field) is their only writer and sets both to 0x100.
unsigned G_AtlasMaxW = 0;
unsigned G_AtlasMaxH = 0;
// seeded at load time by XC thunk the branch: NEURONCOMPONENTS_CRC::GetCRC32("font")
// (the field). Swapped by the routine.
unsigned G_AtlasMaterialKey = NEURONCOMPONENTS_CRC::GetCRC32("font");
// ..77 -- zeroed byte by byte by XC thunk the branch (00412622..00412631); the
// only writer after that is the routine (FontAtlasSetDrawColor).
unsigned G_AtlasDrawColor = 0;
unsigned G_AtlasTexBytes = 0;      // +0x10

// ..4c — free page pointer vector (the routine / the routine push).
std::vector<FFontAtlasPage*> G_AtlasFreePages;

unsigned RoundUpPow2(unsigned Value)
{
    // (00412834..00412880) / the routine (00412d41..00412d66): a value
    // with (x & (x - 1)) == 0 -- 0 included -- is kept; otherwise start at (x > 0x40 ? 0x80 : 1)
    // and double while below x.
    if ((Value & (Value - 1u)) == 0u)
    {
        return Value;
    }
    unsigned Result = (Value > 0x40u) ? 0x80u : 1u;
    while (Result < Value)
    {
        Result <<= 1u;
    }
    return Result;
}

IRenderPipeline* Pipe()
{
    if (NeuronRuntime::Pipeline == nullptr)
    {
        return nullptr;
    }
    return NeuronRuntime::Pipeline;
}

FFontAtlasPage* CreateAtlasPage(unsigned NeedW, unsigned NeedH)
{
    // (thiscall on the pool the field, RET 8).
    // / 0x00412d7a..0x00412da7: the first free page whose size (+4 / +8,
    // unsigned) covers the raw request is erased from the vector and returned as it is -- its
    // offsets, used size and content height keep whatever its last string left there.
    for (std::vector<FFontAtlasPage*>::iterator It = G_AtlasFreePages.begin();
         It != G_AtlasFreePages.end(); ++It)
    {
        FFontAtlasPage* const Page = *It;
        if (Page->PageW >= NeedW && Page->PageH >= NeedH)
        {
            G_AtlasFreePages.erase(It);
            return Page;
        }
    }

    // operator new(0x28), all ten dwords zeroed.
    FFontAtlasPage* const Page = new FFontAtlasPage();
    memset(Page, 0, sizeof(*Page));

    // the routine -- the id of a 16-bit 5/5/5/1 format.
    unsigned long Format = 0;
    PixelFormatFind(&Format, 0x10, 5, 5, 5, 1);

    // both sizes to a power of two (x > 0x40 ? 0x80 : 1, doubled until
    // it covers x) into +4 / +8; +0 = 0.
    unsigned const TexW = RoundUpPow2(NeedW);
    Page->PageW = TexW;
    unsigned const TexH = RoundUpPow2(NeedH);
    Page->PageH = TexH;
    Page->Texture = nullptr;

    // NeuronRuntime::Pipeline vt+0xb4 CreateTexture(w, h, &format, 1, 0, page)
    // -- the handle lands in +0; a failure is reported, the page deleted, 0 returned.
    unsigned Format32 = static_cast<unsigned>(Format);
    GENRESULT const Gr = NeuronRuntime::Pipeline->CreateTexture(
        static_cast<int>(TexW), static_cast<int>(TexH), &Format32, 1u, 0u, &Page->Texture);
    if (Gr != GR_OK)
    {
        FDUMP(0x100002, "%s(%d) : *** WARNING: %s",
              "Client\\cmn\\Font.cpp", 0x192,
              "FontString - failed to create texture");
        delete Page;
        return nullptr;
    }
    // the pool's byte total (+0x10) grows by w * h * 2.
    G_AtlasTexBytes += TexW * TexH * 2u;
    return Page;
}

// the font selection the routine holds over its draw loop ([ESP+0x40] = DC,
// [ESP+0x44] = the previously selected object, built at 00412a0f..00412a21). Its destructor
// runs inline on the normal exit (00412c86..00412c90) and as the EH unwind funclet
// (005b87f0: LEA ECX,[EBP-0x38]; JMP the routine).
struct FAtlasFontSelection
{
    HDC Dc;
    HGDIOBJ Previous;

    FAtlasFontSelection(HDC InDc, HGDIOBJ Font)
        : Dc(InDc)
        , Previous(SelectObject(InDc, Font))
    {
    }

    ~FAtlasFontSelection()
    {
        SelectObject(Dc, Previous);
    }

    FAtlasFontSelection(FAtlasFontSelection const&) = delete;
    FAtlasFontSelection& operator=(FAtlasFontSelection const&) = delete;
};

} // namespace

void FontAtlasEnsureScratch()
{
    // (thiscall on the field, RET 4 with NeuronRuntime::Pipeline, which it never reads).
    // Its only caller is the routine, which deletes the previous objects first: there is no
    // lazy latch and no allocation check. Only the 0x28-byte header is zeroed (STOSD x 0xa at
    // ); the pixel area is left as malloc returns it.
    G_AtlasMaxW = 256;
    G_AtlasMaxH = 256;

    void* Block = std::malloc(0x20028u);
    memset(Block, 0, 0x28u);
    G_AtlasBmi = static_cast<BITMAPINFO*>(Block);
    G_AtlasBits = reinterpret_cast<unsigned short*>(static_cast<unsigned char*>(Block) + 0x28);

    BITMAPINFOHEADER& Hdr = G_AtlasBmi->bmiHeader;
    Hdr.biSize = sizeof(BITMAPINFOHEADER);
    Hdr.biWidth = static_cast<LONG>(G_AtlasMaxW);
    Hdr.biHeight = -static_cast<LONG>(G_AtlasMaxH);
    Hdr.biPlanes = 1;
    Hdr.biBitCount = 16;
    Hdr.biCompression = BI_RGB;
    Hdr.biSizeImage = 0x20000;

    HDC Screen = GetDC(nullptr);
    G_AtlasDc = CreateCompatibleDC(Screen);
    G_AtlasBitmap = CreateCompatibleBitmap(Screen, static_cast<int>(G_AtlasMaxW),
                                           static_cast<int>(G_AtlasMaxH));
    GetDIBits(G_AtlasDc, G_AtlasBitmap, 0, 1, nullptr, G_AtlasBmi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, Screen);

    G_AtlasPrevObj = SelectObject(G_AtlasDc, G_AtlasBitmap);
    SetTextAlign(G_AtlasDc, TA_LEFT | TA_TOP);
    SetBkColor(G_AtlasDc, RGB(0, 0, 0));
    SetBkMode(G_AtlasDc, TRANSPARENT);
    SetTextColor(G_AtlasDc, RGB(255, 255, 255));
}

void FontDeviceReset()
{
    // (called from the render-manager init at 00424bf1).
    // bump the layout generation, skipping 0 on wrap.
    ++G_FontLayoutGeneration;
    if (G_FontLayoutGeneration == 0)
    {
        G_FontLayoutGeneration = 1;
    }
    // drop every number face's glyph string (the routine inlined).
    NumberFaceResetAll();
    // free the DIB block, put the old bitmap back, delete bitmap and DC.
    FontAtlasReleaseScratch();
    // the routine(NeuronRuntime::Pipeline) rebuilds the scratch.
    FontAtlasEnsureScratch();
}

void FontAtlasReleaseScratch()
{
    // (the routine) / 004174a0..004174e6 (the routine).
    std::free(G_AtlasBmi);
    G_AtlasBmi = nullptr;
    SelectObject(G_AtlasDc, G_AtlasPrevObj);
    DeleteObject(G_AtlasBitmap);
    DeleteDC(G_AtlasDc);
}

void FontAtlasReleasePagePool()
{
    // 00417545..004175c0. NeuronRuntime::Pipeline is re-read for every page.
    while (!G_AtlasFreePages.empty())
    {
        FFontAtlasPage* const Page = G_AtlasFreePages.front(); // MOV ESI,[EDI]
        IRenderPipeline* const Rp = Pipe();
        if (Rp != nullptr)
        {
            Rp->DestroyTexture(Page->Texture);             // +0xb8
            G_AtlasTexBytes -= Page->PageH * Page->PageW * 2u;
        }
        delete Page; // (operator delete, skipped for null)
        // slide the rest down one slot.
        G_AtlasFreePages.erase(G_AtlasFreePages.begin());
    }
}

void FontAtlasFreePages(FFontAtlasPage*& Head)
{
    // (thiscall on the pool the field, RET 4; analysis splits the body at
    // ). A null grid returns at once. For each row head the next row (+0x24) is read
    // first; then every page of the row, in order: its next column (+0x20) read, +0x24 and
    // +0x20 zeroed, the page pushed onto the free vector (the push_back growth path is
    // ). Textures stay alive. The callers (0041384e / 00413892 /
    // / 00415127) clear their grid pointer (+0x14) afterwards -- done here through
    // the reference.
    FFontAtlasPage* Row = Head;
    while (Row != nullptr)
    {
        FFontAtlasPage* const NextRow = Row->NextY;
        FFontAtlasPage* Col = Row;
        while (Col != nullptr)
        {
            FFontAtlasPage* const NextCol = Col->NextX;
            Col->NextY = nullptr;
            Col->NextX = nullptr;
            G_AtlasFreePages.push_back(Col);
            Col = NextCol;
        }
        Row = NextRow;
    }
    Head = nullptr;
}

unsigned FontAtlasMaterialKey()
{
    return G_AtlasMaterialKey;
}

void FontAtlasSetMaterialKey(unsigned Key)
{
    G_AtlasMaterialKey = Key;
}

unsigned FontAtlasDrawColor()
{
    return G_AtlasDrawColor;
}

void FontAtlasSetDrawColor(unsigned PackedArgb)
{
    G_AtlasDrawColor = PackedArgb;
}

unsigned char FontAtlasRasterize(FontStringChild* This, unsigned char bSkipSizeCheck)
{
    // (thiscall on a FontString, RET 4).
    // no font (+0x0c) -> 0.
    if (This->Font_ == nullptr)
    {
        return 0;
    }

    // the size through the string's own vt+0x18 (GetSize, the branch),
    // which may itself send a stale grid back to the pool before +0x14 is read below.
    // [ESP+0x30] / [ESP+0x34] = the extents the grid is checked and built with,
    // [ESP+0x38] / [ESP+0x3c] = the raw extents the text is drawn with.
    int Size[2];
    This->GetSize(Size);
    int const RawW = Size[0];
    int const RawH = Size[1];
    unsigned CheckW = static_cast<unsigned>(RawW);
    unsigned CheckH = static_cast<unsigned>(RawH);

    // with a zero argument both extents go to a power of two and must fit
    // the scratch (JA against the field, JBE against the field); otherwise the write-only
    // byte the field is set and 0 returned. A non-zero argument skips both.
    if (bSkipSizeCheck == 0)
    {
        CheckW = RoundUpPow2(CheckW);
        CheckH = RoundUpPow2(CheckH);
        if (CheckW > G_AtlasMaxW || CheckH > G_AtlasMaxH)
        {
            return 0;
        }
    }

    // an existing grid (+0x14) is kept when its first row's widths (+4 over
    // +0x20) and first column's heights (+8 over +0x24) cover the checked extents (unsigned JC);
    // with the head's content height (+0x0c) equal to the raw height it is returned as is (1),
    // otherwise it is redrawn. A grid that does not cover goes back to the pool (the routine
    // inlined at 004128e4..00412923) and +0x14 is cleared.
    if (This->GlyphHead_ != nullptr)
    {
        unsigned SumW = 0;
        for (FFontAtlasPage* Page = This->GlyphHead_; Page != nullptr; Page = Page->NextX)
        {
            SumW += Page->PageW;
        }
        bool bCovers = SumW >= CheckW;
        if (bCovers)
        {
            unsigned SumH = 0;
            for (FFontAtlasPage* Page = This->GlyphHead_; Page != nullptr; Page = Page->NextY)
            {
                SumH += Page->PageH;
            }
            bCovers = SumH >= CheckH;
        }
        if (bCovers)
        {
            if (This->GlyphHead_->ContentH == static_cast<unsigned>(RawH))
            {
                return 1;
            }
        }
        else
        {
            FontAtlasFreePages(This->GlyphHead_);
        }
    }

    // an empty grid is built from the checked extents -- rows of at most
    // , columns of at most the field (unsigned clamps, signed loop tests, both
    // limits re-read every step). Each page comes from the pool (the routine), its result not
    // tested; the first page of a row is linked to the previous row's first page through +0x24,
    // the others to their left neighbour through +0x20; +0x10 / +0x14 get the page's offset in
    // the string. The head goes into +0x14 even when null, and a null head returns 0.
    if (This->GlyphHead_ == nullptr)
    {
        FFontAtlasPage* Head = nullptr;    // [ESP+0x10]
        FFontAtlasPage* RowTail = nullptr; // [ESP+0x24]
        for (int LeftH = static_cast<int>(CheckH); LeftH > 0;
             LeftH -= static_cast<int>(G_AtlasMaxH))
        {
            unsigned TileH = static_cast<unsigned>(LeftH);
            if (TileH >= G_AtlasMaxH)
            {
                TileH = G_AtlasMaxH;
            }
            int const SrcY = static_cast<int>(CheckH) - LeftH;
            FFontAtlasPage* Previous = nullptr;
            for (int LeftW = static_cast<int>(CheckW); LeftW > 0;
                 LeftW -= static_cast<int>(G_AtlasMaxW))
            {
                unsigned TileW = G_AtlasMaxW;
                if (static_cast<unsigned>(LeftW) < TileW)
                {
                    TileW = static_cast<unsigned>(LeftW);
                }
                FFontAtlasPage* const Page = CreateAtlasPage(TileW, TileH);
                if (Previous != nullptr)
                {
                    Previous->NextX = Page;
                }
                else if (Head == nullptr)
                {
                    Head = Page;
                    RowTail = Page;
                }
                else
                {
                    RowTail->NextY = Page;
                    RowTail = Page;
                }
                Page->SrcX = static_cast<int>(CheckW) - LeftW;
                Page->SrcY = SrcY;
                Previous = Page;
            }
        }
        This->GlyphHead_ = Head;
        if (Head == nullptr)
        {
            return 0;
        }
    }

    // the argument slot becomes the success byte (1); IFont vt+0x0c
    // (the branch) returns the HFONT, selected into the scratch DC without a test and held by
    // the RAII selection until the end.
    unsigned char bOk = 1;
    FFontAtlasPage* Row = This->GlyphHead_; // [ESP+0x20]
    HFONT const Font = This->Font_->Slot03();
    FAtlasFontSelection const Selection(G_AtlasDc, Font);

    // the raw extents walk the grid -- row count and column count follow
    // the extents alone (no null test on the page links).
    for (int LeftH = RawH; LeftH > 0; LeftH -= static_cast<int>(G_AtlasMaxH))
    {
        unsigned TileH = static_cast<unsigned>(LeftH); // [ESP+0x10]
        if (TileH >= G_AtlasMaxH)
        {
            TileH = G_AtlasMaxH;
        }
        FFontAtlasPage* Page = Row; // [ESP+0x2c]
        for (int LeftW = RawW; LeftW > 0; LeftW -= static_cast<int>(G_AtlasMaxW))
        {
            unsigned TileW = G_AtlasMaxW; // [ESP+0x18]
            if (static_cast<unsigned>(LeftW) < TileW)
            {
                TileW = static_cast<unsigned>(LeftW);
            }
            // the used size (+0x18 / +0x1c) and the clip rect {0,0,w,h}.
            int const SrcX = Page->SrcX;
            int const SrcY = Page->SrcY;
            Page->UsedW = TileW;
            Page->UsedH = TileH;
            RECT Clip;
            Clip.left = 0;
            Clip.top = 0;
            Clip.right = static_cast<LONG>(TileW);
            Clip.bottom = static_cast<LONG>(TileH);

            // ExtTextOutW (through the unicows thunk the routine) at the
            // negated offset, option 2 (ETO_OPAQUE), the text (+4) with its wcslen (0 for null).
            UINT const Length =
                This->Text_ != nullptr ? static_cast<UINT>(wcslen(This->Text_)) : 0u;
            if (ExtTextOutW(G_AtlasDc, -SrcX, -SrcY, ETO_OPAQUE, &Clip, This->Text_, Length,
                            nullptr) == FALSE)
            {

                FDUMP(0x100002, "%s(%d) : *** WARNING: %s",
                      "Client\\cmn\\Font.cpp", 0x329,
                      "FontString - ExtTextOutW failed.\n");
                bOk = 0;
            }
            else if (bOk != 0)
            {
                // the scratch bitmap is deselected for GetDIBits over the
                // full scratch height (the field) into the DIB block, then selected again.
                SelectObject(G_AtlasDc, G_AtlasPrevObj);
                GetDIBits(G_AtlasDc, G_AtlasBitmap, 0, G_AtlasMaxH, G_AtlasBits, G_AtlasBmi,
                          DIB_RGB_COLORS);
                SelectObject(G_AtlasDc, G_AtlasBitmap);
                unsigned short const* Source = G_AtlasBits;

                // NeuronRuntime::Pipeline vt+0xc0 LockTexture(page +0, 0, &out);
                // only out+0x10 is seeded, with the routine(0). out+8 = pitch, out+0xc = bits.
                unsigned long LockOut[5];
                LockOut[4] = static_cast<unsigned long>(PixelFormatFromD3D(0));
                IRenderPipeline* const Pipeline =
                    NeuronRuntime::Pipeline;
                if (Pipeline->LockTexture(Page->Texture, 0, LockOut) != GR_OK)
                {

                    FDUMP(0x100002, "%s(%d) : *** WARNING: %s",
                          "Client\\cmn\\Font.cpp", 0x34c,
                          "FontString - Could not lock texture.");
                    bOk = 0;
                }
                else
                {
                    // every pixel of the tile with bit 15 set when non-zero;
                    // the source row advances by the field pixels, the destination by pitch.
                    unsigned long const Pitch = LockOut[2];
                    unsigned char* DestRow = reinterpret_cast<unsigned char*>(LockOut[3]);
                    for (unsigned RowsLeft = TileH; RowsLeft != 0u; --RowsLeft)
                    {
                        unsigned short* const Dest = reinterpret_cast<unsigned short*>(DestRow);
                        for (unsigned Col = 0; Col < TileW; ++Col)
                        {
                            unsigned short const Pixel = Source[Col];
                            Dest[Col] = static_cast<unsigned short>(
                                ((Pixel != 0) ? 0x8000u : 0u) | Pixel);
                        }
                        Source += G_AtlasMaxW;
                        DestRow += Pitch;
                    }
                    // NeuronRuntime::Pipeline (re-read) vt+0xc4 UnlockTexture.
                    NeuronRuntime::Pipeline
                        ->UnlockTexture(Page->Texture, 0);
                }
            }

            Page = Page->NextX;
        }

        Row = Row->NextY;
    }

    // with no failure the raw height is stamped into the head (+0x0c).
    if (bOk != 0)
    {
        This->GlyphHead_->ContentH = static_cast<unsigned>(RawH);
    }
    // the selection's destructor puts the previous font back; AL = the byte.
    return bOk;
}
