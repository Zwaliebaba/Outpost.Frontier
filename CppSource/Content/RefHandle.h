// Author: Giperion.

#pragma once

// Content.dll: the ref-holding pointer baseline keeps for its refcounted objects (directives,
// guides, guide descriptions: slot 0 AddRef, slot 1 Release). baseline inlines it as a member
// or local whose copy AddRefs (e.g. the routine) and whose destructor / unwind step Releases
// when set (the routine, the routine). The port holds such members and locals in this type
// so the release runs in the same member / local order and on the exception path (AGENTS.md
// RE rule 13). A raw-pointer assignment does not touch the count: baseline's inlined code around
// the handle does its own AddRef / Release, and the port's callers keep doing the same.
// Lane CNT-AI added it; any lane may use it.

template <typename T>
class TRefHandle
{
public:
    TRefHandle() = default;
    // Copy + AddRef when set (the routine pattern).
    TRefHandle(TRefHandle const& Other)
        : Pointer(Other.Pointer)
    {
        if (Pointer != nullptr)
            Pointer->AddRef();
    }
    TRefHandle& operator=(TRefHandle const&) = delete;
    // Release when set (the routine pattern).
    ~TRefHandle()
    {
        if (Pointer != nullptr)
            Pointer->Release();
    }

    // Raw store, no reference change.
    TRefHandle& operator=(T* Raw)
    {
        Pointer = Raw;
        return *this;
    }
    T* operator->() const
    {
        return Pointer;
    }
    operator T*() const
    {
        return Pointer;
    }

    T* Pointer = nullptr;
};
