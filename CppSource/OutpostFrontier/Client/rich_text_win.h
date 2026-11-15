// Author: Giperion.

#pragma once

// baseline BaseRichTextWin / RichTextWin (Client\...\RichDisplay.cpp,
// RTTI .?AVBaseRichTextWin@@ : WinControl, .?AVRichTextWin@@ : BaseRichTextWin).

// A WinControl that lays a RenderDisplayList out into a GDI memory DC through a DeviceTRC,
// copies the 16-bit DIB into an IBigImage2 and draws that image over the control's projected
// screen rectangle.

// BaseRichTextWin: ctor the routine, dtor the routine (scalar deleting the routine),
// vftable 0x005e459c (47 slots) + IDataViewOwner face 0x005e4590. Overrides of WinControl:
//   0 the routine, 4 the branch HitTest, 5 the routine SetPosition, 9 the routine Create,
//   11 the routine Update, 12 the branch UpdateInactive, 13 the routine Draw,
//   23 the routine Slot23, 42 the branch SetProperty;
// own slots 45 the routine Relayout (MOV AL,1) and 46 the branch GetToolTipHoverInfo.
// Direct subclasses: RichTextWin, UIFastTable (0x005e511c).

// RichTextWin: ctor the routine, dtor the routine (scalar deleting the routine),
// vftable 0x005e476c (48 slots) + face 0x005e4760. Overrides: 0 the routine,
// 9 the routine Create, 11 the routine Update, 45 the branch Relayout;
// own slot 47 the routine CreateWithText.
// Subclasses: RichTextEditWin (0x005e4844), MsgWin (0x005cffc4), UIRichTextWin (0x005e58d4),
// UITextMsgButton (0x005e5af4).

// BaseRichTextWin layout (after WinControl's 0x32c):
//   +0x32c DebugWindow* -- only the ctor (0) and the dtor/Create null tests touch it; the
//          DebugWindow vftable 0x005e465c is installed nowhere but in the dtor (0x00579664),
//          so no DebugWindow is ever built and every branch on it is dead
//   +0x330 / +0x334 width / height in the control's local units (Create: 0.4 / 0.3)
//   +0x338..+0x344 the two corners (0,0,0) and (w,-h,0) in world space (the routine)
//   +0x348..+0x354 their screen rectangle left, top, right, bottom
//   +0x358 / +0x35c pixel width / height (right - left, bottom - top)
//   +0x360 / +0x364 scroll x / y in pixels
//   +0x368 RenderDisplayList -- constructed and destroyed only; no BaseRichTextWin or
//          RichTextWin code reads it
//   +0x374 RenderDisplayList -- LayoutRichText output, executed by Draw
//   +0x380 laid-out height in pixels
//   +0x384 DeviceTRC (0x50)
//   +0x3d4 GDI surface {HDC, HBITMAP, previous object, width, height} (the routine frees it)
//   +0x3e8 DIB pitch, +0x3ec IBigImage2*
//   +0x3f0 vertical UIScroll ("ScrollV_%d"), +0x3f4 horizontal UIScroll (never created here)
//   +0x3f8 scrollbar width 0.02f, +0x3fc scrollbar gap 0.002f
//   +0x400 layout dirty, +0x401 surface dirty, +0x402 draw the debug frame,
//   +0x403 frame RGBA (the field: 00 ff 00 ff), +0x407 automatic vertical scrollbar
//   +0x408 vertical alignment 0..3
//   +0x40c FToolTipHoverInfo (0x64), key (+0x40c) = this
//   +0x470 feed the ToolTip on hover, +0x471 never hit, +0x472 skip the image upload
//   +0x474 image alpha (1.0f), +0x478 keep scrolled to the end
//   +0x47c attach offset[3] (added to every SetPosition)
//   size 0x488
// RichTextWin adds +0x488 RenderDisplayList (the source), +0x494 LayoutRichText's last
// argument (ctor 1); size 0x498.

#include "event_handler.h"
#include "tooltip_hover.h"
#include "wincontrol_base.h"

#include "common_text.h"
#include "render/IBigImage2.h"

#include <set>
#include <stddef.h>
#include <stdint.h>
#include <vector>
#include <windows.h>

class UiScroll;
struct FLImeInputBox;

// RTTI .?AVDebugWindow@@, vftable 0x005e465c -- never constructed (see +0x32c above).
struct DebugWindow;

// BaseRichTextWin +0x3d4 -- the memory DC, its bitmap and the object it replaced.
struct FRichTextSurface
{
    HDC Dc;                  // +0x00
    HBITMAP Bitmap;          // +0x04
    HGDIOBJ PreviousObject;  // +0x08
    int Width;               // +0x0c
    int Height;              // +0x10
};

class BaseRichTextWin : public WinControl
{
public:

    BaseRichTextWin();

    ~BaseRichTextWin() override;

    // slot 4 the branch
    bool HitTest(float const* Ray) override;
    // slot 5 the routine
    void SetPosition(float const* NewPosition) override;
    // slot 9 the routine
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 11 the routine
    void Update() override;
    // slot 12 the branch
    void UpdateInactive() override;
    // slot 13 the routine
    void Draw() override;
    // slot 23 the routine -- 0xf005 from the scrollbars
    bool Slot23(unsigned Msg, unsigned A, unsigned B) override;
    // slot 42 the branch -- property 6 sets the attach offset
    intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override;

    // slot 45 (+0xb4) the routine -- MOV AL,1
    virtual bool Relayout();
    // slot 46 (+0xb8) the branch -- &+0x40c
    virtual FToolTipHoverInfo* GetToolTipHoverInfo();

    // (RET 4)
    void SetDrawFrame(bool bDraw);
    // (RET 4)
    void SetFrameColor(unsigned long const* Rgba);
    // (RET 4)
    void SetNeverHit(bool bNever);
    // (RET 4)
    void SetToolTipEnabled(bool bEnabled);
    // (RET 8)
    void SetScrollOrigin(int ScrollX, int ScrollY);
    // (RET 8)
    void GetScrollOrigin(int* OutScrollX, int* OutScrollY);
    // (RET 4)
    void SetAutoScrollbar(bool bAuto);

    void ScrollToEnd();

    void ScrollToTop();
    // (RET 4)
    void SetKeepScrolledToEnd(bool bKeep);
    // (RET 4)
    void SetAlignMode(int Mode);
    // (RET 4)
    void SetSkipUpload(bool bSkip);
    // build the "ScrollV_%d" UIScroll beside the text.
    UiScroll* CreateScrollBarV();
    // (RET 4)
    void ShowScrollBarV(bool bShow);

    float GetScrollBarGap();
    // (RET 4)
    void SetAttachOffset(float const* NewOffset);
    // (RET 8)
    void SetNormSize(float Width, float Height);
    // project the control, then (re)build the DC, bitmap and image.
    void EnsureSurface();

    DeviceTRC* GetDeviceTRC();
    // (RET 8)
    void MarkDirty(bool bSurface, bool bLayout);

    void RebuildLayoutIfDirty();

    DebugWindow* DebugWin;              // +0x32c
    float NormWidth;                    // +0x330
    float NormHeight;                   // +0x334
    float WorldRect[4];                 // +0x338
    int ScreenRect[4];                  // +0x348
    int PixelWidth;                     // +0x358
    int PixelHeight;                    // +0x35c
    int ScrollX;                        // +0x360
    int ScrollY;                        // +0x364
    RenderDisplayList List368;          // +0x368
    RenderDisplayList LayoutList;       // +0x374
    int LayoutHeight;                   // +0x380
    DeviceTRC Trc;                      // +0x384
    FRichTextSurface Surface;           // +0x3d4
    int Pitch;                          // +0x3e8
    IBigImage2* Image;                  // +0x3ec
    UiScroll* ScrollBarV;               // +0x3f0
    UiScroll* ScrollBarH;               // +0x3f4
    float ScrollBarWidth;               // +0x3f8
    float ScrollBarGap;                 // +0x3fc
    unsigned char bLayoutDirty;         // +0x400
    unsigned char bSurfaceDirty;        // +0x401
    unsigned char bDrawFrame;           // +0x402
    unsigned char FrameColor[4];        // +0x403
    unsigned char bAutoScrollbar;       // +0x407
    int AlignMode;                      // +0x408
    FToolTipHoverInfo HoverInfo;        // +0x40c
    unsigned char bToolTipEnabled;      // +0x470
    unsigned char bNeverHit;            // +0x471
    unsigned char bSkipUpload;          // +0x472
    unsigned char Pad473;               // +0x473
    float Alpha;                        // +0x474
    unsigned char bKeepScrolledToEnd;   // +0x478
    unsigned char Pad479[3];            // +0x479
    float AttachOffset[3];              // +0x47c

protected:
    // The ToolTip half of slots 11 / 12 (0x0057b0e6..0x0057b122).
    void FeedToolTipIfHovered();

private:
    BaseRichTextWin(BaseRichTextWin const&) = delete;
    BaseRichTextWin& operator=(BaseRichTextWin const&) = delete;
};

static_assert(sizeof(BaseRichTextWin) == 0x488, "baseline BaseRichTextWin is 0x488 bytes");
static_assert(offsetof(BaseRichTextWin, NormWidth) == 0x330, "BaseRichTextWin +0x330");
static_assert(offsetof(BaseRichTextWin, WorldRect) == 0x338, "BaseRichTextWin +0x338");
static_assert(offsetof(BaseRichTextWin, ScreenRect) == 0x348, "BaseRichTextWin +0x348");
static_assert(offsetof(BaseRichTextWin, PixelWidth) == 0x358, "BaseRichTextWin +0x358");
static_assert(offsetof(BaseRichTextWin, List368) == 0x368, "BaseRichTextWin +0x368");
static_assert(offsetof(BaseRichTextWin, LayoutList) == 0x374, "BaseRichTextWin +0x374");
static_assert(offsetof(BaseRichTextWin, Trc) == 0x384, "BaseRichTextWin +0x384");
static_assert(offsetof(BaseRichTextWin, Surface) == 0x3d4, "BaseRichTextWin +0x3d4");
static_assert(offsetof(BaseRichTextWin, Image) == 0x3ec, "BaseRichTextWin +0x3ec");
static_assert(offsetof(BaseRichTextWin, ScrollBarV) == 0x3f0, "BaseRichTextWin +0x3f0");
static_assert(offsetof(BaseRichTextWin, bLayoutDirty) == 0x400, "BaseRichTextWin +0x400");
static_assert(offsetof(BaseRichTextWin, FrameColor) == 0x403, "BaseRichTextWin +0x403");
static_assert(offsetof(BaseRichTextWin, AlignMode) == 0x408, "BaseRichTextWin +0x408");
static_assert(offsetof(BaseRichTextWin, HoverInfo) == 0x40c, "BaseRichTextWin +0x40c");
static_assert(offsetof(BaseRichTextWin, bToolTipEnabled) == 0x470, "BaseRichTextWin +0x470");
static_assert(offsetof(BaseRichTextWin, Alpha) == 0x474, "BaseRichTextWin +0x474");
static_assert(offsetof(BaseRichTextWin, AttachOffset) == 0x47c, "BaseRichTextWin +0x47c");

class RichTextWin : public BaseRichTextWin
{
public:

    RichTextWin();

    ~RichTextWin() override;

    // slot 9 the routine
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 11 the routine
    void Update() override;
    // slot 45 the branch -- LayoutRichText(+0x384, +0x488, width - 4, +0x374, 2, 0, 0,
    // &+0x380, +0x494)
    bool Relayout() override;
    // slot 47 (+0xbc) the routine (RET 0x10) -- Create, the size, then one TextNode.
    virtual void CreateWithText(Win3D* NewParent, wchar_t const* Text, float Width,
                                float Height);


    void ClearContent();
    // (RET 4) -- append a clone of every node of Source.
    void AppendContent(RenderDisplayList const& Source);
    // (RET 4) -- append a clone of Node.
    void AppendNode(RDLNode const& Node);
    // (RET 4)
    void SetLayoutFlag(bool bFlag);

    RenderDisplayList Content;          // +0x488
    unsigned char LayoutFlag;           // +0x494
    unsigned char Pad495[3];            // +0x495

private:
    RichTextWin(RichTextWin const&) = delete;
    RichTextWin& operator=(RichTextWin const&) = delete;
};

static_assert(sizeof(RichTextWin) == 0x498, "UIRichTextWin allocates 0x498 (module state)");
static_assert(offsetof(RichTextWin, Content) == 0x488, "RichTextWin +0x488");
static_assert(offsetof(RichTextWin, LayoutFlag) == 0x494, "RichTextWin +0x494");

// baseline RichTextEditWin (RTTI .?AVRichTextEditWin@@ : RichTextWin, Event::Handler), the
// editable rich text box. ctor the routine, scalar deleting dtor the routine, vftable
// (53 slots) + IDataViewOwner face 0x005e4838 + Event::Handler face 0x005e4830 at
// +0x498 (one slot, the branch). Overrides of RichTextWin: 0 the routine,
// 11 the routine Update, 13 the routine Draw, 19 the routine MouseEvent; own slots
// 48 the routine, 49 the routine, 50 the routine, 51 / 52 the routine (RET).
// Subclasses: UITextEdit (0x005e5bd4), ChatTextInputDisplay (0x005cfec4).
// The factory is WINFACTORY<RichTextEditWin> "RichTextEditWin" (the field).

// How it edits: the text is a vector of 0xc-byte cells (+0x4c4) -- a character and the text
// attributes it was typed with. Every change rebuilds the source list (+0x488) as two
// references, one to the prefix list (+0x4dc) and one to the generated text list (+0x4e8), which
// refills: a TRANode at every attribute change (or one reference to +0x4fc when the
// fixed-style flag +0x4f8 is set) and a TextNode per run ('*' each in password mode). Keys and
// characters arrive through the Event::Handler face (+0x498) as events 1 and 2 while the box is
// the input focus (Activate the routine); an IME box (FLImeInputBox, +0x52c) exists only while
// an IMEFocus singleton does.

// The baseline layout is kept in the comments; the port holds the containers as STL members (no
// baseline ABI), so the object is larger than baseline's 0x530.
//   +0x498 Event::Handler, +0x49c caret, +0x4a0 / +0x4a4 (see below), +0x4a8 4,
//   +0x4ac caret shown, +0x4b0 caret timer, +0x4b4 caret period (0.5f), +0x4b8 caret RGBA,
//   +0x4bc caret height scale (1.0f), +0x4c0 editable, +0x4c4 cells, +0x4d4 current attributes
//   (TRA the field, static init the branch: 00 ff ff ff), +0x4dc prefix list, +0x4e8 text
//   list, +0x4f4 allowed formatting, +0x4f8 fixed style, +0x4fc fixed-style list, +0x508
//   password mode, +0x50c case mode, +0x510 max length, +0x514 set<wchar_t> filter,
//   +0x528 replacement character ('_'), +0x52a filter is an allow list, +0x52b IME enabled,
//   +0x52c FLImeInputBox*.

// +0x4c4 element (0xc bytes in baseline: the character, two bytes of padding and an 8-byte
// TextRenderAttributes of which every copy moves only the first dword).
struct FRichEditCell
{
    wchar_t Ch;                     // +0x00
    TextRenderAttributes Attr;      // +0x04
};

class RichTextEditWin : public RichTextWin, public EventHandler
{
public:

    RichTextEditWin();
    // (scalar deleting the routine)
    ~RichTextEditWin() override;

    // slot 11 the routine -- the source list animated, the ToolTip feed, Win3D's child walk,
    // then the caret blink.
    void Update() override;
    // slot 13 the routine -- BaseRichTextWin's draw, the IME box resized, then the caret line.
    void Draw() override;
    // slot 19 the branch -- a parent takes the event, else WinControl's the routine
    bool MouseEvent(FWin3DMouseEvent const* Event) override;
    // slot 48 (+0xc0) the routine (RET 0xc) -- Create (slot 9) with no data, then the size.
    virtual void CreateSized(Win3D* NewParent, float Width, float Height);
    // slot 49 (+0xc4) the routine (RET 4) -- event 1, a key.
    virtual bool OnKeyDown(FHudCmdRecord const* Record);
    // slot 50 (+0xc8) the branch (RET 4) -- event 2, a character.
    virtual bool OnChar(FHudCmdRecord const* Record);
    // slot 51 (+0xcc) the routine -- RET; Activate's tail call.
    virtual void OnActivated();
    // slot 52 (+0xd0) the routine -- RET; Deactivate's first call.
    virtual void OnDeactivating();

    // Event::Handler slot 0 the routine (RET 4, this = the +0x498 face).
    bool OnEvent(unsigned MsgId) override;

    // (RET 4) -- a negative count is ignored.
    void SetMaxLength(int MaxChars);
    // (RET 4) -- +0x4f8; a change rebuilds the display.
    void SetFixedStyle(bool bFixed);
    // (RET 4) -- +0x508; a change rebuilds the display.
    void SetPasswordMode(bool bPassword);
    // (RET 4) -- +0x52b, and the IME box built or destroyed with it.
    void SetImeEnabled(bool bEnabled);
    // (RET 4)
    void SetEditable(bool bNewEditable);
    // every cell dropped, caret 0, display rebuilt, the IME box rebuilt.
    void ClearText();
    // (RET 8) -- replace the text; Attr null means the current attributes.
    void SetText(wchar_t const* Text, TextRenderAttributes const* Attr);
    // (RET 8) -- copy the first min(count, MaxChars - 1) characters and a
    // terminator. Returns the number written including the terminator, or the cell count when
    // MaxChars <= 0 (then nothing is written).
    int GetText(wchar_t* Buffer, int MaxChars);
    // shown, input focus, events 1 / 2 (and 0xf while read-only), IME focus.
    void Activate();
    // (RET 4)
    void Deactivate(bool bHide);
    // (RET 4) -- 0 as typed, 1 upper case, 2 lower case.
    void SetCaseMode(int Mode);
    // (RET 4)
    void SetFilterIsAllowList(bool bAllowList);
    // (RET 8) -- insert Count characters into the filter set, last one first.
    void AddFilterChars(wchar_t const* Chars, int Count);
    // (RET 4)
    void SetReplacementChar(wchar_t Ch);

    void RebuildDisplay();
    // (RET 4) -- clamp to 0..count, then take the attributes of the cell before it.
    void SetCaret(int Position);

    int Caret;                          // +0x49c
    // +0x4a0 / +0x4a4 -- always written together with the caret (SetText, ClearText); every edit
    // path requires them equal. TODO: meaning (a selection is the obvious reading, unproven).
    int Field_0x4a0;                    // +0x4a0
    int Field_0x4a4;                    // +0x4a4
    int Field_0x4a8;                    // +0x4a8 -- ctor 4; nothing in this class reads it
    unsigned char bCaretVisible;        // +0x4ac
    float CaretTimer;                   // +0x4b0
    float CaretPeriod;                  // +0x4b4
    unsigned char CaretColor[4];        // +0x4b8 -- RGBA for Win3DColor4ubv
    float CaretHeightScale;             // +0x4bc
    unsigned char bEditable;            // +0x4c0
    std::vector<FRichEditCell> Cells;   // +0x4c4
    TextRenderAttributes CurrentAttr;   // +0x4d4
    RenderDisplayList Prefix;           // +0x4dc
    RenderDisplayList TextDisplay;      // +0x4e8
    int FormatMask;                     // +0x4f4 -- bit 0 bold, 1 italic, 2 underline, 0xf8 size
    unsigned char bFixedStyle;          // +0x4f8
    RenderDisplayList FixedStyle;       // +0x4fc
    unsigned char bPasswordMode;        // +0x508
    int CaseMode;                       // +0x50c
    int MaxLength;                      // +0x510 -- 0 = no limit
    std::set<wchar_t> FilterChars;      // +0x514
    wchar_t ReplacementChar;            // +0x528
    unsigned char bFilterIsAllowList;   // +0x52a
    unsigned char bImeEnabled;          // +0x52b
    FLImeInputBox* ImeBox;              // +0x52c

protected:
    // (RET 8) -- a TextNode of Count cells from Start ('*' each in password mode)
    // appended to the text list.
    void AppendTextRun(int Start, int Count);
    // The bounded advance every edit spells inline: begin() moved Position cells, stopping at
    // end(); a Position <= 0 stays at begin().
    int ClampedCellIndex(int Position) const;

private:
    RichTextEditWin(RichTextEditWin const&) = delete;
    RichTextEditWin& operator=(RichTextEditWin const&) = delete;
};
