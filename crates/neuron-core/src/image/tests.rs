use super::dds::{self, image_info};
use super::format::{PixelFormat, fourcc, match_source_pixel_format};
use super::format_map::FormatMaps;
use super::library::{LevelSource, LoadOutcome, LoaderSettings, TextureEvent, TextureLoader};
use super::tga::{self, TgaError, TgaHeader};
use super::to_rgba8;
use crate::utf::tests::{Item, build_full};
use crate::vfs::{FileSystem, FsDesc, GENERIC_READ, MemFileSystem, MemSource, UtfFileSystem};

fn tga_bytes(
    kind: u8,
    depth: u8,
    descriptor: u8,
    width: u16,
    height: u16,
    pixels: &[u8],
) -> Vec<u8> {
    let mut out = vec![0u8; 18];
    out[2] = kind;
    out[12..14].copy_from_slice(&width.to_le_bytes());
    out[14..16].copy_from_slice(&height.to_le_bytes());
    out[16] = depth;
    out[17] = descriptor;
    out.extend_from_slice(pixels);
    out
}

fn mapped_tga(pixels: &[u8]) -> Vec<u8> {
    let mut out = vec![0u8; 18];
    out[1] = 1;
    out[2] = 1;
    out[5..7].copy_from_slice(&256u16.to_le_bytes());
    out[7] = 24;
    out[12..14].copy_from_slice(&2u16.to_le_bytes());
    out[14..16].copy_from_slice(&2u16.to_le_bytes());
    out[16] = 8;
    for i in 0..256u32 {
        out.extend_from_slice(&[i as u8, (i >> 1) as u8, 0xFF - i as u8]);
    }
    out.extend_from_slice(pixels);
    out
}

fn dds_bytes(width: u32, height: u32, mips: u32, code: &[u8; 4], data: &[u8]) -> Vec<u8> {
    let mut out = vec![0u8; 0x80];
    let put =
        |out: &mut Vec<u8>, at: usize, v: u32| out[at..at + 4].copy_from_slice(&v.to_le_bytes());
    put(&mut out, 0, dds::MAGIC);
    put(&mut out, 4, 124);
    put(&mut out, 8, 0x000A_1007);
    put(&mut out, 0x0C, height);
    put(&mut out, 0x10, width);
    let block = if code == b"DXT1" { 8 } else { 16 };
    put(
        &mut out,
        0x14,
        width.div_ceil(4) * height.div_ceil(4) * block,
    );
    put(&mut out, 0x1C, mips);
    put(&mut out, 0x4C, 32);
    put(&mut out, 0x50, dds::PF_FOURCC);
    put(&mut out, 0x54, fourcc(*code));
    put(&mut out, 0x6C, 0x0040_1008);
    out.extend_from_slice(data);
    out
}

#[test]
fn format_tables() {
    assert_eq!(PixelFormat::from_d3d_format(0x15), PixelFormat::A8R8G8B8);
    assert_eq!(
        PixelFormat::from_d3d_format(fourcc(*b"DXT3")),
        PixelFormat::DXT3
    );
    assert_eq!(PixelFormat::from_d3d_format(0), PixelFormat::UNKNOWN);
    assert_eq!(PixelFormat::from_d3d_format(0x99), PixelFormat::UNKNOWN);
    assert_eq!(
        PixelFormat::from_fourcc(fourcc(*b"DAOP")),
        PixelFormat::DAOP
    );
    assert_eq!(PixelFormat::from_fourcc(0), PixelFormat(9));
    assert_eq!(PixelFormat::from_fourcc(0x15), PixelFormat::UNKNOWN);
    assert_eq!(PixelFormat(21).descriptor().id, 0);
    assert_eq!(PixelFormat::A1R5G5B5.alpha_bits(), 1);
    assert!(PixelFormat::DXT1.has_alpha());
    assert!(!PixelFormat(30).has_alpha());
    assert_eq!(
        match_source_pixel_format(16, 5, 5, 5, 0),
        Some(PixelFormat::X1R5G5B5)
    );
    assert_eq!(match_source_pixel_format(8, 8, 0, 0, 0), None);
    assert_eq!(
        match_source_pixel_format(32, 8, 8, 8, 8),
        Some(PixelFormat::A8R8G8B8)
    );
}

#[test]
fn tga_source_formats_and_orientation() {
    let pixels: Vec<u8> = (0..12).collect();
    // 2x2, 24 bits, bottom-left origin: copied as stored.
    let image = tga::decode(&tga_bytes(2, 24, 0, 2, 2, &pixels)).unwrap();
    assert_eq!(image.format, PixelFormat::R8G8B8);
    assert_eq!(image.pitch, 6);
    assert_eq!(image.pixels, pixels);
    // Top-left origin: rows reversed.
    let image = tga::decode(&tga_bytes(2, 24, 0x20, 2, 2, &pixels)).unwrap();
    assert_eq!(image.pixels, [6, 7, 8, 9, 10, 11, 0, 1, 2, 3, 4, 5]);
    // Right-to-left: bytes of each row reversed, rows reversed.
    let image = tga::decode(&tga_bytes(2, 24, 0x10, 2, 2, &pixels)).unwrap();
    assert_eq!(image.pixels, [11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0]);
    // Both: bytes reversed, rows kept.
    let image = tga::decode(&tga_bytes(2, 24, 0x30, 2, 2, &pixels)).unwrap();
    assert_eq!(image.pixels, [5, 4, 3, 2, 1, 0, 11, 10, 9, 8, 7, 6]);
    // 15 bits reads as 24.
    let header = TgaHeader::parse(&tga_bytes(2, 15, 0, 1, 1, &[])).unwrap();
    assert_eq!(header.source_format(), Some(PixelFormat::R8G8B8));
    // Greyscale has no format and decodes to nothing.
    let image = tga::decode(&tga_bytes(3, 8, 0, 2, 2, &[])).unwrap();
    assert_eq!(
        (image.format, image.pitch, image.pixels.len()),
        (PixelFormat::UNKNOWN, 0, 0)
    );
    assert_eq!(
        tga::decode(&tga_bytes(2, 12, 0, 1, 1, &[0; 4])),
        Err(TgaError::Unsupported)
    );
    assert_eq!(
        tga::decode(&tga_bytes(2, 32, 0, 2, 2, &[0; 15])),
        Err(TgaError::Truncated)
    );
    assert_eq!(
        tga::decode(&tga_bytes(2, 32, 0, 0xFFFF, 1, &[])),
        Err(TgaError::NegativeSize)
    );
    assert_eq!(tga::decode(&[0; 17]), Err(TgaError::TooShort));
}

#[test]
fn tga_palette() {
    let image = tga::decode(&mapped_tga(&[0, 1, 2, 255])).unwrap();
    assert_eq!(image.format, PixelFormat::P8);
    let palette = image.palette.clone().unwrap();
    assert_eq!(palette.len(), 768);
    // Entry bytes reversed: (i, i >> 1, 255 - i) becomes (255 - i, i >> 1, i).
    assert_eq!(&palette[3..6], &[254, 0, 1]);
    let rgba = to_rgba8(
        image.format,
        2,
        2,
        image.pitch,
        &image.pixels,
        image.palette_rgb().as_deref(),
    )
    .unwrap();
    assert_eq!(&rgba[4..8], &[254, 0, 1, 255]);
}

#[test]
fn dds_information() {
    let file = dds_bytes(8, 4, 2, b"DXT1", &[0; 24]);
    let info = image_info(&file).unwrap();
    assert_eq!((info.width, info.height, info.mip_levels), (8, 4, 2));
    assert_eq!(info.format, fourcc(*b"DXT1"));
    assert_eq!(info.resource_type, dds::RTYPE_TEXTURE);
    let mut cube = file.clone();
    cube[0x70..0x74].copy_from_slice(&0xFE00u32.to_le_bytes());
    assert_eq!(
        image_info(&cube).unwrap().resource_type,
        dds::RTYPE_CUBETEXTURE
    );
    let mut bad = file.clone();
    bad[0x54..0x58].copy_from_slice(b"ATI2");
    assert_eq!(image_info(&bad), None);
    assert_eq!(image_info(&file[..0x7F]), None);
    let mut rgb = file;
    rgb[0x50..0x54].copy_from_slice(&0x41u32.to_le_bytes());
    for (at, v) in [
        (0x58, 16u32),
        (0x5C, 0x7C00),
        (0x60, 0x3E0),
        (0x64, 0x1F),
        (0x68, 0x8000),
    ] {
        rgb[at..at + 4].copy_from_slice(&v.to_le_bytes());
    }
    assert_eq!(image_info(&rgb).unwrap().format, 25);
}

#[test]
fn format_maps() {
    let maps = FormatMaps::defaults();
    let picks: Vec<u32> = (0..15)
        .map(|i| maps.pick(PixelFormat(i)).map_or(99, |f| f.0))
        .collect();
    assert_eq!(
        picks,
        [16, 15, 16, 16, 16, 20, 19, 21, 16, 16, 10, 11, 12, 13, 14]
    );
    let mut maps = FormatMaps::defaults();
    let mut line = *b"clearance\0";
    maps.parse_line(&mut line);
    assert!(maps.rows.is_empty());
    let mut line = vec![0u8; 64];
    line[..20].copy_from_slice(b"16:5:6:5:*:* = DAOT ");
    maps.parse_line(&mut line);
    assert_eq!(maps.rows.len(), 1);
    assert_eq!(maps.rows[0].value, [16, 5, 6, 5, 0, 0]);
    assert_eq!(maps.rows[0].mask, [!0, !0, !0, !0, 0, 0]);
    assert_eq!(maps.rows[0].fourcc, fourcc(*b"DAOT"));
    assert_eq!(maps.pick(PixelFormat::R5G6B5), Some(PixelFormat::DAOT));
    assert_eq!(maps.pick(PixelFormat::X1R5G5B5), None);
    // A line without a value adds nothing...
    let mut line = *b"8:8 =  \0";
    maps.parse_line(&mut line);
    assert_eq!(maps.rows.len(), 1);
    // ...but the value search steps over the NUL a comment left, as the
    // reference does, and finds the text after it; the field scans then
    // run into that text and overwrite it with NUL bytes.
    let mut line = *b"8:8 ; = DAOP\0";
    maps.parse_line(&mut line);
    assert_eq!(maps.rows.len(), 2);
    assert_eq!(maps.rows[1].fourcc, u32::from_le_bytes([0, b' ', 0, 0]));
    assert_eq!(maps.rows[1].value, [8, 8, 0, 0, 0, 0]);
}

#[test]
fn rgba_conversion() {
    // 5-6-5 white and pure red; widening by replication.
    let rgba = to_rgba8(
        PixelFormat::R5G6B5,
        2,
        1,
        4,
        &[0xFF, 0xFF, 0x00, 0xF8],
        None,
    )
    .unwrap();
    assert_eq!(rgba, [255, 255, 255, 255, 255, 0, 0, 255]);
    // A DXT1 block: c0 = white > c1 = black, indices 0, 1, 2, 3 in row 0.
    let block = [0xFF, 0xFF, 0x00, 0x00, 0b1110_0100, 0, 0, 0];
    let rgba = to_rgba8(PixelFormat::DXT1, 4, 1, 8, &block, None).unwrap();
    assert_eq!(
        &rgba[..16],
        &[
            255, 255, 255, 255, 0, 0, 0, 255, 170, 170, 170, 255, 85, 85, 85, 255
        ]
    );
    assert!(to_rgba8(PixelFormat::DXT5, 4, 4, 16, &[0; 15], None).is_err());
    assert!(to_rgba8(PixelFormat::P8, 1, 1, 1, &[0], None).is_err());
    assert!(to_rgba8(PixelFormat::DAOP, 1, 1, 1, &[0], None).is_err());
}

fn archive(items: &[Item]) -> MemFileSystem {
    MemFileSystem::new(
        None,
        GENERIC_READ,
        3,
        MemSource::Copy(Some(&build_full(items))),
    )
}

fn sample_library() -> Vec<Item> {
    let tga = tga_bytes(2, 32, 0, 2, 2, &[7; 16]);
    let small = tga_bytes(2, 32, 0, 1, 1, &[9; 4]);
    let frames: Vec<u8> = [
        0u32,
        0,
        0,
        0x3F80_0000,
        0x3F80_0000,
        1,
        0x7F80_0001,
        0,
        0,
        0,
    ]
    .iter()
    .flat_map(|w| w.to_le_bytes())
    .collect();
    vec![Item::Dir(
        b"Texture library",
        vec![
            Item::Dir(
                b"anim",
                vec![
                    Item::File(b"Texture count", 2u32.to_le_bytes().to_vec()),
                    Item::File(b"Frame count", 2u32.to_le_bytes().to_vec()),
                    Item::File(b"Frame rects", frames),
                ],
            ),
            Item::Dir(
                b"dxt",
                vec![Item::File(
                    b"MIPS",
                    dds_bytes(8, 8, 3, b"DXT1", &[1; 32 + 8 + 8]),
                )],
            ),
            Item::Dir(
                b"anim_0",
                vec![
                    Item::File(b"MIP1", small.clone()),
                    Item::File(b"MIP0", tga.clone()),
                ],
            ),
            Item::Dir(b"anim_1", vec![Item::File(b"MIP0", small)]),
            Item::Dir(b"none", vec![Item::File(b"junk", vec![1, 2, 3])]),
            Item::Dir(b"long", vec![Item::File(b"MIP10", tga)]),
        ],
    )]
}

#[test]
fn texture_library_loading() {
    let mem = archive(&sample_library());
    let mut fs = UtfFileSystem::open_memory(&mem, &FsDesc::read(b"tex.txm")).unwrap();
    let mut loader = TextureLoader::new(LoaderSettings::default());
    let entries = loader.load_group(&mut fs).unwrap();
    let names: Vec<&[u8]> = entries.iter().map(|e| e.name.as_slice()).collect();
    assert_eq!(
        names,
        [&b"anim"[..], b"dxt", b"anim_0", b"anim_1", b"none", b"long"]
    );
    let results: Vec<LoadOutcome> = entries.iter().map(|e| e.outcome).collect();
    let anim = loader.find(b"anim").unwrap();
    let anim_0 = loader.find(b"ANIM_0").unwrap();
    assert_eq!(
        results[0],
        LoadOutcome::Done {
            result: 0,
            record: Some(anim)
        }
    );
    assert!(matches!(
        results[1],
        LoadOutcome::Done {
            result: 0,
            record: Some(_)
        }
    ));
    // anim_0 was loaded as a child of anim, so the group finds it.
    assert_eq!(
        results[2],
        LoadOutcome::Done {
            result: 0,
            record: Some(anim_0)
        }
    );
    assert_eq!(
        results[4],
        LoadOutcome::Done {
            result: -8,
            record: None
        }
    );
    assert_eq!(
        results[5],
        LoadOutcome::Done {
            result: -8,
            record: None
        }
    );

    let record = &loader.records[anim];
    assert_eq!(record.handle, 0);
    assert_eq!(record.tile_rate, 24.0f32.to_bits());
    assert_eq!(record.frames.len(), 2);
    // The x87 copy quiets a signalling NaN.
    assert_eq!(record.frames[1].uv[0], 0x7FC0_0001);
    // Group reference plus one per frame-holding parent.
    assert_eq!(loader.records[anim].ref_count, 1);
    assert_eq!(loader.records[anim_0].ref_count, 2);

    // The DDS levels: 8x8, 4x4, 2x2 with the linear sizes 32, 8, 8.
    let levels: Vec<(u32, u32, u32, u64)> = loader.events[entries[1].events.clone()]
        .iter()
        .filter_map(|e| match e {
            TextureEvent::Level(l) => match &l.source {
                LevelSource::Mapped { offset, .. } => {
                    Some((l.width, l.pitch, l.result as u32, *offset))
                }
                LevelSource::Decoded(_) => None,
            },
            _ => None,
        })
        .collect();
    assert_eq!(levels, [(8, 16, 0, 0x80), (4, 8, 0, 0xA0), (2, 8, 0, 0xA8)]);
    // TGA levels follow the find order of the names, MIP1 then MIP0, but
    // upload by index.
    let tga: Vec<(u32, u32)> = loader.events[entries[0].events.clone()]
        .iter()
        .filter_map(|e| match e {
            TextureEvent::Level(l) if matches!(l.source, LevelSource::Decoded(_)) => {
                Some((l.level, l.width))
            }
            _ => None,
        })
        .collect();
    assert_eq!(tga, [(0, 2), (1, 1), (0, 1)]);
}

#[test]
fn lod_and_dimension_policies() {
    let settings = LoaderSettings {
        lod_min: 4,
        pow2_policy: 1,
        square_policy: 2,
        ..LoaderSettings::default()
    };
    let loader = TextureLoader::new(settings);
    let (mut w, mut h) = (16, 8);
    assert_eq!(loader.select_mip_levels(&mut w, &mut h), 2);
    assert_eq!((w, h), (4, 2));
    assert_eq!(loader.select_dimensions(3, 5), (8, 8));
    assert_eq!(loader.select_dimensions(0, 5), (0, 5));
    assert_eq!(super::library::round_power_of_two(0), 0);
    assert_eq!(super::library::round_power_of_two(0x8000_0001), 1);
}

/// Deterministic mutations of a library archive: every outcome is a
/// result or a fault, never a panic.
#[test]
fn mutated_libraries_do_not_panic() {
    let base = build_full(&sample_library());
    let mut seed = 0x1234_5678u32;
    for round in 0..600 {
        let mut bytes = base.clone();
        for _ in 0..1 + round % 5 {
            seed = seed.wrapping_mul(1_103_515_245).wrapping_add(12345);
            let at = (seed >> 8) as usize % bytes.len();
            bytes[at] = (seed >> 3) as u8;
        }
        let mem = MemFileSystem::new(None, GENERIC_READ, 3, MemSource::Copy(Some(&bytes)));
        let Ok(mut fs) = UtfFileSystem::open_memory(&mem, &FsDesc::read(b"m")) else {
            continue;
        };
        let mut loader = TextureLoader::new(LoaderSettings::default());
        if loader.load_group(&mut fs).is_some() {
            for event in &loader.events {
                if let TextureEvent::Level(level) = event {
                    let _ = level.to_rgba8();
                }
            }
        }
        let _ = fs.open_node(b"..");
    }
    for len in 0..200 {
        let bytes: Vec<u8> = (0..len).map(|i| (i * 37 % 256) as u8).collect();
        let _ = tga::decode(&bytes);
        let _ = image_info(&bytes);
        let mut maps = FormatMaps::defaults();
        let mut line = bytes.clone();
        maps.parse_line(&mut line);
    }
}
