// P4.13: the Win3D immediate-mode layer of the client UI toolkit, through the reference's own
// code. win3d_immediate.cpp (the OpenGL emulator: the accumulator, the colour and texture
// coordinate setters, the capability switches with their depth and blend caches, the
// projection cache with its push and pop, the viewport, Win3DEnd's draws, the sphere grid),
// win3d_renderables.cpp (the two line batchers and their flush, the overlay pass that flushes
// the font atlas's screen quads) and win3d_render.cpp (Win3DApplyCamera) are compiled into this
// file with every function and global they define renamed (OracleImm_*), since
// oracle_ui_controls.cpp defines recording stand-ins of the same names for the controls. They
// draw through a recording pipeline, vertex buffer, index buffer and IRPDraw; the screen-quad
// flush of the overlay pass is the real ScreenQuadFlush of oracle_ui_quads.cpp, on the
// recorders as well. The Win3D camera the flushes read is a camera the case sets. The button
// montage, the font atlas's key and ScaleVec3 (the fuzzy lines', which no case reaches) are
// stand-ins. The Rust side is crates/neuron-client/tests/oracle_vectors.rs with
// crates/neuron-client/tests/support/ui_immediate.rs; the inputs come from
// crates/neuron-client/tests/fixtures/oracle/make_ui_immediate_fixtures.py.
//
// ui_immediate.list holds one case per line: operations separated by " ; ", their fields by
// single spaces, floats as f32 bits in hex, handles, colours and capabilities in hex, other
// integers in decimal. Each case starts from the layer's static initial values (colour 0, the
// cache on, no projection), a surface of 800 x 600, both line batchers created and reset, an
// empty screen-quad batcher, and a recorder whose states read 0 and whose pipeline states do not
// answer:
//   surface W H              G_RenderSurfaceWidth / Height
//   reset                    Win3DResetState
//   begin P | end            Win3DBegin / Win3DEnd
//   v X Y Z                  Win3DVertex3f            tc U V      Win3DTexCoord2f
//   c4f R G B A | c3f R G B  Win3DColor4f / 3f        tex H       Win3DBindTexture
//   c3ub R G B | c4ub R G B A  Win3DColor3ubv / 4ubv (decimal bytes)
//   en C | dis C             Win3DEnable / Win3DDisable
//   linear | point           Win3DSetTextureFilterLinear / Point
//   zfunc F | blend S D | zwrite B   Win3DSetDepthFunc / SetBlendFunc / SetDepthWrite
//   vp | vpcam               Win3DSetViewport(null / the case camera)
//   vprect X Y W H           Win3DSetViewportRect     getvp Q     Win3DGetViewport
//   ortho L R B T N F | ortho2d | orthocam   Win3DSetProjection / SetOrtho2D / SetOrtho2DForCamera
//   persp F A N FAR | applycam  Win3DSetPerspective / Win3DApplyCamera(the case camera)
//   push | pop               Win3DPushProjection / PopProjection
//   cache B                  the projection cache's flag
//   cam FOVY ASPECT NEAR FAR VL VT VR VB V0..V11   the case camera (getters, view rect, view)
//   ps ID V | known H | get S V   the recorder's GetPipelineState / IsTexture / GetRenderState
//   clear R G B A            Win3DSetPipelineClearColor
//   sphere R COLS ROWS       Win3DBuildSpherePoints and Win3DDrawSphereGrid
//   line A|B X0 Y0 Z0 X1 Y1 Z1 COLOR EDGE W   ImmedBatchAddLine
//   flush | breset           Win3DImmedBatchFlushAll / Win3DImmedBatchReset
//   font X0 Y0 X1 Y1 COLOR TEX   a record of the font atlas's list (texture TEX, blend 2)
//   overlay                  UiOverlayPrepPipeline
//   state                    the layer's state
//
// Line formats, each prefixed with "case": the recorders' calls --
//   {"ev":"rs","s","v"} {"ev":"get","s","v"} {"ev":"tss","st","t","v"} {"ev":"gtss","st","t","v"}
//   {"ev":"ps","id","v"} {"ev":"gps","id","r"} ("-" when not answered) {"ev":"tex","st","h"}
//   {"ev":"istex","h","r"} {"ev":"mv","m"} {"ev":"world","m"} {"ev":"view","m"} {"ev":"ortho","a"}
//   {"ev":"persp","a"} {"ev":"gproj"} {"ev":"sproj","m"} {"ev":"vp","x","y","w","h"}
//   {"ev":"draw","prim","fvf","n","data"} {"ev":"drawi","prim","fvf","n","data","idx"}
//   {"ev":"lverts","fvf","n","data"} {"ev":"lidx","n","data"} {"ev":"selvb"} {"ev":"selib","base"}
//   {"ev":"ldraw","prim","min","n","start","count"} (a line batcher's flush)
//   {"ev":"qvlock","fvf","n"} {"ev":"qverts","data"} {"ev":"qilock","n"} {"ev":"qidx","data"}
//   {"ev":"qselect","base"} {"ev":"qdraw","prim","min","n","start","count"} (a screen-quad flush)
// (matrices and vertex data as the hex of their little-endian bytes; the buffer calls answer
// offset 0) -- and per getvp and state
//   {"ev":"getvp","q","v"} {"ev":"state","colour","nv","ni","kind","params","set","skip",
//    "depth","blend","vp","mv"}.

#include "oracle_support.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <set>
#include <string>
#include <vector>

// The layer's own names, and the client functions of other files it reaches that this suite
// stands in for, renamed for this file.
#define GWin3DModelview OracleImm_GWin3DModelview
#define Win3DResetState OracleImm_Win3DResetState
#define Win3DBegin OracleImm_Win3DBegin
#define Win3DVertex3f OracleImm_Win3DVertex3f
#define Win3DColor4f OracleImm_Win3DColor4f
#define Win3DColor3f OracleImm_Win3DColor3f
#define Win3DSetPipelineClearColor OracleImm_Win3DSetPipelineClearColor
#define Win3DColor3ub OracleImm_Win3DColor3ub
#define Win3DColor3ubv OracleImm_Win3DColor3ubv
#define Win3DColor4ub OracleImm_Win3DColor4ub
#define Win3DColor4ubv OracleImm_Win3DColor4ubv
#define Win3DTexCoord2f OracleImm_Win3DTexCoord2f
#define Win3DBindTexture OracleImm_Win3DBindTexture
#define Win3DEnable OracleImm_Win3DEnable
#define Win3DDisable OracleImm_Win3DDisable
#define Win3DSetTextureFilterLinear OracleImm_Win3DSetTextureFilterLinear
#define Win3DSetTextureFilterPoint OracleImm_Win3DSetTextureFilterPoint
#define Win3DSetDepthFunc OracleImm_Win3DSetDepthFunc
#define Win3DSetBlendFunc OracleImm_Win3DSetBlendFunc
#define Win3DSetDepthWrite OracleImm_Win3DSetDepthWrite
#define Win3DSetViewport OracleImm_Win3DSetViewport
#define Win3DGetViewport OracleImm_Win3DGetViewport
#define Win3DSetViewportRect OracleImm_Win3DSetViewportRect
#define Win3DSetOrtho2DForCamera OracleImm_Win3DSetOrtho2DForCamera
#define Win3DSetProjection OracleImm_Win3DSetProjection
#define Win3DSetOrtho2D OracleImm_Win3DSetOrtho2D
#define Win3DSetPerspective OracleImm_Win3DSetPerspective
#define Win3DApplyCameraPerspective OracleImm_Win3DApplyCameraPerspective
#define Win3DPushProjection OracleImm_Win3DPushProjection
#define Win3DPopProjection OracleImm_Win3DPopProjection
#define Win3DGetProjectionSetCount OracleImm_Win3DGetProjectionSetCount
#define Win3DGetProjectionSkipCount OracleImm_Win3DGetProjectionSkipCount
#define Win3DOnPresentFrame OracleImm_Win3DOnPresentFrame
#define Win3DEnd OracleImm_Win3DEnd
#define Win3DGetVertexCount OracleImm_Win3DGetVertexCount
#define Win3DGetIndexCount OracleImm_Win3DGetIndexCount
#define Win3DGetCurrentColour OracleImm_Win3DGetCurrentColour
#define Win3DGetVertex OracleImm_Win3DGetVertex
#define Win3DBuildSpherePoints OracleImm_Win3DBuildSpherePoints
#define Win3DDrawSphereGrid OracleImm_Win3DDrawSphereGrid
#define Win3DDrawWireSphere OracleImm_Win3DDrawWireSphere
#define Win3DVec3Add OracleImm_Win3DVec3Add
#define Win3DVec3Sub OracleImm_Win3DVec3Sub
#define Win3DDrawFuzzyLine OracleImm_Win3DDrawFuzzyLine
#define Win3DDrawFuzzyArc OracleImm_Win3DDrawFuzzyArc
#define G_DrawCallCount OracleImm_G_DrawCallCount
#define G_ImmedBatchA OracleImm_G_ImmedBatchA
#define G_ImmedBatchB OracleImm_G_ImmedBatchB
#define Win3DImmedBatchReset OracleImm_Win3DImmedBatchReset
#define Win3DImmedBatchFlushAll OracleImm_Win3DImmedBatchFlushAll
#define ImmedBatchAddLine OracleImm_ImmedBatchAddLine
#define Win3DImmedBatchCreateAll OracleImm_Win3DImmedBatchCreateAll
#define Win3DImmedBatchDestroyAll OracleImm_Win3DImmedBatchDestroyAll
#define Win3DImmedDrawLine OracleImm_Win3DImmedDrawLine
#define ButtonMontageEnsure OracleImm_ButtonMontageEnsure
#define ButtonMontageFlushBatch OracleImm_ButtonMontageFlushBatch
#define UiOverlayPrepPipeline OracleImm_UiOverlayPrepPipeline
#define Win3DFlushBeforeOverlayDraw OracleImm_Win3DFlushBeforeOverlayDraw
#define ClearWin3DBuffers OracleImm_ClearWin3DBuffers
#define ClearWin3DBuffersToColor OracleImm_ClearWin3DBuffersToColor
#define Win3DApplyCamera OracleImm_Win3DApplyCamera
#define Win3DGetCamera OracleImm_Win3DGetCamera
#define FontAtlasMaterialKey OracleImm_FontAtlasMaterialKey
#define G_bButtonMontageLoaded OracleImm_G_bButtonMontageLoaded
#define LoadButtonMontage OracleImm_LoadButtonMontage
#define NnMontageFlushButtons OracleImm_NnMontageFlushButtons
#define ScaleVec3 OracleImm_ScaleVec3

#include "../OutpostFrontier/Client/win3d_immediate.cpp"
#include "../OutpostFrontier/Client/win3d_renderables.cpp"
#include "../OutpostFrontier/Client/win3d_render.cpp"

#include "../OutpostFrontier/Client/font_atlas.h"
#include "../OutpostFrontier/Client/nn_montage_draw.h"
#include "../OutpostFrontier/Client/screen_quad_state.h"

namespace
{
    // The Win3D camera the batch flushes read (Win3DGetCamera), the case's.
    FSpaceCamera GImmCamera;
}

FSpaceCamera* Win3DGetCamera()
{
    return &GImmCamera;
}

unsigned FontAtlasMaterialKey()
{
    return NEURONCOMPONENTS_CRC::GetCRC32("font");
}

bool G_bButtonMontageLoaded = true;

void LoadButtonMontage()
{
}

void NnMontageFlushButtons()
{
}

void ScaleVec3(float* Out, float const* V, float Scalar)
{
    Out[0] = V[0] * Scalar;
    Out[1] = V[1] * Scalar;
    Out[2] = V[2] * Scalar;
}

namespace
{
    oracle::Suite* GImmOut = nullptr;
    unsigned int GImmCase = 0;

    void ImmEvent(const std::string& Fields)
    {
        GImmOut->Line(oracle::Format("{\"case\":\"%u\",", GImmCase) + Fields + "}");
    }

    std::string Words(const float* Values, size_t Count)
    {
        return oracle::Hex(Values, Count * sizeof(float));
    }

    class FOracleImmPipeline;

    // The vertex buffer, index buffer and draw interfaces: the line batchers' (queried from the
    // pipeline) and the screen-quad flush's (NeuronRuntime's globals), told apart by their
    // event names.
    class FOracleImmBuffers final : public IRPVertexBuffer,
                                    public IRPIndexBuffer,
                                    public IRPDraw,
                                    public IVertexBufferManager
    {
    public:
        std::vector<unsigned char> Vertices;
        unsigned int VertexBytes = 0;
        std::vector<unsigned short> Indices;
        unsigned int IndexCount = 0;
        // The screen-quad flush's object (its events are named q*), else the line batchers'.
        bool bQuads = false;

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Result) override
        {
            *Result = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }

        // IRPVertexBuffer: the line batchers.
        GENRESULT NEURONCOMPONENTS_API CreateVb(unsigned int Fvf, unsigned int Count, unsigned int** Out,
                                                unsigned int Flags) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DestroyVb(void** Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ResizeVb(void* Handle, unsigned int Fvf, unsigned int Count) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API CopyVertices(void* Handle, unsigned int* OutOffset,
                                                    const FRpVertexSrcDesc* SrcData, unsigned int StartVertex,
                                                    unsigned int Count) override
        {
            *OutOffset = 0;
            ImmEvent(oracle::Format("\"ev\":\"lverts\",\"fvf\":\"%x\",\"n\":\"%u\",", SrcData->SrcFvf, Count) +
                     "\"data\":\"" + oracle::Hex(SrcData->Position.Base, Count * SrcData->Position.Stride) +
                     "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API LockVb(void* Handle, unsigned int* Offset, void** Out,
                                              unsigned int Count) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UnlockVb(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UnsupportedVertexBufferOperation(void* Handle) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SelectVb(void* Handle) override
        {
            ImmEvent("\"ev\":\"selvb\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetVbInfo(void* Handle, unsigned int* OutFvf, unsigned int* OutCount) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API IsVbValid(void* Handle) override { return GR_GENERIC; }

        // IRPIndexBuffer: CopyIndices for the line batchers, LockIb for the screen quads.
        GENRESULT NEURONCOMPONENTS_API CreateIb(unsigned int Count, unsigned int** Out, unsigned int Flags) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API DestroyIb(void** Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ResizeIb(void* Handle, unsigned int Count) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CopyIndices(void* Handle, unsigned int* OutOffset,
                                                   const unsigned short* SrcData, unsigned int Count) override
        {
            *OutOffset = 0;
            ImmEvent(oracle::Format("\"ev\":\"lidx\",\"n\":\"%u\",", Count) + "\"data\":\"" +
                     oracle::Hex(SrcData, Count * 2u) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API LockIb(void* Handle, unsigned int* Offset, void** Out,
                                              unsigned int Count) override
        {
            IndexCount = Count;
            Indices.assign(Count, 0xcdcd);
            *Offset = 0;
            *Out = Indices.data();
            ImmEvent(oracle::Format("\"ev\":\"qilock\",\"n\":\"%u\"", Count));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API UnlockIb(void* Handle) override
        {
            ImmEvent("\"ev\":\"qidx\",\"data\":\"" + oracle::Hex(Indices.data(), IndexCount * 2u) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SelectIb(void* Handle, unsigned int BaseVertexIndex, unsigned int Unused3,
                                                unsigned int Unused4) override
        {
            ImmEvent(oracle::Format("\"ev\":\"%s\",\"base\":\"%u\"", bQuads ? "qselect" : "selib",
                                    BaseVertexIndex));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetIbCapacity(void* Handle, unsigned int* OutCapacity) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API IsIbValid(void* Handle) override { return GR_GENERIC; }

        // IRPDraw: the line batchers draw through their queried pointer, the screen quads through
        // NeuronRuntime::RPDraw.
        GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitive(unsigned long Prim, unsigned int MinIndex,
                                                            unsigned int NumVertices, unsigned int StartIndex,
                                                            unsigned int Count) override
        {
            ImmEvent(oracle::Format("\"ev\":\"%s\",\"prim\":\"%lu\",\"min\":\"%u\",\"n\":\"%u\",\"start\":\"%u\","
                                    "\"count\":\"%u\"",
                                    bQuads ? "qdraw" : "ldraw", Prim, MinIndex, NumVertices, StartIndex, Count));
            return GR_OK;
        }

        // IVertexBufferManager: the screen quads.
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
            ImmEvent(oracle::Format("\"ev\":\"qvlock\",\"fvf\":\"%x\",\"n\":\"%u\"", Fvf, Count));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API UnlockVertexMemory(FVertexMemoryLock* Out) override
        {
            ImmEvent("\"ev\":\"qverts\",\"data\":\"" + oracle::Hex(Vertices.data(), VertexBytes) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API UnsupportedManagerOperation() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CopyVertexData(void* Dest, unsigned int Fvf, const FRpVertexSrcDesc* Source,
                                                      unsigned int StartVertex, unsigned int Count) override
        {
            return GR_GENERIC;
        }
    };

    class FOracleImmPipeline final : public IRenderPipeline
    {
    public:
        std::map<int, int> States;
        std::map<std::pair<int, int>, int> StageStates;
        std::map<int, int> PipelineStates;
        std::set<uintptr_t> Known;
        float Projection[16] = {};
        FOracleImmBuffers* LineBuffers = nullptr;
        FOracleImmBuffers* LineDraw = nullptr;

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Result) override
        {
            *Result = nullptr;
            if (strcmp(Iid, Iids::RPVertexBuffer) == 0)
                *Result = static_cast<IRPVertexBuffer*>(LineBuffers);
            else if (strcmp(Iid, Iids::RPIndexBuffer) == 0)
                *Result = static_cast<IRPIndexBuffer*>(LineBuffers);
            else if (strcmp(Iid, Iids::RPDraw) == 0)
                *Result = static_cast<IRPDraw*>(LineDraw);
            return *Result != nullptr ? GR_OK : GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }

        GENRESULT NEURONCOMPONENTS_API SetRenderState(D3DRENDERSTATETYPE State, int Value) override
        {
            States[(int)State] = Value;
            ImmEvent(oracle::Format("\"ev\":\"rs\",\"s\":\"%d\",\"v\":\"%08x\"", (int)State, (unsigned)Value));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetRenderState(D3DRENDERSTATETYPE State, int* Out) override
        {
            const std::map<int, int>::const_iterator Found = States.find((int)State);
            *Out = Found == States.end() ? 0 : Found->second;
            ImmEvent(oracle::Format("\"ev\":\"get\",\"s\":\"%d\",\"v\":\"%08x\"", (int)State, (unsigned)*Out));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageState(int Stage, int Type, int Value) override
        {
            StageStates[std::make_pair(Stage, Type)] = Value;
            ImmEvent(oracle::Format("\"ev\":\"tss\",\"st\":\"%d\",\"t\":\"%d\",\"v\":\"%08x\"", Stage, Type,
                                    (unsigned)Value));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageState(int Stage, int Type, int* Out) override
        {
            const std::map<std::pair<int, int>, int>::const_iterator Found =
                StageStates.find(std::make_pair(Stage, Type));
            *Out = Found == StageStates.end() ? 0 : Found->second;
            ImmEvent(oracle::Format("\"ev\":\"gtss\",\"st\":\"%d\",\"t\":\"%d\",\"v\":\"%08x\"", Stage, Type,
                                    (unsigned)*Out));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageTexture(int Stage, void* Handle) override
        {
            ImmEvent(oracle::Format("\"ev\":\"tex\",\"st\":\"%d\",\"h\":\"%08lx\"", Stage,
                                    (unsigned long)(uintptr_t)Handle));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetPipelineState(int Id, int Value) override
        {
            ImmEvent(oracle::Format("\"ev\":\"ps\",\"id\":\"%d\",\"v\":\"%d\"", Id, Value));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetPipelineState(int Id, int* Out) override
        {
            const std::map<int, int>::const_iterator Found = PipelineStates.find(Id);
            if (Found == PipelineStates.end())
            {
                ImmEvent(oracle::Format("\"ev\":\"gps\",\"id\":\"%d\",\"r\":\"-\"", Id));
                return GR_GENERIC;
            }
            *Out = Found->second;
            ImmEvent(oracle::Format("\"ev\":\"gps\",\"id\":\"%d\",\"r\":\"%d\"", Id, *Out));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API IsTexture(void* Handle) override
        {
            const bool bKnown = Known.count((uintptr_t)Handle) != 0;
            ImmEvent(oracle::Format("\"ev\":\"istex\",\"h\":\"%08lx\",\"r\":\"%d\"",
                                    (unsigned long)(uintptr_t)Handle, bKnown ? 1 : 0));
            return bKnown ? GR_OK : GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetWorld(const float* M12) override
        {
            ImmEvent("\"ev\":\"world\",\"m\":\"" + Words(M12, 12) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetView(const float* M12) override
        {
            ImmEvent("\"ev\":\"view\",\"m\":\"" + Words(M12, 12) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetModelview(const float* M12) override
        {
            ImmEvent("\"ev\":\"mv\",\"m\":\"" + Words(M12, 12) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetProjection(const float* M16) override
        {
            memcpy(Projection, M16, sizeof(Projection));
            ImmEvent("\"ev\":\"sproj\",\"m\":\"" + Words(M16, 16) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetProjection(float* Out16) override
        {
            memcpy(Out16, Projection, sizeof(Projection));
            ImmEvent("\"ev\":\"gproj\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetOrtho(float Left, float Right, float Bottom, float Top, float NearPlane,
                                                float FarPlane) override
        {
            const float Args[6] = {Left, Right, Bottom, Top, NearPlane, FarPlane};
            ImmEvent("\"ev\":\"ortho\",\"a\":\"" + Words(Args, 6) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetPerspective(float FovDeg, float Aspect, float NearPlane,
                                                      float FarPlane) override
        {
            const float Args[4] = {FovDeg, Aspect, NearPlane, FarPlane};
            ImmEvent("\"ev\":\"persp\",\"a\":\"" + Words(Args, 4) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetViewportRect(int X, int Y, int W, int H) override
        {
            ImmEvent(oracle::Format("\"ev\":\"vp\",\"x\":\"%d\",\"y\":\"%d\",\"w\":\"%d\",\"h\":\"%d\"", X, Y, W, H));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API DrawPrimitive(unsigned long Prim, unsigned int Fvf, const void* Verts,
                                                     unsigned int Count, void* Unused) override
        {
            ImmEvent(oracle::Format("\"ev\":\"draw\",\"prim\":\"%lu\",\"fvf\":\"%x\",\"n\":\"%u\",", Prim, Fvf, Count) +
                     "\"data\":\"" + oracle::Hex(Verts, Count * 0x18u) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitiveUp(unsigned long Prim, unsigned int Fvf,
                                                              const void* Verts, unsigned int VertCount,
                                                              const void* Indices, unsigned int IndexCount,
                                                              unsigned long Unused) override
        {
            ImmEvent(oracle::Format("\"ev\":\"drawi\",\"prim\":\"%lu\",\"fvf\":\"%x\",\"n\":\"%u\",", Prim, Fvf,
                                    VertCount) +
                     "\"data\":\"" + oracle::Hex(Verts, VertCount * 0x18u) + "\",\"idx\":\"" +
                     oracle::Hex(Indices, IndexCount * 2u) + "\"");
            return GR_OK;
        }

        // Everything else the layer never calls.
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
        GENRESULT NEURONCOMPONENTS_API GetViewportRect(
            int* OutX, int* OutY, int* OutW, int* OutH) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetDepthRange(float MinZ, float MaxZ) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDepthRange(
            float* OutMinZ, float* OutMaxZ) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetWindow(
            HWND Hwnd, int X, int Y, int W, int H) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetWindow(
            HWND* OutHwnd, int* OutLeft, int* OutRight, int* OutW, int* OutH) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetWorld(float* Out12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetView(float* Out12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetModelview(float* Out12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetLookat(
            float EyeX, float EyeY, float EyeZ, float AtX, float AtY, float AtZ, float UpX, float UpY,
            float UpZ) override { return GR_GENERIC; }
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
        GENRESULT NEURONCOMPONENTS_API SetTextureStageTransform(
            int Stage, const float* Matrix16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageTransform(
            int Stage, float* OutMatrix16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageTexture(
            int Stage, void** OutHandle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API VerifyState() override { return GR_GENERIC; }
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

    std::vector<std::string> SplitImm(const std::string& Text, const std::string& Separator)
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

    unsigned long ImmHex(const std::string& Text)
    {
        return strtoul(Text.c_str(), nullptr, 16);
    }

    float ImmF(const std::string& Text)
    {
        return oracle::FloatFromBits((unsigned int)ImmHex(Text));
    }

    int ImmInt(const std::string& Text)
    {
        return atoi(Text.c_str());
    }

    // The layer's globals back to their static initial values (Win3DResetState and what it
    // leaves: the colour, the texture coordinate, the projection cache and its counters, the
    // saved projection and the viewport).
    void ResetLayer()
    {
        Win3DResetState();
        G_CurrentColour = 0;
        G_CurrentU = 0.0f;
        G_CurrentV = 0.0f;
        G_bProjectionCacheEnabled = 1;
        G_ProjectionSetCount = 0;
        G_ProjectionSkipCount = 0;
        for (int I = 0; I < kProjectionParamCount; ++I)
        {
            G_ProjectionParams[I] = 0.0f;
            G_SavedProjectionParams[I] = 0.0f;
        }
        memset(G_SavedProjectionMatrix, 0, sizeof(G_SavedProjectionMatrix));
        G_ViewportLeft = 0;
        G_ViewportFlippedTop = 0;
        G_ViewportWidth = 0;
        G_ViewportHeight = 0;
    }

    // Two key changes forget the active list; the cache reset then empties the map.
    void ResetQuads()
    {
        ScreenQuadSetMaterialKey(1);
        ScreenQuadSetMaterialKey(2);
        ScreenQuadResetDrawBatchCache();
        ScreenQuadInvalidateStateCache();
    }

    void PrintState()
    {
        const int Depth[3] = {G_bDepthTest, G_DepthWrite, G_DepthFunc};
        ImmEvent(oracle::Format("\"ev\":\"state\",\"colour\":\"%08lx\",\"nv\":\"%d\",\"ni\":\"%d\",\"kind\":\"%d\",",
                                G_CurrentColour, G_VertexCount, G_IndexCount, G_ProjectionKind) +
                 "\"params\":\"" + Words(G_ProjectionParams, 6) + "\"," +
                 oracle::Format("\"set\":\"%d\",\"skip\":\"%d\",\"depth\":\"%d %d %d\",\"blend\":\"%d %d %d\","
                                "\"vp\":\"%d %d %d %d\",",
                                G_ProjectionSetCount, G_ProjectionSkipCount, Depth[0], Depth[1], Depth[2],
                                G_AlphaBlendEnable, G_SrcBlend, G_DestBlend, G_ViewportLeft, G_ViewportFlippedTop,
                                G_ViewportWidth, G_ViewportHeight) +
                 "\"mv\":\"" + Words(GWin3DModelview, 12) + "\"");
    }

    void ImmediateSuite()
    {
        oracle::Suite Out("ui_immediate.jsonl");
        GImmOut = &Out;
        GImmCase = 0;
        IRenderPipeline* const SavedPipeline = NeuronRuntime::Pipeline;
        IVertexBufferManager* const SavedVb = NeuronRuntime::VertexBuffer;
        IRPIndexBuffer* const SavedIb = NeuronRuntime::RPIndexBuffer;
        IRPDraw* const SavedDraw = NeuronRuntime::RPDraw;
        const unsigned SavedWidth = G_RenderSurfaceWidth;
        const unsigned SavedHeight = G_RenderSurfaceHeight;
        for (const std::string& Line : oracle::InputLines("ui_immediate.list"))
        {
            FOracleImmPipeline Pipe;
            FOracleImmBuffers LineBuffers;
            FOracleImmBuffers QuadBuffers;
            QuadBuffers.bQuads = true;
            Pipe.LineBuffers = &LineBuffers;
            Pipe.LineDraw = &LineBuffers;
            NeuronRuntime::Pipeline = &Pipe;
            NeuronRuntime::VertexBuffer = &QuadBuffers;
            NeuronRuntime::RPIndexBuffer = &QuadBuffers;
            NeuronRuntime::RPDraw = &QuadBuffers;
            G_RenderSurfaceWidth = 800;
            G_RenderSurfaceHeight = 600;
            GImmCamera = FSpaceCamera();
            ResetLayer();
            ResetQuads();
            G_ImmedBatchA.Dirty = 1;
            G_ImmedBatchB.Dirty = 1;
            Win3DImmedBatchCreateAll();
            Win3DImmedBatchReset();

            for (const std::string& Op : SplitImm(Line, " ; "))
            {
                const std::vector<std::string> F = SplitImm(Op, " ");
                const std::string& Name = F[0];
                if (Name == "surface")
                {
                    G_RenderSurfaceWidth = (unsigned)ImmInt(F[1]);
                    G_RenderSurfaceHeight = (unsigned)ImmInt(F[2]);
                }
                else if (Name == "reset")
                    Win3DResetState();
                else if (Name == "begin")
                    Win3DBegin(ImmInt(F[1]));
                else if (Name == "end")
                    Win3DEnd();
                else if (Name == "v")
                    Win3DVertex3f(ImmF(F[1]), ImmF(F[2]), ImmF(F[3]));
                else if (Name == "c4f")
                    Win3DColor4f(ImmF(F[1]), ImmF(F[2]), ImmF(F[3]), ImmF(F[4]));
                else if (Name == "c3f")
                    Win3DColor3f(ImmF(F[1]), ImmF(F[2]), ImmF(F[3]));
                else if (Name == "c3ub")
                {
                    const unsigned char Rgb[3] = {(unsigned char)ImmInt(F[1]), (unsigned char)ImmInt(F[2]),
                                                  (unsigned char)ImmInt(F[3])};
                    Win3DColor3ubv(Rgb);
                }
                else if (Name == "c4ub")
                {
                    const unsigned char Rgba[4] = {(unsigned char)ImmInt(F[1]), (unsigned char)ImmInt(F[2]),
                                                   (unsigned char)ImmInt(F[3]), (unsigned char)ImmInt(F[4])};
                    Win3DColor4ubv(Rgba);
                }
                else if (Name == "tc")
                    Win3DTexCoord2f(ImmF(F[1]), ImmF(F[2]));
                else if (Name == "tex")
                    Win3DBindTexture((void*)(uintptr_t)ImmHex(F[1]));
                else if (Name == "en")
                    Win3DEnable((int)ImmHex(F[1]));
                else if (Name == "dis")
                    Win3DDisable((int)ImmHex(F[1]));
                else if (Name == "linear")
                    Win3DSetTextureFilterLinear(nullptr);
                else if (Name == "point")
                    Win3DSetTextureFilterPoint();
                else if (Name == "zfunc")
                    Win3DSetDepthFunc(ImmInt(F[1]));
                else if (Name == "blend")
                    Win3DSetBlendFunc(ImmInt(F[1]), ImmInt(F[2]));
                else if (Name == "zwrite")
                    Win3DSetDepthWrite(ImmInt(F[1]));
                else if (Name == "vp")
                    Win3DSetViewport(nullptr, "oracle", 0);
                else if (Name == "vpcam")
                    Win3DSetViewport(AsCamera(&GImmCamera), "oracle", 0);
                else if (Name == "vprect")
                    Win3DSetViewportRect(ImmInt(F[1]), ImmInt(F[2]), ImmInt(F[3]), ImmInt(F[4]), "oracle", 0);
                else if (Name == "getvp")
                {
                    int Out4[4] = {-1, -1, -1, -1};
                    const int Query = (int)ImmHex(F[1]);
                    Win3DGetViewport(Query, Out4);
                    ImmEvent(oracle::Format("\"ev\":\"getvp\",\"q\":\"%x\",\"v\":\"%d %d %d %d\"", Query, Out4[0],
                                            Out4[1], Out4[2], Out4[3]));
                }
                else if (Name == "ortho")
                    Win3DSetProjection(ImmF(F[1]), ImmF(F[2]), ImmF(F[3]), ImmF(F[4]), ImmF(F[5]), ImmF(F[6]));
                else if (Name == "ortho2d")
                    Win3DSetOrtho2D();
                else if (Name == "orthocam")
                    Win3DSetOrtho2DForCamera(&GImmCamera);
                else if (Name == "persp")
                    Win3DSetPerspective(ImmF(F[1]), ImmF(F[2]), ImmF(F[3]), ImmF(F[4]));
                else if (Name == "applycam")
                    Win3DApplyCamera(AsCamera(&GImmCamera));
                else if (Name == "push")
                    Win3DPushProjection();
                else if (Name == "pop")
                    Win3DPopProjection();
                else if (Name == "cache")
                    G_bProjectionCacheEnabled = (unsigned char)ImmInt(F[1]);
                else if (Name == "cam")
                {
                    GImmCamera.FovYDeg = ImmF(F[1]);
                    GImmCamera.Aspect = ImmF(F[2]);
                    GImmCamera.ZNear = ImmF(F[3]);
                    GImmCamera.ZFar = ImmF(F[4]);
                    for (int I = 0; I < 4; ++I)
                        GImmCamera.Viewport[I] = ImmInt(F[5 + I]);
                    for (int I = 0; I < 12; ++I)
                    {
                        const float Value = ImmF(F[9 + I]);
                        if (I < 9)
                            GImmCamera.View[I] = Value;
                        else
                            GImmCamera.ViewPos[I - 9] = Value;
                    }
                }
                else if (Name == "ps")
                    Pipe.PipelineStates[ImmInt(F[1])] = ImmInt(F[2]);
                else if (Name == "known")
                    Pipe.Known.insert((uintptr_t)ImmHex(F[1]));
                else if (Name == "get")
                    Pipe.States[ImmInt(F[1])] = (int)ImmHex(F[2]);
                else if (Name == "clear")
                    Win3DSetPipelineClearColor(ImmF(F[1]), ImmF(F[2]), ImmF(F[3]), ImmF(F[4]));
                else if (Name == "sphere")
                {
                    const int Cols = ImmInt(F[2]);
                    const int Rows = ImmInt(F[3]);
                    std::vector<float> Points((size_t)(Cols * Rows * 3), 0.0f);
                    Win3DBuildSpherePoints(Points.data(), ImmF(F[1]), Cols, Rows);
                    Win3DDrawSphereGrid(Points.data(), Cols, Rows);
                }
                else if (Name == "line")
                {
                    const float A[3] = {ImmF(F[2]), ImmF(F[3]), ImmF(F[4])};
                    const float B[3] = {ImmF(F[5]), ImmF(F[6]), ImmF(F[7])};
                    ImmedBatchAddLine(F[1] == "A" ? &G_ImmedBatchA : &G_ImmedBatchB, A, B, ImmHex(F[8]),
                                      ImmHex(F[9]), ImmF(F[10]));
                }
                else if (Name == "flush")
                    Win3DImmedBatchFlushAll();
                else if (Name == "breset")
                    Win3DImmedBatchReset();
                else if (Name == "font")
                {
                    ScreenQuadSetMaterialKey(FontAtlasMaterialKey());
                    const unsigned long Frame[5] = {ImmHex(F[6]), 0, 0, 0, 0};
                    ScreenQuadBeginBatchFrame(Frame, 2, false);
                    FScreenQuadRecord Record;
                    Record.X0 = ImmF(F[1]);
                    Record.Y0 = ImmF(F[2]);
                    Record.X1 = ImmF(F[3]);
                    Record.Y1 = ImmF(F[4]);
                    Record.DepthA = 0.0f;
                    Record.DepthB = 1.0f;
                    Record.U0 = 0.0f;
                    Record.V0 = 0.0f;
                    Record.U1 = 1.0f;
                    Record.V1 = 1.0f;
                    Record.ColorBits = (unsigned)ImmHex(F[5]);
                    ScreenQuadAppendRecord(Record);
                }
                else if (Name == "overlay")
                    UiOverlayPrepPipeline();
                else if (Name == "state")
                    PrintState();
            }
            Win3DImmedBatchDestroyAll();
            ResetQuads();
            ++GImmCase;
        }
        NeuronRuntime::Pipeline = SavedPipeline;
        NeuronRuntime::VertexBuffer = SavedVb;
        NeuronRuntime::RPIndexBuffer = SavedIb;
        NeuronRuntime::RPDraw = SavedDraw;
        G_RenderSurfaceWidth = SavedWidth;
        G_RenderSurfaceHeight = SavedHeight;
        GImmOut = nullptr;
    }
}

void RunUiImmediateSuites()
{
    ImmediateSuite();
}
