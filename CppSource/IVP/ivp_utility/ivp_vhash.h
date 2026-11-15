// Author: Giperion.

#pragma once

#include <cstdlib>

// ============================================================================
// IVP_VHash -- the IVP SDK's open-addressing hash of element pointers
// (2026-10-08, claude-code:cmn-phy4).

// Abstract base vtable module storage (two slots):
//   slot 0  the branch (_purecall) -- compare(elem0, elem1), TRUE when equal
//   slot 1  the routine -- scalar deleting destructor -> the routine
// Layout (0x10 bytes):
//   +0x04 SizeMm          capacity - 1 (used as the index mask)
//   +0x08 NElemsAndFlags  low 24 bits = element count, top byte = "the element
//                         array is not owned" (the destructor then leaves it)
//   +0x0c Elems           calloc'd array of {hash index | 0x80000000, elem}

// The probe / insert / remove / rehash bodies (the routine / 063523d0 /
// / 06352320) are the members below, defined in ivp_vhash.cpp
// (moved there from src/Common/common_stubs.cpp, 2026-10-14). They dispatch
// the comparison through slot 0 of the object's own vtable, i.e. Compare.

// The constructor and destructor stay inline here; the destructor below is
// the non-deleting the routine.
// ============================================================================

class IVP_VHash
{
public:
    int SizeMm;              // +0x04
    unsigned NElemsAndFlags; // +0x08
    void* Elems;             // +0x0c

    // (thiscall, RET 4): vtable 063a5648, SizeMm = Size - 1,
    // count and flags 0, Elems = the routine(Size, 8) (zeroed allocation).
    explicit IVP_VHash(int Size)
        : SizeMm(Size - 1)
        , NElemsAndFlags(0)
        , Elems(calloc(static_cast<size_t>(Size), 8))
    {
    }

    // slot 0: baseline _purecall in the base.
    virtual int Compare(void* Elem0, void* Elem1) = 0;

    // slot 1: the routine -> the routine. Stamps the base vtable, then frees
    // Elems (the routine) unless the top byte of +0x08 is set; Elems is nulled
    // only on the freeing path (module storage-06352312).
    virtual ~IVP_VHash()
    {
        if ((NElemsAndFlags & 0xff000000u) == 0 && Elems != nullptr)
        {
            free(Elems);
            Elems = nullptr;
        }
    }

    // (formerly the raw extern "C" the routine): the stored
    // element equal to Elem, or null.
    void* FindElem(void* Elem, unsigned HashIndex);

    // (formerly extern "C" the routine): insert (grows to twice
    // the size first at half load).
    void AddElem(void* Elem, unsigned HashIndex);

    // (formerly extern "C" the routine): removes and returns the
    // stored element equal to Elem. baseline writes to address 0 when the
    // element is absent.
    void* RemoveElem(void* Elem, unsigned HashIndex);

protected:
    // (formerly extern "C" the routine): regrow to exactly
    // NewSize slots and re-insert every stored element.
    void Rehash(int NewSize);
};
static_assert(sizeof(IVP_VHash) == 0x10, "IVP_VHash is 0x10 bytes");

// (formerly extern "C" the routine, thiscall RET 4 with an
// unused receiver): CRC-32 (the field) over the four bytes of Key, OR
// the called copy of the loop the IVP_VHash_Store members
// inline. Caller: ivp_cluster_manager.cpp. Defined in ivp_vhash.cpp.
unsigned __fastcall IVP_VHash_HashPointer(unsigned Key);

// ============================================================================
// IVP_VHash_Store -- the IVP SDK's key -> value store (no vtable, 0x14 bytes),
// claude-code:cmn-ivp3d 2026-10-08. Robin Hood open addressing over 12-byte
// entries {hash | 0x80000000, key, value}; the key is hashed through the
// CRC-32 table the field. A zero value marks an empty slot.
// Layout (the routine module storage-06352618):
//   +0x00 Size        capacity (power of two)
//   +0x04 SizeMm      Size - 1, the index mask
//   +0x08 NElems      occupied entries
//   +0x0c ElemsStore  calloc'd entry array (the routine(Size, 0xc))
//   +0x10 Field_0x10  zeroed by the ctor; the dtor (the routine :06352626)
//                     and the grow step (the routine) leave ElemsStore alone
//                     when it equals this field. TODO: no writer other than
//                     the ctor traced.
// Every member is the one port of its baseline body, defined in ivp_vhash.cpp
// (moved from src/Common/common_stubs.cpp, 2026-10-14).
// Users: IVP_Controller_Phantom (ivp_phantom.h, +0x2c / +0x30) and
// IVP_Core::ObjectHashMap (ivp_core.cpp).
// ============================================================================

class IVP_VHash_Store
{
public:
    int Size;          // +0x00
    int SizeMm;        // +0x04
    int NElems;        // +0x08
    void* ElemsStore;  // +0x0c
    void* Field_0x10;  // +0x10

    // (thiscall, RET 4; formerly extern "C" the routine).
    explicit IVP_VHash_Store(int InSize);

    // (formerly extern "C" the routine; non-virtual, baseline
    // callers run it and then operator delete, e.g. 063442c0
    // :06344311-06344317).
    ~IVP_VHash_Store();

    // (RET 8; formerly extern "C" the routine): unconditional
    // insert of (Key, Value).
    void AddElem(void* Key, void* Value);

    // (formerly extern "C" the routine): the raw Robin Hood
    // insert with a caller-supplied hash index (top bit already set).
    void AddElem(void* Key, void* Value, unsigned HashIndex);

    // (RET 4; formerly extern "C" the routine): removes Key and
    // returns its value.
    void* RemoveElem(void* Key);

    // (formerly extern "C" the routine): the raw backward-shift
    // erase with a caller-supplied hash index; returns the removed value.
    void* RemoveElem(void* Key, unsigned HashIndex);

    // (RET 4; formerly extern "C" the routine): the value
    // stored for Key, or null.
    void* FindElem(void* Key);

    // (formerly extern "C" the routine): the raw find with a
    // caller-supplied hash index.
    void* FindElem(void* Key, unsigned HashIndex);

    // (RET 8; formerly extern "C" the routine): overwrite the
    // value of an existing Key; no insert when Key is absent.
    void ChangeElem(void* Key, void* NewValue);

private:
    // (formerly extern "C" the routine): regrow to NewSize
    // entries and re-insert every occupied one.
    void Rehash(int NewSize);
};
static_assert(sizeof(IVP_VHash_Store) == 0x14, "IVP_VHash_Store is 0x14 bytes (operator new(0x14), 06344710 :063447c1)");
