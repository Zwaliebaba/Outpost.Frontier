// Author: Giperion.

#include "StdAfx.h"

#include "CntrItem.h"
#include "OutpostServerDoc.h"

// IMPLEMENT_SERIAL(COutpostServerCntrItem, CRichEditCntrItem, 0): baseline CRuntimeClass at
// {"COutpostServerCntrItem", 0x84, schema 0, CreateObject 00406610,
// _GetBaseClass 00406670}; GetRuntimeClass 00406680, AfxClassInit initializer 00406690,
// operator>> 004066a0.
IMPLEMENT_SERIAL(COutpostServerCntrItem, CRichEditCntrItem, 0)

// — CRichEditCntrItem(preo, pContainer) then the derived vtable.
COutpostServerCntrItem::COutpostServerCntrItem(REOBJECT* Preo, COutpostServerDoc* Container)
	: CRichEditCntrItem(Preo, Container)
{
}

// — scalar deleting destructor; the body is CRichEditCntrItem's.
COutpostServerCntrItem::~COutpostServerCntrItem()
{
}
