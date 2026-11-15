// Author: Giperion.

#pragma once

// The player's behaviour command layer: the routine and the family of one-line forwarders that
// sit beside it at 0x004c7970 .. 0x004c7ae0, plus the throttle setter the routine.

// Every one of them has the same body in baseline: fetch the player's IObjRW (the routine), ask
// COMMON.DLL::GetBehaviorManager for its IBehaviorManager, and -- only when that comes back
// non-null -- forward one argument to a single method on it. The input handlers at 0x004d7f00 ..
// are the callers, and the docking camera's attach slot uses two of them.

// the player's IObjRW, i.e. the watchable at the field minus eight, or null.
void* GetPlayerObjRW();

// IBehaviorManager::reset_current_behavior_direction.
void ResetPlayerBehaviorDirection();

// IBehaviorManager::update_current_behavior_slide_strafe_burst.
// Dir is Common's StrafeDir (common_cequip.h): 0 none, 1 left, 2 right, 3 up, 4 down. It is taken
// as an int so that callers which only issue the command -- the docking camera among them -- do
// not have to pull Common's equipment headers into their translation unit. See implementation notes.
void SetPlayerStrafeBurst(int Dir);

// IBehaviorManager::update_current_behavior_afterburner.
void SetPlayerAfterburner(bool bOn);

// IBehaviorManager::update_current_behavior_auto_level.
void SetPlayerAutoLevel(bool bOn);

// IBehaviorManager::update_current_behavior_auto_avoidance.
void SetPlayerAutoAvoidance(bool bOn);

// IBehaviorManager::update_current_behavior_brake_reverse.
void SetPlayerBrakeReverse(bool bOn);

// IBehaviorManager::update_current_behavior_cruise.
void SetPlayerCruise(bool bOn);

// IBehaviorManager::update_current_behavior_engage_engine.
void SetPlayerEngageEngine(bool bOn);

// clamps to [-1, 1], stores the result in the baseline global the field, forwards
// it as the throttle, and then -- only when the clamped value is exactly zero -- drops cruise.
void SetPlayerThrottle(float Throttle);

// , the last value SetPlayerThrottle stored. the routine reads it back after
// refreshing it from the ship; that refresh is not ported yet (see the .cpp).
float GetLastPlayerThrottle();

// IBehaviorManager::update_current_behavior_direction, gated exactly as baseline
// gates it: only forwarded when X and Y are both zero, or when the HUD_Status bytes
// +0x5b9 (turret view) / +0x5ba (rear view) are both clear (hud_status_core.h;
// read only when the direction is not zero, with no null test).
// All three baseline call sites (0x004d803e, 0x004d807d, 0x0051d78f) pass a genuine third float.
// The first two are HUD_Maneuvers::OnEvent 0x81 / 0x82 (hud_maneuvers.cpp): (0, 0, trail
// distance), so for them the gate always forwards.
void SetPlayerBehaviorDirectionGated(float X, float Y, float Z);

// reads the player's engine CEEngine+0x54 (bCruiseActive) directly, i.e.
// IBehaviorManager is bypassed and CEEngine::IsCruiseEngaged() is read straight off the player's
// engine equipment. False with no ship or no engine.
bool GetPlayerEngineCruiseActive();

// CEEngine::IsActive() (CEquip vtable slot 1) on the player's engine equipment.
// False with no ship or no engine.
bool GetPlayerEngineIsActive();

// -------------------------------------------------------------------------------------------
// Mouse-flight lock toggle: module storage, and the pair of getters that read it.

// and the routine both open with `the routine()` -- NnMainButtonHasActivePanel
// (nn_modal.h), a query on the NN_MainButton singleton the field. That class is not ported, so
// the query answers false and both getters come down to the toggle below.

// The flag itself is also read by the routine (the mouse-flight direction updater) alongside
// & 1 (the left mouse button, GetClientMouseButtons), which is the basis for calling
// this a lock toggle rather than a plain hold.

// true unless mouse-flight is locked on. Named for the OR-gate it feeds in
// mouse-flight direction updates run when the toggle is set OR the (unported)
// right-mouse-held bit is set.
bool IsMouseFlightUnlocked();

// the toggle's raw state.
bool IsMouseFlightLocked();

// flip the toggle and stop UI sound slot 0x1c (FindUiSoundSlot table, already
// ported in menu_ui_init.cpp).
void ToggleMouseFlightLock();

// the raw mouse-message hook the routine (NnMainButtonOnUnclaimedMouse,
// ) forwards an unclaimed press to while the player is in space. baseline only reacts to
// WM_LBUTTONDOWN (0x201): it arms the one-shot trigger UpdateMouseFlightDirection consumes to
// start a hold-to-engage cycle.
void OnMouseFlightRawMessage(unsigned Msg);

// the mouse-flight hold-to-engage state machine. Every frame: while the lock
// toggle is off and neither the left mouse button (the field bit 0) nor
// OnMouseFlightRawMessage's one-shot trigger is set, the timer stays
// at zero and any previously-engaged direction is reset to (0,0,0). Once triggered (or with the
// lock toggle on), the cursor position at the moment the timer was zero is latched as the origin;
// each frame accumulates GetClientFrameTimeCapped() into the timer, and once it reaches 0.25s
// (the field) the normalized cursor offset from screen centre is forwarded once via
// SetPlayerBehaviorDirectionGated and the state latches "engaged". Before the timer reaches the
// threshold, moving the cursor more than a (800/width)*50 px radius from the latched origin forces
// the timer to the threshold outright, so the very next frame engages immediately instead of
// waiting out the rest of the hold.

// (see above) is NnMainButtonHasActivePanel at every site.
// (Win3DGetHover, the Win3D object under the mouse, the field) gates the frames
// before the first engage: non-null zeroes the timer and returns (0x004c7d59..0x004c7d7b).
// The surface size is the created one (the field / the field), not the options mode.
// baseline also calls `IDPMsgHandler::OnAddHost(the field, 0)` (0x004c8079) when the reset
// branch fires AND the ship's current behavior index is 0x12. The name is identical-code
// folding: the target is 0x0053e4a0, a bare `RET 4`, and the field is the HUD_Maneuvers
// singleton (hud_maneuvers.h) passed as `this`. The call does nothing, so nothing is missing.
// Caller: the space frame the branch at 0x0054b0e9 (space_state.cpp UpdateSpaceState).
void UpdateMouseFlightDirection();

// true once UpdateMouseFlightDirection's hold timer (module storage) has reached
// the 0.25s threshold (the field). Previously left `simplified` because the timer's only writer
// (the routine) wasn't ported yet; now that UpdateMouseFlightDirection owns G_MouseFlightHoldTimer,
// this is a straight comparison against it.
bool IsMouseFlightHoldThresholdReached();

// , the tail of the HUD loader the routine (hud_element_list.cpp):
// (lock) = 0, the field (hold timer) = 0, the field / the field = 0,
// / the field (latched origin) = 0. baseline writes the globals inline; the port's
// copies live in this translation unit, so the loader calls this.
void ResetMouseFlightState();

// / the routine -- two raw setters for module storage/0067356c. Both globals are
// write-only across the whole PE (no read anywhere in OutpostFrontier); purpose/owner not
// identified, but the writes themselves are trivial and fully faithful regardless.
void SetUnknownFloat673568(float Value);
void SetUnknownFloat67356c(float Value);
