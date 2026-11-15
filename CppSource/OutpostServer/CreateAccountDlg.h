// Author: Giperion.

// CCreateAccountDlg — "Create Account" (dialog 0x8a in ServerInterface.dll): creates an account
// with a password through Players.CreateAccount / SetAccountPassword.
// baseline: ctor module storage, dtor 00407090, scalar deleting dtor 00407070, vtable
// , empty message map module storage (entries 0041c728).
// baseline never constructs it: no CALL/JMP reaches the ctor (raw scan of .text), and neither
// the ctor nor the vtable address appears anywhere else in the image; the class is compiled
// in but unused. Ported for structure; nothing creates it here either.
// No RTTI or string names the class; the name follows the dialog caption.
#pragma once

class CAccount;

class CCreateAccountDlg : public CDialog
{
public:
	enum { IDD = 0x8a };

	explicit CCreateAccountDlg(CWnd* Parent = nullptr);

	CRichEditCtrl NameEdit;       // +0x60, control 1037
	CRichEditCtrl PasswordEdit;   // +0xa0, control 1038
	CAccount* CreatedAccount;     // +0xe0, the new account after a successful OnOK

protected:
	virtual void DoDataExchange(CDataExchange* DX);
	virtual void OnOK();

	DECLARE_MESSAGE_MAP()
};
