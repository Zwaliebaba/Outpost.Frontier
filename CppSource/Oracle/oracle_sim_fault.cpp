// Oracle.exe sim, the fault recorder (oracle_sim_fault.h; layout in design/sim-records.md,
// session.jsonl `fault`). Symbols are resolved with dbghelp from the PDB files beside
// Oracle.exe (bin\), as the load-time rand recorder of the load-draw work resolves its
// callers: SymFromAddr / SymFromInlineContext for the function, SymGetLineFromAddr64 for the
// source line, StackWalk64 over the exception's context for the stack (the PDBs' frame data
// cover frames without a frame pointer).
//
// Everything here is inert unless Oracle.exe sim calls it.

#include <windows.h>
#include <dbghelp.h>

#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "oracle_sim_fault.h"
#include "../OutpostFrontier/Client/oracle_json.h"

#pragma comment(lib, "dbghelp.lib")

#if !defined(_M_IX86)
#error "oracle_sim_fault.cpp reads the x86 CONTEXT"
#endif

namespace
{
    oracle::FSimFaultWriteFn GWrite = nullptr;
    unsigned int const* GTick = nullptr;
    bool GSymbols = false;
    bool GStarted = false;
    volatile LONG GInHandler = 0;
    unsigned int GFirstChanceCount = 0;
    unsigned int const kMaxFirstChance = 8;
    unsigned int const kMaxFrames = 48;

    struct FResolved
    {
        std::string Module;             // "Server.dll", "" outside every module
        DWORD64 Offset = 0;             // from the module base (the address outside a module)
        std::vector<std::string> Names; // the innermost inlined function first, then the function
        std::string Line;               // file:line of the innermost, "" when unknown
    };

    std::string ShortPath(char const* Path)
    {
        std::string Text = Path != nullptr ? Path : "";
        for (char& Ch : Text)
        {
            if (Ch == '\\')
                Ch = '/';
        }
        size_t const At = Text.rfind("CppSource/");
        if (At != std::string::npos)
            Text = Text.substr(At + 10);
        return Text;
    }

    std::string Hex(DWORD64 Value)
    {
        char Text[32];
        sprintf_s(Text, "0x%llx", (unsigned long long)Value);
        return Text;
    }

    // Address: a code or data address; bReturn: a return address, whose call is the
    // instruction before it.
    FResolved Resolve(DWORD64 Address, bool bReturn)
    {
        FResolved Out;
        Out.Offset = Address;
        HMODULE Module = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>((ULONG_PTR)Address), &Module))
        {
            char Path[MAX_PATH] = {};
            GetModuleFileNameA(Module, Path, MAX_PATH);
            char const* const Slash = strrchr(Path, '\\');
            Out.Module = Slash != nullptr ? Slash + 1 : Path;
            Out.Offset = Address - (DWORD64)(ULONG_PTR)Module;
        }
        if (!GSymbols || Out.Module.empty())
            return Out;
        DWORD64 const At = bReturn ? Address - 1 : Address;
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

        DWORD const Inlines = SymAddrIncludeInlineTrace(Process, At);
        DWORD Context = 0;
        DWORD FrameIndex = 0;
        if (Inlines > 0 && SymQueryInlineTrace(Process, At, 0, At, At, &Context, &FrameIndex))
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
        {
            std::string Name = Symbol.Info.Name;
            if (Displacement != 0)
                Name += "+" + Hex(Displacement);
            Out.Names.push_back(Name);
        }
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

    void ResolvedJ(FOracleJson& J, FResolved const& R)
    {
        if (R.Module.empty())
            J.Null("module");
        else
            J.Str("module", R.Module.c_str());
        J.Str("offset", Hex(R.Offset).c_str());
        J.BeginArray("functions");
        for (std::string const& Name : R.Names)
            J.Str(nullptr, Name.c_str());
        J.EndArray();
        J.Str("line", R.Line.c_str());
    }

    // Count bytes at Address are committed and readable.
    bool Readable(DWORD Address, size_t Count)
    {
        MEMORY_BASIC_INFORMATION Info;
        if (Address == 0 || VirtualQuery(reinterpret_cast<void const*>((ULONG_PTR)Address), &Info, sizeof(Info)) == 0)
            return false;
        if (Info.State != MEM_COMMIT || (Info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0)
            return false;
        DWORD const ReadMask = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if ((Info.Protect & ReadMask) == 0)
            return false;
        ULONG_PTR const End = (ULONG_PTR)Info.BaseAddress + Info.RegionSize;
        return (ULONG_PTR)Address + Count <= End;
    }

    char const* StateName(DWORD Address)
    {
        MEMORY_BASIC_INFORMATION Info;
        if (VirtualQuery(reinterpret_cast<void const*>((ULONG_PTR)Address), &Info, sizeof(Info)) == 0)
            return "unknown";
        if (Info.State == MEM_FREE)
            return "free";
        if (Info.State == MEM_RESERVE)
            return "reserved";
        return Readable(Address, 1) ? "committed" : "committed, not readable";
    }

    // A register's value and, when it points at readable memory, the first words there, the
    // first one resolved when it lies in a module (a vtable names the object's class).
    void RegisterJ(FOracleJson& J, char const* Name, DWORD Value)
    {
        J.BeginObject(Name);
        J.Str("v", Hex(Value).c_str());
        HMODULE Module = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>((ULONG_PTR)Value), &Module))
        {
            FResolved const R = Resolve(Value, false);
            J.Str("in", (R.Module + "+" + Hex(R.Offset) + (R.Names.empty() ? "" : " " + R.Names.back())).c_str());
        }
        if (Readable(Value, 16))
        {
            DWORD const* const Words = reinterpret_cast<DWORD const*>((ULONG_PTR)Value);
            J.BeginArray("words");
            for (int Index = 0; Index < 4; ++Index)
                J.Str(nullptr, Hex(Words[Index]).c_str());
            J.EndArray();
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>((ULONG_PTR)Words[0]), &Module))
            {
                FResolved const R = Resolve(Words[0], false);
                J.Str("word0_in",
                      (R.Module + "+" + Hex(R.Offset) + (R.Names.empty() ? "" : " " + R.Names.back())).c_str());
            }
        }
        else
        {
            J.Str("memory", StateName(Value));
        }
        J.EndObject();
    }

    char const* CodeName(DWORD Code)
    {
        switch (Code)
        {
        case EXCEPTION_ACCESS_VIOLATION: return "access violation";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "datatype misalignment";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "float divide by zero";
        case EXCEPTION_FLT_INVALID_OPERATION: return "float invalid operation";
        case EXCEPTION_FLT_OVERFLOW: return "float overflow";
        case EXCEPTION_FLT_STACK_CHECK: return "float stack check";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
        case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
        case EXCEPTION_INT_OVERFLOW: return "integer overflow";
        case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
        case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
        case EXCEPTION_BREAKPOINT: return "breakpoint";
        case 0xE06D7363: return "C++ exception";
        default: return "other";
        }
    }

    // The hardware exceptions the vectored handler records; C++ exceptions and the debugger's
    // notifications are not faults.
    bool IsHardwareFault(DWORD Code)
    {
        switch (Code)
        {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
        case EXCEPTION_DATATYPE_MISALIGNMENT:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_INT_OVERFLOW:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
            return true;
        default:
            return false;
        }
    }

    void WriteRecord(EXCEPTION_POINTERS* Info, bool bFirstChance)
    {
        EXCEPTION_RECORD const* const E = Info->ExceptionRecord;
        CONTEXT const* const C = Info->ContextRecord;
        if (GSymbols)
            SymRefreshModuleList(GetCurrentProcess());

        FOracleJson J;
        J.Str("rec", "fault");
        J.Uint("tick", GTick != nullptr ? *GTick : 0);
        J.Bool("first_chance", bFirstChance);
        J.Str("code", Hex(E->ExceptionCode).c_str());
        J.Str("code_name", CodeName(E->ExceptionCode));
        J.Str("address", Hex((DWORD64)(ULONG_PTR)E->ExceptionAddress).c_str());
        {
            J.BeginObject("at");
            ResolvedJ(J, Resolve((DWORD64)(ULONG_PTR)E->ExceptionAddress, false));
            J.EndObject();
        }
        if ((E->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || E->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
            E->NumberParameters >= 2)
        {
            ULONG_PTR const Kind = E->ExceptionInformation[0];
            J.Str("access", Kind == 0 ? "read" : Kind == 1 ? "write" : Kind == 8 ? "execute" : "other");
            J.Str("target", Hex(E->ExceptionInformation[1]).c_str());
            J.Str("target_memory", StateName((DWORD)E->ExceptionInformation[1]));
        }
        J.Bool("symbols", GSymbols);

        J.BeginObject("regs");
        RegisterJ(J, "eax", C->Eax);
        RegisterJ(J, "ebx", C->Ebx);
        RegisterJ(J, "ecx", C->Ecx);
        RegisterJ(J, "edx", C->Edx);
        RegisterJ(J, "esi", C->Esi);
        RegisterJ(J, "edi", C->Edi);
        RegisterJ(J, "ebp", C->Ebp);
        J.Str("esp", Hex(C->Esp).c_str());
        J.Str("eip", Hex(C->Eip).c_str());
        J.Str("eflags", Hex(C->EFlags).c_str());
        J.EndObject();

        // The words at esp: a fallback when the walk stops early.
        if (Readable(C->Esp, 16 * sizeof(DWORD)))
        {
            DWORD const* const Words = reinterpret_cast<DWORD const*>((ULONG_PTR)C->Esp);
            J.BeginArray("esp_words");
            for (int Index = 0; Index < 16; ++Index)
                J.Str(nullptr, Hex(Words[Index]).c_str());
            J.EndArray();
        }

        J.BeginArray("stack");
        if (GSymbols)
        {
            CONTEXT Context = *C;
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
            for (unsigned int Index = 0; Index < kMaxFrames; ++Index)
            {
                if (!StackWalk64(IMAGE_FILE_MACHINE_I386, Process, Thread, &Frame, &Context, nullptr,
                                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
                    break;
                if (Frame.AddrPC.Offset == 0)
                    break;
                J.BeginObject(nullptr);
                ResolvedJ(J, Resolve(Frame.AddrPC.Offset, Index > 0));
                J.EndObject();
            }
        }
        J.EndArray();
        GWrite(J.Finish());
    }

    LONG CALLBACK FirstChanceHandler(EXCEPTION_POINTERS* Info)
    {
        if (!IsHardwareFault(Info->ExceptionRecord->ExceptionCode) || GFirstChanceCount >= kMaxFirstChance)
            return EXCEPTION_CONTINUE_SEARCH;
        if (InterlockedExchange(&GInHandler, 1) != 0)
            return EXCEPTION_CONTINUE_SEARCH;
        ++GFirstChanceCount;
        WriteRecord(Info, true);
        InterlockedExchange(&GInHandler, 0);
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

namespace oracle
{
    void OracleSimFaultStart(FSimFaultWriteFn Write, unsigned int const* Tick)
    {
        if (GStarted)
            return;
        GStarted = true;
        GWrite = Write;
        GTick = Tick;
        char Exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, Exe, MAX_PATH);
        char* const Slash = strrchr(Exe, '\\');
        if (Slash != nullptr)
            *Slash = '\0';
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
        GSymbols = SymInitialize(GetCurrentProcess(), Exe, TRUE) != FALSE;
        AddVectoredExceptionHandler(1, FirstChanceHandler);
    }

    int OracleSimFaultFilter(_EXCEPTION_POINTERS* Info)
    {
        if (GWrite != nullptr && InterlockedExchange(&GInHandler, 1) == 0)
        {
            WriteRecord(Info, false);
            InterlockedExchange(&GInHandler, 0);
        }
        return EXCEPTION_EXECUTE_HANDLER;
    }

    // No C++ object with a destructor may live in this function (C2712).
    bool OracleSimGuardedCall(void (*Fn)(void*), void* Arg)
    {
        __try
        {
            Fn(Arg);
        }
        __except (OracleSimFaultFilter(GetExceptionInformation()))
        {
            return false;
        }
        return true;
    }
}
