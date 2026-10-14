param(
    [ValidateSet("Release", "Debug", "RelWithDebInfo")] [string]$Config = "Release"
)
$ErrorActionPreference = "Stop"

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio C++ Build Tools nicht gefunden." }

Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null

$root = $PSScriptRoot
$build = Join-Path $root "build\$Config"
cmake -S $root -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build $build -- -k 0
exit $LASTEXITCODE
