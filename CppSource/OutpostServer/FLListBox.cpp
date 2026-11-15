// Author: Giperion.

#include "StdAfx.h"
#include "FLListBox.h"
#include "common_misc.h" // HexToWide / WideToHex

// — four 12-point "Arial Unicode MS" fonts (regular, bold, bold italic, italic,
// created in that order).
CFLListBox::CFLListBox()
{
	HDC ScreenDC = ::GetDC(nullptr);
	LOGFONTA Font;
	Font.lfHeight = -::MulDiv(12, ::GetDeviceCaps(ScreenDC, LOGPIXELSY), 72);
	Font.lfWidth = 0;
	Font.lfEscapement = 0;
	Font.lfOrientation = 0;
	Font.lfWeight = FW_NORMAL;
	Font.lfItalic = FALSE;
	Font.lfUnderline = FALSE;
	Font.lfStrikeOut = FALSE;
	Font.lfCharSet = DEFAULT_CHARSET;
	Font.lfOutPrecision = OUT_DEFAULT_PRECIS;
	Font.lfClipPrecision = CLIP_DEFAULT_PRECIS;
	Font.lfQuality = DEFAULT_QUALITY;
	Font.lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE;
	strcpy_s(Font.lfFaceName, "Arial Unicode MS");

	RegularFont.Attach(::CreateFontIndirectA(&Font));
	Font.lfWeight = FW_BOLD;
	BoldFont.Attach(::CreateFontIndirectA(&Font));
	Font.lfItalic = TRUE;
	BoldItalicFont.Attach(::CreateFontIndirectA(&Font));
	Font.lfWeight = FW_NORMAL;
	ItalicFont.Attach(::CreateFontIndirectA(&Font));
	::ReleaseDC(nullptr, ScreenDC);
}

// — deletes the fonts explicitly (regular, bold, italic, bold italic), then the
// maps and the CFont members are destroyed by the compiler.
CFLListBox::~CFLListBox()
{
	RegularFont.DeleteObject();
	BoldFont.DeleteObject();
	ItalicFont.DeleteObject();
	BoldItalicFont.DeleteObject();
}

// Message map module storage: WM_DRAWITEM reflection -> DrawItem through the vcall thunk
// (CListBox vtable +0xc0). Written out by hand because ON_WM_DRAWITEM_REFLECT would
// take &CFLListBox::DrawItem, whose member-pointer representation (CWnd is not the first base)
// does not convert to a CWnd member pointer; &CListBox::DrawItem is what baseline's entry holds.
BEGIN_MESSAGE_MAP(CFLListBox, CListBox)
	{ WM_DRAWITEM + WM_REFLECT_BASE, 0, 0, 0, AfxSig_vs,
		(AFX_PMSG)(AFX_PMSGW)(static_cast<void (AFX_MSG_CALL CWnd::*)(LPDRAWITEMSTRUCT)>(&CListBox::DrawItem)) },
END_MESSAGE_MAP()


void CFLListBox::AddItem(const wchar_t* Text, DWORD_PTR Data)
{
	char Hex[0x400];
	WideToHex(Text, Hex);
	const LRESULT Index = ::SendMessageA(m_hWnd, LB_ADDSTRING, 0, (LPARAM)Hex);
	::SendMessageA(m_hWnd, LB_SETITEMDATA, (WPARAM)Index, (LPARAM)Data);
}


void CFLListBox::GetItemText(int Index, CiWString& OutText)
{
	CString HexText;
	GetText(Index, HexText);
	wchar_t Text[0x400];
	HexToWide(HexText, Text);
	OutText.assign(Text, wcslen(Text));
}


int CFLListBox::SelectItem(const wchar_t* Text)
{
	char Hex[0x400];
	WideToHex(Text, Hex);
	return (int)::SendMessageA(m_hWnd, LB_SELECTSTRING, 0, (LPARAM)Hex);
}

// — map operator[] (baseline inlines the insert-unique path).
void CFLListBox::SetItemColor(DWORD_PTR Data, COLORREF Color)
{
	ItemColors[Data] = Color;
}

// — erase(equal_range(Data)).
void CFLListBox::RemoveItemColor(DWORD_PTR Data)
{
	ItemColors.erase(Data);
}


COLORREF CFLListBox::GetItemColor(DWORD_PTR Data)
{
	std::map<DWORD_PTR, COLORREF>::iterator It = ItemColors.find(Data);
	if (It == ItemColors.end())
	{
		return ::GetSysColor(COLOR_WINDOWTEXT);
	}
	return It->second;
}


void CFLListBox::SetItemItalic(DWORD_PTR Data, bool bItalic)
{
	if (!bItalic)
	{
		ItalicItems.erase(Data);
		return;
	}
	ItalicItems[Data] = true;
}

// — presence only; the stored bool is never read.
bool CFLListBox::IsItemItalic(DWORD_PTR Data)
{
	return ItalicItems.find(Data) != ItalicItems.end();
}


void CFLListBox::SetItemBold(DWORD_PTR Data, bool bBold)
{
	if (!bBold)
	{
		BoldItems.erase(Data);
		return;
	}
	BoldItems[Data] = true;
}


bool CFLListBox::IsItemBold(DWORD_PTR Data)
{
	return BoldItems.find(Data) != BoldItems.end();
}


void CFLListBox::DrawItem(LPDRAWITEMSTRUCT DrawInfo)
{
	CDC* DC = CDC::FromHandle(DrawInfo->hDC);
	RECT ItemRect;
	::CopyRect(&ItemRect, &DrawInfo->rcItem);
	const UINT bSelected = DrawInfo->itemState & ODS_SELECTED;
	{
		CBrush Background(::GetSysColor(bSelected ? COLOR_HIGHLIGHT : COLOR_WINDOW));
		::FillRect(DC->m_hDC, &ItemRect, (HBRUSH)Background.m_hObject);
	}

	if (DrawInfo->itemID == (UINT)-1)
	{
		return;
	}

	CString HexText;
	GetText(DrawInfo->itemID, HexText);
	wchar_t Text[0x400];
	HexToWide(HexText, Text);

	const DWORD_PTR Data = DrawInfo->itemData;
	const bool bBold = IsItemBold(Data);
	const bool bItalic = IsItemItalic(Data);
	CFont* Font;
	if (bBold)
	{
		Font = bItalic ? &BoldItalicFont : &BoldFont;
	}
	else
	{
		Font = bItalic ? &ItalicFont : &RegularFont;
	}
	CFont* OldFont = DC->SelectObject(Font);
	DC->SetBkMode(TRANSPARENT);

	const COLORREF Color = GetItemColor(Data);
	ItemRect.left += 4;
	if (Color == ::GetSysColor(COLOR_WINDOWTEXT))
	{
		DC->SetTextColor(::GetSysColor(bSelected ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
	}
	else
	{
		DC->SetTextColor(Color);
	}
	DC->SetBkColor(::GetSysColor(bSelected ? COLOR_HIGHLIGHT : COLOR_WINDOW));
	::TextOutW(DC->m_hDC, ItemRect.left, ItemRect.top, Text, (int)wcslen(Text));
	DC->SelectObject(OldFont);
}


void CFLListBox::DeleteItem(LPDELETEITEMSTRUCT DeleteInfo)
{
	RemoveItemColor(DeleteInfo->itemData);
	CListBox::DeleteItem(DeleteInfo);
}
