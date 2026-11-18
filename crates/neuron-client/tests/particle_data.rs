//! Every shipped particle effect (`outdir/data`, decision O15) through the
//! simulation of work package P4.8: all `.ale` files load into one node
//! library and one effect library, as the client loads them, and every
//! effect is instantiated under a folder, stepped for a second of frames,
//! told to stop emitting, stepped until its appearances finish (or a time
//! limit), and released. Nothing is compared with the oracle (the
//! synthetic suites `particle_ramp` and `particle_sim` do that); the test
//! checks that no effect panics, that every record finds its node, that
//! particles are made and that the pools are whole again at the end.
//! Nothing it computes is committed.

use std::fs;
use std::path::{Path, PathBuf};

use neuron_client::render::particles::{Effect, NoTextures, ParticleFx, PoolKind};
use neuron_core::fx_data::{self as fx, AleFile, NodeClass};

fn ale_files(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(dir) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        if path.is_dir() {
            ale_files(&path, out);
        } else if path
            .extension()
            .is_some_and(|e| e.eq_ignore_ascii_case("ale"))
        {
            out.push(path);
        }
    }
}

/// What the loaded ramps hold that the reference would read past its
/// tables or keys for (divergence D370): flat ramps of more than one key
/// with a mode above 6 (the shipped data has seven such ramps, each with a
/// single key, whose interpolator is never called), nested ramps without
/// keys, loop types outside 0 to 4 per nibble.
#[derive(Default, Debug)]
struct Oddities {
    flat_modes: usize,
    empty_nested: usize,
    loop_types: usize,
}

impl Oddities {
    fn anim(&mut self, ramp: &fx::AnimatedSingleRamp) {
        self.empty_nested += usize::from(ramp.keys.is_empty());
        for key in &ramp.keys {
            let lt = key.value.loop_type();
            self.loop_types += usize::from(lt & 0xF > 4 || (lt >> 4) & 0xF > 4 || lt > 0xFF);
        }
    }

    fn single(&mut self, ramp: &fx::RampSingleRamp) {
        self.empty_nested += usize::from(ramp.keys.is_empty());
        for key in &ramp.keys {
            self.flat_modes += usize::from(key.value.mode > 6 && key.value.keys.len() > 1);
        }
    }

    fn slot(&mut self, slot: Option<&fx::NestedSingle>) {
        match slot {
            Some(fx::NestedSingle::Ramp(r)) => self.single(r),
            Some(fx::NestedSingle::Animated(r)) => self.anim(r),
            None => {}
        }
    }

    fn set(&mut self, set: &fx::PropertySet) {
        set.transform.channels.iter().for_each(|c| self.anim(c));
        match &set.data {
            fx::ClassData::Emitter(e) => {
                for r in [
                    &e.frequency,
                    &e.max_particles,
                    &e.init_life_span,
                    &e.pressure,
                    &e.emit_count,
                    &e.velocity_approach,
                ] {
                    self.anim(r);
                }
                self.single(&e.lod_curve);
            }
            fx::ClassData::Billboard(b) => {
                self.single(&b.lod_curve);
                self.empty_nested += usize::from(b.color.keys.is_empty());
                for key in &b.color.keys {
                    self.flat_modes += usize::from(key.value.mode > 6 && key.value.keys.len() > 1);
                }
                for slot in [
                    &b.size,
                    &b.primary_size,
                    &b.tex_frame,
                    &b.axis_override,
                    &b.frame,
                    &b.common_tex_frame,
                ] {
                    self.slot(slot.as_ref());
                }
            }
            fx::ClassData::Field(fx::Field::Radial { attenuation, .. }) => self.single(attenuation),
            _ => {}
        }
    }
}

const IDENTITY: [f32; 12] = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0];
const DT: f32 = 1.0 / 30.0;

#[test]
fn every_shipped_effect_runs() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data");
    let mut files = Vec::new();
    ale_files(&root, &mut files);
    if files.is_empty() {
        println!("{} has no .ale files: nothing to run", root.display());
        return;
    }
    files.sort();
    let mut fx = ParticleFx::new();
    let mut hashes = Vec::new();
    let mut oddities = Oddities::default();
    for path in &files {
        let bytes = fs::read(path).expect("readable");
        let ale = AleFile::parse(bytes).expect("a UTF archive");
        for node in ale.nodes.nodes() {
            oddities.set(&node.props);
        }
        hashes.extend(
            ale.effects
                .effects()
                .iter()
                .map(|e| neuron_core::fx_data::name_hash(&e.name)),
        );
        fx.load(&ale);
    }
    hashes.sort_unstable();
    hashes.dedup();
    let mut svc = NoTextures;
    let (mut created, mut missing, mut emitting, mut finished) = (0, 0, 0, 0);
    let mut peak = 0_u32;
    let mut skipped_classes = 0;
    for &hash in &hashes {
        let parent = fx.create_node(NodeClass::Node);
        let mut effect = match fx.create_effect(parent, hash, &mut svc) {
            Ok(effect) => effect,
            Err(_) => {
                missing += 1;
                fx.release(parent, &mut svc);
                continue;
            }
        };
        created += 1;
        skipped_classes += effect
            .records()
            .iter()
            .filter(|r| {
                matches!(
                    fx.node(r.node).map(|n| n.class()),
                    Some(NodeClass::ParticleAppearance | NodeClass::MeshAppearance)
                )
            })
            .count();
        let mut live_peak = 0;
        let frame = |effect: &Effect, fx: &mut ParticleFx, svc: &mut NoTextures| {
            effect.set_transform(fx, 0, &IDENTITY);
            effect.compose(fx);
            effect.update(fx, DT, svc);
            let live: u32 = effect
                .records()
                .iter()
                .filter_map(|r| fx.node(r.node).and_then(|n| n.appearance()).map(|a| a.live))
                .sum();
            live
        };
        for _ in 0..30 {
            live_peak = live_peak.max(frame(&effect, &mut fx, &mut svc));
        }
        effect.dispatch(&mut fx, 1, &mut svc);
        let mut done = false;
        for _ in 0..600 {
            frame(&effect, &mut fx, &mut svc);
            if effect.finished(&fx, 4) {
                done = true;
                break;
            }
        }
        if live_peak > 0 {
            emitting += 1;
        }
        if done {
            finished += 1;
        }
        peak = peak.max(live_peak);
        effect.dispatch(&mut fx, 7, &mut svc);
        effect.release(&mut fx, &mut svc);
        fx.release(parent, &mut svc);
    }
    // Every particle went back to its pool.
    for kind in PoolKind::ALL {
        let pool = fx.pools.get(kind);
        assert_eq!(pool.free_count(), pool.capacity(), "{kind:?} pool leaked");
    }
    println!(
        "{} files, {} effects: {created} created, {missing} with a missing node, \
         {emitting} emitted particles (peak {peak} live), {finished} finished after the stop; \
         {skipped_classes} particle and mesh appearance records not simulated yet",
        files.len(),
        hashes.len()
    );
    assert_eq!(oddities.flat_modes, 0, "an interpolator past the table");
    assert_eq!(oddities.empty_nested, 0, "a nested ramp without keys");
    assert_eq!(
        oddities.loop_types, 0,
        "a loop type the track does not know"
    );
    assert_eq!(missing, 0, "every record finds its node");
    assert!(emitting > created / 2, "most effects make particles");
}
