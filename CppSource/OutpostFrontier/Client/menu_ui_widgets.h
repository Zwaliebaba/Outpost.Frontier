// Author: Giperion.

#pragma once

// Kept from the old header: several includers still reach common_goods.h / <windows.h> through
// it (the removed menu_input.h used to bring <windows.h>).
#include "common_goods.h"

#include <windows.h>

// baseline functions that live in menu_ui_widgets.cpp. The port-only flat menu layer this header
// used to declare (FMenuUiWidget registry, MenuUiCreate*, RenderMenuUi, MenuUiHitTest / hover /
// focus, NormLayoutToScreenRect, the NN_Main shell) was removed 2026-10-07: baseline's menu input
// goes through the Win3D manager (the routine / the routine) and the routine.

// Pre-init 005b1f86..005b1fac — the two the routine calls for ToolTip and Rollover.
void MenuUiInitToolTipAndRollover();

// The Rollover (the field) and its global entry points live in nn_rollover.h.

// (find by name) is Win3DFindByName and the routine (find by class name) is
// Win3DFindByClassName, both in win3d.h.

// — the rank level (the field) = -1, then IServer::RequestRankLevel(connection,
// {0}, 4). Called by the NN_Dealer ctor (0x0047fce1) and NN_ShipTrader::Create (0x004b850f).
void HudMessageQueueRequestRankRefresh();

// / the routine — clear and push_back on the baseline dword vector at
// ..d4. Only caller: IClient slot 120 (the branch).
void baselineU32QueueClear();
void baselineU32QueuePushBack(unsigned Value);
// / the routine — size() and bounds-checked at() (0 out of range). HUD_Target's
// formation test (0x004e1e2d..) reads the entries as object ids of the player's group.
unsigned baselineU32QueueSize();
unsigned baselineU32QueueAt(unsigned Index);
