//! The P4.7 animation suite (third increment, with P4.16's composition):
//! a deformable instance's animations and aim events through the scene
//! engine and the animation system of P1.8, as the engine composes them
//! (`engine::SceneDeformHost` for `Deformable2`'s engine half, its aim
//! owners and sinks, `engine::walk_deformable` for the tick of
//! `FEngineCharacterBody::Advance`), with the contract
//! `CppSource/Oracle/oracle_deformable_anim.cpp` states (its header lists
//! the operations and lines), over `tests/fixtures/oracle/deformable_anim.list`
//! and the archives `deformable/skel.cmp` and `deformable/moves.anm`
//! (`make_deformable_fixtures.py`).

use std::cell::RefCell;
use std::rc::Rc;
use std::sync::Arc;

use neuron_client::engine::{
    DeformableEvent, Links, SceneDeformHost, SceneHost, SharedAnimation, SharedDeformables,
    sink_token, walk_deformable,
};
use neuron_client::render::deformable::ik::AimDesc;
use neuron_client::render::deformable::{
    BoneRuntime, BoneSlot, DeformHost, DeformInstance, DeformPart, DeformServices, Instance,
    LodHolder, Skeleton,
};
use neuron_client::render::ff::defaults::Defaults;
use neuron_client::render::ff::device::{Device, RecordingBackend};
use neuron_client::render::material::{MaterialBatcher, Shading, ShadingFactory};
use neuron_client::render::mesh::VMeshRegistry;
use neuron_client::render::stack::{RenderStack, StackDeform};
use neuron_client::render::texture::{FormatClasses, LibraryConfig, TextureLibrary};
use neuron_core::anim::AnimScript;
use neuron_core::scene::{Animation, Engine as SceneEngine, InstanceId};
use neuron_core::vfs::{FsDesc, Host, UtfFileSystem};

use super::{fixtures, input_lines};

fn f(value: f32) -> String {
    format!("{:08x}", value.to_bits())
}

fn fs(values: &[f32]) -> String {
    values.iter().map(|&v| f(v)).collect::<Vec<_>>().join(",")
}

fn hex_word(text: &str) -> u32 {
    u32::from_str_radix(text, 16).unwrap_or_else(|e| panic!("{text}: {e}"))
}

fn hex_float(text: &str) -> f32 {
    f32::from_bits(hex_word(text))
}

fn hex_floats(text: &str) -> Vec<f32> {
    if text == "-" || text.is_empty() {
        return Vec::new();
    }
    text.split(',').map(hex_float).collect()
}

fn dec(text: &str) -> usize {
    text.parse().unwrap_or_else(|e| panic!("{text}: {e}"))
}

fn decs(text: &str) -> Vec<usize> {
    if text == "-" || text.is_empty() {
        return Vec::new();
    }
    text.split(',').map(dec).collect()
}

fn id(instance: Instance) -> Option<InstanceId> {
    u32::try_from(instance).ok().map(InstanceId::from_index)
}

/// `UTF` file system over a fixture archive (`deformable\...`).
fn archive(path: &str) -> UtfFileSystem {
    let host = Host::new(fixtures());
    let desc = FsDesc::read(path.as_bytes()).with_implementation(b"UTF");
    UtfFileSystem::open(&host, &desc).unwrap_or_else(|e| panic!("{path}: {e:?}"))
}

struct Case {
    index: usize,
    scene: Rc<RefCell<SceneEngine>>,
    animation: SharedAnimation,
    models: SharedDeformables,
    host: SceneDeformHost,
    stack: RenderStack<RecordingBackend>,
    events: Vec<DeformableEvent>,
    root: Instance,
    bones: Vec<Instance>,
    script: u32,
    aims: Vec<Option<u32>>,
    out: Vec<String>,
}

impl Case {
    fn new(index: usize) -> Case {
        let scene = Rc::new(RefCell::new(SceneEngine::new()));
        let animation: SharedAnimation = Rc::new(RefCell::new(Animation::new()));
        let models: SharedDeformables = Rc::new(RefCell::new(Vec::new()));
        let links = Links::shared();
        let host = SceneDeformHost::new(
            Rc::clone(&scene),
            Rc::clone(&links),
            Rc::clone(&animation),
            &models,
        );
        let device = Device::new(Defaults::builtin(), 64, 64, RecordingBackend::default());
        let shading = Shading::new(
            device,
            TextureLibrary::new(LibraryConfig::default()),
            FormatClasses::builtin(),
        );
        let batcher: MaterialBatcher<RenderStack<RecordingBackend>> =
            MaterialBatcher::new(Box::new(ShadingFactory::new()), None);
        let stack = RenderStack::new(
            shading,
            batcher,
            VMeshRegistry::shared(),
            Box::new(SceneHost::detached(links)),
        );
        Case {
            index,
            scene,
            animation,
            models,
            host,
            stack,
            events: Vec::new(),
            root: -1,
            bones: Vec::new(),
            script: 0,
            aims: Vec::new(),
            out: Vec::new(),
        }
    }

    fn line(&mut self, body: &str) {
        self.out
            .push(format!("{{\"case\":\"{}\",{body}}}", self.index));
    }

    /// `f` on the instance with the services its `IDeformable` calls go to.
    fn with<R>(&mut self, f: impl FnOnce(&mut DeformInstance, &mut dyn DeformServices) -> R) -> R {
        let mut models = self.models.borrow_mut();
        let instance = models[0].as_mut().expect("the instance");
        self.host.set_current(Some(0));
        let mut services = StackDeform::new(&mut self.stack, &mut self.host);
        let out = f(instance, &mut services);
        self.host.set_current(None);
        out
    }

    fn instance(&self) -> std::cell::RefMut<'_, DeformInstance> {
        std::cell::RefMut::map(self.models.borrow_mut(), |m| {
            m[0].as_mut().expect("the instance")
        })
    }

    fn bones_text(&self) -> String {
        let instance = self.instance();
        let holder = instance.parts[0].lod.as_ref().expect("a holder");
        holder.bones[..holder.bound()]
            .iter()
            .map(|b| format!("{:x}:{}", b.flags, fs(&b.local)))
            .collect::<Vec<_>>()
            .join(";")
    }

    fn model(&mut self, t: &[&str]) {
        let mut skel = archive(t[1]);
        let handle = self.host.create_archetype(t[1].as_bytes(), &mut skel);
        self.root = if handle == -1 {
            -1
        } else {
            self.host.create_instance(handle, None)
        };
        self.bones.clear();
        if self.root != -1 {
            self.bones.push(self.root);
            let scene = self.scene.borrow();
            let mut cursor = scene.related_instance(id(self.root), 0, None);
            while let Some(at) = cursor {
                self.bones.push(i32::try_from(at.index()).unwrap_or(-1));
                cursor = scene.related_instance(id(self.root), 0, Some(at));
            }
        }
        let count = self.bones.len();
        let mut holder = LodHolder {
            skeleton: Skeleton {
                bones: vec![
                    BoneSlot {
                        archetype: -1,
                        bone_to_root: [0.0; 12],
                    };
                    count
                ],
                ..Skeleton::default()
            },
            bones: vec![BoneRuntime::default(); count],
            bind_cursor: 0,
        };
        for (index, &bone) in self.bones.iter().enumerate() {
            holder.bind_one(&mut self.host, index, bone);
        }
        holder.bind_cursor = i32::try_from(count).unwrap_or(0);
        let part = DeformPart {
            lod: Some(holder),
            ..DeformPart::default()
        };
        let instance = DeformInstance::with_parts(vec![part], 0, hex_word(t[3]));
        self.models.borrow_mut().push(Some(instance));

        let mut script_fs = archive(t[2]);
        let script = AnimScript::open(&mut script_fs).ok().flatten();
        let loaded = script.is_some();
        if let Some(script) = script {
            self.script = self.host.add_script(Arc::new(script));
        }
        let names = {
            let scene = self.scene.borrow();
            self.bones
                .iter()
                .map(|&b| {
                    String::from_utf8_lossy(scene.instance_name(id(b)).0.unwrap_or(b"-"))
                        .into_owned()
                })
                .collect::<Vec<_>>()
                .join(",")
        };
        let bones = self.bones_text();
        self.line(&format!(
            "\"op\":\"model\",\"n\":\"{count}\",\"names\":\"{names}\",\"script\":\"{}\",\"bones\":\"{bones}\"",
            u8::from(loaded)
        ));
    }

    fn play(&mut self, t: &[&str], chain: bool) {
        let script = self.script;
        let name = self.host.names.intern(t[1].as_bytes());
        let w: Vec<u32> = t[2..8].iter().map(|s| hex_word(s)).collect();
        let joint = if chain { self.bones[dec(t[8])] } else { -1 };
        let handle = self.with(|instance, services| {
            if chain {
                instance.play_chain(
                    script,
                    name,
                    f32::from_bits(w[0]),
                    w[1],
                    f32::from_bits(w[2]),
                    w[3],
                    w[4],
                    f32::from_bits(w[5]),
                    joint,
                    services,
                )
            } else {
                instance.play(
                    script,
                    name,
                    f32::from_bits(w[0]),
                    w[1],
                    f32::from_bits(w[2]),
                    w[3],
                    w[4],
                    f32::from_bits(w[5]),
                    0,
                    services,
                )
            }
        });
        let (n, o24) = {
            let instance = self.instance();
            (instance.list.len(), instance.offset24)
        };
        let r = if handle == -1 { -1 } else { 0 };
        self.line(&format!(
            "\"op\":\"{}\",\"r\":\"{r}\",\"ok\":\"{}\",\"n\":\"{n}\",\"o24\":\"{}\"",
            if chain { "chain" } else { "play" },
            u8::from(handle != -1),
            f(o24)
        ));
    }

    fn flush_events(&mut self) {
        for event in std::mem::take(&mut self.events) {
            match event {
                DeformableEvent::Keys {
                    animation,
                    context,
                    keys,
                    count,
                    ..
                } => {
                    let name = animation.map_or_else(
                        || "-".to_owned(),
                        |n| String::from_utf8_lossy(&n).into_owned(),
                    );
                    let times = keys.iter().map(|k| f(k.time)).collect::<Vec<_>>().join(",");
                    self.line(&format!(
                        "\"ev\":\"keys\",\"name\":\"{name}\",\"ctx\":\"{context:x}\",\"n\":\"{count}\",\"t\":\"{times}\""
                    ));
                }
                DeformableEvent::Loop {
                    transform, context, ..
                } => self.line(&format!(
                    "\"ev\":\"loop\",\"ctx\":\"{context:x}\",\"x\":\"{}\"",
                    fs(&transform)
                )),
            }
        }
    }

    fn walk(&mut self, t: &[&str]) {
        let dt = hex_float(t[1]);
        walk_deformable(
            &self.scene,
            &self.animation,
            &self.models,
            &mut self.host.names,
            &mut self.events,
            0,
            dt,
        );
        self.flush_events();
        let n = self.with(|instance, services| instance.list_count(services));
        let o24 = self.instance().offset24;
        let bones = self.bones_text();
        self.line(&format!(
            "\"op\":\"walk\",\"dt\":\"{}\",\"n\":\"{n}\",\"o24\":\"{}\",\"bones\":\"{bones}\"",
            f(dt),
            f(o24)
        ));
    }

    fn aim(&mut self, t: &[&str]) {
        let pos = hex_floats(t[4]);
        let orient = hex_floats(t[5]);
        let desc = AimDesc {
            root: self.bones[dec(t[1])],
            end: self.bones[dec(t[2])],
            flags: hex_word(t[3]),
            target_pos: core::array::from_fn(|i| pos[i]),
            target_orient: core::array::from_fn(|i| orient[i]),
            damping: hex_float(t[6]),
            locks: decs(t[7]).into_iter().map(|b| self.bones[b]).collect(),
        };
        let extra = hex_word(t[8]);
        let node = self.with(|instance, services| instance.start_aim(&desc, extra, services));
        let node = u32::try_from(node).ok();
        self.aims.push(node);
        let mut instance = self.instance();
        let n = instance.list.len();
        let detail = node
            .and_then(|node| instance.aim_mut(node))
            .map_or_else(String::new, |e| {
                format!(
                    ",\"joints\":\"{}\",\"f34\":\"{:x}\"",
                    e.joint_count,
                    e.flags34 & 3
                )
            });
        drop(instance);
        let r = if node.is_some() { 0 } else { -1 };
        self.line(&format!(
            "\"op\":\"aim\",\"r\":\"{r}\",\"n\":\"{n}\"{detail}"
        ));
    }

    fn run(&mut self, t: &[&str]) {
        match t[0] {
            "model" => self.model(t),
            "sink" | "nosink" => {
                self.instance().sink = if t[0] == "sink" { sink_token(0) } else { 0 };
                self.line(&format!("\"op\":\"{}\"", t[0]));
            }
            "play" => self.play(t, false),
            "chain" => self.play(t, true),
            "walk" => self.walk(t),
            "count" => {
                let n = self.with(|instance, services| instance.list_count(services));
                let o24 = self.instance().offset24;
                self.line(&format!(
                    "\"op\":\"count\",\"r\":\"0\",\"n\":\"{n}\",\"o24\":\"{}\"",
                    f(o24)
                ));
            }
            "destroyall" => {
                self.with(|instance, services| instance.destroy_all(services));
                let n = self.instance().list.len();
                self.line(&format!("\"op\":\"destroyall\",\"n\":\"{n}\""));
            }
            "aim" => self.aim(t),
            "target" => {
                if let Some(node) = self.aims[dec(t[1])] {
                    let pos = hex_floats(t[2]);
                    let orient = hex_floats(t[3]);
                    if let Some(event) = self.instance().aim_mut(node) {
                        event.set_targets(
                            core::array::from_fn(|i| pos[i]),
                            core::array::from_fn(|i| orient[i]),
                        );
                    }
                }
                self.line("\"op\":\"target\"");
            }
            "stopaim" => {
                let node = self.aims[dec(t[1])];
                let r = match node {
                    Some(node) => {
                        self.with(|instance, services| instance.stop_aim(node, services));
                        0
                    }
                    None => -1,
                };
                self.line(&format!("\"op\":\"stopaim\",\"r\":\"{r}\""));
            }
            other => panic!("deformable_anim: unknown operation {other}"),
        }
    }

    fn finish(mut self) -> Vec<String> {
        if !self.models.borrow().is_empty() {
            self.with(|instance, services| instance.destroy_all(services));
        }
        if self.root != -1 {
            self.host.destroy_instance(self.root);
        }
        self.out
    }
}

/// The suite's lines (`deformable_anim.jsonl`).
pub fn lines() -> Vec<String> {
    let mut out = Vec::new();
    for (index, line) in input_lines("deformable_anim.list").iter().enumerate() {
        let mut case = Case::new(index);
        for op in line.split(" ; ") {
            let t: Vec<&str> = op.split(' ').collect();
            case.run(&t);
        }
        out.extend(case.finish());
    }
    out
}
