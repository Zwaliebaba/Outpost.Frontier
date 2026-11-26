//! `WinControl`'s child-construction helpers (`wincontrol_children.cpp`):
//! each makes a child of `parent` through the factory by its class name
//! and, when the factory gives one, calls that control's `Init` with the
//! remaining arguments. They answer the factory's result either way.
//!
//! The ones the game's screens need so far; the scroll, cycle, edit,
//! toggle and plain wire helpers follow with their screens.

use neuron_core::math::Vector;

use super::controls::button::ButtonWidget;
use super::controls::rich_text_win::RichTextWin;
use super::controls::static_control::StaticControlWidget;
use super::controls::text_control::TextControlWidget;
use super::controls::wire::WireWidget;
use super::tree::{NodeId, Payload, Ui, Widget};

fn make(ui: &mut Ui, name: &[u8], class: &[u8], parent: NodeId) -> Option<NodeId> {
    ui.create_by_class(Some(name), class, Some(parent), &mut Payload::None)
}

/// Runs `f` on the node's widget as a `T`, if it is one.
fn with<T: Widget>(ui: &mut Ui, id: NodeId, f: impl FnOnce(&mut T, &mut Ui)) {
    ui.with_widget(id, (), |w, ui| {
        if let Some(t) = w.as_any_mut().downcast_mut::<T>() {
            f(t, ui);
        }
    });
}

/// The arguments of `ButtonControl::Init` after the mesh path.
#[derive(Clone, Debug, Default)]
pub struct ButtonInit<'a> {
    /// `+0x350`, the second mesh.
    pub mesh2: Option<&'a [u8]>,
    pub sound: Option<&'a [u8]>,
    pub key_command_name: Option<&'a [u8]>,
    pub position: Vector,
    pub hardpoint: Option<&'a [u8]>,
    pub half_extents: Vector,
    /// `+0x331`.
    pub no_mesh_render: u8,
    /// `+0x330` bit 3.
    pub action_button: u8,
    /// `+0x330` bit 5.
    pub no_color_change: u8,
    pub no_z_enable: bool,
    pub tooltip: i32,
    /// `+0x32c`.
    pub tooltip2: i32,
    /// `+0x34c`.
    pub command: i32,
}

/// `WinControl::CreateButtonControl`.
pub fn create_button_control(
    ui: &mut Ui,
    parent: NodeId,
    name: &[u8],
    mesh: Option<&[u8]>,
    init: &ButtonInit<'_>,
) -> Option<NodeId> {
    let id = make(ui, name, b"ButtonControl", parent)?;
    with::<ButtonWidget>(ui, id, |b, ui| {
        b.button.init(
            ui,
            id,
            mesh,
            init.mesh2,
            init.sound,
            init.key_command_name,
            init.position,
            init.hardpoint,
            init.half_extents,
            init.no_mesh_render,
            init.action_button,
            init.no_color_change,
            init.no_z_enable,
            init.tooltip,
            init.tooltip2,
            init.command,
        );
    });
    Some(id)
}

/// The arguments of `StaticControl::Init` after the mesh path.
#[derive(Clone, Debug, Default)]
pub struct StaticInit<'a> {
    pub offset: Vector,
    pub hardpoint: Option<&'a [u8]>,
    pub half_extents: Vector,
    pub no_z_enable: bool,
    pub zclear: bool,
    pub no_mesh_render: bool,
    pub no_wire_render: bool,
    pub no_coloring: bool,
    pub render_to_window: bool,
    pub no_clear_render: bool,
    pub force_radius: i32,
    pub rotation_speed: i32,
    pub enable_rotation: bool,
    pub start_ypr: [f32; 3],
}

/// `NnStaticControl::Init` on an existing static control.
pub fn init_static_control(ui: &mut Ui, id: NodeId, mesh: Option<&[u8]>, init: &StaticInit<'_>) {
    with::<StaticControlWidget>(ui, id, |s, ui| {
        s.control.init(
            ui,
            id,
            mesh,
            init.offset,
            init.hardpoint,
            init.half_extents,
            init.no_z_enable,
            init.zclear,
            init.no_mesh_render,
            init.no_wire_render,
            init.no_coloring,
            init.render_to_window,
            init.no_clear_render,
            init.force_radius,
            init.rotation_speed,
            init.enable_rotation,
            init.start_ypr,
        );
    });
}

/// `WinControl::CreateStaticControl`.
pub fn create_static_control(
    ui: &mut Ui,
    parent: NodeId,
    name: &[u8],
    mesh: Option<&[u8]>,
    init: &StaticInit<'_>,
) -> Option<NodeId> {
    let id = make(ui, name, b"StaticControl", parent)?;
    init_static_control(ui, id, mesh, init);
    Some(id)
}

/// The arguments of `TextControl::Init` (`Arg6`, which the port's
/// `TextControl::init` does not read, left out).
#[derive(Clone, Debug, Default)]
pub struct TextInit<'a> {
    pub style: Option<&'a [u8]>,
    pub position: Vector,
    pub hardpoint: Option<&'a [u8]>,
    pub width: f32,
    pub height: f32,
    pub ids: u32,
    /// `+0x358`.
    pub justification: i32,
    /// `+0x344`.
    pub tooltip: i32,
    /// `+0x378`.
    pub corner_anchor: bool,
}

/// `TextControl::Init` on an existing text control.
pub fn init_text_control(ui: &mut Ui, id: NodeId, init: &TextInit<'_>) {
    with::<TextControlWidget>(ui, id, |t, ui| {
        t.text.init(
            ui,
            id,
            init.style,
            init.position,
            init.hardpoint,
            init.width,
            init.height,
            init.ids,
            init.justification,
            init.tooltip,
            init.corner_anchor,
        );
    });
}

/// `WinControl::CreateTextControl`.
pub fn create_text_control(
    ui: &mut Ui,
    parent: NodeId,
    name: &[u8],
    init: &TextInit<'_>,
) -> Option<NodeId> {
    let id = make(ui, name, b"TextControl", parent)?;
    init_text_control(ui, id, init);
    Some(id)
}

/// `WinControl::CreateRichTextWin`: the size, the attach offset and the
/// scroll bar setting. The reference does not test the factory's result;
/// the port answers `None` then.
pub fn create_rich_text_win(
    ui: &mut Ui,
    parent: NodeId,
    name: &[u8],
    width: f32,
    height: f32,
    attach_offset: Vector,
    auto_scrollbar: bool,
) -> Option<NodeId> {
    let id = make(ui, name, b"RichTextWin", parent)?;
    with::<RichTextWin>(ui, id, |r, ui| {
        r.base.set_norm_size_in(ui, id, width, height);
        r.base.set_attach_offset(ui, id, attach_offset);
        r.base.set_auto_scrollbar(auto_scrollbar);
    });
    Some(id)
}

/// The arguments of `RichTextWireButtonControl::Init`.
#[derive(Clone, Debug, Default)]
pub struct WireButtonInit<'a> {
    pub sound: Option<&'a [u8]>,
    pub position: Vector,
    pub half_extents: Vector,
    /// `bFlag3`: always white.
    pub always_white: u8,
    pub no_z_enable: bool,
    pub tooltip: i32,
    pub caption_ids: u32,
    pub command: i32,
    pub edge_mask: u32,
}

/// `WinControl::CreateRichTextWireButtonControl`.
pub fn create_rich_text_wire_button_control(
    ui: &mut Ui,
    parent: NodeId,
    name: &[u8],
    init: &WireButtonInit<'_>,
) -> Option<NodeId> {
    let id = make(ui, name, b"RichTextWireButtonControl", parent)?;
    with::<WireWidget>(ui, id, |w, ui| {
        w.wire.init_rich_text(
            ui,
            id,
            init.sound,
            init.position,
            init.half_extents,
            init.always_white,
            init.no_z_enable,
            init.tooltip,
            init.caption_ids,
            init.command,
            init.edge_mask,
        );
    });
    Some(id)
}
