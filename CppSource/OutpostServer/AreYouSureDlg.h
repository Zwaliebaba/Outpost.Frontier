// Author: Giperion.

// CAreYouSureDlg -- dialog 142 "Are you sure?" (ServerInterface.dll). baseline ctor
// , scalar deleting dtor module storage, vtable module storage, empty message
// map module storage (base CDialog). No members past CDialog.
#pragma once

class CAreYouSureDlg : public CDialog
{
public:
	enum { IDD = 142 };

	CAreYouSureDlg(CWnd* Parent = NULL);

protected:
	virtual void DoDataExchange(CDataExchange* DX);   // (empty: RET 4)

	DECLARE_MESSAGE_MAP()
};
