// Author: Giperion.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

// ============================================================================
// IVP_U_Memory -- the 0x14-byte bump/chunk arena installed twice into every
// IVP_Environment (at +0xA0 and +0xA4). Added 2026-09-17 by lane
// claude:common-ivpenv while closing the routine (IVP_Environment's real
// constructor, implementation tracker item 3031).

// PROVENANCE / WHY THIS TYPE REPLACES `CorePool06344120`.
// src/Common/common_stubs.cpp used to carry a second, independent
// transcription of this exact 0x14-byte object under the name
// `CorePool06344120` (a file-local struct plus `the routine`). That copy was
// derived from the CONSTRUCTOR ALONE (the routine -> the routine, which
// only zeroes four dwords and stamps `state = 3`), so every field came out
// typed `unsigned` with no meaning attached, and the name asserted a "core
// pool" role the analysis never supports. Tracing the environment
// constructor's own two call sites settled it:

// MOV EDI,[EBX+0x8]        ; Parms->MemoryBlockSize
// MOV ECX,[EBX+0x4]        ; Parms->MemoryBlock
// SAR EDI,0x1              ; half of it
// PUSH EDI                 ; size
// PUSH ECX                 ; base
// MOV ECX,[ESI+0xa0]
// CALL the routine        ; arena_a0.init_mem(base, half)
// MOV EDX,[EBX+0x4]
// MOV ECX,[ESI+0xa4]
// PUSH EDI                 ; same half size
// ADD EDX,EDI              ; base + half
// PUSH EDX
// CALL the routine        ; arena_a4.init_mem(base+half, half)

// i.e. one caller-supplied raw memory block is split down the middle and
// handed to the two objects, which is a memory arena and nothing else. That
// also matches the independent conclusion already recorded in
// src/IVP/ivp_physics/ivp_contact_point.cpp:590, which calls +0xA0 a
// "per-environment scratch arena".

// FIELD LAYOUT -- every offset below is proven by the routine
// (Common) and the routine (via the routine), not by
// any name.
//   +0x00 void*   ChunkListStart : set to ChunkListHead after a successful
//                 install (`MOV EDX,[ESI+0x4]; MOV [ESI],EDX`, 0634403a/
// ) and again in the reset path (0634505d-06344060).
//   +0x04 void*   ChunkListHead  : head of the singly-linked chunk chain; the
//                 newly installed block's own first dword is overwritten with
//                 the PREVIOUS head (`MOV EDX,[ESI+0x4]; MOV [EAX],EDX`,
// /0634402f), so each chunk carries its `next` inline.
//   +0x08 char*   Cursor         : the 32-byte-aligned first usable byte of
//                 the current chunk, `(block + 0x23) & ~0x1F` (0634403d-
// ). +0x23 == +0x20 (one chunk header) + 0x1F
//                 (round-up slack), so the header is 32 bytes.
//   +0x0C char*   End            : Cursor + UsableSize (06344048-0634404a).
//   +0x10 uint16  State          : 0 once a real block is installed
//                 (0634401b); the routine leaves it at 3 on a fresh object,
//                 so 3 means "constructed, no block yet".
//   +0x12 uint16  UsableSize     : Size - 0x20, i.e. the block minus its
//                 32-byte header (06344028-0634402b). SIXTEEN bits, not 32 --
//                 both writes are `MOV word ptr`, so a block larger than
//                 0xFFFF + 0x20 silently truncates in baseline too.
// ============================================================================
struct IVP_U_Memory
{
    void*    ChunkListStart; // +0x00
    void*    ChunkListHead;  // +0x04
    char*    Cursor;         // +0x08
    char*    End;            // +0x0C
    uint16_t State;          // +0x10
    uint16_t UsableSize;     // +0x12

    // (module storage-0634412b): `CALL the routine; MOV EAX,ESI` --
    // the constructor is init_object. IVP_Environment's ctor news two of these
    // (module storage-0634c511 / 0634c52a-0634c544).
    IVP_U_Memory()
    {
        init_object();
    }

    // First usable byte of a chunk: past the 32-byte header, rounded up to 32 bytes
    // (`LEA EAX,[block+0x23]; AND EAX,0xffffffe0`, inlined at every baseline site).
    static char* ChunkPayload(void* Chunk)
    {
        return (char*)(((uintptr_t)Chunk + 0x23) & ~(uintptr_t)0x1F);
    }

    // (`JMP the routine`): ~IVP_Environment calls it on +0xa0 / +0xa4
    // before operator delete (module storage / 0634c8e5), i.e. `delete Arena`.
    ~IVP_U_Memory()
    {
        free_mem();
    }

    // (implementation tracker item 2756,
    // Common). Zeroes the four leading dwords, then
    // stamps State = 3 and UsableSize = 0.
    void init_object()
    {
        ChunkListStart = nullptr;
        ChunkListHead  = nullptr;
        Cursor         = nullptr;
        End            = nullptr;
        State          = 3;
        UsableSize     = 0;
    }

    // (implementation tracker item 2753, Common,
    // 32 insns, __thiscall with two stack args). Installs a caller-supplied
    // block as the arena's current chunk.

    // NOT PORTED 1:1 -- the `Block == nullptr` reset path is a documented
    // open gap, see the TODO below.
    void init_mem(char* Block, int32_t Size)
    {
        State = 0; // written on BOTH paths, before the branch.

        if (Block == nullptr)
        {
            // (06344051-06344063): UsableSize = 0,
            // `PUSH 0; CALL the routine` (GetMemNewBlock(0) -- allocates the
            // first heap chunk), then ChunkListStart = ChunkListHead.
            UsableSize = 0;
            GetMemNewBlock(0);
            ChunkListStart = ChunkListHead;
            return;
        }

        UsableSize = (uint16_t)(Size - 0x20);

        // The block's own first dword becomes the `next` link (06344025/
        // ), then the block becomes the new head (06344037).
        *(void**)Block = ChunkListHead;
        ChunkListHead = Block;

        // 32-byte-aligned first usable byte, past the 32-byte chunk header.
        Cursor = ChunkPayload(Block);
        End = Cursor + UsableSize;
        ChunkListStart = ChunkListHead; // /06344046
    }

    // (implementation tracker item 2754). Frees every chunk in the chain
    // ABOVE the original one, then rewinds the cursor back to the start of
    // that original chunk -- the arena's "release everything allocated since
    // init_mem" reset.

    // Moved here 2026-09-17 (lane claude:common-ivpenv) from a file-local
    // duplicate: src/IVP/ivp_collision/ivp_mindist_limiter.cpp carried its own
    // `struct FrozenCorePool` + `ResetFrozenCorePoolChain()` transcription of
    // this same object and this same function. Its own comment conceded the
    // identification was a guess ("likely a per-environment bump allocator"),
    // and it is now confirmed to be exactly this type -- note that its
    // independently-derived `(Base + 0x23) & ~0x1F` alignment and 0x7fe0
    // default size agree byte-for-byte with init_mem above, which is what
    // makes the merge safe.
    void reset_chunks()
    {
        void* Node = ChunkListHead;
        while (Node != nullptr)
        {
            if (Node == ChunkListStart)
                break; // the original chunk is the sentinel -- never freed
            void* Next = *(void**)Node;
            free(Node); // confirmed 1:1 free() thunk
            Node = Next;
        }

        char* FreeStart = ChunkPayload(ChunkListStart);
        Cursor = FreeStart;
        // A zero UsableSize means "never had a real block installed"; baseline
        // falls back to the 0x7fe0 default chunk payload in that case.
        End = FreeStart + (UsableSize != 0 ? UsableSize : 0x7fe0);
        ChunkListHead = ChunkListStart;
    }

    // (module storage-06344198; also reached through the JMP thunk
    // , which ~IVP_Environment calls on +0xa0 / +0xa4 before operator
    // delete). Walks the chunk chain from ChunkListHead: the next link is read
    // first; when UsableSize != 0 and the chunk is ChunkListStart (the
    // caller-supplied block) the walk stops without freeing it; otherwise the
    // chunk is freed (06343fb0). Then Cursor, End, ChunkListHead and
    // ChunkListStart are zeroed. cmn-phy7 2026-10-08.
    void free_mem()
    {
        void* Node = ChunkListHead;                          // :06344153
        while (Node != nullptr)                              // :06344156-:06344159 / :06344175
        {
            void* const Next = *(void**)Node;                 // :06344160
            if (UsableSize != 0 && Node == ChunkListStart)    // :0634415b-:06344166
            {
                break;
            }
            free(Node);                                      // :0634416c (null cannot reach here)
            Node = Next;
        }
        Cursor = nullptr;                                    // :0634417c
        End = nullptr;                                       // :06344183
        ChunkListHead = nullptr;                             // :0634418a
        ChunkListStart = nullptr;                            // :06344191
    }

    // (Common module storage-06344113, __thiscall, RET 4).
    // Slow path of GetMem: mallocs a fresh chunk of max(0x7fd8, round32(Size))
    // payload bytes plus 0x28 (the routine = malloc), links it in front of the
    // chain (its first dword = previous head), and hands out its first 32-byte
    // aligned byte. Cursor moves past the requested size, End = start + payload.
    // ChunkListStart is NOT touched (only init_mem/reset_chunks write it).
    void* GetMemNewBlock(int Size)
    {
        unsigned int Rounded = ((unsigned int)Size + 0x1f) & 0xffffffe0u;
        unsigned int Payload = 0x7fd8;
        if (Rounded > Payload)                                                       // CMP ESI,EBX; JBE
            Payload = Rounded;
        char* Block = (char*)malloc(Payload + 0x28);
        *(void**)Block = ChunkListHead;
        ChunkListHead = Block;
        char* Start = ChunkPayload(Block);
        Cursor = Start + Rounded;                                                    // /0634410a
        End = Start + Payload;                                                       // /0634410d
        return Start;
    }

    // (Common module storage-063473a7, __thiscall, RET 4).
    // Bump allocation: the block starts at the current Cursor; the new Cursor is
    // (Cursor + Size + 0x1f) & ~0x1f. If that is below End (unsigned JC) it is
    // committed, otherwise the request goes to GetMemNewBlock. Several baseline
    // callers inline this exact sequence (e.g. 063666f0, 06368300, 06368560).
    void* GetMem(int Size)
    {
        char* Old = Cursor;
        char* New = (char*)(((uintptr_t)Old + (unsigned int)Size + 0x1f) & ~(uintptr_t)0x1F);
        if ((uintptr_t)New < (uintptr_t)End)                                         // CMP EDX,EDI; JC
        {
            Cursor = New;
            return Old;
        }
        return GetMemNewBlock(Size);
    }
};

static_assert(sizeof(IVP_U_Memory) == 0x14, "IVP_U_Memory must be 0x14 bytes");
static_assert(offsetof(IVP_U_Memory, ChunkListHead) == 0x04, "IVP_U_Memory+0x04");
static_assert(offsetof(IVP_U_Memory, Cursor) == 0x08, "IVP_U_Memory+0x08");
static_assert(offsetof(IVP_U_Memory, End) == 0x0C, "IVP_U_Memory+0x0C");
static_assert(offsetof(IVP_U_Memory, State) == 0x10, "IVP_U_Memory+0x10");
static_assert(offsetof(IVP_U_Memory, UsableSize) == 0x12, "IVP_U_Memory+0x12");
