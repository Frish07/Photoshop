param(
    [string]$Toolchain = "C:\msys64\ucrt64\bin",
    [switch]$Test
)
$ErrorActionPreference = "Stop"
$project = $PSScriptRoot
$build = Join-Path $project "build"
$compiler = Join-Path $Toolchain "g++.exe"
$windres = Join-Path $Toolchain "windres.exe"
if (!(Test-Path -LiteralPath $compiler)) { throw "Install MSYS2 UCRT64 GCC or pass -Toolchain <bin directory>." }
New-Item -ItemType Directory -Path $build -Force | Out-Null
$env:Path = "$Toolchain;$env:Path"
$resource = Join-Path $build "app-res.o"
$common = @("-std=c++20", "-O3", "-DNDEBUG", "-Wall", "-Wextra", "-Wpedantic", "-static", "-DUNICODE",
    "-D_UNICODE", "-DNOMINMAX", "-DWIN32_LEAN_AND_MEAN", "-D_WIN32_WINNT=0x0A00", "-Isrc")
$core = @("src/document.cpp", "src/formats.cpp", "src/psd.cpp")
$gui = @("src/main.cpp", "src/app.cpp", "src/interface.cpp", "src/dialogs.cpp", "src/clipboard.cpp", $resource)
$codecs = @("-lole32", "-loleaut32", "-lwindowscodecs", "-luuid")
Push-Location $project
try {
    & $windres -I resources resources/app.rc $resource
    if ($LASTEXITCODE) { throw "Resource compilation failed." }
    & $compiler @common @core @gui -municode -mwindows -o (Join-Path $build "PixelForge.exe") @codecs -ld2d1 -ldwrite -ldwmapi -lshell32 -lcomdlg32 -lgdi32 -luser32
    if ($LASTEXITCODE) { throw "C++ compilation failed." }
    if ($Test) {
        & $compiler @common @core tests/core_tests.cpp -o (Join-Path $build "PixelForgeTests.exe") @codecs -lpsapi
        if ($LASTEXITCODE) { throw "Test compilation failed." }
        Push-Location $build
        try {
            & (Join-Path $build "PixelForgeTests.exe")
            if ($LASTEXITCODE) { throw "Tests failed." }
        } finally {
            Pop-Location
        }
    }
} finally {
    Pop-Location
}
