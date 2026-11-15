// Author: Giperion.

#pragma once

#include "neuroncomponents.h"
#include "soundmanager.h"
#include "luaprofile.h"
#include "audio/dsound8.h"
#include <mmreg.h>
#include <stddef.h>

// SoundManager.dll internals. The ABI other modules see is src/include/soundmanager.h.

static const unsigned long kAggDescSize = 0x14;
static const unsigned long kRegisterPriority = 0x40000000;

static const char kComponentName[] = "SoundManager";

static const unsigned int kFdumpError = 0x100001;
static const unsigned int kFdumpWarning = 0x100002;
static const unsigned int kFdumpNotice = 0x100003;

// Source label shared by the sound manager diagnostics.
static const char kbaselineSource[] =
    "SoundManager\\SoundMan.cpp";
// - the statically linked WavLib (FDUMP sites 06ee867d, 8812, 883f, 88f8).
static const char kWavLibSource[] =
    "WavLib\\wavlib.cpp";
static const char kFdumpErrorFmt[] = "%s(%d) : ERROR:General:%s";
static const char kFdumpWarningFmt[] = "%s(%d) : WARNING:General:%s";
static const char kFdumpNoticeFmt[] = "%s(%d) : NOTICE:General:%s";

// / 06ee91bc / 06ee91ac — last word E5 60, not SDK A7 70.
static const GUID kClsidDirectSound = {
    0x47D4D946, 0x62E8, 0x11CF, {0x93, 0xBC, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID kIidDirectSoundbaseline = {
    0x279AFA83, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
static const GUID kIidDirectSound3DListenerbaseline = {
    0x279AFA84, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
static const GUID kIidDirectSound3DBufferbaseline = {
    0x279AFA86, 0x4981, 0x11CE, {0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60}};
// — IID_IDirectSoundNotify (the routine QI).
static const GUID kIidDirectSoundNotify = {
    0xB0210783, 0x89CD, 0x11D0, {0xAF, 0x08, 0x00, 0xA0, 0xC9, 0x25, 0xCD, 0x16}};

// IKsPropertySet — SDK tail differs; QI fail only disables EAX.
static const GUID kIidKsPropertySet = {
    0x31EFAC30, 0x515C, 0x11D0, {0xA9, 0xAA, 0x00, 0xAA, 0x00, 0x61, 0xBE, 0x93}};
static const GUID kDsPropSetEax20Listener = {
    0x4A4E6FC0, 0xC341, 0x11D1, {0xB7, 0x3A, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
static const GUID kDsPropSetEax20Buffer = {
    0x4A4E6FC1, 0xC341, 0x11D1, {0xB7, 0x3A, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};

// AGGDESC 0x14 for this factory (same layout as SystemContainer children).
struct FSoundAggDesc
{
    unsigned long Size;
    const char* Name;
    INeuronComponent* Outer;
    INeuronComponent** InnerOut;
    const char* Extra;
};

// Nested vtbl @ object +0x08 — not in the aggregator QI table. Slot 3 binds IProfileParser.
struct ISoundManagerParserBind : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API BindParser() = 0; // +0x0C the branch
};

class FSoundArchetype final : public ISoundArchetype
{
public:
    FSoundArchetype();
    ~FSoundArchetype();

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;
    GENRESULT NEURONCOMPONENTS_API GetFormat(void* Out) override;
    GENRESULT NEURONCOMPONENTS_API GetSamples(void* Out) override;
    unsigned long NEURONCOMPONENTS_API GetNumFrames() override;
    float NEURONCOMPONENTS_API GetGain() override;
    unsigned long NEURONCOMPONENTS_API GetChannelCount() override;
    unsigned long NEURONCOMPONENTS_API GetDurationMs() override;
    GENRESULT NEURONCOMPONENTS_API SetSamples(void* A, void* B) override;
    GENRESULT NEURONCOMPONENTS_API SetGain(float InGain) override;
    unsigned long NEURONCOMPONENTS_API IsReady() override;
    GENRESULT NEURONCOMPONENTS_API GetLoopRange(unsigned long* Start, unsigned long* End) override;

    GENRESULT FillFromPcm(const unsigned short* Fmt8, unsigned long ByteCount,
                          unsigned long LoopStart, unsigned long LoopEnd, const void* Samples,
                          IDirectSound* DirectSound, unsigned long InFlags);
    // the caller opens the file as an IFileSystem and this reads the
    // RIFF straight out of its mapping (WavLib the routine). No path is involved.
    GENRESULT FillFromWav(INeuronComponent* FileSystem, IDirectSound* DirectSound,
                          unsigned long InFlags);
    GENRESULT DuplicateBuffer(IDirectSound* DirectSound, IDirectSoundBuffer** Out) const;

    unsigned short nChannels;      // +0x04
    unsigned short Field06;        // +0x06
    unsigned short nSamplesPerSec; // +0x08
    unsigned short nBlockAlign;    // +0x0A
    unsigned long LoopStart;       // +0x0C
    unsigned long LoopEnd;         // +0x10
    unsigned long NumFrames;       // +0x14
    float Gain;                    // +0x18
    unsigned long DurationMs;      // +0x1C
    unsigned long Flags;           // +0x20
    IDirectSoundBuffer* Buffer;    // +0x24
    unsigned char bSamplesReady;   // +0x28
    unsigned char Pad29[3];
    unsigned long RefCount;        // +0x2C
};

struct FArchMapEntry
{
    int Id;
    FSoundArchetype* Archetype;
};

// Instance list node 0x60 (the routine). Payload starts at +8.
struct FSoundInstanceNode
{
    FSoundInstanceNode* Next;      // +0x00
    FSoundInstanceNode* Prev;      // +0x04
    ISoundSource* PlayKey;         // +0x08
    IDirectSoundBuffer* Buffer;    // +0x0C
    IDirectSound3DBuffer* Buffer3d;// +0x10
    HANDLE NotifyEvent;            // +0x14 payload+0x0C (the routine)
    unsigned long Pad18;           // +0x18 payload+0x10
    unsigned long FeatureFlags;    // +0x1C payload+0x14 notify offset / loop-end bytes
    unsigned long CapsFlags;       // +0x20 → Status +8 via node+0x28
    unsigned long StatusFlags;     // +0x24
    unsigned long DsCapsFlags;     // +0x28
    float DistSq;                  // +0x2C payload +0x24
    float MaxDistance;             // +0x30
    float MaxDistSq;               // +0x34
    float MinDistance;             // +0x38
    float MinDistSq;               // +0x3C
    FVec3 Position;                // +0x40
    FVec3 Direction;               // +0x4C listener→source (payload +0x44)
    unsigned char bIs3dCache;      // +0x58 payload+0x50
    unsigned char Pad59[3];
    unsigned long LastStartTick;   // +0x5C payload+0x54
};

struct FAttachedNode
{
    FAttachedNode* Next;
    FAttachedNode* Prev;
    ISoundSource* PlayKey;
};

class FSoundManager;

class FSoundInner final : public INeuronComponent
{
public:
    FSoundInner();

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;

    unsigned long RefCount;
    FSoundManager* Self;
};

class FSoundManagerFactory final : public IComponentFactory
{
public:
    FSoundManagerFactory();

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;
    GENRESULT NEURONCOMPONENTS_API CreateInstance(AGGDESC* Desc, void** Instance) override;

    unsigned long RefCount;
    const char* Name;
};

class FSoundManager final : public ISoundManager,
                            public ISoundManagerStatus,
                            public ISoundManagerParserBind
{
public:
    FSoundManager();
    ~FSoundManager();

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;

    GENRESULT NEURONCOMPONENTS_API SetPcmFormat(unsigned short* Fmt) override;
    GENRESULT NEURONCOMPONENTS_API GetPcmFormat(unsigned short* Fmt) override;
    GENRESULT NEURONCOMPONENTS_API SetMasterGain(float Gain) override;
    GENRESULT NEURONCOMPONENTS_API SetMaxChannels(unsigned long Count) override;
    GENRESULT NEURONCOMPONENTS_API SetSpeakerConfig(unsigned long Index) override;
    float NEURONCOMPONENTS_API GetMasterGain() override;
    unsigned long NEURONCOMPONENTS_API GetMaxChannels() override;
    unsigned long NEURONCOMPONENTS_API GetSpeakerConfig() override;
    GENRESULT NEURONCOMPONENTS_API SetEaxPreview(int Env, float Vol, float Decay, float Damp) override;
    GENRESULT NEURONCOMPONENTS_API GetEaxPreview(unsigned long* Env, unsigned long* Vol, unsigned long* Decay,
                                      unsigned long* Damp) override;
    GENRESULT NEURONCOMPONENTS_API SetOrientation(FVec3* Front, FVec3* Up) override;
    GENRESULT NEURONCOMPONENTS_API SetOrientationFromMatrix(float* Matrix) override;
    GENRESULT NEURONCOMPONENTS_API SetListenerPosition(FVec3* Pos) override;
    GENRESULT NEURONCOMPONENTS_API SetListenerVelocity(FVec3* Vel) override;
    GENRESULT NEURONCOMPONENTS_API SetDistanceFactor(float Value) override;
    GENRESULT NEURONCOMPONENTS_API SetDopplerFactor(float Value) override;
    GENRESULT NEURONCOMPONENTS_API SetRolloffFactor(float Value) override;
    GENRESULT NEURONCOMPONENTS_API GetOrientationSpace(FVec3* Pos, FVec3* Vel) override;
    GENRESULT NEURONCOMPONENTS_API GetOrientationMatrix(float* Out9) override;
    GENRESULT NEURONCOMPONENTS_API GetListenerPosition(FVec3* Out) override;
    GENRESULT NEURONCOMPONENTS_API GetListenerVelocity(FVec3* Out) override;
    float NEURONCOMPONENTS_API GetDistanceFactor() override;
    float NEURONCOMPONENTS_API GetDopplerFactor() override;
    float NEURONCOMPONENTS_API GetRolloffFactor() override;
    GENRESULT NEURONCOMPONENTS_API EaxGetInstance(void* Id, GUID* Set, unsigned long Prop, void* Buf,
                                       unsigned long Cb, unsigned long* Pcb) override;
    GENRESULT NEURONCOMPONENTS_API EaxSetInstance(void* Id, GUID* Set, unsigned long Prop, void* Data,
                                       unsigned long Cb) override;
    GENRESULT NEURONCOMPONENTS_API EaxGetPrimary(GUID* Set, unsigned long Prop, void* Buf, unsigned long Cb,
                                      unsigned long* Pcb) override;
    GENRESULT NEURONCOMPONENTS_API EaxSetPrimary(GUID* Set, unsigned long Prop, void* Data,
                                      unsigned long Cb) override;
    int NEURONCOMPONENTS_API CreateArchetypePcm(void* Fmt, unsigned long ByteCount, unsigned long LoopStart,
                                     unsigned long LoopEnd, void* Samples,
                                     unsigned long ArchFlags) override;
    int NEURONCOMPONENTS_API CreateArchetypeFromSource(void* Source, unsigned long ArchFlags) override;
    int NEURONCOMPONENTS_API CreateArchetypeFromWav(INeuronComponent* FileSystem, unsigned long ArchFlags) override;
    GENRESULT NEURONCOMPONENTS_API DestroyArchetype(int Id) override;
    GENRESULT NEURONCOMPONENTS_API QueryArchetype(int Id, void** Out) override;
    GENRESULT NEURONCOMPONENTS_API AttachPlayKey(ISoundSource* PlayKey, unsigned long Index) override;
    GENRESULT NEURONCOMPONENTS_API DetachPlayKey(ISoundSource* PlayKey) override;
    GENRESULT NEURONCOMPONENTS_API ReplaceAttachedList(void** Keys, unsigned long Count) override;
    unsigned long NEURONCOMPONENTS_API GetAttachedCount() override;
    unsigned long NEURONCOMPONENTS_API GetPlayingCount() override;
    GENRESULT NEURONCOMPONENTS_API StartupDevice(HWND Hwnd, unsigned long Flags) override;
    GENRESULT NEURONCOMPONENTS_API ShutdownDevice() override;
    GENRESULT NEURONCOMPONENTS_API GetDirectSound(IDirectSound** Out) override;
    unsigned long NEURONCOMPONENTS_API GetTickCountSlot() override;
    GENRESULT GetDeviceSnapshot(void* Out) override;
    GENRESULT NEURONCOMPONENTS_API MixerTick(ISoundListenerPose* ListenerPose) override;
    GENRESULT NEURONCOMPONENTS_API StopInstanceBuffer(ISoundSource* PlayKey) override;
    GENRESULT NEURONCOMPONENTS_API PlayInstanceBuffer(ISoundSource* PlayKey) override;
    GENRESULT NEURONCOMPONENTS_API GetArchetypeFormat(void* Id, void* Out) override;
    GENRESULT NEURONCOMPONENTS_API GetArchetypeSamples(void* Id, void* Out) override;
    unsigned long NEURONCOMPONENTS_API GetArchetypeFrames(void* Id) override;
    float NEURONCOMPONENTS_API GetArchetypeGain(void* Id) override;
    unsigned long NEURONCOMPONENTS_API GetArchetypeChannels(void* Id) override;
    GENRESULT NEURONCOMPONENTS_API SetArchetypeSamples(void* A, void* B, void* C) override;
    unsigned long NEURONCOMPONENTS_API GetArchetypeReady(void* Id) override;

    GENRESULT NEURONCOMPONENTS_API GetInstanceStatus(int Index, void* Out) override;
    GENRESULT NEURONCOMPONENTS_API BindParser() override;

    void BindOuter(FSoundAggDesc* Desc);
    GENRESULT CreateDirectSound();
    void DumpDeviceNotices();
    void ApplyListener();
    void ReleaseCom();
    FSoundArchetype* LookupArchetype(int Id);
    int InsertArchetype(FSoundArchetype* Archetype);
    void DestroyAllArchetypes();
    void StopInstancesForArch(int ArchId);
    void ClearAttached();
    FSoundInstanceNode* FindInstanceByKey(ISoundSource* PlayKey);
    FSoundInstanceNode* CreateInstanceFromKey(ISoundSource* PlayKey);
    FAttachedNode* FindAttached(ISoundSource* PlayKey);
    void UnlinkInstance(FSoundInstanceNode* Node);
    unsigned char MixerApplyInstance(FSoundInstanceNode* Node);
    void MixerApplyEax(FSoundInstanceNode* Node);
    void RefreshInstancePose(FSoundInstanceNode* Node);
    unsigned char MixerAllowCreate(ISoundSource* PlayKey);
    void LinkPendingInstance(FSoundInstanceNode* Node);
    void SplicePendingInstances();
    unsigned char MixerSetStartOffset(FSoundInstanceNode* Node);
    void ApplyListenerFromSource(ISoundListenerPose* Pose);
    unsigned char OneShotAlreadyExpired(ISoundSource* PlayKey);
    unsigned char SetupInstanceNotify(FSoundInstanceNode* Node);

    unsigned char bParserBound;     // +0x0C
    char DeviceIdString[0x28];      // +0x0D
    GUID DeviceGuid;                // +0x38
    float MasterGain;               // +0x48
    unsigned long Field4C;
    unsigned long Field50;
    unsigned long MaxChannels;      // +0x54
    int EaxEnv;                     // +0x58
    float EaxVol;                   // +0x5C
    float EaxDecay;                 // +0x60
    float EaxDamp;                  // +0x64
    unsigned long Field68;
    unsigned long Field6C;
    unsigned long Field70;
    unsigned long Field74;
    float ReverbMix;                // +0x78
    unsigned long Field7C;          // +0x7C DS3DALG GUID dword0 (ctor the field = 0)
    unsigned long Field80;          // +0x80 the field
    unsigned long Field84;          // +0x84 the field
    unsigned long Field88;          // +0x88 the field
    unsigned long Flags;            // +0x8C
    unsigned long CapsFlags;        // +0x90
    unsigned long Field94;
    unsigned long SampleRate;       // +0x98
    unsigned long Channels;         // +0x9C
    unsigned long BitsPerSample;    // +0xA0
    int CoordScale;                 // +0xA4
    FVec3 Front;                    // +0xA8
    FVec3 Up;                       // +0xB4
    unsigned long PadC0[3];         // +0xC0 ctor zeros
    FVec3 Position;                 // +0xCC
    FVec3 Velocity;                 // +0xD8
    float DistanceFactor;           // +0xE4
    float DopplerFactor;            // +0xE8
    float RolloffFactor;            // +0xEC
    float Max3dPan;                 // +0xF0
    HWND Window;                    // +0xF4 not written by the routine
    FArchMapEntry* ArchEntries;     // +0xF8 baseline the routine std::map
    unsigned long ArchCount;        // +0xFC
    unsigned long ArchCap;          // +0x100
    unsigned char bArchEmpty;       // +0x104
    unsigned char Pad105[3];
    unsigned long Pad108;
    FAttachedNode* AttachedUnused;  // +0x10C list this (the routine size 0xC)
    FAttachedNode* AttachedSentinel;// +0x110
    unsigned long AttachedCount;    // +0x114
    FSoundInstanceNode* InstanceUnused;   // +0x118 list this (the routine size 0x60)
    FSoundInstanceNode* InstanceSentinel; // +0x11C
    unsigned long InstanceEmbeddedCount;  // +0x120 (play count is the field)
    unsigned long NextArchId;       // +0x124
    IDirectSound3DListener* Listener; // +0x128
    IDirectSound* DirectSound;      // +0x12C
    IDirectSoundBuffer* Primary;    // +0x130
    IProfileParser* Parser;         // +0x134
    unsigned char bCoInitialized;   // +0x138
    unsigned char Pad139[3];
    INeuronComponent* Controlling;      // +0x13C
    FSoundInner Inner;              // +0x140
};

static_assert(sizeof(FSoundArchetype) == 0x30, "archetype is 0x30");
static_assert(offsetof(FSoundArchetype, nChannels) == 0x04, "arch nChannels +4");
static_assert(offsetof(FSoundArchetype, Buffer) == 0x24, "arch Buffer +0x24");
static_assert(offsetof(FSoundArchetype, RefCount) == 0x2C, "arch ref +0x2C");
static_assert(sizeof(FSoundInstanceNode) == 0x60, "instance node is 0x60");
static_assert(offsetof(FSoundInstanceNode, DistSq) == 0x2C, "DistSq +0x2C");
static_assert(offsetof(FSoundInstanceNode, MaxDistance) == 0x30, "MaxDistance +0x30");
static_assert(offsetof(FSoundInstanceNode, MinDistance) == 0x38, "MinDistance +0x38");
static_assert(offsetof(FSoundInstanceNode, Position) == 0x40, "Position +0x40");
static_assert(offsetof(FSoundInstanceNode, Direction) == 0x4C, "Direction +0x4C");
static_assert(sizeof(FSoundManagerFactory) == 0x0C, "factory is 0xC");
static_assert(offsetof(FSoundManager, bParserBound) == 0x0C, "bParserBound +0x0C");
static_assert(offsetof(FSoundManager, DeviceGuid) == 0x38, "DeviceGuid +0x38");
static_assert(offsetof(FSoundManager, MasterGain) == 0x48, "MasterGain +0x48");
static_assert(offsetof(FSoundManager, MaxChannels) == 0x54, "MaxChannels +0x54");
static_assert(offsetof(FSoundManager, Field7C) == 0x7C, "DS3DALG +0x7C");
static_assert(offsetof(FSoundManager, Flags) == 0x8C, "Flags +0x8C");
static_assert(offsetof(FSoundManager, SampleRate) == 0x98, "SampleRate +0x98");
static_assert(offsetof(FSoundManager, Front) == 0xA8, "Front +0xA8");
static_assert(offsetof(FSoundManager, Up) == 0xB4, "Up +0xB4");
static_assert(offsetof(FSoundManager, Position) == 0xCC, "Position +0xCC");
static_assert(offsetof(FSoundManager, Window) == 0xF4, "Window +0xF4");
static_assert(offsetof(FSoundManager, AttachedUnused) == 0x10C, "attached list +0x10C");
static_assert(offsetof(FSoundManager, InstanceUnused) == 0x118, "instance list +0x118");
static_assert(offsetof(FSoundManager, NextArchId) == 0x124, "NextArchId +0x124");
static_assert(offsetof(FSoundManager, DirectSound) == 0x12C, "DirectSound +0x12C");
static_assert(offsetof(FSoundManager, Parser) == 0x134, "Parser +0x134");
static_assert(offsetof(FSoundManager, bCoInitialized) == 0x138, "bCoInitialized +0x138");
static_assert(offsetof(FSoundManager, Controlling) == 0x13C, "Controlling +0x13C");
static_assert(offsetof(FSoundManager, Inner) == 0x140, "Inner +0x140");
static_assert(sizeof(FSoundManager) == 0x14C, "object is 0x14C");
static_assert(sizeof(FSoundInner) == 0x0C, "inner is 0x0C");
