# Decisions

## Ruled

| # | Decision | Ruling |
|---|---|---|
| S1 | Language, platform | C++23, MSVC, Windows x64 (owner, 2026-11-14) |
| S2 | Graphics API | Direct3D 12 on raw Win32 and DXGI (owner, 2026-11-14) |
| S3 | Maths | DirectXMath used natively, no wrapper layer (owner, 2026-11-14). In the simulation the maths is `Fixed64` and its vectors, ported from the original; DirectXMath is the presentation's |
| S4 | Shaders | HLSL compiled with `dxc` at Shader Model 6 (owner, 2026-11-14) |
| S5 | Libraries | C++ standard library and Windows SDK; anything else is an exception recorded here (owner, 2026-11-14) |

## Open (to rule in Plan 0.1)

Each with a recommendation.

| # | Question | Options | Recommendation |
|---|---|---|---|
| D1 | **Presentation fidelity** | (a) parity with the original, then options; (b) modernise from the start | **(a)**: parity is what can be checked; improvements (resolution, AA, ultrawide) as options after M5 |
| D2 | **Renderer and lighting** | deferred, as the original's decal system suggests, or forward+; GI by our own offline bake, or ambient probes only | **deferred, matching the original's pipeline, with an offline bake of light probes and lightmaps** per WorldArt scene (Risks R3) |
| D3 | **When the cook stops depending on AssetStudio** (stage 2b) | before M2, after M2, never | **after M2**: stage 2a unblocks everything; 2b is about reproducibility, not progress |
| D4 | **Mission scripts** | (a) recover the uScript graphs as data + a node runtime; (b) translate the generated C# to C++ mechanically | **(a)**, with (b) as the fallback per graph that does not recover (Risks R2): data is smaller, inspectable and debuggable; generated code would be ~1M lines of C++ |
| D5 | **Streamed audio** (270 clips; all audio is 6.0 GB as PCM, ~1 GB as Vorbis) | (a) keep Vorbis: library exception for a decoder (`stb_vorbis`, public domain, one file); (b) xWMA, native to XAudio2, re-encoded offline; (c) MS-ADPCM, native, larger and lossier | **(a)**: the original's own encoding, no generational loss, one small exception |
| D6 | **Video** (27 Theora + Vorbis) | (a) transcode offline to H.264 + AAC in MP4 for Media Foundation, with ffmpeg as a cook-time tool (not linked, not shipped); (b) a Theora decoder library exception | **(a)**: playback stays in the Windows SDK |
| D7 | **Targets** | minimum OS, D3D12 feature level, resolution and frame-rate targets | Windows 10 22H2+, feature level 12_0, 60 fps at 1080p on a mid-range GPU of the original's era; to confirm |
| D8 | **Saves** | (a) the original's save format, through the ported save/load code; (b) a format of our own | **(a)**: it comes with the port, and loading the original's saves is another oracle |
| D9 | **Cross-play with the original** | a goal, a stretch, not wanted | **stretch**: the protocol is ported exactly anyway; test LAN against the original once M6 is reached |
| D10 | **Coding conventions** | naming, layout, error handling | a `CONTRIBUTING.md` at Plan 0.2. Proposal: `PascalCase` types and functions, `m_` members, `UPPER_CASE` constants, one class per file pair, no exceptions across library boundaries; external SDK names keep their own spelling |
| D11 | **Platform SDK exceptions** | Steamworks, GOG Galaxy | **accept both**, isolated in `Net` and a platform-services interface |
| D12 | **Physics** | a physics library, or none | **none**: the simulation has its own geometry; the presentation needs only ray and shape queries for picking (and simple debris, if any) |
| D13 | **Middleware decompilation as specification** | decompile NGUI, Fabric, CinemaDirector, Cinemachine, PlayMaker, uScript runtime | **yes**, locally beside `DoK-Decompiled` (Plan 0.5); never committed |
| D14 | **Test vectors in CI** | may numeric test vectors computed in the original (Fixed64 results, hashes) be committed? | **yes**: they are numbers produced by running the original, not its code or data; confirm |
