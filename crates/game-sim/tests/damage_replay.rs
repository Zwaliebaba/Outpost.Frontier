//! Damage replay (decision O20, Tier B; record 0003 points 6 and 7): the
//! scenario oracle's `calls.jsonl` holds every `munition_hit` with the
//! damage list the target filled, and (since P2.4) the target's state
//! before the hit (`state`: hull, power, owner, armour, the sub-object
//! that was hit, a shield's threshold and weapon modifier, the munition's
//! damage). The damage computation (`game_sim::object::damage`) runs on
//! that state and its list is compared with the oracle's: the same
//! entries in the same order, hit points within `TOLERANCE`, the same
//! fates and the same destroyed flag. `collision_damage` records replay
//! `Collision::CalcCollisionDamage` from their inputs.
//!
//! Not replayed: the contents a destroyed collision group takes with it
//! (the record does not carry the instance tree), invincible targets (the
//! scenarios have none) and the server's difficulty scale (no players).
//!
//! `OUTPOST_ORACLE_DIR` names a folder above the runs; without it the test
//! prints a notice and passes. A summary goes to `rust-damage-replay.txt`.

mod support;

use std::fmt::Write as _;

use game_sim::data::archetype::eq_obj::CollisionGroup;
use game_sim::data::archetype::equipment::Equipment;
use game_sim::data::constants::Constants;
use game_sim::equip::launcher::ProjectileArch;
use game_sim::equip::shield::Shield;
use game_sim::equip::{Attachment, Equip, EquipKind, EquipManager, Fate, Mount, SubObjId, types};
use game_sim::object::damage::{
    ArchGroup, CAUSE_GUN, DamageList, DamageTarget, GroupState, munition_hit,
};
use neuron_core::physics::calc_collision_damage_scaled;
use support::records::{Json, close, find_files, oracle_dir, read_lines};

const TOLERANCE: f32 = 1.0e-6;
const MAX_REPORTED: usize = 40;

fn f(j: &Json, key: &str) -> Option<f32> {
    j.get(key)?.as_f32()
}

struct Case {
    hp: f32,
    hp_max: f32,
    power: f32,
    power_max: f32,
    owner: u32,
    equipment: EquipManager,
    groups: Vec<ArchGroup>,
    weapon_mod: f64,
    munition: ProjectileArch,
}

fn rebuild(record: &Json) -> Option<Case> {
    let state = record.get("state")?;
    let sub = SubObjId::try_from(record.get("sub")?.as_u32()?).ok()?;
    let mut equipment = EquipManager::default();
    if let Some(scale) = f(state, "armor") {
        let mut arch = Equipment::new(0x22);
        arch.root.nickname = 1;
        equipment.add(Equip::new(
            0x7FF0,
            &arch,
            types::ARMOR,
            EquipKind::Armor(game_sim::equip::misc::Armor {
                hit_pts_scale: scale,
            }),
            false,
        ));
    }
    let mut groups = Vec::new();
    let kind = state
        .get("sub_kind")
        .and_then(Json::as_str)
        .unwrap_or("root");
    let sub_hp = f(state, "sub_hp");
    let sub_max = state.get("sub_hp_max").and_then(Json::as_f64);
    match kind {
        "group" => {
            let mut g = CollisionGroup::new(sub);
            g.hit_pts = sub_max? as i32;
            g.root_health_proxy = state
                .get("sub_proxy")
                .and_then(Json::as_bool)
                .unwrap_or(false);
            g.linked_equip_type = state
                .get("sub_linked_type")
                .and_then(Json::as_u32)
                .unwrap_or(0);
            let mut group = ArchGroup::new(&g, Vec::new());
            group.hit_pts = sub_hp?;
            group.state = match state.get("sub_state").and_then(Json::as_i32) {
                Some(2) => GroupState::Killed,
                Some(3) => GroupState::Destroyed,
                _ => GroupState::Alive,
            };
            groups.push(group);
        }
        "equip" | "shield" => {
            let type_bits = state
                .get("sub_type")
                .and_then(Json::as_u32)
                .unwrap_or(types::GUN);
            let mut arch = Equipment::new(0x11);
            arch.root.nickname = 2;
            arch.root.hit_pts = sub_max? as f32;
            let kind = if kind == "shield" {
                EquipKind::Shield(Shield {
                    online: true,
                    hit_pts: sub_hp?,
                    max_hit_pts: sub_max? as f32,
                    offline_threshold: f(state, "shield_offline_threshold").unwrap_or(0.0),
                    ..Shield::default()
                })
            } else {
                EquipKind::AttachedFx
            };
            let mut e = Equip::new(sub, &arch, type_bits, kind, false);
            let destroyed = state
                .get("sub_destroyed")
                .and_then(Json::as_bool)
                .unwrap_or(false);
            e.mount = Some(Mount {
                connected: !destroyed,
                ..Mount::default()
            });
            if let Some(a) = &mut e.attached {
                a.attachment = Some(Attachment::NonPhysical {
                    instance: None,
                    hp_crc: 0,
                    collisions: true,
                });
                a.hit_pts = sub_hp?;
            }
            equipment.add(e);
        }
        _ => {}
    }
    Some(Case {
        hp: f(state, "hp")?,
        hp_max: f(state, "hp_max")?,
        power: f(state, "power")?,
        power_max: f(state, "power_max")?,
        owner: state.get("owner")?.as_u32()?,
        equipment,
        groups,
        weapon_mod: state
            .get("shield_weapon_mod")
            .and_then(Json::as_f64)
            .unwrap_or(1.0),
        munition: ProjectileArch {
            hull_damage: f(state, "hull_damage")?,
            energy_damage: f(state, "energy_damage")?,
            weapon_type: state.get("weapon_type").and_then(Json::as_u32).unwrap_or(0),
            ..ProjectileArch::default()
        },
    })
}

/// A damage list's entries (sub-object, hit points, fate).
type Entries = Vec<(u32, f32, i32)>;

fn oracle_list(record: &Json) -> Option<(Entries, bool, i32)> {
    let d = record.get("damage")?;
    let entries = d
        .get("entries")?
        .as_array()?
        .iter()
        .map(|e| {
            let a = e.as_array()?;
            Some((
                a.first()?.as_u32()?,
                a.get(1)?.as_f32()?,
                a.get(2)?.as_i32()?,
            ))
        })
        .collect::<Option<Vec<_>>>()?;
    Some((
        entries,
        d.get("destroyed")?.as_bool()?,
        d.get("cause")?.as_i32()?,
    ))
}

fn replay(case: &Case, record: &Json, consts: &Constants) -> Option<DamageList> {
    let sub = SubObjId::try_from(record.get("sub")?.as_u32()?).ok()?;
    let (_, _, cause) = oracle_list(record)?;
    let weapon_mod = case.weapon_mod;
    let modifier = move |_: SubObjId, _: u32| weapon_mod;
    let g = &consts.general;
    let t = DamageTarget {
        hit_pts: case.hp,
        max_hit_pts: case.hp_max,
        owner_player: case.owner,
        difficulty_scale: None,
        invincible: None,
        equipment: &case.equipment,
        groups: &case.groups,
        power: case.power,
        max_power: case.power_max,
        weapons_offline: false,
        single_player: false,
        player_attached_equip_scale: g.player_attached_equip_hit_pts_scale,
        player_group_scale: g.player_collision_group_hit_pts_scale,
        shield_hull_damage_factor: consts.shield.hull_damage_factor,
        death_fuse_burning: false,
        shield_modifier: &modifier,
    };
    let mut list = DamageList::new(cause, 0, 0);
    munition_hit(&t, &case.munition, sub, &mut list, &|_, _| false, &|_| None);
    Some(list)
}

#[test]
fn munition_hits_replay_the_oracle() {
    let Some(dir) = oracle_dir("damage records") else {
        return;
    };
    // The constants the harness ran with: the master's constants.ini.
    let consts =
        support::world::game_data().map_or_else(Constants::default, |d| d.constants.clone());
    let (mut hits, mut checked, mut mismatched, mut collisions, mut collision_mismatched) =
        (0, 0, 0, 0, 0);
    let mut reports = Vec::new();
    for file in find_files(&dir, "calls.jsonl") {
        for record in read_lines(&file) {
            match record.get("rec").and_then(Json::as_str) {
                Some("munition_hit") => {
                    hits += 1;
                    let Some(case) = rebuild(&record) else {
                        continue;
                    };
                    checked += 1;
                    let Some(ours) = replay(&case, &record, &consts) else {
                        continue;
                    };
                    let Some((theirs, destroyed, _)) = oracle_list(&record) else {
                        continue;
                    };
                    let ours_rows: Vec<(u32, f32, i32)> = ours
                        .entries
                        .iter()
                        .map(|e| (u32::from(e.sub_obj_id), e.hit_pts, e.fate.value()))
                        .collect();
                    let ok = ours.destroyed == destroyed
                        && ours_rows.len() == theirs.len()
                        && ours_rows
                            .iter()
                            .zip(&theirs)
                            .all(|(a, b)| a.0 == b.0 && a.2 == b.2 && close(a.1, b.1, TOLERANCE));
                    if !ok {
                        mismatched += 1;
                        if reports.len() < MAX_REPORTED {
                            reports.push(format!(
                                "{} tick {:?}: ours {ours_rows:?} destroyed {} oracle {theirs:?} destroyed {destroyed}",
                                file.display(),
                                record.get("tick").and_then(Json::as_u32),
                                ours.destroyed
                            ));
                        }
                    }
                }
                Some("collision_damage") => {
                    collisions += 1;
                    let (Some(speed), Some(other), Some(own), Some(damage)) = (
                        f(&record, "speed"),
                        f(&record, "other_mass"),
                        f(&record, "own_mass"),
                        f(&record, "damage"),
                    ) else {
                        continue;
                    };
                    let ours = calc_collision_damage_scaled(
                        consts.general.collision_damage_factor,
                        speed,
                        other,
                        own,
                    );
                    if ours.to_bits() != damage.to_bits() {
                        collision_mismatched += 1;
                        if reports.len() < MAX_REPORTED {
                            reports.push(format!(
                                "{}: collision damage {ours} != {damage}",
                                file.display()
                            ));
                        }
                    }
                }
                _ => {}
            }
        }
    }
    let mut summary = String::new();
    let _ = writeln!(
        summary,
        "damage replay: {hits} munition hits, {checked} with target state, {mismatched} mismatched; {collisions} collision damages, {collision_mismatched} mismatched"
    );
    for r in &reports {
        let _ = writeln!(summary, "{r}");
    }
    print!("{summary}");
    let _ = std::fs::write(dir.join("rust-damage-replay.txt"), &summary);
    assert_eq!(mismatched + collision_mismatched, 0, "{summary}");
}

#[test]
fn a_synthetic_hit_rebuilds() {
    let line = concat!(
        r#"{"rec":"munition_hit","tick":12,"target":2,"target_obj":"target","shooter":1,"shooter_obj":"shooter","munition":7,"sub":1,"#,
        r#""hit_pos":["00000000","00000000","00000000"],"#,
        r#""damage":{"cause":1,"inflictor":1,"inflictor_owner":0,"destroyed":false,"count":2,"entries":[[1,"44a28000",0],[2,"42c80000",0]]},"#,
        r#""state":{"hull_damage":"41f00000","energy_damage":"41a00000","weapon_type":0,"hp":"44a64000","hp_max":"44a64000","power":"42f00000","power_max":"42f00000","owner":0,"armor":null,"sub_kind":"root"}}"#
    );
    let record = support::records::parse(line).unwrap();
    let case = rebuild(&record).unwrap();
    let list = replay(&case, &record, &Constants::default()).unwrap();
    let rows: Vec<_> = list
        .entries
        .iter()
        .map(|e| (e.sub_obj_id, e.hit_pts, e.fate))
        .collect();
    assert_eq!(rows, vec![(1, 1300.0, Fate::None), (2, 100.0, Fate::None)]);
    let (theirs, destroyed, cause) = oracle_list(&record).unwrap();
    assert_eq!(cause, CAUSE_GUN);
    assert!(!destroyed);
    assert_eq!(theirs.len(), 2);
}
