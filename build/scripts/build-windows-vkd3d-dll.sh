#!/bin/bash
# Link vkd3d-proton (LGPL-2.1-or-later) as its own DLL, anyos_vkd3d_proton.dll,
# which the Any OS D3D12 user-mode driver (anyos_d3d12_umd.dll,
# scripts/build-windows-d3d12-umd.sh) loads at run time from its own folder.
# The driver used to carry vkd3d-proton statically; as a separate DLL it can
# be rebuilt from the published vkd3d-proton source (scripts/corresponding-source.sh)
# and replaced without relinking the driver.
#
# Inputs: the static libraries of scripts/build-windows-vkd3d-libs.sh
# (vkd3d-build-{aarch64,arm64ec,x86}: vkd3d-proton, vkd3d-shader, vkd3d-common
# and the MIT dxil-spirv / dxbc-spirv subprojects vkd3d-shader needs) plus
# resources/windows-gpu/vkd3d-proton-dll (the export list and the
# debug-control defaults d3d12core.dll would otherwise provide).
#
# Outputs (.runtime/windows-gfxstream/build-vkd3d-dll/):
#   arm64x/anyos_vkd3d_proton.dll  ARM64X: the ARM64 half from vkd3d-build-aarch64,
#                                  the ARM64EC half from vkd3d-build-arm64ec -- loaded
#                                  by the ARM64X driver in native ARM64 and emulated
#                                  x64 processes (gpu\arm64)
#   x86/anyos_vkd3d_proton.dll     i686, next to the 32-bit driver (gpu\x86)
set -e
cd "$(dirname "$0")/.."
RUNTIME=${ANYOS_VKD3D_DLL_RUNTIME:-.runtime/windows-gfxstream}
T=$PWD/.runtime/windows-gfxstream/toolchains/$(python3 -c "import json;print(json.load(open('resources/windows-gpu/pins.json'))['llvmMingw']['directory'])")/bin
SRC=resources/windows-gpu/vkd3d-proton-dll
OUT=$RUNTIME/build-vkd3d-dll
LIBS=(-lversion -lgdi32 -lpathcch -lpsapi -lntdll -lm -lsynchronization -lkernel32 -luser32 -ladvapi32 -lole32 -loleaut32 -luuid -ldxguid)
mkdir -p "$OUT/obj" "$OUT/arm64x" "$OUT/x86"

archives() {  # archives <vkd3d build dir>
    local v=$1 a
    for a in libs/vkd3d/libvkd3d-proton.a libs/vkd3d-shader/libvkd3d-shader.a libs/vkd3d-common/libvkd3d_common.a \
             subprojects/dxil-spirv/libdxil-spirv.a subprojects/dxil-spirv/libdxbc_spv_module.a \
             subprojects/dxil-spirv/subprojects/dxbc-spirv/libdxbc_spv.a; do
        [ -f "$v/$a" ] || { echo "missing $v/$a (run scripts/build-windows-vkd3d-libs.sh)" >&2; exit 1; }
        printf '%s\n' "$v/$a"
    done
}

for arch in aarch64 arm64ec i686; do
    "$T/$arch-w64-mingw32-clang" -O2 -Wall -Werror -c "$SRC/anyos_vkd3d_proton.c" -o "$OUT/obj/anyos_vkd3d_proton-$arch.o"
done

# ARM64X: llvm-mingw's aarch64 sysroot archives carry both ARM64 and ARM64EC
# members, so one -l list serves both halves (as in link-windows-umd-arm64x.sh).
inputs=("$OUT/obj/anyos_vkd3d_proton-aarch64.o" $(archives "$RUNTIME/vkd3d-build-aarch64")
        "$OUT/obj/anyos_vkd3d_proton-arm64ec.o" $(archives "$RUNTIME/vkd3d-build-arm64ec"))
"$T/arm64ec-w64-mingw32-clang++" -Wl,-O1 -shared -Wl,-m,arm64xpe "$SRC/anyos_vkd3d_proton.def" \
    -static -static-libgcc -static-libstdc++ \
    -Wl,--start-group "${inputs[@]}" -Wl,--end-group \
    -Wl,--nxcompat -Wl,--dynamicbase "${LIBS[@]}" \
    -o "$OUT/arm64x/anyos_vkd3d_proton.dll"
"$T/llvm-readobj" --file-headers "$OUT/arm64x/anyos_vkd3d_proton.dll" | grep -q "IMAGE_FILE_MACHINE_ARM64X" || { echo "not an ARM64X image" >&2; exit 1; }

inputs=("$OUT/obj/anyos_vkd3d_proton-i686.o" $(archives "$RUNTIME/vkd3d-build-x86"))
"$T/i686-w64-mingw32-clang++" -Wl,-O1 -shared "$SRC/anyos_vkd3d_proton.def" \
    -static -static-libgcc -static-libstdc++ \
    -Wl,--start-group "${inputs[@]}" -Wl,--end-group \
    -Wl,--nxcompat -Wl,--dynamicbase "${LIBS[@]}" \
    -o "$OUT/x86/anyos_vkd3d_proton.dll"

# Exactly the four entry points the driver resolves, in both halves of the ARM64X image.
for dll in "$OUT/arm64x/anyos_vkd3d_proton.dll" "$OUT/x86/anyos_vkd3d_proton.dll"; do
    for sym in vkd3d_create_instance vkd3d_create_device vkd3d_serialize_root_signature vkd3d_serialize_versioned_root_signature; do
        "$T/llvm-readobj" --coff-exports "$dll" | grep -q "Name: $sym\$" || { echo "$dll does not export $sym" >&2; exit 1; }
    done
    printf '%s  %s\n' "$(shasum -a 256 "$dll" | cut -c1-16)" "$dll"
done
