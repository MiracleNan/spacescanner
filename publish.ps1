param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',

    [string]$Runtime = 'win-x64',

    [ValidateSet('self-contained', 'framework-dependent')]
    [string]$Deployment = 'framework-dependent'
)

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$project = Join-Path $repoRoot 'SpaceScannerUI\spacescanner.csproj'
$publishDir = Join-Path $repoRoot "SpaceScannerUI\publish\$Runtime"

if (-not (Test-Path $project)) {
    throw "Project not found: $project"
}

if (Test-Path $publishDir) {
    Remove-Item -Recurse -Force $publishDir
}

$selfContainedArg = if ($Deployment -eq 'self-contained') { 'true' } else { 'false' }
$publishSingleFile = 'true'
$includeNativeForExtract = if ($Deployment -eq 'self-contained') { 'true' } else { 'false' }
$enableCompression = if ($Deployment -eq 'self-contained') { 'true' } else { 'false' }

$args = @(
    'publish', $project,
    '-c', $Configuration,
    '-r', $Runtime,
    '--self-contained', $selfContainedArg,
    '-o', $publishDir,
    "/p:PublishSingleFile=$publishSingleFile",
    "/p:IncludeNativeLibrariesForSelfExtract=$includeNativeForExtract",
    "/p:EnableCompressionInSingleFile=$enableCompression",
    '/p:PublishReadyToRun=false',
    '/p:DebugType=None',
    '/p:DebugSymbols=false'
)

dotnet @args

if ($LASTEXITCODE -ne 0) {
    throw "Publish failed with exit code $LASTEXITCODE"
}

$exePath = Join-Path $publishDir 'spacescanner.exe'
if (-not (Test-Path $exePath)) {
    throw "Publish succeeded but executable not found: $exePath"
}

Write-Host "Publish succeeded. Mode: $Deployment"
if ($Deployment -eq 'framework-dependent') {
    Write-Host 'Framework-dependent single-file publish completed.'
    Write-Host 'Target machine must have .NET Desktop Runtime 8 installed.'
}
Write-Host "Entry executable: $exePath"
