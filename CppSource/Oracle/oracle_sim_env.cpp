// Oracle.exe sim, the run's environment record (oracle_sim_env.h; design/sim-records.md,
// session.jsonl `env`). Oracle instrumentation: written once, at the end of a run, so it
// cannot change anything the run computes.

#include <windows.h>
#include <intrin.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

#include "oracle_sim_env.h"
#include "../OutpostFrontier/Client/oracle_json.h"

#pragma comment(lib, "version.lib")
#pragma comment(lib, "advapi32.lib")

namespace
{
    std::string Hex32(unsigned int Value)
    {
        char Buffer[16];
        sprintf(Buffer, "%08x", Value);
        return Buffer;
    }

    // CPUID: the vendor, the brand string, the signature of leaf 1 and the feature words
    // that select code paths in the CRT and the OS (SSE2, SSE4.1, AVX, FMA, AVX2, AVX-512F).
    void WriteCpu(FOracleJson& J)
    {
        int Regs[4] = {};
        __cpuid(Regs, 0);
        int const MaxLeaf = Regs[0];
        char Vendor[13] = {};
        memcpy(Vendor + 0, &Regs[1], 4);
        memcpy(Vendor + 4, &Regs[3], 4);
        memcpy(Vendor + 8, &Regs[2], 4);

        char Brand[49] = {};
        __cpuid(Regs, 0x80000000);
        if ((unsigned int)Regs[0] >= 0x80000004u)
        {
            for (int Leaf = 0; Leaf < 3; ++Leaf)
            {
                __cpuid(Regs, 0x80000002 + Leaf);
                memcpy(Brand + 16 * Leaf, Regs, 16);
            }
        }
        int Leaf1[4] = {};
        if (MaxLeaf >= 1)
            __cpuid(Leaf1, 1);
        int Leaf7[4] = {};
        if (MaxLeaf >= 7)
            __cpuidex(Leaf7, 7, 0);

        J.BeginObject("cpu");
        J.Str("vendor", Vendor);
        J.Str("brand", Brand);
        J.Str("signature", Hex32((unsigned int)Leaf1[0]).c_str());
        J.Str("leaf1_ecx", Hex32((unsigned int)Leaf1[2]).c_str());
        J.Str("leaf1_edx", Hex32((unsigned int)Leaf1[3]).c_str());
        J.Str("leaf7_ebx", Hex32((unsigned int)Leaf7[1]).c_str());
        SYSTEM_INFO Info = {};
        GetNativeSystemInfo(&Info);
        J.Uint("logical", Info.dwNumberOfProcessors);
        J.EndObject();
    }

    std::string FileVersion(char const* Path)
    {
        DWORD Handle = 0;
        DWORD const Size = GetFileVersionInfoSizeA(Path, &Handle);
        if (Size == 0)
            return std::string();
        std::vector<unsigned char> Data(Size);
        if (!GetFileVersionInfoA(Path, 0, Size, Data.data()))
            return std::string();
        VS_FIXEDFILEINFO* Fixed = nullptr;
        UINT Length = 0;
        if (!VerQueryValueA(Data.data(), "\\", reinterpret_cast<void**>(&Fixed), &Length) || Fixed == nullptr)
            return std::string();
        char Buffer[64];
        sprintf(Buffer, "%u.%u.%u.%u", HIWORD(Fixed->dwFileVersionMS), LOWORD(Fixed->dwFileVersionMS),
                HIWORD(Fixed->dwFileVersionLS), LOWORD(Fixed->dwFileVersionLS));
        return Buffer;
    }

    // A loaded module: its path, its base (ASLR: whether bases are stable per machine) and its
    // file version.
    void WriteModule(FOracleJson& J, char const* Name)
    {
        HMODULE const Module = GetModuleHandleA(Name);
        J.BeginObject(nullptr);
        J.Str("name", Name);
        if (Module == nullptr)
        {
            J.Bool("loaded", false);
            J.EndObject();
            return;
        }
        char Path[MAX_PATH] = {};
        GetModuleFileNameA(Module, Path, MAX_PATH);
        J.Bool("loaded", true);
        J.Str("path", Path);
        J.Str("base", Hex32((unsigned int)(UINT_PTR)Module).c_str());
        J.Str("version", FileVersion(Path).c_str());
        J.EndObject();
    }

    std::string RegString(char const* Value)
    {
        char Buffer[256] = {};
        DWORD Size = sizeof(Buffer);
        if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", Value,
                         RRF_RT_REG_SZ, nullptr, Buffer, &Size) != ERROR_SUCCESS)
            return std::string();
        return Buffer;
    }

    unsigned int RegDword(char const* Value)
    {
        DWORD Data = 0;
        DWORD Size = sizeof(Data);
        if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", Value,
                         RRF_RT_REG_DWORD, nullptr, &Data, &Size) != ERROR_SUCCESS)
            return 0;
        return Data;
    }
}

namespace oracle
{
    std::string OracleSimEnvRecord()
    {
        FOracleJson J;
        J.Str("rec", "env");
        // The compiler that built Oracle.exe; the solution builds every DLL with the same one.
        J.Uint("msc_full_ver", (unsigned int)_MSC_FULL_VER);
        J.Uint("msc_build", (unsigned int)_MSC_BUILD);
        WriteCpu(J);
        J.BeginObject("os");
        J.Str("build", RegString("CurrentBuild").c_str());
        J.Uint("ubr", RegDword("UBR"));
        J.Str("display_version", RegString("DisplayVersion").c_str());
        J.EndObject();
        J.Str("process_heap", Hex32((unsigned int)(UINT_PTR)GetProcessHeap()).c_str());
        // A local's address: the stack's placement (stack randomization) in this process.
        unsigned int StackProbe = 0;
        J.Str("stack_local", Hex32((unsigned int)(UINT_PTR)&StackProbe).c_str());
        J.BeginArray("modules");
        char const* const kModules[] = {"Oracle.exe",     "Server.dll",       "Common.dll",   "Content.dll",
                                        "ivp.dll",        "SimClientStub.dll", "ucrtbase.dll", "vcruntime140.dll",
                                        "msvcp140.dll",   "ntdll.dll",        "kernel32.dll", "KernelBase.dll"};
        for (char const* Name : kModules)
            WriteModule(J, Name);
        J.EndArray();
        return J.Finish();
    }
}
