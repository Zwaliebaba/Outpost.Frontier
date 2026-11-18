// Oracle.exe gdi-fonts <fixture directory> <output directory> (work package P4.12): GDI's text
// metrics on this machine, for crates/neuron-client/tests/gdi_font_metrics.rs, which compares
// them with the DirectWrite backend's (neuron-client::platform::dwrite) and runs the font
// autoscale search over both.
//
// Each line of tests/fixtures/oracle/gdi_fonts.list is "face|lfHeight|lfWeight|lfItalic". The
// face is realised with the LOGFONTA that FontImp::Init and BuildRichTextLogFont build
// (font_imp.cpp, common_textrender.cpp: DEFAULT_CHARSET, OUT_TT_PRECIS, ANTIALIASED_QUALITY,
// the name cut to 31 bytes) on a memory DC, and one line of gdi_fonts.jsonl records the face GDI
// chose (GetTextFaceA), its TEXTMETRICA (height, ascent, descent, internal and external leading,
// average and maximum character width), GetCharWidthW of U+0020..U+007E and
// GetTextExtentPointW of FontImp's calibration pangram. Every value is a decimal string.
//
// The output depends on the fonts installed on the machine that runs it, so this is a mode of
// its own and not one of the neuron-client suites; CI's oracle job runs it on its runner.

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <string>
#include <vector>

#include "oracle_support.h"

using namespace oracle;

namespace
{
    // font_imp.cpp kPangram.
    const wchar_t kPangram[] = L"In the beginning the Earth was without form and void.";

    std::vector<std::string> SplitFields(const std::string& Line)
    {
        std::vector<std::string> Fields;
        std::string Current;
        for (size_t Index = 0; Index < Line.size(); ++Index)
        {
            if (Line[Index] == '|')
            {
                Fields.push_back(Current);
                Current.clear();
            }
            else
            {
                Current += Line[Index];
            }
        }
        Fields.push_back(Current);
        return Fields;
    }

    // A face name as a JSON string body: printable ASCII kept, '"' and '\\' escaped, anything
    // else as \u00XX.
    std::string JsonText(const char* Text)
    {
        std::string Out;
        for (const unsigned char* Cursor = reinterpret_cast<const unsigned char*>(Text); *Cursor;
             ++Cursor)
        {
            const unsigned char Ch = *Cursor;
            if (Ch == '"' || Ch == '\\')
            {
                Out += '\\';
                Out += static_cast<char>(Ch);
            }
            else if (Ch >= 0x20 && Ch < 0x7f)
            {
                Out += static_cast<char>(Ch);
            }
            else
            {
                Out += Format("\\u%04x", static_cast<unsigned>(Ch));
            }
        }
        return Out;
    }

    void GdiFontsSuite()
    {
        Suite Out("gdi_fonts.jsonl");
        const std::vector<std::string> Lines = InputLines("gdi_fonts.list");
        HDC Dc = CreateCompatibleDC(nullptr);
        if (Dc == nullptr)
        {
            fprintf(stderr, "CreateCompatibleDC failed\n");
            GFailed = true;
            return;
        }
        for (size_t Case = 0; Case < Lines.size(); ++Case)
        {
            const std::vector<std::string> Fields = SplitFields(Lines[Case]);
            if (Fields.size() != 4)
            {
                fprintf(stderr, "gdi_fonts.list: bad line %u\n", static_cast<unsigned>(Case));
                GFailed = true;
                continue;
            }
            const std::string Head = Format(
                "{\"case\":\"%u\",\"face\":\"%s\",\"height\":\"%d\",\"weight\":\"%d\",\"italic\":\"%d\"",
                static_cast<unsigned>(Case), JsonText(Fields[0].c_str()).c_str(),
                atoi(Fields[1].c_str()), atoi(Fields[2].c_str()), atoi(Fields[3].c_str()) != 0 ? 1 : 0);

            LOGFONTA LogFont;
            memset(&LogFont, 0, sizeof(LogFont));
            LogFont.lfHeight = atoi(Fields[1].c_str());
            LogFont.lfWeight = atoi(Fields[2].c_str());
            LogFont.lfItalic = atoi(Fields[3].c_str()) != 0 ? 1 : 0;
            LogFont.lfCharSet = 1;      // DEFAULT_CHARSET
            LogFont.lfOutPrecision = 4; // OUT_TT_PRECIS
            LogFont.lfClipPrecision = 0;
            LogFont.lfQuality = 4;      // ANTIALIASED_QUALITY
            LogFont.lfPitchAndFamily = 0;
            strncpy(LogFont.lfFaceName, Fields[0].c_str(), 0x1f);
            LogFont.lfFaceName[0x1f] = '\0';

            HFONT Font = CreateFontIndirectA(&LogFont);
            if (Font == nullptr)
            {
                Out.Line(Head + ",\"error\":\"CreateFontIndirectA\"}");
                continue;
            }
            HGDIOBJ const Previous = SelectObject(Dc, Font);

            char Face[LF_FACESIZE + 1];
            memset(Face, 0, sizeof(Face));
            GetTextFaceA(Dc, LF_FACESIZE, Face);

            TEXTMETRICA Metrics;
            memset(&Metrics, 0, sizeof(Metrics));
            GetTextMetricsA(Dc, &Metrics);

            std::string Widths;
            for (UINT Ch = 0x20; Ch < 0x7f; ++Ch)
            {
                INT Width = 0;
                GetCharWidthW(Dc, Ch, Ch, &Width);
                if (Ch != 0x20)
                    Widths += ' ';
                Widths += Format("%d", Width);
            }

            SIZE Extent;
            Extent.cx = 0;
            Extent.cy = 0;
            GetTextExtentPointW(Dc, kPangram, static_cast<int>(wcslen(kPangram)), &Extent);

            Out.Line(Head +
                     Format(",\"text_face\":\"%s\",\"tm\":\"%d %d %d %d %d %d %d\",\"widths\":\"%s\","
                            "\"pangram\":\"%d %d\"}",
                            JsonText(Face).c_str(), static_cast<int>(Metrics.tmHeight),
                            static_cast<int>(Metrics.tmAscent), static_cast<int>(Metrics.tmDescent),
                            static_cast<int>(Metrics.tmInternalLeading),
                            static_cast<int>(Metrics.tmExternalLeading),
                            static_cast<int>(Metrics.tmAveCharWidth),
                            static_cast<int>(Metrics.tmMaxCharWidth), Widths.c_str(),
                            static_cast<int>(Extent.cx), static_cast<int>(Extent.cy)));

            SelectObject(Dc, Previous);
            DeleteObject(Font);
        }
        DeleteDC(Dc);
    }
}

int RunGdiFontsMode(int Argc, char** Argv)
{
    if (Argc != 4)
    {
        fprintf(stderr, "usage: Oracle.exe gdi-fonts <fixture directory> <output directory>\n");
        return 2;
    }
    GFixtureDir = Argv[2];
    GOutputDir = Argv[3];
    if (!SetCurrentDirectoryA(GFixtureDir.c_str()))
    {
        fprintf(stderr, "cannot enter %s\n", GFixtureDir.c_str());
        return 2;
    }
    GdiFontsSuite();
    return GFailed ? 1 : 0;
}
