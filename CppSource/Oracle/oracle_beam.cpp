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
//
// RunBeamWalkSuite (beam_walk.jsonl) drives the beams' lifecycle over
// crates/neuron-client/tests/fixtures/oracle/beam_walk.list: one case per line, operations
// separated by " ; ":
//   cam VIEW ZNEAR PX PY        the camera stand-in's View and ViewPos, ZNear, PlaneX, PlaneY
//   type OFF NEAR FAR PRIO      row 0x2c of the type table: EXIST_OFFSCREEN, pbubble, priority
//   beam I MODE FLAGS ARCH STATE XFORM VEL PRIO
//                               a beam made as the client makes one (EffectAlloc(0x84) and the
//                               constructor), with the archetype, +0x70, +0x78..+0x80, the local
//                               transform, the velocity (+0x4c) and the priority; the case holds
//                               its first reference
//   link I | release I | kill I EffectTypeListLink, Release, and slot +0x04 after the link
//   walk DT                     the batch reset, then EffectTypeBucketWalk's body over bucket
//                               0x2c (space_scene_obj_draw.cpp, followed here line by line: the file
//                               is not compiled in)
// After each walk: {"case","walk","bucket"} (the beams in bucket order, by I), then per beam in
// that order {"case","walk","beam","v"} (+0x08, +0x70, +0x78, +0x7c, the translation, the count)
// and {"case","walk","nv","ni"}. G_BeamEffectTexture is set, the perf level is 1. At a case's
// end every bucket is killed and emptied (EffectTypeListKillAll).

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
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

namespace
{
    // EffectTypeBucketWalk (space_scene_obj_draw.cpp), its order and arithmetic as written there.
    void OracleBucketWalk(FSpaceCamera* Cam, float Dt, int TypeIndex)
    {
        void* Node = G_EffectTypeBuckets[TypeIndex].Head;
        while (Node != nullptr)
        {
            unsigned char* const N = static_cast<unsigned char*>(Node);
            void* const Next = *reinterpret_cast<void**>(N + 4);
            void* const Effect = *reinterpret_cast<void**>(N + 8);
            Node = Next;
            unsigned char* const E = static_cast<unsigned char*>(Effect);
            if ((*reinterpret_cast<unsigned char*>(E + 8) & 2u) != 0)
            {
                EffectTypeListUnlinkAndRelease(Effect);
                continue;
            }
            if (*reinterpret_cast<int*>(E + 0x40) == -1)
            {
                float* const Pos = reinterpret_cast<float*>(E + 0x34);
                float const* const Vel = reinterpret_cast<float const*>(E + 0x4c);
                float const StepZ = Dt * Vel[2];
                Pos[0] = static_cast<float>(static_cast<double>(Dt) * Vel[0] + Pos[0]);
                Pos[1] = static_cast<float>(static_cast<double>(Dt) * Vel[1] + Pos[1]);
                Pos[2] = StepZ + Pos[2];
            }
            FEffectBase* const TypedEffect = (FEffectBase*)Effect;
            unsigned char const UpdateOk = TypedEffect->UpdateVisibility(Dt, Cam);
            FEffectTypeInfo const* const Info =
                EffectTypeInfoByIndex(*reinterpret_cast<int*>(E + 0x5c));
            if (Info->VisibilityOffscreen == 0 && UpdateOk == 0)
            {
                EffectTypeListLink(Effect);
                EffectFreezeToWorld(static_cast<FEffectBase*>(Effect));
                *reinterpret_cast<unsigned char*>(E + 8) =
                    static_cast<unsigned char>(*reinterpret_cast<unsigned char*>(E + 8) | 2u);
            }
            else
            {
                unsigned char const DrawOk = TypedEffect->TickDraw(Dt, Cam);
                if (DrawOk != 0 && UpdateOk != 0)
                    TypedEffect->BuildGeometry(Cam);
            }
            if ((*reinterpret_cast<unsigned char*>(E + 8) & 2u) == 0)
            {
                if (TypedEffect->IsAlive() != 0)
                    continue;
            }
            EffectTypeListUnlinkAndRelease(Effect);
        }
    }
}

void RunBeamWalkSuite()
{
    Suite Out("beam_walk.jsonl");
    static int GTextureStandIn = 0;
    G_BeamEffectTexture = &GTextureStandIn;
    FSpaceCamera Cam;
    FEffectTypeInfo const SavedRow = *EffectTypeInfoByIndex(0x2c);
    int Case = 0;
    for (const std::string& Line : InputLines("beam_walk.list"))
    {
        std::vector<FBeamEffectArch> Arches(16);
        std::map<int, FBeamEffect*> Beams;
        int Walk = 0;
        const std::string Head = Format("{\"case\":\"%d\",", Case);
        for (const std::string& Op : SplitOn(Line, ';'))
        {
            std::vector<std::string> W;
            for (const std::string& Word : SplitOn(Op, ' '))
                if (!Word.empty())
                    W.push_back(Word);
            if (W.empty())
                continue;
            bool bOk = true;
            if (W[0] == "cam" && W.size() == 5)
            {
                float View[12];
                bOk = HexFloats(W[1], View, 12) && HexFloats(W[2], &Cam.ZNear, 1) &&
                      HexFloats(W[3], Cam.PlaneX, 3) && HexFloats(W[4], Cam.PlaneY, 3);
                memcpy(Cam.View, View, sizeof(View));
            }
            else if (W[0] == "type" && W.size() == 5)
            {
                FEffectTypeInfo* const Info = EffectTypeInfoByIndex(0x2c);
                Info->VisibilityOffscreen = static_cast<unsigned char>(atoi(W[1].c_str()));
                bOk = HexFloats(W[2], &Info->PbubbleNear, 1) && HexFloats(W[3], &Info->PbubbleFar, 1) &&
                      HexFloats(W[4], &Info->Priority, 1);
            }
            else if (W[0] == "beam" && W.size() == 9)
            {
                const int I = atoi(W[1].c_str());
                FBeamEffectArch& Arch = Arches[I];
                memset(&Arch, 0, sizeof(Arch));
                Arch.SegmentMode = atoi(W[2].c_str());
                float State[3];
                float Xform[12];
                float Vel[3];
                bOk = HexFloats(W[4], &Arch.ColorCoreR, 28) && HexFloats(W[5], State, 3) &&
                      HexFloats(W[6], Xform, 12) && HexFloats(W[7], Vel, 3);
                FBeamEffect* const Beam = static_cast<FBeamEffect*>(EffectAlloc(0x84));
                BeamEffectConstruct(Beam, 0x2cu);
                Beam->Arch = &Arch;
                Beam->Flags70 = static_cast<unsigned char>(strtoul(W[3].c_str(), nullptr, 16));
                Beam->Progress78 = State[0];
                Beam->Float7C = State[1];
                Beam->ProgressDenom80 = State[2];
                memcpy(&Beam->LocalXform, Xform, sizeof(Xform));
                memcpy(Beam->Pad4C, Vel, sizeof(Vel));
                bOk = bOk && HexFloats(W[8], &Beam->Priority0C, 1);
                // A block the effect heap hands out again names the new beam only.
                for (std::map<int, FBeamEffect*>::iterator It = Beams.begin(); It != Beams.end();)
                    It = It->second == Beam ? Beams.erase(It) : ++It;
                Beams[I] = Beam;
            }
            else if ((W[0] == "link" || W[0] == "release" || W[0] == "kill") && W.size() == 2)
            {
                FBeamEffect* const Beam = Beams[atoi(W[1].c_str())];
                if (W[0] == "link")
                    EffectTypeListLink(Beam);
                else if (W[0] == "release")
                    Beam->Release();
                else
                {
                    EffectTypeListLink(Beam);
                    Beam->Kill();
                }
            }
            else if (W[0] == "walk" && W.size() == 2)
            {
                float Dt = 0.0f;
                bOk = HexFloats(W[1], &Dt, 1);
                EffectVertexBatchReset(&G_EffectVertexBatch);
                OracleBucketWalk(&Cam, Dt, 0x2c);
                const std::string WalkHead = Head + Format("\"walk\":\"%d\",", Walk);
                std::vector<int> Order;
                for (void* Node = G_EffectTypeBuckets[0x2c].Head; Node != nullptr;
                     Node = *reinterpret_cast<void**>(static_cast<unsigned char*>(Node) + 4))
                {
                    void* const Effect = *reinterpret_cast<void**>(static_cast<unsigned char*>(Node) + 8);
                    for (std::map<int, FBeamEffect*>::const_iterator It = Beams.begin(); It != Beams.end(); ++It)
                        if (It->second == Effect)
                            Order.push_back(It->first);
                }
                std::string List;
                for (size_t Index = 0; Index < Order.size(); ++Index)
                    List += (Index == 0 ? "" : ",") + Format("%d", Order[Index]);
                Out.Line(WalkHead + "\"bucket\":\"" + List + "\"}");
                for (int I : Order)
                {
                    FBeamEffect const* const B = Beams[I];
                    Out.Line(WalkHead + Format("\"beam\":\"%d\",\"v\":\"%d,%d,", I, B->Flags08, B->Flags70) +
                             F(B->Progress78) + "," + F(B->Float7C) + "," + F(B->LocalXform.Translation.X) +
                             "," + F(B->LocalXform.Translation.Y) + "," + F(B->LocalXform.Translation.Z) +
                             Format(",%u\"}", B->RefCount));
                }
                Out.Line(WalkHead + Format("\"nv\":\"%d\",\"ni\":\"%d\"}", G_EffectVertexBatch.VertCount,
                                           G_EffectVertexBatch.IndexCount));
                ++Walk;
            }
            else
                bOk = false;
            if (!bOk)
            {
                fprintf(stderr, "beam_walk.list: bad operation '%s'\n", Op.c_str());
                GFailed = true;
            }
        }
        EffectTypeListKillAll();
        ++Case;
    }
    G_BeamEffectTexture = nullptr;
    *EffectTypeInfoByIndex(0x2c) = SavedRow;
}
