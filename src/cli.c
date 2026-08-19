/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * CLI / observability for linux_vrf_sync:
 *   show linux vrf sync
 *   linux vrf sync reconcile
 */
#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/interface_funcs.h>
#include <plugins/linux-cp/lcp_interface.h>

#include "linux_vrf_sync.h"

static int
lvs_cmp_u32 (void *a1, void *a2)
{
  u32 *v1 = a1, *v2 = a2;
  return (int) *v1 - (int) *v2;
}

static clib_error_t *
show_linux_vrf_sync_fn (vlib_main_t *vm, unformat_input_t *input,
			vlib_cli_command_t *cmd)
{
  linux_vrf_sync_main_t *lvsm = &linux_vrf_sync_main;
  vnet_main_t *vnm = lvsm->vnet_main;
  linux_vrf_sync_table_t *t;
  linux_vrf_sync_itf_t *e;
  u32 *ids = NULL, *id;

  (void) input;
  (void) cmd;

  vlib_cli_output (vm, "%-9s %-14s %-10s %-14s %-5s %-5s %s", "Table ID",
		   "VPP Name", "Linux VRF", "Altname", "IPv4", "IPv6",
		   "Notes");

  /* clang-format off */
  pool_foreach (t, lvsm->table_pool)
    {
      vec_add1 (ids, t->table_id);
    }
  /* clang-format on */
  vec_sort_with_function (ids, lvs_cmp_u32);

  vec_foreach (id, ids)
    {
      u8 *notes = NULL;
      char ifname[16];

      t = linux_vrf_sync_table_find (*id);
      if (!t)
	continue;

      lvs_vrf_ifname (t->table_id, ifname);

      if (t->incompatible)
	notes = format (notes, "INCOMPATIBLE EXISTING LINK");
      if (t->name_conflict)
	notes = format (notes, "%s%s", notes ? "; " : "",
			"v4/v6 name conflict");

      /* %v (clib vector format) must never be fed a plain C string
       * literal: it reads a vec header at a negative offset from the
       * pointer, so a literal like (u8*)"" makes format() treat
       * unrelated stack/rodata bytes as a vector length. NULL is the
       * only safe "empty" value to pass to %v. */
      vlib_cli_output (vm, "%-9u %-14v %-10s %-14v %-5s %-5s %v", t->table_id,
		       t->v4_name ? t->v4_name : t->v6_name,
		       t->vrf_created ? ifname : "-",
		       t->managed_altname,
		       t->v4_present ? "yes" : "no",
		       t->v6_present ? "yes" : "no", notes);
      vec_free (notes);
    }

  vec_free (ids);

  vlib_cli_output (vm, "");
  vlib_cli_output (vm, "%-16s %-17s %-10s %-10s %-12s %s", "VPP Interface",
		   "Linux Interface", "IPv4 FIB", "IPv6 FIB", "Linux Master",
		   "Notes");

  /* clang-format off */
  pool_foreach (e, lvsm->itf_pool)
    {
      u8 *master;

      if (e->conflict)
	master = format (0, "CONFLICT");
      else if (e->master_table_id == ~0)
	master = format (0, "-");
      else
	master = format (0, "vrf%u", e->master_table_id);

      vlib_cli_output (vm, "%-16U %-17v %-10u %-10u %-12v %s",
		       format_vnet_sw_if_index_name, vnm,
		       e->phy_sw_if_index, e->host_name, e->v4_table_id,
		       e->v6_table_id, master,
		       e->conflict ? "IPv4/IPv6 FIB mismatch" : "");
      vec_free (master);
    }
  /* clang-format on */

  return NULL;
}

VLIB_CLI_COMMAND (show_linux_vrf_sync_command, static) = {
  .path = "show linux vrf sync",
  .short_help = "show linux vrf sync",
  .function = show_linux_vrf_sync_fn,
};

static clib_error_t *
linux_vrf_sync_reconcile_fn (vlib_main_t *vm, unformat_input_t *input,
			     vlib_cli_command_t *cmd)
{
  (void) input;
  (void) cmd;

  linux_vrf_sync_reconcile_all ();
  vlib_cli_output (vm, "linux-vrf-sync: reconciliation complete");
  return NULL;
}

VLIB_CLI_COMMAND (linux_vrf_sync_reconcile_command, static) = {
  .path = "linux vrf sync reconcile",
  .short_help = "linux vrf sync reconcile",
  .function = linux_vrf_sync_reconcile_fn,
};
