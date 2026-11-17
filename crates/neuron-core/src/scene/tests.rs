//! Unit tests of the scene runtime. The oracle comparisons are in
//! `tests/oracle_scene.rs`.

use std::sync::Arc;

use super::joint::{JointKind, JointPayload, axis_angle_to_quat, joint_local};
use super::mixer::ease;
use super::{Animation, Engine, Hardpoints, MixerKey};
use crate::anim::AnimScript;
use crate::math::{Quaternion, Transform, Vector};
use crate::vfs::{FileSystem, UtfFileSystem};

fn fixture(name: &str) -> Vec<u8> {
    let path = format!(
        "{}/tests/fixtures/oracle/scene/{name}",
        env!("CARGO_MANIFEST_DIR")
    );
    std::fs::read(&path).unwrap_or_else(|e| panic!("{path}: {e}"))
}

fn engine() -> Engine {
    let mut engine = Engine::new();
    engine.add_component(Box::new(Hardpoints::new()));
    engine
}

#[test]
fn joint_names_and_sizes() {
    assert_eq!(JointKind::from_name(b"rEv"), Some(JointKind::Rev));
    assert_eq!(JointKind::from_name(b"Revolute"), None);
    assert_eq!(JointKind::Sphere.record_size(), 0xD4);
    assert_eq!(JointKind::Cyl.record_size(), 0xD8);
    for kind in JointKind::ALL {
        assert_eq!(JointKind::from_code(kind.code()), Some(kind));
    }
    assert_eq!(JointPayload::default().code(), 0xFFFF_FFFF);
}

#[test]
fn sphere_payload_moves_its_limits() {
    let data: Vec<u8> = (0u32..21).flat_map(|i| (100 + i).to_le_bytes()).collect();
    let p = JointPayload::from_record(JointKind::Sphere, &data);
    assert_eq!(p.0[0], 4);
    assert_eq!(&p.0[1..16], &(100..115).collect::<Vec<u32>>()[..]);
    assert_eq!([p.0[16], p.0[17], p.0[18]], [119, 120, 0]);
    assert_eq!(&p.0[19..23], &[115, 116, 117, 118]);
    let fix = JointPayload::from_record(JointKind::Fix, &data);
    assert_eq!(&fix.0[..4], &[0, 0, 0, 0]);
    assert_eq!(&fix.0[4..16], &(100..112).collect::<Vec<u32>>()[..]);
}

#[test]
fn joints_without_state_rest_at_their_frame() {
    let mut p = JointPayload::zeroed(JointKind::Rev.code());
    for (i, v) in [1.0f32, 2.0, 3.0, 0.5, 0.25, 0.125].into_iter().enumerate() {
        p.0[1 + i] = v.to_bits();
    }
    for (i, v) in crate::math::Matrix::IDENTITY
        .to_words()
        .into_iter()
        .enumerate()
    {
        p.0[7 + i] = v.to_bits();
    }
    p.0[17] = 1.0f32.to_bits();
    let state = [0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0];
    let local = joint_local(&p, &state).unwrap_or_else(|| unreachable!());
    assert_eq!(local.rotation, crate::math::Matrix::IDENTITY);
    assert_eq!(local.translation, Vector::new(0.5, 1.75, 2.875));
    assert!(joint_local(&JointPayload::default(), &state).is_none());
    let q = axis_angle_to_quat(Vector::new(0.0, 2.0, 0.0), core::f32::consts::PI);
    assert!(q.w.abs() < 1e-6 && (q.y - 1.0).abs() < 1e-6);
    assert_eq!(
        axis_angle_to_quat(Vector::new(1.0, 0.0, 0.0), 0.0),
        Quaternion::IDENTITY
    );
}

#[test]
fn ease_runs_from_zero_to_one() {
    assert_eq!(ease(0), 0.0);
    assert_eq!(ease(255), 1.0);
    assert_eq!(ease(-5), 0.0);
    assert_eq!(ease(1000), 1.0);
    assert!(ease(100) < ease(101));
}

#[test]
fn compound_tree_and_release() {
    let mut engine = engine();
    let mut fs = UtfFileSystem::from_bytes(fixture("nested.cmp"), b"nested.cmp")
        .unwrap_or_else(|e| panic!("{e:?}"));
    let top = engine
        .create_archetype_handle(b"nested.cmp", Some(&mut fs))
        .unwrap_or_else(|e| panic!("{e}"))
        .unwrap_or_else(|| panic!("no archetype"));
    assert!(engine.archetype(top).is_some_and(|a| a.is_compound()));
    let root = engine
        .create_instance(top)
        .unwrap_or_else(|| panic!("no root"));
    let child = engine.related_instance(Some(root), 0, None);
    assert!(child.is_some());
    assert_eq!(engine.instance_parent(child), Some(root));
    assert_eq!(engine.instance_root(child), Some(root));
    engine.update_instance(Some(root), 0);
    let world = engine.instance_transform(child);
    assert_eq!(world.translation, Vector::new(0.0, 0.0, 5.0));
    assert!(engine.set_joint_params(child, 1, &[0.0; 7]));
    assert!(!engine.set_joint_params(Some(root), 0, &[1.0]));
    engine.destroy_instance(root);
    assert!(engine.instances().count() == 0);
    assert_eq!(engine.instance_transform(Some(root)), Transform::IDENTITY);
}

#[test]
fn scripts_play_through_the_mixer() {
    let mut engine = engine();
    let bytes = fixture("anim.cmp");
    let mut fs =
        UtfFileSystem::from_bytes(bytes.clone(), b"anim.cmp").unwrap_or_else(|e| panic!("{e:?}"));
    let top = engine
        .create_archetype_handle(b"anim.cmp", Some(&mut fs))
        .unwrap_or_else(|e| panic!("{e}"))
        .unwrap_or_else(|| panic!("no archetype"));
    let mut fs = UtfFileSystem::from_bytes(bytes, b"anim.cmp").unwrap_or_else(|e| panic!("{e:?}"));
    let script = Arc::new(
        AnimScript::open(&mut fs)
            .unwrap_or_else(|e| panic!("{e}"))
            .unwrap_or_else(|| panic!("no script")),
    );
    let root = engine
        .create_instance(top)
        .unwrap_or_else(|| panic!("no root"));
    let mut animation = Animation::new();
    let id = animation
        .play_script(&engine, &script, Some(root), b"Swing", 0, 0)
        .unwrap_or_else(|| panic!("no playback"));
    assert_eq!(id, super::FIRST_PLAYBACK_ID);
    assert_eq!(animation.objects(id).map(<[_]>::len), Some(3));
    assert_eq!(animation.root_height(id), Some(1.75));
    assert_eq!(
        animation.eval_playback(&engine, id, 0, 0.0, 1.0, 0.0, 1.0, 1.0, None),
        0
    );
    let before = engine
        .instances()
        .map(|(_, i)| i.joint_state)
        .collect::<Vec<_>>();
    animation.mixer_walk(&mut engine, MixerKey::of(Some(root)), 0.5);
    let after = engine
        .instances()
        .map(|(_, i)| i.joint_state)
        .collect::<Vec<_>>();
    assert_ne!(before, after);
    assert!(
        animation
            .play_script(&engine, &script, Some(root), b"Nothing", 0, 0)
            .is_none()
    );
    animation.destroy_script(&engine, id);
    assert!(animation.mixers().values().all(|lists| lists.is_empty()));
    engine.destroy_instance(root);
}

/// Every archive the loaders see may be damaged: flipping bytes anywhere
/// in a compound with a script must never panic.
#[test]
fn damaged_archives_do_not_panic() {
    let original = fixture("anim.cmp");
    let mut state = 0x1234_5678u32;
    for round in 0..200 {
        let mut bytes = original.clone();
        for _ in 0..1 + round % 4 {
            state = state.wrapping_mul(1_103_515_245).wrapping_add(12_345);
            let at = (state >> 8) as usize % bytes.len();
            bytes[at] ^= (state >> 3) as u8 | 1;
        }
        let mut engine = engine();
        let Ok(mut fs) = UtfFileSystem::from_bytes(bytes.clone(), b"anim.cmp") else {
            continue;
        };
        let Ok(Some(top)) = engine.create_archetype_handle(b"anim.cmp", Some(&mut fs)) else {
            continue;
        };
        let Some(root) = engine.create_instance(top) else {
            continue;
        };
        let Ok(mut fs) = UtfFileSystem::from_bytes(bytes, b"anim.cmp") else {
            continue;
        };
        let Ok(Some(script)) = AnimScript::open(&mut fs as &mut dyn FileSystem) else {
            continue;
        };
        let script = Arc::new(script);
        let mut animation = Animation::new();
        let names: Vec<Vec<u8>> = script.entries().map(|(_, e)| e.name.clone()).collect();
        for (i, name) in names.iter().enumerate() {
            if let Some(id) = animation.play_script(&engine, &script, Some(root), name, 0, 0) {
                let flags = [0, 2, 4, 3][i % 4];
                animation.eval_playback(&engine, id, flags, 0.0, 1.0, 0.25, 1.0, 1.0, None);
            }
        }
        for dt in [0.0, 0.3, 1.7] {
            animation.mixer_walk_all(&mut engine, dt);
            engine.update_instance(Some(root), 0);
            let _ = engine.instance_extent(Some(root), 0);
        }
        engine.destroy_instance(root);
        engine.release_handle(top);
    }
}
