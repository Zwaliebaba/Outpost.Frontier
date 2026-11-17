use super::props::kind;
use super::ramp::{default_grey, pack_rgb, unpack_rgb};
use super::*;

fn fixture(name: &str) -> Vec<u8> {
    let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/fixtures/oracle/fx")
        .join(name);
    std::fs::read(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()))
}

#[test]
fn row_names_hash_to_row_hashes() {
    for class in NodeClass::ALL {
        for row in class.rows() {
            // The reference's row gives this name with a hash that is not
            // its CRC; the data uses the hash.
            if row.name == "BeamApp_LineAppearance" {
                assert_eq!(row.hash, 0x1C65_B7B9);
                continue;
            }
            assert_eq!(name_hash(row.name.as_bytes()), row.hash, "{}", row.name);
        }
    }
}

#[test]
fn every_row_is_owned_by_its_class() {
    for class in NodeClass::ALL {
        let set = PropertySet::new(class, class.factory_name());
        for row in class.rows() {
            let owned = match row.kind {
                kind::BOOL => set.get_bool(row.hash).is_ok(),
                kind::INT => set.get_int(row.hash).is_ok(),
                kind::FLOAT => set.get_float(row.hash).is_ok(),
                kind::NAME => set.get_name(row.hash).is_ok(),
                kind::PAIR => set.get_pair(row.hash).is_ok(),
                kind::TRANSFORM => set.transform(row.hash).is_some(),
                kind::SINGLE_RAMP_RAMP => set.single_ramp_ramp(row.hash).is_some(),
                kind::COLOR_RAMP_RAMP => set.color_ramp_ramp(row.hash).is_some(),
                kind::ANIMATED_SINGLE_RAMP => set.animated_single_ramp(row.hash).is_some(),
                _ => false,
            };
            assert!(owned, "{class:?} {}", row.name);
        }
    }
}

#[test]
fn class_names_round_trip() {
    for class in NodeClass::ALL {
        assert_eq!(
            NodeClass::from_factory_name(class.factory_name()),
            Some(class)
        );
    }
    assert_eq!(NodeClass::from_factory_name(b"fxnode"), None);
    assert_eq!(NodeClass::from_factory_name(b"FxColideField"), None);
}

#[test]
fn colour_packing() {
    assert_eq!(default_grey(), 0x00CC_CCCC);
    assert_eq!(pack_rgb([1.0, 0.5, 0.0]), 0x00FF_7F00);
    // Out of range channels spill; NaN and infinities give 0.
    assert_eq!(pack_rgb([-0.1, 1.0, 0.0]), 0xFFE7_FF00);
    assert_eq!(pack_rgb([f32::NAN, f32::INFINITY, f32::NEG_INFINITY]), 0);
    assert_eq!(unpack_rgb(0x00FF_0000), [1.0, 0.0, 0.0]);
}

#[test]
fn key_order_rules() {
    let mut ramp = RampSingle::new();
    ramp.add_key(1.0, 1.0);
    ramp.add_key(1.0, 2.0);
    ramp.add_key(f32::NAN, 3.0);
    ramp.add_key(0.0, 4.0);
    let values: Vec<f32> = ramp.keys.iter().map(|k| k.value).collect();
    assert_eq!(values, [4.0, 1.0, 2.0, 3.0]);

    let mut value = AnimatedSingle::new();
    assert!(!value.set_loop_type(1));
    value.add_key(1.0, [1.0, 0.0, 0.0]);
    value.add_key(1.0, [2.0, 0.0, 0.0]);
    value.add_key(f32::NAN, [3.0, 0.0, 0.0]);
    let values: Vec<f32> = value.keys().iter().map(|k| k.value).collect();
    assert_eq!(values, [2.0, 1.0, 3.0]);
    assert!(value.set_loop_type(1));
    value.reset();
    assert_eq!((value.key_count(), value.loop_type()), (0, 1));
}

#[test]
fn transform_defaults_and_flags() {
    let mut transform = AnimatedTransform::new();
    assert_eq!(transform.modes(), [4, 3, 5]);
    assert_eq!(transform.channel_flags, 0x1FF);
    transform.channels[7].keys[0].value.weight = 2.0;
    transform.channels[0].keys.clear();
    transform.update_channel_flags();
    assert_eq!(transform.channel_flags, 0x1FF & !0x80);
}

#[test]
fn defaults() {
    let radial = PropertySet::new(NodeClass::RadialField, b"FxRadialField");
    let falloff = radial.single_ramp_ramp(props::RADIAL_ATTENUATION).unwrap();
    let child = &falloff.keys[0].value;
    assert_eq!(child.mode, 3);
    assert_eq!(child.keys.len(), 2);
    assert_eq!(child.keys[1].value.to_bits(), 0x3A83_126F);

    let beam = PropertySet::new(NodeClass::BeamAppearance, b"FLBeamAppearance");
    assert_eq!(beam.get_bool(0xF87B_5FD5), Ok(1));
    assert_eq!(beam.get_pair(props::BILLBOARD_BLEND_INFO), Ok([5, 2]));
    assert!(beam.transform(props::BEAM_LOCAL_TRANSFORM).is_some());
    assert!(beam.transform(props::MESH_PARTICLE_TRANSFORM).is_none());
    assert_eq!(
        beam.get_name(props::BILLBOARD_TEX_NAME),
        Ok(Some(&b"???"[..]))
    );

    let mut node = PropertySet::new(NodeClass::Node, b"FxNode");
    assert_eq!(node.get_name(props::NODE_NAME), Ok(None));
    assert_eq!(
        node.set_name(props::NODE_CLASS_NAME, b"x"),
        Err(PropertyError::ReadOnly)
    );
    assert_eq!(node.set_int(1, 1), Err(PropertyError::Miss));
}

#[test]
fn synthetic_file_loads() {
    let ale = AleFile::parse(fixture("basic.ale")).unwrap();
    assert_eq!(ale.nodes_result, Ok(()));
    assert_eq!(ale.effects_result, Ok(()));
    assert_eq!(ale.nodes.nodes().len(), NodeClass::ALL.len());
    let collide = ale.nodes.find(name_hash(b"collide")).unwrap();
    assert_eq!(collide.props.class, NodeClass::CollideField);
    assert_eq!(
        collide.props.class_name.as_deref(),
        Some(&b"FxCollideField"[..])
    );
    let auto = ale.effects.find(name_hash(b"fx_auto")).unwrap();
    let ids: Vec<u32> = auto.nodes.iter().map(|n| n.id).collect();
    assert_eq!(ids, [1, 2, 7, 8, 7, 0x8000_0000, 9]);
    assert_eq!(auto.pairs.len(), 3);
}

#[test]
fn failed_node_library_keeps_its_directory_open() {
    let ale = AleFile::parse(fixture("missing_leaf.ale")).unwrap();
    assert_eq!(ale.nodes_result, Err(FxError::ShortHeader));
    assert_eq!(ale.effects_result, Err(FxError::NoDirectory));
}

/// Deterministic mutations of a synthetic file: byte changes, truncations
/// and splices must never panic.
#[test]
fn mutations_do_not_panic() {
    let base = fixture("basic.ale");
    let mut state = 0x2545_F491_u32;
    let mut next = move || {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        state
    };
    for round in 0..3000 {
        let mut bytes = base.clone();
        let len = bytes.len() as u32;
        match round % 4 {
            0 => {
                for _ in 0..1 + next() % 8 {
                    let at = (next() % len) as usize;
                    bytes[at] = next() as u8;
                }
            }
            1 => bytes.truncate((next() % len) as usize),
            2 => {
                let at = (next() % len) as usize;
                let value = [0x00, 0xFF, 0x80, 0x7F][(next() % 4) as usize];
                for b in bytes.iter_mut().skip(at).take(4) {
                    *b = value;
                }
            }
            _ => {
                let from = (next() % len) as usize;
                let to = (next() % len) as usize;
                let count = (next() % 64) as usize;
                for i in 0..count {
                    if from + i < bytes.len() && to + i < bytes.len() {
                        bytes[to + i] = bytes[from + i];
                    }
                }
            }
        }
        let _ = AleFile::parse(bytes);
    }
}
