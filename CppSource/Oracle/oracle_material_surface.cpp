// P4.5 (third increment): the 19 surface materials of surfacematerials.dll (FMaterialImpl,
// MaterialImpl.cpp), through the reference's own code, against recording services. The Rust side
// is crates/neuron-client/tests/support/material_surface.rs with the same contract; the input is
// crates/neuron-client/tests/fixtures/oracle/material_surface.list from make_material_fixtures.py.
//
// A material is made as FMaterialFactory::CreateInstance makes it once the name matched
// (FlmCreateMaterial, ConfigureAggregation, InitializeFromDescriptor) and initialized with a
// component that answers the texture library, the renderer, IRPDraw, the vertex-buffer manager
// and a material library (the real FMaterialLibrary, whose global records are the process's).
// The calls of oracle_material_kinds.cpp keep its line formats (its header lists them); this
// suite adds:
//
//   GetMaterial               "getmat","r","m": the block of the case's last SetMaterial, r -1
//                             and m "-" (nothing written) before one
//   GetTextureStageState      "gettss","st","t","r","v": the value the case last set on that
//                             stage and state, r -1 and v "-" (nothing written) without one
//   DrawPrimitiveVb           "dpvb","prim","vb","start","n","r": the "dpvb" value (0 at first)
//   GetTextureFrame           "tframe","name","f","h","r": the entry's handle, u0 v0 u1 v1 = 0.25
//                             0 1 0.5, r 0
//   the engine (G_FlmEngineOuter, set with "engine 1"):
//     GetInstanceUserData     "ud","inst","v": the five words of a "ud" entry, "-" (null)
//     GetInstanceRoot         "root","inst","r": a "root" entry, else the instance
//     GetInstanceRelative     "rel","inst"
//     QueryInstance           "qinst","inst","iid": "wire" or "mesh","r": 0 for a "wire" or
//                             "mesh" entry of the instance, else -1
//     IVMeshWire::Draw        "wdraw","inst"
//     IMesh::OpenFaceGroup    "mopen","inst","g","m","r": the next of the "mopen" queue, else 0
//     IMesh::CloseFaceGroup   "mclose","inst"
//     OpenFaceGroupChannels   "mchan","inst","f","r": the entry's channel result
//     LockFaceGroupChannels   "mlock","inst","m"   UnlockFaceGroupChannels  "munlock","inst"
//     CloseFaceGroupChannels  "mchanclose","inst"
//     GetFaceGroupData        "mdata","inst","ch": the values (0x100) or positions (0x80)
//     GetFaceGroupIndices     "midx","inst","ch","n": their indices; n the value-index count
//                             (0x100) or the entry's count (0x80)
//   FlmLookupIdValue          "ival","id","r": 0 for an "ival" (a word) or "iname" (a string)
//                             entry, else -1
//   the candidate (the context's ReplacedMaterial, "cand" configures it):
//     QueryInterface(IID_IBatchedMaterial)  "cqi","r": 0 for a batched candidate, else -1
//     Bind, Teardown          "cbind"/"cteardown","face": "b" (batched) or "m","r"
//     Submit                  "csubmit","face","prim","start","count","ibs","span","flags": hex,"r"
//     GetPassCount            "cpasses","v": "-" writes nothing
//     ClassName               "cclass","name": hex
//     GetParameterDescriptor  "cdesc","i","r","tag": hex, "-" for a null tag
//     GetParameterValue       "cparam","i","v": the word written to the first payload
//
// and every operation writes {"case","op",...} after the lines of its calls. Floats are the
// eight hex digits of the f32. FLAppearance.cpp is not compiled: G_FlmEngineOuter,
// FlmLookupIdValue, FlmCreateMathEngine and FlmCreateAppearanceInstance are defined here.

#include "oracle_support.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "../Shading/MaterialLibrary.h"
// surfacematerials.h repeats ShadingInternal.h's registration token under the same name.
#define kRegisterPriority kSurfaceRegisterPriority
#include "../surfacematerials/surfacematerials.h"
#undef kRegisterPriority
#include "engine/sceneruntime.h"
#include "render/mesh_iface.h"
#include "x86math.h"

using namespace oracle;

namespace
{
    std::vector<std::string> SplitText(const std::string& Text, const std::string& Separator)
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

    unsigned long HexWord(const std::string& Text)
    {
        return strtoul(Text.c_str(), nullptr, 16);
    }

    long Dec(const std::string& Text)
    {
        return strtol(Text.c_str(), nullptr, 10);
    }

    std::vector<unsigned long> HexWords(const std::string& Text)
    {
        std::vector<unsigned long> Out;
        if (Text == "-")
            return Out;
        const std::vector<std::string> Parts = SplitText(Text, ",");
        for (size_t Index = 0; Index < Parts.size(); ++Index)
            Out.push_back(HexWord(Parts[Index]));
        return Out;
    }

    std::string Words(const void* Data, unsigned int Count)
    {
        const unsigned long* W = (const unsigned long*)Data;
        std::string Out;
        for (unsigned int Index = 0; Index < Count; ++Index)
            Out += Format(Index == 0 ? "%08lx" : ",%08lx", W[Index]);
        return Out;
    }

    std::string TextHex(const std::string& Text)
    {
        return Hex(Text.data(), Text.size());
    }

    struct FSurfTexture
    {
        std::string Name;
        unsigned long Crc;
        int Frames;
        unsigned long Handle;
        int Format;
        bool bPlayFail;
    };

    struct FSurfAnim
    {
        std::string Name;
        unsigned long Crc;
        bool bFills;
    };

    struct FSurfSource
    {
        std::vector<unsigned long> Pos;
        std::vector<unsigned long> Normal;
        std::vector<unsigned long> Diffuse;
        std::vector<unsigned long> Tex0;
        std::vector<unsigned long> Index;
        FRpVertexSrcDesc Desc;
    };

    struct FSurfContext
    {
        FVbListing Vb;
        FIbScratch Ib;
        FMaterialSubmitContext Context;
    };

    struct FSurfMesh
    {
        int ChannelResult = 0;
        std::vector<unsigned long> Values;
        std::vector<unsigned short> ValueIndices;
        std::vector<unsigned long> Positions;
        std::vector<unsigned short> PositionIndices;
        unsigned long Count = 0;
    };

    struct FSurfCandidateConfig
    {
        bool bBatched = false;
        bool bPasses = false;
        int Passes = 0;
        std::string Class;
        int DescResult = 0;
        bool bTag = false;
        std::string Tag;
        unsigned long Param = 0;
        int BindResult = 0;
        int TeardownResult = 0;
        int SubmitResult = 0;
    };

    class FSurfAnimInstance;

    // The recording state of a case.
    struct FSurfState
    {
        Suite* Out = nullptr;
        unsigned int Case = 0;
        std::map<int, unsigned long> RenderStates;
        std::map<int, unsigned long> StageStates;
        std::vector<unsigned long> LastMaterial;
        std::vector<int> VerifyQueue;
        bool bModeNone = false;
        int Mode = 1;
        int RpDrawResult = 0;
        int DvbResult = 0;
        int DpvbResult = 0;
        bool bLockFail = false;
        unsigned long NextBase = 50;
        bool bAnimLibrary = false;
        std::vector<FSurfTexture> Textures;
        std::vector<FSurfAnim> Anims;
        std::map<int, size_t> Playbacks;
        int NextPlayback = 1;
        int NextAnim = 1;
        std::vector<FSurfAnimInstance*> Instances;
        std::map<unsigned long, std::vector<unsigned long>> UserData;
        std::map<unsigned long, unsigned long> Roots;
        std::map<int, unsigned long> Values;
        std::map<int, std::string> Names;
        std::map<unsigned long, bool> Wires;
        std::map<unsigned long, FSurfMesh> Meshes;
        std::vector<int> OpenQueue;
        FSurfCandidateConfig Candidate;

        void Event(const std::string& Fields)
        {
            Out->Line(Format("{\"case\":\"%u\",", Case) + Fields + "}");
        }

        const FSurfTexture* TextureByHandle(unsigned long Handle) const
        {
            for (size_t Index = 0; Index < Textures.size(); ++Index)
            {
                if (Textures[Index].Handle == Handle)
                    return &Textures[Index];
            }
            return nullptr;
        }
    };

    FSurfState GSurf;

    class FSurfAnimInstance final : public IImaInstance
    {
    public:
        int Id = 0;
        bool bFills = false;

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override
        {
            GSurf.Event(Format("\"ev\":\"animrel\",\"id\":\"%d\"", Id));
            return 0;
        }
        FMaKeyBlock* NEURONCOMPONENTS_API GetKeyBlock() override { return nullptr; }
        void NEURONCOMPONENTS_API ResetTime() override {}
        void NEURONCOMPONENTS_API Tick(float Dt) override {}
        unsigned char NEURONCOMPONENTS_API FillUvMatrix(float* Out) override
        {
            GSurf.Event(Format("\"ev\":\"uv\",\"id\":\"%d\",\"r\":\"%d\"", Id, bFills ? 1 : 0));
            if (!bFills)
                return 0;
            memset(Out, 0, 16 * sizeof(float));
            Out[0] = 1.0f;
            Out[5] = 1.0f;
            Out[10] = 1.0f;
            Out[15] = 1.0f;
            Out[8] = 0.25f;
            Out[9] = -0.5f;
            return 1;
        }
    };

    class FSurfAnimLibrary final : public IMatAnimLibrary
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        void* NEURONCOMPONENTS_API LoadAnimationGroup(void* FileSystem) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API UnloadAnimationGroup(void* Arg) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CreateAnimationInstance(int Unused, void* Query,
                                                               IImaInstance** OutInstance) override
        {
            const char* Name = *(const char* const*)Query;
            const unsigned long Crc = ((const unsigned long*)Query)[1];
            const std::string NameHex = Name == nullptr ? std::string("-") : HexC(Name);
            for (size_t Index = 0; Index < GSurf.Anims.size(); ++Index)
            {
                if (GSurf.Anims[Index].Crc != Crc)
                    continue;
                FSurfAnimInstance* Instance = new FSurfAnimInstance();
                Instance->Id = GSurf.NextAnim++;
                Instance->bFills = GSurf.Anims[Index].bFills;
                GSurf.Instances.push_back(Instance);
                *OutInstance = Instance;
                GSurf.Event(Format("\"ev\":\"anim\",\"name\":\"%s\",\"crc\":\"%08lx\",\"id\":\"%d\",\"r\":\"0\"",
                                   NameHex.c_str(), Crc, Instance->Id));
                return GR_OK;
            }
            GSurf.Event(Format("\"ev\":\"anim\",\"name\":\"%s\",\"crc\":\"%08lx\",\"id\":\"-\",\"r\":\"-1\"",
                               NameHex.c_str(), Crc));
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API TickAnimationInstances(int Filter, float Dt) override { return GR_GENERIC; }
    };

    FSurfAnimLibrary GSurfAnimLibrary;

    class FSurfPipeline final : public IRenderPipeline
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            if (strcmp(Iid, Iids::MatAnimLibrary) == 0)
            {
                GSurf.Event(Format("\"ev\":\"qianim\",\"r\":\"%d\"", GSurf.bAnimLibrary ? 0 : -1));
                if (GSurf.bAnimLibrary)
                {
                    *Out = (IMatAnimLibrary*)&GSurfAnimLibrary;
                    return GR_OK;
                }
            }
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }

        GENRESULT NEURONCOMPONENTS_API SetMaterial(const D3DMATERIAL8* Material) override
        {
            GSurf.Event("\"ev\":\"mat\",\"m\":\"" + Words(Material, 17) + "\"");
            GSurf.LastMaterial.assign((const unsigned long*)Material, (const unsigned long*)Material + 17);
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetRenderState(D3DRENDERSTATETYPE State, int Value) override
        {
            GSurf.RenderStates[(int)State] = (unsigned long)Value;
            GSurf.Event(Format("\"ev\":\"rs\",\"s\":\"%u\",\"v\":\"%lu\"", (unsigned int)State,
                               (unsigned long)Value));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetRenderState(D3DRENDERSTATETYPE State, int* Out) override
        {
            const std::map<int, unsigned long>::const_iterator Found = GSurf.RenderStates.find((int)State);
            const unsigned long Value = Found == GSurf.RenderStates.end() ? 0 : Found->second;
            *Out = (int)Value;
            GSurf.Event(Format("\"ev\":\"getrs\",\"s\":\"%u\",\"v\":\"%lu\"", (unsigned int)State, Value));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageState(int Stage, int Type, int Value) override
        {
            GSurf.StageStates[Stage * 256 + Type] = (unsigned long)Value;
            GSurf.Event(Format("\"ev\":\"tss\",\"st\":\"%d\",\"t\":\"%d\",\"v\":\"%lu\"", Stage, Type,
                               (unsigned long)Value));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageTexture(int Stage, void* Handle) override
        {
            GSurf.Event(Format("\"ev\":\"tex\",\"st\":\"%d\",\"h\":\"%lu\"", Stage,
                               (unsigned long)(uintptr_t)Handle));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageTransform(int Stage, const float* Matrix16) override
        {
            GSurf.Event(Format("\"ev\":\"txf\",\"st\":\"%d\",\"m\":\"", Stage) + Words(Matrix16, 16) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetTextureFormat(void* Handle, int* OutFormat) override
        {
            const unsigned long Id = (unsigned long)(uintptr_t)Handle;
            const FSurfTexture* Texture = GSurf.TextureByHandle(Id);
            if (Texture == nullptr)
            {
                GSurf.Event(Format("\"ev\":\"fmt\",\"h\":\"%lu\",\"r\":\"-1\",\"f\":\"-\"", Id));
                return GR_GENERIC;
            }
            *OutFormat = Texture->Format;
            GSurf.Event(Format("\"ev\":\"fmt\",\"h\":\"%lu\",\"r\":\"0\",\"f\":\"%d\"", Id, Texture->Format));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API VerifyState() override
        {
            int Result = 0;
            if (!GSurf.VerifyQueue.empty())
            {
                Result = GSurf.VerifyQueue.front();
                GSurf.VerifyQueue.erase(GSurf.VerifyQueue.begin());
            }
            GSurf.Event(Format("\"ev\":\"verify\",\"r\":\"%d\"", Result));
            return Result;
        }
        GENRESULT NEURONCOMPONENTS_API GetPipelineState(int Id, int* Out) override
        {
            if (GSurf.bModeNone)
            {
                GSurf.Event(Format("\"ev\":\"state\",\"id\":\"%d\",\"v\":\"-\"", Id));
                return GR_OK;
            }
            *Out = GSurf.Mode;
            GSurf.Event(Format("\"ev\":\"state\",\"id\":\"%d\",\"v\":\"%d\"", Id, GSurf.Mode));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetWorld(const float* M12) override
        {
            GSurf.Event("\"ev\":\"world\",\"m\":\"" + Words(M12, 12) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetModelview(const float* M12) override
        {
            GSurf.Event("\"ev\":\"modelview\",\"m\":\"" + Words(M12, 12) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitiveVb(unsigned long Prim, void* Vb, unsigned int BaseVertex,
                                                              unsigned int NumVerts, const void* Indices,
                                                              unsigned int IndexCount, unsigned long Unused) override
        {
            std::string List;
            const unsigned short* Index = (const unsigned short*)Indices;
            for (unsigned int At = 0; At < IndexCount; ++At)
                List += Format(At == 0 ? "%u" : ",%u", (unsigned int)Index[At]);
            GSurf.Event(Format("\"ev\":\"dvb\",\"prim\":\"%lu\",\"vb\":\"%lu\",\"base\":\"%u\",\"n\":\"%u\",\"idx\":\"",
                               Prim, (unsigned long)(uintptr_t)Vb, BaseVertex, NumVerts) +
                        List + Format("\",\"r\":\"%d\"", GSurf.DvbResult));
            return GSurf.DvbResult;
        }

        GENRESULT NEURONCOMPONENTS_API SetView(const float* M12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Startup(const char* SectionName) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ShutdownPipeline() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetPipelineState(int Id, int Value) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDeviceInfo(void* Out520) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API QueryDeviceAbility(unsigned int Index, unsigned long* Out) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API GetDisplayModeCount(unsigned int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDisplayModeEnum(unsigned int Index, void* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SelectMode(int* Mode, unsigned int* OutIndex) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CreateBuffers(HWND Hwnd, int* Mode, int* OutMode) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetModeBlob(void* OutAdapter, void* OutMode10Dwords) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API DestroyGpuResources() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ClearBuffers(unsigned long Flags, const void* Rects) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SwapBuffers() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LockBuffer(void* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UnlockBuffer() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetBufferInterface(void* Out, void* Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDeviceStats(void* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetViewportRect(int X, int Y, int W, int H) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetViewportRect(int* OutX, int* OutY, int* OutW, int* OutH) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetDepthRange(float MinZ, float MaxZ) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDepthRange(float* OutMinZ, float* OutMaxZ) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetWindow(HWND Hwnd, int X, int Y, int W, int H) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API GetWindow(HWND* OutHwnd, int* OutLeft, int* OutRight, int* OutW,
                                                 int* OutH) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API GetWorld(float* Out12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetView(float* Out12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetModelview(float* Out12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetProjection(const float* M16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetProjection(float* Out16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetLookat(float EyeX, float EyeY, float EyeZ, float AtX, float AtY, float AtZ,
                                                 float UpX, float UpY, float UpZ) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetOrtho(float Left, float Right, float Bottom, float Top, float NearPlane,
                                                float FarPlane) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetPerspective(float FovDeg, float Aspect, float NearPlane,
                                                      float FarPlane) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetLight(void* Handle, const D3DLIGHT8* Light) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DestroyLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetLight(void* Handle, D3DLIGHT8* OutLight) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetLightEnable(void* Handle, int Enable) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetLightEnable(void* Handle, int* OutEnable) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetMaterial(D3DMATERIAL8* Material) override
        {
            if (GSurf.LastMaterial.empty())
            {
                GSurf.Event("\"ev\":\"getmat\",\"r\":\"-1\",\"m\":\"-\"");
                return GR_GENERIC;
            }
            memcpy(Material, GSurf.LastMaterial.data(), 17 * sizeof(unsigned long));
            GSurf.Event("\"ev\":\"getmat\",\"r\":\"0\",\"m\":\"" + Words(Material, 17) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API CreateTexture(int Width, int Height, unsigned int* Format, unsigned int Levels,
                                                     unsigned int Flags, void** OutHandle) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API DestroyTexture(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API IsTexture(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LockTexture(void* Handle, int Level, void* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UnlockTexture(void* Handle, int Level) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureDim(void* Handle, unsigned int* OutW, unsigned int* OutH,
                                                     int* OutLevels) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API GetTextureInterface(void* Handle, void* UnusedA, void* UnusedB) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetTextureLevelData(void* Handle, int Level, int SrcW, int SrcH,
                                                           unsigned int Pitch, unsigned int* SrcFormat,
                                                           const void* Pixels, void* A, void* B) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API BlitTexture(void* DstHandle, int DstLevel, void* SrcHandle,
                                                   int SrcLevel) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetRenderTarget(void* Handle, void* UnusedA, void* UnusedB) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API GetRenderTarget(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API BeginScene() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API EndScene() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ResetRenderStatesToDefaults() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageState(int Stage, int Type, int* Out) override
        {
            const std::map<int, unsigned long>::const_iterator Found = GSurf.StageStates.find(Stage * 256 + Type);
            if (Found == GSurf.StageStates.end())
            {
                GSurf.Event(Format("\"ev\":\"gettss\",\"st\":\"%d\",\"t\":\"%d\",\"r\":\"-1\",\"v\":\"-\"",
                                   Stage, Type));
                return GR_GENERIC;
            }
            *Out = (int)Found->second;
            GSurf.Event(Format("\"ev\":\"gettss\",\"st\":\"%d\",\"t\":\"%d\",\"r\":\"0\",\"v\":\"%lu\"", Stage,
                               Type, Found->second));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageTransform(int Stage, float* OutMatrix16) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageTexture(int Stage, void** OutHandle) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API DrawPrimitive(unsigned long Prim, unsigned int Fvf, const void* Verts,
                                                     unsigned int Count, void* Unused) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitiveUp(unsigned long Prim, unsigned int Fvf, const void* Verts,
                                                              unsigned int VertCount, const void* Indices,
                                                              unsigned int IndexCount, unsigned long Unused) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API DrawPrimitiveVb(unsigned long Prim, void* Vb, unsigned int StartVertex,
                                                       unsigned int Count, void* Unused) override
        {
            GSurf.Event(Format("\"ev\":\"dpvb\",\"prim\":\"%lu\",\"vb\":\"%lu\",\"start\":\"%u\",\"n\":\"%u\","
                               "\"r\":\"%d\"",
                               Prim, (unsigned long)(uintptr_t)Vb, StartVertex, Count, GSurf.DpvbResult));
            return GSurf.DpvbResult;
        }
        GENRESULT NEURONCOMPONENTS_API AddLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API RemoveLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UpdateLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetWorldN(int WorldIndex, const float* M12) override { return GR_GENERIC; }
    };

    class FSurfDraw final : public IRPDraw
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitive(unsigned long Prim, unsigned int MinIndex,
                                                            unsigned int NumVertices, unsigned int StartIndex,
                                                            unsigned int Count) override
        {
            GSurf.Event(Format("\"ev\":\"rpdraw\",\"prim\":\"%lu\",\"min\":\"%u\",\"n\":\"%u\",\"start\":\"%u\","
                               "\"count\":\"%u\",\"r\":\"%d\"",
                               Prim, MinIndex, NumVertices, StartIndex, Count, GSurf.RpDrawResult));
            return GSurf.RpDrawResult;
        }
    };

    class FSurfVbMgr final : public IVertexBufferManager
    {
    public:
        std::vector<unsigned char> Memory;

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API VbMgrInit(void* A, void* B, void* C, void* D) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API VbMgrReleaseAll() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LockVertexMemory(unsigned int Fvf, unsigned int Count,
                                                        FVertexMemoryLock* Out) override
        {
            if (GSurf.bLockFail)
            {
                GSurf.Event(Format("\"ev\":\"lock\",\"fvf\":\"%u\",\"n\":\"%u\",\"r\":\"-1\"", Fvf, Count));
                return GR_GENERIC;
            }
            GSurf.Event(Format("\"ev\":\"lock\",\"fvf\":\"%u\",\"n\":\"%u\",\"r\":\"0\"", Fvf, Count));
            Memory.assign((size_t)FvfStride(Fvf) * Count, 0);
            Out->Listing.VertexBuffer = (void*)(uintptr_t)900;
            Out->Listing.Fvf = Fvf;
            Out->Listing.BaseVertex = GSurf.NextBase;
            GSurf.NextBase += Count;
            Out->Vertices = Memory.data();
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API UnlockVertexMemory(FVertexMemoryLock* Out) override
        {
            GSurf.Event(Format("\"ev\":\"unlock\",\"base\":\"%lu\",\"bytes\":\"", Out->Listing.BaseVertex) +
                        Hex(Memory.data(), Memory.size()) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API UnsupportedManagerOperation() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CopyVertexData(void* Dest, unsigned int Fvf, const FRpVertexSrcDesc* Source,
                                                      unsigned int StartVertex, unsigned int Count) override
        {
            GSurf.Event(Format("\"ev\":\"copy\",\"fvf\":\"%u\",\"start\":\"%u\",\"n\":\"%u\"", Fvf, StartVertex,
                               Count));
            const unsigned long Stride = FvfStride(Fvf);
            unsigned char* Bytes = (unsigned char*)Dest + (size_t)Stride * StartVertex;
            for (unsigned int Vertex = 0; Vertex < Count; ++Vertex)
            {
                for (unsigned long Byte = 0; Byte < Stride; ++Byte)
                    Bytes[Vertex * Stride + Byte] = (unsigned char)(0x40 + 7 * Vertex + Byte);
            }
            return GR_OK;
        }
    };

    class FSurfTextureLibrary final : public ITextureLibrary2
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }

        GENRESULT NEURONCOMPONENTS_API FindTextureByCrc(void* Query, void** OutTex) override
        {
            const char* Name = *(const char* const*)Query;
            const unsigned long Crc = ((const unsigned long*)Query)[1];
            for (size_t Index = 0; Index < GSurf.Textures.size(); ++Index)
            {
                if (GSurf.Textures[Index].Crc == Crc)
                {
                    GSurf.Event(Format("\"ev\":\"find\",\"name\":\"%s\",\"r\":\"0\"", HexC(Name).c_str()));
                    *OutTex = &GSurf.Textures[Index];
                    return GR_OK;
                }
            }
            GSurf.Event(Format("\"ev\":\"find\",\"name\":\"%s\",\"r\":\"-1\"", HexC(Name).c_str()));
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API GetFrameCount(void* Record, int* Out) override
        {
            const FSurfTexture* Texture = (const FSurfTexture*)Record;
            GSurf.Event(Format("\"ev\":\"frames\",\"name\":\"%s\",\"n\":\"%d\"", TextHex(Texture->Name).c_str(),
                               Texture->Frames));
            *Out = Texture->Frames;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API CreatePlaybackGroup(void* SourceRecord, int* OutId) override
        {
            const FSurfTexture* Texture = (const FSurfTexture*)SourceRecord;
            *OutId = -1;
            if (Texture->bPlayFail)
            {
                GSurf.Event(Format("\"ev\":\"play\",\"name\":\"%s\",\"id\":\"-1\",\"r\":\"-1\"",
                                   TextHex(Texture->Name).c_str()));
                return GR_GENERIC;
            }
            const int Id = GSurf.NextPlayback++;
            GSurf.Playbacks[Id] = (size_t)(Texture - GSurf.Textures.data());
            *OutId = Id;
            GSurf.Event(Format("\"ev\":\"play\",\"name\":\"%s\",\"id\":\"%d\",\"r\":\"0\"",
                               TextHex(Texture->Name).c_str(), Id));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetFrame(int Id, int Frame, unsigned long* Out) override
        {
            const std::map<int, size_t>::const_iterator Found = GSurf.Playbacks.find(Id);
            if (Found == GSurf.Playbacks.end())
            {
                GSurf.Event(Format("\"ev\":\"frame\",\"id\":\"%d\",\"f\":\"%d\",\"h\":\"-\",\"r\":\"-1\"", Id, Frame));
                return GR_GENERIC;
            }
            const FSurfTexture& Texture = GSurf.Textures[Found->second];
            const unsigned long Handle = Texture.Handle + (Frame == -1 && Texture.Frames > 1 ? 100 : 0);
            Out[0] = Handle;
            Out[1] = 0x3E800000;
            Out[2] = 0;
            Out[3] = 0x3F800000;
            Out[4] = 0x3F000000;
            GSurf.Event(Format("\"ev\":\"frame\",\"id\":\"%d\",\"f\":\"%d\",\"h\":\"%lu\",\"r\":\"0\"", Id, Frame,
                               Handle));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API ReleaseTextureRecord(void* Record) override
        {
            const FSurfTexture* Texture = (const FSurfTexture*)Record;
            GSurf.Event(Format("\"ev\":\"trel\",\"name\":\"%s\"", TextHex(Texture->Name).c_str()));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API DecRefById(int Id) override
        {
            GSurf.Event(Format("\"ev\":\"decref\",\"id\":\"%d\"", Id));
            return GR_OK;
        }

        GENRESULT NEURONCOMPONENTS_API SetGenericSlot(int Index, unsigned long Value) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetGenericSlot(int Index, unsigned long* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LoadTextureGroup(void* FileSystem, int* OutGroupId) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API LoadTextureLibrary(void* FileSystem, const char* Name) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API GarbageCollect(int GroupId) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API TickAllPlayback(int Unused, float DeltaSeconds) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetGroupName(int Id, const char* Name) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetGroupName(int Id, char* Buf, unsigned long Size) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API FindOrCreateByName(const char* Name, void** OutTex) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API AddRefById(int Id) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CopyName(void* Node, char* Buf, unsigned long Size) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetRecordName(void* Record, const char* Name) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetSubTexture(void* Record, int Index, int TextureId) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetFrameHandle(void* Record, int Index, unsigned long Handle) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetFrameRect(void* Record, int Index, float U0, float V0, float U1,
                                                    float V1) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API GetTextureFrame(void* Tex, int Frame, unsigned long* Out) override
        {
            const FSurfTexture* Texture = (const FSurfTexture*)Tex;
            Out[0] = Texture->Handle;
            Out[1] = 0x3E800000;
            Out[2] = 0;
            Out[3] = 0x3F800000;
            Out[4] = 0x3F000000;
            GSurf.Event(Format("\"ev\":\"tframe\",\"name\":\"%s\",\"f\":\"%d\",\"h\":\"%lu\",\"r\":\"0\"",
                               TextHex(Texture->Name).c_str(), Frame, Texture->Handle));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetGroupPixelFormat(void* Record, void* Unused, int* OutFormat) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetTileRate(void* Record, float Value) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTileRate(void* Record, float* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ResolveTextureRecord(int Id, void** Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetElapsedTime(int Id, float Seconds) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetElapsedTime(int Id, float* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetFrameIndex(int Id, unsigned int Index) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetFrameIndex(int Id, unsigned int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetLocalRate(int Id, float Value) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetEffectiveRate(int Id, float* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetWrapMode(int Id, int Mode) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetWrapMode(int Id, int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API AdvanceTime(int Id, float DeltaSeconds) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API PeekNextGroupId(unsigned long* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API EnumGroup(int Index, int* OutId) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetSubCount(int Id, int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API EnumTexture(int Id, int Index, void** OutRecord) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API AddSubTexture(int GroupId, void* Record) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API RemoveSubTexture(int GroupId, void* Record) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API PickGpuFormat(const unsigned long* Src, unsigned long* OutId) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API AllocateNewTextureId(int* OutId) override { return GR_GENERIC; }
    };


    class FSurfWire final : public IVMeshWire
    {
    public:
        unsigned long Inst = 0;
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        void* NEURONCOMPONENTS_API GetWireData() override { return nullptr; }
        int NEURONCOMPONENTS_API Draw() override
        {
            GSurf.Event(Format("\"ev\":\"wdraw\",\"inst\":\"%lu\"", Inst));
            return GR_OK;
        }
        void* NEURONCOMPONENTS_API GetMeshData() override { return nullptr; }
    };

    class FSurfMeshObject final : public IMesh
    {
    public:
        unsigned long Inst = 0;

        FSurfMesh& Mesh() { return GSurf.Meshes[Inst]; }

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        int NEURONCOMPONENTS_API ResetMesh() override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API NoOp() override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RefreshMaterials(unsigned long Flags) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation(void* A) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation() override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetFaceGroupCount(int* Out) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API OpenFaceGroup(int Index, int Mode) override
        {
            int Result = 0;
            if (!GSurf.OpenQueue.empty())
            {
                Result = GSurf.OpenQueue.front();
                GSurf.OpenQueue.erase(GSurf.OpenQueue.begin());
            }
            GSurf.Event(Format("\"ev\":\"mopen\",\"inst\":\"%lu\",\"g\":\"%d\",\"m\":\"%d\",\"r\":\"%d\"", Inst, Index,
                               Mode, Result));
            return Result;
        }
        int NEURONCOMPONENTS_API CloseFaceGroup() override
        {
            GSurf.Event(Format("\"ev\":\"mclose\",\"inst\":\"%lu\"", Inst));
            return GR_OK;
        }
        int NEURONCOMPONENTS_API AddFaceGroup(void* A) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RemoveFaceGroup(int Index) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetFaceGroupPrimitiveType(unsigned int* Out) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetFaceGroupMaterial(IMaterial** Out) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API SetFaceGroupMaterial(IMaterial* Material) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetFaceGroupMaterialContext(void** Out) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API SetFaceGroupMaterialContext(void* A) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API OpenFaceGroupChannels(unsigned long Flags) override
        {
            const int Result = Mesh().ChannelResult;
            GSurf.Event(Format("\"ev\":\"mchan\",\"inst\":\"%lu\",\"f\":\"%lu\",\"r\":\"%d\"", Inst, Flags, Result));
            return Result;
        }
        int NEURONCOMPONENTS_API CloseFaceGroupChannels() override
        {
            GSurf.Event(Format("\"ev\":\"mchanclose\",\"inst\":\"%lu\"", Inst));
            return GR_OK;
        }
        int NEURONCOMPONENTS_API LockFaceGroupChannels(unsigned long Mask) override
        {
            GSurf.Event(Format("\"ev\":\"mlock\",\"inst\":\"%lu\",\"m\":\"%lu\"", Inst, Mask));
            return GR_OK;
        }
        int NEURONCOMPONENTS_API UnlockFaceGroupChannels() override
        {
            GSurf.Event(Format("\"ev\":\"munlock\",\"inst\":\"%lu\"", Inst));
            return GR_OK;
        }
        int NEURONCOMPONENTS_API GetFaceGroupData(int Channel, void** A, void** B, void** C, void** D) override
        {
            GSurf.Event(Format("\"ev\":\"mdata\",\"inst\":\"%lu\",\"ch\":\"%d\"", Inst, Channel));
            FSurfMesh& M = Mesh();
            *A = Channel == 0x100 ? (void*)M.Values.data() : (void*)M.Positions.data();
            return GR_OK;
        }
        int NEURONCOMPONENTS_API SetFaceGroupData(int Channel, void* A, void* B, void* C, void* D) override
        {
            return GR_GENERIC;
        }
        int NEURONCOMPONENTS_API GetFaceGroupIndices(int Channel, void** A, void** B) override
        {
            FSurfMesh& M = Mesh();
            const unsigned long Count = Channel == 0x100 ? (unsigned long)M.ValueIndices.size() : M.Count;
            GSurf.Event(Format("\"ev\":\"midx\",\"inst\":\"%lu\",\"ch\":\"%d\",\"n\":\"%lu\"", Inst, Channel, Count));
            *A = Channel == 0x100 ? (void*)M.ValueIndices.data() : (void*)M.PositionIndices.data();
            *B = (void*)(uintptr_t)Count;
            return GR_OK;
        }
        int NEURONCOMPONENTS_API SetFaceGroupIndices(int Channel, void* Src, int Count) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetFaceGroupIndexCount(int Selector, void** Out) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API SetFaceGroupIndexCount(int Selector, int Count) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API AddFaceGroupChannels(unsigned long Flags) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RemoveFaceGroupChannels(unsigned long Flags) override { return GR_GENERIC; }
    };

    FSurfWire GSurfWire;
    FSurfMeshObject GSurfMesh;

    class FSurfEngine final : public IEngine
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API LoadEngineComponents(INeuronComponent* SystemContainer) override
        {
            return GR_GENERIC;
        }
        unsigned long NEURONCOMPONENTS_API EnumerateEngineComponents(int(__cdecl* Callback)(const char*, void*)) override
        {
            return 0;
        }
        GENRESULT NEURONCOMPONENTS_API NotifyEngineComponents(void* Arg) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetSearchPath(const char* Path) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API QuerySearchPath(char* Dest, unsigned long MaxChars) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetFilesystem(INeuronComponent* NewPath) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetSearchPath(void** Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CreateFileSystem(const char* Path, void** Out, void* Factory) override
        {
            return GR_GENERIC;
        }
        void* NEURONCOMPONENTS_API CreateCompoundArchetype(FEngineName* Name) override { return nullptr; }
        void* NEURONCOMPONENTS_API CreateArchetypeHandle(FEngineName* Name, void* FileSystem) override { return nullptr; }
        void* NEURONCOMPONENTS_API DuplicateArchetype(void* ArchIn, FEngineName* Name) override { return nullptr; }
        unsigned long NEURONCOMPONENTS_API AddRefHandle(void* Handle) override { return 0; }
        unsigned long NEURONCOMPONENTS_API ReleaseHandle(void* Handle) override { return 0; }
        FEngineName* NEURONCOMPONENTS_API GetArchetypeName(FEngineName* Out, void* Handle) override { return Out; }
        void* NEURONCOMPONENTS_API FindArchetype(FEngineName* Name) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API QueryEngineComponent(int Id, void* Name, void** Out) override { return GR_GENERIC; }
        unsigned long NEURONCOMPONENTS_API IsCompoundArchetype(void* Handle) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API EnumerateArchetypeConstraints(void* Handle,
                                                                     int(__cdecl* Callback)(void*, void*, void*, void*),
                                                                     void* Context) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API EnumerateArchetypeChildren(void* Handle,
                                                                  int(__cdecl* Callback)(void*, void*, int, void*),
                                                                  void* Context) override
        {
            return GR_GENERIC;
        }
        void* NEURONCOMPONENTS_API CreateInstanceFromName(FEngineName* Name, void* FileSystem, void* Transform) override
        {
            return nullptr;
        }
        void* NEURONCOMPONENTS_API CreateInstance(void* Handle, void* Transform) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API DestroyInstance(void* Instance) override { return GR_GENERIC; }
        void* NEURONCOMPONENTS_API GetInstanceArchetype(void* Instance) override { return nullptr; }
        FEngineName* NEURONCOMPONENTS_API GetInstanceName(FEngineName* Out, void* Instance) override { return Out; }
        GENRESULT NEURONCOMPONENTS_API UpdateInstance(void* Instance, unsigned long Flags, void* Arg) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetInstanceTransform(void* Instance, void* Transform) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetInstanceTransformObject(void* Instance, void** Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API QueryInstance(void* Instance, const char* Iid, void** Out) override
        {
            const unsigned long Inst = (unsigned long)(uintptr_t)Instance;
            const bool bWire = strcmp(Iid, Iids::VMeshWire) == 0;
            bool bFound = false;
            if (bWire)
            {
                const std::map<unsigned long, bool>::const_iterator Found = GSurf.Wires.find(Inst);
                bFound = Found != GSurf.Wires.end() && Found->second;
            }
            else
                bFound = GSurf.Meshes.find(Inst) != GSurf.Meshes.end();
            GSurf.Event(Format("\"ev\":\"qinst\",\"inst\":\"%lu\",\"iid\":\"%s\",\"r\":\"%d\"", Inst,
                               bWire ? "wire" : "mesh", bFound ? 0 : -1));
            if (!bFound)
            {
                *Out = nullptr;
                return GR_GENERIC;
            }
            if (bWire)
            {
                GSurfWire.Inst = Inst;
                *Out = (IVMeshWire*)&GSurfWire;
            }
            else
            {
                GSurfMesh.Inst = Inst;
                *Out = (IMesh*)&GSurfMesh;
            }
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetInstanceUserData(void* Instance, void* Value) override { return GR_GENERIC; }
        void* NEURONCOMPONENTS_API GetInstanceUserData(void* Instance) override
        {
            const unsigned long Inst = (unsigned long)(uintptr_t)Instance;
            const std::map<unsigned long, std::vector<unsigned long>>::iterator Found = GSurf.UserData.find(Inst);
            if (Found == GSurf.UserData.end())
            {
                GSurf.Event(Format("\"ev\":\"ud\",\"inst\":\"%lu\",\"v\":\"-\"", Inst));
                return nullptr;
            }
            GSurf.Event(Format("\"ev\":\"ud\",\"inst\":\"%lu\",\"v\":\"", Inst) + Words(Found->second.data(), 5) +
                        "\"");
            return Found->second.data();
        }
        GENRESULT NEURONCOMPONENTS_API SetInstanceExtent(void* Instance, unsigned long Flags, float Extent,
                                                         const float* Vec) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API GetInstanceExtent(void* Instance, unsigned long Flags, float* OutExtent,
                                                         float* OutVec) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetInstancePosition(void* Instance, void* Position) override { return GR_GENERIC; }
        float* NEURONCOMPONENTS_API GetInstancePosition(void* Instance) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API SetInstanceOrientation(void* Instance, void* Orient) override { return GR_GENERIC; }
        float* NEURONCOMPONENTS_API GetInstanceOrientation(void* Instance) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API SetInstanceMatrix(void* Instance, void* Matrix) override { return GR_GENERIC; }
        float* NEURONCOMPONENTS_API GetInstanceTransform(void* Instance) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API NoOp41(void* A, void* B) override { return GR_GENERIC; }
        float* NEURONCOMPONENTS_API GetIdentityMatrix(void* Unused) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API NoOp43(void* A, void* B) override { return GR_GENERIC; }
        float* NEURONCOMPONENTS_API GetIdentityMatrix2(void* Unused) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API SetTransformUser(void* Instance, unsigned long User) override { return GR_GENERIC; }
        unsigned long NEURONCOMPONENTS_API GetTransformUser(void* Instance) override { return 0; }
        unsigned long NEURONCOMPONENTS_API CreateJoint(void* Parent, void* Child, const unsigned long* Payload) override
        {
            return 0;
        }
        unsigned long NEURONCOMPONENTS_API DestroyJoint(void* Parent, void* Child) override { return 0; }
        unsigned long* NEURONCOMPONENTS_API GetInstanceRelative(void* Instance) override
        {
            GSurf.Event(Format("\"ev\":\"rel\",\"inst\":\"%lu\"", (unsigned long)(uintptr_t)Instance));
            return nullptr;
        }
        unsigned long NEURONCOMPONENTS_API SetJointParams(void* Instance, unsigned long Index, const float* Values) override
        {
            return 0;
        }
        unsigned long NEURONCOMPONENTS_API GetJointParams(void* Instance, unsigned long Index, float* Out) override
        {
            return 0;
        }
        void* NEURONCOMPONENTS_API FindMappedInstance(void* Instance) override { return nullptr; }
        void* NEURONCOMPONENTS_API GetInstanceParent(void* Instance) override { return nullptr; }
        void* NEURONCOMPONENTS_API GetRelatedInstance(void* From, unsigned long Flags, void* Cursor) override
        {
            return nullptr;
        }
        void* NEURONCOMPONENTS_API GetInstanceRoot(void* Instance) override
        {
            const unsigned long Inst = (unsigned long)(uintptr_t)Instance;
            const std::map<unsigned long, unsigned long>::const_iterator Found = GSurf.Roots.find(Inst);
            const unsigned long Root = Found == GSurf.Roots.end() ? Inst : Found->second;
            GSurf.Event(Format("\"ev\":\"root\",\"inst\":\"%lu\",\"r\":\"%lu\"", Inst, Root));
            return (void*)(uintptr_t)Root;
        }
        void* NEURONCOMPONENTS_API FindArchetypeChild(void* Handle, void* ChildHandle) override { return nullptr; }
        unsigned long NEURONCOMPONENTS_API RejectOperation(void* A, void* B) override { return 0; }
    };

    FSurfEngine GSurfEngine;

    class FSurfCandidate;

    class FSurfCandidateBatched final : public IBatchedMaterial
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API CreateInnerMaterial(void* Name) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetBatchBin(void* Value) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetBatchBin(void** Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Bind(void* Context) override
        {
            GSurf.Event(Format("\"ev\":\"cbind\",\"face\":\"b\",\"r\":\"%d\"", GSurf.Candidate.BindResult));
            return GSurf.Candidate.BindResult;
        }
        GENRESULT NEURONCOMPONENTS_API Teardown(void* Context) override
        {
            GSurf.Event(Format("\"ev\":\"cteardown\",\"face\":\"b\",\"r\":\"%d\"", GSurf.Candidate.TeardownResult));
            return GSurf.Candidate.TeardownResult;
        }
        GENRESULT NEURONCOMPONENTS_API Submit(void* Context, unsigned long PrimType, unsigned long Start,
                                              unsigned long Count, unsigned long IbStart, unsigned long IndexSpan,
                                              unsigned long Flags) override
        {
            GSurf.Event(Format("\"ev\":\"csubmit\",\"face\":\"b\",\"prim\":\"%lu\",\"start\":\"%lu\",\"count\":\"%lu\","
                               "\"ibs\":\"%lu\",\"span\":\"%lu\",\"flags\":\"%lx\",\"r\":\"%d\"",
                               PrimType, Start, Count, IbStart, IndexSpan, Flags, GSurf.Candidate.SubmitResult));
            return GSurf.Candidate.SubmitResult;
        }
    };

    FSurfCandidateBatched GSurfCandidateBatched;

    class FSurfCandidate final : public IMaterial
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            if (strcmp(Iid, Iids::BatchedMaterial) == 0)
            {
                GSurf.Event(Format("\"ev\":\"cqi\",\"r\":\"%d\"", GSurf.Candidate.bBatched ? 0 : -1));
                if (GSurf.Candidate.bBatched)
                {
                    *Out = (IBatchedMaterial*)&GSurfCandidateBatched;
                    return GR_OK;
                }
            }
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API Initialize(INeuronComponent* Outer) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LoadFromFileSystem(void* FileSystem) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SaveProperties(void* FileSystem) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ApplyVerify(void* Context, void* Extra) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ResolveTextures(float Delta) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Bind(void* Context) override
        {
            GSurf.Event(Format("\"ev\":\"cbind\",\"face\":\"m\",\"r\":\"%d\"", GSurf.Candidate.BindResult));
            return GSurf.Candidate.BindResult;
        }
        GENRESULT NEURONCOMPONENTS_API Teardown(void* Context) override
        {
            GSurf.Event(Format("\"ev\":\"cteardown\",\"face\":\"m\",\"r\":\"%d\"", GSurf.Candidate.TeardownResult));
            return GSurf.Candidate.TeardownResult;
        }
        GENRESULT NEURONCOMPONENTS_API Submit(void* Context, unsigned long PrimType, unsigned long Start,
                                              unsigned long Count, unsigned long IbStart, unsigned long IndexSpan,
                                              unsigned long Flags) override
        {
            GSurf.Event(Format("\"ev\":\"csubmit\",\"face\":\"m\",\"prim\":\"%lu\",\"start\":\"%lu\",\"count\":\"%lu\","
                               "\"ibs\":\"%lu\",\"span\":\"%lu\",\"flags\":\"%lx\",\"r\":\"%d\"",
                               PrimType, Start, Count, IbStart, IndexSpan, Flags, GSurf.Candidate.SubmitResult));
            return GSurf.Candidate.SubmitResult;
        }
        GENRESULT NEURONCOMPONENTS_API SetName(const char* Name) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetName(char* Out, unsigned long Size) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CopyClassName(char* Out, unsigned long Size) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetPassCount(int* Out) override
        {
            if (!GSurf.Candidate.bPasses)
            {
                GSurf.Event("\"ev\":\"cpasses\",\"v\":\"-\"");
                return GR_GENERIC;
            }
            *Out = GSurf.Candidate.Passes;
            GSurf.Event(Format("\"ev\":\"cpasses\",\"v\":\"%d\"", GSurf.Candidate.Passes));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetRenderFlags(unsigned long* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetParameterCount(unsigned long* OutCount) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetParameterDescriptor(unsigned long Index, void* OutDesc) override
        {
            const FSurfCandidateConfig& C = GSurf.Candidate;
            GSurf.Event(Format("\"ev\":\"cdesc\",\"i\":\"%lu\",\"r\":\"%d\",\"tag\":\"%s\"", Index, C.DescResult,
                               C.bTag ? TextHex(C.Tag).c_str() : "-"));
            if (C.DescResult == GR_OK)
                ((FShadingPropertyRecord*)OutDesc)->Name = C.bTag ? C.Tag.c_str() : nullptr;
            return C.DescResult;
        }
        GENRESULT NEURONCOMPONENTS_API GetParameterValue(unsigned long Index, void* InOutDesc) override
        {
            GSurf.Event(Format("\"ev\":\"cparam\",\"i\":\"%lu\",\"v\":\"%08lx\"", Index, GSurf.Candidate.Param));
            ((FShadingPropertyValue*)InOutDesc)->Payload0 = GSurf.Candidate.Param;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetParameterValue(unsigned long Index, void* Value) override { return GR_GENERIC; }
        const char* __thiscall ClassName() override
        {
            GSurf.Event("\"ev\":\"cclass\",\"name\":\"" + TextHex(GSurf.Candidate.Class) + "\"");
            return GSurf.Candidate.Class.c_str();
        }
        void* __thiscall ScalarDeletingDestructor(unsigned char bDelete) override { return this; }
        GENRESULT __thiscall InitializeFromDescriptor(void* Arg) override { return GR_GENERIC; }
        GENRESULT __thiscall ApplyInitialRenderStates(void* Arg) override { return GR_GENERIC; }
        GENRESULT __thiscall Setup() override { return GR_GENERIC; }
        bool __thiscall ValueWithinDeclaredRange(unsigned long Index, void* Entry, void* Value) override
        {
            return false;
        }
        GENRESULT __thiscall StoreParameterValue(unsigned long Index, void* Entry, void* Value) override
        {
            return GR_GENERIC;
        }
        unsigned long __thiscall GetParameterTable(void** OutBase) override { return 0; }
        void* __thiscall GetParameterAddress(unsigned long Index) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API InnerQueryInterface(const char* Iid, void** Out) override { return GR_GENERIC; }
        unsigned long NEURONCOMPONENTS_API InnerAddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API InnerRelease() override { return 1; }
    };

    FSurfCandidate GSurfCandidate;

    FSurfPipeline GSurfPipeline;
    FSurfDraw GSurfDraw;
    FSurfVbMgr GSurfVbMgr;
    FSurfTextureLibrary GSurfTextures;
    FMaterialLibrary* GSurfLibrary = nullptr;

    // The component Initialize asks.
    class FSurfComponent final : public INeuronComponent
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            if (strcmp(Iid, Iids::TextureLibraryA) == 0)
                *Out = (ITextureLibrary2*)&GSurfTextures;
            else if (strcmp(Iid, Iids::RenderPipeline8B) == 0)
                *Out = (IRenderPipeline*)&GSurfPipeline;
            else if (strcmp(Iid, Iids::RPDraw) == 0)
                *Out = (IRPDraw*)&GSurfDraw;
            else if (strcmp(Iid, Iids::VertexBufferManager) == 0)
                *Out = (IVertexBufferManager*)&GSurfVbMgr;
            else if (strcmp(Iid, Iids::MaterialLibrary) == 0 && GSurfLibrary != nullptr)
            {
                GSurfLibrary->AddRef();
                *Out = (IMaterialLibrary*)GSurfLibrary;
            }
            else
            {
                *Out = nullptr;
                return GR_INTERFACE_UNSUPPORTED;
            }
            return GR_OK;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
    };

    FSurfComponent GSurfComponent;

    void SetStream(FRpVertexStream& Stream, const unsigned long* Data, unsigned int Stride,
                   const std::vector<unsigned long>& Index, unsigned int Count)
    {
        memset(&Stream, 0, sizeof(Stream));
        if (Data == nullptr)
            return;
        Stream.Base = (const unsigned char*)Data;
        Stream.ByteSize = Stride;
        Stream.Stride = Stride;
        Stream.IndexArray = Index.empty() ? nullptr : Index.data();
        Stream.Count = Count;
    }

    std::vector<unsigned short> Shorts(const std::string& Text)
    {
        std::vector<unsigned short> Out;
        if (Text == "-")
            return Out;
        const std::vector<std::string> Parts = SplitText(Text, ",");
        for (size_t Index = 0; Index < Parts.size(); ++Index)
            Out.push_back((unsigned short)Dec(Parts[Index]));
        return Out;
    }

    class FSurfCase
    {
    public:
        FSurfCase(Suite* InOut, unsigned int InCase)
        {
            GSurf = FSurfState();
            GSurf.Out = InOut;
            GSurf.Case = InCase;
            GSurfLibrary = &Library;
            G_FlmEngineOuter = nullptr;
            FlmVisibleCounterReset();
            const unsigned long Unset = 0xFFFFFFFFul;
            Library.SetGlobalRecord(4, &Unset);
        }

        ~FSurfCase()
        {
            for (size_t Index = 0; Index < GSurf.Instances.size(); ++Index)
                delete GSurf.Instances[Index];
            GSurf.Instances.clear();
            GSurfLibrary = nullptr;
            G_FlmEngineOuter = nullptr;
        }

        void Op(const std::string& Fields)
        {
            GSurf.Out->Line(Format("{\"case\":\"%u\",\"op\":", GSurf.Case) + Fields + "}");
        }

        FMaterialSubmitContext* Context(const std::string& Name)
        {
            if (Name == "-")
                return nullptr;
            return &Contexts[Name].Context;
        }

        void Params()
        {
            unsigned long Count = 0;
            Material->GetParameterCount(&Count);
            for (unsigned long Index = 0; Index < Count; ++Index)
            {
                FShadingPropertyValue Value = {};
                const GENRESULT Result = Material->GetParameterValue(Index, &Value);
                std::string Text;
                if (Result < 0)
                    Text = "-";
                else if (Value.Type == 5 || Value.Type == 7)
                    Text = Value.Payload0 == 0 ? std::string("s-") : "s" + HexC((const char*)Value.Payload0);
                else if (Value.Type == 3)
                    Text = Words(&Value.Payload0, 3);
                else if (Value.Type == 4)
                    Text = Words(&Value.Payload0, 4);
                else
                    Text = Words(&Value.Payload0, 1);
                Op(Format("\"param\",\"i\":\"%lu\",\"r\":\"%d\",\"t\":\"%lu\",\"v\":\"%s\"", Index, (int)Result,
                          Value.Type, Text.c_str()));
                Value.ReleaseOwnedPayload();
            }
        }

        void Run(const std::vector<std::string>& F)
        {
            const std::string& K = F[0];
            if (K == "global")
            {
                const unsigned long Value = strtoul(F[2].c_str(), nullptr, 10);
                Library.SetGlobalRecord((unsigned long)Dec(F[1]), &Value);
            }
            else if (K == "tex")
            {
                FSurfTexture Texture;
                Texture.Name = std::string(CStr(Unhex(F[1])));
                Texture.Crc = NEURONCOMPONENTS_CRC::GetCRC32(Texture.Name.c_str());
                Texture.Frames = (int)Dec(F[2]);
                Texture.Handle = (unsigned long)Dec(F[3]);
                Texture.Format = (int)Dec(F[4]);
                Texture.bPlayFail = Dec(F[5]) != 0;
                GSurf.Textures.push_back(Texture);
            }
            else if (K == "animlib")
                GSurf.bAnimLibrary = true;
            else if (K == "anim")
            {
                FSurfAnim Anim;
                Anim.Name = std::string(CStr(Unhex(F[1])));
                Anim.Crc = NEURONCOMPONENTS_CRC::GetCRC32(Anim.Name.c_str());
                Anim.bFills = Dec(F[2]) != 0;
                GSurf.Anims.push_back(Anim);
            }
            else if (K == "rs")
                GSurf.RenderStates[(int)Dec(F[1])] = (unsigned long)Dec(F[2]);
            else if (K == "tss")
                GSurf.StageStates[(int)Dec(F[1]) * 256 + (int)Dec(F[2])] = (unsigned long)Dec(F[3]);
            else if (K == "rpdraw")
                GSurf.RpDrawResult = (int)Dec(F[1]);
            else if (K == "dvb")
                GSurf.DvbResult = (int)Dec(F[1]);
            else if (K == "dpvb")
                GSurf.DpvbResult = (int)Dec(F[1]);
            else if (K == "lockfail")
                GSurf.bLockFail = Dec(F[1]) != 0;
            else if (K == "verify" && F.size() == 2)
            {
                GSurf.VerifyQueue.clear();
                const std::vector<std::string> Parts = SplitText(F[1], ",");
                for (size_t Index = 0; Index < Parts.size(); ++Index)
                    GSurf.VerifyQueue.push_back((int)Dec(Parts[Index]));
            }
            else if (K == "engine")
                G_FlmEngineOuter = Dec(F[1]) != 0 ? (INeuronComponent*)(IEngine*)&GSurfEngine : nullptr;
            else if (K == "ud")
                GSurf.UserData[(unsigned long)Dec(F[1])] = HexWords(F[2]);
            else if (K == "root")
                GSurf.Roots[(unsigned long)Dec(F[1])] = (unsigned long)Dec(F[2]);
            else if (K == "ival")
                GSurf.Values[(int)Dec(F[1])] = HexWord(F[2]);
            else if (K == "iname")
                GSurf.Names[(int)Dec(F[1])] = std::string(CStr(Unhex(F[2])));
            else if (K == "wire")
                GSurf.Wires[(unsigned long)Dec(F[1])] = Dec(F[2]) != 0;
            else if (K == "mesh")
            {
                FSurfMesh& M = GSurf.Meshes[(unsigned long)Dec(F[1])];
                M.ChannelResult = (int)Dec(F[2]);
                M.Values = HexWords(F[3]);
                M.ValueIndices = Shorts(F[4]);
                M.Positions = HexWords(F[5]);
                M.PositionIndices = Shorts(F[6]);
                M.Count = (unsigned long)Dec(F[7]);
            }
            else if (K == "mopen")
            {
                GSurf.OpenQueue.clear();
                const std::vector<std::string> Parts = SplitText(F[1], ",");
                for (size_t Index = 0; Index < Parts.size(); ++Index)
                    GSurf.OpenQueue.push_back((int)Dec(Parts[Index]));
            }
            else if (K == "cand")
            {
                FSurfCandidateConfig& C = GSurf.Candidate;
                C.bBatched = Dec(F[1]) != 0;
                C.bPasses = F[2] != "-";
                C.Passes = C.bPasses ? (int)Dec(F[2]) : 0;
                C.Class = std::string(CStr(Unhex(F[3])));
                C.DescResult = (int)Dec(F[4]);
                C.bTag = F[5] != "-";
                C.Tag = C.bTag ? std::string(CStr(Unhex(F[5]))) : std::string();
                C.Param = HexWord(F[6]);
                C.BindResult = (int)Dec(F[7]);
                C.TeardownResult = (int)Dec(F[8]);
                C.SubmitResult = (int)Dec(F[9]);
            }
            else if (K == "vsrc")
            {
                // Separate streams: positions and normals 12 bytes, diffuse 4, tex0 8.
                FSurfSource& S = Sources[F[1]];
                const unsigned int Count = (unsigned int)Dec(F[4]);
                S.Pos = HexWords(F[5]);
                S.Normal = HexWords(F[6]);
                S.Diffuse = HexWords(F[7]);
                S.Tex0 = HexWords(F[8]);
                S.Index.clear();
                if (F[9] != "-")
                {
                    const std::vector<std::string> Parts = SplitText(F[9], ",");
                    for (size_t Index = 0; Index < Parts.size(); ++Index)
                        S.Index.push_back((unsigned long)Dec(Parts[Index]));
                }
                memset(&S.Desc, 0, sizeof(S.Desc));
                S.Desc.Flags = (unsigned int)Dec(F[2]);
                SetStream(S.Desc.Position, S.Pos.empty() ? nullptr : S.Pos.data(), 12, S.Index, Count);
                SetStream(S.Desc.Normal, S.Normal.empty() ? nullptr : S.Normal.data(), 12, S.Index, Count);
                SetStream(S.Desc.Diffuse, S.Diffuse.empty() ? nullptr : S.Diffuse.data(), 4, S.Index, Count);
                SetStream(S.Desc.Tex[0], S.Tex0.empty() ? nullptr : S.Tex0.data(), 8, S.Index, Count);
                S.Desc.SrcFvf = (unsigned int)Dec(F[3]);
                S.Desc.SrcCount = Count;
            }
            else if (K == "vsil")
            {
                // One interleaved array of the source's vertex format: every stream at its offset
                // with the vertex's stride.
                FSurfSource& S = Sources[F[1]];
                const unsigned int Count = (unsigned int)Dec(F[4]);
                const unsigned long Fvf = (unsigned long)Dec(F[3]);
                S.Pos = HexWords(F[5]);
                S.Index.clear();
                memset(&S.Desc, 0, sizeof(S.Desc));
                S.Desc.Flags = (unsigned int)Dec(F[2]);
                const unsigned int Stride = (unsigned int)FvfStride(Fvf);
                const unsigned long* Data = S.Pos.data();
                unsigned int Offset = 12;
                SetStream(S.Desc.Position, Data, Stride, S.Index, Count);
                if ((Fvf & 0x10) != 0)
                {
                    SetStream(S.Desc.Normal, Data + Offset / 4, Stride, S.Index, Count);
                    Offset += 12;
                }
                if ((Fvf & 0x40) != 0)
                {
                    SetStream(S.Desc.Diffuse, Data + Offset / 4, Stride, S.Index, Count);
                    Offset += 4;
                }
                SetStream(S.Desc.Tex[0], Data + Offset / 4, Stride, S.Index, Count);
                S.Desc.SrcFvf = (unsigned int)Fvf;
                S.Desc.SrcCount = Count;
            }
            else if (K == "idx")
            {
                std::vector<unsigned short>& List = Indices[F[1]];
                const std::vector<std::string> Parts = SplitText(F[2], ",");
                for (size_t Index = 0; Index < Parts.size(); ++Index)
                    List.push_back((unsigned short)Dec(Parts[Index]));
            }
            else if (K == "xf")
                Matrices[F[1]] = HexWords(F[2]);
            else if (K == "ctx")
            {
                FSurfContext& C = Contexts[F[1]];
                memset(&C, 0, sizeof(C));
                C.Context.Inst = (void*)(uintptr_t)Dec(F[8]);
                C.Context.World = Matrices[F[6]].data();
                C.Context.View = Matrices[F[7]].data();
                C.Context.SourceVertices = F[2] == "-" ? nullptr : &Sources[F[2]].Desc;
                C.Context.Indices = Indices[F[3]].data();
                if (F[4] != "-")
                {
                    const std::vector<std::string> Parts = SplitText(F[4], ",");
                    C.Vb.VertexBuffer = (void*)(uintptr_t)Dec(Parts[0]);
                    C.Vb.Fvf = (unsigned long)Dec(Parts[1]);
                    C.Vb.BaseVertex = (unsigned long)Dec(Parts[2]);
                    C.Context.VbListing = &C.Vb;
                }
                if (F[5] != "-")
                {
                    const std::vector<std::string> Parts = SplitText(F[5], ",");
                    C.Ib.IndexBuffer = (void*)(uintptr_t)Dec(Parts[0]);
                    C.Ib.Bias = (unsigned long)Dec(Parts[1]);
                    C.Context.IbScratch = &C.Ib;
                }
                C.Context.ReplacedMaterial = F[9] == "c" ? (IMaterial*)&GSurfCandidate : nullptr;
            }
            else if (K == "new")
            {
                const FMaterialSpec* Spec = nullptr;
                for (size_t Index = 0; Index < sizeof(GSurfSpecs) / sizeof(GSurfSpecs[0]); ++Index)
                {
                    if (F[1] == GSurfSpecs[Index].Name)
                        Spec = &GSurfSpecs[Index];
                }
                if (Spec == nullptr)
                {
                    fprintf(stderr, "material_surface: unknown material %s\n", F[1].c_str());
                    GFailed = true;
                    return;
                }
                FMaterialAggregateDesc Agg = {0x14, Spec->Name, nullptr, nullptr, nullptr};
                Material = (FMaterialImpl*)FlmCreateMaterial(*Spec);
                Material->ConfigureAggregation(&Agg);
                const GENRESULT Made = Material->InitializeFromDescriptor(&Agg);
                const GENRESULT Result = Material->Initialize(&GSurfComponent);
                Op(Format("\"new\",\"kind\":\"%s\",\"made\":\"%d\",\"r\":\"%d\"", F[1].c_str(), (int)Made,
                          (int)Result));
            }
            else if (K == "name")
            {
                const GENRESULT Result = Material->SetName(CStr(Unhex(F[1])));
                Op(Format("\"name\",\"r\":\"%d\"", (int)Result));
            }
            else if (K == "set")
            {
                FShadingPropertyValue Value = {};
                const unsigned long Type = (unsigned long)Dec(F[2]);
                if (Type == 5 || Type == 7)
                {
                    if (F[3] == "-")
                        Value.Type = Type;
                    else
                        Value.Assign(Type, CStr(Unhex(F[3])));
                }
                else
                {
                    Value.Type = Type;
                    const std::vector<unsigned long> W = HexWords(F[3]);
                    unsigned long* Slots = &Value.Payload0;
                    for (size_t Index = 0; Index < W.size() && Index < 4; ++Index)
                        Slots[Index] = W[Index];
                }
                const unsigned long Index = (unsigned long)Dec(F[1]);
                const GENRESULT Result = Material->SetParameterValue(Index, &Value);
                Value.ReleaseOwnedPayload();
                Op(Format("\"set\",\"i\":\"%lu\",\"r\":\"%d\"", Index, (int)Result));
            }
            else if (K == "flags")
            {
                unsigned long Flags = 0xCDCDCDCDu;
                const GENRESULT Result = Material->GetRenderFlags(&Flags);
                Op(Format("\"flags\",\"r\":\"%d\",\"v\":\"%08lx\"", (int)Result, Flags));
            }
            else if (K == "verify")
            {
                const GENRESULT Result = Material->ApplyVerify(nullptr, nullptr);
                Op(Format("\"verify\",\"r\":\"%d\"", (int)Result));
            }
            else if (K == "resolve")
            {
                const float Delta = FloatFromBits((unsigned int)HexWord(F[1]));
                const GENRESULT Result = Material->ResolveTextures(Delta);
                Op(Format("\"resolve\",\"r\":\"%d\"", (int)Result));
            }
            else if (K == "bind" || K == "teardown")
            {
                FMaterialSubmitContext* C = Context(F[1]);
                const GENRESULT Result = K == "bind" ? Material->Bind(C) : Material->Teardown(C);
                Op(Format("\"%s\",\"r\":\"%d\"", K.c_str(), (int)Result));
            }
            else if (K == "submit")
            {
                FMaterialSubmitContext* C = Context(F[1]);
                const GENRESULT Result =
                    Material->Submit(C, (unsigned long)Dec(F[2]), (unsigned long)Dec(F[3]),
                                     (unsigned long)Dec(F[4]), (unsigned long)Dec(F[5]),
                                     (unsigned long)Dec(F[6]), HexWord(F[7]));
                Op(Format("\"submit\",\"r\":\"%d\"", (int)Result));
            }
            else if (K == "passes")
            {
                int Passes = -7;
                const GENRESULT Result = Material->GetPassCount(&Passes);
                Op(Format("\"passes\",\"r\":\"%d\",\"v\":\"%d\"", (int)Result, Passes));
            }
            else if (K == "class")
                Op("\"class\",\"name\":\"" + std::string(Material->ClassName()) + "\"");
            else if (K == "params")
                Params();
            else if (K == "counters")
            {
                unsigned long Counters[3];
                FlmVisibleCounterCopy(Counters);
                Op(Format("\"counters\",\"v\":\"%lu,%lu,%lu\"", Counters[0], Counters[1], Counters[2]));
            }
            else if (K == "release")
            {
                Material->Release();
                Material = nullptr;
                Op("\"release\"");
            }
            else
            {
                fprintf(stderr, "material_surface: unknown operation %s\n", K.c_str());
                GFailed = true;
            }
        }

    private:
        static const FMaterialSpec GSurfSpecs[19];
        FMaterialLibrary Library;
        FMaterialImpl* Material = nullptr;
        std::map<std::string, FSurfSource> Sources;
        std::map<std::string, std::vector<unsigned short>> Indices;
        std::map<std::string, std::vector<unsigned long>> Matrices;
        std::map<std::string, FSurfContext> Contexts;
    };

    // G_MaterialSpecs of surfacematerials.cpp (file-local there).
    const FMaterialSpec FSurfCase::GSurfSpecs[19] = {
        {G_NameVeilbornMaterial, KindVeilborn},
        {G_NameVeilbornNoBendy, KindVeilborn},
        {G_NameHudIconMaterial, KindHudIcon},
        {G_NameHudAnimMaterial, KindHudAnim},
        {G_NameColorizeMaterial, KindColorize},
        {G_NameCloakMaterial, KindCloak},
        {G_NameWireMaterial, KindWire},
        {G_NameWireMaterial2, KindWire2},
        {G_NameExclusionZoneMaterial, KindExclusion},
        {G_NameGlassMaterial, KindGlass},
        {G_NameHighGlassMaterial, KindGlass},
        {G_NameGfGlassMaterial, KindGlass},
        {G_NameEnvMapMaterial, KindEnvMap},
        {G_NameBackdropMaterial, KindBackdrop},
        {G_NameStarsphereMaterial, KindBackdrop},
        {G_NameShadowMaterial, KindShadow},
        {G_NameEngineGlowMaterial, KindEngineGlow},
        {G_NameVisibleCounterMaterial, KindVisibleCounter},
        {G_NamePlanetWaterMaterial, KindPlanetWater},
    };
}

// FLAppearance.cpp's definitions the materials use (FLAppearance itself is not under test).
INeuronComponent* G_FlmEngineOuter = nullptr;

bool FlmLookupIdValue(int Id, void** Out)
{
    const std::map<int, unsigned long>::const_iterator Value = GSurf.Values.find(Id);
    const std::map<int, std::string>::const_iterator Name = GSurf.Names.find(Id);
    const bool bFound = Id != -1 && (Value != GSurf.Values.end() || Name != GSurf.Names.end());
    GSurf.Event(Format("\"ev\":\"ival\",\"id\":\"%d\",\"r\":\"%d\"", Id, bFound ? 0 : -1));
    if (!bFound)
        return false;
    if (Value != GSurf.Values.end())
        *Out = (void*)(uintptr_t)Value->second;
    else
        *Out = (void*)Name->second.c_str();
    return true;
}

I3DMathEngine* FlmCreateMathEngine()
{
    FMathDesc Desc = {0x0C, "3DMathEngine", nullptr};
    ICOManager* Manager = NEURONCOMPONENTS_Acquire();
    void* Created = nullptr;
    Manager->CreateInstance((AGGDESC*)(&Desc), &Created);
    return (I3DMathEngine*)Created;
}

GENRESULT FlmCreateAppearanceInstance(FAggDesc18* Desc, void** Instance)
{
    *Instance = nullptr;
    return GR_GENERIC;
}

void RunMaterialSurfaceSuite()
{
    Suite Out("material_surface.jsonl");
    if (!OracleLoadMathEngine())
    {
        fprintf(stderr, "material_surface: no 3DMathEngine\n");
        GFailed = true;
        return;
    }
    const std::vector<std::string> Lines = InputLines("material_surface.list");
    for (unsigned int Case = 0; Case < Lines.size(); ++Case)
    {
        FSurfCase Runner(&Out, Case);
        const std::vector<std::string> Ops = SplitText(Lines[Case], " ; ");
        for (unsigned int Op = 0; Op < Ops.size(); ++Op)
            Runner.Run(SplitText(Ops[Op], " "));
    }
}
