// Author: Giperion.

#pragma once

// Common.dll goods family -- helpers shared by several of the TUs split out of common_goods.cpp
// (formerly that file's anonymous namespace). Common-internal: nothing here is exported.

#include "common_misc.h"       // malloc_strcpy
#include "common_reputation.h" // Costume

namespace CmnGoods
{
    // Costume's inline operator=, as baseline expands it wherever a Costume is assigned
    // (CostumeHint::operator= module storage-063112fe, CharacterProperties::init
    // , operator= module storage-0632dbde, unmarshal
    // ): the four part ids, the count, then only `count` accessory
    // ids (a signed loop, JLE / JL). There is no bound: a count above 8 writes past
    // AccessoryIds exactly as baseline does; ids past the count keep their old values.
    inline void AssignCostume(Costume& Dest, Costume const& Src)
    {
        Dest.PartIds[0] = Src.PartIds[0];
        Dest.PartIds[1] = Src.PartIds[1];
        Dest.PartIds[2] = Src.PartIds[2];
        Dest.PartIds[3] = Src.PartIds[3];
        Dest.AccessoryCount = Src.AccessoryCount;
        for (int Index = 0; Index < (int)Dest.AccessoryCount; ++Index)
            Dest.AccessoryIds[Index] = Src.AccessoryIds[Index];
    }

    const unsigned kPathChars = 0x104;

    // Every room / setpoint / placement / hotspot / character-property string is written in
    // baseline with malloc_strcpy(&Field, Text) and released with malloc_strfree: the listing
    // ranges 06273300-0627b8e1 (room records), 06261330-06261d80 (RoomData copy / assign)
    // and 0632d890-0632e700 (CharacterProperties) contain no StringAlloc call at all. malloc_strcpy (06334400) is _strdup: a null source
    // gives null, "" gives a fresh "". It never frees the previous value.
    inline char const* MallocCopy(char const* Text)
    {
        char* Copy = nullptr;
        malloc_strcpy(&Copy, Text);
        return Copy;
    }
}
