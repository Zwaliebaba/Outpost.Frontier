//! The viewer over the engine: what one scene shows, the orbit camera and
//! the lights, on any render backend.

use std::fmt::Write as _;

use neuron_client::engine::camera::{ViewCamera, orbit};
use neuron_client::engine::{
    AnimationsId, DeformableId, EffectId, Engine, FrameInput, FrameReport, ModelId, SceneScriptId,
    SoundInfo,
};
use neuron_client::render::ff::device::Backend;
use neuron_client::render::ff::lights::LightData;
use neuron_client::ui::control::{ControlMesh, MeshLoader, PlainControl};
use neuron_client::ui::factory::CLASSES;
use neuron_client::ui::page::{MasterWin, PageDesc};
use neuron_client::ui::tree::NodeId;
use neuron_core::math::{Matrix, Transform, Vector};

use crate::catalog;
use crate::scenes::Scene;
use crate::synthetic;

/// `D3DRS_AMBIENT`.
const RS_AMBIENT: u32 = 139;
/// The ambient light the viewer sets before each frame (the game's lights
/// are P5.4's and P5.9's): enough to see every lit material.
const AMBIENT: u32 = 0xFF60_6060;
/// The key of the viewer's directional light in the light manager.
const LIGHT_KEY: u64 = 0x5649_4557;
/// The horizontal half angle of the viewer's camera, the room camera's.
const HALF_FOV_X: f32 = 35.0;

/// The orbit of the camera around the shown object.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Orbit {
    pub target: [f32; 3],
    /// Radians about the world's Y axis.
    pub yaw: f32,
    /// Radians above the XZ plane, kept inside (-1.5, 1.5).
    pub pitch: f32,
    pub distance: f32,
}

impl Orbit {
    /// Three quarters from the front and a little above, far enough for a
    /// sphere of `radius` around `centre` to fill most of the view.
    #[must_use]
    pub fn framing(centre: [f32; 3], radius: f32) -> Orbit {
        let radius = if radius.is_finite() && radius > 0.0 {
            radius
        } else {
            1.0
        };
        Orbit {
            target: centre,
            yaw: 0.6,
            pitch: 0.35,
            distance: radius * 2.6,
        }
    }

    /// Turns by `yaw` and `pitch` radians.
    pub fn turn(&mut self, yaw: f32, pitch: f32) {
        self.yaw = (self.yaw + yaw) % std::f32::consts::TAU;
        self.pitch = (self.pitch + pitch).clamp(-1.5, 1.5);
    }

    /// Moves closer (`steps` > 0) or away, 10% a step.
    #[cfg_attr(not(windows), allow(dead_code))]
    pub fn zoom(&mut self, steps: f32) {
        self.distance = (self.distance * 0.9f32.powf(steps)).max(0.05);
    }
}

/// A mesh instance of a page's `UIWinObject` that only exists: the
/// engine's UI draw has no mesh pass yet.
struct NoMesh;

impl ControlMesh for NoMesh {
    fn transform(&self) -> Transform {
        Transform::IDENTITY
    }
    fn set_position(&mut self, _: Vector) {}
    fn set_orientation(&mut self, _: Matrix) {}
    fn set_transform(&mut self, _: Transform) {}
    fn hardpoint(&self, _: &[u8]) -> Option<Transform> {
        None
    }
}

/// Gives every `UIWinObject` a [`NoMesh`] and counts them.
struct CountingLoader(std::rc::Rc<std::cell::Cell<usize>>);

impl MeshLoader for CountingLoader {
    fn load(&mut self, _: &[u8], _: &[u8]) -> Option<Box<dyn ControlMesh>> {
        self.0.set(self.0.get() + 1);
        Some(Box::new(NoMesh))
    }
}

/// What the current scene put into the engine.
#[derive(Default)]
struct Content {
    models: Vec<ModelId>,
    deformables: Vec<DeformableId>,
    effects: Vec<EffectId>,
    scripts: Vec<SceneScriptId>,
    page: Option<NodeId>,
}

/// The animations offered to the shown character.
struct Animations {
    id: AnimationsId,
    names: Vec<Vec<u8>>,
    current: usize,
}

/// The viewer.
pub struct Viewer<B: Backend + 'static> {
    pub engine: Engine<B>,
    pub camera: ViewCamera,
    pub orbit: Orbit,
    /// Whether the orbit turns by itself.
    pub spin: bool,
    content: Content,
    animations: Option<Animations>,
    character: Option<DeformableId>,
    sound: Option<SoundInfo>,
    /// What the scenes did, for the report and the console.
    pub log: Vec<String>,
    page_meshes: std::rc::Rc<std::cell::Cell<usize>>,
    /// The interface classes standing as bare controls.
    stand_ins: Vec<&'static str>,
    /// The libraries loaded so far (they stay until the engine drops).
    libraries: std::collections::BTreeSet<Vec<u8>>,
}

impl<B: Backend + 'static> Viewer<B> {
    /// The viewer over a built engine.
    pub fn new(mut engine: Engine<B>) -> Viewer<B> {
        let page_meshes = std::rc::Rc::new(std::cell::Cell::new(0));
        let mut stand_ins = Vec::new();
        if let Some(ui) = engine.ui_mut() {
            // The classes the toolkit does not have yet (P5's screens)
            // stand as bare controls, so that every shipped page loads.
            for (name, _) in CLASSES {
                if ui.factory.find(name.as_bytes()).is_none() {
                    ui.factory
                        .register(name, || Box::new(PlainControl::default()));
                    stand_ins.push(name);
                }
            }
            ui.mesh_loader = Some(Box::new(CountingLoader(page_meshes.clone())));
        }
        let camera = ViewCamera::new(engine.mode().back_buffer(), HALF_FOV_X, 0.25, 1_000_000.0);
        Viewer {
            engine,
            camera,
            orbit: Orbit::framing([0.0; 3], 10.0),
            spin: false,
            content: Content::default(),
            animations: None,
            character: None,
            sound: None,
            log: Vec::new(),
            page_meshes,
            stand_ins,
            libraries: std::collections::BTreeSet::new(),
        }
    }

    /// Removes what the shown scene put into the engine.
    pub fn clear(&mut self) {
        let content = std::mem::take(&mut self.content);
        for id in content.effects {
            self.engine.destroy_effect(id);
        }
        for id in content.deformables {
            self.engine.destroy_deformable(id);
        }
        for id in content.models {
            self.engine.destroy_model(id);
        }
        for id in content.scripts {
            self.engine.remove_scene_script(id);
        }
        if let (Some(id), Some(ui)) = (content.page, self.engine.ui_mut()) {
            ui.destroy(id);
            ui.delete(id);
        }
        self.animations = None;
        self.character = None;
        self.sound = None;
    }

    fn note(&mut self, line: String) {
        self.log.push(line);
    }

    /// Loads a model's material libraries and the model, and frames it.
    fn show_model(&mut self, archetype: &[u8], libraries: &[Vec<u8>]) -> Result<(), String> {
        for library in libraries {
            if !self.libraries.insert(library.to_ascii_lowercase()) {
                continue;
            }
            match self.engine.load_archive(library) {
                Ok(load) => self.note(format!(
                    "library {}: textures {:?}, materials {:?}, meshes {}",
                    lossy(library),
                    load.textures,
                    load.materials,
                    load.meshes
                )),
                Err(e) => self.note(format!("library {}: {e}", lossy(library))),
            }
        }
        let model = self
            .engine
            .create_model(archetype)
            .map_err(|e| e.to_string())?;
        self.frame_model(model, archetype);
        Ok(())
    }

    /// Keeps a model made for the scene and frames it.
    fn frame_model(&mut self, model: ModelId, archetype: &[u8]) {
        self.content.models.push(model);
        let (radius, centre) = self.engine.model_bounds(model).unwrap_or((10.0, [0.0; 3]));
        self.orbit = Orbit::framing(centre, radius);
        let parts = self.model_parts(model);
        self.note(format!(
            "model {}: {parts} instances, radius {radius:.2}",
            lossy(archetype)
        ));
    }

    fn model_parts(&self, model: ModelId) -> usize {
        let Some(root) = self.engine.model_root(model) else {
            return 0;
        };
        let scene = self.engine.scene().borrow();
        scene
            .instances()
            .filter(|(id, _)| scene.instance_root(Some(*id)) == Some(root))
            .count()
    }

    /// Shows one scene, replacing the shown one.
    ///
    /// # Errors
    ///
    /// What the scene needs is missing from the data or did not load.
    pub fn show(&mut self, scene: &Scene) -> Result<(), String> {
        self.clear();
        self.note(format!("scene {}", scene.label()));
        let data = self.engine.data().cloned().ok_or("no data root")?;
        match scene {
            Scene::Synthetic => {
                let model = self
                    .engine
                    .create_model_from_bytes(synthetic::MODEL, synthetic::model_bytes().into())
                    .map_err(|e| e.to_string())?;
                self.frame_model(model, synthetic::MODEL);
            }
            Scene::Ship(nickname) => {
                let entry = catalog::ship(&data, nickname.as_bytes())
                    .ok_or_else(|| format!("no ship {nickname} in shiparch.ini"))?;
                self.show_model(&entry.archetype, &entry.libraries)?;
            }
            Scene::Solar(nickname) => {
                let entry = catalog::solar(&data, nickname.as_bytes())
                    .ok_or_else(|| format!("no solar {nickname} in solararch.ini"))?;
                self.show_model(&entry.archetype, &entry.libraries)?;
            }
            Scene::Model { path, libraries } => {
                let libraries: Vec<Vec<u8>> =
                    libraries.iter().map(|l| l.as_bytes().to_vec()).collect();
                self.show_model(path.as_bytes(), &libraries)?;
            }
            Scene::Character {
                body,
                animations,
                animation,
            } => self.show_character(body, animations.as_deref(), animation.as_deref())?,
            Scene::Effect { file, name } => self.show_effect(file, name.as_deref())?,
            Scene::Script(path) => self.show_script(path)?,
            Scene::Sound(nickname) => {
                let file = catalog::sound(&data, nickname.as_bytes())
                    .ok_or_else(|| format!("no sound {nickname} in the sound tables"))?;
                let bytes = data
                    .read_file(&file)
                    .map_err(|_| format!("{}: not found", lossy(&file)))?
                    .data;
                self.load_and_play(&format!("sound {nickname} ({})", lossy(&file)), &bytes)?;
            }
            Scene::Voice { archive, line } => {
                let bytes = data
                    .read_file(archive.as_bytes())
                    .map_err(|_| format!("{archive}: not found"))?
                    .data;
                let lines = catalog::voice_lines(bytes);
                let chosen = match line {
                    Some(name) => lines.iter().find(|(n, _)| n.eq_ignore_ascii_case(name)),
                    None => lines.first(),
                }
                .ok_or_else(|| format!("{archive}: no such voice line"))?;
                let label = format!("voice line {archive}\\{} of {}", chosen.0, lines.len());
                let bytes = chosen.1.clone();
                self.load_and_play(&label, &bytes)?;
            }
            Scene::Page(path) => self.show_page(path)?,
        }
        Ok(())
    }

    fn show_character(
        &mut self,
        body: &str,
        animations: Option<&str>,
        animation: Option<&str>,
    ) -> Result<(), String> {
        if self.libraries.insert(body.as_bytes().to_ascii_lowercase()) {
            let _ = self.engine.load_archive(body.as_bytes());
        }
        let id = self
            .engine
            .create_deformable(body.as_bytes())
            .map_err(|e| e.to_string())?;
        self.content.deformables.push(id);
        self.character = Some(id);
        self.engine.set_deformable_lod(id, 0.0, &[0]);
        let (radius, centre) = self.engine.deformable_bounds(id).unwrap_or((1.0, [0.0; 3]));
        self.orbit = Orbit::framing(centre, radius);
        self.note(format!("character {body}: radius {radius:.2}"));
        let Some(file) = animations else {
            return Ok(());
        };
        let (anims, names) = self
            .engine
            .load_animations(file.as_bytes())
            .map_err(|e| e.to_string())?;
        self.note(format!("{file}: {} animations", names.len()));
        self.animations = Some(Animations {
            id: anims,
            names,
            current: 0,
        });
        match animation {
            Some(name) => {
                let at = self.animations.as_ref().and_then(|a| {
                    a.names
                        .iter()
                        .position(|n| n.eq_ignore_ascii_case(name.as_bytes()))
                });
                let at = at.ok_or_else(|| format!("{file} has no animation {name}"))?;
                self.play_animation(at)?;
            }
            None => self.next_animation(0)?,
        }
        Ok(())
    }

    /// Plays the animation `index` of the loaded list on the character.
    fn play_animation(&mut self, index: usize) -> Result<(), String> {
        let (Some(character), Some(anims)) = (self.character, self.animations.as_mut()) else {
            return Err("no character with animations".to_owned());
        };
        let name = anims.names.get(index).cloned().ok_or("no such animation")?;
        anims.current = index;
        let id = anims.id;
        let playback = self
            .engine
            .play_deformable_animation(character, id, &name, true)
            .map_err(|e| e.to_string())?;
        let tracks = self
            .engine
            .animation
            .playback(playback)
            .map_or(0, |p| p.typed.len());
        self.note(format!(
            "animation {} ({tracks} tracks bound)",
            lossy(&name)
        ));
        if tracks == 0 {
            return Err(format!("{} binds no track", lossy(&name)));
        }
        Ok(())
    }

    /// Plays the next animation from `from` on that binds tracks on the
    /// character.
    ///
    /// # Errors
    ///
    /// No animation of the list binds.
    pub fn next_animation(&mut self, from: usize) -> Result<(), String> {
        let count = self.animations.as_ref().map_or(0, |a| a.names.len());
        for step in 0..count {
            if self.play_animation((from + step) % count).is_ok() {
                return Ok(());
            }
        }
        Err("no animation binds on this character".to_owned())
    }

    /// The index after the playing animation.
    #[must_use]
    #[cfg_attr(not(windows), allow(dead_code))]
    pub fn animation_after(&self) -> usize {
        self.animations.as_ref().map_or(0, |a| a.current + 1)
    }

    fn show_effect(&mut self, file: &str, name: Option<&str>) -> Result<(), String> {
        for library in [&b"FX\\efx.txm"[..], file.as_bytes()] {
            if self.libraries.insert(library.to_ascii_lowercase()) {
                let _ = self.engine.load_archive(library);
            }
        }
        let names = self
            .engine
            .load_effects(file.as_bytes())
            .map_err(|e| e.to_string())?;
        let chosen = match name {
            Some(n) => names
                .iter()
                .find(|e| e.eq_ignore_ascii_case(n.as_bytes()))
                .cloned()
                .ok_or_else(|| format!("{file} has no effect {n}"))?,
            None => names.first().cloned().ok_or("no effect in the file")?,
        };
        let id = self
            .engine
            .create_effect(&chosen)
            .map_err(|e| e.to_string())?;
        self.content.effects.push(id);
        self.orbit = Orbit::framing([0.0; 3], 6.0);
        self.note(format!(
            "effect {} of {file} ({} effects)",
            lossy(&chosen),
            names.len()
        ));
        Ok(())
    }

    fn show_script(&mut self, path: &str) -> Result<(), String> {
        let id = self
            .engine
            .load_scene_script(path.as_bytes())
            .map_err(|e| e.to_string())?;
        self.content.scripts.push(id);
        if let Some(player) = self.engine.scene_script_mut(id) {
            player.set_loop(true);
            let line = format!(
                "scene script {path}: {} entities, {} events, {} ms",
                player.live_entity_count(),
                player.event_count(),
                player.duration()
            );
            self.note(line);
        }
        Ok(())
    }

    fn load_and_play(&mut self, label: &str, bytes: &[u8]) -> Result<(), String> {
        let info = self.engine.load_sound(bytes).map_err(|e| e.to_string())?;
        self.sound = Some(info);
        let played = self.engine.play_sound(info.id);
        self.note(format!(
            "{label}: {} ms, {} channels at {} Hz, {}",
            info.duration_ms,
            info.channels,
            info.rate,
            if played {
                "playing on the audio device"
            } else {
                "decoded (no audio device)"
            }
        ));
        Ok(())
    }

    /// Plays the scene's sound again.
    #[cfg_attr(not(windows), allow(dead_code))]
    pub fn replay_sound(&mut self) -> bool {
        self.sound.is_some_and(|s| self.engine.play_sound(s.id))
    }

    fn show_page(&mut self, path: &str) -> Result<(), String> {
        let data = self.engine.data().cloned().ok_or("no data")?;
        let bytes = data
            .read_file(path.as_bytes())
            .map_err(|_| format!("{path}: not found"))?
            .data;
        let desc = PageDesc::load(&bytes);
        if !desc.ok {
            return Err(format!("{path}: the page descriptor does not load"));
        }
        self.page_meshes.set(0);
        let ui = self.engine.ui_mut().ok_or("no UI manager")?;
        let id = MasterWin::open(ui, &desc).ok_or_else(|| format!("{path}: the page failed"))?;
        let page = ui.widget::<MasterWin>(id).and_then(|w| w.page.clone());
        let widgets = page.as_ref().map_or(0, |p| p.count());
        // The layout: each widget's class, name and place on the surface
        // through the interface camera.
        let mut layout = Vec::new();
        for i in 0..widgets {
            let Some(page) = page.as_ref() else { break };
            let (Some(widget), Some(name)) = (page.widget(i), page.widget_name(i)) else {
                continue;
            };
            let Some(node) = ui.node(widget) else {
                continue;
            };
            let class = node.class_name.unwrap_or("?");
            let toolkit = if self.stand_ins.contains(&class) {
                " (stand-in)"
            } else {
                ""
            };
            let place = ui.camera.project(node.position).map_or_else(
                || "off screen".to_owned(),
                |(x, y, _)| format!("({x:.0}, {y:.0})"),
            );
            layout.push(format!("  {class}{toolkit} {} at {place}", lossy(name)));
        }
        self.content.page = Some(id);
        self.orbit = Orbit::framing([0.0; 3], 10.0);
        self.note(format!(
            "page {path}: {} entries, {widgets} widgets, {} meshes not drawn",
            desc.entries.len(),
            self.page_meshes.get()
        ));
        self.log.extend(layout);
        Ok(())
    }

    /// The camera from the orbit, and the viewer's lights.
    fn prepare(&mut self, dt: f64) {
        if self.spin {
            #[allow(clippy::cast_possible_truncation)]
            self.orbit.turn((dt * 0.4) as f32, 0.0);
        }
        let o = self.orbit;
        let (orientation, position) = orbit(o.target, o.yaw, o.pitch, o.distance);
        self.camera.set_orientation(orientation);
        self.camera.set_position(position);
        // Planes that keep the depth buffer's precision on the object.
        let near = (o.distance / 2000.0).clamp(0.25, 50.0);
        if near.to_bits() != self.camera.z_near.to_bits() {
            self.camera.set_z_near(near);
        }
        self.camera.set_z_far((o.distance * 50.0).max(1000.0));
        let mut device = self.engine.device();
        let _ = device.state.set_render_state_raw(RS_AMBIENT, AMBIENT);
        let light = directional_light([-0.4, -0.6, -0.7]);
        device.lights.add_light(LIGHT_KEY, light);
        device.lights.set_light_enable(LIGHT_KEY, true);
    }

    /// One frame of the engine with the viewer's camera.
    pub fn frame(&mut self, input: FrameInput<'_>) -> FrameReport {
        self.prepare(input.delta_seconds);
        if let Some(id) = self.character {
            #[allow(clippy::cast_possible_truncation)]
            let dt = input.delta_seconds as f32;
            self.engine.set_deformable_lod(id, dt, &[0]);
        }
        self.engine.frame(input, &mut self.camera)
    }

    /// One line on what the scene shows now.
    #[must_use]
    pub fn status(&mut self) -> String {
        let mut line = String::new();
        let _ = write!(
            line,
            "orbit yaw {:.2} pitch {:.2} distance {:.1}",
            self.orbit.yaw, self.orbit.pitch, self.orbit.distance
        );
        for id in &self.content.scripts {
            if let Some(player) = self.engine.scene_script_mut(*id) {
                let _ = write!(
                    line,
                    "; script clock {} ms of {}, {} events active",
                    player.clock(),
                    player.duration(),
                    player.active().len()
                );
            }
        }
        if self.engine.animations_playing() > 0 {
            let _ = write!(line, "; animation playing");
        }
        let voices = self.engine.sounds_playing();
        if voices > 0 {
            let _ = write!(line, "; {voices} voices playing");
        }
        line
    }
}

/// A white directional light (`D3DLIGHT8`) shining along `direction`.
fn directional_light(direction: [f32; 3]) -> LightData {
    let mut words = [0u32; 26];
    words[0] = 3;
    for i in 0..4 {
        words[1 + i] = 0.8f32.to_bits();
        words[5 + i] = 0.0f32.to_bits();
        words[9 + i] = 0.0f32.to_bits();
    }
    words[16..19].copy_from_slice(&direction.map(f32::to_bits));
    words[19] = 1.0e6f32.to_bits();
    words[20] = 1.0f32.to_bits();
    words[21] = 1.0f32.to_bits();
    LightData(words)
}

pub fn lossy(bytes: &[u8]) -> String {
    String::from_utf8_lossy(bytes).into_owned()
}
