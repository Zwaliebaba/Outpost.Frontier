//! Probe (temporary).

use std::path::{Path, PathBuf};
use std::sync::Arc;

use game_logic::selection;
use game_logic::server::{CreateParams, Cx, Logic, RecordingSink};
use game_logic::world;
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::{Assets, WorldConfig};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::rng::CrtRand;
use neuron_core::vfs::Host;
use neuron_server::controller::{self, Priority};

fn outdir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

fn world() -> Option<World> {
    let root = outdir();
    let config = DataConfig::read(&Host::new(&root), b"OutpostFrontier.ini")?;
    let mut data = GameData::new();
    data.load(&Host::new(root.join("data")), &config, None);
    Some(
        World::new(Arc::new(data), WorldConfig::default())
            .with_assets(Assets::new(root.join("data"))),
    )
}

fn draws_since(seed: u32, now: &CrtRand) -> usize {
    let mut r = CrtRand::with_seed(seed);
    for n in 0..10_000_000 {
        if &r == now {
            return n;
        }
        r.rand();
    }
    usize::MAX
}

#[test]
fn probe() {
    let Some(mut world) = world() else { return };
    let mut logic = Logic::new(Box::new(RecordingSink::default()), 16, true);
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    for (name, priority) in [
        ("GameSupport", Priority::Highest),
        ("SpaceSupport", Priority::Normal),
        ("BaseSupport", Priority::Normal),
    ] {
        controller::create(
            &mut cx,
            b"Content.dll",
            name.as_bytes(),
            &CreateParams::None,
            priority,
        );
    }
    let text = std::fs::read(outdir().join("data/initialworld.ini")).unwrap_or_default();
    let mut reader = IniReader::new();
    reader.open_memory(&text);
    selection::load_world_state(&mut cx, &mut reader);
    world::initialize(&mut cx);
    for name in ["he01", "ve03"] {
        cx.world.rng().seed(7);
        world::load(&mut cx, create_id(name.as_bytes()));
        println!("{name}: {}", draws_since(7, cx.world.rng()));
    }
}
