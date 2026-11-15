// Author: Giperion.

// CChatSystemDlg — "Star System Chat" (dialog 0x95), one modeless window per star system,
// registered while it exists in a global map keyed by system id (module storage). Text typed
// into the input box goes out as system chat; the system's chat arrives through Server.dll's
// g_pSystemChatHook.
// baseline: TU module storage..004172e0. ctor 004161d0 (0x124 bytes), dtor 00416290, scalar
// deleting dtor 00416270, vtable module storage, message map 0041e758 (entries 0041e710).
// Created by CSelectStarSystemDlg::OnOK (module storage), which sets SystemId before Create.
#pragma once

class CChatSystemDlg : public CDialog
{
public:
	enum { IDD = 0x95 };

	explicit CChatSystemDlg(CWnd* Parent = nullptr);
	virtual ~CChatSystemDlg();

	// — the open chat window of a system, or null.
	static CChatSystemDlg* FindBySystem(unsigned int SystemId);

	unsigned int SystemId;       // +0x60, 0xffffffff until the creator sets it

protected:
	virtual void DoDataExchange(CDataExchange* DX);
	virtual BOOL OnInitDialog();
	virtual void OnOK();
	virtual void OnCancel();

	afx_msg void OnMsgFilterInput(NMHDR* Header, LRESULT* Result);
	afx_msg void OnCloseButton();

	// (cdecl) — Server.dll's g_pSystemChatHook, installed by OnInitDialog:
	// appends a system's chat to the log of that system's chat window, if open.
	static void __cdecl OnSystemChat(wchar_t* Text, int SystemId);

	DECLARE_MESSAGE_MAP()

	CRichEditCtrl InputEdit;     // +0x64, control 1047
	CRichEditCtrl LogEdit;       // +0xa4, control 1048
	CRichEditCtrl TitleEdit;     // +0xe4, control 1049
};
