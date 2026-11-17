// Oracle suites and data dumps of work package P1.8 (scene runtime): the
// Engine, Hardpoint and Animation components of SceneRuntime.dll. The Rust
// test crates/neuron-core/tests/oracle_scene.rs writes the same lines; the
// inputs come from crates/neuron-core/tests/fixtures/oracle/make_scene_fixtures.py.
//
// SceneRuntime.dll is loaded by oracle_anim.cpp (OracleLoadSceneRuntime);
// its DllMain registers the four factories with neuroncomponents. Each suite
// and each dump makes its own engine through ICOManager, as the client does,
// and aggregates into it the two components the scene needs:
//
//  * Hardpoint, created with the engine as its outer unknown (as
//    IEngine::LoadEngineComponents creates it from the [Engine] section),
//    initialised, and linked into the engine's engine-component list. The
//    harness links that list node itself (FEngineChildNode, SceneRuntime.h)
//    instead of running LoadEngineComponents, whose component list comes from
//    the process profile; no other engine component (RenderManager,
//    Deformable, ...) is loaded, so archetypes are loaded with their compound
//    structure and hardpoints only.
//  * Animation, created with the engine as its outer unknown and initialised
//    through IAggregateComponent, which points the DLL's G_AnimEngine at this
//    engine.
//
// Everything is driven through IEngine, IHardpoint, IAnimation2 and
// IChannelArchetype; what the interfaces do not return (an archetype's parts
// and constraints, the hardpoint lists, an instance's joint and local
// transform) is read through the layouts in SceneRuntime.h. The DLL and this
// program are built with the same toolset and configuration, so their
// std::string, std::vector, std::list and std::map layouts agree. The engines
// are never released.
//
// Floats are written as their bit patterns, every NaN as 7fc00000.

#include "oracle_support.h"

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "neuroncomponents.h"
#include "readfile.h"
#include "../SceneRuntime/SceneRuntime.h"

using namespace oracle;

// oracle_anim.cpp.
bool OracleLoadSceneRuntime();

namespace
{
    void* const Invalid = (void*)((ptrdiff_t)(-1));
    const unsigned long PackedMask = 0x1F0;

    struct FSceneHost
    {
        IEngine* Engine;
        FEngineObject* EngineObject;
        FHardpointObject* Hardpoints;
        IAnimation2* Anim;
    };

    bool MakeHost(FSceneHost& Host)
    {
        memset(&Host, 0, sizeof(Host));
        if (!OracleLoadSceneRuntime())
            return false;
        ICOManager* Manager = NEURONCOMPONENTS_Acquire();
        FAggDesc8 EngineDesc;
        EngineDesc.Size = G_AggDescSizeEngine;
        EngineDesc.Name = "Engine";
        void* Created = nullptr;
        if (Manager->CreateInstance((AGGDESC*)(&EngineDesc), &Created) != GR_OK || Created == nullptr)
        {
            fprintf(stderr, "scene: no Engine component\n");
            GFailed = true;
            return false;
        }
        Host.Engine = (IEngine*)(Created);
        Host.EngineObject = static_cast<FEngineObject*>(Host.Engine);

        void* HardpointOuter = nullptr;
        FAggDesc18 HardpointDesc;
        memset(&HardpointDesc, 0, sizeof(HardpointDesc));
        HardpointDesc.Size = G_AggDescSizeHardpoint;
        HardpointDesc.Name = "Hardpoint";
        HardpointDesc.Inner = Host.Engine;
        HardpointDesc.Outer = &HardpointOuter;
        Created = nullptr;
        if (Manager->CreateInstance((AGGDESC*)(&HardpointDesc), &Created) != GR_OK || Created == nullptr)
        {
            fprintf(stderr, "scene: no Hardpoint component\n");
            GFailed = true;
            return false;
        }
        Host.Hardpoints = (FHardpointObject*)(Created);
        IEngineComponent* Component = static_cast<IEngineComponent*>(Host.Hardpoints);
        if (Component->Initialize() < 0)
        {
            fprintf(stderr, "scene: Hardpoint initialisation failed\n");
            GFailed = true;
            return false;
        }
        FEngineChildNode* Node = new FEngineChildNode;
        memset(Node, 0, sizeof(*Node));
        memcpy(Node->Name, "Hardpoint", sizeof("Hardpoint"));
        Node->Object = Component;
        FEngineChildNode* Sentinel = Host.EngineObject->EngineComponentList;
        Node->Next = Sentinel;
        Node->Prev = Sentinel->Prev;
        Sentinel->Prev->Next = Node;
        Sentinel->Prev = Node;
        Host.EngineObject->EngineComponentCount += 1;

        void* AnimationOuter = nullptr;
        FAggDesc18 AnimationDesc;
        memset(&AnimationDesc, 0, sizeof(AnimationDesc));
        AnimationDesc.Size = G_AggDescSizeAnimation;
        AnimationDesc.Name = "Animation";
        AnimationDesc.Inner = Host.Engine;
        AnimationDesc.Outer = &AnimationOuter;
        Created = nullptr;
        if (Manager->CreateInstance((AGGDESC*)(&AnimationDesc), &Created) != GR_OK || Created == nullptr)
        {
            fprintf(stderr, "scene: no Animation component\n");
            GFailed = true;
            return false;
        }
        FAnimationObject* Animation = (FAnimationObject*)(Created);
        if (static_cast<IAggregateComponent*>(Animation)->Initialize() != GR_OK)
        {
            fprintf(stderr, "scene: Animation initialisation failed\n");
            GFailed = true;
            return false;
        }
        Host.Anim = static_cast<IAnimation2*>(Animation);
        return true;
    }

    FFileSystemDesc MakeDesc(const char* Implementation, const char* Path)
    {
        FFileSystemDesc Desc;
        memset(&Desc, 0, sizeof(Desc));
        Desc.Size = 0x34;
        Desc.ComponentName = "FileSystem";
        Desc.Implementation = Implementation;
        Desc.Path = Path;
        Desc.Access = 0x80000000;
        Desc.ShareMode = 1;
        Desc.Creation = 3;
        Desc.Flags = 0x08000080;
        Desc.Field30 = 0xFFFFFFFF;
        return Desc;
    }

    IFileSystem* OpenArchive(const std::string& Path)
    {
        FFileSystemDesc Desc = MakeDesc("UTF", Path.c_str());
        IFileSystem* Fs = nullptr;
        if (NEURONCOMPONENTS_Acquire()->CreateInstance((AGGDESC*)(&Desc), (void**)(&Fs)) != GR_OK)
            return nullptr;
        return Fs;
    }

    std::string Bits(float Value)
    {
        const unsigned int B = Value != Value ? 0x7FC00000u : FloatBits(Value);
        return Format("%08x", B);
    }

    std::string FloatList(const float* Values, size_t Count)
    {
        std::string Out;
        for (size_t Index = 0; Index < Count; ++Index)
        {
            if (Index != 0)
                Out += ",";
            Out += Bits(Values[Index]);
        }
        return Out;
    }

    std::string PayloadHex(const unsigned long* Payload)
    {
        std::string Out;
        for (unsigned long Index = 0; Index < G_EngineJointPayloadDwords; ++Index)
            Out += Format("%08lx", Payload[Index]);
        return Out;
    }

    template <typename T>
    std::string IndexOf(const std::vector<T>& List, const T& Item)
    {
        for (size_t Index = 0; Index < List.size(); ++Index)
        {
            if (List[Index] == Item)
                return Format("%u", (unsigned int)Index);
        }
        return "-";
    }

    std::vector<std::string> Split(const std::string& Text, char Separator)
    {
        std::vector<std::string> Out;
        size_t Start = 0;
        for (;;)
        {
            const size_t At = Text.find(Separator, Start);
            Out.push_back(Text.substr(Start, At == std::string::npos ? std::string::npos : At - Start));
            if (At == std::string::npos)
                return Out;
            Start = At + 1;
        }
    }

    float HexFloat(const std::string& Text)
    {
        return FloatFromBits((unsigned int)strtoul(Text.c_str(), nullptr, 16));
    }

    bool Valid(void* Handle)
    {
        return Handle != Invalid && Handle != nullptr;
    }

    // MixerJointValueCount: the words GetJointParams(.., 0, ..) writes.
    unsigned long ValueCount(unsigned long Type)
    {
        switch (Type)
        {
        case 1:
        case 2:
            return 1;
        case 3:
            return 2;
        case 4:
            return 4;
        case 5:
            return 3;
        case 6:
            return 7;
        default:
            return 0;
        }
    }

    void Number(FArchetypeHandle* Handle, std::vector<FArchetypeHandle*>& Order)
    {
        if (std::find(Order.begin(), Order.end(), Handle) != Order.end())
            return;
        Order.push_back(Handle);
        for (size_t Index = 0; Index < Handle->Children.size(); ++Index)
            Number(Handle->Children[Index].Handle, Order);
    }

    std::vector<FEngineInstance*> InstanceOrder(FSceneHost& Host, FEngineInstance* Root)
    {
        std::vector<FEngineInstance*> Out;
        Out.push_back(Root);
        void* Cursor = Host.Engine->GetRelatedInstance(Root, 0, Invalid);
        while (Cursor != Invalid)
        {
            Out.push_back((FEngineInstance*)(Cursor));
            Cursor = Host.Engine->GetRelatedInstance(Root, 0, Cursor);
        }
        return Out;
    }

    const FHardpointList* HardpointsOf(FSceneHost& Host, FArchetypeHandle* Handle)
    {
        FHardpointMap::iterator It = Host.Hardpoints->Archetypes.find((long)((intptr_t)Handle));
        if (It == Host.Hardpoints->Archetypes.end())
            return nullptr;
        return &It->second;
    }

    // The archetype_lines of oracle_scene.rs.
    void ArchetypeLines(FSceneHost& Host, FArchetypeHandle* Top, Suite& Out)
    {
        std::vector<FArchetypeHandle*> Order;
        Number(Top, Order);
        for (size_t Index = 0; Index < Order.size(); ++Index)
        {
            FArchetypeHandle* Handle = Order[Index];
            const FHardpointList* Records = HardpointsOf(Host, Handle);
            Out.Line(Format("{\"archetype\":\"%u\",\"name\":\"%s\",\"crc\":\"%08lx\",\"refs\":\"%lu\",\"parts\":\"%u\",\"constraints\":\"%u\",\"hardpoints\":\"%s\"}",
                            (unsigned int)Index, HexC(Handle->Name.c_str()).c_str(), Handle->Crc,
                            Handle->RefCount, (unsigned int)Handle->Children.size(),
                            (unsigned int)Handle->Constraints.size(),
                            Records == nullptr ? "-" : Format("%u", (unsigned int)Records->size()).c_str()));
            for (size_t Part = 0; Part < Handle->Children.size(); ++Part)
            {
                const FArchetypeChild& Child = Handle->Children[Part];
                Out.Line(Format("{\"part\":\"%u\",\"name\":\"%s\",\"crc\":\"%08lx\",\"archetype\":\"%s\"}",
                                (unsigned int)Part, HexC(Child.Name).c_str(), Child.Crc,
                                IndexOf(Order, Child.Handle).c_str()));
            }
            const FArchetypeChild* First = Handle->Children.empty() ? nullptr : &Handle->Children[0];
            for (size_t Cons = 0; Cons < Handle->Constraints.size(); ++Cons)
            {
                const FConstraintRecord& Record = Handle->Constraints[Cons];
                Out.Line(Format("{\"constraint\":\"%u\",\"parent\":\"%u\",\"child\":\"%u\",\"payload\":\"%s\"}",
                                (unsigned int)Cons, (unsigned int)(Record.ParentPart - First),
                                (unsigned int)(Record.ChildPart - First), PayloadHex(Record.Payload).c_str()));
            }
            if (Records != nullptr)
            {
                unsigned int Hp = 0;
                for (FHardpointList::const_iterator It = Records->begin(); It != Records->end(); ++It, ++Hp)
                {
                    Out.Line(Format("{\"hardpoint\":\"%u\",\"name\":\"%s\",\"crc\":\"%08lx\",\"data\":\"%s\"}", Hp,
                                    HexC(It->Name).c_str(), It->Crc, Hex(It->Data, sizeof(It->Data)).c_str()));
                }
            }
        }
        void* Created = Host.Engine->CreateInstance(Top, nullptr);
        if (!Valid(Created))
        {
            Out.Line("{\"instances\":\"0\"}");
            return;
        }
        FEngineInstance* Root = (FEngineInstance*)(Created);
        const std::vector<FEngineInstance*> Instances = InstanceOrder(Host, Root);
        Out.Line(Format("{\"instances\":\"%u\"}", (unsigned int)Instances.size()));
        for (size_t Index = 0; Index < Instances.size(); ++Index)
        {
            const FEngineInstance* Instance = Instances[Index];
            const std::string Parent =
                Instance->Parent == nullptr ? "-" : IndexOf(Instances, Instance->Parent);
            std::string Part = "-";
            if (Instance->PartChild != nullptr && Instance->ParentArchetype != nullptr)
                Part = Format("%u", (unsigned int)(Instance->PartChild - &Instance->ParentArchetype->Children[0]));
            Out.Line(Format("{\"instance\":\"%u\",\"parent\":\"%s\",\"archetype\":\"%s\",\"part\":\"%s\",\"joint\":\"%s\"}",
                            (unsigned int)Index, Parent.c_str(), IndexOf(Order, Instance->Archetype).c_str(),
                            Part.c_str(), PayloadHex(Instance->Relative).c_str()));
        }
        Host.Engine->DestroyInstance(Root);
    }

    FArchetypeHandle* CreateArchetype(FSceneHost& Host, const std::string& Name, IFileSystem* Fs)
    {
        FEngineName EngineName;
        EngineName.Text = Name.c_str();
        EngineName.Crc = NEURONCOMPONENTS_CRC::GetCRC32(Name.c_str());
        void* Handle = Host.Engine->CreateArchetypeHandle(&EngineName, Fs);
        return Valid(Handle) ? (FArchetypeHandle*)(Handle) : nullptr;
    }

    FArchetypeHandle* LoadFixture(FSceneHost& Host, const std::string& Archive)
    {
        IFileSystem* Fs = OpenArchive(Archive);
        if (Fs == nullptr)
            return nullptr;
        FArchetypeHandle* Handle = CreateArchetype(Host, Archive, Fs);
        Fs->Release();
        return Handle;
    }

    // scene_load: CreateArchetypeHandle on every archive of scene/load.list.
    void SceneLoad()
    {
        Suite Out("scene_load.jsonl");
        FSceneHost Host;
        if (!MakeHost(Host))
            return;
        for (const std::string& Archive : InputLines("scene/load.list"))
        {
            IFileSystem* Fs = OpenArchive(Archive);
            if (Fs == nullptr)
            {
                Out.Line(Format("{\"file\":\"%s\",\"archetype\":\"-\"}", Archive.c_str()));
                continue;
            }
            FArchetypeHandle* Handle = CreateArchetype(Host, Archive, Fs);
            if (Handle == nullptr)
            {
                Out.Line(Format("{\"file\":\"%s\",\"archetype\":\"0\"}", Archive.c_str()));
            }
            else
            {
                Out.Line(Format("{\"file\":\"%s\",\"archetype\":\"1\"}", Archive.c_str()));
                ArchetypeLines(Host, Handle, Out);
                Host.Engine->ReleaseHandle(Handle);
            }
            Fs->Release();
            Out.Line(Format("{\"live\":\"%u\"}", (unsigned int)Host.EngineObject->Archetypes.size()));
        }
    }

    IChannelArchetype* NewChannel()
    {
        FAggDesc8 Desc;
        Desc.Size = G_AggDescSizeChannelArchetype;
        Desc.Name = "ChannelArchetype";
        void* Instance = nullptr;
        if (NEURONCOMPONENTS_Acquire()->CreateInstance((AGGDESC*)(&Desc), &Instance) != GR_OK)
            return nullptr;
        return (IChannelArchetype*)(Instance);
    }

    // A channel of scene/sample.utf, loaded; null on failure.
    IChannelArchetype* SampleChannel(IFileSystem* Archive, const std::string& Name)
    {
        FFileSystemDesc Desc = MakeDesc(nullptr, Name.c_str());
        IFileSystem* Nested = nullptr;
        if (Archive->CreateInstance((AGGDESC*)(&Desc), (void**)(&Nested)) != GR_OK || Nested == nullptr)
            return nullptr;
        IChannelArchetype* Channel = NewChannel();
        if (Channel != nullptr && Channel->Load(Nested) != GR_OK)
        {
            Channel->Release();
            Channel = nullptr;
        }
        Nested->Release();
        if (Channel == nullptr)
        {
            fprintf(stderr, "scene: cannot load channel %s\n", Name.c_str());
            GFailed = true;
        }
        return Channel;
    }

    std::string FrameIndex(const IChannelArchetype* Channel, const unsigned char* Payload)
    {
        const FChannelArchetypeObject* Object = static_cast<const FChannelArchetypeObject*>(Channel);
        const unsigned long Skip = (double)(Object->SampleRate) < 0.0 ? 4 : 0;
        return Format("%lu", (unsigned long)(Payload - Object->Frames - Skip) / Object->Stride);
    }

    std::string KeysText(const IChannelArchetype* Channel, const float* Keys, unsigned long Count,
                         unsigned long Capacity)
    {
        const FChannelArchetypeObject* Object = static_cast<const FChannelArchetypeObject*>(Channel);
        std::string Out;
        const unsigned long Written = Count < Capacity ? Count : Capacity;
        for (unsigned long Index = 0; Index < Written; ++Index)
        {
            unsigned long Key = 0;
            memcpy(&Key, &Keys[Index * 2 + 1], sizeof(Key));
            if (Index != 0)
                Out += ",";
            Out += Bits(Keys[Index * 2]) + Format(":%lx", Key - (unsigned long)((uintptr_t)Object->Frames));
        }
        return Out;
    }

    // scene_sample: IChannelArchetype slots 6 to 10 on the channels of
    // scene/sample.utf.
    void SceneSample()
    {
        Suite Out("scene_sample.jsonl");
        if (!OracleLoadSceneRuntime())
            return;
        IFileSystem* Archive = OpenArchive("scene/sample.utf");
        if (Archive == nullptr)
        {
            fprintf(stderr, "scene: cannot open scene/sample.utf\n");
            GFailed = true;
            return;
        }
        for (const std::string& Line : InputLines("scene/sample.list"))
        {
            const std::vector<std::string> F = Split(Line, '|');
            IChannelArchetype* Channel = SampleChannel(Archive, F[0]);
            if (Channel == nullptr)
                continue;
            const float Time = HexFloat(F[1]);
            std::string Text = Format("{\"channel\":\"%s\",\"time\":\"%s\"", F[0].c_str(), F[1].c_str());
            const unsigned char* A = nullptr;
            const unsigned char* B = nullptr;
            float Alpha = 0.0f;
            const GENRESULT Sampled = Channel->Sample(Time, &A, &B, &Alpha);
            if (Sampled == GR_OK)
                Text += ",\"a\":\"" + FrameIndex(Channel, A) + "\",\"b\":\"" + FrameIndex(Channel, B) +
                        "\",\"alpha\":\"" + Bits(Alpha) + "\"";
            else
                Text += Format(",\"sample\":\"%08lx\"", (unsigned long)Sampled);
            const unsigned char* Lo = nullptr;
            const unsigned char* Hi = nullptr;
            float TimeLo = 0.0f;
            float TimeHi = 0.0f;
            float PairAlpha = 0.0f;
            const GENRESULT Paired = Channel->SamplePair(Time, &Lo, &Hi, &TimeLo, &TimeHi, &PairAlpha);
            if (Paired == GR_OK)
                Text += ",\"lo\":\"" + FrameIndex(Channel, Lo) + "\",\"hi\":\"" + FrameIndex(Channel, Hi) +
                        "\",\"time_lo\":\"" + Bits(TimeLo) + "\",\"time_hi\":\"" + Bits(TimeHi) +
                        "\",\"pair_alpha\":\"" + Bits(PairAlpha) + "\"";
            else
                Text += Format(",\"pair\":\"%08lx\"", (unsigned long)Paired);
            const unsigned char* Frame = nullptr;
            const std::string First =
                Channel->GetFrame(0, &Frame) == GR_OK ? FrameIndex(Channel, Frame) : std::string("-");
            Frame = nullptr;
            const std::string Last =
                Channel->GetFrame(1, &Frame) == GR_OK ? FrameIndex(Channel, Frame) : std::string("-");
            Out.Line(Text + ",\"first\":\"" + First + "\",\"last\":\"" + Last + "\"}");
            Channel->Release();
        }
        for (const std::string& Line : InputLines("scene/range.list"))
        {
            const std::vector<std::string> F = Split(Line, '|');
            IChannelArchetype* Channel = SampleChannel(Archive, F[0]);
            if (Channel == nullptr)
                continue;
            const unsigned long Capacity = strtoul(F[3].c_str(), nullptr, 10);
            std::vector<float> Keys(Capacity * 2 + 2, 0.0f);
            unsigned long Count = 0;
            const GENRESULT Result =
                Channel->CollectRange(HexFloat(F[1]), HexFloat(F[2]), Keys.data(), Capacity, &Count);
            Out.Line(Format("{\"range\":\"%s\",\"result\":\"%08lx\",\"count\":\"%lu\",\"keys\":\"", Line.c_str(),
                            (unsigned long)Result, Count) +
                     KeysText(Channel, Keys.data(), Count, Capacity) + "\"}");
            Channel->Release();
        }
        for (const std::string& Line : InputLines("scene/at.list"))
        {
            const std::vector<std::string> F = Split(Line, '|');
            IChannelArchetype* Channel = SampleChannel(Archive, F[0]);
            if (Channel == nullptr)
                continue;
            const unsigned long Capacity = strtoul(F[2].c_str(), nullptr, 10);
            std::vector<float> Keys(Capacity * 2 + 2, 0.0f);
            unsigned long Count = 0;
            const GENRESULT Result = Channel->CollectAt(HexFloat(F[1]), Keys.data(), Capacity, &Count);
            Out.Line(Format("{\"at\":\"%s\",\"result\":\"%08lx\",\"count\":\"%lu\",\"keys\":\"", Line.c_str(),
                            (unsigned long)Result, Count) +
                     KeysText(Channel, Keys.data(), Count, Capacity) + "\"}");
            Channel->Release();
        }
        Archive->Release();
    }

    std::string ParamsText(FSceneHost& Host, FEngineInstance* Instance)
    {
        float Values[7] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        if (Host.Engine->GetJointParams(Instance, 0, Values) == 0)
            return "-";
        return FloatList(Values, ValueCount(Instance->Relative[0]));
    }

    void ScenarioPose(FSceneHost& Host, FEngineInstance* Root, unsigned int Op, Suite& Out)
    {
        const std::vector<FEngineInstance*> Instances = InstanceOrder(Host, Root);
        for (size_t Index = 0; Index < Instances.size(); ++Index)
        {
            FEngineInstance* Instance = Instances[Index];
            const float* World = Host.Engine->GetInstanceTransform(Instance);
            Out.Line(Format("{\"op\":\"%u\",\"instance\":\"%u\",\"state\":\"", Op, (unsigned int)Index) +
                     FloatList(Instance->JointState, 7) + "\",\"local\":\"" + FloatList(Instance->Matrix, 12) +
                     "\",\"world\":\"" + FloatList(World, 12) + "\",\"values\":\"" +
                     ParamsText(Host, Instance) + "\"}");
        }
    }

    unsigned char __cdecl CountObject(void* Object, void* Context)
    {
        (void)Object;
        *(unsigned int*)(Context) += 1;
        return 1;
    }

    unsigned int BoundCount(FSceneHost& Host, unsigned long Id)
    {
        unsigned int Count = 0;
        Host.Anim->EnumObjects((void*)(&CountObject), Id, &Count);
        return Count;
    }

    void PlayScenario(FSceneHost& Host, void* Script, FArchetypeHandle* Top, const std::string& Line, Suite& Out)
    {
        Out.Line("{\"play\":\"" + Line + "\"}");
        void* Created = Top == nullptr ? Invalid : Host.Engine->CreateInstance(Top, nullptr);
        if (!Valid(Created))
        {
            Out.Line("{\"root\":\"-\"}");
            return;
        }
        FEngineInstance* Root = (FEngineInstance*)(Created);
        Host.Engine->UpdateInstance(Root, 0, nullptr);
        std::vector<unsigned long> Playbacks;
        const std::vector<std::string> Ops = Split(Line, ' ');
        for (size_t Op = 0; Op < Ops.size(); ++Op)
        {
            const std::vector<std::string> F = Split(Ops[Op], ':');
            if (F[0] == "p")
            {
                const Bytes Name = Unhex(F[1]);
                const unsigned long Flags = strtoul(F[2].c_str(), nullptr, 16);
                const unsigned long Id = Host.Anim->PlayScript(Script, Root, CStr(Name), nullptr, nullptr);
                Playbacks.push_back(Id);
                if (Id == 0xFFFFFFFF)
                {
                    Out.Line(Format("{\"op\":\"%u\",\"id\":\"-\"}", (unsigned int)Op));
                    continue;
                }
                const unsigned int Bound = BoundCount(Host, Id);
                const unsigned long Eval =
                    Host.Anim->EvalPlayback(Id, Flags, HexFloat(F[3]), HexFloat(F[4]), AsOpaqueDword(HexFloat(F[5])),
                                            AsOpaqueDword(HexFloat(F[6])), AsOpaqueDword(HexFloat(F[7])), nullptr);
                Out.Line(Format("{\"op\":\"%u\",\"id\":\"%lx\",\"bound\":\"%u\",\"eval\":\"%08lx\"}", (unsigned int)Op,
                                Id, Bound, Eval));
            }
            else if (F[0] == "w" || F[0] == "a")
            {
                if (F[0] == "w")
                    Host.Anim->MixerWalk((unsigned long)((uintptr_t)Root), HexFloat(F[1]));
                else
                    Host.Anim->MixerWalkAll(HexFloat(F[1]));
                Host.Engine->UpdateInstance(Root, 0, nullptr);
                ScenarioPose(Host, Root, (unsigned int)Op, Out);
            }
            else
            {
                const size_t N = strtoul(F[1].c_str(), nullptr, 10);
                if (N < Playbacks.size() && Playbacks[N] != 0xFFFFFFFF)
                {
                    if (F[0] == "s")
                        Host.Anim->StopPlayback(Playbacks[N]);
                    else
                        Host.Anim->DestroyScript((void*)((uintptr_t)Playbacks[N]));
                }
                Out.Line(Format("{\"op\":\"%u\",\"done\":\"%s\"}", (unsigned int)Op, Ops[Op].c_str()));
            }
        }
        for (size_t Index = 0; Index < Playbacks.size(); ++Index)
        {
            if (Playbacks[Index] != 0xFFFFFFFF)
                Host.Anim->DestroyScript((void*)((uintptr_t)Playbacks[Index]));
        }
        Host.Engine->DestroyInstance(Root);
    }

    struct FConnection
    {
        const char* ParentFile;
        const char* ParentHardpoint;
        const char* ChildFile;
        const char* ChildHardpoint;
    };

    // CONNECTIONS in oracle_scene.rs.
    const FConnection Connections[] = {
        {"scene/ship.cmp", "HpMount", "scene/simple.3db", "HpA"},
        {"scene/ship.cmp", "HpTurret01", "scene/simple.3db", "HpB"},
        {"scene/simple.3db", "HpB", "scene/ship.cmp", "HpSlide"},
        {"scene/simple.3db", "HpA", "scene/ship.cmp", "HpOdd"},
        {"scene/simple.3db", "HpZ", "scene/simple.3db", "HpA"},
        {"scene/simple.3db", "HpA", "scene/plain.3db", "HpA"},
    };

    FArchetypeHandle* Top(FSceneHost& Host, std::map<std::string, FArchetypeHandle*>& Tops, const std::string& Archive)
    {
        std::map<std::string, FArchetypeHandle*>::iterator It = Tops.find(Archive);
        if (It != Tops.end())
            return It->second;
        FArchetypeHandle* Handle = LoadFixture(Host, Archive);
        Tops[Archive] = Handle;
        return Handle;
    }

    // scene_eval: joint parameters, scripts through the mixer, hardpoint
    // connections.
    void SceneEval()
    {
        Suite Out("scene_eval.jsonl");
        FSceneHost Host;
        if (!MakeHost(Host))
            return;
        std::map<std::string, FArchetypeHandle*> Tops;
        for (const std::string& Line : InputLines("scene/joints.list"))
        {
            const std::vector<std::string> F = Split(Line, '|');
            FArchetypeHandle* Handle = Top(Host, Tops, F[0]);
            const unsigned long Index = strtoul(F[1].c_str(), nullptr, 10);
            const std::vector<std::string> Words = Split(F[2], ',');
            float Values[7];
            for (size_t Word = 0; Word < 7; ++Word)
                Values[Word] = HexFloat(Words[Word]);
            Out.Line("{\"joints\":\"" + Line + "\"}");
            void* Created = Handle == nullptr ? Invalid : Host.Engine->CreateInstance(Handle, nullptr);
            if (!Valid(Created))
            {
                Out.Line("{\"root\":\"-\"}");
                continue;
            }
            FEngineInstance* Root = (FEngineInstance*)(Created);
            Host.Engine->UpdateInstance(Root, 0, nullptr);
            ScenarioPose(Host, Root, 0, Out);
            const std::vector<FEngineInstance*> Instances = InstanceOrder(Host, Root);
            std::string Set;
            for (size_t Inst = 0; Inst < Instances.size(); ++Inst)
            {
                if (Inst != 0)
                    Set += ",";
                Set += Host.Engine->SetJointParams(Instances[Inst], Index, Values) != 0 ? "1" : "0";
            }
            Out.Line("{\"set\":\"" + Set + "\"}");
            Host.Engine->UpdateInstance(Root, 0, nullptr);
            ScenarioPose(Host, Root, 1, Out);
            Host.Engine->DestroyInstance(Root);
        }

        FArchetypeHandle* AnimTop = Top(Host, Tops, "scene/anim.cmp");
        IFileSystem* ScriptFs = OpenArchive("scene/anim.cmp");
        void* Script = ScriptFs == nullptr ? Invalid : Host.Anim->OpenScript(ScriptFs);
        if (ScriptFs != nullptr)
            ScriptFs->Release();
        if (!Valid(Script))
        {
            fprintf(stderr, "scene: scene/anim.cmp has no script\n");
            GFailed = true;
            return;
        }
        for (const std::string& Line : InputLines("scene/play.list"))
            PlayScenario(Host, Script, AnimTop, Line, Out);

        for (const FConnection& C : Connections)
        {
            FArchetypeHandle* ParentTop = Top(Host, Tops, C.ParentFile);
            FArchetypeHandle* ChildTop = Top(Host, Tops, C.ChildFile);
            void* A = ParentTop == nullptr ? Invalid : Host.Engine->CreateInstance(ParentTop, nullptr);
            void* B = ChildTop == nullptr ? Invalid : Host.Engine->CreateInstance(ChildTop, nullptr);
            FHardpointKey KeyA;
            KeyA.Name = C.ParentHardpoint;
            KeyA.Crc = NEURONCOMPONENTS_CRC::GetCRC32(C.ParentHardpoint);
            FHardpointKey KeyB;
            KeyB.Name = C.ChildHardpoint;
            KeyB.Crc = NEURONCOMPONENTS_CRC::GetCRC32(C.ChildHardpoint);
            const unsigned long Status =
                static_cast<IHardpoint*>(Host.Hardpoints)->ConnectHardpoints(A, &KeyA, B, &KeyB);
            const unsigned long* Relative = Host.Engine->GetInstanceRelative(B);
            std::string Type = "-";
            std::string Values = "-";
            if (Relative != nullptr)
            {
                Type = Format("%08lx", Relative[0]);
                Values = FloatList((const float*)(Relative + 1), G_EngineJointPayloadDwords - 1);
            }
            const float* World = Host.Engine->GetInstanceTransform(B);
            Out.Line(Format("{\"connect\":\"%s|%s|%s|%s\",\"status\":\"%lu\",\"type\":\"%s\",\"values\":\"%s\",\"world\":\"",
                            C.ParentFile, C.ParentHardpoint, C.ChildFile, C.ChildHardpoint, Status, Type.c_str(),
                            Values.c_str()) +
                     FloatList(World, 12) + "\"}");
            if (Valid(B))
            {
                Host.Engine->DestroyJoint(A, B);
                Host.Engine->DestroyInstance(B);
            }
            if (Valid(A))
                Host.Engine->DestroyInstance(A);
        }
    }

    bool SceneFile(const std::string& File)
    {
        if (File.size() < 4)
            return false;
        const char* Extension = File.c_str() + File.size() - 4;
        return _stricmp(Extension, ".cmp") == 0 || _stricmp(Extension, ".3db") == 0 || _stricmp(Extension, ".dfm") == 0;
    }

    // Whether every track of an animation has a loaded channel with frames
    // whose flags name a typed-channel class (playable in oracle_scene.rs).
    bool Playable(const FAnimNameEntry& Entry)
    {
        for (size_t Index = 0; Index < Entry.Tracks.size(); ++Index)
        {
            const FChannelArchetypeObject* Channel = Entry.Tracks[Index].Channel;
            if (Channel == nullptr || Channel->Frames == nullptr || Channel->FrameCount == 0)
                return false;
            const unsigned long Flags = Channel->Flags;
            if (Flags != 1 && Flags != 2 && Flags != 4 && Flags != 6 && Flags != 8)
                return false;
        }
        return true;
    }

    // Whether any channel of the script holds the shared unpacking buffer
    // (see oracle_anim.cpp): such a script is never released.
    bool HoldsPool(FAnimScript* Script)
    {
        for (FAnimNameMap::iterator It = Script->Names.begin(); It != Script->Names.end(); ++It)
        {
            const std::vector<FAnimTrack>& Tracks = It->second.Tracks;
            for (size_t Index = 0; Index < Tracks.size(); ++Index)
            {
                const FChannelArchetypeObject* Channel = Tracks[Index].Channel;
                if (Channel != nullptr && Channel->Frames != nullptr && (Channel->Flags & PackedMask) != 0)
                    return true;
            }
        }
        return false;
    }

    struct FPass
    {
        unsigned long Flags;
        float Start;
        float Steps[5];
        unsigned int Count;
    };

    // PASSES in oracle_scene.rs.
    const FPass Passes[2] = {
        {0, 0.0f, {0.0f, 0.25f, 0.5f, 1.0f, 2.5f}, 5},
        {3, -1.0f, {0.0f, 0.4f, 1.3f, 3.7f, 0.0f}, 4},
    };

    void PoseLines(FSceneHost& Host, FEngineInstance* Root, const std::string& Prefix, Suite& Out)
    {
        const std::vector<FEngineInstance*> Instances = InstanceOrder(Host, Root);
        for (size_t Index = 0; Index < Instances.size(); ++Index)
        {
            FEngineInstance* Instance = Instances[Index];
            const float* World = Host.Engine->GetInstanceTransform(Instance);
            Out.Line("{" + Prefix + Format("\"instance\":\"%u\",\"state\":\"", (unsigned int)Index) +
                     FloatList(Instance->JointState, 7) + "\",\"local\":\"" + FloatList(Instance->Matrix, 12) +
                     "\",\"world\":\"" + FloatList(World, 12) + "\"}");
        }
    }

    // script_lines in oracle_scene.rs.
    void ScriptLines(FSceneHost& Host, FArchetypeHandle* Top, FAnimScript* Script, Suite& Out)
    {
        for (FAnimNameMap::iterator It = Script->Names.begin(); It != Script->Names.end(); ++It)
        {
            const FAnimNameEntry& Entry = It->second;
            const std::string Head =
                Format("{\"anim\":\"%08lx\",\"name\":\"%s\"", It->first, HexC(Entry.Name).c_str());
            if (!Playable(Entry))
            {
                Out.Line(Head + ",\"status\":\"skip\"}");
                continue;
            }
            Out.Line(Head + ",\"status\":\"play\"}");
            for (unsigned int Pass = 0; Pass < 2; ++Pass)
            {
                void* Created = Host.Engine->CreateInstance(Top, nullptr);
                if (!Valid(Created))
                {
                    Out.Line(Format("{\"pass\":\"%u\",\"root\":\"-\"}", Pass));
                    continue;
                }
                FEngineInstance* Root = (FEngineInstance*)(Created);
                Host.Engine->UpdateInstance(Root, 0, nullptr);
                const unsigned long Id = Host.Anim->PlayScript(Script, Root, Entry.Name, nullptr, nullptr);
                if (Id == 0xFFFFFFFF)
                {
                    Out.Line(Format("{\"pass\":\"%u\",\"id\":\"-\"}", Pass));
                    Host.Engine->DestroyInstance(Root);
                    continue;
                }
                const unsigned int Bound = BoundCount(Host, Id);
                const unsigned long Eval = Host.Anim->EvalPlayback(Id, Passes[Pass].Flags, Passes[Pass].Start, 1.0f,
                                                                   AsOpaqueDword(0.0f), AsOpaqueDword(1.0f),
                                                                   AsOpaqueDword(1.0f), nullptr);
                Out.Line(Format("{\"pass\":\"%u\",\"bound\":\"%u\",\"eval\":\"%08lx\"}", Pass, Bound, Eval));
                for (unsigned int Step = 0; Step < Passes[Pass].Count; ++Step)
                {
                    Host.Anim->MixerWalk((unsigned long)((uintptr_t)Root), Passes[Pass].Steps[Step]);
                    Host.Engine->UpdateInstance(Root, 0, nullptr);
                    PoseLines(Host, Root, Format("\"pass\":\"%u\",\"step\":\"%u\",", Pass, Step), Out);
                }
                Host.Anim->DestroyScript((void*)((uintptr_t)Id));
                Host.Engine->DestroyInstance(Root);
            }
        }
    }
}

void RunSceneSuites()
{
    SceneLoad();
    SceneSample();
    SceneEval();
}

void RunSceneDataDump(const std::vector<std::string>& Files)
{
    FSceneHost Host;
    if (!MakeHost(Host))
        return;
    {
        Suite List(GOutputDir + "\\scene\\files.txt");
        unsigned int Count = 0;
        for (const std::string& File : Files)
        {
            if (!SceneFile(File) || FileMagic(File) != "UTF ")
                continue;
            IFileSystem* Fs = OpenArchive(File);
            if (Fs == nullptr)
                continue;
            List.Line(File);
            ++Count;
            Suite Out(DataOutputFile("scene", File));
            FArchetypeHandle* Handle = CreateArchetype(Host, File, Fs);
            if (Handle == nullptr)
            {
                Out.Line(Format("{\"file\":\"%s\",\"archetype\":\"0\"}", File.c_str()));
            }
            else
            {
                Out.Line(Format("{\"file\":\"%s\",\"archetype\":\"1\"}", File.c_str()));
                ArchetypeLines(Host, Handle, Out);
                Host.Engine->ReleaseHandle(Handle);
            }
            Fs->Release();
        }
        printf("dumped %u scene archetypes\n", Count);
    }

    FSceneHost EvalHost;
    if (!MakeHost(EvalHost))
        return;
    Suite List(GOutputDir + "\\scene_eval\\files.txt");
    unsigned int Count = 0;
    for (const std::string& File : Files)
    {
        if (!SceneFile(File) || FileMagic(File) != "UTF ")
            continue;
        IFileSystem* ScriptFs = OpenArchive(File);
        if (ScriptFs == nullptr)
            continue;
        void* Script = EvalHost.Anim->OpenScript(ScriptFs);
        ScriptFs->Release();
        if (!Valid(Script))
            continue;
        FAnimScript* Typed = (FAnimScript*)(Script);
        IFileSystem* Fs = OpenArchive(File);
        FArchetypeHandle* Handle = Fs == nullptr ? nullptr : CreateArchetype(EvalHost, File, Fs);
        if (Fs != nullptr)
            Fs->Release();
        if (Handle != nullptr)
        {
            List.Line(File);
            ++Count;
            Suite Out(DataOutputFile("scene_eval", File));
            Out.Line(Format("{\"file\":\"%s\",\"entries\":\"%u\"}", File.c_str(), (unsigned int)Typed->Names.size()));
            ScriptLines(EvalHost, Handle, Typed, Out);
            EvalHost.Engine->ReleaseHandle(Handle);
        }
        if (!HoldsPool(Typed))
            EvalHost.Anim->CloseScript(Script);
    }
    printf("dumped %u scene evaluations\n", Count);
}
