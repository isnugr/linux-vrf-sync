/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Minimal rtnetlink/libnl helper layer used by linux_vrf_sync.
 * All Linux VRF/altname/master operations go through here.
 * No shell commands, no popen()/system() are used anywhere.
 */
#ifndef __LINUX_VRF_SYNC_NETLINK_H__
#define __LINUX_VRF_SYNC_NETLINK_H__

#include <vppinfra/clib.h>

struct nl_sock;

typedef struct lvs_nl_ctx_
{
  struct nl_sock *sk;	    /* high level libnl route socket */
  struct nl_sock *raw_sk;  /* low level socket, for altname prop-list msgs */
} lvs_nl_ctx_t;

typedef struct lvs_link_info_
{
  u8 exists;
  int ifindex;
  u8 is_vrf;
  u32 table_id; /* valid only if is_vrf */
  int master_ifindex; /* 0 if none */
} lvs_link_info_t;

int lvs_nl_open (lvs_nl_ctx_t *ctx);
void lvs_nl_close (lvs_nl_ctx_t *ctx);

/* Build the deterministic Linux VRF device name for a table id. */
void lvs_vrf_ifname (u32 table_id, char name[16]);

/* Inspect an existing link by name. Never fails on ENODEV; sets
 * info->exists = 0 in that case. Returns 0 on success (including
 * "does not exist"), <0 (-errno) on a real netlink error. */
int lvs_nl_link_inspect (lvs_nl_ctx_t *ctx, const char *ifname,
			 lvs_link_info_t *info);

/* Create table_id's VRF device (name from lvs_vrf_ifname) and bring
 * it up. Returns 0 on success, <0 (-errno) on failure. */
int lvs_nl_vrf_create (lvs_nl_ctx_t *ctx, u32 table_id, const char *ifname,
		       int *out_ifindex);

/* Ensure the device is administratively up. Idempotent. */
int lvs_nl_link_set_up (lvs_nl_ctx_t *ctx, int ifindex);

/* Delete a link by ifindex. Idempotent: ENODEV is treated as success. */
int lvs_nl_link_delete (lvs_nl_ctx_t *ctx, int ifindex);

/* altname management (RTM_NEWLINKPROP / RTM_DELLINKPROP). Idempotent:
 * adding an altname that already exists, or deleting one that is
 * already gone, is treated as success. */
int lvs_nl_altname_add (lvs_nl_ctx_t *ctx, int ifindex, const char *name);
int lvs_nl_altname_del (lvs_nl_ctx_t *ctx, int ifindex, const char *name);

/* Returns a vector of vectors (u8 **) of altnames currently on the
 * link. Caller frees with lvs_nl_altname_list_free(). */
u8 **lvs_nl_altname_list (lvs_nl_ctx_t *ctx, int ifindex);
void lvs_nl_altname_list_free (u8 **names);

/* Set (master_ifindex > 0) or clear (master_ifindex == 0) the VRF
 * master of ifindex. Idempotent. */
int lvs_nl_set_master (lvs_nl_ctx_t *ctx, int ifindex, int master_ifindex);

/* Returns the current master ifindex, or 0 if none, or <0 on error. */
int lvs_nl_get_master (lvs_nl_ctx_t *ctx, int ifindex);

#endif /* __LINUX_VRF_SYNC_NETLINK_H__ */
