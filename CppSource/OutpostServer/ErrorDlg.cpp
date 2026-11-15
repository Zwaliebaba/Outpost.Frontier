// Author: Giperion.

#include "StdAfx.h"

#include "ErrorDlg.h"

CErrorDlg::CErrorDlg(CWnd* Parent)
	: CDialog(CErrorDlg::IDD, Parent)
{
}

void CErrorDlg::DoDataExchange(CDataExchange* DX)
{
	CDialog::DoDataExchange(DX);
}

// no entries.
BEGIN_MESSAGE_MAP(CErrorDlg, CDialog)
END_MESSAGE_MAP()
