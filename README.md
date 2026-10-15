# Any OS open-source components

The source of the open-source components Any OS ships under the GPL and LGPL,
with the changes Any OS made to them. Each release has a tag (`v0.1.0` is
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
same patches are in `build/` for the rebuild scripts, which start from the
upstream sources. `linux-virtio-gpu/` keeps the upstream driver and its patches
apart: `install.sh` applies them in the guest.

To rebuild, follow the README at
https://cdn.fullstackhub.net/anyos/0.1.0/source/README.md, which also holds
this source as archives next to the release.
