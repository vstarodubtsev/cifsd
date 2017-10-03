/*
 *   fs/cifsd/smb1pdu.c
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
#include <linux/math64.h>
#include <linux/fs.h>
#include <linux/posix_acl_xattr.h>
#include "glob.h"
#include "export.h"
#include "smb1pdu.h"
#include "oplock.h"

/*for shortname implementation */
static const char basechars[43] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_-!@#$%";
#define MANGLE_BASE       (sizeof(basechars)/sizeof(char)-1)
#define MAGIC_CHAR '~'
#define PERIOD '.'
#define mangle(V) ((char)(basechars[(V) % MANGLE_BASE]))

/**
 * smb_get_shortname() - get shortname from long filename
 * @conn:	TCP server instance of connection
 * @longname:	source long filename
 * @shortname:	destination short filename
 *
 * Return:	shortname length or 0 when source long name is '.' or '..'
 * TODO: Though this function comforms the restriction of 8.3 Filename spec,
 * but the result is different with Windows 7's one. need to check.
 */
int smb_get_shortname(struct connection *conn, char *longname,
		char *shortname)
{
	char *p, *sp;
	char base[9], extension[4];
	char out[13] = {0};
	int baselen = 0;
	int extlen = 0, len = 0;
	unsigned int csum = 0;
	unsigned char *ptr;
	bool dot_present = true;

	p = longname;
	if ((*p == '.') || (!(strcmp(p, "..")))) {
		/*no mangling required */
		shortname = NULL;
		return 0;
	}
	p = strrchr(longname, '.');
	if (p == longname) { /*name starts with a dot*/
		sp = "___";
		memcpy(extension, sp, 3);
		extension[3] = '\0';
	} else {
		if (p != NULL) {
			p++;
			while (*p && extlen < 3) {
				if (*p != '.')
					extension[extlen++] = toupper(*p);
				p++;
			}
			extension[extlen] = '\0';
		} else
			dot_present = false;
	}

	p = longname;
	if (*p == '.')
		*p++ = 0;
	while (*p && (baselen < 5)) {
		if (*p != '.')
			base[baselen++] = toupper(*p);
		p++;
	}

	base[baselen] = MAGIC_CHAR;
	memcpy(out, base, baselen+1);

	ptr = longname;
	len = strlen(longname);
	for (; len > 0; len--, ptr++)
		csum += *ptr;

	csum = csum % (MANGLE_BASE * MANGLE_BASE);
	out[baselen+1] = mangle(csum/MANGLE_BASE);
	out[baselen+2] = mangle(csum);
	out[baselen+3] = PERIOD;

	if (dot_present)
		memcpy(&out[baselen+4], extension, 4);
	else
		out[baselen+4] = '\0';
	smbConvertToUTF16((__le16 *)shortname, out, PATH_MAX,
			conn->local_nls, 0);
	len = strlen(out) * 2;
	return len;
}

/**
 * smb_NTtimeToUnix() - convert NTFS time to unix style time format
 * @ntutc:	NTFS style time
 *
 * Convert the NT UTC (based 1601-01-01, in hundred nanosecond units)
 * into Unix UTC (based 1970-01-01, in seconds).
 *
 * Return:      timespec containing unix style time
 */
struct timespec
smb_NTtimeToUnix(__le64 ntutc)
{
	struct timespec ts;
	/* BB what about the timezone? BB */

	/* Subtract the NTFS time offset, then convert to 1s intervals. */
	/* this has been taken from cifs, ntfs code */
	u64 t;

	t = le64_to_cpu(ntutc) - NTFS_TIME_OFFSET;
	ts.tv_nsec = do_div(t, 10000000) * 100;
	ts.tv_sec = t;
	return ts;
}

/**
 * get_smb_cmd_val() - get smb command value from smb header
 * @smb_work:	smb work containing smb header
 *
 * Return:      smb command value
 */
int get_smb_cmd_val(struct smb_work *smb_work)
{
	struct smb_hdr *rcv_hdr = (struct smb_hdr *)smb_work->buf;
	return rcv_hdr->Command;
}

/**
 * is_smbreq_unicode() - check if the smb command is request is unicode or not
 * @hdr:	pointer to smb_hdr in the the request part
 *
 * Return: check flags and return true if request is unicode, else false
 */
static inline int is_smbreq_unicode(struct smb_hdr *hdr)
{
	return hdr->Flags2 & SMBFLG2_UNICODE;
}

/**
 * set_smb_rsp_status() - set error type in smb response header
 * @smb_work:	smb work containing smb response header
 * @err:	error code to set in response
 */
void set_smb_rsp_status(struct smb_work *smb_work, unsigned int err)
{
	struct smb_hdr *rsp_hdr = (struct smb_hdr *) smb_work->rsp_buf;
	rsp_hdr->Status.CifsError = err;
}

/**
 * init_smb_rsp_hdr() - initialize smb response header
 * @smb_work:	smb work containing smb request
 *
 * Return:      0 on success, otherwise -EINVAL
 */
int init_smb_rsp_hdr(struct smb_work *smb_work)
{
	struct connection *conn = smb_work->conn;
	struct smb_hdr *rsp_hdr;
	struct smb_hdr *rcv_hdr = (struct smb_hdr *)smb_work->buf;

	rsp_hdr = (struct smb_hdr *) smb_work->rsp_buf;
	memset(rsp_hdr, 0, sizeof(struct smb_hdr) + 2);

	/* remove 4 byte direct TCP header, add 1 byte wc and 2 byte bcc */
	rsp_hdr->smb_buf_length = cpu_to_be32(HEADER_SIZE(conn) - 4 + 3);
	memcpy(rsp_hdr->Protocol, rcv_hdr->Protocol, 4);
	rsp_hdr->Command = rcv_hdr->Command;

	/*
	 * Message is response. Other bits are obsolete.
	 */
	rsp_hdr->Flags = (SMBFLG_RESPONSE);

	/*
	 * Lets assume error code are NTLM. True for CIFS and windows 7
	 */
	rsp_hdr->Flags2 = rcv_hdr->Flags2;
	rsp_hdr->PidHigh = rcv_hdr->PidHigh;
	rsp_hdr->Pid = rcv_hdr->Pid;
	rsp_hdr->Mid = rcv_hdr->Mid;
	rsp_hdr->WordCount = 0;

	/* verfiy if TID and UID are correct */
	if (conn->tcp_status == CifsGood && rcv_hdr->Uid != conn->vuid &&
			rcv_hdr->Command != SMB_COM_ECHO) {
		cifsd_err("wrong Uid sent by client\n");
		return -EINVAL;
	}
	/* We can do the above test because we have set maxVCN as 1 */
	rsp_hdr->Uid = rcv_hdr->Uid;
	rsp_hdr->Tid = rcv_hdr->Tid;
	return 0;
}

/**
 * smb_allocate_rsp_buf() - allocate response buffer for a command
 * @smb_work:	smb work containing smb request
 *
 * Return:      0 on success, otherwise -ENOMEM
 */
int smb_allocate_rsp_buf(struct smb_work *smb_work)
{
	struct smb_hdr *hdr = (struct smb_hdr *)smb_work->buf;
	unsigned char cmd = hdr->Command;
	bool need_large_buf = false;

	if (cmd == SMB_COM_TRANSACTION2) {
		TRANSACTION2_QPI_REQ *req =
			(TRANSACTION2_QPI_REQ *)smb_work->buf;
		u16 sub_cmd = le16_to_cpu(req->SubCommand);
		u16 infolevel = le16_to_cpu(req->InformationLevel);

		if ((sub_cmd == TRANS2_FIND_FIRST) ||
				(sub_cmd == TRANS2_FIND_NEXT) ||
				(sub_cmd == TRANS2_QUERY_PATH_INFORMATION &&
				 (infolevel == SMB_QUERY_FILE_UNIX_LINK ||
				  infolevel == SMB_QUERY_POSIX_ACL ||
				  infolevel == SMB_INFO_QUERY_ALL_EAS)))
			need_large_buf = true;
	}

	if (cmd == SMB_COM_TRANSACTION)
		need_large_buf = true;

	if (cmd == SMB_COM_ECHO) {
		int resp_size;
		ECHO_REQ *req = (ECHO_REQ *)smb_work->buf;

		/* size of ECHO_RSP + Bytecount - Size of Data in ECHO_RSP */
		resp_size = sizeof(ECHO_RSP) + req->ByteCount - 1;
		if (resp_size > MAX_CIFS_SMALL_BUFFER_SIZE)
			need_large_buf = true;
	}

	if (need_large_buf) {
		smb_work->rsp_large_buf = true;
		smb_work->rsp_buf = mempool_alloc(cifsd_rsp_poolp, GFP_NOFS);
	} else {
		smb_work->rsp_large_buf = false;
		smb_work->rsp_buf = mempool_alloc(cifsd_sm_rsp_poolp,
								GFP_NOFS);
	}

	if (smb_work->rsp_buf == NULL) {
		cifsd_err("failed to alloc response buffer, large_buf %d\n",
				smb_work->rsp_large_buf);
		return -ENOMEM;
	}

	return 0;
}

/**
 * andx_request_buffer() - return pointer to matching andx command
 * @smb_work:	buffer containing smb request
 * @command:	match next command with this command
 *
 * Return:      pointer to matching command buffer on success, otherwise NULL
 */
char *andx_request_buffer(char *buf, int command)
{
	struct andx_block *andx_ptr = (struct andx_block *)(buf +
					sizeof(struct smb_hdr) - 1);
	struct andx_block *next;

	while (andx_ptr->AndXCommand != SMB_NO_MORE_ANDX_COMMAND) {
		next = (struct andx_block *)(buf + 4 + andx_ptr->AndXOffset);
		if (andx_ptr->AndXCommand == command)
			return (char *)next;
		andx_ptr = next;
	}
	return NULL;
}

/**
 * andx_response_buffer() - return pointer to andx response buffer
 * @buf:	buffer containing smb request
 *
 * Return:      pointer to andx command response on success, otherwise NULL
 */
char *andx_response_buffer(char *buf)
{
	int pdu_length = get_rfc1002_length(buf);
	return buf + 4 + pdu_length;
}

/**
 * extract_sharename() - get share name from tree connect request
 * @treename:	buffer containing tree name and share name
 *
 * Return:      share name on success, otherwise error
 */
char *extract_sharename(const char *treename)
{
	const char *src;
	char *delim, *dst;
	int len;

	/* skip double chars at the beginning */
	src = treename + 2;

	/* share name is always preceded by '\\' now */
	delim = strchr(src, '\\');
	if (!delim)
		return ERR_PTR(-EINVAL);
	delim++;
	len = strlen(delim);

	/* caller has to free the memory */
	dst = kstrndup(delim, len, GFP_KERNEL);
	if (!dst)
		return ERR_PTR(-ENOMEM);

	return dst;
}

/**
 * smb_check_user_session() - check for valid session for a user
 * @smb_work:	smb work containing smb request buffer
 *
 * Return:      0 on success, otherwise error
 */
int smb_check_user_session(struct smb_work *smb_work)
{
	struct smb_hdr *req_hdr = (struct smb_hdr *)smb_work->buf;
	struct connection *conn = smb_work->conn;
	struct cifsd_sess *sess;
	struct list_head *tmp;
	int rc;
	unsigned int cmd = conn->ops->get_cmd_val(smb_work);

	smb_work->sess = NULL;

	if (cmd == SMB_COM_NEGOTIATE || cmd == SMB_COM_SESSION_SETUP_ANDX)
		return 0;

	if (conn->tcp_status != CifsGood)
		return -EINVAL;

	if (conn->sess_count == 0) {
		cifsd_debug("NO sessions registered\n");
		return 0;
	}

	rc = -EINVAL;
	list_for_each(tmp, &conn->cifsd_sess) {
		sess = list_entry(tmp, struct cifsd_sess, cifsd_ses_list);
		if (sess->usr->vuid == req_hdr->Uid && sess->valid) {
			smb_work->sess = sess;
			rc = 1;
			break;
		}
	}

	if (!smb_work->sess)
		cifsd_debug("Invalid user session, Uid %u\n", req_hdr->Uid);
	return rc;
}

/**
 * smb_get_cifsd_tcon() - get tree connection information for a tree id
 * @sess:	session containing tree list
 * @tid:	match tree connection with tree id
 *
 * Return:      matching tree connection on success, otherwise error
 */
int smb_get_cifsd_tcon(struct smb_work *smb_work)
{
	struct cifsd_tcon *tcon;
	struct list_head *tmp;
	struct smb_hdr *req_hdr = (struct smb_hdr *)smb_work->buf;
	int rc = -1;

	smb_work->tcon = NULL;
	if (!smb_work->sess->tcon_count) {
		cifsd_debug("NO tree connected\n");
		return 0;
	}

	if (smb_work->conn->ops->get_cmd_val(smb_work) ==
		SMB_COM_TREE_CONNECT_ANDX) {
		cifsd_debug("skip to check tree connect request\n");
		return 0;
	}

	list_for_each(tmp, &smb_work->sess->tcon_list) {
		tcon = list_entry(tmp, struct cifsd_tcon, tcon_list);
		if (tcon->share->tid == le16_to_cpu(req_hdr->Tid)) {
			rc = 1;
			smb_work->tcon = tcon;
			break;
		}
	}

	if (rc < 0)
		cifsd_debug("Invalid tid %d\n", req_hdr->Tid);

	return rc;
}

/**
 * smb_session_disconnect() - LOGOFF request handler
 * @smb_work:	smb work containing log off request buffer
 *
 * Return:      0 on success, otherwise error
 */
int smb_session_disconnect(struct smb_work *smb_work)
{
	struct connection *conn = smb_work->conn;
	struct cifsd_sess *sess = smb_work->sess;
	struct list_head *tmp, *t;

	/* Got a valid session, set connection state */
	WARN_ON(conn->sess_count != 1);
	WARN_ON(sess->conn != conn);

	/* setting CifsExiting here may race with start_tcp_sess */
	conn->tcp_status = CifsNeedReconnect;

	/*
	 * We cannot discard session in case some request are already running.
	 * Need to wait for them to finish and update req_running.
	 */
	wait_event(conn->req_running_q,
			atomic_read(&conn->req_running) == 1);

	/* free all tcons */
	list_for_each_safe(tmp, t, &sess->tcon_list) {
		struct cifsd_tcon *tcon = list_entry(tmp,
						struct cifsd_tcon, tcon_list);
		list_del(&tcon->tcon_list);
		sess->tcon_count--;
		kfree(tcon);
	}

	WARN_ON(sess->tcon_count != 0);

	/* free all sessions, we have just 1 */
	list_del(&sess->cifsd_ses_list);
	list_del(&sess->cifsd_ses_global_list);
	destroy_fidtable(sess);
	kfree(sess);
	smb_work->sess = NULL;

	conn->sess_count--;
	/* let start_tcp_sess free conn info now */
	conn->tcp_status = CifsExiting;
	return 0;
}

/**
 * smb_session_disconnect() - tree disconnect request handler
 * @smb_work:	smb work containing tree disconnect request buffer
 *
 * Return:      0 on success, otherwise error
 */
int smb_tree_disconnect(struct smb_work *smb_work)
{
	struct smb_hdr *req_hdr = (struct smb_hdr *)smb_work->buf;
	struct smb_hdr *rsp_hdr = (struct smb_hdr *)smb_work->rsp_buf;
	struct cifsd_tcon *tcon = smb_work->tcon;
	struct cifsd_sess *sess = smb_work->sess;

	if (!tcon) {
		cifsd_err("Invalid tid %d\n", req_hdr->Tid);
		rsp_hdr->Status.CifsError = NT_STATUS_NO_SUCH_USER;
		return -EINVAL;
	}

	if (tcon->share->sharename)
		path_put(&tcon->share_path);
	/* delete tcon from sess tcon list and decrease sess tcon count */
	list_del(&tcon->tcon_list);
	sess->tcon_count--;
	kfree(tcon);

	close_opens_from_fibtable(sess, le16_to_cpu(req_hdr->Tid));
	return 0;
}

void set_service_type(struct connection *conn,
			struct cifsd_share *share, TCONX_RSP_EXT *rsp)
{
	int length;
	char *buf = rsp->Service;

	if (share->is_pipe == true) {
		length = strlen(SERVICE_IPC_SHARE);
		memcpy(buf, SERVICE_IPC_SHARE, length);
		rsp->ByteCount = length + 1;
		buf += length;
		*buf = '\0';
	} else {
		int uni_len = 0;
		length = strlen(SERVICE_DISK_SHARE);
		memcpy(buf, SERVICE_DISK_SHARE, length);
		buf[length] = '\0';
		length += 1;
		uni_len = smbConvertToUTF16((__le16 *)(buf + length),
					     NATIVE_FILE_SYSTEM,
					     PATH_MAX, conn->local_nls, 0);
		uni_len++;
		uni_len *= 2;
		length += uni_len;
		rsp->ByteCount = length;
	}
}

/**
 * smb_tree_connect_andx() - tree connect request handler
 * @smb_work:	smb work containing tree connect request buffer
 *
 * Return:      0 on success, otherwise error
 */
int smb_tree_connect_andx(struct smb_work *smb_work)
{
	struct smb_hdr *req_hdr = (struct smb_hdr *)smb_work->buf;
	struct smb_hdr *rsp_hdr = (struct smb_hdr *)smb_work->rsp_buf;
	struct connection *conn = smb_work->conn;
	TCONX_REQ *req;
	TCONX_RSP_EXT *rsp;
	int extra_byte = 0, rc;
	char *treename = NULL, *name = NULL;
	struct cifsd_share *share;
	struct cifsd_tcon *tcon;
	struct cifsd_sess *sess = smb_work->sess;
	bool can_write;

	/* Is this an ANDX command ? */
	if (req_hdr->Command != SMB_COM_TREE_CONNECT_ANDX) {
		cifsd_debug("SMB_COM_TREE_CONNECT_ANDX is part of ANDX");
		req = (TCONX_REQ *)andx_request_buffer(smb_work->buf,
						SMB_COM_TREE_CONNECT_ANDX);
		rsp = (TCONX_RSP_EXT *)andx_response_buffer(smb_work->rsp_buf);
		extra_byte = 3;
		if (!req) {
			rc = -EINVAL;
			goto out_err;
		}
	} else {
		req = (TCONX_REQ *)(&req_hdr->WordCount);
		rsp = (TCONX_RSP_EXT *)(&rsp_hdr->WordCount);
	}

	/* check if valid tree name is present in request or not */
	if (!req->PasswordLength)
		treename = smb_strndup_from_utf16(req->Password + 1,
				256, true, conn->local_nls);
	else
		treename = smb_strndup_from_utf16(req->Password +
				req->PasswordLength, 256, true,
				conn->local_nls);

	if (IS_ERR(treename)) {
		cifsd_err("treename is NULL for uid %d\n", rsp_hdr->Uid);
		rc = PTR_ERR(treename);
		goto out_err;
	}
	name = extract_sharename(treename);
	if (IS_ERR(name)) {
		rc = PTR_ERR(name);
		goto out_err;
	}

	cifsd_debug("tree connect request for tree %s\n", name);

	share = get_cifsd_share(conn, sess, name, &can_write);
	if (IS_ERR(share)) {
		rc = PTR_ERR(share);
		goto out_err;
	}

	tcon = construct_cifsd_tcon(share, sess);
	if (IS_ERR(tcon)) {
		rc = PTR_ERR(tcon);
		goto out_err;
	}

	tcon->writeable = can_write;
	rsp->WordCount = 7;
	rsp->OptionalSupport = 0;

	rsp->OptionalSupport = (SMB_SUPPORT_SEARCH_BITS |
				SMB_CSC_NO_CACHING | SMB_UNIQUE_FILE_NAME);

	rsp->MaximalShareAccessRights = 0;
	rsp->MaximalShareAccessRights = (FILE_READ_RIGHTS |
					FILE_EXEC_RIGHTS | FILE_WRITE_RIGHTS);
	rsp->GuestMaximalShareAccessRights = 0;

	if (!strncmp("IPC$", name, 4))
		tcon->share->is_pipe = true;

	set_service_type(conn, share, rsp);

	rsp_hdr->Tid = tcon->share->tid;

	/* For each extra andx response, we have to add 1 byte,
		 for wc and 2 bytes for byte count */
	inc_rfc1001_len(rsp_hdr, (7 * 2 + rsp->ByteCount + extra_byte));
	/* this is an ANDx command ? */
	if (req->AndXCommand == 0xFF) {
		rsp->AndXCommand = SMB_NO_MORE_ANDX_COMMAND;
		rsp->AndXReserved = 0;
		rsp->AndXOffset = 0;
		return 0;
	} else {
		/* adjust response */
		rsp->AndXOffset = get_rfc1002_length(rsp_hdr);
		rsp->AndXCommand = req->AndXCommand;
		rsp->AndXReserved = 0;

		return rsp->AndXCommand; /* More processing required */
	}

out_err:
	rsp->WordCount = 7;
	rsp->AndXCommand = SMB_NO_MORE_ANDX_COMMAND;
	rsp->AndXReserved = 0;
	rsp->AndXOffset = 0;
	rsp->OptionalSupport = 0;
	rsp->MaximalShareAccessRights = 0;
	rsp->GuestMaximalShareAccessRights = 0;
	rsp->ByteCount = 0;
	cifsd_debug("error while tree connect\n");
	switch (rc) {
	case -ENOENT:
		rsp_hdr->Status.CifsError = NT_STATUS_BAD_NETWORK_PATH;
		break;
	case -ENOMEM:
		rsp_hdr->Status.CifsError = NT_STATUS_NO_MEMORY;
		break;
	case -EACCES:
		rsp_hdr->Status.CifsError = NT_STATUS_ACCESS_DENIED;
		break;
	case -EINVAL:
		if (!req)
			rsp_hdr->Status.CifsError =
				NT_STATUS_INVALID_PARAMETER;
		else if (!sess)
			rsp_hdr->Status.CifsError =
				NT_STATUS_NO_SUCH_LOGON_SESSION;
		else if (IS_ERR(treename) || IS_ERR(name))
			rsp_hdr->Status.CifsError = NT_STATUS_BAD_NETWORK_NAME;
		else /* default also invalid parameter, repeat of !req*/
			rsp_hdr->Status.CifsError =
				NT_STATUS_INVALID_PARAMETER;
		break;
	default:
		rsp_hdr->Status.CifsError = NT_STATUS_OK;
	}

	/* Clean session if there is no tree attached */
	if (!sess ||  !sess->tcon_count)
		conn->tcp_status = CifsExiting;
	inc_rfc1001_len(rsp_hdr, (7 * 2 + rsp->ByteCount + extra_byte));
	kfree(treename);
	kfree(name);
	return rc;
}

/**
 * smb_rename() - rename request handler
 * @smb_work:	smb work containing rename request buffer
 *
 * Return:      0 on success, otherwise error
 */
int smb_rename(struct smb_work *smb_work)
{
	RENAME_REQ *req = (RENAME_REQ *)smb_work->buf;
	RENAME_RSP *rsp = (RENAME_RSP *)smb_work->rsp_buf;
	struct connection *conn = smb_work->conn;
	bool is_unicode = is_smbreq_unicode(&req->hdr);
	char *abs_oldname, *abs_newname, *tmp_name = NULL;
	int oldname_len;
	struct path path;
	bool file_present = true;
	int rc = 0;

	abs_oldname = smb_get_name(req->OldFileName, PATH_MAX, smb_work, false);
	if (IS_ERR(abs_oldname))
		return PTR_ERR(abs_oldname);

	if (is_unicode) {
		oldname_len = smb_utf16_bytes((__le16 *)req->OldFileName,
				PATH_MAX, conn->local_nls);
		oldname_len += nls_nullsize(conn->local_nls);
		oldname_len *= 2;
	} else {
		oldname_len = strlen(abs_oldname);
		oldname_len++;
	}

	abs_newname = smb_get_name(&req->OldFileName[oldname_len + 2],
			PATH_MAX, smb_work, false);
	if (IS_ERR(abs_newname)) {
		rc = PTR_ERR(abs_newname);
		goto out;
	}

	tmp_name = kmalloc(PATH_MAX, GFP_NOFS);
	if (!tmp_name) {
		rsp->hdr.Status.CifsError = NT_STATUS_NO_MEMORY;
		rc = -ENOMEM;
		goto out;
	}
	strncpy(tmp_name, abs_newname, strlen(abs_newname) + 1);

	rc = smb_kern_path(tmp_name, 0, &path, 1);
	if (rc)
		file_present = false;
	else
		path_put(&path);

	if (file_present &&
			strncmp(abs_oldname, tmp_name,
				strlen(abs_oldname))) {
		rc = -EEXIST;
		rsp->hdr.Status.CifsError =
			NT_STATUS_OBJECT_NAME_COLLISION;
		cifsd_debug("cannot rename already existing file\n");
		goto out;
	}

	cifsd_debug("rename %s -> %s\n", abs_oldname, abs_newname);
	rc = smb_vfs_rename(smb_work->sess, abs_oldname, abs_newname, 0);
	if (rc) {
		rsp->hdr.Status.CifsError = NT_STATUS_NO_MEMORY;
		goto out;
	}
	rsp->hdr.WordCount = 0;
	rsp->ByteCount = 0;
out:
	kfree(tmp_name);
	smb_put_name(abs_oldname);
	smb_put_name(abs_newname);
	return rc;
}

/**
 * smb_negotiate() - negotiate request handler
 * @smb_work:	smb work containing negotiate request buffer
 *
 * Return:      0 on success, otherwise error
 */
int smb_negotiate(struct smb_work *smb_work)
{
	struct connection *conn = smb_work->conn;
	NEGOTIATE_RSP *neg_rsp = (NEGOTIATE_RSP *)smb_work->rsp_buf;
	NEGOTIATE_REQ *neg_req = (NEGOTIATE_REQ *)smb_work->buf;
	__le64 time;

	WARN_ON(neg_req->hdr.WordCount);
	WARN_ON(conn->tcp_status == CifsGood);

	conn->dialect = negotiate_dialect(smb_work->buf);
	cifsd_debug("conn->dialect 0x%x\n", conn->dialect);
	if (conn->dialect == BAD_PROT_ID) {
		neg_rsp->hdr.Status.CifsError = NT_STATUS_INVALID_LOGON_TYPE;
		return 0;
	} else if (conn->dialect == SMB20_PROT_ID ||
			conn->dialect == SMB21_PROT_ID ||
			conn->dialect == SMB2X_PROT_ID ||
			conn->dialect == SMB30_PROT_ID ||
			conn->dialect == SMB302_PROT_ID ||
			conn->dialect == SMB311_PROT_ID)
		return conn->dialect;

	conn->connection_type = 0;

	/* wct 17 for NTLM */
	neg_rsp->hdr.WordCount = 17;
	neg_rsp->DialectIndex = conn->dialect;

	neg_rsp->SecurityMode = SERVER_SECU;
	if (server_signing == AUTO || server_signing == MANDATORY) {
		conn->sign = true;
		neg_rsp->SecurityMode |= SECMODE_SIGN_ENABLED;
	}
	neg_rsp->MaxMpxCount = SERVER_MAX_MPX_COUNT;
	neg_rsp->MaxNumberVcs = SERVER_MAX_VCS;
	neg_rsp->MaxBufferSize = SMBMaxBufSize;
	neg_rsp->MaxRawSize = SERVER_MAX_RAW_SIZE;
	neg_rsp->SessionKey = 0;
	neg_rsp->Capabilities = SERVER_CAPS;

	/* System time is anyway ignored by clients */
	time = cpu_to_le64(cifs_UnixTimeToNT(CURRENT_TIME));
	neg_rsp->SystemTimeLow =  (time & 0x00000000FFFFFFFF);
	neg_rsp->SystemTimeHigh = ((time & 0xFFFFFFFF00000000) >> 32);
	neg_rsp->ServerTimeZone = 0;
	neg_rsp->EncryptionKeyLength = CIFS_CRYPTO_KEY_SIZE;
	neg_rsp->ByteCount = CIFS_CRYPTO_KEY_SIZE;
	/* initialize random server challenge */
	get_random_bytes(conn->ntlmssp_cryptkey, sizeof(__u64));
	memcpy((neg_rsp->u.EncryptionKey), conn->ntlmssp_cryptkey,
			CIFS_CRYPTO_KEY_SIZE);

	/* Null terminated domain name in unicode */


	/* Adjust pdu length, 17 words and 8 bytes added */
	inc_rfc1001_len(neg_rsp, (17 * 2 + 8));
	conn->tcp_status = CifsNeedNegotiate;
	/* Domain name and PC name are ignored by clients, so no need to send.
	 * We can try sending them later */
	return 0;
}

/**
 * smb_session_setup_andx() - session setup request handler
 * @smb_work:	smb work containing session setup request buffer
 *
 * Return:      0 on success, otherwise error
 */
int smb_session_setup_andx(struct smb_work *smb_work)
{
	struct smb_hdr *req_hdr = (struct smb_hdr *)smb_work->buf;
	struct smb_hdr *rsp_hdr = (struct smb_hdr *)smb_work->rsp_buf;
	struct connection *conn = smb_work->conn;
	struct cifsd_sess *sess = NULL;
	char *name;
	int offset, rc;

	SESSION_SETUP_ANDX *pSMB = (SESSION_SETUP_ANDX *)smb_work->buf;
	SESSION_SETUP_ANDX *response = (SESSION_SETUP_ANDX *)smb_work->rsp_buf;

	/* This triggers with cifs client. cifs client needs fixing */
	WARN_ON(req_hdr->WordCount != 13);
	WARN_ON(conn->tcp_status != CifsNeedNegotiate);

	/* check if valid user name is present in request or not */
	offset = pSMB->req_no_secext.CaseInsensitivePasswordLength +
			pSMB->req_no_secext.CaseSensitivePasswordLength;

	/* 1 byte for padding */
	name = smb_strndup_from_utf16(
			(pSMB->req_no_secext.CaseInsensitivePassword +
			 offset + 1), 256, true, conn->local_nls);
	if (IS_ERR(name)) {
		cifsd_err("cannot allocate memory\n");
		rc = PTR_ERR(name);
		goto out_err;
	}

	if (!smb_work->sess) {
		/* build smb session */
		WARN_ON(smb_work->sess);
		sess = kzalloc(sizeof(struct cifsd_sess), GFP_KERNEL);
		if (sess == NULL) {
			rc = -ENOMEM;
			goto out_err;
		}

		sess->conn = conn;
		INIT_LIST_HEAD(&sess->cifsd_ses_list);
		INIT_LIST_HEAD(&sess->cifsd_chann_list);
		list_add(&sess->cifsd_ses_list, &conn->cifsd_sess);
		list_add(&sess->cifsd_ses_global_list, &cifsd_session_list);
		INIT_LIST_HEAD(&sess->tcon_list);
		sess->tcon_count = 0;

		cifsd_debug("session setup request for user %s\n", name);
		sess->usr = cifsd_is_user_present(name);
		kfree(name);
		if (!sess->usr) {
			cifsd_err("user not present in database\n");
			rc = -EINVAL;
			goto out_err;
		}

		rsp_hdr->Uid = sess->usr->vuid;
		sess->sess_id = sess->usr->vuid;
		init_waitqueue_head(&sess->pipe_q);
		sess->ev_state = NETLINK_REQ_INIT;
		cifsd_debug("generate session ID : %llu, Uid : %u\n",
				sess->sess_id, req_hdr->Uid);
	} else {
		sess = smb_work->sess;
		cifsd_debug("reuse session(%p) session ID : %llu, Uid : %u\n",
				sess, sess->sess_id, req_hdr->Uid);
	}

	memcpy(sess->ntlmssp.cryptkey, conn->ntlmssp_cryptkey,
			CIFS_CRYPTO_KEY_SIZE);

	if (sess->usr->guest)
		goto no_password_check;

	if (pSMB->req_no_secext.CaseSensitivePasswordLength ==
		CIFS_AUTH_RESP_SIZE) {
		rc = process_ntlm(sess,
			(char *)pSMB->req_no_secext.CaseInsensitivePassword +
			pSMB->req_no_secext.CaseInsensitivePasswordLength);
		if (rc) {
			cifsd_err("ntlm authentication failed for user %s\n",
				sess->usr->name);
			goto out_err;
		}
	} else {
		char *ntdomain;

		offset = pSMB->req_no_secext.CaseInsensitivePasswordLength +
			pSMB->req_no_secext.CaseSensitivePasswordLength +
			((strlen(sess->usr->name) + 1) * 2);

		ntdomain = smb_strndup_from_utf16(
			pSMB->req_no_secext.CaseInsensitivePassword +
			offset + 1, 256, true, conn->local_nls);
		if (IS_ERR(ntdomain)) {
			cifsd_err("cannot allocate memory\n");
			rc = PTR_ERR(ntdomain);
			goto out_err;
		}

		rc = process_ntlmv2(sess, (struct ntlmv2_resp *) ((char *)
			pSMB->req_no_secext.CaseInsensitivePassword +
			pSMB->req_no_secext.CaseInsensitivePasswordLength),
			pSMB->req_no_secext.CaseSensitivePasswordLength -
			CIFS_ENCPWD_SIZE, ntdomain);
		if (rc) {
			cifsd_err("authentication failed for user %s\n",
				sess->usr->name);
			goto out_err;
		}
	}

no_password_check:

	/* verify that any session is not already added although
	   we have set max vcn as 1 */
	WARN_ON(conn->sess_count);

	sess->usr->ucount++;
	conn->sess_count++;
	rc = init_fidtable(&sess->fidtable);
	if (rc < 0)
		goto out_err;

	sess->valid = 1;
	smb_work->sess = sess;

	/* Build response. We don't use extended security (yet), so wct is 3 */
	rsp_hdr->WordCount = 3;
	response->old_resp.Action = 0;

	/* The names should be unicode */
	response->old_resp.ByteCount = 0;

	/* adjust pdu length. data added 6 bytes */
	inc_rfc1001_len(rsp_hdr, 6);

	/* setup unique client id. TODO: create a list */
	rsp_hdr->Uid = sess->usr->vuid;
	conn->vuid = sess->usr->vuid;

	conn->tcp_status = CifsGood;

	/* this is an ANDx command ? */
	if (pSMB->req_no_secext.AndXCommand == SMB_NO_MORE_ANDX_COMMAND) {
		response->old_resp.AndXCommand = SMB_NO_MORE_ANDX_COMMAND;
		response->old_resp.AndXReserved = 0;
		response->old_resp.AndXOffset = 0;
		return 0;
	} else {
		/* adjust response */
		response->old_resp.AndXOffset = get_rfc1002_length(rsp_hdr);
		response->old_resp.AndXCommand =
					pSMB->req_no_secext.AndXCommand;
		response->old_resp.AndXReserved = 0;
		/* More processing required */
		return pSMB->req_no_secext.AndXCommand;
	}

out_err:
	if (rc < 0 && sess) {
		sess->valid = 0;
		list_del(&sess->cifsd_ses_list);
		list_del(&sess->cifsd_ses_global_list);
		kfree(sess);
		smb_work->sess = NULL;
	}
	rsp_hdr->Status.CifsError = NT_STATUS_LOGON_FAILURE;
	return rc;
}

/**
 * file_create_dispostion_flags() - convert disposition flags to
 *				file open flags
 * @dispostion:		file disposition contained in open request
 * @file_present:	file already present or not
 *
 * Return:      file open flags after conversion from disposition
 */
int file_create_dispostion_flags(int dispostion, bool file_present)
{
	int disp_flags = 0;

	switch (dispostion) {
	/*
	 * If the file already exists, it SHOULD be superseded (overwritten).
	 * If it does not already exist, then it SHOULD be created.
	 */
	case FILE_SUPERSEDE:
		if (file_present)
			disp_flags |= O_TRUNC;
		else
			disp_flags |= O_CREAT;
		break;
	/*
	 * If the file already exists, it SHOULD be opened rather than created.
	 * If the file does not already exist, the operation MUST fail.
	 */
	case FILE_OPEN:
		if (!file_present)
			return -ENOENT;
		break;
	/*
	 * If the file already exists, the operation MUST fail.
	 * If the file does not already exist, it SHOULD be created.
	 */
	case FILE_CREATE:
		if (file_present)
			return -EEXIST;
		disp_flags |= O_CREAT;
		break;
	/*
	 * If the file already exists, it SHOULD be opened. If the file
	 * does not already exist, then it SHOULD be created.
	 */
	case FILE_OPEN_IF:
		if (!file_present)
			disp_flags |= O_CREAT;
		break;
	/*
	 * If the file already exists, it SHOULD be opened and truncated.
	 * If the file does not already exist, the operation MUST fail.
	 */
	case FILE_OVERWRITE:
		if (!file_present)
			return -ENOENT;
		disp_flags |= O_TRUNC;
		break;
	/*
	 * If the file already exists, it SHOULD be opened and truncated.
	 * If the file does not already exist, it SHOULD be created.
	 */
	case FILE_OVERWRITE_IF:
		if (file_present)
			disp_flags |= O_TRUNC;
		else
			disp_flags |= O_CREAT;
		break;
	default:
		return -EINVAL;
	}

	return disp_flags;
}

/**
 * convert_generic_access_flags() - convert access flags to
 *				file open flags
 * @access_flag:	file access flags contained in open request
 * @open_flag:		file open flags are updated as per access flags
 * @attrib:		attribute flag indicating posix symantics or not
 *
 * Return:		access flags
 */
int convert_generic_access_flags(int access_flag, int *open_flags, int attrib)
{
	int aflags = access_flag;
	int oflags = *open_flags;

	if (aflags & GENERIC_READ) {
		aflags &= ~GENERIC_READ;
		aflags |= GENERIC_READ_FLAGS;
	}

	if (aflags & GENERIC_WRITE) {
		aflags &= ~GENERIC_WRITE;
		aflags |= GENERIC_WRITE_FLAGS;
	}

	if (aflags & GENERIC_EXECUTE) {
		aflags &= ~GENERIC_EXECUTE;
		aflags |= GENERIC_EXECUTE_FLAGS;
	}

	if (aflags & GENERIC_ALL) {
		aflags &= ~GENERIC_ALL;
		aflags |= GENERIC_ALL_FLAGS;
	}

	if (oflags & O_TRUNC)
		aflags |= FILE_WRITE_DATA;

	if (aflags & (FILE_WRITE_DATA | FILE_APPEND_DATA)) {
		if (aflags & (FILE_READ_ATTRIBUTES | FILE_READ_DATA |
					FILE_READ_EA | FILE_EXECUTE)) {
			*open_flags |= O_RDWR;

		} else {
			*open_flags |= O_WRONLY;
		}
	} else {
		*open_flags |= O_RDONLY;
	}

	if ((attrib & ATTR_POSIX_SEMANTICS) && (aflags & FILE_APPEND_DATA))
		*open_flags |= O_APPEND;

	return aflags;
}

/**
 * smb_get_dos_attr() - convert unix style stat info to dos attr
 * @stat:	stat to be converted to dos attr
 *
 * Return:	dos style attribute
 */
__u32 smb_get_dos_attr(struct kstat *stat)
{
	__u32 attr = 0;

	/* check whether file has attributes ATTR_READONLY, ATTR_HIDDEN,
	   ATTR_SYSTEM, ATTR_VOLUME, ATTR_DIRECTORY, ATTR_ARCHIVE,
	   ATTR_DEVICE, ATTR_NORMAL, ATTR_TEMPORARY, ATTR_SPARSE,
	   ATTR_REPARSE, ATTR_COMPRESSED, ATTR_OFFLINE */

	if (stat->mode & S_ISVTX)   /* hidden */
		attr |=  (ATTR_HIDDEN | ATTR_SYSTEM);

	if (!(stat->mode & S_IWUGO))  /* read-only */
		attr |=  ATTR_READONLY;

	if (S_ISDIR(stat->mode))
		attr |= ATTR_DIRECTORY;

	if (stat->size > (stat->blksize * stat->blocks))
		attr |= ATTR_SPARSE;

	if (!attr)
		attr |= ATTR_NORMAL;

	return attr;
}

/**
 * smb_locking_andx() - received oplock break response from client
 * @smb_work:	smb work containing oplock break command
 *
 * Return:	0 on success, otherwise error
 */
int smb_locking_andx(struct smb_work *smb_work)
{
	LOCK_REQ *req;
	LOCK_RSP *rsp;
	struct cifsd_file *fp;
	struct connection *conn = smb_work->conn;
	struct ofile_info *ofile;
	struct oplock_info *opinfo;
	char oplock;
	int ret = 0;

	if (!oplocks_enable)
		return -ENOSYS;

	req = (LOCK_REQ *)smb_work->buf;
	rsp = (LOCK_RSP *)smb_work->rsp_buf;

	if (!(req->LockType & LOCKING_ANDX_OPLOCK_RELEASE)) {
		cifsd_err("LockType %d not supported in smb_locking_andx\n",
			    req->LockType);
		rsp->hdr.Status.CifsError = NT_STATUS_NOT_SUPPORTED;
		rsp->ByteCount = 0;
		return 0;
	}
	cifsd_debug("got oplock brk for fid %d level OplockLevel = %d\n",
		      req->Fid, req->OplockLevel);

	oplock = req->OplockLevel;

	/* find fid */
	mutex_lock(&ofile_list_lock);
	fp = get_id_from_fidtable(smb_work->sess, req->Fid);
	if (fp == NULL) {
		mutex_unlock(&ofile_list_lock);
		cifsd_err("cannot obtain fid for %d\n", req->Fid);
		return -EINVAL;
	}

	ofile = fp->ofile;
	if (ofile == NULL) {
		cifsd_err("unexpected null ofile_info\n");
		mutex_unlock(&ofile_list_lock);
		return -EINVAL;
	}

	opinfo = get_matching_opinfo(conn, ofile, req->Fid, 0);
	if (opinfo == NULL) {
		cifsd_err("unexpected null oplock_info\n");
		mutex_unlock(&ofile_list_lock);
		return -EINVAL;
	}

	if (opinfo->op_state == OPLOCK_STATE_NONE) {
		mutex_unlock(&ofile_list_lock);
		cifsd_err("unexpected oplock state 0x%x\n", opinfo->op_state);
		return -EINVAL;
	}

	if (oplock == OPLOCK_EXCLUSIVE || oplock == OPLOCK_BATCH) {
		if (opinfo_write_to_none(ofile, opinfo) < 0) {
			cifsd_err("lock level mismatch for fid %d\n",
					req->Fid);
			mutex_unlock(&ofile_list_lock);
			opinfo->op_state = OPLOCK_STATE_NONE;
			return -EINVAL;
		}
	} else if (((opinfo->lock_type == OPLOCK_EXCLUSIVE) ||
				(opinfo->lock_type == OPLOCK_BATCH)) &&
			(oplock == OPLOCK_READ)) {
		ret = opinfo_write_to_read(ofile, opinfo, 0);
		if (ret) {
			opinfo->op_state = OPLOCK_STATE_NONE;
			mutex_unlock(&ofile_list_lock);
			return -EINVAL;
		}
	} else if ((opinfo->lock_type == OPLOCK_READ) &&
			(oplock == OPLOCK_NONE)) {
		ret = opinfo_read_to_none(ofile, opinfo);
		if (ret) {
			opinfo->op_state = OPLOCK_STATE_NONE;
			mutex_unlock(&ofile_list_lock);
			return -EINVAL;
		}
	}

	opinfo->op_state = OPLOCK_STATE_NONE;
	wake_up_interruptible(&conn->oplock_q);
	wake_up(&opinfo->op_end_wq);
	mutex_unlock(&ofile_list_lock);

	return 0;
}

/**
 * alloc_lanman_pipe_desc() - allocate lanman pipe buffers
 * @sess:	session info
 *
 * Return:	0 on success, otherwise error
 */
int alloc_lanman_pipe_desc(struct cifsd_sess *sess)
{
	struct cifsd_pipe *pipe_desc;

	if (unlikely(!sess))
		return -EINVAL;

	sess->pipe_desc[LANMAN] = kzalloc(sizeof(struct cifsd_pipe),
			GFP_KERNEL);
	pipe_desc = sess->pipe_desc[LANMAN];
	if (!pipe_desc)
		return -ENOMEM;

	pipe_desc->rsp_buf = kmalloc(NETLINK_CIFSD_MAX_PAYLOAD,
			GFP_KERNEL);
	if (!pipe_desc->rsp_buf) {
		kfree(pipe_desc);
		sess->pipe_desc[LANMAN] = NULL;
		return -ENOMEM;
	}

	pipe_desc->pipe_type = LANMAN;
	return 0;
}

/**
 * free_lanman_pipe_desc() - free lanman pipe buffers
 * @sess:	session info
 *
 */
void free_lanman_pipe_desc(struct cifsd_sess *sess)
{
	struct cifsd_pipe *pipe_desc;

	pipe_desc = sess->pipe_desc[LANMAN];
	kfree(pipe_desc->rsp_buf);
	kfree(pipe_desc);
	sess->pipe_desc[LANMAN] = NULL;
}

/**
 * smb_trans() - trans2 command dispatcher
 * @smb_work:	smb work containing trans2 command
 *
 * Return:	0 on success, otherwise error
 */
int smb_trans(struct smb_work *smb_work)
{
	struct connection *conn = smb_work->conn;
	TRANS_REQ *req = (TRANS_REQ *)smb_work->buf;
	TRANS_RSP *rsp = (TRANS_RSP *)smb_work->rsp_buf;
	TRANS_PIPE_REQ *pipe_req = (TRANS_PIPE_REQ *)smb_work->buf;
	struct cifsd_pipe *pipe_desc;
	__u16 subcommand;
	char *name, *pipe;
	char *pipedata;
	int setup_bytes_count = 0;
	int pipe_name_offset = 0;
	int str_len_uni;
	int ret = 0, nbytes = 0;
	int param_len = 0;
	int id, buf_len;
	struct cifsd_uevent *ev;

	buf_len = le16_to_cpu(req->MaxDataCount);
	buf_len = min((int)(NETLINK_CIFSD_MAX_PAYLOAD - sizeof(TRANS_RSP)),
			buf_len);

	if (req->SetupCount)
		setup_bytes_count = 2 * req->SetupCount;

	subcommand = le16_to_cpu(req->SubCommand);
	name = smb_strndup_from_utf16(req->Data + setup_bytes_count, 256, 1,
			conn->local_nls);

	if (IS_ERR(name)) {
		cifsd_err("failed to allocate memory\n");
		rsp->hdr.Status.CifsError = NT_STATUS_NO_MEMORY;
		return PTR_ERR(name);
	}

	cifsd_debug("Obtained string name = %s setupcount = %d\n",
			name, setup_bytes_count);

	pipe_name_offset = strlen("\\PIPE");
	if (strncmp("\\PIPE", name, pipe_name_offset) != 0) {
		cifsd_debug("Not Pipe request\n");
		rsp->hdr.Status.CifsError = NT_STATUS_NOT_SUPPORTED;
		kfree(name);
		return 0;
	}

	if (name[pipe_name_offset] == '\\')
		pipe_name_offset++;

	pipe = name + pipe_name_offset;

	if (*pipe != '\0' && strncmp(pipe, "LANMAN", sizeof("LANMAN")) != 0) {
		cifsd_debug("Pipe %s not supported request\n", pipe);
		rsp->hdr.Status.CifsError = NT_STATUS_NOT_SUPPORTED;
		kfree(name);
		return 0;
	}

	/* Incoming pipe name unicode len */
	str_len_uni = 2 * (strlen(name) + 1);

	cifsd_debug("Pipe name unicode len = %d\n", str_len_uni);

	/* 2 is for padding after pipe name */
	pipedata = req->Data + str_len_uni + 2 + setup_bytes_count;

	if (!strncmp(pipe, "LANMAN", sizeof("LANMAN"))) {
		if (alloc_lanman_pipe_desc(smb_work->sess)) {
			cifsd_err("failed to allocate memory\n");
			rsp->hdr.Status.CifsError = NT_STATUS_NO_MEMORY;
			kfree(name);
			return 0;
		}

		ret = cifsd_sendmsg(smb_work->sess,
				CIFSD_KEVENT_LANMAN_PIPE,
				LANMAN, le16_to_cpu(req->TotalParameterCount),
				pipedata, buf_len);
		if (ret) {
			cifsd_err("failed to send event, err %d\n", ret);
			free_lanman_pipe_desc(smb_work->sess);
			smb_work->sess->ev_state = NETLINK_REQ_COMPLETED;
			goto out;
		}

		pipe_desc = smb_work->sess->pipe_desc[LANMAN];
		ev = &pipe_desc->ev;
		nbytes = ev->u.l_pipe_rsp.data_count;
		param_len = ev->u.l_pipe_rsp.param_count;
		if (nbytes < 0) {
			if (nbytes == -EOPNOTSUPP)
				rsp->hdr.Status.CifsError =
					NT_STATUS_NOT_SUPPORTED;
			else
				rsp->hdr.Status.CifsError =
					NT_STATUS_INVALID_PARAMETER;

			free_lanman_pipe_desc(smb_work->sess);
			smb_work->sess->ev_state = NETLINK_REQ_COMPLETED;
			goto out;
		}

		memcpy((char *)rsp + sizeof(TRANS_RSP),
				pipe_desc->rsp_buf, nbytes);
		free_lanman_pipe_desc(smb_work->sess);
		smb_work->sess->ev_state = NETLINK_REQ_COMPLETED;
		goto resp_out;
	}

	id = le16_to_cpu(pipe_req->fid);
	pipe_desc = get_pipe_desc(smb_work->sess, id);
	if (!pipe_desc) {
		cifsd_debug("Pipe not opened or invalid in Pipe id\n");
		if (pipe_desc)
			cifsd_debug("Incoming id = %d opened pipe id = %d\n",
					id, pipe_desc->id);
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_HANDLE;
		goto out;
	}

	switch (subcommand) {

	case TRANSACT_DCERPCCMD:

		cifsd_debug("GOT TRANSACT_DCERPCCMD\n");
		ret = cifsd_sendmsg(smb_work->sess, CIFSD_KEVENT_IOCTL_PIPE,
				pipe_desc->pipe_type,
				le16_to_cpu(req->DataCount), pipedata,
				buf_len);
		if (ret)
			cifsd_err("failed to send event, err %d\n", ret);
		else {
			ev = &pipe_desc->ev;
			nbytes = ev->u.i_pipe_rsp.data_count;
			ret = ev->error;
			if (ret == -EOPNOTSUPP) {
				rsp->hdr.Status.CifsError =
					NT_STATUS_NOT_SUPPORTED;
				goto out;
			} else if (ret) {
				rsp->hdr.Status.CifsError =
					NT_STATUS_INVALID_PARAMETER;
				goto out;
			}

			memcpy((char *)rsp + sizeof(TRANS_RSP),
					pipe_desc->rsp_buf, nbytes);
			smb_work->sess->ev_state = NETLINK_REQ_COMPLETED;
		}
		break;

	default:
		cifsd_debug("SMB TRANS subcommand not supported %u\n",
				subcommand);
		ret = -EOPNOTSUPP;
		rsp->hdr.Status.CifsError = NT_STATUS_NOT_SUPPORTED;
		goto out;
	}

resp_out:

	rsp->hdr.WordCount = 10;
	rsp->TotalParameterCount = param_len;
	rsp->TotalDataCount = cpu_to_le16(nbytes);
	rsp->Reserved = 0;
	rsp->ParameterCount = param_len;
	rsp->ParameterOffset = cpu_to_le16(56);
	rsp->ParameterDisplacement = 0;
	rsp->DataCount = cpu_to_le16(nbytes);
	rsp->DataOffset = cpu_to_le16(56 + param_len);
	rsp->DataDisplacement = 0;
	rsp->SetupCount = 0;
	rsp->Reserved1 = 0;
	/* Adding 1 for Pad */
	rsp->ByteCount = cpu_to_le16(nbytes + 1 + param_len);
	rsp->Pad = 0;
	inc_rfc1001_len(&rsp->hdr, (10 * 2 + rsp->ByteCount));

out:
	smb_put_name(name);
	return ret;
}

/**
 * create_andx_pipe() - create ipc pipe request handler
 * @smb_work:	smb work containing create command
 *
 * Return:	0 on success, otherwise error
 */
int create_andx_pipe(struct smb_work *smb_work)
{
	OPEN_REQ *req = (OPEN_REQ *)smb_work->buf;
	OPEN_EXT_RSP *rsp = (OPEN_EXT_RSP *)smb_work->rsp_buf;
	unsigned int pipe_type;
	char *name;
	int rc = 0;
	__u16 fid;

	/* one byte pad before unicode file name start */
	if (is_smbreq_unicode(&req->hdr))
		name = smb_strndup_from_utf16(req->fileName + 1, 256, 1,
				smb_work->conn->local_nls);
	else
		name = smb_strndup_from_utf16(req->fileName, 256, 1,
				smb_work->conn->local_nls);

	if (IS_ERR(name)) {
		rc = -ENOMEM;
		goto out;
	}

	pipe_type = get_pipe_type(name);
	if (pipe_type == INVALID_PIPE) {
		cifsd_debug("pipe %s not supported\n", name);
		rc = -EOPNOTSUPP;
		goto out;
	}

	/* Assigning temporary fid for pipe */
	rc = get_pipe_id(smb_work->sess, pipe_type);
	if (rc < 0)
		goto out;
	else
		fid = rc;

	rc = cifsd_sendmsg(smb_work->sess,
			CIFSD_KEVENT_CREATE_PIPE, pipe_type, 0, NULL, 0);
	if (rc) {
		cifsd_err("failed to send event, err %d\n", rc);
		goto out;
	}

	rsp->hdr.WordCount = 42;
	rsp->AndXCommand = cpu_to_le16(0xff);
	rsp->AndXReserved = 0;
	rsp->OplockLevel = 0;
	rsp->Fid = cpu_to_le16(fid);
	rsp->CreateAction = cpu_to_le32(1);
	rsp->CreationTime = 0;
	rsp->LastAccessTime = 0;
	rsp->LastWriteTime = 0;
	rsp->ChangeTime = 0;
	rsp->FileAttributes = cpu_to_le32(ATTR_NORMAL);
	rsp->AllocationSize = cpu_to_le64(0);
	rsp->EndOfFile = cpu_to_le16(0);
	rsp->FileType = cpu_to_le16(2);
	rsp->DeviceState = cpu_to_le16(0x05ff);
	rsp->DirectoryFlag = 0;
	rsp->fid = 0;
	rsp->MaxAccess = cpu_to_le32(FILE_GENERIC_ALL);
	rsp->GuestAccess = cpu_to_le32(FILE_GENERIC_READ);
	rsp->ByteCount = 0;
	inc_rfc1001_len(&rsp->hdr, (100 + rsp->ByteCount));

out:
	switch (rc) {
	case 0:
		rsp->hdr.Status.CifsError = NT_STATUS_OK;
		break;
	case -EINVAL:
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_PARAMETER;
		break;
	case -EOVERFLOW:
		rsp->hdr.Status.CifsError = NT_STATUS_BUFFER_OVERFLOW;
		break;
	case -ETIMEDOUT:
		rsp->hdr.Status.CifsError = NT_STATUS_IO_TIMEOUT;
		break;
	case -EOPNOTSUPP:
		rsp->hdr.Status.CifsError = NT_STATUS_NOT_SUPPORTED;
		break;
	case -EMFILE:
		rsp->hdr.Status.CifsError = NT_STATUS_TOO_MANY_OPENED_FILES;
		break;
	default:
		rsp->hdr.Status.CifsError = NT_STATUS_NO_MEMORY;
		break;
	}

	kfree(name);
	return rc;
}

/**
 * smb_nt_create_andx() - file open request handler
 * @smb_work:	smb work containing nt open command
 *
 * Return:	0 on success, otherwise error
 */
int smb_nt_create_andx(struct smb_work *smb_work)
{
	OPEN_REQ *req = (OPEN_REQ *)smb_work->buf;
	OPEN_RSP *rsp = (OPEN_RSP *)smb_work->rsp_buf;
	OPEN_EXT_RSP *ext_rsp = (OPEN_EXT_RSP *)smb_work->rsp_buf;
	struct connection *conn = smb_work->conn;
	struct cifsd_sess *sess = smb_work->sess;
	struct path path;
	struct kstat stat;
	int oplock_flags, file_info, open_flags, access_flags;
	char *name;
	char *conv_name;
	bool file_present = true, extended_reply;
	__u64 alloc_size = 0, create_time;
	__u16 fid;
	umode_t mode = 0;
	int err;
	int create_directory = 0;
	char *src;
	char *root = NULL;
	bool is_unicode;
	bool is_relative_root = false;
	struct cifsd_file *fp;


	rsp->hdr.Status.CifsError = NT_STATUS_UNSUCCESSFUL;
	if (smb_work->tcon->share->is_pipe == true) {
		cifsd_debug("create pipe on IPC\n");
		return create_andx_pipe(smb_work);
	}

	if (le32_to_cpu(req->CreateOptions) & FILE_OPEN_BY_FILE_ID_LE) {
		cifsd_debug("file open with FID is not supported\n");
		rsp->hdr.Status.CifsError = NT_STATUS_NOT_SUPPORTED;
		return -EINVAL;
	}

	if (req->CreateOptions & FILE_DELETE_ON_CLOSE_LE) {
		if (le32_to_cpu(req->DesiredAccess) &&
				!(le32_to_cpu(req->DesiredAccess) & DELETE)) {
			rsp->hdr.Status.CifsError = NT_STATUS_ACCESS_DENIED;
			return -EPERM;
		}
	}

	if (le32_to_cpu(req->CreateOptions) & FILE_DIRECTORY_FILE_LE) {
		cifsd_debug("GOT Create Directory via CREATE ANDX\n");
		create_directory = 1;
	}

	/*
	 * Filename is relative to this root directory FID, instead of
	 * tree connect point. Find root dir name from this FID and
	 * prepend root dir name in filename.
	 */
	if (req->RootDirectoryFid) {
		cifsd_debug("path lookup relative to RootDirectoryFid\n");

		is_relative_root = true;
		fp = get_id_from_fidtable(sess, req->RootDirectoryFid);
		if (fp)
			root = (char *)fp->filp->f_path.dentry->d_name.name;
		else {
			rsp->hdr.Status.CifsError = NT_STATUS_INVALID_HANDLE;
			memset(&rsp->hdr.WordCount, 0, 3);
			return -EINVAL;
		}
	}

	/* here allocated +2 (UNI '\0') length for both ASCII & UNI
	   to avoid unnecessary if/else check */
	src = kzalloc(req->NameLength + 2, GFP_KERNEL);
	if (!src) {
		rsp->hdr.Status.CifsError =
			NT_STATUS_NO_MEMORY;

		return -ENOMEM;
	}

	if (is_smbreq_unicode(&req->hdr)) {
		memcpy(src, req->fileName + 1, req->NameLength);
		is_unicode = true;

		if (req->hdr.Flags & SMBFLG_CASELESS)
			UniStrlwr((wchar_t *)src);
	} else {
		memcpy(src, req->fileName, req->NameLength);
		is_unicode = false;

		if (req->hdr.Flags & SMBFLG_CASELESS) {
			char *ptr = (char *)src;

			for (; *ptr; ptr++)
				*ptr = tolower(*ptr);
		}
	}

	name = smb_strndup_from_utf16(src, PATH_MAX, is_unicode,
			conn->local_nls);
	kfree(src);

	if (IS_ERR(name)) {
		if (PTR_ERR(name) == -ENOMEM) {
			cifsd_err("failed to allocate memory\n");
			rsp->hdr.Status.CifsError =
				NT_STATUS_NO_MEMORY;
		} else
			rsp->hdr.Status.CifsError =
				NT_STATUS_OBJECT_NAME_INVALID;

		return PTR_ERR(name);
	}

	if (is_relative_root) {
		int org_len = strnlen(name, PATH_MAX);
		int add_len = strnlen(root, PATH_MAX);
		char *full_name;

		/* +3 for: '\'<root>'\' & '\0' */
		full_name = kzalloc(org_len + add_len + 3, GFP_KERNEL);
		if (!full_name) {
			kfree(name);
			rsp->hdr.Status.CifsError = NT_STATUS_NO_MEMORY;
			return -ENOMEM;
		}

		snprintf(full_name, add_len + 3, "\\%s\\", root);
		strncat(full_name, name, org_len);
		kfree(name);
		name = full_name;
	}

	root = strrchr(name, '\\');
	if (root) {
		root++;
		if ((root[0] == '*' || root[0] == '/') && (root[1] == '\0')) {
			rsp->hdr.Status.CifsError =
				NT_STATUS_OBJECT_NAME_INVALID;
			kfree(name);
			return -EINVAL;
		}
	}

	conv_name = smb_get_name(name, PATH_MAX, smb_work, true);
	kfree(name);
	if (IS_ERR(conv_name))
		return PTR_ERR(conv_name);

	err = smb_kern_path(conv_name, 0, &path,
			(req->hdr.Flags & SMBFLG_CASELESS) &&
			!create_directory);
	if (err) {
		file_present = false;
		cifsd_debug("can not get linux path for %s, err = %d\n",
				conv_name, err);
	} else {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
		err = vfs_getattr(&path, &stat, STATX_BASIC_STATS,
			AT_STATX_SYNC_AS_STAT);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
		err = vfs_getattr(&path, &stat);
#else
		err = vfs_getattr(path.mnt, path.dentry, &stat);
#endif
		if (err) {
			cifsd_err("can not stat %s, err = %d\n",
				conv_name, err);
			goto free_path;
		}
	}

	if (file_present && (req->CreateOptions & FILE_NON_DIRECTORY_FILE_LE) &&
			S_ISDIR(stat.mode)) {
		cifsd_debug("Can't open dir %s, request is to open file\n",
			       conv_name);
		if (!(((struct smb_hdr *)smb_work->buf)->Flags2 &
					SMBFLG2_ERR_STATUS)) {
			rsp->hdr.Status.DosError.ErrorClass = ERRDOS;
			rsp->hdr.Status.DosError.Error = ERRfilexists;
		} else
			rsp->hdr.Status.CifsError =
				NT_STATUS_OBJECT_NAME_COLLISION;

		memset(&rsp->hdr.WordCount, 0, 3);

		goto free_path;
	}

	if (file_present && create_directory && !S_ISDIR(stat.mode)) {
		cifsd_debug("Can't open file %s, request is to open dir\n",
				conv_name);
		if (!(((struct smb_hdr *)smb_work->buf)->Flags2 &
					SMBFLG2_ERR_STATUS)) {
			ntstatus_to_dos(NT_STATUS_NOT_A_DIRECTORY,
					&rsp->hdr.Status.DosError.ErrorClass,
					&rsp->hdr.Status.DosError.Error);
		} else
			rsp->hdr.Status.CifsError =
				NT_STATUS_NOT_A_DIRECTORY;

		memset(&rsp->hdr.WordCount, 0, 3);

		goto free_path;
	}

	oplock_flags = le32_to_cpu(req->OpenFlags);
	extended_reply = oplock_flags & REQ_EXTENDED_INFO;
	open_flags = file_create_dispostion_flags(
			le32_to_cpu(req->CreateDisposition), file_present);

	if (open_flags < 0) {
		cifsd_debug("create_dispostion returned %d\n", err);
		if (file_present) {
			if (!(((struct smb_hdr *)smb_work->buf)->Flags2 &
						SMBFLG2_ERR_STATUS)) {
				rsp->hdr.Status.DosError.ErrorClass = ERRDOS;
				rsp->hdr.Status.DosError.Error = ERRfilexists;
			} else
				rsp->hdr.Status.CifsError =
					NT_STATUS_OBJECT_NAME_COLLISION;

			memset(&rsp->hdr.WordCount, 0, 3);

			goto free_path;
		}
		else
			goto out;
	} else {
		if (file_present && S_ISFIFO(stat.mode))
			open_flags |= O_NONBLOCK;

		if (le32_to_cpu(req->CreateOptions) & FILE_WRITE_THROUGH_LE)
			open_flags |= O_SYNC;
	}

	access_flags = convert_generic_access_flags(
			le32_to_cpu(req->DesiredAccess),
			&open_flags, le32_to_cpu(req->FileAttributes));

	mode |= S_IRWXUGO;
	if (le32_to_cpu(req->FileAttributes) & ATTR_READONLY)
		mode &= ~S_IWUGO;

	/* TODO:
	 * - check req->ShareAccess for sharing file among different process
	 * - check req->FileAttributes for special/readonly file attrib
	 * - check req->SecurityFlags for client security context tracking
	 * - check req->ImpersonationLevel
	 */

	if (!smb_work->tcon->writeable) {
		if (!file_present) {
			if (open_flags & O_CREAT) {
				err = -EACCES;
				cifsd_debug("returning as user does not have permission to write\n");
			} else {
				err = -ENOENT;
				cifsd_debug("returning as file does not exist\n");
			}
		}
		goto free_path;
	}

	cifsd_debug("open_flags = 0x%x\n", open_flags);
	if (!file_present && (open_flags & O_CREAT)) {

		if (!create_directory) {
			mode |= S_IFREG;
			err = smb_vfs_create(conv_name, mode);
			if (err)
				goto out;
		} else {
			err = smb_vfs_mkdir(conv_name, mode);
			if (err) {
				cifsd_err("Can't create directory %s",
					conv_name);
				goto out;
			}
		}

		err = smb_kern_path(conv_name, 0, &path, 0);
		if (err) {
			cifsd_err("cannot get linux path, err = %d\n", err);
			goto out;
		}
	}

	/* open  file and get FID */
	err = smb_dentry_open(smb_work, &path, open_flags,
			      &fid, &oplock_flags,
				le32_to_cpu(req->CreateOptions), file_present);
	if (err)
		goto free_path;

	fp = get_id_from_fidtable(sess, fid);
	if (fp) {
		struct cifsd_mfile *mfp;

		mfp = mfp_lookup(FP_INODE(fp));
		if (!mfp) {
			mfp = kmalloc(sizeof(struct cifsd_mfile), GFP_KERNEL);
			if (!mfp) {
				err = -ENOMEM;
				goto free_path;
			}

			mfp_init(mfp, FP_INODE(fp));
		}

		/* Add fp to master fp list. */
		list_add(&fp->node, &mfp->m_fp_list);
		atomic_inc(&mfp->m_count);
		fp->f_mfp = mfp;

		if (le32_to_cpu(req->DesiredAccess) & DELETE)
			fp->is_nt_open = 1;
		if ((le32_to_cpu(req->DesiredAccess) & DELETE) &&
				(req->CreateOptions & FILE_DELETE_ON_CLOSE_LE))
			mfp->m_flags |= S_DEL_ON_CLS;
	}

	/* open success, send back response */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
	err = vfs_getattr(&path, &stat, STATX_BASIC_STATS,
		AT_STATX_SYNC_AS_STAT);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
	err = vfs_getattr(&path, &stat);
#else
	err = vfs_getattr(path.mnt, path.dentry, &stat);
#endif
	if (err) {
		cifsd_err("cannot get stat information\n");
		goto free_path;
	}

	if (file_present) {
		if (!(open_flags & O_TRUNC))
			file_info = F_OPENED;
		else
			file_info = F_OVERWRITTEN;
	} else {
		file_info = F_CREATED;
	}

	alloc_size = le64_to_cpu(req->AllocationSize);
	if (alloc_size && (file_info == F_CREATED ||
				file_info == F_OVERWRITTEN)) {
		if (alloc_size > stat.size) {
			err = smb_vfs_truncate(sess, NULL, (uint64_t)fid,
				alloc_size);
			if (err) {
				cifsd_err("failed to expand file, err = %d\n",
						err);
				goto free_path;
			}
		}
	}

	/* prepare response buffer */
	rsp->hdr.Status.CifsError = NT_STATUS_OK;

	rsp->OplockLevel = oplock_flags;
	rsp->Fid = fid;

	if ((le32_to_cpu(req->CreateDisposition) == FILE_SUPERSEDE) &&
			(file_info == F_OVERWRITTEN))
		rsp->CreateAction = cpu_to_le32(F_SUPERSEDED);
	else
		rsp->CreateAction = cpu_to_le32(file_info);


	create_time = min3(cifs_UnixTimeToNT(stat.ctime),
			cifs_UnixTimeToNT(stat.mtime),
			cifs_UnixTimeToNT(stat.atime));

	if (!create_time)
		create_time = min(cifs_UnixTimeToNT(stat.ctime),
				cifs_UnixTimeToNT(stat.mtime));

	rsp->CreationTime = cpu_to_le64(create_time);
	rsp->LastAccessTime = cpu_to_le64(cifs_UnixTimeToNT(stat.atime));
	rsp->LastWriteTime = cpu_to_le64(cifs_UnixTimeToNT(stat.mtime));
	rsp->ChangeTime = cpu_to_le64(cifs_UnixTimeToNT(stat.mtime));

	rsp->FileAttributes = cpu_to_le32(smb_get_dos_attr(&stat));
	rsp->AllocationSize = cpu_to_le64(stat.blocks << 9);
	rsp->EndOfFile = cpu_to_le64(stat.size);
	/* TODO: is it normal file, named pipe, printer, modem etc*/
	rsp->FileType = 0;
	/* status of named pipe*/
	rsp->DeviceState = 0;
	rsp->DirectoryFlag = S_ISDIR(stat.mode) ? 1 : 0;
	if (extended_reply) {
		struct inode *inode;
		rsp->hdr.WordCount = 50;
		memset(&ext_rsp->VolId, 0, 16);
		if (fp) {
			inode = file_inode(fp->filp);
			ext_rsp->fid = inode->i_ino;
			if (S_ISDIR(inode->i_mode) ||
			    (fp->filp->f_mode & FMODE_WRITE))
				ext_rsp->MaxAccess = FILE_GENERIC_ALL;
			else
				ext_rsp->MaxAccess = FILE_GENERIC_READ|
						     FILE_EXECUTE;
		} else {
			ext_rsp->MaxAccess = FILE_GENERIC_ALL;
			ext_rsp->fid = 0;
		}

		ext_rsp->ByteCount = 0;

	} else {
		rsp->hdr.WordCount = 34;
		rsp->ByteCount = 0;
	}
	inc_rfc1001_len(&rsp->hdr, (rsp->hdr.WordCount * 2 + 0));

free_path:
	path_put(&path);
out:
	switch (err) {
	case 0:
		conn->stats.open_files_count++;
		break;
	case -ENOSPC:
		rsp->hdr.Status.CifsError = NT_STATUS_DISK_FULL;
		break;
	case -EMFILE:
		rsp->hdr.Status.CifsError =
			NT_STATUS_TOO_MANY_OPENED_FILES;
		break;
	case -EINVAL:
		rsp->hdr.Status.CifsError = NT_STATUS_NO_SUCH_USER;
		break;
	case -EACCES:
		rsp->hdr.Status.CifsError = NT_STATUS_ACCESS_DENIED;
		break;
	case -ENOENT:
		rsp->hdr.Status.CifsError = NT_STATUS_OBJECT_NAME_NOT_FOUND;
		break;
	default:
		rsp->hdr.Status.CifsError =
			NT_STATUS_UNEXPECTED_IO_ERROR;
	}
	smb_put_name(conv_name);

	if (!rsp->hdr.WordCount)
		return err;

	/* this is an ANDx command ? */
	if (req->AndXCommand == 0xFF) {
		rsp->AndXCommand = SMB_NO_MORE_ANDX_COMMAND;
		rsp->AndXReserved = 0;
		rsp->AndXOffset = 0;
		return err;
	} else {
		/* adjust response */
		rsp->AndXOffset = get_rfc1002_length(&rsp->hdr);
		rsp->AndXCommand = req->AndXCommand;
		rsp->AndXReserved = 0;

		return rsp->AndXCommand; /* More processing required */
	}
}

/**
 * smb_close_pipe() - ipc pipe close request handler
 * @smb_work:	smb work containing close command
 *
 * Return:	0 on success, otherwise error
 */
int smb_close_pipe(struct smb_work *smb_work)
{
	CLOSE_REQ *req = (CLOSE_REQ *)smb_work->buf;
	CLOSE_RSP *rsp = (CLOSE_RSP *)smb_work->rsp_buf;
	struct cifsd_pipe *pipe_desc;
	int id;
	int rc = 0;

	id = le16_to_cpu(req->FileID);
	pipe_desc = get_pipe_desc(smb_work->sess, id);
	if (!pipe_desc) {
		cifsd_debug("Pipe not opened or invalid in Pipe id\n");
		if (pipe_desc)
			cifsd_debug("Incoming id = %d opened pipe id = %d\n",
					id, pipe_desc->id);
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_HANDLE;
		return -EINVAL;
	}

	rc = cifsd_sendmsg(smb_work->sess,
			CIFSD_KEVENT_DESTROY_PIPE, pipe_desc->pipe_type,
			0, NULL, 0);
	if (rc)
		cifsd_err("failed to send event, err %d\n", rc);
	rc = close_pipe_id(smb_work->sess, pipe_desc->pipe_type);
	return rc;
}

/**
 * smb_close() - ipc pipe close request handler
 * @smb_work:	smb work containing close command
 *
 * Return:	0 on success, otherwise error
 */
int smb_close(struct smb_work *smb_work)
{
	CLOSE_REQ *req = (CLOSE_REQ *)smb_work->buf;
	CLOSE_RSP *rsp = (CLOSE_RSP *)smb_work->rsp_buf;
	struct connection *conn = smb_work->conn;
	int err = 0;

	cifsd_debug("SMB_COM_CLOSE called for fid %u\n", req->FileID);

	if (smb_work->tcon->share->is_pipe == true) {
		err = smb_close_pipe(smb_work);
		if (err < 0)
			goto out;
		goto IPC_out;
	}

	/* TODO: linux cifs client does not send LastWriteTime,
	   need to check if windows client use this field */
	if ((req->LastWriteTime > 0) && (req->LastWriteTime < 0xFFFFFFFF))
		cifsd_info("need to set last modified time before close\n");

	err = close_id(smb_work->sess, req->FileID, 0);
	if (err)
		goto out;

IPC_out:
	/* file close success, return response to server */
	rsp->hdr.Status.CifsError = NT_STATUS_OK;
	rsp->hdr.WordCount = 0;
	rsp->ByteCount = 0;

out:
	if (err)
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_HANDLE;
	else
		conn->stats.open_files_count--;

	return err;
}

/**
 * smb_read_andx_pipe() - read from ipc pipe request handler
 * @smb_work:	smb work containing read command
 *
 * Return:	0 on success, otherwise error
 */
int smb_read_andx_pipe(struct smb_work *smb_work)
{
	READ_REQ *req = (READ_REQ *)smb_work->buf;
	READ_RSP *rsp = (READ_RSP *)smb_work->rsp_buf;
	struct cifsd_pipe *pipe_desc;
	char *data_buf;
	int ret = 0, nbytes = 0;
	int id;
	unsigned int count;
	unsigned int rsp_buflen = MAX_CIFS_SMALL_BUFFER_SIZE - sizeof(READ_RSP);
	struct cifsd_uevent *ev;
	rsp_buflen = min((unsigned int)
			(MAX_CIFS_SMALL_BUFFER_SIZE - sizeof(READ_RSP)),
			rsp_buflen);

	id = le16_to_cpu(req->Fid);
	pipe_desc = get_pipe_desc(smb_work->sess, id);
	if (!pipe_desc) {
		cifsd_debug("Pipe not opened or invalid in Pipe id\n");
		if (pipe_desc)
			cifsd_debug("Incoming id = %d opened pipe id = %d\n",
					id, pipe_desc->id);
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_HANDLE;
		return ret;
	}

	count = min_t(unsigned int, le16_to_cpu(req->MaxCount), rsp_buflen);
	data_buf = (char *) (&rsp->ByteCount) + sizeof(rsp->ByteCount);

	ret = cifsd_sendmsg(smb_work->sess, CIFSD_KEVENT_READ_PIPE,
			pipe_desc->pipe_type,
			0, NULL, rsp_buflen);
	if (ret)
		cifsd_err("failed to send event, err %d\n", ret);
	else {
		ev = &pipe_desc->ev;
		nbytes = ev->u.r_pipe_rsp.read_count;
		if (ev->error < 0 || !nbytes) {
			cifsd_debug("Read bytes zero from pipe\n");
			rsp->hdr.Status.CifsError =
				NT_STATUS_UNEXPECTED_IO_ERROR;
			return -EINVAL;
		}

		memcpy(data_buf, pipe_desc->rsp_buf, nbytes);
		smb_work->sess->ev_state = NETLINK_REQ_COMPLETED;
	}

	rsp->hdr.Status.CifsError = NT_STATUS_OK;
	rsp->hdr.WordCount = 12;
	rsp->Remaining = 0;
	rsp->DataCompactionMode = 0;
	rsp->DataCompactionMode = 0;
	rsp->Reserved = 0;
	rsp->DataLength = cpu_to_le16(nbytes & 0xFFFF);
	rsp->DataOffset = cpu_to_le16(sizeof(READ_RSP) -
			sizeof(rsp->hdr.smb_buf_length));
	rsp->DataLengthHigh = cpu_to_le16(nbytes >> 16);
	rsp->Reserved2 = 0;

	rsp->ByteCount = cpu_to_le16(nbytes);
	inc_rfc1001_len(&rsp->hdr, (rsp->hdr.WordCount * 2 + nbytes));

	/* this is an ANDx command ? */
	if (req->AndXCommand == 0xFF) {
		rsp->AndXCommand = SMB_NO_MORE_ANDX_COMMAND;
		rsp->AndXReserved = 0;
		rsp->AndXOffset = 0;
		return ret;
	} else {
		/* adjust response */
		rsp->AndXOffset = get_rfc1002_length(&rsp->hdr);
		rsp->AndXCommand = req->AndXCommand;
		rsp->AndXReserved = 0;
		return rsp->AndXCommand; /* More processing required */
	}

}

/**
 * smb_read_andx() - read request handler
 * @smb_work:	smb work containing read command
 *
 * Return:	0 on success, otherwise error
 */
int smb_read_andx(struct smb_work *smb_work)
{
	struct connection *conn = smb_work->conn;
	READ_REQ *req = (READ_REQ *)smb_work->buf;
	READ_RSP *rsp = (READ_RSP *)smb_work->rsp_buf;
	loff_t pos;
	size_t count;
	ssize_t nbytes;
	int err = 0;

	if (smb_work->tcon->share->is_pipe == true)
		return smb_read_andx_pipe(smb_work);

	pos = le32_to_cpu(req->OffsetLow);
	if (req->hdr.WordCount == 12)
		pos |= ((loff_t)le32_to_cpu(req->OffsetHigh) << 32);

	count = le16_to_cpu(req->MaxCount);
	if (conn->srv_cap & CAP_LARGE_READ_X)
		count |= le32_to_cpu(req->MaxCountHigh) << 16;

	if (count > CIFS_DEFAULT_IOSIZE) {
		cifsd_debug("read size(%zu) exceeds max size(%u)\n",
				count, CIFS_DEFAULT_IOSIZE);
		cifsd_debug("limiting read size to max size(%u)\n",
				CIFS_DEFAULT_IOSIZE);
		count = CIFS_DEFAULT_IOSIZE;
	}

	cifsd_debug("fid %u, offset %lld, count %zu\n", req->Fid, pos, count);
	nbytes = smb_vfs_read(smb_work->sess, req->Fid, 0, &smb_work->rdata_buf,
		count, &pos);
	if (nbytes < 0) {
		err = nbytes;
		goto out;
	}

	/* read success, prepare response */
	rsp->hdr.Status.CifsError = NT_STATUS_OK;
	rsp->hdr.WordCount = 12;
	rsp->Remaining = 0;
	rsp->DataCompactionMode = 0;
	rsp->DataCompactionMode = 0;
	rsp->Reserved = 0;
	rsp->DataLength = cpu_to_le16(nbytes & 0xFFFF);
	rsp->DataOffset = cpu_to_le16(sizeof(READ_RSP) -
			sizeof(rsp->hdr.smb_buf_length));
	rsp->DataLengthHigh = cpu_to_le16(nbytes >> 16);
	rsp->Reserved2 = 0;

	rsp->ByteCount = cpu_to_le16(nbytes);
	inc_rfc1001_len(&rsp->hdr, (rsp->hdr.WordCount * 2));
	smb_work->rrsp_hdr_size = get_rfc1002_length(rsp) + 4;
	smb_work->rdata_cnt = nbytes;
	inc_rfc1001_len(&rsp->hdr, nbytes);

	/* this is an ANDx command ? */
	if (req->AndXCommand == 0xFF) {
		rsp->AndXCommand = SMB_NO_MORE_ANDX_COMMAND;
		rsp->AndXReserved = 0;
		rsp->AndXOffset = 0;
		return err;
	} else {
		/* adjust response */
		rsp->AndXOffset = get_rfc1002_length(&rsp->hdr);
		rsp->AndXCommand = req->AndXCommand;
		rsp->AndXReserved = 0;
		return rsp->AndXCommand; /* More processing required */
	}

out:
	if (err)
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_HANDLE;
	return err;
}

/**
 * smb_write() - write request handler
 * @smb_work:	smb work containing write command
 *
 * Return:	0 on success, otherwise error
 */
int smb_write(struct smb_work *smb_work)
{
	WRITE_REQ_32BIT *req = (WRITE_REQ_32BIT *)smb_work->buf;
	WRITE_RSP_32BIT *rsp = (WRITE_RSP_32BIT *)smb_work->rsp_buf;
	loff_t pos;
	size_t count;
	char *data_buf;
	ssize_t nbytes = 0;
	int err = 0;

	if (req->hdr.WordCount != 5)
		goto out;

	pos = le32_to_cpu(req->Offset);
	count = le16_to_cpu(req->Length);
	data_buf = req->Data;

	cifsd_debug("fid %u, offset %lld, count %zu\n", req->Fid, pos, count);
	if (!count) {
		err = smb_vfs_truncate(smb_work->sess, NULL, (uint64_t)req->Fid,
			pos);
		nbytes = 0;
	} else
		err = smb_vfs_write(smb_work->sess, req->Fid, 0, data_buf,
			count, &pos, 0, &nbytes);

out:
	rsp->hdr.WordCount = 1;
	rsp->Written = cpu_to_le16(nbytes & 0xFFFF);
	rsp->ByteCount = 0;
	inc_rfc1001_len(&rsp->hdr, (rsp->hdr.WordCount * 2));

	if (!err) {
		rsp->hdr.Status.CifsError = NT_STATUS_OK;
		return 0;
	}

	if (err == -ENOSPC || err == -EFBIG)
		rsp->hdr.Status.CifsError = NT_STATUS_DISK_FULL;
	else
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_HANDLE;
	return err;
}

/**
 * smb_write_andx_pipe() - write on pipe request handler
 * @smb_work:	smb work containing write command
 *
 * Return:	0 on success, otherwise error
 */
int smb_write_andx_pipe(struct smb_work *smb_work)
{
	WRITE_REQ *req = (WRITE_REQ *)smb_work->buf;
	WRITE_RSP *rsp = (WRITE_RSP *)smb_work->rsp_buf;
	struct cifsd_pipe *pipe_desc;
	int ret = 0;
	size_t count = 0;
	int id;
	struct cifsd_uevent *ev;

	id = le16_to_cpu(req->Fid);
	pipe_desc = get_pipe_desc(smb_work->sess, id);
	if (!pipe_desc) {
		cifsd_err("Pipe not opened or invalid in Pipe id %d\n", id);
		if (pipe_desc)
			cifsd_debug("Incoming id = %d opened pipe id = %d\n",
					id, pipe_desc->id);
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_HANDLE;
		return ret;
	}

	count = le16_to_cpu(req->DataLengthLow);
	if (smb_work->conn->srv_cap & CAP_LARGE_WRITE_X)
		count |= (le16_to_cpu(req->DataLengthHigh) << 16);

	ret = cifsd_sendmsg(smb_work->sess, CIFSD_KEVENT_WRITE_PIPE,
			pipe_desc->pipe_type,
			count, req->Data, 0);
	if (ret)
		cifsd_err("failed to send event, err %d\n", ret);
	else {
		ev = &pipe_desc->ev;
		ret = ev->error;
		if (ret == -EOPNOTSUPP) {
			rsp->hdr.Status.CifsError = NT_STATUS_NOT_SUPPORTED;
			return ret;
		} else if (ret) {
			rsp->hdr.Status.CifsError = NT_STATUS_INVALID_HANDLE;
			return ret;
		}

		count = ev->u.w_pipe_rsp.write_count;
		smb_work->sess->ev_state = NETLINK_REQ_COMPLETED;
	}

	rsp->hdr.Status.CifsError = NT_STATUS_OK;
	rsp->hdr.WordCount = 6;
	rsp->Count = cpu_to_le16(count & 0xFFFF);
	rsp->Remaining = 0;
	rsp->CountHigh = cpu_to_le16(count >> 16);
	rsp->Reserved = 0;
	rsp->ByteCount = 0;
	inc_rfc1001_len(&rsp->hdr, (rsp->hdr.WordCount * 2));
	if (req->AndXCommand == 0xFF) {
		rsp->AndXCommand = SMB_NO_MORE_ANDX_COMMAND;
		rsp->AndXReserved = 0;
		rsp->AndXOffset = 0;
		return ret;
	} else {
		/* adjust response */
		rsp->AndXOffset = get_rfc1002_length(&rsp->hdr);
		rsp->AndXCommand = req->AndXCommand;
		rsp->AndXReserved = 0;
		return rsp->AndXCommand; /* More processing required */
	}
}

/**
 * smb_write_andx() - andx write request handler
 * @smb_work:	smb work containing write command
 *
 * Return:	0 on success, otherwise error
 */
int smb_write_andx(struct smb_work *smb_work)
{
	struct connection *conn = smb_work->conn;
	WRITE_REQ *req = (WRITE_REQ *)smb_work->buf;
	WRITE_RSP *rsp = (WRITE_RSP *)smb_work->rsp_buf;
	bool writethrough = false;
	loff_t pos;
	size_t count;
	ssize_t nbytes = 0;
	char *data_buf;
	int err = 0;

	if (smb_work->tcon->share->is_pipe == true) {
		cifsd_debug("Write ANDX called for IPC$");
		return smb_write_andx_pipe(smb_work);
	}

	pos = le32_to_cpu(req->OffsetLow);
	if (req->hdr.WordCount == 14)
		pos |= ((loff_t)le32_to_cpu(req->OffsetHigh) << 32);

	writethrough = (le16_to_cpu(req->WriteMode) == 1);

	count = le16_to_cpu(req->DataLengthLow);
	if (conn->srv_cap & CAP_LARGE_WRITE_X)
		count |= (le16_to_cpu(req->DataLengthHigh) << 16);

	if (count > CIFS_DEFAULT_IOSIZE) {
		cifsd_debug("write size(%zu) exceeds max size(%u)\n",
				count, CIFS_DEFAULT_IOSIZE);
		cifsd_debug("limiting write size to max size(%u)\n",
				CIFS_DEFAULT_IOSIZE);
		count = CIFS_DEFAULT_IOSIZE;
	}

	if (le16_to_cpu(req->DataOffset) ==
			(offsetof(struct smb_com_write_req, Data) - 4)) {
		data_buf = (char *)&req->Data[0];
	} else {
		if ((le16_to_cpu(req->DataOffset) > get_rfc1002_length(req)) ||
				(le16_to_cpu(req->DataOffset) +
				 count > get_rfc1002_length(req))) {
			cifsd_err("invalid write data offset %u, smb_len %u\n",
					le16_to_cpu(req->DataOffset),
					get_rfc1002_length(req));
			err = -EINVAL;
			goto out;
		}

		data_buf = (char *)(((char *)&req->hdr.Protocol) +
				le16_to_cpu(req->DataOffset));
	}

	cifsd_debug("fid %u, offset %lld, count %zu\n", req->Fid, pos, count);
	err = smb_vfs_write(smb_work->sess, req->Fid, 0, data_buf, count, &pos,
			writethrough, &nbytes);
	if (err < 0)
		goto out;

	/* write success, prepare response */
	rsp->hdr.Status.CifsError = NT_STATUS_OK;
	rsp->hdr.WordCount = 6;
	rsp->Count = cpu_to_le16(nbytes & 0xFFFF);
	rsp->Remaining = 0;
	rsp->CountHigh = cpu_to_le16(nbytes >> 16);
	rsp->Reserved = 0;
	rsp->ByteCount = 0;
	inc_rfc1001_len(&rsp->hdr, (rsp->hdr.WordCount * 2));

	/* this is an ANDx command ? */
	if (req->AndXCommand == 0xFF) {
		rsp->AndXCommand = SMB_NO_MORE_ANDX_COMMAND;
		rsp->AndXReserved = 0;
		rsp->AndXOffset = 0;
		return err;
	} else {
		/* adjust response */
		rsp->AndXOffset = get_rfc1002_length(&rsp->hdr);
		rsp->AndXCommand = req->AndXCommand;
		rsp->AndXReserved = 0;
		return rsp->AndXCommand; /* More processing required */
	}

out:
	if (err == -ENOSPC || err == -EFBIG)
		rsp->hdr.Status.CifsError = NT_STATUS_DISK_FULL;
	else
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_HANDLE;
	return err;
}

/**
 * smb_echo() - echo(ping) request handler
 * @smb_work:	smb work containing echo command
 *
 * Return:	0 on success, otherwise error
 */
int smb_echo(struct smb_work *smb_work)
{
	ECHO_REQ *req = (ECHO_REQ *)smb_work->buf;
	ECHO_RSP *rsp = (ECHO_RSP *)smb_work->rsp_buf;
	__u16 data_count;
	int i;

	cifsd_debug("SMB_COM_ECHO called with echo count %u\n",
			le16_to_cpu(req->EchoCount));

	if (le16_to_cpu(req->EchoCount) > 1)
		smb_work->multiRsp = 1;

	data_count = cpu_to_le16(req->ByteCount);
	/* send echo response to server */
	rsp->hdr.Status.CifsError = NT_STATUS_OK;
	rsp->hdr.WordCount = 1;
	rsp->ByteCount = cpu_to_le16(data_count);

	memcpy(rsp->Data, req->Data, data_count);
	inc_rfc1001_len(&rsp->hdr, (rsp->hdr.WordCount * 2) + data_count);

	/* Send req->EchoCount - 1 number of ECHO response now &
	   if SMB CANCEL for Echo comes don't send response */
	for (i = 1; i < le16_to_cpu(req->EchoCount) &&
	     !smb_work->send_no_response; i++) {
		rsp->SequenceNumber = cpu_to_le16(i);
		smb_send_rsp(smb_work);
	}

	/* Last echo response */
	rsp->SequenceNumber = cpu_to_le16(i);
	smb_work->multiRsp = 0;

	return 0;
}

/**
 * smb_flush() - file sync - flush request handler
 * @smb_work:	smb work containing flush command
 *
 * Return:	0 on success, otherwise error
 */
int smb_flush(struct smb_work *smb_work)
{
	FLUSH_REQ *req = (FLUSH_REQ *)smb_work->buf;
	FLUSH_RSP *rsp = (FLUSH_RSP *)smb_work->rsp_buf;
	int err;

	cifsd_debug("SMB_COM_FLUSH called for fid %u\n", req->FileID);

	err = smb_vfs_fsync(smb_work->sess, req->FileID, 0);
	if (err)
		goto out;

	/* file fsync success, return response to server */
	rsp->hdr.Status.CifsError = NT_STATUS_OK;
	rsp->hdr.WordCount = 0;
	rsp->ByteCount = 0;
	return err;

out:
	if (err)
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_HANDLE;

	return err;
}

/*****************************************************************************
 * TRANS2 command implentation functions
 *****************************************************************************/

/**
 * convert_delimiter() - convert windows path to unix format or unix format
 *			 to windos path
 * @path:	path to be converted
 * @flags:	1 is to convert windows, 2 is to convert unix
 *
 */
void convert_delimiter(char *path, int flags)
{
	char *pos = path;

	if (flags == 1)
		while ((pos = strchr(pos, '/')))
			*pos = '\\';
	else
		while ((pos = strchr(pos, '\\')))
			*pos = '/';
}

/**
 * convert_to_unix_name() - convert windows name to unix format
 * @path:	name to be converted
 * @tid:	tree id of mathing share
 *
 * Return:	converted name on success, otherwise NULL
 */
char *convert_to_unix_name(char *name, int tid)
{
	struct cifsd_share *share;
	int len;
	char *new_name;

	share = find_matching_share(tid);
	if (!share)
		return NULL;

	len = strlen(share->path);
	len += strlen(name);

	/* for '/' needed for smb2
	 * as '/' is not present in beginning of name*/
	if (name[0] != '/')
		len++;

	/* 1 extra for NULL byte */
	cifsd_debug("new_name len = %d\n", len);
	new_name = kmalloc(len + 1, GFP_KERNEL);

	if (new_name == NULL) {
		cifsd_debug("Failed to allocate memory\n");
		return new_name;
	}


	memcpy(new_name, share->path, strlen(share->path));

	if (name[0] != '/') {
		memset(new_name + strlen(share->path), '/', 1);
		memcpy(new_name + strlen(share->path) + 1, name, strlen(name));
	} else
		memcpy(new_name + strlen(share->path), name, strlen(name));

	*(new_name + len) = '\0';

	return new_name;

}

/**
 * get_filetype() - convert file mode to smb file type
 * @mode:	file mode to be convertd
 *
 * Return:	converted file type
 */
static __u32 get_filetype(mode_t mode)
{
	if (S_ISREG(mode))
		return UNIX_FILE;
	else if (S_ISDIR(mode))
		return UNIX_DIR;
	else if (S_ISLNK(mode))
		return UNIX_SYMLINK;
	else if (S_ISCHR(mode))
		return UNIX_CHARDEV;
	else if (S_ISBLK(mode))
		return UNIX_BLOCKDEV;
	else if (S_ISFIFO(mode))
		return UNIX_FIFO;
	else if (S_ISSOCK(mode))
		return UNIX_SOCKET;

	return UNIX_UNKNOWN;
}

/**
 * init_unix_info() - convert file stat information to smb file info format
 * @unix_info:	smb file information format
 * @stat:	unix file/dir stat information
 */
static void init_unix_info(FILE_UNIX_BASIC_INFO *unix_info, struct kstat *stat)
{

	unix_info->EndOfFile = cpu_to_le64(stat->size);
	unix_info->NumOfBytes = cpu_to_le64(512 * stat->blocks);
	unix_info->LastStatusChange =
			cpu_to_le64(cifs_UnixTimeToNT(stat->ctime));
	unix_info->LastAccessTime =
			cpu_to_le64(cifs_UnixTimeToNT(stat->atime));
	unix_info->LastModificationTime =
			cpu_to_le64(cifs_UnixTimeToNT(stat->mtime));
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
	unix_info->Uid = cpu_to_le64(from_kuid(&init_user_ns, stat->uid));
	unix_info->Gid = cpu_to_le64(from_kgid(&init_user_ns, stat->gid));
#else
	unix_info->Uid = cpu_to_le64(stat->uid);
	unix_info->Gid = cpu_to_le64(stat->gid);
#endif
	unix_info->Type = cpu_to_le32(get_filetype(stat->mode));
	unix_info->DevMajor = cpu_to_le64(MAJOR(stat->rdev));
	unix_info->DevMinor = cpu_to_le64(MINOR(stat->rdev));
	unix_info->UniqueId = cpu_to_le64(stat->ino);
	unix_info->Permissions = cpu_to_le64(stat->mode);
	unix_info->Nlinks = cpu_to_le64(stat->nlink);
}

/**
 * unix_info_to_attr() - convert smb file info format to unix attr format
 * @unix_info:	smb file information format
 * @attrs:	unix file/dir stat information
 *
 * Return:	0
 */
int unix_info_to_attr(FILE_UNIX_BASIC_INFO *unix_info,
		struct iattr *attrs)
{
	if (le64_to_cpu(unix_info->EndOfFile) != NO_CHANGE_64) {
		attrs->ia_size = le64_to_cpu(unix_info->EndOfFile);
		attrs->ia_valid |= ATTR_SIZE;
	}

	if (le64_to_cpu(unix_info->LastStatusChange) != NO_CHANGE_64) {
		attrs->ia_ctime =
			smb_NTtimeToUnix(unix_info->LastStatusChange);
		attrs->ia_valid |= ATTR_CTIME;
	}

	if (le64_to_cpu(unix_info->LastAccessTime) != NO_CHANGE_64) {
		attrs->ia_atime = smb_NTtimeToUnix(unix_info->LastAccessTime);
		attrs->ia_valid |= ATTR_ATIME;
	}

	if (le64_to_cpu(unix_info->LastModificationTime) != NO_CHANGE_64) {
		attrs->ia_mtime =
			smb_NTtimeToUnix(unix_info->LastModificationTime);
		attrs->ia_valid |= ATTR_MTIME;
	}

	if (le64_to_cpu(unix_info->Uid) != NO_CHANGE_64) {
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
		attrs->ia_uid = make_kuid(&init_user_ns,
				le64_to_cpu(unix_info->Uid));
#else
		attrs->ia_uid = le64_to_cpu(unix_info->Uid);
#endif
		attrs->ia_valid |= ATTR_UID;
	}

	if (le64_to_cpu(unix_info->Gid) != NO_CHANGE_64) {
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
		attrs->ia_gid =  make_kgid(&init_user_ns,
				le64_to_cpu(unix_info->Gid));
#else
		attrs->ia_gid = le64_to_cpu(unix_info->Gid);
#endif
		attrs->ia_valid |= ATTR_GID;
	}

	if (le64_to_cpu(unix_info->Permissions) != NO_CHANGE_64) {
		attrs->ia_mode = le64_to_cpu(unix_info->Permissions);
		attrs->ia_valid |= ATTR_MODE;
	}

	switch (le32_to_cpu(unix_info->Type)) {
	case UNIX_FILE:
		attrs->ia_mode |= S_IFREG;
		break;
	case UNIX_DIR:
		attrs->ia_mode |= S_IFDIR;
		break;
	case UNIX_SYMLINK:
		attrs->ia_mode |= S_IFLNK;
		break;
	case UNIX_CHARDEV:
		attrs->ia_mode |= S_IFCHR;
		break;
	case UNIX_BLOCKDEV:
		attrs->ia_mode |= S_IFBLK;
		break;
	case UNIX_FIFO:
		attrs->ia_mode |= S_IFIFO;
		break;
	case UNIX_SOCKET:
		attrs->ia_mode |= S_IFSOCK;
		break;
	default:
		cifsd_err("unknown file type 0x%x\n",
				le32_to_cpu(unix_info->Type));
	}

	return 0;
}

/**
 * unix_to_dos_time() - convert unix time to dos format
 * @ts:		unix style time
 * @time:	store dos style time
 * @date:	store dos style date
 */
void unix_to_dos_time(struct timespec *ts, __le16 *time, __le16 *date)
{
	struct tm t;
	__u16 val;
	time_to_tm(ts->tv_sec,
			(-sys_tz.tz_minuteswest) * 60, &t);


	val = (((unsigned int)(t.tm_mon + 1)) >> 3) | ((t.tm_year - 80) << 1);
	val = ((val & 0xFF) << 8) | (t.tm_mday |
			(((t.tm_mon + 1) & 0x7) << 5));
	*date = cpu_to_le16(val);


	val = ((((unsigned int)t.tm_min >> 3) & 0x7) |
			(((unsigned int)t.tm_hour) << 3));
	val = ((val & 0xFF) << 8) | ((t.tm_sec/2) | ((t.tm_min & 0x7) << 5));
	*time = cpu_to_le16(val);
}

/**
 * cifs_convert_ace() - helper function for convert an Access Control Entry
 *		from cifs wire format to local POSIX xattr format
 * @ace:	local - unix style Access Control Entry format
 * @cifs_ace:	cifs wire Access Control Entry format
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
static void cifs_convert_ace(struct posix_acl_xattr_entry *ace,
			     struct cifs_posix_ace *cifs_ace)
#else
static void cifs_convert_ace(posix_acl_xattr_entry *ace,
			     struct cifs_posix_ace *cifs_ace)
#endif
{
	/* u8 cifs fields do not need le conversion */
	ace->e_perm = cpu_to_le16(cifs_ace->cifs_e_perm);
	ace->e_tag  = cpu_to_le16(cifs_ace->cifs_e_tag);
	ace->e_id   = cpu_to_le32(le64_to_cpu(cifs_ace->cifs_uid));
	return;
}

/**
 * cifs_copy_posix_acl() - Convert ACL from CIFS POSIX wire format to local
 *		Linux POSIX ACL xattr
 * @trgt:	target buffer for storing in local ace format
 * @src:	source buffer in cifs ace format
 * @buflen:	target buffer length
 * @acl_type:	ace type
 * @size_of_data_area:	max buffer size to store ace xattr
 *
 * Return:	size of convert ace xattr on success, otherwise error
 */
static int cifs_copy_posix_acl(char *trgt, char *src, const int buflen,
			       const int acl_type, const int size_of_data_area)
{
	int size =  0;
	int i;
	__u16 count;
	struct cifs_posix_ace *pACE;
	struct cifs_posix_acl *cifs_acl = (struct cifs_posix_acl *)src;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
	struct posix_acl_xattr_header *local_acl = (void *)trgt;
#else
	posix_acl_xattr_header *local_acl = (posix_acl_xattr_header *)trgt;
#endif
	if (le16_to_cpu(cifs_acl->version) != CIFS_ACL_VERSION)
		return -EOPNOTSUPP;

	if (acl_type & ACL_TYPE_ACCESS) {
		count = le16_to_cpu(cifs_acl->access_entry_count);
		pACE = &cifs_acl->ace_array[0];
		size = sizeof(struct cifs_posix_acl);
		size += sizeof(struct cifs_posix_ace) * count;
		/* check if we would go beyond end of SMB */
		if (size_of_data_area < size) {
			cifsd_debug("bad CIFS POSIX ACL size %d vs. %d\n",
				 size_of_data_area, size);
			return -EINVAL;
		}
	} else if (acl_type & ACL_TYPE_DEFAULT) {
		count = le16_to_cpu(cifs_acl->default_entry_count);
		pACE = &cifs_acl->ace_array[0];
		size = sizeof(struct cifs_posix_acl);
		size += sizeof(struct cifs_posix_ace) * count;
		/* check if we would go beyond end of SMB */
		if (size_of_data_area < size)
			return -EINVAL;
	} else {
		/* illegal type */
		return -EINVAL;
	}

	size = posix_acl_xattr_size(count);
	if ((buflen == 0) || (local_acl == NULL)) {
		/* used to query ACL EA size */
	} else if (size > buflen) {
		return -ERANGE;
	} else /* buffer big enough */ {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
		struct posix_acl_xattr_entry *ace = (void *)(local_acl + 1);
#endif
		local_acl->a_version = cpu_to_le32(POSIX_ACL_XATTR_VERSION);
		for (i = 0; i < count; i++) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
			cifs_convert_ace(&ace[i], pACE);
#else
			cifs_convert_ace(&local_acl->a_entries[i], pACE);
#endif
			pACE++;
		}
	}
	return size;
}

/**
 * convert_ace_to_cifs_ace() - helper function to convert ACL from local
 * Linux POSIX ACL xattr to CIFS POSIX wire format to local
 * @cifs_ace:	target buffer for storing in cifs ace format
 * @local_ace:	source buffer in Linux POSIX ACL xattr format
 *
 * Return:	0
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
static __u16 convert_ace_to_cifs_ace(struct cifs_posix_ace *cifs_ace,
				     const struct posix_acl_xattr_entry *local_ace)
#else
static __u16 convert_ace_to_cifs_ace(struct cifs_posix_ace *cifs_ace,
				     const posix_acl_xattr_entry *local_ace)
#endif
{
	__u16 rc = 0; /* 0 = ACL converted ok */

	cifs_ace->cifs_e_perm = le16_to_cpu(local_ace->e_perm);
	cifs_ace->cifs_e_tag =  le16_to_cpu(local_ace->e_tag);
	/* BB is there a better way to handle the large uid? */
	if (local_ace->e_id == cpu_to_le32(-1)) {
		/* Probably no need to le convert -1 on any
		   arch but can not hurt */
		cifs_ace->cifs_uid = cpu_to_le64(-1);
	} else
		cifs_ace->cifs_uid = cpu_to_le64(le32_to_cpu(local_ace->e_id));
	return rc;
}

/**
 * ACL_to_cifs_posix() - ACL from local Linux POSIX xattr to CIFS POSIX ACL
 *		wire format
 * @parm_data:	target buffer for storing in cifs ace format
 * @pACL:	source buffer in cifs ace format
 * @buflen:	target buffer length
 * @acl_type:	ace type
 *
 * Return:	0 on success, otherwise error
 */
static __u16 ACL_to_cifs_posix(char *parm_data, const char *pACL,
			       const int buflen, const int acl_type)
{
	__u16 rc = 0;
	struct cifs_posix_acl *cifs_acl = (struct cifs_posix_acl *)parm_data;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
	struct posix_acl_xattr_header *local_acl = (void *)pACL;
	struct posix_acl_xattr_entry *ace = (void *)(local_acl + 1);
#else
	posix_acl_xattr_header *local_acl = (posix_acl_xattr_header *)pACL;
#endif
	int count;
	int i, j = 0;

	if ((buflen == 0) || (pACL == NULL) || (cifs_acl == NULL))
		return 0;

	count = posix_acl_xattr_count((size_t)buflen);
	cifsd_debug("setting acl with %d entries from buf of length %d and version of %d\n",
		 count, buflen, le32_to_cpu(local_acl->a_version));
	if (le32_to_cpu(local_acl->a_version) != 2) {
		cifsd_debug("unknown POSIX ACL version %d\n",
			 le32_to_cpu(local_acl->a_version));
		return 0;
	}
	if (acl_type == ACL_TYPE_ACCESS) {
		cifs_acl->access_entry_count = cpu_to_le16(count);
		j = 0;
	} else if (acl_type == ACL_TYPE_DEFAULT) {
		cifs_acl->default_entry_count = cpu_to_le16(count);
		if (le16_to_cpu(cifs_acl->access_entry_count))
			j = le16_to_cpu(cifs_acl->access_entry_count);
	} else {
		cifsd_debug("unknown ACL type %d\n", acl_type);
		return 0;
	}
	for (i = 0; i < count; i++, j++) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 9, 0)
		rc = convert_ace_to_cifs_ace(&cifs_acl->ace_array[i], &ace[i]);
#else
		rc = convert_ace_to_cifs_ace(&cifs_acl->ace_array[j],
					&local_acl->a_entries[i]);
#endif
		if (rc != 0) {
			/* ACE not converted */
			break;
		}
	}
	if (rc == 0) {
		rc = (__u16)(count * sizeof(struct cifs_posix_ace));
		/* BB add check to make sure ACL does not overflow SMB */
	}
	return rc;
}

/**
 * smb_get_acl() - handler for query posix acl information
 * @smb_work:	smb work containing posix acl query command
 * @path:	path of file/dir to query acl
 *
 * Return:	0 on success, otherwise error
 */
int smb_get_acl(struct smb_work *smb_work, struct path *path)
{
	TRANSACTION2_RSP *rsp = (TRANSACTION2_RSP *)smb_work->rsp_buf;
	char *buf = NULL;
	int rc = 0, value_len;
	struct cifs_posix_acl *aclbuf;
	__u16 rsp_data_cnt = 0;

	aclbuf = (struct cifs_posix_acl *)(smb_work->rsp_buf +
			sizeof(TRANSACTION2_RSP) + 4);

	aclbuf->version = cpu_to_le16(CIFS_ACL_VERSION);
	aclbuf->default_entry_count = 0;
	aclbuf->access_entry_count = 0;

	/* check if POSIX_ACL_XATTR_ACCESS exists */
	value_len = smb_vfs_getxattr(path->dentry, XATTR_NAME_POSIX_ACL_ACCESS,
			&buf, 1);
	if (value_len > 0) {
		rsp_data_cnt += ACL_to_cifs_posix((char *)aclbuf, buf,
				value_len, ACL_TYPE_ACCESS);
		kvfree(buf);
	}

	/* check if POSIX_ACL_XATTR_DEFAULT exists */
	value_len = smb_vfs_getxattr(path->dentry, XATTR_NAME_POSIX_ACL_DEFAULT,
			&buf, 1);
	if (value_len > 0) {
		rsp_data_cnt += ACL_to_cifs_posix((char *)aclbuf, buf,
				value_len, ACL_TYPE_DEFAULT);
		kvfree(buf);
	}

	if (rsp_data_cnt)
		rsp_data_cnt += sizeof(struct cifs_posix_acl);

	rsp->hdr.Status.CifsError = NT_STATUS_OK;
	rsp->hdr.WordCount = 10;
	rsp->t2.TotalParameterCount = 2;
	rsp->t2.TotalDataCount = cpu_to_le16(rsp_data_cnt);
	rsp->t2.Reserved = 0;
	rsp->t2.ParameterCount = 2;
	rsp->t2.ParameterOffset = 56;
	rsp->t2.ParameterDisplacement = 0;
	rsp->t2.DataCount = rsp->t2.TotalDataCount;
	rsp->t2.DataOffset = 60;
	rsp->t2.DataDisplacement = 0;
	rsp->t2.SetupCount = 0;
	rsp->t2.Reserved1 = 0;
	rsp->ByteCount = cpu_to_le16(rsp_data_cnt + 5);
	inc_rfc1001_len(&rsp->hdr, (10 * 2 + rsp->ByteCount));

	if (buf)
		kvfree(buf);
	return rc;
}

/**
 * smb_set_acl() - handler for setting posix acl information
 * @smb_work:	smb work containing posix acl set command
 *
 * Return:	0 on success, otherwise error
 */
int smb_set_acl(struct smb_work *smb_work)
{
	TRANSACTION2_SPI_REQ *req = (TRANSACTION2_SPI_REQ *)smb_work->buf;
	TRANSACTION2_RSP *rsp = (TRANSACTION2_RSP *)smb_work->rsp_buf;
	struct cifs_posix_acl *wire_acl_data;
	char *fname, *buf = NULL;
	int rc = 0, acl_type = 0, value_len;

	fname = smb_get_name(req->FileName, PATH_MAX, smb_work, false);
	if (IS_ERR(fname))
		return PTR_ERR(fname);

	buf = vmalloc(XATTR_SIZE_MAX);
	if (!buf) {
		rsp->hdr.Status.CifsError = NT_STATUS_NO_MEMORY;
		rc = -ENOMEM;
		goto out;
	}

	wire_acl_data = (struct cifs_posix_acl *)(((char *) &req->hdr.Protocol)
			+ le16_to_cpu(req->DataOffset));
	if (le16_to_cpu(wire_acl_data->access_entry_count) > 0 &&
		le16_to_cpu(wire_acl_data->access_entry_count) < 0xFFFF) {
		acl_type = ACL_TYPE_ACCESS;

	} else if (le16_to_cpu(wire_acl_data->default_entry_count) > 0 &&
		le16_to_cpu(wire_acl_data->default_entry_count) < 0xFFFF) {
		acl_type = ACL_TYPE_DEFAULT;
	} else {
		rc = -EINVAL;
		goto out;
	}

	rc = cifs_copy_posix_acl(buf,
			(char *)wire_acl_data,
			XATTR_SIZE_MAX, acl_type, XATTR_SIZE_MAX);
	if (rc < 0) {
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_PARAMETER;
		goto out;
	}

	value_len = rc;
	if (acl_type == ACL_TYPE_ACCESS) {
		rc = smb_vfs_setxattr(fname, NULL, XATTR_NAME_POSIX_ACL_ACCESS,
				buf, value_len, 0);
	} else if (acl_type == ACL_TYPE_DEFAULT) {
		rc = smb_vfs_setxattr(fname, NULL, XATTR_NAME_POSIX_ACL_DEFAULT,
				buf, value_len, 0);
	}

	if (rc < 0) {
		rsp->hdr.Status.CifsError = NT_STATUS_UNEXPECTED_IO_ERROR;
		goto out;
	}

	rsp->hdr.Status.CifsError = NT_STATUS_OK;
	rsp->hdr.WordCount = 10;
	rsp->t2.TotalParameterCount = cpu_to_le16(2);
	rsp->t2.TotalDataCount = cpu_to_le16(0);
	rsp->t2.ParameterCount = rsp->t2.TotalParameterCount;
	rsp->t2.Reserved = 0;
	rsp->t2.ParameterCount = cpu_to_le16(2);
	rsp->t2.ParameterOffset = cpu_to_le16(56);
	rsp->t2.ParameterDisplacement = 0;
	rsp->t2.DataCount = rsp->t2.TotalDataCount;
	rsp->t2.DataOffset = cpu_to_le16(0);
	rsp->t2.DataDisplacement = 0;
	rsp->t2.SetupCount = 0;
	rsp->t2.Reserved1 = 0;

	/* 2 for paramater count + 1 pad1*/
	rsp->ByteCount = 3;
	rsp->Pad = 0;
	inc_rfc1001_len(&rsp->hdr,
			(rsp->hdr.WordCount * 2 + rsp->ByteCount));

out:
	if (buf)
		vfree(buf);
	smb_put_name(fname);
	return rc;
}

/**
 * smb_readlink() - handler for reading symlink source path
 * @smb_work:	smb work containing query link information
 *
 * Return:	0 on success, otherwise error
 */
int smb_readlink(struct smb_work *smb_work, struct path *path)
{
	TRANSACTION2_QPI_REQ *req = (TRANSACTION2_QPI_REQ *)smb_work->buf;
	TRANSACTION2_RSP *rsp = (TRANSACTION2_RSP *)smb_work->rsp_buf;
	int err, name_len;
	char *buf, *ptr;

	buf = kzalloc((CIFS_MF_SYMLINK_LINK_MAXLEN), GFP_KERNEL);
	if (!buf) {
		rsp->hdr.Status.CifsError = NT_STATUS_NO_MEMORY;
		return -ENOMEM;
	}

	err = smb_vfs_readlink(path, buf, CIFS_MF_SYMLINK_LINK_MAXLEN);
	if (err < 0) {
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_HANDLE;
		goto out;
	}

	/*
	 * check if this namelen(unicode) and smb header can fit in small rsp
	 * buf. If not, switch to large rsp buffer.
	 */
	err++;
	err *= 2;
	if (err + MAX_HEADER_SIZE(smb_work->conn) >
			MAX_CIFS_SMALL_BUFFER_SIZE) {
		if (switch_rsp_buf(smb_work) < 0) {
			rsp->hdr.Status.CifsError = NT_STATUS_NO_MEMORY;
			err = -ENOMEM;
			goto out;
		}
		rsp = (TRANSACTION2_RSP *)smb_work->rsp_buf;
	}
	err = 0;

	ptr = (char *)&rsp->Pad + 1;
	memset(ptr, 0, 4);
	ptr += 4;

	if (is_smbreq_unicode(&req->hdr)) {
		name_len = smb_strtoUTF16((__le16 *)ptr,
				buf, PATH_MAX, smb_work->conn->local_nls);
		name_len++;     /* trailing null */
		name_len *= 2;
	} else { /* BB add path length overrun check */
		name_len = strnlen(buf, PATH_MAX);
		name_len++;     /* trailing null */
		strncpy(ptr, buf, name_len);
	}

	rsp->hdr.WordCount = 10;
	rsp->t2.TotalParameterCount = 2;
	rsp->t2.TotalDataCount = cpu_to_le16(name_len);
	rsp->t2.Reserved = 0;
	rsp->t2.ParameterCount = 2;
	rsp->t2.ParameterOffset = 56;
	rsp->t2.ParameterDisplacement = 0;
	rsp->t2.DataCount = rsp->t2.TotalDataCount;
	rsp->t2.DataOffset = 60;
	rsp->t2.DataDisplacement = 0;
	rsp->t2.SetupCount = 0;
	rsp->t2.Reserved1 = 0;
	rsp->ByteCount = cpu_to_le16(name_len + 5);
	inc_rfc1001_len(&rsp->hdr, (10 * 2 + rsp->ByteCount));

out:
	kfree(buf);
	return err;
}

/**
 * smb_get_ea() - handler for extended attribute query
 * @smb_work:	smb work containing query xattr command
 * @path:	path of file/dir to query xattr command
 *
 * Return:	0 on success, otherwise error
 */
int smb_get_ea(struct smb_work *smb_work, struct path *path)
{
	struct connection *conn = smb_work->conn;
	TRANSACTION2_RSP *rsp = (TRANSACTION2_RSP *)smb_work->rsp_buf;
	char *name, *ptr, *xattr_list = NULL, *buf;
	int rc, name_len, value_len, xattr_list_len;
	struct fealist *eabuf = (struct fealist *)(smb_work->rsp_buf +
			sizeof(TRANSACTION2_RSP) + 4);
	struct fea *temp_fea;
	ssize_t buf_free_len;
	__u16 rsp_data_cnt = 4;

	eabuf->list_len = cpu_to_le32(rsp_data_cnt);
	buf_free_len = SMBMaxBufSize + MAX_HEADER_SIZE(conn) -
		(get_rfc1002_length(rsp) + 4) -
		sizeof(TRANSACTION2_RSP);
	rc = smb_vfs_listxattr(path->dentry, &xattr_list, XATTR_LIST_MAX);
	if (rc < 0) {
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_HANDLE;
		goto out;
	} else if (!rc) { /* there is no EA in the file */
		eabuf->list_len = cpu_to_le32(rsp_data_cnt);
		goto done;
	}

	xattr_list_len = rc;
	rc = 0;

	ptr = (char *)eabuf->list;
	temp_fea = (struct fea *)ptr;
	for (name = xattr_list; name - xattr_list < xattr_list_len;
			name += strlen(name) + 1) {
		cifsd_debug("%s, len %zd\n", name, strlen(name));
		/*
		 * CIFS does not support EA other name user.* namespace,
		 * still keep the framework generic, to list other attrs
		 * in future.
		 */
		if (strncmp(name, XATTR_USER_PREFIX, XATTR_USER_PREFIX_LEN))
			continue;

		name_len = strlen(name);
		if (!strncmp(name, XATTR_USER_PREFIX, XATTR_USER_PREFIX_LEN))
			name_len -= XATTR_USER_PREFIX_LEN;

		ptr = (char *)(&temp_fea->name + name_len + 1);
		buf_free_len -= (offsetof(struct fea, name) + name_len + 1);

		value_len = smb_vfs_getxattr(path->dentry, name, &buf, 1);
		if (value_len <= 0) {
			rc = -ENOENT;
			rsp->hdr.Status.CifsError = NT_STATUS_INVALID_HANDLE;
			goto out;
		}

		memcpy(ptr, buf, value_len);
		kvfree(buf);

		temp_fea->EA_flags = 0;
		temp_fea->name_len = name_len;
		if (!strncmp(name, XATTR_USER_PREFIX, XATTR_USER_PREFIX_LEN))
			strncpy(temp_fea->name, &name[XATTR_USER_PREFIX_LEN],
					name_len);
		else
			strncpy(temp_fea->name, name, name_len);

		temp_fea->value_len = cpu_to_le16(value_len);
		buf_free_len -= value_len;
		rsp_data_cnt += offsetof(struct fea, name) + name_len + 1 +
			value_len;
		eabuf->list_len += cpu_to_le32(offsetof(struct fea, name) +
				name_len + 1 + value_len);
		ptr += value_len;
		temp_fea = (struct fea *)ptr;
	}

done:
	rsp->hdr.WordCount = 10;
	rsp->t2.TotalParameterCount = 2;
	rsp->t2.TotalDataCount = cpu_to_le16(rsp_data_cnt);
	rsp->t2.Reserved = 0;
	rsp->t2.ParameterCount = 2;
	rsp->t2.ParameterOffset = 56;
	rsp->t2.ParameterDisplacement = 0;
	rsp->t2.DataCount = rsp->t2.TotalDataCount;
	rsp->t2.DataOffset = 60;
	rsp->t2.DataDisplacement = 0;
	rsp->t2.SetupCount = 0;
	rsp->t2.Reserved1 = 0;
	rsp->ByteCount = cpu_to_le16(rsp_data_cnt + 5);
	inc_rfc1001_len(&rsp->hdr, (10 * 2 + rsp->ByteCount));
out:
	if (xattr_list)
		vfree(xattr_list);
	return rc;
}

/**
 * query_path_info() - handler for query path info
 * @smb_work:	smb work containing query path info command
 *
 * Return:	0 on success, otherwise error
 */
int query_path_info(struct smb_work *smb_work)
{
	struct smb_hdr *req_hdr = (struct smb_hdr *)smb_work->buf;
	struct smb_hdr *rsp_hdr = (struct smb_hdr *)smb_work->rsp_buf;
	struct smb_trans2_req *req = (struct smb_trans2_req *)smb_work->buf;
	struct connection *conn = smb_work->conn;
	TRANSACTION2_RSP *rsp = (TRANSACTION2_RSP *)smb_work->rsp_buf;
	TRANSACTION2_QPI_REQ_PARAMS *req_params;
	char *name;
	struct path path;
	struct kstat st;
	int rc;
	FILE_ALL_INFO *ainfo;
	FILE_UNIX_BASIC_INFO *unix_info;
	FILE_BASIC_INFO *basic_info;
	FILE_STANDARD_INFO *standard_info;
	FILE_INFO_STANDARD *infos;
	FILE_EA_INFO *ea_info;
	ALT_NAME_INFO *alt_name_info;
	struct file_internal_info *iinfo;
	char *ptr;

	if (smb_work->tcon->share->is_pipe == true) {
		rsp_hdr->Status.CifsError = NT_STATUS_UNEXPECTED_IO_ERROR;
		return 0;
	}

	req_params = (TRANSACTION2_QPI_REQ_PARAMS *)(smb_work->buf +
		     req->ParameterOffset + 4);
	name = smb_get_name(req_params->FileName, PATH_MAX, smb_work, false);
	if (IS_ERR(name))
		return PTR_ERR(name);

	rc = smb_kern_path(name, 0, &path, 0);
	if (rc) {
		rsp_hdr->Status.CifsError = NT_STATUS_OBJECT_NAME_NOT_FOUND;
		cifsd_debug("cannot get linux path for %s, err %d\n",
				name, rc);
		goto out;
	}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
	rc = vfs_getattr(&path, &st, STATX_BASIC_STATS, AT_STATX_SYNC_AS_STAT);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
	rc = vfs_getattr(&path, &st);
#else
	rc = vfs_getattr(path.mnt, path.dentry, &st);
#endif
	if (rc) {
		cifsd_err("cannot get stat information\n");
		goto err_out;
	}

	if (req_hdr->WordCount != 15) {
		cifsd_err("word count mismatch: expected 15 got %d\n",
				req_hdr->WordCount);
		rc = -EINVAL;
		goto err_out;
	}

	switch (req_params->InformationLevel) {
	case SMB_INFO_STANDARD:
		cifsd_debug("SMB_INFO_STANDARD\n");
		ptr = (char *)&rsp->Pad + 1;
		memset(ptr, 0, 4);
		infos = (FILE_INFO_STANDARD *)(ptr + 4);
		unix_to_dos_time(&st.ctime, &infos->CreationDate,
						&infos->CreationTime);
		unix_to_dos_time(&st.atime, &infos->LastAccessDate,
						&infos->LastAccessTime);
		unix_to_dos_time(&st.mtime, &infos->LastWriteDate,
						&infos->LastWriteTime);
		infos->DataSize = cpu_to_le32(st.size);
		infos->AllocationSize = cpu_to_le32(st.blocks << 9);
		infos->Attributes = S_ISDIR(st.mode) ?
					ATTR_DIRECTORY : ATTR_NORMAL;
		infos->EASize = 0;

		rsp_hdr->WordCount = 10;
		rsp->t2.TotalParameterCount = 2;
		rsp->t2.TotalDataCount = 22;
		rsp->t2.Reserved = 0;
		rsp->t2.ParameterCount = 2;
		rsp->t2.ParameterOffset = 56;
		rsp->t2.ParameterDisplacement = 0;
		rsp->t2.DataCount = 22;
		rsp->t2.DataOffset = 60;
		rsp->t2.DataDisplacement = 0;
		rsp->t2.SetupCount = 0;
		rsp->t2.Reserved1 = 0;
		rsp->ByteCount = 27;
		rsp->Pad = 0;
		inc_rfc1001_len(rsp_hdr, (10 * 2 + rsp->ByteCount));
		break;
	case SMB_QUERY_FILE_STANDARD_INFO:
		cifsd_debug("SMB_QUERY_FILE_STANDARD_INFO\n");
		rsp_hdr->WordCount = 10;
		rsp->t2.TotalParameterCount = 2;
		rsp->t2.TotalDataCount = sizeof(FILE_STANDARD_INFO);
		rsp->t2.Reserved = 0;
		rsp->t2.ParameterCount = 2;
		rsp->t2.ParameterOffset = 56;
		rsp->t2.ParameterDisplacement = 0;
		rsp->t2.DataCount = sizeof(FILE_STANDARD_INFO);
		rsp->t2.DataOffset = 60;
		rsp->t2.DataDisplacement = 0;
		rsp->t2.SetupCount = 0;
		rsp->t2.Reserved1 = 0;
		/*2 for parameter count & 3 pad (1pad1 + 2 pad2)*/
		rsp->ByteCount = 2 + sizeof(FILE_STANDARD_INFO) + 3;
		rsp->Pad = 0;
		/* lets set EA info */
		ptr = (char *)&rsp->Pad + 1;
		memset(ptr, 0, 4);
		standard_info = (FILE_STANDARD_INFO *)(ptr + 4);
		standard_info->AllocationSize = cpu_to_le64(st.blocks << 9);
		standard_info->EndOfFile = cpu_to_le64(st.size);
		standard_info->NumberOfLinks = cpu_to_le32(st.nlink);
		standard_info->DeletePending = 0;
		standard_info->Directory = S_ISDIR(st.mode) ? 1 : 0;
		inc_rfc1001_len(rsp_hdr, (10 * 2 + rsp->ByteCount));
		break;

	case SMB_QUERY_FILE_BASIC_INFO:
		cifsd_debug("SMB_QUERY_FILE_BASIC_INFO\n");
		rsp_hdr->WordCount = 10;
		rsp->t2.TotalParameterCount = 2;
		rsp->t2.TotalDataCount = sizeof(FILE_BASIC_INFO);
		rsp->t2.Reserved = 0;
		rsp->t2.ParameterCount = 2;
		rsp->t2.ParameterOffset = 56;
		rsp->t2.ParameterDisplacement = 0;
		rsp->t2.DataCount = sizeof(FILE_BASIC_INFO);
		rsp->t2.DataOffset = 60;
		rsp->t2.DataDisplacement = 0;
		rsp->t2.SetupCount = 0;
		rsp->t2.Reserved1 = 0;
		/*2 for parameter count & 3 pad (1pad1 + 2 pad2)*/
		rsp->ByteCount = 2 + sizeof(FILE_BASIC_INFO) + 3;
		rsp->Pad = 0;
		/* lets set EA info */
		ptr = (char *)&rsp->Pad + 1;
		memset(ptr, 0, 4);
		basic_info = (FILE_BASIC_INFO *)(ptr + 4);
		basic_info->CreationTime = cpu_to_le64(min3(
					cifs_UnixTimeToNT(st.ctime),
					cifs_UnixTimeToNT(st.mtime),
					cifs_UnixTimeToNT(st.atime)));

		if (!le64_to_cpu(basic_info->CreationTime))
			basic_info->CreationTime = cpu_to_le64(min(
						cifs_UnixTimeToNT(st.ctime),
						cifs_UnixTimeToNT(st.mtime)));
		basic_info->LastAccessTime =
			cpu_to_le64(cifs_UnixTimeToNT(st.atime));
		basic_info->LastWriteTime =
			cpu_to_le64(cifs_UnixTimeToNT(st.mtime));
		basic_info->ChangeTime =
			cpu_to_le64(cifs_UnixTimeToNT(st.mtime));
		basic_info->Attributes = S_ISDIR(st.mode) ?
					 ATTR_DIRECTORY : ATTR_NORMAL;
		basic_info->Pad = 0;
		inc_rfc1001_len(rsp_hdr, (10 * 2 + rsp->ByteCount));
		break;

	case SMB_QUERY_FILE_EA_INFO:
		cifsd_debug("SMB_QUERY_FILE_EA_INFO\n");
		rsp_hdr->WordCount = 10;
		rsp->t2.TotalParameterCount = 2;
		rsp->t2.TotalDataCount = sizeof(FILE_EA_INFO);
		rsp->t2.Reserved = 0;
		rsp->t2.ParameterCount = 2;
		rsp->t2.ParameterOffset = 56;
		rsp->t2.ParameterDisplacement = 0;
		rsp->t2.DataCount = sizeof(FILE_EA_INFO);
		rsp->t2.DataOffset = 60;
		rsp->t2.DataDisplacement = 0;
		rsp->t2.SetupCount = 0;
		rsp->t2.Reserved1 = 0;
		/*2 for paramater count & 3 pad (1pad1 + 2 pad2)*/
		rsp->ByteCount = 2 + sizeof(FILE_EA_INFO) + 3;
		rsp->Pad = 0;
		/* lets set EA info */
		ptr = (char *)&rsp->Pad + 1;
		memset(ptr, 0, 4);
		ea_info = (FILE_EA_INFO *)(ptr + 4);
		ea_info->EaSize = 0;
		inc_rfc1001_len(rsp_hdr, (10 * 2 + rsp->ByteCount));
		break;

	case SMB_QUERY_FILE_ALL_INFO:
		cifsd_debug("SMB_QUERY_FILE_ALL_INFO\n");
		rsp_hdr->WordCount = 10;
		rsp->t2.TotalParameterCount = 2;
		/* add unicode name length of name */
		rsp->t2.TotalDataCount = 72 + 0;
		rsp->t2.Reserved = 0;
		rsp->t2.ParameterCount = 2;
		rsp->t2.ParameterOffset = 56;
		rsp->t2.ParameterDisplacement = 0;
		rsp->t2.DataCount = 72;
		rsp->t2.DataOffset = 60;
		rsp->t2.DataDisplacement = 0;
		rsp->t2.SetupCount = 0;
		rsp->t2.Reserved1 = 0;
		/* 2 for paramater count + 72 data count +
		   3 pad (1pad1 + 2 pad2) */
		rsp->ByteCount = 77;
		rsp->Pad = 0;
		/*
		 * Observation: sizeof smb_hdr is 33 bytes(including word count)
		 * After that: trans2 response 22 bytes when stepcount 0 and
		 * including ByteCount storage.
		 */
		/* lets set EA info */
		ptr = (char *)&rsp->Pad + 1;
		memset(ptr, 0, 4);
		ainfo = (FILE_ALL_INFO *) (ptr + 4);
		ainfo->CreationTime = cpu_to_le64(cifs_UnixTimeToNT(st.ctime));
		ainfo->LastAccessTime =
			cpu_to_le64(cifs_UnixTimeToNT(st.atime));
		ainfo->LastWriteTime = cpu_to_le64(cifs_UnixTimeToNT(st.mtime));
		ainfo->ChangeTime = cpu_to_le64(cifs_UnixTimeToNT(st.mtime));
		ainfo->Attributes = S_ISDIR(st.mode) ?
					ATTR_DIRECTORY : ATTR_NORMAL;
		ainfo->Pad1 = 0;
		ainfo->AllocationSize = cpu_to_le64(st.blocks << 9);
		ainfo->EndOfFile = cpu_to_le64(st.size);
		ainfo->NumberOfLinks = cpu_to_le32(st.nlink);
		ainfo->DeletePending = 0;
		ainfo->Directory = S_ISDIR(st.mode) ? 1 : 0;
		ainfo->Pad2 = 0;
		ainfo->EASize = 0;
		ainfo->FileNameLength = 0;
		inc_rfc1001_len(rsp_hdr, (10 * 2 + rsp->ByteCount));
		break;
	case SMB_QUERY_ALT_NAME_INFO:
		cifsd_debug("SMB_QUERY_ALT_NAME_INFO\n");
		rsp_hdr->WordCount = 10;
		rsp->t2.TotalParameterCount = 2;
		rsp->t2.TotalDataCount = 20;
		rsp->t2.Reserved = 0;
		rsp->t2.ParameterCount = 2;
		rsp->t2.ParameterOffset = 56;
		rsp->t2.ParameterDisplacement = 0;
		rsp->t2.DataCount = 20;
		rsp->t2.DataOffset = 60;
		rsp->t2.DataDisplacement = 0;
		rsp->t2.SetupCount = 0;
		rsp->t2.Reserved1 = 0;
		/*2 for parameter count & 3 pad (1pad1 + 2 pad2)*/
		rsp->ByteCount = 25;
		rsp->Pad = 0;
		/* lets set EA info */
		ptr = (char *)&rsp->Pad + 1;
		memset(ptr, 0, 4);
		alt_name_info = (ALT_NAME_INFO *)(ptr + 4);
		alt_name_info->FileNameLength = smb_get_shortname(conn,
				name, alt_name_info->FileName);
		inc_rfc1001_len(rsp_hdr, (10 * 2 + rsp->ByteCount));
		break;
	case SMB_QUERY_FILE_UNIX_BASIC:
		cifsd_debug("SMB_QUERY_FILE_UNIX_BASIC\n");
		rsp_hdr->WordCount = 10;
		rsp->t2.TotalParameterCount = 0;
		rsp->t2.TotalDataCount = 100;
		rsp->t2.Reserved = 0;
		rsp->t2.ParameterCount = 0;
		rsp->t2.ParameterOffset = 56;
		rsp->t2.ParameterDisplacement = 0;
		rsp->t2.DataCount = 100;
		rsp->t2.DataOffset = 56;
		rsp->t2.DataDisplacement = 0;
		rsp->t2.SetupCount = 0;
		rsp->t2.Reserved1 = 0;
		rsp->ByteCount = 101; /* 100 data count + 1pad */
		rsp->Pad = 0;
		unix_info = (FILE_UNIX_BASIC_INFO *)(&rsp->Pad + 1);
		init_unix_info(unix_info, &st);
		inc_rfc1001_len(rsp_hdr, (10 * 2 + rsp->ByteCount));
		break;
	case SMB_QUERY_FILE_INTERNAL_INFO:
		cifsd_debug("SMB_QUERY_FILE_INTERNAL_INFO\n");
		rsp_hdr->WordCount = 10;
		rsp->t2.TotalParameterCount = 2;
		rsp->t2.TotalDataCount = 8;
		rsp->t2.Reserved = 0;
		rsp->t2.ParameterCount = 2;
		rsp->t2.ParameterOffset = 56;
		rsp->t2.ParameterDisplacement = 0;
		rsp->t2.DataCount = 8;
		rsp->t2.DataOffset = 60;
		rsp->t2.DataDisplacement = 0;
		rsp->t2.SetupCount = 0;
		rsp->t2.Reserved1 = 0;
		rsp->ByteCount = 13;
		rsp->Pad = 0;
		ptr = (char *)&rsp->Pad + 1;
		memset(ptr, 0, 4);
		iinfo = (struct file_internal_info *) (ptr + 4);
		iinfo->UniqueId = cpu_to_le64(st.ino);
		inc_rfc1001_len(rsp_hdr, (10 * 2 + rsp->ByteCount));
		break;
	case SMB_QUERY_FILE_UNIX_LINK:
		cifsd_debug("SMB_QUERY_FILE_UNIX_LINK\n");
		rc = smb_readlink(smb_work, &path);
		if (rc < 0)
			goto err_out;
		break;
	case SMB_INFO_QUERY_ALL_EAS:
		cifsd_debug("SMB_INFO_QUERY_ALL_EAS\n");
		rc = smb_get_ea(smb_work, &path);
		if (rc < 0)
			goto err_out;
		break;
	case SMB_QUERY_POSIX_ACL:
		cifsd_debug("SMB_QUERY_POSIX_ACL\n");
		rc = smb_get_acl(smb_work, &path);
		if (rc < 0)
			goto err_out;
		break;
	default:
		cifsd_err("query path info not implemnted for %x\n",
				req_params->InformationLevel);
		rc = -EINVAL;
		goto err_out;
	}

err_out:
	path_put(&path);
out:
	smb_put_name(name);
	return rc;
}

/**
 * smb_trans2() - handler for trans2 commands
 * @smb_work:	smb work containing trans2 command buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb_trans2(struct smb_work *smb_work)
{
	struct smb_trans2_req *req = (struct smb_trans2_req *)smb_work->buf;
	struct smb_hdr *rsp_hdr = (struct smb_hdr *)smb_work->rsp_buf;
	int err = 0;
	u16 sub_command = req->SubCommand;

	/* at least one setup word for TRANS2 command
			MS-CIFS, SMB COM TRANSACTION */
	if (req->SetupCount < 1) {
		cifsd_err("Wrong setup count in SMB_TRANS2"
				" - indicates wrong request\n");
		rsp_hdr->Status.CifsError = NT_STATUS_UNSUCCESSFUL;
		return -EINVAL;
	}

	switch (sub_command) {
	case TRANS2_FIND_FIRST:
		err = find_first(smb_work);
		break;
	case TRANS2_FIND_NEXT:
		err = find_next(smb_work);
		break;
	case TRANS2_QUERY_FS_INFORMATION:
		err = query_fs_info(smb_work);
		break;

	case TRANS2_QUERY_PATH_INFORMATION:
		err = query_path_info(smb_work);
		break;
	case TRANS2_SET_PATH_INFORMATION:
		err = set_path_info(smb_work);
		break;
	case TRANS2_SET_FS_INFORMATION:
		err = set_fs_info(smb_work);
		break;

	case TRANS2_QUERY_FILE_INFORMATION:
		err = query_file_info(smb_work);
		break;

	case TRANS2_SET_FILE_INFORMATION:
		err = set_file_info(smb_work);
		break;

	case TRANS2_CREATE_DIRECTORY:
		err = create_dir(smb_work);
		break;

	case TRANS2_GET_DFS_REFERRAL:
		err = get_dfs_referral(smb_work);
		break;

	default:
		cifsd_err("sub command 0x%x not implemented yet\n",
				sub_command);
		rsp_hdr->Status.CifsError = NT_STATUS_NOT_SUPPORTED;
		return -EINVAL;
	}

	if (err) {
		cifsd_debug("smb_trans2 failed with error %d\n", err);
		return err;
	}

	return 0;
}

/**
 * set_fs_info() - handler for set fs info commands
 * @smb_work:	smb work containing set fs info command buffer
 *
 * Return:	0 on success, otherwise error
 */
int set_fs_info(struct smb_work *smb_work)
{
	TRANSACTION2_SETFSI_REQ *req = (TRANSACTION2_SETFSI_REQ *)smb_work->buf;
	TRANSACTION2_SETFSI_RSP	*rsp =
				(TRANSACTION2_SETFSI_RSP *)smb_work->rsp_buf;
	int info_level = req->InformationLevel;

	switch (info_level) {
	int client_cap;
	case SMB_SET_CIFS_UNIX_INFO:
		cifsd_debug("SMB_SET_CIFS_UNIX_INFO\n");
		if (req->ClientUnixMajor != CIFS_UNIX_MAJOR_VERSION) {
			cifsd_err("Non compatible unix major info\n");
			return -EINVAL;
		}

		if (req->ClientUnixMinor != CIFS_UNIX_MINOR_VERSION) {
			cifsd_err("Non compatible unix minor info\n");
			return -EINVAL;
		}

		client_cap = req->ClientUnixCap;
		cifsd_debug("clients unix cap = %x\n", client_cap);
		/* TODO: process caps */
		rsp->t2.TotalDataCount = 0;
		break;
	default:
		cifsd_err("info level %x  not supported\n", info_level);
		return -EINVAL;
	}

	create_trans2_reply(smb_work, rsp->t2.TotalDataCount);
	return 0;
}

/**
 * query_fs_info() - handler for query fs info commands
 * @smb_work:	smb work containing query fs info command buffer
 *
 * Return:	0 on success, otherwise error
 */
int query_fs_info(struct smb_work *smb_work)
{
	struct smb_hdr *req_hdr = (struct smb_hdr *)smb_work->buf;
	struct smb_trans2_req *req = (struct smb_trans2_req *)smb_work->buf;
	TRANSACTION2_RSP *rsp = (TRANSACTION2_RSP *)smb_work->rsp_buf;
	TRANSACTION2_QFSI_REQ_PARAMS *req_params;
	struct connection *conn = smb_work->conn;
	struct kstatfs stfs;
	struct cifsd_share *share;
	int rc;
	struct path path;
	bool incomplete = false;
	int info_level, len = 0;

	req_params = (TRANSACTION2_QFSI_REQ_PARAMS *)(smb_work->buf +
				req->ParameterOffset + 4);
	/* check if more data is coming */
	if (req->TotalParameterCount != req->ParameterCount) {
		cifsd_debug("total param = %d, received = %d\n",
				req->TotalParameterCount, req->ParameterCount);
		incomplete = true;
	}

	if (req->TotalDataCount != req->DataCount) {
		cifsd_debug("total data = %d, received = %d\n",
				req->TotalDataCount, req->DataCount);
		incomplete = true;
	}

	if (incomplete) {
		/* create 1 trans_state structure
		   and add to connection list */
	}

	info_level = req_params->InformationLevel;

	/* query_fs_info wct must be 15 */
	if (req_hdr->WordCount != 0x0F) {
		cifsd_err("query_fs_info request wct error, received wct = %x\n",
				req_hdr->WordCount);
		return -EINVAL;
	}

	share = find_matching_share(req_hdr->Tid);
	if (!share)
		return -ENOENT;

	rc = smb_kern_path(share->path, LOOKUP_FOLLOW, &path, 0);
	if (rc) {
		cifsd_err("cannot create vfs path\n");
		return rc;
	}

	rc = vfs_statfs(&path, &stfs);
	if (rc) {
		cifsd_err("cannot do stat of path %s\n", share->path);
		goto err_out;
	}

	switch (info_level) {
	FILE_SYSTEM_DEVICE_INFO *fdi;
	FILE_SYSTEM_ATTRIBUTE_INFO *info;
	FILE_SYSTEM_UNIX_INFO *uinfo;
	FILE_SYSTEM_ALLOC_INFO *ainfo;
	FILE_SYSTEM_VOL_INFO *vinfo;
	FILE_SYSTEM_INFO *sinfo;
	FILE_SYSTEM_POSIX_INFO *pinfo;
	case SMB_INFO_ALLOCATION:
		cifsd_debug("GOT SMB_INFO_ALLOCATION\n");
		rsp->t2.TotalDataCount = cpu_to_le16(18);
		ainfo = (FILE_SYSTEM_ALLOC_INFO *)(&rsp->Pad + 1);
		ainfo->fsid = 0;
		ainfo->BytesPerSector = cpu_to_le16(512);
		ainfo->SectorsPerAllocationUnit =
		cpu_to_le32(stfs.f_bsize/le16_to_cpu(ainfo->BytesPerSector));
		ainfo->TotalAllocationUnits = cpu_to_le32(stfs.f_blocks);
		ainfo->FreeAllocationUnits = cpu_to_le32(stfs.f_bfree);
		break;
	case SMB_QUERY_FS_VOLUME_INFO:
		cifsd_debug("GOT SMB_QUERY_FS_VOLUME_INFO\n");
		vinfo = (FILE_SYSTEM_VOL_INFO *)(&rsp->Pad + 1);
		vinfo->VolumeCreationTime = 0;
		/* Taking dummy value of serial number*/
		vinfo->SerialNumber = cpu_to_le32(0xbc3ac512);
		len = smbConvertToUTF16((__le16 *)vinfo->VolumeLabel,
			share->sharename, PATH_MAX, conn->local_nls, 0);
		vinfo->VolumeLabelSize = cpu_to_le32(len);
		vinfo->Reserved = 0;
		rsp->t2.TotalDataCount =
			cpu_to_le16(sizeof(FILE_SYSTEM_VOL_INFO) + len - 2);
		break;
	case SMB_QUERY_FS_SIZE_INFO:
		cifsd_debug("GOT SMB_QUERY_FS_SIZE_INFO\n");
		rsp->t2.TotalDataCount = cpu_to_le16(24);
		sinfo = (FILE_SYSTEM_INFO *)(&rsp->Pad + 1);
		sinfo->BytesPerSector = cpu_to_le32(512);
		sinfo->SectorsPerAllocationUnit =
		cpu_to_le32(stfs.f_bsize/le16_to_cpu(sinfo->BytesPerSector));
		sinfo->TotalAllocationUnits = cpu_to_le64(stfs.f_blocks);
		sinfo->FreeAllocationUnits = cpu_to_le64(stfs.f_bfree);
		break;
	case SMB_QUERY_FS_DEVICE_INFO:
		/* query fs info device info response is 0 word and 8 bytes */
		cifsd_debug("GOT SMB_QUERY_FS_DEVICE_INFO\n");
		if (req->MaxDataCount < 8) {
			cifsd_err("canno send query_fs_info repsonse as "
					"client send unsufficient bytes\n");
			rc = -EINVAL;
			goto err_out;
		}

		rsp->t2.TotalDataCount = 18;
		fdi = (FILE_SYSTEM_DEVICE_INFO *)(&rsp->Pad + 1);
		fdi->DeviceType = FILE_DEVICE_DISK;
		fdi->DeviceCharacteristics = 0x20;
		break;
	case SMB_QUERY_FS_ATTRIBUTE_INFO:
		cifsd_debug("GOT SMB_QUERY_FS_ATTRIBUTE_INFO\n");
		/* constant 12 bytes + variable filesystem name */
		info = (FILE_SYSTEM_ATTRIBUTE_INFO *)(&rsp->Pad + 1);

		if (req->MaxDataCount < 12) {
			cifsd_err("cannot send SMB_QUERY_FS_ATTRIBUTE_INFO  "
					" repsonse as client send unsufficient"
					" bytes\n");
			rc = -EINVAL;
			goto err_out;
		}

		info->Attributes = FILE_CASE_PRESERVED_NAMES |
				   FILE_CASE_SENSITIVE_SEARCH |
				   FILE_VOLUME_QUOTAS;
		info->MaxPathNameComponentLength = stfs.f_namelen;
		info->FileSystemNameLen = 0;
		rsp->t2.TotalDataCount = 12;
		break;
	case SMB_QUERY_CIFS_UNIX_INFO:
		cifsd_debug("GOT SMB_QUERY_CIFS_UNIX_INFO\n");
		/* constant 12 bytes + variable filesystem name */
		uinfo = (FILE_SYSTEM_UNIX_INFO *)(&rsp->Pad + 1);

		if (req->MaxDataCount < 12) {
			cifsd_err("cannot send SMB_QUERY_CIFS_UNIX_INFO"
					" repsonse as client send unsufficient"
					" bytes\n");
			rc = -EINVAL;
			goto err_out;
		}
		uinfo->MajorVersionNumber = CIFS_UNIX_MAJOR_VERSION;
		uinfo->MinorVersionNumber = CIFS_UNIX_MINOR_VERSION;
		uinfo->Capability = SMB_UNIX_CAPS;
		rsp->t2.TotalDataCount = 12;
		break;
	case SMB_QUERY_POSIX_FS_INFO:
		cifsd_debug("GOT SMB_QUERY_POSIX_FS_INFO\n");
		rsp->t2.TotalDataCount = cpu_to_le16(56);
		pinfo = (FILE_SYSTEM_POSIX_INFO *)(&rsp->Pad + 1);
		pinfo->BlockSize = cpu_to_le32(stfs.f_bsize);
		pinfo->OptimalTransferSize = cpu_to_le32(stfs.f_blocks);
		pinfo->TotalBlocks = cpu_to_le64(stfs.f_blocks);
		pinfo->BlocksAvail = cpu_to_le64(stfs.f_bfree);
		pinfo->UserBlocksAvail = cpu_to_le64(stfs.f_bavail);
		pinfo->TotalFileNodes = cpu_to_le64(stfs.f_files);
		pinfo->FreeFileNodes = cpu_to_le64(stfs.f_ffree);
		pinfo->FileSysIdentifier = 0;
		break;
	default:
		cifsd_err("info level %x not implemented\n", info_level);
		rc = -EINVAL;
		goto err_out;
	}

	create_trans2_reply(smb_work, rsp->t2.TotalDataCount);

err_out:
	path_put(&path);
	return rc;
}

/**
 * smb_get_name() - convert filename on smb packet to char string
 * @src:	source filename, mostly in unicode format
 * @maxlen:	maxlen of src string to be used for parsing
 * @smb_work:	smb work containing smb header flag
 * @converted:	src string already converted to local characterset
 *
 * Return:	pointer to filename string on success, otherwise error ptr
 */
char *
smb_get_name(const char *src, const int maxlen, struct smb_work *smb_work,
		bool converted)
{
	struct smb_hdr *req_hdr = (struct smb_hdr *)smb_work->buf;
	struct smb_hdr *rsp_hdr = (struct smb_hdr *)smb_work->rsp_buf;
	bool is_unicode = is_smbreq_unicode(req_hdr);
	char *name, *unixname;
	char *wild_card_pos;

	if (converted)
		name = (char *)src;
	else {
		name = smb_strndup_from_utf16(src, maxlen, is_unicode,
				smb_work->conn->local_nls);
		if (IS_ERR(name)) {
			cifsd_debug("failed to get name %ld\n",
				PTR_ERR(name));
			if (PTR_ERR(name) == -ENOMEM)
				rsp_hdr->Status.CifsError = NT_STATUS_NO_MEMORY;
			else
				rsp_hdr->Status.CifsError =
					NT_STATUS_OBJECT_NAME_INVALID;
			return name;
		}
	}

	/* change it to absolute unix name */
	convert_delimiter(name, 0);
	/*Handling of dir path in FIND_FIRST2 having '*' at end of path*/
	wild_card_pos = strrchr(name, '*');

	if (wild_card_pos != NULL)
		*wild_card_pos = '\0';

	unixname = convert_to_unix_name(name, req_hdr->Tid);

	if (!converted)
		kfree(name);
	if (!unixname) {
		cifsd_err("can not convert absolute name\n");
		rsp_hdr->Status.CifsError = NT_STATUS_NO_MEMORY;
		return ERR_PTR(-ENOMEM);
	}

	cifsd_debug("absoulte name = %s\n", unixname);
	return unixname;
}

/**
 * smb_get_dir_name() - convert directory name on smb packet to char string
 * @src:	source dir name, mostly in unicode format
 * @maxlen:	maxlen of src string to be used for parsing
 * @smb_work:	smb work containing smb header flag
 * @srch_ptr:	update search pointer in dir for searching dir entries
 *
 * Return:	pointer to dir name string on success, otherwise error ptr
 */
static char *smb_get_dir_name(const char *src, const int maxlen,
		struct smb_work *smb_work, char **srch_ptr)
{
	struct smb_hdr *req_hdr = (struct smb_hdr *)smb_work->buf;
	struct smb_hdr *rsp_hdr = (struct smb_hdr *)smb_work->rsp_buf;
	bool is_unicode = is_smbreq_unicode(req_hdr);
	char *name, *unixname;
	char *wild_card_pos, *pattern_pos, *pattern = NULL;
	int pattern_len;

	name = smb_strndup_from_utf16(src, maxlen, is_unicode,
			smb_work->conn->local_nls);
	if (IS_ERR(name)) {
		cifsd_err("failed to allocate memory\n");
		rsp_hdr->Status.CifsError = NT_STATUS_NO_MEMORY;
		return name;
	}

	/* change it to absolute unix name */
	convert_delimiter(name, 0);

	/*Handling of dir path in FIND_FIRST2 having '*' at end of path*/
	wild_card_pos = strrchr(name, '*');

	if (wild_card_pos != NULL)
		*wild_card_pos = '\0';
	else {
		pattern_pos = strrchr(name, '/');

		if (pattern_pos == NULL)
			pattern_pos = name;
		else
			pattern_pos += 1;

		pattern_len = strlen(pattern_pos);
		if (pattern_len == 0) {
			rsp_hdr->Status.CifsError = NT_STATUS_INVALID_PARAMETER;
			kfree(name);
			return ERR_PTR(-EINVAL);
		}
		cifsd_debug("pattern searched = %s pattern_len = %d\n",
				pattern_pos, pattern_len);
		pattern = kmalloc(pattern_len + 1, GFP_KERNEL);
		if (!pattern) {
			rsp_hdr->Status.CifsError = NT_STATUS_NO_MEMORY;
			kfree(name);
			return ERR_PTR(-ENOMEM);
		}
		memcpy(pattern, pattern_pos, pattern_len);
		*(pattern + pattern_len) = '\0';
		*pattern_pos = '\0';
		*srch_ptr = pattern;
	}

	unixname = convert_to_unix_name(name, req_hdr->Tid);
	kfree(name);
	if (!unixname) {
		kfree(pattern);
		cifsd_err("can not convert absolute name\n");
		rsp_hdr->Status.CifsError = NT_STATUS_INVALID_PARAMETER;
		return ERR_PTR(-EINVAL);
	}

	cifsd_debug("absoulte name = %s\n", unixname);
	return unixname;
}

/**
 * smb_put_name() - free memory allocated for filename
 * @name:	filename pointer to be freed
 */
void smb_put_name(void *name)
{
	if (!IS_ERR(name))
		kfree(name);
}

/**
 * smb_posix_convert_flags() - convert smb posix access flags to open flags
 * @flags:	smb posix access flags
 *
 * Return:	file open flags
 */
static __u32 smb_posix_convert_flags(__u32 flags)
{
	__u32 posix_flags = 0;

	if ((flags & SMB_ACCMODE) == SMB_O_RDONLY)
		posix_flags = O_RDONLY;
	else if ((flags & SMB_ACCMODE) == SMB_O_WRONLY)
		posix_flags = O_WRONLY;
	else if ((flags & SMB_ACCMODE) == SMB_O_RDWR)
		posix_flags = O_RDWR;

	if (flags & SMB_O_SYNC)
		posix_flags |= O_DSYNC;
	if (flags & SMB_O_DIRECTORY)
		posix_flags |= O_DIRECTORY;
	if (flags & SMB_O_NOFOLLOW)
		posix_flags |= O_NOFOLLOW;
/*
	* TODO : need to add special handling for Direct I/O.
	if (flags & SMB_O_DIRECT)
		posix_flags |= O_DIRECT;
*/
	if (flags & SMB_O_APPEND)
		posix_flags |= O_APPEND;

	return posix_flags;
}

/**
 * smb_get_disposition() - convert smb disposition flags to open flags
 * @flags:		smb file disposition flags
 * @file_present:	file already present or not
 * @stat:		file stat information
 * @open_flags:		open flags should be stored here
 *
 * Return:		file disposition flags
 */
static int smb_get_disposition(unsigned int flags, bool file_present,
		struct kstat *stat, unsigned int *open_flags)
{
	int dispostion, disp_flags;

	if ((flags & (SMB_O_CREAT | SMB_O_EXCL)) == (SMB_O_CREAT | SMB_O_EXCL))
		dispostion = FILE_CREATE;
	else if ((flags & (SMB_O_CREAT | SMB_O_TRUNC)) ==
			(SMB_O_CREAT | SMB_O_TRUNC))
		dispostion = FILE_OVERWRITE_IF;
	else if ((flags & SMB_O_CREAT) == SMB_O_CREAT)
		dispostion = FILE_OPEN_IF;
	else if ((flags & SMB_O_TRUNC) == SMB_O_TRUNC)
		dispostion = FILE_OVERWRITE;
	else if ((flags & (SMB_O_CREAT | SMB_O_EXCL | SMB_O_TRUNC)) == 0)
		dispostion = FILE_OPEN;
	else
		dispostion = FILE_SUPERSEDE;

	disp_flags = file_create_dispostion_flags(dispostion, file_present);
	if (disp_flags < 0)
		return disp_flags;

	*open_flags |= disp_flags;
	return disp_flags;
}

/**
 * smb_posix_open() - handler for smb posix open
 * @smb_work:	smb work containing posix open command
 *
 * Return:	0 on success, otherwise error
 */
int smb_posix_open(struct smb_work *smb_work)
{
	TRANSACTION2_SPI_REQ *pSMB_req = (TRANSACTION2_SPI_REQ *)smb_work->buf;
	TRANSACTION2_SPI_RSP *pSMB_rsp =
		(TRANSACTION2_SPI_RSP *)smb_work->rsp_buf;
	struct connection *conn = smb_work->conn;
	OPEN_PSX_REQ *psx_req;
	OPEN_PSX_RSP *psx_rsp;
	FILE_UNIX_BASIC_INFO *unix_info;
	struct path path;
	struct kstat stat;
	__u16 data_offset, rsp_info_level, fid, file_info = 0;
	__u32 oplock_flags, posix_open_flags;
	umode_t mode;
	char *name;
	bool file_present = true;
	int err;

	name = smb_get_name(pSMB_req->FileName, PATH_MAX, smb_work, false);
	if (IS_ERR(name))
		return PTR_ERR(name);

	err = smb_kern_path(name, 0, &path, 0);
	if (err) {
		file_present = false;
		cifsd_debug("cannot get linux path for %s, err = %d\n",
				name, err);
	} else {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
		err = vfs_getattr(&path, &stat, STATX_BASIC_STATS,
			AT_STATX_SYNC_AS_STAT);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
		err = vfs_getattr(&path, &stat);
#else
		err = vfs_getattr(path.mnt, path.dentry, &stat);
#endif
		if (err) {
			cifsd_err("can not stat %s, err = %d\n", name, err);
			goto free_path;
		}
	}

	data_offset = le16_to_cpu(pSMB_req->DataOffset);
	psx_req = (OPEN_PSX_REQ *)(((char *)&pSMB_req->hdr.Protocol) +
			data_offset);
	oplock_flags = le32_to_cpu(psx_req->OpenFlags);

	posix_open_flags = smb_posix_convert_flags(
			le32_to_cpu(psx_req->PosixOpenFlags));
	err = smb_get_disposition(le32_to_cpu(psx_req->PosixOpenFlags),
			file_present, &stat,
			&posix_open_flags);
	if (err < 0) {
		cifsd_debug("create_dispostion returned %d\n", err);
		if (file_present)
			goto free_path;
		else
			goto out;
	}

	mode = (umode_t) le64_to_cpu(psx_req->Permissions);
	rsp_info_level = le16_to_cpu(psx_req->Level);
	cifsd_debug("posix_open_flags 0x%x\n", posix_open_flags);

	if (!smb_work->tcon->writeable) {
		if (!file_present) {
			if (posix_open_flags & O_CREAT) {
				err = -EACCES;
				cifsd_debug("returning as user does not have permission to write\n");
			} else {
				err = -ENOENT;
				cifsd_debug("returning as file does not exist\n");
			}
			goto out;
		}
		goto free_path;
	}

	/* posix mkdir command */
	if (posix_open_flags == (O_DIRECTORY | O_CREAT)) {
		if (file_present) {
			err = -EEXIST;
			goto free_path;
		}

		err = smb_vfs_mkdir(name, mode);
		if (err)
			goto out;

		err = smb_kern_path(name, 0, &path, 0);
		if (err) {
			cifsd_err("cannot get linux path, err = %d\n", err);
			goto out;
		}
		cifsd_debug("mkdir done for %s, inode %lu\n",
				name, path.dentry->d_inode->i_ino);
		fid = 0;
		goto prepare_rsp;
	}

	if (!file_present && (posix_open_flags & O_CREAT)) {
		err = smb_vfs_create(name, mode);
		if (err)
			goto out;

		err = smb_kern_path(name, 0, &path, 0);
		if (err) {
			cifsd_err("cannot get linux path, err = %d\n", err);
			goto out;
		}
	}

	err = smb_dentry_open(smb_work, &path, posix_open_flags,
			      &fid, &oplock_flags, 0, file_present);
	if (err)
		goto free_path;

prepare_rsp:
	/* open/mkdir success, send back response */
	data_offset = sizeof(TRANSACTION2_SPI_RSP) -
		sizeof(pSMB_rsp->hdr.smb_buf_length) +
		3 /*alignment*/;
	psx_rsp = (OPEN_PSX_RSP *)(((char *)&pSMB_rsp->hdr.Protocol) +
			data_offset);

	psx_rsp->OplockFlags = cpu_to_le16(oplock_flags);
	psx_rsp->Fid = fid;

	if (file_present) {
		if (!(posix_open_flags & O_TRUNC))
			file_info = F_OPENED;
		else
			file_info = F_OVERWRITTEN;
	} else {
		file_info = F_CREATED;
	}
	psx_rsp->CreateAction = cpu_to_le16(file_info);

	if (rsp_info_level != SMB_QUERY_FILE_UNIX_BASIC) {
		cifsd_debug("returning null information level response");
		rsp_info_level = SMB_NO_INFO_LEVEL_RESPONSE;
	}
	psx_rsp->ReturnedLevel = cpu_to_le16(rsp_info_level);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
	err = vfs_getattr(&path, &stat, STATX_BASIC_STATS,
		AT_STATX_SYNC_AS_STAT);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
	err = vfs_getattr(&path, &stat);
#else
	err = vfs_getattr(path.mnt, path.dentry, &stat);
#endif
	if (err) {
		cifsd_err("cannot get stat information\n");
		goto free_path;
	}

	pSMB_rsp->hdr.Status.CifsError = NT_STATUS_OK;
	unix_info = (FILE_UNIX_BASIC_INFO *)((char *)psx_rsp +
			sizeof(OPEN_PSX_RSP));
	init_unix_info(unix_info, &stat);

	pSMB_rsp->hdr.WordCount = 10;
	pSMB_rsp->t2.TotalParameterCount = cpu_to_le16(2);
	pSMB_rsp->t2.TotalDataCount = cpu_to_le16(sizeof(OPEN_PSX_RSP) +
			sizeof(FILE_UNIX_BASIC_INFO));
	pSMB_rsp->t2.ParameterCount = pSMB_rsp->t2.TotalParameterCount;
	pSMB_rsp->t2.Reserved = 0;
	pSMB_rsp->t2.ParameterCount = cpu_to_le16(2);
	pSMB_rsp->t2.ParameterOffset = cpu_to_le16(56);
	pSMB_rsp->t2.ParameterDisplacement = 0;
	pSMB_rsp->t2.DataCount = pSMB_rsp->t2.TotalDataCount;
	pSMB_rsp->t2.DataOffset = cpu_to_le16(data_offset);
	pSMB_rsp->t2.DataDisplacement = 0;
	pSMB_rsp->t2.SetupCount = 0;
	pSMB_rsp->t2.Reserved1 = 0;

	/* 2 for paramater count + 112 data count + 3 pad (1 pad1 + 2 pad2)*/
	pSMB_rsp->ByteCount = 117;
	pSMB_rsp->Reserved2 = 0;
	inc_rfc1001_len(&pSMB_rsp->hdr,
			(pSMB_rsp->hdr.WordCount * 2 + pSMB_rsp->ByteCount));

free_path:
	path_put(&path);
out:
	switch (err) {
	case 0:
		conn->stats.open_files_count++;
		break;
	case -ENOSPC:
		pSMB_rsp->hdr.Status.CifsError = NT_STATUS_DISK_FULL;
		break;
	case -EINVAL:
		pSMB_rsp->hdr.Status.CifsError = NT_STATUS_NO_SUCH_USER;
		break;
	case -EACCES:
		pSMB_rsp->hdr.Status.CifsError = NT_STATUS_ACCESS_DENIED;
		break;
	case -ENOENT:
		pSMB_rsp->hdr.Status.CifsError =
			NT_STATUS_OBJECT_NAME_NOT_FOUND;
		break;
	default:
		pSMB_rsp->hdr.Status.CifsError =
			NT_STATUS_UNEXPECTED_IO_ERROR;
	}
	smb_put_name(name);
	return err;
}

/**
 * smb_posix_unlink() - handler for posix file delete
 * @smb_work:	smb work containing trans2 posix delete command
 *
 * Return:	0 on success, otherwise error
 */
int smb_posix_unlink(struct smb_work *smb_work)
{
	TRANSACTION2_SPI_REQ *req = (TRANSACTION2_SPI_REQ *)smb_work->buf;
	TRANSACTION2_RSP *rsp = (TRANSACTION2_RSP *)smb_work->rsp_buf;
	UNLINK_PSX_RSP *psx_rsp = NULL;
	char *name;
	int rc = 0;

	name = smb_get_name(req->FileName, PATH_MAX, smb_work, false);
	if (IS_ERR(name))
		return PTR_ERR(name);

	rc = smb_vfs_remove_file(name);
	if (rc < 0)
		goto out;

	psx_rsp = (UNLINK_PSX_RSP *)((char *)rsp + sizeof(TRANSACTION2_RSP));
	psx_rsp->EAErrorOffset = cpu_to_le16(0);
	rsp->hdr.Status.CifsError = NT_STATUS_OK;

	rsp->hdr.WordCount = 10;
	rsp->t2.TotalParameterCount = cpu_to_le16(2);
	rsp->t2.TotalDataCount = cpu_to_le16(0);
	rsp->t2.ParameterCount = rsp->t2.TotalParameterCount;
	rsp->t2.Reserved = 0;
	rsp->t2.ParameterCount = cpu_to_le16(2);
	rsp->t2.ParameterOffset = cpu_to_le16(56);
	rsp->t2.ParameterDisplacement = 0;
	rsp->t2.DataCount = rsp->t2.TotalDataCount;
	rsp->t2.DataOffset = cpu_to_le16(0);
	rsp->t2.DataDisplacement = 0;
	rsp->t2.SetupCount = 0;
	rsp->t2.Reserved1 = 0;

	/* 2 for paramater count + 1 pad1*/
	rsp->ByteCount = 3;
	rsp->Pad = 0;
	inc_rfc1001_len(&rsp->hdr,
			(rsp->hdr.WordCount * 2 + rsp->ByteCount));

out:
	if (rc)
		rsp->hdr.Status.CifsError = NT_STATUS_UNEXPECTED_IO_ERROR;

	smb_put_name(name);
	return rc;
}

/**
 * smb_set_time_pathinfo() - handler for setting time using set path info
 * @smb_work:	smb work containing set path info command
 *
 * Return:	0 on success, otherwise error
 */
int smb_set_time_pathinfo(struct smb_work *smb_work)
{
	TRANSACTION2_SPI_REQ *req = (TRANSACTION2_SPI_REQ *)smb_work->buf;
	TRANSACTION2_RSP *rsp = (TRANSACTION2_RSP *)smb_work->rsp_buf;
	FILE_BASIC_INFO *info;
	struct iattr attrs;
	char *name;
	int err = 0;

	name = smb_get_name(req->FileName, PATH_MAX, smb_work, false);
	if (IS_ERR(name))
		return PTR_ERR(name);

	info = (FILE_BASIC_INFO *)(((char *) &req->hdr.Protocol) +
			le16_to_cpu(req->DataOffset));

	attrs.ia_valid = 0;
	if (le64_to_cpu(info->LastAccessTime)) {
		attrs.ia_atime = smb_NTtimeToUnix(
					le64_to_cpu(info->LastAccessTime));
		attrs.ia_valid |= (ATTR_ATIME | ATTR_ATIME_SET);
	}

	if (le64_to_cpu(info->ChangeTime)) {
		attrs.ia_ctime = smb_NTtimeToUnix(
					le64_to_cpu(info->ChangeTime));
		attrs.ia_valid |= ATTR_CTIME;
	}

	if (le64_to_cpu(info->LastWriteTime)) {
		attrs.ia_mtime = smb_NTtimeToUnix(
					le64_to_cpu(info->LastWriteTime));
		attrs.ia_valid |= (ATTR_MTIME | ATTR_MTIME_SET);
	}
	/* TODO: check dos mode and acl bits if req->Attributes nonzero */

	if (!attrs.ia_valid)
		goto done;

	err = smb_vfs_setattr(smb_work->sess, name, 0, &attrs);
	if (err) {
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_PARAMETER;
		return err;
	}

done:
	cifsd_debug("%s setattr done\n", name);
	rsp->hdr.Status.CifsError = NT_STATUS_OK;
	rsp->hdr.WordCount = 10;
	rsp->t2.TotalParameterCount = cpu_to_le16(2);
	rsp->t2.TotalDataCount = 0;
	rsp->t2.Reserved = 0;
	rsp->t2.ParameterCount = rsp->t2.TotalParameterCount;
	rsp->t2.ParameterOffset = cpu_to_le16(56);
	rsp->t2.ParameterDisplacement = 0;
	rsp->t2.DataCount = rsp->t2.TotalDataCount;
	rsp->t2.DataOffset = 0;
	rsp->t2.DataDisplacement = 0;
	rsp->t2.SetupCount = 0;
	rsp->t2.Reserved1 = 0;

	/* 3 pad (1 pad1 + 2 pad2)*/
	rsp->ByteCount = 3;
	inc_rfc1001_len(&rsp->hdr,
			(rsp->hdr.WordCount * 2 + rsp->ByteCount));

	smb_put_name(name);
	return 0;
}

/**
 * smb_set_unix_pathinfo() - handler for setting unix path info(setattr)
 * @smb_work:	smb work containing set path info command
 *
 * Return:	0 on success, otherwise error
 */
int smb_set_unix_pathinfo(struct smb_work *smb_work)
{
	TRANSACTION2_SPI_REQ *req = (TRANSACTION2_SPI_REQ *)smb_work->buf;
	TRANSACTION2_RSP *rsp = (TRANSACTION2_RSP *)smb_work->rsp_buf;
	FILE_UNIX_BASIC_INFO *unix_info;
	struct iattr attrs;
	char *name;
	int err = 0;

	name = smb_get_name(req->FileName, PATH_MAX, smb_work, false);
	if (IS_ERR(name))
		return PTR_ERR(name);

	unix_info =  (FILE_UNIX_BASIC_INFO *) (((char *) &req->hdr.Protocol)
			+ le16_to_cpu(req->DataOffset));
	attrs.ia_valid = 0;
	attrs.ia_mode = 0;
	err = unix_info_to_attr(unix_info, &attrs);
	if (err)
		goto out;

	err = smb_vfs_setattr(smb_work->sess, name, 0, &attrs);
	if (err)
		goto out;
	/* setattr success, prepare response */
	rsp->hdr.Status.CifsError = NT_STATUS_OK;
	rsp->hdr.WordCount = 10;
	rsp->t2.TotalParameterCount = cpu_to_le16(2);
	rsp->t2.TotalDataCount = 0;
	rsp->t2.Reserved = 0;
	rsp->t2.ParameterCount = rsp->t2.TotalParameterCount;
	rsp->t2.ParameterOffset = cpu_to_le16(56);
	rsp->t2.ParameterDisplacement = 0;
	rsp->t2.DataCount = rsp->t2.TotalDataCount;
	rsp->t2.DataOffset = 0;
	rsp->t2.DataDisplacement = 0;
	rsp->t2.SetupCount = 0;
	rsp->t2.Reserved1 = 0;

	/* 3 pad (1 pad1 + 2 pad2)*/
	rsp->ByteCount = 3;
	inc_rfc1001_len(&rsp->hdr,
			(rsp->hdr.WordCount * 2 + rsp->ByteCount));

out:
	smb_put_name(name);
	if (err) {
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_PARAMETER;
		return err;
	}
	return 0;
}

/**
 * smb_set_ea() - handler for setting extended attributes using set path
 *		info command
 * @smb_work:	smb work containing set path info command
 *
 * Return:	0 on success, otherwise error
 */
int smb_set_ea(struct smb_work *smb_work)
{
	TRANSACTION2_SPI_REQ *req = (TRANSACTION2_SPI_REQ *)smb_work->buf;
	TRANSACTION2_RSP *rsp = (TRANSACTION2_RSP *)smb_work->rsp_buf;
	struct fealist *eabuf;
	char *fname, *attr_name = NULL, *value;
	int rc = 0;

	fname = smb_get_name(req->FileName, PATH_MAX, smb_work, false);
	if (IS_ERR(fname))
		return PTR_ERR(fname);

	eabuf = (struct fealist *)(((char *) &req->hdr.Protocol)
			+ le16_to_cpu(req->DataOffset));
	if (strlen(eabuf->list[0].name) >
			(XATTR_NAME_MAX - XATTR_USER_PREFIX_LEN)) {
		smb_put_name(fname);
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_PARAMETER;
		return -ERANGE;
	}

	if (le32_to_cpu(eabuf->list_len) != (sizeof(*eabuf) +
				eabuf->list[0].name_len +
				le16_to_cpu(eabuf->list[0].value_len))) {
		cifsd_err("bad EA\n");
		smb_put_name(fname);
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_PARAMETER;
		return -EINVAL;
	}

	attr_name = kmalloc(XATTR_NAME_MAX + 1, GFP_KERNEL);
	if (!attr_name) {
		rc = -ENOMEM;
		goto out;
	}

	memcpy(attr_name, XATTR_USER_PREFIX, XATTR_USER_PREFIX_LEN);
	memcpy(&attr_name[XATTR_USER_PREFIX_LEN], eabuf->list[0].name,
			eabuf->list[0].name_len);
	attr_name[XATTR_USER_PREFIX_LEN + eabuf->list[0].name_len] = '\0';
	value = (char *)&eabuf->list[0].name + eabuf->list[0].name_len + 1;
	cifsd_debug("name: <%s>, name_len %u, value_len %u\n",
			eabuf->list[0].name, eabuf->list[0].name_len,
			le16_to_cpu(eabuf->list[0].value_len));

	rc = smb_vfs_setxattr(fname, NULL, attr_name, value,
			le16_to_cpu(eabuf->list[0].value_len), 0);
	if (rc < 0) {
		rsp->hdr.Status.CifsError = NT_STATUS_UNEXPECTED_IO_ERROR;
		goto out;
	}

	rsp->hdr.Status.CifsError = NT_STATUS_OK;
	rsp->hdr.WordCount = 10;
	rsp->t2.TotalParameterCount = cpu_to_le16(2);
	rsp->t2.TotalDataCount = cpu_to_le16(0);
	rsp->t2.ParameterCount = rsp->t2.TotalParameterCount;
	rsp->t2.Reserved = 0;
	rsp->t2.ParameterCount = cpu_to_le16(2);
	rsp->t2.ParameterOffset = cpu_to_le16(56);
	rsp->t2.ParameterDisplacement = 0;
	rsp->t2.DataCount = rsp->t2.TotalDataCount;
	rsp->t2.DataOffset = cpu_to_le16(0);
	rsp->t2.DataDisplacement = 0;
	rsp->t2.SetupCount = 0;
	rsp->t2.Reserved1 = 0;

	/* 2 for paramater count + 1 pad1*/
	rsp->ByteCount = 3;
	rsp->Pad = 0;
	inc_rfc1001_len(&rsp->hdr,
			(rsp->hdr.WordCount * 2 + rsp->ByteCount));

out:
	kfree(attr_name);
	smb_put_name(fname);
	return rc;
}

/**
 * smb_set_file_size_pinfo() - handler for setting eof or truncate using
 *		trans2 set path info command
 * @smb_work:	smb work containing set path info command
 *
 * Return:	0 on success, otherwise error
 */
int smb_set_file_size_pinfo(struct smb_work *smb_work)
{
	TRANSACTION2_SPI_REQ *req = (TRANSACTION2_SPI_REQ *)smb_work->buf;
	TRANSACTION2_RSP *rsp = (TRANSACTION2_RSP *)smb_work->rsp_buf;
	struct file_end_of_file_info *eofinfo;
	char *name = NULL;
	loff_t newsize;
	int rc = 0;

	name = smb_get_name(req->FileName, PATH_MAX, smb_work, false);
	if (IS_ERR(name))
		return PTR_ERR(name);

	eofinfo =  (struct file_end_of_file_info *)
		(((char *) &req->hdr.Protocol) + le16_to_cpu(req->DataOffset));
	newsize = le64_to_cpu(eofinfo->FileSize);
	rc = smb_vfs_truncate(smb_work->sess, name, 0, newsize);
	if (rc) {
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_PARAMETER;
		return rc;
	}
	cifsd_debug("%s truncated to newsize %lld\n",
			name, newsize);
	rsp->hdr.Status.CifsError = NT_STATUS_OK;
	rsp->hdr.WordCount = 10;
	rsp->t2.TotalParameterCount = cpu_to_le16(2);
	rsp->t2.TotalDataCount = 0;
	rsp->t2.Reserved = 0;
	rsp->t2.ParameterCount = rsp->t2.TotalParameterCount;
	rsp->t2.ParameterOffset = cpu_to_le16(56);
	rsp->t2.ParameterDisplacement = 0;
	rsp->t2.DataCount = rsp->t2.TotalDataCount;
	rsp->t2.DataOffset = 0;
	rsp->t2.DataDisplacement = 0;
	rsp->t2.SetupCount = 0;
	rsp->t2.Reserved1 = 0;

	/* 2 for paramater count + 1 pad1*/
	rsp->ByteCount = 3;
	inc_rfc1001_len(&rsp->hdr,
			(rsp->hdr.WordCount * 2 + rsp->ByteCount));

	smb_put_name(name);
	return 0;
}

/**
 * set_path_info() - handler for trans2 set path info sub commands
 * @smb_work:	smb work containing set path info command
 *
 * Return:	0 on success, otherwise error
 */
int set_path_info(struct smb_work *smb_work)
{
	TRANSACTION2_SPI_REQ *pSMB_req = (TRANSACTION2_SPI_REQ *)smb_work->buf;
	TRANSACTION2_SPI_RSP *pSMB_rsp =
				(TRANSACTION2_SPI_RSP *)smb_work->rsp_buf;
	__u16 info_level, total_param;
	int err = 0;

	info_level = le16_to_cpu(pSMB_req->InformationLevel);
	total_param = le16_to_cpu(pSMB_req->TotalParameterCount);
	if (total_param < 7) {
		pSMB_rsp->hdr.Status.CifsError = NT_STATUS_INVALID_PARAMETER;
		cifsd_err("invalid total parameter for info_level 0x%x\n",
				total_param);
		return -EINVAL;
	}

	if (pSMB_req->hdr.WordCount != 15) {
		pSMB_rsp->hdr.Status.CifsError = NT_STATUS_INVALID_PARAMETER;
		cifsd_err("word count mismatch: expected 15 got %d\n",
				pSMB_req->hdr.WordCount);
		return -EINVAL;
	}

	switch (info_level) {
	case SMB_POSIX_OPEN:
		err = smb_posix_open(smb_work);
		break;
	case SMB_POSIX_UNLINK:
		err = smb_posix_unlink(smb_work);
		break;
	case SMB_SET_FILE_UNIX_HLINK:
		err = smb_creat_hardlink(smb_work);
		break;
	case SMB_SET_FILE_UNIX_LINK:
		err = smb_creat_symlink(smb_work);
		break;
	case SMB_SET_FILE_BASIC_INFO:
		/* fall through */
	case SMB_SET_FILE_BASIC_INFO2:
		err = smb_set_time_pathinfo(smb_work);
		break;
	case SMB_SET_FILE_UNIX_BASIC:
		err = smb_set_unix_pathinfo(smb_work);
		break;
	case SMB_SET_FILE_EA:
		err = smb_set_ea(smb_work);
		break;
	case SMB_SET_POSIX_ACL:
		err = smb_set_acl(smb_work);
		break;
	case SMB_SET_FILE_END_OF_FILE_INFO2:
		/* fall through */
	case SMB_SET_FILE_END_OF_FILE_INFO:
		err = smb_set_file_size_pinfo(smb_work);
		break;
	default:
		cifsd_err("info level = %x not implemented yet\n",
				info_level);
		pSMB_rsp->hdr.Status.CifsError = NT_STATUS_NOT_IMPLEMENTED;
		return -ENOSYS;
	}

	if (err < 0)
		cifsd_debug("info_level 0x%x failed, err %d\n",
				info_level, err);
	return err;
}

/**
 * smb_filldir() - populates a dirent details in readdir
 * @ctx:	dir_context information
 * @name:	dirent name
 * @namelen:	dirent name length
 * @offset:	dirent offset in directory
 * @ino:	dirent inode number
 * @d_type:	dirent type
 *
 * Return:	0 on success, otherwise -EINVAL
 */
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
int smb_filldir(struct dir_context *ctx, const char *name, int namlen,
		loff_t offset, u64 ino, unsigned int d_type)
#else
int smb_filldir(void *__buf, const char *name, int namlen,
		loff_t offset, u64 ino, unsigned int d_type)
#endif
{
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
	struct smb_readdir_data *buf =
		container_of(ctx, struct smb_readdir_data, ctx);
#else
	struct smb_readdir_data *buf = __buf;
#endif
	struct smb_dirent *de = (void *)(buf->dirent + buf->used);
	unsigned int reclen;

	reclen = ALIGN(sizeof(struct smb_dirent) + namlen, sizeof(u64));
	if (buf->used + reclen > PAGE_SIZE) {
		buf->full = 1;
		return -EINVAL;
	}

	de->namelen = namlen;
	de->offset = offset;
	de->ino = ino;
	de->d_type = d_type;
	memcpy(de->name, name, namlen);
	buf->used += reclen;
	buf->dirent_count++;

	return 0;
}

/*
 * fill_file_attributes() - fill FileAttributes of directory entry in smb_kstat.
 * if related config is not yes, just fill 0x10(dir) or 0x80(regular file).
 *
 * @smb_work: smb work containing share config
 * @path: path info
 * @smb_kstat: cifsd kstat wrapper
 */

void fill_file_attributes(struct smb_work *smb_work,
	struct path *path, struct smb_kstat *smb_kstat)
{
	/*
	 * set default value for the case that store dos attributes is not yes
	 * or that acl is disable in server's filesystem and the config is yes.
	 */
	if (S_ISDIR(smb_kstat->kstat->mode))
		smb_kstat->file_attributes = ATTR_DIRECTORY;
	else
		smb_kstat->file_attributes = ATTR_ARCHIVE;

	if (get_attr_store_dos(&smb_work->tcon->share->config.attr)) {
		char *file_attribute = NULL;
		int rc;

		rc = smb_find_cont_xattr(path,
			XATTR_NAME_FILE_ATTRIBUTE,
			XATTR_NAME_FILE_ATTRIBUTE_LEN, &file_attribute, 1);

		if (rc > 0)
			smb_kstat->file_attributes =
				*((__le32 *)file_attribute);
		else
			cifsd_debug("fail to fill file attributes.\n");

		kvfree(file_attribute);
	}
}

/**
 * fill_create_time() - fill create time of directory entry in smb_kstat
 * if related config is not yes, create time is same with change time
 *
 * @smb_work: smb work containing share config
 * @path: path info
 * @smb_kstat: cifsd kstat wrapper
 */

void fill_create_time(struct smb_work *smb_work,
	struct path *path, struct smb_kstat *smb_kstat)
{
	char *create_time = NULL;
	int xattr_len;

	/*
	 * if "store dos attributes" conf is not yes,
	 * create time = change time
	 */
	smb_kstat->create_time = cifs_UnixTimeToNT(smb_kstat->kstat->ctime);

	if (get_attr_store_dos(&smb_work->tcon->share->config.attr)) {
		xattr_len = smb_find_cont_xattr(path,
			XATTR_NAME_CREATION_TIME,
			XATTR_NAME_CREATION_TIME_LEN, &create_time, 1);

		if (xattr_len > 0)
			smb_kstat->create_time = *((u64 *)create_time);

		kvfree(create_time);
	}
}

/**
 * read_next_entry() - read next directory entry and return absolute name
 * @smb_work:	smb work containing share config
 * @smb_kstat:	cifsd wrapper of next dirent's stat
 * @de:		directory entry
 * @dirpath:	directory path name
 *
 * Return:      on success return absolute path of directory entry,
 *              otherwise NULL
 */
char *read_next_entry(struct smb_work *smb_work, struct smb_kstat *smb_kstat,
		struct smb_dirent *de, char *dirpath)
{
	struct path path;
	int rc, file_pathlen, dir_pathlen;
	char *name;

	dir_pathlen = strlen(dirpath);
	/* 1 for '/'*/
	file_pathlen = dir_pathlen +  de->namelen + 1;
	name = kmalloc(file_pathlen + 1, GFP_KERNEL);
	if (!name) {
		cifsd_err("Name memory failed for length %d,"
				" buf_name_len %d\n", dir_pathlen, de->namelen);
		return ERR_PTR(-ENOMEM);
	}

	memcpy(name, dirpath, dir_pathlen);
	memset(name + dir_pathlen, '/', 1);
	memcpy(name + dir_pathlen + 1, de->name, de->namelen);
	name[file_pathlen] = '\0';

	rc = smb_kern_path(name, 0, &path, 1);
	if (rc) {
		cifsd_err("look up failed for (%s) with rc=%d\n", name, rc);
		kfree(name);
		return ERR_PTR(rc);
	}

	generic_fillattr(path.dentry->d_inode, smb_kstat->kstat);
	fill_create_time(smb_work, &path, smb_kstat);
	fill_file_attributes(smb_work, &path, smb_kstat);
	memcpy(name, de->name, de->namelen);
	name[de->namelen] = '\0';
	path_put(&path);
	return name;
}

/**
 * fill_common_info() - convert unix stat information to smb stat format
 * @p:          destination buffer
 * @smb_kstat:      cifsd kstat wrapper
 */
void *fill_common_info(char **p, struct smb_kstat *smb_kstat)
{
	FILE_DIRECTORY_INFO *info = (FILE_DIRECTORY_INFO *)(*p);
	info->FileIndex = 0;
	info->CreationTime = cpu_to_le64(smb_kstat->create_time);
	info->LastAccessTime = cpu_to_le64(
			cifs_UnixTimeToNT(smb_kstat->kstat->atime));
	info->LastWriteTime = cpu_to_le64(
			cifs_UnixTimeToNT(smb_kstat->kstat->mtime));
	info->ChangeTime = cpu_to_le64(
			cifs_UnixTimeToNT(smb_kstat->kstat->ctime));
	info->EndOfFile = cpu_to_le64(smb_kstat->kstat->size);
	info->AllocationSize = cpu_to_le64(smb_kstat->kstat->blocks << 9);
	info->ExtFileAttributes = cpu_to_le32(smb_kstat->file_attributes);

	return info;
}

/**
 * convname_updatenextoffset() - convert name to UTF, update next_entry_offset
 * @namestr:            source filename buffer
 * @len:                source buffer length
 * @size:               used buffer size
 * @local_nls           code page table
 * @name_len:           file name length after conversion
 * @next_entry_offset:  offset of dentry
 * @buf_len:            response buffer length
 * @data_count:         used response buffer size
 *
 * Return:      return error if next entry could not fit in current response
 *              buffer, otherwise return encode buffer.
 */
char *convname_updatenextoffset(char *namestr, int len, int size,
		const struct nls_table *local_nls, int *name_len,
		int *next_entry_offset, int *buf_len, int *data_count,
		int alignment)
{
	char *enc_buf;

	enc_buf = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!enc_buf)
		return NULL;

	*name_len = smbConvertToUTF16((__le16 *)enc_buf,
			namestr, len, local_nls, 0);
	*name_len *= 2;

	*next_entry_offset = (size - 1 + *name_len + alignment) & ~alignment;

	if (*next_entry_offset > *buf_len) {
		cifsd_debug("buf_len : %d next_entry_offset : %d"
				" data_count : %d\n", *buf_len,
				*next_entry_offset, *data_count);
		*buf_len = -1;
		kfree(enc_buf);
		return NULL;
	}
	return enc_buf;
}

/**
 * smb_populate_readdir_entry() - encode directory entry in smb response buffer
 * @conn:	TCP server instance of connection
 * @info_level:	smb information level
 * @p:		smb response buffer pointer
 * @reclen:	smb record length
 * @namestr:	dirent name string
 * @buf_len:	response buffer length
 * @last_entry_offset:	offset of last entry in directory
 * @smb_kstat: cifsd wrapper of dirent stat information
 * @data_count:	used buffer size
 * @num_entry:	number of dirents searched so far
 *
 * if directory has many entries, find first can't read it fully.
 * find next might be called multiple times to read remaining dir entries
 *
 * Return:	0 on success, otherwise error
 */
static int smb_populate_readdir_entry(struct connection *conn,
		int info_level, char **p, int reclen, char *namestr,
		int *buf_len, int *last_entry_offset,
		struct smb_kstat *smb_kstat, int *data_count, int *num_entry)
{
	int name_len;
	int next_entry_offset;
	char *utfname = NULL;

	switch (info_level) {
	case SMB_FIND_FILE_DIRECTORY_INFO:
	{
		FILE_DIRECTORY_INFO *fdinfo = NULL;

		utfname = convname_updatenextoffset(namestr, PATH_MAX,
				sizeof(FILE_DIRECTORY_INFO),
				conn->local_nls, &name_len,
				&next_entry_offset,
				buf_len, data_count, 3);
		if (!utfname)
			break;

		fdinfo = (FILE_DIRECTORY_INFO *)
			fill_common_info(p, smb_kstat);
		fdinfo->FileNameLength = cpu_to_le32(name_len);
		memcpy(fdinfo->FileName, utfname, name_len);
		fdinfo->FileName[name_len - 2] = 0;
		fdinfo->FileName[name_len - 1] = 0;
		fdinfo->NextEntryOffset = next_entry_offset;
		memset((char *)fdinfo + sizeof(FILE_DIRECTORY_INFO) - 1 +
				name_len, '\0', next_entry_offset -
				(sizeof(FILE_DIRECTORY_INFO) - 1 + name_len));
		*p =  (char *)(*p) + next_entry_offset;
		break;
	}
	case SMB_FIND_FILE_FULL_DIRECTORY_INFO:
	{
		FILE_FULL_DIRECTORY_INFO *ffdinfo = NULL;

		utfname = convname_updatenextoffset(namestr, PATH_MAX,
				sizeof(FILE_FULL_DIRECTORY_INFO),
				conn->local_nls, &name_len,
				&next_entry_offset,
				buf_len, data_count, 3);
		if (!utfname)
			break;

		ffdinfo = (FILE_FULL_DIRECTORY_INFO *)
			fill_common_info(p, smb_kstat);
		ffdinfo->FileNameLength = cpu_to_le32(name_len);
		ffdinfo->EaSize = 0;
		memcpy(ffdinfo->FileName, utfname, name_len);
		ffdinfo->FileName[name_len - 2] = 0;
		ffdinfo->FileName[name_len - 1] = 0;
		ffdinfo->NextEntryOffset = next_entry_offset;
		memset((char *)ffdinfo + sizeof(FILE_FULL_DIRECTORY_INFO) - 1 +
				name_len, '\0', next_entry_offset -
				(sizeof(FILE_FULL_DIRECTORY_INFO) - 1 +
				 name_len));
		*p =  (char *)(*p) + next_entry_offset;
		break;
	}
	case SMB_FIND_FILE_BOTH_DIRECTORY_INFO:
	{
		FILE_BOTH_DIRECTORY_INFO *fbdinfo = NULL;

		utfname = convname_updatenextoffset(namestr, PATH_MAX,
				sizeof(FILE_BOTH_DIRECTORY_INFO),
				conn->local_nls, &name_len,
				&next_entry_offset,
				buf_len, data_count, 3);
		if (!utfname)
			break;

		fbdinfo = (FILE_BOTH_DIRECTORY_INFO *)
			fill_common_info(p, smb_kstat);
		fbdinfo->FileNameLength = cpu_to_le32(name_len);
		fbdinfo->EaSize = 0;
		fbdinfo->ShortNameLength = 0;
		fbdinfo->Reserved = 0;
		memset(fbdinfo->ShortName, '\0', 24);
		memcpy(fbdinfo->FileName, utfname, name_len);
		fbdinfo->FileName[name_len - 2] = 0;
		fbdinfo->FileName[name_len - 1] = 0;
		fbdinfo->NextEntryOffset = next_entry_offset;
		memset((char *)fbdinfo + sizeof(FILE_BOTH_DIRECTORY_INFO) - 1 +
				name_len, '\0', next_entry_offset -
				sizeof(FILE_BOTH_DIRECTORY_INFO) - 1 +
				name_len);
		*p =  (char *)(*p) + next_entry_offset;
		break;
	}
	case SMB_FIND_FILE_ID_FULL_DIR_INFO:
	{
		SEARCH_ID_FULL_DIR_INFO *dinfo = NULL;

		utfname = convname_updatenextoffset(namestr, PATH_MAX,
				sizeof(SEARCH_ID_FULL_DIR_INFO),
				conn->local_nls, &name_len,
				&next_entry_offset,
				buf_len, data_count, 3);
		if (!utfname)
			break;

		dinfo = (SEARCH_ID_FULL_DIR_INFO *)
			fill_common_info(p, smb_kstat);
		dinfo->FileNameLength = cpu_to_le32(name_len);
		dinfo->EaSize = 0;
		dinfo->Reserved = 0;
		dinfo->UniqueId = cpu_to_le64(smb_kstat->kstat->ino);
		memcpy(dinfo->FileName, utfname, name_len);
		dinfo->FileName[name_len - 2] = 0;
		dinfo->FileName[name_len - 1] = 0;
		dinfo->NextEntryOffset = next_entry_offset;
		memset((char *)dinfo + sizeof(SEARCH_ID_FULL_DIR_INFO) - 1 +
				name_len, '\0', next_entry_offset -
				sizeof(SEARCH_ID_FULL_DIR_INFO) - 1 + name_len);
		*p =  (char *)(*p) + next_entry_offset;
		break;
	}
	case SMB_FIND_FILE_UNIX:
	{
		FILE_UNIX_INFO *finfo = NULL;
		FILE_UNIX_BASIC_INFO *unix_info;

		utfname = convname_updatenextoffset(namestr, PATH_MAX,
				sizeof(FILE_UNIX_INFO),
				conn->local_nls, &name_len,
				&next_entry_offset,
				buf_len, data_count, 3);
		if (!utfname)
			break;

		finfo = (FILE_UNIX_INFO *)(*p);
		finfo->ResumeKey = 0;
		unix_info = (FILE_UNIX_BASIC_INFO *)((char *)finfo + 8);
		init_unix_info(unix_info, smb_kstat->kstat);
		memcpy(finfo->FileName, utfname, name_len);
		finfo->FileName[name_len - 2] = 0;
		finfo->FileName[name_len - 1] = 0;
		finfo->NextEntryOffset = next_entry_offset;
		memset((char *)finfo + sizeof(FILE_UNIX_INFO) - 1 + name_len,
				'\0', next_entry_offset -
				(sizeof(FILE_UNIX_INFO) - 1 + name_len));
		*p =  (char *)(*p) + next_entry_offset;
		break;
	}
	default:
		cifsd_err("%s: failed\n", __func__);
		return -EOPNOTSUPP;
	}

	if (utfname) {
		*last_entry_offset = *data_count;
		*data_count += next_entry_offset;
		*buf_len -= next_entry_offset;
		(*num_entry)++;
		kfree(utfname);
	}

	cifsd_debug("info_level : %d, buf_len :%d,"
			" next_offset : %d, data_count : %d\n",
			info_level, *buf_len,
			next_entry_offset, *data_count);
	return 0;
}


/**
 * find_first() - smb readdir command
 * @smb_work:	smb work containing find first request params
 *
 * Return:	0 on success, otherwise error
 */
int find_first(struct smb_work *smb_work)
{
	struct smb_hdr *rsp_hdr = (struct smb_hdr *)smb_work->rsp_buf;
	struct connection *conn = smb_work->conn;
	struct cifsd_sess *sess = smb_work->sess;
	struct smb_trans2_req *req = (struct smb_trans2_req *)smb_work->buf;
	TRANSACTION2_RSP *rsp = (TRANSACTION2_RSP *)smb_work->rsp_buf;
	TRANSACTION2_FFIRST_REQ_PARAMS *req_params;
	T2_FFIRST_RSP_PARMS *params = NULL;
	struct path path;
	struct smb_dirent *de;
	struct cifsd_file *dir_fp = NULL;
	struct kstat kstat;
	struct smb_kstat smb_kstat;
	int params_count = sizeof(T2_FFIRST_RSP_PARMS);
	int data_alignment_offset = 0;
	int data_count = 0;
	int num_entry = 0;
	int last_entry_offset = 0;
	int oplock = 0;
	int rc = 0, reclen = 0;
	int out_buf_len;
	__u16 sid;
	char *bufptr = NULL;
	char *namestr = NULL;
	char *dirpath = NULL;
	char *srch_ptr = NULL;
	struct smb_readdir_data r_data = {
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
		.ctx.actor = smb_filldir,
#endif
		.dirent = (void *)__get_free_page(GFP_KERNEL)
	};

	if (!r_data.dirent) {
		rsp->hdr.Status.CifsError = NT_STATUS_NO_MEMORY;
		return -ENOMEM;
	}

	req_params = (TRANSACTION2_FFIRST_REQ_PARAMS *)(smb_work->buf +
			req->ParameterOffset + 4);
	dirpath = smb_get_dir_name(req_params->FileName, PATH_MAX,
			smb_work, &srch_ptr);
	if (IS_ERR(dirpath)) {
		rsp->hdr.Status.CifsError = NT_STATUS_NO_MEMORY;
		rc = PTR_ERR(dirpath);
		goto err_out;
	}

	cifsd_debug("complete dir path = %s\n",  dirpath);
	rc = smb_kern_path(dirpath, LOOKUP_FOLLOW | LOOKUP_DIRECTORY,
			&path, 0);
	if (rc < 0) {
		cifsd_debug("cannot create vfs root path <%s> %d\n",
				dirpath, rc);
		goto err_out;
	}

	rc = smb_dentry_open(smb_work, &path, O_RDONLY, &sid,
			&oplock, 0, 1);
	if (rc) {
		cifsd_debug("dir dentry open failed with rc=%d\n", rc);
		path_put(&path);
		goto err_out;
	}

	dir_fp = get_id_from_fidtable(sess, sid);
	if (!dir_fp) {
		cifsd_debug("error invalid sid\n");
		path_put(&path);
		rc = -EINVAL;
		goto err_out;
	}

	dir_fp->readdir_data.dirent = r_data.dirent;
	dir_fp->readdir_data.used = 0;
	dir_fp->readdir_data.full = 0;
	dir_fp->dirent_offset = 0;

	if (params_count % 4)
		data_alignment_offset = 4 - params_count % 4;

	bufptr = (char *)((char *)rsp + sizeof(TRANSACTION2_RSP) + params_count
			+ data_alignment_offset);

	out_buf_len = min((int)(le16_to_cpu(req_params->SearchCount) *
				sizeof(FILE_UNIX_INFO)),
			MAX_CIFS_LOOKUP_BUFFER_SIZE) -
		(sizeof(TRANSACTION2_RSP) +
		 params_count + data_alignment_offset);

	do {
		if (dir_fp->dirent_offset >= dir_fp->readdir_data.used) {
			dir_fp->dirent_offset = 0;
			r_data.used = 0;
			r_data.full = 0;
			rc = smb_vfs_readdir(dir_fp->filp, smb_filldir,
					&r_data);
			if (rc < 0) {
				cifsd_debug("err : %d\n", rc);
				goto err_out;
			}

			dir_fp->readdir_data.used = r_data.used;
			dir_fp->readdir_data.full = r_data.full;
			if (!dir_fp->readdir_data.used) {
				free_page((unsigned long)
						(dir_fp->readdir_data.dirent));
				dir_fp->readdir_data.dirent = NULL;
				break;
			}

			de = (struct smb_dirent *)
				((char *)dir_fp->readdir_data.dirent);
		} else {
			de = (struct smb_dirent *)
				((char *)dir_fp->readdir_data.dirent +
				 dir_fp->dirent_offset);
		}

		reclen = ALIGN(sizeof(struct smb_dirent) + de->namelen,
				sizeof(__le64));
		dir_fp->dirent_offset += reclen;

		smb_kstat.kstat = &kstat;
		namestr = read_next_entry(smb_work, &smb_kstat, de, dirpath);
		if (IS_ERR(namestr)) {
			rc = PTR_ERR(namestr);
			cifsd_debug("Err while dirent read rc = %d\n", rc);
			rc = 0;
			continue;
		}

		cifsd_debug("filename string = %s\n", namestr);
		if (srch_ptr) {
			cifsd_debug("Single entry requested\n");
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
			if (strlen(srch_ptr) != de->namelen ||
				strncasecmp(de->name, srch_ptr,
					de->namelen)) {
#else
			if (strlen(srch_ptr) != de->namelen ||
					strnicmp(de->name, srch_ptr,
						de->namelen)) {
#endif
					kfree(namestr);
					continue;
			}
		}

		rc = smb_populate_readdir_entry(conn,
				req_params->InformationLevel, &bufptr, reclen,
				namestr, &out_buf_len, &last_entry_offset,
				&smb_kstat, &data_count, &num_entry);
		kfree(namestr);
		if (rc)
			goto err_out;

		if (srch_ptr)
			break;
	} while (out_buf_len >= 0);

	if (out_buf_len < 0)
		dir_fp->dirent_offset -= reclen;

	if (srch_ptr && data_count == 0) {
		rsp->hdr.Status.CifsError =
			NT_STATUS_NO_SUCH_FILE;
		rc = -EINVAL;
		goto err_out;
	}

	params = (T2_FFIRST_RSP_PARMS *)((char *)rsp +
			sizeof(TRANSACTION2_RSP));
	params->SearchHandle = cpu_to_le16(sid);
	params->SearchCount = cpu_to_le16(num_entry);

	if (out_buf_len < 0) {
		cifsd_debug("%s continue search\n", __func__);
		params->EndofSearch = cpu_to_le16(0);
		params->LastNameOffset = cpu_to_le16(last_entry_offset);
	} else {
		cifsd_debug("%s end of search\n", __func__);
		params->EndofSearch = cpu_to_le16(1);
		params->LastNameOffset = cpu_to_le16(0);
		path_put(&(dir_fp->filp->f_path));
		close_id(sess, sid, 0);
	}
	params->EAErrorOffset = cpu_to_le16(0);

	rsp_hdr->WordCount = 0x0A;
	rsp->t2.TotalParameterCount = params_count;
	rsp->t2.TotalDataCount = cpu_to_le16(data_count);
	rsp->t2.Reserved = 0;
	rsp->t2.ParameterCount = params_count;
	rsp->t2.ParameterOffset = sizeof(TRANSACTION2_RSP) - 4;
	rsp->t2.ParameterDisplacement = 0;
	rsp->t2.DataCount = cpu_to_le16(data_count);
	rsp->t2.DataOffset = sizeof(TRANSACTION2_RSP) + params_count +
		data_alignment_offset - 4;
	rsp->t2.DataDisplacement = 0;
	rsp->t2.SetupCount = 0;
	rsp->t2.Reserved1 = 0;
	rsp->Pad = 0;
	rsp->ByteCount = cpu_to_le16(data_count) + params_count + 1 /*pad*/ +
		data_alignment_offset;
	memset((char *)rsp + sizeof(TRANSACTION2_RSP) + params_count, '\0', 2);
	inc_rfc1001_len(rsp_hdr, (10 * 2 + data_count + params_count + 1 +
				data_alignment_offset));
	kfree(srch_ptr);
	smb_put_name(dirpath);
	return 0;

err_out:
	if (dir_fp && dir_fp->readdir_data.dirent) {
		path_put(&(dir_fp->filp->f_path));
		close_id(sess, sid, 0);
		if (dir_fp->readdir_data.dirent)  {
			free_page((unsigned long)(dir_fp->readdir_data.dirent));
			dir_fp->readdir_data.dirent = NULL;
		}
	}

	if (rsp->hdr.Status.CifsError == 0)
		rsp->hdr.Status.CifsError =
			NT_STATUS_UNEXPECTED_IO_ERROR;

	kfree(srch_ptr);
	smb_put_name(dirpath);
	return 0;
}

/**
 * find_next() - smb next readdir command
 * @smb_work:	smb work containing find next request params
 *
 * if directory has many entries, find first can't read it fully.
 * find next might be called multiple times to read remaining dir entries
 *
 * Return:	0 on success, otherwise error
 */
int find_next(struct smb_work *smb_work)
{
	struct smb_hdr *rsp_hdr = (struct smb_hdr *)smb_work->rsp_buf;
	struct connection *conn = smb_work->conn;
	struct cifsd_sess *sess = smb_work->sess;
	struct smb_trans2_req *req = (struct smb_trans2_req *)smb_work->buf;
	TRANSACTION2_RSP *rsp = (TRANSACTION2_RSP *)smb_work->rsp_buf;
	TRANSACTION2_FNEXT_REQ_PARAMS *req_params;
	T2_FNEXT_RSP_PARMS *params = NULL;
	struct smb_dirent *de;
	struct cifsd_file *dir_fp;
	struct kstat kstat;
	struct smb_kstat smb_kstat;
	int params_count = sizeof(T2_FNEXT_RSP_PARMS);
	int data_alignment_offset = 0;
	int data_count = 0;
	int num_entry = 0;
	int last_entry_offset = 0;
	int rc = 0, reclen = 0;
	int out_buf_len;
	__u16 sid;
	char *bufptr = NULL;
	char *namestr = NULL;
	char *dirpath = NULL;
	char *name = NULL;
	char *pathname = NULL;
	struct smb_readdir_data r_data = {
#if LINUX_VERSION_CODE > KERNEL_VERSION(3, 10, 30)
		.ctx.actor = smb_filldir,
#endif
	};

	req_params = (TRANSACTION2_FNEXT_REQ_PARAMS *)(smb_work->buf +
			req->ParameterOffset + 4);
	sid = cpu_to_le16(req_params->SearchHandle);

	/*Currently no usage of ResumeFilename*/
	name = req_params->ResumeFileName;
	name = smb_strndup_from_utf16(name, NAME_MAX, 1, conn->local_nls);
	if (IS_ERR(name)) {
		rsp->hdr.Status.CifsError = NT_STATUS_NO_MEMORY;
		return PTR_ERR(name);
	}
	cifsd_debug("FileName after unicode conversion %s\n", name);
	kfree(name);

	dir_fp = get_id_from_fidtable(sess, sid);
	if (!dir_fp) {
		cifsd_debug("error invalid sid\n");
		rc = -EINVAL;
		goto err_out;
	}

	r_data.dirent = dir_fp->readdir_data.dirent;
	pathname = kmalloc(PATH_MAX, GFP_KERNEL);
	if (!pathname) {
		cifsd_debug("Failed to allocate memory\n");
		rsp->hdr.Status.CifsError = NT_STATUS_NO_MEMORY;
		rc = -ENOMEM;
		goto err_out;
	}

	dirpath = d_path(&(dir_fp->filp->f_path), pathname, PATH_MAX);
	if (IS_ERR(dirpath)) {
		rc = PTR_ERR(dirpath);
		goto err_out;
	}

	cifsd_debug("dirpath = %s\n", dirpath);

	if (params_count % 4)
		data_alignment_offset = 4 - params_count % 4;

	bufptr = (char *)((char *)rsp + sizeof(TRANSACTION2_RSP) +
			params_count + data_alignment_offset);

	out_buf_len = min((int)(le16_to_cpu(req_params->SearchCount) *
				sizeof(FILE_UNIX_INFO)),
			MAX_CIFS_LOOKUP_BUFFER_SIZE) -
		(sizeof(TRANSACTION2_RSP) + params_count +
		 data_alignment_offset);

	do {
		if (dir_fp->dirent_offset >= dir_fp->readdir_data.used) {
			dir_fp->dirent_offset = 0;
			r_data.used = 0;
			r_data.full = 0;
			rc = smb_vfs_readdir(dir_fp->filp, smb_filldir,
					&r_data);
			if (rc < 0) {
				cifsd_debug("err : %d\n", rc);
				goto err_out;
			}

			dir_fp->readdir_data.used = r_data.used;
			dir_fp->readdir_data.full = r_data.full;
			if (!dir_fp->readdir_data.used) {
				free_page((unsigned long)
						(dir_fp->readdir_data.dirent));
				dir_fp->readdir_data.dirent = NULL;
				break;
			}

			de = (struct smb_dirent *)
				((char *)dir_fp->readdir_data.dirent);
		} else {
			de = (struct smb_dirent *)
				((char *)dir_fp->readdir_data.dirent +
				 dir_fp->dirent_offset);
		}

		reclen = ALIGN(sizeof(struct smb_dirent) + de->namelen,
				sizeof(__le64));
		dir_fp->dirent_offset += reclen;

		smb_kstat.kstat = &kstat;
		namestr = read_next_entry(smb_work, &smb_kstat, de, dirpath);
		if (IS_ERR(namestr)) {
			rc = PTR_ERR(namestr);
			cifsd_debug("Err while dirent read rc = %d\n", rc);
			rc = 0;
			continue;
		}

		cifsd_debug("filename string = %s\n", namestr);
		rc = smb_populate_readdir_entry(conn,
				req_params->InformationLevel, &bufptr, reclen,
				namestr, &out_buf_len, &last_entry_offset,
				&smb_kstat, &data_count, &num_entry);
		kfree(namestr);
		if (rc)
			goto err_out;

	} while (out_buf_len >= 0);

	if (out_buf_len < 0)
		dir_fp->dirent_offset -= reclen;

	params = (T2_FNEXT_RSP_PARMS *)((char *)rsp + sizeof(TRANSACTION2_RSP));
	params->SearchCount = cpu_to_le16(num_entry);

	if (out_buf_len < 0) {
		cifsd_debug("%s continue search\n", __func__);
		params->EndofSearch = cpu_to_le16(0);
		params->LastNameOffset = cpu_to_le16(last_entry_offset);
	} else {
		cifsd_debug("%s end of search\n", __func__);
		params->EndofSearch = cpu_to_le16(1);
		params->LastNameOffset = cpu_to_le16(0);
		path_put(&(dir_fp->filp->f_path));
		close_id(sess, sid, 0);
	}
	params->EAErrorOffset = cpu_to_le16(0);

	rsp_hdr->WordCount = 0x0A;
	rsp->t2.TotalParameterCount = cpu_to_le16(params_count);
	rsp->t2.TotalParameterCount = cpu_to_le16(params_count);
	rsp->t2.TotalDataCount = cpu_to_le16(data_count);
	rsp->t2.Reserved = 0;
	rsp->t2.ParameterCount = cpu_to_le16(params_count);
	rsp->t2.ParameterOffset = sizeof(TRANSACTION2_RSP) - 4;
	rsp->t2.ParameterDisplacement = 0;
	rsp->t2.DataCount = cpu_to_le16(data_count);
	rsp->t2.DataOffset = sizeof(TRANSACTION2_RSP) +
		cpu_to_le16(params_count) + data_alignment_offset - 4;
	rsp->t2.DataDisplacement = 0;
	rsp->t2.SetupCount = 0;
	rsp->t2.Reserved1 = 0;
	rsp->Pad = 0;
	rsp->ByteCount = cpu_to_le16(data_count) + params_count + 1 +
		data_alignment_offset;
	memset((char *)rsp + sizeof(TRANSACTION2_RSP) +
			cpu_to_le16(params_count), '\0', data_alignment_offset);
	inc_rfc1001_len(rsp_hdr, (10 * 2 + data_count + params_count + 1 +
				data_alignment_offset));
	kfree(pathname);
	return 0;

err_out:
	if (dir_fp && dir_fp->readdir_data.dirent) {
		if (dir_fp->readdir_data.dirent)  {
			free_page((unsigned long)(dir_fp->readdir_data.dirent));
			dir_fp->readdir_data.dirent = NULL;
		}
		path_put(&(dir_fp->filp->f_path));
		close_id(sess, sid, 0);
	}

	if (rsp->hdr.Status.CifsError == 0)
		rsp->hdr.Status.CifsError =
			NT_STATUS_UNEXPECTED_IO_ERROR;

	kfree(pathname);
	return 0;
}

/**
 * create_trans2_reply() - create response for trans2 request
 * @smb_work:	smb work containing smb response buffer
 * @count:	trans2 response buffer size
 */
void create_trans2_reply(struct smb_work *smb_work, __u16 count)
{
	struct smb_hdr *rsp_hdr = (struct smb_hdr *)smb_work->rsp_buf;
	TRANSACTION2_RSP *rsp = (TRANSACTION2_RSP *)smb_work->rsp_buf;

	rsp_hdr->WordCount = 0x0A;
	rsp->t2.TotalParameterCount = 0;
	rsp->t2.TotalDataCount = cpu_to_le16(count);
	rsp->t2.Reserved = 0;
	rsp->t2.ParameterCount = 0;
	rsp->t2.ParameterOffset = 56;
	rsp->t2.ParameterDisplacement = 0;
	rsp->t2.DataCount = cpu_to_le16(count);
	rsp->t2.DataOffset = 56;
	rsp->t2.DataDisplacement = 0;
	rsp->t2.SetupCount = 0;
	rsp->t2.Reserved1 = 0;

	rsp->ByteCount = count + 1;
	rsp->Pad = 0;
	inc_rfc1001_len(rsp_hdr, (10 * 2 + rsp->ByteCount));
}

/**
 * smb_set_unix_fileinfo() - set smb unix file info(setattr)
 * @smb_work:	smb work containing unix basic info buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb_set_unix_fileinfo(struct smb_work *smb_work)
{
	struct smb_com_transaction2_sfi_req *req;
	struct smb_com_transaction2_sfi_rsp *rsp;
	FILE_UNIX_BASIC_INFO *unix_info;
	struct iattr attrs;
	int err = 0;

	req = (struct smb_com_transaction2_sfi_req *)smb_work->buf;
	rsp = (struct smb_com_transaction2_sfi_rsp *)smb_work->rsp_buf;
	unix_info =  (FILE_UNIX_BASIC_INFO *) (((char *) &req->hdr.Protocol)
			+ le16_to_cpu(req->DataOffset));

	attrs.ia_valid = 0;
	attrs.ia_mode = 0;
	err = unix_info_to_attr(unix_info, &attrs);
	if (err)
		goto out;

	err = smb_vfs_setattr(smb_work->sess, NULL, (uint64_t)req->Fid, &attrs);
	if (err)
		goto out;

	/* setattr success, prepare response */
	rsp->hdr.Status.CifsError = NT_STATUS_OK;
	rsp->hdr.WordCount = 10;
	rsp->t2.TotalParameterCount = cpu_to_le16(2);
	rsp->t2.TotalDataCount = 0;
	rsp->t2.Reserved = 0;
	rsp->t2.ParameterCount = rsp->t2.TotalParameterCount;
	rsp->t2.ParameterOffset = cpu_to_le16(56);
	rsp->t2.ParameterDisplacement = 0;
	rsp->t2.DataCount = rsp->t2.TotalDataCount;
	rsp->t2.DataOffset = 0;
	rsp->t2.DataDisplacement = 0;
	rsp->t2.SetupCount = 0;
	rsp->t2.Reserved1 = 0;

	/* 3 pad (1 pad1 + 2 pad2)*/
	rsp->ByteCount = 3;
	rsp->Reserved2 = 0;
	inc_rfc1001_len(&rsp->hdr,
			(rsp->hdr.WordCount * 2 + rsp->ByteCount));

out:
	if (err) {
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_PARAMETER;
		return err;
	}
	return 0;
}

/**
 * smb_set_file_size_finfo() - set file truncate method using trans2
 *		set file info command
 * @smb_work:	smb work containing set file info command buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb_set_file_size_finfo(struct smb_work *smb_work)
{
	struct smb_com_transaction2_sfi_req *req;
	struct smb_com_transaction2_sfi_rsp *rsp;
	struct file_end_of_file_info *eofinfo;
	loff_t newsize;
	int err = 0;

	req = (struct smb_com_transaction2_sfi_req *)smb_work->buf;
	rsp = (struct smb_com_transaction2_sfi_rsp *)smb_work->rsp_buf;

	eofinfo =  (struct file_end_of_file_info *)
		(((char *) &req->hdr.Protocol) + le16_to_cpu(req->DataOffset));

	newsize = le64_to_cpu(eofinfo->FileSize);
	err = smb_vfs_truncate(smb_work->sess, NULL, (uint64_t)req->Fid,
		newsize);
	if (err) {
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_PARAMETER;
		return err;
	}

	cifsd_debug("fid %u, truncated to newsize %lld\n",
			req->Fid, newsize);
	rsp->hdr.Status.CifsError = NT_STATUS_OK;
	rsp->hdr.WordCount = 10;
	rsp->t2.TotalParameterCount = cpu_to_le16(2);
	rsp->t2.TotalDataCount = 0;
	rsp->t2.Reserved = 0;
	rsp->t2.ParameterCount = rsp->t2.TotalParameterCount;
	rsp->t2.ParameterOffset = cpu_to_le16(56);
	rsp->t2.ParameterDisplacement = 0;
	rsp->t2.DataCount = rsp->t2.TotalDataCount;
	rsp->t2.DataOffset = 0;
	rsp->t2.DataDisplacement = 0;
	rsp->t2.SetupCount = 0;
	rsp->t2.Reserved1 = 0;

	/* 3 pad (1 pad1 + 2 pad2)*/
	rsp->ByteCount = 3;
	rsp->Reserved2 = 0;
	inc_rfc1001_len(&rsp->hdr,
			(rsp->hdr.WordCount * 2 + rsp->ByteCount));

	return 0;
}

/**
 * smb_set_alloc_size() - set file truncate method using trans2
 *		set file info command - file allocation info level
 * @smb_work:	smb work containing set file info command buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb_set_alloc_size(struct smb_work *smb_work)
{
	struct smb_com_transaction2_sfi_req *req;
	struct smb_com_transaction2_sfi_rsp *rsp;
	struct file_allocation_info *allocinfo;
	struct kstat stat;
	loff_t newsize;
	int err = 0;

	req = (struct smb_com_transaction2_sfi_req *)smb_work->buf;
	rsp = (struct smb_com_transaction2_sfi_rsp *)smb_work->rsp_buf;

	allocinfo =  (struct file_allocation_info *)
		(((char *) &req->hdr.Protocol) + le16_to_cpu(req->DataOffset));
	newsize = le64_to_cpu(allocinfo->AllocationSize);
	err = smb_vfs_getattr(smb_work->sess, (uint64_t)req->Fid, &stat);
	if (err) {
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_PARAMETER;
		return err;
	}

	if (newsize == stat.size) /* nothing to do */
		goto out;

	/* Round up size */
	if (alloc_roundup_size) {
		newsize = div64_u64(newsize + alloc_roundup_size - 1,
				alloc_roundup_size);
		newsize *= alloc_roundup_size;
	}

	err = smb_vfs_truncate(smb_work->sess, NULL, (uint64_t)req->Fid,
		newsize);
	if (err) {
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_PARAMETER;
		return err;
	}

out:
	cifsd_debug("fid %u, truncated to newsize %llu\n",
			req->Fid, newsize);

	rsp->hdr.Status.CifsError = NT_STATUS_OK;
	rsp->hdr.WordCount = 10;
	rsp->t2.TotalParameterCount = cpu_to_le16(2);
	rsp->t2.TotalDataCount = 0;
	rsp->t2.Reserved = 0;
	rsp->t2.ParameterCount = rsp->t2.TotalParameterCount;
	rsp->t2.ParameterOffset = cpu_to_le16(56);
	rsp->t2.ParameterDisplacement = 0;
	rsp->t2.DataCount = rsp->t2.TotalDataCount;
	rsp->t2.DataOffset = 0;
	rsp->t2.DataDisplacement = 0;
	rsp->t2.SetupCount = 0;
	rsp->t2.Reserved1 = 0;

	/* 3 pad (1 pad1 + 2 pad2)*/
	rsp->ByteCount = 3;
	rsp->Reserved2 = 0;
	inc_rfc1001_len(&rsp->hdr,
			(rsp->hdr.WordCount * 2 + rsp->ByteCount));

	return 0;
}

/**
 * smb_set_dispostion() - set file dispostion method using trans2
 *		using set file info command
 * @smb_work:	smb work containing set file info command buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb_set_dispostion(struct smb_work *smb_work)
{
	struct smb_com_transaction2_sfi_req *req;
	struct smb_com_transaction2_sfi_rsp *rsp;
	char *disp_info;
	struct cifsd_file *fp;

	req = (struct smb_com_transaction2_sfi_req *)smb_work->buf;
	rsp = (struct smb_com_transaction2_sfi_rsp *)smb_work->rsp_buf;
	disp_info =  (char *) (((char *) &req->hdr.Protocol)
			+ le16_to_cpu(req->DataOffset));

	fp = get_id_from_fidtable(smb_work->sess, req->Fid);
	if (!fp) {
		cifsd_debug("Invalid id for close: %d\n", req->Fid);
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_PARAMETER;
		return -EINVAL;
	}

	if (*disp_info) {
		if (!fp->is_nt_open) {
			rsp->hdr.Status.CifsError = NT_STATUS_ACCESS_DENIED;
			return -EPERM;
		}

		if (!(fp->filp->f_path.dentry->d_inode->i_mode & S_IWUGO)) {
			rsp->hdr.Status.CifsError = NT_STATUS_CANNOT_DELETE;
			return -EPERM;
		}

		if (S_ISDIR(fp->filp->f_path.dentry->d_inode->i_mode) &&
				!is_dir_empty(fp)) {
			rsp->hdr.Status.CifsError =
				NT_STATUS_DIRECTORY_NOT_EMPTY;
			return -ENOTEMPTY;
		}
		fp->f_mfp->m_flags |= S_DEL_ON_CLS;
	}

	rsp->hdr.Status.CifsError = NT_STATUS_OK;
	rsp->hdr.WordCount = 10;
	rsp->t2.TotalParameterCount = cpu_to_le16(2);
	rsp->t2.TotalDataCount = 0;
	rsp->t2.Reserved = 0;
	rsp->t2.ParameterCount = rsp->t2.TotalParameterCount;
	rsp->t2.ParameterOffset = cpu_to_le16(56);
	rsp->t2.ParameterDisplacement = 0;
	rsp->t2.DataCount = rsp->t2.TotalDataCount;
	rsp->t2.DataOffset = 0;
	rsp->t2.DataDisplacement = 0;
	rsp->t2.SetupCount = 0;
	rsp->t2.Reserved1 = 0;

	/* 3 pad (1 pad1 + 2 pad2)*/
	rsp->ByteCount = 3;
	rsp->Reserved2 = 0;
	inc_rfc1001_len(&rsp->hdr,
			(rsp->hdr.WordCount * 2 + rsp->ByteCount));

	return 0;
}

/**
 * smb_set_time_fileinfo() - set file time method using trans2
 *		using set file info command
 * @smb_work:	smb work containing set file info command buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb_set_time_fileinfo(struct smb_work *smb_work)
{
	struct smb_com_transaction2_sfi_req *req;
	struct smb_com_transaction2_sfi_rsp *rsp;
	FILE_BASIC_INFO *info;
	struct iattr attrs;
	int err = 0;

	req = (struct smb_com_transaction2_sfi_req *)smb_work->buf;
	rsp = (struct smb_com_transaction2_sfi_rsp *)smb_work->rsp_buf;

	info = (FILE_BASIC_INFO *)(((char *) &req->hdr.Protocol) +
			le16_to_cpu(req->DataOffset));

	attrs.ia_valid = 0;
	if (le64_to_cpu(info->LastAccessTime)) {
		attrs.ia_atime = smb_NTtimeToUnix(
					le64_to_cpu(info->LastAccessTime));
		attrs.ia_valid |= (ATTR_ATIME | ATTR_ATIME_SET);
	}

	if (le64_to_cpu(info->ChangeTime)) {
		attrs.ia_ctime = smb_NTtimeToUnix(
					le64_to_cpu(info->ChangeTime));
		attrs.ia_valid |= ATTR_CTIME;
	}

	if (le64_to_cpu(info->LastWriteTime)) {
		attrs.ia_mtime = smb_NTtimeToUnix(
					le64_to_cpu(info->LastWriteTime));
		attrs.ia_valid |= (ATTR_MTIME | ATTR_MTIME_SET);
	}
	/* TODO: check dos mode and acl bits if req->Attributes nonzero */

	if (!attrs.ia_valid)
		goto done;

	err = smb_vfs_setattr(smb_work->sess, NULL, (uint64_t)req->Fid, &attrs);
	if (err) {
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_PARAMETER;
		return err;
	}

done:
	cifsd_debug("fid %u, setattr done\n", req->Fid);
	rsp->hdr.Status.CifsError = NT_STATUS_OK;
	rsp->hdr.WordCount = 10;
	rsp->t2.TotalParameterCount = cpu_to_le16(2);
	rsp->t2.TotalDataCount = 0;
	rsp->t2.Reserved = 0;
	rsp->t2.ParameterCount = rsp->t2.TotalParameterCount;
	rsp->t2.ParameterOffset = cpu_to_le16(56);
	rsp->t2.ParameterDisplacement = 0;
	rsp->t2.DataCount = rsp->t2.TotalDataCount;
	rsp->t2.DataOffset = 0;
	rsp->t2.DataDisplacement = 0;
	rsp->t2.SetupCount = 0;
	rsp->t2.Reserved1 = 0;

	/* 3 pad (1 pad1 + 2 pad2)*/
	rsp->ByteCount = 3;
	rsp->Reserved2 = 0;
	inc_rfc1001_len(&rsp->hdr,
			(rsp->hdr.WordCount * 2 + rsp->ByteCount));

	return 0;
}

/**
 * query_file_info_pipe() - query file info of IPC pipe
 *		using query file info command
 * @smb_work:	smb work containing query file info command buffer
 *
 * Return:	0 on success, otherwise error
 */
int query_file_info_pipe(struct smb_work *smb_work)
{
	struct smb_hdr *rsp_hdr = (struct smb_hdr *)smb_work->rsp_buf;
	TRANSACTION2_RSP *rsp = (TRANSACTION2_RSP *)smb_work->rsp_buf;
	struct smb_trans2_req *req = (struct smb_trans2_req *)smb_work->buf;
	TRANSACTION2_QFI_REQ_PARAMS *req_params;
	FILE_STANDARD_INFO *standard_info;
	struct cifsd_pipe *pipe_desc;
	char *ptr;
	int id;

	req_params = (TRANSACTION2_QFI_REQ_PARAMS *)(smb_work->buf +
			req->ParameterOffset + 4);

	if (req_params->InformationLevel != SMB_QUERY_FILE_STANDARD_INFO) {
		cifsd_err("query file info for info %u not supported\n",
				req_params->InformationLevel);
		rsp_hdr->Status.CifsError = NT_STATUS_NOT_SUPPORTED;
		return -EOPNOTSUPP;
	}

	id = cpu_to_le16(req_params->Fid);
	pipe_desc = get_pipe_desc(smb_work->sess, id);

	/* Windows can sometime send query file info request on
	   pipe without opening it, checking error condition here */
	if (!pipe_desc) {
		cifsd_debug("Pipe not opened or invalid in Pipe id\n");
		if (pipe_desc)
			cifsd_debug("Incoming id = %d opened pipe id = %d\n",
					id, pipe_desc->id);
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_HANDLE;
		return 0;
	}

	cifsd_debug("SMB_QUERY_FILE_STANDARD_INFO\n");
	rsp_hdr->WordCount = 10;
	rsp->t2.TotalParameterCount = 2;
	rsp->t2.TotalDataCount = sizeof(FILE_STANDARD_INFO);
	rsp->t2.Reserved = 0;
	rsp->t2.ParameterCount = 2;
	rsp->t2.ParameterOffset = 56;
	rsp->t2.ParameterDisplacement = 0;
	rsp->t2.DataCount = sizeof(FILE_STANDARD_INFO);
	rsp->t2.DataOffset = 60;
	rsp->t2.DataDisplacement = 0;
	rsp->t2.SetupCount = 0;
	rsp->t2.Reserved1 = 0;
	/*2 for paramater count & 3 pad (1pad1 + 2 pad2)*/
	rsp->ByteCount = 2 + sizeof(FILE_STANDARD_INFO) + 3;
	rsp->Pad = 0;
	/* lets set EA info */
	ptr = (char *)&rsp->Pad + 1;
	memset(ptr, 0, 4);
	standard_info = (FILE_STANDARD_INFO *)(ptr + 4);
	standard_info->AllocationSize = 4096;
	standard_info->EndOfFile = 0;
	standard_info->NumberOfLinks = 1;
	standard_info->DeletePending = 0;
	standard_info->Directory = 0;
	standard_info->DeletePending = 1;
	inc_rfc1001_len(rsp_hdr, (10 * 2 + rsp->ByteCount));

	return 0;
}

/**
 * query_file_info() - query file info of file/dir
 *		using query file info command
 * @smb_work:	smb work containing query file info command buffer
 *
 * Return:	0 on success, otherwise error
 */
int query_file_info(struct smb_work *smb_work)
{
	struct smb_hdr *req_hdr = (struct smb_hdr *)smb_work->buf;
	struct smb_hdr *rsp_hdr = (struct smb_hdr *)smb_work->rsp_buf;
	struct smb_trans2_req *req = (struct smb_trans2_req *)smb_work->buf;
	TRANSACTION2_RSP *rsp = (TRANSACTION2_RSP *)smb_work->rsp_buf;
	TRANSACTION2_QFI_REQ_PARAMS *req_params;
	struct cifsd_file *fp;
	struct kstat st;
	struct file *filp;
	FILE_STANDARD_INFO *standard_info;
	FILE_BASIC_INFO *basic_info;
	FILE_EA_INFO *ea_info;
	FILE_UNIX_BASIC_INFO *uinfo;
	FILE_ALL_INFO *ainfo;
	__u16 fid;
	char *ptr;
	int rc = 0;

	req_params = (TRANSACTION2_QFI_REQ_PARAMS *)(smb_work->buf +
			req->ParameterOffset + 4);

	if (req_hdr->WordCount != 15) {
		cifsd_err("word count mismatch: expected 15 got %d\n",
				req_hdr->WordCount);
		rsp_hdr->Status.CifsError = NT_STATUS_INVALID_PARAMETER;
		rc = -EINVAL;
		goto err_out;
	}

	if (smb_work->tcon->share->is_pipe == true) {
		cifsd_debug("query file info for IPC srvsvc\n");
		return query_file_info_pipe(smb_work);
	}

	fid = cpu_to_le16(req_params->Fid);
	fp = get_id_from_fidtable(smb_work->sess, fid);
	if (!fp) {
		cifsd_err("failed to get filp for fid %u\n", fid);
		rsp_hdr->Status.CifsError = NT_STATUS_UNEXPECTED_IO_ERROR;
		rc = -EIO;
		goto err_out;
	} else
		filp = fp->filp;

	generic_fillattr(filp->f_path.dentry->d_inode, &st);

	switch (req_params->InformationLevel) {

	case SMB_QUERY_FILE_STANDARD_INFO:
		cifsd_debug("SMB_QUERY_FILE_STANDARD_INFO\n");
		rsp_hdr->WordCount = 10;
		rsp->t2.TotalParameterCount = 2;
		rsp->t2.TotalDataCount = sizeof(FILE_STANDARD_INFO);
		rsp->t2.Reserved = 0;
		rsp->t2.ParameterCount = 2;
		rsp->t2.ParameterOffset = 56;
		rsp->t2.ParameterDisplacement = 0;
		rsp->t2.DataCount = sizeof(FILE_STANDARD_INFO);
		rsp->t2.DataOffset = 60;
		rsp->t2.DataDisplacement = 0;
		rsp->t2.SetupCount = 0;
		rsp->t2.Reserved1 = 0;
		/*2 for paramater count & 3 pad (1pad1 + 2 pad2)*/
		rsp->ByteCount = 2 + sizeof(FILE_STANDARD_INFO) + 3;
		rsp->Pad = 0;
		/* lets set EA info */
		ptr = (char *)&rsp->Pad + 1;
		memset(ptr, 0, 4);
		standard_info = (FILE_STANDARD_INFO *)(ptr + 4);
		standard_info->AllocationSize = cpu_to_le64(st.blocks << 9);
		standard_info->EndOfFile = cpu_to_le64(st.size);
		standard_info->NumberOfLinks = cpu_to_le32(st.nlink);
		standard_info->DeletePending = 0;
		standard_info->Directory = S_ISDIR(st.mode) ? 1 : 0;
		inc_rfc1001_len(rsp_hdr, (10 * 2 + rsp->ByteCount));
		break;
	case SMB_QUERY_FILE_BASIC_INFO:
		cifsd_debug("SMB_QUERY_FILE_BASIC_INFO\n");
		rsp_hdr->WordCount = 10;
		rsp->t2.TotalParameterCount = 2;
		rsp->t2.TotalDataCount = sizeof(FILE_BASIC_INFO);
		rsp->t2.Reserved = 0;
		rsp->t2.ParameterCount = 2;
		rsp->t2.ParameterOffset = 56;
		rsp->t2.ParameterDisplacement = 0;
		rsp->t2.DataCount = sizeof(FILE_BASIC_INFO);
		rsp->t2.DataOffset = 60;
		rsp->t2.DataDisplacement = 0;
		rsp->t2.SetupCount = 0;
		rsp->t2.Reserved1 = 0;
		/*2 for paramater count & 3 pad (1pad1 + 2 pad2)*/
		rsp->ByteCount = 2 + sizeof(FILE_BASIC_INFO) + 3;
		rsp->Pad = 0;
		/* lets set EA info */
		ptr = (char *)&rsp->Pad + 1;
		memset(ptr, 0, 4);
		basic_info = (FILE_BASIC_INFO *)(ptr + 4);
		basic_info->CreationTime =
			cpu_to_le64(cifs_UnixTimeToNT(st.ctime));
		basic_info->LastAccessTime =
			cpu_to_le64(cifs_UnixTimeToNT(st.atime));
		basic_info->LastWriteTime =
			cpu_to_le64(cifs_UnixTimeToNT(st.mtime));
		basic_info->ChangeTime =
			cpu_to_le64(cifs_UnixTimeToNT(st.mtime));
		basic_info->Attributes = S_ISDIR(st.mode) ?
			ATTR_DIRECTORY : ATTR_NORMAL;
		basic_info->Pad = 0;
		inc_rfc1001_len(rsp_hdr, (10 * 2 + rsp->ByteCount));
		break;

	case SMB_QUERY_FILE_EA_INFO:
		cifsd_debug("SMB_QUERY_FILE_EA_INFO\n");
		rsp_hdr->WordCount = 10;
		rsp->t2.TotalParameterCount = 2;
		rsp->t2.TotalDataCount = sizeof(FILE_EA_INFO);
		rsp->t2.Reserved = 0;
		rsp->t2.ParameterCount = 2;
		rsp->t2.ParameterOffset = 56;
		rsp->t2.ParameterDisplacement = 0;
		rsp->t2.DataCount = sizeof(FILE_EA_INFO);
		rsp->t2.DataOffset = 60;
		rsp->t2.DataDisplacement = 0;
		rsp->t2.SetupCount = 0;
		rsp->t2.Reserved1 = 0;
		/*2 for paramater count & 3 pad (1pad1 + 2 pad2)*/
		rsp->ByteCount = 2 + sizeof(FILE_EA_INFO) + 3;
		rsp->Pad = 0;
		/* lets set EA info */
		ptr = (char *)&rsp->Pad + 1;
		memset(ptr, 0, 4);
		ea_info = (FILE_EA_INFO *)(ptr + 4);
		ea_info->EaSize = 0;
		inc_rfc1001_len(rsp_hdr, (10 * 2 + rsp->ByteCount));
		break;
	case SMB_QUERY_FILE_UNIX_BASIC:
		cifsd_debug("SMB_QUERY_FILE_UNIX_BASIC\n");
		rsp_hdr->WordCount = 10;
		rsp->t2.TotalParameterCount = 2;
		rsp->t2.TotalDataCount = sizeof(FILE_UNIX_BASIC_INFO);
		rsp->t2.Reserved = 0;
		rsp->t2.ParameterCount = 2;
		rsp->t2.ParameterOffset = 56;
		rsp->t2.ParameterDisplacement = 0;
		rsp->t2.DataCount = sizeof(FILE_UNIX_BASIC_INFO);
		rsp->t2.DataOffset = 60;
		rsp->t2.DataDisplacement = 0;
		rsp->t2.SetupCount = 0;
		rsp->t2.Reserved1 = 0;
		/*2 for paramater count & 3 pad (1pad1 + 2 pad2)*/
		rsp->ByteCount = 2 + sizeof(FILE_UNIX_BASIC_INFO) + 3;
		rsp->Pad = 0;
		/* lets set unix info info */
		ptr = (char *)&rsp->Pad + 1;
		memset(ptr, 0, 4);
		uinfo = (FILE_UNIX_BASIC_INFO *)(ptr + 4);
		init_unix_info(uinfo, &st);
		inc_rfc1001_len(rsp_hdr, (10 * 2 + rsp->ByteCount));
		break;
	case SMB_QUERY_FILE_ALL_INFO:
		cifsd_debug("SMB_QUERY_FILE_UNIX_BASIC\n");
		rsp_hdr->WordCount = 10;
		rsp->t2.TotalParameterCount = 2;
		rsp->t2.TotalDataCount = sizeof(FILE_ALL_INFO);
		rsp->t2.Reserved = 0;
		rsp->t2.ParameterCount = 2;
		rsp->t2.ParameterOffset = 56;
		rsp->t2.ParameterDisplacement = 0;
		rsp->t2.DataCount = sizeof(FILE_ALL_INFO);
		rsp->t2.DataOffset = 60;
		rsp->t2.DataDisplacement = 0;
		rsp->t2.SetupCount = 0;
		rsp->t2.Reserved1 = 0;
		/*2 for paramater count & 3 pad (1pad1 + 2 pad2)*/
		rsp->ByteCount = 2 + sizeof(FILE_ALL_INFO) + 3;
		rsp->Pad = 0;
		/* lets set all info info */
		ptr = (char *)&rsp->Pad + 1;
		memset(ptr, 0, 4);
		ainfo = (FILE_ALL_INFO *)(ptr + 4);
		ainfo->CreationTime = cpu_to_le64(cifs_UnixTimeToNT(st.ctime));
		ainfo->LastAccessTime =
			cpu_to_le64(cifs_UnixTimeToNT(st.atime));
		ainfo->LastWriteTime = cpu_to_le64(cifs_UnixTimeToNT(st.mtime));
		ainfo->ChangeTime = cpu_to_le64(cifs_UnixTimeToNT(st.mtime));
		ainfo->Attributes = cpu_to_le32(S_ISDIR(st.mode) ?
				ATTR_DIRECTORY : ATTR_NORMAL);
		ainfo->Pad1 = 0;
		ainfo->AllocationSize = cpu_to_le64(st.blocks << 9);
		ainfo->EndOfFile = cpu_to_le64(st.size);
		ainfo->NumberOfLinks = cpu_to_le32(st.nlink);
		ainfo->DeletePending = 0;
		ainfo->Directory = S_ISDIR(st.mode) ? 1 : 0;
		ainfo->Pad2 = 0;
		ainfo->EASize = 0;
		ainfo->FileNameLength = 0;
		inc_rfc1001_len(rsp_hdr, (10 * 2 + rsp->ByteCount));
		break;
	default:
		cifsd_err("query path info not implemnted for %x\n",
				req_params->InformationLevel);
		rsp_hdr->Status.CifsError = NT_STATUS_NOT_SUPPORTED;
		rc = -EINVAL;
		goto err_out;

	}

err_out:
	return rc;
}

/**
 * smb_fileinfo_rename() - rename method using trans2 set file info command
 * @smb_work:	smb work containing set file info command buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb_fileinfo_rename(struct smb_work *smb_work)
{
	struct smb_com_transaction2_sfi_req *req;
	struct smb_com_transaction2_sfi_rsp *rsp;
	struct set_file_rename *info;
	char *newname;
	int rc = 0;

	req = (struct smb_com_transaction2_sfi_req *)smb_work->buf;
	rsp = (struct smb_com_transaction2_sfi_rsp *)smb_work->rsp_buf;
	info =  (struct set_file_rename *)
		(((char *) &req->hdr.Protocol) + le16_to_cpu(req->DataOffset));

	if (le32_to_cpu(info->overwrite)) {
		rc = smb_vfs_truncate(smb_work->sess, NULL, (uint64_t)req->Fid,
			0);
		if (rc) {
			rsp->hdr.Status.CifsError = NT_STATUS_INVALID_PARAMETER;
			return rc;
		}
	}

	newname = smb_strndup_from_utf16(info->target_name,
			le32_to_cpu(info->target_name_len), true,
			smb_work->conn->local_nls);
	if (IS_ERR(newname)) {
		rsp->hdr.Status.CifsError = NT_STATUS_NO_MEMORY;
		return PTR_ERR(newname);
	}

	cifsd_debug("rename fid %u -> %s\n", req->Fid, newname);
	rc = smb_vfs_rename(smb_work->sess, NULL, newname, (uint64_t)req->Fid);
	if (rc) {
		rsp->hdr.Status.CifsError = NT_STATUS_UNEXPECTED_IO_ERROR;
		goto out;
	}

	rsp->hdr.WordCount = 10;
	rsp->t2.TotalParameterCount = cpu_to_le16(2);
	rsp->t2.TotalDataCount = 0;
	rsp->t2.Reserved = 0;
	rsp->t2.ParameterCount = rsp->t2.TotalParameterCount;
	rsp->t2.ParameterOffset = cpu_to_le16(56);
	rsp->t2.ParameterDisplacement = 0;
	rsp->t2.DataCount = rsp->t2.TotalDataCount;
	rsp->t2.DataOffset = 0;
	rsp->t2.DataDisplacement = 0;
	rsp->t2.SetupCount = 0;
	rsp->t2.Reserved1 = 0;

	/* 3 pad (1 pad1 + 2 pad2)*/
	rsp->ByteCount = 3;
	rsp->Reserved2 = 0;
	inc_rfc1001_len(&rsp->hdr,
			(rsp->hdr.WordCount * 2 + rsp->ByteCount));

out:
	kfree(newname);
	return rc;
}

/**
 * set_file_info() - trans2 set file info command dispatcher
 * @smb_work:	smb work containing set file info command buffer
 *
 * Return:	0 on success, otherwise error
 */
int set_file_info(struct smb_work *smb_work)
{
	struct smb_com_transaction2_sfi_req *req;
	struct smb_com_transaction2_sfi_rsp *rsp;
	__u16 info_level, total_param;
	int err = 0;

	req = (struct smb_com_transaction2_sfi_req *)smb_work->buf;
	rsp = (struct smb_com_transaction2_sfi_rsp *)smb_work->rsp_buf;
	info_level = le16_to_cpu(req->InformationLevel);
	total_param = le16_to_cpu(req->TotalParameterCount);
	if (total_param < 4) {
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_PARAMETER;
		cifsd_err("invalid total parameter for info_level 0x%x\n",
				total_param);
		return -EINVAL;
	}

	if (req->hdr.WordCount != 15) {
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_PARAMETER;
		cifsd_err("word count mismatch: expected 15 got %d\n",
				req->hdr.WordCount);
		return -EINVAL;
	}

	switch (info_level) {
	case SMB_SET_FILE_ALLOCATION_INFO2:
		/* fall through */
	case SMB_SET_FILE_ALLOCATION_INFO:
		err = smb_set_alloc_size(smb_work);
		break;
	case SMB_SET_FILE_END_OF_FILE_INFO2:
		/* fall through */
	case SMB_SET_FILE_END_OF_FILE_INFO:
		err = smb_set_file_size_finfo(smb_work);
		break;
	case SMB_SET_FILE_UNIX_BASIC:
		err = smb_set_unix_fileinfo(smb_work);
		break;
	case SMB_SET_FILE_DISPOSITION_INFO:
		err = smb_set_dispostion(smb_work);
		break;
	case SMB_SET_FILE_BASIC_INFO2:
		/* fall through */
	case SMB_SET_FILE_BASIC_INFO:
		err = smb_set_time_fileinfo(smb_work);
		break;
	case SMB_SET_FILE_RENAME_INFORMATION:
		err = smb_fileinfo_rename(smb_work);
		break;
	default:
		cifsd_err("info level = %x not implemented yet\n",
				info_level);
		rsp->hdr.Status.CifsError = NT_STATUS_NOT_IMPLEMENTED;
		return -ENOSYS;
	}

	if (err < 0)
		cifsd_debug("info_level 0x%x failed, err %d\n",
				info_level, err);
	return err;
}

/**
 * create_dir() - trans2 create directory dispatcher
 * @smb_work:   smb work containing set file info command buffer
 *
 * Return:      0 on success, otherwise error
 */
int create_dir(struct smb_work *smb_work)
{
	struct smb_trans2_req *req = (struct smb_trans2_req *)smb_work->buf;
	TRANSACTION2_RSP *rsp = (TRANSACTION2_RSP *)smb_work->rsp_buf;
	mode_t mode = S_IALLUGO;
	char *name;
	int err;

	/* WordCount should be 15 as per request format */
	if (req->hdr.WordCount != 15) {
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_PARAMETER;
		cifsd_err("word count mismatch: expected 15 got %d\n",
				req->hdr.WordCount);
		return -EINVAL;
	}

	name = smb_get_name(smb_work->buf + req->ParameterOffset + 4,
			PATH_MAX, smb_work, false);
	if (IS_ERR(name))
		return PTR_ERR(name);

	err = smb_vfs_mkdir(name, mode);
	if (err) {
		if (err == -EEXIST) {
			if (!(((struct smb_hdr *)smb_work->buf)->Flags2 &
						SMBFLG2_ERR_STATUS)) {
				ntstatus_to_dos(NT_STATUS_OBJECT_NAME_COLLISION,
					&rsp->hdr.Status.DosError.ErrorClass,
					&rsp->hdr.Status.DosError.Error);
			} else
				rsp->hdr.Status.CifsError =
					NT_STATUS_OBJECT_NAME_COLLISION;
		} else
			rsp->hdr.Status.CifsError = NT_STATUS_DATA_ERROR;
	} else
		rsp->hdr.Status.CifsError = NT_STATUS_OK;

	memset(&rsp->hdr.WordCount, 0, 3);
	smb_put_name(name);
	return err;
}

/**
 * get_dfs_referral() - handler for smb dfs referral command
 * @smb_work:	smb work containing get dfs referral command buffer
 *
 * Return:	0 on success, otherwise error
 */
int get_dfs_referral(struct smb_work *smb_work)
{
	struct smb_hdr *rsp_hdr = (struct smb_hdr *)smb_work->rsp_buf;

	rsp_hdr->Status.CifsError = NT_STATUS_NOT_SUPPORTED;
	return 0;
}

/**
 * smb_mkdir() - handler for smb mkdir
 * @smb_work:	smb work containing creat directory command buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb_mkdir(struct smb_work *smb_work)
{
	CREATE_DIRECTORY_REQ *req = (CREATE_DIRECTORY_REQ *)smb_work->buf;
	CREATE_DIRECTORY_RSP *rsp = (CREATE_DIRECTORY_RSP *)smb_work->rsp_buf;
	mode_t mode = S_IALLUGO;
	char *name;
	int err;

	name = smb_get_name(req->DirName, PATH_MAX, smb_work, false);
	if (IS_ERR(name))
		return PTR_ERR(name);

	err = smb_vfs_mkdir(name, mode);
	if (err) {
		if (err == -EEXIST) {
			if (!(((struct smb_hdr *)smb_work->buf)->Flags2 &
						SMBFLG2_ERR_STATUS)) {
				rsp->hdr.Status.DosError.ErrorClass = ERRDOS;
				rsp->hdr.Status.DosError.Error = ERRnoaccess;
			} else
				rsp->hdr.Status.CifsError =
					NT_STATUS_OBJECT_NAME_COLLISION;
		} else
			rsp->hdr.Status.CifsError = NT_STATUS_DATA_ERROR;
	} else {
		/* mkdir success, return response to server */
		rsp->hdr.Status.CifsError = NT_STATUS_OK;
		rsp->hdr.WordCount = 0;
		rsp->ByteCount = 0;
	}

	smb_put_name(name);
	return err;
}

/**
 * smb_checkdir() - handler to verify whether a specified
 * path resolves to a valid directory or not
 *
 * @smb_work:   smb work containing creat directory command buffer
 *
 * Return:      0 on success, otherwise error
 */
int smb_checkdir(struct smb_work *smb_work)
{
	CHECK_DIRECTORY_REQ *req = (CHECK_DIRECTORY_REQ *)smb_work->buf;
	CHECK_DIRECTORY_RSP *rsp = (CHECK_DIRECTORY_RSP *)smb_work->rsp_buf;
	struct path path;
	struct kstat stat;
	char *name, *last;
	int err;
	bool caseless_lookup = req->hdr.Flags & SMBFLG_CASELESS;

	name = smb_get_name(req->DirName, PATH_MAX, smb_work, false);
	if (IS_ERR(name))
		return PTR_ERR(name);

	err = smb_kern_path(name, 0, &path, caseless_lookup);
	if (err) {
		if (err == -ENOENT) {
			/*
			 * If the parent directory is valid but not the
			 * last component - then returns
			 * NT_STATUS_OBJECT_NAME_NOT_FOUND
			 * for that case and NT_STATUS_OBJECT_PATH_NOT_FOUND
			 * if the path is invalid.
			 */
			last = strrchr(name, '/');
			if (last && last[1] != '\0') {
				*last = '\0';
				last++;

				err = smb_kern_path(name, LOOKUP_FOLLOW |
						LOOKUP_DIRECTORY, &path,
						caseless_lookup);
			} else {
				cifsd_debug("can't lookup parent %s\n", name);
				err = -ENOENT;
			}
		}
		if (err) {
			cifsd_debug("look up failed err %d\n", err);
			switch (err) {
			case -ENOENT:
				rsp->hdr.Status.CifsError =
				NT_STATUS_OBJECT_NAME_NOT_FOUND;
				break;
			case -ENOMEM:
				rsp->hdr.Status.CifsError =
				NT_STATUS_INSUFFICIENT_RESOURCES;
				break;
			case -EACCES:
				rsp->hdr.Status.CifsError =
				NT_STATUS_ACCESS_DENIED;
				break;
			case -EIO:
				rsp->hdr.Status.CifsError =
				NT_STATUS_DATA_ERROR;
				break;
			default:
				rsp->hdr.Status.CifsError =
				NT_STATUS_OBJECT_PATH_SYNTAX_BAD;
				break;
			}
			smb_put_name(name);
			return err;
		}
	}

	generic_fillattr(path.dentry->d_inode, &stat);

	if (!S_ISDIR(stat.mode)) {
		rsp->hdr.Status.CifsError = NT_STATUS_NOT_A_DIRECTORY;
	} else {
		/* checkdir success, return response to server */
		rsp->hdr.Status.CifsError = NT_STATUS_OK;
		rsp->hdr.WordCount = 0;
		rsp->ByteCount = 0;
	}

	path_put(&path);
	smb_put_name(name);
	return err;
}

/**
 * smb_process_exit() - handler for smb process exit
 * @smb_work:	smb work containing process exit command buffer
 *
 * Return:	0 on success always
 * This command is obsolete now. Starting with the LAN Manager 1.0 dialect,
 * FIDs are no longer associated with PIDs.CIFS clients SHOULD NOT send
 * SMB_COM_PROCESS_EXIT requests. Instead, CIFS clients SHOULD perform all
 * process cleanup operations, sending individual file close operations
 * as needed.Here it is implemented very minimally for sake
 * of passing smbtorture testcases.
 */
int smb_process_exit(struct smb_work *smb_work)
{
	PROCESS_EXIT_RSP *rsp = (PROCESS_EXIT_RSP *)smb_work->rsp_buf;

	rsp->hdr.Status.CifsError = NT_STATUS_OK;
	rsp->hdr.WordCount = 0;
	rsp->ByteCount = 0;
	return 0;
}

/**
 * smb_rmdir() - handler for smb rmdir
 * @smb_work:	smb work containing delete directory command buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb_rmdir(struct smb_work *smb_work)
{
	DELETE_DIRECTORY_REQ *req = (DELETE_DIRECTORY_REQ *)smb_work->buf;
	DELETE_DIRECTORY_RSP *rsp = (DELETE_DIRECTORY_RSP *)smb_work->rsp_buf;
	char *name;
	int err;

	name = smb_get_name(req->DirName, PATH_MAX, smb_work, false);
	if (IS_ERR(name))
		return PTR_ERR(name);

	err = smb_vfs_remove_file(name);
	if (err) {
		if (err == -ENOTEMPTY)
			rsp->hdr.Status.CifsError =
					NT_STATUS_DIRECTORY_NOT_EMPTY;
		else
			rsp->hdr.Status.CifsError = NT_STATUS_DATA_ERROR;
	} else {
		/* rmdir success, return response to server */
		rsp->hdr.Status.CifsError = NT_STATUS_OK;
		rsp->hdr.WordCount = 0;
		rsp->ByteCount = 0;
	}

	smb_put_name(name);
	return err;
}

/**
 * smb_unlink() - handler for smb delete file
 * @smb_work:	smb work containing delete file command buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb_unlink(struct smb_work *smb_work)
{
	DELETE_FILE_REQ *req = (DELETE_FILE_REQ *)smb_work->buf;
	DELETE_FILE_RSP *rsp = (DELETE_FILE_RSP *)smb_work->rsp_buf;
	char *name;
	int err;

	name = smb_get_name(req->fileName, PATH_MAX, smb_work, false);
	if (IS_ERR(name))
		return PTR_ERR(name);

	err = smb_vfs_remove_file(name);
	if (err) {
		if (err == -EISDIR)
			rsp->hdr.Status.CifsError =
				NT_STATUS_FILE_IS_A_DIRECTORY;
		else
			rsp->hdr.Status.CifsError =
				NT_STATUS_OBJECT_NAME_NOT_FOUND;
	} else {
		rsp->hdr.Status.CifsError = NT_STATUS_OK;
		rsp->hdr.WordCount = 0;
		rsp->ByteCount = 0;
	}

	smb_put_name(name);
	return err;
}

/**
 * smb_nt_cancel() - handler for smb cancel command
 * @smb_work:	smb work containing cancel command buffer
 *
 * Return:	0
 */
int smb_nt_cancel(struct smb_work *smb_work)
{
	struct connection *conn = smb_work->conn;
	struct smb_hdr *hdr = (struct smb_hdr *)smb_work->buf;
	struct smb_hdr *work_hdr;
	struct smb_work *work;
	struct list_head *tmp;

	cifsd_debug("smb cancel called on mid %u\n", hdr->Mid);

	spin_lock(&conn->request_lock);
	list_for_each(tmp, &conn->requests) {
		work = list_entry(tmp, struct smb_work, request_entry);
		work_hdr = (struct smb_hdr *)work->buf;
		if (work_hdr->Mid == hdr->Mid) {
			cifsd_debug("smb with mid %u cancelled command = 0x%x\n",
			       hdr->Mid, work_hdr->Command);
			work->send_no_response = 1;
			list_del_init(&work->request_entry);
			work->added_in_request_list = 0;
			smb_work->sess->sequence_number--;
			break;
		}
	}
	spin_unlock(&conn->request_lock);

	/* For SMB_COM_NT_CANCEL command itself send no response */
	smb_work->send_no_response = 1;
	return 0;

}

/**
 * smb_nt_rename() - handler for smb rename command
 * @smb_work:	smb work containing nt rename command buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb_nt_rename(struct smb_work *smb_work)
{
	NT_RENAME_REQ *req = (NT_RENAME_REQ *)smb_work->buf;
	RENAME_RSP *rsp = (RENAME_RSP *)smb_work->rsp_buf;
	char *oldname, *newname;
	int oldname_len, err;

	if (le16_to_cpu(req->Flags) != CREATE_HARD_LINK) {
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_PARAMETER;
		return -EINVAL;
	}

	oldname = smb_get_name(req->OldFileName, PATH_MAX, smb_work, false);
	if (IS_ERR(oldname))
		return PTR_ERR(oldname);

	if (is_smbreq_unicode(&req->hdr)) {
		oldname_len = smb_utf16_bytes((__le16 *)req->OldFileName,
				PATH_MAX, smb_work->conn->local_nls);
		oldname_len += nls_nullsize(smb_work->conn->local_nls);
		oldname_len *= 2;
	} else {
		oldname_len = strlen(oldname);
		oldname_len++;
	}

	newname = smb_get_name(&req->OldFileName[oldname_len + 2],
			PATH_MAX, smb_work, false);
	if (IS_ERR(newname)) {
		smb_put_name(oldname);
		return PTR_ERR(newname);
	}
	cifsd_debug("oldname %s, newname %s, oldname_len %d, unicode %d\n",
			oldname, newname, oldname_len,
			is_smbreq_unicode(&req->hdr));

	err = smb_vfs_link(oldname, newname);
	if (err < 0)
		rsp->hdr.Status.CifsError = NT_STATUS_NOT_SAME_DEVICE;

	smb_put_name(newname);
	smb_put_name(oldname);
	return err;
}

/**
 * smb_creat_hardlink() - handler for creating hardlink
 * @smb_work:	smb work containing set path info command buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb_creat_hardlink(struct smb_work *smb_work)
{
	TRANSACTION2_SPI_REQ *req = (TRANSACTION2_SPI_REQ *)smb_work->buf;
	TRANSACTION2_RSP *rsp = (TRANSACTION2_RSP *)smb_work->rsp_buf;
	char *oldname, *newname, *oldname_offset;
	int err;

	newname = smb_get_name(req->FileName, PATH_MAX, smb_work, false);
	if (IS_ERR(newname))
		return PTR_ERR(newname);

	oldname_offset = ((char *)&req->hdr.Protocol) +
				le16_to_cpu(req->DataOffset);
	oldname = smb_get_name(oldname_offset, PATH_MAX, smb_work, false);
	if (IS_ERR(oldname)) {
		err = PTR_ERR(oldname);
		goto out;
	}
	cifsd_debug("oldname %s, newname %s\n", oldname, newname);

	err = smb_vfs_link(oldname, newname);
	if (err < 0)
		rsp->hdr.Status.CifsError = NT_STATUS_NOT_SAME_DEVICE;

	rsp->hdr.Status.CifsError = NT_STATUS_OK;
	rsp->hdr.WordCount = 10;
	rsp->t2.TotalParameterCount = cpu_to_le16(2);
	rsp->t2.TotalDataCount = 0;
	rsp->t2.Reserved = 0;
	rsp->t2.ParameterCount = rsp->t2.TotalParameterCount;
	rsp->t2.ParameterOffset = cpu_to_le16(56);
	rsp->t2.ParameterDisplacement = 0;
	rsp->t2.DataCount = 0;
	rsp->t2.DataOffset = 0;
	rsp->t2.DataDisplacement = 0;
	rsp->t2.SetupCount = 0;
	rsp->t2.Reserved1 = 0;
	rsp->ByteCount = 3;
	inc_rfc1001_len(&rsp->hdr,
			(rsp->hdr.WordCount * 2 + rsp->ByteCount));
out:
	smb_put_name(newname);
	smb_put_name(oldname);
	return err;
}

/**
 * smb_creat_symlink() - handler for creating symlink
 * @smb_work:	smb work containing set path info command buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb_creat_symlink(struct smb_work *smb_work)
{
	TRANSACTION2_SPI_REQ *req = (TRANSACTION2_SPI_REQ *)smb_work->buf;
	TRANSACTION2_SPI_RSP *rsp = (TRANSACTION2_SPI_RSP *)smb_work->rsp_buf;
	char *name, *symname, *name_offset;
	bool is_unicode = is_smbreq_unicode(&req->hdr);
	int err;

	symname = smb_get_name(req->FileName, PATH_MAX, smb_work, false);
	if (IS_ERR(symname))
		return PTR_ERR(symname);

	name_offset = ((char *)&req->hdr.Protocol) +
		le16_to_cpu(req->DataOffset);
	name = smb_strndup_from_utf16(name_offset, PATH_MAX, is_unicode,
			smb_work->conn->local_nls);
	if (IS_ERR(name)) {
		smb_put_name(symname);
		rsp->hdr.Status.CifsError = NT_STATUS_NO_MEMORY;
		return PTR_ERR(name);
	}
	cifsd_debug("name %s, symname %s\n", name, symname);

	err = smb_vfs_symlink(name, symname);
	if (err < 0) {
		if (err == -ENOSPC)
			rsp->hdr.Status.CifsError = NT_STATUS_DISK_FULL;
		else if (err == -EEXIST)
			rsp->hdr.Status.CifsError =
				NT_STATUS_OBJECT_NAME_COLLISION;
		else
			rsp->hdr.Status.CifsError = NT_STATUS_NOT_SAME_DEVICE;
	} else
		rsp->hdr.Status.CifsError = NT_STATUS_OK;

	rsp->hdr.WordCount = 10;
	rsp->t2.TotalParameterCount = cpu_to_le16(2);
	rsp->t2.TotalDataCount = 0;
	rsp->t2.Reserved = 0;
	rsp->t2.ParameterCount = rsp->t2.TotalParameterCount;
	rsp->t2.ParameterOffset = cpu_to_le16(56);
	rsp->t2.ParameterDisplacement = 0;
	rsp->t2.DataCount = 0;
	rsp->t2.DataOffset = 0;
	rsp->t2.DataDisplacement = 0;
	rsp->t2.SetupCount = 0;
	rsp->t2.Reserved1 = 0;
	rsp->ByteCount = 3;
	inc_rfc1001_len(&rsp->hdr,
			(rsp->hdr.WordCount * 2 + rsp->ByteCount));
	kfree(name);
	smb_put_name(symname);
	return err;
}

/**
 * smb_query_info() - handler for query information command
 * @smb_work:	smb work containing query info command buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb_query_info(struct smb_work *smb_work)
{
	QUERY_INFORMATION_REQ *req = (QUERY_INFORMATION_REQ *)smb_work->buf;
	QUERY_INFORMATION_RSP *rsp = (QUERY_INFORMATION_RSP *)smb_work->rsp_buf;
	struct path path;
	struct kstat st;
	char *name;
	__u16 attr = 0;
	int err, i;

	name = smb_get_name(req->FileName, PATH_MAX, smb_work, false);
	if (IS_ERR(name))
		return PTR_ERR(name);

	err = smb_kern_path(name, LOOKUP_FOLLOW, &path, 0);
	if (err) {
		cifsd_err("look up failed err %d\n", err);
		rsp->hdr.Status.CifsError = NT_STATUS_OBJECT_NAME_NOT_FOUND;
		goto out;
	}
	generic_fillattr(path.dentry->d_inode, &st);

	rsp->hdr.Status.CifsError = NT_STATUS_OK;
	rsp->hdr.WordCount = 10;

	if (st.mode & S_ISVTX)
		attr |=  (ATTR_HIDDEN | ATTR_SYSTEM);
	if (!(st.mode & S_IWUGO))
		attr |=  ATTR_READONLY;
	if (S_ISDIR(st.mode))
		attr |= ATTR_DIRECTORY;

	rsp->attr = cpu_to_le16(attr);
	rsp->last_write_time = cpu_to_le32(st.mtime.tv_sec);
	rsp->size = cpu_to_le32((u32)st.size);
	for (i = 0; i < 5; i++)
		rsp->reserved[i] = 0;

	rsp->ByteCount = 0;
	inc_rfc1001_len(&rsp->hdr,
			(rsp->hdr.WordCount * 2 + rsp->ByteCount));

out:
	smb_put_name(name);
	return err;
}

/**
 * smb_closedir() - handler closing dir handle, opened for readdir
 * @smb_work:	smb work containing find close command buffer
 *
 * Return:	0 on success, otherwise error
 */
int smb_closedir(struct smb_work *smb_work)
{
	FINDCLOSE_REQ *req = (FINDCLOSE_REQ *)smb_work->buf;
	CLOSE_RSP *rsp = (CLOSE_RSP *)smb_work->rsp_buf;
	int err;

	cifsd_debug("SMB_COM_FIND_CLOSE2 called for fid %u\n", req->FileID);

	err = close_id(smb_work->sess, req->FileID, 0);
	if (err)
		goto out;

	/* dir close success, return response to server */
	rsp->hdr.Status.CifsError = NT_STATUS_OK;
	rsp->hdr.WordCount = 0;
	rsp->ByteCount = 0;
	return err;

out:
	if (err)
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_HANDLE;

	return err;
}

/**
 * convert_open_flags() - convert smb open flags to file open flags
 * @file_present:	is file already present
 * @mode:		smp file open mode
 * @disposition:	smp file disposition information
 *
 * Return:	converted file open flags
 */
int convert_open_flags(bool file_present, __u16 mode, __u16 dispostion)
{
	int oflags = 0;

	switch (mode & 0x0007) {
	case SMBOPEN_READ:
		oflags |= O_RDONLY;
		break;
	case SMBOPEN_WRITE:
		oflags |= O_WRONLY;
		break;
	case SMBOPEN_READWRITE:
		oflags |= O_RDWR;
		break;
	default:
		oflags |= O_RDONLY;
		break;
	}

	if (mode & SMBOPEN_WRITE_THROUGH)
		oflags |= O_SYNC;

	if (file_present) {
		switch (dispostion & 0x0003) {
		case SMBOPEN_DISPOSITION_NONE:
			return -EEXIST;
		case SMBOPEN_OAPPEND:
			oflags |= O_APPEND;
			break;
		case SMBOPEN_OTRUNC:
			oflags |= O_TRUNC;
			break;
		default:
			break;
		}
	} else {
		switch (dispostion & 0x0010) {
		case SMBOPEN_DISPOSITION_NONE:
			return -EINVAL;
		case SMBOPEN_OCREATE:
			oflags |= O_CREAT;
			break;
		default:
			break;
		}
	}

	return oflags;
}

/**
 * smb_open_andx() - smb andx open method handler
 * @smb_work:	smb work containing buffer for andx open command buffer
 *
 * Return:	error if there is error while processing current command,
 *		otherwise pointer to next andx command in the chain
 */
int smb_open_andx(struct smb_work *smb_work)
{
	OPENX_REQ *req = (OPENX_REQ *)smb_work->buf;
	OPENX_RSP *rsp = (OPENX_RSP *)smb_work->rsp_buf;
	struct connection *conn = smb_work->conn;
	struct path path;
	struct kstat stat;
	int oplock_flags, file_info, open_flags;
	char *name;
	bool file_present = true;
	__u16 fid;
	umode_t mode = 0;
	int err;

	rsp->hdr.Status.CifsError = NT_STATUS_UNSUCCESSFUL;

	/* check for sharing mode flag */
	if ((le32_to_cpu(req->Mode) & SMBOPEN_SHARING_MODE) >
			SMBOPEN_DENY_NONE) {
		rsp->hdr.Status.DosError.ErrorClass = ERRDOS;
		rsp->hdr.Status.DosError.Error = ERRbadaccess;
		rsp->hdr.Flags2 &= ~SMBFLG2_ERR_STATUS;

		memset(&rsp->hdr.WordCount, 0, 3);
		return -EINVAL;
	}

	if (is_smbreq_unicode(&req->hdr))
		name = smb_get_name(req->fileName + 1, PATH_MAX,
				smb_work, false);
	else
		name = smb_get_name(req->fileName, PATH_MAX,
				smb_work, false);

	if (IS_ERR(name))
		return PTR_ERR(name);

	err = smb_kern_path(name, 0, &path, req->hdr.Flags & SMBFLG_CASELESS);
	if (err)
		file_present = false;
	else
		generic_fillattr(path.dentry->d_inode, &stat);

	oplock_flags = le32_to_cpu(req->OpenFlags) &
		(REQ_OPLOCK | REQ_BATCHOPLOCK);

	open_flags = convert_open_flags(file_present, le16_to_cpu(req->Mode),
			le16_to_cpu(req->OpenFunction));
	if (open_flags < 0) {
		cifsd_debug("create_dispostion returned %d\n", err);
		if (file_present)
			goto free_path;
		else
			goto out;
	}

	if (file_present && !(stat.mode & S_IWUGO)) {
		if ((open_flags & O_ACCMODE) == O_WRONLY ||
				(open_flags & O_ACCMODE) == O_RDWR) {
			cifsd_debug("readonly file(%s)\n", name);
			rsp->hdr.Status.CifsError = NT_STATUS_ACCESS_DENIED;
			memset(&rsp->hdr.WordCount, 0, 3);
			goto free_path;
		}
	}

	if (!file_present && (open_flags & O_CREAT)) {
		mode |= S_IRWXUGO;
		if (le16_to_cpu(req->FileAttributes) & ATTR_READONLY)
			mode &= ~S_IWUGO;

		mode |= S_IFREG;
		err = smb_vfs_create(name, mode);
		if (err)
			goto out;

		err = smb_kern_path(name, 0, &path, 0);
		if (err) {
			cifsd_err("cannot get linux path, err = %d\n", err);
			goto out;
		}
		generic_fillattr(path.dentry->d_inode, &stat);
	}

	cifsd_debug("(%s) open_flags = 0x%x, oplock_flags 0x%x\n",
			name, open_flags, oplock_flags);
	/* open  file and get FID */
	err = smb_dentry_open(smb_work, &path, open_flags,
			&fid, &oplock_flags, 0, file_present);
	if (err)
		goto free_path;

	/* open success, send back response */
	if (file_present) {
		if (!(open_flags & O_TRUNC))
			file_info = F_OPENED;
		else
			file_info = F_OVERWRITTEN;
	} else {
		file_info = F_CREATED;
	}

	if (oplock_flags)
		file_info |= SMBOPEN_LOCK_GRANTED;

	/* prepare response buffer */
	rsp->hdr.Status.CifsError = NT_STATUS_OK;
	rsp->hdr.WordCount = 0x0F;
	rsp->Fid = fid;
	rsp->FileAttributes = cpu_to_le16(ATTR_NORMAL);
	rsp->LastWriteTime = cpu_to_le32(stat.mtime.tv_sec);
	rsp->EndOfFile = cpu_to_le32(stat.size);
	switch (open_flags & O_ACCMODE) {
	case O_RDONLY:
		rsp->Access = cpu_to_le16(SMB_DA_ACCESS_READ);
		break;
	case O_WRONLY:
		rsp->Access = cpu_to_le16(SMB_DA_ACCESS_WRITE);
		break;
	case O_RDWR:
		rsp->Access = cpu_to_le16(SMB_DA_ACCESS_READ_WRITE);
		break;
	default:
		rsp->Access = cpu_to_le16(SMB_DA_ACCESS_READ);
		break;
	}

	rsp->FileType = 0;
	rsp->IPCState = 0;
	rsp->Action = cpu_to_le16(file_info);
	rsp->Reserved = 0;
	rsp->ByteCount = 0;
	inc_rfc1001_len(&rsp->hdr, (rsp->hdr.WordCount * 2 + rsp->ByteCount));

free_path:
	path_put(&path);
out:
	if (err) {
		if (err == -ENOSPC)
			rsp->hdr.Status.CifsError = NT_STATUS_DISK_FULL;
		else if (err == -EMFILE)
			rsp->hdr.Status.CifsError =
				NT_STATUS_TOO_MANY_OPENED_FILES;
		else
			rsp->hdr.Status.CifsError =
				NT_STATUS_UNEXPECTED_IO_ERROR;
	} else
		conn->stats.open_files_count++;

	smb_put_name(name);
	if (!rsp->hdr.WordCount)
		return err;

	/* this is an ANDx command ? */
	if (req->AndXCommand == 0xFF) {
		rsp->AndXCommand = SMB_NO_MORE_ANDX_COMMAND;
		rsp->AndXReserved = 0;
		rsp->AndXOffset = 0;
		return err;
	} else {
		/* adjust response */
		rsp->AndXOffset = get_rfc1002_length(&rsp->hdr);
		rsp->AndXCommand = req->AndXCommand;
		rsp->AndXReserved = 0;

		return rsp->AndXCommand; /* More processing required */
	}
}

/**
 * smb_setattr() - set file attributes
 * @smb_work:	smb work containing setattr command
 *
 * Return:	0 on success, otherwise error
 */
int smb_setattr(struct smb_work *smb_work)
{
	SETATTR_REQ *req;
	SETATTR_RSP *rsp;
	struct path path;
	struct kstat stat;
	struct iattr attrs;
	int err = 0;
	char *name;
	__u16 dos_attr;

	req = (SETATTR_REQ *)smb_work->buf;
	rsp = (SETATTR_RSP *)smb_work->rsp_buf;
	name = smb_get_name(req->fileName, PATH_MAX, smb_work, false);
	if (IS_ERR(name))
		return PTR_ERR(name);

	err = smb_kern_path(name, 0, &path, req->hdr.Flags & SMBFLG_CASELESS);
	if (err) {
		cifsd_debug("look up failed err %d\n", err);
		rsp->hdr.Status.CifsError = NT_STATUS_OBJECT_NAME_NOT_FOUND;
		err = 0;
		goto out;
	}
	generic_fillattr(path.dentry->d_inode, &stat);
	path_put(&path);
	attrs.ia_valid = 0;
	attrs.ia_mode = 0;

	dos_attr = le16_to_cpu(req->attr);
	if (!dos_attr)
		attrs.ia_mode = stat.mode | S_IWUSR;

	if (dos_attr & ATTR_READONLY)
		attrs.ia_mode = stat.mode & ~S_IWUGO;

	if (attrs.ia_mode)
		attrs.ia_valid |= ATTR_MODE;

	err = smb_vfs_setattr(smb_work->sess, name, 0, &attrs);
	if (err)
		goto out;

	rsp->hdr.Status.CifsError = NT_STATUS_OK;
	rsp->hdr.WordCount = 0;
	rsp->ByteCount = 0;

out:
	smb_put_name(name);
	if (err) {
		rsp->hdr.Status.CifsError = NT_STATUS_INVALID_PARAMETER;
		return err;
	}

	return 0;
}

/**
 * smb1_is_sign_req() - handler for checking packet signing status
 * @work:	smb work containing notify command buffer
 *
 * Return:	1 if packed is signed, 0 otherwise
 */
int smb1_is_sign_req(struct smb_work *work, unsigned int command)
{
	struct smb_hdr *rcv_hdr1 = (struct smb_hdr *)work->buf;

	if ((rcv_hdr1->Flags2 & SMBFLG2_SECURITY_SIGNATURE) &&
			command != SMB_COM_SESSION_SETUP_ANDX)
		return 1;
	return 0;
}

/**
 * smb1_check_sign_req() - handler for req packet sign processing
 * @work:	smb work containing notify command buffer
 *
 * Return:	1 on success, 0 otherwise
 */
int smb1_check_sign_req(struct smb_work *work)
{
	struct smb_hdr *rcv_hdr1 = (struct smb_hdr *)work->buf;
	char signature_req[CIFS_SMB1_SIGNATURE_SIZE];
	char signature[20];
	struct kvec iov[1];

	memcpy(signature_req, rcv_hdr1->Signature.SecuritySignature,
			CIFS_SMB1_SIGNATURE_SIZE);
	rcv_hdr1->Signature.Sequence.SequenceNumber =
		++work->sess->sequence_number;
	rcv_hdr1->Signature.Sequence.Reserved = 0;

	iov[0].iov_base = rcv_hdr1->Protocol;
	iov[0].iov_len = be32_to_cpu(rcv_hdr1->smb_buf_length);

	if (smb1_sign_smbpdu(work->sess, iov, 1, signature))
		return 0;

	if (memcmp(signature, signature_req, CIFS_SMB1_SIGNATURE_SIZE)) {
		cifsd_debug("bad smb1 sign\n");
		return 0;
	}

	return 1;
}

/**
 * smb1_set_sign_rsp() - handler for rsp packet sign procesing
 * @work:	smb work containing notify command buffer
 *
 */
void smb1_set_sign_rsp(struct smb_work *work)
{
	struct smb_hdr *rsp_hdr = (struct smb_hdr *)work->rsp_buf;
	char signature[20];
	struct kvec iov[2];
	int n_vec = 1;

	rsp_hdr->Flags2 |= SMBFLG2_SECURITY_SIGNATURE;
	rsp_hdr->Signature.Sequence.SequenceNumber =
		++work->sess->sequence_number;
	rsp_hdr->Signature.Sequence.Reserved = 0;

	iov[0].iov_base = rsp_hdr->Protocol;
	iov[0].iov_len = be32_to_cpu(rsp_hdr->smb_buf_length);

	if (work->rdata_buf) {
		iov[0].iov_len -= work->rdata_cnt;

		iov[1].iov_base = work->rdata_buf;
		iov[1].iov_len = work->rdata_cnt;
		n_vec++;
	}

	if (smb1_sign_smbpdu(work->sess, iov, n_vec, signature))
		memset(rsp_hdr->Signature.SecuritySignature,
				0, CIFS_SMB1_SIGNATURE_SIZE);
	else
		memcpy(rsp_hdr->Signature.SecuritySignature,
				signature, CIFS_SMB1_SIGNATURE_SIZE);
}
