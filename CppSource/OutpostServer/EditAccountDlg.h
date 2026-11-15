// Author: Giperion.

// CEditAccountDlg — "Edit Account" (dialog 141): shows one account's name and characters,
// deletes characters and bans / unbans the account. Opened modally by
// CAccountsDlg::OnEditAccount (module storage), which sets Account before DoModal.
// baseline: ctor module storage, dtor 00409300, scalar deleting dtor 004092e0, vtable
// , message map 0041cc00 (entries 0041cb70).
#pragma once

#include "FLListBox.h"

class CAccount;

class CEditAccountDlg : public CDialog
{
public:
	enum { IDD = 141 };

	explicit CEditAccountDlg(CWnd* Parent = nullptr);

protected:
	virtual void DoDataExchange(CDataExchange* DX);
	virtual BOOL OnInitDialog();
	virtual void OnOK();

	afx_msg void OnSelChangeCharacterList();
	afx_msg void OnSelCancelCharacterList();
	afx_msg void OnDeleteCharacter();
	afx_msg void OnMeasureItem(int IDCtl, LPMEASUREITEMSTRUCT MeasureInfo);
	afx_msg void OnKickAccount();

	DECLARE_MESSAGE_MAP()

	CButton BannedCheck;           // +0x60,  control 1044 "Banned"
	CFLListBox CharacterList;      // +0xa0,  control 1040 (CListBox subobject at +0xa4)
	CButton DeleteCharacterButton; // +0x140, control 1041
	CButton Field_0x180;           // +0x180, never DDX-bound or read in baseline
	CRichEditCtrl AccountNameEdit; // +0x1c0, control 1037

public:
	// Set by the creator before DoModal; the baseline ctor leaves it uninitialised.
	CAccount* Account; // +0x200
};
