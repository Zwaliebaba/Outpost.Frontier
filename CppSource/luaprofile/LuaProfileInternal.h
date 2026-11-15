// Author: Giperion.

#pragma once

#include "luaprofile.h"

// luaprofile.dll internals. The ABI other modules see is src/include/luaprofile.h.

static const unsigned long kLuaObjectSize = 0x28;
static const unsigned long kAggDescSize = 0x14;
static const unsigned long kFactorySize = 0xC;
static const unsigned int kRegisterPriority = 0x40000000u;

static const char kComponentName[] = "ILua";
static const unsigned int kIidComponentFactoryBytes = 23;

static const char kInnerILua[] = "ILua";
static const char kInnerProfileParser[] = "IProfileParser";
static const char kInnerAggregate[] = "IAggregateComponent";

// AGGDESC size 0x14 overlay. Same offsets as neuroncomponents.h FAGGDESC14; +8 is outer
// IUnknown* (not an IID string). +0x10 Extra is not read. Header neuroncomponents.h unchanged.
struct FLuaAggDesc
{
    unsigned long Size;
    const char* Name;
    INeuronComponent* Outer;
    INeuronComponent** InnerOut;
    void* Extra;
};

static_assert(sizeof(FLuaAggDesc) == kAggDescSize, "baseline AGGDESC size 0x14");
static_assert(sizeof(Iids::ComponentFactory) == kIidComponentFactoryBytes, "cmpsb ECX=0x17");
static_assert(sizeof(kComponentName) == 5, "ILua + NUL");

struct FSectionHandle
{
    FSectionHandle* Next;
    int LuaRef;
};

static_assert(sizeof(FSectionHandle) == 8, "section handle extent");
