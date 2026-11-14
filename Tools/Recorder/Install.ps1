<#
.SYNOPSIS
  Installs (or removes) BepInEx 5 and the Recorder plugin in the game installation (Design/Plan.md 1.1).

.DESCRIPTION
  The Recorder runs in the installation itself (gameDir in local.json): GOG Galaxy only knows the
  game at its registered folder, its peer (GalaxyPeer64.dll) crashes when a copy elsewhere calls
  it, and the game quits at startup without Galaxy. So nothing of the game is copied or changed;
  BepInEx's files are added beside it:
    winhttp.dll, doorstop_config.ini, .doorstop_version, changelog.txt, BepInEx\
  Install refuses if any of them already exists and is not BepInEx's; -Uninstall removes exactly
  them. To play without the Recorder but keep it installed, set enabled = false in
  doorstop_config.ini.

  Steps: BepInEx 5.4.23.6 (with HarmonyX), checked against its SHA-256; the plugin, built against
  the installation's assemblies; its output pointed at corpusDir\traces; BepInEx's entry point
  moved later (its default crashes Unity 5.2).

.EXAMPLE
  pwsh Tools\Recorder\Install.ps1
  pwsh Tools\Recorder\Install.ps1 -Uninstall
#>
param([string] $Config, [switch] $Uninstall)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repo = (Resolve-Path (Join-Path $here '..\..')).Path
if (-not $Config) { $Config = Join-Path $repo 'local.json' }

$BEPINEX_VERSION = '5.4.23.6'
$BEPINEX_URL = "https://github.com/BepInEx/BepInEx/releases/download/v$BEPINEX_VERSION/BepInEx_win_x64_$BEPINEX_VERSION.zip"
$BEPINEX_SHA256 = '6C57A714A892EFB4E9E1FEF63D608A3391F30EC984031C465404E4EE381A0755'
$ADDED = @('winhttp.dll', 'doorstop_config.ini', '.doorstop_version', 'changelog.txt', 'BepInEx')

function Say([string] $text) { Write-Host "== $text" -ForegroundColor Cyan }
function Fail([string] $text) { Write-Host "ERROR: $text" -ForegroundColor Red; exit 1 }

if (-not (Test-Path $Config)) { Fail "no ${Config}: run Tools\Extract\Extract.ps1 once, or create it (see Tools\Extract\README.md)" }
$cfg = Get-Content $Config -Raw | ConvertFrom-Json
$parent = Split-Path $repo -Parent
$game = $cfg.gameDir
$corpus = if ($cfg.PSObject.Properties['corpusDir'] -and $cfg.corpusDir) { $cfg.corpusDir } else { Join-Path $parent 'Outpost-Corpus' }
$tools = $cfg.toolsDir
if (-not $game -or -not (Test-Path (Join-Path $game 'DesertsOfKharak.exe'))) { Fail "gameDir in $Config does not hold DesertsOfKharak.exe" }

$ours = Test-Path (Join-Path $game 'BepInEx\plugins\Outpost.Recorder')

if ($Uninstall) {
  Say "remove BepInEx and the Recorder from $game"
  if (-not $ours) { Fail 'the Recorder is not installed there; nothing removed' }
  foreach ($name in $ADDED) {
    $path = Join-Path $game $name
    if (Test-Path $path) { Remove-Item -Recurse -Force $path; Write-Host "removed $name" }
  }
  Say 'done: the installation is as before'
  exit 0
}

# 1. BepInEx, only where nothing of the game would be overwritten.
Say "BepInEx $BEPINEX_VERSION -> $game"
if (-not $ours) {
  $clash = $ADDED | Where-Object { Test-Path (Join-Path $game $_) }
  if ($clash) { Fail "already in the installation, not BepInEx's from this script: $($clash -join ', '); nothing changed" }
  New-Item -ItemType Directory -Force $tools | Out-Null
  $zip = Join-Path $tools "BepInEx_win_x64_$BEPINEX_VERSION.zip"
  if (-not (Test-Path $zip)) { Invoke-WebRequest $BEPINEX_URL -OutFile $zip }
  $hash = (Get-FileHash $zip -Algorithm SHA256).Hash
  if ($hash -ne $BEPINEX_SHA256) { Remove-Item $zip; Fail "BepInEx download has SHA-256 $hash, expected $BEPINEX_SHA256" }
  $staging = Join-Path ([IO.Path]::GetTempPath()) "outpost-bepinex-$PID"
  Expand-Archive $zip -DestinationPath $staging -Force
  $unexpected = Get-ChildItem $staging -Force | Where-Object { $_.Name -notin $ADDED }
  if ($unexpected) { Remove-Item -Recurse -Force $staging; Fail "the BepInEx archive holds files this script does not know: $($unexpected.Name -join ', ')" }
  Get-ChildItem $staging -Force | Move-Item -Destination $game
  Remove-Item -Recurse -Force $staging
} else {
  Write-Host 'already installed by this script'
}

# 2. The plugin.
Say 'build Outpost.Recorder'
$out = Join-Path $repo 'Build\Recorder'
& dotnet build (Join-Path $here 'Recorder.csproj') -c Release -nologo -v quiet "-p:GameDir=$game" -o $out | Out-Host
if ($LASTEXITCODE) { Fail 'building the Recorder' }
$plugins = Join-Path $game 'BepInEx\plugins\Outpost.Recorder'
New-Item -ItemType Directory -Force $plugins | Out-Null
Copy-Item -Force (Join-Path $out 'Outpost.Recorder.dll') $plugins

# 3. Configuration.
$traces = Join-Path $corpus 'traces'
$configDir = Join-Path $game 'BepInEx\config'
New-Item -ItemType Directory -Force $traces, $configDir | Out-Null
@"
[Recorder]

## Folder for the trace files (one per game session).
# Setting type: String
OutputDirectory = $traces

## Answer the GOG Galaxy language query locally (only needed when the game runs outside its registered folder).
# Setting type: Boolean
SkipGalaxyLanguageQuery = false
"@ | Set-Content -Encoding utf8 (Join-Path $configDir 'outpost.frontier.recorder.cfg')
# BepInEx's default entry point (UnityEngine.Application's static constructor) is too early for
# Unity 5.2: the chainloader's AddComponent crashes Mono (mono_class_is_subclass_of). The static
# constructor of MonoBehaviour runs once the game creates its first script, which is late enough.
# BepInEx fills in its other settings on the next start and keeps these.
@"
[Preloader.Entrypoint]

## The local filename of the assembly to target.
# Setting type: String
Assembly = UnityEngine.dll

## The name of the type in the entrypoint assembly to search for the entrypoint method.
# Setting type: String
Type = MonoBehaviour

## The name of the method in the specified entrypoint assembly and type to hook and load Chainloader from.
# Setting type: String
Method = .cctor
"@ | Set-Content -Encoding utf8 (Join-Path $configDir 'BepInEx.cfg')

Say 'done'
Write-Host "Start the game as always (GOG Galaxy or $(Join-Path $game 'DesertsOfKharak.exe'))."
Write-Host "Traces: $traces    BepInEx log: $(Join-Path $game 'BepInEx\LogOutput.log')"
Write-Host "Recorder off: enabled = false in doorstop_config.ini; removed: Install.ps1 -Uninstall"
