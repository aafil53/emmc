// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2011 - 2012 Samsung Electronics
 * EXT4 filesystem implementation in Uboot by
 * Uma Shankar <uma.shankar@samsung.com>
 * Manjunatha C Achar <a.manjunatha@samsung.com>
 *
 * Ext4fs support
 * made from existing cmd_ext2.c file of Uboot
 *
 * (C) Copyright 2004
 * esd gmbh <www.esd-electronics.com>
 * Reinhard Arlt <reinhard.arlt@esd-electronics.com>
 *
 * made from cmd_reiserfs by
 *
 * (C) Copyright 2003 - 2004
 * Sysgo Real-Time Solutions, AG <www.elinos.com>
 * Pavel Bartusek <pba@sysgo.com>
 */

/*
 * Changelog:
 *	0.1 - Newly created file for ext4fs support. Taken from cmd_ext2.c
 *	        file in uboot. Added ext4fs ls load and write support.
 */

#include <part.h>
#include <errno.h>
#include <config.h>
#include <command.h>
#include <image.h>
#include <linux/ctype.h>
#include <asm/byteorder.h>
#include <ext4fs.h>
#include <linux/stat.h>
#include <malloc.h>
#include <fs.h>

#if defined(CONFIG_CMD_USB) && defined(CONFIG_USB_STORAGE)
#include <usb.h>
#endif

int do_ext4_size(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	return do_size(cmdtp, flag, argc, argv, FS_TYPE_EXT);
}

int do_ext4_load(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	return do_load(cmdtp, flag, argc, argv, FS_TYPE_EXT);
}

int do_ext4_ls(struct cmd_tbl *cmdtp, int flag, int argc, char *const argv[])
{
	return do_ls(cmdtp, flag, argc, argv, FS_TYPE_EXT);
}

#if defined(CONFIG_CMD_EXT4_WRITE)
static int ext4rm_normalize_path(const char *input, char *output,
				 size_t output_size)
{
	const char *p = input;
	size_t used = 1;
	bool first = true;

	if (!input || !*input || output_size < 2)
		return -EINVAL;
	output[0] = '/';
	output[1] = '\0';
	while (*p) {
		const char *start;
		size_t len;

		while (*p == '/')
			p++;
		if (!*p)
			break;
		start = p;
		while (*p && *p != '/')
			p++;
		len = p - start;
		if (len == 1 && start[0] == '.')
			continue;
		if (len == 2 && start[0] == '.' && start[1] == '.')
			return -EINVAL;
		if (first && ((len == 4 && !strncmp(start, "boot", len)) ||
			      (len == 6 && !strncmp(start, "ostree", len))))
			return -EPERM;
		if (used > 1) {
			if (used + 1 >= output_size)
				return -ENAMETOOLONG;
			output[used++] = '/';
		}
		if (len >= output_size - used)
			return -ENAMETOOLONG;
		memcpy(output + used, start, len);
		used += len;
		output[used] = '\0';
		first = false;
	}
	return first ? -EPERM : 0;
}

/* Ensure every path component is a real directory entry, never a symlink. */
static int ext4rm_validate_path(const char *iface, const char *dev,
				const char *path)
{
	char prefix[256] = "/";
	const char *p = path + 1;

	while (*p) {
		char component[FS_DIRENT_NAME_LEN];
		char next[256];
		const char *start = p;
		size_t len;
		struct fs_dir_stream *dirs;
		struct fs_dirent *dent;
		bool found = false;
		bool reached_eof = false;
		int read_errno = 0;

		while (*p && *p != '/')
			p++;
		len = p - start;
		if (!len || len >= sizeof(component))
			return -EINVAL;
		memcpy(component, start, len);
		component[len] = '\0';
		while (*p == '/')
			p++;
		if (fs_set_blk_dev(iface, dev, FS_TYPE_EXT))
			return -EIO;
		dirs = fs_opendir(prefix);
		if (!dirs)
			return -errno;
		errno = 0;
		while ((dent = fs_readdir(dirs))) {
			if (!strcmp(dent->name, component)) {
				found = dent->type == FS_DT_DIR;
				break;
			}
		}
		if (!dent) {
			read_errno = errno;
			reached_eof = true;
		}
		fs_closedir(dirs);
		if (reached_eof && read_errno != ENOENT)
			return -EIO;
		if (!found)
			return -ENOTDIR;
		if (snprintf(next, sizeof(next), "%s%s%s", prefix,
			     !strcmp(prefix, "/") ? "" : "/", component) >=
		    sizeof(next))
			return -ENAMETOOLONG;
		strlcpy(prefix, next, sizeof(prefix));
	}
	return 0;
}

static int ext4rm_recursive(const char *iface, const char *dev,
			    const char *path, unsigned int depth)
{
	unsigned int visited = 0;

	if (depth > 64)
		return -ELOOP;
	for (;;) {
		struct fs_dir_stream *dirs;
		struct fs_dirent *dent;
		char name[FS_DIRENT_NAME_LEN];
		char child[256];
		unsigned int type;
		bool has_child = false;
		int read_errno = 0;

		if (fs_set_blk_dev(iface, dev, FS_TYPE_EXT))
			return -EIO;
		dirs = fs_opendir(path);
		if (!dirs)
			return -errno;
		for (;;) {
			size_t len;

			errno = 0;
			dent = fs_readdir(dirs);
			if (!dent) {
				read_errno = errno;
				break;
			}
			len = strnlen(dent->name, sizeof(dent->name));
			if (!len || len == sizeof(dent->name)) {
				fs_closedir(dirs);
				return -EIO;
			}
			if (!strcmp(dent->name, ".") ||
			    !strcmp(dent->name, ".."))
				continue;
			strlcpy(name, dent->name, sizeof(name));
			type = dent->type;
			has_child = true;
			break;
		}
		fs_closedir(dirs);
		if (!has_child) {
			if (read_errno != ENOENT)
				return -EIO;
			if (fs_set_blk_dev(iface, dev, FS_TYPE_EXT))
				return -EIO;
			return fs_unlink(path);
		}
		if (++visited > 65536)
			return -E2BIG;
		if (snprintf(child, sizeof(child), "%s%s%s", path,
			     path[strlen(path) - 1] == '/' ? "" : "/", name) >=
		    sizeof(child))
			return -ENAMETOOLONG;
		if (type == FS_DT_DIR) {
			int ret = ext4rm_recursive(iface, dev, child, depth + 1);

			if (ret)
				return ret;
		} else if (type == FS_DT_REG) {
			if (fs_set_blk_dev(iface, dev, FS_TYPE_EXT) ||
			    fs_unlink(child))
				return -EIO;
		} else {
			printf("ext4rm: refusing unsupported entry %s (type %u)\n",
			       child, type);
			return -EOPNOTSUPP;
		}
	}
}

static int do_ext4rm(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	char path[256];
	int ret;

	if (argc == 4)
		return do_rm(cmdtp, flag, argc, argv, FS_TYPE_EXT);
	if (argc != 5 || strcmp(argv[1], "-rf"))
		return CMD_RET_USAGE;
	ret = ext4rm_normalize_path(argv[4], path, sizeof(path));
	if (ret) {
		printf("ext4rm: refusing unsafe or invalid path %s (%d)\n",
		       argv[4], ret);
		return CMD_RET_FAILURE;
	}
	ret = ext4rm_validate_path(argv[2], argv[3], path);
	if (ret) {
		printf("ext4rm: path validation failed for %s (%d)\n", path, ret);
		return CMD_RET_FAILURE;
	}
	ret = ext4rm_recursive(argv[2], argv[3], path, 0);
	if (ret) {
		printf("ext4rm: recursive deletion failed at %s (%d)\n", path, ret);
		return CMD_RET_FAILURE;
	}
	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(ext4rm, 5, 1, do_ext4rm,
	   "remove a file or recursively remove an ext4 directory",
	   "<interface> <dev[:part]> <filename>\n"
	   "ext4rm -rf <interface> <dev[:part]> <directory>");

int do_ext4_write(struct cmd_tbl *cmdtp, int flag, int argc,
		  char *const argv[])
{
	return do_save(cmdtp, flag, argc, argv, FS_TYPE_EXT);
}

U_BOOT_CMD(ext4write, 7, 1, do_ext4_write,
	   "create a file in the root directory",
	   "<interface> <dev[:part]> <addr> <absolute filename path>\n"
	   "    [sizebytes] [file offset]\n"
	   "    - create a file in / directory");

#endif

U_BOOT_CMD(
	ext4size,	4,	0,	do_ext4_size,
	"determine a file's size",
	"<interface> <dev[:part]> <filename>\n"
	"    - Find file 'filename' from 'dev' on 'interface'\n"
	"      and determine its size."
);

U_BOOT_CMD(ext4ls, 4, 1, do_ext4_ls,
	   "list files in a directory (default /)",
	   "<interface> <dev[:part]> [directory]\n"
	   "    - list files from 'dev' on 'interface' in a 'directory'");

U_BOOT_CMD(ext4load, 7, 0, do_ext4_load,
	   "load binary file from a Ext4 filesystem",
	   "<interface> [<dev[:part]> [addr [filename [bytes [pos]]]]]\n"
	   "    - load binary file 'filename' from 'dev' on 'interface'\n"
	   "      to address 'addr' from ext4 filesystem");
