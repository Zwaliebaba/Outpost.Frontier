//! `UIFastTable` and `UIScrollTable` (`ui_fast_table.cpp`): a grid drawn as
//! one rich-text surface. Every cell keeps a source list; the window's
//! slot 45 projects the row and column edges to pixels and lays each
//! cell's list out inside its rectangle less the margins, behind a clip
//! node, chaining the cells into the window's layout list. Row and column
//! edges are dragged with the mouse, rows are hovered and selected, and
//! tooltips can be keyed per column, row or cell. The scroll table adds a
//! vertical bar that pages through more items than there are rows and
//! tells the owner the first item shown.
//!
//! [`UiTable`](super::table::UiTable) is the older sibling with one
//! window per cell.

use std::any::Any;
use std::rc::Rc;

use neuron_core::math::{Matrix, Transform, Vector, general_inverse, transform_xform};
use neuron_core::num::trunc_to_i32;

use crate::text::font::FontSystem;
use crate::text::layout::device::DeviceTrcCx;
use crate::text::layout::layout_rich_text;
use crate::text::layout::list::LayoutParams;
use crate::text::rdl::{ClipRect, RdlNode, RenderDisplayList};
use crate::ui::control::{WinControl, base};
use crate::ui::controls::rich_text_win::{BaseRichTextWin, RichTextCx, place_scroll_bar};
use crate::ui::controls::scroll::UiScroll;
use crate::ui::immediate::{primitive, render_state};
use crate::ui::message::MouseButton;
use crate::ui::page::EntryPayload;
use crate::ui::pulse::palette;
use crate::ui::services::{HoverKey, ToolTipHover};
use crate::ui::tree::{ChildMessage, DrawHost, NodeId, Payload, Property, Ui, VISIBLE, Widget};

/// One cell (`FUiFastTableCell`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct FastTableCell {
    /// `+0x00`: what the owner puts in the cell.
    pub source: RenderDisplayList,
    /// `+0x0c`: slot 45's output, a clip node and the laid text.
    pub laid: Rc<RenderDisplayList>,
    /// `+0x18`: the cell reaches the last column edge.
    pub span_to_end: bool,
}

/// One row (`FUiFastTableRow`).
#[derive(Clone, Default)]
pub struct FastTableRow {
    /// The update hovers only rows with this set.
    pub hoverable: bool,
    /// A selection needs this and `hoverable`.
    pub selectable: bool,
    /// The draw's separator above this row.
    pub row_line: bool,
    /// Without highlight quads the hovered row is outlined with its
    /// neighbours of the same non-zero group.
    pub group_id: u32,
    /// The owner's reference-counted payload (`UIFastTable::RefCounter`).
    pub payload: Option<Rc<dyn Any>>,
}

impl std::fmt::Debug for FastTableRow {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("FastTableRow")
            .field("hoverable", &self.hoverable)
            .field("selectable", &self.selectable)
            .field("row_line", &self.row_line)
            .field("group_id", &self.group_id)
            .field("payload", &self.payload.is_some())
            .finish()
    }
}

impl FastTableRow {
    /// `Create`'s prototype row.
    #[must_use]
    pub fn new() -> FastTableRow {
        FastTableRow {
            hoverable: true,
            selectable: true,
            row_line: true,
            group_id: 0,
            payload: None,
        }
    }
}

/// `FTableEdges`: what `GetEdges` fills and `SetEdges` reads.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct TableEdges {
    pub rows: Vec<f32>,
    pub cols: Vec<f32>,
}

impl TableEdges {
    /// The `REG_BINARY` bytes `SaveToRegistry` writes for one vector (the
    /// floats in little-endian order; the reference skips an empty one).
    #[must_use]
    pub fn edge_bytes(edges: &[f32]) -> Option<Vec<u8>> {
        if edges.is_empty() {
            return None;
        }
        Some(edges.iter().flat_map(|e| e.to_le_bytes()).collect())
    }

    /// One vector of `LoadFromRegistry` from the value's bytes: emptied and
    /// refilled with the whole floats. A missing value leaves the vector
    /// alone (divergence D477).
    pub fn load_edges(edges: &mut Vec<f32>, value: Option<&[u8]>) {
        let Some(bytes) = value else { return };
        edges.clear();
        edges.extend(
            bytes
                .as_chunks::<4>()
                .0
                .iter()
                .map(|c| f32::from_le_bytes(*c)),
        );
    }
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

fn node_transform(ui: &Ui, id: NodeId) -> Transform {
    ui.node(id).map_or(Transform::IDENTITY, |n| {
        Transform::new(n.orientation, n.position)
    })
}

/// `UIFastTable`.
#[derive(Debug)]
pub struct UiFastTable {
    pub base: BaseRichTextWin,
    /// `+0x488`, `+0x48c`: the payload's size.
    pub width: f32,
    pub height: f32,
    /// `columns * row + column`.
    pub cells: Vec<FastTableCell>,
    /// y, 0 at the top, going negative.
    pub row_edges: Vec<f32>,
    /// x, 0 at the left.
    pub column_edges: Vec<f32>,
    pub rows: Vec<FastTableRow>,
    pub selected: bool,
    pub selected_row: u32,
    /// The cell under the mouse (`u32::MAX`: none).
    pub hover_column: u32,
    pub hover_row: u32,
    pub hovered: bool,
    /// The highlighted row.
    pub hovered_row: u32,
    pub frame_rgba: [u8; 4],
    pub selected_rgba: [u8; 4],
    pub hovered_rgba: [u8; 4],
    pub grab_distance: f32,
    pub columns_resizable: bool,
    pub rows_resizable: bool,
    /// `+0x504`: the inverse of the transform.
    pub inverse: Transform,
    pub drag_armed: bool,
    pub dragging: bool,
    pub drag_column: bool,
    pub drag_edge: u32,
    pub drag_offset: f32,
    /// `+0x54c`: the cells lay out on one line.
    pub layout_flag: bool,
    pub draw_row_lines: bool,
    pub draw_column_lines: bool,
    pub draw_border: bool,
    pub border_top: bool,
    pub border_bottom: bool,
    pub border_left: bool,
    pub border_right: bool,
    pub row_highlights: bool,
    pub header_rows: u32,
    /// 0 none (the window's record), 1 per column, 2 per row, 3 per cell.
    pub tip_mode: i32,
    pub tips: Vec<ToolTipHover>,
    /// `+0x570`, `+0x574`: the cell margins in pixels.
    pub cell_margin_x: i32,
    pub cell_margin_y: i32,
}

impl Default for UiFastTable {
    fn default() -> Self {
        UiFastTable::new()
    }
}

impl UiFastTable {
    /// The constructor.
    #[must_use]
    pub fn new() -> UiFastTable {
        UiFastTable {
            base: BaseRichTextWin::new(),
            width: 0.0,
            height: 0.0,
            cells: Vec::new(),
            row_edges: Vec::new(),
            column_edges: Vec::new(),
            rows: Vec::new(),
            selected: false,
            selected_row: 0,
            hover_column: u32::MAX,
            hover_row: u32::MAX,
            hovered: false,
            hovered_row: 0,
            frame_rgba: 0xFF00_FF00u32.to_le_bytes(),
            selected_rgba: 0xA000_00A0u32.to_le_bytes(),
            hovered_rgba: 0x8080_8080u32.to_le_bytes(),
            grab_distance: 0.004,
            columns_resizable: true,
            rows_resizable: true,
            inverse: Transform::IDENTITY,
            drag_armed: false,
            dragging: false,
            drag_column: false,
            drag_edge: 0,
            drag_offset: 0.0,
            layout_flag: false,
            draw_row_lines: true,
            draw_column_lines: true,
            draw_border: true,
            border_top: true,
            border_bottom: true,
            border_left: true,
            border_right: true,
            row_highlights: true,
            header_rows: 0,
            tip_mode: 0,
            tips: Vec::new(),
            cell_margin_x: 2,
            cell_margin_y: 2,
        }
    }

    fn grid_nonempty(&self) -> bool {
        !self.row_edges.is_empty() && !self.column_edges.is_empty()
    }

    /// The flat index of a cell inside the grid.
    fn cell_index(&self, row: u32, column: u32) -> Option<usize> {
        if row as usize >= self.row_edges.len() || column as usize >= self.column_edges.len() {
            return None;
        }
        let index = (self.column_edges.len() as u32)
            .wrapping_mul(row)
            .wrapping_add(column) as usize;
        (index < self.cells.len()).then_some(index)
    }

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

    /// `SetDrawColumnLines`.
    pub fn set_draw_column_lines(&mut self, draw: bool) {
        self.draw_column_lines = draw;
    }

    /// `SetBorders`.
    pub fn set_borders(&mut self, left: bool, right: bool, top: bool, bottom: bool) {
        self.border_top = top;
        self.border_bottom = bottom;
        self.border_left = left;
        self.border_right = right;
    }

    /// `SetHeaderRows`: only below the row count.
    pub fn set_header_rows(&mut self, rows: u32) {
        if (rows as usize) < self.row_edges.len() {
            self.header_rows = rows;
        }
    }

    /// `SetRowHighlights`.
    pub fn set_row_highlights(&mut self, quads: bool) {
        self.row_highlights = quads;
    }

    /// `ClearCell`: the cell's source emptied, the layout dirtied.
    pub fn clear_cell(&mut self, row: u32, column: u32) -> bool {
        let Some(index) = self.cell_index(row, column) else {
            return false;
        };
        self.cells[index].source.nodes.clear();
        self.base.mark_dirty(true, true);
        true
    }

    /// `GetCell`.
    pub fn cell_mut(&mut self, row: u32, column: u32) -> Option<&mut FastTableCell> {
        let index = self.cell_index(row, column)?;
        Some(&mut self.cells[index])
    }

    /// `InvalidateCell`: the layout dirtied for a cell inside the grid.
    pub fn invalidate_cell(&mut self, row: u32, column: u32) {
        if (row as usize) < self.row_edges.len() && (column as usize) < self.column_edges.len() {
            self.base.mark_dirty(true, true);
        }
    }

    /// `SetCellSpanToEnd`.
    pub fn set_cell_span_to_end(&mut self, row: u32, column: u32, span: bool) {
        if let Some(index) = self.cell_index(row, column) {
            self.cells[index].span_to_end = span;
        }
    }

    /// `SetColumnEdge`: edge `index` (1 and up) at `x`, clamped between
    /// its neighbours less twice the grab distance; false for edge 0 or
    /// out of range.
    pub fn set_column_edge(&mut self, index: u32, x: f32) -> bool {
        let n = self.column_edges.len();
        let i = index as usize;
        if index == 0 || i >= n {
            return false;
        }
        let gap2 = f64::from(self.grab_distance) + f64::from(self.grab_distance);
        let lo = (gap2 + f64::from(self.column_edges[i - 1])) as f32;
        let next = if i + 1 == n {
            self.width
        } else {
            self.column_edges[i + 1]
        };
        let hi = (f64::from(next) - gap2) as f32;
        let x = if x < lo {
            lo
        } else if x > hi {
            hi
        } else {
            x
        };
        if x != self.column_edges[i] {
            self.column_edges[i] = x;
            if !self.row_edges.is_empty() {
                self.base.mark_dirty(true, true);
            }
        }
        true
    }

    /// `SetRowEdge`: the same for a row edge (the last edge only ever lands
    /// on its lower limit, as in the reference).
    pub fn set_row_edge(&mut self, index: u32, y: f32) -> bool {
        let n = self.row_edges.len();
        let i = index as usize;
        if index == 0 || i >= n {
            return false;
        }
        let gap2 = f64::from(self.grab_distance) + f64::from(self.grab_distance);
        let lo = (f64::from(self.row_edges[i - 1]) - gap2) as f32;
        let next = if i + 1 == n {
            self.height
        } else {
            self.row_edges[i + 1]
        };
        let hi = f64::from(next) + gap2;
        let y = if f64::from(y) < hi {
            hi as f32
        } else if y > lo {
            lo
        } else {
            y
        };
        if self.row_edges[i] != y {
            self.row_edges[i] = y;
            if !self.column_edges.is_empty() {
                self.base.mark_dirty(true, true);
            }
        }
        true
    }

    /// `GetRow`.
    pub fn row_mut(&mut self, row: u32) -> Option<&mut FastTableRow> {
        if row as usize >= self.row_edges.len() {
            return None;
        }
        self.rows.get_mut(row as usize)
    }

    /// `GetEdges`.
    #[must_use]
    pub fn edges(&self) -> TableEdges {
        TableEdges {
            rows: self.row_edges.clone(),
            cols: self.column_edges.clone(),
        }
    }

    /// `SetEdges`: nothing (and a notice) when the counts or the first
    /// column edge differ.
    pub fn set_edges(&mut self, edges: &TableEdges) -> bool {
        if edges.cols.len() != self.column_edges.len() || edges.rows.len() != self.row_edges.len() {
            return false;
        }
        if edges.cols.first() != self.column_edges.first() {
            return false;
        }
        self.column_edges.clone_from(&edges.cols);
        self.row_edges.clone_from(&edges.rows);
        if self.grid_nonempty() {
            self.base.mark_dirty(true, true);
        }
        true
    }

    /// `ClearSelection`.
    pub fn clear_selection(&mut self) {
        self.selected = false;
    }

    /// The selection of a selectable, hoverable row: whether it changed.
    fn select(&mut self, row: u32) -> bool {
        let changed = !(self.selected && self.selected_row == row);
        self.selected_row = row;
        self.selected = true;
        changed
    }

    fn selectable(&self, row: u32) -> bool {
        (row as usize) < self.row_edges.len()
            && self
                .rows
                .get(row as usize)
                .is_some_and(|r| r.selectable && r.hoverable)
    }

    /// `SelectRow`: a change tells the parent 0x300D.
    pub fn select_row(&mut self, ui: &mut Ui, id: NodeId, row: u32) -> bool {
        if !self.selectable(row) {
            return false;
        }
        if self.select(row)
            && let Some(parent) = ui.parent(id)
        {
            ui.send_child_message(parent, &ChildMessage::TableSelect { from: id, row });
        }
        true
    }

    /// `GetSelectedRow`.
    #[must_use]
    pub fn selected_row(&self) -> Option<u32> {
        self.selected.then_some(self.selected_row)
    }

    /// `SetFrameColor` (the bytes R, G, B, A).
    pub fn set_frame_color(&mut self, rgba: [u8; 4]) {
        self.frame_rgba = rgba;
    }

    /// `SetGrabDistance`: only while the current distance is not negative.
    pub fn set_grab_distance(&mut self, distance: f32) {
        if self.grab_distance >= 0.0 {
            self.grab_distance = distance;
        }
    }

    /// `SetResizable`.
    pub fn set_resizable(&mut self, rows: bool, columns: bool) {
        self.rows_resizable = rows;
        self.columns_resizable = columns;
    }

    /// `SetCellMargins`.
    pub fn set_cell_margins(&mut self, x: i32, y: i32) {
        self.cell_margin_x = x;
        self.cell_margin_y = y;
    }

    /// `SetLayoutFlag`.
    pub fn set_layout_flag(&mut self, single_line: bool) {
        self.layout_flag = single_line;
    }

    /// `SetTipMode`: a new mode makes one reset record per column, row or
    /// cell, keyed by the table and the record's index.
    pub fn set_tip_mode(&mut self, id: NodeId, mode: i32) {
        if mode == self.tip_mode {
            return;
        }
        self.tip_mode = mode;
        self.tips.clear();
        let count = match mode {
            1 => self.column_edges.len(),
            2 => self.row_edges.len(),
            3 => self.cells.len(),
            _ => 0,
        };
        for index in 0..count {
            let mut tip = ToolTipHover::for_ids(-1);
            tip.key = HoverKey::Part(id, index as u32);
            tip.ids_alt = 0;
            self.tips.push(tip);
        }
    }

    /// Slot 46: the window's record in tip mode 0.
    #[must_use]
    pub fn tooltip_hover_info(&self) -> Option<&ToolTipHover> {
        (self.tip_mode == 0).then_some(&self.base.hover)
    }

    /// `GetRowTip`.
    pub fn row_tip_mut(&mut self, row: u32) -> Option<&mut ToolTipHover> {
        if self.tip_mode != 2 {
            return None;
        }
        self.tips.get_mut(row as usize)
    }

    /// `GetCellTip`.
    pub fn cell_tip_mut(&mut self, row: u32, column: u32) -> Option<&mut ToolTipHover> {
        if self.tip_mode != 3 {
            return None;
        }
        let index = (self.column_edges.len() as u32)
            .wrapping_mul(row)
            .wrapping_add(column);
        self.tips.get_mut(index as usize)
    }

    /// Slot 45: the edges projected to pixels (the first at 0, the last
    /// closing at the pixel size), each cell's source laid out behind a
    /// clip node into its rectangle less the margins (to the last column
    /// edge when it spans), and a reference to every cell's list appended
    /// to the layout list, row by row.
    pub fn relayout(
        &mut self,
        fonts: &mut FontSystem,
        transform: &Transform,
        projection: &[f32; 16],
        viewport: [i32; 4],
    ) {
        relayout_cells(
            &mut self.base,
            fonts,
            Grid {
                cells: &mut self.cells,
                row_edges: &self.row_edges,
                column_edges: &self.column_edges,
                margins: (self.cell_margin_x, self.cell_margin_y),
                single_line: self.layout_flag,
            },
            transform,
            projection,
            viewport,
        );
    }

    /// `UIFastTable::Create`: the window's `Create` without a payload,
    /// then the payload's size and counts and evenly spaced edges.
    pub fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        payload: &Payload<'_>,
    ) -> bool {
        let result = ui.base_create(id, parent);
        self.base.create_reset();
        self.base.key_hover(id);
        if let Payload::Page(EntryPayload::Table {
            right,
            bottom,
            int5,
            int6,
        }) = payload
        {
            self.width = *right;
            self.height = *bottom;
            let rows = int5.cast_unsigned() as usize;
            let cols = int6.cast_unsigned() as usize;
            self.row_edges.resize(rows, 0.0);
            self.column_edges.resize(cols, 0.0);
            self.cells
                .resize(rows.wrapping_mul(cols), FastTableCell::default());
            self.rows.resize(rows, FastTableRow::new());
        }
        if self.grid_nonempty() {
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
        self.base.set_norm_size(self.width, self.height);
        result
    }

    /// The inverse of the transform and the layout dirtied.
    fn refresh(&mut self, ui: &Ui, id: NodeId) {
        self.inverse = general_inverse(&node_transform(ui, id)).transform;
        if self.grid_nonempty() {
            self.base.mark_dirty(true, true);
        }
    }

    /// The tooltip record of the column, row or cell under the mouse (the
    /// window's in mode 0).
    fn hovered_tip(&self) -> Option<&ToolTipHover> {
        let index = match self.tip_mode {
            1 => self.hover_column,
            2 => self.hover_row,
            3 => self
                .hover_row
                .wrapping_mul(self.column_edges.len() as u32)
                .wrapping_add(self.hover_column),
            _ => return self.tooltip_hover_info(),
        };
        self.tips.get(index as usize)
    }

    /// `UIFastTable::Update`.
    pub fn update(&mut self, ui: &mut Ui, id: NodeId) {
        let info = self.tooltip_hover_info().cloned();
        self.base
            .feed_tooltip_if_hovered(ui, info.as_ref(), |ui, ray| {
                UiFastTable::hit_test(self, ui, id, ray)
            });
        ui.base_update(id);

        if ui.capture() == Some(id) && self.dragging {
            let local = self.ray_to_local(ui, id, ui.mouse_ray());
            if self.drag_column {
                let x = (f64::from(local.x) + f64::from(self.drag_offset)) as f32;
                self.set_column_edge(self.drag_edge, x);
                ui.services.mouse_set_cursor(b"Stretch");
            } else {
                let y = (f64::from(local.y) + f64::from(self.drag_offset)) as f32;
                self.set_row_edge(self.drag_edge, y);
                ui.services.mouse_set_cursor(b"Arrow");
            }
            return;
        }
        if ui.capture().is_some() {
            return;
        }
        let local = self.ray_to_local(ui, id, ui.mouse_ray());
        self.hover_column = u32::MAX;
        self.hover_row = u32::MAX;
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

        // The column under x (the first edge is read without a test in the
        // reference; divergence D478).
        let col_count = self.column_edges.len();
        let mut col_prev = self.column_edges.first().copied().unwrap_or(0.0);
        let mut col_next = 0.0f32;
        for index in 0..col_count {
            col_next = if index + 1 < col_count {
                self.column_edges[index + 1]
            } else {
                self.width
            };
            if local.x >= col_prev && local.x < col_next {
                self.hover_column = index as u32;
                break;
            }
            col_prev = col_next;
        }
        // The row under y, the same way (edges go down).
        let row_count = self.row_edges.len();
        let mut row_prev = self.row_edges.first().copied().unwrap_or(0.0);
        let mut row_next = 0.0f32;
        for index in 0..row_count {
            row_next = if index + 1 < row_count {
                self.row_edges[index + 1]
            } else {
                bottom
            };
            if local.y <= row_prev && local.y > row_next {
                self.hover_row = index as u32;
                break;
            }
            row_prev = row_next;
        }

        // The record under the mouse (a missing one is read through a null
        // pointer in the reference; divergence D478).
        if self.base.tooltip_enabled
            && let Some(info) = self.hovered_tip()
            && info.ids_alt != 0
        {
            let info = info.clone();
            ui.feed_tooltip_hover(&info);
        }

        // Column edges, in rows at or below the header rows.
        if self.columns_resizable && self.hover_row >= self.header_rows {
            let column = self.hover_column;
            if column > 0
                && f64::from(local.x) - f64::from(col_prev) < f64::from(self.grab_distance)
            {
                ui.services.mouse_set_cursor(b"Stretch");
                self.drag_offset = (f64::from(col_prev) - f64::from(local.x)) as f32;
                self.drag_armed = true;
                self.drag_edge = column;
                self.drag_column = true;
                return;
            }
            if !self.drag_armed && column < (col_count as u32).wrapping_sub(2) {
                let distance = f64::from(col_next) - f64::from(local.x);
                if distance < f64::from(self.grab_distance) {
                    ui.services.mouse_set_cursor(b"Stretch");
                    self.drag_armed = true;
                    self.drag_edge = column.wrapping_add(1);
                    self.drag_offset = distance as f32;
                    self.drag_column = true;
                    return;
                }
            }
        }

        // Row edges.
        if self.rows_resizable {
            let row = self.hover_row;
            if row > 0 {
                let distance = f64::from(row_prev) - f64::from(local.y);
                if distance < f64::from(self.grab_distance) {
                    ui.services.mouse_set_cursor(b"Stretch");
                    self.drag_column = false;
                    self.drag_armed = true;
                    self.drag_edge = row;
                    self.drag_offset = distance as f32;
                    return;
                }
            }
            if !self.drag_armed && row < (row_count as u32).wrapping_sub(2) {
                // The bottom edge's offset has the opposite sign of the
                // other three, as in the reference.
                let distance = f64::from(local.y) - f64::from(row_next);
                if distance < f64::from(self.grab_distance) {
                    ui.services.mouse_set_cursor(b"Stretch");
                    self.drag_column = false;
                    self.drag_armed = true;
                    self.drag_edge = row.wrapping_add(1);
                    self.drag_offset = distance as f32;
                    return;
                }
            }
        }

        let row = self.hover_row;
        if (row as usize) < row_count && self.rows.get(row as usize).is_some_and(|r| r.hoverable) {
            self.hovered_row = row;
            self.hovered = true;
        }
        ui.services.mouse_set_cursor(b"Arrow");
    }

    fn draw_row_quad(&self, host: &mut dyn DrawHost, rgba: [u8; 4], row: u32) {
        let r = row as usize;
        let (Some(&c0), Some(&top)) = (self.column_edges.first(), self.row_edges.get(r)) else {
            return;
        };
        let Some(imm) = host.immediate() else { return };
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

    /// The window's draw with the table's slot 45
    /// ([`BaseRichTextWin::draw_window`]), then the children.
    fn draw_window(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        let Some(node) = ui.node(id) else { return };
        if node.flags & VISIBLE == 0 {
            return;
        }
        let transform = Transform::new(node.orientation, node.position);
        let mut grid = Grid {
            cells: &mut self.cells,
            row_edges: &self.row_edges,
            column_edges: &self.column_edges,
            margins: (self.cell_margin_x, self.cell_margin_y),
            single_line: self.layout_flag,
        };
        self.base
            .draw_window(ui, id, host, &transform, &mut |base, fonts, toolkit, cx| {
                let projection = cx.projection;
                let viewport = cx.viewport;
                base.render_in(
                    fonts,
                    toolkit,
                    &mut |base, fonts| {
                        relayout_cells(
                            base,
                            fonts,
                            grid.reborrow(),
                            &transform,
                            &projection,
                            viewport,
                        );
                    },
                    cx.pipeline,
                );
            });
        ui.draw_children(id, host);
    }

    /// `BaseRichTextWin::Draw` without the toolkit: the projection, the
    /// surface laid out by slot 45, the image; no bar, mesh or frame.
    pub fn draw_surface(
        &mut self,
        transform: &Transform,
        fonts: &mut FontSystem,
        cx: &mut RichTextCx<'_>,
    ) {
        self.base.ensure_surface(transform, cx);
        let projection = cx.projection;
        let viewport = cx.viewport;
        let mut grid = Grid {
            cells: &mut self.cells,
            row_edges: &self.row_edges,
            column_edges: &self.column_edges,
            margins: (self.cell_margin_x, self.cell_margin_y),
            single_line: self.layout_flag,
        };
        self.base.render_with(
            fonts,
            &mut |base, fonts| {
                relayout_cells(
                    base,
                    fonts,
                    grid.reborrow(),
                    transform,
                    &projection,
                    viewport,
                );
            },
            cx.pipeline,
        );
        cx.tiles.extend(self.base.draw_image());
    }

    /// `UIFastTable::Draw`: the highlight quads, the window, then the
    /// border, the row and column lines and, without quads, the hovered
    /// row's outline in the pulsing amber.
    pub fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        let t = node_transform(ui, id);
        if let Some(imm) = host.immediate() {
            imm.bind_modelview(&t);
        }
        set_draw_states(host);
        if self.row_highlights {
            if self.selected {
                self.draw_row_quad(host, self.selected_rgba, self.selected_row);
            }
            if self.hovered {
                self.draw_row_quad(host, self.hovered_rgba, self.hovered_row);
            }
        }
        self.draw_window(ui, id, host);
        if let Some(imm) = host.immediate() {
            imm.bind_modelview(&t);
        }
        set_draw_states(host);
        let pulse = ui.pulse.blend_from(palette::AMBER);
        let Some(imm) = host.immediate() else { return };
        imm.color4ubv(self.frame_rgba);
        let (w, h) = (self.width, self.height);
        if self.draw_border {
            imm.begin(primitive::LINES);
            if self.border_top {
                imm.vertex(0.0, 0.0, 0.0);
                imm.vertex(w, 0.0, 0.0);
            }
            if self.border_bottom {
                imm.vertex(0.0, -h, 0.0);
                imm.vertex(w, -h, 0.0);
            }
            if self.border_left {
                imm.vertex(0.0, 0.0, 0.0);
                imm.vertex(0.0, -h, 0.0);
            }
            if self.border_right {
                imm.vertex(w, 0.0, 0.0);
                imm.vertex(w, -h, 0.0);
            }
            imm.end();
        }
        imm.begin(primitive::LINES);
        if self.draw_row_lines {
            for index in 1..self.row_edges.len() {
                if !self.rows.get(index).is_some_and(|r| r.row_line) {
                    continue;
                }
                imm.vertex(0.0, self.row_edges[index], 0.0);
                imm.vertex(w, self.row_edges[index], 0.0);
            }
        }
        if self.draw_column_lines {
            let top = if self.header_rows > 0 {
                self.row_edges
                    .get(self.header_rows as usize)
                    .copied()
                    .unwrap_or(0.0)
            } else {
                0.0
            };
            for &edge in self.column_edges.iter().skip(1) {
                imm.vertex(edge, top, 0.0);
                imm.vertex(edge, -h, 0.0);
            }
        }
        imm.end();
        if self.row_highlights || !self.hovered {
            return;
        }
        let (mut first, mut last) = (self.hovered_row, self.hovered_row);
        let group = self
            .rows
            .get(self.hovered_row as usize)
            .map_or(0, |r| r.group_id);
        if group != 0 {
            while first > 0
                && self
                    .rows
                    .get(first as usize - 1)
                    .is_some_and(|r| r.group_id == group)
            {
                first -= 1;
            }
            while (last as usize + 1) < self.row_edges.len()
                && self
                    .rows
                    .get(last as usize + 1)
                    .is_some_and(|r| r.group_id == group)
            {
                last += 1;
            }
        }
        let c0 = self.column_edges.first().copied().unwrap_or(0.0);
        let top = self.row_edges.get(first as usize).copied().unwrap_or(0.0);
        imm.color4ubv(pulse.to_le_bytes());
        imm.begin(primitive::LINE_LOOP);
        imm.vertex(c0, top, 0.0);
        imm.vertex(w, top, 0.0);
        if let Some(&next) = self.row_edges.get(last as usize + 1) {
            imm.vertex(w, next, 0.0);
            imm.vertex(c0, next, 0.0);
        } else {
            imm.vertex(w, -h, 0.0);
            imm.vertex(c0, -h, 0.0);
        }
        imm.end();
    }

    /// `UIFastTable::MouseEvent`.
    pub fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        if event.field0() != -1 {
            return base::mouse_event(ui, id, self, event);
        }
        match event.field1() {
            3 => {
                if self.hovered {
                    let row = self.hovered_row;
                    if self.selectable(row) {
                        let changed = self.select(row);
                        if let Some(parent) = ui.parent(id) {
                            if changed {
                                ui.send_child_message(
                                    parent,
                                    &ChildMessage::TableSelect { from: id, row },
                                );
                            }
                            ui.send_child_message(
                                parent,
                                &ChildMessage::TableDoubleClick { from: id, row },
                            );
                        }
                    }
                }
                true
            }
            2 => {
                if self.hovered {
                    let row = self.hovered_row;
                    if self.selectable(row) {
                        if self.select(row)
                            && let Some(parent) = ui.parent(id)
                        {
                            ui.send_child_message(
                                parent,
                                &ChildMessage::TableSelect { from: id, row },
                            );
                        }
                        return true;
                    }
                } else if self.drag_armed {
                    ui.push_capture(id);
                    self.dragging = true;
                    return true;
                }
                // Without a parent the reference calls through a null
                // pointer (divergence D478).
                if let Some(parent) = ui.parent(id) {
                    let cell = (self.hover_row << 16) | self.hover_column;
                    ui.send_child_message(parent, &ChildMessage::TableCellPress { from: id, cell });
                }
                true
            }
            _ => {
                if ui.capture() == Some(id) && self.dragging {
                    self.dragging = false;
                    ui.remove_capture(id);
                    return true;
                }
                base::mouse_event(ui, id, self, event)
            }
        }
    }

    /// `UIFastTable::HitTest`: the ray inside `[0, width)` by `(-height, 0]`.
    #[must_use]
    pub fn hit_test(&self, ui: &Ui, id: NodeId, ray: Vector) -> bool {
        let local = self.ray_to_local(ui, id, ray);
        local.x >= 0.0 && local.x < self.width && local.y <= 0.0 && -self.height < local.y
    }

    /// `SetPosition`: the window's (the attach offset added), then the
    /// inverse and the layout dirtied.
    fn place(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
        self.base.set_position(ui, id, position);
        self.refresh(ui, id);
    }
}

/// What slot 45 lays out.
struct Grid<'a> {
    cells: &'a mut [FastTableCell],
    row_edges: &'a [f32],
    column_edges: &'a [f32],
    margins: (i32, i32),
    single_line: bool,
}

impl Grid<'_> {
    fn reborrow(&mut self) -> Grid<'_> {
        Grid {
            cells: &mut *self.cells,
            row_edges: self.row_edges,
            column_edges: self.column_edges,
            margins: self.margins,
            single_line: self.single_line,
        }
    }
}

/// The pixel of each edge: through the transform and the projection's
/// rows (`a` for x or y, the w row), minus the first edge's pixel; an edge
/// at w = 0 takes the edge before it.
fn edge_pixels(
    edges: &[f32],
    transform: &Transform,
    m: &[f32; 16],
    vertical: bool,
    view: (i32, i32),
    close: i32,
) -> Vec<i32> {
    let mut pixels = vec![0i32; edges.len() + 1];
    for (index, &edge) in edges.iter().enumerate() {
        let local = if vertical {
            Vector::new(0.0, edge, 0.0)
        } else {
            Vector::new(edge, 0.0, 0.0)
        };
        let world = transform_xform(transform, local);
        let (x, y, z) = (f64::from(world.x), f64::from(world.y), f64::from(world.z));
        let (w, clip) = if vertical {
            let w = ((f64::from(m[12]) * x + f64::from(m[13]) * y)
                + f64::from(m[14]) * z
                + f64::from(m[15])) as f32;
            let clip =
                (f64::from(m[4]) * x + f64::from(m[5]) * y) + f64::from(m[6]) * z + f64::from(m[7]);
            (w, clip)
        } else {
            let w = ((f64::from(m[12]) * x + f64::from(m[14]) * z)
                + f64::from(m[13]) * y
                + f64::from(m[15])) as f32;
            let clip =
                (f64::from(m[2]) * z + f64::from(m[1]) * y) + f64::from(m[0]) * x + f64::from(m[3]);
            (w, clip)
        };
        if w == 0.0 {
            pixels[index] = if index > 0 { pixels[index - 1] } else { 0 };
            continue;
        }
        let ndc = clip * (1.0 / f64::from(w));
        let (origin, size) = view;
        let mut pixel = if vertical {
            (ndc + 1.0) * f64::from(size) * 0.5 + f64::from(origin)
        } else {
            (1.0 - ndc) * f64::from(size) * 0.5 + f64::from(origin)
        };
        if index > 0 {
            pixel -= f64::from(pixels[0]);
        }
        pixels[index] = trunc_to_i32(pixel);
    }
    pixels[edges.len()] = close;
    pixels[0] = 0;
    pixels
}

/// `UIFastTable::Relayout` over the window's context.
fn relayout_cells(
    base: &mut BaseRichTextWin,
    fonts: &mut FontSystem,
    grid: Grid<'_>,
    transform: &Transform,
    projection: &[f32; 16],
    viewport: [i32; 4],
) {
    let [view_x, view_y, view_w, view_h] = viewport;
    let cols = edge_pixels(
        grid.column_edges,
        transform,
        projection,
        false,
        (view_x, view_w),
        base.pixel_width,
    );
    let rows = edge_pixels(
        grid.row_edges,
        transform,
        projection,
        true,
        (view_y, view_h),
        base.pixel_height,
    );
    let (col_count, row_count) = (grid.column_edges.len(), grid.row_edges.len());
    let (margin_x, margin_y) = grid.margins;
    for row in 0..row_count {
        for column in 0..col_count {
            let Some(cell) = grid.cells.get_mut(row * col_count + column) else {
                continue;
            };
            let right_edge = if cell.span_to_end {
                cols[col_count]
            } else {
                cols[column + 1]
            };
            let rect = ClipRect {
                left: cols[column].wrapping_add(margin_x),
                top: rows[row].wrapping_add(margin_y),
                right: right_edge.wrapping_sub(margin_x),
                bottom: rows[row + 1].wrapping_sub(margin_y),
            };
            let mut laid = RenderDisplayList::new();
            laid.push(RdlNode::Clip(rect));
            let mut ctx = DeviceTrcCx {
                trc: &mut base.trc,
                fonts: &fonts.rich,
                backend: fonts.backend.as_mut(),
                surface: base.surface.as_mut(),
            };
            layout_rich_text(
                &mut ctx,
                &fonts.styles,
                &cell.source,
                &mut laid,
                LayoutParams {
                    width: rect.right.wrapping_sub(rect.left),
                    origin_h: rect.left,
                    origin_v: rect.top,
                    single_line: grid.single_line,
                    wrap: true,
                },
            );
            cell.laid = Rc::new(laid);
            base.layout_list.push(RdlNode::Ref(Rc::clone(&cell.laid)));
        }
    }
}

impl Widget for UiFastTable {
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
    fn win_control(&self) -> Option<&WinControl> {
        Some(&self.base.control)
    }
    fn win_control_mut(&mut self) -> Option<&mut WinControl> {
        Some(&mut self.base.control)
    }
    fn hit_test(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) -> bool {
        UiFastTable::hit_test(self, ui, id, ray)
    }
    fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
        self.place(ui, id, position);
    }
    fn set_orientation(&mut self, ui: &mut Ui, id: NodeId, orientation: Matrix) {
        base::set_orientation(ui, id, &mut self.base.control, orientation);
    }
    /// Slots 7 and 5, then the inverse and the layout dirtied.
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
        UiFastTable::create(self, ui, id, parent, payload)
    }
    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        base::destroy(ui, id, &mut self.base.control);
    }
    fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        base::on_delete(ui, id, &mut self.base.control);
    }
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        UiFastTable::update(self, ui, id);
    }
    fn update_inactive(&mut self, ui: &mut Ui, id: NodeId) {
        let info = self.tooltip_hover_info().cloned();
        self.base
            .feed_tooltip_if_hovered(ui, info.as_ref(), |ui, ray| {
                UiFastTable::hit_test(self, ui, id, ray)
            });
        ui.base_update_inactive(id);
    }
    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        UiFastTable::draw(self, ui, id, host);
    }
    /// Slot 16: the arrow cursor.
    fn mouse_leave(&mut self, ui: &mut Ui, _id: NodeId, _ray: Vector) {
        ui.services.mouse_set_cursor(b"Arrow");
    }
    fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        UiFastTable::mouse_event(self, ui, id, event)
    }
    fn property(&mut self, ui: &mut Ui, id: NodeId, property: Property<'_>) -> isize {
        if let Property::Offset(offset) = property {
            self.base.set_attach_offset(ui, id, offset);
            return 1;
        }
        base::property(ui, id, self, property)
    }
}

/// The scroll table's callback (`FUiScrollTableCallback` with its context):
/// the first item and the visible row count.
pub type ScrollTableCallback = Box<dyn FnMut(i32, u32)>;

/// `UIScrollTable`.
#[derive(Default)]
pub struct UiScrollTable {
    pub table: UiFastTable,
    /// `+0x578`, `+0x584`.
    pub callback: Option<ScrollTableCallback>,
    /// `+0x57c`.
    pub item_count: u32,
    /// `+0x580`: rows that do not scroll.
    pub fixed_rows: u32,
    /// `+0x588`: the vertical `UIScroll`.
    pub scroll_bar: Option<NodeId>,
}

impl std::fmt::Debug for UiScrollTable {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("UiScrollTable")
            .field("table", &self.table)
            .field("callback", &self.callback.is_some())
            .field("item_count", &self.item_count)
            .field("fixed_rows", &self.fixed_rows)
            .field("scroll_bar", &self.scroll_bar)
            .finish()
    }
}

impl UiScrollTable {
    fn visible_rows(&self) -> u32 {
        (self.table.row_edges.len() as u32).wrapping_sub(self.fixed_rows)
    }

    fn bar<'u>(&self, ui: &'u mut Ui) -> Option<&'u mut UiScroll> {
        ui.widget_mut::<UiScroll>(self.scroll_bar?)
    }

    /// `SetItemCount`: everything fitting hides the bar at 0; otherwise the
    /// bar is shown with the items as its range, the scrolled rows as its
    /// page and a step of 1. The layout is dirtied either way.
    pub fn set_item_count(&mut self, ui: &mut Ui, item_count: u32, fixed_rows: u32) {
        self.item_count = item_count;
        self.fixed_rows = fixed_rows;
        let visible = self.visible_rows();
        let Some(bar_id) = self.scroll_bar else {
            return;
        };
        if item_count <= visible {
            if let Some(bar) = self.bar(ui) {
                bar.set_value(0.0);
            }
            if let Some(n) = ui.node_mut(bar_id) {
                n.flags &= 0xFC;
            }
            self.table.base.mark_dirty(true, true);
            return;
        }
        if let Some(n) = ui.node_mut(bar_id) {
            n.flags |= 3;
        }
        if let Some(bar) = self.bar(ui) {
            bar.set_range(0.0, item_count.cast_signed() as f32);
            bar.set_page(visible as f32);
            bar.set_step(1.0);
        }
        self.table.base.mark_dirty(true, true);
    }

    /// `GetFirstItem`: the bar's value truncated.
    #[must_use]
    pub fn first_item(&self, ui: &Ui) -> i32 {
        self.scroll_bar
            .and_then(|b| ui.widget::<UiScroll>(b))
            .map_or(0, |b| trunc_to_i32(f64::from(b.value)))
    }

    /// `SetCallback`: installed and called once.
    pub fn set_callback(&mut self, ui: &Ui, callback: Option<ScrollTableCallback>) {
        self.callback = callback;
        self.scrolled(ui);
    }

    /// `SetScrollBarGap`: the gap, then the bar placed beside the table
    /// again.
    pub fn set_scroll_bar_gap(&mut self, ui: &mut Ui, id: NodeId, gap: f32) {
        self.table.base.scroll_bar_gap = gap;
        if let Some(bar) = self.scroll_bar {
            place_scroll_bar(ui, id, bar, gap, self.table.base.norm_width);
        }
    }

    /// The callback hears the first item and the visible rows.
    fn scrolled(&mut self, ui: &Ui) {
        let first = self.first_item(ui);
        self.scrolled_to(first);
    }

    /// The callback hears `first` and the visible rows.
    fn scrolled_to(&mut self, first: i32) {
        let visible = self.visible_rows();
        if let Some(cb) = self.callback.as_mut() {
            cb(first, visible);
        }
    }
}

impl Widget for UiScrollTable {
    fn as_any(&self) -> &dyn Any {
        self
    }
    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
    fn win_control(&self) -> Option<&WinControl> {
        Some(&self.table.base.control)
    }
    fn win_control_mut(&mut self) -> Option<&mut WinControl> {
        Some(&mut self.table.base.control)
    }
    fn hit_test(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) -> bool {
        self.table.hit_test(ui, id, ray)
    }
    fn set_position(&mut self, ui: &mut Ui, id: NodeId, position: Vector) {
        self.table.place(ui, id, position);
    }
    fn set_orientation(&mut self, ui: &mut Ui, id: NodeId, orientation: Matrix) {
        Widget::set_orientation(&mut self.table, ui, id, orientation);
    }
    fn set_transform(&mut self, ui: &mut Ui, id: NodeId, transform: &Transform) {
        self.set_orientation(ui, id, transform.rotation);
        self.set_position(ui, id, transform.translation);
        self.table.refresh(ui, id);
    }
    /// `UIScrollTable::Create`: the table's, then the bar with the rows as
    /// items, at 0.
    fn create(
        &mut self,
        ui: &mut Ui,
        id: NodeId,
        parent: Option<NodeId>,
        payload: &mut Payload<'_>,
    ) -> bool {
        if !self.table.create(ui, id, parent, payload) {
            return false;
        }
        self.scroll_bar = self.table.base.create_scroll_bar_v(ui, id);
        let rows = self.table.row_edges.len() as u32;
        self.set_item_count(ui, rows, 0);
        if let Some(bar) = self.bar(ui) {
            bar.set_value(0.0);
        }
        true
    }
    fn destroy(&mut self, ui: &mut Ui, id: NodeId) {
        Widget::destroy(&mut self.table, ui, id);
    }
    fn on_delete(&mut self, ui: &mut Ui, id: NodeId) {
        Widget::on_delete(&mut self.table, ui, id);
    }
    fn update(&mut self, ui: &mut Ui, id: NodeId) {
        self.table.update(ui, id);
    }
    fn update_inactive(&mut self, ui: &mut Ui, id: NodeId) {
        Widget::update_inactive(&mut self.table, ui, id);
    }
    fn draw(&mut self, ui: &mut Ui, id: NodeId, host: &mut dyn DrawHost) {
        self.table.draw(ui, id, host);
    }
    fn mouse_leave(&mut self, ui: &mut Ui, id: NodeId, ray: Vector) {
        Widget::mouse_leave(&mut self.table, ui, id, ray);
    }
    fn mouse_event(&mut self, ui: &mut Ui, id: NodeId, event: MouseButton) -> bool {
        self.table.mouse_event(ui, id, event)
    }
    /// Slot 23: the bar's value change goes to the callback.
    fn child_message(&mut self, _ui: &mut Ui, _id: NodeId, message: &ChildMessage) -> bool {
        match message {
            // The bar's value comes with the message: the bar is busy while
            // it tells the table (D347).
            ChildMessage::ScrollChanged { from, value } if Some(*from) == self.scroll_bar => {
                self.scrolled_to(trunc_to_i32(f64::from(*value)));
                true
            }
            _ => false,
        }
    }
    fn property(&mut self, ui: &mut Ui, id: NodeId, property: Property<'_>) -> isize {
        Widget::property(&mut self.table, ui, id, property)
    }
}

#[cfg(test)]
mod tests;
