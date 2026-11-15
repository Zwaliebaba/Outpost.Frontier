// Author: Giperion.

#include "StdAfx.h"
#include "ServerNewsDlg.h"
#include "RichEditText.h"
#include "Motd.h"
#include "OutpostServer.h"
#include "server_playerdb.h"

namespace
{
	// EM_EXLIMITTEXT limit and EM_GETTEXTEX byte count used by the dialog (0x1400).
	const int NewsTextBytes = 0x1400;
}


CServerNewsDlg::CServerNewsDlg(CWnd* Parent)
	: CDialog(IDD, Parent)
{
}


void CServerNewsDlg::DoDataExchange(CDataExchange* DX)
{
	DDX_Control(DX, 1058, NewsEdit);
}

// no entries.
BEGIN_MESSAGE_MAP(CServerNewsDlg, CDialog)
END_MESSAGE_MAP()


BOOL CServerNewsDlg::OnInitDialog()
{
	CDialog::OnInitDialog();
	LockServer();
	::SendMessageA(NewsEdit.m_hWnd, EM_EXLIMITTEXT, 0, NewsTextBytes);
	SetRichEditText(NewsEdit.m_hWnd, GMotd.c_str());
	UnlockServer();
	return TRUE;
}


void CServerNewsDlg::OnOK()
{
	wchar_t Text[NewsTextBytes];
	GetRichEditText(NewsEdit.m_hWnd, Text, NewsTextBytes);
	LockServer();
	GMotd = Text;
	SaveMotd();
	Players.SetMOTD(GMotd);
	UnlockServer();
	CDialog::OnOK();
}


void ApplyServerNews()
{
	LockServer();
	Players.SetMOTD(GMotd);
	UnlockServer();
}
