// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

// CServerNewsDlg — "Edit Server News" (dialog 0x98): edits the message of the day (GMotd),
// saves motd.dat and hands it to Players.SetMOTD.
// Retail: ctor .text:00413560, dtor 0040d390, vtable .rdata:0041e098, message map 0041e090 (empty).
#pragma once

class CServerNewsDlg : public CDialog
{
public:
	enum { IDD = 0x98 };

	explicit CServerNewsDlg(CWnd* Parent = nullptr);

	CRichEditCtrl NewsEdit; // +0x60, control 1058

protected:
	virtual void DoDataExchange(CDataExchange* DX);
	virtual BOOL OnInitDialog();
	virtual void OnOK();

	DECLARE_MESSAGE_MAP()
};

// .text:00413780 — pushes GMotd into Players under the server lock (called by the app, 0040cc53).
void ApplyServerNews();
