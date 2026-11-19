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
use neuron_client::render::deformable::AimDesc;
use neuron_client::render::ff::device::RecordingBackend;
use neuron_client::render::particles::PoolKind;
use neuron_client::text::font::fake::FakeBackend;
use neuron_client::window::DisplayMode;
use neuron_core::scene::InstanceId;

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
            let tracks = engine.animation().playback(id).map_or(0, |p| p.typed.len());
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

    // The bones' poses before the frames.
    let poses = |engine: &mut Engine<RecordingBackend>| -> Vec<[f32; 12]> {
        let instance = engine.deformable_mut(body).expect("the body");
        instance
            .parts
            .iter()
            .filter_map(|p| p.lod.as_ref())
            .flat_map(|h| h.bones.iter().map(|b| b.local))
            .collect()
    };
    let before = poses(&mut engine);
    engine.listen_deformable(body);

    // Three frames.
    let mut reports = Vec::new();
    let mut packets = Vec::new();
    for _ in 0..3 {
        let before = engine.device().backend.draws.len();
        reports.push(engine.frame(FrameInput::idle(1.0 / 30.0), &mut Viewer));
        packets.push(engine.device().backend.draws.len() - before);
    }
    let after = poses(&mut engine);
    let moved = before.iter().zip(&after).filter(|(a, b)| a != b).count();
    assert!(
        moved > 0,
        "{} moves the bartender's bones",
        String::from_utf8_lossy(&played)
    );
    assert_eq!(
        engine.deformable_list_count(body),
        1,
        "the looping animation stays on the list"
    );
    let events = engine.take_deformable_events();
    let presents = engine.device().backend.presents;
    let clock = engine.scene_script_mut(script).map_or(0, |p| p.clock());
    println!(
        "{fonts} fonts, {styles} styles; {parts} ship instances; frames: {reports:?}; \
         draw packets: ship {ship_packets}, character {character_packets}, per frame of all \
         {packets:?}; scene clock {clock} ms; animation {} with {tracks} tracks, {moved} bone \
         poses moved, {} sink events; notices: {construction_notices:?}",
        String::from_utf8_lossy(&played),
        events.len()
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

    // An aim of the bartender's head through its neck (`IDeformable::StartAim`): the frames'
    // walks tick its typed channels, whose owner solves on the scene engine; once stopped, the
    // sweep drops it and the animation plays on.
    let bones: Vec<i32> = engine
        .deformable_mut(body)
        .and_then(|i| {
            i.parts[0]
                .lod
                .as_ref()
                .map(|h| h.bones.iter().map(|b| b.instance).collect())
        })
        .unwrap_or_default();
    let (head, root) = {
        let scene = engine.scene().borrow();
        let id = |b: i32| u32::try_from(b).ok().map(InstanceId::from_index);
        let head = bones
            .iter()
            .copied()
            .find(|&b| {
                scene
                    .instance_name(id(b))
                    .0
                    .is_some_and(|n| n.eq_ignore_ascii_case(b"Body_Head"))
            })
            .expect("the bartender has a head bone");
        let neck = scene.instance_parent(id(head));
        let root = scene.instance_parent(neck).expect("the neck has a parent");
        (head, i32::try_from(root.index()).unwrap_or(-1))
    };
    let desc = AimDesc {
        root,
        end: head,
        flags: 0x21,
        target_pos: [0.0, 2.0, 1.0],
        target_orient: [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0],
        damping: 0.1,
        locks: Vec::new(),
    };
    let aim = engine
        .start_deformable_aim(body, &desc, 0)
        .expect("the aim starts");
    assert_eq!(engine.deformable_list_count(body), 2);
    for _ in 0..2 {
        engine.frame(FrameInput::idle(1.0 / 30.0), &mut Viewer);
    }
    assert_eq!(engine.deformable_list_count(body), 2, "the aim holds");
    engine.stop_deformable_aim(body, aim);
    engine.frame(FrameInput::idle(1.0 / 30.0), &mut Viewer);
    assert_eq!(
        engine.deformable_list_count(body),
        1,
        "the stopped aim is swept"
    );
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

/// The interface's control meshes through the engine (P4.13): the
/// navigation bar of `INTERFACE\BASESIDE\navbar.ini` builds its twelve
/// `ButtonControl`s, whose meshes the engine's loader makes as scene
/// archetypes and instances (`ClientMeshLoadFromPath`; the seven buttons
/// that name `unknown.3db`, which the data lacks, have none); the pass
/// submits them through the render manager with the Win3D camera, and
/// deleting the bar destroys their instances.
#[test]
fn the_navigation_bar_draws_its_meshes_through_the_engine() {
    let data = outdir().join("data");
    let (Ok(engine_ini), Ok(client_ini), Ok(navbar)) = (
        fs::read(outdir().join("NeuronComponents.ini")),
        fs::read(outdir().join("OutpostFrontier.ini")),
        fs::read(data.join("INTERFACE/BASESIDE/navbar.ini")),
    ) else {
        println!("outdir/ has no data: nothing to draw");
        return;
    };
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
    let ui = engine.ui_mut().expect("the UI manager");
    let bar = ui
        .create_by_class(
            Some(b"NavBar"),
            b"WinControl",
            None,
            &mut neuron_client::ui::tree::Payload::None,
        )
        .expect("the bar");
    let mut ini = neuron_core::ini::IniReader::new();
    assert!(ini.open_memory(&navbar));
    neuron_client::ui::control::load_ini(ui, bar, &mut ini);
    assert_eq!(ui.children(bar).len(), 12);
    // The bar placed one unit in front of the Win3D camera (`SetPosition`;
    // the base screen that places it is P5's): the buttons' meshes move to
    // their offsets from there.
    ui.set_position(bar, neuron_core::math::Vector::new(0.0, 0.0, -1.0));
    let made = engine.ui_meshes_made();
    assert_eq!(made, 5, "a mesh for each button whose file exists");
    let instances = engine.scene().borrow().instances().count();
    assert!(instances >= 5, "{instances} instances");
    let mut reports = Vec::new();
    let mut packets = Vec::new();
    for _ in 0..2 {
        let before = engine.device().backend.draws.len();
        reports.push(engine.frame(FrameInput::idle(1.0 / 30.0), &mut Viewer));
        packets.push(engine.device().backend.draws.len() - before);
    }
    let last = reports[1];
    println!(
        "navbar.ini: {made} meshes, {instances} instances; frames {reports:?}, packets {packets:?}"
    );
    assert!(last.ui_meshes >= 5, "every button's mesh is submitted");
    assert!(last.ui_draws > 0, "the meshes draw");
    let ui = engine.ui_mut().expect("the UI manager");
    ui.destroy(bar);
    ui.delete(bar);
    assert_eq!(
        engine.scene().borrow().instances().count(),
        0,
        "the buttons' instances go with them"
    );
}

/// The controls `Win3DDrawAll` draws for the engine (P4.13, seventh
/// increment): a button whose mesh names a tile of the button montage
/// draws the tile as an icon into the "buttons" list, which the pass
/// flushes; a button's body pass writes its tint into the holder's floats,
/// which its instances have as their user data; a static control that
/// renders to a window draws through a viewport of its own and puts the
/// Win3D camera's back; the game options gate the draws.
#[test]
#[allow(clippy::too_many_lines)]
fn the_interface_pass_draws_montage_icons_tints_and_windows() {
    let data = outdir().join("data");
    let (Ok(engine_ini), Ok(client_ini)) = (
        fs::read(outdir().join("NeuronComponents.ini")),
        fs::read(outdir().join("OutpostFrontier.ini")),
    ) else {
        println!("outdir/ has no data: nothing to draw");
        return;
    };
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
    let ui = engine.ui_mut().expect("the UI manager");
    assert!(ui.montage.loaded, "the montage loads with the UI manager");
    assert_eq!(ui.montage.tiles.len(), 72, "the tiles of ButtonMontage.ini");
    let formation = ui
        .montage
        .find(b"INTERFACE\\HUD\\HUD_FORMATION.3DB")
        .expect("the formation icon");
    assert!(formation.shape.is_some(), "on the Montage32 shape");
    let page = b"[BaseFrame]\noffset = 0, 0, -1\nsubcontrol = Icon\nsubcontrol = Body\n\
subcontrol = Window\n\n\
[Icon]\nclass_name = ButtonControl\nmesh = interface\\HUD\\HUD_formation.3db\n\
offset = -0.2, 0, 0\nmouse_size = 0.02, 0.02, 0\n\n\
[Body]\nclass_name = ButtonControl\nmesh = interface\\baseside\\take_off.3db\n\
offset = 0.2, 0, 0\nmouse_size = 0.02, 0.02, 0\n\n\
[Window]\nclass_name = StaticControl\nmesh = interface\\baseside\\take_off.3db\n\
offset = 0, 0.1, 0\nmouse_size = 0.05, 0.05, 0.05\nrender_to_window = 1\n";
    let frame = ui
        .create_by_class(
            Some(b"Frame"),
            b"WinControl",
            None,
            &mut neuron_client::ui::tree::Payload::None,
        )
        .expect("the frame");
    let mut ini = neuron_core::ini::IniReader::new();
    assert!(ini.open_memory(page));
    neuron_client::ui::control::load_ini(ui, frame, &mut ini);
    assert_eq!(ui.children(frame).len(), 3);
    ui.set_position(frame, neuron_core::math::Vector::new(0.0, 0.0, -1.0));
    // `Create` loads the icon button's mesh too (only `Init` skips a tile
    // with a scale); the tile draws in its place.
    assert_eq!(engine.ui_meshes_made(), 3);
    let floats_before: Vec<[f32; 5]> = {
        let render = engine.render();
        let table = render.stack.user_floats.borrow();
        table.values().map(|f| f.get()).collect()
    };
    assert!(
        floats_before.len() >= 2 && floats_before.iter().all(|f| *f == [1.0; 5]),
        "every instance of the two trees reads its holder's floats, all 1 at first"
    );
    let mut reports = Vec::new();
    let mut packets = Vec::new();
    for _ in 0..2 {
        let before = engine.device().backend.draws.len();
        reports.push(engine.frame(FrameInput::idle(1.0 / 30.0), &mut Viewer));
        packets.push(engine.device().backend.draws[before..].to_vec());
    }
    let last = reports[1];
    println!("frames {reports:?}");
    assert_eq!(last.ui_montage_icons, 1, "the formation icon");
    let icon_draws = packets[1]
        .iter()
        .filter(|d| d.fvf.0 == 0x1C4 && d.textures[0].is_some() && d.vertex_count == 4)
        .count();
    assert!(icon_draws >= 1, "the buttons list goes out as screen quads");
    let window_draws = packets[1]
        .iter()
        .filter(|d| d.viewport.width < 1024 && d.viewport.width > 0)
        .count();
    assert!(window_draws > 0, "the static control draws into its window");
    assert!(
        packets[1]
            .last()
            .is_some_and(|d| d.viewport.width == 1024 && d.viewport.height == 768),
        "the Win3D camera's viewport is back"
    );
    let tinted: Vec<[f32; 5]> = {
        let render = engine.render();
        let table = render.stack.user_floats.borrow();
        table.values().map(|f| f.get()).collect()
    };
    // The body button's and the static control's trees are tinted (the idle
    // cyan); the icon button's mesh is never submitted and keeps its 1s.
    let written = tinted.iter().filter(|f| **f != [1.0; 5]).count();
    assert!(
        written >= 2 && written < tinted.len() && tinted.iter().all(|f| f[3] == 1.0),
        "the body passes wrote their tints: {tinted:?}"
    );
    // SHIP WIRES off: the tinted and outlined submits stop; LIKE_PETAL on:
    // the projection cache is off for the next frame.
    let meshes_with_wires = last.ui_meshes;
    engine
        .options
        .set(neuron_client::engine::options::SHIP_WIRES, 0);
    let report = engine.frame(FrameInput::idle(1.0 / 30.0), &mut Viewer);
    assert!(
        report.ui_meshes < meshes_with_wires,
        "{} < {meshes_with_wires}",
        report.ui_meshes
    );
    engine
        .options
        .set(neuron_client::engine::options::DRAW_SCROLL_BAR_CONTROLS, 0);
    engine.options.set(0x14, 0);
    let report = engine.frame(FrameInput::idle(1.0 / 30.0), &mut Viewer);
    assert_eq!(report.ui_montage_icons, 0, "no button controls drawn");
    let ui = engine.ui_mut().expect("the UI manager");
    ui.destroy(frame);
    ui.delete(frame);
    assert!(
        engine.render().stack.user_floats.borrow().is_empty(),
        "the holders' floats go with their instances"
    );
}

/// Models are named by their whole relative path (D481, closed): the
/// shipped interface has three different `front_button.cmp` files, and
/// each loads as an archetype of its own instead of the first one found.
#[test]
fn models_of_one_file_name_in_several_folders_stay_apart() {
    let data = outdir().join("data");
    let (Ok(engine_ini), Ok(client_ini)) = (
        fs::read(outdir().join("NeuronComponents.ini")),
        fs::read(outdir().join("OutpostFrontier.ini")),
    ) else {
        println!("outdir/ has no data: nothing to load");
        return;
    };
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
    let paths: [&[u8]; 3] = [
        b"INTERFACE\\INTRO\\OBJECTS\\front_button.cmp",
        b"INTERFACE\\LOADSAVE\\front_button.cmp",
        b"INTERFACE\\NEURONET\\INVENTORY\\front_button.cmp",
    ];
    let mut archetypes = Vec::new();
    for path in paths {
        engine.create_model(path).expect("the model");
        let id = engine
            .scene()
            .borrow()
            .lookup_archetype(neuron_core::hash::name_crc32(path))
            .expect("an archetype under the whole path");
        archetypes.push(id);
    }
    assert_ne!(archetypes[0], archetypes[1]);
    assert_ne!(archetypes[0], archetypes[2]);
    assert_ne!(archetypes[1], archetypes[2]);
}

/// The particle effects' client wiring over the original data: the mesh
/// appearance of the trade lane tube (`gf_TLR_tube`) asks the engine's mesh
/// factory for `fx\misc\TLRtube.3db` (divergence D490), whose instances the
/// frame poses and draws through the render manager; the explosion's debris
/// (`Explode`) carry their child effects, drawn with the effect. Releasing
/// the effects destroys the meshes' instances.
#[test]
fn the_engine_draws_mesh_appearances_and_child_effects() {
    let base = outdir();
    let (Ok(engine_ini), Ok(client_ini)) = (
        fs::read(base.join("NeuronComponents.ini")),
        fs::read(base.join("OutpostFrontier.ini")),
    ) else {
        println!("outdir has no INI files: nothing to run");
        return;
    };
    let data = base.join("data");
    if !data.join("FX").is_dir() {
        println!("outdir/data is missing: nothing to run");
        return;
    }
    let mode = DisplayMode {
        width: 640,
        height: 480,
        fullscreen: false,
    };
    let mut config = EngineConfig::new(&data, mode);
    config.engine_ini = Some(engine_ini);
    config.client_ini = Some(client_ini);
    config.font_backend = Some(Box::new(FakeBackend::default()));
    let mut engine: Engine<RecordingBackend> =
        Engine::new(config, RecordingBackend::default()).expect("the engine");
    engine
        .load_effects(b"FX\\MISC\\gf_tlr_tube.ale")
        .expect("the tube's effects");
    engine
        .load_effects(b"FX\\MISC\\standardeffects.ale")
        .expect("the standard effects");
    let tube = engine.create_effect(b"gf_TLR_tube").expect("the tube");
    let mut peak = (0, 0);
    let mut packets = 0;
    for _ in 0..12 {
        packets += frame_packets(&mut engine);
        let (live, instanced) = engine.fx_mesh_count();
        peak = (peak.0.max(live), peak.1.max(instanced));
    }
    assert!(peak.1 > 0, "the tube's meshes have instances: {peak:?}");
    assert!(packets > 0, "the tube draws");
    let explode = engine.create_effect(b"Explode").expect("the explosion");
    for _ in 0..8 {
        frame_packets(&mut engine);
    }
    let fx = &engine.particles;
    let children = fx
        .pools
        .get(PoolKind::Particle)
        .used()
        .iter()
        .filter(|&&i| fx.pools.record(i).life.is_some())
        .count();
    assert!(children > 0, "the debris carry their child effects");
    engine.destroy_effect(explode);
    engine.destroy_effect(tube);
    assert_eq!(engine.fx_mesh_count(), (0, 0), "every mesh went back");
    assert_eq!(
        engine.scene().borrow().instances().count(),
        0,
        "the meshes' instances are destroyed"
    );
    println!(
        "the tube held up to {} meshes ({} with an instance) over 12 frames, {packets} draw \
         packets; the explosion's debris carried {children} child effects",
        peak.0, peak.1
    );
}
