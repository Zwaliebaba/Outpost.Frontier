# Verification strategy

How each work package of the [migration plan](migration-plan.md) is shown
to be correct. The C++ tree has no tests and the repository holds no game
data, so the reference has to be built (Phase 0).

## Fidelity tiers

| Tier | Meaning | Comparison | Typical content |
| --- | --- | --- | --- |
| A | Bit-exact | Byte or value equality | Hashes and IDs, INI parsing, number parsing and formatting, UTF container, asset parsers, save text and `FLS1` decoding (decision O12), account directory names, wire encodings and quantisers, the random generator, money and worth rules, asteroid cube IDs, mission script parsing, text catalogs, interface layout trees |
| B | Equal within tolerance | Per-value absolute or relative bound, stated in the test | Math kernels, animation and curve evaluation, audio attenuation and pan, camera solving, dead reckoning, physics scenarios, AI scenarios, mission runtime traces |
| C | Visually equivalent | Image difference under a threshold, then owner play-test | Rendering, materials, particles, text rasterising, effects |

Tier A items that involve floating point (money rules, cube IDs, quantisers)
use only basic arithmetic and are reproduced with the explicit `f32`/`f64`
sequence the C++ source spells out. The oracle runs them in 53-bit
precision, the dedicated server's mode (decision T11).

Default Tier B bounds, tightened per test where the data allows: 1 ulp for
single math kernels; 1e-5 relative for evaluated curves and transforms;
replay tests of physics, AI and damage on the oracle's recorded per-tick
state; scenario envelopes for closed-loop runs (position within a stated
distance, event times within a stated number of ticks), with bounds set in
P2.0 (decision O20).

## The oracle

Built in work packages P0.4 (the console harness in `CppSource/Oracle/`)
and P0.5 (the client instrumentation), and deleted with the C++ in P7.3.

| Part | What it produces |
| --- | --- |
| Console harness linked against the C++ DLLs | Vectors for exported functions: hashes, INI event streams, database dumps after load, codec bytes for given messages, save round trips, account names, scheduler traces, random-mission offers from a seed |
| `+oracle` mode in the C++ client | Interface-tree dumps per screen, parsed key map and layout files, screenshots at fixed resolution and time step, a log of calls into the client event interface for a scripted session |
| Logging proxy for the renderer interface | A frame's calls and buffers, replayable into the Rust renderer |
| Scenario recorder on the physics and AI facades | Per-tick positions, orientations and events for scripted scenarios |

Vector files are line-oriented text (one JSON object per line) with floats
written as hexadecimal bit patterns, so comparison never depends on decimal
formatting.

Synthetic inputs live in each crate's `tests/fixtures/oracle/`, written by
the scripts beside them (`make_fixtures.py` for P1.1 and P1.2,
`make_<module>_fixtures.py` for later suites). Each later suite has its own
`oracle_<module>.cpp` in the harness and `tests/oracle_<module>.rs` in the
crate. Tier B suites compare the committed values within the bound the
test states; where NaN payloads are not part of the port, both sides write
every NaN as one pattern. The harness and the crate's `oracle_vectors` test
render the same suites from them; the committed `tests/vectors/` files must
equal both. `OUTPOST_BLESS_VECTORS=1 cargo test --test oracle_vectors`
rewrites the files from the Rust port. That is only for adding a suite: the
oracle job then shows whether the C++ agrees.

## Where test data lives

| Kind | Location | Runs in CI |
| --- | --- | --- |
| Vectors from synthetic input (no game content) | Committed under each crate's `tests/vectors/` | Yes |
| Small synthetic fixtures written for the tests (hand-made UTF archives, INI files, meshes) | Committed under `tests/fixtures/` | Yes |
| The original `DATA` and `EXE` trees | Committed in `outdir/` (decision O15); tests take the folder from `OUTPOST_DATA_DIR` | Not yet |
| Dumps and vectors made from the original data | Produced when the test runs, never committed; the oracle's dumps in `OUTPOST_ORACLE_DIR` | Not yet |

Tests that need the last two are skipped with a printed notice when the
variables are unset. They run a package's full set and write a short result
file that is attached to the pull request closing the package; for P1.2
this is the `data_dump` test after `Oracle.exe dump`
([reference-build.md](reference-build.md) section 7). Since decision O15 the data is in the
repository, so CI's oracle job can run these dumps itself; until it does,
they run on the owner's machine.

## Test kinds

| Kind | Used for |
| --- | --- |
| Golden vector | Tier A and B functions against oracle output |
| Round trip | Codecs, save files, containers: decode, encode, compare bytes |
| Property | Parsers and codecs on generated input: no panic, round-trip identity |
| Cross-implementation | The loopback transport with forced encode and decode; the Python UTF walker against the Rust reader |
| Scenario | Headless simulation and server runs against envelopes |
| Trace replay | C++ renderer traces into the Rust renderer |
| Golden image | Synthetic scenes on the software adapter in CI; real scenes on the data machine |
| Soak | Dedicated server under scripted clients (P6.2) |

## CI

| Job | Target | Steps |
| --- | --- | --- |
| Windows (existing) | `x86_64-pc-windows-msvc` | format check, clippy with warnings denied, tests, release build of both executables |
| Host (added in P0.1) | the runner's native target | tests for `neuron-core`, `game-sim`, `neuron-server`, `game-logic` and the platform-neutral parts of the client crates |
| Policy (added in P0.1) | — | `cargo-deny`; a scan that fails on legacy API names, `asm!`, or `unsafe` outside the permitted modules |
| Oracle (added in P0.4) | `Win32` C++ build | builds `CppSource/Oracle`, runs it on the committed inputs and fails unless its output equals the committed vectors byte for byte; runs the `game-sim` mode on `crates/game-sim/tests/fixtures/oracle` and compares it with `crates/game-sim/tests/vectors` (P2.1); runs the data-dump mode on a synthetic installation; builds the eight resource DLLs and compares every string and card with the catalogs `cargo xtask catalog` builds (P1.6; that dump is game text and is not uploaded); removed in P7.3 |
| Data dumps (added for Gate G1) | `Win32` C++ build, original data | `.github/workflows/data.yml`, nightly, on pushes to `main` and on demand: `Oracle.exe dump` over `outdir/` (decision O15), then every `data_dump_<kind>` test compares the port with it. Kinds: `ini`, `utf`, `sur`, `anim`, `fx`, `texture`, `material`, `mesh`, `dfm`, `scene`, `scene_eval`; and for the game databases of P2.1, loaded from the `[Data]` list of `EXE/OutpostFrontier.ini` (`crates/game-sim/tests/data_dump.rs`): `constants`, `goods`, `loadouts`, `reputation`, `archetypes`, `universe`, `bases`, `fuses`. The dumps stay on the runner; only the summaries are kept |

## Definition of done for a work package

1. The scope listed in the plan is implemented with no placeholder code.
2. Tests at the assigned tier pass: committed vectors in CI, data-dependent
   vectors on a machine with the data, with the result file attached.
3. Each deliberate difference from the C++ has an entry in the divergence
   register.
4. Unknown reconstruction fields are carried as named unknowns, not
   interpreted.
5. The ledger rows and the port map are updated.
6. Format, lint and policy checks pass.

## Divergence register

`design/divergences.md`, created in P0.1. One row per deliberate
difference:

| Field | Content |
| --- | --- |
| ID | Sequential |
| Area | Crate and module |
| C++ behaviour | What the reference does, with file reference |
| Rust behaviour | What the port does |
| Reason | Undefined behaviour in the reference, legacy platform dependence, numeric policy, owner decision, or technical decision (citing its ID) |
| Observable effect | What a player, a save file or a peer could notice |

The owner reviews the register at every gate and signs it off at P7.1.

## Gates

| Gate | Evidence |
| --- | --- |
| G0 | The C++ build runs on the original data; first vector suites exist; CI has all three jobs |
| G1 | Every covered file in `DATA` loads; Rust dumps equal oracle dumps |
| G2 | Replay tests of integrator, AI and damage within Tier B; headless scenarios inside the envelopes approved in P2.0 (O20); all codecs match C++ bytes |
| G3 | Headless server session over QUIC; saves interchange both ways; mission and random-mission vectors equal |
| G4 | Viewer shows models, characters, particles, menu; trace replays match within the image threshold |
| G5 | Campaign playable to the end; interface-tree dumps equal for every screen; saves interchange |
| G6 | Multiplayer session with several clients; extras work and can be switched off |
| G7 | `CppSource/` deleted; legacy scan in CI; committed vectors remain as regression tests |
