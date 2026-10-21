#!/bin/sh
# Build and install the AnyOS virtio-gpu kernel module in an ARM64 Linux
# guest (run as root inside the guest, from a copy of this directory): the
# vendored driver sources of the kernel's series plus that series' patches,
# built out of tree against the running kernel's headers, installed under
# updates/dkms (which depmod prefers over the kernel's own copy) and into the
# initramfs, where the display driver is loaded from. Takes effect at the
# next boot.
#
# One variant per kernel source tree (the distro's driver sources, see each
# ORIGIN):
#   6.8.*  virtio/ + patches/          (Ubuntu 24.04 GA kernel)
#   7.0.*  virtio-7.0/ + patches-7.0/  (Ubuntu 26.04, Ubuntu 24.04.5 HWE)
#   6.12.* virtio-6.12/ + patches-6.12/ (upstream 6.12 stable: Debian 13)
#   6.12.0-211.*el10* virtio-el10/ + patches-el10/ (AlmaLinux/RHEL 10.2)
#   6.12.0-NNN.el10, NNN >= 262  virtio-7.0/ + patches-7.0/ (CentOS Stream
#                    10: its driver from kernel-6.12.0-262.el10 on is
#                    byte-identical to virtio-7.0/)
# RHEL-family kernels keep the 6.12 number over a backported DRM tree, so an
# .el kernel matches only its own entry (z-stream .el10_N, el10nv, el10iv and
# older Stream builds have none). Any other kernel has no variant: nothing is
# installed and the stock driver stays.
#
#   ./install.sh --variant-for <release>  print the variant, exit 1 if none
#   sudo ./install.sh                     build + install for $(uname -r)
#   sudo ./install.sh --kernel <release>  for another installed kernel (the
#                                         kernel postinst hook, AnyOS Tools)
#   sudo ./install.sh --remove [--kernel <release>]  back to the stock module
set -eu
variant_for() {
    case $1 in
        6.8.*) echo 6.8 ;;
        7.0.*) echo 7.0 ;;
        6.12.0-211.*el10*) echo el10 ;;
        6.12.0-[0-9]*.el10|6.12.0-[0-9]*.el10.*)
            build=${1#6.12.0-}; build=${build%%.*}
            case $build in ''|*[!0-9]*) return 1 ;; esac
            case ${1#6.12.0-"$build"} in .el10|.el10.*) ;; *) return 1 ;; esac
            [ "$build" -ge 262 ] || return 1
            echo 7.0 ;;
        *.el[0-9]*) return 1 ;;
        6.12.*) echo 6.12 ;;
        *) return 1 ;;
    esac
}
if [ "${1:-}" = --variant-for ]; then
    variant_for "${2:?kernel release}"
    exit
fi
[ "$(id -u)" = 0 ] || { echo 'Run as root' >&2; exit 1; }
[ "$(uname -m)" = aarch64 ] || { echo 'ARM64 Linux required' >&2; exit 1; }
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
release=$(uname -r)
remove=
while [ $# -gt 0 ]; do
    case $1 in
        --kernel) release=${2:?kernel release}; shift 2 ;;
        --remove) remove=yes; shift ;;
        *) echo "Unknown argument: $1" >&2; exit 1 ;;
    esac
done
case $release in */*|..*|'') echo "Invalid kernel release" >&2; exit 1 ;; esac
target=/lib/modules/$release/updates/dkms/virtio-gpu.ko
# The initramfs holds the display driver: Debian/Ubuntu/Kali rebuild it with
# update-initramfs, Fedora/CentOS with dracut.
update_initramfs() {
    if command -v update-initramfs >/dev/null; then
        update-initramfs -u -k "$release"
    elif command -v dracut >/dev/null; then
        dracut -f --kver "$release"
    else
        return 1
    fi
}
# The compositor's drirc entry goes to every Mesa's drirc.d: the system one
# and the AnyOS guest Mesa's (/opt/anyos-gfxstream, which gnome-shell loads
# on a gfxstream guest -- a Mesa reads only its own datadir).
drircs="/usr/share/drirc.d/99-anyos-gnome-shell.conf"
[ -d /opt/anyos-gfxstream/share ] && drircs="$drircs /opt/anyos-gfxstream/share/drirc.d/99-anyos-gnome-shell.conf"
if [ -n "$remove" ]; then
    rm -f "$target" $drircs
    depmod -a "$release"
    update_initramfs || true
    echo "Stock virtio-gpu restored for $release (reboot to apply)"
    exit 0
fi
# Refuse before touching /lib/modules: no variant for this kernel, no
# initramfs tool, no headers or compiler all leave the stock driver.
variant=$(variant_for "$release") || { echo "No Any OS virtio-gpu variant for kernel $release; the stock driver stays" >&2; exit 2; }
case $variant in
    6.8) sources=$here/virtio; patches=$here/patches ;;
    *) sources=$here/virtio-$variant; patches=$here/patches-$variant ;;
esac
[ -d "$sources" ] && [ -d "$patches" ] || { echo "Any OS virtio-gpu $variant sources missing" >&2; exit 1; }
command -v update-initramfs >/dev/null || command -v dracut >/dev/null || { echo 'No update-initramfs or dracut; the stock driver stays' >&2; exit 1; }
[ -d "/lib/modules/$release/build" ] || { echo "Install the kernel headers for $release first" >&2; exit 1; }
command -v make >/dev/null && command -v gcc >/dev/null && command -v patch >/dev/null || { echo 'Install gcc, make and patch first' >&2; exit 1; }
work=$(mktemp -d /tmp/anyos-virtio-gpu.XXXXXX)
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
cp -R "$sources/." "$work/"
rm -f "$work/ORIGIN"
for patch in "$patches"/*.patch; do
    patch -p1 -d "$work" -s -i "$patch"
done
make -s -C "/lib/modules/$release/build" M="$work" modules
install -d "$(dirname "$target")"
install -m 0644 "$work/virtio-gpu.ko" "$target"
for drirc in $drircs; do install -D -m 0644 "$here/drirc-gnome-shell.conf" "$drirc"; done
depmod -a "$release"
if ! update_initramfs; then
    # The initramfs keeps the stock driver while the module tree has ours:
    # undo, so the two never disagree.
    rm -f "$target"
    depmod -a "$release"
    echo "initramfs update failed for $release; the stock driver stays" >&2
    exit 1
fi
echo "Installed $(modinfo -F srcversion "$target") ($variant variant) at $target (reboot to apply; the host vblank is on, virtio_gpu.anyos_vblank=0 turns it off)"
