//! `WinControl` (`wincontrol_base.cpp`): the mesh-backed control every
//! NeuroNet control, scroll bar, rich-text window and most HUD panels
//! derive from.
//!
//! A control is a mesh (an engine instance of a `.cmp` or `.3db`) placed at
//! its position plus an offset, hit by the mouse ray through a box of half
//! extents (`mouse_size`) around the position plus a hit offset, with a
//! tooltip id, the mouse capture flag, a hardpoint of its parent's mesh it
//! attaches to, and a property switch (slot 42). `LoadIni` reads a
//! `[BaseFrame]` file and builds every further section through the factory
//! with the section name as the child's name.
//!
//! The engine instance is behind [`ControlMesh`] and the loader behind
//! [`MeshLoader`]: without a loader a control has no instance and takes the
//! reference's paths for that case (its position plus the offset). Drawing
//! the mesh belongs to the engine port.

use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Transform, Vector, inverse_transform};
use neuron_core::num::trunc_to_i32;

use super::message::{MSG_HIDE, MSG_SHOW, MouseButton};
use super::tree::{
    ACTIVE, DrawHost, NodeId, Payload, Property, ToolTipRequest, Ui, VISIBLE, Widget,
};

/// An engine instance of a control's mesh.
pub trait ControlMesh {
    /// The instance's 3x4 transform (`GetInstanceTransform`).
    fn transform(&self) -> Transform;
    /// `SetInstancePosition` and `UpdateInstance`.
    fn set_position(&mut self, position: Vector);
    /// `SetInstanceOrientation` and `UpdateInstance`.
    fn set_orientation(&mut self, orientation: Matrix);
    /// `SetInstanceMatrix` and `UpdateInstance`.
    fn set_transform(&mut self, transform: Transform);
    /// A hardpoint of the mesh's archetype by name: its orientation and
    /// position.
    fn hardpoint(&self, name: &[u8]) -> Option<Transform>;
}

/// Loads a control mesh (`ClientMeshLoadFromPath`); `None` when the path
/// gives no instance.
pub trait MeshLoader {
    fn load(&mut self, path: &[u8], material_path: &[u8]) -> Option<Box<dyn ControlMesh>>;
}

/// `SlabHitStrict`: the crossing of plane `a = 0` along the ray, strictly
/// in front and strictly inside the other two halves.
#[allow(clippy::too_many_arguments)]
fn slab_hit_strict(
    origin_a: f32,
    origin_b: f32,
    origin_c: f32,
    dir_a: f32,
    dir_b: f32,
    dir_c: f32,
    half_b: f32,
    half_c: f32,
) -> bool {
    if dir_a == 0.0 {
        return false;
    }
    let t = (-(f64::from(origin_a) / f64::from(dir_a))) as f32;
    if !(t > 0.0) {
        return false;
    }
    // `std::fabs(float)` takes its argument as a float.
    let b = (f64::from(origin_b) + f64::from(dir_b) * f64::from(t)) as f32;
    if !(b.abs() < half_b) {
        return false;
    }
    let c = (f64::from(origin_c) + f64::from(dir_c) * f64::from(t)) as f32;
    c.abs() < half_c
}

/// `Win3DRayHitsAabbHalfExtents`: whether the ray from the camera's origin
/// along `ray` crosses one of the three mid-planes of the box of `half`
/// extents placed by `transform`, strictly inside the box's other two
/// extents. All-zero extents never hit.
#[must_use]
pub fn ray_hits_box(half: Vector, transform: &Transform, ray: Vector) -> bool {
    if half.x == 0.0 && half.y == 0.0 && half.z == 0.0 {
        return false;
    }
    let out = inverse_transform(transform, ray);
    let t = transform.translation;
    let add = |a: f32, b: f32| (f64::from(a) + f64::from(b)) as f32;
    let dir = [add(out.x, t.x), add(out.y, t.y), add(out.z, t.z)];
    let origin = [-t.x, -t.y, -t.z];
    slab_hit_strict(
        origin[0], origin[1], origin[2], dir[0], dir[1], dir[2], half.y, half.z,
    ) || slab_hit_strict(
        origin[1], origin[0], origin[2], dir[1], dir[0], dir[2], half.x, half.z,
    ) || slab_hit_strict(
        origin[2], origin[0], origin[1], dir[2], dir[0], dir[1], half.x, half.y,
    )
}

fn add3(a: Vector, b: Vector) -> Vector {
    let add = |x: f32, y: f32| (f64::from(x) + f64::from(y)) as f32;
    Vector::new(add(a.x, b.x), add(a.y, b.y), add(a.z, b.z))
}

/// The `WinControl` part of a control.
pub struct WinControl {
    pub mesh: Option<Box<dyn ControlMesh>>,
    /// `+0xa8` "offset": added to the position for the mesh and the hit box.
    pub offset: Vector,
    /// `+0xb4` "mouse_size".
    pub half_extents: Vector,
    /// `+0xc0`.
    pub hit_offset: Vector,
    /// `+0xcc` "tooltip".
    pub tooltip_ids: i32,
    /// `+0xd0`.
    pub mesh_path: Vec<u8>,
    /// `+0x1d0`: the parent's hardpoint the control attaches to.
    pub hardpoint: Vec<u8>,
    /// `+0x2d0`: the control holds the mouse.
    pub captured: bool,
    /// `+0x2d1` "no_z_enable".
    pub no_z_enable: bool,
    /// `+0x2d2` (property 9).
    pub field_2d2: bool,
    /// `+0x2d3`: the hit box ignores the mesh's orientation.
    pub identity_hit_orientation: bool,
    /// `+0x2d4`: a rest offset HUD panels park here.
    pub home_offset: Vector,
    /// `+0x2e0`: {0, 0, 10000} from the constructor.
    pub field_2e0: Vector,
    /// `+0x2f4` bit 0: the mesh position includes the offset.
    pub position_includes_offset: bool,
    pub attach_orientation: Matrix,
    pub attach_position: Vector,
    pub attached: bool,
}

impl Default for WinControl {
    fn default() -> Self {
        WinControl::new()
    }
}

impl std::fmt::Debug for WinControl {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("WinControl")
            .field("has_mesh", &self.mesh.is_some())
            .field("offset", &self.offset)
            .field("half_extents", &self.half_extents)
            .field("tooltip_ids", &self.tooltip_ids)
            .field("captured", &self.captured)
            .finish_non_exhaustive()
    }
}

impl WinControl {
    /// The constructor's values.
    #[must_use]
    pub fn new() -> WinControl {
        WinControl {
            mesh: None,
            offset: Vector::ZERO,
            half_extents: Vector::ZERO,
            hit_offset: Vector::ZERO,
            tooltip_ids: 0,
            mesh_path: Vec::new(),
            hardpoint: Vec::new(),
            captured: false,
            no_z_enable: false,
            field_2d2: false,
            identity_hit_orientation: true,
            home_offset: Vector::ZERO,
            field_2e0: Vector::new(0.0, 0.0, 10000.0),
            position_includes_offset: true,
            attach_orientation: Matrix::IDENTITY,
            attach_position: Vector::ZERO,
            attached: false,
        }
    }

    /// The mesh position for a control position (`+0x2f4` bit 0).
    fn placed(&self, position: Vector) -> Vector {
        if self.position_includes_offset {
            add3(self.offset, position)
        } else {
            position
        }
    }

    /// The hit box transform of `HitTest`: the instance's transform, or the
    /// position plus the offset; plus the hit offset; identity orientation
    /// when `+0x2d3` is set.
    #[must_use]
    pub fn hit_transform(&self, position: Vector) -> Transform {
        let mut transform = Transform::IDENTITY;
        match &self.mesh {
            Some(mesh) => transform = mesh.transform(),
            None => transform.translation = add3(position, self.offset),
        }
        transform.translation = add3(transform.translation, self.hit_offset);
        if self.identity_hit_orientation {
            transform.rotation = Matrix::IDENTITY;
        }
        transform
    }
}

fn node_position(ui: &Ui, id: NodeId) -> Vector {
    ui.node(id).map_or(Vector::ZERO, |n| n.position)
}

fn control(ui: &mut Ui, id: NodeId) -> Option<&mut WinControl> {
    ui.win_control_mut(id)
}

/// The `WinControl` bodies, for widgets embedding a [`WinControl`] (they
/// stand for the base-class calls of the reference's overrides). Each takes
/// the control out of the tree only while it reads or writes it.
pub mod base {
    use super::*;

    /// `WinControl::HitTest`: the ray against the hit box; on a hit the
    /// parent `WinControl` hears property 0x25, then 0x4F when a tooltip id
    /// is set, and unless it answered non-zero an active, visible control
    /// with a tooltip asks for it.
    pub fn hit_test(ui: &mut Ui, id: NodeId, this: &WinControl, ray: Vector) -> bool {
        let transform = this.hit_transform(node_position(ui, id));
        if !ray_hits_box(this.half_extents, &transform, ray) {
            return false;
        }
        if let Some(parent) = ui.parent(id)
            && ui.win_control(parent).is_some()
        {
            ui.property(parent, Property::ChildHit(id));
            if this.tooltip_ids == 0 {
                return true;
            }
            if ui.property(parent, Property::ChildToolTip(id)) != 0 {
                return true;
            }
        }
        let flags = ui.node(id).map_or(0, |n| n.flags);
        if this.tooltip_ids != 0 && flags & ACTIVE != 0 && flags & VISIBLE != 0 {
            ui.request_tooltip(ToolTipRequest {
                owner: id,
                ids: this.tooltip_ids,
            });
        }
        true
    }

    /// `WinControl::SetPosition`: stored, the mesh moved, every child moved
    /// to the new position.
    pub fn set_position(ui: &mut Ui, id: NodeId, this: &mut WinControl, position: Vector) {
        ui.base_set_position(id, position);
        let placed = this.placed(position);
        if let Some(mesh) = this.mesh.as_mut() {
            mesh.set_position(placed);
        }
        for child in ui.children(id) {
            ui.set_position(child, position);
        }
    }

    /// `WinControl::SetOrientation`.
    pub fn set_orientation(ui: &mut Ui, id: NodeId, this: &mut WinControl, orientation: Matrix) {
        ui.base_set_orientation(id, orientation);
        if let Some(mesh) = this.mesh.as_mut() {
            mesh.set_orientation(orientation);
        }
        for child in ui.children(id) {
            ui.set_orientation(child, orientation);
        }
    }

    /// `WinControl::Destroy`: the mesh released, every child destroyed and
    /// deleted, then `Win3D::Destroy`.
    pub fn destroy(ui: &mut Ui, id: NodeId, this: &mut WinControl) {
        this.mesh = None;
        for child in ui.children(id) {
            ui.destroy(child);
            ui.delete(child);
        }
        ui.base_destroy(id);
    }

    /// `~WinControl`: the capture released, then as `Destroy`.
    pub fn on_delete(ui: &mut Ui, id: NodeId, this: &mut WinControl) {
        if this.captured {
            this.captured = false;
            ui.remove_capture(id);
        }
        destroy(ui, id, this);
    }

    fn captured(w: &dyn Widget) -> bool {
        w.win_control().is_some_and(|c| c.captured)
    }

    /// `WinControl::MouseEvent`: a capturing control takes every left
    /// button event; otherwise the event is taken when the mouse ray hits
    /// (slot 4).
    pub fn mouse_event(ui: &mut Ui, id: NodeId, w: &mut dyn Widget, event: MouseButton) -> bool {
        if captured(w) && event.field0() == -1 {
            return true;
        }
        let ray = ui.mouse_ray();
        w.hit_test(ui, id, ray)
    }

    /// `WinControl::Slot23`: 0xEF01 moves the control to its parent (slot
    /// 5) and shows it, 0xEF02 hides it.
    pub fn broadcast(ui: &mut Ui, id: NodeId, w: &mut dyn Widget, msg: u32) -> bool {
        match msg {
            MSG_SHOW => {
                if let Some(parent) = ui.parent(id) {
                    let position = node_position(ui, parent);
                    w.set_position(ui, id, position);
                }
                if let Some(n) = ui.node_mut(id) {
                    n.flags |= ACTIVE | VISIBLE;
                }
                true
            }
            MSG_HIDE => {
                if let Some(n) = ui.node_mut(id) {
                    n.flags &= !(ACTIVE | VISIBLE);
                }
                true
            }
            _ => false,
        }
    }

    /// `WinControl::CaptureMouse`.
    pub fn capture_mouse(ui: &mut Ui, id: NodeId, this: &mut WinControl) {
        if !this.captured {
            this.captured = true;
            ui.push_capture(id);
        }
    }

    /// `WinControl::ReleaseMouse`.
    pub fn release_mouse(ui: &mut Ui, id: NodeId, this: &mut WinControl) {
        if this.captured {
            this.captured = false;
            ui.remove_capture(id);
        }
    }

    /// `WinControl::SetOffset`: every child control hears the change
    /// (`AddAttachOffset`), then the control is placed (slot 5) at its
    /// parent's position (or its own).
    pub fn set_offset(ui: &mut Ui, id: NodeId, w: &mut dyn Widget, offset: Vector) {
        let Some(this) = w.win_control_mut() else {
            return;
        };
        let sub = |a: f32, b: f32| (f64::from(a) - f64::from(b)) as f32;
        let delta = Vector::new(
            sub(offset.x, this.offset.x),
            sub(offset.y, this.offset.y),
            sub(offset.z, this.offset.z),
        );
        this.offset = offset;
        for child in ui.children(id) {
            if let Some(c) = control(ui, child) {
                c.add_attach_offset(delta);
            }
        }
        let position = match ui.parent(id) {
            Some(parent) => node_position(ui, parent),
            None => node_position(ui, id),
        };
        w.set_position(ui, id, position);
    }

    /// `WinControl::SetMesh`: a different path loads a new instance, placed
    /// at the position plus the offset; then `RefreshAttachment`.
    pub fn set_mesh(
        ui: &mut Ui,
        id: NodeId,
        this: &mut WinControl,
        path: &[u8],
        material_path: &[u8],
    ) {
        if this.mesh.is_some() && neuron_core::hash::eq_ignore_case(&this.mesh_path, path) {
            return;
        }
        this.mesh_path = path.to_vec();
        this.mesh = None;
        if !path.is_empty()
            && let Some(loader) = ui.mesh_loader.as_mut()
        {
            this.mesh = loader.load(path, material_path);
        }
        let position = node_position(ui, id);
        if let Some(mesh) = this.mesh.as_mut() {
            mesh.set_transform(Transform::new(
                Matrix::IDENTITY,
                add3(this.offset, position),
            ));
        }
        refresh_attachment(ui, id, this);
    }

    /// `WinControl::RefreshAttachment`: the children first, then this
    /// control's attachment to its parent's hardpoint, then the parent
    /// placed at its own position.
    pub fn refresh_attachment(ui: &mut Ui, id: NodeId, this: &mut WinControl) {
        for child in ui.children(id) {
            ui.with_widget(child, (), |w, ui| {
                if let Some(c) = w.win_control_mut() {
                    refresh_attachment(ui, child, c);
                }
            });
        }
        let Some(parent) = ui.parent(id) else { return };
        this.attach_orientation = Matrix::IDENTITY;
        this.attach_position = Vector::ZERO;
        let hardpoint = this.hardpoint.clone();
        if let Some(p) = ui.win_control(parent) {
            let (found, transform) = p.hardpoint_transform(&hardpoint);
            this.attach_orientation = transform.rotation;
            this.attach_position = transform.translation;
            this.attached = found;
        }
        let position = node_position(ui, parent);
        ui.set_position(parent, position);
    }

    /// `WinControl::SetHiddenWithChildren`: every child control hears
    /// property 1, then this control is hidden or shown.
    pub fn set_hidden_with_children(ui: &mut Ui, id: NodeId, hide: bool) {
        for child in ui.children(id) {
            if ui.win_control(child).is_some() {
                ui.property(child, Property::Hidden(hide));
            }
        }
        if let Some(n) = ui.node_mut(id) {
            if hide {
                n.flags &= !(ACTIVE | VISIBLE);
            } else {
                n.flags |= ACTIVE | VISIBLE;
            }
        }
    }

    /// `WinControl::SetProperty`, messages 1 to 11.
    pub fn property(ui: &mut Ui, id: NodeId, w: &mut dyn Widget, property: Property<'_>) -> isize {
        if let Property::Position(position) = property {
            w.set_position(ui, id, position);
            return 1;
        }
        let Some(this) = w.win_control_mut() else {
            return 0;
        };
        match property {
            Property::Hidden(hide) => {
                if let Some(n) = ui.node_mut(id) {
                    if hide {
                        n.flags &= !(ACTIVE | VISIBLE);
                    } else {
                        n.flags |= ACTIVE | VISIBLE;
                    }
                }
                1
            }
            Property::Inactive(inactive) => {
                if let Some(n) = ui.node_mut(id) {
                    n.flags = (n.flags & !ACTIVE) | u8::from(!inactive);
                }
                1
            }
            Property::ToolTip(ids) => {
                this.tooltip_ids = ids;
                1
            }
            Property::Mesh(path, material) => {
                set_mesh(ui, id, this, path, material);
                1
            }
            Property::Position(_) => 1,
            Property::Offset(offset) => {
                this.offset = offset;
                let placed = this.placed(node_position(ui, id));
                if let Some(mesh) = this.mesh.as_mut() {
                    mesh.set_position(placed);
                }
                0
            }
            Property::GetOffset(out) => {
                *out = this.offset;
                0
            }
            Property::InstanceOrientation(orientation) => {
                let position = add3(node_position(ui, id), this.offset);
                if let Some(mesh) = this.mesh.as_mut() {
                    mesh.set_transform(Transform::new(orientation, position));
                }
                1
            }
            Property::Field2d2(value) => {
                this.field_2d2 = value;
                1
            }
            Property::Instance => isize::from(this.mesh.is_some()),
            Property::GetMeshPath(out, len) => {
                // strncpy: the path cut at `len`, padded with NULs.
                let mut bytes = this.mesh_path.clone();
                bytes.resize(len, 0);
                *out = bytes;
                0
            }
            Property::ChildHit(_)
            | Property::ChildToolTip(_)
            | Property::ChildButtonUp(_)
            | Property::ScrollRange(..)
            | Property::ScrollValue(_)
            | Property::GetScrollValue
            | Property::ScrollChanged(..)
            | Property::Other(..) => 0,
        }
    }

    /// `WinControl::LoadIni`: the `[BaseFrame]` keys, then every further
    /// section with a `class_name` built through the factory as a child
    /// named after the section, the reader handed to its `Create`.
    pub fn load_ini(ui: &mut Ui, id: NodeId, this: &mut WinControl, ini: &mut IniReader) {
        if !ini.find_header(b"BaseFrame") || !ini.is_header(b"BaseFrame") {
            return;
        }
        while ini.read_value() {
            if ini.is_value(b"behavior") || ini.is_value(b"subcontrol") {
                continue;
            }
            if ini.is_value(b"mesh") {
                let path = ini.value_string();
                set_mesh(ui, id, this, &path, b"");
                continue;
            }
            if ini.is_value(b"offset") {
                let y = ini.value_float(1) as f32;
                let x = ini.value_float(0) as f32;
                let z = ini.value_float(2) as f32;
                this.offset = Vector::new(x, y, z);
                let position = add3(this.offset, node_position(ui, id));
                if let Some(mesh) = this.mesh.as_mut() {
                    mesh.set_position(position);
                }
                continue;
            }
            if ini.is_value(b"mouse_size") {
                let y = ini.value_float(1) as f32;
                let x = ini.value_float(0) as f32;
                let z = ini.value_float(2) as f32;
                this.half_extents = Vector::new(x, y, z);
                continue;
            }
            if ini.is_value(b"no_z_enable") {
                this.no_z_enable = ini.value_float(0) != 0.0;
                continue;
            }
            if ini.is_value(b"z_order") {
                let layer = trunc_to_i32(ini.value_float(0)).cast_unsigned();
                if let Some(n) = ui.node_mut(id) {
                    n.draw_layer = layer;
                }
                continue;
            }
            if ini.is_value(b"tooltip") {
                this.tooltip_ids = trunc_to_i32(ini.value_float(0));
            }
        }
        while ini.read_header() {
            let section = ini.header();
            while ini.read_value() {
                if ini.is_value(b"class_name") {
                    let class = ini.value_string();
                    ui.create_by_class(
                        Some(&section),
                        &class,
                        Some(id),
                        &mut Payload::Ini(&mut *ini),
                    );
                    break;
                }
            }
        }
    }
}

impl WinControl {
    /// `WinControl::GetHardpointTransform`: the hardpoint of the mesh with
    /// the offset added, or identity at the offset (false).
    #[must_use]
    pub fn hardpoint_transform(&self, name: &[u8]) -> (bool, Transform) {
        if let Some(found) = self.mesh.as_ref().and_then(|m| m.hardpoint(name)) {
            let mut t = found;
            t.translation = add3(self.offset, t.translation);
            return (true, t);
        }
        (
            false,
            Transform::new(Matrix::IDENTITY, add3(self.offset, Vector::ZERO)),
        )
    }

    /// `WinControl::AddAttachOffset`.
    pub fn add_attach_offset(&mut self, delta: Vector) {
        self.attach_position = add3(self.attach_position, delta);
    }

    /// `WinControl::GetAttachOrientation`: the attach orientation while
    /// attached to a `WinControl` parent, else identity.
    #[must_use]
    pub fn attach_orientation(&self, ui: &Ui, id: NodeId) -> Matrix {
        match ui.parent(id) {
            Some(p) if self.attached && ui.win_control(p).is_some() => self.attach_orientation,
            _ => Matrix::IDENTITY,
        }
    }

    /// `WinControl::GetAttachPosition`.
    #[must_use]
    pub fn attach_position(&self, ui: &Ui, id: NodeId) -> Vector {
        match ui.parent(id) {
            Some(p) if self.attached && ui.win_control(p).is_some() => self.attach_position,
            _ => Vector::ZERO,
        }
    }
}

/// `WinControl::FindChild`: the first child whose name equals `name`
/// (case-sensitive), if it is a control.
#[must_use]
pub fn find_child(ui: &Ui, id: NodeId, name: &[u8]) -> Option<NodeId> {
    let child = ui
        .children(id)
        .into_iter()
        .find(|&c| ui.node(c).is_some_and(|n| n.name() == name))?;
    ui.win_control(child).map(|_| child)
}

/// The "WinControl" factory product: a bare control.
#[derive(Debug, Default)]
pub struct PlainControl {
    pub control: WinControl,
}

impl Widget for PlainControl {
    fn as_any(&self) -> &dyn std::any::Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn std::any::Any {
        self
    }
    fn win_control(&self) -> Option<&WinControl> {
        Some(&self.control)
    }
    fn win_control_mut(&mut self) -> Option<&mut WinControl> {
        Some(&mut self.control)
    }
    fn hit_test(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) -> bool {
        base::hit_test(ui, id, &self.control, ray)
    }
    fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
        base::set_position(ui, id, &mut self.control, position);
    }
    fn set_orientation(&mut self, ui: &mut Ui, id: NodeId, orientation: Matrix) {
        base::set_orientation(ui, id, &mut self.control, orientation);
    }
    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        base::destroy(ui, id, &mut self.control);
    }
    fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        base::on_delete(ui, id, &mut self.control);
    }
    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        ui.draw_children(id, host);
    }
    fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        base::mouse_event(ui, id, self, event)
    }
    fn broadcast(&mut self, ui: &mut Ui, id: NodeId, msg: u32, _a: u32, _b: u32) -> bool {
        base::broadcast(ui, id, self, msg)
    }
    fn property(&mut self, ui: &mut Ui, id: NodeId, property: Property<'_>) -> isize {
        base::property(ui, id, self, property)
    }
}

/// Loads a `[BaseFrame]` file into the control `id` (slot 28).
pub fn load_ini(ui: &mut Ui, id: NodeId, ini: &mut IniReader) {
    ui.with_widget(id, (), |w, ui| {
        if let Some(c) = w.win_control_mut() {
            base::load_ini(ui, id, c, ini);
        }
    });
}
