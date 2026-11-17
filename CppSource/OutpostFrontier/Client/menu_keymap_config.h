// Author: Giperion.

#pragma once

#include <list>
#include <string>
#include <vector>

// KeyMap.ini / accelerator groups — baseline Client/Interface/KeyMap.cpp
// (the routine / the routine / the routine / the routine / the routine walk).

enum EKeyCmdState : unsigned
{
    kKeyCmdStateKeyDown = 1,
    kKeyCmdStateKeyUp = 2,
    kKeyCmdStateRepeat = 4,
    kKeyCmdStateShiftIgnore = 8,
};

enum EKeyModifier : unsigned
{
    kKeyModNone = 0,
    kKeyModShift = 1,
    kKeyModControl = 4,
    kKeyModAlt = 0x10,
};

struct FKeyBinding
{
    int VirtualKey;
    unsigned Modifiers;
};

// One [KeyCmd]: the 0x20-byte payload of a the field list node (node +8). New entries are
// copies of the lazily built template the field (the routine 0x00576cb6..0x00576cf6),
// whose only non-zero field is StateMask = 1.
struct FKeyCmd
{
    unsigned NicknameId;               // +0x00 index into PTR_s_USER_NONE_00614dd8
    unsigned IdsName;                  // +0x04 ids_name
    unsigned IdsInfo;                  // +0x08 ids_info
    unsigned StateMask;                // +0x0c EKeyCmdState bits
    std::vector<FKeyBinding> Bindings; // +0x10 (allocator byte, then first / last / end)
};

// One [KeyMap] group: the 0x1c-byte payload of a the field list node (node +8) -- a
// std::string nickname (+0x00) and a std::list of KeyCmd pointers (+0x10 allocator, +0x14
// head, +0x18 size).
struct FKeyMapGroup
{
    std::string Nickname;
    std::list<FKeyCmd*> Commands;
};

// — load interface/KeyMap.ini (GetFileSysPath). Returns group count (>0 = ok).
int LoadKeyMapIni(char const* RelPath, bool bFatal);

// — load UserKeyMap.ini override (clears existing bindings, re-applies KeyCmd).
int LoadUserKeyMapIni(char const* FullPath);

// — write live KeyCmd bindings to UserKeyMap.ini (options Controls OK/Apply).
int SaveUserKeyMapIni(char const* FullPath);

// — returns the relative path string "interface/KeyMap.ini".
char const* GetKeyMapIniRelPath();

// — ensure the field holds "<user data>/UserKeyMap.ini"; return that buffer.
char const* EnsureUserKeyMapPath();

// — resolve USER_* nickname string → KeyCmd* (nullptr if unknown / unloaded).
FKeyCmd* FindKeyCmdByNickname(char const* Nickname);

// — update / erase / insert one binding on the KeyCmd with NicknameId.
// NewVirtualKey==0 erases the (OldVirtualKey, OldModifiers) pair; otherwise replaces or appends.
bool UpdateKeyCmdBinding(unsigned NicknameId, int NewVirtualKey, unsigned NewModifiers,
                         int OldVirtualKey, unsigned OldModifiers);

// — resolve accelerator group nickname to group pointer (nullptr if missing).
FKeyMapGroup* FindKeyMapGroup(char const* GroupNickname);

// — first call loads KeyMap+UserKeyMap; always activates the named group.
bool RegisterAcceleratorGroup(char const* GroupNickname);

// — remove named group from the active list.
void UnregisterAcceleratorGroup(char const* GroupNickname);

// — route one pumped message: WM_KEYDOWN/KEYUP/SYSKEY*/CHAR fill the event record
// (HudCommandRecord), go to the handler tree as kind 1 (key) or 2 (char), then
// through the held-key list the field and the active accelerator groups (the routine gate,
// per KeyCmd). True = consumed (skip TranslateMessage / DispatchMessage).
bool KeyMapRouteInputMessage(unsigned Message, unsigned Wparam, unsigned Lparam);

struct FHudCmdRecord;
// (cdecl) — route one queued mouse event (the routine's 0x1c-byte event head):
// copy it into the field, clear +0x0c/+0x10/+0x14, capture Shift/Ctrl/Alt, dispatch the
// event kind (key -1..-8 -> 6..0xd, -9 wheel -> 4, else 0) in mode 2, then the active
// accelerator groups' bindings on that key (mode 2 as well). True = consumed.
bool KeyMapRouteMouseEvent(FHudCmdRecord const* Event);

unsigned KeyMapGetActionNicknameId(char const* Nickname);
char const* KeyMapGetActionNickname(unsigned NicknameId);
unsigned KeyMapGetActiveGroupCount();

// Oracle instrumentation (P0.5, oracle_mode.cpp): the KeyCmd list, the [KeyMap] groups and
// the active groups, read-only.
std::list<FKeyCmd> const& KeyMapOracleCommands();
std::list<FKeyMapGroup> const& KeyMapOracleGroups();
std::list<FKeyMapGroup*> const& KeyMapOracleActiveGroups();

// Out-blob filled by the routine / the routine (NicknameId + first binding).
struct FKeyBindingLookup
{
    unsigned NicknameId; // +0x00
    int VirtualKey;      // +0x04
    unsigned Modifiers;  // +0x08
};

// (cdecl) -- over every KeyCmd (the field) with NicknameId: Out->NicknameId =
// NicknameId, and the first whose binding list has BindingIndex fills VK / modifiers (true).
bool KeyMapFindCommandBinding(unsigned NicknameId, FKeyBindingLookup* Out, unsigned BindingIndex);
// / the routine -- BindingIndex 0 / 1. Callers: NN_Preferences::AddKeyRow.
bool KeyMapFindPrimaryBinding(unsigned NicknameId, FKeyBindingLookup* Out);
bool KeyMapFindAltBinding(unsigned NicknameId, FKeyBindingLookup* Out);
// (cdecl) -- the first KeyCmd with NicknameId, or null.
struct FKeyCmd* FindKeyCmdById(unsigned NicknameId);
// (cdecl) -- GetKeyState: Shift 1 | Ctrl 4 | Alt 0x10.
unsigned KeyMapGetModifierState();

// — walk active accelerator groups (the field); first binding of NicknameId.
bool KeyMapFindActiveBinding(unsigned NicknameId, FKeyBindingLookup* Out);

// — is any binding of command NicknameId (list the field) held right now:
// its modifiers all down and its key down (GetKeyState), or for a mouse key -1/-2/-3 the
// matching bit of the field.
bool KeyMapIsCommandHeld(unsigned NicknameId);

// — format VirtualKey+Modifiers into Dest (GetKeyNameText / IDS fallbacks).
void KeyMapFormatBindingName(FKeyBindingLookup const* Binding, wchar_t* Dest, unsigned DestChars);
