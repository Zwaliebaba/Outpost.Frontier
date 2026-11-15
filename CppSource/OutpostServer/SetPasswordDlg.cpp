// Author: Giperion.

#include "StdAfx.h"
#include "SetPasswordDlg.h"
#include "OperationCompleteDlg.h"
#include "RichEditText.h"
#include "OutpostServer.h"
#include "server_playerdb.h"


CSetPasswordDlg::CSetPasswordDlg(CWnd* Parent)
	: CDialog(IDD, Parent)
	, Account(nullptr)
{
}


void CSetPasswordDlg::DoDataExchange(CDataExchange* DX)
{
	DDX_Control(DX, 1038, PasswordEdit);
}

// no entries.
BEGIN_MESSAGE_MAP(CSetPasswordDlg, CDialog)
END_MESSAGE_MAP()


void CSetPasswordDlg::OnOK()
{
	wchar_t Text[0x400];
	GetRichEditText(PasswordEdit.m_hWnd, Text, sizeof(Text));
	LockServer();
	{
		CiWString Password(Text);
		Players.SetAccountPassword(Account->m_Name, Password);
	}
	UnlockServer();

	COperationCompleteDlg Done(nullptr);
	Done.DoModal();
	CDialog::OnOK();
}
