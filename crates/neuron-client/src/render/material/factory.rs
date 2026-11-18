//! The material factories `Shading.dll` registers (`G_Specs` in
//! `CppSource/Shading/Shading.cpp`): `BatchedMaterial` and the 22 kinds of
//! `kMaterialTypes`, each by its exact name. The component manager also
//! finds `Shading`'s other components (`TextureLibrary`,
//! `MaterialLibrary`, `MaterialBatcher`, `MaterialAnimation`) by name; they
//! are no materials, so the library's `CreateInstance` fails for them as
//! it does here.
//!
//! Types other libraries register (`surfacematerials`'s) come from a
//! fallback factory the caller gives, which is asked after `Shading`'s, as
//! the manager asks the factories in registration order.

use neuron_core::material_data::MaterialKind;

use super::GR_INTERFACE_UNSUPPORTED;
use super::batched::BatchedMaterial;
use super::instance::{Material, MaterialFactory};
use super::kinds::ShadingMaterial;
use super::services::MaterialServices;

/// The names `Shading.dll` registers that are no materials.
pub const OTHER_COMPONENTS: [&str; 4] = [
    "TextureLibrary",
    "MaterialLibrary",
    "MaterialBatcher",
    "MaterialAnimation",
];

/// Makes `Shading`'s materials by type name.
pub struct ShadingFactory<S: ?Sized> {
    fallback: Option<Box<dyn MaterialFactory<S>>>,
}

impl<S: ?Sized> std::fmt::Debug for ShadingFactory<S> {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("ShadingFactory")
            .field("fallback", &self.fallback.is_some())
            .finish()
    }
}

impl<S: ?Sized> Default for ShadingFactory<S> {
    fn default() -> Self {
        Self::new()
    }
}

impl<S: ?Sized> ShadingFactory<S> {
    /// `Shading`'s factories alone.
    #[must_use]
    pub fn new() -> ShadingFactory<S> {
        ShadingFactory { fallback: None }
    }

    /// `Shading`'s factories, then `fallback` for any other name.
    #[must_use]
    pub fn with_fallback(fallback: Box<dyn MaterialFactory<S>>) -> ShadingFactory<S> {
        ShadingFactory {
            fallback: Some(fallback),
        }
    }
}

impl<S> MaterialFactory<S> for ShadingFactory<S>
where
    S: MaterialServices + ?Sized + 'static,
{
    fn create(&mut self, services: &mut S, type_name: &[u8]) -> Result<Box<dyn Material<S>>, i32> {
        if type_name == b"BatchedMaterial" {
            return Ok(Box::new(BatchedMaterial::<S>::new()));
        }
        if let Some(kind) = MaterialKind::by_name(type_name) {
            return Ok(Box::new(ShadingMaterial::<S::Animation>::new(kind)));
        }
        if OTHER_COMPONENTS
            .iter()
            .any(|name| name.as_bytes() == type_name)
        {
            return Err(GR_INTERFACE_UNSUPPORTED);
        }
        match self.fallback.as_mut() {
            Some(fallback) => fallback.create(services, type_name),
            None => Err(GR_INTERFACE_UNSUPPORTED),
        }
    }
}
