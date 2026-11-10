param(
    [ValidateSet("Release", "Debug", "RelWithDebInfo")] [string]$Config = "Release",
    # For iterating: only -Target (default the app), no link-time optimisation, build folder outside OneDrive.
    [switch]$Fast,
    [string]$Target = "HydraMixer"
)
$ErrorActionPreference = "Stop"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio C++ Build Tools nicht gefunden." }

Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null

$root = $PSScriptRoot

# ONNX Runtime + DirectML for the Stems effect (not in git): fetched once; without it Stems is compiled out.
if (-not (Test-Path (Join-Path $root "external\onnxruntime\bin\onnxruntime.dll"))) {
    try { & (Join-Path $root "tools\fetch-onnxruntime.ps1") }
    catch { Write-Warning "ONNX Runtime konnte nicht geladen werden ($_): Stems wird ohne gebaut." }
}

# Copies finished artefacts to build\Complete or build\Fast.
function Publish-Build([string]$Folder, [string[]]$Items) {
    $dest = Join-Path $root "build\$Folder"
    New-Item -ItemType Directory -Force $dest | Out-Null
    foreach ($item in $Items) {
        $target = Join-Path $dest (Split-Path $item -Leaf)
        try { Copy-Item -Recurse -Force $item $dest }
        catch {
            # A running exe cannot be overwritten, but it can be renamed.
            Move-Item -Force $target "$target.old"
            Copy-Item -Recurse -Force $item $dest
        }
    }
    Write-Host "Fertig: $dest"
}

# The phone app (mobile\android\build-apk.ps1), if built: Hydra Remote offers it for download (/HydraLink.apk).
$apk = @(Get-Item (Join-Path $root "mobile\android\HydraLink.apk") -ErrorAction SilentlyContinue | ForEach-Object FullName)

if ($Fast) {
    $build = Join-Path $env:USERPROFILE ".hydra-build\fast-$Config"
    $shots = if ($Target -eq "HydraShots") { "ON" } else { "OFF" }   # screenshot tool (tools/Shots.cpp)
    cmake -S $root -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" -DHYDRA_FAST=ON "-DHYDRA_SHOTS=$shots"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    cmake --build $build --target $Target -- -k 0
    $result = $LASTEXITCODE
    if ($result -eq 0) {
        $items = @((Get-ChildItem (Join-Path $build "${Target}_artefacts\$Config") -File | Where-Object Extension -notin ".ilk", ".pdb").FullName)
        if ($Target -eq "HydraMixer") { $items += $apk }
        Publish-Build "Fast" $items
    }
    exit $result
}
$build = Join-Path $root "build\$Config"
# build\ is synced by OneDrive: its CMake cache belongs to the PC that configured it. If that PC's compiler does not
# exist here, build in a folder of this PC (outside OneDrive) instead of reconfiguring the other PC's one.
$cache = Join-Path $build "CMakeCache.txt"
if (Test-Path $cache) {
    $cl = (Select-String -Path $cache -Pattern "^CMAKE_CXX_COMPILER:FILEPATH=(.*)$" | Select-Object -First 1).Matches.Groups[1].Value
    if ($cl -and -not (Test-Path $cl)) {
        $build = Join-Path $env:USERPROFILE ".hydra-build\full-$Config"
        Write-Host "build\$Config was configured on another PC ($cl): building in $build"
    }
}
cmake -S $root -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build $build -- -k 0
$result = $LASTEXITCODE

# Room Correction plugin: JUCE's VST3 post-build step breaks on an "&" in the build path ("Phoebe & Til").
# Then it is built in a folder without one and copied next to the other artefacts.
if ($build.Contains("&")) {
    $pluginBuild = Join-Path $env:USERPROFILE ".hydra-build\plugin-$Config"
    cmake -S $root -B $pluginBuild -G Ninja "-DCMAKE_BUILD_TYPE=$Config" -DHYDRA_PLUGIN_ONLY=ON
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    cmake --build $pluginBuild -- -k 0
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $from = Join-Path $pluginBuild "HydraRoomCorrection_artefacts\$Config\VST3\Hydra Room Correction.vst3"
    $to = Join-Path $build "HydraRoomCorrection_artefacts\$Config\VST3"
    New-Item -ItemType Directory -Force $to | Out-Null
    Copy-Item -Recurse -Force $from $to
}
if ($result -eq 0) {
    # Stems: ONNX Runtime + DirectML next to the exe (if this build has them).
    $ortDlls = @(Get-Item (Join-Path $build "HydraMixer_artefacts\$Config\*.dll") -ErrorAction SilentlyContinue | ForEach-Object FullName)
    Publish-Build "Complete" (@(
        (Join-Path $build "HydraMixer_artefacts\$Config\Hydra Mixer.exe"),
        (Join-Path $build "HydraRoomCorrection_artefacts\$Config\VST3\Hydra Room Correction.vst3")) + $ortDlls + $apk)
}
exit $result
