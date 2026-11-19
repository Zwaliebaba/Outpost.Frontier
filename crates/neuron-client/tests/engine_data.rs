//! The engine over the original data (`outdir/`, decision O15; work
//! package P4.16): built from the shipped `NeuronComponents.ini` and
//! `OutpostFrontier.ini` on the recording backend, it loads a ship through
//! the scene engine, a skinned character playing a body animation, an
//! engine effect and a scene script, and runs frames that draw them, the
//! interface and present.
//! Nothing is written or committed.

use std::fs;
use std::path::{Path, PathBuf};

use neuron_client::engine::{Engine, EngineConfig, FrameInput, Phase, TEARDOWN_ORDER};
use neuron_client::render::appearance::Camera;
use neuron_client::render::ff::device::RecordingBackend;
use neuron_client::text::font::fake::FakeBackend;
use neuron_client::window::DisplayMode;

/// A camera 60 units behind the origin on +z, looking down -z.
struct Viewer;

impl Camera for Viewer {
    fn position(&mut self) -> [f32; 3] {
        [0.0, 0.0, 60.0]
    }
    fn orientation(&mut self) -> [f32; 9] {
        [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]
    }
    fn viewport(&mut self) -> [i32; 4] {
        [0, 0, 1023, 767]
    }
    fn half_fov_x_deg(&mut self) -> f32 {
        35.0
    }
    fn fov_y_deg(&mut self) -> f32 {
        50.0
    }
    fn z_near(&mut self) -> f32 {
        1.0
    }
    fn z_far(&mut self) -> f32 {
        10_000.0
    }
    fn aspect(&mut self) -> f32 {
        4.0 / 3.0
    }
    fn x_scale_over_near(&mut self) -> f64 {
        1.0
    }
    fn y_scale_over_near(&mut self) -> f64 {
        1.0
    }
    fn project(&mut self, _: [f32; 3], _: &mut [f32; 3]) -> i32 {
        0
    }
    fn unproject(&mut self, _: &mut [f32; 3], _: f32, _: f32) {}
    fn classify_sphere(&mut self, _: [f32; 3], _: f32) -> i32 {
        4
    }
    fn view_transform(&mut self) -> [f32; 12] {
        [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, -60.0]
    }
}

/// The draw packets of one idle frame.
fn frame_packets(engine: &mut Engine<RecordingBackend>) -> usize {
    let before = engine.device().backend.draws.len();
    engine.frame(FrameInput::idle(1.0 / 30.0), &mut Viewer);
    engine.device().backend.draws.len() - before
}

fn outdir() -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir")
}

#[test]
#[allow(clippy::too_many_lines)]
fn the_engine_draws_frames_of_the_original_data() {
    let base = outdir();
    let (Ok(engine_ini), Ok(client_ini)) = (
        fs::read(base.join("NeuronComponents.ini")),
        fs::read(base.join("OutpostFrontier.ini")),
    ) else {
        println!("outdir has no INI files: nothing to run");
        return;
    };
    let data = base.join("data");
    if !data.join("SHIPS").is_dir() {
        println!("outdir/data is missing: nothing to run");
        return;
    }
    let mode = DisplayMode {
        width: 1024,
        height: 768,
        fullscreen: false,
    };
    let mut config = EngineConfig::new(&data, mode);
    config.engine_ini = Some(engine_ini);
    config.client_ini = Some(client_ini);
    config.font_backend = Some(Box::new(FakeBackend::default()));
    let mut engine: Engine<RecordingBackend> =
        Engine::new(config, RecordingBackend::default()).expect("the engine");
    let lifecycle = engine.lifecycle();

    // What construction loaded from the data.
    let fonts = engine.fonts().map_or(0, |f| f.fonts.len());
    let styles = engine.fonts().map_or(0, |f| f.styles.ids().count());
    assert!(fonts > 0, "fonts.ini gives fonts");
    assert!(styles > 0, "rich_fonts.ini gives styles");
    assert!(
        engine.cursors().ready && engine.cursors().cursors.len() > 1,
        "mouse.ini's cursors"
    );
    assert!(
        engine.ui().is_some_and(|u| u.ui_shapes.arrow.is_some()),
        "UITextures.ini's slider shapes"
    );
    assert!(engine.appearance().is_some(), "FLAppearance initialised");
    let construction_notices = engine.notices.clone();

    // A ship through the scene engine (its materials and textures from
    // the class's library), a character, an engine effect, a scene script.
    let library = engine
        .load_archive(b"SHIPS\\CIVILIAN\\cv_ships.mat")
        .expect("the ship materials");
    assert!(library.materials.is_some());
    let ship = engine
        .create_model(b"SHIPS\\CIVILIAN\\CV_STARFLIER\\cv_starflier.cmp")
        .expect("the ship");
    let parts = engine
        .scene()
        .borrow()
        .instances()
        .filter(|(id, _)| {
            engine
                .scene()
                .borrow()
                .instance_root(Some(*id))
                .is_some_and(|r| Some(r) == engine.model_root(ship))
        })
        .count();
    assert!(parts > 1, "the compound's parts are instances");
    let ship_packets = frame_packets(&mut engine);
    let character = b"CHARACTERS\\BODIES\\br_bartender_body.dfm";
    let _ = engine.load_archive(character);
    let body = engine.create_deformable(character).expect("the character");
    engine.set_deformable_lod(body, 0.0, &[0]);
    let character_packets = frame_packets(&mut engine) - ship_packets;
    // A body animation through the animation system.
    let (animations, names) = engine
        .load_animations(b"CHARACTERS\\ANIMATIONS\\bodygenericmale.anm")
        .expect("the body animations");
    assert!(!names.is_empty());
    let mut played = None;
    for name in &names {
        if let Ok(id) = engine.play_deformable_animation(body, animations, name, true) {
            let tracks = engine.animation.playback(id).map_or(0, |p| p.typed.len());
            if tracks > 0 {
                played = Some((name.clone(), tracks));
                break;
            }
        }
    }
    let (played, tracks) =
        played.expect("an animation of bodygenericmale.anm binds tracks on the body");
    assert_eq!(engine.animations_playing(), 1);
    assert!(engine.deformable_bounds(body).is_some_and(|(r, _)| r > 0.0));
    let _ = engine.load_archive(b"FX\\ENGINES\\gf_br_csv_engine01.ale");
    let _ = engine.load_archive(b"FX\\efx.txm");
    let names = engine
        .load_effects(b"FX\\ENGINES\\gf_br_csv_engine01.ale")
        .expect("the engine effect");
    let effect = engine
        .create_effect(names.first().expect("an effect"))
        .expect("the effect is made");
    let script = engine
        .load_scene_script(b"MISSIONS\\M03\\m03_survivor1.thn")
        .expect("the scene script");

    // Three frames.
    let mut reports = Vec::new();
    let mut packets = Vec::new();
    for _ in 0..3 {
        let before = engine.device().backend.draws.len();
        reports.push(engine.frame(FrameInput::idle(1.0 / 30.0), &mut Viewer));
        packets.push(engine.device().backend.draws.len() - before);
    }
    let presents = engine.device().backend.presents;
    let clock = engine.scene_script_mut(script).map_or(0, |p| p.clock());
    println!(
        "{fonts} fonts, {styles} styles; {parts} ship instances; frames: {reports:?}; \
         draw packets: ship {ship_packets}, character {character_packets}, per frame of all \
         {packets:?}; scene clock {clock} ms; animation {} with {tracks} tracks; notices: \
         {construction_notices:?}",
        String::from_utf8_lossy(&played)
    );
    assert_eq!(presents, 5);
    assert!(ship_packets > 0, "the ship draws");
    assert!(character_packets > 0, "the character draws");
    for report in &reports {
        assert_eq!(report.models, 1);
        assert_eq!(report.deformables, 1);
        assert_eq!(report.effects, 1);
        assert_eq!(report.scripts, 1);
        assert_eq!(
            report.animations,
            1,
            "{} advances",
            String::from_utf8_lossy(&played)
        );
    }
    assert!(
        packets
            .iter()
            .all(|&p| p >= ship_packets + character_packets),
        "every frame draws the ship and the character"
    );
    assert!(clock > 0, "the scene script's timeline advanced");
    assert!(
        !construction_notices
            .iter()
            .any(|n| n.contains("decision T22") || n.contains("not loaded")),
        "{construction_notices:?}"
    );

    assert!(engine.model_bounds(ship).is_some_and(|(r, _)| r > 0.0));
    engine.destroy_effect(effect);
    engine.destroy_deformable(body);
    assert_eq!(engine.animations_playing(), 0);
    engine.destroy_model(ship);
    assert_eq!(
        engine.scene().borrow().instances().count(),
        0,
        "the scene engine has no instances left"
    );
    drop(engine);
    assert_eq!(lifecycle.stages(Phase::Teardown), TEARDOWN_ORDER.to_vec());
}
