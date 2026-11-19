// P4.5 (fourth increment): FLAppearance.cpp of surfacematerials.dll -- the override materials,
// the instance values, the engine face, the camera wrapper (FCameraBlob), the backdrop and
// starsphere draws, the fading and fixed blob shadows and CountVisibleGeometry -- through the
// reference's own code, against recording fakes. The Rust side is
// crates/neuron-client/tests/support/material_appearance.rs with the same contract; the input is
// crates/neuron-client/tests/fixtures/oracle/material_appearance.list from
// make_material_fixtures.py.
//
// FLAppearance.cpp is included below, so the suite reaches its file-local classes and statics.
// It defines G_FlmEngineOuter, FlmLookupIdValue, FlmCreateMathEngine and
// FlmCreateAppearanceInstance, which oracle_material_surface.cpp also defines for the surface
// materials (Oracle.exe links both): this file renames the four before the include. Everything
// else FLAppearance.cpp calls (FlmFdump, FlmVisibleCounterReset and Copy, the CRC, the component
// manager for the 3D math engine) is the process's own, from surfacematerials.cpp,
// neuroncomponents and x86math.
//
// A case is one input line of operations separated by " ; ", fields by " ". Configuration:
//   fail <face>            the face's QueryInterface fails (engine: the outer's IID_IEngine;
//                          system: the descriptor's system is null; matlib, pipeline, vbm,
//                          texlib: the system's; renderer: the engine's IID_IRenderer)
//   mat <i> <r> <m>        FindOrCreate of override i returns r and writes material m ("-" a
//                          null one); by default 0 and material i+1
//   ability <v>            QueryDeviceAbility(9) writes v ("-" nothing); by default 1
//   shadowtex <r> <fr> <h> FindTextureByCrc of "efxshadow" returns r (a record on 0, nothing
//                          otherwise), GetTextureFrame returns fr and writes handle h when fr >= 0;
//                          by default r -1
//   lock <r>               LockVertexMemory's result (0 by default)
//   xf <inst> <12 words>   GetInstanceTransform (the identity by default)
//   arch <inst> <a>        GetInstanceArchetype (-1 by default)
//   bounds <a> <r> <6 words|->  GetBounds of archetype a returns r and writes the six floats
//                          ("-" nothing); by default -1 and nothing
//   cam <field> <values>   the inner camera: pos 3, orient 9, view 12, unproj 3 (words), vp 4
//                          (decimal), hfov, fovy, znear, zfar, aspect (a word), xscale, yscale
//                          (16 hex digits of the double), proj <r> <3 words>, classify <r>
//   probe <spheres>        RenderInstance probes the camera it is given and classifies these
//                          spheres ("x,y,z,r/..." in words)
//   vis <a> <b> <c>        RenderInstance adds these to G_VisibleCounterA..C
// Operations, each writing {"case","op",...} after the lines of its calls:
//   create, init (Initialize through the inner's IID_IEngineComponent face), set <id> <word>,
//   get <id>, remove <id>, lookup <id> (FlmLookupIdValue), destroy <inst> (the engine face's
//   DestroyInstance), light <6 words>, lighton <flag>, override <i>, backdrop <inst>,
//   stars <inst>, fading <inst:radius,...>, fixed <inst:radius,...>, count <inst>, blob (the
//   camera wrapper's state), release (the inner's last Release).
//
// The calls write one line {"case","ev",...} each (strings throughout):
//   qi "on","face","r"                     dump "flags","text" (hex of the formatted text)
//   findorcreate "name","type","outer","r","m"   removemat "group","name","crc"
//   global "i","v"                         mrel "m"
//   ability "i","v"                        persp "fov","aspect","near","far"
//   rs "s","v" / getrs "s","v"             modelview "m"
//   tss "st","t","v" / tex "st","h"        dpvb "prim","vb","start","n"
//   find "name","crc","r" / tframe "f","r","h" / trel
//   lock "fvf","n","r" / unlock "base","bytes"
//   xform "inst" / arch "inst","r" / relh "arch"
//   bounds "arch","lod","r","e"            render "inst","t","flags","matrix","extra"
//   cam "f",... (the inner camera's calls)  probe "f",... (what RenderInstance reads)
// Floats are the eight hex digits of the f32, doubles sixteen.

#include "oracle_support.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#define G_FlmEngineOuter OracleAppearanceEngineOuter
#define FlmLookupIdValue OracleAppearanceLookupIdValue
#define FlmCreateMathEngine OracleAppearanceCreateMathEngine
#define FlmCreateAppearanceInstance OracleAppearanceCreateInstance
#include "../surfacematerials/FLAppearance.cpp"
#undef G_FlmEngineOuter
#undef FlmLookupIdValue
#undef FlmCreateMathEngine
#undef FlmCreateAppearanceInstance

using namespace oracle;

namespace
{
    std::vector<std::string> AppSplit(const std::string& Text, const std::string& Separator)
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

    unsigned long AppHex(const std::string& Text)
    {
        return strtoul(Text.c_str(), nullptr, 16);
    }

    long AppDec(const std::string& Text)
    {
        return strtol(Text.c_str(), nullptr, 10);
    }

    float AppFloat(const std::string& Text)
    {
        const unsigned long Bits = AppHex(Text);
        float Value;
        memcpy(&Value, &Bits, 4);
        return Value;
    }

    double AppDouble(const std::string& Text)
    {
        const unsigned long long Bits = strtoull(Text.c_str(), nullptr, 16);
        double Value;
        memcpy(&Value, &Bits, 8);
        return Value;
    }

    std::string AppWords(const void* Data, unsigned int Count)
    {
        const unsigned int* W = (const unsigned int*)Data;
        std::string Out;
        for (unsigned int Index = 0; Index < Count; ++Index)
            Out += Format(Index == 0 ? "%08x" : ",%08x", W[Index]);
        return Out;
    }

    std::string AppF(float Value)
    {
        return Format("%08x", FloatBits(Value));
    }

    std::string AppD(double Value)
    {
        return Format("%016llx", DoubleBits(Value));
    }

    struct FAppCameraConfig
    {
        float Position[3] = {1.0f, 2.0f, 3.0f};
        float Orientation[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        float View[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        float Unproject[3] = {0.0f, 0.0f, 1.0f};
        int Viewport[4] = {0, 0, 640, 480};
        float HalfFov = 45.0f;
        float FovY = 60.0f;
        float ZNear = 0.5f;
        float ZFar = 5000.0f;
        float Aspect = 1.25f;
        double XScale = 1.0;
        double YScale = 1.0;
        int ProjectResult = 1;
        float Project[3] = {320.0f, 240.0f, 0.5f};
        int ClassifyResult = 3;
    };

    struct FAppSphere
    {
        float C[3];
        float R;
    };

    class FAppMaterial;

    struct FAppState
    {
        Suite* Out = nullptr;
        unsigned int Case = 0;
        std::map<std::string, bool> Fails;
        std::map<int, int> MatResults;
        std::map<int, int> MatIds;
        bool bAbility = true;
        unsigned long Ability = 1;
        int ShadowFind = -1;
        int ShadowFrame = 0;
        unsigned long ShadowHandle = 0;
        int LockResult = 0;
        unsigned long NextBase = 30;
        std::map<unsigned long, std::vector<float>> Transforms;
        std::map<unsigned long, long> Archetypes;
        std::map<long, int> BoundsResults;
        std::map<long, std::vector<float>> Bounds;
        std::map<int, unsigned long> RenderStates;
        FAppCameraConfig Camera;
        std::vector<FAppSphere> Probe;
        bool bProbe = false;
        unsigned long Vis[3] = {0, 0, 0};
        std::vector<FAppMaterial*> Materials;

        void Event(const std::string& Fields)
        {
            Out->Line(Format("{\"case\":\"%u\",", Case) + Fields + "}");
        }

        bool Failed(const char* Face) const
        {
            return Fails.count(Face) != 0;
        }
    };

    FAppState GApp;

    float GAppIdentity[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};

    void __cdecl AppDump(int Flags, const char* Pattern, ...)
    {
        char Text[1024];
        va_list Args;
        va_start(Args, Pattern);
        vsnprintf(Text, sizeof(Text), Pattern, Args);
        va_end(Args);
        GApp.Event(Format("\"ev\":\"dump\",\"flags\":\"%x\",\"text\":\"", Flags) + Hex(Text, strlen(Text)) + "\"");
    }

    // --- the override materials -------------------------------------------------------------

    class FAppMaterial final : public IMaterial
    {
    public:
        int Id = 0;

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override
        {
            GApp.Event(Format("\"ev\":\"mrel\",\"m\":\"%d\"", Id));
            return 0;
        }
        GENRESULT NEURONCOMPONENTS_API Initialize(INeuronComponent* Outer) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LoadFromFileSystem(void* FileSystem) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SaveProperties(void* FileSystem) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ApplyVerify(void* Context, void* Extra) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ResolveTextures(float Delta) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Bind(void* Context) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Teardown(void* Context) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Submit(void* Context, unsigned long PrimType, unsigned long Start,
                                              unsigned long Count, unsigned long IbStart, unsigned long IndexSpan,
                                              unsigned long Flags) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetName(const char* Name) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetName(char* Out, unsigned long Size) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CopyClassName(char* Out, unsigned long Size) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetPassCount(int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetRenderFlags(unsigned long* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetParameterCount(unsigned long* OutCount) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetParameterDescriptor(unsigned long Index, void* OutDesc) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API GetParameterValue(unsigned long Index, void* InOutDesc) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetParameterValue(unsigned long Index, void* Value) override { return GR_GENERIC; }
        const char* __thiscall ClassName() override { return ""; }
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

    int OverrideIndex(const char* Name)
    {
        for (int Index = 0; Index < kOverrideCount; ++Index)
        {
            if (strcmp(kOverrideNames[Index], Name) == 0)
                return Index;
        }
        return -1;
    }

    FAppMaterial* MaterialById(int Id)
    {
        for (size_t Index = 0; Index < GApp.Materials.size(); ++Index)
        {
            if (GApp.Materials[Index]->Id == Id)
                return GApp.Materials[Index];
        }
        FAppMaterial* Made = new FAppMaterial();
        Made->Id = Id;
        GApp.Materials.push_back(Made);
        return Made;
    }

    std::string MaterialText(void* Material)
    {
        if (Material == nullptr)
            return "-";
        return Format("%d", ((FAppMaterial*)(IMaterial*)Material)->Id);
    }

    class FAppLibrary final : public IMaterialLibrary
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API FindOrCreate(const char* Name, const char* Type, INeuronComponent* AggregateOuter,
                                                    IMaterial** Out) override
        {
            const int Index = OverrideIndex(Name);
            const int Result = GApp.MatResults.count(Index) != 0 ? GApp.MatResults[Index] : 0;
            const int Id = GApp.MatIds.count(Index) != 0 ? GApp.MatIds[Index] : Index + 1;
            *Out = Id == 0 ? nullptr : (IMaterial*)MaterialById(Id);
            GApp.Event(Format("\"ev\":\"findorcreate\",\"name\":\"%s\",\"type\":\"%s\",\"outer\":\"%d\",\"r\":\"%d\","
                              "\"m\":\"%s\"",
                              HexC(Name).c_str(), HexC(Type).c_str(), AggregateOuter != nullptr ? 1 : 0, Result,
                              MaterialText(*Out).c_str()));
            return Result;
        }
        GENRESULT NEURONCOMPONENTS_API RemoveMaterial(unsigned long GroupId, void* Record) override
        {
            const char* Name = *(const char* const*)Record;
            const unsigned long Crc = ((const unsigned long*)Record)[1];
            GApp.Event(Format("\"ev\":\"removemat\",\"group\":\"%lu\",\"name\":\"%s\",\"crc\":\"%08lx\"", GroupId,
                              HexC(Name).c_str(), Crc));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetGlobalRecord(unsigned long Index, const unsigned long* Value) override
        {
            GApp.Event(Format("\"ev\":\"global\",\"i\":\"%lu\",\"v\":\"%lu\"", Index, *Value));
            return GR_OK;
        }
        // The slots FLAppearance does not call.
        GENRESULT NEURONCOMPONENTS_API FindMaterialByCrc(void* Query, IMaterial** Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API AddMapRule(int Key, const char* Pattern, const char* TypeName) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API FindMapRule(int Key, const char* Pattern, void* Unused, char* OutType) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API RemapType(const char* Name, const char* Type, void* Unused, char* OutType) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API RemoveMapRule(int Key, const char* Pattern) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API AllocateGroup(unsigned long* OutId) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LoadFromFileSystem(void* FileSystem, void* Extra) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UnsupportedOperation(void* Arg1, void* Arg2) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DeleteGroup(int GroupId) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ApplyVerifyGroup(int GroupId, void* Context, void* Extra) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ResolveTexturesGroup(int GroupId, float Delta) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetGroupName(int GroupId, const char* Name) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetGroupName(int GroupId, char* Buffer, unsigned long Size) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API AddMaterial(int GroupId, void* Record, IMaterial* Material) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetGroupCount(unsigned long* OutCount) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API EnumGroup(unsigned long Index, unsigned long* OutId) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetGroupMaterialCount(unsigned long GroupId, int* OutCount) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API EnumGroupMaterial(unsigned long GroupId, unsigned long Index, IMaterial** Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetGlobalRecordValue(unsigned long Index, const unsigned long* Value) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetGlobalRecordValue(unsigned long Index, unsigned long* OutValue) override { return GR_GENERIC; }
        FMaterialLibraryRecord* NEURONCOMPONENTS_API CreateAfterMap(const char* Name, const char* Type, unsigned char bRemap, INeuronComponent* AggregateOuter) override { return nullptr; }
    };

    // --- the renderer -------------------------------------------------------------------------

    class FAppPipeline final : public IRenderPipeline
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API QueryDeviceAbility(unsigned int Index, unsigned long* Out) override
        {
            if (!GApp.bAbility)
            {
                GApp.Event(Format("\"ev\":\"ability\",\"i\":\"%u\",\"v\":\"-\"", Index));
                return GR_GENERIC;
            }
            *Out = GApp.Ability;
            GApp.Event(Format("\"ev\":\"ability\",\"i\":\"%u\",\"v\":\"%lu\"", Index, GApp.Ability));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetPerspective(float FovDeg, float Aspect, float NearPlane, float FarPlane) override
        {
            GApp.Event("\"ev\":\"persp\",\"fov\":\"" + AppF(FovDeg) + "\",\"aspect\":\"" + AppF(Aspect) +
                       "\",\"near\":\"" + AppF(NearPlane) + "\",\"far\":\"" + AppF(FarPlane) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetRenderState(D3DRENDERSTATETYPE State, int Value) override
        {
            GApp.RenderStates[(int)State] = (unsigned long)Value;
            GApp.Event(Format("\"ev\":\"rs\",\"s\":\"%u\",\"v\":\"%lu\"", (unsigned int)State, (unsigned long)Value));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetRenderState(D3DRENDERSTATETYPE State, int* Out) override
        {
            const std::map<int, unsigned long>::const_iterator Found = GApp.RenderStates.find((int)State);
            const unsigned long Value = Found == GApp.RenderStates.end() ? 7 : Found->second;
            *Out = (int)Value;
            GApp.Event(Format("\"ev\":\"getrs\",\"s\":\"%u\",\"v\":\"%lu\"", (unsigned int)State, Value));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetModelview(const float* M) override
        {
            GApp.Event("\"ev\":\"modelview\",\"m\":\"" + AppWords(M, 12) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageState(int Stage, int Type, int Value) override
        {
            GApp.Event(Format("\"ev\":\"tss\",\"st\":\"%d\",\"t\":\"%d\",\"v\":\"%lu\"", Stage, Type, (unsigned long)Value));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageTexture(int Stage, void* Handle) override
        {
            GApp.Event(Format("\"ev\":\"tex\",\"st\":\"%d\",\"h\":\"%lu\"", Stage, (unsigned long)(uintptr_t)Handle));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API DrawPrimitiveVb(unsigned long Prim, void* Vb, unsigned int StartVertex,
                                                       unsigned int Count, void* Unused) override
        {
            GApp.Event(Format("\"ev\":\"dpvb\",\"prim\":\"%lu\",\"vb\":\"%lu\",\"start\":\"%u\",\"n\":\"%u\"", Prim,
                              (unsigned long)(uintptr_t)Vb, StartVertex, Count));
            return GR_OK;
        }
        // The slots FLAppearance does not call.
        GENRESULT NEURONCOMPONENTS_API GetPipelineState(int Id, int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetWorld(const float* M) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetView(const float* M) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Startup(const char* SectionName) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ShutdownPipeline() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetPipelineState(int Id, int Value) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDeviceInfo(void* Out520) override { return GR_GENERIC; }
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
        GENRESULT NEURONCOMPONENTS_API GetView(float* Out12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetModelview(float* Out12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetProjection(const float* M16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetProjection(float* Out16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetLookat(float EyeX, float EyeY, float EyeZ, float AtX, float AtY, float AtZ, float UpX, float UpY, float UpZ) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetOrtho(float Left, float Right, float Bottom, float Top, float NearPlane, float FarPlane) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetLight(void* Handle, const D3DLIGHT8* Light) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DestroyLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetLight(void* Handle, D3DLIGHT8* OutLight) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetLightEnable(void* Handle, int Enable) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetLightEnable(void* Handle, int* OutEnable) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetMaterial(const D3DMATERIAL8* Material) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetMaterial(D3DMATERIAL8* Material) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CreateTexture(int Width, int Height, unsigned int* Format, unsigned int Levels, unsigned int Flags, void** OutHandle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DestroyTexture(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API IsTexture(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LockTexture(void* Handle, int Level, void* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UnlockTexture(void* Handle, int Level) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureFormat(void* Handle, int* OutFormat) override { return GR_GENERIC; }
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
        GENRESULT NEURONCOMPONENTS_API SetTextureStageTransform(int Stage, const float* Matrix16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageTransform(int Stage, float* OutMatrix16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageTexture(int Stage, void** OutHandle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API VerifyState() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DrawPrimitive(unsigned long Prim, unsigned int Fvf, const void* Verts, unsigned int Count, void* Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitiveUp(unsigned long Prim, unsigned int Fvf, const void* Verts, unsigned int VertCount, const void* Indices, unsigned int IndexCount, unsigned long Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitiveVb(unsigned long Prim, void* Vb, unsigned int BaseVertex, unsigned int NumVerts, const void* Indices, unsigned int IndexCount, unsigned long Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API AddLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API RemoveLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UpdateLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetWorldN(int WorldIndex, const float* M12) override { return GR_GENERIC; }
    };

    class FAppVbMgr final : public IVertexBufferManager
    {
    public:
        std::vector<unsigned char> Memory;
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API LockVertexMemory(unsigned int Fvf, unsigned int Count, FVertexMemoryLock* Out) override
        {
            GApp.Event(Format("\"ev\":\"lock\",\"fvf\":\"%x\",\"n\":\"%u\",\"r\":\"%d\"", Fvf, Count, GApp.LockResult));
            if (GApp.LockResult != 0)
                return GApp.LockResult;
            // 0x142: position, diffuse, one 2D texture coordinate.
            Memory.assign((size_t)Count * 24, 0);
            Out->Listing.VertexBuffer = (void*)(uintptr_t)900;
            Out->Listing.Fvf = Fvf;
            Out->Listing.BaseVertex = GApp.NextBase;
            GApp.NextBase += Count;
            Out->Vertices = Memory.data();
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API UnlockVertexMemory(FVertexMemoryLock* Out) override
        {
            GApp.Event(Format("\"ev\":\"unlock\",\"base\":\"%lu\",\"bytes\":\"", Out->Listing.BaseVertex) +
                       Hex(Memory.data(), Memory.size()) + "\"");
            return GR_OK;
        }
        // The slots FLAppearance does not call.
        GENRESULT NEURONCOMPONENTS_API CopyVertexData(void* Dest, unsigned int Fvf, const FRpVertexSrcDesc* Source,
                                                      unsigned int Start, unsigned int Count) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API VbMgrInit(void* A, void* B, void* C, void* D) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API VbMgrReleaseAll() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UnsupportedManagerOperation() override { return GR_GENERIC; }
    };

    int GAppShadowRecord = 0;

    class FAppTextures final : public ITextureLibrary2
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
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
            GApp.Event(Format("\"ev\":\"find\",\"name\":\"%s\",\"crc\":\"%08lx\",\"r\":\"%d\"", HexC(Name).c_str(), Crc,
                              GApp.ShadowFind));
            if (GApp.ShadowFind == 0)
                *OutTex = &GAppShadowRecord;
            return GApp.ShadowFind;
        }
        GENRESULT NEURONCOMPONENTS_API GetTextureFrame(void* Tex, int Frame, unsigned long* Out) override
        {
            GApp.Event(Format("\"ev\":\"tframe\",\"f\":\"%d\",\"r\":\"%d\",\"h\":\"%lu\"", Frame, GApp.ShadowFrame,
                              GApp.ShadowHandle));
            if (GApp.ShadowFrame >= 0)
            {
                Out[0] = GApp.ShadowHandle;
                Out[1] = 0;
                Out[2] = 0;
                Out[3] = 0x3F800000;
                Out[4] = 0x3F800000;
            }
            return GApp.ShadowFrame;
        }
        GENRESULT NEURONCOMPONENTS_API ReleaseTextureRecord(void* Record) override
        {
            GApp.Event("\"ev\":\"trel\"");
            return GR_OK;
        }
        // The slots FLAppearance does not call.
        GENRESULT NEURONCOMPONENTS_API SetGenericSlot(int Index, unsigned long Value) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetGenericSlot(int Index, unsigned long* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LoadTextureGroup(void* FileSystem, int* OutGroupId) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LoadTextureLibrary(void* FileSystem, const char* Name) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GarbageCollect(int GroupId) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API TickAllPlayback(int Unused, float DeltaSeconds) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetGroupName(int Id, const char* Name) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetGroupName(int Id, char* Buf, unsigned long Size) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API FindOrCreateByName(const char* Name, void** OutTex) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CreatePlaybackGroup(void* SourceRecord, int* OutId) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API AddRefById(int Id) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DecRefById(int Id) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CopyName(void* Node, char* Buf, unsigned long Size) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetRecordName(void* Record, const char* Name) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetSubTexture(void* Record, int Index, int TextureId) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetFrameHandle(void* Record, int Index, unsigned long Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetFrameRect(void* Record, int Index, float U0, float V0, float U1, float V1) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetGroupPixelFormat(void* Record, void* Unused, int* OutFormat) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetTileRate(void* Record, float Value) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTileRate(void* Record, float* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetFrameCount(void* Record, int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ResolveTextureRecord(int Id, void** Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetFrame(int Id, int Frame, unsigned long* Out) override { return GR_GENERIC; }
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
        GENRESULT NEURONCOMPONENTS_API EnumTexture(int Id, int Index, void** OutRecord) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API AddSubTexture(int GroupId, void* Record) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API RemoveSubTexture(int GroupId, void* Record) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API PickGpuFormat(const unsigned long* Src, unsigned long* OutId) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API AllocateNewTextureId(int* OutId) override { return GR_GENERIC; }
    };

    // --- the inner camera -----------------------------------------------------------------------

    class FAppCamera final : public ICamera
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        float const* NEURONCOMPONENTS_API GetPosition() override
        {
            GApp.Event("\"ev\":\"cam\",\"f\":\"pos\"");
            return GApp.Camera.Position;
        }
        float const* NEURONCOMPONENTS_API GetOrientation() override
        {
            GApp.Event("\"ev\":\"cam\",\"f\":\"orient\"");
            return GApp.Camera.Orientation;
        }
        int const* NEURONCOMPONENTS_API GetViewport() override
        {
            GApp.Event("\"ev\":\"cam\",\"f\":\"vp\"");
            return GApp.Camera.Viewport;
        }
        float NEURONCOMPONENTS_API GetHalfFovXDeg() override
        {
            GApp.Event("\"ev\":\"cam\",\"f\":\"hfov\"");
            return GApp.Camera.HalfFov;
        }
        float NEURONCOMPONENTS_API GetFovYDeg() override
        {
            GApp.Event("\"ev\":\"cam\",\"f\":\"fovy\"");
            return GApp.Camera.FovY;
        }
        float NEURONCOMPONENTS_API GetZNear() override
        {
            GApp.Event("\"ev\":\"cam\",\"f\":\"znear\"");
            return GApp.Camera.ZNear;
        }
        float NEURONCOMPONENTS_API GetZFar() override
        {
            GApp.Event("\"ev\":\"cam\",\"f\":\"zfar\"");
            return GApp.Camera.ZFar;
        }
        float NEURONCOMPONENTS_API GetAspect() override
        {
            GApp.Event("\"ev\":\"cam\",\"f\":\"aspect\"");
            return GApp.Camera.Aspect;
        }
        double NEURONCOMPONENTS_API GetXScaleOverNear() override
        {
            GApp.Event("\"ev\":\"cam\",\"f\":\"xscale\"");
            return GApp.Camera.XScale;
        }
        double NEURONCOMPONENTS_API GetYScaleOverNear() override
        {
            GApp.Event("\"ev\":\"cam\",\"f\":\"yscale\"");
            return GApp.Camera.YScale;
        }
        int NEURONCOMPONENTS_API Project(float* OutX, float* OutY, float* OutZ, float const* World) override
        {
            GApp.Event("\"ev\":\"cam\",\"f\":\"proj\",\"w\":\"" + AppWords(World, 3) + "\"");
            *OutX = GApp.Camera.Project[0];
            *OutY = GApp.Camera.Project[1];
            *OutZ = GApp.Camera.Project[2];
            return GApp.Camera.ProjectResult;
        }
        void NEURONCOMPONENTS_API Unproject(float* OutWorld3, float ScreenX, float ScreenY) override
        {
            GApp.Event("\"ev\":\"cam\",\"f\":\"unproj\",\"x\":\"" + AppF(ScreenX) + "\",\"y\":\"" + AppF(ScreenY) + "\"");
            memcpy(OutWorld3, GApp.Camera.Unproject, 12);
        }
        int NEURONCOMPONENTS_API ClassifySphere(float const* C, float R) override
        {
            GApp.Event("\"ev\":\"cam\",\"f\":\"classify\",\"c\":\"" + AppWords(C, 3) + "\",\"r\":\"" + AppF(R) + "\"");
            return GApp.Camera.ClassifyResult;
        }
        float const* NEURONCOMPONENTS_API GetViewTransform() override
        {
            GApp.Event("\"ev\":\"cam\",\"f\":\"view\"");
            return GApp.Camera.View;
        }
        // The slots FLAppearance's wrapper does not have.
        int NEURONCOMPONENTS_API ProjectVec(float* Out3, float const* World) override { return GR_GENERIC; }
        void GetVelocity(float* Out3) override {}
        int SetProperty(char const* Key, char const* Value) override { return GR_GENERIC; }
    };

    FAppCamera GAppCamera;

    // The camera RenderInstance is given, read through the slots ICamera and FCameraBlob share.
    void ProbeCamera(ICamera* Camera)
    {
        const float* Position = Camera->GetPosition();
        GApp.Event("\"ev\":\"probe\",\"f\":\"pos\",\"v\":\"" + AppWords(Position, 3) + "\"");
        const float* Orientation = Camera->GetOrientation();
        GApp.Event("\"ev\":\"probe\",\"f\":\"orient\",\"v\":\"" + AppWords(Orientation, 9) + "\"");
        const int* Viewport = Camera->GetViewport();
        GApp.Event(Format("\"ev\":\"probe\",\"f\":\"vp\",\"v\":\"%d,%d,%d,%d\"", Viewport[0], Viewport[1], Viewport[2],
                          Viewport[3]));
        const float HalfFov = Camera->GetHalfFovXDeg();
        GApp.Event("\"ev\":\"probe\",\"f\":\"hfov\",\"v\":\"" + AppF(HalfFov) + "\"");
        const float FovY = Camera->GetFovYDeg();
        GApp.Event("\"ev\":\"probe\",\"f\":\"fovy\",\"v\":\"" + AppF(FovY) + "\"");
        const float ZNear = Camera->GetZNear();
        GApp.Event("\"ev\":\"probe\",\"f\":\"znear\",\"v\":\"" + AppF(ZNear) + "\"");
        const float ZFar = Camera->GetZFar();
        GApp.Event("\"ev\":\"probe\",\"f\":\"zfar\",\"v\":\"" + AppF(ZFar) + "\"");
        const float Aspect = Camera->GetAspect();
        GApp.Event("\"ev\":\"probe\",\"f\":\"aspect\",\"v\":\"" + AppF(Aspect) + "\"");
        const double XScale = Camera->GetXScaleOverNear();
        GApp.Event("\"ev\":\"probe\",\"f\":\"xscale\",\"v\":\"" + AppD(XScale) + "\"");
        const double YScale = Camera->GetYScaleOverNear();
        GApp.Event("\"ev\":\"probe\",\"f\":\"yscale\",\"v\":\"" + AppD(YScale) + "\"");
        const float World[3] = {0.5f, -1.25f, 40.0f};
        float Projected[3] = {0.0f, 0.0f, 0.0f};
        const int ProjectResult = Camera->Project(&Projected[0], &Projected[1], &Projected[2], World);
        GApp.Event(Format("\"ev\":\"probe\",\"f\":\"proj\",\"r\":\"%d\",\"v\":\"", ProjectResult) +
                   AppWords(Projected, 3) + "\"");
        float Unprojected[3] = {0.0f, 0.0f, 0.0f};
        Camera->Unproject(Unprojected, 320.0f, 240.0f);
        GApp.Event("\"ev\":\"probe\",\"f\":\"unproj\",\"v\":\"" + AppWords(Unprojected, 3) + "\"");
        for (size_t Index = 0; Index < GApp.Probe.size(); ++Index)
        {
            const FAppSphere& Sphere = GApp.Probe[Index];
            const int Rank = Camera->ClassifySphere(Sphere.C, Sphere.R);
            GApp.Event(Format("\"ev\":\"probe\",\"f\":\"classify\",\"i\":\"%u\",\"v\":\"%d\"", (unsigned int)Index, Rank));
        }
        const float* View = Camera->GetViewTransform();
        GApp.Event("\"ev\":\"probe\",\"f\":\"view\",\"v\":\"" + AppWords(View, 12) + "\"");
    }

    class FAppRenderer final : public IRenderer2
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        int NEURONCOMPONENTS_API GetBounds(void* Id, float Lod, void* Out) override
        {
            const long Arch = (long)(intptr_t)Id;
            const int Result = GApp.BoundsResults.count(Arch) != 0 ? GApp.BoundsResults[Arch] : -1;
            std::string Written = "-";
            if (GApp.Bounds.count(Arch) != 0)
            {
                memcpy(Out, GApp.Bounds[Arch].data(), 24);
                Written = AppWords(GApp.Bounds[Arch].data(), 6);
            }
            GApp.Event(Format("\"ev\":\"bounds\",\"arch\":\"%ld\",\"lod\":\"", Arch) + AppF(Lod) +
                       Format("\",\"r\":\"%d\",\"e\":\"", Result) + Written + "\"");
            return Result;
        }
        int NEURONCOMPONENTS_API RenderInstance(void* Camera, void* Inst, float Threshold, unsigned long Flags, void* Matrix,
                                                void* Extra) override
        {
            GApp.Event(Format("\"ev\":\"render\",\"inst\":\"%lu\",\"t\":\"", (unsigned long)(uintptr_t)Inst) +
                       AppF(Threshold) +
                       Format("\",\"flags\":\"%lx\",\"matrix\":\"%s\",\"extra\":\"", Flags, Matrix != nullptr ? "set" : "-") +
                       MaterialText(Extra) + "\"");
            if (GApp.bProbe)
                ProbeCamera((ICamera*)Camera);
            G_VisibleCounterA += GApp.Vis[0];
            G_VisibleCounterB += GApp.Vis[1];
            G_VisibleCounterC += GApp.Vis[2];
            return 4;
        }
        // The slots FLAppearance does not call.
        int NEURONCOMPONENTS_API QueryGeometry(void* A, float Lod, void* C, void* D, void* E, void* F, void* G) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetCentroid(void* Id, float Lod, void* Out) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API NoOp(void* A, void* B, void* C, void* D, void* E, void* F, void* G) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetInstanceLodCount(void* Inst, void* Out) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetInstanceCurrentLod(void* Inst, void* Out) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API SetInstanceLod(void* Inst, int Index) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetArchetypeLodCount(void* Id, int* Out) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetArchetypeCurrentLod(void* Id, int* Out) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API SetArchetypeLod(void* Id, int Index) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetArchetypeLodRange(void* Id, int Index, void* OutA, void* OutB) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API SetArchetypeLodRange(void* Id, int Index, float A, float B) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetInstanceLodRange(void* Inst, int Index, void* OutA, void* OutB) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API SetInstanceLodRange(void* Inst, int Index, float A, float B) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API QueryGeometryAtLod(void* A, void* B, void* C, void* D, void* E, void* F, void* G) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API SetArchetypeRootLodSelection(void* Id, void* Unused, unsigned long Flags) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API SetInstanceRootLodSelection(void* Id, void* A, void* B) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetArchetypeRootLodSelection(void* Key, void* Unused, unsigned long* Out) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetInstanceRootLodSelection(void* Id, void* A, void* B) override { return GR_GENERIC; }
    };

    FAppLibrary GAppLibrary;
    FAppPipeline GAppPipeline;
    FAppVbMgr GAppVbMgr;
    FAppTextures GAppTextures;
    FAppRenderer GAppRenderer;

    // --- the engine (the aggregate's outer) and the system ----------------------------------

    void QiEvent(const char* On, const char* Face, bool bOk)
    {
        GApp.Event(Format("\"ev\":\"qi\",\"on\":\"%s\",\"face\":\"%s\",\"r\":\"%d\"", On, Face, bOk ? 0 : -1));
    }

    class FAppEngine final : public IEngine
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            *Out = nullptr;
            if (strcmp(Iid, Iids::Renderer) == 0)
            {
                const bool bOk = !GApp.Failed("renderer");
                QiEvent("engine", "renderer", bOk);
                if (!bOk)
                    return GR_INTERFACE_UNSUPPORTED;
                *Out = (IRenderer2*)&GAppRenderer;
                return GR_OK;
            }
            QiEvent("engine", "other", false);
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        float* NEURONCOMPONENTS_API GetInstanceTransform(void* Instance) override
        {
            const unsigned long I = (unsigned long)(uintptr_t)Instance;
            GApp.Event(Format("\"ev\":\"xform\",\"inst\":\"%lu\"", I));
            std::map<unsigned long, std::vector<float>>::iterator It = GApp.Transforms.find(I);
            return It == GApp.Transforms.end() ? GAppIdentity : It->second.data();
        }
        void* NEURONCOMPONENTS_API GetInstanceArchetype(void* Instance) override
        {
            const unsigned long I = (unsigned long)(uintptr_t)Instance;
            const long R = GApp.Archetypes.count(I) != 0 ? GApp.Archetypes[I] : -1;
            GApp.Event(Format("\"ev\":\"arch\",\"inst\":\"%lu\",\"r\":\"%ld\"", I, R));
            return (void*)(intptr_t)R;
        }
        unsigned long NEURONCOMPONENTS_API ReleaseHandle(void* Handle) override
        {
            GApp.Event(Format("\"ev\":\"relh\",\"arch\":\"%ld\"", (long)(intptr_t)Handle));
            return 0;
        }
        // The slots FLAppearance does not call.
        GENRESULT NEURONCOMPONENTS_API GetInstanceExtent(void* Instance, unsigned long, float* OutR, float* OutC) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetInstanceExtent(void* Instance, unsigned long, float R, const float* C) override { return GR_GENERIC; }
        void* NEURONCOMPONENTS_API GetRelatedInstance(void* From, unsigned long, void* Cursor) override { return (void*)(intptr_t)-1; }
        void* NEURONCOMPONENTS_API GetInstanceRoot(void* Instance) override { return Instance; }
        GENRESULT NEURONCOMPONENTS_API LoadEngineComponents(INeuronComponent* SystemContainer) override { return GR_GENERIC; }
        unsigned long NEURONCOMPONENTS_API EnumerateEngineComponents( int(__cdecl* Callback)(const char*, void*)) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API NotifyEngineComponents(void* Arg) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetSearchPath(const char* Path) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API QuerySearchPath(char* Dest, unsigned long MaxChars) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetFilesystem(INeuronComponent* NewPath) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetSearchPath(void** Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CreateFileSystem(const char* Path, void** Out, void* Factory) override { return GR_GENERIC; }
        void* NEURONCOMPONENTS_API CreateCompoundArchetype(FEngineName* Name) override { return nullptr; }
        void* NEURONCOMPONENTS_API CreateArchetypeHandle(FEngineName* Name, void* FileSystem) override { return nullptr; }
        void* NEURONCOMPONENTS_API DuplicateArchetype(void* ArchIn, FEngineName* Name) override { return nullptr; }
        unsigned long NEURONCOMPONENTS_API AddRefHandle(void* Handle) override { return 0; }
        FEngineName* NEURONCOMPONENTS_API GetArchetypeName(FEngineName* Out, void* Handle) override { return nullptr; }
        void* NEURONCOMPONENTS_API FindArchetype(FEngineName* Name) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API QueryEngineComponent(int Id, void* Name, void** Out) override { return GR_GENERIC; }
        unsigned long NEURONCOMPONENTS_API IsCompoundArchetype(void* Handle) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API EnumerateArchetypeConstraints( void* Handle, int(__cdecl* Callback)(void*, void*, void*, void*), void* Context) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API EnumerateArchetypeChildren( void* Handle, int(__cdecl* Callback)(void*, void*, int, void*), void* Context) override { return GR_GENERIC; }
        void* NEURONCOMPONENTS_API CreateInstanceFromName(FEngineName* Name, void* FileSystem, void* Transform) override { return nullptr; }
        void* NEURONCOMPONENTS_API CreateInstance(void* Handle, void* Transform) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API DestroyInstance(void* Instance) override { return GR_GENERIC; }
        FEngineName* NEURONCOMPONENTS_API GetInstanceName(FEngineName* Out, void* Instance) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API UpdateInstance(void* Instance, unsigned long Flags, void* Arg) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetInstanceTransform(void* Instance, void* Transform) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetInstanceTransformObject(void* Instance, void** Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API QueryInstance(void* Instance, const char* Iid, void** Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetInstanceUserData(void* Instance, void* Value) override { return GR_GENERIC; }
        void* NEURONCOMPONENTS_API GetInstanceUserData(void* Instance) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API SetInstancePosition(void* Instance, void* Position) override { return GR_GENERIC; }
        float* NEURONCOMPONENTS_API GetInstancePosition(void* Instance) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API SetInstanceOrientation(void* Instance, void* Orient) override { return GR_GENERIC; }
        float* NEURONCOMPONENTS_API GetInstanceOrientation(void* Instance) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API SetInstanceMatrix(void* Instance, void* Matrix) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API NoOp41(void* A, void* B) override { return GR_GENERIC; }
        float* NEURONCOMPONENTS_API GetIdentityMatrix(void* Unused) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API NoOp43(void* A, void* B) override { return GR_GENERIC; }
        float* NEURONCOMPONENTS_API GetIdentityMatrix2(void* Unused) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API SetTransformUser(void* Instance, unsigned long User) override { return GR_GENERIC; }
        unsigned long NEURONCOMPONENTS_API GetTransformUser(void* Instance) override { return 0; }
        unsigned long NEURONCOMPONENTS_API CreateJoint(void* Parent, void* Child, const unsigned long* Payload) override { return 0; }
        unsigned long NEURONCOMPONENTS_API DestroyJoint(void* Parent, void* Child) override { return 0; }
        unsigned long* NEURONCOMPONENTS_API GetInstanceRelative(void* Instance) override { return nullptr; }
        unsigned long NEURONCOMPONENTS_API SetJointParams(void* Instance, unsigned long Index, const float* Values) override { return 0; }
        unsigned long NEURONCOMPONENTS_API GetJointParams(void* Instance, unsigned long Index, float* Out) override { return 0; }
        void* NEURONCOMPONENTS_API FindMappedInstance(void* Instance) override { return nullptr; }
        void* NEURONCOMPONENTS_API GetInstanceParent(void* Instance) override { return nullptr; }
        void* NEURONCOMPONENTS_API FindArchetypeChild(void* Handle, void* ChildHandle) override { return nullptr; }
        unsigned long NEURONCOMPONENTS_API RejectOperation(void* A, void* B) override { return 0; }
    };

    FAppEngine GAppEngine;

    // The outer FLAppearance aggregates into: the engine's controlling component.
    class FAppOuter final : public INeuronComponent
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            *Out = nullptr;
            if (strcmp(Iid, Iids::Engine) == 0)
            {
                const bool bOk = !GApp.Failed("engine");
                QiEvent("outer", "engine", bOk);
                if (!bOk)
                    return GR_INTERFACE_UNSUPPORTED;
                *Out = (IEngine*)&GAppEngine;
                return GR_OK;
            }
            QiEvent("outer", "other", false);
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
    };

    FAppOuter GAppOuter;

    class FAppSystem final : public INeuronComponent
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            *Out = nullptr;
            const char* Face = "other";
            void* Answer = nullptr;
            if (strcmp(Iid, Iids::MaterialLibrary) == 0)
            {
                Face = "matlib";
                Answer = (IMaterialLibrary*)&GAppLibrary;
            }
            else if (strcmp(Iid, Iids::RenderPipeline8B) == 0)
            {
                Face = "pipeline";
                Answer = (IRenderPipeline*)&GAppPipeline;
            }
            else if (strcmp(Iid, Iids::VertexBufferManager) == 0)
            {
                Face = "vbm";
                Answer = (IVertexBufferManager*)&GAppVbMgr;
            }
            else if (strcmp(Iid, Iids::TextureLibraryA) == 0)
            {
                Face = "texlib";
                Answer = (ITextureLibrary2*)&GAppTextures;
            }
            const bool bOk = Answer != nullptr && !GApp.Failed(Face);
            QiEvent("system", Face, bOk);
            if (!bOk)
                return GR_INTERFACE_UNSUPPORTED;
            *Out = Answer;
            return GR_OK;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
    };

    FAppSystem GAppSystem;

    // --- the cases ------------------------------------------------------------------------------

    void ReadFloats(float* Out, const std::vector<std::string>& F, size_t First, size_t Count)
    {
        for (size_t Index = 0; Index < Count; ++Index)
            Out[Index] = AppFloat(F[First + Index]);
    }

    class FAppCase
    {
    public:
        FAppCase()
        {
            // FLAppearance's statics start as the process loads them, for every case.
            for (int Index = 0; Index < kOverrideCount; ++Index)
                G_Overrides[Index] = nullptr;
            G_IdValues.clear();
            G_Slot6First[0] = 0.0f;
            G_Slot6First[1] = -1.0f;
            G_Slot6First[2] = 0.0f;
            G_Slot6Second[0] = 1.0f;
            G_Slot6Second[1] = 1.0f;
            G_Slot6Second[2] = 1.0f;
            G_Slot6Flag = 0;
            G_ShadowQuadScale = 1.0f;
            OracleAppearanceEngineOuter = nullptr;
            FlmVisibleCounterReset();
        }

        ~FAppCase()
        {
            for (size_t Index = 0; Index < GApp.Materials.size(); ++Index)
                delete GApp.Materials[Index];
            GApp.Materials.clear();
            OracleAppearanceEngineOuter = nullptr;
        }

        void Op(const std::string& Fields)
        {
            GApp.Out->Line(Format("{\"case\":\"%u\",\"op\":", GApp.Case) + Fields + "}");
        }

        IFLAppearance* Face() const { return (IFLAppearance*)(FFLAppearanceBase*)Appearance; }

        void Shadows(const std::vector<std::string>& F, bool bFading)
        {
            std::vector<FShadowInstance> Instances;
            if (F[1] != "-")
            {
                const std::vector<std::string> Parts = AppSplit(F[1], ",");
                for (size_t Index = 0; Index < Parts.size(); ++Index)
                {
                    const std::vector<std::string> Pair = AppSplit(Parts[Index], ":");
                    FShadowInstance Instance;
                    Instance.Id = (void*)(uintptr_t)AppDec(Pair[0]);
                    Instance.Radius = AppFloat(Pair[1]);
                    Instances.push_back(Instance);
                }
            }
            void* const Count = (void*)(uintptr_t)Instances.size();
            void* const List = Instances.empty() ? nullptr : (void*)Instances.data();
            const GENRESULT Result = bFading ? Face()->DrawFadingShadows(&GAppCamera, List, Count, nullptr)
                                             : Face()->DrawFixedShadows(&GAppCamera, List, Count, nullptr);
            Op(Format("\"%s\",\"r\":\"%d\",\"scale\":\"", bFading ? "fading" : "fixed", (int)Result) +
               AppF(G_ShadowQuadScale) + "\"");
        }

        void Run(const std::vector<std::string>& F)
        {
            const std::string& K = F[0];
            if (K == "fail")
                GApp.Fails[F[1]] = true;
            else if (K == "mat")
            {
                const int Index = (int)AppDec(F[1]);
                GApp.MatResults[Index] = (int)AppDec(F[2]);
                GApp.MatIds[Index] = F[3] == "-" ? 0 : (int)AppDec(F[3]);
            }
            else if (K == "ability")
            {
                GApp.bAbility = F[1] != "-";
                GApp.Ability = GApp.bAbility ? (unsigned long)AppDec(F[1]) : 0;
            }
            else if (K == "shadowtex")
            {
                GApp.ShadowFind = (int)AppDec(F[1]);
                GApp.ShadowFrame = (int)AppDec(F[2]);
                GApp.ShadowHandle = (unsigned long)AppDec(F[3]);
            }
            else if (K == "lock")
                GApp.LockResult = (int)AppDec(F[1]);
            else if (K == "xf")
            {
                std::vector<float>& M = GApp.Transforms[(unsigned long)AppDec(F[1])];
                M.assign(12, 0.0f);
                ReadFloats(M.data(), F, 2, 12);
            }
            else if (K == "arch")
                GApp.Archetypes[(unsigned long)AppDec(F[1])] = AppDec(F[2]);
            else if (K == "bounds")
            {
                const long Arch = AppDec(F[1]);
                GApp.BoundsResults[Arch] = (int)AppDec(F[2]);
                if (F[3] != "-")
                {
                    std::vector<float>& E = GApp.Bounds[Arch];
                    E.assign(6, 0.0f);
                    const std::vector<std::string> Parts = AppSplit(F[3], ",");
                    ReadFloats(E.data(), Parts, 0, 6);
                }
            }
            else if (K == "cam")
            {
                FAppCameraConfig& C = GApp.Camera;
                const std::string& Field = F[1];
                if (Field == "pos")
                    ReadFloats(C.Position, F, 2, 3);
                else if (Field == "orient")
                    ReadFloats(C.Orientation, F, 2, 9);
                else if (Field == "view")
                    ReadFloats(C.View, F, 2, 12);
                else if (Field == "unproj")
                    ReadFloats(C.Unproject, F, 2, 3);
                else if (Field == "vp")
                {
                    for (int Index = 0; Index < 4; ++Index)
                        C.Viewport[Index] = (int)AppDec(F[2 + Index]);
                }
                else if (Field == "hfov")
                    C.HalfFov = AppFloat(F[2]);
                else if (Field == "fovy")
                    C.FovY = AppFloat(F[2]);
                else if (Field == "znear")
                    C.ZNear = AppFloat(F[2]);
                else if (Field == "zfar")
                    C.ZFar = AppFloat(F[2]);
                else if (Field == "aspect")
                    C.Aspect = AppFloat(F[2]);
                else if (Field == "xscale")
                    C.XScale = AppDouble(F[2]);
                else if (Field == "yscale")
                    C.YScale = AppDouble(F[2]);
                else if (Field == "proj")
                {
                    C.ProjectResult = (int)AppDec(F[2]);
                    ReadFloats(C.Project, F, 3, 3);
                }
                else if (Field == "classify")
                    C.ClassifyResult = (int)AppDec(F[2]);
            }
            else if (K == "probe")
            {
                GApp.bProbe = true;
                GApp.Probe.clear();
                if (F[1] != "-")
                {
                    const std::vector<std::string> Spheres = AppSplit(F[1], "/");
                    for (size_t Index = 0; Index < Spheres.size(); ++Index)
                    {
                        const std::vector<std::string> V = AppSplit(Spheres[Index], ",");
                        FAppSphere Sphere;
                        ReadFloats(Sphere.C, V, 0, 3);
                        Sphere.R = AppFloat(V[3]);
                        GApp.Probe.push_back(Sphere);
                    }
                }
            }
            else if (K == "vis")
            {
                for (int Index = 0; Index < 3; ++Index)
                    GApp.Vis[Index] = (unsigned long)AppDec(F[1 + Index]);
            }
            else if (K == "create")
            {
                FAggDesc18 Desc = {};
                Desc.Size = sizeof(Desc);
                Desc.NameAt4 = "FLAppearance";
                Desc.OuterAt8 = (INeuronComponent*)&GAppOuter;
                Desc.InnerOutAtC = (void**)&Inner;
                Desc.SystemAt14 = GApp.Failed("system") ? nullptr : (INeuronComponent*)&GAppSystem;
                void* Instance = nullptr;
                const GENRESULT Result = OracleAppearanceCreateInstance(&Desc, &Instance);
                Appearance = Instance;
                Op(Format("\"create\",\"r\":\"%d\",\"made\":\"%d\"", (int)Result, Instance != nullptr ? 1 : 0));
            }
            else if (K == "init")
            {
                IEngineComponent* Engine = nullptr;
                Inner->QueryInterface(Iids::EngineComponent, (void**)&Engine);
                const int Result = Engine->Initialize();
                Engine->Release();
                Op(Format("\"init\",\"r\":\"%d\",\"engine\":\"%d\"", Result,
                          OracleAppearanceEngineOuter != nullptr ? 1 : 0));
            }
            else if (K == "set")
            {
                const GENRESULT Result = Face()->SetInstanceValue((int)AppDec(F[1]), (void*)(uintptr_t)AppHex(F[2]));
                Op(Format("\"set\",\"r\":\"%d\"", (int)Result));
            }
            else if (K == "get")
            {
                void* Value = (void*)(uintptr_t)0xCDCDCDCD;
                const GENRESULT Result = Face()->GetInstanceValue((int)AppDec(F[1]), &Value);
                Op(Format("\"get\",\"r\":\"%d\",\"v\":\"%08lx\"", (int)Result, (unsigned long)(uintptr_t)Value));
            }
            else if (K == "remove")
            {
                const GENRESULT Result = Face()->RemoveInstanceValue((int)AppDec(F[1]));
                Op(Format("\"remove\",\"r\":\"%d\"", (int)Result));
            }
            else if (K == "lookup")
            {
                void* Value = (void*)(uintptr_t)0xCDCDCDCD;
                const bool bFound = OracleAppearanceLookupIdValue((int)AppDec(F[1]), &Value);
                Op(Format("\"lookup\",\"r\":\"%d\",\"v\":\"%08lx\"", bFound ? 1 : 0, (unsigned long)(uintptr_t)Value));
            }
            else if (K == "destroy")
            {
                IEngineComponent* Engine = nullptr;
                Inner->QueryInterface(Iids::EngineComponent, (void**)&Engine);
                const int Result = Engine->DestroyInstance((void*)(uintptr_t)AppDec(F[1]));
                Engine->Release();
                Op(Format("\"destroy\",\"r\":\"%d\"", Result));
            }
            else if (K == "light" || K == "lighton")
            {
                GENRESULT Result;
                if (K == "light")
                {
                    float First[3];
                    float Second[3];
                    ReadFloats(First, F, 1, 3);
                    ReadFloats(Second, F, 4, 3);
                    Result = Face()->SetDirectionalLight(First, Second);
                }
                else
                    Result = Face()->SetDirectionalLightEnabled((unsigned char)AppDec(F[1]));
                Op(Format("\"%s\",\"r\":\"%d\",\"on\":\"%u\",\"d\":\"", K.c_str(), (int)Result,
                          (unsigned int)G_Slot6Flag) +
                   AppWords(G_Slot6First, 3) + "\",\"c\":\"" + AppWords(G_Slot6Second, 3) + "\"");
            }
            else if (K == "override")
            {
                void* Out = (void*)(uintptr_t)0xCDCDCDCD;
                const GENRESULT Result = Face()->GetOverrideMaterial((unsigned long)AppDec(F[1]), &Out);
                Op(Format("\"override\",\"r\":\"%d\",\"m\":\"", (int)Result) + MaterialText(Out) + "\"");
            }
            else if (K == "backdrop")
            {
                const GENRESULT Result = Face()->DrawBackdrop(&GAppCamera, (void*)(uintptr_t)AppDec(F[1]));
                Op(Format("\"backdrop\",\"r\":\"%d\"", (int)Result));
            }
            else if (K == "stars")
            {
                const GENRESULT Result = Face()->DrawStarsphere(&GAppCamera, (void*)(uintptr_t)AppDec(F[1]));
                Op(Format("\"stars\",\"r\":\"%d\"", (int)Result));
            }
            else if (K == "fading")
                Shadows(F, true);
            else if (K == "fixed")
                Shadows(F, false);
            else if (K == "count")
            {
                unsigned long Out[3] = {0xCDCDCDCD, 0xCDCDCDCD, 0xCDCDCDCD};
                const GENRESULT Result =
                    Face()->CountVisibleGeometry(&GAppCamera, (void*)(uintptr_t)AppDec(F[1]), Out);
                Op(Format("\"count\",\"r\":\"%d\",\"v\":\"%08lx,%08lx,%08lx\"", (int)Result, Out[0], Out[1], Out[2]));
            }
            else if (K == "blob")
            {
                FCameraBlob* Blob = (FCameraBlob*)((FFLAppearanceBase*)Appearance)->Scratch88_;
                if (Blob == nullptr)
                    Op("\"blob\",\"v\":\"-\"");
                else
                    Op(Format("\"blob\",\"b8\":\"%u\",\"b9\":\"%u\",\"a\":\"", (unsigned int)Blob->Byte8(),
                              (unsigned int)Blob->Byte9()) +
                       AppWords(Blob->TransformA(), 12) + "\",\"b\":\"" + AppWords(Blob->TransformB(), 12) +
                       "\",\"x\":\"" + AppWords(Blob->Vec6C(), 3) + "\",\"y\":\"" + AppWords(Blob->Vec78(), 3) + "\"");
            }
            else if (K == "release")
            {
                const unsigned long Result = Inner->Release();
                Appearance = nullptr;
                Inner = nullptr;
                Op(Format("\"release\",\"r\":\"%lu\"", Result));
            }
        }

    private:
        void* Appearance = nullptr;
        INeuronComponent* Inner = nullptr;
    };
}

void RunMaterialAppearanceSuite()
{
    Suite Out("material_appearance.jsonl");
    const std::vector<std::string> Lines = InputLines("material_appearance.list");
    void(__cdecl * Saved)(int, const char*, ...) = FDUMP;
    FDUMP = AppDump;
    for (unsigned int Case = 0; Case < Lines.size(); ++Case)
    {
        GApp = FAppState();
        GApp.Out = &Out;
        GApp.Case = Case;
        FAppCase Runner;
        const std::vector<std::string> Ops = AppSplit(Lines[Case], " ; ");
        for (unsigned int Op = 0; Op < Ops.size(); ++Op)
            Runner.Run(AppSplit(Ops[Op], " "));
    }
    FDUMP = Saved;
}
