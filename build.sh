#!/usr/bin/env bash
# hybrid-inject 一键构建脚本（Linux/macOS 交叉编译 Windows x64 产物）。
# 按优先级尝试两种工具链：
#   1. MinGW-w64 交叉编译器（x86_64-w64-mingw32-g++ + cmake/ninja）
#      Debian/Ubuntu: sudo apt install mingw-w64 cmake ninja-build
#      Arch:          sudo pacman -S mingw-w64-gcc cmake ninja
#   2. zig（zig c++ 自带 MinGW 头/库，无需系统 mingw-w64 包）
# 产物：dist/hybrid-inject.exe、dist/testpayload.dll、dist/hybrid-inject.exe.manifest
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONFIGURATION="${1:-Release}"
BUILD_DIR="$REPO_ROOT/build"
DIST_DIR="$REPO_ROOT/dist"

copy_outputs() {
    local from="$1"
    mkdir -p "$DIST_DIR"
    for name in hybrid-inject.exe testpayload.dll testtarget.exe hybrid-inject.exe.manifest; do
        if [[ -f "$from/$name" ]]; then
            cp -f "$from/$name" "$DIST_DIR/$name"
            echo "  staged: dist/$name"
        fi
    done
}

build_with_mingw() {
    local cxx
    cxx="$(command -v x86_64-w64-mingw32-g++ || true)"
    if [[ -z "$cxx" ]]; then
        return 1
    fi
    echo ">> Building with MinGW-w64 cross ($cxx)" >&2

    local cmake_bin
    cmake_bin="$(command -v cmake || true)"
    if [[ -n "$cmake_bin" ]]; then
        cmake -S "$REPO_ROOT" -B "$BUILD_DIR/cross" -G Ninja \
            -DCMAKE_BUILD_TYPE="$CONFIGURATION" \
            -DCMAKE_SYSTEM_NAME=Windows \
            -DCMAKE_CXX_COMPILER="$cxx"
        cmake --build "$BUILD_DIR/cross" --parallel
        copy_outputs "$BUILD_DIR/cross"
        return 0
    fi

    # 无 cmake：直接调用编译器（不依赖生成器）。
    local opt="-O2"; [[ "$CONFIGURATION" == "Debug" ]] && opt="-g"
    local out="$BUILD_DIR/cross"
    mkdir -p "$out"
    "$cxx" -std=c++20 $opt -municode -Wall -static \
        -lole32 -lshell32 -luser32 \
        "$REPO_ROOT"/src/Main.cpp \
        "$REPO_ROOT"/src/Injector.cpp \
        "$REPO_ROOT"/src/CliOptions.cpp \
        "$REPO_ROOT"/src/IniCompat.cpp \
        "$REPO_ROOT"/src/WineCompat.cpp \
        -o "$out/hybrid-inject.exe"
    "$cxx" -std=c++20 $opt -shared \
        "$REPO_ROOT/testpayload/testpayload.cpp" \
        -o "$out/testpayload.dll"
    "$cxx" -std=c++20 $opt -municode \
        "$REPO_ROOT/testpayload/testtarget.cpp" \
        -o "$out/testtarget.exe"
    cp -f "$REPO_ROOT/app.manifest" "$out/hybrid-inject.exe.manifest"
    copy_outputs "$out"
    return 0
}

build_with_zig() {
    local zig
    zig="$(command -v zig || true)"
    if [[ -z "$zig" ]]; then
        return 1
    fi
    echo ">> Building with zig ($zig)" >&2

    local opt="-O2"; [[ "$CONFIGURATION" == "Debug" ]] && opt="-g"
    local out="$BUILD_DIR/zig"
    mkdir -p "$out"

    "$zig" c++ -target x86_64-windows-gnu -std=c++20 "$opt" -municode -Wall -lole32 -lshell32 -luser32 \
        "$REPO_ROOT/src/Main.cpp" \
        "$REPO_ROOT/src/Injector.cpp" \
        "$REPO_ROOT/src/CliOptions.cpp" \
        "$REPO_ROOT/src/IniCompat.cpp" \
        "$REPO_ROOT/src/WineCompat.cpp" \
        -o "$out/hybrid-inject.exe"
    "$zig" c++ -target x86_64-windows-gnu -std=c++20 "$opt" -shared \
        "$REPO_ROOT/testpayload/testpayload.cpp" \
        -o "$out/testpayload.dll"
    "$zig" c++ -target x86_64-windows-gnu -std=c++20 "$opt" -municode \
        "$REPO_ROOT/testpayload/testtarget.cpp" \
        -o "$out/testtarget.exe"
    cp -f "$REPO_ROOT/app.manifest" "$out/hybrid-inject.exe.manifest"
    copy_outputs "$out"
    return 0
}

echo
echo "=== Building hybrid-inject [$CONFIGURATION] ==="
echo

if ! build_with_mingw && ! build_with_zig; then
    echo "ERROR: no usable toolchain found." >&2
    echo "Install one of:" >&2
    echo "  * mingw-w64 cross toolchain (apt/pacman 包见文件头注释)" >&2
    echo "  * zig (https://ziglang.org/download)" >&2
    exit 1
fi

echo
echo "=== Build complete ==="
echo "Output: $DIST_DIR"
