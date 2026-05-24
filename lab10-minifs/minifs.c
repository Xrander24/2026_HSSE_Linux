// SPDX-License-Identifier: GPL-2.0
/*
 * minifs.c — учебная файловая система в памяти.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/pagemap.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/time.h>

#define MINIFS_MAGIC 0x20260110
#define MINIFS_MAX_FILE_SIZE 4096

struct minifs_file_data {
	char *data;
	size_t size;
};

static struct inode *minifs_get_inode(struct super_block *sb,
				      const struct inode *dir,
				      umode_t mode);

static int minifs_create(struct mnt_idmap *idmap,
			 struct inode *dir,
			 struct dentry *dentry,
			 umode_t mode,
			 bool excl);

static struct dentry *minifs_mkdir(struct mnt_idmap *idmap,
				   struct inode *dir,
				   struct dentry *dentry,
				   umode_t mode);

static ssize_t minifs_read(struct file *file,
			   char __user *buf,
			   size_t len,
			   loff_t *ppos);

static ssize_t minifs_write(struct file *file,
			    const char __user *buf,
			    size_t len,
			    loff_t *ppos);

static const struct file_operations minifs_file_ops = {
	.read = minifs_read,
	.write = minifs_write,
	.llseek = generic_file_llseek,
};

static const struct inode_operations minifs_file_inode_ops = {
	.getattr = simple_getattr,
};

static const struct inode_operations minifs_dir_inode_ops = {
	.lookup = simple_lookup,
	.create = minifs_create,
	.mkdir = minifs_mkdir,
};

static const struct file_operations minifs_dir_ops = {
	.owner = THIS_MODULE,
	.iterate_shared = dcache_readdir,
	.llseek = generic_file_llseek,
};

static ssize_t minifs_read(struct file *file,
			   char __user *buf,
			   size_t len,
			   loff_t *ppos)
{
	struct inode *inode = file_inode(file);
	struct minifs_file_data *fdata = inode->i_private;

	if (!fdata || !fdata->data)
		return 0;

	return simple_read_from_buffer(buf, len, ppos, fdata->data, fdata->size);
}

static ssize_t minifs_write(struct file *file,
			    const char __user *buf,
			    size_t len,
			    loff_t *ppos)
{
	struct inode *inode = file_inode(file);
	struct minifs_file_data *fdata = inode->i_private;
	size_t to_copy;
	ssize_t ret;

	if (!fdata) {
		fdata = kzalloc(sizeof(*fdata), GFP_KERNEL);
		if (!fdata)
			return -ENOMEM;

		fdata->data = kzalloc(MINIFS_MAX_FILE_SIZE, GFP_KERNEL);
		if (!fdata->data) {
			kfree(fdata);
			return -ENOMEM;
		}

		inode->i_private = fdata;
	}

	if (*ppos >= MINIFS_MAX_FILE_SIZE)
		return -ENOSPC;

	to_copy = min_t(size_t, len, MINIFS_MAX_FILE_SIZE - *ppos);

	ret = simple_write_to_buffer(fdata->data,
				     MINIFS_MAX_FILE_SIZE,
				     ppos,
				     buf,
				     to_copy);

	if (ret > 0) {
		if (*ppos > fdata->size)
			fdata->size = *ppos;

		i_size_write(inode, fdata->size);
		inode_set_mtime_to_ts(inode, current_time(inode));
		inode_set_ctime_current(inode);
	}

	return ret;
}

static struct inode *minifs_get_inode(struct super_block *sb,
				      const struct inode *dir,
				      umode_t mode)
{
	struct inode *inode = new_inode(sb);

	if (!inode)
		return NULL;

	inode->i_ino = get_next_ino();
	inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
	simple_inode_init_ts(inode);

	switch (mode & S_IFMT) {
	case S_IFDIR:
	    inode->i_op = &minifs_dir_inode_ops;
	    inode->i_fop = &simple_dir_operations;
	    inc_nlink(inode);
	    break;
	case S_IFREG:
		inode->i_op = &minifs_file_inode_ops;
		inode->i_fop = &minifs_file_ops;
		break;
	default:
		init_special_inode(inode, mode, 0);
		break;
	}

	return inode;
}

static int minifs_create(struct mnt_idmap *idmap,
			 struct inode *dir,
			 struct dentry *dentry,
			 umode_t mode,
			 bool excl)
{
	struct inode *inode = minifs_get_inode(dir->i_sb, dir, S_IFREG | mode);

	if (!inode)
		return -ENOMEM;

	d_instantiate(dentry, inode);
	dget(dentry);

	inode_set_mtime_to_ts(dir, current_time(dir));
	inode_set_ctime_current(dir);

	pr_info("minifs: create file %pd\n", dentry);
	return 0;
}

static struct dentry *minifs_mkdir(struct mnt_idmap *idmap,
				   struct inode *dir,
				   struct dentry *dentry,
				   umode_t mode)
{
	struct inode *inode = minifs_get_inode(dir->i_sb, dir, S_IFDIR | mode);

	if (!inode)
		return ERR_PTR(-ENOMEM);

	inc_nlink(dir);
	d_instantiate(dentry, inode);
	dget(dentry);

	inode_set_mtime_to_ts(dir, current_time(dir));
	inode_set_ctime_current(dir);

	pr_info("minifs: mkdir %pd\n", dentry);
	return NULL;
}

static void minifs_evict_inode(struct inode *inode)
{
	struct minifs_file_data *fdata = inode->i_private;

	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);

	if (fdata) {
		kfree(fdata->data);
		kfree(fdata);
		inode->i_private = NULL;
	}
}

static const struct super_operations minifs_super_ops = {
	.statfs = simple_statfs,
	.evict_inode = minifs_evict_inode,
};

static int minifs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct inode *root_inode;

	sb->s_magic = MINIFS_MAGIC;
	sb->s_op = &minifs_super_ops;
	sb->s_time_gran = 1;

	root_inode = minifs_get_inode(sb, NULL, S_IFDIR | 0755);
	if (!root_inode)
		return -ENOMEM;

	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root)
		return -ENOMEM;

	pr_info("minifs: superblock created\n");
	return 0;
}

static int minifs_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, minifs_fill_super);
}

static const struct fs_context_operations minifs_context_ops = {
	.get_tree = minifs_get_tree,
};

static int minifs_init_fs_context(struct fs_context *fc)
{
	fc->ops = &minifs_context_ops;
	return 0;
}

static struct file_system_type minifs_type = {
	.owner = THIS_MODULE,
	.name = "minifs",
	.init_fs_context = minifs_init_fs_context,
	.kill_sb = kill_anon_super,
};

static int __init minifs_init(void)
{
	int ret = register_filesystem(&minifs_type);

	if (ret)
		return ret;

	pr_info("minifs: registered\n");
	return 0;
}

static void __exit minifs_exit(void)
{
	int ret = unregister_filesystem(&minifs_type);

	if (ret)
		pr_err("minifs: unregister failed: %d\n", ret);
	else
		pr_info("minifs: unregistered\n");
}

module_init(minifs_init);
module_exit(minifs_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Linux Kernel Course");
MODULE_DESCRIPTION("Educational in-memory VFS filesystem");
