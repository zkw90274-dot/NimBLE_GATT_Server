@echo off
REM ============================================================================
REM  set_target.bat - switch ESP-IDF target safely
REM
REM  Why this exists:
REM    `idf.py set-target` always runs `fullclean` first (core_ext.py:606).
REM    `fullclean` refuses to delete a build/ directory that exists, is
REM    non-empty, and has no CMakeCache.txt (core_ext.py:188) -> FatalError
REM    -> exit code 2.
REM    idf.py unconditionally creates build/log/ on every run
REM    (tools.py:342 log_dir_name = 'log', hardcoded, no opt-out), so ANY
REM    failed CMake configure leaves exactly that poisoned state behind and
REM    every later set-target fails forever.
REM
REM  This script removes build/ itself before handing over to idf.py,
REM  which sidesteps the guard entirely.
REM
REM  NOTE: this deletes the build directory. A full rebuild is required after.
REM ============================================================================
setlocal

REM --- Neutralize MSYS/Git-Bash markers so export.bat will actually run.
REM     export.bat bails out silently (with exit code 0!) if MSYSTEM is set.
set MSYSTEM=
set SHELL=
set TERM=

REM --- ESP-IDF installation (values taken from esp_idf.json / .vscode/settings.json)
set "IDF_PATH=E:\Tools\esp32idf\esp32_5_4\v5.4.3\esp-idf"
set "IDF_TOOLS_PATH=E:\Tools\esp32idf\esp32_5_4\mytools"

set "TARGET=%~1"

if "%TARGET%"=="" (
    echo Usage: %~nx0 ^<target^>
    echo.
    echo   Example:  %~nx0 esp32s3
    echo.
    echo   Common targets: esp32 esp32s2 esp32s3 esp32c2 esp32c3 esp32c5
    echo                   esp32c6 esp32c61 esp32h2 esp32p4
    exit /b 1
)

cd /d "%~dp0"

echo [1/3] Removing stale build directory...
if exist "build" (
    rmdir /s /q "build"
    if exist "build" (
        echo [FAIL] Could not remove "build". Close VS Code / any process using it and retry.
        exit /b 1
    )
    echo       build\ removed.
) else (
    echo       build\ not present, nothing to remove.
)

echo [2/3] Activating ESP-IDF from %IDF_PATH% ...
call "%IDF_PATH%\export.bat"
if not exist "%IDF_PATH%\tools\idf.py" (
    echo [FAIL] IDF_PATH looks wrong: "%IDF_PATH%\tools\idf.py" not found.
    exit /b 1
)

echo [3/3] Setting target to %TARGET% ...
idf.py set-target %TARGET%
set "RC=%ERRORLEVEL%"

REM --- Verify by artifact, not by exit code.
if not "%RC%"=="0" (
    echo.
    echo [FAIL] idf.py set-target returned %RC%
    exit /b %RC%
)
if not exist "build\CMakeCache.txt" (
    echo.
    echo [FAIL] No build\CMakeCache.txt produced.
    exit /b 1
)
findstr /B /C:"IDF_TARGET:STRING=%TARGET%" "build\CMakeCache.txt" >nul 2>&1
if errorlevel 1 (
    echo.
    echo [FAIL] build\CMakeCache.txt does not record IDF_TARGET=%TARGET%
    exit /b 1
)

echo.
echo [OK] Target is now %TARGET% - verified via build\CMakeCache.txt
echo      Run "idf.py build" to rebuild from scratch.
exit /b 0
