// Author: Giperion.

#pragma once

// baseline Intro.cpp PageServerSelect (RTTI .?AVPageServerSelect@@ : UIPage::MasterWin) -- the
// multiplayer server browser (PAGE_ServerSelect.page: Connect, MainMenu, SetFilter, ServerTable,
// ServerScroll, ServerDetails, ListServerStatus, Refresh, Title, SubTitle, ServerInfoBox,
// ServerSelectBox, Legend). One static instance, the field (static init the branch -> ctor
// , atexit the branch -> dtor the routine). vftables: main 0x005e2bdc, IUIPage
// face (+0x7c) 0x00575d00, Event::Handler face (+0x80) 0x00572330.

// Call path: IntroFocus EnterState case 5 (0x00574b4e) resets the server lists, creates the
// Gun, loads the page descriptor (+0x8c, the field), calls slot 9 Create(0, 0) and makes this
// the active page (+0x228). IntroFocus::Update poll index 1 (0x00574006) moves to the state in
// +0xb4 (the field) and clears it. IntroFocus Exit calls slot 10 (0x00573e77).

// The rows are CLoginMgr's servers (the routine) filtered by +0x2a4 and sorted by one of the
// sixteen column comparators (module storage ascending / 00614ac0 descending); each row keeps a
// ServerRowData copy of its server as the UIFastTable payload.

// baseline layout past MasterWin (+0x00..+0x8b) is listed member by member below. The port's
// MasterWin has no baseline byte layout (see page_options.h), so the members keep only the order.

#include "common_text.h"   // RenderDisplayList, TextRenderContext::Image
#include "login_mgr.h"     // FServerInfo
#include "server_filter.h"
#include "ui_fast_table.h" // UiFastTableRefCounter, FUiFastTableEdges
#include "ui_page.h"
#include "ui_page_desc.h"

class UiFastTable;
class UiRichTextWin;
class UiScroll;
class UiTextMsgButton;

class PageServerSelect : public UIPage::MasterWin
{
public:
    // RTTI .?AVServerRowData@PageServerSelect@@ : UIFastTable::RefCounter, vftable 0x005e2e64
    // (slot 0 the scalar deleting destructor the routine, dtor the routine). 0xa4 bytes: the
    // RefCounter (+0x00 vftable, +0x04 count) and a copy of one server (+0x08). FillRow
    // (the routine) makes one per row; Slot23 reads the copy back through the row payload.
    class ServerRowData : public UiFastTableRefCounter
    {
    public:
        // (RET 4) -- the RefCounter (count 1), FServerInfo's defaults written
        // inline (0x005712b4..0x00571385), then the vftable and Info = Source memberwise
        // (0x0057138b..0x005714a8).
        explicit ServerRowData(FServerInfo const& Source);
        // Info's members torn down, then the RefCounter vftable.
        ~ServerRowData() override;

        FServerInfo Info; // +0x08
    };

    // .
    PageServerSelect();
    // .
    ~PageServerSelect() override;

    // slot 9 the routine (RET 8).
    bool Create(Win3D* NewParent, void* Payload) override;
    // slot 10 the routine.
    void Destroy() override;
    // slot 11 the routine -- the list pump, the slide in / out, then Win3D's child walk.
    void Update() override;
    // slot 23 the routine (RET 0xc).
    bool Slot23(unsigned Msg, unsigned A, unsigned B) override;
    // Event::Handler face (+0x80) slot 0 the branch (RET 4).
    bool OnEvent(unsigned MsgId) override;

    FUiPageDesc Desc;                        // +0x8c -- EnterState case 5 loads it (the field)
    int Result;                              // +0xb4 -- the field: the next IntroFocus state
    UiTextMsgButton* Connect;                // +0xb8 -- "Connect"
    UiTextMsgButton* MainMenu;               // +0xbc -- "MainMenu"
    UiTextMsgButton* SetFilter;              // +0xc0 -- "SetFilter"
    UiFastTable* ServerTable;                // +0xc4 -- "ServerTable"
    UiScroll* ServerScroll;                  // +0xc8 -- "ServerScroll"
    UiRichTextWin* ListServerStatus;         // +0xcc -- "ListServerStatus"
    UiRichTextWin* ServerDetails;            // +0xd0 -- "ServerDetails"
    UiTextMsgButton* Refresh;                // +0xd4 -- "Refresh"
    UiRichTextWin* Title;                    // +0xd8 -- "Title"
    UiRichTextWin* SubTitle;                 // +0xdc -- "SubTitle"
    Win3D* ServerInfoBox;                    // +0xe0 -- "ServerInfoBox"
    Win3D* ServerSelectBox;                  // +0xe4 -- "ServerSelectBox"
    UiRichTextWin* Legend;                   // +0xe8 -- "Legend"
    Win3D* Version;                          // +0xec -- a TextControl this page creates
    float InfoBoxHome[3];                    // +0xf0 -- ServerInfoBox's position after Create
    float SelectBoxHome[3];                  // +0xfc -- ServerSelectBox's position after Create
    double SlideT;                           // +0x108 -- Create -1.5; 0..2 in, 0..-1.5 leaving
    unsigned char bLeaving;                  // +0x110
    int LeaveDirection;                      // +0x114 -- 1 connect (Result 2), -1 back (1)
    int FirstVisible;                        // +0x118 -- ftol(scroll value) at the last rebuild
    RenderDisplayList ColumnHeaders[8];      // +0x11c -- IDS table 0x005e2604 (0x747..0x74c)
    RenderDisplayList Text729;               // +0x17c -- IDS 0x729. TODO: no reader found yet
    RenderDisplayList DetailsHeader;         // +0x188 -- IDS 0x744
    RenderDisplayList DetailsRestricted;     // +0x194 -- IDS 0x6007c, trailing paragraph cut
    RenderDisplayList PingUnknown;           // +0x1a0 -- TextNode u"????" (ping -2)
    RenderDisplayList PingNone;              // +0x1ac -- TextNode u"-" (ping -1)
    RenderDisplayList TextYes;               // +0x1b8 -- IDS 0x727
    RenderDisplayList TextNo;                // +0x1c4 -- IDS 0x728
    RenderDisplayList DetailsPassword;       // +0x1d0 -- IDS 0x750 (server +0x94 set)
    RenderDisplayList DetailsField95;        // +0x1dc -- IDS 0x74f (server +0x95 clear)
    RenderDisplayList TitleText;             // +0x1e8 -- style 0x1df5 + IDS 0x60072
    RenderDisplayList SubTitleText;          // +0x1f4 -- style 0x43e0 + IDS 0x6007d
    RenderDisplayList LegendSkullText;       // +0x200 -- IDS 0x6007b
    RenderDisplayList LegendLockText;        // +0x20c -- IDS 0x6007a
    RenderDisplayList StatusText60058;       // +0x218 -- style 0xbb80 + IDS 0x60058 (other)
    RenderDisplayList StatusText60057;       // +0x224 -- style 0xbb80 + IDS 0x60057 (1 / 4)
    RenderDisplayList StatusText60055;       // +0x230 -- style 0x9ca4 + IDS 0x60055 (2)
    RenderDisplayList StatusText60056;       // +0x23c -- style 0xbb80 + IDS 0x60056. TODO: reader
    RenderDisplayList Text601eb;             // +0x248 -- style 0x5724 + IDS 0x601eb. TODO: reader
    RenderDisplayList SubTitleLan;           // +0x254 -- style 0xc9b7 + IDS 0x601ef
    RenderDisplayList SubTitleInternet;      // +0x260 -- style 0xc9b7 + IDS 0x601ee
    TextRenderContext::Image* UpPip;         // +0x26c -- interface\intro\images\uppip.bmp
    TextRenderContext::Image* DownPip;       // +0x270 -- DownPip.bmp
    TextRenderContext::Image* Locked;        // +0x274 -- Locked.bmp
    TextRenderContext::Image* Skull;         // +0x278 -- Skull.bmp
    TextRenderContext::Image* GrayLock;      // +0x27c -- GrayLocked.bmp
    TextRenderContext::Image* GraySkull;     // +0x280 -- GraySkull.bmp
    TextRenderContext::Image* BlueLock;      // +0x284 -- BlueLocked.bmp
    TextRenderContext::Image* BlueSkull;     // +0x288 -- BlueSkull.bmp
    // Created and destroyed on Refresh's TRC; nothing in the exe draws them (no other access
    // to +0x28c / +0x290 in the listing).
    TextRenderContext::Image* ServerConnectImage;   // +0x28c -- MULTI_ServerConnect.bmp
    TextRenderContext::Image* ServerNoConnectImage; // +0x290 -- MULTI_ServerNoConnect.bmp
    unsigned char bListRequested;            // +0x294 -- CLoginMgr::RequestServerList answered
    unsigned SortColumn;                     // +0x298 -- 2
    unsigned char bAscending[8];             // +0x29c -- all 1
    ServerFilter Filter;                     // +0x2a4
    unsigned SelectedServerId;               // +0x2c8 -- -1: none
    int ListStatus;                          // +0x2cc -- CLoginMgr::GetGunStatus, 3 at Create
    FUiFastTableEdges Edges;                 // +0x2d0 -- HKCU\...\ServerTable "Columns" / "Rows"

private:
    // the table's edges into HKCU\Software\Microsoft\Microsoft Games\
    // OutpostFrontier\1.0\ServerTable (the routine); true when saved.
    bool SaveEdges();
    // (RET 0x14) -- Content into the cell (Row, Column) behind a style (the header
    // row: the sort column's, with its sort arrow after the content) and a justification.
    void SetCell(unsigned Row, unsigned Column, RenderDisplayList const* Content,
                 bool bVersionMatch, bool bSelected);
    // ServerDetails shows only the details caption.
    void RefreshDetailsText();
    // ListServerStatus shows the text for ListStatus.
    void RefreshListStatus();
    // the filtered, sorted list into the visible rows, the rest emptied.
    void RebuildTable();
    // (RET 8) -- one server into the eight cells of Row and the row payload.
    void FillRow(unsigned Row, FServerInfo const* Server);
    // (RET 4) -- every cell of Row emptied, its payload released.
    void ClearRow(unsigned Row);
    // (RET 8) -- ServerDetails shows Server's description and restrictions;
    // bNewServer resets the vertical scroll.
    void ShowServerDetails(FServerInfo const* Server, bool bNewServer);
    // (RET 4) -- the tool tips on or off (Legend's always off).
    void SetToolTipsEnabled(bool bEnabled);
    // the leave sequence (no direction); Slot23 spells it inline twice more.
    void BeginLeave();
    // (RET 4) -- the connect: current server, the password prompt, then
    // ConnectToServer (the routine) on the ini path. False when any step fails.
    bool ConnectToServer(FServerInfo const* Server);
    // The server Slot23's 0x3003 and 0xf002 act on: the selected row's payload, else the
    // remembered id through CLoginMgr::FindServer; null when there is neither.
    FServerInfo const* GetSelectedServer();

    PageServerSelect(PageServerSelect const&) = delete;
    PageServerSelect& operator=(PageServerSelect const&) = delete;
};

// .
PageServerSelect& GetPageServerSelect();
