// P4.4: the large-image tiler of ximage.dll (FBigImage, IBigImage2), through the reference's own
// code. ximage.cpp is compiled into this file (its DllMain renamed), as oracle_audio.cpp does
// with SoundManager, so that FBigImage of its anonymous namespace can be made directly. The Rust
// side is crates/neuron-client/tests/oracle_vectors.rs with
// crates/neuron-client/tests/support/texture.rs; the inputs come from
// crates/neuron-client/tests/fixtures/oracle/make_texture_fixtures.py.
//
// big_image.list holds one case per line: operations separated by " ; ", their fields by single
// spaces. The first is
//   image SQUARE MAXW MAXH SRCFMT DSTFMT WIDTH HEIGHT PITCH LENGTH
// which makes an image from LENGTH bytes (byte i is i * 31 + 7) on the recording renderer of
// oracle_texture.h with those device abilities, and the texture library's built-in format maps
// for PickGpuFormat. When it fails, the case ends. Then:
//   draw L T R B DL DT DR DB   Draw (L "-" for no source rectangle; then only the destination)
//   alpha BITS                 SetAlpha (an f32 as eight hex digits)
//   color R G B                SetColor (three f32s)
//   fill FMT W H PITCH         SetFromBuffer from PITCH * H bytes (byte i is i * 17 + 3)
//   tile I                     GetTile
//   size                       GetSize
//   count                      GetTileCount
// At the end the image is released, which destroys its textures.
//
// Line formats, each prefixed with "case": renderer calls (oracle_texture.h); per draw callback
// {"tile","h","color","dest","uv"} (floats as f32 hex); per operation {"op","r"} with "v", "h"
// and "rect" where the operation has outputs.

#include "oracle_texture.h"

#include <stdlib.h>

#include <string>
#include <vector>

#define DllMain XimageDllMain
#include "../ximage/ximage.cpp"
#undef DllMain

using namespace oracle;

namespace
{
    Suite* GDrawOut = nullptr;
    unsigned int GDrawCase = 0;
    unsigned int GDrawTile = 0;

    void __cdecl RecordTile(void* TextureHandle, unsigned long PackedColor, float* DestXy, float* UvQuad)
    {
        std::string Uv;
        for (int Index = 0; Index < 8; ++Index)
            Uv += Format(Index == 0 ? "%08x" : ",%08x", FloatBits(UvQuad[Index]));
        GDrawOut->Line(Format("{\"case\":\"%u\",\"tile\":\"%u\",\"h\":\"%lu\",\"color\":\"%08lx\","
                              "\"dest\":\"%08x,%08x,%08x,%08x\",\"uv\":\"",
                              GDrawCase, GDrawTile, (unsigned long)(uintptr_t)TextureHandle, PackedColor,
                              FloatBits(DestXy[0]), FloatBits(DestXy[1]), FloatBits(DestXy[2]),
                              FloatBits(DestXy[3])) +
                       Uv + "\"}");
        ++GDrawTile;
    }

    std::vector<std::string> Split(const std::string& Text, const std::string& Separator)
    {
        std::vector<std::string> Parts;
        size_t Start = 0;
        for (;;)
        {
            const size_t End = Text.find(Separator, Start);
            if (End == std::string::npos)
            {
                Parts.push_back(Text.substr(Start));
                return Parts;
            }
            Parts.push_back(Text.substr(Start, End - Start));
            Start = End + Separator.size();
        }
    }

    long Int(const std::string& Text)
    {
        return strtol(Text.c_str(), nullptr, 10);
    }

    float Bits(const std::string& Text)
    {
        return FloatFromBits((unsigned int)strtoul(Text.c_str(), nullptr, 16));
    }

    std::vector<unsigned char> Pattern(size_t Length, unsigned int Step, unsigned int Start)
    {
        std::vector<unsigned char> Bytes(Length);
        for (size_t Index = 0; Index < Length; ++Index)
            Bytes[Index] = (unsigned char)((Index * Step + Start) & 0xFF);
        return Bytes;
    }

    void Line(Suite& Out, unsigned int Case, unsigned int Op, long Result, const std::string& Extra)
    {
        Out.Line(Format("{\"case\":\"%u\",\"op\":\"%u\",\"r\":\"%ld\"", Case, Op, Result) + Extra + "}");
    }

    void RunCase(Suite& Out, unsigned int Case, const std::vector<std::string>& Ops)
    {
        const std::vector<std::string> I = Split(Ops[0], " ");
        FOracleTexturePipeline Pipeline(&Out, Case, (unsigned long)Int(I[1]), (unsigned long)Int(I[2]),
                                        (unsigned long)Int(I[3]));
        Pipeline.Library = OracleDefaultTextureLibrary();
        unsigned long SrcFormat = (unsigned long)Int(I[4]);
        unsigned long DstFormat = (unsigned long)Int(I[5]);
        const std::vector<unsigned char> Pixels = Pattern((size_t)Int(I[9]), 31, 7);
        Pipeline.SetSource(Pixels.data(), Pixels.size());
        FBigImageDesc Desc;
        Desc.Size = kBigImageDescSize;
        Desc.Name = Render::Factory::BigImage;
        Desc.Pipeline = &Pipeline;
        Desc.ImageSource = nullptr;
        Desc.SrcFormat = &SrcFormat;
        Desc.DstFormat = &DstFormat;
        Desc.Pixels = Pixels.data();
        Desc.Width = (unsigned long)Int(I[6]);
        Desc.Height = (unsigned long)Int(I[7]);
        Desc.Pitch = (unsigned long)Int(I[8]);
        Desc.DrawTile = RecordTile;
        FBigImage* Image = new FBigImage();
        const GENRESULT Created = Image->Initialize(&Desc);
        if (Created != GR_OK)
        {
            // As the factory does: the half-made image destroys the textures it holds.
            delete Image;
            Line(Out, Case, 0, (long)Created, "");
            return;
        }
        Line(Out, Case, 0, (long)Created, "");
        GDrawOut = &Out;
        GDrawCase = Case;
        for (unsigned int Op = 1; Op < Ops.size(); ++Op)
        {
            const std::vector<std::string> F = Split(Ops[Op], " ");
            const std::string& K = F[0];
            if (K == "draw")
            {
                RECT Src;
                RECT Dest;
                size_t At = 1;
                const bool bSrc = F[1] != "-";
                if (bSrc)
                {
                    SetRect(&Src, (int)Int(F[1]), (int)Int(F[2]), (int)Int(F[3]), (int)Int(F[4]));
                    At = 5;
                }
                else
                    At = 2;
                SetRect(&Dest, (int)Int(F[At]), (int)Int(F[At + 1]), (int)Int(F[At + 2]), (int)Int(F[At + 3]));
                GDrawTile = 0;
                Line(Out, Case, Op, (long)Image->Draw(bSrc ? &Src : nullptr, &Dest), "");
            }
            else if (K == "alpha")
                Line(Out, Case, Op, (long)Image->SetAlpha(Bits(F[1])), "");
            else if (K == "color")
            {
                const float Rgb[3] = {Bits(F[1]), Bits(F[2]), Bits(F[3])};
                Image->SetColor(Rgb);
                Line(Out, Case, Op, 0, "");
            }
            else if (K == "fill")
            {
                unsigned long FillFormat = (unsigned long)Int(F[1]);
                const unsigned long W = (unsigned long)Int(F[2]);
                const unsigned long H = (unsigned long)Int(F[3]);
                const unsigned long Pitch = (unsigned long)Int(F[4]);
                const std::vector<unsigned char> Fill = Pattern((size_t)(Pitch * H), 17, 3);
                Pipeline.SetSource(Fill.data(), Fill.size());
                Line(Out, Case, Op, (long)Image->SetFromBuffer(Fill.data(), &FillFormat, W, H, Pitch), "");
                Pipeline.SetSource(Pixels.data(), Pixels.size());
            }
            else if (K == "tile")
            {
                void* Handle = (void*)(uintptr_t)0xCDCDCDCDu;
                RECT Rect;
                SetRect(&Rect, -7, -7, -7, -7);
                const GENRESULT Result = Image->GetTile((int)Int(F[1]), &Handle, &Rect);
                Line(Out, Case, Op, (long)Result,
                     Format(",\"h\":\"%lu\",\"rect\":\"%ld,%ld,%ld,%ld\"", (unsigned long)(uintptr_t)Handle,
                              (long)Rect.left, (long)Rect.top, (long)Rect.right, (long)Rect.bottom));
            }
            else if (K == "size")
            {
                unsigned long W = 0;
                unsigned long H = 0;
                const GENRESULT Result = Image->GetSize(&W, &H);
                Line(Out, Case, Op, (long)Result, Format(",\"v\":\"%lu,%lu\"", W, H));
            }
            else if (K == "count")
                Line(Out, Case, Op, (long)Image->GetTileCount(), "");
            else
            {
                fprintf(stderr, "big_image: unknown operation %s\n", K.c_str());
                GFailed = true;
            }
        }
        Image->Release();
    }
}

void BigImageSuite()
{
    Suite Out("big_image.jsonl");
    const std::vector<std::string> Lines = InputLines("big_image.list");
    for (unsigned int Case = 0; Case < Lines.size(); ++Case)
        RunCase(Out, Case, Split(Lines[Case], " ; "));
}
