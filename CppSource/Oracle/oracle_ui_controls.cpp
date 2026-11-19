// P4.13: the client UI toolkit's generic controls, through the reference's own code.
// The Win3D manager (win3d.cpp), WinControl (wincontrol_base.cpp), the factory registry
// (win_factory.cpp), the hover pulse (nn_ui_pulse.cpp) and the controls ListBox / UIListBox
// (list_box.cpp, ui_list_box.cpp), UIScroll / UICycle (ui_scroll.cpp), WinObject /
// UIWinObject (win_object.cpp, ui_win_object.cpp), TextControl / EditControl
// (nn_text_control.cpp), StaticControl (nn_static_control.cpp), ButtonControl /
// ToggleButtonControl (nn_button_control.cpp), the wire controls (wire_controls.cpp),
// ScrollControl (scroll_control.cpp) and UITable (ui_table.cpp) are compiled into this file
// (their file-local helpers of the same name renamed around the includes). The controls draw
// through recording stand-ins for the immediate-mode layer (win3d_immediate.cpp), the render
// pipeline, the font strings and the mesh holder; the client functions they reach otherwise
// (the screen projection of text objects, the frame time, the software cursor, the game options,
// the texture shapes, Common's math engine) are stand-ins with the values the case sets. Every
// other client symbol the compiled files name has an empty stand-in. The Rust side is
// crates/neuron-client/tests/oracle_vectors.rs with crates/neuron-client/tests/support/ui.rs;
// the inputs come from crates/neuron-client/tests/fixtures/oracle/make_ui_fixtures.py.
//
// ui_controls.list holds one case per line: operations separated by " ; ", their fields by
// single spaces, floats as f32 bits in hex, integers in decimal, texts and paths in hex. Each
// case starts with a recording parent (a WinControl) and an empty manager:
//   new UIListBox R B | new UIScroll R B E0 E1 E2 E3 F | new UICycle (as UIScroll) |
//   new UIWinObject R B Z PATH                WinFactoryCreate("C", class, parent, payload)
//   xform M0..M8 T0 T1 T2      SetTransform        pos X Y Z         SetPosition
//   ray X Y Z                  the mouse ray       cursor X Y        the software cursor
//   dt D                       the frame time      pulse D           NnUiPulseAdvance
//   opt V                      GetGameOption(0x16) update | draw | over | leave
//   btn F0 F1                  MouseEvent          hit               HitTest(the mouse ray)
//   state                      the object's state
//   ListBox: item TEXT DATA (AddItem), settext I TEXT, data I, f80 I, frame V, rules V,
//            rows N, first N, sel V
//   UIScroll: range A B, value V, page P, step S, size W H (the setters), stepmode V, horiz V,
//             custom V, active V, soff X Y Z (SetScrollOffset), offset X Y Z (property 6),
//             prop M A B (SetProperty with integers)
//   UIWinObject: flags V, tint RRGGBBAA
// The later controls are made by the factory with no payload (new CLASS), UITable with its
// payload (new UITable R B ROWS COLS), then set up through their Init:
//   TextControl / EditControl: init STYLE X Y Z HP W H IDS JUST TIP ANCHOR
//   StaticControl: init MESH OX OY OZ HP HX HY HZ NOZ ZCLEAR NOMESH NOWIRE NOCOLOR RTW NOCLEAR
//                  RADIUS SPEED ROT YAW PITCH ROLL
//   ButtonControl / ToggleButtonControl: init MESH MESH2 SOUND KEY OX OY OZ HP HX HY HZ NOMESH
//                  ACTION NOCOLOR NOZ TIP TIP2 CMD (the toggle has no command)
//   WireStaticControl: init OX OY OZ HX HY HZ WHITE NOZ TIP MASK
//   WireButtonControl: init SOUND OX OY OZ HX HY HZ WHITE NOZ TIP CMD MASK
//   ScrollControl: init, per part back, next, prev: MESH OX OY OZ HP MX MY MZ NOMESH; then the
//                  thumb: MESH O0X O0Y O0Z O1X O1Y O1Z HP0 HP1 MX MY MZ NOMESH; then MIN MAX PAGE
//                  SLIDER NOZ
// (texts and paths in hex, "-" for null), and take the operations
//   enter (MouseEnter) | inactive (UpdateInactive) | key K M (KeyEvent) | time T (the elapsed
//   time) | gopt I V (GetGameOption(I)) | prop M A B (SetProperty with integers) | offset X Y Z
//   (property 6) | text HEX (0x1d) | colour RRGGBBAA B (0x33) | colours RRGGBBAA RRGGBBAA (0x18)
//   | getcmd (0x2e) | gettoggle (0x2f) | sounds S0 S1 (0x31) | tset HEX (SetText) | ids N
//   (SetIds) | width W (SetWidth) | cached C I (SetCachedDraw) | number HEX (SetNumberText) |
//   cedge I X / redge I Y (SetColumnEdge / SetRowEdge) | rows H S... (the rows' flags)
// The rich-text windows: RichTextEditWin (new RichTextEditWin), UITextEdit (new UITextEdit R B
// TEXT), UIFastTable and UIScrollTable (new UIFastTable R B ROWS COLS), with the operations
//   edit boxes: ekey K M D (event 1 with the record's key, modifiers and down byte) | echar W
//   (event 2 with the character) | ecancel MSG (event 0xf with the raw message) | settext TEXT |
//   settexta TEXT ATTRS | cleartext | gettext N | caret P | maxlen N | fixed V | password V |
//   case M | allow V | filter TEXT | repl U | editable V | activate | deactivate H | attrs X |
//   fmask X | prefix TEXT (one text node) | fixedlist ATTRS (one attribute node), each list
//   change followed by RebuildDisplay
//   tables: cedge I X | redge I Y | frows H S L G... (the rows' flags and groups) | selrow R |
//   clearsel | getsel | hdr N | borders L R T B | collines V | highlights V | resizable R C |
//   grab X | margins X Y | tipmode M | wtip IDS | coltip C IDS | rtip R IDS | ctip R C IDS |
//   tooltip V | clearcell R C | span R C V | invalidate R C | setedges NR R.. NC C..
//   scroll tables: items N F | first | callback | barvalue V | scrolled (slot 23 0xf005 from the
//   bar) | gap X | bstate (the bar's state)
//
// Line formats, each prefixed with "case": the stand-ins' calls --
//   {"ev":"begin","p"} {"ev":"end"} {"ev":"v","x","y","z"} {"ev":"c3","c"} {"ev":"c4","c"}
//   {"ev":"tc","u","v"} {"ev":"dis","cap"} {"ev":"tex","h"} {"ev":"push"} {"ev":"pop"}
//   {"ev":"ortho"} {"ev":"mv","m"} {"ev":"rs","s","v"} {"ev":"color","c"}
//   {"ev":"paint","t","x","y","l","r"} {"ev":"load","path","mat"} {"ev":"mpos","x","y","z"}
//   {"ev":"mupd"} {"ev":"submit"} {"ev":"tinted","tint","l"}
//   {"ev":"en","cap"} {"ev":"mori","m"} {"ev":"mxf","m"} (mesh events of a second or later
//   instance carry "i") {"ev":"body","t","cull","s"} after a submit with a tint, front culling or
//   a scale; {"ev":"tscale","cull","s"} after a tinted submit without front culling or with a
//   scale; {"ev":"outlined","w","s"} {"ev":"line","f","t","c","w"} (the line batcher)
//   {"ev":"fscale","w","z"} {"ev":"ftext","t"} {"ev":"fnum","f"} {"ev":"fdraw","x","y","w","h",
//   "hm","vm","l","r"} (the Font::String) {"ev":"hover","k","a","x","p","e","l"} {"ev":"hidetip"}
//   {"ev":"sound","s"} {"ev":"tplay","s"} {"ev":"tstop","s"} {"ev":"roll"} {"ev":"post","c"}
//   {"ev":"lclear"} (EngineLightClearRegisteredList) {"ev":"winview","vp","near","far","fov"}
//   (a static control's window camera made current) {"ev":"winview","vp":"-"} (its viewport back)
// -- the parent's (slot 23 and SetProperty from the control; "self" for the control, a list
// row's text in hex or "null") --
//   {"ev":"msg","m","a","b"} {"ev":"prop","m","a","b"}
// -- and the operations' results and states --
//   {"ev":"new","ok"} {"ev":"btn","r"} {"ev":"hit","r"} {"ev":"r","v"}
//   {"ev":"list","hover","f80","sel","count","first","rows","t","w","h"}
//   {"ev":"scroll","flags","pos","v","rect","arrow","drag","rt","dragging","doff","inv","colors",
//    "soff","dpage"}
//   {"ev":"obj","flags","pos","orient","origin","corner"}
//   {"ev":"text",...} {"ev":"static",...} {"ev":"button",...} {"ev":"wire",...}
//   {"ev":"sctl",...} {"ev":"table",...} (the fields in the order the state functions print;
//   their flags are the low four bits, the Win3D constructor keeps the others as allocated)
//   {"ev":"edit",...} {"ev":"ftable",...} {"ev":"stable",...} {"ev":"text","r","t"}, and the HUD's
//   {"ev":"focus","op","h"} {"ev":"evreg","m"} {"ev":"evunreg","m"} for the edit box's handler,
//   {"ev":"mcursor","n"} (MouseSetCursor) and {"ev":"cb","first","rows"} (the scroll callback).
//   Lists print as rich_text.jsonl's nodes, a reference as r(its list).

#include "oracle_support.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <cmath>
#include <new>

#include <string>
#include <vector>

// The client's sound archetypes are compiled into Oracle.exe by
// oracle_audio_client.cpp (menu_sound_arch.cpp); the controls here call
// recording stand-ins of their own under other names, so the two suites
// neither share state nor define a symbol twice.
#define LookupClientSoundArch OracleCtlLookupClientSoundArch
#define PlayClientSoundArchIfIdle OracleCtlPlayClientSoundArchIfIdle
#include "../OutpostFrontier/Client/win3d.cpp"
#include "../OutpostFrontier/Client/wincontrol_base.cpp"
#include "../OutpostFrontier/Client/win_factory.cpp"
#include "../OutpostFrontier/Client/nn_ui_pulse.cpp"
#include "../OutpostFrontier/Client/list_box.cpp"
#include "../OutpostFrontier/Client/ui_list_box.cpp"
#include "../OutpostFrontier/Client/ui_scroll.cpp"
#include "../OutpostFrontier/Client/win_object.cpp"
#include "../OutpostFrontier/Client/ui_win_object.cpp"
#include "../OutpostFrontier/Client/nn_text_control.cpp"
#define HasInstance StaticHasInstance
#define kInv255 StaticInv255
#define SetHolderTint StaticSetHolderTint
#define SetIdentityTransform StaticSetIdentityTransform
#include "../OutpostFrontier/Client/nn_static_control.cpp"
#undef HasInstance
#undef kInv255
#undef SetHolderTint
#undef SetIdentityTransform
#define HasInstance ButtonHasInstance
#define kInv255 ButtonInv255
#define SetHolderTint ButtonSetHolderTint
#define kOne ButtonOne
#define TellParent ButtonTellParent
#include "../OutpostFrontier/Client/nn_button_control.cpp"
#undef HasInstance
#undef kInv255
#undef SetHolderTint
#undef kOne
#undef TellParent
#include "../OutpostFrontier/Client/wire_controls.cpp"
#define SetIdentityTransform ScrollSetIdentityTransform
#include "../OutpostFrontier/Client/scroll_control.cpp"
#undef SetIdentityTransform
#include "../OutpostFrontier/Client/ui_table.cpp"
// The rich-text windows (rich_text_win.cpp, ui_text_edit.cpp, ui_fast_table.cpp). The IME box and
// the IME focus (fl_ime_input_box.h over imeui.h) are declared under other names and stand-ins take
// theirs: no case has an IME focus, so no box is ever made.
#define FLImeInputBox OracleDeclaredImeInputBox
#define IMEFocus OracleDeclaredImeFocus
#include "../OutpostFrontier/Client/fl_ime_input_box.h"
#undef FLImeInputBox
#undef IMEFocus
struct FLImeInputBox
{
    explicit FLImeInputBox(RichTextEditWin*) {}
    void SetInputBufferSize(DWORD) {}
    void Resize(RECT const*) {}
    bool HandleKey(UINT) { return false; }
    bool HandleMultiByteChar(char) { return false; }
};
struct IMEFocus
{
    static IMEFocus* GetSingleton() { return nullptr; }
    void SetFocus(FLImeInputBox*, bool) {}
};
#include "../OutpostFrontier/Client/rich_text_win.cpp"
#include "../OutpostFrontier/Client/ui_text_edit.cpp"
#include "../OutpostFrontier/Client/ui_fast_table.cpp"

namespace
{
    oracle::Suite* GCtlOut = nullptr;
    unsigned int GCtlCase = 0;
    Win3D* GCtlObject = nullptr;
    double GCtlFrameTime = 0.0;
    int GCtlCursor[2] = {0, 0};
    int GCtlOption = 1;
    int GCtlOptions[0x20];
    double GCtlElapsed = 0.0;
    int GCtlInstances = 0;
    float GCtlXforms[8][12];

    void CtlEvent(const std::string& Fields)
    {
        if (GCtlOut != nullptr)
            GCtlOut->Line(oracle::Format("{\"case\":\"%u\",", GCtlCase) + Fields + "}");
    }

    std::string Bits(float Value)
    {
        return oracle::Format("%08x", oracle::FloatBits(Value));
    }

    std::string Bits3(float const* V)
    {
        return Bits(V[0]) + " " + Bits(V[1]) + " " + Bits(V[2]);
    }

    std::string BitsN(float const* V, int Count)
    {
        std::string Out;
        for (int Index = 0; Index < Count; ++Index)
            Out += (Index == 0 ? "" : " ") + Bits(V[Index]);
        return Out;
    }

    std::string Units(wchar_t const* Text)
    {
        if (Text == nullptr)
            return "null";
        std::string Out;
        for (; *Text != 0; ++Text)
            Out += oracle::Format("%04x", (unsigned int)(unsigned short)*Text);
        return Out;
    }

    std::string Rgba(unsigned char const* C)
    {
        return oracle::Format("%02x%02x%02x%02x", C[0], C[1], C[2], C[3]);
    }

    // The recording pipeline: SetRenderState only.
    class FOracleCtlPipeline final : public IRenderPipeline
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Result) override
        {
            *Result = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API SetRenderState(D3DRENDERSTATETYPE State, int Value) override
        {
            CtlEvent(oracle::Format("\"ev\":\"rs\",\"s\":\"%d\",\"v\":\"%d\"", (int)State, Value));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetRenderState(D3DRENDERSTATETYPE State, int* Out) override
        {
            *Out = 0;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageState(int Stage, int Type, int Value) override
        {
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageTexture(int Stage, void* Handle) override
        {
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetPipelineState(int Id, int Value) override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API QueryDeviceAbility(unsigned int Index, unsigned long* Result) override
        {
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API CreateTexture(int Width, int Height, unsigned int* PixelFormat,
                                                     unsigned int Levels, unsigned int Flags,
                                                     void** OutHandle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DestroyTexture(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureFormat(void* Handle, int* OutFormat) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetTextureLevelData(void* Handle, int Level, int SrcW, int SrcH,
                                                           unsigned int Pitch, unsigned int* SrcFormat,
                                                           const void* Pixels, void* A,
                                                           void* Palette) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API Startup(const char* SectionName) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ShutdownPipeline() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetPipelineState(int Id, int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDeviceInfo(void* Out520) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDisplayModeCount(unsigned int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDisplayModeEnum(
            unsigned int Index, void* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SelectMode(
            int* Mode, unsigned int* OutIndex) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API CreateBuffers(
            HWND Hwnd, int* Mode, int* OutMode) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetModeBlob(
            void* OutAdapter, void* OutMode10Dwords) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DestroyGpuResources() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ClearBuffers(
            unsigned long Flags, const void* Rects) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SwapBuffers() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LockBuffer(void* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UnlockBuffer() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetBufferInterface(
            void* Out, void* Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDeviceStats(void* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetViewportRect(
            int X, int Y, int W, int H) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetViewportRect(
            int* OutX, int* OutY, int* OutW, int* OutH) override
        {
            *OutX = 0;
            *OutY = 0;
            *OutW = 0;
            *OutH = 0;
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetDepthRange(float MinZ, float MaxZ) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetDepthRange(
            float* OutMinZ, float* OutMaxZ) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetWindow(
            HWND Hwnd, int X, int Y, int W, int H) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetWindow(
            HWND* OutHwnd, int* OutLeft, int* OutRight, int* OutW, int* OutH) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetWorld(const float* M12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetWorld(float* Out12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetView(const float* M12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetView(float* Out12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetModelview(const float* M12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetModelview(float* Out12) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetProjection(const float* M16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetProjection(float* Out16) override
        {
            for (int Index = 0; Index < 16; ++Index)
                Out16[Index] = 0.0f;
            return GR_GENERIC;
        }
        GENRESULT NEURONCOMPONENTS_API SetLookat(
            float EyeX, float EyeY, float EyeZ, float AtX, float AtY, float AtZ, float UpX, float UpY,
            float UpZ) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetOrtho(
            float Left, float Right, float Bottom, float Top, float NearPlane,
            float FarPlane) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetPerspective(
            float FovDeg, float Aspect, float NearPlane, float FarPlane) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetLight(
            void* Handle, const D3DLIGHT8* Light) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DestroyLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetLight(
            void* Handle, D3DLIGHT8* OutLight) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetLightEnable(
            void* Handle, int Enable) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetLightEnable(
            void* Handle, int* OutEnable) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetMaterial(
            const D3DMATERIAL8* Material) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetMaterial(D3DMATERIAL8* Material) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API IsTexture(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API LockTexture(
            void* Handle, int Level, void* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UnlockTexture(void* Handle, int Level) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureDim(
            void* Handle, unsigned int* OutW, unsigned int* OutH,
            int* OutLevels) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureInterface(
            void* Handle, void* UnusedA, void* UnusedB) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API BlitTexture(
            void* DstHandle, int DstLevel, void* SrcHandle, int SrcLevel) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetRenderTarget(
            void* Handle, void* UnusedA, void* UnusedB) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetRenderTarget(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API BeginScene() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API EndScene() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API ResetRenderStatesToDefaults() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageState(
            int Stage, int Type, int* Out) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetTextureStageTransform(
            int Stage, const float* Matrix16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageTransform(
            int Stage, float* OutMatrix16) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API GetTextureStageTexture(
            int Stage, void** OutHandle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API VerifyState() override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DrawPrimitive(
            unsigned long Prim, unsigned int Fvf, const void* Verts, unsigned int Count,
            void* Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitiveUp(
            unsigned long Prim, unsigned int Fvf, const void* Verts, unsigned int VertCount,
            const void* Indices, unsigned int IndexCount,
            unsigned long Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DrawPrimitiveVb(
            unsigned long Prim, void* Vb, unsigned int StartVertex, unsigned int Count,
            void* Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitiveVb(
            unsigned long Prim, void* Vb, unsigned int BaseVertex, unsigned int NumVerts,
            const void* Indices, unsigned int IndexCount,
            unsigned long Unused) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API AddLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API RemoveLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API UpdateLight(void* Handle) override { return GR_GENERIC; }
        GENRESULT NEURONCOMPONENTS_API SetWorldN(
            int WorldIndex, const float* M12) override { return GR_GENERIC; }
    };

    // The parent every control is created under: slot 23 and SetProperty recorded.
    class FOracleCtlParent final : public WinControl
    {
    public:
        std::string Arg(uintptr_t A) const
        {
            if (GCtlObject != nullptr &&
                (A == reinterpret_cast<uintptr_t>(GCtlObject) ||
                 A == reinterpret_cast<uintptr_t>(GCtlObject->AsWinControl())))
                return "self";
            return oracle::Format("%lx", (unsigned long)A);
        }
        bool Slot23(unsigned Msg, unsigned A, unsigned B) override
        {
            std::string Text;
            if ((Msg == 0xf001u || Msg == 0xf002u) && A != reinterpret_cast<uintptr_t>(GCtlObject))
                Text = Units(reinterpret_cast<wchar_t const*>(static_cast<uintptr_t>(A)));
            else
                Text = Arg(A);
            CtlEvent(oracle::Format("\"ev\":\"msg\",\"m\":\"%x\",\"a\":\"%s\",\"b\":\"%d\"", Msg, Text.c_str(),
                                    (int)B));
            return true;
        }
        intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override
        {
            std::string const Text =
                Msg == 0x45 ? Units(reinterpret_cast<wchar_t const*>(A)) : Arg(A);
            CtlEvent(oracle::Format("\"ev\":\"prop\",\"m\":\"%x\",\"a\":\"%s\",\"b\":\"%d\"", Msg, Text.c_str(),
                                    (int)B));
            return 0;
        }
    };

    // Peeks at the scroll bar's protected fields.
    class FUiScrollPeek : public UiScroll
    {
    public:
        static FUiScrollPeek* Of(Win3D* Object) { return static_cast<FUiScrollPeek*>(static_cast<UiScroll*>(
                                                      static_cast<WinControl*>(Object))); }
        std::string State() const
        {
            FScrollHitRect const* const Rects[5] = {&TopArrow_, &BottomArrow_, &TrackBefore_, &TrackAfter_,
                                                    &Thumb_};
            std::string RectText;
            for (int Index = 0; Index < 5; ++Index)
            {
                RectText += (Index == 0 ? "" : " ") + Bits(Rects[Index]->X0) + " " + Bits(Rects[Index]->Y0) +
                            " " + Bits(Rects[Index]->X1) + " " + Bits(Rects[Index]->Y1) +
                            oracle::Format(" %d", Rects[Index]->bHot ? 1 : 0);
            }
            float const Values[5] = {Value_, RangeMin_, RangeMax_, Page_, Step_};
            return oracle::Format("\"ev\":\"scroll\",\"flags\":\"%02x\",\"pos\":\"", Flags) + Bits3(Position) +
                   "\",\"v\":\"" + BitsN(Values, 5) + "\",\"rect\":\"" + RectText + "\",\"arrow\":\"" +
                   Bits(ArrowSize_) + oracle::Format("\",\"drag\":\"%d\",\"rt\":\"", DragPart_) +
                   Bits(RepeatTimer_) + oracle::Format("\",\"dragging\":\"%d\",\"doff\":\"", bDragging_ ? 1 : 0) +
                   Bits(DragOffset_) + "\",\"inv\":\"" + BitsN(InverseLayout_, 12) +
                   oracle::Format("\",\"colors\":\"%08lx %08lx %08lx %08lx\",\"soff\":\"", ArrowIdleColor_,
                                  ArrowHotColor_, BarIdleColor_, BarHotColor_) +
                   Bits3(ScrollOffset_) + "\",\"dpage\":\"" + Bits(DefaultPage_) + "\"";
        }
        void SetActive(bool b) { bActive_ = b; }
        void SetCustom(bool b) { bCustomColor_ = b; }
    };

    std::vector<std::string> Split(const std::string& Text, const std::string& Separator)
    {
        std::vector<std::string> Out;
        size_t Start = 0;
        for (;;)
        {
            size_t const Found = Text.find(Separator, Start);
            Out.push_back(Text.substr(Start, Found == std::string::npos ? std::string::npos : Found - Start));
            if (Found == std::string::npos)
                return Out;
            Start = Found + Separator.size();
        }
    }

    float F(const std::string& Hex)
    {
        return oracle::FloatFromBits((unsigned int)strtoul(Hex.c_str(), nullptr, 16));
    }

    int I(const std::string& Text)
    {
        return (int)strtol(Text.c_str(), nullptr, 10);
    }

    std::vector<wchar_t> WideFromHex(const std::string& Hex)
    {
        std::vector<wchar_t> Out;
        for (size_t Index = 0; Index + 3 < Hex.size(); Index += 4)
            Out.push_back((wchar_t)strtoul(Hex.substr(Index, 4).c_str(), nullptr, 16));
        Out.push_back(0);
        return Out;
    }

    std::string BytesFromHex(const std::string& Hex)
    {
        std::string Out;
        for (size_t Index = 0; Index + 1 < Hex.size(); Index += 2)
            Out += (char)strtoul(Hex.substr(Index, 2).c_str(), nullptr, 16);
        return Out;
    }

    void ListState(ListBox* List)
    {
        float const Timers[2] = {List->Timer890, List->Timer894};
        CtlEvent(oracle::Format("\"ev\":\"list\",\"hover\":\"%d\",\"f80\":\"%d\",\"sel\":\"%d\",\"count\":\"%d\","
                                "\"first\":\"%d\",\"rows\":\"%d\",\"t\":\"",
                                List->HoverIndex, List->Field80, List->SelectedIndex, List->ItemCount,
                                List->FirstVisible, List->VisibleRows) +
                 BitsN(Timers, 2) + "\",\"w\":\"" + Bits(List->Width) + "\",\"h\":\"" + Bits(List->Height) + "\"");
    }

    void ObjectState(Win3D* Object)
    {
        // UIWinObject's own fields are private: +0xe8 the origin, +0xf4 the corner.
        unsigned char const* const Raw = reinterpret_cast<unsigned char const*>(Object);
        float Origin[3];
        float Corner[3];
        memcpy(Origin, Raw + 0xe8, sizeof(Origin));
        memcpy(Corner, Raw + 0xf4, sizeof(Corner));
        CtlEvent(oracle::Format("\"ev\":\"obj\",\"flags\":\"%02x\",\"pos\":\"", Object->Flags) +
                 Bits3(Object->Position) + "\",\"orient\":\"" + BitsN(Object->Orientation, 9) +
                 "\",\"origin\":\"" + Bits3(Origin) + "\",\"corner\":\"" + Bits3(Corner) + "\"");
    }


    // The mesh instances of a case: the loader hands out 0x1000, 0x1001, ...; each keeps the
    // transform the engine calls set.
    int InstanceIndex(void const* Instance)
    {
        uintptr_t const Raw = reinterpret_cast<uintptr_t>(Instance);
        if (Raw < 0x1000 || Raw >= 0x1008)
            return 0;
        return (int)(Raw - 0x1000);
    }

    std::string MeshEv(char const* Name, int Index)
    {
        std::string Out = oracle::Format("\"ev\":\"%s\"", Name);
        if (Index > 0)
            Out += oracle::Format(",\"i\":\"%d\"", Index);
        return Out;
    }

    void MeshPos(int Index, float const* P)
    {
        GCtlXforms[Index][9] = P[0];
        GCtlXforms[Index][10] = P[1];
        GCtlXforms[Index][11] = P[2];
        CtlEvent(MeshEv("mpos", Index) + ",\"x\":\"" + Bits(P[0]) + "\",\"y\":\"" + Bits(P[1]) + "\",\"z\":\"" +
                 Bits(P[2]) + "\"");
    }

    // The engine the controls place their instances through (NeuronRuntime::Engine while the suite
    // runs): positions, orientations, matrices and updates recorded, the transforms kept, a radius
    // of 0.25 for every instance.
    class FOracleCtlEngine final : public IEngine
    {
    public:
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Result) override
        {
            *Result = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        unsigned long NEURONCOMPONENTS_API AddRef() override { return 1; }
        unsigned long NEURONCOMPONENTS_API Release() override { return 1; }
        GENRESULT NEURONCOMPONENTS_API UpdateInstance(void* Instance, unsigned long Flags, void* Arg) override
        {
            CtlEvent(MeshEv("mupd", InstanceIndex(Instance)));
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetInstancePosition(void* Instance, void* Position) override
        {
            MeshPos(InstanceIndex(Instance), static_cast<float const*>(Position));
            return GR_OK;
        }
        float* NEURONCOMPONENTS_API GetInstancePosition(void* Instance) override
        {
            return &GCtlXforms[InstanceIndex(Instance)][9];
        }
        GENRESULT NEURONCOMPONENTS_API SetInstanceOrientation(void* Instance, void* Orient) override
        {
            int const Index = InstanceIndex(Instance);
            memcpy(GCtlXforms[Index], Orient, 9 * sizeof(float));
            CtlEvent(MeshEv("mori", Index) + ",\"m\":\"" + BitsN(GCtlXforms[Index], 9) + "\"");
            return GR_OK;
        }
        float* NEURONCOMPONENTS_API GetInstanceOrientation(void* Instance) override
        {
            return GCtlXforms[InstanceIndex(Instance)];
        }
        GENRESULT NEURONCOMPONENTS_API SetInstanceMatrix(void* Instance, void* Matrix) override
        {
            int const Index = InstanceIndex(Instance);
            memcpy(GCtlXforms[Index], Matrix, 12 * sizeof(float));
            CtlEvent(MeshEv("mxf", Index) + ",\"m\":\"" + BitsN(GCtlXforms[Index], 12) + "\"");
            return GR_OK;
        }
        float* NEURONCOMPONENTS_API GetInstanceTransform(void* Instance) override
        {
            return GCtlXforms[InstanceIndex(Instance)];
        }
        GENRESULT NEURONCOMPONENTS_API GetInstanceExtent(void* Instance, unsigned long Flags, float* OutExtent,
                                                         float* OutVec) override
        {
            *OutExtent = 0.25f;
            OutVec[0] = 0.0f;
            OutVec[1] = 0.0f;
            OutVec[2] = 0.0f;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API LoadEngineComponents(INeuronComponent* SystemContainer) override { return 0; }
        unsigned long NEURONCOMPONENTS_API EnumerateEngineComponents(int(__cdecl* Callback)(const char*, void*)) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API NotifyEngineComponents(void* Arg) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API SetSearchPath(const char* Path) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API QuerySearchPath(char* Dest, unsigned long MaxChars) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API SetFilesystem(INeuronComponent* NewPath) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API GetSearchPath(void** Out) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API CreateFileSystem(const char* Path, void** Out, void* Factory) override { return 0; }
        void* NEURONCOMPONENTS_API CreateCompoundArchetype(FEngineName* Name) override { return nullptr; }
        void* NEURONCOMPONENTS_API CreateArchetypeHandle(FEngineName* Name, void* FileSystem) override { return nullptr; }
        void* NEURONCOMPONENTS_API DuplicateArchetype(void* ArchIn, FEngineName* Name) override { return nullptr; }
        unsigned long NEURONCOMPONENTS_API AddRefHandle(void* Handle) override { return 0; }
        unsigned long NEURONCOMPONENTS_API ReleaseHandle(void* Handle) override { return 0; }
        FEngineName* NEURONCOMPONENTS_API GetArchetypeName(FEngineName* Out, void* Handle) override { return nullptr; }
        void* NEURONCOMPONENTS_API FindArchetype(FEngineName* Name) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API QueryEngineComponent(int Id, void* Name, void** Out) override { return 0; }
        unsigned long NEURONCOMPONENTS_API IsCompoundArchetype(void* Handle) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API EnumerateArchetypeConstraints(void* Handle, int(__cdecl* Callback)(void*, void*, void*, void*), void* Context) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API EnumerateArchetypeChildren(void* Handle, int(__cdecl* Callback)(void*, void*, int, void*), void* Context) override { return 0; }
        void* NEURONCOMPONENTS_API CreateInstanceFromName(FEngineName* Name, void* FileSystem, void* Transform) override { return nullptr; }
        void* NEURONCOMPONENTS_API CreateInstance(void* Handle, void* Transform) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API DestroyInstance(void* Instance) override { return 0; }
        void* NEURONCOMPONENTS_API GetInstanceArchetype(void* Instance) override { return nullptr; }
        FEngineName* NEURONCOMPONENTS_API GetInstanceName(FEngineName* Out, void* Instance) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API SetInstanceTransform(void* Instance, void* Transform) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API GetInstanceTransformObject(void* Instance, void** Out) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API QueryInstance(void* Instance, const char* Iid, void** Out) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API SetInstanceUserData(void* Instance, void* Value) override { return 0; }
        void* NEURONCOMPONENTS_API GetInstanceUserData(void* Instance) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API SetInstanceExtent(void* Instance, unsigned long Flags, float Extent, const float* Vec) override { return 0; }
        GENRESULT NEURONCOMPONENTS_API NoOp41(void* A, void* B) override { return 0; }
        float* NEURONCOMPONENTS_API GetIdentityMatrix(void* Unused) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API NoOp43(void* A, void* B) override { return 0; }
        float* NEURONCOMPONENTS_API GetIdentityMatrix2(void* Unused) override { return nullptr; }
        GENRESULT NEURONCOMPONENTS_API SetTransformUser(void* Instance, unsigned long User) override { return 0; }
        unsigned long NEURONCOMPONENTS_API GetTransformUser(void* Instance) override { return 0; }
        unsigned long NEURONCOMPONENTS_API CreateJoint(void* Parent, void* Child, const unsigned long* Payload) override { return 0; }
        unsigned long NEURONCOMPONENTS_API DestroyJoint(void* Parent, void* Child) override { return 0; }
        unsigned long* NEURONCOMPONENTS_API GetInstanceRelative(void* Instance) override { return nullptr; }
        unsigned long NEURONCOMPONENTS_API SetJointParams(void* Instance, unsigned long Index, const float* Values) override { return 0; }
        unsigned long NEURONCOMPONENTS_API GetJointParams(void* Instance, unsigned long Index, float* Out) override { return 0; }
        void* NEURONCOMPONENTS_API FindMappedInstance(void* Instance) override { return nullptr; }
        void* NEURONCOMPONENTS_API GetInstanceParent(void* Instance) override { return nullptr; }
        void* NEURONCOMPONENTS_API GetRelatedInstance(void* From, unsigned long Flags, void* Cursor) override { return nullptr; }
        void* NEURONCOMPONENTS_API GetInstanceRoot(void* Instance) override { return nullptr; }
        void* NEURONCOMPONENTS_API FindArchetypeChild(void* Handle, void* ChildHandle) override { return nullptr; }
        unsigned long NEURONCOMPONENTS_API RejectOperation(void* A, void* B) override { return 0; }
    };

    std::string Bits12(float const* V)
    {
        return BitsN(V, 12);
    }

    std::string Pending(wchar_t const* Text)
    {
        return (Text != nullptr && Text[0] != 0) ? Units(Text) : std::string();
    }

    void TextState(NnTextControl* T, NnEditControl* E)
    {
        float const Off[3] = {T->Offset[0], T->Offset[1], T->Offset[2]};
        std::string Line =
            oracle::Format("\"ev\":\"text\",\"flags\":\"%02x\",\"pos\":\"", T->Flags & 0x0f) + Bits3(T->Position) +
            "\",\"off\":\"" + Bits3(Off) + "\",\"w\":\"" + Bits(T->Width) + "\",\"h\":\"" + Bits(T->Height) +
            "\",\"t\":\"" + Units(T->GetFontString()->Text) +
            oracle::Format("\",\"tc\":\"%d\",\"te\":\"", T->TypedCount) + Bits(T->TypeElapsed) + "\",\"ti\":\"" +
            Bits(T->TypeInterval) +
            oracle::Format("\",\"tw\":\"%d\",\"typing\":\"%d\",\"pend\":\"", T->bTypewriter, T->bTyping) +
            Pending(T->PendingText) + oracle::Format("\",\"blink\":\"%d\",\"ph\":\"%d\",\"be\":\"", T->bBlink,
                                                     T->BlinkPhase) +
            Bits(T->BlinkElapsed) + "\",\"oc\":\"" + Rgba(T->OverrideColour) +
            oracle::Format("\",\"co\":\"%d\",\"cap\":\"%d\",\"lc\":\"%d\",\"n\":\"%d\"", T->bColourOverride,
                           T->bInCaptureList, T->Field340, G_TextControlTypingCount);
        if (E != nullptr)
            Line += ",\"ct\":\"" + Bits(E->CursorTimer) +
                    oracle::Format("\",\"cs\":\"%d\",\"ml\":\"%d\",\"ed\":\"%d\",\"dg\":\"%d\"", E->bCursorShown,
                                   E->MaxLength, E->bEditable, E->bDigitsOnly);
        CtlEvent(Line);
    }

    void StaticState(NnStaticControl* S)
    {
        float const Angles[3] = {S->CurRoll, S->CurPitch, S->CurYaw};
        float const Off[3] = {S->Offset[0], S->Offset[1], S->Offset[2]};
        CtlEvent(oracle::Format("\"ev\":\"static\",\"flags\":\"%02x\",\"pos\":\"", S->Flags & 0x0f) + Bits3(S->Position) +
                 "\",\"orient\":\"" + BitsN(S->Orientation, 9) + "\",\"off\":\"" + Bits3(Off) + "\",\"a\":\"" +
                 Bits3(Angles) + "\",\"s\":\"" + Bits3(S->StartYpr) +
                 oracle::Format("\",\"f\":\"%d%d%d%d%d%d%d%d%d%d\",\"fr\":\"", S->bNoMeshRender, S->bNoWireRender,
                                S->bNoColoring, S->Field_0x347, S->bUseColours, S->bEnableRotation,
                                S->bNoClearRender, S->bRenderToWindow, S->bZClear, S->bNoZEnable) +
                 Bits(S->ForceRadius) + "\",\"rs\":\"" + Bits(S->RotationSpeed) + "\",\"mc\":\"" +
                 Rgba(S->MeshColour) + "\",\"wc\":\"" + Rgba(S->WireColour) + "\"");
    }

    void ButtonState(NnButtonControl* B, NnToggleButtonControl* T)
    {
        float const Off[3] = {B->Offset[0], B->Offset[1], B->Offset[2]};
        CtlEvent(oracle::Format("\"ev\":\"button\",\"flags\":\"%02x\",\"pos\":\"", B->Flags & 0x0f) + Bits3(B->Position) +
                 "\",\"orient\":\"" + BitsN(B->Orientation, 9) + "\",\"off\":\"" + Bits3(Off) +
                 oracle::Format("\",\"b\":\"%02x\",\"b2\":\"%02x\",\"spin\":\"", B->ButtonBits, B->ButtonBits2) +
                 Bits(B->SpinAngle) + "\",\"bt\":\"" + Bits(B->BlinkTimer) +
                 oracle::Format("\",\"ph\":\"%d\",\"glow\":\"%08lx\",\"cmd\":\"%d\",\"tip\":\"%d\",\"tip2\":\"%d\","
                                "\"slot\":\"%d\",\"ts\":\"%d\",\"uc\":\"%d\",\"col\":\"",
                                B->bBlinkPhase, B->GlowArgb, B->CommandId, B->ToolTipIds, B->ToolTip2Ids,
                                B->UiSoundSlot, T != nullptr ? T->ToggleState : -1, B->bUseColour) +
                 Rgba(B->Colour) + "\"");
    }

    void WireState(WireStaticControl* W, WireButtonControl* B)
    {
        CtlEvent(oracle::Format("\"ev\":\"wire\",\"flags\":\"%02x\",\"pos\":\"", W->Flags & 0x0f) + Bits3(W->Position) +
                 "\",\"c\":\"" + Bits3(W->WireCenter) +
                 oracle::Format("\",\"st\":\"%02x\",\"m\":\"%x\",\"bl\":\"%d\",\"ph\":\"%d\",\"bt\":\"", W->WireState,
                                W->EdgeMask, W->bBlink, W->bBlinkPhase) +
                 Bits(W->BlinkTimer) +
                 oracle::Format("\",\"bs\":\"%d\",\"cmd\":\"%d\",\"slot\":\"%d\"", B != nullptr ? B->ButtonState & 7 : -1,
                                B != nullptr ? B->CommandId : 0, W->SoundSlot));
    }

    void ScrollControlState(ScrollControl* C)
    {
        CtlEvent(oracle::Format("\"ev\":\"sctl\",\"v\":\"%d\",\"min\":\"%d\",\"max\":\"%d\",\"pg\":\"%d\",\"st\":\"%d\","
                                "\"valid\":\"%d\",\"f\":\"%d%d%d%d%d%d%d\",\"rt\":\"",
                                C->Value, C->MinRange, C->MaxRange, C->PageSize, C->Step, C->bThumbValid,
                                C->bDragging, C->bPaging, C->bNextPressed, C->bPrevPressed, C->bThumbHover,
                                C->bNextHover, C->bPrevHover) +
                 Bits(C->RepeatTime) + "\",\"back\":\"" +
                 Bits12(reinterpret_cast<float const*>(&C->Back.Xform)) + "\",\"next\":\"" +
                 Bits12(reinterpret_cast<float const*>(&C->Next.Xform)) + "\",\"prev\":\"" +
                 Bits12(reinterpret_cast<float const*>(&C->Prev.Xform)) + "\",\"thumb\":\"" +
                 Bits12(reinterpret_cast<float const*>(&C->ThumbXform)) + "\",\"ts\":\"" +
                 Bits12(reinterpret_cast<float const*>(&C->ThumbStart)) + "\",\"te\":\"" +
                 Bits12(reinterpret_cast<float const*>(&C->ThumbEnd)) + "\"");
    }

    void TableState(UiTable* T)
    {
        std::string Rows;
        for (size_t Index = 0; Index < T->RowEdges.size(); ++Index)
            Rows += (Index == 0 ? "" : " ") + Bits(T->RowEdges[Index]);
        std::string Columns;
        for (size_t Index = 0; Index < T->ColumnEdges.size(); ++Index)
            Columns += (Index == 0 ? "" : " ") + Bits(T->ColumnEdges[Index]);
        CtlEvent("\"ev\":\"table\",\"w\":\"" + Bits(T->Width) + "\",\"h\":\"" + Bits(T->Height) + "\",\"re\":\"" +
                 Rows + "\",\"ce\":\"" + Columns +
                 oracle::Format("\",\"sel\":\"%d %u\",\"hov\":\"%d %u\",\"drag\":\"%d%d%d %u\",\"do\":\"",
                                T->bSelected, T->SelectedRow, T->bHovered, T->HoveredRow, T->bDragArmed,
                                T->bDragging, T->bDragColumn, T->DragEdge) +
                 Bits(T->DragOffset) + "\",\"inv\":\"" + Bits12(T->InverseTransform) + "\"");
    }

    // "-" is null; anything else hex bytes.
    std::string NullOr(const std::string& Hex, bool& bNull)
    {
        bNull = Hex == "-";
        return bNull ? std::string() : BytesFromHex(Hex);
    }

    void Rgba4(const std::string& Hex, unsigned char* Out)
    {
        unsigned long const Packed = strtoul(Hex.c_str(), nullptr, 16);
        Out[0] = (unsigned char)(Packed >> 24);
        Out[1] = (unsigned char)(Packed >> 16);
        Out[2] = (unsigned char)(Packed >> 8);
        Out[3] = (unsigned char)Packed;
    }

    // The Init operation of the later controls.
    void InitControl(Win3D* O, const std::string& Class, const std::vector<std::string>& T)
    {
        bool bNull[8];
        if (Class == "TextControl" || Class == "EditControl")
        {
            std::string const Style = NullOr(T[1], bNull[0]);
            std::string const Hp = NullOr(T[5], bNull[1]);
            float const P[3] = {F(T[2]), F(T[3]), F(T[4])};
            static_cast<NnTextControl*>(static_cast<WinControl*>(O))
                ->Init(bNull[0] ? nullptr : Style.c_str(), P, bNull[1] ? nullptr : Hp.c_str(), F(T[6]), F(T[7]),
                       0.0f, (unsigned)I(T[8]), I(T[9]), I(T[10]), (unsigned char)I(T[11]));
        }
        else if (Class == "StaticControl")
        {
            std::string const Mesh = NullOr(T[1], bNull[0]);
            std::string const Hp = NullOr(T[5], bNull[1]);
            float const Off[3] = {F(T[2]), F(T[3]), F(T[4])};
            float const Half[3] = {F(T[6]), F(T[7]), F(T[8])};
            float const Ypr[3] = {F(T[19]), F(T[20]), F(T[21])};
            static_cast<NnStaticControl*>(static_cast<WinControl*>(O))
                ->Init(bNull[0] ? nullptr : Mesh.c_str(), Off, bNull[1] ? nullptr : Hp.c_str(), Half,
                       (unsigned char)I(T[9]), (unsigned char)I(T[10]), (unsigned char)I(T[11]),
                       (unsigned char)I(T[12]), (unsigned char)I(T[13]), (unsigned char)I(T[14]),
                       (unsigned char)I(T[15]), I(T[16]), I(T[17]), (unsigned char)I(T[18]), Ypr);
        }
        else if (Class == "ButtonControl" || Class == "ToggleButtonControl" || Class == "RichTextButtonControl")
        {
            std::string const Mesh = NullOr(T[1], bNull[0]);
            std::string const Mesh2 = NullOr(T[2], bNull[1]);
            std::string const Sound = NullOr(T[3], bNull[2]);
            std::string const Key = NullOr(T[4], bNull[3]);
            std::string const Hp = NullOr(T[8], bNull[4]);
            float const Off[3] = {F(T[5]), F(T[6]), F(T[7])};
            float const Half[3] = {F(T[9]), F(T[10]), F(T[11])};
            char const* const A0 = bNull[0] ? nullptr : Mesh.c_str();
            char const* const A1 = bNull[1] ? nullptr : Mesh2.c_str();
            char const* const A2 = bNull[2] ? nullptr : Sound.c_str();
            char const* const A3 = bNull[3] ? nullptr : Key.c_str();
            char const* const A5 = bNull[4] ? nullptr : Hp.c_str();
            if (Class == "ToggleButtonControl")
                static_cast<NnToggleButtonControl*>(static_cast<WinControl*>(O))
                    ->Init(A0, A1, A2, A3, Off, A5, Half, (unsigned char)I(T[12]), (unsigned char)I(T[13]),
                           (unsigned char)I(T[14]), (unsigned char)I(T[15]), I(T[16]), I(T[17]));
            else
                static_cast<NnButtonControl*>(static_cast<WinControl*>(O))
                    ->Init(A0, A1, A2, A3, Off, A5, Half, (unsigned char)I(T[12]), (unsigned char)I(T[13]),
                           (unsigned char)I(T[14]), (unsigned char)I(T[15]), I(T[16]), I(T[17]), I(T[18]));
        }
        else if (Class == "WireStaticControl")
        {
            float const Off[3] = {F(T[1]), F(T[2]), F(T[3])};
            float const Half[3] = {F(T[4]), F(T[5]), F(T[6])};
            static_cast<WireStaticControl*>(static_cast<WinControl*>(O))
                ->Init(Off, Half, (unsigned char)I(T[7]), (unsigned char)I(T[8]), I(T[9]), (unsigned)I(T[10]));
        }
        else if (Class == "WireButtonControl")
        {
            std::string const Sound = NullOr(T[1], bNull[0]);
            float const Off[3] = {F(T[2]), F(T[3]), F(T[4])};
            float const Half[3] = {F(T[5]), F(T[6]), F(T[7])};
            static_cast<WireButtonControl*>(static_cast<WinControl*>(O))
                ->Init(bNull[0] ? nullptr : Sound.c_str(), Off, Half, (unsigned char)I(T[8]),
                       (unsigned char)I(T[9]), I(T[10]), I(T[11]), (unsigned)I(T[12]));
        }
        else if (Class == "ScrollControl")
        {
            std::string Mesh[4];
            std::string Hp[5];
            bool bMesh[4];
            bool bHp[5];
            float V[11][3];
            bool bNo[4];
            for (int Part = 0; Part < 3; ++Part)
            {
                int const B = 1 + Part * 9;
                Mesh[Part] = NullOr(T[B], bMesh[Part]);
                V[Part * 2][0] = F(T[B + 1]);
                V[Part * 2][1] = F(T[B + 2]);
                V[Part * 2][2] = F(T[B + 3]);
                Hp[Part] = NullOr(T[B + 4], bHp[Part]);
                V[Part * 2 + 1][0] = F(T[B + 5]);
                V[Part * 2 + 1][1] = F(T[B + 6]);
                V[Part * 2 + 1][2] = F(T[B + 7]);
                bNo[Part] = I(T[B + 8]) != 0;
            }
            int const B = 28;
            Mesh[3] = NullOr(T[B], bMesh[3]);
            for (int K = 0; K < 3; ++K)
            {
                V[6][K] = F(T[B + 1 + K]);
                V[7][K] = F(T[B + 4 + K]);
                V[8][K] = F(T[B + 9 + K]);
            }
            Hp[3] = NullOr(T[B + 7], bHp[3]);
            Hp[4] = NullOr(T[B + 8], bHp[4]);
            bNo[3] = I(T[B + 12]) != 0;
            auto M = [&](int K) { return bMesh[K] ? nullptr : Mesh[K].c_str(); };
            auto H = [&](int K) { return bHp[K] ? nullptr : Hp[K].c_str(); };
            // The constructor leaves RepeatTime (+0x670) and bPageTowardMin (+0x67b) as
            // allocated: the zeroed allocation the Rust port starts from (divergence D442),
            // not whatever the heap holds.
            static_cast<ScrollControl*>(static_cast<WinControl*>(O))->RepeatTime = 0.0f;
            static_cast<ScrollControl*>(static_cast<WinControl*>(O))->bPageTowardMin = 0;
            static_cast<ScrollControl*>(static_cast<WinControl*>(O))
                ->Init(M(0), V[0], H(0), V[1], bNo[0], M(1), V[2], H(1), V[3], bNo[1], M(2), V[4], H(2), V[5],
                       bNo[2], M(3), V[6], V[7], H(3), H(4), V[8], bNo[3], I(T[B + 13]), I(T[B + 14]),
                       I(T[B + 15]), I(T[B + 16]) != 0, I(T[B + 17]) != 0);
        }
    }

    // The operations of the later controls; false when the operation is not one of them.
    bool LaterOp(Win3D* O, const std::string& Class, const std::vector<std::string>& T)
    {
        const std::string& Name = T[0];
        WinControl* const W = O->AsWinControl();
        bool const bText = Class == "TextControl" || Class == "EditControl";
        NnTextControl* const Text = bText ? static_cast<NnTextControl*>(W) : nullptr;
        NnEditControl* const Edit = Class == "EditControl" ? static_cast<NnEditControl*>(W) : nullptr;
        NnStaticControl* const Static = Class == "StaticControl" ? static_cast<NnStaticControl*>(W) : nullptr;
        bool const bButton =
            Class == "ButtonControl" || Class == "ToggleButtonControl" || Class == "RichTextButtonControl";
        NnButtonControl* const Button = bButton ? static_cast<NnButtonControl*>(W) : nullptr;
        NnToggleButtonControl* const Toggle =
            Class == "ToggleButtonControl" ? static_cast<NnToggleButtonControl*>(W) : nullptr;
        bool const bWire = Class == "WireStaticControl" || Class == "WireButtonControl";
        WireStaticControl* const Wire = bWire ? static_cast<WireStaticControl*>(W) : nullptr;
        WireButtonControl* const WireButton =
            Class == "WireButtonControl" ? static_cast<WireButtonControl*>(W) : nullptr;
        ScrollControl* const Sctl = Class == "ScrollControl" ? static_cast<ScrollControl*>(W) : nullptr;
        UiTable* const Table = Class == "UITable" ? static_cast<UiTable*>(O) : nullptr;
        if (Name == "init")
            InitControl(O, Class, T);
        else if (Name == "enter")
        {
            float Ray[3];
            Win3DGetMouseRay(Ray);
            O->MouseEnter(Ray);
        }
        else if (Name == "inactive")
            O->UpdateInactive();
        else if (Name == "key")
        {
            unsigned char Mods = (unsigned char)I(T[2]);
            bool const bResult = O->KeyEvent((unsigned)I(T[1]), &Mods);
            CtlEvent(oracle::Format("\"ev\":\"r\",\"v\":\"%d\"", bResult ? 1 : 0));
        }
        else if (Name == "time")
            GCtlElapsed = F(T[1]);
        else if (Name == "gopt")
            GCtlOptions[I(T[1])] = I(T[2]);
        else if (W != nullptr && Name == "prop")
        {
            intptr_t const Result = W->SetProperty((int)strtol(T[1].c_str(), nullptr, 16),
                                                   (uintptr_t)(intptr_t)I(T[2]), (uintptr_t)(intptr_t)I(T[3]));
            CtlEvent(oracle::Format("\"ev\":\"r\",\"v\":\"%d\"", (int)Result));
        }
        else if (W != nullptr && Name == "offset")
        {
            float V[3] = {F(T[1]), F(T[2]), F(T[3])};
            intptr_t const Result = W->SetProperty(6, reinterpret_cast<uintptr_t>(V), 0);
            CtlEvent(oracle::Format("\"ev\":\"r\",\"v\":\"%d\"", (int)Result));
        }
        else if (W != nullptr && Name == "text")
        {
            std::vector<wchar_t> Wide = WideFromHex(T[1]);
            intptr_t const Result = W->SetProperty(0x1d, reinterpret_cast<uintptr_t>(Wide.data()), 0);
            CtlEvent(oracle::Format("\"ev\":\"r\",\"v\":\"%d\"", (int)Result));
        }
        else if (W != nullptr && Name == "colour")
        {
            unsigned char C[4];
            Rgba4(T[1], C);
            intptr_t const Result = W->SetProperty(0x33, reinterpret_cast<uintptr_t>(C), (uintptr_t)I(T[2]));
            CtlEvent(oracle::Format("\"ev\":\"r\",\"v\":\"%d\"", (int)Result));
        }
        else if (W != nullptr && Name == "colours")
        {
            unsigned char C0[4];
            unsigned char C1[4];
            Rgba4(T[1], C0);
            Rgba4(T[2], C1);
            intptr_t const Result =
                W->SetProperty(0x18, reinterpret_cast<uintptr_t>(C0), reinterpret_cast<uintptr_t>(C1));
            CtlEvent(oracle::Format("\"ev\":\"r\",\"v\":\"%d\"", (int)Result));
        }
        else if (W != nullptr && (Name == "getcmd" || Name == "gettoggle"))
        {
            int Out = -7;
            W->SetProperty(Name == "getcmd" ? 0x2e : 0x2f, reinterpret_cast<uintptr_t>(&Out), 0);
            CtlEvent(oracle::Format("\"ev\":\"r\",\"v\":\"%d\"", Out));
        }
        else if (W != nullptr && Name == "sounds")
        {
            bool bNull0 = false;
            bool bNull1 = false;
            std::string const S0 = NullOr(T[1], bNull0);
            std::string const S1 = NullOr(T[2], bNull1);
            W->SetProperty(0x31, bNull0 ? 0 : reinterpret_cast<uintptr_t>(S0.c_str()),
                           bNull1 ? 0 : reinterpret_cast<uintptr_t>(S1.c_str()));
        }
        else if (Text != nullptr && Name == "tset")
        {
            std::vector<wchar_t> Wide = WideFromHex(T[1]);
            Text->SetText(Wide.data());
        }
        else if (Text != nullptr && Name == "ids")
            Text->SetIds((unsigned)I(T[1]));
        else if (Text != nullptr && Name == "width")
            Text->SetWidth(F(T[1]));
        else if (Text != nullptr && Name == "cached")
            Text->SetCachedDraw((unsigned char)I(T[1]), (unsigned char)I(T[2]));
        else if (Text != nullptr && Name == "number")
            Text->SetNumberText(BytesFromHex(T[1]).c_str());
        else if (Table != nullptr && Name == "cedge")
            CtlEvent(oracle::Format("\"ev\":\"r\",\"v\":\"%d\"", Table->SetColumnEdge((unsigned)I(T[1]), F(T[2]))));
        else if (Table != nullptr && Name == "redge")
            CtlEvent(oracle::Format("\"ev\":\"r\",\"v\":\"%d\"", Table->SetRowEdge((unsigned)I(T[1]), F(T[2]))));
        else if (Table != nullptr && Name == "rows")
        {
            for (size_t Index = 0; Index < Table->Rows.size() && 1 + Index * 2 + 1 < T.size(); ++Index)
            {
                Table->Rows[Index].bHoverable = (unsigned char)I(T[1 + Index * 2]);
                Table->Rows[Index].bSelectable = (unsigned char)I(T[2 + Index * 2]);
            }
        }
        else if (Name == "state")
        {
            if (Text != nullptr)
                TextState(Text, Edit);
            else if (Static != nullptr)
                StaticState(Static);
            else if (Button != nullptr)
                ButtonState(Button, Toggle);
            else if (Wire != nullptr)
                WireState(Wire, WireButton);
            else if (Sctl != nullptr)
                ScrollControlState(Sctl);
            else if (Table != nullptr)
                TableState(Table);
            else
                return false;
        }
        else
            return false;
        return true;
    }

    bool IsLaterClass(const std::string& Class)
    {
        return Class == "TextControl" || Class == "EditControl" || Class == "StaticControl" ||
               Class == "ButtonControl" || Class == "ToggleButtonControl" || Class == "RichTextButtonControl" ||
               Class == "WireStaticControl" || Class == "WireButtonControl" || Class == "ScrollControl" ||
               Class == "UITable";
    }

    // ---- the rich-text windows: the HUD's record and input focus, the edit box under test (its
    // handler face is the one the HUD stand-ins record), the list and state printers, the
    // operations.
    FHudCmdRecord GCtlRecord;
    FHudCmdHandler* GCtlInputFocus = nullptr;
    std::vector<FHudCmdHandler*> GCtlFocusStack;
    RichTextEditWin* GCtlEdit = nullptr;

    void FocusEvent(char const* Op, FHudCmdHandler* Handler)
    {
        std::string const Who =
            Handler == nullptr ? "null"
                               : (GCtlEdit != nullptr && Handler == GCtlEdit->AsHudCmdHandler() ? "self" : "other");
        CtlEvent(oracle::Format("\"ev\":\"focus\",\"op\":\"%s\",\"h\":\"%s\"", Op, Who.c_str()));
    }

    std::string RichUnits(wchar_t const* Text, int Count)
    {
        std::string Out;
        for (int Index = 0; Index < Count; ++Index)
            Out += oracle::Format("%04x", (unsigned int)(unsigned short)Text[Index]);
        return Out;
    }

    std::string RichListText(const RenderDisplayList& Rdl);

    // A node as rich_text.jsonl prints it; a reference as r(its list).
    std::string RichNodeText(RDLNode* Node)
    {
        if (TRANode* Tra = Node->operator TRANode*())
            return oracle::Format("a:%08lx:%08x:%08x", (unsigned long)Tra->get_attributes().Packed,
                                  (unsigned)Tra->get_mask(), (unsigned)Tra->get_default_mask());
        if (TextNode* Text = Node->operator TextNode*())
            return "t:" + RichUnits(Text->get_text().c_str(), (int)Text->get_text().size());
        if (Node->operator ParagraphNode*() != nullptr)
            return "n";
        if (ClipNode* Clip = Node->operator ClipNode*())
        {
            const tagRECT& R = Clip->get_rect();
            return oracle::Format("c:%ld:%ld:%ld:%ld", (long)R.left, (long)R.top, (long)R.right, (long)R.bottom);
        }
        if (RDLRefNode* Ref = Node->operator RDLRefNode*())
            return "r(" + RichListText(Ref->get_list()) + ")";
        return "other";
    }

    std::string RichListText(const RenderDisplayList& Rdl)
    {
        std::string Out;
        for (FRdlListNode* Cell = Rdl.NodeList->Next; Cell != Rdl.NodeList; Cell = Cell->Next)
        {
            if (!Out.empty())
                Out += " ";
            Out += RichNodeText(Cell->Node);
        }
        return Out;
    }

    void EditState(RichTextEditWin* E)
    {
        std::string Cells;
        for (size_t Index = 0; Index < E->Cells.size(); ++Index)
            Cells += (Index == 0 ? "" : " ") +
                     oracle::Format("%04x:%08lx", (unsigned)(unsigned short)E->Cells[Index].Ch,
                                    (unsigned long)E->Cells[Index].Attr.Packed);
        CtlEvent(oracle::Format("\"ev\":\"edit\",\"flags\":\"%x\",\"caret\":\"%d %d %d\",\"blink\":\"%d ",
                                E->Flags & 0xf, E->Caret, E->Field_0x4a0, E->Field_0x4a4, E->bCaretVisible) +
                 Bits(E->CaretTimer) + oracle::Format("\",\"attrs\":\"%08lx\",\"cells\":\"",
                                                      (unsigned long)E->CurrentAttr.Packed) +
                 Cells + "\",\"content\":\"" + RichListText(E->Content) +
                 oracle::Format("\",\"dirty\":\"%d%d\",\"modes\":\"%d%d%d %d %d %d %04x\",\"size\":\"",
                                E->bLayoutDirty, E->bSurfaceDirty, E->bEditable, E->bFixedStyle, E->bPasswordMode,
                                E->CaseMode, E->MaxLength, E->bFilterIsAllowList,
                                (unsigned)(unsigned short)E->ReplacementChar) +
                 Bits(E->NormWidth) + " " + Bits(E->NormHeight) + "\"");
    }

    void FastTableState(UiFastTable* T)
    {
        std::string Rows;
        for (size_t Index = 0; Index < T->RowEdges.size(); ++Index)
            Rows += (Index == 0 ? "" : " ") + Bits(T->RowEdges[Index]);
        std::string Columns;
        for (size_t Index = 0; Index < T->ColEdges.size(); ++Index)
            Columns += (Index == 0 ? "" : " ") + Bits(T->ColEdges[Index]);
        std::string Flags;
        for (size_t Index = 0; Index < T->Rows.size(); ++Index)
            Flags += (Index == 0 ? "" : " ") +
                     oracle::Format("%d%d%d:%u", T->Rows[Index].bHoverable, T->Rows[Index].bSelectable,
                                    T->Rows[Index].bRowLine, T->Rows[Index].GroupId);
        CtlEvent("\"ev\":\"ftable\",\"w\":\"" + Bits(T->Width) + "\",\"h\":\"" + Bits(T->Height) + "\",\"re\":\"" +
                 Rows + "\",\"ce\":\"" + Columns + "\",\"rows\":\"" + Flags +
                 oracle::Format("\",\"cells\":\"%u\",\"sel\":\"%d %u\",\"cell\":\"%u %u\",\"hov\":\"%d %u\","
                                "\"drag\":\"%d%d%d %u\",\"do\":\"",
                                (unsigned)T->Cells.size(), T->bSelected, T->SelectedRow, T->HoverRow, T->HoverColumn,
                                T->bHovered, T->HoveredRow, T->bDragArmed, T->bDragging, T->bDragColumn,
                                T->DragEdge) +
                 Bits(T->DragOffset) + "\",\"inv\":\"" + Bits12(T->InverseTransform) +
                 oracle::Format("\",\"tips\":\"%d %u\",\"hdr\":\"%u\",\"dirty\":\"%d%d\",\"size\":\"", T->TipMode,
                                (unsigned)T->Tips.size(), T->HeaderRows, T->bLayoutDirty, T->bSurfaceDirty) +
                 Bits(T->NormWidth) + " " + Bits(T->NormHeight) + "\"");
    }

    void OracleScrollCallback(void* /*Context*/, int FirstItem, unsigned VisibleRows)
    {
        CtlEvent(oracle::Format("\"ev\":\"cb\",\"first\":\"%d\",\"rows\":\"%u\"", FirstItem, VisibleRows));
    }

    bool IsRichClass(const std::string& Class)
    {
        return Class == "RichTextEditWin" || Class == "UITextEdit" || Class == "UIFastTable" ||
               Class == "UIScrollTable";
    }

    void RichResult(int Value)
    {
        CtlEvent(oracle::Format("\"ev\":\"r\",\"v\":\"%d\"", Value));
    }

    // The operations of the rich-text windows; false when the operation is not one of them.
    bool RichOp(Win3D* O, const std::string& Class, const std::vector<std::string>& T)
    {
        const std::string& Name = T[0];
        RichTextEditWin* const E = GCtlEdit;
        bool const bTable = Class == "UIFastTable" || Class == "UIScrollTable";
        UiFastTable* const Table = bTable ? static_cast<UiFastTable*>(static_cast<WinControl*>(O)) : nullptr;
        UiScrollTable* const Scroll =
            Class == "UIScrollTable" ? static_cast<UiScrollTable*>(static_cast<WinControl*>(O)) : nullptr;
        if (E != nullptr)
        {
            if (Name == "ekey" || Name == "echar" || Name == "ecancel")
            {
                memset(&GCtlRecord, 0, sizeof(GCtlRecord));
                unsigned Msg = 1;
                if (Name == "ekey")
                {
                    GCtlRecord.Key = (unsigned)I(T[1]);
                    GCtlRecord.Modifiers = (unsigned)I(T[2]);
                    GCtlRecord.Byte18 = (unsigned char)I(T[3]);
                }
                else if (Name == "echar")
                {
                    Msg = 2;
                    GCtlRecord.Arg10 = (unsigned)I(T[1]);
                }
                else
                {
                    Msg = 0xf;
                    GCtlRecord.Field0C = (unsigned)I(T[1]);
                }
                RichResult(E->OnEvent(Msg) ? 1 : 0);
            }
            else if (Name == "settext")
            {
                std::vector<wchar_t> Text = WideFromHex(T[1]);
                E->SetText(Text.data(), nullptr);
            }
            else if (Name == "settexta")
            {
                std::vector<wchar_t> Text = WideFromHex(T[1]);
                TextRenderAttributes Attr;
                Attr.Packed = strtoul(T[2].c_str(), nullptr, 16);
                E->SetText(Text.data(), &Attr);
            }
            else if (Name == "cleartext")
                E->ClearText();
            else if (Name == "gettext")
            {
                wchar_t Buffer[64];
                for (int Index = 0; Index < 64; ++Index)
                    Buffer[Index] = 0;
                int const Count = I(T[1]);
                int const Result = E->GetText(Buffer, Count);
                std::string Text;
                for (int Index = 0; Index < 64 && Buffer[Index] != 0; ++Index)
                    Text += oracle::Format("%04x", (unsigned)(unsigned short)Buffer[Index]);
                CtlEvent(oracle::Format("\"ev\":\"text\",\"r\":\"%d\",\"t\":\"", Result) + Text + "\"");
            }
            else if (Name == "caret")
                E->SetCaret(I(T[1]));
            else if (Name == "maxlen")
                E->SetMaxLength(I(T[1]));
            else if (Name == "fixed")
                E->SetFixedStyle(I(T[1]) != 0);
            else if (Name == "password")
                E->SetPasswordMode(I(T[1]) != 0);
            else if (Name == "case")
                E->SetCaseMode(I(T[1]));
            else if (Name == "allow")
                E->SetFilterIsAllowList(I(T[1]) != 0);
            else if (Name == "filter")
            {
                std::vector<wchar_t> Chars = WideFromHex(T[1]);
                E->AddFilterChars(Chars.data(), (int)Chars.size() - 1);
            }
            else if (Name == "repl")
                E->SetReplacementChar((wchar_t)strtoul(T[1].c_str(), nullptr, 16));
            else if (Name == "editable")
                E->SetEditable(I(T[1]) != 0);
            else if (Name == "activate")
                E->Activate();
            else if (Name == "deactivate")
                E->Deactivate(I(T[1]) != 0);
            else if (Name == "attrs")
                E->CurrentAttr.Packed = strtoul(T[1].c_str(), nullptr, 16);
            else if (Name == "fmask")
                E->FormatMask = (int)strtol(T[1].c_str(), nullptr, 16);
            else if (Name == "prefix")
            {
                std::vector<wchar_t> Text = WideFromHex(T[1]);
                RdlClear(E->Prefix);
                TextNode const Node(Text.data(), -1);
                RdlAppendNode(E->Prefix, Node.Clone());
                E->RebuildDisplay();
            }
            else if (Name == "fixedlist")
            {
                TextRenderAttributes Attr;
                Attr.Packed = strtoul(T[1].c_str(), nullptr, 16);
                RdlClear(E->FixedStyle);
                TRANode const Node(Attr, 0xffffffffu, 0u);
                RdlAppendNode(E->FixedStyle, Node.Clone());
                E->RebuildDisplay();
            }
            else if (Name == "state")
                EditState(E);
            else
                return false;
            return true;
        }
        if (Table == nullptr)
            return false;
        if (Name == "cedge")
            RichResult(Table->SetColumnEdge((unsigned)I(T[1]), F(T[2])) ? 1 : 0);
        else if (Name == "redge")
            RichResult(Table->SetRowEdge((unsigned)I(T[1]), F(T[2])) ? 1 : 0);
        else if (Name == "frows")
        {
            for (size_t Index = 0; Index < Table->Rows.size() && 4 + Index * 4 < T.size(); ++Index)
            {
                Table->Rows[Index].bHoverable = (unsigned char)I(T[1 + Index * 4]);
                Table->Rows[Index].bSelectable = (unsigned char)I(T[2 + Index * 4]);
                Table->Rows[Index].bRowLine = (unsigned char)I(T[3 + Index * 4]);
                Table->Rows[Index].GroupId = (unsigned)I(T[4 + Index * 4]);
            }
        }
        else if (Name == "selrow")
            RichResult(Table->SelectRow((unsigned)I(T[1])) ? 1 : 0);
        else if (Name == "clearsel")
            Table->ClearSelection();
        else if (Name == "getsel")
        {
            unsigned Row = 0xdead;
            bool const bResult = Table->GetSelectedRow(&Row);
            CtlEvent(oracle::Format("\"ev\":\"r\",\"v\":\"%d %u\"", bResult ? 1 : 0, bResult ? Row : 0u));
        }
        else if (Name == "hdr")
            Table->SetHeaderRows((unsigned)I(T[1]));
        else if (Name == "borders")
            Table->SetBorders(I(T[1]) != 0, I(T[2]) != 0, I(T[3]) != 0, I(T[4]) != 0);
        else if (Name == "collines")
            Table->SetDrawColumnLines(I(T[1]) != 0);
        else if (Name == "highlights")
            Table->SetRowHighlights(I(T[1]) != 0);
        else if (Name == "resizable")
            Table->SetResizable(I(T[1]) != 0, I(T[2]) != 0);
        else if (Name == "grab")
            Table->SetGrabDistance(F(T[1]));
        else if (Name == "margins")
            Table->SetCellMargins(I(T[1]), I(T[2]));
        else if (Name == "tipmode")
            Table->SetTipMode(I(T[1]));
        else if (Name == "wtip")
            Table->HoverInfo.IdsAlt = I(T[1]);
        else if (Name == "coltip")
            Table->Tips[(unsigned)I(T[1])].Info.IdsAlt = I(T[2]);
        else if (Name == "rtip")
            Table->GetRowTip((unsigned)I(T[1]))->IdsAlt = I(T[2]);
        else if (Name == "ctip")
            Table->GetCellTip((unsigned)I(T[1]), (unsigned)I(T[2]))->IdsAlt = I(T[3]);
        else if (Name == "tooltip")
            Table->SetToolTipEnabled(I(T[1]) != 0);
        else if (Name == "clearcell")
            RichResult(Table->ClearCell((unsigned)I(T[1]), (unsigned)I(T[2])) ? 1 : 0);
        else if (Name == "span")
            Table->SetCellSpanToEnd((unsigned)I(T[1]), (unsigned)I(T[2]), I(T[3]) != 0);
        else if (Name == "invalidate")
            Table->InvalidateCell((unsigned)I(T[1]), (unsigned)I(T[2]));
        else if (Name == "setedges")
        {
            FUiFastTableEdges Edges;
            size_t At = 1;
            int const RowCount = I(T[At++]);
            for (int Index = 0; Index < RowCount; ++Index)
                Edges.Rows.push_back(F(T[At++]));
            int const ColCount = I(T[At++]);
            for (int Index = 0; Index < ColCount; ++Index)
                Edges.Cols.push_back(F(T[At++]));
            Table->SetEdges(&Edges);
        }
        else if (Scroll != nullptr && Name == "items")
            Scroll->SetItemCount((unsigned)I(T[1]), (unsigned)I(T[2]));
        else if (Scroll != nullptr && Name == "first")
            RichResult(Scroll->GetFirstItem());
        else if (Scroll != nullptr && Name == "callback")
            Scroll->SetCallback(OracleScrollCallback, nullptr);
        else if (Scroll != nullptr && Name == "barvalue")
            Scroll->ScrollBar->SetValue(F(T[1]));
        else if (Scroll != nullptr && Name == "scrolled")
            RichResult(Scroll->Slot23(0xf005u, static_cast<unsigned>(reinterpret_cast<uintptr_t>(Scroll->ScrollBar)),
                                      0u) ? 1 : 0);
        else if (Scroll != nullptr && Name == "gap")
            Scroll->SetScrollBarGap(F(T[1]));
        else if (Scroll != nullptr && Name == "bstate")
        {
            // The bar's flags as the other states print them: the low four bits.
            unsigned char const Saved = Scroll->ScrollBar->Flags;
            Scroll->ScrollBar->Flags = static_cast<unsigned char>(Saved & 0xf);
            CtlEvent(FUiScrollPeek::Of(Scroll->ScrollBar)->State());
            Scroll->ScrollBar->Flags = Saved;
        }
        else if (Name == "state")
        {
            FastTableState(Table);
            if (Scroll != nullptr)
                CtlEvent(oracle::Format("\"ev\":\"stable\",\"items\":\"%u\",\"fixed\":\"%u\"", Scroll->ItemCount,
                                        Scroll->FixedRows));
        }
        else
            return false;
        return true;
    }

    void RunCase(const std::string& Line, FOracleCtlParent* Parent, std::string& Class)
    {
        for (const std::string& Op : Split(Line, " ; "))
        {
            std::vector<std::string> T = Split(Op, " ");
            const std::string& Name = T[0];
            Win3D* const O = GCtlObject;
            ListBox* const List = (O != nullptr && (Class == "UIListBox")) ? static_cast<ListBox*>(O) : nullptr;
            UiScroll* const Scroll = (O != nullptr && (Class == "UIScroll" || Class == "UICycle"))
                                         ? static_cast<UiScroll*>(static_cast<WinControl*>(O))
                                         : nullptr;
            if (Name == "new")
            {
                Class = T[1];
                Win3D* Created = nullptr;
                if (Class == "UIListBox")
                {
                    FUiPayloadBounds P = {F(T[2]), F(T[3])};
                    Created = WinFactoryCreate("C", Class.c_str(), Parent, &P);
                }
                else if (Class == "UIScroll" || Class == "UICycle")
                {
                    FUiPayloadScrollCycle P;
                    memset(&P, 0, sizeof(P));
                    P.Right = F(T[2]);
                    P.Bottom = F(T[3]);
                    P.Extra0 = F(T[4]);
                    P.Extra1 = F(T[5]);
                    P.Extra2 = F(T[6]);
                    P.Extra3 = F(T[7]);
                    P.Flag = (unsigned char)I(T[8]);
                    Created = WinFactoryCreate("C", Class.c_str(), Parent, &P);
                }
                else if (Class == "UITable")
                {
                    FUiPayloadTable P = {F(T[2]), F(T[3]), I(T[4]), I(T[5])};
                    Created = WinFactoryCreate("C", Class.c_str(), Parent, &P);
                }
                else if (IsLaterClass(Class) || Class == "RichTextEditWin")
                    Created = WinFactoryCreate("C", Class.c_str(), Parent, nullptr);
                else if (Class == "UITextEdit")
                {
                    std::vector<wchar_t> Caption = WideFromHex(T.size() > 4 ? T[4] : "");
                    FUiPayloadText P = {F(T[2]), F(T[3]), Caption.data()};
                    Created = WinFactoryCreate("C", Class.c_str(), Parent, &P);
                }
                else if (Class == "UIFastTable" || Class == "UIScrollTable")
                {
                    FUiPayloadTable P = {F(T[2]), F(T[3]), I(T[4]), I(T[5])};
                    Created = WinFactoryCreate("C", Class.c_str(), Parent, &P);
                }
                else if (Class == "UIWinObject")
                {
                    std::string Path = BytesFromHex(T.size() > 5 ? T[5] : "");
                    std::vector<char> Buffer(Path.begin(), Path.end());
                    Buffer.push_back(0);
                    FUiPayloadWinObject P = {F(T[2]), F(T[3]), Buffer.data(), F(T[4])};
                    Created = WinFactoryCreate("C", Class.c_str(), Parent, &P);
                }
                GCtlObject = Created;
                GCtlEdit = (Created != nullptr && (Class == "RichTextEditWin" || Class == "UITextEdit"))
                               ? static_cast<RichTextEditWin*>(static_cast<WinControl*>(Created))
                               : nullptr;
                CtlEvent(oracle::Format("\"ev\":\"new\",\"ok\":\"%d\"", Created != nullptr ? 1 : 0));
            }
            else if (O == nullptr)
                continue;
            else if (IsLaterClass(Class) && LaterOp(O, Class, T))
                continue;
            else if (IsRichClass(Class) && RichOp(O, Class, T))
                continue;
            else if (Name == "xform")
            {
                float M[12];
                for (int Index = 0; Index < 12; ++Index)
                    M[Index] = F(T[1 + Index]);
                O->SetTransform(M);
            }
            else if (Name == "pos")
            {
                float P[3] = {F(T[1]), F(T[2]), F(T[3])};
                O->SetPosition(P);
            }
            else if (Name == "ray")
            {
                float* const Ray = Win3DMouseRayStorage();
                Ray[0] = F(T[1]);
                Ray[1] = F(T[2]);
                Ray[2] = F(T[3]);
            }
            else if (Name == "cursor")
            {
                GCtlCursor[0] = I(T[1]);
                GCtlCursor[1] = I(T[2]);
            }
            else if (Name == "dt")
                GCtlFrameTime = F(T[1]);
            else if (Name == "pulse")
                NnUiPulseAdvance(F(T[1]));
            else if (Name == "opt")
                GCtlOption = I(T[1]);
            else if (Name == "update")
                O->Update();
            else if (Name == "draw")
                O->Draw();
            else if (Name == "over")
            {
                float Ray[3];
                Win3DGetMouseRay(Ray);
                O->MouseOver(Ray);
            }
            else if (Name == "leave")
            {
                float Ray[3];
                Win3DGetMouseRay(Ray);
                O->MouseLeave(Ray);
            }
            else if (Name == "btn")
            {
                FWin3DMouseEvent Event = {I(T[1]), I(T[2])};
                bool const bResult = O->MouseEvent(&Event);
                CtlEvent(oracle::Format("\"ev\":\"btn\",\"r\":\"%d\"", bResult ? 1 : 0));
            }
            else if (Name == "hit")
            {
                float Ray[3];
                Win3DGetMouseRay(Ray);
                bool const bResult = O->HitTest(Ray);
                CtlEvent(oracle::Format("\"ev\":\"hit\",\"r\":\"%d\"", bResult ? 1 : 0));
            }
            else if (Name == "state")
            {
                if (List != nullptr)
                    ListState(List);
                else if (Scroll != nullptr)
                    CtlEvent(FUiScrollPeek::Of(O)->State());
                else
                    ObjectState(O);
            }
            else if (List != nullptr && Name == "item")
            {
                std::vector<wchar_t> Text = WideFromHex(T[1]);
                int const Result = List->AddItem(Text.data(), I(T[2]));
                CtlEvent(oracle::Format("\"ev\":\"r\",\"v\":\"%d\"", Result));
            }
            else if (List != nullptr && Name == "settext")
            {
                std::vector<wchar_t> Text = WideFromHex(T[2]);
                bool const bResult = List->SetItemText(I(T[1]), Text.data());
                CtlEvent(oracle::Format("\"ev\":\"r\",\"v\":\"%d\"", bResult ? 1 : 0));
            }
            else if (List != nullptr && Name == "data")
                CtlEvent(oracle::Format("\"ev\":\"r\",\"v\":\"%d\"", List->GetItemData(I(T[1]))));
            else if (List != nullptr && Name == "f80")
                List->Field80 = I(T[1]);
            else if (List != nullptr && Name == "frame")
                List->bDrawFrame = (unsigned char)I(T[1]);
            else if (List != nullptr && Name == "rules")
                List->bDrawRowRules = (unsigned char)I(T[1]);
            else if (List != nullptr && Name == "rows")
                List->VisibleRows = I(T[1]);
            else if (List != nullptr && Name == "first")
                List->FirstVisible = I(T[1]);
            else if (List != nullptr && Name == "sel")
                List->bSelectable = (unsigned char)I(T[1]);
            else if (Scroll != nullptr && Name == "range")
                Scroll->SetRange(F(T[1]), F(T[2]));
            else if (Scroll != nullptr && Name == "value")
                Scroll->SetValue(F(T[1]));
            else if (Scroll != nullptr && Name == "page")
                Scroll->SetPage(F(T[1]));
            else if (Scroll != nullptr && Name == "step")
                Scroll->SetStep(F(T[1]));
            else if (Scroll != nullptr && Name == "size")
                Scroll->SetSize(F(T[1]), F(T[2]));
            else if (Scroll != nullptr && Name == "stepmode")
                Scroll->SetStepMode(I(T[1]) != 0);
            else if (Scroll != nullptr && Name == "horiz")
                Scroll->SetHorizontal(I(T[1]) != 0);
            else if (Scroll != nullptr && Name == "custom")
                FUiScrollPeek::Of(O)->SetCustom(I(T[1]) != 0);
            else if (Scroll != nullptr && Name == "active")
                FUiScrollPeek::Of(O)->SetActive(I(T[1]) != 0);
            else if (Scroll != nullptr && Name == "soff")
            {
                float V[3] = {F(T[1]), F(T[2]), F(T[3])};
                Scroll->SetScrollOffset(V);
            }
            else if (Scroll != nullptr && Name == "offset")
            {
                float V[3] = {F(T[1]), F(T[2]), F(T[3])};
                intptr_t const Result = Scroll->SetProperty(6, reinterpret_cast<uintptr_t>(V), 0);
                CtlEvent(oracle::Format("\"ev\":\"r\",\"v\":\"%d\"", (int)Result));
            }
            else if (Scroll != nullptr && Name == "prop")
            {
                intptr_t const Result = Scroll->SetProperty(
                    (int)strtol(T[1].c_str(), nullptr, 16), (uintptr_t)(intptr_t)I(T[2]), (uintptr_t)(intptr_t)I(T[3]));
                CtlEvent(oracle::Format("\"ev\":\"r\",\"v\":\"%d\"", (int)Result));
            }
            else if (Name == "flags")
                static_cast<WinObject*>(O)->DrawFlags = (unsigned char)I(T[1]);
            else if (Name == "tint")
            {
                unsigned long const Packed = strtoul(T[1].c_str(), nullptr, 16);
                WinObject* const W = static_cast<WinObject*>(O);
                W->TintRgba[0] = (unsigned char)(Packed >> 24);
                W->TintRgba[1] = (unsigned char)(Packed >> 16);
                W->TintRgba[2] = (unsigned char)(Packed >> 8);
                W->TintRgba[3] = (unsigned char)Packed;
            }
        }
    }

    void ControlsSuite()
    {
        oracle::Suite Out("ui_controls.jsonl");
        GCtlOut = &Out;
        FOracleCtlPipeline Pipeline;
        IRenderPipeline* const SavedPipeline = NeuronRuntime::Pipeline;
        NeuronRuntime::Pipeline = &Pipeline;
        FOracleCtlEngine Engine;
        IEngine* const SavedEngine = NeuronRuntime::Engine;
        NeuronRuntime::Engine = &Engine;
        GCtlCase = 0;
        for (const std::string& Line : oracle::InputLines("ui_controls.list"))
        {
            GCtlObject = nullptr;
            GCtlFrameTime = 0.0;
            GCtlCursor[0] = 0;
            GCtlCursor[1] = 0;
            GCtlOption = 1;
            for (int Index = 0; Index < 0x20; ++Index)
                GCtlOptions[Index] = 1;
            GCtlElapsed = 0.0;
            GCtlInstances = 0;
            G_TextControlTypingCount = 0;
            GCtlEdit = nullptr;
            GCtlInputFocus = nullptr;
            GCtlFocusStack.clear();
            NnUiPulseReset();
            float* const Ray = Win3DMouseRayStorage();
            Ray[0] = 0.0f;
            Ray[1] = 0.0f;
            Ray[2] = -1.0f;
            FOracleCtlParent* const Parent = new FOracleCtlParent();
            Parent->Create(nullptr, nullptr);
            std::string Class;
            RunCase(Line, Parent, Class);
            GCtlOut = nullptr;
            while (Win3DGetCapture() != nullptr)
                Win3DRemoveCapture(Win3DGetCapture());
            if (GCtlObject != nullptr)
            {
                GCtlObject->Destroy();
                delete GCtlObject;
            }
            GCtlObject = nullptr;
            GCtlEdit = nullptr;
            GCtlInputFocus = nullptr;
            GCtlFocusStack.clear();
            Parent->Destroy();
            delete Parent;
            GCtlOut = &Out;
            ++GCtlCase;
        }
        GCtlOut = nullptr;
        NeuronRuntime::Pipeline = SavedPipeline;
        NeuronRuntime::Engine = SavedEngine;
    }
}

// ---- the recording stand-ins of the immediate-mode layer (win3d_immediate.cpp, star_sphere.cpp)

void Win3DBegin(int PrimitiveMode)
{
    CtlEvent(oracle::Format("\"ev\":\"begin\",\"p\":\"%d\"", PrimitiveMode));
}
void Win3DEnd()
{
    CtlEvent("\"ev\":\"end\"");
}
void Win3DVertex3f(float X, float Y, float Z)
{
    CtlEvent("\"ev\":\"v\",\"x\":\"" + Bits(X) + "\",\"y\":\"" + Bits(Y) + "\",\"z\":\"" + Bits(Z) + "\"");
}
void Win3DColor3ub(unsigned char R, unsigned char G, unsigned char B)
{
    CtlEvent(oracle::Format("\"ev\":\"c3\",\"c\":\"%02x%02x%02x\"", R, G, B));
}
void Win3DColor4ubv(const unsigned char* Rgba4)
{
    CtlEvent("\"ev\":\"c4\",\"c\":\"" + Rgba(Rgba4) + "\"");
}
void Win3DTexCoord2f(float U, float V)
{
    CtlEvent("\"ev\":\"tc\",\"u\":\"" + Bits(U) + "\",\"v\":\"" + Bits(V) + "\"");
}
void Win3DDisable(int Capability)
{
    CtlEvent(oracle::Format("\"ev\":\"dis\",\"cap\":\"%x\"", Capability));
}
void Win3DBindTexture(void* TextureHandle)
{
    CtlEvent(oracle::Format("\"ev\":\"tex\",\"h\":\"%lx\"", (unsigned long)(uintptr_t)TextureHandle));
}
void Win3DPushProjection()
{
    CtlEvent("\"ev\":\"push\"");
}
void Win3DPopProjection()
{
    CtlEvent("\"ev\":\"pop\"");
}
void Win3DSetDepthWrite(int bEnable)
{
    CtlEvent(oracle::Format("\"ev\":\"dw\",\"v\":\"%d\"", bEnable));
}

void Win3DSetOrtho2D()
{
    CtlEvent("\"ev\":\"ortho\"");
}
void Win3DBindModelview(void* /*ObjectOrNull*/, float const* Matrix12)
{
    CtlEvent("\"ev\":\"mv\",\"m\":\"" + BitsN(Matrix12, 12) + "\"");
}

// ---- the font strings (font_string.cpp, font_imp.cpp, font_atlas.cpp): the list's rows store
// their text and record their paints; the "ListText" font has a 16-pixel line and 7-pixel
// characters.

namespace
{
    class FOracleCtlFont final : public IFont
    {
    public:
        char const* Slot00() override { return "ListText"; }
        char const* GetName() override { return "ListText"; }
        float GetFixedHeight() override { return 16.0f; }
        HFONT__* Slot03() override { return nullptr; }
        tagLOGFONTA const* GetLogFont() override { return nullptr; }
        float GetGlyphAdvance(wchar_t /*Ch*/) override { return 7.0f; }
        int GetLineHeight() override { return 16; }
        int Slot07() override { return 16; }
        void MeasureString(int* OutWH, wchar_t const* Text, int Len) override
        {
            int Count = 0;
            if (Text != nullptr)
                for (; (Len < 0 || Count < Len) && Text[Count] != 0; ++Count)
                {
                }
            OutWH[0] = 7 * Count;
            OutWH[1] = 16;
        }
        int ProbeString(int MaxWidth, wchar_t const* Text, int Len) override { return 0; }
        int GetCharWidths(wchar_t const* Text, int Len, int* OutWidths) override { return 0; }
    };

    FOracleCtlFont GCtlFont;
}

IFont* FindMenuFontImp(char const* /*Nickname*/)
{
    return &GCtlFont;
}

void FontAtlasSetDrawColor(unsigned PackedArgb)
{
    CtlEvent(oracle::Format("\"ev\":\"color\",\"c\":\"%08x\"", PackedArgb));
}

FontStringChild::FontStringChild()
    : Text_(nullptr)
    , Capacity_(0)
    , Font_(nullptr)
    , RefCount_(1)
    , GlyphHead_(nullptr)
    , CachedW_(0)
    , CachedH_(0)
    , LayoutGen_(0)
    , Color_(0xff080808u)
    , bShadow_(0)
{
}
FontStringChild::~FontStringChild()
{
    free(Text_);
}
int FontStringChild::AddRef()
{
    return ++RefCount_;
}
int FontStringChild::Release()
{
    int const Count = --RefCount_;
    if (Count == 0)
        delete this;
    return Count;
}
void FontStringChild::BindFont(IFont* Font)
{
    Font_ = Font;
}
IFont* FontStringChild::GetFont() const
{
    return Font_;
}
wchar_t const* FontStringChild::GetText() const
{
    return Text_;
}
void FontStringChild::SetText(wchar_t const* Text)
{
    // font_string.cpp's early outs: the same text (a null read as empty) changes nothing, so a
    // new row given an empty text keeps a null text; null empties the text.
    wchar_t const* const Left = Text != nullptr ? Text : L"";
    wchar_t const* const Right = Text_ != nullptr ? Text_ : L"";
    size_t Same = 0;
    while (Left[Same] != 0 && Left[Same] == Right[Same])
        ++Same;
    if (Left[Same] == Right[Same])
        return;
    if (Text == nullptr)
    {
        Text_[0] = 0;
        return;
    }
    free(Text_);
    Text_ = nullptr;
    size_t Length = 0;
    while (Text[Length] != 0)
        ++Length;
    Text_ = (wchar_t*)malloc((Length + 1) * sizeof(wchar_t));
    memcpy(Text_, Text, (Length + 1) * sizeof(wchar_t));
}
int* FontStringChild::GetSize(int* OutWH)
{
    OutWH[0] = 0;
    OutWH[1] = 0;
    return OutWH;
}
void FontStringChild::SetColor(unsigned const* Argb)
{
    Color_ = *Argb;
}
void FontStringChild::SetShadow(unsigned char Value)
{
    bShadow_ = Value;
}
void FontStringChild::Paint(void* Origin, void* DrawCtx, void* DrawArg)
{
    float const* const O = static_cast<float const*>(Origin);
    float Left = 0.0f;
    float Right = 0.0f;
    memcpy(&Left, &DrawCtx, sizeof(Left));
    memcpy(&Right, &DrawArg, sizeof(Right));
    CtlEvent("\"ev\":\"paint\",\"t\":\"" + Units(Text_) + "\",\"x\":\"" + Bits(O[0]) + "\",\"y\":\"" + Bits(O[1]) +
             "\",\"l\":\"" + Bits(Left) + "\",\"r\":\"" + Bits(Right) + "\"");
}

// ---- the screen projection of text objects (text_win.cpp): a fixed mapping in place of the
// Win3D camera -- x = 400 X + 320, y = 240 - 400 Y, extents 400 times the world's.

void TextWinProjectPoint(float const* World, float* OutX, float* OutY)
{
    *OutX = static_cast<float>(static_cast<double>(World[0]) * 400.0 + 320.0);
    *OutY = static_cast<float>(240.0 - static_cast<double>(World[1]) * 400.0);
}

void TextWinProjectExtent(float Width, float Height, float /*Z*/, float* OutWidth, float* OutHeight)
{
    *OutWidth = static_cast<float>(static_cast<double>(Width) * 400.0);
    *OutHeight = static_cast<float>(static_cast<double>(Height) * 400.0);
}

// ---- the frame, the cursor and the options (globals.cpp, input_manager.cpp, game_options.cpp)

double GetClientFrameTimeCapped()
{
    return GCtlFrameTime;
}

void GetClientSoftwareCursor(int* OutX, int* OutY, int* OutZ)
{
    *OutX = GCtlCursor[0];
    *OutY = GCtlCursor[1];
    if (OutZ != nullptr)
        *OutZ = 0;
}

int GetGameOption(int Index)
{
    if (Index == 0x16)
        return GCtlOption;
    return (Index >= 0 && Index < 0x20) ? GCtlOptions[Index] : 1;
}

double GetClientElapsedTime()
{
    return GCtlElapsed;
}

// ---- the scroll bars' shapes (texture_shapes.cpp): the three names of UITextures.ini, whose
// GPU handles are 0x101 (the arrow), 0x102 (the caps) and 0x103 (the middle).

namespace
{
    FTexShape GCtlShapes[3];
}

FTexShape* FShapeDB::Find(char const* Name, char const* /*SourceFile*/)
{
    if (strcmp(Name, "HUD_sliderarrow") == 0)
        return &GCtlShapes[0];
    if (strcmp(Name, "HUD_slidertextopbottom") == 0)
        return &GCtlShapes[1];
    if (strcmp(Name, "HUD_slidertexmid") == 0)
        return &GCtlShapes[2];
    return nullptr;
}

void* TexShapeGetGpuHandle(FTextureRef const* Ref, int /*Frame*/)
{
    for (int Index = 0; Index < 3; ++Index)
        if (Ref == &GCtlShapes[Index])
            return reinterpret_cast<void*>(static_cast<uintptr_t>(0x101 + Index));
    return nullptr;
}

// ---- the mesh holder (client_mesh_load.cpp) and the engine submits (win3d_engine_submit.cpp)

bool ClientMeshLoadFromPath(FClientMeshHolder* Holder, char const* Path, char const* MaterialPath, bool)
{
    CtlEvent("\"ev\":\"load\",\"path\":\"" + oracle::HexC(Path) + "\",\"mat\":\"" + oracle::HexC(MaterialPath) +
             "\"");
    int const Index = GCtlInstances < 7 ? GCtlInstances++ : 7;
    Holder->EngineInstance = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1000 + Index));
    float const Identity[12] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
    memcpy(GCtlXforms[Index], Identity, sizeof(Identity));
    return true;
}

bool ClientMeshHolderHasInstance(FMeshHolder const* Holder)
{
    return Holder->EngineInstance != nullptr &&
           Holder->EngineInstance != reinterpret_cast<void*>(static_cast<ptrdiff_t>(-1));
}

void ClientMeshHolderSetPosition(FMeshHolder* Holder, float const Position[3])
{
    MeshPos(InstanceIndex(Holder->EngineInstance), Position);
}

void ClientMeshHolderUpdate(FMeshHolder* Holder)
{
    CtlEvent(MeshEv("mupd", InstanceIndex(Holder->EngineInstance)));
}

float* ClientMeshHolderGetPosition(FMeshHolder const* Holder)
{
    return &GCtlXforms[InstanceIndex(Holder->EngineInstance)][9];
}

void ClientMeshHolderSetColor(FClientMeshHolder* Holder, unsigned char const* Rgba)
{
    float const Inv255 = 1.0f / 255.0f;
    Holder->UserFloats[0] = static_cast<float>(static_cast<double>(Rgba[0]) * Inv255);
    Holder->UserFloats[1] = static_cast<float>(static_cast<double>(Rgba[1]) * Inv255);
    Holder->UserFloats[2] = static_cast<float>(static_cast<double>(Rgba[2]) * Inv255);
    Holder->UserFloats[4] = static_cast<float>(static_cast<double>(Rgba[3]) * Inv255);
}

void SubmitWin3DEngineInstance(FClientMeshHolder* Holder, void* /*ArchAnchor*/, unsigned char bFrontCull,
                               float Scale, unsigned long /*Flags*/)
{
    CtlEvent("\"ev\":\"submit\"");
    float const* const U = Holder->UserFloats;
    if (bFrontCull != 0 || Scale != 1.0f || U[0] != 1.0f || U[1] != 1.0f || U[2] != 1.0f || U[4] != 1.0f)
    {
        float const Tint[4] = {U[0], U[1], U[2], U[4]};
        CtlEvent("\"ev\":\"body\",\"t\":\"" + BitsN(Tint, 4) + oracle::Format("\",\"cull\":\"%d\",\"s\":\"", bFrontCull) +
                 Bits(Scale) + "\"");
    }
}

void SubmitWin3DTintedEngineInstanceWrapped(void* /*ArchAnchor*/, void* /*EngineInstance*/,
                                            unsigned char bFrontCull, unsigned char const ColorRgba[4],
                                            unsigned char bLightingEnable, float Scale,
                                            unsigned long /*Flags*/)
{
    CtlEvent("\"ev\":\"tinted\",\"tint\":\"" + Rgba(ColorRgba) + oracle::Format("\",\"l\":\"%d\"", bLightingEnable));
    if (bFrontCull != 1 || Scale != 1.0f)
        CtlEvent(oracle::Format("\"ev\":\"tscale\",\"cull\":\"%d\",\"s\":\"", bFrontCull) + Bits(Scale) + "\"");
}

void SubmitWin3DOutlinedEngineInstance(void* /*Camera*/, void* /*EngineInstance*/, unsigned char /*bFrontCull*/,
                                       unsigned char const* WireRgba, float Scale, unsigned long /*Flags*/)
{
    CtlEvent("\"ev\":\"outlined\",\"w\":\"" + Rgba(WireRgba) + "\",\"s\":\"" + Bits(Scale) + "\"");
}

// ---- the math of the UI_PAGE widgets (ui_page.cpp) and of the HUD (hud_target.cpp), as the
// reference writes them, over Common's math engine.

I3DMathEngine* UiWidgetMathEngine()
{
    static I3DMathEngine* Math = nullptr;
    if (Math == nullptr)
        Math = Acquire3DMathEngine();
    return Math;
}

void UiWidgetProjectExtent(float const Matrix[9], float Z, float Width, float Height, float* OutWidth,
                           float* OutHeight)
{
    FMatrix const& M = *reinterpret_cast<FMatrix const*>(Matrix);
    FVector Source;
    FVector WidthPoint;
    FVector Origin;
    Source.X = Width;
    Source.Y = 0.0f;
    Source.Z = Z;
    UiWidgetMathEngine()->TransformMatrix(WidthPoint, M, Source);
    Source.X = 0.0f;
    Source.Y = 0.0f;
    Source.Z = Z;
    UiWidgetMathEngine()->TransformMatrix(Origin, M, Source);
    *OutWidth = fabsf(WidthPoint.X - Origin.X);
    FVector HeightPoint;
    Source.X = 0.0f;
    Source.Y = Height;
    Source.Z = Z;
    UiWidgetMathEngine()->TransformMatrix(HeightPoint, M, Source);
    Source.X = 0.0f;
    Source.Y = 0.0f;
    Source.Z = Z;
    UiWidgetMathEngine()->TransformMatrix(Origin, M, Source);
    *OutHeight = fabsf(HeightPoint.Y - Origin.Y);
}

FVector* HudTransformPoint(FTransform const* Xform, FVector* Out, FVector const* In)
{
    FVector Result;
    UiWidgetMathEngine()->TransformXform(Result, *Xform, *In);
    *Out = Result;
    return Out;
}

// ---- the other client functions the compiled files name: the Win3D manager's startup, frame
// and draw services, WinControl's INI hook and tooltip feed, the mesh holder's teardown and the
// shape database. No case reaches them.

namespace Archetype
{
    FCliMaterialEntry* ResolveMaterialEntryFromFs(IFileSystem*, char const*, bool, unsigned)
    {
        return nullptr;
    }
    int ReleaseMaterialEntrySlot(FCliMaterialEntry**)
    {
        return 0;
    }
}

void ButtonMontageEnsure()
{
}
void ButtonMontageFlushBatch()
{
}
void ClientMeshHolderConstruct(FClientMeshHolder*)
{
}
void ClientUiMeshHolderRelease(FClientMeshHolder* Holder)
{
    Holder->EngineInstance = reinterpret_cast<void*>(static_cast<ptrdiff_t>(-1));
}
FMeshHolder::~FMeshHolder()
{
}
FClientMeshHolder::~FClientMeshHolder()
{
}
// The static control's draw clears the engine lights (engine_light.cpp): recorded.
void EngineLightClearRegisteredList()
{
    CtlEvent("\"ev\":\"lclear\"");
}
void FontStringConstruct(FFontString* Self)
{
    Self->Text = nullptr;
    Self->Capacity = 0;
    Self->Field08 = 0x7fff;
    Self->Field0C = 0;
    Self->ChildBegin = nullptr;
    Self->ChildEnd = nullptr;
    Self->ChildCap = nullptr;
    Self->FontIface = nullptr;
    Self->ExtentW = 0;
    Self->ExtentH = 0;
    Self->LayoutGen = 0;
    Self->ScaleKey = 0x7fff;
    Self->bMultiLine = 0;
}
void FontStringDestroy(FFontString* Self)
{
    free(Self->Text);
    Self->Text = nullptr;
}
unsigned char GetClientMouseCursorDrawFlag()
{
    return 0;
}
int GetClientMouseWheel()
{
    return 0;
}
// The Win3D camera of a 640 x 480 surface, as the manager sets it up (the field values the
// Rust camera gives): the orientation, the screen centre, the scales, the planes, the viewport.
// The controls read its fields and project through it.
FSpaceCamera* GetWin3DSceneCamera()
{
    alignas(FSpaceCamera) static unsigned char Storage[sizeof(FSpaceCamera)];
    static FSpaceCamera* const Cam = new (Storage) FSpaceCamera();
    float const Identity[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    memcpy(Cam->World, Identity, sizeof(Identity));
    Cam->ScreenCenterX = 320.0f;
    Cam->ScreenCenterY = 240.0f;
    Cam->HalfWidth = 320.0f;
    Cam->HalfHeight = 240.0f;
    Cam->XScale = oracle::FloatFromBits(0x441b8e6au);
    Cam->YScale = oracle::FloatFromBits(0xc41b8e6au);
    Cam->ZNear = oracle::FloatFromBits(0x3dcccccdu);
    Cam->ZFar = 1000.0f;
    Cam->Viewport[0] = 0;
    Cam->Viewport[1] = 0;
    Cam->Viewport[2] = 639;
    Cam->Viewport[3] = 479;
    return Cam;
}
FHudCmdRecord* HudCommandRecord()
{
    return &GCtlRecord;
}
// The input focus as hud_command_dispatch.cpp keeps it (the filter and the stack of the ones it
// replaced); the edit box's pushes and pops are recorded.
void HudPushInputFocus(FHudCmdHandler* Handler)
{
    if (GCtlEdit != nullptr && Handler == GCtlEdit->AsHudCmdHandler())
        FocusEvent("push", Handler);
    if (GCtlInputFocus != nullptr)
        GCtlFocusStack.push_back(GCtlInputFocus);
    GCtlInputFocus = Handler;
}
void HudPopInputFocus(FHudCmdHandler* Handler)
{
    if (GCtlEdit != nullptr && Handler == GCtlEdit->AsHudCmdHandler())
        FocusEvent("pop", Handler);
    if (GCtlInputFocus == Handler)
    {
        if (GCtlFocusStack.empty())
        {
            GCtlInputFocus = nullptr;
            return;
        }
        GCtlInputFocus = GCtlFocusStack.back();
        GCtlFocusStack.pop_back();
        return;
    }
    for (size_t Index = 0; Index < GCtlFocusStack.size(); ++Index)
    {
        if (GCtlFocusStack[Index] == Handler)
        {
            GCtlFocusStack.erase(GCtlFocusStack.begin() + (long)Index);
            break;
        }
    }
}
FHudCmdHandler* HudGetInputFocus()
{
    return GCtlInputFocus;
}
void HudUnregisterCommand(FHudCmdHandler* Handler, unsigned MsgId)
{
    if (GCtlEdit != nullptr && Handler == GCtlEdit->AsHudCmdHandler())
        CtlEvent(oracle::Format("\"ev\":\"evunreg\",\"m\":\"%x\"", MsgId));
}
void MouseSetCursor(char const* Name, bool)
{
    CtlEvent(std::string("\"ev\":\"mcursor\",\"n\":\"") + Name + "\"");
}
void HudDestroyEventHandler(FHudCmdHandler*)
{
}
void HudRegisterCommandHandler(FHudCmdHandler* Handler, unsigned MsgId)
{
    if (GCtlEdit != nullptr && Handler == GCtlEdit->AsHudCmdHandler())
        CtlEvent(oracle::Format("\"ev\":\"evreg\",\"m\":\"%x\"", MsgId));
}
void HudUnregisterCommandHandler(FHudCmdHandler*)
{
}
void ImeFocusDrawPass()
{
}
void MouseFrameUpdate()
{
}
void MouseInitCursors()
{
}
void MouseShutdown()
{
}
void NnRolloverTickGlobal(double)
{
}
void NnToolTipFeedHover(FToolTipHoverInfo const* Info)
{
    std::string Key;
    if (GCtlObject != nullptr && (Info->Ids == static_cast<int>(reinterpret_cast<intptr_t>(GCtlObject)) ||
                                  Info->Ids == static_cast<int>(reinterpret_cast<intptr_t>(GCtlObject->AsWinControl()))))
        Key = "self";
    else if (GCtlObject != nullptr &&
             static_cast<unsigned>(Info->Ids - static_cast<int>(reinterpret_cast<intptr_t>(GCtlObject))) - 1u <
                 0x10000u)
        // A fast table's own records: the table plus one plus their index.
        Key = oracle::Format("self+%d", Info->Ids - static_cast<int>(reinterpret_cast<intptr_t>(GCtlObject)));
    else
        Key = oracle::Format("%d", Info->Ids);
    CtlEvent("\"ev\":\"hover\",\"k\":\"" + Key +
             oracle::Format("\",\"a\":\"%d\",\"x\":\"%d\",\"p\":\"", Info->IdsAlt, Info->AuxField) +
             Bits3(Info->WorldPos) + "\",\"e\":\"" + Bits3(Info->HalfExtents) +
             oracle::Format("\",\"l\":\"%d\"", Info->bLive));
}
void NnToolTipTickGlobal(double)
{
}
void OracleOnWinControlIni(char const*, Win3D*)
{
}
void SpaceCameraSetHalfFovX(FSpaceCamera*, float)
{
}
void SpaceCameraSetNearFar(FSpaceCamera*, float, float)
{
}
bool SpaceCameraSetViewport(FSpaceCamera*, const int*)
{
    return false;
}
// The render-to-window camera's rectangle (space_camera.cpp): kept, for the record below.
void SpaceCameraSetViewportEx(FSpaceCamera* Cam, const int* ViewRect4, int)
{
    for (int I = 0; I < 4; ++I)
        Cam->Viewport[I] = ViewRect4[I];
}
void ToolTipHoverDestroy(FToolTipHoverInfo*)
{
}
void UiOverlayPrepPipeline()
{
}
void Win3DApplyCamera(ICamera*)
{
}
void Win3DImmedBatchCreateAll()
{
}
void Win3DImmedBatchDestroyAll()
{
}
void Win3DImmedBatchFlushAll()
{
}
void Win3DImmedBatchReset()
{
}
void Win3DModelCacheClear()
{
}
// The reference's body (win3d_engine_submit.cpp), over x86math's engine.
namespace
{
    bool CtlSlabHitStrict(float OriginA, float OriginB, float OriginC, float DirA, float DirB, float DirC, float HalfB,
                          float HalfC)
    {
        if (DirA == 0.0f)
            return false;
        float const T = -(OriginA / DirA);
        if (!(T > 0.0f))
            return false;
        if (!(std::fabs(OriginB + DirB * T) < HalfB))
            return false;
        if (!(std::fabs(OriginC + DirC * T) < HalfC))
            return false;
        return true;
    }
}

bool Win3DRayHitsAabbHalfExtents(float const HalfExtents[3], float const Matrix3x4[12], float const RayDir[3])
{
    if (HalfExtents[0] == 0.0f && HalfExtents[1] == 0.0f && HalfExtents[2] == 0.0f)
        return false;
    FVector Out;
    UiWidgetMathEngine()->InverseTransform(Out, *reinterpret_cast<FTransform const*>(Matrix3x4),
                                           *reinterpret_cast<FVector const*>(RayDir));
    float const LocalDir[3] = {Out.X + Matrix3x4[9], Out.Y + Matrix3x4[10], Out.Z + Matrix3x4[11]};
    float const LocalOrigin[3] = {-Matrix3x4[9], -Matrix3x4[10], -Matrix3x4[11]};
    if (CtlSlabHitStrict(LocalOrigin[0], LocalOrigin[1], LocalOrigin[2], LocalDir[0], LocalDir[1], LocalDir[2],
                         HalfExtents[1], HalfExtents[2]))
        return true;
    if (CtlSlabHitStrict(LocalOrigin[1], LocalOrigin[0], LocalOrigin[2], LocalDir[1], LocalDir[0], LocalDir[2],
                         HalfExtents[0], HalfExtents[2]))
        return true;
    if (CtlSlabHitStrict(LocalOrigin[2], LocalOrigin[0], LocalOrigin[1], LocalDir[2], LocalDir[0], LocalDir[1],
                         HalfExtents[0], HalfExtents[1]))
        return true;
    return false;
}
void Win3DSetViewport(ICamera*, const char*, int)
{
}

char const kShapeDbHeaderLower[] = "s_cmn\\shapedb.h";
FShapeDB G_UiShapeDB;
FShapeDB::FShapeDB()
{
}
int FShapeDB::Load(char const*, IFileSystem*)
{
    return 0;
}
void FShapeDB::Clear()
{
}
FTextureRef::FTextureRef()
    : Texture(nullptr)
    , Field04(-1)
{
}
FTextureRef::~FTextureRef()
{
}


// ---- the Font::String of the text controls (font_string.cpp): the text kept, the scale, text,
// number text and child draws recorded. A laid-out string has one line when it has a text or a
// number (the slots point at one shared row); its extent is 7 pixels a character by 16.

namespace
{
    FontStringChild GCtlLine;
    FFontStringChildSlot GCtlSlots[1];
    std::string GCtlNumber;

    int StringLength(FFontString const* Self)
    {
        if (Self->Text != nullptr && Self->Text[0] != 0)
        {
            int Count = 0;
            while (Self->Text[Count] != 0)
                ++Count;
            return Count;
        }
        return Self->Field0C != 0 ? (int)GCtlNumber.size() : 0;
    }

    void LayOut(FFontString* Self)
    {
        int const Lines = StringLength(Self) > 0 || Self->Field0C != 0 ? 1 : 0;
        GCtlSlots[0].Child = &GCtlLine;
        GCtlSlots[0].Kind = Self->Field0C;
        Self->ChildBegin = Lines != 0 ? GCtlSlots : nullptr;
        Self->ChildEnd = Lines != 0 ? GCtlSlots + 1 : nullptr;
    }

    void DropText(FFontString* Self)
    {
        free(Self->Text);
        Self->Text = nullptr;
        Self->ChildBegin = nullptr;
        Self->ChildEnd = nullptr;
        Self->Field0C = 0;
    }
}

void FontStringSetScale(FFontString* /*Self*/, float Width, float Depth)
{
    CtlEvent("\"ev\":\"fscale\",\"w\":\"" + Bits(Width) + "\",\"z\":\"" + Bits(Depth) + "\"");
}

void FontStringSetText(FFontString* Self, wchar_t const* Text)
{
    wchar_t const* const Left = Text != nullptr ? Text : L"";
    wchar_t const* const Right = Self->Text != nullptr ? Self->Text : L"";
    size_t Same = 0;
    while (Left[Same] != 0 && Left[Same] == Right[Same])
        ++Same;
    if (Left[Same] == Right[Same])
        return;
    if (Self->Text != nullptr && Self->Text[0] != 0)
        DropText(Self);
    if (Text != nullptr)
    {
        size_t Length = 0;
        while (Text[Length] != 0)
            ++Length;
        free(Self->Text);
        Self->Text = (wchar_t*)malloc((Length + 1) * sizeof(wchar_t));
        memcpy(Self->Text, Text, (Length + 1) * sizeof(wchar_t));
    }
    else if (Self->Text != nullptr)
        Self->Text[0] = 0;
    CtlEvent("\"ev\":\"ftext\",\"t\":\"" + Units(Self->Text) + "\"");
}

void FontStringSetNumberText(FFontString* Self, char const* Format)
{
    if (Self->Text != nullptr && Self->Text[0] != 0)
        DropText(Self);
    Self->Field0C = 1;
    GCtlNumber = Format;
    CtlEvent("\"ev\":\"fnum\",\"f\":\"" + oracle::HexC(Format) + "\"");
}

void FontStringSetFont(FFontString* Self, IFont* Font)
{
    if (Self->FontIface == Font)
        return;
    Self->FontIface = Font;
    Self->LayoutGen = 0;
}

void FontStringClearContents(FFontString* Self)
{
    DropText(Self);
}

int FontStringChildCount(FFontString* Self)
{
    LayOut(Self);
    return Self->ChildBegin != nullptr ? 1 : 0;
}

int* FontStringGetExtent(FFontString* Self, int* OutWH)
{
    LayOut(Self);
    OutWH[0] = 7 * StringLength(Self);
    OutWH[1] = Self->ChildBegin != nullptr ? 16 : 0;
    return OutWH;
}

void FontStringDrawChildren(FFontStringDrawOrigin* Origin, FFontString* Self, void* DrawCtx, void* DrawArg)
{
    LayOut(Self);
    float Left = 0.0f;
    float Right = 0.0f;
    memcpy(&Left, &DrawCtx, sizeof(Left));
    memcpy(&Right, &DrawArg, sizeof(Right));
    CtlEvent("\"ev\":\"fdraw\",\"x\":\"" + Bits(Origin->X) + "\",\"y\":\"" + Bits(Origin->Y) + "\",\"w\":\"" +
             Bits(Origin->Scale) + "\",\"h\":\"" + Bits(Origin->Height) +
             oracle::Format("\",\"hm\":\"%u\",\"vm\":\"%u\",\"l\":\"", Origin->Field10, Origin->Field14) + Bits(Left) +
             "\",\"r\":\"" + Bits(Right) + "\"");
}

void WideTextReserve(FHudWideText* Self, unsigned Need)
{
    if (Need <= Self->Capacity)
        return;
    if (Self->Text != nullptr)
    {
        Self->Text = static_cast<wchar_t*>(realloc(Self->Text, Need * 2));
        Self->Capacity = Need;
        return;
    }
    Self->Text = static_cast<wchar_t*>(malloc(Need * 2));
    Self->Text[0] = 0;
    Self->Capacity = Need;
}

void WideTextClear(FHudWideText* Self)
{
    if (Self->Text != nullptr && Self->Text[0] != 0)
        Self->Text[0] = 0;
}

void WideTextFree(FHudWideText* Self)
{
    if (Self->Text != nullptr)
    {
        free(Self->Text);
        Self->Text = nullptr;
        Self->Capacity = 0;
    }
}

void WideTextAssign(FHudWideText* Self, wchar_t const* Source, unsigned Len)
{
    if (Source == nullptr)
    {
        WideTextClear(Self);
        return;
    }
    if (Len == 0xffffffffu)
    {
        Len = 0;
        while (Source[Len] != 0)
            ++Len;
    }
    WideTextReserve(Self, Len + 1);
    memcpy(Self->Text, Source, Len * sizeof(wchar_t));
    Self->Text[Len] = 0;
}

// ---- the sounds, the key-map commands, the tooltip and the ids (menu_ui_init.cpp,
// menu_sound_arch.cpp, hud_command_dispatch.cpp, nn_tooltip.cpp, ids_strings.cpp): a UI sound's
// slot is the length of its name; the plays, stops, posts and hides are recorded; no id has a
// string.

int FindUiSoundSlot(const char* Name)
{
    return (Name == nullptr || Name[0] == 0) ? -1 : (int)strlen(Name);
}
void PlayOrRewindUiSoundSlot(unsigned int Slot)
{
    CtlEvent(oracle::Format("\"ev\":\"sound\",\"s\":\"%u\"", Slot));
}
void PlayUiSoundSlotFlag1(unsigned int Slot)
{
    CtlEvent(oracle::Format("\"ev\":\"tplay\",\"s\":\"%u\"", Slot));
}
void StopUiSoundSlot(unsigned int Slot)
{
    CtlEvent(oracle::Format("\"ev\":\"tstop\",\"s\":\"%u\"", Slot));
}
void PlayUiRolloverSound()
{
    CtlEvent("\"ev\":\"roll\"");
}
void* LookupClientSoundArch(struct ID_String const*)
{
    return nullptr;
}
unsigned char PlayClientSoundArchIfIdle(void*, unsigned*, void**)
{
    return 0;
}
int HudPostCommand(unsigned MsgId, int)
{
    CtlEvent(oracle::Format("\"ev\":\"post\",\"c\":\"%u\"", MsgId));
    return 1;
}
void NnToolTipHideGlobal()
{
    CtlEvent("\"ev\":\"hidetip\"");
}
bool ShouldSkipLocalServerTick()
{
    return false;
}
unsigned int GetIdsStringWWithRawFallback(unsigned int, wchar_t* Dest, int)
{
    if (Dest != nullptr)
        Dest[0] = 0;
    return 0;
}

// ---- the immediate-mode layer's other calls (win3d_immediate.cpp, win3d_renderables.cpp)

void Win3DEnable(int Capability)
{
    CtlEvent(oracle::Format("\"ev\":\"en\",\"cap\":\"%x\"", Capability));
}
void Win3DVec3Add(float* Out, float const* A, float const* B)
{
    Out[0] = A[0] + B[0];
    Out[1] = A[1] + B[1];
    Out[2] = A[2] + B[2];
}
float* CockpitMulMatrix(float* Out9, const float* A9, const float* B9)
{
    FMatrix Product;
    UiWidgetMathEngine()->MulMatrix(Product, *reinterpret_cast<FMatrix const*>(A9),
                                    *reinterpret_cast<FMatrix const*>(B9));
    memcpy(Out9, &Product, sizeof(Product));
    return Out9;
}
FImmedBatcher G_ImmedBatchA;
void ImmedBatchAddLine(FImmedBatcher* /*Batch*/, float const* From, float const* To, unsigned long Argb,
                       unsigned long /*Rgb*/, float Width)
{
    CtlEvent("\"ev\":\"line\",\"f\":\"" + Bits3(From) + "\",\"t\":\"" + Bits3(To) +
             oracle::Format("\",\"c\":\"%08lx\",\"w\":\"", Argb) + Bits(Width) + "\"");
}

// ---- the button montage (nn_montage_draw.cpp): no tiles.

std::vector<FButtonMontageTile> G_ButtonMontageTiles;
bool G_bButtonMontageLoaded = true;
void LoadButtonMontage()
{
}
bool NnMontageDrawTileAt(FButtonMontageTile const*, float const*, unsigned char const*, float, int, float)
{
    return false;
}

// ---- the rest of what the later controls name: the rich-text caption window of the rich-text
// wire button and the cells of UITable (rich_text_win.cpp, ids_rdl.cpp), the static control's
// render-to-window camera (space_camera.cpp, win3d_immediate.cpp), the table rows' payload
// handle (ui_fast_table.cpp), and the helpers the reference bodies name (star_sphere.cpp,
// hud_element_list.cpp, the latter as the reference writes it). No case reaches the first four.

bool LoadIdsIntoRdl(unsigned int, RenderDisplayList&)
{
    return false;
}
// The reference's bodies (ids_rdl.cpp): a cell before the sentinel, every cell unlinked.
void RdlAppendNode(RenderDisplayList& Rdl, RDLNode* Node)
{
    void** const Sentinel = (void**)Rdl.NodeList;
    void** const Tail = static_cast<void**>(Sentinel[1]);
    void** const Cell = static_cast<void**>(malloc(3 * sizeof(void*)));
    Cell[0] = Sentinel;
    Cell[1] = Tail != nullptr ? Tail : Cell;
    Sentinel[1] = Cell;
    static_cast<void**>(Cell[1])[0] = Cell;
    Cell[2] = Node;
    Rdl.Count += 1;
}
void RdlClear(RenderDisplayList& Rdl)
{
    void** const Sentinel = (void**)Rdl.NodeList;
    void** Cur = static_cast<void**>(Sentinel[0]);
    while (Cur != Sentinel)
    {
        void** const Cell = Cur;
        Cur = static_cast<void**>(Cur[0]);
        static_cast<void**>(Cell[1])[0] = Cell[0];
        static_cast<void**>(Cell[0])[1] = Cell[1];
        RDLNode* const Node = static_cast<RDLNode*>(Cell[2]);
        if (Node != nullptr)
            delete Node;
        Cell[2] = nullptr;
        free(Cell);
        Rdl.Count -= 1;
    }
}

void SpaceCameraConstruct(FSpaceCamera*)
{
}
// The render-to-window camera's vertical angle: kept. Its perspective is recorded with the
// fields the static control set (the rectangle, the planes, the angle); putting the Win3D
// camera's viewport back is recorded as the window's end.
void SpaceCameraSetFovY(FSpaceCamera* Cam, float FovYDeg)
{
    Cam->FovYDeg = FovYDeg;
}
void Win3DApplyCameraPerspective(ICamera* Camera)
{
    const int* const Rect = Camera->GetViewport();
    CtlEvent(oracle::Format("\"ev\":\"winview\",\"vp\":\"%d %d %d %d\",", Rect[0], Rect[1], Rect[2], Rect[3]) +
             "\"near\":\"" + Bits(Camera->GetZNear()) + "\",\"far\":\"" + Bits(Camera->GetZFar()) +
             "\",\"fov\":\"" + Bits(Camera->GetFovYDeg()) + "\"");
}
void Win3DGetViewport(int, int* Out4)
{
    Out4[0] = 0;
    Out4[1] = 0;
    Out4[2] = 0;
    Out4[3] = 0;
}
void Win3DSetViewportRect(int, int, int, int, const char*, int)
{
    CtlEvent("\"ev\":\"winview\",\"vp\":\"-\"");
}
GENRESULT NEURONCOMPONENTS_API FSpaceCamera::QueryInterface(char const*, void** Out)
{
    *Out = nullptr;
    return GR_INTERFACE_UNSUPPORTED;
}
unsigned long NEURONCOMPONENTS_API FSpaceCamera::AddRef()
{
    return 1;
}
unsigned long NEURONCOMPONENTS_API FSpaceCamera::Release()
{
    return 1;
}
float const* NEURONCOMPONENTS_API FSpaceCamera::GetPosition()
{
    return WorldPos;
}
float const* NEURONCOMPONENTS_API FSpaceCamera::GetOrientation()
{
    return World;
}
int const* NEURONCOMPONENTS_API FSpaceCamera::GetViewport()
{
    return Viewport;
}
float NEURONCOMPONENTS_API FSpaceCamera::GetHalfFovXDeg()
{
    return HalfFovXDeg;
}
float NEURONCOMPONENTS_API FSpaceCamera::GetFovYDeg()
{
    return FovYDeg;
}
float NEURONCOMPONENTS_API FSpaceCamera::GetZNear()
{
    return ZNear;
}
float NEURONCOMPONENTS_API FSpaceCamera::GetZFar()
{
    return ZFar;
}
float NEURONCOMPONENTS_API FSpaceCamera::GetAspect()
{
    return Aspect;
}
// The reference's bodies (space_camera.cpp).
double NEURONCOMPONENTS_API FSpaceCamera::GetXScaleOverNear()
{
    return static_cast<double>(this->XScale) / this->ZNear;
}
double NEURONCOMPONENTS_API FSpaceCamera::GetYScaleOverNear()
{
    return static_cast<double>(this->YScale) / this->ZNear;
}
// The reference's body (space_camera.cpp), over x86math's engine.
int NEURONCOMPONENTS_API FSpaceCamera::Project(float* OutX, float* OutY, float* OutZ, float const* WorldPoint)
{
    FVector Local;
    UiWidgetMathEngine()->InverseTransform(Local, *reinterpret_cast<FTransform const*>(this->World),
                                           *reinterpret_cast<FVector const*>(WorldPoint));
    if (!(-this->ZNear >= Local.Z))
        return 0;
    const double InvZ = -1.0 / static_cast<double>(Local.Z);
    *OutX = static_cast<float>(static_cast<double>(Local.X) * this->XScale * InvZ + this->ScreenCenterX);
    *OutY = static_cast<float>(static_cast<double>(Local.Y) * this->YScale * InvZ + this->ScreenCenterY);
    *OutZ = -Local.Z;
    return 1;
}
void NEURONCOMPONENTS_API FSpaceCamera::Unproject(float*, float, float)
{
}
int NEURONCOMPONENTS_API FSpaceCamera::ClassifySphere(float const*, float)
{
    return 0;
}
float const* NEURONCOMPONENTS_API FSpaceCamera::GetViewTransform()
{
    return View;
}
int NEURONCOMPONENTS_API FSpaceCamera::ProjectVec(float*, float const*)
{
    return 0;
}
void FSpaceCamera::GetVelocity(float*)
{
}
int FSpaceCamera::SetProperty(char const*, char const*)
{
    return 0;
}


void Win3DIdentityRotation3x3(float* Out9)
{
    if (Out9 == nullptr)
        return;
    Out9[0] = 1.0f;
    Out9[1] = 0.0f;
    Out9[2] = 0.0f;
    Out9[3] = 0.0f;
    Out9[4] = 1.0f;
    Out9[5] = 0.0f;
    Out9[6] = 0.0f;
    Out9[7] = 0.0f;
    Out9[8] = 1.0f;
}
void HudVec3ScaleByInverse(float* Out, float const* In, float Length)
{
    double const InvExact = 1.0 / static_cast<double>(Length);
    float const Inv = static_cast<float>(InvExact);
    float const Z = static_cast<float>(InvExact * In[2]);
    float const Y = static_cast<float>(static_cast<double>(Inv) * In[1]);
    float const X = static_cast<float>(static_cast<double>(Inv) * In[0]);
    Out[0] = X;
    Out[1] = Y;
    Out[2] = Z;
}

// The rich-text windows' other names (pixel_format.cpp, reg_key_stack.cpp, nn_tooltip.cpp): no
// case makes a surface or reaches the registry; the hover reset is the reference's body.
int PixelFormatFromD3D(unsigned long)
{
    return 0;
}
bool PixelFormatFind(unsigned long* OutId, unsigned long, unsigned long, unsigned long, unsigned long,
                     unsigned long)
{
    *OutId = 0;
    return false;
}
HKEY FRegKeyStack::Top() const
{
    return nullptr;
}
void ToolTipHoverReset(FToolTipHoverInfo* Info)
{
    Info->IdsAlt = 0;
    Info->Ids = -1;
    Info->AuxField = 0;
    if (Info->CaptionW != nullptr && Info->CaptionW[0] != L'\0')
        Info->CaptionW[0] = L'\0';
    Info->bLive = 0;
}

void RunUiControlSuites()
{
    ControlsSuite();
}
