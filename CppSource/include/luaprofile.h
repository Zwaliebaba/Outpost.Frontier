// Author: Giperion.

#pragma once

#include "neuroncomponents.h"

// Lua values and the profile-parser interface shared with neuroncomponents's native INI parser.

// Tag 9 carries String; inactive union bytes are outside the source-level contract.
struct FLuaTaggedValue
{
    unsigned int Tag;
    unsigned int Unused;
    union
    {
        char* String;
        unsigned int UInt;
        int Int;
        float Float;
        double Number;
        __int64 LongLong;
    } Payload;
};

// ILua @ object+0. Slot 3 = the routine (call Lua global). RET 0x14.
struct ILua : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API InvokeGlobal(const char* Name, FLuaTaggedValue* OutValue,
                                             int Argc, FLuaTaggedValue* const* Argv) = 0;
};

typedef int(NEURONCOMPONENTS_API* FSectionEnumCallback)(void* Parser, const char* Name, void* Context);

// LoadFile and EnumerateSections apply to both INI sections and Lua global tables.
struct IProfileParser : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API LoadFile(const char* Path, unsigned int Flags) = 0; // formerly UnnamedSlot3
    virtual int NEURONCOMPONENTS_API EnumerateSections(FSectionEnumCallback Callback, void* Context) = 0; // formerly UnnamedSlot4
    virtual void* NEURONCOMPONENTS_API OpenSection(const char* Name, unsigned int Flags) = 0;      // RET 0xC
    virtual int NEURONCOMPONENTS_API CloseSection(void* Handle) = 0;                               // RET 8
    virtual int NEURONCOMPONENTS_API ReadLine(void* Handle, int Index, char* Buffer,
                                   unsigned int BufferSize) = 0; // RET 0x14
    virtual int NEURONCOMPONENTS_API GetString(void* Handle, const char* Key, char* Buffer,
                                    unsigned int BufferSize) = 0; // RET 0x14
};

// The manager supplies its aggregated native INI parser.
inline IProfileParser* AcquireProfileParser()
{
    ICOManager* NeuronComponents = NEURONCOMPONENTS_Acquire();
    if (NeuronComponents == nullptr)
        return nullptr;
    IProfileParser* Parser = nullptr;
    if (NeuronComponents->QueryInterface(Iids::ProfileParser, (void**)&Parser) != GR_OK || Parser == nullptr)
        return nullptr;
    return Parser;
}
