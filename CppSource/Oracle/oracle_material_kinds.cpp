// P4.5 (second increment): the 22 material kinds of Shading.dll (FMaterial, Material.cpp) and
// BatchedMaterial, through the reference's own code, against recording services. The Rust side
// is crates/neuron-client/tests/support/material_kinds.rs with the same contract; the input is
// crates/neuron-client/tests/fixtures/oracle/material_kinds.list from make_material_fixtures.py.
//
// A material is made with CreateMaterialInstance (what Shading.cpp's factories call) and
// initialized with a component that answers the texture library, the renderer, IRPDraw, the
// vertex-buffer manager and the material batcher; "batched" makes an FBatchedMaterial around one
// (its inner controller and inner material set as CreateInnerMaterial leaves them). Every call a
// material makes on them writes a line {"case","ev",...} in call order:
//
//   SetMaterial               "mat","m": the 17 words in hex
//   SetRenderState            "rs","s","v"            GetRenderState  "getrs","s","v": the value
//                             set in this case, else the preset ("rs" operation), else 0
//   SetTextureStageState      "tss","st","t","v"      SetTextureStageTexture  "tex","st","h"
//   SetTextureStageTransform  "txf","st","m": 16 words
//   GetTextureFormat          "fmt","h","r","f": the format of the texture table's handle (r 0)
//                             or r -1 with f "-" (output untouched)
//   VerifyState               "verify","r": the next of the queue the "verify a,b,..."
//                             operation set, 0 when it is empty
//   GetPipelineState          "state","id","v": the "mode" value (1 at first), "-" with "mode
//                             none" (nothing written)
//   SetWorld, SetModelview    "world"/"modelview","m": 12 words
//   DrawIndexedPrimitiveVb    "dvb","prim","vb","base","n","idx": the indices read (decimal,
//                             comma-separated),"r": the "dvb" value (0 at first)
//   IRPDraw::DrawIndexedPrimitive  "rpdraw","prim","min","n","start","count","r": "rpdraw" value
//   LockVertexMemory          "lock","fvf","n","r": -1 with "lockfail 1"; else buffer 900, base
//                             vertex 50 then advancing by the count, the bytes zeroed (stride by
//                             ShadingInternal.h's FvfStride)
//   CopyVertexData            "copy","fvf","start","n": vertex i of the copy gets the bytes
//                             0x40 + 7 i + k (k the byte in the vertex, mod 256)
//   UnlockVertexMemory        "unlock","base","bytes": hex
//   FindTextureByCrc          "find","name": the query's name in hex,"r": 0 for the texture
//                             table's entry of that CRC, else -1
//   GetFrameCount             "frames","name","n"
//   CreatePlaybackGroup       "play","name","id","r": ids from 1, -1 and -1 for an entry made
//                             with playfail 1
//   GetFrame                  "frame","id","f","h","r": the entry's handle (100 more for frame
//                             -1 of an entry with more than one frame), u0 v0 u1 v1 = 0.25 0 1
//                             0.5; -1 and nothing written for an unknown id
//   ReleaseTextureRecord      "trel","name"           DecRefById  "decref","id"
//   QueryInterface(IID_IMatAnimLibrary) on the renderer  "qianim","r": 0 after "animlib"
//   CreateAnimationInstance   "anim","name","crc","id","r": ids from 1 for an "anim" entry of
//                             that CRC, else -1 and the output untouched
//   IImaInstance::FillUvMatrix  "uv","id","r": the entry's flag; the matrix is the identity with
//                             [8] 0.25 and [9] -0.5      IImaInstance::Release  "animrel","id"
//   IMaterialBatcher2::SelectBin  "selbin","bin"
//   IMaterialBatcher2::Submit "bsubmit","prim","start","count","ibs","span","depth": hex,
//                             "flags": hex
//
// and every operation writes {"case","op",...} after the lines of its calls. Floats are the
// eight hex digits of the f32. The engine of Field20 is never given (the Rust tests cover it).

#include "oracle_support.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "../Shading/Material.h"
#include "../Shading/MaterialLibrary.h"

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

    struct FKindTexture
    {
        std::string Name;
        unsigned long Crc;
        int Frames;
        unsigned long Handle;
        int Format;
        bool bPlayFail;
    };

    struct FKindAnim
    {
        std::string Name;
        unsigned long Crc;
        bool bFills;
    };

    struct FKindSource
    {
        std::vector<unsigned long> Pos;
        std::vector<unsigned long> Normal;
        std::vector<unsigned long> Diffuse;
        std::vector<unsigned long> Tex0;
        std::vector<unsigned long> Index;
        FRpVertexSrcDesc Desc;
    };

    struct FKindContext
    {
        FVbListing Vb;
        FIbScratch Ib;
        FMaterialSubmitContext Context;
    };

    class FKindAnimInstance;

    // The recording state of a case.
    struct FKindState
    {
        Suite* Out = nullptr;
        unsigned int Case = 0;
        std::map<int, unsigned long> RenderStates;
        std::vector<int> VerifyQueue;
        bool bModeNone = false;
        int Mode = 1;
        int RpDrawResult = 0;
        int DvbResult = 0;
        bool bLockFail = false;
        unsigned long NextBase = 50;
        bool bAnimLibrary = false;
        std::vector<FKindTexture> Textures;
        std::vector<FKindAnim> Anims;
        std::map<int, size_t> Playbacks;
        int NextPlayback = 1;
        int NextAnim = 1;
        std::vector<FKindAnimInstance*> Instances;

        void Event(const std::string& Fields)
        {
            Out->Line(Format("{\"case\":\"%u\",", Case) + Fields + "}");
        }

        const FKindTexture* TextureByHandle(unsigned long Handle) const
        {
            for (size_t Index = 0; Index < Textures.size(); ++Index)
            {
                if (Textures[Index].Handle == Handle)
                    return &Textures[Index];
            }
            return nullptr;
        }
    };

    FKindState GKind;

    class FKindAnimInstance final : public IImaInstance
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
            GKind.Event(Format("\"ev\":\"animrel\",\"id\":\"%d\"", Id));
            return 0;
        }
        FMaKeyBlock* NEURONCOMPONENTS_API GetKeyBlock() override { return nullptr; }
        void NEURONCOMPONENTS_API ResetTime() override {}
        void NEURONCOMPONENTS_API Tick(float Dt) override {}
        unsigned char NEURONCOMPONENTS_API FillUvMatrix(float* Out) override
        {
            GKind.Event(Format("\"ev\":\"uv\",\"id\":\"%d\",\"r\":\"%d\"", Id, bFills ? 1 : 0));
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

    class FKindAnimLibrary final : public IMatAnimLibrary
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
            for (size_t Index = 0; Index < GKind.Anims.size(); ++Index)
            {
                if (GKind.Anims[Index].Crc != Crc)
                    continue;
                FKindAnimInstance* Instance = new FKindAnimInstance();
                Instance->Id = GKind.NextAnim++;
                Instance->bFills = GKind.Anims[Index].bFills;
                GKind.Instances.push_back(Instance);
                *OutInstance = Instance;
                GKind.Event(Format("\"ev\":\"anim\",\"name\":\"%s\",\"crc\":\"%08lx\",\"id\":\"%d\",\"r\":\"0\"",
                                   NameHex.c_str(), Crc, Instance->Id));
                return GR_OK;
            }
            GKind.Event(Format("\"ev\":\"anim\",\"name\":\"%s\",\"crc\":\"%08lx\",\"id\":\"-\",\"r\":\"-1\"",
                               NameHex.c_str(), Crc));
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API TickAnimationInstances(int Filter, float Dt) override { return GR_GENERIC; }
    };

    FKindAnimLibrary GAnimLibrary;

    class FKindPipeline final : public IRenderPipeline
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            if (strcmp(Iid, Iids::MatAnimLibrary) == 0)
            {
                GKind.Event(Format("\"ev\":\"qianim\",\"r\":\"%d\"", GKind.bAnimLibrary ? 0 : -1));
                if (GKind.bAnimLibrary)
                {
                    *Out = (IMatAnimLibrary*)&GAnimLibrary;
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
            GKind.Event("\"ev\":\"mat\",\"m\":\"" + Words(Material, 17) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetRenderState(D3DRENDERSTATETYPE State, int Value) override
        {
            GKind.RenderStates[(int)State] = (unsigned long)Value;
            GKind.Event(Format("\"ev\":\"rs\",\"s\":\"%u\",\"v\":\"%lu\"", (unsigned int)State,
                               (unsigned long)Value));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetRenderState(D3DRENDERSTATETYPE State, int* Out) override
        {
            const std::map<int, unsigned long>::const_iterator Found = GKind.RenderStates.find((int)State);
            const unsigned long Value = Found == GKind.RenderStates.end() ? 0 : Found->second;
            *Out = (int)Value;
            GKind.Event(Format("\"ev\":\"getrs\",\"s\":\"%u\",\"v\":\"%lu\"", (unsigned int)State, Value));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageState(int Stage, int Type, int Value) override
        {
            GKind.Event(Format("\"ev\":\"tss\",\"st\":\"%d\",\"t\":\"%d\",\"v\":\"%lu\"", Stage, Type,
                               (unsigned long)Value));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageTexture(int Stage, void* Handle) override
        {
            GKind.Event(Format("\"ev\":\"tex\",\"st\":\"%d\",\"h\":\"%lu\"", Stage,
                               (unsigned long)(uintptr_t)Handle));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageTransform(int Stage, const float* Matrix16) override
        {
            GKind.Event(Format("\"ev\":\"txf\",\"st\":\"%d\",\"m\":\"", Stage) + Words(Matrix16, 16) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetTextureFormat(void* Handle, int* OutFormat) override
        {
            const unsigned long Id = (unsigned long)(uintptr_t)Handle;
            const FKindTexture* Texture = GKind.TextureByHandle(Id);
            if (Texture == nullptr)
            {
                GKind.Event(Format("\"ev\":\"fmt\",\"h\":\"%lu\",\"r\":\"-1\",\"f\":\"-\"", Id));
                return GR_GENERIC;
            }
            *OutFormat = Texture->Format;
            GKind.Event(Format("\"ev\":\"fmt\",\"h\":\"%lu\",\"r\":\"0\",\"f\":\"%d\"", Id, Texture->Format));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API VerifyState() override
        {
            int Result = 0;
            if (!GKind.VerifyQueue.empty())
            {
                Result = GKind.VerifyQueue.front();
                GKind.VerifyQueue.erase(GKind.VerifyQueue.begin());
            }
            GKind.Event(Format("\"ev\":\"verify\",\"r\":\"%d\"", Result));
            return Result;
        }
        GENRESULT NEURONCOMPONENTS_API GetPipelineState(int Id, int* Out) override
        {
            if (GKind.bModeNone)
            {
                GKind.Event(Format("\"ev\":\"state\",\"id\":\"%d\",\"v\":\"-\"", Id));
                return GR_OK;
            }
            *Out = GKind.Mode;
            GKind.Event(Format("\"ev\":\"state\",\"id\":\"%d\",\"v\":\"%d\"", Id, GKind.Mode));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetWorld(const float* M12) override
        {
            GKind.Event("\"ev\":\"world\",\"m\":\"" + Words(M12, 12) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetModelview(const float* M12) override
        {
            GKind.Event("\"ev\":\"modelview\",\"m\":\"" + Words(M12, 12) + "\"");
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
            GKind.Event(Format("\"ev\":\"dvb\",\"prim\":\"%lu\",\"vb\":\"%lu\",\"base\":\"%u\",\"n\":\"%u\",\"idx\":\"",
                               Prim, (unsigned long)(uintptr_t)Vb, BaseVertex, NumVerts) +
                        List + Format("\",\"r\":\"%d\"", GKind.DvbResult));
            return GKind.DvbResult;
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
        GENRESULT NEURONCOMPONENTS_API GetMaterial(D3DMATERIAL8* Material) override { return GR_GENERIC; }
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
            return GR_GENERIC;
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
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API AddLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API RemoveLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UpdateLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetWorldN(int WorldIndex, const float* M12) override { return GR_GENERIC; }
    };

    class FKindDraw final : public IRPDraw
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
            GKind.Event(Format("\"ev\":\"rpdraw\",\"prim\":\"%lu\",\"min\":\"%u\",\"n\":\"%u\",\"start\":\"%u\","
                               "\"count\":\"%u\",\"r\":\"%d\"",
                               Prim, MinIndex, NumVertices, StartIndex, Count, GKind.RpDrawResult));
            return GKind.RpDrawResult;
        }
    };

    class FKindVbMgr final : public IVertexBufferManager
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
            if (GKind.bLockFail)
            {
                GKind.Event(Format("\"ev\":\"lock\",\"fvf\":\"%u\",\"n\":\"%u\",\"r\":\"-1\"", Fvf, Count));
                return GR_GENERIC;
            }
            GKind.Event(Format("\"ev\":\"lock\",\"fvf\":\"%u\",\"n\":\"%u\",\"r\":\"0\"", Fvf, Count));
            Memory.assign((size_t)FvfStride(Fvf) * Count, 0);
            Out->Listing.VertexBuffer = (void*)(uintptr_t)900;
            Out->Listing.Fvf = Fvf;
            Out->Listing.BaseVertex = GKind.NextBase;
            GKind.NextBase += Count;
            Out->Vertices = Memory.data();
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API UnlockVertexMemory(FVertexMemoryLock* Out) override
        {
            GKind.Event(Format("\"ev\":\"unlock\",\"base\":\"%lu\",\"bytes\":\"", Out->Listing.BaseVertex) +
                        Hex(Memory.data(), Memory.size()) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API UnsupportedManagerOperation() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CopyVertexData(void* Dest, unsigned int Fvf, const FRpVertexSrcDesc* Source,
                                                      unsigned int StartVertex, unsigned int Count) override
        {
            GKind.Event(Format("\"ev\":\"copy\",\"fvf\":\"%u\",\"start\":\"%u\",\"n\":\"%u\"", Fvf, StartVertex,
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

    class FKindTextureLibrary final : public ITextureLibrary2
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
            for (size_t Index = 0; Index < GKind.Textures.size(); ++Index)
            {
                if (GKind.Textures[Index].Crc == Crc)
                {
                    GKind.Event(Format("\"ev\":\"find\",\"name\":\"%s\",\"r\":\"0\"", HexC(Name).c_str()));
                    *OutTex = &GKind.Textures[Index];
                    return GR_OK;
                }
            }
            GKind.Event(Format("\"ev\":\"find\",\"name\":\"%s\",\"r\":\"-1\"", HexC(Name).c_str()));
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API GetFrameCount(void* Record, int* Out) override
        {
            const FKindTexture* Texture = (const FKindTexture*)Record;
            GKind.Event(Format("\"ev\":\"frames\",\"name\":\"%s\",\"n\":\"%d\"", TextHex(Texture->Name).c_str(),
                               Texture->Frames));
            *Out = Texture->Frames;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API CreatePlaybackGroup(void* SourceRecord, int* OutId) override
        {
            const FKindTexture* Texture = (const FKindTexture*)SourceRecord;
            *OutId = -1;
            if (Texture->bPlayFail)
            {
                GKind.Event(Format("\"ev\":\"play\",\"name\":\"%s\",\"id\":\"-1\",\"r\":\"-1\"",
                                   TextHex(Texture->Name).c_str()));
                return GR_GENERIC;
            }
            const int Id = GKind.NextPlayback++;
            GKind.Playbacks[Id] = (size_t)(Texture - GKind.Textures.data());
            *OutId = Id;
            GKind.Event(Format("\"ev\":\"play\",\"name\":\"%s\",\"id\":\"%d\",\"r\":\"0\"",
                               TextHex(Texture->Name).c_str(), Id));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetFrame(int Id, int Frame, unsigned long* Out) override
        {
            const std::map<int, size_t>::const_iterator Found = GKind.Playbacks.find(Id);
            if (Found == GKind.Playbacks.end())
            {
                GKind.Event(Format("\"ev\":\"frame\",\"id\":\"%d\",\"f\":\"%d\",\"h\":\"-\",\"r\":\"-1\"", Id, Frame));
                return GR_GENERIC;
            }
            const FKindTexture& Texture = GKind.Textures[Found->second];
            const unsigned long Handle = Texture.Handle + (Frame == -1 && Texture.Frames > 1 ? 100 : 0);
            Out[0] = Handle;
            Out[1] = 0x3E800000;
            Out[2] = 0;
            Out[3] = 0x3F800000;
            Out[4] = 0x3F000000;
            GKind.Event(Format("\"ev\":\"frame\",\"id\":\"%d\",\"f\":\"%d\",\"h\":\"%lu\",\"r\":\"0\"", Id, Frame,
                               Handle));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API ReleaseTextureRecord(void* Record) override
        {
            const FKindTexture* Texture = (const FKindTexture*)Record;
            GKind.Event(Format("\"ev\":\"trel\",\"name\":\"%s\"", TextHex(Texture->Name).c_str()));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API DecRefById(int Id) override
        {
            GKind.Event(Format("\"ev\":\"decref\",\"id\":\"%d\"", Id));
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
            return GR_GENERIC;
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

    class FKindBatcher final : public IMaterialBatcher2
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API SelectBin(unsigned long Index) override
        {
            GKind.Event(Format("\"ev\":\"selbin\",\"bin\":\"%lu\"", Index));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Submit(IMaterial* Material, void* Context, unsigned long PrimType,
                                              unsigned long Start, unsigned long Count, unsigned long IbStart,
                                              unsigned long IndexSpan, void* Unused, unsigned long Flags) override
        {
            GKind.Event(Format("\"ev\":\"bsubmit\",\"prim\":\"%lu\",\"start\":\"%lu\",\"count\":\"%lu\",\"ibs\":\"%lu\","
                               "\"span\":\"%lu\",\"depth\":\"%08lx\",\"flags\":\"%lx\"",
                               PrimType, Start, Count, IbStart, IndexSpan, (unsigned long)(uintptr_t)Unused, Flags));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API ClearRules() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API AddRule(unsigned long Kind, const char* Pattern) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API RemoveRule(unsigned long Kind, const char* Pattern) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetBinSortKind(unsigned long Index, void* Value) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetBinOrder(unsigned long Index, long Key) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API FlushBin(unsigned long Index, unsigned char Clear) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API FlushAll(unsigned char Clear) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ClearBins() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API NoOp(void* Arg) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetEvents(void* NewValue) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetEvents(void* Arg) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetRecordTag(void* Value) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetRecordTag(void* Arg) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetRecordTagContext(void* Value) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetRecordTagContext(void* Arg) override { return GR_GENERIC; }
    };

    FKindPipeline GKindPipeline;
    FKindDraw GKindDraw;
    FKindVbMgr GKindVbMgr;
    FKindTextureLibrary GKindTextures;
    FKindBatcher GKindBatcher;

    // The component Initialize asks.
    class FKindComponent final : public INeuronComponent
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            if (strcmp(Iid, Iids::TextureLibraryA) == 0)
                *Out = (ITextureLibrary2*)&GKindTextures;
            else if (strcmp(Iid, Iids::RenderPipeline8B) == 0)
                *Out = (IRenderPipeline*)&GKindPipeline;
            else if (strcmp(Iid, Iids::RPDraw) == 0)
                *Out = (IRPDraw*)&GKindDraw;
            else if (strcmp(Iid, Iids::VertexBufferManager) == 0)
                *Out = (IVertexBufferManager*)&GKindVbMgr;
            else if (strcmp(Iid, Iids::MaterialBatcher) == 0)
                *Out = (IMaterialBatcher2*)&GKindBatcher;
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

    FKindComponent GKindComponent;

    std::vector<unsigned long> FloatWords(const std::string& Text)
    {
        return HexWords(Text);
    }

    void SetStream(FRpVertexStream& Stream, std::vector<unsigned long>& Data, unsigned int Size,
                   const std::vector<unsigned long>& Index, unsigned int Count)
    {
        memset(&Stream, 0, sizeof(Stream));
        if (Data.empty())
            return;
        Stream.Base = (const unsigned char*)Data.data();
        Stream.ByteSize = Size;
        Stream.Stride = Size;
        Stream.IndexArray = Index.empty() ? nullptr : Index.data();
        Stream.Count = Count;
    }

    class FKindCase
    {
    public:
        FKindCase(Suite* InOut, unsigned int InCase)
        {
            GKind = FKindState();
            GKind.Out = InOut;
            GKind.Case = InCase;
        }

        ~FKindCase()
        {
            for (size_t Index = 0; Index < GKind.Instances.size(); ++Index)
                delete GKind.Instances[Index];
            GKind.Instances.clear();
        }

        void Op(const std::string& Fields)
        {
            GKind.Out->Line(Format("{\"case\":\"%u\",\"op\":", GKind.Case) + Fields + "}");
        }

        FMaterialSubmitContext* Context(const std::string& Name)
        {
            if (Name == "-")
                return nullptr;
            return &Contexts[Name].Context;
        }

        void State()
        {
            FMaterial* M = Plain;
            std::string Tex;
            for (unsigned long Index = 0; Index < M->TexCount; ++Index)
            {
                const FTexBlock& B = M->TexBlocks[Index];
                // The frame rectangle is not written: GetFrame writes it, nothing initializes it.
                Tex += Format(Index == 0 ? "%d:%lu:%u" : ",%d:%lu:%u", B.TextureId, B.Handle,
                              (unsigned int)B.bSkipResolve);
            }
            Op(Format("\"state\",\"blend\":\"%u\",\"test\":\"%u\",\"ready\":\"%u\",\"passes\":\"%lu\","
                      "\"second\":\"%u\",\"multi\":\"%u\",\"sen\":\"%u\",\"sver\":\"%u\",\"anim\":\"%u\","
                      "\"tex\":\"",
                      (unsigned int)M->bAlphablend, (unsigned int)M->bAlphatest, (unsigned int)M->bAtmosphereReady,
                      M->DetailPassCount, (unsigned int)M->bDetailSecondStageVerified,
                      (unsigned int)M->bDetailMultiPassEnabled, (unsigned int)M->bSecondaryTextureEnabled,
                      (unsigned int)M->bSecondaryTextureVerified, M->MatAnim != nullptr ? 1u : 0u) +
               Tex + "\"");
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
            if (K == "globals")
            {
                for (unsigned long Index = 0; Index < 3; ++Index)
                {
                    const unsigned long Value = strtoul(F[1 + Index].c_str(), nullptr, 10);
                    Library.SetGlobalRecord(Index, &Value);
                }
            }
            else if (K == "tex")
            {
                FKindTexture Texture;
                Texture.Name = std::string(CStr(Unhex(F[1])));
                Texture.Crc = NEURONCOMPONENTS_CRC::GetCRC32(Texture.Name.c_str());
                Texture.Frames = (int)Dec(F[2]);
                Texture.Handle = (unsigned long)Dec(F[3]);
                Texture.Format = (int)Dec(F[4]);
                Texture.bPlayFail = Dec(F[5]) != 0;
                GKind.Textures.push_back(Texture);
            }
            else if (K == "animlib")
                GKind.bAnimLibrary = true;
            else if (K == "anim")
            {
                FKindAnim Anim;
                Anim.Name = std::string(CStr(Unhex(F[1])));
                Anim.Crc = NEURONCOMPONENTS_CRC::GetCRC32(Anim.Name.c_str());
                Anim.bFills = Dec(F[2]) != 0;
                GKind.Anims.push_back(Anim);
            }
            else if (K == "rs")
                GKind.RenderStates[(int)Dec(F[1])] = (unsigned long)Dec(F[2]);
            else if (K == "mode")
            {
                GKind.bModeNone = F[1] == "none";
                if (!GKind.bModeNone)
                    GKind.Mode = (int)Dec(F[1]);
            }
            else if (K == "rpdraw")
                GKind.RpDrawResult = (int)Dec(F[1]);
            else if (K == "dvb")
                GKind.DvbResult = (int)Dec(F[1]);
            else if (K == "lockfail")
                GKind.bLockFail = Dec(F[1]) != 0;
            else if (K == "verify" && F.size() == 2)
            {
                GKind.VerifyQueue.clear();
                const std::vector<std::string> Parts = SplitText(F[1], ",");
                for (size_t Index = 0; Index < Parts.size(); ++Index)
                    GKind.VerifyQueue.push_back((int)Dec(Parts[Index]));
            }
            else if (K == "vsrc")
            {
                FKindSource& S = Sources[F[1]];
                const unsigned int Count = (unsigned int)Dec(F[4]);
                S.Pos = FloatWords(F[5]);
                S.Normal = FloatWords(F[6]);
                S.Diffuse = HexWords(F[7]);
                S.Tex0 = FloatWords(F[8]);
                S.Index.clear();
                if (F[9] != "-")
                {
                    const std::vector<std::string> Parts = SplitText(F[9], ",");
                    for (size_t Index = 0; Index < Parts.size(); ++Index)
                        S.Index.push_back((unsigned long)Dec(Parts[Index]));
                }
                memset(&S.Desc, 0, sizeof(S.Desc));
                S.Desc.Flags = (unsigned int)Dec(F[2]);
                SetStream(S.Desc.Position, S.Pos, 12, S.Index, Count);
                SetStream(S.Desc.Normal, S.Normal, 12, S.Index, Count);
                SetStream(S.Desc.Diffuse, S.Diffuse, 4, S.Index, Count);
                SetStream(S.Desc.Tex[0], S.Tex0, 8, S.Index, Count);
                S.Desc.SrcFvf = (unsigned int)Dec(F[3]);
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
                FKindContext& C = Contexts[F[1]];
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
            }
            else if (K == "new")
            {
                Plain = CreateMaterialInstance(F[1].c_str());
                Plain->InitializeFromDescriptor(nullptr);
                Material = Plain;
                Batched = nullptr;
                const GENRESULT Result = Material->Initialize(&GKindComponent);
                Op(Format("\"new\",\"kind\":\"%s\",\"r\":\"%d\"", F[1].c_str(), (int)Result));
            }
            else if (K == "batched")
            {
                Plain = CreateMaterialInstance(F[1].c_str());
                Plain->InitializeFromDescriptor(nullptr);
                Batched = new FBatchedMaterial();
                Batched->InnerController = (INeuronComponent*)&Plain->Inner;
                Batched->Inner = Plain;
                Material = Batched;
                const GENRESULT Result = Material->Initialize(&GKindComponent);
                Op(Format("\"batched\",\"kind\":\"%s\",\"r\":\"%d\"", F[1].c_str(), (int)Result));
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
            else if (K == "state")
                State();
            else if (K == "resolve")
            {
                const GENRESULT Result = Material->ResolveTextures(0.0f);
                Op(Format("\"resolve\",\"r\":\"%d\"", (int)Result));
            }
            else if (K == "bind" || K == "teardown")
            {
                FMaterialSubmitContext* C = Context(F[1]);
                const GENRESULT Result = K == "bind" ? Material->Bind(C) : Material->Teardown(C);
                Op(Format("\"%s\",\"r\":\"%d\"", K.c_str(), (int)Result));
            }
            else if (K == "submit" || K == "bsubmit")
            {
                FMaterialSubmitContext* C = Context(F[1]);
                const GENRESULT Result =
                    Material->Submit(C, (unsigned long)Dec(F[2]), (unsigned long)Dec(F[3]),
                                     (unsigned long)Dec(F[4]), (unsigned long)Dec(F[5]),
                                     (unsigned long)Dec(F[6]), HexWord(F[7]));
                Op(Format("\"%s\",\"r\":\"%d\"", K.c_str(), (int)Result));
            }
            else if (K == "passes")
            {
                int Passes = -7;
                const GENRESULT Result = Material->GetPassCount(&Passes);
                Op(Format("\"passes\",\"r\":\"%d\",\"v\":\"%d\"", (int)Result, Passes));
            }
            else if (K == "params")
                Params();
            else if (K == "bin")
            {
                void* Bin = nullptr;
                Batched->Batched.GetBatchBin(&Bin);
                Op(Format("\"bin\",\"v\":\"%lu\"", (unsigned long)(uintptr_t)Bin));
            }
            else if (K == "setbin")
            {
                Batched->Batched.SetBatchBin((void*)(uintptr_t)Dec(F[1]));
                Op("\"setbin\"");
            }
            else if (K == "release")
            {
                Material->Release();
                Material = nullptr;
                Plain = nullptr;
                Batched = nullptr;
                Op("\"release\"");
            }
            else
            {
                fprintf(stderr, "material_kinds: unknown operation %s\n", K.c_str());
                GFailed = true;
            }
        }

    private:
        FMaterialLibrary Library;
        IMaterial* Material = nullptr;
        FMaterial* Plain = nullptr;
        FBatchedMaterial* Batched = nullptr;
        std::map<std::string, FKindSource> Sources;
        std::map<std::string, std::vector<unsigned short>> Indices;
        std::map<std::string, std::vector<unsigned long>> Matrices;
        std::map<std::string, FKindContext> Contexts;
    };
}

void RunMaterialKindSuite()
{
    Suite Out("material_kinds.jsonl");
    const std::vector<std::string> Lines = InputLines("material_kinds.list");
    for (unsigned int Case = 0; Case < Lines.size(); ++Case)
    {
        FKindCase Runner(&Out, Case);
        const std::vector<std::string> Ops = SplitText(Lines[Case], " ; ");
        for (unsigned int Op = 0; Op < Ops.size(); ++Op)
            Runner.Run(SplitText(Ops[Op], " "));
    }
}
