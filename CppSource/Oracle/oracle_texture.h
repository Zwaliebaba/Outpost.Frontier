// The recording renderer of the P4.4 suites (oracle_texture.cpp, oracle_big_image.cpp): the
// texture calls of IRenderPipeline under the contract of neuron_core::image::library, which
// crates/neuron-client/src/render/texture/pipeline.rs (ContractPipeline) implements the same way:
//   CreateTexture        fails (-1) for a zero width, height or level count, or more levels
//                        than 1 + log2(max(w, h)); else the next handle, counting from 1.
//   SetTextureLevelData  -2 for handle 0 or source format 0, -1 for a level the texture does
//                        not have, else 0. The bytes it reads are Pitch times the rows (block rows
//                        for DXT), cut at the end of the buffer the suite named with SetSource.
//   DestroyTexture       0.
//   GetTextureFormat     the format a texture was created with; for any other handle 0 and -1.
// Every call except GetTextureFormat writes one line to the suite, prefixed with the case.
// QueryInterface answers the texture library a suite gives it.

#pragma once

#include "oracle_support.h"

#include <stdint.h>
#include <string.h>

#include <map>
#include <string>
#include <utility>

#include "render/IRenderPipeline.h"
#include "render/shading.h"

class FOracleTexturePipeline final : public IRenderPipeline
{
public:
    FOracleTexturePipeline(oracle::Suite* InOut, unsigned int InCase, unsigned long InSquareOnly,
                           unsigned long InMaxWidth, unsigned long InMaxHeight)
        : Out(InOut), Case(InCase), SquareOnly(InSquareOnly), MaxWidth(InMaxWidth), MaxHeight(InMaxHeight),
          NextHandle(1), Library(nullptr), SourceBegin(nullptr), SourceEnd(nullptr)
    {
    }

    oracle::Suite* Out;
    unsigned int Case;
    unsigned long SquareOnly;
    unsigned long MaxWidth;
    unsigned long MaxHeight;
    unsigned long NextHandle;
    ITextureLibrary2* Library;
    // Format and levels of each created texture, by handle.
    std::map<unsigned long, std::pair<unsigned int, unsigned int> > Created;
    // The buffer level data comes from.
    const unsigned char* SourceBegin;
    const unsigned char* SourceEnd;

    void SetSource(const void* Begin, size_t Size)
    {
        SourceBegin = (const unsigned char*)Begin;
        SourceEnd = SourceBegin + Size;
    }

    void Event(const std::string& Fields)
    {
        Out->Line(oracle::Format("{\"case\":\"%u\",", Case) + Fields + "}");
    }

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Result) override
    {
        if (strcmp(Iid, Iids::TextureLibraryA) == 0 && Library != nullptr)
        {
            Library->AddRef();
            *Result = Library;
            return GR_OK;
        }
        *Result = nullptr;
        return GR_INTERFACE_UNSUPPORTED;
    }
    unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
    unsigned long NEURONCOMPONENTS_API Release() override { return 1; }

    GENRESULT NEURONCOMPONENTS_API QueryDeviceAbility(unsigned int Index, unsigned long* Result) override
    {
        *Result = Index == 5 ? SquareOnly : Index == 6 ? MaxWidth : Index == 7 ? MaxHeight : 0;
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API CreateTexture(int Width, int Height, unsigned int* PixelFormat,
                                                 unsigned int Levels, unsigned int Flags,
                                                 void** OutHandle) override
    {
        const unsigned long W = (unsigned long)Width;
        const unsigned long H = (unsigned long)Height;
        unsigned long Side = W > H ? W : H;
        unsigned long Chain = 0;
        while (Side != 0)
        {
            ++Chain;
            Side >>= 1;
        }
        const bool bOk = W != 0 && H != 0 && Levels != 0 && Levels <= Chain && Flags == 0;
        unsigned long Handle = 0;
        if (bOk)
        {
            Handle = NextHandle++;
            Created[Handle] = std::make_pair(*PixelFormat, Levels);
            *OutHandle = (void*)(uintptr_t)Handle;
        }
        Event(oracle::Format("\"ev\":\"create\",\"w\":\"%lu\",\"h\":\"%lu\",\"fmt\":\"%u\",\"levels\":\"%u\","
                             "\"handle\":\"%lu\",\"result\":\"%d\"",
                             W, H, *PixelFormat, Levels, Handle, bOk ? 0 : -1));
        return bOk ? GR_OK : GR_GENERIC;
    }

    GENRESULT NEURONCOMPONENTS_API DestroyTexture(void* Handle) override
    {
        Event(oracle::Format("\"ev\":\"destroy\",\"handle\":\"%lu\"", (unsigned long)(uintptr_t)Handle));
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API GetTextureFormat(void* Handle, int* OutFormat) override
    {
        const std::map<unsigned long, std::pair<unsigned int, unsigned int> >::const_iterator Found =
            Created.find((unsigned long)(uintptr_t)Handle);
        if (Found == Created.end())
        {
            *OutFormat = 0;
            return GR_GENERIC;
        }
        *OutFormat = (int)Found->second.first;
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API SetTextureLevelData(void* Handle, int Level, int SrcW, int SrcH,
                                                       unsigned int Pitch, unsigned int* SrcFormat,
                                                       const void* Pixels, void* A, void* Palette) override
    {
        const unsigned long Id = (unsigned long)(uintptr_t)Handle;
        int Result = 0;
        if (Id == 0 || *SrcFormat == 0)
            Result = -2;
        else
        {
            const std::map<unsigned long, std::pair<unsigned int, unsigned int> >::const_iterator Found =
                Created.find(Id);
            if (Found == Created.end() || (unsigned long)Level >= Found->second.second)
                Result = -1;
        }
        const bool bDxt = *SrcFormat >= 10 && *SrcFormat <= 14;
        const unsigned long Rows = bDxt ? ((unsigned long)SrcH + 3) >> 2 : (unsigned long)SrcH;
        const unsigned long long Need = (unsigned long long)Pitch * Rows;
        const unsigned char* Begin = (const unsigned char*)Pixels;
        unsigned long long Got = 0;
        if (Begin >= SourceBegin && Begin < SourceEnd)
        {
            const unsigned long long Left = (unsigned long long)(SourceEnd - Begin);
            Got = Need < Left ? Need : Left;
        }
        Event(oracle::Format("\"ev\":\"level\",\"handle\":\"%lu\",\"level\":\"%d\",\"w\":\"%d\",\"h\":\"%d\","
                             "\"pitch\":\"%u\",\"fmt\":\"%u\",\"got\":\"%llu\",\"fnv\":\"%s\",\"result\":\"%d\"",
                             Id, Level, SrcW, SrcH, Pitch, *SrcFormat, Got,
                             oracle::Fnv64(Begin, (size_t)Got).c_str(), Result));
        return Result;
    }

    // Everything else the texture code never calls.
        GENRESULT NEURONCOMPONENTS_API Startup(const char* SectionName) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ShutdownPipeline() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetPipelineState(int Id, int Value) override { return GR_GENERIC; }
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
        GENRESULT NEURONCOMPONENTS_API SetRenderState(
            D3DRENDERSTATETYPE State, int Value) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetRenderState(
            D3DRENDERSTATETYPE State, int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageState(
            int Stage, int Type, int Value) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageState(
            int Stage, int Type, int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageTransform(
            int Stage, const float* Matrix16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageTransform(
            int Stage, float* OutMatrix16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageTexture(
            int Stage, void* Handle) override { return GR_GENERIC; }
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

// oracle_texture.cpp: a texture library (FTextureLibrary of Shading.dll, compiled into the
// harness) with the built-in format maps, for ximage's PickGpuFormat.
ITextureLibrary2* OracleDefaultTextureLibrary();
