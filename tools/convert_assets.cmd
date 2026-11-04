@echo off
rem ============================================================================
rem  Gravepowder - convert free models into the embedded .gpm set.
rem
rem  Runs tools/import_model.py for every assets/import/*.json and writes the
rem  converted models to data/models/*.gpm.  The .gpm files are committed, so
rem  CI only runs `make`; this script is for changing the model set locally.
rem
rem  Requirements (host only):
rem      Python 3.8+ on PATH
rem      pip install pygltflib numpy Pillow
rem
rem  Usage:
rem      tools\convert_assets.cmd
rem
rem  Then either rebuild (make) to embed the models, or copy them to the SD
rem  card for the no-rebuild swap path:
rem      tools\install_assets.cmd [SDDRIVE:]
rem ============================================================================
setlocal EnableExtensions
set "SCRIPT_DIR=%~dp0"
set "PS1=%SCRIPT_DIR%convert_assets.ps1"
if not exist "%PS1%" (
    echo [convert_assets] ERROR: %PS1% not found.
    exit /b 1
)
where powershell >nul 2>nul
if errorlevel 1 (
    echo [convert_assets] ERROR: Windows PowerShell not found on PATH.
    exit /b 1
)
echo [convert_assets] converting assets/import/*.json -^> data/models/*.gpm
powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%"
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
    echo [convert_assets] FAILED with exit code %RC%
    exit /b %RC%
)
echo [convert_assets] done.
exit /b 0
