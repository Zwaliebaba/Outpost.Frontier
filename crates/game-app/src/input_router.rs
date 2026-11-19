//! GameApp's input router (work package P4.11): the routing of
//! `PumpMessages` (`message_pump.cpp`) into the engine.
//!
//! Each frame starts with the message pump. It first hands out the mouse
//! events the last frame's `MouseFrameUpdate` queued (`MouseDispatchEvents`:
//! a move posts the Win3D handler's move, a button or a wheel notch goes
//! through `KeyMapRouteMouseEvent` to the handler's button and wheel
//! events, at the cursor and with the wheel delta the update left), then
//! offers every key and character message to `KeyMapRouteInputMessage`,
//! whose handler tree ends in the Win3D input handler: a message the
//! interface takes is neither translated nor dispatched. The IME's result
//! characters go to the focused edit box (`GSendChar`, `IMEFocus::OnChar`)
//! and the window's focus changes to the boxes' focus manager
//! (`IMEFocus::OnSetFocus`, `OnKillFocus`). The frame then runs the
//! interface and ends with the mouse's frame update
//! ([`neuron_client::engine::Engine::frame_with_mouse`]).
//!
//! The key map itself (`KeyMap.ini`, the accelerator groups, held keys and
//! the game's handlers ahead of the interface) and the main button's use
//! of an unclaimed left press are the game's flow (P5.2); until then the
//! interface is the only handler.

use neuron_client::engine::Engine;
use neuron_client::input::keyboard::KeyEvent;
use neuron_client::input::mouse::MouseEvent;
use neuron_client::render::ff::device::Backend;
use neuron_client::text_input::ime::ImeContext;
use neuron_client::ui::message::Input;
use neuron_client::window::WindowEvent;

/// Where the router delivers: the interface and the edit boxes.
pub trait InputTarget {
    /// The Win3D input handler's event; whether the interface took it.
    fn ui_input(&mut self, input: Input) -> bool;
    /// `IMEFocus::OnChar`: a unit for the focused edit box.
    fn ime_char(&mut self, unit: u16, ime: &mut dyn ImeContext) -> bool;
    /// `IMEFocus::OnSetFocus` (`gained`) or `OnKillFocus`.
    fn window_focus(&mut self, gained: bool, ime: &mut dyn ImeContext);
}

/// The engine's boxes use the IME context the engine was given (the same
/// window's as the router's, `Engine::set_ime_context`); a box whose text
/// changed outside its window's call writes it into the window after the
/// call.
impl<B: Backend + 'static> InputTarget for Engine<B> {
    fn ui_input(&mut self, input: Input) -> bool {
        let taken = self.ui_mut().is_some_and(|ui| ui.handle(input));
        self.apply_ime_texts();
        taken
    }

    fn ime_char(&mut self, unit: u16, _ime: &mut dyn ImeContext) -> bool {
        Engine::ime_char(self, unit)
    }

    fn window_focus(&mut self, gained: bool, _ime: &mut dyn ImeContext) {
        self.ime_window_focus(gained);
    }
}

/// The router of one pump.
pub struct GameRouter<'a> {
    target: &'a mut dyn InputTarget,
    ime: &'a mut dyn ImeContext,
    cursor: (i32, i32),
    wheel: i32,
    /// Events and messages the interface took.
    pub taken: usize,
}

impl<'a> GameRouter<'a> {
    /// A router into `target` with the window's IME, at the mouse's
    /// software cursor and frame wheel delta as the last frame update left
    /// them.
    pub fn new(
        target: &'a mut dyn InputTarget,
        ime: &'a mut dyn ImeContext,
        cursor: (i32, i32),
        wheel: i32,
    ) -> Self {
        GameRouter {
            target,
            ime,
            cursor,
            wheel,
            taken: 0,
        }
    }

    /// `MouseDispatchEvents`: one queued mouse event.
    pub fn mouse_event(&mut self, event: MouseEvent) {
        if let Some(input) = Input::from_mouse_event(event, self.cursor, self.wheel)
            && self.target.ui_input(input)
        {
            self.taken += 1;
        }
    }

    /// `KeyMapRouteInputMessage`: a key or character message before
    /// translation; `true` when the interface took it.
    pub fn key_event(&mut self, event: &KeyEvent) -> bool {
        let taken = self.target.ui_input(Input::from_key_event(event));
        if taken {
            self.taken += 1;
        }
        taken
    }

    /// `GSendChar`: a unit of the IME's result string.
    pub fn ime_char(&mut self, unit: u16) {
        self.target.ime_char(unit, self.ime);
    }

    /// The window events the edit boxes follow: the keyboard focus.
    pub fn window_event(&mut self, event: &WindowEvent) {
        match event {
            WindowEvent::FocusGained => self.target.window_focus(true, self.ime),
            WindowEvent::FocusLost => self.target.window_focus(false, self.ime),
            WindowEvent::Mouse(e) => self.mouse_event(*e),
            WindowEvent::ImeChar(unit) => self.ime_char(*unit),
            _ => {}
        }
    }
}

#[cfg(windows)]
impl neuron_client::platform::window::InputRouter for GameRouter<'_> {
    fn mouse_event(&mut self, event: MouseEvent) {
        GameRouter::mouse_event(self, event);
    }

    fn key_event(&mut self, event: &KeyEvent) -> bool {
        GameRouter::key_event(self, event)
    }

    fn ime_char(&mut self, unit: u16) {
        GameRouter::ime_char(self, unit);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use neuron_client::input::keyboard::{KeyEventKind, Modifiers, WM_CHAR, WM_KEYDOWN};
    use neuron_client::input::mouse::WHEEL_BUTTON;
    use neuron_client::text_input::ime::NoIme;

    #[derive(Default)]
    struct Recorder {
        inputs: Vec<Input>,
        chars: Vec<u16>,
        focus: Vec<bool>,
        take_keys: bool,
    }

    impl InputTarget for Recorder {
        fn ui_input(&mut self, input: Input) -> bool {
            self.inputs.push(input);
            matches!(input, Input::Key { .. }) && self.take_keys
        }
        fn ime_char(&mut self, unit: u16, _: &mut dyn ImeContext) -> bool {
            self.chars.push(unit);
            true
        }
        fn window_focus(&mut self, gained: bool, _: &mut dyn ImeContext) {
            self.focus.push(gained);
        }
    }

    #[test]
    fn events_reach_the_interface_in_the_pump_order() {
        let mut target = Recorder {
            take_keys: true,
            ..Recorder::default()
        };
        let mut ime = NoIme;
        let mut router = GameRouter::new(&mut target, &mut ime, (12, 34), -2);
        router.mouse_event(MouseEvent::Moved);
        router.mouse_event(MouseEvent::Button {
            index: WHEEL_BUTTON,
            down: true,
            double_click: false,
        });
        router.mouse_event(MouseEvent::Button {
            index: 9,
            down: true,
            double_click: false,
        });
        let key = KeyEvent::from_message(WM_KEYDOWN, 0x41, 0x001e_0001, Modifiers::NONE).unwrap();
        assert_eq!(key.kind, KeyEventKind::Key);
        assert!(router.key_event(&key));
        let ch = KeyEvent::from_message(WM_CHAR, 0x61, 0x001e_0001, Modifiers::NONE).unwrap();
        assert!(!router.key_event(&ch));
        router.ime_char(0x3042);
        router.window_event(&WindowEvent::FocusLost);
        router.window_event(&WindowEvent::FocusGained);
        assert_eq!(router.taken, 1);
        assert_eq!(target.inputs[0], Input::Move { x: 12, y: 34 });
        assert_eq!(target.inputs[1], Input::Wheel(-2));
        // A button past 7 that is not the wheel posts nothing.
        assert!(matches!(target.inputs[2], Input::Key { key: 0x41, .. }));
        assert!(matches!(target.inputs[3], Input::Char { wparam: 0x61, .. }));
        assert_eq!(target.chars, [0x3042]);
        assert_eq!(target.focus, [false, true]);
    }
}
