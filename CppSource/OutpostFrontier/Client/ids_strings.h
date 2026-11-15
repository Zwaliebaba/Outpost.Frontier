// Author: Giperion.

#pragma once

#include <windows.h>

#include "common_text.h" // FmtStr, FmtStr::Val

// IDS lookup from resource DLLs (baseline the routine / the routine / the routine).
// High word of Ids selects the registered HMODULE; low word is the STRINGTABLE id.

// push the module (null included) onto the ids module list; returns the old
// element count << 16. Callers: WinMain 0x005b3127 (UiText.dll), the routine 0x005b0f7f
// (each [Resources] DLL).
unsigned int RegisterIdsResourceModule(HMODULE Module);
// without the write-back: the module the high word of Ids names, null when the
// index is past the list.
HMODULE GetIdsResourceModule(unsigned int Ids);

// Returns wchar count excluding NUL. Dest may be null to query length (baseline the routine).
unsigned int GetIdsStringW(unsigned int Ids, wchar_t* Dest, int DestChars);
unsigned int GetIdsStringA(unsigned int Ids, char* Dest, int DestChars);

// like GetIdsStringW, but when the id is missing from the module's
// STRINGTABLE it falls back to a raw UTF-16 RCDATA resource (type 0x17) with the same
// numeric id (baseline the routine), copied from its second word on. The fallback writes Dest
// without a null test, as baseline does.
unsigned int GetIdsStringWWithRawFallback(unsigned int Ids, wchar_t* Dest, int DestChars);

// (NN_StoryStar.cpp:0x2b3). Appends one ids-string (via
// GetIdsStringWWithRawFallback) at *pCursor, advancing it by the copied wchar count; FDUMPs a
// warning on a lookup miss. If the copy exactly fills the buffer up to pEnd, the last wchar is
// clobbered with a NUL so a fully-packed buffer still comes out terminated (matches baseline's
// pointer-rewind-and-zero tail).
void AppendIdsStringOrWarn(wchar_t** pCursor, wchar_t* pEnd, unsigned int Ids);

// (NN_StoryStar.cpp, cdecl) -- the first Val of Fmt whose TypeTag is Tag and
// whose Flags (the per-kind running index) is Index; null when there is none.
FmtStr::Val const* FmtStrFindValue(FmtStr const* Fmt, unsigned char Index, unsigned short Tag);

// (NN_StoryStar.cpp, cdecl) -- the story star's own mad-lib formatter (not
// MiscUtil's FmtStrSprintf): Fmt's template ids expanded against its Val list into Dest.
// "%[digits][v<digit>]<code>" escapes: ! B F G I L M R S Z d s; 'M' only with
// bIncludeMission. Returns the characters written, terminator excluded. An empty template
// leaves Dest untouched (baseline writes nothing then).
int FormatFmtStrMadLib(FmtStr const* Fmt, wchar_t* Dest, int DestChars, bool bIncludeMission);
