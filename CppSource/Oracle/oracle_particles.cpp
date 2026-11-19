// P4.8: the particle simulation (first increment), drawing (second) and the particle and mesh
// appearances (third) of particlefx.dll,
// compiled in from the reference's own sources (particlefx_*.cpp but particlefx.cpp, whose
// DllMain is replaced by the registration below), against a recording texture library,
// recording renderer interfaces and camera, and an in-memory file system. The Rust side is
// crates/neuron-client/tests/support/particles.rs with the same contract; the inputs are
// crates/neuron-client/tests/fixtures/oracle/particle_{ramp,sim,draw,child}.list from
// make_particle_fixtures.py.
//
// The libraries load through FAleChunk from a file system holding the two leaves of an .ale
// (ParticleFxNodeLibrary/ParticleFxNodeLibrary and ALEffectLib/ALEffectLib) as the list gives
// their streams: OpenNode enters a directory of the root or leaves with "..", OpenByDesc opens
// the leaf of the directory entered, Read copies what is left.
//
// The ParticleFx component is made by its factory, aggregated under a stand-in whose
// QueryInterface hands out the recording texture library for ITextureLibraryA, the recording
// renderer fakes for IRenderPipeline8B, IRPVertexBuffer, IRPIndexBuffer and IRPDraw, and an
// inert component for anything else.
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
//
// particle_draw: the operations of particle_sim, each case also starting with no vertex
// buffer, the batcher off, the draw limits of 1000-item pools (5000 vertices, 24000 indices),
// cleared counters, contexts and batch lists and an identity camera, and these:
//   cam <view 12f> <world 12f>  the camera: GetViewTransform's twelve floats; GetOrientation's
//                           nine and GetPosition's three after them (FSpaceCamera's layout)
//   batch <0|1>             SetUseMaterialBatcher
//   limits <vb> <v> <i>     G_MaxVertices, G_DrawMaxVertices, G_DrawMaxIndices
//   vbdrop                  the fake vertex buffer forgets its buffers
//   begin | end             BeginDraw / EndDraw: "r"
//   draw <slot>             Slot9(camera): "r"
//   bounds <slot>           Slot15 on {1, 2, 3, 4}: "r","s"
//   spheres <slot>          per record GetBoundingSphere on {1, 2, 3, 4}: "r","res","s"
//   counters                CopyCounters: "c"
//   dview <slot>            per dust record its camera transform, last camera position, step
// The renderer fakes write {"case","ev",...} in call order: GetRenderState "getrs","s","v" (a
// state never set reads state + 1000)   SetRenderState "setrs","s","v"
//   SetTextureStageState "tss","st","s","v"   SetTextureStageTexture "tex","st","h"
//   SetModelview "modelview","m"   SelectVb "selvb","vb","r" (0 for a live buffer, else -1)
//   CreateVb "createvb","fvf","n","f","vb" (handles from 1)   DestroyVb "destroyvb","vb"
//   LockVb "lockvb","vb","n","off" (offset 3 per earlier lock)   UnlockVb "unlockvb","vb",
//   "data" (the requested vertices' bytes, 0xcd where nothing was written)   LockIb "lockib",
//   "ib","n","off" (offset 5 per earlier lock)   UnlockIb "unlockib","ib","data" (0xcdcd
//   where nothing was written)   SelectIb "selib","ib","base"
//   DrawIndexedPrimitive "dip","prim","min","nv","start","n"
//   the camera's GetViewTransform "camview", GetPosition "campos", GetOrientation "camorient"
//
// particle_child: the operations of particle_draw, every case also binding the runtime's
// ParticleFx and effect library (G_RuntimeParticleFx, G_RuntimeEffectLibrary, which the particle
// appearance creates its child effects through) and no mesh factory, and these:
//   meshfac <mode>          G_FxMeshFactory: 0 none, 1 the recording factory, 2 the factory
//                           failing (-1) with a mesh
//   kids <slot>             per particle appearance record of the effect, each record of its
//                           list: "kid","r","i","q" (BlendQuat, VelQuat, Blend), "life","death"
//                           (the child effect's record and root counts, "-" for none), then a
//                           dump of the life and the death effect (each line with "k":
//                           "<record>.<item>.l|d"); per mesh appearance record, each item's
//                           mesh: "mesh","r","i","m" (0 for none)
// The recording mesh factory writes {"case","ev",...} in call order: Slot3 "meshnew","id"
// (MeshId),"name" (MeshName's bytes in hex),"six","r","m" (meshes numbered from 1, each with
// the transform {2, 0, 0, 0, 2, 0, 0, 0, 2, position} until posed); the meshes' Slot3
// "meshxf","m","x", Slot4 "meshget","m", Slot5 "meshupd","m","dt", Slot6 "meshdraw","m", Slot7
// "meshdrop","m", and the Release that reaches 0 "meshrel","m".
// Floats are the eight hex digits of the f32.

// The dumps below redefine private and protected; see there.
#define _ALLOW_KEYWORD_MACROS

#include "oracle_support.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <new>
#include <set>
#include <string>
#include <vector>

#include "luaprofile.h"
#include "neuroncomponents.h"
#include "particlefx.h"
#include "readfile.h"
#include "render/IRenderPipeline.h"
#include "render/shading.h"
#include "x86math.h"
#include "common_misc.h"

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

namespace ParticleFxImpl
{
    // particlefx_common.cpp's draw-context limits, which no header declares.
    extern unsigned long G_DrawMaxVertices;
    extern unsigned long G_DrawMaxIndices;
}

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

    // --- The recording renderer (the draw cases) --------------------------------------------

    // The draw fakes write {"case","ev",...} lines into the suite being run, in call order,
    // interleaved with the texture library's.
    struct FDrawLog
    {
        Suite* Out = nullptr;
        std::string Head;
        void Event(const std::string& Text)
        {
            if (Out != nullptr)
                Out->Line(Head + Text + "}");
        }
    };

    FDrawLog GLog;

    unsigned long HandleId(const void* Handle) { return (unsigned long)(uintptr_t)Handle; }

    class FPfxPipeline final : public IRenderPipeline
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
        GENRESULT NEURONCOMPONENTS_API GetRenderState(D3DRENDERSTATETYPE State, int* Out) override
        {
            const std::map<int, int>::const_iterator Found = States.find((int)State);
            const int V = Found != States.end() ? Found->second : (int)State + 1000;
            GLog.Event(Format("\"ev\":\"getrs\",\"s\":\"%d\",\"v\":\"%d\"", (int)State, V));
            *Out = V;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetRenderState(D3DRENDERSTATETYPE State, int Value) override
        {
            GLog.Event(Format("\"ev\":\"setrs\",\"s\":\"%d\",\"v\":\"%d\"", (int)State, Value));
            States[(int)State] = Value;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageState(int Stage, int Type, int Value) override
        {
            GLog.Event(Format("\"ev\":\"tss\",\"st\":\"%d\",\"s\":\"%d\",\"v\":\"%d\"", Stage, Type, Value));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageTexture(int Stage, void* Handle) override
        {
            GLog.Event(Format("\"ev\":\"tex\",\"st\":\"%d\",\"h\":\"%lx\"", Stage, HandleId(Handle)));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetModelview(const float* M) override
        {
            GLog.Event("\"ev\":\"modelview\",\"m\":\"" + Floats(M, 12) + "\"");
            return GR_OK;
        }
        // The slots the particle draws do not call.
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

    // The vertex buffer: handles from 1; a lock hands out the start of a scratch region filled
    // with 0xcd (room for two vertices past the request), with the offset 3 times the locks
    // before it; the unlock records the requested count's bytes.
    const unsigned int kScratchVerts = 0x4000;

    class FPfxVb final : public IRPVertexBuffer
    {
    public:
        std::set<unsigned long> Live;
        unsigned long NextVb = 1;
        unsigned int Locks = 0;
        unsigned int Pending = 0;
        std::vector<unsigned char> Scratch;
        FPfxVb() : Scratch((kScratchVerts + 2) * 24, 0xcd) {}
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API SelectVb(void* Handle) override
        {
            const int R = Live.count(HandleId(Handle)) != 0 ? 0 : -1;
            GLog.Event(Format("\"ev\":\"selvb\",\"vb\":\"%lx\",\"r\":\"%d\"", HandleId(Handle), R));
            return R;
        }
        GENRESULT NEURONCOMPONENTS_API CreateVb(unsigned int Fvf, unsigned int Count, unsigned int** Out,
                                                unsigned int Flags) override
        {
            const unsigned long Handle = NextVb++;
            Live.insert(Handle);
            GLog.Event(Format("\"ev\":\"createvb\",\"fvf\":\"%x\",\"n\":\"%u\",\"f\":\"%u\",\"vb\":\"%lx\"", Fvf,
                              Count, Flags, Handle));
            *Out = (unsigned int*)(uintptr_t)Handle;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API DestroyVb(void** Handle) override
        {
            const unsigned long H = HandleId(*Handle);
            GLog.Event(Format("\"ev\":\"destroyvb\",\"vb\":\"%lx\"", H));
            Live.erase(H);
            *Handle = nullptr;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API LockVb(void* Handle, unsigned int* Offset, void** Out,
                                              unsigned int Count) override
        {
            const unsigned int At = 3 * Locks++;
            GLog.Event(Format("\"ev\":\"lockvb\",\"vb\":\"%lx\",\"n\":\"%u\",\"off\":\"%u\"", HandleId(Handle),
                              Count, At));
            Pending = Count < kScratchVerts ? Count : kScratchVerts;
            std::fill(Scratch.begin(), Scratch.end(), (unsigned char)0xcd);
            *Offset = At;
            *Out = Scratch.data();
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API UnlockVb(void* Handle) override
        {
            GLog.Event(Format("\"ev\":\"unlockvb\",\"vb\":\"%lx\",\"data\":\"", HandleId(Handle)) +
                       Hex(Scratch.data(), (size_t)Pending * 24) + "\"");
            return GR_OK;
        }
        // The slots the particle draws do not call.
        GENRESULT NEURONCOMPONENTS_API IsVbValid(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CopyVertices(void* Handle, unsigned int* Base, const FRpVertexSrcDesc* Src,
                                                    unsigned int Start, unsigned int Count) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ResizeVb(void* Handle, unsigned int Fvf, unsigned int Count) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UnsupportedVertexBufferOperation(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetVbInfo(void* Handle, unsigned int* OutFvf, unsigned int* OutCount) override { return GR_GENERIC; }
    };

    // The dynamic index buffer (handle -1): the same with 0xcdcd words and offsets 5 times the
    // locks before.
    class FPfxIb final : public IRPIndexBuffer
    {
    public:
        unsigned int Locks = 0;
        unsigned int Pending = 0;
        std::vector<unsigned short> Scratch;
        FPfxIb() : Scratch(kScratchVerts * 6 + 8, 0xcdcd) {}
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API LockIb(void* Handle, unsigned int* Offset, void** Out, unsigned int Count) override
        {
            const unsigned int At = 5 * Locks++;
            GLog.Event(Format("\"ev\":\"lockib\",\"ib\":\"%lx\",\"n\":\"%u\",\"off\":\"%u\"", HandleId(Handle), Count,
                              At));
            Pending = Count < kScratchVerts * 6 ? Count : kScratchVerts * 6;
            std::fill(Scratch.begin(), Scratch.end(), (unsigned short)0xcdcd);
            *Offset = At;
            *Out = Scratch.data();
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API UnlockIb(void* Handle) override
        {
            std::string Text;
            for (unsigned int Index = 0; Index < Pending; ++Index)
                Text += Format(Index ? ",%u" : "%u", (unsigned)Scratch[Index]);
            GLog.Event(Format("\"ev\":\"unlockib\",\"ib\":\"%lx\",\"data\":\"", HandleId(Handle)) + Text + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SelectIb(void* Handle, unsigned int Base, unsigned int, unsigned int) override
        {
            GLog.Event(Format("\"ev\":\"selib\",\"ib\":\"%lx\",\"base\":\"%u\"", HandleId(Handle), Base));
            return GR_OK;
        }
        // The slots the particle draws do not call.
        GENRESULT NEURONCOMPONENTS_API IsIbValid(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CreateIb(unsigned int Count, unsigned int** Out, unsigned int) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DestroyIb(void** Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ResizeIb(void* Handle, unsigned int Count) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CopyIndices(void* Handle, unsigned int* OutOffset, const unsigned short* SrcData, unsigned int Count) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetIbCapacity(void* Handle, unsigned int* OutCapacity) override { return GR_GENERIC; }
    };

    class FPfxDraw final : public IRPDraw
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
            GLog.Event(Format("\"ev\":\"dip\",\"prim\":\"%lu\",\"min\":\"%u\",\"nv\":\"%u\",\"start\":\"%u\",\"n\":\"%u\"",
                              Prim, Min, NumVertices, Start, Count));
            return GR_OK;
        }
    };

    // The camera: the view transform, and the orientation's nine floats followed by the
    // position (FSpaceCamera's World and WorldPos), which the beam reads as one transform.
    class FPfxCamera final : public ICamera
    {
    public:
        float View[12];
        float World[12];
        FPfxCamera() { Reset(); }
        void Reset()
        {
            const float Identity[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
            memcpy(View, Identity, sizeof(View));
            memcpy(World, Identity, sizeof(World));
        }
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        float const* NEURONCOMPONENTS_API GetViewTransform() override
        {
            GLog.Event("\"ev\":\"camview\"");
            return View;
        }
        float const* NEURONCOMPONENTS_API GetPosition() override
        {
            GLog.Event("\"ev\":\"campos\"");
            return World + 9;
        }
        float const* NEURONCOMPONENTS_API GetOrientation() override
        {
            GLog.Event("\"ev\":\"camorient\"");
            return World;
        }
        // The slots the particle draws do not call.
        int NEURONCOMPONENTS_API ClassifySphere(float const* C, float R) override { return 0; }
        int const* NEURONCOMPONENTS_API GetViewport() override { return nullptr; }
        float NEURONCOMPONENTS_API GetHalfFovXDeg() override { return 0; }
        float NEURONCOMPONENTS_API GetFovYDeg() override { return 0; }
        float NEURONCOMPONENTS_API GetZNear() override { return 0; }
        float NEURONCOMPONENTS_API GetZFar() override { return 0; }
        float NEURONCOMPONENTS_API GetAspect() override { return 0; }
        double NEURONCOMPONENTS_API GetXScaleOverNear() override { return 0; }
        double NEURONCOMPONENTS_API GetYScaleOverNear() override { return 0; }
        int NEURONCOMPONENTS_API Project(float* OutX, float* OutY, float* OutZ, float const* World3) override { return GR_GENERIC; }
        void NEURONCOMPONENTS_API Unproject(float* OutWorld3, float ScreenX, float ScreenY) override { }
        int NEURONCOMPONENTS_API ProjectVec(float* Out3, float const* World3) override { return GR_GENERIC; }
        void GetVelocity(float* Out3) override { }
        int SetProperty(char const* Key, char const* Value) override { return GR_GENERIC; }
    };

    // --- The recording mesh factory (particle_child) -----------------------------------------

    class FFakeMesh final : public IFxMesh
    {
    public:
        unsigned long Id = 0;
        unsigned long Refs = 1;
        float Xform[12] = {};
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return ++Refs; }
        unsigned long NEURONCOMPONENTS_API Release() override
        {
            if (Refs > 0)
                --Refs;
            if (Refs != 0)
                return Refs;
            // The fake keeps the object (the factory's list owns it); a released mesh is not
            // called again.
            GLog.Event(Format("\"ev\":\"meshrel\",\"m\":\"%lu\"", Id));
            return 0;
        }
        GENRESULT NEURONCOMPONENTS_API Slot3(void* Transform) override
        {
            memcpy(Xform, Transform, sizeof(Xform));
            GLog.Event(Format("\"ev\":\"meshxf\",\"m\":\"%lu\",\"x\":\"", Id) + Floats(Xform, 12) + "\"");
            return GR_OK;
        }
        float* NEURONCOMPONENTS_API Slot4() override
        {
            GLog.Event(Format("\"ev\":\"meshget\",\"m\":\"%lu\"", Id));
            return Xform;
        }
        GENRESULT NEURONCOMPONENTS_API Slot5(void* Arg) override
        {
            // The step's float bits in the argument's low word.
            const unsigned long Bits = (unsigned long)(uintptr_t)Arg;
            float Dt = 0.0f;
            memcpy(&Dt, &Bits, sizeof(Dt));
            GLog.Event(Format("\"ev\":\"meshupd\",\"m\":\"%lu\",\"dt\":\"", Id) + F(Dt) + "\"");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Slot6(void*) override
        {
            GLog.Event(Format("\"ev\":\"meshdraw\",\"m\":\"%lu\"", Id));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Slot7() override
        {
            GLog.Event(Format("\"ev\":\"meshdrop\",\"m\":\"%lu\"", Id));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Slot8(void*) override { return GR_GENERIC; }
    };

    class FFakeMeshFactory final : public IFxMeshFactory
    {
    public:
        int Mode = 0;
        std::list<FFakeMesh> Meshes;
        unsigned long NextId = 1;
        void Reset()
        {
            Mode = 0;
            Meshes.clear();
            NextId = 1;
        }
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API Slot3(void* ArgA, void* ArgB, float const* Position,
                                             IFxMesh** Out) override
        {
            Meshes.push_back(FFakeMesh());
            FFakeMesh& Mesh = Meshes.back();
            Mesh.Id = NextId++;
            const float Start[12] = {2, 0, 0, 0, 2, 0, 0, 0, 2, Position[0], Position[1], Position[2]};
            memcpy(Mesh.Xform, Start, sizeof(Start));
            const long Result = Mode == 2 ? -1 : 0;
            GLog.Event(Format("\"ev\":\"meshnew\",\"id\":\"%lu\",\"name\":\"", (unsigned long)(uintptr_t)ArgA) +
                       HexC(static_cast<const char*>(ArgB)) + "\",\"six\":\"" + Floats(Position, 6) +
                       Format("\",\"r\":\"%ld\",\"m\":\"%lu\"", Result, Mesh.Id));
            *Out = &Mesh;
            return static_cast<GENRESULT>(Result);
        }
    };

    FFakeMeshFactory GMeshes;

    FPfxPipeline GPipe;
    FPfxVb GVb;
    FPfxIb GIb;
    FPfxDraw GDrawFake;
    FPfxCamera GCamera;

    class FPipelineOuter final : public INeuronComponent
    {
    public:
        INeuronComponent* Textures = nullptr;
        FInert Inert;
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            const bool bTextures = strcmp(Iid, Iids::TextureLibraryA) == 0 && Textures != nullptr;
            *Out = bTextures ? Textures : static_cast<INeuronComponent*>(&Inert);
            if (strcmp(Iid, Iids::RenderPipeline8B) == 0)
                *Out = static_cast<IRenderPipeline*>(&GPipe);
            else if (strcmp(Iid, Iids::RPVertexBuffer) == 0)
                *Out = static_cast<INeuronComponent*>(&GVb);
            else if (strcmp(Iid, Iids::RPIndexBuffer) == 0)
                *Out = static_cast<INeuronComponent*>(&GIb);
            else if (strcmp(Iid, Iids::RPDraw) == 0)
                *Out = static_cast<INeuronComponent*>(&GDrawFake);
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

    // The module's draw globals and the renderer fakes as a case starts: no vertex buffer, the
    // batcher off, the buffer sizes of 1000-item pools, the counters, contexts and lists
    // cleared, an identity camera.
    // The particle appearance's records: per record of a particle appearance's list, its tail
    // and a dump of each child effect; per mesh appearance item, its mesh.
    std::string ChildText(IFxEffect* Child)
    {
        if (Child == nullptr)
            return "-";
        FFxEffectObject* const Object = static_cast<FFxEffectObject*>(Child);
        return Format("%lu/%ld", Object->NodeCount_, (long)Object->RootNodeCount_);
    }

    void DumpKids(Suite& Out, const std::string& Head, FFxEffectObject* Effect)
    {
        for (unsigned long Index = 0; Index < Effect->NodeCount_; ++Index)
        {
            INeuronComponent* const Record = Effect->Nodes_[Index].Node;
            FGenericFxObjectBase* Node = Base(Record);
            const std::string Class = Node->GetFxFactoryName();
            const bool bMesh = Class == "FxMeshAppearance";
            if (!bMesh && Class != "FxParticleAppearance")
                continue;
            FAppearancePool* Pool = PoolOf(Class.c_str());
            for (FPooledParticle* P = Node->InstanceListHead_; P != nullptr; P = P->Next)
            {
                const unsigned long Item = ItemIndex(Pool, P);
                if (bMesh)
                {
                    IFxMesh* const Mesh = *reinterpret_cast<IFxMesh**>(reinterpret_cast<unsigned char*>(P) + 0x30);
                    const unsigned long Id = Mesh != nullptr ? static_cast<FFakeMesh*>(Mesh)->Id : 0;
                    Out.Line(Head + Format("\"op\":\"mesh\",\"r\":\"%lu\",\"i\":\"%lu\",\"m\":\"%lu\"}", Index,
                                           Item, Id));
                    continue;
                }
                FParticleAppRecord* const R = reinterpret_cast<FParticleAppRecord*>(P);
                float Q[9];
                memcpy(Q, R->BlendQuat, 4 * sizeof(float));
                memcpy(Q + 4, R->VelQuat, 4 * sizeof(float));
                Q[8] = R->Blend;
                Out.Line(Head + Format("\"op\":\"kid\",\"r\":\"%lu\",\"i\":\"%lu\",\"q\":\"", Index, Item) +
                         Floats(Q, 9) + "\",\"life\":\"" + ChildText(R->LifeEffect) + "\",\"death\":\"" +
                         ChildText(R->DeathEffect) + "\"}");
                if (R->LifeEffect != nullptr)
                    DumpEffect(Out, Head + Format("\"k\":\"%lu.%lu.l\",", Index, Item),
                               static_cast<FFxEffectObject*>(R->LifeEffect), Record);
                if (R->DeathEffect != nullptr)
                    DumpEffect(Out, Head + Format("\"k\":\"%lu.%lu.d\",", Index, Item),
                               static_cast<FFxEffectObject*>(R->DeathEffect), Record);
            }
        }
    }

    void ResetDraw()
    {
        G_bInDraw = false;
        G_VbHandle = nullptr;
        G_DrawVbHandle = nullptr;
        G_Defaults.bUseMaterialBatcher = 0;
        G_MaxVertices = 5000;
        G_DrawMaxVertices = 5000;
        G_DrawMaxIndices = 24000;
        for (unsigned long Index = 0; Index < 9; ++Index)
            G_DrawCounters[Index] = 0;
        ClearBatchLists();
        FAlDrawContext* const Contexts[] = {&G_BasicDrawContext, &G_RectDrawContext, &G_PerpDrawContext,
                                            &G_OrientedDrawContext, &G_DustDrawContext, &G_BeamDrawContext};
        for (FAlDrawContext* Context : Contexts)
            *Context = FAlDrawContext();
        GPipe.States.clear();
        GVb.Live.clear();
        GVb.NextVb = 1;
        GVb.Locks = 0;
        GVb.Pending = 0;
        GIb.Locks = 0;
        GIb.Pending = 0;
        GCamera.Reset();
    }

    void ReadFloats(const std::string& Text, float* Out, int Count)
    {
        const std::vector<std::string> Words = SplitText(Text, ",");
        for (int Index = 0; Index < Count; ++Index)
            Out[Index] = HexFloat(Words[Index]);
    }

    // particle_sim and particle_draw run the same operations; particle_draw's cases use the
    // draw operations too.
    void ScriptSuite(const char* OutputName, const char* InputName)
    {
        Suite Out(OutputName);
        const std::vector<std::string> Lines = InputLines(InputName);
        FTexLib Textures;
        Textures.Out = &Out;
        GLog.Out = &Out;
        for (size_t Case = 0; Case < Lines.size(); ++Case)
        {
            ResetModule();
            ResetDraw();
            const std::string Head = Format("{\"case\":\"%u\",", (unsigned)Case);
            Textures.Head = Head;
            GLog.Head = Head;
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
            // The runtime's bindings, which the particle appearance's child effects go through;
            // no mesh factory until a case binds one.
            G_RuntimeParticleFx = Fx;
            G_RuntimeEffectLibrary = Library;
            GMeshes.Reset();
            G_FxMeshFactory = nullptr;
            FAleFs Fs;
            FSlot Slots[4];
            for (const std::string& Op : SplitText(Lines[Case], " ; "))
            {
                const std::vector<std::string> P = SplitText(Op, " ");
                const std::string& Name = P[0];
                FSlot* S = P.size() > 1 && Name != "ale" && Name != "pool" && Name != "seed" && Name != "tex" &&
                        Name != "cam" && Name != "batch" && Name != "limits" && Name != "meshfac"
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
                else if (Name == "cam")
                {
                    ReadFloats(P[1], GCamera.View, 12);
                    ReadFloats(P[2], GCamera.World, 12);
                    Out.Line(Head + "\"op\":\"cam\"}");
                }
                else if (Name == "batch")
                {
                    Fx->SetUseMaterialBatcher((unsigned char)(atoi(P[1].c_str()) != 0 ? 1 : 0));
                    Out.Line(Head + "\"op\":\"batch\"}");
                }
                else if (Name == "limits")
                {
                    G_MaxVertices = strtoul(P[1].c_str(), nullptr, 10);
                    G_DrawMaxVertices = strtoul(P[2].c_str(), nullptr, 10);
                    G_DrawMaxIndices = strtoul(P[3].c_str(), nullptr, 10);
                    Out.Line(Head + "\"op\":\"limits\"}");
                }
                else if (Name == "vbdrop")
                {
                    GVb.Live.clear();
                    Out.Line(Head + "\"op\":\"vbdrop\"}");
                }
                else if (Name == "begin")
                {
                    const GENRESULT Result = Fx->BeginDraw();
                    Out.Line(Head + Format("\"op\":\"begin\",\"r\":\"%ld\"}", (long)Result));
                }
                else if (Name == "end")
                {
                    const GENRESULT Result = Fx->EndDraw();
                    Out.Line(Head + Format("\"op\":\"end\",\"r\":\"%ld\"}", (long)Result));
                }
                else if (Name == "draw")
                {
                    const GENRESULT Result = Effect->Slot9(static_cast<ICamera*>(&GCamera));
                    Out.Line(Head + Format("\"op\":\"draw\",\"r\":\"%ld\"}", (long)Result));
                }
                else if (Name == "bounds")
                {
                    float Sphere[4] = {1.0f, 2.0f, 3.0f, 4.0f};
                    const GENRESULT Result = Effect->Slot15(Sphere);
                    Out.Line(Head + Format("\"op\":\"bounds\",\"r\":\"%ld\",\"s\":\"", (long)Result) +
                             Floats(Sphere, 4) + "\"}");
                }
                else if (Name == "spheres")
                {
                    FFxEffectObject* Object = static_cast<FFxEffectObject*>(Effect);
                    for (unsigned long Index = 0; Index < Object->NodeCount_; ++Index)
                    {
                        float Sphere[4] = {1.0f, 2.0f, 3.0f, 4.0f};
                        const GENRESULT Result = AsFxNode(Object->Nodes_[Index].Node)->GetBoundingSphere(Sphere);
                        Out.Line(Head + Format("\"op\":\"sphere\",\"r\":\"%lu\",\"res\":\"%ld\",\"s\":\"",
                                               Index, (long)Result) + Floats(Sphere, 4) + "\"}");
                    }
                }
                else if (Name == "counters")
                {
                    unsigned long Counters[9];
                    Fx->CopyCounters(Counters);
                    std::string Text;
                    for (int Index = 0; Index < 9; ++Index)
                        Text += Format(Index ? ",%lu" : "%lu", Counters[Index]);
                    Out.Line(Head + "\"op\":\"counters\",\"c\":\"" + Text + "\"}");
                }
                else if (Name == "meshfac")
                {
                    GMeshes.Mode = atoi(P[1].c_str());
                    G_FxMeshFactory = GMeshes.Mode != 0 ? static_cast<INeuronComponent*>(&GMeshes) : nullptr;
                    Out.Line(Head + "\"op\":\"meshfac\"}");
                }
                else if (Name == "kids")
                    DumpKids(Out, Head + Format("\"e\":\"%s\",", P[1].c_str()), static_cast<FFxEffectObject*>(Effect));
                else if (Name == "dview")
                {
                    FFxEffectObject* Object = static_cast<FFxEffectObject*>(Effect);
                    for (unsigned long Index = 0; Index < Object->NodeCount_; ++Index)
                    {
                        FGenericFxObjectBase* Node = Base(Object->Nodes_[Index].Node);
                        if (std::string(Node->GetFxFactoryName()) != "FLDustAppearance")
                            continue;
                        FLDustAppearance* D = static_cast<FLDustAppearance*>(Node);
                        Out.Line(Head + Format("\"op\":\"dview\",\"r\":\"%lu\",\"view\":\"", Index) +
                                 Floats(D->ViewXform_, 12) + "\",\"pos\":\"" + Floats(D->LastCameraPos_, 3) +
                                 "\",\"step\":\"" + F(D->LastCameraStep_) + "\"}");
                    }
                }
                else
                {
                    fprintf(stderr, "particle_sim: unknown operation %s\n", Name.c_str());
                    GFailed = true;
                }
            }
            G_TextureLibrary = nullptr;
            G_FxMeshFactory = nullptr;
            G_RuntimeParticleFx = nullptr;
            G_RuntimeEffectLibrary = nullptr;
            // The ParticleFx and the library are left alive: their teardown walks state the cases
            // do not set up (the init node).
        }
        GLog.Out = nullptr;
    }
}

void RunParticleSuites()
{
    if (!Ready())
        return;
    RampSuite();
    ScriptSuite("particle_sim.jsonl", "particle_sim.list");
    ScriptSuite("particle_draw.jsonl", "particle_draw.list");
    ScriptSuite("particle_child.jsonl", "particle_child.list");
}
