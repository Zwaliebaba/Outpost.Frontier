// Helpers shared by the oracle suites; see oracle_support.h.

#include "oracle_support.h"

#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace oracle
{
    std::string GFixtureDir;
    std::string GOutputDir;
    int GDumps = 0;
    bool GFailed = false;

    void __cdecl OracleDump(int /*Flags*/, const char* /*Format*/, ...)
    {
        ++GDumps;
    }

    bool ReadWholeFile(const std::string& Path, Bytes& Out)
    {
        Out.clear();
        FILE* File = fopen(Path.c_str(), "rb");
        if (File == nullptr)
            return false;
        unsigned char Chunk[4096];
        size_t Got;
        while ((Got = fread(Chunk, 1, sizeof(Chunk), File)) > 0)
            Out.insert(Out.end(), Chunk, Chunk + Got);
        fclose(File);
        return true;
    }

    std::vector<std::string> InputLines(const char* Name)
    {
        std::vector<std::string> Lines;
        Bytes Data;
        if (!ReadWholeFile(Name, Data))
        {
            fprintf(stderr, "cannot read %s\n", Name);
            GFailed = true;
            return Lines;
        }
        std::string Current;
        for (size_t Index = 0; Index < Data.size(); ++Index)
        {
            const char Ch = (char)Data[Index];
            if (Ch == '\n')
            {
                if (Current.empty() || Current[0] != '#')
                    Lines.push_back(Current);
                Current.clear();
            }
            else if (Ch != '\r')
            {
                Current += Ch;
            }
        }
        if (!Current.empty() && Current[0] != '#')
            Lines.push_back(Current);
        return Lines;
    }

    int HexValue(char Ch)
    {
        if (Ch >= '0' && Ch <= '9')
            return Ch - '0';
        if (Ch >= 'a' && Ch <= 'f')
            return Ch - 'a' + 10;
        if (Ch >= 'A' && Ch <= 'F')
            return Ch - 'A' + 10;
        return 0;
    }

    Bytes Unhex(const std::string& Text)
    {
        Bytes Out;
        for (size_t Index = 0; Index + 1 < Text.size(); Index += 2)
            Out.push_back((unsigned char)(HexValue(Text[Index]) * 16 + HexValue(Text[Index + 1])));
        Out.push_back(0);
        return Out;
    }

    const char* CStr(const Bytes& Data)
    {
        return (const char*)Data.data();
    }

    std::string Hex(const void* Data, size_t Length)
    {
        static const char Digits[] = "0123456789abcdef";
        const unsigned char* Raw = (const unsigned char*)Data;
        std::string Out;
        for (size_t Index = 0; Index < Length; ++Index)
        {
            Out += Digits[Raw[Index] >> 4];
            Out += Digits[Raw[Index] & 15];
        }
        return Out;
    }

    std::string HexC(const char* Text)
    {
        return Text == nullptr ? std::string() : Hex(Text, strlen(Text));
    }

    std::string Format(const char* Pattern, ...)
    {
        static char Buffer[0x4000];
        va_list Args;
        va_start(Args, Pattern);
        _vsnprintf_s(Buffer, sizeof(Buffer), _TRUNCATE, Pattern, Args);
        va_end(Args);
        return Buffer;
    }

    unsigned long long DoubleBits(double Value)
    {
        unsigned long long Bits;
        memcpy(&Bits, &Value, sizeof(Bits));
        return Bits;
    }

    unsigned int FloatBits(float Value)
    {
        unsigned int Bits;
        memcpy(&Bits, &Value, sizeof(Bits));
        return Bits;
    }

    float FloatFromBits(unsigned int Bits)
    {
        float Value;
        memcpy(&Value, &Bits, sizeof(Value));
        return Value;
    }

    double DoubleFromBits(unsigned long long Bits)
    {
        double Value;
        memcpy(&Value, &Bits, sizeof(Value));
        return Value;
    }

    int Sign(int Value)
    {
        return Value < 0 ? -1 : (Value > 0 ? 1 : 0);
    }

    void MakeParentDirectories(const std::string& Path)
    {
        for (size_t Index = 1; Index < Path.size(); ++Index)
        {
            if ((Path[Index] == '\\' || Path[Index] == '/') && Path[Index - 1] != ':')
                CreateDirectoryA(Path.substr(0, Index).c_str(), nullptr);
        }
    }

    Suite::Suite(const char* Name) : File(nullptr)
    {
        const std::string Path = GOutputDir + "\\" + Name;
        MakeParentDirectories(Path);
        File = fopen(Path.c_str(), "wb");
        if (File == nullptr)
        {
            fprintf(stderr, "cannot write %s\n", Path.c_str());
            GFailed = true;
        }
    }

    Suite::Suite(const std::string& Path) : File(nullptr)
    {
        MakeParentDirectories(Path);
        File = fopen(Path.c_str(), "wb");
        if (File == nullptr)
        {
            fprintf(stderr, "cannot write %s\n", Path.c_str());
            GFailed = true;
        }
    }

    Suite::~Suite()
    {
        if (File != nullptr)
            fclose(File);
    }

    void Suite::Line(const std::string& Text)
    {
        if (File != nullptr)
            fprintf(File, "%s\n", Text.c_str());
    }
}
