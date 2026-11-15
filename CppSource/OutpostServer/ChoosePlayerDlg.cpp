// Author: Giperion.

#include "StdAfx.h"

#include "ChoosePlayerDlg.h"
#include "ChatAccountDlg.h"
#include "OutpostServer.h"          // LockServer / UnlockServer
#include "server_playerdb.h"   // Players, CAccount, CiWString

namespace
{
	// Control id of the list box (ServerInterface.dll dialog 151).
	const int PlayerListId = 1057;
}

// (static initializer: ctor with a null parent, then atexit(004037f0)).
CChoosePlayerDlg GChoosePlayerDlg;


CChoosePlayerDlg::CChoosePlayerDlg(CWnd* Parent)
	: CDialog(IDD, Parent)
{
}

// — baseline's null test on the list pointer is the compiler's adjustment from
// the CFLListBox (+0x60) to its CWnd part (+0x64).
void CChoosePlayerDlg::DoDataExchange(CDataExchange* DX)
{
	DDX_Control(DX, PlayerListId, PlayerList);
}

// Message map module storage: LBN_DBLCLK 1057 -> 00403930, WM_MEASUREITEM -> 00403b00.
BEGIN_MESSAGE_MAP(CChoosePlayerDlg, CDialog)
	ON_LBN_DBLCLK(PlayerListId, &CChoosePlayerDlg::OnDblClkPlayerList)
	ON_WM_MEASUREITEM()
END_MESSAGE_MAP()

// — a double click is the virtual OnOK (vtable +0xcc).
void CChoosePlayerDlg::OnDblClkPlayerList()
{
	OnOK();
}

// — under the server lock, adds the active character name of every player
// session 1..MaxPlayers (baseline reads Players+0x4 on every iteration) to the list.
BOOL CChoosePlayerDlg::OnInitDialog()
{
	CDialog::OnInitDialog();
	LockServer();
	for (unsigned int SessionId = 1; SessionId <= Players.GetMaxPlayerCount(); ++SessionId)
	{
		const wchar_t* Name = Players.GetActiveCharacterName(SessionId);
		if (Name != nullptr)
		{
			PlayerList.AddItem(Name, 0);
		}
	}
	UnlockServer();
	return TRUE;
}

// — the dialog is modeless: Cancel destroys the window (vtable +0x60).
void CChoosePlayerDlg::OnCancel()
{
	DestroyWindow();
}

// — opens (or brings to front) the chat window of the selected character's
// account, then destroys this window; with no selection only the window is destroyed.
void CChoosePlayerDlg::OnOK()
{
	const int Selection = (int)::SendMessage(PlayerList.m_hWnd, LB_GETCURSEL, 0, 0);
	if (Selection > -1)
	{
		CiWString CharacterName;
		PlayerList.GetItemText(Selection, CharacterName);
		LockServer();
		CAccount* Account = Players.FindAccountFromCharacterName(CharacterName);
		if (Account != nullptr)
		{
			CChatAccountDlg* Chat = CChatAccountDlg::FindByName(Account->m_Name);
			if (Chat == nullptr)
			{
				Chat = new CChatAccountDlg(nullptr);
				Chat->AccountName.assign(Account->m_Name, 0, CiWString::npos);
				Chat->Create(CChatAccountDlg::IDD, nullptr);
				Chat->ShowWindow(SW_SHOW);
			}
			else
			{
				Chat->ShowWindow(SW_SHOW);
				::BringWindowToTop(Chat->m_hWnd);
			}
		}
		UnlockServer();
	}
	DestroyWindow();
}

// — list rows are as tall as the list's regular font.
void CChoosePlayerDlg::OnMeasureItem(int IDCtl, LPMEASUREITEMSTRUCT MeasureInfo)
{
	if (IDCtl != PlayerListId)
	{
		CDialog::OnMeasureItem(IDCtl, MeasureInfo);
		return;
	}
	CDC MeasureDC;
	MeasureDC.Attach(::CreateCompatibleDC(nullptr));
	CFont* OldFont = MeasureDC.SelectObject(&PlayerList.RegularFont);
	TEXTMETRICA Metrics;
	MeasureDC.GetTextMetrics(&Metrics);
	MeasureDC.SelectObject(OldFont);
	MeasureInfo->itemHeight = Metrics.tmHeight;
}
