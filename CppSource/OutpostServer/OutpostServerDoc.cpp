// Author: Giperion.

#include "StdAfx.h"

#include "OutpostServerDoc.h"
#include "CntrItem.h"
#include "OutpostServer.h"

// IMPLEMENT_DYNCREATE(COutpostServerDoc, CRichEditDoc): baseline CRuntimeClass module storage
// {"COutpostServerDoc", 0x234, 0xffff, CreateObject 0040d800, _GetBaseClass 0040d8c0};
// GetRuntimeClass 0040d8d0.
IMPLEMENT_DYNCREATE(COutpostServerDoc, CRichEditDoc)

// (GetMessageMap 0040d8f0); the base map is COleServerDoc's because
// CRichEditDoc declares none of its own.
BEGIN_MESSAGE_MAP(COutpostServerDoc, CRichEditDoc)
	ON_UPDATE_COMMAND_UI(ID_OLE_EDIT_LINKS, &CRichEditDoc::OnUpdateEditLinksMenu)
	ON_COMMAND(ID_OLE_EDIT_LINKS, &CRichEditDoc::OnEditLinks)
	ON_UPDATE_COMMAND_UI_RANGE(ID_OLE_VERB_FIRST, ID_OLE_VERB_LAST, &CRichEditDoc::OnUpdateObjectVerbMenu)
END_MESSAGE_MAP()

// (inlined into CreateObject 0040d800).
COutpostServerDoc::COutpostServerDoc()
{
	Load = 33;
	Mutex = CreateMutexA(NULL, FALSE, NULL);
	for (int Index = 0; Index < LoadHistorySize; ++Index)
	{
		LoadHistory[Index] = 33;
	}
	AverageLoad = 33;
	LoadHistoryIndex = 0;
	bAverageDirty = true;
	memset(&Stats, 0, sizeof(Stats));
}


COutpostServerDoc::~COutpostServerDoc()
{
	if (Mutex != NULL)
	{
		CloseHandle(Mutex);
		Mutex = NULL;
	}
}


BOOL COutpostServerDoc::OnNewDocument()
{
	return CRichEditDoc::OnNewDocument() ? TRUE : FALSE;
}


CRichEditCntrItem* COutpostServerDoc::CreateClientItem(REOBJECT* Preo) const
{
	return new COutpostServerCntrItem(Preo, (COutpostServerDoc*)this);
}

// — plain text, not RTF.
void COutpostServerDoc::Serialize(CArchive& Ar)
{
	m_bRTF = FALSE;
	CRichEditDoc::Serialize(Ar);
}


BOOL COutpostServerDoc::SaveModified()
{
	return TRUE;
}

// — the document title is always the server's title string.
void COutpostServerDoc::SetTitle(LPCTSTR Title)
{
	if (GApp != NULL)
	{
		Title = GApp->GetServerTitle();
	}
	CRichEditDoc::SetTitle(Title);
}


int COutpostServerDoc::GetLoad()
{
	if (Mutex != NULL)
	{
		WaitForSingleObject(Mutex, INFINITE);
		int Result = Load;
		ReleaseMutex(Mutex);
		return Result;
	}
	return Load;
}


int COutpostServerDoc::GetAverageLoad()
{
	if (Mutex != NULL)
	{
		WaitForSingleObject(Mutex, INFINITE);
		if (bAverageDirty)
		{
			int Sum = 0;
			for (int Index = 0; Index < LoadHistorySize; ++Index)
			{
				Sum += LoadHistory[Index];
			}
			AverageLoad = Sum / LoadHistorySize;
			bAverageDirty = false;
		}
		int Result = AverageLoad;
		ReleaseMutex(Mutex);
		return Result;
	}
	return AverageLoad;
}


bool COutpostServerDoc::SetLoad(int NewLoad)
{
	if (Mutex != NULL)
	{
		DWORD Wait = WaitForSingleObject(Mutex, 0);
		if (Wait != WAIT_OBJECT_0 && Wait != WAIT_ABANDONED)
		{
			return false;
		}
		Load = NewLoad;
		LoadHistory[LoadHistoryIndex] = NewLoad;
		++LoadHistoryIndex;
		if (LoadHistoryIndex >= LoadHistorySize)
		{
			LoadHistoryIndex = 0;
		}
		bAverageDirty = true;
		ReleaseMutex(Mutex);
		return true;
	}
	Load = NewLoad;
	return true;
}


bool COutpostServerDoc::SetStats(const ServerStats* NewStats)
{
	if (Mutex != NULL)
	{
		DWORD Wait = WaitForSingleObject(Mutex, 0);
		if (Wait != WAIT_OBJECT_0 && Wait != WAIT_ABANDONED)
		{
			return false;
		}
		Stats = *NewStats;
		ReleaseMutex(Mutex);
		return true;
	}
	Stats = *NewStats;
	return true;
}


void COutpostServerDoc::GetStats(ServerStats* OutStats)
{
	if (Mutex != NULL)
	{
		WaitForSingleObject(Mutex, INFINITE);
		*OutStats = Stats;
		ReleaseMutex(Mutex);
		return;
	}
	*OutStats = Stats;
}

// — the rich-edit selection always comes from the app's main view.
COleClientItem* COutpostServerDoc::GetPrimarySelectedItem(CView* View)
{
	if (View != GApp->GetMainView())
	{
		View = GApp->GetMainView();
	}
	return CRichEditDoc::GetPrimarySelectedItem(View);
}
