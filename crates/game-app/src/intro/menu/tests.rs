use neuron_client::ui::tree::{NodeId, Ui};
use neuron_client::ui::widescreen::Surface;

use super::*;

#[derive(Default)]
struct Host {
    screens: Vec<i32>,
    settled: u32,
    page: bool,
}

impl MenuHost for Host {
    fn note_menu_screen(&mut self, state: i32) {
        self.screens.push(state);
    }
    fn refresh_loading_progress(&mut self) {}
    fn single_player(&self) -> bool {
        true
    }
    fn show_cursor(&mut self) {}
    fn single_precision(&self) -> bool {
        false
    }
    fn enter_load_game(&mut self, ui: &mut Ui) -> Option<NodeId> {
        self.page.then(|| ui.create_plain(None))
    }
    fn first_settled_frame(&mut self, _: &mut Ui) {
        self.settled += 1;
    }
}

fn start() -> (Ui, MenuNav, Host) {
    let ui = Ui::new(Surface::new(1024, 768));
    let nav = MenuNav {
        current: 1,
        ..MenuNav::default()
    };
    (ui, nav, Host::default())
}

#[test]
fn the_main_menu_is_entered_once_and_the_first_frame_settles_once() {
    let (mut ui, mut nav, mut host) = start();
    nav.driver_update(&mut ui, &mut host);
    nav.driver_update(&mut ui, &mut host);
    assert_eq!(host.screens, [1]);
    assert_eq!(host.settled, 1);
    let column = nav.column.expect("the column");
    assert_eq!(nav.active_page, Some(column));
    assert_eq!(
        ui.children(column).len(),
        11,
        "5 meshes, 5 captions, the version"
    );
}

#[test]
fn load_game_goes_through_three_and_two_to_eighteen() {
    let (mut ui, mut nav, mut host) = start();
    host.page = true;
    nav.driver_update(&mut ui, &mut host);
    let column = nav.column.unwrap();
    ui.widget_mut::<TypeSelect>(column).unwrap().next_state = 3;
    nav.driver_update(&mut ui, &mut host);
    assert_eq!(nav.current, 3, "the poll takes the column's state");
    nav.driver_update(&mut ui, &mut host);
    assert_eq!(host.screens, [1, 3, 2, 18]);
    assert_eq!(nav.current, 18);
    assert_ne!(nav.active_page, Some(column));
    // The column was destroyed as the page before it.
    assert!(
        !ui.node(column)
            .unwrap()
            .is(neuron_client::ui::tree::VISIBLE)
    );
}

#[test]
fn a_page_that_does_not_load_returns_to_the_main_menu() {
    let (mut ui, mut nav, mut host) = start();
    nav.driver_update(&mut ui, &mut host);
    nav.current = 3;
    nav.driver_update(&mut ui, &mut host);
    assert_eq!(host.screens, [1, 3, 2, 18, 1]);
    assert_eq!(nav.current, 1);
}
