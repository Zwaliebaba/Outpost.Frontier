// Author: Giperion.

#include "menu_sound_arch.h"
#include "oracle_mode.h" // Oracle instrumentation (P0.5)

#include "perf_options.h"

#include "client_equip_fx.h" // FEquipFxListNode / FClientEquip (CliThrusterSound, the routine)
#include "client_gameobj.h"  // FClientEquippedObj (CliThrusterSound +0x30)
#include "common_cequip.h"   // CEquip / CEThruster
#include "common_cobject.h"  // CObject / CSimple::OwnerPlayer
#include "common_csys.h"
#include "common_goods.h"    // ThrusterEquipConsts
#include "player_spawn.h"    // GetPlayerConnectionId (the field), GetPlayerCharacterId
#include "space_camera.h"    // IsFirstPersonCameraCurrent (the routine)
#include "common_id.h"
#include "common_ini.h"
#include "neuroncomponents.h"
#include "readfile.h"
#include "NeuronRuntime.h"
#include "soundmanager.h"
#include "sound_config.h"
#include "soundstreamer.h"
#include "x86math.h"
#include "common_misc.h"
#include "game_options.h"   // GetGameOption (the routine)
#include "sound_archetype.h"

#include <cstdint>
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <map>
#include <new>
#include <utility>
#include <windows.h>
#include <wrl/client.h>

namespace
{
    // s_Client\cmn\SoundArch.cpp (the FDUMP file of every SoundArch.cpp
    // message).
    char const kSoundArchCpp[] = "Client\\cmn\\SoundArch.cpp";
    static const float kDefaultRangeMin = 50.f;
    static const float kDefaultRangeMax = 300.f;
    static const unsigned kDefaultFlags = 0x10000000u;

    // Archetype::SoundImpl (vtable 0x005ca36c, 15 slots of Archetype::ISound). The slot bodies
    // are the ArchSlotN functions below; the overrides only forward to them.
    struct FClientSoundArch : Archetype::ISound
    {
        FClientSoundArch();

        int GetSmArchId() override;
        char const* GetFileName() override;
        unsigned GetSourceType() override;
        unsigned char Acquire() override;
        int Release() override;
        int GetUseCount() override;
        int GetAttenuation() override;
        float GetRangeMin() override;
        float GetRangeMax() override;
        unsigned char IsStreamer() override;
        unsigned char Is2d() override;
        float GetRandomPitch() override;
        Archetype::ISound* GetLinked() override;
        unsigned char IsPitchBendable() override;
        // XOR EAX,EAX.
        int GetPriority() override { return 0; }

        unsigned NicknameId;
        char* File;
        int SmArchId;
        float RangeMin;
        float RangeMax;
        unsigned Flags;
        unsigned Lod[2];
        FClientSoundArch* Linked;
        unsigned short Pad28;
        unsigned short UseCount;
        unsigned RefCount;
        char Nickname[64];
    };

    static_assert(offsetof(FClientSoundArch, NicknameId) == 4, "id @ +4");
    static_assert(offsetof(FClientSoundArch, File) == 8, "file @ +8");
    static_assert(offsetof(FClientSoundArch, SmArchId) == 0xC, "sm @ +0xC");
    static_assert(offsetof(FClientSoundArch, RangeMin) == 0x10, "range @ +0x10");
    static_assert(offsetof(FClientSoundArch, Flags) == 0x18, "flags @ +0x18");
    static_assert(offsetof(FClientSoundArch, Linked) == 0x24, "linked @ +0x24");
    static_assert(offsetof(FClientSoundArch, UseCount) == 0x2A, "use @ +0x2A");

    struct FSoundSource;

    // One node of Sound.cpp's playing-source list the field: a VC6 std::list<Sound::Source*>
    // node {next, prev, value}, 0xc bytes from operator new (the list's _Buynode the routine).
    // A playing source keeps its own node at +0x2c, which is how slot 5 (IsActive) answers and
    // how Stop unlinks in O(1).
    struct FPlayingSourceNode
    {
        FPlayingSourceNode* Next;
        FPlayingSourceNode* Prev;
        FSoundSource* Value;
    };

    static_assert(sizeof(FPlayingSourceNode) == 0xc, "list node 0xc");

    // (the allocator byte) / the field (head node) / the field (size). The
    // constructor is the static initializer the branch, the destructor its atexit handler
    // .
    struct FPlayingSourceList
    {
        FPlayingSourceList();
        ~FPlayingSourceList();

        FPlayingSourceNode* Head;
        unsigned Size;
    };

    // Sound::Source (vtable 0x005c9fac), baseline sizeof 0x30 -- the Heap pool the field has
    // that element size and the routine / the routine dispatch on it, so the size is a
    // behavioural fact here, not a layout nicety. Sound::StreamedSource (0x40) and
    // Sound::Source3D (0x64) derive from it below.
    struct FSoundSource : ISoundSource
    {
        // (bBit2). OldFlags is the flag byte the allocation already held: baseline
        // keeps its top three bits (AND 0xe0) and the pools do not clear memory.
        FSoundSource(unsigned char bBit2, unsigned char OldFlags);

        // 43 slots, src/include/soundmanager.h. Until 2026-09-16 this was a plain struct with
        // a `void** Vtable` first member, filled from a hand-written `static void*[42]` and
        // memcpy'd onto two more arrays to fake the streamed and 3D subclasses. The stub that
        // sat in the unimplemented entries returned `unsigned long` with no arguments, i.e.
        // RET 4, while SoundManager calls six of those slots with an out-pointer -- four bytes
        // of stack leaked per call. Real virtuals make that shape impossible to get wrong.
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
        unsigned long NEURONCOMPONENTS_API AddRef() override;
        unsigned long NEURONCOMPONENTS_API Release() override;

        unsigned long NEURONCOMPONENTS_API Slot3() override;
        unsigned long NEURONCOMPONENTS_API GetStartTick() override;
        unsigned char NEURONCOMPONENTS_API IsActive() override;
        unsigned char NEURONCOMPONENTS_API Is3d() override;
        unsigned char NEURONCOMPONENTS_API IsLooping() override;
        int NEURONCOMPONENTS_API GetArchId() override;
        GENRESULT NEURONCOMPONENTS_API GetAttenuation(float* Out) override;
        GENRESULT NEURONCOMPONENTS_API GetPitch(float* Out) override;
        GENRESULT NEURONCOMPONENTS_API GetPan(long* Out) override;
        // Slots 12 and 15..18 are the 3D block. In baseline the base class does NOT implement
        // them: Sound::Source and Sound::StreamedSource carry the shared "unsupported" stubs
        // (the branch for slot 15, the branch for the rest), and only Sound::Source3D has
        // bodies. FSpatialSource overrides all five below.
        GENRESULT NEURONCOMPONENTS_API GetPosition(FVec3* Out) override;
        GENRESULT NEURONCOMPONENTS_API GetMinDistance(float* Out) override;
        GENRESULT NEURONCOMPONENTS_API GetMaxDistance(float* Out) override;
        GENRESULT NEURONCOMPONENTS_API GetConeAngles(unsigned long* Inside,
                                          unsigned long* Outside) override;
        GENRESULT NEURONCOMPONENTS_API GetConeOrientation(FVec3* Out) override;
        GENRESULT NEURONCOMPONENTS_API GetConeOutsideVolume(float* Out) override;
        GENRESULT NEURONCOMPONENTS_API GetVelocity(FVec3* Out) override;
        GENRESULT NEURONCOMPONENTS_API GetEaxEnvironment(unsigned long* Out) override;
        GENRESULT NEURONCOMPONENTS_API GetSoundMode(unsigned long* Out) override;
        GENRESULT NEURONCOMPONENTS_API GetApplyMode(unsigned long* Out) override;

        unsigned char __thiscall Start(void* Arch, int Extra) override;
        unsigned char __thiscall AltStart(void* Arch) override;
        void __thiscall Stop() override;
        void __thiscall SetStartOffset(int Offset) override;

        void __thiscall Pause() override;
        void __thiscall Resume() override;
        unsigned long __thiscall IsPaused() override { return Field08 != 0 ? 1u : 0u; }
        unsigned char __thiscall IsFinished() override;
        unsigned long __thiscall Slot30() override;
        void __thiscall ComputeMixerScore(const float* Listener) override;
        void __thiscall SetAttenuation(float Attenuation) override;
        void __thiscall SetPan(float NewPan) override;
        void __thiscall SetLooping(unsigned char bLooping) override
        {
            Flags = static_cast<unsigned char>((Flags & 0xfeu) | (bLooping & 1u));
        }
        // Slots 35..37 in the Sound::Source and Sound::StreamedSource tables are the imported
        // do-nothings IDPMsgHandler::OnAddHost (RET 4, slots 35 and 36) and
        // IDPMsgHandler::OnReceive (RET 8, slot 37); only Sound::Source3D has bodies.
        unsigned long __thiscall Slot35(void* /*Arg*/) override { return 0; }
        void __thiscall SetVelocity(const float* /*Velocity*/) override {}
        void __thiscall SetPose(int /*AttachHandle*/, const float* /*Transform*/) override {}
        void __thiscall SetPitch(float NewPitch) override;
        // `OR EAX,0xffffffff; RET`: not attached.
        int __thiscall GetAttachHandle() override { return -1; }

        // `XOR AL,AL; RET`.
        unsigned char __thiscall Slot40() override { return 0; }
        void* __thiscall ScalarDeletingDestructor(unsigned char bDelete) override;
        // (RET 8): slot 31 with the listener, then the negation of slot 29. The
        // time step is not read.
        unsigned char __thiscall Update(const float* Listener, float /*DeltaSeconds*/) override
        {
            ComputeMixerScore(Listener);
            return IsFinished() == 0 ? 1u : 0u;
        }

        int Type;                     // +0x04  category (0 sfx, 1 interface, 2/5 voice, 3 music, 4 ambient)
        int Field08;                  // +0x08  the pause tick (slot 26 stores, slot 27 clears)
        unsigned char Flags;          // +0x0C  1 loop, 2 cockpit, 4 SoundManager-played, 8 started, 0x10 tick latched
        unsigned char Pad0D[3];
        float Pitch;                  // +0x10
        int StartTick;                // +0x14
        Archetype::ISound* Arch;      // +0x18  SoundImpl or VoiceMsg -- only ever used through the table
        float AttenuationDb;          // +0x1C  the per-source dB (play key +0x14, slot 32)
        float Pan;                    // +0x20
        int RefCount;                 // +0x24
        float MixerScale;             // +0x28  the mixer score slot 31 writes
        FPlayingSourceNode* ListNode; // +0x2C  this source's node in the field, null when idle
    };

    static_assert(offsetof(FSoundSource, Flags) == 0xC, "src flags @ +0xC");
    static_assert(offsetof(FSoundSource, Pitch) == 0x10, "src pitch @ +0x10");
    static_assert(offsetof(FSoundSource, Arch) == 0x18, "src arch @ +0x18");
    static_assert(offsetof(FSoundSource, AttenuationDb) == 0x1C, "src dB @ +0x1C");
    static_assert(offsetof(FSoundSource, Pan) == 0x20, "src pan @ +0x20");
    static_assert(offsetof(FSoundSource, RefCount) == 0x24, "src ref @ +0x24");
    static_assert(offsetof(FSoundSource, ListNode) == 0x2C, "src list @ +0x2C");
    static_assert(sizeof(FSoundSource) == 0x30, "Sound::Source is 0x30 (pool module state)");

    // SoundArch.cpp's archetype registry: a VC6 std::set<SoundArch> ordered by
    // the nickname id at +0x04 (unsigned: the routine 0x0042c320 CMP [node+0x10] / JNC), head
    // , size the field. Built by the static initializer the routine, emptied by
    // and destroyed through atexit the routine. baseline holds each SoundArch by
    // value inside its node; the port keeps it on the heap under the same key. Entries are only
    // ever removed at shutdown, so the addresses are equally stable, and the in-order walks
    // (the routine / the routine / the routine) see the same ascending-id order.
    std::map<unsigned, FClientSoundArch*> G_SoundArchs;

    // --- Sound.cpp: the source family ---

    // every started source, in start order (Start pushes at the back).
    // Declared before the three pools so it is built first, as in baseline's initializer table
    // (0x0060f144 the branch, then 0x0060f148 / 0x0060f14c / 0x0060f150).
    FPlayingSourceList G_PlayingSources;

    // The three Common.dll Heap pools of Sound.cpp, {element size, 0, -1, 0} each, built by
    // the static initializers the branch / the branch / the branch (the Heap ctor
    // inlined). the routine / the routine dispatch on the element size; the routine shrinks
    // all three.
    Heap G_SourceHeap(0x30u, -1);   // Sound::Source
    Heap G_Source3DHeap(0x64u, -1); // Sound::Source3D
    Heap G_StreamedHeap(0x40u, -1); // Sound::StreamedSource

    // — live Sound::Source instance count (ctor INC / dtor DEC; the routine warns if != 0).
    int G_AliveSoundSourceCount = 0;

    // — the routine / the routine. Mixer uses GetVelocity (+0x48) for SM slot 16.
    void* G_SoundListenerObject = nullptr;

    // A stream handle of IStreamer2::CreateStream counts only when it is neither 0 nor -1
    // (the paired `TEST EAX,EAX` / `CMP EAX,-1` of 0x0042a1f0, 0x0042a256, 0x0042a2b1, ...).
    bool IsStreamHandle(void* Handle)
    {
        return Handle != nullptr && Handle != reinterpret_cast<void*>(static_cast<intptr_t>(-1));
    }

    // Sound::StreamedSource (vtable 0x005ca05c), baseline sizeof 0x40 (pool the field): a
    // source that plays through IStreamer2 instead of a SoundManager buffer. Against
    // Sound::Source's table it replaces slots 22..27, 29, 32..34, 40, 41 and 42.
    struct FStreamedSource : FSoundSource
    {
        // .
        explicit FStreamedSource(unsigned char OldFlags);

        unsigned char __thiscall Start(void* Arch, int Extra) override;
        unsigned char __thiscall AltStart(void* Arch) override;
        void __thiscall Stop() override;
        void __thiscall SetStartOffset(int Offset) override;
        void __thiscall Pause() override;
        void __thiscall Resume() override;
        unsigned char __thiscall IsFinished() override;
        void __thiscall SetAttenuation(float Attenuation) override;
        void __thiscall SetPan(float NewPan) override;
        void __thiscall SetLooping(unsigned char bLooping) override;
        unsigned char __thiscall Slot40() override;
        void* __thiscall ScalarDeletingDestructor(unsigned char bDelete) override;
        unsigned char __thiscall Update(const float* Listener, float DeltaSeconds) override;

        void* StreamHandle;     // +0x30  IStreamer2 handle (0 / -1 = none)
        float FadeTargetDb;     // +0x34  the dB the fade runs to
        float FadeTime;         // +0x38  seconds left of the fade, -1 = no fade
        unsigned char bFadeOut; // +0x3C  set when the background slot let go of this source
        unsigned char Pad3D[3];
    };

    static_assert(offsetof(FStreamedSource, StreamHandle) == 0x30, "streamed @ +0x30");
    static_assert(offsetof(FStreamedSource, bFadeOut) == 0x3C, "fade-out @ +0x3C");
    static_assert(sizeof(FStreamedSource) == 0x40, "Sound::StreamedSource is 0x40 (pool module state)");

    // / the field -- the background music and ambience. the routine stores the
    // __RTDynamicCast of its play handle to Sound::StreamedSource here.
    FStreamedSource* G_BackgroundMusic = nullptr;
    FStreamedSource* G_BackgroundAmbient = nullptr;

    // Sound::Source3D (vtable 0x005c9efc), baseline sizeof 0x64 (pool the field). Slot 6 ("is
    // this 3D") is answered by the table the object carries: the branch returns true and sits
    // only here.
    struct FSpatialSource : FSoundSource
    {
        // .
        explicit FSpatialSource(unsigned char OldFlags);

        unsigned char NEURONCOMPONENTS_API Is3d() override { return 1; }

        // The five slots only Sound::Source3D implements.
        GENRESULT NEURONCOMPONENTS_API GetPosition(FVec3* Out) override;
        GENRESULT NEURONCOMPONENTS_API GetConeAngles(unsigned long* Inside,
                                          unsigned long* Outside) override;
        GENRESULT NEURONCOMPONENTS_API GetConeOrientation(FVec3* Out) override;
        GENRESULT NEURONCOMPONENTS_API GetConeOutsideVolume(float* Out) override;
        GENRESULT NEURONCOMPONENTS_API GetVelocity(FVec3* Out) override;
        // Slot 35 (+0x8c), the branch: the world position setter (see the body).
        unsigned long __thiscall Slot35(void* Arg) override;
        void __thiscall SetVelocity(const float* Velocity) override;
        void __thiscall SetPose(int AttachHandle, const float* Transform) override;
        // slot 39: `MOV EAX,[ECX+0x60]; RET`.
        int __thiscall GetAttachHandle() override { return Source3dId; }
        void* __thiscall ScalarDeletingDestructor(unsigned char bDelete) override;

        // (RET 8).
        unsigned char __thiscall Update(const float* Listener, float DeltaSeconds) override
        {
            if (Source3dId != -1)
            {
                float* const Position = Spatial;
                float ZStep;
                float X;
                float Y;
                float Z;
                // Only the Z product is rounded to float before adding the old position.
                __asm
                {
                    mov eax, Position
                    fld DeltaSeconds
                    fmul dword ptr [eax + 24]
                    fld DeltaSeconds
                    fmul dword ptr [eax + 28]
                    fld DeltaSeconds
                    fmul dword ptr [eax + 32]
                    fstp ZStep
                    fxch st(1)
                    fadd dword ptr [eax]
                    fstp X
                    fadd dword ptr [eax + 4]
                    fstp Y
                    fld ZStep
                    fadd dword ptr [eax + 8]
                    fstp Z
                }
                Spatial[0] = X;
                Spatial[1] = Y;
                Spatial[2] = Z;
            }
            ComputeMixerScore(Listener);
            return IsFinished() == 0 ? 1u : 0u;
        }

        float Spatial[12]; // +0x30..+0x5c: pos [0..2], cone direction [3..5] (1.0 at [5]),
                           // velocity [6..8], cone inner [9], outer [10] (360), outside volume [11]
        int Source3dId;    // +0x60  attached engine instance, -1 = free
    };

    static_assert(offsetof(FSpatialSource, Spatial) == 0x30, "3d block @ +0x30");
    static_assert(offsetof(FSpatialSource, Source3dId) == 0x60, "attach @ +0x60");
    static_assert(sizeof(FSpatialSource) == 0x64, "Sound::Source3D is 0x64 (pool module state)");

    // --- Sound.cpp globals ---

    // Master volume per source category, in dB. baseline keeps these in zero-initialised .data
    // (= 0 dB = unity) and only the audio_* user options ever write them, through
    // / e30 / e60 / e90 / ec0.
    float G_AudioVoiceDb = 0.f;     // (audio_voice)     — types 2 and 5
    float G_AudioInterfaceDb = 0.f; // (audio_interface) — type 1
    float G_AudioSfxDb = 0.f;       // (audio_sfx)       — type 0
    float G_AudioAmbientDb = 0.f;   // (audio_ambient)   — type 4
    float G_AudioMusicDb = 0.f;     // (audio_music)     — type 3

    // — the current sound level of detail, written by the `sound_level_of_detail`
    // option (the branch → the routine). 0 = "no lod", which is what the port starts from.
    int G_SoundLod = 0;
    // = CreateID("NONE"): a lod slot naming NONE resolves to *no* archetype.
    // Static initializer the branch (PUSH "NONE"; CALL CreateID; MOV [0x667cd4],EAX), so it
    // holds the id from load time; the only reader is 0x0042bfda (CMP EAX,[0x667cd4]).
    unsigned const G_SoundNoneId = CreateID("NONE");

    // mixer state (Sound.cpp globals).
    float G_DuckTargetDb = 0.f;
    float G_DuckCurrentDb = 0.f;  // _DAT_00667ca4
    int G_DuckRefCount = 0;
    float G_DuckTimer = -1.f;     // _DAT_00610904
    float G_MixerListener[3] = {0.f, 0.f, 0.f}; // _DAT_00667c5c/60/64
    static const unsigned kMixerMaxEntries = 0x80;
    FSoundSource* G_MixerRanked[kMixerMaxEntries] = {};
    unsigned G_MixerRankedCount = 0;

    // baseline returns 0xfffffff9 ("no value") from the ISoundSource getters.
    static const GENRESULT kGrNoValue = static_cast<GENRESULT>(0xfffffff9u);
    // / the field (the routine writes 0x7f7fffff).
    static const float kMixerNoScore = FLT_MAX;
    // / the field — the silence floor every dB path clamps to.
    static const float kSilenceDb = -96.f;

    FClientSoundArch* ArchFrom(void* Arch);
    FClientSoundArch* ArchGetLinked(FClientSoundArch* Arch);
    unsigned char ArchAcquire(FClientSoundArch* Arch);
    int ArchReleaseWav(FClientSoundArch* Arch);
    unsigned char SourceStart(FSoundSource* Source, Archetype::ISound* Arch, int Extra);
    void SourceStopLinked(FSoundSource* Source);
    void SourceStopBody(FSoundSource* Source);
    void SourceTeardown(FSoundSource* Source);
    void SourceMarkStarted(FSoundSource* Source);
    unsigned char SourceIsFinished(FSoundSource* Source);
    void StreamedApplyVolume(FStreamedSource* Source);
    unsigned char StreamedRestart(FStreamedSource* Source, Archetype::ISound* Arch, int Offset);
    void ReleaseComponent(void* Component);

    // s_Client\cmn\Sound.cpp (0x005c9ec8) and ...\Sound.h (0x005ca108).
    char const kSoundCpp[] = "Client\\cmn\\Sound.cpp";
    char const kSoundH[] = "Client\\cmn\\Sound.h";

    char const* NEURONCOMPONENTS_API ArchSlot1File(void* This)
    {
        // MOV EAX,[ECX+8]; RET.
        return ArchFrom(This)->File;
    }

    unsigned NEURONCOMPONENTS_API ArchSlot2Type(void* This)
    {
        // XOR EAX,EAX; MOV AL,[ECX+0x1a]; AND EAX,0xf.
        return (ArchFrom(This)->Flags >> 16) & 0xfu;
    }

    unsigned char NEURONCOMPONENTS_API ArchSlot3Acquire(void* This)
    {
        return ArchAcquire(ArchFrom(This));
    }

    int NEURONCOMPONENTS_API ArchSlot4Release(void* This)
    {
        return ArchReleaseWav(ArchFrom(This));
    }

    int NEURONCOMPONENTS_API ArchSlot5UseCount(void* This)
    {
        // slot 12 against this -> MOVSX word [+0x2a]; a link -> its slot 5 (tail
        // jump); none -> MOVSX word [+0x28].
        FClientSoundArch* Arch = ArchFrom(This);
        FClientSoundArch* Linked = ArchGetLinked(Arch);
        if (Linked == Arch)
            return static_cast<short>(Arch->UseCount);
        if (Arch->Linked != nullptr)
            return ArchSlot5UseCount(Arch->Linked);
        return static_cast<short>(Arch->Pad28);
    }

    int NEURONCOMPONENTS_API ArchSlot6Attenuation(void* This)
    {
        // MOVSX EAX,byte [ECX+0x18].
        return static_cast<signed char>(ArchFrom(This)->Flags & 0xffu);
    }

    float NEURONCOMPONENTS_API ArchSlot7RangeMin(void* This)
    {
        // FLD float [ECX+0x10].
        return ArchFrom(This)->RangeMin;
    }

    float NEURONCOMPONENTS_API ArchSlot8RangeMax(void* This)
    {
        // FLD float [ECX+0x14].
        return ArchFrom(This)->RangeMax;
    }

    unsigned char NEURONCOMPONENTS_API ArchSlot9Streamer(void* This)
    {
        // [ECX+0x18] >> 0x1a & 1.
        return static_cast<unsigned char>((ArchFrom(This)->Flags >> 26) & 1u);
    }

    unsigned char NEURONCOMPONENTS_API ArchSlot10Is2d(void* This)
    {
        // [ECX+0x18] >> 0x1b & 1.
        return static_cast<unsigned char>((ArchFrom(This)->Flags >> 27) & 1u);
    }

    // — arch slot 11 is not a range but a *random pitch* offset: when flag
    // is set and the pitch byte (Flags >> 8) is positive, baseline returns
    // rand() % (2N+1) - N. Lod-linked archetypes delegate to the linked archetype.
    float NEURONCOMPONENTS_API ArchSlot11RandomPitch(void* This)
    {
        // . Slot 12 (the lod link) against this; a link -> its slot 11
        // (tail jump), none -> 0.0f (the field). Own: flag 0x2000000 and the signed pitch
        // byte +0x19 > 0 (TEST AL,AL / JLE) -> FILD of rand() % (2N + 1) - N (IDIV, signed).
        FClientSoundArch* Arch = ArchFrom(This);
        if (ArchGetLinked(Arch) != Arch)
        {
            if (Arch->Linked == nullptr)
                return 0.f;
            return ArchSlot11RandomPitch(Arch->Linked);
        }
        if ((Arch->Flags & 0x2000000u) == 0)
            return 0.f;
        int const PitchRange = static_cast<signed char>((Arch->Flags >> 8) & 0xffu);
        if (PitchRange <= 0)
            return 0.f;
        return static_cast<float>(rand() % (2 * PitchRange + 1) - PitchRange);
    }

    unsigned char NEURONCOMPONENTS_API ArchSlot13PitchBendable(void* This)
    {
        // XOR EAX,EAX; MOV AL,[ECX+0x1b]; AND EAX,1.
        return static_cast<unsigned char>((ArchFrom(This)->Flags >> 24) & 1u);
    }

    int NEURONCOMPONENTS_API ArchSlot0SmArchId(void* This)
    {
        // slot 12 against this -> +0xc; a link -> its slot 0 (tail jump); none ->
        // OR EAX,-1.
        FClientSoundArch* Arch = ArchFrom(This);
        FClientSoundArch* Linked = ArchGetLinked(Arch);
        if (Linked == Arch)
            return Arch->SmArchId;
        if (Arch->Linked != nullptr)
            return ArchSlot0SmArchId(Arch->Linked);
        return -1;
    }

    // The Archetype::SoundImpl table 0x005ca36c. Until 2026-09-25 this was a hand-filled
    // `void*[16]` that nothing called through (every caller named the ArchSlotN function
    // directly), so a VoiceMsg could not ride the same heap; the source now holds an ISound*.
    // 's field defaults (formerly InitSoundArch's memset + stores).
    FClientSoundArch::FClientSoundArch()
        : NicknameId(0), File(nullptr), SmArchId(-1), RangeMin(kDefaultRangeMin),
          RangeMax(kDefaultRangeMax), Flags(kDefaultFlags), Linked(nullptr), Pad28(0),
          UseCount(0), RefCount(1)
    {
        Lod[0] = 0;
        Lod[1] = 0;
        Nickname[0] = '\0';
    }

    int FClientSoundArch::GetSmArchId() { return ArchSlot0SmArchId(this); }
    char const* FClientSoundArch::GetFileName() { return ArchSlot1File(this); }
    unsigned FClientSoundArch::GetSourceType() { return ArchSlot2Type(this); }
    unsigned char FClientSoundArch::Acquire() { return ArchSlot3Acquire(this); }
    int FClientSoundArch::Release() { return ArchSlot4Release(this); }
    int FClientSoundArch::GetUseCount() { return ArchSlot5UseCount(this); }
    int FClientSoundArch::GetAttenuation() { return ArchSlot6Attenuation(this); }
    float FClientSoundArch::GetRangeMin() { return ArchSlot7RangeMin(this); }
    float FClientSoundArch::GetRangeMax() { return ArchSlot8RangeMax(this); }
    unsigned char FClientSoundArch::IsStreamer() { return ArchSlot9Streamer(this); }
    unsigned char FClientSoundArch::Is2d() { return ArchSlot10Is2d(this); }
    float FClientSoundArch::GetRandomPitch() { return ArchSlot11RandomPitch(this); }
    Archetype::ISound* FClientSoundArch::GetLinked() { return ArchGetLinked(this); }
    unsigned char FClientSoundArch::IsPitchBendable() { return ArchSlot13PitchBendable(this); }

    // — extra attenuation from the current audio mode (the field). Flag 2 on
    // the source marks it as belonging to the cockpit/interior mix.
    float SourceModeDb(FSoundSource const* Source, int Mode)
    {
        if (Mode == 2 && (Source->Flags & 2u) == 0)
            return GetClientCockpitAttenuation(); // _DAT_00667c9c
        if (Mode == 1 && (Source->Flags & 2u) != 0)
            return -1e10f;
        return 0.f;
    }

    // The category switch the routine (0x00429a1c) and the routine (0x0042a46c) inline, the
    // same one the branch reaches through the routine. Source type (+0x4) picks the audio_*
    // master volume (float loads); sfx / music / ambient / voice-5 add the ducking offset
    // on the FPU (FADD float, so the sum is x87 -- double here), interface and
    // voice-2 do not. The audio-mode term is 0, the field or -1e10 (the field): mode 2
    // without flag 2 / mode 1 with flag 2 for sfx and ambient; mode 1 alone for interface, where
    // it also drops the master volume. Types above 5 contribute nothing.
    void SourceCategoryDb(FSoundSource const* Source, double* OutBase, double* OutMode)
    {
        int const Mode = GetClientAudioMode();
        double Base = 0.0;
        double ModeDb = 0.0;
        bool bDuck = false;
        switch (Source->Type)
        {
        case 0: // audio_sfx
            Base = G_AudioSfxDb;
            ModeDb = SourceModeDb(Source, Mode);
            bDuck = true;
            break;
        case 1: // audio_interface
            if (Mode == 1)
                ModeDb = -1e10f;
            else
                Base = G_AudioInterfaceDb;
            break;
        case 2: // audio_voice
            Base = G_AudioVoiceDb;
            break;
        case 3: // audio_music
            Base = G_AudioMusicDb;
            bDuck = true;
            break;
        case 4: // audio_ambient
            Base = G_AudioAmbientDb;
            ModeDb = SourceModeDb(Source, Mode);
            bDuck = true;
            break;
        case 5: // audio_voice, ducked
            Base = G_AudioVoiceDb;
            bDuck = true;
            break;
        default:
            break;
        }
        if (bDuck)
            Base = Base + static_cast<double>(G_DuckCurrentDb); // _DAT_00667ca4
        *OutBase = Base;
        *OutMode = ModeDb;
    }

    // The low dword of MSVCRT ftol's 64-bit truncation: a value outside the int64 range (NaN
    // included) is the integer indefinite 0x8000000000000000, whose low dword is 0.
    long FtolLow32(double Value)
    {
        if (Value >= -9223372036854775808.0 && Value < 9223372036854775808.0)
            return static_cast<long>(static_cast<long long>(Value));
        return 0;
    }

    // slot 0 of all three source tables is `MOV EAX,0xfffffff9; RET 0xC`: a
    // source answers no interface query and writes nothing to Out. (The port used to hand back
    // this with an AddRef.)
    GENRESULT NEURONCOMPONENTS_API FSoundSource::QueryInterface(const char* /*Iid*/, void** /*Out*/)
    {
        return kGrNoValue;
    }

    unsigned long NEURONCOMPONENTS_API FSoundSource::AddRef()
    {
        ++RefCount;
        return static_cast<unsigned long>(RefCount);
    }

    void SourceTeardown(FSoundSource* Source);

    unsigned long NEURONCOMPONENTS_API FSoundSource::Release()
    {
        // listing 00429c70..00429c8f: --[this+0x24] with no floor; at exactly
        // zero, slot 41 (+0xa4) with 1 -- the scalar deleting destructor of whatever class the
        // source is (CliThrusterSound overrides it); the new count is returned as is. The
        // destructor of a still-playing source releases the list's reference once more
        // (the routine, 0x0042910c) and so takes the count below zero -- no second destroy.
        --RefCount;
        int const Remaining = RefCount;
        if (Remaining == 0)
            ScalarDeletingDestructor(1);
        return static_cast<unsigned long>(Remaining);
    }

    // / the routine -- slots 3 and 4. baseline's slot 4 calls slot 3 and then
    // hands back this+0x14, the tick the sound started on. The port used to install its
    // zero-returning stub in both, so every one-shot looked like it had started at tick 0 and
    // ISoundManager::OneShotAlreadyExpired treated it as already over.
    unsigned long NEURONCOMPONENTS_API FSoundSource::Slot3()
    {
        SourceMarkStarted(this);
        return 0;
    }

    unsigned long NEURONCOMPONENTS_API FSoundSource::GetStartTick()
    {
        if ((Flags & 0x10u) == 0)
            Slot3();
        return static_cast<unsigned long>(StartTick);
    }

    // slot 5: `[this+0x2c] != 0`, the source has a node in the field.
    unsigned char NEURONCOMPONENTS_API FSoundSource::IsActive()
    {
        return ListNode != nullptr ? 1u : 0u;
    }

    // Slot 6: the branch ("return true") only sits in the Sound::Source3D vtable; plain and
    // streamed sources get the routine ("return false"). FSpatialSource overrides it.
    unsigned char NEURONCOMPONENTS_API FSoundSource::Is3d()
    {
        return 0;
    }

    unsigned char NEURONCOMPONENTS_API FSoundSource::IsLooping()
    {
        return static_cast<unsigned char>(Flags & 1u);
    }

    // slot 8: `MOV ECX,[ECX+0x18]; MOV EAX,[ECX]; JMP [EAX]`, the archetype's
    // slot 0. No test of the archetype.
    int NEURONCOMPONENTS_API FSoundSource::GetArchId()
    {
        return Arch->GetSmArchId();
    }

    // (RET 8) — the volume the SoundManager applies to the instance, in dB. No
    // archetype: *Out = 0 and 0xfffffff9. Otherwise (arch slot 6 FILD + the +0x1c dB) +
    // ((master [+ duck]) + mode) on the FPU, then `TEST AH,0x41 / JNZ` (only "greater" takes
    // 0.0) and `TEST AH,5 / JP` (only "less" takes -96, the field). No test of Out.
    GENRESULT NEURONCOMPONENTS_API FSoundSource::GetAttenuation(float* Out)
    {
        if (Arch == nullptr)
        {
            *Out = 0.f;
            return kGrNoValue;
        }

        double const ArchAndSource = static_cast<double>(Arch->GetAttenuation()) +
                                     static_cast<double>(AttenuationDb);
        double Base = 0.0;
        double ModeDb = 0.0;
        SourceCategoryDb(this, &Base, &ModeDb);

        double const Total = ArchAndSource + (Base + ModeDb);
        if (Total > 0.0)
            *Out = 0.f;
        else if (Total < static_cast<double>(kSilenceDb))
            *Out = kSilenceDb;
        else
            *Out = static_cast<float>(Total);
        return GR_OK;
    }

    // (RET 8) — slot 10: the random pitch latched at play time (+0x10), but only
    // for archetypes that declared themselves pitch-bendable (slot 13). `TEST AH,0x44 / JNP`
    // rejects only an exact 0.0 (a NaN pitch is handed out). No test of Out.
    GENRESULT NEURONCOMPONENTS_API FSoundSource::GetPitch(float* Out)
    {
        if (Arch != nullptr && Arch->IsPitchBendable() != 0 && !(Pitch == 0.f))
        {
            *Out = Pitch;
            return GR_OK;
        }
        *Out = 0.f;
        return kGrNoValue;
    }

    // (RET 4) -- slot 32: below -96.0 (double the field, `TEST AH,5 / JP`)
    // -96; above 0.0 (`TEST AH,0x41 / JNZ`) 0; otherwise the value, NaN included.
    void __thiscall FSoundSource::SetAttenuation(float Attenuation)
    {
        if (Attenuation < -96.f)
            Attenuation = -96.f;
        else if (Attenuation > 0.f)
            Attenuation = 0.f;
        AttenuationDb = Attenuation;
    }

    GENRESULT NEURONCOMPONENTS_API FSoundSource::GetPan(long* Out)
    {
        // baseline ftol returns low32 of int64 truncation; invalid conversions have low32 zero.
        *Out = Pan >= -0x1p63f && Pan < 0x1p63f
            ? static_cast<long>(static_cast<long long>(Pan)) : 0;
        return GR_OK;
    }

    void __thiscall FSoundSource::SetPan(float NewPan)
    {
        if (NewPan < -10000.f)
            Pan = -10000.f;
        else if (NewPan > 10000.f)
            Pan = 10000.f;
        else
            Pan = NewPan;
    }

    // Slot 12 in the two non-3D vtables is the branch, "unsupported". Same for 15..18.
    // The mixer guards all five with Is3d(), so a flat source is never asked.
    GENRESULT NEURONCOMPONENTS_API FSoundSource::GetPosition(FVec3* /*Out*/)
    {
        return kGrNoValue;
    }

    GENRESULT NEURONCOMPONENTS_API FSoundSource::GetConeAngles(unsigned long* /*Inside*/,
                                                    unsigned long* /*Outside*/)
    {
        return kGrNoValue;
    }

    GENRESULT NEURONCOMPONENTS_API FSoundSource::GetConeOrientation(FVec3* /*Out*/)
    {
        return kGrNoValue;
    }

    GENRESULT NEURONCOMPONENTS_API FSoundSource::GetConeOutsideVolume(float* /*Out*/)
    {
        return kGrNoValue;
    }

    GENRESULT NEURONCOMPONENTS_API FSoundSource::GetVelocity(FVec3* /*Out*/)
    {
        return kGrNoValue;
    }

    // — lazy Acquire3DMathEngine (the routine), shared by Source3D getters.
    I3DMathEngine* G_SoundMathEngine = nullptr;

    I3DMathEngine* AcquireSoundMathEngine()
    {
        if (G_SoundMathEngine == nullptr)
            G_SoundMathEngine = Acquire3DMathEngine();
        return G_SoundMathEngine;
    }

    // — slot 12. When Source3dId != -1: IEngine+0xa0 GetInstanceTransform +
    // I3DMathEngine+0x28 TransformXform(local +0x30 → world Out). Else copy Spatial[0..2].
    GENRESULT NEURONCOMPONENTS_API FSpatialSource::GetPosition(FVec3* Out)
    {
        // Listing 0x00429d30..0x00429db8 (RET 8): no test of Out, the engine, the transform or
        // the math engine; both arms return 0.
        if (Source3dId != -1)
        {
            void* const Instance = reinterpret_cast<void*>(static_cast<intptr_t>(Source3dId));
            FTransform const* const InstXform =
                reinterpret_cast<FTransform const*>(NeuronRuntime::Engine->GetInstanceTransform(Instance));
            // the field tested and filled.
            I3DMathEngine* const Math = AcquireSoundMathEngine();
            FVector const Local = {Spatial[0], Spatial[1], Spatial[2]};
            FVector World;
            Math->TransformXform(World, *InstXform, Local);
            Out->X = World.X;
            Out->Y = World.Y;
            Out->Z = World.Z;
            return GR_OK;
        }
        Out->X = Spatial[0];
        Out->Y = Spatial[1];
        Out->Z = Spatial[2];
        return GR_OK;
    }

    // slot 35 (+0x8c, RET 4): set the world position. Attached (+0x60 != -1):
    // the instance transform (IEngine+0xa0) and the field's vt+0x3c InverseTransform turn
    // the point into the instance's frame, stored at +0x30; unattached, the point is stored as
    // it is. No null test on the argument, the engine or the transform, as in baseline. The
    // non-3D tables fold this slot onto a `RET 0x4` no-op (the base Slot35).
    unsigned long __thiscall FSpatialSource::Slot35(void* Arg)
    {
        FVector const* const WorldPos = static_cast<FVector const*>(Arg);
        if (Source3dId != -1)
        {
            void* const Instance = reinterpret_cast<void*>(static_cast<intptr_t>(Source3dId));
            FTransform const* const InstXform =
                reinterpret_cast<FTransform const*>(NeuronRuntime::Engine->GetInstanceTransform(Instance));
            FVector Local;
            AcquireSoundMathEngine()->InverseTransform(Local, *InstXform, *WorldPos);
            Spatial[0] = Local.X;
            Spatial[1] = Local.Y;
            Spatial[2] = Local.Z;
            return 0;
        }
        Spatial[0] = WorldPos->X;
        Spatial[1] = WorldPos->Y;
        Spatial[2] = WorldPos->Z;
        return 0;
    }

    GENRESULT NEURONCOMPONENTS_API FSoundSource::GetMinDistance(float* Out)
    {
        if (Arch == nullptr)
            return kGrNoValue;
        *Out = Arch->GetRangeMin();
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FSoundSource::GetMaxDistance(float* Out)
    {
        if (Arch == nullptr)
            return kGrNoValue;
        float const Scale = GetPerfOptionSlot(2);
        *Out = Arch->GetRangeMax() * Scale;
        return GR_OK;
    }

    // ftol() truncates toward zero.
    int Ftol(float Value)
    {
        return static_cast<int>(Value);
    }

    // The cone gate every Source3D cone getter opens with (00429ec0 / 00429f20 / 00429f60):
    // +0x54 FCOMP 0 / TEST AH,0x41 / JZ: above 0 (ordered) passes; otherwise +0x58 FCOMP 360.0f
    // (the field) / TEST AH,0x5 / JP: >= 360 or unordered fails, below 360 passes. So a
    // cone is "set" when inner > 0 or outer < 360, both ordered (a NaN fails its test).
    bool SourceHasCone(FSpatialSource const* This)
    {
        return This->Spatial[9] > 0.f || This->Spatial[10] < 360.f;
    }

    // — slot 15. baseline ftol()s both angles into ints. No test of the out
    // pointers (0x00429ec0..0x00429f07).
    GENRESULT NEURONCOMPONENTS_API FSpatialSource::GetConeAngles(unsigned long* Inside,
                                                      unsigned long* Outside)
    {
        if (!SourceHasCone(this))
            return kGrNoValue;
        *Inside = static_cast<unsigned long>(Ftol(Spatial[9]));
        *Outside = static_cast<unsigned long>(Ftol(Spatial[10]));
        return GR_OK;
    }

    // — slot 16. Cone gate first; then +0x60 == -1 → raw Spatial[3..5], else
    // IEngine+0x98 GetInstanceOrientation + I3DMathEngine+0x2c TransformMatrix.
    GENRESULT NEURONCOMPONENTS_API FSpatialSource::GetConeOrientation(FVec3* Out)
    {
        // Listing 0x00429f60..0x0042a016 (RET 8): the cone gate (fail -> 0xfffffff9 at
        // ), then no test of Out, the engine, the orientation or the math engine.
        if (!SourceHasCone(this))
            return kGrNoValue;
        if (Source3dId != -1)
        {
            void* const Instance = reinterpret_cast<void*>(static_cast<intptr_t>(Source3dId));
            FMatrix const* const Orient =
                reinterpret_cast<FMatrix const*>(NeuronRuntime::Engine->GetInstanceOrientation(Instance));
            // the field tested and filled.
            I3DMathEngine* const Math = AcquireSoundMathEngine();
            FVector const Local = {Spatial[3], Spatial[4], Spatial[5]};
            FVector World;
            Math->TransformMatrix(World, *Orient, Local);
            Out->X = World.X;
            Out->Y = World.Y;
            Out->Z = World.Z;
            return GR_OK;
        }
        Out->X = Spatial[3];
        Out->Y = Spatial[4];
        Out->Z = Spatial[5];
        return GR_OK;
    }

    // — slot 17 (RET 8): the cone gate, then +0x5c to *Out; no test of Out.
    GENRESULT NEURONCOMPONENTS_API FSpatialSource::GetConeOutsideVolume(float* Out)
    {
        if (!SourceHasCone(this))
            return kGrNoValue;
        *Out = Spatial[11];
        return GR_OK;
    }

    // — slot 18. `+0x60 == -1` → Spatial[6..8]; else IEngine+0x80
    // GetInstanceUserData → CObject::get_velocity (vtable +0x54, Vector by value). Null
    // userdata → zero vector (the branch).
    GENRESULT NEURONCOMPONENTS_API FSpatialSource::GetVelocity(FVec3* Out)
    {
        // Listing 0x00429de0..0x00429e5e (RET 8): no test of Out or the engine; only the
        // user-data pointer is tested (0x00429e02).
        if (Source3dId != -1)
        {
            void* const Instance = reinterpret_cast<void*>(static_cast<intptr_t>(Source3dId));
            CObject* const User = (CObject*)NeuronRuntime::Engine->GetInstanceUserData(Instance);
            if (User == nullptr)
            {
                Out->X = 0.f;
                Out->Y = 0.f;
                Out->Z = 0.f;
                return GR_OK;
            }
            Vector const Velocity = User->get_velocity();
            Out->X = Velocity.x;
            Out->Y = Velocity.y;
            Out->Z = Velocity.z;
            return GR_OK;
        }
        Out->X = Spatial[6];
        Out->Y = Spatial[7];
        Out->Z = Spatial[8];
        return GR_OK;
    }

    // slots 19, 20 and 21 share one baseline body: MOV EAX,0xFFFFFFF9; RET 0x8.
    // "Unsupported", one argument. The port installed its zero-argument stub in all three,
    // so SoundManager pushed eight bytes and the callee popped four, three times over.
    GENRESULT NEURONCOMPONENTS_API FSoundSource::GetEaxEnvironment(unsigned long* /*Out*/)
    {
        return static_cast<GENRESULT>(-7);
    }

    GENRESULT NEURONCOMPONENTS_API FSoundSource::GetSoundMode(unsigned long* /*Out*/)
    {
        return static_cast<GENRESULT>(-7);
    }

    GENRESULT NEURONCOMPONENTS_API FSoundSource::GetApplyMode(unsigned long* /*Out*/)
    {
        return static_cast<GENRESULT>(-7);
    }

    // (RET 4) — slot 36: three dwords to +0x48. No test of the pointer.
    void __thiscall FSpatialSource::SetVelocity(const float* Velocity)
    {
        Spatial[6] = Velocity[0];
        Spatial[7] = Velocity[1];
        Spatial[8] = Velocity[2];
    }

    // (RET 8) — slot 37. The second argument is a Transform: a 3x3 orientation at
    // +0x00 and the world position at +0x24 (the call site at 0052d9de builds exactly that on
    // the stack — identity rows at +0x00/+0x10/+0x20 and a zero Vector at +0x24). baseline keeps
    // only the position; the orientation is re-fetched from the attached object when it needs
    // one. +0x60 first, then the three dwords; no test of the pointer.
    void __thiscall FSpatialSource::SetPose(int AttachHandle, const float* Transform)
    {
        Source3dId = AttachHandle;
        Spatial[0] = Transform[9];  // +0x24
        Spatial[1] = Transform[10]; // +0x28
        Spatial[2] = Transform[11]; // +0x2c
    }

    // — slot 38: the pitch percentage, clamped to [-100, 100]
    // (the field / the field).
    void __thiscall FSoundSource::SetPitch(float NewPitch)
    {
        if (NewPitch > 100.f)
            Pitch = 100.f;
        else if (NewPitch < -100.f)
            Pitch = -100.f;
        else
            Pitch = NewPitch;
    }

    // slot 22 of Sound::Source and Sound::Source3D.
    unsigned char __thiscall FSoundSource::Start(void* ArchArg, int Extra)
    {
        return SourceStart(this, static_cast<Archetype::ISound*>(ArchArg), Extra);
    }

    // slot 23 of Sound::Source and Sound::Source3D: `XOR AL,AL; RET 4`. Only
    // the streamed class has a second way to start (the branch). The port used to run the
    // ordinary Start here.
    unsigned char __thiscall FSoundSource::AltStart(void* /*ArchArg*/)
    {
        return 0;
    }

    // slot 24.
    void __thiscall FSoundSource::Stop()
    {
        SourceStopBody(this);
    }

    void __thiscall FSoundSource::SetStartOffset(int Offset)
    {
        if ((Flags & 0x10u) != 0)
        {
            unsigned const Now = NeuronRuntime::Sound->GetTickCountSlot();
            StartTick = static_cast<int>(Now - static_cast<unsigned>(Offset));
        }
        else
            StartTick = Offset;
    }

    void __thiscall FSoundSource::Pause()
    {
        if (NeuronRuntime::Sound == nullptr)
            return;
        Field08 = static_cast<int>(NeuronRuntime::Sound->GetTickCountSlot());
        NeuronRuntime::Sound->StopInstanceBuffer(this);
    }

    void __thiscall FSoundSource::Resume()
    {
        if (NeuronRuntime::Sound == nullptr)
            return;
        if ((Flags & 0x10u) != 0)
        {
            unsigned const Now = NeuronRuntime::Sound->GetTickCountSlot();
            StartTick = static_cast<int>(static_cast<unsigned>(StartTick) +
                Now - static_cast<unsigned>(Field08));
        }
        NeuronRuntime::Sound->PlayInstanceBuffer(this);
        Field08 = 0;
    }

    // slot 41 of Sound::Source: the destructor body (the routine's, inlined:
    // SourceTeardown) always, then with bit 0 the block back to the 0x30 pool the field
    // (Heap::Free, 0x00429125). Returns this.
    void* __thiscall FSoundSource::ScalarDeletingDestructor(unsigned char bDelete)
    {
        void* const Self = this;
        SourceTeardown(this);
        if ((bDelete & 1) != 0)
            G_SourceHeap.Free(Self);
        return Self;
    }

    // slot 41 of Sound::Source3D: the routine, then with bit 0 Heap::Free on
    // the 0x64 pool the field. Returns this.
    void* __thiscall FSpatialSource::ScalarDeletingDestructor(unsigned char bDelete)
    {
        void* const Self = this;
        SourceTeardown(this);
        if ((bDelete & 1) != 0)
            G_Source3DHeap.Free(Self);
        return Self;
    }

    // slot 41 of Sound::StreamedSource: the routine (the base body -- the
    // stream handle at +0x30 is not touched), then with bit 0 Heap::Free on the 0x40 pool
    // . Returns this.
    void* __thiscall FStreamedSource::ScalarDeletingDestructor(unsigned char bDelete)
    {
        void* const Self = this;
        SourceTeardown(this);
        if ((bDelete & 1) != 0)
            G_StreamedHeap.Free(Self);
        return Self;
    }


    // A `void*` archetype at this file's boundary is an Archetype::ISound*. Only paths that can
    // only ever see a SoundImpl (the [Sound] registry itself) come down to FClientSoundArch.
    FClientSoundArch* ArchFrom(void* Arch)
    {
        return static_cast<FClientSoundArch*>(static_cast<Archetype::ISound*>(Arch));
    }

    // — resolve this archetype's lod link. Flag 0x20000000 means "already
    // resolved"; baseline sets it *first* so the slot 3/4/5 calls below re-enter slot 12 harmlessly.
    void ArchResolveLod(FClientSoundArch* Arch, int Lod)
    {
        // No null test (0x0042bf97 reads +0x18 at once).
        Arch->Flags |= 0x20000000u;
        if (Arch->Linked == nullptr)
            Arch->Linked = Arch;

        FClientSoundArch* Target = Arch;
        // any Lod above 0 -- no upper bound -- picks the ID_String at
        // this + 0x18 + Lod * 4 (lod1 +0x1c, lod2 +0x20).
        if (Lod > 0)
        {
            unsigned const LodId = *reinterpret_cast<unsigned const*>(
                reinterpret_cast<unsigned char const*>(Arch) + 0x18 + Lod * 4);
            if (LodId != 0 && LodId != Arch->NicknameId)
            {
                if (LodId == G_SoundNoneId)
                {
                    Target = nullptr;
                }
                else
                {
                    // the routine's body inlined on the lod id --
                    // LookupClientSoundArch emits the same 0x168 miss warning.
                    ID_String LodName;
                    LodName.Id = LodId;
                    Target = ArchFrom(LookupClientSoundArch(&LodName));
                    if (Target == nullptr)
                    {
                        // both names through ID_String::get_string.
                        ID_String ArchName;
                        ArchName.Id = Arch->NicknameId;
                        FDUMP(0x100001,
                              "%s(%d) : *** ERROR: [Sound] %s. Specified 'lod%d = %s' not found."
                              "  Sound won't play.",
                              kSoundArchCpp, 0x3cb, ArchName.get_string(), Lod,
                              LodName.get_string());
                    }
                }
            }
        }

        if (Target == Arch->Linked)
            return;

        // Move the outstanding wav references from the old link to the new one.
        int const Uses = ArchSlot5UseCount(Arch);
        for (int Index = 0; Index < Uses; ++Index)
        {
            ArchReleaseWav(Arch);
            if (Target != nullptr)
                ArchAcquire(Target);
        }
        Arch->Linked = Target;
    }

    // — arch slot 12: resolve the lod once, then hand back the link.
    FClientSoundArch* ArchGetLinked(FClientSoundArch* Arch)
    {
        // TEST [+0x18],0x20000000 / JNZ; else the routine(the field);
        // return +0x24. No null test.
        if ((Arch->Flags & 0x20000000u) == 0)
            ArchResolveLod(Arch, G_SoundLod);
        return Arch->Linked;
    }

    // ISoundManager slot 36 (+0x90) takes two arguments, not one: the routine is RET 0xC and
    // pushes the field before the play key. The port was passing only the
    // key, which unbalances the stack on a __stdcall that pops 12 bytes.

    // TODO: the field is a sound-subsystem global written by the routine (reached through
    // the thunk at the branch) and zeroed on sound shutdown at 0x005a881f / 0x005a5a71. That
    // setter is not ported yet, so this holds baseline's own initial .data value. Port
    // and its caller before treating this as final; do not guess a meaning for
    // the index from AttachPlayKey's insert-before-Index behaviour.
    unsigned long G_SoundAttachIndex = 0;

    // The power-ratio fade step shared by the ducking fade of the routine (0x0042809a..
    // ) and the streamed fade of the branch (0x0042a71a..0x0042a78d), the same
    // arithmetic as the routine (ClientSoundFadeDb): both ends to power ratios through
    // exp(dB * ) (a float constant), moved Dt / (Remaining + Dt) of the way --
    // (To - From) * ratio + From on the FPU --, then `TEST AH,0x41 / JP`: at or below
    // (float 10^-9.6) -96 dB, above it 10 * log10 (FLDLG2 / FYL2X, double 10.0).
    // The result stays on the FPU for the caller, so it is returned as a double.
    double DbBlend(float FromDb, float ToDb, float Dt, double Remaining)
    {
        double const K = static_cast<double>(0.230258509516716f);
        double const From = exp(static_cast<double>(FromDb) * K);
        double const To = exp(static_cast<double>(ToDb) * K);
        double const Value =
            (To - From) * (static_cast<double>(Dt) / (Remaining + static_cast<double>(Dt))) + From;
        if (!(Value > static_cast<double>(2.5118898627773945e-10f)))
            return static_cast<double>(kSilenceDb);
        return log10(Value) * 10.0;
    }

    // slot 30 (bare RET, SEH frame for the archetype reference): the length of
    // the sound in ms. No archetype, SoundManager id -1 (arch slot 0) or no ISoundArchetype
    // (SoundManager slot 35) -> 0. Otherwise the archetype's slot 8, and when slot 10 hands out
    // a pitch, that pitch clamped to [-100, 100] (the field `TEST AH,5 / JP`, the field
    // `TEST AH,0x41 / JNZ`) divides it by pow(2.0, pitch * 0.01f) -- the length as an unsigned
    // FILD qword --, truncated by ftol. The archetype reference is released on every path.
    unsigned long SourceDurationMs(FSoundSource* Source)
    {
        if (Source->Arch == nullptr)
            return 0;
        int const ArchId = Source->Arch->GetSmArchId();
        if (ArchId == -1)
            return 0;

        Microsoft::WRL::ComPtr<ISoundArchetype> Archetype;
        NeuronRuntime::Sound->QueryArchetype(ArchId, reinterpret_cast<void**>(Archetype.GetAddressOf()));
        if (Archetype == nullptr)
            return 0;
        unsigned long Result = Archetype->GetDurationMs();

        float Pitch = 0.f;
        if (Source->GetPitch(&Pitch) == GR_OK)
        {
            if (Pitch < -100.f)
                Pitch = -100.f;
            else if (Pitch > 100.f)
                Pitch = 100.f;
            double const Adjusted = static_cast<double>(Result) /
                pow(2.0, static_cast<double>(Pitch) * static_cast<double>(0.01f));
            // baseline ftol returns the low dword of a signed 64-bit truncation.
            Result = static_cast<unsigned long>(FtolLow32(Adjusted));
        }
        return Result;
    }

    unsigned long __thiscall FSoundSource::Slot30()
    {
        return SourceDurationMs(this);
    }

    // (slot 29, bare RET): looping -> not finished. No archetype, or no
    // SoundManager for a SoundManager-played source (flag 4) -> finished. Not started (flag 8)
    // or not SoundManager-played -> not finished. SoundManager id -1 -> finished. Otherwise
    // finished once start tick + slot 30 is below the SoundManager clock (slot 44; SETC,
    // unsigned).
    unsigned char SourceIsFinished(FSoundSource* Source)
    {
        if ((Source->Flags & 1u) != 0)
            return 0;
        if (Source->Arch != nullptr && (NeuronRuntime::Sound != nullptr || (Source->Flags & 4u) == 0))
        {
            if ((Source->Flags & 8u) == 0)
                return 0;
            if ((Source->Flags & 4u) == 0)
                return 0;
            if (Source->Arch->GetSmArchId() != -1)
            {
                unsigned const Now = NeuronRuntime::Sound->GetTickCountSlot();
                unsigned const Duration = Source->Slot30();
                unsigned const End = static_cast<unsigned>(Source->StartTick) + Duration;
                return End < Now ? 1u : 0u;
            }
        }
        return 1;
    }

    unsigned char __thiscall FSoundSource::IsFinished()
    {
        return SourceIsFinished(this);
    }

    // (slot 3, RET 4) — latch the start tick once, then mark the source started.
    // Under flag 4 without 0x10 the tick becomes clock - tick (clock alone for a zero tick,
    // the same value); no test of the source or of NeuronRuntime::Sound.
    void SourceMarkStarted(FSoundSource* Source)
    {
        if ((Source->Flags & 8u) != 0)
            return;
        if ((Source->Flags & 4u) != 0 && (Source->Flags & 0x10u) == 0)
        {
            unsigned const Now = NeuronRuntime::Sound->GetTickCountSlot();
            Source->StartTick = static_cast<int>(Now - static_cast<unsigned>(Source->StartTick));
            Source->Flags = static_cast<unsigned char>(Source->Flags | 0x10u);
        }
        Source->Flags = static_cast<unsigned char>(Source->Flags | 8u);
    }

    void __thiscall FSoundSource::ComputeMixerScore(const float* Listener)
    {
        if (Arch != nullptr)
        {
            int const ArchDb = Arch->GetAttenuation();
            float SourceDb = AttenuationDb;
            float CombinedDb;
            // baseline rounds this sum to float, but keeps the following category sum in x87.
            __asm
            {
                fild ArchDb
                fadd SourceDb
                fstp CombinedDb
            }
            float Base = 0.f;
            float ModeDb = 0.f;
            bool bDuck = false;
            switch (Type)
            {
            case 0:
                ModeDb = SourceModeDb(this, GetClientAudioMode());
                Base = G_AudioSfxDb;
                bDuck = true;
                break;
            case 1:
                if (GetClientAudioMode() == 1)
                    ModeDb = -1e10f;
                else
                    Base = G_AudioInterfaceDb;
                break;
            case 2:
                Base = G_AudioVoiceDb;
                break;
            case 3:
                Base = G_AudioMusicDb;
                bDuck = true;
                break;
            case 4:
                ModeDb = SourceModeDb(this, GetClientAudioMode());
                Base = G_AudioAmbientDb;
                bDuck = true;
                break;
            case 5:
                Base = G_AudioVoiceDb;
                bDuck = true;
                break;
            }
            unsigned char bSilent;
            __asm
            {
                fld Base
                cmp bDuck, 0
                je AddMode
                fadd G_DuckCurrentDb
            AddMode:
                fadd ModeDb
                fadd CombinedDb
                fcomp kSilenceDb
                fnstsw ax
                test ah, 41h
                setnp bSilent
            }
            if (bSilent != 0)
            {
                MixerScale = kMixerNoScore;
                return;
            }
        }

        if (Is3d() == 0)
        {
            MixerScale = AttenuationDb <= kSilenceDb ? kMixerNoScore : 100.f;
            return;
        }

        FVec3 Pos;
        GetPosition(&Pos);
        float DistSq;
        // No intermediate float stores: baseline adds Z squared, Y squared, then X squared.
        __asm
        {
            mov eax, Listener
            lea ecx, Pos
            fld dword ptr [eax]
            fsub dword ptr [ecx]
            fld dword ptr [eax + 4]
            fsub dword ptr [ecx + 4]
            fld dword ptr [eax + 8]
            fsub dword ptr [ecx + 8]
            fld st(0)
            fmul st(0), st(1)
            fld st(2)
            fmul st(0), st(3)
            faddp st(1), st(0)
            fld st(3)
            fmul st(0), st(4)
            faddp st(1), st(0)
            fstp DistSq
            fstp st(0)
            fstp st(0)
            fstp st(0)
        }

        float MaxRange;
        if (GetMaxDistance(&MaxRange) == GR_OK)
        {
            unsigned char bOutside;
            __asm
            {
                fld MaxRange
                fmul MaxRange
                fcomp DistSq
                fnstsw ax
                test ah, 5
                setnp bOutside
            }
            if (bOutside != 0)
            {
                MixerScale = kMixerNoScore;
                return;
            }
        }

        float MinRange;
        if (GetMinDistance(&MinRange) != GR_OK)
            MinRange = 1.f;
        unsigned Bits;
        memcpy(&Bits, &DistSq, sizeof(Bits));
        Bits = 0x5f3759dfu - ((Bits >> 1) | (Bits & 0x80000000u));
        float Guess;
        memcpy(&Guess, &Bits, sizeof(Guess));
        float const Half = 0.5f;
        float const ThreeHalves = 1.5f;
        double const Ten = 10.0;
        float Score;
        // A float inverse-sqrt intermediate or libm log10 changes baseline's final rounding.
        __asm
        {
            fld MinRange
            fld Guess
            fmul Guess
            fmul DistSq
            fmul Half
            fsubr ThreeHalves
            fmul Guess
            fmul st(0), st(1)
            fldlg2
            fxch st(1)
            fyl2x
            fmul Ten
            fstp st(1)
            fstp Score
        }
        MixerScale = Score;
    }

    // (thiscall, no arguments) -- push a streamed source's volume to its stream.
    // Nothing while +0x30 is 0 (only 0 is tested here) or the archetype is null. The sum is
    // GetAttenuation's, computed inline and NOT clamped to [-96, 0]: the category term
    // (master [+ duck]) + mode, + the arch slot 6 FILD, + the +0x1c dB (read before the arch
    // call), all on the FPU, times 100.0 (double the field) and stored as a float. Below
    // -9600 (the field; `TEST AH,5 / JP`: only "less") the volume is -9999 (0xffffd8f1);
    // above 0 (`TEST AH,0x41 / JNZ`: only "greater") it is 0; otherwise ftol. Then IStreamer2
    // slot 8 (+0x20) SetVolume with no test of NeuronRuntime::Streamer. Callers: the branch,
    // , the routine.
    void StreamedApplyVolume(FStreamedSource* Source)
    {
        if (Source->StreamHandle == nullptr)
            return;
        float const SourceDb = Source->AttenuationDb;
        if (Source->Arch == nullptr)
            return;
        double const ArchDb = static_cast<double>(Source->Arch->GetAttenuation());
        double Base = 0.0;
        double ModeDb = 0.0;
        SourceCategoryDb(Source, &Base, &ModeDb);
        double const Sum = ((Base + ModeDb) + ArchDb) + static_cast<double>(SourceDb);
        float const Hundredths = static_cast<float>(Sum * 100.0);
        long Volume = 0;
        if (Hundredths < -9600.f)
            Volume = -9999;
        else if (Hundredths > 0.f)
            Volume = 0;
        else
            Volume = FtolLow32(Hundredths);
        NeuronRuntime::Streamer->SetVolume(Source->StreamHandle, Volume);
    }

    // (slot 42, RET 8): slot 31 with the listener, then slot 29 -- finished
    // answers false. Otherwise the fade: with +0x38 not below zero (`TEST AH,5 / JNP`: only
    // "less" skips) the time left drops by Dt (FSUB, stored as a float, the x87 value kept for
    // the test and the blend). At or below zero (`TEST AH,0x41 / JP`: "greater" and NaN blend)
    // it becomes -1 and the dB is the target (+0x34); otherwise the power-ratio blend from the
    // +0x1c dB to the target. The result, below -96.0 (double the field) -96, above 0.0
    // (double the field) 0, goes to +0x1c. Then the routine and true.
    unsigned char __thiscall FStreamedSource::Update(const float* Listener, float DeltaSeconds)
    {
        ComputeMixerScore(Listener);
        if (IsFinished() != 0)
            return 0;
        float const FromDb = AttenuationDb;
        float const ToDb = FadeTargetDb;
        if (!(FadeTime < 0.f))
        {
            double const NewFade =
                static_cast<double>(FadeTime) - static_cast<double>(DeltaSeconds);
            FadeTime = static_cast<float>(NewFade);
            double Result = 0.0;
            if (!(NewFade > 0.0))
            {
                FadeTime = -1.f;
                Result = static_cast<double>(ToDb);
            }
            else
            {
                Result = DbBlend(FromDb, ToDb, DeltaSeconds, NewFade);
            }
            if (Result < -96.0)
                AttenuationDb = kSilenceDb;
            else if (Result > 0.0)
                AttenuationDb = 0.f;
            else
                AttenuationDb = static_cast<float>(Result);
        }
        StreamedApplyVolume(this);
        return 1;
    }

    // the mixer's ranking predicate (VC6 std::sort's less): A goes before B
    // when both are non-null and B's score (+0x28) is below A's -- FCOMPP of B against A,
    // `TEST AH,5 / JP`, so only "less" is true and a NaN compares false. The same test is
    // inlined all through the routine / the routine / the routine.
    bool MixerRanksBefore(FSoundSource const* A, FSoundSource const* B)
    {
        return A != nullptr && B != nullptr && B->MixerScale < A->MixerScale;
    }

    // VC6 _Unguarded_insert(Last, Value): every predecessor Value ranks before
    // moves up one place; the first one it does not rank before stops the walk (unguarded:
    // the caller guarantees one), and Value takes the hole.
    void MixerUnguardedInsert(FSoundSource** Last, FSoundSource* Value)
    {
        FSoundSource** Hole = Last;
        FSoundSource** Prev = Last;
        for (;;)
        {
            --Prev;
            if (!MixerRanksBefore(Value, *Prev))
                break;
            *Hole = *Prev;
            Hole = Prev;
        }
        *Hole = Value;
    }

    // VC6 _Insertion_sort(First, Last): an entry that ranks before the first
    // shifts the whole prefix up (copy_backward) and goes to the front; any other one takes the
    // unguarded insert.
    void MixerInsertionSort(FSoundSource** First, FSoundSource** Last)
    {
        if (First == Last)
            return;
        for (FSoundSource** Mid = First + 1; Mid != Last; ++Mid)
        {
            FSoundSource* const Value = *Mid;
            if (MixerRanksBefore(Value, *First))
            {
                for (FSoundSource** Move = Mid; Move != First; --Move)
                    *Move = *(Move - 1);
                *First = Value;
            }
            else
            {
                MixerUnguardedInsert(Mid, Value);
            }
        }
    }

    // VC6 _Sort(First, Last): while more than 16 entries remain, partition
    // around the median of the first, middle (count / 2) and last entries (VC6 _Median, the
    // predicate order of 0x0042a855..0x0042a8cc) with the unguarded partition
    // (0x0042a8d4..0x0042a91b), recurse into the part that is not larger -- the byte distances
    // compared signed (JG) -- and loop on the other.
    void MixerIntroSort(FSoundSource** First, FSoundSource** Last)
    {
        int Count = static_cast<int>(Last - First);
        while (Count > 16)
        {
            FSoundSource* const A = *First;
            FSoundSource* const B = First[Count / 2];
            FSoundSource* const C = *(Last - 1);
            FSoundSource* Pivot = nullptr;
            if (MixerRanksBefore(A, B))
            {
                if (MixerRanksBefore(B, C))
                    Pivot = B;
                else if (MixerRanksBefore(A, C))
                    Pivot = C;
                else
                    Pivot = A;
            }
            else if (MixerRanksBefore(A, C))
            {
                Pivot = A;
            }
            else if (MixerRanksBefore(B, C))
            {
                Pivot = C;
            }
            else
            {
                Pivot = B;
            }

            FSoundSource** Lo = First;
            FSoundSource** Hi = Last;
            for (;;)
            {
                while (MixerRanksBefore(*Lo, Pivot))
                    ++Lo;
                do
                {
                    --Hi;
                } while (MixerRanksBefore(Pivot, *Hi));
                if (Hi <= Lo)
                    break;
                FSoundSource* const Swap = *Lo;
                *Lo = *Hi;
                *Hi = Swap;
                ++Lo;
            }

            FSoundSource** const Mid = Lo;
            if ((Last - Mid) > (Mid - First))
            {
                MixerIntroSort(First, Mid);
                First = Mid;
            }
            else
            {
                MixerIntroSort(Mid, Last);
                Last = Mid;
            }
            Count = static_cast<int>(Last - First);
        }
    }

    // of the routine: VC6 std::sort over the field[0..count) --
    // above 16 entries (byte span > 0x40, signed JG) the routine, the routine on the first
    // 16 and the routine for each of the rest; otherwise the routine alone.
    void MixerSortRanked(FSoundSource** First, FSoundSource** Last)
    {
        if ((Last - First) > 16)
        {
            MixerIntroSort(First, Last);
            MixerInsertionSort(First, First + 16);
            for (FSoundSource** It = First + 16; It != Last; ++It)
                MixerUnguardedInsert(It, *It);
        }
        else
        {
            MixerInsertionSort(First, Last);
        }
    }

    // the static initializer: the head node from operator new(0xc), linked to
    // itself, size 0 (and the branch registered with atexit -- here the destructor).
    FPlayingSourceList::FPlayingSourceList()
    {
        Head = static_cast<FPlayingSourceNode*>(::operator new(sizeof(FPlayingSourceNode)));
        Head->Next = Head;
        Head->Prev = Head;
        Size = 0;
    }

    // every node still in the list is unlinked and freed (the routine), the
    // size counting down with it, then the head; head and size end up zero. The sources the
    // nodes named are not touched.
    FPlayingSourceList::~FPlayingSourceList()
    {
        FPlayingSourceNode* Node = Head->Next;
        while (Node != Head)
        {
            FPlayingSourceNode* const Erased = Node;
            Node = Node->Next;
            Erased->Prev->Next = Erased->Next;
            Erased->Next->Prev = Erased->Prev;
            ::operator delete(Erased);
            --Size;
        }
        ::operator delete(Head);
        Head = nullptr;
        Size = 0;
    }

    // The push_back the routine inlines twice (0x00429265..0x004292ab / 0x004292d4..
    // ): a node made before the head (the list's _Buynode the routine(head,
    // head->prev)), linked in, the value constructed in it (the routine), the size + 1. The
    // source's +0x2c then takes head->prev, which is that node.
    FPlayingSourceNode* PlayingListPushBack(FSoundSource* Source)
    {
        FPlayingSourceNode* const Head = G_PlayingSources.Head;
        FPlayingSourceNode* const Node =
            static_cast<FPlayingSourceNode*>(::operator new(sizeof(FPlayingSourceNode)));
        Node->Next = Head;
        Node->Prev = Head->Prev;
        Head->Prev = Node;
        Node->Prev->Next = Node;
        Node->Value = Source;
        ++G_PlayingSources.Size;
        return G_PlayingSources.Head->Prev;
    }

    // The erase every stop path inlines (0x00429412..0x00429434 and its copies in
    // , the routine and the routine): prev->next = next, next->prev = prev, the
    // node freed (the routine), the size - 1.
    void PlayingListErase(FPlayingSourceNode* Node)
    {
        Node->Prev->Next = Node->Next;
        Node->Next->Prev = Node->Prev;
        ::operator delete(Node);
        --G_PlayingSources.Size;
    }

    // What every stop path does once the source is known to be playing: under flag 4 the
    // archetype is released (its slot 4) and, flag 4 re-tested, SoundManager slot 37 (+0x94)
    // detaches this play key; then the node goes, +0x2c is cleared and the reference the list
    // held is released (vt+8).
    void SourceStopLinked(FSoundSource* Source)
    {
        if ((Source->Flags & 4u) != 0)
        {
            Source->Arch->Release();
            if ((Source->Flags & 4u) != 0)
                NeuronRuntime::Sound->DetachPlayKey(Source);
        }
        PlayingListErase(Source->ListNode);
        Source->ListNode = nullptr;
        Source->Release();
    }

    // The Sound::Source destructor body the routine (inlined again in the routine), shared by
    // every slot 41 so a derived class (CliThrusterSound the routine, the engine emitter
    // ) can run it and then free its own block: a source that still has a node
    // (the routine called through the Sound::Source table, 0x0042914a) takes the stop body,
    // then the field drops by one. baseline has reinstalled the Sound::Source table first, so
    // the stop body's vt+8 is always the routine; every class here shares that Release, and the
    // count is already at zero on this path, so the call only takes it to -1.
    void SourceTeardown(FSoundSource* Source)
    {
        if (Source->ListNode != nullptr)
            SourceStopLinked(Source);
        --G_AliveSoundSourceCount;
    }

    // (slot 24, bare RET) -- listing 004293e0..00429448. Only a playing source
    // (slot 5, vt+0x14) does anything; see SourceStopLinked. A Stop on an idle source is a
    // no-op.
    void SourceStopBody(FSoundSource* Source)
    {
        if (Source->IsActive() == 0)
            return;
        SourceStopLinked(Source);
    }

    // The dword right after the vptr of an Archetype::ISound: FClientSoundArch::NicknameId and
    // FVoiceMsg::Id (voice_profiles.h) both sit at +4. the routine compares the two
    // archetypes by this dword (MOV EDX,[EDI+4] / CMP EDX,[ECX+4]), not by pointer.
    unsigned ArchIdentity(Archetype::ISound* Arch)
    {
        return *reinterpret_cast<unsigned const*>(reinterpret_cast<char const*>(Arch) + 4);
    }

    // (slot 22, RET 8) -- listing 004291c0..00429328.
    unsigned char SourceStart(FSoundSource* Source, Archetype::ISound* Arch, int Extra)
    {
        if (Source->IsActive() != 0) // vt+0x14
        {
            // Same archetype already playing: only move the start offset (vt+0x64, slot 25).
            if (ArchIdentity(Arch) == ArchIdentity(Source->Arch))
            {
                Source->SetStartOffset(Extra);
                return 1;
            }
            // Different archetype: stop (vt+0x60, slot 24), warn, then start the new one.
            Source->Stop();
            FDUMP(0x100002, "%s(%d) : *** WARNING: attempted to re-play active sound (%s)",
                  kSoundCpp, 0x557, Arch->GetFileName());
        }
        if (Arch == nullptr)
            return 0;
        Archetype::ISound* const Linked = Arch->GetLinked(); // slot 12
        Source->Arch = Linked;
        if (Linked == nullptr)
            return 0;
        Source->Type = static_cast<int>(Linked->GetSourceType()); // slot 2
        Source->StartTick = Extra;
        Source->Flags = static_cast<unsigned char>(Source->Flags & 0xe7u);
        // Flag 4 (SoundManager-played) first needs NeuronRuntime::Sound and the archetype's slot 3;
        // either failing answers false before anything is linked (0x004292b4..0x004292c8).
        if ((Source->Flags & 4u) != 0)
        {
            if (NeuronRuntime::Sound == nullptr)
                return 0;
            if (Source->Arch->Acquire() == 0)
                return 0;
        }
        // AddRef (vt+4) for the list, then the push_back; +0x2c = the new node. Playing it is
        // the mixer's job (the routine hands the ranked sources to SoundManager).
        Source->AddRef();
        Source->ListNode = PlayingListPushBack(Source);
        return 1;
    }

    // (thiscall, RET 4) -- the base Sound::Source constructor: vtable, +0x04 /
    // +0x08 / +0x10 .. +0x20 zero, the flag byte's top three bits kept and bit 2 = bBit2 & 1,
    // +0x24 = 1, +0x28 = the field (FLT_MAX), +0x2c = 0, then INC the field.
    FSoundSource::FSoundSource(unsigned char bBit2, unsigned char OldFlags)
        : Type(0), Field08(0),
          Flags(static_cast<unsigned char>((OldFlags & 0xe0u) | ((bBit2 & 1u) << 2))),
          Pitch(0.f), StartTick(0), Arch(nullptr), AttenuationDb(0.f), Pan(0.f), RefCount(1),
          MixerScale(kMixerNoScore), ListNode(nullptr)
    {
        ++G_AliveSoundSourceCount;
    }

    // (thiscall) -- the routine(0), +0x30 / +0x34 = 0, +0x38 = -1.0f,
    // +0x3c = 0, the Sound::StreamedSource table 0x005ca05c.
    FStreamedSource::FStreamedSource(unsigned char OldFlags)
        : FSoundSource(0, OldFlags), StreamHandle(nullptr), FadeTargetDb(0.f), FadeTime(-1.f),
          bFadeOut(0)
    {
    }

    // (thiscall) -- the routine(1), +0x30 .. +0x5c zero but +0x44 = 1.0f (the cone
    // direction's z) and +0x58 = 360.0f (the outer cone), +0x60 = -1, the Sound::Source3D table
    // . The engine emitter's ctor the routine inlines the same stores.
    FSpatialSource::FSpatialSource(unsigned char OldFlags)
        : FSoundSource(1, OldFlags), Source3dId(-1)
    {
        for (int Index = 0; Index < 12; ++Index)
            Spatial[Index] = 0.f;
        Spatial[5] = 1.f;
        Spatial[10] = 360.f;
    }

    // The flag byte a fresh block already holds (+0x0c): the routine keeps its top three
    // bits, and neither the pools nor malloc clear memory.
    unsigned char BlockFlagByte(void const* Block)
    {
        return static_cast<unsigned char const*>(Block)[0xc];
    }

    // (cdecl) -- the sources' operator new: 0x30 / 0x40 / 0x64 bytes come from
    // their Heap pool (the field / the field / the field, JMP Heap::Malloc), any
    // other size from malloc (the 0x50 CliThrusterSound and the 0x68 engine emitter).
    void* SoundSourceAlloc(unsigned Size)
    {
        if (Size == 0x30u)
            return G_SourceHeap.Malloc();
        if (Size == 0x40u)
            return G_StreamedHeap.Malloc();
        if (Size == 0x64u)
            return G_Source3DHeap.Malloc();
        return malloc(Size);
    }

    // (cdecl Ptr, Size) -- the matching operator delete: Heap::Free on the pool of
    // that size, free() for any other.
    void SoundSourceFree(void* Ptr, unsigned Size)
    {
        if (Size == 0x30u)
        {
            G_SourceHeap.Free(Ptr);
            return;
        }
        if (Size == 0x40u)
        {
            G_StreamedHeap.Free(Ptr);
            return;
        }
        if (Size == 0x64u)
        {
            G_Source3DHeap.Free(Ptr);
            return;
        }
        free(Ptr);
    }

    // --- Sound::StreamedSource (vtable 0x005ca05c) ---

    // (thiscall Arch, Offset; RET 8). With NeuronRuntime::Streamer and a stream handle
    // (neither 0 nor -1): the routine(Arch, Offset) called directly, not through the table; on
    // success the routine, IStreamer2 slot 7 (+0x1c) StartStream and slot 10 (+0x28)
    // QueryStatus, a zero status being the "Bad streamer status" warning with the archetype's
    // slot 1 name (Sound.cpp:0x905). The answer is only whether +0x30 is a handle.
    unsigned char StreamedRestart(FStreamedSource* Source, Archetype::ISound* Arch, int Offset)
    {
        if (NeuronRuntime::Streamer != nullptr && IsStreamHandle(Source->StreamHandle))
        {
            if (SourceStart(Source, Arch, Offset) != 0)
            {
                StreamedApplyVolume(Source);
                NeuronRuntime::Streamer->StartStream(Source->StreamHandle);
                if (NeuronRuntime::Streamer->QueryStatus(Source->StreamHandle) == 0)
                {
                    FDUMP(0x100002,
                          "%s(%d) : *** WARNING: Bad streamer status upon starting sound %s",
                          kSoundCpp, 0x905, Arch->GetFileName());
                }
            }
        }
        return IsStreamHandle(Source->StreamHandle) ? 1u : 0u;
    }

    // slot 22 (RET 8): slot 23 (vt+0x5c, open the stream) with Arch, then
    // (Arch, Extra); true when +0x30 ends up a handle.
    unsigned char __thiscall FStreamedSource::Start(void* ArchArg, int Extra)
    {
        Archetype::ISound* const SoundArch = static_cast<Archetype::ISound*>(ArchArg);
        AltStart(SoundArch);
        StreamedRestart(this, SoundArch, Extra);
        return IsStreamHandle(StreamHandle) ? 1u : 0u;
    }

    // slot 23 (RET 4), open the stream. No NeuronRuntime::Streamer: true. Otherwise
    // IStreamer2 slot 4 (+0x10) CreateStream(arch slot 1 name, NeuronRuntime::Data, 2 when slot 7
    // says looping, else 0) into +0x30; 0 or -1 is the "Failed to start streamer sound"
    // warning (Sound.cpp:0x8e5) and false.
    unsigned char __thiscall FStreamedSource::AltStart(void* ArchArg)
    {
        Archetype::ISound* const SoundArch = static_cast<Archetype::ISound*>(ArchArg);
        if (NeuronRuntime::Streamer == nullptr)
            return 1;
        unsigned long const Mode = IsLooping() != 0 ? 2u : 0u;
        char const* const Name = SoundArch->GetFileName();
        StreamHandle = NeuronRuntime::Streamer->CreateStream(Name, NeuronRuntime::Data, Mode);
        if (!IsStreamHandle(StreamHandle))
        {
            FDUMP(0x100002, "%s(%d) : *** WARNING: Failed to start streamer sound %s", kSoundCpp,
                  0x8e5, SoundArch->GetFileName());
            return 0;
        }
        return 1;
    }

    // slot 24 (bare RET). A non-zero +0x30 (only 0 is tested) is stopped and
    // destroyed (IStreamer2 slots 6 and 5, no test of NeuronRuntime::Streamer) and cleared; +0x34 = 0,
    // +0x38 = -1.0f; then, when slot 5 says playing, the common stop body.
    void __thiscall FStreamedSource::Stop()
    {
        if (StreamHandle != nullptr)
        {
            NeuronRuntime::Streamer->StopStream(StreamHandle);
            NeuronRuntime::Streamer->DestroyStream(StreamHandle);
            StreamHandle = nullptr;
        }
        FadeTargetDb = 0.f;
        FadeTime = -1.f;
        if (IsActive() == 0)
            return;
        SourceStopLinked(this);
    }

    // slot 25 (RET 4): a streamed source cannot be moved; the ERROR names
    // Sound.h:0x13f.
    void __thiscall FStreamedSource::SetStartOffset(int /*Offset*/)
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: %s", kSoundH, 0x13f,
              "Tried to restart streaming sound");
    }

    // slot 26: not for the background music (the field), nor without
    // NeuronRuntime::Streamer or a stream handle; otherwise +0x08 = 1 and IStreamer2 slot 6 StopStream.
    void __thiscall FStreamedSource::Pause()
    {
        if (this == G_BackgroundMusic)
            return;
        if (NeuronRuntime::Streamer == nullptr)
            return;
        if (!IsStreamHandle(StreamHandle))
            return;
        Field08 = 1;
        NeuronRuntime::Streamer->StopStream(StreamHandle);
    }

    // slot 27: the same gates, then IStreamer2 slot 7 StartStream and +0x08 = 0.
    void __thiscall FStreamedSource::Resume()
    {
        if (this == G_BackgroundMusic)
            return;
        if (NeuronRuntime::Streamer == nullptr)
            return;
        if (!IsStreamHandle(StreamHandle))
            return;
        NeuronRuntime::Streamer->StartStream(StreamHandle);
        Field08 = 0;
    }

    // slot 29: no stream handle -> finished. With one: slot 40 (faded out) ->
    // finished; IStreamer2 slot 10 QueryStatus 0 or 4 -> finished; any other status goes on to
    // the base the routine (JMP).
    unsigned char __thiscall FStreamedSource::IsFinished()
    {
        if (!IsStreamHandle(StreamHandle))
            return 1;
        if (Slot40() != 0)
            return 1;
        unsigned long const Status = NeuronRuntime::Streamer->QueryStatus(StreamHandle);
        if (Status == 4 || Status == 0)
            return 1;
        return SourceIsFinished(this);
    }

    // slot 32 (RET 4): below -96.0 (double the field, `TEST AH,5 / JP`)
    // -96; above 0.0 (double the field, `TEST AH,0x41 / JNZ`) 0; otherwise the value. It
    // goes to both +0x1c and the fade target +0x34, the fade time +0x38 = 0.0, then
    // .
    void __thiscall FStreamedSource::SetAttenuation(float Attenuation)
    {
        float Value = Attenuation;
        if (Attenuation < -96.f)
            Value = kSilenceDb;
        else if (Attenuation > 0.f)
            Value = 0.f;
        AttenuationDb = Value;
        FadeTargetDb = Value;
        FadeTime = 0.f;
        StreamedApplyVolume(this);
    }

    // slot 33 (RET 4): the pan clamped to [-10000, 10000] (the field
    // `TEST AH,5 / JP`, the field `TEST AH,0x41 / JNZ`) into +0x20; with a non-zero +0x30,
    // slot 11 reads it back as an integer, clamped again (signed JGE / JLE), and IStreamer2
    // slot 13 (+0x34) SetPan gets it.
    void __thiscall FStreamedSource::SetPan(float NewPan)
    {
        float Value = NewPan;
        if (NewPan < -10000.f)
            Value = -10000.f;
        else if (NewPan > 10000.f)
            Value = 10000.f;
        Pan = Value;
        if (StreamHandle == nullptr)
            return;
        long PanOut = 0;
        GetPan(&PanOut);
        if (PanOut < -10000)
            PanOut = -10000;
        else if (PanOut > 10000)
            PanOut = 10000;
        NeuronRuntime::Streamer->SetPan(StreamHandle, PanOut);
    }

    // slot 34 (RET 4): nothing when slot 7 already answers bLooping (a byte
    // compare). A playing source (slot 5) switched to looping is the "attributes changing
    // _after_ the stream was started" ERROR (Sound.cpp:0x8bf, the archetype's slot 1 name);
    // either way bit 0 of the flags takes bLooping's bit 0.
    void __thiscall FStreamedSource::SetLooping(unsigned char bLooping)
    {
        if (bLooping == IsLooping())
            return;
        if (IsActive() != 0 && bLooping != 0)
        {
            FDUMP(0x100001,
                  "%s(%d) : *** ERROR: streamed source %s attributes changing _after_ the stream "
                  "was started",
                  kSoundCpp, 0x8bf, Arch->GetFileName());
        }
        Flags = static_cast<unsigned char>(((Flags ^ bLooping) & 1u) ^ Flags);
    }

    // slot 40 (bare RET): faded out -- +0x3c set and the +0x1c dB at or below
    // the +0x34 target (FCOMP, `TEST AH,0x41 / JNP`: "less" or "equal"; a NaN is false).
    unsigned char __thiscall FStreamedSource::Slot40()
    {
        if (bFadeOut == 0)
            return 0;
        return AttenuationDb <= FadeTargetDb ? 1u : 0u;
    }

    int OpenWavFileSystem(char const* RelPath, void* ParentFs, void** OutFs)
    {
        if (OutFs != nullptr)
            *OutFs = nullptr;
        // Both candidates are IFileSystem, which derives from IComponentFactory -- slot 3
        // (+0x0C) is plain CreateInstance, the same one NeuronRuntime::OpenData goes through.
        // . Only caller is CreateSoundArchWav, which has already required
        // NeuronRuntime::Data; RelPath goes in untested, as baseline's EBP does.
        IComponentFactory* const Factory = ParentFs != nullptr
            ? static_cast<IComponentFactory*>(static_cast<IFileSystem*>(ParentFs))
            : static_cast<IComponentFactory*>(NeuronRuntime::Data);

        FFileSystemAggDesc Desc;
        memset(&Desc, 0, sizeof(Desc));
        Desc.Size = 0x34;
        Desc.ComponentName = "FileSystem";
        Desc.BackendName = ParentFs != nullptr ? "UTF" : "DOS";
        Desc.DataPath = RelPath;
        Desc.Field10 = 0x80000000u;
        Desc.Field14 = 1;
        Desc.Field1C = 3;
        Desc.Field20 = 0x8000080u;
        Desc.Field30 = 0xffffffffu;

        return Factory->CreateInstance(reinterpret_cast<AGGDESC*>(&Desc), OutFs);
    }

    void ReleaseComponent(void* Component)
    {
        if (Component == nullptr)
            return;
        static_cast<INeuronComponent*>(Component)->Release();
    }

    // itself is CreateSoundArchWav, at file scope below (VoiceMsg::Acquire in
    // voice_profiles.cpp calls it too).

    // SoundImpl slot 3.
    unsigned char ArchAcquire(FClientSoundArch* Arch)
    {
        // slot 12 (the lod link) against this.
        FClientSoundArch* Linked = ArchGetLinked(Arch);
        if (Linked == Arch)
        {
            // no SoundManager id yet and a [NeuronRuntime::Sound] -> the routine
            // (this, 0) into +0xc; then INC word [+0x2a]; SETNZ of +0xc != -1.
            if (Arch->SmArchId == -1 && NeuronRuntime::Sound != nullptr)
                Arch->SmArchId = CreateSoundArchWav(Arch, nullptr);
            ++Arch->UseCount;
            return Arch->SmArchId != -1 ? 1 : 0;
        }
        // a link -> its slot 3 (tail jump).
        if (Arch->Linked != nullptr)
            return ArchAcquire(Arch->Linked);
        // no link -- INC word [+0x28], AL = 1.
        ++Arch->Pad28;
        return 1;
    }

    // SoundImpl slot 4. Returns the remaining count sign-extended from its word
    // (MOVSX), not a flag.
    int ArchReleaseWav(FClientSoundArch* Arch)
    {
        // slot 12 (the lod link) against this.
        FClientSoundArch* Linked = ArchGetLinked(Arch);
        if (Linked == Arch)
        {
            // a non-zero +0x2a is decremented.
            if (Arch->UseCount != 0)
                --Arch->UseCount;
            // CMP word [+0x2a],0 / JG (signed) -- at or below zero and
            // with a SoundManager id, [NeuronRuntime::Sound] slot 34 (+0x88) destroys it (no null test)
            // and +0xc = -1.
            if (static_cast<short>(Arch->UseCount) <= 0 && Arch->SmArchId != -1)
            {
                NeuronRuntime::Sound->DestroyArchetype(Arch->SmArchId);
                Arch->SmArchId = -1;
            }
            return static_cast<short>(Arch->UseCount);
        }
        // a link -> its slot 4 (tail jump).
        if (Arch->Linked != nullptr)
            return ArchReleaseWav(Arch->Linked);
        // no link -- the +0x28 word, decremented when non-zero.
        if (Arch->Pad28 != 0)
            --Arch->Pad28;
        return static_cast<short>(Arch->Pad28);
    }

    // The field copy both arms of the routine share (0x0042bcc6..0x0042bdb1 /
    // ): +0x04 .. +0x14 as dwords, the +0x18 flag word bit-field by
    // bit-field except bits 30..31, which stay the target's, the two lod ids, the lod link and
    // the two words at +0x28 / +0x2a. The port's +0x08 file is a malloc'd copy (baseline: a
    // StringAlloc'd CacheString, never freed), so it moves from Src instead of being shared.
    // Nickname is the port's own copy of the nickname text and follows the id.
    void SoundArchCopyFields(FClientSoundArch* This, FClientSoundArch* Src)
    {
        This->NicknameId = Src->NicknameId;
        if (This->File != Src->File)
        {
            if (This->File != nullptr)
                free(This->File);
            This->File = Src->File;
            Src->File = nullptr;
        }
        This->SmArchId = Src->SmArchId;
        This->RangeMin = Src->RangeMin;
        This->RangeMax = Src->RangeMax;
        This->Flags = (Src->Flags & 0x3fffffffu) | (This->Flags & 0xc0000000u);
        This->Lod[0] = Src->Lod[0];
        This->Lod[1] = Src->Lod[1];
        This->Linked = Src->Linked;
        This->Pad28 = Src->Pad28;
        This->UseCount = Src->UseCount;
        memcpy(This->Nickname, Src->Nickname, sizeof(This->Nickname));
    }

    // (thiscall on the registered SoundArch, RET 8) -- redefine it from Src. The
    // same file (the inlined strcmp at 0x0042bc86..0x0042bcbb, case-sensitive) without bForce
    // keeps the loaded wav: every field is copied but +0x0c SmArchId is put back. Otherwise the
    // outstanding uses (slot 5, +0x14) are released one by one (slot 4, +0x10), everything is
    // copied, and the same number of uses is taken again (slot 3, +0x0c). Always true.
    bool SoundArchRedefine(FClientSoundArch* This, FClientSoundArch* Src, bool bForce)
    {
        char const* const ThisFile = This->File != nullptr ? This->File : "";
        char const* const SrcFile = Src->File != nullptr ? Src->File : "";
        if (!bForce && strcmp(ThisFile, SrcFile) == 0)
        {
            int const KeepSmArchId = This->SmArchId;
            SoundArchCopyFields(This, Src);
            This->SmArchId = KeepSmArchId;
            return true;
        }
        int const Uses = This->GetUseCount();
        for (int Index = 0; Index < Uses; ++Index)
            This->Release();
        SoundArchCopyFields(This, Src);
        for (int Index = 0; Index < Uses; ++Index)
            This->Acquire();
        return true;
    }

    // (cdecl Id, Arch, bForce; caller the loader 0x0042b283 with the loader's own
    // second argument) -- register a parsed SoundArch. A non-empty id already present: the
    // same nickname text (stricmp of the two ID_String::get_string results) warns "Redefining"
    // and redefines the registered one through the routine; different texts under one id are a
    // collision ERROR and nothing is registered (null). Otherwise (a new id, or an empty one)
    // the set's insert the routine copies Arch in. Returns the registered SoundArch. The port
    // takes Arch's malloc'd file over on success.
    FClientSoundArch* RegisterSoundArch(FClientSoundArch* Arch, bool bForce)
    {
        ID_String Id;
        Id.Id = Arch->NicknameId;
        if (!Id.IsEmpty())
        {
            // the scratch key the field (+0x04 = the id) looked up
            // with the routine.
            std::map<unsigned, FClientSoundArch*>::iterator Found = G_SoundArchs.find(Id.Id);
            if (Found != G_SoundArchs.end())
            {
                FClientSoundArch* const Existing = Found->second;
                ID_String ExistingId;
                ExistingId.Id = Existing->NicknameId;
                // stricmp(existing text, new text).
                if (_stricmp(ExistingId.get_string(), Id.get_string()) != 0)
                {
                    // .
                    FDUMP(0x100001, "%s(%d) : *** ERROR: Sound nickname collision (%s) and (%s)\n",
                          kSoundArchCpp, 0x182, ExistingId.get_string(), Id.get_string());
                    return nullptr;
                }
                // .
                FDUMP(0x100002, "%s(%d) : *** WARNING: Redefining sound '%s'.\n", kSoundArchCpp,
                      0x17a, Id.get_string());
                SoundArchRedefine(Existing, Arch, bForce);
                return Existing;
            }
        }
        // insert (a present key keeps its entry) and the node's value.
        FClientSoundArch* const Copy = new FClientSoundArch(*Arch);
        std::pair<std::map<unsigned, FClientSoundArch*>::iterator, bool> Inserted =
            G_SoundArchs.insert(std::pair<unsigned const, FClientSoundArch*>(Id.Id, Copy));
        if (!Inserted.second)
        {
            Copy->File = nullptr;
            delete Copy;
            return Inserted.first->second;
        }
        Arch->File = nullptr;
        return Copy;
    }

    bool ParseSoundType(FClientSoundArch* Arch, char const* Value)
    {
        // baseline table (PTR_s_normal_00610988): id order matches the audio_* category
        // mapping (sfx=0/interface=1/voice=2/music=3/ambient=4) documented above.
        static const char* const kTypes[] = {"normal", "interface", "voice", "music", "ambience"};
        static const unsigned kTypeIds[] = {0, 1, 2, 3, 4};
        int Index = 0;
        for (Index = 0; Index < 5; ++Index)
        {
            if (_stricmp(Value, kTypes[Index]) == 0)
            {
                Arch->Flags = ((kTypeIds[Index] << 16) ^ Arch->Flags) & 0xf0000 ^ Arch->Flags;
                return true;
            }
        }
        return false;
    }

    bool ParseSoundValue(FClientSoundArch* Arch, INI_Reader& Reader)
    {
        if (Reader.is_value("file"))
        {
            char const* Path = Reader.get_value_string();
            if (Arch->File != nullptr)
                free(Arch->File);
            Arch->File = Path != nullptr ? _strdup(Path) : nullptr;
            return true;
        }
        if (Reader.is_value("attenuation"))
        {
            // ftol(value 0), low byte into +0x18.
            int const Atten = static_cast<int>(Reader.get_value_float(0));
            Arch->Flags = (Arch->Flags & 0xffffff00u) | (static_cast<unsigned>(Atten) & 0xffu);
            return true;
        }
        if (Reader.is_value("range"))
        {
            Arch->RangeMin = Reader.get_value_float(0);
            if (Reader.is_value_empty(1))
                // baseline multiplier is `_DAT_00610984` = 30.0f (0x41F00000), not 2x.
                Arch->RangeMax = Arch->RangeMin * 30.f;
            else
                Arch->RangeMax = Reader.get_value_float(1);
            return true;
        }
        if (Reader.is_value("persistent"))
        {
            // NEURONCOMPONENTS stricmp against "space" / "gameflow", no test of
            // the value pointer.
            char const* Value = Reader.get_value_string();
            if (_stricmp("space", Value) == 0)
                Arch->Flags = (Arch->Flags & 0xff1fffffu) | 0x100000u;
            else if (_stricmp("gameflow", Value) == 0)
                Arch->Flags = (Arch->Flags & 0xff2fffffu) | 0x200000u;
            else
                FDUMP(0x100002, "%s(%d) : *** WARNING: unknown 'persistent' value %s.\n",
                      "Client\\cmn\\SoundArch.cpp", 0x2fd, Value);
            return true;
        }
        if (Reader.is_value("type"))
        {
            // the PTR_s_normal_00610988 table walk, no test of the value.
            char const* Value = Reader.get_value_string();
            if (!ParseSoundType(Arch, Value))
            {
                FDUMP(0x100002, "%s(%d) : *** WARNING: unknown 'type' value %s.\n",
                      "Client\\cmn\\SoundArch.cpp", 0x30a, Value);
            }
            return true;
        }
        if (Reader.is_value("streamer"))
        {
            unsigned const Bit = Reader.get_value_bool(0) ? 1u : 0u;
            Arch->Flags = (Bit << 26) ^ (Arch->Flags & 0xfbffffffu);
            return true;
        }
        if (Reader.is_value("is_2d"))
        {
            unsigned const Bit = Reader.get_value_bool(0) ? 1u : 0u;
            Arch->Flags = (Bit << 27) ^ (Arch->Flags & 0xf7ffffffu);
            return true;
        }
        if (Reader.is_value("pitch_bendable"))
        {
            unsigned const Bit = Reader.get_value_bool(0) ? 1u : 0u;
            Arch->Flags = (Bit << 24) ^ (Arch->Flags & 0xfeffffffu);
            return true;
        }
        if (Reader.is_value("crv_pitch"))
        {
            Arch->Flags = Arch->Flags | 0x3000000u;
            // baseline also reads the curve's random-pitch range and stores it in the
            // Flags byte at bits 8-15 (consumed by ArchSlot11RandomPitch as `Flags >> 8`).
            float const Pitch = Reader.get_value_float(0);
            unsigned char const PitchByte = static_cast<unsigned char>(static_cast<int>(Pitch));
            Arch->Flags = (Arch->Flags & 0xffff00ffu) | (static_cast<unsigned>(PitchByte) << 8);
            return true;
        }
        // "lod1" -> index 0, "lod2" -> index 1; CreateID of the value
        // (no test) fills Lod[index..1] (REP STOSD of 2 - index dwords from +0x1c).
        int LodIndex = 0;
        if (Reader.is_value("lod1"))
            LodIndex = 0;
        else if (Reader.is_value("lod2"))
            LodIndex = 1;
        else
        {
            // FDUMP with get_name_ptr, then log_link(""), AL = 0.
            FDUMP(0x100002, "%s(%d) : *** WARNING: unknown [Sound] parameter %s.\n",
                  "Client\\cmn\\SoundArch.cpp", 0x329,
                  Reader.get_name_ptr());
            Reader.log_link("");
            return false;
        }
        unsigned const LodId = CreateID(Reader.get_value_string());
        if (LodIndex < 2)
        {
            for (int Fill = LodIndex; Fill < 2; ++Fill)
                Arch->Lod[Fill] = LodId;
        }
        return true;
    }

    // / the routine body: store the global source into *OutSource and AddRef it
    // (428bb7 CALL [EDX+4]); the caller owns that reference and Releases it (StarSys tick
    // ). baseline has no null test on OutSource.
    unsigned char BindSource(FStreamedSource* Source, void** OutSource)
    {
        if (Source == nullptr)
            return 0;
        *OutSource = static_cast<ISoundSource*>(Source);
        Source->AddRef();
        return 1;
    }

    // The shared body of the routine (the field, type 3) / the routine (the field,
    // type 4); baseline's first argument is the ID_String*.
    // the fade time is `music_fade_time` (the field) unless Fade is
    // not below 0.0 (`TEST AH,1 / JNZ`: "less" and NaN keep the default); `cross_fade_silence`
    // (the field) is read with it.
    // with a current source, a non-empty id is looked up
    // (the routine, its 0x168 warning included) and the same archetype pointer as the
    // source's +0x18 keeps it playing (true). Otherwise the current source fades out -- +0x34 =
    // the silence, +0x38 = the fade time, +0x3c = 1 --, is released and the slot cleared.
    // an empty id answers false. Otherwise the stack play key {0,
    // 4 | (Flags & 1), 0, 0, 0, silence} (the byte's bits 3..7 are left over from the stack in
    // baseline; zero here), the routine(the routine(id), key, &handle, 0); on success the
    // handle (__RTDynamicCast to Sound::StreamedSource -- key bit 2 always makes one) takes the
    // slot, +0x04 = the type, +0x34 = 0, +0x38 = the fade time; true.
    unsigned char WriteBackgroundSource(FStreamedSource** Slot, unsigned NicknameId,
                                        unsigned Flags, float Fade, int SourceType)
    {
        ID_String Id;
        Id.Id = NicknameId;

        float const SilenceFloat = GetClientCrossFadeSilence();
        float FadeTime = GetClientMusicFadeTime();
        if (Fade >= 0.f)
            FadeTime = Fade;

        if (*Slot != nullptr)
        {
            if (!Id.IsEmpty())
            {
                void* const Current = LookupClientSoundArch(&Id);
                if (static_cast<void*>((*Slot)->Arch) == Current)
                    return 1;
            }
            (*Slot)->FadeTargetDb = SilenceFloat;
            (*Slot)->FadeTime = FadeTime;
            (*Slot)->bFadeOut = 1;
            (*Slot)->Release();
            *Slot = nullptr;
        }
        if (Id.IsEmpty())
            return 0;

        unsigned PlayKey[6];
        PlayKey[0] = 0;
        PlayKey[1] = 4u | (Flags & 1u);
        PlayKey[2] = 0;
        PlayKey[3] = 0;
        PlayKey[4] = 0;
        memcpy(&PlayKey[5], &SilenceFloat, sizeof(float)); // key +0x14

        void* Handle = nullptr;
        void* const Arch = LookupClientSoundArch(&Id);
        if (PlayClientSoundArch(Arch, PlayKey, &Handle, 0) == 0)
            return 0;
        FStreamedSource* const Source =
            static_cast<FStreamedSource*>(static_cast<ISoundSource*>(Handle));
        *Slot = Source;
        Source->Type = SourceType;
        Source->FadeTargetDb = 0.f;
        Source->FadeTime = FadeTime;
        return 1;
    }

    // The engine equipment's sound emitter (vtable 005dd084, built by the routine into a
    // (0x68) block, i.e. malloc): a Sound::Source3D grown by one field, the
    // archetype it holds a reference on at +0x64. Against Source3D's table (005c9efc) it
    // replaces slot 41 only.
    struct FEquipSoundEmitter : FSpatialSource
    {
        // listing 00527a70..00527af0: the routine(1), the Source3D block
        // (0x00527a94..0x00527ac2, FSpatialSource's stores), +0x64 = Arch, the 005dd084 table,
        // then the archetype's slot 3 (+0x0c, Acquire) with no null test and its result dropped.
        FEquipSoundEmitter(unsigned char OldFlags, Archetype::ISound* Arch)
            : FSpatialSource(OldFlags), EmitterArch(Arch)
        {
            EmitterArch->Acquire();
        }

        // slot 41: the body the routine, then the 0x68 free (the routine)
        // when bit 0 is set; returns this either way.
        void* __thiscall ScalarDeletingDestructor(unsigned char bDelete) override
        {
            // listing 00527b20..00527b7c: the Stop body the routine called
            // directly (0x00527b4b), the +0x64 archetype released through its slot 4
            // (0x00527b55, no null test), then the Source3D destructor, inlined down to the
            // Sound::Source destructor the routine (0x00527b68).
            SourceStopBody(this);
            EmitterArch->Release();
            SourceTeardown(this);
            void* const Self = this;
            if ((bDelete & 1) != 0)
                SoundSourceFree(Self, 0x68u);
            return Self;
        }

        Archetype::ISound* EmitterArch; // baseline +0x64
    };

    static_assert(offsetof(FEquipSoundEmitter, EmitterArch) == 0x64, "emitter arch @ +0x64");
    static_assert(sizeof(FEquipSoundEmitter) == 0x68, "engine emitter is 0x68");

    // CliThrusterSound (RTTI .?AVCliThrusterSound@@, vtable 005ddbfc): a Sound::Source (base
    // ctor the routine(1)) grown to 0x50 (Heap the routine(0x50) / the routine(this, 0x50)).
    // The ship's thrusters as one sound: exterior (3D, at the thrusters' mean position) or,
    // for the player's own ship in the first-person camera, interior (flat). Against
    // Sound::Source's table (005c9fac) it replaces slots 6, 12, 15, 16, 17, 18 and 41.
    struct FThrusterSoundSource : FSoundSource
    {
        // slot 6: `return this->bExterior`.
        unsigned char NEURONCOMPONENTS_API Is3d() override { return bExterior; }
        // slot 12: the thrusters' mean position (+0x34), GR_OK.
        GENRESULT NEURONCOMPONENTS_API GetPosition(FVec3* Out) override
        {
            Out->X = ThrusterPosition[0];
            Out->Y = ThrusterPosition[1];
            Out->Z = ThrusterPosition[2];
            return GR_OK;
        }
        // slot 15: ftol of ThrusterEquipConsts::INSIDE_CONE_ANGLE and
        // OUTSIDE_CONE_ANGLE, GR_OK.
        GENRESULT NEURONCOMPONENTS_API GetConeAngles(unsigned long* Inside, unsigned long* Outside) override
        {
            *Inside = static_cast<unsigned long>(Ftol(ThrusterEquipConsts::INSIDE_CONE_ANGLE));
            *Outside = static_cast<unsigned long>(Ftol(ThrusterEquipConsts::OUTSIDE_CONE_ANGLE));
            return GR_OK;
        }
        // slot 16: the ship's transform (IObjInspect slot 3) third column,
        // rotation dwords +0x08 / +0x14 / +0x20, GR_OK.
        GENRESULT NEURONCOMPONENTS_API GetConeOrientation(FVec3* Out) override
        {
            Transform const& Xform = Ship->get_transform();
            Out->X = Xform.Rotation.E[0][2];
            Out->Y = Xform.Rotation.E[1][2];
            Out->Z = Xform.Rotation.E[2][2];
            return GR_OK;
        }
        // slot 17: ThrusterEquipConsts::OUTSIDE_CONE_ATTENUATION, GR_OK.
        GENRESULT NEURONCOMPONENTS_API GetConeOutsideVolume(float* Out) override
        {
            *Out = ThrusterEquipConsts::OUTSIDE_CONE_ATTENUATION;
            return GR_OK;
        }
        // slot 18: the ship's velocity (IObjInspect slot 1), GR_OK.
        GENRESULT NEURONCOMPONENTS_API GetVelocity(FVec3* Out) override
        {
            Vector const Velocity = Ship->get_velocity();
            Out->X = Velocity.x;
            Out->Y = Velocity.y;
            Out->Z = Velocity.z;
            return GR_OK;
        }
        // slot 41: the body the routine (the exterior, then the interior
        // archetype released through slot 4 when set; then the Sound::Source destructor
        // ), then the 0x50 free when bit 0 is set. Returns this either way.
        void* __thiscall ScalarDeletingDestructor(unsigned char bDelete) override
        {
            if (ExteriorArch != nullptr)
                ExteriorArch->Release();
            if (InteriorArch != nullptr)
                InteriorArch->Release();
            SourceTeardown(this);
            void* const Self = this;
            if ((bDelete & 1) != 0)
                SoundSourceFree(Self, 0x50u);
            return Self;
        }

        // The field stores at the head of the routine (after its the routine(1)); the
        // archetype lookups that follow are in CreateClientThrusterSound.
        FThrusterSoundSource(unsigned char OldFlags, FClientEquippedObj* InShip)
            : FSoundSource(1, OldFlags), Ship(InShip), Field_0x40(0), bExterior(1),
              InteriorArch(nullptr), ExteriorArch(nullptr)
        {
            ThrusterPosition[0] = 0.0f;
            ThrusterPosition[1] = 0.0f;
            ThrusterPosition[2] = 0.0f;
        }

        FClientEquippedObj* Ship;        // baseline +0x30  the ship gameobj (ctor argument)
        float ThrusterPosition[3];       // baseline +0x34  written by the routine
        int Field_0x40;                  // baseline +0x40  zeroed by the ctor; no other access in
                                         // 's slots or 0052f110 -- TODO
        unsigned char bExterior;         // baseline +0x44  1 = exterior (3D), 0 = interior
        Archetype::ISound* InteriorArch; // baseline +0x48  INTERIOR_SOUND_NAME, player ship only
        Archetype::ISound* ExteriorArch; // baseline +0x4c  EXTERIOR_SOUND_NAME
    };

    static_assert(offsetof(FThrusterSoundSource, Ship) == 0x30, "thruster ship @ +0x30");
    static_assert(offsetof(FThrusterSoundSource, bExterior) == 0x44, "thruster mode @ +0x44");
    static_assert(sizeof(FThrusterSoundSource) == 0x50, "CliThrusterSound is 0x50");

    // on one of the ThrusterEquipConsts ID_Strings.
    Archetype::ISound* ThrusterLookupArch(ID_String const& Name)
    {
        return static_cast<Archetype::ISound*>(LookupClientSoundArch(&Name));
    }

    // and 0x0052f273..0x0052f28d: exterior prefers the exterior
    // archetype, interior the interior one; either falls back on the other.
    Archetype::ISound* ThrusterCurrentArch(FThrusterSoundSource const* Self)
    {
        if (Self->bExterior != 0)
            return Self->ExteriorArch != nullptr ? Self->ExteriorArch : Self->InteriorArch;
        return Self->InteriorArch != nullptr ? Self->InteriorArch : Self->ExteriorArch;
    }

    // / 0x0052f376..0x0052f39e: to interior -- the interior archetype
    // looked up and acquired (slot 3) on first use, with no null test of the lookup, then the
    // sound stopped so the next tick starts the other archetype.
    void ThrusterSwitchToInterior(FThrusterSoundSource* Self)
    {
        Archetype::ISound* const Interior = Self->InteriorArch;
        Self->bExterior = 0;
        if (Interior == nullptr)
        {
            Self->InteriorArch = ThrusterLookupArch(ThrusterEquipConsts::INTERIOR_SOUND_NAME);
            Self->InteriorArch->Acquire();
        }
        Self->Stop();
    }
}

ISoundSource* CreateClientThrusterSound(FClientEquippedObj* Ship)
{
    // the routine(0x50) (malloc), null -> null; else
    // (Ship).
    void* const Block = SoundSourceAlloc(0x50u);
    if (Block == nullptr)
        return nullptr;

    // listing 0052efb0..0052f07c. the routine(1) first, then the fields.
    FThrusterSoundSource* const Self = new (Block) FThrusterSoundSource(BlockFlagByte(Block), Ship);

    // the exterior archetype, acquired (slot 3) when found.
    Self->ExteriorArch = ThrusterLookupArch(ThrusterEquipConsts::EXTERIOR_SOUND_NAME);
    if (Self->ExteriorArch != nullptr)
        Self->ExteriorArch->Acquire();

    // the interior one too when the ship is this client's -- the
    // object's owner (class & 3 -> +0xb4, else the routine) against the field.
    CObject* const Obj = Ship->Object;
    unsigned const Owner =
        (Obj != nullptr && (static_cast<unsigned>(Obj->ObjectClass) & 0x3u) == 0x3u)
            ? static_cast<CSimple const*>(Obj)->OwnerPlayer
            : GetPlayerConnectionId();
    if (Owner == GetPlayerConnectionId())
    {
        Self->InteriorArch = ThrusterLookupArch(ThrusterEquipConsts::INTERIOR_SOUND_NAME);
        if (Self->InteriorArch != nullptr)
            Self->InteriorArch->Acquire();
    }

    // looping.
    Self->Flags = static_cast<unsigned char>(Self->Flags | 1u);
    return Self;
}

void TickClientThrusterSound(ISoundSource* Sound, float /*Dt*/)
{
    // (thiscall, RET 4) -- listing 0052f110..0052f3bf. Dt is not read.
    FThrusterSoundSource* const Self = static_cast<FThrusterSoundSource*>(Sound);
    if (ThrusterCurrentArch(Self) == nullptr)
        return;

    // every functioning thruster that could thrust (equip-FX wrapper
    // list at ship +0xb8, equipment type exactly 0x400) adds its thrust and its connection
    // position (CEquip slot 18, no matrix).
    FEquipFxListNode* const Head =
        static_cast<FEquipFxListNode*>(Self->Ship->EquipFx.Head);
    float SumX = 0.0f;
    float SumY = 0.0f;
    float SumZ = 0.0f;
    float SumThrust = 0.0f;
    int Count = 0;
    for (FEquipFxListNode* Node = Head->Next; Node != Head; Node = Node->Next)
    {
        CEquip* const Equip = Node->Wrapper->Equip;
        if (Equip->Type != 0x400u)
            continue;
        if (!Equip->IsFunctioning())
            continue;
        CEThruster* const Thruster = static_cast<CEThruster*>(Equip);
        if (!Thruster->CouldThrust())
            continue;
        float const Thrust = Thruster->GetThrust();
        Vector Where;
        Equip->GetConnectionPosition(&Where, nullptr);
        ++Count;
        SumX = Where.x + SumX;
        SumY = SumY + Where.y;
        SumZ = SumZ + Where.z;
        SumThrust = Thrust + SumThrust;
    }

    // nothing thrusting (no thruster, or a sum not above 0, NaN
    // included: TEST AH,0x41 / JNZ) -> 0x0052f3a9: stop when playing.
    if (Count == 0 || !(SumThrust > 0.0f))
    {
        if (Self->IsActive() != 0)
            Self->Stop();
        return;
    }

    // the mean position; 1/Count stays on the FPU.
    double const InvCount = 1.0 / static_cast<double>(Count);
    Self->ThrusterPosition[0] = static_cast<float>(static_cast<double>(SumX) * InvCount);
    Self->ThrusterPosition[1] = static_cast<float>(static_cast<double>(SumY) * InvCount);
    Self->ThrusterPosition[2] = static_cast<float>(static_cast<double>(SumZ) * InvCount);

    // not playing -> Start(current archetype, 0) (slot 22).
    if (Self->IsActive() == 0)
        Self->Start(ThrusterCurrentArch(Self), 0);

    // the level -- 0 below MIN_VOLUME_FORCE (TEST AH,0x5 / JP: only
    // "less" takes it), 1 above MAX_VOLUME_FORCE (TEST AH,0x41 / JNZ: only "greater"), a linear
    // ramp in between; then 10*log10(level*0.8 + 0.2) dB (the field, the field,
    // FLDLG2/FYL2X, the field) as the attenuation (slot 32).
    double Level = 0.0;
    if (SumThrust < ThrusterEquipConsts::MIN_VOLUME_FORCE)
        Level = 0.0;
    else if (SumThrust > ThrusterEquipConsts::MAX_VOLUME_FORCE)
        Level = 1.0;
    else
        Level = (static_cast<double>(SumThrust) -
                 static_cast<double>(ThrusterEquipConsts::MIN_VOLUME_FORCE)) /
                (static_cast<double>(ThrusterEquipConsts::MAX_VOLUME_FORCE) -
                 static_cast<double>(ThrusterEquipConsts::MIN_VOLUME_FORCE));
    Self->SetAttenuation(static_cast<float>(log10(Level * 0.8 + 0.2) * 10.0));

    // the player's own character in the first-person camera hears the
    // interior sound.
    if (Self->Ship->get_id() == GetPlayerCharacterId() && IsFirstPersonCameraCurrent())
    {
        if (Self->bExterior == 0)
            return;
        ThrusterSwitchToInterior(Self);
        return;
    }

    // back to exterior (1 stays, 0 flips and stops, any other value
    // takes the interior switch).
    if (Self->bExterior == 1)
        return;
    if (Self->bExterior == 0)
    {
        Self->bExterior = 1;
        Self->Stop();
        return;
    }
    ThrusterSwitchToInterior(Self);
}

int LoadClientSoundArchIni(char const* FullPath, bool bForce)
{
    // (cdecl Path, bForce; the one caller, the [Data] sounds = key at 0x005b0d6f,
    // passes the GetFileSysPath result and 0). Returns how many SoundArchs the registry grew by
    // (the field after minus before, 0x0042b04f / 0x0042b2ad).
    size_t const OldCount = G_SoundArchs.size();

    INI_Reader Reader;
    if (Reader.open(FullPath, false))
    {
        while (Reader.read_header())
        {
            if (!Reader.is_header("Sound"))
                continue;

            // a default SoundArch (the routine) with both range ends
            // reset to -1.0 before the keys are read.
            FClientSoundArch Scratch;
            Scratch.RangeMax = -1.0f;
            Scratch.RangeMin = -1.0f;
            while (Reader.read_value())
            {
                if (Reader.is_value("nickname"))
                {
                    // CreateID into +0x04. The port also keeps the text.
                    char const* const Nick = Reader.get_value_string();
                    Scratch.NicknameId = CreateID(Nick);
                    strncpy(Scratch.Nickname, Nick, sizeof(Scratch.Nickname) - 1);
                    Scratch.Nickname[sizeof(Scratch.Nickname) - 1] = '\0';
                }
                else
                {
                    ParseSoundValue(&Scratch, Reader);
                }
            }

            ID_String Id;
            Id.Id = Scratch.NicknameId;
            if (Id.IsEmpty())
            {
                // the reader's link log, then the ERROR with the file
                // (+0x08) and the ini path.
                Reader.log_link("Sound has no nickname!");
                FDUMP(0x100001,
                      "%s(%d) : *** ERROR: Sound has no nickname.  Skipping. Filename(%s) "
                      "IniFile(%s).",
                      kSoundArchCpp, 0x1bb, Scratch.File != nullptr ? Scratch.File : "",
                      FullPath);
            }
            else
            {
                // the attenuation (signed low byte of +0x18) clamped
                // to [-96, 0]. FCOM 0.0 / TEST AH,0x41 / JNZ: a value above 0 takes 0; else
                // FCOMP -96.0 (the field) / TEST AH,5 / JP: below -96 takes -96, anything
                // else (including an unordered compare) keeps the value.
                double const Atten =
                    static_cast<double>(static_cast<signed char>(Scratch.Flags & 0xffu));
                float Clamped;
                if (Atten > 0.0)
                    Clamped = 0.0f;
                else if (Atten < -96.0)
                    Clamped = -96.0f;
                else
                    Clamped = static_cast<float>(Atten);
                // FCOMP / TEST AH,0x44 / JNP skips only on equal.
                if (!(static_cast<double>(Clamped) == Atten))
                {
                    FDUMP(0x100002,
                          "%s(%d) : *** WARNING: sound \"%s\" has attenuation out of valid range "
                          "(0, -96)\n",
                          kSoundArchCpp, 0x1c9, Scratch.File != nullptr ? Scratch.File : "");
                }
                // ftol of the clamp into the low byte.
                int const ClampedInt = static_cast<int>(Clamped);
                Scratch.Flags = (Scratch.Flags & 0xffffff00u) |
                                (static_cast<unsigned>(ClampedInt) & 0xffu);

                // RangeMin (+0x10) FCOMP 0.0 / TEST AH,5 / JP -- only a
                // RangeMin below 0 resets both ends to -1.0.
                if (Scratch.RangeMin < 0.0f)
                {
                    Scratch.RangeMax = -1.0f;
                    Scratch.RangeMin = -1.0f;
                }
                // RangeMin FCOMP RangeMax / TEST AH,1 / JNZ -- no warning
                // when RangeMin is below RangeMax or the compare is unordered.
                else if (!(Scratch.RangeMin < Scratch.RangeMax) &&
                         !_isnan(Scratch.RangeMin) && !_isnan(Scratch.RangeMax))
                {
                    FDUMP(0x100002, "%s(%d) : *** WARNING: Sound archetype '%s' has invalid range.\n",
                          kSoundArchCpp, 0x1d5, Id.get_string());
                }
                // the routine(&Id, &Scratch, bForce).
                RegisterSoundArch(&Scratch, bForce);
            }
            // The scratch's own malloc'd file, when RegisterSoundArch did not take it over.
            if (Scratch.File != nullptr)
                free(Scratch.File);
        }
    }

    return static_cast<int>(G_SoundArchs.size() - OldCount);
}

void* LookupClientSoundArch(ID_String const* Id)
{
    // (cdecl, ID_String const*). An empty id and an id with no registered
    // SoundArch both end at 0x0042aeb2: the 0x168 WARNING with ID_String::get_string, and null.
    // Otherwise the scratch key the field (+0x04 = the id, 0x0042ae94) is looked up with
    // and the node's SoundArch (+0x0c) returned.
    if (!Id->IsEmpty())
    {
        std::map<unsigned, FClientSoundArch*>::iterator Found = G_SoundArchs.find(Id->Id);
        if (Found != G_SoundArchs.end())
            return static_cast<Archetype::ISound*>(Found->second);
    }
    FDUMP(0x100002, "%s(%d) : *** WARNING: unknown Sound archetype '%s'", kSoundArchCpp, 0x168,
          Id->get_string());
    return nullptr;
}

void PreloadClientSoundArchesByCategory(unsigned Mask)
{
    // (cdecl Mask; callers 0x0054ac99 with 1, 0x005588e9 with 2). The in-order
    // walk of the field (0x0042acb0..0x0042ad27): a SoundArch whose persistence nibble
    // ((+0x18 >> 20) & Mask & 0xf) is non-zero and whose slot 0 (GetSmArchId) answers -1 takes
    // one slot 3 (Acquire, +0x0c).
    std::map<unsigned, FClientSoundArch*>::iterator It = G_SoundArchs.begin();
    for (; It != G_SoundArchs.end(); ++It)
    {
        FClientSoundArch* const Arch = It->second;
        if ((((Arch->Flags >> 20) & Mask) & 0xfu) == 0)
            continue;
        if (Arch->GetSmArchId() == -1)
            Arch->Acquire();
    }
}

void UnloadClientSoundArchesByCategory(unsigned Mask)
{
    // (cdecl Mask; callers 0x0054afde with 1, 0x00558a80 with 2). The same walk
    // and the same test, including the slot 0 answer of -1, then one slot 4 (Release, +0x10).
    std::map<unsigned, FClientSoundArch*>::iterator It = G_SoundArchs.begin();
    for (; It != G_SoundArchs.end(); ++It)
    {
        FClientSoundArch* const Arch = It->second;
        if ((((Arch->Flags >> 20) & Mask) & 0xfu) == 0)
            continue;
        if (Arch->GetSmArchId() == -1)
            Arch->Release();
    }
}

// app-shutdown teardown of the whole registry (baseline 0x5b22af, right after the
// voice registry drop / before the rest of WinMain's teardown chain). 0x0042b4d9..0x0042b528:
// every SoundArch in id order has slot 4 (Release, +0x10) called until it answers 0.
// the set is emptied (the fast path frees every node through
// / the routine / operator delete when the range is the whole tree; the general
// one erases node by node). The port also frees each archetype's malloc'd file and heap entry.
void ShutdownClientSoundArch()
{
    std::map<unsigned, FClientSoundArch*>::iterator It = G_SoundArchs.begin();
    for (; It != G_SoundArchs.end(); ++It)
    {
        FClientSoundArch* const Arch = It->second;
        while (Arch->Release() != 0)
        {
        }
    }
    for (It = G_SoundArchs.begin(); It != G_SoundArchs.end(); ++It)
    {
        free(It->second->File);
        delete It->second;
    }
    G_SoundArchs.clear();
}

unsigned char AcquireClientSoundArch(void* Arch)
{
    // Archetype slot 3 (+0x0c). The null test is the one ArchAcquire always had (unchanged by
    // the ISound conversion; its status is part of the menu_ui_init callers' own entries).
    if (Arch == nullptr)
        return 0;
    return static_cast<Archetype::ISound*>(Arch)->Acquire();
}

unsigned char ReleaseClientSoundArch(void* Arch)
{
    // Archetype slot 4 (+0x10); the same pre-existing null test as ArchReleaseWav.
    if (Arch == nullptr)
        return 0;
    return static_cast<unsigned char>(static_cast<Archetype::ISound*>(Arch)->Release());
}

unsigned char PreloadClientSoundArch(ID_String const* Id)
{
    // empty id or unknown archetype -> 0; else tail-jump into slot 3.
    // The second stack argument (the StarSys caller pushes 1) is never read.
    if (Id->IsEmpty())
        return 0;
    void* const Arch = LookupClientSoundArch(Id);
    if (Arch == nullptr)
        return 0;
    return AcquireClientSoundArch(Arch);
}

void UnloadClientSoundArch(ID_String const* Id)
{
    // empty id or unknown archetype -> nothing; else tail-jump into slot 4.
    if (Id->IsEmpty())
        return;
    void* const Arch = LookupClientSoundArch(Id);
    if (Arch == nullptr)
        return;
    ReleaseClientSoundArch(Arch);
}

void* CreateClientSoundSource3D()
{
    // the routine(0x64) (the pool the field); null -> null, else
    // (JMP) on the block.
    void* const Block = SoundSourceAlloc(0x64u);
    if (Block == nullptr)
        return nullptr;
    return static_cast<ISoundSource*>(new (Block) FSpatialSource(BlockFlagByte(Block)));
}

void* CreateClientSoundSource()
{
    // the routine(0x30) (the pool the field); null -> null, else
    // (1) on the block.
    void* const Block = SoundSourceAlloc(0x30u);
    if (Block == nullptr)
        return nullptr;
    return static_cast<ISoundSource*>(new (Block) FSoundSource(1, BlockFlagByte(Block)));
}

void* CreateClientEquipSoundEmitter(void* Arch)
{
    // The caller's the routine(0x68) (malloc) with its null test (e.g. 0x00529086..
    // ): a failed allocation gives a null emitter and the constructor is not run.
    // Every caller passes a found archetype (the routine tested non-null).
    void* const Block = SoundSourceAlloc(0x68u);
    if (Block == nullptr)
        return nullptr;
    FEquipSoundEmitter* const Emitter = new (Block)
        FEquipSoundEmitter(BlockFlagByte(Block), static_cast<Archetype::ISound*>(Arch));
    return static_cast<ISoundSource*>(Emitter);
}

void* GetClientEquipSoundEmitterArch(void* Emitter)
{
    // The engine emitter's +0x64, read raw by the routine (0x005282e2, 0x005283f1).
    return static_cast<FEquipSoundEmitter*>(static_cast<ISoundSource*>(Emitter))->EmitterArch;
}

unsigned char PlayClientSoundArch(void* Arch, unsigned* PlayKey, void** OutHandle,
                                  unsigned char bAltStart)
{
    // (cdecl Arch, PlayKey, OutHandle, bAltStart). A SoundImpl or a VoiceMsg
    // (HUD_Comm 0x004cb188 / 0x004cc01b): table calls only. No archetype: false, OutHandle
    // untouched.
    Archetype::ISound* const SoundArch = static_cast<Archetype::ISound*>(Arch);
    if (SoundArch == nullptr)
        return 0;

    // a streamer archetype (slot 9) or key bit 2 -> Sound::
    // StreamedSource from the 0x40 pool; key bit 1 -> Sound::Source3D from the 0x64 pool;
    // otherwise (no key included) a plain Sound::Source, the routine(1), from the 0x30 pool.
    // Each pool is Heap::Malloc'd directly; a null block skips the constructor and baseline then
    // faults on the flag byte -- there is no failure path.
    unsigned char const* const KeyFlags =
        PlayKey != nullptr ? reinterpret_cast<unsigned char const*>(&PlayKey[1]) : nullptr;
    FSoundSource* Source = nullptr;
    if (SoundArch->IsStreamer() != 0 || (KeyFlags != nullptr && (*KeyFlags & 4u) != 0))
    {
        void* const Block = G_StreamedHeap.Malloc();
        Source = new (Block) FStreamedSource(BlockFlagByte(Block));
    }
    else if (KeyFlags != nullptr && (*KeyFlags & 2u) != 0)
    {
        void* const Block = G_Source3DHeap.Malloc();
        Source = new (Block) FSpatialSource(BlockFlagByte(Block));
    }
    else
    {
        void* const Block = G_SourceHeap.Malloc();
        Source = new (Block) FSoundSource(1, BlockFlagByte(Block));
    }

    // with a key -- flag bit 1 = key bit 3, bit 0 = key bit 0, +0x1c =
    // key +0x14, then slot 35 (+0x8c) with key +0x08 (Source3D: the world position; the other
    // two tables: a do-nothing).
    if (PlayKey != nullptr)
    {
        unsigned char Bits = Source->Flags;
        Bits = static_cast<unsigned char>((((*KeyFlags >> 2) ^ Bits) & 2u) ^ Bits);
        Bits = static_cast<unsigned char>(((*KeyFlags ^ Bits) & 1u) ^ Bits);
        Source->Flags = Bits;
        memcpy(&Source->AttenuationDb, &PlayKey[5], sizeof(float));
        Source->Slot35(&PlayKey[2]);
    }
    // slot 11 (+0x2c) into +0x10.
    Source->Pitch = SoundArch->GetRandomPitch();

    // slot 22 Start(Arch, key[0] or 0), or with bAltStart slot 23
    // AltStart(Arch) -- through the source's own table.
    int const Extra = PlayKey != nullptr ? static_cast<int>(PlayKey[0]) : 0;
    unsigned char bOk = 0;
    if (bAltStart == 0)
        bOk = Source->Start(SoundArch, Extra);
    else
        bOk = Source->AltStart(SoundArch);

    // success with an out pointer -> AddRef, *OutHandle = source; then
    // the creation reference is released either way.
    if (bOk != 0 && OutHandle != nullptr)
    {
        Source->AddRef();
        *OutHandle = static_cast<ISoundSource*>(Source);
    }
    Source->Release();
    return bOk;
}

unsigned char RestartClientStreamedSound(ISoundSource* Handle, void* Arch, int Offset)
{
    // on a Sound::StreamedSource the Gf script started (the routine 0x0044f707).
    return StreamedRestart(static_cast<FStreamedSource*>(Handle),
                           static_cast<Archetype::ISound*>(Arch), Offset);
}

unsigned char PlayClientSoundArchIfIdle(void* Arch, unsigned* PlayKey, void** OutHandle)
{
    // count the nodes of the field, first to last, whose source +0x18 is Arch
    // (0x00428604..0x00428611); a non-zero count answers false (0x00428618), else
    // (Arch, PlayKey, OutHandle, 0).
    int Count = 0;
    FPlayingSourceNode* const Head = G_PlayingSources.Head;
    for (FPlayingSourceNode* Node = Head->Next; Node != Head; Node = Node->Next)
    {
        if (static_cast<void*>(Node->Value->Arch) == Arch)
            ++Count;
    }
    if (Count != 0)
        return 0;
    return PlayClientSoundArch(Arch, PlayKey, OutHandle, 0);
}

void PlayOrRewindClientSoundArch(void* Arch, unsigned* PlayKey, void** OutHandle)
{
    // the first node (from the front of the field) whose source +0x18 is Arch
    // (0x00428651..0x0042865d) gets vt+0x64 (SetStartOffset) with 0 (0x00428676); no match ->
    // (Arch, PlayKey, OutHandle, 0).
    FPlayingSourceNode* const Head = G_PlayingSources.Head;
    for (FPlayingSourceNode* Node = Head->Next; Node != Head; Node = Node->Next)
    {
        if (static_cast<void*>(Node->Value->Arch) == Arch)
        {
            Node->Value->SetStartOffset(0);
            return;
        }
    }
    PlayClientSoundArch(Arch, PlayKey, OutHandle, 0);
}

void StopClientSoundArch(void* Arch)
{
    // walk the field from the front (0x004285c0..0x004285e7). 0x004285d7
    // loads the next node before the match test, so a source that Stop unlinks does not break
    // the walk; a match gets vt+0x60 (0x004285dd), and the head is re-read after the call.
    FPlayingSourceNode* Node = G_PlayingSources.Head->Next;
    while (Node != G_PlayingSources.Head)
    {
        FSoundSource* const Source = Node->Value;
        Node = Node->Next;
        if (static_cast<void*>(Source->Arch) == Arch)
            Source->Stop();
    }
}

int CountClientSoundArchSources(void* Arch)
{
    // (cdecl): walk the field from its first node to the head (00428006..
    // ) and count the nodes whose source +0x18 is Arch (0042801a / 0042801c). An empty
    // list answers 0 (0042800c).
    int Count = 0;
    FPlayingSourceNode* const Head = G_PlayingSources.Head;
    for (FPlayingSourceNode* Node = Head->Next; Node != Head; Node = Node->Next)
    {
        if (static_cast<void*>(Node->Value->Arch) == Arch)
            Count = Count + 1;
    }
    return Count;
}

void PauseAllClientSounds()
{
    // (cdecl): every node of the field from the front gets slot 26 (vt+0x68,
    // Pause); the next node is read after the call.
    FPlayingSourceNode* Node = G_PlayingSources.Head->Next;
    while (Node != G_PlayingSources.Head)
    {
        Node->Value->Pause();
        Node = Node->Next;
    }
    OracleOnSoundCall("PauseAllClientSounds", nullptr, 0, nullptr, 0, -1); // Oracle (P0.5)
}

void ResumePausedClientSounds()
{
    // (cdecl): every node of the field from the front whose source answers
    // slot 28 (vt+0x70, IsPaused) gets slot 27 (vt+0x6c, Resume); the next node is read after
    // the calls.
    FPlayingSourceNode* Node = G_PlayingSources.Head->Next;
    while (Node != G_PlayingSources.Head)
    {
        FSoundSource* const Source = Node->Value;
        if (Source->IsPaused() != 0)
            Source->Resume();
        Node = Node->Next;
    }
    OracleOnSoundCall("ResumePausedClientSounds", nullptr, 0, nullptr, 0, -1); // Oracle (P0.5)
}

unsigned char BindClientBackgroundMusic(void** OutSource)
{
    return BindSource(G_BackgroundMusic, OutSource);
}

unsigned char BindClientBackgroundAmbient(void** OutSource)
{
    return BindSource(G_BackgroundAmbient, OutSource);
}

unsigned char SetClientBackgroundMusic(unsigned NicknameId, unsigned Flags, float Fade)
{
    unsigned char const Result =
        WriteBackgroundSource(&G_BackgroundMusic, NicknameId, Flags, Fade, 3);
    unsigned const OracleUints[2] = {NicknameId, Flags};                      // Oracle
    OracleOnSoundCall("SetClientBackgroundMusic", OracleUints, 2, &Fade, 1, Result); // (P0.5)
    return Result;
}

unsigned char SetClientBackgroundAmbient(unsigned NicknameId, unsigned Flags, float Fade)
{
    unsigned char const Result =
        WriteBackgroundSource(&G_BackgroundAmbient, NicknameId, Flags, Fade, 4);
    unsigned const OracleUints[2] = {NicknameId, Flags};                        // Oracle
    OracleOnSoundCall("SetClientBackgroundAmbient", OracleUints, 2, &Fade, 1, Result); // (P0.5)
    return Result;
}

namespace
{
    // The shared body of the routine / the routine (they differ only in the global they
    // read). 0x00428c6a..0x00428c90: a target above 1e8 (the field; `TEST AH,0x41` + JNZ
    // skips on not-greater) is replaced by the ducking down_by of the audio mode (the field
    // in mode 3, else the field -- the routine's pair). 0x00428c94..0x00428ccf: a rate above
    // 1e8 stores that mode's down_time as the remaining time (+0x38) and the target (+0x34);
    // otherwise 0x00428cd0..0x00428cf8: slot 9 (+0x24) the current dB, target stored, the time
    // |target - current| / Rate (FSUB, FDIV, FABS). No source, nothing.
    void FadeBackgroundSource(FStreamedSource* Source, float TargetDb, float Rate)
    {
        if (Source == nullptr)
            return;
        float DownBy = 0.f;
        float DownTime = 0.f;
        GetClientDuckingConfig(&DownBy, &DownTime, nullptr);
        float Target = TargetDb;
        if (static_cast<double>(TargetDb) > 100000000.0)
            Target = DownBy;
        if (static_cast<double>(Rate) > 100000000.0)
        {
            Source->FadeTime = DownTime;
            Source->FadeTargetDb = Target;
            return;
        }
        float Current = 0.f;
        Source->GetAttenuation(&Current);
        Source->FadeTargetDb = Target;
        Source->FadeTime = static_cast<float>(fabs((Target - Current) / Rate));
    }
}

void FadeClientBackgroundMusic(int Unused, float TargetDb, float Rate)
{
    // (cdecl) on the field. Unused is never read (callers pass 3).
    (void)Unused;
    FadeBackgroundSource(G_BackgroundMusic, TargetDb, Rate);
    float const OracleFloats[2] = {TargetDb, Rate};                               // Oracle
    OracleOnSoundCall("FadeClientBackgroundMusic", nullptr, 0, OracleFloats, 2, -1); // (P0.5)
}

void FadeClientBackgroundAmbient(int Unused, float TargetDb, float Rate)
{
    // (cdecl) on the field. Unused is never read (callers pass 3).
    (void)Unused;
    FadeBackgroundSource(G_BackgroundAmbient, TargetDb, Rate);
    float const OracleFloats[2] = {TargetDb, Rate};                                 // Oracle
    OracleOnSoundCall("FadeClientBackgroundAmbient", nullptr, 0, OracleFloats, 2, -1); // (P0.5)
}

void PushClientSoundDucking()
{
    // (0x00428db0..0x00428dd5) — the first push arms the spaceflight duck:
    // = the field (ducking_spaceflight_down_time), the field =
    // (ducking_spaceflight_down_by), read directly -- not through the routine,
    // so the RTC pair is never used here whatever the audio mode. Then ++the field.
    if (G_DuckRefCount == 0)
    {
        G_DuckTimer = GetClientSpaceflightDuckDownTime();
        G_DuckTargetDb = GetClientSpaceflightDuckDownBy();
    }
    ++G_DuckRefCount;
    OracleOnSoundCall("PushClientSoundDucking", nullptr, 0, nullptr, 0, -1); // Oracle (P0.5)
}

void PopClientSoundDucking()
{
    // — baseline always uses `ducking_spaceflight_up_time` here, even in RTC mode.
    --G_DuckRefCount;
    if (G_DuckRefCount == 0)
    {
        G_DuckTargetDb = 0.f;
        G_DuckTimer = GetClientSpaceflightDuckUpTime();
    }
    OracleOnSoundCall("PopClientSoundDucking", nullptr, 0, nullptr, 0, -1); // Oracle (P0.5)
}

float GetClientSoundDuckingDb()
{
    return G_DuckCurrentDb;
}

void StopAllClientSoundsExceptArch(void* KeepArch)
{
    // (cdecl). 0x00428430..0x00428452: the background music (the field) gets
    // vt+0x60 (Stop), then vt+8 (Release) on the reloaded global, which is then cleared.
    if (G_BackgroundMusic != nullptr)
    {
        G_BackgroundMusic->Stop();
        G_BackgroundMusic->Release();
        G_BackgroundMusic = nullptr;
    }
    // the same for the background ambience (the field).
    if (G_BackgroundAmbient != nullptr)
    {
        G_BackgroundAmbient->Stop();
        G_BackgroundAmbient->Release();
        G_BackgroundAmbient = nullptr;
    }
    // every live source (the field, from the front) whose +0x18 is
    // not KeepArch gets vt+0x60; the next node is read before the call (0x00428496) and the
    // head re-read after it.
    FPlayingSourceNode* Node = G_PlayingSources.Head->Next;
    while (Node != G_PlayingSources.Head)
    {
        FSoundSource* const Source = Node->Value;
        Node = Node->Next;
        if (static_cast<void*>(Source->Arch) != KeepArch)
            Source->Stop();
    }
    // SoundManager slot 46 (+0xb8) with 0.
    if (NeuronRuntime::Sound != nullptr)
        NeuronRuntime::Sound->MixerTick(nullptr);
    // the ducking state reset -- the field / ca4 / ca8 = 0 and
    // = -1.0f.
    G_DuckTargetDb = 0.f;
    G_DuckCurrentDb = 0.f;
    G_DuckRefCount = 0;
    G_DuckTimer = -1.f;
    unsigned const OracleKeep = KeepArch != nullptr ? GetClientSoundArchId(KeepArch) : 0u; // Oracle
    OracleOnSoundCall("StopAllClientSoundsExceptArch", &OracleKeep, 1, nullptr, 0, -1); // (P0.5)
}

void StopAllClientSounds()
{
    // . 0x004284f0..0x004284fd: the routine(0).
    StopAllClientSoundsExceptArch(nullptr);
    // a non-zero live Sound::Source count (the field) is reported.
    if (G_AliveSoundSourceCount != 0)
    {
        FDUMP(0x100002, "%s(%d) : *** WARNING: sound reference count leaks detected: %d sounds",
              kSoundCpp, 0x4da, G_AliveSoundSourceCount);
    }
    // Heap::Shrink on the three source pools, in this order:
    // (0x30, Sound::Source), the field (0x64, Source3D), the field (0x40,
    // StreamedSource).
    G_SourceHeap.Shrink();
    G_Source3DHeap.Shrink();
    G_StreamedHeap.Shrink();
}

void* GetClientSoundListenerObject()
{
    // .
    return G_SoundListenerObject;
}

void SetClientSoundListenerObject(void* CameraOrNull)
{
    // .
    G_SoundListenerObject = CameraOrNull;
}

void TickClientSoundMixer(void* Camera, float DeltaSeconds)
{
    // (cdecl Camera, Dt). the field = 0 first.
    G_MixerRankedCount = 0;

    // (1) 0x00428040..0x0042811f, the global ducking fade. A timer below zero (`TEST AH,5 /
    // JNP`: only "less" skips) means none. Otherwise the timer drops by Dt (stored as a float,
    // the x87 value tested): at or below zero (`TEST AH,0x41 / JP`: "greater" and NaN blend)
    // the timer becomes -1 and the current dB the target; above it the power-ratio blend from
    // the current to the target dB, with the remaining time re-read from the stored float.
    if (!(G_DuckTimer < 0.f))
    {
        double const NewTimer = static_cast<double>(G_DuckTimer) - static_cast<double>(DeltaSeconds);
        G_DuckTimer = static_cast<float>(NewTimer);
        if (!(NewTimer > 0.0))
        {
            G_DuckTimer = -1.f;
            G_DuckCurrentDb = G_DuckTargetDb;
        }
        else
        {
            G_DuckCurrentDb = static_cast<float>(DbBlend(G_DuckCurrentDb, G_DuckTargetDb,
                                                         DeltaSeconds,
                                                         static_cast<double>(G_DuckTimer)));
        }
    }

    // (2) 0x0042811f..0x00428224, the listener. the field/60/64 are a function-local
    // static in baseline (guard bit the field), zero until first written. With a camera its
    // position comes from camera slot 3 (+0xc, no test of the pointer) and, with a
    // SoundManager, the orientation from slot 4 (+0x10) goes to ISoundManager slot 14, the
    // position to slot 15 and the velocity of the routine's object (vt+0x48) to slot 16.
    // With no camera the manager's own listener (slot 22) is read back.
    ISoundManager* const Sound = NeuronRuntime::Sound;
    if (Camera != nullptr)
    {
        ICamera* const Cam = static_cast<ICamera*>(Camera);
        float const* const Pos = Cam->GetPosition();
        G_MixerListener[0] = Pos[0];
        G_MixerListener[1] = Pos[1];
        G_MixerListener[2] = Pos[2];
        if (Sound != nullptr)
        {
            // /004281af: camera slot 4 hands its 3x3 straight to ISoundManager
            // slot 14 SetOrientationFromMatrix.
            Sound->SetOrientationFromMatrix(const_cast<float*>(Cam->GetOrientation()));
            Sound->SetListenerPosition(reinterpret_cast<FVector*>(G_MixerListener));
            // the routine → ICamera::GetVelocity (+0x48) → SM slot 16.
            void* const VelSrc = G_SoundListenerObject;
            if (VelSrc != nullptr)
            {
                float Vel[3] = {};
                static_cast<ICamera*>(VelSrc)->GetVelocity(Vel);
                Sound->SetListenerVelocity(reinterpret_cast<FVector*>(Vel));
            }
        }
    }
    else if (Sound != nullptr)
    {
        Sound->GetListenerPosition(reinterpret_cast<FVector*>(G_MixerListener));
    }

    // (3) 0x00428224..0x0042825e, the voice budget: 0x80, and when GetGameOption(0x2d)
    // ("[AUDIO]") is set `max_submitted_sounds` (the routine): at or above 0x80 (unsigned
    // JNC) 0x80, below 8 (signed JGE) 8, otherwise itself.
    unsigned MaxVoices = kMixerMaxEntries;
    if (GetGameOption(kGameOptAudioGroup) != 0)
    {
        unsigned const Submitted = static_cast<unsigned>(GetMaxSubmittedSounds());
        if (Submitted >= kMixerMaxEntries)
            MaxVoices = kMixerMaxEntries;
        else if (static_cast<int>(Submitted) < 8)
            MaxVoices = 8;
        else
            MaxVoices = Submitted;
    }

    // (4) 0x0042825e..0x004282e6, the audible sources: the field walked from the LAST node
    // back to the first (prev links; the loop ends after the node that is head->next). A
    // source that slot 28 says is paused, or that slot 42 (Update: score and the finished
    // test) drops, or that SoundManager does not play (flag 4 clear), is skipped. A score
    // equal to the field (`TEST AH,0x44 / JNP`: only "equal"; a NaN is ranked) or a full
    // table (0x80) takes slot 3 (start latch) unless flag 8 is set; any other source goes into
    // .
    FPlayingSourceNode* Walk = G_PlayingSources.Head;
    if (Walk->Next != Walk)
    {
        do
        {
            Walk = Walk->Prev;
            FSoundSource* const Source = Walk->Value;
            if (Source->IsPaused() != 0)
                continue;
            if (Source->Update(G_MixerListener, DeltaSeconds) == 0)
                continue;
            unsigned char const Flags = Source->Flags;
            if ((Flags & 4u) == 0)
                continue;
            if (!(Source->MixerScale == kMixerNoScore) && G_MixerRankedCount < kMixerMaxEntries)
            {
                G_MixerRanked[G_MixerRankedCount] = Source;
                ++G_MixerRankedCount;
            }
            else if ((Flags & 8u) == 0)
            {
                Source->Slot3();
            }
        } while (Walk != G_PlayingSources.Head->Next);
    }

    // (5) 0x004282ec..0x004283d1, with a SoundManager: VC6 std::sort by score, the count cut
    // to the voice budget (unsigned JBE), slot 3 for every ranked source without flag 8 (the
    // count re-read after each call), then ISoundManager slot 38 (+0x98) with the table and
    // slot 46 (+0xb8) with 0.
    if (NeuronRuntime::Sound != nullptr)
    {
        MixerSortRanked(G_MixerRanked, G_MixerRanked + G_MixerRankedCount);
        if (G_MixerRankedCount > MaxVoices)
            G_MixerRankedCount = MaxVoices;
        for (unsigned Index = 0; Index < G_MixerRankedCount; ++Index)
        {
            FSoundSource* const Source = G_MixerRanked[Index];
            if ((Source->Flags & 8u) == 0)
                Source->Slot3();
        }
        NeuronRuntime::Sound->ReplaceAttachedList(reinterpret_cast<void**>(G_MixerRanked),
                                          G_MixerRankedCount);
        NeuronRuntime::Sound->MixerTick(nullptr);
    }

    // (6) 0x004283dc..0x0042840c, the retirement: the field from the front, the next node
    // read before the calls; a source slot 28 says is not paused and slot 29 says is finished
    // gets slot 24 (Stop).
    FPlayingSourceNode* Node = G_PlayingSources.Head->Next;
    while (Node != G_PlayingSources.Head)
    {
        FSoundSource* const Source = Node->Value;
        Node = Node->Next;
        if (Source->IsPaused() == 0 && Source->IsFinished() != 0)
            Source->Stop();
    }
}

int CreateSoundArchWav(Archetype::ISound* Arch, void* ParentFs)
{
    // (cdecl). One function for both archetype kinds: the SoundImpl (the routine)
    // and the VoiceMsg (the routine) differ only in what their slots 1/10/13 return.
    // EBX = -1; no ISoundManager or no Data -> -1.
    if (NeuronRuntime::Sound == nullptr || NeuronRuntime::Data == nullptr)
        return -1;

    // slot 1 (+4) GetFileName.
    char const* const RelPath = Arch->GetFileName();

    // the wav is opened as its own IFileSystem ("FileSystem", UTF inside
    // ParentFs or DOS through NeuronRuntime::Data) and that component, not a path, goes to
    // ISoundManager slot 33 (0042b724: MOV EDX,[ESP+0x54]). This is what lets a VoiceMsg read
    // its wav out of the profile .utf.
    void* ChildFs = nullptr;
    int SmId = -1;
    if (OpenWavFileSystem(RelPath, ParentFs, &ChildFs) != 0)
    {
        // "not found", then JMP into the "failed to create" dump below.
        FDUMP(0x100002, "%s(%d) : *** WARNING: Sound filename \"%s\" not found.\n",
              "Client\\cmn\\SoundArch.cpp", 0x227, RelPath);
    }
    else
    {
        // the routine(4) (USE_3D_SOUNDS) FCOMP the field (1.0f);
        // TEST AH,5 + JP: C0 alone (value below 1.0) falls through to BL = 1.
        bool const bNo3dSounds = GetPerfOptionSlot(4) < 1.f;
        // slot 10 (+0x28) Is2d is called first; it or BL -> 0x11, else 9.
        unsigned char const bIs2d = Arch->Is2d();
        unsigned Flags = (bIs2d != 0 || bNo3dSounds) ? 0x11u : 9u;
        // the routine(0x2d) [AUDIO], then (0x34) PITCH_BEND_SOUNDS.
        bool const bPitchBendOn = GetGameOption(kGameOptAudioGroup) != 0 &&
                                  GetGameOption(kGameOptPitchBendSounds) != 0;
        // slot 13 (+0x34) IsPitchBendable, always called; with BL -> |2.
        unsigned char const bBendable = Arch->IsPitchBendable();
        if (bBendable != 0 && bPitchBendOn)
            Flags |= 2u;
        // ISoundManager slot 33 (+0x84).
        SmId = NeuronRuntime::Sound->CreateArchetypeFromWav(static_cast<INeuronComponent*>(ChildFs), Flags);
    }

    if (SmId == -1)
    {
        // .
        FDUMP(0x100002,
              "%s(%d) : *** WARNING: Failed to create archetype for sound filename \"%s\"\n",
              "Client\\cmn\\SoundArch.cpp", 0x22b, RelPath);
    }

    // release the created component when there is one.
    ReleaseComponent(ChildFs);
    return SmId;
}

void DestroyClientSoundWav(int SmArchId)
{
    // 's tail (0x00431282..0x0043128d): ISoundManager slot 34 (+0x88). The caller
    // has already tested SmArchId against -1.
    NeuronRuntime::Sound->DestroyArchetype(SmArchId);
}

// / e30 / e60 / e90 / ec0 — the audio_music / audio_ambient / audio_sfx /
// audio_interface / audio_voice option callbacks: a 0..1 slider becomes 10*log2(x), floored
// at -96 dB (the threshold 0.0012886 is exactly 2^-9.6).
static float LinearToOptionDb(float Linear)
{
    // FCOMP the field (0x3aa8e63e) + TEST AH,0x41 + JNZ: the jump (keep -96) is taken for
    // less, equal and unordered; only "greater" falls through to the log.
    if (!(Linear > 0.0012886f))
        return -96.f;
    // FLDLN2 / FYL2X / FMUL the field (= 10 / ln 2).
    return static_cast<float>(log(static_cast<double>(Linear)) * 14.426950454711914);
}

void SetClientAudioMusicVolume(float Linear)
{
    G_AudioMusicDb = LinearToOptionDb(Linear);
}

void SetClientAudioAmbientVolume(float Linear)
{
    G_AudioAmbientDb = LinearToOptionDb(Linear);
}

void SetClientAudioSfxVolume(float Linear)
{
    G_AudioSfxDb = LinearToOptionDb(Linear);
}

void SetClientAudioInterfaceVolume(float Linear)
{
    G_AudioInterfaceDb = LinearToOptionDb(Linear);
}

void SetClientAudioVoiceVolume(float Linear)
{
    G_AudioVoiceDb = LinearToOptionDb(Linear);
}

GENRESULT GetClientSoundSourceVolumeDb(void* Source, float* OutDb)
{
    return static_cast<FSoundSource*>(Source)->GetAttenuation(OutDb);
}

float GetClientSoundSourceMixerScore(void* Source)
{
    FSoundSource* Typed = static_cast<FSoundSource*>(Source);
    return Typed != nullptr ? Typed->MixerScale : kMixerNoScore;
}

void* GetClientSoundSourceArch(void* Source)
{
    // Sound::Source +0x18, the archetype Start (the routine) stored. No null test.
    return static_cast<FSoundSource*>(Source)->Arch;
}

void* GetClientSoundArchLinked(void* Arch)
{
    // Archetype slot 12 (+0x30), the routine for a SoundImpl.
    return static_cast<Archetype::ISound*>(Arch)->GetLinked();
}

float GetClientSoundArchRandomPitch(void* Arch)
{
    // Archetype slot 11 (+0x2c), the branch for a SoundImpl.
    return static_cast<Archetype::ISound*>(Arch)->GetRandomPitch();
}

unsigned GetClientSoundArchId(void* Arch)
{
    // Archetype +0x04, the nickname id. No null test.
    return ArchFrom(Arch)->NicknameId;
}

// — the `sound_level_of_detail` option callback (the branch): store the new
// level (the field), then re-resolve the lod link (the routine) of every registered
// archetype in id order (the in-order walk of the field, 0x0042ac42..0x0042ac9f).
void SetClientSoundLod(int Lod)
{
    if (Lod == G_SoundLod)
        return;
    G_SoundLod = Lod;
    std::map<unsigned, FClientSoundArch*>::iterator It = G_SoundArchs.begin();
    for (; It != G_SoundArchs.end(); ++It)
        ArchResolveLod(It->second, G_SoundLod);
}

int GetClientSoundLod()
{
    return G_SoundLod;
}

void SetClientSoundSourceCone(void* SourcePtr, float InnerAngle, float OuterAngle)
{
    // (thiscall, RET 8) — Sound::Source3D, not a vtable slot; baseline calls it
    // directly. Writes +0x54 / +0x58; no test of the source.
    FSpatialSource* const Source =
        static_cast<FSpatialSource*>(static_cast<ISoundSource*>(SourcePtr));
    Source->Spatial[9] = InnerAngle;
    Source->Spatial[10] = OuterAngle;
}

void SetClientSoundSourceConeOutsideVolume(void* SourcePtr, float Volume)
{
    // (thiscall, RET 4) — Sound::Source3D: writes +0x5c; no test of the source.
    FSpatialSource* const Source =
        static_cast<FSpatialSource*>(static_cast<ISoundSource*>(SourcePtr));
    Source->Spatial[11] = Volume;
}

void SetClientSoundSourcePose(void* SourcePtr, int AttachHandle, float const* Transform)
{
    // Slot 37 (vt+0x94) on the source.
    static_cast<ISoundSource*>(SourcePtr)->SetPose(AttachHandle, Transform);
}

void SetClientSoundSourceType(void* SourcePtr, int Type)
{
    // A plain store into the source's +0x04 (`MOV dword ptr [EAX+4],2` at 0x004cb0f6 /
    // in HUD_Comm's the routine). No null test: the callers only reach it after a
    // successful the routine, which returned the source.
    static_cast<FSoundSource*>(SourcePtr)->Type = Type;
}

bool ClientSoundFadeDb(float FromDb, float ToDb, float Dt, float* Remaining, float* OutDb)
{
    // (cdecl).
    // `FCOMP 0.0; TEST AH,5; JP` -- a remaining time below zero means
    // no fade is running.
    if (*Remaining < 0.0f)
        return false;

    // Remaining -= Dt (stored as a float, the x87 value kept); not above
    // zero -> the fade ends on its target and Remaining becomes -1.
    double const NewRemaining = static_cast<double>(*Remaining) - static_cast<double>(Dt);
    *Remaining = static_cast<float>(NewRemaining);
    if (!(NewRemaining > 0.0))
    {
        *Remaining = -1.0f;
        *OutDb = ToDb;
        return true;
    }

    // both ends to power ratios through exp(dB * [0x005c9cf4]), the
    // ratio moved Dt / (NewRemaining + Dt) of the way.
    double const K = static_cast<double>(0.230258509516716f); // (float)
    double const From = exp(static_cast<double>(FromDb) * K);
    double const To = exp(static_cast<double>(ToDb) * K);
    double const Linear =
        From + (To - From) * (static_cast<double>(Dt) / (NewRemaining + static_cast<double>(Dt)));

    // `TEST AH,0x41; JP` -- at or below the field (float
    // 2.5118899e-10) the result is -96 dB (0xc2c00000).
    if (!(Linear > static_cast<double>(2.5118898627773945e-10f)))
    {
        *OutDb = -96.0f;
        return true;
    }
    // FLDLG2 / FYL2X = log10, times the field (double 10.0).
    *OutDb = static_cast<float>(log10(Linear) * 10.0);
    return true;
}
