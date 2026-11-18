//! Textures at run time (work package P4.4): the texture library of
//! `CppSource/Shading/` (`TextureLibrary.cpp`, the texture records of
//! `ShadingInternal.h`) and the large-image tiler of `CppSource/ximage/`.
//!
//! * [`library`]: groups, records, reference counts, garbage collection
//!   and loading, over `neuron-core`'s texture loader (work package P1.7,
//!   which owns the file formats and the loading rules).
//! * [`playback`]: texture animation, the frame a playback shows as time
//!   passes.
//! * [`pipeline`]: the renderer calls the texture code makes, as a trait,
//!   and the recording contract the oracle harness shares.
//! * [`device`]: those calls answered by the device model
//!   ([`crate::render::ff`]), whose textures reach the Direct3D 12 backend
//!   through its resource events.
//! * [`format_class`]: the format a texture asked for in a format class is
//!   made in (DX8RENDERER's format catalog).
//! * [`big_image`]: images larger than a texture, cut into tiles.
//!
//! No platform code: everything here builds and tests on every host.

pub mod big_image;
pub mod device;
pub mod format_class;
pub mod library;
pub mod pipeline;
pub mod playback;

pub use big_image::{BigImage, BigImageDesc, Rect, TileDraw};
pub use device::{DevicePipeline, HandleTable};
pub use format_class::FormatClasses;
pub use library::{Group, GroupLoad, LibraryConfig, RecordId, TextureLibrary};
pub use pipeline::{Abilities, ContractPipeline, PipelineCall, TexturePipeline};
pub use playback::{FrameOut, Playback, Source};
