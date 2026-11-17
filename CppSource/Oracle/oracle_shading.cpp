// Oracle suites and data dumps of work package P1.7: texture levels, texture libraries and
// materials of Shading.dll (neuron-core::image and neuron-core::material_data). The Rust test
// crates/neuron-core/tests/oracle_shading.rs writes the same lines; the inputs come from
// crates/neuron-core/tests/fixtures/oracle/make_shading_fixtures.py.
//
// Shading.dll exports nothing but DllMain, so its source is compiled in: TextureLibrary.cpp is
// included below, so that the TGA decoder, the format maps and the TGA mapping records of its
// anonymous namespace can be called; MaterialAnimation.cpp is included by
// oracle_shading_anim.cpp for ParseMaKeys; Material.cpp, MaterialLibrary.cpp,
// MaterialBatcher.cpp and MaterialRegex.cpp are compiled beside them. Shading.cpp (DllMain and
// the factories) is not: materials are made with CreateMaterialInstance, which is what the
// factories call.
//
// The texture library talks to a recording renderer instead of a device (CI has no GPU). It
// implements the contract stated in neuron_core::image::library:
//   CreateTexture        fails (-1) for a zero width, height or level count, or more levels
//                        than 1 + log2(max(w, h)); else the next handle, counting from 1.
//   SetTextureLevelData  -2 for handle 0 or source format 0 (as DX8RENDERER), -1 for a level
//                        the texture does not have, else 0.
//   LoadCubemap          opens and maps the leaf (-8 when it cannot be opened, -1 when it
//                        cannot be mapped); succeeds with the next handle when D3DX describes
//                        a DDS cube texture, else -1.
//   GetImageInfo         D3DXGetImageInfoFromFileInMemory of the statically linked D3DX 8.1,
//                        as DX8RENDERER calls it: this part is the reference's own.
// Every call is written as an event line. What D3DX would do with the pixels on a device
// (D3DXLoadSurfaceFromMemory, D3DXCreateCubeTextureFromFileInMemory) is not checked here.
//
// LoadTextureGroup, FMaterialLibrary::LoadFromFileSystem and LoadAnimationGroup report nothing
// per child, so their enumeration loops are repeated here around the reference's per-child
// calls (LoadTextureRecord and CreatePlaybackGroup; CreateMaterialInstance, Initialize,
// LoadFromFileSystem and GetParameterValue; ParseMaKeys). The [MaterialMap] remapping and the
// batcher are not involved: a material is made from the type its Type leaf names.
//
// The UTF archives are opened through the component manager with the UTF implementation, as
// oracle_vfs.cpp's UTF factory opens them; ReadFile's factories are registered by
// RunVfsSuites and RunVfsDataDump, which run first.

#include "oracle_support.h"

#include <set>

#include "../Shading/TextureLibrary.cpp"
#include "../Shading/Material.h"

using namespace oracle;

// oracle_shading_anim.cpp: MaterialAnimation.cpp's ParseMaKeys.
void OracleParseMaKeys(IFileSystem* FileSystem, FMaKeyBlock* Block);

namespace
{
    // The names of kMaterialTypes (Material.cpp), in order.
    const char* const kShadingKinds[] = {
        "DcDt", "DcDtEc", "DcDtOcOt", "DcDtEcOcOt", "DcDtTwo", "DcDtEcTwo", "DcDtOcOtTwo",
        "DcDtEcOcOtTwo", "DcDtEt", "EcEt", "EcEtTwo", "AtmosphereMaterial", "Nebula", "NebulaTwo",
        "NullMaterial", "LightMap", "DetailMapMaterial", "DetailMap2Dm1Msk2PassMaterial",
        "IllumDetailMapMaterial", "Masked2DetailMapMaterial", "BtDetailMapMaterial",
        "BtDetailMapTwoMaterial"};

    // The texture library settings of a suite line: generic slots 0, 2, 3 and 4 and device
    // ability 5.
    struct FShadingSettings
    {
        unsigned long LodLoad;
        unsigned long SquarePolicy;
        unsigned long Pow2Policy;
        unsigned long LodMin;
        unsigned long SquareOnly;
    };

    std::string SettingsText(const FShadingSettings& Settings)
    {
        return Format("%lu,%lu,%lu,%lu,%lu", Settings.LodLoad, Settings.SquarePolicy, Settings.Pow2Policy,
                      Settings.LodMin, Settings.SquareOnly);
    }

    unsigned int BitsAt(const void* Address)
    {
        unsigned int Bits;
        memcpy(&Bits, Address, sizeof(Bits));
        return Bits;
    }

    // D3DX on bytes it may not accept; an exception counts as a failure.
    HRESULT ShadingImageInfo(const void* Data, unsigned int Size, D3DXIMAGE_INFO* Info)
    {
        memset(Info, 0, sizeof(*Info));
        __try
        {
            return D3DXGetImageInfoFromFileInMemory(Data, Size, Info);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return E_FAIL;
        }
    }

    bool IsDdsInfo(HRESULT Result, const D3DXIMAGE_INFO& Info)
    {
        return Result >= 0 && Info.ImageFileFormat == D3DXIFF_DDS;
    }

    std::string InfoFields(HRESULT Result, const D3DXIMAGE_INFO& Info)
    {
        if (!IsDdsInfo(Result, Info))
            return "\"dds\":\"0\"";
        return Format("\"dds\":\"1\",\"w\":\"%u\",\"h\":\"%u\",\"d\":\"%u\",\"mips\":\"%u\",\"fmt\":\"%08x\","
                      "\"rtype\":\"%u\"",
                      Info.Width, Info.Height, Info.Depth, Info.MipLevels, (unsigned int)Info.Format,
                      (unsigned int)Info.ResourceType);
    }

    class FRecordingPipeline;

    class FRecordingTexture final : public IRPTexture
    {
    public:
        FRecordingPipeline* Owner;

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API PrintScreen(void* ParentFileSystem, const char* FileName) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API LoadTexture(IFileSystem* FileSystem, const char* FileName,
                                                   void** OutHandle) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API LoadSurfaceData(void* Handle, int Level, unsigned int SrcSize,
                                                       const void* SrcData) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API GetImageInfo(_D3DXIMAGE_INFO* Info, unsigned int SrcSize,
                                                    const void* SrcData) override;
        GENRESULT NEURONCOMPONENTS_API LoadCubemap(IFileSystem* FileSystem, const char* FileName,
                                                   void** OutHandle) override;
    };

    class FRecordingPipeline final : public IRenderPipeline
    {
    public:
        FRecordingPipeline(Suite* InOut, unsigned long InSquareOnly)
            : Out(InOut), SquareOnly(InSquareOnly), NextHandle(1), PendingView(nullptr), PendingSize(0),
              MipsView(nullptr), MipsSize(0)
        {
            Texture.Owner = this;
        }

        Suite* Out;
        unsigned long SquareOnly;
        unsigned long NextHandle;
        // Levels of each created texture, by handle.
        std::map<unsigned long, unsigned long> CreatedLevels;
        // The MIPS view the last GetImageInfo described as DDS; CreateTexture moves it to
        // MipsView, which marks the uploads that follow as slices of that view. The TGA loader
        // creates its texture without GetImageInfo, so its uploads are decoded pixels.
        const unsigned char* PendingView;
        unsigned long PendingSize;
        const unsigned char* MipsView;
        unsigned long MipsSize;
        FRecordingTexture Texture;

        void Event(const std::string& Line) { Out->Line(Line); }

        void ResetViews()
        {
            PendingView = nullptr;
            PendingSize = 0;
            MipsView = nullptr;
            MipsSize = 0;
        }

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Result) override
        {
            if (IidEqual(Iid, Iids::RPTexture))
            {
                *Result = (IRPTexture*)&Texture;
                return GR_OK;
            }
            if (IidEqual(Iid, Iids::RenderPipeline8B))
            {
                *Result = (IRenderPipeline*)this;
                return GR_OK;
            }
            *Result = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }

        GENRESULT NEURONCOMPONENTS_API QueryDeviceAbility(unsigned int Index, unsigned long* Result) override
        {
            *Result = Index == 5 ? SquareOnly : Index == 6 || Index == 7 ? 256 : 0;
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
            const bool bOk = W != 0 && H != 0 && Levels != 0 && Levels <= Chain;
            unsigned long Handle = 0;
            if (bOk)
            {
                Handle = NextHandle++;
                CreatedLevels[Handle] = Levels;
                *OutHandle = (void*)(uintptr_t)Handle;
            }
            MipsView = PendingView;
            MipsSize = PendingSize;
            PendingView = nullptr;
            PendingSize = 0;
            Event(Format("{\"ev\":\"create\",\"w\":\"%lu\",\"h\":\"%lu\",\"fmt\":\"%u\",\"levels\":\"%u\","
                         "\"handle\":\"%lu\",\"result\":\"%d\"}",
                         W, H, *PixelFormat, Levels, Handle, bOk ? 0 : -1));
            return bOk ? GR_OK : GR_GENERIC;
        }

        GENRESULT NEURONCOMPONENTS_API DestroyTexture(void* Handle) override
        {
            Event(Format("{\"ev\":\"destroy\",\"handle\":\"%lu\"}", (unsigned long)(uintptr_t)Handle));
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
                const std::map<unsigned long, unsigned long>::const_iterator Found = CreatedLevels.find(Id);
                if (Found == CreatedLevels.end() || (unsigned long)Level >= Found->second)
                    Result = -1;
            }
            std::string Source;
            unsigned long long Need;
            unsigned long long Got;
            std::string Hash;
            if (MipsView != nullptr)
            {
                const unsigned long Offset = (unsigned long)((const unsigned char*)Pixels - MipsView);
                const bool bDxt = *SrcFormat >= 10 && *SrcFormat <= 14;
                const unsigned long Rows = bDxt ? ((unsigned long)SrcH + 3) >> 2 : (unsigned long)SrcH;
                Need = (unsigned long long)Pitch * Rows;
                const unsigned long long Start = Offset < MipsSize ? Offset : MipsSize;
                unsigned long long End = (unsigned long long)Offset + Need;
                if (End > MipsSize)
                    End = MipsSize;
                Got = End > Start ? End - Start : 0;
                Source = Format("mips+%lx", Offset);
                Hash = Fnv64(MipsView + Start, (size_t)Got);
            }
            else
            {
                Need = (unsigned long long)Pitch * (unsigned long)SrcH;
                Got = Need;
                Source = "decoded";
                Hash = Fnv64(Pixels, (size_t)Got);
            }
            const std::string PaletteHash = Palette != nullptr ? Fnv64(Palette, 768) : std::string("-");
            Event(Format("{\"ev\":\"level\",\"handle\":\"%lu\",\"level\":\"%d\",\"w\":\"%d\",\"h\":\"%d\","
                         "\"pitch\":\"%u\",\"fmt\":\"%u\",\"src\":\"%s\",\"need\":\"%llu\",\"got\":\"%llu\","
                         "\"fnv\":\"%s\",\"pal\":\"%s\",\"result\":\"%d\"}",
                         Id, Level, SrcW, SrcH, Pitch, *SrcFormat, Source.c_str(), Need, Got, Hash.c_str(),
                         PaletteHash.c_str(), Result));
            return Result;
        }

        // Everything else the texture library never calls.
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
        GENRESULT NEURONCOMPONENTS_API GetTextureFormat(
            void* Handle, int* OutFormat) override { return GR_GENERIC; }
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

    GENRESULT NEURONCOMPONENTS_API FRecordingTexture::QueryInterface(const char* Iid, void** Out)
    {
        return Owner->QueryInterface(Iid, Out);
    }

    GENRESULT NEURONCOMPONENTS_API FRecordingTexture::GetImageInfo(_D3DXIMAGE_INFO* Info, unsigned int SrcSize,
                                                                   const void* SrcData)
    {
        const HRESULT Result = ShadingImageInfo(SrcData, SrcSize, Info);
        Owner->Event(Format("{\"ev\":\"info\",\"size\":\"%u\",", SrcSize) + InfoFields(Result, *Info) + "}");
        if (IsDdsInfo(Result, *Info))
        {
            Owner->PendingView = (const unsigned char*)SrcData;
            Owner->PendingSize = SrcSize;
        }
        return (GENRESULT)Result;
    }

    GENRESULT NEURONCOMPONENTS_API FRecordingTexture::LoadCubemap(IFileSystem* FileSystem, const char* FileName,
                                                                  void** OutHandle)
    {
        FFileSystemDesc Desc = MakeTextureDescriptor(FileName);
        const int File = FileSystem->OpenByDesc(&Desc);
        if (File == -1)
            return -8;
        const unsigned long Size = FileSystem->GetSize((unsigned long)File, 0);
        const int Mapping = FileSystem->CreateMapping((unsigned long)File, 0, 2, 0, 0, 0);
        const unsigned char* View = nullptr;
        if (Mapping != -1)
            View = (const unsigned char*)FileSystem->MapFileView((unsigned long)Mapping, 4, 0, 0, 0);
        GENRESULT Result = GR_GENERIC;
        if (View != nullptr)
        {
            D3DXIMAGE_INFO Info;
            const HRESULT InfoResult = ShadingImageInfo(View, Size, &Info);
            const bool bCube = IsDdsInfo(InfoResult, Info) && Info.ResourceType == D3DRTYPE_CUBETEXTURE;
            const unsigned long Handle = bCube ? Owner->NextHandle++ : 0;
            Result = bCube ? GR_OK : GR_GENERIC;
            *OutHandle = (void*)(uintptr_t)Handle;
            Owner->Event(Format("{\"ev\":\"cube\",\"size\":\"%lu\",\"fnv\":\"%s\",", Size, Fnv64(View, Size).c_str()) +
                         InfoFields(InfoResult, Info) +
                         Format(",\"handle\":\"%lu\",\"result\":\"%d\"}", Handle, (int)Result));
            FileSystem->UnmapFileView((unsigned long)(uintptr_t)View);
        }
        if (Mapping != -1)
            FileSystem->CloseFile((unsigned long)Mapping);
        FileSystem->CloseFile((unsigned long)File);
        return Result;
    }

    // The services a material's Initialize asks its outer component for. Loading and reading
    // parameters never calls them (no texture is resolved), so one object answers all four.
    class FShadingServices final : public INeuronComponent
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            if (IidEqual(Iid, Iids::TextureLibraryA) || IidEqual(Iid, Iids::RenderPipeline8B) ||
                IidEqual(Iid, Iids::RPDraw) || IidEqual(Iid, Iids::VertexBufferManager))
            {
                *Out = this;
                return GR_OK;
            }
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
    };

    FShadingServices GShadingServices;

    IFileSystem* OpenArchive(const std::string& Path)
    {
        FFileSystemDesc Desc;
        memset(&Desc, 0, sizeof(Desc));
        Desc.Size = 0x34;
        Desc.ComponentName = "FileSystem";
        Desc.Implementation = "UTF";
        Desc.Path = Path.c_str();
        Desc.Access = 0x80000000;
        Desc.ShareMode = 1;
        Desc.Creation = 3;
        Desc.Flags = 0x08000080;
        Desc.Field30 = 0xFFFFFFFF;
        IFileSystem* Fs = nullptr;
        if (NEURONCOMPONENTS_Acquire()->CreateInstance((AGGDESC*)&Desc, (void**)&Fs) != GR_OK)
        {
            if (Fs != nullptr)
                Fs->Release();
            return nullptr;
        }
        return Fs;
    }

    bool HasNode(IFileSystem* Fs, const char* Name)
    {
        if (Fs->OpenNode(Name) == 0)
            return false;
        Fs->OpenNode("..");
        return true;
    }

    // The size of a leaf of the current node, 0 when it cannot be opened.
    unsigned long LeafSize(IFileSystem* Fs, const char* Name)
    {
        FFileSystemDesc Desc = MakeTextureDescriptor(Name);
        const int Handle = Fs->OpenByDesc(&Desc);
        if (Handle == -1)
            return 0;
        const unsigned long Size = Fs->GetSize((unsigned long)Handle, 0);
        Fs->CloseFile((unsigned long)Handle);
        return Size;
    }

    // The ten TGA mapping records persist across loads in the reference; a new library starts
    // without them, as the Rust loader does.
    void ResetTgaRecords()
    {
        FTextureTgaMap* Maps = TgaMappedResources();
        for (int Index = 0; Index < 10; ++Index)
            Maps[Index].CloseResources();
    }

    // LoadTextureRecord, with a processor exception (the reference's division by zero, null or
    // out-of-range reads, a failed allocation) reported as a fault.
    bool GuardedLoadTextureRecord(FTextureLibrary* Library, IFileSystem* Fs, FFileSystemDesc* Desc,
                                  FTextureEntry** Out, GENRESULT* Result)
    {
        __try
        {
            *Result = Library->LoadTextureRecord(Fs, Desc, Out, false);
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void RecordLines(const FTextureEntry* Entry, Suite& Out)
    {
        std::string Children;
        for (size_t Index = 0; Index < Entry->Children.size(); ++Index)
        {
            if (Index != 0)
                Children += ",";
            const FTextureEntry* Child = Entry->Children[Index];
            Children += Child == nullptr ? std::string("-") : HexC(Child->GetName());
        }
        Out.Line("{\"record\":\"" + HexC(Entry->GetName()) +
                 Format("\",\"handle\":\"%lu\",\"refs\":\"%d\",\"rate\":\"%08x\",\"frames\":\"%u\",\"children\":\"",
                        Entry->GpuHandle, Entry->RefCount, BitsAt(&Entry->TileRate),
                        (unsigned int)Entry->Frames.size()) +
                 Children + "\"}");
        for (size_t Index = 0; Index < Entry->Frames.size(); ++Index)
        {
            const FTextureFrame& Frame = Entry->Frames[Index];
            Out.Line(Format("{\"frame\":\"%u\",\"child\":\"%lu\",\"uv\":\"%08x,%08x,%08x,%08x\"}", (unsigned int)Index,
                            Frame.ChildIndex, BitsAt(&Frame.Uv[0]), BitsAt(&Frame.Uv[1]), BitsAt(&Frame.Uv[2]),
                            BitsAt(&Frame.Uv[3])));
        }
    }

    // LoadTextureGroup's loop on an opened archive with a new library, the default format maps
    // and the given settings: per child the renderer calls, the result and the record.
    void TextureLines(IFileSystem* Fs, const FShadingSettings& Settings, Suite& Out)
    {
        ResetTgaRecords();
        FormatMapLoadDefaults();
        FRecordingPipeline Pipeline(&Out, Settings.SquareOnly);
        FTextureLibrary* Library = new FTextureLibrary();
        Library->Pipeline = &Pipeline;
        Library->GenericSlots[0] = Settings.LodLoad;
        Library->GenericSlots[2] = Settings.SquarePolicy;
        Library->GenericSlots[3] = Settings.Pow2Policy;
        Library->GenericSlots[4] = Settings.LodMin;
        Library->RefreshDeviceAbilities();
        if (Fs->OpenNode("Texture library") == 0)
            Out.Line("{\"texlib\":\"0\"}");
        else
        {
            Out.Line("{\"texlib\":\"1\"}");
            WIN32_FIND_DATAA FindData;
            const int FindHandle = Fs->FindFirst("*.*", &FindData);
            if (FindHandle != -1)
            {
                do
                {
                    if (FindData.cFileName[0] != '.')
                    {
                        FFileSystemDesc Desc = MakeTextureDescriptor(FindData.cFileName);
                        Desc.Field30 = (unsigned long)FindHandle;
                        FTextureEntry* Entry = nullptr;
                        GENRESULT Result = GR_GENERIC;
                        Pipeline.ResetViews();
                        const bool bDone = GuardedLoadTextureRecord(Library, Fs, &Desc, &Entry, &Result);
                        if (bDone && Result >= 0)
                        {
                            int PlaybackId;
                            Library->CreatePlaybackGroup(Entry, &PlaybackId);
                        }
                        Out.Line("{\"entry\":\"" + HexC(FindData.cFileName) + "\",\"result\":\"" +
                                 (bDone ? Format("%ld", (long)Result) : std::string("fault")) + "\"}");
                        if (bDone && Result >= 0)
                            RecordLines(Entry, Out);
                    }
                } while (Fs->FindNext((unsigned long)FindHandle, &FindData) != 0);
                Fs->FindClose((unsigned long)FindHandle);
            }
            Fs->OpenNode("..");
        }
        // The library's records and handles are abandoned with it; its destructor would only
        // clear the format maps, which the next call loads again.
        Library->Pipeline = nullptr;
        ResetTgaRecords();
    }

    std::string ValueText(unsigned long Type, const unsigned long* Words, const char* Text)
    {
        if (Type == 5 || Type == 7)
            return Format("%lu:s:", Type) + (Text == nullptr ? std::string("-") : HexC(Text));
        const int Count = Type == 3 ? 3 : Type == 4 ? 4 : 1;
        std::string Out = Format("%lu:", Type);
        for (int Index = 0; Index < Count; ++Index)
            Out += Format(Index == 0 ? "%08lx" : ",%08lx", Words[Index]);
        return Out;
    }

    std::string ValueText(const FShadingPropertyValue& Value)
    {
        return ValueText(Value.Type, &Value.Payload0, (const char*)Value.Payload0);
    }

    void ParameterLines(FMaterial* Material, Suite& Out)
    {
        unsigned long Count = 0;
        Material->GetParameterCount(&Count);
        for (unsigned long Index = 0; Index < Count; ++Index)
        {
            FShadingPropertyValue Value;
            memset(&Value, 0, sizeof(Value));
            if (Material->GetParameterValue(Index, &Value) >= 0)
                Out.Line(Format("{\"p\":\"%lu\",\"v\":\"", Index) + ValueText(Value) + "\"}");
            Value.ReleaseOwnedPayload();
        }
    }

    // FMaterialLibrary::LoadFromFileSystem's loop and LoadAnimationGroup's loop on an opened
    // archive.
    void MaterialLines(IFileSystem* Fs, Suite& Out)
    {
        if (Fs->OpenNode("Material library") == 0)
            Out.Line("{\"matlib\":\"0\"}");
        else
        {
            Out.Line("{\"matlib\":\"1\"}");
            std::set<unsigned long> Created;
            WIN32_FIND_DATAA FindData;
            const int FindHandle = Fs->FindFirst("*.*", &FindData);
            if (FindHandle != -1)
            {
                do
                {
                    const char* ChildName = FindData.cFileName;
                    if (ChildName[0] == '.' || (FindData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                        continue;
                    const std::string Name = HexC(ChildName);
                    const unsigned long Crc = NEURONCOMPONENTS_CRC::GetCRC32(ChildName);
                    if (Created.count(Crc) != 0)
                    {
                        Out.Line("{\"mat\":\"" + Name + "\",\"entry\":\"dup\"}");
                        continue;
                    }
                    if (Fs->OpenNode(ChildName) == 0)
                    {
                        Out.Line("{\"mat\":\"" + Name + "\",\"entry\":\"noopen\"}");
                        continue;
                    }
                    FFileSystemDesc Desc = MakeTextureDescriptor("Type");
                    const int TypeHandle = Fs->OpenByDesc(&Desc);
                    char Type[0x104];
                    bool bTypeRead = false;
                    if (TypeHandle != -1)
                    {
                        unsigned long Size = Fs->GetSize((unsigned long)TypeHandle, 0);
                        if (Size >= 0x103)
                            Size = 0x103;
                        unsigned long BytesRead = 0;
                        const int ReadResult = Fs->Read((unsigned long)TypeHandle, Type, Size, &BytesRead, 0);
                        if (ReadResult != 0 && BytesRead == Size)
                        {
                            Type[BytesRead] = '\0';
                            bTypeRead = true;
                        }
                        Fs->CloseFile((unsigned long)TypeHandle);
                    }
                    if (!bTypeRead)
                        Out.Line("{\"mat\":\"" + Name + "\",\"entry\":\"notype\"}");
                    else
                    {
                        // CreateGenericRecord's 0x40-byte type copy, then the factory of that name.
                        char TypeCopy[0x40];
                        strncpy(TypeCopy, Type, sizeof(TypeCopy));
                        TypeCopy[sizeof(TypeCopy) - 1] = '\0';
                        FMaterial* Material = CreateMaterialInstance(TypeCopy);
                        if (Material != nullptr && Material->Initialize(&GShadingServices) < 0)
                            Material = nullptr;
                        Out.Line("{\"mat\":\"" + Name + "\",\"type\":\"" + HexC(Type) + "\",\"kind\":\"" +
                                 (Material != nullptr ? std::string(Material->FactoryName) : std::string("-")) +
                                 "\"}");
                        if (Material != nullptr)
                        {
                            Created.insert(Crc);
                            Material->LoadFromFileSystem(Fs);
                            ParameterLines(Material, Out);
                            Out.Line(Format("{\"blend\":\"%u\"}", (unsigned int)Material->bAlphablend));
                        }
                    }
                    Fs->OpenNode("..");
                } while (Fs->FindNext((unsigned long)FindHandle, &FindData) != 0);
                Fs->FindClose((unsigned long)FindHandle);
            }
            Fs->OpenNode("..");
        }

        if (Fs->OpenNode("MaterialAnim") == 0)
        {
            Out.Line("{\"anims\":\"0\"}");
            return;
        }
        Out.Line("{\"anims\":\"1\"}");
        WIN32_FIND_DATAA FindData;
        const int Find = Fs->FindFirst("*.*", &FindData);
        if (Find != -1)
        {
            do
            {
                const char* ChildName = FindData.cFileName;
                if ((FindData.dwFileAttributes & 0x10) == 0 || ChildName[0] == '.')
                    continue;
                FFileSystemDesc Desc = MakeTextureDescriptor(ChildName);
                Desc.Field30 = (unsigned long)Find;
                IFileSystem* Child = nullptr;
                if (Fs->CreateInstance((AGGDESC*)&Desc, (void**)&Child) >= 0)
                {
                    FMaKeyBlock Block;
                    memset(&Block, 0, sizeof(Block));
                    OracleParseMaKeys(Child, &Block);
                    std::string Deltas = "-";
                    if (Block.Deltas != nullptr)
                    {
                        const unsigned long Asked = Block.Count * 0x14;
                        const unsigned long Size = LeafSize(Child, "MADeltas");
                        const unsigned long Got = Asked < Size ? Asked : Size;
                        Deltas = Format("%lu,%lu,", Asked, Got) + Fnv64(Block.Deltas, Got);
                    }
                    std::string Keys = "-";
                    if (Block.Keys != nullptr)
                    {
                        const unsigned long Asked = (Block.Count - 1) << 4;
                        const unsigned long Size = LeafSize(Child, "MAKeys");
                        const unsigned long Got = Asked < Size ? Asked : Size;
                        Keys = Format("%lu,%lu,", Asked, Got) + Fnv64(Block.Keys, Got);
                    }
                    Out.Line("{\"anim\":\"" + HexC(ChildName) +
                             Format("\",\"crc\":\"%08lx\",\"count\":\"%lu\",\"flags\":\"%08lx\",\"deltas\":\"",
                                    NEURONCOMPONENTS_CRC::GetCRC32(ChildName), Block.Count, Block.Flags) +
                             Deltas + "\",\"keys\":\"" + Keys + "\"}");
                    operator delete(Block.Deltas);
                    operator delete(Block.Keys);
                }
                if (Child != nullptr)
                    Child->Release();
            } while (Fs->FindNext((unsigned long)Find, &FindData) != 0);
        }
        // LoadAnimationGroup leaves the find handle open.
        Fs->OpenNode("..");
    }

    void TgaSuite()
    {
        Suite Out("shading_tga.jsonl");
        unsigned int Index = 0;
        for (const std::string& Line : InputLines("shading/tga.list"))
        {
            const Bytes Blob = Unhex(Line);
            const unsigned char* Data = Blob.data();
            unsigned long SourceFormat = 0;
            const bool bFormat = GetTgaSourceFormat(Data, &SourceFormat);
            const bool bSupported = IsTgaHeaderSupported(Data);
            std::string Text = Format("{\"i\":\"%u\",\"supported\":\"%d\",\"format\":\"%d:%lu\"", Index++,
                                      bSupported ? 1 : 0, bFormat ? 1 : 0, SourceFormat);
            if (bSupported)
            {
                unsigned long Decoded = 0;
                static unsigned char Palette[256 * 31];
                memset(Palette, 0, sizeof(Palette));
                unsigned char* Pixels = nullptr;
                DecodeTgaTexture(Data, &Decoded, Palette, &Pixels);
                const int Width = (short)ReadU16(Data, 12);
                const int Height = (short)ReadU16(Data, 14);
                const unsigned long Pitch = ((TextureFormatDescriptor(Decoded)->BitsPerPixel + 7) >> 3) * Width;
                const std::string PaletteHex =
                    (Data[2] & 7) == 1 ? Hex(Palette, 256 * (Data[7] >> 3)) : std::string("-");
                Text += Format(",\"decoded\":\"%lu\",\"w\":\"%d\",\"h\":\"%d\",\"pitch\":\"%lu\",\"pixels\":\"",
                               Decoded, Width, Height, Pitch) +
                        Hex(Pixels, Height * Pitch) + "\",\"palette\":\"" + PaletteHex + "\"";
                delete[] Pixels;
            }
            Out.Line(Text + "}");
        }
    }

    void DdsSuite()
    {
        Suite Out("shading_dds.jsonl");
        unsigned int Index = 0;
        for (const std::string& Line : InputLines("shading/dds.list"))
        {
            const Bytes Data = Unhex(Line);
            const unsigned int Size = (unsigned int)Data.size() - 1; // Unhex appends a NUL.
            D3DXIMAGE_INFO Info;
            const HRESULT Result = ShadingImageInfo(Data.data(), Size, &Info);
            Out.Line(Format("{\"i\":\"%u\",\"size\":\"%u\",", Index++, Size) + InfoFields(Result, Info) + "}");
        }
    }

    void FormatMapSuite()
    {
        Suite Out("shading_format_maps.jsonl");
        FTextureLibrary* Library = new FTextureLibrary(); // for PickGpuFormat; never released.
        FormatMapLoadDefaults();
        for (const std::string& Step : InputLines("shading/format_maps.list"))
        {
            if (Step == "defaults")
                FormatMapLoadDefaults();
            else if (Step == "empty")
                FormatMapClear();
            else
            {
                const Bytes Text = Unhex(Step.substr(5)); // "line <hex>"
                char Buffer[1024];
                memset(Buffer, 0, sizeof(Buffer));
                memcpy(Buffer, Text.data(), Text.size() - 1);
                FormatMapParseLine(Buffer);
            }
            std::string Rows;
            for (size_t Row = 0; Row < GFormatMaps.size(); ++Row)
            {
                const FTextureFormatMap& Map = GFormatMaps[Row];
                if (Row != 0)
                    Rows += ";";
                Rows += Format("%lu.%lu.%lu.%lu.%lu.%lu/", Map.Value[0], Map.Value[1], Map.Value[2], Map.Value[3],
                               Map.Value[4], Map.Value[5]);
                for (int Field = 0; Field < 6; ++Field)
                    Rows += Map.Mask[Field] != 0 ? "1" : "0";
                Rows += Format("/%08lx", Map.FourCc);
            }
            std::string Picks;
            for (unsigned long Id = 0; Id < 22; ++Id)
            {
                if (Id != 0)
                    Picks += ",";
                unsigned long Picked = 0;
                Picks += Library->PickGpuFormat(&Id, &Picked) == GR_OK ? Format("%lu", Picked) : std::string("-");
            }
            Out.Line("{\"step\":\"" + Step + "\",\"rows\":\"" + Rows + "\",\"pick\":\"" + Picks + "\"}");
        }
        FormatMapLoadDefaults();
    }

    void SchemaSuite()
    {
        Suite Out("shading_schema.jsonl");
        for (size_t Kind = 0; Kind < sizeof(kShadingKinds) / sizeof(kShadingKinds[0]); ++Kind)
        {
            // Materials are not released: their teardown is not part of this suite.
            FMaterial* Material = CreateMaterialInstance(kShadingKinds[Kind]);
            char ClassName[0x100];
            Material->CopyClassName(ClassName, sizeof(ClassName));
            unsigned long Count = 0;
            Material->GetParameterCount(&Count);
            Out.Line(Format("{\"kind\":\"%s\",\"class\":\"%s\",\"count\":\"%lu\"}", kShadingKinds[Kind], ClassName,
                            Count));
            for (unsigned long Index = 0; Index < Count; ++Index)
            {
                FShadingPropertyRecord Record;
                memset(&Record, 0, sizeof(Record));
                Material->GetParameterDescriptor(Index, &Record);
                Out.Line(Format("{\"i\":\"%lu\",\"flags\":\"%08lx\",\"name\":\"", Index, Record.Flags) +
                         HexC(Record.Name) + "\",\"desc\":\"" + HexC(Record.Description) + "\",\"cur\":\"" +
                         ValueText(Record.Current) + "\",\"min\":\"" + ValueText(Record.Default) +
                         "\",\"max\":\"" + ValueText(Record.Saved) + "\"}");
                Record.Current.ReleaseOwnedPayload();
                Record.Default.ReleaseOwnedPayload();
                Record.Saved.ReleaseOwnedPayload();
            }
            Material->Initialize(&GShadingServices);
            ParameterLines(Material, Out);
        }
    }

    std::vector<std::string> Tokens(const std::string& Text)
    {
        std::vector<std::string> Out;
        size_t Start = 0;
        for (;;)
        {
            const size_t Space = Text.find(' ', Start);
            Out.push_back(Text.substr(Start, Space == std::string::npos ? std::string::npos : Space - Start));
            if (Space == std::string::npos)
                return Out;
            Start = Space + 1;
        }
    }

    void TexlibSuite()
    {
        Suite Out("shading_texlib.jsonl");
        for (const std::string& Line : InputLines("shading/texlib.list"))
        {
            const std::vector<std::string> Fields = Tokens(Line);
            unsigned long Numbers[5] = {0, 0, 0, 0, 0};
            for (size_t Index = 0; Index < 5 && Index + 1 < Fields.size(); ++Index)
                Numbers[Index] = strtoul(Fields[Index + 1].c_str(), nullptr, 10);
            const FShadingSettings Settings = {Numbers[0], Numbers[1], Numbers[2], Numbers[3], Numbers[4]};
            const std::string Head =
                "{\"archive\":\"" + Fields[0] + "\",\"settings\":\"" + SettingsText(Settings) + "\"";
            IFileSystem* Fs = OpenArchive(Fields[0]);
            if (Fs == nullptr)
            {
                Out.Line(Head + ",\"open\":\"0\"}");
                continue;
            }
            Out.Line(Head + ",\"open\":\"1\"}");
            TextureLines(Fs, Settings, Out);
            Fs->Release();
        }
    }

    void MaterialSuite()
    {
        Suite Out("shading_material.jsonl");
        for (const std::string& Archive : InputLines("shading/material.list"))
        {
            IFileSystem* Fs = OpenArchive(Archive);
            if (Fs == nullptr)
            {
                Out.Line("{\"archive\":\"" + Archive + "\",\"open\":\"0\"}");
                continue;
            }
            Out.Line("{\"archive\":\"" + Archive + "\",\"open\":\"1\"}");
            MaterialLines(Fs, Out);
            Fs->Release();
        }
    }
}

void RunShadingSuites()
{
    TgaSuite();
    DdsSuite();
    FormatMapSuite();
    SchemaSuite();
    TexlibSuite();
    MaterialSuite();
}

void RunShadingDataDump(const std::vector<std::string>& Files)
{
    std::vector<std::string> TextureFiles;
    std::vector<std::string> MaterialFiles;
    for (const std::string& File : Files)
    {
        if (FileMagic(File) != "UTF ")
            continue;
        IFileSystem* Fs = OpenArchive(File);
        if (Fs == nullptr)
            continue;
        if (HasNode(Fs, "Texture library"))
            TextureFiles.push_back(File);
        if (HasNode(Fs, "Material library") || HasNode(Fs, "MaterialAnim"))
            MaterialFiles.push_back(File);
        Fs->Release();
    }

    const FShadingSettings Defaults = {1, 0, 0, 0, 0}; // TEXTURE_LOD_LOAD's default, nothing else set
    {
        Suite List(GOutputDir + "\\texture\\files.txt");
        for (const std::string& File : TextureFiles)
            List.Line(File);
    }
    for (const std::string& File : TextureFiles)
    {
        Suite Out(DataOutputFile("texture", File));
        Out.Line("{\"file\":\"" + File + "\",\"settings\":\"" + SettingsText(Defaults) + "\"}");
        IFileSystem* Fs = OpenArchive(File);
        if (Fs == nullptr)
            continue;
        TextureLines(Fs, Defaults, Out);
        Fs->Release();
    }

    {
        Suite List(GOutputDir + "\\material\\files.txt");
        for (const std::string& File : MaterialFiles)
            List.Line(File);
    }
    for (const std::string& File : MaterialFiles)
    {
        Suite Out(DataOutputFile("material", File));
        Out.Line("{\"file\":\"" + File + "\"}");
        IFileSystem* Fs = OpenArchive(File);
        if (Fs == nullptr)
            continue;
        MaterialLines(Fs, Out);
        Fs->Release();
    }
    printf("dumped %u texture libraries and %u material archives\n", (unsigned int)TextureFiles.size(),
           (unsigned int)MaterialFiles.size());
}
