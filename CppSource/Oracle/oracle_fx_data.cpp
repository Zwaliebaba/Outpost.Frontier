// Oracle suite and data dump of work package P1.7, effect data: the node
// libraries and effect libraries of .ale files, loaded by particlefx.dll.
// The Rust test crates/neuron-core/tests/oracle_fx_data.rs writes the same
// lines through neuron_core::fx_data; the synthetic inputs come from
// crates/neuron-core/tests/fixtures/oracle/make_fx_data_fixtures.py.
//
// particlefx.dll exports nothing but DllMain, which registers its
// components with the component manager, so it is loaded at run time (as
// oracle_math.cpp loads x86math.dll). Each file is opened as a UTF file
// system through the component manager (the UTF factory is registered by
// the ReadFile source compiled into oracle_vfs.cpp, whose suites and dump
// run first) and loaded as the client loads an .ale
// (Client/vis_effect_arch.cpp): a fresh "FxNodeLibrary" loads the node
// library, then a fresh "FxEffectLibrary" the effect library, through the
// same file system. The libraries are then read back through their own
// interfaces: every node's property set by the rows of its class table
// (EnumerateProps) and a fixed list of probe hashes, every effect's
// records. The private interfaces (IGenericFx, IFxNodePropertySet, the
// ramps) come from particlefx_types.h; the billboard sets' texture CRC,
// which no getter hands out, is read from FAppearanceRampDescriptor.
//
// The effect library hashes names through the IParticleFx it is bound to.
// The real ParticleFx component needs a render pipeline, so a stand-in
// that forwards HashNodeName to the node library (the same function) is
// bound instead; LoadAle calls nothing else on it.

#include "oracle_support.h"

#include <string>
#include <vector>

#include "neuroncomponents.h"
#include "readfile.h"
#include "../particlefx/particlefx_types.h"

using namespace oracle;
using namespace ParticleFxImpl;

namespace
{
    // A value text longer than this is written as fnv64:<hash>.
    const size_t ValueLimit = 4096;

    const char* const BillboardClasses[] = {
        "FxBasicAppearance", "FxRectAppearance", "FxPerpAppearance",
        "FxOrientedAppearance", "FLDustAppearance", "FLBeamAppearance",
    };

    struct FProbe
    {
        unsigned long Hash;
        unsigned long Kind;
    };

    // Hashes some class owns that its row table does not list.
    const FProbe Probes[] = {
        {0x0C53B008, 0x200}, {0x0BA0B3BB, 0x105}, {0xF0004D58, 0x105}, {0xE462B789, 0x202},
        {0x061AAF28, 0x202}, {0x16FF3474, 0x202}, {0x03503B61, 0x1},   {0x0ABE0402, 0x1},
    };

    // The IParticleFx the effect library is bound to: only HashNodeName is
    // reached while loading.
    class FHashOnlyParticleFx final : public IParticleFx
    {
    public:
        explicit FHashOnlyParticleFx(IFxNodeLibrary* InLibrary) : Library(InLibrary) {}

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API LoadNodeLibrary(IFileSystem*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SaveNodeLibrary(IFileSystem*) override { return GR_GENERIC; }
        unsigned long NEURONCOMPONENTS_API HashNodeName(const char* Name) override
        {
            return Library->HashNodeName(Name);
        }
        GENRESULT NEURONCOMPONENTS_API NodeLibSlot6(const char*, INeuronComponent*, unsigned long*) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API NodeLibSlot7(unsigned long) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API NodeLibSlot8(FNodeLibraryEnumCallback, void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ClearNodeLibrary() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API NodeLibSlot10(void*, void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API NodeLibSlot11(unsigned long, INeuronComponent**) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API NodeLibSlot12(unsigned long, INeuronComponent**) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CreateVertexBuffer() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ReleaseVertexBuffer() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot15(void*, void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot16(void*, void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DrawNodes(void*, void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDrawCookie() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot19(const char*, void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API AttachNode(void*, void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot21(void*, void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DetachNode(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SubmitNode(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SubmitChildNodes(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetRootNode(void**) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API FindNode(void*, void*, void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetUpdateCallback(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetFilterCallback(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API BeginDraw() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API EndDraw() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetUseMaterialBatcher(unsigned char) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetUseMaterialBatcher() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API EnqueueAppearance(void*, void*) override { return GR_GENERIC; }
        void* NEURONCOMPONENTS_API GetVertexBufferHandle() override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API SetDrawFlags(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CopyCounters(void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Initialize(AGGDESC*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Slot38() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetFlagA() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetFlagB() override { return GR_GENERIC; }

    private:
        IFxNodeLibrary* Library;
    };

    bool LoadParticleFx()
    {
        static int State = 0;
        if (State == 0)
        {
            State = LoadLibraryA("particlefx.dll") != nullptr ? 1 : 2;
            if (State == 2)
            {
                fprintf(stderr, "cannot load particlefx.dll (%lu)\n", GetLastError());
                GFailed = true;
            }
        }
        return State == 1;
    }

    std::string Bits(float Value)
    {
        return Format("%08x", FloatBits(Value));
    }

    std::string NameText(const char* Name)
    {
        return Name == nullptr ? std::string("-") : HexC(Name);
    }

    std::string SingleText(IRampSingle* Ramp)
    {
        std::string Text = Format("m%lx[", Ramp->GetMode());
        const int Count = Ramp->GetKeyCount();
        for (int Index = 0; Index < Count; ++Index)
        {
            float Time = 0.0f;
            float Value = 0.0f;
            Ramp->GetKeyTime(Index, &Time);
            Ramp->GetKeyValue(Index, &Value);
            if (Index != 0)
                Text += ",";
            Text += Bits(Time) + "=" + Bits(Value);
        }
        return Text + "]";
    }

    std::string ColorText(IRampColor* Ramp)
    {
        std::string Text = Format("m%lx[", Ramp->GetMode());
        const int Count = Ramp->GetKeyCount();
        for (int Index = 0; Index < Count; ++Index)
        {
            float Time = 0.0f;
            unsigned long Packed = 0;
            Ramp->GetKeyTime(Index, &Time);
            Ramp->GetKeyPacked(Index, &Packed);
            if (Index != 0)
                Text += ",";
            Text += Bits(Time) + Format("=%08lx", Packed);
        }
        return Text + "]";
    }

    std::string AnimatedText(IFxAnimatedSingle* Value)
    {
        float Weight = 0.0f;
        unsigned long LoopType = 0;
        Value->GetWeight(&Weight);
        Value->GetLoopType(&LoopType);
        std::string Text = "w" + Bits(Weight) + Format("l%lx[", LoopType);
        const int Count = Value->GetKeyCount();
        for (int Index = 0; Index < Count; ++Index)
        {
            float Time = 0.0f;
            float Triple[3] = {0.0f, 0.0f, 0.0f};
            Value->GetKeyTime(Index, &Time);
            Value->GetKeyValue(Index, Triple);
            if (Index != 0)
                Text += ",";
            Text += Bits(Time) + "=" + Bits(Triple[0]) + "/" + Bits(Triple[1]) + "/" + Bits(Triple[2]);
        }
        return Text + "]";
    }

    // A nested scalar ramp; Animated selects the child interface.
    std::string NestedSingleText(INestedSingleRamp* Ramp, bool bAnimated)
    {
        std::string Text = Format("m%lx{", Ramp->GetMode());
        const int Count = Ramp->GetKeyCount();
        for (int Index = 0; Index < Count; ++Index)
        {
            float Time = 0.0f;
            INeuronComponent* Child = nullptr;
            Ramp->GetKeyTime(Index, &Time);
            Ramp->GetKeyValue(Index, &Child);
            if (Index != 0)
                Text += ";";
            Text += Bits(Time) + ":";
            if (bAnimated)
                Text += AnimatedText(static_cast<IFxAnimatedSingle*>(Child));
            else
                Text += SingleText(static_cast<IRampSingle*>(Child));
            if (Child != nullptr)
                Child->Release();
        }
        return Text + "}";
    }

    std::string ColorRampText(IRampColorRamp* Ramp)
    {
        std::string Text = Format("m%lx{", Ramp->GetMode());
        const int Count = Ramp->GetKeyCount();
        for (int Index = 0; Index < Count; ++Index)
        {
            float Time = 0.0f;
            IRampColor* Child = nullptr;
            Ramp->GetKeyTime(Index, &Time);
            Ramp->GetKeyValue(Index, &Child);
            if (Index != 0)
                Text += ";";
            Text += Bits(Time) + ":" + ColorText(Child);
            if (Child != nullptr)
                Child->Release();
        }
        return Text + "}";
    }

    std::string TransformText(IAnimatedTransform* Transform)
    {
        unsigned long A = 0;
        unsigned long B = 0;
        unsigned long C = 0;
        unsigned long Flags = 0;
        Transform->GetPackedModes(&A, &B, &C);
        Transform->GetChannelFlags(&Flags);
        std::string Text = Format("p%lx,%lx,%lxf%lx(", A, B, C, Flags);
        for (unsigned long Channel = 0; Channel < 9; ++Channel)
        {
            INeuronComponent* Child = nullptr;
            Transform->GetChild(Channel, &Child);
            if (Channel != 0)
                Text += "|";
            Text += NestedSingleText(static_cast<INestedSingleRamp*>(Child), true);
            if (Child != nullptr)
                Child->Release();
        }
        return Text + ")";
    }

    // The getter of Kind for Hash: its result into Result, the value text returned.
    std::string Property(IFxNodePropertySet* Set, unsigned long Hash, unsigned long Kind, GENRESULT& Result)
    {
        std::string Text;
        switch (Kind)
        {
        case 0x1:
        {
            unsigned char Value = 0;
            Result = Set->GetPropBool(Hash, &Value);
            if (Result == GR_OK)
                Text = Format("%u", (unsigned int)Value);
            break;
        }
        case 0x2:
        {
            unsigned long Value = 0;
            Result = Set->GetPropInt(Hash, &Value);
            if (Result == GR_OK)
                Text = Format("%08lx", Value);
            break;
        }
        case 0x3:
        {
            float Value = 0.0f;
            Result = Set->GetPropFloat(Hash, &Value);
            if (Result == GR_OK)
                Text = Bits(Value);
            break;
        }
        case 0x103:
        {
            const char* Value = nullptr;
            Result = Set->GetPropName(Hash, &Value);
            if (Result == GR_OK)
                Text = NameText(Value);
            break;
        }
        case 0x104:
        {
            unsigned long Value[2] = {0, 0};
            Result = Set->GetPropPair(Hash, Value);
            if (Result == GR_OK)
                Text = Format("%08lx,%08lx", Value[0], Value[1]);
            break;
        }
        case 0x105:
        {
            IAnimatedTransform* Value = nullptr;
            Result = Set->GetPropTransform(Hash, &Value);
            if (Result == GR_OK)
            {
                Text = TransformText(Value);
                Value->Release();
            }
            break;
        }
        case 0x200:
        case 0x202:
        {
            INestedSingleRamp* Value = nullptr;
            Result = Kind == 0x200 ? Set->GetPropSingleRampRamp(Hash, &Value)
                                   : Set->GetPropAnimatedSingleRamp(Hash, &Value);
            if (Result == GR_OK)
            {
                Text = NestedSingleText(Value, Kind == 0x202);
                Value->Release();
            }
            break;
        }
        case 0x201:
        {
            IRampColorRamp* Value = nullptr;
            Result = Set->GetPropColorRampRamp(Hash, &Value);
            if (Result == GR_OK)
            {
                Text = ColorRampText(Value);
                Value->Release();
            }
            break;
        }
        default:
            Result = (GENRESULT)0xFFFFFFF8;
            break;
        }
        if (Result != GR_OK)
            return std::string();
        if (Text.size() > ValueLimit)
            return "fnv64:" + Fnv64(Text.data(), Text.size());
        return Text;
    }

    struct FRowContext
    {
        IFxNodePropertySet* Set;
        Suite* Out;
    };

    int __cdecl DumpRow(const FNodePropertyRow* Row, void* ContextRaw)
    {
        FRowContext* Context = static_cast<FRowContext*>(ContextRaw);
        GENRESULT Result = GR_OK;
        const std::string Value = Property(Context->Set, Row->Hash, Row->Kind, Result);
        Context->Out->Line(Format("{\"p\":\"%s\",\"h\":\"%08lx\",\"k\":\"%lx\",\"r\":\"%ld\",\"v\":\"", Row->Name,
                                  Row->Hash, Row->Kind, Result) +
                           Value + "\"}");
        return 0;
    }

    int __cdecl CollectNode(INeuronComponent* Node, void* Context)
    {
        static_cast<std::vector<INeuronComponent*>*>(Context)->push_back(Node);
        return 0;
    }

    int __cdecl CollectEffect(IFxEffectArchetype* Archetype, void* Context)
    {
        static_cast<std::vector<IFxEffectArchetype*>*>(Context)->push_back(Archetype);
        return 0;
    }

    bool IsBillboard(const char* ClassName)
    {
        if (ClassName == nullptr)
            return false;
        for (const char* Name : BillboardClasses)
        {
            if (strcmp(Name, ClassName) == 0)
                return true;
        }
        return false;
    }

    void DumpNode(IFxNodeLibrary* Library, INeuronComponent* Node, size_t Index, Suite& Out)
    {
        IGenericFx* const Fx = static_cast<IGenericFx*>(Node);
        unsigned long Hash = 0;
        Fx->GetInstanceNameHash(&Hash);
        IFxNodePropertySet* Set = nullptr;
        Fx->GetPropertySet(&Set);
        const char* ClassName = nullptr;
        Set->GetPropName(0xF54EF296, &ClassName);
        std::string Line = Format("{\"node\":\"%u\",\"hash\":\"%08lx\"", (unsigned int)Index, Hash);
        if (IsBillboard(ClassName))
        {
            const FAppearanceRampDescriptor* Descriptor =
                static_cast<const FAppearanceRampDescriptor*>(static_cast<FFxNodePropertySet*>(Set));
            Line += Format(",\"texcrc\":\"%08lx\"", Descriptor->TextureCrc);
        }
        Out.Line(Line + "}");
        FRowContext Context = {Set, &Out};
        Set->EnumerateProps(DumpRow, &Context);
        for (const FProbe& Probe : Probes)
        {
            GENRESULT Result = GR_OK;
            const std::string Value = Property(Set, Probe.Hash, Probe.Kind, Result);
            Out.Line(Format("{\"q\":\"%08lx\",\"k\":\"%lx\",\"r\":\"%ld\",\"v\":\"", Probe.Hash, Probe.Kind, Result) +
                     Value + "\"}");
        }
        Set->Release();
        (void)Library;
    }

    void DumpEffect(IFxNodeLibrary* Library, IFxEffectArchetype* Effect, size_t Index, Suite& Out)
    {
        unsigned char Header[16];
        Effect->GetHeader16(Header);
        unsigned long Words[4];
        memcpy(Words, Header, sizeof(Words));
        const char* Name = Effect->GetName();
        Out.Line(Format("{\"effect\":\"%u\",\"name\":\"", (unsigned int)Index) + HexC(Name) +
                 Format("\",\"hash\":\"%08lx\",\"unknown\":\"%08lx,%08lx,%08lx,%08lx\"}", Library->HashNodeName(Name),
                        Words[0], Words[1], Words[2], Words[3]));
        const unsigned long Nodes = Effect->GetRec16Count();
        for (unsigned long Rec = 0; Rec < Nodes; ++Rec)
        {
            FAleRec16 Value = {};
            Effect->GetRec16(Rec, &Value);
            Out.Line(Format("{\"ref\":\"%lu\",\"kind\":\"%08lx\",\"node\":\"%08lx\",\"parent\":\"%08lx\",\"id\":\"%08lx\"}",
                            Rec, Value.A, Value.B, Value.C, Value.D));
        }
        const unsigned long Pairs = Effect->GetRec8Count();
        for (unsigned long Rec = 0; Rec < Pairs; ++Rec)
        {
            FAleRec8 Value = {};
            Effect->GetRec8(Rec, &Value);
            Out.Line(Format("{\"pair\":\"%lu\",\"a\":\"%08lx\",\"b\":\"%08lx\"}", Rec, Value.A, Value.B));
        }
    }

    template <typename T>
    T* Create(const char* Name)
    {
        AGGDESC Desc = {8, Name};
        T* Out = nullptr;
        if (NEURONCOMPONENTS_Acquire()->CreateInstance(&Desc, (void**)&Out) != GR_OK || Out == nullptr)
        {
            fprintf(stderr, "cannot create %s\n", Name);
            GFailed = true;
            return nullptr;
        }
        return Out;
    }

    // Loads Relative (an .ale under the current directory) as the client
    // does and dumps both libraries. Returns whether both loads succeeded.
    bool DumpAle(const std::string& Relative, Suite& Out)
    {
        if (!LoadParticleFx())
            return false;
        FFileSystemDesc Desc;
        memset(&Desc, 0, sizeof(Desc));
        Desc.Size = 0x34;
        Desc.ComponentName = "FileSystem";
        Desc.Implementation = "UTF";
        Desc.Path = Relative.c_str();
        Desc.Access = 0x80000000;
        Desc.ShareMode = 1;
        Desc.Creation = 3;
        Desc.Flags = 0x08000080;
        Desc.Field30 = 0xFFFFFFFF;
        IFileSystem* Fs = nullptr;
        if (NEURONCOMPONENTS_Acquire()->CreateInstance((AGGDESC*)&Desc, (void**)&Fs) != GR_OK || Fs == nullptr)
        {
            Out.Line("{\"file\":\"" + Relative + "\",\"open\":\"fail\"}");
            return false;
        }
        IFxNodeLibrary* Nodes = Create<IFxNodeLibrary>("FxNodeLibrary");
        IFxEffectLibrary* Effects = Create<IFxEffectLibrary>("FxEffectLibrary");
        if (Nodes == nullptr || Effects == nullptr)
            return false;
        FHashOnlyParticleFx Hasher(Nodes);
        Effects->BindParticleFx(&Hasher, Nodes);
        const GENRESULT NodesResult = Nodes->LoadNodeLibrary(Fs);
        const GENRESULT EffectsResult = Effects->LoadAle(Fs);
        Out.Line(Format("{\"file\":\"%s\",\"nodes\":\"%ld\",\"effects\":\"%ld\"}", Relative.c_str(), NodesResult,
                        EffectsResult));
        std::vector<INeuronComponent*> NodeList;
        Nodes->NodeLibSlot8(CollectNode, &NodeList);
        for (size_t Index = 0; Index < NodeList.size(); ++Index)
            DumpNode(Nodes, NodeList[Index], Index, Out);
        std::vector<IFxEffectArchetype*> EffectList;
        Effects->EnumerateArchetypes(CollectEffect, &EffectList);
        for (size_t Index = 0; Index < EffectList.size(); ++Index)
            DumpEffect(Nodes, EffectList[Index], Index, Out);
        Effects->ClearEffects();
        Effects->BindParticleFx(nullptr, nullptr);
        Effects->Release();
        Nodes->Release();
        Fs->Release();
        return NodesResult == GR_OK && EffectsResult == GR_OK;
    }

    bool IsAle(const std::string& File)
    {
        if (File.size() < 4)
            return false;
        std::string Ext = File.substr(File.size() - 4);
        for (char& Ch : Ext)
            Ch = (char)tolower((unsigned char)Ch);
        return Ext == ".ale" && FileMagic(File) == "UTF ";
    }
}

void RunFxDataSuites()
{
    Suite Out("fx_data.jsonl");
    for (const std::string& File : InputLines("fx/files.list"))
        DumpAle("fx/" + File, Out);
}

void RunFxDataDump(const std::vector<std::string>& Files)
{
    Suite List(GOutputDir + "\\fx\\files.txt");
    unsigned int Count = 0;
    unsigned int Loaded = 0;
    for (const std::string& File : Files)
    {
        if (!IsAle(File))
            continue;
        List.Line(File);
        ++Count;
        Suite Out(DataOutputFile("fx", File));
        if (DumpAle(File, Out))
            ++Loaded;
    }
    printf("dumped %u effect files, %u loaded both libraries\n", Count, Loaded);
}
