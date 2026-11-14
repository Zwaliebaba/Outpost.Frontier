# Tools/Recorder: ground truth from the original game

A BepInEx 5 plugin for the original Deserts of Kharak (Design/Plan.md 1.1). Per simulation tick
(8 per second) it writes the commands applied, the checksum the simulation computed, the checksum
a replay expected, and the scheduler's verdict. Those traces are what the C++ simulation is
checked against (Design/Verification.md §1).

## What the original already does

`SimScheduler.AdvanceSimFrame` writes every tick's checksum into the replay while a game is
recorded, and compares it when the replay is played back: a difference is a `SyncError`. So every
`.dokreplay` (compressed LitJson: a header, then frames with commands and checksums; saved in the
installation's `out\replays\`) is itself a golden trace. The Recorder makes that visible, per
tick, in a form the C++ side can read. **Skirmish and multiplayer games record replays; the
campaign does not.** And a replay stores checksums **only in frames with commands** (11 of 998 in
the first test), so the game's own check is sparse; the Recorder's traces have every tick.

First result (2026-11-14): a skirmish on MP_05-Teeth, 998 ticks, and two playbacks of its replay:
identical checksums at every tick, no `SyncError`.

## Install

```powershell
pwsh Tools\Recorder\Install.ps1              # into gameDir (local.json)
pwsh Tools\Recorder\Install.ps1 -Uninstall   # removes exactly what it added
```

It runs **in the installation itself**. It adds BepInEx 5.4.23.6 with HarmonyX (checked against
its SHA-256) beside the game, and nothing else: `winhttp.dll`, `doorstop_config.ini`,
`.doorstop_version`, `changelog.txt`, `BepInEx\`; it refuses if any of these already exists. The
plugin is built against the installation's assemblies and sends traces to `corpusDir\traces`.
To play without it but keep it installed, set `enabled = false` in `doorstop_config.ini`.

Two things found on the way (2026-11-14):
- BepInEx's default entry point is too early for Unity 5.2 (Mono crashes in the chainloader's
  first `AddComponent`); the installer sets it to `MonoBehaviour`'s static constructor.
- **Start the game from the GOG Galaxy client.** Started directly from `DesertsOfKharak.exe`, the
  game crashes in GOG Galaxy's peer (`GalaxyPeer64.dll`, right after reading `settings.json`),
  with or without BepInEx, and in a copy elsewhere as well; without Galaxy it quits at startup
  (`EnableSteam`'s DLC query). The plugin's `SkipGalaxyLanguageQuery` setting is off and stays
  off: it did not help.

## Recording

1. Start the game from GOG Galaxy.
2. Play. Each game session becomes one trace, `<corpusDir>\traces\<date>-<time>-live.tsv`.
3. For a determinism check: a skirmish, ended properly so its replay is written, then that replay
   played back (main menu → Replay), which becomes `...-replay.tsv` with the recorded checksums in
   `expected`.

## Checking

```powershell
py -3 Tools\Recorder\compare_traces.py <live.tsv> <replay.tsv> [<replay2.tsv> ...]
```

reports every frame where a replay's computed checksum differs from the recorded one (or the game
reported `SyncError`), and the first frame where two traces differ. It ignores `NoOpCommand`, the
end marker a replay carries at its last frame.

## Trace format

UTF-8, tab-separated, one line per tick; `#` lines are metadata (version, start time, mode, scene).

| Column | |
|---|---|
| `frame` | `SimFrameNumber.FrameNumber` |
| `checksum` | `Checksum.Sum` returned by `Sim.Tick` |
| `expected` | in a replay, the checksum recorded for the frame; `-` otherwise |
| `result` | `SchedulerResultType` of the tick (`OK`, `SyncError`, ...) |
| `mode` | `live` or `replay` |
| `commands` | `Type:checksum` per command (`SimCommandBase.GetChecksum()`), `;`-separated; `-` for none |
