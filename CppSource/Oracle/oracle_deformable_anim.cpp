// P4.7 (third increment) and P4.16: the deformable animation suite (deformable_anim.jsonl, input
// deformable_anim.list and the archives skel.cmp and moves.anm in deformable/ from
// make_deformable_fixtures.py). The Rust side is crates/neuron-client/tests/support/deformable_anim.rs
// with the same contract.
//
// The two suites before it run Deformable2 against recording fakes. This one runs its animation
// path against the reference's own scene runtime: the Engine and Animation components of
// SceneRuntime.dll, made through the component manager and aggregated as the client aggregates
// them (oracle_deformable_host.cpp), are Deformable2's G_Engine (G_DeformableEngine in this file),
// G_Animation and G_Channel. A skeleton compound is loaded and instanced by that engine, its
// instances (in GetRelatedInstance order) bound to the bone runtimes of one part's LOD holder
// (BindOneBoneRuntime: the runtime becomes the instance's transform), and the instance's
// IDeformable calls are made on it: Play (PlayInstanceAnim), PlayAlt (PlayInstanceAnimChain),
// GetListCount, DestroyAll, StartAim and StopAim. The animation system calls the instance as the
// sink of its scripts' channels (OnKeys, OnLoop) and each aim event as the owner of its typed
// channels (FIkEvent::Apply) while it walks the mixer; a walk is FEngineCharacterBody::Advance
// (MixerWalk under the root bone, UpdateInstance, GetListCount). The archives are read by the
// parts suite's in-memory UTF file system (FPartsFs).
//
// oracle_mesh_data_dfm.cpp includes this file after oracle_deformable_parts.cpp, whose file system
// and helpers it shares. Operations (one case per line, " ; " between them):
//   model <skeleton> <script> <context hex>: the compound and its instance, the bone runtimes, the
//     instance (UserAt1C the context), the script (OpenScript)
//   sink / nosink: the caller's sink (UserAt18) set or cleared
//   play <animation> <time> <blend> <speed> <scale> <flags hex> <twist>
//   chain <animation> <time> <blend> <speed> <scale> <flags hex> <twist> <bone>
//   walk <dt>
//   count / destroyall
//   aim <root bone> <end bone> <flags hex> <pos> <orient> <damping> <lock bones|-> <blend hex>
//   target <aim> <pos> <orient>
//   stopaim <aim>
// Bones are indices into the bound bones. Every operation writes one line {"case","op",...}; the
// caller's sink writes {"case","ev":"keys","name","ctx","n","t"} (the animation's name the cookie
// points to, the context, the count and the key times) and {"case","ev":"loop","ctx","x"} as the
// walk calls it. Pointers (instances, playbacks, channel keys) are never written. At the end of a
// case the list is destroyed and the root instance with it. FDUMP is silenced for the suite: the
// scene runtime's reports take other arguments than Deformable2's.

#include <list>

// oracle_deformable_host.cpp: the Engine with the Animation aggregated into it, as the client has it.
bool OracleMakeAnimatedEngine(IEngine** Engine, IAnimation2** Animation, IChannel** Channel);

namespace deformable_anim_oracle
{
    using namespace oracle;
    using deformable_oracle::Dec;
    using deformable_oracle::Decs;
    using deformable_oracle::F;
    using deformable_oracle::FPartsFs;
    using deformable_oracle::Fs;
    using deformable_oracle::HexFloat;
    using deformable_oracle::HexFloats;
    using deformable_oracle::HexWord;
    using deformable_oracle::ReadPartsArchive;
    using deformable_oracle::Words;

    void* const Invalid = (void*)((ptrdiff_t)(-1));

    struct FAnimCase
    {
        Suite* Out = nullptr;
        int Case = 0;
        void* Root = Invalid;
        std::vector<void*> Bones;
        FDeformInstance* Instance = nullptr;
        void* Script = nullptr;
        std::list<std::string> Names;
        std::list<std::vector<float>> Targets;
        std::vector<std::vector<float>*> AimTargets;
        std::vector<std::vector<unsigned char>> AimDescs;
        std::vector<void*> AimNodes;

        void Line(const std::string& Body) { Out->Line(Format("{\"case\":\"%d\",", Case) + Body + "}"); }
    };

    FAnimCase A;
    IEngine* GEngine = nullptr;
    IAnimation2* GAnim = nullptr;
    IChannel* GChannelFacet = nullptr;
    FDeformable* GAnimComponent = nullptr;
    int GAnimDumps = 0;

    void __cdecl AnimDump(int, const char*, ...) { ++GAnimDumps; }

    // The caller's sink of the instance (UserAt18).
    class FAnimSink final : public IChannelSink
    {
    public:
        void NEURONCOMPONENTS_API OnKeys(unsigned long Cookie, void* Context, FChannelKeyBatch* Batch) override
        {
            const unsigned long Count = Batch->Count;
            std::string Times;
            for (unsigned long Index = 0; Index < Count && Index < 0x40; ++Index)
                Times += (Index ? "," : "") + F(Batch->Keys[Index][0]);
            const char* Name = (const char*)(uintptr_t)Cookie;
            A.Line(Format("\"ev\":\"keys\",\"name\":\"%s\",\"ctx\":\"%lx\",\"n\":\"%lu\",\"t\":\"", Name ? Name : "-",
                          (unsigned long)(uintptr_t)Context, Count) +
                   Times + "\"");
        }
        void NEURONCOMPONENTS_API OnOpaqueNotification(void*, void*) override {}
        void NEURONCOMPONENTS_API OnLoop(unsigned long, float* Transform, void* Context) override
        {
            A.Line(Format("\"ev\":\"loop\",\"ctx\":\"%lx\",\"x\":\"", (unsigned long)(uintptr_t)Context) +
                   Fs(Transform, 12) + "\"");
        }
    };

    FAnimSink GAnimSink;

    bool MakeAnimHost()
    {
        return GEngine != nullptr || OracleMakeAnimatedEngine(&GEngine, &GAnim, &GChannelFacet);
    }

    std::string BonesText()
    {
        std::string Out;
        FLodHolder* Holder = A.Instance->PartArray[0].Lod;
        for (int Index = 0; Index < Holder->BindCursor; ++Index)
        {
            FBoneRuntime* Bone = &Holder->BoneSlots[Index];
            Out += (Index ? ";" : "") + Format("%x:", Bone->Flags) + Fs(Bone->LocalTransform, 12);
        }
        return Out;
    }

    void* BoneInstance(const std::string& Text) { return A.Bones[(size_t)Dec(Text)]; }

    void Model(const std::vector<std::string>& T)
    {
        FPartsFs* SkelFs = new FPartsFs(ReadPartsArchive(T[1].c_str()));
        FEngineName Name = {T[1].c_str(), NEURONCOMPONENTS_CRC::GetCRC32(T[1].c_str())};
        void* Handle = G_Engine->CreateArchetypeHandle(&Name, SkelFs);
        A.Root = Handle == Invalid ? Invalid : G_Engine->CreateInstance(Handle, nullptr);
        A.Bones.clear();
        if (A.Root != Invalid)
        {
            A.Bones.push_back(A.Root);
            void* Cursor = G_Engine->GetRelatedInstance(A.Root, 0, Invalid);
            while (Cursor != Invalid)
            {
                A.Bones.push_back(Cursor);
                Cursor = G_Engine->GetRelatedInstance(A.Root, 0, Cursor);
            }
        }
        const int Count = (int)A.Bones.size();
        FSkeleton* Skel = ConstructSkeleton();
        Skel->BoneCount = Count;
        FLodHolder* Holder = new FLodHolder;
        Holder->Skeleton = Skel;
        Holder->BoneSlots = (FBoneRuntime*)new unsigned char[(size_t)(Count ? Count : 1) * sizeof(FBoneRuntime)];
        for (int Index = 0; Index < Count; ++Index)
            InitBoneRuntimeSlot(&Holder->BoneSlots[Index]);
        Holder->BindCursor = 0;
        Holder->RefCount = 1;
        for (int Index = 0; Index < Count; ++Index)
            BindOneBoneRuntime(Holder, Index, A.Bones[(size_t)Index]);
        Holder->BindCursor = Count;

        const float Saved = G_DeformRate;
        G_DeformRate = 0.0f;
        FDeformInstance* Instance = new FDeformInstance;
        ConstructInstanceBody(Instance);
        G_DeformRate = Saved;
        int* Block = (int*)new unsigned char[sizeof(FDeformPart) + 4];
        *Block = 1;
        Instance->PartArray = (FDeformPart*)(Block + 1);
        ConstructPart(&Instance->PartArray[0]);
        Instance->PartArray[0].Lod = Holder;
        Instance->PartCount = 1;
        Instance->UserAt18 = nullptr;
        Instance->UserAt1C = (void*)(uintptr_t)HexWord(T[3]);
        A.Instance = Instance;

        FPartsFs* ScriptFs = new FPartsFs(ReadPartsArchive(T[2].c_str()));
        A.Script = G_Animation->OpenScript(ScriptFs);
        ScriptFs->Release();

        std::string Names;
        for (int Index = 0; Index < Count; ++Index)
        {
            FEngineName Out = {nullptr, 0};
            G_Engine->GetInstanceName(&Out, A.Bones[(size_t)Index]);
            Names += (Index ? "," : "") + std::string(Out.Text ? Out.Text : "-");
        }
        const int bScript = A.Script != Invalid && A.Script != nullptr ? 1 : 0;
        A.Line(Format("\"op\":\"model\",\"n\":\"%d\",\"names\":\"", Count) + Names +
               Format("\",\"script\":\"%d\",\"bones\":\"", bScript) + BonesText() + "\"");
    }

    void Play(const std::vector<std::string>& T, bool bChain)
    {
        A.Names.push_back(T[1]);
        void* Name = (void*)A.Names.back().c_str();
        void* Args[6];
        for (int Index = 0; Index < 6; ++Index)
            Args[Index] = (void*)(uintptr_t)HexWord(T[2 + (size_t)Index]);
        int Out = -9;
        GENRESULT R;
        if (bChain)
            R = GAnimComponent->PlayAlt(A.Instance, &Out, A.Script, Name, Args[0], Args[1], Args[2], Args[3], Args[4],
                                        Args[5], BoneInstance(T[8]));
        else
            R = GAnimComponent->Play(A.Instance, &Out, A.Script, Name, Args[0], Args[1], Args[2], Args[3], Args[4],
                                     Args[5], nullptr);
        A.Line(Format("\"op\":\"%s\",\"r\":\"%d\",\"ok\":\"%d\",\"n\":\"%d\",\"o24\":\"", bChain ? "chain" : "play", R,
                      Out != -1 ? 1 : 0, A.Instance->ListCount) +
               F(A.Instance->Offset24) + "\"");
    }

    void Walk(const std::vector<std::string>& T)
    {
        const float Dt = HexFloat(T[1]);
        void* Root = (void*)((ptrdiff_t)FirstBoneInstance(A.Instance));
        G_Animation->MixerWalk((unsigned long)(uintptr_t)Root, Dt);
        void* TimeArg = nullptr;
        memcpy(&TimeArg, &Dt, sizeof(Dt));
        G_Engine->UpdateInstance(Root, 0, TimeArg);
        int Count = -9;
        GAnimComponent->GetListCount(A.Instance, &Count);
        A.Line("\"op\":\"walk\",\"dt\":\"" + F(Dt) + Format("\",\"n\":\"%d\",\"o24\":\"", Count) +
               F(A.Instance->Offset24) + "\",\"bones\":\"" + BonesText() + "\"");
    }

    void Aim(const std::vector<std::string>& T)
    {
        std::vector<unsigned char> Desc(0x20 + 4 * 8, 0);
        const std::vector<long> Locks = Decs(T[7]);
        A.Targets.push_back(HexFloats(T[4]));
        std::vector<float>& Target = A.Targets.back();
        const std::vector<float> Orient = HexFloats(T[5]);
        Target.insert(Target.end(), Orient.begin(), Orient.end());
        A.AimTargets.push_back(&Target);
        void* Root = BoneInstance(T[1]);
        void* End = BoneInstance(T[2]);
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
        for (int Index = 0; Index < LockCount && Index < 8; ++Index)
        {
            void* Lock = A.Bones[(size_t)Locks[(size_t)Index]];
            memcpy(&Desc[0x20 + 4 * Index], &Lock, 4);
        }
        A.AimDescs.push_back(Desc);
        void* Node = nullptr;
        const GENRESULT R =
            GAnimComponent->StartAim(A.Instance, &Node, A.AimDescs.back().data(), (void*)(uintptr_t)HexWord(T[8]));
        std::string Detail;
        if (R == GR_OK)
        {
            FIkEvent* Event = ((FIkListNode*)Node)->Event;
            Detail = Format(",\"joints\":\"%d\",\"f34\":\"%x\"", Event->JointCount, Event->Flags34 & 3);
        }
        A.AimNodes.push_back(R == GR_OK ? Node : nullptr);
        A.Line(Format("\"op\":\"aim\",\"r\":\"%d\",\"n\":\"%d\"", R, A.Instance->ListCount) + Detail);
    }

    void Run(const std::vector<std::string>& T)
    {
        const std::string& K = T[0];
        if (K == "model")
            Model(T);
        else if (K == "sink" || K == "nosink")
        {
            A.Instance->UserAt18 = K == "sink" ? &GAnimSink : nullptr;
            A.Line("\"op\":\"" + K + "\"");
        }
        else if (K == "play" || K == "chain")
            Play(T, K == "chain");
        else if (K == "walk")
            Walk(T);
        else if (K == "count")
        {
            int Count = -9;
            const GENRESULT R = GAnimComponent->GetListCount(A.Instance, &Count);
            A.Line(Format("\"op\":\"count\",\"r\":\"%d\",\"n\":\"%d\",\"o24\":\"", R, Count) +
                   F(A.Instance->Offset24) + "\"");
        }
        else if (K == "destroyall")
        {
            GAnimComponent->DestroyAll(A.Instance);
            A.Line(Format("\"op\":\"destroyall\",\"n\":\"%d\"", A.Instance->ListCount));
        }
        else if (K == "aim")
            Aim(T);
        else if (K == "target")
        {
            std::vector<float>& Target = *A.AimTargets[(size_t)Dec(T[1])];
            const std::vector<float> Pos = HexFloats(T[2]);
            const std::vector<float> Orient = HexFloats(T[3]);
            for (int Index = 0; Index < 3; ++Index)
                Target[(size_t)Index] = Pos[(size_t)Index];
            for (int Index = 0; Index < 9; ++Index)
                Target[3 + (size_t)Index] = Orient[(size_t)Index];
            A.Line("\"op\":\"target\"");
        }
        else if (K == "stopaim")
        {
            const GENRESULT R = GAnimComponent->StopAim(A.Instance, A.AimNodes[(size_t)Dec(T[1])]);
            A.Line(Format("\"op\":\"stopaim\",\"r\":\"%d\"", R));
        }
        else
        {
            fprintf(stderr, "deformable_anim: unknown operation %s\n", K.c_str());
            GFailed = true;
        }
    }

    void RunAnimSuite()
    {
        Suite Out("deformable_anim.jsonl");
        if (!MakeAnimHost())
        {
            fprintf(stderr, "deformable_anim: no Engine and Animation components\n");
            GFailed = true;
            return;
        }
        void(__cdecl * Saved)(int, const char*, ...) = FDUMP;
        FDUMP = AnimDump;
        GAnimComponent = new FDeformableObject();
        G_Engine = GEngine;
        G_Animation = GAnim;
        G_Channel = GChannelFacet;
        const std::vector<std::string> Lines = InputLines("deformable_anim.list");
        for (size_t Case = 0; Case < Lines.size(); ++Case)
        {
            A = FAnimCase();
            A.Out = &Out;
            A.Case = (int)Case;
            for (const std::string& Op : Words(Lines[Case], " ; "))
                Run(Words(Op, " "));
            if (A.Instance != nullptr)
                GAnimComponent->DestroyAll(A.Instance);
            if (A.Root != Invalid)
                G_Engine->DestroyInstance(A.Root);
        }
        G_Engine = nullptr;
        G_Animation = nullptr;
        G_Channel = nullptr;
        FDUMP = Saved;
    }
}
