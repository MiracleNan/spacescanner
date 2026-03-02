param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [ValidateSet('x64', 'x86')]
    [string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$solution = Join-Path $repoRoot 'SpacescannerPro.sln'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'

if (-not (Test-Path $solution)) {
    throw "Solution not found: $solution"
}

if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Please install Visual Studio 2022 (Desktop development with C++ + .NET desktop)."
}

$msbuildPath = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuildPath) {
    throw 'MSBuild.exe not found via vswhere.'
}

Write-Host "Using MSBuild: $msbuildPath"
& $msbuildPath $solution /m /restore /p:Configuration=$Configuration /p:Platform=$Platform
if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

$uiExe = Join-Path $repoRoot "SpaceScannerUI\bin\$Configuration\net8.0-windows\spacescanner.exe"
if (-not (Test-Path $uiExe)) {
    throw "Build succeeded but app executable was not found: $uiExe"
}

Write-Host "Build succeeded. App: $uiExe"
