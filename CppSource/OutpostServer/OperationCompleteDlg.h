// Author: Giperion.

// COperationCompleteDlg — "Operation Complete / The password has been set!" (dialog 0x90).
// baseline: ctor module storage, vtable module storage, message map module storage (empty).
#pragma once

class COperationCompleteDlg : public CDialog
{
public:
	enum { IDD = 0x90 };

	explicit COperationCompleteDlg(CWnd* Parent = nullptr);

protected:
	virtual void DoDataExchange(CDataExchange* DX);

	DECLARE_MESSAGE_MAP()
};
