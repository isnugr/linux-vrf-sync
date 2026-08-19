#!/usr/bin/env bash
# Symlinks the canonical plugin source (src/) into a VPP source tree
# (<vpp>/src/plugins/linux_vrf_sync) so VPP's plugin glob picks it up
# for the build. Safe to re-run.
#
# Usage: ./link-into-vpp-tree.sh [path-to-vpp-source-tree]
# Or:    VPP_DIR=/path/to/vpp ./link-into-vpp-tree.sh
# Defaults to ../vpp relative to this project if neither is given.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC_DIR="${PROJECT_DIR}/src"
VPP_DIR="${1:-${VPP_DIR:-${PROJECT_DIR}/vpp}}"
LINK_PATH="${VPP_DIR}/src/plugins/linux_vrf_sync"

if [ ! -d "${VPP_DIR}/src/plugins" ]; then
  echo "error: ${VPP_DIR}/src/plugins not found; is the vpp source tree present?" >&2
  exit 1
fi

if [ -L "${LINK_PATH}" ]; then
  echo "symlink already present: ${LINK_PATH} -> $(readlink "${LINK_PATH}")"
elif [ -e "${LINK_PATH}" ]; then
  echo "error: ${LINK_PATH} exists and is not a symlink; refusing to overwrite" >&2
  exit 1
else
  ln -s "${SRC_DIR}" "${LINK_PATH}"
  echo "created symlink: ${LINK_PATH} -> ${SRC_DIR}"
fi
