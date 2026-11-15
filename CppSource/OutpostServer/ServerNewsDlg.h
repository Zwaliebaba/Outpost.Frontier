// Author: Giperion.

// CServerNewsDlg — "Edit Server News" (dialog 0x98): edits the message of the day (GMotd),
// saves motd.dat and hands it to Players.SetMOTD.
// baseline: ctor module storage, dtor 0040d390, vtable module storage, message map 0041e090 (empty).
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

// — pushes GMotd into Players under the server lock (called by the app, 0040cc53).
void ApplyServerNews();
