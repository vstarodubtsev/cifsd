/*
 *   fs/cifsd/cifsacl.c
 *
 *   Copyright (C) International Business Machines  Corp., 2007,2008
 *   Author(s): Steve French (sfrench@us.ibm.com)
 *   Modified by Namjae Jeon (namjae.jeon@protocolfreedom.org)
 *
 *   Contains the routines for mapping CIFS/NTFS ACLs
 *
 *   This library is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU Lesser General Public License as published
 *   by the Free Software Foundation; either version 2.1 of the License, or
 *   (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See
 *   the GNU Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public License
 *   along with this library; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */
#include <linux/errno.h>
#include <linux/keyctl.h>
#include <linux/key-type.h>
#include <keys/user-type.h>

#include "glob.h"
#include "smb1pdu.h"
#include "smb2pdu.h"
#include "cifsacl.h"

/* security id for everyone/world system group */
static const struct cifs_sid sid_everyone = {
	1, 1, {0, 0, 0, 0, 0, 1}, {0} };
/* security id for Authenticated Users system group */
static const struct cifs_sid sid_authusers = {
	1, 1, {0, 0, 0, 0, 0, 5}, {cpu_to_le32(11)} };
/* group users */
static const struct cifs_sid sid_user = {1, 2, {0, 0, 0, 0, 0, 5}, {} };

/* S-1-22-1 Unmapped Unix users */
static const struct cifs_sid sid_unix_users = {1, 1, {0, 0, 0, 0, 0, 22},
	{cpu_to_le32(1), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} };

/* S-1-22-2 Unmapped Unix groups */
static const struct cifs_sid sid_unix_groups = { 1, 1, {0, 0, 0, 0, 0, 22},
	{cpu_to_le32(2), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} };

/*
 * See http://technet.microsoft.com/en-us/library/hh509017(v=ws.10).aspx
 */

/* S-1-5-88 MS NFS and Apple style UID/GID/mode */

/* S-1-5-88-1 Unix uid */
static const struct cifs_sid sid_unix_NFS_users = { 1, 2, {0, 0, 0, 0, 0, 5},
	{cpu_to_le32(88),
		cpu_to_le32(1), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} };

/* S-1-5-88-2 Unix gid */
static const struct cifs_sid sid_unix_NFS_groups = { 1, 2, {0, 0, 0, 0, 0, 5},
	{cpu_to_le32(88),
		cpu_to_le32(2), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} };

/* S-1-5-88-3 Unix mode */
static const struct cifs_sid sid_unix_NFS_mode = { 1, 2, {0, 0, 0, 0, 0, 5},
	{cpu_to_le32(88),
		cpu_to_le32(3), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} };

static const struct cred *root_cred;

static int
cifs_idmap_key_instantiate(struct key *key, struct key_preparsed_payload *prep)
{
	char *payload;

	/*
	 * If the payload is less than or equal to the size of a pointer, then
	 * an allocation here is wasteful. Just copy the data directly to the
	 * payload.value union member instead.
	 *
	 * With this however, you must check the datalen before trying to
	 * dereference payload.data!
	 */
	if (prep->datalen <= sizeof(key->payload)) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 37)
		key->payload.data[0] = NULL;
		memcpy(&key->payload, prep->data, prep->datalen);
#else
		key->payload.value = 0;
		memcpy(&key->payload.value, prep->data, prep->datalen);
		key->datalen = prep->datalen;
#endif
	} else {
		payload = kmemdup(prep->data, prep->datalen, GFP_KERNEL);
		if (!payload)
			return -ENOMEM;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 37)
		key->payload.data[0] = payload;
#else
		key->payload.data = payload;
#endif
	}

	key->datalen = prep->datalen;
	return 0;
}

static inline void
cifs_idmap_key_destroy(struct key *key)
{
	if (key->datalen > sizeof(key->payload))
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 37)
		kfree(key->payload.data[0]);
#else
		kfree(key->payload.data);
#endif
}

static struct key_type cifsd_idmap_key_type = {
	.name        = "cifs.idmap",
	.instantiate = cifs_idmap_key_instantiate,
	.destroy     = cifs_idmap_key_destroy,
	.describe    = user_describe,
};

static char *
sid_to_key_str(struct cifs_sid *sidptr, unsigned int type)
{
	int i, len;
	unsigned int saval;
	char *sidstr, *strptr;
	unsigned long long id_auth_val;

	/* 3 bytes for prefix */
	sidstr = kmalloc(3 + SID_STRING_BASE_SIZE +
			(SID_STRING_SUBAUTH_SIZE * sidptr->num_subauth),
			GFP_KERNEL);
	if (!sidstr)
		return sidstr;

	strptr = sidstr;
	len = sprintf(strptr, "%cs:S-%hhu", type == SIDOWNER ? 'o' : 'g',
			sidptr->revision);
	strptr += len;

	/* The authority field is a single 48-bit number */
	id_auth_val = (unsigned long long)sidptr->authority[5];
	id_auth_val |= (unsigned long long)sidptr->authority[4] << 8;
	id_auth_val |= (unsigned long long)sidptr->authority[3] << 16;
	id_auth_val |= (unsigned long long)sidptr->authority[2] << 24;
	id_auth_val |= (unsigned long long)sidptr->authority[1] << 32;
	id_auth_val |= (unsigned long long)sidptr->authority[0] << 48;

	/*
	 * MS-DTYP states that if the authority is >= 2^32, then it should be
	 * expressed as a hex value.
	 */
	if (id_auth_val <= UINT_MAX)
		len = sprintf(strptr, "-%llu", id_auth_val);
	else
		len = sprintf(strptr, "-0x%llx", id_auth_val);

	strptr += len;

	for (i = 0; i < sidptr->num_subauth; ++i) {
		saval = le32_to_cpu(sidptr->sub_auth[i]);
		len = sprintf(strptr, "-%u", saval);
		strptr += len;
	}

	return sidstr;
}

/*
 * if the two SIDs (roughly equivalent to a UUID for a user or group) are
 * the same returns zero, if they do not match returns non-zero.
 */
int
compare_sids(const struct cifs_sid *ctsid, const struct cifs_sid *cwsid)
{
	int i;
	int num_subauth, num_sat, num_saw;

	if ((!ctsid) || (!cwsid))
		return 1;

	/* compare the revision */
	if (ctsid->revision != cwsid->revision) {
		if (ctsid->revision > cwsid->revision)
			return 1;
		else
			return -1;
	}

	/* compare all of the six auth values */
	for (i = 0; i < NUM_AUTHS; ++i) {
		if (ctsid->authority[i] != cwsid->authority[i]) {
			if (ctsid->authority[i] > cwsid->authority[i])
				return 1;
			else
				return -1;
		}
	}

	/* compare all of the subauth values if any */
	num_sat = ctsid->num_subauth;
	num_saw = cwsid->num_subauth;
	num_subauth = num_sat < num_saw ? num_sat : num_saw;
	if (num_subauth) {
		for (i = 0; i < num_subauth; ++i) {
			if (ctsid->sub_auth[i] != cwsid->sub_auth[i]) {
				if (le32_to_cpu(ctsid->sub_auth[i]) >
						le32_to_cpu(cwsid->sub_auth[i]))
					return 1;
				else
					return -1;
			}
		}
	}

	return 0; /* sids compare/match */
}

#if 0
static bool
is_well_known_sid(const struct cifs_sid *psid, uint32_t *puid, bool is_group)
{
	int i;
	int num_subauth;
	const struct cifs_sid *pwell_known_sid;

	if (!psid || (puid == NULL))
		return false;

	num_subauth = psid->num_subauth;

	/* check if Mac (or Windows NFS) vs. Samba format for Unix owner SID */
	if (num_subauth == 2) {
		if (is_group)
			pwell_known_sid = &sid_unix_groups;
		else
			pwell_known_sid = &sid_unix_users;
	} else if (num_subauth == 3) {
		if (is_group)
			pwell_known_sid = &sid_unix_NFS_groups;
		else
			pwell_known_sid = &sid_unix_NFS_users;
	} else
		return false;

	/* compare the revision */
	if (psid->revision != pwell_known_sid->revision)
		return false;

	/* compare all of the six auth values */
	for (i = 0; i < NUM_AUTHS; ++i) {
		if (psid->authority[i] != pwell_known_sid->authority[i]) {
			cifsd_err("auth %d did not match\n", i);
			return false;
		}
	}

	if (num_subauth == 2) {
		if (psid->sub_auth[0] != pwell_known_sid->sub_auth[0])
			return false;

		*puid = le32_to_cpu(psid->sub_auth[1]);
	} else /* 3 subauths, ie Windows/Mac style */ {
		*puid = le32_to_cpu(psid->sub_auth[0]);
		if ((psid->sub_auth[0] != pwell_known_sid->sub_auth[0]) ||
			(psid->sub_auth[1] != pwell_known_sid->sub_auth[1]))
			return false;

		*puid = le32_to_cpu(psid->sub_auth[2]);
	}

	cifsd_err("Unix UID %d returned from SID\n", *puid);
	return true; /* well known sid found, uid returned */
}
#endif

static void
cifs_copy_sid(struct cifs_sid *dst, const struct cifs_sid *src)
{
	int i;

	dst->revision = src->revision;
	dst->num_subauth = min_t(u8, src->num_subauth, SID_MAX_SUB_AUTHORITIES);
	for (i = 0; i < NUM_AUTHS; ++i)
		dst->authority[i] = src->authority[i];
	for (i = 0; i < dst->num_subauth; ++i)
		dst->sub_auth[i] = src->sub_auth[i];
}

/*
   Generate access flags to reflect permissions mode is the existing mode.
   This function is called for every ACE in the DACL whose SID matches
   with either owner or group or everyone.
 */
static void mode_to_access_flags(umode_t mode, umode_t bits_to_use,
		__u32 *pace_flags)
{
	/* reset access mask */
	*pace_flags = 0x0;

	/* bits to use are either S_IRWXU or S_IRWXG or S_IRWXO */
	mode &= bits_to_use;

	/* check for R/W/X UGO since we do not know whose flags
	   is this but we have cleared all the bits sans RWX for
	   either user or group or other as per bits_to_use */
	if (mode & S_IRUGO)
		*pace_flags |= SET_FILE_READ_RIGHTS;
	if (mode & S_IWUGO)
		*pace_flags |= SET_FILE_WRITE_RIGHTS;
	if (mode & S_IXUGO)
		*pace_flags |= SET_FILE_EXEC_RIGHTS;

	cifsd_err("mode: 0x%x, access flags now 0x%x\n",
			mode, *pace_flags);
	return;
}

static __u16 fill_ace_for_sid(struct cifs_ace *pntace,
		const struct cifs_sid *psid, __u64 nmode, umode_t bits)
{
	int i;
	__u16 size = 0;
	__u32 access_req = 0;

	pntace->type = ACCESS_ALLOWED;
	pntace->flags = 0x0;
	mode_to_access_flags(nmode, bits, &access_req);
	if (!access_req)
		access_req = SET_MINIMUM_RIGHTS;
	pntace->access_req = cpu_to_le32(access_req);

	pntace->sid.revision = psid->revision;
	pntace->sid.num_subauth = psid->num_subauth;
	for (i = 0; i < NUM_AUTHS; i++)
		pntace->sid.authority[i] = psid->authority[i];
	for (i = 0; i < psid->num_subauth; i++)
		pntace->sid.sub_auth[i] = psid->sub_auth[i];

	size = 1 + 1 + 2 + 4 + 1 + 1 + 6 + (psid->num_subauth * 4);
	pntace->size = cpu_to_le16(size);

	return size;
}

static int set_chmod_dacl(struct cifs_acl *pndacl, struct cifs_sid *pownersid,
		struct cifs_sid *pgrpsid, __u64 nmode)
{
	u16 size = 0;
	struct cifs_acl *pnndacl;

	pnndacl = (struct cifs_acl *)((char *)pndacl + sizeof(struct cifs_acl));

	size += fill_ace_for_sid((struct cifs_ace *) ((char *)pnndacl + size),
			pownersid, nmode, S_IRWXU);
	size += fill_ace_for_sid((struct cifs_ace *)((char *)pnndacl + size),
			pgrpsid, nmode, S_IRWXG);
	size += fill_ace_for_sid((struct cifs_ace *)((char *)pnndacl + size),
			&sid_everyone, nmode, S_IRWXO);

	pndacl->size = cpu_to_le16(size + sizeof(struct cifs_acl));
	pndacl->num_aces = cpu_to_le32(3);

	return 0;
}

int parse_sid(struct cifs_sid *psid, char *end_of_acl)
{
	/* BB need to add parm so we can store the SID BB */

	/* validate that we do not go past end of ACL - sid must be at least 8
	   bytes long (assuming no sub-auths - e.g. the null SID */
	if (end_of_acl < (char *)psid + 8) {
		cifsd_err("ACL too small to parse SID %p\n", psid);
		return -EINVAL;
	}

	if (psid->num_subauth) {
		int i;
		cifsd_err("SID revision %d num_auth %d\n",
				psid->revision, psid->num_subauth);

		for (i = 0; i < psid->num_subauth; i++) {
			cifsd_err("SID sub_auth[%d]: 0x%x\n",
					i, le32_to_cpu(psid->sub_auth[i]));
		}

		/* BB add length check to make sure that we do not have huge
		   num auths and therefore go off the end */
		cifsd_err("RID 0x%x\n",
			le32_to_cpu(psid->sub_auth[psid->num_subauth-1]));
	}

	return 0;
}

void dump_ace(struct cifs_ace *pace, char *end_of_acl)
{
	int num_subauth;

	/* validate that we do not go past end of acl */

	if (le16_to_cpu(pace->size) < 16) {
		cifsd_err("ACE too small %d\n", le16_to_cpu(pace->size));
		return;
	}

	if (end_of_acl < (char *)pace + le16_to_cpu(pace->size)) {
		cifsd_err("ACL too small to parse ACE\n");
		return;
	}

	num_subauth = pace->sid.num_subauth;
	if (num_subauth) {
		int i;

		cifsd_err("ACE revision %d num_auth %d type %d flags %d size %d\n",
			 pace->sid.revision, pace->sid.num_subauth, pace->type,
			 pace->flags, le16_to_cpu(pace->size));
		for (i = 0; i < num_subauth; ++i) {
			cifsd_err("ACE sub_auth[%d]: 0x%x\n",
				 i, le32_to_cpu(pace->sid.sub_auth[i]));
		}

		/* BB add length check to make sure that we do not have huge
			num auths and therefore go off the end */
	}

	return;
}

int get_dacl_size(struct cifs_acl *pdacl, char *end_of_acl)
{
	if (!pdacl)
		return 0;

	/* validate that we do not go past end of acl */
	if (end_of_acl < (char *)pdacl + le16_to_cpu(pdacl->size)) {
		cifsd_err("ACL too small to parse DACL\n");
		return 0;
	}

	return le16_to_cpu(pdacl->size);
}

int check_access_flags(__le32 access, __le16 type, __le32 desired_access)
{
	int rc;

	if (type == ACCESS_DENIED) {
		if (access & (FILE_GENERIC_ALL_LE | FILE_MAXIMAL_ACCESS_LE)) {
			rc = -EPERM;
			goto out;
		}

		if ((desired_access & FILE_READ_RIGHTS_LE) &
			(access & FILE_READ_RIGHTS_LE)) {
			cifsd_err("Not allow read right access(dacl access : 0x%x, desired access : 0x%x)\n",
				access & FILE_READ_RIGHTS_LE,
				desired_access & FILE_READ_RIGHTS_LE);
			rc = -EPERM;
			goto out;
		}

		if ((desired_access & FILE_WRITE_RIGHTS_LE) &
			(access & FILE_WRITE_RIGHTS_LE)) {
			cifsd_err("Not allow write right access(dacl access : 0x%x, desired access : 0x%x)\n",
				access & FILE_WRITE_RIGHTS_LE,
				desired_access & FILE_WRITE_RIGHTS_LE);
			rc = -EPERM;
			goto out;
		}

		if ((desired_access & FILE_GENERIC_READ_LE) &
			(access & FILE_GENERIC_READ_LE)) {
			rc = -EPERM;
			cifsd_err("Not allow generic read access(dacl access : 0x%x, desired access : 0x%x)\n",
				access & FILE_GENERIC_READ_LE,
				desired_access & FILE_GENERIC_READ_LE);
			goto out;
		}

		if ((desired_access & FILE_GENERIC_WRITE_LE) &
			(access & FILE_GENERIC_WRITE_LE)) {
			cifsd_err("Not allow generic write access(dacl access : 0x%x, desired access : 0x%x)\n",
				access & FILE_GENERIC_WRITE_LE,
				desired_access & FILE_GENERIC_WRITE_LE);
			rc = -EPERM;
		}
	} else if (type == ACCESS_ALLOWED) {
		if (access & (FILE_GENERIC_ALL_LE | FILE_MAXIMAL_ACCESS_LE)) {
			rc = -EPERM;
			goto out;
		}

		if (!(access & (desired_access & FILE_GENERIC_WRITE_LE))) {
			cifsd_err("Not allow generic write access(dacl access : 0x%x, desired access : 0x%x)\n",
				access & FILE_GENERIC_WRITE_LE,
				desired_access & FILE_GENERIC_WRITE_LE);
			rc = -EPERM;
			goto out;
		}

		if (!(access & (desired_access & FILE_GENERIC_READ_LE))) {
			cifsd_err("Not allow generic read access(dacl access : 0x%x, desired access : 0x%x)\n",
				access & FILE_READ_RIGHTS_LE,
				desired_access & FILE_READ_RIGHTS_LE);
			rc = -EPERM;
			goto out;
		}

		if ((access & FILE_READ_RIGHTS_LE) !=
			(desired_access & FILE_READ_RIGHTS_LE)) {
			cifsd_err("Not allow read right access(dacl access : 0x%x, desired access : 0x%x)\n",
				access & FILE_READ_RIGHTS_LE,
				desired_access & FILE_READ_RIGHTS_LE);
			rc = -EPERM;
			goto out;
		}

		if ((access & FILE_WRITE_RIGHTS_LE) !=
			(desired_access & FILE_WRITE_RIGHTS_LE)) {
			cifsd_err("Not allow write right access(dacl access : 0x%x, desired access : 0x%x)\n",
				access & FILE_WRITE_RIGHTS_LE,
				desired_access & FILE_WRITE_RIGHTS_LE);
			rc = -EPERM;
			goto out;
		}
	} else {
		cifsd_err("unknown access control type %d\n", type);
	}

out:
	return rc;
}

int check_permission_dacl(struct cifs_acl *pdacl, char *end_of_acl,
	struct cifs_sid *pownersid, struct cifs_sid *pgrpsid, __le32 daccess)
{
	int i, rc;
	int num_aces = 0;
	int acl_size;
	char *acl_base;
	struct cifs_ace **ppace;

	cifsd_err("DACL revision %d size %d num aces %d\n",
		 le16_to_cpu(pdacl->revision), le16_to_cpu(pdacl->size),
		 le32_to_cpu(pdacl->num_aces));

	acl_base = (char *)pdacl;
	acl_size = sizeof(struct cifs_acl);

	num_aces = le32_to_cpu(pdacl->num_aces);
	rc = -EPERM;
	/* empty DACL doen't allow any access if num_aces is 0 */
	if (num_aces > 0) {
		if (num_aces > ULONG_MAX / sizeof(struct cifs_ace *))
			return rc;
		ppace = kmalloc(num_aces * sizeof(struct cifs_ace *),
				GFP_KERNEL);
		if (!ppace)
			return rc;

		for (i = 0; i < num_aces; ++i) {
			//dump_ace(ppace[i], end_of_acl);

			ppace[i] = (struct cifs_ace *) (acl_base + acl_size);
			if (compare_sids(&(ppace[i]->sid), pownersid) == 0) {
				rc = check_access_flags(ppace[i]->access_req,
					ppace[i]->type, daccess);
				if (rc < 0)
					break;
			}

			acl_base = (char *)ppace[i];
			acl_size = le16_to_cpu(ppace[i]->size);
		}

		kfree(ppace);
	}

	return rc;
}

int id_to_sid(unsigned int cid, uint sidtype, struct cifs_sid *ssid)
{
	int rc;
	struct key *sidkey;
	struct cifs_sid *ksid;
	unsigned int ksid_size;
	char desc[3 + 10 + 1]; /* 3 byte prefix + 10 bytes for value + NULL */
	const struct cred *saved_cred;

	rc = snprintf(desc, sizeof(desc), "%ci:%u",
			sidtype == SIDOWNER ? 'o' : 'g', cid);
	if (rc >= sizeof(desc))
		return -EINVAL;

	rc = 0;
	saved_cred = override_creds(root_cred);
	sidkey = request_key(&cifsd_idmap_key_type, desc, "");
	if (IS_ERR(sidkey)) {
		rc = -EINVAL;
		cifsd_err("%s: Can't map %cid %u to a SID\n",
				__func__, sidtype == SIDOWNER ? 'u' : 'g', cid);
		goto out_revert_creds;
	} else if (sidkey->datalen < CIFS_SID_BASE_SIZE) {
		rc = -EIO;
		cifsd_err("%s: Downcall contained malformed key (datalen=%hu)\n",
				__func__, sidkey->datalen);
		goto invalidate_key;
	}

	/*
	 * A sid is usually too large to be embedded in payload.value, but if
	 * there are no subauthorities and the host has 8-byte pointers, then
	 * it could be.
	 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 37)
	ksid = sidkey->datalen <= sizeof(sidkey->payload) ?
		(struct cifs_sid *)&sidkey->payload :
		(struct cifs_sid *)sidkey->payload.data[0];
#else
	ksid = sidkey->datalen <= sizeof(sidkey->payload) ?
		(struct cifs_sid *)&sidkey->payload.value :
		(struct cifs_sid *)sidkey->payload.data;
#endif

	ksid_size = CIFS_SID_BASE_SIZE + (ksid->num_subauth * sizeof(__le32));
	if (ksid_size > sidkey->datalen) {
		rc = -EIO;
		cifsd_err("%s: Downcall contained malformed key (datalen=%hu, ksid_size=%u)\n",
				__func__, sidkey->datalen, ksid_size);
		goto invalidate_key;
	}

	cifs_copy_sid(ssid, ksid);
out_key_put:
	key_put(sidkey);
out_revert_creds:
	revert_creds(saved_cred);
	return rc;

invalidate_key:
	key_invalidate(sidkey);
	goto out_key_put;
}

int sid_to_id(struct cifs_sid *psid, struct cifsd_fattr *fattr, uint sidtype)
{
	int rc;
	struct key *sidkey;
	char *sidstr;
	const struct cred *saved_cred;
	kuid_t fuid = INVALID_UID;
	kgid_t fgid = INVALID_GID;

	/*
	 * If we have too many subauthorities, then something is really wrong.
	 * Just return an error.
	 */
	if (unlikely(psid->num_subauth > SID_MAX_SUB_AUTHORITIES)) {
		cifsd_err("%s: %u subauthorities is too many!\n",
				__func__, psid->num_subauth);
		return -EIO;
	}

	sidstr = sid_to_key_str(psid, sidtype);
	if (!sidstr)
		return -ENOMEM;

	saved_cred = override_creds(root_cred);
	sidkey = request_key(&cifsd_idmap_key_type, sidstr, "");
	if (IS_ERR(sidkey)) {
		rc = -EINVAL;
		cifsd_err("%s: Can't map SID %s to a %cid\n",
			__func__, sidstr, sidtype == SIDOWNER ? 'u' : 'g');
		goto out_revert_creds;
	}

	/*
	 * FIXME: Here we assume that uid_t and gid_t are same size. It's
	 * probably a safe assumption but might be better to check based on
	 * sidtype.
	 */
	BUILD_BUG_ON(sizeof(uid_t) != sizeof(gid_t));
	if (sidkey->datalen != sizeof(uid_t)) {
		rc = -EIO;
		cifsd_err("%s: Downcall contained malformed key (datalen=%hu)\n",
				__func__, sidkey->datalen);
		key_invalidate(sidkey);
		goto out_key_put;
	}

	if (sidtype == SIDOWNER) {
		kuid_t uid;
		uid_t id;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 37)
		memcpy(&id, &sidkey->payload.data[0], sizeof(uid_t));
#else
		memcpy(&id, &sidkey->payload.value, sizeof(uid_t));
#endif
		uid = make_kuid(&init_user_ns, id);
		if (uid_valid(uid))
			fuid = uid;
	} else {
		kgid_t gid;
		gid_t id;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 37)
		memcpy(&id, &sidkey->payload.data[0], sizeof(gid_t));
#else
		memcpy(&id, &sidkey->payload.value, sizeof(gid_t));
#endif
		gid = make_kgid(&init_user_ns, id);
		if (gid_valid(gid))
			fgid = gid;
	}

out_key_put:
	key_put(sidkey);
out_revert_creds:
	revert_creds(saved_cred);
	kfree(sidstr);

	/*
	 * Note that we return 0 here unconditionally. If the mapping
	 * fails then we just fall back to using the mnt_uid/mnt_gid.
	 */
	if (sidtype == SIDOWNER)
		fattr->cf_uid = fuid;
	else
		fattr->cf_gid = fgid;
	return 0;
}

void cifsd_fattr_to_inode(struct inode *inode, struct cifsd_fattr *fattr)
{

	spin_lock(&inode->i_lock);
	inode->i_uid = fattr->cf_uid;
	inode->i_gid = fattr->cf_gid;

	inode->i_mode = fattr->cf_mode;

	spin_unlock(&inode->i_lock);
	mark_inode_dirty(inode);
}

/*
   change posix mode to reflect permissions
   pmode is the existing mode (we only want to overwrite part of this
   bits to set can be: S_IRWXU, S_IRWXG or S_IRWXO ie 00700 or 00070 or 00007
 */
static void access_flags_to_mode(__le32 ace_flags, int type, umode_t *pmode,
		umode_t *pbits_to_set)
{
	__u32 flags = le32_to_cpu(ace_flags);
	/* the order of ACEs is important.  The canonical order is to begin with
	   DENY entries followed by ALLOW, otherwise an allow entry could be
	   encountered first, making the subsequent deny entry like "dead code"
	   which would be superfluous since Windows stops when a match is made
	   for the operation you are trying to perform for your user */

	/* For deny ACEs we change the mask so that subsequent allow access
	   control entries do not turn on the bits we are denying */
	if (type == ACCESS_DENIED) {
		if (flags & GENERIC_ALL)
			*pbits_to_set &= ~S_IRWXUGO;

		if ((flags & GENERIC_WRITE) ||
			((flags & FILE_WRITE_RIGHTS) == FILE_WRITE_RIGHTS))
			*pbits_to_set &= ~S_IWUGO;
		if ((flags & GENERIC_READ) ||
			((flags & FILE_READ_RIGHTS) == FILE_READ_RIGHTS))
			*pbits_to_set &= ~S_IRUGO;
		if ((flags & GENERIC_EXECUTE) ||
			((flags & FILE_EXEC_RIGHTS) == FILE_EXEC_RIGHTS))
			*pbits_to_set &= ~S_IXUGO;
		return;
	} else if (type != ACCESS_ALLOWED) {
		cifsd_err("unknown access control type %d\n", type);
		return;
	}
	/* else ACCESS_ALLOWED type */

	if (flags & GENERIC_ALL) {
		*pmode |= (S_IRWXUGO & (*pbits_to_set));
		cifsd_err("all perms\n");
		return;
	}
	if ((flags & GENERIC_WRITE) ||
		((flags & FILE_WRITE_RIGHTS) == FILE_WRITE_RIGHTS))
		*pmode |= (S_IWUGO & (*pbits_to_set));
	if ((flags & GENERIC_READ) ||
		((flags & FILE_READ_RIGHTS) == FILE_READ_RIGHTS))
		*pmode |= (S_IRUGO & (*pbits_to_set));
	if ((flags & GENERIC_EXECUTE) ||
		((flags & FILE_EXEC_RIGHTS) == FILE_EXEC_RIGHTS))
		*pmode |= (S_IXUGO & (*pbits_to_set));

	cifsd_err("access flags 0x%x mode now 0x%x\n", flags, *pmode);
	return;
}

static void parse_dacl(struct cifs_acl *pdacl, char *end_of_acl,
		struct cifs_sid *pownersid, struct cifs_sid *pgrpsid,
		struct cifsd_fattr *fattr)
{
	int i;
	int num_aces = 0;
	int acl_size;
	char *acl_base;
	struct cifs_ace **ppace;

	/* BB need to add parm so we can store the SID BB */

	if (!pdacl) {
		/* no DACL in the security descriptor, set
		   all the permissions for user/group/other */
		fattr->cf_mode |= S_IRWXUGO;
		return;
	}

	/* validate that we do not go past end of acl */
	if (end_of_acl < (char *)pdacl + le16_to_cpu(pdacl->size)) {
		cifsd_err("ACL too small to parse DACL\n");
		return;
	}

	cifsd_err("DACL revision %d size %d num aces %d\n",
			le16_to_cpu(pdacl->revision), le16_to_cpu(pdacl->size),
			le32_to_cpu(pdacl->num_aces));

	/* reset rwx permissions for user/group/other.
	   Also, if num_aces is 0 i.e. DACL has no ACEs,
	   user/group/other have no permissions */
	fattr->cf_mode &= ~(S_IRWXUGO);

	acl_base = (char *)pdacl;
	acl_size = sizeof(struct cifs_acl);

	num_aces = le32_to_cpu(pdacl->num_aces);
	if (num_aces > 0) {
		umode_t user_mask = S_IRWXU;
		umode_t group_mask = S_IRWXG;
		umode_t other_mask = S_IRWXU | S_IRWXG | S_IRWXO;

		if (num_aces > ULONG_MAX / sizeof(struct cifs_ace *))
			return;
		ppace = kmalloc(num_aces * sizeof(struct cifs_ace *),
				GFP_KERNEL);
		if (!ppace)
			return;

		for (i = 0; i < num_aces; ++i) {
			ppace[i] = (struct cifs_ace *) (acl_base + acl_size);
	//		dump_ace(ppace[i], end_of_acl);
			if (compare_sids(&(ppace[i]->sid), pownersid) == 0)
				access_flags_to_mode(ppace[i]->access_req,
					ppace[i]->type,
					&fattr->cf_mode,
					&user_mask);
			if (compare_sids(&(ppace[i]->sid), pgrpsid) == 0)
				access_flags_to_mode(ppace[i]->access_req,
					ppace[i]->type,
					&fattr->cf_mode,
					&group_mask);
			if (compare_sids(&(ppace[i]->sid), &sid_everyone) == 0)
				access_flags_to_mode(ppace[i]->access_req,
					ppace[i]->type,
					&fattr->cf_mode,
					&other_mask);
			if (compare_sids(&(ppace[i]->sid), &sid_authusers) == 0)
				access_flags_to_mode(ppace[i]->access_req,
					ppace[i]->type,
					&fattr->cf_mode,
					&other_mask);


			/* memcpy((void *)(&(cifscred->aces[i])),
				(void *)ppace[i],
				sizeof(struct cifs_ace)); */

			acl_base = (char *)ppace[i];
			acl_size = le16_to_cpu(ppace[i]->size);
		}

		kfree(ppace);
	}

	return;
}

int parse_sec_desc(struct cifs_ntsd *pntsd, int acl_len,
	struct cifsd_fattr *fattr)
{
	struct cifs_sid *owner_sid_ptr, *group_sid_ptr;
	struct cifs_acl *dacl_ptr = NULL;  /* no need for SACL ptr */
	__u32 dacloffset;
	char *end_of_acl;
	int rc = 0;

	owner_sid_ptr = (struct cifs_sid *)((char *)pntsd +
		le32_to_cpu(pntsd->osidoffset));
	group_sid_ptr = (struct cifs_sid *)((char *)pntsd +
		le32_to_cpu(pntsd->gsidoffset));
	dacloffset = le32_to_cpu(pntsd->dacloffset);
	dacl_ptr = (struct cifs_acl *)((char *)pntsd + dacloffset);
	cifsd_err("revision %d type 0x%x ooffset 0x%x goffset 0x%x sacloffset 0x%x dacloffset 0x%x\n",
		pntsd->revision, pntsd->type, le32_to_cpu(pntsd->osidoffset),
		le32_to_cpu(pntsd->gsidoffset),
		le32_to_cpu(pntsd->sacloffset), dacloffset);
	end_of_acl = ((char *)pntsd) + acl_len;

	rc = parse_sid(owner_sid_ptr, end_of_acl);
	if (rc) {
		cifsd_err("%s: Error %d parsing Owner SID\n", __func__,
				rc);
		return rc;
	}

	rc = sid_to_id(owner_sid_ptr, fattr, SIDOWNER);
	if (rc) {
		cifsd_err("%s: Error %d mapping Owner SID to uid\n",
				__func__, rc);
		return rc;
	}

	rc = parse_sid(group_sid_ptr, end_of_acl);
	if (rc) {
		cifsd_err("%s: Error %d mapping Owner SID to gid\n",
				__func__, rc);
		return rc;
	}

	rc = sid_to_id(group_sid_ptr, fattr, SIDGROUP);
	if (rc) {
		cifsd_err("%s: Error %d mapping Group SID to gid\n",
				__func__, rc);
		return rc;
	}

	if (dacloffset) {
		parse_dacl(dacl_ptr, end_of_acl, owner_sid_ptr,
			   group_sid_ptr, fattr);
	}

	return rc;
}

int build_sec_desc(struct cifs_ntsd *pntsd, int addition_info,
	struct inode *inode)
{
	struct cifs_sid *owner_sid_ptr = NULL, *group_sid_ptr = NULL;
	struct cifs_sid *nowner_sid_ptr, *ngroup_sid_ptr;
	int offset, rc = 0;

	pntsd->revision = SD_REVISION;
	pntsd->type = SELF_RELATIVE;

	offset = sizeof(struct cifs_ntsd);

	if (addition_info & OWNER_SECINFO) {
		kuid_t uid;

		uid = inode->i_uid;
		if (uid_valid(uid)) {
			uid_t id;

			pntsd->osidoffset = cpu_to_le32(offset);
			owner_sid_ptr = (struct cifs_sid *)((char *)pntsd +
					le32_to_cpu(pntsd->osidoffset));
			nowner_sid_ptr = kmalloc(sizeof(struct cifs_sid),
					GFP_KERNEL);
			if (!nowner_sid_ptr)
				return -ENOMEM;
			id = from_kuid(&init_user_ns, uid);
			rc = id_to_sid(id, SIDOWNER, nowner_sid_ptr);
			if (rc) {
				cifsd_err("%s: Mapping error %d for owner id %d\n",
					__func__, rc, id);
				kfree(nowner_sid_ptr);
				return rc;
			}
			cifs_copy_sid(owner_sid_ptr, nowner_sid_ptr);
			kfree(nowner_sid_ptr);
			pntsd->type |= OWNER_DEFAULTED;
		}

		offset += sizeof(struct cifs_sid);
	}

	if (addition_info & GROUP_SECINFO) {
		kgid_t gid;

		gid = inode->i_gid;
		if (gid_valid(gid)) { /* chgrp */
			gid_t id;

			pntsd->gsidoffset = cpu_to_le32(offset);
			group_sid_ptr = (struct cifs_sid *)((char *)pntsd +
					le32_to_cpu(pntsd->gsidoffset));
			ngroup_sid_ptr = kmalloc(sizeof(struct cifs_sid),
					GFP_KERNEL);
			if (!ngroup_sid_ptr)
				return -ENOMEM;
			id = from_kgid(&init_user_ns, gid);
			rc = id_to_sid(id, SIDGROUP, ngroup_sid_ptr);
			if (rc) {
				cifsd_err("%s: Mapping error %d for group id %d\n",
						__func__, rc, id);
				kfree(ngroup_sid_ptr);
				return rc;
			}
			cifs_copy_sid(group_sid_ptr, ngroup_sid_ptr);
			kfree(ngroup_sid_ptr);
			pntsd->type |= GROUP_DEFAULTED;
		}
		offset += sizeof(struct cifs_sid);
	}

	if (addition_info & DACL_SECINFO) {
		struct cifs_acl *dacl_ptr = NULL;  /* no need for SACL ptr */
		struct cifs_acl *ndacl_ptr = NULL; /* no need for SACL ptr */
		__u32 dacloffset;
		__u32 ndacloffset;
		umode_t nmode = inode->i_mode;

		pntsd->dacloffset = cpu_to_le32(offset);
		dacloffset = le32_to_cpu(pntsd->dacloffset);
		dacl_ptr = (struct cifs_acl *)((char *)pntsd + dacloffset);
		ndacloffset = sizeof(struct cifs_ntsd);
		ndacl_ptr = (struct cifs_acl *)((char *)pntsd + ndacloffset);
		ndacl_ptr->revision = dacl_ptr->revision;
		ndacl_ptr->size = 0;
		ndacl_ptr->num_aces = 0;
		pntsd->type |= DACL_PRESENT;

		rc = set_chmod_dacl(ndacl_ptr, owner_sid_ptr, group_sid_ptr,
				nmode);
		offset += le16_to_cpu(ndacl_ptr->size);
	}

	return offset;
}

int init_cifsd_idmap(void)
{
	struct cred *cred;
	struct key *keyring;
	int ret;

	cifsd_err("Registering the %s key type\n",
		cifsd_idmap_key_type.name);

	/* create an override credential set with a special thread keyring in
	 * which requests are cached
	 *
	 * this is used to prevent malicious redirections from being installed
	 * with add_key().
	 */
	cred = prepare_kernel_cred(NULL);
	if (!cred)
		return -ENOMEM;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 7, 0)
	keyring = keyring_alloc(".cifs_idmap",
			GLOBAL_ROOT_UID, GLOBAL_ROOT_GID, cred,
			(KEY_POS_ALL & ~KEY_POS_SETATTR) |
			KEY_USR_VIEW | KEY_USR_READ,
			KEY_ALLOC_NOT_IN_QUOTA, NULL, NULL);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
	keyring = keyring_alloc(".cifs_idmap",
			GLOBAL_ROOT_UID, GLOBAL_ROOT_GID, cred,
			(KEY_POS_ALL & ~KEY_POS_SETATTR) |
			KEY_USR_VIEW | KEY_USR_READ,
			KEY_ALLOC_NOT_IN_QUOTA, NULL);
#else
	keyring = keyring_alloc(".cifs_idmap",
			0, 0, cred,
			KEY_ALLOC_NOT_IN_QUOTA, NULL);
#endif
	if (IS_ERR(keyring)) {
		ret = PTR_ERR(keyring);
		goto failed_put_cred;
	}

	ret = register_key_type(&cifsd_idmap_key_type);
	if (ret < 0)
		goto failed_put_key;

	/* instruct request_key() to use this special keyring as a cache for
	 * the results it looks up */
	set_bit(KEY_FLAG_ROOT_CAN_CLEAR, &keyring->flags);
	cred->thread_keyring = keyring;
	cred->jit_keyring = KEY_REQKEY_DEFL_THREAD_KEYRING;
	root_cred = cred;

	cifsd_err("cifs idmap keyring: %d\n", key_serial(keyring));
	return 0;

failed_put_key:
	key_put(keyring);
failed_put_cred:
	put_cred(cred);
	return ret;
}

void exit_cifsd_idmap(void)
{
	key_revoke(root_cred->thread_keyring);
	unregister_key_type(&cifsd_idmap_key_type);
	put_cred(root_cred);
	cifsd_err("Unregistered %s key type\n", cifsd_idmap_key_type.name);
}

