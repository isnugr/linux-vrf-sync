# SPDX-License-Identifier: Apache-2.0
#
# linux_vrf_sync plugin build definition.
#
# This file lives in this project's src/ directory (the canonical,
# out-of-tree copy of the plugin source) and is picked up by the VPP
# build via a symlink at
#   <vpp-src-tree>/src/plugins/linux_vrf_sync -> this directory
# (see scripts/link-into-vpp-tree.sh). VPP's src/plugins/CMakeLists.txt
# globs */CMakeLists.txt directly under src/plugins, so the plugin
# source must appear there; the symlink keeps this directory as the
# single canonical copy instead of duplicating source inside a VPP
# clone.

vpp_find_path(LIBNL3_INCLUDE_DIR NAMES libnl3/netlink/route/link/vrf.h)

if (NOT LIBNL3_INCLUDE_DIR)
  message(WARNING "-- libnl3 headers not found - linux_vrf_sync plugin disabled")
  return()
endif()

vpp_plugin_find_library(linux_vrf_sync LIBNL3_LIB libnl-3.so)
vpp_plugin_find_library(linux_vrf_sync LIBNL3_ROUTE_LIB libnl-route-3.so.200)

include_directories(${LIBNL3_INCLUDE_DIR}/libnl3)

add_vpp_plugin(linux_vrf_sync
  SOURCES
  linux_vrf_sync.c
  netlink.c
  cli.c

  LINK_LIBRARIES
  lcp
  ${LIBNL3_LIB}
  ${LIBNL3_ROUTE_LIB}
)
