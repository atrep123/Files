#!/usr/bin/env bash
set -euo pipefail

if [[ $# != 1 || $EUID != 0 ]]; then
    echo "Usage (root, mount namespace recommended): $0 /path/to/storage_package_tests" >&2
    exit 2
fi

tmp=$(mktemp -d)
mount_dir=$(mktemp -d /media/files-test-XXXXXX)
loop_device=''
cleanup() {
    if mountpoint -q -- "$mount_dir"; then umount -- "$mount_dir"; fi
    if [[ -n $loop_device ]]; then losetup --detach "$loop_device"; fi
    rm -f -- "$tmp/drive.img" "$mount_dir/from-drive.txt"
    rmdir -- "$mount_dir" "$tmp"
}
trap cleanup EXIT
truncate -s 48M "$tmp/drive.img"
mkfs.ext4 -q -F -L "Files USB" "$tmp/drive.img"
loop_device=$(losetup --find --show "$tmp/drive.img")
mount -o nosuid,nodev,noexec "$loop_device" "$mount_dir"
"$1" "$mount_dir"
rm -- "$mount_dir/from-drive.txt"
mount -o nosuid,nodev,noexec "$loop_device" "$mount_dir"
"$1" "$mount_dir" --disconnect-during-copy
