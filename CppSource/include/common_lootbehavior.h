// Author: Giperion.

#pragma once

#include "common_exports.h"
#include "common_behavior.h"  // Behavior
#include "common_goods.h"     // BaseWatcher
#include "common_idea.h"      // MoveIdea, FaceIdea

#include <cstddef>

class IBehaviorManager;
class SystemManager;
struct IObjRW;

// ============================================================================
// LootBehavior -- behavior type 16 ("Loot", GDescLoot in common_behaviortypes.cpp), factory body
// (`PUSH 0x180; CALL operator_new; ... CALL the routine`). RTTI: the COL at
// vftable-4 of module storage names ".?AVLootBehavior@@", single inheritance from Behavior.

// Re-ported from the listing 2026-10-08 (claude-code:cmn-ai5). Until then slots 3/4/5/10 were
// unbound members, the candidate search (the routine / the routine) and the tractor calls
// (the routine / the routine) were no-op stand-ins, and Update skipped the live branch.

// Constructor the routine builds MoveIdea +0x2c (the routine, EH 0) and FaceIdea +0xa4
// (the routine, EH 1), the loot-target slot at +0x174 (EH 4), the vptr and its scalars.

// Vtable module storage, 27 slots. Overrides of Behavior's table:
//    0  the routine  scalar-deleting dtor around the routine
//    1  the branch  SetContext
//    3  the branch  Slot3 (`JMP the routine`, shared with IdleBehavior)
//    4  the routine  Slot4
//    5  the routine  Slot5
//    7  the branch  Slot7
//   10  the routine  Slot10
// ============================================================================
class LootBehavior : public Behavior
{
public:
    // The +0x174 record the routine receives by pointer: the loot object being collected and
    // a byte at +8 that Slot10 sets and the search / release paths clear.
    struct FLootTarget
    {
        BaseWatcher Watcher;            // +0x00
        unsigned char bDirected;        // +0x08
        unsigned char Pad09[3];
    };

    // (module storage-062ce48f).
    explicit LootBehavior(int InTypeId);
    // set_pointer(0) on +0x174, +0xb8 (FaceIdea's watcher), +0x40 (MoveIdea's)
    // -- the members' own destructors -- then the routine.
    ~LootBehavior() override;

    LootBehavior(LootBehavior const&) = delete;
    LootBehavior& operator=(LootBehavior const&) = delete;

    // Slot 1, the branch (RET 0xc).
    bool SetContext(void* Owner, void* Manager, void* Sys) override;
    // Slot 3, the branch: `JMP the routine`.
    void Slot3() override;
    // Slot 4, the routine.
    void Slot4() override;
    // Slot 5, the routine (RET 4, argument unread). Result stored at +0x0c.
    int Slot5(float Unused) override;
    // Slot 7, the branch. Result stored at +0x10 (except the "collected" exit).
    int Slot7() override;
    // Slot 10, the routine (RET 4).
    bool Slot10(void* DirectiveOp) override;

    // (RET 4, non-virtual): true unless the loot object is a nearly-empty
    // container already claimed by another ship's LootBehavior.
    bool IsLootUnclaimed(IObjRW* Loot);
    // (RET 4, non-virtual): pick the nearest collectable loot into Slot.
    bool AcquireLootTarget(FLootTarget* Slot);

    // ---- Behavior's own +0x04..+0x2b block, still declared per class ----
    int TypeId;                     // +0x04
    unsigned CachedHandle08;        // +0x08
    int ModeSelector0xc;            // +0x0c, Slot5's result.
    int UpdateResult10;             // +0x10, Slot7's result.
    void* OwnerContext;             // +0x14, the owner IObjRW* (binder).
    unsigned char Opaque18[0x1c - 0x18];
    float DeltaTime;                // +0x1c
    IBehaviorManager* BehaviorMgrPtr; // +0x20 (binder)
    SystemManager* NavHelper;       // +0x24 (binder)
    unsigned char Flags28;          // +0x28
    unsigned char Flag29;           // +0x29
    unsigned char Pad2a[0x2c - 0x2a];

    // ---- LootBehavior's own fields ----
    MoveIdea Move;                  // +0x2c
    FaceIdea Face;                  // +0xa4
    unsigned char bHasTractor164;   // +0x164, SetContext: the owner carries tractor equipment
    unsigned char Pad165[3];
    float TractorReach168;          // +0x168, SetContext: tractor range * 0.75
    unsigned char Flag16c;          // +0x16c, ctor 1; Slot10's failure path copies op+0x10
    unsigned char Pad16d[3];
    float Field170;                 // +0x170, ctor 13.0f; no reader in this class's code
    FLootTarget Target;             // +0x174
};

static_assert(sizeof(LootBehavior::FLootTarget) == 0x0c, "LootBehavior::FLootTarget size");
static_assert(offsetof(LootBehavior, TypeId) == 0x04, "LootBehavior::TypeId offset");
static_assert(offsetof(LootBehavior, ModeSelector0xc) == 0x0c, "LootBehavior::ModeSelector0xc offset");
static_assert(offsetof(LootBehavior, UpdateResult10) == 0x10, "LootBehavior::UpdateResult10 offset");
static_assert(offsetof(LootBehavior, OwnerContext) == 0x14, "LootBehavior::OwnerContext offset");
static_assert(offsetof(LootBehavior, DeltaTime) == 0x1c, "LootBehavior::DeltaTime offset");
static_assert(offsetof(LootBehavior, BehaviorMgrPtr) == 0x20, "LootBehavior::BehaviorMgrPtr offset");
static_assert(offsetof(LootBehavior, NavHelper) == 0x24, "LootBehavior::NavHelper offset");
static_assert(offsetof(LootBehavior, Flags28) == 0x28, "LootBehavior::Flags28 offset");
static_assert(offsetof(LootBehavior, Flag29) == 0x29, "LootBehavior::Flag29 offset");
static_assert(offsetof(LootBehavior, Move) == 0x2c, "LootBehavior::Move offset");
static_assert(offsetof(LootBehavior, Face) == 0xa4, "LootBehavior::Face offset");
static_assert(offsetof(LootBehavior, bHasTractor164) == 0x164, "LootBehavior::bHasTractor164 offset");
static_assert(offsetof(LootBehavior, TractorReach168) == 0x168, "LootBehavior::TractorReach168 offset");
static_assert(offsetof(LootBehavior, Flag16c) == 0x16c, "LootBehavior::Flag16c offset");
static_assert(offsetof(LootBehavior, Field170) == 0x170, "LootBehavior::Field170 offset");
static_assert(offsetof(LootBehavior, Target) == 0x174, "LootBehavior::Target offset");
static_assert(sizeof(LootBehavior) == 0x180, "LootBehavior size must match operator_new(0x180) at 062d1df7");
