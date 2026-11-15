// Author: Giperion.

// CServerInfoDlg — "Server Information..." (dialog 0x83 in ServerInterface.dll): a resizable
// window holding one tree view (control 1005) that hides itself on close.
// baseline: ctor module storage, dtor 00407560, scalar deleting dtor 00407540, vtable
// , message map module storage (entries 0041c8f0).
// baseline never constructs it: no CALL/JMP reaches the ctor (raw scan of .text), and neither
// the ctor nor the vtable address appears anywhere else in the image; the class is compiled
// in but unused. Its tree-refresh routine module storage (recursive, walks an object through
// vtable slots +0x0c/+0x14/+0x20 and prunes stale items through a local std::set) has no
// caller other than itself, so its argument type cannot be recovered; it is not ported
// (tracker: skipped, dead). Ported for structure; nothing creates the dialog here either.
// No RTTI or string names the class; the name follows the dialog caption.
#pragma once

class CServerInfoDlg : public CDialog
{
public:
	enum { IDD = 0x83 };

	explicit CServerInfoDlg(CWnd* Parent = nullptr);

	CTreeCtrl InfoTree;   // +0x60, control 1005

protected:
	virtual void DoDataExchange(CDataExchange* DX);

	afx_msg void OnClose();
	afx_msg void OnTimer(UINT_PTR IDEvent);
	afx_msg int OnCreate(LPCREATESTRUCT CreateInfo);
	afx_msg void OnSize(UINT Type, int Width, int Height);

	DECLARE_MESSAGE_MAP()
};
