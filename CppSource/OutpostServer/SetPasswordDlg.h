// Author: Giperion.

// CSetPasswordDlg — "Set Password" (dialog 0x8f): sets an account's password through
// Players.SetAccountPassword, then shows COperationCompleteDlg.
// baseline: ctor module storage, dtor 00415b80, vtable module storage, message map 0041e4e8 (empty).
#pragma once

class CAccount;

class CSetPasswordDlg : public CDialog
{
public:
	enum { IDD = 0x8f };

	explicit CSetPasswordDlg(CWnd* Parent = nullptr);

	CRichEditCtrl PasswordEdit; // +0x60, control 1038
	CAccount* Account;          // +0xa0, set by the creator before DoModal

protected:
	virtual void DoDataExchange(CDataExchange* DX);
	virtual void OnOK();

	DECLARE_MESSAGE_MAP()
};
