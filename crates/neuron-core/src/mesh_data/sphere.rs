//! The `Sphere` node of a tessellated planet (`FSphereHandle::Load` in
//! `Plugins.cpp`): `Radius`, `Sides` (the number of materials) and the
//! material names `M0`, `M1`, ...

use super::{
    Malformed, f32_at, open_node, printed_children, printed_name, read_exact, read_string, read_u32,
};
use crate::vfs::FileSystem;

/// A loaded `Sphere` node.
#[derive(Clone, Debug, PartialEq)]
pub struct SphereMesh {
    /// `Radius`, or -1.0 (the reset value) when it does not read as four
    /// bytes ("no radius! invalid sphere file?").
    pub radius: f32,
    /// Whether `Radius` read.
    pub radius_read: bool,
    /// `Sides`, or 0 when it does not read ("no sides! invalid sphere
    /// file?").
    pub sides: u32,
    /// Whether `Sides` read.
    pub sides_read: bool,
    /// The material names that read, by index below `sides`: at most 0x3F
    /// bytes, to the first NUL, lower-cased. The others are null names,
    /// each with a warning.
    pub material_names: Vec<(u32, Vec<u8>)>,
    /// The scale the load seeds; an `AtmosphereMaterial` in slot 6 may
    /// replace it once materials are hooked up, which is the client's.
    pub scale: f32,
}

impl SphereMesh {
    /// The name of material `index`, if it read.
    #[must_use]
    pub fn material_name(&self, index: u32) -> Option<&[u8]> {
        self.material_names
            .iter()
            .find(|(i, _)| *i == index)
            .map(|(_, name)| name.as_slice())
    }

    /// The number of `FDUMP` warnings the load made.
    #[must_use]
    pub fn dump_count(&self) -> u64 {
        let missing = u64::from(self.sides) - self.material_names.len() as u64;
        u64::from(!self.radius_read) + u64::from(!self.sides_read) + missing
    }
}

/// `FSphereMeshPlugin::Claim`: `None` when there is no `Sphere` node (the
/// plugin returns -1). The current node ends where it started.
///
/// # Errors
///
/// A malformed archive.
pub fn load(fs: &mut dyn FileSystem) -> Result<Option<SphereMesh>, Malformed> {
    if !open_node(fs, b"Sphere")? {
        return Ok(None);
    }
    let radius = read_exact(fs, b"Radius", 4)?.map(|b| f32_at(&b, 0));
    let sides = read_u32(fs, b"Sides")?;
    let mut sphere = SphereMesh {
        radius: radius.unwrap_or(-1.0),
        radius_read: radius.is_some(),
        sides: sides.unwrap_or(0),
        sides_read: sides.is_some(),
        material_names: Vec::new(),
        scale: 1.0,
    };
    for index in printed_children(fs, b"M", sphere.sides)? {
        if let Some(mut name) = read_string(fs, &printed_name(b"M", index), 0x3F)? {
            name.make_ascii_lowercase();
            sphere.material_names.push((index, name));
        }
    }
    let _ = open_node(fs, b"..")?;
    Ok(Some(sphere))
}
