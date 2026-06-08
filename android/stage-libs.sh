#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# Convert the vendored Apple Eloquence dylibs to Android arm64 ELFs and stage
# them (plus the NDK's libc++_shared.so) into app/src/main/jniLibs/arm64-v8a/
# as lib<name>.so, ready for the Gradle build.
#
# Usage:
#   ANDROID_NDK=/path/to/android-ndk-r27c ./android/stage-libs.sh [API]
#
# Requires: a Python venv with lief (see docs/02-conversion.md) on PATH as
# python3, llvm-lipo (or the bundled fat-slice extractor), and the NDK.
set -euo pipefail

API="${1:-24}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
NDK="${ANDROID_NDK:?set ANDROID_NDK to your NDK r27 path}"
CLANG="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android${API}-clang"
LIBCXX="$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"
OUT="$HERE/app/src/main/jniLibs/arm64-v8a"
VENDOR="$ROOT/vendor/tvOS-18.2"

[ -x "$CLANG" ] || { echo "NDK clang not found: $CLANG" >&2; exit 1; }
mkdir -p "$OUT"

extract_arm64() { # $1=dylib  $2=out  — arm64 slice (uses llvm-lipo if present)
    if command -v llvm-lipo >/dev/null; then
        llvm-lipo -extract arm64 "$1" -output "$2"
    else
        python3 - "$1" "$2" <<'PY'
import struct, sys
f = open(sys.argv[1], 'rb').read()
n = struct.unpack_from('>I', f, 4)[0]; o = 8
for _ in range(n):
    ct = struct.unpack_from('>I', f, o)[0]
    off = struct.unpack_from('>I', f, o + 8)[0]
    sz = struct.unpack_from('>I', f, o + 12)[0]
    if ct == 0x0100000C:  # CPU_TYPE_ARM64
        open(sys.argv[2], 'wb').write(f[off:off + sz]); break
    o += 20
PY
    fi
}

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
for dylib in "$VENDOR"/*.dylib; do
    name="$(basename "$dylib" .dylib)"
    extract_arm64 "$dylib" "$tmp/$name.arm64"
    python3 "$ROOT/macho2elf/macho2elf.py" "$tmp/$name.arm64" \
        --os android --cc "$CLANG" \
        -o "$OUT/lib${name}.so" --workdir "$tmp/${name}_work" >/dev/null
    echo "  staged lib${name}.so"
done

cp "$LIBCXX" "$OUT/libc++_shared.so"
echo "  staged libc++_shared.so"
echo "Done -> $OUT"
echo "Note: eci.ini is generated at runtime by EloquenceTtsService; the engine"
echo "      dlopens lib<lang>.so from the installed nativeLibraryDir."
