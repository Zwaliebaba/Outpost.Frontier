# The pinned MSVC toolset of CppSource builds on CI (.github/msvc-toolset.txt). Writes the
# MSBuild arguments that select it to the pipeline; exits 1 when the runner image lacks it.
#
#   $pin = & .github\scripts\msvc-pin.ps1
#   & $msbuild CppSource\OutpostFrontier.sln ... @pin

$ErrorActionPreference = 'Stop'
$file = Join-Path $PSScriptRoot '..\msvc-toolset.txt'
$settings = @{}
foreach ($line in Get-Content $file) {
    $text = ($line -split '#')[0].Trim()
    if ($text) {
        $key, $value = $text -split '\s+', 2
        $settings[$key] = $value
    }
}
$toolset = $settings['platform-toolset']
$tools = $settings['vc-tools']
if (-not $toolset -or -not $tools) {
    Write-Error "$file must name platform-toolset and vc-tools"
    exit 1
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -property installationPath
$installed = @(Get-ChildItem "$vs\VC\Tools\MSVC" -Directory | ForEach-Object Name)
$default = Get-Content "$vs\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt" -ErrorAction SilentlyContinue
Write-Host "MSVC pin: $toolset $tools (image $env:ImageOS $env:ImageVersion, default $default, installed: $($installed -join ', '))"
if ($installed -notcontains $tools) {
    Write-Host "::error::The runner image lacks the pinned MSVC toolset $tools; bump .github/msvc-toolset.txt to an installed one ($($installed -join ', '))"
    exit 1
}
"/p:PlatformToolset=$toolset"
"/p:VCToolsVersion=$tools"
