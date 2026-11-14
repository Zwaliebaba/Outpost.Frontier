# Tools/Extract: the inputs, rebuilt from a game installation

The rebuild works from two local folders that never go into this repository, because they hold
the original game's code and data ([Design/README.md](../../Design/README.md), legal position):

| Folder | Contents | Size |
|---|---|---|
| `DoK-Decompiled` | the game's 15 assemblies as C# projects (ILSpy); in `Middleware\`, the 13 middleware assemblies the rebuilt runtimes must reproduce (Decisions D13) | 65 MB, 4,846 files + 872 in `Middleware\` |
| `DoK-Assets` | every asset and object, arranged per scene, with `manifest.json`; the asset bundles (`Bundles\`); the animated models as FBX; its own `README.md` explains the layout | 26 GB, ~175,000 files |

`Extract.ps1` makes both on any machine with the game installed, in about 30 minutes.

## Requirements

- Homeworld: Deserts of Kharak installed (made for the GOG build "DisjointedFalafel" of
  2018-05-25; another build gets a warning)
- PowerShell 7, Python 3.9 or later (found through `py -3` or `python`)
- the .NET SDK with the .NET 10 runtime (for ilspycmd and AssetStudioModCLI)
- about 30 GB of free disk space, and internet access the first time (tools download)

## Use

```powershell
pwsh Tools\Extract\Extract.ps1          # first run writes local.json and asks for gameDir
# edit local.json: "gameDir": "D:\\Games\\Homeworld Deserts of Kharak"
pwsh Tools\Extract\Extract.ps1          # all steps
pwsh Tools\Extract\Extract.ps1 -Steps fbx -Force   # redo one step
pwsh Tools\Extract\Extract.ps1 -Steps tools,middleware
```

`local.json` (repository root, git-ignored) holds the paths; by default the outputs go beside the
repository and the tools into `%LOCALAPPDATA%\Outpost.Frontier\Tools`:

```json
{
  "gameDir": "C:\\GOG Games\\Homeworld Deserts of Kharak",
  "decompiledDir": "C:\\src\\DoK-Decompiled",
  "assetsDir": "C:\\src\\DoK-Assets",
  "toolsDir": "C:\\Users\\me\\AppData\\Local\\Outpost.Frontier\\Tools",
  "contentDir": "C:\\src\\Outpost-Content",
  "corpusDir": "C:\\src\\Outpost-Corpus"
}
```

`contentDir` (cooked content) and `corpusDir` (the golden corpus) are for the tools of Plan
phases 1 and 2; `Extract.ps1` does not use them.

## Steps

| Step | Does | Tool |
|---|---|---|
| `tools` | installs ilspycmd 11.2.0.10050 (as a local .NET tool) and AssetStudioModCLI 0.20.0 (download checked against its SHA-256) | |
| `decompile` | the game's assemblies to C# projects | ilspycmd |
| `middleware` | NGUI, `Fabric.*` (4), CinemaDirector, Cinemachine, PlayMaker, `uScriptRuntime`, LeanTween, `UnityEngine.UI`, `UnityScript.Lang`, `Boo.Lang` to C# projects in `Middleware\` | ilspycmd |
| `export` | every asset converted (PNG, OBJ, WAV, OGV, shaders, text, MonoBehaviour JSON) and every object dumped as JSON, grouped by source file, into `_raw\` | AssetStudioModCLI |
| `organize` | per scene: `Scenes\<scene path>\{scene,shared,GI}\`, `Common\`; objects merged into `objects\<Type>.jsonl`; `manifest.json` with each file's dependency table | `organize.py`, `unity_externals.py` |
| `bundles` | the 48 asset bundles in the installation's `Bundles\` (front end, loading screens, voice-over, DLC), one export per bundle, arranged under `Bundles\<bundle>\` with `containers.json` (the assets' paths in the original Unity project); `manifest.json` gains `bundles` | AssetStudioModCLI, `organize_bundles.py` |
| `fbx` | chooses the animated objects (`select_animated.py`), exports them as FBX, arranges them under `Models\` with texture copies hard-linked | AssetStudioModCLI, `organize_fbx.py` |

The tools are pinned so two machines produce the same folders. A step refuses to overwrite an
existing result unless `-Force` is given; with `-Force`, `export` starts the assets folder over.
