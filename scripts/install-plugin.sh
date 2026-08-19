#!/usr/bin/env bash
# Installs ONLY the linux_vrf_sync plugin .so into the existing VPP
# plugin directory (discovered from linux_cp_plugin.so's location,
# never hardcoded). Does not touch any other installed component.
#
# Usage: ./install-plugin.sh [path-to-vpp-source-tree]
# Or:    VPP_DIR=/path/to/vpp ./install-plugin.sh
# Defaults to ../vpp relative to this project if neither is given.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VPP_DIR="${1:-${VPP_DIR:-${PROJECT_DIR}/vpp}}"

BUILT_SO="$(find "${VPP_DIR}/build-root" -type f -name 'linux_vrf_sync_plugin.so' 2>/dev/null | head -1)"
if [ -z "${BUILT_SO}" ]; then
  echo "error: linux_vrf_sync_plugin.so not found under ${VPP_DIR}/build-root; build first" >&2
  exit 1
fi

LINUX_CP_PLUGIN="$(find /usr/lib /usr/local/lib -type f -name linux_cp_plugin.so 2>/dev/null | head -1)"
if [ -z "${LINUX_CP_PLUGIN}" ]; then
  echo "error: linux_cp_plugin.so not found; cannot determine plugin dir" >&2
  exit 1
fi
PLUGIN_DIR="$(dirname "${LINUX_CP_PLUGIN}")"

echo "built plugin:   ${BUILT_SO}"
echo "plugin dir:     ${PLUGIN_DIR}"

file "${BUILT_SO}"
echo "--- ldd ---"
ldd "${BUILT_SO}" || true

install -m 0755 "${BUILT_SO}" "${PLUGIN_DIR}/linux_vrf_sync_plugin.so"

# The in-tree build embeds a RUNPATH pointing into this disposable
# build tree (build-root/...). linux_cp_plugin.so / linux_nl_plugin.so
# carry no such RPATH and resolve liblcp.so.26.06 / libnl-3.so.200 /
# libnl-route-3.so.200 purely via the system ldconfig cache; strip our
# RUNPATH too so the installed plugin behaves the same way and does
# not silently depend on this dev tree continuing to exist.
if command -v chrpath >/dev/null 2>&1; then
  chrpath -d "${PLUGIN_DIR}/linux_vrf_sync_plugin.so" >/dev/null 2>&1 || true
fi

echo "installed -> ${PLUGIN_DIR}/linux_vrf_sync_plugin.so"
echo "--- final ldd (post RUNPATH strip) ---"
ldd "${PLUGIN_DIR}/linux_vrf_sync_plugin.so"
