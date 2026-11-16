// Oracle suites of work package P1.5: the MD5 text form of MD5Hash
// (NeuronRuntime/md5hash.cpp). Called from RunUtilSuites in oracle_util.cpp.
//
// The harness does not link NeuronRuntime.dll, so md5hash.cpp is compiled
// into this file. NEURONRUNTIME_EXPORTS makes md5hash.h declare the class
// for export, which a definition in this module requires.

#define NEURONRUNTIME_EXPORTS
#include "../NeuronRuntime/md5hash.cpp"
#undef NEURONRUNTIME_EXPORTS

#include <stdio.h>
#include <string.h>

#include <string>

#include "oracle_support.h"

using namespace oracle;

namespace
{
    // The text after AddData in pieces of Chunk bytes (0: one call), then
    // CalcValue.
    std::string Md5Text(const unsigned char* Data, size_t Length, size_t Chunk)
    {
        MD5Hash Hash;
        if (Chunk == 0)
        {
            Hash.AddData((void*)Data, (unsigned long)Length);
        }
        else
        {
            for (size_t Offset = 0; Offset < Length; Offset += Chunk)
            {
                const size_t Piece = Length - Offset < Chunk ? Length - Offset : Chunk;
                Hash.AddData((void*)(Data + Offset), (unsigned long)Piece);
            }
        }
        Hash.CalcValue();
        return Hash.AsString();
    }
}

void RunMd5Suites()
{
    {
        Suite Out("md5_text.jsonl");
        for (const std::string& Line : InputLines("util/md5.hex"))
        {
            const Bytes Data = Unhex(Line);
            const size_t Length = Data.size() - 1;
            const std::string Text = Md5Text(Data.data(), Length, 0);
            const std::string Sig = Md5Text((const unsigned char*)Text.c_str(), Text.size(), 0);
            Out.Line("{\"in\":\"" + Line + "\",\"text\":\"" + Text + "\",\"sig\":\"" + Sig + "\",\"c7\":\"" +
                     Md5Text(Data.data(), Length, 7) + "\",\"c64\":\"" + Md5Text(Data.data(), Length, 64) + "\"}");
        }
    }
    {
        Suite Out("md5_parse.jsonl");
        for (const std::string& Line : InputLines("util/md5_parse.hex"))
        {
            const Bytes Text = Unhex(Line);
            MD5Hash Hash;
            Hash.FromString(CStr(Text));
            Out.Line("{\"in\":\"" + Line + "\",\"text\":\"" + Hash.AsString() + "\"}");
        }
    }
}
