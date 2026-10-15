#!/usr/bin/env python3.13
"""Rebuild from extracted corresponding-source archives (see their README).

The release engine's registration/provenance and packaging are intentionally
not needed to rebuild a replaceable LGPL library. DXMT uses the same Meson
configuration helper as build-gpu-runtime-libs.py; vkd3d uses the release
compatibility-layer builder and DLL linker. No sources are downloaded here.
"""
import argparse
from pathlib import Path
import subprocess
import shutil

import samba_stage
import gpu_build_dirs

ROOT = Path(__file__).resolve().parents[1]


def run(*args):
    subprocess.run([str(a) for a in args], cwd=ROOT, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('component', choices=['dxmt', 'vkd3d-proton', 'samba'])
    parser.add_argument('--jobs', type=int, default=4)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if args.component == 'dxmt':
        product = ROOT / '.runtime/gpu-build'
        gpu_build_dirs.ensure('dxmt', ROOT / '.runtime/gfxstream-stage1', product)
        build = product / 'build/dxmt'
        run('ninja', '-j', args.jobs, '-C', build)
        lib = product / 'prefix/lib/libdxmt-native.dylib'
        lib.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(build / 'src/dxmt-native/libdxmt-native.dylib', lib)
        run('codesign', '--force', '--sign', '-', lib)
        pc = product / 'prefix/lib/pkgconfig/dxmt-native.pc'
        pc.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(build / 'src/dxmt-native/dxmt-native.pc', pc)
    elif args.component == 'vkd3d-proton':
        run('/bin/zsh', 'scripts/build-windows-gpu-compat.sh', '--vkd3d-source-archive')
        run('/bin/bash', 'scripts/build-windows-vkd3d-dll.sh')
    else:
        run('/bin/zsh', 'scripts/build-samba.sh')
        stage = ROOT / '.runtime/corresponding-install'
        samba = samba_stage.stage_samba(stage, ROOT / '.runtime/gpu-build/samba-prefix',
                                        ROOT / '.runtime/gpu-build/src/samba')
        if samba is None:
            raise SystemExit('Samba build did not install smbd')
        for binary in samba_stage.machos_under(samba):
            run('codesign', '--force', '--sign', '-', binary)
        print(f'Relocatable Samba installed at {samba}')


if __name__ == '__main__':
    main()
