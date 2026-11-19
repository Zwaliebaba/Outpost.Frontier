// Author: Giperion.

#include "voice_profiles.h"

#include "common_ini.h"
#include "common_misc.h" // GetFileSysPath, File
#include "neuroncomponents.h"
#include "globals.h"
#include "readfile.h"
#include "NeuronRuntime.h"
#include "common_reputation.h"
#include "common_universe.h"
#include "menu_sound_arch.h"
#include "menu_universe_base.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>
#include <windows.h>

namespace
{
    // FDUMP call sites in the routine (s_Client\cmn\Voice.cpp @ 005cacac).
    char const* const kVoiceSource = "Client\\cmn\\Voice.cpp";

    // baseline registry: VC6 std::map at the field (head the field, size the field).
    std::map<unsigned int, FVoiceProfile> GVoiceProfiles;

    // formats the message id into the static buffer the field with "0x%X"
    // (the field) and returns it; the caller must copy before the next call.
    char GVoiceFileScratch[32];

    void ReleaseFileSystem(void* Component)
    {
        if (Component == nullptr)
            return;
        static_cast<IFileSystem*>(Component)->Release();
    }
}

FVoiceMsg::FVoiceMsg()
    : SmArchId(-1), Owner(nullptr), Priority(0), Attenuation(0), RefCount(0)
{
    // ID_String::clear, +0x10/+0x11/+0x12 zeroed, +0xC null, +8 = -1.
    Id.clear();
}

char const* FVoiceMsg::GetFileName()
{
    // MOV EAX,[ECX+4]; MOV ECX,[ECX+0xC]; CALL [vtable+4]. No null test in
    // baseline: every message is built by the [Sound] reader with Owner already set.
    return Owner->GetSoundFile(Id.Id);
}

unsigned char FVoiceMsg::Acquire()
{
    // .
    if (SmArchId == -1)
    {
        // Owner slot 2 (+8) opens the profile .utf as an IFileSystem;
        // (this, fs) reads the wav name through this->slot 1 and the flags through
        // slots 10/13; the file system is released afterwards when there was one.
        void* ScriptFs = Owner->OpenScriptFileSystem();
        SmArchId = CreateSoundArchWav(this, ScriptFs);
        if (ScriptFs != nullptr)
            ReleaseFileSystem(ScriptFs);
    }
    ++RefCount;
    return SmArchId != -1 ? 1 : 0;
}

int FVoiceMsg::Release()
{
    // a non-zero count drops by one; at <= 0 (CMP word,0 + JG skips) with a
    // created archetype, ISoundManager slot 34 destroys it and SmArchId goes back to -1.
    if (RefCount != 0)
        --RefCount;
    if (RefCount < 1 && SmArchId != -1)
    {
        DestroyClientSoundWav(SmArchId);
        SmArchId = -1;
    }
    return RefCount;
}

FVoiceProfile::FVoiceProfile() : Name(nullptr)
{
    Id.clear();
}

void FVoiceProfile::SetSoundFile(unsigned int MsgId, char const* File)
{
    // MOV EAX,[ECX+0x34]; a resolved .utf wins over per-message `file =`.
    if (!UtfPath.empty())
        return;
    // baseline assigns straight from the INI pointer (repne scasb), with no null test.
    SoundFiles[MsgId] = File;
}

char const* FVoiceProfile::GetSoundFile(unsigned int MsgId)
{
    // .
    if (!UtfPath.empty())
    {
        FVoiceMsg const* Msg = FindMessage(MsgId);
        if (Msg == nullptr)
            return "";
        // sprintf(the field, "0x%X", msg+4), unbounded as baseline
        // (at most "0xFFFFFFFF" -- 11 bytes of the 32).
        sprintf(GVoiceFileScratch, "0x%X", Msg->Id.Id);
        return GVoiceFileScratch;
    }

    std::map<unsigned int, std::string>::const_iterator It = SoundFiles.find(MsgId);
    if (It == SoundFiles.end())
        return "";
    return It->second.c_str();
}

void* FVoiceProfile::OpenScriptFileSystem()
{
    // — same AGGDESC as NeuronRuntime::OpenData, backend "UTF" (the field). The only
    // test is the string's length (+0x34, the VC6 _Len of the std::string at +0x2c);
    // NeuronRuntime::Data is called untested (0x004315c3).
    if (UtfPath.empty())
        return nullptr;

    FFileSystemAggDesc Desc;
    memset(&Desc, 0, sizeof(Desc));
    Desc.Size = 0x34;
    Desc.ComponentName = "FileSystem";
    Desc.BackendName = "UTF";
    Desc.DataPath = UtfPath.c_str();
    Desc.Field10 = 0x80000000u;
    Desc.Field14 = 1;
    Desc.Field1C = 3;
    Desc.Field20 = 0x8000080u;
    Desc.Field30 = 0xffffffffu;

    void* Out = nullptr;
    // +0x0C IComponentFactory::CreateInstance -- the FileSystem desc stands in for AGGDESC.
    if (static_cast<IFileSystem*>(NeuronRuntime::Data)
            ->CreateInstance(reinterpret_cast<AGGDESC*>(&Desc), &Out) != 0)
        return nullptr;
    return Out;
}

FVoiceMsg* FVoiceProfile::FindMessage(unsigned int MsgId)
{
    // — lower_bound then the "end / key > wanted" pair of checks.
    std::map<unsigned int, FVoiceMsg>::iterator It = Messages.find(MsgId);
    if (It == Messages.end())
        return nullptr;
    return &It->second;
}

void FVoiceProfile::ResolveScriptFile()
{
    // — DATA\AUDIO\<nickname>.utf ("audio" @ 005cab4c, "utf" @ 005cab48).
    char AudioDir[MAX_PATH];
    AudioDir[0] = '\0';
    GetFileSysPath(AudioDir, "audio", static_cast<IFileSystem*>(NeuronRuntime::Data));

    // the frame is 0x208 bytes -- two 0x104 buffers, the audio directory
    // (local_104) and the full path ([ESP+0x10]).
    char FullPath[MAX_PATH];
    FullPath[0] = '\0';
    _makepath(FullPath, nullptr, AudioDir, Name, "utf");
    if (_access(FullPath, 0) == -1)
        return;

    // Decision O31: baseline stored FullPath, which OpenScriptFileSystem hands
    // to NeuronRuntime::Data. The UTF factory joins it to the data root again
    // (ReadFile.cpp, _makepath(root, path)), which only resolves when the data
    // path is a single "..\NAME" (the shipped "..\DATA"). The path relative to
    // the data root opens the same archive under any data path.
    char RelPath[MAX_PATH];
    RelPath[0] = '\0';
    _makepath(RelPath, nullptr, "audio", Name, "utf");
    UtfPath = RelPath;
}

int LoadClientVoicesIni(char const* FullPath)
{
    // — returns how many profiles the file added (the field delta).
    unsigned int const StartCount = static_cast<unsigned int>(GVoiceProfiles.size());

    INI_Reader Reader;
    FVoiceProfile* Current = nullptr;

    if (Reader.open(FullPath, false) && Reader.read_header())
    {
        do
        {
            if (Reader.is_header("Voice"))
            {
                FVoiceProfile Local;
                unsigned int ExtendLength = 0;
                char ExtendName[0x80];
                ExtendName[0] = '\0';

                if (Reader.read_value())
                {
                    do
                    {
                        if (Reader.is_value("nickname"))
                        {
                            char const* Nickname = Reader.get_value_string();
                            Local.Id.Id = CreateID(Nickname);
                            Local.Name = StringAlloc(Nickname, false);
                        }
                        else if (Reader.is_value("extend"))
                        {
                            char const* Extend = Reader.get_value_string();
                            if (Extend == nullptr)
                            {
                                ExtendLength = 0;
                                ExtendName[0] = '\0';
                            }
                            else
                            {
                                ExtendLength = CopyBoundedString(ExtendName, sizeof(ExtendName), Extend);
                            }
                        }
                        else if (Reader.is_value("script"))
                        {
                            // std::string straight from the pointer
                            // (REPNE SCASB, no null test), appended to the list.
                            char const* Script = Reader.get_value_string();
                            Local.Scripts.push_back(std::string(Script));
                        }
                        else
                        {
                            FDUMP(0x100002,
                                  "%s(%d) : *** WARNING: Voice (%s) has invalid parameter %s",
                                  kVoiceSource, 0x203, Local.Id.get_string(),
                                  Reader.get_name_ptr());
                            // log_link("") (the field) after every warning.
                            Reader.log_link("");
                        }
                    } while (Reader.read_value());
                }

                if (ExtendLength == 0)
                {
                    std::pair<std::map<unsigned int, FVoiceProfile>::iterator, bool> Inserted =
                        GVoiceProfiles.insert(
                            std::map<unsigned int, FVoiceProfile>::value_type(Local.Id.Id, Local));
                    if (Inserted.second)
                    {
                        Current = &Inserted.first->second;
                        Current->ResolveScriptFile();
                    }
                    else
                    {
                        Current = nullptr;
                    }
                }
                else
                {
                    // `extend = <nickname>`: the following [Sound] blocks land in that profile.
                    std::map<unsigned int, FVoiceProfile>::iterator Found =
                        GVoiceProfiles.find(CreateID(ExtendName));
                    Current = Found != GVoiceProfiles.end() ? &Found->second : nullptr;
                }
            }
            else if (Reader.is_header("Sound") || Reader.is_header("VoiceSound"))
            {
                if (Current == nullptr)
                {
                    FDUMP(0x100002, "%s(%d) : *** WARNING: %s", kVoiceSource, 0x21f,
                          "Voice [Sound] listed outside [Voice] block\n");
                    Reader.log_link("");
                }
                else
                {
                    FVoiceMsg Msg;
                    Msg.Owner = Current;

                    if (Reader.read_value())
                    {
                        do
                        {
                            if (Reader.is_value("file"))
                            {
                                Current->SetSoundFile(Msg.Id.Id, Reader.get_value_string());
                            }
                            else if (Reader.is_value("priority"))
                            {
                                Msg.Priority =
                                    static_cast<signed char>(Reader.get_value_int(0));
                            }
                            else if (Reader.is_value("attenuation"))
                            {
                                Msg.Attenuation =
                                    static_cast<signed char>(Reader.get_value_int(0));
                            }
                            else if (Reader.is_value("duration"))
                            {
                                // Parsed by the INI files, ignored by baseline (no store).
                            }
                            else if (Reader.is_value("msg"))
                            {
                                Msg.Id.Id = CreateID(Reader.get_value_string(0));
                            }
                            else
                            {
                                FDUMP(0x100002,
                                      "%s(%d) : *** WARNING: Voice (%s) has invalid [Sound] "
                                      "parameter %s",
                                      kVoiceSource, 0x249, Current->GetName(),
                                      Reader.get_name_ptr());
                                Reader.log_link("");
                            }
                        } while (Reader.read_value());
                    }

                    if (Msg.Id.IsEmpty())
                    {
                        FDUMP(0x100001,
                              "%s(%d) : *** ERROR: Voice (%s) missing [Sound] msg parameter",
                              kVoiceSource, 0x250, Current->GetName());
                        Reader.log_link("");
                    }
                    else if (Current->Messages.find(Msg.Id.Id) == Current->Messages.end())
                    {
                        // baseline inserts a default-constructed VoiceMsg, then the routine
                        // copies +4..+0x13 over it. First entry wins; duplicates are dropped.
                        Current->Messages.insert(
                            std::map<unsigned int, FVoiceMsg>::value_type(Msg.Id.Id, Msg));
                    }
                }
            }
        } while (Reader.read_header());
    }

    Reader.close();

    int const Added = static_cast<int>(GVoiceProfiles.size()) - static_cast<int>(StartCount);
    return Added;
}

FVoiceProfile* LookupClientVoice(unsigned int VoiceId)
{
    // .
    std::map<unsigned int, FVoiceProfile>::iterator Found = GVoiceProfiles.find(VoiceId);
    if (Found == GVoiceProfiles.end())
        return nullptr;
    return &Found->second;
}

void ClearClientVoices()
{
    // — engine shutdown (the routine).
    GVoiceProfiles.clear();
}

unsigned int GetClientVoiceCount()
{
    return static_cast<unsigned int>(GVoiceProfiles.size());
}

// (the "$BGCS_*" context tokens) is FCharacterObject::ResolveBgcsContextToken
// (gf_character_object.cpp), a method of the speaking character as in baseline.
