<#
.SYNOPSIS
  Rebuilds the decompiled code (DoK-Decompiled) and the extracted assets (DoK-Assets) from a local
  installation of Homeworld: Deserts of Kharak. See README.md beside this script.

.DESCRIPTION
  Steps, in order (each can be run on its own with -Steps):
    tools      install the pinned tools into toolsDir: ilspycmd 11.2.0.10050 and
               AssetStudioModCLI 0.20.0 (download checked against its SHA-256)
    decompile  the game's assemblies to C# projects in decompiledDir         (~1 min)
    export     every asset and every object, grouped by source file           (~10 min)
    organize   arranged per scene, with manifest.json                          (~8 min)
    fbx        the animated objects as FBX models under Models\                (~8 min)

  Paths come from local.json at the repository root (git-ignored); the first run writes it with
  defaults and stops if gameDir is not set.

.EXAMPLE
  pwsh Tools\Extract\Extract.ps1
  pwsh Tools\Extract\Extract.ps1 -Steps fbx
#>
param(
  [ValidateSet('all', 'tools', 'decompile', 'export', 'organize', 'fbx')]
  [string[]] $Steps = @('all'),
  [string] $Config,
  [switch] $Force
)

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repo = (Resolve-Path (Join-Path $here '..\..')).Path
if (-not $Config) { $Config = Join-Path $repo 'local.json' }

$ILSPY_VERSION = '11.2.0.10050'
$ASSETSTUDIO_VERSION = '0.20.0'
$ASSETSTUDIO_URL = "https://github.com/aelurum/AssetStudio/releases/download/v$ASSETSTUDIO_VERSION/AssetStudioModCLI_net10_win64.zip"
$ASSETSTUDIO_SHA256 = '0CAFF1E9D381083324F65241F55171768CD08EFFE626B0125B63497CD2510CC7'
$BUILD_NAME = 'DisjointedFalafel'

# The game's own assemblies; the middleware ones are Plan action 0.5.
$ASSEMBLIES = @('Assembly-CSharp-firstpass', 'Assembly-UnityScript', 'BBI.Core', 'BBI.Debugger', 'BBI.Game.Data',
                'BBI.Game', 'BBI.Platform', 'BBI.Spark', 'BBI.Steam', 'BBI.Unity.Core.Data', 'BBI.Unity.Core',
                'BBI.Unity.Game.Data', 'BBI.Unity.Game', 'GOG.GalaxyWrapper', 'Assembly-CSharp')

function Step([string] $name) { $Steps -contains 'all' -or $Steps -contains $name }
function Say([string] $text) { Write-Host "== $text" -ForegroundColor Cyan }
function Fail([string] $text) { Write-Host "ERROR: $text" -ForegroundColor Red; exit 1 }

# --- configuration -------------------------------------------------------------------------------

if (-not (Test-Path $Config)) {
  $parent = Split-Path $repo -Parent
  [ordered]@{
    gameDir       = ''
    decompiledDir = (Join-Path $parent 'DoK-Decompiled')
    assetsDir     = (Join-Path $parent 'DoK-Assets')
    toolsDir      = (Join-Path $env:LOCALAPPDATA 'Outpost.Frontier\Tools')
  } | ConvertTo-Json | Set-Content -Encoding utf8 $Config
  Write-Host "Wrote $Config with defaults."
}
$cfg = Get-Content $Config -Raw | ConvertFrom-Json
if (-not $cfg.gameDir) { Fail "set gameDir in $Config to the folder holding DesertsOfKharak.exe" }
$game = $cfg.gameDir
$data = Join-Path $game 'DesertsOfKharak_Data'
$managed = Join-Path $data 'Managed'
$assets = $cfg.assetsDir
$decompiled = $cfg.decompiledDir
$tools = $cfg.toolsDir

if (-not (Test-Path (Join-Path $data 'mainData'))) { Fail "no DesertsOfKharak_Data\mainData under $game" }
$build = Get-Content (Join-Path $data 'buildinfo.txt') -ErrorAction SilentlyContinue | Where-Object { $_ -like 'BuildName=*' }
if ($build -ne "BuildName=$BUILD_NAME") {
  Write-Warning "this is not build $BUILD_NAME ($build); the extraction was made for that build (GOG, 2018-05-25)"
}

# Python: the py launcher, else python on PATH; it must actually start (a broken install may not).
$py = $null
foreach ($candidate in @(@('py', '-3'), @('python'))) {
  try {
    $exe, $pre = $candidate[0], @($candidate | Select-Object -Skip 1)
    $v = & $exe @pre -c 'import sys; print(sys.version_info >= (3, 9))' 2>$null
    if ($LASTEXITCODE -eq 0 -and $v -eq 'True') { $py = $candidate; break }
  } catch { }
}
if (-not $py) { Fail 'no working Python 3.9 or later (tried "py -3" and "python")' }
function Python { $exe, $pre = $py[0], @($py | Select-Object -Skip 1); & $exe @pre -I @args; if ($LASTEXITCODE) { Fail "python $args" } }

$ilspyDir = Join-Path $tools "ilspycmd-$ILSPY_VERSION"
function IlspyPath {
  # The .NET tool shim is ilspycmd.exe or ilspycmd.cmd, depending on the SDK.
  foreach ($name in 'ilspycmd.exe', 'ilspycmd.cmd') {
    $p = Join-Path $ilspyDir $name
    if (Test-Path $p) { return $p }
  }
  return Join-Path $ilspyDir 'ilspycmd.exe'
}
$ilspy = IlspyPath
$cli = Join-Path $tools "AssetStudioModCLI-$ASSETSTUDIO_VERSION\AssetStudioModCLI.exe"
$logs = Join-Path $assets '_logs'

# --- tools ---------------------------------------------------------------------------------------

if (Step 'tools') {
  Say 'tools'
  if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) { Fail 'the .NET SDK is needed (dotnet), with the .NET 10 runtime' }
  if (-not ((dotnet --list-runtimes) -match 'Microsoft\.NETCore\.App 10\.')) { Fail 'AssetStudioModCLI needs the .NET 10 runtime' }
  if (-not (Test-Path $ilspy)) {
    & dotnet tool install ilspycmd --version $ILSPY_VERSION --tool-path $ilspyDir | Out-Host
    if ($LASTEXITCODE) { Fail 'installing ilspycmd' }
    $script:ilspy = IlspyPath
  }
  if (-not (Test-Path $cli)) {
    $zip = Join-Path $tools "AssetStudioModCLI-$ASSETSTUDIO_VERSION.zip"
    New-Item -ItemType Directory -Force $tools | Out-Null
    Invoke-WebRequest $ASSETSTUDIO_URL -OutFile $zip
    $hash = (Get-FileHash $zip -Algorithm SHA256).Hash
    if ($hash -ne $ASSETSTUDIO_SHA256) { Remove-Item $zip; Fail "AssetStudioModCLI download has SHA-256 $hash, expected $ASSETSTUDIO_SHA256" }
    Expand-Archive $zip -DestinationPath (Split-Path $cli) -Force
    Remove-Item $zip
  }
  Write-Host "ilspycmd: $(& $ilspy --version | Select-Object -First 1)"
  Write-Host "AssetStudioModCLI $ASSETSTUDIO_VERSION, Python $(& $py[0] @($py | Select-Object -Skip 1) -c 'import sys; print(sys.version.split()[0])')"
}
if (-not (Test-Path $ilspy) -or -not (Test-Path $cli)) { Fail 'tools missing: run with -Steps tools' }

# --- decompile -----------------------------------------------------------------------------------

if (Step 'decompile') {
  Say "decompile -> $decompiled"
  if ((Test-Path $decompiled) -and (Get-ChildItem $decompiled -Force | Select-Object -First 1) -and -not $Force) {
    Fail "$decompiled is not empty (use -Force to decompile over it)"
  }
  New-Item -ItemType Directory -Force $decompiled | Out-Null
  $jobs = foreach ($n in $ASSEMBLIES) {
    Start-Job -ArgumentList $ilspy, $managed, $decompiled, $n -ScriptBlock {
      param($ilspy, $managed, $out, $n)
      $dir = Join-Path $out $n
      $o = & $ilspy -p -o $dir -r $managed (Join-Path $managed "$n.dll") 2>&1
      [pscustomobject]@{ Name = $n; Exit = $LASTEXITCODE; Files = @(Get-ChildItem $dir -Recurse -Filter *.cs).Count; Tail = ($o | Select-Object -Last 2) -join ' | ' }
    }
  }
  $results = $jobs | Wait-Job | Receive-Job
  $jobs | Remove-Job
  $results | Sort-Object Name | Format-Table Name, Exit, Files -AutoSize | Out-Host
  if ($results | Where-Object { $_.Exit -ne 0 }) { Fail 'a decompilation failed' }
}

# --- export --------------------------------------------------------------------------------------

$common = @('-g', 'fileName', '-f', 'assetName_pathID', '--assembly-folder', $managed, '--export-asset-list', 'xml')
if (Step 'export') {
  Say "export -> $assets"
  if ((Test-Path (Join-Path $assets 'manifest.json')) -and -not $Force) { Fail "$assets already holds an extraction (use -Force to redo it)" }
  if ($Force -and (Test-Path $assets)) { Remove-Item -Recurse -Force $assets }
  $raw = Join-Path $assets '_raw'
  New-Item -ItemType Directory -Force $raw, $logs | Out-Null
  & $cli $data -m export @common -o (Join-Path $raw 'content') *> (Join-Path $logs 'content.log')
  if ($LASTEXITCODE) { Fail 'asset export (see _logs\content.log)' }
  & $cli $data -m jsonDump --load-all @common -o (Join-Path $raw 'objects') *> (Join-Path $logs 'objects.log')
  if ($LASTEXITCODE) { Fail 'object dump (see _logs\objects.log)' }
}

# --- organize ------------------------------------------------------------------------------------

if (Step 'organize') {
  Say 'organize'
  $raw = Join-Path $assets '_raw'
  if (-not (Test-Path $raw)) { Fail "nothing to organize in $raw (run -Steps export first)" }
  Python (Join-Path $here 'organize.py') $data $assets *>&1 | Tee-Object (Join-Path $logs 'organize.log') | Out-Host
  $left = @(Get-ChildItem (Join-Path $raw 'content') -Recurse -File | Where-Object Name -ne 'assets.xml').Count
  if ($left) { Fail "$left exported files were not arranged; _raw is kept" }
  Move-Item -Force (Join-Path $raw 'content\assets.xml') (Join-Path $logs 'content-assets.xml')
  Move-Item -Force (Join-Path $raw 'objects\assets.xml') (Join-Path $logs 'objects-assets.xml')
  Remove-Item -Recurse -Force $raw
  Copy-Item -Force (Join-Path $here 'AssetsReadme.md') (Join-Path $assets 'README.md')
}

# --- fbx -----------------------------------------------------------------------------------------

if (Step 'fbx') {
  Say 'fbx'
  if (-not (Test-Path (Join-Path $assets 'manifest.json'))) { Fail 'run -Steps export,organize first' }
  if (Test-Path (Join-Path $assets 'Models')) {
    if (-not $Force) { Fail "$assets\Models exists (use -Force to redo it)" }
    Remove-Item -Recurse -Force (Join-Path $assets 'Models')
  }
  Python (Join-Path $here 'select_animated.py') $assets | Out-Host
  $ids = (Get-Content (Join-Path $logs 'fbx-pathids.txt') -Raw).Trim()
  & $cli $data -m animator --filter-by-pathid $ids --fbx-animation auto --assembly-folder $managed `
    -o (Join-Path $assets '_raw_fbx') *> (Join-Path $logs 'fbx.log')
  if ($LASTEXITCODE) { Fail 'FBX export (see _logs\fbx.log)' }
  Python (Join-Path $here 'organize_fbx.py') $assets *>&1 | Tee-Object (Join-Path $logs 'organize_fbx.log') | Out-Host
}

Say 'done'
