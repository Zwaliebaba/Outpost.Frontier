// Oracle instrumentation (work package P0.5, design/reference-build.md section 8).

// The client's `+oracle` mode: fixed display and time step, scripted session, dumps.
//
// Command line
//
//     OutpostFrontier.exe +oracle <script file> <output folder>
//
// `+oracle` is matched as a whole token, in any case; the two paths follow it as separate
// tokens (double quotes allowed, as for every token). Relative paths are taken from the
// executable's folder, which WinMain makes the current directory. The output folder is
// created when missing (its parent must exist); existing files in it are overwritten.
//
// Script format
//
// One command per line. Blank lines and lines starting with '#' are skipped. A command is a
// word, then its arguments separated by blanks; the last argument of `dump`, `tree`, `shot`,
// `click` and `log` is the rest of the line. The script runs at the start of each frame
// (TickClientFrame), one line after another, until a line makes it wait; every executed
// line is written to session.jsonl with its result.
//
//   wait <frames>                      continue <frames> frames later (0: no wait)
//   waitscreen <kind> <id> <frames>    continue once the latest screen change of <kind>
//                                      (client, menu or room) has <id>, at once if it already
//                                      has; give up after <frames> frames (result "timeout")
//   dump [label]                       interface tree and screenshot now
//   tree [label]                       interface tree only
//   shot [label]                       screenshot only
//   command <USER_NICKNAME>            dispatch the key-map command as a matching key press
//                                      would: the event record is filled from the command's
//                                      first active binding (else its first binding), then
//                                      HudDispatchCommand(id, 1) for key down when the
//                                      command's state mask has keydown, and again for key up
//                                      when it has keyup. The raw-key dispatch that comes
//                                      first for a real key and the held-key list are skipped
//   key <virtual key>                  post WM_KEYDOWN and WM_KEYUP for the key (decimal or
//                                      0x hex) to the game window; the message pump routes
//                                      them like a real key. Shift, Ctrl and Alt are read from
//                                      the real keyboard, as for a real key
//   click <target>                     send a left-button press {-1, 2} and release {-1, 1}
//                                      straight to the target's MouseEvent (Win3D slot 19).
//                                      <target> is a widget name (first in dump order), `#<n>`
//                                      (node n of a tree dump taken now), or `text:<caption>`
//                                      (first TextWin / TextControl whose text equals the
//                                      caption, ASCII case ignored, visible nodes first). The
//                                      pick, hover and capture walk are bypassed, so this works
//                                      for message buttons (TextMsgButton, UITextMsgButton) but
//                                      not for NeuroNet buttons, which test the cursor on release
//   state <client state id>            G_PendingClientStateId = id: the frame enters that
//                                      client state (client_state.h; 6 is the main menu)
//   log <text>                         a marker in session.jsonl
//   quit                               client state 1 (the client's own quit) and stop
//
// The end of the script acts as `quit`. A command that opens a modal dialog with its own
// message loop (NnErrorRunModal, NnPromptRunYesNo) stops the script until the dialog closes,
// because TickClientFrame does not run meanwhile.
//
// Output files (one JSON object per line, floats as 8 hex digits, doubles as 16)
//
//   session.jsonl  start, display, seed, screen changes, script lines, dumps, quit
//   tree.jsonl     per dump a "dump" head record, then one record per Win3D node
//   keymap.jsonl   the parsed KeyMap.ini (phase "base") and after UserKeyMap.ini ("user")
//   layout.jsonl   PAGE descriptors, [BaseFrame] control INI subtrees, keylist.ini,
//                  optlist.ini, the HUD element list
//   iclient.jsonl  every IClient / IChatClient call (oracle_iclient_args.h)
//   shot_NNNN_<label>.bmp  screenshots, NNNN counting from 0001
//
// design/reference-build.md section 8 has every record layout.

#include "globals.h"
#include "oracle_mode.h"
#include "oracle_json.h"

#include "client_state.h"          // G_PendingClientStateId
#include "hud_command_dispatch.h"  // HudCommandRecord, HudDispatchCommand
#include "hud_element_list.h"      // HUDItem
#include "menu_keymap_config.h"    // FKeyCmd, FKeyMapGroup, the oracle views
#include "nn_preferences.h"        // NnPreferences, FKeyRow, FPrefOption
#include "nn_text_control.h"       // NnTextControl
#include "text_win.h"              // TextWin
#include "ui_page_desc.h"          // FUiPageData, FUiWidgetEntry, payloads
#include "win3d.h"
#include "wincontrol_base.h"

#include "NeuronRuntime.h"
#include "common_csys.h"           // Csys
#include "common_goods.h"          // RoomData and its records
#include "oracle_sim_hooks.h"      // GOracleSimHooks: the in-process server's fixed clock
#include "neuroncomponents.h"
#include "render/IRenderPipeline.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <typeinfo>
#include <vector>
#include <windows.h>

namespace
{
    double const kOracleFrameSeconds = 1.0 / 30.0;
    // srand seed in oracle mode ("ORAC").
    unsigned const kOracleRandSeed = 0x4f524143u;
    // The in-process server's clock at frame 0 under the fixed clock: Oracle.exe sim's
    // default ClockStart (Oracle/oracle_sim.cpp).
    double const kOracleServerClockStart = 1000.0;
    // frames between a screen change and its automatic dump (one second of game time).
    unsigned const kOracleSettleFrames = 30u;
    int const kOracleWidth = 0x400;
    int const kOracleHeight = 0x300;
    int const kOracleColorBits = 0x20;
    // bounds on one tree walk, against a corrupt tree.
    unsigned const kMaxTreeNodes = 100000u;
    int const kMaxTreeDepth = 64;

    char const* const kStreamFiles[kOracleStreamCount] = {
        "session.jsonl", "tree.jsonl", "keymap.jsonl", "layout.jsonl", "iclient.jsonl"};

    struct FOracleState
    {
        bool bActive;
        std::string OutDir;
        FILE* Files[kOracleStreamCount];
        CRITICAL_SECTION Lock;
        unsigned Frame;
        volatile LONG IClientSeq;

        std::vector<std::string> Script;
        size_t NextLine;
        unsigned WaitUntil;
        bool bWaitScreen;
        std::string WaitKind;
        unsigned WaitId;
        unsigned WaitDeadline;
        unsigned WaitLine;
        bool bDone;

        unsigned AutoDumpFrame;
        std::string AutoLabel;
        // the latest screen change of each kind; -1 (all bits) before the first.
        unsigned LastClient;
        unsigned LastMenu;
        unsigned LastRoom;

        unsigned DumpCount;
        unsigned ShotCount;
    };

    FOracleState G;

    // A node of a flattened Win3D tree: pre-order, roots in registry order.
    struct FNode
    {
        Win3D* Object;
        int Parent;
        int Depth;
        bool bVisible; // its own and every ancestor's kWin3DVisible bit
    };

    [[noreturn]] void Fail(char const* Message, char const* Detail)
    {
        std::string Text = "OutpostFrontier +oracle: ";
        Text += Message;
        if (Detail != nullptr)
        {
            Text += ": ";
            Text += Detail;
        }
        Text += "\n";
        OutputDebugStringA(Text.c_str());
        fputs(Text.c_str(), stderr);
        ExitProcess(3);
    }

    std::string Trim(std::string const& Text)
    {
        size_t Begin = 0;
        size_t End = Text.size();
        while (Begin < End && isspace(static_cast<unsigned char>(Text[Begin])) != 0)
            ++Begin;
        while (End > Begin && isspace(static_cast<unsigned char>(Text[End - 1])) != 0)
            --End;
        return Text.substr(Begin, End - Begin);
    }

    // Splits off the first blank-separated word; Rest gets the trimmed remainder.
    std::string FirstWord(std::string const& Text, std::string* Rest)
    {
        size_t End = 0;
        while (End < Text.size() && isspace(static_cast<unsigned char>(Text[End])) == 0)
            ++End;
        *Rest = Trim(Text.substr(End));
        return Text.substr(0, End);
    }

    bool ParseUnsigned(std::string const& Text, unsigned* Out)
    {
        if (Text.empty())
            return false;
        // decimal, or hex after "0x" (no octal: "030" is thirty).
        bool const bHex = Text.size() > 2 && Text[0] == '0' && (Text[1] == 'x' || Text[1] == 'X');
        char* End = nullptr;
        unsigned long const Value = strtoul(Text.c_str(), &End, bHex ? 16 : 10);
        if (End == nullptr || *End != '\0')
            return false;
        *Out = static_cast<unsigned>(Value);
        return true;
    }

    std::string SanitizeLabel(std::string const& Label)
    {
        std::string Out;
        for (size_t Index = 0; Index < Label.size() && Out.size() < 48; ++Index)
        {
            char const Ch = Label[Index];
            if (isalnum(static_cast<unsigned char>(Ch)) != 0 || Ch == '-' || Ch == '_' ||
                Ch == '.')
                Out.push_back(Ch);
            else
                Out.push_back('_');
        }
        if (Out.empty())
            Out = "dump";
        return Out;
    }

    void Head(FOracleJson& J, char const* Rec)
    {
        J.Uint("frame", G.Frame);
        J.Str("rec", Rec);
    }

    void Session(FOracleJson& J)
    {
        OracleWriteRecord(kOracleSession, J.Finish());
    }

    // ---- interface tree -----------------------------------------------------------------

    void CollectRoot(Win3D* Object, void* Context)
    {
        if (Object == nullptr || Object->GetParent() != nullptr)
            return;
        if ((Object->Flags & kWin3DUnregistered) != 0)
            return;
        static_cast<std::vector<Win3D*>*>(Context)->push_back(Object);
    }

    void FlattenFrom(Win3D* Object, int Parent, int Depth, bool bParentVisible,
                     std::vector<FNode>& Out)
    {
        if (Object == nullptr || Out.size() >= kMaxTreeNodes || Depth > kMaxTreeDepth)
            return;
        FNode Node;
        Node.Object = Object;
        Node.Parent = Parent;
        Node.Depth = Depth;
        Node.bVisible = bParentVisible && (Object->Flags & kWin3DVisible) != 0;
        int const Self = static_cast<int>(Out.size());
        Out.push_back(Node);
        for (Win3D* Child = Object->GetChildHead(); Child != nullptr;
             Child = Child->GetNextSibling())
        {
            if (Out.size() >= kMaxTreeNodes)
                return;
            FlattenFrom(Child, Self, Depth + 1, Node.bVisible, Out);
        }
    }

    void FlattenAll(std::vector<FNode>& Out)
    {
        std::vector<Win3D*> Roots;
        Win3DForEachRegistered(CollectRoot, &Roots);
        for (size_t Index = 0; Index < Roots.size(); ++Index)
            FlattenFrom(Roots[Index], -1, 0, true, Out);
    }

    char const* RttiName(void const* Object, std::type_info const& Info)
    {
        return Object != nullptr ? Info.name() : nullptr;
    }

    // The fields of one node, after the caller's own keys.
    void NodeFields(FOracleJson& J, FNode const& Node, int Index)
    {
        Win3D* const Object = Node.Object;
        J.Int("i", Index);
        J.Int("p", Node.Parent);
        J.Int("d", Node.Depth);
        J.Str("class", Object->ClassName);
        J.Str("rtti", RttiName(Object, typeid(*Object)));
        J.StrN("name", Object->Name, sizeof(Object->Name));
        // Bits 0-3 only: the constructor keeps bits 4-7 from the heap (`Flags & 0xf8`, as the
        // original does), so they differ from run to run and carry no state.
        J.Uint("flags", Object->Flags & 0x0f);
        J.Bool("vis", Node.bVisible);
        J.Uint("layer", Object->DrawLayer);
        J.Int("sub", Object->DrawSubLayer);
        J.Int("hit", Object->HitPriority);
        J.FloatArray("pos", Object->Position, 3);
        J.FloatArray("orient", Object->Orientation, 9);

        if (WinControl* const Control = dynamic_cast<WinControl*>(Object))
        {
            J.BeginObject("ctl");
            J.FloatArray("offset", Control->Offset, 3);
            J.FloatArray("half", Control->HalfExtents, 3);
            J.FloatArray("hitoff", Control->HitOffset, 3);
            J.Int("tooltip", Control->ToolTipIds);
            J.StrN("mesh", Control->MeshPath, sizeof(Control->MeshPath));
            J.StrN("hp", Control->HardpointName, sizeof(Control->HardpointName));
            J.Uint("nz", Control->bNoZEnable);
            J.EndObject();
        }
        if (TextWin* const Text = dynamic_cast<TextWin*>(Object))
        {
            J.WStr("text", Text->Text.Text);
            J.BeginObject("tw");
            J.Float("scale", Text->TextScale);
            J.Float("w", Text->Width);
            J.Float("h", Text->Height);
            J.EndObject();
        }
        else if (NnTextControl* const Label = dynamic_cast<NnTextControl*>(Object))
        {
            FFontString const* const String = Label->GetFontString();
            J.WStr("text", String != nullptr ? String->Text : nullptr);
            J.BeginObject("tc");
            J.Float("w", Label->Width);
            J.Float("h", Label->Height);
            J.EndObject();
        }
    }

    // The text a TextWin or TextControl shows, or null.
    wchar_t const* NodeText(Win3D* Object)
    {
        if (TextWin* const Text = dynamic_cast<TextWin*>(Object))
            return Text->Text.Text;
        if (NnTextControl* const Label = dynamic_cast<NnTextControl*>(Object))
        {
            FFontString const* const String = Label->GetFontString();
            return String != nullptr ? String->Text : nullptr;
        }
        return nullptr;
    }

    bool CaptionEquals(wchar_t const* Text, std::string const& Caption)
    {
        if (Text == nullptr)
            return false;
        size_t Index = 0;
        for (; Index < Caption.size(); ++Index)
        {
            wchar_t const Ch = Text[Index];
            if (Ch == L'\0' || Ch > 0x7f)
                return false;
            if (toupper(static_cast<unsigned char>(Ch)) !=
                toupper(static_cast<unsigned char>(Caption[Index])))
                return false;
        }
        return Text[Index] == L'\0';
    }

    void WriteTree(unsigned DumpId, std::string const& Label, std::string const& ShotName)
    {
        std::vector<FNode> Nodes;
        FlattenAll(Nodes);
        unsigned Roots = 0;
        for (size_t Index = 0; Index < Nodes.size(); ++Index)
        {
            if (Nodes[Index].Parent < 0)
                ++Roots;
        }

        FOracleJson HeadJ;
        Head(HeadJ, "dump");
        HeadJ.Uint("dump", DumpId);
        HeadJ.Str("label", Label.c_str());
        HeadJ.Int("client", static_cast<int>(G.LastClient));
        HeadJ.Int("menu", static_cast<int>(G.LastMenu));
        HeadJ.Uint("room", G.LastRoom);
        if (ShotName.empty())
            HeadJ.Null("shot");
        else
            HeadJ.Str("shot", ShotName.c_str());
        HeadJ.Uint("roots", Roots);
        HeadJ.Uint("nodes", static_cast<unsigned>(Nodes.size()));
        OracleWriteRecord(kOracleTree, HeadJ.Finish());

        for (size_t Index = 0; Index < Nodes.size(); ++Index)
        {
            FOracleJson J;
            J.Uint("dump", DumpId);
            NodeFields(J, Nodes[Index], static_cast<int>(Index));
            OracleWriteRecord(kOracleTree, J.Finish());
        }
    }

    // ---- screenshots --------------------------------------------------------------------

    // PrintScreen through the pipeline's IRPTexture face, as TakeClientScreenShot does; the
    // ".bmp" extension selects D3DX's BMP writer (D3DX 8.1 returns D3DXERR_INVALIDDATA for
    // PNG), as TakeClientScreenShot writes BMP. Returns the file name, empty on failure.
    std::string TakeShot(std::string const& Label)
    {
        G.ShotCount += 1;
        char Name[96];
        sprintf(Name, "shot_%04u_%s.bmp", G.ShotCount, SanitizeLabel(Label).c_str());
        std::string const Path = G.OutDir + "\\" + Name;

        IRenderPipeline* const Pipe = NeuronRuntime::Pipeline;
        if (Pipe == nullptr)
            return std::string();
        IRPTexture* Texture = nullptr;
        GENRESULT Result = GR_GENERIC;
        if (Pipe->QueryInterface(Iids::RPTexture, reinterpret_cast<void**>(&Texture)) == GR_OK &&
            Texture != nullptr)
            Result = Texture->PrintScreen(nullptr, Path.c_str());
        if (Texture != nullptr)
            Texture->Release();
        return Result == GR_OK ? std::string(Name) : std::string();
    }

    void Dump(std::string const& Label, bool bTree, bool bShot, char const* Cause)
    {
        G.DumpCount += 1;
        unsigned const DumpId = G.DumpCount;
        std::string ShotName;
        if (bShot)
            ShotName = TakeShot(Label);
        if (bTree)
            WriteTree(DumpId, Label, ShotName);

        FOracleJson J;
        Head(J, "dump");
        J.Uint("dump", DumpId);
        J.Str("cause", Cause);
        J.Str("label", Label.c_str());
        J.Bool("tree", bTree);
        if (!bShot)
            J.Null("shot");
        else if (ShotName.empty())
            J.Str("shot", "");
        else
            J.Str("shot", ShotName.c_str());
        Session(J);
    }

    // ---- script -------------------------------------------------------------------------

    unsigned* LastScreenOf(std::string const& Kind)
    {
        if (Kind == "client")
            return &G.LastClient;
        if (Kind == "menu")
            return &G.LastMenu;
        if (Kind == "room")
            return &G.LastRoom;
        return nullptr;
    }

    void Quit(char const* Why)
    {
        G.bDone = true;
        G_PendingClientStateId = 1;
        FOracleJson J;
        Head(J, "quit");
        J.Str("why", Why);
        Session(J);
    }

    Win3D* ResolveTarget(std::string const& Spec, std::string* Note)
    {
        std::vector<FNode> Nodes;
        FlattenAll(Nodes);
        if (!Spec.empty() && Spec[0] == '#')
        {
            unsigned Index = 0;
            if (!ParseUnsigned(Spec.substr(1), &Index) || Index >= Nodes.size())
            {
                *Note = "no such node";
                return nullptr;
            }
            return Nodes[Index].Object;
        }

        bool const bText = Spec.compare(0, 5, "text:") == 0;
        std::string const Key = bText ? Spec.substr(5) : Spec;
        Win3D* Fallback = nullptr;
        for (size_t Index = 0; Index < Nodes.size(); ++Index)
        {
            Win3D* const Object = Nodes[Index].Object;
            bool const bMatch = bText ? CaptionEquals(NodeText(Object), Key)
                                      : _stricmp(Object->Name, Key.c_str()) == 0;
            if (!bMatch)
                continue;
            if (Nodes[Index].bVisible)
                return Object;
            if (Fallback == nullptr)
                Fallback = Object;
        }
        if (Fallback == nullptr)
            *Note = "not found";
        return Fallback;
    }

    bool DoClick(std::string const& Spec, std::string* Note)
    {
        Win3D* const Target = ResolveTarget(Spec, Note);
        if (Target == nullptr)
            return false;
        FWin3DMouseEvent Press;
        Press.Field0 = -1;
        Press.Field1 = 2;
        FWin3DMouseEvent Release;
        Release.Field0 = -1;
        Release.Field1 = 1;
        bool const bPress = Target->MouseEvent(&Press);
        bool const bRelease = Target->MouseEvent(&Release);
        char Text[64];
        sprintf(Text, "press=%d release=%d", bPress ? 1 : 0, bRelease ? 1 : 0);
        *Note = Text;
        return true;
    }

    bool DoCommand(std::string const& Nickname, std::string* Note)
    {
        unsigned const Id = KeyMapGetActionNicknameId(Nickname.c_str());
        if (Id == 0)
        {
            *Note = "unknown nickname";
            return false;
        }
        FKeyCmd const* const Cmd = FindKeyCmdById(Id);
        unsigned const Mask = Cmd != nullptr ? Cmd->StateMask : kKeyCmdStateKeyDown;

        FKeyBindingLookup Binding;
        Binding.NicknameId = 0;
        Binding.VirtualKey = 0;
        Binding.Modifiers = 0;
        bool const bActive = KeyMapFindActiveBinding(Id, &Binding);
        if (!bActive)
        {
            Binding.VirtualKey = 0;
            Binding.Modifiers = 0;
            KeyMapFindPrimaryBinding(Id, &Binding);
        }

        FHudCmdRecord* const Record = HudCommandRecord();
        int Down = -1;
        int Up = -1;
        if ((Mask & kKeyCmdStateKeyDown) != 0)
        {
            memset(Record, 0, sizeof(*Record));
            Record->Key = static_cast<unsigned>(Binding.VirtualKey);
            Record->Modifiers = Binding.Modifiers;
            Record->Field0C = WM_KEYDOWN;
            Record->Arg10 = static_cast<unsigned>(Binding.VirtualKey);
            Record->Byte18 = 1;
            Down = HudDispatchCommand(Id, 1) != 0 ? 1 : 0;
        }
        if ((Mask & kKeyCmdStateKeyUp) != 0)
        {
            memset(Record, 0, sizeof(*Record));
            Record->Key = static_cast<unsigned>(Binding.VirtualKey);
            Record->Modifiers = Binding.Modifiers;
            Record->Field0C = WM_KEYUP;
            Record->Arg10 = static_cast<unsigned>(Binding.VirtualKey);
            Record->Byte18 = 0;
            Up = HudDispatchCommand(Id, 1) != 0 ? 1 : 0;
        }
        char Text[96];
        sprintf(Text, "id=%u active=%d vk=%d mods=%u down=%d up=%d", Id, bActive ? 1 : 0,
                Binding.VirtualKey, Binding.Modifiers, Down, Up);
        *Note = Text;
        return true;
    }

    bool DoKey(std::string const& Arg, std::string* Note)
    {
        unsigned Vk = 0;
        if (!ParseUnsigned(Arg, &Vk) || Vk == 0 || Vk > 0xff)
        {
            *Note = "bad virtual key";
            return false;
        }
        unsigned const Scan = MapVirtualKeyA(Vk, 0 /* MAPVK_VK_TO_VSC */) & 0xffu;
        LPARAM const DownParam = static_cast<LPARAM>(1u | (Scan << 16));
        LPARAM const UpParam = static_cast<LPARAM>(1u | (Scan << 16) | 0xc0000000u);
        bool const bDown = PostMessageA(g_client.window, WM_KEYDOWN, Vk, DownParam) != 0;
        bool const bUp = PostMessageA(g_client.window, WM_KEYUP, Vk, UpParam) != 0;
        *Note = (bDown && bUp) ? "posted" : "post failed";
        return bDown && bUp;
    }

    // Runs one script line. Returns false for an unknown command or bad arguments.
    bool Execute(std::string const& Command, std::string const& Rest, std::string* Note)
    {
        if (Command == "wait")
        {
            unsigned Frames = 0;
            if (!ParseUnsigned(Rest, &Frames))
            {
                *Note = "bad frame count";
                return false;
            }
            G.WaitUntil = G.Frame + Frames;
            return true;
        }
        if (Command == "waitscreen")
        {
            std::string Tail;
            std::string const Kind = FirstWord(Rest, &Tail);
            std::string Frames;
            std::string const IdText = FirstWord(Tail, &Frames);
            unsigned Id = 0;
            unsigned Max = 0;
            if (LastScreenOf(Kind) == nullptr || !ParseUnsigned(IdText, &Id) ||
                !ParseUnsigned(Frames, &Max))
            {
                *Note = "usage: waitscreen client|menu|room <id> <frames>";
                return false;
            }
            G.bWaitScreen = true;
            G.WaitKind = Kind;
            G.WaitId = Id;
            G.WaitDeadline = G.Frame + Max;
            G.WaitLine = static_cast<unsigned>(G.NextLine);
            return true;
        }
        if (Command == "dump")
        {
            Dump(Rest, true, true, "script");
            return true;
        }
        if (Command == "tree")
        {
            Dump(Rest, true, false, "script");
            return true;
        }
        if (Command == "shot")
        {
            Dump(Rest, false, true, "script");
            return true;
        }
        if (Command == "command")
            return DoCommand(Rest, Note);
        if (Command == "key")
            return DoKey(Rest, Note);
        if (Command == "click")
            return DoClick(Rest, Note);
        if (Command == "state")
        {
            unsigned Id = 0;
            if (!ParseUnsigned(Rest, &Id))
            {
                *Note = "bad state id";
                return false;
            }
            G_PendingClientStateId = static_cast<int>(Id);
            return true;
        }
        if (Command == "log")
            return true;
        if (Command == "quit")
        {
            Quit("script");
            return true;
        }
        *Note = "unknown command";
        return false;
    }

    void RunScript()
    {
        while (!G.bDone)
        {
            if (G.Frame < G.WaitUntil)
                return;
            if (G.bWaitScreen)
            {
                unsigned const* const Last = LastScreenOf(G.WaitKind);
                bool const bMatch = Last != nullptr && *Last == G.WaitId;
                if (!bMatch && G.Frame < G.WaitDeadline)
                    return;
                G.bWaitScreen = false;
                FOracleJson J;
                Head(J, "waitscreen");
                J.Uint("line", G.WaitLine);
                J.Str("result", bMatch ? "matched" : "timeout");
                Session(J);
            }
            if (G.NextLine >= G.Script.size())
            {
                Quit("end of script");
                return;
            }

            std::string const Line = G.Script[G.NextLine];
            G.NextLine += 1;
            std::string const Text = Trim(Line);
            if (Text.empty() || Text[0] == '#')
                continue;
            std::string Rest;
            std::string const Command = FirstWord(Text, &Rest);
            std::string Note;
            bool const bOk = Execute(Command, Rest, &Note);

            FOracleJson J;
            Head(J, "script");
            J.Uint("line", static_cast<unsigned>(G.NextLine));
            J.Str("text", Text.c_str());
            J.Bool("ok", bOk);
            if (!Note.empty())
                J.Str("note", Note.c_str());
            Session(J);
        }
    }

    // ---- start --------------------------------------------------------------------------

    void Start(char const* ScriptPath, char const* OutDir)
    {
        FILE* const ScriptFile = fopen(ScriptPath, "rt");
        if (ScriptFile == nullptr)
            Fail("cannot read the script", ScriptPath);
        char Buffer[1024];
        while (fgets(Buffer, sizeof(Buffer), ScriptFile) != nullptr)
        {
            size_t Length = strlen(Buffer);
            while (Length > 0 && (Buffer[Length - 1] == '\n' || Buffer[Length - 1] == '\r'))
                Buffer[--Length] = '\0';
            G.Script.push_back(Buffer);
        }
        fclose(ScriptFile);

        if (!CreateDirectoryA(OutDir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            Fail("cannot create the output folder", OutDir);
        G.OutDir = OutDir;
        while (!G.OutDir.empty() && (G.OutDir.back() == '\\' || G.OutDir.back() == '/'))
            G.OutDir.pop_back();

        InitializeCriticalSection(&G.Lock);
        for (int Stream = 0; Stream < kOracleStreamCount; ++Stream)
        {
            std::string const Path = G.OutDir + "\\" + kStreamFiles[Stream];
            G.Files[Stream] = fopen(Path.c_str(), "wb");
            if (G.Files[Stream] == nullptr)
                Fail("cannot write", Path.c_str());
        }

        G.LastClient = ~0u;
        G.LastMenu = ~0u;
        G.LastRoom = ~0u;
        G.bActive = true;

        FOracleJson J;
        Head(J, "start");
        J.Uint("version", 1);
        J.Str("script", ScriptPath);
        J.Str("out", OutDir);
        J.Uint("lines", static_cast<unsigned>(G.Script.size()));
        J.Double("frame_seconds", kOracleFrameSeconds);
        J.Uint("seed", kOracleRandSeed);
        J.Uint("settle_frames", kOracleSettleFrames);
        Session(J);
    }

    // ---- parsed-file dumps --------------------------------------------------------------

    void Layout(FOracleJson& J)
    {
        OracleWriteRecord(kOracleLayout, J.Finish());
    }

    void PayloadFields(FOracleJson& J, FUiWidgetEntry const* Entry)
    {
        char const* const Type = Entry->TypeName != nullptr ? Entry->TypeName : "";
        void const* const Payload = Entry->Payload;
        if (Payload == nullptr)
        {
            J.Null("payload");
            return;
        }
        J.BeginObject("payload");
        if (strcmp(Type, "UIStaticText") == 0 || strcmp(Type, "UITextEdit") == 0)
        {
            FUiPayloadText const* const P = static_cast<FUiPayloadText const*>(Payload);
            J.Float("right", P->Right);
            J.Float("bottom", P->Bottom);
            J.WStr("caption", P->CaptionUtf16);
        }
        else if (strcmp(Type, "UITextMsgButton") == 0)
        {
            FUiPayloadTextMsgButton const* const P =
                static_cast<FUiPayloadTextMsgButton const*>(Payload);
            J.Float("right", P->Right);
            J.Float("bottom", P->Bottom);
            J.WStr("caption", P->CaptionUtf16);
            J.Int("extra_int", P->ExtraInt);
            J.Int("extra_id", P->ExtraId);
            J.Str("style", P->StyleName);
        }
        else if (strcmp(Type, "UIWinObject") == 0)
        {
            FUiPayloadWinObject const* const P = static_cast<FUiPayloadWinObject const*>(Payload);
            J.Float("right", P->Right);
            J.Float("bottom", P->Bottom);
            J.Str("extra_name", P->ExtraName);
            J.Float("extra_float", P->ExtraFloat);
        }
        else if (strcmp(Type, "UITable") == 0 || strcmp(Type, "UIFastTable") == 0)
        {
            FUiPayloadTable const* const P = static_cast<FUiPayloadTable const*>(Payload);
            J.Float("right", P->Right);
            J.Float("bottom", P->Bottom);
            J.Int("int5", P->Int5);
            J.Int("int6", P->Int6);
        }
        else if (strcmp(Type, "UIScroll") == 0 || strcmp(Type, "UICycle") == 0)
        {
            FUiPayloadScrollCycle const* const P =
                static_cast<FUiPayloadScrollCycle const*>(Payload);
            J.Float("right", P->Right);
            J.Float("bottom", P->Bottom);
            float const Extra[4] = {P->Extra0, P->Extra1, P->Extra2, P->Extra3};
            J.FloatArray("extra", Extra, 4);
            J.Uint("flag", P->Flag);
        }
        else
        {
            // UIListBox, UIWinDeform, UIRichTextWin, UIShipViewControl.
            FUiPayloadBounds const* const P = static_cast<FUiPayloadBounds const*>(Payload);
            J.Float("right", P->Right);
            J.Float("bottom", P->Bottom);
        }
        J.EndObject();
    }

    void KeyRowKey(FOracleJson& J, char const* Name, FKeyRowKey const& Key)
    {
        J.BeginArray(Name);
        J.Str(nullptr, KeyMapGetActionNickname(Key.NicknameId));
        J.Int(nullptr, Key.VirtualKey);
        J.Uint(nullptr, Key.Modifiers);
        J.EndArray();
    }

    void OptRows(char const* Path, char const* Section, std::vector<FPrefOption> const& Rows)
    {
        for (size_t Index = 0; Index < Rows.size(); ++Index)
        {
            FPrefOption const& Row = Rows[Index];
            FOracleJson J;
            Head(J, "opt");
            J.Str("path", Path);
            J.Str("section", Section);
            J.Uint("i", static_cast<unsigned>(Index));
            J.Int("id", Row.Id);
            J.Int("info_ids", Row.InfoIds);
            J.Int("default", Row.Default);
            J.Int("min", Row.Min);
            J.Int("max", Row.Max);
            J.Uint("slider", Row.bSlider);
            J.Uint("byte15", Row.Byte15);
            J.Uint("header", Row.bHeader);
            Layout(J);
        }
    }
}

// ---- hooks ----------------------------------------------------------------------------

bool OracleModeActive()
{
    return G.bActive;
}

void OracleParseCommandLine(FClientCommandLine const* CommandLine)
{
    for (unsigned Index = 0; Index < CommandLine->Count; ++Index)
    {
        if (_stricmp(CommandLine->Argv[Index], "+oracle") != 0)
            continue;
        if (Index + 2 >= CommandLine->Count)
            Fail("usage: +oracle <script file> <output folder>", nullptr);
        Start(CommandLine->Argv[Index + 1], CommandLine->Argv[Index + 2]);
        return;
    }
}

double OracleFrameSeconds()
{
    return kOracleFrameSeconds;
}

void OracleForceDisplayMode(DisplaySettings* Mode)
{
    if (!G.bActive)
        return;
    Mode->fullscreen = 0;
    Mode->width = kOracleWidth;
    Mode->height = kOracleHeight;
    Mode->color_bpp = kOracleColorBits;
    FOracleJson J;
    Head(J, "display");
    J.Int("width", Mode->width);
    J.Int("height", Mode->height);
    J.Int("color_bits", Mode->color_bpp);
    J.Int("fullscreen", Mode->fullscreen);
    Session(J);
}

void OracleSeedRandom()
{
    if (!G.bActive)
        return;
    srand(kOracleRandSeed);
    // The in-process server on the oracle's clock (Phase 5, design/adr/0009-game-client.md
    // question 3): the scenario oracle's hooks in Common.dll (include/oracle_sim_hooks.h),
    // which only Oracle.exe sim set until now. Under them IServerImpl::ElapseTime adds the
    // fixed frame to the server time instead of the real clock's delta, IServerImpl::Startup
    // seeds rand() from StartupSeed instead of timeGetTime() (twice, as for the harness), and
    // the server's damage times read ClockSeconds (server_gameobject.cpp). The hooks' callbacks
    // stay null. OracleFrameTick advances ClockSeconds.
    GOracleSimHooks.bFixedClock = true;
    GOracleSimHooks.StartupSeed = kOracleRandSeed;
    GOracleSimHooks.FrameSeconds = kOracleFrameSeconds;
    GOracleSimHooks.ClockSeconds = kOracleServerClockStart;
    FOracleJson J;
    Head(J, "seed");
    J.Uint("seed", kOracleRandSeed);
    J.Uint("server_seed", GOracleSimHooks.StartupSeed);
    J.Double("server_clock", GOracleSimHooks.ClockSeconds);
    Session(J);
}

void OracleFrameTick()
{
    if (!G.bActive)
        return;
    G.Frame += 1;
    GOracleSimHooks.ClockSeconds = kOracleServerClockStart + G.Frame * kOracleFrameSeconds;
    if (G.AutoDumpFrame != 0 && G.Frame >= G.AutoDumpFrame)
    {
        G.AutoDumpFrame = 0;
        std::string const Label = G.AutoLabel;
        Dump(Label, true, true, "screen");
    }
    RunScript();
}

void OracleNoteScreen(char const* Kind, unsigned Id)
{
    if (!G.bActive)
        return;
    unsigned* const Last = LastScreenOf(Kind);
    if (Last != nullptr)
        *Last = Id;

    FOracleJson J;
    Head(J, "screen");
    J.Str("kind", Kind);
    J.Uint("id", Id);
    Session(J);

    char Label[48];
    sprintf(Label, "%s-%u", Kind, Id);
    G.AutoLabel = Label;
    G.AutoDumpFrame = G.Frame + kOracleSettleFrames;
}

void OracleOnKeyMapLoaded(char const* Phase, int Result)
{
    if (!G.bActive)
        return;
    std::list<FKeyCmd> const& Commands = KeyMapOracleCommands();
    std::list<FKeyMapGroup> const& Groups = KeyMapOracleGroups();
    std::list<FKeyMapGroup*> const& Active = KeyMapOracleActiveGroups();

    FOracleJson HeadJ;
    Head(HeadJ, "load");
    HeadJ.Str("phase", Phase);
    HeadJ.Int("result", Result);
    HeadJ.Uint("cmds", static_cast<unsigned>(Commands.size()));
    HeadJ.Uint("groups", static_cast<unsigned>(Groups.size()));
    OracleWriteRecord(kOracleKeyMap, HeadJ.Finish());

    std::vector<FKeyCmd const*> Order;
    unsigned Index = 0;
    for (std::list<FKeyCmd>::const_iterator It = Commands.begin(); It != Commands.end();
         ++It, ++Index)
    {
        Order.push_back(&*It);
        FOracleJson J;
        Head(J, "cmd");
        J.Str("phase", Phase);
        J.Uint("i", Index);
        J.Str("nickname", KeyMapGetActionNickname(It->NicknameId));
        J.Uint("id", It->NicknameId);
        J.Uint("ids_name", It->IdsName);
        J.Uint("ids_info", It->IdsInfo);
        J.Uint("state", It->StateMask);
        J.BeginArray("keys");
        for (size_t Key = 0; Key < It->Bindings.size(); ++Key)
        {
            J.BeginArray(nullptr);
            J.Int(nullptr, It->Bindings[Key].VirtualKey);
            J.Uint(nullptr, It->Bindings[Key].Modifiers);
            J.EndArray();
        }
        J.EndArray();
        OracleWriteRecord(kOracleKeyMap, J.Finish());
    }

    Index = 0;
    for (std::list<FKeyMapGroup>::const_iterator It = Groups.begin(); It != Groups.end();
         ++It, ++Index)
    {
        FOracleJson J;
        Head(J, "group");
        J.Str("phase", Phase);
        J.Uint("i", Index);
        J.Str("nickname", It->Nickname.c_str());
        J.BeginArray("cmds");
        for (std::list<FKeyCmd*>::const_iterator Cmd = It->Commands.begin();
             Cmd != It->Commands.end(); ++Cmd)
        {
            int Found = -1;
            for (size_t Search = 0; Search < Order.size(); ++Search)
            {
                if (Order[Search] == *Cmd)
                {
                    Found = static_cast<int>(Search);
                    break;
                }
            }
            J.Int(nullptr, Found);
        }
        J.EndArray();
        OracleWriteRecord(kOracleKeyMap, J.Finish());
    }

    FOracleJson J;
    Head(J, "active");
    J.Str("phase", Phase);
    J.BeginArray("groups");
    for (std::list<FKeyMapGroup*>::const_iterator It = Active.begin(); It != Active.end(); ++It)
        J.Str(nullptr, (*It)->Nickname.c_str());
    J.EndArray();
    OracleWriteRecord(kOracleKeyMap, J.Finish());
}

void OracleOnPageDescLoaded(char const* Path, bool bOk, FUiPageData const* Data)
{
    if (!G.bActive)
        return;
    unsigned Count = 0;
    if (Data != nullptr && Data->Entries != nullptr)
    {
        while (Data->Entries[Count] != nullptr)
            ++Count;
    }

    FOracleJson HeadJ;
    Head(HeadJ, "page");
    HeadJ.Str("path", Path);
    HeadJ.Bool("ok", bOk);
    HeadJ.Float("aspect", Data != nullptr ? Data->Aspect : 0.0f);
    HeadJ.Uint("entries", Count);
    Layout(HeadJ);

    for (unsigned Index = 0; Index < Count; ++Index)
    {
        FUiWidgetEntry const* const Entry = Data->Entries[Index];
        FOracleJson J;
        Head(J, "widget");
        J.Str("path", Path);
        J.Uint("i", Index);
        J.Str("type", Entry->TypeName);
        J.Str("name", Entry->InstanceName);
        float const Rect[4] = {Entry->Left, Entry->Top, Entry->Right, Entry->Bottom};
        J.FloatArray("rect", Rect, 4);
        PayloadFields(J, Entry);
        Layout(J);
    }
}

void OracleOnWinControlIni(char const* Path, Win3D* Control)
{
    if (!G.bActive || Control == nullptr)
        return;
    std::vector<FNode> Nodes;
    FlattenFrom(Control, -1, 0, true, Nodes);

    FOracleJson HeadJ;
    Head(HeadJ, "winini");
    HeadJ.Str("path", Path);
    HeadJ.Uint("nodes", static_cast<unsigned>(Nodes.size()));
    Layout(HeadJ);

    for (size_t Index = 0; Index < Nodes.size(); ++Index)
    {
        FOracleJson J;
        Head(J, "winnode");
        J.Str("path", Path);
        NodeFields(J, Nodes[Index], static_cast<int>(Index));
        Layout(J);
    }
}

void OracleOnKeyListLoaded(char const* Path, NnPreferences const* Preferences)
{
    if (!G.bActive || Preferences == nullptr)
        return;
    FOracleJson HeadJ;
    Head(HeadJ, "keylist");
    HeadJ.Str("path", Path);
    HeadJ.BeginArray("group_names");
    for (int Group = 0; Group < 3; ++Group)
        HeadJ.Int(nullptr, Preferences->GroupNameIds[Group]);
    HeadJ.EndArray();
    Layout(HeadJ);

    for (int Group = 0; Group < 3; ++Group)
    {
        std::vector<FKeyRow> const& Rows = Preferences->KeyGroups[Group];
        for (size_t Index = 0; Index < Rows.size(); ++Index)
        {
            FKeyRow const& Row = Rows[Index];
            FOracleJson J;
            Head(J, "keyrow");
            J.Str("path", Path);
            J.Int("group", Group);
            J.Uint("i", static_cast<unsigned>(Index));
            KeyRowKey(J, "primary", Row.Primary);
            KeyRowKey(J, "alt", Row.Alt);
            J.Int("name_ids", Row.NameIds);
            J.Int("info_ids", Row.InfoIds);
            J.Uint("cmd", Row.CommandId);
            J.Int("kind", Row.BindingKind);
            J.Uint("editable", Row.bEditable);
            Layout(J);
        }
    }
}

void OracleOnOptListLoaded(char const* Path, bool bOpened, NnPreferences const* Preferences)
{
    if (!G.bActive || Preferences == nullptr)
        return;
    FOracleJson HeadJ;
    Head(HeadJ, "optlist");
    HeadJ.Str("path", Path);
    HeadJ.Bool("opened", bOpened);
    HeadJ.Uint("seed_mode", Preferences->bSeedMode);
    Layout(HeadJ);
    OptRows(Path, "Interface", Preferences->OptionsA);
    OptRows(Path, "Audio", Preferences->OptionsB);
    OptRows(Path, "Video", Preferences->OptionsC);
}

void OracleOnHudElementsLoaded(char const* Path, HUDItem* Head_, int StaticBlend)
{
    if (!G.bActive)
        return;
    unsigned Count = 0;
    for (HUDItem* Item = Head_; Item != nullptr; Item = Item->Next)
        ++Count;

    FOracleJson HeadJ;
    Head(HeadJ, "hud");
    HeadJ.Str("path", Path);
    HeadJ.Int("static_blend", StaticBlend);
    HeadJ.Uint("items", Count);
    Layout(HeadJ);

    unsigned Index = 0;
    for (HUDItem* Item = Head_; Item != nullptr; Item = Item->Next, ++Index)
    {
        FOracleJson J;
        Head(J, "huditem");
        J.Uint("i", Index);
        J.StrN("name", Item->Name.Data, sizeof(Item->Name.Data));
        J.Str("rtti", typeid(*Item).name());
        J.Int("tooltip", Item->ToolTipIds);
        Layout(J);
    }
}

IClient* OracleWrapClientInterface(IClient* Real)
{
    if (!G.bActive || Real == nullptr)
        return Real;
    return OracleClientProxyFor(Real);
}

IChatClient* OracleWrapChatClientInterface(IChatClient* Real)
{
    if (!G.bActive || Real == nullptr)
        return Real;
    return OracleChatClientProxyFor(Real);
}

void OracleWriteRecord(EOracleStream Stream, std::string const& Line)
{
    if (!G.bActive || Stream < 0 || Stream >= kOracleStreamCount)
        return;
    EnterCriticalSection(&G.Lock);
    FILE* const File = G.Files[Stream];
    if (File != nullptr)
    {
        fwrite(Line.data(), 1, Line.size(), File);
        fputc('\n', File);
        fflush(File);
    }
    LeaveCriticalSection(&G.Lock);
}

unsigned OracleFrameNumber()
{
    return G.Frame;
}

unsigned OracleNextIClientSeq()
{
    return static_cast<unsigned>(InterlockedIncrement(&G.IClientSeq));
}

// ---- rooms (P5.9) ------------------------------------------------------------------------

namespace
{
    // A Csys as twelve floats: the position, then the 3x3 row by row.
    void CsysFields(FOracleJson& J, char const* Name, Csys const& Frame)
    {
        float Words[12] = {Frame.Pos.x, Frame.Pos.y, Frame.Pos.z};
        for (unsigned Cell = 0; Cell < 9; ++Cell)
            Words[3 + Cell] = Frame.Orient.E[Cell / 3][Cell % 3];
        J.FloatArray(Name, Words, 12);
    }

    void VectorField(FOracleJson& J, char const* Name, Vector const& Value)
    {
        float const Words[3] = {Value.x, Value.y, Value.z};
        J.FloatArray(Name, Words, 3);
    }

    // The head of one record of the room's file: the frame, the kind, the room and the index.
    void RoomHead(FOracleJson& J, char const* Rec, RoomData const* Room, size_t Index)
    {
        Head(J, Rec);
        J.Uint("room", Room->RoomId);
        J.Uint("i", static_cast<unsigned>(Index));
    }
}

void OracleOnRoomLoaded(RoomData const* Room)
{
    if (!G.bActive || Room == nullptr)
        return;

    FOracleJson HeadJ;
    Head(HeadJ, "room");
    HeadJ.Uint("room", Room->RoomId);
    HeadJ.Uint("base", Room->BaseId);
    HeadJ.Str("nickname", Room->Nickname);
    HeadJ.Str("file", Room->File);
    HeadJ.BeginArray("animations");
    for (int Index = 0; Index < Room->AnimationCount && Index < 0x20; ++Index)
    {
        HeadJ.BeginObject(nullptr);
        HeadJ.Str("name", Room->Animations[Index]);
        HeadJ.Bool("loop", Room->AnimationLoop[Index]);
        HeadJ.EndObject();
    }
    HeadJ.EndArray();
    HeadJ.Str("goodscart", Room->GoodscartScript);
    HeadJ.Uint("goodscart_priority", Room->GoodscartPriorityId);
    HeadJ.Bool("cutoff_on", Room->bCutoffShadowZ);
    HeadJ.Float("cutoff", Room->CutoffShadowZ);
    HeadJ.Float("extra_shadow", Room->ExtraShadowHeightOffset);
    HeadJ.Uint("music", Room->Music.Id);
    HeadJ.Bool("music_loop", Room->bMusicLoop);
    HeadJ.Uint("ambient", Room->Ambient.Id);
    HeadJ.Bool("ambient_loop", Room->bAmbientLoop);
    VectorField(HeadJ, "ambient_color", Room->AmbientColor);
    HeadJ.BeginObject("shadow");
    HeadJ.Bool("on", Room->bShadowLight);
    HeadJ.Bool("dir_on", Room->bShadowLightWorldSpaceDir);
    VectorField(HeadJ, "pos", Room->ShadowLightPosition);
    VectorField(HeadJ, "dir", Room->ShadowLightDirection);
    VectorField(HeadJ, "color", Room->ShadowLightColor);
    HeadJ.EndObject();
    HeadJ.BeginObject("glare");
    HeadJ.Bool("on", Room->bGlareLight);
    HeadJ.Bool("dir_on", Room->bGlareLightWorldSpaceDir);
    VectorField(HeadJ, "pos", Room->GlareLightPosition);
    VectorField(HeadJ, "dir", Room->GlareLightDirection);
    VectorField(HeadJ, "color", Room->GlareLightColor);
    HeadJ.EndObject();
    HeadJ.Str("camera", Room->CameraName);
    HeadJ.Uint("cameras", static_cast<unsigned>(Room->Cameras.size()));
    HeadJ.Uint("lights", static_cast<unsigned>(Room->Lights.size()));
    HeadJ.Uint("props", static_cast<unsigned>(Room->Props.size()));
    HeadJ.Uint("setpoints", static_cast<unsigned>(Room->Setpoints.size()));
    HeadJ.Uint("chars", static_cast<unsigned>(Room->CharacterPlacements.size()));
    HeadJ.Uint("ships", static_cast<unsigned>(Room->ShipPlacements.size()));
    HeadJ.Uint("hotspots", static_cast<unsigned>(Room->Hotspots.size()));
    HeadJ.Uint("flashlights", static_cast<unsigned>(Room->FlashLightSets.size()));
    HeadJ.Uint("scenes", static_cast<unsigned>(Room->AmbientScripts.size()));
    Layout(HeadJ);

    for (size_t Index = 0; Index < Room->Cameras.size(); ++Index)
    {
        RoomCameraInfo const* const Camera = Room->Cameras[Index];
        FOracleJson J;
        RoomHead(J, "roomcam", Room, Index);
        J.Str("name", Camera->Name);
        CsysFields(J, "csys", Camera->Frame);
        J.Float("fov", Camera->Fov);
        J.Float("near", Camera->NearPlane);
        J.Float("far", Camera->FarPlane);
        Layout(J);
    }
    for (size_t Index = 0; Index < Room->Lights.size(); ++Index)
    {
        RoomLightInfo const* const Light = Room->Lights[Index];
        FOracleJson J;
        RoomHead(J, "roomlight", Room, Index);
        CsysFields(J, "csys", Light->Frame);
        J.FloatArray("dir", Light->Direction, 3);
        // The D3DLIGHT8 words from +0x3c (the type, the colour and the opaque tail).
        J.Bytes("d3d", &Light->Mode, 0x1a * sizeof(unsigned));
        J.Uint("type", Light->Type);
        Layout(J);
    }
    for (size_t Index = 0; Index < Room->Props.size(); ++Index)
    {
        RoomPropInfo const* const Prop = Room->Props[Index];
        FOracleJson J;
        RoomHead(J, "roomprop", Room, Index);
        J.Str("template", Prop->Template);
        J.Str("name", Prop->Name);
        J.Int("category", Prop->Category);
        CsysFields(J, "csys", Prop->Frame);
        J.Bool("visible", Prop->bVisible);
        J.Bool("lit", Prop->bLit);
        J.Bool("fog", Prop->bFog);
        J.Bool("main", Prop->bRoomMain);
        J.Uint("light_group", Prop->LightGroup);
        J.Uint("layer", Prop->Layer);
        J.Uint("render", Prop->RenderType);
        J.Str("loadout", Prop->Loadout);
        J.Uint("priority", Prop->PriorityId);
        J.Bool("running_lights", Prop->bRunningLights);
        Layout(J);
    }
    for (size_t Index = 0; Index < Room->Setpoints.size(); ++Index)
    {
        SetpointInfo const* const Setpoint = Room->Setpoints[Index];
        FOracleJson J;
        RoomHead(J, "roomsetpoint", Room, Index);
        J.Str("name", Setpoint->Name);
        CsysFields(J, "csys", Setpoint->Frame);
        SetpointProperties const& Props = Setpoint->Props;
        J.Bool("valid", Props.bValid != 0);
        J.Bool("zs", Props.bZoneScript != 0);
        J.Int("char_type", Props.CharType);
        J.Str("segment", Props.Segment);
        J.Int("zone", Props.Zone);
        J.StrN("pname", Props.NameBuffer, sizeof(Props.NameBuffer));
        J.Int("posture", Props.Posture);
        Layout(J);
    }
    for (size_t Index = 0; Index < Room->CharacterPlacements.size(); ++Index)
    {
        CharPlaceInfo const* const Place = Room->CharacterPlacements[Index];
        FOracleJson J;
        RoomHead(J, "roomchar", Room, Index);
        J.Str("name", Place->Name);
        CsysFields(J, "csys", Place->Frame);
        J.Int("char_type", Place->CharType);
        J.Int("posture", Place->Posture);
        J.Bool("zs", Place->bZoneScript != 0);
        J.Str("start_script", Place->StartScript);
        Layout(J);
    }
    for (size_t Index = 0; Index < Room->ShipPlacements.size(); ++Index)
    {
        ShipPlaceInfo const* const Place = Room->ShipPlacements[Index];
        FOracleJson J;
        RoomHead(J, "roomship", Room, Index);
        J.Str("name", Place->Nickname);
        CsysFields(J, "csys", Place->Frame);
        J.Bool("player", Place->bIsPlayerShip);
        J.Bool("for_sale", Place->bIsForSale);
        J.Str("landing", Place->LandingScript);
        J.Str("launching", Place->LaunchingScript);
        Layout(J);
    }
    for (size_t Index = 0; Index < Room->Hotspots.size(); ++Index)
    {
        RoomButtonInfo const* const Button = Room->Hotspots[Index];
        FOracleJson J;
        RoomHead(J, "roomhotspot", Room, Index);
        J.Str("name", Button->Name);
        J.Str("room_switch", Button->RoomSwitch);
        J.Str("virtual_room", Button->VirtualRoom);
        J.Str("set_virtual_room", Button->SetVirtualRoom);
        J.Int("behavior", Button->Behavior);
        J.Uint("state_send", Button->StateSend);
        J.Uint("state_read", Button->StateRead);
        J.BeginArray("transitions");
        for (int Slot = 0; Slot < 4; ++Slot)
            J.Str(nullptr, Button->StateTransition[Slot]);
        J.EndArray();
        Layout(J);
    }
    for (size_t Index = 0; Index < Room->FlashLightSets.size(); ++Index)
    {
        FlashLightSetInfo const* const Set = Room->FlashLightSets[Index];
        FOracleJson J;
        RoomHead(J, "roomflash", Room, Index);
        float const Params[7] = {Set->Param0, Set->Param1, Set->Param2, Set->Param3,
                                 Set->Param4, Set->Param5, Set->Param6};
        J.FloatArray("params", Params, 7);
        J.Int("mode", static_cast<int>(Set->Mode));
        J.Int("numlights", static_cast<int>(Set->NumLights));
        J.Int("numlights_alt", static_cast<int>(Set->NumLightsAlt));
        long const Count = Set->NumLights < 0 ? 0 : (Set->NumLights > 64 ? 64 : Set->NumLights);
        J.BeginArray("hardpoints");
        for (long Slot = 0; Slot < Count; ++Slot)
            J.Str(nullptr, Set->HardpointNames[Slot]);
        J.EndArray();
        J.BeginArray("setpoints");
        for (long Slot = 0; Slot < Count; ++Slot)
            J.Str(nullptr, Set->SetpointNames[Slot]);
        J.EndArray();
        Layout(J);
    }
    for (size_t Index = 0; Index < Room->AmbientScripts.size(); ++Index)
    {
        AmbientScriptInfo const* const Script = Room->AmbientScripts[Index];
        FOracleJson J;
        RoomHead(J, "roomscene", Room, Index);
        J.Int("type", Script->Type);
        J.Str("path", Script->Path);
        J.Uint("priority", Script->Priority);
        Layout(J);
    }
}
