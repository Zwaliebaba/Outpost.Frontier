// Author: Giperion.

#pragma once

// ============================================================================
// IVP_Hash -- the IVP SDK's chained hash with a fixed key length
// (2026-10-08, claude-code:cmn-ivp4b).

// baseline (Common.dll); the bodies are in ivp_hash.cpp (moved from
// src/Common/common_stubs.cpp, 2026-10-14):
// IVP_Hash(int size, int key_size, void* not_found_value) (thiscall)
// ~IVP_Hash()                                            (thiscall)
// find(const char* key)                                   (thiscall)
// add(const char* key, void* value)                       (thiscall)
// Layout (0x10 bytes): +0 key size, +4 bucket count, +8 value returned on a
// miss, +0xc calloc'd bucket array of chain heads. Callers allocate it with
// operator new(0x10) or keep it on the stack.
// ============================================================================

#include <cstdint>

struct IVP_Hash_Elem;

class IVP_Hash
{
public:
    // (formerly extern "C" the routine).
    IVP_Hash(int32_t Size, int32_t KeySize, unsigned NotFoundValue);

    // (formerly extern "C" the routine).
    ~IVP_Hash();

    IVP_Hash(const IVP_Hash&) = delete;
    IVP_Hash& operator=(const IVP_Hash&) = delete;

    // (formerly extern "C" the routine): the value stored under
    // Key, or the not-found value.
    unsigned Find(const void* Key);

    // (formerly extern "C" the routine): inserts Key -> Value (no
    // duplicate check).
    void Add(const void* Key, unsigned Value);

private:
    int32_t KeyLength;      // +0x00 bytes hashed/compared per key (signed loop tests)
    int32_t BucketCount;    // +0x04 divisor for `hash % BucketCount` (unsigned DIV)
    unsigned DefaultValue;  // +0x08 returned by Find on a miss
    IVP_Hash_Elem** Buckets; // +0x0c chain heads
};
static_assert(sizeof(IVP_Hash) == 0x10, "IVP_Hash is 0x10 bytes");

// (item 3257; defined in ivp_hash.cpp): three-way compare of two
// null-checked strings (-1 / 0 / 1), a null first string sorting after a
// non-null one. Caller: IVP_U_Active_Value_Hash::Compare (ivp_active_value.cpp).
int IVP_CompareStringsNullsLast(const char* First, const char* Second); // formerly extern "C" the routine
