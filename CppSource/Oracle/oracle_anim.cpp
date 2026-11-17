// Oracle suites and data dump of work package P1.7 (animation): channel
// archetypes and animation scripts of SceneRuntime.dll. The Rust test
// crates/neuron-core/tests/oracle_anim.rs writes the same lines; the inputs
// come from crates/neuron-core/tests/fixtures/oracle/make_anim_fixtures.py.
//
// The oracle does not link SceneRuntime.dll (it exports nothing). Loading it
// runs its DllMain, which registers the Animation and ChannelArchetype
// factories with neuroncomponents; both components are then created through
// ICOManager as the client creates them. Archives are opened through the UTF
// factory ReadFile registered (RunVfsSuites and RunVfsDataDump run first).
//
// IChannelArchetype::Load is called directly; IAnimation2::OpenScript,
// AddScriptNames and EnumFsScripts build scripts, and the harness reads the
// objects they build through the layouts in SceneRuntime.h (FAnimScript,
// FAnimNameEntry, FAnimTrack, FChannelArchetypeObject). The DLL and this
// program are built with the same toolset and configuration, so their
// std::map and std::vector layouts agree.
//
// A channel whose flags keep packed bits after Load failed to unpack, and its
// frames are the DLL's shared unpacking buffer: releasing it would free that
// buffer twice, so such channels (and scripts holding one) are never released.

#include "oracle_support.h"

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "neuroncomponents.h"
#include "readfile.h"
#include "../SceneRuntime/SceneRuntime.h"

using namespace oracle;

namespace
{
    const unsigned long PackedMask = 0x1F0;
    const unsigned long ScalarOnly = 0x8;

    bool LoadSceneRuntime()
    {
        static int State = 0;
        if (State == 0)
        {
            State = LoadLibraryA("SceneRuntime.dll") != nullptr ? 1 : 2;
            if (State == 2)
            {
                fprintf(stderr, "cannot load SceneRuntime.dll (%lu)\n", GetLastError());
                GFailed = true;
            }
        }
        return State == 1;
    }

    IAnimation2* AcquireAnimation()
    {
        static IAnimation2* Animation = nullptr;
        if (Animation != nullptr || !LoadSceneRuntime())
            return Animation;
        FAggDesc18 Desc;
        memset(&Desc, 0, sizeof(Desc));
        Desc.Size = G_AggDescSizeAnimation;
        Desc.Name = "Animation";
        void* Instance = nullptr;
        if (NEURONCOMPONENTS_Acquire()->CreateInstance((AGGDESC*)(&Desc), &Instance) != GR_OK ||
            Instance == nullptr)
        {
            fprintf(stderr, "no Animation component\n");
            GFailed = true;
            return nullptr;
        }
        Animation = (IAnimation2*)(Instance);
        return Animation;
    }

    IChannelArchetype* NewChannelArchetype()
    {
        if (!LoadSceneRuntime())
            return nullptr;
        FAggDesc8 Desc;
        Desc.Size = G_AggDescSizeChannelArchetype;
        Desc.Name = "ChannelArchetype";
        void* Instance = nullptr;
        if (NEURONCOMPONENTS_Acquire()->CreateInstance((AGGDESC*)(&Desc), &Instance) != GR_OK)
            return nullptr;
        return (IChannelArchetype*)(Instance);
    }

    FFileSystemDesc MakeDesc(const char* Implementation, const char* Path)
    {
        FFileSystemDesc Desc;
        memset(&Desc, 0, sizeof(Desc));
        Desc.Size = 0x34;
        Desc.ComponentName = "FileSystem";
        Desc.Implementation = Implementation;
        Desc.Path = Path;
        Desc.Access = 0x80000000;
        Desc.ShareMode = 1;
        Desc.Creation = 3;
        Desc.Flags = 0x08000080;
        Desc.Field30 = 0xFFFFFFFF;
        return Desc;
    }

    // An archive through the registered UTF factory, relative to the
    // current directory.
    IFileSystem* OpenArchive(const std::string& Path, GENRESULT* Result)
    {
        FFileSystemDesc Desc = MakeDesc("UTF", Path.c_str());
        IFileSystem* Fs = nullptr;
        *Result = NEURONCOMPONENTS_Acquire()->CreateInstance((AGGDESC*)(&Desc), (void**)(&Fs));
        if (*Result != GR_OK)
            return nullptr;
        return Fs;
    }

    FChannelArchetypeObject* AsObject(IChannelArchetype* Channel)
    {
        return static_cast<FChannelArchetypeObject*>(Channel);
    }

    std::string ChannelFields(const FChannelArchetypeObject* Channel)
    {
        return Format("\"count\":\"%lu\",\"interval\":\"%08x\",\"flags\":\"%lx\",\"stride\":\"%lu\",\"duration\":\"%08x\"",
                      Channel->FrameCount, FloatBits(Channel->SampleRate), Channel->Flags, Channel->Stride,
                      FloatBits(Channel->Duration));
    }

    std::string Render(const unsigned char* Data, size_t Length, bool bHash)
    {
        return bHash ? "fnv:" + Fnv64(Data, Length) : Hex(Data, Length);
    }

    // The frame buffer an archetype in a script holds; see script_frames in
    // oracle_anim.rs.
    std::string ScriptFrames(const FChannelArchetypeObject* Channel, bool bHash)
    {
        if (Channel->Frames == nullptr)
            return "-";
        if ((Channel->Flags & PackedMask) != 0)
            return "pool";
        if (Channel->Flags == ScalarOnly)
            return "event";
        return Render(Channel->Frames, Channel->FrameCount * Channel->Stride, bHash);
    }

    std::string NameField(const char* Name)
    {
        return Name == nullptr ? "-" : HexC(Name);
    }

    // Whether any channel of the script holds the shared unpacking buffer.
    bool HoldsPool(FAnimScript* Script)
    {
        for (FAnimNameMap::iterator It = Script->Names.begin(); It != Script->Names.end(); ++It)
        {
            const std::vector<FAnimTrack>& Tracks = It->second.Tracks;
            for (size_t Index = 0; Index < Tracks.size(); ++Index)
            {
                const FChannelArchetypeObject* Channel = Tracks[Index].Channel;
                if (Channel != nullptr && Channel->Frames != nullptr && (Channel->Flags & PackedMask) != 0)
                    return true;
            }
        }
        return false;
    }

    void ScriptLines(FAnimScript* Script, bool bHash, Suite& Out)
    {
        for (FAnimNameMap::iterator It = Script->Names.begin(); It != Script->Names.end(); ++It)
        {
            const FAnimNameEntry& Entry = It->second;
            const std::vector<FAnimTrack>& Tracks = Entry.Tracks;
            std::string Longest = "-";
            if (Entry.LongestChannel != nullptr)
            {
                for (size_t Index = 0; Index < Tracks.size(); ++Index)
                {
                    if (Tracks[Index].Channel == Entry.LongestChannel)
                    {
                        Longest = Format("%u", (unsigned int)Index);
                        break;
                    }
                }
            }
            Out.Line(Format("{\"entry\":\"%08lx\",\"name\":\"%s\",\"tracks\":\"%u\",\"root\":\"%lu\",\"height\":\"%08x\",\"longest\":\"%s\"}",
                            It->first, NameField(Entry.Name).c_str(), (unsigned int)Tracks.size(),
                            Entry.bHasRootHeight, FloatBits(Entry.RootHeight), Longest.c_str()));
            for (size_t Index = 0; Index < Tracks.size(); ++Index)
            {
                const FAnimTrack& Track = Tracks[Index];
                std::string Line = Format("{\"track\":\"%u\",\"parent\":\"%08lx\",\"parent_name\":\"%s\",\"child\":\"%08lx\",\"child_name\":\"%s\"",
                                          (unsigned int)Index, Track.ParentCrc, NameField(Track.ParentName).c_str(),
                                          Track.ChildCrc, NameField(Track.ChildName).c_str());
                if (Track.Channel == nullptr)
                    Line += ",\"channel\":\"-\"}";
                else
                    Line += ",\"channel\":\"1\"," + ChannelFields(Track.Channel) + ",\"frames\":\"" +
                            ScriptFrames(Track.Channel, bHash) + "\"}";
                Out.Line(Line);
            }
        }
    }

    // Releases a script unless one of its channels holds the shared buffer.
    void CloseScript(IAnimation2* Animation, void* Script)
    {
        if (!HoldsPool((FAnimScript*)(Script)))
            Animation->CloseScript(Script);
    }

    unsigned long FileSize(IFileSystem* Fs, const char* Name)
    {
        FFileSystemDesc Desc = MakeDesc(nullptr, Name);
        const int Handle = Fs->OpenByDesc(&Desc);
        if (Handle == -1)
            return 0;
        const unsigned long Size = Fs->GetSize((unsigned long)Handle, nullptr);
        Fs->CloseFile((unsigned long)Handle);
        return Size;
    }

    // anim_channels: IChannelArchetype::Load on each directory of
    // anim/channels.utf.
    void AnimChannels()
    {
        Suite Out("anim_channels.jsonl");
        GENRESULT Result = GR_OK;
        IFileSystem* Archive = OpenArchive("anim/channels.utf", &Result);
        if (Archive == nullptr)
        {
            fprintf(stderr, "anim: cannot open anim/channels.utf (%ld)\n", Result);
            GFailed = true;
            return;
        }
        for (const std::string& Name : InputLines("anim/channels.list"))
        {
            FFileSystemDesc Desc = MakeDesc(nullptr, Name.c_str());
            IFileSystem* Nested = nullptr;
            const GENRESULT Created = Archive->CreateInstance((AGGDESC*)(&Desc), (void**)(&Nested));
            if (Created != GR_OK || Nested == nullptr)
            {
                Out.Line(Format("{\"channel\":\"%s\",\"create\":\"%ld\"}", Name.c_str(), Created));
                continue;
            }
            IChannelArchetype* Channel = NewChannelArchetype();
            if (Channel == nullptr)
            {
                fprintf(stderr, "anim: no ChannelArchetype component\n");
                GFailed = true;
                Nested->Release();
                break;
            }
            GDumps = 0;
            const GENRESULT Loaded = Channel->Load(Nested);
            const int Dumps = GDumps;
            const FChannelArchetypeObject* Object = AsObject(Channel);
            std::string Frames = "-";
            if (Loaded == GR_OK)
            {
                const unsigned long Length = Object->Flags == ScalarOnly ? FileSize(Nested, "Frames")
                                                                         : Object->FrameCount * Object->Stride;
                Frames = Hex(Object->Frames, Length);
            }
            Out.Line(Format("{\"channel\":\"%s\",\"result\":\"%08lx\",", Name.c_str(), (unsigned long)Loaded) +
                     ChannelFields(Object) + Format(",\"dumps\":\"%d\",\"frames\":\"", Dumps) + Frames + "\"}");
            if (Object->Frames == nullptr || (Object->Flags & PackedMask) == 0)
                Channel->Release();
            Nested->Release();
        }
        Archive->Release();
    }

    void __cdecl CollectName(const char* Name, void* Context)
    {
        ((std::vector<std::string>*)(Context))->push_back(Name);
    }

    // anim_scripts: EnumFsScripts and OpenScript on each archive.
    void AnimScripts()
    {
        Suite Out("anim_scripts.jsonl");
        IAnimation2* Animation = AcquireAnimation();
        if (Animation == nullptr)
            return;
        for (const std::string& Path : InputLines("anim/scripts.list"))
        {
            GENRESULT Result = GR_OK;
            IFileSystem* Fs = OpenArchive(Path, &Result);
            Out.Line(Format("{\"archive\":\"%s\",\"create\":\"%ld\"}", Path.c_str(), Result));
            if (Fs == nullptr)
                continue;
            IFileSystem* EnumFs = OpenArchive(Path, &Result);
            std::vector<std::string> Names;
            if (EnumFs == nullptr ||
                Animation->EnumFsScripts((void*)(&CollectName), EnumFs, &Names) == 0xFFFFFFFF)
            {
                Out.Line("{\"enum\":\"-1\"}");
            }
            else
            {
                std::string Joined;
                for (size_t Index = 0; Index < Names.size(); ++Index)
                    Joined += (Index == 0 ? "" : ",") + HexC(Names[Index].c_str());
                Out.Line("{\"enum\":\"" + Joined + "\"}");
            }
            if (EnumFs != nullptr)
                EnumFs->Release();
            void* Script = Animation->OpenScript(Fs);
            if (Script == (void*)((ptrdiff_t)(-1)) || Script == nullptr)
            {
                Out.Line("{\"script\":\"0\"}");
            }
            else
            {
                FAnimScript* Typed = (FAnimScript*)(Script);
                Out.Line(Format("{\"script\":\"1\",\"entries\":\"%u\"}", (unsigned int)Typed->Names.size()));
                ScriptLines(Typed, false, Out);
                CloseScript(Animation, Script);
            }
            Fs->Release();
        }
    }

    std::vector<std::string> Split(const std::string& Text, char Separator)
    {
        std::vector<std::string> Out;
        size_t Start = 0;
        for (;;)
        {
            const size_t At = Text.find(Separator, Start);
            Out.push_back(Text.substr(Start, At == std::string::npos ? std::string::npos : At - Start));
            if (At == std::string::npos)
                return Out;
            Start = At + 1;
        }
    }

    // anim_add_names: AddScriptNames calls on one script and file system.
    void AnimAddNames()
    {
        Suite Out("anim_add_names.jsonl");
        IAnimation2* Animation = AcquireAnimation();
        if (Animation == nullptr)
            return;
        for (const std::string& Spec : InputLines("anim/add.list"))
        {
            const std::vector<std::string> Parts = Split(Spec, '|');
            GENRESULT Result = GR_OK;
            IFileSystem* Fs = OpenArchive(Parts[0], &Result);
            if (Fs == nullptr)
            {
                fprintf(stderr, "anim: cannot open %s (%ld)\n", Parts[0].c_str(), Result);
                GFailed = true;
                continue;
            }
            Out.Line("{\"archive\":\"" + Parts[0] + "\"}");
            void* Script = Animation->CreateEmptyScript();
            for (size_t Call = 1; Call < Parts.size(); ++Call)
            {
                std::vector<Bytes> Storage;
                if (!Parts[Call].empty())
                {
                    for (const std::string& Token : Split(Parts[Call], ','))
                        Storage.push_back(Token == "-" ? Bytes() : Unhex(Token));
                }
                std::vector<const char*> Names;
                for (const Bytes& Name : Storage)
                    Names.push_back(Name.empty() ? nullptr : CStr(Name));
                GDumps = 0;
                const unsigned long Added =
                    Animation->AddScriptNames(Script, Fs, Names.empty() ? nullptr : Names.data(), (int)Names.size());
                Out.Line(Format("{\"call\":\"%u\",\"result\":\"%08lx\",\"dumps\":\"%d\"}", (unsigned int)(Call - 1),
                                Added, GDumps));
            }
            FAnimScript* Typed = (FAnimScript*)(Script);
            Out.Line(Format("{\"entries\":\"%u\"}", (unsigned int)Typed->Names.size()));
            ScriptLines(Typed, false, Out);
            CloseScript(Animation, Script);
            Fs->Release();
        }
    }
}

void RunAnimSuites()
{
    AnimChannels();
    AnimScripts();
    AnimAddNames();
}

void RunAnimDataDump(const std::vector<std::string>& Files)
{
    IAnimation2* Animation = AcquireAnimation();
    if (Animation == nullptr)
        return;
    Suite List(GOutputDir + "\\anim\\files.txt");
    unsigned int Count = 0;
    for (const std::string& File : Files)
    {
        if (FileMagic(File) != "UTF ")
            continue;
        GENRESULT Result = GR_OK;
        IFileSystem* Fs = OpenArchive(File, &Result);
        if (Fs == nullptr)
            continue;
        void* Script = Animation->OpenScript(Fs);
        if (Script == (void*)((ptrdiff_t)(-1)) || Script == nullptr)
        {
            Fs->Release();
            continue;
        }
        List.Line(File);
        ++Count;
        FAnimScript* Typed = (FAnimScript*)(Script);
        Suite Out(DataOutputFile("anim", File));
        Out.Line(Format("{\"archive\":\"%s\",\"entries\":\"%u\"}", File.c_str(), (unsigned int)Typed->Names.size()));
        ScriptLines(Typed, true, Out);
        CloseScript(Animation, Script);
        Fs->Release();
    }
    printf("dumped %u animation scripts\n", Count);
}
