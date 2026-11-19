//! The toolkit's concrete controls: `TextWin` and `UIStaticText`
//! ([`text_win`]), `BaseRichTextWin` and `RichTextWin` ([`rich_text_win`]),
//! `ListBox` and `UIListBox` ([`list_box`]), `UIScroll` and `UICycle`
//! ([`scroll`]), `WinObject` and `UIWinObject` ([`win_object`]),
//! `TextControl` and `EditControl` ([`text_control`]), `StaticControl`
//! ([`static_control`]), `ButtonControl`, `ToggleButtonControl` and
//! `RichTextButtonControl` ([`button`]), the wire controls ([`wire`]),
//! `ScrollControl` ([`scroll_control`]), `UITable` ([`table`]), `ToolTip`
//! and `Rollover` ([`tooltip`]), `UIRichTextWin` and `UITextMsgButton`
//! ([`ui_rich_text`]). The other classes of [`super::factory::CLASSES`]
//! (the editable and table rich-text windows and the game's) are not
//! ported yet.

pub mod button;
pub mod list_box;
pub mod rich_text_win;
pub mod scroll;
pub mod scroll_control;
pub mod static_control;
pub mod table;
pub mod text_control;
pub mod text_win;
pub mod tooltip;
pub mod ui_rich_text;
pub mod win_object;
pub mod wire;

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
    factory.register("TextControl", || {
        Box::new(text_control::TextControlWidget::default())
    });
    factory.register("EditControl", || {
        Box::new(text_control::EditControl::default())
    });
    factory.register("StaticControl", || {
        Box::new(static_control::StaticControlWidget::default())
    });
    factory.register(
        "ButtonControl",
        || Box::new(button::ButtonWidget::default()),
    );
    factory.register("ToggleButtonControl", || {
        Box::new(button::ToggleButtonWidget::default())
    });
    factory.register("RichTextButtonControl", || {
        Box::new(button::RichTextButtonWidget::default())
    });
    factory.register(
        "WireStaticControl",
        || Box::new(wire::WireWidget::default()),
    );
    factory.register("WireButtonControl", || Box::new(wire::WireWidget::button()));
    factory.register("RichTextWireButtonControl", || {
        Box::new(wire::WireWidget::button())
    });
    factory.register("ScrollControl", || {
        Box::new(scroll_control::ScrollControlWidget::default())
    });
    factory.register("UITable", || Box::new(table::UiTable::default()));
    factory.register("UIRichTextWin", || {
        Box::new(ui_rich_text::UiRichTextWin::default())
    });
    factory.register("UITextMsgButton", || {
        Box::new(ui_rich_text::UiTextMsgButton::default())
    });
    factory.register("ToolTip", || Box::new(tooltip::ToolTip::default()));
    factory.register("Rollover", || Box::new(tooltip::Rollover::default()));
}
