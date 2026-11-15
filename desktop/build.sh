#!/bin/bash
# vinylclean desktop — builds the Windows .exe and the Linux AppImage from a
# Debian/Ubuntu PC (x86-64). Everything is built from source.
#
#   ./desktop/build.sh            → dist/vinylclean-<version>-windows-x64.exe
#                                    dist/vinylclean-<version>-x86_64.AppImage
#
# Tools: MinGW-w64 (Windows cross-compiler), Zig (Linux build that runs on
# old distributions: glibc ≥ 2.27), libFLAC 1.4.3, squashfuse + the AppImage
# type-2 runtime (built from source), squashfs-tools.
# The Raspberry Pi version (../CMakeLists.txt) is not affected.
set -e
cd "$(dirname "$0")/.."
ROOT="$PWD"
DEPS="$ROOT/build-desktop/deps"
OUT="$ROOT/dist"
VERSION="$(sed -n 's/^project(vinylclean_desktop VERSION \([0-9.]*\).*/\1/p' desktop/CMakeLists.txt)"
mkdir -p "$DEPS" "$OUT"

echo "=== 1/6  Packages ==="
sudo apt-get update
sudo apt-get install -y build-essential cmake git python3-pip pkg-config autoconf automake libtool \
    g++-mingw-w64-x86-64-posix squashfs-tools libfuse3-dev zlib1g-dev libzstd-dev liblzma-dev liblz4-dev
python3 -m ziglang version >/dev/null 2>&1 || pip3 install --user --break-system-packages ziglang

echo "=== 2/6  Sources (libFLAC, squashfuse, AppImage runtime) ==="
cd "$DEPS"
[ -d flac ] || git clone --depth 1 --branch 1.4.3 https://github.com/xiph/flac.git
[ -d squashfuse ] || git clone --depth 1 --branch 0.5.2 https://github.com/vasi/squashfuse.git
[ -d type2-runtime ] || git clone --depth 1 https://github.com/AppImage/type2-runtime.git

# Zig wrappers: Linux binaries linked against glibc 2.28 (old distributions work too).
mkdir -p zig
printf '#!/bin/sh\nexec python3 -m ziglang cc -target x86_64-linux-gnu.2.28 "$@"\n' > zig/cc
printf '#!/bin/sh\nexec python3 -m ziglang c++ -target x86_64-linux-gnu.2.28 "$@"\n' > zig/c++
printf '#!/bin/sh\nexec python3 -m ziglang ar "$@"\n' > zig/ar
printf '#!/bin/sh\nexec python3 -m ziglang ranlib "$@"\n' > zig/ranlib
chmod +x zig/*
cat > zig.cmake <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_C_COMPILER $DEPS/zig/cc)
set(CMAKE_CXX_COMPILER $DEPS/zig/c++)
set(CMAKE_AR $DEPS/zig/ar CACHE FILEPATH "")
set(CMAKE_RANLIB $DEPS/zig/ranlib CACHE FILEPATH "")
EOF

echo "=== 3/6  libFLAC (static, Windows and Linux) ==="
FL="-DBUILD_SHARED_LIBS=OFF -DWITH_OGG=OFF -DBUILD_PROGRAMS=OFF -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DBUILD_DOCS=OFF -DINSTALL_MANPAGES=OFF -DBUILD_CXXLIBS=OFF -DCMAKE_BUILD_TYPE=Release -DWITH_STACK_PROTECTOR=OFF"
cmake -S flac -B flac-win $FL -DCMAKE_TOOLCHAIN_FILE="$ROOT/desktop/packaging/mingw-w64.cmake" -DCMAKE_INSTALL_PREFIX="$DEPS/flac-win-inst"
cmake --build flac-win -j"$(nproc)" && cmake --install flac-win
cmake -S flac -B flac-lin $FL -DCMAKE_TOOLCHAIN_FILE="$DEPS/zig.cmake" -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_INSTALL_PREFIX="$DEPS/flac-lin-inst"
cmake --build flac-lin -j"$(nproc)" && cmake --install flac-lin

echo "=== 4/6  Windows .exe ==="
cmake -S "$ROOT/desktop" -B "$ROOT/build-desktop/win" -DCMAKE_TOOLCHAIN_FILE="$ROOT/desktop/packaging/mingw-w64.cmake" -DFLAC_ROOT="$DEPS/flac-win-inst"
cmake --build "$ROOT/build-desktop/win" -j"$(nproc)"
x86_64-w64-mingw32-strip -o "$OUT/vinylclean-$VERSION-windows-x64.exe" "$ROOT/build-desktop/win/vinylclean.exe"

echo "=== 5/6  Linux binary ==="
cmake -S "$ROOT/desktop" -B "$ROOT/build-desktop/lin" -DCMAKE_TOOLCHAIN_FILE="$DEPS/zig.cmake" -DFLAC_ROOT="$DEPS/flac-lin-inst"
cmake --build "$ROOT/build-desktop/lin" -j"$(nproc)"

echo "=== 6/6  AppImage ==="
if [ ! -x runtime-x86_64 ]; then
    (cd squashfuse && ./autogen.sh && ./configure --disable-demo --enable-static --disable-shared --prefix="$DEPS/sqf-inst" CFLAGS="-Os -ffunction-sections -fdata-sections" && make -j"$(nproc)" && make install)
    cp squashfuse/fuseprivate.h squashfuse/nonstd.h "$DEPS/sqf-inst/include/squashfuse/"
    (cd type2-runtime/src/runtime && gcc -std=gnu99 -Os -D_FILE_OFFSET_BITS=64 -DGIT_COMMIT=\"vinylclean\" \
        -I"$DEPS/sqf-inst/include" -I"$DEPS/sqf-inst/include/squashfuse" -I/usr/include/fuse3 \
        -T data_sections.ld -ffunction-sections -fdata-sections -Wl,--gc-sections -static runtime.c \
        "$DEPS/squashfuse/.libs/libfuseprivate.a" "$DEPS/sqf-inst/lib/libsquashfuse_ll.a" "$DEPS/sqf-inst/lib/libsquashfuse.a" \
        -lzstd -lz -llzma -llz4 -lfuse3 -lpthread -ldl -o "$DEPS/runtime-x86_64")
    printf 'AI\002' | dd of=runtime-x86_64 bs=1 seek=8 count=3 conv=notrunc 2>/dev/null   # AppImage type-2 magic
fi
APPDIR="$ROOT/build-desktop/AppDir"
rm -rf "$APPDIR"
mkdir -p "$APPDIR/usr/bin" "$APPDIR/usr/share/licenses/vinylclean"
cp "$ROOT/build-desktop/lin/vinylclean" "$APPDIR/usr/bin/" && strip "$APPDIR/usr/bin/vinylclean"
cp "$ROOT/desktop/packaging/AppRun" "$ROOT/desktop/packaging/vinylclean.desktop" "$ROOT/desktop/packaging/vinylclean.png" "$APPDIR/"
cp "$ROOT/desktop/packaging/vinylclean.png" "$APPDIR/.DirIcon"
cp "$ROOT/LICENSE" "$ROOT/desktop/third_party/LICENSE.miniaudio" "$APPDIR/usr/share/licenses/vinylclean/"
cp "$DEPS/flac/COPYING.Xiph" "$APPDIR/usr/share/licenses/vinylclean/LICENSE.libFLAC"
mksquashfs "$APPDIR" "$ROOT/build-desktop/app.sqfs" -root-owned -noappend -comp zstd -quiet
cat runtime-x86_64 "$ROOT/build-desktop/app.sqfs" > "$OUT/vinylclean-$VERSION-x86_64.AppImage"
chmod +x "$OUT/vinylclean-$VERSION-x86_64.AppImage"

echo "=== Done ==="
ls -la "$OUT"
