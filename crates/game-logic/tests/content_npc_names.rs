//! The NPC name builder (`Content/NpcNames.cpp`, the faction name tables
//! of `Content/FactionProps.cpp`) and `GenerateCargo` (`Content/Cargo.cpp`)
//! over the original data (`outdir/`), on the oracle's start-up with he01
//! loaded (P3.6c's follow-up). Expected values follow the C++.
//!
//! - A fighter formation's leader is named for the formation (`0x3f20`)
//!   and its pilot (`0x3f21` with the rank designation); an escort keeps
//!   the leader's designation (one more use) and gets "Escort", its number
//!   and a last name (`0x3f1d`, `0x3f1e`).
//! - A large ship is named by its archetype, the faction's large ship
//!   designation and a large ship name (`0x3f19`, `0x3f1a`).
//! - A named reputation gives its pilot name and designation back 30 s
//!   after it goes, each on its own timer on SpaceSupport.
//! - A trade guide's ship gets its load from its bases' markets.

use std::path::Path;
use std::sync::Arc;

use game_logic::api::reputation;
use game_logic::content::guides::GuideDesc;
use game_logic::content::npc_names::{self, NpcNameParams, NpcNameRequest};
use game_logic::content::npc_ships::NpcShipParams;
use game_logic::content::rep_handles::{self, RepKind};
use game_logic::content::{cargo, tables};
use game_logic::events::ControllerMessage;
use game_logic::server::{CreateParams, Cx, Logic, RecordingSink};
use game_logic::world;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::{Assets, WorldConfig};
use game_sim::protocol::fmtstr::{FmtStr, FmtValue};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::vfs::Host;
use neuron_server::controller::{self, Priority};

fn outdir() -> std::path::PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

/// he01 on the oracle's start-up, `None` without `outdir/`.
fn setup() -> Option<(World, Logic)> {
    let Some(config) = DataConfig::read(&Host::new(outdir()), b"OutpostFrontier.ini") else {
        println!("outdir/ is missing: no name was built");
        return None;
    };
    let mut data = GameData::new();
    data.load(&Host::new(outdir().join("data")), &config, None);
    let mut world = World::new(Arc::new(data), WorldConfig::default())
        .with_assets(Assets::new(outdir().join("data")));
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 4, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    for (name, priority) in [
        (&b"GameSupport"[..], Priority::Highest),
        (&b"SpaceSupport"[..], Priority::Normal),
        (&b"BaseSupport"[..], Priority::Normal),
    ] {
        let id = controller::create(&mut cx, b"Content.dll", name, &CreateParams::None, priority);
        assert_ne!(id.0, 0);
    }
    let text = std::fs::read(outdir().join("data/initialworld.ini")).unwrap_or_default();
    let mut reader = IniReader::new();
    reader.open_memory(&text);
    game_logic::selection::load_world_state(&mut cx, &mut reader);
    world::load(&mut cx, create_id(b"he01"));
    Some((world, logic))
}

/// The dwords of a name part.
fn values(s: &FmtStr) -> Vec<u32> {
    s.values
        .iter()
        .map(|v| match v {
            FmtValue::Value { value, .. } => *value,
            _ => u32::MAX,
        })
        .collect()
}

fn group(cx: &Cx<'_>, name: &[u8]) -> u32 {
    reputation::reputation_group(cx, name).expect("the faction")
}

fn uses(cx: &Cx<'_>, faction: u32) -> npc_names::NameUses {
    cx.logic.content.state.faction_name_uses[&faction].clone()
}

fn names(cx: &Cx<'_>, faction: u32) -> tables::FactionNames {
    cx.logic
        .content
        .tables
        .faction_names(faction)
        .unwrap()
        .clone()
}

#[test]
fn a_fighter_formation_is_named_by_its_leader_and_escorts() {
    let Some((mut world, mut logic)) = setup() else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let police = group(&cx, b"he_p_grp");
    let n = names(&cx, police);
    let leader = NpcNameRequest {
        name_index: 3,
        formation_size: 3,
        rank_level: 7,
        ship_arch: create_id(b"he_fighter"),
        ..NpcNameRequest::default()
    };
    let mut out = NpcNameParams {
        gender: 1,
        ..NpcNameParams::default()
    };
    assert!(npc_names::build_npc_name(
        &mut cx, police, &leader, &mut out, false
    ));
    assert_eq!(out.affiliation, police);
    // he_p_grp is lawful: its name 3 is 0x4074.
    assert_eq!(out.first.mad_lib_id, 0x3f20);
    let first = values(&out.first);
    assert_eq!(&first[..2], &[n.short_group_name, 0x4074]);
    let (desig, number) = (first[2] as i32, first[3] as i32);
    assert!((0..20).contains(&number));
    // The rank designation by the faction's two thresholds (signed).
    let rank = if 7 < n.rank_threshold[0] {
        n.rank_desig[0]
    } else if 7 < n.rank_threshold[1] {
        n.rank_desig[1]
    } else {
        n.rank_desig[2]
    };
    assert_eq!(out.second.mad_lib_id, 0x3f21);
    let second = values(&out.second);
    assert_eq!(second[0], rank);
    let (pilot_first, pilot_last) = (second[1] as i32, second[2] as i32);
    let u = uses(&cx, police);
    assert!(
        u.pilot
            .iter()
            .any(|p| (p.first, p.second, p.count) == (pilot_first, pilot_last, 1))
    );
    assert!(
        u.formation_desig
            .iter()
            .any(|p| (p.first, p.second, p.count) == (desig, number, 1))
    );

    // The first escort keeps the designation, one use more, and has a last
    // name only.
    let escort = NpcNameRequest {
        escort_number: 1,
        desig,
        desig_number: number,
        ..leader
    };
    let mut out = NpcNameParams {
        gender: 1,
        ..NpcNameParams::default()
    };
    npc_names::build_npc_name(&mut cx, police, &escort, &mut out, false);
    assert_eq!(out.first.mad_lib_id, 0x3f1d);
    assert_eq!(&values(&out.first)[2..], &[desig as u32, number as u32]);
    assert_eq!(out.second.mad_lib_id, 0x3f1e);
    let second = values(&out.second);
    assert_eq!(&second[..3], &[0x3f1f, 1, rank]);
    let u = uses(&cx, police);
    assert!(
        u.formation_desig
            .iter()
            .any(|p| (p.first, p.second, p.count) == (desig, number, 2))
    );
    assert!(
        u.pilot
            .iter()
            .any(|p| (p.first, p.second) == (1, second[3] as i32))
    );
    // The readers find both parts back.
    assert_eq!(npc_names::first_name_value(&out.first), (true, desig));
    assert_eq!(
        npc_names::second_name_values(&out.second),
        (true, 1, second[3] as i32)
    );
}

#[test]
fn a_large_ship_takes_a_large_ship_name() {
    let Some((mut world, mut logic)) = setup() else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let police = group(&cx, b"he_p_grp");
    let n = names(&cx, police);
    let cruiser = create_id(b"li_cruiser");
    let request = NpcNameRequest {
        name_index: 3,
        ship_arch: cruiser,
        ..NpcNameRequest::default()
    };
    let mut out = NpcNameParams::default();
    npc_names::build_npc_name(&mut cx, police, &request, &mut out, false);
    assert_eq!(out.first.mad_lib_id, 0x3f19);
    assert_eq!(out.second.mad_lib_id, 0x3f1a);
    let second = values(&out.second);
    let strid = game_logic::api::misc::ship_arch_strid(&cx, cruiser).unwrap();
    assert_eq!(&second[..2], &[strid, n.large_ship_desig as u32]);
    let large = second[2] as i32;
    assert!(
        n.large_ship_names
            .ranges
            .iter()
            .any(|r| (r.first..=r.last).contains(&large)),
        "{large}"
    );
    assert_eq!(uses(&cx, police).large_ship, vec![(large, 1)]);
    assert_eq!(
        npc_names::second_name_third_value(&out.second),
        (true, large)
    );
}

#[test]
fn a_named_reputation_gives_its_name_back_after_30_seconds() {
    let Some((mut world, mut logic)) = setup() else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let police = group(&cx, b"he_p_grp");
    let mut name = NpcNameParams::default();
    assert!(npc_names::pick_npc_name_gender(&mut cx, police, &mut name));
    assert!(matches!(name.gender, 1 | 2));
    npc_names::build_default_npc_name(&mut cx, police, &mut name, true);
    let handle = npc_names::create_npc_name_reputation(&mut cx, &name);
    let vibe = cx.logic.content.state.rep_handles.get(handle).unwrap().vibe;
    assert_eq!(reputation::affiliation(&mut cx, vibe), Ok(police));
    assert_eq!(
        reputation::name(&mut cx, vibe),
        Ok((name.first.clone(), name.second.clone()))
    );
    let (_, desig) = npc_names::first_name_value(&name.first);
    let (_, number) = npc_names::first_name_last_value(&name.first);
    let (_, first, last) = npc_names::second_name_values(&name.second);
    assert_eq!(
        npc_names::reputation_formation_desig(&mut cx, vibe),
        (true, desig)
    );

    // The last release: both parts wait in the faction's queues, still
    // reserved, and the vibe is freed.
    rep_handles::release(&mut cx, handle);
    assert!(reputation::name(&mut cx, vibe).is_err());
    let u = uses(&cx, police);
    assert_eq!(u.pending_pilot, [(first, last)]);
    assert_eq!(u.pending_formation_desig, [(desig, number)]);
    assert!(u.pilot.iter().any(|p| (p.first, p.second) == (first, last)));

    // The pilot timer releases the pilot name; the designation waits for
    // its own.
    let ids = cx.logic.content.state.load_ids;
    let group_word = names(&cx, police).group;
    let to = cx.logic.content.state.space_support;
    let _ = cx.dispatch(
        to,
        &mut ControllerMessage::TimerExpired {
            param1: ids.faction_name_pilot,
            param2: group_word,
        },
    );
    let u = uses(&cx, police);
    assert!(u.pending_pilot.is_empty());
    assert!(!u.pilot.iter().any(|p| (p.first, p.second) == (first, last)));
    assert!(
        u.formation_desig
            .iter()
            .any(|p| (p.first, p.second) == (desig, number))
    );
    let _ = cx.dispatch(
        to,
        &mut ControllerMessage::TimerExpired {
            param1: ids.faction_name_formation,
            param2: group_word,
        },
    );
    let u = uses(&cx, police);
    assert!(u.pending_formation_desig.is_empty());
    assert!(
        !u.formation_desig
            .iter()
            .any(|p| (p.first, p.second) == (desig, number))
    );
}

#[test]
fn generate_cargo_loads_a_trade_guides_ship() {
    let Some((mut world, mut logic)) = setup() else {
        return;
    };
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let traders = group(&cx, b"co_shi_grp");
    let vibe = reputation::alloc(&mut cx, &FmtStr::new(0), &FmtStr::new(0)).unwrap();
    reputation::set_affiliation(&mut cx, vibe, traders).unwrap();
    let handle = cx
        .logic
        .content
        .state
        .rep_handles
        .create(vibe, RepKind::Plain);
    let ship = create_id(b"he_fighter");
    let mut params = NpcShipParams {
        reputation: Some(handle),
        archetype: ship,
        guide: Some(GuideDesc {
            kind: 2,
            base1: Some(create_id(b"he01_11_Base")),
            base2: Some(create_id(b"he01_03_base")),
            ..GuideDesc::default()
        }),
        ..NpcShipParams::default()
    };
    let out = cargo::generate_cargo(&mut cx, &mut params);
    let desc = params.guide.clone().unwrap();
    let market = cx
        .logic
        .content
        .tables
        .faction_props(traders)
        .unwrap()
        .market_goods
        .clone();
    // A good of the faction's is picked, to take there or back.
    assert!(desc.trade_good != 0 || desc.return_good != 0, "{desc:?}");
    for good in [desc.trade_good, desc.return_good] {
        assert!(good == 0 || market.contains(&good), "{good}");
    }
    // A trade good is one pick of weight 1, fitted to 80% of the hold and
    // appended to the ship's cargo; a return load alone leaves no cargo.
    if desc.trade_good == 0 {
        assert!(out.is_empty() && params.cargo.is_empty());
        return;
    }
    assert_eq!(out.len(), 1);
    assert_eq!((out[0].good, out[0].weight), (desc.trade_good, 1.0));
    assert_eq!(params.cargo.len(), 1);
    assert_eq!(
        (params.cargo[0].good, params.cargo[0].count),
        (out[0].good, out[0].count)
    );
    let hold = game_logic::api::misc::cargo_hold_size(&cx, ship).unwrap();
    let (volume, _) = game_logic::api::misc::good_properties(&cx, desc.trade_good).unwrap();
    assert!(out[0].count >= 1);
    assert!(f64::from(out[0].count) * f64::from(volume) <= f64::from(hold) * 0.8 + 1e-3);
}
