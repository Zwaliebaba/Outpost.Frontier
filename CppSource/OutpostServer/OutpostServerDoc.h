// Author: Giperion.

// COutpostServerDoc — OutpostServer.exe's document (baseline CreateObject module storage, ctor 0040d900,
// dtor 0040d990, vtable module storage, CRuntimeClass module storage, message map
// ). A CRichEditDoc that also carries the server load figures and the last
// ServerStats snapshot; the console view, the summary form and the main frame read them.
#pragma once

#include "common_server_types.h"

class COutpostServerDoc : public CRichEditDoc
{
protected:
	COutpostServerDoc();
	DECLARE_DYNCREATE(COutpostServerDoc)

public:
	enum { LoadHistorySize = 64 };

	// — the last value given to SetLoad.
	int GetLoad();
	// — mean of the LoadHistorySize history slots; recomputed only when a
	// SetLoad happened since the last call.
	int GetAverageLoad();
	// — records a load sample; fails (returns false) without blocking when the
	// mutex is held elsewhere.
	bool SetLoad(int Load);
	// — stores a ServerStats snapshot; fails without blocking like SetLoad.
	bool SetStats(const ServerStats* Stats);
	// — copies the snapshot out (blocks on the mutex).
	void GetStats(ServerStats* Stats);

	virtual CRichEditCntrItem* CreateClientItem(REOBJECT* Preo) const;
	virtual BOOL OnNewDocument();
	virtual void Serialize(CArchive& Ar);
	virtual void SetTitle(LPCTSTR Title);
	virtual BOOL SaveModified();
	virtual COleClientItem* GetPrimarySelectedItem(CView* View);

	virtual ~COutpostServerDoc();

protected:
	DECLARE_MESSAGE_MAP()

	HANDLE Mutex;                        // +0xf4
	int Load;                            // +0xf8
	bool bAverageDirty;                  // +0xfc
	int LoadHistory[LoadHistorySize];    // +0x100
	int LoadHistoryIndex;                // +0x200
	int AverageLoad;                     // +0x204
	ServerStats Stats;                   // +0x208
};
