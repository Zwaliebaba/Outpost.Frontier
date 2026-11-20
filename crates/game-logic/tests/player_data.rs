//! The player database's cash, equipment and ship functions over the
//! original data (`outdir/`, decision O15): what reaches the client sink
//! and the controllers.

use std::cell::RefCell;
use std::path::Path;
use std::rc::Rc;
use std::sync::Arc;

use game_logic::player::{self, CASH_CEILING};
use game_logic::server::{ClientSink, ControllerKind, CreateParams, Cx, Logic};
use game_sim::data::config::{DataConfig, GameData};
use game_sim::object::World;
use game_sim::object::world::WorldConfig;
use game_sim::protocol::ToClient;
use game_sim::protocol::to_client::ServerMessage;
use neuron_core::id::create_id;
use neuron_core::vfs::Host;
use neuron_server::controller::{self, Priority};

#[derive(Clone, Default)]
struct Shared(Rc<RefCell<Vec<(u32, ToClient)>>>);

impl ClientSink for Shared {
    fn send(&mut self, client: u32, message: ToClient) {
        self.0.borrow_mut().push((client, message));
    }
}

thread_local! {
    static HEARD: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
}

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
fn cash_equipment_and_ship() {
    let Some(mut world) = world() else {
        println!("outdir/ is missing: no players");
        return;
    };
    let sink = Shared::default();
    let mut logic = Logic::new(Box::new(sink.clone()), 4, false);
    logic.deliver_hook = Some(|_, _, kind, message| {
        HEARD.with(|h| h.borrow_mut().push(format!("{}:{message:?}", kind.name())));
    });
    logic.players.get_mut(2).unwrap().client_id = 2;
    let mut cx = Cx {
        world: &mut world,
        logic: &mut logic,
    };
    controller::create(
        &mut cx,
        b"Content.dll",
        b"GameSupport",
        &CreateParams::None,
        Priority::Highest,
    );

    player::set_cash(&mut cx, 2, 2_000_000_000);
    player::adjust_cash(&mut cx, 2, -500, true);
    let p = cx.logic.players.get(2).unwrap();
    assert_eq!((p.cash, p.total_earned), (CASH_CEILING - 500, -500));

    let ammo = create_id(b"ge_s_cm_01_ammo");
    let bay = cx.world.strings().cargo_bay();
    let first = player::add_equipment(&mut cx, 2, ammo, bay, 5, 1.0, 0, false, true);
    let again = player::add_equipment(&mut cx, 2, ammo, bay, 3, 1.0, 0, false, true);
    assert_eq!(first, again, "a combinable good joins its line");
    let p = cx.logic.players.get(2).unwrap();
    assert_eq!(p.equipment.len(), 1);
    assert_eq!(p.equipment[0].count, 8);
    player::remove_cargo(&mut cx, 2, ammo, 8, 1.0, true);
    assert!(cx.logic.players.get(2).unwrap().equipment.is_empty());

    let ship = create_id(b"ge_fighter");
    player::set_ship_arch(&mut cx, 2, ship);
    let groups = cx.world.data().archetypes.ships[&ship]
        .eq_obj
        .collision_groups
        .len();
    assert_eq!(
        cx.logic.players.get(2).unwrap().collision_groups.len(),
        groups
    );
    player::set_ship_arch(&mut cx, 2, create_id(b"no_such_ship"));
    assert_eq!(cx.logic.players.get(2).unwrap().ship_archetype, ship);
    // An id outside the array does nothing (D598).
    player::set_cash(&mut cx, 9, 5);

    let sent: Vec<String> = sink
        .0
        .borrow()
        .iter()
        .map(|(client, m)| match m {
            ToClient::Server(ServerMessage::SetCash(c)) => format!("{client} cash {}", c.value),
            ToClient::Server(ServerMessage::SetAddItem(a)) => {
                format!("{client} add {} {}", a.count, a.field_0c)
            }
            ToClient::Server(ServerMessage::SetRemoveItem(r)) => {
                format!(
                    "{client} remove {} {} {}",
                    r.field_00, r.field_04, r.field_08
                )
            }
            ToClient::Server(ServerMessage::SetShipArch(s)) => {
                format!("{client} ship {:08x}", s.value)
            }
            other => format!("{client} {other:?}"),
        })
        .collect();
    assert_eq!(
        sent,
        [
            format!("2 cash {CASH_CEILING}"),
            format!("2 cash {}", CASH_CEILING - 500),
            "2 add 5 0".to_owned(),
            "2 add 3 1".to_owned(),
            format!("2 remove {first} 8 0"),
            format!("2 ship {ship:08x}"),
        ]
    );
    let heard = HEARD.with(|h| h.borrow().clone());
    assert_eq!(
        heard,
        [
            "GameSupport:PlayerWorthChanged(0x0)",
            "GameSupport:PlayerWorthChanged(0x0)",
            "GameSupport:PlayerWorthChanged(0x0)",
            "GameSupport:PlayerWorthChanged(0x0)",
            "GameSupport:PlayerWorthChanged(0x0)",
            "GameSupport:PlayerShipArchSet(0x35)",
        ]
    );
    let _ = ControllerKind::GameSupport;
}
