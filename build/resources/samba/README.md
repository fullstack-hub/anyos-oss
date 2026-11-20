# Shared folders: the bundled SMB server (Samba)

The packaged app ships its own Samba (`smbd`, `pdbedit`, `testparm`, the
DCE/RPC helpers and the loadable modules) under `Contents/Resources/samba`;
the core runs one `smbd` per VM as the user (see `core/src/share.rs` and the
"Shared folders" section of the README). A checkout uses the same build from
`.runtime/gpu-build/samba-prefix` when `scripts/build-samba.sh` has run,
and Homebrew's Samba otherwise.

- Upstream: https://www.samba.org, release tarball pinned in `pins.json`
  (version, SHA-256, URL).
- License: GPL-3.0-or-later (the tarball's `COPYING`).
- Patches (`patches/`, applied by `scripts/build-samba.sh` in order):
  - `0001-dynconfig-directories-from-the-environment.patch`: Samba's
    directories (`MODULESDIR`, `SAMBA_LIBEXECDIR`, `LOGFILEBASE`, ...) are
    compiled in at configure time. The patch lets `SAMBA_DYN_<NAME>`
    environment variables override any directory that a command-line
    option has not already set, so the same binaries run from wherever
    the app bundle is. The core sets them for every smbd/pdbedit it runs.
- Build: `scripts/build-samba.sh` -- the pinned tarball, the patches, the
  feature set Homebrew builds on macOS minus ICU (its data alone is 32 MB
  and the only use is the utf8-nfc/nfd transliteration for Mac clients;
  the guests are Windows and Linux), system MIT Kerberos, no AD DC, no
  winbind, no Python bindings. The result is trimmed to what the app
  runs; `anyos-samba.json` in the prefix stamps the version, tarball
  hash, patch hashes and the `smbd` hash.
- Packaging: `scripts/samba_stage.py` (also called by `scripts/package-mac.py`) copies the prefix,
  rewrites every Mach-O to `@rpath` names inside the tree (Samba's own
  libraries by their installed name, the Homebrew libraries the build
  linked against closed over into `lib/foreign/`), removes what nothing
  reaches, signs the tree and smoke-tests that `smbd`, `pdbedit` and
  `samba-dcerpcd` start loading only bundled or system libraries.

## Corresponding source (GPLv3 section 6)

Distributing the packaged app distributes Samba binaries, so the complete
corresponding source must accompany them or be offered. What the app was
built from is fully determined by this directory: the pinned tarball
(URL + SHA-256 in `pins.json`), the patches, `scripts/build-samba.sh` and
the packaging step above. Release gate: publish, next to each release, the
pinned tarball (or a link that stays valid for the offer period) together
with `resources/samba/`, `scripts/build-samba.sh`, `scripts/samba_stage.py`,
`scripts/samba_foreign_sources.py` and `scripts/rebuild-corresponding-source.py`, and name
Samba (GPL-3.0-or-later) in the app's third-party notices
(`scripts/corresponding-source.sh` builds that bundle).


## Foreign libraries and installing a modified build

`foreign-sources.json` pins the final `lib/foreign` dependency closure to exact
Homebrew keg versions, source URLs and SHA256 checksums. `foreign-formulas/`
contains the corresponding installed Homebrew recipes, including configure
flags, source edits and installation steps. The release source bundle includes
all those upstream tarballs and patches in `samba-foreign-sources.tar.zst`.
This includes the copyleft dependencies GnuTLS, talloc, tdb, tevent, libxcrypt,
nettle, GMP, libtasn1, libidn2, libunistring and gettext, plus the permissive
foreign libraries in the same closure. Source export verifies every hash;
packaging refuses unrecorded libraries or different keg versions.

From extracted source archives, `python3.13 scripts/rebuild-corresponding-source.py
samba` builds the prefix, creates `.runtime/corresponding-install/samba` using
the same relocation/install code as the release, and signs that tree ad hoc.
The stage retains smbd, pdbedit, testparm, libexec helpers and reachable
libraries/modules, removes absolute build/Homebrew rpaths and uses only tree
relative load commands or macOS system libraries. The output must not already
exist; move your previous installation aside before rebuilding.

To install the modified tree, quit Any OS and its VMs, replace
`Contents/Resources/samba` in a writable app copy with it, and re-sign the app
copy with `codesign --force --deep --sign - "Any OS.app"`. Signing the local
copy ad hoc needs no release credentials. The bundle README describes source
archive extraction and rebuilding the exact foreign dependency recipes.

The checked-in notice texts under `resources/licenses/samba-*` are verbatim:
GPL-3.0 and LGPL-2.1 from GnuTLS 3.8.13_2 `COPYING`/`COPYING.LESSERv2`,
LGPL-3.0 from talloc 2.5.0 `LICENSE`, and the Unicode notice from libidn2
2.3.8 `COPYING.unicode`. Individual library source/license headers determine
the library-specific LGPL notices where a formula also covers GPL utilities.
