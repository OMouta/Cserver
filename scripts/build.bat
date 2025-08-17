@echo off
REM Simple build wrapper for MinGW on Windows (moved to scripts/) 
@rem Determine script dir and repo root
set SCRIPT_DIR=%~dp0
set REPO_ROOT=%SCRIPT_DIR%..\
set BUILD_DIR=%REPO_ROOT%build

IF NOT EXIST "%BUILD_DIR%" (
    echo Configuring build directory with MinGW Makefiles in: %BUILD_DIR%
    cmake -S "%REPO_ROOT%" -B "%BUILD_DIR%" -G "MinGW Makefiles"
)
echo Building (Release) in: %BUILD_DIR%
cmake --build "%BUILD_DIR%" --config Release %*
