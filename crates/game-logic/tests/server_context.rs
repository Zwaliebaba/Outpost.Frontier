//! The server context (record 0006) over the original data: the shipped
//! `[Initial SP DLLs]` rows create Content's controllers in the order the
//! record predicts, and a nested broadcast reaches them depth first.

use std::path::Path;
use std::sync::Arc;

use game_logic::events::ControllerMessage;
use game_logic::server::{ControllerKind, Cx, Logic, RecordingSink};
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::WorldConfig;
use neuron_core::ini::IniReader;
use neuron_core::vfs::Host;
use neuron_server::controller::ControllerId;

fn world() -> Option<World> {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir");
    let config = DataConfig::read(&Host::new(&root), b"OutpostFrontier.ini")?;
    let mut data = GameData::new();
    data.load(&Host::new(root.join("data")), &config, None);
    Some(World::new(
        Arc::new(data),
        WorldConfig {
            single_player: true,
            ..WorldConfig::default()
        },
    ))
}

#[test]
fn the_shipped_rows_create_contents_controllers_in_order() {
    let Some(mut world) = world() else {
        println!("outdir/ is missing: no controllers");
        return;
    };
    let mut logic = Logic::new(Box::new(RecordingSink::default()));
    let ini = std::fs::read(
        Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir/OutpostFrontier.ini"),
    )
    .expect("server INI");
    let mut reader = IniReader::new();
    assert!(reader.open_memory(&ini));
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    let ids = cx.load_initial_controllers(&mut reader, true);
    // AISandbox and TestAutomation name no factory: their ids are used up.
    let failed: Vec<usize> = ids
        .iter()
        .enumerate()
        .filter(|(_, id)| id.0 == 0)
        .map(|(i, _)| i)
        .collect();
    assert_eq!(failed, [5, 6]);
    let order: Vec<&str> = logic
        .bus
        .active()
        .iter()
        .map(|n| n.kind.unwrap().name())
        .collect();
    assert_eq!(
        order,
        [
            "GameSupport",
            "BaseSupport",
            "SpaceSupport",
            "StoryLauncher",
            "BasePop",
            "SpacePop"
        ]
    );
    let ids: Vec<u32> = logic.bus.active().iter().map(|n| n.id.0).collect();
    assert!(!ids.contains(&6) && !ids.contains(&7), "{ids:?}");
}

#[test]
fn a_nested_broadcast_is_depth_first() {
    let Some(mut world) = world() else {
        println!("outdir/ is missing: no controllers");
        return;
    };
    let mut logic = Logic::new(Box::new(RecordingSink::default()));
    logic.deliver_hook = Some(|cx, id, kind, message| {
        let mut sink_line = format!("{}:{}", kind.name(), message.name());
        if let ControllerMessage::PlayerLogin { player: 1 } = message
            && kind == ControllerKind::GameSupport
        {
            let mut nested = ControllerMessage::PlayerWorthChanged { client: 1 };
            cx.dispatch(ControllerId(0), &mut nested).unwrap();
            sink_line.push_str(" after nested");
        }
        let _ = id;
        LOG.with(|l| l.borrow_mut().push(sink_line));
    });
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    for name in ["SpaceSupport", "GameSupport"] {
        let id = neuron_server::controller::create(
            &mut cx,
            b"Content.dll",
            name.as_bytes(),
            &game_logic::server::CreateParams::None,
            if name == "GameSupport" {
                neuron_server::controller::Priority::Highest
            } else {
                neuron_server::controller::Priority::Normal
            },
        );
        assert_ne!(id.0, 0);
    }
    let mut login = ControllerMessage::PlayerLogin { player: 1 };
    cx.dispatch(ControllerId(0), &mut login).unwrap();
    let log = LOG.with(|l| l.borrow().clone());
    assert_eq!(
        log,
        [
            "GameSupport:PlayerWorthChanged",
            "SpaceSupport:PlayerWorthChanged",
            "GameSupport:PlayerLogin after nested",
            "SpaceSupport:PlayerLogin",
        ]
    );
}

thread_local! {
    static LOG: std::cell::RefCell<Vec<String>> = const { std::cell::RefCell::new(Vec::new()) };
}
