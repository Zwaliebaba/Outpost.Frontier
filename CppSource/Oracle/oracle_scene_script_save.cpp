// P4.14: saving a scene and the script engine around it -- FScriptEngine of
// CppSource/SceneScript/SceneScript.cpp (Slot4Load with the default or a custom load parser,
// Slot5Save with the default save parser, Slot40SetCom, Slot41SetLoadParser, Slot43LoadPath,
// Slot44SavePath) and FSceneScriptScene::SaveToFileSystem of SceneScriptScene.cpp -- run as
// the reference runs them.
//
// SceneScript.cpp is compiled into this file, with its DllMain and the three functions the
// other scene-script suites define renamed, so FScriptEngine of its anonymous namespace can
// be made here. Its loads go through the parser of oracle_scene_script_build.cpp with real
// curves and paths (GRealObjects). The engine talks to recording stand-ins at its seams:
//   - IFileSystem: a buffer the load reads, and a writer that records each Write (the
//     whole length taken, or half of it for the short-write cases);
//   - ISceneScriptComContext: Slot3 keeps a user event's text by the event's id; Slot5
//     and Slot4 hand it back (context 1), or ask for 4 bytes and fail (context 2);
//   - the component manager (NEURONCOMPONENTS_Acquire, redirected below) answers the
//     engine's two FileSystem requests: a read of "script<i>.thn" gets buffer i, any
//     other read fails with -6, a write gets a recording writer;
//   - ISceneScriptLoadParser (Slot41SetLoadParser): a recording parser of four kinds whose
//     Load fails (1) or succeeds, whose Build adds nothing (2), a monitor and a camera with a
//     user property before it changes its duration (3), or a camera and fails (4).
// The Rust side is crates/neuron-client/tests/support/scene_script_save.rs.
//
// Line format (suite scene_script_save.jsonl, fixture scene_script_save.list):
//   {"case":"<n>","fs":"<path str>","access":"<hex>","creation":"<n>"}    each manager request
//   {"case":"<n>","write":"<hex bytes>","r":"<bytes taken>"}            each Write
//   {"case":"<n>","com":"<3|4|5>","start":..,"primary":..,"duration":..,"taken":"<0|1>"[,"text":"<str>"]
//    [,"size":..],"r":"<r>"}                                            each context call
//   {"case":"<n>","parser":"load","size":..,"r":..} {"case":"<n>","parser":"duration","r":..}
//   {"case":"<n>","parser":"build","size":..,"com":"<0|1>"} {"case":"<n>","parser":"add","r":..,"index":..}
//   {"case":"<n>","parser":"prop","r":..}                               each load parser call
//   {"case":"<n>","op":"<token>","r":"<r>","state":..,"entities":..,"events":..,"duration":..}
// Data dump (kind thn_save): every .thn under DATA loaded with context 1 into a fresh engine
// and module, then saved:
//   {"file":"<path>","load":"<r>","entities":..,"events":..,"duration":..}
//   {"write":"<hex bytes>"}, then {"save":"<r>"}.
// <str> is s<hex bytes> or null.

#include <cstdint>

#define NEURONCOMPONENTS_Acquire OracleSceneScriptSaveManager
#define SceneScriptFdump OracleSaveSceneScriptFdump
#define SceneScriptNameEqual OracleSaveSceneScriptNameEqual
#define SceneScriptAcquire3DMathEngine OracleSaveSceneScriptAcquire3DMathEngine
#define DllMain SceneScriptDllMain
// Not defined in another oracle file today; renamed so none can collide with them.
#define SceneScriptLogStub OracleSaveSceneScriptLogStub
#define SceneScriptMathTransform OracleSaveSceneScriptMathTransform
#define SceneScriptMathMulMatrix OracleSaveSceneScriptMathMulMatrix
#define SceneScriptMathMulTransform OracleSaveSceneScriptMathMulTransform
#define SceneScriptMathRotate OracleSaveSceneScriptMathRotate
#define SceneScriptMatrixToQuat OracleSaveSceneScriptMatrixToQuat
#define SceneScriptMathTransposeTransform OracleSaveSceneScriptMathTransposeTransform
#include "../SceneScript/SceneScript.cpp"
#undef SceneScriptMathTransposeTransform
#undef SceneScriptMatrixToQuat
#undef SceneScriptMathRotate
#undef SceneScriptMathMulTransform
#undef SceneScriptMathMulMatrix
#undef SceneScriptMathTransform
#undef SceneScriptLogStub
#undef DllMain
#undef SceneScriptAcquire3DMathEngine
#undef SceneScriptNameEqual
#undef SceneScriptFdump

#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "oracle_support.h"

namespace scene_script_build
{
    extern bool GRealObjects;
    void OpenModule();
    void CloseModule();
    std::string Str(const char* Text);
}

namespace scene_script_save
{
    using oracle::Format;

    // The current suite's output and case head; null while nothing records.
    oracle::Suite* GOut = nullptr;
    std::string GHead;
    // The buffers of the current case, for the manager's reads.
    std::vector<oracle::Bytes> GScripts;
    // The data dump leaves out the bytes each write took and the context's calls.
    bool GWriteResults = true;

    void Line(const std::string& Body)
    {
        if (GOut != nullptr)
            GOut->Line(GHead + Body);
    }

    void ContextLine(const std::string& Body)
    {
        if (GWriteResults)
            Line(Body);
    }

    std::string Hex(const void* Data, size_t Size)
    {
        return oracle::Hex(Data, Size);
    }

    std::vector<std::string> Split(const std::string& Text, char Separator)
    {
        std::vector<std::string> Parts;
        size_t Start = 0;
        while (true)
        {
            const size_t End = Text.find(Separator, Start);
            Parts.push_back(Text.substr(Start, End == std::string::npos ? std::string::npos : End - Start));
            if (End == std::string::npos)
                break;
            Start = End + 1;
        }
        return Parts;
    }

    float HexFloat(const std::string& Text)
    {
        const unsigned int Bits = static_cast<unsigned int>(std::strtoul(Text.c_str(), nullptr, 16));
        float Value;
        std::memcpy(&Value, &Bits, sizeof(Value));
        return Value;
    }

    // A file: the bytes a load reads, or a writer that records what a save writes.
    class FSaveFileSystem final : public IFileSystem
    {
    public:
        FSaveFileSystem(const unsigned char* Data, size_t Size) : Data(Data), Size(Size), bWriter(false), bShort(false) {}
        explicit FSaveFileSystem(bool bShortWrites) : Data(nullptr), Size(0), bWriter(true), bShort(bShortWrites) {}

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API CreateInstance(AGGDESC*, void** Instance) override
        {
            *Instance = nullptr;
            return GR_GENERIC;
        }
        int NEURONCOMPONENTS_API CloseFile(unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API Read(unsigned long, void* Buffer, unsigned long Count, unsigned long* BytesRead,
                                      unsigned long) override
        {
            const size_t Copy = Count < Size ? Count : Size;
            if (Copy != 0)
                std::memcpy(Buffer, Data, Copy);
            if (BytesRead != nullptr)
                *BytesRead = static_cast<unsigned long>(Copy);
            return 1;
        }
        int NEURONCOMPONENTS_API Write(unsigned long, const void* Buffer, unsigned long Count, unsigned long* Written,
                                       const void*) override
        {
            const unsigned long Taken = bShort ? Count / 2 : Count;
            Line("\"write\":\"" + Hex(Buffer, Count) +
                 (GWriteResults ? Format("\",\"r\":\"%lu\"}", Taken) : std::string("\"}")));
            if (Written != nullptr)
                *Written = Taken;
            return bWriter ? 1 : 0;
        }
        int NEURONCOMPONENTS_API RejectOperation1C(unsigned long, unsigned long, unsigned long, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API SeekFile(unsigned long, unsigned long, unsigned long*, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API SetEndOfFile(unsigned long) override { return 0; }
        unsigned long NEURONCOMPONENTS_API GetSize(unsigned long, unsigned long* HighOut) override
        {
            if (HighOut != nullptr)
                *HighOut = 0;
            return static_cast<unsigned long>(Size);
        }
        int NEURONCOMPONENTS_API ReturnOne2C(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long) override { return 1; }
        int NEURONCOMPONENTS_API ReturnOne30(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long) override { return 1; }
        int NEURONCOMPONENTS_API GetFileTimes(unsigned long, FILETIME*, FILETIME*, FILETIME*) override { return 0; }
        int NEURONCOMPONENTS_API RejectOperation38(unsigned long, unsigned long, unsigned long, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API CreateMapping(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long,
                                               unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API MapFileView(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API UnmapFileView(unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API FindFirst(const char*, WIN32_FIND_DATAA*) override { return -1; }
        int NEURONCOMPONENTS_API FindNext(unsigned long, WIN32_FIND_DATAA*) override { return 0; }
        int NEURONCOMPONENTS_API FindClose(unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API RejectOperation54(const char*, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API RejectOperation58(unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API GetCurrentPath(unsigned long, char* Out) override
        {
            if (Out != nullptr)
                Out[0] = '\0';
            return 0;
        }
        int NEURONCOMPONENTS_API OpenNode(const char*) override { return 0; }
        int NEURONCOMPONENTS_API RejectOperation64(unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API RejectOperation68(unsigned long, unsigned long, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API RejectOperation6C(unsigned long, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API GetPathAttributes(const char*) override { return -1; }
        int NEURONCOMPONENTS_API RejectOperation74(unsigned long, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API GetFileSystemError() override { return 0; }
        int NEURONCOMPONENTS_API OpenByDesc(FFileSystemDesc*) override { return 0; }
        int NEURONCOMPONENTS_API GetFilePosition(unsigned long, unsigned long*) override { return 0; }
        void NEURONCOMPONENTS_API GetFileSysPath(char* Out, unsigned long MaxChars) override
        {
            if (Out != nullptr && MaxChars != 0)
                std::snprintf(Out, MaxChars, "%s", "oracle");
        }
        int NEURONCOMPONENTS_API GetAccessFlags() override { return 0; }
        int NEURONCOMPONENTS_API GetParentFileSystem(IFileSystem** Out) override
        {
            *Out = nullptr;
            return 0;
        }
        int NEURONCOMPONENTS_API FailOperation90(unsigned long, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API FailOperation94(unsigned long, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API FailOperation98(unsigned long, unsigned long, unsigned long, unsigned long,
                                                 unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API FailOperation9C(unsigned long, unsigned long, unsigned long, unsigned long,
                                                 unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API InvokeLockedCallback(unsigned long, unsigned long, unsigned long) override { return 0; }
        int NEURONCOMPONENTS_API ResolvePath(char* Dest, const char*, unsigned long DestChars) override
        {
            if (Dest != nullptr && DestChars != 0)
                Dest[0] = '\0';
            return 0;
        }

    private:
        const unsigned char* Data;
        size_t Size;
        bool bWriter;
        bool bShort;
    };

    // The engine's FileSystem requests (Slot43LoadPath, Slot44SavePath). Objects live until
    // the process ends; the engine releases them, which does nothing here.
    class FSaveManager final : public ICOManager
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API CreateInstance(AGGDESC* Desc, void** Instance) override
        {
            *Instance = nullptr;
            const FFileSystemDesc* Fs = reinterpret_cast<const FFileSystemDesc*>(Desc);
            if (Fs->Size != 0x34 || Fs->ComponentName == nullptr || std::strcmp(Fs->ComponentName, "FileSystem") != 0)
                return GR_INTERFACE_UNSUPPORTED;
            Line("\"fs\":\"" + scene_script_build::Str(Fs->Path) +
                 Format("\",\"access\":\"%08lx\",\"creation\":\"%lu\"}", Fs->Access, Fs->Creation));
            if (Fs->Access == 0xC0000000ul)
            {
                *Instance = static_cast<IFileSystem*>(new FSaveFileSystem(false));
                return GR_OK;
            }
            for (size_t Index = 0; Index < GScripts.size(); ++Index)
            {
                if (Fs->Path != nullptr && Format("script%u.thn", static_cast<unsigned int>(Index)) == Fs->Path)
                {
                    const oracle::Bytes& Script = GScripts[Index];
                    *Instance = static_cast<IFileSystem*>(new FSaveFileSystem(Script.data(), Script.size() - 1));
                    return GR_OK;
                }
            }
            return static_cast<GENRESULT>(-6);
        }
        GENRESULT NEURONCOMPONENTS_API RegisterComponent(INeuronComponent*, const char*, void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UnregisterComponent(INeuronComponent*, const char*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API EnumerateComponents(const char*, FComponentEnumCallback, void*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API AddLibrary(const char*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API RemoveLibrary(const char*) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Shutdown() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetINIConfig(const char*, unsigned int) override { return GR_GENERIC; }
    };

    FSaveManager GManager;

    // An event as the context lines name it: its start, primary and duration (its id is the
    // module's serial number, which the port numbers per scene; divergence D249), and whether
    // the engine took it (an id of -1 when it did not).
    std::string Event(const FSceneScriptUserEventDesc* Desc)
    {
        return Format("\"start\":\"%d\",\"primary\":\"%d\",\"duration\":\"%d\",\"taken\":\"%d\"",
                      Desc->Common.StartMs, Desc->Common.PrimaryIndex, Desc->Common.DurationMs,
                      Desc->Common.UniqueId == -1 ? 0 : 1);
    }

    // The two COM contexts. Both keep Slot3's text by the event's id.
    class FContext final : public ISceneScriptComContext
    {
    public:
        explicit FContext(int Kind) : Kind(Kind) {}

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char*, void** Out) override
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API Slot3(FSceneScriptUserEventDesc* Desc, const char* String) override
        {
            const int Id = Desc->Common.UniqueId;
            Texts[Id] = String == nullptr ? std::string("\x01null") : std::string(String);
            ContextLine("\"com\":\"3\"," + Event(Desc) + ",\"text\":\"" + scene_script_build::Str(String) +
                 "\",\"r\":\"0\"}");
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Slot4(FSceneScriptUserEventDesc* Desc, char* Buffer, unsigned int Size) override
        {
            const int Id = Desc->Common.UniqueId;
            GENRESULT R = GR_GENERIC;
            if (Kind == 1)
            {
                const std::string Text = Find(Id);
                const size_t Copy = Text.size() + 1 < Size ? Text.size() + 1 : Size;
                std::memcpy(Buffer, Text.c_str(), Copy);
                R = GR_OK;
            }
            else
                std::memcpy(Buffer, "ab", 3);
            ContextLine("\"com\":\"4\"," + Event(Desc) + Format(",\"size\":\"%u\",\"r\":\"%d\"}", Size, R));
            return R;
        }
        unsigned int NEURONCOMPONENTS_API Slot5(FSceneScriptUserEventDesc* Desc) override
        {
            const int Id = Desc->Common.UniqueId;
            unsigned int Size = 4;
            if (Kind == 1)
            {
                const std::map<int, std::string>::const_iterator Entry = Texts.find(Id);
                Size = Entry == Texts.end() || Entry->second == "\x01null"
                    ? 0u : static_cast<unsigned int>(Entry->second.size() + 1);
            }
            ContextLine("\"com\":\"5\"," + Event(Desc) + Format(",\"r\":\"%u\"}", Size));
            return Size;
        }

    private:
        std::string Find(int Id) const
        {
            const std::map<int, std::string>::const_iterator Entry = Texts.find(Id);
            return Entry == Texts.end() ? std::string() : Entry->second;
        }

        int Kind;
        std::map<int, std::string> Texts;
    };

    // The recording load parser of the q<kind> token. The engine borrows it; it lives until
    // the case ends.
    class FLoadParser final : public ISceneScriptLoadParser
    {
    public:
        explicit FLoadParser(int Kind) : Kind(Kind), DurationMs(1500) {}

        void* NEURONCOMPONENTS_API Destroy(unsigned int) override { return this; }
        unsigned int NEURONCOMPONENTS_API GetEntityCount() override { return 0; }
        unsigned int NEURONCOMPONENTS_API GetEventCount() override { return 0; }
        unsigned int NEURONCOMPONENTS_API GetDuration() override
        {
            Line(Format("\"parser\":\"duration\",\"r\":\"%u\"}", DurationMs));
            return DurationMs;
        }
        GENRESULT NEURONCOMPONENTS_API Load(IFileSystem* FileSystem) override
        {
            const GENRESULT R = Kind == 1 ? GR_GENERIC : GR_OK;
            Line(Format("\"parser\":\"load\",\"size\":\"%lu\",\"r\":\"%d\"}", FileSystem->GetSize(0, nullptr), R));
            return R;
        }
        GENRESULT NEURONCOMPONENTS_API Build(IFileSystem* FileSystem, IScriptEngine* Engine,
                                             ISceneScriptComContext* ComContext) override
        {
            Line(Format("\"parser\":\"build\",\"size\":\"%lu\",\"com\":\"%d\"}", FileSystem->GetSize(0, nullptr),
                        ComContext != nullptr ? 1 : 0));
            if (Kind == 3)
            {
                Add(Engine, kSceneScriptEntityMonitor, "mon", 1.0f);
                const int Camera = Add(Engine, kSceneScriptEntityCamera, "cam", -2.0f);
                const GENRESULT R = Engine->Slot34SetUserProp(Camera, "Note", "seen");
                Line(Format("\"parser\":\"prop\",\"r\":\"%d\"}", R));
                DurationMs = 2500;
            }
            else if (Kind == 4)
            {
                Add(Engine, kSceneScriptEntityCamera, "solo", 0.5f);
                return GR_GENERIC;
            }
            return GR_OK;
        }

    private:
        // A zero-filled descriptor of Type named Name at (X, 2, 3) with the identity
        // orientation and the camera words 0.5, 1, 1000 and 1.25.
        int Add(IScriptEngine* Engine, int Type, const char* Name, float X)
        {
            FSceneScriptEntitySnap Snap;
            std::memset(&Snap, 0, sizeof(Snap));
            Snap.Type = Type;
            Snap.Pos[0] = X;
            Snap.Pos[1] = 2.0f;
            Snap.Pos[2] = 3.0f;
            Snap.Orient[0] = 1.0f;
            Snap.Orient[4] = 1.0f;
            Snap.Orient[8] = 1.0f;
            Snap.Name = const_cast<char*>(Name);
            Snap.Fovh = 0.5f;
            Snap.NearPlane = 1.0f;
            Snap.FarPlane = 1000.0f;
            Snap.Aspect = 1.25f;
            int Index = -7;
            const GENRESULT R = Engine->Slot35AddEntity(&Snap, &Index);
            Line(Format("\"parser\":\"add\",\"r\":\"%d\",\"index\":\"%d\"}", R, Index));
            return Index;
        }

        int Kind;
        unsigned int DurationMs;
    };

    std::string Status(FScriptEngine* Engine)
    {
        return Format("\"state\":\"%d\",\"entities\":\"%lu\",\"events\":\"%lu\",\"duration\":\"%d\"}",
                      Engine->Slot21GetState(), Engine->Slot6EntityCount(), Engine->Slot14EventCount(),
                      Engine->Slot17GetDuration());
    }

    GENRESULT RunOp(FScriptEngine* Engine, const std::string& Op, std::vector<FContext*>& Contexts,
                    std::vector<FLoadParser*>& Parsers)
    {
        const std::string Arg = Op.substr(1);
        switch (Op[0])
        {
        case 'q':
        {
            ISceneScriptLoadParser* Parser = nullptr;
            if (Arg != "-")
            {
                Parsers.push_back(new FLoadParser(std::atoi(Arg.c_str())));
                Parser = Parsers.back();
            }
            return Engine->Slot41SetLoadParser(Parser);
        }
        case 'c':
        {
            ISceneScriptComContext* Context = nullptr;
            if (Arg != "0")
            {
                Contexts.push_back(new FContext(std::atoi(Arg.c_str())));
                Context = Contexts.back();
            }
            return Engine->Slot40SetCom(Context);
        }
        case 'L':
        {
            if (Arg == "-")
                return Engine->Slot4Load(nullptr);
            const oracle::Bytes& Script = GScripts[static_cast<size_t>(std::atoi(Arg.c_str()))];
            FSaveFileSystem File(Script.data(), Script.size() - 1);
            return Engine->Slot4Load(static_cast<IFileSystem*>(&File));
        }
        case 'l':
        {
            if (Arg == "-")
                return Engine->Slot43LoadPath(nullptr);
            const std::string Path = Arg == "x" ? std::string("missing.thn") : "script" + Arg + ".thn";
            return Engine->Slot43LoadPath(Path.c_str());
        }
        case 'S':
        case 's':
        {
            FSaveFileSystem File(Op[0] == 's');
            return Engine->Slot5Save(static_cast<IFileSystem*>(&File));
        }
        case 'N': return Engine->Slot5Save(nullptr);
        case 'W': return Engine->Slot44SavePath("saved.thn");
        case 't': return Engine->Slot26Tick(std::atoi(Arg.c_str()));
        case 'g': return Engine->Slot22Start(std::atoi(Arg.c_str()));
        case 'D': return Engine->Slot18SetDuration(std::atoi(Arg.c_str()));
        case 'u':
        {
            const std::vector<std::string> Parts = Split(Arg, ':');
            oracle::Bytes Key = oracle::Unhex(Parts[1]);
            oracle::Bytes Value = oracle::Unhex(Parts[2]);
            return Engine->Slot34SetUserProp(std::atoi(Parts[0].c_str()), reinterpret_cast<const char*>(Key.data()),
                                             reinterpret_cast<const char*>(Value.data()));
        }
        case 'r': return Engine->Slot36RemoveEntity(std::atoi(Arg.c_str()));
        case 'e':
        {
            // By position in the master list: ids are the module's (divergence D249).
            const FSceneScriptEventDesc* Desc =
                static_cast<const FSceneScriptEventDesc*>(Engine->Slot15EventByIndex(std::atoi(Arg.c_str())));
            return Engine->Slot39RemoveEvent(Desc != nullptr ? Desc->UniqueId : -1);
        }
        case 'P':
        case 'O':
        {
            std::vector<float> Words;
            for (const std::string& Word : Split(Arg, '/'))
                Words.push_back(HexFloat(Word));
            IGeoTransformControl* Geo = static_cast<IGeoTransformControl*>(Engine);
            return Op[0] == 'P' ? Geo->SetPosition(Words.data()) : Geo->SetOrientation(Words.data());
        }
        default:
            fprintf(stderr, "scene_script_save: bad op %s\n", Op.c_str());
            oracle::GFailed = true;
            return GR_GENERIC;
        }
    }

    void SaveSuite()
    {
        oracle::Suite Out("scene_script_save.jsonl");
        GOut = &Out;
        scene_script_build::GRealObjects = true;
        unsigned int Case = 0;
        for (const std::string& Text : oracle::InputLines("scene_script_save.list"))
        {
            const std::vector<std::string> Fields = Split(Text, ' ');
            GScripts.clear();
            for (size_t Index = 1; Index < Fields.size(); ++Index)
                GScripts.push_back(oracle::Unhex(Fields[Index]));
            GHead = Format("{\"case\":\"%u\",", Case);
            scene_script_build::OpenModule();
            FScriptEngine* Engine = new FScriptEngine();
            std::vector<FContext*> Contexts;
            std::vector<FLoadParser*> Parsers;
            for (const std::string& Op : Split(Fields[0], ','))
            {
                const GENRESULT R = RunOp(Engine, Op, Contexts, Parsers);
                Line("\"op\":\"" + Op + Format("\",\"r\":\"%d\",", R) + Status(Engine));
            }
            // The engine releases its context on the way out; nothing records it.
            GOut = nullptr;
            Engine->Release();
            GOut = &Out;
            for (FLoadParser* Parser : Parsers)
                delete Parser;
            scene_script_build::CloseModule();
            ++Case;
        }
        scene_script_build::GRealObjects = false;
        GOut = nullptr;
        GScripts.clear();
    }

    bool IsThn(const std::string& File)
    {
        if (File.compare(0, 5, "DATA/") != 0 || File.size() < 4)
            return false;
        const std::string Tail = File.substr(File.size() - 4);
        return _stricmp(Tail.c_str(), ".thn") == 0;
    }
}

extern "C" ICOManager* OracleSceneScriptSaveManager()
{
    return &scene_script_save::GManager;
}

void SceneScriptSaveSuite()
{
    scene_script_save::SaveSuite();
}

void RunSceneScriptSaveDataDump(const std::vector<std::string>& Files)
{
    using namespace scene_script_save;
    oracle::Suite List(oracle::GOutputDir + "\\thn_save\\files.txt");
    unsigned int Count = 0;
    scene_script_build::GRealObjects = true;
    for (const std::string& File : Files)
    {
        if (!IsThn(File))
            continue;
        oracle::Bytes Buffer;
        if (!oracle::ReadWholeFile(File, Buffer))
        {
            fprintf(stderr, "thn_save: cannot read %s\n", File.c_str());
            oracle::GFailed = true;
            continue;
        }
        List.Line(File);
        ++Count;
        oracle::Suite Out(oracle::DataOutputFile("thn_save", File));
        GOut = &Out;
        GHead = "{";
        scene_script_build::OpenModule();
        FScriptEngine* Engine = new FScriptEngine();
        FContext Context(1);
        Engine->Slot40SetCom(&Context);
        GOut = nullptr;
        FSaveFileSystem Source(Buffer.data(), Buffer.size());
        const GENRESULT Load = Engine->Slot4Load(static_cast<IFileSystem*>(&Source));
        Out.Line("{\"file\":\"" + File + Format("\",\"load\":\"%d\",\"entities\":\"%lu\",\"events\":\"%lu\",\"duration\":\"%d\"}",
                                                Load, Engine->Slot6EntityCount(), Engine->Slot14EventCount(),
                                                Engine->Slot17GetDuration()));
        GOut = &Out;
        GWriteResults = false;
        FSaveFileSystem Writer(false);
        const GENRESULT Save = Load < 0 ? GR_GENERIC : Engine->Slot5Save(static_cast<IFileSystem*>(&Writer));
        GWriteResults = true;
        Out.Line(Format("{\"save\":\"%d\"}", Save));
        GOut = nullptr;
        Engine->Release();
        scene_script_build::CloseModule();
    }
    scene_script_build::GRealObjects = false;
    GScripts.clear();
    printf("saved %u scene scripts\n", Count);
}
