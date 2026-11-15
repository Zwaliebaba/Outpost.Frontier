// Author: Giperion.

#pragma once

// — construct the client IMEFocus singleton (pre-init tail 005b1f5f).
// baseline installs the routine (the routine / the branch / the branch) over the
// DLL vtable; the port uses ClientIMEFocus (below) for the same three slots.

#include "imeui.h"

#include <stddef.h>

class FontStringChild;

#pragma pack(push, 4)
// Client-side IMEFocus — listing the routine. baseline operator_new(0x1c).
struct ClientIMEFocus : public IMEFocus
{
    explicit ClientIMEFocus(HWND Hwnd);
    ~ClientIMEFocus() override;

    void DrawRect(int X1, int Y1, int X2, int Y2, DWORD Color) override;
    void DrawTextW(const wchar_t* Text, unsigned Color, void* Layout,
                   unsigned Count) override;

    FontStringChild* ConfirmText; // baseline wrapper +0x18
};
#pragma pack(pop)

static_assert(sizeof(IMEFocus) == 0x18, "IMEFocus base 0x18");
static_assert(offsetof(ClientIMEFocus, ConfirmText) == 0x18, "Confirm at +0x18");
static_assert(sizeof(ClientIMEFocus) == 0x1c, "client wrapper 0x1c");

IMEFocus* EnsureClientIMEFocus();

// (cdecl) -- delete the IMEFocus the routine stored in the field and zero it.
// Caller: the client shutdown 0x005b2349, after StringShutdown and before CoUninitialize.
void ReleaseClientIMEFocus();

class IFont;

// "ConfirmPlayerText" (the routine), fetched on first use at every site
// (ClientIMEFocus ctor, FLImeInputBox ctor 0x00564296 / 0x005642ff / 0x0056433e).
IFont* ClientConfirmPlayerFont();

// / the field -- the CRCs of "IMEbkground" / "IMEtext" (static initializers
// / the branch).
unsigned long ClientImeBackgroundKey();
unsigned long ClientImeTextKey();

// swap the font atlas material key; the previous one goes to OutPrev.
void ClientImeSwapFontMaterialKey(unsigned* OutPrev, unsigned NewKey);
