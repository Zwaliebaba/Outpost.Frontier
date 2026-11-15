// Author: Giperion.

#include "common_reputation.h"
#include "common_reputation_internal.h"

#include "common_ini.h"
#include "common_misc.h"
#include "common_net.h"
#include "neuroncomponents.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

using CmnReputation::EnsureStoryFactionIds;
using CmnReputation::FGroup;
using CmnReputation::FindGroupConst;
using CmnReputation::GStoryFactionIds;

namespace
{
    // Reputation::Vibe keeps its records in two stores (see the note on Reputation::Vibe in
    // common_reputation.h).

    // The static store: baseline's std::vector at the field (begin/end/capacity
    // /bc/c0; constructed by the branch, destroyed through atexit by
    // ), 0x90-byte elements {int key; VibeValue value}. Filled while the universe
    // loads (BeginStaticLoad .. EndStaticLoad), survives FreeDynamic, emptied by FreeStatic.
    struct FVibeSlot
    {
        int Key;
        Reputation::Vibe::VibeValue Value;
    };
    std::vector<FVibeSlot> GStaticVibes;
    // set by BeginStaticLoad, cleared by EndStaticLoad. While set, Alloc appends
    // to the static store and FindStaticVibe scans it linearly; afterwards it is sorted and
    // binary-searched.
    bool GbStaticVibeLoading = false;
    // FindStaticVibe's one-entry cache (the last slot it returned, or null).
    FVibeSlot* GStaticVibeHit = nullptr;

    // The dynamic store: baseline's std::map<int, VibeValue> the field (signed key compare,
    // JGE/JL/SETL in the routine / the routine; key node+0xc, value node+0x10), built by
    // / the routine and destroyed through atexit by the branch /
    // . Alloc inserts here once the universe has loaded; FreeDynamic empties it.
    typedef std::map<int, Reputation::Vibe::VibeValue> FDynamicVibeMap;
    FDynamicVibeMap GDynamicVibes;
    // (the word after the map object): the last node a lookup landed on, end()
    // when it missed. the routine initialises it to the head (end()).
    FDynamicVibeMap::iterator GDynamicVibeHit = GDynamicVibes.end();

    // (a static std::map<unsigned,unsigned>, constructed at
    // , destroyed through atexit at module storage): client id -> previous
    // affiliation. Written by SetAffiliation (module storage) and by the auto-affiliation pass
    // (module storage); handed out by GetChangedAffiliationClientMap; cleared by its consumers.
    std::map<unsigned int, unsigned int> GChangedAffiliationClientMap;

    // / the field (.data, no writers anywhere in the listing): 0x3f266666 and
    // .
    float const kAffiliationKeepFeeling = 0.65f;
    float const kAffiliationJoinFeeling = 0.9f;

    // baseline compares static-store keys by subtraction: the slot operator< of the sort
    // is `SUB; JS` and the bsearch comparator the routine is `MOV EAX,[a];
    // SUB EAX,[b]`. The 32-bit difference wraps for keys more than 2^31 apart (the keys are
    // nickname hashes, so such pairs exist), which makes the order non-transitive: the result
    // of the sort, and which keys bsearch then finds, depend on the exact algorithms. Both are
    // reproduced below with this comparator.
    int StaticVibeKeyDiff(int A, int B)
    {
        return (int)((unsigned)A - (unsigned)B);
    }

    bool StaticVibeSlotBefore(FVibeSlot const& A, FVibeSlot const& B)
    {
        return StaticVibeKeyDiff(A.Key, B.Key) < 0;
    }

    // , handed to MSVCRT bsearch: the search key against a slot's key.
    int __cdecl StaticVibeKeyCompare(void const* KeyPtr, void const* SlotPtr)
    {
        return StaticVibeKeyDiff(*(int const*)KeyPtr, ((FVibeSlot const*)SlotPtr)->Key);
    }

    // VC6 <algorithm> std::sort as the routine instantiates it for the static store, step
    // for step: _Sort_0 (the routine itself, with the n <= 16 insertion sort inlined),
    // _Sort (the routine), _Unguarded_partition (inlined), _Median (the routine),
    // _Insertion_sort (the routine), _Unguarded_insert (the routine, also inlined in the
    // tail loop), iter_swap (the routine), copy_backward (the routine). Slots are copied
    // with the slot copy constructor (the routine: the key, a default record and the copy
    // tail the routine -- FVibeSlot's implicit copy) and assigned with Vc6AssignSlot.
    int const kVc6SortMax = 16;

    // (thiscall, RET 4): the slot assignment every step of the sort uses -- the
    // key, then the copy tail the routine on the record, which prepends the source's deltas
    // to the destination's list without freeing it (VibeValue::CopyFieldsFrom). It is not
    // VibeValue::operator=, which frees the destination's list first.
    void Vc6AssignSlot(FVibeSlot& Dst, FVibeSlot const& Src)
    {
        Dst.Key = Src.Key;
        Dst.Value.CopyFieldsFrom(Src.Value);
    }

    // (cdecl; the third argument is VC6's unused value-type tag): tmp = *X,
    // *X = *Y, *Y = tmp, tmp destroyed.
    void Vc6IterSwap(FVibeSlot* X, FVibeSlot* Y)
    {
        FVibeSlot const Tmp = *X;
        Vc6AssignSlot(*X, *Y);
        Vc6AssignSlot(*Y, Tmp);
    }

    // (cdecl): copy_backward over slots with the slot assignment; returns the
    // new destination start.
    FVibeSlot* Vc6CopyBackward(FVibeSlot* First, FVibeSlot* Last, FVibeSlot* DestLast)
    {
        while (First != Last)
            Vc6AssignSlot(*--DestLast, *--Last);
        return DestLast;
    }

    void Vc6UnguardedInsert(FVibeSlot* Last, FVibeSlot Value)
    {
        FVibeSlot* Mid = Last;
        while (StaticVibeSlotBefore(Value, *--Mid))
        {
            Vc6AssignSlot(*Last, *Mid);
            Last = Mid;
        }
        Vc6AssignSlot(*Last, Value);
    }

    void Vc6InsertionSort(FVibeSlot* First, FVibeSlot* Last)
    {
        if (First == Last)
            return;
        for (FVibeSlot* Mid = First; ++Mid != Last;)
        {
            FVibeSlot Value = *Mid;
            if (!StaticVibeSlotBefore(Value, *First))
            {
                Vc6UnguardedInsert(Mid, Value);
            }
            else
            {
                Vc6CopyBackward(First, Mid, Mid + 1);
                Vc6AssignSlot(*First, Value);
            }
        }
    }

    FVibeSlot Vc6Median(FVibeSlot const& X, FVibeSlot const& Y, FVibeSlot const& Z)
    {
        if (StaticVibeSlotBefore(X, Y))
            return StaticVibeSlotBefore(Y, Z) ? Y : (StaticVibeSlotBefore(X, Z) ? Z : X);
        return StaticVibeSlotBefore(X, Z) ? X : (StaticVibeSlotBefore(Y, Z) ? Z : Y);
    }

    FVibeSlot* Vc6UnguardedPartition(FVibeSlot* First, FVibeSlot* Last, FVibeSlot const& Pivot)
    {
        for (;; ++First)
        {
            while (StaticVibeSlotBefore(*First, Pivot))
                ++First;
            while (StaticVibeSlotBefore(Pivot, *--Last))
            {
            }
            if (Last <= First)
                return First;
            Vc6IterSwap(First, Last);
        }
    }

    void Vc6Sort(FVibeSlot* First, FVibeSlot* Last)
    {
        while (kVc6SortMax < Last - First)
        {
            FVibeSlot* Mid = nullptr;
            {
                // The pivot temporary dies before the recursion (module storage).
                FVibeSlot const Pivot =
                    Vc6Median(*First, *(First + (Last - First) / 2), *(Last - 1));
                Mid = Vc6UnguardedPartition(First, Last, Pivot);
            }
            if (Last - Mid <= Mid - First)
            {
                Vc6Sort(Mid, Last);
                Last = Mid;
            }
            else
            {
                Vc6Sort(First, Mid);
                First = Mid;
            }
        }
    }

    void Vc6SortStaticVibes(FVibeSlot* First, FVibeSlot* Last)
    {
        if (Last - First <= kVc6SortMax)
        {
            Vc6InsertionSort(First, Last);
            return;
        }
        Vc6Sort(First, Last);
        Vc6InsertionSort(First, First + kVc6SortMax);
        for (First += kVc6SortMax; First != Last; ++First)
            Vc6UnguardedInsert(First, *First);
    }

    // (module storage-06320365): the static-store lookup.
    //   cache the field non-null and its key == Key -> that slot (06320295-063202a0);
    //   loading mode: linear scan begin..end for the first slot with that key (063202b2-063202d4);
    //   otherwise MSVCRT bsearch(&record, begin, count, 0x90, the routine) over the sorted
    //   store (06320305-0632034e; the key record the field is a lazily constructed static
    //   slot, the routine + atexit the branch, of which only the key -- overwritten each
    //   call -- is ever read; the port passes the key alone);
    //   the result (null on a miss) becomes the new cache (06320354); returns &slot.Value.
    Reputation::Vibe::VibeValue* FindStaticVibe(int Key)
    {
        if (GStaticVibeHit != nullptr && GStaticVibeHit->Key == Key)
            return &GStaticVibeHit->Value;

        FVibeSlot* Found = nullptr;
        if (GbStaticVibeLoading)
        {
            std::size_t Index = 0;
            for (; Index < GStaticVibes.size(); ++Index)
            {
                if (GStaticVibes[Index].Key == Key)
                {
                    Found = &GStaticVibes[Index];
                    break;
                }
            }
        }
        else
        {
            int SearchKey = Key;
            Found = (FVibeSlot*)bsearch(&SearchKey, GStaticVibes.data(), GStaticVibes.size(),
                                        sizeof(FVibeSlot), StaticVibeKeyCompare);
        }

        GStaticVibeHit = Found;
        if (Found == nullptr)
            return nullptr;
        return &Found->Value;
    }

    // The dynamic-store lookup every accessor inlines (e.g. Verify module storage-063206f3,
    // Get 06320bc9-06320c04 through the routine + the routine, Set through the routine,
    // SetClientID through the routine): reuse the cached node when it is not end() and its
    // key matches, otherwise find() and cache the result, end() included.
    Reputation::Vibe::VibeValue* FindDynamicVibe(int Key)
    {
        if (GDynamicVibeHit == GDynamicVibes.end() || GDynamicVibeHit->first != Key)
            GDynamicVibeHit = GDynamicVibes.find(Key);
        if (GDynamicVibeHit == GDynamicVibes.end())
            return nullptr;
        return &GDynamicVibeHit->second;
    }

    // Every accessor except SetClientID / GetClientID: the dynamic store first, then the
    // static one (the `CMP node,head; JZ` / `ADD 0x10; TEST; JNZ` then `CALL the routine`
    // sequence, e.g. Get module storage-06320c29).
    Reputation::Vibe::VibeValue* FindVibe(int Key)
    {
        Reputation::Vibe::VibeValue* Value = FindDynamicVibe(Key);
        if (Value != nullptr)
            return Value;
        return FindStaticVibe(Key);
    }

    // The delta-list purge FreeDynamic (module storage-063205df) and Free (module storage-
    // , 06320b70-06320b94) run on a record: every AttitudeDelta whose GroupId equals
    // Key is unlinked and freed, in list order.
    void PurgeVibeDeltas(Reputation::Vibe::VibeValue& Value, int Key)
    {
        Reputation::Vibe::AttitudeDelta** Link = &Value.Deltas;
        while (*Link != nullptr)
        {
            Reputation::Vibe::AttitudeDelta* Node = *Link;
            if (Node->GroupId == (unsigned)Key)
            {
                *Link = Node->Next;
                delete Node;
            }
            else
            {
                Link = &Node->Next;
            }
        }
    }

    // (module storage-0631ff2e, this = the entry). The player-only
    // auto-affiliation pass that ends Vibe::Set and Vibe::SetGroupFeelingsTowards:
    //   - NPC entries (ClientID == 0) are left alone (module storage);
    //   - if the entry still likes its current affiliation more than 0.65, nothing changes
    //     (module storage-0631fe2a: FCOMP + TEST AH,0x41 + JZ, i.e. Feeling > 0.65);
    //   - otherwise the new affiliation is the group with the highest feeling strictly above
    //     0.9 that is not a story faction (the fc_*_grp id list IsStoryFaction uses, built
    //     lazily at the first candidate, module storage-0631fea5), or none (0xffffffff) --
    //     an empty feelings vector goes straight to "none" (module storage);
    //   - on a real change: Affiliation is replaced; in SinglePlayer the cached group record
    //     is just cleared (module storage); otherwise the client's previous affiliation is
    //     recorded in the changed-affiliation map before clearing it (module storage-0631ff24).
    void UpdatePlayerAffiliation(Reputation::Vibe::VibeValue& V)
    {
        if (V.ClientID == 0)
            return;

        float BestFeeling = kAffiliationJoinFeeling;
        unsigned NewAffiliation = 0xffffffffu;
        std::vector<Reputation::Vibe::GroupFeeling>::const_iterator It = V.GroupFeelings.begin();
        for (; It != V.GroupFeelings.end(); ++It)
        {
            if (It->GroupId == V.Affiliation && It->Feeling > kAffiliationKeepFeeling)
                return;
            if (!(It->Feeling > BestFeeling))
                continue;

            EnsureStoryFactionIds();
            bool bStoryFaction = false;
            std::size_t Index = 0;
            for (; Index < GStoryFactionIds.size(); ++Index)
            {
                if (GStoryFactionIds[Index] == It->GroupId)
                {
                    bStoryFaction = true;
                    break;
                }
            }
            if (bStoryFaction)
                continue;

            NewAffiliation = It->GroupId;
            BestFeeling = It->Feeling;
        }

        unsigned const OldAffiliation = V.Affiliation;
        if (NewAffiliation == OldAffiliation)
            return;
        V.Affiliation = NewAffiliation;
        if (SinglePlayer())
        {
            V.AffiliationGroup = nullptr;
            return;
        }
        GChangedAffiliationClientMap[V.ClientID] = OldAffiliation;
        V.AffiliationGroup = nullptr;
    }
}

std::map<unsigned int, unsigned int>* __cdecl Reputation::GetChangedAffiliationClientMap()
{
    // MOV EAX,[06401910] (the map's _Size) / NEG / SBB / AND EAX,06401900.
    if (GChangedAffiliationClientMap.size() != 0)
        return &GChangedAffiliationClientMap;
    return nullptr;
}

Reputation::Vibe::VibeValue::VibeValue()
    : Affiliation(0xffffffffu)
    , AttitudeTowardsPlayer(0.0f)
    , ClientID(0)
    , AffiliationGroup(nullptr)
    , Infocard(0)
    , Rank(0)
    , Reserved78(0)
    , Deltas(nullptr)
{
    // (module storage-0631fa3b), the default record Alloc builds on its stack:
    // everything zero, Affiliation 0xffffffff, both FmtStrs empty, DisplayName[0] = 0.
    DisplayName[0] = L'\0';
}

void Reputation::Vibe::VibeValue::CopyFieldsFrom(VibeValue const& Other)
{
    // (Common), term by term:
    //   +0x00 Affiliation, +0x0c AffiliationGroup, +0x08 ClientID (module storage-0631fae8);
    //   the +0x88 delta list: a fresh 0xc-byte node per source node, each PREPENDED to this
    //   entry's EXISTING list, which is never freed here (module storage-0631fb3e);
    //   +0x74 Rank; the GroupFeelings vector, skipped when both are the same vector
    //   (module storage); Name1, Name2 (FmtStr::operator=); +0x04 AttitudeTowardsPlayer;
    //   +0x40 Infocard; DisplayName via wcsncpy(..., 0x17) and a NUL at [0x17] (+0x72).
    Affiliation = Other.Affiliation;
    AffiliationGroup = Other.AffiliationGroup;
    ClientID = Other.ClientID;

    for (AttitudeDelta const* Src = Other.Deltas; Src != nullptr; Src = Src->Next)
    {
        AttitudeDelta* Node = new AttitudeDelta();
        Node->GroupId = Src->GroupId;
        Node->Delta = Src->Delta;
        Node->Next = Deltas;
        Deltas = Node;
    }

    Rank = Other.Rank;
    if (&GroupFeelings != &Other.GroupFeelings)
        GroupFeelings = Other.GroupFeelings;
    Name1 = Other.Name1;
    Name2 = Other.Name2;
    AttitudeTowardsPlayer = Other.AttitudeTowardsPlayer;
    Infocard = Other.Infocard;
    wcsncpy(DisplayName, Other.DisplayName, 0x17);
    DisplayName[0x17] = L'\0';
}

Reputation::Vibe::VibeValue::VibeValue(VibeValue const& Other)
    : Affiliation(0xffffffffu)
    , AttitudeTowardsPlayer(Other.AttitudeTowardsPlayer)
    , ClientID(0)
    , AffiliationGroup(nullptr)
    , Infocard(0)
    , Rank(0)
    , Reserved78(0)
    , Deltas(nullptr)
{
    DisplayName[0] = L'\0';
    CopyFieldsFrom(Other);
}

Reputation::Vibe::VibeValue& Reputation::Vibe::VibeValue::operator=(VibeValue const& Other)
{
    if (this == &Other)
        return *this;
    for (AttitudeDelta* Node = Deltas; Node != nullptr;)
    {
        AttitudeDelta* Next = Node->Next;
        delete Node;
        Node = Next;
    }
    Deltas = nullptr;
    CopyFieldsFrom(Other);
    return *this;
}

Reputation::Vibe::VibeValue::~VibeValue()
{
    // Mirrors the routine (Common): frees the delta list, the
    // GroupFeelings vector buffer, and both FmtStrs (the latter two handled by their own real
    // dtors here instead of by hand).
    for (AttitudeDelta* Node = Deltas; Node != nullptr;)
    {
        AttitudeDelta* Next = Node->Next;
        delete Node;
        Node = Next;
    }
}

unsigned __cdecl Reputation::Vibe::Count()
{
    // static slots ((end - begin) / 0x90, skipped while begin is
    // null) plus the dynamic map's size the field.
    return (unsigned)(GStaticVibes.size() + GDynamicVibes.size());
}

bool __cdecl Reputation::Vibe::Verify(int GroupId)
{
    // dynamic store (cache, find), then the routine; true when either
    // yields a record.
    return FindVibe(GroupId) != nullptr;
}

void __cdecl Reputation::Vibe::BeginStaticLoad()
{
    // (module storage-0632052a), called by the universe load right after its INI
    // opens (module storage). Loading mode on (06320437), then -- when the store was never
    // allocated or holds fewer than 0x1000 slots of capacity -- reallocate it to exactly
    // 0x1000 slots (operator new(0x90000), copy-construct each slot with the routine,
    // destroy the old ones with the routine, free the old block, 06320465-06320523).
    // The static cache the field is not touched: the universe load has just emptied the
    // store with FreeStatic (module storage), which cleared it.
    GbStaticVibeLoading = true;
    if (GStaticVibes.capacity() < 0x1000)
        GStaticVibes.reserve(0x1000);
}

void __cdecl Reputation::Vibe::EndStaticLoad()
{
    // (module storage-0632054e), called after the universe load's header loop
    // (module storage, not on the open-failed path): loading mode off (0632053f), then
    // (begin, end, 0) -- the VC6 std::sort of the slots with the wrapping key
    // comparator (Vc6SortStaticVibes above). The cache the field is kept; it still points
    // at a slot, and the key check in FindStaticVibe handles whatever key now sits there.
    GbStaticVibeLoading = false;
    Vc6SortStaticVibes(GStaticVibes.data(), GStaticVibes.data() + GStaticVibes.size());
}

void __cdecl Reputation::Vibe::FreeStatic()
{
    // (module storage-06320589), reached through the thunk 062b8e20 from the
    // universe object's destructor the routine (06336f3d), Universe::Startup (063370f3),
    // the universe load (063372a4) and Universe::Shutdown (0633bee4): every slot destroyed
    // (the routine), end = begin -- capacity kept -- and the cache the field cleared.
    GStaticVibes.clear();
    GStaticVibeHit = nullptr;
}

int __cdecl Reputation::Vibe::Alloc(int const& GroupId)
{
    // .
    //  - GroupId -1: return -1 (06320746-0632074d).
    //  - Loading mode (the field, 06320753-0632075d): a default record is built on the
    //    stack (06320763-063207bf: two empty FmtStrs, Affiliation -1, everything else 0) and
    //    the static store searched (the routine, 063207c6):
    //      found -> the copy tail the routine(found, default) (06320836);
    //      missed -> the slot {GroupId, default} (the routine) is appended (the routine
    //      insert at end, 06320808) and the cache the field = end - 0x90 (06320820-0632082c).
    //  - Otherwise (063208bd-063209aa): {GroupId, the routine default} (the routine) is
    //    inserted into the dynamic map (the routine, unique -- an existing record stays), then
    //    a second inline default record is applied to the node's value with the copy tail
    // (063209aa). The dynamic store is not consulted for the static one: a key
    //    that already has a static slot gets a dynamic record too, which every later lookup
    //    finds first. The insert does not touch the cache the field.
    //  The copy tail resets every field but keeps the record's own delta list (it only
    //  prepends the default's, which is empty).
    //  - Return 0 (06320a09) on every path past the -1 check.
    if (GroupId == -1)
        return -1;

    if (GbStaticVibeLoading)
    {
        VibeValue const Default;
        VibeValue* Found = FindStaticVibe(GroupId);
        if (Found != nullptr)
        {
            Found->CopyFieldsFrom(Default);
        }
        else
        {
            FVibeSlot Slot;
            Slot.Key = GroupId;
            Slot.Value.CopyFieldsFrom(Default);
            GStaticVibes.push_back(Slot);
            GStaticVibeHit = &GStaticVibes.back();
        }
        return 0;
    }

    FDynamicVibeMap::iterator It;
    {
        VibeValue const Default;
        It = GDynamicVibes.insert(std::pair<int const, VibeValue>(GroupId, Default)).first;
    }
    VibeValue const Reset;
    It->second.CopyFieldsFrom(Reset);
    return 0;
}

int __cdecl Reputation::Vibe::EnsureExists(int const& GroupId)
{
    // dynamic store (cache, find), then the routine, and only when
    // both miss, Alloc(GroupId) (06320acc); its result is ignored, EAX = 0.
    if (FindVibe(GroupId) == nullptr)
        Alloc(GroupId);
    return 0;
}

int __cdecl Reputation::Vibe::Free(int const& GroupId)
{
    // . Only the dynamic store is freed from: erase(key) (the routine,
    // which returns the erased count) and the cache the field = end() whatever the result
    // (06320af9-06320aff). Nothing erased -> -2 (06320bb9) -- a key that only has a static
    // slot is not freed. Otherwise every remaining dynamic record (06320b17-06320b54) and then
    // every static slot (06320b70-06320baa) drops its AttitudeDelta entries for that key;
    // 0 (EBP).
    std::size_t const Erased = GDynamicVibes.erase(GroupId);
    GDynamicVibeHit = GDynamicVibes.end();
    if (Erased == 0)
        return -2;

    FDynamicVibeMap::iterator It = GDynamicVibes.begin();
    for (; It != GDynamicVibes.end(); ++It)
        PurgeVibeDeltas(It->second, GroupId);

    std::size_t Index = 0;
    for (; Index < GStaticVibes.size(); ++Index)
        PurgeVibeDeltas(GStaticVibes[Index].Value, GroupId);
    return 0;
}

void __cdecl Reputation::Vibe::FreeDynamic()
{
    // . For every dynamic record, in key order (06320593-0632060f),
    // every static slot drops its AttitudeDelta entries for that record's key; then the
    // dynamic map is emptied (06320613-0632066a: the whole tree freed from the root when it
    // holds anything, else the begin..end erase loop) and the cache the field = end()
    // (06320675 / 063206b1). The static store itself is kept.
    FDynamicVibeMap::iterator It = GDynamicVibes.begin();
    for (; It != GDynamicVibes.end(); ++It)
    {
        std::size_t Index = 0;
        for (; Index < GStaticVibes.size(); ++Index)
            PurgeVibeDeltas(GStaticVibes[Index].Value, It->first);
    }
    GDynamicVibes.clear();
    GDynamicVibeHit = GDynamicVibes.end();
}

int __cdecl Reputation::Vibe::GetAffiliation(int const& GroupId, unsigned& Out, bool ExcludeIfPlayerOwned)
{
    Out = 0xffffffffu;
    VibeValue const* V = FindVibe(GroupId);
    if (V == nullptr)
        return -2;
    if (ExcludeIfPlayerOwned && V->ClientID != 0)
        Out = 0xffffffffu;
    else
        Out = V->Affiliation;
    return 0;
}

int __cdecl Reputation::Vibe::SetAffiliation(int const& GroupId, unsigned Affiliation, bool Notify)
{
    // , full control flow:
    //   entry lookup (cache -> tree -> array)            miss -> -2   (module storage)
    //   group registry the field find(Affiliation)    miss -> -3   (module storage;
    // is lower_bound, the `key < node key` test makes it a find)
    //   unchanged affiliation                                  -> 0    (module storage)
    //   Affiliation = new value                                        (module storage)
    //   SinglePlayer(), or !Notify: AffiliationGroup = &group record, 0 (module storage/0632149c)
    //   else: changed-affiliation map insert {ClientID, 0} (the routine, a unique
    //         _Tree::insert) and the found-or-new node's value = previous affiliation
    //         (module storage); AffiliationGroup = 0 (module storage); 0.
    VibeValue* V = FindVibe(GroupId);
    if (V == nullptr)
        return -2;
    FGroup const* Group = FindGroupConst(Affiliation);
    if (Group == nullptr)
        return -3;
    unsigned const OldAffiliation = V->Affiliation;
    if (OldAffiliation == Affiliation)
        return 0;
    V->Affiliation = Affiliation;
    if (SinglePlayer() || !Notify)
    {
        V->AffiliationGroup = Group;
        return 0;
    }
    GChangedAffiliationClientMap[V->ClientID] = OldAffiliation;
    V->AffiliationGroup = nullptr;
    return 0;
}

unsigned __cdecl Reputation::Vibe::GetClientID(int GroupId)
{
    // the dynamic store ONLY (cache, else lower_bound the routine +
    // `key < node key` test, cached) -- no static-store fallback. ClientID (node+0x18), or 0.
    VibeValue const* V = FindDynamicVibe(GroupId);
    if (V == nullptr)
        return 0;
    return V->ClientID;
}

int __cdecl Reputation::Vibe::SetClientID(int GroupId, unsigned ClientID)
{
    // the dynamic store ONLY, like GetClientID; -2 on a miss (a key
    // that only has a static slot included), else ClientID (node+0x18) = ClientID, 0.
    VibeValue* V = FindDynamicVibe(GroupId);
    if (V == nullptr)
        return -2;
    V->ClientID = ClientID;
    return 0;
}

int __cdecl Reputation::Vibe::GetInfocard(int const& GroupId, unsigned& Out)
{
    Out = 0;
    VibeValue const* V = FindVibe(GroupId);
    if (V == nullptr)
        return -2;
    Out = V->Infocard;
    return 0;
}

int __cdecl Reputation::Vibe::SetInfocard(int const& GroupId, unsigned Infocard)
{
    VibeValue* V = FindVibe(GroupId);
    if (V == nullptr)
        return -2;
    V->Infocard = Infocard;
    return 0;
}

int __cdecl Reputation::Vibe::GetRank(int const& GroupId, unsigned& Out)
{
    Out = 0;
    VibeValue const* V = FindVibe(GroupId);
    if (V == nullptr)
        return -2;
    Out = V->Rank;
    return 0;
}

int __cdecl Reputation::Vibe::SetRank(int const& GroupId, unsigned Rank)
{
    VibeValue* V = FindVibe(GroupId);
    if (V == nullptr)
        return -2;
    V->Rank = Rank;
    return 0;
}

int __cdecl Reputation::Vibe::GetName(int const& GroupId, FmtStr& Name1Out, FmtStr& Name2Out, wchar_t const*& DisplayNameOut)
{
    VibeValue* V = FindVibe(GroupId);
    if (V == nullptr)
        return -2;
    Name1Out = V->Name1;
    Name2Out = V->Name2;
    DisplayNameOut = V->DisplayName;
    return 0;
}

int __cdecl Reputation::Vibe::SetName(int const& GroupId, FmtStr const& Name1, FmtStr const& Name2, wchar_t const* DisplayName)
{
    VibeValue* V = FindVibe(GroupId);
    if (V == nullptr)
        return -2;
    V->Name1 = Name1;
    V->Name2 = Name2;
    if (DisplayName != nullptr)
    {
        wcsncpy(V->DisplayName, DisplayName, 0x17);
        V->DisplayName[0x17] = L'\0';
    }
    else
    {
        V->DisplayName[0] = L'\0';
    }
    return 0;
}

int __cdecl Reputation::Vibe::GetGroupFeelingsTowards(int const& GroupId, unsigned const& OtherGroupId, float& Out)
{
    // .
    //   Out = 0 (06320fd1); record lookup (dynamic, then static), miss -> -2 (EDI, 06321035);
    //   get_feelings_towards(Other, Affiliation, Out) written straight into Out (06321043-
    // ), non-zero -> -3 (EDI = 0xfffffffd, 06321055);
    //   player-owned record (ClientID != 0): Out = 0 (06321057-06321060);
    //   Override = the GroupFeelings entry for Other, 0 when Other is -1 or has none
    //   (06321065-0632108f);
    //   Out = Override + Out (06321092-06321095), then clamped: `1.0 < Out` -> 1.0 (FCOMP,
    //   TEST AH,0x5, JP keeps Out), `-1.0 > Out` -> -1.0 (TEST AH,0x41, JNZ keeps Out);
    //   returns ECX = 0.
    Out = 0.0f;
    VibeValue const* V = FindVibe(GroupId);
    if (V == nullptr)
        return -2;

    if (Reputation::get_feelings_towards(OtherGroupId, V->Affiliation, Out) != 0)
        return -3;
    if (V->ClientID != 0)
        Out = 0.0f;

    float Override = 0.0f;
    if (OtherGroupId != 0xffffffffu)
    {
        std::size_t Index = 0;
        for (; Index < V->GroupFeelings.size(); ++Index)
        {
            if (V->GroupFeelings[Index].GroupId == OtherGroupId)
            {
                Override = V->GroupFeelings[Index].Feeling;
                break;
            }
        }
    }
    Out = Override + Out;
    if (1.0f < Out)
        Out = 1.0f;
    if (-1.0f > Out)
        Out = -1.0f;
    return 0;
}

int __cdecl Reputation::Vibe::SetGroupFeelingsTowards(int const& GroupId, unsigned const& OtherGroupId, float Feeling)
{
    // , full control flow:
    //   entry lookup                                       miss -> -2 (module storage)
    //   Result = -3; get_feelings_towards(Other, Affiliation, Base) != 0 -> skip to the tail
    //   Result = 0; Delta = Feeling - (ClientID != 0 ? 0 : Base)       (module storage-063215a6)
    //   group registry find(Other) miss -> skip to the tail              (module storage)
    //   clamp Delta to [-1, +1] (the field / the field)             (module storage-063215f2)
    //   store into the GroupFeelings entry for Other, or push_back a new one
    //   tail (module storage): player entries only (ClientID != 0) --
    //     with an affiliation: its own feeling (0 when it has no entry) > 0.65 -> done
    //       (FCOMP + TEST AH,0x41 + JP), otherwise Affiliation = 0xffffffff (module storage);
    //     then the auto-affiliation pass the routine (module storage).
    VibeValue* V = FindVibe(GroupId);
    if (V == nullptr)
        return -2;

    int Result = -3;
    float Base = 0.0f;
    if (Reputation::get_feelings_towards(OtherGroupId, V->Affiliation, Base) == 0)
    {
        Result = 0;
        float const Delta = Feeling - (V->ClientID != 0 ? 0.0f : Base);
        if (FindGroupConst(OtherGroupId) != nullptr)
        {
            float Clamped = (1.0f < Delta) ? 1.0f : Delta;
            if (Clamped < -1.0f)
                Clamped = -1.0f;

            bool bStored = false;
            std::size_t Index = 0;
            for (; Index < V->GroupFeelings.size(); ++Index)
            {
                if (V->GroupFeelings[Index].GroupId == OtherGroupId)
                {
                    V->GroupFeelings[Index].Feeling = Clamped;
                    bStored = true;
                    break;
                }
            }
            if (!bStored)
            {
                GroupFeeling Entry;
                Entry.GroupId = OtherGroupId;
                Entry.Feeling = Clamped;
                V->GroupFeelings.push_back(Entry);
            }
        }
    }

    if (V->ClientID == 0)
        return Result;
    if (V->Affiliation != 0xffffffffu)
    {
        float Own = 0.0f;
        std::size_t Index = 0;
        for (; Index < V->GroupFeelings.size(); ++Index)
        {
            if (V->GroupFeelings[Index].GroupId == V->Affiliation)
            {
                Own = V->GroupFeelings[Index].Feeling;
                break;
            }
        }
        if (!(Own <= kAffiliationKeepFeeling))
            return Result;
        V->Affiliation = 0xffffffffu;
    }
    UpdatePlayerAffiliation(*V);
    return Result;
}

int __cdecl Reputation::Vibe::SetAttitudeTowardsPlayer(int GroupId, float Attitude)
{
    VibeValue* V = FindVibe(GroupId);
    if (V == nullptr)
        return -2;
    V->AttitudeTowardsPlayer = Attitude;
    return 0;
}

int __cdecl Reputation::Vibe::Get(int const& GroupId, unsigned& AffiliationOut, unsigned& RankOut, unsigned char& CountOut, Relation* const Relations, FmtStr& Name1Out, FmtStr& Name2Out, wchar_t const*& DisplayNameOut)
{
    VibeValue* V = FindVibe(GroupId);
    if (V == nullptr)
        return -2;

    AffiliationOut = V->Affiliation;
    RankOut = V->Rank;

    // Commonc46-06320c74 -- single-byte counter, wraps at 256 exactly like
    // baseline's `INC byte ptr [ECX]` (see the declaration's comment in common_reputation.h).
    CountOut = 0;
    for (std::size_t i = 0; i < V->GroupFeelings.size(); ++i)
    {
        Relations[CountOut].GroupId = V->GroupFeelings[i].GroupId;
        Relations[CountOut].Feeling = V->GroupFeelings[i].Feeling;
        ++CountOut;
    }

    Name1Out = V->Name1;
    Name2Out = V->Name2;
    DisplayNameOut = V->DisplayName;
    return 0;
}

int __cdecl Reputation::Vibe::Set(int const& GroupId, unsigned Affiliation, unsigned Rank, unsigned char Count, Relation const* const Relations, FmtStr const& Name1, FmtStr const& Name2, wchar_t const* DisplayName)
{
    if (GroupId == -1)
        return -1;
    VibeValue* V = FindVibe(GroupId);
    if (V == nullptr)
        return -2;

    // (see the declaration's comment in common_reputation.h).
    V->Affiliation = Affiliation;
    // NPC entries cache the new affiliation's group record, null when it is not registered
    // (module storage-06320d43); player entries leave AffiliationGroup as it was.
    if (V->ClientID == 0)
        V->AffiliationGroup = FindGroupConst(Affiliation);
    V->Rank = Rank;

    // erase(begin, end), then grow the buffer to exactly Count when it is smaller
    // (module storage-06320dfc), then keep only registered groups (module storage-06320e57).
    V->GroupFeelings.clear();
    V->GroupFeelings.reserve(Count);
    for (unsigned char i = 0; i < Count; ++i)
    {
        if (FindGroupConst(Relations[i].GroupId) == nullptr)
            continue;
        GroupFeeling Entry;
        Entry.GroupId = Relations[i].GroupId;
        Entry.Feeling = Relations[i].Feeling;
        V->GroupFeelings.push_back(Entry);
    }

    V->Name1 = Name1;
    V->Name2 = Name2;
    wcsncpy(V->DisplayName, DisplayName, 0x17);
    V->DisplayName[0x17] = L'\0';
    UpdatePlayerAffiliation(*V);
    return 0;
}

namespace
{
    // Common:0x0631ff70 (the routine), the attitude-blend core shared by
    // AttitudeTowards/AttitudeTowardsSymmetrical (via the routine @ 0x063217e0, tail-called by
    // AttitudeTowardsSymmetrical @ 0x06321a70). Traced operation-by-operation against the raw
    // analysis (this pass, 2026-09-09, D58):

    //  MP client (not SinglePlayer() and not IsMPServer()): returns From->AttitudeTowardsPlayer
    //  directly -- no blend, matching a networked client trusting its last-replicated value.

    //  SinglePlayer or MP server (the authoritative side):
    //   Base comes from From->AffiliationGroup (VibeValue+0xc), the group record SetAffiliation
    //   and Set cache (module storage-0631ffe7): 0 when that pointer is null or To has no
    //   affiliation; 1 when both share an affiliation; otherwise the record's own feelings map
    //   (record+0x20, the routine find) at To->Affiliation, 0 when absent. The pointer is
    //   cleared by Alloc, by SetAffiliation's MP-notify path and by the auto-affiliation pass,
    //   so Base is 0 for such an entry until its affiliation is set again -- which is why this
    //   reads the cache instead of calling get_feelings_towards(From->Affiliation, ...).
    //   Forced to 0 if To is player-owned (To->ClientID != 0; module storage-0631fff4).
    //   Override = To->GroupFeelings (VibeValue+0x7c vector) entry whose GroupId ==
    //   From->Affiliation, else 0 -- i.e. a per-source-faction override the TARGET carries about
    //   the FROM side's faction.
    //   Sum = Base + Override, then += the From->Deltas entry (AttitudeDelta list, VibeValue+0x88)
    //   whose GroupId == the *raw* target GroupId (baseline dereferences the original int*
    //   argument, not Affiliation) -- matching AdjustAttitude's own by-raw-GroupId bookkeeping.

    // baseline's final clamp-then-quantize step (the routine, Commona0a)
    // snaps the [-1,1]-clamped result through the field/the field -- a round-to-step,
    // rescale and re-offset quantizer, `(v +/- Q) * S`, ftol, `/ S -/+ Q` -- with the field
    // (a near-zero double, ~1e-8, confirmed via raw .rdata bytes -- not a meaningful gameplay
    // dead-zone) snapping the dead center to exactly 0.0f. An earlier pass recorded the two
    // constants as an open gap because they looked like runtime-set globals; they are not, and
    // QuantizeAttitude below now reproduces the step (see its comment for where the values
    // actually come from).
    // The result stays in ST0 (FADDP module storage, FADD float module storage) and every caller
    // consumes it there, so it is double here.
    double BlendAttitudeRaw(Reputation::Vibe::VibeValue const& From, Reputation::Vibe::VibeValue const& To, int RawToGroupId)
    {
        if (!SinglePlayer() && !IsMPServer())
            return From.AttitudeTowardsPlayer;

        float Base = 0.0f;
        FGroup const* FromGroup = (FGroup const*)From.AffiliationGroup;
        if (FromGroup != nullptr && To.Affiliation != 0xffffffffu)
        {
            if (From.Affiliation == To.Affiliation)
            {
                Base = 1.0f;
            }
            else
            {
                std::map<unsigned, float>::const_iterator It = FromGroup->Feelings.find(To.Affiliation);
                if (It != FromGroup->Feelings.end())
                    Base = It->second;
            }
        }
        if (To.ClientID != 0)
            Base = 0.0f;

        // skipped entirely when From has no affiliation.
        float Override = 0.0f;
        if (From.Affiliation != 0xffffffffu)
        {
            for (std::size_t i = 0; i < To.GroupFeelings.size(); ++i)
            {
                if (To.GroupFeelings[i].GroupId == From.Affiliation)
                {
                    Override = To.GroupFeelings[i].Feeling;
                    break;
                }
            }
        }

        double Sum = (double)Base + Override;

        unsigned RawTo = (unsigned)RawToGroupId;
        for (Reputation::Vibe::AttitudeDelta const* Node = From.Deltas; Node != nullptr; Node = Node->Next)
        {
            if (Node->GroupId == RawTo)
            {
                Sum += Node->Delta;
                break;
            }
        }

        return Sum;
    }

    // 's clamp shape (Commond1-06321902 for the forward direction,
    // for the symmetrical reverse direction), the field=+1.0f/
    // =-1.0f confirmed via raw .rdata bytes.
    double ClampAttitude(double Value)
    {
        if (Value > 1.0)
            return 1.0;
        if (Value < -1.0)
            return -1.0;
        return Value;
    }

    // 's quantize step, applied to each output right after ClampAttitude
    // (Common for the forward direction, 063219cb-06321a0a for the
    // symmetrical reverse one). A previous pass left this unported because baseline's two
    // constants sit in .data and looked like values something set at runtime. They are not:
    // both are file-scope doubles with ordinary CRT dynamic initialisers, and this pass read
    // them straight off those initialiser thunks --
    // , CRT init-table slot 0x063ea558:
    //       FLD  double module storage (raw 00 00 00 00 00 00 f0 3f = 1.0)
    //       FDIV double module storage (raw 7b 14 ae 47 e1 7a 84 3f = 0.01)
    //       FSTP double module storage                      -> the field = 1.0 / 0.01
    // , CRT init-table slot 0x063ea55c:
    //       FLD  double module storage (raw 00 00 00 00 00 00 e0 3f = 0.5)
    //       FDIV double module storage
    //       FSTP double module storage                      -> the field = 0.5 / the field
    // so the "unknown-width buckets" are just hundredths: the attitude is rounded half-away-
    // from-zero to two decimal places. The divisions are kept as baseline's initialisers write
    // them; both round to exactly 100.0 and 0.005 (as doubles, and through x87 FSTP double).
    // MSVCRT's ftol truncates toward zero, which is what the long cast does.
    float QuantizeAttitude(double Value)
    {
        double const Step = 1.0 / 0.01;
        double const HalfStep = 0.5 / Step;
        double const Epsilon = 1.0e-8;       // baseline .rdata the field

        double const V = Value;
        if (V < -Epsilon)
            return (float)((double)(long)((V - HalfStep) * Step) / Step - Epsilon);
        if (V > Epsilon)
            return (float)((double)(long)((V + HalfStep) * Step) / Step + Epsilon);
        return 0.0f;
    }

    // (Commonff30-0631ff63), traced this pass (D61). Its only real
    // caller is AdjustAttitude (0x06321d06) -- not GetGroupFeelingsTowards/SetGroupFeelingsTowards
    // as an earlier tracker note guessed from the logic shape alone; those two already reproduce
    // the identical GroupFeelings-walk-with-sentinel-guard pattern independently, this is a third,
    // separate instance baseline factored into its own function specifically for AdjustAttitude's
    // use. Returns 0.0f immediately for the "no affiliation" sentinel (0xffffffff, matching
    // FindVibe's own default Affiliation and the CMP EDX,-1/JZ guard at 0631ff3c-0631ff3f), else
    // walks Owner.GroupFeelings (VibeValue+0x7c vector) for an entry whose GroupId matches
    // SourceAffiliation and returns its Feeling, else 0.0f -- exactly BlendAttitudeRaw's own
    // Override loop above, just baseline's separately-compiled copy of it.
    float FindGroupFeelingOverride(Reputation::Vibe::VibeValue const& Owner, unsigned SourceAffiliation)
    {
        if (SourceAffiliation == 0xffffffffu)
            return 0.0f;

        for (std::size_t i = 0; i < Owner.GroupFeelings.size(); ++i)
        {
            if (Owner.GroupFeelings[i].GroupId == SourceAffiliation)
                return Owner.GroupFeelings[i].Feeling;
        }
        return 0.0f;
    }
}

namespace
{
    // (module storage-06321a6c), the body of AttitudeTowards (which calls it with
    // a null fourth argument, 063217c0-063217d9) and of AttitudeTowardsSymmetrical (a bare
    // `JMP the routine`, 06321a70, so its fourth argument is &Out2).
    //   *Out1 = 0 (063217e3-063217f1); Out2 is not touched before the end;
    //   From lookup (dynamic, then static) miss -> -2 (ESI, 06321857 -> 06321a63);
    //   To lookup miss -> -3 (EBP = 0xfffffffd at 06321871, 063218c1 -> 06321a59);
    //   *Out1 = quantize(clamp(blend(From, To, &ToGroupId))) (063218c7-06321980; the clamped
    //   value is stored first, 06321906-06321908, and overwritten by the quantized one);
    //   Out2 null -> 0 (06321988); else *Out2 = quantize(clamp(blend(To, From,
    //   &FromGroupId))) (0632198e-06321a57); 0.
    // The blend, the clamp and the quantize input stay on the x87 stack in baseline: double here.
    int AttitudeTowardsCore(int const& FromGroupId, int const& ToGroupId, float& Out1, float* Out2)
    {
        Out1 = 0.0f;
        Reputation::Vibe::VibeValue const* From = FindVibe(FromGroupId);
        if (From == nullptr)
            return -2;
        Reputation::Vibe::VibeValue const* To = FindVibe(ToGroupId);
        if (To == nullptr)
            return -3;

        Out1 = QuantizeAttitude(ClampAttitude(BlendAttitudeRaw(*From, *To, ToGroupId)));
        if (Out2 == nullptr)
            return 0;
        *Out2 = QuantizeAttitude(ClampAttitude(BlendAttitudeRaw(*To, *From, FromGroupId)));
        return 0;
    }
}

int __cdecl Reputation::Vibe::AttitudeTowards(int const& FromGroupId, int const& ToGroupId, float& Out)
{
    // the routine(From, To, Out, 0).
    return AttitudeTowardsCore(FromGroupId, ToGroupId, Out, nullptr);
}

int __cdecl Reputation::Vibe::AttitudeTowardsSymmetrical(int const& FromGroupId, int const& ToGroupId, float& Out1, float& Out2)
{
    // JMP the routine -- the fourth argument is &Out2.
    return AttitudeTowardsCore(FromGroupId, ToGroupId, Out1, &Out2);
}

int __cdecl Reputation::Vibe::AdjustAttitude(int const& FromGroupId, int const& ToGroupId, float Delta)
{
    // .
    //   From lookup (dynamic, then static) miss -> -2 (ESI, 06321b5a -> 06321d6b);
    //   To lookup miss -> -3 (ESI = 0xfffffffd at 06321b71, 06321bc3 -> 06321d6b);
    //   the From record's AttitudeDelta for ToGroupId is searched (06321bc9-06321bdf);
    //   the blend the routine(From, To, &ToGroupId) is called and its result popped unused
    //   (06321be4 / 06321be9 FSTP ST0) -- a pure read (SinglePlayer / IsMPServer and loads),
    //   so the port does not call it;
    //   no delta yet: a new 0xc-byte node {Next = old head, GroupId = ToGroupId, Delta = 0} is
    //   pushed on the From record's list (06321bf2-06321c20);
    //   FromAffiliation = -1, or the Affiliation of a fresh From lookup (06321c23-06321c8c);
    //   Base = 0 (06321c90); a fresh To lookup (the routine, then static, 06321c99-06321cd9):
    //     miss, or get_feelings_towards(FromAffiliation, To->Affiliation, Base) != 0
    //     (06321ce4-06321cee) -> node Delta = Delta - Base, unclamped (the branch);
    //     otherwise Base = 0 for a player-owned To (06321cf0-06321cf7), Sum = override(To,
    //     FromAffiliation) (the routine, 06321d06) + Base, clamped like the blend
    //     (`1.0 < Sum` -> 1.0, `-1.0 > Sum` -> -1.0), node Delta = Delta - Sum;
    //   0 on every path past the lookups.
    // The fresh lookups find the records the first two did -- nothing between them adds or
    // removes one -- and are kept as baseline has them.
    VibeValue* From = FindVibe(FromGroupId);
    if (From == nullptr)
        return -2;
    VibeValue* To = FindVibe(ToGroupId);
    if (To == nullptr)
        return -3;

    unsigned const ToId = (unsigned)ToGroupId;
    AttitudeDelta* Node = From->Deltas;
    while (Node != nullptr && Node->GroupId != ToId)
        Node = Node->Next;
    if (Node == nullptr)
    {
        Node = new AttitudeDelta();
        Node->Next = From->Deltas;
        From->Deltas = Node;
        Node->Delta = 0.0f;
        Node->GroupId = ToId;
    }

    unsigned FromAffiliation = 0xffffffffu;
    VibeValue const* FromAgain = FindVibe(FromGroupId);
    if (FromAgain != nullptr)
        FromAffiliation = FromAgain->Affiliation;

    float Base = 0.0f;
    VibeValue const* ToAgain = FindVibe(ToGroupId);
    if (ToAgain == nullptr || Reputation::get_feelings_towards(FromAffiliation, ToAgain->Affiliation, Base) != 0)
    {
        Node->Delta = (float)((double)Delta - Base);
        return 0;
    }
    if (ToAgain->ClientID != 0)
        Base = 0.0f;

    // override + Base stays on the x87 stack through the clamp.
    double Sum = (double)FindGroupFeelingOverride(*ToAgain, FromAffiliation) + Base;
    if (1.0 < Sum)
        Sum = 1.0;
    if (-1.0 > Sum)
        Sum = -1.0;
    Node->Delta = (float)((double)Delta - Sum);
    return 0;
}
