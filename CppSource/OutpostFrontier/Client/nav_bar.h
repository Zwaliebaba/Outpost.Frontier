// Author: Giperion.

#pragma once

// baseline NavBar (RTTI .?AUNavBar@@ : WinControl, ButtonMover, ButtonScaler;
// Client\GF\NavBar.cpp) -- the base-side hotspot bar: the room-switch
// buttons along the bottom ("ButtonControl1..9" children of the loaded base UI), the service
// tabs above them ("RoomControl1..9": news, missions, repair, dealers, launch), the maneuver frame
// and tab meshes, and the slide / scale animation that opens NN_News, NN_MissionVendor,
// NN_Repair and NN_ShipTrader out of a tab.

// Factory WINFACTORY<NavBar> the field "NavBar" (static init the branch -> the routine,
// atexit the branch -> the routine). operator new 0x3f8, ctor the routine, dtor
// , scalar deleting dtor the routine.

// vftable 0x005cb584 (47 slots) -- WinControl's 0x005e5e44 with: 0 the routine, 9 Create
// , 11 Update the branch, 13 Draw the routine, 29 the branch (FILE* save),
// 42 SetProperty the branch, and two new slots: 45 the branch (OnLocationEntered) and
// 46 the routine (RET 4). IDataViewOwner +0x7c keeps WinControl's 0x005cb574 face.
// ButtonMover +0x32c (0x005cb55c: 0 the branch, 1 the branch) and ButtonScaler +0x34c
// (0x005cb568: 0 the branch, 1 the branch).

// baseline layout (the ButtonMover / ButtonScaler bases hold std::vectors in the port, so the
// member offsets below are baseline's, not this class's):
//   +0x36c std::string VirtualRoom        +0x3c8 VirtualRoomLocation
//   +0x37c std::list<FNavBarButton>       +0x3cc bScriptRunning
//   +0x388 std::map<std::string, char*>   +0x3d0 LayoutDelay
//   +0x39c DefaultMesh                    +0x3d4 bRepairRedirect   +0x3d5 bReturnToDesk
//   +0x3a0 / +0x3ac std::list<std::string> +0x3d8 ManeuverFrame   +0x3dc ManeuverTab
//   +0x3b8 Current                        +0x3e0 PendingCharacter  +0x3e4 bCharacterPending
//   +0x3bc bFadingOut  +0x3c0 FadeTimer   +0x3e8 ScaledBackground  +0x3ec ClosingButton
//   +0x3c4 bFadingIn   +0x3c5 bVirtualRoomPending  +0x3f0 QueuedClose  +0x3f4 QueuedOpen

#include "button_mover.h"
#include "wincontrol_base.h"

#include <list>
#include <map>
#include <stdio.h>
#include <string>

struct RoomButtonInfo;
struct FCharacterObject;

// baseline 0x34-byte list element (ctor the routine).
struct FNavBarButton
{
    // everything 0 except bRoomSwitch = 1; Position is left unwritten.
    FNavBarButton();

    unsigned char bDisabled;         // +0x00 ctor 0
    unsigned char bRoomSwitch;       // +0x01 ctor 1: a room button ("ButtonControlN"), else a
                                     // service tab ("RoomControlN")
    std::string Name;                // +0x04 hotspot name, the IniValues key
    char const* IdsName;             // +0x14 the IDS_HOTSPOT_* name looked up in the field
    WinControl* Control;             // +0x18 the RoomControlN / ButtonControlN child
    RoomButtonInfo const* Hotspot;   // +0x1c the RoomData hotspot, null for the launch tab
    unsigned char bOpen;             // +0x20 its service window is open (slid out)
    float Position[3];               // +0x24 the laid-out offset (the routine)
    unsigned Type;                   // +0x30 a location id (room buttons) or hotspot behaviour
};

class NavBar : public WinControl, public ButtonMover, public ButtonScaler
{
public:

    NavBar();

    ~NavBar() override;

    // --- Win3D / WinControl overrides ---
    // slot 9 the branch (RET 8)
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 11 the branch
    void Update() override;
    // slot 13 the routine
    void Draw() override;
    // slot 29 the branch (RET 4) -- the [NavBar] section of IniValues written to File.
    void Slot29(FILE* File) override;
    // slot 42 the branch (RET 0xc) -- only Msg 0x26 (a button clicked, A = the control).
    intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override;

    // --- new slots ---
    // slot 45 (+0xb4) the branch -- rebuild the buttons for the room just entered.
    virtual void OnLocationEntered();
    // slot 46 (+0xb8) the routine -- RET 4.
    virtual void Slot46(unsigned A);

    // --- ButtonMover / ButtonScaler ---
    // (this = the +0x32c face)
    void OnMoveFinished() override;
    // (this = the +0x34c face)
    void OnScaleFinished() override;

    // per button enabled state and room highlight, then the routine and
    // (1).
    void Refresh();
    // (RET 4) -- lay the service tabs out above the selected room button.
    void LayoutTabs(bool bAnimate);
    // lay the room buttons out and pick the maneuver frame mesh.
    void LayoutRoomButtons();
    // (RET 4) -- the repair tab enabled only while the ship needs repair
    // (the routine) and bForce (force disabled) is clear.
    void RefreshRepairTab(bool bForce);
    // SetProperty(0x4e) on NN_Repair, NN_ShipTrader, NN_News, NN_MissionVendor.
    static void NotifyServiceWindows();
    // (RET 8) -- IniValues[Name] (a new 0x80-byte buffer on first use) = Value.
    bool SetIniValue(char const* Name, char const* Value);
    // rebuild the button list from the current room's hotspots.
    void Rebuild();
    // (RET 0x14) -- push a button onto the list, bound to the next free
    // RoomControl / ButtonControl child; null when no such child exists.
    FNavBarButton* AddButton(unsigned Type, int Unused, unsigned char bRoomSwitch,
                             char const* Name, char const* IdsName);
    // (RET 4) -- fade out to Button's room, or (second call) enter it.
    void SelectRoomButton(FNavBarButton* Button);
    // the desk camera of Current's room, then fade back in.
    void CameraToDesk();
    // (RET 4) -- slide a service tab out (queued while animating).
    void OpenService(FNavBarButton* Button);
    // (RET 4) -- shrink the service background (queued while animating).
    void CloseService(FNavBarButton* Button);
    // (thiscall, RET 4; this not read) -- the background scale of a service type.
    static float ServiceScale(unsigned Type);
    // (RET 8) -- the service type's background at Scale (the routine).
    void MakeServiceBackground(unsigned Type, float Scale);
    // (RET 4) -- a button's action.
    void OnButtonClicked(FNavBarButton* Button);
    // (RET 8) -- the state transition script From -> To, or To's camera move.
    void ActivateButton(FNavBarButton* From, FNavBarButton* To);
    // (RET 4) -- swap which of the desk (type 6) / dealer (5, 7, 8) tabs is shown.
    void ToggleDealerTabs(bool bShowDesk);
    // (RET 0xc) -- play Name as a Gf script sequence (ScriptDone at its stop).
    void StartScript(char const* Name, unsigned char bFade, unsigned char bLetterbox);
    // (cdecl) -- the stop callback of StartScript and of the camera tracks.
    static void __cdecl ScriptDone(void* Bar, void* Unused, unsigned char bArg);
    // (RET 8) -- a room character was clicked; true when the bar took the click.
    bool CharacterClicked(FCharacterObject* Character, int Role);
    // (RET 0xc) -- replace the "ScaledBackground" child and scale its instance.
    void MakeScaledBackground(float Scale, char const* Mesh, float const* Position);

    bool IsAnimating() const { return ButtonMover::IsActive() || ButtonScaler::IsActive(); }

    std::string VirtualRoom;                // +0x36c
    std::list<FNavBarButton> Buttons;       // +0x37c
    std::map<std::string, char*> IniValues; // +0x388 values: operator new(0x80)
    char const* DefaultMesh;                // +0x39c set by Create, not by the ctor
    std::list<std::string> RoomControlNames;   // +0x3a0
    std::list<std::string> ButtonControlNames; // +0x3ac
    FNavBarButton* Current;                 // +0x3b8
    unsigned char bFadingOut;               // +0x3bc
    float FadeTimer;                        // +0x3c0
    unsigned char bFadingIn;                // +0x3c4
    unsigned char bVirtualRoomPending;      // +0x3c5
    unsigned VirtualRoomLocation;           // +0x3c8
    unsigned char bScriptRunning;           // +0x3cc
    float LayoutDelay;                      // +0x3d0
    unsigned char bRepairRedirect;          // +0x3d4
    unsigned char bReturnToDesk;            // +0x3d5
    WinControl* ManeuverFrame;              // +0x3d8
    WinControl* ManeuverTab;                // +0x3dc
    FCharacterObject* PendingCharacter;     // +0x3e0
    unsigned char bCharacterPending;        // +0x3e4
    WinControl* ScaledBackground;           // +0x3e8
    FNavBarButton* ClosingButton;           // +0x3ec
    FNavBarButton* QueuedClose;             // +0x3f0
    FNavBarButton* QueuedOpen;              // +0x3f4

private:
    NavBar(NavBar const&) = delete;
    NavBar& operator=(NavBar const&) = delete;
};

// --- cdecl entry points (RoomControl / NavBar.cpp) ---
// the "NavBar" window, created when missing, told the room was entered (slot
// 45). Caller: the room load the routine (0x0043a05a).
void NavBarEnsure();
// every "RoomControl1..9" / "ButtonControl1..9" window deleted (no Destroy).
void NavBarDeleteRoomControls();
// the "NavBar" window's background, the room controls and the bar itself
// destroyed. Callers: 0x004393aa, 0x0043b55b, 0x0043b7e9, 0x005739b0.
void NavBarDestroy();
// the bar hands over to the dealer (the routine(Index, 0, 0)).
void NavBarOpenDealer(int Index);
// the first button of Type activated (unless a repair redirect runs).
void NavBarOpenButton(unsigned Type);
// the open service tab closed (the routine first when bNotify).
void NavBarCloseService(bool bNotify);
// a NavBar and one of the four service windows exist.
bool NavBarHasServiceWindow();
// the NavBar's mover or scaler runs (no slot 2 test).
bool NavBarIsAnimating();
// the routine(bForce) on the bar.
void NavBarRefreshRepairTab(bool bForce);
// the routine on the bar; false without one.
bool NavBarCharacterClicked(FCharacterObject* Character, int Role);
// the routine(0) on the bar.
void NavBarShowDealerTab();
