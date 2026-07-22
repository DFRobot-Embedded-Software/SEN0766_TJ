# Verify cry-distill model is present before build/flash
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$tflite = Join-Path $root "models\yamnet\yamnet_cry_distill.tflite"

if (-not (Test-Path $tflite)) {
    Write-Error "Missing $tflite — run scripts/fetch_yamnet_model.ps1"
    exit 1
}

$size = (Get-Item $tflite).Length
Write-Host "yamnet_cry_distill.tflite: $size bytes"
if ($size -lt 50000 -or $size -gt 500000) {
    Write-Warning "Unusual size for cry-distill (~110 KB). Confirm the file is correct."
} else {
    Write-Host "OK"
}
