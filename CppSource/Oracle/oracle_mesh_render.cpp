// P4.6 (first increment): RendComp.dll at run time -- the render manager, the levels of detail,
// the VMesh registry, groups and parts, VMeshWire, the TriMesh and SphereMesh plugins -- through
// the reference's own code, against recording services. The Rust side is
// crates/neuron-client/tests/support/mesh_render.rs with the same contract; the inputs are
// crates/neuron-client/tests/fixtures/oracle/mesh_render.list and the archives in mesh/ from
// make_mesh_fixtures.py.
//
// RendComp.dll exports nothing but DllMain; oracle_mesh_data.cpp compiles its sources for P1.7
// and includes this file at its end, so the suite reaches the reference's classes. The
// globals they reach (G_Pipeline, G_VertexBuffer, G_IndexBuffer, G_Draw, G_VbManager,
// G_MaterialLibrary, G_Engine) point at the recording fakes below; the plugins are the
// reference's, made as their factories make them (TriMesh with the client's Tristrips = 0).
//
// The archives are read by a small in-memory IFileSystem over the UTF bytes (FTreeFs): the
// calls RendComp makes (OpenNode, OpenByDesc, GetSize, Read, CloseFile, CreateInstance,
// FindFirst, FindNext) with UTF's rules for well-formed archives (names without case, the
// archive's order, a read of nothing fails). P1.7 checks the loaders on ReadFile's own UTF code.
//
// Every call writes one line {"case","ev",...} (strings throughout, hex where marked):
//   pstate "id","r": GetPipelineState, r the value written ("-" none; "pstate" configures it)
//   modelview / world / view "m": the twelve words (hex)
//   lockvm "fvf","n","r" / copyvd "fvf","start","n","src" / unlockvm "vb","base"
//   selvb "vb","r" / createvb "fvf","n","r","vb" / lockvb "vb","n","r" /
//   copyv "vb","base","start","n","src" / unlockvb "vb" / destroyvb "vb"
//   selib "ib","base","r" / createib "n","r","ib" / lockib "ib","n","r" / unlockib "ib","h" /
//   destroyib "ib" / copyidx "n","h","start"
//   dip "prim","min","nv","start","n"
//   xform "inst" / arch "inst","r" / relh "arch" / getext "inst","r","c" ("-" nothing written) /
//   setext "inst","r","c" / child "inst","prev","r" (-1 for none) / root "inst"
//   findmat "crc","r","m" / addref "m","refs" / release "m","refs" / flags "m","v" /
//   class "m","v" / pcount "m","v" / param "m","i","t","v"
//   bind / teardown "m",<context> / submit "m",<context>,"prim","start","count","ibs","span",
//   "flags","src","idx"
//   camview / viewport / fov / xscale / znear / campos / classify "c","r","res"
//   dump "flags","file","line","msg" (msg hex)
// with <context> "inst","world","view","vb","ib","rep","f20" and "src" the vertex source:
// fvf,count,flags then per stream (position, normal, diffuse, tex0, tex1)
// ;count/stride/bytes/<elements hash>/<index hash> ("-" for none). Hashes are FNV-1a 64 of the
// bytes. Every operation then writes {"case","op",...}. Floats are the eight hex digits of the
// f32 (sixteen for the f64 of xscale).

// This file is included at the end of oracle_mesh_data.cpp, after the RendComp sources (it is
// not compiled on its own).

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace oracle;

namespace mesh_render
{
    std::vector<std::string> Words(const std::string& Text, const char* Separator)
    {
        std::vector<std::string> Out;
        size_t Start = 0;
        const size_t Width = strlen(Separator);
        for (;;)
        {
            const size_t End = Text.find(Separator, Start);
            if (End == std::string::npos)
            {
                Out.push_back(Text.substr(Start));
                return Out;
            }
            Out.push_back(Text.substr(Start, End - Start));
            Start = End + Width;
        }
    }

    unsigned long HexWord(const std::string& Text) { return strtoul(Text.c_str(), nullptr, 16); }
    long Dec(const std::string& Text) { return strtol(Text.c_str(), nullptr, 10); }
    float HexFloat(const std::string& Text)
    {
        const uint32_t Bits = (uint32_t)HexWord(Text);
        float Value;
        memcpy(&Value, &Bits, 4);
        return Value;
    }
    std::string F(float Value) { return Format("%08x", FloatBits(Value)); }
    std::string F3(const float* V) { return F(V[0]) + "," + F(V[1]) + "," + F(V[2]); }
    std::string W12(const float* M)
    {
        if (M == nullptr)
            return "-";
        std::string Out;
        for (int Index = 0; Index < 12; ++Index)
            Out += (Index ? "," : "") + F(M[Index]);
        return Out;
    }
    std::string HashOf(const void* Data, size_t Length) { return Fnv64(Data, Length); }

    // --- the recording state ------------------------------------------------------------------

    struct FMeshMat;

    struct FMeshState
    {
        Suite* Out = nullptr;
        int Case = 0;
        // pipeline
        bool bStateSet = true;
        int StateValue = 1;
        bool bLockFail = false;
        unsigned long NextBase = 0;
        // buffers
        unsigned long NextVb = 0x100;
        unsigned long NextIb = 0x200;
        std::vector<unsigned long> LiveVb;
        std::vector<unsigned long> LiveIb;
        std::map<unsigned long, std::vector<unsigned short>> IbData;
        unsigned long ScratchNext = 0;
        // engine
        std::map<unsigned long, std::vector<float>> Transforms;
        std::map<unsigned long, long> Archetypes;
        std::map<unsigned long, std::pair<float, std::vector<float>>> Extents;
        std::map<unsigned long, std::vector<unsigned long>> Kids;
        // camera
        float View[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        int Viewport[4] = {0, 0, 639, 479};
        double XScale = 1.0;
        float ZNear = 1.0f;
        float Position[3] = {0, 0, 0};
        int ClassifyResult = 4;
        // materials
        std::vector<FMeshMat*> Materials;

        void Event(const std::string& Body)
        {
            Out->Line(Format("{\"case\":\"%d\",", Case) + Body + "}");
        }
    };

    FMeshState GMesh;
    float GIdentity[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};

    bool Contains(const std::vector<unsigned long>& List, unsigned long Value)
    {
        for (unsigned long Item : List)
            if (Item == Value)
                return true;
        return false;
    }

    void Remove(std::vector<unsigned long>& List, unsigned long Value)
    {
        for (size_t Index = 0; Index < List.size(); ++Index)
            if (List[Index] == Value)
            {
                List.erase(List.begin() + Index);
                return;
            }
    }

    std::string StreamText(const FRpVertexStream& S, unsigned long SrcCount)
    {
        std::string Elements = "-";
        if (S.Base != nullptr && S.Count != 0)
        {
            std::vector<unsigned char> Bytes;
            for (unsigned int I = 0; I < S.Count; ++I)
                Bytes.insert(Bytes.end(), S.Base + I * S.Stride, S.Base + I * S.Stride + S.ByteSize);
            Elements = HashOf(Bytes.data(), Bytes.size());
        }
        std::string Index = "-";
        if (S.IndexArray != nullptr)
            Index = HashOf(S.IndexArray, (size_t)SrcCount * 4);
        return Format(";%u/%u/%u/", S.Count, S.Stride, S.ByteSize) + Elements + "/" + Index;
    }

    std::string SourceText(const FRpVertexSrcDesc* D)
    {
        if (D == nullptr)
            return "-";
        std::string Out = Format("%x,%u,%x", D->SrcFvf, D->SrcCount, D->Flags);
        Out += StreamText(D->Position, D->SrcCount);
        Out += StreamText(D->Normal, D->SrcCount);
        Out += StreamText(D->Diffuse, D->SrcCount);
        Out += StreamText(D->Tex[0], D->SrcCount);
        Out += StreamText(D->Tex[1], D->SrcCount);
        return Out;
    }

    // --- materials ----------------------------------------------------------------------------

    struct FMeshMat final : IMaterial
    {
        std::string Name;
        unsigned long Crc = 0;
        unsigned long Flags = 0;
        std::string Class;
        bool bParam = false;
        unsigned long Param = 0;
        unsigned long Refs = 1;

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override
        {
            ++Refs;
            GMesh.Event(Format("\"ev\":\"addref\",\"m\":\"%s\",\"refs\":\"%lu\"", Name.c_str(), Refs));
            return Refs;
        }
        unsigned long NEURONCOMPONENTS_API Release() override
        {
            if (Refs > 0)
                --Refs;
            GMesh.Event(Format("\"ev\":\"release\",\"m\":\"%s\",\"refs\":\"%lu\"", Name.c_str(), Refs));
            return Refs;
        }
        std::string Context(void* Arg)
        {
            const FMaterialSubmitContext* C = (const FMaterialSubmitContext*)Arg;
            std::string Vb = "-";
            if (C->VbListing != nullptr)
                Vb = Format("%lx,%lx,%lx", (unsigned long)(uintptr_t)C->VbListing->VertexBuffer, C->VbListing->Fvf,
                            C->VbListing->BaseVertex);
            std::string Ib = "-";
            if (C->IbScratch != nullptr)
                Ib = Format("%lx,%lx", (unsigned long)(uintptr_t)C->IbScratch->IndexBuffer, C->IbScratch->Bias);
            std::string Rep = "-";
            if (C->ReplacedMaterial != nullptr)
                Rep = ((FMeshMat*)C->ReplacedMaterial)->Name;
            std::string F20 = C->Field20 == nullptr ? "0" : (C->Field20 == (void*)G_Engine ? "engine" : "?");
            return Format("\"inst\":\"%lu\",\"world\":\"", (unsigned long)(uintptr_t)C->Inst) + W12((const float*)C->World) +
                   "\",\"view\":\"" + W12((const float*)C->View) + "\",\"vb\":\"" + Vb + "\",\"ib\":\"" + Ib +
                   "\",\"rep\":\"" + Rep + "\",\"f20\":\"" + F20 + "\"";
        }
        GENRESULT NEURONCOMPONENTS_API Bind(void* Arg) override
        {
            GMesh.Event(Format("\"ev\":\"bind\",\"m\":\"%s\",", Name.c_str()) + Context(Arg));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Teardown(void* Arg) override
        {
            GMesh.Event(Format("\"ev\":\"teardown\",\"m\":\"%s\",", Name.c_str()) + Context(Arg));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Submit(void* Arg, unsigned long Prim, unsigned long Start, unsigned long Count,
                                              unsigned long IbStart, unsigned long Span, unsigned long Flags) override
        {
            const FMaterialSubmitContext* C = (const FMaterialSubmitContext*)Arg;
            std::string Idx = "-";
            if (C->Indices != nullptr)
                Idx = HashOf(C->Indices + IbStart, (size_t)Span * 2);
            GMesh.Event(Format("\"ev\":\"submit\",\"m\":\"%s\",", Name.c_str()) + Context(Arg) +
                        Format(",\"prim\":\"%lu\",\"start\":\"%lu\",\"count\":\"%lu\",\"ibs\":\"%lu\",\"span\":\"%lu\","
                               "\"flags\":\"%lx\",\"src\":\"",
                               Prim, Start, Count, IbStart, Span, Flags) +
                        SourceText(C->SourceVertices) + "\",\"idx\":\"" + Idx + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API CopyClassName(char* Out, unsigned long Size) override
        {
            GMesh.Event(Format("\"ev\":\"class\",\"m\":\"%s\",\"v\":\"%s\"", Name.c_str(), Class.c_str()));
            strncpy(Out, Class.c_str(), Size);
            Out[Size - 1] = 0;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetRenderFlags(unsigned long* Out) override
        {
            GMesh.Event(Format("\"ev\":\"flags\",\"m\":\"%s\",\"v\":\"%lx\"", Name.c_str(), Flags));
            *Out = Flags;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetParameterCount(unsigned long* Out) override
        {
            const unsigned long Count = bParam ? 9 : 0;
            GMesh.Event(Format("\"ev\":\"pcount\",\"m\":\"%s\",\"v\":\"%lu\"", Name.c_str(), Count));
            *Out = Count;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetParameterValue(unsigned long Index, void* InOut) override
        {
            FShadingPropertyValue* Value = (FShadingPropertyValue*)InOut;
            Value->Type = 1;
            Value->Payload0 = Param;
            GMesh.Event(Format("\"ev\":\"param\",\"m\":\"%s\",\"i\":\"%lu\",\"t\":\"1\",\"v\":\"%08lx\"", Name.c_str(),
                               Index, Param));
            return GR_OK;
        }
        // The other slots are never called.
        GENRESULT NEURONCOMPONENTS_API Initialize(INeuronComponent* Outer) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LoadFromFileSystem(void* FileSystem) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SaveProperties(void* FileSystem) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ApplyVerify(void* Context, void* Extra) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ResolveTextures(float Delta) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetName(const char* Name) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetName(char* Out, unsigned long Size) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetPassCount(int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetParameterDescriptor(unsigned long Index, void* OutDesc) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetParameterValue(unsigned long Index, void* Value) override { return GR_GENERIC; }
        const char* __thiscall ClassName() override { return Class.c_str(); }
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

    class FMeshLibrary final : public IMaterialLibrary
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API FindMaterialByCrc(void* Query, IMaterial** Out) override
        {
            const unsigned long Crc = ((const unsigned long*)Query)[1];
            for (FMeshMat* Mat : GMesh.Materials)
            {
                if (Mat->Crc != Crc)
                    continue;
                ++Mat->Refs;
                *Out = Mat;
                GMesh.Event(Format("\"ev\":\"findmat\",\"crc\":\"%08lx\",\"r\":\"0\",\"m\":\"%s\"", Crc, Mat->Name.c_str()));
                return GR_OK;
            }
            GMesh.Event(Format("\"ev\":\"findmat\",\"crc\":\"%08lx\",\"r\":\"-1\",\"m\":\"-\"", Crc));
            return GR_GENERIC;
        }
        // The slots RendComp does not call.
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
        GENRESULT NEURONCOMPONENTS_API FindOrCreate(const char* Name, const char* Type, INeuronComponent* AggregateOuter, IMaterial** Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API AddMaterial(int GroupId, void* Record, IMaterial* Material) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API RemoveMaterial(unsigned long GroupId, void* Record) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetGroupCount(unsigned long* OutCount) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API EnumGroup(unsigned long Index, unsigned long* OutId) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetGroupMaterialCount(unsigned long GroupId, int* OutCount) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API EnumGroupMaterial(unsigned long GroupId, unsigned long Index, IMaterial** Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetGlobalRecord(unsigned long Index, const unsigned long* Value) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetGlobalRecordValue(unsigned long Index, const unsigned long* Value) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetGlobalRecordValue(unsigned long Index, unsigned long* OutValue) override { return GR_GENERIC; }
        FMaterialLibraryRecord* NEURONCOMPONENTS_API CreateAfterMap(const char* Name, const char* Type, unsigned char bRemap, INeuronComponent* AggregateOuter) override { return nullptr; }
    };

    // --- the renderer -------------------------------------------------------------------------

    class FMeshPipeline final : public IRenderPipeline
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API GetPipelineState(int Id, int* Out) override
        {
            if (!GMesh.bStateSet)
            {
                GMesh.Event(Format("\"ev\":\"pstate\",\"id\":\"%d\",\"r\":\"-\"", Id));
                return GR_GENERIC;
            }
            GMesh.Event(Format("\"ev\":\"pstate\",\"id\":\"%d\",\"r\":\"%d\"", Id, GMesh.StateValue));
            *Out = GMesh.StateValue;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetModelview(const float* M) override
        {
            GMesh.Event("\"ev\":\"modelview\",\"m\":\"" + W12(M) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetWorld(const float* M) override
        {
            GMesh.Event("\"ev\":\"world\",\"m\":\"" + W12(M) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetView(const float* M) override
        {
            GMesh.Event("\"ev\":\"view\",\"m\":\"" + W12(M) + "\"");
            return GR_OK;
        }
        // The slots RendComp does not call.
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
        GENRESULT NEURONCOMPONENTS_API SetRenderState(D3DRENDERSTATETYPE State, int Value) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetRenderState(D3DRENDERSTATETYPE State, int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageState(int Stage, int Type, int Value) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageState(int Stage, int Type, int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageTransform(int Stage, const float* Matrix16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageTransform(int Stage, float* OutMatrix16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageTexture(int Stage, void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageTexture(int Stage, void** OutHandle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API VerifyState() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DrawPrimitive(unsigned long Prim, unsigned int Fvf, const void* Verts, unsigned int Count, void* Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitiveUp(unsigned long Prim, unsigned int Fvf, const void* Verts, unsigned int VertCount, const void* Indices, unsigned int IndexCount, unsigned long Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DrawPrimitiveVb(unsigned long Prim, void* Vb, unsigned int StartVertex, unsigned int Count, void* Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitiveVb(unsigned long Prim, void* Vb, unsigned int BaseVertex, unsigned int NumVerts, const void* Indices, unsigned int IndexCount, unsigned long Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API AddLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API RemoveLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UpdateLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetWorldN(int WorldIndex, const float* M12) override { return GR_GENERIC; }
    };

    class FMeshVbMgr final : public IVertexBufferManager
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
            if (GMesh.bLockFail)
            {
                GMesh.bLockFail = false;
                GMesh.Event(Format("\"ev\":\"lockvm\",\"fvf\":\"%x\",\"n\":\"%u\",\"r\":\"-1\"", Fvf, Count));
                return GR_GENERIC;
            }
            GMesh.Event(Format("\"ev\":\"lockvm\",\"fvf\":\"%x\",\"n\":\"%u\",\"r\":\"0\"", Fvf, Count));
            Memory.assign((size_t)Count * 64 + 64, 0);
            Out->Listing.VertexBuffer = (void*)(uintptr_t)0x900;
            Out->Listing.Fvf = Fvf;
            Out->Listing.BaseVertex = GMesh.NextBase;
            GMesh.NextBase += Count;
            Out->Vertices = Memory.data();
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API UnlockVertexMemory(FVertexMemoryLock* Out) override
        {
            GMesh.Event(Format("\"ev\":\"unlockvm\",\"vb\":\"%lx\",\"base\":\"%lx\"",
                               (unsigned long)(uintptr_t)Out->Listing.VertexBuffer, Out->Listing.BaseVertex));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API CopyVertexData(void* Dest, unsigned int Fvf, const FRpVertexSrcDesc* Source,
                                                      unsigned int Start, unsigned int Count) override
        {
            GMesh.Event(Format("\"ev\":\"copyvd\",\"fvf\":\"%x\",\"start\":\"%u\",\"n\":\"%u\",\"src\":\"", Fvf, Start, Count) +
                        SourceText(Source) + "\"");
            return GR_OK;
        }
        // The slots RendComp does not call.
        GENRESULT NEURONCOMPONENTS_API VbMgrInit(void* A, void* B, void* C, void* D) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API VbMgrReleaseAll() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UnsupportedManagerOperation() override { return GR_GENERIC; }
    };

    class FMeshDraw final : public IRPDraw
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitive(unsigned long Prim, unsigned int Min, unsigned int NumVertices,
                                                            unsigned int Start, unsigned int Count) override
        {
            GMesh.Event(Format("\"ev\":\"dip\",\"prim\":\"%lu\",\"min\":\"%u\",\"nv\":\"%u\",\"start\":\"%u\",\"n\":\"%u\"",
                               Prim, Min, NumVertices, Start, Count));
            return GR_OK;
        }
        // The slots RendComp does not call.

    };

    class FMeshIb final : public IRPIndexBuffer
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API CreateIb(unsigned int Count, unsigned int** Out, unsigned int) override
        {
            const unsigned long Handle = GMesh.NextIb++;
            GMesh.LiveIb.push_back(Handle);
            GMesh.IbData[Handle].assign(Count, 0);
            GMesh.Event(Format("\"ev\":\"createib\",\"n\":\"%u\",\"r\":\"0\",\"ib\":\"%lx\"", Count, Handle));
            *Out = (unsigned int*)(uintptr_t)Handle;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API DestroyIb(void** Handle) override
        {
            const unsigned long H = (unsigned long)(uintptr_t)*Handle;
            GMesh.Event(Format("\"ev\":\"destroyib\",\"ib\":\"%lx\"", H));
            Remove(GMesh.LiveIb, H);
            *Handle = nullptr;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API CopyIndices(void*, unsigned int* Start, const unsigned short* Src, unsigned int Count) override
        {
            GMesh.Event(Format("\"ev\":\"copyidx\",\"n\":\"%u\",\"h\":\"", Count) + HashOf(Src, (size_t)Count * 2) +
                        Format("\",\"start\":\"%lu\"", GMesh.ScratchNext));
            *Start = (unsigned int)GMesh.ScratchNext;
            GMesh.ScratchNext += Count;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API LockIb(void* Handle, unsigned int* Offset, void** Out, unsigned int Count) override
        {
            const unsigned long H = (unsigned long)(uintptr_t)Handle;
            GMesh.Event(Format("\"ev\":\"lockib\",\"ib\":\"%lx\",\"n\":\"%u\",\"r\":\"0\"", H, Count));
            *Offset = 0;
            *Out = GMesh.IbData[H].data();
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API UnlockIb(void* Handle) override
        {
            const unsigned long H = (unsigned long)(uintptr_t)Handle;
            const std::vector<unsigned short>& Data = GMesh.IbData[H];
            GMesh.Event(Format("\"ev\":\"unlockib\",\"ib\":\"%lx\",\"h\":\"", H) + HashOf(Data.data(), Data.size() * 2) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SelectIb(void* Handle, unsigned int Base, unsigned int, unsigned int) override
        {
            const unsigned long H = (unsigned long)(uintptr_t)Handle;
            const int R = (H == 0xFFFFFFFFul || Contains(GMesh.LiveIb, H)) ? 0 : -1;
            GMesh.Event(Format("\"ev\":\"selib\",\"ib\":\"%lx\",\"base\":\"%u\",\"r\":\"%d\"", H, Base, R));
            return R;
        }
        // The slots RendComp does not call.
        GENRESULT NEURONCOMPONENTS_API ResizeIb(void* Handle, unsigned int Count) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetIbCapacity(void* Handle, unsigned int* OutCapacity) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API IsIbValid(void* Handle) override { return GR_GENERIC; }
    };

    class FMeshVb final : public IRPVertexBuffer
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API CreateVb(unsigned int Fvf, unsigned int Count, unsigned int** Out, unsigned int) override
        {
            const unsigned long Handle = GMesh.NextVb++;
            GMesh.LiveVb.push_back(Handle);
            GMesh.Event(Format("\"ev\":\"createvb\",\"fvf\":\"%x\",\"n\":\"%u\",\"r\":\"0\",\"vb\":\"%lx\"", Fvf, Count, Handle));
            *Out = (unsigned int*)(uintptr_t)Handle;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API DestroyVb(void** Handle) override
        {
            const unsigned long H = (unsigned long)(uintptr_t)*Handle;
            GMesh.Event(Format("\"ev\":\"destroyvb\",\"vb\":\"%lx\"", H));
            Remove(GMesh.LiveVb, H);
            *Handle = nullptr;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API CopyVertices(void* Handle, unsigned int* Base, const FRpVertexSrcDesc* Src,
                                                    unsigned int Start, unsigned int Count) override
        {
            GMesh.Event(Format("\"ev\":\"copyv\",\"vb\":\"%lx\",\"base\":\"%u\",\"start\":\"%u\",\"n\":\"%u\",\"src\":\"",
                               (unsigned long)(uintptr_t)Handle, *Base, Start, Count) +
                        SourceText(Src) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API LockVb(void* Handle, unsigned int* Offset, void** Out, unsigned int Count) override
        {
            static unsigned char Scratch[16];
            GMesh.Event(Format("\"ev\":\"lockvb\",\"vb\":\"%lx\",\"n\":\"%u\",\"r\":\"0\"", (unsigned long)(uintptr_t)Handle, Count));
            *Offset = 0;
            *Out = Scratch;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API UnlockVb(void* Handle) override
        {
            GMesh.Event(Format("\"ev\":\"unlockvb\",\"vb\":\"%lx\"", (unsigned long)(uintptr_t)Handle));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SelectVb(void* Handle) override
        {
            const unsigned long H = (unsigned long)(uintptr_t)Handle;
            const int R = Contains(GMesh.LiveVb, H) ? 0 : -1;
            GMesh.Event(Format("\"ev\":\"selvb\",\"vb\":\"%lx\",\"r\":\"%d\"", H, R));
            return R;
        }
        // The slots RendComp does not call.
        GENRESULT NEURONCOMPONENTS_API ResizeVb(void* Handle, unsigned int Fvf, unsigned int Count) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UnsupportedVertexBufferOperation(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetVbInfo(void* Handle, unsigned int* OutFvf, unsigned int* OutCount) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API IsVbValid(void* Handle) override { return GR_GENERIC; }
    };

    // --- the engine and the camera ------------------------------------------------------------

    class FMeshEngine final : public IEngine
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        float* NEURONCOMPONENTS_API GetInstanceTransform(void* Instance) override
        {
            const unsigned long I = (unsigned long)(uintptr_t)Instance;
            GMesh.Event(Format("\"ev\":\"xform\",\"inst\":\"%lu\"", I));
            std::map<unsigned long, std::vector<float>>::iterator It = GMesh.Transforms.find(I);
            return It == GMesh.Transforms.end() ? GIdentity : It->second.data();
        }
        void* NEURONCOMPONENTS_API GetInstanceArchetype(void* Instance) override
        {
            const unsigned long I = (unsigned long)(uintptr_t)Instance;
            long R = -1;
            if (GMesh.Archetypes.count(I) != 0)
                R = GMesh.Archetypes[I];
            GMesh.Event(Format("\"ev\":\"arch\",\"inst\":\"%lu\",\"r\":\"%ld\"", I, R));
            return (void*)(intptr_t)R;
        }
        unsigned long NEURONCOMPONENTS_API ReleaseHandle(void* Handle) override
        {
            GMesh.Event(Format("\"ev\":\"relh\",\"arch\":\"%ld\"", (long)(intptr_t)Handle));
            return 0;
        }
        GENRESULT NEURONCOMPONENTS_API GetInstanceExtent(void* Instance, unsigned long, float* OutR, float* OutC) override
        {
            const unsigned long I = (unsigned long)(uintptr_t)Instance;
            if (GMesh.Extents.count(I) == 0)
            {
                GMesh.Event(Format("\"ev\":\"getext\",\"inst\":\"%lu\",\"r\":\"-\",\"c\":\"-\"", I));
                return GR_OK;
            }
            const std::pair<float, std::vector<float>>& E = GMesh.Extents[I];
            *OutR = E.first;
            memcpy(OutC, E.second.data(), 12);
            GMesh.Event(Format("\"ev\":\"getext\",\"inst\":\"%lu\",\"r\":\"", I) + F(E.first) + "\",\"c\":\"" +
                        F3(E.second.data()) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetInstanceExtent(void* Instance, unsigned long, float R, const float* C) override
        {
            const unsigned long I = (unsigned long)(uintptr_t)Instance;
            GMesh.Event(Format("\"ev\":\"setext\",\"inst\":\"%lu\",\"r\":\"", I) + F(R) + "\",\"c\":\"" + F3(C) + "\"");
            GMesh.Extents[I] = std::make_pair(R, std::vector<float>(C, C + 3));
            return GR_OK;
        }
        void* NEURONCOMPONENTS_API GetRelatedInstance(void* From, unsigned long, void* Cursor) override
        {
            const unsigned long I = (unsigned long)(uintptr_t)From;
            const long Prev = (long)(intptr_t)Cursor;
            long R = -1;
            const std::vector<unsigned long>& Kids = GMesh.Kids[I];
            if (Prev == -1)
                R = Kids.empty() ? -1 : (long)Kids[0];
            else
                for (size_t K = 0; K + 1 < Kids.size(); ++K)
                    if ((long)Kids[K] == Prev)
                        R = (long)Kids[K + 1];
            GMesh.Event(Format("\"ev\":\"child\",\"inst\":\"%lu\",\"prev\":\"%ld\",\"r\":\"%ld\"", I, Prev, R));
            return (void*)(intptr_t)R;
        }
        void* NEURONCOMPONENTS_API GetInstanceRoot(void* Instance) override
        {
            GMesh.Event(Format("\"ev\":\"root\",\"inst\":\"%lu\"", (unsigned long)(uintptr_t)Instance));
            return Instance;
        }
        // The slots RendComp does not call.
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

    class FMeshCamera final : public ICamera
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
            GMesh.Event("\"ev\":\"campos\"");
            return GMesh.Position;
        }
        int const* NEURONCOMPONENTS_API GetViewport() override
        {
            GMesh.Event("\"ev\":\"viewport\"");
            return GMesh.Viewport;
        }
        float NEURONCOMPONENTS_API GetHalfFovXDeg() override
        {
            GMesh.Event("\"ev\":\"fov\"");
            return 45.0f;
        }
        float NEURONCOMPONENTS_API GetZNear() override
        {
            GMesh.Event("\"ev\":\"znear\"");
            return GMesh.ZNear;
        }
        double NEURONCOMPONENTS_API GetXScaleOverNear() override
        {
            GMesh.Event("\"ev\":\"xscale\"");
            return GMesh.XScale;
        }
        int NEURONCOMPONENTS_API ClassifySphere(float const* C, float R) override
        {
            GMesh.Event("\"ev\":\"classify\",\"c\":\"" + F3(C) + "\",\"r\":\"" + F(R) +
                        Format("\",\"res\":\"%d\"", GMesh.ClassifyResult));
            return GMesh.ClassifyResult;
        }
        float const* NEURONCOMPONENTS_API GetViewTransform() override
        {
            GMesh.Event("\"ev\":\"camview\"");
            return GMesh.View;
        }
        // The slots RendComp does not call.
        float const* NEURONCOMPONENTS_API GetOrientation() override { return nullptr; }
        float NEURONCOMPONENTS_API GetFovYDeg() override { return 0; }
        float NEURONCOMPONENTS_API GetZFar() override { return 0; }
        float NEURONCOMPONENTS_API GetAspect() override { return 0; }
        double NEURONCOMPONENTS_API GetYScaleOverNear() override { return 0; }
        int NEURONCOMPONENTS_API Project(float* OutX, float* OutY, float* OutZ, float const* World) override { return GR_GENERIC; }
        void NEURONCOMPONENTS_API Unproject(float* OutWorld3, float ScreenX, float ScreenY) override { }
        int NEURONCOMPONENTS_API ProjectVec(float* Out3, float const* World) override { return GR_GENERIC; }
        void GetVelocity(float* Out3) override { }
        int SetProperty(char const* Key, char const* Value) override { return GR_GENERIC; }
    };

    FMeshLibrary GMeshLibrary;
    FMeshPipeline GMeshPipeline;
    FMeshVbMgr GMeshVbMgr;
    FMeshDraw GMeshDraw;
    FMeshIb GMeshIb;
    FMeshVb GMeshVb;
    FMeshEngine GMeshEngine;
    FMeshCamera GMeshCamera;

    // --- FDUMP ---------------------------------------------------------------------------------

    void __cdecl MeshDump(int Flags, const char* Pattern, ...)
    {
        va_list Args;
        va_start(Args, Pattern);
        const char* File = va_arg(Args, const char*);
        const int Line = va_arg(Args, int);
        const char* Message = va_arg(Args, const char*);
        va_end(Args);
        GMesh.Event(Format("\"ev\":\"dump\",\"flags\":\"%x\",\"file\":\"%s\",\"line\":\"%x\",\"msg\":\"", Flags, File, Line) +
                    Hex(Message, strlen(Message)) + "\"");
    }

    // --- the archive ---------------------------------------------------------------------------

    struct FTreeNode
    {
        std::string Name;
        bool bDir = false;
        const unsigned char* Data = nullptr;
        unsigned long Size = 0;
        FTreeNode* Parent = nullptr;
        std::vector<FTreeNode*> Children;
    };

    bool SameName(const std::string& A, const char* B)
    {
        return _stricmp(A.c_str(), B) == 0;
    }

    FTreeNode* FindChild(FTreeNode* Node, const char* Name, bool bDir)
    {
        for (FTreeNode* Child : Node->Children)
            if (Child->bDir == bDir && SameName(Child->Name, Name))
                return Child;
        return nullptr;
    }

    // UTF: header +8 tree offset, +0x18 names, +0x24 data; 0x2C-byte entries {next, name, flags,
    // ..., +0x10 child or data offset, +0x1C size}.
    FTreeNode* ParseUtf(const Bytes& File)
    {
        const unsigned char* B = File.data();
        auto U32 = [&](size_t At) {
            unsigned long V;
            memcpy(&V, B + At, 4);
            return V;
        };
        const unsigned long Tree = U32(8), Names = U32(0x18), Data = U32(0x24);
        struct FWalk
        {
            static void Children(FTreeNode* Parent, unsigned long First, const unsigned char* B, unsigned long Tree,
                                 unsigned long Names, unsigned long Data)
            {
                unsigned long At = First;
                while (true)
                {
                    unsigned long Next, Name, Flags, Off, Size;
                    memcpy(&Next, B + Tree + At, 4);
                    memcpy(&Name, B + Tree + At + 4, 4);
                    memcpy(&Flags, B + Tree + At + 8, 4);
                    memcpy(&Off, B + Tree + At + 0x10, 4);
                    memcpy(&Size, B + Tree + At + 0x1C, 4);
                    FTreeNode* Node = new FTreeNode();
                    Node->Name = (const char*)(B + Names + Name);
                    Node->Parent = Parent;
                    Node->bDir = (Flags & 0x10) != 0;
                    if (Node->bDir)
                    {
                        if (Off != 0)
                            Children(Node, Off, B, Tree, Names, Data);
                    }
                    else
                    {
                        Node->Data = B + Data + Off;
                        Node->Size = Size;
                    }
                    Parent->Children.push_back(Node);
                    if (Next == 0)
                        return;
                    At = Next;
                }
            }
        };
        FTreeNode* Root = new FTreeNode();
        Root->bDir = true;
        unsigned long First;
        memcpy(&First, B + Tree + 0x10, 4);
        if (First != 0)
            FWalk::Children(Root, First, B, Tree, Names, Data);
        return Root;
    }

    class FTreeFs final : public IFileSystem
    {
    public:
        FTreeNode* Current;
        unsigned long Refs = 1;
        struct FOpen
        {
            FTreeNode* Node;
            unsigned long Position;
        };
        std::vector<FOpen> Open;
        std::vector<FTreeNode*> Found;
        size_t FoundAt = 0;

        explicit FTreeFs(FTreeNode* Node) : Current(Node) {}

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return ++Refs; }
        unsigned long NEURONCOMPONENTS_API Release() override
        {
            if (--Refs == 0)
            {
                delete this;
                return 0;
            }
            return Refs;
        }
        GENRESULT NEURONCOMPONENTS_API CreateInstance(AGGDESC* Arg, void** Out) override
        {
            FFileSystemDesc* Desc = (FFileSystemDesc*)Arg;
            FTreeNode* Child = FindChild(Current, Desc->Path, true);
            if (Child == nullptr)
            {
                *Out = nullptr;
                return GR_GENERIC;
            }
            *Out = (IFileSystem*)new FTreeFs(Child);
            return GR_OK;
        }
        int NEURONCOMPONENTS_API OpenNode(const char* Name) override
        {
            if (strcmp(Name, "..") == 0)
            {
                if (Current->Parent == nullptr)
                    return 0;
                Current = Current->Parent;
                return 1;
            }
            FTreeNode* Child = FindChild(Current, Name, true);
            if (Child == nullptr)
                return 0;
            Current = Child;
            return 1;
        }
        int NEURONCOMPONENTS_API OpenByDesc(FFileSystemDesc* Desc) override
        {
            FTreeNode* Leaf = FindChild(Current, Desc->Path, false);
            if (Leaf == nullptr)
                return -1;
            Open.push_back({Leaf, 0});
            return (int)Open.size();
        }
        int NEURONCOMPONENTS_API CloseFile(unsigned long) override { return 1; }
        unsigned long NEURONCOMPONENTS_API GetSize(unsigned long Handle, unsigned long*) override
        {
            return Open[Handle - 1].Node->Size;
        }
        int NEURONCOMPONENTS_API Read(unsigned long Handle, void* Buffer, unsigned long Size, unsigned long* Got,
                                      unsigned long) override
        {
            FOpen& F = Open[Handle - 1];
            const unsigned long Left = F.Node->Size - F.Position;
            const unsigned long N = Size < Left ? Size : Left;
            memcpy(Buffer, F.Node->Data + F.Position, N);
            F.Position += N;
            *Got = N;
            return N != 0 ? 1 : 0;
        }
        int Fill(WIN32_FIND_DATAA* Data)
        {
            memset(Data, 0, sizeof(*Data));
            FTreeNode* Node = Found[FoundAt];
            Data->dwFileAttributes = Node->bDir ? 0x10 : 0x80;
            strncpy(Data->cFileName, Node->Name.c_str(), sizeof(Data->cFileName) - 1);
            return 1;
        }
        int NEURONCOMPONENTS_API FindFirst(const char*, WIN32_FIND_DATAA* Data) override
        {
            Found = Current->Children;
            FoundAt = 0;
            if (Found.empty())
                return -1;
            Fill(Data);
            return 0;
        }
        int NEURONCOMPONENTS_API FindNext(unsigned long, WIN32_FIND_DATAA* Data) override
        {
            if (++FoundAt >= Found.size())
                return 0;
            return Fill(Data);
        }
        int NEURONCOMPONENTS_API FindClose(unsigned long) override { return 1; }
        int NEURONCOMPONENTS_API GetCurrentPath(unsigned long, char* Out) override
        {
            Out[0] = 0;
            return 1;
        }
        int NEURONCOMPONENTS_API ResolvePath(char* Dest, const char*, unsigned long) override
        {
            Dest[0] = 0;
            return 1;
        }
        // The slots RendComp does not call.
        int NEURONCOMPONENTS_API Write(unsigned long Handle, const void* Buffer, unsigned long Size, unsigned long* BytesWritten, const void* PositionOverride) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation1C(unsigned long A1, unsigned long A2, unsigned long A3, unsigned long A4) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API SeekFile(unsigned long Handle, unsigned long OffLow, unsigned long* OffHighOut, unsigned long Origin) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API SetEndOfFile(unsigned long A1) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API ReturnOne2C(unsigned long A1, unsigned long A2, unsigned long A3, unsigned long A4, unsigned long A5) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API ReturnOne30(unsigned long A1, unsigned long A2, unsigned long A3, unsigned long A4, unsigned long A5) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetFileTimes(unsigned long Handle, FILETIME* CreationOut, FILETIME* AccessOut, FILETIME* WriteOut) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation38(unsigned long A1, unsigned long A2, unsigned long A3, unsigned long A4) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API CreateMapping(unsigned long A1, unsigned long A2, unsigned long A3, unsigned long A4, unsigned long A5, unsigned long A6) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API MapFileView(unsigned long A1, unsigned long A2, unsigned long A3, unsigned long A4, unsigned long A5) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API UnmapFileView(unsigned long A1) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation54(const char* Name, unsigned long Extra) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation58(unsigned long A1) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation64(unsigned long A1) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation68(unsigned long A1, unsigned long A2, unsigned long A3) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation6C(unsigned long A1, unsigned long A2) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetPathAttributes(const char* Path) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation74(unsigned long A1, unsigned long A2) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetFileSystemError() override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetFilePosition(unsigned long Handle, unsigned long* HighOut) override { return GR_GENERIC; }
        void NEURONCOMPONENTS_API GetFileSysPath(char* Out, unsigned long MaxChars) override { }
        int NEURONCOMPONENTS_API GetAccessFlags() override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetParentFileSystem(IFileSystem** Out) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API FailOperation90(unsigned long A1, unsigned long A2) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API FailOperation94(unsigned long A1, unsigned long A2) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API FailOperation98(unsigned long A1, unsigned long A2, unsigned long A3, unsigned long A4, unsigned long A5) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API FailOperation9C(unsigned long A1, unsigned long A2, unsigned long A3, unsigned long A4, unsigned long A5) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API InvokeLockedCallback(unsigned long A1, unsigned long A2, unsigned long A3) override { return GR_GENERIC; }
    };

    // --- the cases ----------------------------------------------------------------------------

    FTreeNode* GTree = nullptr;
    Bytes GArchive;

    IFileSystem* NodeFs(const std::string& Path)
    {
        if (Path == ".")
            return new FTreeFs(GTree);
        FTreeNode* Child = FindChild(GTree, Path.c_str(), true);
        if (Child == nullptr)
            return nullptr;
        return new FTreeFs(Child);
    }

    FMeshMat* MaterialNamed(const std::string& Name)
    {
        for (FMeshMat* Mat : GMesh.Materials)
            if (Mat->Name == Name)
                return Mat;
        return nullptr;
    }

    std::vector<float> HexFloats(const std::string& Text)
    {
        std::vector<float> Out;
        for (const std::string& Part : Words(Text, ","))
            Out.push_back(HexFloat(Part));
        return Out;
    }

    const char* IidOf(const std::string& Name)
    {
        if (Name == "mesh")
            return Iids::Mesh;
        if (Name == "hook")
            return Iids::MatHookUp;
        if (Name == "renderable")
            return Iids::RenderableMesh;
        if (Name == "component")
            return Iids::NeuronComponent;
        if (Name == "trianim")
            return Iids::TriMeshAnim;
        if (Name == "sphere")
            return Iids::SphereMesh;
        if (Name == "vmesh")
            return Iids::VMeshLibrary;
        if (Name == "wire")
            return Iids::VMeshWire;
        return "1.11_INothing";
    }

    struct FMeshCase
    {
        FRenderManager* Manager = nullptr;
        FSphereMeshPlugin* Sphere = nullptr;
        std::map<std::string, void*> Libraries;
        FVMeshLibrary* Library = nullptr;
        FVMeshWire* Wire = nullptr;

        void Op(const std::string& Body)
        {
            GMesh.Event(Body);
        }

        void AddPlugin(IRenderComponent* Plugin)
        {
            FPluginNode* Node = new FPluginNode();
            Node->Plugin = Plugin;
            Node->Next = &Manager->PluginSentinel;
            Node->Prev = Manager->PluginSentinel.Prev;
            Manager->PluginSentinel.Prev->Next = Node;
            Manager->PluginSentinel.Prev = Node;
            Manager->PluginCount += 1;
        }

        void Start()
        {
            G_VmeshDataMap.clear();
            G_Pipeline = &GMeshPipeline;
            G_Draw = &GMeshDraw;
            G_IndexBuffer = &GMeshIb;
            G_VertexBuffer = &GMeshVb;
            G_VbManager = &GMeshVbMgr;
            G_MaterialLibrary = &GMeshLibrary;
            Manager = new FRenderManager();
            G_Engine = &GMeshEngine;
            FRenderPluginDesc Desc;
            Desc.Size = 0x10;
            Desc.Parent = (INeuronComponent*)&GMeshEngine;
            Desc.Engine = &GMeshEngine;
            void* Memory = calloc(1, sizeof(FVMeshPlugin));
            FVMeshPlugin* VMesh = new (Memory) FVMeshPlugin();
            VMesh->Bound = nullptr;
            VMesh->RefCount = 1;
            AddPlugin(VMesh);
            void* Out = nullptr;
            Desc.Name = G_NameTriMesh;
            CreateTriMeshPlugin(&Desc, &Out);
            ((FTriMeshPlugin*)(IRenderComponent*)Out)->Tristrips = 0;
            AddPlugin((IRenderComponent*)Out);
            Desc.Name = G_NameSphereMesh;
            CreateSphereMeshPlugin(&Desc, &Out);
            Sphere = (FSphereMeshPlugin*)(IRenderComponent*)Out;
            AddPlugin((IRenderComponent*)Out);
            Library = new FVMeshLibrary();
            Library->Inner.RefCount = 1;
            Library->Inner.Owner = Library;
            Library->Outer = &Library->Inner;
            Wire = new FVMeshWire();
            Wire->Inner.RefCount = 1;
            Wire->Inner.Owner = Wire;
            Wire->Outer = &Wire->Inner;
        }

        std::string LodText(int Id)
        {
            FLodSet* Set = Manager->FindSet(Id);
            if (Set == nullptr)
                return "\"lods\":\"-\"";
            std::string Switch;
            for (size_t I = 0; I < Set->Switch.size(); ++I)
                Switch += (I ? "," : "") + F(Set->Switch[I]);
            std::string Plugins;
            for (int I = 0; I < Set->LodCount; ++I)
            {
                int Number = -1, N = 0;
                for (FPluginNode* W = Manager->PluginSentinel.Next; W != &Manager->PluginSentinel; W = W->Next, ++N)
                    if (W->Plugin == Set->Entries[I].Plugin)
                        Number = N;
                Plugins += Format(I ? ",%d" : "%d", Number);
            }
            return Format("\"lods\":\"%d\",\"cur\":\"%d\",\"switch\":\"", Set->LodCount, Set->CurrentLod) + Switch +
                   "\",\"plugins\":\"" + Plugins + "\"";
        }

        FSphereAnim* SphereOf(unsigned long Inst)
        {
            return Sphere->FindSphere((void*)(uintptr_t)Inst);
        }

        void Run(const std::vector<std::string>& T)
        {
            const std::string& K = T[0];
            if (K == "fs")
            {
                if (!ReadWholeFile(T[1], GArchive))
                {
                    fprintf(stderr, "mesh_render: cannot read %s\n", T[1].c_str());
                    GFailed = true;
                    return;
                }
                GTree = ParseUtf(GArchive);
                Op("\"op\":\"fs\"");
            }
            else if (K == "mat")
            {
                FMeshMat* Mat = new FMeshMat();
                Mat->Name = T[1];
                Mat->Crc = T[2][0] == '#' ? HexWord(T[2].substr(1)) : NEURONCOMPONENTS_CRC::GetCRC32(T[2].c_str());
                Mat->Flags = HexWord(T[3]);
                Mat->Class = T[4];
                if (T.size() > 5)
                {
                    Mat->bParam = true;
                    Mat->Param = HexWord(T[5]);
                }
                GMesh.Materials.push_back(Mat);
                Op(Format("\"op\":\"mat\",\"m\":\"%s\",\"crc\":\"%08lx\"", Mat->Name.c_str(), Mat->Crc));
            }
            else if (K == "xform")
                GMesh.Transforms[HexWord(T[1])] = HexFloats(T[2]);
            else if (K == "ext")
                GMesh.Extents[HexWord(T[1])] = std::make_pair(HexFloat(T[2]), HexFloats(T[3]));
            else if (K == "instarch")
                GMesh.Archetypes[HexWord(T[1])] = Dec(T[2]);
            else if (K == "kids")
            {
                std::vector<unsigned long> List;
                for (const std::string& Part : Words(T[2], ","))
                    List.push_back(HexWord(Part));
                GMesh.Kids[HexWord(T[1])] = List;
            }
            else if (K == "cam")
            {
                if (T[1] == "view")
                    memcpy(GMesh.View, HexFloats(T[2]).data(), 48);
                else if (T[1] == "vp")
                    for (int I = 0; I < 4; ++I)
                        GMesh.Viewport[I] = (int)Dec(T[2 + I]);
                else if (T[1] == "xs")
                {
                    const unsigned long long Bits = strtoull(T[2].c_str(), nullptr, 16);
                    memcpy(&GMesh.XScale, &Bits, 8);
                }
                else if (T[1] == "zn")
                    GMesh.ZNear = HexFloat(T[2]);
                else if (T[1] == "pos")
                    memcpy(GMesh.Position, HexFloats(T[2]).data(), 12);
                else if (T[1] == "class")
                    GMesh.ClassifyResult = (int)Dec(T[2]);
            }
            else if (K == "pstate")
            {
                GMesh.bStateSet = T[1] != "-";
                GMesh.StateValue = GMesh.bStateSet ? (int)Dec(T[1]) : 0;
            }
            else if (K == "failvm")
                GMesh.bLockFail = true;
            else if (K == "lib")
            {
                IFileSystem* Fs = NodeFs(".");
                void* List = Library->Load(Fs, -1);
                Fs->Release();
                Libraries[T[1]] = List;
                std::string Count = "-";
                if (List != nullptr)
                    Count = Format("%u", (unsigned int)((FVmeshDataList*)List)->size());
                Op("\"op\":\"lib\",\"n\":\"" + Count + "\"");
            }
            else if (K == "unlib")
            {
                Library->Unload(Libraries[T[1]]);
                Libraries.erase(T[1]);
                Op("\"op\":\"unlib\"");
            }
            else if (K == "arch")
            {
                const int Id = (int)Dec(T[1]);
                IFileSystem* Fs = NodeFs(T[2]);
                const int R = Manager->CreateArchetype((void*)(intptr_t)Id, Fs);
                Fs->Release();
                Op(Format("\"op\":\"arch\",\"r\":\"%d\",", R) + LodText(Id));
            }
            else if (K == "dup")
            {
                const int R = Manager->DuplicateArchetype((void*)(intptr_t)Dec(T[1]), (void*)(intptr_t)Dec(T[2]));
                Op(Format("\"op\":\"dup\",\"r\":\"%d\",", R) + LodText((int)Dec(T[1])));
            }
            else if (K == "destroy")
            {
                const int R = Manager->DestroyArchetype((void*)(intptr_t)Dec(T[1]));
                Op(Format("\"op\":\"destroy\",\"r\":\"%d\"", R));
            }
            else if (K == "inst")
            {
                const int R = Manager->CreateInstance((void*)(uintptr_t)HexWord(T[1]), (void*)(intptr_t)Dec(T[2]));
                Op(Format("\"op\":\"inst\",\"r\":\"%d\"", R));
            }
            else if (K == "uninst")
            {
                const int R = Manager->DestroyInstance((void*)(uintptr_t)HexWord(T[1]));
                Op(Format("\"op\":\"uninst\",\"r\":\"%d\"", R));
            }
            else if (K == "tick")
            {
                const int R = Manager->UpdateInstance((void*)(uintptr_t)HexWord(T[1]), (void*)(uintptr_t)HexWord(T[2]));
                Op(Format("\"op\":\"tick\",\"r\":\"%d\"", R));
            }
            else if (K == "notify")
            {
                const int R = Manager->Notify((void*)(uintptr_t)HexWord(T[1]));
                Op(Format("\"op\":\"notify\",\"r\":\"%d\"", R));
            }
            else if (K == "draw")
            {
                std::vector<float> Matrix;
                IMaterial* Extra = nullptr;
                for (size_t I = 4; I < T.size(); ++I)
                {
                    if (T[I].compare(0, 2, "m=") == 0)
                        Matrix = HexFloats(T[I].substr(2));
                    else if (T[I].compare(0, 2, "x=") == 0)
                        Extra = MaterialNamed(T[I].substr(2));
                }
                const int R = Manager->RenderInstance(&GMeshCamera, (void*)(uintptr_t)HexWord(T[1]), HexFloat(T[2]),
                                                      HexWord(T[3]), Matrix.empty() ? nullptr : Matrix.data(), Extra);
                Op(Format("\"op\":\"draw\",\"r\":\"%d\"", R));
            }
            else if (K == "lods")
            {
                int Count = -9, Current = -9;
                const int R1 = Manager->GetArchetypeLodCount((void*)(intptr_t)Dec(T[1]), &Count);
                const int R2 = Manager->GetArchetypeCurrentLod((void*)(intptr_t)Dec(T[1]), &Current);
                Op(Format("\"op\":\"lods\",\"r\":\"%d,%d\",\"n\":\"%d\",\"cur\":\"%d\"", R1, R2, Count, Current));
            }
            else if (K == "ilods")
            {
                int Count = -9, Current = -9;
                const int R1 = Manager->GetInstanceLodCount((void*)(uintptr_t)HexWord(T[1]), &Count);
                const int R2 = Manager->GetInstanceCurrentLod((void*)(uintptr_t)HexWord(T[1]), &Current);
                Op(Format("\"op\":\"ilods\",\"r\":\"%d,%d\",\"n\":\"%d\",\"cur\":\"%d\"", R1, R2, Count, Current));
            }
            else if (K == "setlod")
            {
                const int R = Manager->SetArchetypeLod((void*)(intptr_t)Dec(T[1]), (int)Dec(T[2]));
                Op(Format("\"op\":\"setlod\",\"r\":\"%d\"", R));
            }
            else if (K == "ilod")
            {
                const int R = Manager->SetInstanceLod((void*)(uintptr_t)HexWord(T[1]), (int)Dec(T[2]));
                Op(Format("\"op\":\"ilod\",\"r\":\"%d\"", R));
            }
            else if (K == "range")
            {
                float A = -9.0f, B = -9.0f;
                const int R = Manager->GetArchetypeLodRange((void*)(intptr_t)Dec(T[1]), (int)Dec(T[2]), &A, &B);
                Op(Format("\"op\":\"range\",\"r\":\"%d\",\"a\":\"", R) + F(A) + "\",\"b\":\"" + F(B) + "\"");
            }
            else if (K == "setrange")
            {
                const int R = Manager->SetArchetypeLodRange((void*)(intptr_t)Dec(T[1]), (int)Dec(T[2]), HexFloat(T[3]),
                                                            HexFloat(T[4]));
                Op(Format("\"op\":\"setrange\",\"r\":\"%d\"", R));
            }
            else if (K == "geom" || K == "geomat")
            {
                int Out = -9;
                const int Mode = (int)Dec(T[3]);
                const unsigned long Flags = HexWord(T[4]);
                int R;
                if (K == "geom")
                    R = Manager->QueryGeometry((void*)(intptr_t)Dec(T[1]), HexFloat(T[2]), (void*)(intptr_t)Mode,
                                               (void*)(uintptr_t)Flags, nullptr, &Out, nullptr);
                else
                    R = Manager->QueryGeometryAtLod((void*)(intptr_t)Dec(T[1]), (void*)(intptr_t)Dec(T[2]),
                                                    (void*)(intptr_t)Mode, (void*)(uintptr_t)Flags, nullptr, &Out, nullptr);
                Op(Format("\"op\":\"%s\",\"r\":\"%d\",\"v\":\"%d\"", K.c_str(), R, Out));
            }
            else if (K == "bounds" || K == "centroid")
            {
                float Out[6] = {-9, -9, -9, -9, -9, -9};
                int R;
                if (K == "bounds")
                    R = Manager->GetBounds((void*)(intptr_t)Dec(T[1]), HexFloat(T[2]), Out);
                else
                    R = Manager->GetCentroid((void*)(intptr_t)Dec(T[1]), HexFloat(T[2]), Out);
                std::string V;
                for (int I = 0; I < (K == "bounds" ? 6 : 3); ++I)
                    V += (I ? "," : "") + F(Out[I]);
                Op(Format("\"op\":\"%s\",\"r\":\"%d\",\"v\":\"", K.c_str(), R) + V + "\"");
            }
            else if (K == "root")
            {
                const int R = Manager->SetArchetypeRootLodSelection((void*)(intptr_t)Dec(T[1]), nullptr, HexWord(T[2]));
                unsigned long Flags = 9;
                const int R2 = Manager->GetArchetypeRootLodSelection((void*)(intptr_t)Dec(T[1]), nullptr, &Flags);
                Op(Format("\"op\":\"root\",\"r\":\"%d,%d\",\"v\":\"%lu\"", R, R2, Flags));
            }
            else if (K == "qarch")
            {
                void* Out = nullptr;
                const int R = Manager->QueryArchetype((void*)(intptr_t)Dec(T[1]), IidOf(T[2]), &Out);
                Op(Format("\"op\":\"qarch\",\"r\":\"%d\"", R));
            }
            else if (K == "qinst")
            {
                void* Out = nullptr;
                const int R = Manager->QueryInstance((void*)(uintptr_t)HexWord(T[1]), IidOf(T[2]), &Out);
                Op(Format("\"op\":\"qinst\",\"r\":\"%d\"", R));
            }
            else if (K == "wire")
            {
                IFileSystem* Fs = NodeFs(T[2]);
                const int R = Wire->CreateArchetype((void*)(intptr_t)Dec(T[1]), Fs);
                Fs->Release();
                Op(Format("\"op\":\"wire\",\"r\":\"%d\"", R));
            }
            else if (K == "wdraw")
            {
                void* Out = nullptr;
                int R = Wire->QueryArchetype((void*)(intptr_t)Dec(T[1]), Iids::VMeshWire, &Out);
                if (R == 0)
                {
                    R = ((IVMeshWire*)Out)->Draw();
                    ((IVMeshWire*)Out)->Release();
                }
                Op(Format("\"op\":\"wdraw\",\"r\":\"%d\"", R));
            }
            else if (K == "sphmax" || K == "sphradius" || K == "sphscale")
            {
                FSphereAnim* Anim = SphereOf(HexWord(T[1]));
                if (K == "sphmax")
                    Anim->SetMaxTessellationLevel((int)Dec(T[2]));
                else if (K == "sphradius")
                    Anim->SetRadius(HexFloat(T[2]));
                else
                    Anim->SetLodDistanceScale(HexFloat(T[2]));
                Op("\"op\":\"" + K + "\"");
            }
            else if (K == "sphlevel")
            {
                FSphereAnim* Anim = SphereOf(HexWord(T[1]));
                Anim->UpdateLevel(HexFloat(T[2]));
                FSphereMaterialSlot* Slot = *Anim->MaterialSlots.begin();
                Op(Format("\"op\":\"sphlevel\",\"level\":\"%d\",\"nv\":\"%lu\",\"ni\":\"%lu\",\"src\":\"", Anim->CachedLevel,
                          Anim->VertexCount, Slot->IndexCount) +
                   SourceText(&Anim->Desc) + "\",\"idx\":\"" + HashOf(Slot->Indices, Slot->IndexCount * 2) + "\"");
            }
            else if (K == "end")
            {
                delete Manager;
                Manager = nullptr;
                Op("\"op\":\"end\"");
            }
            else
            {
                fprintf(stderr, "mesh_render: unknown operation %s\n", K.c_str());
                GFailed = true;
            }
        }
    };
}

// The suite (mesh_render.jsonl), called from RunMaterialSuites after the material suites.
void RunMeshRenderSuite()
{
    using namespace mesh_render;
    Suite Out("mesh_render.jsonl");
    if (!OracleLoadMathEngine())
    {
        fprintf(stderr, "mesh_render: no 3DMathEngine\n");
        GFailed = true;
        return;
    }
    const std::vector<std::string> Lines = InputLines("mesh_render.list");
    void(__cdecl * Saved)(int, const char*, ...) = FDUMP;
    IRenderPipeline* const SavedPipeline = G_Pipeline;
    IRPDraw* const SavedDraw = G_Draw;
    IRPIndexBuffer* const SavedIndexBuffer = G_IndexBuffer;
    IRPVertexBuffer* const SavedVertexBuffer = G_VertexBuffer;
    IVertexBufferManager* const SavedVbManager = G_VbManager;
    IMaterialLibrary* const SavedMaterialLibrary = G_MaterialLibrary;
    IEngine* const SavedEngine = G_Engine;
    FDUMP = MeshDump;
    for (size_t Case = 0; Case < Lines.size(); ++Case)
    {
        GMesh = FMeshState();
        GMesh.Out = &Out;
        GMesh.Case = (int)Case;
        FMeshCase Runner;
        Runner.Start();
        for (const std::string& Op : Words(Lines[Case], " ; "))
            Runner.Run(Words(Op, " "));
    }
    FDUMP = Saved;
    G_Pipeline = SavedPipeline;
    G_Draw = SavedDraw;
    G_IndexBuffer = SavedIndexBuffer;
    G_VertexBuffer = SavedVertexBuffer;
    G_VbManager = SavedVbManager;
    G_MaterialLibrary = SavedMaterialLibrary;
    G_Engine = SavedEngine;
    G_VmeshDataMap.clear();
}
