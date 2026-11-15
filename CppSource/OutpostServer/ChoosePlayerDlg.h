// Author: Giperion.

// CChoosePlayerDlg — "Choose A Player" (dialog 0x97 in ServerInterface.dll). Lists the names
// of the characters logged in right now; OK (or a double click) opens the CChatAccountDlg of
// the selected character's account. Modeless: baseline keeps one static instance (GChoosePlayerDlg)
// that COutpostServerApp's Chat > Player command (module storage) creates or brings to front.
// baseline: ctor module storage, dtor 00403800, scalar deleting dtor 004038b0, vtable
// , message map module storage (entries 0041c1a0). No RTTI or string names the
// class; the name follows the dialog caption.
#pragma once

#include "FLListBox.h"

class CChoosePlayerDlg : public CDialog
{
public:
	enum { IDD = 0x97 };

	explicit CChoosePlayerDlg(CWnd* Parent = nullptr);

protected:
	virtual void DoDataExchange(CDataExchange* DX);
	virtual BOOL OnInitDialog();
	virtual void OnOK();
	virtual void OnCancel();

	afx_msg void OnDblClkPlayerList();
	afx_msg void OnMeasureItem(int IDCtl, LPMEASUREITEMSTRUCT MeasureInfo);

	DECLARE_MESSAGE_MAP()

	CFLListBox PlayerList;   // +0x60 (CListBox part at +0x64), control 1057
};

// — constructed by the static initializer module storage (CRT XC table
// ), destroyed through the atexit thunk module storage.
extern CChoosePlayerDlg GChoosePlayerDlg;
