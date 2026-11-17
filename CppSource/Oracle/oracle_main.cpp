// Oracle harness (design/migration-plan.md P0.4, design/verification.md).
//
// Reads the synthetic inputs in crates/neuron-core/tests/fixtures/oracle,
// calls the C++ reference through neuroncomponents.dll and Common.dll, and
// writes one JSON object per line. Shared helpers are in oracle_support.h;
// later work packages add their suites in oracle_<module>.cpp. The Rust test
// crates/neuron-core/tests/oracle_vectors.rs renders the same suites; the
// committed vectors must equal this program's output byte for byte.
//
// Usage: Oracle.exe <fixture directory> <output directory>
//        Oracle.exe dump <data directory> <output directory>
//        Oracle.exe catalog <resource DLL directory> <output directory>
// The second form dumps the original installation's INI files for
// crates/neuron-core/tests/data_dump.rs; the third dumps the text
// resources of the resource DLLs for crates/neuron-core/tests/catalog_dump.rs
// (oracle_catalog.cpp). Their output must never be committed.
// All paths must be absolute: the program changes into the input
// directory so that the engine parser and INI_Reader see relative names.

#include <windows.h>
#include <emmintrin.h>

#include <float.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <vector>

#include "oracle_support.h"

#include "neuroncomponents.h"
#include "luaprofile.h"
#include "common_id.h"
#include "common_ini.h"
#include "msvcrt_compat.h"
#include "../Common/msvcrt_atof.h"

int OracleNeuronComponentsStricmp(const char* Left, const char* Right);

using namespace oracle;

namespace
{
    const unsigned long CrcContinueSeed = 0x5A5A5A5Au;
    const unsigned int LocationBase = 0x8ABCDEF0u;

    void Names()
    {
        Suite Out("names.jsonl");
        for (const std::string& Line : InputLines("names.hex"))
        {
            const Bytes Name = Unhex(Line);
            const char First = (char)Name[0];
            Out.Line(Format("{\"in\":\"%s\",\"crc\":\"%08lx\",\"crc_cont\":\"%08lx\",\"crc_byte\":\"%08lx\","
                            "\"create_id\":\"%08x\",\"make_id\":\"%04x\",\"location_id\":\"%08x\"}",
                            Line.c_str(), NEURONCOMPONENTS_CRC::GetCRC32(CStr(Name)),
                            NEURONCOMPONENTS_CRC::GetContinuedCRC32(CrcContinueSeed, CStr(Name)),
                            NEURONCOMPONENTS_CRC::GetContinuedCRC32(CrcContinueSeed, First), CreateID(CStr(Name)),
                            (unsigned int)MakeId(CStr(Name)), MakeLocationID(LocationBase, CStr(Name))));
        }
    }

    void StricmpPairs()
    {
        Suite Out("stricmp.jsonl");
        for (const std::string& Line : InputLines("stricmp.hex"))
        {
            const size_t Colon = Line.find(':');
            const std::string Left = Line.substr(0, Colon);
            const std::string Right = Colon == std::string::npos ? std::string() : Line.substr(Colon + 1);
            const Bytes A = Unhex(Left);
            const Bytes B = Unhex(Right);
            Out.Line(Format("{\"a\":\"%s\",\"b\":\"%s\",\"stricmp\":\"%d\",\"crt\":\"%d\"}", Left.c_str(),
                            Right.c_str(), Sign(OracleNeuronComponentsStricmp(CStr(A), CStr(B))),
                            Sign(NEURONCOMPONENTS_CRC::CompareStringsI(CStr(A), CStr(B)))));
        }
    }

    void Numbers()
    {
        Suite Out("numbers.jsonl");
        INI_Reader Reader;
        for (const std::string& Line : InputLines("numbers.hex"))
        {
            const Bytes Text = Unhex(Line);
            GDumps = 0;
            const double Parsed = ParseNumber(CStr(Text));
            const int ParseDumps = GDumps;
            Out.Line(Format("{\"in\":\"%s\",\"atoi\":\"%08x\",\"atof\":\"%016llx\",\"parse\":\"%016llx\","
                            "\"parse_dumps\":\"%d\",\"is_number\":\"%d\"}",
                            Line.c_str(), (unsigned int)MsvcrtAtoi(CStr(Text)), DoubleBits(MsvcrtAtof(CStr(Text))),
                            DoubleBits(Parsed), ParseDumps, Reader.is_number(CStr(Text)) ? 1 : 0));
        }
    }

    void Fields()
    {
        Suite Out("fields.jsonl");
        for (const std::string& Line : InputLines("fields.hex"))
        {
            const Bytes Text = Unhex(Line);
            std::string Parts[4];
            for (unsigned int Index = 0; Index < 4; ++Index)
                Parts[Index] = HexC(StringIndex(CStr(Text), Index));
            Out.Line(Format("{\"in\":\"%s\",\"f0\":\"%s\",\"f1\":\"%s\",\"f2\":\"%s\",\"f3\":\"%s\","
                            "\"value_num1\":\"%016llx\"}",
                            Line.c_str(), Parts[0].c_str(), Parts[1].c_str(),
                            Parts[2].c_str(), Parts[3].c_str(), DoubleBits(ValueNum(CStr(Text), 1))));
        }
    }

    void Wide()
    {
        Suite Out("wide.jsonl");
        for (const std::string& Line : InputLines("wide.hex"))
        {
            std::vector<wchar_t> Units;
            for (size_t Index = 0; Index + 3 < Line.size(); Index += 4)
                Units.push_back((wchar_t)(HexValue(Line[Index]) << 12 | HexValue(Line[Index + 1]) << 8 |
                                          HexValue(Line[Index + 2]) << 4 | HexValue(Line[Index + 3])));
            Units.push_back(0);
            Out.Line(Format("{\"in\":\"%s\",\"wtoi\":\"%08x\"}", Line.c_str(), (unsigned int)MsvcrtWtoi(Units.data())));
        }
    }

    void Doubles()
    {
        Suite Out("doubles.jsonl");
        for (const std::string& Line : InputLines("doubles.hex"))
        {
            const unsigned long long Bits = _strtoui64(Line.c_str(), nullptr, 16);
            double Value;
            memcpy(&Value, &Bits, sizeof(Value));
            char G[4][64];
            const int Precisions[4] = {1, 6, 9, 17};
            for (int Index = 0; Index < 4; ++Index)
                sprintf_s(G[Index], sizeof(G[Index]), "%.*g", Precisions[Index], Value);
            // The conversions as the reference compiles them (/arch:IA32).
            const int Trunc32 = (int)Value;
            const long long Trunc64 = (long long)Value;
            const int Via64 = (int)(long long)Value;
            const int Round32 = _mm_cvtsd_si32(_mm_set_sd(Value));
            Out.Line(Format("{\"bits\":\"%s\",\"g1\":\"%s\",\"g6\":\"%s\",\"g9\":\"%s\",\"g17\":\"%s\","
                            "\"trunc32\":\"%08x\",\"trunc64\":\"%016llx\",\"via64\":\"%08x\",\"round32\":\"%08x\"}",
                            Line.c_str(), G[0], G[1], G[2], G[3], (unsigned int)Trunc32,
                            (unsigned long long)Trunc64, (unsigned int)Via64, (unsigned int)Round32));
        }
    }

    void Rand()
    {
        Suite Out("rand.jsonl");
        for (const std::string& Line : InputLines("rand.hex"))
        {
            srand((unsigned int)strtoul(Line.c_str(), nullptr, 16));
            std::string Values;
            for (int Index = 0; Index < 8; ++Index)
            {
                if (Index > 0)
                    Values += ",";
                Values += Format("%d", rand());
            }
            Out.Line(Format("{\"seed\":\"%s\",\"rand\":\"%s\"}", Line.c_str(), Values.c_str()));
        }
    }

    // --- Engine INI ------------------------------------------------------------------

    std::vector<std::string> GSectionNames;

    int NEURONCOMPONENTS_API CollectSection(void* /*Parser*/, const char* Name, void* /*Context*/)
    {
        GSectionNames.push_back(Name == nullptr ? std::string() : std::string(Name));
        return 1;
    }

    DWORD GExceptionCode = 0;

    int ExceptionFilter(DWORD Code)
    {
        GExceptionCode = Code;
        if (Code == EXCEPTION_BREAKPOINT || Code == EXCEPTION_ACCESS_VIOLATION)
            return EXCEPTION_EXECUTE_HANDLER;
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // 0 loaded, 1 GR_GENERIC, 2 breakpoint, 3 access violation. No C++
    // objects here: __try cannot share a frame with object unwinding.
    int GuardedLoad(IProfileParser* Parser, const char* Path)
    {
        __try
        {
            return Parser->LoadFile(Path, 0) == GR_OK ? 0 : 1;
        }
        __except (ExceptionFilter(GetExceptionCode()))
        {
            return GExceptionCode == EXCEPTION_BREAKPOINT ? 2 : 3;
        }
    }

    IProfileParser* AcquireParser()
    {
        IProfileParser* Parser = nullptr;
        if (NEURONCOMPONENTS_Acquire()->QueryInterface(Iids::ProfileParser, (void**)(&Parser)) < 0 ||
            Parser == nullptr)
        {
            fprintf(stderr, "no profile parser\n");
            GFailed = true;
            return nullptr;
        }
        return Parser;
    }

    // The parser's result and database for one file.
    void DumpEngineFile(IProfileParser* Parser, const std::string& File, Suite& Out)
    {
        static const char* const Results[] = {"ok", "generic", "breakpoint", "fault"};
        {
            const int Result = GuardedLoad(Parser, File.c_str());
            Out.Line(Format("{\"file\":\"%s\",\"result\":\"%s\"}", File.c_str(), Results[Result]));
            GSectionNames.clear();
            Parser->EnumerateSections(CollectSection, nullptr);
            for (const std::string& Name : GSectionNames)
            {
                Out.Line("{\"section\":\"" + HexC(Name.c_str()) + "\"}");
                void* Section = Parser->OpenSection(Name.c_str(), 3);
                if (Section == nullptr)
                    continue;
                static char LineText[0x1000];
                static char Value[0x1000];
                for (int Index = 0; Parser->ReadLine(Section, Index, LineText, sizeof(LineText)) > 0; ++Index)
                {
                    std::string Key = LineText;
                    const size_t Equals = Key.find(" = ");
                    if (Equals != std::string::npos)
                        Key.resize(Equals);
                    Value[0] = '\0';
                    Parser->GetString(Section, Key.c_str(), Value, sizeof(Value));
                    Out.Line("{\"line\":\"" + HexC(LineText) + "\",\"value\":\"" + HexC(Value) + "\"}");
                }
                Parser->CloseSection(Section);
            }
        }
    }

    void EngineIni()
    {
        Suite Out("engine_ini.jsonl");
        IProfileParser* Parser = AcquireParser();
        if (Parser == nullptr)
            return;
        for (const std::string& File : InputLines("engine.list"))
            DumpEngineFile(Parser, File, Out);
    }

    // --- INI_Reader --------------------------------------------------------------------

    std::string WideHex(const wchar_t* Text)
    {
        std::string Out;
        for (; Text != nullptr && *Text != 0; ++Text)
            Out += Format("%04x", (unsigned int)(unsigned short)*Text);
        return Out;
    }

    void DumpReader(INI_Reader& Reader, bool bExtraParam, Suite& Out)
    {
        std::vector<std::string> Headers;
        while (Reader.read_header())
        {
            const std::string Header = Reader.get_header_ptr();
            Out.Line("{\"header\":\"" + HexC(Header.c_str()) + "\"}");
            Headers.push_back(Header);
            while (Reader.read_value())
            {
                GDumps = 0;
                const std::string Name = HexC(Reader.get_name_ptr());
                const std::string Line = HexC(Reader.get_line_ptr());
                const unsigned int Count = Reader.get_num_parameters();
                const std::string Whole = HexC(Reader.get_value_string());
                const std::string WideText = WideHex(Reader.get_value_wstring());
                const int LineNum = Reader.get_line_num();
                const unsigned int Tell = Reader.tell();
                const bool bEnd = Reader.is_end();
                Out.Line("{\"name\":\"" + Name + "\",\"line\":\"" + Line + "\",\"n\":\"" + Format("%u", Count) +
                         "\",\"str\":\"" + Whole + "\",\"wide\":\"" + WideText + "\",\"line_num\":\"" +
                         Format("%d", LineNum) + "\",\"tell\":\"" + Format("%u", Tell) + "\",\"end\":\"" +
                         (bEnd ? "1" : "0") + "\",\"dumps\":\"" + Format("%d", GDumps) + "\"}");
                const unsigned int Limit = bExtraParam ? Count + 1 : Count;
                for (unsigned int Index = 0; Index < Limit; ++Index)
                {
                    GDumps = 0;
                    const std::string S = HexC(Reader.get_value_string(Index));
                    const int I = Reader.get_value_int(Index);
                    const double F = Reader.get_value_float(Index);
                    const bool B = Reader.get_value_bool(Index);
                    const bool E = Reader.is_value_empty(Index);
                    Out.Line(Format("{\"param\":\"%u\",\"s\":\"", Index) + S +
                             Format("\",\"i\":\"%08x\",\"f\":\"%016llx\",\"b\":\"%d\",\"e\":\"%d\",\"dumps\":\"%d\"}",
                                    (unsigned int)I, DoubleBits(F), B ? 1 : 0, E ? 1 : 0, GDumps));
                }
            }
        }
        for (const std::string& Header : Headers)
        {
            std::string Query = Header;
            for (char& Ch : Query)
            {
                if (Ch >= 'a' && Ch <= 'z')
                    Ch = (char)(Ch - 'a' + 'A');
            }
            const bool bFound = Reader.find_header(Query.c_str());
            std::string First;
            if (bFound && Reader.read_value())
                First = Reader.get_name_ptr();
            Out.Line("{\"find\":\"" + HexC(Query.c_str()) + "\",\"found\":\"" + (bFound ? "1" : "0") +
                     "\",\"first\":\"" + HexC(First.c_str()) + "\"}");
        }
    }

    void IniReader()
    {
        Suite Out("ini_reader.jsonl");
        for (const std::string& File : InputLines("reader.list"))
        {
            Bytes Contents;
            if (!ReadWholeFile(File, Contents))
            {
                fprintf(stderr, "cannot read %s\n", File.c_str());
                GFailed = true;
                continue;
            }
            for (int Mode = 0; Mode < 2; ++Mode)
            {
                INI_Reader* Reader = new INI_Reader();
                const bool bOpen = Mode == 0 ? Reader->open(File.c_str(), false)
                                             : Reader->open_memory(Contents.empty() ? nullptr : (const char*)Contents.data(),
                                                                   (unsigned int)Contents.size());
                Out.Line("{\"file\":\"" + File + "\",\"mode\":\"" + (Mode == 0 ? "file" : "memory") +
                         "\",\"open\":\"" + (bOpen ? "1" : "0") + "\"}");
                if (bOpen)
                    DumpReader(*Reader, Mode == 1, Out);
                delete Reader;
            }
        }
    }

    // --- Original data ----------------------------------------------------------------

    // .ini files under Relative (a name longer than four bytes ending in .ini in
    // any case), as '/'-separated paths. crates/neuron-core/tests/data_dump.rs
    // applies the same rule.
    void ListIniFiles(const std::string& Relative, std::vector<std::string>& Out)
    {
        WIN32_FIND_DATAA Found;
        HANDLE Search = FindFirstFileA((Relative + "/*").c_str(), &Found);
        if (Search == INVALID_HANDLE_VALUE)
            return;
        do
        {
            const std::string Name = Found.cFileName;
            if (Name == "." || Name == "..")
                continue;
            const std::string Child = Relative + "/" + Name;
            if ((Found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                ListIniFiles(Child, Out);
            else if (Name.size() > 4 && _stricmp(Name.c_str() + Name.size() - 4, ".ini") == 0)
                Out.push_back(Child);
        } while (FindNextFileA(Search, &Found));
        FindClose(Search);
    }

    std::string OutputFile(const char* Kind, const std::string& Relative)
    {
        std::string Path = GOutputDir + "\\" + Kind + "\\" + Relative + ".jsonl";
        for (char& Ch : Path)
        {
            if (Ch == '/')
                Ch = '\\';
        }
        return Path;
    }

    // Every .ini file under DATA and EXE through INI_Reader, and every .ini file
    // directly in EXE through the engine parser, one dump per file; then the data
    // dumps of later work packages (oracle_support.h) over every file. Run from
    // the folder that holds DATA and EXE. Nothing it writes may be committed.
    void DumpData()
    {
        std::vector<std::string> Files;
        ListIniFiles("DATA", Files);
        ListIniFiles("EXE", Files);
        std::sort(Files.begin(), Files.end());
        std::vector<std::string> AllFiles;
        ListDataFiles("DATA", AllFiles);
        ListDataFiles("EXE", AllFiles);
        std::sort(AllFiles.begin(), AllFiles.end());
        if (Files.empty())
        {
            fprintf(stderr, "no .ini files under DATA or EXE in %s\n", GFixtureDir.c_str());
            GFailed = true;
            return;
        }
        {
            Suite List(GOutputDir + "\\files.txt");
            for (const std::string& File : Files)
                List.Line(File);
        }
        IProfileParser* Parser = AcquireParser();
        size_t Engine = 0;
        for (const std::string& File : Files)
        {
            {
                Suite Out(OutputFile("reader", File));
                INI_Reader* Reader = new INI_Reader();
                const bool bOpen = Reader->open(File.c_str(), false);
                Out.Line("{\"file\":\"" + File + "\",\"open\":\"" + (bOpen ? "1" : "0") + "\"}");
                if (bOpen)
                    DumpReader(*Reader, false, Out);
                delete Reader;
            }
            const bool bEngine = File.compare(0, 4, "EXE/") == 0 && File.find('/', 4) == std::string::npos;
            if (bEngine && Parser != nullptr)
            {
                Suite Out(OutputFile("engine", File));
                DumpEngineFile(Parser, File, Out);
                ++Engine;
            }
        }
        printf("dumped %u files, %u also through the engine parser\n", (unsigned int)Files.size(),
               (unsigned int)Engine);
        RunVfsDataDump(AllFiles);
        RunSurDataDump(AllFiles);
        RunAnimDataDump(AllFiles);
        RunFxDataDump(AllFiles);
        RunShadingDataDump(AllFiles);
    }
}

int main(int Argc, char** Argv)
{
    const bool bDump = Argc == 4 && strcmp(Argv[1], "dump") == 0;
    const bool bCatalog = Argc == 4 && strcmp(Argv[1], "catalog") == 0;
    if (Argc != 3 && !bDump && !bCatalog)
    {
        fprintf(stderr, "usage: Oracle.exe <fixture directory> <output directory>\n"
                        "       Oracle.exe dump <data directory> <output directory>\n"
                        "       Oracle.exe catalog <resource DLL directory> <output directory>\n");
        return 2;
    }
    const bool bMode = bDump || bCatalog;
    GFixtureDir = Argv[bMode ? 2 : 1];
    GOutputDir = Argv[bMode ? 3 : 2];
    if (!SetCurrentDirectoryA(GFixtureDir.c_str()))
    {
        fprintf(stderr, "cannot enter %s\n", GFixtureDir.c_str());
        return 2;
    }
    // Simulation vectors run in 53-bit precision (decision T11); this is
    // also the process default.
    unsigned int Control = 0;
    _controlfp_s(&Control, _PC_53, _MCW_PC);
    FDUMP = OracleDump;

    if (bDump)
    {
        DumpData();
        return GFailed ? 1 : 0;
    }
    if (bCatalog)
    {
        RunCatalogDump();
        return GFailed ? 1 : 0;
    }
    Names();
    StricmpPairs();
    Numbers();
    Fields();
    Wide();
    Doubles();
    Rand();
    EngineIni();
    IniReader();
    RunMathSuites();
    RunNetSuites();
    RunUtilSuites();
    RunVfsSuites();
    RunSurSuites();
    RunAnimSuites();
    RunFxDataSuites();
    RunShadingSuites();
    return GFailed ? 1 : 0;
}
