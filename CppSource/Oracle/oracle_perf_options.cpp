// Oracle suite of work package P4.1 (Oracle.exe game-app): PerfOptions.ini through the client's
// LoadPerfOptions and SavePerfOptions (perf_options.cpp, compiled in), over
// crates/game-app/tests/fixtures/oracle/perf_options.list and the files under perf/. The Rust
// test crates/game-app/tests/oracle_vectors.rs renders the same lines.
//
// GetUserDataPath is redirected to the fixture's perf/ folder for the loads and to the output
// directory for the save, whose file is read back and removed. The option callbacks' targets
// outside perf_options.cpp are stood in below with no effect; the ones inside it run, on a
// pipeline whose every call fails and without material or texture libraries. Each line starts
// from the registry's initial state (values 0, never applied, warnings not skipped).
//
// Input lines: <file>[,<file>...] <d|-> <width> <height> <color> <depth> -- each file loaded in
// turn (d: with a display block preset to InitializeEngine's 0, 0, -1, -1; -: without), then
// the options saved with g_display's four fields set to the last four numbers.
// Output lines: {"in","loads" (1 or 0 per file),"display","skip","values","applied" (the 36
// options' float bits),"save" (the saved file's bytes)}.

// Oracle.exe also links oracle_audio_client.cpp, which compiles the real sound setters
// (menu_sound_arch.cpp) and stands in for perf_options.cpp's getters. This translation unit
// renames both sets, so perf_options.cpp's getters and the callback stand-ins below are its
// own and no symbol is defined twice.
#define GetMaxSubmittedSounds OraclePerfGetMaxSubmittedSounds
#define GetPerfOptionSlot OraclePerfGetPerfOptionSlot
#define SetClientAudioAmbientVolume OraclePerfSetClientAudioAmbientVolume
#define SetClientAudioInterfaceVolume OraclePerfSetClientAudioInterfaceVolume
#define SetClientAudioMusicVolume OraclePerfSetClientAudioMusicVolume
#define SetClientAudioSfxVolume OraclePerfSetClientAudioSfxVolume
#define SetClientAudioVoiceVolume OraclePerfSetClientAudioVoiceVolume
#define SetClientSoundLod OraclePerfSetClientSoundLod

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "oracle_support.h"

#include "common_misc.h"

namespace
{
    // The folder GetUserDataPath answers.
    std::string GUserPath;

    bool OracleUserDataPath(char* const Dest)
    {
        strcpy(Dest, GUserPath.c_str());
        return true;
    }
}

#define GetUserDataPath OracleUserDataPath
#include "../OutpostFrontier/Client/perf_options.cpp"
#undef GetUserDataPath

using namespace oracle;

// The option callbacks' targets elsewhere in the client.
DisplaySettings g_display = {};
float G_SunEffectQuality = 0.0f;

namespace
{
    FStarSphereListNode GStarSentinel;
    FStarSphereList GStarList;
}

FStarSphereList* StarSphereList()
{
    GStarSentinel.Next = &GStarSentinel;
    GStarSentinel.Prev = &GStarSentinel;
    GStarList.Sentinel = &GStarSentinel;
    GStarList.Count = 0;
    return &GStarList;
}

void* GetPlayerObjRW()
{
    return nullptr;
}

CShip* HudShipOfObjRw(IObjRW*)
{
    return nullptr;
}

void ApplyMouseSpeedOption(float) {}
void ApplyGammaRampOption(float) {}
void ApplyDitherControlOption(float) {}
void ApplyMipFilterControlOption(float) {}
void NebulaApplyGlobalDetail(float) {}
void CliAsteroidApplyGlobalDetail(float) {}
void StarSphereApplyDetail(float) {}
void SolarSpheresApplyDetail(float) {}
void SetClientAudioMusicVolume(float) {}
void SetClientAudioAmbientVolume(float) {}
void SetClientAudioSfxVolume(float) {}
void SetClientAudioInterfaceVolume(float) {}
void SetClientAudioVoiceVolume(float) {}
void SetClientSoundLod(int) {}
void GfScriptSequenceSetSkipOption(float) {}

bool GfScriptSequenceSkipLatch()
{
    return false;
}

namespace Archetype
{
    void CliObjRecomputeLodPick(ICliObj*, float const*) {}
    void CliObjSetArchLodGlobals(unsigned char, float) {}
}

namespace
{
    // A pipeline on which every call fails.
    class FPerfPipeline final : public IRenderPipeline
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API GetRenderState(D3DRENDERSTATETYPE State, int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetRenderState(D3DRENDERSTATETYPE State, int Value) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageState(int Stage, int Type, int Value) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageTexture(int Stage, void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetModelview(const float* M) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetPipelineState(int Id, int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetWorld(const float* M) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetWorldN(int WorldIndex, const float* M12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetMaterial(const D3DMATERIAL8* Material) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageTransform(int Stage, const float* Matrix16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureFormat(void* Handle, int* OutFormat) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API VerifyState() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitiveVb(unsigned long Prim, void* Vb, unsigned int BaseVertex,
                                                              unsigned int NumVerts, const void* Indices,
                                                              unsigned int IndexCount, unsigned long) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Startup(const char* SectionName) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ShutdownPipeline() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetPipelineState(int Id, int Value) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDeviceInfo(void* Out520) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API QueryDeviceAbility(unsigned int Index, unsigned long* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDisplayModeCount(unsigned int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDisplayModeEnum(unsigned int Index, void* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SelectMode(int* Mode, unsigned int* OutIndex) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CreateBuffers(HWND Hwnd, int* Mode, int* OutMode) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetModeBlob(void* OutAdapter, void* OutMode10Dwords) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DestroyGpuResources() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ClearBuffers(unsigned long Flags, const void* Rects) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SwapBuffers() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LockBuffer(void* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UnlockBuffer() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetBufferInterface(void* Out, void* Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDeviceStats(void* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetViewportRect(int X, int Y, int W, int H) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetViewportRect(int* OutX, int* OutY, int* OutW, int* OutH) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetDepthRange(float MinZ, float MaxZ) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDepthRange(float* OutMinZ, float* OutMaxZ) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetWindow(HWND Hwnd, int X, int Y, int W, int H) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetWindow(HWND* OutHwnd, int* OutLeft, int* OutRight, int* OutW, int* OutH) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetWorld(float* Out12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetView(const float* M12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetView(float* Out12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetModelview(float* Out12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetProjection(const float* M16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetProjection(float* Out16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetLookat(float EyeX, float EyeY, float EyeZ, float AtX, float AtY, float AtZ, float UpX, float UpY, float UpZ) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetOrtho(float Left, float Right, float Bottom, float Top, float NearPlane, float FarPlane) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetPerspective(float FovDeg, float Aspect, float NearPlane, float FarPlane) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetLight(void* Handle, const D3DLIGHT8* Light) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DestroyLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetLight(void* Handle, D3DLIGHT8* OutLight) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetLightEnable(void* Handle, int Enable) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetLightEnable(void* Handle, int* OutEnable) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetMaterial(D3DMATERIAL8* Material) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CreateTexture(int Width, int Height, unsigned int* Format, unsigned int Levels, unsigned int Flags, void** OutHandle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DestroyTexture(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API IsTexture(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LockTexture(void* Handle, int Level, void* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UnlockTexture(void* Handle, int Level) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureDim(void* Handle, unsigned int* OutW, unsigned int* OutH, int* OutLevels) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureInterface(void* Handle, void* UnusedA, void* UnusedB) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetTextureLevelData(void* Handle, int Level, int SrcW, int SrcH, unsigned int Pitch, unsigned int* SrcFormat, const void* Pixels, void* A, void* B) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API BlitTexture(void* DstHandle, int DstLevel, void* SrcHandle, int SrcLevel) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetRenderTarget(void* Handle, void* UnusedA, void* UnusedB) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetRenderTarget(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API BeginScene() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API EndScene() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ResetRenderStatesToDefaults() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageState(int Stage, int Type, int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageTransform(int Stage, float* OutMatrix16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageTexture(int Stage, void** OutHandle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DrawPrimitive(unsigned long Prim, unsigned int Fvf, const void* Verts, unsigned int Count, void* Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitiveUp(unsigned long Prim, unsigned int Fvf, const void* Verts, unsigned int VertCount, const void* Indices, unsigned int IndexCount, unsigned long Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DrawPrimitiveVb(unsigned long Prim, void* Vb, unsigned int StartVertex, unsigned int Count, void* Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API AddLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API RemoveLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UpdateLight(void* Handle) override { return GR_GENERIC; }
    };

    FPerfPipeline GPipeline;

    std::vector<std::string> Fields(const std::string& Line)
    {
        std::vector<std::string> Out;
        size_t Start = 0;
        for (;;)
        {
            const size_t End = Line.find_first_of(" ,", Start);
            Out.push_back(Line.substr(Start, End == std::string::npos ? std::string::npos : End - Start));
            if (End == std::string::npos)
                break;
            Start = End + 1;
        }
        return Out;
    }

    std::string FloatList(bool bApplied)
    {
        std::string Out;
        for (unsigned Index = 0; Index < kOptionCount; ++Index)
        {
            if (Index != 0)
                Out += ",";
            Out += Format("%08x", FloatBits(bApplied ? G_Options[Index].Applied : G_Options[Index].Value));
        }
        return Out;
    }
}

void RunPerfOptionsSuite()
{
    IRenderPipeline* const SavedPipeline = NeuronRuntime::Pipeline;
    NeuronRuntime::Pipeline = &GPipeline;
    Suite Out("perf_options.jsonl");
    for (const std::string& Line : InputLines("perf_options.list"))
    {
        // The files, then the display flag and g_display's four fields.
        const size_t Space = Line.find(' ');
        if (Space == std::string::npos)
        {
            fprintf(stderr, "perf_options.list: bad line %s\n", Line.c_str());
            GFailed = true;
            continue;
        }
        std::vector<std::string> Files;
        {
            const std::string List = Line.substr(0, Space);
            size_t Start = 0;
            for (;;)
            {
                const size_t Comma = List.find(',', Start);
                Files.push_back(List.substr(Start, Comma == std::string::npos ? std::string::npos : Comma - Start));
                if (Comma == std::string::npos)
                    break;
                Start = Comma + 1;
            }
        }
        const std::vector<std::string> Rest = Fields(Line.substr(Space + 1));
        if (Rest.size() != 5)
        {
            fprintf(stderr, "perf_options.list: bad line %s\n", Line.c_str());
            GFailed = true;
            continue;
        }

        for (unsigned Index = 0; Index < kOptionCount; ++Index)
        {
            G_Options[Index].Value = 0.f;
            G_Options[Index].Applied = -1.f;
        }
        G_bSkipMachineWarnings = false;

        FPerfDisplaySettings Display = {0, 0, -1, -1};
        std::string Loads;
        GUserPath = GFixtureDir + "\\perf";
        for (const std::string& File : Files)
        {
            const bool bOk = LoadPerfOptions(File.c_str(), Rest[0] == "d" ? &Display : nullptr);
            Loads += Loads.empty() ? "" : ",";
            Loads += bOk ? "1" : "0";
        }

        g_display.width = atoi(Rest[1].c_str());
        g_display.height = atoi(Rest[2].c_str());
        g_display.color_bpp = atoi(Rest[3].c_str());
        g_display.depth_bpp = atoi(Rest[4].c_str());
        GUserPath = GOutputDir;
        SavePerfOptions("perf_options_save.ini");
        const std::string SavedPath = GOutputDir + "\\perf_options_save.ini";
        Bytes Saved;
        if (!ReadWholeFile(SavedPath, Saved))
        {
            fprintf(stderr, "perf_options: cannot read %s\n", SavedPath.c_str());
            GFailed = true;
        }
        remove(SavedPath.c_str());

        Out.Line("{\"in\":\"" + Line + "\",\"loads\":\"" + Loads + "\",\"display\":\"" +
                 Format("%d,%d,%d,%d", Display.Width, Display.Height, Display.ColorBpp, Display.DepthBpp) +
                 "\",\"skip\":\"" + (G_bSkipMachineWarnings ? "1" : "0") + "\",\"values\":\"" + FloatList(false) +
                 "\",\"applied\":\"" + FloatList(true) + "\",\"save\":\"" + Hex(Saved.data(), Saved.size()) + "\"}");
    }
    NeuronRuntime::Pipeline = SavedPipeline;
}
