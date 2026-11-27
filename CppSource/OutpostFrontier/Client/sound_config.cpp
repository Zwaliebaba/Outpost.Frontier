// Author: Giperion.

#include "sound_config.h"
#include "oracle_mode.h" // Oracle instrumentation (P0.5)

#include "common_ini.h"
#include "neuroncomponents.h"
#include "NeuronRuntime.h"
#include "soundmanager.h"

#include <math.h>
#include <string.h>
#include <windows.h>

namespace
{
    // Defaults are the initialized .data values in baseline (see implementation notes);
    // keys missing from audio\SoundCFG.ini keep them.
    float GMusicFadeTime = 3.f;
    float GCrossFadeSilence = -96.f;
    float GDuckingRtcDownBy = -10.f;
    float GDuckingRtcDownTime = 0.25f;
    float GDuckingRtcUpTime = 1.f;
    float GDuckingSpaceDownBy = -6.f;
    float GDuckingSpaceDownTime = 0.2f;
    float GDuckingSpaceUpTime = 0.75f;
    float GDuckingCommDownBy = -6.f;
    float GDuckingCommDownTime = 0.25f;
    float GDuckingCommUpTime = 0.25f;
    float GDialoguePanRange = 90.f;
    float GDialoguePanFactor = 20.f;
    float GCockpitAttenuation = 0.f;
    int GAudioMode = 0;

    // — baseline object fields +0x10 (current), +0x14 (last reported),
    // +0x18 (target), +0x1c (rate per second), +0x20 (dirty).
    struct FAudioFade
    {
        float Current;
        float Last;
        float Target;
        float Rate;
        unsigned char bDirty;
    };

    FAudioFade GAudioFade = {0.f, 0.f, 0.f, 0.f, 0};

}

void LoadClientSoundCfgIni(char const* FullPath)
{
    // (cdecl path). 0x00427a93..0x00427ad5: an INI_Reader on the stack, open(path,
    // 0) and read_header -- either failing skips straight to the reader's destructor (inlined
    // at 0x00427d6d, which closes the file). The path itself is not tested. Only the "CFG"
    // header (0x005c9e78) is read; `[reverb]` blocks in the same file are not.
    INI_Reader Reader;
    if (!Reader.open(FullPath, false) || !Reader.read_header())
    {
        return;
    }

    do
    {
        if (!Reader.is_header("CFG"))
            continue;
        if (!Reader.read_value())
            continue;
        do
        {
            if (Reader.is_value("COCKPIT_ATTENUATION"))
                GCockpitAttenuation = Reader.get_value_float(0);
            else if (Reader.is_value("ducking_rtc_down_by"))
                GDuckingRtcDownBy = Reader.get_value_float(0);
            else if (Reader.is_value("ducking_rtc_down_time"))
                GDuckingRtcDownTime = Reader.get_value_float(0);
            else if (Reader.is_value("ducking_rtc_up_time"))
                GDuckingRtcUpTime = Reader.get_value_float(0);
            else if (Reader.is_value("ducking_spaceflight_down_by"))
                GDuckingSpaceDownBy = Reader.get_value_float(0);
            else if (Reader.is_value("ducking_spaceflight_down_time"))
                GDuckingSpaceDownTime = Reader.get_value_float(0);
            else if (Reader.is_value("ducking_spaceflight_up_time"))
                GDuckingSpaceUpTime = Reader.get_value_float(0);
            else if (Reader.is_value("ducking_comm_down_by"))
                GDuckingCommDownBy = Reader.get_value_float(0);
            else if (Reader.is_value("ducking_comm_down_time"))
                GDuckingCommDownTime = Reader.get_value_float(0);
            else if (Reader.is_value("ducking_comm_up_time"))
                GDuckingCommUpTime = Reader.get_value_float(0);
            else if (Reader.is_value("music_fade_time"))
                GMusicFadeTime = Reader.get_value_float(0);
            else if (Reader.is_value("cross_fade_silence"))
                GCrossFadeSilence = Reader.get_value_float(0);
            else if (Reader.is_value("spaceflight_dialogue_pan_range"))
                GDialoguePanRange = Reader.get_value_float(0);
            else if (Reader.is_value("spaceflight_dialogue_pan_factor"))
                GDialoguePanFactor = Reader.get_value_float(0);
            else if (Reader.is_value("ear_doppler_factor"))
            {
                // baseline's own NeuronRuntime::Sound null test comes before
                // the value is read.
                if (NeuronRuntime::Sound != nullptr)
                    NeuronRuntime::Sound->SetDopplerFactor(Reader.get_value_float(0)); // +0x48
            }
            else if (Reader.is_value("ear_rolloff_factor"))
            {
                // the same test.
                if (NeuronRuntime::Sound != nullptr)
                    NeuronRuntime::Sound->SetRolloffFactor(Reader.get_value_float(0)); // +0x4c
            }
        } while (Reader.read_value());
    } while (Reader.read_header());
    // the reader's destructor closes the file.
}

void SetClientAudioMode(int Mode)
{
    GAudioMode = Mode;
}

int GetClientAudioMode()
{
    return GAudioMode;
}

void GetClientDuckingConfig(float* OutDownBy, float* OutDownTime, float* OutMusicFadeTime)
{
    // .
    float const DownBy = GAudioMode == 3 ? GDuckingRtcDownBy : GDuckingSpaceDownBy;
    float const DownTime = GAudioMode == 3 ? GDuckingRtcDownTime : GDuckingSpaceDownTime;
    if (OutDownBy != nullptr)
        *OutDownBy = DownBy;
    if (OutDownTime != nullptr)
        *OutDownTime = DownTime;
    if (OutMusicFadeTime != nullptr)
        *OutMusicFadeTime = GMusicFadeTime;
}

float GetClientMusicFadeTime()
{
    return GMusicFadeTime;
}

float GetClientCrossFadeSilence()
{
    return GCrossFadeSilence;
}

float GetClientCockpitAttenuation()
{
    return GCockpitAttenuation;
}

float GetClientSpaceflightDuckDownBy()
{
    return GDuckingSpaceDownBy;
}

float GetClientSpaceflightDuckDownTime()
{
    return GDuckingSpaceDownTime;
}

float GetClientSpaceflightDuckUpTime()
{
    return GDuckingSpaceUpTime;
}

float GetClientCommDuckDownBy()
{
    return GDuckingCommDownBy;
}

float GetClientCommDuckDownTime()
{
    return GDuckingCommDownTime;
}

float GetClientCommDuckUpTime()
{
    return GDuckingCommUpTime;
}

float GetClientDialoguePanRange()
{
    return GDialoguePanRange;
}

float GetClientDialoguePanFactor()
{
    return GDialoguePanFactor;
}

void SnapClientAudioFade(float Value)
{
    // — +0x14 (last) is deliberately left alone, as in baseline.
    GAudioFade.Current = Value;
    GAudioFade.Target = Value;
    GAudioFade.Rate = 0.f;
    GAudioFade.bDirty = 1;
    OracleOnSoundCall("SnapClientAudioFade", nullptr, 0, &Value, 1, -1); // Oracle (P0.5)
}

void SetClientAudioFade(float Target, float Seconds)
{
    float const OracleFloats[2] = {Target, Seconds};                      // Oracle
    OracleOnSoundCall("SetClientAudioFade", nullptr, 0, OracleFloats, 2, -1); // (P0.5)
    // (thiscall on the field, RET 8).
    // FCOMP 0.0, `TEST AH,5` + JP -- not below 0 (or unordered) keeps
    // Seconds; below 0 takes the ducking down_time (the routine(0, &Seconds, 0)).
    if (Seconds < 0.f)
        GetClientDuckingConfig(nullptr, &Seconds, nullptr);

    GAudioFade.Target = Target;
    // FCOMP 0.0, `TEST AH,0x41` + JNZ -- at or below 0, or unordered,
    // the rate is 1e6 (0x49742400); above 0 |Target - Current| / Seconds on the FPU (FSUB,
    // FABS, FDIV), stored as a float once.
    if (!(Seconds > 0.f))
        GAudioFade.Rate = 1000000.f;
    else
        GAudioFade.Rate = static_cast<float>(
            fabs(static_cast<double>(Target) - static_cast<double>(GAudioFade.Current)) /
            static_cast<double>(Seconds));
}

void SetClientAudioFadeTargetRate(float Target, float Rate)
{
    GAudioFade.Target = Target;
    GAudioFade.Rate = Rate;
    float const OracleFloats[2] = {Target, Rate};                                   // Oracle
    OracleOnSoundCall("SetClientAudioFadeTargetRate", nullptr, 0, OracleFloats, 2, -1); // (P0.5)
}

bool TickClientAudioFade(float DeltaSeconds)
{
    // (thiscall on the field, RET 4). 0x00456a40..0x00456a5b: nothing to do
    // when Current equals Target or the rate equals 0 (`TEST AH,0x44` + JNP each; unordered
    // goes on). The step Dt * Rate +/- Current stays on the FPU: FST stores it as a float and
    // the extended value is compared with Target -- the same outcome as comparing the stored
    // float, since Target is a float itself.
    if (GAudioFade.Current != GAudioFade.Target && GAudioFade.Rate != 0.f)
    {
        if (GAudioFade.Target < GAudioFade.Current)
        {
            // above the target -- Current - Dt * Rate (FSUBR), clamped.
            GAudioFade.Current = static_cast<float>(
                static_cast<double>(GAudioFade.Current) -
                static_cast<double>(DeltaSeconds) * static_cast<double>(GAudioFade.Rate));
            if (GAudioFade.Current < GAudioFade.Target)
                GAudioFade.Current = GAudioFade.Target;
        }
        else if (GAudioFade.Target > GAudioFade.Current)
        {
            // below the target -- Dt * Rate + Current, clamped.
            GAudioFade.Current = static_cast<float>(
                static_cast<double>(DeltaSeconds) * static_cast<double>(GAudioFade.Rate) +
                static_cast<double>(GAudioFade.Current));
            if (GAudioFade.Target < GAudioFade.Current)
                GAudioFade.Current = GAudioFade.Target;
        }
    }

    bool const bChanged = GAudioFade.Current != GAudioFade.Last;
    unsigned char const bWasDirty = GAudioFade.bDirty;
    GAudioFade.Last = GAudioFade.Current;
    GAudioFade.bDirty = 0;
    return bChanged || bWasDirty != 0;
}

float GetClientAudioFade()
{
    return GAudioFade.Current;
}
