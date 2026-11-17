// Helpers shared by the oracle suites (design/migration-plan.md P0.4).
//
// oracle_main.cpp holds the P1.1 and P1.2 suites and main(). A later work
// package adds its suites in oracle_<module>.cpp, declares its entry point
// at the end of this header and calls it from main(). Every suite writes
// one JSON object per line to a file in the output directory; the Rust
// test crates/neuron-core/tests/oracle_<module>.rs renders the same file.

#pragma once

#include <stdio.h>

#include <string>
#include <vector>

namespace oracle
{
    // The fixture directory (the current directory while suites run) and
    // the output directory, both absolute.
    extern std::string GFixtureDir;
    extern std::string GOutputDir;
    // Calls into FDUMP since the counter was last reset.
    extern int GDumps;
    // Set by any suite that cannot read an input or write an output.
    extern bool GFailed;

    void __cdecl OracleDump(int Flags, const char* Format, ...);

    typedef std::vector<unsigned char> Bytes;

    bool ReadWholeFile(const std::string& Path, Bytes& Out);

    // The non-comment lines of an input list; empty lines are inputs too.
    std::vector<std::string> InputLines(const char* Name);

    int HexValue(char Ch);

    // Decoded bytes plus a terminating NUL, so data() is a C string.
    Bytes Unhex(const std::string& Text);

    const char* CStr(const Bytes& Data);

    // Lower-case hex of Length bytes.
    std::string Hex(const void* Data, size_t Length);

    // Lower-case hex of a C string; empty for a null pointer.
    std::string HexC(const char* Text);

    // printf into a std::string (at most 16 KiB).
    std::string Format(const char* Pattern, ...);

    unsigned long long DoubleBits(double Value);
    unsigned int FloatBits(float Value);
    float FloatFromBits(unsigned int Bits);
    double DoubleFromBits(unsigned long long Bits);

    int Sign(int Value);

    void MakeParentDirectories(const std::string& Path);

    class Suite
    {
    public:
        // A suite file in the output directory.
        explicit Suite(const char* Name);

        // A file at a full path; missing directories are created.
        explicit Suite(const std::string& Path);

        ~Suite();

        void Line(const std::string& Text);

    private:
        Suite(const Suite&);
        Suite& operator=(const Suite&);

        FILE* File;
    };
}

// Suite entry points of later work packages, called from main() in order.

// P1.4, oracle_math.cpp: neuron-core::math against x86math.dll.
void RunMathSuites();

// Oracle.exe catalog <resource DLL directory> <output directory>
// (oracle_catalog.cpp, work package P1.6): a separate mode, not a suite.
// Its output is game text and is never committed.
void RunCatalogDump();

// P1.9, oracle_net.cpp: wire primitives and quantisers.
void RunNetSuites();

// P1.5 (oracle_util.cpp): rect, spline, MD5 text form, Timing::Delta, ErrorHandler.
void RunUtilSuites();

// P1.3 (oracle_vfs.cpp): the UTF container and the file systems of ReadFile.
void RunVfsSuites();
