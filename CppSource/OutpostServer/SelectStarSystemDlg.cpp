// Author: Giperion.

#include "StdAfx.h"

#include "SelectStarSystemDlg.h"
#include "ChatSystemDlg.h"
#include "StarSystemList.h"

namespace
{
	// Control id of the list box (ServerInterface.dll dialog 150).
	const int SystemListId = 1040;
}

// (static initializer: ctor with a null parent, then atexit(00406200)).
CSelectStarSystemDlg GSelectStarSystemDlg;


CSelectStarSystemDlg::CSelectStarSystemDlg(CWnd* Parent)
	: CDialog(IDD, Parent)
{
}

// — baseline's null test on the list pointer is the compiler's adjustment from
// the CFLListBox (+0x60) to its CWnd part (+0x64).
void CSelectStarSystemDlg::DoDataExchange(CDataExchange* DX)
{
	DDX_Control(DX, SystemListId, SystemList);
}

// Message map module storage: LBN_DBLCLK 1040 -> 00406450, WM_MEASUREITEM -> 00406460.
BEGIN_MESSAGE_MAP(CSelectStarSystemDlg, CDialog)
	ON_LBN_DBLCLK(SystemListId, &CSelectStarSystemDlg::OnDblClkSystemList)
	ON_WM_MEASUREITEM()
END_MESSAGE_MAP()

// — a double click is the virtual OnOK (vtable +0xcc).
void CSelectStarSystemDlg::OnDblClkSystemList()
{
	OnOK();
}

// — fills the list with every system that has at least one player.
BOOL CSelectStarSystemDlg::OnInitDialog()
{
	CDialog::OnInitDialog();
	FillStarSystemList(1, &SystemList);
	return TRUE;
}

// — the dialog is modeless: Cancel destroys the window (vtable +0x60).
void CSelectStarSystemDlg::OnCancel()
{
	DestroyWindow();
}

// — opens (or brings to front) the chat window of the selected system (the
// row's item data), then destroys this window; with no selection only the window is destroyed.
// baseline takes no server lock here.
void CSelectStarSystemDlg::OnOK()
{
	const int Selection = (int)::SendMessage(SystemList.m_hWnd, LB_GETCURSEL, 0, 0);
	if (Selection > -1)
	{
		const unsigned int SystemId =
			(unsigned int)::SendMessage(SystemList.m_hWnd, LB_GETITEMDATA, Selection, 0);
		CChatSystemDlg* Chat = CChatSystemDlg::FindBySystem(SystemId);
		if (Chat == nullptr)
		{
			Chat = new CChatSystemDlg(nullptr);
			Chat->SystemId = SystemId;
			Chat->Create(CChatSystemDlg::IDD, nullptr);
			Chat->ShowWindow(SW_SHOW);
		}
		else
		{
			Chat->ShowWindow(SW_SHOW);
			::BringWindowToTop(Chat->m_hWnd);
		}
	}
	DestroyWindow();
}

// — list rows are as tall as the list's regular font.
void CSelectStarSystemDlg::OnMeasureItem(int IDCtl, LPMEASUREITEMSTRUCT MeasureInfo)
{
	if (IDCtl != SystemListId)
	{
		CDialog::OnMeasureItem(IDCtl, MeasureInfo);
		return;
	}
	CDC MeasureDC;
	MeasureDC.Attach(::CreateCompatibleDC(nullptr));
	CFont* OldFont = MeasureDC.SelectObject(&SystemList.RegularFont);
	TEXTMETRICA Metrics;
	MeasureDC.GetTextMetrics(&Metrics);
	MeasureDC.SelectObject(OldFont);
	MeasureInfo->itemHeight = Metrics.tmHeight;
}
