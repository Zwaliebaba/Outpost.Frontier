// Author: Giperion.

// and the player behaviour command family at 0x004c7950 .. 0x004c7b8b.

#include "player_behavior.h"

#include "player_spawn.h"

#include "common_cequip.h"
#include "common_cobject.h"
#include "common_iface.h"
#include "game_options.h"
#include "globals.h"
#include "hud_status_core.h" // HudStatusIsTurretView / HudStatusIsRearView (+0x5b9 / +0x5ba)
#include "input_manager.h"
#include "menu_ui_init.h"
#include "nn_modal.h" // NnMainButtonHasActivePanel (the routine)
#include "space_letterbox.h"
#include "win3d.h" // Win3DGetHover (the routine)

namespace
{
    // and every other camera in the file use the same eight: a BaseWatcher stores a
    // Watchable*, which is the IObjRW plus eight.
    const int kWatchableOffset = 8;

    // . baseline keeps the clamped throttle here rather than on any object, and
    // both refreshes and returns it.
    float G_PlayerThrottle = 0.0f;

    // . See the mouse-flight lock toggle section in the header.
    bool G_MouseFlightLocked = false;

    // UpdateMouseFlightDirection's own state, module storage/554/555/558/55c.
    float G_MouseFlightHoldTimer = 0.0f;    // _DAT_00673550 -- seconds accumulated this hold
    bool G_MouseFlightEngaged = false;      // latched true once forwarded
    bool G_MouseFlightButtonTrigger = false;// OnMouseFlightRawMessage's one-shot
    float G_MouseFlightOriginX = 0.0f;      // _DAT_00673558 -- cursor X latched at timer==0
    float G_MouseFlightOriginY = 0.0f;      // _DAT_0067355c -- cursor Y latched at timer==0 (same
                                             // invert-aware reading as every other cursor Y below)

    // /0x004c7e5f/0x004c7f4c/0x004c88a9-equivalent: the hold threshold. 0.25s
    // (the field, .rdata 0x3E800000).
    constexpr float kMouseFlightHoldThreshold = 0.25f;

    // The repeated `cVar4 = the routine(); if ((cVar4=='\0' && the field) ||
    // <=_DAT_00673550) && GetGameOption(INVERSE_MOUSE) then height-Y else Y` block --
    // identical at all four sites in the routine (the routine is NnMainButtonHasActivePanel).
    // Reading G_MouseFlightHoldTimer live (rather than taking it as a parameter) reproduces each
    // call site's own point-in-time value automatically.
    int MouseFlightAdjustedCursorY(int RawCursorY)
    {
        bool const bLockOrHoldReady =
            (!NnMainButtonHasActivePanel() && G_MouseFlightLocked) ||
            (G_MouseFlightHoldTimer >= kMouseFlightHoldThreshold);
        // / 0x004c7e84 / 0x004c7f75: the field (the created surface height) minus
        // .
        if (bLockOrHoldReady && GetGameOption(kGameOptInverseMouse) != 0)
            return static_cast<int>(G_RenderSurfaceHeight) - RawCursorY;
        return RawCursorY;
    }

    // The three lines every function in this family opens with. baseline spells them out at each
    // site; there is no shared helper in the binary, but the sequence is byte-identical.
    IBehaviorManager* PlayerBehaviorManager()
    {
        // /0x004c7956: the routine's result goes straight into GetBehaviorManager,
        // null included -- Common's own entry test (062d4ba6 `TEST EDI,EDI / JNZ`) is what turns
        // a missing ship into a null manager.
        return GetBehaviorManager(static_cast<IObjRW*>(GetPlayerObjRW()));
    }

    // Shared preamble of the routine / the routine: fetch the player's CEqObj directly (not
    // through GetBehaviorManager's vtable-slot-84 path) and validate it is a ship.

    // `ADD EAX,0xc` then a null test -- the only way that is zero after a proven
    // non-null pointer is EAX == -0xc, so -0xc is a distinguished "no object" sentinel some
    // ObjRW accessors return.
    // `MOV EAX,[EAX+0x4]` off the +0xc'd pointer reads [ObjRW+0x10], the CEqObj*.
    // [CEqObj+0x4c] & 0x503 == 0x503 is the ship class check (CObject::ObjectClass).
    CEqObj* GetPlayerShipEqObj()
    {
        void* const ObjRW = GetPlayerObjRW();
        if (ObjRW == nullptr || ObjRW == reinterpret_cast<void*>(-0xc))
            return nullptr;

        CEqObj* const EqObj =
            *reinterpret_cast<CEqObj**>(static_cast<unsigned char*>(ObjRW) + 0x10);
        if (EqObj == nullptr)
            return nullptr;

        if ((static_cast<unsigned>(EqObj->ObjectClass) & 0x503u) != 0x503u)
            return nullptr;

        return EqObj;
    }

    // Shared tail of the routine / the routine: the player's engine equipment, or null.
    // /0x004c7c0d: `LEA ECX,[EAX+0xe4]` is CEqObj::EquipManager taken by address (an
    // embedded member, not a pointer), matching CShip::collect_force_components' own
    // `EquipManager.FindFirst(0x20000)` elsewhere in Common.
    CEEngine* GetPlayerEngineEquip()
    {
        CEqObj* const Ship = GetPlayerShipEqObj();
        if (Ship == nullptr)
            return nullptr;

        CEquip* const Eq = const_cast<CEquip*>(Ship->EquipManager.FindFirst(0x20000));
        return CEEngine::cast(Eq);
    }
}

void* GetPlayerObjRW()
{
    // read the field, and when it is set subtract eight. A null watchable yields a
    // null object rather than the address -8.
    void* const Watched = GetPlayerShipWatcher();
    if (Watched == nullptr)
        return nullptr;

    return static_cast<unsigned char*>(Watched) - kWatchableOffset;
}

void ResetPlayerBehaviorDirection()
{
    // a tail JMP, so the manager's own return -- void -- is this function's.
    IBehaviorManager* const Manager = PlayerBehaviorManager();
    if (Manager != nullptr)
        Manager->reset_current_behavior_direction();
}

void SetPlayerStrafeBurst(int Dir)
{
    // .
    IBehaviorManager* const Manager = PlayerBehaviorManager();
    if (Manager != nullptr)
        Manager->update_current_behavior_slide_strafe_burst(static_cast<StrafeDir>(Dir));
}

void SetPlayerAfterburner(bool bOn)
{
    // .
    IBehaviorManager* const Manager = PlayerBehaviorManager();
    if (Manager != nullptr)
        Manager->update_current_behavior_afterburner(bOn);
}

void SetPlayerAutoLevel(bool bOn)
{
    // .
    IBehaviorManager* const Manager = PlayerBehaviorManager();
    if (Manager != nullptr)
        Manager->update_current_behavior_auto_level(bOn);
}

void SetPlayerAutoAvoidance(bool bOn)
{
    // .
    IBehaviorManager* const Manager = PlayerBehaviorManager();
    if (Manager != nullptr)
        Manager->update_current_behavior_auto_avoidance(bOn);
}

void SetPlayerBrakeReverse(bool bOn)
{
    // .
    IBehaviorManager* const Manager = PlayerBehaviorManager();
    if (Manager != nullptr)
        Manager->update_current_behavior_brake_reverse(bOn);
}

void SetPlayerCruise(bool bOn)
{
    // .
    IBehaviorManager* const Manager = PlayerBehaviorManager();
    if (Manager != nullptr)
        Manager->update_current_behavior_cruise(bOn);
}

void SetPlayerEngageEngine(bool bOn)
{
    // .
    IBehaviorManager* const Manager = PlayerBehaviorManager();
    if (Manager != nullptr)
        Manager->update_current_behavior_engage_engine(bOn);
}

void SetPlayerThrottle(float Throttle)
{
    // the argument is stored FIRST, unclamped, and only then overwritten if it is out
    // of range. Nothing observes the intermediate value, but the order is baseline's.
    G_PlayerThrottle = Throttle;

    // the low comparison is against a DOUBLE -1.0 at module storage, while the high
    // one at 0x004c7b1a is against a FLOAT 1.0 at module storage. Widening the float argument to
    // compare it against the double is what the FPU does either way, so the asymmetry costs
    // nothing -- it is just how the two constants were emitted.
    if (static_cast<double>(Throttle) < -1.0)
        G_PlayerThrottle = -1.0f;               // , 0xbf800000
    else if (Throttle > 1.0f)
        G_PlayerThrottle = 1.0f;                // , 0x3f800000

    // .
    IBehaviorManager* const Manager = PlayerBehaviorManager();
    if (Manager != nullptr)
        Manager->update_current_behavior_throttle(G_PlayerThrottle);

    // FCOMP 0.0f (the field), `TEST AH,0x44 / JP`. Equal leaves AH&0x44
    // = 0x40 (odd parity, PF=0): no jump, fall through to the cruise drop. Greater / less (0) and
    // unordered (0x44) have even parity and jump past it. A zero throttle cancels cruise.
    if (G_PlayerThrottle == 0.0f)
    {
        // baseline re-fetches the manager rather than reusing the one above.
        IBehaviorManager* const Again = PlayerBehaviorManager();
        if (Again != nullptr)
            Again->update_current_behavior_cruise(false);   // , PUSH 0
    }
}

float GetLastPlayerThrottle()
{
    // (cdecl, float on the FPU). 0x004c7a00..0x004c7a0f: no player object ->
    // (0.0f).
    if (GetPlayerObjRW() == nullptr)
        return 0.0f;

    // the routine again, then vt+0x4c = IObjInspect::get_throttle
    // (IObjRW's first base) into the field, and that global is what comes back. The int
    // result is discarded.
    IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW());
    Player->get_throttle(G_PlayerThrottle);
    return G_PlayerThrottle;
}

void SetPlayerBehaviorDirectionGated(float X, float Y, float Z)
{
    // X and Y both compare equal to zero (TEST AH,0x44: NaN is not
    // zero), OR -- read only when they do not -- the HUD_Status bytes the field+0x5b9 (turret
    // view) and +0x5ba (rear view) are both clear. HUD_Status is read without a null test.
    bool const bDirIsZero = (X == 0.0f) && (Y == 0.0f);
    if (bDirIsZero || (!HudStatusIsTurretView() && !HudStatusIsRearView()))
    {
        IBehaviorManager* const Manager = PlayerBehaviorManager();
        if (Manager != nullptr)
        {
            Vector Dir;
            Dir.x = X;
            Dir.y = Y;
            Dir.z = Z;
            Manager->update_current_behavior_direction(Dir);
        }
    }
}

bool GetPlayerEngineCruiseActive()
{
    CEEngine* const Engine = GetPlayerEngineEquip();
    if (Engine == nullptr)
        return false;
    return Engine->IsCruiseEngaged();
}

bool GetPlayerEngineIsActive()
{
    CEEngine* const Engine = GetPlayerEngineEquip();
    if (Engine == nullptr)
        return false;
    return Engine->IsActive();
}

bool IsMouseFlightUnlocked()
{
    // `if (the routine()) return true; return the field == 0;`
    if (NnMainButtonHasActivePanel())
        return true;
    return !G_MouseFlightLocked;
}

bool IsMouseFlightLocked()
{
    // `if (the routine()) return false; return the field != 0;`
    if (NnMainButtonHasActivePanel())
        return false;
    return G_MouseFlightLocked;
}

void ToggleMouseFlightLock()
{
    // .
    G_MouseFlightLocked = !G_MouseFlightLocked;
    PlayOrRewindUiSoundSlot(0x1c); // the routine(0x1c)
}

void OnMouseFlightRawMessage(unsigned Msg)
{
    // `if (param_1 == 0x201) the field = 1;` -- WM_LBUTTONDOWN only.
    if (Msg == 0x201u)
        G_MouseFlightButtonTrigger = true;
}

void UpdateMouseFlightDirection()
{
    // the ship's behavior interface, when the player is in a ship in the
    // required object-class state (GetPlayerShipEqObj already reproduces baseline's -0xc-sentinel
    // and 0x503 class-mask checks byte for byte).
    CEqObj* const ShipEqObj = GetPlayerShipEqObj();
    IBehaviorManager* const ShipBehavior =
        (ShipEqObj != nullptr) ? ShipEqObj->get_behavior_interface() : nullptr;

    // `pIVar9[0x168] == 0`. IBehaviorManager has no name for this byte yet (see
    // common_iface.h's Opaque168 field); read it raw the same way baseline's own byte-sized
    // "IBehaviorManager*" indexing does.
    bool bSuppressMouseFlight = false;
    if (ShipBehavior != nullptr)
    {
        unsigned char const Opaque168 = reinterpret_cast<unsigned char const*>(ShipBehavior)[0x168];
        bSuppressMouseFlight = (Opaque168 == 0);
    }

    // /0x004c7d47: the cinematic letterbox, or riding a tradelane, forces the reset path
    // below regardless of the byte above.
    if (IsLetterboxVisible() ||
        (ShipEqObj != nullptr && static_cast<CShip*>(ShipEqObj)->is_using_tradelane()))
    {
        bSuppressMouseFlight = true;
    }

    // `((the field & 1) != 1) && (the field == 0)`. the field is the mouse
    // button bitmask the routine / the routine keep (input_manager.cpp); bit 0 is the left
    // button.
    bool const bMouseButtonHeld = (GetClientMouseButtons() & 1u) == 1u;

    if ((!bMouseButtonHeld && !G_MouseFlightLocked) || bSuppressMouseFlight)
    {
        // reset -- timer 0; when engaged, both latches cleared and the
        // direction zeroed. The tail's call at 0x004c8079 (ship behavior +0xc0 == 0x12) goes to
        // , a bare RET 4 on the HUD_Maneuvers singleton: it has no effect (header).
        G_MouseFlightHoldTimer = 0.0f;
        if (G_MouseFlightEngaged)
        {
            G_MouseFlightEngaged = false;
            G_MouseFlightButtonTrigger = false;

            IBehaviorManager* const Manager = PlayerBehaviorManager();
            if (Manager != nullptr)
            {
                Vector Zero;
                Zero.x = 0.0f;
                Zero.y = 0.0f;
                Zero.z = 0.0f;
                Manager->update_current_behavior_direction(Zero);
            }
        }
        return;
    }

    // `the field || the field`.
    if (!(G_MouseFlightButtonTrigger || G_MouseFlightLocked))
        return;

    // not engaged yet and the routine (the Win3D object under the mouse,
    // ) non-null -- the timer goes back to zero and nothing else happens.
    if (!G_MouseFlightEngaged && Win3DGetHover() != nullptr)
    {
        G_MouseFlightHoldTimer = 0.0f;
        return;
    }

    int CursorX = 0;
    int CursorY = 0;
    int CursorZ = 0;
    GetClientSoftwareCursor(&CursorX, &CursorY, &CursorZ);

    // FCOMP 0.0 / TEST AH,0x44 / JP -- only an ordered-equal zero timer
    // latches the origin (the field FILD, and the invert-aware y).
    if (G_MouseFlightHoldTimer == 0.0f)
    {
        G_MouseFlightOriginX = static_cast<float>(CursorX);
        G_MouseFlightOriginY = static_cast<float>(MouseFlightAdjustedCursorY(CursorY));
    }

    // the routine (double on the FPU) + the float timer; FST float
    // stores the sum, FCOMP compares the unrounded sum against the field (TEST AH,1 / JNZ:
    // less or unordered goes to the cancel-radius test at 0x004c7f2c).
    double const HoldSum =
        GetClientFrameTimeCapped() + static_cast<double>(G_MouseFlightHoldTimer);
    G_MouseFlightHoldTimer = static_cast<float>(HoldSum);

    if (HoldSum >= static_cast<double>(kMouseFlightHoldThreshold))
    {
        // x from the centre of the created surface (the field * 0.5
        // kept on the FPU), divided by the same half width, stored as float.
        double const HalfW = static_cast<double>(static_cast<int>(G_RenderSurfaceWidth)) * 0.5;
        float const NormX =
            static_cast<float>((static_cast<double>(CursorX) - HalfW) / HalfW);
        // the half height is stored as a float first; the invert-aware
        // y minus it, over it.
        float const HalfH =
            static_cast<float>(static_cast<double>(static_cast<int>(G_RenderSurfaceHeight)) * 0.5);
        int const AdjustedY = MouseFlightAdjustedCursorY(CursorY);
        float const NormY = static_cast<float>(
            (static_cast<double>(AdjustedY) - static_cast<double>(HalfH)) /
            static_cast<double>(HalfH));

        // both zero (TEST AH,0x44 / JP|JNP: ordered-equal), or the
        // HUD_Status turret / rear view bytes clear -> the behavior manager's direction
        // {x, y, 0}. That is the routine's body, inlined here byte for byte.
        SetPlayerBehaviorDirectionGated(NormX, NormY, 0.0f);

        // the field = 1.
        G_MouseFlightEngaged = true;
        return;
    }

    // below the threshold. dx is stored as a float, dy stays on the FPU;
    // the radius is 800 / the field * 50 (the field, the field). FCOMPP r*r against
    // dx*dx + dy*dy, TEST AH,5 / JP: only an ordered r*r < sum forces the timer to the
    // threshold (the field).
    float const DeltaX =
        static_cast<float>(static_cast<double>(CursorX) - static_cast<double>(G_MouseFlightOriginX));
    int const AdjustedY2 = MouseFlightAdjustedCursorY(CursorY);
    double const DeltaY =
        static_cast<double>(AdjustedY2) - static_cast<double>(G_MouseFlightOriginY);
    double const CancelRadius =
        800.0 / static_cast<double>(static_cast<int>(G_RenderSurfaceWidth)) * 50.0;
    double const DistSq =
        static_cast<double>(DeltaX) * static_cast<double>(DeltaX) + DeltaY * DeltaY;
    if (CancelRadius * CancelRadius < DistSq)
        G_MouseFlightHoldTimer = kMouseFlightHoldThreshold;
}

void ResetMouseFlightState()
{
    // (inside the routine), in baseline's store order.
    G_MouseFlightLocked = false;
    G_MouseFlightHoldTimer = 0.0f;
    G_MouseFlightEngaged = false;
    G_MouseFlightButtonTrigger = false;
    G_MouseFlightOriginX = 0.0f;
    G_MouseFlightOriginY = 0.0f;
}

bool IsMouseFlightHoldThresholdReached()
{
    // the field (0.25f) <= _DAT_00673550, i.e. the hold timer UpdateMouseFlightDirection
    // maintains has reached the engage threshold.
    return G_MouseFlightHoldTimer >= kMouseFlightHoldThreshold;
}

namespace
{
    float G_UnknownFloat673568 = 0.0f;
    float G_UnknownFloat67356c = 0.0f;
}

void SetUnknownFloat673568(float Value)
{
    // .
    G_UnknownFloat673568 = Value;
}

void SetUnknownFloat67356c(float Value)
{
    // .
    G_UnknownFloat67356c = Value;
}
