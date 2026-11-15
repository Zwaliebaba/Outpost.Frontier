// Author: Giperion.

// Common.dll -- ScriptBehavior (the marshalled script-behavior description).
// CMN-SPLIT stage 1 (2026-10-14): split out of common_iface.cpp unchanged.

#include "common_iface.h"
#include "common_misc.h" // malloc_strcpy / malloc_strfree -- ScriptBehavior string slots

#include <cstring>

// ============================================================================
// ScriptBehavior
// ============================================================================

// The string slots (StringArray[i] / StringArray2[i]) own _strdup / MarshalBuffer::get_string
// heap copies that malloc_strfree releases. Only the first IdArrayCount entries of StringArray
// are ever freed (destroy, dtor, operator=, init all loop `i < [this+0x118]`), while all 32
// StringArray2 entries are freed unconditionally. Neither count is clamped to 32 anywhere in
// baseline: a count above 32 indexes past the arrays exactly as baseline does (cheap literal UB,
// owner ruling 2026-10-08); every producer (init from a description, unmarshal of a
// server-built buffer) keeps them <= 32.

// . The two FmtStr members are zero-constructed inline (module storage-0632dd8d,
// the port's inline FmtStr() zeroes the same 0x16 bytes each), then clear() -- which only
// zeroes, never frees, so the uninitialised string slots are never touched.
ScriptBehavior::ScriptBehavior()
{
    clear();
}

// . A plain memberwise copy: both pointer arrays are block-copied with REP MOVSD
// (module storage / 0632d7cd / 0632d7ec / 0632d7ff), so the copy SHARES the source's strings;
// only the two FmtStr members are deep-copied through FmtStr's copy ctor (module storage /
// ). Destroying both the copy and the source would free the shared strings twice --
// baseline's own hazard; no baseline consumer calls this overload (Server.dll and OutpostFrontier.exe
// import only the default ctor, ordinal 417, at 06d12424 / 06d18020 / 00437763 / 0043c8f8).
ScriptBehavior::ScriptBehavior(ScriptBehavior const& Other)
    : NameFmtStr(Other.NameFmtStr)
    , DescFmtStr(Other.DescFmtStr)
{
    BehaviorId = Other.BehaviorId;
    Field04 = Other.Field04;
    Flag08 = Other.Flag08;
    Flag09 = Other.Flag09;
    Flag0a = Other.Flag0a;
    Field0c = Other.Field0c;
    Field10 = Other.Field10;
    Flag14 = Other.Flag14;
    for (int I = 0; I < 32; ++I)
    {
        IdArray[I] = Other.IdArray[I];
    }
    for (int I = 0; I < 32; ++I)
    {
        StringArray[I] = Other.StringArray[I];
    }
    IdArrayCount = Other.IdArrayCount;
    for (int I = 0; I < 32; ++I)
    {
        StringArray2[I] = Other.StringArray2[I];
    }
    for (int I = 0; I < 32; ++I)
    {
        StringArray2Dword[I] = Other.StringArray2Dword[I];
    }
    StringArray2Count = Other.StringArray2Count;
    Field220 = Other.Field220;
    Field224 = Other.Field224;
    Field228 = Other.Field228;
    Field25c = Other.Field25c;
}

// . The body is destroy() inlined (free StringArray[i] for i < IdArrayCount,
// free all 32 StringArray2[i], FmtStr::destroy on +0x22c then +0x244); the member
// ~FmtStr calls that follow (+0x244 at module storage, then +0x22c at module storage) are the
// compiler-generated member destructors here as well.
ScriptBehavior::~ScriptBehavior()
{
    destroy();
}

// . No self-assignment test: baseline releases its own strings and FmtStr first
// (module storage-0632e3d2, destroy() inlined) and then copies from Other.
ScriptBehavior& ScriptBehavior::operator=(ScriptBehavior const& Other)
{
    destroy();
    BehaviorId = Other.BehaviorId;
    Field04 = Other.Field04;
    Flag08 = Other.Flag08;
    Flag09 = Other.Flag09;
    Flag0a = Other.Flag0a;
    Field0c = Other.Field0c;
    Field10 = Other.Field10;
    Flag14 = Other.Flag14;

    IdArrayCount = Other.IdArrayCount;
    for (int I = 0; I < IdArrayCount; ++I)
    {
        IdArray[I] = Other.IdArray[I];
        malloc_strcpy(&StringArray[I], Other.StringArray[I]);
    }

    StringArray2Count = Other.StringArray2Count;
    for (int I = 0; I < StringArray2Count; ++I)
    {
        malloc_strcpy(&StringArray2[I], Other.StringArray2[I]);
        StringArray2Dword[I] = Other.StringArray2Dword[I];
    }

    Field220 = Other.Field220;
    Field224 = Other.Field224;
    Field228 = Other.Field228;
    NameFmtStr = Other.NameFmtStr;
    DescFmtStr = Other.DescFmtStr;
    Field25c = Other.Field25c;
    return *this;
}

// . Resets every field WITHOUT releasing anything: the string slots are only
// overwritten with 0 (module storage / 0632dec0 `MOV [EAX-0x80],EBX; MOV [EAX],EBX`), so a
// behaviour that still owns strings leaks them -- baseline's contract, and what makes clear()
// safe to run from the default ctor on uninitialised slots. The FmtStr members are destroyed
// and then their 0x18 bytes zeroed (REP STOSD x6, module storage / 0632df0d).
void ScriptBehavior::clear()
{
    Flag08 = 1;
    Flag0a = 1;
    Flag14 = 1;
    BehaviorId = -1;
    Field04 = 0;
    Flag09 = 0;
    Field0c = 0;
    Field10 = 0;
    for (int I = 0; I < 32; ++I)
    {
        IdArray[I] = 0;
        StringArray[I] = nullptr;
    }
    IdArrayCount = 0;
    for (int I = 0; I < 32; ++I)
    {
        StringArray2[I] = nullptr;
        StringArray2Dword[I] = 0;
    }
    StringArray2Count = 0;
    Field220 = 0;
    Field224 = 0;
    Field228 = 0;
    NameFmtStr.destroy();
    memset(&NameFmtStr, 0, sizeof(NameFmtStr));
    DescFmtStr.destroy();
    memset(&DescFmtStr, 0, sizeof(DescFmtStr));
    Field25c = 0;
}

// . Releases the owned strings and the FmtStr contents; counts and the other
// fields are left as they are.
void ScriptBehavior::destroy()
{
    for (int I = 0; I < IdArrayCount; ++I)
    {
        malloc_strfree(&StringArray[I]);
    }
    for (int I = 0; I < 32; ++I)
    {
        malloc_strfree(&StringArray2[I]);
    }
    NameFmtStr.destroy();
    DescFmtStr.destroy();                                               // (tail JMP)
}

unsigned long ScriptBehavior::get_key() const
{
    // (export 2848, ICF-folded with Rect::leftmost, BaseData::get_base_id and
    // a dozen other getters): `MOV EAX,dword ptr [this]; RET` -- the dword at +0x00, which is
    // BehaviorId. The previous body returned Field04 (+0x04).
    return static_cast<unsigned long>(BehaviorId);
}

// . Starts with destroy() inlined (module storage-0632e590: free StringArray[i]
// for i < the OLD IdArrayCount, free all 32 StringArray2[i], FmtStr::destroy x2) -- no clear(),
// no zeroing -- then copies the description. Strings go through malloc_strcpy, which stores
// a null slot for a null source.
void ScriptBehavior::init(CharacterBehaviorDescription const& Desc)
{
    destroy();
    BehaviorId = -1;
    Field04 = static_cast<unsigned>(Desc.Field00);
    Flag08 = Desc.Field04;
    Flag09 = Desc.Field05;
    Flag0a = Desc.Field06;
    Field0c = Desc.Field08;
    Field10 = Desc.Field0c;
    Flag14 = Desc.Field10;

    IdArrayCount = Desc.IdArrayCount;
    for (int I = 0; I < IdArrayCount; ++I)
    {
        IdArray[I] = Desc.IdArray[I];
        malloc_strcpy(&StringArray[I], Desc.StringArray[I]);
    }

    StringArray2Count = Desc.StringArray2Count;
    for (int I = 0; I < StringArray2Count; ++I)
    {
        malloc_strcpy(&StringArray2[I], Desc.StringArray2[I]);
        StringArray2Dword[I] = Desc.StringArray2Dword[I];
    }

    Field220 = Desc.Field21c;
    Field224 = Desc.Field220;
    Field228 = Desc.Field224;
    NameFmtStr = Desc.NameFmtStr;
    DescFmtStr = Desc.DescFmtStr;
    Field25c = Desc.Field258;
}

// . Field order on the wire as baseline; the bools go through put_bool, which
// normalises to 0/1 itself (module storage SETNZ).
void ScriptBehavior::marshal(MarshalBuffer* Buf) const
{
    Buf->put_void(&BehaviorId, 4);
    Buf->put_void(&Field04, 4);
    Buf->put_bool(Flag08 != 0);
    Buf->put_bool(Flag09 != 0);
    Buf->put_bool(Flag0a != 0);
    Buf->put_void(&Field0c, 4);
    Buf->put_void(&Field10, 4);
    Buf->put_bool(Flag14 != 0);
    Buf->put_void(&IdArrayCount, 4);
    for (int I = 0; I < IdArrayCount; ++I)
    {
        Buf->put_void(&IdArray[I], 4);
        Buf->put_string(StringArray[I]);
    }
    Buf->put_void(&StringArray2Count, 4);
    for (int I = 0; I < StringArray2Count; ++I)
    {
        Buf->put_string(StringArray2[I]);
        Buf->put_void(&StringArray2Dword[I], 4);
    }
    Buf->put_void(&Field220, 4);
    Buf->put_void(&Field224, 4);
    Buf->put_void(&Field228, 4);
    Buf->put_FmtStr(NameFmtStr);
    Buf->put_FmtStr(DescFmtStr);
    Buf->put_void(&Field25c, 4);
}

// . No clear(), no clamp: every field is overwritten in place (string slots
// that still owned a string leak, as in baseline). Each dword is read through ONE scratch
// slot -- baseline reuses its own `param_1` stack cell (module storage ... 0632e2b1) -- and then
// stored, so when MarshalBuffer::get_void has too few bytes left and writes nothing, the field
// receives the scratch slot's previous content (initially the MarshalBuffer pointer itself).
// The trailing +0x25c dword goes through a different, never-initialised stack cell (local_28,
// ); the port starts that one at 0 -- the only point where it differs from
// baseline, and only on a truncated buffer.
void ScriptBehavior::unmarshal(MarshalBuffer* Buf)
{
    unsigned int Scratch = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(Buf));

    Buf->get_void(&Scratch, 4);
    BehaviorId = static_cast<int>(Scratch);
    Buf->get_void(&Scratch, 4);
    Field04 = Scratch;
    Flag08 = Buf->get_bool();
    Flag09 = Buf->get_bool();
    Flag0a = Buf->get_bool();
    Buf->get_void(&Scratch, 4);
    Field0c = Scratch;
    Buf->get_void(&Scratch, 4);
    Field10 = Scratch;
    Flag14 = Buf->get_bool();
    Buf->get_void(&Scratch, 4);
    IdArrayCount = static_cast<int>(Scratch);
    for (int I = 0; I < IdArrayCount; ++I)
    {
        Buf->get_void(&Scratch, 4);
        IdArray[I] = Scratch;
        StringArray[I] = Buf->get_string();
    }
    Buf->get_void(&Scratch, 4);
    StringArray2Count = static_cast<int>(Scratch);
    for (int I = 0; I < StringArray2Count; ++I)
    {
        StringArray2[I] = Buf->get_string();
        Buf->get_void(&Scratch, 4);
        StringArray2Dword[I] = Scratch;
    }
    Buf->get_void(&Scratch, 4);
    Field220 = Scratch;
    Buf->get_void(&Scratch, 4);
    Field224 = Scratch;
    Buf->get_void(&Scratch, 4);
    Field228 = Scratch;
    NameFmtStr = Buf->get_FmtStr();                                     // / 0632e300
    DescFmtStr = Buf->get_FmtStr();                                     // / 0632e330
    unsigned int TrailingScratch = 0;
    Buf->get_void(&TrailingScratch, 4);
    Field25c = TrailingScratch;
}
