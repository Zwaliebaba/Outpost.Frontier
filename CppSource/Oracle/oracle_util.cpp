// Oracle suites of work package P1.5 (design/migration-plan.md): Rect, the
// ErrorHandler log channels, and through the files beside this one the
// Catmull-Rom segment (oracle_util_spline.cpp), Timing::Delta
// (oracle_util_timing.cpp) and the MD5 text form (oracle_util_md5.cpp).
// The Rust test crates/neuron-core/tests/oracle_util.rs renders the same
// lines from the inputs under tests/fixtures/oracle/util/.

#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "oracle_support.h"

#include "neuroncomponents.h"
#include "common_errorhandler.h"
#include "common_text.h"

using namespace oracle;

void RunSplineSuite();
void RunTimingSuite();
void RunMd5Suites();

namespace
{
    std::string RectText(const Rect& R)
    {
        return Format("%d,%d,%d,%d", R.Left, R.Top, R.Width, R.Height);
    }

    void Rects()
    {
        Suite Out("rect.jsonl");
        for (const std::string& Line : InputLines("util/rect.list"))
        {
            int V[10] = {};
            if (sscanf(Line.c_str(), "%d %d %d %d %d %d %d %d %d %d", &V[0], &V[1], &V[2], &V[3], &V[4],
                       &V[5], &V[6], &V[7], &V[8], &V[9]) != 10)
            {
                fprintf(stderr, "bad rect line %s\n", Line.c_str());
                GFailed = true;
                continue;
            }
            const Rect A(V[0], V[1], V[2], V[3]);
            const Rect B(V[4], V[5], V[6], V[7]);
            const int X = V[8];
            const int Y = V[9];
            int Cx = 0;
            int Cy = 0;
            A.get_center(&Cx, &Cy);
            const ViewRect View = Rect_to_ViewRect(A);
            const tagRECT Win = Rect_to_RECT(A);
            ViewRect BView;
            BView.Left = B.Left;
            BView.Top = B.Top;
            BView.Right = B.Width;
            BView.Bottom = B.Height;
            tagRECT BWin;
            BWin.left = B.Left;
            BWin.top = B.Top;
            BWin.right = B.Width;
            BWin.bottom = B.Height;
            Rect Ltrb;
            Ltrb.init_from_ltrb(B.Left, B.Top, B.Width, B.Height);
            std::string Text = "{\"a\":\"" + RectText(A) + "\",\"b\":\"" + RectText(B) + "\"";
            Text += Format(",\"p\":\"%d,%d\",\"right\":\"%d\",\"bottom\":\"%d\",\"area\":\"%d\",\"null\":\"%d\","
                           "\"nonnull\":\"%d\",\"center\":\"%d,%d\",\"inside\":\"%d\",\"eq\":\"%d\",\"ne\":\"%d\"",
                           X, Y, A.rightmost(), A.bottommost(), A.area(), A.null() ? 1 : 0,
                           A.nonnull() ? 1 : 0, Cx, Cy, A.point_is_inside(X, Y) ? 1 : 0, (A == B) ? 1 : 0,
                           (A != B) ? 1 : 0);
            Text += Format(",\"view\":\"%d,%d,%d,%d\",\"win\":\"%d,%d,%d,%d\"", View.Left, View.Top, View.Right,
                           View.Bottom, (int)Win.left, (int)Win.top, (int)Win.right, (int)Win.bottom);
            Text += ",\"from_view\":\"" + RectText(ViewRect_to_Rect(BView)) + "\"";
            Text += ",\"from_win\":\"" + RectText(RECT_to_Rect(BWin)) + "\"";
            Text += ",\"ltrb\":\"" + RectText(Ltrb) + "\"";
            Text += ",\"isect\":\"" + RectText(rect_intersect(A, B)) + "\"";
            Text += ",\"union\":\"" + RectText(rect_union(A, B)) + "\"}";
            Out.Line(Text);
        }
    }

    // A script argument: hex text, or "-" for the empty string.
    Bytes TextArg(const std::string& Arg)
    {
        return Arg == "-" ? Unhex(std::string()) : Unhex(Arg);
    }

    std::vector<std::string> Split(const std::string& Line)
    {
        std::vector<std::string> Parts;
        size_t Start = 0;
        for (;;)
        {
            const size_t Space = Line.find(' ', Start);
            Parts.push_back(Line.substr(Start, Space == std::string::npos ? std::string::npos : Space - Start));
            if (Space == std::string::npos)
                break;
            Start = Space + 1;
        }
        while (Parts.size() < 6)
            Parts.push_back(std::string());
        return Parts;
    }

    // ErrorHandler driven by util/diag.script. The log files are created in a
    // scratch directory under %TEMP%, which is the current directory while the
    // script runs; "read" prints a file and deletes it.
    void DiagLog()
    {
        const std::vector<std::string> Script = InputLines("util/diag.script");
        Suite Out("diag_log.jsonl");

        char TempPath[MAX_PATH] = {};
        GetTempPathA(MAX_PATH, TempPath);
        const std::string Scratch = std::string(TempPath) + "outpost_oracle_diag";
        CreateDirectoryA(Scratch.c_str(), nullptr);
        char Saved[MAX_PATH] = {};
        GetCurrentDirectoryA(MAX_PATH, Saved);
        if (!SetCurrentDirectoryA(Scratch.c_str()))
        {
            fprintf(stderr, "cannot enter %s\n", Scratch.c_str());
            GFailed = true;
            return;
        }
        // Files a crashed earlier run may have left.
        for (const std::string& Line : Script)
        {
            const std::vector<std::string> Args = Split(Line);
            if (Args[0] == "read")
                DeleteFileA(CStr(TextArg(Args[1])));
        }

        for (const std::string& Line : Script)
        {
            const std::vector<std::string> Args = Split(Line);
            const std::string& Op = Args[0];
            if (Op == "ini")
            {
                const std::string Path = GFixtureDir + "\\util\\" + Args[1];
                ErrorHandler::activate(Path.c_str(), nullptr);
                Out.Line("{\"op\":\"ini\",\"file\":\"" + Args[1] + "\"}");
            }
            else if (Op == "new")
            {
                const Bytes Name = TextArg(Args[1]);
                const bool bOk = ErrorHandler::new_log(CStr(Name), atoi(Args[2].c_str()), atoi(Args[3].c_str()),
                                                       Args[4] == "1", Args[5] == "1") != nullptr;
                Out.Line("{\"op\":\"new\",\"name\":\"" + HexC(CStr(Name)) + "\",\"ok\":\"" + (bOk ? "1" : "0") +
                         "\"}");
            }
            else if (Op == "dump")
            {
                const int Flags = (int)strtoul(Args[1].c_str(), nullptr, 16);
                const Bytes Text = TextArg(Args[2]);
                FDUMP(Flags, "%s", CStr(Text));
            }
            else if (Op == "comment")
            {
                const Bytes Text = TextArg(Args[2]);
                ErrorHandler::log_comment(atoi(Args[1].c_str()), "%s", CStr(Text));
            }
            else if (Op == "enable")
            {
                ErrorHandler::log_enable(atoi(Args[1].c_str()));
            }
            else if (Op == "disable")
            {
                ErrorHandler::log_disable(atoi(Args[1].c_str()));
            }
            else if (Op == "enabled")
            {
                const bool bEnabled = ErrorHandler::is_log_enabled(atoi(Args[1].c_str()));
                Out.Line("{\"op\":\"enabled\",\"id\":\"" + Args[1] + "\",\"r\":\"" + (bEnabled ? "1" : "0") +
                         "\"}");
            }
            else if (Op == "close")
            {
                ErrorHandler::deactivate();
                Out.Line("{\"op\":\"close\"}");
            }
            else if (Op == "read")
            {
                const Bytes Name = TextArg(Args[1]);
                Bytes Data;
                if (ReadWholeFile(CStr(Name), Data))
                {
                    Out.Line("{\"op\":\"read\",\"name\":\"" + Args[1] + "\",\"data\":\"" +
                             Hex(Data.data(), Data.size()) + "\"}");
                    DeleteFileA(CStr(Name));
                }
                else
                {
                    Out.Line("{\"op\":\"read\",\"name\":\"" + Args[1] + "\",\"missing\":\"1\"}");
                }
            }
            else
            {
                fprintf(stderr, "unknown script command %s\n", Op.c_str());
                GFailed = true;
            }
        }
        // ErrorHandler::deactivate restored the harness's FDUMP.
        SetCurrentDirectoryA(Saved);
        RemoveDirectoryA(Scratch.c_str());
    }
}

void RunUtilSuites()
{
    Rects();
    RunSplineSuite();
    RunMd5Suites();
    RunTimingSuite();
    DiagLog();
}
