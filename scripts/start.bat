@echo off
REM Start script (cmd) with optional php and logging presets
setlocal enabledelayedexpansion
set PORT=8080
set SERVE=public
set PHPPATH=D:\IDE's\php-8.4.11-Win32-vs17-x64\php-cgi.exe
set LOG=server.log
:set FOREGROUND=0
:set ARGS=--serve "%SERVE%" --port %PORT%
:parse
if "%1"=="" goto run
if /I "%1"=="/php" set ARGS=%ARGS% --php-cgi "%PHPPATH%"
if /I "%1"=="/logs" set ARGS=%ARGS% --log-file "%LOG%"
if /I "%1"=="/foreground" set FOREGROUND=1
shift & goto parse
:run
 set SCRIPT_DIR=%~dp0
 set REPO_ROOT=%SCRIPT_DIR%..\
 if exist "%REPO_ROOT%build\Release\Cserver.exe" (
     set EXE=%REPO_ROOT%build\Release\Cserver.exe
 ) else if exist "%REPO_ROOT%build\Cserver.exe" (
     set EXE=%REPO_ROOT%build\Cserver.exe
 ) else if exist "%REPO_ROOT%server.exe" (
     set EXE=%REPO_ROOT%server.exe
 ) else (
     set EXE=
 )
if "%EXE%"=="" (
    echo server executable not found. Build first.
    exit /b 1
)

echo Starting %EXE% %ARGS%
if "%FOREGROUND%"=="1" (
    "%EXE%" %ARGS%
) else (
    start "Cserver" "%EXE%" %ARGS%
)
endlocal
