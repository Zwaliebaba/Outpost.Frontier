// Author: Giperion.

#include "image.h"

#include "dataconfig_progress.h" // GetIntroConfigState: TgaPath the field
#include "globals.h"             // g_display.data_path the field, G_RenderSurfaceWidth/Height
#include "widescreen.h"   // port-only (OUTPOST_FRONTIER_WIDESCREEN_SUPPORT)
#include "pixel_format.h"
#include "screen_quad_state.h"    // ScreenQuadDrawBigImageTile (the routine)
#include "win3d_immediate.h"     // Win3DSetOrtho2D (the routine)

#include "NeuronRuntime.h"
#include "neuroncomponents.h"
#include "readfile.h"
#include "render/IBigImage2.h"
#include "render/IRenderPipeline.h"

#include <windows.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <new>
#include <string>

static_assert(sizeof(void*) != 4 || offsetof(Image, BigImage) == 0x14, "Image +0x14 BigImage");

namespace
{
    char const kIntroCpp[] = "Client\\Interface\\Intro.cpp";
    char const kBigImageFailed[] = "Failed to create a BigImage from an image source!\n";

    // The render-state saver the routine builds seven of: {Pipeline, State, Saved}, filled by
    // GetRenderState (IRenderPipeline slot 61, +0xf4) and then forced by SetRenderState (slot
    // 60, +0xf0); its destructor the routine puts Saved back. Destroyed in reverse order, as
    // the EH states 6..0 of 0x00565a10 unwind them.
    struct FImageRenderStateSave
    {
        FImageRenderStateSave(int NewState, int Value)
        {
            Pipeline = NeuronRuntime::Pipeline;
            State = NewState;
            Pipeline->GetRenderState((D3DRENDERSTATETYPE)State, &Saved);
            Pipeline->SetRenderState((D3DRENDERSTATETYPE)State, Value);
        }
        ~FImageRenderStateSave()
        {
            // .
            Pipeline->SetRenderState((D3DRENDERSTATETYPE)State, Saved);
        }

        IRenderPipeline* Pipeline;
        int State;
        int Saved;

    private:
        FImageRenderStateSave(FImageRenderStateSave const&) = delete;
        FImageRenderStateSave& operator=(FImageRenderStateSave const&) = delete;
    };

    // {D3DTEXTURESTAGESTATETYPE, value} pairs, zero-terminated: .rdata 0x005e2558 (stage 0) and
    // (stage 1), read from the PE bytes.
    struct FTextureStageSetting
    {
        int Type;
        int Value;
    };
    FTextureStageSetting const kImageStage0[] = {
        {1, 4}, {2, 2}, {3, 0}, {4, 4}, {5, 2}, {6, 0}, {0, 0}};
    FTextureStageSetting const kImageStage1[] = {
        {1, 1}, {2, 2}, {3, 1}, {4, 1}, {5, 2}, {6, 1}, {0, 0}};

    // The mapped-file object the routine keeps on its stack (0x1c..0x2c): the routine is its
    // IFileSystem AddRef copy, the routine Open, the routine Close, the routine the dtor the
    // EH unwind (0x005c1f3e) runs.
    struct FImageMappedFile
    {
        explicit FImageMappedFile(IFileSystem* NewFs)
        {
            // (RET 4): store and AddRef (slot 1) when non-null.
            Fs = NewFs;
            if (Fs != nullptr)
                Fs->AddRef();
            // . Mapping (+0x08) is left unwritten: Open stores it before
            // anything reads it, and Close reads it only behind a valid Handle.
            Handle = 0xffffffffu;
            Size = 0;
            View = 0;
        }

        ~FImageMappedFile()
        {
            // Close's body, then the IFileSystem member's own release (already
            // null by then).
            Close();
            if (Fs != nullptr)
            {
                Fs->Release();
                Fs = nullptr;
            }
        }

        // (thiscall, RET 8): 0 when the file is mapped; -8 open failed, -6 mapping
        // failed, -1 view failed.
        int Open(char const* Path, unsigned long Field30)
        {
            FFileSystemDesc Desc;
            memset(&Desc, 0, 0x34);
            Desc.Size = 0x34;
            Desc.ComponentName = "FileSystem";
            Desc.Path = Path;
            Desc.Access = 0x80000000u;
            Desc.ShareMode = 1;
            Desc.Creation = 3;
            Desc.Flags = 0x8000080u;
            Desc.Field30 = Field30;
            Handle = static_cast<unsigned long>(Fs->OpenByDesc(&Desc)); // slot 31 (+0x7c)
            if (Handle == 0xffffffffu)
                return -8;
            Size = Fs->GetSize(Handle, 0); // +0x28
            Mapping = static_cast<unsigned long>(Fs->CreateMapping(Handle, 0, 2, 0, 0, 0));
            if (Mapping == 0xffffffffu)
                return -6;
            View = static_cast<unsigned long>(Fs->MapFileView(Mapping, 4, 0, 0, 0));
            if (View != 0)
                return 0;
            return -1;
        }

        // .
        void Close()
        {
            if (Handle != 0xffffffffu)
            {
                if (Mapping != 0xffffffffu)
                {
                    if (View != 0)
                    {
                        Fs->UnmapFileView(View); // +0x44
                        View = 0;
                    }
                    Fs->CloseFile(Mapping); // +0x10
                }
                Fs->CloseFile(Handle);
                Handle = 0xffffffffu;
            }
            if (Fs != nullptr)
            {
                Fs->Release();
                Fs = nullptr;
            }
        }

        unsigned long Handle;  // +0x00
        unsigned long Size;    // +0x04
        unsigned long Mapping; // +0x08
        unsigned long View;    // +0x0c
        IFileSystem* Fs;       // +0x10

    private:
        FImageMappedFile(FImageMappedFile const&) = delete;
        FImageMappedFile& operator=(FImageMappedFile const&) = delete;
    };

    // (thiscall on the TGA header): the pixel depth -- byte +7 (colour-map entry
    // size) for a colour-mapped image, else byte +0x10 -- equal to 32 means 8 alpha bits.
    bool TgaHasAlpha(unsigned char const* Header)
    {
        unsigned char const Bits = ((Header[2] & 7) == 1) ? Header[7] : Header[0x10];
        int const AlphaBits = (Bits == 0x20) ? 8 : 0;
        return AlphaBits > 0;
    }

    // (thiscall on the TGA header): colour-mapped -> first entry index (MOVSX
    // +3) + map length (MOVSX +5); type 3 -> 0x100; otherwise 0.
    int TgaPaletteEntries(unsigned char const* Header)
    {
        unsigned char const Type = static_cast<unsigned char>(Header[2] & 7);
        if (Type == 1)
        {
            short Length = 0;
            short First = 0;
            memcpy(&Length, Header + 5, sizeof(Length));
            memcpy(&First, Header + 3, sizeof(First));
            return static_cast<int>(Length) + static_cast<int>(First);
        }
        if (Type == 3)
            return 0x100;
        return 0;
    }

    short TgaShort(unsigned char const* At)
    {
        short Value = 0;
        memcpy(&Value, At, sizeof(Value));
        return Value;
    }

    // The 32-iteration bit count 0x005652c0..0x00565301 over one DIB colour mask.
    unsigned long CountMaskBits(unsigned long Mask)
    {
        unsigned long Count = 0;
        for (int Bit = 0; Bit < 0x20; ++Bit)
        {
            if ((Mask & 1u) != 0)
                ++Count;
            Mask >>= 1;
        }
        return Count;
    }
}

IImage::~IImage()
{
    // the base vptr store only.
}

Image::Image()
    : BigImage(nullptr)
{
}

Image::~Image()
{
    // release +0x14 (0x00565040..0x00565048), then the member's own release
    // (0x0056504f..0x00565061), which finds it null.
    Release();
    if (BigImage != nullptr)
    {
        BigImage->Release();
        BigImage = nullptr;
    }
}

void Image::Release()
{
    // .
    if (BigImage != nullptr)
    {
        BigImage->Release();
        BigImage = nullptr;
    }
}

bool Image::Load(char const* Path)
{
    // .
    char const* const Dot = strrchr(Path, '.');
    if (Dot != nullptr)
    {
        char const* const Extension = Dot + 1;
        if (_stricmp(Extension, "bmp") == 0) // NEURONCOMPONENTS stricmp, 0x005e279c
            return LoadBmp(Path);
        // stricmp against "tga" (0x005e2798); the result is not read.
        (void)_stricmp(Extension, "tga");
    }
    return LoadTga(Path);
}

bool Image::LoadBmp(char const* Path)
{
    // .
    if (BigImage != nullptr)
        return true;

    // data path, a '\\' unless it is empty or already ends in one,
    // TgaPath, then Path.
    std::string FullPath(g_display.data_path.c_str());
    if (!FullPath.empty() && FullPath[FullPath.size() - 1] != '\\')
        FullPath.append("\\");
    FullPath.append(GetIntroConfigState()->TgaPath.Text);
    FullPath.append(Path);

    bool bResult = false;
    HBITMAP const Bitmap = static_cast<HBITMAP>(
        LoadImageA(nullptr, FullPath.c_str(), IMAGE_BITMAP, 0, 0, 0x2010));
    if (Bitmap == nullptr)
        return bResult;

    DIBSECTION Dib;
    memset(&Dib, 0, sizeof(Dib));
    if (GetObjectA(Bitmap, 0x54, &Dib) == 0x54)
    {
        unsigned long const Compression = Dib.dsBmih.biCompression;
        unsigned short const BitCount = Dib.dsBmih.biBitCount; // word at +0x26
        if ((Compression == 0 || Compression == 3) &&
            (BitCount == 0x10 || BitCount == 0x18 || BitCount == 0x20))
        {
            unsigned long RedBits = 8;
            unsigned long GreenBits = 8;
            unsigned long BlueBits = 8;
            if (Compression != 0)
            {
                // BI_BITFIELDS: the widths are the masks' bit counts.
                RedBits = CountMaskBits(Dib.dsBitfields[0]);
                GreenBits = CountMaskBits(Dib.dsBitfields[1]);
                BlueBits = CountMaskBits(Dib.dsBitfields[2]);
            }
            unsigned long ImageFormat = 0;
            PixelFormatFind(&ImageFormat, BitCount, RedBits, GreenBits, BlueBits, 0);
            unsigned long TargetFormat = 0;
            PixelFormatFind(&TargetFormat, 0x20, 8, 8, 8, 0);

            // Descriptor +0x10 is the 32-bit target format, +0x14 the DIB's own (the header
            // calls them SrcFormat / DstFormat; ximage uses +0x10 to choose the texture format
            // and +0x14 to step the pixels).
            FBigImageDesc Desc;
            Desc.Size = 0x2c;
            Desc.Name = "BigImage";
            Desc.Pipeline = NeuronRuntime::Pipeline;
            Desc.ImageSource = nullptr;
            Desc.SrcFormat = &TargetFormat;
            Desc.DstFormat = &ImageFormat;
            Desc.Pixels = Dib.dsBm.bmBits;
            Desc.Width = static_cast<unsigned long>(Dib.dsBm.bmWidth);
            Desc.Height = static_cast<unsigned long>(Dib.dsBm.bmHeight);
            Desc.Pitch = static_cast<unsigned long>(Dib.dsBm.bmWidthBytes);
            Desc.DrawTile = &ScreenQuadDrawBigImageTile;

            Release();
            if (NeuronRuntime::NeuronComponents->CreateInstance(reinterpret_cast<AGGDESC*>(&Desc),
                                             reinterpret_cast<void**>(&BigImage)) == 0)
            {
                bResult = true;
            }
            else
            {
                FDUMP(0x100001, "%s(%d) : ERROR:General:%s", kIntroCpp, 0x1ab, kBigImageFailed);
                bResult = false;
            }
        }
    }
    DeleteObject(Bitmap);
    return bResult;
}

bool Image::LoadTga(char const* Path)
{
    // .
    if (BigImage != nullptr)
        return true;

    // a "FileSystem" (desc size 0x34) rooted at TgaPath under
    // NeuronRuntime::Data.
    FFileSystemDesc Desc;
    memset(&Desc, 0, 0x34);
    Desc.Size = 0x34;
    Desc.ComponentName = "FileSystem";
    Desc.Path = GetIntroConfigState()->TgaPath.Text;
    Desc.Access = 0x80000000u;
    Desc.ShareMode = 1;
    Desc.Creation = 3;
    Desc.Flags = 0x8000080u;
    Desc.Field30 = 0xffffffffu;
    IFileSystem* SubFs = nullptr;
    if (NeuronRuntime::Data->CreateInstance(reinterpret_cast<AGGDESC*>(&Desc),
                                    reinterpret_cast<void**>(&SubFs)) != 0)
    {
        // the routine on the holder, return false.
        if (SubFs != nullptr)
        {
            SubFs->Release();
            SubFs = nullptr;
        }
        return false;
    }

    bool bResult = false;
    {
        FImageMappedFile Mapped(SubFs);
        if (Mapped.Open(Path, 0xffffffffu) >= 0)
        {
            unsigned char const* const Header =
                reinterpret_cast<unsigned char const*>(static_cast<uintptr_t>(Mapped.View));

            // the header's format, seeded with the routine(0).
            unsigned long ImageFormat = static_cast<unsigned long>(PixelFormatFromD3D(0));
            unsigned char const Type = static_cast<unsigned char>(Header[2] & 7);
            if (Type == 1 && Header[1] == 1)
                PixelFormatFind(&ImageFormat, 8, 0, 0, 0, 0);
            else if (Header[0x10] == 0x10)
                PixelFormatFind(&ImageFormat, 0x10, 5, 5, 5, 0);
            else if (Type == 3)
                PixelFormatFind(&ImageFormat, 8, 8, 0, 0, 0);
            else if (TgaHasAlpha(Header))
                PixelFormatFind(&ImageFormat, 0x20, 8, 8, 8, 8);
            else
                PixelFormatFind(&ImageFormat, 0x18, 8, 8, 8, 0);

            // CMP bpp,0x20 / JNC -- anything under 32 bits fails.
            unsigned long const Bpp = PixelFormatGetDesc(static_cast<int>(ImageFormat))->Bpp;
            if (Bpp >= 0x20)
            {
                // pitch = ((bpp + 7) >> 3) * (short)width (IMUL).
                long const Width = static_cast<long>(TgaShort(Header + 0xc));
                long const Height = static_cast<long>(TgaShort(Header + 0xe));
                unsigned long const Pitch =
                    ((Bpp + 7) >> 3) * static_cast<unsigned long>(Width);
                unsigned long TargetFormat = 0;
                PixelFormatFind(&TargetFormat, 0x20, 8, 8, 8, 8);

                // the pixels follow the 0x12-byte header, the id field
                // and, for a colour-mapped image, 3 bytes per palette entry.
                unsigned char const* Pixels = nullptr;
                if ((Header[2] & 7) == 1)
                    Pixels = Header + Header[0] + 3 * (TgaPaletteEntries(Header) + 6);
                else
                    Pixels = Header + Header[0] + 0x12;

                FBigImageDesc ImageDesc;
                ImageDesc.Size = 0x2c;
                ImageDesc.Name = "BigImage";
                ImageDesc.Pipeline = NeuronRuntime::Pipeline;
                ImageDesc.ImageSource = nullptr;
                ImageDesc.SrcFormat = &TargetFormat; // +0x10 (see LoadBmp)
                ImageDesc.DstFormat = &ImageFormat;  // +0x14
                ImageDesc.Pixels = Pixels;
                ImageDesc.Width = static_cast<unsigned long>(Width);
                ImageDesc.Height = static_cast<unsigned long>(Height);
                ImageDesc.Pitch = Pitch;
                ImageDesc.DrawTile = &ScreenQuadDrawBigImageTile;

                Release();
                if (NeuronRuntime::NeuronComponents->CreateInstance(reinterpret_cast<AGGDESC*>(&ImageDesc),
                                                 reinterpret_cast<void**>(&BigImage)) == 0)
                {
                    bResult = true;
                }
                else
                {
                    FDUMP(0x100001, "%s(%d) : ERROR:General:%s", kIntroCpp, 0x1ff,
                          kBigImageFailed);
                }
            }
        }
        // Every exit: the routine, then the routine on the (already cleared) +0x10 holder.
        Mapped.Close();
    }
    // / 0x00565794..0x005657aa: release the sub filesystem.
    if (SubFs != nullptr)
        SubFs->Release();
    return bResult;
}

#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
namespace
{
    int G_ImageWidescreenAlign = 0;
}

void ImageSetWidescreenAlign(int Side)
{
    G_ImageWidescreenAlign = Side;
}
#endif

void Image::Draw()
{
    // .
    if (BigImage == nullptr)
        return;

    Win3DSetOrtho2D();

    // D3DRS_CULLMODE none, ZFUNC always, ZWRITEENABLE off, ZENABLE
    // off, SRCBLEND SRCALPHA, DESTBLEND INVSRCALPHA, ALPHABLENDENABLE on.
    FImageRenderStateSave CullMode(0x16, 1);
    FImageRenderStateSave ZFunc(0x17, 8);
    FImageRenderStateSave ZWrite(0xe, 0);
    FImageRenderStateSave ZEnable(7, 0);
    FImageRenderStateSave SrcBlend(0x13, 5);
    FImageRenderStateSave DestBlend(0x14, 6);
    FImageRenderStateSave AlphaBlend(0x1b, 1);

    // the two stage tables through SetTextureStageState (slot 62,
    // +0xf8); these are not restored.
    IRenderPipeline* Pipeline = NeuronRuntime::Pipeline;
    for (FTextureStageSetting const* Setting = kImageStage0; Setting->Type != 0; ++Setting)
        Pipeline->SetTextureStageState(0, Setting->Type, Setting->Value);
    Pipeline = NeuronRuntime::Pipeline;
    for (FTextureStageSetting const* Setting = kImageStage1; Setting->Type != 0; ++Setting)
        Pipeline->SetTextureStageState(1, Setting->Type, Setting->Value);

    // alpha 1, colour {1, 1, 1}, then Draw(null, {0, height, width, 0}).
    RECT Dest;
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
    // Port-only Hor+: the 4:3 picture keeps its own aspect instead of stretching -- centred,
    // or against the edge ImageSetWidescreenAlign asked for (the main menu logo).
    LONG const ImageWidth = static_cast<LONG>(WidescreenEquivalentSurfaceWidth() + 0.5);
    LONG const Spare = static_cast<LONG>(G_RenderSurfaceWidth) - ImageWidth;
    if (G_ImageWidescreenAlign < 0)
        Dest.left = 0;
    else if (G_ImageWidescreenAlign > 0)
        Dest.left = Spare;
    else
        Dest.left = Spare / 2;
    Dest.top = static_cast<LONG>(G_RenderSurfaceHeight);
    Dest.right = Dest.left + ImageWidth;
    Dest.bottom = 0;
#else
    Dest.left = 0;
    Dest.top = static_cast<LONG>(G_RenderSurfaceHeight);
    Dest.right = static_cast<LONG>(G_RenderSurfaceWidth);
    Dest.bottom = 0;
#endif
    float const Color[3] = {1.0f, 1.0f, 1.0f};
    BigImage->SetAlpha(1.0f); // +0x18
    BigImage->SetColor(Color); // +0x1c
    BigImage->Draw(nullptr, &Dest); // +0x10
    // the seven savers restore in reverse order on scope exit.
}

Image* CreateImage()
{
    // operator new(0x18) (MSVCRT, null on failure); the Image ctor only on
    // success, null otherwise.
    return new (std::nothrow) Image();
}
