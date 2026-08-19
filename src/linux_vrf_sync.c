/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * linux_vrf_sync: VPP -> Linux one-way synchronization of:
 *   - IP/IP6 FIB table existence            -> Linux VRF device existence
 *   - IP/IP6 FIB table name                 -> Linux VRF altname
 *   - VPP interface IP/IP6 FIB binding      -> Linux LCP interface VRF master
 *
 * VPP hooks used (all pre-existing, none added):
 *   - VNET_IP_TABLE_ADD_DEL_FUNCTION()      ip/ip6 "table add/del" events
 *     (src/vnet/ip/ip_table.h, fired from src/vnet/ip/ip_api.c
 *     ip_table_create()/ip_table_delete(), which is what both the CLI
 *     ("ip table add/del") and the API use under the hood)
 *   - ip4_main.table_bind_callbacks / ip6_main.table_bind_callbacks
 *     interface FIB-binding-changed events, fired from fib_table_bind()
 *     in src/vnet/interface_api.c. This is the same mechanism used by
 *     src/vnet/adj/adj_glean.c, src/plugins/svs/svs.c and
 *     src/plugins/nat/nat44-ed/nat44_ed.c to learn of per-interface VRF
 *     changes; no core patch was necessary for this plugin.
 *   - lcp_itf_pair_register_vft()           LCP pair add/delete events
 *     (src/plugins/linux-cp/lcp_interface.h)
 *
 * See README.md for the full architecture writeup.
 */
#include <vnet/plugin/plugin.h>
#include <vpp/app/version.h>

#include <vnet/ip/ip4.h>
#include <vnet/ip/ip6.h>
#include <vnet/ip/ip_table.h>
#include <vnet/fib/fib_table.h>

#include <plugins/linux-cp/lcp_interface.h>

#include "linux_vrf_sync.h"

linux_vrf_sync_main_t linux_vrf_sync_main;
vlib_log_class_t linux_vrf_sync_logger;

/* -------------------------------------------------------------------
 * small helpers
 * ------------------------------------------------------------------- */

static u8
vec_str_eq (u8 *a, u8 *b)
{
  if (a == NULL || b == NULL)
    return (a == b);
  if (vec_len (a) != vec_len (b))
    return 0;
  return (0 == memcmp (a, b, vec_len (a)));
}

/* clib format() vectors are length-prefixed, not NUL-terminated.
 * libnl's nla_put_string() takes a plain C string and calls
 * strlen() on it, so anything derived from a fib_table_t->ft_desc
 * (or any other formatted vector) must be given a trailing NUL
 * before being handed to the netlink layer. Caller must vec_free()
 * the result. */
static u8 *
lvs_cstr (u8 *v)
{
  return format (0, "%v%c", v, 0);
}

/* fib_table_find_or_create_and_lock_i() (src/vnet/fib/fib_table.c)
 * writes ft_desc = "<ipv4|ipv6>-VRF:<table_id>" whenever no explicit
 * name was supplied. That is the literal string this VPP release
 * produces (confirmed against the running "show ip table" output),
 * so we treat a ft_desc matching it as "no VPP-assigned name" rather
 * than as real metadata to mirror into Linux. */
static u8
lvs_name_is_default (u8 *desc, u32 table_id, u8 is_ip6)
{
  u8 *def;
  u8 eq;

  def = format (0, "%s-VRF:%u", is_ip6 ? "ipv6" : "ipv4", table_id);
  eq = vec_str_eq (desc, def);
  vec_free (def);
  return eq;
}

static u8 *
lvs_get_table_name_by_index (fib_protocol_t proto, u32 fib_index,
			     u32 table_id)
{
  fib_table_t *fib;

  if (fib_index == (u32) ~0)
    return NULL;

  fib = fib_table_get (fib_index, proto);
  if (!fib->ft_desc)
    return NULL;

  if (lvs_name_is_default (fib->ft_desc, table_id, proto == FIB_PROTOCOL_IP6))
    return NULL;

  return vec_dup (fib->ft_desc);
}

static linux_vrf_sync_table_t *
lvs_table_get_or_create (u32 table_id)
{
  linux_vrf_sync_main_t *lvsm = &linux_vrf_sync_main;
  uword *p;
  linux_vrf_sync_table_t *t;

  p = hash_get (lvsm->table_by_id, table_id);
  if (p)
    return pool_elt_at_index (lvsm->table_pool, p[0]);

  pool_get_zero (lvsm->table_pool, t);
  t->table_id = table_id;
  t->ifindex = -1;
  hash_set (lvsm->table_by_id, table_id, t - lvsm->table_pool);
  return t;
}

static linux_vrf_sync_table_t *
lvs_table_find (u32 table_id)
{
  linux_vrf_sync_main_t *lvsm = &linux_vrf_sync_main;
  uword *p;

  p = hash_get (lvsm->table_by_id, table_id);
  if (!p)
    return NULL;
  return pool_elt_at_index (lvsm->table_pool, p[0]);
}

linux_vrf_sync_table_t *
linux_vrf_sync_table_find (u32 table_id)
{
  return lvs_table_find (table_id);
}

static void
lvs_table_remove (linux_vrf_sync_table_t *t)
{
  linux_vrf_sync_main_t *lvsm = &linux_vrf_sync_main;

  hash_unset (lvsm->table_by_id, t->table_id);
  vec_free (t->v4_name);
  vec_free (t->v6_name);
  vec_free (t->managed_altname);
  pool_put (lvsm->table_pool, t);
}

/* -------------------------------------------------------------------
 * table (VRF) reconciliation:  VPP ip/ip6 FIB table -> Linux VRF
 * ------------------------------------------------------------------- */

static void
lvs_ensure_vrf (linux_vrf_sync_table_t *t, u8 *desired_name)
{
  linux_vrf_sync_main_t *lvsm = &linux_vrf_sync_main;
  char ifname[16];
  lvs_link_info_t info;
  int rv;

  lvs_vrf_ifname (t->table_id, ifname);

  rv = lvs_nl_link_inspect (&lvsm->nl, ifname, &info);
  if (rv < 0)
    {
      LVS_ERR ("%s: netlink inspect failed: %d", ifname, rv);
      return;
    }

  if (!info.exists)
    {
      rv = lvs_nl_vrf_create (&lvsm->nl, t->table_id, ifname, &t->ifindex);
      if (rv < 0)
	{
	  LVS_ERR ("failed to create Linux VRF %s table %u: %d", ifname,
		  t->table_id, rv);
	  return;
	}
      lvs_nl_link_set_up (&lvsm->nl, t->ifindex);
      t->vrf_created = 1;
      t->incompatible = 0;
      lvsm->dirty_this_pass = 1;
      LVS_NOTICE ("created Linux VRF %s table %u", ifname, t->table_id);
    }
  else if (!info.is_vrf || info.table_id != t->table_id)
    {
      if (!t->incompatible)
	LVS_ERR ("%s exists but is %s; refusing to touch it "
		"(wanted VRF device bound to table %u)",
		ifname, info.is_vrf ? "a VRF for a different table" : "not a VRF",
		t->table_id);
      t->incompatible = 1;
      t->vrf_created = 0;
      return;
    }
  else
    {
      /* compatible pre-existing device: adopt it */
      if (!t->vrf_created)
	LVS_INFO ("adopted existing compatible Linux VRF %s table %u",
		 ifname, t->table_id);
      t->ifindex = info.ifindex;
      t->vrf_created = 1;
      t->incompatible = 0;
      lvs_nl_link_set_up (&lvsm->nl, t->ifindex);
    }

  /* altname metadata sync: only touch the altname we ourselves manage */
  if (desired_name)
    {
      if (!vec_str_eq (t->managed_altname, desired_name))
	{
	  u8 *cstr;

	  if (t->managed_altname)
	    {
	      cstr = lvs_cstr (t->managed_altname);
	      lvs_nl_altname_del (&lvsm->nl, t->ifindex, (char *) cstr);
	      vec_free (cstr);
	    }

	  cstr = lvs_cstr (desired_name);
	  rv = lvs_nl_altname_add (&lvsm->nl, t->ifindex, (char *) cstr);
	  vec_free (cstr);

	  if (rv < 0)
	    {
	      LVS_WARN ("could not set altname '%v' on %s: %d", desired_name,
		       ifname, rv);
	      vec_free (t->managed_altname);
	      t->managed_altname = NULL;
	    }
	  else
	    {
	      vec_free (t->managed_altname);
	      t->managed_altname = vec_dup (desired_name);
	      lvsm->dirty_this_pass = 1;
	      LVS_NOTICE ("set altname %v on %s", desired_name, ifname);
	    }
	}
    }
  else if (t->managed_altname)
    {
      u8 *cstr = lvs_cstr (t->managed_altname);
      lvs_nl_altname_del (&lvsm->nl, t->ifindex, (char *) cstr);
      vec_free (cstr);
      lvsm->dirty_this_pass = 1;
      LVS_NOTICE ("removed altname %v from %s", t->managed_altname, ifname);
      vec_free (t->managed_altname);
      t->managed_altname = NULL;
    }
}

static void
lvs_maybe_delete_vrf (linux_vrf_sync_table_t *t)
{
  linux_vrf_sync_main_t *lvsm = &linux_vrf_sync_main;
  char ifname[16];
  lvs_link_info_t info;
  int rv;

  /* Only ever delete VRFs we positively created or adopted ourselves. */
  if (!t->vrf_created)
    return;

  /* Defensive re-check straight from VPP FIB state: never trust a
   * stale callback, always look at the live tables right before
   * doing anything destructive. This is what makes
   * "delete table X; immediately recreate table X" safe: if X is
   * back by the time we get here, we simply do nothing. */
  if (fib_table_find (FIB_PROTOCOL_IP4, t->table_id) != (u32) ~0 ||
      fib_table_find (FIB_PROTOCOL_IP6, t->table_id) != (u32) ~0)
    return;

  lvs_vrf_ifname (t->table_id, ifname);

  rv = lvs_nl_link_inspect (&lvsm->nl, ifname, &info);
  if (rv < 0)
    {
      LVS_ERR ("%s: netlink inspect failed during delete check: %d", ifname,
	       rv);
      return;
    }

  if (!info.exists)
    {
      /* already gone */
      lvs_table_remove (t);
      return;
    }

  if (!info.is_vrf || info.table_id != t->table_id)
    {
      LVS_ERR ("refusing to delete %s: no longer looks like our VRF", ifname);
      return;
    }

  rv = lvs_nl_link_delete (&lvsm->nl, info.ifindex);
  if (rv < 0)
    {
      LVS_ERR ("failed to delete Linux VRF %s: %d", ifname, rv);
      return;
    }

  lvsm->dirty_this_pass = 1;
  LVS_NOTICE ("deleted Linux VRF %s table %u", ifname, t->table_id);
  lvs_table_remove (t);
}

static void
lvs_reconcile_tables (void)
{
  linux_vrf_sync_main_t *lvsm = &linux_vrf_sync_main;
  fib_table_t *fib;
  u32 *stale_ids = NULL, *id;
  linux_vrf_sync_table_t *t;

  /* Pass 1: discover table_ids we don't yet track, by scanning the
   * live fib pools. This is the only thing the pool scan is used
   * for -- it must NOT be used to decide presence/absence (see pass
   * 2). A fib_table_t's removal from ip4_main.fibs/ip6_main.fibs can
   * lag slightly behind fib_table_find() reporting it gone:
   * fib_table_find() reflects the table's lock count dropping to
   * zero immediately, while the pool entry reclaim can trail that by
   * a moment. Presence must therefore be established authoritatively
   * via fib_table_find() (pass 2), independently per protocol, never
   * inferred from whether this scan happened to visit a table_id. */
  /* clang-format off */
  pool_foreach (fib, ip4_main.fibs)
    {
      if (fib->ft_table_id == 0)
	continue;
      lvs_table_get_or_create (fib->ft_table_id);
    }

  pool_foreach (fib, ip6_main.fibs)
    {
      /* link-local and other internal ip6 tables use table_id ~0;
       * ignore those, they are not real user VRFs */
      if (fib->ft_table_id == 0 || fib->ft_table_id == (u32) ~0)
	continue;
      lvs_table_get_or_create (fib->ft_table_id);
    }
  /* clang-format on */

  /* Pass 2: authoritatively (re)establish presence/name for every
   * table_id we track -- old or newly discovered above -- straight
   * from fib_table_find(), independently per protocol. */
  /* clang-format off */
  pool_foreach (t, lvsm->table_pool)
    {
      u32 v4_idx = fib_table_find (FIB_PROTOCOL_IP4, t->table_id);
      u32 v6_idx = fib_table_find (FIB_PROTOCOL_IP6, t->table_id);
      u8 *desired;
      u8 was_conflict;

      t->v4_present = (v4_idx != (u32) ~0);
      vec_free (t->v4_name);
      t->v4_name = t->v4_present ?
	lvs_get_table_name_by_index (FIB_PROTOCOL_IP4, v4_idx, t->table_id) :
	NULL;

      t->v6_present = (v6_idx != (u32) ~0);
      vec_free (t->v6_name);
      t->v6_name = t->v6_present ?
	lvs_get_table_name_by_index (FIB_PROTOCOL_IP6, v6_idx, t->table_id) :
	NULL;

      if (!t->v4_present && !t->v6_present)
	{
	  vec_add1 (stale_ids, t->table_id);
	  continue;
	}

      was_conflict = t->name_conflict;

      if (t->v4_name && t->v6_name)
	{
	  if (vec_str_eq (t->v4_name, t->v6_name))
	    {
	      desired = t->v4_name;
	      t->name_conflict = 0;
	    }
	  else
	    {
	      /* keep whatever altname is already set; do not oscillate */
	      desired = t->managed_altname;
	      t->name_conflict = 1;
	    }
	}
      else if (t->v4_name)
	{
	  desired = t->v4_name;
	  t->name_conflict = 0;
	}
      else if (t->v6_name)
	{
	  desired = t->v6_name;
	  t->name_conflict = 0;
	}
      else
	{
	  desired = NULL;
	  t->name_conflict = 0;
	}

      if (t->name_conflict && !was_conflict)
	LVS_WARN ("IPv4 table %u name '%v' differs from IPv6 table name "
		 "'%v'; keeping existing Linux altname unchanged",
		 t->table_id, t->v4_name, t->v6_name);

      lvs_ensure_vrf (t, desired);
    }
  /* clang-format on */

  vec_foreach (id, stale_ids)
    {
      t = lvs_table_find (*id);
      if (t)
	lvs_maybe_delete_vrf (t);
    }

  vec_free (stale_ids);
}

/* -------------------------------------------------------------------
 * interface reconciliation: VPP interface FIB binding -> LCP host
 * device VRF master
 * ------------------------------------------------------------------- */

static linux_vrf_sync_itf_t *
lvs_itf_get_or_create (u32 phy_sw_if_index)
{
  linux_vrf_sync_main_t *lvsm = &linux_vrf_sync_main;
  uword *p;
  linux_vrf_sync_itf_t *e;

  p = hash_get (lvsm->itf_by_phy, phy_sw_if_index);
  if (p)
    return pool_elt_at_index (lvsm->itf_pool, p[0]);

  pool_get_zero (lvsm->itf_pool, e);
  e->phy_sw_if_index = phy_sw_if_index;
  e->master_table_id = ~0;
  hash_set (lvsm->itf_by_phy, phy_sw_if_index, e - lvsm->itf_pool);
  return e;
}

static void
lvs_reconcile_one_itf (lcp_itf_pair_t *lip)
{
  linux_vrf_sync_main_t *lvsm = &linux_vrf_sync_main;
  linux_vrf_sync_itf_t *e;
  u32 v4_tid, v6_tid, desired = ~0;
  u8 is_conflict;
  int rv, cur_master;

  e = lvs_itf_get_or_create (lip->lip_phy_sw_if_index);
  e->host_sw_if_index = lip->lip_host_sw_if_index;
  e->host_vif_index = lip->lip_vif_index;
  vec_free (e->host_name);
  e->host_name = lip->lip_host_name ? vec_dup (lip->lip_host_name) : NULL;

  v4_tid = fib_table_get_table_id_for_sw_if_index (FIB_PROTOCOL_IP4,
						   lip->lip_phy_sw_if_index);
  v6_tid = fib_table_get_table_id_for_sw_if_index (FIB_PROTOCOL_IP6,
						   lip->lip_phy_sw_if_index);
  if (v4_tid == (u32) ~0)
    v4_tid = 0;
  if (v6_tid == (u32) ~0)
    v6_tid = 0;

  e->v4_table_id = v4_tid;
  e->v6_table_id = v6_tid;

  is_conflict = 0;
  if (v4_tid == 0 && v6_tid == 0)
    desired = ~0;
  else if (v4_tid == 0)
    desired = v6_tid;
  else if (v6_tid == 0)
    desired = v4_tid;
  else if (v4_tid == v6_tid)
    desired = v4_tid;
  else
    is_conflict = 1;

  if (is_conflict)
    {
      if (!e->conflict)
	LVS_WARN ("%v has IPv4 FIB %u and IPv6 FIB %u; cannot represent "
		 "this using a single Linux VRF master, leaving %v alone",
		 e->host_name, v4_tid, v6_tid, e->host_name);
      e->conflict = 1;
      return; /* preserve last known safe state */
    }
  e->conflict = 0;

  if (e->host_vif_index == 0)
    return; /* no linux device to act on yet */

  cur_master = lvs_nl_get_master (&lvsm->nl, e->host_vif_index);
  if (cur_master < 0)
    {
      LVS_ERR ("could not read master of %v: %d", e->host_name, cur_master);
      return;
    }

  if (desired == (u32) ~0)
    {
      if (cur_master != 0)
	{
	  rv = lvs_nl_set_master (&lvsm->nl, e->host_vif_index, 0);
	  if (rv == 0)
	    {
	      lvsm->dirty_this_pass = 1;
	      LVS_NOTICE ("removed %v from Linux VRF", e->host_name);
	    }
	  else
	    LVS_ERR ("failed to clear master on %v: %d", e->host_name, rv);
	}
      e->master_table_id = ~0;
      return;
    }

  linux_vrf_sync_table_t *t = lvs_table_find (desired);
  if (!t || t->ifindex < 0 || t->incompatible)
    {
      LVS_WARN ("cannot bind %v to vrf%u: Linux VRF not ready", e->host_name,
	       desired);
      return;
    }

  if (cur_master != t->ifindex)
    {
      rv = lvs_nl_set_master (&lvsm->nl, e->host_vif_index, t->ifindex);
      if (rv == 0)
	{
	  lvsm->dirty_this_pass = 1;
	  LVS_NOTICE ("moved %v to vrf%u", e->host_name, desired);
	}
      else
	LVS_ERR ("failed to set master vrf%u on %v: %d", desired,
		e->host_name, rv);
    }

  e->master_table_id = desired;
}

static walk_rc_t
lvs_itf_walk_cb (index_t index, void *ctx)
{
  lcp_itf_pair_t *lip = lcp_itf_pair_get (index);

  if (lip)
    lvs_reconcile_one_itf (lip);

  return WALK_CONTINUE;
}

static void
lvs_reconcile_interfaces (void)
{
  lcp_itf_pair_walk (lvs_itf_walk_cb, NULL);
}

/* -------------------------------------------------------------------
 * top level reconcile entry point
 * ------------------------------------------------------------------- */

void
linux_vrf_sync_reconcile_all (void)
{
  linux_vrf_sync_main_t *lvsm = &linux_vrf_sync_main;

  if (lvsm->reconcile_in_progress)
    return;
  lvsm->reconcile_in_progress = 1;
  lvsm->dirty_this_pass = 0;

  /* tables first: interface reconciliation depends on the target
   * VRF's ifindex already being known (this is what makes both
   * orderings in section 19 converge to the same state) */
  lvs_reconcile_tables ();
  lvs_reconcile_interfaces ();

  lvsm->reconcile_in_progress = 0;
}

/* -------------------------------------------------------------------
 * deferred-work process node
 * ------------------------------------------------------------------- */

/* A fib_table_t can be kept alive by a lock this plugin has no
 * visibility into: `linux_nl_plugin`'s route-sync module takes its
 * own independent lock on a table as soon as it observes any Linux
 * route in it (including kernel-generated routes that never reach
 * the VPP FIB), and releases it later, asynchronously, when it
 * processes the matching route-delete notification. That release
 * happens through a code path with no callback list this plugin (or
 * any plugin -- no in-tree consumer of the table-bind callback needs
 * to know when a table is *fully* dereferenced, only what it's newly
 * bound to) can subscribe to. There is therefore no event that
 * reliably announces "this table's last lock, held elsewhere, was
 * just released" -- see README.md for the full trace.
 *
 * Rather than a permanent poll, this is a short, targeted retry:
 * lvsm->dirty_this_pass is set only when a reconcile pass actually
 * changed Linux state (created/altered/deleted a VRF, moved/cleared
 * an interface master -- see the dirty_this_pass assignments above).
 * A change this pass is exactly the signal that a lock this plugin
 * controls may have just been released, so it's worth checking again
 * soon in case that unblocks a table some other lock is still
 * holding. A pass that changed nothing means the system is settled,
 * and the process node goes back to a plain, zero-cost indefinite
 * wait -- no periodic wakeups at all while idle. */
#define LVS_RETRY_INTERVAL 1.0

static uword
linux_vrf_sync_process (vlib_main_t *vm, vlib_node_runtime_t *rt,
			vlib_frame_t *f)
{
  linux_vrf_sync_main_t *lvsm = &linux_vrf_sync_main;
  uword event_type;
  uword *event_data = 0;
  f64 retry_wait = 0; /* 0 = wait indefinitely (no pending retry) */

  while (1)
    {
      if (retry_wait > 0)
	vlib_process_wait_for_event_or_clock (vm, retry_wait);
      else
	vlib_process_wait_for_event (vm);

      event_type = vlib_process_get_events (vm, &event_data);

      switch (event_type)
	{
	case LVS_EVENT_RECONCILE:
	case ~0: /* retry timeout: no new event, re-verify anyway */
	  /* Multiple events may have piled up (multiple table_ids,
	   * multiple interfaces); a single full reconcile pass
	   * against live VPP state naturally coalesces and handles
	   * all of them, and is idempotent if triggered again. */
	  linux_vrf_sync_reconcile_all ();
	  break;
	default:
	  break;
	}

      retry_wait = lvsm->dirty_this_pass ? LVS_RETRY_INTERVAL : 0;

      vec_reset_length (event_data);
    }

  return 0;
}

VLIB_REGISTER_NODE (linux_vrf_sync_process_node) = {
  .function = linux_vrf_sync_process,
  .type = VLIB_NODE_TYPE_PROCESS,
  .name = "linux-vrf-sync-process",
};

static void
lvs_signal_reconcile (void)
{
  linux_vrf_sync_main_t *lvsm = &linux_vrf_sync_main;
  vlib_process_signal_event (lvsm->vlib_main,
			     linux_vrf_sync_process_node.index,
			     LVS_EVENT_RECONCILE, 0);
}

/* -------------------------------------------------------------------
 * VPP callbacks
 * ------------------------------------------------------------------- */

/* ip/ip6 "table add"/"table del" event. Fired from
 * src/vnet/ip/ip_api.c ip_table_create()/ip_table_delete() via the
 * VNET_IP_TABLE_ADD_DEL_FUNCTION constructor-registration mechanism
 * (same one nat44-ed uses). Note table deletion here can run BEFORE
 * the FIB table is actually unlocked/freed, hence deferring to the
 * process node rather than acting inline. */
static clib_error_t *
linux_vrf_sync_ip_table_add_del (vnet_main_t *vnm, u32 table_id, u32 is_add)
{
  (void) vnm;
  (void) is_add;

  if (table_id == 0)
    return 0;

  lvs_signal_reconcile ();
  return 0;
}

VNET_IP_TABLE_ADD_DEL_FUNCTION (linux_vrf_sync_ip_table_add_del);

/* interface ip4/ip6 FIB binding changed ("set interface ip[6] table").
 * Fired synchronously from fib_table_bind() in
 * src/vnet/interface_api.c for every ip4_main/ip6_main
 * table_bind_callbacks subscriber; deferred here for the same reason
 * as above and to keep this fast, uncontested callback path short. */
static void
linux_vrf_sync_ip4_table_bind (struct ip4_main_t *im, uword opaque,
			       u32 sw_if_index, u32 new_fib_index,
			       u32 old_fib_index)
{
  (void) im;
  (void) opaque;
  (void) sw_if_index;
  (void) new_fib_index;
  (void) old_fib_index;
  lvs_signal_reconcile ();
}

static void
linux_vrf_sync_ip6_table_bind (struct ip6_main_t *im, uword opaque,
			       u32 sw_if_index, u32 new_fib_index,
			       u32 old_fib_index)
{
  (void) im;
  (void) opaque;
  (void) sw_if_index;
  (void) new_fib_index;
  (void) old_fib_index;
  lvs_signal_reconcile ();
}

/* LCP pair add/delete. Registered via lcp_itf_pair_register_vft()
 * (src/plugins/linux-cp/lcp_interface.h), the standard extension
 * point other consumers of LCP pair lifecycle events use. */
static void
linux_vrf_sync_lcp_pair_add (lcp_itf_pair_t *lip)
{
  (void) lip;
  lvs_signal_reconcile ();
}

static void
linux_vrf_sync_lcp_pair_del (lcp_itf_pair_t *lip)
{
  linux_vrf_sync_main_t *lvsm = &linux_vrf_sync_main;
  uword *p;

  /* Clean up our bookkeeping only. Per design, VRF lifecycle depends
   * on VPP FIB table existence, never on LCP pair existence, so we
   * must not touch the Linux VRF here. */
  p = hash_get (lvsm->itf_by_phy, lip->lip_phy_sw_if_index);
  if (p)
    {
      linux_vrf_sync_itf_t *e = pool_elt_at_index (lvsm->itf_pool, p[0]);
      hash_unset (lvsm->itf_by_phy, e->phy_sw_if_index);
      vec_free (e->host_name);
      pool_put (lvsm->itf_pool, e);
    }
}

static lcp_itf_pair_vft_t linux_vrf_sync_lcp_vft = {
  .pair_add_fn = linux_vrf_sync_lcp_pair_add,
  .pair_del_fn = linux_vrf_sync_lcp_pair_del,
};

/* -------------------------------------------------------------------
 * init
 * ------------------------------------------------------------------- */

static clib_error_t *
linux_vrf_sync_init (vlib_main_t *vm)
{
  linux_vrf_sync_main_t *lvsm = &linux_vrf_sync_main;
  int rv;

  clib_memset (lvsm, 0, sizeof (*lvsm));
  lvsm->vlib_main = vm;
  lvsm->vnet_main = vnet_get_main ();
  lvsm->table_by_id = hash_create (0, sizeof (uword));
  lvsm->itf_by_phy = hash_create (0, sizeof (uword));

  linux_vrf_sync_logger = vlib_log_register_class ("linux-vrf-sync", 0);

  rv = lvs_nl_open (&lvsm->nl);
  if (rv < 0)
    return clib_error_return (0, "linux-vrf-sync: failed to open netlink "
			      "socket: %d",
			      rv);

  /* interface FIB-binding-change notifications (ip4/ip6) */
  ip4_table_bind_callback_t cb4 = {
    .function = linux_vrf_sync_ip4_table_bind,
    .function_opaque = 0,
  };
  vec_add1 (ip4_main.table_bind_callbacks, cb4);

  ip6_table_bind_callback_t cb6 = {
    .function = linux_vrf_sync_ip6_table_bind,
    .function_opaque = 0,
  };
  vec_add1 (ip6_main.table_bind_callbacks, cb6);

  /* LCP pair add/delete notifications */
  lcp_itf_pair_register_vft (&linux_vrf_sync_lcp_vft);

  /* startup reconciliation: VPP state is the source of truth, rebuild
   * the Linux representation from it now, in case Linux state is
   * missing (fresh boot) or stale (VPP restarted). */
  linux_vrf_sync_reconcile_all ();

  return 0;
}

VLIB_INIT_FUNCTION (linux_vrf_sync_init) = {
  .runs_after =
    VLIB_INITS ("lcp_interface_init", "ip4_lookup_init", "ip6_lookup_init"),
};

VLIB_PLUGIN_REGISTER () = {
  .version = VPP_BUILD_VER,
  .description = "Linux VRF Sync - VPP FIB table / interface binding -> Linux VRF (LCP)",
};
