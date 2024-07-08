#include "kerncompat.h"
#include <stdio.h>
#include "kernel-shared/uapi/btrfs_tree.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/transaction.h"
#include "tune.h"

static int remove_one_owner_ref(struct btrfs_fs_info *fs_info,
				struct btrfs_trans_handle *trans)
{
	return 0;
}

int remove_squota(struct btrfs_fs_info *fs_info)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_root *extent_root;
	struct btrfs_key key;
	struct extent_buffer *leaf;
	struct btrfs_path path = { 0 };
	int ret;

	printf("Removing squotas!\n");

	extent_root = btrfs_extent_root(fs_info, 0);

	trans = btrfs_start_transaction(extent_root, 0);

	key.objectid = 0;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(trans, extent_root, &key, &path, 1, 1);
	if (ret < 0)
		return ret;
	leaf = path.nodes[0];

	while (1) {
		struct btrfs_key found_key;
		struct btrfs_extent_item *ei;
		struct btrfs_extent_inline_ref *iref;
		int slot = path.slots[0];
		u64 item_end;
		u8 type;
		u32 nr;
		unsigned long ptr;
		unsigned long rest;
		unsigned long leaf_end;

		if (slot >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(extent_root, &path);
			if (ret < 0) {
				break;
			} else if (ret) {
				ret = 0;
				break;
			}
			leaf = path.nodes[0];
			slot = path.slots[0];
		}

		btrfs_item_key_to_cpu(leaf, &found_key, path.slots[0]);
		if (found_key.type != BTRFS_EXTENT_ITEM_KEY)
			goto next;
		ei = btrfs_item_ptr(leaf, slot, struct btrfs_extent_item);
		ptr = (unsigned long)(ei + 1);
		item_end = (unsigned long)ei + btrfs_item_size(leaf, slot);
		if (ptr > item_end)
			goto next;
		iref = (struct btrfs_extent_inline_ref *)ptr;
		type = btrfs_extent_inline_ref_type(leaf, iref);
		if (type == BTRFS_EXTENT_OWNER_REF_KEY) {
			nr = btrfs_header_nritems(leaf);
			rest = (unsigned long)(iref + 1);
			leaf_end = btrfs_item_offset(leaf, nr - 1);
			printf("found an owner ref extent item %llu %u %llu; need to nuke!\n",
			       found_key.objectid, found_key.type, found_key.offset);
			memmove_extent_buffer(leaf, ptr, rest, leaf_end - rest);
			btrfs_set_item_size(leaf, slot,
					    btrfs_item_size(leaf, slot) - sizeof(*iref));
			btrfs_mark_buffer_dirty(leaf);
		}
next:
		path.slots[0]++;
	}

	btrfs_release_path(&path);
	btrfs_commit_transaction(trans, extent_root);
	return 0;
}
