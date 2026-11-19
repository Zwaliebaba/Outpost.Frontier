//! `FLAppearance`'s blob shadows over the shipped data (`outdir/data`,
//! decision O15): `FX/efx.txm` is loaded into the texture library on the
//! device model, as the client loads it, and a fading and a fixed shadow
//! are drawn with its `efxshadow` texture. Nothing is written or compared
//! with the oracle (`material_appearance` does that on synthetic input);
//! the test checks that the shipped library has the texture the shadows
//! name, that its first frame is a device texture, that each shadow is one
//! quad of six vertices, and that the record's reference is given back.

use std::path::Path;

use neuron_client::render::appearance::{
    Appearance, AppearanceServices, Camera, Face, ShadowInstance,
};
use neuron_client::render::ff::defaults::Defaults;
use neuron_client::render::ff::device::{Device, RecordingBackend};
use neuron_client::render::material::{
    InstanceValues, MaterialId, Materials, VbListing, VertexLock,
};
use neuron_client::render::mesh::{Archetype, Instance};
use neuron_client::render::texture::library::RecordId;
use neuron_client::render::texture::{
    Abilities, DevicePipeline, FormatClasses, FrameOut, HandleTable, LibraryConfig, TextureLibrary,
};
use neuron_core::vfs::{FileSystem, FsDesc, Host, UtfFileSystem};

struct Data {
    device: Device<RecordingBackend>,
    handles: HandleTable,
    classes: FormatClasses,
    library: TextureLibrary,
    values: InstanceValues,
    arena: Materials<()>,
    found: Vec<RecordId>,
    released: usize,
    draws: Vec<u32>,
    dumps: Vec<String>,
}

/// A material for the override ids.
struct Dummy;

impl neuron_client::render::material::Material<()> for Dummy {
    fn initialize(&mut self, (): &mut ()) -> i32 {
        0
    }
    fn set_name(&mut self, _: &[u8]) -> i32 {
        0
    }
    fn load(&mut self, (): &mut (), _: &mut dyn FileSystem) -> i32 {
        0
    }
    fn apply_verify(&mut self, (): &mut ()) -> i32 {
        0
    }
    fn resolve_textures(&mut self, (): &mut (), _: f32) -> i32 {
        0
    }
    fn render_flags(&mut self, (): &mut ()) -> u32 {
        0
    }
    fn submit(
        &mut self,
        (): &mut (),
        _: &neuron_client::render::material::SubmitContext<'_>,
        _: &neuron_client::render::material::DrawArgs,
    ) -> i32 {
        0
    }
}

impl AppearanceServices for Data {
    type TextureRecord = RecordId;

    fn query(&mut self, _: Face) -> bool {
        true
    }
    fn has_system(&mut self) -> bool {
        true
    }
    fn publish_engine(&mut self) {}
    fn find_or_create(&mut self, _: &str, _: &str) -> (i32, Option<MaterialId>) {
        (0, Some(self.arena.insert(Box::new(Dummy), 1)))
    }
    fn remove_material(&mut self, _: &str, _: u32) {}
    fn set_global_record(&mut self, _: u32, _: u32) {}
    fn release_material(&mut self, _: MaterialId) {}
    fn instance_values(&mut self) -> &mut InstanceValues {
        &mut self.values
    }
    fn device_ability(&mut self, _: u32) -> Option<u32> {
        Some(1)
    }
    fn set_perspective(&mut self, _: f32, _: f32, _: f32, _: f32) {}
    fn render_state(&mut self, _: u32) -> u32 {
        0
    }
    fn set_render_state(&mut self, _: u32, _: u32) {}
    fn set_modelview(&mut self, _: &[f32; 12]) {}
    fn set_stage_state(&mut self, _: u32, _: u32, _: u32) {}
    fn set_stage_texture(&mut self, _: u32, handle: u32) {
        assert!(
            self.handles.get(handle).is_some(),
            "efxshadow's frame is no device texture"
        );
    }
    fn draw_vb(&mut self, _: u32, _: u32, _: u32, count: u32) {
        self.draws.push(count);
    }
    fn find_texture(&mut self, name: &str, crc: u32) -> Result<RecordId, i32> {
        assert_eq!(crc, neuron_core::hash::name_crc32(name.as_bytes()));
        let record = self.library.find_by_crc(crc, true)?.ok_or(-1)?;
        self.found.push(record);
        Ok(record)
    }
    fn record_frame(&mut self, record: &RecordId, frame: i32) -> Option<FrameOut> {
        let (result, out) = self.library.texture_frame(Some(*record), frame);
        (result >= 0).then_some(out)
    }
    fn release_texture(&mut self, record: RecordId) {
        let (library, mut pipeline) = (
            &mut self.library,
            DevicePipeline {
                device: &mut self.device,
                handles: &mut self.handles,
                classes: &self.classes,
                abilities: Abilities::default(),
            },
        );
        assert_eq!(library.release_record(Some(record), &mut pipeline), 0);
        self.released += 1;
    }
    fn lock_vertex_memory(&mut self, fvf: u32, count: u32) -> Result<VertexLock, i32> {
        Ok(VertexLock {
            listing: VbListing {
                vertex_buffer: 1,
                fvf,
                base_vertex: 0,
            },
            bytes: vec![0; count as usize * 24],
            offset: 0,
        })
    }
    fn unlock_vertex_memory(&mut self, _: VertexLock) {}
    fn instance_transform(&mut self, _: Instance) -> [f32; 12] {
        [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 4.0, 2.5, -3.0]
    }
    fn instance_archetype(&mut self, _: Instance) -> Archetype {
        -1
    }
    fn release_handle(&mut self, _: Archetype) {}
    fn bounds(&mut self, _: Archetype, _: f32) -> (i32, Option<[f32; 6]>) {
        (-1, None)
    }
    fn render_instance(
        &mut self,
        _: &mut dyn Camera,
        _: Instance,
        _: f32,
        _: u32,
        _: Option<MaterialId>,
    ) {
    }
    fn reset_visible(&mut self) {}
    fn visible(&mut self) -> [u32; 3] {
        [0; 3]
    }
    fn dump(&mut self, _: i32, text: &str) {
        self.dumps.push(text.to_owned());
    }
}

struct StillCamera;

impl Camera for StillCamera {
    fn position(&mut self) -> [f32; 3] {
        [0.0; 3]
    }
    fn orientation(&mut self) -> [f32; 9] {
        [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]
    }
    fn viewport(&mut self) -> [i32; 4] {
        [0, 0, 640, 480]
    }
    fn half_fov_x_deg(&mut self) -> f32 {
        45.0
    }
    fn fov_y_deg(&mut self) -> f32 {
        40.0
    }
    fn z_near(&mut self) -> f32 {
        1.0
    }
    fn z_far(&mut self) -> f32 {
        1000.0
    }
    fn aspect(&mut self) -> f32 {
        1.333_333_4
    }
    fn x_scale_over_near(&mut self) -> f64 {
        1.0
    }
    fn y_scale_over_near(&mut self) -> f64 {
        1.0
    }
    fn project(&mut self, _: [f32; 3], _: &mut [f32; 3]) -> i32 {
        1
    }
    fn unproject(&mut self, _: &mut [f32; 3], _: f32, _: f32) {}
    fn classify_sphere(&mut self, _: [f32; 3], _: f32) -> i32 {
        4
    }
    fn view_transform(&mut self) -> [f32; 12] {
        [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0]
    }
}

#[test]
fn shipped_efxshadow_draws_the_blob_shadows() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../../outdir/data");
    if !root.join("FX/efx.txm").is_file() {
        println!("outdir/data has no FX/efx.txm: nothing to draw");
        return;
    }
    let host = Host::new(&root);
    let mut data = Data {
        device: Device::new(Defaults::builtin(), 640, 480, RecordingBackend::default()),
        handles: HandleTable::new(),
        classes: FormatClasses::builtin(),
        library: TextureLibrary::new(LibraryConfig::default()),
        values: InstanceValues::new(),
        arena: Materials::new(),
        found: Vec::new(),
        released: 0,
        draws: Vec::new(),
        dumps: Vec::new(),
    };
    let mut fs = UtfFileSystem::open(
        &host,
        &FsDesc::read(b"FX/efx.txm").with_implementation(b"UTF"),
    )
    .expect("FX/efx.txm opens");
    let (library, mut pipeline) = (
        &mut data.library,
        DevicePipeline {
            device: &mut data.device,
            handles: &mut data.handles,
            classes: &data.classes,
            abilities: Abilities::default(),
        },
    );
    let load = library.load_group(&mut fs, &mut pipeline, true);
    assert_eq!(load.result, 0);
    let group = load.group.expect("a texture group");

    let mut appearance = Appearance::create(&mut data).expect("created");
    assert_eq!(appearance.initialize(&mut data), 0);
    let shadows = [ShadowInstance {
        instance: 7,
        radius: 1.5,
    }];
    appearance.draw_fading_shadows(&mut data, &mut StillCamera, &shadows);
    appearance.draw_fixed_shadows(&mut data, &mut StillCamera, &shadows);
    assert!(data.dumps.is_empty(), "{:?}", data.dumps);
    assert_eq!(data.draws, [6, 6]);
    assert_eq!(data.found.len(), 2);
    assert_eq!(data.released, 2);
    appearance.release(&mut data);

    let (library, mut pipeline) = (
        &mut data.library,
        DevicePipeline {
            device: &mut data.device,
            handles: &mut data.handles,
            classes: &data.classes,
            abilities: Abilities::default(),
        },
    );
    library.garbage_collect(group, &mut pipeline);
    assert_eq!(
        data.handles.len(),
        0,
        "textures left after the group was collected"
    );
}
