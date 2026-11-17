//! How the render manager turns an archetype node into levels of detail
//! (`FRenderManager::CreateArchetype`, `MultiLevel` and `WalkPlugins` in
//! `RenderManager.cpp`).
//!
//! A `MultiLevel` node holds `Switch2`, the switch distances (one more than
//! the levels), and `Level0`, `Level1`, ... Every plugin is offered every
//! level and the last one that claims it keeps it. Without `MultiLevel`
//! the first plugin that claims the node itself is the only level, with
//! the switch distances 0 and 1e6.

use super::part::{self, VMeshPart};
use super::sphere::{self, SphereMesh};
use super::trimesh::{self, TriMesh};
use super::{Malformed, child, f32s, read_bytes};
use crate::vfs::FileSystem;

/// The far switch distance of a single level (`G_DefaultLodDistance`).
pub const DEFAULT_LOD_DISTANCE: f32 = 1_000_000.0;

/// A render plugin, as the `[RenderManager]` section names them.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RenderPlugin {
    /// `VMesh`: a `VMeshPart` node.
    VMesh,
    /// `TriMesh`: a `Mesh` node, with the `[TriMesh] tristrips` setting.
    TriMesh {
        /// Keep strips as strips.
        tristrips: bool,
    },
    /// `SphereMesh`: a `Sphere` node.
    SphereMesh,
}

/// The client's plugins: `[RenderManager]` and `[TriMesh] tristrips = 0`
/// of `EXE/NeuronComponents.ini`.
pub const CLIENT_PLUGINS: [RenderPlugin; 3] = [
    RenderPlugin::VMesh,
    RenderPlugin::TriMesh { tristrips: false },
    RenderPlugin::SphereMesh,
];

/// What a plugin claimed.
#[derive(Clone, Debug, PartialEq)]
pub enum RenderClaim {
    /// A `VMeshPart`.
    VMesh(VMeshPart),
    /// A `Mesh`.
    TriMesh(Box<TriMesh>),
    /// A `Sphere`.
    Sphere(SphereMesh),
}

/// One level of detail.
#[derive(Clone, Debug, PartialEq)]
pub struct RenderLevel {
    /// The index into the plugin list of the plugin that kept it.
    pub plugin: usize,
    /// What it claimed.
    pub claim: RenderClaim,
}

/// An archetype's levels of detail (`FLodSet`).
#[derive(Clone, Debug, PartialEq)]
pub struct RenderArchetype {
    /// Whether they came from a `MultiLevel` node.
    pub multi_level: bool,
    /// The switch distances: one more than the levels, or none for a
    /// `Switch2` of a single value.
    pub switch: Vec<f32>,
    /// The levels.
    pub levels: Vec<RenderLevel>,
}

/// `Claim(fs)` of one plugin: `Some` when it claims the node.
///
/// # Errors
///
/// A malformed archive.
pub fn claim(
    fs: &mut dyn FileSystem,
    plugin: RenderPlugin,
) -> Result<Option<RenderClaim>, Malformed> {
    Ok(match plugin {
        RenderPlugin::VMesh => part::claim(fs)?.ok().map(RenderClaim::VMesh),
        RenderPlugin::TriMesh { tristrips } => {
            trimesh::load(fs, tristrips)?.map(|m| RenderClaim::TriMesh(Box::new(m)))
        }
        RenderPlugin::SphereMesh => sphere::load(fs)?.map(RenderClaim::Sphere),
    })
}

/// `FRenderManager::MultiLevel`. `None` where it returns -1.
///
/// `Switch2` read as floats; a read that fails counts as none, which gives
/// one level and two zero distances. A single value gives no levels and
/// succeeds; the reference copies the value into an empty array there
/// (divergence register).
///
/// # Errors
///
/// A malformed archive.
pub fn multi_level(
    fs: &dyn FileSystem,
    plugins: &[RenderPlugin],
) -> Result<Option<RenderArchetype>, Malformed> {
    let Some(multi) = child(fs, b"MultiLevel", None)? else {
        return Ok(None);
    };
    let values = read_bytes(multi.as_ref(), b"Switch2")?
        .map(|b| f32s(&b))
        .unwrap_or_default();
    let (count, switch) = match values.len() {
        0 => (1, vec![0.0; 2]),
        1 => (0, Vec::new()),
        n => (n - 1, values),
    };
    let mut levels = Vec::with_capacity(count.min(64));
    for index in 0..count {
        let name = format!("Level{index}");
        let Some(mut level) = child(multi.as_ref(), name.as_bytes(), None)? else {
            return Ok(None);
        };
        if plugins.is_empty() {
            return Ok(None);
        }
        let mut kept = None;
        for (plugin, &kind) in plugins.iter().enumerate() {
            if let Some(claim) = claim(level.as_mut(), kind)? {
                kept = Some(RenderLevel { plugin, claim });
            }
        }
        match kept {
            Some(kept) => levels.push(kept),
            None => return Ok(None),
        }
    }
    Ok(Some(RenderArchetype {
        multi_level: true,
        switch,
        levels,
    }))
}

/// `FRenderManager::WalkPlugins`: the first plugin that claims the node.
///
/// # Errors
///
/// A malformed archive.
pub fn single_level(
    fs: &mut dyn FileSystem,
    plugins: &[RenderPlugin],
) -> Result<Option<RenderArchetype>, Malformed> {
    for (plugin, &kind) in plugins.iter().enumerate() {
        if let Some(claim) = claim(fs, kind)? {
            return Ok(Some(RenderArchetype {
                multi_level: false,
                switch: vec![0.0, DEFAULT_LOD_DISTANCE],
                levels: vec![RenderLevel { plugin, claim }],
            }));
        }
    }
    Ok(None)
}

/// `FRenderManager::CreateArchetype`: [`multi_level`], else
/// [`single_level`]. `None` where it returns 0.
///
/// # Errors
///
/// A malformed archive.
pub fn create_archetype(
    fs: &mut dyn FileSystem,
    plugins: &[RenderPlugin],
) -> Result<Option<RenderArchetype>, Malformed> {
    if let Some(archetype) = multi_level(fs, plugins)? {
        return Ok(Some(archetype));
    }
    single_level(fs, plugins)
}
