// Author: Giperion.

// COutpostServerView — OutpostServer.exe's console view, a CRichEditView (baseline CreateObject
// , ctor 0040ded0, dtor 0040df40, vtable module storage, CRuntimeClass
// , message map module storage). While it exists it is the target of
// ServerLogf (module storage), which it installs as NEURONCOMPONENTS's FDUMP hook.
#pragma once

#include "remoteclient.h"   // ErrorCode

class COutpostServerDoc;

class COutpostServerView : public CRichEditView
{
protected:
	COutpostServerView();
	DECLARE_DYNCREATE(COutpostServerView)

public:
	COutpostServerDoc* GetDocument() { return (COutpostServerDoc*)m_pDocument; }

	virtual BOOL PreCreateWindow(CREATESTRUCT& Cs);
	virtual void OnInitialUpdate();
	virtual BOOL OnPreparePrinting(CPrintInfo* Info);

	virtual ~COutpostServerView();

	// — deletes the oldest ServerControl.ConsoleTrimLines lines.
	bool TrimLines();

	unsigned int LineCount;   // +0x184: lines appended by ServerLogf since the last trim

protected:
	afx_msg void OnDestroy();
	afx_msg void OnKeyDown(UINT Char, UINT RepCnt, UINT Flags);

	DECLARE_MESSAGE_MAP()
};

// — exported @57 through OutpostServer.def; also NEURONCOMPONENTS's FDUMP while the view lives
// and RemoteClient's log function (SetServerLogFunction). Must stay
// ?ServerLogf@@YAHUErrorCode@@PBDZZ.
int __cdecl ServerLogf(ErrorCode Code, const char* Format, ...);
