#include <errno.h>
#include "kernel-shared/ctree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/transaction.h"
#include "kernel-shared/uapi/btrfs_tree.h"
#include "kernel-shared/ulist.h"
#include "common/messages.h"
#include "check/qgroup-verify.h"
#include "tune/tune.h"

static int remove_quota_tree(struct btrfs_fs_info *fs_info)
{
	int ret;
	struct btrfs_root *quota_root = fs_info->quota_root;
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_super_block *sb = fs_info->super_copy;
	int super_flags = btrfs_super_incompat_flags(sb);
	struct btrfs_trans_handle *trans;

	trans = btrfs_start_transaction(quota_root, 0);
	ret = btrfs_clear_tree(trans, quota_root);
	if (ret) {
		btrfs_abort_transaction(trans, ret);
		return ret;
	}

	ret = btrfs_delete_and_free_root(trans, quota_root);
	if (ret) {
		btrfs_abort_transaction(trans, ret);
		return ret;
	}
	fs_info->quota_root = NULL;
	super_flags &= ~BTRFS_FEATURE_INCOMPAT_SIMPLE_QUOTA;
	btrfs_set_super_incompat_flags(sb, super_flags);
	btrfs_commit_transaction(trans, tree_root);
	return 0;
}

/*
 * Given a pointer (ptr) into DATAi (i = slot), and an amount to shift,
 * move all the data to the left (slots >= slot) of that ptr to the right by
 * the shift amount. This overwrites the shift bytes after ptr, effectively
 * removing them from the item data. We must update affected item sizes (only
 * at slot) and offsets (slots >= slot).
 *
 * Leaf view, using '-' to show shift scale:
 * Before:
 * [ITEM0,...,ITEMi,...,ITEMn,-------,DATAn,...,[---DATAi---],...,DATA0]
 * After:
 * [ITEM0,...,ITEMi,...,ITEMn,--------,DATAn,...,[--DATAi---],...,DATA0]
 *
 * Zooming in on DATAi
 * (ptr points at the start of the Ys, and shift is length of the Ys)
 * Before:
 * ...[DATAi+1][XXXXXXXXXXXXYYYYYYYYYYYYYYYYXXXXXXX][DATAi-1]...
 * After:
 * ...................[DATAi+1][XXXXXXXXXXXXXXXXXXX][DATAi-1]...
 * Note that DATAi-1 and smaller are not affected.
 */
static void shift_leaf_data(struct btrfs_trans_handle *trans,
			    struct extent_buffer *leaf, int slot,
			    unsigned long ptr, u32 shift)
{
	u32 nr = btrfs_header_nritems(leaf);
	u32 leaf_data_off = btrfs_item_ptr_offset(leaf, nr - 1);
	u32 len = ptr - leaf_data_off;
	u32 new_size = btrfs_item_size(leaf, slot) - shift;
	for (int i = slot; i < nr; i++) {
		u32 old_item_offset = btrfs_item_offset(leaf, i);
		btrfs_set_item_offset(leaf, i, old_item_offset + shift);
	}
	memmove_extent_buffer(leaf, leaf_data_off + shift, leaf_data_off, len);
	btrfs_set_item_size(leaf, slot, new_size);
	btrfs_set_header_generation(leaf, trans->transid);
	btrfs_mark_buffer_dirty(leaf);
}

/*
 * Iterate over the extent tree and for each EXTENT_DATA item that has an inline
 * ref of type OWNER_REF, shift that leaf to eliminate the owner ref.
 *
 * Note: we use a search_slot per leaf rather than find_next_leaf to get the
 * needed CoW-ing and rebalancing for each leaf and its path up to the root.
 */
static int remove_owner_refs(struct btrfs_fs_info *fs_info)
{
	struct btrfs_trans_handle *trans;
	struct btrfs_root *extent_root;
	struct btrfs_key key;
	struct extent_buffer *leaf;
	struct btrfs_path path = { 0 };
	int slot;
	int ret;

	extent_root = btrfs_extent_root(fs_info, 0);

	trans = btrfs_start_transaction(extent_root, 0);

	key.objectid = 0;
	key.type = BTRFS_EXTENT_ITEM_KEY;
	key.offset = 0;

search_slot:
	ret = btrfs_search_slot(trans, extent_root, &key, &path, 1, 1);
	if (ret < 0)
		return ret;
	leaf = path.nodes[0];
	slot = path.slots[0];

	while (1) {
		struct btrfs_key found_key;
		struct btrfs_extent_item *ei;
		struct btrfs_extent_inline_ref *iref;
		u8 type;
		unsigned long ptr;
		unsigned long item_end;

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
			btrfs_item_key_to_cpu(leaf, &key, slot);
			btrfs_release_path(&path);
			goto search_slot;
		}

		btrfs_item_key_to_cpu(leaf, &found_key, slot);
		if (found_key.type != BTRFS_EXTENT_ITEM_KEY)
			goto next;
		ei = btrfs_item_ptr(leaf, slot, struct btrfs_extent_item);
		ptr = (unsigned long)(ei + 1);
		item_end = (unsigned long)ei + btrfs_item_size(leaf, slot);
		/* No inline extent references; accessing type is invalid. */
		if (ptr > item_end)
			goto next;
		iref = (struct btrfs_extent_inline_ref *)ptr;
		type = btrfs_extent_inline_ref_type(leaf, iref);
		if (type == BTRFS_EXTENT_OWNER_REF_KEY)
			shift_leaf_data(trans, leaf, slot, ptr, sizeof(*iref));
next:
		slot++;
	}
	btrfs_release_path(&path);

	ret = btrfs_commit_transaction(trans, extent_root);
	if (ret < 0) {
		errno = -ret;
		error_msg(ERROR_MSG_COMMIT_TRANS, "%m");
		return ret;
	}
	return 0;
}

int remove_squota(struct btrfs_fs_info *fs_info)
{
	int ret;

	ret = remove_owner_refs(fs_info);
	if (ret)
		return ret;

	return remove_quota_tree(fs_info);
}

static int create_qgroup(struct btrfs_fs_info *fs_info,
			 struct btrfs_trans_handle *trans,
			 u64 qgroupid)
{
	struct btrfs_path path = { 0 };
	struct btrfs_root *quota_root = fs_info->quota_root;
	struct btrfs_key key;
	int ret;

	if (qgroupid >> BTRFS_QGROUP_LEVEL_SHIFT) {
		error("qgroup level other than 0 is not supported yet");
		return -ENOTTY;
	}

	key.objectid = 0;
	key.type = BTRFS_QGROUP_INFO_KEY;
	key.offset = qgroupid;

	ret = btrfs_insert_empty_item(trans, quota_root, &path, &key,
				      sizeof(struct btrfs_qgroup_info_item));
	btrfs_release_path(&path);
	if (ret < 0)
		return ret;

	key.objectid = 0;
	key.type = BTRFS_QGROUP_LIMIT_KEY;
	key.offset = qgroupid;
	ret = btrfs_insert_empty_item(trans, quota_root, &path, &key,
				      sizeof(struct btrfs_qgroup_limit_item));
	btrfs_release_path(&path);

	printf("created qgroup for %llu\n", qgroupid);
	return ret;
}

static int create_qgroups(struct btrfs_fs_info *fs_info,
			  struct btrfs_trans_handle *trans)
{
	struct btrfs_key key = {
		.objectid = 0,
		.type = BTRFS_ROOT_REF_KEY,
		.offset = 0,
	};
	struct btrfs_path path = { 0 };
	struct extent_buffer *leaf;
	int slot;
	struct btrfs_root *tree_root = fs_info->tree_root;
	int ret;


	ret = create_qgroup(fs_info, trans, BTRFS_FS_TREE_OBJECTID);
	if (ret)
		goto out;

	ret = btrfs_search_slot_for_read(tree_root, &key, &path, 1, 0);
	if (ret)
		goto out;

	while (1) {
		slot = path.slots[0];
		leaf = path.nodes[0];
		btrfs_item_key_to_cpu(leaf, &key, slot);
		if (key.type == BTRFS_ROOT_REF_KEY) {
			ret = create_qgroup(fs_info, trans, key.offset);
			if (ret)
				goto out;
		}
		ret = btrfs_next_item(tree_root, &path);
		if (ret < 0) {
			error("failed to advance to next item");
			goto out;
		}
		if (ret)
			break;
	}

out:
	btrfs_release_path(&path);
	return ret;
}

int enable_quota(struct btrfs_fs_info *fs_info, bool simple)
{
	struct btrfs_super_block *sb = fs_info->super_copy;
	struct btrfs_trans_handle *trans;
	int super_flags = btrfs_super_incompat_flags(sb);
	struct btrfs_qgroup_status_item *qsi;
	struct btrfs_root *quota_root;
	struct btrfs_path path = { 0 };
	struct btrfs_key key;
	int flags;
	int ret;

	trans = btrfs_start_transaction(fs_info->tree_root, 2);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		errno = -ret;
		error_msg(ERROR_MSG_START_TRANS, "%m");
		return ret;
	}

	ret = btrfs_create_root(trans, fs_info, BTRFS_QUOTA_TREE_OBJECTID);
	if (ret < 0) {
		error("failed to create quota root: %d (%m)", ret);
		goto fail;
	}
	quota_root = fs_info->quota_root;

	/* Create the qgroup status item */
	key.objectid = 0;
	key.type = BTRFS_QGROUP_STATUS_KEY;
	key.offset = 0;

	ret = btrfs_insert_empty_item(trans, quota_root, &path, &key,
				      sizeof(*qsi));
	if (ret < 0) {
		error("failed to insert qgroup status item: %d (%m)", ret);
		goto fail;
	}

	qsi = btrfs_item_ptr(path.nodes[0], path.slots[0],
			     struct btrfs_qgroup_status_item);
	btrfs_set_qgroup_status_generation(path.nodes[0], qsi, trans->transid);
	btrfs_set_qgroup_status_rescan(path.nodes[0], qsi, 0);
	flags = BTRFS_QGROUP_STATUS_FLAG_ON;
	if (simple) {
		btrfs_set_qgroup_status_enable_gen(path.nodes[0], qsi, trans->transid);
		flags |= BTRFS_QGROUP_STATUS_FLAG_SIMPLE_MODE;
	} else {
		flags |= BTRFS_QGROUP_STATUS_FLAG_INCONSISTENT;
	}

	btrfs_set_qgroup_status_version(path.nodes[0], qsi, 1);
	btrfs_set_qgroup_status_flags(path.nodes[0], qsi, flags);
	btrfs_release_path(&path);

	/* Create the qgroup items */
	ret = create_qgroups(fs_info, trans);
	if (ret < 0) {
		error("failed to create qgroup items for subvols %d (%m)", ret);
		goto fail;
	}

	/* Set squota incompat flag */
	if (simple) {
		super_flags |= BTRFS_FEATURE_INCOMPAT_SIMPLE_QUOTA;
		btrfs_set_super_incompat_flags(sb, super_flags);
	}

	ret = btrfs_commit_transaction(trans, fs_info->tree_root);
	if (ret < 0) {
		errno = -ret;
		error_msg(ERROR_MSG_COMMIT_TRANS, "%m");
		return ret;
	}
	return ret;
fail:
	btrfs_abort_transaction(trans, ret);
	return ret;
}

/*
 * Backfill simple quotas over the whole filesystem.
 *
 * Squota usage is a pure function of the extent tree: every extent with
 * generation >= enable_gen is charged to its owner, which is the inline
 * OWNER_REF for data and the header owner for tree blocks. Tree blocks
 * always have an owner, so with enable_gen lowered to 0 all metadata is
 * already accounted. Data extents written before squota was enabled lack
 * an OWNER_REF, so give them one: walk every fs tree in ascending id order
 * and let the first tree that reaches an extent claim it, i.e. an extent is
 * owned by the lowest-id subvol referencing it. No backref resolution is
 * needed, and subtrees already visited from a lower id are skipped, so
 * every tree block is read once.
 *
 * Finally measure the usage with the qgroup verifier from fsck and write it
 * to the qgroup items. All steps are idempotent: if interrupted, run again.
 */

/* Owner refs to insert before committing the transaction. */
#define BACKFILL_BATCH		4096

struct backfill_ctx {
	struct btrfs_fs_info *fs_info;
	struct btrfs_root *extent_root;
	struct btrfs_trans_handle *trans;
	/* Fs tree blocks already walked. */
	struct ulist visited;
	/* Fs tree ids that are charged for something and need a qgroup. */
	struct ulist owners;
	u64 nr_pending;
	u64 nr_owned;
	u64 owned_bytes;
};

static int backfill_commit(struct backfill_ctx *ctx)
{
	int ret;

	ret = btrfs_commit_transaction(ctx->trans, ctx->extent_root);
	ctx->trans = NULL;
	ctx->nr_pending = 0;
	if (ret < 0) {
		errno = -ret;
		error_msg(ERROR_MSG_COMMIT_TRANS, "%m");
	}
	return ret;
}

static int backfill_start(struct backfill_ctx *ctx)
{
	struct btrfs_trans_handle *trans;

	trans = btrfs_start_transaction(ctx->extent_root, 1);
	if (IS_ERR(trans)) {
		errno = -PTR_ERR(trans);
		error_msg(ERROR_MSG_START_TRANS, "%m");
		return PTR_ERR(trans);
	}
	ctx->trans = trans;
	return 0;
}

/*
 * Refuse states where some extents are referenced only from trees we do
 * not walk, or charged to trees we cannot see: log trees, relocation in
 * progress, and subvolumes deleted but not yet cleaned up. Collect the ids
 * of all live fs trees in ascending order.
 */
static int backfill_scan_roots(struct btrfs_fs_info *fs_info,
			       struct ulist *fs_roots)
{
	struct btrfs_root *tree_root = fs_info->tree_root;
	struct btrfs_path path = { 0 };
	struct btrfs_key key = { 0 };
	int ret;

	if (btrfs_super_log_root(fs_info->super_copy)) {
		error("log tree present, mount the filesystem to replay it first");
		return -EINVAL;
	}

	ret = btrfs_search_slot(NULL, tree_root, &key, &path, 0, 0);
	if (ret < 0)
		return ret;
	while (1) {
		struct extent_buffer *leaf = path.nodes[0];
		struct btrfs_root_item *ri;

		if (path.slots[0] >= btrfs_header_nritems(leaf)) {
			ret = btrfs_next_leaf(tree_root, &path);
			if (ret < 0)
				goto out;
			if (ret > 0)
				break;
			continue;
		}
		btrfs_item_key_to_cpu(leaf, &key, path.slots[0]);

		if (key.objectid == BTRFS_BALANCE_OBJECTID ||
		    (key.type == BTRFS_ROOT_ITEM_KEY &&
		     key.objectid == BTRFS_TREE_RELOC_OBJECTID)) {
			error("relocation in progress, finish or cancel the balance first");
			ret = -EINVAL;
			goto out;
		}
		if (key.objectid == BTRFS_ORPHAN_OBJECTID &&
		    key.type == BTRFS_ORPHAN_ITEM_KEY) {
			error("subvolume %llu is pending deletion, mount the filesystem to let it be cleaned up first",
			      key.offset);
			ret = -EINVAL;
			goto out;
		}
		if (key.type == BTRFS_ROOT_ITEM_KEY && is_fstree(key.objectid)) {
			ri = btrfs_item_ptr(leaf, path.slots[0],
					    struct btrfs_root_item);
			if (btrfs_disk_root_refs(leaf, ri) == 0) {
				error("subvolume %llu is pending deletion, mount the filesystem to let it be cleaned up first",
				      key.objectid);
				ret = -EINVAL;
				goto out;
			}
			ret = ulist_add(fs_roots, key.objectid, 0, 0);
			if (ret < 0)
				goto out;
		}
		path.slots[0]++;
	}
	ret = 0;
out:
	btrfs_release_path(&path);
	return ret;
}

/*
 * Give the data extent at @bytenr an OWNER_REF for @root_id unless it
 * already has one. OWNER_REF sorts before all other inline refs, so an
 * existing one is the first, and a new one is inserted at the front.
 */
static int backfill_data_extent(struct backfill_ctx *ctx, u64 bytenr,
				u64 num_bytes, u64 root_id)
{
	struct btrfs_key key = {
		.objectid = bytenr,
		.type = BTRFS_EXTENT_ITEM_KEY,
		.offset = num_bytes,
	};
	struct btrfs_path path = { 0 };
	struct btrfs_extent_inline_ref *iref;
	struct btrfs_extent_item *ei;
	struct extent_buffer *leaf;
	const u32 size = sizeof(*iref);
	unsigned long ptr;
	unsigned long end;
	int ret;

	/* Read-only lookup first: most extents are visited more than once. */
	ret = btrfs_search_slot(NULL, ctx->extent_root, &key, &path, 0, 0);
	if (ret > 0) {
		error("missing extent item for data extent %llu %llu",
		      bytenr, num_bytes);
		ret = -EUCLEAN;
	}
	if (ret < 0)
		goto out;
	leaf = path.nodes[0];
	ei = btrfs_item_ptr(leaf, path.slots[0], struct btrfs_extent_item);
	if (btrfs_extent_flags(leaf, ei) & BTRFS_EXTENT_FLAG_TREE_BLOCK) {
		error("data extent %llu %llu is a tree block", bytenr, num_bytes);
		ret = -EUCLEAN;
		goto out;
	}
	ptr = (unsigned long)(ei + 1);
	end = (unsigned long)ei + btrfs_item_size(leaf, path.slots[0]);
	if (ptr < end) {
		iref = (struct btrfs_extent_inline_ref *)ptr;
		if (btrfs_extent_inline_ref_type(leaf, iref) ==
		    BTRFS_EXTENT_OWNER_REF_KEY)
			goto out;
	}
	btrfs_release_path(&path);

	path.search_for_extension = 1;
	ret = btrfs_search_slot(ctx->trans, ctx->extent_root, &key, &path,
				size, 1);
	if (ret > 0)
		ret = -EUCLEAN;
	if (ret < 0)
		goto out;

	btrfs_extend_item(&path, size);
	leaf = path.nodes[0];
	ei = btrfs_item_ptr(leaf, path.slots[0], struct btrfs_extent_item);
	ptr = (unsigned long)(ei + 1);
	end = (unsigned long)ei + btrfs_item_size(leaf, path.slots[0]);
	memmove_extent_buffer(leaf, ptr + size, ptr, end - size - ptr);
	iref = (struct btrfs_extent_inline_ref *)ptr;
	btrfs_set_extent_inline_ref_type(leaf, iref, BTRFS_EXTENT_OWNER_REF_KEY);
	btrfs_set_extent_inline_ref_offset(leaf, iref, root_id);
	btrfs_mark_buffer_dirty(leaf);
	btrfs_release_path(&path);

	ctx->nr_owned++;
	ctx->owned_bytes += num_bytes;
	if (++ctx->nr_pending >= BACKFILL_BATCH) {
		ret = backfill_commit(ctx);
		if (ret < 0)
			return ret;
		return backfill_start(ctx);
	}
	return 0;
out:
	btrfs_release_path(&path);
	return ret;
}

static int backfill_tree_block(struct backfill_ctx *ctx,
			       struct extent_buffer *eb, u64 root_id)
{
	u64 owner = btrfs_header_owner(eb);
	int nritems = btrfs_header_nritems(eb);
	int ret;
	int i;

	if (is_fstree(owner)) {
		ret = ulist_add(&ctx->owners, owner, 0, 0);
		if (ret < 0)
			return ret;
	}

	if (btrfs_header_level(eb) > 0) {
		for (i = 0; i < nritems; i++) {
			struct extent_buffer *child;

			/* Reached from a lower id, already claimed. */
			ret = ulist_add(&ctx->visited,
					btrfs_node_blockptr(eb, i), 0, 0);
			if (ret < 0)
				return ret;
			if (ret == 0)
				continue;
			child = btrfs_read_node_slot(eb, i);
			if (IS_ERR(child))
				return PTR_ERR(child);
			ret = backfill_tree_block(ctx, child, root_id);
			free_extent_buffer(child);
			if (ret < 0)
				return ret;
		}
		return 0;
	}

	for (i = 0; i < nritems; i++) {
		struct btrfs_file_extent_item *fi;
		struct btrfs_key key;
		u64 bytenr;
		u8 type;

		btrfs_item_key_to_cpu(eb, &key, i);
		if (key.type != BTRFS_EXTENT_DATA_KEY)
			continue;
		fi = btrfs_item_ptr(eb, i, struct btrfs_file_extent_item);
		type = btrfs_file_extent_type(eb, fi);
		if (type != BTRFS_FILE_EXTENT_REG &&
		    type != BTRFS_FILE_EXTENT_PREALLOC)
			continue;
		bytenr = btrfs_file_extent_disk_bytenr(eb, fi);
		if (bytenr == 0)
			continue;
		ret = backfill_data_extent(ctx, bytenr,
				btrfs_file_extent_disk_num_bytes(eb, fi),
				root_id);
		if (ret < 0)
			return ret;
	}
	return 0;
}

/* Create qgroup 0/@qgroupid with zeroed info and limit items, if missing. */
static int ensure_qgroup(struct btrfs_trans_handle *trans,
			 struct btrfs_root *quota_root, u64 qgroupid)
{
	static const struct {
		u8 type;
		u32 size;
	} items[] = {
		{ BTRFS_QGROUP_INFO_KEY, sizeof(struct btrfs_qgroup_info_item) },
		{ BTRFS_QGROUP_LIMIT_KEY, sizeof(struct btrfs_qgroup_limit_item) },
	};
	struct btrfs_path path = { 0 };
	int ret;

	for (int i = 0; i < ARRAY_SIZE(items); i++) {
		struct btrfs_key key = {
			.objectid = 0,
			.type = items[i].type,
			.offset = qgroupid,
		};
		struct extent_buffer *leaf;

		ret = btrfs_insert_empty_item(trans, quota_root, &path, &key,
					      items[i].size);
		if (ret == -EEXIST) {
			btrfs_release_path(&path);
			continue;
		}
		if (ret < 0)
			return ret;
		leaf = path.nodes[0];
		memset_extent_buffer(leaf, 0,
				     btrfs_item_ptr_offset(leaf, path.slots[0]),
				     items[i].size);
		btrfs_mark_buffer_dirty(leaf);
		btrfs_release_path(&path);
		if (i == 0)
			printf("created qgroup 0/%llu\n", qgroupid);
	}
	return 0;
}

/*
 * Create qgroups for all owners, count all extents from generation 0, and
 * mark the counts inconsistent until they are measured and written.
 */
static int backfill_prepare_quota(struct backfill_ctx *ctx)
{
	struct btrfs_fs_info *fs_info = ctx->fs_info;
	struct btrfs_root *quota_root = fs_info->quota_root;
	struct btrfs_key key = {
		.objectid = 0,
		.type = BTRFS_QGROUP_STATUS_KEY,
		.offset = 0,
	};
	struct btrfs_path path = { 0 };
	struct btrfs_qgroup_status_item *qsi;
	struct extent_buffer *leaf;
	struct ulist_iterator uiter;
	struct ulist_node *unode;
	int ret;

	ULIST_ITER_INIT(&uiter);
	while ((unode = ulist_next(&ctx->owners, &uiter))) {
		ret = ensure_qgroup(ctx->trans, quota_root, unode->val);
		if (ret < 0)
			return ret;
	}

	ret = btrfs_search_slot(ctx->trans, quota_root, &key, &path, 0, 1);
	if (ret > 0)
		ret = -ENOENT;
	if (ret < 0) {
		btrfs_release_path(&path);
		return ret;
	}
	leaf = path.nodes[0];
	qsi = btrfs_item_ptr(leaf, path.slots[0],
			     struct btrfs_qgroup_status_item);
	btrfs_set_qgroup_status_enable_gen(leaf, qsi, 0);
	btrfs_set_qgroup_status_flags(leaf, qsi,
			btrfs_qgroup_status_flags(leaf, qsi) |
			BTRFS_QGROUP_STATUS_FLAG_INCONSISTENT);
	btrfs_mark_buffer_dirty(leaf);
	btrfs_release_path(&path);
	return 0;
}

int backfill_squota(struct btrfs_fs_info *fs_info)
{
	struct backfill_ctx ctx = {
		.fs_info = fs_info,
		.extent_root = btrfs_extent_root(fs_info, 0),
	};
	struct ulist fs_roots;
	struct ulist_iterator uiter;
	struct ulist_node *unode;
	int repaired = 0;
	int ret;

	if (fs_info->quota_root && !btrfs_fs_incompat(fs_info, SIMPLE_QUOTA)) {
		error("qgroups are enabled, remove them before enabling simple quotas");
		return -EINVAL;
	}

	ulist_init(&fs_roots);
	ulist_init(&ctx.visited);
	ulist_init(&ctx.owners);

	ret = backfill_scan_roots(fs_info, &fs_roots);
	if (ret < 0)
		goto out;

	if (!fs_info->quota_root) {
		ret = enable_quota(fs_info, true);
		if (ret < 0)
			goto out;
	}

	ret = backfill_start(&ctx);
	if (ret < 0)
		goto out;

	/* ulist iterates in insertion order, i.e. ascending ids. */
	ULIST_ITER_INIT(&uiter);
	while ((unode = ulist_next(&fs_roots, &uiter))) {
		struct btrfs_key key = {
			.objectid = unode->val,
			.type = BTRFS_ROOT_ITEM_KEY,
			.offset = (u64)-1,
		};
		struct btrfs_root *root;

		root = btrfs_read_fs_root(fs_info, &key);
		if (IS_ERR(root)) {
			ret = PTR_ERR(root);
			errno = -ret;
			error("failed to read fs tree %llu: %m", unode->val);
			goto abort;
		}
		ret = ulist_add(&ctx.owners, unode->val, 0, 0);
		if (ret < 0)
			goto abort;
		ret = ulist_add(&ctx.visited, root->node->start, 0, 0);
		if (ret < 0)
			goto abort;
		if (ret == 0)
			continue;
		ret = backfill_tree_block(&ctx, root->node, unode->val);
		if (ret < 0) {
			errno = -ret;
			error("failed to backfill fs tree %llu: %m", unode->val);
			goto abort;
		}
	}
	printf("added owner refs to %llu data extents (%llu bytes)\n",
	       ctx.nr_owned, ctx.owned_bytes);

	ret = backfill_prepare_quota(&ctx);
	if (ret < 0) {
		errno = -ret;
		error("failed to update quota tree: %m");
		goto abort;
	}
	ret = backfill_commit(&ctx);
	if (ret < 0)
		goto out;

	ret = qgroup_verify(fs_info, true);
	if (ret < 0) {
		error("failed to measure simple quota usage");
		goto out_counts;
	}
	report_qgroups(0);
	ret = repair_qgroups(fs_info, &repaired, false);
	if (ret) {
		error("failed to write simple quota usage");
		goto out_counts;
	}
out_counts:
	free_qgroup_counts();
out:
	ulist_release(&fs_roots);
	ulist_release(&ctx.visited);
	ulist_release(&ctx.owners);
	return ret;
abort:
	btrfs_abort_transaction(ctx.trans, ret);
	goto out;
}
