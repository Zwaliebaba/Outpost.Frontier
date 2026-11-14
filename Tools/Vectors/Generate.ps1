<#
.SYNOPSIS
  Regenerates Source/Tests/Vectors/Fixed64.tsv from the original's Fixed64 and Checksum
  (compiled from decompiledDir in local.json; see Program.cs).
#>
param([string] $Config)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$repo = (Resolve-Path (Join-Path $here '..\..')).Path
if (-not $Config) { $Config = Join-Path $repo 'local.json' }
$cfg = Get-Content $Config -Raw | ConvertFrom-Json
$out = Join-Path $repo 'Build\Vectors'
& dotnet build (Join-Path $here 'Vectors.csproj') -c Release -nologo -v quiet "-p:DecompiledDir=$($cfg.decompiledDir)" -o $out | Out-Host
if ($LASTEXITCODE) { Write-Host 'ERROR: building the vector generator' -ForegroundColor Red; exit 1 }
$target = Join-Path $repo 'Source\Tests\Vectors\Fixed64.tsv'
New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
& (Join-Path $out 'Outpost.Vectors.exe') $target
exit $LASTEXITCODE
