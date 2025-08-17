# PowerShell build helper for MinGW (robust to current working directory)
# repoRoot is the parent directory of this scripts folder
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildDir = Join-Path $repoRoot "build"

if (-not (Test-Path -Path $buildDir)) {
    Write-Host "Configuring build directory with MinGW Makefiles in: $buildDir"
    cmake -S "$repoRoot" -B "$buildDir" -G "MinGW Makefiles"
}
Write-Host "Building (Release) in: $buildDir"
cmake --build "$buildDir" --config Release $args
