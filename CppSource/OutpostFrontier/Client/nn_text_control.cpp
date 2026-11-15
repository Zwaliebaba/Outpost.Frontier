// Author: Giperion.

#include "nn_text_control.h"
#include "hud_widescreen.h"   // port-only (OUTPOST_FRONTIER_WIDESCREEN_SUPPORT)

#include "font_imp.h"
#include "font_atlas.h"
#include "font_string.h"
#include "game_options.h"
#include "globals.h"                // GetClientFrameTimeCapped (the routine)
#include "ids_strings.h"
#include "input_manager.h"          // GetClientSoftwareCursor (the field / the field)
#include "menu_ui_init.h"
#include "nn_ui_pulse.h"
#include "space_camera.h"           // FSpaceCamera (the field fields)
#include "text_win.h"
#include "tooltip_hover.h"          // FToolTipHoverInfo, NnToolTipFeedHover (the routine)
#include "win3d.h"
#include "win3d_engine_submit.h"    // Win3DRayHitsAabbHalfExtents (the routine)
#include "win3d_immediate.h"
#include "win_factory.h"

#include "common_csys.h"      // Acquire3DMathEngine (the routine)
#include "common_ini.h"
#include "x86math.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <string>

// WINFACTORY<TextControl> (static init the branch: the routine
// "TextControl" + atexit the branch -> the routine). Its Create the routine (vftable
// ) is the template's: operator_new(0x3a0), the routine, +0x10 = &factory+0xc,
// Win3D slot 9 Create(Parent, Payload); false -> the deleting dtor (flag 1) and 0.
WINFACTORY<NnTextControl> G_TextControlFactory("TextControl");

namespace
{
    // how many TextControls hold a typewriter run (+0x389). The ctor clamps it
    // to 0 from below (00589ceb); the dtor and property 0x50 decrement it and stop UI sound
    // slot 0xb when it reaches zero.
    int G_TextControlTypingCount = 0;

    // TextControl's cached 3D math engine (the routine on first use).
    I3DMathEngine* G_TextControlMath = nullptr;

    // The FLD [+0xb0] / FADD [+0x68] pair every Font::String scale call of this class feeds to
    // as its second argument: Offset.z + Position.z.
    float TextDepth(NnTextControl const* Self)
    {
        return Self->Offset[2] + Self->Position[2];
    }
}

NnTextControl::NnTextControl()
{
    // . WinControl the routine runs as the base ctor.
    // the colour dwords at +0x348 and +0x35e are zeroed byte by byte first.
    OverrideColour[0] = 0;
    OverrideColour[1] = 0;
    OverrideColour[2] = 0;
    OverrideColour[3] = 0;
    PendingText = nullptr;
    PendingCapacity = 0;
    for (int Index = 0; Index < 4; ++Index)
    {
        BlinkColourOn[Index] = 0;
        BlinkColourOff[Index] = 0;
    }
    Width = 0.0f;
    Height = 0.0f;
    Byte354 = 0;
    Dword358 = 0;
    // the field into +0x348, then byte +0x34b = 0xff.
    TextColour = (G_NnPaletteArgb_679bb0 & 0x00ffffffUL) | 0xff000000UL;
    Field338 = -1;
    Field33c = -1;
    Field340 = 0;
    Field34c = 0;
    Field350 = 0;
    Dword344 = 0;
    Byte35c = 0;
    bColourOverride = 0;
    bTypewriter = 0;
    TypedCount = 1;
    TypeElapsed = 0.0f;
    TypeInterval = 0.05f;           // 0x3d4ccccd
    Byte378 = 0;
    bBlink = 0;
    BlinkPhase = 0;
    BlinkElapsed = 0.0f;
    Byte380 = 1;
    bTyping = 0;
    if (G_TextControlTypingCount < 0)
        G_TextControlTypingCount = 0;

    // operator_new(0x34) + the routine (Font::String ctor) into +0x334.
    FontString = static_cast<FFontString*>(::operator new(sizeof(FFontString)));
    FontStringConstruct(FontString);
    Byte394 = 0;
    Byte395 = 0;
    bInCaptureList = 0;
}

NnTextControl::~NnTextControl()
{
    // .
    if (bTyping != 0)
    {
        // end the typewriter run -- the count drops, the last run stops
        // UI sound slot 0xb, and the full pending text goes into the Font::String.
        bTyping = 0;
        G_TextControlTypingCount = G_TextControlTypingCount - 1;
        if (G_TextControlTypingCount <= 0)
            StopUiSoundSlot(0xb);   // (0xb)
        wchar_t* const Text = PendingText;
        FontStringSetScale(FontString, Width, TextDepth(this));
        FontStringSetText(FontString, Text);
    }
    FontStringClearContents(FontString); // the routine

    // the Font::String's own destructor inlined (the routine again, then
    // on every child slot left -- none, the clear emptied the range -- the child
    // buffer, the text buffer through free when there is one, and the object).
    FFontString* const String = FontString;
    if (String != nullptr)
    {
        FontStringClearContents(String);
        operator delete[](String->ChildBegin);
        String->ChildBegin = nullptr;
        String->ChildEnd = nullptr;
        String->ChildCap = nullptr;
        ::operator delete(String);
    }
    FontString = nullptr;

    if (bInCaptureList != 0)
        Win3DRemoveCapture(this);

    if (PendingText != nullptr)
    {
        free(PendingText);
        PendingText = nullptr;
        PendingCapacity = 0;
    }
    // ~WinControl the routine runs as the base dtor.
}

void NnTextControl::Init(char const* StyleName, float const* InPosition, char const* Hardpoint,
                         float InWidth, float InHeight, float Arg6, unsigned Ids, int InDword358,
                         int InDword344, unsigned char InByte378)
{
    // . Arg6 has no read in the body.
    (void)Arg6;
    Byte378 = InByte378;

    if (StyleName != nullptr)
    {
        // the style's font (the routine); a non-null one goes through the
        // body the routine has, inlined here byte for byte.
        IFont* const Font = FindMenuFontImp(StyleName);
        if (Font != nullptr)
            SetFont(Font);
    }

    // Offset(+0xa8) = *Position.
    Offset[0] = InPosition[0];
    Offset[1] = InPosition[1];
    Offset[2] = InPosition[2];

    if (Hardpoint != nullptr)
    {
        // strcpy into +0x1d0.
        strcpy(HardpointName, Hardpoint);
        // attach transform = identity, zero position.
        AttachOrientation[0] = 1.0f;
        AttachOrientation[4] = 1.0f;
        AttachOrientation[8] = 1.0f;
        AttachOrientation[7] = 0.0f;
        AttachOrientation[6] = 0.0f;
        AttachOrientation[5] = 0.0f;
        AttachOrientation[3] = 0.0f;
        AttachOrientation[2] = 0.0f;
        AttachOrientation[1] = 0.0f;
        AttachPosition[2] = 0.0f;
        AttachPosition[1] = 0.0f;
        AttachPosition[0] = 0.0f;
        // the parent's WinControl face (slot 2) resolves the hardpoint into
        // +0x2f8 through its vt+0x90; the result is not looked at.
        Win3D* const ParentWin = GetParent();
        if (ParentWin != nullptr)
        {
            WinControl* const ParentControl = ParentWin->AsWinControl();
            if (ParentControl != nullptr)
                ParentControl->GetHardpointTransform(Hardpoint, AttachOrientation);
        }
    }

    Height = InHeight;
    Width = InWidth;
    SetIds(Ids);                    // the routine
    Dword358 = InDword358;
    Dword344 = InDword344;
}

bool NnTextControl::Create(Win3D* NewParent, void* Data)
{
    // (RET 8). 00589edb..00589efa: the routine(this, NewParent), the routine(this),
    // flags = (flags & 0xfb) | 3 -- Win3D's Create the routine inlined, same effects.
    Win3D::Create(NewParent, nullptr);

    INI_Reader* const Ini = static_cast<INI_Reader*>(Data);
    // / 00589f05: no payload, or no first value -> straight to the tail.
    if (Ini != nullptr && Ini->read_value())
    {
        do
        {
            if (Ini->is_value("font"))
            {
                // the routine(value); a non-null font goes to the
                // Font::String through the routine (not through SetFont the routine).
                IFont* const Font = FindMenuFontImp(Ini->get_value_string());
                if (Font != nullptr)
                    FontStringSetFont(FontString, Font);
            }
            else if (Ini->is_value("offset"))
            {
                // the values are read 1, 0, 2; Offset(+0xa8) = {0, 1, 2}.
                float const Y = static_cast<float>(Ini->get_value_float(1));
                float const X = Ini->get_value_float(0);
                float const Z = Ini->get_value_float(2);
                Offset[0] = X;
                Offset[1] = Y;
                Offset[2] = Z;
            }
            else if (Ini->is_value("hardpoint"))
            {
                // identity attach orientation (+0x2f8), zero attach position
                // (+0x31c). Unlike Init, the name is NOT copied to +0x1d0.
                AttachOrientation[8] = 1.0f;
                AttachOrientation[4] = 1.0f;
                AttachOrientation[0] = 1.0f;
                AttachOrientation[7] = 0.0f;
                AttachOrientation[6] = 0.0f;
                AttachOrientation[5] = 0.0f;
                AttachOrientation[3] = 0.0f;
                AttachOrientation[2] = 0.0f;
                AttachOrientation[1] = 0.0f;
                AttachPosition[2] = 0.0f;
                AttachPosition[1] = 0.0f;
                AttachPosition[0] = 0.0f;
                // the NEW parent (the argument, not +0x04) through its
                // WinControl face (slot 2): vt+0x90(value, +0x2f8); the result is not looked at.
                if (NewParent != nullptr)
                {
                    WinControl* const ParentControl = NewParent->AsWinControl();
                    if (ParentControl != nullptr)
                        ParentControl->GetHardpointTransform(Ini->get_value_string(),
                                                             AttachOrientation);
                }
            }
            else if (Ini->is_value("width"))
            {
                Width = Ini->get_value_float(0);
            }
            else if (Ini->is_value("height"))
            {
                Height = Ini->get_value_float(0);
            }
            else if (Ini->is_value("font_scale"))
            {
                // matched and skipped -- no read.
            }
            else if (Ini->is_value("stringID"))
            {
                // ftol(value) into the routine.
                SetIds(static_cast<unsigned>(static_cast<long>(Ini->get_value_float(0))));
            }
            else if (Ini->is_value("justification"))
            {
                Dword358 = static_cast<long>(Ini->get_value_float(0));
            }
            else if (Ini->is_value("tooltip"))
            {
                Dword344 = static_cast<long>(Ini->get_value_float(0));
            }
        } while (Ini->read_value());
    }

    // the parent (+0x04) gets its own slot 5 with its own position (+0x60).
    Win3D* const ParentWin = GetParent();
    if (ParentWin != nullptr)
        ParentWin->SetPosition(ParentWin->Position);
    return true;
}

bool NnTextControl::HitTest(float const* Ray)
{
    // (RET 4). 0058a24f..0058a261: neither a tooltip nor +0x35c -> false.
    if (Dword344 == 0 && Byte35c == 0)
        return false;

    // the x87 sums (Position + Offset) stay on the stack; each half extent is
    // stored as a float first and added / subtracted from memory; FSTP float at the end.
    double const SumX = static_cast<double>(Position[0]) + static_cast<double>(Offset[0]);
    double const SumY = static_cast<double>(Position[1]) + static_cast<double>(Offset[1]);
    float const CentreZ =
        static_cast<float>(static_cast<double>(Position[2]) + static_cast<double>(Offset[2]));
    float const HalfWidth = Width * 0.5f;   // FMUL the field, FSTP float
    float const HalfHeight = Height * 0.5f;
    float const CentreX = static_cast<float>(SumX + static_cast<double>(HalfWidth));
    float const CentreY = static_cast<float>(SumY - static_cast<double>(HalfHeight));

    float const HalfExtents[3] = {HalfWidth, HalfHeight, 0.0f};
    float Matrix[12] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f,
                        CentreX, CentreY, CentreZ};
    // the routine(HalfExtents; Matrix, Ray); false -> false.
    if (!Win3DRayHitsAabbHalfExtents(HalfExtents, Matrix, Ray))
        return false;

    if (Dword344 != 0)
    {
        // the hover record (caption zeroed, the routine on +0x10), +0x00 = -1,
        // +0x04 = 0, +0x44 = 0, then the routine(caption, null, -1) -- a null source on an empty
        // caption, no effect.
        FToolTipHoverInfo Info;
        Info.AuxField = 0;                              // +0x44
        // +0x00 and +0x04 = the tooltip ids, +0x60 = 1, WorldPos = the box
        // centre recomputed the same way, HalfExtents = {W/2, H/2, 0}.
        Info.Ids = Dword344;
        Info.IdsAlt = Dword344;
        Info.bLive = 1;
        Info.WorldPos[0] = CentreX;
        Info.WorldPos[1] = CentreY;
        Info.WorldPos[2] = CentreZ;
        Info.HalfExtents[0] = HalfWidth;
        Info.HalfExtents[1] = HalfHeight;
        Info.HalfExtents[2] = 0.0f;
        NnToolTipFeedHover(&Info);                      // the routine
        // the record's teardown (~FToolTipHoverInfo).
    }

    // the parent's WinControl face gets SetProperty(0x46, this's face, 0).
    Win3D* const ParentWin = GetParent();
    if (ParentWin != nullptr)
    {
        WinControl* const ParentControl = ParentWin->AsWinControl();
        if (ParentControl != nullptr)
            ParentControl->SetProperty(0x46, reinterpret_cast<uintptr_t>(AsWinControl()), 0);
    }
    return true;
}

bool NnTextControl::MouseEvent(FWin3DMouseEvent const* Event)
{
    // (RET 4). 0058ad95..0058ad9e: without +0x35c -> false.
    if (Byte35c == 0)
        return false;

    // {-1, 3} -> the parent's WinControl face: SetProperty(0x27, this, 0).
    if (Event->Field0 == -1 && Event->Field1 == 3 && Parent != nullptr)
    {
        WinControl* const ParentControl = Parent->AsWinControl();
        if (ParentControl != nullptr)
            ParentControl->SetProperty(0x27, reinterpret_cast<uintptr_t>(AsWinControl()), 0);
    }

    if (bInCaptureList != 0)
    {
        // off the capture stack (the routine).
        bInCaptureList = 0;
        Win3DRemoveCapture(this);
        if (Event->Field0 != -1)
            return false;
        if (Event->Field1 == 1)
        {
            // the parent hears 0x26; true with or without a parent.
            if (Parent != nullptr)
            {
                WinControl* const ParentControl = Parent->AsWinControl();
                if (ParentControl != nullptr)
                    ParentControl->SetProperty(0x26, reinterpret_cast<uintptr_t>(AsWinControl()),
                                               0);
            }
            return true;
        }
    }

    // {-1, n != 1} -> onto the capture stack (the routine), true.
    if (Event->Field0 != -1 || Event->Field1 == 1)
        return false;
    bInCaptureList = 1;
    Win3DPushCapture(this);
    return true;
}

void NnTextControl::SetOrientation(float const* NewOrientation)
{
    // (RET 4).
    bAttached = 1;
    float Attach[9];
    GetAttachOrientation(Attach);   // slot 40 (returns its argument)
    if (G_TextControlMath == nullptr)
        G_TextControlMath = Acquire3DMathEngine();
    // math +0x20 MulMatrix(Out, NewOrientation, Attach).
    FMatrix Product;
    G_TextControlMath->MulMatrix(Product, *reinterpret_cast<FMatrix const*>(NewOrientation),
                                 *reinterpret_cast<FMatrix const*>(Attach));
    memcpy(Orientation, &Product, sizeof(Orientation)); // 9 dwords to +0x3c
}

void NnTextControl::UpdateInactive()
{
    // . 0058afd7..0058aff2: slot 12 on every child whose bit 0 is clear.
    for (Win3D* Child = ChildHead; Child != nullptr; Child = Child->NextSibling)
    {
        if ((Child->Flags & 1) == 0)
            Child->UpdateInactive();
    }
    // the typewriter stops -- the same flush as property 0x50.
    if (bTyping != 0)
    {
        bTyping = 0;
        G_TextControlTypingCount = G_TextControlTypingCount - 1;
        if (G_TextControlTypingCount <= 0)  // TEST / JG skips when still positive
            StopUiSoundSlot(0xb);           // the routine(0xb)
        FontStringSetScale(FontString, Width, TextDepth(this));
        FontStringSetText(FontString, PendingText);             // the routine
    }
}

void NnTextControl::Update()
{
    // . 0058b069: the routine -- per child, bit 0 ? slot 11 : slot 12.
    Win3D::Update();

    // the typewriter runs while +0x362 is set, the Font::String has a child
    // array with at least one slot (read inline, no layout rebuild), the control is visible
    // (flags bit 1) and the local server tick is not held (the routine).
    bool const bHasChildren =
        FontString->ChildBegin != nullptr && (FontString->ChildEnd - FontString->ChildBegin) != 0;
    if (bTypewriter != 0 && bHasChildren && (Flags & 2) != 0 && !ShouldSkipLocalServerTick())
    {
        // an empty pending text takes a copy of the Font::String's text
        // (grown to wcslen + 1 as the routine, wcsncpy, terminator); with no text there, a
        // non-empty pending text is emptied.
        if (PendingText == nullptr || PendingText[0] == L'\0')
        {
            wchar_t const* const Source = FontString->Text;
            if (Source != nullptr)
            {
                unsigned const Len = static_cast<unsigned>(wcslen(Source));
                FHudWideText Pending = {PendingText, PendingCapacity};
                WideTextReserve(&Pending, Len + 1);
                PendingText = Pending.Text;
                PendingCapacity = Pending.Capacity;
                wcsncpy(PendingText, Source, Len);
                PendingText[Len] = L'\0';
            }
            else if (PendingText != nullptr && PendingText[0] != L'\0')
            {
                PendingText[0] = L'\0';
            }
        }
        // still nothing to type -> the blink.
        if (PendingText != nullptr && PendingText[0] != L'\0')
        {
            // FCOMP 0.0f (the field), TEST AH,0x44, JP -- the jump is taken
            // when not equal (or unordered); fall-through = the elapsed time is exactly 0: the
            // run starts (the routine(0xb)) and takes a typing reference once.
            if (TypeElapsed == 0.0f)
            {
                PlayUiSoundSlotFlag1(0xb);
                if (bTyping == 0)
                {
                    bTyping = 1;
                    G_TextControlTypingCount = G_TextControlTypingCount + 1;
                }
            }
            // FADD float, FSTP float.
            TypeElapsed = static_cast<float>(GetClientFrameTimeCapped() +
                                             static_cast<double>(TypeElapsed));
            // the pending text's length (0 without a buffer).
            int const Length =
                PendingText != nullptr ? static_cast<int>(wcslen(PendingText)) : 0;
            // / 0058b22e..0058b23f: FCOMP +0x36c, TEST AH,0x41 -- JNZ leaves
            // (not greater), the loop runs while greater.
            while (TypeElapsed > TypeInterval)
            {
                // FSUB float, FSTP float; one more character.
                TypeElapsed = TypeElapsed - TypeInterval;
                TypedCount = TypedCount + 1;
                // JL -- below the length keeps typing.
                if (TypedCount >= Length)
                {
                    bTypewriter = 0; // (before the +0x389 test)
                    if (bTyping != 0)
                    {
                        bTyping = 0;
                        G_TextControlTypingCount = G_TextControlTypingCount - 1;
                        if (G_TextControlTypingCount <= 0) // TEST / JG
                            StopUiSoundSlot(0xb);
                        FontStringSetScale(FontString, Width, TextDepth(this));
                        FontStringSetText(FontString, PendingText);
                    }
                }
            }
        }
    }
    else if (bTyping != 0)
    {
        // a run in progress ends when the control is hidden or its
        // Font::String has no child slot; a visible one with children keeps it.
        if ((Flags & 2) == 0 || !bHasChildren)
        {
            bTyping = 0;
            G_TextControlTypingCount = G_TextControlTypingCount - 1;
            if (G_TextControlTypingCount <= 0) // TEST / JG
                StopUiSoundSlot(0xb);
            FontStringSetScale(FontString, Width, TextDepth(this));
            FontStringSetText(FontString, PendingText);
        }
    }

    // the colour blink.
    if (bBlink == 0)
        return;
    // FADD float +0x37c onto the frame time; FST float back; the sum stays
    // on the x87 stack. FCOM 1.0f (the field), TEST AH,0x41, JNZ -- not greater skips.
    double Sum = GetClientFrameTimeCapped() + static_cast<double>(BlinkElapsed);
    BlinkElapsed = static_cast<float>(Sum);
    if (Sum > 1.0)
    {
        // subtract whole seconds on the stack, toggling (SETZ) each time;
        // then FSTP float and the phase byte.
        unsigned char Phase = BlinkPhase;
        do
        {
            Sum = Sum - 1.0;
            Phase = static_cast<unsigned char>(Phase == 0);
        } while (Sum > 1.0);
        BlinkElapsed = static_cast<float>(Sum);
        BlinkPhase = Phase;
    }
    // the override colour (+0x35e) from +0x381 in phase, else +0x385.
    if (BlinkPhase != 0)
        memcpy(OverrideColour, BlinkColourOn, sizeof(OverrideColour));
    else
        memcpy(OverrideColour, BlinkColourOff, sizeof(OverrideColour));
}

void NnTextControl::SetIds(unsigned Ids)
{
    // .
    if (Ids != 0)
    {
        // the routine(the field, Ids, buffer, 0x1000).
        wchar_t Buffer[0x1000];
        unsigned const Count = GetIdsStringWWithRawFallback(Ids, Buffer, 0x1000);
        // The local wide string: "IDS?" (the routine + wcsncpy, 0058a65b..0058a690) when the
        // lookup copied nothing, else the Count characters (the routine, 0058a698).
        std::wstring Temp;
        if (Count == 0)
            Temp.assign(L"IDS?");
        else
            Temp.assign(Buffer, Count);
        FontStringSetScale(FontString, Width, TextDepth(this)); // the routine
        FontStringSetText(FontString, Temp.c_str());            // the routine
        return; // the local is freed
    }
    // an empty string (the field).
    FontStringSetScale(FontString, Width, TextDepth(this));
    FontStringSetText(FontString, L"");
}

void NnTextControl::SetText(wchar_t const* Text)
{
    // 0058a536..0058a555 the routine(Width, +0xb0 + +0x68), 0058a565
    // (Text).
    FontStringSetScale(FontString, Width, TextDepth(this));
    FontStringSetText(FontString, Text);
}

void NnTextControl::SetNumberText(char const* Format)
{
    // 0058a586..0058a5a5 the routine(Width +0x32c, +0xb0 + +0x68), 0058a5b7
    // (0, Format).
    FontStringSetScale(FontString, Width, TextDepth(this));
    FontStringSetNumberText(FontString, Format);
}

void NnTextControl::SetFont(IFont* Font)
{
    // . The kind byte of a child slot is not looked at: every child gets its vt+8.
    if (FontString->FontIface == Font)
        return;
    FontString->FontIface = Font;
    for (unsigned Index = 0;
         FontString->ChildBegin != nullptr &&
         Index < static_cast<unsigned>(FontString->ChildEnd - FontString->ChildBegin);
         ++Index)
    {
        // the argument is re-read from +0x1c each pass; no null test.
        FontString->ChildBegin[Index].Child->BindFont(FontString->FontIface);
    }
    FontString->LayoutGen = 0;
}

void NnTextControl::SetFontByName(char const* StyleName)
{
    // 0058a798 the routine(StyleName); null -> nothing (0058a7a2). The rest,
    // , is the routine's body (same-font early out, +0x1c, every child's
    // vt+8, +0x28 = 0).
    IFont* const Font = FindMenuFontImp(StyleName);
    if (Font == nullptr)
        return;
    SetFont(Font);
}

void NnTextControl::SetWidth(float NewWidth)
{
    // 0058b737 +0x32c = NewWidth; 0058b73d..0058b756 the routine(Font::String,
    // NewWidth, float(+0xb0 + +0x68)).
    Width = NewWidth;
    FontStringSetScale(FontString, Width, TextDepth(this));
}

void NnTextControl::SetCachedDraw(unsigned char bCached, unsigned char bInvalidate)
{
    // .
    Byte395 = bCached;
    if (bInvalidate != 0)
        Byte394 = 0;
}

void NnTextControl::SetPosition(float const* NewPosition)
{
    // . Keep the requested anchor separately; the base WinControl position is
    // the rectangle's upper-left corner when Byte378 is clear.
    memcpy(Field2e0, NewPosition, sizeof(Field2e0));
    bAttached = 1;
    float Attach[3];
    GetAttachPosition(Attach);
    if (Byte378 == 0)
    {
        Attach[0] -= Width * 0.5f;
        Attach[1] += Height * 0.5f;
    }

    float Placed[3] = {NewPosition[0], NewPosition[1], NewPosition[2]};
    if ((PositionFlags & 1) != 0)
    {
        Placed[0] += Attach[0];
        Placed[1] += Attach[1];
        Placed[2] += Attach[2];
    }
    WinControl::SetPosition(Placed); // , direct base call
    if (Width > 0.0f)               // TEST AH,0x41 / JNZ
        FontStringSetScale(FontString, Width, TextDepth(this));
}

void NnTextControl::Draw()
{
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
    // Port-only: owners that walk their children themselves (HudTargetBase, NN_MainButton)
    // reach this without the Win3D draw walk, so the control sets its own HUD shift.
    FWidescreenHudShiftScope const WidescreenScope(WidescreenHudShiftPixelsFor(this), true);
#endif
    // options 6, 9 and 0x13 gate the draw before the child walk.
    if (GetGameOption(kGameOptInterfaceGroup) == 0)
        return;
    if (GetGameOption(kGameOptNeuronet) == 0)
        return;
    if (GetGameOption(kGameOptDrawTextControls) == 0)
        return;

    // the routine -- the children first (no visibility test of its own).
    Win3DDrawChildren(this);

    if (bTypewriter != 0)
    {
        // an empty pending text takes the Font::String's text
        // (the routine with length -1).
        if (PendingText == nullptr || PendingText[0] == L'\0')
        {
            FHudWideText Pending = {PendingText, PendingCapacity};
            WideTextAssign(&Pending, FontString->Text, 0xffffffffu);
            PendingText = Pending.Text;
            PendingCapacity = Pending.Capacity;
        }
        if (PendingText != nullptr && PendingText[0] != L'\0')
        {
            // past 0x3ff characters (unsigned, JBE) the run is cut there:
            // the typewriter goes off and a live run shows the whole text.
            if (static_cast<unsigned>(TypedCount) > 0x3ffu)
            {
                TypedCount = 0x3ff;
                bTypewriter = 0;            // (before the +0x389 test)
                if (bTyping != 0)
                {
                    bTyping = 0;
                    G_TextControlTypingCount = G_TextControlTypingCount - 1;
                    if (G_TextControlTypingCount <= 0) // TEST / JG
                        StopUiSoundSlot(0xb);
                    wchar_t* const Whole = PendingText;
                    float Sum[3];
                    Win3DVec3Add(Sum, Position, Offset); // the routine
                    FontStringSetScale(FontString, Width, Sum[2]);
                    FontStringSetText(FontString, Whole);
                }
            }
            // the first TypedCount characters (wcsncpy into a 0x400-wchar
            // stack buffer, terminated at TypedCount) become the text shown.
            wchar_t Typed[0x400];
            wcsncpy(Typed, PendingText, static_cast<size_t>(TypedCount));
            Typed[TypedCount] = L'\0';
            float const TypedDepth = Position[2] + Offset[2]; // on the three sums
            FontStringSetScale(FontString, Width, TypedDepth);
            FontStringSetText(FontString, Typed);
        }
    }

    // World = Position + Offset (each sum a float, the routine), then the
    // scale at its z.
    float const World[3] = {Position[0] + Offset[0], Position[1] + Offset[1],
                            Position[2] + Offset[2]};
    FontStringSetScale(FontString, Width, World[2]);
    // the routine -- no line, nothing more (and no depth-test call).
    if (FontStringChildCount(FontString) == 0)
        return;
    // no font -> "Normal" (the routine) through the routine's body inline.
    if (FontString->FontIface == nullptr)
        FontStringSetFont(FontString, FindMenuFontImp("Normal"));
    if (FontString->FontIface == nullptr)
    {
        Win3DDisable(kWin3DDepthTest); // -> 0058ad57
        return;
    }
    // FLD z, FCHS, FCOMP 0.001 (double the field), TEST AH,0x5, JNP --
    // taken when -z < 0.001 (C0 alone); equal or unordered fall through. Taken returns without
    // the depth-test call.
    if (-static_cast<double>(World[2]) < 0.001)
        return;

    // the colour (the routine).
    if (bColourOverride != 0)
    {
        unsigned long PackedOverride;
        memcpy(&PackedOverride, OverrideColour, sizeof(PackedOverride));
        FontAtlasSetDrawColor(static_cast<unsigned>(PackedOverride));
    }
    else if (Byte354 != 0)
    {
        FontAtlasSetDrawColor(static_cast<unsigned>(G_NnPaletteArgb_679ba4));
    }
    else if ((Flags & kWin3DActive) == 0)
    {
        FontAtlasSetDrawColor(0xff808080u); // bytes 80 80 80 ff
    }
    else
    {
        FontAtlasSetDrawColor(static_cast<unsigned>(TextColour)); // +0x348
        if (Byte35c != 0)
        {
            // the cursor (the field / the field) onto the near plane of
            // the Win3D camera the field, inline: ((c - centre) / scale) * ZNear, z = -ZNear.
            FSpaceCamera* const Cam = Win3DGetCamera();
            int CursorX = 0;
            int CursorY = 0;
            GetClientSoftwareCursor(&CursorX, &CursorY, nullptr);
            FVector Near;
            Near.X = static_cast<float>(
                (static_cast<double>(CursorX) - static_cast<double>(Cam->ScreenCenterX)) /
                static_cast<double>(Cam->XScale) * static_cast<double>(Cam->ZNear));
            Near.Y = static_cast<float>(
                (static_cast<double>(CursorY) - static_cast<double>(Cam->ScreenCenterY)) /
                static_cast<double>(Cam->YScale) * static_cast<double>(Cam->ZNear));
            Near.Z = -Cam->ZNear;
            // through a copy of the camera orientation (the field)
            // (the routine), then normalised inline (FSQRT, FDIVR 1.0f, three FMUL / FSTP).
            FMatrix CamOrientation;
            memcpy(&CamOrientation, Cam->World, sizeof(CamOrientation));
            FVector Dir;
            Win3DTransformByMatrix(Dir, CamOrientation, Near);
            double const Inv = 1.0 / sqrt(static_cast<double>(Dir.X) * Dir.X +
                                          static_cast<double>(Dir.Y) * Dir.Y +
                                          static_cast<double>(Dir.Z) * Dir.Z);
            float Ray[3];
            Ray[0] = static_cast<float>(Dir.X * Inv);
            Ray[1] = static_cast<float>(Dir.Y * Inv);
            Ray[2] = static_cast<float>(Dir.Z * Inv);
            // this control's own slot 4 with that ray; a hit with +0x380 set
            // draws in the pulse colour (the routine).
            if (HitTest(Ray) && Byte380 != 0)
            {
                unsigned long Pulse = 0;
                NnUiPulseBlendArgb(&Pulse);
                FontAtlasSetDrawColor(static_cast<unsigned>(Pulse));
            }
        }
    }

    // +0x340 = the line count (the routine).
    Field340 = FontStringChildCount(FontString);
    // the draw origin starts all zero; Scale / Height = the routine(Width,
    // Height, z).
    FFontStringDrawOrigin Origin;
    Origin.X = 0.0f;
    Origin.Y = 0.0f;
    Origin.Field10 = 0;
    Origin.Field14 = 0;
    float ScreenWidth = 0.0f;
    float ScreenHeight = 0.0f;
    TextWinProjectExtent(Width, Height, World[2], &ScreenWidth, &ScreenHeight);
    Origin.Scale = ScreenWidth;
    Origin.Height = ScreenHeight;
    if (Byte395 != 0)
    {
        // a different surface width drops the cache.
        if (CachedSurfaceWidth != G_RenderSurfaceWidth)
            Byte394 = 0;
        if (Byte394 == 0)
        {
            // the routine(World) into +0x38c / +0x390; x moves right by
            // half of (box width - text width) (FILD qword of the zero-extended the routine
            // width, FSUBR, FMUL 0.5f, FADD, FSTP float); the surface width is remembered.
            Byte394 = 1;
            TextWinProjectPoint(World, &CachedX, &CachedY);
            int Extent[2] = {0, 0};
            int const* const TextExtent = FontStringGetExtent(FontString, Extent);
            double const TextWidth =
                static_cast<double>(static_cast<unsigned>(TextExtent[0]));
            CachedX = static_cast<float>(
                (static_cast<double>(ScreenWidth) - TextWidth) * 0.5 +
                static_cast<double>(CachedX));
            CachedSurfaceWidth = G_RenderSurfaceWidth;
        }
        // left-aligned at the cached point.
        Origin.Field10 = 0;
        Origin.X = CachedX;
        Origin.Y = CachedY;
    }
    else
    {
        // aligned by +0x358 at the routine(World).
        Origin.Field10 = static_cast<unsigned>(Dword358);
        TextWinProjectPoint(World, &Origin.X, &Origin.Y);
    }
    FontStringDrawChildren(&Origin, FontString,
                           reinterpret_cast<void*>(static_cast<intptr_t>(Field34c)),
                           reinterpret_cast<void*>(static_cast<intptr_t>(Field350)));
    Win3DDisable(kWin3DDepthTest);
}

intptr_t NnTextControl::SetProperty(int Msg, uintptr_t A, uintptr_t B)
{
    // Msg - 6 through the byte table 0058b6e0 (0x4b entries), then 0058b6a8.
    switch (Msg)
    {
    case 6:
    {
        // caseD_6 0058b4af: Offset(+0xa8) = *(float[3])A.
        float const* const NewOffset = reinterpret_cast<float const*>(A);
        Offset[0] = NewOffset[0];
        Offset[1] = NewOffset[1];
        Offset[2] = NewOffset[2];
        // no engine instance (+0x84 == -1) -- done.
        if (MeshHolder.EngineInstance == reinterpret_cast<void*>(static_cast<intptr_t>(-1)))
            return 0;
        float Local[3];
        GetAttachPosition(Local);   // vt+0xa4
        if (Byte378 == 0)
        {
            // x -= Width * 0.5, y += Height * 0.5 (the field).
            Local[0] = Local[0] - Width * 0.5f;
            Local[1] = Height * 0.5f + Local[1];
        }
        // with +0x2f4 bit 0 the position is Local + Offset; without it EDI
        // still points at +0xa8, so the Offset itself is what gets copied.
        float Sum[3];
        float const* Source = Offset;
        if ((PositionFlags & 1) != 0)
        {
            Sum[0] = Local[0] + Offset[0];
            Sum[1] = Local[1] + Offset[1];
            Sum[2] = Local[2] + Offset[2];
            Source = Sum;
        }
        float Copy[3] = {Source[0], Source[1], Source[2]};
        WinControl::SetPosition(Copy); // the routine, a direct call
        return 0;
    }
    case 0xc:
        // caseD_c 0058b629.
        Byte354 = static_cast<unsigned char>(A != 0 ? 1 : 0);
        return 0;
    case 0x1d:
        // caseD_1d 0058b5a8: the wide string A into the Font::String.
        FontStringSetScale(FontString, Width, TextDepth(this));
        FontStringSetText(FontString, reinterpret_cast<wchar_t const*>(A));
        break;
    case 0x1e:
        // caseD_1e 0058b59a: the routine(A).
        SetIds(static_cast<unsigned>(A));
        break;
    case 0x1f:
        // caseD_1f 0058b5fb: the Font::String's text pointer.
        return reinterpret_cast<intptr_t>(FontString->Text);
    case 0x20:
        // caseD_20 0058b60b: its length, 0 without text.
        if (FontString->Text == nullptr)
            return 0;
        return static_cast<intptr_t>(wcslen(FontString->Text));
    case 0x21:
        // caseD_21 0058b660.
        return Field340;
    case 0x22:
        // caseD_22 0058b642.
        Field33c = static_cast<int>(B);
        Field338 = static_cast<int>(A);
        return 0;
    case 0x23:
        // caseD_23 0058b66e.
        Field34c = static_cast<int>(A);
        Field350 = static_cast<int>(B);
        return 0;
    case 0x24:
        // caseD_24 0058b570.
        Byte35c = static_cast<unsigned char>(A != 0 ? 1 : 0);
        return 0;
    case 0x33:
        // caseD_33 0058b489: the override flag is B == 1; the colour is the dword A points at.
        bColourOverride = static_cast<unsigned char>(B == 1 ? 1 : 0);
        memcpy(OverrideColour, reinterpret_cast<void const*>(A), sizeof(OverrideColour));
        return 0;
    case 0x50:
    {
        // caseD_50 0058b3b3.
        if (bTyping != 0)
        {
            bTyping = 0;
            G_TextControlTypingCount = G_TextControlTypingCount - 1;
            if (G_TextControlTypingCount <= 0)
                StopUiSoundSlot(0xb); // the routine(0xb)
            // the pending text goes in whole; the depth is the z of
            // (Position(+0x60), Offset(+0xa8)).
            wchar_t* const Text = PendingText;
            float const Depth = Position[2] + Offset[2];
            FontStringSetScale(FontString, Width, Depth);
            FontStringSetText(FontString, Text);
        }
        // .
        bTypewriter = 0;
        TypedCount = 1;
        TypeElapsed = 0.0f;
        if (PendingText != nullptr && PendingText[0] != L'\0')
            PendingText[0] = L'\0';
        // A == 1 turns the typewriter on with B milliseconds per step
        // (FILD of the zero-extended B, times 0.001f the field).
        if (A == 1)
        {
            bTypewriter = 1;
            TypeInterval = static_cast<float>(static_cast<double>(static_cast<unsigned>(B)) *
                                              static_cast<double>(0.001f));
        }
        return 0;
    }
    case 0x51:
        // caseD_51 0058b385.
        bBlink = 0;
        if (A != 0)
        {
            bBlink = 1;
            bColourOverride = 1;
        }
        return 0;
    default:
        // caseD_7 0058b68c: every id without a case goes to WinControl the routine.
        return WinControl::SetProperty(Msg, A, B);
    }

    // (cases 0x1d and 0x1e): a non-empty pending text is emptied.
    if (PendingText != nullptr && PendingText[0] != L'\0')
        PendingText[0] = L'\0';
    return 0;
}

// ---------------------------------------------------------------------------
// EditControl (vftable 0x005e501c)
// ---------------------------------------------------------------------------

// WINFACTORY<EditControl> (static init the branch: the routine
// "EditControl" + atexit the branch -> the routine). Its Create the routine (vftable
// ) is the template's: operator_new(0x3b0), the ctor, +0x10 = &factory+0xc, Win3D
// slot 9 Create(Parent, Payload); false -> the deleting dtor (flag 1) and 0.
WINFACTORY<NnEditControl> G_EditControlFactory("EditControl");

namespace
{
    wchar_t const kEditCursor[] = L"|";
    float const kEditBlinkSeconds = 0.5f;

    // with Len -1 on a {text, capacity} pair: a null source empties the string
    // (the routine's body); otherwise it grows to wcslen + 1 (the routine's body) and the
    // characters are copied with a terminator. EditControl copies the Font::String's text this
    // way before every change (e.g. 0x0058b80b..0x0058b81e, 0x0058bf3f..0x0058bf56).
    void EditCopyText(FHudWideText* Copy, wchar_t const* Source)
    {
        if (Source == nullptr)
        {
            WideTextClear(Copy);
            return;
        }
        unsigned const Len = static_cast<unsigned>(wcslen(Source));
        WideTextReserve(Copy, Len + 1);
        wcsncpy(Copy->Text, Source, Len);
        Copy->Text[Len] = L'\0';
    }
}

NnEditControl::NnEditControl()
    : CursorTimer(0.0f)
    , bCursorShown(0)
    , MaxLength(-1)
    , bEditable(1)
    , bDigitsOnly(0)
{
    // 0x0058c26f..0x0058c299: +0x3a0 = 0 (a dword store), +0x3a4 = 0,
    // +0x3a8 = -1, +0x3ac = 1, +0x3ad = 0.
    Pad3a5[0] = Pad3a5[1] = Pad3a5[2] = 0;
    Pad3ae[0] = Pad3ae[1] = 0;
}

void NnEditControl::RemoveCursor()
{
    // (thiscall). Nothing without the cursor.
    if (bCursorShown == 0)
        return;

    // a copy of the text (wcslen, the routine, wcsncpy, terminator);
    // one that is non-empty loses its last character (the store only when the capacity is above
    // len - 1, JBE) and goes back through the routine (Width, Offset.z + Position.z) and
    // the routine's two calls.
    FHudWideText Copy = {nullptr, 0};
    EditCopyText(&Copy, FontString->Text);
    if (Copy.Text != nullptr)
    {
        size_t const Len = wcslen(Copy.Text);
        if (static_cast<int>(Len) > 0)
        {
            if (Copy.Capacity > static_cast<unsigned>(Len - 1))
                Copy.Text[Len - 1] = L'\0';
            SetText(Copy.Text);
        }
    }

    bCursorShown = 0;
    WideTextFree(&Copy);
}

void NnEditControl::Update()
{
    // (slot 11). Not editable: straight out (0x0058bdd3 -> 0x0058c01b).
    if (bEditable == 0)
        return;

    // the timer gets the frame time -- stored as a float, compared on the
    // x87 stack (FCOMP 0.5f, TEST AH,1, JNZ: the loop is skipped when less or unordered).
    double const Sum = GetClientFrameTimeCapped() + static_cast<double>(CursorTimer);
    CursorTimer = static_cast<float>(Sum);
    if (Sum >= static_cast<double>(kEditBlinkSeconds))
    {
        // one toggle per 0.5 s owed.
        do
        {
            CursorTimer = static_cast<float>(static_cast<double>(CursorTimer) -
                                             static_cast<double>(kEditBlinkSeconds));
            if (bCursorShown == 0)
            {
                // the text plus '|' (the buffer grown to len + 2 the
                // way; a null text gives just the cursor), shown, flag set.
                FHudWideText Copy = {nullptr, 0};
                EditCopyText(&Copy, FontString->Text);
                unsigned const Len =
                    Copy.Text != nullptr ? static_cast<unsigned>(wcslen(Copy.Text)) : 0;
                WideTextReserve(&Copy, Len + 2);
                wcsncpy(&Copy.Text[Len], kEditCursor, 1);
                Copy.Text[Len + 1] = L'\0';
                SetText(Copy.Text);
                bCursorShown = 1;
                WideTextFree(&Copy);
            }
            else
            {
                // the same removal as the routine (the routine for
                // the copy, the depth through the routine's vector, Position + Offset).
                RemoveCursor();
            }
        } while (CursorTimer >= kEditBlinkSeconds); // FCOMP, TEST AH,1, JZ
    }

    // TextControl's Update the routine.
    NnTextControl::Update();
}

bool NnEditControl::KeyEvent(unsigned Key, unsigned char* Modifiers)
{
    // (slot 20, RET 8). Only an active (Flags bit 0), editable control.
    if ((Flags & 1) == 0 || bEditable == 0)
        return false;

    if (Key == 0x0d && (*Modifiers & 1) != 0)
    {
        // Enter. Without a parent nothing happens (but it is taken).
        if (Parent == nullptr)
            return true;
        RemoveCursor(); // , inline
        CursorTimer = 0.0f;
        WinControl* const Owner = Parent->AsWinControl(); // slot 2
        if (Owner != nullptr)
            Owner->SetProperty(0x45, reinterpret_cast<uintptr_t>(FontString->Text), 1);
        return true;
    }

    if (Key == 0x08)
    {
        // Backspace.
        if ((*Modifiers & 2) != 0)
            return true;
        RemoveCursor(); // , inline
        CursorTimer = 0.0f;

        FHudWideText Copy = {nullptr, 0};
        EditCopyText(&Copy, FontString->Text); // (text, -1)
        int Len = 0;
        if (Copy.Text != nullptr)
        {
            Len = static_cast<int>(wcslen(Copy.Text));
            if (Len > 0)
            {
                if (Copy.Capacity > static_cast<unsigned>(Len - 1))
                    Copy.Text[Len - 1] = L'\0';
                // (Position + Offset) for the depth, the routine, the routine.
                SetText(Copy.Text);
            }
        }
        // the parent is not tested here. What is left of the text goes
        // to its WinControl face; nothing left sends a null text.
        WinControl* const Owner = Parent->AsWinControl();
        if (Len - 1 > 0)
        {
            if (Owner != nullptr)
                Owner->SetProperty(0x45, reinterpret_cast<uintptr_t>(FontString->Text), 0);
        }
        else if (Owner != nullptr)
        {
            Owner->SetProperty(0x45, 0, 0);
        }
        WideTextFree(&Copy);
        return true;
    }

    // the numeric keypad (0x60..0x69) types '0'..'9'. Enter without
    // modifier bit 0 comes here too, as the character 0x0d.
    if (Key != 0x0d && Key >= 0x60 && Key <= 0x69)
        Key -= 0x30;


    if (bDigitsOnly != 0 && (Key < 0x30 || Key > 0x39))
        return false;
    if ((*Modifiers & 2) != 0)
        return true;

    // with a maximum, a text already that long takes nothing more (the
    // copy is measured without a null test, as baseline does).
    if (MaxLength > 0)
    {
        FHudWideText Copy = {nullptr, 0};
        EditCopyText(&Copy, FontString->Text);
        int const Len = static_cast<int>(wcslen(Copy.Text));
        WideTextFree(&Copy);
        if (Len + 1 > MaxLength)
            return true;
    }

    // the key's low byte as a one-byte UTF-8 string (the argument slot
    // reused as the buffer) into at most two wide characters (the routine, the MSLU thunk of
    // MultiByteToWideChar).
    char Narrow[2] = {static_cast<char>(Key & 0xff), '\0'};
    wchar_t Wide[2] = {L'\0', L'\0'}; // [ESP+0x10]
    if (MultiByteToWideChar(CP_UTF8, 0, Narrow, static_cast<int>(strlen(Narrow)), Wide, 2) <= 0)
        return true;


    RemoveCursor();
    CursorTimer = 0.0f;

    // the text plus the character (grown to len + 2 as the routine).
    FHudWideText Copy = {nullptr, 0};
    EditCopyText(&Copy, FontString->Text);
    unsigned const Len = Copy.Text != nullptr ? static_cast<unsigned>(wcslen(Copy.Text)) : 0;
    WideTextReserve(&Copy, Len + 2);
    wcsncpy(&Copy.Text[Len], Wide, 1);
    Copy.Text[Len + 1] = L'\0';

    // shown (the routine with Offset.z + Position.z, the routine),
    // then the parent's WinControl face gets the new text (no parent test).
    SetText(Copy.Text);
    WinControl* const Owner = Parent->AsWinControl();
    if (Owner != nullptr)
        Owner->SetProperty(0x45, reinterpret_cast<uintptr_t>(FontString->Text), 0);
    WideTextFree(&Copy);
    return true;
}

intptr_t NnEditControl::SetProperty(int Msg, uintptr_t A, uintptr_t B)
{
    // (slot 42, RET 0xc).
    switch (Msg)
    {
    case 0x1d:
        // the new text carries no cursor; then TextControl's property.
        bCursorShown = 0;
        return NnTextControl::SetProperty(Msg, A, B); // JMP the routine
    case 0x43:

        if (A == 0)
        {
            bEditable = 0;
            RemoveCursor();
            return 1;
        }
        bEditable = 1;
        if (B != 0) // JBE: unsigned, only zero keeps the old maximum
            MaxLength = static_cast<int>(B);
        return 1;
    case 0x44:

        bDigitsOnly = (A & 1) != 0 ? 1 : 0;
        return 1;
    default:
        return NnTextControl::SetProperty(Msg, A, B); // JMP the routine
    }
}
