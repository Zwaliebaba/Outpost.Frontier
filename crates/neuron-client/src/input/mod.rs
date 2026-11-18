//! Client input mechanics (work package P4.11): the mouse model of the
//! C++ client's `input_manager.cpp` fed by Raw Input instead of
//! DirectInput 8, the decoding of Raw Input mouse reports, and the typed
//! key messages the message pump routes before `TranslateMessage`.
//!
//! Everything here is platform-free. `platform::input` registers the game
//! window for Raw Input, reads the reports and implements
//! [`mouse::MousePlatform`] with the cursor calls; `platform::window`
//! drives the model from the window procedure as the C++ does.
//!
//! The game's key bindings (`KeyMap.ini`, the accelerator groups, the
//! held-key list and the handler tree of `KeyMapRouteInputMessage`) are
//! GameApp's (P5.2): the pump hands each [`keyboard::KeyEvent`] to the
//! game's router, which says whether it consumed it.

pub mod keyboard;
pub mod mouse;
pub mod raw;
