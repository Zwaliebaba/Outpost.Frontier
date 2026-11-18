//! `PrintScreen` (`FDirectX8::PrintScreen` in
//! `CppSource/DX8RENDERER/DX8RENDERER.cpp`): a file of the scene in the
//! format its extension names.
//!
//! The C++ takes the extension from the last `.` and matches it without
//! case against `.BMP`, `.JPG`, `.TGA`, `.PNG`, `.DDS`, `.PPM` and `.DIB`,
//! then saves the front buffer through D3DX. The port writes BMP (also for
//! `.DIB`) and PNG (record 0004, owner answer 4); the other five names are
//! refused (divergence D210). Both C++ callers write BMP. The model returns
//! the file's bytes; writing them is the caller's.

use neuron_core::image::write::{self, WriteError};

/// Why a screenshot was not made.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ScreenshotError {
    /// The name has no `.` ("couldnt find appropriate extension").
    NoExtension,
    /// The extension is none of the seven the C++ knows.
    UnknownExtension(String),
    /// A format the C++ wrote and the port does not (D210).
    Unsupported(String),
    /// The backend has no scene to read (a recording backend).
    NoScene,
    Write(WriteError),
}

impl std::fmt::Display for ScreenshotError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::NoExtension => f.write_str("the screenshot name has no extension"),
            Self::UnknownExtension(e) => write!(f, "no screenshot format for {e}"),
            Self::Unsupported(e) => write!(f, "screenshots are not written as {e}"),
            Self::NoScene => f.write_str("the renderer has no scene to read"),
            Self::Write(e) => write!(f, "the screenshot could not be written: {e}"),
        }
    }
}

impl std::error::Error for ScreenshotError {}

/// The file format for `file_name`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Format {
    Bmp,
    Png,
}

/// The format `PrintScreen` would pick for `file_name`.
pub fn format_for(file_name: &str) -> Result<Format, ScreenshotError> {
    let Some(at) = file_name.rfind('.') else {
        return Err(ScreenshotError::NoExtension);
    };
    let ext = &file_name[at..];
    let known = [".BMP", ".JPG", ".TGA", ".PNG", ".DDS", ".PPM", ".DIB"];
    let Some(name) = known.iter().find(|k| k.eq_ignore_ascii_case(ext)) else {
        return Err(ScreenshotError::UnknownExtension(ext.to_string()));
    };
    match *name {
        ".BMP" | ".DIB" => Ok(Format::Bmp),
        ".PNG" => Ok(Format::Png),
        _ => Err(ScreenshotError::Unsupported(ext.to_string())),
    }
}

/// The file for `file_name` of a `width * height` BGRA scene.
pub fn encode(
    file_name: &str,
    width: u32,
    height: u32,
    bgra: &[u8],
) -> Result<Vec<u8>, ScreenshotError> {
    let bytes = match format_for(file_name)? {
        Format::Bmp => write::bmp(width, height, bgra),
        Format::Png => write::png(width, height, bgra),
    };
    bytes.map_err(ScreenshotError::Write)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn extensions_choose_as_print_screen_does() {
        assert_eq!(format_for("shot.bmp"), Ok(Format::Bmp));
        assert_eq!(format_for("a.b/SHOT.Dib"), Ok(Format::Bmp));
        assert_eq!(format_for("x.png"), Ok(Format::Png));
        assert_eq!(
            format_for("x.jpg"),
            Err(ScreenshotError::Unsupported(".jpg".into()))
        );
        assert_eq!(
            format_for("x.gif"),
            Err(ScreenshotError::UnknownExtension(".gif".into()))
        );
        assert_eq!(format_for("noext"), Err(ScreenshotError::NoExtension));
        // The last dot counts, as strrchr finds it.
        assert_eq!(format_for("a.png.bmp"), Ok(Format::Bmp));
    }

    #[test]
    fn files_are_encoded() {
        let px = [0u8, 0, 255, 255];
        assert!(encode("s.bmp", 1, 1, &px).unwrap().starts_with(b"BM"));
        assert!(encode("s.png", 1, 1, &px).unwrap().starts_with(b"\x89PNG"));
    }
}
