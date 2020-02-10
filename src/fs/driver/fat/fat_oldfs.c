/**
 * @file
 * @brief Implementation of FAT driver
 *
 * @date 28.03.2012
 * @author Andrey Gazukin
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <limits.h>
#include <libgen.h>

#include <util/err.h>

#include <fs/file_desc.h>
#include <fs/file_operation.h>
#include <fs/super_block.h>
#include <fs/fs_driver.h>
#include <fs/inode.h>
#include <fs/vfs.h>
#include <drivers/block_dev.h>

#include "fat.h"

static int fat_create_dir_entry(struct nas *parent_nas,
		struct dirinfo *parent_di,
		struct fat_dirent *self_de) {
	struct fat_dirent de;
	char name[PATH_MAX];
	struct nas *nas;
	struct fat_file_info *fi;
	struct inode *node;
	mode_t mode;
	int ret;
	struct dirinfo *new_di;
	struct fat_fs_info *fsi = parent_nas->fs->sb_data;

	fat_reset_dir(parent_di);
	read_dir_buf(parent_di);

	while (DFS_EOF != fat_get_next_long(parent_di, &de, name)) {
		if (de.name[0] == 0) {
			continue;
		}

		if ((0 == strcmp(name, ".")) || (0 == strcmp(name, ".."))) {
			continue;
		}

		if (de.attr & ATTR_VOLUME_ID) {
			continue;
		}

		if (de.attr & ATTR_DIRECTORY) {
			mode = S_IFDIR;

			if (NULL == (new_di = fat_dirinfo_alloc())) {
				return -ENOMEM;
			}

			fi = &new_di->fi;
			new_di->p_scratch = fat_sector_buff;
		} else {
			mode = S_IFREG;

			if (NULL == (fi = fat_file_alloc())) {
				return -ENOMEM;
			}
		}

		if (NULL == (node = vfs_subtree_create_child(parent_nas->node, name, mode))) {
			if (de.attr & ATTR_DIRECTORY) {
				fat_dirinfo_free(new_di);
			} else {
				fat_file_free(fi);
			}
			return -ENOMEM;
		}

		memset(fi, 0, sizeof(*fi));

		fi->fsi          = parent_nas->fs->sb_data;
		fi->filelen      = fat_direntry_get_size(&de);
		fi->dirsector    = fat_current_dirsector(parent_di);
		fi->diroffset    = parent_di->currententry - 1;
		fi->cluster      = fat_direntry_get_clus(&de);
		fi->firstcluster = fi->cluster;
		fi->filelen      = fat_direntry_get_size(&de);
		fi->volinfo      = &fsi->vi;
		fi->fdi          = parent_di;
		fi->mode         = mode;

		nas = node->nas;
		nas->fi->privdata = fi;
		nas->fi->ni.size = fi->filelen;

		if (de.attr & ATTR_DIRECTORY) {
			if ((ret = fat_create_dir_entry(nas, (void *) fi, &de))) {
					return ret;
			}
			/* Children directories use the same read buffer,
			 * so we need to reread relevant content */
			read_dir_buf(parent_di);
		}
	}

	return ENOERR;
}

static void fat_free_fs(struct nas *nas) {
	if (NULL != nas->fs) {
		struct fat_fs_info *fsi;
		fsi = nas->fs->sb_data;

		if(NULL != fsi) {
			fat_fs_free(fsi);
		}
		super_block_free(nas->fs);
	}

	if (NULL != inode_priv(nas->node)) {
		fat_dirinfo_free(inode_priv(nas->node));
	}
}

static int fat_umount_entry(struct inode *node) {
	struct inode *child;

	if (node_is_directory(node)) {
		while (NULL != (child = vfs_subtree_get_child_next(node, NULL))) {
			if (node_is_directory(child)) {
				fat_umount_entry(child);
				fat_dirinfo_free(inode_priv(child));
			} else {
				fat_file_free(inode_priv(child));
			}
			vfs_del_leaf(child);
		}
	}

	return 0;
}

extern struct file_operations fat_fops;

struct inode_operations fat_iops;
struct super_block_operations fat_sbops;
extern int fat_fill_sb(struct super_block *sb, const char *source);

static int fatfs_mount(struct super_block *sb, struct inode *dest) {
	struct nas *dir_nas;
	struct dirinfo *di;
	struct fat_fs_info *fsi;
	uint32_t pstart, psize;
	uint8_t pactive, ptype;
	struct fat_dirent de;
	int rc;

	dir_nas = dest->nas;
	fsi = sb->sb_data;

	/* allocate this directory info */
	if (NULL == (di = fat_dirinfo_alloc())) {
		rc = -ENOMEM;
		goto error;
	}
	memset(di, 0, sizeof(struct dirinfo));
	dir_nas->fi->privdata = (void *) di;
	di->fi.fsi = fsi;
	di->p_scratch = fat_sector_buff;
	fsi->root = dest;

	pstart = fat_get_ptn_start(sb->bdev, 0, &pactive, &ptype, &psize);
	if (pstart == 0xffffffff) {
		rc = -1;
		goto error;
	}

	if (fat_open_rootdir(fsi, di)) {
		return -EBUSY;
	}

	return fat_create_dir_entry(dir_nas, di, &de);

error:
	fat_free_fs(dir_nas);

	return rc;
}

static int fatfs_create(struct inode *parent_node, struct inode *node) {
	struct nas *nas;
	struct fat_file_info *fi;
	struct fat_fs_info *fsi;
	struct dirinfo *di;

	assert(parent_node && node);

	nas = node->nas;
	nas->fi->ni.size = 0;

	di = (void *) inode_priv(parent_node);

	if (S_ISDIR(node->i_mode)) {
		struct dirinfo *new_di;
		new_di = fat_dirinfo_alloc();
		if (!new_di) {
			return -ENOMEM;
		}
		new_di->p_scratch = fat_sector_buff;
		fi = &new_di->fi;
	} else {
		fi = fat_file_alloc();
		if (!fi) {
			return -ENOMEM;
		}
	}

	fsi = di->fi.fsi;
	nas->fi->privdata = fi;
	nas->fs = parent_node->nas->fs;
	*fi = (struct fat_file_info) {
		.fsi     = fsi,
		.volinfo = &fsi->vi,
		.fdi     = di,
		.mode    = node->i_mode,
	};

	if (0 != fat_create_file(fi, di, node->name, node->i_mode)) {
		return -EIO;
	}

	return 0;
}

static int fatfs_delete(struct inode *node) {
	struct fat_file_info *fi;

	fi = inode_priv(node);

	if (fat_unlike_file(fi, (uint8_t *) fat_sector_buff)) {
		return -1;
	}

	if (S_ISDIR(node->i_mode)) {
		fat_dirinfo_free((void *) fi);
	} else {
		fat_file_free(fi);
	}

	vfs_del_leaf(node);
	return 0;
}

static int fatfs_truncate(struct inode *node, off_t length) {
	struct nas *nas = node->nas;

	nas->fi->ni.size = length;

	/* TODO realloc blocks*/

	return 0;
}

static int fatfs_umount(struct inode *dir) {
	struct nas *dir_nas;

	dir_nas = dir->nas;

	fat_umount_entry(dir);

	fat_free_fs(dir_nas);

	return 0;
}

extern int fat_format(struct block_dev *dev, void *priv);
static struct fsop_desc fatfs_fsop = {
	.format = fat_format,
	.mount = fatfs_mount,
	.create_node = fatfs_create,
	.delete_node = fatfs_delete,
	.truncate = fatfs_truncate,
	.umount = fatfs_umount,
};

static const struct fs_driver fatfs_driver = {
	.name = "vfat",
	.fill_sb = fat_fill_sb,
	.file_op = &fat_fops,
	.fsop = &fatfs_fsop,
};

DECLARE_FILE_SYSTEM_DRIVER(fatfs_driver);
