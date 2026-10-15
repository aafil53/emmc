// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2011 - 2012 Samsung Electronics
 * EXT4 filesystem implementation in Uboot by
 * Uma Shankar <uma.shankar@samsung.com>
 * Manjunatha C Achar <a.manjunatha@samsung.com>
 *
 * Journal data structures and headers for Journaling feature of ext4
 * have been referred from JBD2 (Journaling Block device 2)
 * implementation in Linux Kernel.
 * Written by Stephen C. Tweedie <sct@redhat.com>
 *
 * Copyright 1998-2000 Red Hat, Inc --- All Rights Reserved
 */

#include <blk.h>
#include <errno.h>
#include <ext4fs.h>
#include <log.h>
#include <malloc.h>
#include <time.h>
#include <ext_common.h>
#include "ext4_common.h"

static struct revoke_blk_list *revk_blk_list;
static struct revoke_blk_list *prev_node;
static int first_node = true;

int gindex;
int gd_index;
int jrnl_blk_idx;
struct journal_log *journal_ptr[MAX_JOURNAL_ENTRIES];
struct dirty_blocks *dirty_block_ptr[MAX_JOURNAL_ENTRIES];

#ifdef CONFIG_EXT4_PERF_TIMING
static unsigned long ext4fs_perf_us[EXT4FS_PERF_STAGE_COUNT];
static unsigned int ext4fs_perf_counts[EXT4FS_PERF_COUNTER_COUNT];

void ext4fs_perf_reset(void)
{
	memset(ext4fs_perf_us, 0, sizeof(ext4fs_perf_us));
	memset(ext4fs_perf_counts, 0, sizeof(ext4fs_perf_counts));
}

void ext4fs_perf_count(enum ext4fs_perf_counter counter)
{
	if (counter < EXT4FS_PERF_COUNTER_COUNT)
		ext4fs_perf_counts[counter]++;
}

void ext4fs_perf_add(enum ext4fs_perf_stage stage, unsigned long usec)
{
	if (stage < EXT4FS_PERF_STAGE_COUNT)
		ext4fs_perf_us[stage] += usec;
}

void ext4fs_perf_report(const char *operation)
{
	printf("ext4perf %s us: init=%lu scan=%lu recovery=%lu alloc=%lu "
	       "free=%lu data_io=%lu home_meta=%lu journal_update=%lu "
	       "bitmap_reads=%u zero_batches=%u\n",
	       operation, ext4fs_perf_us[EXT4FS_PERF_INIT],
	       ext4fs_perf_us[EXT4FS_PERF_JOURNAL_SCAN],
	       ext4fs_perf_us[EXT4FS_PERF_JOURNAL_RECOVERY],
	       ext4fs_perf_us[EXT4FS_PERF_ALLOC],
	       ext4fs_perf_us[EXT4FS_PERF_FREE],
	       ext4fs_perf_us[EXT4FS_PERF_DATA_IO],
	       ext4fs_perf_us[EXT4FS_PERF_HOME_METADATA],
	       ext4fs_perf_us[EXT4FS_PERF_JOURNAL_UPDATE],
	       ext4fs_perf_counts[EXT4FS_PERF_BITMAP_READS],
	       ext4fs_perf_counts[EXT4FS_PERF_ZERO_BATCH_WRITES]);
}
#else
void ext4fs_perf_reset(void) {}
void ext4fs_perf_add(enum ext4fs_perf_stage stage, unsigned long usec)
{
	(void)stage;
	(void)usec;
}

void ext4fs_perf_count(enum ext4fs_perf_counter counter)
{
	(void)counter;
}

void ext4fs_perf_report(const char *operation)
{
	(void)operation;
}
#endif

bool ext4fs_journal_has_block(uint32_t blknr)
{
	int i;

	for (i = 0; i < MAX_JOURNAL_ENTRIES; i++) {
		if (!journal_ptr[i] || journal_ptr[i]->blknr == -1)
			return false;
		if (journal_ptr[i]->blknr == blknr)
			return true;
	}

	return false;
}

int ext4fs_init_journal(void)
{
	int i;
	char *temp = NULL;
	struct ext_filesystem *fs = get_fs();
	unsigned long start;

	/* init globals */
	revk_blk_list = NULL;
	prev_node = NULL;
	gindex = 0;
	gd_index = 0;
	jrnl_blk_idx = 1;

	for (i = 0; i < MAX_JOURNAL_ENTRIES; i++) {
		journal_ptr[i] = zalloc(sizeof(struct journal_log));
		if (!journal_ptr[i])
			goto fail;
		dirty_block_ptr[i] = zalloc(sizeof(struct dirty_blocks));
		if (!dirty_block_ptr[i])
			goto fail;
		journal_ptr[i]->buf = NULL;
		journal_ptr[i]->blknr = -1;

		dirty_block_ptr[i]->buf = NULL;
		dirty_block_ptr[i]->blknr = -1;
	}

	if (fs->blksz == 4096) {
		temp = zalloc(fs->blksz);
		if (!temp)
			goto fail;
		journal_ptr[gindex]->buf = zalloc(fs->blksz);
		if (!journal_ptr[gindex]->buf)
			goto fail;
		if (!ext4fs_devread(0, 0, fs->blksz, temp))
			goto fail;
		memcpy(temp + SUPERBLOCK_SIZE, fs->sb, SUPERBLOCK_SIZE);
		memcpy(journal_ptr[gindex]->buf, temp, fs->blksz);
		journal_ptr[gindex++]->blknr = 0;
		free(temp);
		temp = NULL;
	} else {
		journal_ptr[gindex]->buf = zalloc(fs->blksz);
		if (!journal_ptr[gindex]->buf)
			goto fail;
		memcpy(journal_ptr[gindex]->buf, fs->sb, SUPERBLOCK_SIZE);
		journal_ptr[gindex++]->blknr = 1;
	}

	/* Check the file system state using journal super block */
	start = timer_get_us();
	i = ext4fs_check_journal_state(SCAN);
	ext4fs_perf_add(EXT4FS_PERF_JOURNAL_SCAN, timer_get_us() - start);
	if (i)
		goto fail;
	/* Check the file system state using journal super block */
	start = timer_get_us();
	i = ext4fs_check_journal_state(RECOVER);
	ext4fs_perf_add(EXT4FS_PERF_JOURNAL_RECOVERY,
			timer_get_us() - start);
	if (i)
		goto fail;

	return 0;
fail:
	free(temp);
	ext4fs_free_journal();
	ext4fs_free_revoke_blks();
	return -1;
}

int ext4fs_dump_metadata(void)
{
	struct ext_filesystem *fs = get_fs();
	int i;
	int ret;

	if (!fs)
		return -ENODEV;

	for (i = 0; i < MAX_JOURNAL_ENTRIES; i++) {
		if (!dirty_block_ptr[i] ||
		    dirty_block_ptr[i]->blknr == -1)
			break;
		if (!dirty_block_ptr[i]->buf)
			return -EINVAL;

		ret = put_ext4((uint64_t)dirty_block_ptr[i]->blknr *
			       (uint64_t)fs->blksz,
			       dirty_block_ptr[i]->buf, fs->blksz);
		if (ret)
			return ret;
	}

	return 0;
}

void ext4fs_free_journal(void)
{
	int i;

	/*
	 * Initialization may fail before every entry is allocated.
	 * Visit every slot and tolerate NULL pointers.
	 */
	for (i = 0; i < MAX_JOURNAL_ENTRIES; i++) {
		if (dirty_block_ptr[i]) {
			free(dirty_block_ptr[i]->buf);
			dirty_block_ptr[i]->buf = NULL;
			free(dirty_block_ptr[i]);
			dirty_block_ptr[i] = NULL;
		}

		if (journal_ptr[i]) {
			free(journal_ptr[i]->buf);
			journal_ptr[i]->buf = NULL;
			free(journal_ptr[i]);
			journal_ptr[i] = NULL;
		}
	}

	gindex = 0;
	gd_index = 0;
	jrnl_blk_idx = 1;
}

int ext4fs_log_gdt(char *gd_table)
{
	struct ext_filesystem *fs = get_fs();
	short i;
	long int var = fs->gdtable_blkno;

	/* Make sure there is enough journal entries */
	if (fs->no_blk_pergdt > MAX_JOURNAL_ENTRIES) {
		log_err("*** Not enough journal entries allocated\n");
		return -ENOMEM;
	}

	for (i = 0; i < fs->no_blk_pergdt; i++) {
		journal_ptr[gindex]->buf = zalloc(fs->blksz);
		if (!journal_ptr[gindex]->buf)
			return -ENOMEM;
		memcpy(journal_ptr[gindex]->buf, gd_table, fs->blksz);
		gd_table += fs->blksz;
		journal_ptr[gindex++]->blknr = var++;
	}

	return 0;
}

/*
 * This function stores the backup copy of meta data in RAM
 * journal_buffer -- Buffer containing meta data
 * blknr -- Block number on disk of the meta data buffer
 */
int ext4fs_log_journal(char *journal_buffer, uint32_t blknr)
{
	struct ext_filesystem *fs = get_fs();
	short i;

	if (!journal_buffer) {
		printf("Invalid input arguments %s\n", __func__);
		return -EINVAL;
	}

	for (i = 0; i < MAX_JOURNAL_ENTRIES; i++) {
		if (journal_ptr[i]->blknr == -1)
			break;
		if (journal_ptr[i]->blknr == blknr)
			return 0;
	}

	/* Prevent out-of-bounds access */
	if (gindex >= MAX_JOURNAL_ENTRIES) {
		log_err("Journal entry limit reached\n");
		return -ENOMEM;
	}

	journal_ptr[gindex]->buf = zalloc(fs->blksz);
	if (!journal_ptr[gindex]->buf)
		return -ENOMEM;

	memcpy(journal_ptr[gindex]->buf, journal_buffer, fs->blksz);
	journal_ptr[gindex++]->blknr = blknr;

	return 0;
}

/*
 * This function stores the modified meta data in RAM
 * metadata_buffer -- Buffer containing meta data
 * blknr -- Block number on disk of the meta data buffer
 */
int ext4fs_put_metadata(char *metadata_buffer, uint32_t blknr)
{
	struct ext_filesystem *fs = get_fs();
	if (!metadata_buffer) {
		printf("Invalid input arguments %s\n", __func__);
		return -EINVAL;
	}

	/* Prevent out-of-bounds access */
	if (gd_index >= MAX_JOURNAL_ENTRIES) {
		log_err("Dirty metadata entry limit reached\n");
		return -ENOMEM;
	}


	if (dirty_block_ptr[gd_index]->buf)
		assert(dirty_block_ptr[gd_index]->blknr == blknr);
	else
		dirty_block_ptr[gd_index]->buf = zalloc(fs->blksz);

	if (!dirty_block_ptr[gd_index]->buf)
		return -ENOMEM;
	memcpy(dirty_block_ptr[gd_index]->buf, metadata_buffer, fs->blksz);
	dirty_block_ptr[gd_index++]->blknr = blknr;

	return 0;
}

void print_revoke_blks(char *revk_blk)
{
	int offset;
	int max;
	long int blocknr;
	struct journal_revoke_header_t *header;

	if (revk_blk == NULL)
		return;

	header = (struct journal_revoke_header_t *) revk_blk;
	offset = sizeof(struct journal_revoke_header_t);
	max = be32_to_cpu(header->r_count);
	printf("total bytes %d\n", max);

	while (offset < max) {
		blocknr = be32_to_cpu(*((__be32 *)(revk_blk + offset)));
		printf("revoke blknr is %ld\n", blocknr);
		offset += 4;
	}
}

static struct revoke_blk_list *_get_node(void)
{
	struct revoke_blk_list *tmp_node;
	tmp_node = zalloc(sizeof(struct revoke_blk_list));
	if (tmp_node == NULL)
		return NULL;
	tmp_node->content = NULL;
	tmp_node->next = NULL;

	return tmp_node;
}

void ext4fs_push_revoke_blk(char *buffer)
{
	struct revoke_blk_list *node = NULL;
	struct ext_filesystem *fs = get_fs();
	if (buffer == NULL) {
		printf("buffer ptr is NULL\n");
		return;
	}
	node = _get_node();
	if (!node) {
		printf("_get_node: malloc failed\n");
		return;
	}

	node->content = zalloc(fs->blksz);
	if (!node->content) {
		free(node);
		return;
	}
	memcpy(node->content, buffer, fs->blksz);

	if (first_node == true) {
		revk_blk_list = node;
		prev_node = node;
		 first_node = false;
	} else {
		prev_node->next = node;
		prev_node = node;
	}
}

void ext4fs_free_revoke_blks(void)
{
	struct revoke_blk_list *tmp_node = revk_blk_list;
	struct revoke_blk_list *next_node = NULL;

	while (tmp_node != NULL) {
		free(tmp_node->content);
		tmp_node = tmp_node->next;
	}

	tmp_node = revk_blk_list;
	while (tmp_node != NULL) {
		next_node = tmp_node->next;
		free(tmp_node);
		tmp_node = next_node;
	}

	revk_blk_list = NULL;
	prev_node = NULL;
	first_node = true;
}

int check_blknr_for_revoke(long int blknr, int sequence_no)
{
	struct journal_revoke_header_t *header;
	int offset;
	int max;
	long int blocknr;
	char *revk_blk;
	struct revoke_blk_list *tmp_revk_node = revk_blk_list;
	while (tmp_revk_node != NULL) {
		revk_blk = tmp_revk_node->content;

		header = (struct journal_revoke_header_t *) revk_blk;
		if (sequence_no < be32_to_cpu(header->r_header.h_sequence)) {
			offset = sizeof(struct journal_revoke_header_t);
			max = be32_to_cpu(header->r_count);

			while (offset < max) {
				blocknr = be32_to_cpu(*((__be32 *)
						  (revk_blk + offset)));
				if (blocknr == blknr)
					goto found;
				offset += 4;
			}
		}
		tmp_revk_node = tmp_revk_node->next;
	}

	return -1;

found:
	return 0;
}

/*
 * This function parses the journal blocks and replays the
 * suceessful transactions. A transaction is successfull
 * if commit block is found for a descriptor block
 * The tags in descriptor block contain the disk block
 * numbers of the metadata  to be replayed
 */

int recover_transaction(int prev_desc_logical_no)
{
	struct ext2_inode inode_journal;
	struct ext_filesystem *fs = get_fs();
	struct journal_header_t *jdb;
	long int blknr;
	char *p_jdb;
	int ofs, flags = 0;
	int i;
	int ret = -1;
	struct ext3_journal_block_tag *tag;
	char *temp_buff = NULL;
	char *metadata_buff = NULL;

	if (!fs || !fs->blksz || !ext4fs_root)
		return -1;

	temp_buff = zalloc(fs->blksz);
	metadata_buff = zalloc(fs->blksz);
	if (!temp_buff || !metadata_buff)
		goto fail;

	if (!ext4fs_read_inode(ext4fs_root, EXT2_JOURNAL_INO,
			       &inode_journal))
		goto fail;

	i = prev_desc_logical_no;
	blknr = read_allocated_block(&inode_journal, i, NULL);
	if (blknr <= 0)
		goto fail;

	if (!ext4fs_devread((lbaint_t)blknr * fs->sect_perblk,
			    0, fs->blksz, temp_buff))
		goto fail;

	p_jdb = temp_buff;
	jdb = (struct journal_header_t *)temp_buff;
	ofs = sizeof(struct journal_header_t);

	do {
		if (ofs > fs->blksz ||
		    sizeof(*tag) > fs->blksz - ofs)
			goto fail;

		tag = (struct ext3_journal_block_tag *)(p_jdb + ofs);
		ofs += sizeof(*tag);

		flags = be32_to_cpu(tag->flags);
		if (!(flags & EXT3_JOURNAL_FLAG_SAME_UUID)) {
			if (16 > fs->blksz - ofs)
				goto fail;
			ofs += 16;
		}

		i++;
		debug("\ttag %u\n", be32_to_cpu(tag->block));

		if (revk_blk_list != NULL &&
		    check_blknr_for_revoke(be32_to_cpu(tag->block),
					   be32_to_cpu(jdb->h_sequence)) == 0)
			continue;

		blknr = read_allocated_block(&inode_journal, i, NULL);
		if (blknr <= 0)
			goto fail;

		if (!ext4fs_devread((lbaint_t)blknr * fs->sect_perblk,
				    0, fs->blksz, metadata_buff))
			goto fail;

		if (be32_to_cpu(tag->block) == 0)
			goto fail;

		if (put_ext4((uint64_t)be32_to_cpu(tag->block) *
			     (uint64_t)fs->blksz,
			     metadata_buff, fs->blksz))
			goto fail;

	} while (!(flags & EXT3_JOURNAL_FLAG_LAST_TAG));

	ret = 0;

fail:
	free(temp_buff);
	free(metadata_buff);
	return ret;
}

void print_jrnl_status(int recovery_flag)
{
	if (recovery_flag == RECOVER)
		printf("Journal Recovery Completed\n");
	else
		printf("Journal Scan Completed\n");
}

int ext4fs_check_journal_state(int recovery_flag)
{
	int i;
	int DB_FOUND = NO;
	long int blknr;
	int transaction_state = TRANSACTION_COMPLETE;
	int prev_desc_logical_no = 0;
	int curr_desc_logical_no = 0;
	int ofs, flags;
	struct ext2_inode inode_journal;
	struct journal_superblock_t *jsb = NULL;
	struct journal_header_t *jdb = NULL;
	char *p_jdb = NULL;
	struct ext3_journal_block_tag *tag = NULL;
	char *temp_buff = NULL;
	char *temp_buff1 = NULL;
	struct ext_filesystem *fs = get_fs();

	if (le32_to_cpu(fs->sb->feature_ro_compat) & EXT4_FEATURE_RO_COMPAT_METADATA_CSUM)
		return 0;

	temp_buff = zalloc(fs->blksz);
	if (!temp_buff)
		return -ENOMEM;
	temp_buff1 = zalloc(fs->blksz);
	if (!temp_buff1) {
		free(temp_buff);
		return -ENOMEM;
	}

	if (!ext4fs_read_inode(ext4fs_root, EXT2_JOURNAL_INO,
			       &inode_journal))
		goto fail;
	blknr = read_allocated_block(&inode_journal, EXT2_JOURNAL_SUPERBLOCK,
				     NULL);
	if (blknr <= 0 ||
	    !ext4fs_devread((lbaint_t)blknr * fs->sect_perblk, 0,
			    fs->blksz, temp_buff))
		goto fail;
	jsb = (struct journal_superblock_t *) temp_buff;

	if (le32_to_cpu(fs->sb->feature_incompat) & EXT3_FEATURE_INCOMPAT_RECOVER) {
		if (recovery_flag == RECOVER)
			printf("Recovery required\n");
	} else {
		if (recovery_flag == RECOVER)
			log_debug("File System is consistent\n");
		goto end;
	}

	if (be32_to_cpu(jsb->s_start) == 0)
		goto end;

	if (!(jsb->s_feature_compat &
				cpu_to_be32(JBD2_FEATURE_COMPAT_CHECKSUM)))
		jsb->s_feature_compat |=
				cpu_to_be32(JBD2_FEATURE_COMPAT_CHECKSUM);

	i = be32_to_cpu(jsb->s_first);
	while (1) {
		blknr = read_allocated_block(&inode_journal, i, NULL);
		if (blknr <= 0)
			goto fail;
		memset(temp_buff1, '\0', fs->blksz);
		if (!ext4fs_devread((lbaint_t)blknr * fs->sect_perblk,
				    0, fs->blksz, temp_buff1))
			goto fail;
		jdb = (struct journal_header_t *) temp_buff1;

		if (be32_to_cpu(jdb->h_blocktype) ==
		    EXT3_JOURNAL_DESCRIPTOR_BLOCK) {
			if (be32_to_cpu(jdb->h_sequence) !=
			    be32_to_cpu(jsb->s_sequence)) {
				print_jrnl_status(recovery_flag);
				break;
			}

			curr_desc_logical_no = i;
			if (transaction_state == TRANSACTION_COMPLETE)
				transaction_state = TRANSACTION_RUNNING;
			else
				goto fail;
			p_jdb = (char *)temp_buff1;
			ofs = sizeof(struct journal_header_t);
			do {
				if (ofs < 0 || ofs > fs->blksz ||
				    sizeof(*tag) > fs->blksz - ofs)
					goto fail;

				tag = (struct ext3_journal_block_tag *)
				    (p_jdb + ofs);
				ofs += sizeof(*tag);
				flags = be32_to_cpu(tag->flags);

				if (!(flags & EXT3_JOURNAL_FLAG_SAME_UUID)) {
					if (ofs > fs->blksz ||
					    16 > fs->blksz - ofs)
						goto fail;
					ofs += 16;
				}

				i++;
				debug("\t\ttag %u\n", be32_to_cpu(tag->block));
			} while (!(flags & EXT3_JOURNAL_FLAG_LAST_TAG));
			i++;
			DB_FOUND = YES;
		} else if (be32_to_cpu(jdb->h_blocktype) ==
				EXT3_JOURNAL_COMMIT_BLOCK) {
			if (be32_to_cpu(jdb->h_sequence) !=
			     be32_to_cpu(jsb->s_sequence)) {
				print_jrnl_status(recovery_flag);
				break;
			}

			if (transaction_state == TRANSACTION_RUNNING ||
					(DB_FOUND == NO)) {
				transaction_state = TRANSACTION_COMPLETE;
				i++;
				jsb->s_sequence =
					cpu_to_be32(be32_to_cpu(
						jsb->s_sequence) + 1);
			}
			prev_desc_logical_no = curr_desc_logical_no;
			if ((recovery_flag == RECOVER) && (DB_FOUND == YES)) {
				if (recover_transaction(prev_desc_logical_no))
					goto fail;
			}

			DB_FOUND = NO;
		} else if (be32_to_cpu(jdb->h_blocktype) ==
				EXT3_JOURNAL_REVOKE_BLOCK) {
			if (be32_to_cpu(jdb->h_sequence) !=
			    be32_to_cpu(jsb->s_sequence)) {
				print_jrnl_status(recovery_flag);
				break;
			}
			if (recovery_flag == SCAN)
				ext4fs_push_revoke_blk((char *)jdb);
			i++;
		} else {
			debug("Else Case\n");
			if (be32_to_cpu(jdb->h_sequence) !=
			    be32_to_cpu(jsb->s_sequence)) {
				print_jrnl_status(recovery_flag);
				break;
			}
		}
	}

end:
	if (recovery_flag == RECOVER) {
		uint32_t new_feature_incompat;
		jsb->s_start = cpu_to_be32(1);
		jsb->s_sequence = cpu_to_be32(be32_to_cpu(jsb->s_sequence) + 1);
		/* Read the filesystem superblock before updating it. */
		if (!ext4_read_superblock((char *)fs->sb))
			goto fail;

		new_feature_incompat = le32_to_cpu(fs->sb->feature_incompat);
		new_feature_incompat |= EXT3_FEATURE_INCOMPAT_RECOVER;
		fs->sb->feature_incompat = cpu_to_le32(new_feature_incompat);

		/* Update the filesystem superblock. */
		if (put_ext4((uint64_t)SUPERBLOCK_SIZE,
			     (struct ext2_sblock *)fs->sb,
			     (uint32_t)SUPERBLOCK_SIZE))
			goto fail;

		if (!ext4_read_superblock((char *)fs->sb))
			goto fail;

		blknr = read_allocated_block(&inode_journal,
					 EXT2_JOURNAL_SUPERBLOCK, NULL);
		if (blknr <= 0)
			goto fail;

		if (put_ext4((uint64_t)blknr * (uint64_t)fs->blksz,
			     (struct journal_superblock_t *)temp_buff,
			     (uint32_t)fs->blksz))
			goto fail;

		ext4fs_free_revoke_blks();
	}
	free(temp_buff);
	free(temp_buff1);

	return 0;

fail:
	free(temp_buff);
	free(temp_buff1);
	return -1;
}

static int update_descriptor_block(long int blknr)
{
	int i;
	long int jsb_blknr;
	struct journal_header_t jdb;
	struct ext3_journal_block_tag tag;
	struct ext2_inode inode_journal;
	struct journal_superblock_t *jsb;
	char *buf = NULL;
	char *temp;
	struct ext_filesystem *fs = get_fs();
	char *temp_buff;

	if (!fs || !fs->blksz || blknr <= 0)
		return -EINVAL;

	for (i = 0; i < MAX_JOURNAL_ENTRIES; i++) {
		if (!journal_ptr[i] || journal_ptr[i]->blknr == -1)
			break;
		if (!journal_ptr[i]->buf)
			return -EINVAL;
	}
	if (i == 0)
		return -EINVAL;

	temp_buff = zalloc(fs->blksz);
	if (!temp_buff)
		return -ENOMEM;

	if (!ext4fs_read_inode(ext4fs_root, EXT2_JOURNAL_INO,
			       &inode_journal)) {
		free(temp_buff);
		return -EIO;
	}

	jsb_blknr = read_allocated_block(&inode_journal,
					 EXT2_JOURNAL_SUPERBLOCK, NULL);
	if (jsb_blknr <= 0 ||
	    !ext4fs_devread((lbaint_t)jsb_blknr * fs->sect_perblk,
			    0, fs->blksz, temp_buff)) {
		free(temp_buff);
		return -EIO;
	}
	jsb = (struct journal_superblock_t *)temp_buff;

	buf = zalloc(fs->blksz);
	if (!buf) {
		free(temp_buff);
		return -ENOMEM;
	}

	jdb.h_blocktype = cpu_to_be32(EXT3_JOURNAL_DESCRIPTOR_BLOCK);
	jdb.h_magic = cpu_to_be32(EXT3_JOURNAL_MAGIC_NUMBER);
	jdb.h_sequence = jsb->s_sequence;

	temp = buf + sizeof(struct journal_header_t);
	memcpy(buf, &jdb, sizeof(struct journal_header_t));

	for (i = 0; i < MAX_JOURNAL_ENTRIES; i++) {
		if (!journal_ptr[i] || journal_ptr[i]->blknr == -1)
			break;

		tag.block = cpu_to_be32(journal_ptr[i]->blknr);
		tag.flags = cpu_to_be32(EXT3_JOURNAL_FLAG_SAME_UUID);
		memcpy(temp, &tag, sizeof(tag));
		temp += sizeof(tag);
	}

	tag.block = cpu_to_be32(journal_ptr[i - 1]->blknr);
	tag.flags = cpu_to_be32(EXT3_JOURNAL_FLAG_LAST_TAG);
	memcpy(temp - sizeof(tag), &tag, sizeof(tag));

	i = put_ext4((uint64_t)blknr * fs->blksz, buf, fs->blksz);

	free(buf);
	free(temp_buff);
	return i ? -EIO : 0;
}

static int update_commit_block(long int blknr)
{
	struct journal_header_t jdb;
	struct ext_filesystem *fs = get_fs();
	char *buf;
	struct ext2_inode inode_journal;
	struct journal_superblock_t *jsb;
	long int jsb_blknr;
	char *temp_buff;
	int ret;

	if (!fs || !fs->blksz || blknr <= 0)
		return -EINVAL;

	temp_buff = zalloc(fs->blksz);
	if (!temp_buff)
		return -ENOMEM;

	if (!ext4fs_read_inode(ext4fs_root, EXT2_JOURNAL_INO,
			       &inode_journal)) {
		free(temp_buff);
		return -EIO;
	}

	jsb_blknr = read_allocated_block(&inode_journal,
					 EXT2_JOURNAL_SUPERBLOCK, NULL);
	if (jsb_blknr <= 0 ||
	    !ext4fs_devread((lbaint_t)jsb_blknr * fs->sect_perblk,
			    0, fs->blksz, temp_buff)) {
		free(temp_buff);
		return -EIO;
	}
	jsb = (struct journal_superblock_t *)temp_buff;

	jdb.h_blocktype = cpu_to_be32(EXT3_JOURNAL_COMMIT_BLOCK);
	jdb.h_magic = cpu_to_be32(EXT3_JOURNAL_MAGIC_NUMBER);
	jdb.h_sequence = jsb->s_sequence;

	buf = zalloc(fs->blksz);
	if (!buf) {
		free(temp_buff);
		return -ENOMEM;
	}

	memcpy(buf, &jdb, sizeof(struct journal_header_t));
	ret = put_ext4((uint64_t)blknr * fs->blksz, buf, fs->blksz);

	free(buf);
	free(temp_buff);
	return ret ? -EIO : 0;
}

int ext4fs_update_journal(void)
{
	struct ext2_inode inode_journal;
	struct ext_filesystem *fs = get_fs();
	long int blknr;
	int i;
	int ret;

	if (!fs || !fs->sb)
		return -ENODEV;

	if (!(fs->sb->feature_compatibility &
	      EXT4_FEATURE_COMPAT_HAS_JOURNAL))
		return 0;

	if (!journal_ptr[0] || journal_ptr[0]->blknr == -1)
		return 0;

	if (!ext4fs_read_inode(ext4fs_root, EXT2_JOURNAL_INO,
			       &inode_journal))
		return -EIO;

	blknr = read_allocated_block(&inode_journal, jrnl_blk_idx, NULL);
	if (blknr <= 0)
		return -EIO;
	jrnl_blk_idx++;

	ret = update_descriptor_block(blknr);
	if (ret)
		return ret;

	for (i = 0; i < MAX_JOURNAL_ENTRIES; i++) {
		if (!journal_ptr[i] || journal_ptr[i]->blknr == -1)
			break;
		if (!journal_ptr[i]->buf)
			return -EINVAL;

		blknr = read_allocated_block(&inode_journal, jrnl_blk_idx,
					     NULL);
		if (blknr <= 0)
			return -EIO;
		jrnl_blk_idx++;

		ret = put_ext4((uint64_t)blknr * fs->blksz,
			       journal_ptr[i]->buf, fs->blksz);
		if (ret)
			return ret;
	}

	blknr = read_allocated_block(&inode_journal, jrnl_blk_idx, NULL);
	if (blknr <= 0)
		return -EIO;
	jrnl_blk_idx++;

	ret = update_commit_block(blknr);
	if (ret)
		return ret;

	printf("update journal finished\n");
	return 0;
}
