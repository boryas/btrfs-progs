#include "kerncompat.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/print-tree.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/extent_io.h"
#include "common/messages.h"
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>

/*
 * TODO:
 * nodesize
 * BOX_MAIN ??
 */

#define NODESIZE 16384

int main(int argc, char **argv)
{
	size_t nodesize = 16384;
	struct extent_buffer *eb;
	char *buf;
	int ret;
	unsigned long long start;
	size_t bs = nodesize;
	struct btrfs_fs_info dummy_fsi = {
		.nodesize = nodesize,
		.leaf_data_size = __BTRFS_LEAF_DATA_SIZE(nodesize),
	};

	if (argc != 2) {
		error("usage: btrfs-dump-eb <eb-start>");
		return -EINVAL;
	}
	start = strtoull(argv[1], NULL, 10);
	eb = alloc_dummy_extent_buffer(&dummy_fsi, start, nodesize);
	if (!eb) {
		error("failed to allocate eb %llu\n", start);
		return -ENOMEM;
	}

	buf = calloc(nodesize, sizeof(char));
	if (!buf)
		return -ENOMEM;
	while (bs) {
		ret = read(STDIN_FILENO, buf + (nodesize - bs), bs);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			error("failed to read from stdin: %m");
			return -errno;
		}
		if (ret == 0)
			break;
		bs -= ret;
	}
	memcpy(eb->data, buf, nodesize);
	btrfs_print_tree(eb, 0);
}
