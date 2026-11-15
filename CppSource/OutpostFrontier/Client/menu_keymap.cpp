// Author: Giperion.

#include "menu_keymap_config.h"
#ifdef OUTPOST_FRONTIER_WALK_MODE
#include "base_walk.h"
#endif
#include "msvcrt_compat.h" // MsvcrtAtoi: baseline MSVCRT.DLL wraps mod 2^32, the UCRT saturates

#include "common_ini.h"
#include "common_misc.h" // GetFileSysPath, GetUserDataPath
#include "neuroncomponents.h"
#include "hud_command_dispatch.h"
#include "ids_strings.h"
#include "input_manager.h" // GetClientMouseButtons (the field)

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <list>
#include <string.h>
#include <vector>
#include <windows.h>

namespace
{
    // baseline PTR_s_USER_NONE_00614dd8 — 0xcc entries (listing module storage..00615104).
    unsigned const kKeyCmdNicknameCount = 0xCCu;

    char const* const GKeyCmdNicknames[kKeyCmdNicknameCount] = {
#include "menu_keymap_nicknames.inc"
    };

    // (allocator byte) / the field (head) / the field (size): every KeyCmd,
    // held by value in 0x28-byte nodes. The static initializer the branch builds the empty
    // sentinel; the branch (atexit) unlinks each node, runs the payload destructor
    // (the routine, the binding vector) and frees it, then frees the sentinel. The
    // namespace-scope std::list does all of that; the routine is its insert and
    // the payload copy-constructor.
    std::list<FKeyCmd> GKeyCmds;
    // / the field / the field: every [KeyMap] group, by value, 0x24-byte
    // nodes. the branch builds it, the branch (atexit) destroys each payload through
    // (the KeyCmd-pointer list, then the nickname string); the routine is the
    // insert and the routine the payload copy-constructor.
    std::list<FKeyMapGroup> GKeyMapGroups;
    // (allocator byte) / the field (head) / the field (size): the active
    // groups, newest first -- a std::list of group pointers in 0xc-byte nodes. The static
    // initializer the branch builds the empty sentinel, the branch (atexit) frees the nodes
    // and the sentinel; a namespace-scope std::list does both. The walkers read a node's next
    // pointer after its body, as a list iterator does.
    std::list<FKeyMapGroup*> GActiveGroups;
    bool G_bKeyMapLoaded = false;
    char GUserKeyMapPath[0x104] = {};                 // (0x104 bytes, next is the field)

    // (allocator byte) / the field (head) / the field (size) -- the list of
    // keys held down, each with the modifier mask captured at its key-down. 0x10-byte nodes:
    // next +0, prev +4, key +8, modifiers +0xc (the routine copies the pair into node+8).
    // The static initializer the branch builds the empty sentinel and registers the branch
    // through atexit to free every node; a namespace-scope std::list does both.
    struct FKeyMapHeldKey
    {
        unsigned Key;       // node+0x08
        unsigned Modifiers; // node+0x0c
    };
    std::list<FKeyMapHeldKey> GHeldKeys;

    // The inlined nickname lookup of the routine (0x00576723..0x00576746), the routine
    // (0x005770e0..0x005770ff) and the routine: stricmp against the 0xcc names in order,
    // the first match wins, no match gives 0 (USER_NONE).
    unsigned ResolveNicknameId(char const* Name)
    {
        unsigned Index = 0;
        for (; Index < kKeyCmdNicknameCount; ++Index)
        {
            if (_stricmp(Name, GKeyCmdNicknames[Index]) == 0)
                return Index;
        }
        return 0;
    }

    // The first KeyCmd in list order with this id (the routine / the routine's walk).
    FKeyCmd* FindKeyCmdByNicknameId(unsigned NicknameId)
    {
        for (std::list<FKeyCmd>::iterator It = GKeyCmds.begin(); It != GKeyCmds.end(); ++It)
        {
            if (It->NicknameId == NicknameId)
                return &*It;
        }
        return nullptr;
    }

    // — parse one "key" value into VirtualKey + Modifiers.
    bool ParseKeyValue(INI_Reader& Reader, FKeyBinding* OutBinding)
    {
        OutBinding->VirtualKey = 0;
        OutBinding->Modifiers = 0;

        char const* Token = Reader.get_value_string(0);
        if (Token == nullptr || Token[0] == '\0')
            return false;

        if (Token[0] == '"')
        {
            OutBinding->VirtualKey = static_cast<unsigned char>(Token[1]);
        }
        else if (Token[0] == '0')
        {
            unsigned Parsed = 0;
            if (sscanf(Token, "%X", &Parsed) != 1)
            {
                OutBinding->VirtualKey = 0;
                return false;
            }
            OutBinding->VirtualKey = static_cast<int>(Parsed);
        }
        else
        {
            int Parsed = MsvcrtAtoi(Token);
            if (Parsed < -8 || Parsed > 0xff)
            {
                FDUMP(0x100001, "%s(%d) : *** ERROR: invalid KeyMap key value (%s)",
                      "Client\\Interface\\KeyMap.cpp", 0x1e6, Token);
            }
            OutBinding->VirtualKey = Parsed;
        }

        unsigned ParamIndex = 1;
        while (!Reader.is_value_empty(ParamIndex))
        {
            char const* Mod = Reader.get_value_string(ParamIndex);
            if (Mod == nullptr || Mod[0] == '\0')
                break;
            if (_strnicmp(Mod, "shift", 5) == 0)
                OutBinding->Modifiers = kKeyModShift;
            else if (_strnicmp(Mod, "control", 7) == 0)
                OutBinding->Modifiers = kKeyModControl;
            else if (_strnicmp(Mod, "alt", 3) == 0)
                OutBinding->Modifiers = kKeyModAlt;
            else
                return false;
            ++ParamIndex;
        }
        return true;
    }

    // (thiscall on the KeyCmd, RET 4) -- parse the current [KeyCmd] section into
    // *Cmd. Only the nickname id is reset (0x005766f3); the other fields keep whatever the
    // caller's template put there. True when the nickname resolved (SETNZ on +0 at the end).
    bool ParseKeyCmdSection(INI_Reader& Reader, FKeyCmd* Cmd)
    {
        Cmd->NicknameId = 0;

        while (Reader.read_value())
        {
            if (Reader.is_value("nickname"))
            {
                // an unknown nickname stores 0 and fails at once.
                Cmd->NicknameId = ResolveNicknameId(Reader.get_value_string());
                if (Cmd->NicknameId == 0)
                    return false;
            }
            else if (Reader.is_value("ids_name"))
            {
                // get_value_float(0) through ftol.
                Cmd->IdsName = static_cast<unsigned>(static_cast<int>(Reader.get_value_float(0)));
            }
            else if (Reader.is_value("ids_info"))
            {
                Cmd->IdsInfo = static_cast<unsigned>(static_cast<int>(Reader.get_value_float(0)));
            }
            else if (Reader.is_value("state"))
            {
                // an empty first parameter means keydown; otherwise
                // every parameter up to the first empty one ORs its bit in (unknown words add
                // nothing), and a zero result becomes keydown.
                unsigned Mask = 0;
                unsigned ParamIndex = 0;
                if (!Reader.is_value_empty(0))
                {
                    do
                    {
                        char const* const StateTok = Reader.get_value_string(ParamIndex);
                        if (_stricmp(StateTok, "keydown") == 0)
                            Mask |= kKeyCmdStateKeyDown;
                        else if (_stricmp(StateTok, "keyup") == 0)
                            Mask |= kKeyCmdStateKeyUp;
                        else if (_stricmp(StateTok, "repeat") == 0)
                            Mask |= kKeyCmdStateRepeat;
                        else if (_stricmp(StateTok, "shift_ignore") == 0)
                            Mask |= kKeyCmdStateShiftIgnore;
                        ++ParamIndex;
                    } while (!Reader.is_value_empty(ParamIndex));
                }
                if (Mask == 0)
                    Mask = kKeyCmdStateKeyDown;
                Cmd->StateMask = Mask;
            }
            else if (Reader.is_value("key"))
            {
                // a zeroed local through the routine, appended at the
                // end of the binding vector (the routine(end, 1, &binding)).
                FKeyBinding Binding;
                Binding.VirtualKey = 0;
                Binding.Modifiers = 0;
                if (ParseKeyValue(Reader, &Binding))
                    Cmd->Bindings.push_back(Binding);
            }
        }

        return Cmd->NicknameId != 0;
    }

    // (thiscall on the KeyCmd, RET 8: binding, event record the field) — state /
    // modifier gate for one binding against the event being routed.
    bool BindingMatchesState(FKeyCmd const* Cmd, FKeyBinding const* Binding,
                             FHudCmdRecord const* Record)
    {
        // record+0x18 (key down) selects state bit 1, else bit 2.
        unsigned const NeedBit =
            Record->Byte18 != 0 ? kKeyCmdStateKeyDown : kKeyCmdStateKeyUp;
        if ((Cmd->StateMask & NeedBit) == 0)
            return false;

        // an auto-repeat key-down (record+0x19) needs the `repeat` bit.
        if (Record->Byte19 != 0 && (Cmd->StateMask & kKeyCmdStateRepeat) == 0)
            return false;

        // state bit 8 accepts any modifiers.
        if ((Cmd->StateMask & kKeyCmdStateShiftIgnore) != 0)
            return true;

        // binding+4 against record+8.
        return Binding->Modifiers == Record->Modifiers;
    }

    // — real body in hud_command_dispatch.cpp (KeyMap handler tree).
    bool DispatchKeyCommand(unsigned NicknameId, int EventKind)
    {
        return HudDispatchCommand(NicknameId, EventKind) != 0;
    }
}

int LoadKeyMapIni(char const* RelPath, bool bFatal)
{
    // (cdecl: path, bFatal). Appends every [KeyCmd] and [KeyMap] of the file to
    // the two lists (nothing is cleared first) and returns the number of [KeyMap] sections
    // seen; a failed open returns 0.
    char FullPath[MAX_PATH];
    GetFileSysPath(FullPath, RelPath, nullptr);

    INI_Reader Reader;
    // the caller's bFatal is open's second argument.
    if (!Reader.open(FullPath, bFatal))
        return 0;

    // a missing or wrong first header is reported and loading goes on.
    if (!Reader.read_header() || !Reader.is_header("KeyMap=1.1"))
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: KeyMap file (%s) is not version 1.1",
              "Client\\Interface\\KeyMap.cpp", 0x2ea, FullPath);
    }

    // the template every new KeyCmd is copied from (the field, built
    // on first use behind the the field bit, freed at exit through the branch): all zero
    // except StateMask = 1.
    static FKeyCmd const DefaultKeyCmd = {0u, 0u, 0u, kKeyCmdStateKeyDown,
                                          std::vector<FKeyBinding>()};

    int GroupCount = 0;
    while (Reader.read_header())
    {
        if (Reader.is_header("KeyCmd"))
        {
            // a copy of the template at the end of the list; if the
            // section does not resolve, that last node is unlinked and destroyed again.
            GKeyCmds.push_back(DefaultKeyCmd);
            if (!ParseKeyCmdSection(Reader, &GKeyCmds.back()))
                GKeyCmds.pop_back();
        }
        else if (Reader.is_header("KeyMap"))
        {
            // count it, append an empty group (a temporary copied in
            // and destroyed), then fill the new last node.
            ++GroupCount;
            GKeyMapGroups.push_back(FKeyMapGroup());
            FKeyMapGroup& Group = GKeyMapGroups.back();
            while (Reader.read_value())
            {
                if (Reader.is_value("nickname"))
                {
                    // std::string assign of the whole value.
                    Group.Nickname = Reader.get_value_string();
                }
                else if (Reader.is_value("key"))
                {
                    // the routine(get_value_string(0)); a hit is
                    // appended to the group's list (the routine at the end).
                    FKeyCmd* const Cmd = FindKeyCmdByNickname(Reader.get_value_string(0));
                    if (Cmd != nullptr)
                        Group.Commands.push_back(Cmd);
                }
            }
        }
    }

    return GroupCount;
}

int LoadUserKeyMapIni(char const* Path)
{
    // (cdecl, 1 arg). The path goes through GetFileSysPath like the base map.
    char FullPath[MAX_PATH];
    GetFileSysPath(FullPath, Path, nullptr);

    INI_Reader Reader;
    if (!Reader.open(FullPath, false))
        return 0;

    // without a leading [KeyMap=1.1] nothing changes and 0 returns.
    if (!Reader.read_header() || !Reader.is_header("KeyMap=1.1"))
        return 0;

    // every KeyCmd's binding vector emptied.
    for (std::list<FKeyCmd>::iterator It = GKeyCmds.begin(); It != GKeyCmds.end(); ++It)
        It->Bindings.clear();

    // per [KeyCmd] section, "nickname" selects the existing KeyCmd
    // (the first with that id, or none) and every later "key" is appended to it while one is
    // selected. Nothing else in the section is read. Every [KeyCmd] section is counted.
    int SectionCount = 0;
    while (Reader.read_header())
    {
        if (!Reader.is_header("KeyCmd"))
            continue;

        FKeyCmd* Cmd = nullptr;
        while (Reader.read_value())
        {
            if (Reader.is_value("nickname"))
            {
                Cmd = FindKeyCmdByNicknameId(ResolveNicknameId(Reader.get_value_string()));
            }
            else if (Reader.is_value("key") && Cmd != nullptr)
            {
                FKeyBinding Binding;
                Binding.VirtualKey = 0;
                Binding.Modifiers = 0;
                if (ParseKeyValue(Reader, &Binding))
                    Cmd->Bindings.push_back(Binding);
            }
        }
        ++SectionCount;
    }

    return SectionCount;
}

int SaveUserKeyMapIni(char const* FullPath)
{
    // — listing 0x005772d0..0x005773ac. Writes only KeyCmds that still have
    // at least one binding; nickname 0 (USER_NONE) is skipped. The tail from 0x005773ad (the
    // modifier suffix fprintf, the loop step and fclose) is the analysis-split the branch.
    FILE* File = fopen(FullPath, "w");
    if (File == nullptr)
        return 0;

    fprintf(File, "; OutpostFrontier - User Key Commands\n");
    fprintf(File, "\n[KeyMap=1.1]\n");

    for (std::list<FKeyCmd>::iterator It = GKeyCmds.begin(); It != GKeyCmds.end(); ++It)
    {
        FKeyCmd* const Cmd = &*It;
        if (Cmd->Bindings.empty())
            continue;
        if (Cmd->NicknameId == 0)
            continue;

        char const* Nick = (Cmd->NicknameId < kKeyCmdNicknameCount)
                               ? GKeyCmdNicknames[Cmd->NicknameId]
                               : "USER_NONE";
        fprintf(File, "\n[KeyCmd]\n");
        fprintf(File, "nickname = %s\n", Nick);

        unsigned BindIndex = 0;
        for (; BindIndex < Cmd->Bindings.size(); ++BindIndex)
        {
            FKeyBinding const& Binding = Cmd->Bindings[BindIndex];
            fprintf(File, "key = %d", Binding.VirtualKey);
            if (Binding.Modifiers == kKeyModShift)
                fprintf(File, ",SHIFT\n");
            else if (Binding.Modifiers == kKeyModControl)
                fprintf(File, ",CONTROL\n");
            else if (Binding.Modifiers == kKeyModAlt)
                fprintf(File, ",ALT\n");
            else
                fprintf(File, "\n");
        }
    }

    fclose(File);
    return 1;
}

char const* GetKeyMapIniRelPath()
{
    // — returns pointer to the static "interface/KeyMap.ini" string.
    return "interface/KeyMap.ini";
}

char const* EnsureUserKeyMapPath()
{
    // — the field. Build once: GetUserDataPath + "/UserKeyMap.ini".
    if (GUserKeyMapPath[0] == '\0')
    {
        GetUserDataPath(GUserKeyMapPath);
        // unconditional strcat of "/UserKeyMap.ini" (16 bytes with NUL).
        size_t Len = strlen(GUserKeyMapPath);
        memcpy(GUserKeyMapPath + Len, "/UserKeyMap.ini", 16);
    }
    return GUserKeyMapPath;
}

FKeyCmd* FindKeyCmdByNickname(char const* Nickname)
{
    // — stricmp against PTR_s_USER_NONE table, then walk the field.
    return FindKeyCmdByNicknameId(ResolveNicknameId(Nickname));
}

bool KeyMapFindCommandBinding(unsigned NicknameId, FKeyBindingLookup* Out, unsigned BindingIndex)
{
    // walk the field; every command with this id writes Out->NicknameId, and
    // the first whose binding list reaches BindingIndex fills VK / modifiers and wins.
    for (std::list<FKeyCmd>::iterator It = GKeyCmds.begin(); It != GKeyCmds.end(); ++It)
    {
        FKeyCmd* const Cmd = &*It;
        if (Cmd->NicknameId != NicknameId)
            continue;
        Out->NicknameId = NicknameId;
        if (BindingIndex < Cmd->Bindings.size())
        {
            Out->VirtualKey = Cmd->Bindings[BindingIndex].VirtualKey;
            Out->Modifiers = Cmd->Bindings[BindingIndex].Modifiers;
            return true;
        }
    }
    return false;
}

bool KeyMapFindPrimaryBinding(unsigned NicknameId, FKeyBindingLookup* Out)
{

    return KeyMapFindCommandBinding(NicknameId, Out, 0);
}

bool KeyMapFindAltBinding(unsigned NicknameId, FKeyBindingLookup* Out)
{

    return KeyMapFindCommandBinding(NicknameId, Out, 1);
}

FKeyCmd* FindKeyCmdById(unsigned NicknameId)
{
    // the first command with this id (its +8 payload), or null.
    return FindKeyCmdByNicknameId(NicknameId);
}

unsigned KeyMapGetModifierState()
{
    // GetKeyState high bit of VK_SHIFT -> 1, VK_CONTROL -> 4, VK_MENU -> 0x10.
    unsigned Mods = 0;
    if (GetKeyState(VK_SHIFT) < 0)
        Mods = 1;
    if (GetKeyState(VK_CONTROL) < 0)
        Mods |= 4;
    if (GetKeyState(VK_MENU) < 0)
        Mods |= 0x10;
    return Mods;
}

bool UpdateKeyCmdBinding(unsigned NicknameId, int NewVirtualKey, unsigned NewModifiers,
                         int OldVirtualKey, unsigned OldModifiers)
{
    // (actionIndex, newVk, newMod, oldVk, oldMod).
    // Vector growth helpers (the routine / the routine / the routine) collapse to
    // std::vector; IDPMsgHandler::OnReceive on reallocation is COMDAT container glue —
    // not reproduced (vector owns its buffer).
    FKeyCmd* Cmd = FindKeyCmdByNicknameId(NicknameId);
    if (Cmd == nullptr)
        return false;

    unsigned BindIndex = 0;
    for (; BindIndex < Cmd->Bindings.size(); ++BindIndex)
    {
        FKeyBinding& Binding = Cmd->Bindings[BindIndex];
        if (Binding.VirtualKey != OldVirtualKey || Binding.Modifiers != OldModifiers)
            continue;

        if (NewVirtualKey != 0)
        {
            Binding.VirtualKey = NewVirtualKey;
            Binding.Modifiers = NewModifiers;
            return true;
        }

        Cmd->Bindings.erase(Cmd->Bindings.begin() + static_cast<int>(BindIndex));
        return true;
    }

    // Not found: insert only when NewVirtualKey != 0 and the vector is "small"
    // (baseline gate: size < 2 before grow — listing 00578220 after the find-miss fallthrough).
    if (NewVirtualKey == 0)
        return false;
    if (Cmd->Bindings.size() >= 2)
        return false;

    FKeyBinding Added = {};
    Added.VirtualKey = NewVirtualKey;
    Added.Modifiers = NewModifiers;
    Cmd->Bindings.push_back(Added);
    return false; // baseline returns low-byte 0 on the insert-via-grow path
}

FKeyMapGroup* FindKeyMapGroup(char const* GroupNickname)
{
    // (cdecl) — the first group whose nickname stricmp-matches (an unset string
    // reads as ""), its node payload; none -> null.
    for (std::list<FKeyMapGroup>::iterator It = GKeyMapGroups.begin(); It != GKeyMapGroups.end();
         ++It)
    {
        if (_stricmp(It->Nickname.c_str(), GroupNickname) == 0)
            return &*It;
    }
    return nullptr;
}

namespace
{
    void EnsureKeyMapLoaded()
    {
        // Lazy-load block inlined at the head of both the routine (00577490..00577505) and
        // (0057759e..00577610): the field is set only after a successful
        // ; a failed load leaves it clear so the next call retries.
        if (G_bKeyMapLoaded)
            return;
        int Loaded = LoadKeyMapIni("interface/KeyMap.ini", true);
        if (Loaded == 0)
            return; // baseline still resolves the group after a failed load
        G_bKeyMapLoaded = true;
        LoadUserKeyMapIni(EnsureUserKeyMapPath());
    }
}

bool RegisterAcceleratorGroup(char const* GroupNickname)
{
    // .
    EnsureKeyMapLoaded();

    FKeyMapGroup* const Group = FindKeyMapGroup(GroupNickname);
    if (Group == nullptr)
    {
        MessageBoxA(nullptr, "Invalid accelerator", "Error:", MB_OK);
        return false;
    }

    // already active -> true.
    for (std::list<FKeyMapGroup*>::iterator It = GActiveGroups.begin();
         It != GActiveGroups.end(); ++It)
    {
        if (*It == Group)
            return true;
    }
    // links the new node before head->next (std::list
    // insert(begin())): the newest group sits at the front and every walker
    // (the routine, the routine, 00577a59) goes head->next forward, so the most
    // recently registered group wins.
    GActiveGroups.push_front(Group);
    return true;
}

void UnregisterAcceleratorGroup(char const* GroupNickname)
{
    // .
    // empty active list (the field == 0) returns before any
    // load, lookup or error box.
    if (GActiveGroups.empty())
        return;
    EnsureKeyMapLoaded();

    FKeyMapGroup* const Group = FindKeyMapGroup(GroupNickname);
    if (Group == nullptr)
    {
        MessageBoxA(nullptr, "Invalid accelerator", "Error:", MB_OK);
        return;
    }

    // the first node holding the group is unlinked and freed
    // (the routine), size - 1; none -> nothing.
    for (std::list<FKeyMapGroup*>::iterator It = GActiveGroups.begin();
         It != GActiveGroups.end(); ++It)
    {
        if (*It != Group)
            continue;
        GActiveGroups.erase(It);
        return;
    }
}

bool KeyMapRouteInputMessage(unsigned Message, unsigned Wparam, unsigned Lparam)
{
#ifdef OUTPOST_FRONTIER_WALK_MODE
    if (BaseWalkHandleKey(Message, Wparam))
        return true;
#endif
    // (cdecl, 3 args) -- listing 00577850..00577b1c. Every message the pump reads
    // comes through here; true means consumed (the caller skips Translate/Dispatch).
    FHudCmdRecord* const Record = HudCommandRecord();

    // / 0057786c: local_14 = 1 and record+0x19 (the field) = 0 before the switch,
    // so every message clears the auto-repeat flag, including the ones that fall out below.
    int Mode = 1;
    Record->Byte19 = 0;

    // only 0x100..0x105; 0x103 (WM_DEADCHAR) shares the "false" arm.
    unsigned Kind = 0;
    switch (Message)
    {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        // key = wParam, repeat = (lParam & 0xf0000000) == 0x40000000
        // (previous state down, transition 0, no context bit), key-down byte set at 005778ea.
        Record->Key = Wparam;
        Record->Byte19 = (Lparam & 0xF0000000u) == 0x40000000u ? 1 : 0;
        Kind = 1;
        Record->Byte18 = 1;
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        // same key/repeat, key-down byte cleared.
        Record->Key = Wparam;
        Record->Byte18 = 0;
        Record->Byte19 = (Lparam & 0xF0000000u) == 0x40000000u ? 1 : 0;
        Kind = 1;
        break;
    case WM_CHAR:
        // no key, kind 2, key-down byte set.
        Record->Key = 0;
        Kind = 2;
        Record->Byte18 = 1;
        break;
    default:
        return false;
    }

    // the raw message, wParam and lParam into record +0x0c/+0x10/+0x14.
    Record->Field0C = Message;
    Record->Arg10 = Wparam;
    Record->Arg14 = Lparam;

    // GetKeyState high bit of Shift / Ctrl / Alt into record+8.
    unsigned Modifiers = 0;
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0)
        Modifiers = 1;
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
        Modifiers = Modifiers | 4u;
    if ((GetKeyState(VK_MENU) & 0x8000) != 0)
        Modifiers = Modifiers | 0x10u;
    Record->Modifiers = Modifiers;

    // kind 0 cannot reach here; kinds 1..2 dispatch in mode 1, anything
    // else in mode 2 (local_14 = 2 as well).
    if (Kind == 0)
        return false;
    if (Kind > 2)
        Mode = 2;

    // the raw kind (1 = key, 2 = char) goes to the handler tree first.
    if (HudDispatchCommand(Kind, Mode) != 0)
        return true;

    // no key (WM_CHAR) skips the held-key list and the group walk.
    if (Record->Key != 0)
    {
        // remember the live modifiers; restore them only when a held key
        // lent its own.
        bool bRestoreModifiers = false;
        unsigned const LiveModifiers = Record->Modifiers;

        // key events that are not Shift/Ctrl/Alt themselves and not an
        // auto-repeat.
        unsigned const Key = Record->Key;
        if (Kind == 1 && Key != VK_SHIFT && Key != VK_CONTROL && Key != VK_MENU &&
            Record->Byte19 == 0)
        {
            if (Record->Byte18 != 0)
            {
                // key down -- the routine inserts {key, modifiers} before
                // the sentinel (push_back).
                FKeyMapHeldKey Held;
                Held.Key = Key;
                Held.Modifiers = LiveModifiers;
                GHeldKeys.push_back(Held);
            }
            else
            {
                // key up -- every held node with this key lends its
                // modifiers to the record and is unlinked; the walk continues past an erase,
                // so the last match wins. A zero key in a node is skipped (00577a05).
                std::list<FKeyMapHeldKey>::iterator It = GHeldKeys.begin();
                while (It != GHeldKeys.end())
                {
                    if (It->Key != 0 && It->Key == Record->Key)
                    {
                        Record->Modifiers = It->Modifiers;
                        It = GHeldKeys.erase(It);
                        bRestoreModifiers = true;
                        continue;
                    }
                    ++It;
                }
            }
        }

        // active accelerator groups (the field, newest first) -> each
        // group's KeyCmd list (+0x14) -> each binding (+0x14..+0x18, 8 bytes). The key is
        // re-read from the record for every binding (00577a90).
        if (Record->Key != 0)
        {
            for (std::list<FKeyMapGroup*>::iterator GroupIt = GActiveGroups.begin();
                 GroupIt != GActiveGroups.end(); ++GroupIt)
            {
                FKeyMapGroup* const Group = *GroupIt;
                for (std::list<FKeyCmd*>::iterator CmdIt = Group->Commands.begin();
                     CmdIt != Group->Commands.end(); ++CmdIt)
                {
                    FKeyCmd* const Cmd = *CmdIt;
                    unsigned BindIndex = 0;
                    for (; BindIndex < Cmd->Bindings.size(); ++BindIndex)
                    {
                        FKeyBinding const* Binding = &Cmd->Bindings[BindIndex];
                        if (static_cast<unsigned>(Binding->VirtualKey) != Record->Key)
                            continue;
                        if (!BindingMatchesState(Cmd, Binding, Record))
                            continue;
                        // the routine(KeyCmd nickname id, local_14);
                        // consumed returns true at once -- the lent modifiers are NOT
                        // restored on this path (00577b09).
                        if (DispatchKeyCommand(Cmd->NicknameId, Mode))
                        {
                            return true;
                        }
                    }
                }
            }
        }

        // .
        if (bRestoreModifiers)
            Record->Modifiers = LiveModifiers;
    }

    // a key event for VK_MENU itself is always consumed.
    if (Kind == 1 && Wparam == VK_MENU)
        return true;
    return false;
}

bool KeyMapRouteMouseEvent(FHudCmdRecord const* Event)
{
    // (cdecl, 1 arg) -- listing 00577680..00577821, called only by the routine
    // (0x0041f106) for button and wheel events.
    FHudCmdRecord* const Record = HudCommandRecord();

    // the event kind from the key (switch table 0x00577824, key + 9).
    unsigned Kind = 0;
    switch (static_cast<int>(Event->Key))
    {
    case -9:
        Kind = 4;
        break;
    case -8:
        Kind = 0xd;
        break;
    case -7:
        Kind = 0xc;
        break;
    case -6:
        Kind = 0xb;
        break;
    case -5:
        Kind = 0xa;
        break;
    case -4:
        Kind = 9;
        break;
    case -3:
        Kind = 8;
        break;
    case -2:
        Kind = 7;
        break;
    case -1:
        Kind = 6;
        break;
    default:
        break;
    }

    // seven dwords into the field, then +0x0c / +0x10 / +0x14 cleared.
    *Record = *Event;
    Record->Field0C = 0;
    Record->Arg10 = 0;
    Record->Arg14 = 0;

    // GetKeyState high bit of Shift / Ctrl / Alt into record+8.
    unsigned Modifiers = 0;
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0)
        Modifiers = 1;
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
        Modifiers = Modifiers | 4u;
    if ((GetKeyState(VK_MENU) & 0x8000) != 0)
        Modifiers = Modifiers | 0x10u;
    Record->Modifiers = Modifiers;

    // the kind to the handler tree in mode 2.
    if (DispatchKeyCommand(Kind, 2))
        return true;

    // no key -> not consumed; else every binding on the key in the active
    // groups, gated like the routine, dispatches its KeyCmd in mode 2. The record's flags are
    // re-read after every dispatch (0x005777e2).
    if (Record->Key == 0)
        return false;

    for (std::list<FKeyMapGroup*>::iterator GroupIt = GActiveGroups.begin();
         GroupIt != GActiveGroups.end(); ++GroupIt)
    {
        FKeyMapGroup* const Group = *GroupIt;
        for (std::list<FKeyCmd*>::iterator CmdIt = Group->Commands.begin();
             CmdIt != Group->Commands.end(); ++CmdIt)
        {
            FKeyCmd* const Cmd = *CmdIt;
            unsigned BindIndex = 0;
            for (; BindIndex < Cmd->Bindings.size(); ++BindIndex)
            {
                FKeyBinding const* Binding = &Cmd->Bindings[BindIndex];
                if (static_cast<unsigned>(Binding->VirtualKey) != Record->Key)
                    continue;
                if (!BindingMatchesState(Cmd, Binding, Record))
                    continue;
                if (DispatchKeyCommand(Cmd->NicknameId, 2))
                    return true;
            }
        }
    }
    return false;
}

unsigned KeyMapGetActionNicknameId(char const* Nickname)
{
    return ResolveNicknameId(Nickname);
}

char const* KeyMapGetActionNickname(unsigned NicknameId)
{
    if (NicknameId >= kKeyCmdNicknameCount)
        return GKeyCmdNicknames[0];
    return GKeyCmdNicknames[NicknameId];
}

unsigned KeyMapGetActiveGroupCount()
{
    return static_cast<unsigned>(GActiveGroups.size());
}

bool KeyMapFindActiveBinding(unsigned NicknameId, FKeyBindingLookup* Out)
{
    // (cdecl) → the routine(group, NicknameId, Out, 0) per active group, newest
    // first, until one succeeds. the routine walks the group's KeyCmd list: a command with the
    // id writes Out->NicknameId, and succeeds when its binding vector is allocated and holds
    // the index (0x00577d80..0x00577d9a); otherwise the walk goes on. Nothing is null-tested.
    for (std::list<FKeyMapGroup*>::iterator GroupIt = GActiveGroups.begin();
         GroupIt != GActiveGroups.end(); ++GroupIt)
    {
        FKeyMapGroup* const Group = *GroupIt;
        for (std::list<FKeyCmd*>::iterator CmdIt = Group->Commands.begin();
             CmdIt != Group->Commands.end(); ++CmdIt)
        {
            FKeyCmd* const Cmd = *CmdIt;
            if (Cmd->NicknameId != NicknameId)
                continue;
            Out->NicknameId = NicknameId;
            if (Cmd->Bindings.empty())
                continue;

            FKeyBinding const& Binding = Cmd->Bindings[0];
            Out->VirtualKey = Binding.VirtualKey;
            Out->Modifiers = Binding.Modifiers;
            return true;
        }
    }
    return false;
}

bool KeyMapIsCommandHeld(unsigned NicknameId)
{
    // . The first command in the field with this id; none, or no bindings -> false.
    FKeyCmd* const Cmd = FindKeyCmdByNicknameId(NicknameId);
    if (Cmd == nullptr)
        return false;

    unsigned Index = 0;
    for (; Index < Cmd->Bindings.size(); ++Index)
    {
        FKeyBinding const& Binding = Cmd->Bindings[Index];

        // Shift 1, Ctrl 4, Alt 0x10, re-read for every binding.
        unsigned Modifiers = 0;
        if ((GetKeyState(VK_SHIFT) & 0x8000) != 0)
            Modifiers = 1;
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
            Modifiers = Modifiers | 4u;
        if ((GetKeyState(VK_MENU) & 0x8000) != 0)
            Modifiers = Modifiers | 0x10u;
        if ((Modifiers & Binding.Modifiers) != Binding.Modifiers)
            continue;

        if (Binding.VirtualKey >= 0)
        {
            // the key's own high bit.
            if ((GetKeyState(Binding.VirtualKey) & 0x8000) != 0)
                return true;
            continue;
        }

        // mouse buttons -1/-2/-3 -> bits 1/2/4 of the field; any
        // other negative key tests mask 0 and never matches.
        unsigned Mask = 0;
        if (Binding.VirtualKey == -3)
            Mask = 4;
        else if (Binding.VirtualKey == -2)
            Mask = 2;
        else if (Binding.VirtualKey == -1)
            Mask = 1;
        if ((GetClientMouseButtons() & Mask) != 0)
            return true;
    }
    return false;
}

void KeyMapFormatBindingName(FKeyBindingLookup const* Binding, wchar_t* Dest, unsigned DestChars)
{
    // — MapVirtualKey + GetKeyNameTextW; IDS fallbacks on miss; modifier prefix.
    if (Binding == nullptr || Dest == nullptr || DestChars == 0)
        return;
    Dest[0] = L'\0';

    unsigned const Vk = static_cast<unsigned>(Binding->VirtualKey);
    wchar_t KeyName[256];
    KeyName[0] = L'\0';

    if (Vk < 0x101u)
    {
        UINT const Scan = MapVirtualKeyA(Vk, MAPVK_VK_TO_VSC);
        LONG LParam = static_cast<LONG>(Scan << 16);
        bool bExtended = false;
        if (Vk == 0x26 || Vk == 0x23 || Vk == 0x24 || Vk == 0x25 || Vk == 0x27 || Vk == 0x28
            || Vk == 0x2d || Vk == 0x2e || Vk == 0x21 || Vk == 0x22)
            bExtended = true;

        if (Scan != 0)
        {
            if (bExtended || GetKeyNameTextW(LParam, KeyName, 256) == 0)
            {
                LParam |= 0x1000000;
                if (GetKeyNameTextW(LParam, KeyName, 256) == 0)
                    KeyName[0] = L'\0';
            }
        }
        if (KeyName[0] == L'\0')
            GetIdsStringWWithRawFallback(0x3ae, KeyName, 256);
    }
    else
    {
        unsigned Ids = 0x3ae;
        switch (static_cast<int>(Binding->VirtualKey))
        {
        case -9:
            Ids = 0x4ea;
            break;
        case -8:
            Ids = 0x3ad;
            break;
        case -7:
            Ids = 0x3ac;
            break;
        case -6:
            Ids = 0x3ab;
            break;
        case -5:
            Ids = 0x3aa;
            break;
        case -4:
            Ids = 0x3a9;
            break;
        case -3:
            Ids = 0x3a8;
            break;
        case -2:
            Ids = 0x3a7;
            break;
        case -1:
            Ids = 0x3a6;
            break;
        default:
            Ids = 0x3ae;
            break;
        }
        GetIdsStringWWithRawFallback(Ids, KeyName, 256);
    }

    if (Binding->Modifiers == 0)
    {
        wcsncpy(Dest, KeyName, DestChars - 1);
        Dest[DestChars - 1] = L'\0';
        return;
    }

    // Modifiers first (Shift/Control/Alt), then key name — listing the branch.
    wchar_t Prefix[256];
    Prefix[0] = L'\0';
    if ((Binding->Modifiers & kKeyModShift) != 0)
    {
        size_t const Len = wcslen(Prefix);
        UINT const Scan = MapVirtualKeyA(0x10, MAPVK_VK_TO_VSC);
        if (GetKeyNameTextW(static_cast<LONG>(Scan << 16), Prefix + Len,
                            static_cast<int>(256 - Len))
            == 0)
            GetIdsStringWWithRawFallback(0x4eb, Prefix + Len, static_cast<unsigned>(256 - Len));
        else
            wcscat(Prefix, L" + ");
    }
    if ((Binding->Modifiers & kKeyModControl) != 0)
    {
        size_t const Len = wcslen(Prefix);
        UINT const Scan = MapVirtualKeyA(0x11, MAPVK_VK_TO_VSC);
        if (GetKeyNameTextW(static_cast<LONG>(Scan << 16), Prefix + Len,
                            static_cast<int>(256 - Len))
            == 0)
            GetIdsStringWWithRawFallback(0x4ec, Prefix + Len, static_cast<unsigned>(256 - Len));
        else
            wcscat(Prefix, L" + ");
    }
    if ((Binding->Modifiers & kKeyModAlt) != 0)
    {
        size_t const Len = wcslen(Prefix);
        UINT const Scan = MapVirtualKeyA(0x12, MAPVK_VK_TO_VSC);
        if (GetKeyNameTextW(static_cast<LONG>(Scan << 16), Prefix + Len,
                            static_cast<int>(256 - Len))
            == 0)
            GetIdsStringWWithRawFallback(0x4ed, Prefix + Len, static_cast<unsigned>(256 - Len));
        else
            wcscat(Prefix, L" + ");
    }

    size_t const PrefixLen = wcslen(Prefix);
    size_t const KeyLen = wcslen(KeyName);
    if (PrefixLen + KeyLen < DestChars)
    {
        wcscpy(Dest, Prefix);
        wcscat(Dest, KeyName);
    }
    else
        GetIdsStringWWithRawFallback(0x3ae, Dest, DestChars);
}
