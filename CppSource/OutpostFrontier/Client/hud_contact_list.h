// Author: Giperion.

#pragma once

// -------------------------------------------------------------------------------------------
// HUD_ContactList / [ContactList] -- Client\Space\HUD_ContactList.cpp. The contact list the
// target panel swaps in (HUD_Target mode 0) and the formation list (mode 2): up to eight rows of
// icon / range / name, a scroll bar and five filter buttons (important, ships, solars, loot,
// all).

// RTTI HUD_ContactList : HUDItem @0, WinControl @0x38 (IDataViewOwner @0xb4), Event::Handler
// @0x364. Size 0x478 (operator new at 0x004d46b6).
// vftables:
//   HUDItem face 0x005d7a5c (11 slots) -- overrides 0 the routine (scalar deleting dtor around
// ) and 6 the routine Render.
//   WinControl face 0x005d79a4 (45 slots) -- overrides 0 the branch (thunk, ECX -= 0x38, into
// ), 9 the routine Create, 11 the branch Update, 12 the branch
//     UpdateInactive, 13 the branch Draw, 42 the branch SetProperty; every other slot is
//     WinControl's 0x005e5e44 entry.
//   Handler 0x005d798c -- the branch OnEvent.

// Factory: the static HUDFACTORY<HUD_ContactList> the field ("ContactList"; XC entry
// runs the ctor the routine and registers the branch with atexit ->
// , the unlink), vftable whose Create the routine is operator new(0x478) +
// .
// -------------------------------------------------------------------------------------------

#include "event_handler.h"
#include "hud_element_list.h"
#include "wincontrol_base.h"

#include "common_goods.h"   // BaseWatcher

#include <list>
#include <stdint.h>

class CEquip;
struct CShip;
struct CSimple;
class FClientGameobj;
class INI_Reader;
class IFont;
class UiScroll;
struct FButtonMontageTile;
struct FFontString;
struct IObjRW;

// "rebuild the contact list on the next Draw". Written 1 by the routine
// (0x004bcbfc; the best-path -> player-waypoint rebuild, not ported yet, so nothing in the
// port raises it), cleared by the ctor (0x004cf590) and by Draw when it consumes it
// (0x004d2532).
extern unsigned char G_bHudContactListRefreshRequested;

// One row source of the list (baseline element 0x1c bytes in a VC6 std::list node of 0x24).
struct FHudContactEntry
{
    // (thiscall, RET 0x14) -- see the .cpp: the watcher takes Obj, and the
    // distance, id and "is the player's target listed" latch (the field) come from the kind.
    FHudContactEntry(int InKind, IObjRW* Obj, float InDistance, unsigned InId, int InIndex);
    // (node copy-construct, and the same body inlined at 0x004d2267 /
    // ): Kind, a fresh watcher on the other's target (BaseWatcher::set_pointer when it
    // watches something, the routine), then Distance, Id, Index and the watch-list byte.
    FHudContactEntry(FHudContactEntry const& Other);
    // the watcher's release (~BaseWatcher); nothing else.

    // Watcher.target() - 8, or null: the IObjRW the row stands for.
    IObjRW* Object() const;

    int Kind;                    // +0x00 0..0xf (icon / tooltip row), see the .cpp
    BaseWatcher Watcher;         // +0x04
    float Distance;              // +0x0c sort key, the range text
    unsigned Id;                 // +0x10 the watched object's get_id
    int Index;                   // +0x14 waypoint / best-path index (kinds 4 / 3)
    unsigned char bWatchListed;  // +0x18 the object is on the client watch list
    unsigned char Pad19[3];

private:
    FHudContactEntry& operator=(FHudContactEntry const&) = delete;
};

class HUD_ContactList : public HUDItem, public WinControl, public EventHandler
{
public:
    // (thiscall, RET 4) -- the reader is not read.
    explicit HUD_ContactList(INI_Reader& Reader);
    // (scalar deleting the routine; WinControl-face thunk the branch)
    ~HUD_ContactList() override;

    // HUDItem slot 6 the routine (RET 8) -- follow the target panel's mode: shown in modes 0
    // and 2; a mode change swaps the filter buttons for the formation title.
    void Render(FSpaceCamera* Camera, float DeltaSeconds) override;

    // WinControl slot 9 the routine (RET 8) -- build the rows, the filter buttons and the scroll
    // bar; look the icon tiles up.
    bool Create(Win3D* NewParent, void* Data) override;
    // WinControl slot 11 the branch -- Win3D's child walk unless the mouse pick is blocked.
    void Update() override;
    // WinControl slot 12 the branch -- show the window when it should be up, then Win3D's
    // inactive child walk.
    void UpdateInactive() override;
    // WinControl slot 13 the branch -- rebuild every 3 s (or when dirty), then refresh or lay
    // out the visible rows and draw.
    void Draw() override;
    // WinControl slot 42 the branch (RET 0xc) -- 0x26 click, 0x27 double click, 0x3e scroll.
    intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override;

    // Event::Handler slot 0 the branch (RET 4) -- 0x1d next / 0x1e previous contact.
    bool OnEvent(unsigned MsgId) override;

    // (thiscall on the HUDItem face, RET 4) -- select row Index of the list.
    void SelectEntry(int Index);
    // (thiscall on the HUDItem face) -- rebuild Contacts.
    void RebuildContacts();
    // (thiscall, RET 4; `this` is not read) -- the row kind of an object, -1 for
    // none. Callers: RebuildContacts 0x004d216e / 0x004d236d.
    int ClassifyContact(CSimple const* Object);
    // (thiscall, RET 8) -- push a ship row for Ship / its client object Gameobj.
    void AddShipEntry(CShip const* Ship, FClientGameobj* Gameobj);

    FButtonMontageTile* Tiles[16];     // +0x368 (WinControl +0x330) per-kind icon tile
    FButtonMontageTile* ChatTile;      // +0x3a8 (+0x370) "interface\HUD\HUD_chat.3db"
    WinControl* Icons[8];              // +0x3ac (+0x374) "ContactListIcon%d"
    WinControl* RangeTexts[8];         // +0x3cc (+0x394) "ContactListRangeText%d"
    WinControl* NameTexts[8];          // +0x3ec (+0x3b4) "ContactListNameText%d"
    UiScroll* Scroll;                  // +0x40c (+0x3d4) "ContactListScroll"
    // +0x410 (+0x3d8) -- baseline VC6 std::list<entry> (allocator byte, head +0x414, size
    // +0x418, 0xc bytes). The port's std::list has another size, so the offsets below are the
    // baseline ones, not this build's.
    std::list<FHudContactEntry> Contacts;
    unsigned ScrollPos;                // +0x41c (+0x3e4) ctor 0; first visible row
    float RebuildTimer;                // +0x420 (+0x3e8) ctor 3.0f
    WinControl* ImportantButton;       // +0x424 (+0x3ec) "ImportantFilterButton"
    WinControl* ShipsButton;           // +0x428 (+0x3f0) "ShipsFilterButton"
    WinControl* SolarButton;           // +0x42c (+0x3f4) "SolarFilterButton"
    WinControl* LootButton;            // +0x430 (+0x3f8) "LootFilterButton"
    WinControl* AllButton;             // +0x434 (+0x3fc) "AllFilterButton"
    int FilterMode;                    // +0x438 (+0x400) 0 important, 1 ships, 2 solars, 3 loot, 4 all
    unsigned char bDirty;              // +0x43c (+0x404) rebuild on the next Draw
    unsigned char Pad43d[3];
    CEquip* Scanner;                   // +0x440 (+0x408) the player ship's scanner (type 0x80000)
    int LastPanelMode;                 // +0x444 (+0x40c) ctor -1; the target panel's last mode
    WinControl* FormationTitle;        // +0x448 (+0x410) "FormationTitle"
    IFont* HudSmallFont;               // +0x44c (+0x414) "HudSmall"
    IFont* PlayerNameFont;             // +0x450 (+0x418) "PlayerName"
    // +0x454 (+0x41c) -- a row's own label (the client object's Font::String +0x48), swapped
    // into the name text around the draw; zeroed at the top of every Draw.
    FFontString* RowLabels[8];
    float NameRefreshTimer;            // +0x474 (+0x43c) ctor 0; > 30 s re-words the names

private:
    HUD_ContactList(HUD_ContactList const&) = delete;
    HUD_ContactList& operator=(HUD_ContactList const&) = delete;
};
