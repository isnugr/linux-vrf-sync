#!/usr/bin/env bash
# Packages an already-built linux_vrf_sync_plugin.so into a .deb.
# Build first (see README's "Building" section), then:
#
#   VERSION=1.0.0 VPP_TAG=v26.06 ./scripts/build-deb.sh [vpp-source-tree] [output-dir]
#
# VERSION is the plugin's own release version (no leading "v"); VPP_TAG
# identifies the VPP version this .deb was built against and is folded
# into the package version, since this plugin's ABI is tied to a
# specific VPP release, not a stable one.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VPP_DIR="${1:-${VPP_DIR:-${PROJECT_DIR}/vpp}}"
OUT_DIR="${2:-${PROJECT_DIR}/dist}"

VERSION="${VERSION:?VERSION env var required, e.g. VERSION=1.0.0}"
VPP_TAG="${VPP_TAG:-$(cat "${PROJECT_DIR}/VPP_VERSION" 2>/dev/null || echo unknown)}"
ARCH="${ARCH:-amd64}"
PKG_NAME="linux-vrf-sync-plugin"

BUILT_SO="$(find "${VPP_DIR}/build-root" -type f -name 'linux_vrf_sync_plugin.so' 2>/dev/null | head -1)"
if [ -z "${BUILT_SO}" ]; then
  echo "error: linux_vrf_sync_plugin.so not found under ${VPP_DIR}/build-root; build first" >&2
  exit 1
fi

# Determine the install path the same way scripts/install-plugin.sh
# does at runtime: from wherever linux_cp_plugin.so is already
# installed, falling back to the conventional Debian/Ubuntu path for
# the given architecture when building on a host without VPP
# installed (e.g. a CI runner).
LINUX_CP_PLUGIN="$(find /usr/lib /usr/local/lib -type f -name linux_cp_plugin.so 2>/dev/null | head -1)"
if [ -n "${LINUX_CP_PLUGIN}" ]; then
  PLUGIN_DIR="$(dirname "${LINUX_CP_PLUGIN}")"
else
  PLUGIN_DIR="/usr/lib/${ARCH}-linux-gnu/vpp_plugins"
  echo "warning: linux_cp_plugin.so not found on this host; assuming plugin dir ${PLUGIN_DIR}" >&2
fi

PKG_ROOT="$(mktemp -d)"
trap 'rm -rf "${PKG_ROOT}"' EXIT
chmod 0755 "${PKG_ROOT}"

install -D -m 0644 "${BUILT_SO}" "${PKG_ROOT}${PLUGIN_DIR}/linux_vrf_sync_plugin.so"

# Strip the disposable build tree's RUNPATH so the packaged plugin
# resolves liblcp.so/libnl-3.so/libnl-route-3.so via the target
# system's normal library search path, same as install-plugin.sh does
# for a direct (non-packaged) install.
if command -v chrpath >/dev/null 2>&1; then
  chrpath -d "${PKG_ROOT}${PLUGIN_DIR}/linux_vrf_sync_plugin.so" >/dev/null 2>&1 || true
fi

mkdir -p "${PKG_ROOT}/DEBIAN"
cat > "${PKG_ROOT}/DEBIAN/control" <<EOF
Package: ${PKG_NAME}
Version: ${VERSION}~vpp${VPP_TAG#v}
Section: net
Priority: optional
Architecture: ${ARCH}
Depends: libnl-3-200, libnl-route-3-200
Maintainer: ${MAINTAINER:-linux_vrf_sync contributors}
Description: VPP plugin syncing FIB tables and interface bindings to Linux VRF (LCP)
 Synchronizes VPP IP/IP6 FIB table existence, FIB table names, and
 interface FIB bindings to Linux VRF devices used by Linux Control
 Plane (LCP). Built against VPP ${VPP_TAG}; requires a matching VPP
 installation with the linux-cp plugin set (linux_cp_plugin.so,
 linux_nl_plugin.so) already installed and enabled on the target
 system -- this package only installs linux_vrf_sync_plugin.so
 itself. See the project README for configuration and known
 limitations.
EOF

mkdir -p "${OUT_DIR}"
DEB_FILE="${OUT_DIR}/${PKG_NAME}_${VERSION}~vpp${VPP_TAG#v}_${ARCH}.deb"
dpkg-deb --build --root-owner-group "${PKG_ROOT}" "${DEB_FILE}"
echo "built: ${DEB_FILE}"
