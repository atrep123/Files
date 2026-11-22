#!/usr/bin/env bash
set -euo pipefail

if [[ $# != 2 || $EUID != 0 ]]; then
    echo "Usage (root): $0 /path/to/files_hardening_probe /path/to/Files-sdl" >&2
    exit 2
fi
if [[ ${FILES_TEST_MOUNT_NAMESPACE:-} != private ]]; then
    export FILES_TEST_MOUNT_NAMESPACE=private
    exec unshare --mount --propagation private -- "$0" "$(realpath "$1")" "$(realpath "$2")"
fi

probe=$1
app=$2
tmp=$(mktemp -d)
appdir=/usr/share/APPLaunch
created_appdir=false
test_mounts=()
cleanup() {
    for ((index=${#test_mounts[@]}-1; index>=0; --index)); do
        umount -- "${test_mounts[index]}" || umount -l -- "${test_mounts[index]}"
    done
    if $created_appdir; then
        rmdir -- "$appdir"
    fi
    rm -rf -- "$tmp"
}
trap cleanup EXIT
chmod 0755 "$tmp"
install -m 0755 -- "$probe" "$tmp/probe"
probe="$tmp/probe"
record_mount() {
    local target=$1
    shift
    mount "$@" "$target"
    test_mounts+=("$target")
}

drive="$tmp/Cíl bez jmenovky"
mkdir "$drive" "$tmp/Jiný" "$tmp/ro" "$tmp/full"
truncate -s 16M "$tmp/image"
mkfs.ext4 -q -F "$tmp/image"
record_mount "$drive" -o loop,nosuid,nodev "$tmp/image"
[[ -z $(lsblk -no LABEL "$(findmnt -nro SOURCE --mountpoint "$drive")") ]]
record_mount "$tmp/full" -t tmpfs -o size=32k tmpfs
record_mount "$tmp/ro" -t tmpfs -o ro tmpfs
record_mount "$tmp/Jiný" -t tmpfs tmpfs

for mode in sync-file sync-directory sync-parent move mount bind; do
    source="$tmp/$mode strom"
    mkdir -p "$source/Jiný svazek"
    printf 'verified source\n' >"$source/Příliš žluťoučký.txt"
    if [[ $mode == move ]]; then
        ln "$source/Příliš žluťoučký.txt" "$source/Jiný svazek/Tvrdý odkaz.txt"
    fi
    if [[ $mode == mount || $mode == bind ]]; then
        from="$tmp/Jiný"
        if [[ $mode == bind ]]; then
            from="$tmp/Same filesystem"
            mkdir "$from"
        fi
        record_mount "$source/Jiný svazek" --bind "$from"
        printf 'protected\n' >"$source/Jiný svazek/chráněný.txt"
    fi
    "$probe" "$mode" "$source" "$drive"
    echo "PASS $mode"
done
if command -v mkfs.vfat >/dev/null; then
    mkdir "$tmp/fat" "$tmp/fat-source"
    chmod 0777 "$tmp/fat-source"
    mkdir "$tmp/fat-source/Přesun na FAT"
    printf 'FAT filename fixture\n' >"$tmp/fat-source/Přesun na FAT/Příliš žluťoučký.txt"
    chown -R nobody "$tmp/fat-source/Přesun na FAT"
    truncate -s 32M "$tmp/fat.img"
    mkfs.vfat "$tmp/fat.img"
    record_mount "$tmp/fat" -o "loop,utf8,uid=$(id -u nobody),gid=$(id -g nobody),fmask=0133,dmask=0022" "$tmp/fat.img"
    runuser -u nobody -- "$probe" move "$tmp/fat-source/Přesun na FAT" "$tmp/fat"
    grep -Fx 'FAT filename fixture' "$tmp/fat/Přesun na FAT/Příliš žluťoučký.txt"
    echo "PASS move and fsync to unlabeled FAT image"
fi
dd if=/dev/zero of="$tmp/velký soubor" bs=1M count=1 status=none
"$probe" space "$tmp/velký soubor" "$tmp/full"
"$probe" readonly "$tmp/velký soubor" "$tmp/ro"
mkdir -m 0555 "$tmp/protected"
runuser -u nobody -- "$probe" permission "$tmp/velký soubor" "$tmp/protected"
"$probe" pipe
echo "PASS ENOSPC, EROFS, EACCES and pipe drain deadline"

for version in 1 2; do
    mkdir -p "$tmp/pkg/DEBIAN" "$tmp/pkg/usr/share/doc/cz-zkouska-files"
    printf 'Package: cz-zkouska-files\nArchitecture: all\nVersion: 1.0.0\nMaintainer: Test <test@example.invalid>\nDescription: Fixture %s\n' "$version" >"$tmp/pkg/DEBIAN/control"
    printf '%s\n' "$version" >"$tmp/pkg/usr/share/doc/cz-zkouska-files/payload"
    dpkg-deb --build --root-owner-group "$tmp/pkg" "$tmp/balíček $version.deb"
done
deb="$tmp/balíček 1.deb"
ln -s "$deb" "$drive/odkaz.deb"
"$probe" symlink "$drive/odkaz.deb"
"$probe" review "$deb"

if [[ ! -d $appdir ]]; then
    mkdir -- "$appdir"
    created_appdir=true
fi
record_mount "$appdir" -t tmpfs -o mode=0755 tmpfs
mkdir "$appdir/bin"
install -o root -g root -m 0755 -- "$app" "$appdir/bin/M5CardputerZero-Files"
record_mount /run -t tmpfs tmpfs
mkdir "$tmp/store-state"
touch "$tmp/real-dpkg" "$tmp/real-apt"
record_mount "$tmp/real-dpkg" --bind /usr/bin/dpkg
record_mount "$tmp/real-apt" --bind /usr/bin/apt-get
export TEST_DIR="$tmp" TEST_LOG="$tmp/manager.log" M5APPSTORE_STATE_DIR="$tmp/store-state"

cat >"$tmp/sudo" <<'FAKE'
#!/bin/sh
set -eu
case "$*" in
    "-n -- /usr/share/APPLaunch/bin/M5CardputerZero-Files --files-install-reviewed-deb "*) ;;
    *) echo "unexpected privileged command"; exit 1;;
esac
shift 2
export SUDO_UID=0
exec "$@"
FAKE
cat >"$tmp/dpkg" <<'FAKE'
#!/bin/sh
set -eu
case "${1:-}" in
    --audit)
        if [ -f "$TEST_DIR/broken" ]; then echo "cz-zkouska-files is unpacked but unconfigured"; fi
        exit 0;;
    --force-confdef)
        [ "$#" = 4 ] && [ "$2" = --force-confold ] && [ "$3" = --install ] || exit 24
        [ "$(stat -c '%u:%a' "$4")" = 0:644 ]
        [ "$(stat -c '%u:%a' "$(dirname "$4")")" = 0:711 ]
        if runuser -u nobody -- test -w "$4"; then exit 25; fi
        printf 'dpkg %s\n' "$*" >>"$TEST_LOG"
        touch "$TEST_DIR/running"
        sleep 1
        if [ -n "${TEST_FAIL:-}" ]; then touch "$TEST_DIR/broken"; echo "fixture postinst failed"; exit 42; fi
        touch "$TEST_DIR/installed"
        exit 0;;
    --install) echo 'missing conffile options'; exit 26;;
    *) exec "$TEST_DIR/real-dpkg" "$@";;
esac
FAKE
cat >"$tmp/dpkg-query" <<'FAKE'
#!/bin/sh
set -eu
test -f "$TEST_DIR/installed"
printf 'install ok installed\n1.0.0'
FAKE
cat >"$tmp/apt-get" <<'FAKE'
#!/bin/sh
set -eu
if [ -z "${TEST_APT:-}" ]; then exec "$TEST_DIR/real-apt" "$@"; fi
for arg in "$@"; do
    case "$arg" in
        --simulate)
            printf 'Inst cz-files-fixture-dependency (1.0 Test [all])\nInst cz-zkouska-files (1.0.0 local-deb [all])\nConf cz-zkouska-files (1.0.0 local-deb [all])\n'
            exit 0;;
        Dpkg::Options::=--force-confdef) confdef=yes;;
        Dpkg::Options::=--force-confold) confold=yes;;
        DPkg::Pre-Install-Pkgs::=*) hook=${arg#*=};;
    esac
done
[ "${confdef:-}:${confold:-}" = yes:yes ]
[ -x "$hook" ]
if printf '/wrong/archive.deb\n' | "$hook"; then exit 23; fi
printf '%s\n' "$arg" | "$hook"
printf 'apt-get %s\n' "$*" >>"$TEST_LOG"
touch "$TEST_DIR/installed"
FAKE
chmod 0755 "$tmp"/{sudo,dpkg,dpkg-query,apt-get}
for tool in sudo dpkg dpkg-query apt-get; do
    record_mount "/usr/bin/$tool" --bind "$tmp/$tool"
done

"$probe" changed "$deb" "$tmp/balíček 2.deb"
[[ ! -e $TEST_LOG ]]
echo "PASS archive replacement rejected before dpkg"

"$probe" install "$deb" >"$tmp/first.log" &
first=$!
for ((try=0; try<400; ++try)); do
    [[ ! -e $tmp/running ]] || break
    sleep 0.02
done
[[ -e $tmp/running ]]
if "$probe" install "$deb" >"$tmp/second.log"; then exit 27; fi
wait "$first"
cat "$tmp/first.log" "$tmp/second.log"
grep -F 'Another Files or Store package transaction is running' "$tmp/second.log"
[[ $(grep -c '^dpkg ' "$TEST_LOG") == 1 ]]
echo "PASS overlapping Files transactions"

cat >"$appdir/bin/M5CardputerZero-AppStore" <<'FAKE'
#!/bin/sh
set -eu
[ "$#" = 8 ] && [ "$1" = --package-helper ] && [ "$2" = install ] &&
    [ "$3" = --package-value ] && [ "$5" = --package-transaction ] &&
    [ "$6" = files-local ] && [ "$7" = --package-pending-path ]
[ "$8" = "$M5APPSTORE_STATE_DIR/pending-package.json" ]
grep -F "\"deb_path\":\"$4\"" "$8"
grep -F 'expected_md5' "$8"
printf 'Store helper invoked\n' >>"$TEST_LOG"
exec dpkg --install "$4"
FAKE
chmod 0755 "$appdir/bin/M5CardputerZero-AppStore"
exec 8>"$M5APPSTORE_STATE_DIR/package-transaction.lock"
flock -n 8
if "$probe" install "$deb" >"$tmp/busy-store.log"; then exit 28; fi
grep -F 'Another Files or Store package transaction is running' "$tmp/busy-store.log"
flock -u 8
exec 8>&-
"$probe" install "$deb"
grep -F 'Store helper invoked' "$TEST_LOG"
echo "PASS Store lock, helper, flags and protected input"

TEST_APT=1 "$probe" install "$deb" | tee "$tmp/apt.log"
grep -F 'Inst cz-files-fixture-dependency' "$tmp/apt.log"
grep -F 'apt-get -y --reinstall --no-remove --no-install-recommends' "$TEST_LOG"
echo "PASS Store APT dependencies, conffile flags and archive check"

TEST_FAIL=1 "$probe" install-failure "$deb" | tee "$tmp/failure.log"
grep -F 'fixture postinst failed' "$tmp/failure.log"
grep -F 'dpkg --audit reported unfinished packages' "$tmp/failure.log"
echo "PASS failed installation audit and recovery guidance"
