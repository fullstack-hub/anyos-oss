#!/bin/zsh
# Build the pinned vkd3d-proton static libraries used by the system D3D12 UMD
# and its separately replaceable anyos_vkd3d_proton.dll (LGPL).
# Outputs .runtime/windows-gfxstream/vkd3d-build-{aarch64,arm64ec,x86}.
# --vkd3d-source-archive uses an extracted, patched corresponding-source tree.
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

# vkd3d-proton: widl comes from llvm-mingw (the meson option name is upstream's).
cross aarch64-w64-mingw32 aarch64 "$RUNTIME/vkd3d-aarch64-windows.txt" "widl-mingw-tools-fallback = '$T/aarch64-w64-mingw32-widl'"
cross i686-w64-mingw32 x86 "$RUNTIME/vkd3d-i686-windows.txt" "widl-mingw-tools-fallback = '$T/i686-w64-mingw32-widl'"
cross arm64ec-w64-mingw32 aarch64 "$RUNTIME/vkd3d-arm64ec-windows.txt" "widl-mingw-tools-fallback = '$T/arm64ec-w64-mingw32-widl'"
for pair in aarch64:vkd3d-build-aarch64 i686:vkd3d-build-x86 arm64ec:vkd3d-build-arm64ec; do
    arch=${pair%%:*}; build=$RUNTIME/${pair#*:}
    [ -f "$build/build.ninja" ] || "$MESON" setup "$build" "$RUNTIME/vkd3d-proton" --cross-file "$RUNTIME/vkd3d-$arch-windows.txt" \
        -Dbuildtype=release -Denable_tests=false -Denable_extras=false
    "$NINJA" -C "$build" libs/vkd3d/libvkd3d-proton.a libs/vkd3d-shader/libvkd3d-shader.a \
        libs/vkd3d-common/libvkd3d_common.a subprojects/dxil-spirv/libdxil-spirv.a \
        subprojects/dxil-spirv/libdxbc_spv_module.a \
        subprojects/dxil-spirv/subprojects/dxbc-spirv/libdxbc_spv.a
done

