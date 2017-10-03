/*
 *   fs/cifsd/vfs.c
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

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/backing-dev.h>
#include <linux/writeback.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
#include <linux/xattr.h>
#endif
#include <linux/falloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>

#include "export.h"
#include "glob.h"
#include "oplock.h"

/**
 * smb_vfs_create() - vfs helper for smb create file
 * @name:	file name
 * @mode:	file create mode
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_create(const char *name, umode_t mode)
{
	struct path path;
	struct dentry *dentry;
	int err;

	dentry = kern_path_create(AT_FDCWD, name, &path, 0);
	if (IS_ERR(dentry)) {
		err = PTR_ERR(dentry);
		cifsd_err("path create failed for %s, err %d\n", name, err);
		return err;
	}

	mode |= S_IFREG;
	err = vfs_create(path.dentry->d_inode, dentry, mode, true);
	if (err)
		cifsd_err("File(%s): creation failed (err:%d)\n", name, err);

	done_path_create(&path, dentry);

	return err;
}

/**
 * smb_vfs_mkdir() - vfs helper for smb create directory
 * @name:	directory name
 * @mode:	directory create mode
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_mkdir(const char *name, umode_t mode)
{
	struct path path;
	struct dentry *dentry;
	int err;

	dentry = kern_path_create(AT_FDCWD, name, &path, LOOKUP_DIRECTORY);
	if (IS_ERR(dentry)) {
		err = PTR_ERR(dentry);
		if (err != -EEXIST)
			cifsd_err("path create failed for %s, err %d\n",
					name, err);
		return err;
	}

	mode |= S_IFDIR;
	err = vfs_mkdir(path.dentry->d_inode, dentry, mode);
	if (err)
		cifsd_err("mkdir(%s): creation failed (err:%d)\n", name, err);

	done_path_create(&path, dentry);

	return err;
}

/**
 * smb_vfs_read() - vfs helper for smb file read
 * @sess:	session
 * @fid:	file id of open file
 * @buf:	buf containing read data
 * @count:	read byte count
 * @pos:	file pos
 *
 * Return:	number of read bytes on success, otherwise error
 */
int smb_vfs_read(struct cifsd_sess *sess, uint64_t fid, uint64_t p_id,
	char **buf, size_t count, loff_t *pos)
{
	struct file *filp;
	ssize_t nbytes = 0;
	mm_segment_t old_fs;
	struct cifsd_file *fp;
	char *rbuf, *name;
	struct inode *inode;
	char namebuf[NAME_MAX];
	int ret;

	fp = get_id_from_fidtable(sess, fid);
	if (!fp) {
		cifsd_err("failed to get filp for fid %llu\n", fid);
		return -ENOENT;
	}

	filp = fp->filp;
	inode = filp->f_path.dentry->d_inode;
	if (S_ISDIR(inode->i_mode)) {
		nbytes = -EISDIR;
		goto out;
	}

	if (unlikely(count == 0))
		goto out;

#ifdef CONFIG_CIFS_SMB2_SERVER
	if (fp->is_durable && fp->persistent_id != p_id) {
		cifsd_err("persistent id mismatch : %llu, %llu\n",
				fp->persistent_id, p_id);
		nbytes = -ENOENT;
		goto out;
	}

	if (sess->conn->connection_type) {
		if (!(fp->daccess & (FILE_READ_DATA_LE |
		    FILE_GENERIC_READ_LE | FILE_MAXIMAL_ACCESS_LE |
		    FILE_GENERIC_ALL_LE))) {
			cifsd_err("no right to read(%llu)\n", fid);
			nbytes = -EACCES;
			goto out;
		}
	}
#endif

	rbuf = alloc_data_mem(count);
	if (!rbuf) {
		nbytes = -ENOMEM;
		goto out;
	}
	if (fp->is_stream) {
		ssize_t v_len;
		char *stream_buf = NULL;

		cifsd_debug("read stream data pos : %llu, count : %zd\n",
			*pos, count);

		v_len = smb_find_cont_xattr(&filp->f_path, fp->stream.name,
			fp->stream.size, &stream_buf, 1);
		if (v_len == -ENOENT) {
			cifsd_err("not found stream in xattr : %zd\n", v_len);
			kvfree(rbuf);
			nbytes = -ENOENT;
			goto out;
		}

		memcpy(rbuf, &stream_buf[*pos], count);

		*buf = rbuf;
		nbytes = v_len > count ? count : v_len;
		goto out;
	}

	ret = check_lock_range(filp, *pos, *pos + count - 1,
			READ);
	if (ret) {
		cifsd_err("%s: unable to read due to lock\n",
				__func__);
		kvfree(rbuf);
		nbytes = -EAGAIN;
		goto out;
	}

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	nbytes = vfs_read(filp, rbuf, count, pos);
	set_fs(old_fs);
	if (nbytes < 0) {
		name = d_path(&filp->f_path, namebuf, sizeof(namebuf));
		if (IS_ERR(name))
			name = "(error)";
		cifsd_err("smb read failed for (%s), err = %zd\n",
				name, nbytes);
		kvfree(rbuf);
	} else {
		*buf = rbuf;
		filp->f_pos = *pos;
	}

out:
	fp_put(fp);
	return nbytes;
}

/**
 * smb_vfs_write() - vfs helper for smb file write
 * @sess:	session
 * @fid:	file id of open file
 * @buf:	buf containing data for writing
 * @count:	read byte count
 * @pos:	file pos
 * @sync:	fsync after write
 * @written:	number of bytes written
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_write(struct cifsd_sess *sess, uint64_t fid, uint64_t p_id,
	char *buf, size_t count, loff_t *pos, bool sync, ssize_t *written)
{
	struct file *filp;
	loff_t	offset = *pos;
	mm_segment_t old_fs;
	struct cifsd_file *fp;
	int err = 0;

	fp = get_id_from_fidtable(sess, fid);
	if (!fp) {
		cifsd_err("failed to get filp for fid %llu session = 0x%p\n",
				fid, sess);
		return -ENOENT;
	}

#ifdef CONFIG_CIFS_SMB2_SERVER
	if (fp->is_durable && fp->persistent_id != p_id) {
		cifsd_err("persistent id mismatch : %llu, %llu\n",
			fp->persistent_id, p_id);
		err = -ENOENT;
		goto out;
	}

	if (sess->conn->connection_type) {
		if (!(fp->daccess & (FILE_WRITE_DATA_LE |
		   FILE_GENERIC_WRITE_LE | FILE_MAXIMAL_ACCESS_LE |
		   FILE_GENERIC_ALL_LE))) {
			cifsd_err("no right to write(%llu)\n", fid);
			err = -EACCES;
			goto out;
		}
	}
#endif

	filp = fp->filp;

	if (fp->is_stream) {
		char *stream_buf = NULL, *wbuf;
		size_t size, v_len;

		cifsd_debug("write stream data pos : %llu, count : %zd\n",
			*pos, count);

		size = *pos + count;
		if (size > XATTR_SIZE_MAX) {
			size = XATTR_SIZE_MAX;
			count = (*pos + count) - XATTR_SIZE_MAX;
		}

		v_len = smb_find_cont_xattr(&filp->f_path, fp->stream.name,
			fp->stream.size, &stream_buf, 1);
		if (v_len == -ENOENT) {
			cifsd_err("not found stream in xattr : %zd\n", v_len);
			kvfree(stream_buf);
			err = -ENOENT;
			goto out;
		}

		if (v_len < size) {
			wbuf = alloc_data_mem(size);
			if (!wbuf) {
				kvfree(stream_buf);
				err = -ENOMEM;
				goto out;
			}

			if (v_len > 0) {
				memcpy(wbuf, stream_buf, v_len);
				kvfree(stream_buf);
			}
			stream_buf = wbuf;
		}

		memcpy(&stream_buf[*pos], buf, count);

		err = smb_store_cont_xattr(&filp->f_path, fp->stream.name,
			(void *)stream_buf, size);
		if (err < 0)
			goto out;

		kvfree(stream_buf);
		filp->f_pos = *pos;
		*written = count;
		err = 0;
		goto out;
	}

	err = check_lock_range(filp, *pos, *pos + count - 1,
			WRITE);
	if (err) {
		cifsd_err("%s: unable to write due to lock\n",
				__func__);
		err = -EAGAIN;
		goto out;
	}

	old_fs = get_fs();
	set_fs(KERNEL_DS);

	if (oplocks_enable) {
		/* Do we need to break any of a levelII oplock? */
		mutex_lock(&ofile_list_lock);
		smb_break_all_levII_oplock(sess->conn, fp, NULL, 1);
		mutex_unlock(&ofile_list_lock);
	}

	err = vfs_write(filp, buf, count, pos);
	set_fs(old_fs);
	if (err < 0) {
		cifsd_debug("smb write failed, err = %d\n", err);
		goto out;
	}

	filp->f_pos = *pos;
	*written = err;
	err = 0;
	if (sync) {
		err = vfs_fsync_range(filp, offset, offset + *written, 0);
		if (err < 0)
			cifsd_err("fsync failed for fid %llu, err = %d\n",
					fid, err);
	}

out:
	fp_put(fp);
	return err;
}

/**
 * smb_check_attrs() - sanitize inode attributes
 * @inode:	inode
 * @attrs:	inode attributes
 */
void smb_check_attrs(struct inode *inode, struct iattr *attrs)
{
	/* sanitize the mode change */
	if (attrs->ia_valid & ATTR_MODE) {
		attrs->ia_mode &= S_IALLUGO;
		attrs->ia_mode |= (inode->i_mode & ~S_IALLUGO);
	}

	/* Revoke setuid/setgid on chown */
	if (!S_ISDIR(inode->i_mode) &&
		(((attrs->ia_valid & ATTR_UID) &&
				!uid_eq(attrs->ia_uid, inode->i_uid)) ||
		 ((attrs->ia_valid & ATTR_GID) &&
				!gid_eq(attrs->ia_gid, inode->i_gid)))) {
		attrs->ia_valid |= ATTR_KILL_PRIV;
		if (attrs->ia_valid & ATTR_MODE) {
			/* we're setting mode too, just clear the s*id bits */
			attrs->ia_mode &= ~S_ISUID;
			if (attrs->ia_mode & S_IXGRP)
				attrs->ia_mode &= ~S_ISGID;
		} else {
			/* set ATTR_KILL_* bits and let VFS handle it */
			attrs->ia_valid |= (ATTR_KILL_SUID | ATTR_KILL_SGID);
		}
	}
}

/**
 * smb_vfs_setattr() - vfs helper for smb setattr
 * @sess:	session
 * @name:	file name
 * @fid:	file id of open file
 * @attrs:	inode attributes
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_setattr(struct cifsd_sess *sess, const char *name,
		uint64_t fid, struct iattr *attrs)
{
	struct file *filp;
	struct dentry *dentry;
	struct inode *inode;
	struct path path;
	bool update_size = false;
	int err = 0;
	struct cifsd_file *fp = NULL;

	if (name) {
		err = kern_path(name, 0, &path);
		if (err) {
			cifsd_debug("lookup failed for %s, err = %d\n",
					name, err);
			return -ENOENT;
		}
		dentry = path.dentry;
		inode = dentry->d_inode;
	} else {

		fp = get_id_from_fidtable(sess, fid);
		if (!fp) {
			cifsd_err("failed to get filp for fid %llu\n", fid);
			return -ENOENT;
		}

		filp = fp->filp;
		dentry = filp->f_path.dentry;
		inode = dentry->d_inode;
	}

	/* no need to update mode of symlink */
	if (S_ISLNK(inode->i_mode))
		attrs->ia_valid &= ~ATTR_MODE;

	/* skip setattr, if nothing to update */
	if (!attrs->ia_valid) {
		err = 0;
		goto out;
	}

	smb_check_attrs(inode, attrs);
	if (attrs->ia_valid & ATTR_SIZE) {
		err = get_write_access(inode);
		if (err)
			goto out;

		err = locks_verify_truncate(inode, NULL, attrs->ia_size);
		if (err) {
			put_write_access(inode);
			goto out;
		}
		update_size = true;
	}

	attrs->ia_valid |= ATTR_CTIME;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0)
	inode_lock(inode);
	err = notify_change(dentry, attrs, NULL);
	inode_unlock(inode);
#else
	mutex_lock(&inode->i_mutex);
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 13, 0)
	err = notify_change(dentry, attrs, NULL);
#else
	err = notify_change(dentry, attrs);
#endif
	mutex_unlock(&inode->i_mutex);
#endif

	if (update_size)
		put_write_access(inode);

	if (!err) {
		sync_inode_metadata(inode, 1);
		cifsd_debug("fid %llu, setattr done\n", fid);
	}

out:
	if (name)
		path_put(&path);
	fp_put(fp);
	return err;
}

/**
 * smb_vfs_getattr() - vfs helper for smb getattr
 * @sess:	session
 * @fid:	file id of open file
 * @attrs:	inode attributes
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_getattr(struct cifsd_sess *sess, uint64_t fid,
		struct kstat *stat)
{
	struct file *filp;
	struct cifsd_file *fp;
	int err;

	fp = get_id_from_fidtable(sess, fid);
	if (!fp) {
		cifsd_err("failed to get filp for fid %llu\n", fid);
		return -ENOENT;
	}

	filp = fp->filp;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
	err = vfs_getattr(&filp->f_path, stat, STATX_BASIC_STATS,
		AT_STATX_SYNC_AS_STAT);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
	err = vfs_getattr(&filp->f_path, stat);
#else
	err = vfs_getattr(filp->f_path.mnt, filp->f_path.dentry, stat);
#endif
	if (err)
		cifsd_err("getattr failed for fid %llu, err %d\n", fid, err);
	fp_put(fp);
	return err;
}

/**
 * smb_vfs_fsync() - vfs helper for smb fsync
 * @sess:	session
 * @fid:	file id of open file
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_fsync(struct cifsd_sess *sess, uint64_t fid, uint64_t p_id)
{
	struct cifsd_file *fp;
	int err;

	fp = get_id_from_fidtable(sess, fid);
	if (!fp) {
		cifsd_err("failed to get filp for fid %llu\n", fid);
		return -ENOENT;
	}

	if (fp->is_durable && fp->persistent_id != p_id) {
		cifsd_err("persistent id mismatch : %llu, %llu\n",
				fp->persistent_id, p_id);
		return -ENOENT;
	}

	err = vfs_fsync(fp->filp, 0);
	if (err < 0)
		cifsd_err("smb fsync failed, err = %d\n", err);
	fp_put(fp);

	return err;
}

/**
 * smb_vfs_remove_file() - vfs helper for smb rmdir or unlink
 * @name:	absolute directory or file name
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_remove_file(char *name)
{
	struct path parent;
	struct dentry *dir, *dentry;
	char *last;
	int err = -ENOENT;

	last = strrchr(name, '/');
	if (last && last[1] != '\0') {
		*last = '\0';
		last++;
	}
	else {
		cifsd_debug("can't get last component in path %s\n", name);
		return -ENOENT;
	}

	err = kern_path(name, LOOKUP_FOLLOW | LOOKUP_DIRECTORY, &parent);
	if (err) {
		cifsd_debug("can't get %s, err %d\n", name, err);
		return err;
	}

	dir = parent.dentry;
	if (!dir->d_inode)
		goto out;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0)
	inode_lock_nested(dir->d_inode, I_MUTEX_PARENT);
#else
	mutex_lock_nested(&dir->d_inode->i_mutex, I_MUTEX_PARENT);
#endif
	dentry = lookup_one_len(last, dir, strlen(last));
	if (IS_ERR(dentry)) {
		err = PTR_ERR(dentry);
		cifsd_debug("%s: lookup failed, err %d\n", last, err);
		goto out_err;
	}

	if (!dentry->d_inode || !dentry->d_inode->i_nlink) {
		dput(dentry);
		err = -ENOENT;
		goto out_err;
	}

	if (S_ISDIR(dentry->d_inode->i_mode)) {
		err = vfs_rmdir(dir->d_inode, dentry);
		if (err && err != -ENOTEMPTY)
			cifsd_debug("%s: rmdir failed, err %d\n", name, err);
	} else {
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
		err = vfs_unlink(dir->d_inode, dentry, NULL);
#else
		err = vfs_unlink(dir->d_inode, dentry);
#endif
		if (err)
			cifsd_debug("%s: unlink failed, err %d\n", name, err);
	}

	dput(dentry);
out_err:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0)
	inode_unlock(dir->d_inode);
#else
	mutex_unlock(&dir->d_inode->i_mutex);
#endif
out:
	path_put(&parent);
	return err;
}

/**
 * smb_vfs_link() - vfs helper for creating smb hardlink
 * @oldname:	source file name
 * @newname:	hardlink name
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_link(const char *oldname, const char *newname)
{
	struct path oldpath, newpath;
	struct dentry *dentry;
	int err;

	err = kern_path(oldname, LOOKUP_FOLLOW, &oldpath);
	if (err) {
		cifsd_err("cannot get linux path for %s, err = %d\n",
				oldname, err);
		goto out1;
	}

	dentry = kern_path_create(AT_FDCWD, newname, &newpath,
			LOOKUP_FOLLOW & LOOKUP_REVAL);
	if (IS_ERR(dentry)) {
		err = PTR_ERR(dentry);
		cifsd_err("path create err for %s, err %d\n", newname, err);
		goto out2;
	}

	err = -EXDEV;
	if (oldpath.mnt != newpath.mnt) {
		cifsd_err("vfs_link failed err %d\n", err);
		goto out3;
	}

#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
	err = vfs_link(oldpath.dentry, newpath.dentry->d_inode, dentry, NULL);
#else
	err = vfs_link(oldpath.dentry, newpath.dentry->d_inode, dentry);
#endif
	if (err)
		cifsd_debug("vfs_link failed err %d\n", err);

out3:
	done_path_create(&newpath, dentry);
out2:
	path_put(&oldpath);

out1:
	return err;
}

/**
 * smb_vfs_symlink() - vfs helper for creating smb symlink
 * @name:	source file name
 * @symname:	symlink name
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_symlink(const char *name, const char *symname)
{
	struct path path;
	struct dentry *dentry;
	int err;

	dentry = kern_path_create(AT_FDCWD, symname, &path, 0);
	if (IS_ERR(dentry)) {
		err = PTR_ERR(dentry);
		cifsd_err("path create failed for %s, err %d\n", name, err);
		return err;
	}

	err = vfs_symlink(dentry->d_parent->d_inode, dentry, name);
	if (err && (err != -EEXIST || err != -ENOSPC))
		cifsd_debug("failed to create symlink, err %d\n", err);

	done_path_create(&path, dentry);

	return err;
}

/**
 * smb_vfs_readlink() - vfs helper for reading value of symlink
 * @path:	path of symlink
 * @buf:	destination buffer for symlink value
 * @lenp:	destination buffer length
 *
 * Return:	symlink value length on success, otherwise error
 */
int smb_vfs_readlink(struct path *path, char *buf, int lenp)
{
	struct inode *inode;
	mm_segment_t old_fs;
	int err;

	if (!path)
		return -ENOENT;

	inode = path->dentry->d_inode;
	if (!S_ISLNK(inode->i_mode))
		return -EINVAL;

	old_fs = get_fs();
	set_fs(KERNEL_DS);
	err = inode->i_op->readlink(path->dentry, (char __user *)buf, lenp);
	set_fs(old_fs);
	if (err < 0)
		cifsd_err("readlink failed, err = %d\n", err);

	return err;
}

/**
 * smb_vfs_rename() - vfs helper for smb rename
 * @sess:		session
 * @abs_oldname:	old filename
 * @abs_newname:	new filename
 * @oldfid:		file id of old file
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_rename(struct cifsd_sess *sess, char *abs_oldname,
		char *abs_newname, uint64_t oldfid)
{
	struct path oldpath_p, newpath_p;
	struct dentry *dold, *dnew, *dold_p, *dnew_p, *trap, *child_de;
	char *oldname = NULL, *newname = NULL;
	struct file *filp = NULL;
	struct cifsd_file *fp = NULL;
	int err;

	if (abs_oldname) {
		/* normal case: rename with source filename */
		oldname = strrchr(abs_oldname, '/');
		if (oldname && oldname[1] != '\0') {
			*oldname = '\0';
			oldname++;
		}
		else {
			cifsd_err("can't get last component in path %s\n",
					abs_oldname);
			return -ENOENT;
		}

		err = kern_path(abs_oldname, LOOKUP_FOLLOW | LOOKUP_DIRECTORY,
				&oldpath_p);
		if (err) {
			cifsd_err("cannot get linux path for %s, err %d\n",
					abs_oldname, err);
			return -ENOENT;
		}
		dold_p = oldpath_p.dentry;

		newname = strrchr(abs_newname, '/');
		if (newname && newname[1] != '\0') {
			*newname = '\0';
			newname++;
		}
		else {
			cifsd_err("can't get last component in path %s\n",
					abs_newname);
			err = -ENOMEM;
			goto out1;
		}

		err = kern_path(abs_newname, LOOKUP_FOLLOW | LOOKUP_DIRECTORY,
				&newpath_p);
		if (err) {
			cifsd_err("cannot get linux path for %s, err = %d\n",
					abs_newname, err);
			goto out1;
		}
		dnew_p = newpath_p.dentry;
	} else {
		/* rename by fid of source file instead of source filename */
		fp = get_id_from_fidtable(sess, oldfid);
		if (!fp) {
			cifsd_err("can't find filp for fid %llu\n", oldfid);
			return -ENOENT;
		}

		filp = fp->filp;
		dold_p = filp->f_path.dentry->d_parent;

		newname = strrchr(abs_newname, '/');
		if (newname && newname[1] != '\0') {
			*newname = '\0';
			newname++;
		}
		else {
			cifsd_err("can't get last component in path %s\n",
					abs_newname);
			return -ENOMEM;
		}

		err = kern_path(abs_newname, LOOKUP_FOLLOW | LOOKUP_DIRECTORY,
				&newpath_p);
		if (err) {
			cifsd_err("cannot get linux path for %s, err = %d\n",
					abs_newname, err);
			return err;
		}
		dnew_p = newpath_p.dentry;
	}

	cifsd_debug("oldname %s, newname %s\n", oldname, newname);
	trap = lock_rename(dold_p, dnew_p);
	if (abs_oldname) {
		dold = lookup_one_len(oldname, dold_p, strlen(oldname));
		err = PTR_ERR(dold);
		if (IS_ERR(dold)) {
			cifsd_err("%s lookup failed with error = %d\n",
					oldname, err);
			goto out2;
		}
	} else {
		dold = filp->f_path.dentry;
		dget(dold);
	}

	spin_lock(&dold->d_lock);
	list_for_each_entry(child_de, &dold->d_subdirs, d_child) {
		struct cifsd_file *child_fp;

		if (!child_de->d_inode)
			continue;

		child_fp = find_fp_using_inode(child_de->d_inode);
		if (child_fp) {
			cifsd_debug("not allow to rename dir with opening sub file\n");
			err = -ENOTEMPTY;
			spin_unlock(&dold->d_lock);
			goto out3;
		}
	}
	spin_unlock(&dold->d_lock);

	err = -ENOENT;
	if (!dold->d_inode)
		goto out3;
	err = -EINVAL;
	if (dold == trap)
		goto out3;

	dnew = lookup_one_len(newname, dnew_p, strlen(newname));
	err = PTR_ERR(dnew);
	if (IS_ERR(dnew)) {
		cifsd_err("%s lookup failed with error = %d\n",
				newname, err);
		goto out3;
	}

	err = -ENOTEMPTY;
	if (dnew == trap)
		goto out4;

#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
	err = vfs_rename(dold_p->d_inode, dold, dnew_p->d_inode, dnew, NULL, 0);
#else
	err = vfs_rename(dold_p->d_inode, dold, dnew_p->d_inode, dnew);
#endif
	if (err)
		cifsd_err("vfs_rename failed err %d\n", err);
out4:
	dput(dnew);
out3:
	dput(dold);
out2:
	unlock_rename(dold_p, dnew_p);
	path_put(&newpath_p);
out1:
	if (abs_oldname)
		path_put(&oldpath_p);

	fp_put(fp);
	return err;
}

/**
 * smb_vfs_truncate() - vfs helper for smb file truncate
 * @sess:	session
 * @name:	old filename
 * @fid:	file id of old file
 * @size:	truncate to given size
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_truncate(struct cifsd_sess *sess, const char *name,
		uint64_t fid, loff_t size)
{
	struct path path;
	struct file *filp;
	int err = 0;
	struct cifsd_file *fp;
	struct inode *inode;

	if (name) {
		err = kern_path(name, 0, &path);
		if (err) {
			cifsd_err("cannot get linux path for %s, err %d\n",
					name, err);
			return err;
		}
		err = vfs_truncate(&path, size);
		if (err)
			cifsd_err("truncate failed for %s err %d\n",
					name, err);
		path_put(&path);
	} else {
		fp = get_id_from_fidtable(sess, fid);
		if (!fp) {
			cifsd_err("failed to get filp for fid %llu\n", fid);
			return -ENOENT;
		}

		filp = fp->filp;
		if (oplocks_enable) {
			/* Do we need to break any of a levelII oplock? */
			mutex_lock(&ofile_list_lock);
			smb_break_all_levII_oplock(sess->conn, fp, NULL, 1);
			mutex_unlock(&ofile_list_lock);
		} else {
			inode = file_inode(filp);
			if (size < inode->i_size) {
				err = check_lock_range(filp, size,
					inode->i_size - 1, WRITE);
			} else {
				err = check_lock_range(filp, inode->i_size,
					size - 1, WRITE);
			}

			if (err) {
				cifsd_err("failed due to lock\n");
				return -EAGAIN;
			}
		}
		err = vfs_truncate(&filp->f_path, size);
		if (err)
			cifsd_err("truncate failed for fid %llu err %d\n",
					fid, err);
		fp_put(fp);
	}

	return err;
}

/**
 * smb_vfs_listxattr() - vfs helper for smb list extended attributes
 * @dentry:	dentry of file for listing xattrs
 * @list:	destination buffer
 * @size:	destination buffer length
 *
 * Return:	xattr list length on success, otherwise error
 */
ssize_t smb_vfs_listxattr(struct dentry *dentry, char **list, int size)
{
	ssize_t err;
	char *vlist = NULL;

	if (size) {
		if (size > XATTR_LIST_MAX)
			size = XATTR_LIST_MAX;
		vlist = vmalloc(size);
		if (!vlist)
			return -ENOMEM;
	}

	*list = vlist;
	err = vfs_listxattr(dentry, vlist, size);
	if (err == -ERANGE) {
		/* The file system tried to returned a list bigger
		   than XATTR_LIST_MAX bytes. Not possible. */
		err = -E2BIG;
		cifsd_debug("listxattr failed\n");
	}

	return err;
}

/**
 * smb_vfs_getxattr() - vfs helper for smb get extended attributes value
 * @dentry:	dentry of file for getting xattrs
 * @xattr_name:	name of xattr name to query
 * @xattr_buf:	destination buffer xattr value
 * @flags: if 0, return stored xattr's length only
 *
 * Return:	read xattr value length on success, otherwise error
 */
ssize_t smb_vfs_getxattr(struct dentry *dentry, char *xattr_name,
	char **xattr_buf, int flags)
{
	ssize_t xattr_len;
	char *buf;

	xattr_len = vfs_getxattr(dentry, xattr_name, NULL, 0);
	if (xattr_len <= 0)
		return xattr_len;

	if (!flags)
		return xattr_len;

	buf = alloc_data_mem(xattr_len);
	if (!buf)
		return -ENOMEM;

	xattr_len = vfs_getxattr(dentry, xattr_name, (void *)buf,
		xattr_len);
	if (xattr_len < 0)
		cifsd_debug("getxattr failed, ret %zd\n", xattr_len);
	else
		*xattr_buf = buf;

	return xattr_len;
}

/**
 * smb_vfs_setxattr() - vfs helper for smb set extended attributes value
 * @filename:	file name
 * @fpath:	path of file for setxattr
 * @name:	xattr name for setxattr
 * @value:	xattr value to set
 * @size:	size of xattr value
 * @flags:	destination buffer length
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_setxattr(const char *filename, struct path *fpath, const char *name,
		const void *value, size_t size, int flags)
{
	struct path path;
	int err;

	if (filename) {
		err = kern_path(filename, 0, &path);
		if (err) {
			cifsd_debug("cannot get linux path %s, err %d\n",
					filename, err);
			return err;
		}
		err = vfs_setxattr(path.dentry, name, value, size, flags);
		if (err)
			cifsd_debug("setxattr failed, err %d\n", err);
		path_put(&path);
	} else {
		err = vfs_setxattr(fpath->dentry, name, value, size, flags);
		if (err)
			cifsd_debug("setxattr failed, err %d\n", err);
	}

	return err;
}

int smb_vfs_truncate_xattr(struct dentry *dentry)
{
	char *name, *xattr_list = NULL;
	ssize_t xattr_list_len;
	int err = 0;

	xattr_list_len = smb_vfs_listxattr(dentry, &xattr_list,
		XATTR_LIST_MAX);
	if (xattr_list_len < 0) {
		goto out;
	} else if (!xattr_list_len) {
		cifsd_debug("empty xattr in the file\n");
		goto out;
	}

	for (name = xattr_list; name - xattr_list < xattr_list_len;
			name += strlen(name) + 1) {
		cifsd_debug("%s, len %zd\n", name, strlen(name));

		if (!strncmp(&name[XATTR_USER_PREFIX_LEN], STREAM_PREFIX,
					STREAM_PREFIX_LEN))
			continue;

		err = vfs_removexattr(dentry, name);
		if (err)
			cifsd_err("remove xattr failed : %s\n", name);
	}
out:
	if (xattr_list)
		vfree(xattr_list);

	return err;
}

int smb_vfs_truncate_stream_xattr(struct dentry *dentry)
{
	char *name, *xattr_list = NULL;
	ssize_t xattr_list_len;
	int err = 0;

	xattr_list_len = smb_vfs_listxattr(dentry, &xattr_list,
		XATTR_LIST_MAX);
	if (xattr_list_len < 0) {
		goto out;
	} else if (!xattr_list_len) {
		cifsd_debug("empty xattr in the file\n");
		goto out;
	}

	for (name = xattr_list; name - xattr_list < xattr_list_len;
			name += strlen(name) + 1) {
		cifsd_debug("%s, len %zd\n", name, strlen(name));

		if (strncmp(&name[XATTR_USER_PREFIX_LEN], STREAM_PREFIX,
					STREAM_PREFIX_LEN))
			continue;

		err = vfs_removexattr(dentry, name);
		if (err)
			cifsd_err("remove xattr failed : %s\n", name);
	}
out:
	if (xattr_list)
		vfree(xattr_list);

	return err;
}

/**
 * smb_vfs_set_fadvise() - convert smb IO caching options to linux options
 * @filp:	file pointer for IO
 * @options:	smb IO options
 */
void smb_vfs_set_fadvise(struct file *filp, int option)
{
	struct address_space *mapping;
	mapping = filp->f_mapping;

	if (!option || !mapping)
		return;

	if (option & FILE_WRITE_THROUGH_LE)
		filp->f_flags |= O_SYNC;
/*
	 * TODO : need to add special handling for Direct I/O.
	 * Direct I/O relies on MM Context of the "current" process
	 * to retrieve the pages corresponding to the user address
	 * do_direct_IO()->dio_get_page()->
				dio_refill_pages()->get_user_pages_fast()
	 * struct mm_struct *mm = current->mm;
	 * All work items in CIFSD are handled through default "kworker"
	 * - which do not have any MM Context.
	 * To handle Direct I/O will need to create another thread
	 * in kernel with MM context and redirect all direct I/O calls to
	 * thread. Since, this is Server and direct I/O not bottleneck.
	 * So, making default READ path to be buffered in all sequences
	 * (clearing direct IO flag).
	else if (option & FILE_NO_INTERMEDIATE_BUFFERING_LE &&
		 filp->f_mapping->a_ops->direct_IO)
		filp->f_flags |= O_DIRECT;
*/
	else if (option & FILE_SEQUENTIAL_ONLY_LE) {
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
		filp->f_ra.ra_pages = inode_to_bdi(mapping->host)->ra_pages * 2;
#else
		filp->f_ra.ra_pages = mapping->backing_dev_info->ra_pages * 2;
#endif
		spin_lock(&filp->f_lock);
		filp->f_mode &= ~FMODE_RANDOM;
		spin_unlock(&filp->f_lock);
	} else if (option & FILE_RANDOM_ACCESS_LE) {
		spin_lock(&filp->f_lock);
		filp->f_mode |= FMODE_RANDOM;
		spin_unlock(&filp->f_lock);
	}
}

/**
 * smb_vfs_lock() - vfs helper for smb file locking
 * @filp:	the file to apply the lock to
 * @cmd:	type of locking operation (F_SETLK, F_GETLK, etc.)
 * @flock:	The lock to be applied
 *
 * Return:	0 on success, otherwise error
 */
int smb_vfs_lock(struct file *filp, int cmd,
			struct file_lock *flock)
{
	cifsd_debug("%s: calling vfs_lock_file\n", __func__);
	return vfs_lock_file(filp, cmd, flock, NULL);
}

/**
 * check_lock_range() - vfs helper for smb byte range file locking
 * @filp:	the file to apply the lock to
 * @start:	lock start byte offset
 * @end:	lock end byte offset
 * @type:	byte range type read/write
 *
 * Return:	0 on success, otherwise error
 */
int check_lock_range(struct file *filp, loff_t start, loff_t end,
		unsigned char type)
{
	struct file_lock *flock;
	struct file_lock_context *ctx = file_inode(filp)->i_flctx;
	int error = 0;

	if (!ctx || list_empty_careful(&ctx->flc_posix))
		return 0;

	list_for_each_entry(flock, &ctx->flc_posix, fl_list) {
		/* check conflict locks */
		if (flock->fl_end >= start && end >= flock->fl_start) {
			if (flock->fl_type == F_RDLCK) {
				if (type == WRITE) {
					cifsd_err("not allow write by shared lock\n");
					error = 1;
					goto out;
				}
			} else if (flock->fl_type == F_WRLCK) {
				/* check owner in lock */
				if (flock->fl_file != filp) {
					error = 1;
					cifsd_err("not allow rw access by exclusive lock from other opens\n");
					goto out;
				}
			}
		}
	}
out:
	return error;
}

int smb_vfs_readdir(struct file *file, filldir_t filler,
			struct smb_readdir_data *rdata)
{
	int err;

#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
	err = iterate_dir(file, &rdata->ctx);
#else
	err = vfs_readdir(file, smb_filldir, rdata);
#endif

	return err;
}

int smb_vfs_alloc_size(struct file *filp, loff_t len)
{
	return vfs_fallocate(filp, FALLOC_FL_KEEP_SIZE, 0, len);
}

int smb_vfs_remove_xattr(struct path *path, char *field_name)
{
	return vfs_removexattr(path->dentry, field_name);
}

int smb_vfs_unlink(struct dentry *dir, struct dentry *dentry)
{
	int err = 0;

	dget(dentry);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0)
	inode_lock(dir->d_inode);
#else
	mutex_lock(&dir->d_inode->i_mutex);
#endif
	if (!dentry->d_inode || !dentry->d_inode->i_nlink) {
		err = -ENOENT;
		goto out;
	}

	if (S_ISDIR(dentry->d_inode->i_mode))
		err = vfs_rmdir(dir->d_inode, dentry);
	else
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
		err = vfs_unlink(dir->d_inode, dentry, NULL);
#else
	err = vfs_unlink(dir->d_inode, dentry);
#endif

out:
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 2, 0)
	inode_unlock(dir->d_inode);
#else
	mutex_unlock(&dir->d_inode->i_mutex);
#endif
	dput(dentry);
	if (err)
		cifsd_debug("failed to delete, err %d\n", err);

	return err;
}

/*
 * get_logical_sector_size() - get logical sector size from inode
 * @inode: inode
 *
 * Return: logical sector size
 */
unsigned short get_logical_sector_size(struct inode *inode)
{
	struct request_queue *q;
	unsigned short ret_val;

	ret_val = 512;
	q = inode->i_sb->s_bdev->bd_disk->queue;

	if (q && q->limits.logical_block_size)
		ret_val = q->limits.logical_block_size;

	return ret_val;
}

/*
 * get_smb2_sector_size() - get fs sector sizes
 * @inode: inode
 * @fs_ss: fs sector size struct
 */
void get_smb2_sector_size(struct inode *inode,
	struct smb2_fs_sector_size *fs_ss)
{
	struct request_queue *q;

	q = inode->i_sb->s_bdev->bd_disk->queue;
	fs_ss->logical_sector_size = 512;
	fs_ss->physical_sector_size = 512;
	fs_ss->optimal_io_size = 512;

	if (q) {
		if (q->limits.logical_block_size)
			fs_ss->logical_sector_size =
				q->limits.logical_block_size;
		if (q->limits.physical_block_size)
			fs_ss->physical_sector_size =
				q->limits.physical_block_size;
		if (q->limits.io_opt)
			fs_ss->optimal_io_size = q->limits.io_opt;
	}
}
