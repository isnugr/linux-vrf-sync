#!/usr/bin/env bash
#
# Functional test driver for linux_vrf_sync. Uses ONLY isolated,
# unused test table IDs (9001, 9002) and a dedicated loopback + LCP
# pair created by this script -- never any pre-existing production
# table or interface.
#
# Run as root on the target VPP box, after the plugin is installed
# and vpp has been restarted with it enabled.
#
set -u

VPPCTL=${VPPCTL:-vppctl}
T1=9001
T2=9002
LOOP_HOST=lvstest0   # Linux name for the test LCP pair's host device
PASS=0
FAIL=0

log()  { printf '\n=== %s ===\n' "$*"; }
ok()   { PASS=$((PASS+1)); printf '[PASS] %s\n' "$*"; }
bad()  { FAIL=$((FAIL+1)); printf '[FAIL] %s\n' "$*"; }

vc() { $VPPCTL "$@"; }

require_unused_table () {
  local t=$1
  if vc show ip table "$t" 2>/dev/null | grep -q "table_id:$t "; then
    echo "table $t already in use, aborting" >&2
    exit 1
  fi
}

cleanup () {
  log "cleanup"
  vc lcp delete "$LOOP_LCP_PHY" 2>/dev/null || true
  vc delete loopback interface intfc "$LOOP_LCP_PHY" 2>/dev/null || true
  vc ip6 table del "$T2" 2>/dev/null || true
  vc ip table del "$T2" 2>/dev/null || true
  vc ip6 table del "$T1" 2>/dev/null || true
  vc ip table del "$T1" 2>/dev/null || true
}
trap cleanup EXIT

log "pre-flight: confirm test tables are unused"
require_unused_table $T1
require_unused_table $T2

# ---------------------------------------------------------------
# 1: table creation
# ---------------------------------------------------------------
log "1: table creation (ip table add $T1 name VRF-TEST)"
vc ip table add "$T1" name VRF-TEST
sleep_reconcile() { vc linux vrf sync reconcile >/dev/null; }
sleep_reconcile

if ip -d link show "vrf$T1" 2>/dev/null | grep -q "vrf table $T1"; then
  ok "vrf$T1 created with table $T1"
else
  bad "vrf$T1 not created correctly"
fi

if ip -d link show "vrf$T1" 2>/dev/null | grep -q "altname VRF-TEST"; then
  ok "altname VRF-TEST present"
else
  bad "altname VRF-TEST missing"
fi

vc ip6 table add "$T1" name VRF-TEST
sleep_reconcile
count=$(ip -d link show type vrf 2>/dev/null | grep -c "vrf$T1:")
if [ "$count" = "1" ]; then
  ok "still exactly one vrf$T1 after ip6 table add"
else
  bad "unexpected vrf$T1 device count: $count"
fi

# ---------------------------------------------------------------
# 2: delete one protocol
# ---------------------------------------------------------------
log "2: delete IPv4 table only, IPv6 remains"
vc ip table del "$T1"
sleep_reconcile
if ip -d link show "vrf$T1" >/dev/null 2>&1; then
  ok "vrf$T1 still present after deleting only the IPv4 table"
else
  bad "vrf$T1 incorrectly removed while IPv6 table $T1 still exists"
fi

vc ip6 table del "$T1"
sleep_reconcile
if ip -d link show "vrf$T1" >/dev/null 2>&1; then
  bad "vrf$T1 still present after both protocol tables deleted"
else
  ok "vrf$T1 removed once both IPv4 and IPv6 tables are gone"
fi

# ---------------------------------------------------------------
# 3: table without a name
# ---------------------------------------------------------------
log "3: table without a name"
vc ip table add "$T1"
sleep_reconcile
if ip -d link show "vrf$T1" >/dev/null 2>&1; then
  ok "vrf$T1 created for unnamed table"
else
  bad "vrf$T1 not created for unnamed table"
fi
if ip -d link show "vrf$T1" 2>/dev/null | grep -q altname; then
  bad "unexpected altname present on unnamed table's VRF"
else
  ok "no altname on unnamed table's VRF"
fi
vc ip table del "$T1"
sleep_reconcile

# ---------------------------------------------------------------
# 4: name conflict
# ---------------------------------------------------------------
log "4: IPv4/IPv6 name conflict"
vc ip table add "$T1" name CUSTOMER-A
vc ip6 table add "$T1" name CUSTOMER-B
sleep_reconcile
if ip -d link show "vrf$T1" >/dev/null 2>&1; then
  ok "vrf$T1 remains functional under name conflict"
else
  bad "vrf$T1 missing under name conflict"
fi
if vc show linux vrf sync | grep -E "^$T1 " | grep -qi conflict; then
  ok "CLI reports the name conflict"
else
  bad "CLI does not report the name conflict"
fi

# ---------------------------------------------------------------
# 5-9: interface binding tests, using a dedicated
# loopback + LCP pair so no production interface is touched.
# ---------------------------------------------------------------
log "create isolated test loopback + LCP pair"
LOOP_LCP_PHY=$(vc create loopback interface instance 253 2>&1 | tail -1)
# 'create loopback interface' prints the created if name e.g. loop253
LOOP_LCP_PHY=$(echo "$LOOP_LCP_PHY" | grep -oE 'loop[0-9]+' | tail -1)
if [ -z "$LOOP_LCP_PHY" ]; then
  echo "could not create test loopback, skipping interface tests" >&2
else
  vc set interface state "$LOOP_LCP_PHY" up
  vc lcp create "$LOOP_LCP_PHY" host-if "$LOOP_HOST"
  sleep_reconcile

  log "5: interface VRF binding"
  vc set interface ip table "$LOOP_LCP_PHY" "$T1"
  vc set interface ip6 table "$LOOP_LCP_PHY" "$T1"
  sleep_reconcile
  if ip link show "$LOOP_HOST" 2>/dev/null | grep -q "master vrf$T1"; then
    ok "$LOOP_HOST bound to vrf$T1"
  else
    bad "$LOOP_HOST not bound to vrf$T1"
  fi

  log "6: move interface between VRFs"
  vc ip table add "$T2" name VRF-TEST2
  vc ip6 table add "$T2" name VRF-TEST2
  sleep_reconcile
  vc set interface ip table "$LOOP_LCP_PHY" "$T2"
  vc set interface ip6 table "$LOOP_LCP_PHY" "$T2"
  sleep_reconcile
  if ip link show "$LOOP_HOST" 2>/dev/null | grep -q "master vrf$T2"; then
    ok "$LOOP_HOST moved to vrf$T2"
  else
    bad "$LOOP_HOST not moved to vrf$T2"
  fi

  log "7: return to default"
  vc set interface ip table "$LOOP_LCP_PHY" 0
  vc set interface ip6 table "$LOOP_LCP_PHY" 0
  sleep_reconcile
  if ip link show "$LOOP_HOST" 2>/dev/null | grep -q "master"; then
    bad "$LOOP_HOST still has a master after returning to table 0"
  else
    ok "$LOOP_HOST has no master after returning to table 0"
  fi
  if ip link show "$LOOP_HOST" >/dev/null 2>&1; then
    ok "$LOOP_HOST still exists (not deleted)"
  else
    bad "$LOOP_HOST was deleted"
  fi

  log "8: IPv4/IPv6 interface conflict"
  vc set interface ip table "$LOOP_LCP_PHY" "$T1"
  vc set interface ip6 table "$LOOP_LCP_PHY" "$T2"
  sleep_reconcile
  if vc show linux vrf sync | grep "$LOOP_HOST" | grep -qi conflict; then
    ok "CLI reports the interface FIB conflict"
  else
    bad "CLI does not report the interface FIB conflict"
  fi
  vc set interface ip table "$LOOP_LCP_PHY" 0
  vc set interface ip6 table "$LOOP_LCP_PHY" 0
  sleep_reconcile

  log "9: ordering - FIB bind before LCP pair"
  vc lcp delete "$LOOP_LCP_PHY" 2>/dev/null || true
  sleep_reconcile
  vc set interface ip table "$LOOP_LCP_PHY" "$T1"
  vc set interface ip6 table "$LOOP_LCP_PHY" "$T1"
  vc lcp create "$LOOP_LCP_PHY" host-if "$LOOP_HOST"
  sleep_reconcile
  if ip link show "$LOOP_HOST" 2>/dev/null | grep -q "master vrf$T1"; then
    ok "bind-before-lcp-create converges to vrf$T1"
  else
    bad "bind-before-lcp-create did not converge"
  fi

  vc lcp delete "$LOOP_LCP_PHY" 2>/dev/null || true
  vc delete loopback interface intfc "$LOOP_LCP_PHY" 2>/dev/null || true
fi

vc ip6 table del "$T2" 2>/dev/null || true
vc ip table del "$T2" 2>/dev/null || true
vc ip6 table del "$T1" 2>/dev/null || true
vc ip table del "$T1" 2>/dev/null || true
sleep_reconcile

log "summary"
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
