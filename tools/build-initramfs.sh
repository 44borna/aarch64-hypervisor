#!/bin/sh
# Concat Alpine's initramfs-virt with a tiny overlay whose /init runs
# a usable shell (Alpine's own /init blocks in nlplug-findfs; busybox
# `sh -i` hangs on controlling-tty issues; the overlay replaces both
# with a hand-rolled `while read` REPL).
#
# Usage:
#   tools/build-initramfs.sh <alpine-initramfs-virt> <output-path>
# Example:
#   tools/build-initramfs.sh ~/lab/initramfs-virt ~/lab/initramfs-combined

set -eu

alpine_cpio=${1:?usage: $0 <alpine-cpio-gz> <output-path>}
out_path=${2:?usage: $0 <alpine-cpio-gz> <output-path>}

[ -r "$alpine_cpio" ] || { echo "not readable: $alpine_cpio" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

cat > "$work/init" <<'INIT'
#!/bin/busybox sh
# Hypervisor project guest /init.
/bin/busybox mount -t proc     none /proc  2>/dev/null
/bin/busybox mount -t sysfs    none /sys   2>/dev/null
/bin/busybox mount -t devtmpfs none /dev   2>/dev/null

# Redirect stdio to the UART so RX IRQs start flowing, then install
# busybox applets as /bin /sbin symlinks and run a small REPL.
exec </dev/ttyAMA0 >/dev/ttyAMA0 2>&1
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
/bin/busybox --install -s 2>/dev/null

/bin/busybox echo ""
/bin/busybox echo "--- Hypervisor guest shell ---"
/bin/busybox echo "Alpine $( /bin/busybox uname -r ) under the Type-1 hypervisor."
/bin/busybox echo "Ctrl-A X exits QEMU."
/bin/busybox echo ""

while :; do
    /bin/busybox printf "guest:%s# " "$( /bin/busybox pwd )"
    IFS= read -r line || { /bin/busybox echo ""; break; }
    [ -z "$line" ] && continue
    eval "$line"
done
INIT
chmod +x "$work/init"

( cd "$work" && echo init | cpio -o -H newc 2>/dev/null | gzip > overlay.cpio.gz )

cat "$alpine_cpio" "$work/overlay.cpio.gz" > "$out_path"
echo "wrote $out_path ($( wc -c < "$out_path" ) bytes)"
