// Author: Giperion.

#pragma once

#include "common_exports.h"
#include "common_behavior.h"  // Behavior, BaseFollowerBehavior
#include "common_goods.h"     // BaseWatcher
#include "common_ini.h"       // Vector
#include "common_idea.h"      // MatchThrottle2Idea, FaceIdea, MoveIdea

#include <cstddef>

// ============================================================================
// FollowBehavior -- RTTI ".?AVFollowBehavior@@" (COL module storage, TypeDescriptor
// ; BaseClassArray {FollowBehavior, BaseFollowerBehavior, Behavior}, a single
// inheritance chain). Behavior type 17 ("Follow"); factory `PUSH 0x2bc` before the ctor
// , so sizeof is 0x2bc.

// The +0x2c..+0x2a0 block (LeadWatcher, the five Ideas, the delegates, the flags, the
// +0x290 offset) is BaseFollowerBehavior's: the routine builds it for both subclasses.

// Vtable module storage (29 slots). Own slots; the rest are BaseFollowerBehavior's:
//    3  the routine  Slot3            4  the routine  Slot4
//    5  the routine  Slot5            6  the branch  evaluate
//   10  the routine  Slot10          27  the branch  Slot27
//   28  the branch  Slot28
// Re-ported 2026-10-08 (claude-code:cmn-ai5): slots 3/4/5/6 were unbound members and slot 8
// was a FollowBehavior-local stub of the base's own the branch.
// ============================================================================
class FollowBehavior : public BaseFollowerBehavior
{
public:
    // (RET 4): the routine(TypeId); +0x281 = +0x280 = 0; vptr 0639fbe4;
    // +0x2a0 = 150.0f; +0x290/+0x294/+0x298 = 0; +0x285 = 0; +0x2ac = 150.0f;
    // +0x2b0 = 2.0f; +0x2b4 = 400.0f; +0x2b8 = 100.0f; +0x2a4 = 1.
    explicit FollowBehavior(int InTypeId);

    // Slot 3, the routine.
    void Slot3() override;
    // Slot 4, the routine.
    void Slot4() override;
    // Slot 5, the routine (RET 4, argument unread).
    int Slot5(float Value) override;
    // Slot 6, the branch (RET 4, argument unread).
    EvaluateResult evaluate(unsigned int Context) override;
    // Slot 10, the routine (RET 4): take a pub::AI::DirectiveFollowOp.
    bool Slot10(void* DirectiveOp) override;
    // Slot 27, the branch (536 instructions): the per-tick follow work.
    void Slot27() override;
    // Slot 28, the branch: Offset290 rotated out of the leader's frame, by value.
    Vector Slot28() override;

private:
    // Not a baseline function: the engage toggle Slot27 inlines twice (062ccf83-062cd043 and
    // ) -- Enabled2a4 follows IsWithinInterceptRangeOfIface and each
    // transition goes to IBehaviorManager::NotifyFollowEvent (the routine).
    void UpdateInterceptEngageState();

public:
    // ---- FollowBehavior's own fields (+0x2a0..+0x2bc) ----
    float SpeedParam2a0;        // +0x2a0. Ctor 150.0f; Slot10 copies op+0x10.
    unsigned char Enabled2a4;   // +0x2a4. Ctor 1; Slot27 toggles it on range transitions.
    unsigned char Pad2a5[3];
    float Distance2a8;          // +0x2a8. Not ctor-set; Slot10: |Offset290|.
    float Field2ac;             // +0x2ac. Ctor 150.0f.
    float Field2b0;             // +0x2b0. Ctor 2.0f; Slot27's weight factor.
    float Field2b4;             // +0x2b4. Ctor 400.0f; Slot5's range; Slot10's failure path
                                // copies op+0x20.
    float Field2b8;             // +0x2b8. Ctor 100.0f; Slot27's range bias.
};

static_assert(offsetof(FollowBehavior, SpeedParam2a0) == 0x2a0, "FollowBehavior::SpeedParam2a0 offset");
static_assert(offsetof(FollowBehavior, Enabled2a4) == 0x2a4, "FollowBehavior::Enabled2a4 offset");
static_assert(offsetof(FollowBehavior, Distance2a8) == 0x2a8, "FollowBehavior::Distance2a8 offset");
static_assert(offsetof(FollowBehavior, Field2ac) == 0x2ac, "FollowBehavior::Field2ac offset");
static_assert(offsetof(FollowBehavior, Field2b0) == 0x2b0, "FollowBehavior::Field2b0 offset");
static_assert(offsetof(FollowBehavior, Field2b4) == 0x2b4, "FollowBehavior::Field2b4 offset");
static_assert(offsetof(FollowBehavior, Field2b8) == 0x2b8, "FollowBehavior::Field2b8 offset");
static_assert(sizeof(FollowBehavior) == 0x2bc, "FollowBehavior size == baseline operator_new(0x2bc)");
