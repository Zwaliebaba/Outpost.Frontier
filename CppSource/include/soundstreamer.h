// Author: Giperion.

#pragma once

#include "neuroncomponents.h"
#include "audio/dsound8.h"

// soundstreamer.dll public ABI. Source: soundstreamer.dll,
// implementation notes. NeuronRuntime holds the live streamer and drives the device through it;
// everything private to the port stays in src/SoundStreamer/SoundStreamerInternal.h.

struct FStreamerInitDesc
{
    unsigned long Size;        // +0x00 must be 0x18
    IDirectSound* DirectSound; // +0x04
    HWND Window;               // +0x08
    unsigned long Extra;       // +0x0C never read after store
    unsigned long Unused10;    // +0x10
    float Gain;                // +0x14; 0 → 1.0f
};

struct IStreamer2 : INeuronComponent
{
    virtual int NEURONCOMPONENTS_API InitializeDevice(FStreamerInitDesc* Desc) = 0; // +0x0C BOOL
    virtual void* NEURONCOMPONENTS_API CreateStream(const char* Path, INeuronComponent* FileSystem,
                                         unsigned long Flags) = 0; // +0x10
    virtual GENRESULT NEURONCOMPONENTS_API DestroyStream(void* Handle) = 0;   // +0x14
    virtual GENRESULT NEURONCOMPONENTS_API StopStream(void* Handle) = 0;      // +0x18
    virtual GENRESULT NEURONCOMPONENTS_API StartStream(void* Handle) = 0;     // +0x1C
    virtual GENRESULT NEURONCOMPONENTS_API SetVolume(void* Handle, long Volume) = 0; // +0x20
    virtual GENRESULT NEURONCOMPONENTS_API GetVolume(void* Handle, long* Out) = 0;   // +0x24
    virtual unsigned long NEURONCOMPONENTS_API QueryStatus(void* Handle) = 0;        // +0x28
    virtual unsigned long NEURONCOMPONENTS_API GetReserved58() = 0;                  // +0x2C
    virtual unsigned long NEURONCOMPONENTS_API GetReserved5C() = 0;                  // +0x30
    virtual GENRESULT NEURONCOMPONENTS_API SetPan(void* Handle, long Pan) = 0;       // +0x34
    virtual GENRESULT NEURONCOMPONENTS_API GetPan(void* Handle, long* Out) = 0;      // +0x38
};
