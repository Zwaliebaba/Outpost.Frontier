// Author: Giperion.

// CMainFrame — OutpostServer.exe's main frame window (baseline CreateObject module storage, ctor
// , vtable module storage, CRuntimeClass module storage, message map
// ; TU 00411ea0..00412a80). Owns the status bar with the uptime pane, a 1 s
// timer that samples RemoteClient's per-client traffic counters and the optional bandwidth CSV
// log (+0x20c).
#pragma once

#include <stdio.h>

class CMainFrame : public CFrameWnd
{
protected:
	CMainFrame();
	DECLARE_DYNCREATE(CMainFrame)

public:
	virtual ~CMainFrame();

	// — clears WS_THICKFRAME (0x40000) from the default style, then the base.
	virtual BOOL LoadFrame(UINT IdResource, DWORD DefaultStyle = WS_OVERLAPPEDWINDOW | FWS_ADDTOTITLE,
		CWnd* ParentWnd = NULL, CCreateContext* Context = NULL) override;

	virtual BOOL PreCreateWindow(CREATESTRUCT& Cs) override;

	// — (re)opens the bandwidth CSV log; called by
	// COutpostServerApp::OnServerStartupFinished (module storage).
	void OpenBandwidthLog(const char* Path);
	// — closes the bandwidth log. No caller in baseline (no call/jmp/abs ref).
	void CloseBandwidthLog();

	// CCmdUI handlers with no message-map entry and no caller in baseline (strings 0xf028..0xf02c).
	void OnUpdateTraffic(CCmdUI* CmdUI);
	void OnUpdateConnects(CCmdUI* CmdUI);
	void OnUpdateThroughput(CCmdUI* CmdUI);
	void OnUpdatePacketsOut(CCmdUI* CmdUI);
	void OnUpdatePacketsIn(CCmdUI* CmdUI);

protected:
	afx_msg int OnCreate(LPCREATESTRUCT CreateStruct);
	afx_msg void OnUpdateUptime(CCmdUI* CmdUI);
	afx_msg void OnTimer(UINT_PTR IdEvent);
	afx_msg void OnClose();

	DECLARE_MESSAGE_MAP()

public:
	CTime StartTime;                 // +0xc0: CTime::GetCurrentTime() in the ctor
	CStatusBar StatusBar;            // +0xc4
	CToolBar ToolBar;                // +0x144 (never created)
	// Traffic summed over every client at the last 1 s sample (OnTimer).
	unsigned int TotalBytes;         // +0x1e4 sent + received
	unsigned int BytesReceived;      // +0x1e8
	unsigned int BytesSent;          // +0x1ec
	unsigned int PrevTotalBytes;     // +0x1f0 TotalBytes of the previous sample
	unsigned int ThroughputBits;     // +0x1f4 bits per second between the last two samples
	unsigned int PacketsSent;        // +0x1f8
	unsigned int PacketsReceived;    // +0x1fc
	unsigned int PrevSampleTime;     // +0x200 timeGetTime of the previous sample
	unsigned int SampleTime;         // +0x204 timeGetTime of the last sample
	int ClientCount;                 // +0x208 GetNumClients at the last sample
	FILE* BandwidthLog;              // +0x20c
};
