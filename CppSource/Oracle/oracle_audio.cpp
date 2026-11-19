// P4.10: the software mixer of SoundManager.dll, through the reference's own
// code. SoundManager.cpp is compiled into this file (its DllMain renamed), as
// oracle_mesh_data.cpp does with RendComp, so that its file-local helpers
// (QuakeDistance, SoftwareAttenuation, PitchToMultiplier, ScaledDurationMs,
// ParseIniUnsigned, ParseIniFloat) and the FSoundManager members can be
// called directly. The Rust side is crates/neuron-client/tests/oracle_vectors.rs
// with crates/neuron-client/tests/support/audio.rs; the inputs come from
// crates/neuron-client/tests/fixtures/oracle/make_audio_fixtures.py.
//
// Nothing here has a device. The manager is constructed without
// StartupDevice; FOracleDirectSound and FOracleBuffer stand for the device
// and record what the mixer sets, and FOracleSource is a scripted play key.
// GetTickCount is redirected to a value each tick line sets, so MixerTick is
// deterministic. 3D archetypes get buffers without a 3D interface, so every
// voice takes the reference's software path, the one the port keeps.
//
// Line formats (floats as eight hex digits of the f32, '-' for none):
//   audio_apply.jsonl    {"in":..,"dist_sq","dist","atten","dir","pan","freq","volume"}
//   audio_pitch.jsonl    {"in":..,"mult" (16 hex digits of the double),"duration"}
//   audio_start.jsonl    {"in":..,"play","pos","last","freq","restamps"}
//   audio_listener.jsonl {"in":..,"front","up","pos","vel","matrix","space","get_pos","get_vel"}
//   audio_range.jsonl    {"in":..,"allow","expired"}
//   audio_ini.jsonl      {"in":..,"int","float"}
//   audio_eax.jsonl      {"in":..,"env","vol","decay","damp","gain"}
//   audio_tick.jsonl     {"case","arch","flags","buffer","frames","duration"} per archetype,
//                        {"case","tick","instances"} per tick, then per instance in list
//                        order {"case","tick","key","rank","pan","volume","freq","pos","plays"}
//   audio_reverb.jsonl   {"in":..,"step","env","vol","decay","damp"}: the four words GetEaxPreview
//                        hands out, step 0 with the [AudioProperties] values stored as
//                        StartupDevice reads them and no device, step 1 after StartupDevice's
//                        SetEaxPreview of them with a device, then one step per scene-script
//                        SetReverb (SetEaxPreview of its four values)
//
// The client wiring's suites (oracle_audio_client.cpp, audio_arch.jsonl and
// audio_client.jsonl) drive a manager of this file through the bridge functions at its end.

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "oracle_support.h"

namespace
{
    DWORD GOracleTick = 0;
}

static DWORD OracleTickCount()
{
    return GOracleTick;
}

#define GetTickCount OracleTickCount
#define DllMain SoundManagerDllMain
#include "../SoundManager/SoundManager.cpp"
#undef DllMain
#undef GetTickCount

using namespace oracle;

namespace
{
    const GENRESULT kNoValue = static_cast<GENRESULT>(-7);

    std::vector<std::string> Split(const std::string& Text, char Separator)
    {
        std::vector<std::string> Parts;
        size_t Start = 0;
        for (;;)
        {
            const size_t End = Text.find(Separator, Start);
            if (End == std::string::npos)
            {
                Parts.push_back(Text.substr(Start));
                return Parts;
            }
            Parts.push_back(Text.substr(Start, End - Start));
            Start = End + 1;
        }
    }

    bool IsNone(const std::string& Token)
    {
        return Token == "-";
    }

    float HexFloat(const std::string& Token)
    {
        return FloatFromBits(static_cast<unsigned int>(strtoul(Token.c_str(), nullptr, 16)));
    }

    FVec3 HexVec(const std::string& Token)
    {
        const std::vector<std::string> Parts = Split(Token, ',');
        FVec3 Out;
        Out.X = HexFloat(Parts[0]);
        Out.Y = HexFloat(Parts[1]);
        Out.Z = HexFloat(Parts[2]);
        return Out;
    }

    long Dec(const std::string& Token)
    {
        return strtol(Token.c_str(), nullptr, 10);
    }

    unsigned long UDec(const std::string& Token)
    {
        return strtoul(Token.c_str(), nullptr, 10);
    }

    // Every NaN as one pattern: x87 NaN propagation is not part of the port.
    std::string F(float Value)
    {
        const unsigned int Bits = Value != Value ? 0x7FC00000u : FloatBits(Value);
        return Format("%08x", Bits);
    }

    std::string V(const FVec3& Value)
    {
        return F(Value.X) + F(Value.Y) + F(Value.Z);
    }

    std::string Field(const char* Key, const std::string& Value)
    {
        return Format(",\"%s\":\"", Key) + Value + "\"";
    }

    std::string Head(const std::string& Input)
    {
        return "{\"in\":\"" + Input + "\"";
    }

    // A buffer of the device: it keeps a pan, a volume and a playing state, and
    // records the last value of each setter.
    class FOracleBuffer final : public IDirectSoundBuffer
    {
    public:
        DWORD Flags = 0;
        std::vector<unsigned char> Data;
        LONG Pan = 0;
        LONG Volume = 0;
        bool bPlaying = false;
        bool bSetPan = false;
        LONG SetPanValue = 0;
        bool bSetVolume = false;
        LONG SetVolumeValue = 0;
        bool bSetFrequency = false;
        DWORD SetFrequencyValue = 0;
        bool bSetPosition = false;
        DWORD SetPositionValue = 0;
        unsigned int Plays = 0;
        unsigned char Dummy = 0;

        STDMETHODIMP QueryInterface(REFIID, LPVOID* Out) override
        {
            *Out = nullptr;
            return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return 1; }
        STDMETHODIMP_(ULONG) Release() override { return 1; }
        STDMETHODIMP GetCaps(LPDSBCAPS Caps) override
        {
            Caps->dwFlags = Flags;
            Caps->dwBufferBytes = static_cast<DWORD>(Data.size());
            return DS_OK;
        }
        STDMETHODIMP GetCurrentPosition(LPDWORD Play, LPDWORD Write) override
        {
            if (Play != nullptr)
                *Play = 0;
            if (Write != nullptr)
                *Write = 0;
            return DS_OK;
        }
        STDMETHODIMP GetFormat(LPWAVEFORMATEX, DWORD, LPDWORD) override { return DSERR_GENERIC; }
        STDMETHODIMP GetVolume(LPLONG Out) override
        {
            *Out = Volume;
            return DS_OK;
        }
        STDMETHODIMP GetPan(LPLONG Out) override
        {
            *Out = Pan;
            return DS_OK;
        }
        STDMETHODIMP GetFrequency(LPDWORD Out) override
        {
            *Out = 22050;
            return DS_OK;
        }
        STDMETHODIMP GetStatus(LPDWORD Out) override
        {
            *Out = bPlaying ? DSBSTATUS_PLAYING : 0;
            return DS_OK;
        }
        STDMETHODIMP Initialize(LPDIRECTSOUND, LPCDSBUFFERDESC) override { return DS_OK; }
        STDMETHODIMP Lock(DWORD, DWORD, LPVOID* Ptr1, LPDWORD Bytes1, LPVOID* Ptr2, LPDWORD Bytes2,
                          DWORD) override
        {
            *Ptr1 = Data.empty() ? static_cast<void*>(&Dummy) : static_cast<void*>(Data.data());
            *Bytes1 = static_cast<DWORD>(Data.size());
            if (Ptr2 != nullptr)
                *Ptr2 = nullptr;
            if (Bytes2 != nullptr)
                *Bytes2 = 0;
            return DS_OK;
        }
        STDMETHODIMP Play(DWORD, DWORD, DWORD) override
        {
            bPlaying = true;
            ++Plays;
            return DS_OK;
        }
        STDMETHODIMP SetCurrentPosition(DWORD Position) override
        {
            bSetPosition = true;
            SetPositionValue = Position;
            return DS_OK;
        }
        STDMETHODIMP SetFormat(LPCWAVEFORMATEX) override { return DS_OK; }
        STDMETHODIMP SetVolume(LONG Value) override
        {
            Volume = Value;
            bSetVolume = true;
            SetVolumeValue = Value;
            return DS_OK;
        }
        STDMETHODIMP SetPan(LONG Value) override
        {
            Pan = Value;
            bSetPan = true;
            SetPanValue = Value;
            return DS_OK;
        }
        STDMETHODIMP SetFrequency(DWORD Value) override
        {
            bSetFrequency = true;
            SetFrequencyValue = Value;
            return DS_OK;
        }
        STDMETHODIMP Stop() override
        {
            bPlaying = false;
            return DS_OK;
        }
        STDMETHODIMP Unlock(LPVOID, DWORD, LPVOID, DWORD) override { return DS_OK; }
        STDMETHODIMP Restore() override { return DS_OK; }
    };

    // The device: archetype buffers and their duplicates, kept until the case ends.
    class FOracleDirectSound final : public IDirectSound
    {
    public:
        std::vector<std::unique_ptr<FOracleBuffer>> Buffers;

        FOracleBuffer* NewBuffer(DWORD Flags, DWORD Bytes)
        {
            Buffers.push_back(std::unique_ptr<FOracleBuffer>(new FOracleBuffer()));
            FOracleBuffer* Buffer = Buffers.back().get();
            Buffer->Flags = Flags;
            Buffer->Data.assign(Bytes, 0);
            return Buffer;
        }

        STDMETHODIMP QueryInterface(REFIID, LPVOID* Out) override
        {
            *Out = nullptr;
            return E_NOINTERFACE;
        }
        STDMETHODIMP_(ULONG) AddRef() override { return 1; }
        STDMETHODIMP_(ULONG) Release() override { return 1; }
        STDMETHODIMP CreateSoundBuffer(LPCDSBUFFERDESC Desc, LPDIRECTSOUNDBUFFER* Out, LPUNKNOWN) override
        {
            *Out = NewBuffer(Desc->dwFlags, Desc->dwBufferBytes);
            return DS_OK;
        }
        STDMETHODIMP GetCaps(LPDSCAPS) override { return DSERR_GENERIC; }
        STDMETHODIMP DuplicateSoundBuffer(LPDIRECTSOUNDBUFFER Original, LPDIRECTSOUNDBUFFER* Out) override
        {
            *Out = NewBuffer(static_cast<FOracleBuffer*>(Original)->Flags, 0);
            return DS_OK;
        }
        STDMETHODIMP SetCooperativeLevel(HWND, DWORD) override { return DS_OK; }
        STDMETHODIMP Compact() override { return DS_OK; }
        STDMETHODIMP GetSpeakerConfig(LPDWORD) override { return DSERR_GENERIC; }
        STDMETHODIMP SetSpeakerConfig(DWORD) override { return DS_OK; }
        STDMETHODIMP Initialize(LPCGUID) override { return DS_OK; }
    };

    // A scripted play key. A getter without a value answers -7 and writes
    // nothing, except GetPitch, which writes 0 as the client's does.
    class FOracleSource final : public ISoundSource
    {
    public:
        unsigned char bActive = 1;
        unsigned char bIs3dValue = 0;
        unsigned char bLoopingValue = 0;
        int ArchId = -1;
        unsigned long StartTick = 0;
        float Atten = 0.0f;
        bool bHasPitch = false;
        float PitchValue = 0.0f;
        bool bHasPan = true;
        long PanValue = 0;
        bool bHasPos = true;
        FVec3 Pos = {0.0f, 0.0f, 0.0f};
        bool bHasMin = false;
        float MinValue = 0.0f;
        bool bHasMax = false;
        float MaxValue = 0.0f;
        unsigned long Restamps = 0;

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Slot3() override
        {
            ++Restamps;
            return 0;
        }
        unsigned long NEURONCOMPONENTS_API GetStartTick() override { return StartTick; }
        unsigned char NEURONCOMPONENTS_API IsActive() override { return bActive; }
        unsigned char NEURONCOMPONENTS_API Is3d() override { return bIs3dValue; }
        unsigned char NEURONCOMPONENTS_API IsLooping() override { return bLoopingValue; }
        int NEURONCOMPONENTS_API GetArchId() override { return ArchId; }
        GENRESULT NEURONCOMPONENTS_API GetAttenuation(float* Out) override
        {
            *Out = Atten;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetPitch(float* Out) override
        {
            if (!bHasPitch)
            {
                *Out = 0.0f;
                return kNoValue;
            }
            *Out = PitchValue;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetPan(long* Out) override
        {
            if (!bHasPan)
                return kNoValue;
            *Out = PanValue;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetPosition(FVec3* Out) override
        {
            if (!bHasPos)
                return kNoValue;
            *Out = Pos;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetMinDistance(float* Out) override
        {
            if (!bHasMin)
                return kNoValue;
            *Out = MinValue;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetMaxDistance(float* Out) override
        {
            if (!bHasMax)
                return kNoValue;
            *Out = MaxValue;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetConeAngles(unsigned long*, unsigned long*) override { return kNoValue; }
        GENRESULT NEURONCOMPONENTS_API GetConeOrientation(FVec3*) override { return kNoValue; }
        GENRESULT NEURONCOMPONENTS_API GetConeOutsideVolume(float*) override { return kNoValue; }
        GENRESULT NEURONCOMPONENTS_API GetVelocity(FVec3*) override { return kNoValue; }
        GENRESULT NEURONCOMPONENTS_API GetEaxEnvironment(unsigned long*) override { return kNoValue; }
        GENRESULT NEURONCOMPONENTS_API GetSoundMode(unsigned long*) override { return kNoValue; }
        GENRESULT NEURONCOMPONENTS_API GetApplyMode(unsigned long*) override { return kNoValue; }
        unsigned char __thiscall Start(void*, int) override { return 0; }
        unsigned char __thiscall AltStart(void*) override { return 0; }
        void __thiscall Stop() override {}
        void __thiscall SetStartOffset(int) override {}
        void __thiscall Pause() override {}
        void __thiscall Resume() override {}
        unsigned long __thiscall IsPaused() override { return 0; }
        unsigned char __thiscall IsFinished() override { return 0; }
        unsigned long __thiscall Slot30() override { return 0; }
        void __thiscall ComputeMixerScore(const float*) override {}
        void __thiscall SetAttenuation(float) override {}
        void __thiscall SetPan(float) override {}
        void __thiscall SetLooping(unsigned char) override {}
        unsigned long __thiscall Slot35(void*) override { return 0; }
        void __thiscall SetVelocity(const float*) override {}
        void __thiscall SetPose(int, const float*) override {}
        void __thiscall SetPitch(float) override {}
        int __thiscall GetAttachHandle() override { return -1; }
        unsigned char __thiscall Slot40() override { return 0; }
        void* __thiscall ScalarDeletingDestructor(unsigned char) override { return this; }
        unsigned char __thiscall Update(const float*, float) override { return 1; }
    };

    // An instance node outside the lists, as MixerTick's helpers receive it.
    FSoundInstanceNode* NewNode(ISoundSource* Key, IDirectSoundBuffer* Buffer)
    {
        FSoundInstanceNode* Node = static_cast<FSoundInstanceNode*>(calloc(1, sizeof(FSoundInstanceNode)));
        InitInstancePayload(Node);
        Node->PlayKey = Key;
        Node->Buffer = Buffer;
        return Node;
    }

    // The pan and volume a buffer holds before the mixer sets it: a value the
    // mixer never computes for these inputs, so every computed value is set.
    const LONG kUnset = 0x7FFFFFFF;

    std::string SetOr(bool bSet, long Value)
    {
        return bSet ? Format("%ld", Value) : std::string("-");
    }

    std::string SetOrU(bool bSet, unsigned long Value)
    {
        return bSet ? Format("%lu", Value) : std::string("-");
    }

    void ApplySuite()
    {
        Suite Out("audio_apply.jsonl");
        FSoundManager* Mgr = new FSoundManager();
        FSoundArchetype* Arch = new FSoundArchetype();
        Arch->nChannels = 1;
        Arch->nBlockAlign = 2;
        const int ArchId = Mgr->InsertArchetype(Arch);
        for (const std::string& Line : InputLines("audio/apply.list"))
        {
            const std::vector<std::string> T = Split(Line, ' ');
            Mgr->CoordScale = static_cast<int>(Dec(T[0]));
            FVec3 Front = HexVec(T[1]);
            FVec3 Up = HexVec(T[2]);
            FVec3 ListenerPos = HexVec(T[3]);
            Mgr->SetOrientation(&Front, &Up);
            Mgr->SetListenerPosition(&ListenerPos);
            Mgr->SetDistanceFactor(HexFloat(T[4]));
            Mgr->SetRolloffFactor(HexFloat(T[5]));
            Mgr->SetMasterGain(HexFloat(T[6]));
            Arch->Gain = HexFloat(T[7]);
            Arch->nSamplesPerSec = static_cast<unsigned short>(UDec(T[8]));
            Mgr->Max3dPan = HexFloat(T[9]);
            FOracleSource Source;
            Source.ArchId = ArchId;
            Source.bIs3dValue = static_cast<unsigned char>(Dec(T[10]));
            Source.bHasPos = !IsNone(T[11]);
            if (Source.bHasPos)
                Source.Pos = HexVec(T[11]);
            Source.bHasMin = !IsNone(T[12]);
            if (Source.bHasMin)
                Source.MinValue = HexFloat(T[12]);
            Source.bHasMax = !IsNone(T[13]);
            if (Source.bHasMax)
                Source.MaxValue = HexFloat(T[13]);
            Source.Atten = HexFloat(T[14]);
            Source.bHasPitch = !IsNone(T[15]);
            if (Source.bHasPitch)
                Source.PitchValue = HexFloat(T[15]);
            Source.bHasPan = !IsNone(T[16]);
            if (Source.bHasPan)
                Source.PanValue = Dec(T[16]);
            FOracleBuffer Buffer;
            Buffer.Pan = kUnset;
            Buffer.Volume = kUnset;
            FSoundInstanceNode* Node = NewNode(&Source, &Buffer);
            Node->DsCapsFlags = Dec(T[17]) != 0 ? 0x40u : 0u;
            Node->StatusFlags = Dec(T[18]) != 0 ? 0x20u : 0u;
            if (Source.bIs3dValue != 0)
                Mgr->RefreshInstancePose(Node);
            const float DistSq = Node->DistSq;
            const float Dist = QuakeDistance(DistSq);
            std::string Atten = "-";
            if (Source.bIs3dValue != 0 && DistSq > Node->MinDistSq)
                Atten = F(SoftwareAttenuation(Node->MinDistance, Dist, Mgr->DistanceFactor,
                                              Mgr->RolloffFactor, Source.Atten));
            Mgr->MixerApplyInstance(Node);
            Out.Line(Head(Line) + Field("dist_sq", F(DistSq)) + Field("dist", F(Dist)) +
                     Field("atten", Atten) + Field("dir", V(Node->Direction)) +
                     Field("pan", SetOr(Buffer.bSetPan, Buffer.SetPanValue)) +
                     Field("freq", SetOrU(Buffer.bSetFrequency, Buffer.SetFrequencyValue)) +
                     Field("volume", SetOr(Buffer.bSetVolume, Buffer.SetVolumeValue)) + "}");
            free(Node);
        }
        delete Mgr;
    }

    void PitchSuite()
    {
        Suite Out("audio_pitch.jsonl");
        for (const std::string& Line : InputLines("audio/pitch.list"))
        {
            const std::vector<std::string> T = Split(Line, ' ');
            FOracleSource Source;
            Source.bHasPitch = !IsNone(T[0]);
            if (Source.bHasPitch)
                Source.PitchValue = HexFloat(T[0]);
            FSoundArchetype Arch;
            Arch.DurationMs = UDec(T[1]);
            Arch.Flags = strtoul(T[2].c_str(), nullptr, 16);
            std::string Mult = "-";
            if (Source.bHasPitch)
            {
                const double Value = PitchToMultiplier(Source.PitchValue);
                Mult = Format("%016llx", Value != Value ? 0x7FF8000000000000ull : DoubleBits(Value));
            }
            const unsigned long Duration = ScaledDurationMs(&Arch, &Source);
            Out.Line(Head(Line) + Field("mult", Mult) + Field("duration", Format("%lu", Duration)) + "}");
        }
    }

    void StartSuite()
    {
        Suite Out("audio_start.jsonl");
        FSoundManager* Mgr = new FSoundManager();
        FSoundArchetype* Arch = new FSoundArchetype();
        Arch->nChannels = 1;
        const int ArchId = Mgr->InsertArchetype(Arch);
        for (const std::string& Line : InputLines("audio/start.list"))
        {
            const std::vector<std::string> T = Split(Line, ' ');
            Mgr->Field4C = UDec(T[0]);
            FOracleSource Source;
            Source.ArchId = ArchId;
            Source.StartTick = UDec(T[1]);
            Source.bLoopingValue = static_cast<unsigned char>(Dec(T[5]));
            Source.bHasPitch = !IsNone(T[6]);
            if (Source.bHasPitch)
                Source.PitchValue = HexFloat(T[6]);
            Arch->nSamplesPerSec = static_cast<unsigned short>(UDec(T[7]));
            Arch->nBlockAlign = static_cast<unsigned short>(UDec(T[8]));
            Arch->NumFrames = UDec(T[9]);
            Arch->LoopStart = UDec(T[10]);
            Arch->LoopEnd = UDec(T[11]);
            FOracleBuffer Buffer;
            Buffer.bPlaying = Dec(T[12]) != 0;
            G_NeverSetStartPos = UDec(T[13]);
            FSoundInstanceNode* Node = NewNode(&Source, &Buffer);
            Node->LastStartTick = UDec(T[2]);
            Node->StatusFlags = (Dec(T[3]) != 0 ? 0x40u : 0u) | (Dec(T[4]) != 0 ? 0x20u : 0u);
            const unsigned char Play = Mgr->MixerSetStartOffset(Node);
            Out.Line(Head(Line) + Field("play", Format("%u", Play)) +
                     Field("pos", SetOrU(Buffer.bSetPosition, Buffer.SetPositionValue)) +
                     Field("last", Format("%lu", Node->LastStartTick)) +
                     Field("freq", (Node->StatusFlags & 0x20) != 0 ? "1" : "0") +
                     Field("restamps", Format("%lu", Source.Restamps)) + "}");
            free(Node);
        }
        G_NeverSetStartPos = 0;
        delete Mgr;
    }

    void ListenerSuite()
    {
        Suite Out("audio_listener.jsonl");
        for (const std::string& Line : InputLines("audio/listener.list"))
        {
            const std::vector<std::string> T = Split(Line, ' ');
            FSoundManager* Mgr = new FSoundManager();
            Mgr->CoordScale = static_cast<int>(Dec(T[1]));
            if (Dec(T[2]) != 0)
                Mgr->Flags |= 0x10;
            else
                Mgr->Flags &= ~0x10u;
            size_t Next = 3;
            if (T[0] == "m")
            {
                const std::vector<std::string> M = Split(T[3], ',');
                float Matrix[9];
                for (int Index = 0; Index < 9; ++Index)
                    Matrix[Index] = HexFloat(M[Index]);
                Mgr->SetOrientationFromMatrix(Matrix);
                Next = 4;
            }
            else
            {
                FVec3 Front = HexVec(T[3]);
                FVec3 Up = HexVec(T[4]);
                Mgr->SetOrientation(&Front, &Up);
                Next = 5;
            }
            FVec3 Pos = HexVec(T[Next]);
            FVec3 Vel = HexVec(T[Next + 1]);
            Mgr->SetListenerPosition(&Pos);
            Mgr->SetListenerVelocity(&Vel);
            float Matrix[9];
            Mgr->GetOrientationMatrix(Matrix);
            std::string MatrixText;
            for (int Index = 0; Index < 9; ++Index)
                MatrixText += F(Matrix[Index]);
            FVec3 SpaceFront;
            FVec3 SpaceUp;
            Mgr->GetOrientationSpace(&SpaceFront, &SpaceUp);
            FVec3 GetPos;
            FVec3 GetVel;
            Mgr->GetListenerPosition(&GetPos);
            Mgr->GetListenerVelocity(&GetVel);
            Out.Line(Head(Line) + Field("front", V(Mgr->Front)) + Field("up", V(Mgr->Up)) +
                     Field("pos", V(Mgr->Position)) + Field("vel", V(Mgr->Velocity)) +
                     Field("matrix", MatrixText) + Field("space", V(SpaceFront) + V(SpaceUp)) +
                     Field("get_pos", V(GetPos)) + Field("get_vel", V(GetVel)) + "}");
            delete Mgr;
        }
    }

    void RangeSuite()
    {
        Suite Out("audio_range.jsonl");
        for (const std::string& Line : InputLines("audio/range.list"))
        {
            const std::vector<std::string> T = Split(Line, ' ');
            FSoundManager* Mgr = new FSoundManager();
            FSoundArchetype* Arch = new FSoundArchetype();
            Arch->Flags = strtoul(T[3].c_str(), nullptr, 16);
            Arch->DurationMs = UDec(T[4]);
            const int ArchId = Mgr->InsertArchetype(Arch);
            Mgr->Field4C = UDec(T[0]);
            Mgr->CoordScale = static_cast<int>(Dec(T[7]));
            FVec3 ListenerPos = HexVec(T[8]);
            Mgr->SetListenerPosition(&ListenerPos);
            FOracleSource Source;
            Source.ArchId = Dec(T[11]) != 0 ? ArchId : -1;
            Source.bLoopingValue = static_cast<unsigned char>(Dec(T[1]));
            Source.bIs3dValue = static_cast<unsigned char>(Dec(T[2]));
            Source.bHasPitch = !IsNone(T[5]);
            if (Source.bHasPitch)
                Source.PitchValue = HexFloat(T[5]);
            Source.StartTick = UDec(T[6]);
            Source.Pos = HexVec(T[9]);
            Source.bHasMax = !IsNone(T[10]);
            if (Source.bHasMax)
                Source.MaxValue = HexFloat(T[10]);
            const unsigned char Allow = Mgr->MixerAllowCreate(&Source);
            const unsigned char Expired = Mgr->OneShotAlreadyExpired(&Source);
            Out.Line(Head(Line) + Field("allow", Format("%u", Allow)) +
                     Field("expired", Format("%u", Expired)) + "}");
            delete Mgr;
        }
    }

    void IniSuite()
    {
        Suite Out("audio_ini.jsonl");
        for (const std::string& Line : InputLines("audio/ini.list"))
        {
            const Bytes Value = Unhex(Line);
            char IntText[0x420];
            char FloatText[0x420];
            strncpy(IntText, CStr(Value), sizeof(IntText) - 1);
            IntText[sizeof(IntText) - 1] = '\0';
            strcpy(FloatText, IntText);
            const unsigned long Int = ParseIniUnsigned(IntText);
            const float Float = ParseIniFloat(FloatText);
            Out.Line(Head(Line) + Field("int", Format("%08lx", Int)) + Field("float", F(Float)) + "}");
        }
    }

    void EaxSuite()
    {
        Suite Out("audio_eax.jsonl");
        FSoundManager* Mgr = new FSoundManager();
        FOracleDirectSound Device;
        // SetEaxPreview stores its clamps once a device exists, then fails
        // because useEAX (flag 4) is off; the device is never called.
        Mgr->DirectSound = &Device;
        Mgr->Flags &= ~4u;
        for (const std::string& Line : InputLines("audio/eax.list"))
        {
            const std::vector<std::string> T = Split(Line, ' ');
            Mgr->SetEaxPreview(static_cast<int>(Dec(T[0])), HexFloat(T[1]), HexFloat(T[2]),
                               HexFloat(T[3]));
            FSoundArchetype Arch;
            Arch.SetGain(HexFloat(T[4]));
            Out.Line(Head(Line) + Field("env", Format("%d", Mgr->EaxEnv)) + Field("vol", F(Mgr->EaxVol)) +
                     Field("decay", F(Mgr->EaxDecay)) + Field("damp", F(Mgr->EaxDamp)) +
                     Field("gain", F(Arch.Gain)) + "}");
        }
        Mgr->DirectSound = nullptr;
        delete Mgr;
    }

    // One scripted session of audio/tick.list.
    struct FTickCase
    {
        int Index = -1;
        FSoundManager* Mgr = nullptr;
        std::unique_ptr<FOracleDirectSound> Device;
        std::map<long, std::unique_ptr<FOracleSource>> Sources;
        std::vector<int> ArchIds;

        void Finish()
        {
            if (Mgr != nullptr)
            {
                delete Mgr;
                Mgr = nullptr;
            }
            Sources.clear();
            Device.reset();
            ArchIds.clear();
        }
    };

    std::vector<long> KeyList(const std::string& Token)
    {
        std::vector<long> Keys;
        if (IsNone(Token))
            return Keys;
        for (const std::string& Part : Split(Token, ','))
            Keys.push_back(Dec(Part));
        return Keys;
    }

    void TickSuite()
    {
        Suite Out("audio_tick.jsonl");
        FTickCase Case;
        for (const std::string& Line : InputLines("audio/tick.list"))
        {
            const std::vector<std::string> T = Split(Line, ' ');
            const std::string& Command = T[0];
            if (Command == "case")
            {
                Case.Finish();
                ++Case.Index;
                Case.Device.reset(new FOracleDirectSound());
                Case.Mgr = new FSoundManager();
                Case.Mgr->DirectSound = Case.Device.get();
                Case.Mgr->SetMaxChannels(UDec(T[1]));
                Case.Mgr->SetMasterGain(HexFloat(T[2]));
                Case.Mgr->SetDistanceFactor(HexFloat(T[3]));
                Case.Mgr->SetRolloffFactor(HexFloat(T[4]));
                Case.Mgr->CoordScale = static_cast<int>(Dec(T[5]));
                Case.Mgr->Max3dPan = HexFloat(T[6]);
            }
            else if (Command == "listener")
            {
                FVec3 Front = HexVec(T[1]);
                FVec3 Up = HexVec(T[2]);
                FVec3 Pos = HexVec(T[3]);
                Case.Mgr->SetOrientation(&Front, &Up);
                Case.Mgr->SetListenerPosition(&Pos);
            }
            else if (Command == "arch")
            {
                const unsigned long Flags = strtoul(T[1].c_str(), nullptr, 16);
                unsigned short Fmt8[4];
                Fmt8[0] = static_cast<unsigned short>(UDec(T[2]));
                Fmt8[1] = static_cast<unsigned short>(UDec(T[3]));
                Fmt8[2] = static_cast<unsigned short>(UDec(T[4]));
                Fmt8[3] = static_cast<unsigned short>(UDec(T[5]));
                const unsigned long ByteCount = UDec(T[6]) * Fmt8[3];
                std::vector<unsigned char> Samples(ByteCount + 1, 0);
                const int Id = Case.Mgr->CreateArchetypePcm(Fmt8, ByteCount, UDec(T[7]), UDec(T[8]),
                                                            Samples.data(), Flags);
                Case.ArchIds.push_back(Id);
                FSoundArchetype* Arch = Case.Mgr->LookupArchetype(Id);
                if (Arch == nullptr)
                {
                    fprintf(stderr, "audio_tick: archetype %d was not created\n", Id);
                    GFailed = true;
                    continue;
                }
                Arch->SetGain(HexFloat(T[9]));
                Out.Line(Format("{\"case\":\"%d\",\"arch\":\"%d\",\"flags\":\"%lx\",\"buffer\":\"%lx\","
                                "\"frames\":\"%lu\",\"duration\":\"%lu\"}",
                                Case.Index, Id, Arch->Flags,
                                static_cast<unsigned long>(static_cast<FOracleBuffer*>(Arch->Buffer)->Flags),
                                Arch->NumFrames, Arch->DurationMs));
                Arch->Release();
            }
            else if (Command == "src")
            {
                FOracleSource* Source = new FOracleSource();
                Source->bIs3dValue = static_cast<unsigned char>(Dec(T[2]));
                Source->bLoopingValue = static_cast<unsigned char>(Dec(T[3]));
                const long ArchIndex = Dec(T[4]);
                Source->ArchId = ArchIndex >= 0 ? Case.ArchIds[static_cast<size_t>(ArchIndex)] : -1;
                Case.Sources[Dec(T[1])].reset(Source);
            }
            else if (Command == "set")
            {
                FOracleSource* Source = Case.Sources[Dec(T[1])].get();
                const std::string& Name = T[2];
                const std::string& Value = T[3];
                if (Name == "active")
                    Source->bActive = static_cast<unsigned char>(Dec(Value));
                else if (Name == "start")
                    Source->StartTick = UDec(Value);
                else if (Name == "looping")
                    Source->bLoopingValue = static_cast<unsigned char>(Dec(Value));
                else if (Name == "atten")
                    Source->Atten = HexFloat(Value);
                else if (Name == "pitch")
                {
                    Source->bHasPitch = !IsNone(Value);
                    if (Source->bHasPitch)
                        Source->PitchValue = HexFloat(Value);
                }
                else if (Name == "pan")
                {
                    Source->bHasPan = !IsNone(Value);
                    if (Source->bHasPan)
                        Source->PanValue = Dec(Value);
                }
                else if (Name == "pos")
                {
                    Source->bHasPos = !IsNone(Value);
                    if (Source->bHasPos)
                        Source->Pos = HexVec(Value);
                }
                else if (Name == "min")
                {
                    Source->bHasMin = !IsNone(Value);
                    if (Source->bHasMin)
                        Source->MinValue = HexFloat(Value);
                }
                else if (Name == "max")
                {
                    Source->bHasMax = !IsNone(Value);
                    if (Source->bHasMax)
                        Source->MaxValue = HexFloat(Value);
                }
                else
                {
                    fprintf(stderr, "audio_tick: unknown field %s\n", Name.c_str());
                    GFailed = true;
                }
            }
            else if (Command == "tick")
            {
                const unsigned long Now = UDec(T[1]);
                const std::vector<long> Attached = KeyList(T[2]);
                const std::vector<long> Stopped = KeyList(T[3]);
                for (long Key : Stopped)
                {
                    ISoundSource* Source = Case.Sources[Key].get();
                    for (FSoundInstanceNode* Node = G_InstanceSentinel.Next; Node != &G_InstanceSentinel;
                         Node = Node->Next)
                    {
                        if (Node->PlayKey == Source)
                            static_cast<FOracleBuffer*>(Node->Buffer)->bPlaying = false;
                    }
                }
                std::vector<void*> Keys;
                for (long Key : Attached)
                    Keys.push_back(static_cast<ISoundSource*>(Case.Sources[Key].get()));
                void* Empty = nullptr;
                Case.Mgr->ReplaceAttachedList(Keys.empty() ? &Empty : Keys.data(),
                                              static_cast<unsigned long>(Keys.size()));
                GOracleTick = Now;
                Case.Mgr->MixerTick(nullptr);
                Out.Line(Format("{\"case\":\"%d\",\"tick\":\"%lu\",\"instances\":\"%lu\"}", Case.Index, Now,
                                G_InstanceCount));
                for (FSoundInstanceNode* Node = G_InstanceSentinel.Next; Node != &G_InstanceSentinel;
                     Node = Node->Next)
                {
                    long Key = -1;
                    for (const auto& Entry : Case.Sources)
                    {
                        if (static_cast<ISoundSource*>(Entry.second.get()) == Node->PlayKey)
                            Key = Entry.first;
                    }
                    const FOracleBuffer* Buffer = static_cast<FOracleBuffer*>(Node->Buffer);
                    Out.Line(Format("{\"case\":\"%d\",\"tick\":\"%lu\",\"key\":\"%ld\",\"rank\":\"%lu\"",
                                    Case.Index, Now, Key, Node->CapsFlags) +
                             Field("pan", SetOr(Buffer->bSetPan, Buffer->SetPanValue)) +
                             Field("volume", SetOr(Buffer->bSetVolume, Buffer->SetVolumeValue)) +
                             Field("freq", SetOrU(Buffer->bSetFrequency, Buffer->SetFrequencyValue)) +
                             Field("pos", SetOrU(Buffer->bSetPosition, Buffer->SetPositionValue)) +
                             Field("plays", Format("%u", Buffer->Plays)) + "}");
                }
            }
            else
            {
                fprintf(stderr, "audio_tick: unknown command %s\n", Command.c_str());
                GFailed = true;
            }
        }
        Case.Finish();
    }
}

// The bridge of oracle_audio_client.cpp: a manager with the recording device, its clock, its
// instances and its archetypes.
struct FOracleVoiceInfo
{
    void* Key;
    unsigned long Rank;
    bool bPan;
    long Pan;
    bool bVolume;
    long Volume;
    bool bFreq;
    unsigned long Freq;
    bool bPos;
    unsigned long Pos;
    unsigned int Plays;
};

namespace
{
    struct FBridgedManager
    {
        FSoundManager* Manager;
        FOracleDirectSound* Device;
    };
    std::vector<FBridgedManager> GBridged;
}

ISoundManager* OracleAudioCreateManager(unsigned long MaxChannels)
{
    FBridgedManager Entry;
    Entry.Device = new FOracleDirectSound();
    Entry.Manager = new FSoundManager();
    Entry.Manager->DirectSound = Entry.Device;
    Entry.Manager->SetMaxChannels(MaxChannels);
    GBridged.push_back(Entry);
    return Entry.Manager;
}

void OracleAudioDestroyManager(ISoundManager* Manager)
{
    for (size_t Index = 0; Index < GBridged.size(); ++Index)
    {
        if (static_cast<ISoundManager*>(GBridged[Index].Manager) != Manager)
            continue;
        delete GBridged[Index].Manager;
        delete GBridged[Index].Device;
        GBridged.erase(GBridged.begin() + static_cast<long>(Index));
        return;
    }
}

void OracleAudioSetTick(unsigned long Now)
{
    GOracleTick = Now;
}

unsigned long OracleAudioVoices(FOracleVoiceInfo* Out, unsigned long Max)
{
    unsigned long Count = 0;
    for (FSoundInstanceNode* Node = G_InstanceSentinel.Next; Node != &G_InstanceSentinel && Count < Max;
         Node = Node->Next)
    {
        const FOracleBuffer* Buffer = static_cast<FOracleBuffer*>(Node->Buffer);
        FOracleVoiceInfo& V = Out[Count++];
        V.Key = Node->PlayKey;
        V.Rank = Node->CapsFlags;
        V.bPan = Buffer->bSetPan;
        V.Pan = Buffer->SetPanValue;
        V.bVolume = Buffer->bSetVolume;
        V.Volume = Buffer->SetVolumeValue;
        V.bFreq = Buffer->bSetFrequency;
        V.Freq = Buffer->SetFrequencyValue;
        V.bPos = Buffer->bSetPosition;
        V.Pos = Buffer->SetPositionValue;
        V.Plays = Buffer->Plays;
    }
    return Count;
}

bool OracleAudioArchetypeInfo(ISoundManager* Manager, int Id, unsigned long* Flags, unsigned long* Buffer,
                              unsigned long* Frames, unsigned long* Duration)
{
    FSoundArchetype* Arch = static_cast<FSoundManager*>(Manager)->LookupArchetype(Id);
    if (Arch == nullptr)
        return false;
    *Flags = Arch->Flags;
    *Buffer = static_cast<unsigned long>(static_cast<FOracleBuffer*>(Arch->Buffer)->Flags);
    *Frames = Arch->NumFrames;
    *Duration = Arch->DurationMs;
    Arch->Release();
    return true;
}

namespace
{
    void ReverbSuite()
    {
        Suite Out("audio_reverb.jsonl");
        for (const std::string& Line : InputLines("audio/reverb.list"))
        {
            const std::vector<std::string> Groups = Split(Line, ';');
            FSoundManager* Mgr = new FSoundManager();
            FOracleDirectSound Device;
            Mgr->Flags &= ~4u;
            auto Emit = [&](int Step) {
                unsigned long Env = 0;
                unsigned long Vol = 0;
                unsigned long Decay = 0;
                unsigned long Damp = 0;
                Mgr->GetEaxPreview(&Env, &Vol, &Decay, &Damp);
                Out.Line(Head(Line) + Field("step", Format("%d", Step)) + Field("env", Format("%08lx", Env)) +
                         Field("vol", Format("%08lx", Vol)) + Field("decay", Format("%08lx", Decay)) +
                         Field("damp", Format("%08lx", Damp)) + "}");
            };
            for (size_t Index = 0; Index < Groups.size(); ++Index)
            {
                const std::vector<std::string> T = Split(Groups[Index], ' ');
                const int Env = static_cast<int>(Dec(T[0]));
                if (Index == 0)
                {
                    // StartupDevice's INI reads store the values as they are.
                    Mgr->EaxEnv = Env;
                    Mgr->EaxVol = HexFloat(T[1]);
                    Mgr->EaxDecay = HexFloat(T[2]);
                    Mgr->EaxDamp = HexFloat(T[3]);
                    Emit(0);
                    Mgr->DirectSound = &Device;
                    Mgr->SetEaxPreview(Mgr->EaxEnv, Mgr->EaxVol, Mgr->EaxDecay, Mgr->EaxDamp);
                    Emit(1);
                }
                else
                {
                    Mgr->SetEaxPreview(Env, HexFloat(T[1]), HexFloat(T[2]), HexFloat(T[3]));
                    Emit(static_cast<int>(Index) + 1);
                }
            }
            Mgr->DirectSound = nullptr;
            delete Mgr;
        }
    }
}

// oracle_audio_client.cpp.
void RunAudioClientSuites();

void RunAudioSuites()
{
    ApplySuite();
    PitchSuite();
    StartSuite();
    ListenerSuite();
    RangeSuite();
    IniSuite();
    EaxSuite();
    TickSuite();
    ReverbSuite();
    RunAudioClientSuites();
}
