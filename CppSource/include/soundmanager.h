// Author: Giperion.

#pragma once

#include "neuroncomponents.h"
#include "x86math.h"            // FVector -- SoundManager's FVec3 was a third copy of it
#include "audio/dsound8.h"

// soundmanager.dll public ABI. Source: soundmanager.dll, implementation notes.
// Object size 0x14C; IIDs are NEURONCOMPONENTS strings, not COM GUIDs. Everything private to the port
// (the DirectSound GUIDs, the instance/attached node layouts, the factory) stays in
// src/SoundManager/SoundManagerInternal.h.

// NeuronRuntime holds the live manager in NeuronRuntime::Sound and OutpostFrontier drives it from
// menu_sound_arch.cpp; until 2026-09-16 both did it by raw slot number
// (implementation notes).

// The listener/source vectors this interface takes are plain 3-float records -- x86math.h's
// FVector, which SoundManager and SceneScript each used to redeclare as FVec3.
typedef FVector FVec3;

struct ISoundManagerStatus : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API GetInstanceStatus(int Index, void* Out) = 0; // +0x0C
};

struct ISoundArchetype : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API GetFormat(void* Out) = 0;          // +0x0C the branch
    virtual GENRESULT NEURONCOMPONENTS_API GetSamples(void* Out) = 0;         // +0x10 stub FDUMP
    virtual unsigned long NEURONCOMPONENTS_API GetNumFrames() = 0;            // +0x14
    virtual float NEURONCOMPONENTS_API GetGain() = 0;                         // +0x18
    virtual unsigned long NEURONCOMPONENTS_API GetChannelCount() = 0;         // +0x1C
    virtual unsigned long NEURONCOMPONENTS_API GetDurationMs() = 0;           // +0x20
    virtual GENRESULT NEURONCOMPONENTS_API SetSamples(void* A, void* B) = 0;  // +0x24 stub FDUMP
    virtual GENRESULT NEURONCOMPONENTS_API SetGain(float Gain) = 0;           // +0x28
    virtual unsigned long NEURONCOMPONENTS_API IsReady() = 0;                 // +0x2C AL=1
    virtual GENRESULT NEURONCOMPONENTS_API GetLoopRange(unsigned long* Start,
                                             unsigned long* End) = 0; // +0x30
};

// ISoundSource -- the "play key". SoundManager never creates one: OutpostFrontier.exe hands it in
// through AttachPlayKey and the mixer then calls back into it every tick to ask where the
// sound is, how loud, how far, whether it is still alive. The port had no declaration for it
// at all -- SoundManager reached it through 44 hand-written `reinterpret_cast<Fn>(Vtable[N])`
// wrappers, and menu_sound_arch.cpp answered with a hand-built `static void* [42]` table that
// it memcpy'd onto two more tables to fake derived classes.

// Slot widths below are read off baseline's own Source3D vtable, the branch in
// OutpostFrontier.exe. All three source vtables -- the branch (Sound::Source),
// (Sound::StreamedSource) and the branch (Sound::Source3D) -- are
// exactly 0xAC bytes, i.e. 43 entries; the port's hand-built table had 42 and was one short.

// THE CALLING CONVENTION IS MIXED, exactly like IMaterial, and the split is clean: slots
// 3..21 read `this` off the stack (NEURONCOMPONENTS_API); slots 22..42 take it in ECX (__thiscall).
// Verified per slot against the listing -- 0x00429910 (slot 5) opens `MOV ECX,[ESP+4]`, while
// (slot 22) opens `PUSH ESI; MOV ESI,ECX` and returns `RET 0x8` for two arguments,
// and slot 39 at 0x00428810 is `MOV EAX,[ECX+0x60]; RET` -- a bare RET, which no explicit-this
// reading allows. Everything from slot 22 down is called only from inside OutpostFrontier.exe;
// SoundManager only ever touches 3..21, which is why the port got away with declaring the
// whole tail NEURONCOMPONENTS_API until 2026-09-16.

// THE BASE CLASS DOES NOT IMPLEMENT THE 3D SLOTS. In Sound::Source and Sound::StreamedSource
// slots 12 and 15..18 (position, cone angles, cone orientation, cone outside volume,
// velocity) are the shared "unsupported" stubs -- the branch (`MOV EAX,0xFFFFFFF9; RET 0xC`)
// for the three-dword slot 15, the branch (`MOV EAX,0xFFFFFFF9; RET 0x8`) for the rest.
// Only Sound::Source3D has real bodies. The mixer guards every one of those calls with
// Is3d(), so this is not a behavioural difference, but the port now mirrors it: FSoundSource
// answers "unsupported" and FSpatialSource overrides.
struct ISoundSource : INeuronComponent
{
    // 3 +0x0C RET 0x4 -- the routine. Refresh pass slot 4 runs first: on flag 0x4 without
    // 0x10 it asks NeuronRuntime::Sound for the tick (slot 44) and restamps this+0x14. The mixer
    // never calls this one directly; it is slot 4's own first instruction.
    virtual unsigned long NEURONCOMPONENTS_API Slot3() = 0;
    // 4 +0x10 RET 0x4 -- the routine: unless flag 0x10 is set, call slot 3, then return
    // this+0x14. That field is the tick the sound started on.
    virtual unsigned long NEURONCOMPONENTS_API GetStartTick() = 0;
    // 5 +0x14 RET 0x4 -- the routine: `this+0x2C != 0`.
    virtual unsigned char NEURONCOMPONENTS_API IsActive() = 0;
    // 6 +0x18 RET 0x4 -- the branch. True only for the spatial (3D) source class; baseline
    // answers it by which vtable the object carries, not by a flag.
    virtual unsigned char NEURONCOMPONENTS_API Is3d() = 0;
    // 7 +0x1C RET 0x4 -- the branch: bit 0 of the flag byte at this+0x0C.
    virtual unsigned char NEURONCOMPONENTS_API IsLooping() = 0;
    // 8 +0x20 -- the branch is a three-instruction thunk with no RET of its own:
    // `MOV ECX,[ECX+0x18]; MOV EAX,[ECX]; JMP [EAX]` -- forward to the archetype at +0x18,
    // slot 0. Arity is therefore whatever the caller pushes, and every caller pushes nothing
    // but the object.
    virtual int NEURONCOMPONENTS_API GetArchId() = 0;
    // 9 +0x24 RET 0x8 -- the routine, the per-source attenuation in dB.
    virtual GENRESULT NEURONCOMPONENTS_API GetAttenuation(float* Out) = 0;
    // 10 +0x28 RET 0x8 -- the routine.
    virtual GENRESULT NEURONCOMPONENTS_API GetPitch(float* Out) = 0;
    // 11 +0x2C RET 0x8 -- the branch: `ftol(this+0x20)` into the out-param. The mixer
    // clamps it to +/-10000 and hands it to IDirectSoundBuffer::SetPan, so it is a pan.
    virtual GENRESULT NEURONCOMPONENTS_API GetPan(long* Out) = 0;
    // 12 +0x30 RET 0x8 -- the branch: world position, resolved through NeuronRuntime::Engine when
    // the source is attached (this+0x60 != -1).
    virtual GENRESULT NEURONCOMPONENTS_API GetPosition(FVec3* Out) = 0;
    // 13 +0x34 RET 0x8 -- the branch. The mixer defaults it to 10.0f before asking.
    virtual GENRESULT NEURONCOMPONENTS_API GetMinDistance(float* Out) = 0;
    // 14 +0x38 RET 0x8 -- the routine. Default 500.0f; squared into the mixer's range cull.
    virtual GENRESULT NEURONCOMPONENTS_API GetMaxDistance(float* Out) = 0;
    // 15 +0x3C RET 0xC -- the routine, the two DS3D cone angles.
    virtual GENRESULT NEURONCOMPONENTS_API GetConeAngles(unsigned long* Inside,
                                              unsigned long* Outside) = 0;
    // 16 +0x40 RET 0x8 -- the branch.
    virtual GENRESULT NEURONCOMPONENTS_API GetConeOrientation(FVec3* Out) = 0;
    // 17 +0x44 RET 0x8 -- the branch.
    virtual GENRESULT NEURONCOMPONENTS_API GetConeOutsideVolume(float* Out) = 0;
    // 18 +0x48 RET 0x8 -- the branch.
    virtual GENRESULT NEURONCOMPONENTS_API GetVelocity(FVec3* Out) = 0;
    // 19 +0x4C RET 0x8 -- the mixer forwards the answer straight into EAX 2.0 listener
    // property 1 via IKsPropertySet, so it is an environment id.
    virtual GENRESULT NEURONCOMPONENTS_API GetEaxEnvironment(unsigned long* Out) = 0;
    // 20 +0x50 RET 0x8 -- 0 => DS3DMODE_NORMAL, 1 => DS3DMODE_HEADRELATIVE, anything else is
    // a warning and NORMAL.
    virtual GENRESULT NEURONCOMPONENTS_API GetSoundMode(unsigned long* Out) = 0;
    // 21 +0x54 RET 0x8 -- the SoundManager mixer (the routine) maps 0 => dwApply 1
    // (DS3D_DEFERRED, committed once per tick by CommitDeferredSettings at 06ee524e) and
    // 1 => dwApply 0 (DS3D_IMMEDIATE); anything else is a warning and 1.
    virtual GENRESULT NEURONCOMPONENTS_API GetApplyMode(unsigned long* Out) = 0;
    // Slots 19/20/21 all share one baseline implementation, the branch:
    // `MOV EAX,0xFFFFFFF9; RET 0x8` -- "unsupported", one argument. The port used to install a
    // zero-argument stub here, which is what made these three a live stack bug (see below).

    // 22..25 +0x58..+0x64: start / alternate start / stop / offset. All __thiscall.
    // 22 the routine RET 0x8 -- `PUSH ESI; MOV ESI,ECX`, two stack arguments.
    virtual unsigned char __thiscall Start(void* Arch, int Extra) = 0;
    // 23 the routine RET 0x4 -- `XOR AL,AL; RET 0x4`: one argument, always fails.
    virtual unsigned char __thiscall AltStart(void* Arch) = 0;
    // 24 the routine bare RET -- NO arguments. The port declared `Stop(int)` and threw the
    // argument away, which is four bytes wider than baseline.
    virtual void __thiscall Stop() = 0;
    // 25 the routine RET 0x4 -- stores Offset, or clock - Offset when flag 0x10 is set.
    virtual void __thiscall SetStartOffset(int Offset) = 0;

    // 26..35 +0x68..+0x8C: all __thiscall; unnamed entries retain listing-derived shapes.
    // 26 the branch: stores the pause tick, then stops the instance buffer.
    virtual void __thiscall Pause() = 0;
    // 27 the branch: adjusts a latched start tick, plays the buffer, then clears the pause tick.
    virtual void __thiscall Resume() = 0;
    // 28 the branch: pause timestamp != 0; pausing at clock zero still reports false.
    virtual unsigned long __thiscall IsPaused() = 0;
    // 29 the routine bare RET, 0 args. an earlier note read this one as
    // *is_finished*: it returns false when the looping bit at +0x0C is set, which squares with
    // SoundManager handing looping buffers DSBPLAY_LOOPING. Slot 42 calls it.
    virtual unsigned char __thiscall IsFinished() = 0;
    // 30 the branch bare RET, 0 args -- sets up an SEH frame, so it owns a C++ object.
    virtual unsigned long __thiscall Slot30() = 0;
    // 31 the branch RET 0x4: write mixer score, preserving x87 rounding and unordered branches.
    virtual void __thiscall ComputeMixerScore(const float* Listener) = 0;
    // 32 the branch: clamps source attenuation to [-96, 0] dB, preserving NaN bits.
    virtual void __thiscall SetAttenuation(float Attenuation) = 0;
    // 33 the branch RET 0x4 -- clamps pan to [-10000, 10000]; NaN is stored unchanged.
    virtual void __thiscall SetPan(float Pan) = 0;
    // 34 the routine: replaces flag bit 0 with the argument's low bit.
    virtual void __thiscall SetLooping(unsigned char bLooping) = 0;
    // 35 the branch RET 0x4, one argument. In the two non-3D vtables this slot is folded
    // onto IDPMsgHandler::OnAddHost (`RET 0x4`), an unrelated do-nothing of the same shape.
    virtual unsigned long __thiscall Slot35(void* Arg) = 0;

    // 36..39: __thiscall, object in ECX -- see the header note above.
    // 36 +0x90 RET 0x4 -- the branch copies three floats into this+0x48.
    virtual void __thiscall SetVelocity(const float* Velocity) = 0;
    // 37 +0x94 RET 0x8 -- the routine: this+0x60 = handle, then three floats from Pose+0x24
    // into this+0x30.
    virtual void __thiscall SetPose(int AttachHandle, const float* Transform) = 0;
    // 38 +0x98 RET 0x4 -- the branch clamps the pitch into +/-100.0f at this+0x10.
    virtual void __thiscall SetPitch(float Pitch) = 0;
    // 39 +0x9C bare RET -- the branch is `MOV EAX,[ECX+0x60]; RET`. Zero arguments, and the
    // bare RET is the proof: an explicit this would have to pop 4.
    virtual int __thiscall GetAttachHandle() = 0;

    // 40 +0xA0 the routine -- `XOR AL,AL; RET`. Bare RET, no arguments, always false; the
    // same folded stub sits in 20 other vtables.
    virtual unsigned char __thiscall Slot40() = 0;
    // 41 +0xA4 the routine RET 0x4 -- MSVC's scalar deleting destructor (the base class uses
    // its own, the routine, which reinstalls the branch first).
    virtual void* __thiscall ScalarDeletingDestructor(unsigned char bDelete) = 0;
    // 42 +0xA8 RET 0x8 -- two arguments, and the second is a float. Source3D's the branch
    // integrates the velocity triple at +0x48 into the position at +0x30 by that delta, then
    // calls its own slot 31 with the first argument and slot 29 (IsFinished), returning the
    // logical NOT of that. The base class body the routine is the same minus the integration.
    // The port's hand-built table stopped at 41 and never had this slot at all.
    virtual unsigned char __thiscall Update(const float* Listener, float DeltaSeconds) = 0;
};

// ISoundListenerPose -- the camera pose ISoundManager::MixerTick takes. the routine copies
// orientation, position, velocity and the three DirectSound scale factors out of it into the
// manager's own listener state. Every getter answers >= 0 when it has a value, and the mixer
// keeps its current setting otherwise.

// NOT IMPLEMENTED ANYWHERE IN THE PORT YET: both MixerTick call sites in
// menu_sound_arch.cpp pass nullptr, so the listener never moves. Declaring the interface is
// what makes that gap visible instead of hiding it behind a void*.
struct ISoundListenerPose : INeuronComponent
{
    virtual int NEURONCOMPONENTS_API GetOrientation(FVec3* OutFront, FVec3* OutUp) = 0; // +0x0C
    virtual int NEURONCOMPONENTS_API GetPosition(FVec3* Out) = 0;                       // +0x10
    virtual int NEURONCOMPONENTS_API GetVelocity(FVec3* Out) = 0;                       // +0x14
    virtual int NEURONCOMPONENTS_API GetDistanceFactor(float* Out) = 0;                 // +0x18
    virtual int NEURONCOMPONENTS_API GetDopplerFactor(float* Out) = 0;                  // +0x1C
    virtual int NEURONCOMPONENTS_API GetRolloffFactor(float* Out) = 0;                  // +0x20
};

// 56 stdcall slots unless noted. Unnamed slots keep documented RET sizes; do not invent names.
struct ISoundManager : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API SetPcmFormat(unsigned short* Fmt) = 0;                 // 3  +0x0C
    virtual GENRESULT NEURONCOMPONENTS_API GetPcmFormat(unsigned short* Fmt) = 0;                 // 4
    virtual GENRESULT NEURONCOMPONENTS_API SetMasterGain(float Gain) = 0;                         // 5
    virtual GENRESULT NEURONCOMPONENTS_API SetMaxChannels(unsigned long Count) = 0;               // 6
    virtual GENRESULT NEURONCOMPONENTS_API SetSpeakerConfig(unsigned long Index) = 0;             // 7
    virtual float NEURONCOMPONENTS_API GetMasterGain() = 0;                                       // 8
    virtual unsigned long NEURONCOMPONENTS_API GetMaxChannels() = 0;                              // 9
    virtual unsigned long NEURONCOMPONENTS_API GetSpeakerConfig() = 0;                            // 10
    virtual GENRESULT NEURONCOMPONENTS_API SetEaxPreview(int Env, float Vol, float Decay,
                                              float Damp) = 0;                         // 11
    virtual GENRESULT NEURONCOMPONENTS_API GetEaxPreview(unsigned long* Env, unsigned long* Vol,
                                              unsigned long* Decay,
                                              unsigned long* Damp) = 0;                // 12
    virtual GENRESULT NEURONCOMPONENTS_API SetOrientation(FVector* Front, FVector* Up) = 0;           // 13
    // 14: Front = column 2 (m[2], m[5], m[8]), Up = column 1 (m[1], m[4], m[7]) - 06ee2380.
    virtual GENRESULT NEURONCOMPONENTS_API SetOrientationFromMatrix(float* Matrix) = 0;           // 14
    virtual GENRESULT NEURONCOMPONENTS_API SetListenerPosition(FVector* Pos) = 0;                   // 15
    virtual GENRESULT NEURONCOMPONENTS_API SetListenerVelocity(FVector* Vel) = 0;                   // 16
    virtual GENRESULT NEURONCOMPONENTS_API SetDistanceFactor(float Value) = 0;                    // 17
    virtual GENRESULT NEURONCOMPONENTS_API SetDopplerFactor(float Value) = 0;                     // 18
    virtual GENRESULT NEURONCOMPONENTS_API SetRolloffFactor(float Value) = 0;                     // 19
    // 20..23 return no status: baseline leaves the (last) out pointer in EAX (06ee2730, 25f0, 27c0,
    // 2810). Slot 20 writes (front, up) as slot 13 took them (stored +0xA8 negated back); slot 21
    // writes columns (right, up, front), the inverse of slot 14.
    virtual GENRESULT NEURONCOMPONENTS_API GetOrientationSpace(FVector* Pos, FVector* Vel) = 0;       // 20
    virtual GENRESULT NEURONCOMPONENTS_API GetOrientationMatrix(float* Out9) = 0;                 // 21
    virtual GENRESULT NEURONCOMPONENTS_API GetListenerPosition(FVector* Out) = 0;                   // 22
    virtual GENRESULT NEURONCOMPONENTS_API GetListenerVelocity(FVector* Out) = 0;                   // 23
    virtual float NEURONCOMPONENTS_API GetDistanceFactor() = 0;                                   // 24
    virtual float NEURONCOMPONENTS_API GetDopplerFactor() = 0;                                    // 25
    virtual float NEURONCOMPONENTS_API GetRolloffFactor() = 0;                                    // 26
    virtual GENRESULT NEURONCOMPONENTS_API EaxGetInstance(void* Id, GUID* Set, unsigned long Prop,
                                               void* Buf, unsigned long Cb,
                                               unsigned long* Pcb) = 0;                // 27 RET 0x1C
    virtual GENRESULT NEURONCOMPONENTS_API EaxSetInstance(void* Id, GUID* Set, unsigned long Prop,
                                               void* Data, unsigned long Cb) = 0;      // 28 RET 0x18
    virtual GENRESULT NEURONCOMPONENTS_API EaxGetPrimary(GUID* Set, unsigned long Prop, void* Buf,
                                              unsigned long Cb, unsigned long* Pcb) = 0; // 29
    virtual GENRESULT NEURONCOMPONENTS_API EaxSetPrimary(GUID* Set, unsigned long Prop, void* Data,
                                              unsigned long Cb) = 0;                   // 30
    virtual int NEURONCOMPONENTS_API CreateArchetypePcm(void* Fmt, unsigned long ByteCount, unsigned long LoopStart,
                                             unsigned long LoopEnd, void* Samples,
                                             unsigned long ArchFlags) = 0;             // 31 +0x7C RET 0x1C
    // 32 +0x80 RET 0xC. the branch: Source is a plain PCM descriptor, not a COM object --
    // format 8 bytes at +0, loop start +0x8, loop end +0xC, byte count +0x14, samples +0x18,
    // fed to the same the routine fill as slot 31.
    virtual int NEURONCOMPONENTS_API CreateArchetypeFromSource(void* Source, unsigned long ArchFlags) = 0;
    // 33 +0x84 RET 0xC. baseline 0042b72f pushes the caller's IFileSystem, not a path.
    virtual int NEURONCOMPONENTS_API CreateArchetypeFromWav(INeuronComponent* FileSystem,
                                                 unsigned long ArchFlags) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API DestroyArchetype(int Id) = 0;                          // 34 +0x88 RET 0x8
    virtual GENRESULT NEURONCOMPONENTS_API QueryArchetype(int Id, void** Out) = 0;                // 35 +0x8C RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API AttachPlayKey(ISoundSource* PlayKey,
                                              unsigned long Index) = 0;                // 36 +0x90 RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API DetachPlayKey(ISoundSource* PlayKey) = 0;              // 37 +0x94 RET 0x8
    virtual GENRESULT NEURONCOMPONENTS_API ReplaceAttachedList(void** Keys, unsigned long Count) = 0; // 38 +0x98 RET 0xC
    virtual unsigned long NEURONCOMPONENTS_API GetAttachedCount() = 0;                            // 39 RET 0x4
    virtual unsigned long NEURONCOMPONENTS_API GetPlayingCount() = 0;                             // 40 RET 0x4
    virtual GENRESULT NEURONCOMPONENTS_API StartupDevice(HWND Hwnd, unsigned long Flags) = 0;     // 41 +0xA4
    virtual GENRESULT NEURONCOMPONENTS_API ShutdownDevice() = 0;                                  // 42 +0xA8
    virtual GENRESULT NEURONCOMPONENTS_API GetDirectSound(IDirectSound** Out) = 0;                // 43 +0xAC
    virtual unsigned long NEURONCOMPONENTS_API GetTickCountSlot() = 0;                            // 44 +0xB0
    virtual GENRESULT GetDeviceSnapshot(void* Out) = 0;                                // 45 +0xB4 thiscall
    virtual GENRESULT NEURONCOMPONENTS_API MixerTick(ISoundListenerPose* Listener) = 0;           // 46
    virtual GENRESULT NEURONCOMPONENTS_API StopInstanceBuffer(ISoundSource* PlayKey) = 0;       // 47
    virtual GENRESULT NEURONCOMPONENTS_API PlayInstanceBuffer(ISoundSource* PlayKey) = 0;         // 48
    virtual GENRESULT NEURONCOMPONENTS_API GetArchetypeFormat(void* Id, void* Out) = 0;           // 49
    virtual GENRESULT NEURONCOMPONENTS_API GetArchetypeSamples(void* Id, void* Out) = 0;          // 50
    virtual unsigned long NEURONCOMPONENTS_API GetArchetypeFrames(void* Id) = 0;                  // 51
    virtual float NEURONCOMPONENTS_API GetArchetypeGain(void* Id) = 0;                            // 52
    virtual unsigned long NEURONCOMPONENTS_API GetArchetypeChannels(void* Id) = 0;                // 53
    virtual GENRESULT NEURONCOMPONENTS_API SetArchetypeSamples(void* A, void* B, void* C) = 0;    // 54
    virtual unsigned long NEURONCOMPONENTS_API GetArchetypeReady(void* Id) = 0;                   // 55
};
