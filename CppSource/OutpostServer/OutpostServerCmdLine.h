// Author: Giperion.

// COutpostServerCommandLineInfo — the command-line parser InitInstance hands to ParseCommandLine.
// baseline: vtable module storage, ParseParam module storage, dtor 0040bed0 / scalar 0040bee0
// (both compiler-generated). Switches (first character, either case) fill
// GApp->ServerControl; anything else is logged as unknown and passed to MFC.
#pragma once

class COutpostServerCommandLineInfo : public CCommandLineInfo
{
public:
	virtual void ParseParam(const TCHAR* Param, BOOL bFlag, BOOL bLast);
};
