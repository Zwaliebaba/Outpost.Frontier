// Author: Giperion.

// CChatAccountDlg — "Chat With Account" (dialog 0x94), one modeless window per account,
// registered while it exists in a global map keyed by account name (module storage). Text typed
// into the input box is sent to the account's client as console chat; the account's private
// chat to the server arrives through Server.dll's g_pPrivateChatHook.
// baseline: TU module storage..00415920. ctor 00413fd0 (0x130 bytes), dtor 004140a0, scalar
// deleting dtor 00414080, vtable module storage, message map 0041e2d8 (entries 0041e290).
// Created by CChoosePlayerDlg::OnOK (module storage), which sets AccountName before Create.
#pragma once

#include "server_playerdb.h" // CiWString

class CChatAccountDlg : public CDialog
{
public:
	enum { IDD = 0x94 };

	explicit CChatAccountDlg(CWnd* Parent = nullptr);
	virtual ~CChatAccountDlg();

	// — the open chat window of an account, or null.
	static CChatAccountDlg* FindByName(CiWString& Name);

	CiWString AccountName;       // +0x60

protected:
	virtual void DoDataExchange(CDataExchange* DX);
	virtual BOOL OnInitDialog();
	virtual void OnOK();
	virtual void OnCancel();

	afx_msg void OnMsgFilterInput(NMHDR* Header, LRESULT* Result);
	afx_msg void OnCloseButton();

	// (cdecl) — Server.dll's g_pPrivateChatHook, installed by OnInitDialog:
	// appends a player's private chat to the log of that account's chat window, if open.
	static void __cdecl OnPrivateChat(wchar_t* Text, int ClientId);

	DECLARE_MESSAGE_MAP()

	CRichEditCtrl InputEdit;     // +0x70, control 1047
	CRichEditCtrl LogEdit;       // +0xb0, control 1048
	CRichEditCtrl TitleEdit;     // +0xf0, control 1049
};
