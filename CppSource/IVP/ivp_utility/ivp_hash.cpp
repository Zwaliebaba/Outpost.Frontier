// Author: Giperion.

// IVP_Hash bodies (06356a40 / 06356a70 / 06356ac0 / 06356b40) and the string
// compare the routine next to them. Moved verbatim from
// src/Common/common_stubs.cpp (via common_stubs_a3.cpp) on 2026-10-14
// (claude-code:cmn-a3); the raw extern "C" FUN_ entry points became the
// IVP_Hash members declared in ivp_hash.h. No logic changed in the move.

#include "ivp_hash.h"

#include "ivp_crc32.h"

#include <cstdlib>
#include <cstring>

// ============================================================================
// d230:cluster-063568a0 pass (2026-09-10): items 3255/3257/3258/3259/3260/
// 3261 (Common). A SEPARATE hash-table implementation
// from IVP_VHash_Store (ivp_vhash.cpp) -- this one is a classic
// fixed-key-length chained (linked-list-bucket) hash table, not
// open-addressed, and its entries own malloc'd copies of their own key bytes
// rather than storing a bare 32-bit key. It reuses the SAME CRC32 table
// global (g_Crc32Table_063edcdc, confirmed byte-identical: both read
// &the field) but hashes over a caller-fixed number of KEY BYTES (a
// string/blob, not a single 4-byte int) via its own byte-at-a-time loop
// (Commonad7-06356af4 / 06356b56-06356b73), so it gets its own
// small helper below rather than reusing HashKey_063edcdc.

// Layout (16 bytes), fields confirmed against the routine's own ctor writes
// and cross-checked by every read site in the routine/the routine/

//   +0x00 KeyLength    number of bytes hashed/compared per key (same for
//                       every key in a given table instance -- a
//                       FIXED-length-key table, not null-terminated strings).
//   +0x04 BucketCount  divisor for `hash % BucketCount`; also the element
//                       count passed to the ctor's calloc call.
//   +0x08 DefaultValue returned by Find on a miss.
//   +0x0c Buckets      calloc'd BucketCount-slot array of chain-head
//                       pointers (IVP_Hash_Elem*).
// Node layout (KeyLength+0xc bytes, malloc'd per insert -- baseline
// over-allocates by 4 bytes past the 8-byte header + KeyLength payload it
// actually uses; reproduced 1:1, not "fixed" as a bug):
//   +0x00 Next   chain link.
//   +0x04 Value  caller payload.
//   +0x08 Key[]  KeyLength raw bytes, memcpy'd in by Add.
// ============================================================================

struct IVP_Hash_Elem // formerly FixedKeyHashNode_06356a40
{
    IVP_Hash_Elem* Next; // +0x00
    unsigned Value;      // +0x04
    // +0x08: KeyLength raw key bytes follow inline (see the
    // malloc(KeyLength+0xc) call in IVP_Hash::Add below).
    unsigned char* KeyBytes()
    {
        return (unsigned char*)(this + 1);
    }
};
static_assert(sizeof(IVP_Hash_Elem) == 0x8, "IVP_Hash_Elem key bytes start at +0x08");

namespace
{
    // (inlined again at 06356b56-06356b73 for Add): CRC-32
    // over Length key bytes, starting from 0xffffffff, no final inversion. A
    // Length <= 0 hashes nothing (`LEA ECX,[EBX-1]; TEST ECX,ECX; JL`).
    unsigned HashKeyBytes(const unsigned char* Key, int32_t Length)
    {
        unsigned Crc = 0xffffffffu;
        for (int32_t I = 0; I < Length; ++I)
        {
            Crc = (Crc >> 8) ^ g_Crc32Table_063edcdc[(Key[I] ^ Crc) & 0xff];
        }
        return Crc;
    }
}

// (item 3258, Common, 16 insns, tiny).
// Constructor. `param_1`=BucketCount, `param_2`=KeyLength, `param_3`=
// DefaultValue (stack-arg order confirmed against the raw asm's own
// `MOV [ESI+4],EAX(param_1)` / `MOV [ESI],ECX(param_2)` /
// `MOV [ESI+8],EDX(param_3)` writes). Buckets = calloc(4, BucketCount) --
// is calloc(count,size) (established convention, see
// IVP_VHash_Store's own ctor); swapping which literal argument is "count" vs
// "size" doesn't change the allocated byte total (4*BucketCount either way),
// so this is a faithful port regardless of that ambiguity.
IVP_Hash::IVP_Hash(int32_t Size, int32_t KeySize, unsigned NotFoundValue) // formerly extern "C" the routine
{
    BucketCount = Size;
    KeyLength = KeySize;
    DefaultValue = NotFoundValue;
    Buckets = (IVP_Hash_Elem**)calloc(4, (size_t)Size); // the routine
}

// (item 3259, Common, 36 insns). Frees
// every chain node in every bucket, then frees the Buckets array itself and
// nulls the field (matches baseline's own final `MOV [param_1+0xc],0`). Does
// NOT free `this` and does NOT touch KeyLength/BucketCount/DefaultValue --
// operator delete (or the stack frame) releases the object separately.
IVP_Hash::~IVP_Hash() // formerly extern "C" the routine
{
    for (int32_t I = 0; I < BucketCount; ++I) // (signed, count re-read)
    {
        IVP_Hash_Elem* Node = Buckets[I];
        while (Node != nullptr)
        {
            IVP_Hash_Elem* Next = Node->Next;
            free(Node);
            Node = Next;
        }
    }
    if (Buckets != nullptr)
    {
        free(Buckets);
        Buckets = nullptr;
    }
}

// (item 3260, Common, 51 insns). Find:
// hash the first KeyLength bytes of `param_1`, walk the matching bucket's
// chain comparing KeyLength bytes exactly (baseline's `CMPSB.REPE`), return
// the matching node's Value or DefaultValue on a miss/empty chain.
unsigned IVP_Hash::Find(const void* Key) // formerly extern "C" the routine
{
    const unsigned char* KeyBytes = (const unsigned char*)Key;
    const unsigned Hash = HashKeyBytes(KeyBytes, KeyLength);
    IVP_Hash_Elem* Node = Buckets[Hash % (unsigned)BucketCount]; // (unsigned DIV)
    while (Node != nullptr)
    {
        if (memcmp(Node->KeyBytes(), KeyBytes, (size_t)KeyLength) == 0) // REPE CMPSB
        {
            return Node->Value;
        }
        Node = Node->Next;
    }
    return DefaultValue;
}

// (item 3261, Common, 50 insns). Add:
// unconditional head-insert into the target bucket's chain (no duplicate-key
// check, matching baseline exactly -- a repeat key just shadows the earlier
// node, which becomes unreachable and leaks, same as baseline). Allocates
// KeyLength+0xc bytes (see the node-layout comment above for the 4
// unexplained-but-harmless extra bytes) and memcpy's the key in.
void IVP_Hash::Add(const void* Key, unsigned Value) // formerly extern "C" the routine
{
    const unsigned char* KeyBytes = (const unsigned char*)Key;
    const unsigned Hash = HashKeyBytes(KeyBytes, KeyLength);
    const unsigned BucketIndex = Hash % (unsigned)BucketCount;

    IVP_Hash_Elem* Node = (IVP_Hash_Elem*)malloc((size_t)KeyLength + 0xc); // the routine
    memcpy(Node->KeyBytes(), KeyBytes, (size_t)KeyLength);

    Node->Next = Buckets[BucketIndex];
    Buckets[BucketIndex] = Node;
    Node->Value = Value;
}

// (item 3257, module storage-06356a32, cdecl). Three-way compare
// of two null-checked strings: 0 when equal, -1 / 1 at the first byte that
// differs (bytes compared unsigned: `CMP DL,BL` then `SBB EAX,EAX; SBB EAX,-1`).
// A null first string returns 1 when the second is non-null (0 when both are
// null); a non-null first string against a null second returns -1. The loop is
// the inlined CRT strcmp, two bytes per pass. Caller: the branch,
// IVP_U_Active_Value_Hash::Compare (ivp_active_value.cpp).
int IVP_CompareStringsNullsLast(const char* First, const char* Second) // formerly extern "C" the routine
{
    if (First == nullptr)
    {
        return Second != nullptr ? 1 : 0;
    }
    if (Second == nullptr)
    {
        return -1;
    }
    const unsigned char* A = (const unsigned char*)First;
    const unsigned char* B = (const unsigned char*)Second;
    for (;;)
    {
        if (A[0] != B[0])
        {
            return A[0] < B[0] ? -1 : 1;
        }
        if (A[0] == 0)
        {
            return 0;
        }
        if (A[1] != B[1])
        {
            return A[1] < B[1] ? -1 : 1;
        }
        if (A[1] == 0)
        {
            return 0;
        }
        A += 2;
        B += 2;
    }
}
