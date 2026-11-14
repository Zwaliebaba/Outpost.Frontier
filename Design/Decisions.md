# Decisions

All rulings by the owner. A ruling that differs from the recommendation it was offered with says
so, because the documents were first written to the recommendation.

## Standards (2026-11-14)

| # | Decision | Ruling |
|---|---|---|
| S1 | Language, platform | C++23 (MSVC `/std:c++latest`), Windows x64 |
| S2 | Graphics API | Direct3D 12 on raw Win32 and DXGI |
| S3 | Maths | DirectXMath used natively, no wrapper layer. In the simulation the maths is `Fixed64` and its vectors, ported from the original; DirectXMath is the presentation's |
| S4 | Shaders | HLSL compiled with `dxc` at Shader Model 6 |
| S5 | Libraries | C++ standard library and Windows SDK; anything else is an exception recorded here. **Exceptions today: none.** Cook-time tools that are run, not linked, and never shipped are listed under T below |

## Rulings on the open questions (2026-11-14, Plan 0.1)

| # | Question | Ruling | Consequence |
|---|---|---|---|
| D1 | Presentation fidelity | **Parity with the original first**; improvements (resolution, AA, ultrawide) as options after M5 | reference-shot comparison is the acceptance test for phases 5–8 |
| D2 | Renderer and lighting | **Deferred shading, as the original; GI by our own offline bake** of light probes and lightmaps per WorldArt scene | Plan 5.3; Risks R3 |
| D3 | When the cook stops depending on AssetStudioModCLI | **After M2** | Plan 2.7 follows phase 4 |
| D4 | Mission scripts | **Recover the uScript graphs as data**, run by a node runtime; mechanical translation of the generated C# only as the fallback per graph | Plan 2.6, 9.1; Risks R2 |
| D5 | Streamed audio | **xWMA**, which XAudio2 plays natively, re-encoded offline at cook time; short clips stay PCM *(the recommendation was to keep Vorbis with a decoder exception)* | no runtime decoder; one lossy re-encode of 270 streamed clips; tool T2 |
| D6 | Video | **Transcode to H.264 + AAC in MP4** at cook time; playback through Media Foundation | tool T1 |
| D7 | Targets | **Windows 11 only; Direct3D 12 feature level 12_1** *(the recommendation was Windows 10 22H2+ and 12_0)*; 60 fps at 1080p on a mid-range GPU | the renderer may rely on 12_1 features (conservative rasterisation, rasterizer-ordered views) without fallbacks |
| D8 | Saves | **A format of our own** *(the recommendation was the original's format)* | original saves do not load; save/load is designed in phase 10 against our `Sim` state, and saves are not a test oracle |
| D9 | Cross-play with the original | **Not wanted** *(the recommendation was a stretch goal)* | the network protocol may change; only lockstep among rebuilt clients matters |
| D10 | Coding conventions | **PascalCase types and functions, `m_` members, `_` parameters, UPPER_CASE constants** | [CONTRIBUTING.md](../CONTRIBUTING.md) |
| D11 | Platform SDKs (Steamworks, GOG Galaxy) | **Neither for now** *(the recommendation was to accept both)* | multiplayer is LAN only; achievements, stats and cloud saves are out of scope until this is revisited; `BBI.Steam`, `GOG.GalaxyWrapper` and `BBI.Platform` are not ported |
| D12 | Physics | **None**; ray and shape queries of our own for picking | |
| D13 | Middleware decompiled as specification | **Yes**, locally (`DoK-Decompiled\Middleware\`), never committed | Plan 0.5 |
| D14 | Test vectors computed in the original, in CI | **Yes, committed**: they are numbers the original produced, not its code or data | CI tests `Core` against them |

## Open

| # | Question | Options | Recommendation |
|---|---|---|---|
| D15 | **The licensed fonts** in `encrypted/font` (an AES-encrypted bundle the game decrypts at runtime, `LegalFonts`): they were encrypted to protect their licence, so they are not extracted | (a) substitute fonts of our own choice; (b) decrypt them at runtime from the owner's installation, as the original does; (c) license the fonts | **(a)** for now: the UI (Plan phase 8) is the first to need them; revisit then |

## Tools (run, not linked into the product, not shipped)

| # | Tool | For | Decided by |
|---|---|---|---|
| T1 | ffmpeg | Theora/Vorbis → H.264/AAC MP4 | D6 |
| T2 | an xWMA encoder (Microsoft's `xWMAEncode`, from the DirectX SDK) | streamed audio → xWMA | D5 |
| T3 | `dxc` | HLSL → DXIL | S4 (part of the Windows SDK / build) |
| T4 | ilspycmd 11.2, AssetStudioModCLI 0.20.0 | the inputs (`Tools/Extract`) | 2026-11-14 |
| T5 | BepInEx 5.4.23.6 with HarmonyX, in a copy of the original game | the Recorder (`Tools/Recorder`, C# for the game's .NET 3.5 Mono): ground truth from the original | 2026-11-14 |
