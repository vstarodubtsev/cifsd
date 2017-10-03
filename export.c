/*
 *   fs/cifsd/export.c
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

#include <linux/types.h>
#include <linux/parser.h>
#include "glob.h"
#include "export.h"
#include "smb1pdu.h"

/* max string size for share and parameters */
#define SHARE_MAX_NAME_LEN	100
/* max string size data, ex- path, usernames, servernames etc */
#define SHARE_MAX_DATA_LEN	PATH_MAX
#define MAX_NT_PWD_LEN		128

/*
 * There could be 2 ways to add path to an export list.
 * One is static, via a conf file. Other is dynamic, via sysfs entry.
 */
struct cifsd_sysfs_obj *sysobj;

LIST_HEAD(cifsd_usr_list);
LIST_HEAD(cifsd_share_list);
LIST_HEAD(cifsd_connection_list);
LIST_HEAD(cifsd_session_list);

__u16 vid = 1;
__u16 tid = 1;
int cifsd_debug_enable;
int cifsd_caseless_search;
static char statIP[MAX_ADDRBUFLEN];
static inline void free_share(struct cifsd_share *share);

/* Number of shares defined on server */
int cifsd_num_shares;

/* The parameters defined on configuration */
int maptoguest;
int server_signing;
char *guestAccountName;
char *server_string;
char *workgroup;
char *netbios_name;
int server_min_pr;
int server_max_pr;

struct cifsd_pipe_table cifsd_pipes[] = {
	{"\\srvsvc", SRVSVC},
	{"srvsvc", SRVSVC},
	{"\\wkssvc", SRVSVC},
	{"wkssvc", SRVSVC},
	{"\\winreg", WINREG},
	{"winreg", WINREG},
};
unsigned int npipes = ARRAY_SIZE(cifsd_pipes);

/**
 * get_pipe_type() - get the type of the pipe from the string name
 * @name:      string name for representation of pipe, need to be searched
 *             in the supported table
 * Return:     the pipe type number if found in the table,
 *             else invalid pipe type
 */
unsigned int get_pipe_type(char *pipename)
{
	int i;
	unsigned int pipetype = INVALID_PIPE;

	for (i = 0; i < npipes; i++) {
		if (!strcmp(cifsd_pipes[i].pipename, pipename)) {
			pipetype = cifsd_pipes[i].pipetype;
			break;
		}
	}
	return pipetype;
}

/**
 * get_pipe_desc() - get matching pipe descriptor from pipe id
 * @sess:	session info
 * @id:		lookup pipe id
 *
 * Return:	matching pipe descriptor from opened pipe id
 */
struct cifsd_pipe *get_pipe_desc(struct cifsd_sess *sess,
		unsigned int id)
{
	struct cifsd_pipe *pipe_desc;
	int i;

	if (unlikely(!sess))
		return NULL;

	for (i = 0; i < MAX_PIPE; i++) {
		/* fid is not created for LANMAN */
		if (i == LANMAN)
			continue;

		pipe_desc = sess->pipe_desc[i];
		if (!pipe_desc)
			continue;

		if (pipe_desc->id == id)
			return pipe_desc;
	}

	return NULL;
}

/**
 * __add_share() - helper function to add a share in global exported share list
 * @share:	share instance to be added to global share list
 * @sharename:	name of share
 * @pathname:	path of share point
 *
 * Return:      true on success, false on error
 */
static bool __add_share(struct cifsd_share *share, char *sharename,
		       char *pathname)
{
	struct kstat stat;
	struct path share_path;
	int err;

	/* pathname will be NULL for IPC$ share */
	if (pathname != NULL) {
		err = kern_path(pathname, 0, &share_path);
		if (err) {
			cifsd_err("share add failed for %s\n", pathname);
			return false;
		} else {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 11, 0)
			err = vfs_getattr(&share_path, &stat, STATX_BASIC_STATS,
				AT_STATX_SYNC_AS_STAT);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
			err = vfs_getattr(&share_path, &stat);
#else
			err = vfs_getattr(share_path.mnt, share_path.dentry, &stat);
#endif
			path_put(&share_path);
			if (err) {
				cifsd_err("share add failed for %s\n",
					    pathname);
				return false;
			}
		}
	}

	share->path = pathname;
	share->tcount = 0;
	share->tid = tid++;
	share->sharename = sharename;
	INIT_LIST_HEAD(&share->list);
	list_add(&share->list, &cifsd_share_list);
	cifsd_num_shares++;
	return true;
}

/**
 * init_params() - initialize config parameters of a share
 * @share:	share instance to be initialized
 */
static void init_params(struct cifsd_share *share)
{
	set_attr_available(&share->config.attr);
	set_attr_browsable(&share->config.attr);
	clear_attr_guestok(&share->config.attr);
	clear_attr_guestonly(&share->config.attr);
	set_attr_oplocks(&share->config.attr);
	set_attr_readonly(&share->config.attr);
	set_attr_writeok(&share->config.attr);
	share->config.max_connections = 0;
}

/**
 * add_share() - allocate and add a share in global exported share list
 * @sharename:	name of share
 * @pathname:	path of share point
 *
 * Return:      0 on success, error number on error
 */
static int add_share(char *sharename, char *pathname)
{
	struct cifsd_share *share;
	int ret;

	share = kzalloc(sizeof(struct cifsd_share), GFP_KERNEL);
	if (!share)
		return -ENOMEM;

	init_params(share);

	ret = __add_share(share, sharename, pathname);
	if (!ret) {
		free_share(share);
		kfree(share);
	}

	return 0;
}

/**
 * free_share() - free a share point release associated memory
 * @share:	share instance to be freed
 */
static inline void free_share(struct cifsd_share *share)
{
	kfree(share->sharename);

	if (share->path)
		kfree(share->path);

	kfree(share->config.comment);
	kfree(share->config.allow_hosts);
	kfree(share->config.deny_hosts);
	kfree(share->config.invalid_users);
	kfree(share->config.read_list);
	kfree(share->config.write_list);
	kfree(share->config.valid_users);
}

/**
 * cifsd_share_free() - delete all shares from global exported share list
 */
static void cifsd_share_free(void)
{
	struct cifsd_share *share;
	struct list_head *tmp, *t;

	list_for_each_safe(tmp, t, &cifsd_share_list) {
		share = list_entry(tmp, struct cifsd_share, list);
		list_del(&share->list);
		cifsd_num_shares--;
		free_share(share);
		kfree(share);
	}
}

/**
 * cleanup_bad_share() - remove a bad share, added while config parse error
 * @badshare:	share name to be removed
 */
static void cleanup_bad_share(struct cifsd_share *badshare)
{
	struct cifsd_share *share;
	struct list_head *tmp, *t;

	list_for_each_safe(tmp, t, &cifsd_share_list) {
		share = list_entry(tmp, struct cifsd_share, list);
		if (share != badshare)
			continue;
		list_del(&share->list);
		cifsd_num_shares--;
	}
	free_share(badshare);
	kfree(badshare);
}

/**
 * add_user() - allocate and add an user in global user list
 * @name:	user name to be added
 * @pass:	password of user
 *
 * Return:      0 on success, error number on error
 */
static int add_user(char *name, char *pass, kuid_t uid, kgid_t gid)
{
	struct cifsd_usr *usr;

	usr = kmalloc(sizeof(struct cifsd_usr), GFP_KERNEL);
	if (!usr)
		return -ENOMEM;

	if (guestAccountName) {
		if (strcmp(guestAccountName, name) == 0) {
			usr->vuid = 0;
			usr->guest = true;
			usr->name = guestAccountName;
		} else{
			usr->vuid = vid++;
			usr->guest = false;
			usr->name = name;
			memcpy(usr->passkey, pass, CIFS_NTHASH_SIZE);
		}
	} else{
		usr->vuid = vid++;
		usr->guest = false;
		usr->name = name;
		memcpy(usr->passkey, pass, CIFS_NTHASH_SIZE);
	}

	usr->uid.val = uid.val;
	usr->gid.val = gid.val;
	usr->sess_uid = 0;
	INIT_LIST_HEAD(&usr->list);
	list_add(&usr->list, &cifsd_usr_list);
	usr->ucount = 0;
	return 0;
}

/**
 * cifsd_user_free() - delete all users from global exported user list
 */
static void cifsd_user_free(void)
{
	struct cifsd_usr *usr, *tmp;

	list_for_each_entry_safe(usr, tmp, &cifsd_usr_list, list) {
		list_del(&usr->list);
		kfree(usr->name);
		kfree(usr);
	}
}

static int parse_user_strings(const char *src, char **str, int exp_num,
	ssize_t src_len)
{
	int s_num = 0, pos = 0;
	ssize_t len;

	while (s_num < exp_num) {
		len = strcspn(&src[pos], (const char *)":") + 1;
		if (!len)
			break;

		str[s_num] = kmalloc(len, GFP_KERNEL);
		if (!str[s_num])
			break;

		strlcpy(str[s_num], &src[pos], len);
		s_num++;
		pos += len;
		if (pos >= src_len)
			break;
	}

	return s_num;
}

/**
 * chktkn() - utility function to validate user or host
 * @userslist:	list of allowed or denied user or host
 * @str2:	check if this user or host is present in userslist
 *
 * Return:      1 if str2 is present in userslist, otherwise 0
 */
static int chktkn(char *userslist, char *str2)
{
	char *token;
	char *dup, *dup_orig;

	if (userslist) {
		dup_orig = dup = kstrdup(userslist, GFP_KERNEL);
		if (!dup)
			return -ENOMEM;

		while ((token = strsep(&dup, "	, ")) != NULL) {
			if (!strcmp(token, str2)) {
				kfree(dup_orig);
				return 1;
			}
		}
		kfree(dup_orig);
		return -ENOENT;
	}
	return 0;
}

/**
 * validate_host() - check if a client is allowed or denied access of a share
 * @cip:	host ip to be checked
 * @share:	share config containing allowed and denied list of client ip
 *
 * Return:      1 if cip is allowed access to share, otherwise 0
 */
int validate_host(char *cip, struct cifsd_share *share)
{
	char *alist = share->config.allow_hosts;
	char *dlist = share->config.deny_hosts;
	int allow, deny;
	int asz = 0;
	int dsz = 0;

	if (alist)
		asz = strlen(alist);
	if (dlist)
		dsz = strlen(dlist);

	if (!asz && !dsz)
		return 1;

	allow = chktkn(alist, cip);
	if (allow == -ENOENT)
		return -EACCES;
	else if (allow < 0)
		return allow;

	/*
	 * "allow hosts" list takes precedence over "deny hosts" list,
	 *  No further checking needed
	 */
	if (allow > 0)
		return 1;

	deny = chktkn(dlist, cip);
	if (deny < 0)
		return -ENOMEM;

	if (!asz && deny)
		return -EACCES;

	/*
	 * Default is always allowed - So, when there is no allowed list
	 * and no entry in Deny, then switch to default behaviour
	 */
	return 1;
}

/**
 * validate_usr() - check if an user is allowed or denied access of a share
 * @usr:	user to be checked
 * @share:	share config containing allowed and denied list of users
 *
 * Return:      1 if usr is allowed access to share, otherwise error
 */
int validate_usr(struct cifsd_sess *sess, struct cifsd_share *share,
	bool *can_write)
{
	char *vlist = share->config.valid_users;
	char *ilist = share->config.invalid_users;
	char *wlist = share->config.write_list;
	char *rlist = share->config.read_list;
	int ret;

	/* for share IPC$, does not support smb.conf share parameters*/
	if (!share->path)
		return 1;

	/* if "guest = ok, no checking of users required "*/
	/*
	* if guest ok not set, but guestAccountname
	* mapped with valid share path
	*/
	if (get_attr_guestok(&share->config.attr)) {
		cifsd_debug("guest login on to share %s\n",
				share->sharename);
		return 1;
	}

	/* name should not be present in "invalid users" */
	ret = chktkn(ilist, sess->usr->name);
	if (ret == -ENOMEM)
		return -ENOMEM;
	if (ret > 0)
		return -EACCES;

	*can_write = (share->writeable == 1) ? true : false;
	/* if user present in read list, sess will be readable */
	ret = chktkn(rlist, sess->usr->name);
	if (ret > 0)
		*can_write = false;

	/* if user present in write list, make user session writeable */
	ret = chktkn(wlist, sess->usr->name);
	if (ret > 0)
		*can_write = true;

	/* if "valid users" list is empty then any user can login */
	if (!vlist)
		return 1;

	/* user exists in "valid users" list? */
	return chktkn(vlist, sess->usr->name);
}

struct cifsd_share *get_cifsd_share(struct connection *conn,
		struct cifsd_sess *sess,
		char *sharename, bool *can_write)
{
	struct list_head *tmp;
	struct cifsd_share *share;
	int rc;

	list_for_each(tmp, &cifsd_share_list) {
		share = list_entry(tmp, struct cifsd_share, list);
		cifsd_debug("comparing(%s) with treename %s\n",
				sharename, share->sharename);
		if (!strcasecmp(share->sharename, sharename)) {
			rc = validate_host(conn->peeraddr, share);
			if (rc < 0) {
				cifsd_err(
				"[host:%s] not allowed for [share:%s]\n"
				, conn->peeraddr, share->sharename);
				return ERR_PTR(rc);
			}
			rc = validate_usr(sess, share, can_write);
			if (rc < 0) {
				cifsd_err(
				"[user:%s] not authorised for [share:%s]\n",
				sess->usr->name, share->sharename);
				return ERR_PTR(rc);
			}
			return share;
		}
	}
	cifsd_debug("Tree(%s) not exported on connection\n", sharename);
	return ERR_PTR(-ENOENT);
}

/**
 * find_matching_share() - get a share instance from tree id
 * @tid:	tree id for share instance lookup
 *
 * Return:      share if there is matching share tid, otherwise NULL
 */
struct cifsd_share *find_matching_share(__u16 tid)
{
	struct cifsd_share *share;
	struct list_head *tmp;

	list_for_each(tmp, &cifsd_share_list) {
		share = list_entry(tmp, struct cifsd_share, list);
		if (share->tid == tid)
			return share;
	}

	return NULL;
}

struct cifsd_usr *cifsd_is_user_present(char *name)
{
	struct cifsd_usr *usr, *tmp, *guest_user = NULL;

	if (!name)
		return NULL;

	list_for_each_entry_safe(usr, tmp, &cifsd_usr_list, list) {
		cifsd_debug("comparing with user %s\n", usr->name);
		if (!strcmp(name, usr->name))
			return usr;
		else if (usr->guest && maptoguest)
			guest_user = usr;
	}
	return guest_user;
}

/**
 * get_smb_session_user() - get logged in user information for a session
 * @sess:    session information
 *
 * Return:      matching user for a session on success, otherwise NULL
 */
struct cifsd_usr *get_smb_session_user(struct cifsd_sess *sess)
{
	struct cifsd_usr *usr;

	list_for_each_entry(usr, &cifsd_usr_list, list) {
		if (sess->conn->vuid  == usr->vuid)
			return usr;
	}

	return NULL;
}

/**
 * check_sharepath() - check if a share path is already exported
 * @path:	share path to check
 *
 * Return:      false if share is already exported, otherwise true
 */
static bool check_sharepath(char *path)
{
	struct cifsd_share *share;
	struct list_head *tmp;
	int srclen, targetlen = 0;

	srclen = strlen(path);

	list_for_each(tmp, &cifsd_share_list) {
		share = list_entry(tmp, struct cifsd_share, list);
		if (share->path) {
			targetlen = strlen(share->path);
			if (srclen == targetlen) {
				if (!strncmp(path, share->path, srclen))
					return false;
			}
		}
	}

	return true;
}

/**
 * getUser() - check if a user name is already added
 * @name:	user name to be checked
 * @pass:	user password
 *
 * Return:      false if user entry exists, otherwise true
 */
static bool getUser(char *name, char *pass)
{
	struct cifsd_usr *usr;

	usr = cifsd_is_user_present(name);
	if (usr) {
		if (!strlen(pass)) {
			list_del(&usr->list);
			kfree(usr->name);
			kfree(usr);
			return false;
		}
		memcpy(usr->passkey, pass,
				CIFS_NTHASH_SIZE);
		return false;
	}

	return true;
}

/**
 * check_share() - check if a share name is already exported,if not
 *		allocate a new empty share
 * @share_buf:	buffer containing share name
 * @share_sz:	share name length
 *
 * Return:      share name if already exported, otherwise NULL
 */
static struct cifsd_share *check_share(char *share_name, int *alloc_share)
{
	struct cifsd_share *share;
	struct list_head *tmp;
	int srclen;

	list_for_each(tmp, &cifsd_share_list) {
		share = list_entry(tmp, struct cifsd_share, list);
		srclen = strlen(share->sharename);
		if (srclen == strlen(share_name)) {
			if (strncasecmp(share->sharename,
					share_name, srclen) == 0) {
				return share;
			}
		}
	}

	share = kzalloc(sizeof(struct cifsd_share), GFP_KERNEL);
	if (!share)
		return ERR_PTR(-ENOMEM);

	init_params(share);
	*alloc_share = 1;
	return share;
}

/**
 * share_show() - show a list of exported shares
 * @kobj:	kobject of the modules
 * @kobj_attr:	kobject attribute of the modules
 * @buf:	buffer containing share list output
 *
 * Return:      output buffer length
 */
static ssize_t share_show(struct kobject *kobj,
			  struct kobj_attribute *kobj_attr,
			  char *buf)
{
	struct cifsd_share *share;
	struct list_head *tmp;
	ssize_t len = 0, total = 0, limit = PAGE_SIZE;
	char *tbuf = buf;

	list_for_each(tmp, &cifsd_share_list) {
		share = list_entry(tmp, struct cifsd_share, list);
		if (share->path) {
			len = snprintf(tbuf, limit, "%s:%s\n",
				 share->sharename, share->path);
			if (len < 0) {
				total = len;
				break;
			}
			tbuf += len;
			total += len;
			limit -= len;
		}
	}

	return total;
}

/**
 * share_store() - add a share path in exported share list
 * @kobj:	kobject of the modules
 * @kobj_attr:	kobject attribute of the modules
 * @buf:	buffer containing share path to be exported
 * @len:	share name buf length
 *
 * Return:      share name buf length on success, otherwise error
 */
static ssize_t share_store(struct kobject *kobj,
			   struct kobj_attribute *kobj_attr,
			   const char *buf, size_t len)
{
	char *share, *path;
	int rc;
	char *parse_ptr[2];

	rc = parse_user_strings(buf, parse_ptr, 2, len);
	if (rc < 2)
		return -EINVAL;

	share = parse_ptr[0];
	path = parse_ptr[1];

	/* check if sharepath is already exported */
	rc = check_sharepath(path);
	if (!rc) {
		cifsd_err("path %s is already exported\n", path);
		kfree(share);
		kfree(path);
		return -EEXIST;
	}

	rc = add_share(share, path);
	if (rc) {
		kfree(share);
		kfree(path);
		return rc;
	}

	return len;
}

/**
 * user_show() - show a list of added user
 * @kobj:	kobject of the modules
 * @kobj_attr:	kobject attribute of the modules
 * @buf:	buffer containing user list output
 *
 * Return:      output buffer length
 */
static ssize_t user_show(struct kobject *kobj,
			 struct kobj_attribute *kobj_attr,
			 char *buf)

{
	struct cifsd_usr *usr, *tmp;
	ssize_t len = 0, total = 0, limit = PAGE_SIZE;
	char *tbuf = buf;

	list_for_each_entry_safe(usr, tmp, &cifsd_usr_list, list) {
		len = snprintf(tbuf, limit, "%s\n", usr->name);
		if (len < 0) {
			total = len;
			break;
		}
		tbuf += len;
		total += len;
		limit -= len;
	}

	return total;
}

/**
 * user_store() - add a user in valid user list
 * @kobj:	kobject of the modules
 * @kobj_attr:	kobject attribute of the modules
 * @buf:	buffer containing user name to be added
 * @len:	user name buf length
 *
 * Return:      user name buf length on success, otherwise error
 */
static ssize_t user_store(struct kobject *kobj,
			  struct kobj_attribute *kobj_attr,
			  const char *buf, size_t len)
{
	char *usrname, *passwd;
	int rc, i;
	char *parse_ptr[4] = {0};
	kuid_t uid;
	kgid_t gid;

	rc = parse_user_strings(buf, parse_ptr, 4, len);
	if (rc < 2) {
		cifsd_err("[%s] <usr:pass> format err\n", __func__);
		goto out;
	}

	usrname = parse_ptr[0];
	passwd = parse_ptr[1];

	if (rc > 2) {
		if (kstrtouint(parse_ptr[2], 10, &uid.val) ||
				kstrtouint(parse_ptr[3], 10, &gid.val)) {
			goto out;
		}

		cifsd_debug("uid : %u, gid %u\n", uid.val, gid.val);
	} else {
		uid.val = 0;
		gid.val = 0;
	}

	/* check if user is already present*/
	rc = getUser(usrname, passwd);
	if (!rc) {
		kfree(usrname);
		kfree(passwd);
	} else {
		rc = add_user(usrname, passwd, uid, gid);
		kfree(passwd);
		if (rc) {
			kfree(usrname);
			if (rc == -ENOMEM)
				goto out;
		}
	}

	return len;
out:
	for (i = 0; i < 4; i++)
		kfree(parse_ptr[i]);

	return -EINVAL;
}

/**
 * debug_store() - enable debug prints
 * @kobj:	kobject of the modules
 * @kobj_attr:	kobject attribute of the modules
 * @buf:	buffer containing debug enable disable setting
 * @len:	buf length of debug enable disable setting
 *
 * Return:      debug setting buf length
 */
static ssize_t debug_store(struct kobject *kobj,
			   struct kobj_attribute *kobj_attr,
			   const char *buf, size_t len)
{
	long int value;

	if (kstrtol(buf, 10, &value))
		return len;

	if (value > 0)
		cifsd_debug_enable = value;
	else if (value == 0)
		cifsd_debug_enable = 0;

	return len;
}

/**
 * debug_show() - show debug print enable disable setting
 * @kobj:	kobject of the modules
 * @kobj_attr:	kobject attribute of the modules
 * @buf:	buffer containing debug print setting
 *
 * Return:      output buffer length
 */
static ssize_t debug_show(struct kobject *kobj,
			  struct kobj_attribute *kobj_attr,
			  char *buf)

{
	return snprintf(buf, PAGE_SIZE, "%d\n", cifsd_debug_enable);
}

/**
 * caseless_search_store() - enable disable case insensitive search of files
 * @kobj:	kobject of the modules
 * @kobj_attr:	kobject attribute of the modules
 * @buf:	buffer containing case setting
 * @len:	buf length of case setting
 *
 * Return:      case setting buf length
 */
static ssize_t caseless_search_store(struct kobject *kobj,
				     struct kobj_attribute *kobj_attr,
				     const char *buf, size_t len)
{
	long int value;

	if (kstrtol(buf, 10, &value))
		goto out;
	if (value > 0)
		cifsd_caseless_search = 1;
	else if (value == 0)
		cifsd_caseless_search = 0;

out:
	return len;
}

/**
 * caseless_search_show() - show caseless search enable disable setting status
 * @kobj:	kobject of the modules
 * @kobj_attr:	kobject attribute of the modules
 * @buf:	buffer containing caseless search setting
 *
 * Return:      output buffer length
 */
static ssize_t caseless_search_show(struct kobject *kobj,
		struct kobj_attribute *kobj_attr,
		char *buf)

{
	return snprintf(buf, PAGE_SIZE, "%d\n", cifsd_caseless_search);
}

enum {
	Opt_guest,
	Opt_servern,
	Opt_domain,
	Opt_netbiosname,
	Opt_signing,
	Opt_maptoguest,
	Opt_server_min_protocol,
	Opt_server_max_protocol,

	Opt_global_err
};

static const match_table_t cifsd_global_tokens = {
	{ Opt_guest, "guest account = %s" },
	{ Opt_servern, "server string = %s" },
	{ Opt_domain, "workgroup = %s" },
	{ Opt_netbiosname, "netbios name = %s" },
	{ Opt_signing, "server signing = %s" },
	{ Opt_maptoguest, "map to guest = %s" },
	{ Opt_server_min_protocol, "server min protocol = %s" },
	{ Opt_server_max_protocol, "server max protocol = %s" },

	{ Opt_global_err, NULL }
};

enum {
	Opt_sharename,
	Opt_available,
	Opt_browsable,
	Opt_writeable,
	Opt_guestok,
	Opt_guestonly,
	Opt_oplocks,
	Opt_maxcon,
	Opt_comment,
	Opt_allowhost,
	Opt_denyhost,
	Opt_validusers,
	Opt_invalidusers,
	Opt_path,
	Opt_readlist,
	Opt_readonly,
	Opt_writeok,
	Opt_writelist,
	Opt_hostallow,
	Opt_hostdeny,
	Opt_store_dos_attr,

	Opt_share_err
};

static const match_table_t cifsd_share_tokens = {
	{ Opt_sharename, "sharename = %s" },
	{ Opt_available, "available = %s" },
	{ Opt_browsable, "browsable = %s" },
	{ Opt_writeable, "writeable = %s" },
	{ Opt_guestok, "guest ok = %s" },
	{ Opt_guestonly, "guest only = %s" },
	{ Opt_oplocks, "oplocks = %s" },
	{ Opt_maxcon, "max connections = %s" },
	{ Opt_comment, "comment = %s" },
	{ Opt_allowhost, "allow hosts = %s" },
	{ Opt_denyhost, "deny hosts = %s" },
	{ Opt_validusers, "valid users = %s" },
	{ Opt_invalidusers, "invalid users = %s" },
	{ Opt_path, "path = %s" },
	{ Opt_readlist, "read list = %s" },
	{ Opt_readonly, "read only = %s" },
	{ Opt_writeok, "write ok = %s" },
	{ Opt_writelist, "write list = %s" },
	{ Opt_hostallow, "hosts allow = %s" },
	{ Opt_hostdeny, "hosts deny = %s" },
	{ Opt_store_dos_attr, "store dos attributes = %s" },

	{ Opt_share_err, NULL }
};

/*
 * cifsd_get_config_str() - get a configuration string
 * @arg:	configuration argument list
 * @config:	destination to store output config string
 *
 * Return:      0 on success, otherwise -ENOMEM
 */
static int cifsd_get_config_str(substring_t args[], char **config)
{
	kfree(*config);
	*config = match_strdup(args);
	if (!*config)
		return -ENOMEM;

	return 0;
}

/*
 * cifsd_get_config_val() - get a configuration string value
 * @arg:	configuration argument list
 * @val:	destination to store output val
 *
 * Return:      0 on success, otherwise error
 */
static int cifsd_get_config_val(substring_t args[], unsigned int *val)
{
	char *str;
	int ret = 0;

	str = match_strdup(args);
	if (str == NULL)
		return -ENOMEM;

	if (!strcasecmp(str, "yes") ||
	    !strcasecmp(str, "true") ||
	    !strcasecmp(str, "enable") ||
	    !strcasecmp(str, "Bad User") ||
	    !strcmp(str, "1"))
		*val = ENABLE;
	else if (!strcasecmp(str, "no") ||
		 !strcasecmp(str, "false") ||
		 !strcasecmp(str, "disable") ||
		 !strcasecmp(str, "Never") ||
		 !strcmp(str, "0"))
		*val = DISABLE;
	else if (!strcasecmp(str, "auto"))
		*val = AUTO;
	else if (!strcasecmp(str, "mandatory"))
		*val = MANDATORY;
	else {
		cifsd_err("bad option value %s\n", str);
		ret = -EINVAL;
	}

	kfree(str);
	return ret;
}

static int cifsd_parse_global_options(char *configdata)
{
	char *data;
	char *options;
	char separator[2];
	char *string = NULL;

	separator[0] = '<';
	separator[1] = 0;

	if (!configdata)
		goto config_err;

	options = configdata;

	while ((data = strsep(&options, separator)) != NULL) {
		substring_t args[MAX_OPT_ARGS];
		int token;

		if (!*data)
			continue;

		token = match_token(data, cifsd_global_tokens, args);
		switch (token) {
		case Opt_guest:
		{
			kuid_t uid;
			kgid_t gid;

			if (cifsd_get_config_str(args, &guestAccountName))
				goto out_nomem;

			uid.val = 9999;
			gid.val = 9999;
			add_user(guestAccountName, NULL, uid, gid);
			break;
		}
		case Opt_servern:
			if (cifsd_get_config_str(args, &server_string))
				goto out_nomem;
			break;
		case Opt_domain:
			if (cifsd_get_config_str(args, &workgroup))
				goto out_nomem;
			break;
		case Opt_netbiosname:
			if (cifsd_get_config_str(args, &netbios_name))
				goto out_nomem;
			break;
		case Opt_signing:
			if (cifsd_get_config_val(args, &server_signing) < 0)
				goto out_nomem;
			break;
		case Opt_maptoguest:
			if (cifsd_get_config_val(args, &maptoguest) < 0)
				goto out_nomem;
			break;
		case Opt_server_min_protocol:
			string = match_strdup(args);
			if (string == NULL)
				goto out_nomem;
			server_min_pr = get_protocol_idx(string);
			if (server_min_pr < 0)
				server_min_pr = cifsd_min_protocol();
			kfree(string);
			break;
		case Opt_server_max_protocol:
			string = match_strdup(args);
			if (string == NULL)
				goto out_nomem;
			server_max_pr = get_protocol_idx(string);
			if (server_max_pr < 0)
				server_max_pr = cifsd_max_protocol();
			kfree(string);
			break;
		default:
			cifsd_err("[%s] not supported\n", data);
			break;
		}
	}

	return 0;

out_nomem:
	cifsd_err("Could not allocate buffer\n");

config_err:
	return 1;
}

static int cifsd_parse_share_options(const char *configdata)
{
	struct cifsd_share *share = NULL;
	char *data, *end;
	char *configdata_copy = NULL, *options;
	char separator[2];
	char *string = NULL;
	unsigned int val;
	unsigned int new_share = 0;

	separator[0] = '<';
	separator[1] = 0;

	if (!configdata)
		goto config_err;

	configdata_copy = kstrndup(configdata, PAGE_SIZE, GFP_KERNEL);
	if (!configdata_copy)
		goto config_err;

	options = configdata_copy;
	end = options + strlen(options);

	while ((data = strsep(&options, separator)) != NULL) {
		substring_t args[MAX_OPT_ARGS];
		int token;

		if (!*data)
			continue;

		token = match_token(data, cifsd_share_tokens, args);
		switch (token) {
		case Opt_sharename:
			string = match_strdup(args);
			if (string == NULL)
				goto out_nomem;
			if (!strncmp(string, "global", 6)) {
				kfree(string);
				if (cifsd_parse_global_options(options) != 0)
					goto config_err;
				options = end;
			} else {
				share = check_share(string, &new_share);
				if (IS_ERR(share)) {
					kfree(string);
					share = NULL;
					goto config_err;
				}
				share->sharename = string;
			}
			break;
		case Opt_available:
			if (!share || cifsd_get_config_val(args, &val))
				goto config_err;

			if (val == 0)
				clear_attr_available(&share->config.attr);
			else
				set_attr_available(&share->config.attr);
			break;
		case Opt_browsable:
			if (!share || cifsd_get_config_val(args, &val))
				goto config_err;
			if (val == 0)
				clear_attr_browsable(&share->config.attr);
			else
				set_attr_browsable(&share->config.attr);
			break;
		case Opt_writeable:
			if (!share ||
				cifsd_get_config_val(args, &share->writeable))
				goto config_err;
			break;
		case Opt_guestok:
			if (!share || cifsd_get_config_val(args, &val))
				goto config_err;
			if (val == 1)
				set_attr_guestok(&share->config.attr);
			else
				clear_attr_guestok(&share->config.attr);
			break;
		case Opt_guestonly:
			if (!share || cifsd_get_config_val(args, &val))
				goto config_err;
			if (val == 1)
				set_attr_guestonly(&share->config.attr);
			else
				clear_attr_guestonly(&share->config.attr);
			break;
		case Opt_oplocks:
			if (!share || cifsd_get_config_val(args, &val))
				goto config_err;
			if (val == 0)
				clear_attr_oplocks(&share->config.attr);
			else
				set_attr_oplocks(&share->config.attr);
			break;
		case Opt_maxcon:
			string = match_strdup(args);
			if (string == NULL)
				goto out_nomem;
			if (!share || kstrtouint(string, 10, &val)) {
				kfree(string);
				goto config_err;
			}
			share->config.max_connections = val;
			kfree(string);
			break;
		case Opt_comment:
			if (!share || cifsd_get_config_str(args,
						&share->config.comment))
				goto out_nomem;
			break;
		case Opt_allowhost:
		case Opt_hostallow:
			if (!share || cifsd_get_config_str(args,
						&share->config.allow_hosts))
				goto out_nomem;
			break;
		case Opt_denyhost:
		case Opt_hostdeny:
			if (!share || cifsd_get_config_str(args,
						&share->config.deny_hosts))
				goto out_nomem;
			break;
		case Opt_validusers:
			if (!share || cifsd_get_config_str(args,
						&share->config.valid_users))
				goto out_nomem;
			break;
		case Opt_invalidusers:
			if (!share || cifsd_get_config_str(args,
						&share->config.invalid_users))
				goto out_nomem;
			break;
		case Opt_path:
			if (!share || cifsd_get_config_str(args,
						&share->path))
				goto out_nomem;
			if (new_share && !__add_share(share, share->sharename,
						share->path))
				cifsd_err("share add error %s:%s\n",
						share->sharename, share->path);
			break;
		case Opt_readlist:
			if (!share || cifsd_get_config_str(args,
						&share->config.read_list))
				goto out_nomem;
			break;
		case Opt_readonly:
			if (!share || cifsd_get_config_val(args, &val))
				goto config_err;
			if (val == 1)
				set_attr_readonly(&share->config.attr);
			else
				clear_attr_readonly(&share->config.attr);
			break;
		case Opt_writeok:
			if (!share || cifsd_get_config_val(args, &val))
				goto config_err;
			if (val == 1)
				set_attr_writeok(&share->config.attr);
			else
				clear_attr_writeok(&share->config.attr);
			break;
		case Opt_writelist:
			if (!share || cifsd_get_config_str(args,
						&share->config.write_list))
				goto out_nomem;
			break;
		case Opt_store_dos_attr:
			if (!share || cifsd_get_config_val(args, &val))
				goto config_err;
			if (val == 1)
				set_attr_store_dos(&share->config.attr);
			else
				clear_attr_store_dos(&share->config.attr);
			break;
		default:
			cifsd_err("[%s] not supported\n", data);
			break;
		}
	}

	kfree(configdata_copy);
	return 0;

out_nomem:
	cifsd_err("Could not allocate buffer\n");

config_err:
	if (new_share && share)
		cleanup_bad_share(share);
	kfree(configdata_copy);
	return 1;
}

/**
 * show_share_config() - show cifsd share config
 * @buf:	destination buffer for config info
 * @offset:	offset in destination buffer
 * @share:	show config info of this share
 *
 * Return:      output buffer length
 */
static ssize_t show_share_config(char *buf, int offset,
		struct cifsd_share *share)
{
	int cum = offset;
	int ret = 0;
	int limit = PAGE_SIZE - offset;

	if (cum < limit) {
		ret = snprintf(buf + cum, limit - cum, "[%s]\n",
				share->sharename);
		if (ret < 0)
			return cum;
		cum += ret;
	}

	if (cum < limit && share->config.comment) {
		ret = snprintf(buf + cum, limit - cum, "\tcomment = %s\n",
				share->config.comment);
		if (ret < 0)
			return cum;
		cum += ret;
	}

	if (cum < limit) {
		ret = snprintf(buf + cum, limit - cum, "\tpath = %s\n",
				share->path);
		if (ret < 0)
			return cum;
		cum += ret;
	}

	if (cum < limit && share->config.allow_hosts) {
		ret = snprintf(buf + cum, limit - cum,
				"\tallow hosts = %s\n",
				share->config.allow_hosts);
		if (ret < 0)
			return cum;
		cum += ret;
	}

	if (cum < limit && share->config.deny_hosts) {
		ret = snprintf(buf + cum, limit - cum,
				"\tdeny hosts = %s\n",
				share->config.deny_hosts);
		if (ret < 0)
			return cum;
		cum += ret;
	}

	if (cum < limit && share->config.invalid_users) {
		ret = snprintf(buf + cum, limit - cum,
				"\tinvalid users = %s\n",
				share->config.invalid_users);
		if (ret < 0)
			return cum;
		cum += ret;
	}

	if (cum < limit && share->config.read_list) {
		ret = snprintf(buf + cum, limit - cum,
				"\tread list = %s\n",
				share->config.read_list);
		if (ret < 0)
			return cum;
		cum += ret;
	}

	if (cum < limit && share->config.valid_users) {
		ret = snprintf(buf + cum, limit - cum,
				"\tvalid users = %s\n",
				share->config.valid_users);
	}

	if (cum < limit) {
		ret = snprintf(buf + cum, limit - cum,
				"\tavailable = %d\n",
				get_attr_available(&share->config.attr));
		if (ret < 0)
			return cum;
		cum += ret;
	}

	if (cum < limit) {
		ret = snprintf(buf + cum, limit - cum,
				"\tbrowsable = %d\n",
				get_attr_browsable(&share->config.attr));
		if (ret < 0)
			return cum;
		cum += ret;
	}

	if (cum < limit) {
		ret = snprintf(buf + cum, limit - cum,
				"\tguest ok = %d\n",
				get_attr_guestok(&share->config.attr));
		if (ret < 0)
			return cum;
		cum += ret;
	}

	if (cum < limit) {
		ret = snprintf(buf + cum, limit - cum,
				"\tguest only = %d\n",
				get_attr_guestonly(&share->config.attr));
		if (ret < 0)
			return cum;
		cum += ret;
	}

	if (cum < limit) {
		ret = snprintf(buf + cum, limit - cum, "\toplocks = %d\n",
				get_attr_oplocks(&share->config.attr));
		if (ret < 0)
			return cum;
		cum += ret;
	}

	if (cum < limit) {
		ret = snprintf(buf + cum, limit - cum,
				"\twriteable = %d\n",
				share->writeable);
		if (ret < 0)
			return cum;
		cum += ret;
	}

	if (cum < limit) {
		ret = snprintf(buf + cum, limit - cum,
				"\tmax connections = %u\n",
				share->config.max_connections);
		if (ret < 0)
			return cum;
		cum += ret;
	}

	if (cum < limit && share->config.write_list) {
		ret = snprintf(buf + cum, limit - cum,
				"\twrite list = %s\n",
				share->config.write_list);
		if (ret < 0)
			return cum;
	}

	if (cum < limit) {
		ret = snprintf(buf + cum, limit - cum,
			"\tstore dos attributes = %d\n",
			get_attr_store_dos(&share->config.attr));
		if (ret < 0)
			return cum;
		cum += ret;
	}

	return cum;
}

/**
 * config_show() - show config setting
 * @kobj:	kobject of the modules
 * @kobj_attr:	kobject attribute of the modules
 * @buf:	buffer containing config setting
 *
 * Return:      output buffer length
 */
static ssize_t config_show(struct kobject *kobj,
			   struct kobj_attribute *kobj_attr,
			   char *buf)

{
	struct cifsd_share *share;
	struct list_head *tmp;
	int cum = 0;
	int ret = 0;

	list_for_each(tmp, &cifsd_share_list) {
		share = list_entry(tmp, struct cifsd_share, list);
		/* no need to show IPC$ share details */
		if (!share->path)
			continue;

		ret = show_share_config(buf, cum, share);
		if (ret < 0)
			return cum;
		cum += ret;
	}

	return cum;
}

/**
 * config_store() - update config settings
 * @kobj:	kobject of the modules
 * @kobj_attr:	kobject attribute of the modules
 * @buf:	buffer containing config setting
 * @len:	buf length of config setting
 *
 * Return:      config setting buf length
 */
static ssize_t config_store(struct kobject *kobj,
		struct kobj_attribute *kobj_attr,
		const char *buf, size_t len)
{
	if (cifsd_parse_share_options(buf))
		return -EINVAL;

	return len;
}

/**
 * show_server_stat() - show cifsd server stat
 * @buf:	destination buffer for stat info
 *
 * Return:      output buffer length
 */
static ssize_t show_server_stat(char *buf)
{
	struct cifsd_share *share;
	struct list_head *tmp;
	int count = 0, cum = 0, ret = 0, limit = PAGE_SIZE;

	ret = snprintf(buf+cum, limit - cum,
			"Server uptime secs = %ld\n",
			(jiffies - server_start_time)/HZ);
	if (ret < 0)
		return cum;
	cum += ret;

	list_for_each(tmp, &cifsd_share_list) {
		share = list_entry(tmp, struct cifsd_share, list);
		if (share->path)
			count++;
	}

	ret = snprintf(buf+cum, limit - cum,
			"Number of shares = %d\n", count);
	if (ret < 0)
		return cum;
	cum += ret;

	return cum;
}

/**
 * show_client_stat() - show cifsd client stat
 * @buf:	destination buffer for stat info
 * @sess:	session
 *
 * Return:      output buffer length
 */
static ssize_t show_client_stat(char *buf, struct connection *conn)
{
	int cum = 0, ret = 0, limit = PAGE_SIZE;

	ret = snprintf(buf+cum, limit - cum,
			"Connection type = SMB%s\n",
			conn->vals->version_string);
	if (ret < 0)
		return cum;
	cum += ret;

	ret = snprintf(buf+cum, limit - cum,
			"Current open files count = %d\n",
			conn->stats.open_files_count);
	if (ret < 0)
		return cum;
	cum += ret;

	ret = snprintf(buf+cum, limit - cum,
			"Outstanding Request = %d\n",
			atomic_read(&conn->req_running));
	if (ret < 0)
		return cum;
	cum += ret;

	ret = snprintf(buf+cum, limit - cum,
			"Total Requests Served = %d\n",
			conn->stats.request_served);
	if (ret < 0)
		return cum;
	cum += ret;

	if (cifsd_debug_enable) {
		ret = snprintf(buf+cum, limit - cum,
				"Avg. duration per request = %ld\n",
				conn->stats.avg_req_duration);
		if (ret < 0)
			return cum;
		cum += ret;

		ret = snprintf(buf+cum, limit - cum,
				"Max. duration request = %ld\n",
				conn->stats.max_timed_request);
		if (ret < 0)
			return cum;
		cum += ret;
	}

	return cum;
}

/**
 * stat_store() - update client stat IP
 * @kobj:	kobject of the modules
 * @kobj_attr:	kobject attribute of the modules
 * @buf:	buffer containing config setting
 * @len:	buf length of client stat IP setting
 *
 * Return:      client stat IP setting buf length
 */
static ssize_t stat_store(struct kobject *kobj,
		struct kobj_attribute *kobj_attr,
		const char *buf, size_t len)
{
	if (len > 1 && len < MAX_ADDRBUFLEN)
		strncpy(statIP, buf, len);

	return len;
}

/**
 * stat_show() - show cifsd stat
 * @kobj:	kobject of the modules
 * @kobj_attr:	kobject attribute of the modules
 * @buf:	buffer containing stat info
 *
 * Return:      output buffer length
 */
static ssize_t stat_show(struct kobject *kobj,
		struct kobj_attribute *kobj_attr,
		char *buf)
{
	struct list_head *tmp;
	struct connection *conn;
	int ret = 0;

	if (!strlen(statIP)) {
		ret = show_server_stat(buf);
		goto out;
	} else {
		int len1, len2;
		len1 = strlen(statIP);

		list_for_each(tmp, &cifsd_connection_list) {
			conn = list_entry(tmp, struct connection, list);
			len2 = strlen(conn->peeraddr);
			if (len1 == len2 && !strncmp(statIP,
				conn->peeraddr, len1)) {
				ret = show_client_stat(buf, conn);
				break;
			}
		}
		memset(statIP, 0, MAX_ADDRBUFLEN);
	}

out:
	return ret;
}

/* cifsd sysfs entries */
static ssize_t cifsd_attr_show(struct kobject *kobj,
				 struct attribute *attr, char *buf)
{
	struct kobj_attribute *kobj_attr =
			container_of(attr, struct kobj_attribute, attr);
	return kobj_attr->show(kobj, kobj_attr, buf);
}


static ssize_t cifsd_attr_store(struct kobject *kobj,
				  struct attribute *attr,
				  const char *buf, size_t len)
{
	struct kobj_attribute *kobj_attr =
			container_of(attr, struct kobj_attribute, attr);
	return kobj_attr->store(kobj, kobj_attr, buf, len);
}

#define SMB_ATTR(_name) \
	static struct kobj_attribute _name##_attr = \
__ATTR(_name, 0755, _name##_show, _name##_store)

SMB_ATTR(share);
SMB_ATTR(user);
SMB_ATTR(debug);
SMB_ATTR(caseless_search);
SMB_ATTR(config);
SMB_ATTR(stat);

static struct attribute *cifsd_sysfs_attrs[] = {
	&share_attr.attr,
	&user_attr.attr,
	&debug_attr.attr,
	&caseless_search_attr.attr,
	&config_attr.attr,
	&stat_attr.attr,
	NULL,
};

struct cifsd_sysfs_obj {
	struct kobject kobj;
	struct completion kobj_unregister;
};

static const struct sysfs_ops cifsd_attr_ops = {
	.show   = cifsd_attr_show,
	.store  = cifsd_attr_store,
};

static void cifsd_attr_release(struct kobject *kobj)
{
	complete(&sysobj->kobj_unregister);
}

struct kobj_type cifsdfs_ktype  = {
	.default_attrs  = cifsd_sysfs_attrs,
	.sysfs_ops      = &cifsd_attr_ops,
	.release        = cifsd_attr_release,
};

/**
 * cifsd_init_sysfs_parser() - init cifsd sysfs entries
 *
 * Return:      0 on success, otherwise error
 */
static int cifsd_init_sysfs_parser(void)
{
	int ret;
	sysobj = kzalloc(sizeof(struct cifsd_sysfs_obj), GFP_NOFS);
	if (!sysobj)
		return -ENOMEM;

	init_completion(&sysobj->kobj_unregister);
	ret = kobject_init_and_add(&sysobj->kobj, &cifsdfs_ktype,
							fs_kobj, "cifsd");
	if (ret)
		kfree(sysobj);

	return 0;
}

/**
 * cifsd_init_sysfs_parser() - cleanup cifsd sysfs entries at modules exit
 *
 * Return:      0 on success, otherwise error
 */
static void exit_sysfs_parser(void)
{
	kobject_put(&sysobj->kobj);
	wait_for_completion(&sysobj->kobj_unregister);
	kfree(sysobj);
}

/**
 * cifsd_add_IPC_share() - add share entry for IPC$ pipe with tid = 1
 *
 * Return:      0 on success, otherwise error
 */
static int cifsd_add_IPC_share(void)
{
	char *ipc;
	int len, rc;

	len = strlen(STR_IPC) + 1;

	ipc = kmalloc(len, GFP_KERNEL);
	if (!ipc)
		return -ENOMEM;

	memcpy(ipc, STR_IPC, len - 1);
	ipc[len - 1] = '\0';

	rc = add_share(ipc, NULL);
	if (rc)
		kfree(ipc);

	return rc;
}

/**
 * cifsd_init_global_params() - initialize default values of Server name
 *		Domain name
 *
 * Return:      0 on success, otherwise -ENOMEM
 */
int cifsd_init_global_params(void)
{
	int len;

	len = strlen(STR_SRV_NAME);
	server_string = kzalloc(len + 1, GFP_KERNEL);
	if (!server_string)
		return -ENOMEM;
	memcpy(server_string, STR_SRV_NAME, len);

	len = strlen(STR_WRKGRP);
	workgroup = kzalloc(len + 1, GFP_KERNEL);
	if (!workgroup) {
		kfree(server_string);
		return -ENOMEM;
	}
	memcpy(workgroup, STR_WRKGRP, len);

	len = strlen(TGT_Name);
	netbios_name = kzalloc(len + 1, GFP_KERNEL);
	if (!netbios_name) {
		kfree(server_string);
		kfree(workgroup);
		return -ENOMEM;
	}
	memcpy(netbios_name, TGT_Name, len);

	server_signing = 0;
	maptoguest = 0;
	server_min_pr = cifsd_min_protocol();
	server_max_pr = cifsd_max_protocol();
	return 0;
}

/**
 * cifsd_free_global_params() - free global parameters
 */
void cifsd_free_global_params(void)
{
	kfree(server_string);
	kfree(workgroup);
	kfree(guestAccountName);
	kfree(netbios_name);
}

/**
 * cifsd_export_init() - perform export related setup at module
 *			load time
 *
 * Return:      0 on success, otherwise error
 */
int cifsd_export_init(void)
{
	int rc;

	/* IPC share */
	rc = cifsd_add_IPC_share();
	if (rc)
		return rc;

	rc = cifsd_init_sysfs_parser();
	if (rc) {
		cifsd_share_free();
		return rc;
	}

	rc = cifsd_init_global_params();
	if (rc) {
		exit_sysfs_parser();
		cifsd_share_free();
		return rc;
	}

	return 0;
}

/**
 * cifsd_export_exit() - perform export related cleanup at module
 *			exit time
 */
void cifsd_export_exit(void)
{
	exit_sysfs_parser();
	cifsd_free_global_params();
	cifsd_user_free();
	cifsd_share_free();
}
