/*
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * ---------------------------------------
 */

/**
 * @addtogroup idmapper
 * @{
 */

/**
 * @file idmapper.c
 * @brief Id mapping functions
 */

#include "config.h"
#include "ganesha_rpc.h"
#include "nfs_core.h"
#include "nfs_tools.h"
#include <unistd.h>		/* for using gethostname */
#include <stdlib.h>		/* for using exit */
#include <strings.h>
#include <string.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#ifdef USE_NFSIDMAP
#include <nfsidmap.h>
#endif				/* USE_NFSIDMAP */
#ifdef _MSPAC_SUPPORT
#include <wbclient.h>
#endif
#include "common_utils.h"
#include "idmapper.h"

static struct gsh_buffdesc owner_domain;

/**
 * @brief Initialize the ID Mapper
 *
 * @return true on success, false on failure
 */

bool idmapper_init(void)
{
#ifdef USE_NFSIDMAP
	if (!nfs_param.nfsv4_param.use_getpwnam) {
		if (nfs4_init_name_mapping(nfs_param.nfsv4_param.idmapconf)
		    != 0) {
			return false;
		}
		owner_domain.addr = gsh_malloc(NFS4_MAX_DOMAIN_LEN + 1);
		if (owner_domain.addr == NULL)
			return false;

		if (nfs4_get_default_domain
		    (NULL, owner_domain.addr, NFS4_MAX_DOMAIN_LEN) != 0) {
			gsh_free(owner_domain.addr);
			return false;
		}
		owner_domain.len = strlen(owner_domain.addr);
	}
#endif				/* USE_NFSIDMAP */
	if (nfs_param.nfsv4_param.use_getpwnam) {
		owner_domain.addr = gsh_strdup(nfs_param.nfsv4_param
					       .domainname);
		if (owner_domain.addr == NULL)
			return false;

		owner_domain.len = strlen(nfs_param.nfsv4_param.domainname);
	}

	idmapper_cache_init();
	return true;
}

#ifdef _VID_MAPPING
bool vidmap_init(vidmap_parameter_t param)
{
	vidmap_cache_init();

	return true;
}                               /* vidmap_init */
#endif

/**
 * @brief Encode a UID or GID as a string
 *
 * @param[in,out] xdrs  XDR stream to which to encode
 * @param[in]     id    UID or GID
 * @param[in]     group True if this is a GID, false for a UID
 *
 * @retval true on success.
 * @retval false on failure.
 */

static bool xdr_encode_nfs4_princ(XDR *xdrs, uint32_t id, bool group)
{
	const struct gsh_buffdesc *found;
	uint32_t not_a_size_t;
	bool success = false;

#ifdef _VID_MAPPING
	if (group) {
		if (!vgid2gid(&id, &id)) {
			LogWarn(COMPONENT_IDMAPPER, "xdr_encode_nfs4_princ: Failed to map GID %"PRIu32, id);
		}
	}
	else {
		if (!vuid2uid(&id, &id)) {
			LogWarn(COMPONENT_IDMAPPER, "xdr_encode_nfs4_princ: Failed to map UID %"PRIu32, id);
		}
	}
#endif

	pthread_rwlock_rdlock(group ? &idmapper_group_lock :
			      &idmapper_user_lock);
	if (group)
		success = idmapper_lookup_by_gid(id, &found);
	else
		success = idmapper_lookup_by_uid(id, &found, NULL);

	if (likely(success)) {
		not_a_size_t = found->len;

		/* Fully qualified owners are always stored in the
		   hash table, no matter what our lookup method. */
		success =
		    inline_xdr_bytes(xdrs, (char **)&found->addr, &not_a_size_t,
				     UINT32_MAX);
		pthread_rwlock_unlock(group ? &idmapper_group_lock :
				      &idmapper_user_lock);
		return success;
	} else {
		pthread_rwlock_unlock(group ? &idmapper_group_lock :
				      &idmapper_user_lock);
		int rc;
		bool looked_up = false;
		char *namebuff =
		    alloca(nfs_param.nfsv4_param.
			   use_getpwnam ? (PWENT_MAX_LEN + owner_domain.len +
					   2) : (NFS4_MAX_DOMAIN_LEN + 2));
		struct gsh_buffdesc new_name = {
			.addr = namebuff
		};
		if (nfs_param.nfsv4_param.use_getpwnam) {
			char *cursor;
			bool nulled;

			if (group) {
				struct group g;
				struct group *gres;

				rc = getgrgid_r(id, &g, namebuff, PWENT_MAX_LEN,
						&gres);
				nulled = (gres == NULL);
			} else {
				struct passwd p;
				struct passwd *pres;

				rc = getpwuid_r(id, &p, namebuff, PWENT_MAX_LEN,
						&pres);
				nulled = (pres == NULL);
			}

			if ((rc == 0) && !nulled) {
				new_name.len = strlen(namebuff);
				cursor = namebuff + new_name.len;
				*(cursor++) = '@';
				++new_name.len;
				memcpy(cursor, owner_domain.addr,
				       owner_domain.len);
				new_name.len += owner_domain.len;
				looked_up = true;
			} else {
				LogWarn(COMPONENT_IDMAPPER,
					"%s failed with code %d.",
					(group ? "getgrgid_r" : "getpwuid_r"),
					rc);
			}
		} else {
#ifdef USE_NFSIDMAP
			if (group) {
				rc = nfs4_gid_to_name(id, owner_domain.addr,
						      namebuff,
						      NFS4_MAX_DOMAIN_LEN + 1);
			} else {
				rc = nfs4_uid_to_name(id, owner_domain.addr,
						      namebuff,
						      NFS4_MAX_DOMAIN_LEN + 1);
			}
			if (rc == 0) {
				new_name.len = strlen(namebuff);
				looked_up = true;
			} else {
				LogWarn(COMPONENT_IDMAPPER,
					"%s failed with code %d.",
					(group ? "nfs4_gid_to_name" :
					 "nfs4_uid_to_name"), rc);
			}
#else				/* USE_NFSIDMAP */
			looked_up = false;
#endif				/* !USE_NFSIDMAP */
		}

		if (!looked_up) {
			if (nfs_param.nfsv4_param.allow_numeric_owners) {
				LogWarn(COMPONENT_IDMAPPER,
					"Lookup for %d failed, "
					"using numeric %s", id,
					(group ? "group" : "owner"));
				/* 2³² is 10 digits long in decimal */
				sprintf(namebuff, "%u", id);
				new_name.len = strlen(namebuff);
			} else {
				LogWarn(COMPONENT_IDMAPPER,
					"Lookup for %d failed, using nobody.",
					id);
				memcpy(new_name.addr, "nobody", 6);
				new_name.len = 6;
			}
		}

		/* Add to the cache and encode the result. */
		pthread_rwlock_wrlock(group ? &idmapper_group_lock :
				      &idmapper_user_lock);
		if (group)
			success = idmapper_add_group(&new_name, id);
		else
			success = idmapper_add_user(&new_name, id, NULL, false);

		pthread_rwlock_unlock(group ? &idmapper_group_lock :
				      &idmapper_user_lock);
		if (unlikely(!success)) {
			LogMajor(COMPONENT_IDMAPPER, "%s failed.",
				 group ? "idmapper_add_group" :
				 "idmaper_add_user");
		}
		not_a_size_t = new_name.len;
		return inline_xdr_bytes(xdrs, (char **)&new_name.addr,
					&not_a_size_t, UINT32_MAX);
	}
}

/**
 * @brief Encode a UID as a string
 *
 * @param[in,out] xdrs  XDR stream to which to encode
 * @param[in]     uid   UID
 *
 * @retval true on success.
 * @retval false on failure.
 */

bool xdr_encode_nfs4_owner(XDR *xdrs, uid_t uid)
{
	return xdr_encode_nfs4_princ(xdrs, uid, false);
}

/**
 * @brief Encode a GID as a string
 *
 * @param[in,out] xdrs  XDR stream to which to encode
 * @param[in]     gid   GID
 *
 * @retval true on success.
 * @retval false on failure.
 */

bool xdr_encode_nfs4_group(XDR *xdrs, gid_t gid)
{
	return xdr_encode_nfs4_princ(xdrs, gid, true);
}

/**
 * @brief Handle unqualified names
 *
 * @param[in]  name C string of name
 * @param[in]  len  Length of name
 * @param[out] id   ID found
 * @param[in]  anon ID to use in case of nobody
 *
 * @return true on success, false on just phoning it in.
 */

static bool atless2id(char *name, size_t len, uint32_t *id,
		      const uint32_t anon)
{
	if ((len == 6) && (!memcmp(name, "nobody", 6))) {
		*id = anon;
		return true;
	} else if (nfs_param.nfsv4_param.allow_numeric_owners) {
		char *end = NULL;
		*id = strtol(name, &end, 10);
		if (!(end && *end != '\0'))
			return true;
	}

	/* Nothing else without an @ is allowed. */
	return false;
}

/**
 * @brief Lookup a name using PAM
 *
 * @param[in]  name       C string of name
 * @param[in]  len        Length of name
 * @param[out] id         ID found
 * @param[in]  anon       ID to use in case of nobody
 * @param[in]  group      Whether this a group lookup
 * @param[out] gss_gid    Found GID
 * @param[out] gss_uid    Found UID
 * @apram[out] gotgss_gid Found a GID.
 * @param[in]  at         Location of the @
 *
 * @return true on success, false not making the grade
 */
static bool pwentname2id(char *name, size_t len, uint32_t *id,
			 const uint32_t anon, bool group, gid_t *gid,
			 bool *got_gid, char *at)
{
	if (at != NULL) {
		if (strcmp(at + 1, owner_domain.addr) != 0) {
			/* We won't map what isn't even in the right domain */
			return false;
		}
		*at = '\0';
	}
	if (group) {
		struct group g;
		struct group *gres;
		char *gbuf = alloca(PWENT_MAX_LEN);

		if (getgrnam_r(name, &g, gbuf, PWENT_MAX_LEN, &gres) != 0) {
			LogMajor(COMPONENT_IDMAPPER, "getpwnam_r %s failed",
				 name);
			return false;
		} else if (gres != NULL) {
			*id = gres->gr_gid;
			return true;
		}
#ifndef USE_NFSIDMAP
		else {
			char *end = NULL;
			gid_t gid;

			gid = strtol(name, &end, 10);
			if (end && *end != '\0')
				return 0;

			*id = gid;
			return true;
		}
#endif
	} else {
		struct passwd p;
		struct passwd *pres;
		char *buf = alloca(PWENT_MAX_LEN);

		if (getpwnam_r(name, &p, buf, PWENT_MAX_LEN, &pres) != 0) {
			LogInfo(COMPONENT_IDMAPPER, "getpwnam_r %s failed",
				name);
			return false;
		} else if (pres != NULL) {
			*id = pres->pw_uid;
			*gid = pres->pw_gid;
			*got_gid = true;
			return true;
		}
#ifndef USE_NFSIDMAP
		else {
			char *end = NULL;
			uid_t uid;

			uid = strtol(name, &end, 10);
			if (end && *end != '\0')
				return 0;

			*id = uid;
			*got_gid = false;
			return true;
		}
#endif
	}
	return false;
}

/**
 * @brief Lookup a name NFS ID Mapper
 *
 * @param[in]  name       C string of name
 * @param[in]  len        Length of name
 * @param[out] id         ID found
 * @param[in]  anon       ID to use in case of nobody
 * @param[in]  group      Whether this a group lookup
 * @param[out] gss_gid    Found GID
 * @param[out] gss_uid    Found UID
 * @apram[out] gotgss_gid Found a GID.
 * @param[in]  at         Location of the @
 *
 * @return true on success, false not making the grade
 */

static bool idmapname2id(char *name, size_t len, uint32_t *id,
			 const uint32_t anon, bool group, gid_t *gid,
			 bool *got_gid, char *at)
{
#ifdef USE_NFSIDMAP
	int rc;

	if (group)
		rc = nfs4_name_to_gid(name, id);
	else
		rc = nfs4_name_to_uid(name, id);

	if (rc == 0) {
		if (!group) {
#ifdef _HAVE_GSSAPI
			/* nfs4_gss_princ_to_ids takes the unqualified
			   name. */
			*at = '\0';
			rc = nfs4_gss_princ_to_ids("krb5", name, id, gid);
			if (rc == 0)
				*got_gid = true;
			else
				LogMajor(COMPONENT_IDMAPPER,
					 "nfs4_gss_princ_to_ids %s failed %d",
					 name, -rc);
		}
#endif				/* _HAVE_GSSAPI */
		return true;
	} else {
		LogInfo(COMPONENT_IDMAPPER,
			"%s %s failed with %d, using anonymous.",
			(group ? "nfs4_name_to_gid" : "nfs4_name_to_uid"), name,
			-rc);
		return false;
	}
#else				/* USE_NFSIDMAP */
	return false;
#endif				/* USE_NFSIDMAP */
}

/**
 * @brief Convert a name to an ID
 *
 * @param[in]  name  The name of the user
 * @param[out] id    The resulting id
 * @param[in]  group True if this is a group name
 * @param[in]  anon  ID to return if look up fails
 *
 * @return true if successful, false otherwise
 */

static bool name2id(const struct gsh_buffdesc *name, uint32_t *id, bool group,
		    const uint32_t anon)
{
	bool success;

	pthread_rwlock_rdlock(group ? &idmapper_group_lock :
			      &idmapper_user_lock);
	if (group)
		success = idmapper_lookup_by_gname(name, id);
	else
		success = idmapper_lookup_by_uname(name, id, NULL, false);
	pthread_rwlock_unlock(group ? &idmapper_group_lock :
			      &idmapper_user_lock);

	if (success)
		return true;
	else {
		gid_t gid;
		bool got_gid = false;
		/* Something we can mutate and count on as terminated */
		char *namebuff = alloca(name->len + 1);
		char *at;
		bool looked_up = false;

		memcpy(namebuff, name->addr, name->len);
		*(namebuff + name->len) = '\0';
		at = memchr(namebuff, '@', name->len);

		if (at == NULL) {
			if (pwentname2id
			    (namebuff, name->len, id, anon, group, &gid,
			     &got_gid, NULL))
				looked_up = true;
			else if (atless2id(namebuff, name->len, id, anon))
				looked_up = true;
			else
				return false;
		} else if (nfs_param.nfsv4_param.use_getpwnam) {
			looked_up =
			    pwentname2id(namebuff, name->len, id, anon, group,
					 &gid, &got_gid, at);
		} else {
			looked_up =
			    idmapname2id(namebuff, name->len, id, anon, group,
					 &gid, &got_gid, at);
		}

		if (!looked_up) {
			LogInfo(COMPONENT_IDMAPPER,
				"All lookups failed for %s, using anonymous.",
				namebuff);
			*id = anon;
		}

		pthread_rwlock_wrlock(group ? &idmapper_group_lock :
				      &idmapper_user_lock);
		if (group)
			success = idmapper_add_group(name, *id);
		else
			success =
			    idmapper_add_user(name, *id, got_gid ? &gid : NULL,
					      false);

		pthread_rwlock_unlock(group ? &idmapper_group_lock :
				      &idmapper_user_lock);

		if (!success)
			LogMajor(COMPONENT_IDMAPPER, "%s(%s %u) failed",
				 (group ? "gidmap_add" : "uidmap_add"),
				 namebuff, *id);
		return true;
	}
}

/**
 * @brief Convert a name to a uid
 *
 * @param[in]  name The name of the user
 * @param[out] uid  The resulting UID
 * @param[in]  anon The anonymous UID for this export
 *
 * @return true if successful, false otherwise
 *
 */
bool name2uid(const struct gsh_buffdesc *name, uid_t *uid, const uid_t anon)
{
	return name2id(name, uid, false, anon);
}

/**
 * @brief Convert a name to a GID
 *
 * @param[in]  name The name of the user
 * @param[out] gid  The resulting GID
 * @param[in]  anon The anonymous GID for this export
 *
 * @return true  if successful, false otherwise
 */
bool name2gid(const struct gsh_buffdesc *name, gid_t *gid, const gid_t anon)
{
	return name2id(name, gid, true, anon);
}

#ifdef _HAVE_GSSAPI
#ifdef _MSPAC_SUPPORT
/**
 * @brief Convert a principal (as returned by @c gss_display_name) to a UID
 *
 * @param[in]     name The principal of the user
 * @param[out]    uid The resulting UID
 * @param[in,out] gd   GSS data
 *
 * @return true if successful, false otherwise
 */
bool principal2uid(char *principal, uid_t *uid, gid_t *gid,
		   struct svc_rpc_gss_data *gd)
#else
/**
 * @brief Convert a principal (as returned by @c gss_display_name) to a UID
 *
 * @param[in]     name The principal of the user
 * @param[out]    uid The resulting UID
 *
 * @return true if successful, false otherwise
 */
bool principal2uid(char *principal, uid_t *uid, gid_t *gid)
#endif
{
#ifdef USE_NFSIDMAP
	uid_t gss_uid = ANON_UID;
	gid_t gss_gid = ANON_GID;
	const gid_t *gss_gidres = NULL;
	int rc;
	bool success;
	struct gsh_buffdesc princbuff = {
		.addr = principal,
		.len = strlen(principal)
	};
#endif

	if (nfs_param.nfsv4_param.use_getpwnam)
		return false;

#ifdef USE_NFSIDMAP
	pthread_rwlock_rdlock(&idmapper_user_lock);
	success =
	    idmapper_lookup_by_uname(&princbuff, &gss_uid, &gss_gidres, true);
	if (success && gss_gidres)
		gss_gid = *gss_gidres;
	pthread_rwlock_unlock(&idmapper_user_lock);
	if (unlikely(!success)) {
		if ((princbuff.len >= 4)
		    && (!memcmp(princbuff.addr, "nfs/", 4)
			|| !memcmp(princbuff.addr, "root/", 5)
			|| !memcmp(princbuff.addr, "host/", 5))) {
			/* NFSv4 specific features: RPCSEC_GSS will
			 * provide user like
			 *
			 * nfs/<host>
			 * root/<host>
			 * host/<host>
			 * choice is made to map them to root */
			/* This is a "root" request made from the
			   hostbased nfs principal, use root */
			*uid = 0;
			return true;
		}
		/* nfs4_gss_princ_to_ids required to extract uid/gid
		   from gss creds */
		rc = nfs4_gss_princ_to_ids("krb5", principal, &gss_uid,
					   &gss_gid);
		if (rc) {
#ifdef _MSPAC_SUPPORT
			bool found_uid = false;
			bool found_gid = false;
			if (gd->flags & SVC_RPC_GSS_FLAG_MSPAC) {
				struct wbcAuthUserParams params;
				wbcErr wbc_err;
				struct wbcAuthUserInfo *info;
				struct wbcAuthErrorInfo *error = NULL;

				memset(&params, 0, sizeof(params));
				params.level = WBC_AUTH_USER_LEVEL_PAC;
				params.password.pac.data =
				    (uint8_t *) gd->pac.ms_pac.value;
				params.password.pac.length =
				    gd->pac.ms_pac.length;

				wbc_err =
				    wbcAuthenticateUserEx(&params, &info,
							  &error);
				if (!WBC_ERROR_IS_OK(wbc_err)) {
					LogCrit(COMPONENT_IDMAPPER,
						"wbcAuthenticateUserEx returned %s",
						wbcErrorString(wbc_err));
					return false;
				}

				if (error) {
					LogCrit(COMPONENT_IDMAPPER,
						"nt_status: %s, display_string %s",
						error->nt_string,
						error->display_string);
					wbcFreeMemory(error);
					return false;
				}

				/* 1st SID is account sid, see wbclient.h */
				wbc_err =
				    wbcSidToUid(&info->sids[0].sid, &gss_uid);
				if (!WBC_ERROR_IS_OK(wbc_err)) {
					LogCrit(COMPONENT_IDMAPPER,
						"wbcSidToUid for uid returned %s",
						wbcErrorString(wbc_err));
					wbcFreeMemory(info);
					return false;
				}

				/* 2nd SID is primary_group sid, see
				   wbclient.h */
				wbc_err =
				    wbcSidToGid(&info->sids[1].sid, &gss_gid);
				if (!WBC_ERROR_IS_OK(wbc_err)) {
					LogCrit(COMPONENT_IDMAPPER,
						"wbcSidToUid for gid returned %s\n",
						wbcErrorString(wbc_err));
					wbcFreeMemory(info);
					return false;
				}
				wbcFreeMemory(info);
				found_uid = true;
				found_gid = true;
			}
#endif				/* _MSPAC_SUPPORT */
#ifdef _MSPAC_SUPPORT
			if ((found_uid == true) && (found_gid == true))
				goto principal_found;
#endif

			return false;
		}
#ifdef _MSPAC_SUPPORT
 principal_found:
#endif

		pthread_rwlock_wrlock(&idmapper_user_lock);
		success =
		    idmapper_add_user(&princbuff, gss_uid, &gss_gid, true);
		pthread_rwlock_unlock(&idmapper_user_lock);

		if (!success) {
			LogMajor(COMPONENT_IDMAPPER,
				 "idmapper_add_user(%s, %d, %d) failed",
				 principal, gss_uid, gss_gid);
		}
	}

	*uid = gss_uid;
	*gid = gss_gid;

	return true;
#else				/* !USE_NFSIDMAP */
	assert(!"prohibited by configuration");
	return false;
#endif
}
#endif

#ifdef _VID_MAPPING
bool query_vid(uint32_t * pid, uint32_t * pid_result, char * type, char * type_result)
{
	char command[2 * MAXPATHLEN];
	FILE *command_stream = NULL;
	char buffer[1024];
	size_t type_result_len;
	char *cur_pos;
	char *end_pos;
	bool_t result_read = FALSE;
	uint32_t id_result;

	if (nfs_param.vidmap_param.map_script[0] == '\0')
	{
		*pid_result = *pid;
		return 1;
	}

	snprintf(command, 2 * MAXPATHLEN, "%s %s %u",
		 nfs_param.vidmap_param.map_script, type, *pid);

	LogFullDebug(COMPONENT_IDMAPPER, "query_vid: About to run command %s", command);

	if ((command_stream = popen(command, "r")) == NULL) {
		LogCrit(COMPONENT_IDMAPPER, "query_vid: Failed to run command %s", command);
		return false;
	}

	type_result_len = strlen(type_result);

	/* Search for a line of the form [V]UID: <number> */
	while(fgets(buffer, sizeof(buffer), command_stream) != NULL) {
		LogFullDebug(COMPONENT_IDMAPPER, "query_vid: Parsing line \"%s\"", buffer);

		if (strncmp(buffer, type_result, type_result_len) != 0) {
			LogFullDebug(COMPONENT_IDMAPPER, "query_vid: Line does not start with %s", type_result);
			continue;
		}

		cur_pos = buffer + type_result_len;

		while (isspace(*cur_pos)) {
			++cur_pos;
		}

		if (*cur_pos != ':') {
			LogFullDebug(COMPONENT_IDMAPPER, "query_vid: No \':\' after type");
			continue;
		}

		++cur_pos;

		while (isspace(*cur_pos)) {
			++cur_pos;
		}

		if (*cur_pos == '\0') {
			LogFullDebug(COMPONENT_IDMAPPER, "query_vid: No number after \':\'");
			continue;
		}

		id_result = strtoul(cur_pos, &end_pos, 10);

		if (cur_pos == end_pos) {
			LogFullDebug(COMPONENT_IDMAPPER, "query_vid: No number was read");
			continue;
		}
		else {
			LogFullDebug(COMPONENT_IDMAPPER, "query_vid: Read number %"PRIu32, id_result);
		}

		cur_pos = end_pos;

		while (isspace(*cur_pos)) {
			++cur_pos;
		}

		if (*cur_pos != '\0') {
			LogFullDebug(COMPONENT_IDMAPPER, "query_vid: Extra characters \'%s\' after number", cur_pos);
			continue;
		}

		result_read = TRUE;
		break;
	}

	pclose(command_stream);

	if (!result_read) {
		return false;
	}

	*pid_result = id_result;

	return true;
}

/**
 * @brief Convert UID to a virtual UID
 *
 * @param[in] puid The ID
 * @param[out] pvuid The virtual UID
 *
 * @return true if successful, false otherwise
 */
bool uid2vuid(uid_t *puid, uid_t *pvuid)
{
	bool success = false;
	uid_t vuid;

	pthread_rwlock_rdlock(&vidmap_user_lock);

	success = vidmap_lookup_by_uid(*puid, pvuid);

	pthread_rwlock_unlock(&vidmap_user_lock);

	if (likely(success)) {
		return true;
	}

	if (!query_vid(puid, &vuid, "XIDTOID UID", "UID")) {
		LogWarn(COMPONENT_IDMAPPER,
			"uid2vuid: Query for UID %u failed",
			*puid);
		return false;
	}
	else {
		pthread_rwlock_wrlock(&vidmap_user_lock);

		vidmap_add_user(*puid,vuid);

		pthread_rwlock_unlock(&vidmap_user_lock);

		*pvuid = vuid;
	}

	LogFullDebug(COMPONENT_IDMAPPER,
		     "uid2vuid: Mapped UID %u to VUID %u",
		     *puid, *pvuid);

	return true;
}

/**
 * @brief Convert a virtual UID to UID
 *
 * @param[out] pvuid The virtual UID
 * @param[in] puid The ID
 *
 * @return true if successful, false otherwise
 */
bool vuid2uid( uid_t *pvuid, uid_t *puid)
{
	bool success = false;
	uid_t uid;

	pthread_rwlock_rdlock(&vidmap_user_lock);

	success = vidmap_lookup_by_vuid(*pvuid, puid);

	pthread_rwlock_unlock(&vidmap_user_lock);

	if (likely(success)) {
		return true;
	}

	if (!query_vid(pvuid, &uid, "IDTOXID UID", "UID")) {
		LogWarn(COMPONENT_IDMAPPER,
			"vuid2uid: Query for VUID %u failed",
			*pvuid);
		return false;
	}
	else {
		pthread_rwlock_wrlock(&vidmap_user_lock);

		vidmap_add_user(uid,*pvuid);

		pthread_rwlock_unlock(&vidmap_user_lock);

		*puid = uid;
	}

	LogFullDebug(COMPONENT_IDMAPPER,
		     "vuid2uid: Mapped VUID %u to UID %u",
		     *pvuid, *puid);

	return true;
}

/**
 * @brief Convert GID to a virtual GID
 *
 * @param[in] pgid The ID
 * @param[out] pvgid The virtual GID
 *
 * @return true if successful, false otherwise
 */
bool gid2vgid(gid_t *pgid, gid_t *pvgid)
{
	bool success = false;
	gid_t vgid;

	pthread_rwlock_rdlock(&vidmap_group_lock);

	success = vidmap_lookup_by_gid(*pgid, pvgid);

	pthread_rwlock_unlock(&vidmap_group_lock);

	if (likely(success)) {
		return true;
	}

	if (!query_vid(pgid, &vgid, "XIDTOID GID", "GID")) {
		LogWarn(COMPONENT_IDMAPPER,
			"gid2vgid: Query for GID %u failed",
			*pgid);
		return false;
	}
	else {
		pthread_rwlock_wrlock(&vidmap_group_lock);

		vidmap_add_group(*pgid,vgid);

		pthread_rwlock_unlock(&vidmap_group_lock);

		*pvgid = vgid;
	}

	LogFullDebug(COMPONENT_IDMAPPER,
		     "gid2vgid: Mapped GID %u to VGID %u",
		     *pgid, *pvgid);

	return true;
}

/**
 * @brief Convert a virtual GID to GID
 *
 * @param[out] pvgid The virtual GID
 * @param[in] pgid The ID
 *
 * @return true if successful, false otherwise
 */
bool vgid2gid( gid_t *pvgid, gid_t *pgid)
{
	bool success = false;
	gid_t gid;

	pthread_rwlock_rdlock(&vidmap_group_lock);

	success = vidmap_lookup_by_vgid(*pvgid, pgid);

	pthread_rwlock_unlock(&vidmap_group_lock);

	if (likely(success)) {
		return true;
	}

	if (!query_vid(pvgid, &gid, "IDTOXID GID", "GID")) {
		LogWarn(COMPONENT_IDMAPPER,
			"vgid2gid: Query for VGID %u failed",
			*pvgid);
		return false;
	}
	else {
		pthread_rwlock_wrlock(&vidmap_group_lock);

		vidmap_add_group(gid,*pvgid);

		pthread_rwlock_unlock(&vidmap_group_lock);

		*pgid = gid;
	}

	LogFullDebug(COMPONENT_IDMAPPER,
		     "vgid2gid: Mapped VGID %u to GID %u",
		     *pvgid, *pgid);

	return true;
}

#endif

/** @} */