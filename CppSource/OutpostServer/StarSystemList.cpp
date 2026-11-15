// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

// Non-MFC TU on purpose: SysDB::SysMap needs server_sysdb.h, whose closure declares
// OutpostFrontier's CObject (clashes with MFC CObject). So no StdAfx.h and no afx headers here.
#include "StarSystemList.h"
#include "IFLListBox.h"
#include "ResourceStrings.h"
#include "server_sysdb.h"     // SysDB::SysMap
#include "server_playerdb.h"  // Players
#include "common_universe.h"  // Universe::get_system / GetFirstSystem / GetNextSystem

#include <cwchar>

// OutpostServer.cpp (.text:0040cf90 / 0040d040); OutpostServer.h cannot be included here (it is MFC).
void LockServer();
void UnlockServer();

// .text:00417700 — LockServer; Universe::get_system(SystemId); SysDB::SysMap find (lower_bound
// 00417e80 plus the key test); found: the system's strid_name (ISystem +0x0c) through the
// resource DLL table, the same steps as LoadIdsString 004176b0 inlined; not found: L"???"
// (.rdata:00425a60). Tail call UnlockServer. get_system's result is used without a null test.
void GetSystemName(unsigned int SystemId, wchar_t* Buf, int BufChars)
{
	LockServer();
	const Universe::ISystem* System = Universe::get_system(SystemId);
	if (SysDB::SysMap.find(SystemId) != SysDB::SysMap.end())
	{
		LoadIdsString(nullptr, (unsigned int)System->GetStridName(), Buf, BufChars);
	}
	else
	{
		wcscpy(Buf, L"???");
	}
	UnlockServer();
}

// .text:004177d0 — the per-system name lookup is GetSystemName inlined (0041786c..004178ff,
// buffer 0x400 characters); the list add is IFLListBox slot 0.
void FillStarSystemList(unsigned int MinPlayers, IFLListBox* List)
{
	LockServer();
	Universe::ISystem* System = Universe::GetFirstSystem();
	while (System != nullptr)
	{
		const unsigned int SystemId = System->GetId();
		if (SysDB::SysMap.find(SystemId) != SysDB::SysMap.end()
			&& Players.CountPlayersInSystem((int)SystemId) >= MinPlayers)
		{
			wchar_t Name[0x400];
			GetSystemName(SystemId, Name, 0x400);
			List->AddItem(Name, SystemId);
		}
		System = Universe::GetNextSystem();
	}
	UnlockServer();
}
