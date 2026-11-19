//! Run-time HLSL compilation for the programmable passes (feature
//! `shader-compile`, development only; record 0005, point 11): the
//! pinned `dxcompiler.dll` (and `dxil.dll`, which signs the output) from
//! the `Microsoft.Direct3D.DXC` package beside the executable. Includes
//! are read from disk, first beside the compiled file. Release builds
//! embed the blobs `cargo xtask shaders` writes and never load the DLL.

use std::path::Path;

use windows::Win32::Graphics::Direct3D::Dxc::{
    CLSID_DxcCompiler, CLSID_DxcUtils, DXC_CP_UTF8, DxcBuffer, DxcCreateInstance, IDxcCompiler3,
    IDxcIncludeHandler, IDxcResult, IDxcUtils,
};
use windows::core::{HSTRING, PCWSTR};

use super::{Context, GpuError};
use crate::render::pass::ShaderCompiler;

/// The options after the entry and profile: the fixed set `cargo xtask
/// shaders` compiles the committed blobs with (`xtask/src/shaders.rs`,
/// record 0004's amendment of 2026-11-19), without `-nologo`, which only
/// the command line knows.
pub const FLAGS: &[&str] = &[
    "-WX",
    "-O3",
    "-Ges",
    "-Gis",
    "-Zpr",
    "-HV",
    "2021",
    "-Qstrip_debug",
    "-Qstrip_reflect",
    "-Qstrip_priv",
];

/// The compiler.
pub struct Dxc {
    compiler: IDxcCompiler3,
    include: IDxcIncludeHandler,
}

impl Dxc {
    /// Loads the compiler; fails without `dxcompiler.dll`.
    pub fn new() -> Result<Dxc, GpuError> {
        // SAFETY: plain object creation through the DLL's entry point.
        let compiler: IDxcCompiler3 =
            unsafe { DxcCreateInstance(&CLSID_DxcCompiler) }.at("DxcCreateInstance")?;
        // SAFETY: as above.
        let utils: IDxcUtils =
            unsafe { DxcCreateInstance(&CLSID_DxcUtils) }.at("DxcCreateInstance")?;
        // SAFETY: a plain call on a live object.
        let include =
            unsafe { utils.CreateDefaultIncludeHandler() }.at("CreateDefaultIncludeHandler")?;
        Ok(Dxc { compiler, include })
    }
}

/// The bytes of a blob.
///
/// # Safety
///
/// `pointer` and `len` are a live blob's own buffer.
unsafe fn bytes(pointer: *const std::ffi::c_void, len: usize) -> Vec<u8> {
    if pointer.is_null() || len == 0 {
        return Vec::new();
    }
    // SAFETY: the caller passes a live blob's own pointer and size.
    unsafe { std::slice::from_raw_parts(pointer.cast::<u8>(), len) }.to_vec()
}

impl ShaderCompiler for Dxc {
    fn compile(&mut self, path: &Path, entry: &str, profile: &str) -> Result<Vec<u8>, String> {
        let source = std::fs::read(path).map_err(|e| format!("{}: {e}", path.display()))?;
        let dir = path.parent().unwrap_or(Path::new("."));
        let mut args: Vec<HSTRING> = vec![
            HSTRING::from(path.as_os_str()),
            "-E".into(),
            entry.into(),
            "-T".into(),
            profile.into(),
            "-I".into(),
            HSTRING::from(dir.as_os_str()),
        ];
        args.extend(FLAGS.iter().map(|f| HSTRING::from(*f)));
        let pointers: Vec<PCWSTR> = args.iter().map(|a| PCWSTR(a.as_ptr())).collect();
        let buffer = DxcBuffer {
            Ptr: source.as_ptr().cast(),
            Size: source.len(),
            Encoding: DXC_CP_UTF8.0,
        };
        let fail = |e: windows::core::Error| format!("{}: {e}", path.display());
        // SAFETY: the buffer and the argument strings live for the call.
        let result: IDxcResult = unsafe {
            self.compiler
                .Compile(&buffer, Some(&pointers), &self.include)
        }
        .map_err(fail)?;
        // SAFETY: plain queries of the result.
        let status = unsafe { result.GetStatus() }.map_err(fail)?;
        if status.is_err() {
            // SAFETY: as above; the blob's pointer and size are its own.
            let log = unsafe { result.GetErrorBuffer() }
                .map(|b| unsafe { bytes(b.GetBufferPointer(), b.GetBufferSize()) })
                .unwrap_or_default();
            return Err(String::from_utf8_lossy(&log).into_owned());
        }
        // SAFETY: as above.
        let blob = unsafe { result.GetResult() }.map_err(fail)?;
        // SAFETY: the blob's pointer and size are its own.
        Ok(unsafe { bytes(blob.GetBufferPointer(), blob.GetBufferSize()) })
    }
}
