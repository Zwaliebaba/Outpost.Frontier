// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

// OutpostServer.exe common header. Every OutpostServer translation unit includes this file FIRST:
// MFC must see its own windows.h configuration before any project header pulls windows.h in.
#pragma once

#ifndef VC_EXTRALEAN
#define VC_EXTRALEAN
#endif

#include <sdkddkver.h>

#include <afxwin.h>     // MFC core and standard components
#include <afxext.h>     // MFC extensions (CStatusBar, CToolBar)
#include <afxole.h>     // MFC OLE (COleDocument / COleClientItem used by the rich-edit doc/view)
#include <afxrich.h>    // CRichEditDoc / CRichEditView / CRichEditCntrItem
#include <afxcmn.h>     // common controls (list controls in the dialogs)
#include <afxdlgs.h>
