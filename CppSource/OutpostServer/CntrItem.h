// Author: Giperion.

// COutpostServerCntrItem — the rich-edit container item of COutpostServerDoc (OutpostServer.exe, baseline
// , vtable module storage, CRuntimeClass module storage).
#pragma once

class COutpostServerDoc;
class COutpostServerView;

class COutpostServerCntrItem : public CRichEditCntrItem
{
	DECLARE_SERIAL(COutpostServerCntrItem)

public:
	COutpostServerCntrItem(REOBJECT* Preo = NULL, COutpostServerDoc* Container = NULL);

	COutpostServerDoc* GetDocument()
	{
		return (COutpostServerDoc*)CRichEditCntrItem::GetDocument();
	}
	COutpostServerView* GetActiveView()
	{
		return (COutpostServerView*)CRichEditCntrItem::GetActiveView();
	}

	virtual ~COutpostServerCntrItem();
};
