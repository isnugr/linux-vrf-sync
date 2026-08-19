# linux_vrf_sync

A VPP plugin that synchronizes VPP IP/IP6 FIB table existence, FIB
table names, and VPP interface-to-FIB bindings to Linux VRF devices
used by [Linux Control Plane (LCP)](https://s3-docs.fd.io/vpp/25.02/developer/plugins/lcp.html),
so that Linux routing daemons (BIRD, FRR) driven off the LCP mirror
interfaces see the same VRF topology as VPP.

```
VPP                               Linux

IP/IP6 FIB table 100       <->    vrf100
name CUSTOMER-A                     altname CUSTOMER-A
                                     table 100

VPP interface                     LCP Linux interface
rdma0.100                         wan0.100
    |                                  |
    +-- FIB table 100                 +-- master vrf100
```

VPP is the source of truth. This plugin only ever pushes
`VPP -> Linux`. It never reads Linux VRF/route state back into VPP;
[`linux_nl` (part of the LCP plugin set)](https://s3-docs.fd.io/vpp/25.02/developer/plugins/lcp.html)
remains solely responsible for Linux netlink -> VPP route
synchronization, so no feedback loop is introduced. **This plugin does
not implement route synchronization of any kind.**

## Features

- Creates a deterministically-named Linux VRF device (`vrf<table_id>`)
  for every active VPP IPv4 or IPv6 FIB table, and removes it once
  both protocols are gone from that table_id.
- Mirrors a VPP FIB table's name to a Linux altname it exclusively
  manages, without ever touching an altname it did not itself set.
- Moves an LCP-mirrored Linux interface into the correct VRF master
  as its VPP interface's FIB binding changes, using the LCP pair
  database (never guessing a Linux interface name from a VPP one).
- Fully event-driven: reacts to `ip[6] table add/del`, `set interface
  ip[6] table`, and LCP pair add/delete as they happen; a short,
  targeted resync backstop (see [Design notes](#design-notes)) covers
  the one case VPP itself gives no notification for. No polling loop
  drives normal operation.
- Idempotent, safe reconciliation: every pass re-derives the desired
  state from live VPP data rather than trusting a specific callback,
  so duplicate/batched events, restarts, and "delete X then
  immediately recreate X" all converge to the same correct state.
- Conflict-safe: an interface with different IPv4 and IPv6 FIB
  bindings, or a table with different IPv4/IPv6 names, is never
  resolved by guessing -- it's logged once and the last known-safe
  Linux state is preserved.
- All Linux-side changes go through rtnetlink/libnl directly; no
  shell commands are ever invoked.
- `show linux vrf sync` / `linux vrf sync reconcile` CLI for
  observability and manual reconciliation.

## Requirements

- A VPP build with the `linux-cp` plugin set (`linux_cp_plugin.so`,
  `linux_nl_plugin.so`) already installed and enabled.
- `libnl-3` and `libnl-route-3` development headers (the same
  libraries `linux-cp` itself links against).
- A VPP source tree matching your installed VPP version closely
  enough to build against -- this plugin uses internal VPP/LCP/FIB
  APIs that are not part of any stable ABI. It was developed and
  tested against VPP `v26.06`; check `vnet/ip/ip4.h`,
  `vnet/ip/ip6.h`, `vnet/fib/fib_table.h` and
  `plugins/linux-cp/lcp_interface.h` in your target version for the
  APIs listed in [Design notes](#design-notes) before relying on this
  plugin against a different release.

## Project layout

```
.
├── src/                         plugin source
│   ├── CMakeLists.txt
│   ├── linux_vrf_sync.h
│   ├── linux_vrf_sync.c
│   ├── netlink.h
│   ├── netlink.c
│   └── cli.c
├── scripts/
│   ├── link-into-vpp-tree.sh    symlinks src/ into <vpp>/src/plugins/
│   ├── install-plugin.sh        installs only the built .so
│   └── enable-plugin.sh         adds the plugin stanza to startup.conf
└── tests/
    └── run_functional_tests.sh  functional test suite (see below)
```

`src/` is meant to be the single canonical copy of the plugin, kept
outside of any VPP source clone. VPP's `src/plugins/CMakeLists.txt`
discovers plugins by globbing `*/CMakeLists.txt` directly under
`src/plugins`, so `scripts/link-into-vpp-tree.sh` symlinks this
`src/` directory into a VPP source tree rather than requiring you to
copy the source into it.

## Building

Clone a VPP source tree matching your installed version, then symlink
this plugin's source into it and build normally:

```bash
git clone --branch <tag-matching-your-installed-vpp> \
    https://github.com/FDio/vpp.git vpp

/path/to/linux_vrf_sync/scripts/link-into-vpp-tree.sh   # edit VPP_DIR in the script,
                                                          # or export it, to point at ./vpp

cd vpp
make install-dep
make build-release
```

The resulting artifact is always named `linux_vrf_sync_plugin.so`:
VPP's `add_vpp_plugin()` build macro names every plugin
`<name>_plugin.so`, and VPP's plugin loader only auto-discovers files
matching that pattern in the configured plugin directory.

## Installing

Install **only** the built plugin `.so` into your existing VPP
installation -- never run a global `make install`, and never replace
`linux_cp_plugin.so`, `linux_nl_plugin.so`, or any core VPP library:

```bash
scripts/install-plugin.sh
```

This locates the built `linux_vrf_sync_plugin.so`, discovers your
runtime plugin directory from wherever `linux_cp_plugin.so` is
already installed (never hardcoded), strips the build tree's RUNPATH
so the installed copy resolves its dependencies (`liblcp.so`,
`libnl-3.so`, `libnl-route-3.so`) the same way `linux_cp_plugin.so`
itself does -- via the system's normal library search path -- and
installs it there.

Then enable it in `startup.conf`:

```bash
scripts/enable-plugin.sh
```

This backs up `/etc/vpp/startup.conf` to a timestamped copy and adds
only:

```
plugin linux_vrf_sync_plugin.so {
  enable
}
```

inside the existing `plugins { }` stanza -- no other line is touched.
Restart VPP to load it.

## CLI

```
show linux vrf sync
```

```
Table ID  VPP Name      Linux VRF  Altname       IPv4  IPv6
100       CUSTOMER-A    vrf100     CUSTOMER-A    yes   yes
200       TRANSIT       vrf200     TRANSIT       yes   no

VPP Interface   Linux Interface   IPv4 FIB   IPv6 FIB   Linux Master
rdma0.100       wan0.100          100        100        vrf100
```

```
linux vrf sync reconcile
```

Forces an immediate full reconciliation pass. Safe to run at any
time; it never recreates or touches state that is already correct.

## Testing

`tests/run_functional_tests.sh` exercises table creation with and
without a name, dual-stack tables collapsing to one VRF, deferred
deletion (VRF stays up while either protocol's table remains),
IPv4/IPv6 name-conflict handling, interface VRF binding and
re-binding, return-to-default, IPv4/IPv6 interface FIB conflicts, and
FIB-bind/LCP-pair ordering independence.

It uses only isolated, high-numbered test table IDs and a dedicated
throwaway loopback + LCP pair that it creates and tears down itself
-- it never touches any pre-existing table or interface. Run it
against a live VPP instance with the plugin installed and enabled:

```bash
VPPCTL=vppctl tests/run_functional_tests.sh
```

## Design notes

**VPP hooks used** (none invented; no VPP core patch required):

1. **`ip`/`ip6` table add/delete** -- `VNET_IP_TABLE_ADD_DEL_FUNCTION()`
   (`vnet/ip/ip_table.h`), a constructor-based registration list fired
   from `ip_table_create()`/`ip_table_delete()` in `vnet/ip/ip_api.c`
   for both the CLI and the API. The callback fires *before* the
   table is actually unlocked/freed on delete, which is why deletion
   defers to a process node instead of acting inline.
2. **Interface FIB binding changed** ("set interface ip[6] table") --
   `ip4_main.table_bind_callbacks`/`ip6_main.table_bind_callbacks`
   (`vnet/ip/ip4.h`, `vnet/ip/ip6.h`), invoked from `fib_table_bind()`
   in `vnet/interface_api.c`. This is a pre-existing, already-used
   extension point (`vnet/adj/adj_glean.c`, `plugins/svs/svs.c`, and
   `plugins/nat/nat44-ed/nat44_ed.c` all register into it) -- no core
   patch was needed to learn of per-interface VRF changes.
3. **LCP pair add/delete** -- `lcp_itf_pair_register_vft()`
   (`plugins/linux-cp/lcp_interface.h`), giving `pair_add_fn`/
   `pair_del_fn` callbacks. This also fires for LCP auto-subinterfaces
   (`lcp lcp-auto-subint on`) with no extra code needed.
4. **Reading current state** -- `fib_table_find()`, `fib_table_get()`,
   `fib_table_get_table_id_for_sw_if_index()`, direct iteration of
   `ip4_main.fibs`/`ip6_main.fibs` for discovery, and
   `lcp_itf_pair_walk()`/`lcp_itf_pair_get()` for LCP pair state.

**Netlink implementation.** `src/netlink.c` uses libnl-3/libnl-route-3
(the same libraries `linux-cp` links against). No shell command is
ever invoked. VRF create/inspect/delete/up and interface master
set/clear/get use libnl3's high-level `route/link` and
`route/link/vrf` API. Altname management (`IFLA_PROP_LIST`/
`IFLA_ALT_IFNAME`) has no high-level accessor in some libnl3 releases,
so it's built directly from libnl3's low-level `nlmsg`/`nla`
primitives -- the same `RTM_NEWLINKPROP`/`RTM_DELLINKPROP` messages
`ip link property add/del` sends. All netlink operations are
idempotent: "already exists", "already gone", and "already correct"
are all treated as success.

**Core design rules:**

- `table_id` is the only identity. The Linux primary interface name
  is always `vrf<table_id>`, deterministic, never derived from the
  VPP table name.
- The VPP table name is metadata only, mirrored into a Linux altname
  this plugin exclusively manages.
- IPv4 and IPv6 table presence are tracked independently per
  table_id, evaluated fresh via `fib_table_find()` on every
  reconcile pass; the Linux VRF exists iff at least one protocol's
  table exists in VPP.
- Table deletion always re-verifies live FIB state immediately before
  acting, never trusting the delete callback alone -- this is what
  makes "delete table N; immediately recreate table N" safe by
  construction.
- Interface-to-VRF mapping always goes through the LCP pair database
  (the Linux ifindex of the host/tap device), never by guessing a
  name.
- An IPv4/IPv6 FIB mismatch on one interface, or a name mismatch
  between an IPv4 and IPv6 table sharing a table_id, is never resolved
  by picking one side arbitrarily -- it's logged once and the last
  known-safe Linux state is left untouched.
- A Linux VRF is only ever deleted for a table_id this plugin itself
  created or adopted, and only after re-verifying at delete time that
  both protocols are actually absent and the Linux device still looks
  like the VRF this plugin created.

**Reconciliation flow:**

```
ip/ip6 table add/del, interface FIB bind change, or LCP pair add/del
        |
        v
   event signaled to a deferred process node
        |
        v
   linux_vrf_sync_reconcile_all()
      +-- lvs_reconcile_tables()
      |      re-derive active table_ids from ip4_main.fibs/ip6_main.fibs
      |      -> create/adopt/update or safely delete vrf<table_id>
      |      -> mirror the VPP table name to a plugin-managed altname
      +-- lvs_reconcile_interfaces()
             walk every LCP pair
             -> read its current IPv4/IPv6 FIB binding
             -> set/clear the Linux interface's VRF master accordingly
```

Table reconciliation always runs before interface reconciliation in
the same pass, which is what makes both possible orderings of
"FIB bind before LCP pair created" and "LCP pair created before FIB
bind" converge to the same final state.

## Known limitations

- On startup, a `vrf<N>` Linux device whose table_id has no
  corresponding VPP FIB table, and which this plugin process did not
  itself create or adopt during its own lifetime, is **not** deleted
  automatically -- this plugin only ever deletes a VRF it has a
  positive record of having created/adopted. This is a deliberate
  safety choice: never delete an arbitrary Linux interface just
  because its name matches the expected pattern. Such orphans are
  discoverable via `ip -d link show type vrf` cross-referenced against
  `show ip table`, but are not specially flagged by this plugin's CLI
  today.
- A FIB table can be brought into existence, or kept alive, by
  something other than the paths this plugin has an event for. In
  particular, `linux_nl`'s route-sync path can lock a table purely in
  reaction to a Linux netlink route -- including kernel-generated
  routes that never reach the VPP FIB -- and releases that lock
  asynchronously, through a code path with no callback this plugin
  can subscribe to (no other in-tree consumer of the interface
  FIB-bind callback needs to know when a table is *fully*
  dereferenced, so none of them expose or need such a hook either).
  Creation is always picked up at the next reconcile trigger of any
  kind (including the next restart), since discovery always re-scans
  live VPP state. Deletion is additionally covered by a short,
  targeted resync: any reconcile pass that actually changes Linux
  state schedules one more check about a second later, which is
  normally enough to catch the table's last lock clearing shortly
  after this plugin releases its own. The one case that isn't
  shortened by this: a table that becomes free with **no** further
  activity from this plugin's own events at all -- that's still only
  picked up at the next unrelated event, a manual
  `linux vrf sync reconcile`, or a restart. A complete fix would
  require hooking the underlying `fib_table_find_or_create_and_lock()`/
  `fib_table_unlock()` primitives themselves, which are used far more
  broadly than table lifecycle across the FIB subsystem and are not
  something a plugin should wrap.
- `lvs_reconcile_tables()`/`lvs_reconcile_interfaces()` are full
  passes over every tracked table/LCP pair on each triggering event.
  This is intentionally simple and always correct -- there's no
  incremental state to diff wrong -- and cheap at realistic VRF
  counts; it is not designed for tens of thousands of VRFs.
- Conflict warnings are logged once on state transition, not on every
  reconcile pass, to avoid log spam; there is no separate rate
  limiter beyond that edge-triggering.

## License

Apache License 2.0. See [LICENSE](LICENSE).
