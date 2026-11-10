# Fetches ONNX Runtime with DirectML (MIT) and DirectML (Microsoft redistributable) from nuget.org into
# external\onnxruntime (headers + DLLs). Needed for the Stems effect (src/engine/Stems.*); without it the
# effect is compiled out. The folder is not in git (binaries, ~36 MB). build.ps1 calls this when it is missing.
param([string]$OrtVersion = "1.24.4", [string]$DmlVersion = "1.15.4")
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$dest = Join-Path $root "external\onnxruntime"
if (Test-Path (Join-Path $dest "bin\onnxruntime.dll")) { Write-Host "ONNX Runtime ist schon da: $dest"; exit 0 }

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) ("hydra-ort-" + [guid]::NewGuid())
New-Item -ItemType Directory -Force $tmp | Out-Null
try {
    $pkgs = @(
        @{ Id = "microsoft.ml.onnxruntime.directml"; Version = $OrtVersion; Name = "ort" },
        @{ Id = "microsoft.ai.directml"; Version = $DmlVersion; Name = "dml" })
    foreach ($p in $pkgs) {
        $zip = Join-Path $tmp "$($p.Name).zip"
        $url = "https://api.nuget.org/v3-flatcontainer/$($p.Id)/$($p.Version)/$($p.Id).$($p.Version).nupkg"
        Write-Host "Lade $url"
        Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $zip
        Expand-Archive -Force $zip (Join-Path $tmp $p.Name)
    }
    New-Item -ItemType Directory -Force (Join-Path $dest "include"), (Join-Path $dest "bin") | Out-Null
    Copy-Item -Force (Join-Path $tmp "ort\build\native\include\*.h") (Join-Path $dest "include")
    Copy-Item -Force (Join-Path $tmp "ort\runtimes\win-x64\native\onnxruntime.dll") (Join-Path $dest "bin")
    Copy-Item -Force (Join-Path $tmp "dml\bin\x64-win\DirectML.dll") (Join-Path $dest "bin")
    Copy-Item -Force (Join-Path $tmp "ort\LICENSE") (Join-Path $dest "LICENSE-onnxruntime.txt")
    Copy-Item -Force (Join-Path $tmp "ort\ThirdPartyNotices.txt") (Join-Path $dest "ThirdPartyNotices-onnxruntime.txt")
    Copy-Item -Force (Join-Path $tmp "dml\LICENSE.txt") (Join-Path $dest "LICENSE-DirectML.txt")
    Set-Content -Encoding utf8 (Join-Path $dest "VERSION.txt") "onnxruntime-directml $OrtVersion`nDirectML $DmlVersion"
    Write-Host "Fertig: $dest"
}
finally { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
