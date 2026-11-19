// P4.13: the client UI toolkit's page descriptors, through the reference's own code.
// ui_page_desc.cpp (FUiPageDesc, the PAGE_*.page loader of the front end) is compiled into
// this file; the client's oracle hook it calls (OracleOnPageDescLoaded, oracle_mode.cpp) is a
// no-op here. The Rust side is crates/neuron-client/tests/oracle_vectors.rs with
// crates/neuron-client/tests/support/ui.rs; the inputs come from
// crates/neuron-client/tests/fixtures/oracle/make_ui_fixtures.py.
//
// ui_page_desc.list holds one case per line: the bytes of a PAGE file in hex ("-" for an empty
// file). Each is written to ui_page_desc.page in the output directory and loaded with
// FUiPageDesc::LoadFromFile. Line formats, each prefixed with "case":
//   {"ok","aspect","count"}  the result, FUiPageData::Aspect's bits and the entries read (also
//                            those before a failure)
//   {"i","type","name","rect","payload"} for each entry of FUiPageData::Entries (none after a
//                            failure): rect the four floats' bits; payload by class --
//     text:<right>,<bottom>,<caption units>
//     button:<right>,<bottom>,<caption units>,<extra int>,<extra id>,<style>
//     bounds:<right>,<bottom>
//     object:<right>,<bottom>,<name hex>,<fifth float>
//     table:<right>,<bottom>,<int5>,<int6>
//     scroll:<right>,<bottom>,<four floats>,<flag>
//   floats as f32 bits in hex, caption units as four hex digits each, the style "none",
//   "default" or the name in hex.

#include "oracle_support.h"

#include <stdio.h>
#include <string.h>

#include <string>

#include "../OutpostFrontier/Client/ui_page_desc.cpp"

// The client's oracle hook (oracle_mode.cpp), which only dumps when the client runs +oracle.
void OracleOnPageDescLoaded(char const* /*Path*/, bool /*bOk*/, FUiPageData const* /*Data*/)
{
}

using namespace oracle;

namespace
{
    std::string Bits(float Value)
    {
        return Format("%08x", FloatBits(Value));
    }

    std::string Units(wchar_t const* Text)
    {
        std::string Out;
        for (; Text != nullptr && *Text != 0; ++Text)
            Out += Format("%04x", (unsigned int)(unsigned short)*Text);
        return Out;
    }

    std::string Head(float Right, float Bottom)
    {
        return Bits(Right) + "," + Bits(Bottom);
    }

    std::string PayloadText(FUiWidgetEntry const* Entry)
    {
        char const* Type = Entry->TypeName;
        if (strcmp(Type, "UIStaticText") == 0 || strcmp(Type, "UITextEdit") == 0)
        {
            FUiPayloadText const* P = static_cast<FUiPayloadText const*>(Entry->Payload);
            return "text:" + Head(P->Right, P->Bottom) + "," + Units(P->CaptionUtf16);
        }
        if (strcmp(Type, "UITextMsgButton") == 0)
        {
            FUiPayloadTextMsgButton const* P = static_cast<FUiPayloadTextMsgButton const*>(Entry->Payload);
            std::string Style;
            if (P->StyleName == nullptr)
                Style = "none";
            else if (P->StyleName == kUiPageDefaultStyle)
                Style = "default";
            else
                Style = HexC(P->StyleName);
            return "button:" + Head(P->Right, P->Bottom) + "," + Units(P->CaptionUtf16) +
                   Format(",%d,%d,", P->ExtraInt, P->ExtraId) + Style;
        }
        if (strcmp(Type, "UIWinObject") == 0)
        {
            FUiPayloadWinObject const* P = static_cast<FUiPayloadWinObject const*>(Entry->Payload);
            return "object:" + Head(P->Right, P->Bottom) + "," + HexC(P->ExtraName) + "," +
                   Bits(P->ExtraFloat);
        }
        if (strcmp(Type, "UITable") == 0 || strcmp(Type, "UIFastTable") == 0)
        {
            FUiPayloadTable const* P = static_cast<FUiPayloadTable const*>(Entry->Payload);
            return "table:" + Head(P->Right, P->Bottom) + Format(",%d,%d", P->Int5, P->Int6);
        }
        if (strcmp(Type, "UIScroll") == 0 || strcmp(Type, "UICycle") == 0)
        {
            FUiPayloadScrollCycle const* P = static_cast<FUiPayloadScrollCycle const*>(Entry->Payload);
            return "scroll:" + Head(P->Right, P->Bottom) + "," + Bits(P->Extra0) + "," +
                   Bits(P->Extra1) + "," + Bits(P->Extra2) + "," + Bits(P->Extra3) +
                   Format(",%d", (int)P->Flag);
        }
        FUiPayloadBounds const* P = static_cast<FUiPayloadBounds const*>(Entry->Payload);
        return "bounds:" + Head(P->Right, P->Bottom);
    }

    void PageDescSuite()
    {
        Suite Out("ui_page_desc.jsonl");
        std::string const Path = GOutputDir + "\\ui_page_desc.page";
        unsigned int Case = 0;
        for (std::string const& Line : InputLines("ui_page_desc.list"))
        {
            Bytes Data;
            if (Line != "-")
            {
                Data = Unhex(Line);
                Data.pop_back();
            }
            FILE* File = fopen(Path.c_str(), "wb");
            if (File == nullptr)
            {
                fprintf(stderr, "ui_page_desc: cannot write %s\n", Path.c_str());
                GFailed = true;
                return;
            }
            if (!Data.empty())
                fwrite(Data.data(), 1, Data.size(), File);
            fclose(File);

            FUiPageDesc Desc;
            bool const bOk = Desc.LoadFromFile(Path.c_str());
            FUiPageData* const PageData = Desc.GetPageData();
            Out.Line(Format("{\"case\":\"%u\",\"ok\":\"%d\",\"aspect\":\"%08x\",\"count\":\"%u\"}", Case,
                            bOk ? 1 : 0, FloatBits(PageData->Aspect), Desc.GetEntryCount()));
            unsigned int Index = 0;
            for (FUiWidgetEntry** Cursor = PageData->Entries; Cursor != nullptr && *Cursor != nullptr;
                 ++Cursor, ++Index)
            {
                FUiWidgetEntry const* Entry = *Cursor;
                Out.Line(Format("{\"case\":\"%u\",\"i\":\"%u\",\"type\":\"%s\",\"name\":\"%s\",\"rect\":\"",
                                Case, Index, HexC(Entry->TypeName).c_str(),
                                HexC(Entry->InstanceName).c_str()) +
                         Bits(Entry->Left) + "," + Bits(Entry->Top) + "," + Bits(Entry->Right) + "," +
                         Bits(Entry->Bottom) + "\",\"payload\":\"" + PayloadText(Entry) + "\"}");
            }
            ++Case;
        }
        remove(Path.c_str());
    }
}

void RunUiSuites()
{
    PageDescSuite();
    RunUiQuadSuites();
    RunUiControlSuites();
    RunUiImmediateSuites();
}
