// Author: Giperion.

#pragma once

#include "neuroncomponents.h"
#include "sound_archetype.h"

// Client SoundArch table (SoundArch.cpp). the routine loads [Sound] from
// OutpostFrontier.ini Data sounds=; the routine looks up by CreateID.
// creates the ISoundManager wav archetype into SmArchId.
// Heap play: the routine / the routine / the routine / the routine.
// Not SoundManager.dll internals.

// read every [Sound] of the ini into the registry (the field); returns how many
// SoundArchs it added. bForce = baseline's second argument (0 from the one caller, 0x005b0d6f),
// handed on to the redefine the routine.
int LoadClientSoundArchIni(char const* FullPath, bool bForce);
// the registered SoundArch of Id; null (with the 0x168 "unknown Sound archetype"
// WARNING, which names Id->get_string()) for an empty or unknown id.
void* LookupClientSoundArch(struct ID_String const* Id);
// every registered SoundArch whose persistence bits ((flags >> 20) & Mask & 0xf)
// match and whose SoundManager id is still -1 is acquired once. Mask 1 from the space entry
// (0x0054ac99), 2 from the base entry (0x005588e9).
void PreloadClientSoundArchesByCategory(unsigned Mask);
// the same walk and test (SoundManager id -1 included), releasing once. Mask 1
// from the space exit (0x0054afde), 2 from the base exit (0x00558a80).
void UnloadClientSoundArchesByCategory(unsigned Mask);

// — app-shutdown teardown: force-release every archetype's wav references and
// free the whole registry. baseline calls this from WinMain's teardown chain (0x5b22af), right
// after the voice registry drop and before the rest of shutdown.
void ShutdownClientSoundArch();

// — slot 3: create wav if needed, bump UseCount.
unsigned char AcquireClientSoundArch(void* Arch);
// — slot 4.
unsigned char ReleaseClientSoundArch(void* Arch);

// — look the archetype up by ID_String (the routine) and Acquire it (slot 3).
// Returns 0 for an empty id or an unknown archetype. Caller: StarSys [Archetype] snd =.
unsigned char PreloadClientSoundArch(struct ID_String const* Id);
// — the matching Release (slot 4). Caller: the StarSys leave the routine.
void UnloadClientSoundArch(struct ID_String const* Id);

// / the routine — alloc Sound::Source3D (ISoundSource*). Caller owns one
// ref; Release when done. createeffect factory vt+4.
void* CreateClientSoundSource3D();

// alloc a plain Sound::Source (0x30, the routine(1)). Slot 1 of the effect
// factories the field and the field: the tradelane camera flash plays unpositioned.
void* CreateClientSoundSource();

class FClientEquippedObj;
struct ISoundSource;

// (0x50) + the routine -- a CliThrusterSound (vtable 005ddbfc) for the ship
// (0x00545535): exterior archetype always, interior too for this client's own ship, looping.
// Null when the allocation fails. The ship holds it at +0xf0.
ISoundSource* CreateClientThrusterSound(FClientEquippedObj* Ship);
// (RET 4) -- the per-update tick (0x00545567): the working thrusters' summed
// thrust sets the level and their mean position the 3D position; the player's first-person
// view switches to the interior sound. Dt is not read.
void TickClientThrusterSound(ISoundSource* Sound, float Dt);

// — Source3D + Acquire(Arch). Engine equip emitters (baseline sizeof 0x68, vtable
// , arch reference held at +0x64 and released by its destructor the routine);
// gun/InternalFX use CreateClientSoundSource3D + Start separately. Returns ISoundSource*
// (null only when the allocation fails); caller Release when done. Arch must not be null:
// baseline calls its Acquire unconditionally.
void* CreateClientEquipSoundEmitter(void* Arch);
// That emitter's +0x64 archetype (the routine restarts the emitter with it through
// , 0x005282e2 / 0x005283f1).
void* GetClientEquipSoundEmitterArch(void* Emitter);

// . PlayKey layout matches listing (flags @ +4). OutHandle = param_3.
unsigned char PlayClientSoundArch(void* Arch, unsigned* PlayKey, void** OutHandle,
                                  unsigned char bAltStart);
// (Sound::StreamedSource) -- restart a streamed source on Arch at Offset and kick
// its stream. Caller: the Gf script's streamed sound events (FGfScript::PlayStreamedSoundEvent,
// 0x0044f707). The answer is only whether the source holds a stream handle.
unsigned char RestartClientStreamedSound(ISoundSource* Handle, void* Arch, int Offset);
// (Arch, PlayKey, OutHandle): if any live source already plays Arch, return 0;
// otherwise the routine(Arch, PlayKey, OutHandle, 0).
unsigned char PlayClientSoundArchIfIdle(void* Arch, unsigned* PlayKey, void** OutHandle);
// (Arch, PlayKey, OutHandle): the first live source playing Arch is rewound through
// vt+0x64 SetStartOffset(0); with none, the routine(Arch, PlayKey, OutHandle, 0).
void PlayOrRewindClientSoundArch(void* Arch, unsigned* PlayKey, void** OutHandle);
// (Arch): every live source whose +0x18 is Arch gets vt+0x60 (Stop); the next
// node is read before the call.
void StopClientSoundArch(void* Arch);
// (KeepArch) (cdecl) -- stop and drop the background music / ambience sources, Stop
// every live source whose archetype is not KeepArch, SoundManager MixerTick(0), reset the
// ducking state. Only caller: the routine.
void StopAllClientSoundsExceptArch(void* KeepArch);
// (cdecl) -- the routine(0), then the "sound reference count leaks" warning when
// Sound::Source instances are still alive (the field), then Heap::Shrink on the three source
// pools (the field / a34 / a24). Callers: 0x0054afd9 (space exit), 0x00558a7b (base exit),
// (shutdown).
void StopAllClientSounds();
// / the routine (cdecl) -- every playing source (the field, from the front)
// paused (slot 26), resp. every paused one (slot 28) resumed (slot 27). Only caller:
// (the single-player server-tick pause) on its 0 -> 1 and 1 -> 0 edges.
void PauseAllClientSounds();
void ResumePausedClientSounds();
// (Arch) -- how many live sources play Arch. Caller: the routine (the UI sound
// table's "anything still playing" test).
int CountClientSoundArchSources(void* Arch);

// / the routine: copy global BGM/ambient source.
unsigned char BindClientBackgroundMusic(void** OutSource);
unsigned char BindClientBackgroundAmbient(void** OutSource);
// / the routine — the field / the field writers.
unsigned char SetClientBackgroundMusic(unsigned NicknameId, unsigned Flags, float Fade);
unsigned char SetClientBackgroundAmbient(unsigned NicknameId, unsigned Flags, float Fade);
// / the routine (cdecl) -- fade the background music / ambience source to
// TargetDb at Rate dB per second (+0x34 target, +0x38 = |target - current| / Rate). A target
// above 1e8 means the ducking down_by, a rate above 1e8 the ducking down_time as the time.
// Unused is never read. Callers: the Gf script sequence setup / stop (the routine /
// ).
void FadeClientBackgroundMusic(int Unused, float TargetDb, float Rate);
void FadeClientBackgroundAmbient(int Unused, float TargetDb, float Rate);

// — per-frame client sound mixer: global ducking fade, listener update,
// audible-source collection, ranking by score (+0x28) and ISoundManager 38/46.
// Camera is baseline's listener source (vtable +0xC/+0x10); null falls back to SM slot 22.
void TickClientSoundMixer(void* Camera, float DeltaSeconds);

// / the routine — the field: ICamera* the mixer reads for listener
// velocity (slot 16 / GetVelocity @ +0x48). Space camera mode switch writes it.
void* GetClientSoundListenerObject();
void SetClientSoundListenerObject(void* CameraOrNull);

// Sound::Source3D spatial setters. The cone pair is the routine / the routine (both
// non-virtual in baseline); the pose is vtable slot 37 (the routine), whose Transform argument
// is a 3x3 orientation followed by the world position at +0x24.
void SetClientSoundSourceCone(void* SourcePtr, float InnerAngle, float OuterAngle);
void SetClientSoundSourceConeOutsideVolume(void* SourcePtr, float Volume);
void SetClientSoundSourcePose(void* SourcePtr, int AttachHandle, float const* Transform);
// The source's +0x04 category (0 sfx, 1 interface, 2/5 voice, 3 music, 4 ambient), written
// directly by callers after the routine (HUD_Comm 0x004cb0f6 / 0x004cb1f4 store 2).
void SetClientSoundSourceType(void* SourcePtr, int Type);

// (cdecl) -- one step of a dB fade. False when *Remaining < 0 (no fade running).
// Otherwise *Remaining -= Dt; at or below zero *Remaining = -1 and *OutDb = ToDb; else
// *OutDb = FromDb..ToDb interpolated Dt / (old remaining) of the way in power-ratio space,
// -96 dB at or below 10^-9.6. Returns true. Caller: HUD_Receiver::Update (0x004cdf76).
bool ClientSoundFadeDb(float FromDb, float ToDb, float Dt, float* Remaining, float* OutDb);

// / the routine — refcounted spaceflight ducking of the whole mix.
void PushClientSoundDucking();
void PopClientSoundDucking();
// Current global ducking attenuation in dB (baseline `_DAT_00667ca4`).
float GetClientSoundDuckingDb();

// / e30 / e60 / e90 / ec0 — the audio_* user-option callbacks. Each takes a
// 0..1 slider and stores 10*log2(x) dB (floored at -96) in the matching category master
// (the field / c84 / c80 / c7c / c78). Category ← source type: sfx 0, interface 1,
// voice 2 and 5, music 3, ambient 4.
void SetClientAudioMusicVolume(float Linear);
void SetClientAudioAmbientVolume(float Linear);
void SetClientAudioSfxVolume(float Linear);
void SetClientAudioInterfaceVolume(float Linear);
void SetClientAudioVoiceVolume(float Linear);

// — the `sound_level_of_detail` option (the field). Changing it re-resolves
// the `lod1=` / `lod2=` link of every archetype (the routine); a lod naming NONE unlinks the
// archetype entirely, so it stops playing.
void SetClientSoundLod(int Lod);
int GetClientSoundLod();

// (ISoundSource slot 9) and the mixer score at +0x28 — exposed for tests.
GENRESULT GetClientSoundSourceVolumeDb(void* Source, float* OutDb);
float GetClientSoundSourceMixerScore(void* Source);

// Direct reads a caller makes on a source or archetype: the source's +0x18 archetype (what
// Start stored), the archetype's slot 12 (+0x30) lod link and slot 11 (+0x2c) random pitch, and
// its +0x04 nickname id. Caller: GameObject slot 109 (the routine).
void* GetClientSoundSourceArch(void* Source);
void* GetClientSoundArchLinked(void* Arch);
float GetClientSoundArchRandomPitch(void* Arch);
unsigned GetClientSoundArchId(void* Arch);

// (cdecl): create Arch's ISoundManager wav archetype. The name comes from Arch
// slot 1, the flags from slots 10/13 and the [AUDIO]/PITCH_BEND_SOUNDS options; ParentFs is
// the IFileSystem the name lives in (a voice profile's .utf), null for NeuronRuntime::Data.
// Returns the SM archetype id or -1. Callers: SoundImpl Acquire (the routine) and
// VoiceMsg::Acquire (the routine).
int CreateSoundArchWav(Archetype::ISound* Arch, void* ParentFs);
// ISoundManager slot 34 (+0x88) — used by VoiceMsg::Release (the routine).
void DestroyClientSoundWav(int SmArchId);
