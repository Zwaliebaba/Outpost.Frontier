use super::*;
use crate::text::font::fake::FakeBackend;
use crate::text::layout::StyleCollection;
use crate::text::rdl::{RdlNode, RenderDisplayList};

fn setup() -> (RichFonts, FakeBackend) {
    let mut backend = FakeBackend::default();
    let mut fonts = RichFonts::new();
    fonts.initialize(&mut backend, 768);
    fonts.reinitialize_fonts(&mut backend);
    (fonts, backend)
}

fn units(text: &str) -> Vec<u16> {
    text.encode_utf16().collect()
}

#[test]
fn measures_only_with_a_surface() {
    let (fonts, mut backend) = setup();
    let mut trc = DeviceTrc::new();
    let text = units("aM i");
    let mut size = VisualSize::default();
    {
        let mut cx = DeviceTrcCx {
            trc: &mut trc,
            fonts: &fonts,
            backend: &mut backend,
            surface: None,
        };
        cx.text_visual_size(Some(&text), &mut size);
        // Font byte 0 is Arial Unicode MS 22: the fake's 22-pixel cell.
        assert_eq!(
            size,
            VisualSize {
                width: 0,
                height: 0,
                ascent: 17,
                descent: 5
            }
        );
        assert_eq!(cx.text_fit(Some(&text), 100), 0);
        cx.execute_text(Some(&text), false);
        assert_eq!(cx.trc().pos_h, 0);
    }
    let mut surface = RichSurface::new(64, 32);
    let mut cx = DeviceTrcCx {
        trc: &mut trc,
        fonts: &fonts,
        backend: &mut backend,
        surface: Some(&mut surface),
    };
    cx.text_visual_size(Some(&text), &mut size);
    assert_eq!((size.width, size.height), (11 + 22 + 5 + 5, 22));
    assert_eq!(cx.text_fit(Some(&text), 33), 2);
    assert_eq!(cx.text_fit(Some(&text), 32), 1);
    assert_eq!(cx.text_fit(Some(&[]), 32), 0);
    assert_eq!(cx.cell_height(), 22);
    let mut null = VisualSize {
        width: 9,
        height: 9,
        ascent: 9,
        descent: 9,
    };
    cx.text_visual_size(None, &mut null);
    assert_eq!(
        null,
        VisualSize {
            width: 9,
            height: 9,
            ascent: 0,
            descent: 0
        }
    );
}

#[test]
fn highlighted_text_draws_shadow_then_colour() {
    let (mut fonts, mut backend) = setup();
    fonts.set_drop_shadow_offset(3, 1);
    let mut trc = DeviceTrc::new();
    // Red (COLORREF 0x0000FF) in font byte 0.
    trc.state.current = 0x0000_FF00;
    trc.state.origin_h = 2;
    let mut surface = RichSurface::new(64, 32);
    {
        let mut cx = DeviceTrcCx {
            trc: &mut trc,
            fonts: &fonts,
            backend: &mut backend,
            surface: Some(&mut surface),
        };
        cx.execute_text(Some(&units("ab")), true);
        assert_eq!(cx.trc().pos_h, 22);
    }
    // The fake's glyph covers its advance less one column from the cell
    // top to the baseline (17 rows); the text lies over its shadow.
    assert_eq!(surface.pixels[2 * 64 + 4], 0x00FF_0000);
    assert_eq!(surface.pixels[17 * 64 + 7], 0x000A_0A0A);
    assert_eq!(surface.pixels[25 * 64 + 4], 0);
    let a1 = surface.to_a1r5g5b5(128);
    assert_eq!(
        u16::from_le_bytes([a1[2 * 128 + 8], a1[2 * 128 + 9]]),
        0xFC00
    );
    assert_eq!(u16::from_le_bytes([a1[0], a1[1]]), 0);
}

#[test]
fn clip_rectangles_follow_the_origin() {
    let (fonts, mut backend) = setup();
    let mut trc = DeviceTrc::new();
    trc.state.origin_h = 10;
    trc.state.origin_v = -4;
    let mut surface = RichSurface::new(64, 32);
    let rect = ClipRect {
        left: 30,
        top: 20,
        right: 5,
        bottom: 8,
    };
    {
        let mut cx = DeviceTrcCx {
            trc: &mut trc,
            fonts: &fonts,
            backend: &mut backend,
            surface: Some(&mut surface),
        };
        cx.set_clip_rect(rect);
        assert_eq!(cx.trc.clip, rect);
        cx.trc.state.origin_h = 0;
        assert_eq!(
            cx.clip_rect(),
            ClipRect {
                left: 15,
                top: 8,
                right: 40,
                bottom: 20
            }
        );
        cx.set_clip_rect(ClipRect {
            left: 1,
            top: 2,
            right: 1,
            bottom: 9,
        });
        assert_eq!(cx.clip_rect(), ClipRect::default());
    }
    assert_eq!(surface.clip, None);
    // Without a surface the rectangle is only kept.
    let mut cx = DeviceTrcCx {
        trc: &mut trc,
        fonts: &fonts,
        backend: &mut backend,
        surface: None,
    };
    cx.set_clip_rect(rect);
    assert_eq!(cx.clip_rect(), rect);
}

#[test]
fn images_load_stretch_and_advance() {
    let (mut fonts, mut backend) = setup();
    fonts.set_image_root(b"DATA");
    assert_eq!(fonts.image_root, b"DATA\\");
    // A 2 x 1 24-bit BMP: blue, then white.
    let mut bmp = Vec::new();
    bmp.extend_from_slice(b"BM");
    bmp.extend_from_slice(&62u32.to_le_bytes());
    bmp.extend_from_slice(&[0; 4]);
    bmp.extend_from_slice(&54u32.to_le_bytes());
    bmp.extend_from_slice(&40u32.to_le_bytes());
    bmp.extend_from_slice(&2i32.to_le_bytes());
    bmp.extend_from_slice(&1i32.to_le_bytes());
    bmp.extend_from_slice(&1u16.to_le_bytes());
    bmp.extend_from_slice(&24u16.to_le_bytes());
    bmp.extend_from_slice(&[0; 24]);
    bmp.extend_from_slice(&[0xFF, 0, 0, 0xFF, 0xFF, 0xFF, 0, 0]);
    let mut asked = Vec::new();
    let mut read = |path: &[u8]| {
        asked.push(path.to_vec());
        (path == b"DATA\\pip.bmp").then(|| bmp.clone())
    };
    let image = DeviceTrc::create_image(&fonts, b"pip.bmp", &mut read);
    let missing = DeviceTrc::create_image(&fonts, b"C:\\x.bmp", &mut read);
    let unc = DeviceTrc::create_image(&fonts, b"\\\\host\\x.bmp", &mut read);
    assert_eq!(
        asked,
        [
            b"DATA\\pip.bmp".to_vec(),
            b"C:\\x.bmp".to_vec(),
            b"\\\\host\\x.bmp".to_vec()
        ]
    );
    assert_eq!((image.width, image.height), (2.0, 1.0));
    assert_eq!((missing.width, missing.bitmap.is_none()), (0.0, true));
    assert!(unc.bitmap.is_none());

    fonts.scale_y = 1.5;
    let mut trc = DeviceTrc::new();
    trc.state.pos_h = 1;
    let mut surface = RichSurface::new(8, 4);
    {
        let mut cx = DeviceTrcCx {
            trc: &mut trc,
            fonts: &fonts,
            backend: &mut backend,
            surface: Some(&mut surface),
        };
        let list = RenderDisplayList {
            nodes: vec![
                RdlNode::Image(Some(std::rc::Rc::new(image))),
                RdlNode::Image(None),
            ],
        };
        let mut size = VisualSize::default();
        list.nodes[0].visual_size(&mut cx, &mut size);
        assert_eq!(
            size,
            VisualSize {
                width: 3,
                height: 1,
                ascent: 1,
                descent: 0
            }
        );
        list.execute(&mut cx, &StyleCollection::new(), true);
        assert_eq!(cx.trc().pos_h, 4);
    }
    assert_eq!(
        &surface.pixels[..5],
        &[0, 0x0000_00FF, 0x0000_00FF, 0x00FF_FFFF, 0]
    );
}
