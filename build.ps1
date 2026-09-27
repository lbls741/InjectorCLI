# hybrid-inject 一键构建脚本（Windows）。
# 按优先级尝试三种工具链：
#   1. MSVC（vswhere 定位 VS + 自带 CMake/Ninja）
#   2. MinGW-w64（PATH 中的 g++ + cmake/ninja）
#   3. zig（zig c++ 单命令构建，自带 MinGW 头/库，无需 Windows SDK）
# 产物：dist\hybrid-inject.exe、dist\testpayload.dll、dist\hybrid-inject.exe.manifest
[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [string]$ZigPath
)

$ErrorActionPreference = "Stop"

$RepoRoot = $PSScriptRoot
$DistDir = Join-Path $RepoRoot "dist"
$BuildDir = Join-Path $RepoRoot "build"

function Copy-Outputs {
    param([string]$FromDir)

    New-Item -ItemType Directory -Path $DistDir -Force | Out-Null
    foreach ($name in @("hybrid-inject.exe", "testpayload.dll", "hybrid-inject.exe.manifest")) {
        $src = Join-Path $FromDir $name
        if (Test-Path $src) {
            Copy-Item -LiteralPath $src -Destination (Join-Path $DistDir $name) -Force
            Write-Host "  staged: dist\$name"
        }
    }
}

function Build-WithMsvc {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $false }

    $vsPath = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) { return $false }

    $cmake = Get-ChildItem -Path $vsPath -Recurse -Filter "cmake.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $cmake) { return $false }
    $cmakeDir = Split-Path $cmake.FullName
    $ninja = Get-ChildItem -Path $vsPath -Recurse -Filter "ninja.exe" -ErrorAction SilentlyContinue |
        Select-Object -First 1

    Write-Host ">> Building with MSVC ($vsPath)" -ForegroundColor Cyan

    $vcvars = Get-ChildItem -Path (Join-Path $vsPath "VC\Auxiliary\Build") -Filter "vcvars64.bat" -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $vcvars) { return $false }

    $generatorArgs = @()
    if ($ninja) {
        $env:PATH = "$(Split-Path $ninja.FullName);$env:PATH"
        $generatorArgs = @("-G", "Ninja")
    }

    & cmd /c "`"$($vcvars.FullName)`" >nul 2>&1 && `"$($cmake.FullName)`" -S `"$RepoRoot`" -B `"$BuildDir\msvc`" $generatorArgs -DCMAKE_BUILD_TYPE=$Configuration && `"$($cmake.FullName)`" --build `"$BuildDir\msvc`" --parallel"
    if ($LASTEXITCODE -ne 0) { throw "MSVC build failed" }

    Copy-Outputs -FromDir (Join-Path $BuildDir "msvc")
    return $true
}

function Build-WithZig {
    $zig = $ZigPath
    if (-not $zig) { $zig = (Get-Command zig -ErrorAction SilentlyContinue).Source }
    if (-not $zig) { return $false }

    Write-Host ">> Building with zig ($zig)" -ForegroundColor Cyan

    New-Item -ItemType Directory -Path (Join-Path $BuildDir "zig") -Force | Out-Null

    $optimize = if ($Configuration -eq "Release") { "-O2" } else { "-g" }
    $outDir = Join-Path $BuildDir "zig"

    & $zig c++ -target x86_64-windows-gnu -std=c++20 $optimize -municode -Wall -lole32 -lshell32 -luser32 `
        (Join-Path $RepoRoot "src\Main.cpp") `
        (Join-Path $RepoRoot "src\Injector.cpp") `
        (Join-Path $RepoRoot "src\CliOptions.cpp") `
        (Join-Path $RepoRoot "src\IniCompat.cpp") `
        (Join-Path $RepoRoot "src\WineCompat.cpp") `
        -o (Join-Path $outDir "hybrid-inject.exe")
    if ($LASTEXITCODE -ne 0) { throw "zig build (hybrid-inject) failed" }

    & $zig c++ -target x86_64-windows-gnu -std=c++20 $optimize -shared `
        (Join-Path $RepoRoot "testpayload\testpayload.cpp") `
        -o (Join-Path $outDir "testpayload.dll")
    if ($LASTEXITCODE -ne 0) { throw "zig build (testpayload) failed" }

    Copy-Item -LiteralPath (Join-Path $RepoRoot "app.manifest") `
        -Destination (Join-Path $outDir "hybrid-inject.exe.manifest") -Force
    Copy-Outputs -FromDir $outDir
    return $true
}

Write-Host ""
Write-Host "=== Building hybrid-inject [$Configuration] ===" -ForegroundColor Cyan
Write-Host ""

if (Build-WithMsvc) {
    # MSVC 成功
}
elseif (Build-WithZig) {
    # zig 成功
}
else {
    Write-Host "ERROR: no usable toolchain found." -ForegroundColor Red
    Write-Host "Install one of:"
    Write-Host "  * Visual Studio with C++ workload (MSVC)"
    Write-Host "  * MinGW-w64 (g++ on PATH, e.g. MSYS2: pacman -S mingw-w64-ucrt-x86_64-gcc)"
    Write-Host "  * zig (https://ziglang.org/download)"
    exit 1
}

Write-Host ""
Write-Host "=== Build complete ===" -ForegroundColor Cyan
Write-Host "Output: $DistDir"
