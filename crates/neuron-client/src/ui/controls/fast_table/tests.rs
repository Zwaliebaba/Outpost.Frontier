use std::cell::RefCell;

use super::*;
use crate::text::font::FontEnv;
use crate::text::font::fake::FakeBackend;
use crate::text::layout::device::RichSurface;
use crate::ui::widescreen::Surface;

fn units(text: &str) -> Vec<u16> {
    text.encode_utf16().collect()
}

fn fonts() -> FontSystem {
    let mut sys = FontSystem::new(Box::new(FakeBackend::default()), FontEnv::new(800, 600));
    sys.rich.initialize(sys.backend.as_mut(), 600);
    sys.rich.reinitialize_fonts(sys.backend.as_mut());
    sys
}

fn table_payload(rows: i32, cols: i32) -> EntryPayload {
    EntryPayload::Table {
        right: 0.6,
        bottom: 0.4,
        int5: rows,
        int6: cols,
    }
}

/// A projection that mirrors x and y, so the reference's `1 - ndc` and
/// `ndc + 1` put the table's top left above and left of its bottom right.
const MIRROR: [f32; 16] = [
    -1.0, 0.0, 0.0, 0.0, 0.0, -1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0,
];

#[test]
fn the_cells_lay_out_behind_their_clip_rectangles() {
    let mut ui = Ui::new(Surface::new(640, 480));
    let payload = table_payload(2, 3);
    let id = ui
        .create_by_class(
            Some(b"T"),
            b"UIFastTable",
            None,
            &mut Payload::Page(&payload),
        )
        .unwrap();
    let mut table = std::mem::take(ui.widget_mut::<UiFastTable>(id).unwrap());
    assert_eq!(table.cells.len(), 6);
    assert_eq!(table.column_edges, vec![0.0, 0.2, 0.4]);
    table
        .cell_mut(0, 0)
        .unwrap()
        .source
        .push(RdlNode::Text(units("Name")));
    table
        .cell_mut(1, 2)
        .unwrap()
        .source
        .push(RdlNode::Text(units("42")));
    table.set_cell_span_to_end(1, 0, true);
    table.base.pixel_width = 300;
    table.base.pixel_height = 200;
    table.base.surface = Some(RichSurface::new(512, 256));
    table.base.layout_dirty = true;
    let mut fonts = fonts();
    let transform = Transform::new(Matrix::IDENTITY, Vector::new(-0.3, 0.2, 0.0));
    // A viewport of 1000 x 1000 at the origin: x pixels are (1 + x) * 500,
    // y pixels (1 - y) * 500, each minus the first edge's.
    let viewport = [0, 0, 1000, 1000];
    let mut grid_table = table;
    let projection = MIRROR;
    {
        let UiFastTable {
            base,
            cells,
            row_edges,
            column_edges,
            cell_margin_x,
            cell_margin_y,
            layout_flag,
            ..
        } = &mut grid_table;
        base.rebuild_layout_with(&mut fonts, &mut |base, fonts| {
            relayout_cells(
                base,
                fonts,
                Grid {
                    cells,
                    row_edges,
                    column_edges,
                    margins: (*cell_margin_x, *cell_margin_y),
                    single_line: *layout_flag,
                },
                &transform,
                &projection,
                viewport,
            );
        });
    }
    let t = &grid_table;
    // One reference per cell, row by row.
    assert_eq!(t.base.layout_list.nodes.len(), 6);
    // Columns at 0, 100 and 200 pixels, closing at the pixel width; rows at
    // 0 and 101 (the first row's 399.99... truncates to 399), closing at the
    // pixel height; 2-pixel margins.
    assert_eq!(
        t.cells[0].laid.nodes[0],
        RdlNode::Clip(ClipRect {
            left: 2,
            top: 2,
            right: 98,
            bottom: 99
        })
    );
    assert_eq!(
        t.cells[3].laid.nodes[0],
        RdlNode::Clip(ClipRect {
            left: 2,
            top: 103,
            right: 298,
            bottom: 198
        })
    );
    assert_eq!(
        t.cells[5].laid.nodes[0],
        RdlNode::Clip(ClipRect {
            left: 202,
            top: 103,
            right: 298,
            bottom: 198
        })
    );
    // The cell's text laid out after its clip.
    assert!(t.cells[0].laid.nodes.len() > 1);
    assert_eq!(t.cells[1].laid.nodes.len(), 1);
    assert!(
        matches!(&t.base.layout_list.nodes[5], RdlNode::Ref(l) if Rc::ptr_eq(l, &t.cells[5].laid))
    );
    assert!(!t.base.layout_dirty);
}

#[test]
fn the_edges_round_trip_through_their_registry_bytes() {
    let edges = [0.0f32, -0.1, -0.25];
    let bytes = TableEdges::edge_bytes(&edges).unwrap();
    assert_eq!(bytes.len(), 12);
    let mut back = vec![9.0];
    TableEdges::load_edges(&mut back, Some(&bytes));
    assert_eq!(back, edges);
    // A missing value leaves the vector alone.
    TableEdges::load_edges(&mut back, None);
    assert_eq!(back, edges);
    assert!(TableEdges::edge_bytes(&[]).is_none());
}

#[test]
fn a_scroll_table_tells_its_owner_the_first_item() {
    let mut ui = Ui::new(Surface::new(640, 480));
    let payload = table_payload(4, 2);
    let id = ui
        .create_by_class(
            Some(b"T"),
            b"UIScrollTable",
            None,
            &mut Payload::Page(&payload),
        )
        .unwrap();
    let heard = Rc::new(RefCell::new(Vec::new()));
    let h = heard.clone();
    let mut t = std::mem::take(ui.widget_mut::<UiScrollTable>(id).unwrap());
    let bar = t.scroll_bar.unwrap();
    assert_eq!(ui.node(bar).unwrap().name(), b"ScrollV_1");
    // Four rows show four items: the bar is hidden.
    assert_eq!(ui.node(bar).unwrap().flags & 3, 0);
    t.set_item_count(&mut ui, 20, 1);
    assert_eq!(ui.node(bar).unwrap().flags & 3, 3);
    t.set_callback(
        &ui,
        Some(Box::new(move |first, rows| {
            h.borrow_mut().push((first, rows))
        })),
    );
    ui.widget_mut::<UiScroll>(bar).unwrap().set_value(7.5);
    *ui.widget_mut::<UiScrollTable>(id).unwrap() = t;
    assert!(ui.send_child_message(id, &ChildMessage::ScrollChanged { from: bar }));
    assert_eq!(*heard.borrow(), vec![(0, 3), (7, 3)]);
}
