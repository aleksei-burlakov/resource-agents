/******************************************************************************
*******************************************************************************
**
**  Copyright (C) Sistina Software, Inc.  1997-2003  All rights reserved.
**  Copyright (C) 2004 Red Hat, Inc.  All rights reserved.
**
**  This copyrighted material is made available to anyone wishing to use,
**  modify, copy, or redistribute it subject to the terms and conditions
**  of the GNU General Public License v.2.
**
*******************************************************************************
******************************************************************************/

#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smp_lock.h>
#include <linux/spinlock.h>
#include <asm/semaphore.h>
#include <linux/completion.h>
#include <linux/buffer_head.h>

#include "gfs.h"
#include "dio.h"
#include "file.h"
#include "format.h"
#include "glock.h"
#include "glops.h"
#include "inode.h"
#include "log.h"
#include "quota.h"
#include "recovery.h"
#include "rgrp.h"
#include "super.h"
#include "unlinked.h"

/**
 * gfs_init_tune_data - Fill in the struct gfs_tune (sd_tune) in the struct gfs_sbd.
 * @sdp: the filesystem
 *
 */

void
gfs_init_tune_data(struct gfs_sbd *sdp)
{
	struct gfs_tune *gt = &sdp->sd_tune;

	gt->gt_tune_version = GFS_TUNE_VERSION;

	gt->gt_ilimit1 = 100;
	gt->gt_ilimit1_tries = 3;
	gt->gt_ilimit1_min = 1;
	gt->gt_ilimit2 = 500;
	gt->gt_ilimit2_tries = 10;
	gt->gt_ilimit2_min = 3;
	gt->gt_demote_secs = 300;
	gt->gt_incore_log_blocks = 1024;
	gt->gt_jindex_refresh_secs = 60;
	gt->gt_depend_secs = 60;
	gt->gt_scand_secs = 5;
	gt->gt_recoverd_secs = 60;
	gt->gt_logd_secs = 1;
	gt->gt_quotad_secs = 5;
	gt->gt_inoded_secs = 15;
	gt->gt_quota_simul_sync = 64;
	gt->gt_quota_warn_period = 10;
	gt->gt_atime_quantum = 3600;
	gt->gt_quota_quantum = 60;
	gt->gt_quota_scale_num = 1;
	gt->gt_quota_scale_den = 1;
	gt->gt_quota_enforce = 1;
	gt->gt_quota_account = 1;
	gt->gt_new_files_jdata = 0;
	gt->gt_new_files_directio = 0;
	gt->gt_max_atomic_write = 4 << 20;
	gt->gt_max_readahead = 1 << 18;
	gt->gt_lockdump_size = 131072;
	gt->gt_stall_secs = 600;
	gt->gt_complain_secs = 10;
	gt->gt_reclaim_limit = 5000;
	gt->gt_entries_per_readdir = 32;
	gt->gt_prefetch_secs = 10;
	gt->gt_statfs_slots = 64;
	gt->gt_max_mhc = 10000;
	gt->gt_greedy_default = HZ / 10;
	gt->gt_greedy_quantum = HZ / 40;
	gt->gt_greedy_max = HZ / 4;
}

/**
 * gfs_check_sb - Check superblock
 * @sdp: the filesystem
 * @sb: The superblock
 * @silent: Don't print a message if the check fails
 *
 * Checks the version code of the FS is one that we understand how to
 * read and that the sizes of the various on-disk structures have not
 * changed.
 */

int
gfs_check_sb(struct gfs_sbd *sdp, struct gfs_sb *sb, int silent)
{
	unsigned int x;

	if (sb->sb_header.mh_magic != GFS_MAGIC ||
	    sb->sb_header.mh_type != GFS_METATYPE_SB) {
		if (!silent)
			printk("GFS: not a GFS filesystem\n");
		return -EINVAL;
	}

	/*  If format numbers match exactly, we're done.  */

	if (sb->sb_fs_format == GFS_FORMAT_FS &&
	    sb->sb_multihost_format == GFS_FORMAT_MULTI)
		return 0;

	if (sb->sb_fs_format != GFS_FORMAT_FS) {
		for (x = 0; gfs_old_fs_formats[x]; x++)
			if (gfs_old_fs_formats[x] == sb->sb_fs_format)
				break;

		if (!gfs_old_fs_formats[x]) {
			printk("GFS: code version (%u, %u) is incompatible with ondisk format (%u, %u)\n",
			       GFS_FORMAT_FS, GFS_FORMAT_MULTI,
			       sb->sb_fs_format, sb->sb_multihost_format);
			printk("GFS: I don't know how to upgrade this FS\n");
			return -EINVAL;
		}
	}

	if (sb->sb_multihost_format != GFS_FORMAT_MULTI) {
		for (x = 0; gfs_old_multihost_formats[x]; x++)
			if (gfs_old_multihost_formats[x] == sb->sb_multihost_format)
				break;

		if (!gfs_old_multihost_formats[x]) {
			printk("GFS: code version (%u, %u) is incompatible with ondisk format (%u, %u)\n",
			     GFS_FORMAT_FS, GFS_FORMAT_MULTI,
			       sb->sb_fs_format, sb->sb_multihost_format);
			printk("GFS: I don't know how to upgrade this FS\n");
			return -EINVAL;
		}
	}

	if (!sdp->sd_args.ar_upgrade) {
		printk("GFS: code version (%u, %u) is incompatible with ondisk format (%u, %u)\n",
		       GFS_FORMAT_FS, GFS_FORMAT_MULTI,
		       sb->sb_fs_format, sb->sb_multihost_format);
		printk("GFS: Use the \"upgrade\" mount option to upgrade the FS\n");
		printk("GFS: See the manual for more details\n");
		return -EINVAL;
	}

	return 0;
}

/**
 * gfs_read_sb - Read super block
 * @sdp: The GFS superblock
 * @gl: the glock for the superblock (assumed to be held)
 * @silent: Don't print message if mount fails
 *
 */

int
gfs_read_sb(struct gfs_sbd *sdp, struct gfs_glock *gl, int silent)
{
	struct buffer_head *bh;
	uint32_t hash_blocks, ind_blocks, leaf_blocks;
	uint32_t tmp_blocks;
	unsigned int x;
	int error;

	error = gfs_dread(sdp, GFS_SB_ADDR >> sdp->sd_fsb2bb_shift,
			  gl, DIO_FORCE | DIO_START | DIO_WAIT, &bh);
	if (error) {
		if (!silent)
			printk("GFS: fsid=%s: can't read superblock\n",
			       sdp->sd_fsname);
		return error;
	}

	GFS_ASSERT_SBD(sizeof(struct gfs_sb) <= bh->b_size, sdp,);

	gfs_sb_in(&sdp->sd_sb, bh->b_data);

	brelse(bh);

	error = gfs_check_sb(sdp, &sdp->sd_sb, silent);
	if (error)
		return error;

	sdp->sd_fsb2bb_shift = sdp->sd_sb.sb_bsize_shift -
		GFS_BASIC_BLOCK_SHIFT;
	sdp->sd_fsb2bb = 1 << sdp->sd_fsb2bb_shift;
	sdp->sd_diptrs = (sdp->sd_sb.sb_bsize - sizeof(struct gfs_dinode)) /
		sizeof(uint64_t);
	sdp->sd_inptrs = (sdp->sd_sb.sb_bsize - sizeof(struct gfs_indirect)) /
		sizeof(uint64_t);
	sdp->sd_jbsize = sdp->sd_sb.sb_bsize - sizeof(struct gfs_meta_header);
	sdp->sd_hash_bsize = sdp->sd_sb.sb_bsize / 2;
	sdp->sd_hash_bsize_shift = sdp->sd_sb.sb_bsize_shift - 1;
	sdp->sd_hash_ptrs = sdp->sd_hash_bsize / sizeof(uint64_t);

	/*  Compute maximum reservation required to add a entry to a directory  */

	hash_blocks = DIV_RU(sizeof(uint64_t) * (1 << GFS_DIR_MAX_DEPTH),
			     sdp->sd_jbsize);

	ind_blocks = 0;
	for (tmp_blocks = hash_blocks; tmp_blocks > sdp->sd_diptrs;) {
		tmp_blocks = DIV_RU(tmp_blocks, sdp->sd_inptrs);
		ind_blocks += tmp_blocks;
	}

	leaf_blocks = 2 + GFS_DIR_MAX_DEPTH;

	sdp->sd_max_dirres = hash_blocks + ind_blocks + leaf_blocks;

	sdp->sd_heightsize[0] = sdp->sd_sb.sb_bsize - sizeof(struct gfs_dinode);
	sdp->sd_heightsize[1] = sdp->sd_sb.sb_bsize * sdp->sd_diptrs;
	for (x = 2;; x++) {
		uint64_t space, d;
		uint32_t m;

		space = sdp->sd_heightsize[x - 1] * sdp->sd_inptrs;
		d = space;
		m = do_div(d, sdp->sd_inptrs);

		if (d != sdp->sd_heightsize[x - 1] || m)
			break;
		sdp->sd_heightsize[x] = space;
	}
	sdp->sd_max_height = x;
	GFS_ASSERT_SBD(sdp->sd_max_height <= GFS_MAX_META_HEIGHT, sdp,);

	sdp->sd_jheightsize[0] = sdp->sd_sb.sb_bsize - sizeof(struct gfs_dinode);
	sdp->sd_jheightsize[1] = sdp->sd_jbsize * sdp->sd_diptrs;
	for (x = 2;; x++) {
		uint64_t space, d;
		uint32_t m;

		space = sdp->sd_jheightsize[x - 1] * sdp->sd_inptrs;
		d = space;
		m = do_div(d, sdp->sd_inptrs);

		if (d != sdp->sd_jheightsize[x - 1] || m)
			break;
		sdp->sd_jheightsize[x] = space;
	}
	sdp->sd_max_jheight = x;
	GFS_ASSERT_SBD(sdp->sd_max_jheight <= GFS_MAX_META_HEIGHT, sdp,);

	return 0;
}

/**
 * gfs_do_upgrade - upgrade a filesystem
 * @sdp: The GFS superblock
 *
 */

int
gfs_do_upgrade(struct gfs_sbd *sdp, struct gfs_glock *sb_gl)
{
	struct gfs_holder ji_gh, t_gh, j_gh;
	struct gfs_log_header lh;
	struct buffer_head *bh;
	unsigned int x;
	int error;

	/*  If format numbers match exactly, we're done.  */

	if (sdp->sd_sb.sb_fs_format == GFS_FORMAT_FS &&
	    sdp->sd_sb.sb_multihost_format == GFS_FORMAT_MULTI) {
		printk("GFS: fsid=%s: no upgrade necessary\n",
		       sdp->sd_fsname);
		sdp->sd_args.ar_upgrade = FALSE;
		return 0;
	}

	error = gfs_jindex_hold(sdp, &ji_gh);
	if (error)
		goto fail;

	error = gfs_glock_nq_init(sdp->sd_trans_gl,
				  LM_ST_EXCLUSIVE, GL_NOCACHE,
				  &t_gh);
	if (error)
		goto fail_ji_relse;

	if (test_bit(SDF_ROFS, &sdp->sd_flags)) {
		printk("GFS: fsid=%s: can't upgrade: read-only FS\n",
		       sdp->sd_fsname);
		error = -EROFS;
		goto fail_gunlock_tr;
	}

	for (x = 0; x < sdp->sd_journals; x++) {
		error = gfs_glock_nq_num(sdp,
					 sdp->sd_jindex[x].ji_addr,
					 &gfs_meta_glops, LM_ST_SHARED,
					 LM_FLAG_TRY | GL_NOCACHE, &j_gh);
		switch (error) {
		case 0:
			break;

		case GLR_TRYFAILED:
			printk("GFS: fsid=%s: journal %u is busy\n",
			       sdp->sd_fsname, x);
			error = -EBUSY;

		default:
			goto fail_gunlock_tr;
		}

		error = gfs_find_jhead(sdp, &sdp->sd_jindex[x],
				       j_gh.gh_gl, &lh);

		gfs_glock_dq_uninit(&j_gh);

		if (error)
			goto fail_gunlock_tr;

		if (!(lh.lh_flags & GFS_LOG_HEAD_UNMOUNT) || lh.lh_last_dump) {
			printk("GFS: fsid=%s: journal %u is busy\n",
			       sdp->sd_fsname, x);
			error = -EBUSY;
			goto fail_gunlock_tr;
		}
	}

	/* We don't need to journal this change because we're changing
	   only one sector of one block.  We definitely don't want to have
	   the journaling code running at this point. */

	error = gfs_dread(sdp, GFS_SB_ADDR >> sdp->sd_fsb2bb_shift, sb_gl,
			  DIO_START | DIO_WAIT, &bh);
	if (error)
		goto fail_gunlock_tr;

	gfs_sb_in(&sdp->sd_sb, bh->b_data);

	error = gfs_check_sb(sdp, &sdp->sd_sb, FALSE);
	GFS_ASSERT_SBD(!error, sdp,);

	sdp->sd_sb.sb_fs_format = GFS_FORMAT_FS;
	sdp->sd_sb.sb_multihost_format = GFS_FORMAT_MULTI;

	gfs_sb_out(&sdp->sd_sb, bh->b_data);

	set_bit(GLF_DIRTY, &sb_gl->gl_flags);
	error = gfs_dwrite(sdp, bh, DIO_DIRTY | DIO_START | DIO_WAIT);

	brelse(bh);

	gfs_glock_dq_uninit(&t_gh);

	gfs_glock_dq_uninit(&ji_gh);

	if (!error) {
		printk("GFS: fsid=%s: upgrade successful\n",
		       sdp->sd_fsname);
		sdp->sd_args.ar_upgrade = FALSE;
	}

	return error;

 fail_gunlock_tr:
	gfs_glock_dq_uninit(&t_gh);

 fail_ji_relse:
	gfs_glock_dq_uninit(&ji_gh);

 fail:
	if (error == -EBUSY)
		printk("GFS: fsid=%s: can't upgrade: the FS is still busy or contains dirty journals\n",
		       sdp->sd_fsname);
	else
		printk("GFS: fsid=%s: can't upgrade: %d\n",
		       sdp->sd_fsname, error);

	return error;
}

/**
 * clear_journalsi - Clear all the journal index information (without locking)
 * @sdp: The GFS superblock
 *
 */

static void
clear_journalsi(struct gfs_sbd *sdp)
{
	if (sdp->sd_jindex) {
		kfree(sdp->sd_jindex);
		sdp->sd_jindex = NULL;
	}
	sdp->sd_journals = 0;
}

/**
 * gfs_clear_journals - Clear all the journal index information
 * @sdp: The GFS superblock
 *
 */

void
gfs_clear_journals(struct gfs_sbd *sdp)
{
	down(&sdp->sd_jindex_lock);
	clear_journalsi(sdp);
	up(&sdp->sd_jindex_lock);
}

/**
 * gfs_ji_update - Update the journal index information
 * @ip: The journal index inode
 *
 * Returns: 0 on success, error code otherwise
 */

static int
gfs_ji_update(struct gfs_inode *ip)
{
	struct gfs_sbd *sdp = ip->i_sbd;
	char buf[sizeof(struct gfs_jindex)];
	unsigned int j;
	int error;

	GFS_ASSERT_SBD(!do_mod(ip->i_di.di_size, sizeof(struct gfs_jindex)),
		       sdp,);

	clear_journalsi(sdp);

	sdp->sd_jindex = gmalloc(ip->i_di.di_size);
	memset(sdp->sd_jindex, 0, ip->i_di.di_size);

	for (j = 0;; j++) {
		error = gfs_internal_read(ip, buf,
					  j * sizeof(struct gfs_jindex),
					  sizeof(struct gfs_jindex));
		if (!error)
			break;
		if (error != sizeof(struct gfs_jindex)) {
			if (error > 0)
				error = -EIO;
			goto fail;
		}

		gfs_jindex_in(sdp->sd_jindex + j, buf);
	}

	GFS_ASSERT_SBD(j * sizeof(struct gfs_jindex) == ip->i_di.di_size,
		       sdp,);

	sdp->sd_journals = j;
	sdp->sd_jiinode_vn = ip->i_gl->gl_vn;

	return 0;

 fail:
	clear_journalsi(sdp);
	return error;
}

/**
 * gfs_jindex_hold - Grab a lock on the jindex
 * @sdp: The GFS superblock
 * @ji_gh: the holder for the jindex glock
 *
 * This makes sure that we're using the latest copy of the journal index
 *   special file, which might have been updated if someone added journals
 *   (via gfs_jadd utility).
 *
 * This is very similar to the gfs_rindex_hold() function, except that
 * in general we hold the jindex lock for longer periods of time and
 * we grab it far less frequently (in general) then the rgrp lock.
 *
 * Returns: 0 on success, error code otherwise
 */

int
gfs_jindex_hold(struct gfs_sbd *sdp, struct gfs_holder *ji_gh)
{
	struct gfs_inode *ip = sdp->sd_jiinode;
	struct gfs_glock *gl = ip->i_gl;
	int error;

	error = gfs_glock_nq_init(gl, LM_ST_SHARED, 0, ji_gh);
	if (error)
		return error;

	/* Read new copy from disk if we don't have the latest */
	if (sdp->sd_jiinode_vn != gl->gl_vn) {
		down(&sdp->sd_jindex_lock);
		if (sdp->sd_jiinode_vn != gl->gl_vn)
			error = gfs_ji_update(ip);
		up(&sdp->sd_jindex_lock);
	}

	if (error)
		gfs_glock_dq_uninit(ji_gh);

	return error;
}

/**
 * gfs_get_jiinode - Read in the jindex inode for the superblock
 * @sdp: The GFS superblock
 *
 * Returns: 0 on success, error code otherwise
 */

int
gfs_get_jiinode(struct gfs_sbd *sdp)
{
	struct gfs_holder ji_gh;
	int error;

	error = gfs_glock_nq_num(sdp,
				 sdp->sd_sb.sb_jindex_di.no_formal_ino,
				 &gfs_inode_glops,
				 LM_ST_SHARED, GL_LOCAL_EXCL,
				 &ji_gh);
	if (error)
		return error;

	error = gfs_inode_get(ji_gh.gh_gl, &sdp->sd_sb.sb_jindex_di,
			      CREATE, &sdp->sd_jiinode);
	if (!error) {
		sdp->sd_jiinode_vn = ji_gh.gh_gl->gl_vn - 1;
		set_bit(GLF_STICKY, &ji_gh.gh_gl->gl_flags);
	}

	gfs_glock_dq_uninit(&ji_gh);

	return error;
}

/**
 * gfs_get_riinode - Read in the rindex inode for the superblock
 * @sdp: The GFS superblock
 *
 * Returns: 0 on success, error code otherwise
 */

int
gfs_get_riinode(struct gfs_sbd *sdp)
{
	struct gfs_holder ri_gh;
	int error;

	error = gfs_glock_nq_num(sdp,
				 sdp->sd_sb.sb_rindex_di.no_formal_ino,
				 &gfs_inode_glops,
				 LM_ST_SHARED, GL_LOCAL_EXCL,
				 &ri_gh);
	if (error)
		return error;

	error = gfs_inode_get(ri_gh.gh_gl, &sdp->sd_sb.sb_rindex_di,
			      CREATE, &sdp->sd_riinode);
	if (!error) {
		sdp->sd_riinode_vn = ri_gh.gh_gl->gl_vn - 1;
		set_bit(GLF_STICKY, &ri_gh.gh_gl->gl_flags);
	}

	gfs_glock_dq_uninit(&ri_gh);

	return error;
}

/**
 * gfs_get_rootinode - Read in the root inode
 * @sdp: The GFS superblock
 *
 * Returns: 0 on success, error code otherwise
 */

int
gfs_get_rootinode(struct gfs_sbd *sdp)
{
	struct gfs_holder i_gh;
	int error;

	error = gfs_glock_nq_num(sdp,
				 sdp->sd_sb.sb_root_di.no_formal_ino,
				 &gfs_inode_glops,
				 LM_ST_SHARED, GL_LOCAL_EXCL,
				 &i_gh);
	if (error)
		return error;

	error = gfs_inode_get(i_gh.gh_gl, &sdp->sd_sb.sb_root_di,
			      CREATE, &sdp->sd_rooti);

	gfs_glock_dq_uninit(&i_gh);

	return error;
}

/**
 * gfs_get_qinode - Read in the special (hidden) quota inode
 * @sdp: The GFS superblock
 *
 * If one is not on-disk already, create a new one.
 * Does not read in file contents, just the dinode.
 *
 * Returns: 0 on success, error code otherwise
 */

int
gfs_get_qinode(struct gfs_sbd *sdp)
{
	struct gfs_holder i_gh;
	int error;

	/* Create, if not on-disk already */
	if (!sdp->sd_sb.sb_quota_di.no_formal_ino) {
		error = gfs_alloc_qinode(sdp);
		if (error)
			return error;
	}

	error = gfs_glock_nq_num(sdp,
				 sdp->sd_sb.sb_quota_di.no_formal_ino,
				 &gfs_inode_glops,
				 LM_ST_SHARED, GL_LOCAL_EXCL,
				 &i_gh);
	if (error)
		return error;

	error = gfs_inode_get(i_gh.gh_gl, &sdp->sd_sb.sb_quota_di,
			      CREATE, &sdp->sd_qinode);

	gfs_glock_dq_uninit(&i_gh);

	return error;
}

/**
 * gfs_get_linode - Read in the special (hidden) license inode
 * @sdp: The GFS superblock
 *
 * If one is not on-disk already, create a new one.
 * Does not read in file contents, just the dinode.
 *
 * Returns: 0 on success, error code otherwise
 */

int
gfs_get_linode(struct gfs_sbd *sdp)
{
	struct gfs_holder i_gh;
	int error;

	/* Create, if not on-disk already */
	if (!sdp->sd_sb.sb_license_di.no_formal_ino) {
		error = gfs_alloc_linode(sdp);
		if (error)
			return error;
	}

	error = gfs_glock_nq_num(sdp,
				 sdp->sd_sb.sb_license_di.no_formal_ino,
				 &gfs_inode_glops,
				 LM_ST_SHARED, GL_LOCAL_EXCL,
				 &i_gh);
	if (error)
		return error;

	error = gfs_inode_get(i_gh.gh_gl, &sdp->sd_sb.sb_license_di,
			      CREATE, &sdp->sd_linode);

	gfs_glock_dq_uninit(&i_gh);

	return error;
}

/**
 * gfs_make_fs_rw - Turn a Read-Only FS into a Read-Write one
 * @sdp: the filesystem
 *
 * Returns: errno
 */

int
gfs_make_fs_rw(struct gfs_sbd *sdp)
{
	struct gfs_glock *j_gl = sdp->sd_journal_gh.gh_gl;
	struct gfs_holder t_gh;
	struct gfs_log_header head;
	int error;

	error = gfs_glock_nq_init(sdp->sd_trans_gl,
				  LM_ST_SHARED,
				  GL_LOCAL_EXCL | GL_EXACT,
				  &t_gh);
	if (error)
		return error;

	j_gl->gl_ops->go_inval(j_gl, DIO_METADATA | DIO_DATA);

	error = gfs_find_jhead(sdp, &sdp->sd_jdesc, j_gl, &head);
	if (error)
		goto fail;

	GFS_ASSERT_SBD(head.lh_flags & GFS_LOG_HEAD_UNMOUNT, sdp,);

	/*  Initialize some head of the log stuff  */
	sdp->sd_sequence = head.lh_sequence;
	sdp->sd_log_head = head.lh_first + 1;

	error = gfs_recover_dump(sdp);
	if (error)
		goto fail;

	set_bit(SDF_JOURNAL_LIVE, &sdp->sd_flags);
	clear_bit(SDF_ROFS, &sdp->sd_flags);

	set_bit(GLF_DIRTY, &j_gl->gl_flags);
	gfs_log_dump(sdp, TRUE);

	gfs_glock_dq_uninit(&t_gh);

	return 0;

 fail:
	t_gh.gh_flags |= GL_NOCACHE;
	gfs_glock_dq_uninit(&t_gh);

	return error;
}

/**
 * gfs_make_fs_ro - Turn a Read-Write FS into a Read-Only one
 * @sdp: the filesystem
 *
 * Returns: errno
 */

int
gfs_make_fs_ro(struct gfs_sbd *sdp)
{
	struct gfs_holder t_gh;
	int error;

	error = gfs_glock_nq_init(sdp->sd_trans_gl,
				  LM_ST_SHARED,
				  GL_LOCAL_EXCL | GL_EXACT | GL_NOCACHE,
				  &t_gh);
	if (error)
		return error;

	gfs_sync_meta(sdp);
	gfs_log_dump(sdp, TRUE);

	error = gfs_log_shutdown(sdp);
	if (error)
		gfs_io_error(sdp);

	set_bit(SDF_ROFS, &sdp->sd_flags);
	clear_bit(SDF_JOURNAL_LIVE, &sdp->sd_flags);

	gfs_glock_dq_uninit(&t_gh);

	gfs_unlinked_cleanup(sdp);
	gfs_quota_cleanup(sdp);

	return error;
}

/**
 * stat_gfs_async - Stat a filesystem using asynchronous locking
 * @sdp: the filesystem
 * @usage: the usage info that will be returned
 * @interruptible: TRUE if we should look for signals.
 *
 * Any error (other than a signal) will cause this routine to fall back
 * to the synchronous version.
 *
 * FIXME: This really shouldn't busy wait like this.
 *
 * Returns: errno
 */

static int
stat_gfs_async(struct gfs_sbd *sdp, struct gfs_usage *usage, int interruptible)
{
	struct gfs_rgrpd *rgd_next = gfs_rgrpd_get_first(sdp), *rgd;
	struct gfs_holder *gha, *gh;
	struct gfs_rgrp_lvb *rb;
	unsigned int slots = sdp->sd_tune.gt_statfs_slots;
	unsigned int x;
	int done;
	int error = 0, err;

	gha = gmalloc(slots * sizeof(struct gfs_holder));
	memset(gha, 0, slots * sizeof(struct gfs_holder));

	for (;;) {
		done = TRUE;

		for (x = 0; x < slots; x++) {
			gh = gha + x;

			if (gh->gh_gl && gfs_glock_poll(gh)) {
				err = gfs_glock_wait(gh);
				if (err) {
					gfs_holder_uninit(gh);
					error = err;
				} else {
					rgd = gl2rgd(gh->gh_gl);

					rb = (struct gfs_rgrp_lvb *)rgd->rd_gl->gl_lvb;
					if (gfs32_to_cpu(rb->rb_magic) == GFS_MAGIC &&
					    !test_bit(GLF_LVB_INVALID, &rgd->rd_gl->gl_flags)) {
						usage->gu_total_blocks += rgd->rd_ri.ri_data;
						usage->gu_free += gfs32_to_cpu(rb->rb_free);
						usage->gu_used_dinode += gfs32_to_cpu(rb->rb_useddi);
						usage->gu_free_dinode += gfs32_to_cpu(rb->rb_freedi);
						usage->gu_used_meta += gfs32_to_cpu(rb->rb_usedmeta);
						usage->gu_free_meta += gfs32_to_cpu(rb->rb_freemeta);
					} else
						error = -EINVAL;

					gfs_glock_dq_uninit(gh);
				}
			}

			if (gh->gh_gl)
				done = FALSE;
			else if (rgd_next && !error) {
				gfs_glock_nq_init(rgd_next->rd_gl,
						  LM_ST_SHARED,
						  GL_LOCAL_EXCL | GL_SKIP | GL_ASYNC,
						  gh);
				rgd_next = gfs_rgrpd_get_next(rgd_next);
				done = FALSE;
			}

			if (interruptible && signal_pending(current))
				error = -ERESTARTSYS;
		}

		if (done)
			break;

		yield();
	}

	kfree(gha);

	return error;
}

/**
 * gfs_stat_gfs - Do a statfs
 * @sdp: the filesystem
 * @usage: the usage structure
 * @interruptible:  Stop if there is a signal pending
 *
 * Returns: errno
 */

int
gfs_stat_gfs(struct gfs_sbd *sdp, struct gfs_usage *usage, int interruptible)
{
	struct gfs_holder ri_gh, rgd_gh;
	struct gfs_rgrpd *rgd;
	struct gfs_rgrp_lvb *rb;
	int error;

	memset(usage, 0, sizeof(struct gfs_usage));
	usage->gu_block_size = sdp->sd_sb.sb_bsize;

	error = gfs_rindex_hold(sdp, &ri_gh);
	if (error)
		return error;

	error = stat_gfs_async(sdp, usage, interruptible);
	if (!error || error == -ERESTARTSYS)
		goto out;

	memset(usage, 0, sizeof(struct gfs_usage));
	usage->gu_block_size = sdp->sd_sb.sb_bsize;

	for (rgd = gfs_rgrpd_get_first(sdp);
	     rgd;
	     rgd = gfs_rgrpd_get_next(rgd)) {
		for (;;) {
			error = gfs_glock_nq_init(rgd->rd_gl,
						  LM_ST_SHARED,
						  GL_LOCAL_EXCL | GL_SKIP,
						  &rgd_gh);
			if (error)
				goto out;

			rb = (struct gfs_rgrp_lvb *)rgd->rd_gl->gl_lvb;
			if (gfs32_to_cpu(rb->rb_magic) == GFS_MAGIC &&
			    !test_bit(GLF_LVB_INVALID, &rgd->rd_gl->gl_flags)) {
				usage->gu_total_blocks += rgd->rd_ri.ri_data;
				usage->gu_free += gfs32_to_cpu(rb->rb_free);
				usage->gu_used_dinode += gfs32_to_cpu(rb->rb_useddi);
				usage->gu_free_dinode += gfs32_to_cpu(rb->rb_freedi);
				usage->gu_used_meta += gfs32_to_cpu(rb->rb_usedmeta);
				usage->gu_free_meta += gfs32_to_cpu(rb->rb_freemeta);

				gfs_glock_dq_uninit(&rgd_gh);

				break;
			} else {
				gfs_glock_dq_uninit(&rgd_gh);

				error = gfs_rgrp_lvb_init(rgd);
				if (error)
					goto out;
			}
		}

		if (interruptible && signal_pending(current)) {
			error = -ERESTARTSYS;
			goto out;
		}
	}

 out:
	gfs_glock_dq_uninit(&ri_gh);

	return error;
}

/**
 * gfs_lock_fs_check_clean - Stop all writes to the FS and check that all journals are clean
 * @sdp: the file system
 * @state: the state to put the transaction lock into
 * @t_gh: the hold on the transaction lock
 *
 * Returns: errno
 */

int
gfs_lock_fs_check_clean(struct gfs_sbd *sdp, unsigned int state,
			struct gfs_holder *t_gh)
{
	struct gfs_holder ji_gh, cl_gh;
	struct gfs_log_header lh;
	unsigned int x;
	int error;

	error = gfs_jindex_hold(sdp, &ji_gh);
	if (error)
		return error;

	error = gfs_glock_nq_num(sdp,
				 GFS_CRAP_LOCK, &gfs_meta_glops,
				 LM_ST_SHARED, GL_NOCACHE,
				 &cl_gh);
	if (error)
		goto fail;

	error = gfs_glock_nq_init(sdp->sd_trans_gl, state,
				  LM_FLAG_PRIORITY | GL_EXACT | GL_NOCACHE,
				  t_gh);
	if (error)
		goto fail_gunlock_craplock;

	for (x = 0; x < sdp->sd_journals; x++) {
		error = gfs_find_jhead(sdp, &sdp->sd_jindex[x],
				       cl_gh.gh_gl, &lh);
		if (error)
			goto fail_gunlock_trans;

		if (!(lh.lh_flags & GFS_LOG_HEAD_UNMOUNT)) {
			error = -EBUSY;
			goto fail_gunlock_trans;
		}
	}

	gfs_glock_dq_uninit(&cl_gh);
	gfs_glock_dq_uninit(&ji_gh);

	return 0;

 fail_gunlock_trans:
	gfs_glock_dq_uninit(t_gh);

 fail_gunlock_craplock:
	gfs_glock_dq_uninit(&cl_gh);

 fail:
	gfs_glock_dq_uninit(&ji_gh);

	return error;
}

/**
 * gfs_freeze_fs - freezes the file system
 * @sdp: the file system
 *
 * This function flushes data and meta data for all machines by
 * aquiring the transaction log exclusively.  All journals are
 * ensured to be in a clean state as well.
 *
 * Returns: errno
 */

int
gfs_freeze_fs(struct gfs_sbd *sdp)
{
	int error = 0;

	down(&sdp->sd_freeze_lock);

	if (!sdp->sd_freeze_count++) {
		error = gfs_lock_fs_check_clean(sdp, LM_ST_DEFERRED,
						&sdp->sd_freeze_gh);
		if (error)
			sdp->sd_freeze_count--;
		else
			sdp->sd_freeze_gh.gh_owner = NULL;
	}

	up(&sdp->sd_freeze_lock);

	return error;
}

/**
 * gfs_unfreeze_fs - unfreezes the file system
 * @sdp: the file system
 *
 * This function allows the file system to proceed by unlocking
 * the exclusively held transaction lock.  Other GFS nodes are
 * now free to acquire the lock shared and go on with their lives.
 *
 */

void
gfs_unfreeze_fs(struct gfs_sbd *sdp)
{
	down(&sdp->sd_freeze_lock);

	if (sdp->sd_freeze_count && !--sdp->sd_freeze_count)
		gfs_glock_dq_uninit(&sdp->sd_freeze_gh);

	up(&sdp->sd_freeze_lock);
}
