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
 * @defgroup idmapper ID Mapper
 *
 * The ID Mapper module provides mapping between numerical user and
 * group IDs and NFSv4 style owner and group strings.
 *
 * @{
 */

/**
 * @file idmapper.c
 * @brief Id mapping functions
 */

#ifndef IDMAPPER_H
#define IDMAPPER_H
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include "ganesha_rpc.h"
#include "ganesha_types.h"
#include "err_hashtable.h"
#include "gsh_config.h"

/**
 * @brief Shared between idmapper.c and idmapper_cache.c.  If you
 * aren't in idmapper.c, leave these symbols alone.
 *
 * @{
 */

extern pthread_rwlock_t idmapper_user_lock;
extern pthread_rwlock_t idmapper_group_lock;

#ifdef _VID_MAPPING
extern pthread_rwlock_t vidmap_user_lock;
extern pthread_rwlock_t vidmap_group_lock;
#endif

void idmapper_cache_init(void);
bool idmapper_add_user(const struct gsh_buffdesc *, uid_t, const gid_t *,
		       bool);
bool idmapper_add_group(const struct gsh_buffdesc *, gid_t);
bool idmapper_lookup_by_uname(const struct gsh_buffdesc *, uid_t *,
			      const gid_t **, bool);
bool idmapper_lookup_by_uid(const uid_t, const struct gsh_buffdesc **,
			    const gid_t **);
bool idmapper_lookup_by_gname(const struct gsh_buffdesc *, uid_t *);
bool idmapper_lookup_by_gid(const gid_t, const struct gsh_buffdesc **);

#ifdef _VID_MAPPING
bool vidmap_add_user(uid_t uid, uid_t vuid);
bool vidmap_add_group(gid_t gid, gid_t vgid);
bool vidmap_lookup_by_uid(const uid_t uid,
			  uid_t *vuid);
bool vidmap_lookup_by_vuid(const uid_t vuid,
			  uid_t *uid);
bool vidmap_lookup_by_gid(const gid_t gid,
			  gid_t *vgid);
bool vidmap_lookup_by_vgid(const gid_t vgid,
			  gid_t *gid);
#endif
/** @} */

bool idmapper_init(void);
void idmapper_clear_cache(void);

bool xdr_encode_nfs4_owner(XDR *, uid_t);
bool xdr_encode_nfs4_group(XDR *, gid_t);

bool name2uid(const struct gsh_buffdesc *, uid_t *, const uid_t);
bool name2gid(const struct gsh_buffdesc *, gid_t *, const gid_t);

#ifdef _VID_MAPPING
bool uid2vuid(uid_t * puid, uid_t * pvuid);
bool vuid2uid(uid_t * pvuid, uid_t * puid);
bool gid2vgid(gid_t * pgid, gid_t * pvgid);
bool vgid2gid(gid_t * pvgid, gid_t * pgid);
#endif

#ifdef _HAVE_GSSAPI
#ifdef _MSPAC_SUPPORT
bool principal2uid(char *, uid_t *, gid_t *, struct svc_rpc_gss_data *);
#else
bool principal2uid(char *, uid_t *, gid_t *);
#endif
#endif

#ifdef _VID_MAPPING
bool vidmap_init(vidmap_parameter_t param);
void vidmap_cache_init(void);
#endif

#endif				/* IDMAPPER_H */
/** @} */
