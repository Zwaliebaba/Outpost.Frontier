// Oracle instrumentation (work package P0.5, design/reference-build.md section 8).

#pragma once

// One line of an oracle output file: a JSON object built in place. Floats are written as
// their IEEE bit pattern in 8 lower-case hex digits ("3f800000"), doubles in 16, so no
// comparison depends on decimal formatting (design/verification.md, "The oracle"). Narrow
// strings are written byte by byte, every byte >= 0x80 as \u00XX (Latin-1); wide strings
// unit by unit, every unit outside printable ASCII as \uXXXX. Byte blocks are one string of
// two lower-case hex digits per byte.
//
// A null Name writes a value without a key, for elements of an array.

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

class FOracleJson
{
public:
    FOracleJson()
    {
        Text.reserve(256);
        Text.push_back('{');
        NeedComma.push_back(false);
    }

    void Uint(char const* Name, unsigned Value)
    {
        char Buffer[16];
        sprintf(Buffer, "%u", Value);
        Sep(Name);
        Text += Buffer;
    }

    void Int(char const* Name, int Value)
    {
        char Buffer[16];
        sprintf(Buffer, "%d", Value);
        Sep(Name);
        Text += Buffer;
    }

    void Bool(char const* Name, bool Value)
    {
        Sep(Name);
        Text += Value ? "true" : "false";
    }

    void Null(char const* Name)
    {
        Sep(Name);
        Text += "null";
    }

    void Float(char const* Name, float Value)
    {
        unsigned Bits = 0;
        memcpy(&Bits, &Value, sizeof(Bits));
        char Buffer[16];
        sprintf(Buffer, "\"%08x\"", Bits);
        Sep(Name);
        Text += Buffer;
    }

    void Double(char const* Name, double Value)
    {
        unsigned Halves[2] = {0u, 0u};
        memcpy(Halves, &Value, sizeof(Halves));
        char Buffer[24];
        sprintf(Buffer, "\"%08x%08x\"", Halves[1], Halves[0]);
        Sep(Name);
        Text += Buffer;
    }

    void FloatArray(char const* Name, float const* Values, unsigned Count)
    {
        BeginArray(Name);
        for (unsigned Index = 0; Index < Count; ++Index)
            Float(nullptr, Values[Index]);
        EndArray();
    }

    // A null Value writes null.
    void Str(char const* Name, char const* Value)
    {
        if (Value == nullptr)
        {
            Null(Name);
            return;
        }
        StrN(Name, Value, static_cast<size_t>(-1));
    }

    // At most Max bytes, stopping at the first zero.
    void StrN(char const* Name, char const* Value, size_t Max)
    {
        if (Value == nullptr)
        {
            Null(Name);
            return;
        }
        Sep(Name);
        Text.push_back('"');
        for (size_t Index = 0; Index < Max && Value[Index] != '\0'; ++Index)
            PutUnit(static_cast<unsigned char>(Value[Index]));
        Text.push_back('"');
    }

    void WStr(char const* Name, wchar_t const* Value)
    {
        if (Value == nullptr)
        {
            Null(Name);
            return;
        }
        WStrN(Name, Value, static_cast<size_t>(-1));
    }

    void WStrN(char const* Name, wchar_t const* Value, size_t Max)
    {
        if (Value == nullptr)
        {
            Null(Name);
            return;
        }
        Sep(Name);
        Text.push_back('"');
        for (size_t Index = 0; Index < Max && Value[Index] != L'\0'; ++Index)
            PutUnit(static_cast<unsigned>(static_cast<unsigned short>(Value[Index])));
        Text.push_back('"');
    }

    // A null Data writes null; a zero Size writes "".
    void Bytes(char const* Name, void const* Data, size_t Size)
    {
        if (Data == nullptr)
        {
            Null(Name);
            return;
        }
        static char const kHex[] = "0123456789abcdef";
        Sep(Name);
        Text.push_back('"');
        unsigned char const* const Octets = static_cast<unsigned char const*>(Data);
        for (size_t Index = 0; Index < Size; ++Index)
        {
            Text.push_back(kHex[Octets[Index] >> 4]);
            Text.push_back(kHex[Octets[Index] & 0xf]);
        }
        Text.push_back('"');
    }

    void BeginObject(char const* Name)
    {
        Sep(Name);
        Text.push_back('{');
        NeedComma.push_back(false);
    }

    void EndObject()
    {
        Text.push_back('}');
        NeedComma.pop_back();
    }

    void BeginArray(char const* Name)
    {
        Sep(Name);
        Text.push_back('[');
        NeedComma.push_back(false);
    }

    void EndArray()
    {
        Text.push_back(']');
        NeedComma.pop_back();
    }

    // Closes every open array and object and returns the line, without a newline.
    std::string const& Finish()
    {
        while (NeedComma.size() > 1)
        {
            // Unbalanced callers are a bug; close as objects to keep the line parseable.
            Text.push_back('}');
            NeedComma.pop_back();
        }
        if (!NeedComma.empty())
        {
            Text.push_back('}');
            NeedComma.pop_back();
        }
        return Text;
    }

private:
    void Sep(char const* Name)
    {
        if (NeedComma.back())
            Text.push_back(',');
        NeedComma.back() = true;
        if (Name != nullptr)
        {
            Text.push_back('"');
            Text += Name;
            Text += "\":";
        }
    }

    void PutUnit(unsigned Unit)
    {
        if (Unit == '"' || Unit == '\\')
        {
            Text.push_back('\\');
            Text.push_back(static_cast<char>(Unit));
        }
        else if (Unit >= 0x20 && Unit < 0x7f)
        {
            Text.push_back(static_cast<char>(Unit));
        }
        else
        {
            char Buffer[8];
            sprintf(Buffer, "\\u%04x", Unit & 0xffffu);
            Text += Buffer;
        }
    }

    std::string Text;
    std::vector<bool> NeedComma;
};
