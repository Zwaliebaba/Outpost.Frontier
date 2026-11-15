// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

#include "StdAfx.h"

#include "AreYouSureDlg.h"

CAreYouSureDlg::CAreYouSureDlg(CWnd* Parent)
	: CDialog(CAreYouSureDlg::IDD, Parent)
{
}

void CAreYouSureDlg::DoDataExchange(CDataExchange* DX)
{
	CDialog::DoDataExchange(DX);
}

// .rdata:0041c094: no entries.
BEGIN_MESSAGE_MAP(CAreYouSureDlg, CDialog)
END_MESSAGE_MAP()
