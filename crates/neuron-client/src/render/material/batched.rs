//! `BatchedMaterial` (`FBatchedMaterial` and its `IBatchedMaterial` face in
//! `CppSource/Shading/Material.cpp`): a material that holds an inner
//! material of another type, draws with it, and submits through the
//! material batcher into the bin it names.
//!
//! The reference aggregates the inner material (its `QueryInterface`,
//! `AddRef` and `Release` go to the batched material) and queues the inner
//! material in the batcher, which asks it for the batched face again when
//! it draws. Here the batched material owns its inner material and is
//! queued itself: [`Material::render_flags`] answers with the inner
//! material's flags (what the batcher reads from the queued material) and
//! the batched-face calls draw with the inner material. Its `Submit` is
//! [`super::MaterialBatcher::submit_material`], which the batched material
//! cannot reach from inside the batcher's library (divergence D311).

use neuron_core::diag::{self, DumpFlags, Severity};
use neuron_core::material_data::{LeafWrite, PropertyValue};
use neuron_core::math::{Transform, Vector, transform_xform};
use neuron_core::vfs::{FileSystem, FsDesc};

use super::batcher::{DrawArgs, SubmitContext};
use super::instance::{BatchRoute, Material, MaterialFactory};
use super::{GR_GENERIC, GR_OK};

/// The bin of a batched material that has none yet.
pub const NO_BIN: u32 = 0xFFFF_FFFF;

/// A batched material.
pub struct BatchedMaterial<S: ?Sized> {
    /// `Field8`: the batch bin.
    bin: u32,
    /// The inner material (`Inner`, held through `InnerController`).
    inner: Option<Box<dyn Material<S>>>,
}

impl<S: ?Sized> std::fmt::Debug for BatchedMaterial<S> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("BatchedMaterial")
            .field("bin", &self.bin)
            .field("inner", &self.inner.as_ref().map(|m| m.class_name()))
            .finish()
    }
}

impl<S: ?Sized> Default for BatchedMaterial<S> {
    fn default() -> Self {
        Self::new()
    }
}

impl<S: ?Sized> BatchedMaterial<S> {
    /// The constructor and `InitializeFromDescriptor`: no inner material,
    /// no bin.
    #[must_use]
    pub fn new() -> BatchedMaterial<S> {
        BatchedMaterial {
            bin: NO_BIN,
            inner: None,
        }
    }

    /// A batched material around `inner` (what `CreateInnerMaterial` leaves
    /// when the factory makes it).
    #[must_use]
    pub fn with_inner(inner: Box<dyn Material<S>>) -> BatchedMaterial<S> {
        BatchedMaterial {
            bin: NO_BIN,
            inner: Some(inner),
        }
    }

    /// `IBatchedMaterial::GetBatchBin`.
    #[must_use]
    pub fn batch_bin(&self) -> u32 {
        self.bin
    }

    /// `IBatchedMaterial::SetBatchBin`.
    pub fn set_batch_bin(&mut self, bin: u32) {
        self.bin = bin;
    }

    /// The inner material.
    #[must_use]
    pub fn inner(&self) -> Option<&dyn Material<S>> {
        self.inner.as_deref()
    }

    /// The inner material, mutably.
    pub fn inner_mut(&mut self) -> Option<&mut (dyn Material<S> + 'static)> {
        self.inner.as_deref_mut()
    }
}

/// `ReadNamedBytes(fs, "Bin", &Field8, 4)`.
fn read_bin(fs: &dyn FileSystem) -> Option<u32> {
    let mut file = fs.open_file(&FsDesc::read(b"Bin")).ok()?;
    let mut buf = [0u8; 4];
    match file.read(&mut buf) {
        Ok(4) => Some(u32::from_le_bytes(buf)),
        _ => None,
    }
}

impl<S: ?Sized> Material<S> for BatchedMaterial<S> {
    /// `Initialize`: the inner material's (the reference first asks the
    /// aggregate for the batcher; the batcher's library is the only one
    /// that makes batched materials here).
    fn initialize(&mut self, services: &mut S) -> i32 {
        match self.inner.as_mut() {
            Some(inner) => inner.initialize(services),
            None => GR_GENERIC,
        }
    }

    fn set_name(&mut self, name: &[u8]) -> i32 {
        match self.inner.as_mut() {
            Some(inner) => inner.set_name(name),
            None => GR_GENERIC,
        }
    }

    /// `LoadFromFileSystem`: the bin from the `Bin` leaf (none when it
    /// cannot be read), then the inner material.
    fn load(&mut self, services: &mut S, fs: &mut dyn FileSystem) -> i32 {
        let Some(inner) = self.inner.as_mut() else {
            return GR_GENERIC;
        };
        self.bin = read_bin(&*fs).unwrap_or(NO_BIN);
        inner.load(services, fs)
    }

    /// `ApplyVerify`: the inner material's; the first success without a
    /// bin takes bit 1 of the inner material's render flags as the bin.
    fn apply_verify(&mut self, services: &mut S) -> i32 {
        let Some(inner) = self.inner.as_mut() else {
            return GR_GENERIC;
        };
        let result = inner.apply_verify(services);
        if self.bin == NO_BIN && result >= 0 {
            let flags = inner.flags(services).unwrap_or(0);
            self.bin = ((flags & 0xFF) >> 1) & 1;
        }
        result
    }

    fn resolve_textures(&mut self, services: &mut S, delta: f32) -> i32 {
        match self.inner.as_mut() {
            Some(inner) => inner.resolve_textures(services, delta),
            None => GR_GENERIC,
        }
    }

    /// The flags of the inner material, which the reference queues.
    fn render_flags(&mut self, services: &mut S) -> u32 {
        self.inner
            .as_mut()
            .map_or(0, |inner| inner.render_flags(services))
    }

    /// `GetRenderFlags`: the inner material's with 0x10.
    fn flags(&mut self, services: &mut S) -> Result<u32, i32> {
        let inner = self.inner.as_mut().ok_or(GR_GENERIC)?;
        inner
            .flags(services)
            .map(|f| f | 0x10)
            .map_err(|_| GR_GENERIC)
    }

    fn is_batched(&self) -> bool {
        true
    }

    /// `CreateInnerMaterial`: the previous inner material released, then
    /// one of `type_name` made with `factory`. A type the factory does not
    /// make is reported and leaves none; the result is success either way.
    fn create_inner_material(
        &mut self,
        services: &mut S,
        factory: &mut dyn MaterialFactory<S>,
        type_name: &[u8],
    ) -> i32 {
        if let Some(mut old) = self.inner.take() {
            old.destroy(services);
        }
        match factory.create(services, type_name) {
            Ok(inner) => self.inner = Some(inner),
            Err(_) => {
                let message = format!(
                    "Batched material references invalid super type ({})\n",
                    String::from_utf8_lossy(type_name)
                );
                diag::dump(
                    DumpFlags::NOTICE,
                    &diag::general_line(
                        "Shading\\Materials\\BatchedMaterial.cpp",
                        0x159,
                        Severity::Notice,
                        &message,
                    ),
                );
            }
        }
        GR_OK
    }

    /// Draws nothing: a batched material submits through
    /// [`super::MaterialBatcher::submit_material`] (divergence D311).
    fn submit(&mut self, services: &mut S, context: &SubmitContext<'_>, args: &DrawArgs) -> i32 {
        let _ = (services, context, args);
        GR_GENERIC
    }

    fn bind(&mut self, services: &mut S, context: Option<&SubmitContext<'_>>) -> i32 {
        match self.inner.as_mut() {
            Some(inner) => inner.bind(services, context),
            None => GR_GENERIC,
        }
    }

    fn teardown(&mut self, services: &mut S, context: Option<&SubmitContext<'_>>) -> i32 {
        match self.inner.as_mut() {
            Some(inner) => inner.teardown(services, context),
            None => GR_GENERIC,
        }
    }

    /// `IBatchedMaterial::Bind`: the inner material's `Bind`.
    fn batch_bind(&mut self, services: &mut S, context: &SubmitContext<'_>) -> i32 {
        self.bind(services, Some(context))
    }

    /// `IBatchedMaterial::Teardown`.
    fn batch_teardown(&mut self, services: &mut S, context: &SubmitContext<'_>) -> i32 {
        self.teardown(services, Some(context))
    }

    /// `IBatchedMaterial::Submit`: the inner material's `Submit`.
    fn batch_submit(
        &mut self,
        services: &mut S,
        context: &SubmitContext<'_>,
        args: &DrawArgs,
    ) -> i32 {
        match self.inner.as_mut() {
            Some(inner) => inner.submit(services, context, args),
            None => GR_GENERIC,
        }
    }

    fn name(&self) -> Result<Vec<u8>, i32> {
        self.inner.as_ref().ok_or(GR_GENERIC)?.name()
    }

    fn class_name(&self) -> &'static str {
        self.inner
            .as_ref()
            .map_or("BatchedMaterial", |inner| inner.class_name())
    }

    fn pass_count(&self) -> Result<i32, i32> {
        self.inner.as_ref().ok_or(GR_GENERIC)?.pass_count()
    }

    fn parameter_count(&self) -> Result<u32, i32> {
        self.inner.as_ref().ok_or(GR_GENERIC)?.parameter_count()
    }

    fn parameter(&self, index: u32) -> Result<PropertyValue, i32> {
        self.inner.as_ref().ok_or(GR_GENERIC)?.parameter(index)
    }

    fn set_parameter(&mut self, services: &mut S, index: u32, value: &PropertyValue) -> i32 {
        match self.inner.as_mut() {
            Some(inner) => inner.set_parameter(services, index, value),
            None => GR_GENERIC,
        }
    }

    /// `SaveProperties`: the `Bin` leaf, then the inner material's leaves.
    /// The reference forgets the bin when its write fails; writing is the
    /// caller's here (divergence D311).
    fn save(&mut self, fs: &dyn FileSystem) -> Result<Vec<LeafWrite>, i32> {
        let inner = self.inner.as_mut().ok_or(GR_GENERIC)?;
        let mut writes = vec![LeafWrite {
            name: b"Bin".to_vec(),
            bytes: self.bin.to_le_bytes().to_vec(),
        }];
        writes.extend(inner.save(fs)?);
        Ok(writes)
    }

    /// `Submit` up to the batcher: the bin, and with flag 0x40 the depth of
    /// the world origin in view space (flag 0x20 added).
    fn batch_route(
        &mut self,
        context: &SubmitContext<'_>,
        args: &DrawArgs,
    ) -> Option<Result<BatchRoute, i32>> {
        if self.inner.is_none() {
            return Some(Err(GR_GENERIC));
        }
        let mut flags = args.flags;
        let mut depth_bits = 0;
        if flags & 0x40 != 0 {
            flags |= 0x20;
            let view = Transform::from_words(*context.view);
            let w = context.world;
            let at = transform_xform(&view, Vector::new(w[9], w[10], w[11]));
            depth_bits = (-at.z).to_bits();
        }
        Some(Ok(BatchRoute {
            bin: self.bin,
            flags,
            depth_bits,
        }))
    }

    /// The destructor: the inner material released.
    fn destroy(&mut self, services: &mut S) {
        if let Some(mut inner) = self.inner.take() {
            inner.destroy(services);
        }
    }
}
