//! The toolkit's concrete controls. So far: `TextWin` and `UIStaticText`
//! ([`text_win`]), `BaseRichTextWin` and `RichTextWin`
//! ([`rich_text_win`]). The other generic controls of
//! [`super::factory::CLASSES`] are not ported yet.

pub mod rich_text_win;
pub mod text_win;

use super::factory::Factory;

/// Registers the toolkit's controls.
pub fn register(factory: &mut Factory) {
    factory.register("UIStaticText", || {
        Box::new(text_win::UiStaticText::default())
    });
    factory.register("RichTextWin", || {
        Box::new(rich_text_win::RichTextWin::default())
    });
}
