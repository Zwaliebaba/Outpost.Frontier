// Author: Giperion.

#include "fl_ime_input_box.h"

#include "client_ime_focus.h"   // ClientConfirmPlayerFont, ClientIme*Key, ClientImeSwapFontMaterialKey
#include "font.h"               // IFont::GetLogFont
#include "font_atlas.h"         // FontAtlasSetDrawColor (the routine)
#include "font_string.h"        // FontStringChild, FFontStringDrawOrigin
#include "rich_text_win.h"      // RichTextEditWin::SetText
#include "screen_quad_state.h"  // ScreenQuadFlush (the routine)

#include "NeuronRuntime.h"              // NeuronRuntime::Pipeline

FLImeInputBox::FLImeInputBox(RichTextEditWin* InOwner)
    // the field fetched on first use, its slot 4 (+0x10) LOGFONTA handed to
    // IMEInputBox's constructor (0x005642c2).
    : IMEInputBox(ClientConfirmPlayerFont()->GetLogFont())
    , Owner(InOwner)
    , TextString(nullptr)
    , CompositionString(nullptr)
{
    // +0x98 = the routine, the font bound (slot 2) and slot 8(0).
    TextString = new FontStringChild();
    TextString->BindFont(ClientConfirmPlayerFont());
    TextString->SetShadow(0);
    // the same for +0x9c.
    CompositionString = new FontStringChild();
    CompositionString->BindFont(ClientConfirmPlayerFont());
    CompositionString->SetShadow(0);
}

FLImeInputBox::~FLImeInputBox()
{
    // slot 1 (Release) on +0x98 and +0x9c when set, then ~IMEInputBox.
    if (TextString != nullptr)
        TextString->Release();
    if (CompositionString != nullptr)
        CompositionString->Release();
}

void FLImeInputBox::Resize(const RECT* pRect)
{
    // a local {0, 0, right (+8), bottom (+0xc)} to IMEInputBox::Resize.
    RECT Local;
    Local.left = 0;
    Local.top = 0;
    Local.right = pRect->right;
    Local.bottom = pRect->bottom;
    IMEInputBox::Resize(&Local);
}

void FLImeInputBox::UpdatedText(const unsigned short* pText)
{
    // the routine(Text, 0) on the owner, then slot 5 (SetText) on +0x9c.
    wchar_t const* const Text = reinterpret_cast<wchar_t const*>(pText);
    Owner->SetText(Text, nullptr);
    CompositionString->SetText(Text);
}

void FLImeInputBox::Draw(void* /*pContext*/)
{
    // RET 4.
}

void FLImeInputBox::DrawString(int X, int Y, unsigned Color, const wchar_t* Text)
{
    // (RET 0x10).
    // the font material swapped to the field.
    unsigned PreviousKey = 0;
    ClientImeSwapFontMaterialKey(&PreviousKey, static_cast<unsigned>(ClientImeTextKey()));

    // the composed text measured (slot 6); its width comes off X.
    int Size[2];
    CompositionString->GetSize(Size);
    int const Left = X - Size[0];

    // the draw colour, Color's bytes reordered {2, 1, 0, 3} into the
    // record the routine reads (0xAARRGGBB -> R, G, B, A in memory).
    unsigned char Rgba[4];
    Rgba[0] = static_cast<unsigned char>((Color >> 16) & 0xffu);
    Rgba[1] = static_cast<unsigned char>((Color >> 8) & 0xffu);
    Rgba[2] = static_cast<unsigned char>(Color & 0xffu);
    Rgba[3] = static_cast<unsigned char>((Color >> 24) & 0xffu);
    unsigned Packed = 0;
    Packed |= static_cast<unsigned>(Rgba[0]);
    Packed |= static_cast<unsigned>(Rgba[1]) << 8;
    Packed |= static_cast<unsigned>(Rgba[2]) << 16;
    Packed |= static_cast<unsigned>(Rgba[3]) << 24;
    FontAtlasSetDrawColor(Packed);

    // +0x98 takes the text (slot 5).
    TextString->SetText(Text);

    // X = margin X + Left and Y = margin Y + Y, each summed as a dword
    // and converted as an UNSIGNED 32-bit value (FILD qword with a zero high half); then slot 9
    // (Paint) with (origin, 0, 0). baseline's origin record is a stack run whose tail holds the
    // measured size and other residue; the port's Paint reads X and Y only, so the rest is zero.
    unsigned const SumX = static_cast<unsigned>(m_dwMarginX) + static_cast<unsigned>(Left);
    unsigned const SumY = static_cast<unsigned>(m_dwMarginY) + static_cast<unsigned>(Y);
    FFontStringDrawOrigin Origin;
    Origin.X = static_cast<float>(static_cast<double>(SumX));
    Origin.Y = static_cast<float>(static_cast<double>(SumY));
    Origin.Scale = 0.0f;
    Origin.Height = 0.0f;
    Origin.Field10 = 0;
    Origin.Field14 = 0;
    TextString->Paint(&Origin, nullptr, nullptr);

    // the previous material key back, then the routine for the
    // and the field lists of the screen-quad container map.
    unsigned Discard = 0;
    ClientImeSwapFontMaterialKey(&Discard, PreviousKey);
    ScreenQuadFlush(NeuronRuntime::Pipeline, ClientImeBackgroundKey());   // (.., the field)
    ScreenQuadFlush(NeuronRuntime::Pipeline, ClientImeTextKey());         // (.., the field)
}
