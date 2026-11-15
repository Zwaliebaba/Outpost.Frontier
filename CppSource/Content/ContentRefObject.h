// Author: Giperion.

#pragma once

// The ref-counted base shared by Content.dll's reference-held objects (vtable 06fb43f8:
// AddRef, the routine Release, the branch scalar deleting dtor). Its dtor
// body the routine only resets the vptr to 06fb43f8; the shared scalar deleting dtor
// (base dtor, then free when bit 0 is set) is slot 2 of about twenty derived
// vtables (FBaseFixture 06fb4498, FContentPlayerStateBase 06fb4640, the bar offers, the
// guide descriptions, the ObjList directives, ...). Content.dll carries no RTTI (the dword
// before each vtable is the previous vtable's last slot), so the baseline class name is
// unknown; the port name is ours.
class FContentRefObject
{
public:
    FContentRefObject() = default;
    // Slot 0, the routine (06eacd40-06eacd47): ++count, returns it.
    virtual unsigned int AddRef();
    // Slot 1, the routine (06eacd50-06eacd66): --count; at 0 vtbl[+8](1) (delete this);
    // returns the count.
    virtual unsigned int Release();
    // Slot 2 (the routine body; the branch / the routine deleting forms).
    virtual ~FContentRefObject() = default;

    unsigned int ReferenceCount = 1;        // +0x04 1 after new
};
