//! The offers the generator builds (TU61, `Content/RandomMissionOffer.h`,
//! `Content/RandomMissionRuntime.cpp:196-670`): the vignette offer with its
//! four components, ship groups and ships, solars, loot, objectives and
//! comm sequence conversations, as the generator leaves them (work package
//! P3.7a). The components' message handling and the conversations' run
//! time are the runtime machines' (P3.7b).
//!
//! Both mission-type filters create a vignette offer (`FRmVignetteOffer`),
//! so [`Offer`] holds the base offer's fields and the vignette parts
//! together. The C++ offer is reference counted; the port owns it, and
//! [`release_offer`] is its destructor's effect on Content's state.

use std::collections::BTreeMap;

use game_sim::protocol::fmtstr::FmtStr;
use neuron_core::math::{Matrix, Vector};
use neuron_core::num::trunc_to_i32;

use super::vignette::ParamText;
use crate::api;
use crate::content::npc_names::{self, NpcNameParams};
use crate::server::Cx;

/// One loot object of a vignette (`FRmLootSpawn`): the fields of the
/// `FMissionLoot` the generator writes, and its own three.
#[derive(Clone, Debug, PartialEq)]
pub struct LootSpawn {
    /// `+0xfc`: 2 for the critical loot, 1 for a bonus loot.
    pub kind: i32,
    /// `+0x100`: the critical loot's kind for a critical loot, else 0.
    pub field_0x100: i32,
    /// `+0x104`: the critical loot record (an index into
    /// [`super::tables::RmTables::critical_loot`]).
    pub critical: Option<usize>,
    /// `Info.EquipArchID`.
    pub archetype: u32,
    /// `Info.Units`.
    pub units: u32,
    /// `Info.SystemID`.
    pub system: u32,
    /// `Info.HitPtsScale`.
    pub hit_pts_scale: f32,
    /// `Info.bCanAiTractor` (1 in the template, 0 here).
    pub can_ai_tractor: u8,
    /// `bRelPos`.
    pub rel_pos: u8,
    /// `RelPosOffset`.
    pub rel_pos_offset: Vector,
}

/// One ship of a vignette ship group (`FRmShipSpawn`): the fields the
/// generator writes.
#[derive(Clone, Debug, PartialEq)]
pub struct ShipSpawn {
    /// `FMissionShip::Nickname`: `Vignette_ship_%X`.
    pub nickname: Vec<u8>,
    /// `FMissionShip::bRandomName` (`+0x50`).
    pub random_name: u8,
    /// `Npc.Affiliation`.
    pub affiliation: u32,
    /// `Npc.NpcShipArch`.
    pub npc_ship_arch: u32,
    /// `+0xd4`: the ship carries the offer's target name.
    pub target_name: bool,
    /// `+0xd8` (2).
    pub field_0xd8: i32,
    /// `+0xdc` (1).
    pub field_0xdc: i32,
    /// `+0xe0`.
    pub loot: Vec<LootSpawn>,
}

/// One ship group of a vignette (`FRmShipGroup`).
#[derive(Clone, Debug, PartialEq)]
pub struct ShipGroup {
    /// `+0x00` (1).
    pub field_0x00: u8,
    /// `+0x01` (0).
    pub field_0x01: u8,
    /// `+0x02` (0).
    pub field_0x02: u8,
    /// `+0x04` (0).
    pub field_0x04: i32,
    /// `+0x08` (-1).
    pub field_0x08: i32,
    /// `+0x0c`: the mission system's record (`FindSystemInfo`), by id; 0
    /// without one.
    pub system: u32,
    /// `+0x10` (the identity).
    pub orientation: Matrix,
    /// `+0x34`.
    pub position: Vector,
    /// `+0x40` (-2; 0.9 friendly, -0.9 hostile).
    pub field_0x40: f32,
    /// `+0x80`: `CreateID("transport_helion")`, or the faction's `fighters`
    /// formation.
    pub formation: u32,
    /// `+0x84`.
    pub ships: Vec<ShipSpawn>,
    /// `+0x94`: `++GRmShipGroupSerial`.
    pub serial: u32,
}

/// One solar of a vignette (`FRmSolarSpawn`): the fields the generator
/// writes.
#[derive(Clone, Debug, PartialEq)]
pub struct SolarSpawn {
    /// `FMissionSolar::Nickname`: `Vignette_solar_%X`.
    pub nickname: Vec<u8>,
    /// `Faction`.
    pub faction: u32,
    /// `Orientation`: the formation slot's.
    pub orientation: Matrix,
    /// `Position`: the slot's plus the objective's.
    pub position: Vector,
    /// `ArchetypeId`.
    pub archetype: u32,
    /// `LoadoutId`.
    pub loadout: u32,
    /// The pilot whose personality `GetPilotPersonality` put into
    /// `Personality` (`pilot_rm_solar_*`).
    pub pilot: u32,
    /// `bPilot` (1).
    pub has_pilot: u8,
    /// `+0x4c8` (-0.9).
    pub field_0x4c8: f32,
    /// `+0x4cc`: the `KillableSolars` entry's nickname.
    pub arch: u32,
    /// The entry's `string_id` (read through `+0x4cc`).
    pub arch_string_id: i32,
    /// `+0x4d0`: the entry's solar type.
    pub solar_type: i32,
    /// `+0x4d4`.
    pub dest_sys: u32,
    /// `+0x4d8`: 2 for a solar the player must kill, else 0.
    pub mode_flag: i32,
    /// `+0x4dc` (0).
    pub invincible_duration: f32,
    /// `+0x4e0` (0).
    pub field_0x4e0: u8,
    /// `+0x4e4`.
    pub hitpoints: i32,
    /// `+0x4e8`.
    pub loot: Vec<LootSpawn>,
}

/// An objective of a component (`FRmObjectiveType2`, the guides, or
/// `FRmObjectiveType3`): its text and its marker.
#[derive(Clone, Debug, PartialEq)]
pub struct Objective {
    /// `GetType`: 2 or 3.
    pub kind: i32,
    /// `StrId` and `Params`.
    pub text: ParamText,
    /// `MarkerSystemId`.
    pub marker_system: u32,
    /// `MarkerValue` (0x44d from the generator).
    pub marker_value: i32,
    /// `MarkerPosition`.
    pub marker_position: Vector,
}

/// One line of a comm sequence conversation (`FRmLine`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct ConversationLine {
    /// `+0x40`: the line's speaker.
    pub speaker: i32,
    /// `+0x44`: the sequence's target.
    pub target: i32,
    /// `+0x48`.
    pub text: Vec<u8>,
}

/// A comm sequence conversation (`FRmConversation`).
#[derive(Clone, Debug, PartialEq)]
pub struct Conversation {
    /// `+0x18`: `MarkReady` was called.
    pub ready: bool,
    /// `+0x1c`: seconds.
    pub delay: f32,
    /// `+0x20`.
    pub trigger: i32,
    /// The lines.
    pub lines: Vec<ConversationLine>,
}

/// An offer component (`FRmOfferComponent`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Component {
    /// `+0x0c`: set by the filter that fills it.
    pub active: bool,
    /// `+0x10`.
    pub conversations: Vec<Conversation>,
    /// `+0x1c`: by objective key.
    pub objectives: BTreeMap<i32, Objective>,
}

/// A ship component (`FRmShipComponent`; the target ship component adds
/// `+0x50` and `+0x54`).
#[derive(Clone, Debug, PartialEq)]
pub struct ShipComponent {
    /// The component.
    pub component: Component,
    /// `+0x34`.
    pub groups: Vec<ShipGroup>,
    /// `+0x44`: unset in the C++ until a filter writes it; the port starts
    /// it at 0.
    pub position: Vector,
    /// `+0x50` (0), the target ship component's.
    pub field_0x50: i32,
    /// `+0x54` (-1), the target ship component's.
    pub field_0x54: i32,
}

impl Default for ShipComponent {
    fn default() -> Self {
        Self {
            component: Component::default(),
            groups: Vec::new(),
            position: Vector::ZERO,
            field_0x50: 0,
            field_0x54: -1,
        }
    }
}

/// The solar component (`FRmSolarComponent`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SolarComponent {
    /// The component.
    pub component: Component,
    /// `+0x34`: unset in the C++ until a filter writes it; 0 here.
    pub position: Vector,
    /// `+0x40`.
    pub solars: Vec<SolarSpawn>,
}

/// A vignette offer (`FRmOffer` and `FRmVignetteOffer`).
#[derive(Clone, Debug, PartialEq)]
pub struct Offer {
    /// `+0x10`: destroy solars 1, assassinate 2 or 3.
    pub machine_type: i32,
    /// `+0x14`: the mission system's record, by id; 0 without one.
    pub system: u32,
    /// `+0x18`: the offer group's `mc_costume`.
    pub mc_costume: i32,
    /// `+0x1c`: `pub::GetVoiceID("mc_leg_m01")`.
    pub voice: u32,
    /// `+0x20`.
    pub offer_base: u32,
    /// `+0x24` (-1).
    pub offer_group: i32,
    /// `+0x28` (-1).
    pub hostile_group: u32,
    /// `+0x2c`: the remapped rank.
    pub difficulty: f32,
    /// `+0x30`.
    pub reward: i32,
    /// `+0x34`.
    pub rep_hit: f32,
    /// `+0x38`.
    pub rep_reward: f32,
    /// `+0x3c`: the offer text.
    pub text: FmtStr,
    /// `+0x54`: the type shown to the player.
    pub kind: u8,
    /// `+0x58`.
    pub reward_text: ParamText,
    /// `+0x88`.
    pub failure_text: ParamText,
    /// `+0xb8`: the comm sequences.
    pub general: Component,
    /// `+0xec`: the assassination target's groups.
    pub target_ships: ShipComponent,
    /// `+0x144`.
    pub solars: SolarComponent,
    /// `+0x190`.
    pub ships: ShipComponent,
    /// `+0x1e0`: the target's name.
    pub target_name: NpcNameParams,
}

impl Offer {
    /// `FRmVignetteOffer(machine_type)`.
    #[must_use]
    pub fn new(machine_type: i32) -> Self {
        Self {
            machine_type,
            system: 0,
            mc_costume: 0,
            voice: 0,
            offer_base: 0,
            offer_group: -1,
            hostile_group: u32::MAX,
            difficulty: 0.0,
            reward: 0,
            rep_hit: 0.0,
            rep_reward: 0.0,
            text: FmtStr::new(0),
            kind: 0,
            reward_text: ParamText::default(),
            failure_text: ParamText::default(),
            general: Component::default(),
            target_ships: ShipComponent::default(),
            solars: SolarComponent::default(),
            ships: ShipComponent::default(),
            target_name: NpcNameParams::default(),
        }
    }

    /// `CountCriticalLoot` over `groups`: each critical loot sets the
    /// record and adds its units; a ship's scan stops past 1.
    fn count_critical_loot(groups: &[ShipGroup], loot: &mut Option<usize>, count: &mut i32) {
        for group in groups {
            for ship in &group.ships {
                for l in &ship.loot {
                    if l.kind != 2 {
                        continue;
                    }
                    *loot = l.critical;
                    *count = count.wrapping_add(l.units as i32);
                    if *count > 1 {
                        break;
                    }
                }
            }
        }
    }
}

/// `FRmVignetteOffer::GetTextParam` (`RandomMissionRuntime.cpp:565-670`)
/// with the base offer's `FRmOffer::GetTextParam` (`:215-256`) first:
/// appends `param`'s value to `out`, false when the offer has none.
pub fn text_param(cx: &mut Cx<'_>, offer: &Offer, param: i32, out: &mut FmtStr) -> bool {
    match param {
        1 => {
            out.append_rep_group(offer.offer_group as u32);
            true
        }
        2 => {
            out.append_rep_group(offer.hostile_group);
            true
        }
        7 => {
            out.append_int(offer.reward as u32);
            true
        }
        8 => {
            out.append_int(trunc_to_i32(f64::from(offer.rep_reward) * 100.0) as u32);
            true
        }
        9 => {
            out.append_int(trunc_to_i32(f64::from(offer.rep_hit) * 100.0) as u32);
            true
        }
        11 => {
            out.append_int(trunc_to_i32(f64::from(offer.difficulty)) as u32);
            true
        }
        12 => {
            out.append_base(offer.offer_base);
            true
        }
        14 => {
            // `pub::System::GetName` into the argument slot, which holds the
            // parameter (14) when the call fails.
            let name = api::system::name(cx, offer.system).unwrap_or(14);
            out.append_string(name);
            true
        }
        3 => match offer.solars.solars.iter().find(|s| s.solar_type == 0) {
            Some(s) => {
                out.append_installation(s.arch_string_id);
                true
            }
            None => false,
        },
        4 => {
            let mut count = 0;
            let mut last = 0;
            for s in &offer.solars.solars {
                if s.solar_type == 1 || s.solar_type == 2 {
                    last = s.arch_string_id;
                    count += 1;
                    if count > 1 {
                        out.append_installation(last.wrapping_add(1));
                        return true;
                    }
                }
            }
            if count == 0 {
                return false;
            }
            out.append_installation(last);
            true
        }
        5 => {
            let mut loot = None;
            let mut count = 0i32;
            Offer::count_critical_loot(&offer.target_ships.groups, &mut loot, &mut count);
            if count <= 1 {
                Offer::count_critical_loot(&offer.ships.groups, &mut loot, &mut count);
                if count <= 1 {
                    for s in &offer.solars.solars {
                        for l in &s.loot {
                            if l.kind != 2 {
                                continue;
                            }
                            count = count.wrapping_add(l.units as i32);
                            loot = l.critical;
                            if count > 1 {
                                break;
                            }
                        }
                    }
                }
            }
            if count == 0 {
                return false;
            }
            // The record's name; a loot without one is read through null in
            // the C++ (D875).
            let name = loot
                .and_then(|i| cx.logic.content.rm.tables.critical_loot.get(i))
                .map_or(0, |r| r.name);
            out.append_loot(name);
            true
        }
        6 => {
            let (_, first, second) = npc_names::second_name_values(&offer.target_name.second);
            if first != 0 && second != 0 {
                out.append_string(first as u32);
                out.append_string(second as u32);
                return true;
            }
            let (_, large) = npc_names::second_name_third_value(&offer.target_name.second);
            if large == 0 {
                return false;
            }
            let desig = cx
                .logic
                .content
                .tables
                .faction_names(offer.target_name.affiliation)
                .map_or(0, |n| n.large_ship_desig);
            out.append_string(desig as u32);
            out.append_string(second as u32);
            true
        }
        13 => {
            let position = if offer.target_ships.component.active {
                offer.target_ships.position
            } else if offer.ships.component.active {
                offer.ships.position
            } else {
                offer.solars.position
            };
            let zone = zone_at_position(cx, offer.system, position);
            out.append_zone_id(zone);
            true
        }
        _ => false,
    }
}

/// `GetRmZoneAtPosition` (`RandomMissionGen.cpp:114-159`): of the named
/// zones within 1 of `position` (at most 0x14) the one with the smallest
/// radius; with none, the navigation map cell `row + column * 8 + 1`.
pub fn zone_at_position(cx: &mut Cx<'_>, system: u32, position: Vector) -> u32 {
    let zones = api::system::in_zones(cx, system, position, 1.0, 0x14).unwrap_or_default();
    let mut named = Vec::new();
    for zone in zones {
        if let Ok(name) = api::zone::name(cx, zone)
            && name != 0
        {
            named.push((zone, api::zone::radius(cx, zone)));
        }
    }
    let mut best_zone = 0;
    let mut best_radius = -1.0f32;
    for (zone, radius) in named {
        if radius < best_radius || best_radius == -1.0 {
            best_radius = radius;
            best_zone = zone;
        }
    }
    if best_zone != 0 {
        return best_zone;
    }
    // `GetNavMapScale` writes 0 before it fails.
    let scale = api::misc::nav_map_scale(cx, system).unwrap_or(0.0);
    let size = (133_333.0 / f64::from(scale)) as f32;
    let cell = (f64::from(size) * 0.25) as f32;
    let column =
        trunc_to_i32(((f64::from(position.x) + f64::from(size)) / f64::from(cell)).floor());
    let row = trunc_to_i32(((f64::from(position.z) + f64::from(size)) / f64::from(cell)).floor());
    row.wrapping_add(column.wrapping_mul(8)).wrapping_add(1) as u32
}

/// The offer's destructor (`~FRmVignetteOffer`, `ReleaseTargetName`,
/// `RandomMissionRuntime.cpp:463-518`): when a name was made, it goes back
/// to its faction's names at once unless a ship of a group with `+0x01`
/// carries it (the first ship with the name found decides).
pub fn release_offer(cx: &mut Cx<'_>, offer: &Offer) {
    let name = &offer.target_name;
    if name.first.mad_lib_id == 0 && name.second.mad_lib_id == 0 {
        return;
    }
    let carrier = offer
        .target_ships
        .groups
        .iter()
        .chain(&offer.ships.groups)
        .find(|g| g.ships.iter().any(|s| s.target_name));
    if carrier.is_some_and(|g| g.field_0x01 != 0) {
        return;
    }
    npc_names::release_name_now(cx, name);
}
