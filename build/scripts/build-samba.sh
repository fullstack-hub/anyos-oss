#!/bin/zsh
# Build the shared-folders SMB server (Samba) for the app bundle: the pinned
# release tarball, AnyOS's patches (resources/samba/patches), the same
# feature set Homebrew builds on macOS, installed into a prefix under
# .runtime that scripts/package-mac.py copies into Contents/Resources/samba.
# Samba is GPLv3: the packaged app ships this exact source, the patches and
# this script as the corresponding source (see docs).
#
#   scripts/build-samba.sh            build into .runtime/gpu-build/samba-prefix
#   SAMBA_JOBS=8 scripts/build-samba.sh
set -eu
cd "$(dirname "$0")/.."
root=$PWD
pins=resources/samba/pins.json
version=$(python3 -c "import json;print(json.load(open('$pins'))['version'])")
sha=$(python3 -c "import json;print(json.load(open('$pins'))['sha256'])")
src=$root/.runtime/gpu-build/src/samba
prefix=$root/.runtime/gpu-build/samba-prefix
tarball=$src/samba-$version.tar.gz
mkdir -p "$src"
if [ ! -f "$tarball" ]; then
  curl -sSL -o "$tarball" "https://download.samba.org/pub/samba/stable/samba-$version.tar.gz"
fi
actual=$(shasum -a 256 "$tarball" | cut -d ' ' -f 1)
[ "$actual" = "$sha" ] || { echo "samba-$version.tar.gz: sha256 $actual, expected $sha" >&2; exit 1; }
tree=$src/samba-$version
rm -rf "$tree"
tar xzf "$tarball" -C "$src"
for patch in resources/samba/patches/*.patch; do
  (cd "$tree" && patch -p1 --batch -F0 < "$root/$patch")
done
brew=/opt/homebrew
# Homebrew python 3.14 lacks pyexpat (the parameter generator needs it): 3.13.
export PYTHON=$brew/bin/python3.13
# macOS ships bison 2.3, which lacks %code: Homebrew bison and flex first.
export PATH="$brew/opt/python@3.13/bin:$brew/opt/bison/bin:$brew/opt/flex/bin:$brew/opt/krb5/bin:$brew/opt/krb5/sbin:$brew/bin:$PATH"
# ICU (keg-only, so invisible unless named here) only serves the utf8-nfc/nfd
# transliteration for Mac SMB clients; the guests are Windows and Linux, and
# ICU's data alone is 32 MB, so it stays out.
export PKG_CONFIG_PATH="$brew/opt/krb5/lib/pkgconfig:$brew/opt/gnutls/lib/pkgconfig:$brew/opt/readline/lib/pkgconfig:$brew/opt/libxcrypt/lib/pkgconfig:$brew/lib/pkgconfig"
export CPPFLAGS="-I$brew/opt/krb5/include -I$brew/opt/readline/include -I$brew/opt/libxcrypt/include -I$brew/include"
export LDFLAGS="-L$brew/opt/krb5/lib -L$brew/opt/readline/lib -L$brew/opt/libxcrypt/lib -L$brew/lib"
rm -rf "$prefix"
cd "$tree"
./configure \
  --bundled-libraries=NONE \
  --private-libraries='!ldb' \
  --disable-cephfs --disable-cups --disable-iprint --disable-glusterfs --disable-python \
  --without-acl-support --without-ad-dc --without-ads --without-ldap --without-libarchive \
  --without-json --without-pam --without-regedit --without-syslog --without-utmp --without-winbind \
  --with-shared-modules='!vfs_snapper' \
  --with-system-mitkrb5 \
  --prefix="$prefix" --libdir="$prefix/lib" --sysconfdir="$prefix/etc" --localstatedir="$prefix/var"
make -j"${SAMBA_JOBS:-$(sysctl -n hw.ncpu)}"
make install
# What the app needs: the server, the account tool, the (private) libraries
# and loadable modules, the DCE/RPC helpers smbd spawns (libexec/samba).
# Everything else is dropped so the bundle stays small; smbclient stays for
# testing a running server from the Mac.
cd "$prefix"
keep_bin="bin/pdbedit bin/smbclient bin/testparm sbin/smbd"
for entry in bin/* sbin/*; do
  case " $keep_bin " in *" $entry "*) ;; *) rm -f "$entry" ;; esac
done
rm -rf var etc include share bind-dns private lib/pkgconfig
python3 - "$tree" "$prefix" "$root" <<'PY'
import json, sys, pathlib, hashlib, subprocess
tree, prefix, root = map(pathlib.Path, sys.argv[1:4])
pins = json.loads((root / "resources/samba/pins.json").read_text())
patches = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted((root / "resources/samba/patches").glob("*.patch"))}
stamp = {"version": pins["version"], "sha256": pins["sha256"], "patches": patches,
         "smbd": hashlib.sha256((prefix / "sbin/smbd").read_bytes()).hexdigest()}
(prefix / "anyos-samba.json").write_text(json.dumps(stamp, indent=1) + "\n")
print(json.dumps(stamp, indent=1))
PY
echo "installed to $prefix"
