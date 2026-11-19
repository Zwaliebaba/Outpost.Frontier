//! The toolkit's concrete controls: `TextWin` and `UIStaticText`
//! ([`text_win`]), `BaseRichTextWin` and `RichTextWin` ([`rich_text_win`]),
//! `ListBox` and `UIListBox` ([`list_box`]), `UIScroll` and `UICycle`
//! ([`scroll`]), `WinObject` and `UIWinObject` ([`win_object`]). The other
//! generic controls of [`super::factory::CLASSES`] are not ported yet.

pub mod list_box;
pub mod rich_text_win;
pub mod scroll;
pub mod text_win;
pub mod win_object;

use super::factory::Factory;

/// Registers the toolkit's controls.
pub fn register(factory: &mut Factory) {
    factory.register("UIStaticText", || {
        Box::new(text_win::UiStaticText::default())
    });
    factory.register("RichTextWin", || {
        Box::new(rich_text_win::RichTextWin::default())
    });
    factory.register("UIListBox", || Box::new(list_box::UiListBox::default()));
    factory.register("UIScroll", || Box::new(scroll::UiScroll::new(false)));
    factory.register("UICycle", || Box::new(scroll::UiScroll::new(true)));
    factory.register("UIWinObject", || {
        Box::new(win_object::UiWinObject::default())
    });
}
