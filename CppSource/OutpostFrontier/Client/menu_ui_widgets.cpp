// Author: Giperion.

#include "menu_ui_widgets.h"

#include "character_select_equip.h" // SetPlayerRankLevelbaseline (the routine's global)
#include "common_server.h"          // IServer::RequestRankLevel (slot 78)
#include "login_mgr.h"              // G_Server (the field)
#include "player_spawn.h"           // GetPlayerConnectionId (the field)
#include "win_factory.h"            // WinFactoryCreate (the routine)

#include <vector>

// 2026-10-07 (claude-code:dev-a): the port-only flat menu layer that filled this file -- the
// FMenuUiWidget registry and its factories, RegisterMainMenuUiPlaceholders (a port-only
// "NN_Main" Win3D root), RenderMenuUi, the screen-rect hit test / hover / focus walk
// (MenuUiHitTest, MenuUiSetHover, MenuUiFocusDelta, ...), NormLayoutToScreenRect (the last
// g_display reader of the letterbox entry) and the dead MenuUiCreate* helpers -- is gone.
// baseline's menu input is the Win3D manager's: MouseDispatchEvents (the routine) ->
// KeyMapRouteMouseEvent -> commands 4..13 -> FWin3DInputHandler::Handle (the routine), the
// hover pick in Win3DUpdateAll (the routine), keys through KeyMapRouteInputMessage
// (the routine) from the message pump. What remains here are baseline functions that happened
// to live in this file.

// — the player's rank level (the field) becomes -1, pending, and
// IServer::RequestRankLevel(connection, {0}, 4) (vtable slot 78, +0x138) asks the server for
// it again; NN_ShipTrader::Update (the routine) notices the change. Call sites 0x0047fce1
// (NN_Dealer ctor, nn_dealer.cpp) and 0x004b850f (NN_ShipTrader::Create).
void HudMessageQueueRequestRankRefresh()
{
    // the field = -1; 0x004c5613: the 4-byte local zeroed; 0x004c55f6 /
    // ECX = the field with no null test, slot 78 (+0x138).
    SetPlayerRankLevelbaseline(-1);
    unsigned char Data[4] = {0, 0, 0, 0};
    G_Server->RequestRankLevel(GetPlayerConnectionId(), Data, 4);
}

namespace
{
    // (ctor)/the routine(dtor)/the routine(clear)/the routine(push_back)/
    // (size)/the routine(at) — baseline keeps a small std::vector<unsigned>-shaped
    // object (globals the field/d0/d4, begin/end/capacity-end) plus a leading flag byte
    // (the field). analysis folded the vector's internal destroy-range helper onto an
    // unrelated symbol "IDPMsgHandler::OnReceive" (identical-code-folding of two same-shaped
    // template instantiations). External call sites: 0x004d074a/0x004d076c/0x004d0791
    // (HUD_ContactList), 0x004e1e2d/0x004e1e4b/0x004e1e71 (HUD_Target) read via size()+at(),
    // and 0x0053e27c/0x0053e29e (IClient slot 120) clear then push_back.
    struct FbaselineU32QueueStub
    {
        bool bFlag = false;
        std::vector<unsigned> Items;
    };
    // (static init: flag 0, empty vector) and its atexit the branch (free the
    // buffer, the routine) are this static object's own constructor and destructor.
    FbaselineU32QueueStub GbaselineU32Queue;
}

// — clear() without releasing storage (matches baseline's end=begin reset).
void baselineU32QueueClear()
{
    GbaselineU32Queue.Items.clear();
}

// — push_back(value).
void baselineU32QueuePushBack(unsigned Value)
{
    GbaselineU32Queue.Items.push_back(Value);
}

// — size(): 0 for an unallocated vector, else (end - begin) >> 2.
unsigned baselineU32QueueSize()
{
    return static_cast<unsigned>(GbaselineU32Queue.Items.size());
}

// — at(index): a negative index (JL), an unallocated vector or index >= size
// (unsigned JNC) give 0. A negative int converts to an unsigned above any size, so the one
// unsigned compare covers baseline's two tests.
unsigned baselineU32QueueAt(unsigned Index)
{
    if (Index >= GbaselineU32Queue.Items.size())
        return 0;
    return GbaselineU32Queue.Items[Index];
}

// is the exe's copy of BaseWatcher::BaseWatcher(IObjRW*); its one caller, the
// static init the branch (0x004c5067), passes null for the field -- best_path.cpp's
// G_BestPathBaseWatcher, whose default constructor is the port of that call.

void MenuUiInitToolTipAndRollover()
{
    // Pre-init 005b1f86..005b1fac: the routine("ToolTip","ToolTip",0,0) then
    // ("Rollover","Rollover",0,0). Both use the nickname as the class name, and
    // baseline ignores the return value in both cases.
    // Both are WINFACTORY objects reached through the routine itself: WINFACTORY<ToolTip>
    // (nn_tooltip.cpp, the field) and WINFACTORY<Rollover> (nn_rollover.cpp, the field).
    WinFactoryCreate("ToolTip", "ToolTip", nullptr, nullptr);
    WinFactoryCreate("Rollover", "Rollover", nullptr, nullptr);
}
