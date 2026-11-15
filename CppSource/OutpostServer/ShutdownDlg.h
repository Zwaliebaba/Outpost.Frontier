// Author: Giperion.

// CShutdownDlg — the modeless "Please wait while the server shuts down..." box (dialog 0xa0 in
// ServerInterface.dll). baseline keeps one by value in COutpostServerApp at +0xbac and polls a worker
// thread of the app once a second until it has finished, then closes itself.
// baseline: ctor module storage, vtable module storage, message map module storage.
#pragma once

class CShutdownDlg : public CDialog
{
public:
	enum { IDD = 0xa0 };

	// Which app worker thread the timer waits for (the dword at +0x60).
	enum EWaitThread
	{
		WaitStartupThread = 0,  // COutpostServerApp::PollStartupThread  (module storage)
		WaitShutdownThread = 1, // COutpostServerApp::PollShutdownThread (module storage)
	};

	explicit CShutdownDlg(CWnd* Parent = nullptr);


	void SetWaitThread(int Thread) { WaitThread = Thread; }

protected:
	virtual void DoDataExchange(CDataExchange* DX);
	virtual BOOL OnInitDialog();

	afx_msg void OnTCard(UINT Action, DWORD ActionData);
	afx_msg void OnTimer(UINT_PTR IDEvent);

	DECLARE_MESSAGE_MAP()

	int WaitThread; // +0x60
};
