// P4.12: rich-text layout and execution, run by Oracle.exe neuron-client at the end of the
// text suites (RunTextSuites calls RunRichTextSuites, which then runs the rich fonts suite of
// oracle_rich_fonts.cpp).
//
// rich_text.jsonl: Common.dll's LayoutRichText, RenderDisplayList::execute, get_dimensions,
// get_element_pos and get_element_count, the node behaviours behind them, StyleCollection and
// lex_compare, over tests/fixtures/oracle/rich_text.list. The context is FRecordingTRC, a
// TextRenderContext (common_textrender.cpp's base class: attribute and state stacks, pen,
// origin) whose eleven device functions measure with fixed metrics and record what they are
// asked to draw: the fake font of crates/neuron-client/tests/support/rich_text.rs, which
// renders the same lines through the port.

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <deque>
#include <string>
#include <vector>

#include "oracle_support.h"

#include "common_text.h"

using namespace oracle;

namespace
{
    // The image FRecordingTRC hands out and the fixtures build: a size and nothing else.
    class FOracleImage : public TextRenderContext::Image
    {
    public:
        FOracleImage(float InWidth, float InHeight)
        {
            Width = InWidth;
            Height = InHeight;
        }
    };

    // Everything the fixtures' nodes point at lives as long as the process: text-pointer
    // runs, images and referenced lists (styles keep clones of them in Common's registry).
    std::deque<std::wstring>& Runs()
    {
        static std::deque<std::wstring> Storage;
        return Storage;
    }

    std::deque<FOracleImage>& Images()
    {
        static std::deque<FOracleImage> Storage;
        return Storage;
    }

    std::deque<RenderDisplayList>& Lists()
    {
        static std::deque<RenderDisplayList> Storage;
        return Storage;
    }

    std::string Units(const wchar_t* Text, int Count)
    {
        std::string Out;
        for (int Index = 0; Index < Count; ++Index)
            Out += Format("%04x", (unsigned int)(unsigned short)Text[Index]);
        return Out;
    }

    // The fake font: a font byte B (the low byte of the attributes) has a cell of
    // 10 + 2 * ((B >> 3) & 7) pixels, an ascent of three quarters of it and a bold bit (B & 1)
    // that widens every non-NUL character by one. Advances: NUL 0, space a third of the cell,
    // i l . , ' | a quarter (at least 1), M W the cell, anything else half of it.
    int CellOf(unsigned long Attrs)
    {
        const unsigned int Byte = (unsigned int)(Attrs & 0xff);
        return 10 + 2 * (int)((Byte >> 3) & 7);
    }

    int AscentOf(unsigned long Attrs)
    {
        return CellOf(Attrs) * 3 / 4;
    }

    int AdvanceOf(unsigned long Attrs, wchar_t Ch)
    {
        const int Cell = CellOf(Attrs);
        const unsigned int Unit = (unsigned int)(unsigned short)Ch;
        if (Unit == 0)
            return 0;
        int Advance = Cell / 2;
        if (Unit == 0x20)
            Advance = Cell / 3;
        else if (Unit == 'i' || Unit == 'l' || Unit == '.' || Unit == ',' || Unit == '\'' || Unit == '|')
            Advance = Cell / 4 > 0 ? Cell / 4 : 1;
        else if (Unit == 'M' || Unit == 'W')
            Advance = Cell;
        return Advance + (int)(Attrs & 1);
    }

    int WidthOf(unsigned long Attrs, const wchar_t* Text, int Len)
    {
        int Width = 0;
        for (int Index = 0; Index < Len; ++Index)
            Width += AdvanceOf(Attrs, Text[Index]);
        return Width;
    }

    class FRecordingTRC : public TextRenderContext
    {
    public:
        FRecordingTRC()
        {
            Clip.left = 0;
            Clip.top = 0;
            Clip.right = 0;
            Clip.bottom = 0;
        }

        std::string Log;

        void Record(const std::string& Entry)
        {
            if (!Log.empty())
                Log += " ";
            Log += Entry;
        }

        int get_cell_width() const override
        {
            return CellOf(CurrentAttrs.Packed) + (int)(CurrentAttrs.Packed & 1);
        }

        int get_cell_height() const override
        {
            return CellOf(CurrentAttrs.Packed);
        }

        void get_text_visual_size(const wchar_t* Text, VisualSize& Size, int Len) const override
        {
            if (Text == nullptr)
            {
                Size.Ascent = 0;
                Size.Descent = 0;
                return;
            }
            if (Len == -1)
                Len = (int)wcslen(Text);
            Size.Width = WidthOf(CurrentAttrs.Packed, Text, Len);
            Size.Height = CellOf(CurrentAttrs.Packed);
            Size.Ascent = AscentOf(CurrentAttrs.Packed);
            Size.Descent = Size.Height - Size.Ascent;
        }

        void get_font_metrics(int& Ascent, int& Descent) const override
        {
            Ascent = AscentOf(CurrentAttrs.Packed);
            Descent = CellOf(CurrentAttrs.Packed) - Ascent;
        }

        // GetTextExtentExPointW: the leading characters whose summed advances are at most
        // MaxWidth.
        void get_text_fit(const wchar_t* Text, int MaxWidth, int& FitCount, int*, VisualSize&,
                          int Len) const override
        {
            if (Text == nullptr || Len == 0)
            {
                FitCount = 0;
                return;
            }
            if (Len == -1)
                Len = (int)wcslen(Text);
            FitCount = 0;
            int Total = 0;
            for (int Index = 0; Index < Len; ++Index)
            {
                Total += AdvanceOf(CurrentAttrs.Packed, Text[Index]);
                if (Total > MaxWidth)
                    break;
                ++FitCount;
            }
        }

        void execute_text(const wchar_t* Text, bool bHighlight, int Len) override
        {
            if (Text == nullptr)
            {
                Record("T:-");
                return;
            }
            if (Len == -1)
                Len = (int)wcslen(Text);
            Record(Format("T%d@%d,%d/%d,%d#%08lx:", bHighlight ? 1 : 0, PosH, PosV, OriginH,
                          OriginV, CurrentAttrs.Packed) +
                   Units(Text, Len));
            PosH += WidthOf(CurrentAttrs.Packed, Text, Len);
        }

        // DeviceTRC's advance at an image scale of 1 (Common's GRichTextScaleY before
        // InitializeRichText): FLD scale; FMUL width; FIADD PosH; ftol.
        void execute_image(const Image* Img, bool bDraw) override
        {
            if (Img == nullptr)
            {
                Record(Format("I%d:-", bDraw ? 1 : 0));
                return;
            }
            Record(Format("I%d@%d,%d/%d,%d:%08x:%08x", bDraw ? 1 : 0, PosH, PosV, OriginH,
                          OriginV, FloatBits(Img->Width), FloatBits(Img->Height)));
            const double Scale = 1.0f;
            PosH = (int)(Scale * Img->Width + PosH);
        }

        void set_clip_rect(const RECT& NewClip) override
        {
            Record(Format("C%ld,%ld,%ld,%ld", NewClip.left, NewClip.top, NewClip.right,
                          NewClip.bottom));
            Clip = NewClip;
        }

        void get_clip_rect(RECT& OutRect) override
        {
            OutRect = Clip;
        }

        Image* create_image(const char* const) override
        {
            return new FOracleImage(0.0f, 0.0f);
        }

        void destroy_image(Image* Img) override
        {
            delete Img;
        }

        // A fresh context's state: both attribute words set, the pen and the origin at 0.
        void Begin(unsigned long Attrs)
        {
            TextRenderAttributes Value;
            Value.Packed = Attrs;
            set_current_attributes(Value);
            set_default_attributes(Value);
            clear_default_attributes();
            set_pos(0, 0);
            set_origin(0, 0);
            Log.clear();
        }

        std::string After()
        {
            return Format("%08lx:%d:%d:%d:%d", CurrentAttrs.Packed, get_pos_h(), get_pos_v(),
                          (int)AttrStack.size(), (int)StateStack.size());
        }

    private:
        RECT Clip;
    };

    // A cell appended at the tail holding Node's clone (Clone() runs in Common.dll; cells are
    // malloc'd as every module's are).
    void Append(RenderDisplayList& Rdl, const RDLNode& Node)
    {
        FRdlListNode* const Sentinel = Rdl.NodeList;
        FRdlListNode* const Cell = (FRdlListNode*)malloc(sizeof(FRdlListNode));
        Cell->Next = Sentinel;
        Cell->Prev = Sentinel->Prev;
        Sentinel->Prev->Next = Cell;
        Sentinel->Prev = Cell;
        Cell->Node = Node.Clone();
        ++Rdl.Count;
    }

    std::vector<std::string> Fields(const std::string& Token)
    {
        std::vector<std::string> Out;
        size_t Start = 0;
        for (;;)
        {
            const size_t End = Token.find(':', Start);
            Out.push_back(Token.substr(Start, End == std::string::npos ? std::string::npos : End - Start));
            if (End == std::string::npos)
                break;
            Start = End + 1;
        }
        return Out;
    }

    std::wstring WideUnits(const std::string& Hex)
    {
        std::wstring Out;
        for (size_t At = 0; At + 4 <= Hex.size(); At += 4)
            Out += (wchar_t)strtoul(Hex.substr(At, 4).c_str(), nullptr, 16);
        return Out;
    }

    unsigned long HexWord(const std::string& Text)
    {
        return strtoul(Text.c_str(), nullptr, 16);
    }

    // The ops of a list from Tokens[At] up to a closing "]" or the end (see rich_text.list).
    void Build(RenderDisplayList& Rdl, const std::vector<std::string>& Tokens, size_t& At)
    {
        while (At < Tokens.size())
        {
            const std::string Token = Tokens[At++];
            if (Token == "]")
                return;
            if (Token == "[")
            {
                Lists().push_back(RenderDisplayList());
                RenderDisplayList& Inner = Lists().back();
                Build(Inner, Tokens, At);
                RDLRefNode Node(Inner);
                Append(Rdl, Node);
                continue;
            }
            const std::vector<std::string> F = Fields(Token);
            const std::string& Kind = F[0];
            if (Kind == "t")
            {
                TextNode Node(WideUnits(F.size() > 1 ? F[1] : std::string()), -1);
                Append(Rdl, Node);
            }
            else if (Kind == "p")
            {
                Runs().push_back(WideUnits(F.size() > 1 ? F[1] : std::string()));
                TextPtrNode Node(Runs().back().c_str(), (int)Runs().back().size());
                Append(Rdl, Node);
            }
            else if (Kind == "a")
            {
                TextRenderAttributes Attrs;
                Attrs.Packed = HexWord(F[1]);
                TRANode Node(Attrs, (unsigned int)HexWord(F[2]), (unsigned int)HexWord(F[3]));
                Append(Rdl, Node);
            }
            else if (Kind == "s")
            {
                AttribStackNode Node((AttribStackNode::Op)atoi(F[1].c_str()));
                Append(Rdl, Node);
            }
            else if (Kind == "x")
            {
                PositionNode Node;
                Node.set_rel_h(F[1] == "1");
                Node.set_rel_v(F[2] == "1");
                Node.set_pos(FloatFromBits((unsigned int)HexWord(F[3])),
                             FloatFromBits((unsigned int)HexWord(F[4])));
                Append(Rdl, Node);
            }
            else if (Kind == "n")
            {
                ParagraphNode Node;
                Append(Rdl, Node);
            }
            else if (Kind == "j")
            {
                JustifyNode Node((TextJustify)atoi(F[1].c_str()));
                Append(Rdl, Node);
            }
            else if (Kind == "c")
            {
                RECT Rect;
                Rect.left = atol(F[1].c_str());
                Rect.top = atol(F[2].c_str());
                Rect.right = atol(F[3].c_str());
                Rect.bottom = atol(F[4].c_str());
                ClipNode Node(Rect);
                Append(Rdl, Node);
            }
            else if (Kind == "y")
            {
                StyleNode Node((unsigned short)strtoul(F[1].c_str(), nullptr, 10));
                Append(Rdl, Node);
            }
            else if (Kind == "0")
            {
                NullNode Node;
                Append(Rdl, Node);
            }
            else if (Kind == "i")
            {
                if (F[1] == "-")
                {
                    ImageNode Node(nullptr);
                    Append(Rdl, Node);
                }
                else
                {
                    Images().push_back(FOracleImage(FloatFromBits((unsigned int)HexWord(F[1])),
                                                    FloatFromBits((unsigned int)HexWord(F[2]))));
                    ImageNode Node(&Images().back());
                    Append(Rdl, Node);
                }
            }
            else
            {
                fprintf(stderr, "rich_text: bad token %s\n", Token.c_str());
                GFailed = true;
            }
        }
    }

    std::string NodeText(RDLNode* Node)
    {
        if (TRANode* Tra = Node->operator TRANode*())
            return Format("a:%08lx:%08x:%08x", Tra->get_attributes().Packed, Tra->get_mask(),
                          Tra->get_default_mask());
        if (TextNode* Text = Node->operator TextNode*())
            return "t:" + Units(Text->get_text().c_str(), (int)Text->get_text().size());
        if (TextPtrNode* Ptr = Node->operator TextPtrNode*())
            return "p:" + Units(Ptr->get_text(), Ptr->get_text_length());
        if (AttribStackNode* Stack = Node->operator AttribStackNode*())
            return Format("s:%d", (int)Stack->get_operation());
        if (PositionNode* Pos = Node->operator PositionNode*())
            return Format("x:%d:%d:%08x:%08x", Pos->is_rel_h() ? 1 : 0, Pos->is_rel_v() ? 1 : 0,
                          FloatBits(Pos->get_pos_h()), FloatBits(Pos->get_pos_v()));
        if (Node->operator ParagraphNode*() != nullptr)
            return "n";
        if (JustifyNode* Justify = Node->operator JustifyNode*())
            return Format("j:%d", (int)Justify->get_justify());
        if (ClipNode* Clip = Node->operator ClipNode*())
        {
            const tagRECT& R = Clip->get_rect();
            return Format("c:%ld:%ld:%ld:%ld", R.left, R.top, R.right, R.bottom);
        }
        if (StyleNode* Style = Node->operator StyleNode*())
            return Format("y:%u", (unsigned int)Style->get_style_id());
        if (Node->operator NullNode*() != nullptr)
            return "0";
        if (ImageNode* Image = Node->operator ImageNode*())
        {
            const TextRenderContext::Image* Img = Image->get_image();
            if (Img == nullptr)
                return "i:-";
            return Format("i:%08x:%08x", FloatBits(Img->Width), FloatBits(Img->Height));
        }
        if (Node->operator RDLRefNode*() != nullptr)
            return "ref";
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

    // One "layout" case: LayoutRichText, then the laid list executed with highlight from the
    // default attributes at (0, 0) with the origin (3, 5), its dimensions, and the caret
    // positions of the source list.
    std::string LayoutCase(unsigned int Case, const std::vector<std::string>& T)
    {
        const int Width = atoi(T[1].c_str());
        const int OriginH = atoi(T[2].c_str());
        const int OriginV = atoi(T[3].c_str());
        const bool bSingle = T[4] == "1";
        const bool bWrap = T[5] == "1";
        const unsigned long Attrs = HexWord(T[6]);
        RenderDisplayList Source;
        size_t At = 7;
        Build(Source, T, At);

        FRecordingTRC Ctx;
        Ctx.Begin(Attrs);
        RenderDisplayList Laid;
        int Height = -12345;
        const bool bResult =
            LayoutRichText(Ctx, Source, Width, Laid, OriginH, OriginV, bSingle, &Height, bWrap);
        const std::string LayoutLog = Ctx.Log;
        const std::string After = Ctx.After();

        Ctx.Log.clear();
        Ctx.reset_to_default();
        Ctx.set_pos(0, 0);
        Ctx.set_origin(3, 5);
        Laid.execute(Ctx, true);
        const std::string ExecLog = Ctx.Log;
        const std::string ExecAfter = Ctx.After();

        Ctx.Log.clear();
        float W = 0.0f;
        float H = 0.0f;
        float D = 0.0f;
        Laid.get_dimensions(Ctx, W, H, D);
        const std::string Dims = Format("%08x:%08x:%08x", FloatBits(W), FloatBits(H), FloatBits(D));
        const std::string DimsLog = Ctx.Log;

        const int Count = Source.get_element_count();
        std::string Positions;
        const int Probes[5] = {0, 1, Count / 2, Count - 1, Count};
        for (int Probe = 0; Probe < 5; ++Probe)
        {
            Ctx.Begin(Attrs);
            float PH = 0.0f;
            float PV = 0.0f;
            float PD = 0.0f;
            Source.get_element_pos(Ctx, Probes[Probe], PH, PV, PD);
            if (!Positions.empty())
                Positions += " ";
            Positions += Format("%d=%08x:%08x:%08x", Probes[Probe], FloatBits(PH), FloatBits(PV),
                                FloatBits(PD));
        }

        return Format("{\"case\":\"%u\",\"result\":\"%d\",\"height\":\"%d\"", Case, bResult ? 1 : 0,
                      Height) +
               ",\"laid\":\"" + ListText(Laid) + "\",\"layout\":\"" + LayoutLog +
               "\",\"after\":\"" + After + "\",\"exec\":\"" + ExecLog + "\",\"exec_after\":\"" +
               ExecAfter + "\",\"dims\":\"" + Dims + "\",\"dims_log\":\"" + DimsLog +
               "\",\"count\":\"" + Format("%d", Count) + "\",\"pos\":\"" + Positions + "\"}";
    }

    void RichTextSuite()
    {
        Suite Out("rich_text.jsonl");
        unsigned int Case = 0;
        for (const std::string& Line : InputLines("rich_text.list"))
        {
            const std::vector<std::string> T = Tokens(Line);
            if (T.empty())
                continue;
            if (T[0] == "style" && T.size() >= 2)
            {
                RenderDisplayList Style;
                size_t At = 2;
                Build(Style, T, At);
                StyleCollection::set_style((unsigned short)strtoul(T[1].c_str(), nullptr, 10), Style);
                Out.Line(Format("{\"case\":\"%u\",\"style\":\"%s\",\"nodes\":\"", Case, T[1].c_str()) +
                         ListText(StyleCollection::get_style(
                             (unsigned short)strtoul(T[1].c_str(), nullptr, 10))) +
                         "\"}");
            }
            else if (T[0] == "styleattr" && T.size() == 3)
            {
                TextRenderAttributes Attrs;
                Attrs.Packed = HexWord(T[2]);
                StyleCollection::set_style((unsigned short)strtoul(T[1].c_str(), nullptr, 10), Attrs);
                Out.Line(Format("{\"case\":\"%u\",\"style\":\"%s\",\"nodes\":\"", Case, T[1].c_str()) +
                         ListText(StyleCollection::get_style(
                             (unsigned short)strtoul(T[1].c_str(), nullptr, 10))) +
                         "\"}");
            }
            else if (T[0] == "layout" && T.size() >= 7)
            {
                Out.Line(LayoutCase(Case, T));
            }
            else if (T[0] == "lex" && T.size() >= 2)
            {
                RenderDisplayList A;
                RenderDisplayList B;
                std::vector<std::string> Left;
                std::vector<std::string> Right;
                bool bRight = false;
                for (size_t Index = 2; Index < T.size(); ++Index)
                {
                    if (T[Index] == "/")
                        bRight = true;
                    else
                        (bRight ? Right : Left).push_back(T[Index]);
                }
                size_t At = 0;
                Build(A, Left, At);
                At = 0;
                Build(B, Right, At);
                const int Result = lex_compare(A, B, T[1] == "1");
                const int Self = lex_compare(A, A, T[1] == "1");
                Out.Line(Format("{\"case\":\"%u\",\"lex\":\"%d\",\"self\":\"%d\"}", Case, Result, Self));
            }
            else
            {
                fprintf(stderr, "rich_text: bad line %s\n", Line.c_str());
                GFailed = true;
            }
            ++Case;
        }
    }
}

void RunRichTextSuites()
{
    RichTextSuite();
    RunRichFontsSuite();
}
