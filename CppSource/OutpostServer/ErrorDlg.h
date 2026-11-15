// Author: Giperion.

// CErrorDlg -- dialog 145 "Error" (ServerInterface.dll). baseline ctor module storage (called
// from module storage), scalar deleting dtor module storage, vtable module storage, empty
// message map module storage (base CDialog). No members past CDialog.
#pragma once

class CErrorDlg : public CDialog
{
public:
	enum { IDD = 145 };

	CErrorDlg(CWnd* Parent = NULL);

protected:
	virtual void DoDataExchange(CDataExchange* DX);   // (empty: RET 4)

	DECLARE_MESSAGE_MAP()
};
