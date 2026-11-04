<#
.SYNOPSIS
    Convert assets/import/*.json + assets/models/*.glb into data/models/*.gpm.

.DESCRIPTION
    Runs tools/import_model.py once per import config (assets/import/*.json), so
    the 3DS build can embed the converted "Gravepowder Model" files with bin2o.

    Output naming (config file stem -> runtime name):
        hero_medieval.json   -> data/models/hero.gpm
        zombie_animated.json -> data/models/zombie.gpm
        gun_flintlock.json   -> data/models/gun_flintlock.gpm
        gun_revolver.json    -> data/models/gun_revolver.gpm
        gun_musket.json      -> data/models/gun_musket.gpm
    A JSON config may override the target name with a "target" field.  Extra
    configs (e.g. a future zombie_runner.json) keep their own stem.

    bin2o then embeds them with symbols hero_gpm / zombie_gpm /
    gun_flintlock_gpm / ... (plus _end and _size) and generated headers
    build/<name>_gpm.h.  The same names are used for the SD-card overrides.

    The generated files are committed, so CI does NOT need Python: it just runs
    `make`.  Run this locally only when the source models or import configs
    change.

    REQUIREMENTS (host only):
        Python 3.8+ on PATH
        pip install pygltflib numpy Pillow

.NOTES
    Run from anywhere: it cd's to the repository root itself.
    Exit code 0 = all configs converted; 1 = at least one failure (build will
    then fall back to the procedural meshes because data/models/*.gpm is what
    gets embedded).
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$importModel = Join-Path $PSScriptRoot 'import_model.py'
$importDir = Join-Path $root 'assets\import'
$modelDir = Join-Path $root 'data\models'

# config stem -> output model name (runtime/embedded name)
$targets = @{
    'hero_medieval'   = 'hero'
    'zombie_animated' = 'zombie'
}

function Fail([string]$message) {
    Write-Host "[convert_assets] ERROR: $message" -ForegroundColor Red
    exit 1
}

Write-Host "Gravepowder asset conversion"
Write-Host "  root: $root"

if (-not (Test-Path $importModel)) {
    Fail "tools\import_model.py not found - it is the engine-core importer and must be checked in before assets can be converted."
}
try {
    $python = (Get-Command python -ErrorAction Stop).Source
} catch {
    Fail "python not found on PATH (install Python 3.8+ and 'pip install pygltflib numpy Pillow')."
}

# Fail early with a readable message instead of a traceback per file.
& $python -c "import pygltflib, numpy, PIL" 2>$null
if ($LASTEXITCODE -ne 0) {
    Fail "missing Python packages; run: pip install pygltflib numpy Pillow"
}

if (-not (Test-Path $importDir)) {
    Fail "assets\import\ directory not found; nothing to convert."
}

$configs = @(Get-ChildItem -Path $importDir -Filter '*.json' | Sort-Object Name)
if ($configs.Count -eq 0) {
    Fail "no assets\import\*.json configs found; nothing to convert."
}

New-Item -ItemType Directory -Path $modelDir -Force | Out-Null

$results = @()
$failed = 0

foreach ($config in $configs) {
    $stem = [System.IO.Path]::GetFileNameWithoutExtension($config.Name)
    try {
        $json = Get-Content -Path $config.FullName -Raw | ConvertFrom-Json
    } catch {
        Write-Host "[convert_assets] ERROR: cannot parse $($config.Name): $_" -ForegroundColor Red
        $failed++
        continue
    }

    if (-not $json.source) {
        Write-Host "[convert_assets] ERROR: $($config.Name) has no 'source' field" -ForegroundColor Red
        $failed++
        continue
    }
    $sourcePath = Join-Path $root $json.source
    if (-not (Test-Path $sourcePath)) {
        Write-Host "[convert_assets] ERROR: source model not found: $($json.source)" -ForegroundColor Red
        $failed++
        continue
    }

    if ($json.target) {
        $name = [string]$json.target
    } elseif ($targets.ContainsKey($stem)) {
        $name = $targets[$stem]
    } else {
        $name = $stem
    }
    $outPath = Join-Path $modelDir "$name.gpm"

    Write-Host ""
    Write-Host "==> $($config.Name)  ->  data/models/$name.gpm"
    $importerArgs = @('tools\import_model.py', $sourcePath, '--config', $config.FullName, '--out', $outPath)
    if ($json.kind) {
        $importerArgs += @('--kind', [string]$json.kind)
    }
    & $python @importerArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[convert_assets] ERROR: importer failed for $($config.Name) (exit $LASTEXITCODE)" -ForegroundColor Red
        $failed++
        continue
    }

    if (-not (Test-Path $outPath)) {
        Write-Host "[convert_assets] ERROR: importer reported success but $name.gpm is missing" -ForegroundColor Red
        $failed++
        continue
    }

    # Sanity check the GPM header: magic "GPM1" (0x31504D47 little-endian) and
    # a size that at least covers the 64-byte header.
    $bytes = [System.IO.File]::ReadAllBytes($outPath)
    $magic = [System.BitConverter]::ToUInt32($bytes, 0)
    if ($bytes.Length -lt 64 -or $magic -ne 0x31504D47) {
        Write-Host "[convert_assets] ERROR: $name.gpm is not a valid GPM file (size $($bytes.Length), magic 0x$($magic.ToString('X8')))" -ForegroundColor Red
        $failed++
        continue
    }

    $results += [pscustomobject]@{ Config = $config.Name; Model = "$name.gpm"; Bytes = $bytes.Length }
}

Write-Host ""
if ($results.Count -gt 0) {
    Write-Host "Converted models:"
    $results | Format-Table -AutoSize | Out-String | Write-Host
    $total = ($results | Measure-Object -Property Bytes -Sum).Sum
    Write-Host ("  {0} file(s), {1:N0} bytes total ({2:N2} MB)" -f $results.Count, $total, ($total / 1MB))
}
if ($failed -gt 0) {
    Write-Host "[convert_assets] FAILED: $failed config(s) did not convert. The 3DS build will keep the procedural fallback for those." -ForegroundColor Red
    exit 1
}

Write-Host "[convert_assets] OK - data/models/*.gpm ready; 'make' embeds them in the next .3dsx." -ForegroundColor Green
Write-Host "                 copy them to the SD card with tools\install_assets.cmd"
exit 0
