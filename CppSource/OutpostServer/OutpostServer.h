// Author: Giperion.

// COutpostServerApp — OutpostServer.exe's CWinApp (baseline ctor module storage, dtor 0040b420,
// vtable module storage, message map module storage, the global object theApp at
// ). baseline keeps a second pointer to it in module storage (GApp below), set by
// the constructor and cleared by the destructor; code all over the exe reads the app through it.
#pragma once

#include <string>

#include "ServerControl.h"
#include "ShutdownDlg.h"

class CGunWrapper;
class COutpostServerDoc;
class CFLSummaryForm;

class COutpostServerApp : public CWinApp
{
public:
	COutpostServerApp();
	virtual ~COutpostServerApp();

	// Overrides (baseline vtable slots: InitInstance +0x58 -> 0040b840, OnIdle +0x68 -> 0040d670,
	// ExitInstance +0x70 -> 0040b6f0).
	virtual BOOL InitInstance();
	virtual BOOL OnIdle(LONG Count);
	virtual int ExitInstance();

	// — stops the server and gun threads, starts the Server.dll shutdown
	// thread and runs the shutdown dialog until it has finished.
	bool BeginShutdown();
	// — true once the shutdown thread has finished (or none is running).
	bool PollShutdownThread();
	// — true once the startup thread has finished (or none is running); on
	// completion runs OnServerStartupFinished and returns its result.
	bool PollStartupThread();

	bool IsShuttingDown() const { return bShuttingDown; }
	// — while the startup thread runs, closing the frame waits for it in the
	// shutdown dialog.
	void WaitForStartupThread();

	bool IsStartupPending() const { return bStartupPending; }


	CView* GetMainView() const { return MainView; }

	CFLSummaryForm* GetSummaryForm() const { return SummaryForm; }

	// The window/document title kept by ServerControl (app +0x704, set from "OutpostServer -" by
	// InitInstance through module storage); COutpostServerDoc::SetTitle (0040da80) always uses it.
	LPCTSTR GetServerTitle() const;

	// — toggles the frame between the console view and the summary form;
	// returns the view that was active before.
	CView* SwitchView();

protected:

	bool OnServerStartupFinished(bool bStarted);
	// — reads [OutpostFrontier] / [ListServer] / [Server] / [Resources] of an
	// ini file; returns true if a "Data path" was found and opened.
	bool ReadOutpostFrontierIni(const char* Path);
public:
	// — refreshes the gun (matchmaking) report; called by the gun thread.
	bool UpdateGunReport();

protected:
	// Message handlers (message map module storage).
	afx_msg void OnAppAbout();
	afx_msg void OnDebugDebug();
	afx_msg void OnDebugAiDebugger();
	afx_msg void OnUpdateDisabled(CCmdUI* CmdUI);           // / 0040d410
	afx_msg void OnUpdateNeedsServer(CCmdUI* CmdUI);        // / 0040d230 / 0040d290 / 0040d2f0 / 0040d3e0
	afx_msg void OnEditAccounts();
	afx_msg void OnChatPlayer();
	afx_msg void OnChatStarSystem();
	afx_msg void OnChatUniverse();
	afx_msg void OnServerNews();
	afx_msg void OnDebugCrashMe();
	afx_msg void OnUpdateViewConsole(CCmdUI* CmdUI);
	afx_msg void OnUpdateViewStatusSummary(CCmdUI* CmdUI);
	afx_msg void OnSwitchView();

	DECLARE_MESSAGE_MAP()

public:
	// +0xc4 is CWinApp's last field in MFC42; +0xc8 is the first COutpostServerApp field.
	CGunWrapper* GunWrapper = nullptr;      // +0xc8: NeuronRuntime gun (matchmaking) reporter
	FServerControl ServerControl;           // +0xcc (ctor 00401440, dtor 00401730)

	// +0xa7c / +0xb7c: strings owned by the app, freed by the destructor. Their only writer in
	// baseline, module storage (add a name unless already present, at most 64), has no caller and
	// no stored pointer anywhere in the image, so the array stays empty; not ported.
	std::string* OwnedStrings[64] = {};     // +0xa7c
	int OwnedStringCount = 0;               // +0xb7c

	RECT ConsoleFrameRect = {};             // +0xb80: frame rect saved when leaving the console
	CView* MainView = nullptr;              // +0xb90: main frame's active view after startup
	CFLSummaryForm* SummaryForm = nullptr;  // +0xb94
	CDocument* MainDocument = nullptr;      // +0xb98: main frame's active document
	HMODULE RichEditModule = nullptr;       // +0xb9c: RICHED20.DLL
	CWinThread* StartupThread = nullptr;    // +0xba0: runs IServerImpl::Startup (0040be90)
	CWinThread* ShutdownThread = nullptr;   // +0xba4: runs IServerImpl::Shutdown (0040b710)
	bool bStartupPending = false;           // +0xba8
	bool bShuttingDown = false;             // +0xba9
	CShutdownDlg ShutdownDlg;               // +0xbac
};

extern COutpostServerApp theApp;
// — set by the constructor, cleared by the destructor.
extern COutpostServerApp* GApp;

extern HANDLE GAppMutex;
extern HANDLE GServerMutex;
extern int GServerRunning;
extern HANDLE GServerThread;
extern DWORD GServerThreadId;
extern HANDLE GGunThread;

void LockApp();
void UnlockApp();
void LockServer();              // (21 callers across the exe)
void UnlockServer();
