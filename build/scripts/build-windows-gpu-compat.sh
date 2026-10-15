#!/bin/zsh
# Cross-build the app-local Direct3D compatibility layers for the Windows
# guest, pinned in resources/windows-gpu/pins.json:
#   vkd3d-proton  d3d12.dll + d3d12core.dll   ARM64, x64, x86   (D3D12 over Vulkan)
#   DXVK 2.x      dxgi.dll                    ARM64, x64, x86   (the DXGI vkd3d-proton
#                                                               needs: it ships none, and
#                                                               the system dxgi.dll cannot
#                                                               create swapchains on it)
#   DXVK 2.x      d3d9.dll                    x64, x86          (D3D9 over Vulkan)
# All sit next to a game's executable (scripts/stage-windows-gpu-userspace.sh
# packs them as tools/gpu/compat/<arch>, Add-GpuCompat.ps1 places them); the
# guest has no D3D12 or D3D9 driver for the virtual GPU otherwise.
#
# DXVK is the 2.x generation on purpose: DXVK 3 binds a 2048-entry sampler heap
# and aliases 2D/cube/3D textures on one binding.  The gfxstream host can give a
# guest instance Metal argument buffers since patch 0096 (ANYOS_MVK_ARGUMENT_BUFFERS,
# default engines vkd3d,DXVK), but Metal allows 1024 argument-buffer samplers
# per stage on Apple GPUs, under DXVK 3's heap.
#
# Outputs .runtime/windows-gfxstream/vkd3d-build-{aarch64,x64,x86,arm64ec}/libs/d3d12*/
# and .runtime/windows-gfxstream/dxvk2-build-{aarch64,x64,x86}/src/{dxgi/dxgi.dll,d3d9/d3d9.dll}.
# --vkd3d-source-archive rebuilds only vkd3d from the already patched source
# distribution. No Git metadata or DXVK source is needed in that mode.
set -e
SOURCE_ARCHIVE=
case ${1:-} in
    --vkd3d-source-archive) SOURCE_ARCHIVE=yes ;;
    '') ;;
    *) echo "Unknown argument: $1" >&2; exit 1 ;;
esac
cd "$(dirname "$0")/.."
RUNTIME=.runtime/windows-gfxstream
PINS=resources/windows-gpu/pins.json
T=$PWD/$RUNTIME/toolchains/$(python3 -c "import json;print(json.load(open('$PINS'))['llvmMingw']['directory'])")/bin
[ -x "$T/x86_64-w64-mingw32-clang" ] || { echo "llvm-mingw missing: run scripts/build-windows-gfxstream.py first" >&2; exit 1; }
# Homebrew python 3.14's pyexpat is broken for meson; python-bin/ links /usr/bin/python3.
export PATH="$PWD/$RUNTIME/python-bin:$PATH"
MESON=${MESON:-/opt/homebrew/bin/meson}
NINJA=${NINJA:-/opt/homebrew/bin/ninja}

checkout() {  # checkout <pin-key> <dir>
    local key=$1 dir=$2
    if [ -n "$SOURCE_ARCHIVE" ]; then
        [ -f "$dir/meson.build" ] || { echo "missing extracted source: $dir" >&2; exit 1; }
        return
    fi
    local origin revision
    origin=$(python3 -c "import json;print(json.load(open('$PINS'))['$key']['origin'])")
    revision=$(python3 -c "import json;print(json.load(open('$PINS'))['$key']['revision'])")
    if [ ! -d "$dir/.git" ]; then
        git clone --filter=blob:none --recurse-submodules "$origin" "$dir"
        git -C "$dir" checkout --detach "$revision"
        git -C "$dir" submodule update --init --recursive
        for patch in $(python3 -c "import json;print(' '.join(json.load(open('$PINS'))['$key']['patches']))"); do
            git -C "$dir" apply --check "$PWD/resources/windows-gpu/$patch"
            git -C "$dir" apply "$PWD/resources/windows-gpu/$patch"
        done
    fi
    local last
    last=$(python3 -c "import json;print(json.load(open('$PINS'))['$key']['patches'][-1])")
    if ! git -C "$dir" apply --reverse --check "$PWD/resources/windows-gpu/$last" 2>/dev/null; then
        # An existing tree from before a newly pinned patch (vkd3d-proton 0036 left the
        # main checkout's tree behind, 2026-09-29): apply, in order, the pinned patches
        # that still apply; the last one must then reverse-apply (it depends on the others).
        [ "$(git -C "$dir" rev-parse HEAD)" = "$(python3 -c "import json;print(json.load(open('$PINS'))['$key']['revision'])")" ] ||
            { echo "$dir: not at the pinned revision" >&2; exit 1; }
        for patch in $(python3 -c "import json;print(' '.join(json.load(open('$PINS'))['$key']['patches']))"); do
            if git -C "$dir" apply --check "$PWD/resources/windows-gpu/$patch" 2>/dev/null; then
                echo "$dir: applying $patch"
                git -C "$dir" apply "$PWD/resources/windows-gpu/$patch"
            fi
        done
    fi
    git -C "$dir" apply --reverse --check "$PWD/resources/windows-gpu/$last" || { echo "$dir: patch $last is not applied" >&2; exit 1; }
}

cross() {  # cross <arch-triple> <cpu_family> <file> [extra binaries]
    local triple=$1 family=$2 file=$3 extra=$4
    cat > "$file" <<EOF
[binaries]
c = '$T/$triple-clang'
cpp = '$T/$triple-clang++'
ar = '$T/$triple-ar'
strip = '$T/$triple-strip'
windres = '$T/$triple-windres'
$extra

[properties]
needs_exe_wrapper = true

[host_machine]
system = 'windows'
cpu_family = '$family'
cpu = '$family'
endian = 'little'
EOF
}

checkout vkd3dProton "$RUNTIME/vkd3d-proton"
[ -n "$SOURCE_ARCHIVE" ] || checkout dxvk "$RUNTIME/dxvk-2"

# vkd3d-proton: widl comes from llvm-mingw (the meson option name is upstream's).
cross aarch64-w64-mingw32 aarch64 "$RUNTIME/vkd3d-aarch64-windows.txt" "widl-mingw-tools-fallback = '$T/aarch64-w64-mingw32-widl'"
cross x86_64-w64-mingw32 x86_64 "$RUNTIME/vkd3d-x86_64-windows.txt" "widl-mingw-tools-fallback = '$T/x86_64-w64-mingw32-widl'"
cross i686-w64-mingw32 x86 "$RUNTIME/vkd3d-i686-windows.txt" "widl-mingw-tools-fallback = '$T/i686-w64-mingw32-widl'"
# ARM64EC: only the static libraries matter (the ARM64X anyos_d3d12_umd.dll
# links them beside the ARM64 ones); the DLL targets are what ninja knows.
cross arm64ec-w64-mingw32 aarch64 "$RUNTIME/vkd3d-arm64ec-windows.txt" "widl-mingw-tools-fallback = '$T/arm64ec-w64-mingw32-widl'"
for pair in aarch64:vkd3d-build-aarch64 x86_64:vkd3d-build-x64 i686:vkd3d-build-x86 arm64ec:vkd3d-build-arm64ec; do
    arch=${pair%%:*}; build=$RUNTIME/${pair#*:}
    [ -f "$build/build.ninja" ] || "$MESON" setup "$build" "$RUNTIME/vkd3d-proton" --cross-file "$RUNTIME/vkd3d-$arch-windows.txt" \
        -Dbuildtype=release -Denable_tests=false -Denable_extras=false
    "$NINJA" -C "$build" libs/d3d12/d3d12.dll libs/d3d12core/d3d12core.dll
done

[ -z "$SOURCE_ARCHIVE" ] || exit 0

# DXVK dxgi + d3d9 only: d3d10/d3d11 stay on the Neptune (DXMT) driver.  An
# app-local dxgi.dll serves only the game it sits next to; a game that also
# creates a D3D11 device through it would not get the Neptune driver.
cross aarch64-w64-mingw32 aarch64 "$RUNTIME/dxvk-aarch64-windows.txt"
cross x86_64-w64-mingw32 x86_64 "$RUNTIME/dxvk-x86_64-windows.txt"
cross i686-w64-mingw32 x86 "$RUNTIME/dxvk-i686-windows.txt"
for pair in aarch64:dxvk2-build-aarch64 x86_64:dxvk2-build-x64 i686:dxvk2-build-x86; do
    arch=${pair%%:*}; build=$RUNTIME/${pair#*:}
    if [ -f "$build/build.ninja" ]; then
        "$MESON" configure "$build" -Denable_dxgi=true > /dev/null   # build dirs from before dxgi was shipped
    else
        "$MESON" setup "$build" "$RUNTIME/dxvk-2" --cross-file "$RUNTIME/dxvk-$arch-windows.txt" \
            -Dbuildtype=release -Denable_dxgi=true -Denable_d3d8=false -Denable_d3d10=false -Denable_d3d11=false -Denable_d3d9=true
    fi
    targets=(src/dxgi/dxgi.dll)
    [ "$arch" = aarch64 ] || targets+=(src/d3d9/d3d9.dll)   # no ARM64 D3D9 applications to serve
    "$NINJA" -C "$build" "${targets[@]}"
done

for f in vkd3d-build-aarch64/libs/d3d12/d3d12.dll vkd3d-build-aarch64/libs/d3d12core/d3d12core.dll dxvk2-build-aarch64/src/dxgi/dxgi.dll \
         vkd3d-build-x64/libs/d3d12/d3d12.dll vkd3d-build-x64/libs/d3d12core/d3d12core.dll dxvk2-build-x64/src/dxgi/dxgi.dll dxvk2-build-x64/src/d3d9/d3d9.dll \
         vkd3d-build-x86/libs/d3d12/d3d12.dll vkd3d-build-x86/libs/d3d12core/d3d12core.dll dxvk2-build-x86/src/dxgi/dxgi.dll dxvk2-build-x86/src/d3d9/d3d9.dll; do
    printf '%s  %s\n' "$(shasum -a 256 "$RUNTIME/$f" | cut -c1-16)" "$f"
done
