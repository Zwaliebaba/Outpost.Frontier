// Author: Giperion.

#pragma once

#include "common_exports.h"
#include "common_behavior.h"  // Behavior
#include "common_pub.h"       // pub::AI::DirectiveGotoOp, pub::AI::DirectiveDockOp

#include <cstddef>

// ============================================================================
// FleeBehavior -- AI "break off and run" state (Behavior family)
// ============================================================================

// RTTI identity (hand-decoded directly from Common raw
// bytes, NOT from analysis's .c analysis -- see "ground truth precedence"
// in this marathon's methodology):

//   RTTICompleteObjectLocator @ module storage (offset field = 0, i.e. the
//   primary -- and only -- vtable installs at object offset 0)
//     -> pTypeDescriptor @ module storage, name ".?AVFleeBehavior@@"
//     -> pClassHierarchyDescriptor @ module storage, numBaseClasses = 2,
//        BaseClassArray @ 0x063a8da0 = [ FleeBehavior(self, numContained=1),
//                                        Behavior(numContained=0, mdisp=0,
//                                        the SAME shared Behavior
//                                        BaseClassDescriptor every other
//                                        sibling in this family reuses) ]

// Single-inheritance direct child of Behavior -- same shape as
// IdleBehavior/CruiseBehavior, no secondary vtable. baseline's own debug strings in
// .rdata name three of its methods: "FleeBehavior::Perform()" (slot 7, 0639f6b8 and
// ), "FleeBehavior::deactivate()" (slots 3 and 4, 0639f7a8) and
// "FleeBehavior::event_handler()" (slot 22, 0639f810).

// ----------------------------------------------------------------------------
// Constructor: the routine (Common). the routine (Behavior's
// own field initialisation), DirectiveGotoOp at +0x4c, DirectiveDockOp at +0xbc,
// byte +0x2c = 0, byte +0x38 = 0, +0x34 = 0, vptr, +0x30 = 4.
// ============================================================================

class FleeBehavior : public Behavior
{
public:
    // baseline's ctor argument is the behavior type id (4, "Flee"), pushed by the factory out of
    // the descriptor's +0x04 -- see Behavior::GetTypeId() in common_behavior.h.
    explicit FleeBehavior(int InTypeId);
    ~FleeBehavior() override;

    // ------------------------------------------------------------------
    // Vtable module storage, 27 slots. Overrides of Behavior's table (read off the raw
    // .rdata dwords, 2026-10-08):
    //   0  the routine  scalar-deleting dtor, ICF-shared by 7 behavior vtables
    //   1  the branch  `JMP the routine` -- Behavior::BindContext
    //   3  the branch  Slot3 below
    //   4  the branch  Slot4 below
    //   5  the branch  Slot5 below
    //   7  the branch  Perform
    //  22  the branch  Slot22 below
    // Every other slot is Behavior's own entry.
    // ------------------------------------------------------------------

    // Slot 1, the branch: `JMP the routine`.
    bool SetContext(void* Owner, void* Manager, void* Sys) override;

    // Slot 3, the branch (Common). Re-reads the three delegates out
    // of the manager's dispatch table; if a flee is running (Field2c), re-arms whichever
    // delegate it drives through that delegate's own slot 3.
    void Slot3() override;

    // Slot 4, the branch (Common). Same delegate refresh; with
    // Field38 set, Field44's slot 4; otherwise the mode-selected delegate's slot 4 and, if
    // the manager's current directive is the delegate's own kind and Field48 says Perform
    // took a copy of it, the copied op is handed back through the delegate's slot 10.
    void Slot4() override;

    // Slot 5, the branch (Common), `RET 4`, the argument unread.
    // The flee evaluation: hull+shield ratio against the personality's flee threshold,
    // the TargetManager's hostile power ratio against a per-level table, and the "flee
    // when no ready weapon" switch. Returns 0 (flee) or 2, or 3 when +0x29 is set; the
    // value is also stored at +0x0c.
    int Slot5(float Unused) override;

    // Slot 7, the branch (Common), baseline "FleeBehavior::Perform()".
    int Perform();
    int Slot7() override { return Perform(); }

    // Slot 22, the branch (Common), baseline
    // "FleeBehavior::event_handler()". Same delegate refresh, then tail-jumps the event
    // into the active delegate's own slot 22.
    void Slot22(void* Event) override;

private:
    // The delegate refresh all four of slots 3/4/5/22 begin with:
    // Field3c = Manager->BehaviorDispatchTable[2] (+0x17c, Goto), Field40 = [7] (+0x190,
    // Dock), Field44 = [10] (+0x19c, Formation).
    void RefreshDelegates();

public:
    // ------------------------------------------------------------------
    // Fields, ascending offset. +0x04..+0x29 is Behavior's own block (see common_behavior.h),
    // still declared here until the family-wide hoist.
    // ------------------------------------------------------------------
    int TypeId;  // +0x04. The type id baseline's shared base ctor the routine parks here, taken
                 // from this class's own ctor argument -- see Behavior::GetTypeId() (common_behavior.h)
                 // for the twenty-one-factory evidence.
    unsigned CachedHandle08;   // +0x08. IObjInspect::get_id() of the owner (the binder).
    int ModeSelector0xc;       // +0x0c. Slot5's result.
    int UpdateResult10;        // +0x10. Perform's result.
    void* OwnerContextPtr14;   // +0x14. The owner (the binder).
    unsigned char Opaque18[0x20 - 0x18];
    // +0x20. The IBehaviorManager (the binder). Receiver proof: module storage
    // `MOV ECX,[EAX+0x20]` then [ECX+0x17c]/[ECX+0x190]/[ECX+0x19c] (the dispatch table), and
    // /062c7472/062c769d/062c76d9 `MOV ECX,[ESI+0x20]; CALL
    // IBehaviorManager::get_current_directive`.
    IBehaviorManager* BehaviorMgrPtr20;
    SystemManager* NavHelperPtr24; // +0x24. The SystemManager (the binder).
    unsigned char Flags28;     // +0x28.
    unsigned char Flag29;      // +0x29. Slot5 answers 3 while it is set.
    unsigned char Pad2a[0x2c - 0x2a];

    unsigned char Field2c;     // +0x2c. "A flee is running": Perform latches it on the first
                               // tick and clears it when the delegate finishes; Slot5 sets it
                               // when the watched target's own manager is fleeing.
    unsigned char Pad2d[0x30 - 0x2d];
    int Field30;               // +0x30. The FleeReason handed to the content callback: ctor 4;
                               // Slot5 writes 1 (target is fleeing), 2 (hull+shield ratio),
                               // 0 (hostile power ratio) or 3 (no ready weapon).
    int Field34;               // +0x34. FleeDestinationType Perform got back: 1/3 -> Goto, 2 -> Dock.
    unsigned char Field38;     // +0x38. Slot5 sets it when the watched target is itself
                               // fleeing; then Field44 (Formation) is the delegate.
    unsigned char Pad39[0x3c - 0x39];
    Behavior* Field3c;         // +0x3c. Manager->BehaviorDispatchTable[2], a GotoBehavior.
    Behavior* Field40;         // +0x40. Manager->BehaviorDispatchTable[7], a DockBehavior.
    Behavior* Field44;         // +0x44. Manager->BehaviorDispatchTable[10], a FormationBehavior.
    unsigned char Field48;     // +0x48. Perform sets it when the current directive is the
                               // delegate's own kind and it copied that directive's op.
    unsigned char Pad49[0x4c - 0x49];

    pub::AI::DirectiveGotoOp GotoOp;  // +0x4c, 0x70 bytes.
    pub::AI::DirectiveDockOp DockOp;  // +0xbc, 0x2c bytes, ends at +0xe8.
};

static_assert(offsetof(FleeBehavior, CachedHandle08) == 0x08, "FleeBehavior::CachedHandle08 offset");
static_assert(offsetof(FleeBehavior, ModeSelector0xc) == 0x0c, "FleeBehavior::ModeSelector0xc offset");
static_assert(offsetof(FleeBehavior, UpdateResult10) == 0x10, "FleeBehavior::UpdateResult10 offset");
static_assert(offsetof(FleeBehavior, OwnerContextPtr14) == 0x14, "FleeBehavior::OwnerContextPtr14 offset");
static_assert(offsetof(FleeBehavior, BehaviorMgrPtr20) == 0x20, "FleeBehavior::BehaviorMgrPtr20 offset");
static_assert(offsetof(FleeBehavior, NavHelperPtr24) == 0x24, "FleeBehavior::NavHelperPtr24 offset");
static_assert(offsetof(FleeBehavior, Flags28) == 0x28, "FleeBehavior::Flags28 offset");
static_assert(offsetof(FleeBehavior, Flag29) == 0x29, "FleeBehavior::Flag29 offset");
static_assert(offsetof(FleeBehavior, Field2c) == 0x2c, "FleeBehavior::Field2c offset");
static_assert(offsetof(FleeBehavior, Field30) == 0x30, "FleeBehavior::Field30 offset");
static_assert(offsetof(FleeBehavior, Field34) == 0x34, "FleeBehavior::Field34 offset");
static_assert(offsetof(FleeBehavior, Field38) == 0x38, "FleeBehavior::Field38 offset");
static_assert(offsetof(FleeBehavior, Field3c) == 0x3c, "FleeBehavior::Field3c offset");
static_assert(offsetof(FleeBehavior, Field40) == 0x40, "FleeBehavior::Field40 offset");
static_assert(offsetof(FleeBehavior, Field44) == 0x44, "FleeBehavior::Field44 offset");
static_assert(offsetof(FleeBehavior, Field48) == 0x48, "FleeBehavior::Field48 offset");
static_assert(offsetof(FleeBehavior, GotoOp) == 0x4c, "FleeBehavior::GotoOp offset");
static_assert(offsetof(FleeBehavior, DockOp) == 0xbc, "FleeBehavior::DockOp offset");

// baseline object size: the factory pushes 0xe8 before operator new.
static_assert(sizeof(FleeBehavior) == 0xe8, "FleeBehavior size must match baseline 0xe8");
