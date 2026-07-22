# Re-apply after `idf.py update-dependencies` / re-fetch of espressif__esp-sr.
# Fixes Windows GBK UnicodeDecodeError when reading UTF-8 sdkconfig (Chinese menu titles).
$py = Join-Path $PSScriptRoot "..\managed_components\espressif__esp-sr\model\movemodel.py"
if (-not (Test-Path $py)) {
    Write-Error "esp-sr movemodel.py not found: $py"
    exit 1
}
$c = Get-Content -Raw -Encoding UTF8 $py
$fixed = $c -replace 'io\.open\(sdkconfig_path, "r"\)', 'io.open(sdkconfig_path, "r", encoding="utf-8")'
if ($c -eq $fixed) {
    Write-Host "Already patched or pattern not found."
} else {
    Set-Content -Path $py -Value $fixed -Encoding UTF8 -NoNewline
    Write-Host "Patched: $py"
}
