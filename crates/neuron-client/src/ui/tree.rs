//! The control tree and its manager: `Win3D` and the rest of `win3d.cpp`.
//!
//! # The reference
//!
//! Every interface object derives from `Win3D`: a node of a parent, first
//! child and next sibling tree with a factory class name, a name of up to
//! 31 bytes, a 3x3 orientation and a position in the Win3D camera's space,
//! a flags byte (active, visible, unregistered, pickable), a draw layer, a
//! hit priority and a draw sub-layer, and 28 virtual slots. `Create`
//! appends the object to its parent's children and to a global registry (a
//! list in creation order); destroying it unregisters it: the registry
//! entry's payload is nulled at once and the entry erased at the end of the
//! next update. The manager keeps the keyboard focus, a stack of mouse
//! captures (an intrusive list through each object), the object under the
//! mouse and the mouse ray.
//!
//! * Picking walks the registry for active, visible, pickable objects whose
//!   ancestors are all visible, and keeps the deepest (or the explicit hit
//!   priority) that the ray hits; equal depths go to the larger z.
//! * Mouse buttons go first through the capture stack (the pick inside each
//!   capture, then the capture itself), then to the global pick; a slot 19
//!   that does not take the event hands it to the parent.
//! * Keys go to the focus, else to every registered object in order until
//!   one takes them.
//! * Each frame the hover object is picked again and every root updated
//!   (only the captured `WinControl` while a modal dialog counts up
//!   `G_ModalCaptureCount`, the modal stack).
//! * Drawing sorts the visible roots by layer and z and draws them in eight
//!   sub-layer passes; layer 0 draws at once; children sort the same way
//!   without the sub-layer passes.
//!
//! # The port
//!
//! Nodes live in an arena with generational [`NodeId`]s inside one [`Ui`]
//! value (decision T10: no process globals). Each node may hold a
//! [`Widget`], the trait standing for the virtual slots; its default
//! methods are `Win3D`'s bodies, which overriding widgets reach through
//! [`Ui`]'s `base_*` methods. A widget is taken out of its node while one
//! of its methods runs, so it may change the tree freely; a call that
//! reaches the same widget again finds no widget and does nothing (the
//! reference recurses). The capture stack is a vector (divergence D340).
//! Widescreen placement is data on the nodes ([`Anchor`]), set by the
//! game (divergence D343).

use std::any::Any;

use neuron_core::math::{Matrix, Transform, Vector};

use super::camera::Camera;
use super::control::{ControlMesh, WinControl};
use super::factory::Factory;
use super::immediate::Immediate;
use super::message::{Input, KeyMods, MouseButton};
use super::pulse::Pulse;
use super::quad_batch::QuadBatcher;
use super::shape::{ShapeTextures, TexShape};
use super::widescreen::{Anchor, Shift, Surface, cursor_ray, shift_pixels};
use crate::text::font::FontSystem;
use crate::text::font::string::PaintCx;

/// `kWin3DActive`: updated with `Update`, else `UpdateInactive`.
pub const ACTIVE: u8 = 0x01;
/// `kWin3DVisible`.
pub const VISIBLE: u8 = 0x02;
/// `kWin3DUnregistered`: set when the object leaves the registry.
pub const UNREGISTERED: u8 = 0x04;
/// `kWin3DPickable`.
pub const PICKABLE: u8 = 0x08;

/// The capacity of the sort arrays of the draws (0x400 objects).
pub const DRAW_SORT_CAPACITY: usize = 0x400;

/// A node of the tree. Stale ids (of deleted nodes) find nothing.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct NodeId {
    index: u32,
    generation: u32,
}

/// What a factory hands a new object's `Create` (the reference's `void*`).
pub enum Payload<'a> {
    None,
    /// The INI reader of a `[BaseFrame]` file, at the child's section.
    Ini(&'a mut neuron_core::ini::IniReader),
    /// A page descriptor entry's payload.
    Page(&'a super::page::EntryPayload),
}

/// What draws see: the screen-quad batcher, what string paints need, and
/// the calls the manager makes around the draw (`Win3DBeginDraw` and the
/// shift scopes).
pub trait DrawHost {
    fn quads(&mut self) -> &mut QuadBatcher;
    /// The texture pipeline and quad sink of string paints (the sink is
    /// normally the host's batcher), when text can be drawn.
    fn paint_cx(&mut self) -> Option<PaintCx<'_>> {
        None
    }
    /// `GetGameOption(INTERFACE)`: interface text is drawn only when set.
    fn interface_enabled(&self) -> bool {
        true
    }
    /// `Win3DBeginDraw`, the immediate batch reset and the button montage
    /// before the roots draw.
    fn begin_draw(&mut self, _camera: &Camera) {}
    /// The batch flushes after the roots drew.
    fn end_draw(&mut self) {}
    /// A widescreen shift scope changed the camera while drawing: the
    /// batches queued under the old camera go out and the new camera is
    /// handed to the pipeline (`FWidescreenHudShiftScope` with `bDraw`).
    fn camera_changed(&mut self, _camera: &Camera) {}
    /// What rich-text windows draw with (the pipeline's viewport and
    /// projection, textures and the big-image tile sink), when the host
    /// can draw them.
    fn rich_text_cx(&mut self) -> Option<crate::ui::controls::rich_text_win::RichTextCx<'_>> {
        None
    }
    /// The immediate-mode layer the control frames, arrows and rules are
    /// drawn with, when there is one.
    fn immediate(&mut self) -> Option<&mut dyn Immediate> {
        None
    }
    /// The texture library the shapes of control draws resolve their GPU
    /// handles in (`TexShapeGetGpuHandle`).
    fn shape_textures(&mut self) -> Option<&mut dyn ShapeTextures> {
        None
    }
    /// `GetGameOption(index)` for the other options the control draws
    /// read (6 the interface, 9 NeuroNet, 0x13 text controls, 0x14 button
    /// controls, 0x15 static controls).
    fn game_option(&self, _index: u32) -> i32 {
        1
    }
    /// `GetGameOption(DRAW SCROLL BAR CONTROLS)` (option 0x16).
    fn scroll_bars_enabled(&self) -> bool {
        true
    }
    /// Submits a control's engine instance (`SubmitWin3DEngineInstance`
    /// and its tinted variant) with the camera the draw runs under.
    fn submit_mesh(&mut self, _mesh: &dyn ControlMesh, _pass: MeshPass) {}
    /// A static control draws into a window of the screen through its own
    /// camera (`Some`), or goes back to the Win3D camera (`None`).
    fn window_view(&mut self, _view: Option<WindowView>) {}
    /// `EngineLightClearRegisteredList`: the engine's lights that the
    /// scene enabled switched off (a static control's draw).
    fn clear_engine_lights(&mut self) {}
    /// `NnMontageDrawTileAt(tile, where, rgba, size, 0, 0)`: whether the
    /// montage icon was drawn.
    fn draw_montage_tile(
        &mut self,
        _tile: Option<super::services::MontageTile>,
        _at: Vector,
        _rgba: [u8; 4],
        _size: f32,
    ) -> bool {
        false
    }
    /// For widgets that need more of their host.
    fn as_any_mut(&mut self) -> &mut dyn Any;
}

/// How a control's engine instance is submitted.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum MeshPass {
    /// `SubmitWin3DEngineInstance(holder, camera, 0, 1.0, 0)` after the
    /// render states `ZFUNC = 8` and `ZWRITEENABLE = 1`.
    Plain,
    /// `SubmitWin3DTintedEngineInstanceWrapped(camera, instance, 1, tint,
    /// lighting, 1.0, 0)`: front culling, the tint bytes R, G, B, A.
    Tinted { tint: [u8; 4], lighting: bool },
    /// `SubmitWin3DEngineInstance(holder, camera, front_cull, scale, 0)`
    /// after the holder's tint floats were set from the bytes R, G, B, A
    /// (each times 1/255).
    Body {
        tint: [u8; 4],
        front_cull: bool,
        scale: f32,
    },
    /// `SubmitWin3DTintedEngineInstanceWrapped(camera, instance,
    /// front_cull, tint, lighting, scale, 0)`.
    TintedScaled {
        tint: [u8; 4],
        front_cull: bool,
        lighting: bool,
        scale: f32,
    },
    /// `SubmitWin3DOutlinedEngineInstance(camera, instance, 1, wire,
    /// scale, 0)`.
    Outlined { wire: [u8; 4], scale: f32 },
    /// `IRenderer2::RenderInstance(camera, instance, 1.0, 0x30, scaled
    /// identity)` after `ZFUNC = 8` and `ZWRITEENABLE = 1` (a static
    /// control's z-clear pass).
    Direct { tint: [u8; 4], scale: f32 },
}

/// A static control's render-to-window view (`StaticControl::Draw`): the
/// screen rectangle its own camera is fitted to, with the Win3D camera's
/// planes and a 35 degree vertical field of view; `None` puts the Win3D
/// camera and the saved viewport back.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct WindowView {
    pub rect: [i32; 4],
    pub z_near: f32,
    pub z_far: f32,
    pub fov_y_deg: f32,
}

/// `GetTextureFrame(shape, 0)` of a shape through the host's library: 0
/// without a shape or a library.
pub fn shape_handle(host: &mut dyn DrawHost, shape: Option<&TexShape>) -> u32 {
    match (shape, host.shape_textures()) {
        (Some(shape), Some(lib)) => shape.gpu_handle(lib, 0),
        _ => 0,
    }
}

/// A [`DrawHost`] with only a batcher.
#[derive(Debug, Default)]
pub struct QuadHost {
    pub quads: QuadBatcher,
}

impl DrawHost for QuadHost {
    fn quads(&mut self) -> &mut QuadBatcher {
        &mut self.quads
    }

    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
}

/// The virtual slots of `Win3D` (and `WinControl`'s slot 42). Every default
/// is the `Win3D` body.
#[allow(unused_variables)]
pub trait Widget: Any {
    fn as_any(&self) -> &dyn Any;
    fn as_any_mut(&mut self) -> &mut dyn Any;

    /// Slot 2: the `WinControl` part, if any.
    fn win_control(&self) -> Option<&WinControl> {
        None
    }
    fn win_control_mut(&mut self) -> Option<&mut WinControl> {
        None
    }

    /// Slot 4: whether the ray hits the object.
    fn hit_test(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) -> bool {
        false
    }
    /// Slot 5.
    fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
        ui.base_set_position(id, position);
    }
    /// Slot 7.
    fn set_orientation(&mut self, ui: &mut Ui, id: NodeId, orientation: Matrix) {
        ui.base_set_orientation(id, orientation);
    }
    /// Slot 8: slot 7, then slot 5.
    fn set_transform(&mut self, ui: &mut Ui, id: NodeId, transform: &Transform) {
        self.set_orientation(ui, id, transform.rotation);
        self.set_position(ui, id, transform.translation);
    }
    /// Slot 9: reparent, register, active and visible.
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        payload: &mut Payload<'_>,
    ) -> bool {
        ui.base_create(id, parent)
    }
    /// Slot 10: inactive, invisible, every child destroyed, unregistered.
    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        ui.base_destroy(id);
    }
    /// The destructor chain, run before the node is freed (`~Win3D`).
    fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        ui.base_on_delete(id);
    }
    /// Slot 11.
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        ui.base_update(id);
    }
    /// Slot 12.
    fn update_inactive(&mut self, ui: &mut Ui, id: NodeId) {
        ui.base_update_inactive(id);
    }
    /// Slot 13.
    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        ui.draw_children(id, host);
    }
    /// Slot 14.
    fn mouse_over(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) {}
    /// Slot 15.
    fn mouse_enter(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) {}
    /// Slot 16.
    fn mouse_leave(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) {}
    /// Slot 18: the wheel.
    fn wheel(&mut self, ui: &mut Ui, id: NodeId, delta: i32) -> bool {
        false
    }
    /// Slot 19: a button event; the default hands it to the parent.
    fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        ui.base_mouse_event(id, event)
    }
    /// Slot 20: a key with the modifier byte, which the handler may change.
    fn key_event(&mut self, ui: &mut Ui, id: NodeId, key: u32, mods: &mut KeyMods) -> bool {
        false
    }
    /// Slot 22: a character message.
    fn char_event(&mut self, ui: &mut Ui, id: NodeId, wparam: u32, lparam: u32) -> bool {
        false
    }
    /// Slot 23: a broadcast message.
    fn broadcast(&mut self, ui: &mut Ui, id: NodeId, msg: u32, a: u32, b: u32) -> bool {
        false
    }
    /// Slot 23 with the messages controls send their parent, whose
    /// arguments are the sender and its data (divergence D430).
    fn child_message(&mut self, ui: &mut Ui, id: NodeId, message: &ChildMessage) -> bool {
        false
    }
    /// Slot 24: handed down to every child.
    fn notify(&mut self, ui: &mut Ui, id: NodeId, value: u32) {
        ui.base_notify(id, value);
    }
    /// Slot 34 (`ShowToolTip`, `WinControl` and its controls): the hover
    /// record fed to the tooltip when the control's hit test hits.
    fn show_tooltip(&self, ui: &mut Ui, id: NodeId) {
        if let Some(c) = self.win_control() {
            super::control::base::show_tooltip(ui, id, c);
        }
    }
    /// The tooltip singleton's `UpdateFromHover` ([`Ui::feed_tooltip_hover`]).
    fn tooltip_hover(&mut self, ui: &mut Ui, id: NodeId, hover: &super::services::ToolTipHover) {}
    /// The tooltip singleton's `HideNow` ([`Ui::hide_tooltip`]).
    fn tooltip_hide(&mut self, ui: &mut Ui, id: NodeId) {}
    /// Slot 27: the hit priority (0: the depth decides).
    fn hit_priority(&self, node_priority: i32) -> i32 {
        node_priority
    }
    /// `WinControl` slot 42, the property switch.
    fn property(&mut self, ui: &mut Ui, id: NodeId, property: Property<'_>) -> isize {
        0
    }
}

/// See [`Ui::ini_loaded`].
pub type IniLoadHook = Box<dyn FnMut(&Ui, NodeId, &[u8])>;

/// The messages of `WinControl::SetProperty` (slot 42) and those a child's
/// hit test sends its parent.
#[derive(Debug)]
pub enum Property<'a> {
    /// 1: hide (true) or show.
    Hidden(bool),
    /// 2: inactive (true) or active.
    Inactive(bool),
    /// 3.
    ToolTip(i32),
    /// 4: mesh and material paths.
    Mesh(&'a [u8], &'a [u8]),
    /// 5.
    Position(Vector),
    /// 6.
    Offset(Vector),
    /// 7: the offset written to the output.
    GetOffset(&'a mut Vector),
    /// 8: the instance's orientation.
    InstanceOrientation(Matrix),
    /// 9.
    Field2d2(bool),
    /// 10: whether there is a mesh instance (the reference returns it).
    Instance,
    /// 11: the mesh path, cut to the given length as `strncpy` does.
    GetMeshPath(&'a mut Vec<u8>, usize),
    /// 0x25: a child's hit test hit.
    ChildHit(NodeId),
    /// 0x4F: a child with a tooltip was hit; non-zero means the parent
    /// shows it.
    ChildToolTip(NodeId),
    /// 0x29: a mouse button other than a press reached a scroll bar.
    ChildButtonUp(NodeId),
    /// 0x37: a scroll bar's integer range.
    ScrollRange(i32, i32),
    /// 0x38: a scroll bar's value.
    ScrollValue(i32),
    /// 0x3B: the scroll bar's value, returned truncated.
    GetScrollValue,
    /// 0x3E: a scroll bar's value changed (the sender, its value
    /// truncated).
    ScrollChanged(NodeId, i32),
    /// A control telling its parent about itself with `SetProperty(msg,
    /// this, value)`: 0x1A and 0x1B (a static control clicked, right),
    /// 0x26 (activated), 0x27 (right click or double click), 0x28
    /// (pressed), 0x2A and 0x2B (the press left and came back), 0x2C and
    /// 0x2D (lit, unlit), 0x30 (a toggle's new state), 0x46 (a text
    /// control was hit). The sender is a node (divergence D430).
    FromChild { msg: i32, from: NodeId, value: i32 },
    /// 0x45: an edit control's text changed (`None` for the null text
    /// Backspace sends), with Enter.
    EditText {
        from: NodeId,
        text: Option<Vec<u16>>,
        enter: bool,
    },
    /// 0x1D: a text control's text.
    SetText(Option<&'a [u16]>),
    /// 0x33: a colour override (the bytes R, G, B, A), on when the flag
    /// is set (`B == 1`).
    Colour([u8; 4], bool),
    /// 0x18: a static control's mesh and wire colours.
    Colours([u8; 4], [u8; 4]),
    /// 0x2E: a button's key-map command written to the output.
    GetCommand(&'a mut i32),
    /// 0x2F: a toggle button's state written to the output.
    GetToggleState(&'a mut i32),
    /// 0x31: a toggle button's sounds of state 0 and 1 (`None` keeps one).
    ToggleSounds(Option<&'a [u8]>, Option<&'a [u8]>),
    /// Any other message.
    Other(i32, usize, usize),
}

/// The slot 23 messages a control sends its parent.
#[derive(Clone, Debug, PartialEq)]
pub enum ChildMessage {
    /// 0xF001 (a press) and 0xF002 (a double click): a list box row was
    /// picked, with its text (`None` for no row) and index.
    ListPick {
        from: NodeId,
        double_click: bool,
        text: Option<Vec<u16>>,
        index: i32,
    },
    /// 0xF005: a scroll bar's value changed; `value` is the bar's value
    /// (`GetValue`), which the parent cannot ask the bar for while the bar
    /// tells it (D347).
    ScrollChanged { from: NodeId, value: f32 },
    /// 0xF004: Enter in a rich-text edit box.
    EditEnter { from: NodeId },
    /// 0xF003: Escape in a rich-text edit box.
    EditEscape { from: NodeId },
    /// 0x300D: a fast table's selected row changed.
    TableSelect { from: NodeId, row: u32 },
    /// 0xF002: a double click on a fast table's selectable row.
    TableDoubleClick { from: NodeId, row: u32 },
    /// 0xF001: a press on a fast table that selected nothing, with the
    /// hovered cell as `(row << 16) | column`.
    TableCellPress { from: NodeId, cell: u32 },
}

/// A request a control made to show its tooltip (`ShowToolTip`: the
/// tooltip window is the game's).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ToolTipRequest {
    pub owner: NodeId,
    pub ids: i32,
}

/// The `Win3D` part of an object.
pub struct Node {
    /// The class name of the factory that made it.
    pub class_name: Option<&'static str>,
    name: Vec<u8>,
    parent: Option<NodeId>,
    children: Vec<NodeId>,
    pub orientation: Matrix,
    pub position: Vector,
    /// [`ACTIVE`], [`VISIBLE`], [`UNREGISTERED`], [`PICKABLE`].
    pub flags: u8,
    /// 0 draws at once, otherwise the sort key.
    pub draw_layer: u32,
    pub hit_priority: i32,
    /// 0 to 7: the root draw pass.
    pub draw_sub_layer: i32,
    /// Widescreen placement.
    pub anchor: Anchor,
    widget: Option<Box<dyn Widget>>,
    /// The widget is a `WinControl` (slot 2), known also while the widget
    /// is out of the node for one of its methods.
    is_control: bool,
}

impl Node {
    fn new(widget: Option<Box<dyn Widget>>) -> Node {
        let is_control = widget.as_ref().is_some_and(|w| w.win_control().is_some());
        Node {
            is_control,
            class_name: None,
            name: Vec::new(),
            parent: None,
            children: Vec::new(),
            orientation: Matrix::IDENTITY,
            position: Vector::ZERO,
            flags: PICKABLE,
            draw_layer: 0,
            hit_priority: 0,
            draw_sub_layer: 0,
            anchor: Anchor::Inherit,
            widget,
        }
    }

    #[must_use]
    pub fn name(&self) -> &[u8] {
        &self.name
    }

    /// `SetName`: up to 31 bytes, empty for `None`.
    pub fn set_name(&mut self, name: Option<&[u8]>) {
        let name = name.unwrap_or_default();
        let len = name
            .iter()
            .position(|&b| b == 0)
            .unwrap_or(name.len())
            .min(31);
        self.name = name[..len].to_vec();
    }

    #[must_use]
    pub fn parent(&self) -> Option<NodeId> {
        self.parent
    }

    /// The children in sibling order.
    #[must_use]
    pub fn children(&self) -> &[NodeId] {
        &self.children
    }

    #[must_use]
    pub fn is(&self, flag: u8) -> bool {
        self.flags & flag != 0
    }

    #[must_use]
    pub fn widget(&self) -> Option<&dyn Widget> {
        self.widget.as_deref()
    }

    pub fn widget_mut(&mut self) -> Option<&mut (dyn Widget + 'static)> {
        self.widget.as_deref_mut()
    }
}

struct Slot {
    generation: u32,
    node: Option<Node>,
}

/// The manager: the arena, the registry, the focus, the capture stack, the
/// hover object, the mouse ray and the Win3D camera.
pub struct Ui {
    slots: Vec<Slot>,
    free: Vec<u32>,
    /// Creation order; `None` for an entry unregistered since the last
    /// drain.
    registry: Vec<Option<NodeId>>,
    key_focus: Option<NodeId>,
    /// The capture stack, head first.
    capture: Vec<NodeId>,
    hover: Option<NodeId>,
    mouse_ray: Vector,
    /// The software cursor (`GetClientSoftwareCursor`).
    pub cursor: (i32, i32),
    /// Whether the software cursor is drawn (`GetClientMouseCursorDrawFlag`):
    /// the frame's pick runs only then.
    pub cursor_drawn: bool,
    /// `G_ModalCaptureCount`: while non-zero only the captured control is
    /// updated.
    pub modal_capture_count: i32,
    /// The Win3D camera.
    pub camera: Camera,
    shift: Shift,
    /// How many registry entries the last update walked.
    pub update_node_count: usize,
    tooltip_requests: Vec<ToolTipRequest>,
    /// Loads a control's mesh (`ClientMeshLoadFromPath`); none until the
    /// engine has meshes.
    pub mesh_loader: Option<Box<dyn super::control::MeshLoader>>,
    /// The control factories.
    pub factory: Factory,
    /// The client's hook on every `WinControl::LoadIni` that found
    /// `[BaseFrame]`, called after the children were built with the
    /// control and the reader's file name (`OracleOnWinControlIni`).
    pub ini_loaded: Option<IniLoadHook>,
    /// The client's fonts, which text controls lay out and release their
    /// strings with.
    pub fonts: Option<FontSystem>,
    /// The hover pulse.
    pub pulse: Pulse,
    /// `GetClientFrameTimeCapped`: the frame time the last update ran with.
    pub frame_time: f64,
    /// The shapes of `Interface\\UI\\UITextures.ini` the scroll bars draw
    /// with.
    pub ui_shapes: super::controls::scroll::ScrollShapes,
    /// The client's services the controls call (sounds, commands, the
    /// montage, options, cursors).
    pub services: Box<dyn super::services::UiServices>,
    /// The string tables the controls' ids resolve in.
    pub catalogs: Option<std::rc::Rc<neuron_core::catalog::TextCatalogs>>,
    /// `G_ToolTipSingleton`: the tooltip the factory made last.
    pub tooltip: Option<NodeId>,
    /// `G_RolloverSingleton`.
    pub rollover: Option<NodeId>,
    /// `G_TextControlTypingCount`: the text controls in a typewriter run.
    pub text_typing_count: i32,
    /// `TextWinProjectPoint` and `TextWinProjectExtent` in place of the
    /// Win3D camera's (none: the camera).
    pub text_projection: Option<Box<dyn super::controls::text_win::TextProjection>>,
    /// `G_ScrollVSerial`: the number of the next `ScrollV_%d` bar a
    /// rich-text window makes (divergence D475).
    pub scroll_v_serial: i32,
    /// The values a parent set on a scroll bar while the bar was telling it
    /// of its own change, which the bar takes when the parent returns
    /// (divergence D561).
    scroll_values_back: Vec<(NodeId, f32)>,
    /// Slot 23 messages that came while their receiver's own method ran (a
    /// scroll bar drawn in its window's draw), delivered when that method
    /// returns (divergence D561).
    pending_child_messages: Vec<(NodeId, ChildMessage)>,
    /// The button montage the engine loaded (its tiles take the place of
    /// the services' tile search once loaded).
    pub montage: super::montage::ButtonMontage,
}

impl Ui {
    /// The manager for a surface: `Win3DStartupManager`'s camera, the
    /// mouse ray (0, 0, -1).
    #[must_use]
    pub fn new(surface: Surface) -> Ui {
        Ui {
            slots: Vec::new(),
            free: Vec::new(),
            registry: Vec::new(),
            key_focus: None,
            capture: Vec::new(),
            hover: None,
            mouse_ray: Vector::new(0.0, 0.0, -1.0),
            cursor: (0, 0),
            cursor_drawn: true,
            modal_capture_count: 0,
            camera: Camera::win3d(surface),
            shift: Shift::default(),
            update_node_count: 0,
            tooltip_requests: Vec::new(),
            mesh_loader: None,
            factory: Factory::with_toolkit(),
            ini_loaded: None,
            fonts: None,
            pulse: Pulse::default(),
            frame_time: 0.0,
            ui_shapes: super::controls::scroll::ScrollShapes::default(),
            services: Box::new(super::services::NoServices),
            catalogs: None,
            tooltip: None,
            rollover: None,
            text_typing_count: 0,
            text_projection: None,
            scroll_v_serial: 1,
            scroll_values_back: Vec::new(),
            pending_child_messages: Vec::new(),
            montage: super::montage::ButtonMontage::default(),
        }
    }

    /// The button montage tile of a mesh path: the loaded montage's
    /// (`FindMontageTileForPath`), else the services' answer.
    pub fn montage_tile(&mut self, path: &[u8]) -> Option<super::services::MontageTile> {
        if self.montage.loaded {
            self.montage.find(path)
        } else {
            self.services.montage_tile(path)
        }
    }

    /// `GetIdsStringWWithRawFallback`: the string-table entry, else the
    /// raw text resource, else nothing (an empty result).
    #[must_use]
    pub fn ids_string(&self, ids: u32) -> Vec<u16> {
        let Some(c) = self.catalogs.as_deref() else {
            return Vec::new();
        };
        let s = c.string(ids);
        if !s.is_empty() {
            return s.to_vec();
        }
        c.text(ids).map(|t| t.units()).unwrap_or_default()
    }

    /// `NnToolTipFeedHover`: the hover to the tooltip, when there is one.
    pub fn feed_tooltip_hover(&mut self, hover: &super::services::ToolTipHover) {
        let Some(tip) = self.tooltip else { return };
        self.with_widget(tip, (), |w, ui| w.tooltip_hover(ui, tip, hover));
    }

    /// `NnToolTipHideGlobal`.
    pub fn hide_tooltip(&mut self) {
        let Some(tip) = self.tooltip else { return };
        self.with_widget(tip, (), |w, ui| w.tooltip_hide(ui, tip));
    }

    // ---- the arena ----------------------------------------------------

    /// A new node holding `widget`, in no tree and not registered (the
    /// constructor; `Create` comes after).
    pub fn alloc(&mut self, widget: Option<Box<dyn Widget>>) -> NodeId {
        let node = Node::new(widget);
        if let Some(index) = self.free.pop() {
            let slot = &mut self.slots[index as usize];
            slot.node = Some(node);
            NodeId {
                index,
                generation: slot.generation,
            }
        } else {
            self.slots.push(Slot {
                generation: 0,
                node: Some(node),
            });
            NodeId {
                index: (self.slots.len() - 1) as u32,
                generation: 0,
            }
        }
    }

    #[must_use]
    pub fn node(&self, id: NodeId) -> Option<&Node> {
        self.slots
            .get(id.index as usize)
            .filter(|s| s.generation == id.generation)
            .and_then(|s| s.node.as_ref())
    }

    pub fn node_mut(&mut self, id: NodeId) -> Option<&mut Node> {
        self.slots
            .get_mut(id.index as usize)
            .filter(|s| s.generation == id.generation)
            .and_then(|s| s.node.as_mut())
    }

    /// Whether the id names a live node.
    #[must_use]
    pub fn contains(&self, id: NodeId) -> bool {
        self.node(id).is_some()
    }

    /// `G_Win3DLiveCount`.
    #[must_use]
    pub fn live_count(&self) -> usize {
        self.slots.iter().filter(|s| s.node.is_some()).count()
    }

    /// The widget of a node, downcast.
    #[must_use]
    pub fn widget<T: Widget>(&self, id: NodeId) -> Option<&T> {
        self.node(id)?.widget.as_ref()?.as_any().downcast_ref()
    }

    pub fn widget_mut<T: Widget>(&mut self, id: NodeId) -> Option<&mut T> {
        self.node_mut(id)?
            .widget
            .as_mut()?
            .as_any_mut()
            .downcast_mut()
    }

    /// The `WinControl` part of a node (slot 2).
    #[must_use]
    pub fn win_control(&self, id: NodeId) -> Option<&WinControl> {
        self.node(id)?.widget.as_ref()?.win_control()
    }

    /// Whether a node's widget is a `WinControl` (slot 2 answers), also
    /// while one of its methods runs.
    #[must_use]
    pub fn is_win_control(&self, id: NodeId) -> bool {
        self.node(id).is_some_and(|n| n.is_control)
    }

    pub fn win_control_mut(&mut self, id: NodeId) -> Option<&mut WinControl> {
        self.node_mut(id)?.widget.as_mut()?.win_control_mut()
    }

    /// Runs `f` on a node's widget, taken out of the node for the call;
    /// `default` when the node is gone or its widget is busy.
    pub fn with_widget<R>(
        &mut self,
        id: NodeId,
        default: R,
        f: impl FnOnce(&mut dyn Widget, &mut Ui) -> R,
    ) -> R {
        let Some(mut widget) = self.node_mut(id).and_then(|n| n.widget.take()) else {
            return default;
        };
        let result = f(widget.as_mut(), self);
        if let Some(node) = self.node_mut(id)
            && node.widget.is_none()
        {
            node.widget = Some(widget);
        }
        if !self.pending_child_messages.is_empty() {
            self.deliver_pending_child_messages(id);
        }
        result
    }

    /// Runs `f` with `this`, the widget of node `id` whose method runs, back
    /// in its node, and takes it out again after: calls through the tree
    /// that reach the node while `f` runs find the widget and its state, as
    /// the reference's calls reach the object (this narrows D347). A node
    /// whose widget is not out, or that `f` deletes, leaves `this` as it is.
    pub fn lend<W: Widget + Default>(&mut self, id: NodeId, this: &mut W, f: impl FnOnce(&mut Ui)) {
        match self.node_mut(id) {
            Some(node) if node.widget.is_none() => {
                node.widget = Some(Box::new(std::mem::take(this)));
            }
            _ => {
                f(self);
                return;
            }
        }
        f(self);
        if let Some(mut widget) = self.node_mut(id).and_then(|n| n.widget.take())
            && let Some(back) = widget.as_any_mut().downcast_mut::<W>()
        {
            *this = std::mem::take(back);
        }
    }

    /// Calls [`Ui::ini_loaded`] for the control `id` and the file `name`.
    pub fn notify_ini_loaded(&mut self, id: NodeId, name: &[u8]) {
        if let Some(mut hook) = self.ini_loaded.take() {
            hook(self, id, name);
            if self.ini_loaded.is_none() {
                self.ini_loaded = Some(hook);
            }
        }
    }

    /// The slot 23 messages that waited for `id`, in the order they came;
    /// those for deleted nodes are dropped.
    fn deliver_pending_child_messages(&mut self, id: NodeId) {
        let pending = std::mem::take(&mut self.pending_child_messages);
        let (mine, rest): (Vec<_>, Vec<_>) = pending.into_iter().partition(|(to, _)| *to == id);
        self.pending_child_messages = rest
            .into_iter()
            .filter(|(to, _)| self.contains(*to))
            .collect();
        for (_, message) in mine {
            self.send_child_message(id, &message);
        }
    }

    fn flags(&self, id: NodeId) -> u8 {
        self.node(id).map_or(0, |n| n.flags)
    }

    fn set_flags(&mut self, id: NodeId, f: impl FnOnce(u8) -> u8) {
        if let Some(n) = self.node_mut(id) {
            n.flags = f(n.flags);
        }
    }

    #[must_use]
    pub fn parent(&self, id: NodeId) -> Option<NodeId> {
        self.node(id)?.parent
    }

    #[must_use]
    pub fn children(&self, id: NodeId) -> Vec<NodeId> {
        self.node(id)
            .map(|n| n.children.clone())
            .unwrap_or_default()
    }

    /// Slot 3: whether `ancestor` is above `id`.
    #[must_use]
    pub fn is_descendant_of(&self, id: NodeId, ancestor: NodeId) -> bool {
        let mut up = self.parent(id);
        while let Some(p) = up {
            if p == ancestor {
                return true;
            }
            up = self.parent(p);
        }
        false
    }

    /// The topmost ancestor (the node itself for a root).
    #[must_use]
    pub fn root_of(&self, id: NodeId) -> NodeId {
        let mut root = id;
        while let Some(p) = self.parent(root) {
            root = p;
        }
        root
    }

    // ---- the registry ---------------------------------------------------

    /// `Win3DRegister`.
    pub fn register(&mut self, id: NodeId) {
        self.registry.push(Some(id));
    }

    /// The registered objects in creation order.
    pub fn registered(&self) -> impl Iterator<Item = NodeId> + '_ {
        self.registry.iter().filter_map(|e| *e)
    }

    /// `Win3DGetFirstRegistered`.
    #[must_use]
    pub fn first_registered(&self) -> Option<NodeId> {
        self.registry.first().copied().flatten()
    }

    fn registry_at(&self, index: usize) -> Option<Option<NodeId>> {
        self.registry.get(index).copied()
    }

    /// `Win3DUnregister`: the latch, the focus, capture and hover references
    /// dropped, the parent link cut and the registry entry nulled. False
    /// when the object was not registered (its parent link stays then).
    pub fn unregister(&mut self, id: NodeId) -> bool {
        if self.flags(id) & UNREGISTERED != 0 {
            return true;
        }
        self.set_flags(id, |f| f | UNREGISTERED);
        if self.key_focus == Some(id) {
            self.key_focus = None;
        }
        if self.capture.first() == Some(&id) {
            // The reference clears the whole stack head pointer.
            self.capture.clear();
        }
        if self.hover == Some(id) {
            self.hover = None;
        }
        let Some(entry) = self.registry.iter().position(|e| *e == Some(id)) else {
            return false;
        };
        if let Some(parent) = self.parent(id) {
            if let Some(p) = self.node_mut(parent) {
                p.children.retain(|&c| c != id);
            }
            if let Some(n) = self.node_mut(id) {
                n.parent = None;
            }
        }
        self.registry[entry] = None;
        true
    }

    /// `DrainPendingRemove`.
    fn drain_pending(&mut self) {
        self.registry.retain(Option::is_some);
    }

    /// `Win3DReparent`: unlinked from the old parent, appended as the new
    /// parent's last child.
    pub fn reparent(&mut self, child: NodeId, new_parent: Option<NodeId>) {
        let Some(old) = self.node(child).map(|n| n.parent) else {
            return;
        };
        if old == new_parent {
            return;
        }
        if let Some(old) = old
            && let Some(p) = self.node_mut(old)
        {
            p.children.retain(|&c| c != child);
        }
        if let Some(n) = self.node_mut(child) {
            n.parent = new_parent;
        }
        if let Some(new_parent) = new_parent
            && let Some(p) = self.node_mut(new_parent)
        {
            p.children.push(child);
        }
    }

    /// `Win3DFindByClassName` (case-insensitive).
    #[must_use]
    pub fn find_by_class_name(&self, class_name: &[u8]) -> Option<NodeId> {
        self.registered().find(|&id| {
            self.node(id)
                .and_then(|n| n.class_name)
                .is_some_and(|c| neuron_core::hash::eq_ignore_case(c.as_bytes(), class_name))
        })
    }

    /// `Win3DFindByName` (case-insensitive).
    #[must_use]
    pub fn find_by_name(&self, name: &[u8]) -> Option<NodeId> {
        self.registered().find(|&id| {
            self.node(id)
                .is_some_and(|n| neuron_core::hash::eq_ignore_case(&n.name, name))
        })
    }

    // ---- focus, capture, hover -------------------------------------------

    /// `Win3DSetKeyFocus`.
    pub fn set_key_focus(&mut self, focus: Option<NodeId>) {
        self.key_focus = focus;
    }

    #[must_use]
    pub fn key_focus(&self) -> Option<NodeId> {
        self.key_focus
    }

    /// `Win3DPushCapture`: the object to the head of the stack, moved there
    /// when it is on it already.
    pub fn push_capture(&mut self, id: NodeId) {
        self.capture.retain(|&c| c != id);
        self.capture.insert(0, id);
    }

    /// `Win3DRemoveCapture`.
    pub fn remove_capture(&mut self, id: NodeId) {
        if let Some(i) = self.capture.iter().position(|&c| c == id) {
            self.capture.remove(i);
        }
    }

    /// `Win3DGetCapture`: the head of the stack.
    #[must_use]
    pub fn capture(&self) -> Option<NodeId> {
        self.capture.first().copied()
    }

    /// The capture stack, head first.
    #[must_use]
    pub fn capture_stack(&self) -> &[NodeId] {
        &self.capture
    }

    /// `Win3DGetHover`.
    #[must_use]
    pub fn hover(&self) -> Option<NodeId> {
        self.hover
    }

    /// `Win3DAnyCaptured`: whether a registered `WinControl` holds the
    /// mouse.
    #[must_use]
    pub fn any_captured(&self) -> bool {
        self.registered()
            .any(|id| self.win_control(id).is_some_and(|c| c.captured))
    }

    /// `SetValue` on the scroll bar `bar` while the bar is busy telling its
    /// parent of a change (D347): the bar makes the call when its parent
    /// returns ([`Self::take_scroll_value`]).
    pub fn defer_scroll_value(&mut self, bar: NodeId, value: f32) {
        self.scroll_values_back.push((bar, value));
    }

    /// The last value deferred for `bar`, the others for it dropped.
    pub fn take_scroll_value(&mut self, bar: NodeId) -> Option<f32> {
        let mut last = None;
        self.scroll_values_back.retain(|&(id, value)| {
            if id == bar {
                last = Some(value);
                false
            } else {
                true
            }
        });
        last
    }

    /// The tooltip requests since the last call.
    pub fn take_tooltip_requests(&mut self) -> Vec<ToolTipRequest> {
        std::mem::take(&mut self.tooltip_requests)
    }

    pub(crate) fn request_tooltip(&mut self, request: ToolTipRequest) {
        self.tooltip_requests.push(request);
    }

    // ---- the mouse ray and the widescreen shift -----------------------------

    /// `Win3DGetMouseRay`: the stored ray, or inside a shifted window the
    /// cursor's ray through the shifted camera.
    #[must_use]
    pub fn mouse_ray(&self) -> Vector {
        if self.shift.pixels != 0.0 {
            return cursor_ray(&self.camera, self.cursor.0, self.cursor.1);
        }
        self.mouse_ray
    }

    /// The ray as the last move event stored it.
    #[must_use]
    pub fn stored_mouse_ray(&self) -> Vector {
        self.mouse_ray
    }

    /// Stores the mouse ray (`Win3DMouseRayStorage`).
    pub fn set_mouse_ray(&mut self, ray: Vector) {
        self.mouse_ray = ray;
    }

    /// `WidescreenHudCurrentShift`.
    #[must_use]
    pub fn current_shift(&self) -> f32 {
        self.shift.pixels
    }

    #[must_use]
    pub fn shift(&self) -> &Shift {
        &self.shift
    }

    /// `WidescreenHudShiftPixelsFor`: 0 when the object or an ancestor is
    /// [`Anchor::Centre`], otherwise the shift of the share of the nearest
    /// [`Anchor::Edge`] on the way up (0 without one).
    #[must_use]
    pub fn shift_pixels_for(&self, id: NodeId) -> f32 {
        let mut share = None;
        let mut walk = Some(id);
        while let Some(w) = walk {
            let Some(node) = self.node(w) else { break };
            match node.anchor {
                Anchor::Centre => return 0.0,
                Anchor::Edge(s) => {
                    share.get_or_insert(s);
                }
                Anchor::Inherit => {}
            }
            walk = node.parent;
        }
        shift_pixels(&self.camera.surface, share.unwrap_or(0.0))
    }

    /// Runs `f` with the camera shifted by `pixels`, the previous shift put
    /// back after (`FWidescreenHudShiftScope`).
    pub fn with_shift<R>(&mut self, pixels: f32, f: impl FnOnce(&mut Ui) -> R) -> R {
        let previous = self.shift.pixels;
        let changed = pixels != previous;
        if changed {
            self.shift.set(&mut self.camera, pixels);
        }
        let result = f(self);
        if changed {
            self.shift.set(&mut self.camera, previous);
        }
        result
    }

    fn with_shift_draw(
        &mut self,
        pixels: f32,
        host: &mut dyn DrawHost,
        f: impl FnOnce(&mut Ui, &mut dyn DrawHost),
    ) {
        let previous = self.shift.pixels;
        let changed = pixels != previous;
        if changed {
            self.shift.set(&mut self.camera, pixels);
            host.camera_changed(&self.camera);
        }
        f(self, host);
        if changed {
            self.shift.set(&mut self.camera, previous);
            host.camera_changed(&self.camera);
        }
    }

    // ---- the Win3D bodies ------------------------------------------------

    /// `Win3D::Create`.
    pub fn base_create(&mut self, id: NodeId, parent: Option<NodeId>) -> bool {
        self.reparent(id, parent);
        self.register(id);
        self.set_flags(id, |f| (f & !UNREGISTERED) | ACTIVE | VISIBLE);
        true
    }

    /// `Win3D::Destroy`.
    pub fn base_destroy(&mut self, id: NodeId) {
        self.set_flags(id, |f| f & !(ACTIVE | VISIBLE));
        for child in self.children(id) {
            self.destroy(child);
        }
        self.unregister(id);
    }

    /// `~Win3D`: the same as `Destroy`.
    pub fn base_on_delete(&mut self, id: NodeId) {
        self.base_destroy(id);
    }

    /// `Win3D::SetPosition`.
    pub fn base_set_position(&mut self, id: NodeId, position: Vector) {
        if let Some(n) = self.node_mut(id) {
            n.position = position;
        }
    }

    /// `Win3D::SetOrientation`.
    pub fn base_set_orientation(&mut self, id: NodeId, orientation: Matrix) {
        if let Some(n) = self.node_mut(id) {
            n.orientation = orientation;
        }
    }

    /// `Win3D::Update`: each child updated (active) or updated inactive,
    /// the next sibling read before each call.
    pub fn base_update(&mut self, id: NodeId) {
        for child in self.children(id) {
            if self.contains(child) {
                self.update_node(child);
            }
        }
    }

    /// `Win3D::UpdateInactive`: the inactive children only.
    pub fn base_update_inactive(&mut self, id: NodeId) {
        for child in self.children(id) {
            if self.flags(child) & ACTIVE == 0 && self.contains(child) {
                self.update_node(child);
            }
        }
    }

    /// `Win3D::MouseEvent`: to the parent's slot 19 (in the caller's
    /// shift), false at the root.
    pub fn base_mouse_event(&mut self, id: NodeId, event: MouseButton) -> bool {
        match self.parent(id) {
            Some(parent) => self.mouse_event_unshifted(parent, event),
            None => false,
        }
    }

    fn mouse_event_unshifted(&mut self, id: NodeId, event: MouseButton) -> bool {
        self.with_widget(id, None, |w, ui| Some(w.mouse_event(ui, id, event)))
            .unwrap_or_else(|| self.base_mouse_event(id, event))
    }

    /// `Win3D::Slot24`: to every child.
    pub fn base_notify(&mut self, id: NodeId, value: u32) {
        for child in self.children(id) {
            self.with_widget(child, (), |w, ui| w.notify(ui, child, value));
        }
    }

    // ---- virtual calls ----------------------------------------------------

    /// Slot 10.
    pub fn destroy(&mut self, id: NodeId) {
        if !self.with_widget(id, false, |w, ui| {
            w.destroy(ui, id);
            true
        }) && self.contains(id)
        {
            self.base_destroy(id);
        }
    }

    /// Slot 5.
    pub fn set_position(&mut self, id: NodeId, position: Vector) {
        if !self.with_widget(id, false, |w, ui| {
            w.set_position(ui, id, position);
            true
        }) {
            self.base_set_position(id, position);
        }
    }

    /// Slot 7.
    pub fn set_orientation(&mut self, id: NodeId, orientation: Matrix) {
        if !self.with_widget(id, false, |w, ui| {
            w.set_orientation(ui, id, orientation);
            true
        }) {
            self.base_set_orientation(id, orientation);
        }
    }

    /// Slot 8.
    pub fn set_transform(&mut self, id: NodeId, transform: &Transform) {
        if !self.with_widget(id, false, |w, ui| {
            w.set_transform(ui, id, transform);
            true
        }) {
            self.base_set_orientation(id, transform.rotation);
            self.base_set_position(id, transform.translation);
        }
    }

    /// Slot 19 with the object's shift (`MouseEventWidescreen`).
    pub fn mouse_event(&mut self, id: NodeId, event: MouseButton) -> bool {
        let shift = self.shift_pixels_for(id);
        self.with_shift(shift, |ui| ui.mouse_event_unshifted(id, event))
    }

    /// Slot 42.
    pub fn property(&mut self, id: NodeId, property: Property<'_>) -> isize {
        self.with_widget(id, 0, |w, ui| w.property(ui, id, property))
    }

    /// The update of one object with its shift: slot 11 when active, else
    /// slot 12 (`UpdateWidescreen`).
    pub fn update_node(&mut self, id: NodeId) {
        let shift = self.shift_pixels_for(id);
        self.with_shift(shift, |ui| {
            let active = ui.flags(id) & ACTIVE != 0;
            let ran = ui.with_widget(id, false, |w, ui| {
                if active {
                    w.update(ui, id);
                } else {
                    w.update_inactive(ui, id);
                }
                true
            });
            if !ran && ui.contains(id) {
                if active {
                    ui.base_update(id);
                } else {
                    ui.base_update_inactive(id);
                }
            }
        });
    }

    /// `HitTestWidescreen`: slot 4 with the object's shift; a shifted
    /// object is tested with the cursor's ray through the shifted camera.
    fn hit_test(&mut self, id: NodeId, ray: Vector) -> bool {
        let shift = self.shift_pixels_for(id);
        if shift == self.shift.pixels {
            return self.with_widget(id, false, |w, ui| w.hit_test(ui, id, ray));
        }
        self.with_shift(shift, |ui| {
            let shifted = ui.mouse_ray();
            ui.with_widget(id, false, |w, ui| w.hit_test(ui, id, shifted))
        })
    }

    fn mouse_call(&mut self, id: NodeId, call: fn(&mut dyn Widget, &mut Ui, NodeId, Vector)) {
        let shift = self.shift_pixels_for(id);
        self.with_shift(shift, |ui| {
            let ray = ui.mouse_ray();
            ui.with_widget(id, (), |w, ui| call(w, ui, id, ray));
        });
    }

    fn wheel_call(&mut self, id: NodeId, delta: i32) -> bool {
        let shift = self.shift_pixels_for(id);
        self.with_shift(shift, |ui| {
            ui.with_widget(id, false, |w, ui| w.wheel(ui, id, delta))
        })
    }

    // ---- picking -------------------------------------------------------------

    fn visible_chain(&self, id: NodeId) -> bool {
        let mut walk = Some(id);
        while let Some(w) = walk {
            if self.flags(w) & VISIBLE == 0 {
                return false;
            }
            walk = self.parent(w);
        }
        true
    }

    fn pick_depth(&self, id: NodeId) -> i32 {
        let node_priority = self.node(id).map_or(0, |n| n.hit_priority);
        let mut depth = self
            .node(id)
            .and_then(|n| n.widget.as_deref())
            .map_or(node_priority, |w| w.hit_priority(node_priority));
        if depth == 0 {
            let mut walk = Some(id);
            while let Some(w) = walk {
                depth += 1;
                walk = self.parent(w);
            }
        }
        depth
    }

    fn pickable(&self, id: NodeId) -> bool {
        let f = self.flags(id);
        f & ACTIVE != 0 && f & VISIBLE != 0 && f & PICKABLE != 0
    }

    /// `Win3DPick`: the topmost pickable, visible object the ray hits.
    pub fn pick(&mut self, ray: Vector) -> Option<NodeId> {
        self.pick_impl(None, ray)
    }

    /// `Win3DPickWithin`: the same among `capture` and its descendants
    /// (the visibility walk only for a deeper candidate).
    pub fn pick_within(&mut self, capture: NodeId, ray: Vector) -> Option<NodeId> {
        self.pick_impl(Some(capture), ray)
    }

    fn pick_impl(&mut self, within: Option<NodeId>, ray: Vector) -> Option<NodeId> {
        let mut best: Option<NodeId> = None;
        let mut best_depth = -1;
        let mut i = 0;
        while let Some(entry) = self.registry_at(i) {
            i += 1;
            let Some(id) = entry else { continue };
            if !self.pickable(id) {
                continue;
            }
            if let Some(capture) = within {
                if id != capture && !self.is_descendant_of(id, capture) {
                    continue;
                }
            } else if !self.visible_chain(id) {
                continue;
            }
            let depth = self.pick_depth(id);
            if depth > best_depth {
                if within.is_some() && !self.visible_chain(id) {
                    continue;
                }
                if self.hit_test(id, ray) {
                    best = Some(id);
                    best_depth = depth;
                }
                continue;
            }
            if depth != best_depth {
                continue;
            }
            let (Some(z), Some(best_z)) = (
                self.node(id).map(|n| n.position.z),
                best.and_then(|b| self.node(b)).map(|n| n.position.z),
            ) else {
                continue;
            };
            if !(z > best_z) {
                continue;
            }
            if self.hit_test(id, ray) {
                best = Some(id);
                best_depth = depth;
            }
        }
        best
    }

    /// The pick of a move: inside the capture head (falling back to it) or
    /// global.
    fn pick_for_hover(&mut self) -> Option<NodeId> {
        let ray = self.mouse_ray;
        match self.capture() {
            None => self.pick(ray),
            Some(capture) => self.pick_within(capture, ray).or(Some(capture)),
        }
    }

    /// `Win3DSetHover`: leave and enter on a change, over on every call
    /// with an object.
    pub fn set_hover(&mut self, object: Option<NodeId>) {
        if self.hover != object {
            if let Some(old) = self.hover {
                self.mouse_call(old, |w, ui, id, ray| w.mouse_leave(ui, id, ray));
            }
            self.hover = object;
            if let Some(new) = object {
                self.mouse_call(new, |w, ui, id, ray| w.mouse_enter(ui, id, ray));
            }
        }
        if let Some(new) = object {
            self.mouse_call(new, |w, ui, id, ray| w.mouse_over(ui, id, ray));
        }
    }

    // ---- input -----------------------------------------------------------------

    /// The Win3D input handler: whether the event was taken.
    pub fn handle(&mut self, input: Input) -> bool {
        match input {
            Input::Key { key, mods } => self.dispatch_key(key, mods),
            Input::Char { wparam, lparam } => self.dispatch_char(wparam, lparam),
            Input::Wheel(delta) => {
                if let Some(capture) = self.capture() {
                    return self.wheel_call(capture, delta);
                }
                match self.pick(self.mouse_ray) {
                    Some(picked) => self.wheel_call(picked, delta),
                    None => false,
                }
            }
            Input::Move { x, y } => {
                self.cursor = (x, y);
                self.mouse_ray = self.camera.mouse_ray(x, y);
                let picked = self.pick_for_hover();
                self.set_hover(picked);
                picked.is_some()
            }
            Input::Button(event) => self.dispatch_button(event),
        }
    }

    /// `Win3DDispatchKey`: the focus alone, else every registered object in
    /// order until one takes the key.
    pub fn dispatch_key(&mut self, key: u32, mods: KeyMods) -> bool {
        let mut mods = mods;
        if let Some(focus) = self.key_focus {
            return self.with_widget(focus, false, |w, ui| w.key_event(ui, focus, key, &mut mods));
        }
        let mut i = 0;
        while let Some(entry) = self.registry_at(i) {
            i += 1;
            let Some(id) = entry else { continue };
            if self.with_widget(id, false, |w, ui| w.key_event(ui, id, key, &mut mods)) {
                return true;
            }
        }
        false
    }

    /// Event 2: slot 22 to the focus, else to every registered object until
    /// one takes it.
    pub fn dispatch_char(&mut self, wparam: u32, lparam: u32) -> bool {
        if let Some(focus) = self.key_focus {
            return self.with_widget(focus, false, |w, ui| {
                w.char_event(ui, focus, wparam, lparam)
            });
        }
        let mut i = 0;
        while let Some(entry) = self.registry_at(i) {
            i += 1;
            let Some(id) = entry else { continue };
            if self.with_widget(id, false, |w, ui| w.char_event(ui, id, wparam, lparam)) {
                return true;
            }
        }
        false
    }

    /// Events 6 to 13: down the capture stack (the pick inside each
    /// capture, then the capture itself unless it was the pick), then the
    /// global pick.
    pub fn dispatch_button(&mut self, event: MouseButton) -> bool {
        let ray = self.mouse_ray;
        let mut handled = false;
        let mut capture = self.capture();
        while let Some(c) = capture {
            if handled {
                return handled;
            }
            let next = self
                .capture
                .iter()
                .position(|&x| x == c)
                .and_then(|i| self.capture.get(i + 1).copied());
            let hit = self.pick_within(c, ray);
            if let Some(hit) = hit {
                handled = self.mouse_event(hit, event);
            }
            if !handled && hit != Some(c) {
                handled = self.mouse_event(c, event);
            }
            capture = next;
        }
        if handled {
            return handled;
        }
        match self.pick(ray) {
            Some(picked) => self.mouse_event(picked, event),
            None => handled,
        }
    }

    /// `Win3DBroadcast`: slot 23 to every registered object until one takes
    /// it.
    pub fn broadcast(&mut self, msg: u32, a: u32, b: u32) -> bool {
        let mut i = 0;
        while let Some(entry) = self.registry_at(i) {
            i += 1;
            let Some(id) = entry else { continue };
            if self.with_widget(id, false, |w, ui| w.broadcast(ui, id, msg, a, b)) {
                return true;
            }
        }
        false
    }

    /// [`Widget::child_message`] on one object.
    pub fn send_child_message(&mut self, id: NodeId, message: &ChildMessage) -> bool {
        if self.node(id).is_some_and(|n| n.widget.is_none()) {
            // The receiver's own method is running (D347): the message waits
            // for it to return (D561); the sender sees no answer.
            self.pending_child_messages.push((id, message.clone()));
            return false;
        }
        self.with_widget(id, false, |w, ui| w.child_message(ui, id, message))
    }

    /// Slot 23 on one object.
    pub fn send_broadcast(&mut self, id: NodeId, msg: u32, a: u32, b: u32) -> bool {
        self.with_widget(id, false, |w, ui| w.broadcast(ui, id, msg, a, b))
    }

    // ---- the frame ------------------------------------------------------------

    /// `Win3DUpdateAll`: the pulse advanced (the tooltip and rollover ticks
    /// are the game's), the hover pick (while the cursor is drawn), then
    /// either the first captured `WinControl` alone (a modal dialog is up)
    /// or every root, then the unregistered entries erased.
    pub fn update_all(&mut self, delta_seconds: f64) {
        self.frame_time = delta_seconds;
        self.pulse.advance(delta_seconds);
        self.tick_tooltips(delta_seconds);
        let picked = if self.cursor_drawn {
            self.pick_for_hover()
        } else {
            None
        };
        if self.hover != picked {
            if let Some(old) = self.hover {
                self.mouse_call(old, |w, ui, id, ray| w.mouse_leave(ui, id, ray));
            }
            self.hover = picked;
            if let Some(new) = picked {
                self.mouse_call(new, |w, ui, id, ray| w.mouse_enter(ui, id, ray));
                self.mouse_call(new, |w, ui, id, ray| w.mouse_over(ui, id, ray));
            }
        }

        self.update_node_count = 0;
        if self.modal_capture_count != 0 {
            let mut i = 0;
            while let Some(entry) = self.registry_at(i) {
                i += 1;
                let Some(id) = entry else { continue };
                if self.win_control(id).is_some_and(|c| c.captured) {
                    self.update_node(id);
                    self.drain_pending();
                    return;
                }
            }
        }
        let mut i = 0;
        while let Some(entry) = self.registry_at(i) {
            i += 1;
            if let Some(id) = entry
                && self.parent(id).is_none()
            {
                self.update_node(id);
            }
            self.update_node_count += 1;
        }
        self.drain_pending();
    }

    /// The draw of one object with its shift (`DrawWidescreen`).
    pub fn draw_node(&mut self, id: NodeId, host: &mut dyn DrawHost) {
        let shift = self.shift_pixels_for(id);
        self.with_shift_draw(shift, host, |ui, host| {
            if !ui.with_widget(id, false, |w, ui| {
                w.draw(ui, id, host);
                true
            }) && ui.contains(id)
            {
                ui.draw_children(id, host);
            }
        });
    }

    /// `InsertSorted`: by layer, then by z, equal keys in insertion order;
    /// an append to a full array is dropped (the reference overruns the
    /// array on an insertion into a full one).
    fn insert_sorted(&self, sorted: &mut Vec<NodeId>, id: NodeId) {
        let Some(node) = self.node(id) else { return };
        let (layer, z) = (node.draw_layer, node.position.z);
        let last_layer = sorted
            .last()
            .and_then(|&l| self.node(l))
            .map(|n| n.draw_layer);
        if last_layer.is_none_or(|last| layer > last) {
            if sorted.len() < DRAW_SORT_CAPACITY {
                sorted.push(id);
            }
            return;
        }
        let at = sorted
            .iter()
            .position(|&other| {
                let o = self.node(other).expect("sorted nodes are live");
                layer < o.draw_layer || (layer == o.draw_layer && z < o.position.z)
            })
            .unwrap_or(sorted.len());
        sorted.insert(at, id);
    }

    /// `Win3DDrawChildren`: the visible children, layer 0 at once, the rest
    /// sorted.
    pub fn draw_children(&mut self, id: NodeId, host: &mut dyn DrawHost) {
        let mut sorted = Vec::new();
        for child in self.children(id) {
            let Some(node) = self.node(child) else {
                continue;
            };
            if node.flags & VISIBLE == 0 {
                continue;
            }
            if node.draw_layer == 0 {
                self.draw_node(child, host);
                continue;
            }
            self.insert_sorted(&mut sorted, child);
        }
        for child in sorted {
            if self.contains(child) {
                self.draw_node(child, host);
            }
        }
    }

    /// `Win3DDrawAll`: the visible roots, layer 0 at once, the rest sorted
    /// and drawn in eight sub-layer passes.
    pub fn draw_all(&mut self, host: &mut dyn DrawHost) {
        host.begin_draw(&self.camera);
        let mut sorted = Vec::new();
        let mut i = 0;
        while let Some(entry) = self.registry_at(i) {
            i += 1;
            let Some(id) = entry else { continue };
            let Some(node) = self.node(id) else { continue };
            if node.parent.is_some() || node.flags & VISIBLE == 0 {
                continue;
            }
            if node.draw_layer == 0 {
                self.draw_node(id, host);
                continue;
            }
            self.insert_sorted(&mut sorted, id);
        }
        for sub_layer in 0..8 {
            for &id in &sorted {
                if self.node(id).is_some_and(|n| n.draw_sub_layer == sub_layer) {
                    self.draw_node(id, host);
                }
            }
        }
        host.end_draw();
    }

    // ---- creation and deletion ------------------------------------------------

    /// The constructor and `Create` of a factory product: a new node with
    /// the widget and class name, `Create` called; on failure the object is
    /// deleted and `None` returned.
    pub fn create(
        &mut self,
        widget: Box<dyn Widget>,
        class_name: Option<&'static str>,
        parent: Option<NodeId>,
        payload: &mut Payload<'_>,
    ) -> Option<NodeId> {
        let id = self.alloc(Some(widget));
        if let Some(n) = self.node_mut(id) {
            n.class_name = class_name;
        }
        let ok = self.with_widget(id, false, |w, ui| w.create(ui, id, parent, payload));
        if !ok {
            self.delete(id);
            return None;
        }
        Some(id)
    }

    /// A plain `Win3D` (no widget) created under `parent`.
    pub fn create_plain(&mut self, parent: Option<NodeId>) -> NodeId {
        let id = self.alloc(None);
        self.base_create(id, parent);
        id
    }

    /// `delete`: the destructor chain, then the node freed.
    pub fn delete(&mut self, id: NodeId) {
        if !self.contains(id) {
            return;
        }
        if !self.with_widget(id, false, |w, ui| {
            w.on_delete(ui, id);
            true
        }) {
            self.base_on_delete(id);
        }
        // A node the destructor left linked (never registered) is cut
        // loose here; the reference leaves a dangling child pointer.
        if let Some(parent) = self.parent(id)
            && let Some(p) = self.node_mut(parent)
        {
            p.children.retain(|&c| c != id);
        }
        for child in self.children(id) {
            if let Some(c) = self.node_mut(child) {
                c.parent = None;
            }
        }
        if self.key_focus == Some(id) {
            self.key_focus = None;
        }
        if self.tooltip == Some(id) {
            self.tooltip = None;
        }
        if self.rollover == Some(id) {
            self.rollover = None;
        }
        self.capture.retain(|&c| c != id);
        if self.hover == Some(id) {
            self.hover = None;
        }
        for entry in &mut self.registry {
            if *entry == Some(id) {
                *entry = None;
            }
        }
        let slot = &mut self.slots[id.index as usize];
        slot.node = None;
        slot.generation = slot.generation.wrapping_add(1);
        self.free.push(id.index);
    }

    /// `Win3DDeleteTree`: post-order, each child's subtree, then `Destroy`
    /// and `delete` on the root.
    pub fn delete_tree(&mut self, root: NodeId) {
        if !self.contains(root) {
            return;
        }
        for child in self.children(root) {
            self.delete_tree(child);
        }
        self.destroy(root);
        self.delete(root);
    }

    /// `Win3DShutdownManager`'s tree part: focus and capture dropped, every
    /// object unregistered and the registry emptied (the objects stay
    /// allocated, as in the reference, until their owners delete them).
    pub fn shutdown(&mut self) {
        self.key_focus = None;
        self.capture.clear();
        let ids: Vec<NodeId> = self.registered().collect();
        for id in ids {
            self.unregister(id);
        }
        self.registry.clear();
    }
}

#[cfg(test)]
mod tests;
