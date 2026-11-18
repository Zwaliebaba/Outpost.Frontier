# Shaders

The HLSL sources of the renderer's shaders (decision record 0004, section
3). The blobs in `../compiled_shaders/` are committed, so building the
crate needs no shader compiler on any host.

`shaders.toml` lists every blob: the source file, entry point, profile
(shader model 5.1, DXBC), output name and preprocessor defines.
`cargo xtask shaders` compiles them on Windows with `fxc.exe` from Windows
SDK 10.0.26100.0 and these flags, which are fixed:

```
/nologo /WX /O3 /Ges /Gis /Zpr /Qstrip_debug /Qstrip_reflect /Qstrip_priv
```

Strict mode and IEEE strictness keep the compiler from reassociating
arithmetic, `/Zpr` makes matrices row-major as the renderer uploads them,
and stripping debug, reflection and private data makes a blob depend only
on its source and the compiler. The compiler DLL below is the only one the
step accepts:

d3dcompiler_47.dll file version: `10.0.26100.9457`

`cargo xtask shaders --check` (run by the Windows CI job) recompiles and
fails when a committed blob differs from its source; it then prints each
new blob in base64 between `----- begin <name>` and `----- end <name>`
lines, so a blob compiled on the CI runner can be committed from a host
without the compiler.

`fixed_function.hlsl` is Direct3D 8's fixed-function pipeline: one vertex
shader and one pixel shader (compiled twice, once with `FLAT` for
`SHADEMODE FLAT`) that branch on the per-draw constants. Its constant
buffer is laid out by `render::shader::constants`, and a test compares the
two.

Without a Windows machine the blobs can be made under Wine: the
`Microsoft.Windows.SDK.CPP` NuGet package of version 10.0.26100.9457
carries `fxc.exe` and the recorded `d3dcompiler_47.dll` in
`c/bin/10.0.26100.0/x64/`. Run `fxc.exe` from the `shaders` directory with
the flags above, then `/T <profile> /E <entry>`, `/D <define>` for each
define and `/Fo ../compiled_shaders/<output> fixed_function.hlsl`, in that
order; the Windows CI check confirms the result.
