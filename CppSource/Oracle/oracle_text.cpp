// P4.12, P4.13, P4.14: the client's text suites, run by Oracle.exe neuron-client after the
// scene-script suite (RunNeuronClientSuites).
//
// number_text.jsonl: MsvcrtFormatLuaNumber and MsvcrtNumericVsnprintf of
// CppSource/include/msvcrt_numeric_format.h, the header lvm.c and font_string.cpp include,
// over tests/fixtures/oracle/number_text.list.
// rdl_markup.jsonl: Common.dll's XMLReader::read_buffer (MSXML's SAXXMLReader with the RDLX
// content handler) into an empty RenderDisplayList, over tests/fixtures/oracle/rdl_markup.list.
// rich_text.jsonl: oracle_rich_text.cpp, run last.
// The Rust side is crates/neuron-client/tests/support/text.rs, which documents the lines.

#include <windows.h>
#include <objbase.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "oracle_support.h"

#include "common_text.h"
#include "msvcrt_numeric_format.h"

using namespace oracle;

namespace
{
    // The fill byte of the printf buffer.
    const unsigned char kFill = 0xaa;

    struct FArg
    {
        char Kind; // 'i' or 'd'
        int Int;
        double Double;
    };

    int CallNumeric(char* Buffer, size_t Count, const char* Format, ...)
    {
        va_list Args;
        va_start(Args, Format);
        const int Result = MsvcrtNumericVsnprintf(Buffer, Count, Format, Args);
        va_end(Args);
        return Result;
    }

    // MsvcrtNumericVsnprintf with the arguments of a case, passed as the variadic call passes
    // them. False for an argument pattern this suite does not spell out.
    bool CallWithArgs(char* Buffer, size_t Count, const char* Format, const std::vector<FArg>& Args,
                      int& Result)
    {
        std::string Kinds;
        for (const FArg& Arg : Args)
            Kinds += Arg.Kind;
        if (Kinds.empty())
            Result = CallNumeric(Buffer, Count, Format);
        else if (Kinds == "d")
            Result = CallNumeric(Buffer, Count, Format, Args[0].Double);
        else if (Kinds == "i")
            Result = CallNumeric(Buffer, Count, Format, Args[0].Int);
        else if (Kinds == "ii")
            Result = CallNumeric(Buffer, Count, Format, Args[0].Int, Args[1].Int);
        else if (Kinds == "di")
            Result = CallNumeric(Buffer, Count, Format, Args[0].Double, Args[1].Int);
        else if (Kinds == "dd")
            Result = CallNumeric(Buffer, Count, Format, Args[0].Double, Args[1].Double);
        else if (Kinds == "iii")
            Result = CallNumeric(Buffer, Count, Format, Args[0].Int, Args[1].Int, Args[2].Int);
        else
            return false;
        return true;
    }

    std::vector<std::string> Split(const std::string& Line)
    {
        std::vector<std::string> Out;
        size_t Start = 0;
        while (Start <= Line.size())
        {
            size_t End = Line.find(' ', Start);
            if (End == std::string::npos)
                End = Line.size();
            Out.push_back(Line.substr(Start, End - Start));
            Start = End + 1;
        }
        return Out;
    }

    void NumberTextSuite()
    {
        Suite Out("number_text.jsonl");
        for (const std::string& Line : InputLines("number_text.list"))
        {
            const std::vector<std::string> Fields = Split(Line);
            if (Fields.size() == 2 && Fields[0] == "lua")
            {
                const double Value = DoubleFromBits(strtoull(Fields[1].c_str(), nullptr, 16));
                char Text[32];
                MsvcrtFormatLuaNumber(Text, Value);
                Out.Line("{\"lua\":\"" + Fields[1] + "\",\"text\":\"" + HexC(Text) + "\"}");
                continue;
            }
            if (Fields.size() < 3 || Fields[0] != "printf")
            {
                fprintf(stderr, "number_text: bad line %s\n", Line.c_str());
                GFailed = true;
                continue;
            }
            const size_t Count = (size_t)strtoul(Fields[1].c_str(), nullptr, 10);
            const Bytes FormatText = Fields[2] == "-" ? Bytes(1, 0) : Unhex(Fields[2]);
            std::vector<FArg> Args;
            std::string ArgsText;
            for (size_t Index = 3; Index < Fields.size(); ++Index)
            {
                const std::string& Field = Fields[Index];
                FArg Arg = {Field[0], 0, 0.0};
                if (Arg.Kind == 'i')
                    Arg.Int = (int)strtoul(Field.c_str() + 2, nullptr, 16);
                else
                    Arg.Double = DoubleFromBits(strtoull(Field.c_str() + 2, nullptr, 16));
                Args.push_back(Arg);
                ArgsText += (ArgsText.empty() ? "" : " ") + Field;
            }
            std::vector<char> Buffer(Count + 1, (char)kFill);
            int Result = 0;
            if (!CallWithArgs(Buffer.data(), Count, CStr(FormatText), Args, Result))
            {
                fprintf(stderr, "number_text: unsupported arguments in %s\n", Line.c_str());
                GFailed = true;
                continue;
            }
            Out.Line("{\"count\":\"" + Format("%u", (unsigned int)Count) + "\",\"format\":\"" + Fields[2] +
                     "\",\"args\":\"" + (ArgsText.empty() ? std::string("-") : ArgsText) +
                     "\",\"result\":\"" + Format("%d", Result) + "\",\"buffer\":\"" +
                     Hex(Buffer.data(), Count) + "\"}");
        }
    }

    std::string Units(const wchar_t* Text, size_t Count)
    {
        std::string Out;
        for (size_t Index = 0; Index < Count; ++Index)
            Out += Format("%04x", (unsigned int)(unsigned short)Text[Index]);
        return Out;
    }

    std::string NodeText(RDLNode* Node)
    {
        if (TRANode* Tra = Node->operator TRANode*())
            return Format("tra:%08lx:%08x:%08x", Tra->get_attributes().Packed, Tra->get_mask(),
                          Tra->get_default_mask());
        if (TextNode* Text = Node->operator TextNode*())
            return "text:" + Units(Text->get_text().c_str(), Text->get_text().size());
        if (TextPtrNode* Ptr = Node->operator TextPtrNode*())
            return "ptr:" + Units(Ptr->get_text(), (size_t)Ptr->get_text_length());
        if (AttribStackNode* Stack = Node->operator AttribStackNode*())
            return Format("stack:%d", (int)Stack->get_operation());
        if (PositionNode* Pos = Node->operator PositionNode*())
            return Format("pos:%d:%d:%08x:%08x", Pos->is_rel_h() ? 1 : 0, Pos->is_rel_v() ? 1 : 0,
                          FloatBits(Pos->get_pos_v()), FloatBits(Pos->get_pos_h()));
        if (Node->operator ParagraphNode*() != nullptr)
            return "para";
        if (JustifyNode* Justify = Node->operator JustifyNode*())
            return Format("just:%d", (int)Justify->get_justify());
        if (StyleNode* Style = Node->operator StyleNode*())
            return Format("style:%u", (unsigned int)Style->get_style_id());
        return "other";
    }

    void RdlMarkupSuite()
    {
        // XMLReader::read_buffer needs COM on the calling thread (the client's main() calls
        // CoInitialize before any text loads).
        CoInitialize(nullptr);
        Suite Out("rdl_markup.jsonl");
        unsigned int Case = 0;
        for (const std::string& Line : InputLines("rdl_markup.list"))
        {
            const Bytes Text = Line == "-" ? Bytes(2, 0) : Unhex(Line);
            const unsigned int Size = Line == "-" ? 0u : (unsigned int)(Line.size() / 2);
            RenderDisplayList Rdl;
            XMLReader Reader;
            const bool bOk = Reader.read_buffer(Rdl, (const char*)Text.data(), Size);
            std::string Nodes;
            for (FRdlListNode* Cell = Rdl.NodeList->Next; Cell != Rdl.NodeList; Cell = Cell->Next)
            {
                if (!Nodes.empty())
                    Nodes += " ";
                Nodes += NodeText(Cell->Node);
            }
            Out.Line(Format("{\"case\":\"%u\",\"ok\":\"%d\",\"nodes\":\"", Case, bOk ? 1 : 0) + Nodes + "\"}");
            ++Case;
        }
        CoUninitialize();
    }
}

void RunTextSuites()
{
    NumberTextSuite();
    RdlMarkupSuite();
    RunRichTextSuites();
}
