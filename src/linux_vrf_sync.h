/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * linux_vrf_sync: synchronize VPP IP/IP6 FIB tables and VPP
 * interface-to-FIB bindings to Linux VRF devices used by LCP.
 *
 * See /opt/linux_vrf_sync_plugin/README.md for the full design.
 */
#ifndef __LINUX_VRF_SYNC_H__
#define __LINUX_VRF_SYNC_H__

#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/fib/fib_table.h>

#include "netlink.h"

extern vlib_log_class_t linux_vrf_sync_logger;

/* No trailing ';' baked into these macros (unlike their statement-less
 * cousins elsewhere in the tree) so that "if (x) LVS_WARN(...); else
 * LVS_WARN(...);" call sites remain valid if/else statements. */
#define LVS_DBG(...) vlib_log_debug (linux_vrf_sync_logger, __VA_ARGS__)
#define LVS_INFO(...) vlib_log_info (linux_vrf_sync_logger, __VA_ARGS__)
#define LVS_NOTICE(...) vlib_log_notice (linux_vrf_sync_logger, __VA_ARGS__)
#define LVS_WARN(...) vlib_log_warn (linux_vrf_sync_logger, __VA_ARGS__)
#define LVS_ERR(...) vlib_log_err (linux_vrf_sync_logger, __VA_ARGS__)

/* per-table_id bookkeeping. Correctness never depends solely on this;
 * every reconcile pass re-derives truth from live VPP FIB state and
 * live Linux netlink state. This cache exists to (a) know which
 * altname we personally manage so we never touch a foreign altname,
 * (b) know which table_ids we previously created/adopted a VRF for so
 * we know which candidates to re-check for deferred deletion, and
 * (c) drive "show linux vrf sync" without re-querying netlink. */
typedef struct linux_vrf_sync_table_
{
  u32 table_id;
  u8 v4_present;
  u8 v6_present;
  u8 *v4_name; /* vec, NULL if table has no custom name */
  u8 *v6_name; /* vec, NULL if table has no custom name */
  u8 *managed_altname; /* vec, altname currently set by us, or NULL */
  u8 name_conflict;   /* v4_name != v6_name, both non-NULL */
  u8 incompatible;    /* vrf<id> exists in Linux but is not usable */
  u8 vrf_created;     /* we created or adopted the Linux VRF */
  int ifindex;	       /* Linux ifindex of vrf<id>, -1 if unknown */
} linux_vrf_sync_table_t;

typedef struct linux_vrf_sync_itf_
{
  u32 phy_sw_if_index;
  u32 host_sw_if_index;
  u32 host_vif_index; /* linux ifindex of the LCP host/tap device */
  u8 *host_name;
  u32 v4_table_id;
  u32 v6_table_id;
  u32 master_table_id; /* ~0 = no master, otherwise table_id set as master */
  u8 conflict;		/* v4/v6 table_id mismatch, both non-zero */
} linux_vrf_sync_itf_t;

typedef struct linux_vrf_sync_main_
{
  vlib_main_t *vlib_main;
  vnet_main_t *vnet_main;

  /* table_id -> pool index */
  uword *table_by_id;
  linux_vrf_sync_table_t *table_pool;

  /* phy_sw_if_index -> pool index */
  uword *itf_by_phy;
  linux_vrf_sync_itf_t *itf_pool;

  lvs_nl_ctx_t nl;

  u8 reconcile_in_progress;

  /* Set by lvs_ensure_vrf() / lvs_maybe_delete_vrf() / interface
   * master changes whenever a reconcile pass actually mutated Linux
   * state. A pass that made a change may have released one lock
   * among several a table needs before it can actually disappear
   * from VPP (see README "cross-plugin lock" section) with no
   * further event ever announcing when the *other* lock holder lets
   * go, so a change this pass means "worth checking again shortly";
   * a pass that changed nothing means the system is settled and the
   * process node goes back to pure event-driven waiting with zero
   * background cost. */
  u8 dirty_this_pass;
} linux_vrf_sync_main_t;

extern linux_vrf_sync_main_t linux_vrf_sync_main;

/* Runs a full reconcile pass: VPP FIB table state -> Linux VRF state,
 * then VPP interface FIB bindings -> Linux VRF master state. Safe to
 * call from CLI, from the deferred process node, or at startup. */
void linux_vrf_sync_reconcile_all (void);

/* Lookup helper exposed for CLI use. Returns NULL if table_id is not
 * currently tracked. */
linux_vrf_sync_table_t *linux_vrf_sync_table_find (u32 table_id);

#define LVS_EVENT_RECONCILE 1

#endif /* __LINUX_VRF_SYNC_H__ */
