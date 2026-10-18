[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [string]$Generator = 'Visual Studio 17 2022',
    [ValidateRange(1, 128)]
    [int]$Jobs = [Math]::Max(1, [Environment]::ProcessorCount),
    [switch]$ConfigureOnly,
    [switch]$WithSetup
)

$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildRoot = Join-Path $repoRoot 'build-private'
$cmake = (Get-Command cmake -ErrorAction Stop).Source
$databaseKey = [Environment]::GetEnvironmentVariable('GNSS_DOWNLOADER_DB_KEY', 'Process')

if ($databaseKey -and $databaseKey -notmatch '^[A-Za-z0-9_+/=.-]{16,}$') {
    throw 'Use a database key of at least 16 characters, containing letters, digits, _, +, /, =, ., or -.'
}
if (-not $databaseKey) {
    $databaseKey = ''
    Write-Warning 'No database key supplied. This build cannot open the official encrypted knowledge bases; it is suitable for compiling the public source.'
}
if ($WithSetup -and $databaseKey -and $databaseKey -notmatch '^[0-9A-Fa-f]{64}$') {
    throw 'The setup package requires a key containing 64 hexadecimal characters.'
}

New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
$privateInitialCache = Join-Path $buildRoot 'private-initial-cache.cmake'
$cacheLines = @(
    ('set(GNSS_DOWNLOADER_DB_KEY "{0}" CACHE STRING "Private database key" FORCE)' -f $databaseKey),
    'set(GNSS_DOWNLOADER_REQUIRE_ENCRYPTED_DB ON CACHE BOOL "Require encrypted DuckDB" FORCE)'
)
[IO.File]::WriteAllLines($privateInitialCache, $cacheLines, [Text.UTF8Encoding]::new($false))

try {
    & $cmake -S $repoRoot -B $buildRoot -G $Generator -A x64 -C $privateInitialCache
    if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
} finally {
    Remove-Item -LiteralPath $privateInitialCache -Force -ErrorAction SilentlyContinue
    $databaseKey = $null
    $cacheLines = $null
}

if ($ConfigureOnly) {
    Write-Host 'Configuration completed.'
    return
}

$targets = @('GNSSDownloaderDuckDB')
if ($WithSetup) { $targets += 'MARKSetup' }
& $cmake --build $buildRoot --config $Configuration --target $targets --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'MARK build failed.' }

$application = Join-Path $buildRoot "$Configuration\MARK.exe"
if (-not (Test-Path -LiteralPath $application -PathType Leaf)) {
    throw 'Expected executable was not produced.'
}
Write-Host "Built application: $application"
Write-Host 'The build directory is private and ignored by Git. It may contain your key in CMake and compiler output.'
