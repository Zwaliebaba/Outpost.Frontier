// Oracle suites of work package P1.3: the UTF container and the file systems
// of ReadFile.dll (FDosFileSystem, FUtfFileSystem, FMemFileSystem,
// FSearchPathInstance). The Rust test crates/neuron-core/tests/oracle_vfs.rs
// writes the same lines; the inputs come from
// crates/neuron-core/tests/fixtures/oracle/make_vfs_fixtures.py.
//
// ReadFile.dll exports nothing but DllMain, so its source is compiled into
// this file: JoinRoot and MatchUtfWildcard are called directly, and DllMain
// registers the DOS, UTF, MEM and SearchPath factories with the component
// manager exactly as the DLL does when it loads. Every file system below is
// then created through those factories.

#include "oracle_support.h"

#include "../ReadFile/ReadFile.cpp"

using namespace oracle;

namespace
{
    // Reads larger than this are not attempted (none of the fixtures needs one).
    const unsigned long ReadLimit = 0x10000;

    // The script line being executed, reported if a step raises an exception.
    std::string GVfsLine;

    void RegisterReadFile()
    {
        static bool bRegistered = false;
        if (bRegistered)
            return;
        bRegistered = true;
        DllMain(GetModuleHandleA(nullptr), DLL_PROCESS_ATTACH, nullptr);
    }

    std::vector<std::string> Tokens(const std::string& Text)
    {
        std::vector<std::string> Out;
        size_t Start = 0;
        for (;;)
        {
            const size_t Space = Text.find(' ', Start);
            Out.push_back(Text.substr(Start, Space == std::string::npos ? std::string::npos : Space - Start));
            if (Space == std::string::npos)
                return Out;
            Start = Space + 1;
        }
    }

    // A hex argument; "-" is empty.
    Bytes Arg(const std::string& Token)
    {
        return Token == "-" ? Unhex("") : Unhex(Token);
    }

    unsigned long Number(const std::string& Token)
    {
        return strtoul(Token.c_str(), nullptr, 10);
    }

    // The 0x34-byte descriptor callers build (FillFileSystemDesc).
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

    // A UTF factory object, as DllMain creates one: its own path is "\", so
    // CreateInstance opens a new archive. Called directly so that a file
    // that is not an archive fails instead of falling through to the DOS
    // factory.
    IFileSystem* UtfFactory()
    {
        static FUtfFileSystem* Factory = new FUtfFileSystem();
        return Factory;
    }

    const char* KindName(IFileSystem* Fs)
    {
        if (dynamic_cast<FUtfFileSystem*>(Fs) != nullptr)
            return "utf";
        if (FDosFileSystem* Dos = dynamic_cast<FDosFileSystem*>(Fs))
        {
            if (Dos->bFileInstance == 0)
                return "dos-dir";
            return Dos->NestedChild != nullptr ? "nested" : "dos-file";
        }
        if (dynamic_cast<FMemFileSystem*>(Fs) != nullptr)
            return "mem";
        return "unknown";
    }

    // Sets the last error to a value the next operation never stores, so an
    // operation that leaves it alone reports 0.
    unsigned long Sentinel(IFileSystem* Fs)
    {
        if (dynamic_cast<FUtfFileSystem*>(Fs) != nullptr)
        {
            unsigned long Got = 0;
            Fs->Read(0, nullptr, 0, &Got, 0); // error 0x57
            return 0x57;
        }
        if (dynamic_cast<FDosFileSystem*>(Fs) != nullptr)
        {
            Fs->CloseFile(0); // error 6
            return 6;
        }
        return 0; // the memory file system has no error code
    }

    unsigned long ErrorSince(IFileSystem* Fs, unsigned long SentinelValue)
    {
        const unsigned long Error = (unsigned long)Fs->GetFileSystemError();
        return Error == SentinelValue ? 0 : Error;
    }

    std::string ReadFields(IFileSystem* Fs, int Result, unsigned long Got, const std::vector<unsigned char>& Buffer)
    {
        if (Result != 0)
            return Format("\"read\":\"1\",\"got\":\"%lu\",\"rerr\":\"0\",\"data\":\"", Got) +
                   Hex(Buffer.data(), Got) + "\"";
        return Format("\"read\":\"0\",\"got\":\"0\",\"rerr\":\"%lx\",\"data\":\"\"",
                      (unsigned long)Fs->GetFileSystemError());
    }

    // OpenByDesc, a read of the whole file, and one read past its end.
    std::string OpenFields(IFileSystem* Fs, const char* Path)
    {
        const unsigned long Before = Sentinel(Fs);
        FFileSystemDesc Desc = MakeDesc(nullptr, Path);
        const int Handle = Fs->OpenByDesc(&Desc);
        if (Handle == -1)
            return Format("\"ok\":\"0\",\"err\":\"%lx\"", ErrorSince(Fs, Before));
        const unsigned long Size = Fs->GetSize((unsigned long)Handle, nullptr);
        std::string Read;
        if (Size > ReadLimit)
            Read = "\"read\":\"-\"";
        else
        {
            std::vector<unsigned char> Buffer(Size + 1);
            unsigned long Got = 0;
            const int Result = Fs->Read((unsigned long)Handle, Buffer.data(), Size, &Got, 0);
            Read = ReadFields(Fs, Result, Got, Buffer);
        }
        unsigned char Probe[4];
        unsigned long ProbeGot = 0;
        const int ProbeResult = Fs->Read((unsigned long)Handle, Probe, 4, &ProbeGot, 0);
        const unsigned long ProbeError = ProbeResult != 0 ? 0 : (unsigned long)Fs->GetFileSystemError();
        Fs->CloseFile((unsigned long)Handle);
        return Format("\"ok\":\"1\",\"fsize\":\"%08lx\",", Size) + Read +
               Format(",\"eof\":\"%d,%lu,%lx\"", ProbeResult != 0 ? 1 : 0, ProbeResult != 0 ? ProbeGot : 0,
                      ProbeError);
    }

    // GetSize(0) and a read of that many bytes from handle 0.
    std::string SelfFields(IFileSystem* Fs)
    {
        const unsigned long Size = Fs->GetSize(0, nullptr);
        if (Size == 0xFFFFFFFF)
            return Format("\"size\":\"ffffffff\",\"serr\":\"%lx\"", (unsigned long)Fs->GetFileSystemError());
        if (Size > ReadLimit)
            return Format("\"size\":\"%08lx\",\"read\":\"-\"", Size);
        std::vector<unsigned char> Buffer(Size + 1);
        unsigned long Got = 0;
        const int Result = Fs->Read(0, Buffer.data(), Size, &Got, 0);
        return Format("\"size\":\"%08lx\",", Size) + ReadFields(Fs, Result, Got, Buffer);
    }

    // --- path joins, wildcard matching, DOS times --------------------------------------

    void VfsJoin()
    {
        Suite Out("vfs_join.jsonl");
        for (const std::string& Line : InputLines("vfs/join.list"))
        {
            const size_t Second = Line.find(':', Line.find(':') + 1);
            const std::string RootHex = Line.substr(0, Line.find(':'));
            const std::string RelHex = Line.substr(Line.find(':') + 1, Second - Line.find(':') - 1);
            const long Chars = (long)Number(Line.substr(Second + 1));
            const Bytes Root = Unhex(RootHex);
            const Bytes Rel = Unhex(RelHex);

            char Joined[0x400] = {};
            const int Ok = JoinRoot(CStr(Root), CStr(Rel), Joined, Chars);

            // The memory file system's ResolvePath, on one rooted at Root.
            unsigned char Raw[0x40];
            memset(Raw, 0, sizeof(Raw));
            FFileSystemDesc* Desc = (FFileSystemDesc*)Raw;
            Desc->Size = 0x40;
            Desc->ComponentName = "FileSystem";
            Desc->Implementation = "MEM";
            Desc->Path = CStr(Root);
            Desc->Access = 0x80000000;
            Desc->Creation = 3;
            IFileSystem* Mem = nullptr;
            char Resolved[0x400] = {};
            int MemOk = -1;
            if (NEURONCOMPONENTS_Acquire()->CreateInstance((AGGDESC*)Desc, (void**)&Mem) == GR_OK && Mem != nullptr)
            {
                MemOk = Mem->ResolvePath(Resolved, CStr(Rel), (unsigned long)Chars);
                Mem->Release();
            }
            Out.Line("{\"root\":\"" + RootHex + "\",\"rel\":\"" + RelHex + "\",\"n\":\"" + Format("%ld", Chars) +
                     "\",\"ok\":\"" + Format("%d", Ok) + "\",\"out\":\"" + HexC(Joined) + "\",\"mem_ok\":\"" +
                     Format("%d", MemOk) + "\",\"mem_out\":\"" + HexC(Resolved) + "\"}");
        }
    }

    void VfsMatch()
    {
        Suite Out("vfs_match.jsonl");
        for (const std::string& Line : InputLines("vfs/match.hex"))
        {
            const size_t Colon = Line.find(':');
            const std::string NameHex = Line.substr(0, Colon);
            const std::string PatternHex = Line.substr(Colon + 1);
            const Bytes Name = Unhex(NameHex);
            const Bytes Pattern = Unhex(PatternHex);
            const int Matched = MatchUtfWildcard(CStr(Name), CStr(Pattern));
            Out.Line("{\"name\":\"" + NameHex + "\",\"pattern\":\"" + PatternHex + "\",\"match\":\"" +
                     (Matched != 0 ? "1" : "0") + "\"}");
        }
        unsigned char Folds[256];
        for (int Byte = 0; Byte < 256; ++Byte)
            Folds[Byte] = (unsigned char)FoldUtfWildChar((char)Byte);
        Out.Line("{\"fold\":\"" + Hex(Folds, sizeof(Folds)) + "\"}");
    }

    void VfsDosTime()
    {
        Suite Out("vfs_dostime.jsonl");
        for (const std::string& Line : InputLines("vfs/dostime.list"))
        {
            const size_t Colon = Line.find(':');
            const WORD Date = (WORD)strtoul(Line.substr(0, Colon).c_str(), nullptr, 16);
            const WORD Time = (WORD)strtoul(Line.substr(Colon + 1).c_str(), nullptr, 16);
            FILETIME Ft = {};
            const BOOL Ok = DosDateTimeToFileTime(Date, Time, &Ft);
            const unsigned long long Value = ((unsigned long long)Ft.dwHighDateTime << 32) | Ft.dwLowDateTime;
            Out.Line(Format("{\"date\":\"%s\",\"time\":\"%s\",\"ok\":\"%d\",\"ft\":\"%016llx\"}",
                            Line.substr(0, Colon).c_str(), Line.substr(Colon + 1).c_str(), Ok ? 1 : 0, Value));
        }
    }

    // --- every node of each archive ----------------------------------------------------

    unsigned long long FileTimeValue(const FILETIME& Time)
    {
        return ((unsigned long long)Time.dwHighDateTime << 32) | Time.dwLowDateTime;
    }

    void DumpTree(IFileSystem* Fs, const std::string& Prefix, int Depth, bool bFull, Suite& Out)
    {
        const std::string Pattern = Prefix + "*";
        std::vector<WIN32_FIND_DATAA> Entries;
        WIN32_FIND_DATAA Data;
        memset(&Data, 0, sizeof(Data));
        const int Find = Fs->FindFirst(Pattern.c_str(), &Data);
        if (Find == -1)
        {
            Out.Line("{\"find\":\"" + HexC(Pattern.c_str()) +
                     Format("\",\"err\":\"%lx\"}", (unsigned long)Fs->GetFileSystemError()));
            return;
        }
        for (;;)
        {
            Entries.push_back(Data);
            memset(&Data, 0, sizeof(Data));
            if (Fs->FindNext((unsigned long)Find, &Data) == 0)
                break;
        }
        Fs->FindClose((unsigned long)Find);

        for (const WIN32_FIND_DATAA& Entry : Entries)
        {
            const std::string Path = Prefix + Entry.cFileName;
            std::string Times = "-";
            if (bFull)
                Times = Format("%016llx,%016llx,%016llx", FileTimeValue(Entry.ftCreationTime),
                               FileTimeValue(Entry.ftLastAccessTime), FileTimeValue(Entry.ftLastWriteTime));
            const unsigned long Before = Sentinel(Fs);
            const unsigned long Attributes = (unsigned long)Fs->GetPathAttributes(Path.c_str());
            const unsigned long AttributesError = ErrorSince(Fs, Before);
            Out.Line("{\"path\":\"" + HexC(Path.c_str()) +
                     Format("\",\"attr\":\"%08lx\",\"size\":\"%08lx\",\"times\":\"%s\",\"pattr\":\"%08lx\","
                            "\"perr\":\"%lx\",",
                            Entry.dwFileAttributes, Entry.nFileSizeLow, Times.c_str(), Attributes, AttributesError) +
                     OpenFields(Fs, Path.c_str()) + "}");
            if ((Entry.dwFileAttributes & 0x10) != 0 && Depth < 6)
                DumpTree(Fs, Path + "\\", Depth + 1, bFull, Out);
        }
    }

    void UtfTree()
    {
        Suite Out("utf_tree.jsonl");
        for (const std::string& Archive : InputLines("vfs/utf.list"))
        {
            FFileSystemDesc Desc = MakeDesc("UTF", Archive.c_str());
            IFileSystem* Fs = nullptr;
            const GENRESULT Result = UtfFactory()->CreateInstance((AGGDESC*)&Desc, (void**)&Fs);
            Out.Line(Format("{\"archive\":\"%s\",\"result\":\"%ld\"}", Archive.c_str(), Result));
            if (Result != GR_OK || Fs == nullptr)
                continue;
            FUtfFileSystem* Utf = dynamic_cast<FUtfFileSystem*>(Fs);
            const bool bFull = ReadUtfDword(Utf->ActiveWalker()->GetView(), 0x14) == 0x2C;
            DumpTree(Fs, "", 0, bFull, Out);
            Fs->Release();
        }
    }

    // --- operation scripts -------------------------------------------------------------

    class FScript
    {
    public:
        explicit FScript(const char* Name) : Out(Name), Search(nullptr) {}

        ~FScript()
        {
            ReleaseAll();
            if (Search != nullptr)
                Search->Release();
        }

        void Run(const std::string& Line)
        {
            const size_t Space = Line.find(' ');
            const std::string Op = Line.substr(0, Space);
            const std::string Rest = Space == std::string::npos ? std::string() : Line.substr(Space + 1);
            const std::vector<std::string> T = Tokens(Rest);
            GVfsLine = Line;
            const std::string Fields = Execute(Op, T);
            Out.Line("{\"op\":\"" + Op + "\",\"arg\":\"" + Rest + "\"" + (Fields.empty() ? "" : ",") + Fields + "}");
        }

    private:
        IFileSystem* Top() const
        {
            return Stack.back();
        }

        void ReleaseAll()
        {
            for (IFileSystem* Fs : Stack)
                Fs->Release();
            Stack.clear();
        }

        // The result and kind of a creation; keeps what was created.
        std::string Created(GENRESULT Result, IFileSystem* Fs)
        {
            if (Result != GR_OK || Fs == nullptr)
            {
                if (Fs != nullptr)
                    Fs->Release();
                return Format("\"result\":\"%ld\"", Result);
            }
            const char* Kind = KindName(Fs);
            if (strcmp(Kind, "nested") == 0)
                Fs->Release();
            else
                Stack.push_back(Fs);
            return std::string("\"result\":\"0\",\"kind\":\"") + Kind + "\"";
        }

        std::string Execute(const std::string& Op, const std::vector<std::string>& T)
        {
            if (Op == "archive")
            {
                ReleaseAll();
                const Bytes Path = Arg(T[0]);
                FFileSystemDesc Desc = MakeDesc("UTF", CStr(Path));
                IFileSystem* Fs = nullptr;
                const GENRESULT Result = UtfFactory()->CreateInstance((AGGDESC*)&Desc, (void**)&Fs);
                return Created(Result, Fs);
            }
            if (Op == "dos")
            {
                ReleaseAll();
                const Bytes Implementation = Arg(T[0]);
                const Bytes Path = Arg(T[1]);
                FFileSystemDesc Desc = MakeDesc(T[0] == "-" ? nullptr : CStr(Implementation), CStr(Path));
                IFileSystem* Fs = nullptr;
                const GENRESULT Result = NEURONCOMPONENTS_Acquire()->CreateInstance((AGGDESC*)&Desc, (void**)&Fs);
                return Created(Result, Fs);
            }
            if (Op == "child" || Op == "clone")
            {
                const std::string ImplementationToken = Op == "child" ? T[0] : std::string("-");
                const Bytes Implementation = Arg(ImplementationToken);
                const Bytes Path = Arg(Op == "child" ? T[1] : T[0]);
                FFileSystemDesc Desc =
                    MakeDesc(ImplementationToken == "-" ? nullptr : CStr(Implementation), CStr(Path));
                IFileSystem* Fs = nullptr;
                const GENRESULT Result = Top()->CreateInstance((AGGDESC*)&Desc, (void**)&Fs);
                return Created(Result, Fs);
            }
            if (Op == "clonefind")
            {
                const Bytes Pattern = Arg(T[0]);
                const unsigned long Index = Number(T[1]);
                WIN32_FIND_DATAA Data;
                memset(&Data, 0, sizeof(Data));
                const int Find = Top()->FindFirst(CStr(Pattern), &Data);
                if (Find == -1)
                    return "\"result\":\"none\"";
                for (unsigned long Step = 0; Step < Index; ++Step)
                {
                    memset(&Data, 0, sizeof(Data));
                    if (Top()->FindNext((unsigned long)Find, &Data) == 0)
                    {
                        Top()->FindClose((unsigned long)Find);
                        return "\"result\":\"none\"";
                    }
                }
                FFileSystemDesc Desc = MakeDesc(nullptr, Data.cFileName);
                Desc.Field30 = (unsigned long)Find;
                IFileSystem* Fs = nullptr;
                IFileSystem* Parent = Top();
                const GENRESULT Result = Parent->CreateInstance((AGGDESC*)&Desc, (void**)&Fs);
                Parent->FindClose((unsigned long)Find);
                const std::string Name = HexC(Data.cFileName);
                return "\"name\":\"" + Name + "\"," + Created(Result, Fs);
            }
            if (Op == "pop")
            {
                Stack.back()->Release();
                Stack.pop_back();
                return std::string();
            }
            if (Op == "self")
                return SelfFields(Top());
            if (Op == "readself")
            {
                const unsigned long Count = Number(T[0]);
                std::vector<unsigned char> Buffer(Count + 1);
                unsigned long Got = 0;
                const int Result = Top()->Read(0, Buffer.data(), Count, &Got, 0);
                return ReadFields(Top(), Result, Got, Buffer);
            }
            if (Op == "seek")
            {
                const int Result = Top()->SeekFile(0, Number(T[1]), nullptr, Number(T[0]));
                return Format("\"result\":\"%d\"", Result);
            }
            if (Op == "map")
            {
                const unsigned long Offset = Number(T[0]);
                const unsigned long Count = Number(T[1]);
                IFileSystem* Fs = Top();
                const int Token = Fs->CreateMapping(0, 0, PAGE_READONLY, 0, 0, 0);
                if (Token == 0 || Token == -1)
                    return Format("\"ok\":\"0\",\"err\":\"%lx\"", (unsigned long)Fs->GetFileSystemError());
                const int View = Fs->MapFileView((unsigned long)Token, FILE_MAP_READ, 0, Offset, Count);
                if (View == 0 || View == -1)
                {
                    const unsigned long Error = (unsigned long)Fs->GetFileSystemError();
                    Fs->CloseFile((unsigned long)Token);
                    return Format("\"ok\":\"0\",\"err\":\"%lx\"", Error);
                }
                const unsigned long Length = Count != 0 ? Count : Fs->GetSize(0, nullptr) - Offset;
                const std::string Data = Hex((const void*)(uintptr_t)(unsigned long)View, Length);
                Fs->CloseFile((unsigned long)Token);
                return "\"ok\":\"1\",\"data\":\"" + Data + "\"";
            }
            if (Op == "syspath")
            {
                char Buffer[0x400] = {};
                Top()->GetFileSysPath(Buffer, Number(T[0]));
                return "\"out\":\"" + HexC(Buffer) + "\"";
            }
            if (Op == "curpath")
            {
                char Buffer[0x400] = {};
                const unsigned long Before = Sentinel(Top());
                const int Result = Top()->GetCurrentPath(Number(T[0]), Buffer);
                if (Result != 0)
                    return "\"ok\":\"1\",\"out\":\"" + HexC(Buffer) + "\"";
                return Format("\"ok\":\"0\",\"err\":\"%lx\"", ErrorSince(Top(), Before));
            }
            if (Op == "attr")
            {
                const Bytes Path = Arg(T[0]);
                const unsigned long Before = Sentinel(Top());
                const unsigned long Value = (unsigned long)Top()->GetPathAttributes(CStr(Path));
                if (Value == 0xFFFFFFFF)
                    return "\"value\":\"ffffffff\",\"err\":\"-\"";
                return Format("\"value\":\"%08lx\",\"err\":\"%lx\"", Value & 0x10, ErrorSince(Top(), Before));
            }
            if (Op == "find")
            {
                const Bytes Pattern = Arg(T[0]);
                IFileSystem* Fs = Top();
                const unsigned long Before = Sentinel(Fs);
                WIN32_FIND_DATAA Data;
                memset(&Data, 0, sizeof(Data));
                const int Find = Fs->FindFirst(CStr(Pattern), &Data);
                if (Find == -1)
                    return Format("\"count\":\"0\",\"err\":\"%lx\"", ErrorSince(Fs, Before));
                unsigned long Count = 0;
                unsigned long Error = 0;
                for (;;)
                {
                    Out.Line("{\"op\":\"entry\",\"name\":\"" + HexC(Data.cFileName) +
                             Format("\",\"attr\":\"%08lx\",\"size\":\"%08lx\"}", Data.dwFileAttributes,
                                    Data.nFileSizeLow));
                    ++Count;
                    memset(&Data, 0, sizeof(Data));
                    if (Fs->FindNext((unsigned long)Find, &Data) == 0)
                    {
                        Error = (unsigned long)Fs->GetFileSystemError();
                        break;
                    }
                }
                Fs->FindClose((unsigned long)Find);
                return Format("\"count\":\"%lu\",\"err\":\"%lx\"", Count, Error);
            }
            if (Op == "open")
            {
                const Bytes Path = Arg(T[0]);
                return OpenFields(Top(), CStr(Path));
            }
            if (Op == "resolve")
            {
                const Bytes Source = Arg(T[0]);
                char Buffer[0x400] = {};
                const int Result = Top()->ResolvePath(Buffer, CStr(Source), Number(T[1]));
                return Format("\"ok\":\"%d\",\"out\":\"", Result) + HexC(Buffer) + "\"";
            }
            if (Op == "node")
            {
                const Bytes Name = Arg(T[0]);
                const unsigned long Before = Sentinel(Top());
                const int Result = Top()->OpenNode(CStr(Name));
                return Format("\"ok\":\"%d\",\"err\":\"%lx\"", Result != 0 ? 1 : 0, ErrorSince(Top(), Before));
            }
            if (Op == "search")
                return SetSearch(Arg(T[0]));
            if (Op == "getpath")
            {
                char Buffer[0x400] = {};
                const int Result = Search->GetPath(Buffer, Number(T[0]));
                return Format("\"ret\":\"%d\",\"out\":\"", Result) + HexC(Buffer) + "\"";
            }
            if (Op == "sfind")
            {
                const Bytes Implementation = Arg(T[0]);
                const Bytes Path = Arg(T[1]);
                FFileSystemDesc Desc = MakeDesc(T[0] == "-" ? nullptr : CStr(Implementation), CStr(Path));
                IFileSystem* Fs = nullptr;
                const GENRESULT Result = Search->CreateInstance((AGGDESC*)&Desc, (void**)&Fs);
                if (Result != GR_OK || Fs == nullptr)
                    return Format("\"result\":\"%ld\"", Result);
                const char* Kind = KindName(Fs);
                std::string Fields = std::string("\"result\":\"0\",\"kind\":\"") + Kind + "\"";
                if (strcmp(Kind, "nested") != 0)
                {
                    char Buffer[0x400] = {};
                    Fs->GetFileSysPath(Buffer, 0x104);
                    Fields += ",\"path\":\"" + HexC(Buffer) + "\"";
                    if (strcmp(Kind, "dos-file") == 0)
                        Fields += "," + SelfFields(Fs);
                }
                Fs->Release();
                return Fields;
            }
            if (Op == "mem")
                return CreateMem(T);
            if (Op == "state")
            {
                IFileSystem* Fs = Top();
                unsigned long High = 0;
                const unsigned long Size = Fs->GetSize(0, &High);
                const unsigned long Position = (unsigned long)Fs->GetFilePosition(0, &High);
                IDataBuffer* Data = nullptr;
                void* Buffer = nullptr;
                unsigned long Allocated = 0;
                if (Fs->QueryInterface(Iids::DataBuffer, (void**)&Data) == GR_OK && Data != nullptr)
                {
                    Data->GetBuffer(&Buffer, &Allocated);
                    Data->Release();
                }
                unsigned long Fnv = 0x811C9DC5u;
                const unsigned char* Raw = (const unsigned char*)Buffer;
                for (unsigned long Index = 0; Index < Allocated; ++Index)
                    Fnv = (Fnv ^ Raw[Index]) * 0x01000193u;
                return Format("\"size\":\"%08lx\",\"pos\":\"%lu\",\"alloc\":\"%lu\",\"fnv\":\"%08lx\",\"head\":\"",
                              Size, Position, Allocated, Fnv) +
                       Hex(Buffer, Allocated < 32 ? Allocated : 32) + "\"";
            }
            if (Op == "write" || Op == "writeat")
            {
                const bool bAt = Op == "writeat";
                const Bytes Data = Arg(bAt ? T[1] : T[0]);
                OVERLAPPED Position = {};
                if (bAt)
                    Position.Offset = Number(T[0]);
                unsigned long Written = 0;
                const int Result =
                    Top()->Write(0, Data.data(), (unsigned long)(Data.size() - 1), &Written, bAt ? &Position : nullptr);
                return Format("\"ok\":\"%d\",\"written\":\"%lu\"", Result != 0 ? 1 : 0, Result != 0 ? Written : 0);
            }
            if (Op == "read" || Op == "readat")
            {
                const bool bAt = Op == "readat";
                const unsigned long Count = Number(bAt ? T[1] : T[0]);
                OVERLAPPED Position = {};
                if (bAt)
                    Position.Offset = Number(T[0]);
                std::vector<unsigned char> Buffer(Count + 1);
                unsigned long Got = 0;
                const int Result = Top()->Read(0, Buffer.data(), Count, &Got,
                                               bAt ? (unsigned long)(uintptr_t)&Position : 0);
                return Format("\"ok\":\"%d\",\"got\":\"%lu\",\"data\":\"", Result != 0 ? 1 : 0, Got) +
                       Hex(Buffer.data(), Got) + "\"";
            }
            if (Op == "setend")
                return Format("\"ok\":\"%d\"", Top()->SetEndOfFile(0) != 0 ? 1 : 0);
            fprintf(stderr, "unknown operation %s\n", Op.c_str());
            GFailed = true;
            return std::string();
        }

        std::string SetSearch(const Bytes& List)
        {
            if (Search != nullptr)
                Search->Release();
            Search = nullptr;
            AGGDESC Desc = {8, "SearchPath"};
            if (NEURONCOMPONENTS_Acquire()->CreateInstance(&Desc, (void**)&Search) != GR_OK || Search == nullptr)
            {
                fprintf(stderr, "no search path\n");
                GFailed = true;
                return std::string();
            }
            Search->SetPath(CStr(List));
            FSearchPathInstance* Instance = static_cast<FSearchPathInstance*>(Search);
            std::string Roots;
            for (int Index = 0; Index < Instance->ChildCount; ++Index)
            {
                if (Index != 0)
                    Roots += ",";
                IFileSystem* Child = Instance->Children[(size_t)Index];
                if (Child == nullptr)
                {
                    Roots += "-";
                    continue;
                }
                char Buffer[0x400] = {};
                Child->GetFileSysPath(Buffer, 0x104);
                Roots += std::string(KindName(Child)) + ":" + HexC(Buffer);
            }
            return "\"roots\":\"" + Roots + "\"";
        }

        // mem <access hex> <creation> <copy|wrap> <data hex> <path hex>
        std::string CreateMem(const std::vector<std::string>& T)
        {
            ReleaseAll();
            const Bytes Data = Arg(T[3]);
            const Bytes Path = Arg(T[4]);
            const unsigned long Size = (unsigned long)(Data.size() - 1);
            unsigned char Raw[0x40];
            memset(Raw, 0, sizeof(Raw));
            FFileSystemDesc* Desc = (FFileSystemDesc*)Raw;
            Desc->Size = 0x40;
            Desc->ComponentName = "FileSystem";
            Desc->Implementation = "MEM";
            Desc->Path = T[4] == "-" ? nullptr : CStr(Path);
            Desc->Access = strtoul(T[0].c_str(), nullptr, 16);
            Desc->Creation = Number(T[1]);
            Desc->Field30 = 0xFFFFFFFF;
            if (T[2] == "wrap")
            {
                // The file system keeps this buffer and never frees it (flag bit 1 clear).
                std::vector<unsigned char>* Kept = new std::vector<unsigned char>(Data.begin(), Data.end());
                Desc->MemBuffer = Size != 0 ? Kept->data() : nullptr;
                Desc->MemSize = Size;
                Desc->MemFlags = 1;
            }
            else
            {
                Desc->MemBuffer = Size != 0 ? (void*)Data.data() : nullptr;
                Desc->MemSize = Size;
                Desc->MemFlags = 0;
            }
            IFileSystem* Fs = nullptr;
            const GENRESULT Result = NEURONCOMPONENTS_Acquire()->CreateInstance((AGGDESC*)Desc, (void**)&Fs);
            if (Result == GR_OK && Fs != nullptr)
                Stack.push_back(Fs);
            return Format("\"result\":\"%ld\"", Result);
        }

        Suite Out;
        std::vector<IFileSystem*> Stack;
        ISearchPath* Search;
    };

    void RunScript(const char* Input, const char* Name)
    {
        FScript Script(Name);
        for (const std::string& Line : InputLines(Input))
            Script.Run(Line);
    }

    void UtfOps()
    {
        RunScript("vfs/utf_ops.txt", "utf_ops.jsonl");
    }

    void DosOps()
    {
        RunScript("vfs/dos_ops.txt", "vfs_dos.jsonl");
    }

    void MemOps()
    {
        RunScript("vfs/mem_ops.txt", "vfs_mem.jsonl");
    }

    // Runs one step under SEH; no C++ objects live in this frame. Returns the
    // exception code, or 0.
    DWORD GuardedStep(void (*Step)())
    {
        __try
        {
            Step();
            return 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return GetExceptionCode();
        }
    }

    void Step(const char* Name, void (*Fn)())
    {
        GVfsLine.clear();
        const DWORD Code = GuardedStep(Fn);
        if (Code != 0)
        {
            fprintf(stderr, "vfs: exception %08lx in %s, last script line: %s\n", (unsigned long)Code, Name,
                    GVfsLine.c_str());
            GFailed = true;
        }
    }
}

void RunVfsSuites()
{
    Step("RegisterReadFile", RegisterReadFile);
    Step("VfsJoin", VfsJoin);
    Step("VfsMatch", VfsMatch);
    Step("VfsDosTime", VfsDosTime);
    Step("UtfTree", UtfTree);
    Step("utf_ops", UtfOps);
    Step("dos_ops", DosOps);
    Step("mem_ops", MemOps);
}
