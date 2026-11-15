// Author: Giperion.

#include "StdAfx.h"

#include "ServerInfoDlg.h"

namespace
{
	// SetTimer id and period used by OnCreate (module storage).
	const UINT_PTR InfoTimerId = 0x2000;
	const UINT InfoTimerPeriod = 2000;
}


CServerInfoDlg::CServerInfoDlg(CWnd* Parent)
	: CDialog(IDD, Parent)
{
}


void CServerInfoDlg::DoDataExchange(CDataExchange* DX)
{
	DDX_Control(DX, 1005, InfoTree);
}

// Message map module storage: WM_CLOSE -> 004075f0, WM_TIMER -> 004079c0, WM_CREATE -> 004079d0,
// WM_SIZE -> 00407a00.
BEGIN_MESSAGE_MAP(CServerInfoDlg, CDialog)
	ON_WM_CLOSE()
	ON_WM_TIMER()
	ON_WM_CREATE()
	ON_WM_SIZE()
END_MESSAGE_MAP()

// — closing only hides the window.
void CServerInfoDlg::OnClose()
{
	ShowWindow(SW_HIDE);
}

// — CWnd::OnTimer (the inline Default()); baseline's timer does nothing else.
void CServerInfoDlg::OnTimer(UINT_PTR IDEvent)
{
	Default();
}

// — after the default creation starts a 2-second timer.
int CServerInfoDlg::OnCreate(LPCREATESTRUCT CreateInfo)
{
	if ((int)Default() == -1)
	{
		return -1;
	}
	::SetTimer(m_hWnd, InfoTimerId, InfoTimerPeriod, nullptr);
	return 0;
}

// — keeps the tree's left/top margin on the right and bottom too.
void CServerInfoDlg::OnSize(UINT Type, int Width, int Height)
{
	Default();
	if (!::IsWindow(GetSafeHwnd()) || !::IsWindow(InfoTree.GetSafeHwnd()))
	{
		return;
	}
	RECT Client;
	::GetClientRect(m_hWnd, &Client);
	RECT TreeRect;
	::GetWindowRect(InfoTree.m_hWnd, &TreeRect);
	ScreenToClient(&TreeRect);
	const int TreeHeight = Client.bottom - 2 * TreeRect.top - Client.top;
	const int TreeWidth = Client.right - 2 * TreeRect.left - Client.left;
	InfoTree.SetWindowPos(nullptr, 0, 0, TreeWidth, TreeHeight,
		SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
