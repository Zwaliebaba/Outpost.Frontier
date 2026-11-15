// Author: Giperion.

// CAccountsDlg — "Manage Accounts & Characters" (dialog 0x93). Lists every account of
// Players (account rows carry the CAccount* as item data, the character rows under them carry
// their 1-based position below the account row) and edits / deletes accounts and characters.
// baseline: ctor module storage, dtor 0040d150, vtable module storage, message map 0041df88.
#pragma once

#include "FLListBox.h"

class CAccount;

class CAccountsDlg : public CDialog
{
public:
	enum { IDD = 0x93 };

	explicit CAccountsDlg(CWnd* Parent = nullptr);

	// — refills the list from Players' account map.
	void RefreshAccountList();
	// — colour (banned red / admin green), bold and italic (logged in) of a row.
	void UpdateAccountStyle(CAccount* Account);

protected:
	virtual void DoDataExchange(CDataExchange* DX);
	virtual BOOL OnInitDialog();

	afx_msg void OnMeasureItem(int IDCtl, LPMEASUREITEMSTRUCT MeasureInfo);
	afx_msg void OnSelChangeList();
	afx_msg void OnEditAccount();
	afx_msg void OnDblClkList();
	afx_msg void OnDeleteCharacter();
	afx_msg void OnDeleteAccount();
	afx_msg HBRUSH OnCtlColor(CDC* DC, CWnd* Wnd, UINT CtlColor);

	DECLARE_MESSAGE_MAP()

	CButton DeleteAccountButton;   // +0x60,  control 3
	CButton Field_0xa0;            // +0xa0,  never DDX-bound or read in baseline
	CButton DeleteCharacterButton; // +0xe0,  control 2
	CStatic BannedLabel;           // +0x120, control 1045
	CButton EditAccountButton;     // +0x160, control 1029
	CFLListBox AccountList;        // +0x1a0, control 1040
};
