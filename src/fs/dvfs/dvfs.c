/**
 * @file
 * @brief  DVFS interface implementation
 * @author Denis Deryugin
 * @date   11 Mar 2014
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

#include <util/err.h>

#include <drivers/block_dev.h>
#include <fs/dvfs.h>
#include <fs/hlpr_path.h>
#include <kernel/task/resource/vfs.h>
#include <util/log.h>
#include <util/math.h>

/* Utility functions */
extern int inode_fill(struct super_block *, struct inode *, struct dentry *);
extern int            dvfs_update_root(void);
extern struct dentry *dvfs_root(void);
extern int dvfs_path_walk(const char *path, struct dentry *parent, struct lookup *lookup);
extern int dvfs_lookup(const char *path, struct lookup *lookup);

/**
 * @brief Create new inode
 * @param name   Directory name for new inode
 * @param lookup Structure containing parent dentry; lookup->item should be NULL
 * @param flags  Flags passed to FS driver
 *
 * @return Negative error number
 * @retval       0 Ok
 * @retval -ENOMEM New dentry can't be allocated
 */
int dvfs_create_new(const char *name, struct lookup *lookup, int flags) {
	struct super_block *sb;
	struct inode *new_inode;
	int res;

	assert(lookup);
	assert(lookup->parent);
	assert(lookup->parent->flags & S_IFDIR);

	sb = lookup->parent->d_sb;
	lookup->item = dvfs_alloc_dentry();
	if (!lookup->item) {
		return -ENOMEM;
	}

	new_inode = dvfs_alloc_inode(sb);
	if (!new_inode) {
		dvfs_destroy_dentry(lookup->item);
		return -ENOMEM;
	}
	dentry_fill(sb, new_inode, lookup->item, lookup->parent);
	strncpy(lookup->item->name, name, DENTRY_NAME_LEN - 1);
	inode_fill(sb, new_inode, lookup->item);

	lookup->item->flags |= flags;
	new_inode->i_mode |= flags;
	if (flags & VFS_DIR_VIRTUAL) {
		res = 0;
		lookup->item->d_sb = NULL;
		dentry_ref_inc(lookup->item);
		lookup->parent->flags |= DVFS_CHILD_VIRTUAL;
	} else {
		if (!sb->sb_iops->create) {
			res = -ENOSUPP;
		} else {
			res = sb->sb_iops->create(new_inode,
					lookup->parent->d_inode,
					flags);
			if (res == -ENOMEM) {
				/* This may be due to lack of some internal FS
				 * resources,  so  we  can  try  to free  some
				 * dentry and try again */
				if (dvfs_fs_dentry_try_free(sb)) {
					res = sb->sb_iops->create(new_inode,
							lookup->parent->d_inode,
							flags);
				}
			}
		}
	}

	if (res) {
		dvfs_destroy_dentry(lookup->item);
	}

	return res;
}

extern const struct idesc_ops idesc_file_ops;
/**
 * @brief Initialize file descriptor for usage according to path
 * @param path Path to the file
 * @param desc The file descriptor to be initailized
 * @param mode Defines behavior according to POSIX
 *
 * @returns Negative error number
 * @retval       0 Ok
 * @retval -ENOENT File is directory or file not found and
 *                 creating is not requested
 */
struct idesc *dvfs_file_open_idesc(struct lookup *lookup, int __oflag) {
	struct file_desc *desc;
	struct idesc *res;
	struct inode  *i_no;

	assert(lookup);

	desc = dvfs_alloc_file();
	if (desc == NULL)
		return err_ptr(ENOMEM);

	i_no = lookup->item->d_inode;

	*desc = (struct file_desc) {
		.f_dentry = lookup->item,
		.f_inode  = i_no,
		.f_ops    = lookup->item->d_sb->sb_fops,
		.f_idesc  = {
			.idesc_ops   = &idesc_file_ops,
		},
	};

	assert(desc->f_ops);
	if (desc->f_ops->open) {
		res = desc->f_ops->open(i_no, &desc->f_idesc);
		if (res == NULL) {
			return NULL;
		}
	}

	if ((__oflag & O_APPEND) && (desc->f_inode)) {
		/* XXX for some reason position of the last symbol
		 * is zero both for empty and 1-byte files,  this
		 * probably should be fixed  */
		desc->pos = desc->f_inode->length == 0 ?
			0 : desc->f_inode->length - 1;
	}

	return &desc->f_idesc;
}

/**
 * @brief Delete file from storage
 * @param path Path to file
 *
 * @return Negative error code
 * @retval  0 Ok
 * @retval -1 File not found
 */
int dvfs_remove(const char *path) {
	struct lookup lookup = {};
	struct inode  *i_no;
	int res;

	dvfs_lookup(path, &lookup);

	if (!lookup.item) {
		return -ENOENT;
	}

	i_no = lookup.item->d_inode;

	assert(i_no->i_ops);

	if (!i_no->i_ops->remove)
		return -EPERM;

	res = i_no->i_ops->remove(i_no);

	if (res == 0) {
		dentry_ref_dec(lookup.item);
		res = dvfs_destroy_dentry(lookup.item);
		if (res != 0) {
			log_error("Failed to destroy dentry");
		}
	} else {
		log_error("Failed to remove inode");
	}

	return res;
}

/**
 * @brief Uninitialize file descriptor
 * @param desc File descriptor to be uninitialized
 *
 * @return Negative error code
 * @retval  0 Ok
 * @retval -1 Descriptor fields are inconsistent
 */
int dvfs_close(struct file_desc *desc) {
	if (!desc || !desc->f_inode || !desc->f_dentry)
		return -1;

	assert(desc->f_ops);
	if (desc->f_ops->close) {
		desc->f_ops->close(desc);
	}

	if (!dentry_ref_dec(desc->f_dentry))
		dvfs_destroy_dentry(desc->f_dentry);

	dvfs_destroy_file(desc);
	return 0;
}

/**
 * @brief Application level interface to write the file
 * @param desc  File to be written
 * @param buf   Source of the data
 * @param count Length of the data
 *
 * @return Bytes written or negative error code
 * @retval       0 Ok
 * @retval -ENOSYS Function is not implemented in file system driver
 */
int dvfs_write(struct file_desc *desc, char *buf, int count) {
	int res = 0; /* Assign to avoid compiler warning when use -O2 */
	int retcode = count;
	struct inode *inode;
	if (!desc)
		return -1;

	inode = desc->f_inode;
	assert(inode);

	if (inode->length - desc->pos < count && !(inode->i_mode & DVFS_NO_LSEEK)) {
		if (inode->i_ops && inode->i_ops->truncate) {
			res = inode->i_ops->truncate(desc->f_inode, desc->pos + count);
			if (res)
				retcode = -EFBIG;
		} else
			retcode = -EFBIG;
	}

	if (desc->f_ops && desc->f_ops->write)
		res = desc->f_ops->write(desc, buf, count);
	else
		retcode = -ENOSYS;

	if (res > 0)
		desc->pos += res;

	return retcode;
}

/**
 * @brief Application level interface to read the file
 * @param desc  File to be read
 * @param buf   Destination
 * @param count Length of the data
 *
 * @return Bytes read or negative error code
 * @retval       0 Ok
 * @retval -ENOSYS Function is not implemented in file system driver
 */
int dvfs_read(struct file_desc *desc, char *buf, int count) {
	int res;
	int sz;
	if (!desc)
		return -1;

	sz = min(count, desc->f_inode->length - desc->pos);

	if (sz <= 0)
		return 0;

	if (desc->f_ops && desc->f_ops->read)
		res = desc->f_ops->read(desc, buf, count);
	else
		return -ENOSYS;

	if (res > 0)
		desc->pos += res;

	return res;
}

int dvfs_fstat(struct file_desc *desc, struct stat *sb) {
	*sb = (struct stat) {
		.st_size = desc->f_inode->length,
		.st_mode = desc->f_inode->i_mode,
		.st_uid = 0,
		.st_gid = 0
	};
	return 0;
}

static struct file_desc *dvfs_get_mount_bdev(const char *dev_name) {
	struct file_desc *bdev_file;
	struct lookup lookup = {};
	int res;

	if (!dev_name) {
		return NULL;
	}
	if (!strlen(dev_name)) {
		return NULL;
	}

	bdev_file = NULL;

	/* Check if devfs is initialized */
	res = dvfs_lookup("/dev", &lookup);
	if (res) {
		/* XXX devfs not present, we should use a really ugly
		 * hack for pseudo-open() blockdev file. This code
		 * won't work if devfs driver will be changed. If
		 * someone knows how to rewrite it, please, do it :) */
		const struct dumb_fs_driver *devfs_drv;
		/* We fill local variable with file operations to
		 * initialize bdev_file properly */
		struct super_block devfs_sb;
		struct block_dev *block_dev;

		devfs_drv = dumb_fs_driver_find("devfs");
		assert(devfs_drv);

		block_dev = block_dev_find(dev_name);
		bdev_file = dvfs_alloc_file();
		if (!bdev_file) {
			SET_ERRNO(ENOENT);
			return NULL;
		}
		devfs_drv->fill_sb(&devfs_sb, bdev_file);
		memset(bdev_file, 0, sizeof(struct file_desc));
		bdev_file->f_inode = dvfs_alloc_inode(&devfs_sb);
		bdev_file->f_ops   = devfs_sb.sb_fops;
		bdev_file->f_inode->i_data = block_dev;
		/* Now we have file object that could be used for fs
		 * initialization */
		return bdev_file;
	}

	/* devfs presents, perform usual mount */
	dvfs_lookup(dev_name, &lookup);
	if (!lookup.item) {
		SET_ERRNO(ENOENT);
		return NULL;
	}
	/* TODO pass flags */
	bdev_file = (struct file_desc*) dvfs_file_open_idesc(&lookup, 0);
	if (err(bdev_file)) {
		return bdev_file;
	}
	return bdev_file;
}

extern int dvfs_cache_del(struct dentry *dentry);
extern int set_rootfs_sb(struct super_block *sb);
/**
 * @brief Mount file system
 * @param dev    Path to the source device (e.g. /dev/sda1)
 * @param dest   Path to the mount point (e.g. /mnt)
 * @param fstype File system type related to FS driver
 * @param flags  NIY
 *
 * @return Negative error value
 * @retval       0 Ok
 * @retval -ENOENT Mount point or device not found
 */
int dvfs_mount(const char *dev, const char *dest, const char *fstype, int flags) {
	struct lookup lookup = {};
	const struct dumb_fs_driver *drv;
	struct super_block *sb;
	struct dentry *d = NULL;
	struct file_desc *bdev_file;
	int err;

	assert(dest);
	assert(fstype);

	drv = dumb_fs_driver_find(fstype);
	assert(drv);

	bdev_file = dvfs_get_mount_bdev(dev);

	if (NULL == (sb = dvfs_alloc_sb(drv, bdev_file))) {
		return -ENOMEM;
	}

	if (!strcmp(dest, "/")) {
		set_rootfs_sb(sb);
		dvfs_update_root();
	} else {
		dvfs_lookup(dest, &lookup);

		if (lookup.item == NULL) {
			err = -ENOENT;
			goto err_free_all;
		}

		if (lookup.item->flags & DVFS_MOUNT_POINT) {
			err = -EBUSY;
			goto err_free_all;
		}

		if (!(lookup.item->flags & S_IFDIR)) {
			err = -EINVAL;
			goto err_free_all;
		}

		dentry_disconnect(lookup.item);

		d = dvfs_alloc_dentry();

		d->flags |= VFS_DIR_VIRTUAL;
		dentry_fill(sb, NULL, d, lookup.parent);
		strcpy(d->name, lookup.item->name);

		d->flags |= S_IFDIR | DVFS_MOUNT_POINT;
		d->d_sb  = sb,
		sb->root = d;

		d->d_inode = dvfs_alloc_inode(sb);
		*d->d_inode = (struct inode) {
			.i_mode   = S_IFDIR,
			.i_ops    = sb->sb_iops,
			.i_sb     = sb,
			.i_dentry = d,
		};
	}

	if (drv->mount_end) {
		if ((err = drv->mount_end(sb)))
			goto err_free_all;
	}

	goto err_ok;
err_free_all:
	if (d != NULL) {
		dvfs_destroy_inode(d->d_inode);
		dentry_reconnect(d->parent, d->name);
		dvfs_destroy_dentry(d);
	}

	if (bdev_file) {
		dvfs_close(bdev_file);
	}

	if (sb) {
		dvfs_destroy_sb(sb);
	}

	return err;
err_ok:
	return 0;
}


/**
 * @brief Recoursive dentry freeing
 *
 * @param d Dentry to be freed
 *
 * @return Negative error code or zero if succed
 */
static int _dentry_destroy(struct dentry *parent) {
	struct dentry *child;
	int err;
	dlist_foreach_entry(child, &parent->children, children_lnk) {
		if ((err = _dentry_destroy(child)))
			return err;
	}

	return dvfs_destroy_dentry(parent);
}

/**
 * @brief Perform unmount operation
 *
 * @param mpoint Dentry of FS root
 *
 * @return Negative error code or zero if succeed
 * @retval 0 Success
 * @retval -ENOBUSY Some files in FS tree are being used, can't unmount
 */
int dvfs_umount(struct dentry *mpoint) {
	int err;
	struct super_block *sb;

	sb = mpoint->d_sb;

	if (sb->sb_ops && sb->sb_ops->umount_begin) {
		if ((err = sb->sb_ops->umount_begin(sb)))
			return err;
	}

	if (sb->bdev_file) {
		dvfs_close(sb->bdev_file);
	}

	/* TODO what if mount point was renamed? */
	dentry_reconnect(mpoint->parent, mpoint->name);
	dentry_ref_dec(mpoint);

	if ((err = dvfs_destroy_sb(sb))) {
		return err;
	}

	if ((err = _dentry_destroy(mpoint))) {
		return err;
	}

	return 0;
}

static struct dentry *iterate_virtual(struct lookup *lookup, struct dir_ctx *ctx) {
	struct dentry *next_dentry;
	struct dlist_head *l;
	int i;

	i = 0;
	dlist_foreach(l, &lookup->parent->children)
	{
		next_dentry = mcast_out(l, struct dentry, children_lnk);

		if (next_dentry->flags & VFS_DIR_VIRTUAL) {
			if (i++ == (ctx->flags & ~DVFS_CHILD_VIRTUAL)) {
				ctx->flags++;
				dentry_ref_inc(next_dentry);
				lookup->item = next_dentry;

				return next_dentry;
			}
		}
	}

	return NULL;
}

/**
 * @brief Get next entry in the directory
 * @param lookup  Contains directory dentry (.parent) and
 *                previous element (.item)
 * @param dir_ctx Position to be found in directory
 *
 * @return Negative error value
 * @retval 0 Ok
 */
int dvfs_iterate(struct lookup *lookup, struct dir_ctx *ctx) {
	struct super_block *sb;
	struct inode *next_inode;
	struct dentry *next_dentry, *cached;
	int res;

	assert(ctx);
	assert(lookup);
	assert(lookup->parent);

	if (lookup->parent->flags & VFS_DIR_VIRTUAL &&
			!lookup->parent->d_sb) {
		/* Clean virtual dir, no files */
		lookup->item = NULL;
		return 0;
	}

	assert(lookup->parent->d_sb);
	assert(lookup->parent->d_inode);

	sb = lookup->parent->d_sb;
	assert(sb->sb_iops && sb->sb_iops->iterate);

	if (ctx->flags & DVFS_CHILD_VIRTUAL) {
		/* we are already in virtual iterate mode */
		lookup->item = iterate_virtual(lookup, ctx);

		return 0;
	}

	next_inode = dvfs_alloc_inode(sb);
	if (!next_inode) {
		return -ENOMEM;
	}
	next_dentry = dvfs_alloc_dentry();
	if (!next_dentry) {
		dvfs_destroy_inode(next_inode);
		return -ENOMEM;
	}

	res = sb->sb_iops->iterate(next_inode, next_dentry->name, lookup->parent->d_inode, ctx);
	if (res) {
		/* iterate virtual */
		dentry_ref_dec(next_dentry);
		dvfs_destroy_dentry(next_dentry);

		lookup->item = NULL;
		if (lookup->parent->flags & DVFS_CHILD_VIRTUAL) {
			ctx->flags = DVFS_CHILD_VIRTUAL;

			lookup->item = iterate_virtual(lookup, ctx);
		}
		/* Virtual entries are always cached, so we skip cache check */
		return 0;
	}

	if ((cached = dvfs_cache_get(next_dentry->name, lookup))) {
		/* This node is already in the VFS tree */
		dvfs_destroy_dentry(next_dentry);
		lookup->item = cached;
	} else {
		/* Integrate next_dentry into VFS tree */
		dentry_fill(sb, next_inode, next_dentry, lookup->parent);
		inode_fill(sb, next_inode, next_dentry);
		dentry_upd_flags(next_dentry);
		dvfs_cache_add(next_dentry);
		lookup->item = next_dentry;
	}

	//dentry_ref_inc(lookup->item);

	return 0;
}
