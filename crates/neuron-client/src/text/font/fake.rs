//! A deterministic [`FontBackend`] for the platform-free tests.
//!
//! A face at `lfHeight` h has a cell of `|h|` pixels (a negative height
//! adds a fifth for the leading), ascent four fifths of it. Advances: a
//! quarter of the cell for `i l . , ' |` and space (at least 1), the whole
//! cell for `M W`, half of it for anything else, 0 for NUL. A glyph draws
//! as a box one pixel narrower than its advance from the cell top to the
//! baseline: coverage 255, and 128 in its first column.

use std::cell::RefCell;
use std::collections::BTreeMap;
use std::rc::Rc;

use super::{FaceHandle, FontBackend, LogFont, TextMetrics};

/// What the fake was asked.
#[derive(Default)]
pub struct FakeLog {
    /// Every `create_font` height, in order.
    pub created: Vec<i32>,
    pub deleted: usize,
    pub extents: usize,
    pub draws: Vec<(i32, i32, u32, u32, Vec<u16>)>,
}

#[derive(Default)]
pub struct FakeBackend {
    faces: BTreeMap<u32, LogFont>,
    next: u32,
    pub log: Rc<RefCell<FakeLog>>,
}

impl FakeBackend {
    /// A fake and a handle on its log.
    pub fn with_log() -> (FakeBackend, Rc<RefCell<FakeLog>>) {
        let fake = FakeBackend::default();
        let log = Rc::clone(&fake.log);
        (fake, log)
    }

    fn cell(log_font: &LogFont) -> i32 {
        let h = log_font.height;
        if h < 0 { -h + (-h) / 5 } else { h }
    }

    /// The fake's advance of `ch` in a cell of `cell` pixels.
    pub fn advance(cell: i32, ch: u16) -> i32 {
        match ch {
            0 => 0,
            0x20 | 0x69 | 0x6C | 0x2E | 0x2C | 0x27 | 0x7C => (cell / 4).max(1),
            0x4D | 0x57 => cell,
            _ => cell / 2,
        }
    }

    fn face(&self, face: FaceHandle) -> &LogFont {
        &self.faces[&face.0]
    }
}

impl FontBackend for FakeBackend {
    fn create_font(&mut self, log_font: &LogFont) -> Option<FaceHandle> {
        self.log.borrow_mut().created.push(log_font.height);
        if log_font.height == 0 || log_font.face().is_empty() {
            return None;
        }
        self.next += 1;
        self.faces.insert(self.next, *log_font);
        Some(FaceHandle(self.next))
    }

    fn delete_font(&mut self, face: FaceHandle) {
        self.log.borrow_mut().deleted += 1;
        self.faces.remove(&face.0);
    }

    fn text_metrics(&mut self, face: FaceHandle) -> TextMetrics {
        let cell = Self::cell(self.face(face));
        let ascent = cell * 4 / 5;
        TextMetrics {
            height: cell,
            ascent,
            descent: cell - ascent,
            internal_leading: 0,
            external_leading: 0,
            ave_char_width: cell / 2,
            max_char_width: cell,
        }
    }

    fn char_width(&mut self, face: FaceHandle, ch: u16) -> Option<i32> {
        Some(Self::advance(Self::cell(self.face(face)), ch))
    }

    fn text_extent(&mut self, face: FaceHandle, text: &[u16]) -> Option<(i32, i32)> {
        self.log.borrow_mut().extents += 1;
        let cell = Self::cell(self.face(face));
        Some((text.iter().map(|&c| Self::advance(cell, c)).sum(), cell))
    }

    fn draw_text(
        &mut self,
        face: FaceHandle,
        x: i32,
        y: i32,
        width: u32,
        height: u32,
        text: &[u16],
    ) -> Option<Vec<u8>> {
        self.log
            .borrow_mut()
            .draws
            .push((x, y, width, height, text.to_vec()));
        let cell = Self::cell(self.face(face));
        let ascent = cell * 4 / 5;
        let mut out = vec![0u8; (width * height) as usize];
        let mut pen = x;
        for &ch in text {
            let adv = Self::advance(cell, ch);
            if ch != 0x20 {
                for gy in y..y + ascent {
                    for gx in pen..pen + adv - 1 {
                        if gx >= 0 && gy >= 0 && (gx as u32) < width && (gy as u32) < height {
                            out[gy as usize * width as usize + gx as usize] =
                                if gx == pen { 128 } else { 255 };
                        }
                    }
                }
            }
            pen += adv;
        }
        Some(out)
    }
}
