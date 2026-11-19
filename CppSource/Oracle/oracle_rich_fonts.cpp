// P4.12: the rich fonts INI through the client's own reader, LoadRichFontsFromIni of
// OutpostFrontier/Client/menu_fonts_load.cpp, compiled into this file; run by Oracle.exe
// neuron-client after the rich_text suite (RunRichTextSuites calls RunRichFontsSuite).
//
// rich_fonts.jsonl, over tests/fixtures/oracle/rich_fonts.list: each line names an INI file
// under tests/fixtures/oracle/rich_fonts/ ("-" for a file that does not exist) and the style
// IDs to look at afterwards, as n<name in hex> (the ID LoadRichFontsFromIni hashes the name
// to, HashStyleName) or i<decimal ID>. The file is read through Common's INI_Reader from its
// absolute path (GetFileSysPath copies an absolute path). The [TrueType] lines go to a
// recording stand-in for Common's SetStandardFont (the file's other calls, CopyBoundedString
// and RdlAppendNode, are stand-ins with the client's bodies under names of their own, as is
// the reader itself), and the [Style] sections to Common's StyleCollection, whose probed
// styles are emptied before the file is read and again afterwards, so the cases are
// independent and leave the registry as they found it for the later suites.
//
// Line formats:
//   {"case":"<n>","fonts":"<index>:<face hex>:<size>:<bold><italic><underline> ..."}
//       the standard fonts the file set, by index, each with its last values
//   {"case":"<n>","probe":"<token>","id":"<id>","nodes":"<list>"}
//       a probed style: its nodes as rich_text.jsonl prints them (j:<mode>, a:<attrs>:<mask>:
//       <default mask>, "other" for any other kind)
// The Rust side is crates/neuron-client/tests/support/rich_text.rs (rich_fonts_lines).

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "oracle_support.h"

#include "common_ini.h"
#include "common_misc.h"
#include "common_text.h"
#include "../OutpostFrontier/Client/globals.h"
#include "../OutpostFrontier/Client/ids_rdl.h"
#include "../OutpostFrontier/Client/menu_fonts_load.h"

// The headers above are in; these names now reach only menu_fonts_load.cpp's body.
// SetStandardFont is Common's export and records here. CopyBoundedString (the client's
// startup_config.cpp; Server.lib has one of its own) and RdlAppendNode (the client's
// ids_rdl.cpp; oracle_ui_controls.cpp defines one) are file-local copies of the client's
// bodies, and the reader is renamed, so this file defines none of their names.
#define SetStandardFont OracleRichFontsSetStandardFont
#define CopyBoundedString OracleRichFontsCopyBoundedString
#define RdlAppendNode OracleRichFontsRdlAppendNode
#define LoadRichFontsFromIni OracleLoadRichFontsFromIni

namespace
{
    // The standard fonts a case set, by index.
    struct FOracleStandardFont
    {
        bool bSet;
        std::string Face;
        unsigned long Size;
        unsigned char Style[3];
    };

    FOracleStandardFont GRichFonts[32];
}

// The stand-ins have internal linkage; a declaration the included file repeats keeps it.
static void OracleRichFontsSetStandardFont(int Index, TextRenderContext::FontDesc const& Desc)
{
    if (Index < 0 || Index >= 32)
        return;
    FOracleStandardFont& Font = GRichFonts[Index];
    Font.bSet = true;
    Font.Face = Desc.Name != nullptr ? Desc.Name : "";
    Font.Size = Desc.Size;
    Font.Style[0] = Desc.bBold;
    Font.Style[1] = Desc.bItalic;
    Font.Style[2] = Desc.bUnderline;
}

// startup_config.cpp's body.
static unsigned OracleRichFontsCopyBoundedString(char* Dest, unsigned Capacity, const char* Src)
{
    if (Src == nullptr)
        return 0;
    unsigned Length = static_cast<unsigned>(strlen(Src));
    if (Length >= Capacity)
        Length = Capacity - 1;
    memcpy(Dest, Src, Length);
    Dest[Length] = '\0';
    return Length;
}

// ids_rdl.cpp's body: a 0x0c-byte {next, prev, node} cell before the sentinel.
static void OracleRichFontsRdlAppendNode(RenderDisplayList& Rdl, RDLNode* Node)
{
    void** const Sentinel = (void**)Rdl.NodeList;
    void** const Tail = static_cast<void**>(Sentinel[1]);
    void** const Cell = static_cast<void**>(malloc(0x0c));
    Cell[0] = Sentinel;
    Cell[1] = Tail != nullptr ? Tail : Cell;
    Sentinel[1] = Cell;
    static_cast<void**>(Cell[1])[0] = Cell;
    Cell[2] = Node;
    Rdl.Count += 1;
}

#include "../OutpostFrontier/Client/menu_fonts_load.cpp"

#undef LoadRichFontsFromIni
#undef RdlAppendNode
#undef CopyBoundedString
#undef SetStandardFont

using namespace oracle;

namespace
{
    std::string NodeText(RDLNode* Node)
    {
        if (TRANode* Tra = Node->operator TRANode*())
            return Format("a:%08lx:%08x:%08x", Tra->get_attributes().Packed, Tra->get_mask(),
                          Tra->get_default_mask());
        if (JustifyNode* Justify = Node->operator JustifyNode*())
            return Format("j:%d", (int)Justify->get_justify());
        return "other";
    }

    std::string ListText(const RenderDisplayList& Rdl)
    {
        std::string Out;
        for (FRdlListNode* Cell = Rdl.NodeList->Next; Cell != Rdl.NodeList; Cell = Cell->Next)
        {
            if (!Out.empty())
                Out += " ";
            Out += NodeText(Cell->Node);
        }
        return Out;
    }

    std::vector<std::string> Tokens(const std::string& Line)
    {
        std::vector<std::string> Out;
        size_t Start = 0;
        while (Start <= Line.size())
        {
            size_t End = Line.find(' ', Start);
            if (End == std::string::npos)
                End = Line.size();
            if (End > Start)
                Out.push_back(Line.substr(Start, End - Start));
            Start = End + 1;
        }
        return Out;
    }

    // The style ID of a probe token.
    unsigned short ProbeId(const std::string& Token)
    {
        if (Token[0] == 'n')
        {
            const Bytes Name = Unhex(Token.substr(1));
            return static_cast<unsigned short>(HashStyleName(CStr(Name)));
        }
        return static_cast<unsigned short>(strtoul(Token.c_str() + 1, nullptr, 10));
    }

    void EmptyStyles(const std::vector<std::string>& T)
    {
        for (size_t Index = 1; Index < T.size(); ++Index)
        {
            RenderDisplayList Empty;
            StyleCollection::set_style(ProbeId(T[Index]), Empty);
        }
    }

    void RichFontsSuite()
    {
        Suite Out("rich_fonts.jsonl");
        unsigned int Case = 0;
        for (const std::string& Line : InputLines("rich_fonts.list"))
        {
            const std::vector<std::string> T = Tokens(Line);
            if (T.empty())
            {
                fprintf(stderr, "rich_fonts: empty line\n");
                GFailed = true;
                continue;
            }
            for (int Index = 0; Index < 32; ++Index)
            {
                GRichFonts[Index].bSet = false;
                GRichFonts[Index].Face.clear();
                GRichFonts[Index].Size = 0;
                memset(GRichFonts[Index].Style, 0, sizeof(GRichFonts[Index].Style));
            }
            EmptyStyles(T);
            const std::string Name = T[0] == "-" ? std::string("missing.ini") : T[0];
            const std::string Path = GFixtureDir + "\\rich_fonts\\" + Name;
            OracleLoadRichFontsFromIni(Path.c_str(), nullptr);

            std::string Fonts;
            for (int Index = 0; Index < 32; ++Index)
            {
                const FOracleStandardFont& Font = GRichFonts[Index];
                if (!Font.bSet)
                    continue;
                if (!Fonts.empty())
                    Fonts += " ";
                Fonts += Format("%d:", Index) + HexC(Font.Face.c_str()) +
                         Format(":%lu:%d%d%d", Font.Size, Font.Style[0], Font.Style[1], Font.Style[2]);
            }
            Out.Line(Format("{\"case\":\"%u\",\"fonts\":\"", Case) + Fonts + "\"}");
            for (size_t Index = 1; Index < T.size(); ++Index)
            {
                const unsigned short Id = ProbeId(T[Index]);
                Out.Line(Format("{\"case\":\"%u\",\"probe\":\"%s\",\"id\":\"%u\",\"nodes\":\"", Case,
                                T[Index].c_str(), (unsigned int)Id) +
                         ListText(StyleCollection::get_style(Id)) + "\"}");
            }
            EmptyStyles(T);
            ++Case;
        }
    }
}

void RunRichFontsSuite()
{
    RichFontsSuite();
}
