# Builds the Hydra Link APK. gradlew.bat cannot be used here: cmd breaks on the "&" in the project path
# ("Phoebe & Til"), so the Gradle wrapper is started with Java directly (Android Studio's own JDK).
#   .\build-apk.ps1            release APK (signed with the debug key, installs directly)
#   .\build-apk.ps1 -Debug     debug APK
param([switch]$Debug)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $env:JAVA_HOME) {
    # Android Studio's own JDK: installed (Program Files) or unpacked from the ZIP (no admin), else a portable JDK.
    $candidates = @("C:\Program Files\Android\Android Studio\jbr", "$env:LOCALAPPDATA\Programs\android-studio\jbr") +
                  @(Get-ChildItem "$env:USERPROFILE\.hydra-build\tools\jdk" -Directory -ErrorAction SilentlyContinue | ForEach-Object FullName)
    $env:JAVA_HOME = $candidates | Where-Object { Test-Path "$_\bin\java.exe" } | Select-Object -First 1
}
if (-not $env:ANDROID_HOME) { $env:ANDROID_HOME = "$env:LOCALAPPDATA\Android\Sdk" }
$sdk = $env:ANDROID_HOME -replace '\\', '\\' -replace ':', '\:'
$props = Join-Path $here "local.properties"
if (-not (Test-Path $props)) { Set-Content -Path $props -Value "sdk.dir=$sdk" -Encoding ascii }
$task = if ($Debug) { "assembleDebug" } else { "assembleRelease" }
Push-Location $here
try {
    # Gradle writes progress to stderr: in Windows PowerShell 5.1 that must not count as an error.
    $ErrorActionPreference = "Continue"
    & "$env:JAVA_HOME\bin\java.exe" "-Dorg.gradle.appname=gradlew" -jar "$here\gradle\wrapper\gradle-wrapper.jar" $task --console=plain 2>&1 | ForEach-Object { "$_" }
    $code = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    if ($code -ne 0) { throw "Gradle failed ($code)" }
} finally { Pop-Location }
$kind = if ($Debug) { "debug" } else { "release" }
$apk = Join-Path $env:USERPROFILE ".hydra-build\android\app\outputs\apk\$kind\app-$kind.apk"
$dest = Join-Path $here "HydraLink.apk"
Copy-Item $apk $dest -Force
Write-Host "Fertig: $dest"
