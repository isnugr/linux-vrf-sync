/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * rtnetlink/libnl implementation for linux_vrf_sync.
 *
 * Uses libnl3's high level route/link API (the same library
 * linux_cp/linux_nl already link against) for link create/delete/up/
 * master operations, and libnl3's low level nl_msg API for altname
 * (IFLA_PROP_LIST / IFLA_ALT_IFNAME) management, since altnames have
 * no high level accessor in the libnl3 version shipped with this
 * VPP release (3.7.0). No shell commands are used anywhere.
 */
#include <string.h>
#include <errno.h>
#include <net/if.h>
#include <linux/if_link.h>
#include <linux/rtnetlink.h>

#include <netlink/netlink.h>
#include <netlink/socket.h>
#include <netlink/msg.h>
#include <netlink/attr.h>
#include <netlink/errno.h>
#include <netlink/route/link.h>
#include <netlink/route/link/vrf.h>

#include <vppinfra/mem.h>
#include <vppinfra/vec.h>
#include <vppinfra/string.h>

#include "netlink.h"

int
lvs_nl_open (lvs_nl_ctx_t *ctx)
{
  clib_memset (ctx, 0, sizeof (*ctx));

  ctx->sk = nl_socket_alloc ();
  if (!ctx->sk)
    return -ENOMEM;

  if (nl_connect (ctx->sk, NETLINK_ROUTE) < 0)
    {
      nl_socket_free (ctx->sk);
      ctx->sk = NULL;
      return -EIO;
    }

  return 0;
}

void
lvs_nl_close (lvs_nl_ctx_t *ctx)
{
  if (ctx->sk)
    nl_socket_free (ctx->sk);
  ctx->sk = NULL;
}

void
lvs_vrf_ifname (u32 table_id, char name[16])
{
  snprintf (name, 16, "vrf%u", table_id);
}

int
lvs_nl_link_inspect (lvs_nl_ctx_t *ctx, const char *ifname,
		     lvs_link_info_t *info)
{
  struct rtnl_link *link = NULL;
  int rv;

  clib_memset (info, 0, sizeof (*info));

  rv = rtnl_link_get_kernel (ctx->sk, 0, ifname, &link);
  if (rv < 0)
    {
      if (rv == -NLE_NODEV || rv == -NLE_OBJ_NOTFOUND)
	return 0; /* does not exist: not an error */
      return rv;
    }

  info->exists = 1;
  info->ifindex = rtnl_link_get_ifindex (link);
  info->is_vrf = rtnl_link_is_vrf (link) ? 1 : 0;
  if (info->is_vrf)
    {
      uint32_t tid = 0;
      rtnl_link_vrf_get_tableid (link, &tid);
      info->table_id = tid;
    }
  info->master_ifindex = rtnl_link_get_master (link);

  rtnl_link_put (link);
  return 0;
}

int
lvs_nl_vrf_create (lvs_nl_ctx_t *ctx, u32 table_id, const char *ifname,
		   int *out_ifindex)
{
  struct rtnl_link *link;
  int rv;

  link = rtnl_link_vrf_alloc ();
  if (!link)
    return -ENOMEM;

  rtnl_link_set_name (link, ifname);
  rv = rtnl_link_vrf_set_tableid (link, table_id);
  if (rv < 0)
    {
      rtnl_link_put (link);
      return rv;
    }

  rv = rtnl_link_add (ctx->sk, link, NLM_F_CREATE | NLM_F_EXCL);
  rtnl_link_put (link);

  if (rv < 0 && rv != -NLE_EXIST)
    return rv;

  /* re-fetch to get the kernel-assigned ifindex */
  link = NULL;
  rv = rtnl_link_get_kernel (ctx->sk, 0, ifname, &link);
  if (rv < 0)
    return rv;

  *out_ifindex = rtnl_link_get_ifindex (link);
  rtnl_link_put (link);
  return 0;
}

int
lvs_nl_link_set_up (lvs_nl_ctx_t *ctx, int ifindex)
{
  struct rtnl_link *change;
  int rv;

  change = rtnl_link_alloc ();
  if (!change)
    return -ENOMEM;

  rtnl_link_set_ifindex (change, ifindex);
  rtnl_link_set_flags (change, IFF_UP);

  rv = rtnl_link_change (ctx->sk, change, change, 0);
  rtnl_link_put (change);
  return (rv < 0) ? rv : 0;
}

int
lvs_nl_link_delete (lvs_nl_ctx_t *ctx, int ifindex)
{
  struct rtnl_link *link;
  int rv;

  link = rtnl_link_alloc ();
  if (!link)
    return -ENOMEM;

  rtnl_link_set_ifindex (link, ifindex);
  rv = rtnl_link_delete (ctx->sk, link);
  rtnl_link_put (link);

  if (rv < 0 && (rv == -NLE_NODEV || rv == -NLE_OBJ_NOTFOUND))
    return 0;
  return (rv < 0) ? rv : 0;
}

static int
lvs_altname_msg (lvs_nl_ctx_t *ctx, int ifindex, const char *name, int add)
{
  struct nl_msg *msg;
  struct ifinfomsg ifi;
  struct nlattr *prop;
  int rv;

  clib_memset (&ifi, 0, sizeof (ifi));
  ifi.ifi_family = AF_UNSPEC;
  ifi.ifi_index = ifindex;

  msg = nlmsg_alloc_simple (add ? RTM_NEWLINKPROP : RTM_DELLINKPROP,
			    NLM_F_REQUEST | NLM_F_ACK);
  if (!msg)
    return -ENOMEM;

  if (nlmsg_append (msg, &ifi, sizeof (ifi), NLMSG_ALIGNTO) < 0)
    {
      nlmsg_free (msg);
      return -ENOMEM;
    }

  prop = nla_nest_start (msg, IFLA_PROP_LIST);
  if (!prop)
    {
      nlmsg_free (msg);
      return -ENOMEM;
    }

  if (nla_put_string (msg, IFLA_ALT_IFNAME, name) < 0)
    {
      nlmsg_free (msg);
      return -ENOMEM;
    }

  nla_nest_end (msg, prop);

  rv = nl_send_auto (ctx->sk, msg);
  nlmsg_free (msg);
  if (rv < 0)
    return rv;

  return nl_wait_for_ack (ctx->sk);
}

int
lvs_nl_altname_add (lvs_nl_ctx_t *ctx, int ifindex, const char *name)
{
  int rv = lvs_altname_msg (ctx, ifindex, name, 1);
  if (rv == -NLE_EXIST)
    return 0;
  return rv;
}

int
lvs_nl_altname_del (lvs_nl_ctx_t *ctx, int ifindex, const char *name)
{
  int rv = lvs_altname_msg (ctx, ifindex, name, 0);
  if (rv == -NLE_OBJ_NOTFOUND || rv == -NLE_NODEV || rv == -NLE_INVAL)
    return 0;
  return rv;
}

typedef struct
{
  void *buf;
  int len;
} lvs_raw_capture_t;

static int
lvs_capture_cb (struct nl_msg *msg, void *arg)
{
  lvs_raw_capture_t *cap = arg;
  struct nlmsghdr *nlh = nlmsg_hdr (msg);

  /* keep only the first matching message */
  if (cap->buf)
    return NL_OK;

  cap->len = nlh->nlmsg_len;
  cap->buf = clib_mem_alloc (cap->len);
  clib_memcpy (cap->buf, nlh, cap->len);
  return NL_OK;
}

u8 **
lvs_nl_altname_list (lvs_nl_ctx_t *ctx, int ifindex)
{
  struct nl_sock *tmp;
  struct nl_msg *msg = NULL;
  lvs_raw_capture_t cap = { 0 };
  u8 **result = NULL;

  tmp = nl_socket_alloc ();
  if (!tmp)
    return NULL;

  if (nl_connect (tmp, NETLINK_ROUTE) < 0)
    {
      nl_socket_free (tmp);
      return NULL;
    }

  nl_socket_modify_cb (tmp, NL_CB_VALID, NL_CB_CUSTOM, lvs_capture_cb, &cap);

  if (rtnl_link_build_get_request (ifindex, NULL, &msg) < 0)
    {
      nl_socket_free (tmp);
      return NULL;
    }

  if (nl_send_auto (tmp, msg) < 0)
    {
      nlmsg_free (msg);
      nl_socket_free (tmp);
      return NULL;
    }
  nlmsg_free (msg);

  nl_recvmsgs_default (tmp);
  nl_socket_free (tmp);

  if (!cap.buf)
    return NULL;

  struct nlmsghdr *nlh = cap.buf;
  struct nlattr *tb[IFLA_MAX + 1];

  if (nlmsg_parse (nlh, sizeof (struct ifinfomsg), tb, IFLA_MAX, NULL) >= 0 &&
      tb[IFLA_PROP_LIST])
    {
      struct nlattr *nested;
      int remaining;

      nla_for_each_nested (nested, tb[IFLA_PROP_LIST], remaining)
	{
	  if (nla_type (nested) == IFLA_ALT_IFNAME)
	    {
	      char *s = nla_get_string (nested);
	      u8 *name = NULL;
	      size_t len = strlen (s);

	      vec_validate (name, len - 1);
	      clib_memcpy (name, s, len);
	      vec_add1 (result, name);
	    }
	}
    }

  clib_mem_free (cap.buf);
  return result;
}

void
lvs_nl_altname_list_free (u8 **names)
{
  u8 **n;
  vec_foreach (n, names)
    vec_free (*n);
  vec_free (names);
}

int
lvs_nl_set_master (lvs_nl_ctx_t *ctx, int ifindex, int master_ifindex)
{
  struct rtnl_link *change;
  int rv;

  change = rtnl_link_alloc ();
  if (!change)
    return -ENOMEM;

  rtnl_link_set_ifindex (change, ifindex);
  rtnl_link_set_master (change, master_ifindex);

  rv = rtnl_link_change (ctx->sk, change, change, 0);
  rtnl_link_put (change);
  return (rv < 0) ? rv : 0;
}

int
lvs_nl_get_master (lvs_nl_ctx_t *ctx, int ifindex)
{
  struct rtnl_link *link = NULL;
  int rv, master;

  rv = rtnl_link_get_kernel (ctx->sk, ifindex, NULL, &link);
  if (rv < 0)
    return rv;

  master = rtnl_link_get_master (link);
  rtnl_link_put (link);
  return master;
}
