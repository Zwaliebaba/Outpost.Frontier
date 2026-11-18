// P4.13: the screen-quad batcher of the client UI toolkit, through the reference's own code.
// screen_quad_state.cpp (the material lists, ScreenQuadBeginBatchFrame, the record append,
// ScreenQuadFlush with its render states and indexed FVF 0x1C4 draws, the menu path's state
// cache, ScreenQuadClip1D) is compiled into this file over recording stand-ins for the
// pipeline, the vertex buffer manager, the index buffer and IRPDraw, which NeuronRuntime's
// globals point at while the suite runs. The client functions the file also references (the
// Win3D projection stack of the big-image tile path, the surface size, Common's math engine for
// the flares, which Common.dll does not export) are stand-ins here; no case reaches them. The
// Rust side is crates/neuron-client/tests/oracle_vectors.rs with
// crates/neuron-client/tests/support/ui.rs; the inputs come from
// crates/neuron-client/tests/fixtures/oracle/make_ui_fixtures.py.
//
// ui_quad_batch.list holds one case per line: operations separated by " ; ", their fields by
// single spaces, floats as f32 bits and keys, textures and colours in hex:
//   key K                                       ScreenQuadSetMaterialKey
//   frame TEX U0 V0 U1 V1 BLEND FIND            ScreenQuadBeginBatchFrame
//   rec X0 Y0 X1 Y1 DA DB U0 V0 U1 V1 COLOR     ScreenQuadAppendRecord
//   flush K                                     ScreenQuadFlush (ffffffff: the current list)
//   reset                                       ScreenQuadResetDrawBatchCache
//   menu                                        ApplyScreenQuadRenderStates
//   invalidate                                  ScreenQuadInvalidateStateCache
//   get S V                                     the value GetRenderState(S) answers from now
//   clip E0 E1 A0 A1 LO HI SPAN                 ScreenQuadClip1D
// Each case starts from an empty batcher (two key changes, the cache reset, the state cache
// invalidated) and a recorder whose render states read 0.
//
// Line formats, each prefixed with "case": the recorder's calls --
//   {"ev":"rs","s","v"}  {"ev":"get","s","v"}  {"ev":"tss","st","t","v"}  {"ev":"ps","id","v"}
//   {"ev":"tex","st","h"}  {"ev":"vlock","fvf","n"}  {"ev":"verts","data"}  {"ev":"ilock","n"}
//   {"ev":"idx","data"}  {"ev":"select","base"}  {"ev":"draw","prim","min","n","start","count"}
// (the vertex and index locks answer base vertex 0 and offset 0) -- and per clip
//   {"op":"clip","r","e0","e1","a0","a1"}.

#include "oracle_support.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "../OutpostFrontier/Client/screen_quad_state.cpp"

// The client's surface size (globals.cpp) and Win3D projection stack (win3d_immediate.cpp),
// referenced by the big-image tile path, and Common's math engine (common_csys.cpp),
// referenced by ScreenQuadEnqueueFlare; no case calls either.
I3DMathEngine* __cdecl Acquire3DMathEngine()
{
    return nullptr;
}
unsigned G_RenderSurfaceWidth = 800;
unsigned G_RenderSurfaceHeight = 600;
void Win3DSetOrtho2D()
{
}
void Win3DPushProjection()
{
}
void Win3DPopProjection()
{
}

namespace
{
    oracle::Suite* GQuadOut = nullptr;
    unsigned int GQuadCase = 0;

    void QuadEvent(const std::string& Fields)
    {
        GQuadOut->Line(oracle::Format("{\"case\":\"%u\",", GQuadCase) + Fields + "}");
    }

    class FOracleQuadPipeline final : public IRenderPipeline
    {
    public:
        std::map<int, int> States;

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Result) override
        {
            *Result = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }

        GENRESULT NEURONCOMPONENTS_API SetRenderState(D3DRENDERSTATETYPE State, int Value) override
        {
            States[(int)State] = Value;
            QuadEvent(oracle::Format("\"ev\":\"rs\",\"s\":\"%d\",\"v\":\"%08x\"", (int)State, (unsigned)Value));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetRenderState(D3DRENDERSTATETYPE State, int* Out) override
        {
            const std::map<int, int>::const_iterator Found = States.find((int)State);
            *Out = Found == States.end() ? 0 : Found->second;
            QuadEvent(oracle::Format("\"ev\":\"get\",\"s\":\"%d\",\"v\":\"%08x\"", (int)State, (unsigned)*Out));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageState(int Stage, int Type, int Value) override
        {
            QuadEvent(oracle::Format("\"ev\":\"tss\",\"st\":\"%d\",\"t\":\"%d\",\"v\":\"%08x\"", Stage, Type,
                                     (unsigned)Value));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageTexture(int Stage, void* Handle) override
        {
            QuadEvent(oracle::Format("\"ev\":\"tex\",\"st\":\"%d\",\"h\":\"%08lx\"", Stage,
                                     (unsigned long)(uintptr_t)Handle));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetPipelineState(int Id, int Value) override
        {
            QuadEvent(oracle::Format("\"ev\":\"ps\",\"id\":\"%d\",\"v\":\"%d\"", Id, Value));
            return GR_OK;
        }

        // Everything else the batcher never calls.
        GENRESULT NEURONCOMPONENTS_API QueryDeviceAbility(unsigned int Index, unsigned long* Result) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API CreateTexture(int Width, int Height, unsigned int* PixelFormat,
                                                     unsigned int Levels, unsigned int Flags,
                                                     void** OutHandle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DestroyTexture(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureFormat(void* Handle, int* OutFormat) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetTextureLevelData(void* Handle, int Level, int SrcW, int SrcH,
                                                           unsigned int Pitch, unsigned int* SrcFormat,
                                                           const void* Pixels, void* A,
                                                           void* Palette) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Startup(const char* SectionName) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ShutdownPipeline() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetPipelineState(int Id, int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDeviceInfo(void* Out520) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDisplayModeCount(unsigned int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDisplayModeEnum(
            unsigned int Index, void* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SelectMode(
            int* Mode, unsigned int* OutIndex) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CreateBuffers(
            HWND Hwnd, int* Mode, int* OutMode) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetModeBlob(
            void* OutAdapter, void* OutMode10Dwords) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DestroyGpuResources() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ClearBuffers(
            unsigned long Flags, const void* Rects) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SwapBuffers() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LockBuffer(void* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UnlockBuffer() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetBufferInterface(
            void* Out, void* Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDeviceStats(void* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetViewportRect(
            int X, int Y, int W, int H) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetViewportRect(
            int* OutX, int* OutY, int* OutW, int* OutH) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetDepthRange(float MinZ, float MaxZ) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDepthRange(
            float* OutMinZ, float* OutMaxZ) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetWindow(
            HWND Hwnd, int X, int Y, int W, int H) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetWindow(
            HWND* OutHwnd, int* OutLeft, int* OutRight, int* OutW, int* OutH) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetWorld(const float* M12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetWorld(float* Out12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetView(const float* M12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetView(float* Out12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetModelview(const float* M12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetModelview(float* Out12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetProjection(const float* M16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetProjection(float* Out16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetLookat(
            float EyeX, float EyeY, float EyeZ, float AtX, float AtY, float AtZ, float UpX, float UpY,
            float UpZ) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetOrtho(
            float Left, float Right, float Bottom, float Top, float NearPlane,
            float FarPlane) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetPerspective(
            float FovDeg, float Aspect, float NearPlane, float FarPlane) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetLight(
            void* Handle, const D3DLIGHT8* Light) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DestroyLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetLight(
            void* Handle, D3DLIGHT8* OutLight) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetLightEnable(
            void* Handle, int Enable) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetLightEnable(
            void* Handle, int* OutEnable) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetMaterial(
            const D3DMATERIAL8* Material) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetMaterial(D3DMATERIAL8* Material) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API IsTexture(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LockTexture(
            void* Handle, int Level, void* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UnlockTexture(void* Handle, int Level) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureDim(
            void* Handle, unsigned int* OutW, unsigned int* OutH,
            int* OutLevels) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureInterface(
            void* Handle, void* UnusedA, void* UnusedB) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API BlitTexture(
            void* DstHandle, int DstLevel, void* SrcHandle, int SrcLevel) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetRenderTarget(
            void* Handle, void* UnusedA, void* UnusedB) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetRenderTarget(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API BeginScene() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API EndScene() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ResetRenderStatesToDefaults() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageState(
            int Stage, int Type, int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageTransform(
            int Stage, const float* Matrix16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageTransform(
            int Stage, float* OutMatrix16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageTexture(
            int Stage, void** OutHandle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API VerifyState() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DrawPrimitive(
            unsigned long Prim, unsigned int Fvf, const void* Verts, unsigned int Count,
            void* Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitiveUp(
            unsigned long Prim, unsigned int Fvf, const void* Verts, unsigned int VertCount,
            const void* Indices, unsigned int IndexCount,
            unsigned long Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DrawPrimitiveVb(
            unsigned long Prim, void* Vb, unsigned int StartVertex, unsigned int Count,
            void* Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitiveVb(
            unsigned long Prim, void* Vb, unsigned int BaseVertex, unsigned int NumVerts,
            const void* Indices, unsigned int IndexCount,
            unsigned long Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API AddLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API RemoveLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UpdateLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetWorldN(
            int WorldIndex, const float* M12) override { return GR_GENERIC; }
    };

    class FOracleQuadBuffers final : public IVertexBufferManager, public IRPIndexBuffer, public IRPDraw
    {
    public:
        std::vector<unsigned char> Vertices;
        unsigned int VertexBytes = 0;
        std::vector<unsigned short> Indices;
        unsigned int IndexCount = 0;

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Result) override
        {
            *Result = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }

        // IVertexBufferManager
        GENRESULT NEURONCOMPONENTS_API VbMgrInit(void* A, void* B, void* C, void* D) override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API VbMgrReleaseAll() override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API LockVertexMemory(unsigned int Fvf, unsigned int Count,
                                                        FVertexMemoryLock* Out) override
        {
            VertexBytes = Count * 0x20u;
            Vertices.assign(VertexBytes, 0xcd);
            Out->Listing.VertexBuffer = nullptr;
            Out->Listing.Fvf = Fvf;
            Out->Listing.BaseVertex = 0;
            Out->Vertices = Vertices.data();
            QuadEvent(oracle::Format("\"ev\":\"vlock\",\"fvf\":\"%x\",\"n\":\"%u\"", Fvf, Count));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API UnlockVertexMemory(FVertexMemoryLock* Out) override
        {
            QuadEvent("\"ev\":\"verts\",\"data\":\"" + oracle::Hex(Vertices.data(), VertexBytes) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API UnsupportedManagerOperation() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CopyVertexData(void* Dest, unsigned int Fvf, const FRpVertexSrcDesc* Source,
                                                      unsigned int StartVertex, unsigned int Count) override
        {
            return GR_GENERIC;
        }

        // IRPIndexBuffer
        GENRESULT NEURONCOMPONENTS_API CreateIb(unsigned int Count, unsigned int** Out, unsigned int Flags) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API DestroyIb(void** Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ResizeIb(void* Handle, unsigned int Count) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CopyIndices(void* Handle, unsigned int* OutOffset,
                                                   const unsigned short* SrcData, unsigned int Count) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API LockIb(void* Handle, unsigned int* Offset, void** Out,
                                              unsigned int Count) override
        {
            IndexCount = Count;
            Indices.assign(Count, 0xcdcd);
            *Offset = 0;
            *Out = Indices.data();
            QuadEvent(oracle::Format("\"ev\":\"ilock\",\"n\":\"%u\"", Count));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API UnlockIb(void* Handle) override
        {
            QuadEvent("\"ev\":\"idx\",\"data\":\"" + oracle::Hex(Indices.data(), IndexCount * 2u) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SelectIb(void* Handle, unsigned int BaseVertexIndex, unsigned int Unused3,
                                                unsigned int Unused4) override
        {
            QuadEvent(oracle::Format("\"ev\":\"select\",\"base\":\"%u\"", BaseVertexIndex));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetIbCapacity(void* Handle, unsigned int* OutCapacity) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API IsIbValid(void* Handle) override { return GR_GENERIC; }

        // IRPDraw
        GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitive(unsigned long Prim, unsigned int MinIndex,
                                                            unsigned int NumVertices, unsigned int StartIndex,
                                                            unsigned int Count) override
        {
            QuadEvent(oracle::Format("\"ev\":\"draw\",\"prim\":\"%lu\",\"min\":\"%u\",\"n\":\"%u\",\"start\":\"%u\","
                                     "\"count\":\"%u\"",
                                     Prim, MinIndex, NumVertices, StartIndex, Count));
            return GR_OK;
        }
    };

    std::vector<std::string> SplitOps(const std::string& Text, const std::string& Separator)
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

    unsigned long Hex32(const std::string& Text)
    {
        return strtoul(Text.c_str(), nullptr, 16);
    }

    float F32(const std::string& Text)
    {
        return oracle::FloatFromBits((unsigned int)Hex32(Text));
    }

    // Two key changes forget the active list; the cache reset then empties the map.
    void ResetBatcher()
    {
        ScreenQuadSetMaterialKey(1);
        ScreenQuadSetMaterialKey(2);
        ScreenQuadResetDrawBatchCache();
    }

    void QuadBatchSuite()
    {
        oracle::Suite Out("ui_quad_batch.jsonl");
        GQuadOut = &Out;
        GQuadCase = 0;
        IRenderPipeline* const SavedPipeline = NeuronRuntime::Pipeline;
        IVertexBufferManager* const SavedVb = NeuronRuntime::VertexBuffer;
        IRPIndexBuffer* const SavedIb = NeuronRuntime::RPIndexBuffer;
        IRPDraw* const SavedDraw = NeuronRuntime::RPDraw;
        for (const std::string& Line : oracle::InputLines("ui_quad_batch.list"))
        {
            FOracleQuadPipeline Pipe;
            FOracleQuadBuffers Buffers;
            NeuronRuntime::Pipeline = &Pipe;
            NeuronRuntime::VertexBuffer = &Buffers;
            NeuronRuntime::RPIndexBuffer = &Buffers;
            NeuronRuntime::RPDraw = &Buffers;
            ResetBatcher();
            ScreenQuadInvalidateStateCache();

            for (const std::string& Op : SplitOps(Line, " ; "))
            {
                const std::vector<std::string> F = SplitOps(Op, " ");
                if (F[0] == "key")
                    ScreenQuadSetMaterialKey(Hex32(F[1]));
                else if (F[0] == "frame")
                {
                    const unsigned long Frame[5] = {Hex32(F[1]), Hex32(F[2]), Hex32(F[3]), Hex32(F[4]),
                                                    Hex32(F[5])};
                    ScreenQuadBeginBatchFrame(Frame, atoi(F[6].c_str()), atoi(F[7].c_str()) != 0);
                }
                else if (F[0] == "rec")
                {
                    FScreenQuadRecord Record;
                    Record.X0 = F32(F[1]);
                    Record.Y0 = F32(F[2]);
                    Record.X1 = F32(F[3]);
                    Record.Y1 = F32(F[4]);
                    Record.DepthA = F32(F[5]);
                    Record.DepthB = F32(F[6]);
                    Record.U0 = F32(F[7]);
                    Record.V0 = F32(F[8]);
                    Record.U1 = F32(F[9]);
                    Record.V1 = F32(F[10]);
                    Record.ColorBits = (unsigned)Hex32(F[11]);
                    ScreenQuadAppendRecord(Record);
                }
                else if (F[0] == "flush")
                    ScreenQuadFlush(&Pipe, Hex32(F[1]));
                else if (F[0] == "reset")
                    ScreenQuadResetDrawBatchCache();
                else if (F[0] == "menu")
                    ApplyScreenQuadRenderStates(&Pipe);
                else if (F[0] == "invalidate")
                    ScreenQuadInvalidateStateCache();
                else if (F[0] == "get")
                    Pipe.States[atoi(F[1].c_str())] = (int)Hex32(F[2]);
                else if (F[0] == "clip")
                {
                    float E0 = F32(F[1]);
                    float E1 = F32(F[2]);
                    float A0 = F32(F[3]);
                    float A1 = F32(F[4]);
                    const int R = ScreenQuadClip1D(&E0, &E1, &A0, &A1, F32(F[5]), F32(F[6]), F32(F[7]));
                    QuadEvent(oracle::Format("\"op\":\"clip\",\"r\":\"%d\",\"e0\":\"%08x\",\"e1\":\"%08x\","
                                             "\"a0\":\"%08x\",\"a1\":\"%08x\"",
                                             R, oracle::FloatBits(E0), oracle::FloatBits(E1),
                                             oracle::FloatBits(A0), oracle::FloatBits(A1)));
                }
            }
            ResetBatcher();
            ++GQuadCase;
        }
        NeuronRuntime::Pipeline = SavedPipeline;
        NeuronRuntime::VertexBuffer = SavedVb;
        NeuronRuntime::RPIndexBuffer = SavedIb;
        NeuronRuntime::RPDraw = SavedDraw;
        GQuadOut = nullptr;
    }
}

void RunUiQuadSuites()
{
    QuadBatchSuite();
}
