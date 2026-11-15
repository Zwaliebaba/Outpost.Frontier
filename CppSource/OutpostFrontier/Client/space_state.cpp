// Author: Giperion.

#include "space_state.h"
#include "space_app.h"

#include "character_info.h"         // ResetCharacterScripts (the routine)
#include "chat_init.h"              // ChatShowMsgWin (the routine)
#include "login_mgr.h"              // WaitForServerResponse (the routine), the field
#include "main_command_handler.h"   // SetClientQuitRequested (the field)
#include "nn_arrival_text.h"        // NnArrivalTextShowSystemNow (the routine)
#include "space_scene_load.h"       // / the routine
#include "hud_target.h"             // HudIsMousePickBlocked (the routine)
#include "common_misc.h"            // Geometry::Sphere, SinglePlayer, GetFileSysPath, GetFullFilename
#include "common_universe.h"        // ISpatialPartition, SpatialFill
#include "neuroncomponents.h"                  // FDUMP
#include "character_select_equip.h" // / the routine
#include "client_equip_fx.h"
#include "client_frame_tick.h"      // / the routine / the routine, the routine
#include "client_tick_profile.h"    // Port-only owner-requested diagnostics
#include "client_fuse.h"
#include "client_gameobj.h"         // FClientGameobj (the player's object, the routine)
#include "client_objdb.h"
#include "cockpit_load.h"
#include "effect_type_list.h"
#include "engine_light.h"
#include "fade_to_black.h"
#include "game_options.h"
#include "hud_command_dispatch.h"   // HudPostCommand (the routine)
#include "hud_element_list.h"
#include "hud_status_core.h"        // GetHudStatus (the field) +0x5b9 / +0x5ba
#include "hud_targetable_objects.h"
#include "input_manager.h"
#include "gameflow.h"               // ResetEngineInstanceCache (the routine on the field)
#include "gate_tunnel.h"
#include "jump_effect.h"
#include "loading_screen.h"         // LoadingScreenReset (the routine)
#include "menu_keymap_config.h"
#include "menu_sound_arch.h"
#include "menu_ui_widgets.h"
#include "nn_exit.h"                // NnExitPostShipLostMessage (the routine)
#include "nn_modal.h"
#include "player_behavior.h"
#include "player_spawn.h"
#include "sound_config.h"
#include "space_camera.h"
#include "space_camera_game.h"
#include "space_letterbox.h"        // IsLetterboxVisible / SetLetterboxTarget (the routine / 0041a0f0)
#include "space_scene_obj_draw.h"   // LightGridRebuild (the routine), SpaceDrawQueueRelease (the routine)
#include "star_sphere.h"
#include "star_sys.h"
#include "tradelane_fx.h"
#include "win3d.h"                  // Win3DDrawAll (the routine)
#include "win3d_renderables.h"
#include "common_animdb.h"
#include "common_iface.h"
#include "common_physys.h"
#include "NeuronRuntime.h"
#include "globals.h"
#include "render/IRenderPipeline.h"
#include "win3d_render.h"
#include "common_cobject.h"
#include "common_ini.h"     // RefreshLoadingProgress
#include "common_server.h"  // IServer (the field)
#include "server_connect.h" // GetLocalServerInterface

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

namespace
{
    // the value the space frame returns -- the next client state, 0 to stay.
    // The frame zeroes it at 0x0054b02b and raises 6 from +0x0a (0x0054b208); the space init
    // zeroes it (0x0054abec / 0x0054ac03 / 0x0054ae5d) and the routine stores 4.
    int G_SpaceNextState = 0;

    // the base a landing ship asked for (the routine). The space frame reads it
    // at 0x0054aaea (ProcessPendingSystemSwitch: the routine with it, the routine(0), next
    // state 3, then zeroes it).
    unsigned G_PendingLandBase = 0;

    // / the field: the system a jumping ship asked for and a byte beside it
    // (the routine, the Ship's jump state 3 at 0x005439d6 and the IClient system-switch-out
    // handler the routine at 0x0054378a). ProcessPendingSystemSwitch consumes the system and
    // zeroes both; nothing in the listing reads the field (XREFs: 0x00548ee1 W, 0x0054aad9 W).
    unsigned G_PendingJumpSystem = 0;
    unsigned G_PendingJumpFlag = 0;

    // The player's ship as a CEqObj through the watcher the field: minus eight, the +0xc'd
    // pointer's +0x4, ship class mask 0x503 -- the chain the routine walks twice. Null when any
    // link is missing.
    CEqObj* PlayerShipEqObj()
    {
        void* const ObjRW = GetPlayerObjRW();
        if (ObjRW == nullptr || ObjRW == reinterpret_cast<void*>(-0xc))
            return nullptr;
        CEqObj* const EqObj =
            *reinterpret_cast<CEqObj**>(static_cast<unsigned char*>(ObjRW) + 0x10);
        if (EqObj == nullptr ||
            (static_cast<unsigned>(EqObj->ObjectClass) & 0x503u) != 0x503u)
            return nullptr;
        return EqObj;
    }

    // (cdecl, one caller 0x0054b40d): the docking port's camera hardpoint
    // "HPDockCam" + ('A' + PortIndex) (sprintf "%s%c" into a 0x40-byte buffer), read through the
    // port's IObjInspect slot 48 get_hardpoint(Name, &Csys[9] position, &Csys[0] orientation);
    // true when that answers 0 (NEG / SBB / INC).
    bool GetDockCameraCsys(IObjRW const* Port, int PortIndex, float* Csys12)
    {
        char Name[0x40];
        sprintf_s(Name, "%s%c", "HPDockCam", PortIndex + 0x41);
        return Port->get_hardpoint(Name, reinterpret_cast<Vector*>(Csys12 + 9),
                                   reinterpret_cast<Matrix*>(Csys12)) == 0;
    }

    // (thiscall on the state, 0x0054b08d).
    void UpdateSpaceDocking(SpaceApp* State)
    {
        if (State->bSpaceReady != 0)
        {
            // once the ship is no longer excluded from control bit 4
            // the launch is over -- +0xba9 drops and, unless +0xba8 holds it, the camera goes
            // back to "player" (attach target 0).
            CEqObj* const EqObj = PlayerShipEqObj();
            if (EqObj != nullptr && !EqObj->is_control_excluded(4u))
            {
                State->bSpaceReady = 0;
                if (State->bScriptCameraLatch == 0)
                    SwitchSpaceCameraMode("player", nullptr);
            }
        }
        else if (State->bDockingPending == 0)
        {
            // the object the active camera (+0xba0, not tested again
            // after the frame's own test) watches through its +0xc0 BaseWatcher, minus eight;
            // null when nothing is watched.
            FGameCamera* const Cam = reinterpret_cast<FGameCamera*>(GetActiveSpaceCamera());
            Watchable const* const Watched = Cam->Watched.target();
            void const* const WatchedObjRW =
                Watched != nullptr ? reinterpret_cast<unsigned char const*>(Watched) - 8
                                   : nullptr;
            // the player's ship CEqObj (the chain above) -- and the
            // camera must be watching exactly that object.
            CEqObj* const EqObj = PlayerShipEqObj();
            if (EqObj != nullptr && WatchedObjRW == GetPlayerObjRW())
            {
                // CEqObj::get_behavior_interface, then
                // IBehaviorManager::get_docking_port(&Port, &Index); its answer is +0xbaa.
                IObjRW const* Port = nullptr;
                int PortIndex = 0;
                State->bDockingPending = static_cast<char>(
                    EqObj->get_behavior_interface()->get_docking_port(Port, PortIndex) ? 1 : 0);
                if (State->bDockingPending != 0)
                {
                    // the server (the field, no test) slot 57
                    // SPRequestInvincibility(ship +0xb0 id, 1, 2, the field).
                    static_cast<IServer*>(GetLocalServerInterface())
                        ->SPRequestInvincibility(static_cast<CSimple*>(EqObj)->Id, true,
                                                 INVINCIBILITY_REASON_2,
                                                 GetPlayerConnectionId());
                    // the port's type (IObjInspect slot 34 get_type
                    // into a zeroed local, skipped without a port) tested for 0x120.
                    unsigned PortType = 0;
                    if (Port != nullptr)
                        Port->get_type(PortType);
                    if ((PortType & 0x120u) != 0)
                    {
                        // an identity Csys, the routine fills it from
                        // the port's HPDockCam hardpoint.
                        float Csys[12] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
                                          0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f};
                        if (GetDockCameraCsys(Port, PortIndex, Csys))
                        {
                            // the body of the routine inlined -- the
                            // docking camera's Csys (the field) and view-dirty byte
                            // (the field), then with the field the routine("docking", 0).
                            SetDockingCameraTransform(Csys);
                        }
                    }
                    // the rear view (+0x5ba == 1) and the turret view
                    // (+0x5b9 == 1) of HUD_Status (the field, no test) are cancelled
                    // through the routine(0x39, 0) / (0xbf, 0).
                    if (GetHudStatus()->bRearView == 1)
                        HudPostCommand(0x39u, 0);
                    if (GetHudStatus()->bTurretView == 1)
                        HudPostCommand(0xbfu, 0);
                    // no letterbox yet (the routine) -> bars to 1.0 at
                    // 3.0 per second (the routine) and the software cursor off
                    // (the routine(0)).
                    if (!IsLetterboxVisible())
                    {
                        SetLetterboxTarget(1.0f, 3.0f);
                        SetClientMouseCursorDrawFlag(0);
                    }
                    // CEqObj::set_control_exclusion(4).
                    EqObj->set_control_exclusion(4u);
                }
            }
        }

        // while docking (+0xbaa) with a player object, the ship
        // Gameobj's +0x158 state (the routine) steers +0xbab: raised once the state leaves 0,
        // and when it is back at 0 with +0xbab raised both bytes drop.
        void* const PlayerObjRW = GetPlayerObjRW();
        if (State->bDockingPending != 0 && PlayerObjRW != nullptr)
        {
            FClientShipGameobj const* const Ship = static_cast<FClientShipGameobj const*>(
                static_cast<IObjRW const*>(PlayerObjRW));
            int const DockState = Ship->Field_0x158;
            if (State->bDockingActive != 0)
            {
                if (DockState == 0)
                {
                    State->bDockingActive = 0;
                    State->bDockingPending = 0;
                }
            }
            else if (DockState != 0)
            {
                State->bDockingActive = 1;
            }
        }
    }
}

// . Its static initializer the branch runs the routine and registers
// (-> the routine) with atexit -- a C++ global with a ctor and a dtor.
SpaceApp G_SpaceApp;

SpaceApp::SpaceApp()
{
    // . 0x0054931f: the Event::Handler face's base vptr 0x005cbed8 -- the Events
    // member, constructed before this body.

    // the eight cameras, in this order.
    FirstPersonCameraConstruct(&FirstPerson); // on +0x00c
    ThirdPersonCameraConstruct(&ThirdPerson); // on +0x1b0
    TurretCameraConstruct(&Turret);           // on +0x3c8
    RearViewCameraConstruct(&RearView);       // on +0x500
    DeathCameraConstruct(&Death);             // , 4 floats, vt 0x005e061c (+0x60c)
    ScriptCameraConstruct(&Script);           // on +0x760
    StaticCameraConstruct(&Static);           // , vt 0x005e0594 (+0x9a4)
    DockingCameraConstruct(&Docking);         // on +0xa9c

    // / 0x005493e2: SpaceApp's own two vptrs 0x005e0554 / 0x005e0548 -- the C++
    // vptrs of IEnvironment and of the Events face.

    // .
    bCameraReady = 0;
    ActiveCamera = nullptr;
    bPlayerLostHandled = 0;
    bLeaveRequested = 0;

    // the field unhooked when it holds something.
    ClearPlayerShipWatcher();

    // .
    bDockingPending = 0;
    bDockingActive = 0;
    bSpaceReady = 0;
    bScriptCameraLatch = 0;
    bInitInProgress = 0;
    PlayerPreference = 1;
}

SpaceApp::~SpaceApp()
{
    // Member destruction runs Docking, Static, Script, Death, RearView, Turret, ThirdPerson,
    // FirstPerson, then Events, matching the routine. Each HUD camera destroys its
    // EventHandler base before FGameCamera's watcher.
}

IEnvironment* GetSpaceState()
{
    // MOV EAX,0x00678b98 / RET.
    return &G_SpaceApp;
}

// Slots 0, 2, 3 and 4 (the branch RET 0x10, the branch RET 0xc, the branch RET 0x10,
// RET 0xc): window-message handlers that build an input event and offer it to the
// active camera (+0xba0) slots 0x70 / 0x74 and the watched gameobj's slot 0x1cc. Nothing in the
// exe calls slots 0..6 of any state (every holder of the state pointer read -- tracker
// has the survey), so their bodies are not ported (tracker: skipped, dead in
// baseline); these overrides only fill the slots and answer false.
bool SpaceApp::Slot00(unsigned A, unsigned Message, unsigned B, unsigned C)
{
    (void)A;
    (void)Message;
    (void)B;
    (void)C;
    return false;
}

// Slot 1 (0x004b50e0): XOR AL,AL / RET 0xc.
bool SpaceApp::Slot01(unsigned A, unsigned B, unsigned C)
{
    (void)A;
    (void)B;
    (void)C;
    return false;
}

bool SpaceApp::Slot02(unsigned A, unsigned B, unsigned long Packed)
{
    (void)A;
    (void)B;
    (void)Packed;
    return false;
}

bool SpaceApp::Slot03(unsigned A, unsigned Message, unsigned B, unsigned long Packed)
{
    (void)A;
    (void)Message;
    (void)B;
    (void)Packed;
    return false;
}

bool SpaceApp::Slot04(unsigned A, unsigned B, unsigned C)
{
    (void)A;
    (void)B;
    (void)C;
    return false;
}

// Slot 5 (0x00401e90): a bare RET.
void SpaceApp::Slot05()
{
}

// Slot 6 (0x0053e4a0): a bare RET 4.
void SpaceApp::Slot06(unsigned A)
{
    (void)A;
}

// Slot 7 (0x004b5010): XOR EAX,EAX / RET -- environment type 0.
int SpaceApp::GetEnvironmentType()
{
    return 0;
}

// Slot 8 (0x00401e90): a bare RET (InitializeEngine 0x005b1e9c..0x005b1ea5 calls it).
void SpaceApp::PreInit()
{
}

// Slot 9: the routine.
void SpaceApp::Shutdown()
{
    ShutdownSpaceState(this);
}

// Slot 10 (0x00401e90): a bare RET (the routine case 5 calls it).
void SpaceApp::Slot10()
{
}

// slot 11 (+0x2c), thiscall, RET 8. Its caller is the render surface reset
// (Width, Height). With an active camera (+0xba0): a local rectangle {0, 0,
// Width - 1, Height - 1} handed to the camera's slot 0x5c (the viewport apply). No active
// camera: nothing. `this` is read only for +0xba0.
void SpaceApp::Resize(int Width, int Height)
{
    FSpaceCamera* const Active = ActiveCamera;
    if (Active == nullptr)
        return;
    int ViewRect[4];
    ViewRect[0] = 0;
    ViewRect[1] = 0;
    ViewRect[2] = Width - 1;  // DEC EAX -> [ESP + 0x8]
    ViewRect[3] = Height - 1; // DEC EDX -> [ESP + 0xc]
    ((FGameCamera*)Active)->ApplyViewport(ViewRect);
}

// Slot 12: the routine.
bool SpaceApp::Init(int Param)
{
    return InitSpaceState(this, Param);
}

// Slot 13: the routine.
void SpaceApp::Destroy()
{
    DestroySpaceState(this);
}

// Slot 14: the branch.
int SpaceApp::Update(float DeltaSeconds)
{
    return UpdateSpaceState(this, DeltaSeconds);
}

namespace
{
    // The two-way tail the branch / the branch of the face's handler: back to "turret" when
    // the turret view is still up (HUD_Status +0x5b9), else to "player" -- each only when the
    // controller byte +0x08 is up -- then HUD_Status the routine.
    void SpaceAppRestoreViewAfterRear(SpaceApp* App)
    {
        HUD_Status* const Status = GetHudStatus();
        if (Status->bTurretView != 0)
        {
            if (App->bCameraReady != 0)
                SwitchSpaceCameraMode("turret", nullptr);
        }
        else if (App->bCameraReady != 0)
        {
            SwitchSpaceCameraMode("player", nullptr);
        }
        GetHudStatus()->UpdateSpecialModeCaptions();
    }
}

bool FSpaceAppEventFace::OnEvent(unsigned MsgId)
{
    // thiscall on the face (+4), RET 4. Every controller field is read through
    // the face: [face + 0xba0] is SpaceApp + 0xba4, [face + 0xb9c] + 0xba0, [face + 0xba6] + 0xbaa,
    // face + 0x4fc the rear-view camera at + 0x500. The switch (0x0054a587..0x0054a59f) takes
    // MsgId - 0xf through the byte table 0x0054a888: 0x0f, 0x31, 0x38, 0x39, 0xbf; every other id
    // answers false (0x0054a867).
    SpaceApp* const App = reinterpret_cast<SpaceApp*>(reinterpret_cast<unsigned char*>(this) -
                                                      offsetof(SpaceApp, Events));
    HUD_Status* Status = nullptr;

    switch (MsgId)
    {
    case 0x31:
        // the first/third-person toggle, refused while the mouse pick
        // is blocked (the routine).
        if (HudIsMousePickBlocked())
            return false;
        if (App->PlayerPreference == 0)
        {
            App->PlayerPreference = 1;
            if (App->bCameraReady != 0)
                SwitchSpaceCameraMode("third_person", nullptr);
            SetGameSessionFlags(8, false); // (8, 0)
            return true;
        }
        if (App->PlayerPreference == 1)
        {
            App->PlayerPreference = 0;
            if (App->bCameraReady != 0)
                SwitchSpaceCameraMode("first_person", nullptr);
            SetGameSessionFlags(8, true); // (8, 1)
            return true;
        }
        // -> 0x0054a6d3: any other preference -- consumed, nothing done.
        return true;

    case 0xbf:
        // the turret view toggle (HUD_Status +0x5b9), refused while the
        // mouse pick is blocked.
        if (HudIsMousePickBlocked())
            return false;
        Status = GetHudStatus();
        Status->bTurretView = Status->bTurretView == 0 ? 1 : 0;
        if (GetHudStatus()->bTurretView != 0)
        {
            // the player's behaviour manager (the field minus eight,
            // or null) drops its steering direction.
            IBehaviorManager* const Manager =
                GetBehaviorManager(static_cast<IObjRW*>(GetPlayerObjRW()));
            if (Manager != nullptr)
                Manager->reset_current_behavior_direction();
            if (App->bCameraReady != 0)
                SwitchSpaceCameraMode("turret", nullptr);
        }
        else
        {
            // the routine(0x36, 0), then "player".
            HudPostCommand(0x36u, 0);
            if (App->bCameraReady != 0)
                SwitchSpaceCameraMode("player", nullptr);
        }
        GetHudStatus()->UpdateSpecialModeCaptions(); // the routine
        return true;

    case 0x38:
        // the rear view follows the key (the command record's key-down
        // byte, the routine +0x18). With the mouse pick blocked only a rear view already up
        // gets through (to be released).
        if (HudIsMousePickBlocked() && GetHudStatus()->bRearView == 0)
            return false;
        GetHudStatus()->bRearView = HudCommandRecord()->Byte18 != 0 ? 1 : 0;
        if (GetHudStatus()->bRearView != 0)
        {
            if (App->bCameraReady != 0)
                SwitchSpaceCameraMode("rearview", nullptr);
            GetHudStatus()->UpdateSpecialModeCaptions();
            return true;
        }
        // released -- only when the rear-view camera is the active one
        // does the view go back.
        if (App->ActiveCamera != SpaceCameraOf(&App->RearView))
        {
            GetHudStatus()->UpdateSpecialModeCaptions();
            return true;
        }
        SpaceAppRestoreViewAfterRear(App);
        return true;

    case 0x39:
        // rear view off (+0x5ba = 0); the same tail as 0x38's release.
        GetHudStatus()->bRearView = 0;
        if (App->ActiveCamera != SpaceCameraOf(&App->RearView))
        {
            GetHudStatus()->UpdateSpecialModeCaptions();
            return true;
        }
        SpaceAppRestoreViewAfterRear(App);
        return true;

    case 0x0f:
    {
        // single player only, while docking (+0xbaa): the player ship's
        // docking port (IBehaviorManager::get_docking_port with a null port and index -1, the
        // answer unread), that port's dock target (IObjInspect slot 36), and when it names one
        // IServer (the field, no test) slot 91 Dock(target, the field).
        if (!SinglePlayer())
            return false;
        if (App->bDockingPending == 0)
            return false;
        CEqObj* const EqObj = PlayerShipEqObj();
        if (EqObj == nullptr)
            return false;
        IBehaviorManager* const Manager = EqObj->get_behavior_interface();
        if (Manager == nullptr)
            return false;
        IObjRW const* Port = nullptr;
        int PortIndex = -1;
        Manager->get_docking_port(Port, PortIndex);
        if (Port == nullptr)
            return false;
        unsigned DockTarget = 0;
        Port->get_dock_target(DockTarget);
        if (DockTarget == 0)
            return false;
        const unsigned Connection = GetPlayerConnectionId();
        static_cast<IServer*>(GetLocalServerInterface())->Dock(DockTarget, Connection);
        return true;
    }

    default:
        return false;
    }
}

void SpaceAppLoadCamerasIni(SpaceApp* App)
{
    // (thiscall, no arguments). 0x00549855..0x00549882: GetFullFilename
    // ("cameras.ini"), a stack INI_Reader, open(path, false).
    INI_Reader Reader;
    if (Reader.open(GetFullFilename("cameras.ini"), false))
    {
        // every section.
        while (Reader.read_header())
        {
            // the camera a section names. [WinCamera] is the Win3D
            // camera the field; the rest are the controller's own.
            FSpaceCamera* Camera = nullptr;
            if (Reader.is_header("WinCamera"))
                Camera = Win3DGetCamera();
            else if (Reader.is_header("CockpitCamera"))
                Camera = SpaceCameraOf(&App->FirstPerson);
            else if (Reader.is_header("ThirdPersonCamera"))
                Camera = SpaceCameraOf(&App->ThirdPerson);
            else if (Reader.is_header("TurretCamera"))
                Camera = SpaceCameraOf(&App->Turret);
            else if (Reader.is_header("RearViewCamera"))
                Camera = SpaceCameraOf(&App->RearView);
            else if (Reader.is_header("DeathCamera"))
                Camera = SpaceCameraOf(&App->Death);
            else
            {
                // space.cpp line 0x17e.
                FDUMP(0x100002, "%s(%d) : *** WARNING: Unknown camera type? (%s)",
                      "Client\\space\\space.cpp", 0x17e,
                      Reader.get_header_ptr());
                continue;
            }

            // every entry to the camera's slot 0x4c (SetProperty); a
            // refused one warns with space.cpp line 0x18a. (The [WinCamera] path jumps straight
            // here, past the null test the others make at 0x00549930 -- they are never null.)
            while (Reader.read_value())
            {
                const char* const Name = Reader.get_name_ptr();
                const char* const Value = Reader.get_value_string();
                // slot 0x4c __thiscall (ECX = camera, key and value
                // pushed), AL tested -- ICamera::SetProperty.
                if (static_cast<unsigned char>(AsCamera(Camera)->SetProperty(Name, Value)) == 0)
                {
                    FDUMP(0x100002, "%s(%d) : *** WARNING: [%s] unknown property '%s = %s'",
                          "Client\\space\\space.cpp", 0x18a,
                          Reader.get_header_ptr(), Name, Value);
                }
            }
        }
    }
    // the INI_Reader's inlined destructor -- ~INI_Reader here.
}

void SpaceAppArmDeathCamera()
{
    // (cdecl, no arguments). The death camera is SpaceApp + 0x60c = 0x006791a4:
    // its +0x04 World is the field, +0x34 the field, +0x110 the field, +0x134
    // , +0x140..+0x150 the field..the field.
    FDeathCamera* const Death = &G_SpaceApp.Death;

    // the client ObjDB's spatial bucket (slot 0xc, mask 0xfdff).
    ISpatialPartition* const Bucket =
        static_cast<ISpatialPartition*>(GetClientObjDb()->GetBucket(0xfdff));
    bool bCrowded = false;
    if (Bucket != nullptr)
    {
        // everything within 50 (the field) of the active camera's
        // position (slot 0x0c; +0xba0 is not tested), capacity 0x800.
        Geometry::Sphere Query;
        const float* const CameraPos = AsCamera(G_SpaceApp.ActiveCamera)->GetPosition();
        Query.Center.x = CameraPos[0];
        Query.Center.y = CameraPos[1];
        Query.Center.z = CameraPos[2];
        Query.Radius = 50.0f;
        FSpatialQueryResult Result;
        Result.Capacity = 0x800;
        Result.Count = 0;
        if (SpatialFill(Bucket, &Query, &Result) != 0)
        {
            // something close -- the rear view (0x39) and the turret
            // view (0xbf) closed when up, and the drift limited to 100.
            if (GetHudStatus()->bRearView == 1)
                HudPostCommand(0x39u, 0);
            if (GetHudStatus()->bTurretView == 1)
                HudPostCommand(0xbfu, 0);
            Death->MaxDriftSpeed = 100.0f;
            bCrowded = true;
        }
        else
        {
            // nothing close -- only the rear view is closed.
            if (GetHudStatus()->bRearView == 1)
                HudPostCommand(0x39u, 0);
        }
    }
    if (!bCrowded)
        Death->MaxDriftSpeed = 200.0f;

    // +0x14c = 0.5; a first-person view goes to "third_person" on the
    // player's object first.
    Death->DriftDamping = 0.5f;
    if (G_SpaceApp.ActiveCamera == SpaceCameraOf((FHudCamera*)&G_SpaceApp.FirstPerson))
        SwitchSpaceCameraMode("third_person", GetPlayerObjRW());

    // the listener camera's (the routine) orientation + position
    // (slot 0x10, twelve floats) into the death camera's World, its 3x3 into the shake matrix,
    // its position into the drift accumulator; view dirty, timer = duration, drift speed 1.0.
    FSpaceCamera* const Listener = static_cast<FSpaceCamera*>(GetClientSoundListenerObject());
    const float* const Src = AsCamera(Listener)->GetOrientation();
    FSpaceCamera* const DeathBase = SpaceCameraOf((FGameCamera*)Death);
    DeathBase->bViewDirty = 1;
    memcpy(DeathBase->World, Src, 9 * sizeof(float));
    memcpy(DeathBase->WorldPos, Src + 9, 3 * sizeof(float));
    memcpy(Death->ShakeOrientation, Src, 9 * sizeof(float));
    Death->DriftPosition[0] = Src[9];
    Death->DriftPosition[1] = Src[10];
    Death->DriftPosition[2] = Src[11];
    Death->ShakeTimer = Death->ShakeDuration;
    Death->DriftSpeed = 1.0f;

    // the player's object narrowed with mask 0x103 (a CEqObj); control
    // exclusion 3 set unless it already is.
    void* const PlayerRW = GetPlayerObjRW();
    if (PlayerRW != nullptr && PlayerRW != reinterpret_cast<void*>(-0xc))
    {
        CEqObj* const EqObj =
            *reinterpret_cast<CEqObj**>(static_cast<unsigned char*>(PlayerRW) + 0x10);
        if (EqObj != nullptr &&
            (static_cast<unsigned>(EqObj->ObjectClass) & 0x103u) == 0x103u &&
            !EqObj->is_control_excluded(3u))
        {
            EqObj->set_control_exclusion(3u);
        }
    }

    // "death" on the player's object (re-read).
    SwitchSpaceCameraMode("death", GetPlayerObjRW());
}

void SetStaticCameraTransform(const float* Csys12)
{
    // (cdecl). 0x0054bb56..0x0054bb67: twelve dwords into the static camera's
    // Csys (the field = SpaceApp + 0x9a4 + 0x04) and its view-dirty byte (the field =
    // + 0x34) -- before the guard.
    FSpaceCamera* const Static = SpaceCameraOf((FGameCamera*)&G_SpaceApp.Static);
    Static->bViewDirty = 1;
    memcpy(Static->World, Csys12, 9 * sizeof(float));
    memcpy(Static->WorldPos, Csys12 + 9, 3 * sizeof(float));

    // the field, then the routine("static", 0).
    if (G_SpaceApp.bCameraReady != 0)
        SwitchSpaceCameraMode("static", nullptr);
}

void SetPendingLandBase(unsigned const* BaseId)
{
    // (cdecl): the field = *BaseId, no null test.
    G_PendingLandBase = *BaseId;
}

void EnterBaseFromDock(unsigned const* BaseId)
{
    // (cdecl): the routine(BaseId), the routine(0), the field = 4.
    SetPlayerBaseIdbaseline(*BaseId);
    SetPlayerFactionScratchbaseline(0);
    G_SpaceNextState = 4;
}

void SetPendingJumpSystem(unsigned const* SystemId, bool bFlag)
{
    // (cdecl): the field = *SystemId (no null test), the field = the byte
    // argument zero-extended (MOVZX).
    G_PendingJumpSystem = *SystemId;
    G_PendingJumpFlag = bFlag ? 1u : 0u;
}

namespace
{
    // (thiscall on the space state; `this` is never read). Called once per space
    // frame at 0x0054b177.
    void ProcessPendingSystemSwitch()
    {
        if (G_PendingJumpSystem != 0)
        {
            // the player's object through the BaseWatcher the field
            // (minus eight = the IObjRW, +0xc the CBase, +4 its object) when its class has bit 0
            // set, else null -- used below with no test.
            IObjRW* const PlayerRW = static_cast<IObjRW*>(GetPlayerObjRW());
            CObject* Object = nullptr;
            if (PlayerRW != nullptr)
            {
                CObject* const Candidate = static_cast<FClientGameobj*>(PlayerRW)->GetObject();
                if (Candidate != nullptr &&
                    (static_cast<unsigned>(Candidate->ObjectClass) & 1u) == 1u)
                {
                    Object = Candidate;
                }
            }

            RefreshLoadingProgress();

            // the re-make parameters -- mass 1.0f, the default
            // dampings, system 0, type 2, flag 0, collisions on, empty name, no inertia.
            PhySys::CreateParms Parms;
            Parms.Mass = 1.0f;
            Parms.LinearDamping = PhySys::DEFAULT_LINEAR_DAMPING;
            Parms.AngularDamping[0] = PhySys::DEFAULT_ANGULAR_DAMPING.x;
            Parms.AngularDamping[1] = PhySys::DEFAULT_ANGULAR_DAMPING.y;
            Parms.AngularDamping[2] = PhySys::DEFAULT_ANGULAR_DAMPING.z;
            Parms.CollisionGroupId = 0;
            Parms.Type = 2;
            Parms.bFlag1c = 0;
            Parms.bEnableCollisions = 1;
            Parms.Name[0] = '\0';
            Parms.bHasInertia = 0;
            Parms.Inertia.x = 0.0f;
            Parms.Inertia.y = 0.0f;
            Parms.Inertia.z = 0.0f;

            // the object's mass (vt+0x6c), then the inertia flag.
            Parms.Mass = Object->get_mass();
            Parms.bHasInertia = 1;

            // the moment of inertia when the object has a body (+0x54),
            // else zero.
            Vector Inertia;
            if (Object->BodyPtr != nullptr)
            {
                Inertia = PhySys::GetMomentOfInertia(Object);
            }
            else
            {
                Inertia.x = 0.0f;
                Inertia.y = 0.0f;
                Inertia.z = 0.0f;
            }
            Parms.Inertia = Inertia;

            // the collision state, the dampings and the collision group
            // name (strcpy, unbounded, as baseline) off the object as it is now.
            Parms.bEnableCollisions = PhySys::GetCollisionState(Object) ? 1 : 0;
            Vector AngularDamping;
            AngularDamping.x = Parms.AngularDamping[0];
            AngularDamping.y = Parms.AngularDamping[1];
            AngularDamping.z = Parms.AngularDamping[2];
            PhySys::GetDamping(Object, Parms.LinearDamping, AngularDamping);
            Parms.AngularDamping[0] = AngularDamping.x;
            Parms.AngularDamping[1] = AngularDamping.y;
            Parms.AngularDamping[2] = AngularDamping.z;
            strcpy(Parms.Name, PhySys::GetCollisionGroup(Object));

            // out of the physics (vt+0x74), out of the old system with
            // the player gameobj (the routine), the new system recorded (the routine) and
            // entered (the routine).
            Object->unmake_physical();
            StarSysLeave(PlayerRW);
            SetPlayerStarSystemIdbaseline(G_PendingJumpSystem);
            StarSysEnter(G_PendingJumpSystem);

            // back into the physics in the new system, radius 0.
            Parms.CollisionGroupId = G_PendingJumpSystem;
            Object->remake_physical(Parms, 0.0f);

            // IServer (the field) slot 47 with the player's ship id
            // (the field) and connection (the field).
            static_cast<IServer*>(GetLocalServerInterface())
                ->SystemSwitchOutComplete(GetPlayerShipId(), GetPlayerConnectionId());

            // .
            G_PendingJumpFlag = 0;
            G_PendingJumpSystem = 0;
            return;
        }

        // a landing ship's base -- the routine(&base),
        // (0), next state 3 (enter the base), and the request consumed.
        if (G_PendingLandBase != 0)
        {
            SetPlayerBaseIdbaseline(G_PendingLandBase);
            SetPlayerFactionScratchbaseline(0);
            G_SpaceNextState = 3;
            G_PendingLandBase = 0;
        }
    }
}

namespace
{
    // The local at [EBP-0x234] of the routine: a pointer to a byte that is raised on
    // construction (0x0054ab64 stores &this->+0x0b, 0x0054ab68 writes 1, then the try level
    // becomes 0) and dropped on scope exit. the routine (MOV EAX,[ECX]; MOV byte [EAX],0; RET)
    // is its destructor, reached only through the unwind funclet Unwind@005c12b0 (LEA ECX,
    // [EBP-0x234]); both normal exits inline it (0x0054abf2 / 0x0054ac09 MOV [EBP],BL and
    // MOV [EDX],BL after the result is computed).
    struct FScopedFlagByte
    {
        explicit FScopedFlagByte(unsigned char* InFlag)
            : Flag(InFlag)
        {
            *Flag = 1;
        }

        ~FScopedFlagByte()
        {
            // .
            *Flag = 0;
        }

        FScopedFlagByte(FScopedFlagByte const&) = delete;
        FScopedFlagByte& operator=(FScopedFlagByte const&) = delete;

        unsigned char* Flag;
    };
}

bool InitSpaceState(SpaceApp* State, int Param)
{
    // slot 12 (+0x30), thiscall RET 4; the argument is never read.
    (void)Param;

    // +0x0b raised for the whole init (the routine reads it) and
    // dropped by the guard's destructor on every exit, the exception path included.
    FScopedFlagByte InitInProgress(&State->bInitInProgress);

    // the loading screen (the routine), the NN system (the routine(1)),
    // RefreshLoadingProgress.
    LoadingScreenStart();
    NnSystemInit(true);
    RefreshLoadingProgress();

    // +0x08 = 1 between the push of "IDR_SPACE" and the routine.
    State->bCameraReady = 1;
    RegisterAcceleratorGroup("IDR_SPACE");

    // IServer (the field, no test) slot 64 RequestCreateShip with
    // the connection the field.
    static_cast<IServer*>(GetLocalServerInterface())->RequestCreateShip(GetPlayerConnectionId());

    // the field / 94 cleared, then the routine(&the field,
    // 5.0f, 60.0f) -- the pushes are 60.0f (0x42700000), 5.0f (0x40a00000), the address.
    ClearPlayerCreateShipResponse();
    WaitForServerResponse(GetPlayerCreateShipAnsweredByte(), 5.0f, 60.0f);

    // no answer -- single player raises the field (leave the frame
    // loop), multiplayer the field (disconnected); next state 0, +0x0b dropped, return 1.
    if (*GetPlayerCreateShipAnsweredByte() == 0)
    {
        if (SinglePlayer())
            SetClientQuitRequested(true);
        else
            G_bServerDisconnected = true;
        G_SpaceNextState = 0;
        return true; // / 0x0054ac09: +0x0b = 0 (the guard's destructor, inlined)
    }

    // RefreshLoadingProgress, the routine, the routine,
    // RefreshLoadingProgress (LoadSpaceSceneSlice is exactly those three after the first).
    RefreshLoadingProgress();
    LoadSpaceSceneSlice();

    // the field is a pending background loader -- waited for,
    // deleted and cleared when set. Its XREF list is 0x0054ac21 (R), 0x0054ac3c (R) and
    // (W, the clear): nothing in the exe ever stores one, so the other branch is the
    // only one baseline takes.
    // lightanim.ini (the routine), the fuse DB (the routine), the gate
    // tunnels from [Data] gate_tunnels (the field, the routine), the routine(1).
    {
        char LightAnimPath[MAX_PATH];
        GetFileSysPath(LightAnimPath, "fx\\lightanim.ini", nullptr);
        LightAnimationLoad(LightAnimPath);
    }
    ClientFuseDB::LoadAll();
    {
        char GateTunnelPath[MAX_PATH];
        GetFileSysPath(GateTunnelPath, g_display.gate_tunnels.Text, nullptr);
        GateTunnelIniLoad(GateTunnelPath);
    }
    PreloadClientSoundArchesByCategory(1);

    // RefreshLoadingProgress, the routine(the routine()),
    // RefreshLoadingProgress.
    RefreshLoadingProgress();
    StarSysEnter(GetPlayerStarSystemIdbaseline());
    RefreshLoadingProgress();

    // the routine, the result dropped.
    SpawnPlayerShip();

    // RefreshLoadingProgress, IServer slot 11 (+0x2c) Update,
    // RefreshLoadingProgress.
    RefreshLoadingProgress();
    static_cast<IServer*>(GetLocalServerInterface())->Update();
    RefreshLoadingProgress();

    // the cockpit ini, the four cockpit camera loaders, the routine.
    InitPlayerCockpitSlice();

    // +0xbaa = 0, RefreshLoadingProgress, the routine(this, 0, 0), the
    // cockpit shake amplitude the field = 0, RefreshLoadingProgress.
    State->bDockingPending = 0;
    RefreshLoadingProgress();
    InstallSpaceCamera(nullptr, false);
    ResetCameraShakeAmount();
    RefreshLoadingProgress();

    // the routine (cameras.ini), RefreshLoadingProgress.
    SpaceAppLoadCamerasIni(State);
    RefreshLoadingProgress();

    // the routine(1) -- fade in from a covered screen.
    FadeIn(true);

    // / 0x0054ad9a.
    State->bPlayerLostHandled = 0;
    State->bLeaveRequested = 0;

    // the seven ids of a stack array registered on the Event::Handler
    // face at +4 (the routine), in this order.
    {
        const unsigned EventIds[7] = {0xc3, 0xc4, 0xf, 0x31, 0x39, 0x38, 0xbf};
        for (int Index = 0; Index < 7; ++Index)
            State->Events.RegisterEvent(EventIds[Index]);
    }

    // RefreshLoadingProgress, the routine (the system-arrival caption).
    RefreshLoadingProgress();
    NnArrivalTextShowSystemNow();

    // +0xba9 = 1; the routine(8) -- session flag 8 -> +0xba4 = 0 (first
    // person); otherwise the preference stays (the ctor's 1, or the last choice).
    State->bSpaceReady = 1;
    if (TestGameSessionFlags(8))
        State->PlayerPreference = 0;

    // no camera installed yet -> the routine(this, "player", 0).
    if (State->ActiveCamera == nullptr)
        SwitchSpaceCameraMode("player", nullptr);

    // the routine(&the field, 5.0f, 60.0f); no launch answer ->
    // (single player) or the field.
    WaitForServerResponse(GetPlayerLaunchAcknowledgedByte(), 5.0f, 60.0f);
    if (*GetPlayerLaunchAcknowledgedByte() == 0)
    {
        if (SinglePlayer())
            SetClientQuitRequested(true);
        else
            G_bServerDisconnected = true;
    }

    // next state 0, and outside single player (the next state being 0,
    // which it now always is) the routine(1) shows the chat window.
    G_SpaceNextState = 0;
    if (!SinglePlayer() && G_SpaceNextState == 0)
        ChatShowMsgWin(true);

    // RefreshLoadingProgress, the answer (next state == 0), then +0x0b = 0
    // (the guard's destructor, after the SETZ).
    RefreshLoadingProgress();
    return G_SpaceNextState == 0;
}

int UpdateSpaceState(SpaceApp* State, float DeltaSeconds)
{
    // slot 14 (+0x38) of the space state vftable .rdata 0x005e0554, thiscall,
    // RET 4. The whole in-space frame: simulation, camera, sound, then the render bracket
    // .. the routine / the routine with the scene drawn through the active
    // camera's slot 0x78. Returns the field, the next client state (0 = stay).

    // no active camera (+0xba0) -> 6, leave space.
    if (GetActiveSpaceCamera() == nullptr)
        return 6;

    // the simulation half, skipped while the routine holds the local
    // server.
    if (!ShouldSkipLocalServerTick())
    {
        // .
        G_SpaceNextState = 0;

        // CALL 0x00401e90 (Dt) -- the shared empty `RET` (ICF-folded
        // IDPMsgHandler::OnDisconnect). Nothing to do.

        // the routine(Dt) -- the per-bucket Gameobj update. Each ship's slot 93
        // (the routine, FClientShipGameobj::update) runs its CShip's CObject::update with
        // UPDATE_DEFAULT for the local player's objects, so the player CShip is ticked there,
        // not here (W3-A 2026-09-26: the "Update slice" CShip::update(Dt, 0) scaffold removed).
        FClientTickScope ObjectsProfile(EClientTickSection::Objects);
        ClientObjDb_UpdateAll(DeltaSeconds);
        ObjectsProfile.Stop();

        // .
        FClientTickScope AnimationProfile(EClientTickSection::WorldFx);
        AnimDB::Update(DeltaSeconds);
        AnimationProfile.Stop();
        // PUSH Dt, then the routine's system id.
        FClientTickScope PhysicsProfile(EClientTickSection::Physics);
        FClientPhysicsProfileScope PhysicsDetails;
        FClientTickScope StepProfile(EClientTickSection::PhysicsStep);
        PhySys::Update(StarSysGetActiveSystemId(), DeltaSeconds);
        StepProfile.Stop();

        // the routine(Dt) -- CBeam::move on every bucket-3 Gameobj's beam.
        FClientTickScope BeamsProfile(EClientTickSection::MoveBeams);
        ClientObjDb_MoveBeams(DeltaSeconds);
        BeamsProfile.Stop();
        // the routine() -- the same list through PhySys::GenerateCollisions, then
        // the PhySys collision list resolved into Gameobj hit pairs (the routine, slot 0x194).
        ClientObjDb_ProcessCollisions();
        PhysicsDetails.Stop();
        PhysicsProfile.Stop();

        // the routine(Dt, camera); the callee reads only Dt.
        FClientTickScope WorldProfile(EClientTickSection::WorldFx);
        TradeLaneDetachTick(DeltaSeconds);

        // LIGHTING (option 0x18) -> the routine(Dt), the routine.
        if (GetGameOption(kGameOptLighting) != 0)
        {
            GameLightTickAll(DeltaSeconds);
            LightGridRebuild();
        }

        // the routine.
        UpdateSpaceDocking(State);

        // the active camera's slot 0x6c (Dt), re-read and tested.
        FSpaceCamera* const Cam = GetActiveSpaceCamera();
        if (Cam != nullptr)
            ((FGameCamera*)Cam)->Update(DeltaSeconds);

        // the routine(Dt).
        StarSysUpdate(DeltaSeconds);
        // the routine(Dt).
        TickMaterialAnimationPlayback(DeltaSeconds);
        WorldProfile.Stop();
    }

    // the sound environment from the camera's slot 0x7c into
    // , then the mixer the routine(camera, Dt). No null test: +0xba0 was checked
    // on entry.
    {
        FClientTickScope SoundProfile(EClientTickSection::Sound);
        FSpaceCamera* const Cam = GetActiveSpaceCamera();
        SetClientAudioMode(
            ((FGameCamera*)Cam)->GetSoundMode());
        TickClientSoundMixer(GetActiveSpaceCamera(), DeltaSeconds);
    }

    // the routine, kept in a stack byte for the present below.
    const bool bSceneBegun = BeginClientRenderFrame();
    if (bSceneBegun)
    {
        // .
        Win3DImmedBatchReset();
        ButtonMontageEnsure();
        UpdateMouseFlightDirection();
        // the routine -- with the field (the Targetable HUD) and the letterbox
        // up (the routine), clear the low two bits of +0x6c on its four alert children.
        HudTargetableObjectsHideAlertsInLetterbox();

        // the caller-side test of the active camera, then the routine
        // (letterbox, clear, viewport, Win3D camera) and the camera's slot 0x78 (0xffff, 0).
        FSpaceCamera* const Cam = GetActiveSpaceCamera();
        if (Cam != nullptr)
        {
            GameCameraBeginFrame(reinterpret_cast<FGameCamera*>(Cam));
            FSpaceCamera* const DrawCam = GetActiveSpaceCamera();
            ((FGameCamera*)DrawCam)->DrawScene(0xffffu, 0u);
        }

        // .
        Win3DImmedBatchFlushAll();
        ButtonMontageFlushBatch();
        UiOverlayPrepPipeline();
        Win3DDrawAll();               // (NN_Pause is one of its roots)

        // with Dt > 0 (FCOMP 0.0f / TEST AH,0x41 / JNZ) and the server
        // tick not paused (the routine), the fade: the routine(Dt), the routine(camera).
        if (DeltaSeconds > 0.0f && !IsClientServerTickPaused())
        {
            FSpaceCamera* const FadeCam = GetActiveSpaceCamera();
            UpdateFade(DeltaSeconds);
            DrawFade(AsCamera(FadeCam));
        }

        // the routine.
        EndClientRenderScene();
    }

    // the routine() -- the routine on each Gameobj bucket (the field ..
    // ): every Gameobj whose slot 0x158 reports it done is unregistered and deleted.
    ClientObjDb_Reap();
    // unless the routine, the routine(Dt) -- multiplayer only
    // (returns at once under SinglePlayer), walks the field.
    if (!ShouldSkipLocalServerTick())
        ClientObjDb_UpdateRemotePhysics(DeltaSeconds);
    // the routine(this) -- a pending system switch (the field) or
    // landing (the field).
    ProcessPendingSystemSwitch();

    // the player's last position (slot 0) and orientation (slot 0xc)
    // into the field / the field (the routine / the routine).
    {
        IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW());
        if (Player != nullptr)
        {
            SetPlayerLastPositionbaseline(&Player->get_position().x);
            IObjRW* const Again = static_cast<IObjRW*>(GetPlayerObjRW());
            SetPlayerLastOrientationbaseline(&Again->get_transform().Rotation.E[0][0]);
        }
    }

    // when the active camera reports it is finished (slot 0x60), either
    // the player ship is gone -- the routine once, latched in +0x09 -- or the camera returns to
    // "player", attached to the player object, and +0xba8 drops.
    {
        FSpaceCamera* const Cam = GetActiveSpaceCamera();
        if (Cam != nullptr &&
            ((FGameCamera*)Cam)->IsFinished())
        {
            void* const Player = GetPlayerObjRW();
            if (Player == nullptr)
            {
                if (State->bPlayerLostHandled == 0)
                {
                    NnExitPostShipLostMessage(); // the routine
                    State->bPlayerLostHandled = 1;
                }
            }
            else
            {
                SwitchSpaceCameraMode("player", Player);
                State->bScriptCameraLatch = 0;
            }
        }
    }

    // .
    if (State->bLeaveRequested != 0)
        G_SpaceNextState = 6;

    // the routine, only when the routine began the scene.
    if (bSceneBegun)
        PresentClientRenderFrame();

    // the routine -- the loading screen's (the field) timer reset:
    // Timing::read_ticks into the field/cc, the field = the field = 0,
    // = 1.
    LoadingScreenReset();

    // .
    return G_SpaceNextState;
}

void DestroySpaceState(SpaceApp* State)
{
    // slot 13 (+0x34) of the space state vftable .rdata 0x005e0554, thiscall on
    // the in-flight controller the field. No null test on `this`: the client state driver
    // only calls slot 13 on a live state.

    // the routine("IDR_SPACE") -- drop the space accelerator group the init
    // registered (0x0054ab92).
    UnregisterAcceleratorGroup("IDR_SPACE");

    // the routine on controller + 0x04, the Event::Handler face -- drop every
    // registration of this handler (the init's seven, 0x0054addc).
    State->Events.UnregisterAllEvents();

    // the routine on the field (engine-instance table: the routine on each
    // +0x400 slot, IEngine slot 0x3c on each instance, then every slot and the count reset).
    ResetEngineInstanceCache();

    // the routine -- every character script's reference count back to 0 and a
    // fresh IAnimation2 script for the gf\CharacterInfo.cpp database (character_info.cpp).
    ResetCharacterScripts();

    // the routine(this, 0, 0) -- deactivate the active camera.
    InstallSpaceCamera(nullptr, false);

    // the routine on + 0x00c, OnDisconnect (empty) on + 0x1b0 /
    // + 0x3c8 / + 0x500.
    ReleaseSpaceCameraControllerParts();

    // the routine.
    HudElementListShutdown();

    // the four state bytes and the field. +0xbaa is tested and only
    // then cleared (the compare is on the value before the other two stores).
    State->bSpaceReady = 0;        // +0xba9
    State->bScriptCameraLatch = 0; // +0xba8
    if (State->bDockingPending != 0)
        State->bDockingPending = 0; // +0xbaa
    G_bNnScreenUp = false;
    State->bDockingActive = 0;  // +0xbab

    // the player's ship through the watcher the field (minus eight),
    // its CEqObj at ObjRW + 0x10 (the +0xc'd pointer's +0x4, with the -0xc sentinel), the ship
    // class mask 0x503 at CObject + 0x4c; when control exclusion 7 is set it is cleared (0).
    void* const ObjRW = GetPlayerObjRW();
    if (ObjRW != nullptr && ObjRW != reinterpret_cast<void*>(-0xc))
    {
        CEqObj* const EqObj =
            *reinterpret_cast<CEqObj**>(static_cast<unsigned char*>(ObjRW) + 0x10);
        if (EqObj != nullptr &&
            (static_cast<unsigned>(EqObj->ObjectClass) & 0x503u) == 0x503u &&
            EqObj->is_control_excluded(7u))
        {
            EqObj->set_control_exclusion(0u);
        }
    }

    // controller + 0x08 (the field) = 0.
    State->bCameraReady = 0;

    // / 0x0054af90: the routine(0).
    StarSysLeave(nullptr);

    // the routine("Arrow", 1).
    MouseSetCursor("Arrow", true);

    // CObject::ReleaseAll.
    CObject::ReleaseAll();

    // the routine (Heap::Shrink on the field / the field, then free() every
    // queued ZItem of the the field.. vectors and empty them).
    SpaceDrawQueueRelease();

    // the routine.
    ClientFuseDB::Clear();

    // the routine (unlink and free every node of the the field list, then free
    // ).
    JumpEffectDescsClear();

    // the routine.
    ClearSolarStarArchShapes();

    // the routine (virtual-delete every entry of the the field vector, then
    // end = begin).
    GateTunnelsClear();

    // the routine.
    EffectTypeListKillAll();

    // the routine (reset the light-grid root the field..9c and rethread the two
    // node pools the field / the field).
    LightGridReset();
    // the routine (drop one reference on every GameLight of the field,
    // virtual-delete at zero).
    GameLightReleaseAll();

    // the routine; 0x0054afd4: the routine.
    GameLightHeapShrink();
    LightAnimationFreeAll();

    // the routine -- every sound stopped, the live-source leak warning.
    StopAllClientSounds();
    // the routine(1) (walk the the field sound list by the category mask).
    UnloadClientSoundArchesByCategory(1);

    // AnimDB::Shutdown.
    AnimDB::Shutdown();

    // JMP the routine -- the NN system teardown.
    NnSystemShutdown();
}

void ShutdownSpaceState(SpaceApp* State)
{
    // (slot 9, +0x24), thiscall, no arguments.
    // the routine -- the routine(0), leave the system without a player.
    StarSysLeaveWithoutPlayer();
    // this->vftable[13] (+0x34) = the routine. baseline calls it even
    // when the atexit teardown already ran slot 13 on the current state (0x005b21ac).
    State->Destroy();
    // CObject::ReleaseAll.
    CObject::ReleaseAll();
    // JMP the routine -- the ZItem heaps and draw queues released.
    SpaceDrawQueueRelease();
}
