// Author: Giperion.

// CServerConfigDlg — "Server Configuration" (dialog 0x84 in ServerInterface.dll): server name,
// description, password, player limit and three options. OnInitDialog loads the last settings
// from OutpostServer.cfg in the local user directory; OnOK validates them, hands them to the app's
// FServerControl and writes OutpostServer.cfg back.
// baseline: the only instance is a local of COutpostServerApp::InitInstance, built inline there
// (module storage: CDialog::CDialog(0x84, NULL), vtable module storage stored at 0040bd7b);
// dtor module storage (a bare JMP CDialog::~CDialog), scalar deleting dtor 0040be60. The class
// has no message map of its own (vtable +0x30 is MFC's CDialog::GetMessageMap) and its
// DoDataExchange slot is the ICF-folded empty `RET 4` at module storage. No RTTI or string names
// the class; the name follows the dialog caption.
#pragma once

class CServerConfigDlg : public CDialog
{
public:
	enum { IDD = 0x84 };

	explicit CServerConfigDlg(CWnd* Parent = nullptr);

protected:
	virtual void DoDataExchange(CDataExchange* DX);
	virtual BOOL OnInitDialog();
	virtual void OnOK();

	// Controls fetched with GetDlgItem by OnInitDialog (MFC temporary wrappers, as baseline).
	CWnd* MaxPlayersSpin;         // +0x60, control 1011 (msctls_updown32)
	CWnd* ServerNameEdit;         // +0x64, control 1006 (RICHEDIT20W)
	CWnd* DescriptionEdit;        // +0x68, control 1007 (RICHEDIT20W)
	CWnd* PasswordEdit;           // +0x6c, control 1008 (RICHEDIT20W, IME disabled)
	CWnd* MaxPlayersEdit;         // +0x70, control 1012
	CWnd* InternetCheck;          // +0x74, control 1014 "Make Your Server Internet Accessible"
	CWnd* AllowNewPlayersCheck;   // +0x78, control 1010 "Allow New Players"
	CWnd* PvpCheck;               // +0x7c, control 1059 "Allow Players to Harm Other Players"
	CWnd* LegalNotice;            // +0x80, control 1070
};
