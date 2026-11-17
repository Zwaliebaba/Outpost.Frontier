//! Animation data (work package P1.7): channel archetypes and the script,
//! joint and object maps of `CppSource/SceneRuntime/SceneRuntime.cpp`.
//!
//! An archive (`.cmp`, `.anm`) holds its animations under
//! `\Animation\Script\<name>\`. Each `<name>` directory holds maps:
//!
//! * `Joint map*`: `Parent name`, `Child name` (NUL-terminated strings) and
//!   a `Channel` directory with `Header` and `Frames`;
//! * `Object map*`: `Parent name` and `Channel`;
//! * `Event map*`: skipped by the loader;
//! * any other directory: a track with no names and no channel;
//! * a file `Root height` (one f32); other files are ignored.
//!
//! [`script`] loads them as `IAnimation2::OpenScript` and `AddScriptNames`
//! do, through the [`FileSystem`](crate::vfs::FileSystem) port of
//! `ReadFile`; [`channel`] reads and unpacks one `Channel`. Playback,
//! sampling and the mixer are in [`crate::scene`].
//!
//! Where the reference has undefined behaviour (a null file system, a name
//! read with no terminator, a stamp read outside the frames, a frame buffer
//! shared between channels) the port defines it; the divergence register
//! lists each case. A malformed archive, on which the reference reads
//! outside its mapping, is [`AnimError::Malformed`].

pub mod channel;
pub mod script;

#[cfg(test)]
mod tests;

use crate::utf::UtfError;
use crate::vfs::VfsError;

pub use channel::{
    COMPRESS_MASK, Channel, ChannelError, ChannelHeader, ChannelKind, DecodeFatal,
    FLAG_IDENTITY_QUATERNION, FLAG_PACKED_AXIS_ANGLE, FLAG_PACKED_QUATERNION,
    FLAG_PACKED_QUATERNION4, FLAG_POSITION, FLAG_QUATERNION, FLAG_SCALAR, FLAG_SCALAR_ONLY,
    FLAG_ZERO_POSITION, LoadedChannel, SHORT_TO_UNIT, load_channel, stride_for,
};
pub use script::{AnimScript, MapKind, MapName, ScriptEntry, ScriptWarning, Track};

/// A failure the reference has no defined behaviour for.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AnimError {
    /// The archive is malformed where the reference reads outside it.
    Malformed(UtfError),
}

impl core::fmt::Display for AnimError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            Self::Malformed(e) => write!(f, "malformed animation archive: {e}"),
        }
    }
}

impl std::error::Error for AnimError {}

/// Separates the failures the reference reports (kept in the inner
/// result) from a malformed archive (the outer error).
pub(crate) fn lift<T>(result: Result<T, VfsError>) -> Result<Result<T, VfsError>, AnimError> {
    match result {
        Err(VfsError::Malformed(e)) => Err(AnimError::Malformed(e)),
        other => Ok(other),
    }
}
