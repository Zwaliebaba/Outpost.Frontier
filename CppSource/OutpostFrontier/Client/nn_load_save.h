// Author: Giperion.

#pragma once

// baseline NN_LoadSave (RTTI .?AVNN_LoadSave@@ : WinControl, Event::Handler;
// Client\nn\NN_LoadSave.cpp) -- the NeuroNet load / save game window:
// a list of the saved characters (name and date columns), the info panel of the selected one,
// the description edit box (save mode) and the Save / Load / Delete / Main Menu / Resume buttons.

// WINFACTORY<NN_LoadSave> the field "NN_LoadSave": static init the branch (ctor
// , atexit the branch -> the routine), Create the routine (operator_new(0x178c),
// ctor the routine, slot 9; on failure the scalar deleting dtor).

// vftable 0x005d1d5c (45 slots, COL 0x005eb2ac) -- WinControl's 0x005e5e44 with these overrides:
//   0 the routine (scalar deleting dtor around the routine), 9 the branch Create,
//   11 the routine Update, 13 the routine Draw, 23 the branch, 42 the branch SetProperty.
// IDataViewOwner face +0x7c vftable 0x005d1d50; Event::Handler face +0x32c vftable 0x005d1d48
// (the branch).

// Modes (+0x334, the int the factory payload points at): 1 load, 2 save, 3 load with no way
// back (NN_StoryStar 0x004bc1b3 / 0x004bc908 -- Delete, Resume and the close box hidden, and
// USER_CANCEL ignored).

// Layout:
//   +0x000 WinControl (0x32c)
//   +0x32c Event::Handler vptr
//   +0x330 software-cursor draw flag saved when the window opened (the routine)
//   +0x334 mode
//   +0x338 what the open NN_Confirm asks: 1 delete, 2 main menu, 3 the NN_Error raised for
//          failed saves, 4 overwrite; 0 none
//   +0x33c main menu confirmed -- Update closes the window and posts USER_EXIT_GAME (0xc5)
//   +0x33d save allowed (USER_ENTER 0xf004 saves only when set; Draw sets it once +0x33e is)
//   +0x33e the list has been refilled since the last save (the routine's callers set it)
//   +0x340 std::vector<FNnLoadSaveEntry> (VC6: allocator byte, begin +0x344, end +0x348, cap +0x34c)
//   +0x350 std::wstring -- ids 0x65d, the prefix of generated save names (VC6: +0x354 ptr,
//          +0x358 length, +0x35c capacity)
//   +0x360 that prefix's length, +0x364 the number the next generated name gets
//   +0x368 CHARACTER_ID the save overwrites (first byte 0 = a new save)
//   +0x568 wchar_t[0x20] the name the save is made under
//   +0x5a8 LoadSave_Title (TextControl)
//   +0x5ac SaveGame / LoadGame / Delete / MainMenu / ResumeGame (ButtonControl), +0x5c0 their
//          captions (TextControl), +0x5d4 whether each is enabled (SelectSave writes them)
//   +0x5dc ListBox of names, +0xe8c its offset from the window; +0xe98 ListBox of dates,
//          +0x1748 its offset
//   +0x1754 visible rows (0x10 in save mode, else 0x14), +0x1758 LoadSave_Scroll (UIScroll)
//   +0x175c LoadSave_DescripText, +0x1760 LoadSave_DescripButton, +0x1764 LoadSave_Edit2
//          (UITextEdit) -- save mode only, else all three 0
//   +0x1768 / +0x176c / +0x1770 / +0x1774 location / credits / rank / ship titles,
//   +0x1778 / +0x177c / +0x1780 / +0x1784 their values (TextControl)
//   +0x1788 LoadSave_Close (ButtonControl)
//   size 0x178c
// The port keeps baseline's member order; the two std containers are the current STL, so offsets
// past +0x340 are baseline's in the comments only.

#include "event_handler.h"
#include "list_box.h"
#include "wincontrol_base.h"

#include "common_server_types.h" // CHARACTER_ID
#include "server_characterset.h" // CCharacterSet::CharacterInfo

#include <stddef.h>

#include <string>
#include <vector>

class RichTextEditWin;
class UiScroll;

// One row of the list (0x24 bytes in baseline; element dtor the routine, which destroys the two
// strings -- the same body folded for every {dword, wstring, wstring} record).
struct FNnLoadSaveEntry
{
    CCharacterSet::CharacterInfo* Info; // +0x00 -- the G_LoginMgr record (the routine)
    std::wstring Time;                  // +0x04 -- filetime_to_wstring(Info +0x228)
    std::wstring Name;                  // +0x14 -- NnLoadSaveEntryText(Info) (the routine)
};

enum
{
    kNnLoadSaveModeLoad = 1,
    kNnLoadSaveModeSave = 2,
    kNnLoadSaveModeStoryStar = 3,
};

class NnLoadSave : public WinControl, public EventHandler
{
public:
    // (inlined nowhere: the routine calls it).
    NnLoadSave();
    // vptrs back; the members and the bases go in reverse order.
    ~NnLoadSave() override;

    // slot 9 the branch (RET 8); Data points at the mode.
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 11 the routine
    void Update() override;
    // slot 13 the routine
    void Draw() override;
    // slot 23 the branch (RET 0xc)
    bool Slot23(unsigned Msg, unsigned A, unsigned B) override;
    // slot 42 the branch (RET 0xc)
    intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override;

    // Event::Handler face, the branch (RET 4)
    bool OnEvent(unsigned MsgId) override;

    // save under the edit box's name (or a generated one); an existing save of
    // that name asks first (NN_Confirm, +0x338 = 4).
    void SaveGame();
    // rebuild the list from a fresh character list (the routine).
    void RefreshSaveList();
    // (RET 4) -- show row Index in the info panel and set the buttons; -1 clears.
    void SelectSave(int Index);
    // (RET 4) -- load row Index: queue the character (the routine), close, fade in.
    void LoadSave(int Index);

    unsigned char bCursorDrawFlag;             // +0x330
    unsigned char Pad331[3];                   // +0x331
    int Mode;                                  // +0x334
    int ConfirmMode;                           // +0x338
    unsigned char bExitToMainMenu;             // +0x33c
    unsigned char bSaveAllowed;                // +0x33d
    unsigned char bListRefreshed;              // +0x33e
    unsigned char Pad33f;                      // +0x33f
    unsigned char Byte340;                     // +0x340 -- VC6 vector allocator byte
    std::vector<FNnLoadSaveEntry> Entries;     // baseline +0x344..+0x34c
    std::wstring SaveNamePrefix;               // baseline +0x350 (allocator byte), +0x354..+0x35c
    int SaveNamePrefixLength;                  // baseline +0x360
    int NextSaveNumber;                        // baseline +0x364
    CHARACTER_ID OverwriteId;                  // baseline +0x368
    wchar_t SaveName[0x20];                    // baseline +0x568
    WinControl* Title;                         // baseline +0x5a8
    WinControl* Buttons[5];                    // baseline +0x5ac
    WinControl* ButtonTexts[5];                // baseline +0x5c0
    unsigned char bButtonEnabled[5];           // baseline +0x5d4
    unsigned char Pad5d9[3];                   // baseline +0x5d9
    ListBox NameList;                          // baseline +0x5dc
    float NameListOffset[3];                   // baseline +0xe8c
    ListBox TimeList;                          // baseline +0xe98
    float TimeListOffset[3];                   // baseline +0x1748
    int VisibleRows;                           // baseline +0x1754
    UiScroll* Scroll;                          // baseline +0x1758
    WinControl* DescripText;                   // baseline +0x175c
    WinControl* DescripButton;                 // baseline +0x1760
    RichTextEditWin* DescripEdit;              // baseline +0x1764
    WinControl* InfoTitles[4];                 // baseline +0x1768 location, credits, rank, ship
    WinControl* InfoValues[4];                 // baseline +0x1778 location, credits, rank, ship
    WinControl* CloseButton;                   // baseline +0x1788

private:
    // The close every path shares (OnEvent 0x00486b93..0x00486c00, Update 0x00488261..
    // , LoadSave 0x00488149..0x004881b0): the routine(+0x330), slot 10 on both lists,
    // slot 33 ReleaseMouse, the routine on every child, slot 10, the scalar deleting dtor.
    void ResumeAndDelete();
    // Both lists emptied and unselected (+0xe80 / +0x173c = 0, +0xe88 / +0x1744 = -1), then
    // SelectSave(-1) -- inline at 0x00487211, 0x004872d2 and 0x0048774e.
    void ClearListsAndSelection();
    // 0x2c / 0x2d: the caption of the button A, or null for another sender.
    WinControl* ButtonTextFor(uintptr_t Button) const;
};

static_assert(sizeof(void*) != 4 || offsetof(NnLoadSave, bCursorDrawFlag) == 0x330,
              "NN_LoadSave +0x330 cursor flag");
static_assert(sizeof(void*) != 4 || offsetof(NnLoadSave, Mode) == 0x334, "NN_LoadSave +0x334");
static_assert(sizeof(void*) != 4 || offsetof(NnLoadSave, ConfirmMode) == 0x338,
              "NN_LoadSave +0x338");
static_assert(sizeof(void*) != 4 || offsetof(NnLoadSave, bExitToMainMenu) == 0x33c,
              "NN_LoadSave +0x33c");
static_assert(sizeof(void*) != 4 || offsetof(NnLoadSave, Byte340) == 0x340,
              "NN_LoadSave +0x340");

// (cdecl) -- unless an NN_LoadSave exists (the routine), build one with Mode as
// its payload, SetProperty(0x42, &{0, 0, 0}, 0) and capture the mouse. Callers: NN_Pause
// (2) / 0x004a73b3 (1), NN_StoryStar 0x004bc1b3 / 0x004bc908 (3).
void OpenNnLoadSave(int Mode);

// (cdecl) -- the text a save is listed under: DescripStrId (+0x224) through
// into G_NnLoadSaveText (FDUMP on a miss, the buffer either way), else the
// Description string (+0x214). Callers: the routine 0x00487967, 0x0056b5ee, 0x0056c708,
// .
wchar_t const* NnLoadSaveEntryText(CCharacterSet::CharacterInfo const* Info);

// 0x100 wide characters (up to the field); the string-resource buffer of
// and SelectSave.
extern wchar_t G_NnLoadSaveText[0x100];
