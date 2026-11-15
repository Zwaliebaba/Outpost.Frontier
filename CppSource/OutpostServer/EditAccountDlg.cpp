// Author: Giperion.

#include "StdAfx.h"
#include "EditAccountDlg.h"
#include "AreYouSureDlg.h"
#include "RichEditText.h"
#include "OutpostServer.h"
#include "server_playerdb.h"

// — CButton +0x60, CFLListBox +0xa0 (0040fad0), CButton +0x140, CButton +0x180,
// CRichEditCtrl +0x1c0; Account (+0x200) is not written.
CEditAccountDlg::CEditAccountDlg(CWnd* Parent)
	: CDialog(IDD, Parent)
{
}


void CEditAccountDlg::DoDataExchange(CDataExchange* DX)
{
	DDX_Control(DX, 1044, BannedCheck);
	DDX_Control(DX, 1040, CharacterList);
	DDX_Control(DX, 1041, DeleteCharacterButton);
	DDX_Control(DX, 1037, AccountNameEdit);
}

// Message map module storage.
BEGIN_MESSAGE_MAP(CEditAccountDlg, CDialog)
	ON_LBN_SELCHANGE(1040, &CEditAccountDlg::OnSelChangeCharacterList)
	ON_LBN_SELCANCEL(1040, &CEditAccountDlg::OnSelCancelCharacterList)
	ON_BN_CLICKED(1041, &CEditAccountDlg::OnDeleteCharacter)
	ON_WM_MEASUREITEM()
	ON_BN_CLICKED(1045, &CEditAccountDlg::OnKickAccount)
END_MESSAGE_MAP()


BOOL CEditAccountDlg::OnInitDialog()
{
	CDialog::OnInitDialog();
	LockServer();
	SetRichEditText(AccountNameEdit.m_hWnd, Account->m_Name.c_str());
	std::list<CiWString>::iterator Character = Account->m_Characters.begin();
	while (Character != Account->m_Characters.end())
	{
		CharacterList.AddItem(Character->c_str(), 0);
		++Character;
	}
	::SendMessageA(BannedCheck.m_hWnd, BM_SETCHECK, !Account->m_bNotBanned, 0);
	UnlockServer();
	return TRUE;
}


void CEditAccountDlg::OnSelChangeCharacterList()
{
	DeleteCharacterButton.EnableWindow(TRUE);
}


void CEditAccountDlg::OnSelCancelCharacterList()
{
	DeleteCharacterButton.EnableWindow(FALSE);
}


void CEditAccountDlg::OnDeleteCharacter()
{
	CAreYouSureDlg Confirm(nullptr);
	if (Confirm.DoModal() != IDOK)
	{
		return;
	}
	const int Selection = (int)::SendMessageA(CharacterList.m_hWnd, LB_GETCURSEL, 0, 0);
	if (Selection <= -1)
	{
		return;
	}

	LockServer();
	CiWString CharacterName;
	CharacterList.GetItemText(Selection, CharacterName);
	Players.DeleteCharacterFromName(CharacterName);
	UnlockServer();
	::SendMessageA(CharacterList.m_hWnd, LB_DELETESTRING, Selection, 0);
	DeleteCharacterButton.EnableWindow(FALSE);
}

// — rows are as tall as the list box's regular font.
void CEditAccountDlg::OnMeasureItem(int IDCtl, LPMEASUREITEMSTRUCT MeasureInfo)
{
	if (IDCtl != 1040)
	{
		CWnd::OnMeasureItem(IDCtl, MeasureInfo);
		return;
	}
	CDC MeasureDC;
	MeasureDC.Attach(::CreateCompatibleDC(nullptr));
	CFont* OldFont = MeasureDC.SelectObject(&CharacterList.RegularFont);
	TEXTMETRICA Metrics;
	::GetTextMetricsA(MeasureDC.m_hDC, &Metrics);
	MeasureDC.SelectObject(OldFont);
	MeasureInfo->itemHeight = Metrics.tmHeight;
}

// — the "Banned" box is applied only when it differs from the account's state
// (m_bNotBanned equal to the check state means they disagree).
void CEditAccountDlg::OnOK()
{
	const bool bBanned = ::SendMessageA(BannedCheck.m_hWnd, BM_GETCHECK, 0, 0) != 0;
	LockServer();
	if (Account->m_bNotBanned == bBanned)
	{
		Players.LockAccountAccess(Account->m_Name);
		Players.BanAccount(Account->m_Name, bBanned);
		Players.UnlockAccountAccess(Account->m_Name);
	}
	UnlockServer();
	CDialog::OnOK();
}

// — "Kick Account From Server": applies the "Banned" box like OnOK, but with the
// account access lock taken unconditionally, then closes the dialog with IDCANCEL. baseline does
// nothing else here (no logout call of its own).
void CEditAccountDlg::OnKickAccount()
{
	LockServer();
	Players.LockAccountAccess(Account->m_Name);
	const bool bBanned = ::SendMessageA(BannedCheck.m_hWnd, BM_GETCHECK, 0, 0) != 0;
	if (Account->m_bNotBanned == bBanned)
	{
		Players.BanAccount(Account->m_Name, bBanned);
	}
	Players.UnlockAccountAccess(Account->m_Name);
	UnlockServer();
	EndDialog(IDCANCEL);
}
