// Author: Giperion.

// CSelectStarSystemDlg — "Select A Star System" (dialog 0x96 in ServerInterface.dll). Lists
// the star systems that have at least one player in them; OK (or a double click) opens the
// CChatSystemDlg of the selected system. Modeless: baseline keeps one static instance
// (GSelectStarSystemDlg) that COutpostServerApp's Chat > Star System command (module storage)
// creates or brings to front.
// baseline: ctor module storage, dtor 00406210, scalar deleting dtor 004062c0, vtable
// , message map module storage (entries 0041c4e8). No RTTI or string names the
// class; the name follows the dialog caption.
#pragma once

#include "FLListBox.h"

class CSelectStarSystemDlg : public CDialog
{
public:
	enum { IDD = 0x96 };

	explicit CSelectStarSystemDlg(CWnd* Parent = nullptr);

protected:
	virtual void DoDataExchange(CDataExchange* DX);
	virtual BOOL OnInitDialog();
	virtual void OnOK();
	virtual void OnCancel();

	afx_msg void OnDblClkSystemList();
	afx_msg void OnMeasureItem(int IDCtl, LPMEASUREITEMSTRUCT MeasureInfo);

	DECLARE_MESSAGE_MAP()

	CFLListBox SystemList;   // +0x60 (CListBox part at +0x64), control 1040; item data = system id
};

// — constructed by the static initializer module storage (CRT XC table
// ), destroyed through the atexit thunk module storage.
extern CSelectStarSystemDlg GSelectStarSystemDlg;
