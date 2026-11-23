#!/bin/bash
set -euo pipefail
umask 022
if [[ $# -ne 2 || $EUID -ne 0 ]]; then
    echo "Usage inside an isolated WSL as root: $0 /path/to/Files-sdl-audit /path/to/empty-output-dir" >&2
    exit 2
fi
if [[ ${FILES_AUDIT_MOUNT_NAMESPACE:-} != private ]]; then
    export FILES_AUDIT_MOUNT_NAMESPACE=private
    exec unshare --mount --propagation private -- "$0" "$@"
fi
if [[ $(readlink /proc/self/ns/mnt) == "$(readlink /proc/1/ns/mnt)" ]]; then
    echo "Mount namespace must be private" >&2
    exit 2
fi
repo=$(cd "$(dirname "$0")/../.." && pwd)
binary=$(realpath "$1")
out=$(realpath "$2")
if [[ ! -x "$binary" || ! -d "$out" || -n $(find "$out" -mindepth 1 -maxdepth 1 -print -quit) ]]; then
    echo "Provide an audit binary and an empty output directory" >&2
    exit 2
fi
base=$(mktemp -d /tmp/files-audit-fixtures.XXXXXX)
clean() {
    trap - EXIT
    set +e
    if [[ -f "$base/loop-devices" ]]; then
        while read -r dev; do
            umount --lazy "$dev" 2>/dev/null || true
            losetup -d "$dev" || true
        done < "$base/loop-devices"
    fi
    for member in /root /media /run /etc /var/tmp /var /usr; do umount --lazy "$member" 2>/dev/null || true; done
    if [[ "$base" == /tmp/files-audit-fixtures.* ]]; then rm -rf -- "$base"; fi
}
trap clean EXIT
chmod 0755 "$base"
mount -t tmpfs tmpfs /media
mkdir -p "$base/destination"
mount --bind "$base/destination" /root
for member in /usr /var /etc /run; do
    dirname="$base/$(basename "$member")"
    mkdir -p "$dirname/upper" "$dirname/work"
    mount -t overlay overlay -o "lowerdir=$member,upperdir=$dirname/upper,workdir=$dirname/work" "$member"
done
install -d -m1777 "$base/apt-tmp"
mount --bind "$base/apt-tmp" /var/tmp
mkdir -p /usr/share/APPLaunch/bin
install -m0755 "$binary" /usr/share/APPLaunch/bin/M5CardputerZero-Files
cat >/usr/bin/udisksctl <<'EOF'
#!/bin/sh
[ "$1" = unmount ] && [ "$2" = --no-user-interaction ] && [ "$3" = -b ] || exit 2
if [ -e "$UX2_ROOT/eject-fails" ]; then
    printf '%s\n' 'Could not flush the filesystem.' 'Unmount request was rejected by the device.' 'Final journal flush returned an I/O error.' >&2
    exit 1
fi
sleep 0.40
exec /usr/bin/umount -- "$4"
EOF
chmod 0755 /usr/bin/udisksctl
gcc -shared -fPIC -o "$base/slow-copy.so" "$repo/tests/slow_copy.c" -ldl
gcc -shared -fPIC -o "$base/read-fault.so" "$repo/tests/layout_audit/read_fault.c" -ldl
rm -f /usr/share/APPLaunch/bin/M5CardputerZero-AppStore
export UX2_ROOT="$base" UX2_OUTPUT="$out" XDG_CACHE_HOME="$base/cache" FILES_AUDIT_BINARY="$binary"
mv /usr/bin/apt-get /usr/bin/apt-get.ux2-original
cat >/usr/bin/apt-get <<'EOF'
#!/bin/sh
case " $* " in *' --simulate '*)
    if [ -e "$UX2_ROOT/apt-fails" ]; then
        printf '%s\n' 'E: dependency resolution failed for ux2-files-test' \
            'Required packages are unavailable.' 'Check the configured package sources.' >&2
        exit 1
    fi
    ;;
esac
exec /usr/bin/apt-get.ux2-original "$@"
EOF
chmod 0755 /usr/bin/apt-get
mv /usr/bin/sudo /usr/bin/sudo.files-audit-original
cat >/usr/bin/sudo <<'EOF'
#!/bin/sh
if [ -e "$UX2_ROOT/install-fails" ]; then
    sleep 0.55
    printf '%s\n' 'No space left on device.' '' 'dpkg: could not finish unpacking the package.' '' 'dpkg --audit: no unfinished packages.' >&2
    exit 2
fi
if [ -e "$UX2_ROOT/reject-password" ] && [ "$1" = -S ]; then
    echo 'Sorry, incorrect password' >&2
    exit 1
fi
if [ -e "$UX2_ROOT/require-password" ]; then
    echo 'sudo: a password is required' >&2
    exit 1
fi
sleep 0.70
exec /usr/bin/sudo.files-audit-original "$@"
EOF
chmod 0755 /usr/bin/sudo
chmod 0777 "$out"
xvfb-run -a -s '-screen 0 320x170x24' "${FILES_AUDIT_PYTHON:-$repo/.venv/bin/python}" "$repo/tests/layout_audit/scenarios.py" || {
    shopt -s nullglob globstar
    for log in "$base"/*.log "$base"/cs/*.log "$base"/en/*.log; do printf '%s\n' "$log"; tail -25 "$log"; done
    exit 1
}
