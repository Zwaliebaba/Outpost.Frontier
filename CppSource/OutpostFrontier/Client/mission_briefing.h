// Author: Giperion.

#pragma once

// The MissionBriefing client state (RTTI COL 0x005e88d4), the singleton the field returned
// by the routine -- client state code 8. The GF base frame answers 8 after the routine
// (RequestGameflowMissionBriefing); the routine case 8 then destroys GF and Inits this state
// with 0. It hides the base, shows the "BriefingParentWindow" StaticControl with the
// NeuroNetNavMap in briefing mode inside it, and answers 2 (back to space) once the nav map
// asks to continue (the routine) and the UI sounds have finished.

// vftable 0x005cb094 (static init the branch), fifteen slots:
//   0 the branch (RET 0x10)  key message: the routine test, GetKeyState x2, true
//   1 the branch (RET 0xc)   true
//   2 the branch (RET 0xc)   true
//   3 the branch (RET 0x10)  true
//   4 the branch (RET 0xc)   true
//   5 0x00401e90               bare RET
//   6 0x0053e4a0 (RET 4)       bare RET 4 (ICF-folded with IDPMsgHandler::OnAddHost)
//   7 the branch             3 (the environment type)
//   8 the branch             pre-init: the BriefingParentWindow
//   9 the branch             shutdown: delete the window tree
//  10 0x00401e90               bare RET
//  11 0x0053aaa0 (RET 8)       bare RET
//  12 the branch (RET 4)     Init
//  13 the branch             Destroy
//  14 the branch (RET 4)     Update

// Layout (0x18):
//   +0x00 vptr
//   +0x04 byte  the first frame is still pending (1 from pre-init and every exit)
//   +0x08 Win3D* the "BriefingParentWindow" StaticControl
//   +0x0c byte  the field -- 1 while the NeuroNetNavMap is being built (the routine)
//   +0x0d byte  the field -- the nav map's continue request (the routine)
//   +0x10 dword the field -- the base the routine stored
//   +0x14 byte  idle: 1 from pre-init and every exit, 0 from Init
// The three members at +0x0c / +0x0d / +0x10 live in gameflow.cpp, where their accessors are.

#include "client_environment.h"

// &the field, struct MissionBriefing : IEnvironment (mission_briefing.cpp).
// Slot 8 is sent by InitializeEngine (0x005b1f53..0x005b1f5c), slot 9 by the client teardown
// (0x005b21f6..0x005b21ff).
IEnvironment* GetMissionBriefingState();
