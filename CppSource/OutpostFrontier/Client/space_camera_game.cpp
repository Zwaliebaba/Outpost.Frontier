// Author: Giperion.

// FGameCamera (the routine) and FHudCamera (the routine): the two intermediate camera
// classes. See space_camera_game.h for how the hierarchy was recovered.

// Ported here: both constructors, both name slots, and the four GameCamera slots that are pure
// field work -- get_velocity (0x48), set_property (0x4c), attach (0x58) and has-no-target
// (0x60). Everything else in the layer touches things the port does not have yet; the header
// lists those slots with their baseline addresses.

// 2026-09-25: GameCamera's 0x60 is the routine (always false), 0x68 the empty OnDisconnect and
// 0x70/0x74 the branch (return 0); HUDCamera adds 0x58 (the routine), 0x5c (the routine),
// 0x60 (0x005172f0), 0x70 (the routine) and 0x74 (the branch). Later the same day: GameCamera
// 0x6c (the routine), HUDCamera 0x64 (the routine), 0x68 (the routine), 0x78 (the routine)
// and the HUDCamera Event::Handler face (the routine), then HUDCamera 0x6c (the routine).

#include "space_camera_game.h"

#include "character_select_equip.h" // Get/SetPlayerVisitFlagsbaseline, GetPlayerStarSystemIdbaseline
#include "cli_projectile_fx.h"  // FCliEquipFirePacket, CliShipHandleEquipUpdate (the routine)
#include "client_gameobj.h"     // FClientGameobj::ManagerKind (+0x1c)
#include "client_objdb.h"       // GetClientObjDb, ClientObjDb_GetKindHead (the routine)
#include "common_misc.h"        // Geometry::Sphere
#include "common_reputation.h"  // Reputation::Vibe::GetAffiliation
#include "common_universe.h"    // Universe::get_system, ISpatialPartition, SpatialFill
#include "cockpit_load.h"       // SetCockpitResourceStringLoading (the routine)
#include "common_cequip.h"      // CEGun, CEquipTraverser
#include "common_cobject.h"     // CEqObj
#include "common_iface.h"       // IObjRW
#include "game_options.h"
#include "globals.h"            // GetClientFrameTimeCapped (the routine)
#include "hud_command_dispatch.h"
#include "hud_element_list.h"
#include "hud_target.h"         // HudIsMousePickBlocked (the routine)
#include "hud_targetable_objects.h" // G_HudTargetableObjects (the field), the routine
#include "input_manager.h"      // MouseIsArrowCursor (the routine)
#include "jump_effect.h"        // FJumpShipEffect::ShakeAmplitude (the routine)
#include "menu_keymap_config.h" // (Un)RegisterAcceleratorGroup, KeyMapIsCommandHeld (the routine)
#include "nn_modal.h"          // G_bNnScreenUp (the field)
#include "player_behavior.h"    // ToggleMouseFlightLock, SetPlayerBehaviorDirectionGated
#include "player_spawn.h"       // GetPlayerConnectionId (the field)
#include "space_letterbox.h"
#include "space_scene_obj_draw.h" // SpaceCameraRenderSlot78 (the routine)
#include "win3d_immediate.h"    // Win3DSetViewport (the routine), Win3DSetPipelineClearColor
#include "win3d.h"          // Win3DBroadcast (the routine)
#include "win3d_render.h"       // Win3DApplyCamera (the routine)

#include <cstddef>
#include <math.h>
#include <new>
#include <string.h>
#include <windows.h>            // GetKeyState

#ifdef _MSC_VER
#include <stdlib.h>          // _stricmp
#else
#define _stricmp strcasecmp
#endif

const float kGameCameraDefaultHalfFovXDeg = 35.0f;   // the literal 0x420c0000

namespace
{
    // baseline keeps the object as its Watchable subobject, which sits 8 bytes into IObjRW.
    // Both directions are spelled out inline at every use site rather than in a helper.
    const int kWatchableOffset = 8;

    // .rdata 0x005dbfe8, the source path the routine / the routine hand the routine.
    const char kGamecamCpp[] = "Client\\space\\Gamecam.cpp";

    // The six keyboard steering halves the routine writes (1.0 / -1.0 while the key is down,
    // 0.0 otherwise) and the routine sums into one steering vector.
    float G_HudKeySteerPosX = 0.0f; // , event 0x2a
    float G_HudKeySteerNegX = 0.0f; // , event 0x2b
    float G_HudKeySteerPosY = 0.0f; // , event 0x2c
    float G_HudKeySteerNegY = 0.0f; // , event 0x2d
    float G_HudKeySteerPosZ = 0.0f; // , event 0x2e
    float G_HudKeySteerNegZ = 0.0f; // , event 0x2f
    // Event 0xc7: the fire key's down state (the field) and the "just changed" latch
    // consumes (the field).
    unsigned char G_bHudFireHeld = 0;
    int G_HudFireChanged = 0;

    // 's keyboard-steering ramp (the field): grows by G_HudSteerRampRate per second
    // while a steering key is held, capped at 1, dropped to 0 when the keys change or let go.
    float G_HudSteerRamp = 0.0f;
    // .data (not .rdata) 0x00613740 / 0x00613744, both 2.0f and each read only by the routine:
    // the ramp rate per second and the exponent of the shift-steer falloff.
    float G_HudSteerRampRate = 2.0f;
    float G_HudShiftSteerExponent = 2.0f;

    // the cockpit shake amplitude (radians) the routine decays and raises and
    // jitters the view with. the field (.data, read only there) is the decay per
    // update.
    float G_CameraShakeAmount = 0.0f;
    float G_CameraShakeDecay = 0.001f;


}

void GameCameraConstruct(FGameCamera* Cam)
{
    // , in baseline's order.
    SpaceCameraConstruct((FSpaceCamera*)Cam);

    // The BaseWatcher member was constructed with the owning camera.

    // baseline installs vtable 0x005dbf5c here; a derived ctor overwrites it immediately after.

    Cam->FieldF0 = 0;                        // +0xf0
    Cam->FieldEC = 0;                        // +0xec

    // baseline re-tests the watcher it has just zeroed and would call
    // BaseWatcher::set_pointer(0) if it were non-null. It never is; reproduced for shape only.
    if (Cam->Watched.target() != nullptr)
        Cam->Watched.set(nullptr);

    Cam->Flags = static_cast<unsigned char>(Cam->Flags & 0xfc);

    Cam->bFlagF4 = 0;
    Cam->bFlagF5 = 0;
    Cam->bFlagF6 = 0;

    // the resting half-FOV is cached BEFORE it is applied, and the apply goes
    // through the base's non-virtual setter rather than through the vtable.
    Cam->BaseHalfFovXDeg = kGameCameraDefaultHalfFovXDeg;
    SpaceCameraSetHalfFovX((FSpaceCamera*)Cam, kGameCameraDefaultHalfFovXDeg);

    // the two vectors are cleared last, and downwards -- +0xe4 first, +0xd0 last.
    for (int i = 0; i < 3; ++i)
    {
        Cam->Velocity[i] = 0.0f;
        Cam->VectorD0[i] = 0.0f;
    }

    Cam->Pad0CC[0] = 0;
    Cam->Pad0CC[1] = 0;
    Cam->Pad0CC[2] = 0;
    Cam->Pad0CC[3] = 0;
    Cam->ClearColor[0] = 0;
    Cam->ClearColor[1] = 0;
    Cam->ClearColor[2] = 0;
    Cam->Pad0F7 = 0;
}

void HudCameraConstruct(FHudCamera* Cam)
{
    // .
    GameCameraConstruct((FGameCamera*)Cam);

    // the secondary base's own (abstract) table goes in first, then the two flags,
    // then HUDCamera's primary and secondary tables replace it. The abstract table at
    // holds a single _purecall thunk, so the intermediate value is never callable --
    // it is just what the secondary base's ctor left behind.
    Cam->bFlagFC = 0;        // +0xfc (0x0051ca92)
    Cam->bFlagFE = 1;        // +0xfe (0x0051ca99)
    // +0xfd and +0xff are not written here (the FirstPerson ctor clears +0xfd itself); every
    // HUDCamera lives in G_SpaceApp's zeroed static storage.

    // baseline then swaps the primary vptr to 0x005dc08c (HUDCamera) and the face's to
    // (0x0051caa6).
}

const char* GameCameraGetName()
{
    return "GameCamera";     // -> .rdata 0x005dbfdc
}

const char* HudCameraGetName()
{
    return "HUDCamera";      // -> .rdata 0x005dc10c
}

void GameCameraUpdate(FGameCamera* Cam, float DeltaSeconds)
{
    // (slot 0x6c, thiscall, RET 4). 0x0051b656: the position through the
    // camera's own slot 0x0c (the base's the branch hands back WorldPos).
    FSpaceCamera* const Space = SpaceCameraOf(Cam);
    const float* const Position =
        Space->GetPosition();

    // FCOMP dt against 0.0 / TEST AH,0x44 / JNP -- only an exact
    // zero skips (a NaN does not). Velocity = (Position - last position) * (1 / dt), the
    // reciprocal taken once.
    if (!(DeltaSeconds == 0.0f))
    {
        const float DiffZ = Position[2] - Cam->VectorD0[2];
        const float DiffX = Position[0] - Cam->VectorD0[0];
        const float DiffY = Position[1] - Cam->VectorD0[1];
        const float InvDt = 1.0f / DeltaSeconds;
        Cam->Velocity[0] = InvDt * DiffX;
        Cam->Velocity[1] = InvDt * DiffY;
        Cam->Velocity[2] = InvDt * DiffZ;
    }

    // the position becomes the last position.
    Cam->VectorD0[0] = Position[0];
    Cam->VectorD0[1] = Position[1];
    Cam->VectorD0[2] = Position[2];
}

void GameCameraBeginFrame(FGameCamera* Cam)
{
    // (thiscall, RET 0). 0x0051b4b3: the capped frame time, stored as a float for
    // (this, dt) -- the letterbox advances and, when it moved, re-cuts this camera's
    // viewport.
    FSpaceCamera* const Space = SpaceCameraOf(Cam);
    const float DeltaSeconds = static_cast<float>(GetClientFrameTimeCapped());
    UpdateLetterbox(Space, DeltaSeconds);

    // either clear bit set -> the routine.
    if ((Cam->Flags & 3) != 0)
        GameCameraClear(Cam);

    // the routine(this, "Gamecam.cpp", 0x85), the routine(this).
    Win3DSetViewport(AsCamera(Space), kGamecamCpp, 0x85);
    Win3DApplyCamera(AsCamera(Space));
}

void GameCameraClear(FGameCamera* Cam)
{
    // (thiscall, RET 0).
    unsigned ClearBits = 0;          // EDI
    bool bBarsBlackened = false;     // BL
    float Red = 0.0f;
    float Green = 0.0f;
    float Blue = 0.0f;

    if ((Cam->Flags & 1) != 0)
    {
        // the colour bytes scaled by 1/255 (double 0x005c89c8).
        Red = static_cast<float>(Cam->ClearColor[0] * (1.0 / 255.0));
        Green = static_cast<float>(Cam->ClearColor[1] * (1.0 / 255.0));
        Blue = static_cast<float>(Cam->ClearColor[2] * (1.0 / 255.0));

        // with the letterbox showing and any colour byte non-zero, the
        // whole surface is cleared black first (0x0051b631) and the colour goes into the
        // camera's own viewport afterwards; otherwise the colour is the clear colour straight
        // away.
        if (IsLetterboxVisible() &&
            (Cam->ClearColor[0] != 0 || Cam->ClearColor[1] != 0 || Cam->ClearColor[2] != 0))
        {
            bBarsBlackened = true;
            Win3DSetPipelineClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        }
        else
        {
            Win3DSetPipelineClearColor(Red, Green, Blue, 1.0f);
        }
        ClearBits = kWin3DClearColour;
    }

    // bit 1 adds the depth buffer.
    if ((Cam->Flags & 2) != 0)
        ClearBits = ClearBits | kWin3DClearDepth;

    if (ClearBits == 0)
        return;

    // the whole surface (the routine(0, ..., 0xb7)), then the clear.
    Win3DSetViewport(nullptr, kGamecamCpp, 0xb7);
    ClearWin3DBuffers(ClearBits);

    // after a black full-surface clear, the camera's own viewport is
    // cleared to the real colour.
    if ((Cam->Flags & 1) != 0 && bBarsBlackened)
    {
        Win3DSetViewport(AsCamera(SpaceCameraOf(Cam)), kGamecamCpp, 0xc0);
        Win3DSetPipelineClearColor(Red, Green, Blue, 1.0f);
        ClearWin3DBuffers(kWin3DClearColour);
    }
}

void GameCameraGetVelocity(const FGameCamera* Cam, float* OutVelocity)
{
    // . Three dwords out of +0xdc, replacing the base's zero-vector stub at
    // . This is why a camera bound to a moving ship can drive doppler and motion
    // blur while a free camera reports standing still.
    OutVelocity[0] = Cam->Velocity[0];
    OutVelocity[1] = Cam->Velocity[1];
    OutVelocity[2] = Cam->Velocity[2];
}

bool GameCameraSetProperty(FGameCamera* Cam, const char* Key, const char* Value)
{
    // . The base runs first and decides whether the key is known at all -- a direct
    // CALL the routine (0x0051b3fe), not slot 0x4c, which is this very function.
    if (SpaceCameraSetProperty((FSpaceCamera*)Cam, Key, Value) == 0)
        return false;

    // only the two fov keys re-cache the resting angle. baseline reads it back
    // through slot 0x18 rather than off the field, so an override of the getter would be
    // honoured -- and none of the seven concrete classes overrides it.
    if (_stricmp(Key, "fovx") == 0 || _stricmp(Key, "fovy") == 0)
    {
        Cam->BaseHalfFovXDeg = AsCamera((FSpaceCamera*)Cam)->GetHalfFovXDeg();
    }

    return true;
}

bool GameCameraAttach(FGameCamera* Cam, void* Object)
{
    // . Null unbinds; anything else is stored as its Watchable subobject.
    // baseline then tail-jumps BaseWatcher::set_pointer (IAT); public set() is the same body
    // after the same CMP-skip that 0x0051b3d5 already performed.
    Watchable const* const Target =
        (Object != nullptr)
            ? reinterpret_cast<Watchable const*>(static_cast<unsigned char*>(Object) +
                                                 kWatchableOffset)
            : nullptr;

    // re-pointing the watcher to what it already holds is skipped outright, so
    // re-attaching to the same object costs nothing and does not disturb the watcher chain.
    if (Target == Cam->Watched.target())
        return false;

    Cam->Watched.set(Target);
    return true;
}

bool GameCameraHasNoTarget(const FGameCamera* Cam)
{
    // . baseline loads the watcher, and when it is null jumps to a block that zeroes
    // EAX and tests THAT -- so both a null watcher and a watcher holding exactly 8 (an IObjRW
    // at address 0) report "empty". Reproduced rather than simplified because the second case
    // is reachable in principle.
    const unsigned char* const Raw =
        reinterpret_cast<const unsigned char*>(Cam->Watched.target());
    if (Raw == nullptr)
        return true;
    return (Raw - kWatchableOffset) == nullptr;
}

void* GameCameraGetTarget(const FGameCamera* Cam)
{
    if (Cam->Watched.target() == nullptr)
        return nullptr;
    return const_cast<unsigned char*>(
        reinterpret_cast<const unsigned char*>(Cam->Watched.target())) - kWatchableOffset;
}

bool GameCameraActivate(FGameCamera* Cam, const int* ViewRect4)
{
    // . `this` in ECX, one stack pointer, RET 4.
    const bool bApplied = SpaceCameraSetViewport((FSpaceCamera*)Cam, ViewRect4);

    // the slot 0x5c call is guarded on that result, which is always
    // true -- see the header. It is VIRTUAL (`MOV EAX,[ESI] / CALL [EAX+0x5c]`): a HUDCamera
    // (whose activate the routine ends with this call at 0x0051cb59) lands in the routine,
    // which also hands the rectangle to every HUD element.
    if (bApplied)
        Cam->ApplyViewport(ViewRect4);

    return bApplied;
}

void GameCameraApplyViewport(FGameCamera* Cam, const int* ViewRect4)
{
    // the two-argument viewport setter with the dead flag zero, then the letterbox
    // is told to forget which camera it last drew on -- because this reset has just wiped the
    // bars out of the viewport it was drawn into.
    SpaceCameraSetViewportEx((FSpaceCamera*)Cam, ViewRect4, 0);
    ForgetLetterboxCamera();
}

void HudCameraAttach(FHudCamera* Cam, void* Object)
{
    // . The same watcher swap as GameCamera's 0x0051b3c0, inlined, with the same
    // skip when the watcher already holds that Watchable.
    BaseWatcher& Watched = Cam->Watched;
    Watchable const* const Target =
        (Object != nullptr)
            ? reinterpret_cast<Watchable const*>(static_cast<unsigned char*>(Object) +
                                                 kWatchableOffset)
            : nullptr;
    if (Target != Watched.target())
        Watched.set(Target);

    // the HUD is then retargeted from what the watcher now holds -- the IObjRW when
    // it is a client ship (Gameobj +0x1c ManagerKind == 1), otherwise null. `watched - 8 == 0`
    // also reads as null, exactly as at 0x0051cbdd.
    IObjRW* HudTarget = nullptr;
    const unsigned char* const Held = reinterpret_cast<const unsigned char*>(Watched.target());
    if (Held != nullptr)
    {
        IObjRW* const Object8 =
            reinterpret_cast<IObjRW*>(const_cast<unsigned char*>(Held - kWatchableOffset));
        if (Object8 != nullptr && static_cast<FClientGameobj*>(Object8)->ManagerKind == 1)
            HudTarget = Object8;
    }
    HudElementListSetTarget(HudTarget);   // , the routine
}

void HudCameraApplyViewport(FHudCamera* Cam, const int* ViewRect4)
{
    // GameCamera's the routine first, then every HUD element gets the same
    // rectangle (the routine).
    GameCameraApplyViewport((FGameCamera*)Cam, ViewRect4);
    HudElementListSetViewport(ViewRect4);
}

int HudCameraCommand70(void* Command)
{
    // (and the branch, which tail-jumps here). While a NeuroNet window holds the
    // screen (the field) nothing is offered to the HUD; otherwise 1 when some element's
    // Slot7 took the command (the routine).
    if (!G_bNnScreenUp && HudElementListAnySlot7(Command))
        return 1;
    return 0;
}

int HudCameraCommand74(void* Command)
{
    // the same gate, then Slot8 (the routine), its bool zero-extended.
    if (G_bNnScreenUp)
        return 0;
    return HudElementListAnySlot8(Command) ? 1 : 0;
}

bool HudCameraActivate(FHudCamera* Cam, const int* ViewRect4)
{
    // . 0x0051cad6: the routine(1).
    SetCockpitResourceStringLoading(true);
    // the routine("IDR_COCKPIT").
    RegisterAcceleratorGroup("IDR_COCKPIT");

    // the nine ids from a stack table, in this order, each through
    // on the face at +0xf8.
    static const unsigned kCockpitEvents[9] = {0x5c, 0x2a, 0x2b, 0x2c, 0x2d,
                                               0x2e, 0x2f, 0xc7, 0xc8};
    FHudCmdHandler* const Face = Cam->AsHudCmdHandler();
    for (unsigned Index = 0; Index < 9; ++Index)
        HudRegisterCommandHandler(Face, kCockpitEvents[Index]);

    // the routine, its answer returned.
    return GameCameraActivate((FGameCamera*)Cam, ViewRect4);
}

void HudCameraDeactivate(FHudCamera* Cam)
{
    // . 0x0051cb78: the routine("IDR_COCKPIT"); 0x0051cb7f: the routine(0);
    // JMP IDPMsgHandler::OnDisconnect (a bare RET).
    (void)Cam;
    UnregisterAcceleratorGroup("IDR_COCKPIT");
    SetCockpitResourceStringLoading(false);
}

void HudCameraDerivedDeactivate(FHudCamera* Cam)
{
    // (thiscall, RET 0). 0x0051b0b3: the routine on the face at +0xf8 -- every
    // command registration of it goes; 0x0051b0c1: JMP the routine.
    HudUnregisterCommandHandler(Cam->AsHudCmdHandler());
    HudCameraDeactivate(Cam);
}

void HudCameraDraw(FGameCamera* Cam)
{
    // . 0x0051daa4..0x0051dab3: a watched object (and not the degenerate 8).
    const unsigned char* const Held =
        reinterpret_cast<const unsigned char*>(Cam->Watched.target());
    if (Held == nullptr || Held - kWatchableOffset == nullptr)
        return;

    // the routine(6) [INTERFACE] and the routine(10) HUD.
    if (GetGameOption(kGameOptInterfaceGroup) == 0)
        return;
    if (GetGameOption(kGameOptHud) == 0)
        return;

    // the routine(this); 0x0051dad7: the routine (FSTP to float);
    // the routine(this, dt).
    FSpaceCamera* const Space = SpaceCameraOf(Cam);
    Win3DApplyCamera(AsCamera(Space));
    const float DeltaSeconds = static_cast<float>(GetClientFrameTimeCapped());
    HudElementListRender(Space, DeltaSeconds);
}

bool HudCameraOnEvent(FHudCamera* Cam, unsigned MsgId)
{
    // (thiscall on the face, RET 4). 0x0051d7f6: the routine -> false.
    if (HudIsMousePickBlocked())
        return false;

    // a zeroed 0x28-byte record on the stack, byte +0x26 = 1. Only
    // event 0x5c stores its id into +0x00 -- which is what the send test below reads.
    FCliEquipFirePacket Record;
    memset(&Record, 0, sizeof(Record));
    Record.Pad24[2] = 1;

    switch (MsgId)
    {
    case 0x5c:
        // .
        Record.Op = static_cast<int>(MsgId);
        break;
    case 0x2a:
        // SETNZ -> FILD -> +1.0 / 0.0.
        G_HudKeySteerPosX = (HudCommandRecord()->Byte18 != 0) ? 1.0f : 0.0f;
        break;
    case 0x2b:
        // NEG / SBB -> FILD -> -1.0 / 0.0.
        G_HudKeySteerNegX = (HudCommandRecord()->Byte18 != 0) ? -1.0f : 0.0f;
        break;
    case 0x2c:
        G_HudKeySteerPosY = (HudCommandRecord()->Byte18 != 0) ? 1.0f : 0.0f;
        break;
    case 0x2d:
        G_HudKeySteerNegY = (HudCommandRecord()->Byte18 != 0) ? -1.0f : 0.0f;
        break;
    case 0x2e:
        G_HudKeySteerPosZ = (HudCommandRecord()->Byte18 != 0) ? 1.0f : 0.0f;
        break;
    case 0x2f:
        G_HudKeySteerNegZ = (HudCommandRecord()->Byte18 != 0) ? -1.0f : 0.0f;
        break;
    case 0xc7:
        // the routine (the arrow cursor) -> nothing.
        if (!MouseIsArrowCursor())
        {
            G_bHudFireHeld = (HudCommandRecord()->Byte18 != 0) ? 1 : 0;
            G_HudFireChanged = 1;
        }
        break;
    case 0xc8:
        // the routine.
        ToggleMouseFlightLock();
        break;
    default:
        // caseD_30: every other id in 0x2a..0xc8 and everything outside it.
        return false;
    }

    // nothing to send unless +0x00 was stored (event 0x5c only).
    if (Record.Op == 0)
        return false;

    // the watched client object when it is a ship (+0x1c == 1), else
    // null -- which baseline then dereferences, as the port does.
    FClientGameobj* Ship = nullptr;
    const unsigned char* const Held =
        reinterpret_cast<const unsigned char*>(Cam->Watched.target());
    if (Held != nullptr)
    {
        FClientGameobj* const Object = reinterpret_cast<FClientGameobj*>(
            const_cast<unsigned char*>(Held - kWatchableOffset));
        if (Object != nullptr && Object->ManagerKind == 1)
            Ship = Object;
    }

    // the object's slot 0x1cc with the record. For a ship that slot
    // is the routine in the one vtable that holds it (0x005dfd6c + 0x1cc = 0x005dff38, the
    // client Ship class the routine builds); the port's client objects do not carry baseline's
    // numbering there, so it is called directly -- the same devirtualisation cli_projectile_fx
    // uses for the ship's +0x34 fire slot.
    CliShipHandleEquipUpdate((FClientEquippedObj*)Ship, &Record);
    return true;
}

namespace
{
    // the routine's clock: frame time summed, one second taken off per scan.
    float G_HudVisitScanClock = 0.0f;

    // .data 0x00612434 -- the radar range assumed when the player ship has no scanner (read
    // only; 0x451c4000).
    const float kDefaultRadarRange = 2500.0f;

    // 's constants: .rdata 0x005d8850 (solar radius factor), 0x005ca218 (the
    // smallest solar visit range), 0x005db0b4 (the range for a solar whose visit byte carries
    // 0x10), 0x469c4000 and 0x45ea6000 (the two query radii).
    const float kSolarVisitRadiusScale = 7.0f;
    const float kSolarVisitMinRange = 10000.0f;
    const float kSolarVisitFlag10Range = 7000.0f;
    const float kSolarQueryRadius = 20000.0f;
    const float kZoneQueryRadius = 7500.0f;

    // (cdecl, no arguments; HUDCamera's update calls it at 0x0051d2f1).
    void HudCameraScanVisits()
    {
        // not while the letterbox bars are up (the routine).
        if (IsLetterboxVisible())
            return;

        // the routine + the field, stored with FST and compared
        // unrounded against 1.0f; TEST AH,5 / JNP leaves only on "below" (a NaN scans). Then one
        // second comes off the stored float.
        const double Clock = GetClientFrameTimeCapped() + G_HudVisitScanClock;
        G_HudVisitScanClock = static_cast<float>(Clock);
        if (Clock < 1.0f)
            return;
        G_HudVisitScanClock = static_cast<float>(static_cast<double>(G_HudVisitScanClock) - 1.0f);

        // the player's IObjRW (the routine); none, no scan.
        IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW());
        if (Player == nullptr)
            return;

        // its position (slot 0), kept for all three passes.
        const Vector PlayerPos = Player->get_position();

        // pass 1 -- the object DB's spatial bucket (slot 0xc, mask 0xfdff;
        // the result is used unchecked) filled with everything within 20000 of the player.
        {
            ISpatialPartition* const Bucket =
                static_cast<ISpatialPartition*>(GetClientObjDb()->GetBucket(0xfdff));
            Geometry::Sphere Query;
            Query.Center = PlayerPos;
            Query.Radius = kSolarQueryRadius;
            FSpatialQueryResult Result;   // baseline: 0x2008 bytes of its 0x4040 stack frame
            Result.Capacity = 0x800;
            Result.Count = 0;
            if (SpatialFill(Bucket, &Query, &Result) != 0)
            {
                for (unsigned Index = 0; Index < Result.Count; ++Index)
                {
                    // the entry's CObject (slot 0x150) when it is a
                    // solar (mask 0x303), else null -- and GetVisitValue is called either way.
                    FClientGameobj* const Entry = static_cast<FClientGameobj*>(Result.Items[Index]);
                    CObject const* const Obj = Entry->cobject();
                    CSolar const* Solar = nullptr;
                    if (Obj != nullptr &&
                        (static_cast<unsigned>(Obj->ObjectClass) & 0x303u) == 0x303u)
                        Solar = static_cast<CSolar const*>(Obj);
                    const unsigned char Visit = Solar->GetVisitValue();

                    // already visited (bit 0) or never to be (bit 7) -- skip.
                    if ((Visit & 0x81u) != 0)
                        continue;

                    // solar minus player, each component stored as a
                    // float (the routine); then |d| (y*y + z*z first) minus the radius, kept.
                    const float Dx = static_cast<float>(static_cast<double>(Solar->WorldPosition.x) - PlayerPos.x);
                    const float Dy = static_cast<float>(static_cast<double>(Solar->WorldPosition.y) - PlayerPos.y);
                    const float Dz = static_cast<float>(static_cast<double>(Solar->WorldPosition.z) - PlayerPos.z);
                    double SumSq = static_cast<double>(Dy) * Dy + static_cast<double>(Dz) * Dz;
                    SumSq = SumSq + static_cast<double>(Dx) * Dx;
                    const float Radius = Solar->Radius;
                    const double Gap = sqrt(SumSq) - Radius;

                    // the visit range -- radius * 7, raised to 10000 when
                    // strictly below it (TEST AH,5 / JP), and 7000 outright for bit 0x10.
                    double Range = static_cast<double>(Radius) * kSolarVisitRadiusScale;
                    if (Range < kSolarVisitMinRange)
                        Range = kSolarVisitMinRange;
                    if ((Visit & 0x10u) != 0)
                        Range = kSolarVisitFlag10Range;

                    // strictly inside -> the object id (CSimple +0xb0)
                    // marked with the visit byte | 1, the server told.
                    if (Gap < Range)
                        SetPlayerVisitFlagsbaseline(Solar->Id,
                                                  static_cast<unsigned char>(Visit | 0x01u), true);
                }
            }
        }

        // pass 2 -- the current system's zone tree (Universe::get_system
        // of the routine, slot 0x1c, unchecked system) filled with everything within 7500.
        {
            Universe::ISystem const* const System =
                Universe::get_system(GetPlayerStarSystemIdbaseline());
            ISpatialPartition* const Zones = System->GetZoneTree();
            if (Zones != nullptr)
            {
                Geometry::Sphere Query;
                Query.Center = PlayerPos;
                Query.Radius = kZoneQueryRadius;
                FSpatialQueryResult Result;
                Result.Capacity = 0x800;
                Result.Count = 0;
                if (SpatialFill(Zones, &Query, &Result) != 0)
                {
                    for (unsigned Index = 0; Index < Result.Count; ++Index)
                    {
                        // the zone's visit byte (slot 0x80); unless
                        // bit 0 or 7 is set, a player inside the zone grown by 7500 (slot 0x34)
                        // marks the zone id (slot 4) with the byte | 0x29.
                        Universe::IZone const* const Zone =
                            static_cast<Universe::IZone const*>(Result.Items[Index]);
                        const unsigned char Visit = Zone->GetVisit();
                        if ((Visit & 0x81u) != 0)
                            continue;
                        if (!Zone->InZone(PlayerPos, kZoneQueryRadius))
                            continue;
                        SetPlayerVisitFlagsbaseline(Zone->GetId(),
                                                  static_cast<unsigned char>(Visit | 0x29u), true);
                    }
                }
            }
        }

        // pass 3 -- the radar range: the player ship's first scanner
        // (CEquipManager::FindFirst(0x80000) on CShip + 0xe4, the ship pointer unchecked) or the
        // .data default.
        CEqObj* PlayerShip = nullptr;
        {
            unsigned char* const Plus0c = reinterpret_cast<unsigned char*>(Player) + 0xc;
            if (Plus0c != nullptr)
            {
                CEqObj* const EqObj = *reinterpret_cast<CEqObj**>(Plus0c + 4);
                if (EqObj != nullptr &&
                    (static_cast<unsigned>(EqObj->ObjectClass) & 0x503u) == 0x503u)
                    PlayerShip = EqObj;
            }
        }
        float RadarRange = kDefaultRadarRange;
        CEquip* const Scanner = PlayerShip->EquipManager.FindFirst(0x80000);
        if (Scanner != nullptr)
            RadarRange = static_cast<CEScanner*>(Scanner)->GetRadarRange();

        // every ship Gameobj (ManagerKind 1, the routine, then +0x14).
        int Kind = 1;
        for (FClientGameobj* Go = ClientObjDb_GetKindHead(&Kind); Go != nullptr; Go = Go->ListNext)
        {
            // already counted (the routine, +0xa4), or the player.
            if (Go->GetFieldA4() != 0)
                continue;
            if (static_cast<void*>(Go) == static_cast<void*>(Player))
                continue;

            // its ship (mask 0x503, else null -- read unchecked); only
            // while CEqObj +0x140 is below 1.0f (TEST AH,1 / JZ: a NaN passes).
            CEqObj* Ship = nullptr;
            {
                unsigned char* const Plus0c = reinterpret_cast<unsigned char*>(Go) + 0xc;
                if (Plus0c != nullptr)
                {
                    CEqObj* const EqObj = *reinterpret_cast<CEqObj**>(Plus0c + 4);
                    if (EqObj != nullptr &&
                        (static_cast<unsigned>(EqObj->ObjectClass) & 0x503u) == 0x503u)
                        Ship = EqObj;
                }
            }
            if (Ship->CloakPercent >= 1.0f)
                continue;

            // squared distance on the FPU ((dz*dz + dy*dy) + dx*dx),
            // strictly inside the squared radar range.
            const double Dx = static_cast<double>(Ship->WorldPosition.x) - PlayerPos.x;
            const double Dy = static_cast<double>(Ship->WorldPosition.y) - PlayerPos.y;
            const double Dz = static_cast<double>(Ship->WorldPosition.z) - PlayerPos.z;
            double DistSq = Dz * Dz + Dy * Dy;
            DistSq = DistSq + Dx * Dx;
            const double RangeSq = static_cast<double>(RadarRange) * RadarRange;
            if (!(RangeSq > DistSq))
                continue;

            // counted (the routine(1)).
            Go->SetFieldA4(1);

            // its affiliation (Reputation::Vibe::GetAffiliation on
            // CEqObj +0x104, false); a non-zero answer skips it.
            int VibeId = static_cast<int>(Ship->VibeId);
            unsigned Affiliation = 0;
            if (Reputation::Vibe::GetAffiliation(VibeId, Affiliation, false) != 0)
                continue;

            // the faction's visit byte (the routine, zero when absent);
            // unless bit 0 is already set, stored as byte | 0x41 and sent (the routine).
            unsigned char Flags = 0;
            GetPlayerVisitFlagsbaseline(Affiliation, &Flags, 0, 1);
            if ((Flags & 0x01u) != 0)
                continue;
            SetPlayerVisitFlagsbaseline(Affiliation, static_cast<unsigned char>(Flags | 0x41u), true);
        }
    }
}

void HudCameraUpdate(FHudCamera* Cam, float DeltaSeconds)
{
    // (slot 0x6c, thiscall RET 4).

    // the watched object's CEqObj -- watcher - 8, its +0xc'd pointer's
    // +0x4 (ObjRW + 0x10), the ship class mask 0x503 at CObject + 0x4c; anything else is null.
    CEqObj* Ship = nullptr;
    {
        unsigned char* const Held = const_cast<unsigned char*>(
            reinterpret_cast<const unsigned char*>(Cam->Watched.target()));
        if (Held != nullptr)
        {
            unsigned char* const Object = Held - kWatchableOffset;
            if (Object != nullptr)
            {
                unsigned char* const Plus0c = Object + 0xc;
                if (Plus0c != nullptr)
                {
                    CEqObj* const EqObj = *reinterpret_cast<CEqObj**>(Plus0c + 4);
                    if (EqObj != nullptr &&
                        (static_cast<unsigned>(EqObj->ObjectClass) & 0x503u) == 0x503u)
                        Ship = EqObj;
                }
            }
        }
    }

    // with a ship, control exclusion 4 (cinematic) drives the cursor
    // latch and the letterbox. Excluded: unless the local server tick is held (the routine),
    // latch off (the routine(0)) and, bars not showing, bring them in (the routine(1, 3)).
    // Not excluded: latch on and, bars showing, take them out (the routine(0, 3)).
    if (Ship != nullptr)
    {
        if (Ship->is_control_excluded(4u))
        {
            if (!IsClientServerTickPaused())
            {
                SetClientMouseCursorDrawFlag(0);
                if (!IsLetterboxVisible())
                    SetLetterboxTarget(1.0f, 3.0f);
            }
        }
        else
        {
            SetClientMouseCursorDrawFlag(1);
            if (IsLetterboxVisible())
                SetLetterboxTarget(0.0f, 3.0f);
        }
    }

    // the fire latch drops when command 0xc7 is no longer held
    // (the routine), the ship is excluded from control 4, or a NeuroNet window is up. baseline
    // asks the exclusion on EBX even when no ship was found.
    if (G_bHudFireHeld != 0)
    {
        if (!KeyMapIsCommandHeld(0xc7u) || Ship->is_control_excluded(4u) || G_bNnScreenUp)
            G_bHudFireHeld = 0;
    }

    // the aim point of the Targetable_Objects element (the routine on
    // , no null test). The three zero stores before it are overwritten unread.
    Vector Aim;
    G_HudTargetableObjects->GetAimPoint(&Aim);

    // every gun (traverser mask 0x80) gets that point.
    if (Ship != nullptr)
    {
        CEquipTraverser Walk(EquipTypeGun);
        CEGun* Gun = CEGun::cast(Ship->EquipManager.Traverse(Walk));
        while (Gun != nullptr)
        {
            Gun->SetTargetPoint(Aim);
            Gun = CEGun::cast(Ship->EquipManager.Traverse(Walk));
        }
    }

    // firing while the latch is up.
    if (G_bHudFireHeld != 0)
    {
        // the routine(0x3006, 0, 0).
        Win3DBroadcast(0x3006u, 0u, 0u);

        // The watched object again (watcher - 8, or null when the watcher is empty); its +4 is
        // the adjustor whose slot 0x34 fires -- the routine, called directly here as
        // cli_projectile_fx does (baseline's slot numbering is not carried by the port's objects).
        unsigned char* const Held = const_cast<unsigned char*>(
            reinterpret_cast<const unsigned char*>(Cam->Watched.target()));
        unsigned char* const Object = (Held != nullptr) ? Held - kWatchableOffset : nullptr;

        if (G_HudFireChanged != 0)
        {
            // the first frame after the key changed fires with no
            // hardpoint list at all.
            G_HudFireChanged = 0;
            CliShipFireWeapons(Object + 4, nullptr, 0u, &Aim,
                               static_cast<int>(GetPlayerConnectionId()));
        }
        else if (Ship != nullptr)
        {
            // every gun whose munition has no seeker (+0x90 == 0) and
            // which is active (slot +4) goes into a stack list of hardpoint ids (+8); a non-empty
            // list is fired. baseline's list has no bound check.
            unsigned short HardpointIds[24];
            unsigned Count = 0;
            CEquipTraverser Walk(EquipTypeGun);
            CEGun* Gun = CEGun::cast(Ship->EquipManager.Traverse(Walk));
            while (Gun != nullptr)
            {
                if (Gun->MunitionArch()->Seeker == 0 && Gun->IsActive())
                {
                    HardpointIds[Count] = Gun->Id;
                    ++Count;
                }
                Gun = CEGun::cast(Ship->EquipManager.Traverse(Walk));
            }
            if (Count != 0)
                CliShipFireWeapons(Object + 4, HardpointIds, Count, &Aim,
                                   static_cast<int>(GetPlayerConnectionId()));
        }
    }

    // the routine -- once a second, mark the solars / zones in range and the
    // factions of ships within radar range as visited (the player visit map the field).
    HudCameraScanVisits();

    // the routine(dt) -> the routine on the field (the HUD's
    // per-frame hazard / proximity warning).
    UpdateHudHazardWarning(DeltaSeconds);

    // two function-local statics, zeroed on first use (guard bits 1 and 2
    // of the field; their atexit destructors are empty).
    static Vector PrevSteer = {0.0f, 0.0f, 0.0f};  // ..the field
    static Vector LastSent = {0.0f, 0.0f, 0.0f};   // ..the field

    // the steering vector, negative half + positive half per axis.
    Vector Steer;
    Steer.x = G_HudKeySteerNegX + G_HudKeySteerPosX;
    Steer.y = G_HudKeySteerNegY + G_HudKeySteerPosY;
    Steer.z = G_HudKeySteerNegZ + G_HudKeySteerPosZ;

    // the change since last frame; its length (the 0x5f3759df estimate
    // with one Newton step, times the squared length) over 0.5 drops the ramp.
    {
        Vector Delta;
        Delta.x = Steer.x - PrevSteer.x;
        Delta.y = Steer.y - PrevSteer.y;
        Delta.z = Steer.z - PrevSteer.z;
        const double LenSq = static_cast<double>(Delta.z) * Delta.z +
                             static_cast<double>(Delta.y) * Delta.y +
                             static_cast<double>(Delta.x) * Delta.x;
        const float LenSqF = static_cast<float>(LenSq);
        int Bits = *reinterpret_cast<const int*>(&LenSqF);
        Bits = 0x5f3759df - (Bits >> 1);
        const float Seed = *reinterpret_cast<const float*>(&Bits);
        const double Length =
            ((1.5 - static_cast<double>(Seed) * Seed * LenSq * 0.5) * Seed) * LenSq;
        if (Length > 0.5)
            G_HudSteerRamp = 0.0f;
    }

    // remembered before the clamp.
    PrevSteer = Steer;

    // each axis clamped to [-1, 1] (a NaN passes both tests).
    if (Steer.x < -1.0)
        Steer.x = -1.0f;
    else if (Steer.x > 1.0)
        Steer.x = 1.0f;
    if (Steer.y < -1.0)
        Steer.y = -1.0f;
    else if (Steer.y > 1.0)
        Steer.y = 1.0f;
    if (Steer.z < -1.0)
        Steer.z = -1.0f;
    else if (Steer.z > 1.0)
        Steer.z = 1.0f;

    // no steering (squared length under 0.01) drops the ramp; otherwise
    // it grows by rate * dt, capped at 1.
    {
        const double LenSq = static_cast<double>(Steer.y) * Steer.y +
                             static_cast<double>(Steer.x) * Steer.x +
                             static_cast<double>(Steer.z) * Steer.z;
        if (LenSq < 0.01)
        {
            G_HudSteerRamp = 0.0f;
        }
        else
        {
            const double Ramp = static_cast<double>(G_HudSteerRampRate) * DeltaSeconds +
                                G_HudSteerRamp;
            G_HudSteerRamp = static_cast<float>(Ramp);
            if (Ramp > 1.0f)
                G_HudSteerRamp = 1.0f;
        }
    }

    // GetKeyState(VK_SHIFT), high bit.
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0)
    {
        // with a target (the watched object's get_target, slot 0x74),
        // steering is scaled by how far the nose is off the target: s = 1.5 * (1 - dot^2),
        // clamped to [0.1, 1], where dot is the normalised target direction against the nose
        // (minus the third column of the orientation). The ramp is NOT applied on this path.
        // Without a target the vector is left as clamped.
        unsigned char* const Held = const_cast<unsigned char*>(
            reinterpret_cast<const unsigned char*>(Cam->Watched.target()));
        IObjRW* const Object =
            reinterpret_cast<IObjRW*>((Held != nullptr) ? Held - kWatchableOffset : nullptr);
        IObjRW* Target = nullptr;
        Object->get_target(Target);
        if (Target != nullptr)
        {
            const Vector TargetPos = Target->get_position();
            const Vector& OwnPos = Object->get_position();
            const double Dx = static_cast<double>(TargetPos.x) - OwnPos.x;
            const double Dy = static_cast<double>(TargetPos.y) - OwnPos.y;
            const double Dz = static_cast<double>(TargetPos.z) - OwnPos.z;
            const float DzF = static_cast<float>(Dz);   // FST [ESP+0x2c], then FMUL by it
            const double LenSq = Dz * DzF + Dy * Dy + Dx * Dx;
            const float InvLen = static_cast<float>(1.0 / sqrt(LenSq));
            Vector Dir;
            Dir.x = static_cast<float>(Dx * InvLen);
            Dir.y = static_cast<float>(Dy * InvLen);
            Dir.z = DzF * InvLen;

            const Matrix& Rot = Object->get_transform().Rotation;
            Vector Nose;
            Nose.x = -Rot.E[0][2];
            Nose.y = -Rot.E[1][2];
            Nose.z = -Rot.E[2][2];
            const double Dot = static_cast<double>(Nose.z) * Dir.z +
                               static_cast<double>(Nose.y) * Dir.y +
                               static_cast<double>(Nose.x) * Dir.x;

            // _CIpow(dot, the field); (1 - p) * 1.5; below 0.1 -> 0.1f, above 1 -> 1.0f.
            double Scale = (1.0 - pow(Dot, static_cast<double>(G_HudShiftSteerExponent))) * 1.5;
            if (Scale < 0.1)
                Scale = 0.1f;
            if (Scale > 1.0)
                Scale = 1.0f;
            Steer.x = static_cast<float>(Steer.x * Scale);
            Steer.y = static_cast<float>(Steer.y * Scale);
            Steer.z = static_cast<float>(Steer.z * Scale);
        }
    }
    else
    {
        // .
        Steer.x = Steer.x * G_HudSteerRamp;
        Steer.y = Steer.y * G_HudSteerRamp;
        Steer.z = G_HudSteerRamp * Steer.z;
    }

    // any axis moved by more than 1e-6 squared (a NaN counts as moved)
    // since the last one sent -> the routine with (-y, -x, -z), exactly in that argument order,
    // and the vector (unnegated, x/y/z in place) becomes the last one sent.
    const float kSendThresholdSq = 1.0000001111620804e-06f;   // .rdata 0x005dc124
    const double DiffX = static_cast<double>(Steer.x) - LastSent.x;
    const double DiffY = static_cast<double>(Steer.y) - LastSent.y;
    const double DiffZ = static_cast<double>(Steer.z) - LastSent.z;
    if (!(DiffX * DiffX <= kSendThresholdSq) || !(DiffY * DiffY <= kSendThresholdSq) ||
        !(DiffZ * DiffZ <= kSendThresholdSq))
    {
        SetPlayerBehaviorDirectionGated(-Steer.y, -Steer.x, -Steer.z);
        LastSent = Steer;
    }

    // the routine.
    GameCameraUpdate((FGameCamera*)Cam, DeltaSeconds);
}

void ResetCameraShakeAmount()
{
    // (the space init): MOV dword ptr [the field],0x0.
    G_CameraShakeAmount = 0.0f;
}

void HudCameraUpdateShake(FHudCamera* Cam)
{
    // (thiscall, RET 0). 0x0051db00..0x0051db32: a positive amplitude loses
    // per call and snaps to zero once under 1e-4f (.rdata 0x005db098; the compare
    // is on the unrounded difference).
    if (G_CameraShakeAmount > 0.0f)
    {
        const double Decayed = static_cast<double>(G_CameraShakeAmount) - G_CameraShakeDecay;
        G_CameraShakeAmount = static_cast<float>(Decayed);
        if (Decayed < 9.999999747378752e-05f)
            G_CameraShakeAmount = 0.0f;
    }

    // only with a watched object (watcher - 8 non-null).
    const unsigned char* const Held =
        reinterpret_cast<const unsigned char*>(Cam->Watched.target());
    if (Held == nullptr)
        return;
    if (Held - 8 == nullptr)
        return;
    // baseline reads the Ship Gameobj fields straight off that object. The port's watched object
    // is its IObjInspect face; dynamic_cast is the port's own interface-to-object step (the same
    // one GameobjJumpArmState makes), the downcast to the Ship class is unchecked, as baseline's
    // fixed offsets are.
    FClientShipGameobj* const Ship = static_cast<FClientShipGameobj*>(dynamic_cast<FClientGameobj*>(
        reinterpret_cast<IObjInspect*>(const_cast<unsigned char*>(Held - 8))));

    // +0x18c bit 0 (a hull hit, set by slot 105 the routine at
    // ) -> amplitude 0.02f (0x3ca3d70a), bit cleared.
    if ((Ship->Field_0x18c & 0x1u) != 0)
    {
        G_CameraShakeAmount = 0.02f;
        Ship->Field_0x18c = static_cast<unsigned char>(Ship->Field_0x18c & 0xfeu);
    }

    // bit 1 (a shield hit) -> FCOMP 0.005f (.rdata 0x005d3e44),
    // TEST AH,0x41 / JZ: unless the amplitude is already strictly greater, it becomes 0.005f
    // (0x3ba3d70a); the bit is cleared either way.
    if ((Ship->Field_0x18c & 0x2u) != 0)
    {
        if (!(G_CameraShakeAmount > 0.005f))
            G_CameraShakeAmount = 0.005f;
        Ship->Field_0x18c = static_cast<unsigned char>(Ship->Field_0x18c & 0xfdu);
    }

    // with +0x158 (the the routine state) and the +0x188 jump object
    // both set, the routine (FJumpShipEffect::ShakeAmplitude); the current amplitude FCOMP the
    // result, TEST AH,0x41 / JZ -- strictly greater keeps it, otherwise (NaN included) the
    // result is stored (FSTP float). +0x188 is built by the routine / the routine
    // (client_gameobj.cpp).
    if (Ship->Field_0x158 != 0 && Ship->Field_0x188 != nullptr)
    {
        double const Jump = static_cast<FJumpShipEffect*>(Ship->Field_0x188)->ShakeAmplitude();
        if (!(static_cast<double>(G_CameraShakeAmount) > Jump))
            G_CameraShakeAmount = static_cast<float>(Jump);
    }
}

void HudCameraApplyShake(FHudCamera* Cam, float* Orientation)
{
    // (thiscall, RET 4) on a 3x3 row-major orientation.
    HudCameraUpdateShake(Cam);

    // only a positive amplitude shakes.
    if (!(G_CameraShakeAmount > 0.0f))
        return;

    // a = rand() * (1/32768) * (amp - -amp) + -amp; cos stored as a
    // float, sin kept unrounded on the FPU stack.
    {
        const float Amp = G_CameraShakeAmount;
        const float NegAmp = -G_CameraShakeAmount;
        const int Random = rand();
        const double Angle = static_cast<double>(Random) * 3.0517578125e-05f *
                                 (static_cast<double>(Amp) - NegAmp) + NegAmp;
        const float CosA = static_cast<float>(cos(Angle));
        const double SinA = sin(Angle);

        // rows 0 and 2, column by column.
        for (int Column = 0; Column < 3; ++Column)
        {
            const float Row0 = Orientation[Column];
            const float Row2 = Orientation[6 + Column];
            Orientation[Column] = static_cast<float>(SinA * Row2 + static_cast<double>(Row0) * CosA);
            Orientation[6 + Column] =
                static_cast<float>(static_cast<double>(CosA) * Row2 - Row0 * SinA);
        }
    }

    // a second draw, rows 0 and 1.
    {
        const float Amp = G_CameraShakeAmount;
        const float NegAmp = -G_CameraShakeAmount;
        const int Random = rand();
        const double Angle = static_cast<double>(Random) * 3.0517578125e-05f *
                                 (static_cast<double>(Amp) - NegAmp) + NegAmp;
        const float CosB = static_cast<float>(cos(Angle));
        const double SinB = sin(Angle);

        for (int Column = 0; Column < 3; ++Column)
        {
            const float Row0 = Orientation[Column];
            const float Row1 = Orientation[3 + Column];
            Orientation[Column] =
                static_cast<float>(static_cast<double>(CosB) * Row0 - SinB * Row1);
            Orientation[3 + Column] =
                static_cast<float>(static_cast<double>(CosB) * Row1 + Row0 * SinB);
        }
    }
}

void CameraOrientFromForwardAndUp(float* Out, const float* M, const float* Up)
{
    // (cdecl). Forward F = column 2 of M (M[2], M[5], M[8]).
    const double A2 = M[2];
    const double A5 = M[5];
    const float A8 = M[8];   // FSTP to a float local, reloaded from there

    // |F . Up| summed as (a2*u0 + a8*u2) + a5*u1, on the FPU.
    double Dot = A2 * Up[0] + static_cast<double>(A8) * Up[2];
    Dot = Dot + A5 * Up[1];

    float Rx;
    float Ry;
    float Rz;
    // FCOMP 0.999 (double) / TEST AH,5 / JP -- the cross product only
    // when strictly under (a NaN takes the fallback).
    if (fabs(Dot) < 0.999)
    {
        // R = Up x F, normalised. z is stored as a float once and the
        // square uses z * (float)z; the reciprocal length is stored as a float before use.
        const double X = static_cast<double>(A8) * Up[1] - A5 * Up[2];
        const double Y = A2 * Up[2] - static_cast<double>(A8) * Up[0];
        const double Z = A5 * Up[0] - A2 * Up[1];
        const float Zf = static_cast<float>(Z);
        double SumSq = Z * Zf + Y * Y;
        SumSq = SumSq + X * X;
        const float Inv = static_cast<float>(1.0f / sqrt(SumSq));
        Rx = static_cast<float>(X * Inv);
        Ry = static_cast<float>(Y * Inv);
        Rz = static_cast<float>(static_cast<double>(Zf) * Inv);
    }
    else
    {
        // column 0 of M.
        Rx = M[0];
        Ry = M[3];
        Rz = M[6];
    }

    // C = F x R, each component stored as a float.
    const float C0 = static_cast<float>(static_cast<double>(Rz) * A5 - static_cast<double>(Ry) * A8);
    const float C1 = static_cast<float>(static_cast<double>(A8) * Rx - static_cast<double>(Rz) * A2);
    const float C2 = static_cast<float>(static_cast<double>(Ry) * A2 - static_cast<double>(Rx) * A5);

    // the nine locals are laid out row-major {R, C, F} by column and
    // copied out in one REP MOVSD.
    Out[0] = Rx;
    Out[1] = C0;
    Out[2] = M[2];
    Out[3] = Ry;
    Out[4] = C1;
    Out[5] = M[5];
    Out[6] = Rz;
    Out[7] = C2;
    Out[8] = A8;
}

void FGameCamera::GetVelocity(float* OutVelocity)
{
    GameCameraGetVelocity(this, OutVelocity);
}

int FGameCamera::SetProperty(char const* Key, char const* Value)
{
    return GameCameraSetProperty(this, Key, Value) ? 1 : 0;
}

char const* FGameCamera::GetName()
{
    return GameCameraGetName();
}

void FGameCamera::Attach(void* Object)
{
    GameCameraAttach(this, Object);
}

void FGameCamera::ApplyViewport(int const* ViewRect4)
{
    GameCameraApplyViewport(this, ViewRect4);
}

bool FGameCamera::Activate(int const* ViewRect4)
{
    return GameCameraActivate(this, ViewRect4);
}

bool FGameCamera::IsFinished()
{
    return false;
}

void FGameCamera::Deactivate()
{
}

void FGameCamera::Update(float DeltaSeconds)
{
    GameCameraUpdate(this, DeltaSeconds);
}

int FGameCamera::HandleCommand70(void* Command)
{
    return 0;
}

int FGameCamera::HandleCommand74(void* Command)
{
    return 0;
}

void FGameCamera::DrawScene(unsigned LayerFlags, unsigned Param2)
{
    SpaceObjDrawPass((FSpaceCamera*)this, LayerFlags, Param2);
}

void FGameCamera::ScaleEnvironmentEffects()
{
    SpaceEnvFxBeginCockpitPass((FSpaceCamera*)this);
    SpaceEnvFxEndCockpitPass((FSpaceCamera*)this);
}

int FGameCamera::GetSoundMode()
{
    return 1;
}

char const* FHudCamera::GetName()
{
    return HudCameraGetName();
}

void FHudCamera::Attach(void* Object)
{
    HudCameraAttach(this, Object);
}

void FHudCamera::ApplyViewport(int const* ViewRect4)
{
    HudCameraApplyViewport(this, ViewRect4);
}

bool FHudCamera::IsFinished()
{
    return GameCameraHasNoTarget((FGameCamera*)this);
}

bool FHudCamera::Activate(int const* ViewRect4)
{
    return HudCameraActivate(this, ViewRect4);
}

void FHudCamera::Deactivate()
{
    HudCameraDeactivate(this);
}

void FHudCamera::Update(float DeltaSeconds)
{
    HudCameraUpdate(this, DeltaSeconds);
}

int FHudCamera::HandleCommand70(void* Command)
{
    return HudCameraCommand70(Command);
}

int FHudCamera::HandleCommand74(void* Command)
{
    return HudCameraCommand74(Command);
}

void FHudCamera::DrawScene(unsigned LayerFlags, unsigned Param2)
{
    HudCameraDraw((FGameCamera*)this);
}

int FHudCamera::GetSoundMode()
{
    return 2;
}

bool FHudCamera::OnEvent(unsigned MsgId)
{
    return HudCameraOnEvent(this, MsgId);
}
