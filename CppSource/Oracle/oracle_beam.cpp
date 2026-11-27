// Oracle suite of work package P5.6 (Oracle.exe neuron-client): the beam's geometry
// (beam_effect.cpp's BuildGeometry and its two builders) into the effect vertex batch
// (effect_vertex_batch.cpp), with the effect base (effect_base.cpp) and the type buckets
// (effect_type_list.cpp), all compiled in, over crates/neuron-client/tests/fixtures/oracle/beam.list.
// The Rust test crates/neuron-client/tests/oracle_vectors.rs (beam) renders the same lines from
// neuron-client::render::effects.
//
// Each input line is one case:
//   beam MODE HEAD TRAIL FLAGS ARCH STATE XFORM
// MODE, HEAD and TRAIL are the archetype's segment mode and texture modes (decimal), FLAGS the
// beam's +0x70 byte (hex), ARCH the archetype's 28 floats from +0x10 (tip_color on, eight hex
// digits each, comma-joined), STATE the beam's +0x78, +0x7c and +0x80, XFORM the local transform's
// rotation rows and translation. The beam watches nothing, so BuildGeometry builds from the local
// transform. The stand-ins the process already has answer GetGameOption(0x26) with 1 and
// IsFirstPersonCameraCurrent with false (oracle_ui_controls.cpp, oracle_audio_client.cpp).
//
// Output lines per case (floats as eight hex digits of the f32):
//   {"case","r","flags","nv","ni"}  BuildGeometry's answer, the flags after it, the counts
//   {"case","vtx","v"}              per vertex: x, y, z, the colour dword (B, G, R, A in memory,
//                                   printed as the little-endian dword), u, v
//   {"case","idx"}                  the indices, comma-joined
// The batch is reset before each case.

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sstream>
#include <string>
#include <vector>

#include "oracle_support.h"

#include "../OutpostFrontier/Client/effect_type_list.cpp"
#include "../OutpostFrontier/Client/effect_base.cpp"
#include "../OutpostFrontier/Client/effect_vertex_batch.cpp"
#include "../OutpostFrontier/Client/beam_effect.cpp"

// The two client globals the compiled files name and no case reaches: the effect shape database
// (texture_shapes.cpp; FShapeDB's members are oracle_ui_controls.cpp's stand-ins) and the FX mesh
// factory (fl_fx_mesh.cpp).
FShapeDB G_EffectShapeDB;
IFxMeshFactory* G_FLFxMeshFactory = nullptr;

using namespace oracle;

namespace
{
    std::vector<std::string> SplitOn(const std::string& Text, char Separator)
    {
        std::vector<std::string> Parts;
        std::string Part;
        std::istringstream Stream(Text);
        while (std::getline(Stream, Part, Separator))
            Parts.push_back(Part);
        return Parts;
    }

    bool HexFloats(const std::string& Text, float* Out, size_t Count)
    {
        const std::vector<std::string> Parts = SplitOn(Text, ',');
        if (Parts.size() != Count)
            return false;
        for (size_t Index = 0; Index < Count; ++Index)
        {
            const unsigned Bits = static_cast<unsigned>(strtoul(Parts[Index].c_str(), nullptr, 16));
            memcpy(&Out[Index], &Bits, sizeof(float));
        }
        return true;
    }

    std::string F(float Value)
    {
        return Format("%08x", FloatBits(Value));
    }
}

void RunBeamSuite()
{
    Suite Out("beam.jsonl");
    if (!OracleLoadMathEngine())
    {
        fprintf(stderr, "beam: no 3DMathEngine\n");
        GFailed = true;
        return;
    }
    int Case = 0;
    for (const std::string& Line : InputLines("beam.list"))
    {
        const std::vector<std::string> Words = SplitOn(Line, ' ');
        FBeamEffectArch Arch;
        memset(&Arch, 0, sizeof(Arch));
        float State[3];
        float Xform[12];
        if (Words.size() != 8 || Words[0] != "beam" || !HexFloats(Words[5], &Arch.ColorCoreR, 28) ||
            !HexFloats(Words[6], State, 3) || !HexFloats(Words[7], Xform, 12))
        {
            fprintf(stderr, "beam.list: bad line %s\n", Line.c_str());
            GFailed = true;
            continue;
        }
        Arch.SegmentMode = atoi(Words[1].c_str());
        Arch.UvModeV = atoi(Words[2].c_str());
        Arch.UvModeU = atoi(Words[3].c_str());

        FBeamEffect Beam(0x2cu);
        Beam.Arch = &Arch;
        Beam.Flags70 = static_cast<unsigned char>(strtoul(Words[4].c_str(), nullptr, 16));
        Beam.Progress78 = State[0];
        Beam.Float7C = State[1];
        Beam.ProgressDenom80 = State[2];
        memcpy(&Beam.LocalXform, Xform, sizeof(Xform));

        EffectVertexBatchReset(&G_EffectVertexBatch);
        const unsigned char Built = Beam.BuildGeometry(nullptr);
        FEffectVertexBatch const& Batch = G_EffectVertexBatch;
        const std::string Head = Format("{\"case\":\"%d\",", Case);
        Out.Line(Head + Format("\"r\":\"%d\",\"flags\":\"%d\",\"nv\":\"%d\",\"ni\":\"%d\"}", Built,
                               Beam.Flags70, Batch.VertCount, Batch.IndexCount));
        for (int Index = 0; Index < Batch.VertCount; ++Index)
        {
            FEffectVertex const& V = Batch.Vertices[Index];
            const unsigned Color = static_cast<unsigned>(V.B) | (static_cast<unsigned>(V.G) << 8) |
                                   (static_cast<unsigned>(V.R) << 16) |
                                   (static_cast<unsigned>(V.A) << 24);
            Out.Line(Head + Format("\"vtx\":\"%d\",\"v\":\"", Index) + F(V.X) + "," + F(V.Y) + "," +
                     F(V.Z) + "," + Format("%08x", Color) + "," + F(V.U) + "," + F(V.V) + "\"}");
        }
        std::string Indices;
        for (int Index = 0; Index < Batch.IndexCount; ++Index)
            Indices += (Index == 0 ? "" : ",") + Format("%u", Batch.Indices[Index]);
        Out.Line(Head + "\"idx\":\"" + Indices + "\"}");
        ++Case;
    }
}
