param(
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Release",
    [switch]$Clean,
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir = Join-Path $ProjectRoot "build"

if ($Clean -and (Test-Path $BuildDir)) {
    Remove-Item -Recurse -Force $BuildDir
}

# Locate Visual Studio vcvars64.bat, cmake.exe, and ninja.exe
$VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $VsWhere)) {
    throw "vswhere.exe bulunamadi. Visual Studio Build Tools veya C++ is yuku gerekli."
}

$VcVars = & $VsWhere -all -products * -find "**\VC\Auxiliary\Build\vcvars64.bat" | Select-Object -First 1
$CMakeExe = & $VsWhere -all -products * -find "**\CMake\bin\cmake.exe" | Select-Object -First 1
$NinjaExe = & $VsWhere -all -products * -find "**\Ninja\ninja.exe" | Select-Object -First 1
$CTestExe = & $VsWhere -all -products * -find "**\CMake\bin\ctest.exe" | Select-Object -First 1

if (-not $VcVars -or -not $CMakeExe) {
    throw "MSVC vcvars64.bat veya CMake bulunamadi."
}

$GeneratorArgs = ""
if ($NinjaExe) {
    $GeneratorArgs = "-G Ninja -DCMAKE_MAKE_PROGRAM=`"$NinjaExe`""
}

$CmdScript = @"
@echo off
call "$VcVars" >nul
if errorlevel 1 exit /b 1
"$CMakeExe" -S "$ProjectRoot" -B "$BuildDir" $GeneratorArgs -DCMAKE_BUILD_TYPE=$Config
if errorlevel 1 exit /b 1
"$CMakeExe" --build "$BuildDir" --config $Config
if errorlevel 1 exit /b 1
"@

$TempBat = Join-Path $env:TEMP "vsport_build_env.bat"
Set-Content -Path $TempBat -Value $CmdScript -Encoding ASCII
& cmd.exe /c $TempBat
if ($LASTEXITCODE -ne 0) {
    throw "Derleme basarisiz oldu (Exit code: $LASTEXITCODE)."
}

if (-not $SkipTests -and $CTestExe) {
    Write-Host "`n[vsport] Birim testleri calistiriliyor..." -ForegroundColor Cyan
    & $CTestExe --test-dir $BuildDir --output-on-failure -C $Config
    if ($LASTEXITCODE -ne 0) {
        throw "Birim testleri basarisiz oldu!"
    }
}

Write-Host "`n[vsport] Derleme tamamlandi: $BuildDir" -ForegroundColor Green
