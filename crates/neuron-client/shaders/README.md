# Shaders

The HLSL sources of the renderer's shaders (decision record 0004, section
3). The blobs in `../compiled_shaders/` are committed, so building the
crate needs no shader compiler on any host.

`shaders.toml` lists every blob: the source file, entry point, profile
(shader model 6.1, DXIL; record 0004, point 16 as amended by the owner on
2026-11-19), output name and preprocessor defines. `cargo xtask shaders`
compiles them on Windows with `dxc.exe` and these flags, which are fixed:

```
-nologo -WX -O3 -Ges -Gis -Zpr -HV 2021 -Qstrip_debug -Qstrip_reflect -Qstrip_priv
```

Strict mode and IEEE strictness keep the compiler from reassociating
arithmetic, `-Zpr` makes matrices row-major as the renderer uploads them,
`-HV 2021` is the HLSL language version, and stripping debug, reflection
and private data makes a blob depend only on its source and the compiler.
`dxil.dll` must sit beside `dxc.exe`: it validates and signs the blobs,
which Direct3D 12 requires of DXIL.

The compiler is the `Microsoft.Direct3D.DXC` NuGet package, version
1.9.2609.5 (`microsoft.direct3d.dxc.1.9.2609.5.nupkg`, SHA-256
`73cfa082245918c2cc1ec6740250eb7377e83a757b3b3a3e16e0d71535bfbfc3`), its
`build/native/bin/x64/` folder. Name it with `--dxc <path to dxc.exe>` or
the `OUTPOST_DXC` environment variable. The compiler DLL below is the only
one the step accepts:

dxcompiler.dll file version: `1.9.2609.5`

`cargo xtask shaders --check` (run by the Windows CI job, which fetches the
package from nuget.org and checks its hash) recompiles and fails when a
committed blob differs from its source; it then prints each new blob in
base64 between `----- begin <name>` and `----- end <name>` lines, so a
blob compiled on the CI runner can be committed from a host without the
compiler.

`fixed_function.hlsl` is Direct3D 8's fixed-function pipeline: one vertex
shader and one pixel shader (compiled twice, once with `FLAT` for
`SHADEMODE FLAT`) that branch on the per-draw constants. Its constant
buffer is laid out by `render::shader::constants`, and a test compares the
two.
