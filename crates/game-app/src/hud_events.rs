//! The HUD command dispatcher's event registry (`hud_command_dispatch.cpp`,
//! `event_handler.h`): the `Event::Handler` faces registered per key-map
//! event id, the input focus that alone receives mode-1 events, and the
//! event record the handlers read.
//!
//! The screens register through the interface's services
//! (`ui.services.register_event`, `Event::Handler::RegisterEvent`): the
//! client's context ([`crate::client::ClientContext`]) wraps the services it
//! is given in [`EventServices`], which keeps the registrations in the
//! context's [`HudEvents`]. [`dispatch`] is `HudDispatchCommand`: the
//! handlers of an event, newest first, until one takes it.
//!
//! Not here yet (D1113): the cancel stack (`HudPushCancelHandler`), which
//! no ported screen pushes to, and the two handlers the C++ registers from
//! static constructors, the Win3D input handler (`win3d.cpp`, events 1, 2
//! and 4 to 13) and the main command handler (`main_command_handler.cpp`,
//! `USER_EXIT_GAME`, `USER_PAUSE`, `USER_RESET`, `USER_FULLSCREEN`,
//! `USER_SCREEN_SHOT`).

use std::cell::RefCell;
use std::rc::Rc;

use neuron_client::ui::controls::rich_text_edit::RichTextEditWin;
use neuron_client::ui::services::{HudRecord, ImeBox, MontageTile, SystemCursor, UiServices};
use neuron_client::ui::tree::{NodeId, Ui};

use crate::intro::char_select_single::PageCharacterSelectSingle;
use crate::intro::options::PageOptions;
use crate::intro::type_select::TypeSelect;

/// `USER_CANCEL`'s event id.
pub const EVENT_CANCEL: u32 = 0xf;

/// `WM_KEYDOWN`.
pub const WM_KEYDOWN: u32 = 0x100;
/// `WM_KEYUP`.
pub const WM_KEYUP: u32 = 0x101;

/// The registry (`G_HudCmdRegistryPtr`), the input focus
/// (`G_HudCmdMode1Filter`, `G_HudCmdFocusStack`) and the event record
/// (`G_HudCmdRecord`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct HudEvents {
    /// `(event id, handler)`, ordered by the id as a signed number, equal
    /// ids in registration order (the C++'s multimap).
    registry: Vec<(u32, NodeId)>,
    /// The handler that alone receives mode-1 events.
    focus: Option<NodeId>,
    /// The foci pushed aside, oldest first.
    focus_stack: Vec<NodeId>,
    /// `HudCommandRecord()`: what the handler of an event reads.
    pub record: HudRecord,
}

impl HudEvents {
    /// `HudRegisterCommandHandler(handler, msg)`: after every entry whose id
    /// is not above `msg` (signed); the same pair may be registered twice.
    pub fn register(&mut self, handler: NodeId, msg: u32) {
        let at = self
            .registry
            .iter()
            .position(|&(m, _)| m.cast_signed() > msg.cast_signed())
            .unwrap_or(self.registry.len());
        self.registry.insert(at, (msg, handler));
    }

    /// `HudUnregisterCommand(handler, msg)`: every entry of the pair.
    pub fn unregister(&mut self, handler: NodeId, msg: u32) {
        self.registry.retain(|&(m, h)| m != msg || h != handler);
    }

    /// `HudUnregisterCommandHandler(handler)`: every entry of the handler.
    pub fn unregister_all(&mut self, handler: NodeId) {
        self.registry.retain(|&(_, h)| h != handler);
    }

    /// `HudGetInputFocus`.
    #[must_use]
    pub fn input_focus(&self) -> Option<NodeId> {
        self.focus
    }

    /// `HudPushInputFocus(handler)`: the current focus pushed aside.
    pub fn push_input_focus(&mut self, handler: NodeId) {
        if let Some(current) = self.focus {
            self.focus_stack.push(current);
        }
        self.focus = Some(handler);
    }

    /// `HudPopInputFocus(handler)`: the focus back to the last one pushed
    /// aside when the handler has it, else its first entry of the stack
    /// dropped.
    pub fn pop_input_focus(&mut self, handler: NodeId) {
        if self.focus == Some(handler) {
            self.focus = self.focus_stack.pop();
        } else if let Some(at) = self.focus_stack.iter().position(|&h| h == handler) {
            self.focus_stack.remove(at);
        }
    }

    /// The handlers `HudDispatchCommand(msg, mode)` calls, in registry
    /// order (it calls them last first); in mode 1 only the input focus,
    /// when there is one.
    #[must_use]
    pub fn handlers(&self, msg: u32, mode: i32) -> Vec<NodeId> {
        self.registry
            .iter()
            .filter(|&&(m, h)| m == msg && (mode != 1 || self.focus.is_none_or(|f| f == h)))
            .map(|&(_, h)| h)
            .collect()
    }

    /// No handler is registered (`G_HudCmdRegistryPtr` is null once the
    /// last handler unregistered all its events, or before the first).
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.registry.is_empty()
    }
}

/// `Event::Handler::OnEvent` (slot 0) of the handler at `node`: the ported
/// screens' and the rich-text edit box's; false for any other node.
fn on_event(ui: &mut Ui, node: NodeId, msg: u32, record: &HudRecord) -> bool {
    if let Some(column) = ui.widget_mut::<TypeSelect>(node) {
        return column.on_event(msg);
    }
    if let Some(page) = ui.widget_mut::<PageCharacterSelectSingle>(node) {
        return page.on_event(msg);
    }
    if let Some(page) = ui.widget_mut::<PageOptions>(node) {
        return page.on_event(msg);
    }
    ui.with_widget(node, false, |w, ui| {
        w.as_any_mut()
            .downcast_mut::<RichTextEditWin>()
            .is_some_and(|edit| edit.on_event(ui, node, msg, record))
    })
}

/// `HudDispatchCommand(msg, mode)`: false without a registered handler;
/// else the event's handlers, the last registered first, until one takes
/// it. The cancel stack, which mode 1's `USER_CANCEL` would pop next, is
/// not ported (D1113): no ported screen pushes to it.
pub fn dispatch(ui: &mut Ui, events: &RefCell<HudEvents>, msg: u32, mode: i32) -> bool {
    let (handlers, record) = {
        let events = events.borrow();
        if events.is_empty() {
            return false;
        }
        (events.handlers(msg, mode), events.record)
    };
    handlers
        .into_iter()
        .rev()
        .any(|handler| on_event(ui, handler, msg, &record))
}

/// The services the context installs on its interface: the given ones,
/// but the input focus and the event registrations, which go to the
/// context's [`HudEvents`].
pub struct EventServices {
    /// The services the interface had.
    pub inner: Box<dyn UiServices>,
    /// The context's registry.
    pub events: Rc<RefCell<HudEvents>>,
}

impl UiServices for EventServices {
    fn find_ui_sound_slot(&mut self, name: &[u8]) -> i32 {
        self.inner.find_ui_sound_slot(name)
    }
    fn play_or_rewind_ui_sound_slot(&mut self, slot: u32) {
        self.inner.play_or_rewind_ui_sound_slot(slot);
    }
    fn play_ui_sound_slot(&mut self, slot: u32) {
        self.inner.play_ui_sound_slot(slot);
    }
    fn play_ui_sound_slot_flag1(&mut self, slot: u32) {
        self.inner.play_ui_sound_slot_flag1(slot);
    }
    fn stop_ui_sound_slot(&mut self, slot: u32) {
        self.inner.stop_ui_sound_slot(slot);
    }
    fn play_ui_rollover_sound(&mut self) {
        self.inner.play_ui_rollover_sound();
    }
    fn play_sound_arch(&mut self, name: &[u8]) {
        self.inner.play_sound_arch(name);
    }
    fn post_command(&mut self, command: u32) {
        self.inner.post_command(command);
    }
    fn montage_tile(&mut self, path: &[u8]) -> Option<MontageTile> {
        self.inner.montage_tile(path)
    }
    fn perf_option(&mut self, name: &[u8]) -> f32 {
        self.inner.perf_option(name)
    }
    fn key_binding_name(&mut self, command: i32) -> Option<Vec<u16>> {
        self.inner.key_binding_name(command)
    }
    fn set_system_cursor(&mut self, cursor: SystemCursor) {
        self.inner.set_system_cursor(cursor);
    }
    fn skip_local_server_tick(&mut self) -> bool {
        self.inner.skip_local_server_tick()
    }
    fn mouse_buttons(&mut self) -> u32 {
        self.inner.mouse_buttons()
    }
    fn letterbox_visible(&mut self) -> bool {
        self.inner.letterbox_visible()
    }
    fn intro_environment(&mut self) -> bool {
        self.inner.intro_environment()
    }
    fn knowledge_map_reveal(&mut self, ids: u32) {
        self.inner.knowledge_map_reveal(ids);
    }
    fn data_file(&mut self, path: &[u8]) -> Option<Vec<u8>> {
        self.inner.data_file(path)
    }
    fn elapsed_time(&mut self) -> f64 {
        self.inner.elapsed_time()
    }
    fn mouse_set_cursor(&mut self, name: &[u8]) {
        self.inner.mouse_set_cursor(name);
    }
    fn input_focus(&mut self) -> Option<NodeId> {
        self.events.borrow().input_focus()
    }
    fn push_input_focus(&mut self, node: NodeId) {
        self.events.borrow_mut().push_input_focus(node);
    }
    fn pop_input_focus(&mut self, node: NodeId) {
        self.events.borrow_mut().pop_input_focus(node);
    }
    fn register_event(&mut self, node: NodeId, msg: u32) {
        self.events.borrow_mut().register(node, msg);
    }
    fn unregister_event(&mut self, node: NodeId, msg: u32) {
        self.events.borrow_mut().unregister(node, msg);
    }
    fn create_ime_box(&mut self, owner: NodeId) -> Option<Box<dyn ImeBox>> {
        self.inner.create_ime_box(owner)
    }
    fn clear_ime_focus(&mut self) {
        self.inner.clear_ime_focus();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use neuron_client::ui::widescreen::Surface;

    #[test]
    fn equal_ids_keep_their_order_and_the_focus_filters_mode_one() {
        let mut ui = Ui::new(Surface::new(1024, 768));
        let [a, b, c] = [(); 3].map(|()| ui.alloc(None));
        let mut events = HudEvents::default();
        events.register(a, EVENT_CANCEL);
        events.register(b, 1);
        events.register(c, EVENT_CANCEL);
        events.register(b, 0xffff_ffff);
        assert_eq!(
            events.registry,
            [(0xffff_ffff, b), (1, b), (15, a), (15, c)]
        );
        assert_eq!(events.handlers(EVENT_CANCEL, 1), [a, c]);
        events.push_input_focus(c);
        events.push_input_focus(a);
        assert_eq!(events.handlers(EVENT_CANCEL, 1), [a]);
        assert_eq!(events.handlers(EVENT_CANCEL, 0), [a, c]);
        events.pop_input_focus(c);
        events.pop_input_focus(a);
        assert_eq!(events.input_focus(), None);
        events.unregister(c, EVENT_CANCEL);
        events.unregister_all(b);
        assert_eq!(events.registry, [(15, a)]);
    }

    #[test]
    fn a_dispatch_without_handlers_is_not_taken() {
        let mut ui = Ui::new(Surface::new(1024, 768));
        let events = RefCell::new(HudEvents::default());
        assert!(!dispatch(&mut ui, &events, EVENT_CANCEL, 1));
        let node = ui.alloc(None);
        events.borrow_mut().register(node, EVENT_CANCEL);
        assert!(!dispatch(&mut ui, &events, EVENT_CANCEL, 1));
    }
}
