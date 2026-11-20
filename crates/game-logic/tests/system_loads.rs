//! The `system_loads` dump (work package P3.3, decision record 0007 point
//! 11): every Universe system loaded on a multiplayer server
//! ([`world::load`]) and each solar of its list rendered as `Server.dll`'s
//! `OracleDumpSystemLoads` writes it (`CppSource/Server/IServerImpl.cpp`),
//! which `Oracle.exe sim` runs for `CppSource/Oracle/system_loads.txt`.
//!
//! `OUTPOST_ORACLE_DIR` names the folder holding the oracle's
//! `system_loads.jsonl`; without it the test renders the port's lines from
//! `outdir/` and compares nothing.

use std::fmt::Write as _;
use std::path::Path;
use std::sync::Arc;

use game_logic::server::{Cx, Logic, RecordingSink};
use game_logic::world;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::world::{Assets, WorldConfig};
use game_sim::object::{KindData, ObjectKind, World};
use neuron_core::vfs::Host;

fn bits(value: f32) -> String {
    format!("\"{:08x}\"", value.to_bits())
}

/// One solar as the C++ export writes it.
fn render(
    out: &mut String,
    world: &World,
    system: u32,
    index: u32,
    h: game_sim::object::ObjHandle,
) {
    let Some(o) = world.get(h) else {
        return;
    };
    let KindData::Solar(sd) = &o.data else {
        return;
    };
    let Some(eq) = o.equipped.as_deref() else {
        return;
    };
    let p = o.engine.position;
    let orient: Vec<String> = o
        .engine
        .orientation
        .m
        .iter()
        .flatten()
        .map(|v| bits(*v))
        .collect();
    let accessories: Vec<String> = eq
        .space_costume
        .accessory_ids
        .iter()
        .map(u32::to_string)
        .collect();
    let c = eq.space_costume.part_ids;
    let _ = writeln!(
        out,
        "{{\"system\":{system},\"index\":{index},\"id\":{},\"archetype\":{},\"pos\":[{},{},{}],\"orient\":[{}],\
\"type_bits\":{},\"docking_base\":{},\"docking_point_base\":{},\"dest_system\":{},\"dest_gate\":{},\
\"prev_ring\":{},\"next_ring\":{},\"tradelane_space_name\":{},\"visit\":{},\"parent\":{},\"destructible\":{},\
\"weapons_offline\":{},\"vibe\":{},\"voice\":{},\"atmosphere_range\":{},\"equipment\":{},\
\"costume\":[{},{},{},{}],\"accessories\":[{}]}}",
        o.simple.id,
        o.archetype,
        bits(p.x),
        bits(p.y),
        bits(p.z),
        orient.join(","),
        o.simple.type_bits,
        eq.docking_base,
        eq.docking_point_base,
        sd.dest_system,
        sd.dest_gate,
        sd.prev_ring,
        sd.next_ring,
        sd.tradelane_space_name,
        sd.visit_value,
        sd.parent_nickname,
        u32::from(sd.destructible),
        u32::from(eq.weapons_offline),
        eq.vibe,
        eq.voice,
        bits(sd.atmosphere_range),
        eq.equipment.len(),
        c[0],
        c[1],
        c[2],
        c[3],
        accessories.join(","),
    );
}

/// The port's lines for every system.
fn port_lines() -> Option<String> {
    let outdir = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir");
    let config = DataConfig::read(&Host::new(&outdir), b"OutpostFrontier.ini")?;
    let mut data = GameData::new();
    data.load(&Host::new(outdir.join("data")), &config, None);
    let mut world = World::new(Arc::new(data), WorldConfig::default())
        .with_assets(Assets::new(outdir.join("data")));
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 1, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    world::initialize(&mut cx);
    let systems: Vec<u32> = cx.logic.server_world.systems.keys().copied().collect();
    let mut out = String::new();
    for system in systems {
        world::load(&mut cx, system);
        let mut index = 0;
        for h in cx.world.objects_in(system) {
            if cx.world.get(h).is_some_and(|o| o.kind == ObjectKind::Solar) {
                render(&mut out, cx.world, system, index, h);
                index += 1;
            }
        }
        let _ = writeln!(
            out,
            "{{\"system\":{system},\"solars\":{index},\"dynamic\":0}}"
        );
    }
    if !cx.logic.reports.0.is_empty() {
        println!("{} load reports", cx.logic.reports.0.len());
        for r in cx.logic.reports.0.iter().take(10) {
            println!("  {r:?}");
        }
    }
    Some(out)
}

#[test]
fn system_loads_equal_the_oracle() {
    let Some(port) = port_lines() else {
        println!("outdir/ is missing: no system loads");
        return;
    };
    println!("{} port lines", port.lines().count());
    let Ok(dir) = std::env::var("OUTPOST_ORACLE_DIR") else {
        println!("OUTPOST_ORACLE_DIR is unset: system loads rendered, not compared");
        return;
    };
    let path = Path::new(&dir).join("system_loads.jsonl");
    let Ok(oracle) = std::fs::read_to_string(&path) else {
        panic!("{} is missing", path.display());
    };
    let (a, b): (Vec<&str>, Vec<&str>) = (oracle.lines().collect(), port.lines().collect());
    let mut differ = 0;
    for (i, (x, y)) in a.iter().zip(&b).enumerate() {
        if x != y {
            differ += 1;
            if differ <= 20 {
                println!("line {}:\n  oracle {x}\n  port   {y}", i + 1);
            }
        }
    }
    let summary = format!(
        "system_loads: {} oracle lines, {} port lines, {differ} differ",
        a.len(),
        b.len()
    );
    println!("{summary}");
    let _ = std::fs::write(Path::new(&dir).join("rust-system-loads.txt"), &summary);
    assert!(differ == 0 && a.len() == b.len(), "{summary}");
}
