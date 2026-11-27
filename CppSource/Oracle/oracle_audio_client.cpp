// P4.10, the client wiring: the client's sound archetypes, play keys and per-frame sound tick
// (Client\cmn\SoundArch.cpp and Sound.cpp, ported as menu_sound_arch.cpp) and its audio
// configuration (sound_config.cpp), through the reference's own code. Both files are compiled
// into this one; the manager they drive is a SoundManager.dll FSoundManager with the recording
// device of oracle_audio.cpp (OracleAudioCreateManager and the other bridge functions below),
// so the 3D archetypes take the software path there as in audio_tick.jsonl. The Rust side is
// crates/neuron-client/tests/oracle_vectors.rs with crates/neuron-client/tests/support/audio.rs;
// the inputs come from crates/neuron-client/tests/fixtures/oracle/make_audio_fixtures.py.
//
// The client functions the compiled files reach otherwise are stand-ins: the perf options
// sound_max_dist_scale (slot 2), USE_3D_SOUNDS (slot 4) and max_submitted_sounds take the
// case's values, the thruster sound's player and camera queries answer 0. GetGameOption is
// oracle_ui_controls.cpp's stand-in, which answers 1 for "[AUDIO]" and "PITCH_BEND_SOUNDS".
// NeuronRuntime::Data is a file system that makes each wav from its name, c<channels>b<bits>
// r<rate>f<frames>.wav, a PCM RIFF file of that many zero frames; any other name is not found.
// No case gives an archetype a random pitch, which would call the C runtime's rand.
//
// audio/arch.list and audio/client.list hold one command per line; "case" starts a session
// with a fresh manager and an empty archetype table:
//   case USE3D SCALE MAXSUB MAXCH   perf slots 4 and 2 (f32 bits), max_submitted_sounds, the
//                                   manager's channel count
//   ini FORCE FILE                  LoadClientSoundArchIni(FILE, FORCE), FILE under the fixtures
//   acquire NICK | release NICK     archetype slot 3 / 4 of the registered [Sound] (nickname in
//                                   hex; an unknown one is a null archetype)
//   lod N | preload M | unload M    SetClientSoundLod, Preload/UnloadClientSoundArchesByCategory
//   play NICK FLAGS START X,Y,Z ATTEN HANDLE
//                                   PlayClientSoundArch with the play key {START, FLAGS, X, Y, Z,
//                                   ATTEN} (FLAGS "-": no key) and an out handle when HANDLE is 1
//   src S OP [V]                    on the source the S-th successful play made: atten V (slot
//                                   32), pan V (33), pitch V (38), pos X,Y,Z (35), vel X,Y,Z (36),
//                                   loop B (34), type T, offset V (25), stop (24), release
//   vol CAT V | mode M | duck push | duck pop
//                                   the audio_* masters (0 sfx .. 4 voice), SetClientAudioMode,
//                                   Push/PopClientSoundDucking
//   listener FRONT UP POS           ISoundManager SetOrientation and SetListenerPosition
//   tick NOW DT                     the manager's clock at NOW, then TickClientSoundMixer(0, DT)
//
// Line formats (floats as eight hex digits of the f32):
//   audio_arch.jsonl   {"case","op","nick","r"} per command, then per registered archetype in
//                      id order {"case","arch","file","sm","min","max","flags","lod","linked",
//                      "uses","unlinked","sm_flags","sm_buffer","sm_frames","sm_duration"}
//   audio_client.jsonl {"case","op","r","src"} per play, {"case","tick","duck","ranked"} per tick,
//                      then per playing source from the front {"case","tick","src","flags",
//                      "start","score","atten","pitch","finished"} and per manager instance
//                      {"case","tick","voice","rank","pan","volume","freq","pos","plays"}
// A source's flag byte is printed without its top three bits, which the pools leave as the block
// held them (divergence D527); "src" is "?" for a source no play made.
//
// The voice profiles (Client\cmn\Voice.cpp, ported as voice_profiles.cpp) are compiled in too:
// audio/voice.list runs the same commands, and
//   voices FILE                     LoadClientVoicesIni(FILE), FILE under the fixtures
// A NICK of the form PROFILE/MSG (both in hex) names the line MSG of the profile PROFILE, a
// VoiceMsg, through LookupClientVoice and FindMessage (null when either is unknown). In that
// session NeuronRuntime::Data's root is the fixture directory, so ResolveScriptFile finds
// audio\<nickname>.utf there; opening that path ("UTF") gives an archive whose text lines map a
// leaf name to a wav name of the form above ("0x2A c1b16r22050f100.wav"); its other leaves are
// not found. Lines of audio_voice.jsonl: the arch ones above and, after every command that
// prints the archetypes, per profile in id order {"case","voice","name","utf","scripts","files"}
// (texts in hex, "files" the `file =` table as id:name, "-" when empty), then per line in id
// order {"case","voice","msg","sm","prio","atten","refs","file","sm_flags","sm_buffer",
// "sm_frames","sm_duration"} ("file" is slot 1, the line's wav name).

#include "oracle_support.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "../OutpostFrontier/Client/sound_config.cpp"
#include "../OutpostFrontier/Client/menu_sound_arch.cpp"

// voice_profiles.cpp's headers first; then CopyBoundedString (the client's startup_config.cpp;
// Server.lib has one of its own) reaches its body as a file-local copy of the client's under a
// name of its own, so this file defines none of the name.
#include <io.h>
#include "common_reputation.h"
#include "common_universe.h"
#include "../OutpostFrontier/Client/globals.h"
#include "../OutpostFrontier/Client/menu_universe_base.h"
#include "../OutpostFrontier/Client/voice_profiles.h"
#define CopyBoundedString OracleVoiceProfilesCopyBoundedString

// startup_config.cpp's body.
static unsigned OracleVoiceProfilesCopyBoundedString(char* Dest, unsigned Capacity, const char* Src)
{
    if (Src == nullptr)
        return 0;
    unsigned Length = static_cast<unsigned>(strlen(Src));
    if (Length >= Capacity)
        Length = Capacity - 1;
    memcpy(Dest, Src, Length);
    Dest[Length] = '\0';
    return Length;
}

#include "../OutpostFrontier/Client/voice_profiles.cpp"

#undef CopyBoundedString

using namespace oracle;

// The bridge to oracle_audio.cpp's FSoundManager and its recording device.
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
ISoundManager* OracleAudioCreateManager(unsigned long MaxChannels);
void OracleAudioDestroyManager(ISoundManager* Manager);
void OracleAudioSetTick(unsigned long Now);
unsigned long OracleAudioVoices(FOracleVoiceInfo* Out, unsigned long Max);
bool OracleAudioArchetypeInfo(ISoundManager* Manager, int Id, unsigned long* Flags, unsigned long* Buffer,
                              unsigned long* Frames, unsigned long* Duration);

namespace
{
    float GPerfMaxDistScale = 1.0f;
    float GPerfUse3dSounds = 1.0f;
    int GPerfMaxSubmitted = 32;
}

// The client's oracle mode (oracle_mode.cpp) is not linked in: its audio.jsonl hook does
// nothing here.
void OracleOnSoundCall(char const*, unsigned const*, unsigned, float const*, unsigned, int)
{
}

// The client's perf options (perf_options.cpp): only the slots the sound code reads.
float GetPerfOptionSlot(int Index)
{
    if (Index == 2)
        return GPerfMaxDistScale;
    if (Index == 4)
        return GPerfUse3dSounds;
    return 0.0f;
}

int GetMaxSubmittedSounds()
{
    return GPerfMaxSubmitted;
}

// The thruster sound's queries (player_spawn.cpp, space_camera.cpp); no case makes one.
unsigned GetPlayerConnectionId()
{
    return 0;
}

unsigned GetPlayerCharacterId()
{
    return 0;
}

bool IsFirstPersonCameraCurrent()
{
    return false;
}

namespace
{
    std::vector<std::string> SplitText(const std::string& Text, char Separator)
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

    float HexF(const std::string& Token)
    {
        return FloatFromBits(static_cast<unsigned int>(strtoul(Token.c_str(), nullptr, 16)));
    }

    std::string Fh(float Value)
    {
        const unsigned int Bits = Value != Value ? 0x7FC00000u : FloatBits(Value);
        return Format("%08x", Bits);
    }

    std::string Fld(const char* Key, const std::string& Value)
    {
        return Format(",\"%s\":\"", Key) + Value + "\"";
    }

    // A PCM RIFF file of zero frames, from c<channels>b<bits>r<rate>f<frames>.wav.
    bool MakeWav(const char* Path, std::vector<unsigned char>& Out)
    {
        if (Path == nullptr)
            return false;
        const char* Name = Path;
        for (const char* P = Path; *P != 0; ++P)
        {
            if (*P == '\\' || *P == '/')
                Name = P + 1;
        }
        unsigned int Channels = 0;
        unsigned int Bits = 0;
        unsigned int Rate = 0;
        unsigned int Frames = 0;
        char Tail[8] = {0};
        if (sscanf(Name, "c%ub%ur%uf%u.%3s", &Channels, &Bits, &Rate, &Frames, Tail) != 5)
            return false;
        const unsigned int Align = Channels * (Bits / 8);
        const unsigned int DataBytes = Frames * Align;
        Out.clear();
        auto Put32 = [&Out](unsigned int V) {
            for (int I = 0; I < 4; ++I)
                Out.push_back(static_cast<unsigned char>(V >> (8 * I)));
        };
        auto Put16 = [&Out](unsigned int V) {
            Out.push_back(static_cast<unsigned char>(V));
            Out.push_back(static_cast<unsigned char>(V >> 8));
        };
        Put32(0x46464952u);
        Put32(36 + DataBytes);
        Put32(0x45564157u);
        Put32(0x20746D66u);
        Put32(16);
        Put16(1);
        Put16(Channels);
        Put32(Rate);
        Put32(Rate * Align);
        Put16(Align);
        Put16(Bits);
        Put32(0x61746164u);
        Put32(DataBytes);
        Out.insert(Out.end(), DataBytes, 0);
        return true;
    }

    // One wav as a file system: the mapping WavLibLoad reads.
    class FWavFs final : public IFileSystem
    {
    public:
        std::vector<unsigned char> Data;
        unsigned long Refs = 1;

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return ++Refs; }
        unsigned long NEURONCOMPONENTS_API Release() override
        {
            if (--Refs == 0)
            {
                delete this;
                return 0;
            }
            return Refs;
        }
        GENRESULT NEURONCOMPONENTS_API CreateInstance(AGGDESC*, void** Out) override
        {
            *Out = nullptr;
            return GR_GENERIC;
        }
        int NEURONCOMPONENTS_API OpenNode(const char*) override { return 0; }
        int NEURONCOMPONENTS_API OpenByDesc(FFileSystemDesc*) override { return -1; }
        int NEURONCOMPONENTS_API CloseFile(unsigned long) override { return 1; }
        unsigned long NEURONCOMPONENTS_API GetSize(unsigned long, unsigned long*) override
        {
            return static_cast<unsigned long>(Data.size());
        }
        int NEURONCOMPONENTS_API Read(unsigned long, void*, unsigned long, unsigned long* Got, unsigned long) override
        {
            *Got = 0;
            return 0;
        }
        int NEURONCOMPONENTS_API Write(unsigned long, const void*, unsigned long, unsigned long* Written,
                                       const void*) override
        {
            *Written = 0;
            return 0;
        }
        int NEURONCOMPONENTS_API CreateMapping(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long,
                                               unsigned long) override
        {
            return 1;
        }
        int NEURONCOMPONENTS_API MapFileView(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long) override
        {
            return static_cast<int>(reinterpret_cast<intptr_t>(Data.data()));
        }
        int NEURONCOMPONENTS_API UnmapFileView(unsigned long) override { return 1; }
        int NEURONCOMPONENTS_API FindFirst(const char*, WIN32_FIND_DATAA*) override { return -1; }
        int NEURONCOMPONENTS_API FindNext(unsigned long, WIN32_FIND_DATAA*) override { return 0; }
        int NEURONCOMPONENTS_API FindClose(unsigned long) override { return 1; }
        int NEURONCOMPONENTS_API GetCurrentPath(unsigned long, char* Out) override
        {
            Out[0] = 0;
            return 1;
        }
        int NEURONCOMPONENTS_API ResolvePath(char* Dest, const char*, unsigned long) override
        {
            Dest[0] = 0;
            return 1;
        }
        int NEURONCOMPONENTS_API RejectOperation1C(unsigned long, unsigned long, unsigned long, unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API SeekFile(unsigned long, unsigned long, unsigned long*, unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API SetEndOfFile(unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API ReturnOne2C(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API ReturnOne30(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetFileTimes(unsigned long, FILETIME*, FILETIME*, FILETIME*) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation38(unsigned long, unsigned long, unsigned long, unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation54(const char*, unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation58(unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation64(unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation68(unsigned long, unsigned long, unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation6C(unsigned long, unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetPathAttributes(const char*) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation74(unsigned long, unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetFileSystemError() override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetFilePosition(unsigned long, unsigned long*) override { return GR_GENERIC; }
        void NEURONCOMPONENTS_API GetFileSysPath(char* Out, unsigned long MaxChars) override
        {
            if (Out != nullptr && MaxChars != 0)
                Out[0] = 0;
        }
        int NEURONCOMPONENTS_API GetAccessFlags() override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetParentFileSystem(IFileSystem**) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API FailOperation90(unsigned long, unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API FailOperation94(unsigned long, unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API FailOperation98(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API FailOperation9C(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API InvokeLockedCallback(unsigned long, unsigned long, unsigned long) override { return GR_GENERIC; }
    };

    // NeuronRuntime::Data: CreateInstance makes the wav its description names. In the voice
    // session the root is the fixture directory and a "UTF" path ending in .utf opens an archive
    // (a FWavData of its own, made on the heap) whose leaves are the file's lines.
    class FWavData final : public IFileSystem
    {
    public:
        std::string Root;
        bool bArchive = false;
        unsigned long Refs = 1;
        std::map<std::string, std::string> Leaves;

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return bArchive ? ++Refs : 1; }
        unsigned long NEURONCOMPONENTS_API Release() override
        {
            if (!bArchive)
                return 1;
            if (--Refs == 0)
            {
                delete this;
                return 0;
            }
            return Refs;
        }
        // The archive audio\<nickname>.utf of the fixtures: "leaf wav" per line.
        static FWavData* OpenArchive(const char* Path)
        {
            if (Path == nullptr)
                return nullptr;
            const size_t Length = strlen(Path);
            if (Length < 4 || _stricmp(Path + Length - 4, ".utf") != 0)
                return nullptr;
            std::string Relative = Path;
            for (char& Ch : Relative)
            {
                if (Ch == '\\')
                    Ch = '/';
            }
            Bytes Text;
            if (!ReadWholeFile(GFixtureDir + "/" + Relative, Text))
                return nullptr;
            FWavData* Archive = new FWavData();
            Archive->bArchive = true;
            std::string Line;
            Text.push_back('\n');
            for (unsigned char Byte : Text)
            {
                if (Byte == '\n')
                {
                    const size_t Space = Line.find(' ');
                    if (Space != std::string::npos)
                        Archive->Leaves[Line.substr(0, Space)] = Line.substr(Space + 1);
                    Line.clear();
                }
                else if (Byte != '\r')
                    Line += static_cast<char>(Byte);
            }
            return Archive;
        }
        GENRESULT NEURONCOMPONENTS_API CreateInstance(AGGDESC* Arg, void** Out) override
        {
            const FFileSystemAggDesc* Desc = reinterpret_cast<const FFileSystemAggDesc*>(Arg);
            const char* Name = Desc->DataPath;
            if (bArchive)
            {
                const std::map<std::string, std::string>::const_iterator Leaf =
                    Name != nullptr ? Leaves.find(Name) : Leaves.end();
                Name = Leaf != Leaves.end() ? Leaf->second.c_str() : nullptr;
            }
            else if (!Root.empty() && Desc->BackendName != nullptr && strcmp(Desc->BackendName, "UTF") == 0)
            {
                FWavData* const Archive = OpenArchive(Name);
                *Out = Archive != nullptr ? static_cast<IFileSystem*>(Archive) : nullptr;
                return Archive != nullptr ? GR_OK : GR_GENERIC;
            }
            FWavFs* Fs = new FWavFs();
            if (!MakeWav(Name, Fs->Data))
            {
                Fs->Release();
                *Out = nullptr;
                return GR_GENERIC;
            }
            *Out = static_cast<IFileSystem*>(Fs);
            return GR_OK;
        }
        int NEURONCOMPONENTS_API OpenNode(const char*) override { return 0; }
        int NEURONCOMPONENTS_API OpenByDesc(FFileSystemDesc*) override { return -1; }
        int NEURONCOMPONENTS_API CloseFile(unsigned long) override { return 1; }
        unsigned long NEURONCOMPONENTS_API GetSize(unsigned long, unsigned long*) override { return 0; }
        int NEURONCOMPONENTS_API Read(unsigned long, void*, unsigned long, unsigned long* Got, unsigned long) override
        {
            *Got = 0;
            return 0;
        }
        int NEURONCOMPONENTS_API Write(unsigned long, const void*, unsigned long, unsigned long* Written,
                                       const void*) override
        {
            *Written = 0;
            return 0;
        }
        int NEURONCOMPONENTS_API CreateMapping(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long,
                                               unsigned long) override
        {
            return -1;
        }
        int NEURONCOMPONENTS_API MapFileView(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long) override
        {
            return 0;
        }
        int NEURONCOMPONENTS_API UnmapFileView(unsigned long) override { return 1; }
        int NEURONCOMPONENTS_API FindFirst(const char*, WIN32_FIND_DATAA*) override { return -1; }
        int NEURONCOMPONENTS_API FindNext(unsigned long, WIN32_FIND_DATAA*) override { return 0; }
        int NEURONCOMPONENTS_API FindClose(unsigned long) override { return 1; }
        int NEURONCOMPONENTS_API GetCurrentPath(unsigned long, char* Out) override
        {
            Out[0] = 0;
            return 1;
        }
        int NEURONCOMPONENTS_API ResolvePath(char* Dest, const char*, unsigned long) override
        {
            Dest[0] = 0;
            return 1;
        }
        int NEURONCOMPONENTS_API RejectOperation1C(unsigned long, unsigned long, unsigned long, unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API SeekFile(unsigned long, unsigned long, unsigned long*, unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API SetEndOfFile(unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API ReturnOne2C(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API ReturnOne30(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetFileTimes(unsigned long, FILETIME*, FILETIME*, FILETIME*) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation38(unsigned long, unsigned long, unsigned long, unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation54(const char*, unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation58(unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation64(unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation68(unsigned long, unsigned long, unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation6C(unsigned long, unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetPathAttributes(const char*) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API RejectOperation74(unsigned long, unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetFileSystemError() override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetFilePosition(unsigned long, unsigned long*) override { return GR_GENERIC; }
        void NEURONCOMPONENTS_API GetFileSysPath(char* Out, unsigned long MaxChars) override
        {
            if (Out == nullptr || MaxChars == 0)
                return;
            const size_t Length = Root.size() < MaxChars - 1 ? Root.size() : MaxChars - 1;
            memcpy(Out, Root.c_str(), Length);
            Out[Length] = 0;
        }
        int NEURONCOMPONENTS_API GetAccessFlags() override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API GetParentFileSystem(IFileSystem**) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API FailOperation90(unsigned long, unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API FailOperation94(unsigned long, unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API FailOperation98(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API FailOperation9C(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long) override { return GR_GENERIC; }
        int NEURONCOMPONENTS_API InvokeLockedCallback(unsigned long, unsigned long, unsigned long) override { return GR_GENERIC; }
    };

    std::string IdText(unsigned Id)
    {
        return Format("%08x", Id);
    }

    // One registered archetype, with the manager's archetype when it has one.
    std::string ArchLine(int Case, FClientSoundArch* Arch)
    {
        std::string Linked = "-";
        if (Arch->Linked != nullptr)
            Linked = IdText(Arch->Linked->NicknameId);
        std::string Line = Format("{\"case\":\"%d\",\"arch\":\"%08x\"", Case, Arch->NicknameId) +
                           Fld("file", Arch->File != nullptr ? HexC(Arch->File) : std::string("-")) +
                           Fld("sm", Format("%d", Arch->SmArchId)) + Fld("min", Fh(Arch->RangeMin)) +
                           Fld("max", Fh(Arch->RangeMax)) + Fld("flags", Format("%08x", Arch->Flags)) +
                           Fld("lod", IdText(Arch->Lod[0]) + "," + IdText(Arch->Lod[1])) + Fld("linked", Linked) +
                           Fld("uses", Format("%u", static_cast<unsigned>(Arch->UseCount))) +
                           Fld("unlinked", Format("%u", static_cast<unsigned>(Arch->Pad28)));
        unsigned long Flags = 0;
        unsigned long Buffer = 0;
        unsigned long Frames = 0;
        unsigned long Duration = 0;
        if (Arch->SmArchId != -1 &&
            OracleAudioArchetypeInfo(NeuronRuntime::Sound, Arch->SmArchId, &Flags, &Buffer, &Frames, &Duration))
        {
            Line += Fld("sm_flags", Format("%lx", Flags)) + Fld("sm_buffer", Format("%lx", Buffer)) +
                    Fld("sm_frames", Format("%lu", Frames)) + Fld("sm_duration", Format("%lu", Duration));
        }
        else
        {
            Line += Fld("sm_flags", "-") + Fld("sm_buffer", "-") + Fld("sm_frames", "-") + Fld("sm_duration", "-");
        }
        return Line + "}";
    }

    // The manager's archetype of a SoundManager id, or four dashes.
    std::string SmFields(int SmArchId)
    {
        unsigned long Flags = 0;
        unsigned long Buffer = 0;
        unsigned long Frames = 0;
        unsigned long Duration = 0;
        if (SmArchId != -1 &&
            OracleAudioArchetypeInfo(NeuronRuntime::Sound, SmArchId, &Flags, &Buffer, &Frames, &Duration))
        {
            return Fld("sm_flags", Format("%lx", Flags)) + Fld("sm_buffer", Format("%lx", Buffer)) +
                   Fld("sm_frames", Format("%lu", Frames)) + Fld("sm_duration", Format("%lu", Duration));
        }
        return Fld("sm_flags", "-") + Fld("sm_buffer", "-") + Fld("sm_frames", "-") + Fld("sm_duration", "-");
    }

    // The voice profiles in id order, each followed by its lines.
    void DumpVoices(Suite& Out, int Case)
    {
        for (std::map<unsigned int, FVoiceProfile>::iterator It = GVoiceProfiles.begin(); It != GVoiceProfiles.end();
             ++It)
        {
            FVoiceProfile& Profile = It->second;
            std::string Scripts;
            for (std::list<std::string>::const_iterator S = Profile.Scripts.begin(); S != Profile.Scripts.end(); ++S)
                Scripts += (Scripts.empty() ? "" : ",") + HexC(S->c_str());
            std::string Files;
            for (std::map<unsigned int, std::string>::const_iterator F = Profile.SoundFiles.begin();
                 F != Profile.SoundFiles.end(); ++F)
                Files += (Files.empty() ? "" : ",") + IdText(F->first) + ":" + HexC(F->second.c_str());
            Out.Line(Format("{\"case\":\"%d\",\"voice\":\"%08x\"", Case, Profile.Id.Id) +
                     Fld("name", HexC(Profile.Name)) +
                     Fld("utf", Profile.UtfPath.empty() ? std::string("-") : HexC(Profile.UtfPath.c_str())) +
                     Fld("scripts", Scripts.empty() ? std::string("-") : Scripts) +
                     Fld("files", Files.empty() ? std::string("-") : Files) + "}");
            for (std::map<unsigned int, FVoiceMsg>::iterator M = Profile.Messages.begin(); M != Profile.Messages.end();
                 ++M)
            {
                FVoiceMsg& Msg = M->second;
                Out.Line(Format("{\"case\":\"%d\",\"voice\":\"%08x\",\"msg\":\"%08x\"", Case, Profile.Id.Id,
                                Msg.Id.Id) +
                         Fld("sm", Format("%d", Msg.SmArchId)) + Fld("prio", Format("%d", Msg.Priority)) +
                         Fld("atten", Format("%d", Msg.Attenuation)) + Fld("refs", Format("%d", Msg.RefCount)) +
                         Fld("file", HexC(Msg.GetFileName())) + SmFields(Msg.SmArchId) + "}");
            }
        }
    }

    void DumpArchs(Suite& Out, int Case)
    {
        for (std::map<unsigned, FClientSoundArch*>::iterator It = G_SoundArchs.begin(); It != G_SoundArchs.end(); ++It)
            Out.Line(ArchLine(Case, It->second));
        DumpVoices(Out, Case);
    }

    void* FindArch(const std::string& NickHex)
    {
        const size_t Slash = NickHex.find('/');
        if (Slash != std::string::npos)
        {
            const Bytes Profile = Unhex(NickHex.substr(0, Slash));
            const Bytes Line = Unhex(NickHex.substr(Slash + 1));
            FVoiceProfile* const Voice = LookupClientVoice(CreateID(CStr(Profile)));
            FVoiceMsg* const Msg = Voice != nullptr ? Voice->FindMessage(CreateID(CStr(Line))) : nullptr;
            return Msg != nullptr ? static_cast<Archetype::ISound*>(static_cast<Archetype::IVoiceMsg*>(Msg)) : nullptr;
        }
        const Bytes Nick = Unhex(NickHex);
        ID_String Id;
        Id.Id = CreateID(CStr(Nick));
        if (Id.IsEmpty())
            return nullptr;
        std::map<unsigned, FClientSoundArch*>::iterator Found = G_SoundArchs.find(Id.Id);
        if (Found == G_SoundArchs.end())
            return nullptr;
        return static_cast<Archetype::ISound*>(Found->second);
    }

    // One session: the manager, the sources the plays made, in play order, with the handles
    // the harness holds.
    struct FClientCase
    {
        int Index = -1;
        ISoundManager* Manager = nullptr;
        std::vector<FSoundSource*> Sources;
        std::vector<ISoundSource*> Handles;

        void Finish()
        {
            if (Manager == nullptr)
                return;
            for (ISoundSource* Handle : Handles)
            {
                if (Handle != nullptr)
                    Handle->Release();
            }
            Handles.clear();
            Sources.clear();
            StopAllClientSounds();
            SetClientSoundLod(0);
            ShutdownClientSoundArch();
            ClearClientVoices();
            OracleAudioDestroyManager(Manager);
            Manager = nullptr;
            NeuronRuntime::Sound = nullptr;
        }
    };

    std::string SourceName(const FClientCase& Case, FSoundSource* Source)
    {
        for (size_t Index = Case.Sources.size(); Index > 0; --Index)
        {
            if (Case.Sources[Index - 1] == Source)
                return Format("%u", static_cast<unsigned>(Index - 1));
        }
        return "?";
    }

    void SetMaster(int Category, float Linear)
    {
        switch (Category)
        {
        case 0: SetClientAudioSfxVolume(Linear); break;
        case 1: SetClientAudioInterfaceVolume(Linear); break;
        case 2: SetClientAudioMusicVolume(Linear); break;
        case 3: SetClientAudioAmbientVolume(Linear); break;
        default: SetClientAudioVoiceVolume(Linear); break;
        }
    }

    FVec3 HexVec3(const std::string& Token)
    {
        const std::vector<std::string> Parts = SplitText(Token, ',');
        FVec3 Out;
        Out.X = HexF(Parts[0]);
        Out.Y = HexF(Parts[1]);
        Out.Z = HexF(Parts[2]);
        return Out;
    }

    void RunSession(const char* List, const char* Output, bool bVoiceRoot)
    {
        Suite Out(Output);
        FWavData Data;
        if (bVoiceRoot)
            Data.Root = GFixtureDir;
        IFileSystem* const SavedData = NeuronRuntime::Data;
        ISoundManager* const SavedSound = NeuronRuntime::Sound;
        NeuronRuntime::Data = &Data;
        FClientCase Case;
        for (const std::string& Line : InputLines(List))
        {
            const std::vector<std::string> T = SplitText(Line, ' ');
            const std::string& Command = T[0];
            if (Command == "case")
            {
                Case.Finish();
                ++Case.Index;
                GPerfUse3dSounds = HexF(T[1]);
                GPerfMaxDistScale = HexF(T[2]);
                GPerfMaxSubmitted = static_cast<int>(strtol(T[3].c_str(), nullptr, 10));
                Case.Manager = OracleAudioCreateManager(strtoul(T[4].c_str(), nullptr, 10));
                NeuronRuntime::Sound = Case.Manager;
                SetClientAudioMode(0);
                for (int Category = 0; Category < 5; ++Category)
                    SetMaster(Category, 1.0f);
            }
            else if (Command == "voices")
            {
                const int Added = LoadClientVoicesIni(T[1].c_str());
                Out.Line(Format("{\"case\":\"%d\",\"op\":\"voices\",\"nick\":\"-\",\"r\":\"%d\"}", Case.Index,
                                Added));
                DumpArchs(Out, Case.Index);
            }
            else if (Command == "ini")
            {
                const int Added = LoadClientSoundArchIni(T[2].c_str(), T[1] != "0");
                Out.Line(Format("{\"case\":\"%d\",\"op\":\"ini\",\"nick\":\"-\",\"r\":\"%d\"}", Case.Index, Added));
                DumpArchs(Out, Case.Index);
            }
            else if (Command == "acquire" || Command == "release")
            {
                void* const Arch = FindArch(T[1]);
                const int R = Command == "acquire" ? AcquireClientSoundArch(Arch)
                                                   : static_cast<int>(ReleaseClientSoundArch(Arch));
                Out.Line(Format("{\"case\":\"%d\",\"op\":\"%s\",\"nick\":\"%s\",\"r\":\"%d\"}", Case.Index,
                                Command.c_str(), T[1].c_str(), R));
                DumpArchs(Out, Case.Index);
            }
            else if (Command == "lod" || Command == "preload" || Command == "unload")
            {
                const unsigned long Value = strtoul(T[1].c_str(), nullptr, 10);
                if (Command == "lod")
                    SetClientSoundLod(static_cast<int>(Value));
                else if (Command == "preload")
                    PreloadClientSoundArchesByCategory(Value);
                else
                    UnloadClientSoundArchesByCategory(Value);
                Out.Line(Format("{\"case\":\"%d\",\"op\":\"%s\",\"nick\":\"-\",\"r\":\"%lu\"}", Case.Index,
                                Command.c_str(), Value));
                DumpArchs(Out, Case.Index);
            }
            else if (Command == "play")
            {
                void* const Arch = FindArch(T[1]);
                unsigned Key[6] = {0, 0, 0, 0, 0, 0};
                const bool bKey = T[2] != "-";
                if (bKey)
                {
                    Key[0] = static_cast<unsigned>(strtoul(T[3].c_str(), nullptr, 10));
                    Key[1] = static_cast<unsigned>(strtoul(T[2].c_str(), nullptr, 16));
                    const FVec3 Pos = HexVec3(T[4]);
                    memcpy(&Key[2], &Pos.X, 4);
                    memcpy(&Key[3], &Pos.Y, 4);
                    memcpy(&Key[4], &Pos.Z, 4);
                    const float Atten = HexF(T[5]);
                    memcpy(&Key[5], &Atten, 4);
                }
                void* Handle = nullptr;
                const bool bHandle = T[6] != "0";
                FPlayingSourceNode* const Last = G_PlayingSources.Head->Prev;
                const unsigned char R = PlayClientSoundArch(Arch, bKey ? Key : nullptr, bHandle ? &Handle : nullptr, 0);
                std::string Name = "-";
                if (R != 0 && G_PlayingSources.Head->Prev != Last)
                {
                    Name = Format("%u", static_cast<unsigned>(Case.Sources.size()));
                    Case.Sources.push_back(G_PlayingSources.Head->Prev->Value);
                    Case.Handles.push_back(static_cast<ISoundSource*>(Handle));
                }
                else if (Handle != nullptr)
                {
                    static_cast<ISoundSource*>(Handle)->Release();
                }
                Out.Line(Format("{\"case\":\"%d\",\"op\":\"play\",\"r\":\"%u\",\"src\":\"%s\"}", Case.Index,
                                static_cast<unsigned>(R), Name.c_str()));
            }
            else if (Command == "src")
            {
                const size_t Index = strtoul(T[1].c_str(), nullptr, 10);
                ISoundSource* const Handle = Case.Handles[Index];
                const std::string& Op = T[2];
                if (Handle == nullptr)
                {
                    fprintf(stderr, "%s: source %u has no handle\n", List, static_cast<unsigned>(Index));
                    GFailed = true;
                }
                else if (Op == "atten")
                    Handle->SetAttenuation(HexF(T[3]));
                else if (Op == "pan")
                    Handle->SetPan(HexF(T[3]));
                else if (Op == "pitch")
                    Handle->SetPitch(HexF(T[3]));
                else if (Op == "pos")
                {
                    FVec3 Pos = HexVec3(T[3]);
                    Handle->Slot35(&Pos);
                }
                else if (Op == "vel")
                {
                    const FVec3 Vel = HexVec3(T[3]);
                    const float Words[3] = {Vel.X, Vel.Y, Vel.Z};
                    Handle->SetVelocity(Words);
                }
                else if (Op == "loop")
                    Handle->SetLooping(static_cast<unsigned char>(strtoul(T[3].c_str(), nullptr, 10)));
                else if (Op == "type")
                    SetClientSoundSourceType(Handle, static_cast<int>(strtol(T[3].c_str(), nullptr, 10)));
                else if (Op == "offset")
                    Handle->SetStartOffset(static_cast<int>(strtol(T[3].c_str(), nullptr, 10)));
                else if (Op == "stop")
                    Handle->Stop();
                else if (Op == "release")
                {
                    Handle->Release();
                    Case.Handles[Index] = nullptr;
                }
                else
                {
                    fprintf(stderr, "%s: unknown source operation %s\n", List, Op.c_str());
                    GFailed = true;
                }
            }
            else if (Command == "vol")
                SetMaster(static_cast<int>(strtol(T[1].c_str(), nullptr, 10)), HexF(T[2]));
            else if (Command == "mode")
                SetClientAudioMode(static_cast<int>(strtol(T[1].c_str(), nullptr, 10)));
            else if (Command == "duck")
            {
                if (T[1] == "push")
                    PushClientSoundDucking();
                else
                    PopClientSoundDucking();
            }
            else if (Command == "listener")
            {
                FVec3 Front = HexVec3(T[1]);
                FVec3 Up = HexVec3(T[2]);
                FVec3 Pos = HexVec3(T[3]);
                Case.Manager->SetOrientation(&Front, &Up);
                Case.Manager->SetListenerPosition(&Pos);
            }
            else if (Command == "tick")
            {
                const unsigned long Now = strtoul(T[1].c_str(), nullptr, 10);
                OracleAudioSetTick(Now);
                TickClientSoundMixer(nullptr, HexF(T[2]));
                std::string Ranked;
                for (unsigned Index = 0; Index < G_MixerRankedCount; ++Index)
                {
                    if (Index != 0)
                        Ranked += ",";
                    Ranked += SourceName(Case, G_MixerRanked[Index]);
                }
                Out.Line(Format("{\"case\":\"%d\",\"tick\":\"%lu\"", Case.Index, Now) +
                         Fld("duck", Fh(GetClientSoundDuckingDb())) + Fld("ranked", Ranked.empty() ? "-" : Ranked) +
                         "}");
                for (FPlayingSourceNode* Node = G_PlayingSources.Head->Next; Node != G_PlayingSources.Head;
                     Node = Node->Next)
                {
                    FSoundSource* const Source = Node->Value;
                    float Atten = 0.0f;
                    Source->GetAttenuation(&Atten);
                    float Pitch = 0.0f;
                    const bool bPitch = Source->GetPitch(&Pitch) == GR_OK;
                    Out.Line(Format("{\"case\":\"%d\",\"tick\":\"%lu\"", Case.Index, Now) +
                             Fld("src", SourceName(Case, Source)) +
                             Fld("flags", Format("%02x", static_cast<unsigned>(Source->Flags & 0x1Fu))) +
                             Fld("start", Format("%d", Source->StartTick)) + Fld("score", Fh(Source->MixerScale)) +
                             Fld("atten", Fh(Atten)) + Fld("pitch", bPitch ? Fh(Pitch) : std::string("-")) +
                             Fld("finished", Format("%u", static_cast<unsigned>(Source->IsFinished()))) + "}");
                }
                FOracleVoiceInfo Voices[64];
                const unsigned long Count = OracleAudioVoices(Voices, 64);
                for (unsigned long Index = 0; Index < Count; ++Index)
                {
                    const FOracleVoiceInfo& V = Voices[Index];
                    FSoundSource* const Source = static_cast<FSoundSource*>(static_cast<ISoundSource*>(V.Key));
                    Out.Line(Format("{\"case\":\"%d\",\"tick\":\"%lu\"", Case.Index, Now) +
                             Fld("voice", SourceName(Case, Source)) + Fld("rank", Format("%lu", V.Rank)) +
                             Fld("pan", V.bPan ? Format("%ld", V.Pan) : std::string("-")) +
                             Fld("volume", V.bVolume ? Format("%ld", V.Volume) : std::string("-")) +
                             Fld("freq", V.bFreq ? Format("%lu", V.Freq) : std::string("-")) +
                             Fld("pos", V.bPos ? Format("%lu", V.Pos) : std::string("-")) +
                             Fld("plays", Format("%u", V.Plays)) + "}");
                }
            }
            else
            {
                fprintf(stderr, "%s: unknown command %s\n", List, Command.c_str());
                GFailed = true;
            }
        }
        Case.Finish();
        NeuronRuntime::Data = SavedData;
        NeuronRuntime::Sound = SavedSound;
    }
}

void RunAudioClientSuites()
{
    RunSession("audio/arch.list", "audio_arch.jsonl", false);
    RunSession("audio/client.list", "audio_client.jsonl", false);
    RunSession("audio/voice.list", "audio_voice.jsonl", true);
}
