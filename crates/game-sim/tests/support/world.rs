//! The original game data in `outdir/` (decision O15) loaded once per test
//! binary, and worlds over it with the data root as assets.

use std::path::{Path, PathBuf};
use std::sync::{Arc, OnceLock};

use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::{Assets, WorldConfig};
use neuron_core::vfs::Host;

/// The repository's `outdir/`.
#[must_use]
pub fn outdir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

static DATA: OnceLock<Option<Arc<GameData>>> = OnceLock::new();

/// The databases of `outdir/`, `None` when it is missing.
pub fn game_data() -> Option<Arc<GameData>> {
    DATA.get_or_init(|| {
        let root = outdir();
        let config = DataConfig::read(&Host::new(&root), b"OutpostFrontier.ini")?;
        let mut data = GameData::new();
        data.load(&Host::new(root.join("data")), &config, None);
        Some(Arc::new(data))
    })
    .clone()
}

/// A world over `outdir/` with its assets.
pub fn world(config: WorldConfig) -> Option<World> {
    let data = game_data()?;
    Some(World::new(data, config).with_assets(Assets::new(outdir().join("data"))))
}

/// A world over `outdir/` with its assets and the faction feelings of
/// `InitialWorld.ini`, which the oracle's harness loads before every
/// scenario (`Server_LoadWorldState`; `game_sim::scenario::
/// load_initial_feelings`).
pub fn scenario_world(config: WorldConfig) -> Option<World> {
    let mut world = world(config)?;
    let text = std::fs::read(outdir().join("data/initialworld.ini")).unwrap_or_default();
    game_sim::scenario::load_initial_feelings(&mut world, &text);
    Some(world)
}
