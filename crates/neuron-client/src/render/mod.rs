//! The renderer (decision record 0004): a platform-free model of the
//! Direct3D 8 device the C++ draws through ([`ff`]), and later the
//! Direct3D 12 backend and the trace player; textures at run time
//! ([`texture`]); materials, their animation and the batcher
//! ([`material`]); the render manager, levels of detail and the mesh
//! kinds ([`mesh`]); `FLAppearance`'s override materials, backdrop,
//! starsphere and blob shadows ([`appearance`]); particle effects
//! ([`particles`]).

pub mod appearance;
pub mod compare;
pub mod ff;
pub mod material;
pub mod mesh;
pub mod particles;
pub mod shader;
pub mod texture;

/// The trace reader and model replay, behind the feature `render-trace`
/// (on by default; the game executable turns it off).
#[cfg(feature = "render-trace")]
pub mod trace;
