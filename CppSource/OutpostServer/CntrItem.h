// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

// COutpostServerCntrItem — the rich-edit container item of COutpostServerDoc (OutpostServer.exe, retail
// .text:00406610..004066ff, vtable .rdata:0041c620, CRuntimeClass .data:0042543c).
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
