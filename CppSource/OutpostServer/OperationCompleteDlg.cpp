// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

#include "StdAfx.h"
#include "OperationCompleteDlg.h"

// .text:00413d50
COperationCompleteDlg::COperationCompleteDlg(CWnd* Parent)
	: CDialog(IDD, Parent)
{
}

// .text:00413da0 — retail body is RET 4.
void COperationCompleteDlg::DoDataExchange(CDataExchange* DX)
{
	CDialog::DoDataExchange(DX);
}

// .rdata:0041e188: no entries.
BEGIN_MESSAGE_MAP(COperationCompleteDlg, CDialog)
END_MESSAGE_MAP()
