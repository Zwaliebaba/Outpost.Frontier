// Author: Giperion.

#pragma once

#include "common_exports.h"
#include "common_gotobehavior.h"  // GotoBehavior

#include <cstddef>

// ============================================================================
// FreeFlightBehavior -- AI "unescorted free-roam flight" state (Behavior
// family). UNIQUE among this family's siblings: a genuine 2-level
// inheritance chain, FreeFlightBehavior : public GotoBehavior : public
// Behavior, NOT a direct Behavior child.
// ============================================================================

// RTTI identity (hand-decoded directly from Common raw
// bytes, NOT from analysis's .c analysis -- see "ground truth precedence"
// in this marathon's methodology):

//   RTTICompleteObjectLocator @ module storage (offset field = 0)
//     -> pTypeDescriptor @ module storage, name ".?AVFreeFlightBehavior@@"
//     -> pClassHierarchyDescriptor @ 0x063a8d64, numBaseClasses = 3,
//        BaseClassArray @ 0x063a8d54 = [ FreeFlightBehavior(self),
//                                        GotoBehavior(mdisp=0, TD@063ebe3c,
//                                          mangled ".?AVGotoBehavior@@"),
//                                        Behavior(mdisp=0, the SAME shared
//                                          Behavior BaseClassDescriptor every
//                                          other sibling in this family
//                                          reuses) ]

// This is a genuine single-chain 2-level hierarchy (each entry's
// numContainedBases progression confirms a chain, not 3 flat/parallel
// bases) -- see common_gotobehavior.h's own class comment for the full
// cross-check, including the slot-by-slot vtable diff between the two
// classes (FreeFlightBehavior's vtable is GotoBehavior's vtable with exactly
// 6 of 27 slots replaced -- the unmistakable signature of a derived class
// overriding a handful of virtuals while inheriting the rest unchanged).

// ----------------------------------------------------------------------------
// Constructor: the routine (Common), 10 instructions.
// Tail-calls the routine (GotoBehavior's own full constructor, see
// common_gotobehavior.h/.cpp) FIRST -- textbook base-subobject-ctor-then-
// derived-vtable-install C++ codegen -- then overwrites the vtable pointer
// installed by GotoBehavior's own ctor with FreeFlightBehavior's own
// (the routine), and finally sets byte+0x2eb = 1 (GotoBehavior's own
// Flag2eb field, zeroed by GotoBehavior's own ctor -- FreeFlightBehavior
// overrides it to 1 immediately after taking ownership of the object). The
// incoming `undefined4 param_1` analysis's signature shows (also present on
// GotoBehavior's own ctor signature) is forwarded to the routine but never
// read back from `this` by either ctor body -- an MSVC-internal parameter
// with no observable effect on object state, matching the (parameterless)
// convention every other ported sibling ctor in this family already uses;
// not modeled as a real constructor argument here.

// ----------------------------------------------------------------------------
// Destructor: the routine (item 1313, Common) is the
// scalar-deleting-dtor thunk; it calls the routine (Common
// 193102-193179) for the real cleanup body. the routine is BYTE-FOR-BYTE
// IDENTICAL in control flow and field offsets to GotoBehavior's own real
// destructor the routine (common_gotobehavior.cpp) -- same six
// BaseWatcher::set_pointer(nullptr) unwatch calls at the same six offsets
// (+0x370/+0x310/+0x204/+0x18c/+0xb8/+0x40), same the routine(this,-1) tail
// call. FreeFlightBehavior's only own field is the byte at +0x3fc (baseline
// allocates 0x400; Slot3 clears that byte), which needs no teardown, so there is
// nothing extra for a FreeFlightBehavior-specific destructor body to clean up -- baseline's
// compiler simply re-emitted (rather than folding via ICF, for whatever
// linker-internal reason) the identical base-class teardown logic at a
// second address. This port's own ~FreeFlightBehavior() is therefore
// intentionally empty: C++ automatically chains into ~GotoBehavior() (which
// already performs the identical, real teardown) when a FreeFlightBehavior
// is destroyed, with no loss of fidelity versus baseline.
// ============================================================================

class FreeFlightBehavior : public GotoBehavior
{
public:
    // baseline's ctor argument is the behavior type id (19, "FreeFlight"); it is forwarded to GotoBehavior.
    // See Behavior::GetTypeId() in common_behavior.h.
    explicit FreeFlightBehavior(int InTypeId);
    ~FreeFlightBehavior() override;

    // ------------------------------------------------------------------
    // Vtable module storage (27 slots). Raw-dword diff against GotoBehavior's 063a2284
    // (2026-10-08): FreeFlightBehavior replaces slots 0/3/4/6/10 and fills slot 15, which is
    // Behavior's in GotoBehavior's table. Every other slot is GotoBehavior's pointer. All six
    // are overrides as of 2026-10-08 (claude-code:cmn-ai5); bodies in the .cpp.
    //   0 the routine -> the routine   ~FreeFlightBehavior (chains into ~GotoBehavior)
    //   3 the routine   Slot3            4 the branch   Slot4 (`JMP the routine`)
    //   6 the branch   evaluate (1)    10 the branch   Slot10   15 the branch engage_engine
    // ------------------------------------------------------------------
    void Slot3() override;
    void Slot4() override;
    EvaluateResult evaluate(unsigned int Context) override;
    bool Slot10(void* DirectiveOp) override;
    void engage_engine(bool bOn) override;

    // +0x3fc: FreeFlightBehavior's one own byte, past GotoBehavior's 0x3fc. Slot3 clears it
    // (module storage); nothing in the port reads it yet. Not written by the ctor.
    unsigned char Flag3fc;
    unsigned char Pad3fd[0x400 - 0x3fd];
};
static_assert(offsetof(FreeFlightBehavior, Flag3fc) == 0x3fc, "FreeFlightBehavior::Flag3fc offset");

// baseline object size, read off the factory body that allocates this class (`PUSH 0x400`
// immediately before `CALL operator_new`). This is an UPPER bound on purpose: the existing
// `>=` asserts in this family prove the model reaches its furthest known field, but they
// cannot catch the opposite error -- a field declared PAST the end of the baseline object.
// That is exactly what happened to FormationBehavior, which carried a block of DockBehavior
// fields up to 0x470 behind a `>= 0x470` assert while baseline allocates 0x2b8. An upper bound
// cannot fail on an incomplete model, so it is safe to assert even where the port is short.
static_assert(sizeof(FreeFlightBehavior) == 0x400, "FreeFlightBehavior is its baseline size 0x400");
