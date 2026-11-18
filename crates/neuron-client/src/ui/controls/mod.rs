//! The toolkit's concrete controls. So far: `TextWin` and `UIStaticText`
//! ([`text_win`]). The other generic controls of
//! [`super::factory::CLASSES`] are not ported yet.

pub mod text_win;

use super::factory::Factory;

/// Registers the toolkit's controls.
pub fn register(factory: &mut Factory) {
    factory.register("UIStaticText", || {
        Box::new(text_win::UiStaticText::default())
    });
}
