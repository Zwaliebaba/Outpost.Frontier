// Author: Giperion.

// Common.dll -- the behavior type registry: the twenty-one BehaviorTypeDesc descriptors and
// their load-time registry (GBehaviorRegistryInit, dynamic initialisation -- keep this TU's
// globals together and in this order), BehaviorTypes::get_behavior_id/get_behavior_name and
// CREATE_BEHAVIOR.
// CMN-SPLIT stage 1 (2026-10-14): split out of common_iface.cpp unchanged.

#include "common_iface.h"
#include "common_behavior.h" // class Behavior; also FormationBehavior, the id-10 registry entry

// The twenty concrete behavior classes the id 0..20 registry entries construct. BehaviorTypes'
// create_behavior() has to see every one of them, so this is the one translation unit that
// includes the whole family. FormationBehavior lives in common_behavior.h above.
#include "common_buzzbehavior.h"
#include "common_cruisebehavior.h"
#include "common_delaybehavior.h"
#include "common_dockbehavior.h"
#include "common_drasticevadebehavior.h"
#include "common_evadebehavior.h"
#include "common_facebehavior.h"
#include "common_fleebehavior.h"
#include "common_followbehavior.h"
#include "common_freeflightbehavior.h"
#include "common_gotobehavior.h"
#include "common_guidebehavior.h"
#include "common_idlebehavior.h"
#include "common_itlbehavior.h"
#include "common_largeshipbehavior.h"
#include "common_launchbehavior.h"
#include "common_lootbehavior.h"
#include "common_nullbehavior.h"
#include "common_strafebehavior.h"
#include "common_trailbehavior.h"

#include <cstring>
#include <new>

// ============================================================================
// BehaviorTypes
// ============================================================================

// ----------------------------------------------------------------------------
// Behavior type registry — baseline the field / the field / the field.

// Recovered from the twenty-one .CRT$XC static initialisers listed in the
// module init table at module storage..063ea360 (Common),
// one per behavior type. Each one is the same shape, e.g. the id-20 "Delay"
// initialiser at 062d15b0:

//     MOV  dword ptr [063fc540], module storage   ; +0x00 vptr
//     MOV  dword ptr [063fc544], 0x14              ; +0x04 type id
//     ... strlen(name) clamped to 0x1f, MOVSD/MOVSB copy of the literal ...
//     MOV  byte  ptr [063fc54c + len], 0           ; +0x0C name buffer, NUL at len
//     MOV  [063fc548], len                         ; +0x08 cached name length
//     MOV  dword ptr [063fc8e4 + 0x14*4], 063fc540 ; registry[id] = &descriptor
//     INC  [063fc8e0]                              ; ++count

// so the descriptor is exactly {vptr, int Id, int NameLength, char Name[32]} =
// 0x2C bytes, which is precisely the element stride of the 0x063fc540..0x063fc8dc
// block (twenty-one entries, laid out in DESCENDING id order: id 20 first at
// , id 0 "NULL" last at 0x063fc8b0). the field is the count and
// the 21-entry `REP STOSD ECX=0x15`-cleared pointer array
// (zeroed by the initialiser at 062d0b90), running to 0x063fc938.

// Each descriptor's vtable is a three-slot .rdata block sitting 0x0C bytes ahead
// of its own name literal (e.g. 063a002c for "NULL" at 063a0038):
//   slot 0 (+0x00): get_type_id()   — ICF-folded onto TractorArm::GetMode, i.e.
//                                     the one-instruction `return *(this+4)`
//   slot 1 (+0x04): create_behavior() — the only per-type slot (the routine for
//                                     NULL, the routine for Buzz, ... stride 0x70)
//   slot 2 (+0x08): get_name()      — ICF-folded onto the branch, shared by all
//                                     21 vtables (`return this+0x0C`)
// which is exactly what get_behavior_name/get_behavior_id/CREATE_BEHAVIOR below
// call through (+0x08 and +0x04).

// CLOSED 2026-09-17 (claude:common-factory). The note that stood here said create_behavior()
// was "still open" and that every descriptor "inherits the base CreateBehavior() and returns
// nullptr for now". It does not any more: all twenty-one types construct.

// WHAT THE TWENTY-ONE FACTORY BODIES ACTUALLY DO, read off module storage..062d1f90 (the
// twenty-one entries the descriptor vtables' +0x04 slots point at, one per id in ascending id
// order -- their starts are NOT evenly spaced, so they were taken from the .rdata slots and
// from the tracker's own function boundaries, not from an assumed stride):
//     PUSH <size>            ; CALL MSVCRT.DLL::operator_new
//     TEST EAX,EAX / JZ      ; allocation failure returns null
//     MOV  ECX,[ESI + 0x4]   ; ESI is the descriptor; +0x04 is its Id
//     PUSH ECX               ; ...which is the constructor's argument
//     MOV  ECX,EAX
//     CALL <that type's ctor>
// so a factory is exactly `new(nothrow) T(TypeId)`. The ids, baseline sizes and constructors,
// in registry order:
//     0  NULL             0x2c   (base ctor the routine + vptr 063a0244 inline)
//     1  Buzz             0x354  the routine      11 Large Ship (move) 0x180 the routine
//     2  Goto             0x3fc  the routine      12 Cruise            0x2c  the routine
//     3  Trail            0x248  the routine      13 Strafe            0x1bc the routine
//     4  Flee             0xe8   the routine      14 Guide             0x124 the routine
//     5  Evade            0x2c0  the routine      15 Face              0x1dc the routine
//     6  Idle             0x40   the routine      16 Loot              0x180 the routine
//     7  Dock             0x470  the routine      17 Follow            0x2bc the routine
//     8  Launch           0x234  the routine      18 DrasticEvade      0x44  the routine
//     9  InstantTradeLane 0x5c   the routine      19 FreeFlight        0x400 the routine
//    10  Formation        0x2b8  the routine      20 Delay             0x48  the routine
// Every one of those constructors is now `explicit XBehavior(int InTypeId)` in this port, and
// every one of those sizes is pinned by a `static_assert(sizeof(X) <= ...)` in the class's own
// header. Id 8 deserves a note: several tracker items still describe that row as having an
// "empty-string name", because analysis found no string literal inline in the .rdata row at
// . The name is not inline -- the id-8 static initialiser at module storage copies it
// from module storage, and it is "Launch". The row is LaunchBehavior's.

// HOW THE PER-TYPE SLOT IS MODELLED. baseline has twenty-one descriptor vtables, identical
// except for slot +0x04. The port gets that with one template subclass per behavior type
// (TBehaviorTypeDesc<T> below): each instantiation is its own C++ type with its own vtable and
// its own create_behavior override, it adds no data member and no new virtual, so every
// descriptor is still exactly the {vptr, int Id, int NameLength, char Name[32]} = 0x2C object
// the initialisers build.
class BehaviorTypeDesc;

// the number of registered descriptors; the field -- registry[id] =
// &descriptor, a fixed 0x15-entry static. baseline's first initialiser 062d0b90 zeroes both
// (count = 0, REP STOSD ECX=0x15 over the array) before the twenty-one descriptor
// initialisers run; static zero-initialisation does the same here, ahead of every dynamic
// initialiser in this TU. Each descriptor then registers itself (BehaviorTypeDesc's ctor).
static int GBehaviorCount;
static BehaviorTypeDesc* GBehaviorRegistry[0x15];

class BehaviorTypeDesc
{
public:
    explicit BehaviorTypeDesc(int InTypeId, char const* InName)
        : TypeId(InTypeId), NameLength(0)
    {
        // strlen, clamp to 0x1f, copy, terminate at [len].
        int Length = static_cast<int>(strlen(InName));
        if (Length > 0x1F)
            Length = 0x1F;
        memcpy(Name, InName, static_cast<size_t>(Length));
        Name[Length] = '\0';
        NameLength = Length;
        // (id 20; same tail in all twenty-one initialisers):
        // registry[id] = &descriptor, then ++count.
        GBehaviorRegistry[TypeId] = this;
        ++GBehaviorCount;
    }
    // No virtual destructor, deliberately: baseline's vtable is exactly three
    // slots (063a002c..063a0034) with get_type_id first, and these descriptors
    // are file-scope statics that are never deleted through a base pointer.
    // Adding one would insert an MSVC scalar-deleting-dtor slot at index 0 and
    // shift get_name off the +0x08 offset the three callers below hand-index.

    virtual int get_type_id() const { return TypeId; }           // vtable +0x00
    // vtable +0x04. Only the twenty-one TBehaviorTypeDesc<T> instantiations below ever reach
    // the registry, so this base body is unreachable through it; it stays non-pure so the
    // class is still a plain constructible descriptor, and null is what baseline's own
    // allocation-failure path returns.
    virtual Behavior* create_behavior() { return nullptr; }      // vtable +0x04
    virtual char const* get_name() const { return Name; }        // vtable +0x08

    int TypeId;         // +0x04
    int NameLength;     // +0x08
    char Name[32];      // +0x0C
};

// One C++ type per baseline descriptor vtable. Adds no data and no new virtual, so
// sizeof(TBehaviorTypeDesc<T>) == sizeof(BehaviorTypeDesc) == 0x2C for every T.

// `new (std::nothrow)` rather than plain `new` is deliberate and is the faithful reading:
// baseline calls MSVCRT's `operator new`, which in that CRT returns null on failure, and every
// factory body has an explicit `TEST EAX,EAX; JZ <return 0>` for exactly that case. Throwing
// here would turn a path baseline handles into one it does not.
template <class BehaviorClass>
class TBehaviorTypeDesc : public BehaviorTypeDesc
{
public:
    TBehaviorTypeDesc(int InTypeId, char const* InName)
        : BehaviorTypeDesc(InTypeId, InName)
    {
    }

    Behavior* create_behavior() override
    {
        return new (std::nothrow) BehaviorClass(TypeId);
    }
};
static_assert(sizeof(TBehaviorTypeDesc<NullBehavior>) == sizeof(BehaviorTypeDesc),
              "a typed descriptor must stay the same 0x2C object baseline's initialisers build");

// baseline id order, read straight off the twenty-one initialisers (the literal
// pushed as the id at +0x04 is given in the comment on each line; the class each one
// constructs is the ctor its own factory body calls -- see the table in the comment above).
static TBehaviorTypeDesc<NullBehavior>             GDescNull(0, "NULL");
static TBehaviorTypeDesc<BuzzBehavior>             GDescBuzz(1, "Buzz");
static TBehaviorTypeDesc<GotoBehavior>             GDescGoto(2, "Goto");
static TBehaviorTypeDesc<TrailBehavior>            GDescTrail(3, "Trail");
static TBehaviorTypeDesc<FleeBehavior>             GDescFlee(4, "Flee");
static TBehaviorTypeDesc<EvadeBehavior>            GDescEvade(5, "Evade");
static TBehaviorTypeDesc<IdleBehavior>             GDescIdle(6, "Idle");
static TBehaviorTypeDesc<DockBehavior>             GDescDock(7, "Dock");
static TBehaviorTypeDesc<LaunchBehavior>           GDescLaunch(8, "Launch");
static TBehaviorTypeDesc<InstantTradeLaneBehavior> GDescItl(9, "InstantTradeLane");
static TBehaviorTypeDesc<FormationBehavior>        GDescFormation(10, "Formation");
static TBehaviorTypeDesc<LargeShipBehavior>        GDescLargeShip(11, "Large Ship (move)");
static TBehaviorTypeDesc<CruiseBehavior>           GDescCruise(12, "Cruise");
static TBehaviorTypeDesc<StrafeBehavior>           GDescStrafe(13, "Strafe");
static TBehaviorTypeDesc<GuideBehavior>            GDescGuide(14, "Guide");
static TBehaviorTypeDesc<FaceBehavior>             GDescFace(15, "Face");
static TBehaviorTypeDesc<LootBehavior>             GDescLoot(16, "Loot");
static TBehaviorTypeDesc<FollowBehavior>           GDescFollow(17, "Follow");
static TBehaviorTypeDesc<DrasticEvadeBehavior>     GDescDrasticEvade(18, "DrasticEvade");
static TBehaviorTypeDesc<FreeFlightBehavior>       GDescFreeFlight(19, "FreeFlight");
static TBehaviorTypeDesc<DelayBehavior>            GDescDelay(20, "Delay");

// NEURONCOMPONENTS.DLL!stricmp (Common IAT 0639902c), common_neuroncomponents_stricmp.cpp.
int CallNeuronComponentsStricmp(const char* Left, const char* Right);

// The three lookups below index the registry by id with no range test, as baseline does: every
// id that reaches them is 0..20 (BehaviorManager's CREATE_BEHAVIOR loop) or -1 (handled by
// get_behavior_name). baseline's array is a fixed 0x15-dword static followed by a float at
// , so an out-of-range id there reads a junk pointer; the port has the same hole.

int __cdecl BehaviorTypes::get_behavior_id(char const* Name)
{
    // linear search, the count re-read from the field on every
    // pass; get_name() (+0x08) of each entry, no null test on the entry; NEURONCOMPONENTS stricmp with
    // the entry's name first.
    for (int I = 0; I < GBehaviorCount; ++I)
    {
        if (CallNeuronComponentsStricmp(GBehaviorRegistry[I]->get_name(), Name) == 0)
            return I;
    }
    return -1;
}

char const* __cdecl BehaviorTypes::get_behavior_name(int Id)
{
    // . Id -1 returns the string the field points at.
    if (Id == -1)
        return "BEHAVIOR_ID_NONE";
    BehaviorTypeDesc* Desc = GBehaviorRegistry[Id];
    if (Desc == nullptr)
        return nullptr;
    return Desc->get_name();                                   // JMP [vt+0x08]
}

Behavior* __cdecl CREATE_BEHAVIOR(int param_1)
{
    // registry[id]; null -> 0, else JMP [vt+0x04] create_behavior().
    BehaviorTypeDesc* Desc = GBehaviorRegistry[param_1];
    if (Desc == nullptr)
        return nullptr;
    return Desc->create_behavior();
}
