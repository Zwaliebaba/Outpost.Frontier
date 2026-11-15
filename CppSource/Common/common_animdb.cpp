// Author: Giperion.

// common_animdb.cpp — AnimDB subsystem port from Common.dll
// Listing references: Shutdown 0x1d470, Add(VChannel*) 0x1d4a0, Rem(VChannel*) 0x1d680,
//   Add(int) 0x1d8a0, Rem(int) 0x1dc80, Update 0x1dda0, Count 0x1e480

// Data-structure model corrected 2026-09-06 (recovered from stranded worktree
// vigilant-benz-4251dc) against Common analysis lines 36519-36933,
// 37247-37325, 37511-37560, 38167-38190, 38468-38675, 38717-38760:
//   - FAnimEntry (the field list) embeds a genuine MSVC6 std::list sub-object
//     at +0x14/+0x18 (Myhead/Mysize), constructed via the routine (node buy) +
// (payload copy-ctor). We use a real std::list member instead of
//     hand-rolled sentinel pointers. (Element type corrected to CObject* on
//     2026-09-17 — see the following comment block.)
//   - AnimDB::Rem(VChannel*) calls the routine (skipped, id 617) to destroy that
//     embedded sub-list *before* recycling the node — FreeEntry() now does the
//     equivalent (Sub.Members.clear()) before pushing onto the free list.
//   - the field (the PlaybackId collection) is a real MSVC6 std::_Tree
//     (red-black tree), confirmed via the classic _Tree::_Inc idiom in
//     AnimDB::Rem(int) (`*(char*)(node[2]+0x21) == 0`, the _Isnil byte at
//     node+0x21; node+0x00/0x04/0x08 = Left/Parent/Right; key at node+0x0C)
//     and the lower_bound/upper_bound helpers the routine/the routine. Its
//     insert (the routine) returns a "newly inserted" bool that gates
//     AnimDB::Add(int)'s whole channel-population loop (Common
//     36713-36720: `if (local_4c == '\0') return;`) — the classic
//     std::map::insert()-> pair<iterator,bool> shape, i.e. unique keys, not
//     duplicates. We replace the hand-rolled red-black tree with
//     std::map<int, FAnimSub> — matching this repo's established convention
//     (see common_physys.cpp: baseline std::map/std::_Tree collections are
//     ported as real/standard containers, not hand-rolled MSVC6 node layouts).

// Update() retraced instruction-by-instruction against Common
// on 2026-09-17 (see the block comment on Update below). That pass also settled the
// element type of BOTH embedded sub-lists: they hold CObject*, not VChannel*/int.
//   - Add(VChannel*) Commond661 stores [ESP+0x3c] into the new node's
//     payload; that slot was written at 0627d618 from EBX == the CObject* returned by
//     IEngine::GetInstanceUserData(Chan->GetId()).
//   - Add(int) Common calls the routine(newnode+8, &pCStack_64) where
//     pCStack_64 is likewise the GetInstanceUserData CObject*.
//   - Update's own phase D dereferences every sub-list element at +4 and feeds the
//     result to IEngine::GetRelatedInstance as an instance id, which only makes sense
//     for a CObject* (CObject+0x04 == EngineInstanceId).
// Both collections therefore share one 0x10-byte value type, modelled below as
// FAnimSub { CObject* Root; std::list<CObject*> Members; }.

#include "common_cobject.h"
#include "common_csys.h"  // Acquire3DMathEngine
#include "common_physys.h"  // FLControllerGolem entry points, PhySys::GetCurrentTime
#include "../IVP/ivp_physics/ivp_core.h"  // ::IVP_Time, GetCurrentTime's return type
#include "common_animdb.h"
#include "neuroncomponents.h"
#include "x86math.h"  // I3DMathEngine::MatrixToQuaternion, FQuaternion, FMatrix

#include <NeuronRuntime.h>

#include <algorithm>
#include <list>
#include <map>
#include <vector>

// AnimDB::VChannel (abstract, Tick slot 0 / GetId slot 1, both __thiscall) is declared in
// common_animdb.h.

namespace AnimDB {

// ============================================================================
// FAnimSub — the 0x10-byte value type BOTH AnimDB collections store. baseline's
// Update() proves the two are layout-identical: its phase D pushes a pointer to
// this sub-object from either collection into one shared vector<FAnimSub*> and
// then walks all of them through a single code path (Common
// pushes ESI == treenode+0x10; 58788-58794 pushes EDI == listnode+0x0C; both are
// consumed at 58830-58836 as `p+4` = the std::list object, `[p+8]` = its _Head).
//   +0x00  Root (CObject*) — GetRoot() of the resolved engine instance
//   +0x04  embedded std::list<CObject*>: allocator byte (+0x04, the slot both
//          earlier revisions of this file called "unknown/reserved"),
//          _Myhead (+0x08), _Mysize (+0x0C)
// Sub-list element type: see the file header comment. It is CObject*, and every
// entry gets at most one element (the engine instance whose own
// EngineInstanceId matches the channel/playback id that produced it).
// ============================================================================

struct FAnimSub
{
    CObject* Root = nullptr;     // +0x00
    std::list<CObject*> Members; // +0x04 alloc / +0x08 _Myhead / +0x0C _Mysize
};

// ============================================================================
// FAnimEntry — node for the VChannel list (the field sentinel), size 0x1c
// in baseline:
//   +0x00  Next (FAnimEntry*)
//   +0x04  Prev (FAnimEntry*)
//   +0x08  Channel (VChannel*)
//   +0x0C  FAnimSub (0x10 bytes, above) — 0x0c+0x10 = 0x1c, the whole node.
// ============================================================================

struct FAnimEntry
{
    FAnimEntry* Next;   // +0x00
    FAnimEntry* Prev;   // +0x04
    VChannel* Channel;  // +0x08
    FAnimSub Sub;       // +0x0C .. +0x1B
};

// ============================================================================
// The PlaybackId collection (the field tree) maps int -> FAnimSub.
// baseline node layout (std::_Tree node, confirmed via the routine/f600/f620/
// f670/f6a0/f750/f780): +0x00 Left, +0x04 Parent, +0x08 Right, +0x0C key
// (PlaybackId), +0x10..+0x1F the FAnimSub value, +0x20/+0x21 Color/Isnil.
// An earlier revision of this file carried a redundant PlaybackId field in the
// mapped value and typed the sub-list as std::list<int>; baseline's node has
// neither (the key IS the PlaybackId and nothing reads it back out of the
// value), so both are gone.
// ============================================================================

// std::_Tree<int, FAnimSub> keyed by PlaybackId.
// AnimDB::Add(int)'s insert result carries a "newly inserted" bool (local_4c,
// Common: `if (local_4c == '\0') return;` right after the insert
// helper the routine — the classic std::map::insert() -> pair<iterator,bool>
// shape) and the whole channel-population loop is skipped when the key already
// existed, i.e. re-Add()ing an id already present is a no-op. That is
// find-or-insert-once semantics, i.e. unique keys — a std::map, not a
// std::multimap. AnimDB::Rem(int)'s [lower_bound, upper_bound) walk still works
// identically on a unique-key map (the range is exactly 0 or 1 elements), so it
// does not imply duplicate keys.
using PlayMap = std::map<int, FAnimSub>;

// ============================================================================
// CObject fields AnimDB::Update reads. All three used to be read out of named
// padding through reinterpret_cast helpers here; they are now real, named
// members of CObject (common_cobject.h), re-verified against the listing:
//   +0x50 PhysicsSystemId    -- written next to BodyPtr from the PhySys system
//                               record's +0x14 at module storage / 0628a681,
//                               cleared with BodyPtr at module storage, and
//                               consumed here as the `unsigned` argument of
//                               ?GetCurrentTime@PhySys@@YA?AVIVP_Time@@I@Z.
//   +0x60 GolemController    -- operator_new(0xd0) + vftable 0x0639c1c8 at
// , null-tested at
// , ECX receiver of the routine /
// at module storage / 0627e2d9.
//   +0x74 PhysicsControlMode -- mirrored from PhySys::CreateParms+0x18 at
// / 06288edd, dispatched here at
// (DEC EAX; JZ; DEC EAX; JNZ).
// See common_cobject.h for the full proof chain on each.
// ============================================================================

static_assert(offsetof(CObject, PhysicsSystemId) == 0x50, "CObject+0x50 PhySys system id");
static_assert(offsetof(CObject, GolemController) == 0x60, "CObject+0x60 golem force controller");
static_assert(offsetof(CObject, PhysicsControlMode) == 0x74, "CObject+0x74 physics control mode");
static_assert(sizeof(CObject) == 0x9c, "CObject sizeof 0x9c -- every offset above is inside it");

static int GetTransformMode(CObject const* Obj)
{
    return Obj->PhysicsControlMode;
}

static IVP_Controller* GetGolemController(CObject const* Obj)
{
    return Obj->GolemController;
}

// ============================================================================
// Global state — mirrors baseline statics
// ============================================================================

static FAnimEntry* G_VChannelSentinel = nullptr;
static int G_VChannelCount = 0;

static PlayMap G_PlayMap;                          // (std::_Tree)

static FAnimEntry* G_FreeList = nullptr;
static int G_FreeCount = 0;

// ============================================================================
// Internal helpers
// ============================================================================

static FAnimEntry* AllocEntry()
{
    // baseline: the routine pops the field (free list) or operator_new(0x1c)s a
    // fresh node, then the routine copy-constructs the payload (Channel/Root plus
    // a freshly-empty embedded std::list<CObject*>) into it. Since we keep
    // recycled FAnimEntry objects alive as real C++ objects (never delete them
    // until Shutdown), a freshly-popped free-list entry's sub-list is already
    // guaranteed empty by FreeEntry()'s contract below; the clear() here is a
    // cheap defensive no-op that also covers first-construction.
    FAnimEntry* Entry;
    if (G_FreeList != nullptr)
    {
        Entry = G_FreeList;
        G_FreeList = G_FreeList->Next;
        G_FreeCount--;
    }
    else
    {
        Entry = new FAnimEntry();
    }
    Entry->Next = nullptr;
    Entry->Prev = nullptr;
    Entry->Channel = nullptr;
    Entry->Sub.Root = nullptr;
    Entry->Sub.Members.clear();
    return Entry;
}

static void FreeEntry(FAnimEntry* Entry)
{
    // No null test: every baseline recycle site (Rem 0627d6af..0627d6e2, the list dtor
    // .., the routine) hands it a live node. baseline: AnimDB::Rem(VChannel*) calls the routine (skipped, id 617) to walk
    // and free every node of the embedded sub-list *before* the FAnimEntry itself
    // is recycled onto the field. std::list::clear() is the exact equivalent —
    // it destroys/frees every sub-list node so a reused entry never carries stale
    // sub-list content forward.
    Entry->Sub.Members.clear();
    Entry->Channel = nullptr;
    Entry->Sub.Root = nullptr;
    if (G_FreeCount < 0x7FFFFFFF)
    {
        Entry->Next = G_FreeList;
        G_FreeList = Entry;
        G_FreeCount++;
    }
    else
    {
        delete Entry;
    }
}

// prev->next = next, next->prev = prev; no tests.
static void UnlinkVChannel(FAnimEntry* Entry)
{
    Entry->Prev->Next = Entry->Next;
    Entry->Next->Prev = Entry->Prev;
}

// Frees every node parked on the recycled-node pool (the field) and zeroes the
// pool head and depth (the field). baseline inlines this loop three times:
// AnimDB::Shutdown 0627d470..0627d49e, the list ctor 0627d41e..0627d43e and the
// list dtor 0627e755..0627e777.
static void FlushFreeList()
{
    FAnimEntry* Cur = G_FreeList;
    while (Cur != nullptr)
    {
        FAnimEntry* Next = Cur->Next;
        delete Cur;   // (operator delete)
        Cur = Next;
    }
    G_FreeList = nullptr;
    G_FreeCount = 0;
}

// how many live lists share the the field node pool. baseline touches it
// only in the VChannel list's ctor (0627d3b0), its dtor (0627e690) and the ctor's
// unwind funclet (0627e790), so the one instance is the global list at the field:
// the count is 1 from DLL load until the atexit dtor drops it to 0 and drains the pool.
static int G_PoolListCount = 0;

// The VChannel list object the field (+0 allocator, +4 head = the field,
// +8 size = the field). baseline constructs it from the static-initializer entry
// (table slot 063ea04c: CALL 0627d3b0, then atexit(the branch), which
// is `MOV ECX,063fbfec; JMP 0627e690`). This static object reproduces both: its ctor
// is the routine, its dtor the routine.
struct FVChannelList
{
    // , listing 0627d3b0..0627d454.
    FVChannelList()
    {
        // two allocator copies are counted (the default-argument
        // temporary and the list's own); the temporary is released at 0627d40b.
        G_PoolListCount += 2;

        // pop the pool head, else operator_new(0x1c).
        FAnimEntry* Head;
        if (G_FreeList != nullptr)
        {
            Head = G_FreeList;
            G_FreeList = Head->Next;
            --G_FreeCount;
        }
        else
        {
            Head = new FAnimEntry();
        }

        // self-looped head, size 0.
        Head->Next = Head;
        Head->Prev = Head;
        G_VChannelSentinel = Head;
        G_VChannelCount = 0;

        // the temporary allocator dies; drain the pool at zero.
        if (--G_PoolListCount == 0)
            FlushFreeList();
    }

    // , listing 0627e690..0627e78f (ECX = the field).
    ~FVChannelList()
    {
        FAnimEntry* const Head = G_VChannelSentinel;
        FAnimEntry* Node = Head->Next;
        while (Node != Head)
        {
            FAnimEntry* const Next = Node->Next;
            Node->Prev->Next = Node->Next;
            Node->Next->Prev = Node->Prev;
            FreeEntry(Node);  // (node+8, 0) payload dtor, then pool-or-delete
            --G_VChannelCount;
            Node = Next;
        }

        // the head goes back to the pool too (or is deleted at the cap).
        FreeEntry(Head);

        // head = 0, size = 0.
        G_VChannelSentinel = nullptr;
        G_VChannelCount = 0;

        // last list on the pool -> drain it.
        if (--G_PoolListCount == 0)
            FlushFreeList();
    }
};

static FVChannelList G_VChannelList;

// ============================================================================
// Exported functions
// ============================================================================

void __cdecl Shutdown()
{
    // Listing 0x1d470: baseline ONLY frees the recycled-node free list (the field /
    // ) -- it does NOT touch the live VChannel sentinel list (the field)
    // or the live PlaybackId list (the field) at all. A previous version of this port
    // additionally deleted both live lists and their sentinels here, which is NOT what
    // baseline does. The list head itself lives from static init (FVChannelList, the routine) to the
    // atexit dtor (the routine).
    FlushFreeList();
}

void __cdecl Add(VChannel* Chan)
{
    // Listing 0x1d4a0 (module storage..0627d67c), Common. No null test on
    // Chan: 0627d4df dereferences it straight away.

    // Skip if this VChannel is already registered (0627d4d0..0627d4dd).
    FAnimEntry* Cur = G_VChannelSentinel->Next;
    while (Cur != G_VChannelSentinel)
    {
        if (Cur->Channel == Chan)
            return;
        Cur = Cur->Next;
    }

    int ChanId = Chan->GetId();
    if (ChanId == -1)
        return;  // Unbound channel, skip

    // Allocate and link a fresh entry at the tail of the VChannel list.
    FAnimEntry* Entry = AllocEntry();
    Entry->Next = G_VChannelSentinel;
    Entry->Prev = G_VChannelSentinel->Prev;
    G_VChannelSentinel->Prev->Next = Entry;
    G_VChannelSentinel->Prev = Entry;
    G_VChannelCount++;

    Entry->Channel = Chan;

    // Engine+0x80 GetInstanceUserData(Chan->GetId()) (a second GetId
    // call), Root = GetRoot(result). No null test on Engine or on the result: 0627d626 reads
    // the result's +4 unconditionally.
    CObject* const InstObj = (CObject*)NeuronRuntime::Engine->GetInstanceUserData((void*)(intptr_t)Chan->GetId());
    Entry->Sub.Root = GetRoot(InstObj);

    // InstObj->EngineInstanceId compared with a third Chan->GetId().
    if (InstObj->EngineInstanceId == Chan->GetId())
    {
        // The sub-list element is the ENGINE INSTANCE, not the channel:
        // Commond661 `MOV EDX,[ESP+0x3c]` reloads the slot written at
        // from EBX == GetInstanceUserData's CObject*.
        Entry->Sub.Members.push_back(InstObj);
    }
}

void __cdecl Rem(VChannel const* Chan)
{
    // Listing 0x1d680 (module storage..0627d6f6), Common: find the first
    // entry whose Channel == Chan (pointer compare, no null test), unlink it, destroy its
    // sub-list (the routine), recycle the node (pool if count < 0x7fffffff, else delete),
    // --count.
    FAnimEntry* Cur = G_VChannelSentinel->Next;
    while (Cur != G_VChannelSentinel)
    {
        if (Cur->Channel == Chan)
        {
            UnlinkVChannel(Cur);
            FreeEntry(Cur);  // now also destroys the embedded sub-list first
            G_VChannelCount--;
            return;
        }
        Cur = Cur->Next;
    }
}

// (__cdecl, plain RET): the IAnimation2::EnumObjects callback Add(int) passes.
// takes the context (2nd argument) as the std::vector, the body is an inlined
// vector::push_back of the 1st argument (grow path 0627d717..0627d7fe, in-place path
// ..), and both exits load AL = 1 (0627d7e2 / 0627d7fa), so enumeration continues.
static unsigned char __cdecl CollectChannel(void* Channel, void* Context)
{
    ((std::vector<void*>*)Context)->push_back(Channel);
    return 1;
}

COMMON_API void __cdecl Add(int PlaybackId)
{
    // Listing module storage..0627dad5, Common. No guards on the id, on
    // NeuronRuntime::Anim / Channel / Engine or on the resolved instance -- baseline has none.

    // build {PlaybackId, empty FAnimSub} and insert it (the routine,
    // a map::insert returning pair<iterator,bool>); an id already present is a no-op. The
    // scratch-list pool bookkeeping around it (the field/424/42c) is VC6 allocator
    // internals with no counterpart in std::list.
    std::pair<PlayMap::iterator, bool> const Ins = G_PlayMap.try_emplace(PlaybackId);
    PlayMap::iterator const It = Ins.first;
    if (!Ins.second)
        return;
    FAnimSub& Entry = It->second;

    // an empty local vector (frame -0x40) is filled by IAnimation2 slot 35
    // (+0x8c) EnumObjects(the branch, PlaybackId, &vector); the callback push_backs every
    // typed channel and answers 1. The slot's own result is ignored.
    std::vector<void*> Channels;
    NeuronRuntime::Anim->EnumObjects((void*)&CollectChannel, (unsigned long)PlaybackId, &Channels);

    // . LastInstance (frame -0x64) starts at 0 (0627d9ba) and is written
    // with every GetInstanceUserData result at 0627d9ff -- BEFORE the EngineInstanceId
    // compare, so a non-matching instance still counts as the last one resolved.
    CObject* LastInstance = nullptr;
    for (std::vector<void*>::const_iterator ChanIt = Channels.begin(); ChanIt != Channels.end(); ++ChanIt)
    {
        // IChannel slot 16 (+0x40) GetHeader(&Header, channel).
        unsigned long Header[4];
        NeuronRuntime::Channel->GetHeader(Header, (unsigned long)*ChanIt);
        // Header[0] != 0 -> next channel.
        if (Header[0] != 0)
            continue;
        // Engine+0x80 GetInstanceUserData(Header[3]).
        CObject* const Inst = (CObject*)NeuronRuntime::Engine->GetInstanceUserData((void*)Header[3]);
        LastInstance = Inst;
        // [Inst+4] == Header[3], no null test.
        if (Inst->EngineInstanceId == (int)Header[3])
        {
            // the routine + the routine push the CObject* onto the
            // entry's member list, size++.
            Entry.Members.push_back(Inst);
        }
    }

    if (LastInstance != nullptr)
    {
        // baseline: pCVar6 = GetRoot(pCStack_64); node->Root(+0x10) = pCVar6.
        Entry.Root = GetRoot(LastInstance);
    }
    else
    {
        // baseline (Common): when no channel resolved an owning
        // engine instance for this PlaybackId, it re-derives [lower_bound,
        // upper_bound) for PlaybackId (the routine/the routine — the same
        // lower/upper-bound shape as Rem(int)'s the routine/the routine),
        // measures the range (the routine — a plain iterator-distance walk using
        // the _Tree::_Inc idiom, incrementing a counter), then erases that whole
        // range (the routine — structurally identical to Rem(int)'s erase: a
        // fast whole-tree clear, or a walk-and-erase-each fallback otherwise).
        // Net effect: the node just inserted by this call is removed again since
        // no channel could actually be resolved for it. We already hold its
        // iterator (It), so erase it directly rather than re-deriving the range.
        G_PlayMap.erase(It);
    }
    // the vector's buffer is freed (Channels' destructor).
}

COMMON_API void __cdecl Rem(int PlaybackId)
{
    // Listing 0x1dc80, Common: remove the play entry matching
    // PlaybackId (0 or 1 element on a unique-key map). baseline walks
    // [lower_bound, upper_bound) via the classic std::_Tree::_Inc idiom and
    // special-cases erasing the *entire* tree when that range covers it (a pure
    // perf optimization over its hand-rolled tree). std::map::erase(first, last)
    // covers both cases uniformly.
    std::pair<PlayMap::iterator, PlayMap::iterator> const Range = G_PlayMap.equal_range(PlaybackId);
    G_PlayMap.erase(Range.first, Range.second);
}

// ============================================================================
// Update() helpers
// ============================================================================

// (Common) is a literal linear
// std::find over [v._First, v._Last) that returns _Last on a miss -- NOT a
// sorted lower_bound. the routine (59623-59674) wraps it as "find; if absent
// insert at the end; return pair<iterator,bool>", and Update() open-codes the
// same pattern three more times. So both scratch collections below are
// insertion-ordered vectors used as sets; keeping them vectors rather than
// std::set is what preserves baseline's phase-C visiting order.
// , listing 0627e7d0..0627e84b (__thiscall, ECX = vector, RET 8: hidden
// pair return buffer + &Value):
// inline find over [_First, _Last); a miss yields _Last.
// /0627e7f3   hit -> {it, false} (0627e83b..0627e849).
// miss -> vector::insert(_Last, 1, &Value).
// re-find after the insert (the buffer may have moved) and
//                       return {it, true}; the re-find always hits the new element.
template <typename T>
static std::pair<typename std::vector<T>::iterator, bool> InsertUnique(std::vector<T>& Vec, T Value)
{
    typename std::vector<T>::iterator It = std::find(Vec.begin(), Vec.end(), Value);
    if (It != Vec.end())
        return std::pair<typename std::vector<T>::iterator, bool>(It, false);
    Vec.insert(Vec.end(), 1, Value);
    It = std::find(Vec.begin(), Vec.end(), Value);
    return std::pair<typename std::vector<T>::iterator, bool>(It, true);
}

// (Common), a recursive scene-graph
// walk. For every engine child of InstanceId: resolve its user-data CObject;
// if that is a real object distinct from Obj, add it to Objs (set semantics)
// and recurse into it as the new "current object"; otherwise recurse with Obj
// unchanged. Either way the recursion descends on the CHILD's instance id, so
// subtrees hanging off instances that carry no CObject of their own are still
// visited. baseline's null/self test is `if (childObj != 0 && childObj != obj)`
// -- both failures take the same branch (59198-59201 pushes EBP == obj).
// Listing 0627e3b0..0627e475 (__cdecl, 3 args): first child = Engine+0xd8
// GetRelatedInstance(InstanceId, 1, -1) (0627e3c5); loop while != -1 (0627e3cd /
// ); user data = Engine+0x80 (0627e3ec); insert via the inline find + vector
// insert (0627e3fe..0627e43f, result unused); recursion at 0627e449; next sibling =
// GetRelatedInstance(InstanceId, 1, Child) (0627e460). No null checks.
static void CollectInstanceChildren(CObject* Obj, long InstanceId, std::vector<CObject*>& Objs)
{
    // IEngine carries the instance id in a void* (engine/sceneruntime.h).
    long Child = (long)(intptr_t)NeuronRuntime::Engine->GetRelatedInstance((void*)(intptr_t)InstanceId, 1,
                                                                   G_InvalidEngineHandle);
    while (Child != -1)
    {
        CObject* const ChildObj = (CObject*)NeuronRuntime::Engine->GetInstanceUserData((void*)(intptr_t)Child);
        if (ChildObj != nullptr && ChildObj != Obj)
        {
            InsertUnique(Objs, ChildObj);
            CollectInstanceChildren(ChildObj, Child, Objs);
        }
        else
        {
            CollectInstanceChildren(Obj, Child, Objs);
        }
        Child = (long)(intptr_t)NeuronRuntime::Engine->GetRelatedInstance((void*)(intptr_t)InstanceId, 1,
                                                                  (void*)(intptr_t)Child);
    }
}

void __cdecl Update(float Dt)
{
    // Listing 0x1dda0 == Common (pseudo-analysis
    // Common). Retraced instruction by instruction 2026-09-17;
    // the previous port of this function was wrong in structure, not merely
    // incomplete, so the whole body was rewritten. What baseline actually does,
    // in order, with three function-local scratch containers:

    //   V1 (frame B+0x54) vector<int>       -- engine instance ids, set semantics
    //   V2 (frame B+0x44) vector<FAnimSub*> -- sub-objects whose Root is physical
    //   V3 (frame B+0x34) vector<CObject*>  -- objects whose transform to push

    //   A. Walk the PlaybackId map. For each entry take Root->EngineInstanceId;
    //      if it was not already in V1, call Anim->MixerWalk(id, Dt). Then, if
    //      Root->BodyPtr != 0, record &entry in V2.
    //   B. Walk the VChannel list. Call Channel->vtable[0](Dt) on every entry,
    //      then do the same V1 insert (result discarded -- NO MixerWalk here)
    //      and the same BodyPtr-gated V2 insert.
    //   C. For every id in V1, Engine->UpdateInstance(id, 0, 0).
    //   D. For every FAnimSub in V2, for every CObject* in its member list: add
    //      it to V3 and recursively collect its whole engine child subtree
    //      (the routine, open-coded one level deep at the call site).
    //   E. For every CObject* in V3, push its live transform into physics --
    //      see the per-branch comments below, the only part still deferred.
    //   F. Clear the VChannel list outright (the routine ==
    //      list::erase(begin(), end()), Common) -- so
    //      registration via Add(VChannel*) is per-frame, not persistent.
    //   G. Walk the PlaybackId map and ERASE every entry for which
    //      Anim->FilterObjects(key) returns NON-ZERO.

    // Corrections against the previous port, each re-derived from the listing:
    //   * MixerWalk was driven off the VChannel list and off Channel->GetId();
    //     baseline drives it off the PlaybackId map and off Root->EngineInstanceId,
    //     gated on the dedup insert (Common).
    //   * Channel->vtable[0](Dt) (58744-58747) was not called at all.
    //   * IAnimation2::EvalStored (slot 12, +0x30) was called over the whole map;
    //     baseline's Update never references slot 12. The +0x6c and +0x44 slots are
    //     the only two Anim slots in this function.
    //   * The Engine->UpdateInstance sweep (58804-58817) was missing entirely.
    //   * Phase D walked the VChannel list directly instead of the collected V2,
    //     and never recursed past the first child level.
    //   * Phase F (clearing the list) was missing entirely.
    //   * Phase G's condition was INVERTED: the port erased on
    //     FilterObjects()==0, baseline erases on !=0 (59082-59083 `TEST EAX,EAX;
    //     JZ <advance>`, i.e. the erase helper the routine is on the non-zero
    //     fall-through, confirmed again in Common).

    // baseline has no early-out and no null checks on Root, the member objects, NeuronRuntime::Anim,
    // NeuronRuntime::Engine or the math singleton; neither does this port.

    std::vector<int> SeenIds;     // baseline V1
    std::vector<FAnimSub*> Subs;  // baseline V2

    // ---- Phase A: the PlaybackId map (Common) ----------
    for (PlayMap::iterator It = G_PlayMap.begin(); It != G_PlayMap.end(); ++It)
    {
        FAnimSub& Sub = It->second;
        // [Root+4] read with no test.
        int const InstanceId = Sub.Root->EngineInstanceId;
        if (InsertUnique(SeenIds, InstanceId).second)
        {
            // Anim vtable +0x6c == IAnimation2 slot 27, MixerWalk. sceneruntime.h
            // spells the first parameter "PlaybackId", but baseline loads it from
            // `(*(Root)) + 4` (58710-58712), i.e. the engine instance id of the
            // entry's root object -- not the map key.
            NeuronRuntime::Anim->MixerWalk((unsigned long)InstanceId, Dt);
        }
        if (Sub.Root->BodyPtr != nullptr)
            Subs.push_back(&Sub);
    }

    // ---- Phase B: the VChannel list (Common) -----------
    // The head always exists (static FVChannelList, the routine).
    for (FAnimEntry* Cur = G_VChannelSentinel->Next; Cur != G_VChannelSentinel; Cur = Cur->Next)
    {
        // Channel->Tick(Dt), then [Root+4] at 0627def8..0627defb; no tests.
        Cur->Channel->Tick(Dt);
        // baseline performs the identical V1 insert here (58755-58784) and
        // then throws the pair<iterator,bool> away: no MixerWalk in this
        // loop. The insert still matters -- it is what stops phase C from
        // updating the same instance twice.
        InsertUnique(SeenIds, Cur->Sub.Root->EngineInstanceId);
        if (Cur->Sub.Root->BodyPtr != nullptr)
            Subs.push_back(&Cur->Sub);
    }

    // ---- Phase C: Common ------------------------------
    // Engine vtable +0x6c == IEngine slot 27, UpdateInstance(Instance, 0, 0).
    for (std::vector<int>::const_iterator It = SeenIds.begin(); It != SeenIds.end(); ++It)
        NeuronRuntime::Engine->UpdateInstance((void*)(intptr_t)*It, 0, nullptr);

    // ---- Phase D: Common ------------------------------
    std::vector<CObject*> Objs;  // baseline V3
    for (std::vector<FAnimSub*>::const_iterator SubIt = Subs.begin(); SubIt != Subs.end(); ++SubIt)
    {
        FAnimSub const& Sub = **SubIt;
        for (std::list<CObject*>::const_iterator MemberIt = Sub.Members.begin();
             MemberIt != Sub.Members.end(); ++MemberIt)
        {
            // the element is inserted, then its +4 read with no test.
            CObject* const Member = *MemberIt;
            InsertUnique(Objs, Member);
            CollectInstanceChildren(Member, Member->EngineInstanceId, Objs);
        }
    }

    // ---- Phase E: Common ------------------------------
    for (std::vector<CObject*>::const_iterator It = Objs.begin(); It != Objs.end(); ++It)
    {
        CObject* const Obj = *It;
        int const Mode = GetTransformMode(Obj);  // CObject+0x74
        if (Mode == 1)
        {
            // baseline (58998-59053): if the object has a PhySys "golem" force
            // controller at +0x60, refresh its target with the object's own live
            // WorldPosition, its orientation as an IVP-ordered quaternion, and
            // PhySys::GetCurrentTime(Obj+0x50) -- two __thiscalls on the
            // controller, the routine(&pos, &zeroVector, &time) and
            // (&quat, &time, 0, 0) (tracker items 2741/2743). baseline
            // does NOT call BeamR or otherwise move the object here; the actual
            // blend happens in the controller's own tick.

            // This retraces the receiver those two items list as "an unidentified
            // struct built by an untraced caller": the caller IS this line, and
            // the receiver is CObject+0x60, allocated at Common
            // (`MOV [ESI+0x60],EDI`) in the PhySys attach path.

            // LIVE as of 2026-09-17 (lane w18-golem): the controller is a real
            // class now -- RTTI-confirmed FLControllerGolem, ported in
            // src/Common/common_controllergolem.cpp together with its constructor and
            // both setters. PhySys::AttachGolemController allocates it.
            FLControllerGolem* const Golem =
                (FLControllerGolem*)GetGolemController(Obj);  // CObject+0x60
            if (Golem != nullptr)
            {
                // baseline 59025-59036: PhySys::GetCurrentTime(CObject+0x50) via a
                // hidden return buffer, then the same IVP_Time feeds BOTH calls.
                IVP_Time const Now = PhySys::GetCurrentTime(Obj->PhysicsSystemId);

                // lazily cached singleton (the field), no null test.
                I3DMathEngine* const GolemMath = Acquire3DMathEngine();

                // Singleton vtable +0x50, fed Obj+0x08 (59014-59019), then the
                // same one-slot rotation as the mode-2 branch below:
                // dst[0..3] = src[1], src[2], src[3], src[0] (59020-59028).
                // FQuaternion is (W,X,Y,Z) so this hands over (X,Y,Z,W).
                FQuaternion GolemQuat = {0.0f, 0.0f, 0.0f, 0.0f};
                GolemMath->MatrixToQuaternion(GolemQuat, (FMatrix const&)Obj->Orientation);
                float const GolemIvpQuat[4] = {GolemQuat.X, GolemQuat.Y, GolemQuat.Z, GolemQuat.W};

                // 59033-59035 zeroes a stack triple and passes it as the target
                // speed -- the golem is told "be here, at rest", and its own
                // per-tick integrator does the actual blending.
                Vector const GolemZeroSpeed = {0.0f, 0.0f, 0.0f};

                // (item 2741) then 0627e2d9 (item 2743), in that order,
                // both __thiscall with the controller in ECX.
                PhySys::GolemSetTargetPosition(Golem, Obj->WorldPosition, GolemZeroSpeed, Now);
                PhySys::GolemSetTargetQuaternion(Golem, GolemIvpQuat, Now, nullptr, 0.0f);
            }
            continue;
        }
        if (Mode != 2)
            continue;

        // baseline (58952-58997): unconditional for mode 2.
        Vector const Pos = Obj->WorldPosition;  // +0x2c..+0x34

        // lazily cached singleton (the field), no null test.
        I3DMathEngine* const Math = Acquire3DMathEngine();

        // Singleton vtable +0x50 == I3DMathEngine::MatrixToQuaternion, fed
        // Obj->Orientation (Obj+8).
        FQuaternion MathQuat = {0.0f, 0.0f, 0.0f, 0.0f};
        Math->MatrixToQuaternion(MathQuat, (FMatrix const&)Obj->Orientation);

        // baseline then rotates the four dwords by one slot before handing them on
        // (58971-58990: dst[0]=src[1], dst[1]=src[2], dst[2]=src[3], dst[3]=src[0]).
        // FQuaternion is (W,X,Y,Z), so the shuffled buffer is (X,Y,Z,W) -- IVP's
        // own quaternion order. This is a convention conversion, not a copy.
        float const IvpQuat[4] = {MathQuat.X, MathQuat.Y, MathQuat.Z, MathQuat.W};

        // baseline's final call (58982-58996, module storage) is
        // (0, Obj->BodyPtr, IvpQuat, &Pos, 0)
        // -- __cdecl, five pushes, ADD ESP,0x14. Tracker item 2746, ported
        // 2026-09-17 (lane claude:w24-blocked) as PhySys::BeamBodyToWorldPose in
        // src/Common/common_controllergolem.cpp; see that definition for the full
        // instruction-level derivation. It re-seats the body inside its own
        // IVP_Core so the body lands on this absolute world pose.

        // The fifth argument is read by nothing in the callee's body (verified
        // against the whole frame, not just its prologue), so the port's
        // signature omits it. The first is the literal 0 here and at the DLL's
        // only other call site.
        PhySys::BeamBodyToWorldPose(nullptr, Obj->BodyPtr, IvpQuat, Pos);
    }

    // ---- Phase F: Common ------------------------------
    // with ECX = the list object at 0x063fbfec and (begin, end):
    // std::list::erase over the whole range. Each node is unlinked, its embedded
    // sub-list destroyed (the routine), and the node itself recycled onto the
    // same free list AnimDB::Rem(VChannel*) uses (the field/the field);
    // the list's own size word (the field) is decremented per node. So the
    // VChannel registration is rebuilt from scratch every frame by whoever calls
    // Add(VChannel*) -- it is not a persistent registry.
    {
        FAnimEntry* Cur = G_VChannelSentinel->Next;
        while (Cur != G_VChannelSentinel)
        {
            FAnimEntry* const Next = Cur->Next;  // FreeEntry overwrites Cur->Next
            FreeEntry(Cur);
            Cur = Next;
        }
        G_VChannelSentinel->Next = G_VChannelSentinel;
        G_VChannelSentinel->Prev = G_VChannelSentinel;
        G_VChannelCount = 0;
    }

    // ---- Phase G: Common ------------------------------
    // Anim vtable +0x44 == IAnimation2 slot 17, FilterObjects, called with the
    // map KEY (node+0xc, 59076). Non-zero => erase (the routine returns the
    // next iterator); zero => just advance (the routine == iterator::operator++).
    PlayMap::iterator PlayIt = G_PlayMap.begin();
    while (PlayIt != G_PlayMap.end())
    {
        if (NeuronRuntime::Anim->FilterObjects((void*)(intptr_t)PlayIt->first) != 0)
            PlayIt = G_PlayMap.erase(PlayIt);
        else
            ++PlayIt;
    }
}

unsigned int __cdecl Count()
{
    // Listing 0x1e480: return total count of both collections
    return (unsigned int)G_PlayMap.size() + (unsigned int)G_VChannelCount;
}

} // namespace AnimDB
