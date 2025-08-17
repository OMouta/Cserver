param(
    [switch]$WithPhp,
    [switch]$WithLogs,
    [switch]$Foreground,
    [int]$Port = 8080,
    [string]$ServeDir = "public",
    [string]$PhpCgiPath = "D:\IDE's\php-8.4.11-Win32-vs17-x64\php-cgi.exe",
    [string]$LogFile = "server.log"
)

# Prefer CMake build output: build/Release/Cserver.exe
        # Determine repo root (parent of scripts directory)
        $repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
        # Prefer CMake build output: build/Release/Cserver.exe
        $exe = Join-Path -Path $repoRoot -ChildPath "build\Release\Cserver.exe"
        if (-not (Test-Path $exe)) {
            $exe = Join-Path -Path $repoRoot -ChildPath "build\Cserver.exe"
        }
        if (-not (Test-Path $exe)) {
            # Fallback to legacy server.exe in repo root
            $exe = Join-Path -Path $repoRoot -ChildPath "server.exe"
        }

if (-not (Test-Path $exe)) {
    Write-Error "server executable not found. Build first or set correct path to server.exe"
    exit 1
}

# Build an explicit argument list to avoid quoting/parsing issues
$argList = @()
$argList += '--serve'
$argList += $ServeDir
$argList += '--port'
$argList += $Port.ToString()
if ($WithPhp) {
    $argList += '--php-cgi'
    $argList += $PhpCgiPath
}
if ($WithLogs) {
    $argList += '--log-file'
    $argList += $LogFile
}

Write-Host "Starting server: $exe $($argList -join ' ')"
# If user did not explicitly provide the -Foreground switch, default to foreground so interactive runs stay attached
if (-not $PSBoundParameters.ContainsKey('Foreground')) { $Foreground = $true }

if ($Foreground) {
    # Run in foreground so output/logs remain attached to this terminal
    & "$exe" @argList
    exit $LASTEXITCODE
} else {
    # Launch in background but ensure the working directory is the repo root so relative paths resolve as expected
    Start-Process -FilePath $exe -ArgumentList $argList -WorkingDirectory $repoRoot -NoNewWindow
}
