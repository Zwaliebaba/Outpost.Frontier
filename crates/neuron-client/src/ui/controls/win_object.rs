//! `WinObject` (`win_object.cpp`) and `UIWinObject` (`ui_win_object.cpp`).
//!
//! A `WinObject` is a `Win3D` carrying one engine mesh instance placed at
//! its position, moved to its transform each update and drawn after its
//! children: a plain pass with depth test less-equal and depth writes, and
//! a tinted pass when bit 0 of its draw flags is set (bit 1 skips the plain
//! pass and the tinted pass's lighting). `UIWinObject` is the page
//! descriptor's framed panel: its mesh is the payload's path, and the page
//! transform places it at the centre of its box scaled onto the payload's
//! depth, upright.

use std::any::Any;

use neuron_core::math::{Matrix, Transform, Vector, transform_matrix, transform_xform};

use crate::ui::control::ControlMesh;
use crate::ui::immediate::render_state;
use crate::ui::page::EntryPayload;
use crate::ui::tree::{DrawHost, MeshPass, NodeId, Payload, Ui, VISIBLE, Widget};

/// The `WinObject` part.
#[derive(Default)]
pub struct WinObject {
    /// The holder's engine instance.
    pub mesh: Option<Box<dyn ControlMesh>>,
    /// `+0xa4`: bit 0 the tinted pass, bit 1 no plain pass and no lighting.
    pub draw_flags: u8,
    /// `+0xa5`: the tint, R, G, B, A.
    pub tint: [u8; 4],
}

impl std::fmt::Debug for WinObject {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("WinObject")
            .field("has_mesh", &self.mesh.is_some())
            .field("draw_flags", &self.draw_flags)
            .field("tint", &self.tint)
            .finish()
    }
}

impl WinObject {
    /// `ClientMeshLoadFromPath` through the manager's loader.
    pub fn load(&mut self, ui: &mut Ui, path: &[u8], material: &[u8]) -> bool {
        self.mesh = ui
            .mesh_loader
            .as_mut()
            .and_then(|loader| loader.load(path, material));
        self.mesh.is_some()
    }

    /// `WinObject::LoadMeshAndCreate`: the mesh, then `Win3D::Create`.
    pub fn load_and_create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        path: &[u8],
        material: &[u8],
    ) -> bool {
        self.load(ui, path, material) && ui.base_create(id, parent)
    }

    /// `WinObject::SetPosition`: stored and given to the instance.
    pub fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
        ui.base_set_position(id, position);
        if let Some(mesh) = self.mesh.as_mut() {
            mesh.set_position(position);
        }
    }

    /// `WinObject::Destroy`: the instance released, then as `Win3D`.
    pub fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        self.mesh = None;
        ui.base_destroy(id);
    }

    /// `WinObject::Update`: the children, then the instance moved to the
    /// orientation and position (divergence D433).
    pub fn update(&mut self, ui: &mut Ui, id: NodeId) {
        ui.base_update(id);
        if let (Some(mesh), Some(n)) = (self.mesh.as_mut(), ui.node(id)) {
            mesh.set_transform(Transform::new(n.orientation, n.position));
        }
    }

    /// `WinObject::Draw`: visible only; the children, then the passes.
    pub fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        if ui.node(id).is_none_or(|n| n.flags & VISIBLE == 0) {
            return;
        }
        ui.draw_children(id, host);
        let Some(mesh) = self.mesh.as_deref() else {
            return;
        };
        if self.draw_flags & 2 == 0 {
            if let Some(imm) = host.immediate() {
                imm.set_render_state(render_state::ZFUNC, 8);
                imm.set_render_state(render_state::ZWRITEENABLE, 1);
            }
            host.submit_mesh(mesh, MeshPass::Plain);
        }
        if self.draw_flags & 1 != 0 {
            host.submit_mesh(
                mesh,
                MeshPass::Tinted {
                    tint: self.tint,
                    lighting: (!self.draw_flags >> 1) & 1 != 0,
                },
            );
        }
    }
}

/// `UIWinObject`.
#[derive(Debug)]
pub struct UiWinObject {
    pub object: WinObject,
    /// The payload's width, height and fifth float (the depth).
    pub width: f32,
    pub height: f32,
    pub depth: f32,
    /// `+0xb8`: the last page transform.
    pub page_block: Transform,
    /// `+0xe8`: the block's origin.
    pub origin: Vector,
    /// `+0xf4`: the block's `(width, -height, 0)`.
    pub corner: Vector,
}

impl Default for UiWinObject {
    fn default() -> Self {
        UiWinObject {
            object: WinObject::default(),
            width: 0.0,
            height: 0.0,
            depth: 0.0,
            page_block: Transform::IDENTITY,
            origin: Vector::ZERO,
            corner: Vector::ZERO,
        }
    }
}

impl UiWinObject {
    /// The position `SetTransform` gives the object: the box's centre in
    /// the block scaled onto the depth plane.
    #[must_use]
    pub fn placement(&self, block: &Transform) -> Vector {
        let half = Vector::new(
            (f64::from(self.width) * 0.5) as f32,
            (f64::from(self.height) * -0.5) as f32,
            0.0,
        );
        let rotated = transform_matrix(&block.rotation, half);
        let t = block.translation;
        let add = |a: f32, b: f32| (f64::from(a) + f64::from(b)) as f32;
        let center = Vector::new(
            add(rotated.x, t.x),
            add(rotated.y, t.y),
            add(rotated.z, t.z),
        );
        let scale = f64::from(self.depth) / f64::from(t.z);
        Vector::new(
            (f64::from(center.x) * scale) as f32,
            (f64::from(center.y) * scale) as f32,
            self.depth,
        )
    }
}

impl Widget for UiWinObject {
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }

    /// `UIWinObject::SetPosition`: `WinObject`'s, then the instance updated.
    fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
        self.object.set_position(ui, id, position);
    }

    /// `UIWinObject::SetTransform`: the block kept, its origin and corner,
    /// then upright at the placement.
    fn set_transform(&mut self, ui: &mut Ui, id: NodeId, transform: &Transform) {
        self.page_block = *transform;
        let position = self.placement(transform);
        self.origin = transform_xform(transform, Vector::ZERO);
        self.corner = transform_xform(transform, Vector::new(self.width, -self.height, 0.0));
        self.set_orientation(ui, id, Matrix::IDENTITY);
        self.set_position(ui, id, position);
    }

    /// `UIWinObject::Create`: the payload's size, depth and mesh; fails
    /// without a payload or a mesh.
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        payload: &mut Payload<'_>,
    ) -> bool {
        let Payload::Page(EntryPayload::WinObject {
            right,
            bottom,
            extra_name,
            extra_float,
        }) = payload
        else {
            return false;
        };
        self.width = *right;
        self.height = *bottom;
        self.depth = *extra_float;
        let path = extra_name.clone();
        self.object.load(ui, &path, b"") && ui.base_create(id, parent)
    }

    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        self.object.destroy(ui, id);
    }

    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        self.object.update(ui, id);
    }

    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        self.object.draw(ui, id, host);
    }
}
