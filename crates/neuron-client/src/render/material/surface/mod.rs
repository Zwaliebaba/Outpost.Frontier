//! The surface materials (work package P4.5): the 19 material factories
//! `surfacematerials.dll` registers (`G_MaterialSpecs` in
//! `CppSource/surfacematerials/surfacematerials.cpp`), 15 kinds of
//! `FMaterialImpl` (`MaterialImpl.cpp`).
//!
//! * [`schema`]: the kinds' property schemas.
//! * [`services`]: what they call beyond [`MaterialServices`]
//!   ([`SurfaceServices`]): the engine `FLAppearance` publishes, its
//!   instance values, the visible-geometry counters and the material a
//!   submission replaces.
//! * [`material`]: [`SurfaceMaterial`], one type whose kind selects the
//!   recipes.
//!
//! [`SurfaceFactory`] makes them by exact name; it is the fallback
//! [`super::ShadingFactory::with_fallback`] takes, as the component
//! manager asks `surfacematerials`' factories after `Shading`'s.
//!
//! The override materials (`ColorizeMaterial`, `WireMaterial`,
//! `WireMaterial2`, `CloakMaterial`, `EnvMapMaterial`, `BackdropMaterial`,
//! `StarsphereMaterial`, `ShadowMaterial`, `VisibleCounterMaterial`) are
//! the ones `FLAppearance` creates by name and the mesh renderer draws a
//! face group's material with; the others are named by the shipped
//! material libraries.
//!
//! [`MaterialServices`]: super::MaterialServices

pub mod material;
pub mod schema;
pub mod services;

pub use material::{SurfaceMaterial, SurfaceState, SurfaceTexture};
pub use services::{MeshIndices, SurfaceEngine, SurfaceServices};

use super::GR_INTERFACE_UNSUPPORTED;
use super::instance::{Material, MaterialFactory};

/// The kinds of `FMaterialImpl` (`EMaterialKind`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum SurfaceKind {
    /// `GlassMaterial`, `HighGlassMaterial`, `GFGlassMaterial`.
    Glass,
    /// `VeilbornMaterial`, `VeilbornMaterialNoBendy`.
    Veilborn,
    /// `BackdropMaterial`, `StarsphereMaterial`.
    Backdrop,
    /// `HUDIconMaterial`.
    HudIcon,
    /// `HUDAnimMaterial`.
    HudAnim,
    /// `ColorizeMaterial`.
    Colorize,
    /// `CloakMaterial`.
    Cloak,
    /// `WireMaterial`.
    Wire,
    /// `WireMaterial2`.
    Wire2,
    /// `ExclusionZoneMaterial`.
    Exclusion,
    /// `EnvMapMaterial`.
    EnvMap,
    /// `ShadowMaterial`.
    Shadow,
    /// `EngineGlowMaterial`.
    EngineGlow,
    /// `VisibleCounterMaterial`.
    VisibleCounter,
    /// `PlanetWaterMaterial`.
    PlanetWater,
}

/// A factory: its name and kind (`FMaterialSpec`).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct SurfaceSpec {
    /// The exact name the factory answers.
    pub name: &'static str,
    /// The kind it makes.
    pub kind: SurfaceKind,
}

const fn spec(name: &'static str, kind: SurfaceKind) -> SurfaceSpec {
    SurfaceSpec { name, kind }
}

/// `G_MaterialSpecs`, in registration order.
pub const SPECS: [SurfaceSpec; 19] = [
    spec("VeilbornMaterial", SurfaceKind::Veilborn),
    spec("VeilbornMaterialNoBendy", SurfaceKind::Veilborn),
    spec("HUDIconMaterial", SurfaceKind::HudIcon),
    spec("HUDAnimMaterial", SurfaceKind::HudAnim),
    spec("ColorizeMaterial", SurfaceKind::Colorize),
    spec("CloakMaterial", SurfaceKind::Cloak),
    spec("WireMaterial", SurfaceKind::Wire),
    spec("WireMaterial2", SurfaceKind::Wire2),
    spec("ExclusionZoneMaterial", SurfaceKind::Exclusion),
    spec("GlassMaterial", SurfaceKind::Glass),
    spec("HighGlassMaterial", SurfaceKind::Glass),
    spec("GFGlassMaterial", SurfaceKind::Glass),
    spec("EnvMapMaterial", SurfaceKind::EnvMap),
    spec("BackdropMaterial", SurfaceKind::Backdrop),
    spec("StarsphereMaterial", SurfaceKind::Backdrop),
    spec("ShadowMaterial", SurfaceKind::Shadow),
    spec("EngineGlowMaterial", SurfaceKind::EngineGlow),
    spec("VisibleCounterMaterial", SurfaceKind::VisibleCounter),
    spec("PlanetWaterMaterial", SurfaceKind::PlanetWater),
];

/// The name `surfacematerials.dll` registers that is no material.
pub const APPEARANCE: &str = "FLAppearance";

impl SurfaceSpec {
    /// The factory whose name is exactly `name`.
    #[must_use]
    pub fn by_name(name: &[u8]) -> Option<SurfaceSpec> {
        SPECS.iter().copied().find(|s| s.name.as_bytes() == name)
    }
}

/// Makes the surface materials by exact name (`FMaterialFactory`).
#[derive(Clone, Copy, Debug, Default)]
pub struct SurfaceFactory;

impl SurfaceFactory {
    /// The 19 factories.
    #[must_use]
    pub fn new() -> SurfaceFactory {
        SurfaceFactory
    }
}

impl<S> MaterialFactory<S> for SurfaceFactory
where
    S: SurfaceServices + ?Sized + 'static,
{
    /// `FMaterialFactory::CreateInstance`: the material of that name,
    /// constructed and initialized from its descriptor. Other names, and
    /// `FLAppearance` (no material), are unsupported.
    fn create(&mut self, services: &mut S, type_name: &[u8]) -> Result<Box<dyn Material<S>>, i32> {
        let _ = services;
        let spec = SurfaceSpec::by_name(type_name).ok_or(GR_INTERFACE_UNSUPPORTED)?;
        Ok(Box::new(
            SurfaceMaterial::<S::Animation, S::TextureRecord>::new(spec),
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn nineteen_names_of_fifteen_kinds() {
        let mut kinds: Vec<SurfaceKind> = SPECS.iter().map(|s| s.kind).collect();
        kinds.dedup();
        assert_eq!(SPECS.len(), 19);
        let unique: std::collections::HashSet<_> = SPECS.iter().map(|s| s.kind).collect();
        assert_eq!(unique.len(), 15);
        assert_eq!(
            SurfaceSpec::by_name(b"HighGlassMaterial").map(|s| s.kind),
            Some(SurfaceKind::Glass)
        );
        assert!(SurfaceSpec::by_name(b"FLAppearance").is_none());
        assert!(SurfaceSpec::by_name(b"highglassmaterial").is_none());
    }
}
