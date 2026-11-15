// Author: Giperion.

#pragma once

// STUB -- not wired into any build unit yet. Written to document an identified but entirely
// unported baseline subsystem (implementation tracker "stub before simplified" convention), not to be
// functional. See OutpostFrontier.exe 0x00566ab0-0x005765b0 (batch48a).

// Class names are the RTTI type descriptors of the vftables (checked 2026-09-25): an earlier pass
// had invented CIntroScreen / CPilotSelectScreen / CLoadGameScreen / CServerListScreen for them.

// baseline source: Client\Interface\Intro.cpp (same translation unit as
// TypeSelect -- RTTI .?AUTypeSelect@@ : Win3D, Event::Handler (+0x7c), vftable 0x005e27cc, whose
// per-frame Update loop is the routine -- already marked simplified in a prior pass, see
// OutpostFrontier.exe items 0x00566510 / 0x00614ae0). Intro.cpp also implements three further,
// distinct classes that this stub covers, each a UIPage::MasterWin (: Win3D, IUIPage +0x7c,
// Event::Handler +0x80). Evidence: direct
// "Interface\\Intro.cpp" FDUMP-string citations inside functions at 0x00566ab0, 0x00567350's
// dependents, 0x00572100, 0x00573860's dependents (baseline lines 0x14b4/0x14c5/0x14cb/0x17ed/0x1ab/
// 0x1ff), plus consistent CHARACTER_ID / registry-key / FILETIME usage patterns cross-checked
// against each class's own vtable constructor.

// 1) PageCharacterSelect -- PORTED in page_character_select.h / .cpp (2026-09-30, listing-traced;
//    the multiplayer character list, not a single-player screen, and its table lives under HKCU).
//    Its faction records (CLoginMgr::ReloadFactions, the routine) and the character-create
//    pick its direction-2 exit fills (G_CharacterCreateSelection, the field) are ported.

// 2) PageCharacterSelectSingle -- PORTED in page_character_select_single.h / .cpp (2026-09-30,
//    listing-traced): the single-player "Load Game" page. Its rows are a std::vector of
//    {CharacterInfo*, name, time} records (not a std::list) shown in two UIListBoxes
//    (CharSelDesc / CharSelTime), not a sortable table.

// 3) PageServerSelect -- PORTED in page_server_select.h / .cpp (2026-09-26, listing-traced),
//    with its filter box in server_filter_dialog.h / .cpp and PageAccountSelect in
//    page_account_select.h / .cpp. The claims that stood here were wrong in several places: the
//    ServerTable edges live under HKCU, not HKLM (FRegKeyStack, reg_key_stack.h), and
// is IntroFocus' single-player start (menu_enter_state.cpp), not a
//    PageServerSelect method.

// A 4th, smaller reference-counted row-data record (vtable the routine, built inline inside
// ) backs PageCharacterSelect's rows; not modeled here. PageServerSelect's row record
// is PageServerSelect::ServerRowData (the routine / the routine).

// All three pages above are ported, and so is PageCharacterCreate (the field, CharCreate.cpp;
// page_character_create.h).
