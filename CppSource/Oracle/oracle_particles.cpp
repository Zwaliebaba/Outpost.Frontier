// P4.8 (first increment): the particle simulation of particlefx.dll, compiled in from the
// reference's own sources (particlefx_*.cpp but particlefx.cpp, whose DllMain is replaced by
// the registration below), against a recording texture library and an in-memory file system.
// The Rust side is crates/neuron-client/tests/support/particles.rs with the same contract; the
// inputs are crates/neuron-client/tests/fixtures/oracle/particle_{ramp,sim}.list from
// make_particle_fixtures.py.
//
// The libraries load through FAleChunk from a file system holding the two leaves of an .ale
// (ParticleFxNodeLibrary/ParticleFxNodeLibrary and ALEffectLib/ALEffectLib) as the list gives
// their streams: OpenNode enters a directory of the root or leaves with "..", OpenByDesc opens
// the leaf of the directory entered, Read copies what is left.
//
// The ParticleFx component is made by its factory, aggregated under a stand-in whose
// QueryInterface hands out the recording texture library for ITextureLibraryA and an inert
// component for the other pipeline interfaces Initialize asks for (the simulation calls none).
//
// particle_ramp: per case one ParticleFx loads the node library; every node's property rows
// (EnumerateProps) and then the probes the row tables miss are asked for their ramp or
// transform, and each one found is evaluated at the times of the case in order:
//   kinds 0x200/0x202  Eval(outer, inner), the double's 16 hex digits
//   kind 0x201         EvalRgb then EvalPacked: "r.g.b.packed"
//   kind 0x105         Evaluate(outer bits, inner bits): the 12 floats joined by "."
// one line {"case","node","hash","kind","v"} per property, the results joined by ",".
//
// particle_sim: per case a fresh ParticleFx (node library) and FxEffectLibrary bound to it,
// every pool reallocated with 1000 items and the generator at state 0, counter 0. The
// operations (" ; "-separated) and their lines {"case","op",...}:
//   ale <nodes> <effects>   LoadNodeLibrary, LoadAle: "nodes","effects" results
//   pool <kind> <count>     AllocateAppearancePool of that pool
//   seed <state> <counter>  the generator's globals (decimal)
//   tex <crc> <frames>      a texture the fake library holds
//   effect <slot> <name>    a new "FxNode" parent and CreateEffect(parent, HashNameCrc(name)):
//                           "r", the record and root counts ("-" after a failure)
//   xform <slot> <i> <12f>  Slot3(i, xform): "r"
//   outer|age|lod <slot> <f>  Slot6 / Slot5 / Slot14
//   step <slot> <dt>        Slot7(0) then Slot8(dt)
//   stop <slot> <flags>     Slot13(flags)
//   done <slot> <flags>     Slot11(flags): "r"
//   release <slot>          the effect's Release, then the parent's
//   rand                    the generator: "s","c"
//   pools                   per pool "k","free","used": the used items oldest first
//   dump <slot>             per record a "node" line (base state, then the family's), and per
//                           particle of an appearance's list a "part" line
// The fake texture library writes {"case","ev",...} in call order:
//   FindTextureByCrc "find","crc","r" (0 for a texture the case added, record = its number
//   from 1; else -1)   CreatePlaybackGroup "play","rec","id" (ids from 1)
//   GetFrameCount "frames","rec","n"   ReleaseTextureRecord "trel","rec"
//   SetWrapMode "wrap","id","m"   GetFrame "frame","id","f","r": handle id*100+f, u0 =
//   0.125f*f, v0 0, u1 1, v1 0.5   AdvanceTime "adv","id","dt"   DecRefById "decref","id"
// Floats are the eight hex digits of the f32.

// The dumps below redefine private and protected; see there.
#define _ALLOW_KEYWORD_MACROS

#include "oracle_support.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <new>
#include <string>
#include <vector>

#include "luaprofile.h"
#include "neuroncomponents.h"
#include "particlefx.h"
#include "readfile.h"
#include "render/IRenderPipeline.h"
#include "render/shading.h"
#include "x86math.h"

// The dumps read the nodes' private data members, and nothing else of their private parts:
// MSVC's names of member functions carry their access, so every particlefx object is made by
// the component factories (in the particlefx sources' own translation units) and called through
// its interfaces. Every standard header they use is included above, before the keywords are
// redefined (and _ALLOW_KEYWORD_MACROS, at the top, quiets MSVC's check of that).
#define private public
#define protected public
#include "../particlefx/particlefx_types.h"
#include "../particlefx/particlefx_common.h"
#include "../particlefx/particlefx_genericfx.h"
#include "../particlefx/particlefx_emitters.h"
#include "../particlefx/particlefx_fields.h"
#include "../particlefx/particlefx_appearances.h"
#include "../particlefx/particlefx_effectobject.h"
#include "../particlefx/particlefx_effectlibrary.h"
#include "../particlefx/particlefx_update.h"
#include "../particlefx/particlefx_factory.h"
#undef private
#undef protected

using namespace oracle;
using namespace ParticleFxImpl;

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

    float HexFloat(const std::string& Text)
    {
        return FloatFromBits((unsigned int)strtoul(Text.c_str(), nullptr, 16));
    }

    std::string F(float Value)
    {
        return Format("%08x", FloatBits(Value));
    }

    std::string Floats(const float* Values, int Count, const char* Separator = ",")
    {
        std::string Text;
        for (int Index = 0; Index < Count; ++Index)
        {
            if (Index != 0)
                Text += Separator;
            Text += F(Values[Index]);
        }
        return Text;
    }

    // --- The in-memory file system ---------------------------------------------------------

    class FAleFs final : public IFileSystem
    {
    public:
        std::map<std::string, Bytes> Leaves;
        std::string Current;
        struct FOpen
        {
            const Bytes* Data;
            size_t Position;
        };
        std::vector<FOpen> Open;

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API CreateInstance(AGGDESC*, void**) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API CloseFile(unsigned long) override { return 1; }
        int NEURONCOMPONENTS_API Read(unsigned long Handle, void* Buffer, unsigned long Size,
                                      unsigned long* BytesRead, unsigned long) override
        {
            if (Handle >= Open.size())
                return 0;
            FOpen& File = Open[Handle];
            const size_t Left = File.Data->size() - File.Position;
            const size_t Count = Size < Left ? Size : Left;
            if (Count != 0)
                memcpy(Buffer, File.Data->data() + File.Position, Count);
            File.Position += Count;
            *BytesRead = (unsigned long)Count;
            return 1;
        }
        int NEURONCOMPONENTS_API Write(unsigned long, const void*, unsigned long, unsigned long*,
                                       const void*) override { return 0; }
        int NEURONCOMPONENTS_API RejectOperation1C(unsigned long, unsigned long, unsigned long,
                                                   unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API SeekFile(unsigned long, unsigned long, unsigned long*,
                                          unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API SetEndOfFile(unsigned long) override { return 0; }
        unsigned long NEURONCOMPONENTS_API GetSize(unsigned long, unsigned long*) override { return 0; }
        int NEURONCOMPONENTS_API ReturnOne2C(unsigned long, unsigned long, unsigned long, unsigned long,
                                             unsigned long) override { return 1; }
        int NEURONCOMPONENTS_API ReturnOne30(unsigned long, unsigned long, unsigned long, unsigned long,
                                             unsigned long) override { return 1; }
        int NEURONCOMPONENTS_API GetFileTimes(unsigned long, FILETIME*, FILETIME*, FILETIME*) override
        {
            return 0;
        }
        int NEURONCOMPONENTS_API RejectOperation38(unsigned long, unsigned long, unsigned long,
                                                   unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API CreateMapping(unsigned long, unsigned long, unsigned long, unsigned long,
                                               unsigned long, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API MapFileView(unsigned long, unsigned long, unsigned long, unsigned long,
                                             unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API UnmapFileView(unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API FindFirst(const char*, WIN32_FIND_DATAA*) override { return -1; }
        int NEURONCOMPONENTS_API FindNext(unsigned long, WIN32_FIND_DATAA*) override { return 0; }
        int NEURONCOMPONENTS_API FindClose(unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API RejectOperation54(const char*, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API RejectOperation58(unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API GetCurrentPath(unsigned long, char*) override { return 0; }
        int NEURONCOMPONENTS_API OpenNode(const char* Name) override
        {
            if (strcmp(Name, "..") == 0)
            {
                Current.clear();
                return 1;
            }
            if (Current.empty() && Leaves.count(Name) != 0)
            {
                Current = Name;
                return 1;
            }
            return 0;
        }
        int NEURONCOMPONENTS_API RejectOperation64(unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API RejectOperation68(unsigned long, unsigned long, unsigned long) override
        {
            return 0;
        }
        int NEURONCOMPONENTS_API RejectOperation6C(unsigned long, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API GetPathAttributes(const char*) override { return 0; }
        int NEURONCOMPONENTS_API RejectOperation74(unsigned long, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API GetFileSystemError() override { return 0; }
        int NEURONCOMPONENTS_API OpenByDesc(FFileSystemDesc* Desc) override
        {
            if (Current.empty() || Desc->Path == nullptr || Current != Desc->Path)
                return -1;
            FOpen File = {&Leaves[Current], 0};
            Open.push_back(File);
            return (int)Open.size() - 1;
        }
        int NEURONCOMPONENTS_API GetFilePosition(unsigned long, unsigned long*) override { return 0; }
        void NEURONCOMPONENTS_API GetFileSysPath(char* Out, unsigned long MaxChars) override
        {
            if (MaxChars != 0)
                Out[0] = 0;
        }
        int NEURONCOMPONENTS_API GetAccessFlags() override { return 0; }
        int NEURONCOMPONENTS_API GetParentFileSystem(IFileSystem** Out) override
        {
            *Out = nullptr;
            return 0;
        }
        int NEURONCOMPONENTS_API FailOperation90(unsigned long, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API FailOperation94(unsigned long, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API FailOperation98(unsigned long, unsigned long, unsigned long, unsigned long,
                                                 unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API FailOperation9C(unsigned long, unsigned long, unsigned long, unsigned long,
                                                 unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API InvokeLockedCallback(unsigned long, unsigned long, unsigned long) override
        {
            return 0;
        }
        int NEURONCOMPONENTS_API ResolvePath(char*, const char*, unsigned long) override { return 0; }
    };

    void FillFs(FAleFs& Fs, const std::string& NodesHex, const std::string& EffectsHex)
    {
        Bytes Nodes = Unhex(NodesHex);
        Nodes.pop_back();
        Bytes Effects = Unhex(EffectsHex);
        Effects.pop_back();
        Fs.Leaves["ParticleFxNodeLibrary"] = Nodes;
        Fs.Leaves["ALEffectLib"] = Effects;
    }

    // --- The recording texture library -------------------------------------------------------

    class FTexLib final : public ITextureLibrary2
    {
    public:
        Suite* Out = nullptr;
        std::string Head;
        std::vector<std::pair<unsigned long, int>> Entries; // CRC, frame count
        std::map<int, size_t> Playbacks;
        int NextId = 1;

        void Line(const std::string& Text) { Out->Line(Head + Text + "}"); }

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out2) override
        {
            *Out2 = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API SetGenericSlot(int, unsigned long) override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API GetGenericSlot(int, unsigned long*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LoadTextureGroup(void*, int*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LoadTextureLibrary(void*, const char*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GarbageCollect(int) override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API TickAllPlayback(int, float) override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API SetGroupName(int, const char*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetGroupName(int, char*, unsigned long) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API FindOrCreateByName(const char*, void**) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API FindTextureByCrc(void* Query, void** OutTex) override
        {
            const FFxTextureKey* Key = static_cast<const FFxTextureKey*>(Query);
            for (size_t Index = 0; Index < Entries.size(); ++Index)
            {
                if (Entries[Index].first != Key->Crc)
                    continue;
                *OutTex = reinterpret_cast<void*>(Index + 1);
                Line(Format("\"ev\":\"find\",\"crc\":\"%08lx\",\"r\":\"0\"", Key->Crc));
                return GR_OK;
            }
            Line(Format("\"ev\":\"find\",\"crc\":\"%08lx\",\"r\":\"-1\"", Key->Crc));
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API CreatePlaybackGroup(void* Record, int* OutId) override
        {
            const size_t Entry = reinterpret_cast<size_t>(Record);
            const int Id = NextId++;
            Playbacks[Id] = Entry - 1;
            *OutId = Id;
            Line(Format("\"ev\":\"play\",\"rec\":\"%u\",\"id\":\"%d\"", (unsigned)Entry, Id));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API ReleaseTextureRecord(void* Record) override
        {
            Line(Format("\"ev\":\"trel\",\"rec\":\"%u\"", (unsigned)reinterpret_cast<size_t>(Record)));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API AddRefById(int) override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API DecRefById(int Id) override
        {
            Line(Format("\"ev\":\"decref\",\"id\":\"%d\"", Id));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API CopyName(void*, char*, unsigned long) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetRecordName(void*, const char*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetSubTexture(void*, int, int) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetFrameHandle(void*, int, unsigned long) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetFrameRect(void*, int, float, float, float, float) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API GetTextureFrame(void*, int, unsigned long*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetGroupPixelFormat(void*, void*, int*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetTileRate(void*, float) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTileRate(void*, float*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetFrameCount(void* Record, int* Out2) override
        {
            const size_t Entry = reinterpret_cast<size_t>(Record);
            *Out2 = Entries[Entry - 1].second;
            Line(Format("\"ev\":\"frames\",\"rec\":\"%u\",\"n\":\"%d\"", (unsigned)Entry, *Out2));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API ResolveTextureRecord(int, void**) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetFrame(int Id, int Frame, unsigned long* Out2) override
        {
            if (Playbacks.count(Id) == 0)
            {
                Line(Format("\"ev\":\"frame\",\"id\":\"%d\",\"f\":\"%d\",\"r\":\"-1\"", Id, Frame));
                return GR_GENERIC;
            }
            Out2[0] = (unsigned long)(Id * 100 + Frame);
            const float Words[4] = {0.125f * (float)Frame, 0.0f, 1.0f, 0.5f};
            memcpy(Out2 + 1, Words, sizeof(Words));
            Line(Format("\"ev\":\"frame\",\"id\":\"%d\",\"f\":\"%d\",\"r\":\"0\"", Id, Frame));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetElapsedTime(int, float) override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API GetElapsedTime(int, float*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetFrameIndex(int, unsigned int) override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API GetFrameIndex(int, unsigned int*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetLocalRate(int, float) override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API GetEffectiveRate(int, float*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetWrapMode(int Id, int Mode) override
        {
            Line(Format("\"ev\":\"wrap\",\"id\":\"%d\",\"m\":\"%d\"", Id, Mode));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetWrapMode(int, int*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API AdvanceTime(int Id, float Seconds) override
        {
            Line(Format("\"ev\":\"adv\",\"id\":\"%d\",\"dt\":\"%s\"", Id, F(Seconds).c_str()));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API PeekNextGroupId(unsigned long*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API EnumGroup(int, int*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetSubCount(int, int*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API EnumTexture(int, int, void**) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API AddSubTexture(int, void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API RemoveSubTexture(int, void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API PickGpuFormat(const unsigned long*, unsigned long*) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API AllocateNewTextureId(int*) override { return GR_GENERIC; }
    };

    // --- The component stand-ins ----------------------------------------------------------------

    class FInert final : public INeuronComponent
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

    class FPipelineOuter final : public INeuronComponent
    {
    public:
        INeuronComponent* Textures = nullptr;
        FInert Inert;
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            const bool bTextures = strcmp(Iid, Iids::TextureLibraryA) == 0 && Textures != nullptr;
            *Out = bTextures ? Textures : static_cast<INeuronComponent*>(&Inert);
            return GR_OK;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
    };

    FPipelineOuter GOuter;

    IParticleFx* CreateParticleFx()
    {
        INeuronComponent* Inner = nullptr;
        FAGGDESC14 Desc = {0x14, "ParticleFx", &GOuter, &Inner, 0};
        void* Object = nullptr;
        if (NEURONCOMPONENTS_Acquire()->CreateInstance(reinterpret_cast<AGGDESC*>(&Desc), &Object) != GR_OK ||
            Object == nullptr)
        {
            fprintf(stderr, "neuron-client: cannot create ParticleFx\n");
            GFailed = true;
            return nullptr;
        }
        return static_cast<IParticleFx*>(static_cast<INeuronComponent*>(Object));
    }

    IFxEffectLibrary* CreateEffectLibrary()
    {
        AGGDESC Desc = {8, "FxEffectLibrary"};
        void* Object = nullptr;
        if (NEURONCOMPONENTS_Acquire()->CreateInstance(&Desc, &Object) != GR_OK || Object == nullptr)
        {
            fprintf(stderr, "neuron-client: cannot create FxEffectLibrary\n");
            GFailed = true;
            return nullptr;
        }
        return static_cast<IFxEffectLibrary*>(static_cast<INeuronComponent*>(Object));
    }

    // --- Setup ---------------------------------------------------------------------------------

    struct FPoolName
    {
        const char* Name;
        FAppearancePool* Pool;
        const unsigned long* Stride;
    };

    const FPoolName PoolNames[] = {
        {"basic", &G_PoolBasic, &kStrideBasic},       {"particle", &G_PoolParticle, &kStrideParticle},
        {"rect", &G_PoolRect, &kStrideRect},          {"perp", &G_PoolPerp, &kStridePerp},
        {"mesh", &G_PoolMesh, &kStrideMesh},          {"oriented", &G_PoolOriented, &kStrideOriented},
        {"dust", &G_PoolDust, &kStrideDust},          {"beam", &G_PoolBeam, &kStrideBeam},
    };

    bool Ready()
    {
        static int State = 0;
        if (State == 0)
        {
            State = OracleLoadMathEngine() ? 1 : 2;
            if (State == 1)
            {
                SelectParticleFxMathImplementation();
                InitializeParticleFxModule();
            }
            else
            {
                fprintf(stderr, "neuron-client: no 3DMathEngine for the particle suites\n");
                GFailed = true;
            }
        }
        return State == 1;
    }

    void ResetModule()
    {
        for (const FPoolName& Entry : PoolNames)
            AllocateAppearancePool(Entry.Pool, 1000, *Entry.Stride);
        G_ParticleFxRandState = 0;
        G_ParticleFxRandCounter = 0;
    }

    int __cdecl CollectNode(INeuronComponent* Node, void* Context)
    {
        static_cast<std::vector<INeuronComponent*>*>(Context)->push_back(Node);
        return 0;
    }

    int __cdecl CollectRow(const FNodePropertyRow* Row, void* Context)
    {
        static_cast<std::vector<std::pair<unsigned long, unsigned long>>*>(Context)->push_back(
            std::make_pair(Row->Hash, Row->Kind));
        return 0;
    }

    // Hashes some class owns that its row table does not list (oracle_fx_data.cpp's list).
    const unsigned long Probes[][2] = {
        {0x0C53B008, 0x200}, {0x0BA0B3BB, 0x105}, {0xF0004D58, 0x105}, {0xE462B789, 0x202},
        {0x061AAF28, 0x202}, {0x16FF3474, 0x202},
    };

    // --- particle_ramp -----------------------------------------------------------------------

    void RampSuite()
    {
        Suite Out("particle_ramp.jsonl");
        const std::vector<std::string> Lines = InputLines("particle_ramp.list");
        for (size_t Case = 0; Case < Lines.size(); ++Case)
        {
            const std::vector<std::string> Parts = SplitText(Lines[Case], " ");
            FAleFs Fs;
            FillFs(Fs, Parts[0], "");
            IParticleFx* Fx = CreateParticleFx();
            if (Fx == nullptr)
                return;
            const GENRESULT Loaded = Fx->LoadNodeLibrary(&Fs);
            Out.Line(Format("{\"case\":\"%u\",\"load\":\"%ld\"}", (unsigned)Case, (long)Loaded));
            std::vector<std::pair<float, float>> Times;
            for (const std::string& Pair : SplitText(Parts[1], ","))
            {
                const std::vector<std::string> Both = SplitText(Pair, ":");
                Times.push_back(std::make_pair(HexFloat(Both[0]), HexFloat(Both[1])));
            }
            std::vector<INeuronComponent*> Nodes;
            Fx->NodeLibSlot8(CollectNode, &Nodes);
            for (size_t NodeIndex = 0; NodeIndex < Nodes.size(); ++NodeIndex)
            {
                IFxNodePropertySet* Set = nullptr;
                AsFxNode(Nodes[NodeIndex])->GetPropertySet(&Set);
                std::vector<std::pair<unsigned long, unsigned long>> Rows;
                Set->EnumerateProps(CollectRow, &Rows);
                const size_t Listed = Rows.size();
                for (const auto& Probe : Probes)
                {
                    bool bListed = false;
                    for (size_t Row = 0; Row < Listed; ++Row)
                        bListed |= Rows[Row].first == Probe[0] && Rows[Row].second == Probe[1];
                    if (!bListed)
                        Rows.push_back(std::make_pair(Probe[0], Probe[1]));
                }
                for (const auto& Row : Rows)
                {
                    std::string Values;
                    bool bFound = false;
                    for (size_t Time = 0; Time < Times.size(); ++Time)
                    {
                        const float Outer = Times[Time].first;
                        const float Inner = Times[Time].second;
                        std::string Value;
                        if (Row.second == 0x200 || Row.second == 0x202)
                        {
                            INestedSingleRamp* Ramp = nullptr;
                            const GENRESULT Got = Row.second == 0x200
                                ? Set->GetPropSingleRampRamp(Row.first, &Ramp)
                                : Set->GetPropAnimatedSingleRamp(Row.first, &Ramp);
                            if (Got != GR_OK || Ramp == nullptr)
                                break;
                            Value = Format("%016llx", DoubleBits(Ramp->Eval(Outer, Inner)));
                            Ramp->Release();
                        }
                        else if (Row.second == 0x201)
                        {
                            IRampColorRamp* Ramp = nullptr;
                            if (Set->GetPropColorRampRamp(Row.first, &Ramp) != GR_OK || Ramp == nullptr)
                                break;
                            float Rgb[3];
                            Ramp->EvalRgb(Rgb, Outer, Inner);
                            const unsigned long Packed = Ramp->EvalPacked(Outer, Inner);
                            Value = Floats(Rgb, 3, ".") + Format(".%08lx", Packed);
                            Ramp->Release();
                        }
                        else if (Row.second == 0x105)
                        {
                            IAnimatedTransform* Transform = nullptr;
                            if (Set->GetPropTransform(Row.first, &Transform) != GR_OK || Transform == nullptr)
                                break;
                            float Out12[12];
                            Transform->Evaluate(FloatBits(Outer), FloatBits(Inner), Out12);
                            Value = Floats(Out12, 12, ".");
                            Transform->Release();
                        }
                        else
                            break;
                        bFound = true;
                        if (Time != 0)
                            Values += ",";
                        Values += Value;
                    }
                    if (bFound)
                        Out.Line(Format("{\"case\":\"%u\",\"node\":\"%u\",\"hash\":\"%08lx\",\"kind\":\"%lx\",\"v\":\"%s\"}",
                                        (unsigned)Case, (unsigned)NodeIndex, Row.first, Row.second,
                                        Values.c_str()));
                }
                Set->Release();
            }
        }
    }

    // --- particle_sim ------------------------------------------------------------------------

    struct FSlot
    {
        IFxEffect* Effect = nullptr;
        INeuronComponent* Parent = nullptr;
    };

    FGenericFxObjectBase* Base(INeuronComponent* Node)
    {
        return static_cast<FGenericFxObjectBase*>(AsFxNode(Node));
    }

    std::string RecordOf(FFxEffectObject* Effect, INeuronComponent* Node, INeuronComponent* Parent)
    {
        if (Node == nullptr)
            return "-";
        for (unsigned long Index = 0; Index < Effect->NodeCount_; ++Index)
            if (Effect->Nodes_[Index].Node == Node)
                return Format("%lu", Index);
        return Node == Parent ? "p" : "?";
    }

    FAppearancePool* PoolOf(const char* Factory)
    {
        return PoolForAppearanceFactory(Factory);
    }

    unsigned long ItemIndex(FAppearancePool* Pool, const void* Item)
    {
        return (unsigned long)((static_cast<const unsigned char*>(Item) - static_cast<unsigned char*>(Pool->Items)) /
                               Pool->Stride);
    }

    void DumpEffect(Suite& Out, const std::string& Head, FFxEffectObject* Effect, INeuronComponent* Parent)
    {
        for (unsigned long Index = 0; Index < Effect->NodeCount_; ++Index)
        {
            const FFxEffectNodeRec& Record = Effect->Nodes_[Index];
            FGenericFxObjectBase* Node = Base(Record.Node);
            const char* Factory = Node->GetFxFactoryName();
            std::string Line = Head + Format("\"op\":\"node\",\"r\":\"%lu\",\"id\":\"%lu\",\"kind\":\"%lu\",", Index,
                                             Record.Id, Record.Kind);
            Line += Format("\"class\":\"%s\",\"refs\":\"%lu\",\"flags\":\"%08lx\",", Factory, Node->RefCount_,
                           Node->NodeFlags_);
            Line += "\"age\":\"" + F(Node->Age_) + "\",\"dur\":\"" + F(Node->Duration_) + "\",\"outer\":\"" +
                    F(Node->AppearanceOuterTime_) + "\",\"lod\":\"" + F(Node->LodInput_) + "\",";
            Line += "\"parent\":\"" + RecordOf(Effect, Node->AttachParent_, Parent) + "\",";
            Line += "\"local\":\"" + Floats(Node->NodeLocalXform_, 12) + "\",\"world\":\"" +
                    Floats(Node->NodeWorldXform_, 12) + "\"";
            const std::string Class = Factory;
            if (Node->bIsEmitterNode_ != 0)
            {
                FFxEmitterBase* E = static_cast<FFxEmitterBase*>(Node);
                Line += ",\"app\":\"" + RecordOf(Effect, E->AssociatedAppearance_, Parent) + "\"";
                Line += ",\"last\":\"" + F(E->LastEmitTime_) + "\",\"press\":\"" + F(E->Pressure_) +
                        "\",\"life\":\"" + F(E->InitLifeSpan_) + "\"";
                Line += Format(",\"reset\":\"%u\"", (unsigned)E->bResetEmitHistory_);
                Line += ",\"prev\":\"" + Floats(E->PrevNodePos_, 3) + "\",\"frac\":\"" + F(E->EmitFraction_) +
                        "\",\"x\":\"" + Floats(E->EmitterXform_, 12) + "\"";
                if (Class == "FxConeEmitter")
                {
                    FxConeEmitter* C = static_cast<FxConeEmitter*>(Node);
                    const float S[4] = {C->MinRadius_, C->RadiusRange_, C->MinSpreadRad_, C->SpreadRangeRad_};
                    Line += ",\"shape\":\"" + Floats(S, 4) + "\"";
                }
                else if (Class == "FxCubeEmitter")
                {
                    FxCubeEmitter* C = static_cast<FxCubeEmitter*>(Node);
                    const float S[5] = {C->MinSpreadRad_, C->SpreadRangeRad_, C->Width_, C->Height_, C->Depth_};
                    Line += ",\"shape\":\"" + Floats(S, 5) + "\"";
                }
                else
                {
                    FxSphereEmitter* C = static_cast<FxSphereEmitter*>(Node);
                    const float S[2] = {C->MinRadius_, C->RadiusRange_};
                    Line += ",\"shape\":\"" + Floats(S, 2) + "\"";
                }
            }
            else if (Class == "FxGravityField")
            {
                FxGravityField* G = static_cast<FxGravityField*>(Node);
                Line += ",\"g\":\"" + F(G->Gravity_) + "\",\"c\":\"" + Floats(G->Cache_, 3) + "\"";
            }
            else if (Class == "FxAirField")
            {
                FxAirField* A = static_cast<FxAirField*>(Node);
                Line += ",\"ap\":\"" + F(A->Approach_) + "\",\"mag\":\"" + F(A->Magnitude_) + "\",\"bl\":\"" +
                        F(A->Blend_) + "\",\"c\":\"" + Floats(A->Cache_, 3) + "\"";
            }
            else if (Class == "FxRadialField")
            {
                FxRadialField* R = static_cast<FxRadialField*>(Node);
                Line += ",\"mag\":\"" + F(R->Magnitude_) + "\",\"ap\":\"" + F(R->Approach_) + "\",\"bl\":\"" +
                        F(R->Blend_) + "\",\"ds\":\"" + F(R->DistScale_) + "\",\"c\":\"" + Floats(R->Cache_, 3) +
                        "\"";
            }
            else if (Class == "FxTurbulenceField")
            {
                FxTurbulenceField* T = static_cast<FxTurbulenceField*>(Node);
                Line += ",\"ap\":\"" + F(T->Approach_) + "\",\"bl\":\"" + F(T->Blend_) + "\",\"mag\":\"" +
                        F(T->Magnitude_) + "\"";
            }
            else if (Class == "FxCollideField")
            {
                FxCollideField* C = static_cast<FxCollideField*>(Node);
                Line += ",\"rf\":\"" + F(C->Reflect_) + "\",\"dt\":\"" + F(C->UpdateDt_) + "\",\"n\":\"" +
                        Floats(C->Normal_, 3) + "\",\"co\":\"" + Floats(C->Corner_, 3) + "\",\"sn\":\"" +
                        Floats(C->ScaledNormal_, 3) + "\"";
            }
            else if (Class == "FLDustField")
            {
                FLDustField* D = static_cast<FLDustField*>(Node);
                Line += ",\"rad\":\"" + F(D->Radius_) + "\",\"rsq\":\"" + F(D->RadiusSq_) + "\",\"dt\":\"" +
                        F(D->UpdateDt_) + "\"";
            }
            else if (Node->bIsAppearanceNode_ != 0)
            {
                Line += Format(",\"live\":\"%lu\",\"erefs\":\"%ld\",\"fields\":\"", Node->LiveInstanceCount_,
                               (long)Node->EmitterRefCount_);
                for (unsigned long Field = 0; Field < Node->FieldCount_; ++Field)
                {
                    if (Field != 0)
                        Line += ",";
                    Line += RecordOf(Effect, Node->AssociatedFields_[Field], Parent);
                }
                const FFxTextureWrapper& T = Node->Texture_;
                Line += Format("\",\"tex\":\"%08lx,%d,%d,", T.TexCrc, T.Handle, T.FrameCount) +
                        Floats(T.Frame, 5) + "\"";
                if (Class == "FLDustAppearance")
                    Line += ",\"ldt\":\"" + F(static_cast<FLDustAppearance*>(Node)->LastDt_) + "\"";
                if (Class == "FLBeamAppearance")
                    Line += Format(",\"par\":\"%u\"", (unsigned)static_cast<FLBeamAppearance*>(Node)->bRibbonParity_);
            }
            Out.Line(Line + "}");
            if (Node->bIsAppearanceNode_ == 0)
                continue;
            FAppearancePool* Pool = PoolOf(Factory);
            for (FPooledParticle* P = Node->InstanceListHead_; P != nullptr; P = P->Next)
            {
                std::string Part = Head + Format("\"op\":\"part\",\"r\":\"%lu\",\"i\":\"%lu\",\"p\":\"", Index,
                                                 ItemIndex(Pool, P));
                Part += Floats(&P->PosX, 9);
                const unsigned char* Extra = reinterpret_cast<const unsigned char*>(P) + 0x30;
                if (Class == "FxPerpAppearance" || Class == "FLDustAppearance")
                    Part += "," + Floats(reinterpret_cast<const float*>(Extra), 3);
                else if (Class == "FLBeamAppearance")
                    Part += "," + Floats(reinterpret_cast<const float*>(Extra), 1);
                Out.Line(Part + "\"}");
            }
        }
    }

    void DumpPools(Suite& Out, const std::string& Head)
    {
        for (const FPoolName& Entry : PoolNames)
        {
            FAppearancePool* Pool = Entry.Pool;
            std::string Used;
            for (FPoolLink* Link = Pool->UsedHead; Link != nullptr; Link = Link->Next)
            {
                if (!Used.empty())
                    Used += ",";
                Used += Format("%lu", (unsigned long)(Link - Pool->FreeNodes));
            }
            Out.Line(Head + Format("\"op\":\"pool\",\"k\":\"%s\",\"free\":\"%lu\",\"used\":\"%s\"}", Entry.Name,
                                   Pool->FreeCount, Used.c_str()));
        }
    }

    void SimSuite()
    {
        Suite Out("particle_sim.jsonl");
        const std::vector<std::string> Lines = InputLines("particle_sim.list");
        FTexLib Textures;
        Textures.Out = &Out;
        for (size_t Case = 0; Case < Lines.size(); ++Case)
        {
            ResetModule();
            const std::string Head = Format("{\"case\":\"%u\",", (unsigned)Case);
            Textures.Head = Head;
            Textures.Entries.clear();
            Textures.Playbacks.clear();
            Textures.NextId = 1;
            GOuter.Textures = &Textures;
            IParticleFx* Fx = CreateParticleFx();
            IFxEffectLibrary* Library = CreateEffectLibrary();
            if (Fx == nullptr || Library == nullptr)
                return;
            G_TextureLibrary = &Textures;
            Library->BindParticleFx(Fx, Fx);
            FAleFs Fs;
            FSlot Slots[4];
            for (const std::string& Op : SplitText(Lines[Case], " ; "))
            {
                const std::vector<std::string> P = SplitText(Op, " ");
                const std::string& Name = P[0];
                FSlot* S = P.size() > 1 && Name != "ale" && Name != "pool" && Name != "seed" && Name != "tex"
                    ? &Slots[atoi(P[1].c_str())]
                    : nullptr;
                IFxEffect* Effect = S != nullptr ? S->Effect : nullptr;
                if (Name == "ale")
                {
                    FillFs(Fs, P[1], P[2]);
                    const GENRESULT Nodes = Fx->LoadNodeLibrary(&Fs);
                    const GENRESULT Effects = Library->LoadAle(&Fs);
                    Out.Line(Head + Format("\"op\":\"ale\",\"nodes\":\"%ld\",\"effects\":\"%ld\"}", (long)Nodes,
                                           (long)Effects));
                }
                else if (Name == "pool")
                {
                    for (const FPoolName& Entry : PoolNames)
                        if (P[1] == Entry.Name)
                            AllocateAppearancePool(Entry.Pool, strtoul(P[2].c_str(), nullptr, 10), *Entry.Stride);
                    Out.Line(Head + "\"op\":\"pool\"}");
                }
                else if (Name == "seed")
                {
                    G_ParticleFxRandState = strtoul(P[1].c_str(), nullptr, 10);
                    G_ParticleFxRandCounter = strtoul(P[2].c_str(), nullptr, 10);
                    Out.Line(Head + "\"op\":\"seed\"}");
                }
                else if (Name == "tex")
                {
                    Textures.Entries.push_back(
                        std::make_pair(strtoul(P[1].c_str(), nullptr, 16), atoi(P[2].c_str())));
                    Out.Line(Head + "\"op\":\"tex\"}");
                }
                else if (Name == "effect")
                {
                    AGGDESC Desc = {8, "FxNode"};
                    NEURONCOMPONENTS_Acquire()->CreateInstance(&Desc, (void**)&S->Parent);
                    const Bytes EffectName = Unhex(P[2]);
                    IFxEffect* Created = nullptr;
                    const GENRESULT Result = Library->CreateEffect(
                        S->Parent, reinterpret_cast<void*>((uintptr_t)HashNameCrc(CStr(EffectName))), &Created);
                    S->Effect = Created;
                    FFxEffectObject* Object = static_cast<FFxEffectObject*>(Created);
                    if (Result == GR_OK && Object != nullptr)
                        Out.Line(Head + Format("\"op\":\"effect\",\"r\":\"%ld\",\"n\":\"%lu\",\"roots\":\"%ld\"}",
                                               (long)Result, Object->NodeCount_, (long)Object->RootNodeCount_));
                    else
                        Out.Line(Head + Format("\"op\":\"effect\",\"r\":\"%ld\",\"n\":\"-\",\"roots\":\"-\"}",
                                               (long)Result));
                }
                else if (Name == "xform")
                {
                    float Xform[12];
                    const std::vector<std::string> Words = SplitText(P[3], ",");
                    for (int Index = 0; Index < 12; ++Index)
                        Xform[Index] = HexFloat(Words[Index]);
                    const GENRESULT Result =
                        Effect->Slot3(reinterpret_cast<void*>((uintptr_t)atoi(P[2].c_str())), Xform);
                    Out.Line(Head + Format("\"op\":\"xform\",\"r\":\"%ld\"}", (long)Result));
                }
                else if (Name == "outer" || Name == "age" || Name == "lod")
                {
                    const float Value = HexFloat(P[2]);
                    if (Name == "outer")
                        Effect->Slot6(Value);
                    else if (Name == "age")
                        Effect->Slot5(Value);
                    else
                        Effect->Slot14(Value);
                    Out.Line(Head + "\"op\":\"" + Name + "\"}");
                }
                else if (Name == "step")
                {
                    const float Dt = HexFloat(P[2]);
                    void* Arg = nullptr;
                    memcpy(&Arg, &Dt, sizeof(Dt));
                    Effect->Slot7(nullptr);
                    Effect->Slot8(Arg);
                    Out.Line(Head + "\"op\":\"step\"}");
                }
                else if (Name == "stop")
                {
                    Effect->Slot13(reinterpret_cast<void*>((uintptr_t)atoi(P[2].c_str())));
                    Out.Line(Head + "\"op\":\"stop\"}");
                }
                else if (Name == "done")
                {
                    const GENRESULT Result = Effect->Slot11(reinterpret_cast<void*>((uintptr_t)atoi(P[2].c_str())));
                    Out.Line(Head + Format("\"op\":\"done\",\"r\":\"%ld\"}", (long)Result));
                }
                else if (Name == "release")
                {
                    if (S->Effect != nullptr)
                        S->Effect->Release();
                    S->Effect = nullptr;
                    if (S->Parent != nullptr)
                        S->Parent->Release();
                    S->Parent = nullptr;
                    Out.Line(Head + "\"op\":\"release\"}");
                }
                else if (Name == "rand")
                    Out.Line(Head + Format("\"op\":\"rand\",\"s\":\"%08lx\",\"c\":\"%08lx\"}", G_ParticleFxRandState,
                                           G_ParticleFxRandCounter));
                else if (Name == "pools")
                    DumpPools(Out, Head);
                else if (Name == "dump")
                    DumpEffect(Out, Head + Format("\"e\":\"%s\",", P[1].c_str()),
                               static_cast<FFxEffectObject*>(Effect), S->Parent);
                else
                {
                    fprintf(stderr, "particle_sim: unknown operation %s\n", Name.c_str());
                    GFailed = true;
                }
            }
            G_TextureLibrary = nullptr;
            // The ParticleFx and the library are left alive: their teardown walks state the cases
            // do not set up (the init node).
        }
    }
}

void RunParticleSuites()
{
    if (!Ready())
        return;
    RampSuite();
    SimSuite();
}
