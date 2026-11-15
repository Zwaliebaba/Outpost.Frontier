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
