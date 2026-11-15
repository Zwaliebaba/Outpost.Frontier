// Author: Giperion.

#pragma once

#include "common_exports.h"
#include "common_behavior.h"  // Behavior
#include "common_goods.h"     // BaseWatcher
#include "common_idea.h"      // MatchThrottle2Idea, FaceIdea, MoveIdea, AfterburnerIdea

#include <cstddef>

class IBehaviorManager;
class SystemManager;
struct IObjRW;

// ============================================================================
// TrailBehavior -- behavior type 3 ("Trail", GDescTrail in common_behaviortypes.cpp). RTTI: COL
// at vftable-4 names ".?AVTrailBehavior@@"; its ClassHierarchyDescriptor
// (063a8f14, numBaseClasses 2) is [TrailBehavior, Behavior], single inheritance at offset 0.

// Re-ported from the listing 2026-10-08 (claude-code:cmn-ai5). Until then the slot-4 body
// lived in the destructor, slots 3/5/6/8/10/12/14 were unbound members or missing, the
// +0x2c sub-object was the invented FormationMoveFx, the +0x1c0 AfterburnerIdea was a byte
// buffer, and Update ran on no-op stand-ins.

// Constructor the routine builds, in this order (EH states 0..4):
//   +0x02c  MatchThrottle2Idea  the routine
//   +0x088  FaceIdea            the routine
//   +0x148  MoveIdea            the routine
//   +0x1c0  AfterburnerIdea     the routine
// then the bare BaseWatcher at +0x1d4 (EH state 5), the vptr 0639fa6c and its scalars.

// Vtable module storage, 27 slots. Overrides of Behavior's table (vtd.py, raw .rdata):
//    0  the routine  scalar-deleting dtor around the routine
//    1  the branch  SetContext
//    3  the branch  Slot3
//    4  the routine  Slot4
//    5  the branch  Slot5
//    6  the branch  evaluate
//    7  the branch  Slot7 (the per-tick update)
//    8  the branch  Slot8
//   10  the branch  Slot10
//   12  the routine  set_direction
//   14  the branch  set_cruise
// Every other slot is Behavior's own entry.
// ============================================================================
class TrailBehavior : public Behavior
{
public:
    // (module storage-062ca496).
    explicit TrailBehavior(int InTypeId);
    // set_pointer(0) on +0x1d4, +0x15c (MoveIdea's watcher), +0x9c (FaceIdea's)
    // and +0x40 (MatchThrottle2Idea's) -- the members' own destructors in reverse order --
    // then the routine (~Behavior).
    ~TrailBehavior() override;

    TrailBehavior(TrailBehavior const&) = delete;
    TrailBehavior& operator=(TrailBehavior const&) = delete;

    // Slot 1, the branch (RET 0xc).
    bool SetContext(void* Owner, void* Manager, void* Sys) override;
    // Slot 3, the branch (RET 0): re-arm from the personality's TrailUseStruct.
    void Slot3() override;
    // Slot 4, the routine (RET 0): release everything, then Behavior::Slot4.
    void Slot4() override;
    // Slot 5, the branch (RET 4, argument unread). Result stored at +0x0c.
    int Slot5(float Unused) override;
    // Slot 6, the branch (RET 4, argument unread): retarget the owner's target for the
    // player trail.
    EvaluateResult evaluate(unsigned int Context) override;
    // Slot 7, the branch (516 instructions). Result stored at +0x10.
    int Slot7() override;
    // Slot 8, the branch (RET 0): MatchThrottle2Idea::Slot2(DeltaTime) while it drives.
    void Slot8() override;
    // Slot 10, the branch (RET 4): take the DirectiveTrailOp's target and distance.
    bool Slot10(void* DirectiveOp) override;
    // Slot 12, the routine (RET 4).
    void set_direction(Vector const& Dir) override;
    // Slot 14, the branch: `ADD ECX,0x2c; JMP the routine` -- MatchThrottle2Idea::SetFlag38.
    void set_cruise(bool bOn) override;

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

    // ---- TrailBehavior's own fields ----
    MatchThrottle2Idea MatchThrottle; // +0x2c
    FaceIdea Face;                  // +0x88
    MoveIdea Move;                  // +0x148
    AfterburnerIdea Afterburner;    // +0x1c0
    BaseWatcher DirectiveTarget;    // +0x1d4, Slot10's DirectiveTrailOp target
    unsigned LeaderId1dc;           // +0x1dc, id of the ship last trailed (Slot3 / Slot7)
    unsigned char bHasDirectiveTarget1e0; // +0x1e0, Slot10 sets, SetContext / Slot4 clear
    unsigned char Pad1e1[3];
    float TrailOffset1e4;           // +0x1e4, ctor 0; Slot7: leader extent + TrailDistance1e8
    float TrailDistance1e8;         // +0x1e8, ctor 50.0f; op +0x10, set_direction z, TrailParams.TrailDistance
    unsigned char bLagging1ec;      // +0x1ec, not written by the ctor; Slot3 clears
    unsigned char Pad1ed[3];
    float FacingCos1f0;             // +0x1f0, ctor cos(pi/6); Slot3 cos(TrailParams.TrailLockConeAngle)
    float LagTimer1f4;              // +0x1f4, not written by the ctor; Slot3 clears
    float AxisTimer1f8;             // +0x1f8, not written by the ctor; Slot3 clears
    float LagTime1fc;               // +0x1fc, ctor 2.5f; TrailParams.TrailBreakTime
    float AxisPause200;             // +0x200, ctor 5.0f; never reloaded
    float AxisTime204;              // +0x204, ctor 3.0f; TrailParams.TrailMinNoLockTime
    float AxisZ208;                 // +0x208, ctor 0; TrailParams.TrailBreakRollThrottle
    unsigned char bAfterburner20c;  // +0x20c, ctor 0; TrailParams.bTrailBreakAfterburner
    unsigned char Pad20d[3];
    float Scale210;                 // +0x210, ctor 1.0f; TrailParams.TrailMaxTurnThrottle (also FaceIdea +0x94)
    unsigned char bAxisArmed214;    // +0x214, ctor 0
    unsigned char Pad215[3];
    float SightTimer218;            // +0x218, not written by the ctor; Slot3 clears
    float SightTime21c;             // +0x21c, ctor 4.0f
    float TrailExtent220;           // +0x220, not written by the ctor; Slot7
    float Field224;                 // +0x224, ctor 0x3d1de9e7; no reader in this class's code
    float Field228;                 // +0x228, ctor 400.0f; no reader in this class's code
    unsigned char bDirectionSet22c; // +0x22c, set_direction sets, ctor / Slot3 clear
    unsigned char Pad22d[3];
    float DirectionX230;            // +0x230, set_direction: -Dir.y normalised, clamped [-1,1]
    float DirectionY234;            // +0x234, set_direction: -Dir.x normalised, clamped [-1,1]
    float DirectionZ238;            // +0x238, set_direction: 0
    unsigned char bMatchThrottle23c; // +0x23c, Slot7: the MatchThrottle2Idea is driving
    unsigned char Pad23d[3];
    float Field240;                 // +0x240, ctor 20.0f; no reader in this class's code
    float CatchUpRange244;          // +0x244, ctor 1500.0f; handed to MatchThrottle2Idea
};

static_assert(offsetof(TrailBehavior, TypeId) == 0x04, "TrailBehavior::TypeId offset");
static_assert(offsetof(TrailBehavior, ModeSelector0xc) == 0x0c, "TrailBehavior::ModeSelector0xc offset");
static_assert(offsetof(TrailBehavior, UpdateResult10) == 0x10, "TrailBehavior::UpdateResult10 offset");
static_assert(offsetof(TrailBehavior, OwnerContext) == 0x14, "TrailBehavior::OwnerContext offset");
static_assert(offsetof(TrailBehavior, DeltaTime) == 0x1c, "TrailBehavior::DeltaTime offset");
static_assert(offsetof(TrailBehavior, BehaviorMgrPtr) == 0x20, "TrailBehavior::BehaviorMgrPtr offset");
static_assert(offsetof(TrailBehavior, NavHelper) == 0x24, "TrailBehavior::NavHelper offset");
static_assert(offsetof(TrailBehavior, Flags28) == 0x28, "TrailBehavior::Flags28 offset");
static_assert(offsetof(TrailBehavior, Flag29) == 0x29, "TrailBehavior::Flag29 offset");
static_assert(offsetof(TrailBehavior, MatchThrottle) == 0x2c, "TrailBehavior::MatchThrottle offset");
static_assert(offsetof(TrailBehavior, Face) == 0x88, "TrailBehavior::Face offset");
static_assert(offsetof(TrailBehavior, Move) == 0x148, "TrailBehavior::Move offset");
static_assert(offsetof(TrailBehavior, Afterburner) == 0x1c0, "TrailBehavior::Afterburner offset");
static_assert(offsetof(TrailBehavior, DirectiveTarget) == 0x1d4, "TrailBehavior::DirectiveTarget offset");
static_assert(offsetof(TrailBehavior, LeaderId1dc) == 0x1dc, "TrailBehavior::LeaderId1dc offset");
static_assert(offsetof(TrailBehavior, bHasDirectiveTarget1e0) == 0x1e0, "TrailBehavior::bHasDirectiveTarget1e0 offset");
static_assert(offsetof(TrailBehavior, TrailOffset1e4) == 0x1e4, "TrailBehavior::TrailOffset1e4 offset");
static_assert(offsetof(TrailBehavior, TrailDistance1e8) == 0x1e8, "TrailBehavior::TrailDistance1e8 offset");
static_assert(offsetof(TrailBehavior, bLagging1ec) == 0x1ec, "TrailBehavior::bLagging1ec offset");
static_assert(offsetof(TrailBehavior, FacingCos1f0) == 0x1f0, "TrailBehavior::FacingCos1f0 offset");
static_assert(offsetof(TrailBehavior, LagTimer1f4) == 0x1f4, "TrailBehavior::LagTimer1f4 offset");
static_assert(offsetof(TrailBehavior, AxisTimer1f8) == 0x1f8, "TrailBehavior::AxisTimer1f8 offset");
static_assert(offsetof(TrailBehavior, LagTime1fc) == 0x1fc, "TrailBehavior::LagTime1fc offset");
static_assert(offsetof(TrailBehavior, AxisPause200) == 0x200, "TrailBehavior::AxisPause200 offset");
static_assert(offsetof(TrailBehavior, AxisTime204) == 0x204, "TrailBehavior::AxisTime204 offset");
static_assert(offsetof(TrailBehavior, AxisZ208) == 0x208, "TrailBehavior::AxisZ208 offset");
static_assert(offsetof(TrailBehavior, bAfterburner20c) == 0x20c, "TrailBehavior::bAfterburner20c offset");
static_assert(offsetof(TrailBehavior, Scale210) == 0x210, "TrailBehavior::Scale210 offset");
static_assert(offsetof(TrailBehavior, bAxisArmed214) == 0x214, "TrailBehavior::bAxisArmed214 offset");
static_assert(offsetof(TrailBehavior, SightTimer218) == 0x218, "TrailBehavior::SightTimer218 offset");
static_assert(offsetof(TrailBehavior, SightTime21c) == 0x21c, "TrailBehavior::SightTime21c offset");
static_assert(offsetof(TrailBehavior, TrailExtent220) == 0x220, "TrailBehavior::TrailExtent220 offset");
static_assert(offsetof(TrailBehavior, Field224) == 0x224, "TrailBehavior::Field224 offset");
static_assert(offsetof(TrailBehavior, Field228) == 0x228, "TrailBehavior::Field228 offset");
static_assert(offsetof(TrailBehavior, bDirectionSet22c) == 0x22c, "TrailBehavior::bDirectionSet22c offset");
static_assert(offsetof(TrailBehavior, DirectionX230) == 0x230, "TrailBehavior::DirectionX230 offset");
static_assert(offsetof(TrailBehavior, DirectionZ238) == 0x238, "TrailBehavior::DirectionZ238 offset");
static_assert(offsetof(TrailBehavior, bMatchThrottle23c) == 0x23c, "TrailBehavior::bMatchThrottle23c offset");
static_assert(offsetof(TrailBehavior, Field240) == 0x240, "TrailBehavior::Field240 offset");
static_assert(offsetof(TrailBehavior, CatchUpRange244) == 0x244, "TrailBehavior::CatchUpRange244 offset");
static_assert(sizeof(TrailBehavior) == 0x248, "TrailBehavior size must match baseline 0x248");
