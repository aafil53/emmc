// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2011 - 2012 Samsung Electronics
 * EXT4 filesystem implementation in Uboot by
 * Uma Shankar <uma.shankar@samsung.com>
 * Manjunatha C Achar <a.manjunatha@samsung.com>
 *
 * ext4ls and ext4load : Based on ext2 ls and load support in Uboot.
 *		       Ext4 read optimization taken from Open-Moko
 *		       Qi bootloader
 *
 * (C) Copyright 2004
 * esd gmbh <www.esd-electronics.com>
 * Reinhard Arlt <reinhard.arlt@esd-electronics.com>
 *
 * based on code from grub2 fs/ext2.c and fs/fshelp.c by
 * GRUB  --  GRand Unified Bootloader
 * Copyright (C) 2003, 2004  Free Software Foundation, Inc.
 *
 * ext4write : Based on generic ext4 protocol.
 */

#include <blk.h>
#include <log.h>
#include <malloc.h>
#include <memalign.h>
#include <part.h>
#include <linux/stat.h>
#include <div64.h>
#include <time.h>
#include "ext4_common.h"

static inline void ext4fs_sb_free_inodes_inc(struct ext2_sblock *sb)
{
	sb->free_inodes = cpu_to_le32(le32_to_cpu(sb->free_inodes) + 1);
}

static inline void ext4fs_sb_free_blocks_inc(struct ext2_sblock *sb)
{
	sb->free_blocks = cpu_to_le32(le32_to_cpu(sb->free_blocks) + 1);
}

static inline void ext4fs_bg_free_inodes_inc
	(struct ext2_block_group *bg, const struct ext_filesystem *fs)
{
	uint32_t free_inodes = le16_to_cpu(bg->free_inodes);
	if (fs->gdsize == 64)
		free_inodes += le16_to_cpu(bg->free_inodes_high) << 16;
	free_inodes++;

	bg->free_inodes = cpu_to_le16(free_inodes & 0xffff);
	if (fs->gdsize == 64)
		bg->free_inodes_high = cpu_to_le16(free_inodes >> 16);
}

static int ext4fs_bg_used_dirs_dec(struct ext2_block_group *bg,
				   const struct ext_filesystem *fs)
{
	uint32_t used = le16_to_cpu(bg->used_dir_cnt);

	if (fs->gdsize == 64)
		used += (uint32_t)le16_to_cpu(bg->used_dir_cnt_high) << 16;
	if (!used)
		return -EIO;
	used--;
	bg->used_dir_cnt = cpu_to_le16(used & 0xffff);
	if (fs->gdsize == 64)
		bg->used_dir_cnt_high = cpu_to_le16(used >> 16);
	return 0;
}

static inline void ext4fs_bg_free_blocks_inc
	(struct ext2_block_group *bg, const struct ext_filesystem *fs)
{
	uint32_t free_blocks = le16_to_cpu(bg->free_blocks);
	if (fs->gdsize == 64)
		free_blocks += le16_to_cpu(bg->free_blocks_high) << 16;
	free_blocks++;

	bg->free_blocks = cpu_to_le16(free_blocks & 0xffff);
	if (fs->gdsize == 64)
		bg->free_blocks_high = cpu_to_le16(free_blocks >> 16);
}

static int ext4fs_update(void)
{
	int i;
	int ret;
	struct ext_filesystem *fs = get_fs();
	struct ext2_block_group *bgd;
	uint64_t bitmap_blk;
	unsigned long start;
	unsigned long home_start;

	if (!fs || !fs->sb)
		return -EINVAL;

	start = timer_get_us();
	ret = ext4fs_update_journal();
	ext4fs_perf_add(EXT4FS_PERF_JOURNAL_UPDATE,
			timer_get_us() - start);
	if (ret) {
		printf("ext4: journal update failed (%d)\n", ret);
		return ret;
	}
	home_start = timer_get_us();

	ret = put_ext4(SUPERBLOCK_SIZE, (struct ext2_sblock *)fs->sb,
		       SUPERBLOCK_SIZE);
	if (ret) {
		ext4fs_perf_add(EXT4FS_PERF_HOME_METADATA,
				timer_get_us() - home_start);
		return ret;
	}

	for (i = 0; i < fs->no_blkgrp; i++) {
		bgd = ext4fs_get_group_descriptor(fs, i);
		bgd->bg_checksum = cpu_to_le16(ext4fs_checksum_update(i));
		bitmap_blk = ext4fs_bg_get_block_id(bgd, fs);
		ret = put_ext4(bitmap_blk * fs->blksz, fs->blk_bmaps[i],
			       fs->blksz);
		if (ret) {
			ext4fs_perf_add(EXT4FS_PERF_HOME_METADATA,
					timer_get_us() - home_start);
			return ret;
		}
	}

	for (i = 0; i < fs->no_blkgrp; i++) {
		bgd = ext4fs_get_group_descriptor(fs, i);
		bitmap_blk = ext4fs_bg_get_inode_id(bgd, fs);
		ret = put_ext4(bitmap_blk * fs->blksz, fs->inode_bmaps[i],
			       fs->blksz);
		if (ret) {
			ext4fs_perf_add(EXT4FS_PERF_HOME_METADATA,
					timer_get_us() - home_start);
			return ret;
		}
	}

	ret = put_ext4((uint64_t)fs->gdtable_blkno * fs->blksz,
		       (struct ext2_block_group *)fs->gdtable,
		       fs->blksz * fs->no_blk_pergdt);
	if (ret) {
		ext4fs_perf_add(EXT4FS_PERF_HOME_METADATA,
				timer_get_us() - home_start);
		return ret;
	}

	ret = ext4fs_dump_metadata();
	if (ret) {
		ext4fs_perf_add(EXT4FS_PERF_HOME_METADATA,
				timer_get_us() - home_start);
		return ret;
	}
	ext4fs_perf_add(EXT4FS_PERF_HOME_METADATA,
			timer_get_us() - home_start);

	gindex = 0;
	gd_index = 0;
	return 0;
}

int ext4fs_get_bgdtable(void)
{
	int status;
	struct ext_filesystem *fs = get_fs();
	size_t alloc;
	size_t gdsize_total;

	if (__builtin_mul_overflow(fs->no_blkgrp, fs->gdsize, &alloc))
		return -1;

	gdsize_total = ROUND(alloc, fs->blksz);
	fs->no_blk_pergdt = gdsize_total / fs->blksz;

	/* allocate memory for gdtable */
	fs->gdtable = zalloc(gdsize_total);
	if (!fs->gdtable)
		return -ENOMEM;
	/* read the group descriptor table */
	status = ext4fs_devread((lbaint_t)fs->gdtable_blkno * fs->sect_perblk,
				0, fs->blksz * fs->no_blk_pergdt, fs->gdtable);
	if (status == 0)
		goto fail;

	if (ext4fs_log_gdt(fs->gdtable)) {
		printf("Error in ext4fs_log_gdt\n");
		free(fs->gdtable);
		fs->gdtable = NULL;
		return -1;
	}

	return 0;
fail:
	free(fs->gdtable);
	fs->gdtable = NULL;

	return -1;
}

/* Free a block only after its bitmap has been journaled successfully. */
static int ext4fs_free_allocated_block(uint32_t blknr)
{
	struct ext_filesystem *fs = get_fs();
	struct ext2_block_group *bgd;
	uint32_t total_blocks, blocks_per_group;
	uint64_t bitmap_blk;
	unsigned int bg_idx, bit;
	unsigned char mask;
	char *journal_buffer = NULL;
	int status;
#ifdef CONFIG_EXT4_PERF_TIMING
	unsigned long start = timer_get_us();
#endif

	if (!fs || !ext4fs_root || !fs->sb || !fs->blk_bmaps ||
	    !fs->gdtable || !fs->gdsize || !fs->dev_desc || !fs->blksz ||
	    !fs->sect_perblk)
		return -EINVAL;

	total_blocks = le32_to_cpu(ext4fs_root->sblock.total_blocks);
	blocks_per_group =
		le32_to_cpu(ext4fs_root->sblock.blocks_per_group);
	if (!blocks_per_group || !blknr || blknr >= total_blocks)
		return -EINVAL;

	bg_idx = blknr / blocks_per_group;
	if (fs->blksz == 1024 && !(blknr % blocks_per_group)) {
		if (!bg_idx)
			return -EINVAL;
		bg_idx--;
	}
	if (bg_idx >= fs->no_blkgrp || !fs->blk_bmaps[bg_idx])
		return -EINVAL;

	/* Match ext4fs_reset_block_bmap()'s 1 KiB block numbering. */
	bit = blknr / 8 - bg_idx * fs->blksz;
	if (fs->blksz == 1024 && !(blknr % 8)) {
		if (!bit)
			return -EINVAL;
		bit--;
		mask = 1 << 7;
	} else {
		mask = 1 << (fs->blksz == 1024 ? blknr % 8 - 1 : blknr % 8);
	}
	if (bit >= fs->blksz || !(fs->blk_bmaps[bg_idx][bit] & mask))
		return -EINVAL;

	bgd = ext4fs_get_group_descriptor(fs, bg_idx);
	bitmap_blk = ext4fs_bg_get_block_id(bgd, fs);
	if (!bitmap_blk || bitmap_blk >= total_blocks || bitmap_blk > UINT32_MAX)
		return -EINVAL;
	/* Keep the original on-disk bitmap once per transaction. */
	if (!ext4fs_journal_has_block(bitmap_blk)) {
		journal_buffer = zalloc(fs->blksz);
		if (!journal_buffer) {
			status = -ENOMEM;
			goto out;
		}
		status = ext4fs_devread(bitmap_blk * fs->sect_perblk, 0,
					fs->blksz, journal_buffer);
		if (!status) {
			status = -EIO;
			goto out;
		}
		ext4fs_perf_count(EXT4FS_PERF_BITMAP_READS);
		status = ext4fs_log_journal(journal_buffer, bitmap_blk);
		if (status)
			goto out;
	}

	/* The original bitmap is journaled before any in-memory counters change. */
	ext4fs_reset_block_bmap(blknr, fs->blk_bmaps[bg_idx], bg_idx);
	ext4fs_bg_free_blocks_inc(bgd, fs);
	ext4fs_sb_free_blocks_inc(fs->sb);
	debug("allocated block %u freed from group %u\n", blknr,
	      bg_idx);
	status = 0;
out:
	free(journal_buffer);
#ifdef CONFIG_EXT4_PERF_TIMING
	ext4fs_perf_add(EXT4FS_PERF_FREE, timer_get_us() - start);
#endif
	return status;
}

/* Recursively free indirect metadata; level 1 points directly to data. */
static int ext4fs_free_indirect_tree(uint32_t blknr, unsigned int level)
{
	struct ext_filesystem *fs = get_fs();
	__le32 *entries;
	uint32_t i, count;
	int status = 0;

	if (!blknr)
		return 0;
	if (!fs || !ext4fs_root || level < 1 || level > 3 ||
	    blknr >= le32_to_cpu(ext4fs_root->sblock.total_blocks))
		return -EINVAL;

	entries = zalloc(fs->blksz);
	if (!entries)
		return -ENOMEM;

	status = ext4fs_devread((lbaint_t)blknr * fs->sect_perblk, 0,
				fs->blksz, (char *)entries);
	if (!status) {
		status = -EIO;
		goto out;
	}

	count = fs->blksz / sizeof(*entries);
	if (level > 1) {
		for (i = 0; i < count; i++) {
			uint32_t child = le32_to_cpu(entries[i]);

			if (!child)
				continue;
			status = ext4fs_free_indirect_tree(child, level - 1);
			if (status)
				goto out;
		}
	}

	status = ext4fs_free_allocated_block(blknr);
out:
	free(entries);
	return status;
}

static int ext4fs_flush_delete_blocks(const uint32_t *blocks,
				      unsigned int count, char *zero_buffer)
{
	struct ext_filesystem *fs = get_fs();
	unsigned int i;
	int ret;
#ifdef CONFIG_EXT4_PERF_TIMING
	unsigned long start;
#endif

	if (!fs || !ext4fs_root || !fs->blksz || !blocks || !count ||
	    !zero_buffer || (uint64_t)blocks[0] + count >
	    le32_to_cpu(ext4fs_root->sblock.total_blocks))
		return -EINVAL;

#ifdef CONFIG_EXT4_PERF_TIMING
	start = timer_get_us();
#endif
	ret = put_ext4((uint64_t)blocks[0] * fs->blksz, zero_buffer,
		       count * fs->blksz);
	if (!ret)
		ext4fs_perf_count(EXT4FS_PERF_ZERO_BATCH_WRITES);
#ifdef CONFIG_EXT4_PERF_TIMING
	ext4fs_perf_add(EXT4FS_PERF_DATA_IO, timer_get_us() - start);
#endif
	if (ret)
		return ret;

	for (i = 0; i < count; i++) {
		ret = ext4fs_free_allocated_block(blocks[i]);
		if (ret)
			return ret;
	}
	return 0;
}

static int ext4fs_delete_file(int inodeno, int parent_inodeno,
			      bool delete_directory)
{
	struct ext2_inode inode;
	short status;
	int i;
	long int blknr;
	int ibmap_idx;
	char *read_buffer = NULL;
	char *start_block_address = NULL;
	char *parent_buffer = NULL;
	uint32_t no_blocks;
	uint32_t pending_blocks[16];
	unsigned int pending_count = 0;
	unsigned int max_batch_blocks;
	char *zero_buffer = NULL;

	unsigned int inodes_per_block;
	uint32_t blkno;
	unsigned int blkoff;
	uint32_t inode_per_grp;
	struct ext2_inode *inode_buffer = NULL;
	struct ext2_block_group *bgd = NULL;
	struct ext_filesystem *fs = get_fs();
	char *journal_buffer;
	if (!fs || !ext4fs_root || !fs->sb || !fs->dev_desc || !fs->blksz ||
	    !fs->inodesz || !fs->sect_perblk || !fs->blk_bmaps ||
	    !fs->inode_bmaps)
		return -EINVAL;
	inode_per_grp = le32_to_cpu(ext4fs_root->sblock.inodes_per_group);
	journal_buffer = zalloc(fs->blksz);
	if (!journal_buffer)
		return -ENOMEM;
	status = ext4fs_read_inode(ext4fs_root, inodeno, &inode);
	if (status == 0)
		goto fail;
	if (le32_to_cpu(ext4fs_root->sblock.feature_incompat) &
	    EXT4_FEATURE_INCOMPAT_64BIT) {
		printf("ext4: deletion on 64-bit filesystems is unsupported\n");
		goto fail;
	}

	/* read the block no allocated to a file */
	no_blocks = le32_to_cpu(inode.size) / fs->blksz;
	if (le32_to_cpu(inode.size) % fs->blksz)
		no_blocks++;
	if (no_blocks > INT_MAX)
		goto fail;

	/*
	 * special case for symlinks whose target are small enough that
	 *it fits in struct ext2_inode.b.symlink: no block had been allocated
	 */
	if (S_ISLNK(le16_to_cpu(inode.mode)) &&
	    le32_to_cpu(inode.size) <= sizeof(inode.b.symlink)) {
		no_blocks = 0;
	}

	if (le32_to_cpu(inode.flags) & EXT4_EXTENTS_FL) {
    struct ext4_extent_header *eh =
        (struct ext4_extent_header *)
            inode.b.blocks.dir_blocks;

    /*
     * Do not delete extent trees that this implementation
     * cannot safely free.
     */
    if (le16_to_cpu(eh->eh_depth) != 0) {
        printf("rm: unsupported extent tree depth %u\n",
               le16_to_cpu(eh->eh_depth));
        goto fail;
    }

    /*
     * Depth zero still requires correct handling of every
     * referenced data extent before the inode can be freed.
     * Do not treat the extent header as a block-pointer array.
     */
    printf("rm: extent-based deletion is not yet supported\n");
    goto fail;
	}

	/* This routine removes the inode, so shared hard links are unsupported. */
	if (le16_to_cpu(inode.nlinks) != (delete_directory ? 2 : 1)) {
		printf("rm: refusing inode with link count %u\n",
		       le16_to_cpu(inode.nlinks));
		goto fail;
	}

	/* Coalesce adjacent physical blocks into bounded zero-write batches. */
	max_batch_blocks = min_t(unsigned int, ARRAY_SIZE(pending_blocks),
				  (64 * 1024) / fs->blksz);
	if (!max_batch_blocks)
		max_batch_blocks = 1;
	if (no_blocks && (delete_directory || le16_to_cpu(inode.nlinks) == 1)) {
		zero_buffer = zalloc(max_batch_blocks * fs->blksz);
		if (!zero_buffer)
			goto fail;
	}
	/* release data blocks */
	for (i = 0; i < no_blocks; i++) {
		blknr = read_allocated_block(&inode, i, NULL);
		if (blknr == 0) {
			if (pending_count) {
				if (ext4fs_flush_delete_blocks(pending_blocks,
							       pending_count,
						       zero_buffer))
					goto fail;
				pending_count = 0;
			}
			continue;
		}
		if (blknr < 0)
			goto fail;
		if ((uint64_t)blknr >=
		    le32_to_cpu(ext4fs_root->sblock.total_blocks))
			goto fail;
		if (!zero_buffer) {
			if (ext4fs_free_allocated_block(blknr))
				goto fail;
			continue;
		}
		if (pending_count &&
		    (blknr != pending_blocks[pending_count - 1] + 1 ||
		     pending_count == max_batch_blocks)) {
			if (ext4fs_flush_delete_blocks(pending_blocks, pending_count,
						       zero_buffer))
				goto fail;
			pending_count = 0;
		}
		pending_blocks[pending_count++] = blknr;
	}
	if (pending_count &&
	    ext4fs_flush_delete_blocks(pending_blocks, pending_count,
				       zero_buffer))
		goto fail;
	free(zero_buffer);
	zero_buffer = NULL;

	/* All data lookups are complete; indirect blocks can now be released. */
	if (ext4fs_free_indirect_tree(
		    le32_to_cpu(inode.b.blocks.indir_block), 1) ||
	    ext4fs_free_indirect_tree(
		    le32_to_cpu(inode.b.blocks.double_indir_block), 2) ||
	    ext4fs_free_indirect_tree(
		    le32_to_cpu(inode.b.blocks.triple_indir_block), 3))
		goto fail;

	/* release inode */
	/* from the inode no to blockno */
	inodes_per_block = fs->blksz / fs->inodesz;
	if (!inode_per_grp || !inodes_per_block || inodeno <= 0 ||
	    (uint32_t)inodeno >
	    le32_to_cpu(ext4fs_root->sblock.total_inodes))
		goto fail;
	ibmap_idx = (inodeno - 1) / inode_per_grp;
	if (ibmap_idx >= fs->no_blkgrp || !fs->inode_bmaps[ibmap_idx])
		goto fail;

	/* get the block no */
	inodeno--;
	/* get  block group descriptor table */
	bgd = ext4fs_get_group_descriptor(fs, ibmap_idx);
	blkno = ext4fs_bg_get_inode_table_id(bgd, fs) +
		(inodeno % inode_per_grp) / inodes_per_block;

	/* get the offset of the inode */
	blkoff = ((inodeno) % inodes_per_block) * fs->inodesz;

	/* read the block no containing the inode */
	read_buffer = zalloc(fs->blksz);
	if (!read_buffer)
		goto fail;
	start_block_address = read_buffer;
	status = ext4fs_devread((lbaint_t)blkno * fs->sect_perblk,
				0, fs->blksz, read_buffer);
	if (status == 0)
		goto fail;

	if (ext4fs_log_journal(read_buffer, blkno))
		goto fail;

	read_buffer = read_buffer + blkoff;
	inode_buffer = (struct ext2_inode *)read_buffer;
	memset(inode_buffer, '\0', fs->inodesz);

	/* write the inode to original position in inode table */
	if (delete_directory) {
		uint32_t parent_zero_based, parent_idx, parent_blk, parent_off;
		struct ext2_block_group *parent_bgd;

		if (!parent_inodeno || !g_parent_inode ||
		    le16_to_cpu(g_parent_inode->nlinks) <= 2)
			goto fail;
		g_parent_inode->nlinks =
			cpu_to_le16(le16_to_cpu(g_parent_inode->nlinks) - 1);
		parent_zero_based = parent_inodeno - 1;
		parent_idx = parent_zero_based / inode_per_grp;
		if (parent_idx >= fs->no_blkgrp)
			goto fail;
		parent_bgd = ext4fs_get_group_descriptor(fs, parent_idx);
		parent_blk = ext4fs_bg_get_inode_table_id(parent_bgd, fs) +
			(parent_zero_based % inode_per_grp) / inodes_per_block;
		parent_off = (parent_zero_based % inodes_per_block) * fs->inodesz;
		if (parent_blk == blkno) {
			memcpy(start_block_address + parent_off, g_parent_inode,
			       fs->inodesz);
			if (ext4fs_put_metadata(start_block_address, blkno))
				goto fail;
		} else {
			parent_buffer = zalloc(fs->blksz);
			if (!parent_buffer)
				goto fail;
			status = ext4fs_devread((lbaint_t)parent_blk * fs->sect_perblk,
						0, fs->blksz, parent_buffer);
			if (!status || ext4fs_log_journal(parent_buffer, parent_blk))
				goto fail;
			memcpy(parent_buffer + parent_off, g_parent_inode,
			       fs->inodesz);
			if (ext4fs_put_metadata(parent_buffer, parent_blk))
				goto fail;
			if (ext4fs_put_metadata(start_block_address, blkno))
				goto fail;
		}
	} else if (ext4fs_put_metadata(start_block_address, blkno)) {
		goto fail;
	}

	/* update the respective inode bitmaps */
	inodeno++;
	ext4fs_reset_inode_bmap(inodeno, fs->inode_bmaps[ibmap_idx], ibmap_idx);
	ext4fs_bg_free_inodes_inc(bgd, fs);
	ext4fs_sb_free_inodes_inc(fs->sb);
	if (delete_directory && ext4fs_bg_used_dirs_dec(bgd, fs))
		goto fail;
	/* journal backup */
	memset(journal_buffer, '\0', fs->blksz);
	status = ext4fs_devread(ext4fs_bg_get_inode_id(bgd, fs) *
				fs->sect_perblk, 0, fs->blksz, journal_buffer);
	if (status == 0)
		goto fail;
	if (ext4fs_log_journal(journal_buffer, ext4fs_bg_get_inode_id(bgd, fs)))
		goto fail;

	if (ext4fs_update())
		goto fail;
	ext4fs_deinit();
	ext4fs_reinit_global();

	if (ext4fs_init() != 0) {
		printf("error in File System init\n");
		goto fail;
	}

	free(start_block_address);
	free(parent_buffer);
	free(journal_buffer);

	return 0;
fail:
	free(zero_buffer);
	free(start_block_address);
	free(parent_buffer);
	free(journal_buffer);

	return -1;
}

int ext4fs_init(void)
{
	short status;
	int i;
	uint32_t real_free_blocks = 0;
	struct ext_filesystem *fs = get_fs();
#ifdef CONFIG_EXT4_PERF_TIMING
	unsigned long init_start = timer_get_us();
#endif

	/* populate fs */
	fs->blksz = EXT2_BLOCK_SIZE(ext4fs_root);
	fs->sect_perblk = fs->blksz >> fs->dev_desc->log2blksz;

	/* get the superblock */
	fs->sb = zalloc(SUPERBLOCK_SIZE);
	if (!fs->sb) {
#ifdef CONFIG_EXT4_PERF_TIMING
		ext4fs_perf_add(EXT4FS_PERF_INIT,
				timer_get_us() - init_start);
#endif
		return -ENOMEM;
	}
	if (!ext4_read_superblock((char *)fs->sb))
		goto fail;

	/* init journal */
	if (ext4fs_init_journal())
		goto fail;

	/* get total no of blockgroups */
	fs->no_blkgrp = (uint32_t)ext4fs_div_roundup(
			le32_to_cpu(ext4fs_root->sblock.total_blocks)
			- le32_to_cpu(ext4fs_root->sblock.first_data_block),
			le32_to_cpu(ext4fs_root->sblock.blocks_per_group));

	/* get the block group descriptor table */
	fs->gdtable_blkno = ((EXT2_MIN_BLOCK_SIZE == fs->blksz) + 1);
	if (ext4fs_get_bgdtable() == -1) {
		printf("Error in getting the block group descriptor table\n");
		goto fail;
	}

	/* load all the available bitmap block of the partition */
	fs->blk_bmaps = zalloc(fs->no_blkgrp * sizeof(char *));
	if (!fs->blk_bmaps)
		goto fail;
	for (i = 0; i < fs->no_blkgrp; i++) {
		fs->blk_bmaps[i] = zalloc(fs->blksz);
		if (!fs->blk_bmaps[i])
			goto fail;
	}

	for (i = 0; i < fs->no_blkgrp; i++) {
		struct ext2_block_group *bgd =
			ext4fs_get_group_descriptor(fs, i);
		status = ext4fs_devread(ext4fs_bg_get_block_id(bgd, fs) *
				   fs->sect_perblk, 0,
				   fs->blksz, (char *)fs->blk_bmaps[i]);
		if (status == 0)
			goto fail;
	}

	/* load all the available inode bitmap of the partition */
	fs->inode_bmaps = zalloc(fs->no_blkgrp * sizeof(unsigned char *));
	if (!fs->inode_bmaps)
		goto fail;
	for (i = 0; i < fs->no_blkgrp; i++) {
		fs->inode_bmaps[i] = zalloc(fs->blksz);
		if (!fs->inode_bmaps[i])
			goto fail;
	}

	for (i = 0; i < fs->no_blkgrp; i++) {
		struct ext2_block_group *bgd =
			ext4fs_get_group_descriptor(fs, i);
		status = ext4fs_devread(ext4fs_bg_get_inode_id(bgd, fs) *
					fs->sect_perblk,
					0, fs->blksz,
					(char *)fs->inode_bmaps[i]);
		if (status == 0)
			goto fail;
	}

	/*
	 * check filesystem consistency with free blocks of file system
	 * some time we observed that superblock freeblocks does not match
	 * with the  blockgroups freeblocks when improper
	 * reboot of a linux kernel
	 */
	for (i = 0; i < fs->no_blkgrp; i++) {
		struct ext2_block_group *bgd =
			ext4fs_get_group_descriptor(fs, i);
		real_free_blocks = real_free_blocks +
			ext4fs_bg_get_free_blocks(bgd, fs);
	}
	if (real_free_blocks != ext4fs_sb_get_free_blocks(fs->sb))
		ext4fs_sb_set_free_blocks(fs->sb, real_free_blocks);

	#ifdef CONFIG_EXT4_PERF_TIMING
	ext4fs_perf_add(EXT4FS_PERF_INIT, timer_get_us() - init_start);
	#endif
	return 0;
fail:
	ext4fs_deinit();
#ifdef CONFIG_EXT4_PERF_TIMING
	ext4fs_perf_add(EXT4FS_PERF_INIT, timer_get_us() - init_start);
#endif

	return -1;
}

void ext4fs_deinit(void)
{
	int i;
	struct ext2_inode inode_journal;
	struct journal_superblock_t *jsb;
	uint32_t blknr;
	struct ext_filesystem *fs = get_fs();
	uint32_t new_feature_incompat;

	/* free journal */
	char *temp_buff = zalloc(fs->blksz);
	if (temp_buff) {
		ext4fs_read_inode(ext4fs_root, EXT2_JOURNAL_INO,
				  &inode_journal);
		blknr = read_allocated_block(&inode_journal,
					EXT2_JOURNAL_SUPERBLOCK, NULL);
		ext4fs_devread((lbaint_t)blknr * fs->sect_perblk, 0, fs->blksz,
			       temp_buff);
		jsb = (struct journal_superblock_t *)temp_buff;
		jsb->s_start = 0;
		put_ext4((uint64_t) ((uint64_t)blknr * (uint64_t)fs->blksz),
			 (struct journal_superblock_t *)temp_buff, fs->blksz);
		free(temp_buff);
	}
	ext4fs_free_journal();

	/* get the superblock */
	ext4_read_superblock((char *)fs->sb);
	new_feature_incompat = le32_to_cpu(fs->sb->feature_incompat);
	new_feature_incompat &= ~EXT3_FEATURE_INCOMPAT_RECOVER;
	fs->sb->feature_incompat = cpu_to_le32(new_feature_incompat);
	put_ext4((uint64_t)(SUPERBLOCK_SIZE),
		 (struct ext2_sblock *)fs->sb, (uint32_t)SUPERBLOCK_SIZE);
	free(fs->sb);
	fs->sb = NULL;

	if (fs->blk_bmaps) {
		for (i = 0; i < fs->no_blkgrp; i++) {
			free(fs->blk_bmaps[i]);
			fs->blk_bmaps[i] = NULL;
		}
		free(fs->blk_bmaps);
		fs->blk_bmaps = NULL;
	}

	if (fs->inode_bmaps) {
		for (i = 0; i < fs->no_blkgrp; i++) {
			free(fs->inode_bmaps[i]);
			fs->inode_bmaps[i] = NULL;
		}
		free(fs->inode_bmaps);
		fs->inode_bmaps = NULL;
	}

	free(fs->gdtable);
	fs->gdtable = NULL;
	/*
	 * reinitiliazed the global inode and
	 * block bitmap first execution check variables
	 */
	fs->first_pass_ibmap = 0;
	fs->first_pass_bbmap = 0;
	fs->curr_inode_no = 0;
	fs->curr_blkno = 0;
}

/*
 * Write data to filesystem blocks. Uses same optimization for
 * contigous sectors as ext4fs_read_file
 */
static int ext4fs_write_file(struct ext2_inode *file_inode,
			     int pos, unsigned int len, const char *buf)
{
	int i;
	int blockcnt;
	uint32_t filesize = le32_to_cpu(file_inode->size);
	struct ext_filesystem *fs = get_fs();
	int log2blksz = fs->dev_desc->log2blksz;
	int log2_fs_blocksize = LOG2_BLOCK_SIZE(ext4fs_root) - log2blksz;
	int previous_block_number = -1;
	int delayed_start = 0;
	int delayed_extent = 0;
	int delayed_next = 0;
	const char *delayed_buf = NULL;

	/* Adjust len so it we can't read past the end of the file. */
	if (len > filesize)
		len = filesize;

	blockcnt = ((len + pos) + fs->blksz - 1) / fs->blksz;

	for (i = pos / fs->blksz; i < blockcnt; i++) {
		long int blknr;
		int blockend = fs->blksz;
		int skipfirst = 0;
		blknr = read_allocated_block(file_inode, i, NULL);
		if (blknr <= 0)
			return -1;

		blknr = blknr << log2_fs_blocksize;

		if (blknr) {
			if (previous_block_number != -1) {
				if (delayed_next == blknr) {
					delayed_extent += blockend;
					delayed_next += blockend >> log2blksz;
				} else {	/* spill */
					if (put_ext4((uint64_t)
						 ((uint64_t)delayed_start << log2blksz),
						 delayed_buf,
						 (uint32_t)delayed_extent))
						return -EIO;
					previous_block_number = blknr;
					delayed_start = blknr;
					delayed_extent = blockend;
					delayed_buf = buf;
					delayed_next = blknr +
					    (blockend >> log2blksz);
				}
			} else {
				previous_block_number = blknr;
				delayed_start = blknr;
				delayed_extent = blockend;
				delayed_buf = buf;
				delayed_next = blknr +
				    (blockend >> log2blksz);
			}
		} else {
			if (previous_block_number != -1) {
				/* spill */
				if (put_ext4((uint64_t) ((uint64_t)delayed_start <<
						     log2blksz),
					 delayed_buf,
					 (uint32_t)delayed_extent))
					return -EIO;
				previous_block_number = -1;
			}
		}
		buf += fs->blksz - skipfirst;
	}
	if (previous_block_number != -1) {
		/* spill */
		if (put_ext4((uint64_t) ((uint64_t)delayed_start << log2blksz),
			     delayed_buf, (uint32_t)delayed_extent))
			return -EIO;
		previous_block_number = -1;
	}

	return 0;
}

int ext4fs_write(const char *fname, const char *buffer,
		 unsigned long sizebytes, int type)
{
	int ret = 0;
	struct ext2_inode *file_inode = NULL;
	struct ext2_inode *existing_file_inode = NULL;
	unsigned char *inode_buffer = NULL;
	int parent_inodeno;
	int inodeno;
	time_t timestamp = 0;

	uint64_t bytes_reqd_for_file;
	unsigned int blks_reqd_for_file;
	unsigned int blocks_remaining;
	int existing_file_inodeno;
	char *temp_ptr = NULL;
	long int itable_blkno;
	long int parent_itable_blkno;
	long int blkoff;
	struct ext2_sblock *sblock = &(ext4fs_root->sblock);
	unsigned int inodes_per_block;
	unsigned int ibmap_idx;
	struct ext2_block_group *bgd = NULL;
	struct ext_filesystem *fs = get_fs();
#ifdef CONFIG_EXT4_PERF_TIMING
	unsigned long stage_start;
#endif
	ALLOC_CACHE_ALIGN_BUFFER(char, filename, 256);
	bool store_link_in_inode = false;
	ext4fs_perf_reset();
	memset(filename, 0x00, 256);
	int missing_feat;

	if (sizebytes > UINT32_MAX)
		return -EFBIG;
	if (type != FILETYPE_REG && type != FILETYPE_SYMLINK)
		return -1;

	g_parent_inode = zalloc(fs->inodesz);
	if (!g_parent_inode)
		goto fail;

	ext4fs_reinit_global();
	if (ext4fs_init() != 0) {
		printf("error in File System init\n");
		/* Skip ext4fs_deinit since ext4fs_init() already done that */
		goto fail_init;
	}

	missing_feat = le32_to_cpu(fs->sb->feature_incompat) & ~EXT4_FEATURE_INCOMPAT_SUPP;
	if (missing_feat) {
		log_err("Unsupported features found %08x, not writing.\n", missing_feat);
		goto fail;
	}

	missing_feat = le32_to_cpu(fs->sb->feature_ro_compat) & ~EXT4_FEATURE_RO_COMPAT_SUPP;
	if (missing_feat) {
		log_err("Unsupported RO compat features found %08x, not writing.\n", missing_feat);
		goto fail;
	}

	inodes_per_block = fs->blksz / fs->inodesz;
	parent_inodeno = ext4fs_get_parent_inode_num(fname, filename, F_FILE);
	if (parent_inodeno == -1)
		goto fail;
	if (ext4fs_iget(parent_inodeno, g_parent_inode))
		goto fail;
	/* do not mess up a directory using hash trees */
	if (le32_to_cpu(g_parent_inode->flags) & EXT4_INDEX_FL) {
		printf("hash tree directory\n");
		goto fail;
	}
	/* check if the filename is already present in root */
	existing_file_inodeno = ext4fs_filename_unlink(filename);
	if (existing_file_inodeno != -1) {
		existing_file_inode = (struct ext2_inode *)zalloc(fs->inodesz);
		if (!existing_file_inode)
			goto fail;
		ret = ext4fs_iget(existing_file_inodeno, existing_file_inode);
		if (ret) {
			free(existing_file_inode);
			goto fail;
		}

		ret = ext4fs_delete_file(existing_file_inodeno, parent_inodeno,
					 false);
		fs->first_pass_bbmap = 0;
		fs->curr_blkno = 0;

		fs->first_pass_ibmap = 0;
		fs->curr_inode_no = 0;
		if (ret)
			goto fail;
	}

	/* calculate how many blocks required */
	if (type == FILETYPE_SYMLINK &&
	    sizebytes <= sizeof(file_inode->b.symlink)) {
		store_link_in_inode = true;
		bytes_reqd_for_file = 0;
	} else {
		bytes_reqd_for_file = sizebytes;
	}

	blks_reqd_for_file = lldiv(bytes_reqd_for_file, fs->blksz);
	if (do_div(bytes_reqd_for_file, fs->blksz) != 0) {
		blks_reqd_for_file++;
		debug("total bytes for a file %u\n", blks_reqd_for_file);
	}
	blocks_remaining = blks_reqd_for_file;
	/* test for available space in partition */
	if (le32_to_cpu(fs->sb->free_blocks) < blks_reqd_for_file) {
		printf("Not enough space on partition !!!\n");
		goto fail;
	}

	inodeno = ext4fs_update_parent_dentry(filename, type);
	if (inodeno == -1)
		goto fail;
	/* prepare file inode */
	inode_buffer = zalloc(fs->inodesz);
	if (!inode_buffer)
		goto fail;
	file_inode = (struct ext2_inode *)inode_buffer;
	file_inode->size = cpu_to_le32(sizebytes);
	if (type == FILETYPE_SYMLINK) {
		file_inode->mode = cpu_to_le16(S_IFLNK | S_IRWXU | S_IRWXG |
					       S_IRWXO);
		if (store_link_in_inode) {
			strncpy(file_inode->b.symlink, buffer, sizebytes);
			sizebytes = 0;
		}
	} else {
		if (existing_file_inode) {
			file_inode->mode = existing_file_inode->mode;
		} else {
			file_inode->mode = cpu_to_le16(S_IFREG | S_IRWXU | S_IRGRP |
						       S_IROTH | S_IXGRP | S_IXOTH);
		}
	}
	if (existing_file_inode)
		free(existing_file_inode);
	/* ToDo: Update correct time */
	file_inode->mtime = cpu_to_le32(timestamp);
	file_inode->atime = cpu_to_le32(timestamp);
	file_inode->ctime = cpu_to_le32(timestamp);
	file_inode->nlinks = cpu_to_le16(1);

	/* Allocate data blocks */
	#ifdef CONFIG_EXT4_PERF_TIMING
	stage_start = timer_get_us();
	#endif
	ext4fs_allocate_blocks(file_inode, blocks_remaining,
			       &blks_reqd_for_file);
	#ifdef CONFIG_EXT4_PERF_TIMING
	ext4fs_perf_add(EXT4FS_PERF_ALLOC, timer_get_us() - stage_start);
	#endif
	file_inode->blockcnt = cpu_to_le32((blks_reqd_for_file * fs->blksz) >>
					   LOG2_SECTOR_SIZE);

	temp_ptr = zalloc(fs->blksz);
	if (!temp_ptr)
		goto fail;
	ibmap_idx = inodeno / le32_to_cpu(ext4fs_root->sblock.inodes_per_group);
	inodeno--;
	bgd = ext4fs_get_group_descriptor(fs, ibmap_idx);
	itable_blkno = ext4fs_bg_get_inode_table_id(bgd, fs) +
			(inodeno % le32_to_cpu(sblock->inodes_per_group)) /
			inodes_per_block;
	blkoff = (inodeno % inodes_per_block) * fs->inodesz;
	ext4fs_devread((lbaint_t)itable_blkno * fs->sect_perblk, 0, fs->blksz,
		       temp_ptr);
	if (ext4fs_log_journal(temp_ptr, itable_blkno))
		goto fail;

	memcpy(temp_ptr + blkoff, inode_buffer, fs->inodesz);
	if (ext4fs_put_metadata(temp_ptr, itable_blkno))
		goto fail;
	/* copy the file content into data blocks */
	#ifdef CONFIG_EXT4_PERF_TIMING
	stage_start = timer_get_us();
	#endif
	if (ext4fs_write_file(file_inode, 0, sizebytes, buffer)) {
		printf("Error in copying content\n");
		/* FIXME: Deallocate data blocks */
		goto fail;
	}
	#ifdef CONFIG_EXT4_PERF_TIMING
	ext4fs_perf_add(EXT4FS_PERF_DATA_IO, timer_get_us() - stage_start);
	#endif
	ibmap_idx = parent_inodeno / le32_to_cpu(ext4fs_root->sblock.inodes_per_group);
	parent_inodeno--;
	bgd = ext4fs_get_group_descriptor(fs, ibmap_idx);
	parent_itable_blkno = ext4fs_bg_get_inode_table_id(bgd, fs) +
	    (parent_inodeno %
	     le32_to_cpu(sblock->inodes_per_group)) / inodes_per_block;
	blkoff = (parent_inodeno % inodes_per_block) * fs->inodesz;
	if (parent_itable_blkno != itable_blkno) {
		memset(temp_ptr, '\0', fs->blksz);
		ext4fs_devread((lbaint_t)parent_itable_blkno * fs->sect_perblk,
			       0, fs->blksz, temp_ptr);
		if (ext4fs_log_journal(temp_ptr, parent_itable_blkno))
			goto fail;

		memcpy(temp_ptr + blkoff, g_parent_inode, fs->inodesz);
		if (ext4fs_put_metadata(temp_ptr, parent_itable_blkno))
			goto fail;
	} else {
		/*
		 * If parent and child fall in same inode table block
		 * both should be kept in 1 buffer
		 */
		memcpy(temp_ptr + blkoff, g_parent_inode, fs->inodesz);
		gd_index--;
		if (ext4fs_put_metadata(temp_ptr, itable_blkno))
			goto fail;
	}
	if (ext4fs_update())
		goto fail;
	ext4fs_deinit();

	fs->first_pass_bbmap = 0;
	fs->curr_blkno = 0;
	fs->first_pass_ibmap = 0;
	fs->curr_inode_no = 0;
	free(inode_buffer);
	free(g_parent_inode);
	free(temp_ptr);
	g_parent_inode = NULL;
	ext4fs_perf_report("ext4write");

	return 0;
fail:
	ext4fs_deinit();
fail_init:
	free(inode_buffer);
	free(g_parent_inode);
	free(temp_ptr);
	g_parent_inode = NULL;
	ext4fs_perf_report("ext4write-failed");

	return -1;
}

int ext4_write_file(const char *filename, void *buf, loff_t offset,
		    loff_t len, loff_t *actwrite)
{
	int ret;

	if (offset != 0) {
		printf("** Cannot support non-zero offset **\n");
		return -1;
	}

	if (len > UINT_MAX) {
		printf("** File is too large for ext4write **\n");
		goto fail;
	}
	ret = ext4fs_write(filename, buf, len, FILETYPE_REG);
	if (ret) {
		printf("** Error ext4fs_write() **\n");
		goto fail;
	}

	*actwrite = len;

	return 0;

fail:
	*actwrite = 0;

	return -1;
}

int ext4fs_create_link(const char *target, const char *fname)
{
	return ext4fs_write(fname, target, strlen(target), FILETYPE_SYMLINK);
}

/* Empty directories must contain valid self and parent entries only. */
static int ext4fs_directory_empty(struct ext2fs_node *node, int parent_ino)
{
	struct ext_filesystem *fs = get_fs();
	struct ext2_dirent de;
	bool dot = false, dotdot = false;
	uint64_t pos = 0;
	uint32_t size = le32_to_cpu(node->inode.size);

	if (!fs->blksz || !S_ISDIR(le16_to_cpu(node->inode.mode)) ||
	    !size || size % 4)
		return -EINVAL;

	while (pos < size) {
		char name[EXT2_NAME_LEN + 1];
		loff_t actread;
		uint16_t rec_len;

		if (size - pos < sizeof(de) ||
		    ext4fs_read_file(node, pos, sizeof(de), (char *)&de,
				     &actread) || actread != sizeof(de))
			return -EIO;
		rec_len = le16_to_cpu(de.direntlen);
		if (rec_len < sizeof(de) || (rec_len & 3) ||
		    rec_len > size - pos ||
		    pos % fs->blksz + rec_len > fs->blksz ||
		    de.namelen > rec_len - sizeof(de) ||
		    de.namelen > EXT2_NAME_LEN)
			return -EIO;

		if (le32_to_cpu(de.inode)) {
			if (!de.namelen ||
			    ext4fs_read_file(node, pos + sizeof(de), de.namelen,
					     name, &actread) ||
			    actread != de.namelen)
				return -EIO;
			if (de.namelen == 1 && name[0] == '.') {
				if (dot || le32_to_cpu(de.inode) != node->ino)
					return -EIO;
				dot = true;
			} else if (de.namelen == 2 && name[0] == '.' &&
				   name[1] == '.') {
				if (dotdot || le32_to_cpu(de.inode) != parent_ino)
					return -EIO;
				dotdot = true;
			} else {
				return -ENOTEMPTY;
			}
		}
		pos += rec_len;
	}

	return dot && dotdot ? 0 : -EIO;
}

int ext4fs_unlink(const char *fname)
{
	struct ext_filesystem *fs = get_fs();
	struct ext2fs_node *node = NULL;
	ALLOC_CACHE_ALIGN_BUFFER(char, filename, 256);
	int parent_inodeno, inodeno, expected_ino, found_type;
	bool is_directory;
	int missing_feat;
	ext4fs_perf_reset();

	memset(filename, 0x00, 256);

	g_parent_inode = zalloc(fs->inodesz);
	if (!g_parent_inode)
		return -1;

	ext4fs_reinit_global();
	if (ext4fs_init() != 0) {
		printf("error in File System init\n");
		goto fail_init;
	}

	missing_feat = le32_to_cpu(fs->sb->feature_incompat) &
		       ~EXT4_FEATURE_INCOMPAT_SUPP;
	if (missing_feat) {
		log_err("Unsupported features found %08x, not writing.\n",
			missing_feat);
		goto fail;
	}
	missing_feat = le32_to_cpu(fs->sb->feature_ro_compat) &
		       ~EXT4_FEATURE_RO_COMPAT_SUPP;
	if (missing_feat) {
		log_err("Unsupported RO compat features found %08x, not writing.\n",
			missing_feat);
		goto fail;
	}

	/* look up first: nothing is modified until every check has passed */
	if (!ext4fs_find_file_nofollow(fname, &ext4fs_root->diropen, &node,
				       &found_type)) {
		printf("rm: %s not found or path contains a symlink\n", fname);
		goto fail;
	}
	if (!ext4fs_read_inode(ext4fs_root, node->ino, &node->inode)) {
		printf("rm: cannot read inode\n");
		ext4fs_free_node(node, &ext4fs_root->diropen);
		goto fail;
	}
	is_directory = S_ISDIR(le16_to_cpu(node->inode.mode));
	if (node->ino == EXT2_ROOT_INO) {
		printf("rm: refusing to remove filesystem root\n");
		ext4fs_free_node(node, &ext4fs_root->diropen);
		goto fail;
	}
	if ((is_directory && (found_type != FILETYPE_DIRECTORY ||
			      (le32_to_cpu(node->inode.flags) & EXT4_INDEX_FL))) ||
	    (!is_directory && (!S_ISREG(le16_to_cpu(node->inode.mode)) ||
			      found_type != FILETYPE_REG))) {
		printf("rm: unsupported inode type or indexed directory\n");
		ext4fs_free_node(node, &ext4fs_root->diropen);
		goto fail;
	}
	if ((!is_directory && le16_to_cpu(node->inode.nlinks) != 1) ||
	    (is_directory && le16_to_cpu(node->inode.nlinks) != 2)) {
		printf("rm: refusing, link count is %u\n",
		       le16_to_cpu(node->inode.nlinks));
		ext4fs_free_node(node, &ext4fs_root->diropen);
		goto fail;
	}
	expected_ino = node->ino;
	ext4fs_free_node(node, &ext4fs_root->diropen);

	parent_inodeno = ext4fs_get_parent_inode_num(
		fname, filename, is_directory ? (F_FILE | F_DIR) : F_FILE);
	if (parent_inodeno == -1)
		goto fail;
	if (ext4fs_iget(parent_inodeno, g_parent_inode))
		goto fail;
	if (is_directory) {
		if (ext4fs_find_file_nofollow(fname, &ext4fs_root->diropen,
					      &node, &found_type) != 1)
			goto fail;
		if (!ext4fs_read_inode(ext4fs_root, node->ino, &node->inode) ||
		    ext4fs_directory_empty(node, parent_inodeno)) {
			printf("rm: directory is non-empty or malformed\n");
			ext4fs_free_node(node, &ext4fs_root->diropen);
			goto fail;
		}
		ext4fs_free_node(node, &ext4fs_root->diropen);
	}
	if (le32_to_cpu(g_parent_inode->flags) & EXT4_INDEX_FL) {
		printf("hash tree directory\n");
		goto fail;
	}

	inodeno = ext4fs_filename_unlink(filename);   /* removes dir entry */
	if (inodeno == -1)
		goto fail;
	if (inodeno != expected_ino) {
		printf("rm: inode changed during lookup (%d vs %d)\n",
		       inodeno, expected_ino);
		goto fail;
	}

	if (ext4fs_delete_file(inodeno, parent_inodeno, is_directory))
		goto fail;

	fs->first_pass_bbmap = 0;
	fs->curr_blkno = 0;
	fs->first_pass_ibmap = 0;
	fs->curr_inode_no = 0;

	ext4fs_deinit();
	free(g_parent_inode);
	g_parent_inode = NULL;
	ext4fs_perf_report("ext4rm");
	return 0;

fail:
	ext4fs_deinit();
fail_init:
	free(g_parent_inode);
	g_parent_inode = NULL;
	ext4fs_perf_report("ext4rm-failed");
	return -1;
}
