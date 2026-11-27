//! `HUD_Status`'s weapon list (`FindHardpointRow`, `RebuildWeaponList`,
//! `ShowWeaponRows`), its counts, its special-mode captions and its update.

use neuron_client::ui::tree::{NodeId, Property, Ui};
use neuron_core::hash::stricmp;
use neuron_core::math::Vector;

use super::{
    HP_TYPE_ROWS, Status, StatusInput, StatusLegalHps, StatusShip, StatusWeapon, format_key, hide,
    r, set_ids, set_number_text, set_text, show_flags, wide,
};

/// `FindHardpointRow`: the caption table's row of the highest hp type the
/// ship's archetype allows on `hardpoint`, cached by name; 0 when nothing
/// fits (and then nothing is cached).
pub fn find_hardpoint_row(
    cache: &mut Vec<(Vec<u8>, i32)>,
    legal_hps: Option<&[StatusLegalHps]>,
    hardpoint: &[u8],
) -> i32 {
    if let Some(&(_, row)) = cache.iter().find(|(name, _)| name == hardpoint) {
        return row;
    }
    let Some(legal_hps) = legal_hps else {
        return 0;
    };
    let mut types = Vec::new();
    for legal in legal_hps {
        for name in &legal.hardpoints {
            if stricmp(hardpoint, name).is_eq() {
                types.push(legal.hp_type);
            }
        }
    }
    if types.is_empty() {
        return 0;
    }
    let mut best_row = 0;
    let mut best_type = 0;
    for (row, &ty) in (0i32..).zip(HP_TYPE_ROWS.iter()) {
        for &t in &types {
            if ty == t && ty > best_type {
                best_type = ty;
                best_row = row;
            }
        }
    }
    cache.push((hardpoint.to_vec(), best_row));
    best_row
}

/// `RebuildWeaponList`: the ship's launchers ordered by their row, then by
/// hardpoint name (`stricmp`), through the reference's bubble sort.
pub fn rebuild_weapon_list(
    cache: &mut Vec<(Vec<u8>, i32)>,
    ship: Option<&StatusShip>,
) -> Vec<StatusWeapon> {
    let mut found = Vec::new();
    if let Some(ship) = ship {
        for l in &ship.launchers {
            let sort_key = find_hardpoint_row(cache, ship.legal_hps.as_deref(), &l.hardpoint);
            found.push(StatusWeapon {
                sub_id: l.sub_id,
                ammo_count: l.ammo_count,
                ids_name: l.ids_name,
                active: l.active,
                hardpoint: l.hardpoint.clone(),
                sort_key,
            });
        }
    }
    let mut order: Vec<usize> = (0..found.len()).collect();
    let last = found.len().saturating_sub(1);
    if last > 0 {
        loop {
            let mut sorted = true;
            for i in 0..last {
                let (first, second) = (&found[order[i]], &found[order[i + 1]]);
                let swap = first.sort_key < second.sort_key
                    || (first.sort_key == second.sort_key
                        && stricmp(&first.hardpoint, &second.hardpoint).is_lt());
                if swap {
                    order.swap(i, i + 1);
                    sorted = false;
                }
            }
            if sorted {
                break;
            }
        }
    }
    order.iter().rev().map(|&i| found[i].clone()).collect()
}

impl Status {
    /// The list rebuilt and the top row clamped (unsigned) to the last
    /// full page.
    fn rebuild_and_clamp(&mut self, input: &StatusInput) {
        self.weapons = rebuild_weapon_list(&mut self.hp_row_cache, input.ship.as_ref());
        let count = i32::try_from(self.weapons.len()).unwrap_or(i32::MAX);
        let max_top = if count > 10 { count - 10 } else { 0 };
        if self.weapon_top_row.cast_unsigned() > max_top.cast_unsigned() {
            self.weapon_top_row = max_top;
        }
    }

    /// `ShowWeaponRows`: up to ten rows filled from the top row and shown,
    /// the scroll bar's range, the rest hidden.
    fn show_weapon_rows(&mut self, ui: &mut Ui, input: &StatusInput) {
        self.rebuild_and_clamp(input);
        let count = self.weapons.len();
        let mut row = 0;
        let mut index = usize::try_from(self.weapon_top_row).unwrap_or(usize::MAX);
        while index < count {
            let weapon = self.weapons[index].clone();
            let mut name = ui.ids_string(weapon.ids_name.cast_unsigned());
            name.truncate(0xFFF);
            hide(ui, self.weapon_text[row], false);
            hide(ui, self.weapon_number_text[row], false);
            set_text(ui, self.weapon_text[row], Some(&name));
            index += 1;
            set_number_text(
                ui,
                self.weapon_number_text[row],
                index.to_string().as_bytes(),
            );
            if weapon.ammo_count >= 0 {
                hide(ui, self.ammo_text[row], false);
                set_number_text(
                    ui,
                    self.ammo_text[row],
                    weapon.ammo_count.to_string().as_bytes(),
                );
            } else {
                hide(ui, self.ammo_text[row], true);
            }
            row += 1;
            if row >= 10 {
                break;
            }
        }
        if let Some(scroll) = self.weapon_list_scroll {
            let last = i32::try_from(count).unwrap_or(i32::MAX).wrapping_sub(1);
            ui.property(scroll, Property::ScrollRange(0, last));
        }
        for row in row..10 {
            hide(ui, self.weapon_number_text[row], true);
            hide(ui, self.ammo_text[row], true);
            hide(ui, self.weapon_text[row], true);
        }
    }

    /// `UpdateRepairBatteryCounts`: the two counts, capped at 999.
    fn update_counts(&self, ui: &mut Ui, input: &StatusInput) {
        for (count, node) in [
            (input.repair_count, self.repair_count_text),
            (input.battery_count, self.battery_count_text),
        ] {
            let count = count.map_or(0, |c| c.min(999));
            set_text(ui, node, Some(&wide(&count.to_string())));
        }
    }

    /// `UpdateSpecialModeCaptions`: the two captions shown, hidden and
    /// worded for the turret view, the rear view, mouse flight and the
    /// tradelane.
    pub(super) fn update_special_mode_captions(&self, ui: &mut Ui, input: &StatusInput) {
        let ship = input.ship.as_ref();
        let tradelane = ship.is_some_and(|s| s.tradelane);
        let behavior_10 = ship
            .and_then(|s| s.behavior.as_ref())
            .is_some_and(|b| b.current_behavior == 10);
        let unlocked = input.mouse_flight_unlocked;
        let (hide_text, hide_cancel) =
            if !self.turret_view && (unlocked || behavior_10) && !tradelane {
                let shown = self.rear_view || (input.mouse_flight_hold_reached && !behavior_10);
                (!shown, true)
            } else {
                (false, false)
            };
        hide(ui, self.special_mode_text, hide_text);
        hide(ui, self.special_mode_text_cancel, hide_cancel);
        if hide_text {
            return;
        }
        if self.turret_view {
            set_ids(
                ui,
                self.special_mode_text,
                if unlocked { 0x615 } else { 0x633 },
            );
            if tradelane {
                return;
            }
        } else if self.rear_view {
            set_ids(ui, self.special_mode_text, 0x62b);
            if tradelane {
                return;
            }
        } else if tradelane {
            show_flags(ui, self.special_mode_text, false);
            return;
        } else {
            set_ids(ui, self.special_mode_text, 0x62c);
        }
        let mut format = ui.ids_string(0x62d);
        format.truncate(0xFFF);
        let command = if self.turret_view {
            0xbf
        } else if unlocked {
            0xf
        } else {
            0xc8
        };
        let mut key = input
            .bindings
            .iter()
            .find(|(c, _)| *c == command)
            .map_or_else(|| wide("???"), |(_, name)| name.clone());
        key.truncate(0xFF);
        let line = format_key(&format, &key);
        set_text(ui, self.special_mode_text_cancel, Some(&line));
    }

    /// The special mode of the player's ship (`Update`'s second part): the
    /// docking hold's caption, the tradelane's, or the captions put back.
    fn update_special_mode(&mut self, ui: &mut Ui, input: &StatusInput) {
        let Some(ship) = input.ship.as_ref() else {
            return;
        };
        let Some(behavior) = ship.behavior.as_ref() else {
            return;
        };
        if behavior.opaque_168 == 0 && !ship.tradelane && behavior.docking_port_status != 0 {
            set_ids(ui, self.special_mode_text, 0x63b);
            show_flags(ui, self.special_mode_text, true);
            show_flags(ui, self.special_mode_text_cancel, false);
            self.special_mode = true;
            if let Some(caption) = &behavior.target_caption {
                self.caption = Some(caption.clone());
                set_text(ui, self.special_mode_text_cancel, Some(caption));
                show_flags(ui, self.special_mode_text_cancel, true);
            }
        } else if self.special_mode || ship.tradelane {
            if ship.tradelane {
                let caption = self.caption.clone();
                set_text(ui, self.special_mode_text_cancel, caption.as_deref());
                show_flags(ui, self.special_mode_text_cancel, true);
                self.special_mode = true;
            } else {
                self.special_mode = false;
                show_flags(ui, self.special_mode_text, false);
                show_flags(ui, self.special_mode_text_cancel, false);
                self.update_special_mode_captions(ui, input);
            }
        }
    }

    /// `HUD_Status::Update` (with `HUD_Target_Base::Update`).
    pub(super) fn update(&mut self, ui: &mut Ui, id: NodeId, input: &mut StatusInput) {
        if self.sync_pending && input.view_object {
            self.sync_pending = false;
            if (input.session_flags & 2 != 0) != self.expanded {
                self.slot12(ui);
            }
        }
        if input.mouse_flight_unlocked != input.latches.mouse_flight_unlocked {
            self.update_special_mode_captions(ui, input);
            input.latches.mouse_flight_unlocked = input.mouse_flight_unlocked;
        }
        if input.mouse_flight_hold_reached != input.latches.mouse_flight_hold_reached {
            self.update_special_mode_captions(ui, input);
            input.latches.mouse_flight_hold_reached = input.mouse_flight_hold_reached;
        }
        self.update_special_mode(ui, input);

        // HUD_Target_Base::Update: the children, the slide, then (open)
        // the sub-target tool tip (D1121).
        ui.base_update(id);
        let frame_time = ui.frame_time;
        if self.mover.update(ui, frame_time) {
            self.on_move_finished(ui, id);
        }
        if self.scaler.update(frame_time) {
            self.on_scale_finished(ui, id);
        }
        if !self.mover.active && !self.scaler.active {
            if self.rows_on != self.rows_on_seen {
                self.rows_on_seen = self.rows_on;
                self.timer = 1.0;
                self.hide_rows(ui, self.rows_on == 0);
            }
            if self.select_group && input.view_object {
                // The weapon groups' first selection (D1121).
                self.select_group = false;
            }
            if input.target {
                let sum = frame_time + f64::from(self.timer);
                self.timer = r(sum);
                if sum > 1.0 {
                    self.timer = r(sum - 1.0);
                    if !self.mover.active && !self.scaler.active {
                        self.once_a_second(ui, input);
                    }
                }
            }
        }
        hide(ui, self.toggle_button, self.expanded && !self.mover.active);
        if let Some(open) = self.session_flag_2 {
            input.session_flags = (input.session_flags & !2) | if open { 2 } else { 0 };
        }
    }

    /// The part of the update that runs once a second: the ship-state
    /// bytes, the counts, the loot button and (open) the rows.
    fn once_a_second(&mut self, ui: &mut Ui, input: &StatusInput) {
        self.thrusters_on = false;
        self.has_thruster = false;
        self.in_tradelane = false;
        self.cruise_active = false;
        if let Some(ship) = &input.ship {
            self.thrusters_on = ship.thrusters_on;
            self.in_tradelane = ship.tradelane;
            self.cruise_active = ship.cruise_active;
            self.has_thruster = ship.has_thruster;
        }
        self.update_counts(ui, input);
        show_flags(ui, self.loot_button, input.loot_in_reach);
        if self.expanded {
            self.show_weapon_rows(ui, input);
        }
    }

    /// `HUD_Status::SetProperty` with `HUD_Target_Base`'s: the close and
    /// open buttons, the scroll bar; 0 or 1, the base never called.
    pub(super) fn property(
        &mut self,
        ui: &mut Ui,
        property: &Property<'_>,
        input: &StatusInput,
    ) -> isize {
        match *property {
            Property::FromChild {
                msg: 0x26, from, ..
            } => {
                if Some(from) == self.close_button {
                    self.slot12(ui);
                    return 1;
                }
                // A weapon row's text toggles its launcher (D1121).
                if Some(from) == self.toggle_button {
                    self.slot12(ui);
                }
                0
            }
            Property::ScrollChanged(from, value) => {
                if !input.mouse_pick_blocked && Some(from) == self.weapon_list_scroll {
                    self.weapon_top_row = value;
                    self.timer = 1.0;
                }
                0
            }
            _ => 0,
        }
    }

    /// `HUD_Target_Base::HitTest` once its gates pass (D1121): the open
    /// panel's box around the hit centre.
    pub(super) fn hit_box(&self, ray: Vector) -> bool {
        if !self.expanded {
            return false;
        }
        let c = self.hit_center;
        let hw = f64::from(self.hit_half_width);
        let hh = f64::from(self.hit_half_height);
        let scale = f64::from(r(f64::from(c.z) / f64::from(ray.z)));
        let plane_x = r(scale * f64::from(ray.x));
        let plane_y = r(scale * f64::from(ray.y));
        let dx = f64::from(r(f64::from(plane_x) - (f64::from(c.x) - hw)));
        let dy = f64::from(r((f64::from(c.y) + hh) - f64::from(plane_y)));
        dx > 0.0 && dx < hw + hw && dy > 0.0 && hh + hh > dy
    }
}
