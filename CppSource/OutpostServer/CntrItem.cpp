// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

#include "StdAfx.h"

#include "CntrItem.h"
#include "OutpostServerDoc.h"

// IMPLEMENT_SERIAL(COutpostServerCntrItem, CRichEditCntrItem, 0): retail CRuntimeClass at
// .data:0042543c {"COutpostServerCntrItem", 0x84, schema 0, CreateObject 00406610,
// _GetBaseClass 00406670}; GetRuntimeClass 00406680, AfxClassInit initializer 00406690,
// operator>> 004066a0.
IMPLEMENT_SERIAL(COutpostServerCntrItem, CRichEditCntrItem, 0)

// .text:004066c0 — CRichEditCntrItem(preo, pContainer) then the derived vtable.
COutpostServerCntrItem::COutpostServerCntrItem(REOBJECT* Preo, COutpostServerDoc* Container)
	: CRichEditCntrItem(Preo, Container)
{
}

// .text:004066e0 — scalar deleting destructor; the body is CRichEditCntrItem's.
COutpostServerCntrItem::~COutpostServerCntrItem()
{
}
