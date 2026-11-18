//! `FLAppearance` (work package P4.5, `CppSource/surfacematerials/
//! FLAppearance.cpp`): the engine component of `surfacematerials.dll` that
//! makes the nine override materials the mesh renderer draws a face
//! group's material with, keeps the instance values the surface materials
//! read, publishes the engine to them, and draws the backdrop, the
//! starsphere, the blob shadows and the visible-geometry count.
//!
//! * [`services`]: what it calls, as [`AppearanceServices`].
//! * [`camera`]: `ICamera` as [`Camera`] and the camera wrapper
//!   `FCameraBlob` as [`CameraBlob`].
//! * [`shadow`]: the fading and fixed blob shadows.
//!
//! The reference keeps the overrides, the instance values, the directional
//! light, the shadow quad scale and the engine in process statics and is a
//! COM aggregate with an engine face; here [`Appearance`] is a value that
//! owns its overrides, light, wrapper and scale, the services own the
//! instance values and the engine, and the aggregate's plumbing
//! (`QueryInterface`, the reference counts, the engine face's empty slots)
//! is gone (divergence D318).

// The reference's x87 comparisons send unordered operands (NaN) to one
// side; the port spells each outcome as the comparison that is true
// exactly when the reference takes that side, which needs negated
// comparisons.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

pub mod camera;
pub mod services;
pub mod shadow;

pub use camera::{BoundCamera, Camera, CameraBlob, MeshView};
pub use services::{AppearanceServices, Face};
pub use shadow::{FADING, FIXED, ShadowFlags, ShadowInstance};

use neuron_core::diag::{self, DumpFlags, Severity};
use neuron_core::hash::name_crc32;

use crate::render::material::{GR_GENERIC, GR_OK, InstanceValue, MaterialId};
use crate::render::mesh::Instance;

/// The file the reference's dumps name (`kFlmFile`).
pub const FILE: &str = "SurfaceMaterials\\SurfaceMaterials.cpp";

/// The override materials, by index (`kOverrideNames`): the names
/// `FindOrCreate` is given as name and type.
pub const OVERRIDE_NAMES: [&str; 9] = [
    "ColorizeMaterial",
    "WireMaterial",
    "WireMaterial2",
    "CloakMaterial",
    "EnvMapMaterial",
    "BackdropMaterial",
    "StarsphereMaterial",
    "ShadowMaterial",
    "VisibleCounterMaterial",
];

/// The index of `EnvMapMaterial`.
pub const OVERRIDE_ENVMAP: usize = 4;
/// The index of `BackdropMaterial`.
pub const OVERRIDE_BACKDROP: usize = 5;
/// The index of `StarsphereMaterial`.
pub const OVERRIDE_STARSPHERE: usize = 6;
/// The index of `VisibleCounterMaterial`.
pub const OVERRIDE_VISIBLE_COUNTER: usize = 8;

/// The global material record `Initialize` sets to 1 (the HUD animation's).
pub const GLOBAL_RECORD: u32 = 4;

/// `D3DRS_AMBIENT`.
const D3DRS_AMBIENT: u32 = 139;
/// `RenderInstance`'s flag for the starsphere and the count.
const RENDER_NO_DEPTH: u32 = 0x80;

/// The light `SetDirectionalLight` stores (`G_Slot6First`, `G_Slot6Second`,
/// `G_Slot6Flag`). Nothing in `surfacematerials` reads it.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct DirectionalLight {
    /// The flag byte.
    pub enabled: u8,
    /// The first vector (a direction).
    pub direction: [f32; 3],
    /// The second vector (a colour).
    pub colour: [f32; 3],
}

impl Default for DirectionalLight {
    /// The C runtime's initial values.
    fn default() -> Self {
        DirectionalLight {
            enabled: 0,
            direction: [0.0, -1.0, 0.0],
            colour: [1.0, 1.0, 1.0],
        }
    }
}

/// The interfaces `Initialize` acquired.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Faces {
    /// `IID_IMaterialLibrary`.
    pub material_library: bool,
    /// `IID_IRenderPipeline`.
    pub pipeline: bool,
    /// `IID_IVertexBufferManager`.
    pub vertex_buffers: bool,
    /// `IID_ITextureLibrary`.
    pub textures: bool,
    /// `IID_IRenderer`.
    pub renderer: bool,
}

/// `FFLAppearance`.
#[derive(Clone, Debug, PartialEq)]
pub struct Appearance {
    overrides: [Option<MaterialId>; 9],
    faces: Faces,
    camera: Option<CameraBlob>,
    light: DirectionalLight,
    shadow_scale: f32,
}

impl Appearance {
    /// `FlmCreateAppearanceInstance` and `FinishCreate`: the appearance,
    /// once the outer component answers `IID_IEngine` and a system is
    /// named.
    ///
    /// # Errors
    ///
    /// `GR_GENERIC`, after a dump, for either failure.
    pub fn create<S: AppearanceServices + ?Sized>(services: &mut S) -> Result<Appearance, i32> {
        if !services.query(Face::Engine) {
            Self::fail(services, 0xCA, "IID_IEngine");
            return Err(GR_GENERIC);
        }
        if !services.has_system() {
            Self::fail(services, 0xD1, "IID_ISystem");
            return Err(GR_GENERIC);
        }
        Ok(Appearance {
            overrides: [None; 9],
            faces: Faces::default(),
            camera: None,
            light: DirectionalLight::default(),
            shadow_scale: 1.0,
        })
    }

    fn fail<S: AppearanceServices + ?Sized>(services: &mut S, line: u32, iid: &str) {
        let message =
            format!("FLAppearance: Initialize: Unable to acquire {iid}, unable to continue.");
        services.dump(
            DumpFlags::ERROR.bits().cast_signed(),
            &diag::general_line(FILE, line, Severity::Error, &message),
        );
    }

    /// The engine face's `Initialize` (`EngineInitialize`): acquires the
    /// material library, the pipeline, the vertex-buffer manager, the
    /// texture library and the renderer in turn, stopping at the first that
    /// fails; then publishes the engine, creates the override materials,
    /// sets global record 4 and makes the camera wrapper.
    pub fn initialize<S: AppearanceServices + ?Sized>(&mut self, services: &mut S) -> i32 {
        if !services.has_system() {
            return GR_GENERIC;
        }
        let steps: [(Face, u32, &str); 5] = [
            (Face::MaterialLibrary, 0x118, "IID_IMaterialLibrary"),
            (Face::Pipeline, 0x11E, "IID_IRenderPipeline"),
            (Face::VertexBuffers, 0x125, "IID_IVertexBufferManager"),
            (Face::Textures, 300, "IID_ITextureLibrary"),
            (Face::Renderer, 0x133, "IID_IRenderer"),
        ];
        for (face, line, iid) in steps {
            if !services.query(face) {
                Self::fail(services, line, iid);
                return GR_GENERIC;
            }
            match face {
                Face::MaterialLibrary => self.faces.material_library = true,
                Face::Pipeline => self.faces.pipeline = true,
                Face::VertexBuffers => self.faces.vertex_buffers = true,
                Face::Textures => self.faces.textures = true,
                Face::Renderer => self.faces.renderer = true,
                Face::Engine => {}
            }
        }
        services.publish_engine();
        for (index, name) in OVERRIDE_NAMES.iter().enumerate() {
            self.create_override(services, index, name);
        }
        services.set_global_record(GLOBAL_RECORD, 1);
        self.camera = Some(CameraBlob::new());
        GR_OK
    }

    /// `CreateOneOverride`: the material is kept whatever `FindOrCreate`
    /// returns; on success it leaves the library's master map (the
    /// appearance holds the reference). An override of an earlier
    /// `Initialize` is dropped without a release, as in the reference.
    fn create_override<S: AppearanceServices + ?Sized>(
        &mut self,
        services: &mut S,
        index: usize,
        name: &str,
    ) {
        let (result, material) = services.find_or_create(name, name);
        self.overrides[index] = material;
        if result != GR_OK {
            let text = format!(
                "{FILE}({}) : ERROR:General:SurfaceMaterials: couldnt create override material {name}",
                0x49 + index
            );
            services.dump(DumpFlags::ERROR.bits().cast_signed(), &text);
            return;
        }
        services.remove_material(name, name_crc32(name.as_bytes()));
    }

    /// The interfaces `Initialize` acquired.
    #[must_use]
    pub fn faces(&self) -> Faces {
        self.faces
    }

    /// The camera wrapper, made by a successful `Initialize`.
    #[must_use]
    pub fn camera(&self) -> Option<&CameraBlob> {
        self.camera.as_ref()
    }

    /// `G_ShadowQuadScale`: the quad scale the last fading shadow left.
    #[must_use]
    pub fn shadow_scale(&self) -> f32 {
        self.shadow_scale
    }

    // --- the instance values ---------------------------------------------------------------

    /// `SetInstanceValue`: -1 for id -1.
    pub fn set_instance_value<S: AppearanceServices + ?Sized>(
        &self,
        services: &mut S,
        id: i32,
        value: InstanceValue,
    ) -> i32 {
        services.instance_values().set(id, value)
    }

    /// `GetInstanceValue`.
    ///
    /// # Errors
    ///
    /// -1 for id -1 or a miss.
    pub fn instance_value<S: AppearanceServices + ?Sized>(
        &self,
        services: &mut S,
        id: i32,
    ) -> Result<InstanceValue, i32> {
        services.instance_values().get(id).cloned().ok_or(-1)
    }

    /// `RemoveInstanceValue`: -1 for id -1, else 0.
    pub fn remove_instance_value<S: AppearanceServices + ?Sized>(
        &self,
        services: &mut S,
        id: i32,
    ) -> i32 {
        services.instance_values().remove(id)
    }

    /// The engine face's `DestroyInstance`: the instance's value is removed
    /// (the instance handle's low 32 bits are the id).
    pub fn destroy_instance<S: AppearanceServices + ?Sized>(
        &self,
        services: &mut S,
        instance: Instance,
    ) -> i32 {
        self.remove_instance_value(services, (instance as u32).cast_signed())
    }

    // --- the light and the overrides -----------------------------------------------------------

    /// `SetDirectionalLight`: enabled, then both vectors.
    pub fn set_directional_light(&mut self, direction: [f32; 3], colour: [f32; 3]) -> i32 {
        self.light = DirectionalLight {
            enabled: 1,
            direction,
            colour,
        };
        GR_OK
    }

    /// `SetDirectionalLightEnabled`.
    pub fn set_directional_light_enabled(&mut self, flag: u8) -> i32 {
        self.light.enabled = flag;
        GR_OK
    }

    /// The light the two calls above store.
    #[must_use]
    pub fn light(&self) -> DirectionalLight {
        self.light
    }

    /// `GetOverrideMaterial`: the override at `index`.
    ///
    /// # Errors
    ///
    /// -1 for a null override, or an index past the nine, which the
    /// reference reads past its array (divergence D321).
    pub fn override_material(&self, index: u32) -> Result<MaterialId, i32> {
        self.overrides
            .get(index as usize)
            .copied()
            .flatten()
            .ok_or(-1)
    }

    // --- the draws ----------------------------------------------------------------------------

    /// `CallSetPerspective`: the camera's far, near, aspect and field of
    /// view, read in that order.
    fn set_perspective<S: AppearanceServices + ?Sized>(services: &mut S, camera: &mut dyn Camera) {
        let far = camera.z_far();
        let near = camera.z_near();
        let aspect = camera.aspect();
        let fov = camera.fov_y_deg();
        services.set_perspective(fov, aspect, near, far);
    }

    /// `DrawBackdrop`: `instance` drawn with `BackdropMaterial` through the
    /// wrapper under the fixed depth range and its own perspective, then
    /// the camera's perspective again. Always `GR_OK`.
    pub fn draw_backdrop<S: AppearanceServices + ?Sized>(
        &mut self,
        services: &mut S,
        camera: &mut dyn Camera,
        instance: Instance,
    ) -> i32 {
        if !self.faces.renderer {
            return GR_OK;
        }
        let extra = self.overrides[OVERRIDE_BACKDROP];
        let Some(blob) = self.camera.as_mut() else {
            return GR_OK;
        };
        blob.fixed_depth = true;
        blob.own_transform = false;
        let mut bound = BoundCamera {
            blob,
            inner: camera,
        };
        Self::set_perspective(services, &mut bound);
        bound.fill_local_axes();
        // The reference's own-transform branch here never runs: the bind
        // has just cleared the flag.
        services.render_instance(&mut bound, instance, 1.0, 0, extra);
        if bound.blob.fixed_depth {
            bound.blob.fixed_depth = false;
            Self::set_perspective(services, &mut bound);
        }
        bound.blob.own_transform = false;
        GR_OK
    }

    /// `DrawStarsphere`: `instance` drawn with `StarsphereMaterial` through
    /// the wrapper at the camera's orientation without its position, with
    /// the ambient light white and restored afterwards. Always `GR_OK`.
    pub fn draw_starsphere<S: AppearanceServices + ?Sized>(
        &mut self,
        services: &mut S,
        camera: &mut dyn Camera,
        instance: Instance,
    ) -> i32 {
        if !self.faces.renderer {
            return GR_OK;
        }
        let saved = services.render_state(D3DRS_AMBIENT);
        services.set_render_state(D3DRS_AMBIENT, u32::MAX);
        let extra = self.overrides[OVERRIDE_STARSPHERE];
        if let Some(blob) = self.camera.as_mut() {
            blob.fixed_depth = false;
            blob.own_transform = true;
            let orientation = camera.orientation();
            blob.set_orientation_and_translation(orientation, [0.0; 3]);
            blob.view = blob.inverse_transform();
            let mut bound = BoundCamera {
                blob,
                inner: camera,
            };
            services.render_instance(&mut bound, instance, 1.0, RENDER_NO_DEPTH, extra);
            if bound.blob.fixed_depth {
                bound.blob.fixed_depth = false;
                Self::set_perspective(services, &mut bound);
            }
            bound.blob.own_transform = false;
        }
        services.set_render_state(D3DRS_AMBIENT, saved);
        GR_OK
    }

    /// `DrawFadingShadows`. Always `GR_OK`.
    pub fn draw_fading_shadows<S: AppearanceServices + ?Sized>(
        &mut self,
        services: &mut S,
        camera: &mut dyn Camera,
        shadows: &[ShadowInstance],
    ) -> i32 {
        self.draw_shadows(services, camera, shadows, FADING);
        GR_OK
    }

    /// `DrawFixedShadows`. Always `GR_OK`.
    pub fn draw_fixed_shadows<S: AppearanceServices + ?Sized>(
        &mut self,
        services: &mut S,
        camera: &mut dyn Camera,
        shadows: &[ShadowInstance],
    ) -> i32 {
        self.draw_shadows(services, camera, shadows, FIXED);
        GR_OK
    }

    /// `CountVisibleGeometry`: `instance` drawn with
    /// `VisibleCounterMaterial` between a reset and a copy of the
    /// counters; `None`, with nothing called, without that override.
    pub fn count_visible_geometry<S: AppearanceServices + ?Sized>(
        &mut self,
        services: &mut S,
        camera: &mut dyn Camera,
        instance: Instance,
    ) -> Option<[u32; 3]> {
        let visible = self
            .override_material(OVERRIDE_VISIBLE_COUNTER as u32)
            .ok()?;
        services.reset_visible();
        services.render_instance(camera, instance, 1.0, RENDER_NO_DEPTH, Some(visible));
        Some(services.visible())
    }

    /// The last release (`ReleaseOverrides`): each override released, in
    /// order.
    pub fn release<S: AppearanceServices + ?Sized>(self, services: &mut S) {
        for material in self.overrides.into_iter().flatten() {
            services.release_material(material);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::render::material::InstanceValues;

    #[test]
    fn instance_values_keep_words_and_names_apart() {
        let mut values = InstanceValues::new();
        assert_eq!(values.set(-1, InstanceValue::Word(1)), -1);
        assert_eq!(values.set(3, InstanceValue::Word(0.5f32.to_bits())), 0);
        assert_eq!(
            values.set(4, InstanceValue::Name(b"envmapbasic".to_vec())),
            0
        );
        assert_eq!(values.word(3), Some(0.5f32.to_bits()));
        assert_eq!(values.name(3), None);
        assert_eq!(values.word(4), None);
        assert_eq!(values.name(4).as_deref(), Some(&b"envmapbasic"[..]));
        assert_eq!(values.get(-1), None);
        assert_eq!(values.remove(-1), -1);
        assert_eq!(values.remove(9), 0);
        assert_eq!(values.remove(3), 0);
        assert_eq!(values.len(), 1);
    }

    #[test]
    fn the_wrapper_inverts_its_transform() {
        let mut blob = CameraBlob::new();
        let identity = blob.inverse_transform();
        assert_eq!(
            &identity[..9],
            &[1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]
        );
        blob.set_orientation_and_translation(
            [0.0, 1.0, 0.0, -1.0, 0.0, 0.0, 0.0, 0.0, 1.0],
            [2.0, 3.0, 4.0],
        );
        let inverse = blob.inverse_transform();
        assert_eq!(
            &inverse[..9],
            &[0.0, -1.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0]
        );
        // transpose(R) * t = (-3, 2, 4), negated.
        assert_eq!(&inverse[9..], &[3.0, -2.0, -4.0]);
    }

    #[test]
    fn light_starts_as_the_runtime_loads_it() {
        let light = DirectionalLight::default();
        assert_eq!(light.enabled, 0);
        assert_eq!(light.direction, [0.0, -1.0, 0.0]);
        assert_eq!(light.colour, [1.0, 1.0, 1.0]);
    }
}
