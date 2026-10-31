# Any OS open-source components

The source of the open-source components Any OS ships under the GPL and LGPL,
with the changes Any OS made to them. Each release has a tag (`v0.2.2` is
this tree): the commit before an "Any OS changes" commit holds the unmodified
upstream sources, so that commit's diff shows every change.

| Directory | Component | License | Upstream |
| --- | --- | --- | --- |
| `dxmt/` | DXMT (Direct3D 11 on Metal, UTM fork) | LGPL-2.1-or-later | https://github.com/utmapp/dxmt.git at bcdcf6da89af6c9fedeb2fc92b4367b23d5d6ed6 |
| `vkd3d-proton/` | vkd3d-proton with its statically linked dxil-spirv | LGPL-2.1-or-later | https://github.com/HansKristian-Work/vkd3d-proton.git at 2a38e7991f851e85151c104504d5e343530d1523 |
| `samba/` | Samba (shared-folders SMB server) | GPL-3.0-or-later | https://download.samba.org/pub/samba/stable/samba-4.24.7.tar.gz (sha256 45b7747a47452eff2b2159a44cc63eb43690d339fd1069088e023a015fed06c7) |
| `samba-foreign/` | Libraries shipped with Samba: exact source tarballs, Homebrew patches and formulae | see each library | `samba-foreign/foreign-sources.json` |
| `linux-virtio-gpu/` | Linux kernel virtio-gpu driver for Linux guests | GPL-2.0-only | kernel versions in each `virtio*/ORIGIN` |
| `build/` | Any OS rebuild scripts, pins and the patches applied above | see each component | |

`dxmt/`, `vkd3d-proton/` and `samba/` already contain the Any OS patches; the
same patches are in `build/` alongside the rebuild scripts and pins.
`linux-virtio-gpu/` keeps the upstream driver and its patches
apart: `install.sh` applies them in the guest.

## Rebuild

Use the version tag's patched tree (the upstream parent is for comparison).
All component sources, patches and build recipes are in this repository.
System toolchains, kernel headers and build-only dependencies are prerequisites.

On Apple silicon macOS install Python 3.13, Meson, Ninja, Xcode command-line
tools and `/opt/homebrew/opt/llvm@15` for DXMT. For Samba install the Homebrew
build dependencies listed in `build/scripts/build-samba.sh`. For vkd3d install
the llvm-mingw toolchain pinned by URL and SHA256 in
`build/resources/windows-gpu/pins.json`, under
`build/.runtime/windows-gfxstream/toolchains/<directory>`.

From this repository root prepare the build layout:

```sh
mkdir -p build/.runtime/gpu-build/src/samba build/.runtime/windows-gfxstream
cp -R dxmt build/.runtime/gpu-build/src/dxmt
cp -R vkd3d-proton build/.runtime/windows-gfxstream/vkd3d-proton
cp build/source-inputs/samba-4.24.7.tar.gz build/.runtime/gpu-build/src/samba/
cd build
python3.13 scripts/rebuild-corresponding-source.py dxmt
python3.13 scripts/rebuild-corresponding-source.py vkd3d-proton
python3.13 scripts/rebuild-corresponding-source.py samba
```

Start with a fresh build layout. DXMT and vkd3d snapshots already contain the
shipped patches; do not apply them twice. Samba uses the supplied unmodified
tarball: `build-samba.sh` verifies its hash, extracts it and applies the supplied
patches. The vkd3d build includes statically linked dxil-spirv/dxbc-spirv; its
final DLL link uses `scripts/build-windows-vkd3d-dll.sh` and the supplied C/DEF
files. Outputs use the `.runtime` paths in the scripts, relative to `build/`.
DXMT's library and pkg-config file are in `.runtime/gpu-build/prefix/lib/`.

Samba's `scripts/samba_stage.py` closes over foreign libraries, rewrites install
names and rpaths, and prunes unreachable libraries. The rebuild command installs
a relocatable tree in `.runtime/corresponding-install/samba` and signs its Mach-O
files ad hoc. To install a modified tree, quit Any OS and its VMs, replace
`Contents/Resources/samba` in a writable app copy with that directory, then run
`codesign --force --deep --sign - "Any OS.app"`. Release notarization credentials
are not required.

`samba-foreign/foreign-sources.json` pins every shipped foreign library to its
exact Homebrew keg version and source hash. Each library directory contains
its upstream tarball and Homebrew patches. `samba-foreign/foreign-formulas/`
contains the exact recipes at the manifest paths, including configure flags,
inplace edits, build and install steps. Rebuild with those recipes and apply
patches in formula order; Homebrew and the recipes' build-only dependencies
are prerequisites. Install matching kegs under `/opt/homebrew` before building
Samba, or edit the build paths and source pins for modified dependency builds.
The packager refuses unrecorded libraries or changed keg versions until
`build/resources/samba/foreign-sources.json` is updated. readline is pruned
and is not shipped. Rebuilds use the supplied source files.

For Linux copy `linux-virtio-gpu/` into the ARM64 guest, install its matching
kernel headers plus gcc, make and patch, and run `sudo ./install.sh` there.
It selects the matching vendored source/patch variant, builds `virtio-gpu.ko`,
installs it into `updates/dkms` and rebuilds the initramfs. Reboot to load it.
The kernel headers/build system and system toolchains are prerequisites.
No Mesa or host GPU source is needed for this module.
