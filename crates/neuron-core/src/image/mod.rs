//! Texture data (work package P1.7): pixel formats, TGA and DDS levels,
//! the texture library's format rules and its loading of `Texture
//! library` nodes, ported from `CppSource/Shading/TextureLibrary.cpp` and
//! `ShadingInternal.h`.
//!
//! * [`format`]: the engine's pixel format numbers and their tables.
//! * [`tga`]: the reference's own decoder for uncompressed TGA mip levels
//!   (`MIP0` to `MIP9` nodes).
//! * [`dds`]: DDS headers and the image information D3DX reports for
//!   them (`MIPS` and `CUBE` nodes).
//! * [`format_map`]: `TextureFormatMaps`, the choice of the format a
//!   texture is created in.
//! * [`library`]: the loading rules of the texture library, recorded as
//!   renderer calls instead of being sent to a device.
//! * [`convert`]: the port's conversion of source pixels to RGBA8, which
//!   the reference leaves to `D3DXLoadSurfaceFromMemory` on a device.
//! * [`write`]: BMP and PNG files of BGRA pixels, for screenshots.
//!
//! No renderer and no device: creating textures from these calls is work
//! package P4.4.

pub mod convert;
pub mod dds;
pub mod format;
pub mod format_map;
pub mod library;
pub mod tga;
pub mod write;

pub use convert::{ConvertError, to_rgba8};
pub use dds::{DdsHeader, ImageInfo};
pub use format::PixelFormat;
pub use format_map::{FormatMapRow, FormatMaps};
pub use library::{
    Fault, GroupEntry, LevelSource, LevelUpload, LoadOutcome, LoaderSettings, TextureEvent,
    TextureFrame, TextureLoader, TextureRecord,
};
pub use tga::{TgaError, TgaHeader, TgaImage};

#[cfg(test)]
mod tests;
