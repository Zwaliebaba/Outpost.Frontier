// Oracle.exe sim: the rand() recorder (oracle_sim_rand.h; layout in design/sim-records.md,
// `load_rand.jsonl`). One counting wrapper sits in the import table of every module that
// imports rand; it calls the C runtime's rand, so the sequence is the one an unrecorded run
// draws. The load-time recorder keeps every draw from the harness's srand until the first
// frame with its caller (module offset, function, source line, a short stack, resolved with
// dbghelp from the build's PDB files) and checks that no draw escaped the hooks by following
// the CRT's generator from the seed.
//
// Everything here is inert unless Oracle.exe sim calls it.

#include <windows.h>
#include <dbghelp.h>
#include <intrin.h>
#include <tlhelp32.h>

#include <stdio.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "oracle_sim_rand.h"
#include "../OutpostFrontier/Client/oracle_json.h"

#pragma comment(lib, "dbghelp.lib")
#pragma intrinsic(_ReturnAddress)

namespace
{
    typedef int(__cdecl* FRandFn)();

    // A module whose rand import the wrapper replaced, with the import it had.
    struct FHookedModule
    {
        HMODULE Module;
        std::string Name;
        FRandFn Original;
    };
    std::vector<FHookedModule> GModules;
    FRandFn GFallbackRand = nullptr;
    oracle::FRandObserver GObserver = nullptr;

    // Frames kept per draw: the caller first.
    const unsigned int kMaxFrames = 10;
    // Frames that make the grouping key of the summary.
    const unsigned int kKeyFrames = 4;
    // Lines of the summary's in-order listing per phase.
    const unsigned int kMaxRuns = 400;
    // The generator steps the gap search tries before it gives up.
    const unsigned int kMaxGap = 1000000;

    struct FDraw
    {
        unsigned int Index;   // in the CRT sequence of the seed, from 0
        int Value;
        int Module;           // in GModules, -1 when the caller is in no hooked module
        unsigned int Phase;   // in GPhases
        unsigned int Gap;     // draws the hooks did not see just before this one
        unsigned int FrameCount;
        DWORD64 Frames[kMaxFrames];
    };

    bool GLoadActive = false;
    FILE* GLoadFile = nullptr;
    std::string GLoadDir;
    unsigned int GSeed = 0;
    unsigned int GShadowState = 0;
    unsigned int GShadowIndex = 0;
    unsigned int GGapTotal = 0;
    bool GGapLost = false;
    bool GSymbols = false;
    std::vector<std::string> GPhases;
    std::vector<FDraw> GDraws;
    // Each srand after the first: the draws recorded before it, and the index it reset.
    std::vector<std::pair<unsigned int, unsigned int>> GReseeds;

    // The hooked module that holds Address, -1 for none.
    int ModuleOf(void const* Address)
    {
        HMODULE Module = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCSTR>(Address), &Module))
            return -1;
        for (size_t Index = 0; Index < GModules.size(); ++Index)
        {
            if (GModules[Index].Module == Module)
                return (int)Index;
        }
        return -1;
    }

    // The CRT's generator, from the seed: the index of the next draw that gives Value.
    void FollowGenerator(int Value, FDraw& Draw)
    {
        Draw.Gap = 0;
        if (GGapLost)
        {
            Draw.Index = GShadowIndex++;
            return;
        }
        unsigned int State = GShadowState;
        unsigned int Index = GShadowIndex;
        for (unsigned int Step = 0; Step < kMaxGap; ++Step)
        {
            State = State * 214013u + 2531011u;
            ++Index;
            if ((int)((State >> 16) & 0x7fffu) == Value)
            {
                Draw.Gap = Step;
                Draw.Index = Index - 1;
                GGapTotal += Step;
                GShadowState = State;
                GShadowIndex = Index;
                return;
            }
        }
        // Not found: the generator is lost (counted from here on without a check).
        GGapLost = true;
        Draw.Index = GShadowIndex++;
    }

    __declspec(noinline) void CaptureStack(void* Return, FDraw& Draw)
    {
        Draw.FrameCount = 0;
        Draw.Frames[Draw.FrameCount++] = (DWORD64)(ULONG_PTR)Return;
#if defined(_M_IX86)
        if (GSymbols)
        {
            // StackWalk64 reads the PDBs' frame data, so frames without a frame pointer
            // (/Oy, the release build) are walked too.
            CONTEXT Context;
            memset(&Context, 0, sizeof(Context));
            RtlCaptureContext(&Context);
            STACKFRAME64 Frame;
            memset(&Frame, 0, sizeof(Frame));
            Frame.AddrPC.Offset = Context.Eip;
            Frame.AddrPC.Mode = AddrModeFlat;
            Frame.AddrFrame.Offset = Context.Ebp;
            Frame.AddrFrame.Mode = AddrModeFlat;
            Frame.AddrStack.Offset = Context.Esp;
            Frame.AddrStack.Mode = AddrModeFlat;
            HANDLE const Process = GetCurrentProcess();
            HANDLE const Thread = GetCurrentThread();
            bool bFound = false;
            for (unsigned int Step = 0; Step < kMaxFrames + 8 && Draw.FrameCount < kMaxFrames; ++Step)
            {
                if (!StackWalk64(IMAGE_FILE_MACHINE_I386, Process, Thread, &Frame, &Context, nullptr,
                                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                    break;
                if (Frame.AddrPC.Offset == 0)
                    break;
                // This recorder's own frames come first; the caller's frame is Return.
                if (!bFound)
                {
                    bFound = Frame.AddrPC.Offset == Draw.Frames[0];
                    continue;
                }
                Draw.Frames[Draw.FrameCount++] = Frame.AddrPC.Offset;
            }
            if (bFound)
                return;
            Draw.FrameCount = 1;
        }
#endif
        // Without symbols: the frame-pointer chain (stops at the first frame without one).
        void* Frames[kMaxFrames + 3] = {};
        const USHORT Count = RtlCaptureStackBackTrace(0, kMaxFrames + 3, Frames, nullptr);
        bool bFound = false;
        for (USHORT Index = 0; Index < Count && Draw.FrameCount < kMaxFrames; ++Index)
        {
            if (!bFound)
            {
                bFound = Frames[Index] == Return;
                continue;
            }
            Draw.Frames[Draw.FrameCount++] = (DWORD64)(ULONG_PTR)Frames[Index];
        }
    }

    void RecordLoadDraw(int Value, int Module, void* Return)
    {
        FDraw Draw;
        memset(&Draw, 0, sizeof(Draw));
        Draw.Value = Value;
        Draw.Module = Module;
        Draw.Phase = GPhases.empty() ? 0 : (unsigned int)(GPhases.size() - 1);
        FollowGenerator(Value, Draw);
        CaptureStack(Return, Draw);
        GDraws.push_back(Draw);
    }

    __declspec(noinline) int __cdecl CountingRand()
    {
        void* const Return = _ReturnAddress();
        const int Module = ModuleOf(Return);
        FRandFn const Real = Module >= 0 ? GModules[Module].Original : GFallbackRand;
        const int Value = Real();
        if (GLoadActive)
            RecordLoadDraw(Value, Module, Return);
        if (GObserver != nullptr)
            GObserver(Value, Module >= 0 ? GModules[Module].Name.c_str() : "?");
        return Value;
    }

    // Replaces Module's import of Name (by name, from any DLL) with Replacement when it is
    // bound to Expected; the old pointer goes to Original. The IAT is the module's own, so
    // only its calls are seen.
    bool PatchImport(HMODULE Module, const char* Name, void* Replacement, void* Expected, void** Original)
    {
        if (Module == nullptr)
            return false;
        BYTE* const Base = reinterpret_cast<BYTE*>(Module);
        IMAGE_DOS_HEADER const* const Dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(Base);
        if (Dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;
        IMAGE_NT_HEADERS const* const Nt = reinterpret_cast<IMAGE_NT_HEADERS const*>(Base + Dos->e_lfanew);
        IMAGE_DATA_DIRECTORY const& Dir = Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (Dir.VirtualAddress == 0)
            return false;
        IMAGE_IMPORT_DESCRIPTOR const* Desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR const*>(Base + Dir.VirtualAddress);
        for (; Desc->Name != 0; ++Desc)
        {
            if (Desc->OriginalFirstThunk == 0)
                continue;
            IMAGE_THUNK_DATA const* Names = reinterpret_cast<IMAGE_THUNK_DATA const*>(Base + Desc->OriginalFirstThunk);
            IMAGE_THUNK_DATA* Slots = reinterpret_cast<IMAGE_THUNK_DATA*>(Base + Desc->FirstThunk);
            for (; Names->u1.AddressOfData != 0; ++Names, ++Slots)
            {
                if (IMAGE_SNAP_BY_ORDINAL(Names->u1.Ordinal))
                    continue;
                IMAGE_IMPORT_BY_NAME const* const ByName =
                    reinterpret_cast<IMAGE_IMPORT_BY_NAME const*>(Base + Names->u1.AddressOfData);
                if (strcmp(reinterpret_cast<const char*>(ByName->Name), Name) != 0)
                    continue;
                if (Slots->u1.Function != reinterpret_cast<ULONG_PTR>(Expected))
                    return false;
                DWORD Old = 0;
                if (!VirtualProtect(&Slots->u1.Function, sizeof(Slots->u1.Function), PAGE_READWRITE, &Old))
                    return false;
                *Original = reinterpret_cast<void*>(Slots->u1.Function);
                Slots->u1.Function = reinterpret_cast<ULONG_PTR>(Replacement);
                VirtualProtect(&Slots->u1.Function, sizeof(Slots->u1.Function), Old, &Old);
                return true;
            }
        }
        return false;
    }

    // ---- symbols --------------------------------------------------------------------------

    struct FResolved
    {
        std::string Module;             // "Server.dll"
        DWORD64 Offset;                 // from the module base
        std::vector<std::string> Names; // the innermost inlined function first, then the function
        std::string Line;               // file:line of the innermost, "" when unknown
    };

    std::string ShortPath(const char* Path)
    {
        std::string Text = Path != nullptr ? Path : "";
        for (char& Ch : Text)
        {
            if (Ch == '\\')
                Ch = '/';
        }
        const size_t At = Text.rfind("CppSource/");
        if (At != std::string::npos)
            Text = Text.substr(At + 10);
        return Text;
    }

    FResolved Resolve(DWORD64 Address)
    {
        FResolved Out;
        Out.Offset = Address;
        HMODULE Module = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>((ULONG_PTR)Address), &Module))
        {
            char Path[MAX_PATH] = {};
            GetModuleFileNameA(Module, Path, MAX_PATH);
            const char* const Slash = strrchr(Path, '\\');
            Out.Module = Slash != nullptr ? Slash + 1 : Path;
            Out.Offset = Address - (DWORD64)(ULONG_PTR)Module;
        }
        if (!GSymbols)
            return Out;
        // A return address: the call is the instruction before it.
        const DWORD64 At = Address - 1;
        HANDLE const Process = GetCurrentProcess();
        union
        {
            SYMBOL_INFO Info;
            char Buffer[sizeof(SYMBOL_INFO) + 512];
        } Symbol;
        memset(&Symbol, 0, sizeof(Symbol));
        Symbol.Info.SizeOfStruct = sizeof(SYMBOL_INFO);
        Symbol.Info.MaxNameLen = 511;
        DWORD64 Displacement = 0;

        const DWORD Inlines = SymAddrIncludeInlineTrace(Process, At);
        DWORD Context = 0;
        DWORD FrameIndex = 0;
        bool bInline = Inlines > 0 && SymQueryInlineTrace(Process, At, 0, At, At, &Context, &FrameIndex);
        if (bInline)
        {
            for (DWORD Index = 0; Index < Inlines; ++Index)
            {
                if (SymFromInlineContext(Process, At, Context + Index, &Displacement, &Symbol.Info))
                    Out.Names.push_back(Symbol.Info.Name);
            }
            IMAGEHLP_LINE64 Line;
            memset(&Line, 0, sizeof(Line));
            Line.SizeOfStruct = sizeof(Line);
            DWORD LineDisplacement = 0;
            if (SymGetLineFromInlineContext(Process, At, Context, 0, &LineDisplacement, &Line))
                Out.Line = ShortPath(Line.FileName) + ":" + std::to_string(Line.LineNumber);
        }
        if (SymFromAddr(Process, At, &Displacement, &Symbol.Info))
            Out.Names.push_back(Symbol.Info.Name);
        if (Out.Line.empty())
        {
            IMAGEHLP_LINE64 Line;
            memset(&Line, 0, sizeof(Line));
            Line.SizeOfStruct = sizeof(Line);
            DWORD LineDisplacement = 0;
            if (SymGetLineFromAddr64(Process, At, &LineDisplacement, &Line))
                Out.Line = ShortPath(Line.FileName) + ":" + std::to_string(Line.LineNumber);
        }
        return Out;
    }

    std::string Hex(DWORD64 Value)
    {
        char Text[32];
        sprintf_s(Text, "0x%llx", (unsigned long long)Value);
        return Text;
    }

    std::string Where(FResolved const& R)
    {
        return (R.Module.empty() ? std::string("?") : R.Module) + "+" + Hex(R.Offset);
    }

    std::string Chain(FResolved const& R)
    {
        std::string Text;
        for (size_t Index = 0; Index < R.Names.size(); ++Index)
        {
            if (Index > 0)
                Text += " < ";
            Text += R.Names[Index];
        }
        return Text.empty() ? Where(R) : Text;
    }

    void WriteLine(FOracleJson& J)
    {
        if (GLoadFile == nullptr)
            return;
        fputs(J.Finish().c_str(), GLoadFile);
        fputc('\n', GLoadFile);
    }
}

namespace oracle
{
    void OracleSimRandHook(std::vector<std::string>& Hooked)
    {
        // Only imports bound to the universal CRT's rand, the one generator the game DLLs
        // share (system DLLs that import msvcrt.dll's have a generator of their own).
        HMODULE Crt = GetModuleHandleA("ucrtbase.dll");
        if (Crt == nullptr)
            Crt = GetModuleHandleA("ucrtbased.dll");
        void* const CrtRand = Crt != nullptr ? reinterpret_cast<void*>(GetProcAddress(Crt, "rand")) : nullptr;
        if (CrtRand == nullptr)
        {
            Hooked.clear();
            return;
        }
        if (GFallbackRand == nullptr)
            GFallbackRand = reinterpret_cast<FRandFn>(CrtRand);
        HANDLE const Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (Snapshot != INVALID_HANDLE_VALUE)
        {
            MODULEENTRY32 Entry;
            memset(&Entry, 0, sizeof(Entry));
            Entry.dwSize = sizeof(Entry);
            for (BOOL bMore = Module32First(Snapshot, &Entry); bMore; bMore = Module32Next(Snapshot, &Entry))
            {
                bool bKnown = false;
                for (FHookedModule const& M : GModules)
                    bKnown = bKnown || M.Module == Entry.hModule;
                if (bKnown)
                    continue;
                void* Original = nullptr;
                if (!PatchImport(Entry.hModule, "rand", reinterpret_cast<void*>(&CountingRand), CrtRand, &Original))
                    continue;
                FHookedModule M;
                M.Module = Entry.hModule;
                M.Name = Entry.szModule;
                M.Original = reinterpret_cast<FRandFn>(Original);
                GModules.push_back(M);
            }
            CloseHandle(Snapshot);
        }
        Hooked.clear();
        for (FHookedModule const& M : GModules)
            Hooked.push_back(M.Name);
    }

    void OracleSimRandSetObserver(FRandObserver Observer)
    {
        GObserver = Observer;
    }

    bool OracleSimLoadRandBegin(std::string const& OutDir, unsigned int Seed)
    {
        const std::string Path = OutDir + "\\load_rand.jsonl";
        GLoadFile = fopen(Path.c_str(), "wb");
        if (GLoadFile == nullptr)
            return false;
        GLoadDir = OutDir;
        GSeed = Seed;
        GShadowState = Seed;
        GShadowIndex = 0;
        GGapTotal = 0;
        GGapLost = false;
        GPhases.clear();
        GPhases.push_back("start");
        GDraws.clear();
        GDraws.reserve(16384);
        GReseeds.clear();

        // The symbol handler over every loaded module, the PDBs beside them (bin\).
        char Exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, Exe, MAX_PATH);
        char* const Slash = strrchr(Exe, '\\');
        if (Slash != nullptr)
            *Slash = '\0';
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
        GSymbols = SymInitialize(GetCurrentProcess(), Exe, TRUE) != FALSE;

        std::vector<std::string> Hooked;
        OracleSimRandHook(Hooked);
        GLoadActive = true;
        return true;
    }

    void OracleSimLoadRandPhase(std::string const& Phase)
    {
        if (GLoadFile != nullptr)
            GPhases.push_back(Phase);
    }

    void OracleSimLoadRandReseed(unsigned int Seed)
    {
        if (!GLoadActive)
            return;
        GReseeds.push_back(std::make_pair((unsigned int)GDraws.size(), GShadowIndex));
        GShadowState = Seed;
        GShadowIndex = 0;
        GGapLost = false;
    }

    void OracleSimLoadRandEnd()
    {
        if (!GLoadActive)
            return;
        GLoadActive = false;
        if (GSymbols)
            SymRefreshModuleList(GetCurrentProcess());

        // Every address once.
        std::map<DWORD64, FResolved> Cache;
        for (FDraw const& D : GDraws)
        {
            for (unsigned int Index = 0; Index < D.FrameCount; ++Index)
            {
                if (Cache.find(D.Frames[Index]) == Cache.end())
                    Cache[D.Frames[Index]] = Resolve(D.Frames[Index]);
            }
        }
        std::vector<std::string> Hooked;
        for (FHookedModule const& M : GModules)
            Hooked.push_back(M.Name);

        {
            FOracleJson J;
            J.Str("rec", "load_rand_start");
            J.Uint("version", 1);
            J.Uint("seed", GSeed);
            J.Bool("symbols", GSymbols);
            J.BeginArray("hooks");
            for (std::string const& Name : Hooked)
                J.Str(nullptr, Name.c_str());
            J.EndArray();
            WriteLine(J);
        }
        size_t NextReseed = 0;
        for (size_t DrawIndex = 0; DrawIndex <= GDraws.size(); ++DrawIndex)
        {
            while (NextReseed < GReseeds.size() && GReseeds[NextReseed].first == DrawIndex)
            {
                FOracleJson R;
                R.Str("rec", "reseed");
                R.Uint("seed", GSeed);
                R.Uint("drawn", GReseeds[NextReseed].second);
                WriteLine(R);
                ++NextReseed;
            }
            if (DrawIndex == GDraws.size())
                break;
            FDraw const& D = GDraws[DrawIndex];
            FResolved const& Caller = Cache[D.Frames[0]];
            FOracleJson J;
            J.Str("rec", "draw");
            J.Uint("i", D.Index);
            J.Str("phase", GPhases[D.Phase].c_str());
            J.Str("mod", D.Module >= 0 ? GModules[D.Module].Name.c_str() : "?");
            J.Int("v", D.Value);
            J.Uint("gap", D.Gap);
            J.Str("at", Where(Caller).c_str());
            J.Str("fn", Chain(Caller).c_str());
            J.Str("line", Caller.Line.c_str());
            J.BeginArray("stack");
            for (unsigned int Index = 1; Index < D.FrameCount; ++Index)
            {
                FResolved const& R = Cache[D.Frames[Index]];
                const std::string Text = Where(R) + " " + Chain(R) + (R.Line.empty() ? "" : " (" + R.Line + ")");
                J.Str(nullptr, Text.c_str());
            }
            J.EndArray();
            WriteLine(J);
        }

        // The summary, per phase: the callers by first draw, then the runs in order.
        std::string Summary;
        char Text[512];
        sprintf_s(Text, "load-time rand() draws: seed %u (0x%08x), %u recorded, %u not seen by the hooks%s, next index %u\n",
                  GSeed, GSeed, (unsigned)GDraws.size(), GGapTotal, GGapLost ? " (generator lost)" : "", GShadowIndex);
        Summary += Text;
        Summary += "hooked:";
        for (std::string const& Name : Hooked)
            Summary += " " + Name;
        Summary += GSymbols ? "\nsymbols: dbghelp\n" : "\nsymbols: none\n";
        for (std::pair<unsigned int, unsigned int> const& R : GReseeds)
        {
            sprintf_s(Text, "srand(%u) again after %u draws: the later indices count from 0\n", GSeed, R.second);
            Summary += Text;
        }
        for (unsigned int Phase = 0; Phase < GPhases.size(); ++Phase)
        {
            std::vector<FDraw const*> Draws;
            for (FDraw const& D : GDraws)
            {
                if (D.Phase == Phase)
                    Draws.push_back(&D);
            }
            FOracleJson J;
            J.Str("rec", "phase");
            J.Str("phase", GPhases[Phase].c_str());
            J.Uint("first", Draws.empty() ? GShadowIndex : Draws.front()->Index);
            J.Uint("count", Draws.empty() ? 0 : Draws.back()->Index + 1 - Draws.front()->Index);
            J.Uint("recorded", (unsigned)Draws.size());
            WriteLine(J);
            if (Draws.empty())
            {
                Summary += "phase " + GPhases[Phase] + ": no draws\n";
                continue;
            }
            sprintf_s(Text, "phase %s: %u draws, index %u..%u\n", GPhases[Phase].c_str(), (unsigned)Draws.size(),
                      Draws.front()->Index, Draws.back()->Index);
            Summary += Text;

            // The grouping key: the module and the functions of the first frames.
            auto KeyOf = [&](FDraw const& D) {
                std::string Key = D.Module >= 0 ? GModules[D.Module].Name : std::string("?");
                for (unsigned int Index = 0; Index < D.FrameCount && Index < kKeyFrames; ++Index)
                    Key += (Index == 0 ? " " : " <- ") + Chain(Cache[D.Frames[Index]]);
                return Key;
            };
            std::vector<std::string> Order;
            std::map<std::string, std::pair<unsigned int, unsigned int>> Groups; // first index, count
            std::map<std::string, std::string> Lines;
            for (FDraw const* D : Draws)
            {
                const std::string Key = KeyOf(*D);
                auto Found = Groups.find(Key);
                if (Found == Groups.end())
                {
                    Order.push_back(Key);
                    Groups[Key] = std::make_pair(D->Index, 1u);
                    Lines[Key] = Cache[D->Frames[0]].Line;
                }
                else
                    ++Found->second.second;
            }
            Summary += "  by caller (first index, draws, caller <- its callers, source line):\n";
            for (std::string const& Key : Order)
            {
                sprintf_s(Text, "    %6u %6u  ", Groups[Key].first, Groups[Key].second);
                Summary += Text + Key + (Lines[Key].empty() ? "" : "  [" + Lines[Key] + "]") + "\n";
            }
            Summary += "  in order (index range, draws, caller):\n";
            unsigned int Runs = 0;
            for (size_t At = 0; At < Draws.size();)
            {
                const std::string Key = KeyOf(*Draws[At]);
                size_t End = At + 1;
                while (End < Draws.size() && KeyOf(*Draws[End]) == Key)
                    ++End;
                if (Runs++ < kMaxRuns)
                {
                    sprintf_s(Text, "    %6u..%-6u %6u  ", Draws[At]->Index, Draws[End - 1]->Index, (unsigned)(End - At));
                    Summary += Text + Key + "\n";
                }
                At = End;
            }
            if (Runs > kMaxRuns)
            {
                sprintf_s(Text, "    ... %u runs, the first %u listed\n", Runs, kMaxRuns);
                Summary += Text;
            }
        }
        {
            FOracleJson J;
            J.Str("rec", "load_rand_end");
            J.Uint("recorded", (unsigned)GDraws.size());
            J.Uint("unseen", GGapTotal);
            J.Bool("lost", GGapLost);
            J.Uint("next_index", GShadowIndex);
            WriteLine(J);
        }
        fclose(GLoadFile);
        GLoadFile = nullptr;
        const std::string SummaryPath = GLoadDir + "\\load_rand.txt";
        FILE* const File = fopen(SummaryPath.c_str(), "wb");
        if (File != nullptr)
        {
            fputs(Summary.c_str(), File);
            fclose(File);
        }
        GDraws.clear();
        GDraws.shrink_to_fit();
    }
}
