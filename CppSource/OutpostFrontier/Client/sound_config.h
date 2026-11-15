// Author: Giperion.

#pragma once

// Client audio configuration + Gf master fade (Client\cmn\Sound.cpp
// and Gf\ScriptSound-family helpers).

// — parse `audio\SoundCFG.ini` `[CFG]` (called at the tail of the client's
//                 OutpostFrontier.ini pass, the routine @ 005b0fc4).
// — ducking triple: down_by / down_time (RTC vs spaceflight) + music_fade_time.
// — audio mode selector (the field; 3 = RTC).
// /d0/a40, the routine — the Gf audio fade object at the field
//                 (dB attenuation animated toward a target; the routine ticks it).

// . Defaults stay in place for keys the file omits.
void LoadClientSoundCfgIni(char const* FullPath);

// / the field. Mode 3 selects the RTC ducking pair.
void SetClientAudioMode(int Mode);
int GetClientAudioMode();

// — any out pointer may be null.
void GetClientDuckingConfig(float* OutDownBy, float* OutDownTime, float* OutMusicFadeTime);

// / the field — used by the [Sound] background writers the routine /
// when the caller passes a negative fade.
float GetClientMusicFadeTime();
float GetClientCrossFadeSilence();
// — `COCKPIT_ATTENUATION`; no consumer ported yet.
float GetClientCockpitAttenuation();
// / the field — `ducking_spaceflight_down_by` / `_down_time`, read directly
// (whatever the audio mode) by the first push of the routine.
float GetClientSpaceflightDuckDownBy();
float GetClientSpaceflightDuckDownTime();
// — `ducking_spaceflight_up_time`, used by the routine on the last pop.
float GetClientSpaceflightDuckUpTime();
// / the field / the field -- `ducking_comm_down_by` / `_down_time` /
// `_up_time`. HUD_Comm's ducking (hud_comm.cpp FCommVoice::Play and every release path) reads
// them directly.
float GetClientCommDuckDownBy();
float GetClientCommDuckDownTime();
float GetClientCommDuckUpTime();
// / the field -- `spaceflight_dialogue_pan_range` / `_pan_factor`, read by the
// comm pan the routine (hud_comm.cpp FCommVoice::Pan).
float GetClientDialoguePanRange();
float GetClientDialoguePanFactor();

// Gf audio fade (the field). Value is in dB: -96 = silent, 0 = full.
// — snap current+target, clear rate, raise the dirty flag.
void SnapClientAudioFade(float Value);
// — target + seconds; seconds <= 0 falls back to the ducking down_time,
// and a zero-length fade stores the baseline 1000000.0f rate.
void SetClientAudioFade(float Target, float Seconds);
// — advance by DeltaSeconds, returns true when the value changed
// (baseline then re-applies volume on every Gf script sound).
bool TickClientAudioFade(float DeltaSeconds);
// The Gf script sequence's direct writes of target (+0x18) and rate (+0x1c): the routine
// (0x004553ba / 0x004553c9) and the routine (0x00455616 / 0x0045561f) store a fixed rate
// instead of the routine's seconds.
void SetClientAudioFadeTargetRate(float Target, float Rate);
float GetClientAudioFade();
