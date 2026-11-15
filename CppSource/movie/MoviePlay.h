// Author: Giperion.

#pragma once

#include <windows.h>

// . Builds the filter graph with WM ASF Reader, calls Run, returns while playback continues.
long __cdecl BuildAndRunWmAsfGraph(HWND Hwnd, int Fullscreen, const char* Path);

// . Logs when Code < 0. Returns true when Code < 0.
bool __cdecl Movie_LogNegativeCode(long Code, const char* File, int Line);

// . Returns the playback flag.
bool __cdecl Movie_IsPlaying();

// . Releases the COM pointers and restores the window procedure.
void __cdecl Movie_ReleaseComAndRestoreWndProc();

// . Empty.
void __cdecl Movie_Nop();
