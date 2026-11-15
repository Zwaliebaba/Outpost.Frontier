// Author: Giperion.

#pragma once

// TString<N> on its own so a header that only needs the fixed-size string (common_universe.h,
// for the SYSTEM object record) does not have to pull common_reputation.h and, through
// common_text.h, <windows.h>. common_reputation.h includes this header, so every existing user
// still sees the template where it saw it before.

#include <cstring>

// baseline TString<N>: length at +0, chars at +4. Mangle ?$TString@$0BA@ for N=16.
template <unsigned N>
struct TString
{
    unsigned Length;
    char Data[N];

    // OutpostFrontier.exe 0x004c9610 (inlined per-N template instantiation): compares Data against a
    // plain C string. baseline walks two bytes per loop iteration (an unrolled strcmp); this port
    // does the equivalent one byte at a time. A null Str matches only an empty TString.
    bool operator==(char const* Str) const
    {
        if (Str == nullptr)
            return Length == 0;
        unsigned i = 0;
        for (;; ++i)
        {
            if (Data[i] != Str[i])
                return false;
            if (Data[i] == '\0')
                return true;
        }
    }

    // Common.dll the routine (the N=64 instance, not exported; also inlined into
    // CommReferrable::operator= module storage, CommReferrable::read_comm_ini_data
    // and Server.dll pub::Translate{Good,Ship}ToMsgIdPrefix module storage/
    // ): a null Str empties the string; otherwise strlen, clamped to N-1
    // (`CMP EAX,N; JC; MOV EAX,N-1`), copied without the terminator, then Data[Len] = 0.
    // Common.dll the routine (`len = strlen; if (len >= N) len = N-1; copy; NUL`) is the same
    // operation spelled as a free function.
    TString& operator=(char const* Str)
    {
        if (Str == nullptr)
        {
            Length = 0;
            Data[0] = '\0';
            return *this;
        }
        unsigned Len = static_cast<unsigned>(strlen(Str));
        if (Len >= N)
            Len = N - 1;
        memcpy(Data, Str, Len);
        Data[Len] = '\0';
        Length = Len;
        return *this;
    }
};
