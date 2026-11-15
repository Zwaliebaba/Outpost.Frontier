// Author: Giperion.

#pragma once

#include "neuroncomponents.h"

// particlefx.dll public ABI. Source: particlefx.dll, implementation notes.
// Internal node implementations, particle pools and .ale chunks stay in src/particlefx/.
// The shared node-library contract is declared here and inherited by IParticleFx.

// These are declared at GLOBAL scope on purpose. NeuronRuntime exports ::IParticleFx* / ::IFxRuntime* /
// ::IFxEffectLibrary* (the mangled names baseline uses, see src/include/NeuronRuntime.h), while particlefx
// declared the same interfaces inside namespace ParticleFxImpl -- two unrelated types with one
// name, which is why consumers could only ever call them by raw slot number. particlefx_types.h
// now pulls these into ParticleFxImpl with using-declarations, so both sides mean one type.

struct IFxEffect;
struct IFileSystem;

// File / vtable rec16 is 4 dwords. AddRec16 stores [in2, id, in0, in1]; GetRec16 un-reorders.
// These are node IDs / edges, not factory names — no emitter/field objects from them.
struct FAleRec16
{
    unsigned long A;
    unsigned long B;
    unsigned long C;
    unsigned long D;
};

struct FAleRec8
{
    unsigned long A;
    unsigned long B;
};

struct IFxEffectArchetype : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API ResetRecords() = 0;                              // 3  +0x0C the branch RET 4
    virtual GENRESULT NEURONCOMPONENTS_API SetName(const char* Name) = 0;                   // 4  +0x10 the routine RET 8
    virtual const char* NEURONCOMPONENTS_API GetName() = 0;                                 // 5  +0x14 the branch RET 4
    virtual GENRESULT NEURONCOMPONENTS_API AddRec16(const FAleRec16* Rec, unsigned long* OutId,
                                         unsigned long Flag) = 0;                // 6  +0x18 the routine RET 0x10
    virtual GENRESULT NEURONCOMPONENTS_API RemoveRec16(unsigned long Id) = 0;               // 7  +0x1C the routine RET 8
    virtual GENRESULT NEURONCOMPONENTS_API Slot8(unsigned long Id, unsigned long Value) = 0; // 8  +0x20 the branch RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API AddRec8(const FAleRec8* Rec) = 0;                // 9  +0x24 the routine RET 8
    virtual GENRESULT NEURONCOMPONENTS_API RemoveRec8(const FAleRec8* Rec) = 0;             // 10 +0x28 the routine RET 8
    // 11/12: baseline the branch / the branch are thiscall (RET 0). No baseline DLL is loaded,
    // so the convention only has to agree between our caller and callee: NEURONCOMPONENTS_API like every
    // other interface method (AGENTS.md), as done for IMatAnimLibrary 3-6.
    virtual unsigned long NEURONCOMPONENTS_API GetRec16Count() = 0;               // 11 +0x2C the branch
    virtual unsigned long NEURONCOMPONENTS_API GetRec8Count() = 0;                // 12 +0x30 the branch
    virtual GENRESULT NEURONCOMPONENTS_API GetRec16(unsigned long Index, FAleRec16* Out) = 0; // 13 +0x34 the branch RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API GetRec8(unsigned long Index, FAleRec8* Out) = 0; // 14 +0x38 the branch RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API GetRec16ById(unsigned long Id, FAleRec16* Out) = 0; // 15 +0x3C the branch RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API FindRec8Index(const FAleRec8* Rec,
                                             unsigned long* OutIndex) = 0;      // 16 +0x40 the branch RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API RemapRec16FieldD(unsigned long OldId,
                                                 unsigned long NewId) = 0;      // 17 +0x44 the branch RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API GetHeader16(unsigned char* Out16) = 0;           // 18 +0x48 the branch RET 8
    virtual GENRESULT NEURONCOMPONENTS_API SetHeader16(const unsigned char* Header16) = 0;  // 19 +0x4C the routine RET 8
    virtual GENRESULT NEURONCOMPONENTS_API Slot20(void* Arg) = 0;                           // 20 +0x50 the branch RET 8
};

// Node-library contract shared by the standalone component and IParticleFx.
// The port keeps its existing COM prefix and method indices; all DLLs use this declaration.
using FNodeLibraryEnumCallback = int(__cdecl*)(INeuronComponent* Node, void* Context);

struct IFxNodeLibrary : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API LoadNodeLibrary(IFileSystem* FileSystem) = 0;   // 3  RET 8
    virtual GENRESULT NEURONCOMPONENTS_API SaveNodeLibrary(IFileSystem* FileSystem) = 0;   // 4  RET 8
    virtual unsigned long NEURONCOMPONENTS_API HashNodeName(const char* Name) = 0;         // 5  RET 8 → the routine
    virtual GENRESULT NEURONCOMPONENTS_API NodeLibSlot6(const char* Name, INeuronComponent* Node,
                                           unsigned long* OutHash) = 0;        // 6 RET 0x10
    virtual GENRESULT NEURONCOMPONENTS_API NodeLibSlot7(unsigned long Hash) = 0;          // 7 RET 8
    virtual GENRESULT NEURONCOMPONENTS_API NodeLibSlot8(FNodeLibraryEnumCallback Callback,
                                           void* Context) = 0;                // 8 RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API ClearNodeLibrary() = 0;                         // 9  RET 4
    virtual GENRESULT NEURONCOMPONENTS_API NodeLibSlot10(void* A, void* B) = 0;            // 10 RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API NodeLibSlot11(unsigned long Hash, INeuronComponent** Out) = 0; // 11 RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API NodeLibSlot12(unsigned long Hash, INeuronComponent** Out) = 0; // 12 RET 0xC
};

// IParticleFx retains the same slots 0–40; slots 3–12 are inherited unchanged.
struct IParticleFx : IFxNodeLibrary
{
    virtual GENRESULT NEURONCOMPONENTS_API CreateVertexBuffer() = 0;                       // 13 RET 4
    virtual GENRESULT NEURONCOMPONENTS_API ReleaseVertexBuffer() = 0;                      // 14 RET 4
    virtual GENRESULT NEURONCOMPONENTS_API Slot15(void* A, void* B) = 0;                   // 15 RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API Slot16(void* A, void* B) = 0;                   // 16 RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API DrawNodes(void* A, void* B) = 0;                // 17 RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API GetDrawCookie() = 0;                            // 18 RET 4
    virtual GENRESULT NEURONCOMPONENTS_API Slot19(const char* Name, void* Unused) = 0;    // 19 +0x4C RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API AttachNode(void* A, void* B) = 0;               // 20 RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API Slot21(void* A, void* B) = 0;                   // 21 RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API DetachNode(void* Node) = 0;                     // 22 RET 8
    virtual GENRESULT NEURONCOMPONENTS_API SubmitNode(void* Node) = 0;                     // 23 RET 8
    virtual GENRESULT NEURONCOMPONENTS_API SubmitChildNodes(void* Node) = 0;               // 24 RET 8
    virtual GENRESULT NEURONCOMPONENTS_API GetRootNode(void** Out) = 0;                    // 25 RET 8
    virtual GENRESULT NEURONCOMPONENTS_API FindNode(void* A, void* B, void* C) = 0;        // 26 RET 0x10
    virtual GENRESULT NEURONCOMPONENTS_API SetUpdateCallback(void* Fn) = 0;                // 27 RET 8
    virtual GENRESULT NEURONCOMPONENTS_API SetFilterCallback(void* Fn) = 0;                // 28 RET 8
    virtual GENRESULT NEURONCOMPONENTS_API BeginDraw() = 0;                                // 29 RET 4
    virtual GENRESULT NEURONCOMPONENTS_API EndDraw() = 0;                                  // 30 RET 4
    virtual GENRESULT NEURONCOMPONENTS_API SetUseMaterialBatcher(unsigned char Value) = 0; // 31 RET 8
    virtual GENRESULT NEURONCOMPONENTS_API GetUseMaterialBatcher() = 0;                    // 32 RET 4
    virtual GENRESULT NEURONCOMPONENTS_API EnqueueAppearance(void* A, void* B) = 0;        // 33 RET 0xC
    virtual void* NEURONCOMPONENTS_API GetVertexBufferHandle() = 0;                        // 34 RET 4
    virtual GENRESULT NEURONCOMPONENTS_API SetDrawFlags(void* Pair) = 0;                   // 35 RET 8
    virtual GENRESULT NEURONCOMPONENTS_API CopyCounters(void* Out9Dwords) = 0;             // 36 RET 8
    virtual GENRESULT NEURONCOMPONENTS_API Initialize(AGGDESC* Desc) = 0;                  // 37 +0x94
    virtual GENRESULT NEURONCOMPONENTS_API Slot38() = 0;                                   // 38 RET 4
    virtual GENRESULT NEURONCOMPONENTS_API GetFlagA() = 0;                                 // 39 RET 4
    virtual GENRESULT NEURONCOMPONENTS_API GetFlagB() = 0;                                 // 40 RET 4
};

// The mesh appearance's instance object. baseline's only implementation is OutpostFrontier.exe's
// FLFxMesh (RTTI `FLFxMesh : IFxMesh`, vtable 005d98ec); particlefx.dll consumes it. Slot names are
// not recovered -- each comment says what FLFxMesh does in that slot. Arity from RET imm16.
struct IFxMesh : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API Slot3(void* Transform) = 0; // +0x0C the branch RET 8: IEngine::SetInstanceMatrix
    virtual float* NEURONCOMPONENTS_API Slot4() = 0;                   // +0x10 the branch RET 4: IEngine::GetInstanceTransform
    virtual GENRESULT NEURONCOMPONENTS_API Slot5(void* Arg) = 0;       // +0x14 the branch RET 8: IEngine::UpdateInstance(inst, 0, Arg)
    virtual GENRESULT NEURONCOMPONENTS_API Slot6(void* Camera) = 0;    // +0x18 the routine RET 8: draw (ZWRITEENABLE 0, ZFUNC 4 LESSEQUAL)
    virtual GENRESULT NEURONCOMPONENTS_API Slot7() = 0;                // +0x1C the routine RET 4: drop material + instance
    virtual GENRESULT NEURONCOMPONENTS_API Slot8(void* Arg) = 0;       // +0x20 the branch RET 8: returns -1
};

// The factory particlefx.dll keeps in the field (IFxRuntime::BindMeshFactory) and calls through
// slot 3 at 0x06224d56 with (appearance +0xc0 -> +0x60, +0x74, a local position, &out).
// baseline's only implementation is OutpostFrontier.exe's FLFxMeshFactory (vtable 005d98bc). baseline
// has the scalar deleting destructor as slot 4 (+0x10, the routine), which Release calls; the
// port leaves it out of the interface (no virtual destructors on NEURONCOMPONENTS interfaces) and the
// implementation deletes itself.
struct IFxMeshFactory : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API Slot3(void* ArgA, void* ArgB, float const* Position,
                                      IFxMesh** Out) = 0; // +0x0C the routine RET 0x14
};

struct IFxRuntime : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API BindParticleFx(void* ParticleFx) = 0;            // +0x0C RET 8
    virtual GENRESULT NEURONCOMPONENTS_API BindUnknown(void* Value) = 0;              // +0x10 RET 8
    virtual GENRESULT NEURONCOMPONENTS_API BindEffectLibrary(void* Library) = 0;      // +0x14 RET 8
    virtual GENRESULT NEURONCOMPONENTS_API GetParticleFx(void** Out) = 0;                // +0x18 RET 8
    virtual GENRESULT NEURONCOMPONENTS_API GetUnknown(void** Out) = 0;                // +0x1C RET 8
    virtual GENRESULT NEURONCOMPONENTS_API GetEffectLibrary(void** Out) = 0;          // +0x20 RET 8
    virtual GENRESULT NEURONCOMPONENTS_API BindMeshFactory(void* Factory) = 0;        // +0x24 the branch the field
    virtual GENRESULT NEURONCOMPONENTS_API GetMeshFactory(void** Out) = 0;            // +0x28 the branch
};

using FEffectLibraryEnumCallback = int(__cdecl*)(IFxEffectArchetype* Archetype, void* Context);

struct IFxEffectLibrary : INeuronComponent
{
    // independent raw bindings the field and the field. RET 0xC.
    virtual GENRESULT NEURONCOMPONENTS_API BindParticleFx(IParticleFx* ParticleFx, IFxNodeLibrary* NodeLibrary) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API InsertArchetype(IFxEffectArchetype* Archetype,
                                                unsigned long* OutHash) = 0; // 4 the routine RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API RemoveByHash(unsigned long Hash) = 0;   // 5 the branch RET 8
    virtual GENRESULT NEURONCOMPONENTS_API CreateEffect(void* Unused, void* Key,
                                             IFxEffect** Out) = 0;      // RET 0x10
    virtual GENRESULT NEURONCOMPONENTS_API SaveAle(IFileSystem* FileSystem) = 0;   // RET 8
    virtual GENRESULT NEURONCOMPONENTS_API LoadAle(IFileSystem* FileSystem) = 0;   // RET 8
    virtual GENRESULT NEURONCOMPONENTS_API ClearEffects() = 0;                     // RET 4
    virtual GENRESULT NEURONCOMPONENTS_API EnumerateArchetypes(FEffectLibraryEnumCallback Callback,
                                                    void* Context) = 0; // 10 the routine RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API FindByHash(unsigned long Hash,
                                           IFxEffectArchetype** Out) = 0; // 11 the branch RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API MakeUniqueName(const char* Name, const char** Out) = 0; // 12 RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API RemapAllRec16FieldD(unsigned long OldId,
                                                    unsigned long NewId) = 0; // 13 the routine RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API Slot14(void* Unused) = 0;               // RET 8 → ClearEffects
};

struct IFxEffect : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API Slot3(void* A, void* B) = 0;           // RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API Slot4(void* A, void* B) = 0;           // JMP node+0x40 RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API Slot5(float Age) = 0;                  // RET 8: node +0x64
    virtual GENRESULT NEURONCOMPONENTS_API Slot6(float OuterTime) = 0;            // RET 8: node +0x5C
    virtual GENRESULT NEURONCOMPONENTS_API Slot7(void* Arg) = 0;                  // RET 8
    virtual GENRESULT NEURONCOMPONENTS_API Slot8(void* Arg) = 0;                  // RET 8
    virtual GENRESULT NEURONCOMPONENTS_API Slot9(void* Arg) = 0;                  // RET 8
    virtual GENRESULT NEURONCOMPONENTS_API Slot10(void* A, void* B) = 0;          // RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API Slot11(void* Arg) = 0;                 // RET 8
    virtual GENRESULT NEURONCOMPONENTS_API Slot12(void* A, void* B) = 0;          // RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API Slot13(void* Arg) = 0;                 // RET 8 (analysis thiscall)
    virtual GENRESULT NEURONCOMPONENTS_API Slot14(float LodInput) = 0;            // RET 8: node +0x88
    virtual GENRESULT NEURONCOMPONENTS_API Slot15(void* Arg) = 0;                 // RET 8
    virtual GENRESULT NEURONCOMPONENTS_API GetBoundComponent(void** Out) = 0;     // 16 the branch RET 8
    virtual GENRESULT NEURONCOMPONENTS_API Slot17() = 0;                          // dtor-with-flag
    virtual GENRESULT NEURONCOMPONENTS_API Initialize(AGGDESC* Desc) = 0;         // +0x48 the routine RET 8
};
