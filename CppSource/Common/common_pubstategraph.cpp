// Author: Giperion.

#include "common_newchar.h"

#include "common_misc.h"

#include <cstdio>
#include <cstring>
#include <new>

extern "C"
{
    __declspec(dllimport) extern void(__cdecl* FDUMP)(int flags, const char* fmt, ...);
}

namespace
{
    const char kStateGraphSource[] = "Common\\Ai\\pubStateGraph.cpp";
    const unsigned kStateGraphStride = 0x72c;

    int GStateGraphCount = 0;
    IStateGraph* GStateGraphs = nullptr;

    // (static-initializer table, module storage) zeroes the count and the array
    // pointer and registers the branch through atexit (the routine): at module teardown the
    // array is released (operator delete, the routine) and the pointer cleared; the count is
    // left. A file-static object with that destructor is the same registration (CMN-S2 AIS).
    struct FStateGraphStorageTeardown
    {
        ~FStateGraphStorageTeardown()
        {

            if (GStateGraphs != nullptr)
            {
                ::operator delete(GStateGraphs);
                GStateGraphs = nullptr;
            }
        }
    };
    FStateGraphStorageTeardown GStateGraphStorageTeardown;

    static_assert(sizeof(IStateGraph) == kStateGraphStride, "IStateGraph stride 0x72c");
}

int CallNeuronComponentsStricmp(const char* Left, const char* Right);

void __cdecl pub::StateGraph::Free()
{
    // (and the inline copies at 0x062df430 / 0x062df580): operator delete
    // (the routine) on the array, the pointer cleared; the count is left as it was.
    if (GStateGraphs != nullptr)
    {
        ::operator delete(GStateGraphs);
        GStateGraphs = nullptr;
    }
}

void __cdecl pub::StateGraph::LoadFromFile(char const* Path)
{
    // .
    FILE* File = fopen(Path, "r");
    if (File == nullptr)
    {
        // .
        FDUMP(0x100001, "%s(%d) : *** ERROR: %s", kStateGraphSource, 0x7d,
              "pub::StateGraph: unable to load state graph definitions");
        return;
    }

    int GraphCount = 0;
    int BehaviorCount = 0;
    fscanf(File, "%*s%d", &GraphCount);
    fscanf(File, "%*s%d", &BehaviorCount);
    if (BehaviorCount != kStateGraphBehaviors)
    {
        // reported and left without fclose -- baseline leaks the FILE
        // here, and so does the port.
        FDUMP(0x100001, "%s(%d) : *** ERROR: %s", kStateGraphSource, 0x8d,
              "pub::StateGraph: state graph definitions don't sync with current behavior list, need to re-save state_graph.ini");
        return;
    }

    // (module storage): the count stored, then operator new(count * 0x72c) with the
    // 32-bit product (IMUL) -- the previous array is not freed here (refresh_state_graph frees
    // it first; DataConfig::init at module storage does not). VC6 operator new answers null on
    // failure; every record gets its weights zeroed and its id -1, the id/type/name dwords at
    // +0x04..+0x47 are left as allocated.
    GStateGraphCount = GraphCount;
    unsigned const Bytes = (unsigned)GraphCount * kStateGraphStride;
    GStateGraphs = static_cast<IStateGraph*>(::operator new(Bytes, std::nothrow));
    if (GStateGraphs != nullptr)
    {
        int InitIndex = 0;
        for (; InitIndex < GraphCount; ++InitIndex)
        {
            memset(GStateGraphs[InitIndex].Weights, 0, sizeof(GStateGraphs[InitIndex].Weights));
            GStateGraphs[InitIndex].Id = -1;
        }
    }
    else if (GraphCount > 0)
    {
        // baseline UB: with the allocation failed and a positive count baseline goes on and writes
        // through the null array (module storage); the port stops reading instead and drops the
        // count, so the lookups below (which, like baseline, do not test the pointer) find nothing.
        GStateGraphCount = 0;
        fclose(File);
        return;
    }

    int GraphIndex = 0;
    for (; GraphIndex < GraphCount; ++GraphIndex)
    {
        IStateGraph& Graph = GStateGraphs[GraphIndex];

        // the name is scanned straight into +0x08 and the type into a
        // 0x100-byte stack buffer, both with an unbounded "%s". baseline UB: a longer token
        // overruns the field (into the weights, later re-read) or the stack buffer. The port
        // scans both into stack buffers and keeps the name to the field: deterministic for a
        // name longer than 0x3f characters, identical otherwise.
        char NameBuf[0x100];
        char TypeBuf[0x100];
        NameBuf[0] = '\0';
        TypeBuf[0] = '\0';
        fscanf(File, "%255s", NameBuf);
        fscanf(File, "%255s", TypeBuf);
        strncpy(Graph.Name, NameBuf, kStateGraphNameChars - 1);
        Graph.Name[kStateGraphNameChars - 1] = '\0';

        // NEG/SBB/NEG -- 1 (ESCORT) when "ESCORT" occurs, else 0.
        Graph.Type = (strstr(TypeBuf, "ESCORT") != nullptr) ? ESCORT : LEADER;

        // 0x15 x 0x15 "%f" reads into +0x48, row by row.
        int Row = 0;
        for (; Row < kStateGraphBehaviors; ++Row)
        {
            int Col = 0;
            for (; Col < kStateGraphBehaviors; ++Col)
                fscanf(File, "%f", &Graph.Weights[Row][Col]);
        }

        // .
        Graph.Id = GraphIndex;
    }

    fclose(File);
}

void __cdecl pub::StateGraph::refresh_state_graph()
{
    char Path[0x104];
    Free();
    GetFileSysPath(Path, "AI\\state_graph.db", nullptr);
    LoadFromFile(Path);
}

int __cdecl pub::StateGraph::get_state_graph(char const* Name, Type GraphType)
{
    // the records in order while index < count (no pointer test), NEURONCOMPONENTS
    // stricmp(record name, Name), then the type; the first match answers its id.
    int Index = 0;
    for (; Index < GStateGraphCount; ++Index)
    {
        if (CallNeuronComponentsStricmp(GStateGraphs[Index].Name, Name) != 0)
            continue;
        if (GStateGraphs[Index].Type != GraphType)
            continue;
        return GStateGraphs[Index].Id;
    }
    return -1;
}

int __cdecl pub::StateGraph::get_state_graph(int GraphId, Type GraphType)
{
    // as above with the name of record GraphId (`IMUL EAX,EAX,0x72c` +
    // 8). Count <= 0 answers -1 before the record is touched, as baseline's JLE at 062df4cd.
    // Very strange, but baseline: GraphId is not range-checked (062df4cf-062df4df), so an id
    // outside [0, count) reads past the array. The former `-1` range guard was invented
    // (removed CMN-S2 AIS 2026-10-14, owner's ruling on cheap baseline UB).
    if (GStateGraphCount <= 0)
        return -1;

    char const* Name = GStateGraphs[GraphId].Name;
    int Index = 0;
    for (; Index < GStateGraphCount; ++Index)
    {
        if (CallNeuronComponentsStricmp(GStateGraphs[Index].Name, Name) != 0)
            continue;
        if (GStateGraphs[Index].Type != GraphType)
            continue;
        return GStateGraphs[Index].Id;
    }
    return -1;
}

IStateGraph const* __cdecl pub::StateGraph::get_state_graph_internal(int GraphId)
{
    // the first record whose id (+0) equals GraphId, else null; no pointer
    // test.
    int Index = 0;
    for (; Index < GStateGraphCount; ++Index)
    {
        if (GStateGraphs[Index].Id == GraphId)
            return &GStateGraphs[Index];
    }
    return nullptr;
}
