// Author: Giperion.

// FServerControl -- the 0x9b0-byte server-host settings object OutpostServer keeps by value inside
// the app at COutpostServerApp+0xcc (baseline ctor module storage, called from the app ctor at
// ; dtor module storage, called from the app dtor at module storage).
// It owns the server log file, the OutpostFrontier.ini / SrvLocal ini paths, the list-server
// host/port, the AppGUID and the SStartupInfo the app hands to IServer::Startup.
// baseline has no RTTI or string naming the class; the name follows the app member
// (COutpostServerApp::ServerControl). Field offsets are baseline's; the std members are modern
// std types (no baseline ABI), so offsets past +0x618 hold only in baseline.
#pragma once

#include <string>
#include <vector>
#include <fstream>

#include "common_server_types.h"   // SStartupInfo

// the exe's packed version (written by COutpostServerApp::InitInstance at
// (major&0xff | minor<<8) << 16 | build&0xffff), read by SetTitle.
extern unsigned int GServerVersion;

// CString::LoadString(Id); on failure the string becomes
// "<Invalid resource id>". Returns whether the load succeeded.
bool LoadStringChecked(CString& Out, UINT Id);

class FServerControl
{
public:
	FServerControl();
	~FServerControl();

	bool SetAppGuid(const GUID* Guid);
	bool SetIniPath(const char* Path);
	bool SetClientModulePath(const char* ModulePath);
	bool SetMaxPlayers(unsigned int MaxPlayers);
	bool OpenLog(const char* FileName);
	bool CloseLog();
	bool WriteLog(const char* Data, int Count);
	bool AddLogCategory(int Category);
	bool SetLogLevel(int Level);
	bool SetMaxConsoleLines(unsigned int Value);
	bool SetConsoleTrimLines(unsigned int Value);
	bool SetUpdateRate(float Rate);
	bool SetTitle(const char* Prefix);
	bool SetListServerHost(const char* HostName);
	bool SetListServerPort(int Port);
	bool SetServerName(const wchar_t* Name);
	bool SetServerDescription(const wchar_t* Text);
	bool SetServerPort(unsigned int Port);              // (StartupInfo +0x20c)
	void SetBandwidthLogPath(const char* Path);
	void SetAllowNewPlayers(unsigned char Value);       // (StartupInfo +0x211)
	unsigned char GetAllowNewPlayers() const;
	void SetAllowPvp(unsigned char Value);              // (StartupInfo +0x210)
	unsigned char GetAllowPvp() const;
	void SetAutoAcceptConfig(bool bValue);

public:
	char IniFileName[0x104];           // +0x000 "OutpostFrontier.ini"
	char Field_0x104[0x104];           // +0x104 zeroed by the ctor. TODO: meaning (no writer in this TU)
	char BandwidthLogPath[0x104];      // +0x208 "OutpostServerBWLog-%u.txt" (cmd line module storage); passed to CMainFrame
	char IniDir[0x100];                // +0x30c _splitpath dir of SetIniPath's argument
	char IniDrive[3];                  // +0x40c _splitpath drive
	char IniPath[0x104];               // +0x40f full OutpostFrontier.ini path
	char LocalServerIniPath[0x104];    // +0x513 same directory, "SrvLocal-" + IniFileName
	std::wstring ServerPassword;       // +0x618 empty after the ctor; Server Configuration dialog (module storage),
	                                   //        SetRemoteClientPassword (module storage), gun report (module storage)
	bool bListServerReporting;         // +0x628 NOT touched by the ctor (static object: zero). Written by the
	                                   //        Server Configuration dialog from a checkbox (module storage); gates
	                                   //        the gun thread (module storage) and UpdateGunReport (module storage)
	unsigned int MaxConsoleLines;      // +0x62c 100000; SetMaxConsoleLines accepts 1..100000; ServerLogf trims
	                                   //        the console once its line count exceeds it (module storage)
	unsigned int ConsoleTrimLines;     // +0x630 200; SetConsoleTrimLines accepts 1..MaxConsoleLines-1; lines
	                                   //        TrimLines deletes (module storage, module storage)
	float UpdatePeriod;                // +0x634 0.05f; SetUpdateRate stores 1/Rate (Rate in 1..60)
	char Title[0x100];                 // +0x638 "<prefix> <version>" formatted by SetTitle
	std::string ListServerHost;        // +0x738 "localhost"; [ListServer] hostname (module storage)
	int ListServerPort;                // +0x748 0x9988; [ListServer] p (module storage)
	std::wstring ServerName;           // +0x74c Server Configuration dialog, 0x42-byte buffer
	std::wstring ServerDescription;    // +0x75c Server Configuration dialog, 0x102-byte buffer
	std::ofstream* LogFile;            // +0x76c
	std::vector<int> LogCategories;    // +0x770 starts as { 0x10000 }; command-line pushes; ServerLogf
	                                   //        accepts a non-zero-level line only if Code>>4 is listed (module storage)
	int LogLevel;                      // +0x780 4; SetLogLevel accepts 1..7
	GUID AppGuid;                      // +0x784 [OutpostFrontier] AppGUID (module storage)
	bool Field_0x794;                  // +0x794 true. TODO: meaning
	SStartupInfo StartupInfo;          // +0x798 handed to IServer::Startup
	bool bLogOpen;                     // +0x9ac
	bool bUnmaskFpuExceptions;         // +0x9ad false; command line (module storage/00413b60); InitInstance
	                                   //        (module storage) and the server thread (module storage) unmask
	                                   //        zero-divide/overflow/invalid when set
	bool bAutoAcceptConfig;            // +0x9ae false; set by the /C command-line switch (module storage);
	                                   //        its only reader, CServerConfigDlg::OnInitDialog
	                                   //        (module storage), then accepts a loaded OutpostServer.cfg
	                                   //        through OnOK without waiting for the user
};
