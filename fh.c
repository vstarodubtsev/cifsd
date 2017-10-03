/*
 *   fs/cifsd/fh.c
 *
 *   Copyright (C) 2015 Samsung Electronics Co., Ltd.
 *   Copyright (C) 2016 Namjae Jeon <namjae.jeon@protocolfreedom.org>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#include "glob.h"
#include "export.h"
#include "smb1pdu.h"
#include "oplock.h"

#include <linux/bootmem.h>
#include <linux/xattr.h>

void fp_get(struct cifsd_file *fp)
{
	atomic_inc(&fp->f_count);
}

void fp_put(struct cifsd_file *fp)
{
	if (!fp)
		return;

	if (atomic_dec_and_test(&fp->f_count))
		wake_up(&fp->wq);
}

/**
 * alloc_fid_mem() - alloc memory for fid management
 * @size:	mem allocation request size
 *
 * Return:      ptr to allocated memory or NULL
 */
static void *alloc_fid_mem(size_t size)
{
	if (size <= (PAGE_SIZE << PAGE_ALLOC_COSTLY_ORDER)) {
		return kzalloc(size, GFP_KERNEL|
				__GFP_NOWARN|__GFP_NORETRY);
	}
	return vzalloc(size);
}

/**
 * free_fid_mem() - free memory allocated for fid management
 * @ptr:	ptr to memory to be freed
 */
static void free_fid_mem(void *ptr)
{
	is_vmalloc_addr(ptr) ? vfree(ptr) : kfree(ptr);
}

/**
 * free_fidtable() - free memory allocated for fid table
 * @ftab:	fid table ptr to be freed
 */
void free_fidtable(struct fidtable *ftab)
{
	free_fid_mem(ftab->fileid);
	free_fid_mem(ftab->cifsd_bitmap);
	kfree(ftab);
}

/**
 * alloc_fidtable() - alloc fid table
 * @num:	alloc number of entries in fid table
 *
 * Return:      ptr to allocated fid table or NULL
 */
static struct fidtable *alloc_fidtable(unsigned int num)
{
	struct fidtable *ftab;
	void *tmp;

	ftab = kmalloc(sizeof(struct fidtable), GFP_KERNEL);
	if (!ftab) {
		cifsd_err("kmalloc for fidtable failed!\n");
		goto out;
	}

	ftab->max_fids = num;

	tmp = alloc_fid_mem(num * sizeof(void *));
	if (!tmp) {
		cifsd_err("alloc_fid_mem failed!\n");
		goto out_ftab;
	}

	ftab->fileid = tmp;

	tmp = alloc_fid_mem(num / BITS_PER_BYTE);
	if (!tmp) {
		cifsd_err("alloc_fid_mem failed!\n");
		goto out_fileid;
	}

	ftab->cifsd_bitmap = tmp;

	return ftab;

out_fileid:
	free_fid_mem(ftab->fileid);
out_ftab:
	kfree(ftab);
out:
	return NULL;
}

/**
 * copy_fidtable() - copy a fid table
 * @nftab:	destination fid table ptr
 * @oftab:	src fid table ptr
 */
static void copy_fidtable(struct fidtable *nftab, struct fidtable *oftab)
{
	unsigned int cpy, set;

	BUG_ON(nftab->max_fids < oftab->max_fids);

	cpy = oftab->max_fids * sizeof(void *);
	set = (nftab->max_fids - oftab->max_fids) *
		sizeof(void *);
	memcpy(nftab->fileid, oftab->fileid, cpy);
	memset((char *)(nftab->fileid) + cpy, 0, set);

	cpy = oftab->max_fids / BITS_PER_BYTE;
	set = (nftab->max_fids - oftab->max_fids) / BITS_PER_BYTE;
	memcpy(nftab->cifsd_bitmap, oftab->cifsd_bitmap, cpy);
	memset((char *)(nftab->cifsd_bitmap) + cpy, 0, set);
}

/**
 * grow_fidtable() - grow fid table
 * @num:	requested number of entries in fid table
 *
 * Return:      1 if success, otherwise error number
 */
static int grow_fidtable(struct fidtable_desc *ftab_desc, int num)
{
	struct fidtable *new_ftab, *cur_ftab;
	int old_num = num;

	num /= (1024 / sizeof(struct cifsd_file *));
	num = roundup_pow_of_two(num + 1);
	num *= (1024 / sizeof(struct cifsd_file *));

	if (num >= CIFSD_BITMAP_SIZE + 1)
		return -EMFILE;

	new_ftab = alloc_fidtable(num);
	spin_lock(&ftab_desc->fidtable_lock);
	if (!new_ftab) {
		spin_unlock(&ftab_desc->fidtable_lock);
		return -ENOMEM;
	}

	if (unlikely(new_ftab->max_fids <= old_num)) {
		spin_unlock(&ftab_desc->fidtable_lock);
		free_fidtable(new_ftab);
		return -EMFILE;
	}

	cur_ftab = ftab_desc->ftab;
	if (num >= cur_ftab->max_fids) {
		copy_fidtable(new_ftab, cur_ftab);
		ftab_desc->ftab = new_ftab;
		ftab_desc->ftab->start_pos = cur_ftab->start_pos;
		free_fidtable(cur_ftab);
	} else {
		free_fidtable(new_ftab);
	}

	spin_unlock(&ftab_desc->fidtable_lock);
	return 1;
}

/**
 * cifsd_get_unused_id() - get unused fid entry
 * @ftab_desc:	fid table from where fid should be allocated
 *
 * Return:      id if success, otherwise error number
 */
int cifsd_get_unused_id(struct fidtable_desc *ftab_desc)
{
	void *bitmap;
	int id = -EMFILE;
	int err;
	struct fidtable *fidtable;

repeat:
	spin_lock(&ftab_desc->fidtable_lock);
	fidtable = ftab_desc->ftab;
	bitmap = fidtable->cifsd_bitmap;
	id = cifsd_find_next_zero_bit(bitmap,
			fidtable->max_fids, fidtable->start_pos);
	if (id > fidtable->max_fids - 1) {
		spin_unlock(&ftab_desc->fidtable_lock);
		goto grow;
	}

	cifsd_set_bit(id, bitmap);
	fidtable->start_pos = id + 1;
	spin_unlock(&ftab_desc->fidtable_lock);

	return id;

grow:
	err = grow_fidtable(ftab_desc, id);
	if (err == 1)
		goto repeat;

	return err;
}

/**
 * cifsd_close_id() - mark fid entry as free in fid table bitmap
 * @ftab_desc:	fid table from where fid was allocated
 * @id:		fid entry to be marked as free in fid table bitmap
 *
 * If caller of cifsd_close_id() has already checked for
 * invalid value of ID, return value is not checked in that
 * caller. If caller is not checking invalid ID then caller
 * need to do error handling corresponding the return value
 * of cifsd_close_id()
 *
 * Return:      0 if success, otherwise -EINVAL
 */
int cifsd_close_id(struct fidtable_desc *ftab_desc, int id)
{
	void *bitmap;

	if (id >= ftab_desc->ftab->max_fids - 1) {
		cifsd_debug("Invalid id passed to clear in bitmap\n");
		return -EINVAL;
	}

	spin_lock(&ftab_desc->fidtable_lock);
	bitmap = ftab_desc->ftab->cifsd_bitmap;
	cifsd_clear_bit(id, bitmap);
	if (id < ftab_desc->ftab->start_pos)
		ftab_desc->ftab->start_pos = id;
	spin_unlock(&ftab_desc->fidtable_lock);
	return 0;
}

/**
 * init_fidtable() - initialize fid table
 * @ftab_desc:	fid table for which bitmap should be allocated and initialized
 *
 * Return:      0 if success, otherwise -ENOMEM
 */
int init_fidtable(struct fidtable_desc *ftab_desc)
{
	ftab_desc->ftab = alloc_fidtable(CIFSD_NR_OPEN_DEFAULT);
	if (!ftab_desc->ftab) {
		cifsd_err("Failed to allocate fid table\n");
		return -ENOMEM;
	}
	ftab_desc->ftab->max_fids = CIFSD_NR_OPEN_DEFAULT;
	ftab_desc->ftab->start_pos = 1;
	spin_lock_init(&ftab_desc->fidtable_lock);
	return 0;
}

/* Volatile ID operations */

/**
 * insert_id_in_fidtable() - insert a fid in fid table
 * @conn:	TCP server instance of connection
 * @id:		fid to be inserted into fid table
 * @filp:	associate this filp with fid
 *
 * allocate a cifsd file node, associate given filp with id
 * add insert this fid in fid table by marking it in fid bitmap
 *
 * Return:      cifsd file pointer if success, otherwise NULL
 */
struct cifsd_file *
insert_id_in_fidtable(struct cifsd_sess *sess, uint64_t sess_id,
		uint32_t tree_id, unsigned int id, struct file *filp)
{
	struct cifsd_file *fp = NULL;
	struct fidtable *ftab;

	fp = kmem_cache_zalloc(cifsd_filp_cache, GFP_NOFS);
	if (!fp) {
		cifsd_err("Failed to allocate memory for id (%u)\n", id);
		return NULL;
	}

	fp->filp = filp;
	fp->tid = tree_id;
#ifdef CONFIG_CIFS_SMB2_SERVER
	fp->sess_id = sess_id;
#endif
	fp->f_state = FP_NEW;
	INIT_LIST_HEAD(&fp->node);
	spin_lock_init(&fp->f_lock);
	init_waitqueue_head(&fp->wq);

	spin_lock(&sess->fidtable.fidtable_lock);
	ftab = sess->fidtable.ftab;
	BUG_ON(ftab->fileid[id] != NULL);
	ftab->fileid[id] = fp;
	spin_unlock(&sess->fidtable.fidtable_lock);

	return ftab->fileid[id];
}

/**
 * get_id_from_fidtable() - get cifsd file pointer for a fid
 * @conn:	TCP server instance of connection
 * @id:		fid to be looked into fid table
 *
 * lookup a fid in fid table and return associated cifsd file pointer
 *
 * Return:      cifsd file pointer if success, otherwise NULL
 */
struct cifsd_file *
get_id_from_fidtable(struct cifsd_sess *sess, uint64_t id)
{
	struct cifsd_file *file;
	struct fidtable *ftab;

	spin_lock(&sess->fidtable.fidtable_lock);
	ftab = sess->fidtable.ftab;
	if ((id < CIFSD_START_FID) || (id > ftab->max_fids - 1)) {
		spin_unlock(&sess->fidtable.fidtable_lock);
		cifsd_debug("invalid fileid (%llu)\n", id);
		return NULL;
	}

	file = ftab->fileid[id];
	if (!file) {
	spin_unlock(&sess->fidtable.fidtable_lock);
		return NULL;
	}

	spin_lock(&file->f_lock);
	if (file->f_state == FP_FREEING) {
		spin_unlock(&file->f_lock);
		spin_unlock(&sess->fidtable.fidtable_lock);
		return NULL;
	}

	spin_unlock(&file->f_lock);
	fp_get(file);
	spin_unlock(&sess->fidtable.fidtable_lock);
	return file;
}

static void wait_on_freeing_fp(struct cifsd_file *fp)
{
	int rc;

	if (atomic_read(&fp->f_count)) {
		rc = wait_event_timeout(fp->wq, atomic_read(&fp->f_count) == 0,
			1000 * HZ);
		if (!rc) {
			cifsd_err("fp : %p, f_count : %d\n",
				fp, atomic_read(&fp->f_count));
			BUG();
		}
	}
}

/**
 * delete_id_from_fidtable() - delete a fid from fid table
 * @conn:	TCP server instance of connection
 * @id:		fid to be deleted from fid table
 *
 * delete a fid from fid table and free associated cifsd file pointer
 */
void delete_id_from_fidtable(struct cifsd_sess *sess, unsigned int id)
{
	struct cifsd_file *fp;
	struct fidtable *ftab;

	spin_lock(&sess->fidtable.fidtable_lock);
	ftab = sess->fidtable.ftab;
	BUG_ON(!ftab->fileid[id]);
	fp = ftab->fileid[id];
	ftab->fileid[id] = NULL;
	spin_lock(&fp->f_lock);
	if (fp->is_stream)
		kfree(fp->stream.name);
	fp->f_mfp = NULL;
	spin_unlock(&fp->f_lock);
	fp_put(fp);

	if (atomic_read(&fp->f_count)) {
		spin_unlock(&sess->fidtable.fidtable_lock);
		wait_on_freeing_fp(fp);
		spin_lock(&sess->fidtable.fidtable_lock);
	}

	kmem_cache_free(cifsd_filp_cache, fp);
	spin_unlock(&sess->fidtable.fidtable_lock);
}

/**
 * close_id() - close filp for a fid and delete it from fid table
 * @conn:	TCP server instance of connection
 * @id:		fid to be deleted from fid table
 *
 * lookup fid from fid table, release oplock info and close associated filp.
 * delete fid, free associated cifsd file pointer and clear fid bitmap entry
 * in fid table.
 *
 * Return:      0 on success, otherwise error number
 */
int close_id(struct cifsd_sess *sess, uint64_t id, uint64_t p_id)
{
	struct cifsd_file *fp;
	struct cifsd_mfile *mfp;
	struct file *filp;
	struct dentry *dir, *dentry;
	struct cifsd_lock *lock, *tmp;
	int err;

	fp = get_id_from_fidtable(sess, id);
	if (!fp) {
		cifsd_debug("Invalid id for close: %llu\n", id);
		return -EINVAL;
	}

	if (fp->is_durable && fp->persistent_id != p_id) {
		cifsd_err("persistent id mismatch : %llu, %llu\n",
				fp->persistent_id, p_id);
		return -ENOENT;
	}

	spin_lock(&fp->f_lock);
	mfp = fp->f_mfp;
	fp->f_state = FP_FREEING;
	spin_lock(&mfp->m_lock);
	list_del(&fp->node);
	spin_unlock(&mfp->m_lock);
	spin_unlock(&fp->f_lock);

	close_id_del_oplock(sess->conn, fp, id);

	if (fp->islink)
		filp = fp->lfilp;
	else
		filp = fp->filp;

	list_for_each_entry_safe(lock, tmp, &fp->lock_list, flist) {
		struct file_lock *flock = NULL;

		if (lock->work && lock->work->type == ASYNC &&
			lock->work->async->async_status == ASYNC_PROG) {
			struct smb_work *async_work = lock->work;

			async_work->async->async_status = ASYNC_CLOSE;
		} else {
			flock = smb_flock_init(filp);
			flock->fl_type = F_UNLCK;
			flock->fl_start = lock->start;
			flock->fl_end = lock->end;
			err = smb_vfs_lock(filp, 0, flock);
			if (err)
				cifsd_err("unlock fail : %d\n", err);
			list_del(&lock->llist);
			list_del(&lock->glist);
			list_del(&lock->flist);
			locks_free_lock(lock->fl);
			locks_free_lock(flock);
			kfree(lock);
		}
	}

	if (fp->is_stream && (mfp->m_flags & S_DEL_ON_CLS_STREAM)) {
		mfp->m_flags &= ~S_DEL_ON_CLS_STREAM;
		err = smb_vfs_remove_xattr(&(filp->f_path), fp->stream.name);
		if (err)
			cifsd_err("remove xattr failed : %s\n",
				fp->stream.name);

	}

	if (atomic_dec_and_test(&mfp->m_count)) {
		spin_lock(&mfp->m_lock);
		if ((mfp->m_flags & S_DEL_ON_CLS)) {
			dentry = filp->f_path.dentry;
			dir = dentry->d_parent;
			mfp->m_flags &= ~S_DEL_ON_CLS;
			spin_unlock(&mfp->m_lock);
			smb_vfs_unlink(dir, dentry);
			spin_lock(&mfp->m_lock);
		}
		spin_unlock(&mfp->m_lock);

		mfp_free(mfp);
	}

	delete_id_from_fidtable(sess, id);
	cifsd_close_id(&sess->fidtable, id);
	filp_close(filp, (struct files_struct *)filp);
	return 0;
}

/**
 * close_opens_from_fibtable() - close all opens from a fid table
 * @sess:	session
 *
 * lookup fid from fid table, release oplock info and close associated filp.
 * delete fid, free associated cifsd file pointer and clear fid bitmap entry
 * in fid table.
 */
void close_opens_from_fibtable(struct cifsd_sess *sess, uint32_t tree_id)
{
	struct cifsd_file *file;
	struct fidtable *ftab;
	int id;

	spin_lock(&sess->fidtable.fidtable_lock);
	ftab = sess->fidtable.ftab;
	spin_unlock(&sess->fidtable.fidtable_lock);

	for (id = 0; id < ftab->max_fids; id++) {
		file = ftab->fileid[id];
		if (file && file->tid == tree_id) {
#ifdef CONFIG_CIFS_SMB2_SERVER
			if (file->is_durable)
				close_persistent_id(file->persistent_id);
#endif
			if (!close_id(sess, id, file->persistent_id) &&
				sess->conn->stats.open_files_count > 0)
				sess->conn->stats.open_files_count--;

		}
	}
}

/**
 * destroy_fidtable() - destroy a fid table for given cifsd thread
 * @sess:	session
 *
 * lookup fid from fid table, release oplock info and close associated filp.
 * delete fid, free associated cifsd file pointer and clear fid bitmap entry
 * in fid table.
 */
void destroy_fidtable(struct cifsd_sess *sess)
{
	struct cifsd_file *file;
	struct fidtable *ftab;
	int id;

	spin_lock(&sess->fidtable.fidtable_lock);
	ftab = sess->fidtable.ftab;
	spin_unlock(&sess->fidtable.fidtable_lock);

	if (!ftab)
		return;
	for (id = 0; id < ftab->max_fids; id++) {
		file = ftab->fileid[id];
		if (file) {
#ifdef CONFIG_CIFS_SMB2_SERVER
			if (file->is_durable)
				close_persistent_id(file->persistent_id);
#endif

			if (!close_id(sess, id, file->persistent_id) &&
				sess->conn->stats.open_files_count > 0)
				sess->conn->stats.open_files_count--;
		}
	}
	sess->fidtable.ftab = NULL;
	free_fidtable(ftab);
}

/* End of Volatile-ID operations */

/* Persistent-ID operations */

#ifdef CONFIG_CIFS_SMB2_SERVER
/**
 * cifsd_insert_in_global_table() - insert a fid in global fid table
 *					for persistent id
 * @conn:		TCP server instance of connection
 * @volatile_id:	volatile id
 * @filp:		file pointer
 * @durable_open:	true if durable open is requested
 *
 * Return:      persistent_id on success, otherwise error number
 */
int cifsd_insert_in_global_table(struct cifsd_sess *sess,
				   int volatile_id, struct file *filp,
				   int durable_open)
{
	int rc;
	int persistent_id;
	struct cifsd_durable_state *durable_state;
	struct fidtable *ftab;

	persistent_id = cifsd_get_unused_id(&global_fidtable);

	if (persistent_id < 0) {
		cifsd_err("failed to get unused persistent_id for file\n");
		rc = persistent_id;
		return rc;
	}

	cifsd_debug("persistent_id allocated %d", persistent_id);

	/* If not durable open just return the ID.
	 * No need to store durable state */
	if (!durable_open)
		return persistent_id;

	durable_state = kzalloc(sizeof(struct cifsd_durable_state),
			GFP_KERNEL);

	if (durable_state == NULL) {
		cifsd_err("persistent_id insert failed\n");
		cifsd_close_id(&global_fidtable, persistent_id);
		rc = -ENOMEM;
		return rc;
	}

	durable_state->sess = sess;
	durable_state->volatile_id = volatile_id;
	generic_fillattr(filp->f_path.dentry->d_inode, &durable_state->stat);
	durable_state->refcount = 1;

	cifsd_debug("filp stored = 0x%p sess = 0x%p\n", filp, sess);

	spin_lock(&global_fidtable.fidtable_lock);
	ftab = global_fidtable.ftab;
	BUG_ON(ftab->fileid[persistent_id] != NULL);
	ftab->fileid[persistent_id] = (void *)durable_state;
	spin_unlock(&global_fidtable.fidtable_lock);

	return persistent_id;
}

/**
 * cifsd_get_durable_state() - get durable state info for a fid
 * @id:		persistent id
 *
 * Return:      durable state on success, otherwise NULL
 */
struct cifsd_durable_state *
cifsd_get_durable_state(uint64_t id)
{
	struct cifsd_durable_state *durable_state;
	struct fidtable *ftab;

	spin_lock(&global_fidtable.fidtable_lock);
	ftab = global_fidtable.ftab;
	if ((id < CIFSD_START_FID) || (id > ftab->max_fids - 1)) {
		cifsd_err("invalid persistentID (%llu)\n", id);
		spin_unlock(&global_fidtable.fidtable_lock);
		return NULL;
	}

	durable_state = (struct cifsd_durable_state *)ftab->fileid[id];
	spin_unlock(&global_fidtable.fidtable_lock);
	return durable_state;
}

/**
 * cifsd_update_durable_state() - update durable state for a fid
 * @conn:		TCP server instance of connection
 * @persistent_id:	persistent id
 * @volatile_id:	volatile id
 * @filp:		file pointer
 */
void cifsd_update_durable_state(struct cifsd_sess *sess,
			     unsigned int persistent_id,
			     unsigned int volatile_id, struct file *filp)
{
	struct cifsd_durable_state *durable_state;
	struct fidtable *ftab;

	spin_lock(&global_fidtable.fidtable_lock);
	ftab = global_fidtable.ftab;

	durable_state =
		(struct cifsd_durable_state *)ftab->fileid[persistent_id];

	durable_state->sess = sess;
	durable_state->volatile_id = volatile_id;
	generic_fillattr(filp->f_path.dentry->d_inode, &durable_state->stat);
	durable_state->refcount++;
	spin_unlock(&global_fidtable.fidtable_lock);
	cifsd_debug("durable state updated persistentID (%u)\n",
		      persistent_id);
}

/**
 * cifsd_durable_disconnect() - update file stat with durable state
 * @conn:		TCP server instance of connection
 * @persistent_id:	persistent id
 * @filp:		file pointer
 */
void cifsd_durable_disconnect(struct connection *conn,
			   unsigned int persistent_id, struct file *filp)
{
	struct cifsd_durable_state *durable_state;
	struct fidtable *ftab;

	spin_lock(&global_fidtable.fidtable_lock);
	ftab = global_fidtable.ftab;

	durable_state =
		(struct cifsd_durable_state *)ftab->fileid[persistent_id];
	BUG_ON(durable_state == NULL);
	generic_fillattr(filp->f_path.dentry->d_inode, &durable_state->stat);
	spin_unlock(&global_fidtable.fidtable_lock);
	cifsd_debug("durable state disconnect persistentID (%u)\n",
		    persistent_id);
}

/**
 * cifsd_delete_durable_state() - delete durable state for a id
 * @id:		persistent id
 *
 * Return:      0 or 1 on success, otherwise error number
 */
int cifsd_delete_durable_state(uint64_t id)
{
	struct cifsd_durable_state *durable_state;
	struct fidtable *ftab;

	spin_lock(&global_fidtable.fidtable_lock);
	ftab = global_fidtable.ftab;
	if (id > ftab->max_fids - 1) {
		cifsd_err("Invalid id %llu\n", id);
		spin_unlock(&global_fidtable.fidtable_lock);
		return -EINVAL;
	}
	durable_state = (struct cifsd_durable_state *)ftab->fileid[id];

	/* If refcount > 1 return 1 to avoid deletion of persistent-id
	   from the global_fidtable bitmap */
	if (durable_state && durable_state->refcount > 1) {
		--durable_state->refcount;
		spin_unlock(&global_fidtable.fidtable_lock);
		return 1;
	}

	/* Check if durable state is associated with file
	   before deleting persistent id of a opened file,
	   because opened file may or may not be associated
	   with a durable handle */
	if (durable_state) {
		cifsd_debug("durable state delete persistentID (%llu) refcount = %d\n",
			    id, durable_state->refcount);
		kfree(durable_state);
	}

	ftab->fileid[id] = NULL;
	spin_unlock(&global_fidtable.fidtable_lock);
	return 0;
}

/**
 * close_persistent_id() - delete a persistent id from global fid table
 * @id:		persistent id
 *
 * Return:      0 on success, otherwise error number
 */
int close_persistent_id(uint64_t id)
{
	int rc = 0;

	rc = cifsd_delete_durable_state(id);
	if (rc < 0)
		return rc;
	else if (rc > 0)
		return 0;

	rc = cifsd_close_id(&global_fidtable, id);
	return rc;
}

/**
 * destroy_global_fidtable() - destroy global fid table at module exit
 */
void destroy_global_fidtable(void)
{
	struct cifsd_durable_state *durable_state;
	struct fidtable *ftab;
	int i;

	spin_lock(&global_fidtable.fidtable_lock);
	ftab = global_fidtable.ftab;
	global_fidtable.ftab = NULL;
	spin_unlock(&global_fidtable.fidtable_lock);

	for (i = 0; i < ftab->max_fids; i++) {
		durable_state = (struct cifsd_durable_state *)ftab->fileid[i];
			kfree(durable_state);
			ftab->fileid[i] = NULL;
	}
	free_fidtable(ftab);
}
#endif

/**
 * cifsd_check_stat_info() - compare durable state and current inode stat
 * @durable_stat:	inode stat stored in durable state
 * @current_stat:	current inode stat
 *
 * Return:		0 if mismatch, 1 if no mismatch
 */
int cifsd_check_stat_info(struct kstat *durable_stat,
				struct kstat *current_stat)
{
	if (durable_stat->ino != current_stat->ino) {
		cifsd_err("Inode mismatch\n");
		return 0;
	}

	if (durable_stat->dev != current_stat->dev) {
		cifsd_err("Device mismatch\n");
		return 0;
	}

	if (durable_stat->mode != current_stat->mode) {
		cifsd_err("Mode mismatch\n");
		return 0;
	}

	if (durable_stat->nlink != current_stat->nlink) {
		cifsd_err("Nlink mismatch\n");
		return 0;
	}

#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
	if (!uid_eq(durable_stat->uid, current_stat->uid)) {
#else
	if (durable_stat->uid != current_stat->uid) {
#endif
		cifsd_err("Uid mismatch\n");
		return 0;
	}

#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
	if (!gid_eq(durable_stat->gid, current_stat->gid)) {
#else
	if (durable_stat->gid != current_stat->gid) {
#endif
		cifsd_err("Gid mismatch\n");
		return 0;
	}

	if (durable_stat->rdev != current_stat->rdev) {
		cifsd_err("Special file devid mismatch\n");
		return 0;
	}

	if (durable_stat->size != current_stat->size) {
		cifsd_err("Size mismatch\n");
		return 0;
	}

	if (durable_stat->atime.tv_sec != current_stat->atime.tv_sec &&
	    durable_stat->atime.tv_nsec != current_stat->atime.tv_nsec) {
		cifsd_err("Last access time mismatch\n");
		return 0;
	}

	if (durable_stat->mtime.tv_sec  != current_stat->mtime.tv_sec &&
	    durable_stat->mtime.tv_nsec != current_stat->mtime.tv_nsec) {
		cifsd_err("Last modification time mismatch\n");
		return 0;
	}

	if (durable_stat->ctime.tv_sec != current_stat->ctime.tv_sec &&
	    durable_stat->ctime.tv_nsec != current_stat->ctime.tv_nsec) {
		cifsd_err("Last status change time mismatch\n");
		return 0;
	}

	if (durable_stat->blksize != current_stat->blksize) {
		cifsd_err("Block size mismatch\n");
		return 0;
	}

	if (durable_stat->blocks != current_stat->blocks) {
		cifsd_err("Block number mismatch\n");
		return 0;
	}

	return 1;
}

#ifdef CONFIG_CIFS_SMB2_SERVER
/**
 * cifsd_durable_reconnect() - verify durable state on reconnect
 * @curr_conn:	TCP server instance of connection
 * @durable_stat:	durable state of filp
 * @filp:		current inode stat
 *
 * Return:		0 if no mismatch, otherwise error
 */
int cifsd_durable_reconnect(struct cifsd_sess *curr_sess,
			  struct cifsd_durable_state *durable_state,
			  struct file **filp)
{
	struct kstat stat;
	struct path *path;
	int rc = 0;

	rc = cifsd_durable_verify_and_del_oplock(curr_sess,
						   durable_state->sess,
						   durable_state->volatile_id,
						   filp, curr_sess->sess_id);

	if (rc < 0) {
		*filp = NULL;
		cifsd_err("Oplock state not consistent\n");
		return rc;
	}

	/* Get the current stat info.
	   Incrementing refcount of filp
	   because when server thread is destroy_fidtable
	   will close the filp when old server thread is destroyed*/
	get_file(*filp);
	path = &((*filp)->f_path);
	generic_fillattr(path->dentry->d_inode, &stat);

	if (!cifsd_check_stat_info(&durable_state->stat, &stat)) {
		cifsd_err("Stat info mismatch file state changed\n");
		fput(*filp);
		rc = -EINVAL;
	}

	return rc;
}

/**
 * cifsd_update_durable_stat_info() - update durable state of all
 *		persistent fid of a server thread
 * @conn:	TCP server instance of connection
 */
void cifsd_update_durable_stat_info(struct cifsd_sess *sess)
{
	struct cifsd_file *fp;
	struct fidtable *ftab;
	int id;
	struct cifsd_durable_state *durable_state;
	struct fidtable *gtab;
	struct file *filp;
	uint64_t p_id;

	if (durable_enable == false || !sess)
		return;

	spin_lock(&sess->fidtable.fidtable_lock);
	ftab = sess->fidtable.ftab;

	for (id = 0; id < ftab->max_fids; id++) {
		fp = ftab->fileid[id];
		if (fp && fp->is_durable) {
			/* Mainly for updating kstat info */
			filp = fp->filp;
			p_id = fp->persistent_id;
			spin_lock(&global_fidtable.fidtable_lock);

			gtab = global_fidtable.ftab;
			durable_state =
			  (struct cifsd_durable_state *)gtab->fileid[p_id];
			BUG_ON(durable_state == NULL);
			generic_fillattr(filp->f_path.dentry->d_inode,
					 &durable_state->stat);
			spin_unlock(&global_fidtable.fidtable_lock);
		}
	}
	spin_unlock(&sess->fidtable.fidtable_lock);
}
#endif

/* End of persistent-ID functions */

/**
 * smb_dentry_open() - open a dentry and provide fid for it
 * @work:	smb work ptr
 * @path:	path of dentry to be opened
 * @flags:	open flags
 * @ret_id:	fid returned on this
 * @oplock:	return oplock state granted on file
 * @option:	file access pattern options for fadvise
 * @fexist:	file already present or not
 *
 * Return:	0 on success, otherwise error
 */
int smb_dentry_open(struct smb_work *work, const struct path *path,
		    int flags, __u16 *ret_id, int *oplock, int option,
		    int fexist)
{
	struct file *filp;
	int id, err = 0;
	struct cifsd_file *fp;
	struct smb_hdr *rcv_hdr = (struct smb_hdr *)work->buf;
	uint64_t sess_id;

	/* first init id as invalid id - 0xFFFF ? */
	*ret_id = 0xFFFF;

	id = cifsd_get_unused_id(&work->sess->fidtable);
	if (id < 0)
		return id;

	if (flags & O_TRUNC) {
		if (oplocks_enable && fexist)
			smb_break_all_oplock(work, NULL,
					path->dentry->d_inode);
		err = vfs_truncate((struct path *)path, 0);
		if (err)
			goto err_out;
	}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
	filp = dentry_open(path, flags | O_LARGEFILE, current_cred());
#else
	filp = dentry_open(path->dentry, path->mnt, flags | O_LARGEFILE,
		current_cred());
#endif
	if (IS_ERR(filp)) {
		err = PTR_ERR(filp);
		cifsd_err("dentry open failed, err %d\n", err);
		goto err_out;
	}

	smb_vfs_set_fadvise(filp, option);

	sess_id = work->sess == NULL ? 0 : work->sess->sess_id;
	fp = insert_id_in_fidtable(work->sess, sess_id,
		le16_to_cpu(rcv_hdr->Tid), id, filp);
	if (fp == NULL) {
		fput(filp);
		cifsd_err("id insert failed\n");
		goto err_out;
	}

	INIT_LIST_HEAD(&fp->lock_list);

	if (!oplocks_enable || S_ISDIR(file_inode(filp)->i_mode))
		*oplock = OPLOCK_NONE;

	if (!S_ISDIR(file_inode(filp)->i_mode) &&
			(*oplock & (REQ_BATCHOPLOCK | REQ_OPLOCK))) {
		/* Client cannot request levelII oplock directly */
		err = smb_grant_oplock(work, oplock, id, fp, rcv_hdr->Tid,
			NULL);
		/* if we enconter an error, no oplock is granted */
		if (err)
			*oplock = 0;
	}

	*ret_id = id;
	return 0;

err_out:
	cifsd_close_id(&work->sess->fidtable, id);
	return err;
}

/**
 * is_dir_empty() - check for empty directory
 * @fp:	cifsd file pointer
 *
 * Return:	true if directory empty, otherwise false
 */
bool is_dir_empty(struct cifsd_file *fp)
{
	struct smb_readdir_data r_data = {
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
		.ctx.actor = smb_filldir,
#endif
		.dirent = (void *)__get_free_page(GFP_KERNEL),
		.dirent_count = 0
	};

	if (!r_data.dirent)
		return false;

	smb_vfs_readdir(fp->filp, smb_filldir, &r_data);
	cifsd_debug("dirent_count = %d\n", r_data.dirent_count);
	if (r_data.dirent_count > 2) {
		free_page((unsigned long)(r_data.dirent));
		return false;
	}

	free_page((unsigned long)(r_data.dirent));
	return true;
}

/**
 * smb_kern_path() - lookup a file and get path info
 * @name:	name of file for lookup
 * @flags:	lookup flags
 * @path:	if lookup succeed, return path info
 * @caseless:	caseless filename lookup
 *
 * Return:	0 on success, otherwise error
 */
int smb_kern_path(char *name, unsigned int flags, struct path *path,
		bool caseless)
{
	int err;

	err = kern_path(name, flags, path);
	if (err && caseless) {
		char *filename = strrchr((const char *)name, '/');
		if (filename == NULL)
			return err;
		*(filename++) = '\0';
		if (strlen(name) == 0) {
			/* root reached */
			filename--;
			*filename = '/';
			return err;
		}
		err = smb_search_dir(name, filename);
		if (err)
			return err;
		err = kern_path(name, flags, path);
		return err;
	} else
		return err;
}

/**
 * smb_search_dir() - lookup a file in a directory
 * @dirname:	directory name
 * @filename:	filename to lookup
 *
 * Return:	0 on success, otherwise error
 */
int smb_search_dir(char *dirname, char *filename)
{
	struct path dir_path;
	int ret;
	struct file *dfilp;
	int flags = O_RDONLY|O_LARGEFILE;
	int used_count, reclen;
	int iter;
	struct smb_dirent *buf_p;
	int namelen = strlen(filename);
	int dirnamelen = strlen(dirname);
	bool match_found = false;
	struct smb_readdir_data readdir_data = {
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
		.ctx.actor = smb_filldir,
#endif
		.dirent = (void *)__get_free_page(GFP_KERNEL)
	};

	if (!readdir_data.dirent) {
		ret = -ENOMEM;
		goto out;
	}

	ret = smb_kern_path(dirname, 0, &dir_path, true);
	if (ret)
		goto out;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
	dfilp = dentry_open(&dir_path, flags, current_cred());
#else
	dfilp = dentry_open(dir_path.dentry, dir_path.mnt, flags, current_cred());
#endif
	if (IS_ERR(dfilp)) {
		cifsd_err("cannot open directory %s\n", dirname);
		ret = -EINVAL;
		goto out2;
	}

	while (!ret && !match_found) {
		readdir_data.used = 0;
		readdir_data.full = 0;
		ret = smb_vfs_readdir(dfilp, smb_filldir, &readdir_data);
		used_count = readdir_data.used;
		if (ret || !used_count)
			break;

		buf_p = (struct smb_dirent *)readdir_data.dirent;
		for (iter = 0; iter < used_count; iter += reclen,
		     buf_p = (struct smb_dirent *)((char *)buf_p + reclen)) {
			int length;

			reclen = ALIGN(sizeof(struct smb_dirent) +
				       buf_p->namelen, sizeof(__le64));
			length = buf_p->namelen;
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
			if (length != namelen ||
				strncasecmp(filename, buf_p->name, namelen))
#else
			if (length != namelen ||
				strnicmp(filename, buf_p->name, namelen))
#endif
				continue;
			/* got match, make absolute name */
			memcpy(dirname + dirnamelen + 1, buf_p->name, namelen);
			match_found = true;
			break;
		}
	}

	free_page((unsigned long)(readdir_data.dirent));
	fput(dfilp);
out2:
	path_put(&dir_path);
out:
	dirname[dirnamelen] = '/';
	return ret;
}

/**
 * get_pipe_id() - get a free id for a pipe
 * @conn:	TCP server instance of connection
 *
 * Return:	id on success, otherwise error
 */
int get_pipe_id(struct cifsd_sess *sess, unsigned int pipe_type)
{
	int id;
	struct cifsd_pipe *pipe_desc;

	id = cifsd_get_unused_id(&sess->fidtable);
	if (id < 0)
		return -EMFILE;

	sess->pipe_desc[pipe_type] = kzalloc(sizeof(struct cifsd_pipe),
			GFP_KERNEL);
	if (!sess->pipe_desc)
		return -ENOMEM;

	pipe_desc = sess->pipe_desc[pipe_type];
	pipe_desc->id = id;
	pipe_desc->pkt_type = -1;

	pipe_desc->rsp_buf = kmalloc(NETLINK_CIFSD_MAX_PAYLOAD,
			GFP_KERNEL);
	if (!pipe_desc->rsp_buf) {
		kfree(pipe_desc);
		sess->pipe_desc[pipe_type] = NULL;
		return -ENOMEM;
	}

	switch (pipe_type) {
	case SRVSVC:
		pipe_desc->pipe_type = SRVSVC;
		break;
	case WINREG:
		pipe_desc->pipe_type = WINREG;
		break;
	default:
		cifsd_err("pipe type :%d not supported\n", pipe_type);
		return -EINVAL;
	}

	return id;
}

/**
 * close_pipe_id() - free id for pipe on a server thread
 * @sess:	session information
 * @pipe_type:	pipe type
 *
 * Return:	0 on success, otherwise error
 */
int close_pipe_id(struct cifsd_sess *sess, int pipe_type)
{
	struct cifsd_pipe *pipe_desc;
	int rc = 0;

	pipe_desc = sess->pipe_desc[pipe_type];
	if (!pipe_desc)
		return -EINVAL;

	rc = cifsd_close_id(&sess->fidtable, pipe_desc->id);
	if (rc < 0)
		return rc;

	kfree(pipe_desc->rsp_buf);
	kfree(pipe_desc);
	sess->pipe_desc[pipe_type] = NULL;

	return rc;
}

static unsigned int mfp_hash_mask __read_mostly;
static unsigned int mfp_hash_shift __read_mostly;
static struct hlist_head *mfp_hashtable __read_mostly;
static __cacheline_aligned_in_smp DEFINE_SPINLOCK(mfp_hash_lock);

static unsigned long mfp_hash(struct super_block *sb, unsigned long hashval)
{
	unsigned long tmp;

	tmp = (hashval * (unsigned long)sb) ^ (GOLDEN_RATIO_PRIME + hashval) /
		L1_CACHE_BYTES;
	tmp = tmp ^ ((tmp ^ GOLDEN_RATIO_PRIME) >> mfp_hash_shift);
	return tmp & mfp_hash_mask;
}

struct cifsd_mfile *mfp_lookup(struct inode *inode)
{
	struct hlist_head *head = mfp_hashtable +
		mfp_hash(inode->i_sb, inode->i_ino);
	struct cifsd_mfile *mfp = NULL, *ret_mfp = NULL;

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,9,0)
	struct hlist_node *pos;
#endif


	spin_lock(&mfp_hash_lock);
	compat_hlist_for_each_entry(mfp, pos, head, m_hash) {
		if (mfp->m_inode == inode) {
			atomic_inc(&mfp->m_count);
			ret_mfp = mfp;
			break;
		}
	}
	spin_unlock(&mfp_hash_lock);

	return ret_mfp;
}

void insert_mfp_hash(struct cifsd_mfile *mfp)
{
	struct hlist_head *b = mfp_hashtable +
		mfp_hash(mfp->m_inode->i_sb, mfp->m_inode->i_ino);

	spin_lock(&mfp_hash_lock);
	hlist_add_head(&mfp->m_hash, b);
	spin_unlock(&mfp_hash_lock);
}

void remove_mfp_hash(struct cifsd_mfile *mfp)
{
	spin_lock(&mfp_hash_lock);
	hlist_del_init(&mfp->m_hash);
	spin_unlock(&mfp_hash_lock);
}

void mfp_init(struct cifsd_mfile *mfp, struct inode *inode)
{
	mfp->m_inode = inode;
	atomic_set(&mfp->m_count, 1);
	mfp->m_flags = 0;
	INIT_LIST_HEAD(&mfp->m_fp_list);
	spin_lock_init(&mfp->m_lock);
	insert_mfp_hash(mfp);
}

void mfp_free(struct cifsd_mfile *mfp)
{
	remove_mfp_hash(mfp);
	kfree(mfp);
}

void __init mfp_hash_init(void)
{
	unsigned int loop;
	unsigned long numentries = 16384;
	unsigned long bucketsize = sizeof(struct hlist_head);
	unsigned long size;

	mfp_hash_shift = ilog2(numentries);
	mfp_hash_mask = (1 << mfp_hash_shift) - 1;

	size = bucketsize << mfp_hash_shift;

	/* init master fp hash table */
	mfp_hashtable = __vmalloc(size, GFP_ATOMIC, PAGE_KERNEL);

	for (loop = 0; loop < (1U << mfp_hash_shift); loop++)
		INIT_HLIST_HEAD(&mfp_hashtable[loop]);
}
