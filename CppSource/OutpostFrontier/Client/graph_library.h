// Author: Giperion.

#pragma once

// GraphLibrary -- Client\cmn\GraphLibrary.cpp.

// Named keyframe curves ("IGraph") loaded from the [IGraph] sections of the file DATA's
// OutpostFrontier.ini names under `igraph =` (the routine @ 005b0ee1 -> the routine). The library
// is a std::list<GraphData> (the field allocator byte, the field sentinel node,
// size) built by the static initializer the branch and torn down twice: first by
// from the client atexit chain (005b2286), then by the static destructor
// .

// [LightSource] names its ambient_curve / atten_curve / color_curve by nickname; the engine
// light resolves them through GraphLibraryFind (the routine), which AddRefs the curve it
// returns.

// This is an ordinary C++ object, not a NEURONCOMPONENTS component: every slot takes `this` in ECX
// (__thiscall). The abstract IGraph vtable is the branch (ten _purecall entries, so no
// virtual destructor); GraphData's is the branch.

#include <string>

// The {flag, min, max} block at GraphData +0x20. Callers build one on the stack and pass it to
// Evaluate / EvaluateColor to map their own parameter range onto the curve's X range.
struct FGraphRange
{
    unsigned char bClamp; // +0x00 non-zero: clamp to [Min, Max - 0.001]; zero: wrap
    unsigned char Pad01[3];
    float Min;            // +0x04
    float Max;            // +0x08
};

// One key: X, then either a float (ElemType FLOAT) or a dword (INTEGER, or COLOR packed
// 0xAARRGGBB). 8 bytes, which is the stride every GraphData loop uses.
struct FGraphPoint
{
    float X;
    union
    {
        float Value;
        unsigned Bits;
    };
};

// The {ElemType, Count, Data} triple GetDesc copies out and SetData takes in (GraphData
// +0x14..+0x1C).
struct FGraphDesc
{
    int ElemType;
    unsigned Count;
    FGraphPoint* Data;
};

// "type" values.
enum EGraphElemType
{
    GraphElem_Integer = 1,
    GraphElem_Float = 2,
    GraphElem_Color = 3,
};

class IGraph
{
public:
    virtual char const* GetName() = 0;                                        // 0  the branch
    virtual int GetElemType() = 0;                                            // 1  the routine (ICF)
    virtual unsigned GetCount() = 0;                                          // 2  the branch
    virtual FGraphRange* GetRange() = 0;                                      // 3  the branch (ICF)
    virtual FGraphDesc* GetDesc(FGraphDesc* Out) = 0;                         // 4  the branch
    virtual void SetData(FGraphDesc const* Desc) = 0;                         // 5  the routine
    // Returned in ST0 unrounded (00419452): callers either FSTP it to a float (0041064e) or
    // multiply with it first (00410934), so the port keeps the double.
    virtual double Evaluate(float T, FGraphRange const* Range) = 0;           // 6  the branch
    virtual unsigned EvaluateColor(float T, FGraphRange const* Range) = 0;    // 7  the branch
    virtual int AddRef() = 0;                                                 // 8  the branch
    virtual int Release() = 0;                                                // 9  the branch
};

// GraphData, 0x30 bytes in baseline. Lives by value inside the library's list node (node + 8).
class FGraphData : public IGraph
{
public:
    // . ElemType is deliberately left unset, as in baseline: SetData writes it before
    // anything reads it.
    FGraphData();
    // (reached from the routine's node construct): memberwise copy -- the name,
    // then +0x14..+0x2C verbatim, so the key buffer is shared, not duplicated.
    FGraphData(FGraphData const& Other);
    // free the key buffer, then the name.
    ~FGraphData();

    char const* GetName() override;
    int GetElemType() override;
    unsigned GetCount() override;
    FGraphRange* GetRange() override;
    FGraphDesc* GetDesc(FGraphDesc* Out) override;
    void SetData(FGraphDesc const* Desc) override;
    double Evaluate(float T, FGraphRange const* Caller) override;
    unsigned EvaluateColor(float T, FGraphRange const* Caller) override;
    int AddRef() override;
    int Release() override;

    // name a fresh graph and take the library's reference. Refused once the
    // graph has references or keys.
    bool SetName(char const* NewName);

private:
    FGraphData& operator=(FGraphData const&) = delete;

    std::string Name;     // +0x04
    int ElemType;         // +0x14
    unsigned Count;       // +0x18
    FGraphPoint* Data;    // +0x1C malloc/realloc'd, Count keys
    FGraphRange Range;    // +0x20 bClamp is always 0 here (wrap); Min/Max = key X extent
    int RefCount;         // +0x2C
};

// __thiscall on an FGraphRange: clamp T into [Min, Max - 0.001] or wrap it
// into [0, Max - Min).
double GraphRangeWrap(FGraphRange const* Range, float T);
// __thiscall on an FGraphRange: (T - Min) / (Max - Min).
double GraphRangeNormalize(FGraphRange const* Range, float T);

// find by nickname (case-insensitive), AddRef and return; null plus an FDUMP
// warning when absent.
IGraph* GraphLibraryFind(char const* Name);
// load every [IGraph] section of Path; returns the number of graphs created.
int GraphLibraryLoad(char const* Path);
// release every graph until the library is empty.
void GraphLibraryShutdown();
