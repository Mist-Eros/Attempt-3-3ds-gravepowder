@echo off
rem ============================================================================
rem  Gravepowder - install converted models on the SD card (no rebuild needed).
rem
rem  Copies data\models\*.gpm to  <SD>:\3ds\Gravepowder\models\  so the game can
rem  load them at runtime and prefer them over the models embedded in the .3dsx.
rem
rem  Usage:
rem      tools\install_assets.cmd            (default SD drive: E:)
rem      tools\install_assets.cmd F:\        (or any drive / mount path)
rem
rem  Safety:
rem      - only copies files; never deletes or overwrites user files other than
rem        same-named .gpm models under 3ds\Gravepowder\models\
rem      - a missing/removable drive is reported and exits with code 1 without
rem        touching anything
rem
rem  Runtime override names are exactly the embedded names: hero.gpm, zombie.gpm,
rem  gun_flintlock.gpm, gun_revolver.gpm, gun_musket.gpm (per docs/assets.md).
rem ============================================================================
setlocal EnableExtensions EnableDelayedExpansion
set "ROOT=%~dp0.."
set "SDROOT=%~1"
if "%SDROOT%"=="" set "SDROOT=E:"

if exist "%SDROOT%\" goto have_sd
echo [install_assets] ERROR: SD card root "%SDROOT%" was not found.
echo.
echo   * Plug the SD card into the computer or mount it, then try again.
echo   * If it mounts on another drive letter, pass it explicitly:
echo         tools\install_assets.cmd F:\
echo   * See available drives with:
echo         powershell -NoProfile -Command "Get-PSDrive -PSProvider FileSystem"
echo.
echo   No files were modified.
exit /b 1

:have_sd
set "SRC=%ROOT%\data\models"
if exist "%SRC%\*.gpm" goto have_src
echo [install_assets] ERROR: no converted models found in data\models\.
echo   Run tools\convert_assets.cmd first - it needs Python, pygltflib and numpy.
echo   No files were modified.
exit /b 1

:have_src
set "DEST=%SDROOT%\3ds\Gravepowder\models"
if exist "%DEST%\" goto have_dest
mkdir "%DEST%" 2>nul
if exist "%DEST%\" goto have_dest
echo [install_assets] ERROR: could not create "%DEST%".
echo   Is the SD card write-protected or full? No other files were modified.
exit /b 1

:have_dest
set "COUNT=0"
echo [install_assets] copying models to "%DEST%"
for %%F in ("%SRC%\*.gpm") do (
    set "NAME=%%~nxF"
    copy /Y "%%~fF" "%DEST%\!NAME!" >nul
    if errorlevel 1 (
        echo [install_assets] ERROR: copy failed: %%~nxF -^> !NAME!
        exit /b 1
    )
    echo    !NAME!  ^(%%~zF bytes^)
    set /a COUNT+=1
)

echo.
echo [install_assets] copied !COUNT! model^(s^) to "%DEST%"
echo   The game prefers these over the embedded models; remove a .gpm file to
echo   fall back to the embedded one. No other files were touched.
exit /b 0
