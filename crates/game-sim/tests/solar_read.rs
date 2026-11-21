//! `CSolar::ReadObj` over every system of the original data (`outdir/`):
//! the solars each system file gives, with the fields the multiplayer
//! load reads (gates, bases, lane rings, visits, parents, voices,
//! costumes, reputation), and the reader's reports.

mod support;

use game_sim::data::Reports;
use game_sim::object::world::WorldConfig;
use game_sim::object::{KindData, ObjectKind};
use neuron_core::id::create_id;

#[test]
fn every_system_file_reads() {
    let Some(mut world) = support::world::world(WorldConfig::default()) else {
        println!("outdir/ is missing: no system files");
        return;
    };
    let systems: Vec<u32> = world.data().universe.systems.keys().copied().collect();
    let mut reports = Reports::default();
    let (mut solars, mut gates, mut bases, mut rings, mut voiced, mut costumed) =
        (0, 0, 0, 0, 0, 0);
    for &system in &systems {
        for s in world.system_solar_spawns_reported(system, &mut reports) {
            solars += 1;
            assert_ne!(s.nickname, 0);
            assert_eq!(s.vibe, s.nickname as i32, "verified reputation");
            gates += usize::from(s.dest_system != 0 && s.dest_gate != 0);
            bases += usize::from(s.base != 0);
            rings += usize::from(s.prev_ring != 0 || s.next_ring != 0);
            voiced += usize::from(s.voice != 0);
            costumed += usize::from(s.costume.part_ids[1] != 0);
        }
    }
    println!(
        "{} systems: {solars} solars, {gates} gates, {bases} on bases, {rings} lane rings, {voiced} voiced, {costumed} with costumes; {} reports",
        systems.len(),
        reports.0.len()
    );
    for r in reports.0.iter().take(20) {
        println!("{r:?}");
    }
    assert!(solars > 1000 && gates > 50 && bases > 50 && rings > 100 && voiced > 50);
}

#[test]
fn he01s_load_carries_the_section_fields() {
    let Some(mut world) = support::world::world(WorldConfig::default()) else {
        println!("outdir/ is missing: no he01");
        return;
    };
    let he01 = create_id(b"he01");
    let mut reports = Reports::default();
    let made = world.load_system_solars_reported(he01, &mut reports);
    let gate = world.find(create_id(b"he01_to_he02")).expect("the gate");
    let o = world.get(gate).unwrap();
    let KindData::Solar(sd) = &o.data else {
        panic!("a solar");
    };
    assert_eq!(sd.dest_system, create_id(b"he02"));
    assert_eq!(sd.dest_gate, create_id(b"he02_to_he01"));
    assert!(sd.gate_anim.is_some(), "a gate plays its open animation");
    let docked = made
        .iter()
        .filter_map(|&h| world.get(h))
        .filter(|o| o.kind == ObjectKind::Solar)
        .filter(|o| o.equipped.as_deref().is_some_and(|e| e.docking_base != 0))
        .count();
    assert!(docked > 0, "solars on bases");
    let with_parent = made
        .iter()
        .filter_map(|&h| world.get(h))
        .filter(
            |o| matches!(&o.data, KindData::Solar(s) if s.parent_nickname == create_id(b"he01_14")),
        )
        .count();
    assert_eq!(with_parent, 5);
    // A lane ring carries its trade-lane item (`CSolar::alloc_equip`) with
    // the other 21 of its loadout and the shield its generator links: 23.
    let ring = made
        .iter()
        .filter_map(|&h| world.get(h))
        .find(|o| o.archetype == create_id(b"trade_lane_ring"))
        .expect("a lane ring");
    let eq = ring.equipped.as_deref().unwrap();
    assert!(
        eq.equipment
            .iter()
            .any(|e| matches!(e.kind, game_sim::equip::EquipKind::TradeLane))
    );
    assert_eq!(eq.equipment.len(), 23);
}
