#!/bin/bash
# Verify btrfstune --backfill-simple-quota: all existing extents are accounted,
# data extents are owned by the lowest subvolume id referencing them, the
# result passes check, and the kernel keeps it consistent when freeing
# backfilled extents.

source "$TEST_TOP/common" || exit

check_prereq mkfs.btrfs
check_prereq btrfstune
check_prereq btrfs

if ! [ -f "/sys/fs/btrfs/features/simple_quota" ]; then
	_not_run "kernel does not support simple quotas"
fi

setup_root_helper
prepare_test_dev

# Print exclusive bytes of qgroup 0/$1
qgroup_excl()
{
	run_check_stdout $SUDO_HELPER "$TOP/btrfs" qgroup show --raw "$TEST_MNT" |
		awk -v id="0/$1" '$1 == id { print $3 }'
}

subvol_id()
{
	run_check_stdout $SUDO_HELPER "$TOP/btrfs" inspect-internal rootid "$1"
}

backfill()
{
	run_check $SUDO_HELPER "$TOP/btrfstune" --backfill-simple-quota "$TEST_DEV"
	run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
}

# Populate with snapshots, a reflink and a deleted snapshot origin. Small
# nodesize and many inodes make the subvolume trees at least 3 levels deep, so
# leaves are shared implicitly through shared nodes.
#
# $1: enable simple quotas with the kernel midway
populate()
{
	local i

	run_check_mkfs_test_dev -n 4096
	run_check_mount_test_dev
	run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/a"
	run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/b"
	run_check $SUDO_HELPER "$TOP/btrfs" subvolume create "$TEST_MNT/d"
	for i in $(seq 1 1500); do
		echo "file $i" | $SUDO_HELPER tee "$TEST_MNT/a/small$i" > /dev/null
	done
	for i in 1 2 3; do
		run_check $SUDO_HELPER dd if=/dev/urandom of="$TEST_MNT/a/big$i" \
			bs=1M count=4 status=none
	done
	run_check $SUDO_HELPER dd if=/dev/urandom of="$TEST_MNT/d/file" \
		bs=1M count=3 status=none
	for i in $(seq 1 500); do
		run_check $SUDO_HELPER touch "$TEST_MNT/d/small$i"
	done
	run_check $SUDO_HELPER cp --reflink=always "$TEST_MNT/a/big1" "$TEST_MNT/b/ref1"
	run_check $SUDO_HELPER sync

	if [ "$1" = "midway" ]; then
		run_check $SUDO_HELPER "$TOP/btrfs" quota enable --simple "$TEST_MNT"
		run_check $SUDO_HELPER dd if=/dev/urandom of="$TEST_MNT/b/post" \
			bs=1M count=1 status=none
	fi

	run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot "$TEST_MNT/a" "$TEST_MNT/s"
	run_check $SUDO_HELPER "$TOP/btrfs" subvolume snapshot "$TEST_MNT/d" "$TEST_MNT/e"
	# Diverge the origin from its snapshot
	run_check $SUDO_HELPER dd if=/dev/urandom of="$TEST_MNT/a/big2" \
		bs=1M count=1 conv=notrunc status=none
	for i in $(seq 1 300); do
		echo "new $i" | $SUDO_HELPER tee "$TEST_MNT/a/new$i" > /dev/null
	done
	run_check $SUDO_HELPER find "$TEST_MNT/a" -name 'small1*' -delete

	id_a=$(subvol_id "$TEST_MNT/a")
	id_b=$(subvol_id "$TEST_MNT/b")
	id_d=$(subvol_id "$TEST_MNT/d")
	id_s=$(subvol_id "$TEST_MNT/s")
	id_e=$(subvol_id "$TEST_MNT/e")
	run_check $SUDO_HELPER "$TOP/btrfs" subvolume delete "$TEST_MNT/d"
	run_check $SUDO_HELPER "$TOP/btrfs" subvolume sync "$TEST_MNT"
	run_check_umount_test_dev
}

# Free backfilled extents through the kernel, then verify with check
kernel_ops()
{
	run_check_mount_test_dev
	run_check $SUDO_HELPER rm -f "$TEST_MNT/a/big1" "$TEST_MNT/s/big3" "$TEST_MNT/b/ref1"
	run_check $SUDO_HELPER "$TOP/btrfs" subvolume delete "$TEST_MNT/s"
	run_check $SUDO_HELPER dd if=/dev/urandom of="$TEST_MNT/a/big2" \
		bs=1M count=4 conv=notrunc status=none
	run_check $SUDO_HELPER find "$TEST_MNT/e" -name 'small2*' -delete
	run_check $SUDO_HELPER find "$TEST_MNT/a" -name 'small2*' -delete
	run_check $SUDO_HELPER "$TOP/btrfs" subvolume sync "$TEST_MNT"
	run_check_umount_test_dev
	run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"
}

# $1: least bytes, $2: most bytes, $3: qgroup subvolid, $4: description
check_excl()
{
	local excl

	excl=$(qgroup_excl "$3")
	[ -n "$excl" ] || _fail "qgroup 0/$3 ($4) not found"
	if [ "$excl" -lt "$1" ] || [ "$excl" -gt "$2" ]; then
		_fail "qgroup 0/$3 ($4): exclusive $excl not in [$1, $2]"
	fi
}

mib=$((1024 * 1024))

_log "Backfill a filesystem without simple quotas"
populate
backfill
run_check_mount_test_dev
# Shared and reflinked data goes to the lowest id: a, not its snapshot s or b
check_excl $((12 * mib)) $((32 * mib)) "$id_a" "origin"
check_excl 1 $((1 * mib - 1)) "$id_b" "reflink target"
check_excl 1 $((1 * mib - 1)) "$id_s" "snapshot"
# The snapshot of a deleted origin takes its data, the deleted origin is still
# charged for the tree blocks shared with the snapshot
check_excl $((3 * mib)) $((4 * mib)) "$id_e" "snapshot of deleted"
check_excl 1 $((1 * mib - 1)) "$id_d" "deleted origin"
run_check_umount_test_dev
kernel_ops

_log "Backfill again, nothing to do"
out=$(run_check_stdout $SUDO_HELPER "$TOP/btrfstune" --backfill-simple-quota "$TEST_DEV")
echo "$out" | grep -q "added owner refs to 0 data extents" ||
	_fail "backfill was not idempotent"
run_check $SUDO_HELPER "$TOP/btrfs" check "$TEST_DEV"

_log "Backfill a filesystem with simple quotas enabled midway"
populate midway
backfill
run_check_mount_test_dev
# Data written after enabling keeps its owner
check_excl $((1 * mib)) $((2 * mib)) "$id_b" "owner of post-enable data"
run_check_umount_test_dev
kernel_ops

_log "Refuse a filesystem with qgroups"
run_check_mkfs_test_dev
run_check_mount_test_dev
run_check $SUDO_HELPER "$TOP/btrfs" quota enable "$TEST_MNT"
run_check_umount_test_dev
run_mustfail "backfill with qgroups enabled" \
	$SUDO_HELPER "$TOP/btrfstune" --backfill-simple-quota "$TEST_DEV"
