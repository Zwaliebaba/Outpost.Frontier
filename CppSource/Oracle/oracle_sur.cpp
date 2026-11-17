// Oracle suite and data dump of work package P1.7 (.sur): .sur files and the
// IVP compact surfaces in them. The Rust test
// crates/neuron-core/tests/oracle_sur.rs writes the same lines (its module
// comment lists them); the inputs come from
// crates/neuron-core/tests/fixtures/oracle/make_sur_fixtures.py.
//
// Every file is loaded by Common.dll's exported PhyArch::LoadSurfaces, which
// opens it through NeuronRuntime::Data as the game does. The harness points
// NeuronRuntime::Data at a ReadFile DOS file system over the current
// directory, created the way NeuronRuntime::OpenData creates it; ReadFile's
// factories are registered by oracle_vfs.cpp (RunVfsSuites and
// RunVfsDataDump run first). Each loaded surface is then walked with the
// accessors of the IVP headers (ivp_compact_surface.h, ivp_compact_ledge.h).
// Common.dll exports no IVP code, so the two tree walks the port mirrors,
// IVP_Compact_Ledge_Solver::get_all_ledges and
// IVP_SurfaceManager_Polygon::get_single_convex, are restated below from
// ivp_compact_ledge_solver.cpp and ivp_surman_polygon.cpp.
//
// A part's corners are dumped as LoadSurfaces leaves them; for a part with
// neither an exts chunk nor a repeated id they were never written, so the
// fixtures have none.

#include "oracle_support.h"

#include <string.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "neuroncomponents.h"
#include "readfile.h"
#include "neuronruntime_core.h"
#include "common_phyarch.h"
#include "../IVP/ivp_collision/ivp_compact_surface.h"

// The one NeuronRuntime global LoadSurfaces reads (NeuronRuntime.h declares it
// beside headers that need the DirectX 8 SDK).
namespace NeuronRuntime
{
    NEURONRUNTIME_API extern IFileSystem* Data;
}

using namespace oracle;

namespace
{
    IFileSystem* GPreviousData = nullptr;

    typedef std::vector<unsigned char> FBytes;

    void PutWord(FBytes& Out, unsigned int Value)
    {
        for (int Shift = 0; Shift < 32; Shift += 8)
            Out.push_back((unsigned char)(Value >> Shift));
    }

    std::string HashOf(const FBytes& Bytes)
    {
        return Fnv64(Bytes.empty() ? nullptr : &Bytes[0], Bytes.size());
    }

    unsigned int OffsetIn(const void* Base, const void* Record)
    {
        return (unsigned int)((const char*)Record - (const char*)Base);
    }

    // NeuronRuntime::Data over the current directory (NeuronRuntime::OpenData's
    // descriptor).
    bool OpenSurData()
    {
        FFileSystemDesc Desc;
        memset(&Desc, 0, sizeof(Desc));
        Desc.Size = 0x34;
        Desc.ComponentName = "FileSystem";
        Desc.Implementation = "DOS";
        Desc.Path = GFixtureDir.c_str();
        Desc.Access = 0x80000000;
        Desc.ShareMode = 1;
        Desc.Creation = 3;
        Desc.Flags = 0x08000080;
        Desc.Field30 = 0xFFFFFFFF;
        IFileSystem* Fs = nullptr;
        if (NEURONCOMPONENTS_Acquire()->CreateInstance((AGGDESC*)&Desc, (void**)&Fs) != GR_OK || Fs == nullptr)
        {
            fprintf(stderr, "sur: no DOS file system over %s (are ReadFile's factories registered?)\n",
                    GFixtureDir.c_str());
            GFailed = true;
            return false;
        }
        GPreviousData = NeuronRuntime::Data;
        NeuronRuntime::Data = Fs;
        return true;
    }

    void CloseSurData()
    {
        if (NeuronRuntime::Data != nullptr)
            NeuronRuntime::Data->Release();
        NeuronRuntime::Data = GPreviousData;
        GPreviousData = nullptr;
    }

    void PreOrder(const IVP_Compact_Ledgetree_Node* Node, std::vector<const IVP_Compact_Ledgetree_Node*>& Out)
    {
        Out.push_back(Node);
        if (Node->offset_right_node != 0)
        {
            PreOrder(Node->left_son(), Out);
            PreOrder(Node->right_son(), Out);
        }
    }

    // IVP_Compact_Ledge_Solver::get_all_ledges (ivp_compact_ledge_solver.cpp).
    void TerminalLedges(const IVP_Compact_Ledgetree_Node* Node, std::vector<const IVP_Compact_Ledge*>& Out)
    {
        while (Node->offset_right_node != 0)
        {
            TerminalLedges(Node->left_son(), Out);
            Node = Node->right_son();
        }
        Out.push_back(Node->get_compact_ledge());
    }

    // IVP_SurfaceManager_Polygon::get_single_convex (ivp_surman_polygon.cpp).
    const IVP_Compact_Ledge* SingleConvex(const IVP_Compact_Surface* Surface)
    {
        const IVP_Compact_Ledgetree_Node* Root = Surface->get_compact_ledge_tree_root();
        if (Root->offset_right_node == 0)
            return Root->get_compact_ledge();
        if (Root->offset_compact_ledge != 0)
            return Root->get_compact_ledge();
        return nullptr;
    }

    void DumpLedge(const IVP_Compact_Surface* Surface, const IVP_Compact_Ledge* Ledge, Suite& Out)
    {
        const int Triangles = Ledge->GetNTriangles();
        const IVP_Compact_Triangle* First = Ledge->GetFirstTriangle();
        unsigned int Points = 0;
        FBytes Opposites;
        for (int T = 0; T < Triangles; ++T)
        {
            for (int E = 0; E < 3; ++E)
            {
                const IVP_Compact_Edge* Edge = First[T].GetEdge(E);
                if (Edge->GetStartPointIndex() + 1 > Points)
                    Points = Edge->GetStartPointIndex() + 1;
                const IVP_Compact_Edge* Opposite = Edge->GetOppositeEdge();
                PutWord(Opposites, (unsigned int)((const char*)Opposite - (const char*)First));
            }
        }
        Out.Line(Format("{\"ledge\":\"%u\",\"point_offset\":\"%d\",\"client\":\"%d\",\"flags\":\"%08x\","
                        "\"triangles\":\"%d\",\"unknown_0x0e\":\"%d\",\"points\":\"%u\",\"point_bytes\":\"%s\","
                        "\"triangle_bytes\":\"%s\",\"opposites\":\"%s\"}",
                        OffsetIn(Surface, Ledge), Ledge->CPointOffset, Ledge->ClientData, Ledge->SizeAndFlags,
                        Triangles, (int)Ledge->HasChildrenFlag, Points,
                        Fnv64(Ledge->GetPointArray(), (size_t)Points * sizeof(IVP_U_Float_Point)).c_str(),
                        Fnv64(First, (size_t)Triangles * sizeof(IVP_Compact_Triangle)).c_str(),
                        HashOf(Opposites).c_str()));
    }

    void DumpSurface(unsigned int PartId, const IVP_Compact_Surface* Surface, Suite& Out)
    {
        const int Size = (int)Surface->max_deviation_and_size >> 8;
        std::vector<const IVP_Compact_Ledgetree_Node*> Nodes;
        PreOrder(Surface->get_compact_ledge_tree_root(), Nodes);
        std::set<const IVP_Compact_Ledge*> Distinct;
        for (const IVP_Compact_Ledgetree_Node* Node : Nodes)
        {
            if (Node->offset_compact_ledge != 0)
                Distinct.insert(Node->get_compact_ledge());
        }
        std::vector<const IVP_Compact_Ledge*> Terminal;
        TerminalLedges(Surface->get_compact_ledge_tree_root(), Terminal);
        FBytes TerminalOffsets;
        for (const IVP_Compact_Ledge* Ledge : Terminal)
            PutWord(TerminalOffsets, OffsetIn(Surface, Ledge));
        const IVP_Compact_Ledge* Single = SingleConvex(Surface);
        const std::string SingleText = Single != nullptr ? Format("%u", OffsetIn(Surface, Single)) : "-";
        Out.Line(Format("{\"surface\":\"%u\",\"size\":\"%d\",\"blob\":\"%s\",\"mass_center\":\"%s\","
                        "\"rotation_inertia\":\"%s\",\"radius\":\"%08x\",\"deviation\":\"%u\",\"root\":\"%d\","
                        "\"nodes\":\"%u\",\"ledges\":\"%u\",\"terminal\":\"%s\",\"single_convex\":\"%s\"}",
                        PartId, Size, Fnv64(Surface, (size_t)(Size > 0 ? Size : 0)).c_str(),
                        Hex(Surface->mass_center, 12).c_str(), Hex(Surface->rotation_inertia, 12).c_str(),
                        FloatBits(Surface->upper_limit_radius), Surface->max_deviation_and_size & 0xFFu,
                        Surface->offset_ledgetree_root, (unsigned int)Nodes.size(), (unsigned int)Distinct.size(),
                        HashOf(TerminalOffsets).c_str(), SingleText.c_str()));
        std::set<const IVP_Compact_Ledge*> Seen;
        for (const IVP_Compact_Ledgetree_Node* Node : Nodes)
        {
            Out.Line(Format("{\"node\":\"%u\",\"right\":\"%d\",\"ledge\":\"%d\",\"center\":\"%s\",\"radius\":\"%08x\","
                            "\"box\":\"%s\",\"free\":\"%u\"}",
                            OffsetIn(Surface, Node), Node->offset_right_node, Node->offset_compact_ledge,
                            Hex(Node->center, 12).c_str(), FloatBits(Node->radius), Hex(Node->box_sizes, 3).c_str(),
                            (unsigned int)Node->free_0));
            if (Node->offset_compact_ledge == 0)
                continue;
            const IVP_Compact_Ledge* Ledge = Node->get_compact_ledge();
            if (!Seen.insert(Ledge).second)
                continue;
            DumpLedge(Surface, Ledge, Out);
        }
    }

    // One file through PhyArch::LoadSurfaces; File is '/'-separated and
    // relative to the current directory.
    void DumpSurFile(const std::string& File, Suite& Out)
    {
        std::string SurPath = File;
        for (char& Ch : SurPath)
        {
            if (Ch == '/')
                Ch = '\\';
        }
        std::map<unsigned int, PhyArch::Part> Parts;
        GDumps = 0;
        const bool bLoaded = PhyArch::LoadSurfaces(SurPath.c_str(), Parts);
        Out.Line(Format("{\"file\":\"%s\",\"result\":\"%d\",\"dumps\":\"%d\",\"parts\":\"%u\"}", File.c_str(),
                        bLoaded ? 1 : 0, GDumps, (unsigned int)Parts.size()));
        for (std::map<unsigned int, PhyArch::Part>::const_iterator It = Parts.begin(); It != Parts.end(); ++It)
        {
            const PhyArch::Part& Part = It->second;
            std::string Hardpoints;
            for (size_t Index = 0; Index < Part.HardpointIds.size(); ++Index)
            {
                if (Index != 0)
                    Hardpoints += " ";
                Hardpoints += Format("%u", Part.HardpointIds[Index]);
            }
            Out.Line(Format("{\"part\":\"%u\",\"not_fixed\":\"%u\",\"extents\":\"%s%s\",\"hardpoints\":\"%s\","
                            "\"surface\":\"%d\"}",
                            It->first, (unsigned int)Part.bNotFixed, Hex(&Part.ExtentsMin, 12).c_str(),
                            Hex(&Part.ExtentsMax, 12).c_str(), Hardpoints.c_str(), Part.Surface != nullptr ? 1 : 0));
        }
        for (std::map<unsigned int, PhyArch::Part>::const_iterator It = Parts.begin(); It != Parts.end(); ++It)
        {
            if (It->second.Surface != nullptr)
                DumpSurface(It->first, It->second.Surface, Out);
        }
    }

    bool IsSurFile(const std::string& File)
    {
        return File.size() >= 4 && _stricmp(File.c_str() + File.size() - 4, ".sur") == 0;
    }
}

void RunSurSuites()
{
    if (!OpenSurData())
        return;
    Suite Out("sur_load.jsonl");
    for (const std::string& File : InputLines("sur/files.list"))
        DumpSurFile(File, Out);
    CloseSurData();
}

void RunSurDataDump(const std::vector<std::string>& Files)
{
    if (!OpenSurData())
        return;
    Suite List(GOutputDir + "\\sur\\files.txt");
    unsigned int Count = 0;
    for (const std::string& File : Files)
    {
        if (!IsSurFile(File))
            continue;
        List.Line(File);
        ++Count;
        Suite Out(DataOutputFile("sur", File));
        DumpSurFile(File, Out);
    }
    CloseSurData();
    printf("dumped %u .sur files\n", Count);
}
