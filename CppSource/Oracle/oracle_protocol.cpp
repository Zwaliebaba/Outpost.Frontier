// Oracle suites of work package P2.2 (protocol), Common.dll part: FmtStr
// flatten/unflatten, the binary RDL reader and writer, and MarshalBuffer.
// crates/game-sim/tests/oracle_protocol.rs renders the same lines; each case
// below names the Rust function it mirrors.

#include "oracle_support.h"

#include "oracle_protocol.h"

#include "common_goods.h"
#include "common_misc.h"
#include "common_net.h"
#include "common_reputation.h"
#include "common_text.h"

#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

using namespace oracle;
using oracle_protocol::Gen;

namespace oracle_protocol
{
    void SetMode(Mode Which)
    {
        switch (Which)
        {
        case ModeServer:
            SetIsMPServer(true); // also leaves single player
            break;
        case ModeClient:
            SetIsMPServer(false);
            SetMultiPlayer(true);
            break;
        case ModeSingle:
            SetIsMPServer(false);
            SetMultiPlayer(false);
            break;
        }
    }

    const char* ModeName(Mode Which)
    {
        switch (Which)
        {
        case ModeServer:
            return "mps";
        case ModeClient:
            return "mpc";
        default:
            return "sp";
        }
    }

    void RestoreMode()
    {
        SetIsMPServer(false);
        SetMultiPlayer(false);
    }
}

namespace
{
    std::string Dec(unsigned int Value)
    {
        return Format("%u", Value);
    }

    std::string BytesHex(const std::vector<unsigned char>& Data)
    {
        return Data.empty() ? std::string() : Hex(&Data[0], Data.size());
    }

    std::string WideHex(const wchar_t* Text, size_t Count)
    {
        std::string Out;
        for (size_t Index = 0; Index < Count; ++Index)
            Out += Format("%04x", (unsigned int)(unsigned short)Text[Index]);
        return Out;
    }

    // --- FmtStr (fmt_case) -----------------------------------------------------

    void FmtStrCase(unsigned int Case, Gen& G, FmtStr& Out)
    {
        Out.begin_mad_lib(G.U32());
        if (Case == 0)
            return;
        if (Case == 1)
        {
            unsigned int Value;
            int Signed;
            Value = G.U32(); Out.append_system(Value);
            Value = G.U32(); Out.append_int(Value);
            Value = G.U32(); Out.append_string(Value);
            Value = G.U32(); Out.append_base(Value);
            Value = G.U32(); Out.append_good(Value);
            Signed = G.I32(); Out.append_rep_instance(Signed);
            Value = G.U32(); Out.append_rep_group(Value);
            Value = G.U32(); Out.append_zone_id(Value);
            Value = G.U32(); Out.append_spaceobj_id(Value);
            Signed = G.I32(); Out.append_installation(Signed);
            Signed = G.I32(); Out.append_loot(Signed);
            Value = G.U32(); Out.append_system(Value);
            FmtStr::NavMarker Marker;
            for (int Index = 0; Index < 5; ++Index)
                Marker.Data[Index] = G.U32();
            Out.append_nav_marker(Marker);
            return;
        }
        FmtStr Inner;
        Inner.begin_mad_lib(G.U32());
        unsigned int Value = G.U32();
        Inner.append_int(Value);
        Value = G.U32();
        Inner.append_system(Value);
        FmtStr Middle;
        Middle.begin_mad_lib(G.U32());
        Middle.append_fmt_str(Inner);
        const int Loot = G.I32();
        Middle.append_loot(Loot);
        Out.append_fmt_str(Inner);
        Value = G.U32();
        Out.append_int(Value);
        Out.append_fmt_str(Middle);
    }

    std::vector<unsigned char> Flatten(const FmtStr& Text)
    {
        unsigned int Size = 6;
        for (const FmtStr::Val* Node = Text.Head; Node != nullptr; Node = Node->Next)
            Size += Node->get_flattened_size();
        std::vector<unsigned char> Out(Size + 64u, 0);
        const int Written = Text.flatten(&Out[0], Size);
        Out.resize((size_t)Written);
        return Out;
    }

    void FmtStrSuite()
    {
        Suite Out("game-sim\\protocol_fmtstr.jsonl");
        for (unsigned int Case = 0; Case < 3; ++Case)
        {
            Gen G = Gen::ForCase(0xf3u, Case);
            FmtStr Text;
            FmtStrCase(Case, G, Text);
            const std::vector<unsigned char> Flat = Flatten(Text);
            unsigned int Size = 6;
            for (const FmtStr::Val* Node = Text.Head; Node != nullptr; Node = Node->Next)
                Size += Node->get_flattened_size();
            std::vector<unsigned char> Input(Flat);
            Input.resize(Input.size() + 64u, 0);
            FmtStr Back;
            const int Used = Back.unflatten(&Input[0], 0);
            const std::vector<unsigned char> Again = Flatten(Back);
            Out.Line("{\"in\":\"fmtstr " + Dec(Case) + "\",\"flat\":\"" + BytesHex(Flat) + "\",\"size\":\"" +
                     Dec(Size) + "\",\"used\":\"" + Dec((unsigned int)Used) + "\",\"back\":\"" + BytesHex(Again) +
                     "\"}");
        }
    }

    // --- Binary RDL (rdl_case) ---------------------------------------------------

    void Put32(std::vector<unsigned char>& Out, unsigned int Value)
    {
        for (int Index = 0; Index < 4; ++Index)
            Out.push_back((unsigned char)(Value >> (8 * Index)));
    }

    void Put16(std::vector<unsigned char>& Out, unsigned int Value)
    {
        Out.push_back((unsigned char)Value);
        Out.push_back((unsigned char)(Value >> 8));
    }

    void RdlHeader(std::vector<unsigned char>& Out, int Kind, unsigned int Length)
    {
        Put32(Out, (unsigned int)Kind);
        Put32(Out, Length);
    }

    void RdlText(std::vector<unsigned char>& Out, Gen& G, unsigned int Count)
    {
        RdlHeader(Out, 2, Count * 2u + 2u);
        for (unsigned int Index = 0; Index < Count; ++Index)
            Put16(Out, G.TextUnit());
        Put16(Out, 0);
    }

    unsigned int FloatBitsOf(float Value)
    {
        unsigned int Bits;
        std::memcpy(&Bits, &Value, 4u);
        return Bits;
    }

    std::vector<unsigned char> RdlCase(unsigned int Case, Gen& G)
    {
        std::vector<unsigned char> Out;
        if (Case == 1)
        {
            RdlHeader(Out, 1, 8);
            Put32(Out, G.U32());
            Put32(Out, G.U32());
            RdlText(Out, G, 3);
            RdlHeader(Out, 4, 9);
            Out.push_back((unsigned char)(G.U8() & 3u));
            Put32(Out, FloatBitsOf(G.F32()));
            Put32(Out, FloatBitsOf(G.F32()));
            RdlHeader(Out, 5, 0);
            RdlHeader(Out, 6, 2);
            Put16(Out, G.U16());
            RdlText(Out, G, 0);
        }
        else if (Case == 2)
        {
            RdlText(Out, G, 5);
            RdlHeader(Out, 3, 4);
            for (int Index = 0; Index < 4; ++Index)
                Out.push_back(G.U8());
            RdlHeader(Out, 9, 1);
            Out.push_back(G.U8());
            RdlText(Out, G, 2);
        }
        else if (Case == 3)
        {
            RdlHeader(Out, 5, 2);
            Put16(Out, 0);
            RdlHeader(Out, 2, 0);
            RdlHeader(Out, 6, 2);
            Put16(Out, G.U16());
            RdlText(Out, G, 7);
        }
        return Out;
    }

    std::string RdlWrite(const RenderDisplayList& Rdl, unsigned int MaxSize, bool& Ok)
    {
        std::vector<char> Buffer(0x1000u + 64u, 0);
        unsigned int Size = 0;
        BinaryRDLWriter Writer;
        Ok = Writer.write_buffer(Rdl, &Buffer[0], MaxSize, Size);
        return Size == 0 ? std::string() : Hex(&Buffer[0], Size);
    }

    std::string RdlExtract(const std::vector<unsigned char>& Input, unsigned int MaxSize, bool& Ok)
    {
        std::vector<wchar_t> Text(MaxSize + 64u, 0);
        unsigned int Unused = 0;
        BinaryRDLReader Reader;
        const char* Data = Input.empty() ? "" : (const char*)&Input[0];
        Ok = Reader.extract_text_from_buffer(&Text[0], MaxSize, Unused, Data, (unsigned int)Input.size());
        return WideHex(&Text[0], wcslen(&Text[0]));
    }

    void RdlSuite()
    {
        Suite Out("game-sim\\protocol_rdl.jsonl");
        for (unsigned int Case = 0; Case < 4; ++Case)
        {
            Gen G = Gen::ForCase(0xd1u, Case);
            const std::vector<unsigned char> Input = RdlCase(Case, G);
            // Room after the input: no case reads past it.
            std::vector<unsigned char> Padded(Input);
            Padded.resize(Padded.size() + 64u, 0);
            RenderDisplayList Rdl;
            BinaryRDLReader Reader;
            const bool ReadOk = Reader.read_buffer(Rdl, (const char*)&Padded[0], (unsigned int)Input.size());
            bool FullOk = false;
            bool PartOk = false;
            bool ShortOk = false;
            bool LongOk = false;
            const std::string Full = RdlWrite(Rdl, 0x1000u, FullOk);
            const std::string Part = RdlWrite(Rdl, 0x18u, PartOk);
            const std::string Short = RdlExtract(Input, 4u, ShortOk);
            const std::string Long = RdlExtract(Input, 100u, LongOk);
            Out.Line("{\"in\":\"" + BytesHex(Input) + "\",\"read\":\"" + Dec(ReadOk ? 1u : 0u) + "\",\"write\":\"" +
                     Full + "\",\"write_ok\":\"" + Dec(FullOk ? 1u : 0u) + "\",\"part\":\"" + Part +
                     "\",\"part_ok\":\"" + Dec(PartOk ? 1u : 0u) + "\",\"text4\":\"" + Short + "\",\"text4_ok\":\"" +
                     Dec(ShortOk ? 1u : 0u) + "\",\"text\":\"" + Long + "\",\"text_ok\":\"" + Dec(LongOk ? 1u : 0u) +
                     "\"}");
        }
    }

    // --- MarshalBuffer (protocol_marshal) ----------------------------------------

    std::string Written(const MarshalBuffer& Buffer)
    {
        const int Count = Buffer.get_num_written();
        return Count == 0 ? std::string() : Hex(Buffer.get_buffer(), (size_t)Count);
    }

    std::string StringValue(char* Text)
    {
        if (Text == nullptr)
            return "null";
        const std::string Out = HexC(Text);
        std::free(Text);
        return Out;
    }

    std::string WideValue(wchar_t* Text)
    {
        if (Text == nullptr)
            return "null";
        const std::string Out = WideHex(Text, wcslen(Text));
        std::free(Text);
        return Out;
    }

    void MarshalSuite()
    {
        Suite Out("game-sim\\protocol_marshal.jsonl");
        static unsigned char Storage[0x1000];
        static unsigned char BackStorage[0x1000];

        // Case 0: scalars.
        {
            Gen G = Gen::ForCase(0x3au, 0);
            const unsigned char B = G.U8();
            const unsigned short S = G.U16();
            const unsigned int D = G.U32();
            const int I = G.I32();
            const float F = G.F32();
            const unsigned int Id = G.U32();
            std::memset(Storage, 0, sizeof(Storage));
            MarshalBuffer W;
            W.use_write_buffer(Storage, (int)sizeof(Storage));
            W.put_U8(B);
            W.put_U16(S);
            W.put_U32(D);
            W.put_int(I);
            W.put_float(F);
            W.put_bool(true);
            W.put_bool(false);
            W.put_FL_ID(Id);
            const std::string Encoding = Written(W);
            MarshalBuffer R;
            R.use_read_buffer(Storage, W.get_num_written());
            const unsigned int RB = R.get_U8();
            const unsigned int RS = R.get_U16();
            const unsigned int RD = (unsigned int)R.get_U32();
            const unsigned int RI = (unsigned int)R.get_int();
            const unsigned int RF = FloatBitsOf(R.get_float());
            const unsigned int B1 = R.get_bool() ? 1u : 0u;
            const unsigned int B2 = R.get_bool() ? 1u : 0u;
            const unsigned int RId = R.get_FL_ID();
            Out.Line("{\"in\":\"marshal 0\",\"out\":\"" + Encoding + "\",\"values\":\"" +
                     Format("%02x.%04x.%08x.%08x.%08x.%u.%u.%08x", RB, RS, RD, RI, RF, B1, B2, RId) + "\"}");
        }

        // Case 1: strings.
        {
            Gen G = Gen::ForCase(0x3au, 1);
            char Text[6];
            for (int Index = 0; Index < 5; ++Index)
                Text[Index] = (char)G.TextByte();
            Text[5] = '\0';
            std::memset(Storage, 0, sizeof(Storage));
            MarshalBuffer W;
            W.use_write_buffer(Storage, (int)sizeof(Storage));
            W.put_string(nullptr);
            W.put_string("");
            W.put_string("abc");
            W.put_wstring(nullptr);
            W.put_wstring(L"Hi!");
            W.put_string(Text);
            const std::string Encoding = Written(W);
            MarshalBuffer R;
            R.use_read_buffer(Storage, W.get_num_written());
            std::string Values = StringValue(R.get_string());
            Values += "." + StringValue(R.get_string());
            char Small[3];
            R.get_string_to_buffer(Small, 3);
            Values += "." + HexC(Small);
            Values += "." + WideValue(R.get_wstring());
            Values += "." + WideValue(R.get_wstring());
            Values += "." + StringValue(R.get_string());
            Out.Line("{\"in\":\"marshal 1\",\"out\":\"" + Encoding + "\",\"values\":\"" + Values + "\"}");
        }

        // Case 2: costume and lists (marshal_lists), then what reading them back gives.
        {
            Gen G = Gen::ForCase(0x3au, 2);
            Costume C;
            std::memset(&C, 0, sizeof(C));
            for (int Index = 0; Index < 4; ++Index)
                C.PartIds[Index] = G.U32();
            C.AccessoryCount = 3;
            for (int Index = 0; Index < 3; ++Index)
                C.AccessoryIds[Index] = G.U32();
            GoodDescList Goods;
            for (int Index = 0; Index < 2; ++Index)
            {
                GoodDesc* Good = new GoodDesc;
                Good->GoodId = G.U32();
                Good->Count = G.I32();
                Good->Price = G.F32();
                Good->Field0C = G.F32();
                Goods.append(Good);
            }
            EquipDescList Equip;
            const char* const Names[2] = {"HpWeapon01", "HpTurret02"};
            for (int Index = 0; Index < 2; ++Index)
            {
                EquipDesc Item;
                Item.ArchId = G.U32();
                Item.Hardpoint = Names[Index];
                Item.Count = G.I32();
                Item.Health = G.F32();
                Item.Id = G.U16();
                Item.bEquipped = (unsigned char)(G.U8() & 1u);
                Item.bMission = (G.U8() & 1u) != 0;
                Equip.Items.push_back(Item);
            }
            std::memset(Storage, 0, sizeof(Storage));
            MarshalBuffer W;
            W.use_write_buffer(Storage, (int)sizeof(Storage));
            W.put_Costume(C);
            W.put_GoodDescList(Goods);
            W.put_EquipDescList(Equip);
            const std::string Encoding = Written(W);
            MarshalBuffer R;
            R.use_read_buffer(Storage, W.get_num_written());
            const Costume C2 = R.get_Costume();
            const GoodDescList Goods2 = R.get_GoodDescList();
            const EquipDescList Equip2 = R.get_EquipDescList();
            std::memset(BackStorage, 0, sizeof(BackStorage));
            MarshalBuffer B;
            B.use_write_buffer(BackStorage, (int)sizeof(BackStorage));
            B.put_Costume(C2);
            B.put_GoodDescList(Goods2);
            B.put_EquipDescList(Equip2);
            Out.Line("{\"in\":\"marshal 2\",\"out\":\"" + Encoding + "\",\"back\":\"" + Written(B) + "\"}");
        }

        // Case 3: a format string (fmt_case 2 of protocol_fmtstr).
        {
            Gen G = Gen::ForCase(0xf3u, 2);
            FmtStr Text;
            FmtStrCase(2, G, Text);
            std::memset(Storage, 0, sizeof(Storage));
            MarshalBuffer W;
            W.use_write_buffer(Storage, (int)sizeof(Storage));
            W.put_FmtStr(Text);
            W.put_U8(0x5a);
            const std::string Encoding = Written(W);
            MarshalBuffer R;
            R.use_read_buffer(Storage, W.get_num_written());
            const FmtStr Back = R.get_FmtStr();
            const unsigned char Tail = R.get_U8();
            std::memset(BackStorage, 0, sizeof(BackStorage));
            MarshalBuffer B;
            B.use_write_buffer(BackStorage, (int)sizeof(BackStorage));
            B.put_FmtStr(Back);
            B.put_U8(Tail);
            Out.Line("{\"in\":\"marshal 3\",\"out\":\"" + Encoding + "\",\"back\":\"" + Written(B) + "\"}");
        }

        // Case 4: reads of a crafted buffer.
        {
            static const unsigned char Input[12] = {0x00, 0x40, 0x00, 0x00, 0x41, 0x02,
                                                    0x01, 0xff, 0xff, 0xff, 0xff, 0x07};
            MarshalBuffer R;
            R.use_read_buffer(Input, (int)sizeof(Input));
            std::string Values = StringValue(R.get_string());
            Values += "." + Format("%02x", (unsigned int)R.get_U8());
            Values += R.get_bool() ? ".1" : ".0";
            Values += R.get_bool() ? ".1" : ".0";
            Values += "." + WideValue(R.get_wstring());
            Values += "." + Format("%02x", (unsigned int)R.get_U8());
            Out.Line("{\"in\":\"" + Hex(Input, sizeof(Input)) + "\",\"values\":\"" + Values + "\"}");
        }
    }
}

void RunProtocolSuites()
{
    FmtStrSuite();
    RdlSuite();
    MarshalSuite();
    RunProtocolPacketSuites();
    oracle_protocol::RestoreMode();
}
