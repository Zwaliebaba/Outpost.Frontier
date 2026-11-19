// P4.11, P4.12, P4.13: the client's IME boxes and their draw, through the reference's own code.
// imeui.dll's sources (CCaret.cpp, CInputBox.cpp, IMEInputBox.cpp, CImeUiFont.cpp, IMEFocus.cpp,
// ImeUiCore.cpp, ImeUiToolbar.cpp) and the client's FLImeInputBox (fl_ime_input_box.cpp) and
// ClientIMEFocus (client_ime_focus.cpp) are compiled into this file. The Windows calls they make
// that the cases depend on are renamed to stand-ins (#define before the includes, after the
// system headers): IMM32 (an input context the window has or not, the open and conversion
// status, a composition and a candidate list the case sets), the keyboard layout, the tick
// count and caret blink time, the string resources of ImeUi.dll (imeui.rc's string table), the
// character classes, and GDI's font calls, whose faces measure as the port's deterministic
// metrics backend does (neuron-client::text::font::fake: a cell of |lfHeight| pixels, a fifth
// more for a negative height; a quarter cell for "il.,'|" and space, a whole one for M and W,
// half for the rest; no face for a height of 0 or an empty name). The client's font strings,
// font atlas, screen quads and immediate-mode layer are recording stand-ins; a string measures
// 7 pixels a unit and 13 high (0 when empty). The rich-text window that owns a box is a
// recording stand-in of its SetText. ImeFocusDrawPass is compiled under another name (the
// controls suite has a stand-in of it); the cases call IMEFocus::Draw. The Rust side is
// crates/neuron-client/tests/oracle_vectors.rs with crates/neuron-client/tests/support/ime.rs;
// the inputs come from crates/neuron-client/tests/fixtures/oracle/make_ime_fixtures.py.
//
// ime_ui.list holds one case per line: operations separated by " ; ", their fields by single
// spaces, integers in decimal unless noted, texts as UTF-16 units in hex (4 digits each, "-"
// for none). Every case starts with no IME focus, a window with its default input context, the
// layout 04090409, the IME open, conversion mode 1, tick 0, a caret blink time of 500, an 800 x
// 600 client area and "ConfirmPlayerText" as Arial at height 16. Operations:
//   screen W H | blink B | lf H FACE (before focus) | focus (new ClientIMEFocus) |
//   box (new FLImeInputBox, owned by window N, N the box's number) | del I |
//   key I K (HandleKey) | mb I B (HandleMultiByteChar of a byte) | wc I U (HandleWideChar) |
//   onchar U | onkey K (IMEFocus::OnChar, OnKeyDown) | resize I N L T R B
//   (SetInputBufferSize(N) and slot 3 Resize) | setfocus I|- | force I (SetFocus(box, true)) |
//   win 0|1 (IMEFocus::OnKillFocus, OnSetFocus) | lang L (the layout in hex, then
//   ImeUi_GetState) | imestate OPEN CONV (then ImeUi_GetState) | comp TEXT ATTRS CURSOR
//   (WM_IME_COMPOSITION with the composition string, its attribute bytes in hex and the cursor) |
//   endcomp (WM_IME_ENDCOMPOSITION) | cand SEL PAGE TEXT... (IMN_OPENCANDIDATE with that list) |
//   nocand (IMN_CLOSECANDIDATE) | tick T | caret I (the box's UpdateCaret) | fheight I H (its
//   font's SetHeight) | extent I TEXT (its font's GetTextExtent) | draw (IMEFocus::Draw(0)) |
//   state I | fstate | gdi | appearance | parse TEXT | langstate | candstate
//
// Lines, each prefixed with "case": the owner windows' SetText, the last text each window got
// during an operation, in window order -- {"ev":"settext","o","t"}; the draw's calls --
// {"ev":"begin","p"} {"ev":"v","x","y","z"} {"ev":"c3","r","g","b"} {"ev":"end"} (the
// immediate-mode layer), {"ev":"qkey","k"} {"ev":"qframe","f","blend","find"}
// {"ev":"quad","x0","y0","x1","y1","da","db","u0","v0","u1","v1","c"} {"ev":"flush","k"} (the
// screen quads), {"ev":"akey","k"} {"ev":"acolor","c"} (the font atlas), {"ev":"ssize","s","t",
// "w","h"} {"ev":"spaint","s","t","x","y"} (the strings "confirm", "text" and "comp"); and the
// operations' results -- {"ev":"focus","avail"} {"ev":"r","v"} {"ev":"caret","v"}
// {"ev":"extent","w","h"} {"ev":"box",...} {"ev":"fstate","n","focus"}
// {"ev":"gdi","created","deleted"} {"ev":"appearance",...} {"ev":"parse","v"}
// {"ev":"lang","vertical","symbol"} {"ev":"cand","count","page","sel","e"}. Floats are f32 bits in
// hex. CCaret leaves its blink flag uninitialised; the harness sets it to 1 after each box is
// made, as the port starts it (divergence D282).

#define IMEUI_EXPORTS

#include "oracle_support.h"

#include <windows.h>
#include <imm.h>
#include <msctf.h>
#include <ctfutb.h>
#include <winnls.h>

#include <stdlib.h>
#include <string.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "imeui.h"
#include "font.h"
#include "../OutpostFrontier/Client/font_atlas.h"
#include "../OutpostFrontier/Client/font_imp.h"
#include "../OutpostFrontier/Client/font_string.h"
#include "../OutpostFrontier/Client/globals.h"
#include "../OutpostFrontier/Client/screen_quad_state.h"
#include "../OutpostFrontier/Client/win3d_immediate.h"
#include "../OutpostFrontier/Client/win3d_renderables.h"
#include "../OutpostFrontier/Client/rich_text_win.h"
#include "NeuronRuntime.h"
#include "neuroncomponents.h"
#include "render/IRenderPipeline.h"

namespace
{
    oracle::Suite* GImeOut = nullptr;
    unsigned GImeCase = 0;

    void ImeEvent(const std::string& Fields)
    {
        if (GImeOut != nullptr)
            GImeOut->Line(oracle::Format("{\"case\":\"%u\",", GImeCase) + Fields + "}");
    }

    std::string Bits(float Value)
    {
        return oracle::Format("%08x", oracle::FloatBits(Value));
    }

    std::string Units(const wchar_t* Text, size_t Count)
    {
        std::string Out;
        for (size_t Index = 0; Index < Count; ++Index)
            Out += oracle::Format("%04x", static_cast<unsigned>(static_cast<unsigned short>(Text[Index])));
        return Out;
    }

    std::string UnitsC(const wchar_t* Text)
    {
        return Text != nullptr ? Units(Text, wcslen(Text)) : std::string();
    }

    // UTF-16 texts as vectors of units, without a terminator (no std::wstring: a local build
    // with 2-byte wchar_t over a runtime built for 4-byte ones cannot use it).
    typedef std::vector<wchar_t> FWide;

    FWide WideOf(const wchar_t* Text)
    {
        FWide Out;
        if (Text != nullptr)
            Out.assign(Text, Text + wcslen(Text));
        return Out;
    }

    FWide Terminated(const FWide& Text)
    {
        FWide Out(Text);
        Out.push_back(0);
        return Out;
    }

    FWide DecodeUnits(const std::string& Hex)
    {
        FWide Out;
        if (Hex == "-")
            return Out;
        for (size_t Index = 0; Index + 4 <= Hex.size(); Index += 4)
            Out.push_back(static_cast<wchar_t>(strtoul(Hex.substr(Index, 4).c_str(), nullptr, 16)));
        return Out;
    }

    // --- The window's IME (IMM32 stand-ins) ----------------------------------------------------
    HWND const kWindow = reinterpret_cast<HWND>(static_cast<UINT_PTR>(0x5678));
    HIMC const kDefaultContext = reinterpret_cast<HIMC>(static_cast<UINT_PTR>(0x1234));
    HIMC GAssociated = kDefaultContext;
    BOOL GImeOpenStatus = TRUE;
    DWORD GConversion = 1;
    DWORD GTickNow = 0;
    UINT GBlinkTime = 500;
    UINT_PTR GLayout = 0x04090409;
    FWide GCompText;
    std::vector<BYTE> GCompAttrs;
    LONG GCompCursor = 0;
    std::vector<unsigned char> GCandBytes;

    HIMC OracleImmGetContext(HWND) { return GAssociated; }
    BOOL OracleImmReleaseContext(HWND, HIMC) { return TRUE; }
    HIMC OracleImmAssociateContext(HWND, HIMC Context)
    {
        HIMC const Previous = GAssociated;
        GAssociated = Context;
        return Previous;
    }
    BOOL OracleImmIsIME(HKL) { return TRUE; }
    BOOL OracleImmGetOpenStatus(HIMC) { return GImeOpenStatus; }
    BOOL OracleImmGetConversionStatus(HIMC, LPDWORD Conversion, LPDWORD Sentence)
    {
        if (Conversion != nullptr)
            *Conversion = GConversion;
        if (Sentence != nullptr)
            *Sentence = 0;
        return TRUE;
    }
    BOOL OracleImmNotifyIME(HIMC, DWORD, DWORD, DWORD) { return TRUE; }
    DWORD OracleImmGetCandidateListW(HIMC, DWORD, LPCANDIDATELIST List, DWORD Size)
    {
        DWORD const Need = static_cast<DWORD>(GCandBytes.size());
        if (List == nullptr || Size == 0)
            return Need;
        if (Size < Need)
            return 0;
        memcpy(List, GCandBytes.data(), Need);
        return Need;
    }
    DWORD OracleImmGetCandidateListA(HIMC, DWORD, LPCANDIDATELIST, DWORD) { return 0; }
    LONG OracleImmGetCompositionStringW(HIMC, DWORD Kind, LPVOID Buffer, DWORD Length)
    {
        if (Kind == GCS_CURSORPOS)
            return GCompCursor;
        if (Kind == GCS_COMPSTR)
        {
            DWORD Bytes = static_cast<DWORD>(GCompText.size() * sizeof(wchar_t));
            if (Bytes > Length)
                Bytes = Length;
            if (Buffer != nullptr && Bytes != 0)
                memcpy(Buffer, GCompText.data(), Bytes);
            return static_cast<LONG>(Bytes);
        }
        if (Kind == GCS_COMPATTR)
        {
            DWORD Bytes = static_cast<DWORD>(GCompAttrs.size());
            if (Bytes > Length)
                Bytes = Length;
            if (Buffer != nullptr && Bytes != 0)
                memcpy(Buffer, GCompAttrs.data(), Bytes);
            return static_cast<LONG>(Bytes);
        }
        return 0;
    }
    LONG OracleImmGetCompositionStringA(HIMC, DWORD, LPVOID, DWORD) { return 0; }
    UINT OracleImmGetVirtualKey(HWND) { return 0; }
    UINT OracleImmGetIMEFileNameA(HKL, LPSTR Name, UINT)
    {
        if (Name != nullptr)
            Name[0] = 0;
        return 0;
    }
    HWND OracleImmGetDefaultIMEWnd(HWND) { return nullptr; }

    // --- The rest of the system ---------------------------------------------------------------
    int GScreenW = 800;
    int GScreenH = 600;

    DWORD OracleGetTickCount() { return GTickNow; }
    UINT OracleGetCaretBlinkTime() { return GBlinkTime; }
    HKL OracleGetKeyboardLayout(DWORD) { return reinterpret_cast<HKL>(GLayout); }
    int OracleGetKeyboardLayoutList(int Count, HKL* Layouts)
    {
        if (Count > 0 && Layouts != nullptr)
            Layouts[0] = reinterpret_cast<HKL>(GLayout);
        return 1;
    }
    BOOL OracleIsWindowUnicode(HWND) { return TRUE; }
    BOOL OracleGetClientRect(HWND, LPRECT Rect)
    {
        Rect->left = 0;
        Rect->top = 0;
        Rect->right = GScreenW;
        Rect->bottom = GScreenH;
        return TRUE;
    }
    HMODULE const kModule = reinterpret_cast<HMODULE>(static_cast<UINT_PTR>(0x10000));
    HMODULE OracleLoadLibraryA(LPCSTR) { return kModule; }
    HMODULE OracleLoadLibraryW(LPCWSTR) { return kModule; }
    FARPROC OracleGetProcAddress(HMODULE, LPCSTR) { return nullptr; }
    // imeui.rc's string table.
    const char* const kAppearanceStrings[17] = {
        "0", "0x00404040", "0xff000000", "24", "0", "0xa0", "0xffffffff", "0xff000000",
        "0xff000000", "0x00ffff00", "0x000000ff", "0x0000ff00", "0x00ff0000", "0x00ff0000", "0x80",
        "0xff000000", "2"};
    int OracleLoadStringW(HINSTANCE, UINT Id, LPWSTR Buffer, int Capacity)
    {
        if (Id < 1 || Id > 17 || Capacity <= 0)
            return 0;
        const char* const Text = kAppearanceStrings[Id - 1];
        int Count = 0;
        while (Text[Count] != 0 && Count + 1 < Capacity)
        {
            Buffer[Count] = static_cast<wchar_t>(Text[Count]);
            ++Count;
        }
        Buffer[Count] = 0;
        return Count;
    }
    BOOL OracleGetVersionExA(LPOSVERSIONINFOA Info)
    {
        Info->dwMajorVersion = 5;
        Info->dwMinorVersion = 1;
        Info->dwBuildNumber = 2600;
        Info->dwPlatformId = VER_PLATFORM_WIN32_NT;
        return TRUE;
    }
    BOOL OracleGetVersionExW(LPOSVERSIONINFOW Info)
    {
        Info->dwMajorVersion = 5;
        Info->dwMinorVersion = 1;
        Info->dwBuildNumber = 2600;
        Info->dwPlatformId = VER_PLATFORM_WIN32_NT;
        return TRUE;
    }
    int OracleGetLocaleInfoA(LCID, LCTYPE, LPSTR Data, int Capacity)
    {
        if (Data != nullptr && Capacity >= 5)
            strcpy(Data, "1252");
        return 5;
    }
    BOOL OracleIsDBCSLeadByteEx(UINT, BYTE) { return FALSE; }
    BOOL OracleIsDBCSLeadByte(BYTE) { return FALSE; }
    // A byte is the unit of the same value (the cases type ASCII).
    int OracleMultiByteToWideChar(UINT, DWORD, LPCCH Bytes, int Count, LPWSTR Out, int Capacity)
    {
        if (Count < 0)
            Count = static_cast<int>(strlen(Bytes)) + 1;
        int Index = 0;
        for (; Index < Count && Index < Capacity; ++Index)
            Out[Index] = static_cast<wchar_t>(static_cast<unsigned char>(Bytes[Index]));
        return Index;
    }
    // CT_CTYPE1's C1_CNTRL: the Unicode Cc class.
    BOOL OracleGetStringTypeExW(LCID, DWORD, LPCWCH Text, int Count, LPWORD Types)
    {
        for (int Index = 0; Index < Count; ++Index)
        {
            unsigned const Unit = static_cast<unsigned short>(Text[Index]);
            Types[Index] = (Unit < 0x20u || (Unit >= 0x7fu && Unit <= 0x9fu)) ? C1_CNTRL : 0;
        }
        return TRUE;
    }
    HRESULT OracleCoCreateInstance(REFCLSID, LPUNKNOWN, DWORD, REFIID, LPVOID* Out)
    {
        if (Out != nullptr)
            *Out = nullptr;
        return E_FAIL;
    }

    // --- GDI's fonts, measured as the port's deterministic backend ------------------------------
    struct FFakeFace
    {
        LONG Cell;
    };
    std::map<UINT_PTR, FFakeFace> GFaces;
    std::map<UINT_PTR, UINT_PTR> GSelected;
    UINT_PTR GNextHandle = 0x100;
    std::vector<int> GCreated;
    int GDeleted = 0;

    HDC OracleCreateCompatibleDC(HDC)
    {
        GNextHandle += 4;
        GSelected[GNextHandle] = 0;
        return reinterpret_cast<HDC>(GNextHandle);
    }
    HFONT OracleCreateFontIndirectA(const LOGFONTA* Font)
    {
        GCreated.push_back(static_cast<int>(Font->lfHeight));
        if (Font->lfHeight == 0 || Font->lfFaceName[0] == 0)
            return nullptr;
        LONG const Height = Font->lfHeight;
        GNextHandle += 4;
        GFaces[GNextHandle].Cell = Height < 0 ? -Height + (-Height) / 5 : Height;
        return reinterpret_cast<HFONT>(GNextHandle);
    }
    HGDIOBJ OracleSelectObject(HDC Dc, HGDIOBJ Object)
    {
        UINT_PTR const Key = reinterpret_cast<UINT_PTR>(Dc);
        UINT_PTR const Value = reinterpret_cast<UINT_PTR>(Object);
        if (GSelected.find(Key) == GSelected.end())
            return nullptr;
        if (Value != 0 && GFaces.find(Value) == GFaces.end())
            return nullptr;
        UINT_PTR const Previous = GSelected[Key];
        if (Value != 0)
            GSelected[Key] = Value;
        return reinterpret_cast<HGDIOBJ>(Previous);
    }
    BOOL OracleDeleteObject(HGDIOBJ Object)
    {
        UINT_PTR const Key = reinterpret_cast<UINT_PTR>(Object);
        if (GFaces.erase(Key) != 0)
        {
            ++GDeleted;
            return TRUE;
        }
        return GSelected.erase(Key) != 0;
    }
    LONG FakeAdvance(LONG Cell, unsigned Unit)
    {
        switch (Unit)
        {
        case 0:
            return 0;
        case 0x20: case 0x69: case 0x6c: case 0x2e: case 0x2c: case 0x27: case 0x7c:
            return Cell / 4 > 1 ? Cell / 4 : 1;
        case 0x4d: case 0x57:
            return Cell;
        default:
            return Cell / 2;
        }
    }
    BOOL OracleGetTextExtentPoint32W(HDC Dc, LPCWSTR Text, int Count, LPSIZE Size)
    {
        std::map<UINT_PTR, UINT_PTR>::const_iterator const Selected =
            GSelected.find(reinterpret_cast<UINT_PTR>(Dc));
        if (Selected == GSelected.end() || Selected->second == 0)
            return FALSE;
        LONG const Cell = GFaces[Selected->second].Cell;
        LONG Width = 0;
        for (int Index = 0; Index < Count; ++Index)
            Width += FakeAdvance(Cell, static_cast<unsigned short>(Text[Index]));
        Size->cx = Width;
        Size->cy = Cell;
        return TRUE;
    }
    BOOL OracleGetTextExtentPoint32A(HDC, LPCSTR, int, LPSIZE) { return FALSE; }

    // --- The client's font, strings, atlas, screen quads and immediate-mode layer ---------------
    LOGFONTA GConfirmLogFont;

    class OracleImeFont : public IFont
    {
    public:
        char const* Slot00() override { return ""; }
        char const* GetName() override { return "ConfirmPlayerText"; }
        float GetFixedHeight() override { return 0.0f; }
        HFONT__* Slot03() override { return nullptr; }
        tagLOGFONTA const* GetLogFont() override { return &GConfirmLogFont; }
        float GetGlyphAdvance(wchar_t) override { return 0.0f; }
        int GetLineHeight() override { return 0; }
        int Slot07() override { return 0; }
        void MeasureString(int* OutWH, wchar_t const*, int) override { OutWH[0] = OutWH[1] = 0; }
        int ProbeString(int, wchar_t const*, int) override { return 0; }
        int GetCharWidths(wchar_t const*, int, int*) override { return 0; }
    };
    OracleImeFont GConfirmFont;

    IFont* OracleFindMenuFontImp(char const*) { return &GConfirmFont; }

    unsigned GAtlasKey = 0;
    unsigned OracleFontAtlasMaterialKey() { return GAtlasKey; }
    void OracleFontAtlasSetMaterialKey(unsigned Key)
    {
        GAtlasKey = Key;
        ImeEvent(oracle::Format("\"ev\":\"akey\",\"k\":\"%08x\"", Key));
    }
    void OracleFontAtlasSetDrawColor(unsigned Color)
    {
        ImeEvent(oracle::Format("\"ev\":\"acolor\",\"c\":\"%08x\"", Color));
    }

    void OracleScreenQuadSetMaterialKey(unsigned long Key)
    {
        ImeEvent(oracle::Format("\"ev\":\"qkey\",\"k\":\"%08lx\"", Key));
    }
    void OracleScreenQuadBeginBatchFrame(unsigned long const* Frame, int Blend, bool bFind)
    {
        ImeEvent(oracle::Format("\"ev\":\"qframe\",\"f\":\"%08lx %08lx %08lx %08lx %08lx\","
                                "\"blend\":\"%d\",\"find\":\"%d\"",
                                Frame[0], Frame[1], Frame[2], Frame[3], Frame[4], Blend,
                                bFind ? 1 : 0));
    }
    void OracleScreenQuadAppendRecord(FScreenQuadRecord const& R)
    {
        ImeEvent("\"ev\":\"quad\",\"x0\":\"" + Bits(R.X0) + "\",\"y0\":\"" + Bits(R.Y0) +
                 "\",\"x1\":\"" + Bits(R.X1) + "\",\"y1\":\"" + Bits(R.Y1) + "\",\"da\":\"" +
                 Bits(R.DepthA) + "\",\"db\":\"" + Bits(R.DepthB) + "\",\"u0\":\"" + Bits(R.U0) +
                 "\",\"v0\":\"" + Bits(R.V0) + "\",\"u1\":\"" + Bits(R.U1) + "\",\"v1\":\"" +
                 Bits(R.V1) + oracle::Format("\",\"c\":\"%08x\"", static_cast<unsigned>(R.ColorBits)));
    }
    void OracleScreenQuadFlush(IRenderPipeline*, unsigned long Key)
    {
        ImeEvent(oracle::Format("\"ev\":\"flush\",\"k\":\"%08lx\"", Key));
    }

    void OracleWin3DBegin(int Mode) { ImeEvent(oracle::Format("\"ev\":\"begin\",\"p\":\"%d\"", Mode)); }
    void OracleWin3DVertex3f(float X, float Y, float Z)
    {
        ImeEvent("\"ev\":\"v\",\"x\":\"" + Bits(X) + "\",\"y\":\"" + Bits(Y) + "\",\"z\":\"" + Bits(Z) + "\"");
    }
    void OracleWin3DColor3ub(unsigned char R, unsigned char G, unsigned char B)
    {
        ImeEvent(oracle::Format("\"ev\":\"c3\",\"r\":\"%u\",\"g\":\"%u\",\"b\":\"%u\"", R, G, B));
    }
    void OracleWin3DEnd() { ImeEvent("\"ev\":\"end\""); }
    void OracleWin3DPushProjection() {}
    void OracleWin3DPopProjection() {}
    void OracleWin3DSetOrtho2D() {}
    void OracleWin3DEnable(int) {}
    void OracleWin3DDisable(int) {}

    unsigned OracleImeSurfaceWidth = 800;
    unsigned OracleImeSurfaceHeight = 600;
    struct FOracleImeClient
    {
        HWND window;
    };
    FOracleImeClient OracleImeClient = {kWindow};
}

// The client's font string as a recording stand-in (FontStringChild's name in the client
// sources compiled below).
class OracleImeString
{
public:
    OracleImeString() : Role("?"), bNull(true) {}
    int Release()
    {
        delete this;
        return 0;
    }
    void BindFont(IFont*) {}
    void SetShadow(unsigned char) {}
    void SetText(wchar_t const* Text)
    {
        bNull = Text == nullptr;
        Value = WideOf(Text);
    }
    int* GetSize(int* OutWH)
    {
        OutWH[0] = static_cast<int>(Value.size()) * 7;
        OutWH[1] = Value.empty() ? 0 : 13;
        ImeEvent(oracle::Format("\"ev\":\"ssize\",\"s\":\"%s\",\"t\":\"", Role) +
                 Units(Value.data(), Value.size()) +
                 oracle::Format("\",\"w\":\"%d\",\"h\":\"%d\"", OutWH[0], OutWH[1]));
        return OutWH;
    }
    void Paint(void* Origin, void*, void*)
    {
        FFontStringDrawOrigin const* const O = static_cast<FFontStringDrawOrigin const*>(Origin);
        ImeEvent(oracle::Format("\"ev\":\"spaint\",\"s\":\"%s\",\"t\":\"", Role) +
                 Units(Value.data(), Value.size()) + "\",\"x\":\"" + Bits(O->X) + "\",\"y\":\"" +
                 Bits(O->Y) + "\"");
    }

    const char* Role;
    bool bNull;
    FWide Value;
};

// The rich-text edit window that owns a box: its SetText recorded.
class OracleImeOwner
{
public:
    void SetText(wchar_t const* Text, void const*);
    int Index;
};

namespace
{
    std::map<int, FWide> GOwnerTexts;
}

void OracleImeOwner::SetText(wchar_t const* Text, void const*)
{
    GOwnerTexts[Index] = WideOf(Text);
}

#define ImmGetContext OracleImmGetContext
#define ImmReleaseContext OracleImmReleaseContext
#define ImmAssociateContext OracleImmAssociateContext
#define ImmIsIME OracleImmIsIME
#define ImmGetOpenStatus OracleImmGetOpenStatus
#define ImmGetConversionStatus OracleImmGetConversionStatus
#define ImmNotifyIME OracleImmNotifyIME
#define ImmGetCandidateListW OracleImmGetCandidateListW
#define ImmGetCandidateListA OracleImmGetCandidateListA
#define ImmGetCompositionStringW OracleImmGetCompositionStringW
#define ImmGetCompositionStringA OracleImmGetCompositionStringA
#define ImmGetVirtualKey OracleImmGetVirtualKey
#define ImmGetIMEFileNameA OracleImmGetIMEFileNameA
#define ImmGetDefaultIMEWnd OracleImmGetDefaultIMEWnd
#define GetTickCount OracleGetTickCount
#define GetCaretBlinkTime OracleGetCaretBlinkTime
#define GetKeyboardLayout OracleGetKeyboardLayout
#define GetKeyboardLayoutList OracleGetKeyboardLayoutList
#define IsWindowUnicode OracleIsWindowUnicode
#define GetClientRect OracleGetClientRect
#define LoadLibraryA OracleLoadLibraryA
#define LoadLibraryW OracleLoadLibraryW
#define GetProcAddress OracleGetProcAddress
#define LoadStringW OracleLoadStringW
#define GetVersionExA OracleGetVersionExA
#define GetVersionExW OracleGetVersionExW
#define GetLocaleInfoA OracleGetLocaleInfoA
#define IsDBCSLeadByteEx OracleIsDBCSLeadByteEx
#define IsDBCSLeadByte OracleIsDBCSLeadByte
#define MultiByteToWideChar OracleMultiByteToWideChar
#define GetStringTypeExW OracleGetStringTypeExW
#define CoCreateInstance OracleCoCreateInstance
#define CreateCompatibleDC OracleCreateCompatibleDC
#define CreateFontIndirectA OracleCreateFontIndirectA
#define SelectObject OracleSelectObject
#define DeleteObject OracleDeleteObject
#define GetTextExtentPoint32W OracleGetTextExtentPoint32W
#define GetTextExtentPoint32A OracleGetTextExtentPoint32A
#define FindMenuFontImp OracleFindMenuFontImp
#define FontAtlasMaterialKey OracleFontAtlasMaterialKey
#define FontAtlasSetMaterialKey OracleFontAtlasSetMaterialKey
#define FontAtlasSetDrawColor OracleFontAtlasSetDrawColor
#define ScreenQuadSetMaterialKey OracleScreenQuadSetMaterialKey
#define ScreenQuadBeginBatchFrame OracleScreenQuadBeginBatchFrame
#define ScreenQuadAppendRecord OracleScreenQuadAppendRecord
#define ScreenQuadFlush OracleScreenQuadFlush
#define Win3DBegin OracleWin3DBegin
#define Win3DVertex3f OracleWin3DVertex3f
#define Win3DColor3ub OracleWin3DColor3ub
#define Win3DEnd OracleWin3DEnd
#define Win3DPushProjection OracleWin3DPushProjection
#define Win3DPopProjection OracleWin3DPopProjection
#define Win3DSetOrtho2D OracleWin3DSetOrtho2D
#define Win3DEnable OracleWin3DEnable
#define Win3DDisable OracleWin3DDisable
#define G_RenderSurfaceWidth OracleImeSurfaceWidth
#define G_RenderSurfaceHeight OracleImeSurfaceHeight
#define g_client OracleImeClient
#define ImeFocusDrawPass OracleImeFocusDrawPass
#define FontStringChild OracleImeString
#define RichTextEditWin OracleImeOwner

#include "../imeui/CCaret.cpp"
#include "../imeui/CInputBox.cpp"
#include "../imeui/IMEInputBox.cpp"
#include "../imeui/CImeUiFont.cpp"
#include "../imeui/IMEFocus.cpp"
#include "../imeui/ImeUiCore.cpp"
#include "../imeui/ImeUiToolbar.cpp"
#include "../OutpostFrontier/Client/fl_ime_input_box.cpp"
#include "../OutpostFrontier/Client/client_ime_focus.cpp"

#undef RichTextEditWin
#undef FontStringChild

namespace
{
    std::vector<std::string> SplitOn(const std::string& Text, const std::string& Separator)
    {
        std::vector<std::string> Out;
        size_t Start = 0;
        for (;;)
        {
            size_t const At = Text.find(Separator, Start);
            if (At == std::string::npos)
            {
                Out.push_back(Text.substr(Start));
                return Out;
            }
            Out.push_back(Text.substr(Start, At - Start));
            Start = At + Separator.size();
        }
    }

    int Int(const std::string& Text)
    {
        return static_cast<int>(strtol(Text.c_str(), nullptr, 10));
    }

    unsigned Hex32(const std::string& Text)
    {
        return static_cast<unsigned>(strtoul(Text.c_str(), nullptr, 16));
    }

    struct FImeWorld
    {
        ClientIMEFocus* Focus = nullptr;
        std::vector<FLImeInputBox*> Boxes; // null once deleted
        std::vector<std::unique_ptr<OracleImeOwner>> Owners;
    };

    void SetConfirmLogFont(int Height, const std::string& Face)
    {
        memset(&GConfirmLogFont, 0, sizeof(GConfirmLogFont));
        GConfirmLogFont.lfHeight = Height;
        GConfirmLogFont.lfWeight = FW_NORMAL;
        GConfirmLogFont.lfCharSet = DEFAULT_CHARSET;
        GConfirmLogFont.lfOutPrecision = OUT_TT_PRECIS;
        GConfirmLogFont.lfQuality = ANTIALIASED_QUALITY;
        strncpy(GConfirmLogFont.lfFaceName, Face.c_str(), 31);
    }

    // imeui's and the client's globals back to their load-time values.
    void ResetIme()
    {
        GAssociated = kDefaultContext;
        GImeOpenStatus = TRUE;
        GConversion = 1;
        GTickNow = 0;
        GBlinkTime = 500;
        GScreenW = 800;
        GScreenH = 600;
        OracleImeSurfaceWidth = 800;
        OracleImeSurfaceHeight = 600;
        GCompText.clear();
        GCompAttrs.clear();
        GCompCursor = 0;
        GCandBytes.clear();
        GFaces.clear();
        GSelected.clear();
        GNextHandle = 0x100;
        GCreated.clear();
        GDeleted = 0;
        GOwnerTexts.clear();
        GAtlasKey = NEURONCOMPONENTS_CRC::GetCRC32("font");
        SetConfirmLogFont(16, "Arial");
        // ImeUi_CheckLanguage keeps the last layout in a function-local static: a layout no case
        // uses makes the next check compute again.
        GLayout = 0x5a5a0409;
        ImeUi_CheckLanguage();
        GLayout = 0x04090409;

        GWindow = NULL;
        GMessageWindow = NULL;
        GbInitialized = false;
        GbEnabled = false;
        GSupportLevel = 0;
        GRequestedSupportLevel = 0;
        GbInsertMode = true;
        GbShowReading = false;
        GbCandidateListOpen = false;
        GbVerticalCandidates = true;
        GbDisabled = false;
        GImeState = 0;
        GScreenWidth = 0;
        GScreenHeight = 0;
        GCompositionFont = NULL;
        GCandidateColor = 0;
        GCompositionColor = 0;
        memset(GCompositionBackgrounds, 0, sizeof(GCompositionBackgrounds));
        GbFinalizing = false;
        GbSyntheticKey = false;
        memset(GCompositionAttributes, 0, sizeof(GCompositionAttributes));
        GbUnicodeWindow = true;
        GbUnicodeIme = true;
        GbCapturedContext = false;
        GLanguageSymbol = L"A";
        GbHorizontalReading = false;
        GLanguage = 0;
        GKeyboardLayout = NULL;
        GCapturedContext = NULL;
        GCaretBlinkInterval = 0;
        GPosition.x = 0;
        GPosition.y = 0;
        memset(&GBounding, 0, sizeof(GBounding));
        memset(GComposition, 0, sizeof(GComposition));
        memset(GCandidates, 0, sizeof(GCandidates));
        memset(GReading, 0, sizeof(GReading));
        GClientCaret = 0;
        GCompositionCursor = 0;
        GCandidateSelection = 0;
        GCandidateCount = 0;
        GCandidatePageSize = 0;
        GReadingSelection = 0;
        GLastCaretTick = 0;
        GbCaretVisible = false;
        memset(GRenderedComposition, 0, sizeof(GRenderedComposition));
        GToolbarLastTick = 0.0;
        GToolbarPhase = 0;
        GDBCSLeadByte = 0;
        GSymbolFont.Color = 0xffffffffu;
        memset(GSymbolFont.Symbol, 0, sizeof(GSymbolFont.Symbol));
        G_ConfirmPlayerFont = nullptr;
        G_ClientImeFocus = nullptr;
    }

    void FlushOwnerTexts()
    {
        for (std::map<int, FWide>::const_iterator It = GOwnerTexts.begin(); It != GOwnerTexts.end(); ++It)
            ImeEvent(oracle::Format("\"ev\":\"settext\",\"o\":\"%d\",\"t\":\"", It->first) +
                     Units(It->second.data(), It->second.size()) + "\"");
        GOwnerTexts.clear();
    }

    int BoxIndex(const FImeWorld& World, IMEInputBox* Box)
    {
        for (size_t Index = 0; Index < World.Boxes.size(); ++Index)
            if (World.Boxes[Index] != nullptr && World.Boxes[Index] == Box)
                return static_cast<int>(Index);
        return -1;
    }

    void BoxState(FLImeInputBox* B)
    {
        std::string Buffer;
        for (DWORD Index = 0; Index <= B->m_dwBufferSize; ++Index)
            Buffer += oracle::Format("%04x", static_cast<unsigned>(B->m_pBuffer[Index]));
        ImeEvent("\"ev\":\"box\",\"text\":\"" + UnitsC(reinterpret_cast<wchar_t const*>(B->m_pBuffer)) +
                 "\",\"buf\":\"" + Buffer +
                 oracle::Format("\",\"size\":\"%lu\",\"caret\":\"%lu\",\"cx\":\"%d\",\"cy\":\"%d\","
                                "\"px\":\"%d\",\"py\":\"%d\",\"shown\":\"%d\",\"blink\":\"%d\","
                                "\"focus\":\"%d\",\"insert\":\"%d\",\"rect\":\"%ld %ld %ld %ld\","
                                "\"margins\":\"%lu %lu\"",
                                static_cast<unsigned long>(B->m_dwBufferSize),
                                static_cast<unsigned long>(B->m_dwCaretPos), B->m_nCaretPosX,
                                B->m_nCaretPosY, B->m_Caret.m_nX, B->m_Caret.m_nY,
                                B->m_Caret.m_bShow ? 1 : 0, B->m_Caret.m_bBlink ? 1 : 0,
                                B->m_bHasFocus ? 1 : 0, B->m_bInsertMode ? 1 : 0,
                                static_cast<long>(B->m_rcBounding.left), static_cast<long>(B->m_rcBounding.top),
                                static_cast<long>(B->m_rcBounding.right), static_cast<long>(B->m_rcBounding.bottom),
                                static_cast<unsigned long>(B->m_dwMarginX),
                                static_cast<unsigned long>(B->m_dwMarginY)));
    }

    void AppearanceLine(const char* Name, const _IMEUI_APPEARANCE& A)
    {
        ImeEvent(oracle::Format("\"ev\":\"appearance\",\"k\":\"%s\",\"v\":\"%08x %08x %08x %u %u %u "
                                "%08x %08x %08x %08x %08x %08x %08x %08x %u %08x\"",
                                Name, A.SymbolOnColor, A.SymbolOffColor, A.SymbolBackgroundColor,
                                A.SymbolHeight, A.SymbolAlpha, A.SymbolPosition, A.CandidateTextColor,
                                A.CandidateSelectedTextColor, A.CandidateColor, A.CompositionInputColor,
                                A.CompositionTargetColor, A.CompositionConvertedColor,
                                A.CompositionTargetConvertedColor, A.CompositionErrorColor,
                                A.CompositionAlpha, A.CompositionColor));
    }

    // A CANDIDATELIST of the strings, as ImmGetCandidateListW fills it.
    void BuildCandidates(DWORD Selection, DWORD PageSize, const std::vector<FWide>& Strings)
    {
        DWORD const Count = static_cast<DWORD>(Strings.size());
        DWORD const Header = 6 * sizeof(DWORD) + Count * sizeof(DWORD);
        DWORD Size = Header;
        for (size_t Index = 0; Index < Strings.size(); ++Index)
            Size += static_cast<DWORD>((Strings[Index].size() + 1) * sizeof(wchar_t));
        GCandBytes.assign(Size < sizeof(CANDIDATELIST) ? sizeof(CANDIDATELIST) : Size, 0);
        CANDIDATELIST* const List = reinterpret_cast<CANDIDATELIST*>(GCandBytes.data());
        List->dwSize = Size;
        List->dwStyle = IME_CAND_READ;
        List->dwCount = Count;
        List->dwSelection = Selection;
        List->dwPageStart = 0;
        List->dwPageSize = PageSize;
        DWORD Offset = Header;
        for (DWORD Index = 0; Index < Count; ++Index)
        {
            List->dwOffset[Index] = Offset;
            if (!Strings[Index].empty())
                memcpy(GCandBytes.data() + Offset, Strings[Index].data(),
                       Strings[Index].size() * sizeof(wchar_t));
            Offset += static_cast<DWORD>((Strings[Index].size() + 1) * sizeof(wchar_t));
        }
    }

    void RunCase(const std::string& Line)
    {
        ResetIme();
        FImeWorld World;
        const std::vector<std::string> Ops = SplitOn(Line, " ; ");
        for (size_t OpIndex = 0; OpIndex < Ops.size(); ++OpIndex)
        {
            const std::vector<std::string> F = SplitOn(Ops[OpIndex], " ");
            const std::string& Name = F[0];
            FLImeInputBox* B = nullptr;
            if (F.size() > 1 && (Name == "del" || Name == "key" || Name == "mb" || Name == "wc" ||
                                 Name == "resize" || Name == "force" || Name == "caret" ||
                                 Name == "fheight" || Name == "extent" || Name == "state"))
            {
                const int Index = Int(F[1]);
                if (Index >= 0 && Index < static_cast<int>(World.Boxes.size()))
                    B = World.Boxes[Index];
                if (B == nullptr)
                    continue;
            }
            if (Name == "screen")
            {
                GScreenW = Int(F[1]);
                GScreenH = Int(F[2]);
                OracleImeSurfaceWidth = static_cast<unsigned>(GScreenW);
                OracleImeSurfaceHeight = static_cast<unsigned>(GScreenH);
            }
            else if (Name == "blink")
                GBlinkTime = static_cast<UINT>(Int(F[1]));
            else if (Name == "lf")
            {
                FWide const Face = DecodeUnits(F[2]);
                std::string Narrow;
                for (size_t Index = 0; Index < Face.size(); ++Index)
                    Narrow += static_cast<char>(Face[Index]);
                SetConfirmLogFont(Int(F[1]), Narrow);
            }
            else if (Name == "focus")
            {
                World.Focus = new ClientIMEFocus(kWindow);
                World.Focus->ConfirmText->Role = "confirm";
                ImeEvent(oracle::Format("\"ev\":\"focus\",\"avail\":\"%d\"",
                                        World.Focus->IsImeAvailable() ? 1 : 0));
            }
            else if (Name == "box")
            {
                std::unique_ptr<OracleImeOwner> Owner(new OracleImeOwner());
                Owner->Index = static_cast<int>(World.Owners.size());
                FLImeInputBox* const Box = new FLImeInputBox(Owner.get());
                Box->TextString->Role = "text";
                Box->CompositionString->Role = "comp";
                Box->m_Caret.m_bBlink = true;
                World.Owners.push_back(std::move(Owner));
                World.Boxes.push_back(Box);
            }
            else if (Name == "del")
            {
                World.Boxes[Int(F[1])] = nullptr;
                delete B;
            }
            else if (Name == "key")
                ImeEvent(oracle::Format("\"ev\":\"r\",\"v\":\"%d\"", B->HandleKey(static_cast<UINT>(Int(F[2]))) ? 1 : 0));
            else if (Name == "mb")
                ImeEvent(oracle::Format("\"ev\":\"r\",\"v\":\"%d\"",
                                        B->HandleMultiByteChar(static_cast<char>(Int(F[2]))) ? 1 : 0));
            else if (Name == "wc")
                ImeEvent(oracle::Format("\"ev\":\"r\",\"v\":\"%d\"",
                                        B->HandleWideChar(static_cast<unsigned short>(Hex32(F[2]))) ? 1 : 0));
            else if (Name == "onchar" && World.Focus != nullptr)
                ImeEvent(oracle::Format("\"ev\":\"r\",\"v\":\"%d\"", World.Focus->OnChar(Hex32(F[1])) ? 1 : 0));
            else if (Name == "onkey" && World.Focus != nullptr)
                ImeEvent(oracle::Format("\"ev\":\"r\",\"v\":\"%d\"",
                                        World.Focus->OnKeyDown(static_cast<UINT>(Int(F[1]))) ? 1 : 0));
            else if (Name == "resize")
            {
                RECT Rect;
                Rect.left = Int(F[3]);
                Rect.top = Int(F[4]);
                Rect.right = Int(F[5]);
                Rect.bottom = Int(F[6]);
                B->SetInputBufferSize(static_cast<DWORD>(Int(F[2])));
                B->Resize(&Rect);
            }
            else if (Name == "setfocus" && World.Focus != nullptr)
            {
                FLImeInputBox* Target = nullptr;
                if (F[1] != "-")
                {
                    const int Index = Int(F[1]);
                    if (Index >= 0 && Index < static_cast<int>(World.Boxes.size()))
                        Target = World.Boxes[Index];
                }
                World.Focus->SetFocus(Target, false);
            }
            else if (Name == "force" && World.Focus != nullptr)
                World.Focus->SetFocus(B, true);
            else if (Name == "win" && World.Focus != nullptr)
            {
                if (Int(F[1]) != 0)
                    World.Focus->OnSetFocus();
                else
                    World.Focus->OnKillFocus();
            }
            else if (Name == "lang")
            {
                GLayout = Hex32(F[1]);
                ImeUi_GetState();
            }
            else if (Name == "imestate")
            {
                GImeOpenStatus = Int(F[1]) != 0 ? TRUE : FALSE;
                GConversion = static_cast<DWORD>(Int(F[2]));
                ImeUi_GetState();
            }
            else if (Name == "comp")
            {
                GCompText = DecodeUnits(F[1]);
                GCompAttrs.clear();
                if (F[2] != "-")
                    for (size_t Index = 0; Index + 2 <= F[2].size(); Index += 2)
                        GCompAttrs.push_back(static_cast<BYTE>(Hex32(F[2].substr(Index, 2))));
                GCompCursor = Int(F[3]);
                BOOL Handled = FALSE;
                ImeUi_ProcessMessage(kWindow, WM_IME_COMPOSITION, 0, GCS_COMPSTR | GCS_COMPATTR | GCS_CURSORPOS,
                                     &Handled);
            }
            else if (Name == "endcomp")
            {
                BOOL Handled = FALSE;
                ImeUi_ProcessMessage(kWindow, WM_IME_ENDCOMPOSITION, 0, 0, &Handled);
            }
            else if (Name == "cand")
            {
                std::vector<FWide> Strings;
                for (size_t Index = 3; Index < F.size(); ++Index)
                    Strings.push_back(DecodeUnits(F[Index]));
                BuildCandidates(static_cast<DWORD>(Int(F[1])), static_cast<DWORD>(Int(F[2])), Strings);
                BOOL Handled = FALSE;
                ImeUi_ProcessMessage(kWindow, WM_IME_NOTIFY, IMN_OPENCANDIDATE, 0, &Handled);
            }
            else if (Name == "nocand")
            {
                BOOL Handled = FALSE;
                ImeUi_ProcessMessage(kWindow, WM_IME_NOTIFY, IMN_CLOSECANDIDATE, 0, &Handled);
            }
            else if (Name == "tick")
                GTickNow = static_cast<DWORD>(strtoul(F[1].c_str(), nullptr, 10));
            else if (Name == "caret")
                ImeEvent(oracle::Format("\"ev\":\"caret\",\"v\":\"%d\"", B->m_Caret.UpdateCaret() ? 1 : 0));
            else if (Name == "fheight")
                B->m_pFont->SetHeight(Int(F[2]));
            else if (Name == "extent")
            {
                FWide const Text = Terminated(DecodeUnits(F[2]));
                int Width = -1;
                int Height = -1;
                B->m_pFont->GetTextExtent(Text.data(), &Width, &Height);
                ImeEvent(oracle::Format("\"ev\":\"extent\",\"w\":\"%d\",\"h\":\"%d\"", Width, Height));
            }
            else if (Name == "draw" && World.Focus != nullptr)
                World.Focus->Draw(nullptr);
            else if (Name == "state")
                BoxState(B);
            else if (Name == "fstate")
            {
                int const Count = World.Focus != nullptr ? World.Focus->NumInputBoxes() : 0;
                int const Focused = World.Focus != nullptr ? BoxIndex(World, World.Focus->GetFocus()) : -1;
                ImeEvent(oracle::Format("\"ev\":\"fstate\",\"n\":\"%d\",\"focus\":\"%d\"", Count, Focused));
            }
            else if (Name == "gdi")
            {
                std::string Created;
                for (size_t Index = 0; Index < GCreated.size(); ++Index)
                    Created += (Index != 0 ? " " : "") + oracle::Format("%d", GCreated[Index]);
                ImeEvent("\"ev\":\"gdi\",\"created\":\"" + Created +
                         oracle::Format("\",\"deleted\":\"%d\"", GDeleted));
            }
            else if (Name == "appearance" && World.Focus != nullptr)
            {
                const _IMEUI_APPEARANCE* const Default = World.Focus->GetDefaultAppearance();
                _IMEUI_APPEARANCE Copy = *Default;
                Copy.SymbolFont = nullptr;
                AppearanceLine("default", Copy);
                ImeUi_SetAppearance(Default);
                _IMEUI_APPEARANCE Set;
                ImeUi_GetAppearance(&Set);
                Set.SymbolFont = nullptr;
                AppearanceLine("set", Set);
            }
            else if (Name == "parse")
            {
                FWide const Text = Terminated(DecodeUnits(F[1]));
                ImeEvent(oracle::Format("\"ev\":\"parse\",\"v\":\"%08x\"", ParseAppearanceNumber(Text.data())));
            }
            else if (Name == "langstate")
                ImeEvent(oracle::Format("\"ev\":\"lang\",\"vertical\":\"%d\",\"symbol\":\"", GbVerticalCandidates ? 1 : 0) +
                         UnitsC(GLanguageSymbol) + "\"");
            else if (Name == "candstate")
            {
                std::string Entries;
                for (int Index = 0; Index < MAX_CANDLIST; ++Index)
                    Entries += (Index != 0 ? " " : "") + (GCandidates[Index][0] != 0 ? UnitsC(GCandidates[Index]) : std::string("-"));
                ImeEvent(oracle::Format("\"ev\":\"cand\",\"count\":\"%d\",\"page\":\"%d\",\"sel\":\"%d\",\"e\":\"",
                                        GCandidateCount, GCandidatePageSize, GCandidateSelection) +
                         Entries + "\"");
            }
            FlushOwnerTexts();
        }
        for (size_t Index = 0; Index < World.Boxes.size(); ++Index)
            delete World.Boxes[Index];
        World.Boxes.clear();
        delete World.Focus;
        GOwnerTexts.clear();
    }

    void ImeUiSuite()
    {
        oracle::Suite Out("ime_ui.jsonl");
        GImeOut = &Out;
        const std::vector<std::string> Lines = oracle::InputLines("ime_ui.list");
        for (size_t Index = 0; Index < Lines.size(); ++Index)
        {
            GImeCase = static_cast<unsigned>(Index);
            RunCase(Lines[Index]);
        }
        GImeOut = nullptr;
    }
}

void RunImeUiSuites()
{
    ImeUiSuite();
}
