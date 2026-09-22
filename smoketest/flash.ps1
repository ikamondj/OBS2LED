param(
    [string]$Port = "",
    [switch]$BuildOnly
)

$ErrorActionPreference = "Stop"
Set-Location $PSScriptRoot

function Find-Pio {
    $cmd = Get-Command pio -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    # Check installed launchers before consulting Python: py may select a
    # different Python version from the one that installed PlatformIO.
    $launcherPaths = @(
        "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
        "$env:APPDATA\Python\Python*\Scripts\pio.exe"
        "$env:LOCALAPPDATA\Programs\Python\Python*\Scripts\pio.exe"
    )
    foreach ($launcherPath in $launcherPaths) {
        $launcher = Get-Item -Path $launcherPath -ErrorAction SilentlyContinue |
            Where-Object { -not $_.PSIsContainer } | Select-Object -First 1
        if ($launcher) {
            return $launcher.FullName
        }
    }

    $py = Get-Command py -ErrorAction SilentlyContinue
    if (-not $py) {
        $py = Get-Command python -ErrorAction SilentlyContinue
    }

    if (-not $py) {
        throw "Python was not found. Install Python 3 from python.org, then run this script again."
    }

    # Windows user installs include a version directory (e.g. Python311).
    # Ask the same Python used by pip for both regular and --user script paths.
    $scriptDirs = & $py.Source -c "import sysconfig; print(sysconfig.get_path('scripts')); print(sysconfig.get_path('scripts', scheme='nt_user'))"
    if ($LASTEXITCODE -ne 0) {
        throw "Could not determine Python's Scripts directories. Check that Python runs correctly."
    }
    foreach ($scriptDir in $scriptDirs) {
        $candidate = Join-Path $scriptDir.Trim() "pio.exe"
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    return $null
}

$pio = Find-Pio

if (-not $pio) {
    $py = Get-Command py -ErrorAction SilentlyContinue
    if (-not $py) {
        $py = Get-Command python -ErrorAction Stop
    }

    Write-Host "PlatformIO was not found. Installing it for the current Windows user..."
    & $py.Source -m pip install --user --upgrade platformio
    if ($LASTEXITCODE -ne 0) {
        throw "PlatformIO installation failed."
    }

    $pio = Find-Pio
    if (-not $pio) {
        throw "PlatformIO installed, but pio.exe was not found. Check pip's installation output for the Scripts directory."
    }
}

Write-Host ""
Write-Host "Using PlatformIO: $pio"
Write-Host "Project: $PSScriptRoot"
Write-Host ""
Write-Host "IMPORTANT:"
Write-Host "  * Plug the PC into the board's upper USB-C data/programming port."
Write-Host "  * Close OBS and any Serial Monitor first so the COM port is not locked."
Write-Host "  * For a first flash, if upload cannot connect: hold BOOT, tap RST, release BOOT, then rerun."
Write-Host ""

Write-Host "Building scrolling-text smoke test..."
& $pio run -e hub75s3
if ($LASTEXITCODE -ne 0) {
    throw "Firmware build failed."
}

if ($BuildOnly) {
    Write-Host "Build succeeded. No firmware was uploaded."
    return
}

Write-Host ""
if ($Port) {
    Write-Host "Uploading through $Port ..."
    & $pio run -e hub75s3 -t upload --upload-port $Port
} else {
    Write-Host "Detected serial devices:"
    & $pio device list
    Write-Host ""
    Write-Host "Uploading using PlatformIO auto-detection..."
    & $pio run -e hub75s3 -t upload
}

if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "Upload failed."
    Write-Host "If this is the first flash, put the ESP32-S3 in download mode:"
    Write-Host "  1. Hold BOOT."
    Write-Host "  2. Press and release RST."
    Write-Host "  3. Release BOOT."
    Write-Host "Then rerun, optionally specifying the COM port, for example:"
    Write-Host "  .\flash.ps1 -Port COM12"
    exit 1
}

Write-Host ""
Write-Host "Flash succeeded."
Write-Host "Release BOOT, then press RST once to start the smoke test."
Write-Host "The matrix should scroll: LED CONTROL WORKING"

