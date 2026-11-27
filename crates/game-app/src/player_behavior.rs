//! The player's flight commands (`player_behavior.cpp`; work package
//! P5.4b): the forwarders from the space input to the player's ship's
//! behaviour manager, the throttle setter, the engine queries and the
//! mouse-flight hold-to-engage state machine.
//!
//! In single player the player's ship is the shared world's object
//! (record 0002), so the commands go to its behaviour manager in the
//! [`World`] (`GetBehaviorManager`, which makes one for an equipped object
//! that has none). [`PlayerShip`] is that pairing: the world and the
//! ship's handle, `None` while the player has no ship.
//!
//! What the mouse flight reads of the rest of the client each frame (the
//! HUD's turret and rear views, the letterbox, the Win3D hover, the
//! software cursor, the mouse buttons, the main button's panel, the
//! inverse-mouse option and the capped frame time) comes in
//! [`FlightFrame`]; their owners fill it.

use game_sim::ai::manager::PlayerInput;
use game_sim::ai::world::StrafeDir;
use game_sim::equip::{EquipKind, types};
use game_sim::object::{ObjHandle, ObjectKind, World};
use neuron_core::math::Vector;

/// `kMouseFlightHoldThreshold`: the hold before the mouse steers.
pub const MOUSE_FLIGHT_HOLD: f32 = 0.25;

/// `WM_LBUTTONDOWN`, the one raw message the mouse flight takes.
pub const WM_LBUTTONDOWN: u32 = 0x201;

/// The UI sound slot the lock toggle plays (`PlayOrRewindUiSoundSlot`).
pub const MOUSE_FLIGHT_TOGGLE_SOUND: u32 = 0x1c;

/// The player's ship in the shared world.
pub struct PlayerShip<'a> {
    /// The world.
    pub world: &'a mut World,
    /// The ship (`GetPlayerObjRW`); `None` without one.
    pub ship: Option<ObjHandle>,
}

impl PlayerShip<'_> {
    /// `GetBehaviorManager(GetPlayerObjRW())` and one call on it; nothing
    /// without a ship or a manager.
    fn manager(
        &mut self,
        f: impl FnOnce(&mut game_sim::ai::manager::BehaviorManager<World>, &mut World, ObjHandle),
    ) {
        if let Some(h) = self.ship {
            let _ = self.world.with_behavior_manager(h, |m, w| f(m, w, h));
        }
    }

    /// `GetPlayerShipEqObj`: the ship when it is one (class 0x503).
    fn is_ship(&self) -> Option<ObjHandle> {
        let h = self.ship?;
        (self.world.get(h)?.kind == ObjectKind::Ship).then_some(h)
    }

    /// `ResetPlayerBehaviorDirection`.
    pub fn reset_direction(&mut self) {
        self.manager(|m, w, h| m.reset_current_behavior_direction(w, h));
    }

    /// `SetPlayerStrafeBurst`.
    pub fn strafe_burst(&mut self, dir: StrafeDir) {
        self.manager(|m, w, h| m.update_current_behavior(w, h, PlayerInput::SlideStrafeBurst(dir)));
    }

    /// `SetPlayerAfterburner`.
    pub fn afterburner(&mut self, on: bool) {
        self.manager(|m, w, h| m.update_current_behavior(w, h, PlayerInput::Afterburner(on)));
    }

    /// `SetPlayerAutoLevel`.
    pub fn auto_level(&mut self, on: bool) {
        self.manager(|m, w, h| m.update_current_behavior_auto_level(w, h, on));
    }

    /// `SetPlayerAutoAvoidance`.
    pub fn auto_avoidance(&mut self, on: bool) {
        self.manager(|m, _, _| m.update_current_behavior_auto_avoidance(on));
    }

    /// `SetPlayerBrakeReverse`.
    pub fn brake_reverse(&mut self, on: bool) {
        self.manager(|m, w, h| m.update_current_behavior(w, h, PlayerInput::BrakeReverse(on)));
    }

    /// `SetPlayerCruise`.
    pub fn cruise(&mut self, on: bool) {
        self.manager(|m, w, h| m.update_current_behavior_cruise(w, h, on));
    }

    /// `SetPlayerEngageEngine`.
    pub fn engage_engine(&mut self, on: bool) {
        self.manager(|m, w, h| m.update_current_behavior(w, h, PlayerInput::EngageEngine(on)));
    }

    /// `update_current_behavior_direction` on the manager.
    fn direction(&mut self, dir: Vector) {
        self.manager(|m, w, h| m.update_current_behavior_direction(w, h, dir));
    }

    /// The player's engine (`EquipManager.FindFirst(0x20000)`).
    fn engine(&self) -> Option<&game_sim::equip::Equip> {
        let h = self.is_ship()?;
        self.world
            .get(h)?
            .as_equipped()?
            .equipment
            .first(types::ENGINE)
    }

    /// `GetPlayerEngineCruiseActive`: the engine's `IsCruiseEngaged`.
    #[must_use]
    pub fn engine_cruise_active(&self) -> bool {
        self.engine().is_some_and(|e| match &e.kind {
            EquipKind::Engine(engine) => engine.cruise_active,
            _ => false,
        })
    }

    /// `GetPlayerEngineIsActive`.
    #[must_use]
    pub fn engine_is_active(&self) -> bool {
        self.engine().is_some_and(game_sim::equip::Equip::is_active)
    }

    /// The ship's current throttle (`IObjInspect::get_throttle`).
    fn throttle(&self) -> Option<f32> {
        let h = self.ship?;
        Some(self.world.get(h)?.as_ship()?.flight.throttle)
    }

    /// The ship's behaviour manager's +0x168 byte; `None` without a
    /// manager (`get_behavior_interface` null).
    fn manager_field_168(&self) -> Option<bool> {
        let h = self.is_ship()?;
        Some(self.world.behavior_manager(h)?.state.field_168)
    }

    /// `CShip::is_using_tradelane`.
    fn using_tradelane(&self) -> bool {
        self.is_ship()
            .and_then(|h| self.world.get(h)?.as_ship())
            .is_some_and(|s| s.using_tradelane)
    }
}

/// What the mouse flight reads of the client in a frame.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct FlightFrame {
    /// The HUD's turret view (`HudStatusIsTurretView`, +0x5b9).
    pub turret_view: bool,
    /// The HUD's rear view (`HudStatusIsRearView`, +0x5ba).
    pub rear_view: bool,
    /// The cinematic letterbox is up (`IsLetterboxVisible`).
    pub letterbox: bool,
    /// The mouse buttons (`GetClientMouseButtons`; bit 0 the left).
    pub mouse_buttons: u32,
    /// A Win3D object is under the mouse (`Win3DGetHover`).
    pub win3d_hover: bool,
    /// The software cursor (`GetClientSoftwareCursor`).
    pub cursor: (i32, i32),
    /// The created surface (`G_RenderSurfaceWidth`, `Height`).
    pub surface: (i32, i32),
    /// `GetClientFrameTimeCapped`.
    pub frame_time: f64,
    /// `NnMainButtonHasActivePanel`.
    pub main_button_panel: bool,
    /// The inverse-mouse option (`kGameOptInverseMouse`).
    pub inverse_mouse: bool,
}

/// The module state of `player_behavior.cpp`.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PlayerFlight {
    /// `G_PlayerThrottle`: the last throttle set or read.
    pub throttle: f32,
    /// `G_MouseFlightLocked`: the lock toggle.
    pub locked: bool,
    /// The hold timer.
    pub hold_timer: f32,
    /// Latched once the direction was forwarded.
    pub engaged: bool,
    /// `OnMouseFlightRawMessage`'s one-shot.
    pub button_trigger: bool,
    /// The cursor when the timer was zero.
    pub origin: (f32, f32),
}

impl PlayerFlight {
    /// `SetPlayerThrottle`: clamped to [-1, 1] (the low bound compared as a
    /// double, the high as a float), stored and forwarded; a throttle of
    /// exactly zero also drops cruise.
    pub fn set_throttle(&mut self, ship: &mut PlayerShip<'_>, throttle: f32) {
        self.throttle = throttle;
        if f64::from(throttle) < -1.0 {
            self.throttle = -1.0;
        } else if throttle > 1.0 {
            self.throttle = 1.0;
        }
        let t = self.throttle;
        ship.manager(|m, w, h| m.update_current_behavior(w, h, PlayerInput::Throttle(t)));
        if self.throttle == 0.0 {
            ship.cruise(false);
        }
    }

    /// `GetLastPlayerThrottle`: 0 without a ship, else the ship's throttle,
    /// which is also stored.
    pub fn last_throttle(&mut self, ship: &PlayerShip<'_>) -> f32 {
        if ship.ship.is_none() {
            return 0.0;
        }
        if let Some(t) = ship.throttle() {
            self.throttle = t;
        }
        self.throttle
    }

    /// `SetPlayerBehaviorDirectionGated`: forwarded when x and y are both
    /// zero, or when neither the turret nor the rear view is up.
    pub fn direction_gated(ship: &mut PlayerShip<'_>, frame: &FlightFrame, dir: Vector) {
        let zero = dir.x == 0.0 && dir.y == 0.0;
        if zero || (!frame.turret_view && !frame.rear_view) {
            ship.direction(dir);
        }
    }

    /// `IsMouseFlightUnlocked`.
    #[must_use]
    pub fn is_unlocked(&self, frame: &FlightFrame) -> bool {
        frame.main_button_panel || !self.locked
    }

    /// `IsMouseFlightLocked`.
    #[must_use]
    pub fn is_locked(&self, frame: &FlightFrame) -> bool {
        !frame.main_button_panel && self.locked
    }

    /// `ToggleMouseFlightLock`: the toggle flipped; the caller plays
    /// [`MOUSE_FLIGHT_TOGGLE_SOUND`].
    pub fn toggle_lock(&mut self) -> u32 {
        self.locked = !self.locked;
        MOUSE_FLIGHT_TOGGLE_SOUND
    }

    /// `OnMouseFlightRawMessage`.
    pub fn on_raw_message(&mut self, msg: u32) {
        if msg == WM_LBUTTONDOWN {
            self.button_trigger = true;
        }
    }

    /// `IsMouseFlightHoldThresholdReached`.
    #[must_use]
    pub fn hold_reached(&self) -> bool {
        self.hold_timer >= MOUSE_FLIGHT_HOLD
    }

    /// `ResetMouseFlightState` (the HUD loader's tail).
    pub fn reset_mouse_flight(&mut self) {
        let throttle = self.throttle;
        *self = Self {
            throttle,
            ..Self::default()
        };
    }

    /// `MouseFlightAdjustedCursorY`: the y the inverse-mouse option flips
    /// once the lock is on (without the main button's panel) or the hold
    /// is reached.
    fn adjusted_y(&self, frame: &FlightFrame, y: i32) -> i32 {
        let ready = (!frame.main_button_panel && self.locked) || self.hold_reached();
        if ready && frame.inverse_mouse {
            frame.surface.1.wrapping_sub(y)
        } else {
            y
        }
    }

    /// `UpdateMouseFlightDirection`: the space frame's mouse steering.
    pub fn update_mouse_flight(&mut self, ship: &mut PlayerShip<'_>, frame: &FlightFrame) {
        // The manager's +0x168 clear suppresses; so do the letterbox and a
        // trade lane.
        let mut suppress = ship.manager_field_168() == Some(false);
        if frame.letterbox || ship.using_tradelane() {
            suppress = true;
        }
        let held = frame.mouse_buttons & 1 == 1;
        if (!held && !self.locked) || suppress {
            self.hold_timer = 0.0;
            if self.engaged {
                self.engaged = false;
                self.button_trigger = false;
                ship.direction(Vector::ZERO);
            }
            return;
        }
        if !(self.button_trigger || self.locked) {
            return;
        }
        if !self.engaged && frame.win3d_hover {
            self.hold_timer = 0.0;
            return;
        }
        let (x, y) = frame.cursor;
        if self.hold_timer == 0.0 {
            #[allow(clippy::cast_precision_loss)]
            let origin = (x as f32, self.adjusted_y(frame, y) as f32);
            self.origin = origin;
        }
        let sum = frame.frame_time + f64::from(self.hold_timer);
        #[allow(clippy::cast_possible_truncation)]
        let stored = sum as f32;
        self.hold_timer = stored;
        if sum >= f64::from(MOUSE_FLIGHT_HOLD) {
            let half_w = f64::from(frame.surface.0) * 0.5;
            #[allow(clippy::cast_possible_truncation)]
            let nx = ((f64::from(x) - half_w) / half_w) as f32;
            #[allow(clippy::cast_possible_truncation)]
            let half_h = (f64::from(frame.surface.1) * 0.5) as f32;
            let ay = self.adjusted_y(frame, y);
            #[allow(clippy::cast_possible_truncation)]
            let ny = ((f64::from(ay) - f64::from(half_h)) / f64::from(half_h)) as f32;
            Self::direction_gated(
                ship,
                frame,
                Vector {
                    x: nx,
                    y: ny,
                    z: 0.0,
                },
            );
            self.engaged = true;
            return;
        }
        #[allow(clippy::cast_possible_truncation)]
        let dx = (f64::from(x) - f64::from(self.origin.0)) as f32;
        let dy = f64::from(self.adjusted_y(frame, y)) - f64::from(self.origin.1);
        let radius = 800.0 / f64::from(frame.surface.0) * 50.0;
        let dist = f64::from(dx) * f64::from(dx) + dy * dy;
        if radius * radius < dist {
            self.hold_timer = MOUSE_FLIGHT_HOLD;
        }
    }
}
