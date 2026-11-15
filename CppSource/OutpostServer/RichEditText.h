// Author: Giperion.

// Rich-edit text helpers shared by the OutpostServer dialogs (baseline module storage..004098c9,
// all cdecl, UTF-16 through code page 1200).
#pragma once

// EM_GETTEXTEX with GETTEXTEX{cb = BufBytes, GT_DEFAULT, 1200, NULL, NULL};
// returns SendMessage's result.
int GetRichEditText(HWND Window, wchar_t* Buf, int BufBytes);

// EM_SETTEXTEX with SETTEXTEX{ST_DEFAULT, 1200} (replaces the whole text).
void SetRichEditText(HWND Window, const wchar_t* Text);

// moves the caret to the end (WM_GETTEXTLENGTH, EM_SETSEL(len, len)) and
// replaces the selection with EM_SETTEXTEX{ST_SELECTION, 1200}.
void AppendRichEditText(HWND Window, const wchar_t* Text);
