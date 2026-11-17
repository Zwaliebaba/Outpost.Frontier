// Oracle suites of work package P1.9 (net): the wire primitives and the
// quantisers of RemoteCommon/, as RemoteClient.dll compiles them (see
// oracle_net_codecs.h). Inputs are in net/ under the fixture directory,
// written by make_net_fixtures.py; the Rust test
// crates/neuron-core/tests/oracle_net.rs renders the same suites.

#include "oracle_support.h"

#include "oracle_net_codecs.h"

#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

using namespace oracle;

namespace
{
    // FLPACKET_SERVER_CHARSELECTVERIFIED's first field in net_doubles.
    const unsigned int RepInstance = 0x04030201u;

    unsigned int ParseHex32(const std::string& Text)
    {
        return (unsigned int)strtoul(Text.c_str(), nullptr, 16);
    }

    unsigned long long ParseHex64(const std::string& Text)
    {
        return _strtoui64(Text.c_str(), nullptr, 16);
    }

    std::vector<std::string> Split(const std::string& Text)
    {
        std::vector<std::string> Parts;
        size_t Start = 0;
        for (;;)
        {
            const size_t Space = Text.find(' ', Start);
            Parts.push_back(Text.substr(Start, Space == std::string::npos ? std::string::npos : Space - Start));
            if (Space == std::string::npos)
                return Parts;
            Start = Space + 1;
        }
    }

    // UTF-16 units spelled as four hex digits each, plus a terminating NUL.
    std::vector<wchar_t> Units(const std::string& Text)
    {
        std::vector<wchar_t> Out;
        for (size_t Index = 0; Index + 3 < Text.size(); Index += 4)
            Out.push_back((wchar_t)ParseHex32(Text.substr(Index, 4)));
        Out.push_back(L'\0');
        return Out;
    }

    std::string WideHex(const std::vector<unsigned short>& Units)
    {
        std::string Out;
        for (size_t Index = 0; Index < Units.size(); ++Index)
            Out += Format("%04x", Units[Index]);
        return Out;
    }

    std::string BytesHex(const std::vector<unsigned char>& Data)
    {
        return Data.empty() ? std::string() : Hex(&Data[0], Data.size());
    }

    void Quantise()
    {
        Suite Out("net_quantise.jsonl");
        for (const std::string& Line : InputLines("net/quantise.hex"))
        {
            const unsigned int Bits = ParseHex32(Line);
            const unsigned char TierXy = oracle_net::FirePickTier(Bits, true);
            const unsigned char TierZ = oracle_net::FirePickTier(Bits, false);

            unsigned char Fire[4] = {0, 0, 0, 0};
            oracle_net::WireContext* Writer = oracle_net::BeginWrite(Fire, sizeof(Fire));
            if (oracle_net::PutFireComponent(Writer, Bits, TierXy) != 0)
                GFailed = true;
            const unsigned int FireLength = oracle_net::Position(Writer);
            oracle_net::End(Writer);
            unsigned int FireBack = 0;
            oracle_net::WireContext* Reader = oracle_net::BeginRead(Fire, FireLength);
            if (oracle_net::GetFireComponent(Reader, TierXy, &FireBack) != 0)
                GFailed = true;
            oracle_net::End(Reader);

            unsigned char Status[16];
            unsigned int StatusLength = 0;
            if (oracle_net::EncodeHullStatus(Bits, Status, sizeof(Status), &StatusLength) != 0)
                GFailed = true;

            Out.Line(Format("{\"in\":\"%s\",\"unit\":\"%02x\",\"update\":\"%02x\",\"fraction\":\"%02x\","
                            "\"tenths\":\"%02x\",\"tier\":\"%d%d\",\"small\":\"%02x\",\"medium\":\"%04x\","
                            "\"fire\":\"%s\",\"fire_back\":\"%08x\",\"hull_status\":\"%s\"}",
                            Line.c_str(), (unsigned int)oracle_net::Quantize127(Bits),
                            (unsigned int)oracle_net::UpdateQuantize(Bits),
                            (unsigned int)oracle_net::Quantize255(Bits),
                            (unsigned int)oracle_net::SendCommQuantize(Bits), (int)TierXy, (int)TierZ,
                            oracle_net::FireQuantize(Bits, false) & 0xffu,
                            oracle_net::FireQuantize(Bits, true) & 0xffffu, Hex(Fire, FireLength).c_str(),
                            FireBack, Hex(Status, StatusLength).c_str()));
        }
    }

    void Expand()
    {
        Suite Out("net_expand.jsonl");
        for (const std::string& Line : InputLines("net/expand.hex"))
        {
            const unsigned int Word = ParseHex32(Line);
            const unsigned char Byte = (unsigned char)(Word & 0xffu);
            const unsigned char Pair[2] = {Byte, (unsigned char)(Word >> 8)};

            unsigned int Small = 0;
            oracle_net::WireContext* Reader = oracle_net::BeginRead(Pair, 1);
            if (oracle_net::GetFireComponent(Reader, 0x01u, &Small) != 0)
                GFailed = true;
            oracle_net::End(Reader);
            unsigned int Medium = 0;
            Reader = oracle_net::BeginRead(Pair, 2);
            if (oracle_net::GetFireComponent(Reader, 0x02u, &Medium) != 0)
                GFailed = true;
            oracle_net::End(Reader);

            Out.Line(Format("{\"in\":\"%s\",\"unit\":\"%08x\",\"update\":\"%08x\",\"fraction\":\"%08x\","
                            "\"tenths\":\"%08x\",\"small\":\"%08x\",\"medium\":\"%08x\",\"hull\":\"%08x\"}",
                            Line.c_str(), oracle_net::Expand127(Byte), oracle_net::UpdateExpand(Byte),
                            oracle_net::Expand255(Byte), oracle_net::SendCommExpand(Byte), Small, Medium,
                            (unsigned int)oracle_net::ExpandHull(Byte)));
        }
    }

    void Hull()
    {
        Suite Out("net_hull.jsonl");
        for (const std::string& Line : InputLines("net/hull.hex"))
        {
            const int HitPts = (int)ParseHex32(Line);
            Out.Line(Format("{\"in\":\"%s\",\"hull\":\"%02x\"}", Line.c_str(),
                            (unsigned int)oracle_net::QuantizeHull(HitPts)));
        }
    }

    void Doubles()
    {
        Suite Out("net_doubles.jsonl");
        for (const std::string& Line : InputLines("net/doubles.hex"))
        {
            unsigned char Packet[32];
            unsigned int Length = 0;
            if (oracle_net::EncodeCharSelectVerified(RepInstance, ParseHex64(Line), Packet, sizeof(Packet),
                                                     &Length) != 0)
                GFailed = true;
            Out.Line(Format("{\"in\":\"%s\",\"packet\":\"%s\"}", Line.c_str(), Hex(Packet, Length).c_str()));
        }
    }

    void Header()
    {
        Suite Out("net_header.jsonl");
        for (const std::string& Line : InputLines("net/header.hex"))
        {
            unsigned char Packet[16];
            unsigned int Length = 0;
            if (oracle_net::EncodeHeader(ParseHex32(Line), Packet, sizeof(Packet), &Length) != 0)
                GFailed = true;
            Out.Line(Format("{\"in\":\"%s\",\"header\":\"%s\"}", Line.c_str(), Hex(Packet, Length).c_str()));
        }
    }

    // One writer op; 0 or the C++ code.
    unsigned int WriteOp(oracle_net::WireContext* Writer, const std::string& Name, const std::string& Arg)
    {
        if (Name == "u8")
        {
            const unsigned char Value = (unsigned char)ParseHex32(Arg);
            return oracle_net::Put(Writer, &Value, 1u);
        }
        if (Name == "u16")
        {
            const unsigned short Value = (unsigned short)ParseHex32(Arg);
            return oracle_net::Put(Writer, &Value, 2u);
        }
        if (Name == "u32" || Name == "f32")
        {
            const unsigned int Value = ParseHex32(Arg);
            return oracle_net::Put(Writer, &Value, 4u);
        }
        if (Name == "f32x")
            return oracle_net::PutFloat(Writer, ParseHex32(Arg));
        if (Name == "vec")
        {
            const unsigned int Bits[3] = {ParseHex32(Arg.substr(0, 8)), ParseHex32(Arg.substr(8, 8)),
                                          ParseHex32(Arg.substr(16, 8))};
            return oracle_net::PutVector(Writer, Bits);
        }
        if (Name == "bytes" || Name == "b16" || Name == "b32")
        {
            const Bytes Data = Unhex(Arg); // plus a NUL
            const unsigned int Size = (unsigned int)Data.size() - 1u;
            if (Name == "b16")
            {
                const unsigned short Count = (unsigned short)Size;
                const unsigned int Result = oracle_net::Put(Writer, &Count, 2u);
                if (Result != 0)
                    return Result;
            }
            else if (Name == "b32")
            {
                const unsigned int Result = oracle_net::Put(Writer, &Size, 4u);
                if (Result != 0)
                    return Result;
            }
            return oracle_net::PutBlock(Writer, &Data[0], Size);
        }
        if (Name == "wide16")
            return oracle_net::PutWideString(Writer, &Units(Arg)[0]);
        if (Name == "text16")
        {
            const std::vector<wchar_t> Text = Units(Arg);
            return oracle_net::PutWideText(Writer, &Text[0], (unsigned int)Text.size() - 1u);
        }
        fprintf(stderr, "net/write.txt: unknown op %s\n", Name.c_str());
        GFailed = true;
        return 0xffu;
    }

    void Write()
    {
        Suite Out("net_write.jsonl");
        for (const std::string& Line : InputLines("net/write.txt"))
        {
            const std::vector<std::string> Words = Split(Line);
            const unsigned int Limit = (unsigned int)strtoul(Words[0].c_str(), nullptr, 10);
            // Room past the limit: no op in the fixtures writes there.
            std::vector<unsigned char> Buffer(Limit + 4096u, 0);
            oracle_net::WireContext* Writer = oracle_net::BeginWrite(&Buffer[0], Limit);
            unsigned int Result = 0;
            int Failed = -1;
            unsigned int Length = 0;
            for (size_t Index = 1; Index < Words.size(); ++Index)
            {
                const size_t Colon = Words[Index].find(':');
                const std::string Name = Words[Index].substr(0, Colon);
                const std::string Arg = Colon == std::string::npos ? std::string() : Words[Index].substr(Colon + 1);
                const unsigned int Start = oracle_net::Position(Writer);
                const unsigned int Code = WriteOp(Writer, Name, Arg);
                if (Code != 0)
                {
                    Result = Code;
                    Failed = (int)Index - 1;
                    Length = Start;
                    break;
                }
            }
            if (Failed < 0)
                Length = oracle_net::Position(Writer);
            oracle_net::End(Writer);
            Out.Line(Format("{\"in\":\"%s\",\"result\":\"%02x\",\"failed\":\"%d\",\"out\":\"%s\"}", Line.c_str(),
                            Result, Failed, Hex(&Buffer[0], Length).c_str()));
        }
    }

    // One reader op; 0 or the C++ code. Value receives what was read.
    unsigned int ReadOp(oracle_net::WireContext* Reader, const std::string& Op, std::string& Value)
    {
        const size_t Colon = Op.find(':');
        const std::string Name = Op.substr(0, Colon);
        const std::string Arg = Colon == std::string::npos ? std::string() : Op.substr(Colon + 1);
        unsigned int Result = 0;
        if (Name == "u8")
        {
            unsigned char Read = 0;
            Result = oracle_net::Get(Reader, &Read, 1u);
            Value = Format("%02x", (unsigned int)Read);
        }
        else if (Name == "u16")
        {
            unsigned short Read = 0;
            Result = oracle_net::Get(Reader, &Read, 2u);
            Value = Format("%04x", (unsigned int)Read);
        }
        else if (Name == "u32" || Name == "f32")
        {
            unsigned int Read = 0;
            Result = oracle_net::Get(Reader, &Read, 4u);
            Value = Format("%08x", Read);
        }
        else if (Name == "f64")
        {
            unsigned long long Read = 0;
            Result = oracle_net::Get(Reader, &Read, 8u);
            Value = Format("%016llx", Read);
        }
        else if (Name == "vec")
        {
            unsigned int Bits[3] = {0, 0, 0};
            Result = oracle_net::GetVector(Reader, Bits);
            Value = Format("%08x%08x%08x", Bits[0], Bits[1], Bits[2]);
        }
        else if (Name == "bytes")
        {
            const unsigned int Count = (unsigned int)strtoul(Arg.c_str(), nullptr, 10);
            std::vector<unsigned char> Read(Count + 1u, 0);
            Result = oracle_net::Get(Reader, &Read[0], Count);
            Value = Hex(&Read[0], Count);
        }
        else if (Name == "wide16" || Name == "text16")
        {
            std::vector<unsigned short> Read;
            Result = Name == "wide16" ? oracle_net::GetWideString(Reader, Read) : oracle_net::GetWideText(Reader, Read);
            Value = WideHex(Read);
        }
        else if (Name == "b32")
        {
            std::vector<unsigned char> Read;
            Result = oracle_net::GetOwnedBlock(Reader, Read);
            Value = BytesHex(Read);
        }
        else
        {
            fprintf(stderr, "net/read.txt: unknown op %s\n", Name.c_str());
            GFailed = true;
            Result = 0xffu;
        }
        return Result;
    }

    void Read()
    {
        Suite Out("net_read.jsonl");
        for (const std::string& Line : InputLines("net/read.txt"))
        {
            const size_t Space = Line.find(' ');
            const Bytes Data = Unhex(Line.substr(0, Space)); // plus a NUL
            const unsigned int Size = (unsigned int)Data.size() - 1u;
            // Room past the end: no op in the fixtures reads there.
            std::vector<unsigned char> Buffer(Data.begin(), Data.end());
            Buffer.resize(Buffer.size() + 4096u, 0);
            const std::vector<std::string> Ops = Split(Line.substr(Space + 1));
            oracle_net::WireContext* Reader = oracle_net::BeginRead(&Buffer[0], Size);
            std::string Values;
            unsigned int Result = 0;
            int Failed = -1;
            for (size_t Index = 0; Index < Ops.size(); ++Index)
            {
                std::string Value;
                const unsigned int Code = ReadOp(Reader, Ops[Index], Value);
                if (Code != 0)
                {
                    Result = Code;
                    Failed = (int)Index;
                    break;
                }
                if (Index != 0)
                    Values += '.';
                Values += Value;
            }
            const std::string Position = Failed < 0 ? Format("%u", oracle_net::Position(Reader)) : std::string("-");
            oracle_net::End(Reader);
            Out.Line(Format("{\"in\":\"%s\",\"values\":\"%s\",\"result\":\"%02x\",\"failed\":\"%d\",\"pos\":\"%s\"}",
                            Line.c_str(), Values.c_str(), Result, Failed, Position.c_str()));
        }
    }
}

void RunNetSuites()
{
    Quantise();
    Expand();
    Hull();
    Doubles();
    Header();
    Write();
    Read();
}
