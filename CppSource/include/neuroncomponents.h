// Author: Giperion.

#pragma once

// Component registration and interface lookup.
// C exports are __cdecl. Interface methods are __stdcall (this on the stack).
// IID strings look like "1.11_ICOManager", not COM GUIDs.

#include <windows.h>

#include "neuroncomponents_iids.h"  // every "1.11_*" interface id, in one place

typedef long GENRESULT;

// Shared NEURONCOMPONENTS result codes; module-specific failures remain with their modules.
static const GENRESULT GR_OK = 0;
static const GENRESULT GR_GENERIC = (GENRESULT)0xFFFFFFFF;
static const GENRESULT GR_INVALID_PARMS = (GENRESULT)0xFFFFFFFE;
static const GENRESULT GR_INTERFACE_UNSUPPORTED = (GENRESULT)0xFFFFFFFD;
static const GENRESULT GR_OUT_OF_MEMORY = (GENRESULT)0xFFFFFFFC;

#define NEURONCOMPONENTS_API __stdcall

struct INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* iid, void** out) = 0;
    virtual unsigned long NEURONCOMPONENTS_API AddRef() = 0;
    virtual unsigned long NEURONCOMPONENTS_API Release() = 0;
};

// Aggregate initialization is defined by the concrete component.
struct IAggregateComponent : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API Initialize() = 0; // +0x0C
};

struct AGGDESC
{
    unsigned long size;
    const char* interface_name;
};

// Aggregated factory descriptor: OuterUnknown controls identity; InnerUnknown receives ownership.
struct FAGGDESC14
{
    unsigned long Size;
    const char* NameAt4;
    INeuronComponent* OuterUnknown;
    INeuronComponent** InnerUnknown;
    unsigned long Reserved10;
};

// ICOManager::EnumerateComponents callback. baseline 065b24e0 calls it cdecl as
// (factory, registration name, priority, context); a zero return stops the walk.
typedef int(__cdecl* FComponentEnumCallback)(INeuronComponent* Factory, const char* Name, void* Priority,
                                             void* Context);

// ICOManager singleton from NEURONCOMPONENTS_Acquire(), vtable 065b70cc (implementation notes).
struct ICOManager : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API CreateInstance(AGGDESC* desc, void** instance) = 0;          // +0x0C
    virtual GENRESULT NEURONCOMPONENTS_API RegisterComponent(INeuronComponent* factory, const char* name,
                                                  void* priority) = 0;                       // +0x10
    // , RET 0xC. A null name searches every registration.
    virtual GENRESULT NEURONCOMPONENTS_API UnregisterComponent(INeuronComponent* factory, const char* name) = 0; // +0x14
    // , RET 0x10. A null name walks every registration.
    virtual GENRESULT NEURONCOMPONENTS_API EnumerateComponents(const char* name, FComponentEnumCallback callback,
                                                    void* context) = 0;                     // +0x18
    virtual GENRESULT NEURONCOMPONENTS_API AddLibrary(const char* dll_name) = 0;                        // +0x1C
    virtual GENRESULT NEURONCOMPONENTS_API RemoveLibrary(const char* dll_name) = 0;                     // +0x20
    virtual GENRESULT NEURONCOMPONENTS_API Shutdown() = 0;                                              // +0x24
    virtual GENRESULT NEURONCOMPONENTS_API SetINIConfig(const char* ini_path, unsigned int flags) = 0;  // +0x28
};

struct IComponentFactory : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API CreateInstance(AGGDESC* desc, void** instance) = 0; // +0x0C
};

// "RegExp" component: the compiled program is owned; pattern and subject are borrowed.
struct IRegExp : INeuronComponent
{
    // +0x0C 065b5ac0: frees the previous program, regcomp(pattern); true when it compiled.
    virtual bool NEURONCOMPONENTS_API CompilePattern(const char* Pattern) = 0; // formerly Slot3
    // +0x10 065b5b00: remembers subject, returns regexec(program, subject) (0 when no program).
    virtual int NEURONCOMPONENTS_API Search(const char* Subject) = 0; // formerly Slot4
    // +0x14: copies to Buffer or object-owned scratch and appends NUL.
    virtual char* NEURONCOMPONENTS_API CopyCapture(int Index, char* Buffer, int Size, int Skip, int bNullOnFail) = 0; // formerly Slot5
    // +0x18 065b5be0: startp[index] - startp[0], or -1.
    virtual int NEURONCOMPONENTS_API GetCaptureOffsetFromMatch(int Index) = 0; // formerly Slot6
    // +0x1C 065b5c10: startp[index] - subject, or -1 (baseline reads startp here, not endp).
    virtual int NEURONCOMPONENTS_API GetCaptureOffsetFromSubject(int Index) = 0; // formerly Slot7
};

namespace NEURONCOMPONENTS_CRC
{
    // baseline neuroncomponents.dll — CRC32 over lowercased name; keys the routine / the routine tree.
    unsigned long __cdecl GetCRC32(const char* str);
    // (export ordinal 3, ?GetCRC32@NEURONCOMPONENTS_CRC@@YAKPBD0@Z). begin == end
    // returns 0; any other range never terminates (baseline has no exit test). No importer.
    unsigned long __cdecl GetCRC32(const char* begin, const char* end);
    // — continues a prior GetCRC32/GetContinuedCRC32 result over another
    // string (null str -> initial_crc unchanged; GetContinuedCRC32(0, s) == GetCRC32(s)).
    unsigned long __cdecl GetContinuedCRC32(unsigned long initial_crc, const char* str);
    // — single-character overload, one CRC table step over ch (case-folded).
    unsigned long __cdecl GetContinuedCRC32(unsigned long initial_crc, char ch);
    int __cdecl CompareStringsI(const char* a, const char* b);
}

namespace LogStream
{
    // ?LogEvent@LogStream@@YAXPBDMK@Z (neuroncomponents.dll export, neuroncomponents.def). OutpostFrontier.exe imports it
    // through module storage (sole caller 0x0052fcc5, "Timing_cu.collide.num_bullets").
    void __cdecl LogEvent(const char* Name, float Value, unsigned long Flags);
}

#ifdef NEURONCOMPONENTS_EXPORTS
#define NEURONCOMPONENTS_LINK __declspec(dllexport)
#else
#define NEURONCOMPONENTS_LINK __declspec(dllimport)
#endif

extern "C"
{
    ICOManager* NEURONCOMPONENTS_Acquire();
    GENRESULT __cdecl NEURONCOMPONENTS_GetDllVersion(const char* module, unsigned int* major,
                                          unsigned int* minor, unsigned int* build);
    GENRESULT __cdecl NEURONCOMPONENTS_GetVersion(unsigned int* major, unsigned int* minor,
                                       unsigned int* build);
    NEURONCOMPONENTS_LINK extern void(__cdecl* FDUMP)(int flags, const char* fmt, ...);
}
