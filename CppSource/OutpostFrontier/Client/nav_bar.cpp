// Author: Giperion.

#include "nav_bar.h"
#ifdef OUTPOST_FRONTIER_WALK_MODE
#include "base_walk.h"
#endif

#include "character_select_equip.h" // GetPlayerShipArchetypeIdbaseline (the routine), visit flags
#include "client_base_market.h"     // GetClientCurrentLocationBaseData (the routine)
#include "fade_to_black.h"          // SetFadeTarget (the routine), GetFadeDuration (the field)
#include "gameflow.h"               // RequestGameflowMissionBriefing (the routine)
#include "gf_character_object.h"    // FCharacterObject, PlacePlayerAtSetpoint (the routine)
#include "gf_object_ship.h"         // GfObjectShip (the routine), PlayLaunchScript (the routine)
#include "gf_script_object.h"       // G_GfScriptManager (the field), FGfScriptSequence
#include "globals.h"                // GetClientFrameTimeCapped (the routine)
#include "hud_command_dispatch.h"   // HudPostCommand (the routine)
#include "hud_maneuvers.h"          // FHudNamedValue, HudLookupNamedValue (the routine)
#include "hud_target.h"             // HudIsMousePickBlocked (the routine)
#include "input_manager.h"          // Get/SetClientMouseCursorDrawFlag (the routine / the routine)
#include "menu_ui_init.h"           // PlayOrRewindUiSoundSlot (the routine)
#include "menu_ui_widgets.h"
#include "nn_rollover.h"            // NnRolloverShowIdsGlobal (the routine)
#include "menu_universe_base.h"     // GetClientCurrentLocationRuntime (the routine / the routine)
#include "mirror_property_bucket.h"
#include "nn_button_control.h"      // NnButtonControl (+0x330 bits)
#include "nn_inventory.h"           // NnInventoryOpenDealer (the routine) / OpenShipDealer (the routine)
#include "nn_messaging.h"           // NnVoiceEnqueue (the routine)
#include "nn_modal.h"               // NnMainButtonOnModalOpen (the routine), G_bNnScreenUp, NnFloatBits
#include "nn_repair.h"              // NnRepair::PlayerNeedsRepair (the routine)
#include "nn_tooltip.h"             // NnToolTipHideGlobal (the routine)
#include "room_control.h"
#include "win3d.h"                  // Win3DFindByName (the routine), Win3DDeleteTree (the routine)
#include "win_factory.h"            // WINFACTORY, WinFactoryCreate (the routine)

#include "NeuronRuntime.h"
#include "common_archetype.h"       // Archetype::GetEquipment
#include "common_csys.h"            // Csys, Acquire3DMathEngine (the routine)
#include "common_goods.h"           // RoomData, RoomButtonInfo, BaseData, EquipDesc
#include "common_id.h"              // CreateID, MakeLocationID
#include "common_ini.h"             // INI_Reader
#include "common_misc.h"            // GetFileSysPath
#include "neuroncomponents.h"                  // FDUMP
#include "engine/sceneruntime.h"
#include "x86math.h"

#include <stdio.h>
#include <string.h>

// WINFACTORY<NavBar> the field (the branch: the routine(&the field, "NavBar"), atexit
// -> the routine).
WINFACTORY<NavBar> G_NavBarFactory("NavBar");

namespace
{
    char const kNavBarCpp[] = "Client\\GF\\NavBar.cpp";

    // the routine's lazily acquired 3DMathEngine (the routine).
    I3DMathEngine* G_NavBarMath = nullptr;

    // (0x3e99999a) -- every slide and scale; the field -- the layout delay.
    float const kAnimSeconds = NnFloatBits(0x3e99999a);
    float const kLayoutDelay = NnFloatBits(0x3e800000);

    // / the branch / the branch -- three float statics of this translation unit
    // (the field = the field = [0x005cb4c4] -0.6, the field = [0x005cb4c8] 0.6). Each
    // has one XREF, its initializer's write; nothing reads them (the same trio as NN_Repair's and
    // NN_MissionVendor's).
    float G_NavBarStatic668824 = NnFloatBits(0xbf19999a);
    float G_NavBarStatic668820 = NnFloatBits(0xbf19999a);
    float G_NavBarStatic66881c = NnFloatBits(0x3f19999a);

    // The launch checks' voice messages, CreateID'd at load (the branch / the branch /
    // ).
    unsigned const G_NavBarVoiceNoEngine = CreateID("must_have_mounted_engine_to_launch");
    unsigned const G_NavBarVoiceNoPower =
        CreateID("must_have_mounted_power_plant_to_launch");
    unsigned const G_NavBarVoiceNoScanner = CreateID("must_have_mounted_scanner_to_launch");

    // Where an opened service tab slides to (the branch / the branch / the branch /
    // ).
    float const G_NavBarRepairTarget[3] = {0.0f, NnFloatBits(0xbeb33333), 0.0f};  // (0xc)
    float const G_NavBarShipTarget[3] = {0.0f, NnFloatBits(0xbeb33333), 0.0f};    // (7)
    float const G_NavBarNewsTarget[3] = {0.0f, NnFloatBits(0xbecccccd), 0.0f};    // (2)
    float const G_NavBarMissionTarget[3] = {0.0f, NnFloatBits(0xbecccccd), 0.0f}; // (3)

    // the maneuver tab mesh for 1..3 service tabs.
    char const* const kTabMeshes[3] = {"Interface\\Baseside\\CITY_maneuvertab1.cmp",
                                       "Interface\\Baseside\\CITY_maneuvertab2.cmp",
                                       "Interface\\Baseside\\CITY_maneuvertab3.cmp"};

    // the tabs' x offsets for 1..3 tabs: the field (.bss, never written),
    // , the field.
    float G_NavBarTabOffset1[1] = {0.0f};
    float const kTabOffset2[2] = {NnFloatBits(0xbc9374bc), NnFloatBits(0x3c9374bc)};
    float const kTabOffset3[3] = {NnFloatBits(0xbd178d50), 0.0f, NnFloatBits(0x3d178d50)};
    float const* const kTabOffsets[3] = {G_NavBarTabOffset1, kTabOffset2, kTabOffset3};

    // the room buttons' x offsets for 2, 3, 4 and 5 buttons, one
    // contiguous run as in baseline (row 2 at [0], row 3 at [2], row 4 at [5], row 5 at [9]).
    // caps its count at 5 but walks every room button, so a sixth..ninth button
    // reads on past row 5 into the pointer table the field (+4..+0x10): those four dwords
    // are kept here with baseline's bits (a ninth button reads index 17, 0x00610c60).
    float const kRowOffsetData[18] = {
        NnFloatBits(0x3d051eb8), NnFloatBits(0xbd051eb8),                          // 2
        NnFloatBits(0x3d851eb8), 0.0f, NnFloatBits(0xbd851eb8),                    // 3
        NnFloatBits(0x3dc7ae14), NnFloatBits(0x3d051eb8), NnFloatBits(0xbd051eb8),
        NnFloatBits(0xbdc7ae14),                                                   // 4
        NnFloatBits(0x3e051eb8), NnFloatBits(0x3d851eb8), 0.0f, NnFloatBits(0xbd851eb8),
        NnFloatBits(0xbe051eb8),                                                   // 5
        NnFloatBits(0x00668830), NnFloatBits(0x00610c1c), NnFloatBits(0x00610c24),
        NnFloatBits(0x00610c30)};                                                  // past row 5
    // row 1 (.bss, never written).
    float G_NavBarRowOffset1[1] = {0.0f};
    // [count] for 1..5; [0] is the dword 0xbe051eb8 (row 5's last float, never
    // dereferenced: a zero count walks no button).
    float const* const kRowOffsets[6] = {nullptr, G_NavBarRowOffset1, &kRowOffsetData[0],
                                         &kRowOffsetData[2], &kRowOffsetData[5],
                                         &kRowOffsetData[9]};

    // the maneuver frame mesh for 1..5 room buttons.
    char const* const kBoxMeshes[5] = {"Interface\\HUD\\HUD_maneuverbox2.cmp",
                                       "Interface\\HUD\\HUD_maneuverbox3.cmp",
                                       "Interface\\HUD\\HUD_maneuverbox4.cmp",
                                       "Interface\\HUD\\HUD_maneuverbox5.cmp",
                                       "Interface\\HUD\\HUD_maneuverbox6.cmp"};

    // 0x13 {IDS name, tooltip ids} pairs (the routine is a stricmp walk).
    FHudNamedValue const kHotspotIds[0x13] = {
        {"IDS_HOTSPOT_EXIT", 0x1965},
        {"IDS_HOTSPOT_BAR", 0x196a},
        {"IDS_HOTSPOT_COMMODITYTRADER", 0x1969},
        {"IDS_HOTSPOT_EQUIPMENTDEALER", 0x1968},
        {"IDS_HOTSPOT_SHIPDEALER", 0x196b},
        {"IDS_HOTSPOT_PLANETSCAPE", 0x1976},
        {"IDS_HOTSPOT_DECK", 0x1977},
        {"IDS_HOTSPOT_LAUNCH", 0x196c},
        {"IDS_NN_REPAIR_YOUR_SHIP", 0x475},
        {"IDS_HOTSPOT_NEWSVENDOR", 0x1966},
        {"IDS_HOTSPOT_MISSIONVENDOR", 0x1967},
        {"IDS_COMMODITYTRADER_NAME", 0x48b},
        {"IDS_DEALER_FRONT_DESK", 0x196e},
        {"IDS_EQUIPMENTDEALER_NAME", 0x48c},
        {"IDS_SHIPDEALER_NAME", 0x48d},
        {"IDS_EQUIPMENT_ROOM_RIGHT", 0x40e},
        {"IDS_HOTSPOT_COMMODITYTRADER_ROOM", 0x527},
        {"IDS_HOTSPOT_EQUIPMENTDEALER_ROOM", 0x528},
        {"IDS_HOTSPOT_SHIPDEALER_ROOM", 0x529}};

    // / the routine (identical bodies) -- the current room's runtime.
    FRoomRuntime* CurrentRoomRuntime()
    {
        return static_cast<FRoomRuntime*>(GetClientCurrentLocationRuntime());
    }

    // then its first dword -- the id of the base the player is at.
    unsigned CurrentBaseId()
    {
        return GetClientCurrentLocationBaseData()->get_base_id();
    }

    // The inline the routine(Name) -> slot 2 -> SetProperty(0x4e, 0, 0) of the routine and
    // OnButtonClicked (e.g. 0x00440e7c..0x00440ea2).
    void NotifyServiceWindow(char const* Name)
    {
        Win3D* const Found = Win3DFindByName(Name);
        if (Found == nullptr)
            return;
        WinControl* const Control = Found->AsWinControl();
        if (Control == nullptr)
            return;
        Control->SetProperty(0x4e, 0, 0);
    }

    // A Win3D shown and parented: [+0x6c] & 2 and [+0x04] (e.g. 0x0044193b..0x00441949).
    bool IsShownWindow(Win3D const* Window)
    {
        return Window != nullptr && (Window->Flags & 2) != 0 && Window->GetParent() != nullptr;
    }

    // ("NavBar") -> slot 2 (the wrappers' shared head, e.g. 0x004427e0..0x004427fa).
    NavBar* FindNavBar()
    {
        Win3D* const Found = Win3DFindByName("NavBar");
        if (Found == nullptr)
            return nullptr;
        WinControl* const Control = Found->AsWinControl();
        if (Control == nullptr)
            return nullptr;
        return static_cast<NavBar*>(Control);
    }

    // (cdecl) -- the player's ship may launch: a copy of the equipment list
    // (the routine) walked for an equipped (+0x0c) engine (class 0xc), power plant (0xb) and
    // scanner (0x14); the first one missing is voiced (the routine) and its rollover shown
    // for 8 seconds (the routine), false. Caller: 0x00440f94.
    bool CheckLaunchEquipment()
    {
        bool bEngine = false;
        bool bPower = false;
        bool bScanner = false;
        std::list<EquipDesc> const Items = GetPlayerEquipDescListbaseline().Items;
        std::list<EquipDesc>::const_iterator It;
        for (It = Items.begin(); It != Items.end(); ++It)
        {
            if (!It->is_equipped())
                continue;
            Archetype::Equipment* const Arch = Archetype::GetEquipment(It->get_arch_id());
            if (Arch == nullptr)
                continue;
            Archetype::AClassType const Class = Arch->get_class_type();
            if (Class == Archetype::AClassPower)
                bPower = true;
            else if (Class == Archetype::AClassEngine)
                bEngine = true;
            else if (Class == Archetype::AClassScanner)
                bScanner = true;
        }
        if (!bEngine)
        {
            NnVoiceEnqueue(G_NavBarVoiceNoEngine, 0);
            NnRolloverShowIdsGlobal(0x49d, NnFloatBits(0x41000000));
            return false;
        }
        if (!bPower)
        {
            NnVoiceEnqueue(G_NavBarVoiceNoPower, 0);
            NnRolloverShowIdsGlobal(0x49e, NnFloatBits(0x41000000));
            return false;
        }
        if (!bScanner)
        {
            NnVoiceEnqueue(G_NavBarVoiceNoScanner, 0);
            NnRolloverShowIdsGlobal(0x49f, NnFloatBits(0x41000000));
            return false;
        }
        return true;
    }

    // The service types the routine / OnButtonClicked close (their jump tables send 2, 3, 7
    // and 0xc to the close, everything else past it).
    bool IsServiceType(unsigned Type)
    {
        return Type == 2 || Type == 3 || Type == 7 || Type == 0xc;
    }
}

FNavBarButton::FNavBarButton()
    : bDisabled(0)
    , bRoomSwitch(1)
    , Name()
    , IdsName(nullptr)
    , Control(nullptr)
    , Hotspot(nullptr)
    , bOpen(0)
    , Type(0)
{
    // +0x24 Position is not written.
}

NavBar::NavBar()
    : WinControl()
    , ButtonMover()
    , ButtonScaler()
    , VirtualRoom()
    , Buttons()
    , IniValues()
    , RoomControlNames()
    , ButtonControlNames()
    , Current(nullptr)
    , bFadingOut(0)
    , FadeTimer(0.0f)
    , bFadingIn(0)
    , bVirtualRoomPending(0)
    , VirtualRoomLocation(0)
    , bScriptRunning(0)
    , LayoutDelay(0.0f)
    , bRepairRedirect(0)
    , bReturnToDesk(0)
    , ManeuverFrame(nullptr)
    , ManeuverTab(nullptr)
    , PendingCharacter(nullptr)
    , bCharacterPending(0)
    , ScaledBackground(nullptr)
    , ClosingButton(nullptr)
    , QueuedClose(nullptr)
    , QueuedOpen(nullptr)
{
    // . +0x39c DefaultMesh is left unwritten (Create sets it).
}

NavBar::~NavBar()
{
    // . 0x0043dab3..0x0043dad0: the background subtree.
    if (ScaledBackground != nullptr)
    {
        Win3DDeleteTree(ScaledBackground);
        ScaledBackground = nullptr;
    }
    // every IniValues buffer freed, then the map emptied. The lists,
    // the string and the mover / scaler vectors follow in reverse declaration order, then
    // WinControl's dtor (the routine).
    std::map<std::string, char*>::iterator It;
    for (It = IniValues.begin(); It != IniValues.end(); ++It)
        delete[] It->second;
    IniValues.clear();
}

bool NavBar::Create(Win3D* NewParent, void* Data)
{
    // (RET 8). 0x0043dc62..0x0043dcdb: "ManeuverFrame" at (0, 0.176, -0.5).
    float const HalfExtents[3] = {0.0f, 0.0f, 0.0f};
    float const Zero338[3] = {0.0f, 0.0f, 0.0f};
    float const FramePosition[3] = {0.0f, NnFloatBits(0x3e343958), NnFloatBits(0xbf000000)};
    ManeuverFrame = CreateStaticControl("ManeuverFrame", "Interface\\HUD\\HUD_maneuverbox6.cmp",
                                        FramePosition, nullptr, HalfExtents, 0, 0, 0, 1, 0, 0, 1,
                                        0, 0, 0, Zero338);
    // "ManeuverTab" at (0, 0.283, -1.0), hidden (property 1).
    float const TabPosition[3] = {0.0f, NnFloatBits(0x3e90624e), NnFloatBits(0xbf800000)};
    ManeuverTab = CreateStaticControl("ManeuverTab", "Interface\\Baseside\\CITY_maneuvertab3.cmp",
                                      TabPosition, nullptr, HalfExtents, 0, 0, 0, 1, 0, 0, 1, 0,
                                      0, 0, Zero338);
    ManeuverTab->SetProperty(1, 1, 0);

    // Win3D::Create (the routine); on failure both children deleted
    // (their pointers are not cleared) and false.
    if (!Win3D::Create(NewParent, Data))
    {
        if (ManeuverFrame != nullptr)
            Win3DDeleteTree(ManeuverFrame);
        if (ManeuverTab != nullptr)
            Win3DDeleteTree(ManeuverTab);
        return false;
    }

    // the layout ini through slot 28.
    char Path[0x100];
    GetFileSysPath(Path, "interface\\baseside\\navbar.ini", nullptr);
    LoadIni(Path);

    // the same file's [NavBar] values into IniValues (the routine).
    INI_Reader Ini;
    if (Ini.open(Path, false) && Ini.find_header("NavBar") && Ini.read_value())
    {
        do
        {
            char const* const Name = Ini.get_name_ptr();
            SetIniValue(Name, Ini.get_value_string());
        } while (Ini.read_value());
    }

    // the mesh an unconfigured button shows.
    DefaultMesh = "interface\\baseside\\unknown.3db";
    return true;
}

void NavBar::OnLocationEntered()
{
    // .
    bReturnToDesk = 0;
    if (CurrentRoomRuntime() == nullptr)
        return;
    if (GetClientCurrentLocationBaseData() == nullptr)
        return;
    Rebuild();
    // the routine again, its result unused.
    CurrentRoomRuntime();
    // Current = the first button whose hotspot reads state 1.
    FNavBarButton* Found = nullptr;
    std::list<FNavBarButton>::iterator It;
    for (It = Buttons.begin(); It != Buttons.end(); ++It)
    {
        if (It->Hotspot != nullptr && It->Hotspot->StateRead == 1)
        {
            Found = &*It;
            break;
        }
    }
    Current = Found;
    Refresh();
}

void NavBar::Slot46(unsigned /*A*/)
{
    // RET 4.
}

void NavBar::Refresh()
{
    // .
    std::list<FNavBarButton>::iterator It;
    if (VirtualRoom.size() == 0)
    {
        // a room button is highlighted (0xc) when it is the current
        // location (the field).
        unsigned const Location = GetClientLocationCurrentRoom();
        for (It = Buttons.begin(); It != Buttons.end(); ++It)
        {
            FNavBarButton& Button = *It;
            if (Button.bRoomSwitch != 0)
            {
                Button.Control->SetProperty(0xc, Button.Type == Location ? 1 : 0, 0);
            }
            else if (Button.Type == 6)
            {
                Button.bDisabled = 1;
                Button.Control->SetProperty(2, 1, 0);
            }
            else if (Button.Type == 0xc)
            {
                bool const bNeedsRepair = NnRepair::PlayerNeedsRepair();
                Button.Control->SetProperty(2, bNeedsRepair ? 0 : 1, 0);
                Button.bDisabled = bNeedsRepair ? 0 : 1;
            }
            else
            {
                Button.Control->SetProperty(2, 0, 0);
            }
        }
    }
    else
    {
        // inside a virtual room the room button whose hotspot's
        // room_switch strcmp-equals it is highlighted (the hotspot is not null-tested).
        for (It = Buttons.begin(); It != Buttons.end(); ++It)
        {
            FNavBarButton& Button = *It;
            if (Button.bRoomSwitch != 0)
            {
                char const* const Switch = Button.Hotspot->RoomSwitch;
                bool const bHere = Switch != nullptr && strcmp(VirtualRoom.c_str(), Switch) == 0;
                Button.Control->SetProperty(0xc, bHere ? 1 : 0, 0);
            }
            else if (Button.Type == 6)
            {
                Button.bDisabled = 1;
                Button.Control->SetProperty(2, 1, 0);
            }
            else if (Button.Type == 0xc)
            {
                bool const bNeedsRepair = NnRepair::PlayerNeedsRepair();
                Button.Control->SetProperty(2, bNeedsRepair ? 0 : 1, 0);
                Button.bDisabled = bNeedsRepair ? 0 : 1;
            }
            else
            {
                Button.Control->SetProperty(2, 0, 0);
            }
        }
    }

    LayoutRoomButtons();
    LayoutTabs(true);
}

void NavBar::LayoutTabs(bool bAnimate)
{
    // (RET 4). 0x0043e219..0x0043e27b: the selected room button (+0x330 bit
    // 0x40). The first room button selected -> step -0.01 and stop; the selected one being the
    // last room button -> +0.01; otherwise 0 (reset at every room button). No selection ->
    // return.
    FNavBarButton* Selected = nullptr;
    FNavBarButton* Last = nullptr;
    float Step = 0.0f;
    bool bFirst = true;
    bool bFirstSelected = false;
    std::list<FNavBarButton>::iterator It;
    for (It = Buttons.begin(); It != Buttons.end(); ++It)
    {
        FNavBarButton& Button = *It;
        if (Button.bRoomSwitch == 0)
            continue;
        Step = 0.0f;
        if ((static_cast<NnButtonControl*>(Button.Control)->GetFlags330() & 0x40) != 0)
        {
            Selected = &Button;
            if (bFirst)
            {
                Step = NnFloatBits(0xbc23d70a);
                bFirstSelected = true;
                break;
            }
        }
        Last = &Button;
        bFirst = false;
    }
    if (!bFirstSelected && Last == Selected)
        Step = NnFloatBits(0x3c23d70a);
    if (Selected == nullptr)
        return;

    // its offset (property 7).
    float Base[3] = {0.0f, 0.0f, 0.0f};
    Selected->Control->SetProperty(7, reinterpret_cast<uintptr_t>(Base), 0);

    // the enabled service tabs; 1..3 of them, else return.
    int Count = 0;
    for (It = Buttons.begin(); It != Buttons.end(); ++It)
    {
        if (It->bRoomSwitch == 0 && It->bDisabled == 0)
            ++Count;
    }
    if (Count <= 0 || Count >= 4)
        return;
    int const TabIndex = Count - 1;
    if (TabIndex > 0)
        Base[0] = Base[0] + Step;

    // the tab mesh and its offset (x, 0.295, -1.0).
    ManeuverTab->SetProperty(4, reinterpret_cast<uintptr_t>(kTabMeshes[TabIndex]), 0);
    float const TabPosition[3] = {Base[0], NnFloatBits(0x3e970a3d), NnFloatBits(0xbf800000)};
    ManeuverTab->SetProperty(6, reinterpret_cast<uintptr_t>(TabPosition), 0);


    float const* Offset = kTabOffsets[TabIndex];
    ButtonMover::Start(kAnimSeconds);
    LayoutDelay = kLayoutDelay;

    // every enabled tab laid out 0.065 below the selected button at
    // its x offset (slid there when bAnimate, placed otherwise); the desk (type 6) and dealer
    // (5 / 7 / 8) tabs remembered, enabled or not.
    FNavBarButton* DeskTab = nullptr;
    FNavBarButton* DealerTab = nullptr;
    for (It = Buttons.begin(); It != Buttons.end(); ++It)
    {
        FNavBarButton& Button = *It;
        if (Button.bRoomSwitch != 0)
            continue;
        if (Button.bDisabled == 0)
        {
            float End[3];
            if (bAnimate)
            {
                Button.Control->SetProperty(6, reinterpret_cast<uintptr_t>(Base), 0);
                End[0] = *Offset + Base[0];
                End[1] = Base[1] - NnFloatBits(0x3d851eb8);
                End[2] = Base[2];
                ++Offset;
                ButtonMover::AddButton(Button.Control, Base, End);
            }
            else
            {
                End[0] = *Offset + Base[0];
                End[1] = Base[1] - NnFloatBits(0x3d851eb8);
                End[2] = Base[2];
                ++Offset;
                Button.Control->SetProperty(6, reinterpret_cast<uintptr_t>(End), 0);
            }
            Button.Position[0] = End[0];
            Button.Position[1] = End[1];
            Button.Position[2] = End[2];
        }
        if (Button.Type == 6)
            DeskTab = &Button;
        if (Button.Type == 5 || Button.Type == 7 || Button.Type == 8)
            DealerTab = &Button;
    }

    // while sliding, the desk tab starts where the dealer tab sits.
    if (bAnimate && DeskTab != nullptr && DealerTab != nullptr)
        DeskTab->Control->SetProperty(6, reinterpret_cast<uintptr_t>(DealerTab->Position), 0);
}

void NavBar::LayoutRoomButtons()
{
    // . 0x0043e4c7..0x0043e4f3: the room buttons, counted up to 5.
    int Count = 0;
    std::list<FNavBarButton>::iterator It;
    for (It = Buttons.begin(); It != Buttons.end(); ++It)
    {
        if (It->bRoomSwitch != 0)
            ++Count;
    }
    if (Count > 5)
        Count = 5;

    // each at (offset, 0.007, 0), walking every room button.
    float const* Offset = kRowOffsets[Count];
    for (It = Buttons.begin(); It != Buttons.end(); ++It)
    {
        if (It->bRoomSwitch == 0)
            continue;
        float const Position[3] = {*Offset, NnFloatBits(0x3be56042), 0.0f};
        It->Control->SetProperty(6, reinterpret_cast<uintptr_t>(Position), 0);
        ++Offset;
    }

    // the frame mesh the field[count - 1]. With no room button
    // baseline reads index -1, the dword 0x00610c40 -- row 5's offsets taken as a mesh name
    // (a baseline defect, reproduced).
    char const* const Mesh =
        Count > 0 ? kBoxMeshes[Count - 1] : reinterpret_cast<char const*>(&kRowOffsetData[9]);
    ManeuverFrame->SetProperty(4, reinterpret_cast<uintptr_t>(Mesh), 0);
}

void NavBar::RefreshRepairTab(bool bForce)
{
    // (RET 4): the first repair tab (type 0xc) disabled unless the ship is
    // damaged and bForce is clear; a changed state re-lays the tabs without sliding.
    std::list<FNavBarButton>::iterator It;
    for (It = Buttons.begin(); It != Buttons.end(); ++It)
    {
        FNavBarButton& Button = *It;
        if (Button.bRoomSwitch != 0 || Button.Type != 0xc)
            continue;
        bool const bNeedsRepair = NnRepair::PlayerNeedsRepair();
        unsigned char const bDisable = (bNeedsRepair && !bForce) ? 0 : 1;
        Button.Control->SetProperty(2, bDisable, 0);
        unsigned char const bOld = Button.bDisabled;
        Button.bDisabled = bDisable;
        if (bOld != bDisable)
            LayoutTabs(false);
        return;
    }
}

void NavBar::NotifyServiceWindows()
{
    // .
    NotifyServiceWindow("NN_Repair");
    NotifyServiceWindow("NN_ShipTrader");
    NotifyServiceWindow("NN_News");
    NotifyServiceWindow("NN_MissionVendor");
}

void NavBar::Draw()
{
    // WinControl::Draw (the routine) only while the cursor shows, no fade runs
    // and the HUD does not block picking.
#ifdef OUTPOST_FRONTIER_WALK_MODE
    bool const bWalkFade = BaseWalkShouldDrawInterface();
    if (GetClientMouseCursorDrawFlag() == 0 && !bWalkFade)
#else
    if (GetClientMouseCursorDrawFlag() == 0)
#endif
        return;
    if (bFadingOut != 0 || bFadingIn != 0)
        return;
#ifdef OUTPOST_FRONTIER_WALK_MODE
    if (HudIsMousePickBlocked() && !bWalkFade)
#else
    if (HudIsMousePickBlocked())
#endif
        return;
    WinControl::Draw();
}

void NavBar::Update()
{
    // . 0x0043e708..0x0043e729: the children (the routine inlined).
    Win3D::Update();

    // everything hidden unless the cursor shows, no fade runs and no
    // NeuroNet screen is up; the tab mesh also needs an enabled tab; disabled buttons re-greyed.
#ifdef OUTPOST_FRONTIER_WALK_MODE
    bool const bShow = (GetClientMouseCursorDrawFlag() != 0 || BaseWalkShouldDrawInterface()) &&
                       bFadingOut == 0 && bFadingIn == 0 && !G_bNnScreenUp;
#else
    bool const bShow = GetClientMouseCursorDrawFlag() != 0 && bFadingOut == 0 &&
                       bFadingIn == 0 && !G_bNnScreenUp;
#endif
    uintptr_t const Hide = bShow ? 0 : 1;
    ManeuverFrame->SetProperty(1, Hide, 0);
    int EnabledTabs = 0;
    std::list<FNavBarButton>::iterator It;
    for (It = Buttons.begin(); It != Buttons.end(); ++It)
    {
        if (It->bRoomSwitch == 0 && It->bDisabled == 0)
            ++EnabledTabs;
    }
    ManeuverTab->SetProperty(1, (bShow && EnabledTabs != 0) ? 0 : 1, 0);
    for (It = Buttons.begin(); It != Buttons.end(); ++It)
    {
        It->Control->SetProperty(1, Hide, 0);
        if (It->bDisabled != 0)
            It->Control->SetProperty(2, 1, 0);
    }

    // with the bar up, the return to the desk tab and the animations.
    if (GetClientMouseCursorDrawFlag() != 0 && bFadingOut == 0 && bFadingIn == 0 &&
        !G_bNnScreenUp)
    {
        if (bReturnToDesk != 0)
        {
            // an open repair tab, or no animation running.
            bool bRepairOpen = false;
            for (It = Buttons.begin(); It != Buttons.end(); ++It)
            {
                if (It->bRoomSwitch == 0 && It->bOpen != 0 && It->Type == 0xc)
                    bRepairOpen = true;
            }
            if (bRepairOpen || !IsAnimating())
            {
                // no dealer window -> the desk tab (type 6) activated.
                if (Win3DFindByName("NN_Dealer") == nullptr &&
                    Win3DFindByName("NN_ShipTrader") == nullptr)
                {
                    bReturnToDesk = 0;
                    for (It = Buttons.begin(); It != Buttons.end(); ++It)
                    {
                        if (It->bRoomSwitch == 0 && It->Type == 6)
                        {
                            ActivateButton(Current, &*It);
                            break;
                        }
                    }
                }
            }
        }
        // the layout delay counts down, then the animations run.
        if (LayoutDelay > 0.0f)
            LayoutDelay = static_cast<float>(static_cast<double>(LayoutDelay) -
                                             GetClientFrameTimeCapped());
        else
        {
            ButtonMover::Update();
            ButtonScaler::Update();
        }
    }

    // a running fade; once FadeTimer (kept at extended precision for
    // the compare) reaches the fade duration, the fade-out moves the camera to the desk and
    // the fade-in shows the cursor and enters Current's room.
    if (bFadingOut != 0 || bFadingIn != 0)
    {
        double const Elapsed = GetClientFrameTimeCapped() + static_cast<double>(FadeTimer);
        FadeTimer = static_cast<float>(Elapsed);
        if (Elapsed >= static_cast<double>(GetFadeDuration()))
        {
            if (bFadingOut != 0)
            {
                bFadingOut = 0;
                CameraToDesk();
            }
            else if (bFadingIn != 0)
            {
                bFadingIn = 0;
                SetClientMouseCursorDrawFlag(1);
                SelectRoomButton(Current);
            }
        }
    }

    // a virtual room entered by a room button, once the location
    // changed -- its desk camera and the player at its desk (three independent tests).
    if (bVirtualRoomPending != 0 && VirtualRoomLocation != GetClientLocationCurrentRoom())
    {
        bVirtualRoomPending = 0;
        if (VirtualRoom.size() != 0)
        {
            FRoomRuntime* const Room = CurrentRoomRuntime();
            if (_stricmp(VirtualRoom.c_str(), "Trader") == 0) // NEURONCOMPONENTS stricmp
            {
                RoomControlSnapCamera(Room, "Camera_Trader_Desk");
                PlacePlayerAtSetpoint("Player_Trader");
            }
            if (_stricmp(VirtualRoom.c_str(), "Equipment") == 0)
            {
                RoomControlSnapCamera(Room, "Camera_Equip_Desk");
                PlacePlayerAtSetpoint("Player_Equip");
            }
            if (_stricmp(VirtualRoom.c_str(), "ShipDealer") == 0)
            {
                RoomControlSnapCamera(Room, "Camera_Ship_Desk");
                PlacePlayerAtSetpoint("Player_Ship");
            }
        }
    }

    // a repair redirect waiting for the equipment room opens the
    // repair tab once the bar is idle there.
    if (bRepairRedirect != 0 && bFadingOut == 0 && bFadingIn == 0 && LayoutDelay <= 0.0f &&
        !IsAnimating() && GetClientMouseCursorDrawFlag() != 0 && bFadingOut == 0 &&
        bFadingIn == 0 && !G_bNnScreenUp)
    {
        bool bInEquipment = false;
        if (VirtualRoom.size() != 0)
            bInEquipment = _stricmp(VirtualRoom.c_str(), "Equipment") == 0;
        else
        {
            unsigned const Location = GetClientLocationCurrentRoom();
            bInEquipment = Location == MakeLocationID(CurrentBaseId(), "Equipment");
        }
        if (bInEquipment)
        {
            bRepairRedirect = 0;
            for (It = Buttons.begin(); It != Buttons.end(); ++It)
            {
                if (It->bRoomSwitch == 0 && It->Type == 0xc)
                {
                    OpenService(&*It);
                    break;
                }
            }
        }
    }

    // a clicked character's walk finished -- the slide completed and
    // the character clicked (slot 12 with a zeroed point, the routine). Without a character
    // the pending flag stays set.
    if (bCharacterPending != 0 && bFadingOut == 0 && bFadingIn == 0 &&
        GetClientMouseCursorDrawFlag() != 0 && bFadingOut == 0 && bFadingIn == 0 &&
        !G_bNnScreenUp)
    {
        ButtonMover::Finish();
        if (PendingCharacter != nullptr)
        {
            int const Point[2] = {0, 0};
            PendingCharacter->Slot12(0, Point);
            PendingCharacter = nullptr;
            bCharacterPending = 0;
        }
    }

    // a queued close runs once idle (the routine's body inlined:
    // the background at full size, shrunk to the type's scale); a close that finds the bar
    // animating again after the notify is dropped.
    if (!IsAnimating() && QueuedClose != nullptr)
    {
        NotifyServiceWindows();
        FNavBarButton* const Button = QueuedClose;
        if (IsAnimating())
        {
            QueuedClose = nullptr;
            return;
        }
        ClosingButton = Button;
        PlayOrRewindUiSoundSlot(0x1b);
        MakeServiceBackground(Button->Type, 1.0f);
        float const Scale = ServiceScale(Button->Type);
        ButtonScaler::Start(kAnimSeconds);
        ButtonScaler::AddButton(ScaledBackground, 1.0f, Scale);
        QueuedClose = nullptr;
        return;
    }
    // else a queued open.
    if (!IsAnimating() && QueuedOpen != nullptr)
    {
        OpenService(QueuedOpen);
        QueuedOpen = nullptr;
    }
}

intptr_t NavBar::SetProperty(int Msg, uintptr_t A, uintptr_t /*B*/)
{
    // (RET 0xc). baseline answers every other Msg with 0 and does not forward to
    // WinControl::SetProperty.
    if (Msg != 0x26)
        return 0;
    if (G_bNnScreenUp)
        return 0;
    // swallowed while animating or while the room camera moves.
    if (IsAnimating())
        return 1;
    FRoomRuntime* const Room = CurrentRoomRuntime();
    if (Room != nullptr && RoomControlGetField1338(Room) != 0)
        return 1;

    // the button whose control was clicked; none -> 0.
    FNavBarButton* Button = nullptr;
    std::list<FNavBarButton>::iterator It;
    for (It = Buttons.begin(); It != Buttons.end(); ++It)
    {
        if (reinterpret_cast<uintptr_t>(It->Control) == A)
        {
            Button = &*It;
            break;
        }
    }
    if (Button == nullptr)
        return 0;

    // a dealer tab clicks its dealer character when the room has one.
    if (Button->bRoomSwitch == 0)
    {
        int Role = 0;
        if (Button->Type == 5)
            Role = 2;
        else if (Button->Type == 7)
            Role = 3;
        else if (Button->Type == 8)
            Role = 4;
        if (Role != 0)
        {
            FCharacterObject* const Character =
                RoomControlFindCharacterByRole(CurrentRoomRuntime(), Role);
            if (Character != nullptr)
            {
                int const Point[2] = {0, 0};
                Character->Slot12(0, Point);
                return 1;
            }
        }
    }

    // a stateful hotspot runs its transition, anything else its action.
    if (Button->Hotspot != nullptr && Button->Hotspot->StateRead != 0)
    {
        if (Current == nullptr)
            Current = Button;
        ActivateButton(Current, Button);
        return 1;
    }
    OnButtonClicked(Button);
    return 1;
}

void NavBar::Slot29(FILE* File)
{
    // (RET 4).
    fprintf(File, "[NavBar]\n");
    std::map<std::string, char*>::iterator It;
    for (It = IniValues.begin(); It != IniValues.end(); ++It)
        fprintf(File, "%s = %s\n", It->first.c_str(), It->second);
    fprintf(File, "\n");
}

bool NavBar::SetIniValue(char const* Name, char const* Value)
{
    // (RET 8): a missing key gets a new 0x80-byte buffer (the routine insert),
    // then the key is looked up again (the routine) and Value copied in unbounded.
    std::string const Key(Name);
    if (IniValues.find(Key) == IniValues.end())
        IniValues.insert(std::map<std::string, char*>::value_type(Key, new char[0x80]));
    strcpy(IniValues.find(Key)->second, Value);
    return true;
}

void NavBar::Rebuild()
{
    // . 0x0043f300..0x0043f438: the list emptied and the nine RoomControl /
    // ButtonControl child names queued.
    Buttons.clear();
    char ControlName[0x100];
    int Index = 0;
    for (Index = 1; Index < 10; ++Index)
    {
        sprintf(ControlName, "RoomControl%d", Index);
        RoomControlNames.push_back(std::string(ControlName));
        sprintf(ControlName, "ButtonControl%d", Index);
        ButtonControlNames.push_back(std::string(ControlName));
    }

    // one button per hotspot of the current room (runtime +0x40).
    RoomData* const Data = GetRoomRuntimeRoomData(CurrentRoomRuntime());
    size_t HotspotIndex = 0;
    for (HotspotIndex = 0; HotspotIndex < Data->Hotspots.size(); ++HotspotIndex)
    {
        RoomButtonInfo const* const Hotspot = Data->Hotspots[HotspotIndex];

        // a hotspot of another virtual room is skipped, except the
        // Deck / Planetscape ones outside any virtual room.
        if (Hotspot->VirtualRoom != nullptr && VirtualRoom.compare(Hotspot->VirtualRoom) != 0)
        {
            if (_stricmp(Hotspot->VirtualRoom, "Deck") != 0 &&
                _stricmp(Hotspot->VirtualRoom, "Planetscape") != 0)
                continue;
            if (VirtualRoom.size() != 0)
                continue;
        }

        // a news hotspot (behaviour 2) only with news at this base.
        if (Hotspot->Behavior == 2)
        {
            TMirrorPropertyBucket<NewsBroadcastProperties>* const News =
                GetClientLocationBaseNews(GetRoomRuntimeBaseId(CurrentRoomRuntime()));
            int NewsCount = 0;
            MetaNode<NewsBroadcastProperties>* Node = nullptr;
            while (News->List.traverse(Node, true))
                ++NewsCount;
            if (NewsCount == 0)
                continue;
        }

        // a room button for a set_virtual_room or room_switch hotspot
        // (the latter selected when it names this room), else a tab of the hotspot's behaviour.
        FNavBarButton* Button = nullptr;
        if (Hotspot->SetVirtualRoom != nullptr && Hotspot->RoomSwitch != nullptr)
        {
            unsigned const Location = MakeLocationID(CurrentBaseId(), Hotspot->RoomSwitch);
            Button = AddButton(Location, 0, 1, Hotspot->SetVirtualRoom, Hotspot->Name);
        }
        else if (Hotspot->RoomSwitch != nullptr)
        {
            int const bThisRoom = strcmp(Hotspot->RoomSwitch, Data->Nickname) == 0 ? 1 : 0;
            unsigned const Location = MakeLocationID(CurrentBaseId(), Hotspot->RoomSwitch);
            Button = AddButton(Location, bThisRoom, 1, Hotspot->RoomSwitch, Hotspot->Name);
        }
        else
        {
            Button = AddButton(static_cast<unsigned>(Hotspot->Behavior), 0, 0, Hotspot->Name,
                               Hotspot->Name);
        }
        if (Button == nullptr)
            continue;


        Button->Hotspot = Hotspot;
        if (Hotspot->RoomSwitch != nullptr && VirtualRoom.compare(Hotspot->RoomSwitch) == 0)
            Current = Button;
    }

    // the launch tab in the Cityscape / Deck / Planetscape rooms outside
    // any other virtual room, with a ship to launch.
    char const* const Nickname = Data->Nickname;
    if (strcmp(Nickname, "Cityscape") == 0 || strcmp(Nickname, "Deck") == 0 ||
        strcmp(Nickname, "Planetscape") == 0)
    {
        if (VirtualRoom.size() == 0 || strcmp(VirtualRoom.c_str(), "Deck") == 0 ||
            strcmp(VirtualRoom.c_str(), "Planetscape") == 0)
        {
            if (GetPlayerShipArchetypeIdbaseline() != 0)
                AddButton(4, 0, 0, "IDS_HOTSPOT_LAUNCH", "IDS_HOTSPOT_LAUNCH");
        }
    }

    // every unused child reset to the default mesh, no tooltip,
    // hidden and disabled; both name queues emptied (ButtonControl first).
    std::list<std::string>::iterator Name;
    for (Name = ButtonControlNames.begin(); Name != ButtonControlNames.end(); ++Name)
    {
        WinControl* const Control = FindChild(Name->c_str());
        if (Control == nullptr)
            continue;
        Control->SetProperty(4, reinterpret_cast<uintptr_t>(DefaultMesh), 0);
        Control->SetProperty(3, 0, 0);
        Control->SetProperty(1, 1, 0);
        Control->SetProperty(2, 1, 0);
    }
    ButtonControlNames.clear();
    for (Name = RoomControlNames.begin(); Name != RoomControlNames.end(); ++Name)
    {
        WinControl* const Control = FindChild(Name->c_str());
        if (Control == nullptr)
            continue;
        Control->SetProperty(4, reinterpret_cast<uintptr_t>(DefaultMesh), 0);
        Control->SetProperty(3, 0, 0);
        Control->SetProperty(1, 1, 0);
        Control->SetProperty(2, 1, 0);
    }
    RoomControlNames.clear();
}

FNavBarButton* NavBar::AddButton(unsigned Type, int /*Unused*/, unsigned char bRoomSwitch,
                                 char const* Name, char const* IdsName)
{
    // (RET 0x14). 0x0043f915..0x0043f989: these names are always room buttons.
    if (strcmp(Name, "Cityscape") == 0 || strcmp(Name, "Deck") == 0 ||
        strcmp(Name, "Planetscape") == 0 || strcmp(Name, "IDS_HOTSPOT_DECK") == 0 ||
        strcmp(Name, "IDS_HOTSPOT_PLANETSCAPE") == 0)
        bRoomSwitch = 1;

    // room buttons take the next "ButtonControlN", tabs the next
    // "RoomControlN" (the queue's front, popped). baseline assumes the queue is not empty.
    std::string ControlName;
    if (bRoomSwitch != 0)
    {
        ControlName = ButtonControlNames.front();
        ButtonControlNames.pop_front();
    }
    else
    {
        ControlName = RoomControlNames.front();
        RoomControlNames.pop_front();
    }

    // a type of -1 becomes the location Name of this base.
    if (Type == 0xffffffffu)
        Type = MakeLocationID(CurrentBaseId(), Name);

    // the child; none -> null.
    WinControl* const Control = FindChild(ControlName.c_str());
    if (Control == nullptr)
        return nullptr;

    // pushed to the front (the routine, the routine at begin()).
    Buttons.push_front(FNavBarButton());
    FNavBarButton* const Button = &Buttons.front();
    Button->IdsName = IdsName;
    Button->Name = Name;
    Button->bRoomSwitch = bRoomSwitch;
    Button->Type = Type;
    Button->Control = Control;

    // its tooltip ids (the field), FDUMP when unknown.
    int Ids = 0;
    if (!HudLookupNamedValue(kHotspotIds, Button->IdsName, &Ids, 0x13))
    {
        FDUMP(0x100002, "%s(%d) : *** WARNING: unknown HotspotName = '%s'\n", kNavBarCpp,
              0x530, Button->IdsName);
    }

    // shown, the tooltip (a ship dealer's tab of type 8 says
    // "equipment dealer"), the ids kept in +0x2ec, tabs flagged 0x36.
    Button->Control->SetProperty(1, 0, 0);
    Button->Control->SetProperty(3, static_cast<uintptr_t>(Ids), 0);
    if (Ids == 0x196b && Type == 8)
        Button->Control->SetProperty(3, 0x1968, 0);
    Button->Control->Field2ec = Ids;
    if (bRoomSwitch == 0)
        Button->Control->SetProperty(0x36, 1, 0);

    // its mesh from IniValues; without an entry the default mesh, and
    // the path the control reports (property 0xb) recorded under the hotspot name.
    std::map<std::string, char*>::iterator Found = IniValues.find(Button->Name);
    if (Found != IniValues.end())
    {
        Button->Control->SetProperty(4, reinterpret_cast<uintptr_t>(Found->second), 0);
    }
    else
    {
        Button->Control->SetProperty(4, reinterpret_cast<uintptr_t>(DefaultMesh), 0);
        char MeshPath[0x100];
        Button->Control->SetProperty(0xb, reinterpret_cast<uintptr_t>(MeshPath), 0x100);
        SetIniValue(Button->Name.c_str(), MeshPath);
    }
    return Button;
}

void NavBar::SelectRoomButton(FNavBarButton* Button)
{
    // (RET 4). 0x0043fd37..0x0043fd8b: the room already entered -> nothing (the
    // hotspot is not null-tested here).
    char const* const Switch = Button->Hotspot->RoomSwitch;
    if (Switch != nullptr && strcmp(VirtualRoom.c_str(), Switch) == 0)
        return;

    // the first call latches bit 0x10000 of the control's +0x2ec and
    // fades out (the routine(1.0, 1 / duration)); the room camera released, the cursor hidden
    // and the service windows told.
    WinControl* const Control = Button->Control;
    if ((Control->Field2ec & 0x10000) == 0)
    {
        Control->Field2ec |= 0x10000;
        Current = Button;
        SetFadeTarget(1.0f, static_cast<float>(1.0 / static_cast<double>(GetFadeDuration())));
        bFadingOut = 1;
        FadeTimer = 0.0f;
        RoomControlClearField1338(CurrentRoomRuntime());
        SetClientMouseCursorDrawFlag(0);
        NotifyServiceWindows();
        return;
    }

    // the second (after the fade) clears the latch word, enters the
    // virtual room (or none) and rebuilds.
    Control->Field2ec &= 0xffff;
    RoomButtonInfo const* const Hotspot = Button->Hotspot;
    if (Hotspot != nullptr && Hotspot->RoomSwitch != nullptr)
        VirtualRoom.assign(Hotspot->RoomSwitch);
    else
        VirtualRoom.erase();
    Current = nullptr;
    Rebuild();
    Refresh();
}

void NavBar::CameraToDesk()
{
    // . Current is not null-tested.
    FNavBarButton* const Button = Current;
    FRoomRuntime* const Room = CurrentRoomRuntime();
    RoomButtonInfo const* const Hotspot = Button->Hotspot;
    bool bDefaultCamera = true;
    if (Hotspot != nullptr && Hotspot->RoomSwitch != nullptr)
    {
        char const* const Switch = Hotspot->RoomSwitch;
        if (_stricmp(Switch, "Trader") == 0)
        {
            RoomControlSnapCamera(Room, "Camera_Trader_Desk");
            PlacePlayerAtSetpoint("Player_Trader");
            bDefaultCamera = false;
        }
        else if (_stricmp(Switch, "Equipment") == 0)
        {
            RoomControlSnapCamera(Room, "Camera_Equip_Desk");
            PlacePlayerAtSetpoint("Player_Equip");
            bDefaultCamera = false;
        }
        else if (_stricmp(Switch, "ShipDealer") == 0)
        {
            RoomControlSnapCamera(Room, "Camera_Ship_Desk");
            PlacePlayerAtSetpoint("Player_Ship");
            bDefaultCamera = false;
        }
        else if (_stricmp(Switch, "Planetscape") != 0 && _stricmp(Switch, "Deck") != 0)
        {
            // another room keeps its camera.
            bDefaultCamera = false;
        }
    }
    if (bDefaultCamera)
        RoomControlSnapCamera(Room, "Camera_0");

    // fade back in.
    SetFadeTarget(0.0f, static_cast<float>(1.0 / static_cast<double>(GetFadeDuration())));
    bFadingIn = 1;
    FadeTimer = 0.0f;
}

void NavBar::OpenService(FNavBarButton* Button)
{
    // (RET 4). While animating it is queued.
    if (IsAnimating())
    {
        QueuedOpen = Button;
        return;
    }
    NnMainButtonOnModalOpen();
    PlayOrRewindUiSoundSlot(0x1b);

    // a service tab slides from where it is to its type's target
    // (the repair tab also asks for the desk afterwards).
    float const* Target = nullptr;
    switch (Button->Type)
    {
    case 0xc:
        Target = G_NavBarRepairTarget;
        break;
    case 7:
        Target = G_NavBarShipTarget;
        break;
    case 3:
        Target = G_NavBarMissionTarget;
        break;
    case 2:
        Target = G_NavBarNewsTarget;
        break;
    default:
        return;
    }
    Button->bOpen = 1;
    Button->Control->SetProperty(7, reinterpret_cast<uintptr_t>(Button->Position), 0);
    if (Button->Type == 0xc)
        bReturnToDesk = 1;
    ButtonMover::Start(kAnimSeconds);
    ButtonMover::AddButton(Button->Control, Button->Position, Target);
}

void NavBar::CloseService(FNavBarButton* Button)
{
    // (RET 4). While animating it is queued.
    if (IsAnimating())
    {
        QueuedClose = Button;
        return;
    }
    // the background at full size shrinks to the type's scale.
    ClosingButton = Button;
    PlayOrRewindUiSoundSlot(0x1b);
    MakeServiceBackground(Button->Type, 1.0f);
    float const Scale = ServiceScale(Button->Type);
    ButtonScaler::Start(kAnimSeconds);
    ButtonScaler::AddButton(ScaledBackground, 1.0f, Scale);
}

float NavBar::ServiceScale(unsigned Type)
{
    // (RET 4): 2 / 3 / 0xc -> 0.08 (the field), 7 -> 0.05 (the field),
    // anything else 1.0 (the field).
    switch (Type)
    {
    case 2:
    case 3:
    case 0xc:
        return NnFloatBits(0x3da3d70a);
    case 7:
        return NnFloatBits(0x3d4ccccd);
    default:
        return 1.0f;
    }
}

void NavBar::MakeServiceBackground(unsigned Type, float Scale)
{
    // (RET 8): the type's background mesh and position; other types none.
    switch (Type)
    {
    case 0xc:
    {
        float const Position[3] = {0.0f, 0.0f, NnFloatBits(0xbf8ccccd)};
        MakeScaledBackground(Scale, "Interface\\NeuroNet\\Inventory\\repair_bakgrnd.cmp",
                             Position);
        return;
    }
    case 7:
    {
        float const Position[3] = {0.0f, NnFloatBits(0xbdb020c5), NnFloatBits(0xbfa4dd2f)};
        MakeScaledBackground(Scale, "Interface\\NeuroNet\\Inventory\\INV_biginforcard_center.cmp",
                             Position);
        return;
    }
    case 2:
    case 3:
    {
        float const Position[3] = {0.0f, NnFloatBits(0xbdcccccd), NnFloatBits(0xbfbfbe77)};
        MakeScaledBackground(Scale, "Interface\\NeuroNet\\NewsVendor\\NEWS_background.cmp",
                             Position);
        return;
    }
    default:
        return;
    }
}

void NavBar::OnScaleFinished()
{
    // (this = the +0x34c face).
    if (ClosingButton != nullptr)
    {
        // the shrunk background deleted.
        if (ScaledBackground != nullptr)
        {
            Win3DDeleteTree(ScaledBackground);
            ScaledBackground = nullptr;
        }
        // the closing tab slides home from its target and is enabled
        // again; other types do nothing.
        switch (ClosingButton->Type)
        {
        case 0xc:
            ButtonMover::Start(kAnimSeconds);
            ButtonMover::AddButton(ClosingButton->Control, G_NavBarRepairTarget,
                                   ClosingButton->Position);
            ClosingButton->bDisabled = 0;
            ClosingButton->Control->SetProperty(2, 0, 0);
            ClosingButton->bOpen = 0;
            return;
        case 7:
        case 2:
        case 3:
        {
            float const* const Target = ClosingButton->Type == 7
                                            ? G_NavBarShipTarget
                                            : (ClosingButton->Type == 2 ? G_NavBarNewsTarget
                                                                        : G_NavBarMissionTarget);
            ButtonMover::Start(kAnimSeconds);
            ClosingButton->bOpen = 0;
            ButtonMover::AddButton(ClosingButton->Control, Target, ClosingButton->Position);
            ClosingButton->bDisabled = 0;
            ClosingButton->Control->SetProperty(2, 0, 0);
            return;
        }
        default:
            return;
        }
    }

    // the grown background deleted.
    if (ScaledBackground != nullptr)
    {
        Win3DDeleteTree(ScaledBackground);
        ScaledBackground = nullptr;
    }

    // the open tab's window comes up.
    std::list<FNavBarButton>::iterator It;
    for (It = Buttons.begin(); It != Buttons.end(); ++It)
    {
        FNavBarButton& Button = *It;
        if (Button.bRoomSwitch != 0 || Button.bOpen == 0)
            continue;
        switch (Button.Type)
        {
        case 2:
        case 3:
        {
            // the room's controllers for the news / mission vendor.
            NnMainButtonOnModalOpen();
            FRoomRuntime* const Room = CurrentRoomRuntime();
            if (Room != nullptr)
                RoomControlNotifyControllers(Room, -1, 3, static_cast<int>(Button.Type));
            return;
        }
        case 0xc:
        {
            // NN_Repair, placed (property 0x42) at (0, 0, -1.1).
            Win3D* const Repair = WinFactoryCreate("NN_Repair", "NN_Repair", nullptr, nullptr);
            if (Repair == nullptr)
                return;
            float const Position[3] = {0.0f, 0.0f, NnFloatBits(0xbf8ccccd)};
            static_cast<WinControl*>(Repair)->SetProperty(
                0x42, reinterpret_cast<uintptr_t>(Position), 0);
            return;
        }
        case 7:
        {
            // the base's solar marked visited for the ship dealer
            // (flag 0x10, the server told), NN_ShipTrader, the desk tab back.
            unsigned const Solar = GetClientLocationBaseSolarId();
            if (Solar != 0)
            {
                unsigned char Flags = 0;
                GetPlayerVisitFlagsbaseline(Solar, &Flags, 0, 0);
                Flags |= 0x10;
                SetPlayerVisitFlagsbaseline(Solar, Flags, true);
            }
            WinFactoryCreate("NN_ShipTrader", "NN_ShipTrader", nullptr, nullptr);
            ToggleDealerTabs(true);
            return;
        }
        default:
            return;
        }
    }
}

void NavBar::OnMoveFinished()
{
    // (this = the +0x32c face). 0x004408b7..0x004408f0: a closed tab is home --
    // the dealer tab swapped back for the ship dealer, the repair tab refreshed.
    if (ClosingButton != nullptr)
    {
        if (ClosingButton->Type == 7)
            ToggleDealerTabs(false);
        ClosingButton = nullptr;
        RefreshRepairTab(false);
        return;
    }


    if (ScaledBackground != nullptr)
    {
        Win3DDeleteTree(ScaledBackground);
        ScaledBackground = nullptr;
    }

    // an opened tab arrived -- its background grows from the type's
    // scale to full size and the tab is disabled.
    std::list<FNavBarButton>::iterator It;
    for (It = Buttons.begin(); It != Buttons.end(); ++It)
    {
        FNavBarButton& Button = *It;
        if (Button.bRoomSwitch != 0 || Button.bOpen == 0)
            continue;
        float const Scale = ServiceScale(Button.Type);
        MakeServiceBackground(Button.Type, Scale);
        ButtonScaler::Start(kAnimSeconds);
        ButtonScaler::AddButton(ScaledBackground, Scale, 1.0f);
        Button.bDisabled = 1;
        Button.Control->SetProperty(2, 1, 0);
        return;
    }
}

void NavBar::OnButtonClicked(FNavBarButton* Button)
{
    // (RET 4).
    bRepairRedirect = 0;
    FRoomRuntime* const Room = CurrentRoomRuntime();
    RoomButtonInfo const* const Hotspot = Button->Hotspot;

    // behaviour 0xb hotspots switch rooms.
    if (Hotspot != nullptr && Hotspot->Behavior == 0xb)
    {
        SelectRoomButton(Button);
        return;
    }

    // the current location's own button leaves a virtual room.
    unsigned const Type = Button->Type;
    if (Type != 0 && Type == GetClientLocationCurrentRoom())
    {
        if (VirtualRoom.size() == 0)
            return;
        VirtualRoom.erase();
        SelectRoomButton(Button);
        return;
    }

    // another room button -- its virtual room (entered once the
    // location changes), the room's controllers told, the service windows told.
    if (Button->bRoomSwitch != 0 && Type != 0 && Hotspot != nullptr)
    {
        VirtualRoom.erase();
        if (Hotspot->SetVirtualRoom != nullptr)
        {
            VirtualRoom.assign(Hotspot->SetVirtualRoom);
            bVirtualRoomPending = 1;
            VirtualRoomLocation = GetClientLocationCurrentRoom();
        }
        RoomControlNotifyControllers(Room, -1, 3, static_cast<int>(Button->Type));
        NotifyServiceWindows();
        return;
    }

    // jump table 0x004415e4 on Type - 2; outside 2..0xc -> the 0xb case.
    switch (Type)
    {
    case 2:
    case 3:
    {
        // (news) / 0x00441247 (missions): open -- the other open service closed
        // first; open already -- its window told and closed.
        if (Button->bOpen == 0)
        {
            NotifyServiceWindows();
            std::list<FNavBarButton>::iterator It;
            for (It = Buttons.begin(); It != Buttons.end(); ++It)
            {
                if (It->bRoomSwitch == 0 && It->bOpen != 0)
                {
                    if (IsServiceType(It->Type))
                        CloseService(&*It);
                    break;
                }
            }
            OpenService(Button);
            return;
        }
        NotifyServiceWindow(Type == 2 ? "NN_News" : "NN_MissionVendor");
        CloseService(Button);
        return;
    }
    case 4:
    {
        // launch -- the equipment checked, the ship's launch script
        // (or straight to the mission briefing), then the bar and its controls destroyed.
        if (!CheckLaunchEquipment())
            return;
        if (GetPlayerShipArchetypeIdbaseline() == 0)
            return;
        FObjectShip* const Ship = GfObjectShip();
        if (Ship != nullptr)
            Ship->PlayLaunchScript();
        else
            RequestGameflowMissionBriefing();
        Win3D* const Found = Win3DFindByName("NavBar");
        if (Found == nullptr)
            return;
        NavBar* const Bar = static_cast<NavBar*>(Found);
        if (Bar->ScaledBackground != nullptr)
        {
            Win3DDeleteTree(Bar->ScaledBackground);
            Bar->ScaledBackground = nullptr;
        }
        NavBarDeleteRoomControls();
        Bar->Destroy();
        delete Bar; // usually this -- nothing after it touches a member
        return;
    }
    case 5:
    case 8:
    {
        // (commodity dealer, visit flag 4) / 0x004411f4 (equipment dealer, 8):
        // the base's solar marked, the dealer screen opened.
        unsigned const Solar = GetClientLocationBaseSolarId();
        if (Solar != 0)
        {
            unsigned char Flags = 0;
            GetPlayerVisitFlagsbaseline(Solar, &Flags, 0, 0); // / 0x00441213
            Flags |= Type == 5 ? 4 : 8;
            SetPlayerVisitFlagsbaseline(Solar, Flags, true);
        }
        // / 0x0044122d..0x00441235: the routine(1 or 3, 0, 0, 0).
        NnInventoryOpenDealer(Type == 5 ? 1 : 3, nullptr, nullptr, 0);
        return;
    }
    case 6:
    case 9:
    case 0xa:
        return;
    case 7:
    {
        // the ship dealer tab.
        if (Button->bOpen == 0)
        {
            OpenService(Button);
            return;
        }
        NotifyServiceWindow("NN_ShipTrader");
        CloseService(Button);
        return;
    }
    case 0xc:
    {
        // the repair tab. Open -- NN_Repair told and closed.
        if (Button->bOpen != 0)
        {
            NotifyServiceWindow("NN_Repair");
            CloseService(Button);
            return;
        }
        // repairs happen in the equipment room.
        NnMainButtonOnModalOpen();
        unsigned const EquipmentLocation = MakeLocationID(CurrentBaseId(), "Equipment");
        std::list<FNavBarButton>::iterator It;
        if (VirtualRoom.size() == 0 && EquipmentLocation != 0)
        {
            // there already -> open; else its room button clicked
            // and the redirect armed.
            if (GetClientLocationCurrentRoom() == EquipmentLocation)
            {
                OpenService(Button);
                return;
            }
            for (It = Buttons.begin(); It != Buttons.end(); ++It)
            {
                if (It->bRoomSwitch != 0 && It->Type == EquipmentLocation)
                {
                    OnButtonClicked(&*It);
                    bRepairRedirect = 1;
                    break;
                }
            }
        }
        else
        {
            // in the "Equipment" virtual room -> open; else the room
            // button switching to it selected and the redirect armed.
            if (_stricmp(VirtualRoom.c_str(), "Equipment") == 0)
            {
                OpenService(Button);
                return;
            }
            for (It = Buttons.begin(); It != Buttons.end(); ++It)
            {
                if (It->bRoomSwitch == 0)
                    continue;
                char const* const Switch = It->Hotspot->RoomSwitch;
                if (Switch != nullptr && strcmp(Switch, "Equipment") == 0)
                {
                    SelectRoomButton(&*It);
                    bRepairRedirect = 1;
                    break;
                }
            }
        }
        if (bRepairRedirect != 0)
            return;
        // else the room button that sets the "Equipment" virtual room,
        // after the layout delay.
        for (It = Buttons.begin(); It != Buttons.end(); ++It)
        {
            if (It->bRoomSwitch == 0)
                continue;
            char const* const SetRoom = It->Hotspot->SetVirtualRoom;
            if (SetRoom != nullptr && strcmp(SetRoom, "Equipment") == 0)
            {
                LayoutDelay = kLayoutDelay;
                OnButtonClicked(&*It);
                bRepairRedirect = 1;
                return;
            }
        }
        return;
    }
    default:
    {
        // (0xb and every type outside 2..0xc): a shown NN_Inventory is
        // closed (command 0x6b), then the room's controllers told.
        if (IsShownWindow(Win3DFindByName("NN_Inventory")))
            HudPostCommand(0x6b, 0);
        RoomControlNotifyControllers(Room, -1, 3, static_cast<int>(Button->Type));
        return;
    }
    }
}

void NavBar::ActivateButton(FNavBarButton* From, FNavBarButton* To)
{
    // (RET 8).
    NnToolTipHideGlobal();
    Current = To;
    if (To->Hotspot == nullptr)
    {
        OnButtonClicked(Current);
        return;
    }

    // From's state_transition naming To's state ("%d,") runs the
    // script named after that prefix (the prefix is assumed to lead the entry).
    char StateKey[8];
    sprintf(StateKey, "%d,", static_cast<int>(To->Hotspot->StateRead));
    int Index = 0;
    for (Index = 0; Index < 4; ++Index)
    {
        char const* const Transition = From->Hotspot->StateTransition[Index];
        if (Transition != nullptr && strstr(Transition, StateKey) != nullptr)
        {
            StartScript(Transition + strlen(StateKey), 0, 0);
            return;
        }
    }

    // else To's behaviour moves the camera (jump table 0x00441a1c on
    // behaviour - 5); a track that does not start falls back to the button's action.
    FRoomRuntime* const Room = CurrentRoomRuntime();
    int const Behavior = To->Hotspot->Behavior;
    switch (Behavior)
    {
    case 8:
    {
        // the bar camera.
        NnMainButtonOnModalOpen();
        NotifyServiceWindows();
        RoomControlSetField1340(Room, 1);
        if (RoomControlTrackCamera(Room, "Camera_Track_1", &NavBar::ScriptDone, this, nullptr))
        {
            SetClientMouseCursorDrawFlag(0);
            ToggleDealerTabs(true);
            bReturnToDesk = 1;
            return;
        }
        ToggleDealerTabs(true);
        bReturnToDesk = 1;
        OnButtonClicked(Current);
        return;
    }
    case 5:
    case 7:
    {
        // the wide camera (the trader's inside a virtual room, for behaviour 5).
        if (Behavior != 7)
            NnMainButtonOnModalOpen();
        char const* Camera = "Camera_Wide";
        if (VirtualRoom.size() != 0 && To->Hotspot->Behavior == 5)
            Camera = "Camera_Trader_Wide";
        RoomControlSetField1340(Room, 1);
        if (!RoomControlTrackCamera(Room, Camera, &NavBar::ScriptDone, this, nullptr))
        {
            OnButtonClicked(Current);
            return;
        }
        SetClientMouseCursorDrawFlag(0);
        bReturnToDesk = 1;
        if (To->Hotspot->Behavior != 7)
            ToggleDealerTabs(true);
        return;
    }
    case 6:
    {
        // the desk camera of the virtual room; a shown dealer with a shown
        // inventory closes it (command 0x6b); NN_ShipTrader told; the room camera released.
        char const* Camera = "Camera_0";
        if (VirtualRoom.size() != 0)
        {
            if (_stricmp(VirtualRoom.c_str(), "Trader") == 0)
                Camera = "Camera_Trader_Desk";
            else if (_stricmp(VirtualRoom.c_str(), "Equipment") == 0)
                Camera = "Camera_Equip_Desk";
            else if (_stricmp(VirtualRoom.c_str(), "ShipDealer") == 0)
                Camera = "Camera_Ship_Desk";
        }
        if (IsShownWindow(Win3DFindByName("NN_Dealer")) &&
            IsShownWindow(Win3DFindByName("NN_Inventory")))
            HudPostCommand(0x6b, 0);
        NotifyServiceWindow("NN_ShipTrader");
        RoomControlSetField1340(Room, 1);
        RoomControlClearField1338(Room);
        if (!RoomControlTrackCamera(Room, Camera, &NavBar::ScriptDone, this, nullptr))
        {
            OnButtonClicked(Current);
            return;
        }
        SetClientMouseCursorDrawFlag(0);
        ToggleDealerTabs(false);
        bReturnToDesk = 0;
        return;
    }
    case 9:
    case 0xa:
    {
        // the scripted camera track.
        RoomControlSetField1340(Room, 0);
        if (RoomControlTrackScriptCamera(Room, 0, &NavBar::ScriptDone, this, nullptr, 1))
            return;
        OnButtonClicked(Current);
        return;
    }
    default:
        OnButtonClicked(Current);
        return;
    }
}

void NavBar::ToggleDealerTabs(bool bShowDesk)
{
    // (RET 4). 0x00441a44..0x00441a97: the last desk tab (type 6) and the last
    // dealer tab (5 / 7 / 8); both needed.
    FNavBarButton* DeskTab = nullptr;
    FNavBarButton* DealerTab = nullptr;
    std::list<FNavBarButton>::iterator It;
    for (It = Buttons.begin(); It != Buttons.end(); ++It)
    {
        if (It->bRoomSwitch != 0)
            continue;
        if (It->Type == 6)
            DeskTab = &*It;
        else if (It->Type == 5 || It->Type == 7 || It->Type == 8)
            DealerTab = &*It;
    }
    if (DeskTab == nullptr || DealerTab == nullptr)
        return;

    float Offset[3] = {0.0f, 0.0f, 0.0f};
    if (bShowDesk)
    {
        // an open dealer tab put back; the desk tab enabled and moved
        // to the dealer tab's place; the dealer tab disabled.
        if (DealerTab->bOpen != 0)
        {
            DealerTab->bOpen = 0;
            DealerTab->Control->SetProperty(6, reinterpret_cast<uintptr_t>(DealerTab->Position), 0);
        }
        DeskTab->bDisabled = 0;
        DeskTab->Control->SetProperty(2, 0, 0);
        DealerTab->Control->SetProperty(7, reinterpret_cast<uintptr_t>(Offset), 0);
        DeskTab->Control->SetProperty(6, reinterpret_cast<uintptr_t>(Offset), 0);
        DealerTab->bDisabled = 1;
        DealerTab->Control->SetProperty(2, 1, 0);
        return;
    }
    // the mirror image.
    DealerTab->bDisabled = 0;
    DealerTab->Control->SetProperty(2, 0, 0);
    DeskTab->Control->SetProperty(7, reinterpret_cast<uintptr_t>(Offset), 0);
    DealerTab->Control->SetProperty(6, reinterpret_cast<uintptr_t>(Offset), 0);
    DeskTab->bDisabled = 1;
    DeskTab->Control->SetProperty(2, 1, 0);
}

void NavBar::StartScript(char const* Name, unsigned char bFade, unsigned char bLetterbox)
{
    // (RET 0xc): one script at a time.
    if (bScriptRunning != 0)
        return;
    bScriptRunning = 1;
    FGfScriptSequence* const Sequence = G_GfScriptManager.CreateSequence(CurrentRoomRuntime(), 0);
    Csys Frame;
    Frame.set_identity();
    Sequence->AddScript(Name, 0, &Frame, nullptr);
    Sequence->BindMode = 0;
    Sequence->bLetterbox = bLetterbox != 0 ? 1 : 0;
    Sequence->bFade = bFade;
    Sequence->bApplySpatial = 1;
    Sequence->bRestoreCamera = 0;
    Sequence->Priority = 1;
    Sequence->Callback = &NavBar::ScriptDone;
    Sequence->CallbackA = this;
    Sequence->CallbackB = nullptr;
    Sequence->Play();
    SetClientMouseCursorDrawFlag(0);
}

void __cdecl NavBar::ScriptDone(void* Bar, void* /*Unused*/, unsigned char /*bArg*/)
{
    // (cdecl; only the first argument is read).
    NavBar* const Self = static_cast<NavBar*>(Bar);
    Self->bScriptRunning = 0;
    SetClientMouseCursorDrawFlag(1);
    if (Self->Current != nullptr)
        Self->OnButtonClicked(Self->Current);
}

bool NavBar::CharacterClicked(FCharacterObject* Character, int Role)
{
    // (RET 8). 0x00441c75..0x00441caa: not while a click is pending; a shown
    // NN_Dealer takes it.
    if (bCharacterPending != 0)
        return false;
    if (IsShownWindow(Win3DFindByName("NN_Dealer")))
        return true;

    // the dealer's room -- not when already in it; its room button.
    char const* RoomName = nullptr;
    if (Role == 4)
        RoomName = "Equipment";
    else if (Role == 2)
        RoomName = "Trader";
    else if (Role == 3)
        RoomName = "ShipDealer";
    else
        return false;
    if (_stricmp(VirtualRoom.c_str(), RoomName) == 0)
        return false;
    FNavBarButton* Found = nullptr;
    std::list<FNavBarButton>::iterator It;
    for (It = Buttons.begin(); It != Buttons.end(); ++It)
    {
        if (It->bRoomSwitch == 0 || It->Hotspot == nullptr)
            continue;
        char const* const Switch = It->Hotspot->RoomSwitch;
        if (Switch != nullptr && strcmp(Switch, RoomName) == 0)
        {
            Found = &*It;
            break;
        }
    }
    if (Found == nullptr)
        return false;

    // the click held until the walk ends (Update), the room entered.
    bCharacterPending = 1;
    PendingCharacter = Character;
    if (Found->Hotspot != nullptr && Found->Hotspot->StateRead != 0)
    {
        if (Current == nullptr)
            Current = Found;
        ActivateButton(Current, Found);
        return true;
    }
    OnButtonClicked(Found);
    return true;
}

void NavBar::MakeScaledBackground(float Scale, char const* Mesh, float const* Position)
{
    // (RET 0xc). 0x00441ea7..0x00441ee2: the old background deleted.
    if (ScaledBackground != nullptr)
    {
        Win3DDeleteTree(ScaledBackground);
        ScaledBackground = nullptr;
    }

    // "ScaledBackground" (arguments 0, {0,0,0}, 0,0,0,1,0,0,1,0,0,0,
    // {0,0,0}).
    float const HalfExtents[3] = {0.0f, 0.0f, 0.0f};
    float const Zero338[3] = {0.0f, 0.0f, 0.0f};
    ScaledBackground = CreateStaticControl("ScaledBackground", Mesh, Position, nullptr,
                                           HalfExtents, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, Zero338);

    // the instance's rotation reset to identity scaled by Scale (its
    // translation kept), SetInstanceMatrix, UpdateInstance. No null or -1 test.
    void* const Instance = ScaledBackground->MeshHolder.EngineInstance;
    FTransform Transform;
    Transform.Rotation.D[0][0] = 1.0f;
    Transform.Rotation.D[0][1] = 0.0f;
    Transform.Rotation.D[0][2] = 0.0f;
    Transform.Rotation.D[1][0] = 0.0f;
    Transform.Rotation.D[1][1] = 1.0f;
    Transform.Rotation.D[1][2] = 0.0f;
    Transform.Rotation.D[2][0] = 0.0f;
    Transform.Rotation.D[2][1] = 0.0f;
    Transform.Rotation.D[2][2] = 1.0f;
    float const* const CurrentTransform = NeuronRuntime::Engine->GetInstanceTransform(Instance);
    Transform.Translation.X = CurrentTransform[9];
    Transform.Translation.Y = CurrentTransform[10];
    Transform.Translation.Z = CurrentTransform[11];
    if (G_NavBarMath == nullptr)
        G_NavBarMath = Acquire3DMathEngine();
    G_NavBarMath->Scale(Transform.Rotation, Transform.Rotation, Scale);
    NeuronRuntime::Engine->SetInstanceMatrix(Instance, &Transform);
    NeuronRuntime::Engine->UpdateInstance(Instance, 0, nullptr);
}

void NavBarEnsure()
{
    // found or created (the routine("NavBar", "NavBar", 0, 0)), then slot 2 and
    // slot 45 (+0xb4, tail jump).
    Win3D* Found = Win3DFindByName("NavBar");
    if (Found == nullptr)
    {
        Found = WinFactoryCreate("NavBar", "NavBar", nullptr, nullptr);
        if (Found == nullptr)
            return;
    }
    WinControl* const Control = Found->AsWinControl();
    if (Control == nullptr)
        return;
    static_cast<NavBar*>(Control)->OnLocationEntered();
}

void NavBarDeleteRoomControls()
{
    // for 1..9 each "RoomControl%d", then "ButtonControl%d", window answering
    // slot 2 goes through its scalar deleting dtor (no slot 10 Destroy).
    char Name[0x100];
    int Index = 0;
    for (Index = 1; Index < 10; ++Index)
    {
        sprintf(Name, "RoomControl%d", Index);
        Win3D* Found = Win3DFindByName(Name);
        if (Found != nullptr)
        {
            WinControl* const Control = Found->AsWinControl();
            if (Control != nullptr)
                delete Control;
        }
        sprintf(Name, "ButtonControl%d", Index);
        Found = Win3DFindByName(Name);
        if (Found != nullptr)
        {
            WinControl* const Control = Found->AsWinControl();
            if (Control != nullptr)
                delete Control;
        }
    }
}

void NavBarDestroy()
{
    // the "NavBar" window (no slot 2 test) -- its background subtree, the room
    // controls (the routine), slot 10 and the scalar deleting dtor.
    Win3D* const Found = Win3DFindByName("NavBar");
    if (Found == nullptr)
        return;
    NavBar* const Bar = static_cast<NavBar*>(Found);
    if (Bar->ScaledBackground != nullptr)
    {
        Win3DDeleteTree(Bar->ScaledBackground);
        Bar->ScaledBackground = nullptr;
    }
    NavBarDeleteRoomControls();
    Bar->Destroy();
    delete Bar;
}

void NavBarOpenDealer(int Index)
{
    // .
    NavBar* const Bar = FindNavBar();
    if (Bar == nullptr)
        return;
    // the routine("NN_Dealer"), the result unused.
    Win3DFindByName("NN_Dealer");
    Bar->ButtonMover::Finish();
    Bar->ToggleDealerTabs(true);
    Bar->bReturnToDesk = 1;
    NnInventoryOpenShipDealer(Index, nullptr, nullptr); // the routine(Index, 0, 0)
}

void NavBarOpenButton(unsigned Type)
{
    // not during a repair redirect; a running slide finished first; the first
    // button (of either kind) of Type activated.
    NavBar* const Bar = FindNavBar();
    if (Bar == nullptr)
        return;
    if (Bar->bRepairRedirect != 0)
        return;
    if (Bar->ButtonMover::IsActive())
        Bar->ButtonMover::Finish();
    std::list<FNavBarButton>::iterator It;
    for (It = Bar->Buttons.begin(); It != Bar->Buttons.end(); ++It)
    {
        if (It->Type != Type)
            continue;
        FNavBarButton* const Button = &*It;
        if (Button->Hotspot != nullptr && Button->Hotspot->StateRead != 0)
        {
            if (Bar->Current == nullptr)
                Bar->Current = Button;
            Bar->ActivateButton(Bar->Current, Button);
            return;
        }
        Bar->OnButtonClicked(Button);
        return;
    }
}

void NavBarCloseService(bool bNotify)
{
    // the first open tab closed when it is a service (2 / 3 / 7 / 0xc; the
    // body inlined).
    NavBar* const Bar = FindNavBar();
    if (Bar == nullptr)
        return;
    if (bNotify)
        NavBar::NotifyServiceWindows();
    std::list<FNavBarButton>::iterator It;
    for (It = Bar->Buttons.begin(); It != Bar->Buttons.end(); ++It)
    {
        if (It->bRoomSwitch != 0 || It->bOpen == 0)
            continue;
        if (IsServiceType(It->Type))
            Bar->CloseService(&*It);
        return;
    }
}

bool NavBarHasServiceWindow()
{
    // .
    if (FindNavBar() == nullptr)
        return false;
    if (Win3DFindByName("NN_Repair") != nullptr)
        return true;
    if (Win3DFindByName("NN_ShipTrader") != nullptr)
        return true;
    if (Win3DFindByName("NN_News") != nullptr)
        return true;
    return Win3DFindByName("NN_MissionVendor") != nullptr;
}

bool NavBarIsAnimating()
{
    // the "NavBar" window's +0x32c / +0x34c active flags, no slot 2 test.
    Win3D* const Found = Win3DFindByName("NavBar");
    if (Found == nullptr)
        return false;
    return static_cast<NavBar*>(Found)->IsAnimating();
}

void NavBarRefreshRepairTab(bool bForce)
{
    // .
    NavBar* const Bar = FindNavBar();
    if (Bar == nullptr)
        return;
    Bar->RefreshRepairTab(bForce);
}

bool NavBarCharacterClicked(FCharacterObject* Character, int Role)
{
    // .
    NavBar* const Bar = FindNavBar();
    if (Bar == nullptr)
        return false;
    return Bar->CharacterClicked(Character, Role);
}

void NavBarShowDealerTab()
{
    // .
    NavBar* const Bar = FindNavBar();
    if (Bar == nullptr)
        return;
    Bar->ToggleDealerTabs(false);
}
