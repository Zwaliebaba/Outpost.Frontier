//! `UITable` (`ui_table.cpp`): a grid of a page descriptor whose row and
//! column edges the mouse can drag, with one rich-text window per cell
//! laid out on the grid, a hovered and a selected row drawn as quads and a
//! line frame.
//!
//! The table is a `Win3D` (not a `WinControl`). The mouse ray is taken onto
//! its plane and into table space through the inverse of its transform; an
//! edge within the grab distance sets the system's resize cursor and a
//! press drags it, clamped between its neighbours; otherwise the row under
//! the ray is hovered and a press selects it. The cells are the owner's
//! windows; the table places them (their projection waits for their first
//! draw, divergence D423).

use std::any::Any;

use neuron_core::math::{
    Matrix, Transform, Vector, general_inverse, mul_transform, transform_xform,
};

use crate::ui::controls::rich_text_win::RichTextWin;
use crate::ui::immediate::{primitive, render_state};
use crate::ui::message::MouseButton;
use crate::ui::page::EntryPayload;
use crate::ui::services::SystemCursor;
use crate::ui::tree::{DrawHost, NodeId, Payload, Ui, Widget};

/// One row's flags (`FUiTableRow`; the payload handle is the owner's).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TableRow {
    /// Update hovers only rows with this set.
    pub hoverable: bool,
    /// A press selects only rows with this set.
    pub selectable: bool,
}

/// `UITable`.
#[derive(Clone, Debug, PartialEq)]
pub struct UiTable {
    pub width: f32,
    pub height: f32,
    /// `Columns * Row + Col`: the cells' windows.
    pub cells: Vec<Option<NodeId>>,
    /// y, 0 at the top, going negative.
    pub row_edges: Vec<f32>,
    /// x, 0 at the left.
    pub column_edges: Vec<f32>,
    pub rows: Vec<TableRow>,
    pub field_c4: u8,
    pub selected: bool,
    pub selected_row: u32,
    pub hovered: bool,
    pub hovered_row: u32,
    pub frame_rgba: [u8; 4],
    pub selected_rgba: [u8; 4],
    pub hovered_rgba: [u8; 4],
    pub grab_distance: f32,
    pub columns_resizable: bool,
    pub rows_resizable: bool,
    /// The inverse of the orientation and position.
    pub inverse: Transform,
    pub drag_armed: bool,
    pub dragging: bool,
    pub drag_column: bool,
    pub drag_edge: u32,
    pub drag_offset: f32,
}

impl Default for UiTable {
    fn default() -> Self {
        UiTable {
            width: 0.0,
            height: 0.0,
            cells: Vec::new(),
            row_edges: Vec::new(),
            column_edges: Vec::new(),
            rows: Vec::new(),
            field_c4: 1,
            selected: false,
            selected_row: 0,
            hovered: false,
            hovered_row: 0,
            frame_rgba: [0x00, 0xFF, 0x00, 0xFF],
            selected_rgba: [0xA0, 0x00, 0x00, 0xA0],
            hovered_rgba: [0x80, 0x80, 0x80, 0x80],
            grab_distance: 0.004,
            columns_resizable: true,
            rows_resizable: true,
            inverse: Transform::IDENTITY,
            drag_armed: false,
            dragging: false,
            drag_column: false,
            drag_edge: 0,
            drag_offset: 0.0,
        }
    }
}

fn node_transform(ui: &Ui, id: NodeId) -> Transform {
    ui.node(id).map_or(Transform::IDENTITY, |n| {
        Transform::new(n.orientation, n.position)
    })
}

/// The seven render states both halves of the draw set.
fn set_draw_states(host: &mut dyn DrawHost) {
    let Some(imm) = host.immediate() else { return };
    imm.set_render_state(render_state::ZENABLE, 0);
    imm.set_render_state(render_state::ZWRITEENABLE, 0);
    imm.set_render_state(render_state::CULLMODE, 1);
    imm.set_render_state(render_state::LIGHTING, 0);
    imm.set_render_state(render_state::SRCBLEND, 5);
    imm.set_render_state(render_state::DESTBLEND, 6);
    imm.set_render_state(render_state::ALPHABLENDENABLE, 1);
}

impl UiTable {
    /// The ray on the table's plane, in table space.
    #[must_use]
    pub fn ray_to_local(&self, ui: &Ui, id: NodeId, ray: Vector) -> Vector {
        let z = ui.node(id).map_or(0.0, |n| n.position.z);
        let scale = f64::from(z) / f64::from(ray.z);
        let scaled = Vector::new(
            (f64::from(ray.x) * scale) as f32,
            (f64::from(ray.y) * scale) as f32,
            (f64::from(ray.z) * scale) as f32,
        );
        transform_xform(&self.inverse, scaled)
    }

    /// `SetColumnEdge`: edge `index` (1 and up) at `x`, clamped between its
    /// neighbours; false for edge 0 or out of range.
    pub fn set_column_edge(&mut self, ui: &mut Ui, id: NodeId, index: u32, x: f32) -> bool {
        let n = self.column_edges.len();
        let i = index as usize;
        if index == 0 || n == 0 || i >= n {
            return false;
        }
        let prev = i - 1;
        let count = if prev + 2 > n { n - prev } else { 2 };
        let gap2 = self.grab_distance + self.grab_distance;
        let lo = gap2 + self.column_edges[prev];
        let next = if i + 1 == n {
            self.width
        } else {
            self.column_edges[i + 1]
        };
        let hi = next - gap2;
        let x = if x < lo {
            lo
        } else if x > hi {
            hi
        } else {
            x
        };
        if x != self.column_edges[i] {
            self.column_edges[i] = x;
            self.layout(ui, id, 0, 0, prev as u32, count as u32);
        }
        true
    }

    /// `SetRowEdge`: the same for a row edge (the last edge only ever lands
    /// on its lower limit, as in the reference).
    pub fn set_row_edge(&mut self, ui: &mut Ui, id: NodeId, index: u32, y: f32) -> bool {
        let n = self.row_edges.len();
        let i = index as usize;
        if index == 0 || n == 0 || i >= n {
            return false;
        }
        let prev = i - 1;
        let count = if prev + 2 > n { n - prev } else { 2 };
        let gap2 = self.grab_distance + self.grab_distance;
        let lo = self.row_edges[prev] - gap2;
        let next = if i + 1 == n {
            self.height
        } else {
            self.row_edges[i + 1]
        };
        let hi = f64::from(next) + f64::from(gap2);
        let y = if f64::from(y) < hi {
            hi as f32
        } else if y > lo {
            lo
        } else {
            y
        };
        if self.row_edges[i] != y {
            self.row_edges[i] = y;
            self.layout(ui, id, prev as u32, count as u32, 0, 0);
        }
        true
    }

    /// `Layout`: the cells of the rows and columns given (0: to the end)
    /// placed on the grid and sized.
    pub fn layout(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        row_start: u32,
        row_count: u32,
        col_start: u32,
        col_count: u32,
    ) {
        let (nr, nc) = (self.row_edges.len() as u32, self.column_edges.len() as u32);
        if nr == 0 || row_start >= nr || nc == 0 || col_start >= nc {
            return;
        }
        let row_count = if row_count == 0 {
            nr - row_start
        } else {
            row_count
        };
        let col_count = if col_count == 0 {
            nc - col_start
        } else {
            col_count
        };
        let this = node_transform(ui, id);
        for row in row_start..row_start + row_count {
            let r = row as usize;
            let row_height = if row < nr - 1 {
                self.row_edges[r] - self.row_edges[r + 1]
            } else {
                self.row_edges[r] + self.height
            };
            for col in col_start..col_start + col_count {
                let c = col as usize;
                let flat = (nc * row + col) as usize;
                let Some(Some(cell)) = (row < nr && col < nc)
                    .then(|| self.cells.get(flat).copied())
                    .flatten()
                else {
                    continue;
                };
                let offset = Transform::new(
                    Matrix::IDENTITY,
                    Vector::new(self.column_edges[c], self.row_edges[r], 0.0),
                );
                let world = mul_transform(&this, &offset);
                ui.set_transform(cell, &world);
                let column_width = if col < nc - 1 {
                    self.column_edges[c + 1] - self.column_edges[c]
                } else {
                    self.width - self.column_edges[c]
                };
                if let Some(w) = ui.widget_mut::<RichTextWin>(cell) {
                    w.base.set_norm_size(column_width, row_height);
                }
            }
        }
    }

    /// The table space inverse and the whole layout.
    fn refresh(&mut self, ui: &mut Ui, id: NodeId) {
        self.inverse = general_inverse(&node_transform(ui, id)).transform;
        self.layout(ui, id, 0, 0, 0, 0);
    }

    /// `UITable::Create`: the payload's size and counts, evenly spaced
    /// edges.
    pub fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        payload: &Payload<'_>,
    ) -> bool {
        ui.base_create(id, parent);
        if let Payload::Page(EntryPayload::Table {
            right,
            bottom,
            int5,
            int6,
        }) = payload
        {
            self.width = *right;
            self.height = *bottom;
            let rows = *int5 as u32 as usize;
            let cols = *int6 as u32 as usize;
            self.row_edges.resize(rows, 0.0);
            self.column_edges.resize(cols, 0.0);
            self.cells.resize(rows.wrapping_mul(cols), None);
            self.rows.resize(
                rows,
                TableRow {
                    hoverable: true,
                    selectable: true,
                },
            );
        }
        if !self.column_edges.is_empty() && !self.row_edges.is_empty() {
            let row_step = (f64::from(self.height) / f64::from(self.row_edges.len() as u32)) as f32;
            let col_step =
                (f64::from(self.width) / f64::from(self.column_edges.len() as u32)) as f32;
            for (i, e) in self.row_edges.iter_mut().enumerate() {
                *e = (-(f64::from(i as u32) * f64::from(row_step))) as f32;
            }
            for (i, e) in self.column_edges.iter_mut().enumerate() {
                *e = (f64::from(i as u32) * f64::from(col_step)) as f32;
            }
        }
        true
    }

    /// `UITable::Update`.
    pub fn update(&mut self, ui: &mut Ui, id: NodeId) {
        ui.base_update(id);
        if ui.capture() == Some(id) && self.dragging {
            let ray = ui.mouse_ray();
            let local = self.ray_to_local(ui, id, ray);
            if self.drag_column {
                self.set_column_edge(ui, id, self.drag_edge, local.x + self.drag_offset);
                ui.services.set_system_cursor(SystemCursor::ColumnResize);
            } else {
                self.set_row_edge(ui, id, self.drag_edge, local.y + self.drag_offset);
                ui.services.set_system_cursor(SystemCursor::RowResize);
            }
            return;
        }
        if ui.capture().is_some() {
            return;
        }
        let ray = ui.mouse_ray();
        let local = self.ray_to_local(ui, id, ray);
        self.hovered = false;
        self.drag_armed = false;
        self.dragging = false;
        if !(local.x >= 0.0) || !(local.x < self.width) || !(local.y <= 0.0) {
            return;
        }
        let bottom = -self.height;
        if !(local.y > bottom) {
            return;
        }
        if self.columns_resizable {
            for i in 1..self.column_edges.len() {
                let edge = self.column_edges[i];
                if f64::from(self.grab_distance) > (f64::from(local.x) - f64::from(edge)).abs() {
                    ui.services.set_system_cursor(SystemCursor::ColumnResize);
                    self.drag_edge = i as u32;
                    self.drag_armed = true;
                    self.drag_offset = edge - local.x;
                    self.drag_column = true;
                    return;
                }
            }
        }
        let mut i = 0;
        while i < self.row_edges.len() {
            let edge = self.row_edges[i];
            let next = self.row_edges.get(i + 1).copied().unwrap_or(bottom);
            if self.rows_resizable
                && i > 0
                && f64::from(self.grab_distance) > (f64::from(local.y) - f64::from(edge)).abs()
            {
                ui.services.set_system_cursor(SystemCursor::RowResize);
                self.drag_edge = i as u32;
                self.drag_column = false;
                self.drag_offset = edge - local.y;
                self.drag_armed = true;
                return;
            }
            if !(local.y <= edge) || !(local.y > next) {
                i += 1;
                continue;
            }
            if self.rows.get(i).is_some_and(|r| r.hoverable) {
                self.hovered_row = i as u32;
                self.hovered = true;
            }
            break;
        }
        ui.services.set_system_cursor(SystemCursor::Default);
    }

    fn draw_row_quad(&self, host: &mut dyn DrawHost, rgba: [u8; 4], row: u32) {
        let Some(imm) = host.immediate() else { return };
        let r = row as usize;
        let (Some(&c0), Some(&top)) = (self.column_edges.first(), self.row_edges.get(r)) else {
            return;
        };
        imm.color4ubv(rgba);
        imm.begin(primitive::QUADS);
        imm.vertex(c0, top, 0.0);
        imm.vertex(self.width, top, 0.0);
        if let Some(&next) = self.row_edges.get(r + 1) {
            imm.vertex(self.width, next, 0.0);
            imm.vertex(c0, next, 0.0);
        } else {
            imm.vertex(self.width, -self.height, 0.0);
            imm.vertex(c0, -self.height, 0.0);
        }
        imm.end();
    }

    /// `UITable::Draw`.
    pub fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        let t = node_transform(ui, id);
        if let Some(imm) = host.immediate() {
            imm.bind_modelview(&t);
        }
        set_draw_states(host);
        if self.selected {
            self.draw_row_quad(host, self.selected_rgba, self.selected_row);
        }
        if self.hovered {
            self.draw_row_quad(host, self.hovered_rgba, self.hovered_row);
        }
        ui.draw_children(id, host);
        if let Some(imm) = host.immediate() {
            imm.bind_modelview(&t);
        }
        set_draw_states(host);
        let Some(imm) = host.immediate() else { return };
        imm.color4ubv(self.frame_rgba);
        imm.begin(primitive::LINE_LOOP);
        imm.vertex(0.0, 0.0, 0.0);
        imm.vertex(self.width, 0.0, 0.0);
        imm.vertex(self.width, -self.height, 0.0);
        imm.vertex(0.0, -self.height, 0.0);
        imm.end();
        imm.begin(primitive::LINES);
        for &e in &self.row_edges {
            imm.vertex(0.0, e, 0.0);
            imm.vertex(self.width, e, 0.0);
        }
        for &e in &self.column_edges {
            imm.vertex(e, 0.0, 0.0);
            imm.vertex(e, -self.height, 0.0);
        }
        imm.end();
    }

    /// `UITable::MouseEvent`: a press selects the hovered row or starts an
    /// edge drag; a release ends the drag; anything else goes to the
    /// parent.
    pub fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        if event.field0() == -1 {
            if event.field1() == 2 {
                if self.hovered {
                    let row = self.hovered_row as usize;
                    if self
                        .rows
                        .get(row)
                        .is_some_and(|r| r.selectable && r.hoverable)
                        && row < self.row_edges.len()
                    {
                        self.selected_row = row as u32;
                        self.selected = true;
                    }
                    return true;
                }
                if self.drag_armed {
                    ui.push_capture(id);
                    self.dragging = true;
                    return true;
                }
            } else if ui.capture() == Some(id) && self.dragging {
                self.dragging = false;
                ui.remove_capture(id);
                return true;
            }
        }
        ui.base_mouse_event(id, event)
    }

    /// `UITable::HitTest`: the ray inside `[0, width)` by `(-height, 0]`.
    #[must_use]
    pub fn hit_test(&self, ui: &Ui, id: NodeId, ray: Vector) -> bool {
        let local = self.ray_to_local(ui, id, ray);
        local.x >= 0.0 && local.x < self.width && local.y <= 0.0 && -self.height < local.y
    }
}

impl Widget for UiTable {
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
    fn hit_test(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) -> bool {
        UiTable::hit_test(self, ui, id, ray)
    }
    /// The position, then the inverse and the whole layout.
    fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
        ui.base_set_position(id, position);
        self.refresh(ui, id);
    }
    /// Slots 7 and 5, then the inverse and the whole layout.
    fn set_transform(&mut self, ui: &mut Ui, id: NodeId, transform: &Transform) {
        self.set_orientation(ui, id, transform.rotation);
        self.set_position(ui, id, transform.translation);
        self.refresh(ui, id);
    }
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        payload: &mut Payload<'_>,
    ) -> bool {
        UiTable::create(self, ui, id, parent, payload)
    }
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        UiTable::update(self, ui, id);
    }
    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        UiTable::draw(self, ui, id, host);
    }
    fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        UiTable::mouse_event(self, ui, id, event)
    }
}
