// Author: Giperion.

#pragma once

#include <cstdlib>

template <class T>
class IVP_U_Vector
{
public:
    unsigned short memsize; // +0x00: Capacity
    unsigned short n_elems; // +0x02: Element count
    T** elems;              // +0x04: Pointer to array of T* pointers

    IVP_U_Vector()
        : memsize(0)
        , n_elems(0)
        , elems(nullptr)
    {
    }

    // (thiscall, RET 4): memsize = Size, n_elems = 0, elems =
    // P_MALLOC(Size * 4) (the routine) when Size != 0, else null.
    explicit IVP_U_Vector(int Size)
        : memsize((unsigned short)Size)
        , n_elems(0)
        , elems(Size != 0 ? (T**)malloc((size_t)Size * 4u) : nullptr)
    {
    }

    IVP_U_Vector(T** Buffer, int Size)
        : memsize((unsigned short)Size)
        , n_elems(0)
        , elems(Buffer)
    {
    }

    ~IVP_U_Vector()
    {
        Clear();
    }

    // (and the identical instantiation the routine, the friction
    // solver's vector destructor run by Unwind@06398580): when elems is not the
    // inline buffer right after the header, free it if non-null and then --
    // unconditionally on that branch -- null elems and zero memsize
    // (module storage-063449f1); n_elems is zeroed on both branches.
    // 2026-10-08 (claude-code:cmn-phy2): the null-elems case used to leave
    // memsize alone.
    void Clear()
    {
        if (elems != InlineBuffer())
        {
            if (elems != nullptr)
            {
                free(elems);
            }
            elems = nullptr;
            memsize = 0;
        }
        n_elems = 0;
    }

    void Add(T* Elem)
    {
        if (n_elems >= memsize)
        {
            Realloc();
        }
        elems[n_elems++] = Elem;
    }

    // (Common, implementation tracker
    // Common.dll item 3288, ported D104 2026-09-09). Backward linear search
    // returning found/not-found only (no removal) -- the read-only sibling of
    // RemoveElement's identical backward scan below. Confirmed analysis-exact:
    // `XOR EAX,EAX; MOV AX,word[ECX+0x1a] (n_elems); DEC EAX; JS -> return 0;
    // MOV ECX,[ECX+0x1c] (elems); LEA ECX,[ECX+EAX*4]` then walks ECX
    // downward by 4 comparing *ECX to the needle, `JZ -> return 1`, else
    // `DEC EAX; SUB ECX,4; TEST EAX,EAX; JGE loop`. Single caller in the
    // still-unmodeled Common.dll candidate-mindist cluster (the routine
    // family, 0x06358d60-0x0635a1c0) is the routine (item 3287,
    // Common, `CALL the routine` on the vector at
    // this+8 to skip re-adding a candidate already present) -- that caller
    // itself is left blocked pending the owning class, see item 3287's notes.
    bool Contains(T* Elem) const
    {
        for (int I = (int)n_elems - 1; I >= 0; --I)
        {
            if (elems[I] == Elem)
                return true;
        }
        return false;
    }

    // The generic IVP_U_Vector::remove (06355310 and its inlined copies, e.g.
    // / 06344260): backward search, then -- with NO not-found test --
    // n_elems = n_elems - 1 (u16) and the tail shifts down from the found
    // index while i < n_elems (re-read each pass).
    // VERY STRANGE, REPRODUCED ON PURPOSE (cmn-ivp3d 2026-10-08; the port
    // used to return -1 untouched): an absent Elem gives index -1, so
    // elems[-1] = elems[0] and the whole vector shifts down; an empty vector
    // wraps n_elems to 0xffff. Returns the index (-1 when absent).
    int RemoveElement(T* Elem)
    {
        int I = (int)n_elems - 1;
        while (I >= 0)
        {
            if (elems[I] == Elem)
            {
                break;
            }
            --I;
        }
        const int Index = I;
        n_elems = (unsigned short)(n_elems - 1);
        for (; I < (int)n_elems; ++I)
        {
            elems[I] = elems[I + 1];
        }
        return Index;
    }

    void RemoveAt(int Index)
    {
        n_elems--;
        for (int I = Index; I < (int)n_elems; ++I)
        {
            elems[I] = elems[I + 1];
        }
    }

    void RemoveAtAndAllowSort(int Index)
    {
        n_elems--;
        elems[Index] = elems[n_elems];
    }

    T* ElementAt(int Index) const
    {
        return elems[Index];
    }

    T* operator[](int Index) const
    {
        return elems[Index];
    }

    int Len() const
    {
        return (int)n_elems;
    }

protected:
    // The inline storage of IVP_U_Vector_Fixed starts right after this 8-byte header;
    // baseline tests `elems == this+8` (e.g. module storage) before freeing.
    T** InlineBuffer()
    {
        return (T**)(this + 1);
    }

    // (module storage-06344206): malloc(memsize * 8 + 8) bytes, i.e. room
    // for 2 * memsize + 2 pointers, while memsize becomes the u16 2 * memsize + 1; copy
    // the n_elems pointers; free the old block unless it is the inline buffer or null.
    void Realloc()
    {
        unsigned int NewByteCount = ((unsigned int)memsize * 8) + 8;
        T** NewBuffer = (T**)malloc(NewByteCount);                  // the routine

        unsigned short NewMemsize = (unsigned short)((memsize << 1) + 1);
        memsize = NewMemsize;

        for (int I = 0; I < (int)n_elems; ++I)
        {
            NewBuffer[I] = elems[I];
        }

        if (elems != InlineBuffer())
        {
            if (elems != nullptr)
            {
                free(elems);
            }
        }

        elems = NewBuffer;
    }
};

static_assert(sizeof(IVP_U_Vector<void>) == 0x8, "IVP_U_Vector size must be 0x8");

template <class T, int FIXED_SIZE>
class IVP_U_Vector_Fixed : public IVP_U_Vector<T>
{
public:
    T* Buffer[FIXED_SIZE]; // +0x08: Preallocated inline storage

    // NOTE (D76, 2026-09-09): must offset from a BASE-typed (IVP_U_Vector<T>*)
    // pointer, not from `this` directly -- `this` has the DERIVED type
    // IVP_U_Vector_Fixed<T,FIXED_SIZE>*, so `this + 1` would add
    // sizeof(IVP_U_Vector_Fixed<T,FIXED_SIZE>) (base + the whole Buffer
    // array) and land past the entire object instead of at Buffer[0]. Casting
    // to the 8-byte base first makes `+ 1` add exactly sizeof(IVP_U_Vector<T>),
    // matching baseline's own inline-buffer test (elems == this+1 evaluated
    // with `this` typed as the 8-byte vector header, confirmed against
    // 's Objects/Controllers cleanup, Common).
    // This template was previously never actually instantiated as a real
    // field anywhere in the port (only sizeof-checked below), so the bug was
    // latent; caught while wiring IVP_Core::Objects/Controllers to it.
    IVP_U_Vector_Fixed()
        : IVP_U_Vector<T>(Buffer, FIXED_SIZE)
    {
    }
};

static_assert(sizeof(IVP_U_Vector_Fixed<void, 8>) == 0x28, "IVP_U_Vector_Fixed<8> size must be 0x28");
