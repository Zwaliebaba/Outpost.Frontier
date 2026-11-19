//! `MsgWin` (`chat_init.cpp`, `chat_msg_win.h`): the chat message window,
//! a `RichTextWin` that shows a snapshot of the chat's message store and
//! is never picked.
//!
//! The store, its snapshots and the reputation's changed-affiliation map
//! are the game's (the chat of P5); the game binds a window to them with a
//! [`MsgWinFeed`] (divergence D473). Each update the feed does the game's
//! part (the lost and new affiliation notices, the ageing of the recent
//! messages), and when the snapshot's generation moved on the window shows
//! the snapshot's list again.

use std::any::Any;
use std::rc::Rc;

use neuron_core::math::{Matrix, Vector};

use crate::text::rdl::{RdlNode, RenderDisplayList};
use crate::ui::control::WinControl;
use crate::ui::controls::rich_text_win::RichTextWin;
use crate::ui::tree::{DrawHost, NodeId, Payload, Property, Ui, Widget};

/// What a message window shows (`FChatDisplaySnapshot`): a generation
/// that moves on with every change, and the list it versions.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ChatSnapshot {
    pub generation: i32,
    pub display: Rc<RenderDisplayList>,
}

/// The game's side of a message window.
pub trait MsgWinFeed {
    /// `MsgWin::Update`'s game part, before the snapshot is looked at: the
    /// notices of the local client's lost and new affiliations from the
    /// changed-affiliation map (then emptied), and the recent messages
    /// rebuilt when the store's age limit passed.
    fn tick(&mut self, ui: &mut Ui) {
        let _ = ui;
    }
    /// The snapshot the window is bound to (`+0x498`), if any.
    fn snapshot(&self) -> Option<ChatSnapshot>;
}

/// `MsgWin`.
#[derive(Default)]
pub struct MsgWin {
    pub win: RichTextWin,
    /// `+0x498`: the game's binding.
    pub feed: Option<Box<dyn MsgWinFeed>>,
    /// `+0x49c`: the snapshot generation last shown.
    pub snapshot_generation: i32,
}

impl std::fmt::Debug for MsgWin {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("MsgWin")
            .field("win", &self.win)
            .field("feed", &self.feed.is_some())
            .field("snapshot_generation", &self.snapshot_generation)
            .finish()
    }
}

impl MsgWin {
    /// `RefreshFromSnapshot`: the content emptied, one reference to the
    /// snapshot's list, the generation remembered.
    pub fn refresh_from_snapshot(&mut self) {
        let Some(snapshot) = self.feed.as_ref().and_then(|f| f.snapshot()) else {
            return;
        };
        self.win.clear_content();
        self.win.append_node(&RdlNode::Ref(snapshot.display));
        self.snapshot_generation = snapshot.generation;
    }

    /// Binds the window to the game's feed with generation 0 shown
    /// (`ChatCreateMsgWin`'s `Snapshot` and `SnapshotGeneration`).
    pub fn bind(&mut self, feed: Box<dyn MsgWinFeed>) {
        self.feed = Some(feed);
        self.snapshot_generation = 0;
    }

    /// `MsgWin::Update`: the game's part, the snapshot shown again when its
    /// generation moved on, then `RichTextWin::Update` (whose tooltip feed
    /// never fires: the window's hit test fails).
    pub fn update(&mut self, ui: &mut Ui, id: NodeId) {
        if let Some(feed) = self.feed.as_mut() {
            feed.tick(ui);
        }
        let generation = self
            .feed
            .as_ref()
            .and_then(|f| f.snapshot())
            .map(|s| s.generation);
        if let Some(generation) = generation
            && generation != self.snapshot_generation
        {
            self.refresh_from_snapshot();
        }
        ui.base_update(id);
    }
}

impl Widget for MsgWin {
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
    fn win_control(&self) -> Option<&WinControl> {
        Some(&self.win.base.control)
    }
    fn win_control_mut(&mut self) -> Option<&mut WinControl> {
        Some(&mut self.win.base.control)
    }
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        payload: &mut Payload<'_>,
    ) -> bool {
        self.win.create(ui, id, parent, payload)
    }
    /// Slot 4: never hit, so the mouse goes through to space.
    fn hit_test(&mut self, _ui: &mut Ui, _id: NodeId, _ray: Vector) -> bool {
        false
    }
    fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
        self.win.set_position(ui, id, position);
    }
    fn set_orientation(&mut self, ui: &mut Ui, id: NodeId, orientation: Matrix) {
        self.win.set_orientation(ui, id, orientation);
    }
    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        self.win.destroy(ui, id);
    }
    fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        self.win.on_delete(ui, id);
    }
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        MsgWin::update(self, ui, id);
    }
    fn update_inactive(&mut self, ui: &mut Ui, id: NodeId) {
        ui.base_update_inactive(id);
    }
    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        self.win.draw(ui, id, host);
    }
    fn property(&mut self, ui: &mut Ui, id: NodeId, property: Property<'_>) -> isize {
        self.win.property(ui, id, property)
    }
}

#[cfg(test)]
mod tests {
    use std::cell::RefCell;

    use super::*;
    use crate::text::font::fake::FakeBackend;
    use crate::text::font::{FontEnv, FontSystem};
    use crate::ui::widescreen::Surface;

    struct Feed(Rc<RefCell<ChatSnapshot>>, Rc<RefCell<u32>>);

    impl MsgWinFeed for Feed {
        fn tick(&mut self, _ui: &mut Ui) {
            *self.1.borrow_mut() += 1;
        }
        fn snapshot(&self) -> Option<ChatSnapshot> {
            Some(self.0.borrow().clone())
        }
    }

    fn units(text: &str) -> Vec<u16> {
        text.encode_utf16().collect()
    }

    #[test]
    fn a_message_window_follows_its_snapshot_and_is_never_hit() {
        let mut ui = Ui::new(Surface::new(640, 480));
        let id = ui
            .create_by_class(Some(b"MsgWin"), b"MsgWin", None, &mut Payload::None)
            .unwrap();
        let snapshot = Rc::new(RefCell::new(ChatSnapshot::default()));
        let ticks = Rc::new(RefCell::new(0));
        {
            let w = ui.widget_mut::<MsgWin>(id).unwrap();
            w.bind(Box::new(Feed(snapshot.clone(), ticks.clone())));
            // ChatCreateMsgWin's set-up.
            w.win.base.set_norm_size(1.0, 0.39);
            w.win.base.draw_frame = false;
            w.win.base.set_align_mode(3);
            w.win.base.set_auto_scrollbar(false);
        }
        // Generation 0: nothing to show yet.
        ui.update_node(id);
        assert_eq!(*ticks.borrow(), 1);
        assert!(
            ui.widget::<MsgWin>(id)
                .unwrap()
                .win
                .content
                .nodes
                .is_empty()
        );
        let mut list = RenderDisplayList::new();
        list.push(RdlNode::Text(units("[Trader] hello")));
        *snapshot.borrow_mut() = ChatSnapshot {
            generation: 1,
            display: Rc::new(list.clone()),
        };
        ui.update_node(id);
        let w = ui.widget::<MsgWin>(id).unwrap();
        assert_eq!(w.snapshot_generation, 1);
        assert_eq!(w.win.content.nodes, vec![RdlNode::Ref(Rc::new(list))]);
        assert!(w.win.base.layout_dirty);
        // The same generation leaves the content alone.
        ui.widget_mut::<MsgWin>(id).unwrap().win.base.layout_dirty = false;
        ui.update_node(id);
        assert!(!ui.widget::<MsgWin>(id).unwrap().win.base.layout_dirty);
        assert!(!ui.with_widget(id, true, |w, ui| w.hit_test(
            ui,
            id,
            Vector::new(0.0, 0.0, -1.0)
        )));
    }

    #[test]
    fn a_message_window_lays_its_messages_out_from_the_bottom() {
        let mut fonts = FontSystem::new(Box::new(FakeBackend::default()), FontEnv::new(800, 600));
        fonts.rich.initialize(fonts.backend.as_mut(), 600);
        fonts.rich.reinitialize_fonts(fonts.backend.as_mut());
        let mut w = MsgWin::default();
        let mut list = RenderDisplayList::new();
        list.push(RdlNode::Text(units("one")));
        list.push(RdlNode::Paragraph);
        list.push(RdlNode::Text(units("two")));
        w.bind(Box::new(Feed(
            Rc::new(RefCell::new(ChatSnapshot {
                generation: 3,
                display: Rc::new(list),
            })),
            Rc::new(RefCell::new(0)),
        )));
        w.refresh_from_snapshot();
        w.win.base.set_align_mode(3);
        w.win.base.pixel_width = 200;
        w.win.base.pixel_height = 100;
        w.win.base.surface = Some(crate::text::layout::device::RichSurface::new(256, 128));
        w.win.base.layout_dirty = true;
        let content = (&w.win.content, w.win.layout_flag);
        w.win
            .base
            .rebuild_layout_if_dirty(&mut fonts, Some(content));
        // Two lines: the layout is shorter than the window, so align mode 3
        // reports the scroll that puts its last line at the bottom.
        let height = w.win.base.layout_height;
        assert!(height > 0 && height < 100, "{height}");
        assert_eq!(w.win.base.scroll_origin(), (0, Some(height - 100)));
    }
}
