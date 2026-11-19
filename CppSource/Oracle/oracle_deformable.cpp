// P4.7 (first increment): Deformable2.dll at run time -- the skeleton and its bone runtimes, the
// LOD holder's bone link, CPU skinning, the index buffer, the draw through the face groups'
// materials (the EyeMaterial with its UV-deform binding), parts and levels, the instance's draw,
// bounds and statistics, the animation list and the pose calls, aim solving (the aim event, the
// single-joint look-at with its Euler clamp, both damped-least-squares solvers over the SVD) and
// the EyeMaterial's parameters -- through the reference's own code, against recording services.
// The Rust side is crates/neuron-client/tests/support/deformable.rs with the same contract; the
// input is crates/neuron-client/tests/fixtures/oracle/deformable.list from
// make_deformable_fixtures.py.
//
// Deformable2.dll exports nothing but DllMain; oracle_mesh_data_dfm.cpp compiles its sources for
// P1.7 and includes this file at its end, so the suite reaches the reference's classes and static
// functions. The module globals they reach (G_Engine -- renamed G_DeformableEngine there --,
// G_Animation, G_Channel, G_Renderer, G_Pipeline, G_VertexBuffer, G_IndexBuffer, G_DeformRate,
// G_MeshCache) point at the recording fakes below. Meshes and skeletons are built from the
// fixture's tables with the reference's own builders (RebaseFaceIndices, BindUvBoneIdsToFaceGroups,
// BuildVertexRemap, BuildBoneVertexLists, ConstructSkeleton, FLodHolder::AttachSkeleton,
// FDeformPart::BindMeshes); the loaders over UTF archives are P1.7's.
//
// Every call writes one line {"case","ev",...} (strings throughout; floats as the eight hex
// digits of the f32, integers in decimal unless marked hex):
//   relh "h" / cinst "h","r" / link "inst","arch","r" / dinst "inst" / setxf "inst","bone" /
//   parent "inst","r" / pos "inst" / ori "inst" / setpos "inst","v" / setori "inst","v" /
//   jp "inst","n" / hpq "r" / enumhp "arch" / connhp "a","b","name","r"
//   play "name","root","extra","script","r" / dscript "h" / filter "h","r" /
//   root "h","flags","t","s","d","f","r","v" / named "name","extra","r","v" /
//   evalpb "h","flags","t","s","d","f","x","r" / hasobj "h","key","r" / firstf "h","v" /
//   durn "h","r","v" / touch "h" / ovpos "h","v" / ovextra "h","v" / addov "h","v" /
//   findobj "h","kind","inst","r" / reghandle "k" / hashandle "k","r" / hasany "k"
//   ctyped "joint","bind","r" / cplay "key","blob" / cdestroy "key" / inmixer "key","r" /
//   sample "key","t"
//   rinst "inst","flags","arg","r"
//   pstate "id","r" / modelview / world "m" / worldn "i","m" / getrs "s","v" / setrs "s","v" /
//   setmat "v" / tss "st","s","v" / tex "st","h" / ttrans "st","m" / texfmt "h","r","v" / verify
//   dipvb "prim","vb","base","n","ibs","span"
//   isvb / selvb "vb","r" / createvb "fvf","n","vb" / destroyvb "vb" / lockvb "vb","n" /
//   copyv "vb","base","start","n","src" / unlockvb "vb","data"
//   isib / selib "ib","base","r" / createib "n","ib" / destroyib "ib" / lockib "ib","n" /
//   unlockib "ib","data"
//   dip "prim","min","nv","start","n"
//   bind / teardown "m",<context> / submit "m",<context>,"prim","start","count","ibs","span","flags" /
//   class "m" / mrelease "m"
//   camview / classify "c","r","res" / vmdraw "part","inst"
//   findtex "name","r" / fcount "rec" / cplayback "rec","id" / getframe "id","frame","v" /
//   reltex "rec" / decref "id"
//   dump "flags","file","line","msg" (line hex, msg hex)
// with <context> "inst","world","view","vb","ib","f20". Every operation then writes
// {"case","op",...}.
//
// The second increment's suite (deformable_parts.jsonl, input deformable_parts.list) runs the
// same cases with the operations of oracle_deformable_parts.cpp, which this file reaches through
// RunPartsOp: parts made from archives by the reference's Create (ConnectInstanceParts and
// LoadSkeleton over an in-memory file system) and the EyeMaterial made by its factory. For them
// the engine also records
//   carch "name","r" (CreateArchetypeHandle) / qvmesh "h","r" (QueryEngineComponent)
// and the material library
//   findmat "name","r" (FindMaterialByCrc) / maprule "key","pattern","type" (AddMapRule).

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace deformable_oracle
{
    using namespace oracle;

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
    float HexFloat(const std::string& Text) { return FloatFromBits((unsigned int)HexWord(Text)); }
    std::vector<float> HexFloats(const std::string& Text)
    {
        std::vector<float> Out;
        if (Text == "-" || Text.empty())
            return Out;
        for (const std::string& Part : Words(Text, ","))
            Out.push_back(HexFloat(Part));
        return Out;
    }
    std::vector<long> Decs(const std::string& Text)
    {
        std::vector<long> Out;
        if (Text == "-" || Text.empty())
            return Out;
        for (const std::string& Part : Words(Text, ","))
            Out.push_back(Dec(Part));
        return Out;
    }
    std::string F(float Value) { return Format("%08x", FloatBits(Value)); }
    std::string Fs(const float* V, int N)
    {
        std::string Out;
        for (int Index = 0; Index < N; ++Index)
            Out += (Index ? "," : "") + F(V[Index]);
        return Out;
    }
    long Id(const void* Pointer) { return (long)(intptr_t)Pointer; }
    void* Ptr(long Value) { return (void*)(intptr_t)Value; }

    // --- the recording state ------------------------------------------------------------------

    struct FDefMat;

    struct FSample
    {
        std::vector<float> A;
        std::vector<float> B;
        float Alpha = 0.0f;
    };

    struct FDefState
    {
        Suite* Out = nullptr;
        int Case = 0;
        // engine
        std::map<long, std::vector<float>> Matrix;
        std::map<long, long> Parent;
        std::map<long, std::vector<unsigned long>> Relative;
        std::map<long, std::vector<float>> Position;
        std::map<long, std::vector<float>> Orientation;
        std::map<long, std::vector<float>> JointParams;
        long SkelHandle = 0x50;
        long LinkRoot = -1;
        std::vector<std::pair<long, long>> Links;
        long NextInstance = 0x500;
        // The archetype handles CreateArchetypeHandle hands out (skeletons get SkelHandle) and the
        // bone links queued for the next skeleton instances (the parts suite).
        long NextArch = 0x100;
        std::vector<std::pair<long, std::vector<std::pair<long, long>>>> LinkQueue;
        bool bHardpoints = true;
        std::map<long, std::vector<std::string>> HardpointNames;
        unsigned long ConnectResult = 0;
        // animation
        std::vector<long> PlayResults;
        long NextPlay = 0x700;
        std::set<long> Filtered;
        std::map<long, float> Durations;
        std::map<long, std::vector<float>> Touch;
        bool bRootData = true;
        unsigned char RootResult = 0;
        std::vector<float> RootVec;
        bool bNamed = false;
        float NamedValue = 0.0f;
        long EvalResult = 0;
        std::set<std::pair<long, unsigned long>> Objects;
        std::map<long, unsigned long> FirstField;
        std::set<long> Blocked;
        // channels
        unsigned long NextChannel = 0x900;
        std::set<unsigned long> InMixer;
        std::map<unsigned long, FSample> Samples;
        // renderer, pipeline, buffers
        int RenderRank = 3;
        bool bStateSet = true;
        int StateValue = 1;
        std::map<long, int> TextureFormats;
        std::map<unsigned long, std::vector<unsigned char>> VbData;
        std::map<unsigned long, std::vector<unsigned short>> IbData;
        std::set<unsigned long> LiveVb;
        std::set<unsigned long> LiveIb;
        unsigned long NextVb = 0x100;
        unsigned long NextIb = 0x200;
        int DrawResult = 0;
        // textures
        std::map<std::string, std::pair<int, unsigned long>> Textures; // name -> frames, frame handle
        int NextPlayback = 0x40;
        // camera
        float View[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
        int ClassifyResult = 4;
        // the objects of the case
        FLodHolder* Holder = nullptr;
        std::vector<FDeformPart*> Parts;
        FDeformInstance* Instance = nullptr;
        std::map<std::string, IMaterial*> Materials;
        std::map<std::string, FDefMat*> Fakes;
        std::map<long, std::vector<unsigned char>> AimDescs;
        std::map<long, std::vector<float>> AimTargets;
        std::vector<FIkListNode*> AimNodes;
        // the caller's channel sink
        int Forwarded = 0;
        unsigned long Cookie = 0;

        void Event(const std::string& Body) { Out->Line(Format("{\"case\":\"%d\",", Case) + Body + "}"); }
    };

    FDefState G;

    const float Identity12[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    const float Identity9[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

    std::vector<float>& Floats(std::map<long, std::vector<float>>& Map, long Key, const float* Default, int N)
    {
        std::map<long, std::vector<float>>::iterator Found = Map.find(Key);
        if (Found == Map.end())
            Found = Map.insert(std::make_pair(Key, std::vector<float>(Default, Default + N))).first;
        return Found->second;
    }

    std::string StreamText(const FRpVertexStream& S, unsigned long SrcCount)
    {
        std::string Elements = "-";
        if (S.Base != nullptr && S.Count != 0)
        {
            std::vector<unsigned char> Bytes;
            for (unsigned int I = 0; I < S.Count; ++I)
                Bytes.insert(Bytes.end(), S.Base + I * S.Stride, S.Base + I * S.Stride + S.ByteSize);
            Elements = Fnv64(Bytes.data(), Bytes.size());
        }
        std::string Index = "-";
        if (S.IndexArray != nullptr)
            Index = Fnv64(S.IndexArray, (size_t)SrcCount * 4);
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
        return Format("\"inst\":\"%ld\",\"world\":\"", Id(C->Inst)) + Fs((const float*)C->World, 12) + "\",\"view\":\"" +
               Fs((const float*)C->View, 12) + "\",\"vb\":\"" + Vb + "\",\"ib\":\"" + Ib + "\",\"f20\":\"" +
               (C->Field20 == nullptr ? "0" : "1") + "\"";
    }

    // --- materials ----------------------------------------------------------------------------

    struct FDefMat final : IMaterial
    {
        std::string Name;
        std::string Class;

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override
        {
            G.Event(Format("\"ev\":\"mrelease\",\"m\":\"%s\"", Name.c_str()));
            return 1;
        }
        GENRESULT NEURONCOMPONENTS_API Bind(void* Arg) override
        {
            G.Event(Format("\"ev\":\"bind\",\"m\":\"%s\",", Name.c_str()) + Context(Arg));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Teardown(void* Arg) override
        {
            G.Event(Format("\"ev\":\"teardown\",\"m\":\"%s\",", Name.c_str()) + Context(Arg));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Submit(void* Arg, unsigned long Prim, unsigned long Start, unsigned long Count,
                                              unsigned long IbStart, unsigned long Span, unsigned long Flags) override
        {
            G.Event(Format("\"ev\":\"submit\",\"m\":\"%s\",", Name.c_str()) + Context(Arg) +
                    Format(",\"prim\":\"%lu\",\"start\":\"%lu\",\"count\":\"%lu\",\"ibs\":\"%lu\",\"span\":\"%lu\","
                           "\"flags\":\"%lx\"",
                           Prim, Start, Count, IbStart, Span, Flags));
            return GR_OK;
        }
        const char* __thiscall ClassName() override
        {
            G.Event(Format("\"ev\":\"class\",\"m\":\"%s\"", Name.c_str()));
            return Class.c_str();
        }
        // The slots Deformable2 does not call.
        GENRESULT NEURONCOMPONENTS_API Initialize(INeuronComponent* Outer) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LoadFromFileSystem(void* FileSystem) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SaveProperties(void* FileSystem) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ApplyVerify(void* Context, void* Extra) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ResolveTextures(float Delta) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetName(const char* Name) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetName(char* Out, unsigned long Size) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CopyClassName(char* Out, unsigned long Size) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetPassCount(int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetRenderFlags(unsigned long* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetParameterCount(unsigned long* OutCount) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetParameterDescriptor(unsigned long Index, void* OutDesc) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetParameterValue(unsigned long Index, void* InOutDesc) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetParameterValue(unsigned long Index, void* Value) override { return GR_GENERIC; }
        void* __thiscall ScalarDeletingDestructor(unsigned char bDelete) override { return nullptr; }
        GENRESULT __thiscall InitializeFromDescriptor(void* Arg) override { return GR_GENERIC; }
        GENRESULT __thiscall ApplyInitialRenderStates(void* Arg) override { return GR_GENERIC; }
        GENRESULT __thiscall Setup() override { return GR_GENERIC; }
        bool __thiscall ValueWithinDeclaredRange(unsigned long Index, void* Entry, void* Value) override { return false; }
        GENRESULT __thiscall StoreParameterValue(unsigned long Index, void* Entry, void* Value) override { return GR_GENERIC; }
        unsigned long __thiscall GetParameterTable(void** OutBase) override { return 0; }
        void* __thiscall GetParameterAddress(unsigned long Index) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API InnerQueryInterface(const char* Iid, void** Out) override { return GR_GENERIC; }
        unsigned long NEURONCOMPONENTS_API InnerAddRef() override { return 0; }
        unsigned long NEURONCOMPONENTS_API InnerRelease() override { return 0; }
    };

    // --- the engine ---------------------------------------------------------------------------

    // The search path GetSearchPath hands out; LoadSkeleton releases it.
    class FDefSearchPath final : public INeuronComponent
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
    };

    FDefSearchPath GSearchPath;

    class FDefHardpoint final : public IHardpoint
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API EnumerateHardpoints(FHardpointEnumProc Callback, void* Handle, void* Context) override
        {
            G.Event(Format("\"ev\":\"enumhp\",\"arch\":\"%ld\"", Id(Handle)));
            const std::vector<std::string> Names = G.HardpointNames[Id(Handle)];
            for (const std::string& Name : Names)
                Callback(Name.c_str(), Context);
            return GR_OK;
        }
        unsigned long NEURONCOMPONENTS_API ConnectHardpoints(void* A, FHardpointKey const* KeyA, void* B,
                                                             FHardpointKey const* KeyB) override
        {
            G.Event(Format("\"ev\":\"connhp\",\"a\":\"%ld\",\"b\":\"%ld\",\"name\":\"%s\",\"r\":\"%lu\"", Id(A), Id(B),
                           KeyA->Name, G.ConnectResult));
            return G.ConnectResult;
        }
        // The slots Deformable2 does not call.
        unsigned long NEURONCOMPONENTS_API GetHardpoint(void* Handle, FHardpointKey const* Key, void* Out) override { return 0; }
        unsigned long NEURONCOMPONENTS_API AddHardpoint(void* Handle, FHardpointKey const* Key, const void* Data) override { return 0; }
        char const* NEURONCOMPONENTS_API FindHardpoint(void* Handle, unsigned long const* Crc) override { return nullptr; }
    };

    FDefHardpoint GHardpoint;

    class FDefEngine final : public IEngine
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            if (strcmp(Iid, Iids::Hardpoint) == 0)
            {
                G.Event(Format("\"ev\":\"hpq\",\"r\":\"%d\"", G.bHardpoints ? 0 : -1));
                *Out = G.bHardpoints ? (void*)&GHardpoint : nullptr;
                return G.bHardpoints ? GR_OK : GR_INTERFACE_UNSUPPORTED;
            }
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        unsigned long NEURONCOMPONENTS_API ReleaseHandle(void* Handle) override
        {
            G.Event(Format("\"ev\":\"relh\",\"h\":\"%ld\"", Id(Handle)));
            return 0;
        }
        void* NEURONCOMPONENTS_API CreateInstance(void* Handle, void*) override
        {
            if (Id(Handle) == G.SkelHandle)
            {
                if (!G.LinkQueue.empty())
                {
                    G.LinkRoot = G.LinkQueue.front().first;
                    G.Links = G.LinkQueue.front().second;
                    G.LinkQueue.erase(G.LinkQueue.begin());
                }
                G.Event(Format("\"ev\":\"cinst\",\"h\":\"%ld\",\"r\":\"%ld\"", Id(Handle), G.LinkRoot));
                for (const std::pair<long, long>& Link : G.Links)
                {
                    const GENRESULT R = EngineCallbackTrampoline(nullptr, Ptr(Link.first), Ptr(Link.second));
                    G.Event(Format("\"ev\":\"link\",\"inst\":\"%ld\",\"arch\":\"%ld\",\"r\":\"%d\"", Link.first,
                                   Link.second, (int)(R & 0xFF)));
                }
                return Ptr(G.LinkRoot);
            }
            const long Made = G.NextInstance++;
            G.Event(Format("\"ev\":\"cinst\",\"h\":\"%ld\",\"r\":\"%ld\"", Id(Handle), Made));
            return Ptr(Made);
        }
        GENRESULT NEURONCOMPONENTS_API DestroyInstance(void* Instance) override
        {
            G.Event(Format("\"ev\":\"dinst\",\"inst\":\"%ld\"", Id(Instance)));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetInstanceTransform(void* Instance, void* Transform) override
        {
            const long Bone = G.Holder != nullptr ? (long)((FBoneRuntime*)Transform - G.Holder->BoneSlots) : -1;
            G.Event(Format("\"ev\":\"setxf\",\"inst\":\"%ld\",\"bone\":\"%ld\"", Id(Instance), Bone));
            if (Id(Instance) == -1)
                return GR_OK;
            IInstanceTransform* T = (IInstanceTransform*)Transform;
            T->Init(Instance);
            T->SetMatrix(Instance, Floats(G.Matrix, Id(Instance), Identity12, 12).data());
            float Zero[3] = {0, 0, 0};
            T->SetExtra(Instance, 0.0f, Zero);
            T->SetUser(Instance, 0);
            return GR_OK;
        }
        void* NEURONCOMPONENTS_API GetInstanceParent(void* Instance) override
        {
            std::map<long, long>::iterator Found = G.Parent.find(Id(Instance));
            const long R = Found == G.Parent.end() ? -1 : Found->second;
            G.Event(Format("\"ev\":\"parent\",\"inst\":\"%ld\",\"r\":\"%ld\"", Id(Instance), R));
            return Ptr(R);
        }
        unsigned long* NEURONCOMPONENTS_API GetInstanceRelative(void* Instance) override
        {
            // Not recorded: a pure query, and the number of IK rounds that ask it turns on the
            // last bit of a residual (the trial steps are not scaled; see the Rust port).
            std::map<long, std::vector<unsigned long>>::iterator Found = G.Relative.find(Id(Instance));
            if (Found == G.Relative.end())
                Found = G.Relative.insert(std::make_pair(Id(Instance), std::vector<unsigned long>(0x17, 0))).first;
            return Found->second.data();
        }
        float* NEURONCOMPONENTS_API GetInstancePosition(void* Instance) override
        {
            G.Event(Format("\"ev\":\"pos\",\"inst\":\"%ld\"", Id(Instance)));
            static const float Zero[3] = {0, 0, 0};
            return Floats(G.Position, Id(Instance), Zero, 3).data();
        }
        float* NEURONCOMPONENTS_API GetInstanceOrientation(void* Instance) override
        {
            G.Event(Format("\"ev\":\"ori\",\"inst\":\"%ld\"", Id(Instance)));
            return Floats(G.Orientation, Id(Instance), Identity9, 9).data();
        }
        GENRESULT NEURONCOMPONENTS_API SetInstancePosition(void* Instance, void* Position) override
        {
            G.Event(Format("\"ev\":\"setpos\",\"inst\":\"%ld\",\"v\":\"", Id(Instance)) + Fs((const float*)Position, 3) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetInstanceOrientation(void* Instance, void* Orient) override
        {
            G.Event(Format("\"ev\":\"setori\",\"inst\":\"%ld\",\"v\":\"", Id(Instance)) + Fs((const float*)Orient, 9) + "\"");
            return GR_OK;
        }
        unsigned long NEURONCOMPONENTS_API GetJointParams(void* Instance, unsigned long, float* Out) override
        {
            const std::vector<float>& Params = G.JointParams[Id(Instance)];
            G.Event(Format("\"ev\":\"jp\",\"inst\":\"%ld\",\"n\":\"%u\"", Id(Instance), (unsigned int)Params.size()));
            for (size_t Index = 0; Index < Params.size(); ++Index)
                Out[Index] = Params[Index];
            return Params.empty() ? 0 : 1;
        }
        // The slots Deformable2 does not call.
        GENRESULT NEURONCOMPONENTS_API LoadEngineComponents(INeuronComponent* SystemContainer) override { return GR_GENERIC; }
        unsigned long NEURONCOMPONENTS_API EnumerateEngineComponents( int(__cdecl* Callback)(const char*, void*)) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API NotifyEngineComponents(void* Arg) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetSearchPath(const char* Path) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API QuerySearchPath(char* Dest, unsigned long MaxChars) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetFilesystem(INeuronComponent*) override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API GetSearchPath(void** Out) override
        {
            *Out = &GSearchPath;
            return GR_OK;
        }
        void* NEURONCOMPONENTS_API CreateArchetypeHandle(FEngineName* Name, void*) override
        {
            const char* Text = Name->Text != nullptr ? Name->Text : "";
            const long R = strncmp(Text, "skel_", 5) == 0 ? G.SkelHandle : G.NextArch++;
            G.Event(Format("\"ev\":\"carch\",\"name\":\"%s\",\"r\":\"%ld\"", Text, R));
            return Ptr(R);
        }
        GENRESULT NEURONCOMPONENTS_API QueryEngineComponent(int Handle, void*, void** Out) override
        {
            G.Event(Format("\"ev\":\"qvmesh\",\"h\":\"%d\",\"r\":\"-1\"", Handle));
            *Out = nullptr;
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API CreateFileSystem(const char* Path, void** Out, void* Factory) override { return GR_GENERIC; }
        void* NEURONCOMPONENTS_API CreateCompoundArchetype(FEngineName* Name) override { return nullptr; }
        void* NEURONCOMPONENTS_API DuplicateArchetype(void* ArchIn, FEngineName* Name) override { return nullptr; }
        unsigned long NEURONCOMPONENTS_API AddRefHandle(void* Handle) override { return 0; }
        FEngineName* NEURONCOMPONENTS_API GetArchetypeName(FEngineName* Out, void* Handle) override { return nullptr; }
        void* NEURONCOMPONENTS_API FindArchetype(FEngineName* Name) override { return nullptr; }
        unsigned long NEURONCOMPONENTS_API IsCompoundArchetype(void* Handle) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API EnumerateArchetypeConstraints( void* Handle, int(__cdecl* Callback)(void*, void*, void*, void*), void* Context) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API EnumerateArchetypeChildren( void* Handle, int(__cdecl* Callback)(void*, void*, int, void*), void* Context) override { return GR_GENERIC; }
        void* NEURONCOMPONENTS_API CreateInstanceFromName(FEngineName* Name, void* FileSystem, void* Transform) override { return nullptr; }
        void* NEURONCOMPONENTS_API GetInstanceArchetype(void* Instance) override { return nullptr; }
        FEngineName* NEURONCOMPONENTS_API GetInstanceName(FEngineName* Out, void* Instance) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API UpdateInstance(void* Instance, unsigned long Flags, void* Arg) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetInstanceTransformObject(void* Instance, void** Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API QueryInstance(void* Instance, const char* Iid, void** Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetInstanceUserData(void* Instance, void* Value) override { return GR_GENERIC; }
        void* NEURONCOMPONENTS_API GetInstanceUserData(void* Instance) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API SetInstanceExtent(void* Instance, unsigned long Flags, float Extent, const float* Vec) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetInstanceExtent(void* Instance, unsigned long Flags, float* OutExtent, float* OutVec) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetInstanceMatrix(void* Instance, void* Matrix) override { return GR_GENERIC; }
        float* NEURONCOMPONENTS_API GetInstanceTransform(void* Instance) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API NoOp41(void* A, void* B) override { return GR_GENERIC; }
        float* NEURONCOMPONENTS_API GetIdentityMatrix(void* Unused) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API NoOp43(void* A, void* B) override { return GR_GENERIC; }
        float* NEURONCOMPONENTS_API GetIdentityMatrix2(void* Unused) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API SetTransformUser(void* Instance, unsigned long User) override { return GR_GENERIC; }
        unsigned long NEURONCOMPONENTS_API GetTransformUser(void* Instance) override { return 0; }
        unsigned long NEURONCOMPONENTS_API CreateJoint(void* Parent, void* Child, const unsigned long* Payload) override { return 0; }
        unsigned long NEURONCOMPONENTS_API DestroyJoint(void* Parent, void* Child) override { return 0; }
        unsigned long NEURONCOMPONENTS_API SetJointParams(void* Instance, unsigned long Index, const float* Values) override { return 0; }
        void* NEURONCOMPONENTS_API FindMappedInstance(void* Instance) override { return nullptr; }
        void* NEURONCOMPONENTS_API GetRelatedInstance(void* From, unsigned long Flags, void* Cursor) override { return nullptr; }
        void* NEURONCOMPONENTS_API GetInstanceRoot(void* Instance) override { return nullptr; }
        void* NEURONCOMPONENTS_API FindArchetypeChild(void* Handle, void* ChildHandle) override { return nullptr; }
        unsigned long NEURONCOMPONENTS_API RejectOperation(void* A, void* B) override { return 0; }
    };

    // --- the animation system -----------------------------------------------------------------

    class FDefRootData final : public IRootData
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        unsigned char NEURONCOMPONENTS_API Eval(unsigned long Handle, void* TouchArg, unsigned long Flags, float Time,
                                                float Scale, void* Arg6, void* Arg7, float Arg8) override
        {
            float* Vec = (float*)TouchArg;
            for (size_t Index = 0; Index < G.RootVec.size() && Index < 3; ++Index)
                Vec[Index] = G.RootVec[Index];
            G.Event(Format("\"ev\":\"root\",\"h\":\"%ld\",\"flags\":\"%lx\",\"t\":\"", (long)Handle, Flags) + F(Time) +
                    "\",\"s\":\"" + F(Scale) + Format("\",\"d\":\"%lx\",\"f\":\"%lx\",\"one\":\"", Id(Arg6), Id(Arg7)) +
                    F(Arg8) + Format("\",\"r\":\"%u\",\"v\":\"", G.RootResult) + Fs(Vec, 3) + "\"");
            return G.RootResult;
        }
    };

    FDefRootData GRootData;

    class FDefChannelArchetype final : public IChannelArchetype
    {
    public:
        unsigned long Key = 0;
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API GetFlags(unsigned long* Out) override
        {
            *Out = 0;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Sample(float Time, const unsigned char** OutA, const unsigned char** OutB,
                                              float* Alpha) override
        {
            FSample& S = G.Samples[Key];
            S.A.resize(8, 0.0f);
            S.B.resize(8, 0.0f);
            G.Event(Format("\"ev\":\"sample\",\"key\":\"%lx\",\"t\":\"", Key) + F(Time) + "\"");
            *OutA = (const unsigned char*)S.A.data();
            *OutB = (const unsigned char*)S.B.data();
            *Alpha = S.Alpha;
            return GR_OK;
        }
        // The slots Deformable2 does not call.
        GENRESULT NEURONCOMPONENTS_API Load(void* FileSystem) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDuration(float* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SamplePair(float Time, const unsigned char** OutLo, const unsigned char** OutHi, float* TimeLo, float* TimeHi, float* Alpha) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetFrame(unsigned char bLast, const unsigned char** Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CollectRange(float Time0, float Time1, float* OutKeys, unsigned long Capacity, unsigned long* Count) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CollectAt(float Time, float* OutKeys, unsigned long Capacity, unsigned long* Count) override { return GR_GENERIC; }
    };

    FDefChannelArchetype GChannelArchetype;

    class FDefChannel final : public IChannel
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        unsigned long NEURONCOMPONENTS_API CreateTyped(void*, unsigned long Type, void* BindA, void* BindB) override
        {
            const unsigned long* Cookie = (const unsigned long*)BindA;
            const unsigned long Key = G.NextChannel++;
            G.Event(Format("\"ev\":\"ctyped\",\"type\":\"%lu\",\"cookie\":\"%lx,%lx,%lx,%ld\",\"bind\":\"%ld\",\"r\":\"%lx\"", Type,
                           Cookie[0], Cookie[1], Cookie[2], (long)Cookie[3], Id(BindB), Key));
            return Key;
        }
        unsigned long NEURONCOMPONENTS_API Destroy(unsigned long Key) override
        {
            G.Event(Format("\"ev\":\"cdestroy\",\"key\":\"%lx\"", Key));
            return 0;
        }
        unsigned long NEURONCOMPONENTS_API GetObject(unsigned long Key, void** Out) override
        {
            GChannelArchetype.Key = Key;
            *Out = &GChannelArchetype;
            return 0;
        }
        unsigned long NEURONCOMPONENTS_API Play(unsigned long Key, float* Time) override
        {
            const unsigned long* Blob = (const unsigned long*)Time;
            G.Event(Format("\"ev\":\"cplay\",\"key\":\"%lx\",\"blob\":\"%lx,%lx,%lx,%lx,%lx,%lx,%lx\"", Key, Blob[0], Blob[1],
                           Blob[2], Blob[3], Blob[4], Blob[5], Blob[6]));
            return 0;
        }
        unsigned long NEURONCOMPONENTS_API InMixer(unsigned long Key) override
        {
            const unsigned long R = G.InMixer.count(Key) != 0 ? 1 : 0;
            G.Event(Format("\"ev\":\"inmixer\",\"key\":\"%lx\",\"r\":\"%lu\"", Key, R));
            return R;
        }
        // The slots Deformable2 does not call.
        unsigned long NEURONCOMPONENTS_API CreateFromObject(void* Object, void* BindA, void* BindB) override { return 0; }
        unsigned long NEURONCOMPONENTS_API StopGraph(unsigned long Key) override { return 0; }
        float NEURONCOMPONENTS_API GetDuration(unsigned long Key) override { return 0; }
        unsigned long NEURONCOMPONENTS_API Blend(unsigned long Key, float Duration) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API SetUser(unsigned long Key, unsigned long User) override { return GR_GENERIC; }
        float NEURONCOMPONENTS_API GetUser(unsigned long Key) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API SetOverlayPos(unsigned long Key, float* Pos) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetOverlayExtra(unsigned long Key, float* Extra) override { return GR_GENERIC; }
        unsigned long* NEURONCOMPONENTS_API GetHeader(unsigned long* Out, unsigned long Key) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API AddOverlayPos(unsigned long Key, float* Delta) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetMapped(unsigned long Key, unsigned char* Out) override { return GR_GENERIC; }
    };

    FDefChannel GChannel;

    class FDefAnimation final : public IAnimation2
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            if (strcmp(Iid, Iids::RootData) == 0 && G.bRootData)
            {
                *Out = &GRootData;
                return GR_OK;
            }
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        unsigned long NEURONCOMPONENTS_API PlayScript(void* Script, void* Root, const char* Name, void* Sink, void* Script2) override
        {
            long R = G.NextPlay++;
            if (!G.PlayResults.empty())
            {
                R = G.PlayResults.front();
                G.PlayResults.erase(G.PlayResults.begin());
            }
            G.Event(Format("\"ev\":\"play\",\"name\":\"%lx\",\"root\":\"%ld\",\"extra\":\"%lx\",\"sink\":\"%d\",\"script\":\"%lx\",\"r\":\"%ld\"",
                           Id(Script), Id(Root), Id(Name), Sink == (void*)G.Instance ? 1 : 0, Id(Script2), R));
            return (unsigned long)R;
        }
        GENRESULT NEURONCOMPONENTS_API DestroyScript(void* Handle) override
        {
            G.Event(Format("\"ev\":\"dscript\",\"h\":\"%ld\"", Id(Handle)));
            return GR_OK;
        }
        unsigned long NEURONCOMPONENTS_API EvalPlayback(unsigned long Handle, unsigned long Flags, float Time, float Scale, void* A,
                                                        void* B, void* C, void* D) override
        {
            G.Event(Format("\"ev\":\"evalpb\",\"h\":\"%ld\",\"flags\":\"%lx\",\"t\":\"", (long)Handle, Flags) + F(Time) +
                    "\",\"s\":\"" + F(Scale) + Format("\",\"d\":\"%lx\",\"f\":\"%lx\",\"one\":\"%lx\",\"x\":\"", Id(A), Id(B), Id(C)) +
                    Fs((const float*)D, 12) + Format("\",\"r\":\"%ld\"", G.EvalResult));
            return (unsigned long)G.EvalResult;
        }
        unsigned long NEURONCOMPONENTS_API FilterObjects(void* Handle) override
        {
            const unsigned long R = G.Filtered.count(Id(Handle)) != 0 ? 1 : 0;
            G.Event(Format("\"ev\":\"filter\",\"h\":\"%ld\",\"r\":\"%lu\"", Id(Handle), R));
            return R;
        }
        unsigned long NEURONCOMPONENTS_API SetOverlayPos(unsigned long Handle, float* Pos) override
        {
            G.Event(Format("\"ev\":\"ovpos\",\"h\":\"%ld\",\"v\":\"", (long)Handle) + Fs(Pos, 3) + "\"");
            return 0;
        }
        unsigned long NEURONCOMPONENTS_API SetOverlayExtra(unsigned long Handle, float* Extra) override
        {
            G.Event(Format("\"ev\":\"ovextra\",\"h\":\"%ld\",\"v\":\"", (long)Handle) + Fs(Extra, 9) + "\"");
            return 0;
        }
        unsigned long NEURONCOMPONENTS_API HasObject(unsigned long Handle, void* Object) override
        {
            const unsigned long R = G.Objects.count(std::make_pair((long)Handle, (unsigned long)Id(Object))) != 0 ? 1 : 0;
            G.Event(Format("\"ev\":\"hasobj\",\"h\":\"%ld\",\"key\":\"%lx\",\"r\":\"%lu\"", (long)Handle, Id(Object), R));
            return R;
        }
        unsigned long NEURONCOMPONENTS_API TouchMapped(unsigned long Handle, float* Pos, float* Extra) override
        {
            G.Event(Format("\"ev\":\"touch\",\"h\":\"%ld\"", (long)Handle));
            const std::vector<float>& V = G.Touch[(long)Handle];
            for (size_t Index = 0; Index < V.size() && Index < 6; ++Index)
                (Index < 3 ? Pos : Extra)[Index % 3] = V[Index];
            return 0;
        }
        unsigned long NEURONCOMPONENTS_API AddOverlayPos(unsigned long Handle, float* Delta) override
        {
            G.Event(Format("\"ev\":\"addov\",\"h\":\"%ld\",\"v\":\"", (long)Handle) + Fs(Delta, 3) + "\"");
            return 0;
        }
        unsigned long NEURONCOMPONENTS_API GetFirstInstanceField(unsigned long Handle, unsigned long* Out) override
        {
            const unsigned long V = G.FirstField[(long)Handle];
            G.Event(Format("\"ev\":\"firstf\",\"h\":\"%ld\",\"v\":\"%lx\"", (long)Handle, V));
            *Out = V;
            return 0;
        }
        unsigned long NEURONCOMPONENTS_API GetDurationIfNamed(unsigned long Handle, float* Out) override
        {
            std::map<long, float>::iterator Found = G.Durations.find((long)Handle);
            const bool bHit = Found != G.Durations.end();
            G.Event(Format("\"ev\":\"durn\",\"h\":\"%ld\",\"r\":\"%d\",\"v\":\"", (long)Handle, bHit ? 1 : 0) +
                    F(bHit ? Found->second : 0.0f) + "\"");
            if (bHit)
                *Out = Found->second;
            return bHit ? 1 : 0;
        }
        unsigned long NEURONCOMPONENTS_API GetNamedDurationOut(void* Script, const char* Name, float* Out) override
        {
            G.Event(Format("\"ev\":\"named\",\"name\":\"%lx\",\"extra\":\"%lx\",\"r\":\"%d\",\"v\":\"", Id(Script), Id(Name),
                           G.bNamed ? 1 : 0) +
                    F(G.NamedValue) + "\"");
            if (G.bNamed)
                *Out = G.NamedValue;
            return G.bNamed ? 1 : 0;
        }
        unsigned long NEURONCOMPONENTS_API FindObject(unsigned long Handle, void* Query) override
        {
            const unsigned long* Q = (const unsigned long*)Query;
            const unsigned long R = (unsigned long)Handle * 0x100 + (unsigned long)Q[3] * 2 + Q[0];
            G.Event(Format("\"ev\":\"findobj\",\"h\":\"%ld\",\"kind\":\"%lu\",\"inst\":\"%ld\",\"r\":\"%lx\"", (long)Handle, Q[0],
                           (long)Q[3], R));
            return R;
        }
        unsigned long NEURONCOMPONENTS_API RegisterHandle(unsigned long Key) override
        {
            G.Event(Format("\"ev\":\"reghandle\",\"k\":\"%ld\"", (long)Key));
            return 0;
        }
        unsigned long NEURONCOMPONENTS_API HasHandle(unsigned long Key, unsigned char* Out) override
        {
            *Out = G.Blocked.count((long)Key) != 0 ? 1 : 0;
            G.Event(Format("\"ev\":\"hashandle\",\"k\":\"%ld\",\"r\":\"%u\"", (long)Key, *Out));
            return 0;
        }
        unsigned long NEURONCOMPONENTS_API HasAnyHandle(unsigned long Key) override
        {
            G.Event(Format("\"ev\":\"hasany\",\"k\":\"%ld\"", (long)Key));
            return 0;
        }
        // The slots Deformable2 does not call.
        void* NEURONCOMPONENTS_API OpenScript(void* FileSystem) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API CloseScript(void* Script) override { return GR_GENERIC; }
        float NEURONCOMPONENTS_API GetScriptDuration(void* PlaybackId) override { return 0; }
        float NEURONCOMPONENTS_API GetNamedDuration(void* Script, const char* Name) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API EnumScriptNames(void* Callback, void* Script, void* Context) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API FindScriptName(void* Callback, void* Script, const char* Name, void* Context) override { return GR_GENERIC; }
        unsigned long NEURONCOMPONENTS_API EvalStored(unsigned long PlaybackId) override { return 0; }
        unsigned long NEURONCOMPONENTS_API ApplyChannels(unsigned long PlaybackId) override { return 0; }
        unsigned long NEURONCOMPONENTS_API StopPlayback(unsigned long PlaybackId) override { return 0; }
        unsigned long NEURONCOMPONENTS_API StopChannels(unsigned long PlaybackId, void* Unused) override { return 0; }
        float NEURONCOMPONENTS_API GetStoredDuration(unsigned long PlaybackId) override { return 0; }
        unsigned long NEURONCOMPONENTS_API MixerWalk(unsigned long PlaybackId, float Time) override { return 0; }
        unsigned long NEURONCOMPONENTS_API MixerWalkAll(float Time) override { return 0; }
        void* NEURONCOMPONENTS_API CreateEmptyScript() override { return nullptr; }
        unsigned long NEURONCOMPONENTS_API AddScriptNames(void* Script, void* FileSystem, const char** Names, int Count) override { return 0; }
        unsigned long NEURONCOMPONENTS_API RemoveScriptNames(void* Script, const char** Names, int Count) override { return 0; }
        unsigned long NEURONCOMPONENTS_API EnumFsScripts(void* Callback, void* FileSystem, void* Context) override { return 0; }
        unsigned long NEURONCOMPONENTS_API HasScriptName(void* Script, const char* Name) override { return 0; }
        unsigned long NEURONCOMPONENTS_API GetScriptNameCount(void* Script, unsigned long* Out) override { return 0; }
        unsigned long NEURONCOMPONENTS_API EnumObjects(void* Callback, unsigned long PlaybackId, void* Context) override { return 0; }
    };

    // --- the renderer -------------------------------------------------------------------------

    class FDefRenderer final : public IRenderer2
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        int NEURONCOMPONENTS_API RenderInstance(void*, void* Inst, float Threshold, unsigned long Flags, void* Matrix,
                                                void* Extra) override
        {
            G.Event(Format("\"ev\":\"rinst\",\"inst\":\"%ld\",\"thr\":\"", Id(Inst)) + F(Threshold) +
                    Format("\",\"flags\":\"%lx\",\"m\":\"%d\",\"arg\":\"%lx\",\"r\":\"%d\"", Flags, Matrix == nullptr ? 0 : 1,
                           Id(Extra), G.RenderRank));
            return G.RenderRank;
        }
        // The slots Deformable2 does not call.
        int NEURONCOMPONENTS_API QueryGeometry(void* A, float Lod, void* C, void* D, void* E, void* F, void* G) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetBounds(void* Id, float Lod, void* Out) override { return GR_GENERIC; }
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

    class FDefVMeshPart final : public IVMeshPart
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        bool NEURONCOMPONENTS_API Draw(ICamera*, void* Inst, unsigned long, const void*, IMaterial*) override
        {
            G.Event(Format("\"ev\":\"vmdraw\",\"part\":\"1\",\"inst\":\"%ld\"", Id(Inst)));
            return true;
        }
        // The slots Deformable2 does not call.
        bool NEURONCOMPONENTS_API Prepare(void* Listing, void* Scratch) override { return false; }
        FVmeshRef* NEURONCOMPONENTS_API GetRef() override { return nullptr; }
        FVmeshDataHeader* NEURONCOMPONENTS_API GetHeader() override { return nullptr; }
    };

    FDefVMeshPart GVMeshPart;

    class FDefPipeline final : public IRenderPipeline
    {
    public:
        std::map<int, int> States;
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API GetPipelineState(int Id, int* Out) override
        {
            if (!G.bStateSet)
            {
                G.Event(Format("\"ev\":\"pstate\",\"id\":\"%d\",\"r\":\"-\"", Id));
                return GR_GENERIC;
            }
            G.Event(Format("\"ev\":\"pstate\",\"id\":\"%d\",\"r\":\"%d\"", Id, G.StateValue));
            *Out = G.StateValue;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetModelview(const float* M) override
        {
            G.Event("\"ev\":\"modelview\",\"m\":\"" + Fs(M, 12) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetWorld(const float* M) override
        {
            G.Event("\"ev\":\"world\",\"m\":\"" + Fs(M, 12) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetWorldN(int WorldIndex, const float* M12) override
        {
            G.Event(Format("\"ev\":\"worldn\",\"i\":\"%d\",\"m\":\"", WorldIndex) + Fs(M12, 12) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetRenderState(D3DRENDERSTATETYPE State, int* Out) override
        {
            const int V = States[(int)State];
            G.Event(Format("\"ev\":\"getrs\",\"s\":\"%d\",\"v\":\"%d\"", (int)State, V));
            *Out = V;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetRenderState(D3DRENDERSTATETYPE State, int Value) override
        {
            G.Event(Format("\"ev\":\"setrs\",\"s\":\"%d\",\"v\":\"%d\"", (int)State, Value));
            States[(int)State] = Value;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetMaterial(const D3DMATERIAL8* Material) override
        {
            G.Event("\"ev\":\"setmat\",\"v\":\"" + Fs((const float*)Material, 17) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageState(int Stage, int Type, int Value) override
        {
            G.Event(Format("\"ev\":\"tss\",\"st\":\"%d\",\"s\":\"%d\",\"v\":\"%d\"", Stage, Type, Value));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageTexture(int Stage, void* Handle) override
        {
            G.Event(Format("\"ev\":\"tex\",\"st\":\"%d\",\"h\":\"%lx\"", Stage, (unsigned long)Id(Handle)));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageTransform(int Stage, const float* Matrix16) override
        {
            G.Event(Format("\"ev\":\"ttrans\",\"st\":\"%d\",\"m\":\"", Stage) + Fs(Matrix16, 16) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetTextureFormat(void* Handle, int* OutFormat) override
        {
            std::map<long, int>::iterator Found = G.TextureFormats.find(Id(Handle));
            const bool bHit = Found != G.TextureFormats.end();
            G.Event(Format("\"ev\":\"texfmt\",\"h\":\"%lx\",\"r\":\"%d\",\"v\":\"%d\"", (unsigned long)Id(Handle), bHit ? 0 : -1,
                           bHit ? Found->second : 0));
            if (!bHit)
                return GR_GENERIC;
            *OutFormat = Found->second;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API VerifyState() override
        {
            G.Event("\"ev\":\"verify\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitiveVb(unsigned long Prim, void* Vb, unsigned int BaseVertex,
                                                              unsigned int NumVerts, const void* Indices,
                                                              unsigned int IndexCount, unsigned long) override
        {
            G.Event(Format("\"ev\":\"dipvb\",\"prim\":\"%lu\",\"vb\":\"%lx\",\"base\":\"%u\",\"n\":\"%u\",\"span\":\"%u\"", Prim,
                           (unsigned long)Id(Vb), BaseVertex, NumVerts, IndexCount));
            return GR_OK;
        }
        // The slots Deformable2 does not call.
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

    FDefPipeline GPipeline;

    class FDefVbMgr final : public IVertexBufferManager
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API LockVertexMemory(unsigned int Fvf, unsigned int Count, FVertexMemoryLock*) override
        {
            G.Event(Format("\"ev\":\"lockvm\",\"fvf\":\"%x\",\"n\":\"%u\",\"r\":\"-1\"", Fvf, Count));
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API UnlockVertexMemory(FVertexMemoryLock*) override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API CopyVertexData(void*, unsigned int, const FRpVertexSrcDesc*, unsigned int, unsigned int) override
        {
            return GR_OK;
        }
        // The slots Deformable2 does not call.
        GENRESULT NEURONCOMPONENTS_API VbMgrInit(void* A, void* B, void* C, void* D) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API VbMgrReleaseAll() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UnsupportedManagerOperation() override { return GR_GENERIC; }
    };

    class FDefDraw final : public IRPDraw
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
            G.Event(Format("\"ev\":\"dip\",\"prim\":\"%lu\",\"min\":\"%u\",\"nv\":\"%u\",\"start\":\"%u\",\"n\":\"%u\",\"r\":\"%d\"", Prim, Min,
                           NumVertices, Start, Count, G.DrawResult));
            return G.DrawResult;
        }

    };

    class FDefIb final : public IRPIndexBuffer
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API IsIbValid(void* Handle) override
        {
            const int R = G.LiveIb.count((unsigned long)Id(Handle)) != 0 ? 1 : 0;
            G.Event(Format("\"ev\":\"isib\",\"ib\":\"%lx\",\"r\":\"%d\"", (unsigned long)Id(Handle), R));
            return R;
        }
        GENRESULT NEURONCOMPONENTS_API SelectIb(void* Handle, unsigned int Base, unsigned int, unsigned int) override
        {
            const int R = G.LiveIb.count((unsigned long)Id(Handle)) != 0 ? 0 : -1;
            G.Event(Format("\"ev\":\"selib\",\"ib\":\"%lx\",\"base\":\"%u\",\"r\":\"%d\"", (unsigned long)Id(Handle), Base, R));
            return R;
        }
        GENRESULT NEURONCOMPONENTS_API CreateIb(unsigned int Count, unsigned int** Out, unsigned int) override
        {
            const unsigned long Handle = G.NextIb++;
            G.LiveIb.insert(Handle);
            G.IbData[Handle].assign(Count, 0);
            G.Event(Format("\"ev\":\"createib\",\"n\":\"%u\",\"ib\":\"%lx\"", Count, Handle));
            *Out = (unsigned int*)(uintptr_t)Handle;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API DestroyIb(void** Handle) override
        {
            const unsigned long H = (unsigned long)Id(*Handle);
            G.Event(Format("\"ev\":\"destroyib\",\"ib\":\"%lx\"", H));
            G.LiveIb.erase(H);
            *Handle = nullptr;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API LockIb(void* Handle, unsigned int* Offset, void** Out, unsigned int Count) override
        {
            const unsigned long H = (unsigned long)Id(Handle);
            G.Event(Format("\"ev\":\"lockib\",\"ib\":\"%lx\",\"n\":\"%u\"", H, Count));
            std::vector<unsigned short>& Data = G.IbData[H];
            if (Data.size() < Count)
                Data.resize(Count, 0);
            *Offset = 0;
            *Out = Data.data();
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API UnlockIb(void* Handle) override
        {
            const unsigned long H = (unsigned long)Id(Handle);
            const std::vector<unsigned short>& Data = G.IbData[H];
            std::string Text;
            for (size_t Index = 0; Index < Data.size(); ++Index)
                Text += Format(Index ? ",%u" : "%u", Data[Index]);
            G.Event(Format("\"ev\":\"unlockib\",\"ib\":\"%lx\",\"data\":\"", H) + Text + "\"");
            return GR_OK;
        }
        // The slots Deformable2 does not call.
        GENRESULT NEURONCOMPONENTS_API ResizeIb(void* Handle, unsigned int Count) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CopyIndices(void* Handle, unsigned int* OutOffset, const unsigned short* SrcData, unsigned int Count) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetIbCapacity(void* Handle, unsigned int* OutCapacity) override { return GR_GENERIC; }
    };

    class FDefVb final : public IRPVertexBuffer
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API IsVbValid(void* Handle) override
        {
            const int R = G.LiveVb.count((unsigned long)Id(Handle)) != 0 ? 1 : 0;
            G.Event(Format("\"ev\":\"isvb\",\"vb\":\"%lx\",\"r\":\"%d\"", (unsigned long)Id(Handle), R));
            return R;
        }
        GENRESULT NEURONCOMPONENTS_API SelectVb(void* Handle) override
        {
            const int R = G.LiveVb.count((unsigned long)Id(Handle)) != 0 ? 0 : -1;
            G.Event(Format("\"ev\":\"selvb\",\"vb\":\"%lx\",\"r\":\"%d\"", (unsigned long)Id(Handle), R));
            return R;
        }
        GENRESULT NEURONCOMPONENTS_API CreateVb(unsigned int Fvf, unsigned int Count, unsigned int** Out, unsigned int) override
        {
            const unsigned long Handle = G.NextVb++;
            G.LiveVb.insert(Handle);
            G.VbData[Handle].assign((size_t)Count * ComputeFvfStride(Fvf), 0);
            G.Event(Format("\"ev\":\"createvb\",\"fvf\":\"%x\",\"n\":\"%u\",\"vb\":\"%lx\"", Fvf, Count, Handle));
            *Out = (unsigned int*)(uintptr_t)Handle;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API DestroyVb(void** Handle) override
        {
            const unsigned long H = (unsigned long)Id(*Handle);
            G.Event(Format("\"ev\":\"destroyvb\",\"vb\":\"%lx\"", H));
            G.LiveVb.erase(H);
            *Handle = nullptr;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API LockVb(void* Handle, unsigned int* Offset, void** Out, unsigned int Count) override
        {
            const unsigned long H = (unsigned long)Id(Handle);
            G.Event(Format("\"ev\":\"lockvb\",\"vb\":\"%lx\",\"n\":\"%u\"", H, Count));
            std::vector<unsigned char>& Data = G.VbData[H];
            if (Data.size() < (size_t)Count * 0x20)
                Data.resize((size_t)Count * 0x20, 0);
            *Offset = 0;
            *Out = Data.data();
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API CopyVertices(void* Handle, unsigned int* Base, const FRpVertexSrcDesc* Src,
                                                    unsigned int Start, unsigned int Count) override
        {
            G.Event(Format("\"ev\":\"copyv\",\"vb\":\"%lx\",\"base\":\"%u\",\"start\":\"%u\",\"n\":\"%u\",\"src\":\"",
                           (unsigned long)Id(Handle), *Base, Start, Count) +
                    SourceText(Src) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API UnlockVb(void* Handle) override
        {
            const unsigned long H = (unsigned long)Id(Handle);
            const std::vector<unsigned char>& Data = G.VbData[H];
            G.Event(Format("\"ev\":\"unlockvb\",\"vb\":\"%lx\",\"data\":\"", H) + Hex(Data.data(), Data.size()) + "\"");
            return GR_OK;
        }
        // The slots Deformable2 does not call.
        GENRESULT NEURONCOMPONENTS_API ResizeVb(void* Handle, unsigned int Fvf, unsigned int Count) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UnsupportedVertexBufferOperation(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetVbInfo(void* Handle, unsigned int* OutFvf, unsigned int* OutCount) override { return GR_GENERIC; }
    };

    class FDefTextures final : public ITextureLibrary2
    {
    public:
        std::vector<std::string> Records;
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API FindTextureByCrc(void* Query, void** OutTex) override
        {
            const FEngineName* Q = (const FEngineName*)Query;
            const bool bHit = G.Textures.count(Q->Text) != 0;
            long Record = 0;
            if (bHit)
            {
                Records.push_back(Q->Text);
                Record = (long)Records.size();
            }
            G.Event(Format("\"ev\":\"findtex\",\"name\":\"%s\",\"crc\":\"%08lx\",\"r\":\"%ld\"", Q->Text, Q->Crc, Record));
            if (!bHit)
                return GR_GENERIC;
            *OutTex = Ptr(Record);
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetFrameCount(void* Record, int* Out) override
        {
            const int N = G.Textures[Records[Id(Record) - 1]].first;
            G.Event(Format("\"ev\":\"fcount\",\"rec\":\"%ld\",\"v\":\"%d\"", Id(Record), N));
            *Out = N;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API CreatePlaybackGroup(void* Record, int* OutId) override
        {
            const int Made = G.NextPlayback++;
            G.Event(Format("\"ev\":\"cplayback\",\"rec\":\"%ld\",\"id\":\"%d\"", Id(Record), Made));
            *OutId = Made;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API ReleaseTextureRecord(void* Record) override
        {
            G.Event(Format("\"ev\":\"reltex\",\"rec\":\"%ld\"", Id(Record)));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API DecRefById(int Id) override
        {
            G.Event(Format("\"ev\":\"decref\",\"id\":\"%d\"", Id));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetFrame(int Id, int Frame, unsigned long* Out) override
        {
            unsigned long V = 0;
            if (Id >= 0x40 && (size_t)(Id - 0x40) < Records.size())
                V = G.Textures[Records[Id - 0x40]].second;
            G.Event(Format("\"ev\":\"getframe\",\"id\":\"%d\",\"frame\":\"%d\",\"v\":\"%lx\"", Id, Frame, V));
            *Out = V;
            return GR_OK;
        }
        // The slots Deformable2 does not call.
        GENRESULT NEURONCOMPONENTS_API SetGenericSlot(int Index, unsigned long Value) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetGenericSlot(int Index, unsigned long* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LoadTextureGroup(void* FileSystem, int* OutGroupId) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LoadTextureLibrary(void* FileSystem, const char* Name) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GarbageCollect(int GroupId) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API TickAllPlayback(int Unused, float DeltaSeconds) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetGroupName(int Id, const char* Name) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetGroupName(int Id, char* Buf, unsigned long Size) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API FindOrCreateByName(const char* Name, void** OutTex) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API AddRefById(int Id) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CopyName(void* Node, char* Buf, unsigned long Size) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetRecordName(void* Record, const char* Name) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetSubTexture(void* Record, int Index, int TextureId) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetFrameHandle(void* Record, int Index, unsigned long Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetFrameRect(void* Record, int Index, float U0, float V0, float U1, float V1) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureFrame(void* Tex, int Frame, unsigned long* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetGroupPixelFormat(void* Record, void* Unused, int* OutFormat) override { return GR_GENERIC; }
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
        GENRESULT NEURONCOMPONENTS_API EnumTexture(int Id, int Index, void** OutRecord) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API AddSubTexture(int GroupId, void* Record) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API RemoveSubTexture(int GroupId, void* Record) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API PickGpuFormat(const unsigned long* Src, unsigned long* OutId) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API AllocateNewTextureId(int* OutId) override { return GR_GENERIC; }
    };

    class FDefCamera final : public ICamera
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        int NEURONCOMPONENTS_API ClassifySphere(float const* C, float R) override
        {
            G.Event("\"ev\":\"classify\",\"c\":\"" + Fs(C, 3) + "\",\"r\":\"" + F(R) +
                    Format("\",\"res\":\"%d\"", G.ClassifyResult));
            return G.ClassifyResult;
        }
        float const* NEURONCOMPONENTS_API GetViewTransform() override
        {
            G.Event("\"ev\":\"camview\"");
            return G.View;
        }
        // The slots Deformable2 does not call.
        float const* NEURONCOMPONENTS_API GetPosition() override { return nullptr; }
        float const* NEURONCOMPONENTS_API GetOrientation() override { return nullptr; }
        int const* NEURONCOMPONENTS_API GetViewport() override { return nullptr; }
        float NEURONCOMPONENTS_API GetHalfFovXDeg() override { return 0; }
        float NEURONCOMPONENTS_API GetFovYDeg() override { return 0; }
        float NEURONCOMPONENTS_API GetZNear() override { return 0; }
        float NEURONCOMPONENTS_API GetZFar() override { return 0; }
        float NEURONCOMPONENTS_API GetAspect() override { return 0; }
        double NEURONCOMPONENTS_API GetXScaleOverNear() override { return 0; }
        double NEURONCOMPONENTS_API GetYScaleOverNear() override { return 0; }
        int NEURONCOMPONENTS_API Project(float* OutX, float* OutY, float* OutZ, float const* World) override { return GR_GENERIC; }
        void NEURONCOMPONENTS_API Unproject(float* OutWorld3, float ScreenX, float ScreenY) override { }
        int NEURONCOMPONENTS_API ProjectVec(float* Out3, float const* World) override { return GR_GENERIC; }
        void GetVelocity(float* Out3) override { }
        int SetProperty(char const* Key, char const* Value) override { return GR_GENERIC; }
    };

    FDefEngine GEngineFake;
    FDefAnimation GAnimation;
    FDefRenderer GRenderer;
    FDefVb GVb;
    FDefIb GIb;
    FDefDraw GDraw;
    FDefVbMgr GVbMgr;
    FDefTextures GTextures;
    FDefCamera GCamera;

    // The outer an EyeMaterial initializes against.
    class FDefSystem final : public INeuronComponent
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            if (strcmp(Iid, Iids::TextureLibraryA) == 0)
                *Out = &GTextures;
            else if (strcmp(Iid, Iids::RenderPipeline8B) == 0)
                *Out = &GPipeline;
            else if (strcmp(Iid, Iids::RPDraw) == 0)
                *Out = &GDraw;
            else if (strcmp(Iid, Iids::VertexBufferManager) == 0)
                *Out = &GVbMgr;
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

    FDefSystem GSystem;

    // The caller's channel sink an instance forwards OnKeys and OnLoop to.
    class FDefRecordingSink final : public IChannelSink
    {
    public:
        void NEURONCOMPONENTS_API OnKeys(unsigned long Cookie, void*, FChannelKeyBatch*) override
        {
            G.Forwarded = 1;
            G.Cookie = Cookie;
        }
        void NEURONCOMPONENTS_API OnOpaqueNotification(void*, void*) override {}
        void NEURONCOMPONENTS_API OnLoop(unsigned long, float*, void*) override { G.Forwarded = 1; }
    };

    FDefRecordingSink GSink;

    // The slot-36 walk's user callback.
    void __cdecl HardpointRecord(void* BoneInst, char const* Name, void*)
    {
        G.Event(Format("\"ev\":\"hpvisit\",\"inst\":\"%ld\",\"name\":\"%s\"", Id(BoneInst), Name));
    }

    // The component whose IDeformable slots the operations call.
    FDeformable* GComponent = nullptr;

    // --- FDUMP ---------------------------------------------------------------------------------

    void __cdecl DefDump(int Flags, const char* Pattern, ...)
    {
        va_list Args;
        va_start(Args, Pattern);
        const char* File = va_arg(Args, const char*);
        const int Line = va_arg(Args, int);
        const char* Message = va_arg(Args, const char*);
        va_end(Args);
        G.Event(Format("\"ev\":\"dump\",\"flags\":\"%x\",\"file\":\"%s\",\"line\":\"%x\",\"msg\":\"", Flags, File, Line) +
                Hex(Message, strlen(Message)) + "\"");
    }

    // --- the operations -------------------------------------------------------------------------

    struct FMeshBuild
    {
        std::string Name;
        int Flag70 = 1;
        std::vector<float> Points;   // six per vertex
        std::vector<long> PointIndices;
        std::vector<float> Uvs;
        std::vector<long> UvIndices;
        bool bUvIndices = false;
        std::vector<std::pair<int, std::vector<std::pair<long, float>>>> Skin;
        std::vector<std::pair<std::string, std::vector<long>>> Groups;
        std::vector<std::vector<float>> UvDeform;
    };

    void Op(const std::string& Body) { G.Event(Body); }

    std::string QuatText(const float* Q) { return Fs(Q, 4); }

    struct FDefCase;
    // oracle_deformable_parts.cpp: the parts suite's operations; false for an unknown one.
    bool RunPartsOp(FDefCase& Case, const std::vector<std::string>& T);

    struct FDefCase
    {
        FMeshBuild Build;
        std::vector<float> Bones;

        FMeshArchetype* MakeMesh()
        {
            FMeshArchetype* Mesh = new FMeshArchetype(Build.Name.c_str());
            const int Count = (int)Build.Groups.size();
            int* Block = (int*)new unsigned char[(size_t)Count * sizeof(FFaceGroup) + 4];
            *Block = Count;
            Mesh->FaceGroups = (FFaceGroup*)(Block + 1);
            for (int Index = 0; Index < Count; ++Index)
            {
                FFaceGroup* Group = &Mesh->FaceGroups[Index];
                ConstructFaceGroup(Group);
                const std::string& Name = Build.Groups[Index].first;
                if (Name != "-")
                {
                    Group->MaterialCrc = NEURONCOMPONENTS_CRC::GetCRC32(Name.c_str());
                    std::map<std::string, IMaterial*>::iterator Found = G.Materials.find(Name);
                    if (Found != G.Materials.end())
                    {
                        Group->Material = Found->second;
                        const std::vector<long>& Indices = Build.Groups[Index].second;
                        Group->Indices = (unsigned short*)new unsigned char[Indices.size() * 2 + 2];
                        for (size_t K = 0; K < Indices.size(); ++K)
                            Group->Indices[K] = (unsigned short)Indices[K];
                        Group->IndexCount = (unsigned long)Indices.size();
                        Group->Kind = 4;
                        RebaseFaceIndices(Group);
                    }
                }
                Mesh->FaceGroupCount += 1;
            }
            Mesh->PointIndexCount = (int)Build.PointIndices.size();
            Mesh->PointIndices = (unsigned long*)new unsigned char[Build.PointIndices.size() * 4 + 4];
            for (size_t K = 0; K < Build.PointIndices.size(); ++K)
                Mesh->PointIndices[K] = (unsigned long)Build.PointIndices[K];
            Mesh->VertexCount = (int)(Build.Points.size() / 6);
            Mesh->Vertices = new unsigned char[Build.Points.size() * 4 + 4];
            memcpy(Mesh->Vertices, Build.Points.data(), Build.Points.size() * 4);
            if (!Build.Uvs.empty())
            {
                Mesh->UvCount = (int)(Build.Uvs.size() / 2);
                Mesh->UvData = new unsigned char[Build.Uvs.size() * 4];
                memcpy(Mesh->UvData, Build.Uvs.data(), Build.Uvs.size() * 4);
            }
            if (Build.bUvIndices)
            {
                Mesh->UvIndices = (unsigned long*)new unsigned char[Build.UvIndices.size() * 4 + 4];
                for (size_t K = 0; K < Build.UvIndices.size(); ++K)
                    Mesh->UvIndices[K] = (unsigned long)Build.UvIndices[K];
            }
            size_t Pairs = 0;
            for (size_t K = 0; K < Build.Skin.size(); ++K)
                Pairs += Build.Skin[K].second.size();
            Mesh->SkinPairs = (FSkinPair*)new unsigned char[Pairs * sizeof(FSkinPair) + 8];
            Mesh->SkinTable = (FSkinVertex*)new unsigned char[Build.Skin.size() * sizeof(FSkinVertex) + 8];
            size_t At = 0;
            for (size_t K = 0; K < Build.Skin.size(); ++K)
            {
                Mesh->SkinTable[K].BoneCount = Build.Skin[K].first;
                Mesh->SkinTable[K].Pairs = Mesh->SkinPairs + At;
                for (const std::pair<long, float>& Pair : Build.Skin[K].second)
                {
                    Mesh->SkinPairs[At].BoneId = (int)Pair.first;
                    Mesh->SkinPairs[At].Weight = Pair.second;
                    ++At;
                }
            }
            if (!Build.UvDeform.empty())
            {
                Mesh->UvDeformCount = (int)Build.UvDeform.size();
                Mesh->UvDeform = (FUvDeformRecord*)new unsigned char[Build.UvDeform.size() * sizeof(FUvDeformRecord)];
                for (size_t K = 0; K < Build.UvDeform.size(); ++K)
                {
                    const std::vector<float>& R = Build.UvDeform[K];
                    FUvDeformRecord* Out = &Mesh->UvDeform[K];
                    Out->BoneId = (int)R[0];
                    Out->VertexCount = (int)R[1];
                    Out->PlaneDistance = R[2];
                    Out->BoneXToU = R[3];
                    Out->BoneYToV = R[4];
                    Out->MinDu = R[5];
                    Out->MaxDu = R[6];
                    Out->MinDv = R[7];
                    Out->MaxDv = R[8];
                }
            }
            BindUvBoneIdsToFaceGroups(Mesh);
            BuildVertexRemap(Mesh);
            BuildBoneVertexLists(Mesh, G.Holder, nullptr);
            Mesh->FaceFlagCount = Mesh->VertexCount;
            Mesh->FaceFlags = new unsigned char[(size_t)Mesh->FaceFlagCount + 1];
            memset(Mesh->FaceFlags, 0, (size_t)Mesh->FaceFlagCount + 1);
            Mesh->ParseTag = G_ParseTagUtf;
            Mesh->Flag70 = (unsigned char)Build.Flag70;
            return Mesh;
        }

        FDeformPart* PartAt(const std::string& Text) { return G.Parts[(size_t)Dec(Text)]; }

        FBoneRuntime* BoneAt(const std::string& Text) { return &G.Holder->BoneSlots[Dec(Text)]; }

        std::string BoneText(FBoneRuntime* Bone)
        {
            return Format("\"inst\":\"%ld\",\"flags\":\"%x\",\"local\":\"", Id(Bone->Instance), Bone->Flags) +
                   Fs(Bone->LocalTransform, 12) + "\",\"matrix\":\"" + Fs(Bone->Matrix, 12) + "\"";
        }

        void Run(const std::vector<std::string>& T)
        {
            const std::string& K = T[0];
            if (K == "bones")
            {
                Bones = HexFloats(T[1]);
                Op(Format("\"op\":\"bones\",\"n\":\"%u\"", (unsigned int)(Bones.size() / 12)));
            }
            else if (K == "link")
            {
                G.LinkRoot = Dec(T[1]);
                G.Links.clear();
                if (T.size() > 2 && T[2] != "-")
                    for (const std::string& Pair : Words(T[2], ","))
                    {
                        const std::vector<std::string> P = Words(Pair, ":");
                        G.Links.push_back(std::make_pair(Dec(P[0]), Dec(P[1])));
                    }
            }
            else if (K == "matrix")
                G.Matrix[Dec(T[1])] = HexFloats(T[2]);
            else if (K == "holder")
            {
                FSkeleton* Skel = ConstructSkeleton();
                const int Count = (int)(Bones.size() / 12);
                for (int Index = 0; Index < Count; ++Index)
                {
                    Skel->Bones[Index].Parent = 0x100 + Index;
                    memcpy(Skel->Bones[Index].BoneToRoot, &Bones[(size_t)Index * 12], 48);
                }
                Skel->BoneCount = Count;
                Skel->EngineHandle = Ptr(G.SkelHandle);
                FLodHolder* Holder = new FLodHolder;
                Holder->Skeleton = nullptr;
                Holder->BoneSlots = nullptr;
                Holder->BindCursor = 0;
                Holder->RefCount = 1;
                G.Holder = Holder;
                const unsigned char R = Holder->AttachSkeleton(Skel);
                ReleaseSkeleton(Skel);
                std::string Slots;
                for (int Index = 0; Index < Count; ++Index)
                    Slots += Format(Index ? ",%ld:%x" : "%ld:%x", Id(Holder->BoneSlots[Index].Instance), Holder->BoneSlots[Index].Flags);
                Op(Format("\"op\":\"holder\",\"r\":\"%u\",\"bound\":\"%d\",\"slots\":\"", R, Holder->BindCursor) + Slots + "\"");
            }
            else if (K == "mat")
            {
                FDefMat* Mat = new FDefMat();
                Mat->Name = T[1];
                Mat->Class = T[2];
                G.Materials[T[1]] = Mat;
                G.Fakes[T[1]] = Mat;
            }
            else if (K == "eyemat")
            {
                FEyeMaterial* Eye = new FEyeMaterial();
                Eye->Controlling = &Eye->Inner;
                const GENRESULT R = Eye->Initialize(&GSystem);
                G.Materials[T[1]] = Eye;
                Op(Format("\"op\":\"eyemat\",\"r\":\"%d\"", R) + ",\"v\":\"" + Fs(&Eye->Colors.Diffuse[0], 17) +
                   Format("\",\"tr\":\"%u\"", Eye->bTranslucent));
            }
            else if (K == "mesh")
            {
                Build = FMeshBuild();
                Build.Name = T[1];
                Build.Flag70 = (int)Dec(T[2]);
            }
            else if (K == "pts")
                Build.Points = HexFloats(T[1]);
            else if (K == "pidx")
                Build.PointIndices = Decs(T[1]);
            else if (K == "uv")
                Build.Uvs = HexFloats(T[1]);
            else if (K == "uvidx")
            {
                Build.bUvIndices = true;
                Build.UvIndices = Decs(T[1]);
            }
            else if (K == "skin")
            {
                for (const std::string& Vertex : Words(T[1], ","))
                {
                    const std::vector<std::string> V = Words(Vertex, ":");
                    std::vector<std::pair<long, float>> Pairs;
                    for (size_t I = 1; I < V.size(); ++I)
                    {
                        const std::vector<std::string> P = Words(V[I], "/");
                        Pairs.push_back(std::make_pair(Dec(P[0]), HexFloat(P[1])));
                    }
                    Build.Skin.push_back(std::make_pair((int)Dec(V[0]), Pairs));
                }
            }
            else if (K == "grp")
                Build.Groups.push_back(std::make_pair(T[1], Decs(T.size() > 2 ? T[2] : "-")));
            else if (K == "uvd")
                Build.UvDeform.push_back(HexFloats(T[1]));
            else if (K == "endmesh")
            {
                FMeshArchetype* Mesh = MakeMesh();
                G_MeshCache[Mesh->NameCrc] = Mesh;
                float Radius = -1.0f;
                float Center[3] = {-1, -1, -1};
                Mesh->GetBoundSphere(&Radius, Center);
                std::string Lists;
                for (int B = 0; B < G.Holder->BindCursor; ++B)
                    Lists += Format(B ? ",%d" : "%d", (int)(Mesh->BoneVerts[B].Finish - Mesh->BoneVerts[B].Start));
                std::string Uvb;
                for (int Index = 0; Index < Mesh->FaceGroupCount; ++Index)
                    Uvb += Format(Index ? ",%ld" : "%ld", (long)Mesh->FaceGroups[Index].UvBoneIndex);
                Op(Format("\"op\":\"mesh\",\"name\":\"%s\",\"ic\":\"%d\",\"vc\":\"%d\",\"lists\":\"", Build.Name.c_str(),
                          Mesh->GetIndexCount(), Mesh->GetVertexCapacity()) +
                   Lists + "\",\"uvb\":\"" + Uvb + "\",\"r\":\"" + F(Radius) + "\",\"c\":\"" + Fs(Center, 3) + "\"");
            }
            else if (K == "part")
            {
                std::vector<FDeformMesh*> Meshes;
                for (const std::string& Name : Words(T[1], ","))
                {
                    FDeformMesh* Mesh = G_MeshCache[NEURONCOMPONENTS_CRC::GetCRC32(Name.c_str())];
                    AddRefDeformMesh(Mesh);
                    Meshes.push_back(Mesh);
                }
                FDeformPart* Part = new FDeformPart;
                ConstructPart(Part);
                const unsigned char R = Part->BindMeshes(G.Holder, Meshes.data(), (int)Meshes.size());
                G.Parts.push_back(Part);
                Op(Format("\"op\":\"part\",\"r\":\"%u\",\"vcap\":\"%d\",\"icap\":\"%d\"", R, Part->Vb.Capacity, Part->Ib.Capacity));
            }
            else if (K == "inst")
            {
                const float Saved = G_DeformRate;
                G_DeformRate = 0.0f;
                FDeformInstance* Instance = new FDeformInstance;
                ConstructInstanceBody(Instance);
                G_DeformRate = Saved;
                Instance->UserAt18 = nullptr;
                Instance->UserAt1C = Ptr((long)HexWord(T[1]));
                const int Count = (int)G.Parts.size();
                int* Block = (int*)new unsigned char[(size_t)Count * sizeof(FDeformPart) + 4];
                *Block = Count;
                Instance->PartArray = (FDeformPart*)(Block + 1);
                for (int Index = 0; Index < Count; ++Index)
                {
                    memcpy(&Instance->PartArray[Index], G.Parts[Index], sizeof(FDeformPart));
                    delete G.Parts[Index];
                    G.Parts[Index] = &Instance->PartArray[Index];
                }
                Instance->PartCount = Count;
                G.Instance = Instance;
                Op("\"op\":\"inst\"");
            }
            else if (K == "sink")
                G.Instance->UserAt18 = &GSink;
            else if (K == "bone")
            {
                FBoneRuntime* Bone = BoneAt(T[1]);
                const std::vector<float> V = HexFloats(T[3]);
                if (T[2] == "mat")
                    Bone->SetMatrix(Bone->Instance, V.data());
                else if (T[2] == "pos")
                    Bone->SetPosition(Bone->Instance, V.data());
                else if (T[2] == "ori")
                    Bone->SetOrientation(Bone->Instance, V.data());
                Op("\"op\":\"bone\"," + BoneText(Bone));
            }
            else if (K == "dirty")
            {
                FBoneRuntime* Bone = BoneAt(T[1]);
                const unsigned long R = Bone->MarkWorldDirty(Bone->Instance);
                Op(Format("\"op\":\"dirty\",\"r\":\"%lu\",", R) + BoneText(Bone));
            }
            else if (K == "showbone")
                Op("\"op\":\"showbone\"," + BoneText(BoneAt(T[1])));
            else if (K == "lod")
            {
                FDeformPart* Part = PartAt(T[1]);
                const bool R = SelectPartLod(Part, (int)Dec(T[2]));
                Op(Format("\"op\":\"lod\",\"r\":\"%d\",\"flags\":\"%x\",\"idx\":\"%d\"", R ? 1 : 0, Part->Flags, Part->LodIndex));
            }
            else if (K == "setlod")
            {
                G_DeformRate = HexFloat(T[3]);
                std::vector<int> Lods;
                for (long L : Decs(T[2]))
                    Lods.push_back((int)L);
                const GENRESULT R = GComponent->SetLod(G.Instance, HexFloat(T[1]), Lods.data());
                std::string Flags;
                for (int Index = 0; Index < G.Instance->PartCount; ++Index)
                    Flags += Format(Index ? ",%x" : "%x", G.Instance->PartArray[Index].Flags);
                Op(Format("\"op\":\"setlod\",\"r\":\"%d\",\"cookie\":\"", R) + F(G.Instance->RateCookie) + "\",\"flags\":\"" + Flags + "\"");
            }
            else if (K == "drawpart")
            {
                const unsigned char R = DrawPartLive(PartAt(T[1]), &GCamera, nullptr);
                Op(Format("\"op\":\"drawpart\",\"r\":\"%u\",\"flags\":\"%x\"", R, PartAt(T[1])->Flags));
            }
            else if (K == "draw")
            {
                int Lod = -9;
                DrawInstanceParts(G.Instance, nullptr, &GCamera, &Lod, Ptr((long)HexWord(T[1])), HexWord(T[2]));
                Op(Format("\"op\":\"draw\",\"lod\":\"%d\"", Lod));
            }
            else if (K == "sphere")
            {
                float Radius = -1.0f;
                float Center[3] = {-1, -1, -1};
                const GENRESULT R = GComponent->GetBoundSphere(G.Instance, &Radius, Center);
                Op(Format("\"op\":\"sphere\",\"r\":\"%d\",\"rad\":\"", R) + F(Radius) + "\",\"c\":\"" + Fs(Center, 3) + "\"");
            }
            else if (K == "stats")
            {
                unsigned int Out[9];
                const GENRESULT R = GComponent->GetMeshStats(G.Instance, Out);
                std::string V;
                for (int Index = 0; Index < 9; ++Index)
                    V += Format(Index ? ",%u" : "%u", Out[Index]);
                Op(Format("\"op\":\"stats\",\"r\":\"%d\",\"v\":\"", R) + V + "\"");
            }
            else if (K == "first")
            {
                int Out = -9;
                GComponent->GetFirstBone(G.Instance, &Out);
                Op(Format("\"op\":\"first\",\"v\":\"%d\"", Out));
            }
            else if (K == "walkhp")
            {
                const unsigned char R = WalkInstanceHardpoints(G.Instance, (void*)&HardpointRecord, nullptr);
                Op(Format("\"op\":\"walkhp\",\"r\":\"%u\"", R));
            }
            else if (K == "connect")
            {
                // ConnectInstanceParts' tail: the hardpoint connections and the unparented bones.
                FHardpointConnectContext Context;
                Context.Hardpoint = nullptr;
                if (G_Engine->QueryInterface(Iids::Hardpoint, (void**)(&Context.Hardpoint)) < 0)
                    FDUMP(0x100002, "%s(%d) : WARNING:General:%s", G_FdumpFileDeformableObject, 0x2B,
                          "Deform: Unable to acquire IID_IHardpoint, unable to continue");
                WalkInstanceHardpoints(G.Instance, (void*)(&HardpointConnectCallback), &Context);
                for (int PartIndex = 0; PartIndex < G.Instance->PartCount; ++PartIndex)
                {
                    FLodHolder* Lod = G.Instance->PartArray[PartIndex].Lod;
                    for (int Bone = 1; Bone < Lod->BindCursor; ++Bone)
                    {
                        FBoneRuntime* Slot = &Lod->BoneSlots[Bone];
                        if (G_Engine->GetInstanceParent(Slot->Instance) == G_InvalidEngineHandle)
                        {
                            G_Engine->DestroyInstance(Slot->Instance);
                            Slot->Instance = G_InvalidEngineHandle;
                        }
                    }
                }
                Op("\"op\":\"connect\"");
            }
            else if (K == "hp")
            {
                std::vector<std::string> Names;
                if (T.size() > 2 && T[2] != "-")
                    Names = Words(T[2], ",");
                G.HardpointNames[Dec(T[1])] = Names;
            }
            else if (K == "nohp")
                G.bHardpoints = false;
            else if (K == "connres")
                G.ConnectResult = HexWord(T[1]);
            else if (K == "parent")
                G.Parent[Dec(T[1])] = Dec(T[2]);
            else if (K == "rel")
            {
                std::vector<unsigned long> Words23(0x17, 0);
                const std::vector<std::string> W = Words(T[2], ",");
                for (size_t I = 0; I < W.size() && I < 0x17; ++I)
                    Words23[I] = HexWord(W[I]);
                G.Relative[Dec(T[1])] = Words23;
            }
            else if (K == "pos")
                G.Position[Dec(T[1])] = HexFloats(T[2]);
            else if (K == "ori")
                G.Orientation[Dec(T[1])] = HexFloats(T[2]);
            else if (K == "jp")
                G.JointParams[Dec(T[1])] = HexFloats(T[2]);
            else if (K == "block")
                G.Blocked.insert(Dec(T[1]));
            else if (K == "cam")
            {
                if (T[1] == "view")
                    memcpy(G.View, HexFloats(T[2]).data(), 48);
                else
                    G.ClassifyResult = (int)Dec(T[2]);
            }
            else if (K == "pstate")
            {
                G.bStateSet = T[1] != "-";
                G.StateValue = G.bStateSet ? (int)Dec(T[1]) : 0;
            }
            else if (K == "rank")
                G.RenderRank = (int)Dec(T[1]);
            else if (K == "aim")
            {
                // root end flags pos orient damping locks
                std::vector<unsigned char> Desc(0x20 + 4 * 8, 0);
                const std::vector<long> Locks = Decs(T[7]);
                const long Key = (long)G.AimDescs.size();
                std::vector<float>& Target = G.AimTargets[Key];
                Target = HexFloats(T[4]);
                const std::vector<float> Orient = HexFloats(T[5]);
                Target.insert(Target.end(), Orient.begin(), Orient.end());
                const long Root = Dec(T[1]);
                const long End = Dec(T[2]);
                const unsigned long Flags = HexWord(T[3]);
                const float Damp = HexFloat(T[6]);
                const int LockCount = (int)Locks.size();
                float* PosPtr = Target.data();
                float* OriPtr = Target.data() + 3;
                memcpy(&Desc[4], &Root, 4);
                memcpy(&Desc[8], &End, 4);
                memcpy(&Desc[0xC], &Flags, 4);
                memcpy(&Desc[0x10], &PosPtr, 4);
                memcpy(&Desc[0x14], &OriPtr, 4);
                memcpy(&Desc[0x18], &Damp, 4);
                memcpy(&Desc[0x1C], &LockCount, 4);
                for (int I = 0; I < LockCount && I < 8; ++I)
                {
                    const long Lock = Locks[I];
                    memcpy(&Desc[0x20 + 4 * I], &Lock, 4);
                }
                G.AimDescs[Key] = Desc;
                void* Node = nullptr;
                const GENRESULT R = GComponent->StartAim(G.Instance, &Node, Desc.data(),
                                                                                  Ptr((long)HexWord(T[8])));
                std::string Detail;
                if (R == GR_OK)
                {
                    FIkEvent* Event = ((FIkListNode*)Node)->Event;
                    std::string Ids, Mats, Lk;
                    for (int I = 0; I < Event->JointCount; ++I)
                    {
                        Ids += Format(I ? ",%d" : "%d", Event->JointIds[I]);
                        Mats += (I ? ";" : "") + Fs(Event->BindMats + I * 9, 9);
                        Lk += Format(I ? ",%u" : "%u", Event->LockBytes[I]);
                    }
                    Detail = Format(",\"n\":\"%d\",\"f34\":\"%x\",\"ids\":\"", Event->JointCount, Event->Flags34 & 3) + Ids +
                             "\",\"bind\":\"" + Mats + "\",\"locks\":\"" + Lk + "\"";
                }
                Op(Format("\"op\":\"aim\",\"r\":\"%d\"", R) + Detail);
                if (R == GR_OK)
                    G.AimNodes.push_back((FIkListNode*)Node);
                else
                    G.AimNodes.push_back(nullptr);
            }
            else if (K == "target")
            {
                std::vector<float>& Target = G.AimTargets[Dec(T[1])];
                const std::vector<float> Pos = HexFloats(T[2]);
                const std::vector<float> Orient = HexFloats(T[3]);
                for (int I = 0; I < 3; ++I)
                    Target[I] = Pos[I];
                for (int I = 0; I < 9; ++I)
                    Target[3 + I] = Orient[I];
            }
            else if (K == "apply")
            {
                // node channel-index skip joint
                FIkListNode* Node = G.AimNodes[(size_t)Dec(T[1])];
                FIkEvent* Event = Node->Event;
                const unsigned long Key = (unsigned long)(intptr_t)Event->Channels[Dec(T[2])];
                FIkApplyGate Gate;
                memset(&Gate, 0, sizeof(Gate));
                Gate.SkipFlag = (int)Dec(T[3]);
                Gate.JointAt0C = (int)Dec(T[4]);
                float Out[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
                const int R = Event->Apply(Out, Key, &Gate, 0.0f);
                std::string Quats;
                for (int I = 0; I < Event->JointCount; ++I)
                    Quats += (I ? ";" : "") + QuatText(Event->Quats + I * 4);
                Op(Format("\"op\":\"apply\",\"r\":\"%d\",\"tick\":\"%d\",\"out\":\"", R, Event->Tick3C) + Fs(Out, 4) +
                   "\",\"quats\":\"" + Quats + "\"");
            }
            else if (K == "stopaim")
            {
                GComponent->StopAim(G.Instance, G.AimNodes[(size_t)Dec(T[1])]);
                Op("\"op\":\"stopaim\"");
            }
            else if (K == "play" || K == "playalt")
            {
                // name extra time d scale f g twist script|joint
                int Out = -9;
                GENRESULT R;
                if (K == "play")
                    R = GComponent->Play(G.Instance, &Out, Ptr((long)HexWord(T[1])), Ptr((long)HexWord(T[2])),
                                                                     Ptr((long)HexWord(T[3])), Ptr((long)HexWord(T[4])), Ptr((long)HexWord(T[5])),
                                                                     Ptr((long)HexWord(T[6])), Ptr((long)HexWord(T[7])), Ptr((long)HexWord(T[8])),
                                                                     Ptr((long)HexWord(T[9])));
                else
                    R = GComponent->PlayAlt(G.Instance, &Out, Ptr((long)HexWord(T[1])), Ptr((long)HexWord(T[2])),
                                                                        Ptr((long)HexWord(T[3])), Ptr((long)HexWord(T[4])), Ptr((long)HexWord(T[5])),
                                                                        Ptr((long)HexWord(T[6])), Ptr((long)HexWord(T[7])), Ptr((long)HexWord(T[8])),
                                                                        Ptr(Dec(T[9])));
                Op(Format("\"op\":\"%s\",\"r\":\"%d\",\"h\":\"%d\",\"o24\":\"", K.c_str(), R, Out) + F(G.Instance->Offset24) + "\"");
            }
            else if (K == "playres")
                G.PlayResults.push_back(Dec(T[1]));
            else if (K == "filter")
                G.Filtered.insert(Dec(T[1]));
            else if (K == "durn")
                G.Durations[Dec(T[1])] = HexFloat(T[2]);
            else if (K == "touch")
                G.Touch[Dec(T[1])] = HexFloats(T[2]);
            else if (K == "rootres")
            {
                G.bRootData = T[1] != "-";
                G.RootResult = (unsigned char)(G.bRootData ? Dec(T[1]) : 0);
                G.RootVec = HexFloats(T.size() > 2 ? T[2] : "-");
            }
            else if (K == "named")
            {
                G.bNamed = T[1] != "-";
                G.NamedValue = G.bNamed ? HexFloat(T[1]) : 0.0f;
            }
            else if (K == "evalres")
                G.EvalResult = Dec(T[1]);
            else if (K == "object")
                G.Objects.insert(std::make_pair(Dec(T[1]), HexWord(T[2])));
            else if (K == "firstf")
                G.FirstField[Dec(T[1])] = HexWord(T[2]);
            else if (K == "mixer")
                G.InMixer.insert(HexWord(T[1]));
            else if (K == "samp")
            {
                FSample& S = G.Samples[HexWord(T[1])];
                S.Alpha = HexFloat(T[2]);
                S.A = HexFloats(T[3]);
                S.B = HexFloats(T[4]);
            }
            else if (K == "height")
            {
                const GENRESULT R = GComponent->SetHeight(G.Instance, HexFloat(T[1]));
                Op(Format("\"op\":\"height\",\"r\":\"%d\",\"o24\":\"", R) + F(G.Instance->Offset24) + "\"");
            }
            else if (K == "posepos")
            {
                const GENRESULT R = GComponent->SetPosePos(G.Instance, HexFloats(T[1]).data());
                Op(Format("\"op\":\"posepos\",\"r\":\"%d\"", R));
            }
            else if (K == "poseori")
            {
                const GENRESULT R = GComponent->SetPoseOri(G.Instance, HexFloats(T[1]).data());
                Op(Format("\"op\":\"poseori\",\"r\":\"%d\"", R));
            }
            else if (K == "count")
            {
                int Out = -9;
                const GENRESULT R = GComponent->GetListCount(G.Instance, &Out);
                Op(Format("\"op\":\"count\",\"r\":\"%d\",\"v\":\"%d\",\"o24\":\"", R, Out) + F(G.Instance->Offset24) + "\"");
            }
            else if (K == "destroyall")
            {
                GComponent->DestroyAll(G.Instance);
                Op("\"op\":\"destroyall\"");
            }
            else if (K == "destroyh")
            {
                GComponent->DestroyHandle(G.Instance, (int)Dec(T[1]));
                Op("\"op\":\"destroyh\"");
            }
            else if (K == "onloop")
            {
                std::vector<float> X = HexFloats(T[2]);
                X.resize(12, 0.0f);
                G.Forwarded = 0;
                G.Instance->OnLoop(HexWord(T[1]), X.data(), nullptr);
                Op(Format("\"op\":\"onloop\",\"fwd\":\"%d\",\"x\":\"", G.Forwarded) + Fs(X.data(), 12) + "\"");
            }
            else if (K == "onkeys")
            {
                G.Forwarded = 0;
                G.Cookie = 0xFFFFFFFF;
                FChannelKeyBatch Batch;
                Batch.Count = 0;
                G.Instance->OnKeys(HexWord(T[1]), nullptr, &Batch);
                Op(Format("\"op\":\"onkeys\",\"fwd\":\"%d\",\"cookie\":\"%lx\"", G.Forwarded, G.Cookie));
            }
            else if (K == "destroy")
            {
                GComponent->Destroy(G.Instance);
                G.Instance = nullptr;
                G.Parts.clear();
                Op(Format("\"op\":\"destroy\",\"cache\":\"%u\"", (unsigned int)G_MeshCache.size()));
            }
            else if (K == "svd")
            {
                FSvdMatrix Source;
                memset(&Source, 0, sizeof(Source));
                Source.RowCount = (int)Dec(T[1]);
                Source.ColCount = (int)Dec(T[2]);
                const std::vector<float> V = HexFloats(T[3]);
                for (int R = 0; R < Source.RowCount; ++R)
                    for (int C = 0; C < Source.ColCount; ++C)
                        Source.Cell[R][C] = V[(size_t)(R * Source.ColCount + C)];
                FSvdMatrix Out;
                memset(&Out, 0, sizeof(Out));
                SvdPseudoInverse(&Source, &Out);
                std::string Cells;
                for (int R = 0; R < Out.RowCount; ++R)
                    for (int C = 0; C < Out.ColCount; ++C)
                        Cells += ((R || C) ? "," : "") + F(Out.Cell[R][C]);
                Op(Format("\"op\":\"svd\",\"rows\":\"%d\",\"cols\":\"%d\",\"v\":\"", Out.RowCount, Out.ColCount) + Cells + "\"");
            }
            else if (K == "euler")
            {
                const std::vector<float> L = HexFloats(T[1]);
                float Bone[0x17] = {0};
                Bone[0x40 / 4] = L[0];
                Bone[0x44 / 4] = L[1];
                Bone[0x4C / 4] = L[2];
                Bone[0x50 / 4] = L[3];
                Bone[0x54 / 4] = L[4];
                Bone[0x58 / 4] = L[5];
                const std::vector<float> M = HexFloats(T[2]);
                float Out[9];
                IkEulerClamp(Out, Bone, M.data());
                Op("\"op\":\"euler\",\"v\":\"" + Fs(Out, 9) + "\"");
            }
            else if (K == "look")
            {
                const std::vector<float> Dir = HexFloats(T[2]);
                const std::vector<float> Parent = HexFloats(T[3]);
                float Out[9];
                IkLookAt(Out, Dir.data(), Parent.data(), (unsigned char)HexWord(T[1]));
                Op("\"op\":\"look\",\"v\":\"" + Fs(Out, 9) + "\"");
            }
            else if (K == "qlog")
            {
                const std::vector<float> A = HexFloats(T[1]);
                const std::vector<float> B = HexFloats(T[2]);
                float Out[3];
                IkQuatLog(Out, A.data(), B.data());
                Op("\"op\":\"qlog\",\"v\":\"" + Fs(Out, 3) + "\"");
            }
            else if (K == "qexp")
            {
                const std::vector<float> A = HexFloats(T[1]);
                float Out[4];
                IkQuatExp(Out, A.data());
                Op("\"op\":\"qexp\",\"v\":\"" + Fs(Out, 4) + "\"");
            }
            else if (K == "tex")
                G.Textures[T[1]] = std::make_pair((int)Dec(T[2]), HexWord(T[3]));
            else if (K == "texfmt")
                G.TextureFormats[(long)HexWord(T[1])] = (int)Dec(T[2]);
            else if (K == "eye")
                RunEye(T);
            else if (!RunPartsOp(*this, T))
            {
                fprintf(stderr, "deformable: unknown operation %s\n", K.c_str());
                GFailed = true;
            }
        }

        FEyeMaterial* EyeAt(const std::string& Name) { return (FEyeMaterial*)G.Materials[Name]; }

        std::string CellText(const FMaterialParamValue& Cell)
        {
            if (Cell.Type == 5 || Cell.Type == 7)
                return Format("%d:", Cell.Type) + (Cell.Value[0] == 0 ? std::string("-") : HexC((const char*)Ptr((long)Cell.Value[0])));
            return Format("%d:%lx,%lx,%lx,%lx", Cell.Type, Cell.Value[0], Cell.Type >= 3 && Cell.Type <= 4 ? Cell.Value[1] : 0,
                          Cell.Type >= 3 && Cell.Type <= 4 ? Cell.Value[2] : 0, Cell.Type == 4 ? Cell.Value[3] : 0);
        }

        std::string EyeState(FEyeMaterial* Eye)
        {
            const FMaterialTextureRow* Row = Eye->TextureRows;
            return "\"v\":\"" + Fs(&Eye->Colors.Diffuse[0], 17) +
                   Format("\",\"tr\":\"%u\",\"one\":\"%u\",\"row\":\"%d,%lx,%lu,%lu,%lu,%u\"", Eye->bTranslucent, Eye->bOneBitAlpha,
                          Row->Handle, Row->Frame, Row->WrapMode, Row->AddressU, Row->AddressV, Row->bSingleFrame) +
                   ",\"name\":\"" + (Row->Name == nullptr ? std::string("-") : HexC(Row->Name)) + "\"";
        }

        void RunEye(const std::vector<std::string>& T)
        {
            // eye <op> <material> ...
            const std::string& Sub = T[1];
            FEyeMaterial* Eye = EyeAt(T[2]);
            if (Sub == "set")
            {
                const int Index = (int)Dec(T[3]);
                FMaterialParamValue Cell((int)Dec(T[4]));
                if (Cell.Type == 5 || Cell.Type == 7)
                    Cell.DupString(T[5] == "-" ? nullptr : (const char*)Unhex(T[5]).data());
                else
                {
                    const std::vector<std::string> W = Words(T[5], ",");
                    for (size_t I = 0; I < W.size() && I < 4; ++I)
                        Cell.Value[I] = HexWord(W[I]);
                }
                const GENRESULT R = Eye->SetParameterValue((unsigned long)Index, &Cell);
                Op(Format("\"op\":\"eyeset\",\"r\":\"%d\",", R) + EyeState(Eye));
            }
            else if (Sub == "get")
            {
                FMaterialParamValue Cell;
                const GENRESULT R = Eye->GetParameterValue((unsigned long)Dec(T[3]), &Cell);
                Op(Format("\"op\":\"eyeget\",\"r\":\"%d\",\"cell\":\"", R) + CellText(Cell) + "\"");
            }
            else if (Sub == "desc")
            {
                FMaterialParamDesc* Table = nullptr;
                const unsigned long Count = Eye->GetParameterTable((void**)&Table);
                std::string Text;
                for (unsigned long I = 0; I < Count; ++I)
                    Text += Format(I ? ";%lx," : "%lx,", Table[I].Flags) + Table[I].Key + "," + CellText(Table[I].Value) + "," +
                            CellText(Table[I].Min) + "," + CellText(Table[I].Max);
                Op("\"op\":\"eyedesc\",\"v\":\"" + Text + "\"");
            }
            else if (Sub == "name")
            {
                Eye->SetName(T[3] == "-" ? nullptr : (const char*)Unhex(T[3]).data());
                char Out[0x80];
                memset(Out, 0, sizeof(Out));
                const GENRESULT R = Eye->GetName(Out, sizeof(Out));
                Op(Format("\"op\":\"eyename\",\"r\":\"%d\",\"v\":\"", R) + HexC(Out) + "\"");
            }
            else if (Sub == "setup")
            {
                const GENRESULT R = Eye->Setup();
                Op(Format("\"op\":\"eyesetup\",\"r\":\"%d\",", R) + EyeState(Eye));
            }
            else if (Sub == "resolve")
            {
                const GENRESULT R = Eye->ResolveTextures(0.0f);
                Op(Format("\"op\":\"eyeresolve\",\"r\":\"%d\",", R) + EyeState(Eye));
            }
            else if (Sub == "verify")
            {
                const GENRESULT R = Eye->ApplyVerify(nullptr, nullptr);
                Op(Format("\"op\":\"eyeverify\",\"r\":\"%d\"", R));
            }
            else if (Sub == "flags")
            {
                unsigned long Out = 0;
                Eye->GetRenderFlags(&Out);
                Op(Format("\"op\":\"eyeflags\",\"v\":\"%lx\"", Out));
            }
            else if (Sub == "bind")
            {
                // eye bind <m> <bone> <record 9 floats> | null
                if (T[3] == "null")
                {
                    Eye->Bind(nullptr);
                    Eye->Teardown(nullptr);
                    Op("\"op\":\"eyebind\"");
                    return;
                }
                FBoneRuntime* Bone = BoneAt(T[3]);
                const std::vector<float> R = HexFloats(T[4]);
                FUvDeformRecord Record;
                Record.BoneId = (int)R[0];
                Record.VertexCount = (int)R[1];
                Record.PlaneDistance = R[2];
                Record.BoneXToU = R[3];
                Record.BoneYToV = R[4];
                Record.MinDu = R[5];
                Record.MaxDu = R[6];
                Record.MinDv = R[7];
                Record.MaxDv = R[8];
                FUvDeformBinding Binding;
                Binding.Record = &Record;
                Binding.Bone = Bone;
                Binding.EyeUvTransformCount = 0;
                FMaterialSubmitContext Ctx;
                memset(&Ctx, 0, sizeof(Ctx));
                Ctx.Inst = Bone->Instance;
                Ctx.World = (void*)Identity12;
                Ctx.View = (void*)Identity12;
                Ctx.Field20 = &Binding;
                Eye->Bind(&Ctx);
                Eye->Teardown(&Ctx);
                Op(Format("\"op\":\"eyebind\",\"n\":\"%lu\"", Binding.EyeUvTransformCount));
            }
            else if (Sub == "submit")
            {
                // eye submit <m> <draw result>
                G.DrawResult = (int)Dec(T[3]);
                FVbListing Listing;
                Listing.VertexBuffer = Ptr(0x123);
                Listing.Fvf = 0x112;
                Listing.BaseVertex = 5;
                FIbScratch Scratch;
                Scratch.IndexBuffer = Ptr(0x456);
                Scratch.Bias = 7;
                FMaterialSubmitContext Ctx;
                memset(&Ctx, 0, sizeof(Ctx));
                Ctx.World = (void*)Identity12;
                Ctx.View = (void*)Identity12;
                Ctx.VbListing = &Listing;
                Ctx.IbScratch = &Scratch;
                const GENRESULT R = Eye->Submit(&Ctx, 4, 2, 9, 3, 12, 0x100);
                G.DrawResult = 0;
                Op(Format("\"op\":\"eyesubmit\",\"r\":\"%d\"", R));
            }
        }
    };
}

namespace deformable_oracle
{
    // The cases of a list into Out, against the fakes, with this material library (none for the
    // first suite).
    IMaterialLibrary* GListLibrary = nullptr;
    void RunDeformableList(Suite& Out, const char* List);
    // oracle_deformable_parts.cpp: deformable_parts.jsonl.
    void RunPartsSuite();
}

namespace deformable_anim_oracle
{
    // oracle_deformable_anim.cpp: deformable_anim.jsonl.
    void RunAnimSuite();
}

// The suites (deformable.jsonl, deformable_parts.jsonl, then deformable_anim.jsonl against the
// scene runtime's Engine and Animation), called from RunMaterialSuites after
// the mesh render suite.
void RunDeformableSuite()
{
    using namespace deformable_oracle;
    if (!OracleLoadMathEngine())
    {
        fprintf(stderr, "deformable: no 3DMathEngine\n");
        GFailed = true;
        return;
    }
    {
        Suite Out("deformable.jsonl");
        RunDeformableList(Out, "deformable.list");
    }
    RunPartsSuite();
    deformable_anim_oracle::RunAnimSuite();
}

void deformable_oracle::RunDeformableList(Suite& Out, const char* List)
{
    const std::vector<std::string> Lines = InputLines(List);
    void(__cdecl * Saved)(int, const char*, ...) = FDUMP;
    FDUMP = DefDump;
    // FDeformable's constructor zeroes the module caches; the fakes go in after it.
    GComponent = new FDeformableObject();
    G_Engine = &GEngineFake;
    G_Animation = &GAnimation;
    G_Channel = &GChannel;
    G_Renderer = &GRenderer;
    G_Pipeline = &GPipeline;
    G_VertexBuffer = &GVb;
    G_IndexBuffer = &GIb;
    G_MaterialLibrary = GListLibrary;
    for (size_t Case = 0; Case < Lines.size(); ++Case)
    {
        G = FDefState();
        G.Out = &Out;
        G.Case = (int)Case;
        GPipeline.States.clear();
        GTextures.Records.clear();
        G_DeformRate = 0.0f;
        G_MeshCache.clear();
        FDefCase Runner;
        for (const std::string& Op : Words(Lines[Case], " ; "))
            Runner.Run(Words(Op, " "));
    }
    FDUMP = Saved;
    G_Engine = nullptr;
    G_Animation = nullptr;
    G_Channel = nullptr;
    G_Renderer = nullptr;
    G_Pipeline = nullptr;
    G_VertexBuffer = nullptr;
    G_IndexBuffer = nullptr;
    G_MaterialLibrary = nullptr;
    G_MeshCache.clear();
}
