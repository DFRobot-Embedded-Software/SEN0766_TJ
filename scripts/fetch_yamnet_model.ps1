# Download cry-distill INT8 model into models/yamnet/
# Single model file: replace yamnet_cry_distill.tflite later to swap models
# (same I/O: [1,96,64] log-mel in, 521 logits out → on-device softmax).
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$dest = Join-Path $root "models\yamnet"
New-Item -ItemType Directory -Force -Path $dest | Out-Null

$files = @(
    @{ Name = "yamnet_cry_distill.tflite"; Url = "https://huggingface.co/chayuto/yamnet-cry-distill-int8/resolve/main/model.tflite" },
    @{ Name = "yamnet_class_map.csv"; Url = "https://huggingface.co/chayuto/yamnet-mel-int8-tflm/resolve/main/yamnet_class_map.csv" }
)

foreach ($f in $files) {
    $path = Join-Path $dest $f.Name
    Write-Host "Downloading $($f.Name) ..."
    Invoke-WebRequest -Uri $f.Url -OutFile $path -UseBasicParsing
    Write-Host "  -> $path ($((Get-Item $path).Length) bytes)"
}

Write-Host "Done. Rebuild with: idf.py build flash"
