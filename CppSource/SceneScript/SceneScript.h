// Author: Giperion.

#pragma once

// SceneScript.dll — architecture: implementation notes.
// Public contracts are declared in src/include/scenescript_iface.h.
// Interface methods: NEURONCOMPONENTS_API stdcall unless noted. IScriptEngine slot 20 = thiscall.
// IParamCurve is declared once in scenescript_iface.h using modern NEURONCOMPONENTS_API methods.

#include "neuroncomponents.h"
#include "scenescript_iface.h"
#include "x86math.h"

static const char* const G_NameScriptEngine = "IScriptEngine";
static const char* const G_Name3DMathEngine = "3DMathEngine";

static const unsigned long G_SceneScriptRegisterPriority = 0x40000000;
static const unsigned long G_FactoryIidCompareBytes = 0x17; // "1.11_IComponentFactory" + NUL
static const unsigned long G_AggDescSizeSceneScript = 0x14;
static const unsigned long G_AggDescSizeMath = 0xC;

// x86math.h's FVector, which SceneScript and SoundManager each used to redeclare.
typedef FVector FVec3;

struct FQuat
{
    float W;
    float X;
    float Y;
    float Z;
};

// stack desc (size + name + unused +8).
struct FAggDescC
{
    unsigned long Size;
    const char* NameAt4;
    void* Field8;
};

// SceneScript factories: size 0x14, +4 name, +8 outer, +0xC inner-out (not FAGGDESC14 IID layout).
struct FSceneScriptAggDesc14
{
    unsigned long Size;
    const char* NameAt4;
    INeuronComponent* Outer;
    INeuronComponent** InnerOut;
    unsigned int Field_0x10; // TODO: meaning unknown; Lua producers store zero.
};
static_assert(sizeof(FSceneScriptAggDesc14) == 0x14, "SceneScript creation descriptor size");

// baseline COM aggregation shape shared by the CV_CRSplinePath object
// (controlling unknown at +0x70, embedded inner unknown at +0x74) and the
// IScriptEngine object (+0x108 / +0x10c), built by the factories
// / the branch:
//   * every public facet's QueryInterface/AddRef/Release is a pure forward to
//     the controlling unknown (the routine/e0/f0, the routine/c0/e0:
//     `MOV EAX,[this+0x70]; JMP [[EAX]+slot]`);
//   * the controlling unknown is Desc->Outer when the caller aggregates,
//     otherwise the object's own embedded {vtbl, RefCount=1, Owner} triple;
//   * that triple's vtable (the branch / the branch) is the
//     NON-delegating implementation: QI (the branch / the branch) walks
//     the IID table and AddRefs the facet through the facet's own
//     (delegating) vtbl+4; AddRef (the branch / the branch) is
//     `return ++RefCount`; Release (the branch / the branch) is a
//     saturating decrement that, on reaching zero, parks RefCount at 1,
//     runs the owner's destructor + free through the Owner back-pointer
//     and returns 0.
// FSceneScriptInnerUnknown is that embedded triple, a member rather than a base
// class (baseline keeps it at +0x10c / +0x74, after the data); the owner
// supplies the two callbacks. The owners' facets (+4/+8) are real bases.
typedef GENRESULT (*FSceneScriptInnerQueryFn)(void* Owner, const char* Iid, void** Out);
typedef void (*FSceneScriptInnerDestroyFn)(void* Owner);

class FSceneScriptInnerUnknown final : public INeuronComponent
{
public:
    FSceneScriptInnerUnknown(void* Owner, FSceneScriptInnerQueryFn Query, FSceneScriptInnerDestroyFn Destroy)
        : RefCount_(1), Owner_(Owner), Query_(Query), Destroy_(Destroy)
    {
    }

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
    {
        return Query_(Owner_, Iid, Out);
    }

    unsigned long NEURONCOMPONENTS_API AddRef() override { return ++RefCount_; }

    unsigned long NEURONCOMPONENTS_API Release() override
    {
        // `if (count) --count; if (count) return count;`
        if (RefCount_ != 0)
            --RefCount_;
        if (RefCount_ != 0)
            return RefCount_;
        // baseline parks the count at 1 before destroying the owner so a
        // re-entrant Release during teardown cannot destroy it twice.
        RefCount_ = 1;
        void* Owner = Owner_;
        FSceneScriptInnerDestroyFn Destroy = Destroy_;
        if (Owner != nullptr && Destroy != nullptr)
            Destroy(Owner);
        return 0;
    }

    unsigned long RefCount() const { return RefCount_; }

private:
    unsigned long RefCount_;
    void* Owner_;
    FSceneScriptInnerQueryFn Query_;
    FSceneScriptInnerDestroyFn Destroy_;
};

// Raw I3DMathEngine* from x86math CreateInstance — do not QI (implementation notes).
// Its ABI is x86math.dll's published one; FQuat above is layout-identical to FQuaternion.

void SceneScriptFdump(const char* Message);
void SceneScriptLogStub(const char* What);
bool SceneScriptNameEqual(const char* Left, const char* Right);
I3DMathEngine* SceneScriptAcquire3DMathEngine();
GENRESULT SceneScriptQuatMul(I3DMathEngine* Math, FQuat& Dst, const FQuat& A, const FQuat& B);
GENRESULT SceneScriptQuatSlerp(I3DMathEngine* Math, FQuat& Dst, const FQuat& A, const FQuat& B, float T);
GENRESULT SceneScriptQuatToMatrix(I3DMathEngine* Math, float* Dst9, const FQuat& Quat);
GENRESULT SceneScriptMatrixToQuat(I3DMathEngine* Math, FQuat& Dst, const float* Src9);
GENRESULT SceneScriptMathTransform(I3DMathEngine* Math, float* OutVec3, const float* Xform12,
                             const float* InVec3);
GENRESULT SceneScriptMathMulMatrix(I3DMathEngine* Math, float* Out9, const float* Left9,
                             const float* Right9);
GENRESULT SceneScriptMathMulTransform(I3DMathEngine* Math, float* Out12, const float* Left12,
                                const float* Right12);
GENRESULT SceneScriptMathRotate(I3DMathEngine* Math, float* OutVec3, const float* Xform12,
                          const float* InVec3);
GENRESULT SceneScriptMathTransposeTransform(I3DMathEngine* Math, float* OutVec3, const float* Matrix9,
                                      const float* InVec3);

INeuronComponent* SceneScriptCreateParamCurve(const char* FactoryName, bool bDeadSecondLinear);
// object construction + aggregation step: the new object's
// controlling unknown becomes Outer (or its own inner unknown when Outer is
// null); *OutInnerUnknown receives the embedded non-delegating inner unknown.
INeuronComponent* SceneScriptCreateSplinePath(const char* FactoryName, bool bOrientation, INeuronComponent* Outer,
                                    INeuronComponent** OutInnerUnknown);

// Lua 3.2 (DllMain the routine / slot 4 the routine). See SceneScriptLua.cpp.
class FSceneScriptScene;

bool SceneScriptLuaOpen();
void SceneScriptLuaClose();
ISceneScriptLoadParser* SceneScriptCreateLuaLoadParser(FSceneScriptScene* Scene);


// 16-slot ICRSpline (the branch). Slot 4 = load points the routine.
struct ICRSpline : INeuronComponent
{
    // baseline outer wrappers validate an index against the inner spline and
    // forward these two-argument operations (this, index, output).
    virtual GENRESULT NEURONCOMPONENTS_API Slot3(unsigned int Index, float* OutPoint) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot4LoadPoints(const float* Pts, unsigned long Count) = 0;
    virtual unsigned long NEURONCOMPONENTS_API Slot5() = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot6(unsigned int Index, float* OutPoint) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot7(unsigned int Index, float* OutValue) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot8(unsigned char bClosed) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot9() = 0;
    // Orientation-spline extension slots (present on the orientation vtable).
    // Slot10/Slot11 are per-index / bulk ORIENTATION SETTERS (confirmed
    // 2026-09-10 via the routine's instruction-level trace: the [this+0x5d]
    // orientation-gate + -8 return and the 16-byte/4-float record stride
    // only match a Quats_ writer, never Points_ -- the earlier "indexed
    // point"/"OutRecords" names and getter framing were misattributions).
    virtual GENRESULT NEURONCOMPONENTS_API Slot10(unsigned int Index, float* Orientation) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot11(unsigned int Count, float* Orientations) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot12(unsigned int Index, float* OutOrientation) = 0;
    // Slot13/Slot14 widened 2026-09-10 from the 0-argument position-only
    // stub shape to the orientation variant's real, distinct implementations
    // (the branch / the branch, retraced against scenescript). Neither
    // is called anywhere outside SplinePath.cpp (grepped across src/SceneScript/),
    // so widening is safe.
    virtual GENRESULT NEURONCOMPONENTS_API Slot13(unsigned int Index, const float* MatrixRecords) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot14(unsigned int Count, const float* TransformRecords) = 0;
    // Slot15 (RET 0xc on both variants): position-only the branch is a
    // constant -7 stub; the orientation variant the branch (.rdata
    // / 06f5acc4 = vtable head + 0x3c) is the inverse of Slot13 --
    // it writes the indexed point's [3x3 rotation | position] 12-float
    // transform record into OutTransform and always returns -1.
    virtual GENRESULT NEURONCOMPONENTS_API Slot15(unsigned int Index, float* OutTransform) = 0;
};

// diagnostic event aliases (separate from THN save names).
const char* SceneScriptEventTypeDebugName(unsigned int Type);
