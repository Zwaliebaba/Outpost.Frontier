// Author: Giperion.

#pragma once

// Archetype::ISound -- the client sound-archetype interface (OutpostFrontier.exe only).

// RTTI: ".?AUISound@Archetype@@" (type descriptor 0x006109b0). Two implementations:
//   Archetype::SoundImpl (TD 0x006109d0), vtable 0x005ca36c -- the [Sound] archetypes of
//     SoundArch.cpp, port FClientSoundArch in menu_sound_arch.cpp;
//   VoiceMsg : Archetype::IVoiceMsg : Archetype::ISound (TDs 0x00610a70 / 0x00610a4c),
//     vtable 0x005cabfc -- one line of a voice profile, port FVoiceMsg in voice_profiles.h.
// Both tables are 15 slots. The sound heap (the routine and Sound::Source Start the routine)
// only ever talks to an archetype through this table, which is how HUD_Comm can hand a VoiceMsg
// straight to the routine (0x004cb188, 0x004cc01b).

// Slot names come from what both implementations return; the calling convention is the plain
// member-function one (baseline ECX = this, callee pops the stack arguments -- there are none).

#include <list>
#include <string>

namespace Archetype
{
    struct ISound
    {
        // 0  SoundImpl the routine (the lod-linked archetype's SoundManager id), VoiceMsg
        // (MOV EAX,[ECX+8]).
        virtual int GetSmArchId() = 0;
        // 1  SoundImpl the branch (+8 file), VoiceMsg the branch (profile slot 1 on its id).
        virtual char const* GetFileName() = 0;
        // 2  SoundImpl the branch (type nibble of the flags), VoiceMsg the branch (2).
        virtual unsigned GetSourceType() = 0;
        // 3  SoundImpl the routine, VoiceMsg the routine -- create the wav on first use and
        //    count the reference; SETNZ of "has a SoundManager id".
        virtual unsigned char Acquire() = 0;
        // 4  SoundImpl the routine, VoiceMsg the routine -- both return the remaining count
        //    sign-extended from its word (MOVSX).
        virtual int Release() = 0;
        // 5  SoundImpl the routine, VoiceMsg the branch -- the reference count.
        virtual int GetUseCount() = 0;
        // 6  SoundImpl the branch, VoiceMsg the branch -- attenuation in dB (signed byte).
        virtual int GetAttenuation() = 0;
        // 7  SoundImpl the routine, VoiceMsg the branch (-1.0f).
        virtual float GetRangeMin() = 0;
        // 8  SoundImpl the branch, VoiceMsg the branch (-1.0f).
        virtual float GetRangeMax() = 0;
        // 9  SoundImpl the branch (flag bit 26), VoiceMsg the routine (false).
        virtual unsigned char IsStreamer() = 0;
        // 10 SoundImpl the branch (flag bit 27), VoiceMsg the routine (true).
        virtual unsigned char Is2d() = 0;
        // 11 SoundImpl the branch (random pitch), VoiceMsg the branch (0.0f).
        virtual float GetRandomPitch() = 0;
        // 12 SoundImpl the routine (resolve the lod link), VoiceMsg the routine (this).
        virtual ISound* GetLinked() = 0;
        // 13 SoundImpl the branch (flag bit 24), VoiceMsg the routine (false).
        virtual unsigned char IsPitchBendable() = 0;
        // 14 SoundImpl the branch (0), VoiceMsg the branch (MOVSX [+0x10], `priority =`).
        virtual int GetPriority() = 0;
    };

    // RTTI ".?AUIVoiceMsg@Archetype@@": no slots of its own beyond ISound's 15.
    struct IVoiceMsg : ISound
    {
    };

    // RTTI ".?AUIVoice@Archetype@@": the voice-profile interface, vtable 0x005cad10 -- eight
    // slots, every one _purecall (the branch), stored into VoiceProfile objects at 0x00430ba0 /
    // / 0x00433bfa (the inlined base-class vptr stores). Its one implementation is
    // VoiceProfile (vtable 0x005cacec, port FVoiceProfile in voice_profiles.h). There is no
    // virtual destructor slot. Plain member-function convention (ECX = this, callee pops).
    struct IVoice
    {
        // 0  the branch -- `file =` of one message (ignored once the profile has a .utf).
        virtual void SetSoundFile(unsigned int MsgId, char const* File) = 0;
        // 1  the branch -- the message's sound file ("0x%X" of the id inside a .utf profile).
        virtual char const* GetSoundFile(unsigned int MsgId) = 0;
        // 2  the branch -- the profile's .utf as an IFileSystem, null without one.
        virtual void* OpenScriptFileSystem() = 0;
        // 3  the branch -- the profile id.
        virtual unsigned int GetId() const = 0;
        // 4  the branch -- ID_String::get_string of the id.
        virtual char const* GetName() const = 0;
        // 5  the branch -- the message by id, null when absent.
        virtual IVoiceMsg* FindMessage(unsigned int MsgId) = 0;
        // 6  the branch -- the `script =` list.
        virtual std::list<std::string>& GetScripts() = 0;
        // 7  the branch -- MOV EAX,"" (the field); RET 4: one stack argument, never read.
        virtual char const* GetUnusedString(void* Unused) const = 0;
    };
}
