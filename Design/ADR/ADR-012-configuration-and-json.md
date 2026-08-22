# ADR-012 — Configuration: JSON Files Only, Custom Parser in NeuronCore

**Status:** Accepted · 2026-08-17 (owner directive) · §3 amended by
[ADR-018](ADR-018-scaling-baseline.md) (2026-08-19): the user layer's key families widen
(settings, wing names, route avoid-list) with unknown-key tolerance stated as forward-compat
(D15.5) · **§D13 cashed in by [ADR-024](ADR-024-mining-economy.md)** (2026-08-20): the
economy tables are the first hash-guarded balance content (`Economy.json` + `economyHash`);
the movement table stays compiled
**Depends on:** ADR-008 (hosting), ADR-009 (universe content)
**Supersedes:** ADR-008 §4 (the `--headless/--port/--transport/--selftest/--connect` command
line) and ADR-009 §7 (the line-oriented universe format) — both are replaced below.

## Context

Owner directive: **no environment variables, no argv parameters — configuration comes from
`.json` files only, parsed by a custom JSON parser in NeuronCore.** ADR-008 had specified a
command line; that decision is overturned here, and this ADR carries the replacement in full
so no reader is left reconciling two answers.

The directive is coherent with what the game already needs: the settings screen must persist
user choices to *something*, the universe is authored content that needs a format, and one
parser serving config + content + sound banks is less machinery than a parser plus a flag
grammar plus an ad-hoc content format.

## Decision

### A. No argv, no environment
1. `wWinMain`/`main` **ignore their arguments entirely**, and no code anywhere reads
   `GetEnvironmentVariable`/`getenv`. Every knob — hosting mode, port, transport, window,
   renderer, audio, universe path, self-test — comes from JSON.
2. **Resolution order (first hit wins, no merging between them):**
   1. `Outpost.json` in the **current working directory**;
   2. `Outpost.json` beside the executable (`GetModuleFileNameW` → replace filename).
   The working-directory rule is what replaces argv operationally: a dev or CI runner selects
   a configuration by choosing the directory it launches from, not by passing a flag.
3. **User layer for settings:** `Settings.json` under
   `SHGetKnownFolderPath(FOLDERID_LocalAppData)\Outpost.Frontier\` — a known-folder API, not
   an environment variable. It is optional, deep-merged over the base (user wins), and is the
   **only** file the game writes. It carries exactly the keys the settings screen owns
   (display, audio volumes, accessibility, input); anything else there is ignored with a
   warning. Base config stays read-only shipped content.

   > **Ruling, 2026-08-22 — per-player state is device-local, and the account service is the
   > named reopen trigger.** Three surfaces asked one question and it is answered once:
   > `settings.png` §3 (*"do settings live on the device, or on the account so a player's palette
   > follows them to a second tablet?"*), D-P5 (where a fleet template is stored) and the
   > strategic map's site layer (where the scouting journal persists). **All three are this
   > file.**
   >
   > It is the only answer this corpus can currently honour: [ADR-023](ADR-023-remote-play.md)
   > states outright that it *"does not design the account service"* and names it an external
   > dependency of the first remote milestone. Answering account-side would pull that whole phase
   > forward to settle a preferences question.
   >
   > **The reopen trigger is named rather than left to someone noticing**: the day an account
   > service exists, each family here is re-examined for whether it should follow the player, and
   > the honest expectation is that templates and the journal will and display preferences will
   > not. Recording it as a trigger is what stops "device-local" hardening into a rule nobody
   > chose.
   >
   > **The touch decision already shrank this.** Under
   > [ADR-020](ADR-020-ui-architecture.md)'s 2026-08-22 amendment **wings are the control
   > groups**, and a wing lives on the shard — so the grouping case, which looked like this
   > file's fourth family, never reaches it at all.

   > **Built, 2026-08-22 (N2) — the file is written, and one family is in it.**
   > "The only file the game writes" had no writer: `Settings.json` was found, parsed and merged
   > since S-whenever, and nothing had ever created one. `SaveUserSettings` does, and three
   > decisions came with it that this section did not previously make.
   >
   > **It writes what the player *changed*, not what they have.** `WriteUserLayer` takes the
   > configuration as loaded *and* the same configuration before the user layer went over it, and
   > emits only the keys that differ; a section with nothing changed in it is absent rather than
   > empty. The clause above says the file "carries exactly the keys the settings screen owns",
   > which is a statement about what it *may* contain — writing every one of them on the first
   > clean exit would pin a player to the shipped defaults of the day they installed the game,
   > through a file they never edited. An untouched installation therefore has no settings file
   > at all, which is the state these shipped defaults are meant to be read from.
   >
   > **It is atomic.** Written to a temporary beside the target and renamed over it, so a crash
   > mid-write leaves the previous settings whole instead of a truncated file the next boot
   > reports as corrupt.
   >
   > **§A4's "backed up beside itself" is now true.** A user layer that will not parse was warned
   > about and ignored, and never copied; it is copied to `Settings.json.bad` before the game
   > starts on the shipped values, because "ignored" was about to become "gone" at the first save.
   >
   > **Wing names are the only family in it so far**, and the layer's *shape* rather than its
   > coverage is what N2 delivered. The display, audio and diagnostics keys are written the moment
   > something changes them, and today nothing does: the settings screen is N3, and the F1
   > diagnostics strip — which this ADR's `DiagnosticsSettings` calls "a shortcut to the same bit,
   > not a second switch" — has no way to tell the composition root it was pressed. That
   > write-back is N3's to build, and it is named here so the claim is not read as already met.
   >
   > **The families this file therefore owns:** display, audio volumes, accessibility, input
   > bindings, wing *names* (ADR-017 **§6** — client-side because the shard has no name for a
   > wing; §6a.4 cites that rule rather than stating it, and this line pointed at the citation
   > until N2 went looking for the sentence), fleet templates, and the scouting journal.

4. **Failure posture:** base config missing or unparseable ⇒ **fatal**, with file, line, column
   and message in the log and a `MessageBoxW` in windowed mode. Unknown keys ⇒ warning
   (forward-compatibility with newer configs). **Type mismatches ⇒ fatal** — silent coercion
   is how configuration bugs hide. Missing optional keys ⇒ documented in-code defaults.
   Corrupt *user* layer ⇒ warning, ignored, backed up beside itself, defaults used.

### B. The config surface (replaces the command line one-for-one)

```jsonc
{
  "mode": "host",                       // "host" | "headless" | "client"   (was --headless / --connect)
  "selfTest": false,                    //                                   (was --selftest)
  "logging":  { "level": "info", "file": "Outpost.log" },
  "universe": { "definition": "GameData/Universe/Frontier.json" },
  "server": {
    "port": 7777,                       //                                   (was --port)
    "maxSessions": 8
  },
  "client": {
    "connect":  { "host": "127.0.0.1", "port": 7777 },
    "window":   { "width": 1600, "height": 900, "mode": "windowed" },
    "renderer": { "vsync": true, "msaa": 4, "frameCap": 0 },
    "camera":   { "zoomMetres": 8000, "yawSnapDegrees": 45 },
    "audio":    { "master": 1.0, "world": 1.0, "ui": 0.8, "music": 0.6, "alerts": 1.0, "ambience": 0.7 },
    "ui":       { "scale": 1.0, "palette": "default" }
  }
}
```

`mode` drives ADR-008's boot: `host` starts `ServerHost` then `ClientApp`; `headless` starts
only `ServerHost` (the standing proof the server has no client dependency); `client` skips
hosting and connects to `client.connect`. **Tick rate is deliberately absent** — it is a
balancing constant owned by ADR-002, not a deployment knob.

### C. The parser (`NeuronCore`, hand-rolled)
5. **Grammar:** RFC 8259, with two documented relaxations for hand-authored files —
   `//` and `/* */` comments, and trailing commas. The **writer emits strict JSON** (no
   comments, no trailing commas), so round-tripping a file through the settings screen
   produces something any JSON tool accepts. Rejected outright: `NaN`/`Infinity`, unquoted
   keys, single quotes, control characters in strings, and **duplicate keys** (fatal — last-one-
   wins is a config footgun, not a convenience).
6. **Shape:** a DOM parsed into a **flat node array with index handles** — no per-node
   allocation, no pointer chasing, no ownership subtleties; strings reference offsets into the
   source buffer and are unescaped into an arena on demand. Parsing is **iterative with an
   explicit stack and a depth cap (64)**: hostile or accidentally deep input must not smash
   the C++ stack.
7. **Numbers matter here.** The parser keeps **exact `int64`** for integral tokens (no
   fraction, no exponent) and `double` otherwise. This is load-bearing, not pedantry:
   ADR-009's universe coordinates are `int64` metres and exceed the 2⁵³ that a double-only
   parser preserves. Out-of-range integers are an error, not a silent truncation.
8. **Errors, not exceptions:** the parser never throws and never asserts on input — it is
   reading files humans edit. It returns a result plus a list of `(line, column, path,
   message)` diagnostics. Typed accessors (`GetInt64`, `GetDouble`, `GetBool`, `GetString`,
   `GetArray`, `GetObject`) take a default and record a diagnostic on missing/mismatched paths,
   so one pass reports *all* problems in a config rather than one per run.
9. **UTF-8 throughout**; `\uXXXX` escapes including surrogate pairs decode to UTF-8; a leading
   BOM is tolerated. A size cap is passed by the caller (16 MB for config; universe files set
   their own).
10. **Writer:** minimal and strict — insertion-ordered keys, two-space indent, deterministic
    output so a settings write produces a reviewable diff.
11. **Tests** (`NeuronCoreTests`): a valid/invalid corpus, `int64` exactness at and beyond
    2⁵³, depth-cap rejection, duplicate-key rejection, escape/surrogate decoding, comment and
    trailing-comma acceptance, writer round-trip stability.

### D. Content follows the same road
12. **The universe definition is JSON** (replacing ADR-009's line-oriented format), parsed by
    this parser inside GameLogic's pure `bytes → UniverseDef` function — NeuronCore is
    GameLogic's only library dependency, so this is in charter. The **`universeHash` is
    computed over the canonicalised parsed content**, not the raw bytes, so comments,
    whitespace, and key order never change it while real content changes always do.
13. The sound bank (ADR-011) uses the same parser. Compiled-in tables that are genuinely
    balancing constants (`ShipClassTable`, tick rate) stay in code; if any of them later
    becomes authored data, it adopts the same hash-guarded pattern. *(Adopted by
    [ADR-024](ADR-024-mining-economy.md), 2026-08-20: `Economy.json` and its `economyHash`
    are the first — the movement table stays compiled.)*

## Prior art worth reading first

The sibling repository **Outpost.Warzone** already has a hand-written `Neuron::Json`
(`NeuronCore/Json.{h,cpp}`) with a `std::expected<Json, Error> Parse(std::string_view)`
surface and a `Kind` enum. Read it before writing ours; it shares the naming convention
(AGENTS.md §1), so agreement is cheap. Two Frontier requirements to check it against before
adopting wholesale: **exact `int64`** (§C7 — universe coordinates need it, and an `AsInt()`
returning `int` does not provide it) and the **iterative parse with a depth cap** (§C6). If
those are missing, extend rather than fork, and consider pushing the improvement back to the
sibling.

## Alternatives rejected

- **A command line** — the previous decision; overturned by directive. It also had a real
  weakness: flags and the settings file would have been two sources of truth for overlapping
  knobs (window, transport), with precedence rules nobody enjoys.
- **Environment variables** — invisible state, per-shell divergence, and no place for the
  settings screen to write. Rejected by directive and on merit.
- **INI/TOML/XML** — INI has no nesting for the universe or sound banks; TOML is a bigger
  hand-written grammar than JSON; XML needs a heavier parser and reads worse. JSON is the
  directive and the right size.
- **A third-party JSON library** (nlohmann, RapidJSON, simdjson) — library policy; and the
  subset above is a contained, well-understood piece of work with an obvious test corpus.
- **Strict-RFC-only parsing (no comments)** — hand-authored config without comments loses the
  reason each value was chosen. The relaxations are read-side only, so nothing we *emit* is
  non-standard.

## Consequences

- **CI/self-test runs by directory, not by flag:** a runner `cd`s into a folder containing a
  self-test `Outpost.json` (`"mode": "headless", "selfTest": true`) and launches the exe.
  Documented in the build order; it is the one workflow that got slightly more ceremonious.
- Unit tests construct config structs directly in code — no file, no parser, no I/O — so the
  hosting and server suites stay fast and hermetic.
- The settings screen has a real home from day one, which closes the corpus's "where do
  settings live?" question with **device-local** for now (account-side sync is a post-MVP
  service concern).
- One parser is now load-bearing for config, universe content, and sound banks. It gets
  correspondingly serious tests (Risk R12).
- `ClientConfig`/`ServerConfig` remain plain structs assembled by the composition root
  (ADR-008 §3 unchanged) — libraries still never read files or know where config came from.
