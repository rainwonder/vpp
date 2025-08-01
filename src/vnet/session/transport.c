/*
 * Copyright (c) 2017-2019 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <vnet/session/transport.h>
#include <vnet/session/session.h>
#include <vnet/fib/fib.h>

/**
 * Per-type vector of transport protocol virtual function tables
 */
transport_proto_vft_t *tp_vfts;

typedef struct local_endpoint_
{
  transport_endpoint_t ep;
  transport_proto_t proto;
  int refcnt;
} local_endpoint_t;

typedef struct transport_main_
{
  transport_endpoint_table_t local_endpoints_table;
  local_endpoint_t *local_endpoints;
  u32 *lcl_endpts_freelist;
  u32 port_allocator_seed;
  u16 port_alloc_max_tries;
  u16 port_allocator_min_src_port;
  u16 port_allocator_max_src_port;
  u8 lcl_endpts_cleanup_pending;
  clib_spinlock_t local_endpoints_lock;
} transport_main_t;

static transport_main_t tp_main;

u8 *
format_transport_proto (u8 * s, va_list * args)
{
  u32 transport_proto = va_arg (*args, u32);

  if (tp_vfts[transport_proto].transport_options.name)
    s = format (s, "%s", tp_vfts[transport_proto].transport_options.name);
  else
    s = format (s, "n/a");

  return s;
}

u8 *
format_transport_proto_short (u8 * s, va_list * args)
{
  u32 transport_proto = va_arg (*args, u32);
  char *short_name;

  short_name = tp_vfts[transport_proto].transport_options.short_name;
  if (short_name)
    s = format (s, "%s", short_name);
  else
    s = format (s, "NA");

  return s;
}

const char *transport_flags_str[] = {
#define _(sym, str) str,
  foreach_transport_connection_flag
#undef _
};

u8 *
format_transport_flags (u8 *s, va_list *args)
{
  transport_connection_flags_t flags;
  int i, last = -1;

  flags = va_arg (*args, transport_connection_flags_t);

  for (i = 0; i < TRANSPORT_CONNECTION_N_FLAGS; i++)
    if (flags & (1 << i))
      last = i;

  for (i = 0; i < last; i++)
    {
      if (flags & (1 << i))
	s = format (s, "%s, ", transport_flags_str[i]);
    }
  if (last >= 0)
    s = format (s, "%s", transport_flags_str[last]);

  return s;
}

u8 *
format_transport_connection (u8 * s, va_list * args)
{
  u32 transport_proto = va_arg (*args, u32);
  u32 conn_index = va_arg (*args, u32);
  clib_thread_index_t thread_index = va_arg (*args, u32);
  u32 verbose = va_arg (*args, u32);
  transport_proto_vft_t *tp_vft;
  transport_connection_t *tc;
  u32 indent;

  tp_vft = transport_protocol_get_vft (transport_proto);
  if (!tp_vft)
    return s;

  s = format (s, "%U", tp_vft->format_connection, conn_index, thread_index,
	      verbose);
  tc = tp_vft->get_connection (conn_index, thread_index);
  if (tc && verbose > 1)
    {
      indent = format_get_indent (s) + 1;
      if (transport_connection_is_tx_paced (tc))
	s = format (s, "%Upacer: %U\n", format_white_space, indent,
		    format_transport_pacer, &tc->pacer, tc->thread_index);
      s = format (s, "%Utransport: flags: %U\n", format_white_space, indent,
		  format_transport_flags, tc->flags);
    }
  return s;
}

u8 *
format_transport_listen_connection (u8 * s, va_list * args)
{
  u32 transport_proto = va_arg (*args, u32);
  transport_proto_vft_t *tp_vft;

  tp_vft = transport_protocol_get_vft (transport_proto);
  if (!tp_vft)
    return s;

  s = (tp_vft->format_listener) (s, args);
  return s;
}

u8 *
format_transport_half_open_connection (u8 * s, va_list * args)
{
  u32 transport_proto = va_arg (*args, u32);
  transport_proto_vft_t *tp_vft;

  tp_vft = transport_protocol_get_vft (transport_proto);
  if (!tp_vft)
    return s;

  s = (tp_vft->format_half_open) (s, args);
  return s;
}

static u8
unformat_transport_str_match (unformat_input_t * input, const char *str)
{
  int i;

  if (strlen (str) > vec_len (input->buffer) - input->index)
    return 0;

  for (i = 0; i < strlen (str); i++)
    {
      if (input->buffer[i + input->index] != str[i])
	return 0;
    }
  return 1;
}

uword
unformat_transport_proto (unformat_input_t * input, va_list * args)
{
  u32 *proto = va_arg (*args, u32 *);
  transport_proto_vft_t *tp_vft;
  u8 longest_match = 0, match;
  char *str, *str_match = 0;
  transport_proto_t tp;

  for (tp = 0; tp < vec_len (tp_vfts); tp++)
    {
      tp_vft = &tp_vfts[tp];
      str = tp_vft->transport_options.name;
      if (!str)
	continue;
      if (unformat_transport_str_match (input, str))
	{
	  match = strlen (str);
	  if (match > longest_match)
	    {
	      *proto = tp;
	      longest_match = match;
	      str_match = str;
	    }
	}
    }
  if (longest_match)
    {
      (void) unformat (input, str_match);
      return 1;
    }

  return 0;
}

u8 *
format_transport_protos (u8 * s, va_list * args)
{
  u32 indent = format_get_indent (s) + 1;
  transport_proto_vft_t *tp_vft;

  vec_foreach (tp_vft, tp_vfts)
    if (tp_vft->transport_options.name)
      s = format (s, "%U%s\n", format_white_space, indent,
		  tp_vft->transport_options.name);

  return s;
}

u8 *
format_transport_state (u8 *s, va_list *args)
{
  transport_main_t *tm = &tp_main;

  s = format (s, "registered protos:\n%U", format_transport_protos);

  s = format (s, "configs:\n");
  s =
    format (s, " min_lcl_port: %u max_lcl_port: %u\n",
	    tm->port_allocator_min_src_port, tm->port_allocator_max_src_port);

  s = format (s, "state:\n");
  s = format (s, " lcl ports alloced: %u\n lcl ports freelist: %u \n",
	      pool_elts (tm->local_endpoints),
	      vec_len (tm->lcl_endpts_freelist));
  s =
    format (s, " port_alloc_max_tries: %u\n lcl_endpts_cleanup_pending: %u\n",
	    tm->port_alloc_max_tries, tm->lcl_endpts_cleanup_pending);
  return s;
}

u32
transport_endpoint_lookup (transport_endpoint_table_t *ht, u8 proto,
			   u32 fib_index, ip46_address_t *ip, u16 port)
{
  clib_bihash_kv_24_8_t kv;
  int rv;

  kv.key[0] = ip->as_u64[0];
  kv.key[1] = ip->as_u64[1];
  kv.key[2] = (u64) fib_index << 32 | (u64) port << 8 | (u64) proto;

  rv = clib_bihash_search_inline_24_8 (ht, &kv);
  if (rv == 0)
    return kv.value;

  return ENDPOINT_INVALID_INDEX;
}

void
transport_endpoint_table_add (transport_endpoint_table_t * ht, u8 proto,
			      transport_endpoint_t * te, u32 value)
{
  clib_bihash_kv_24_8_t kv;

  kv.key[0] = te->ip.as_u64[0];
  kv.key[1] = te->ip.as_u64[1];
  kv.key[2] = (u64) te->fib_index << 32 | (u64) te->port << 8 | (u64) proto;
  kv.value = value;

  clib_bihash_add_del_24_8 (ht, &kv, 1);
}

void
transport_endpoint_table_del (transport_endpoint_table_t * ht, u8 proto,
			      transport_endpoint_t * te)
{
  clib_bihash_kv_24_8_t kv;

  kv.key[0] = te->ip.as_u64[0];
  kv.key[1] = te->ip.as_u64[1];
  kv.key[2] = (u64) te->fib_index << 32 | (u64) te->port << 8 | (u64) proto;

  clib_bihash_add_del_24_8 (ht, &kv, 0);
}

void
transport_register_protocol (transport_proto_t transport_proto,
			     const transport_proto_vft_t * vft,
			     fib_protocol_t fib_proto, u32 output_node)
{
  u8 is_ip4 = fib_proto == FIB_PROTOCOL_IP4;

  vec_validate (tp_vfts, transport_proto);
  tp_vfts[transport_proto] = *vft;

  session_register_transport (transport_proto, vft, is_ip4, output_node);
}

transport_proto_t
transport_register_new_protocol (const transport_proto_vft_t * vft,
				 fib_protocol_t fib_proto, u32 output_node)
{
  transport_proto_t transport_proto;
  u8 is_ip4;

  transport_proto = session_add_transport_proto ();
  is_ip4 = fib_proto == FIB_PROTOCOL_IP4;

  vec_validate (tp_vfts, transport_proto);
  tp_vfts[transport_proto] = *vft;

  session_register_transport (transport_proto, vft, is_ip4, output_node);

  return transport_proto;
}

/**
 * Get transport virtual function table
 *
 * @param type - session type (not protocol type)
 */
transport_proto_vft_t *
transport_protocol_get_vft (transport_proto_t transport_proto)
{
  if (transport_proto >= vec_len (tp_vfts))
    return 0;
  return &tp_vfts[transport_proto];
}

transport_service_type_t
transport_protocol_service_type (transport_proto_t tp)
{
  return tp_vfts[tp].transport_options.service_type;
}

transport_tx_fn_type_t
transport_protocol_tx_fn_type (transport_proto_t tp)
{
  return tp_vfts[tp].transport_options.tx_type;
}

void
transport_cleanup (transport_proto_t tp, u32 conn_index, u8 thread_index)
{
  tp_vfts[tp].cleanup (conn_index, thread_index);
}

void
transport_cleanup_half_open (transport_proto_t tp, u32 conn_index)
{
  if (tp_vfts[tp].cleanup_ho)
    tp_vfts[tp].cleanup_ho (conn_index);
}

int
transport_connect (transport_proto_t tp, transport_endpoint_cfg_t * tep)
{
  if (PREDICT_FALSE (!tp_vfts[tp].connect))
    return SESSION_E_TRANSPORT_NO_REG;
  return tp_vfts[tp].connect (tep);
}

void
transport_half_close (transport_proto_t tp, u32 conn_index, u8 thread_index)
{
  if (tp_vfts[tp].half_close)
    tp_vfts[tp].half_close (conn_index, thread_index);
}

void
transport_close (transport_proto_t tp, u32 conn_index, u8 thread_index)
{
  tp_vfts[tp].close (conn_index, thread_index);
}

void
transport_reset (transport_proto_t tp, u32 conn_index, u8 thread_index)
{
  if (tp_vfts[tp].reset)
    tp_vfts[tp].reset (conn_index, thread_index);
  else
    tp_vfts[tp].close (conn_index, thread_index);
}

u32
transport_start_listen (transport_proto_t tp, u32 session_index,
			transport_endpoint_cfg_t *tep)
{
  if (PREDICT_FALSE (!tp_vfts[tp].start_listen))
    return SESSION_E_TRANSPORT_NO_REG;
  return tp_vfts[tp].start_listen (session_index, tep);
}

u32
transport_stop_listen (transport_proto_t tp, u32 conn_index)
{
  return tp_vfts[tp].stop_listen (conn_index);
}

u8
transport_protocol_is_cl (transport_proto_t tp)
{
  return (tp_vfts[tp].transport_options.service_type == TRANSPORT_SERVICE_CL);
}

always_inline void
default_get_transport_endpoint (transport_connection_t * tc,
				transport_endpoint_t * tep, u8 is_lcl)
{
  if (is_lcl)
    {
      tep->port = tc->lcl_port;
      tep->is_ip4 = tc->is_ip4;
      clib_memcpy_fast (&tep->ip, &tc->lcl_ip, sizeof (tc->lcl_ip));
    }
  else
    {
      tep->port = tc->rmt_port;
      tep->is_ip4 = tc->is_ip4;
      clib_memcpy_fast (&tep->ip, &tc->rmt_ip, sizeof (tc->rmt_ip));
    }
}

void
transport_get_endpoint (transport_proto_t tp, u32 conn_index,
			clib_thread_index_t thread_index,
			transport_endpoint_t *tep, u8 is_lcl)
{
  if (tp_vfts[tp].get_transport_endpoint)
    tp_vfts[tp].get_transport_endpoint (conn_index, thread_index, tep,
					is_lcl);
  else
    {
      transport_connection_t *tc;
      tc = transport_get_connection (tp, conn_index, thread_index);
      default_get_transport_endpoint (tc, tep, is_lcl);
    }
}

void
transport_get_listener_endpoint (transport_proto_t tp, u32 conn_index,
				 transport_endpoint_t * tep, u8 is_lcl)
{
  if (tp_vfts[tp].get_transport_listener_endpoint)
    tp_vfts[tp].get_transport_listener_endpoint (conn_index, tep, is_lcl);
  else
    {
      transport_connection_t *tc;
      tc = transport_get_listener (tp, conn_index);
      default_get_transport_endpoint (tc, tep, is_lcl);
    }
}

int
transport_connection_attribute (transport_proto_t tp, u32 conn_index,
				u8 thread_index, u8 is_get,
				transport_endpt_attr_t *attr)
{
  if (!tp_vfts[tp].attribute)
    return -1;

  return tp_vfts[tp].attribute (conn_index, thread_index, is_get, attr);
}

#define PORT_MASK ((1 << 16)- 1)

void
transport_endpoint_free (u32 tepi)
{
  transport_main_t *tm = &tp_main;
  pool_put_index (tm->local_endpoints, tepi);
}

always_inline local_endpoint_t *
transport_endpoint_alloc (void)
{
  transport_main_t *tm = &tp_main;
  local_endpoint_t *lep;

  ASSERT (vlib_get_thread_index () <= transport_cl_thread ());

  pool_get_aligned_safe (tm->local_endpoints, lep, 0);
  return lep;
}

static void
transport_cleanup_freelist (void)
{
  transport_main_t *tm = &tp_main;
  local_endpoint_t *lep;
  u32 *lep_indexp;

  clib_spinlock_lock (&tm->local_endpoints_lock);

  vec_foreach (lep_indexp, tm->lcl_endpts_freelist)
    {
      lep = pool_elt_at_index (tm->local_endpoints, *lep_indexp);

      /* Port re-shared after attempt to cleanup */
      if (lep->refcnt > 0)
	continue;

      transport_endpoint_table_del (&tm->local_endpoints_table, lep->proto,
				    &lep->ep);
      transport_endpoint_free (*lep_indexp);
    }

  vec_reset_length (tm->lcl_endpts_freelist);

  tm->lcl_endpts_cleanup_pending = 0;

  clib_spinlock_unlock (&tm->local_endpoints_lock);
}

void
transport_program_endpoint_cleanup (u32 lepi)
{
  transport_main_t *tm = &tp_main;
  u8 flush_fl = 0;

  /* All workers can free connections. Synchronize access to freelist */
  clib_spinlock_lock (&tm->local_endpoints_lock);

  vec_add1 (tm->lcl_endpts_freelist, lepi);

  /* Avoid accumulating lots of endpoints for cleanup */
  if (!tm->lcl_endpts_cleanup_pending &&
      vec_len (tm->lcl_endpts_freelist) > 32)
    {
      tm->lcl_endpts_cleanup_pending = 1;
      flush_fl = 1;
    }

  clib_spinlock_unlock (&tm->local_endpoints_lock);

  if (flush_fl)
    session_send_rpc_evt_to_thread_force (transport_cl_thread (),
					  transport_cleanup_freelist, 0);
}

int
transport_release_local_endpoint (u8 proto, u32 fib_index,
				  ip46_address_t *lcl_ip, u16 port)
{
  transport_main_t *tm = &tp_main;
  local_endpoint_t *lep;
  u32 lepi;

  lepi = transport_endpoint_lookup (&tm->local_endpoints_table, proto,
				    fib_index, lcl_ip, port);
  if (lepi == ENDPOINT_INVALID_INDEX)
    return -1;

  /* First worker may be cleaning up ports so avoid touching free bitmap */
  lep = &tm->local_endpoints[lepi];
  ASSERT (lep->refcnt >= 1);

  /* Local endpoint no longer in use, program cleanup */
  if (!clib_atomic_sub_fetch (&lep->refcnt, 1))
    {
      transport_program_endpoint_cleanup (lepi);
      return 0;
    }

  /* Not an error, just in idication that endpoint was not cleaned up */
  return -1;
}

static int
transport_endpoint_mark_used (u8 proto, u32 fib_index, ip46_address_t *ip,
			      u16 port)
{
  transport_main_t *tm = &tp_main;
  local_endpoint_t *lep;
  u32 tei;

  ASSERT (vlib_get_thread_index () <= transport_cl_thread ());

  tei = transport_endpoint_lookup (&tm->local_endpoints_table, proto,
				   fib_index, ip, port);
  if (tei != ENDPOINT_INVALID_INDEX)
    return SESSION_E_PORTINUSE;

  /* Pool reallocs with worker barrier */
  lep = transport_endpoint_alloc ();
  clib_memcpy_fast (&lep->ep.ip, ip, sizeof (*ip));
  lep->ep.fib_index = fib_index;
  lep->ep.port = port;
  lep->proto = proto;
  lep->refcnt = 1;

  transport_endpoint_table_add (&tm->local_endpoints_table, proto, &lep->ep,
				lep - tm->local_endpoints);

  return 0;
}

void
transport_share_local_endpoint (u8 proto, u32 fib_index,
				ip46_address_t *lcl_ip, u16 port)
{
  transport_main_t *tm = &tp_main;
  local_endpoint_t *lep;
  u32 lepi;

  /* Active opens should call this only from a control thread, which are also
   * used to allocate and free ports. So, pool has only one writer and
   * potentially many readers. Listeners are allocated with barrier */
  lepi = transport_endpoint_lookup (&tm->local_endpoints_table, proto,
				    fib_index, lcl_ip, port);
  if (lepi != ENDPOINT_INVALID_INDEX)
    {
      lep = pool_elt_at_index (tm->local_endpoints, lepi);
      clib_atomic_add_fetch (&lep->refcnt, 1);
    }
}

/**
 * Allocate local port and add if successful add entry to local endpoint
 * table to mark the pair as used.
 *
 * @return port in net order or -1 if port cannot be allocated
 */
int
transport_alloc_local_port (u8 proto, ip46_address_t *lcl_addr,
			    transport_endpoint_cfg_t *rmt)
{
  transport_main_t *tm = &tp_main;
  u16 min = tm->port_allocator_min_src_port;
  u16 max = tm->port_allocator_max_src_port;
  int tries, limit, port = -1;

  limit = max - min;

  /* Only support active opens from one of ctrl threads */
  ASSERT (vlib_get_thread_index () <= transport_cl_thread ());

  /* Search for first free slot */
  for (tries = 0; tries < limit; tries++)
    {
      /* Find a port in the specified range */
      while (1)
	{
	  port = random_u32 (&tm->port_allocator_seed) & PORT_MASK;
	  if (PREDICT_TRUE (port >= min && port < max))
	    {
	      port = clib_host_to_net_u16 (port);
	      break;
	    }
	}

      if (!transport_endpoint_mark_used (proto, rmt->fib_index, lcl_addr,
					 port))
	break;

      /* IP:port pair already in use, check if 6-tuple available */
      if (session_lookup_6tuple (rmt->fib_index, lcl_addr, &rmt->ip, port,
				 rmt->port, proto, rmt->is_ip4))
	continue;

      /* 6-tuple is available so increment lcl endpoint refcount */
      transport_share_local_endpoint (proto, rmt->fib_index, lcl_addr, port);

      break;
    }

  tm->port_alloc_max_tries = clib_max (tm->port_alloc_max_tries, tries);

  return port;
}

u16
transport_port_alloc_max_tries ()
{
  transport_main_t *tm = &tp_main;
  return tm->port_alloc_max_tries;
}

u32
transport_port_local_in_use ()
{
  transport_main_t *tm = &tp_main;
  return pool_elts (tm->local_endpoints) - vec_len (tm->lcl_endpts_freelist);
}

void
transport_clear_stats ()
{
  transport_main_t *tm = &tp_main;
  tm->port_alloc_max_tries = 0;
}

static session_error_t
transport_get_interface_ip (u32 sw_if_index, u8 is_ip4, ip46_address_t * addr)
{
  if (is_ip4)
    {
      ip4_address_t *ip4;
      ip4 = ip_interface_get_first_ip (sw_if_index, 1);
      if (!ip4)
	return SESSION_E_NOIP;
      addr->ip4.as_u32 = ip4->as_u32;
    }
  else
    {
      ip6_address_t *ip6;
      ip6 = ip_interface_get_first_ip (sw_if_index, 0);
      if (ip6 == 0)
	return SESSION_E_NOIP;
      clib_memcpy_fast (&addr->ip6, ip6, sizeof (*ip6));
    }
  return 0;
}

static session_error_t
transport_find_local_ip_for_remote (u32 *sw_if_index,
				    transport_endpoint_t *rmt,
				    ip46_address_t *lcl_addr)
{
  fib_node_index_t fei;
  fib_prefix_t prefix;

  if (*sw_if_index == ENDPOINT_INVALID_INDEX)
    {
      /* Find a FIB path to the destination */
      clib_memcpy_fast (&prefix.fp_addr, &rmt->ip, sizeof (rmt->ip));
      prefix.fp_proto = rmt->is_ip4 ? FIB_PROTOCOL_IP4 : FIB_PROTOCOL_IP6;
      prefix.fp_len = rmt->is_ip4 ? 32 : 128;

      ASSERT (rmt->fib_index != ENDPOINT_INVALID_INDEX);
      fei = fib_table_lookup (rmt->fib_index, &prefix);

      /* Couldn't find route to destination. Bail out. */
      if (fei == FIB_NODE_INDEX_INVALID)
	return SESSION_E_NOROUTE;

      *sw_if_index = fib_entry_get_resolving_interface (fei);
      if (*sw_if_index == ENDPOINT_INVALID_INDEX)
	return SESSION_E_NOINTF;
    }

  clib_memset (lcl_addr, 0, sizeof (*lcl_addr));
  return transport_get_interface_ip (*sw_if_index, rmt->is_ip4, lcl_addr);
}

int
transport_alloc_local_endpoint (u8 proto, transport_endpoint_cfg_t * rmt_cfg,
				ip46_address_t * lcl_addr, u16 * lcl_port)
{
  transport_endpoint_t *rmt = (transport_endpoint_t *) rmt_cfg;
  transport_main_t *tm = &tp_main;
  session_error_t error;
  int port;

  /*
   * Find the local address
   */
  if (ip_is_zero (&rmt_cfg->peer.ip, rmt_cfg->peer.is_ip4))
    {
      error = transport_find_local_ip_for_remote (&rmt_cfg->peer.sw_if_index,
						  rmt, lcl_addr);
      if (error)
	return error;
    }
  else
    {
      /* Assume session layer vetted this address */
      clib_memcpy_fast (lcl_addr, &rmt_cfg->peer.ip,
			sizeof (rmt_cfg->peer.ip));
    }

  /* Cleanup freelist if need be */
  if (vec_len (tm->lcl_endpts_freelist))
    transport_cleanup_freelist ();

  /*
   * Allocate source port
   */
  if (rmt_cfg->peer.port == 0)
    {
      port = transport_alloc_local_port (proto, lcl_addr, rmt_cfg);
      if (port < 1)
	return SESSION_E_NOPORT;
      *lcl_port = port;
    }
  else
    {
      *lcl_port = rmt_cfg->peer.port;

      if (!transport_endpoint_mark_used (proto, rmt->fib_index, lcl_addr,
					 rmt_cfg->peer.port))
	return 0;

      /* IP:port pair already in use, check if 6-tuple available */
      if (session_lookup_6tuple (rmt->fib_index, lcl_addr, &rmt->ip,
				 rmt_cfg->peer.port, rmt->port, proto,
				 rmt->is_ip4))
	return SESSION_E_PORTINUSE;

      /* 6-tuple is available so increment lcl endpoint refcount */
      transport_share_local_endpoint (proto, rmt->fib_index, lcl_addr,
				      rmt_cfg->peer.port);

      return 0;
    }

  return 0;
}

u8 *
format_clib_us_time (u8 * s, va_list * args)
{
  clib_us_time_t t = va_arg (*args, clib_us_time_t);
  if (t < 1e3)
    s = format (s, "%u us", t);
  else
    s = format (s, "%.3f s", (f64) t * CLIB_US_TIME_PERIOD);
  return s;
}

u8 *
format_transport_pacer (u8 * s, va_list * args)
{
  spacer_t *pacer = va_arg (*args, spacer_t *);
  clib_thread_index_t thread_index = va_arg (*args, int);
  clib_us_time_t now, diff;

  now = transport_us_time_now (thread_index);
  diff = now - pacer->last_update;
  s = format (s, "rate %lu bucket %ld t/p %.3f last_update %U burst %u",
	      pacer->bytes_per_sec, pacer->bucket, pacer->tokens_per_period,
	      format_clib_us_time, diff, pacer->max_burst);
  return s;
}

static inline u32
spacer_max_burst (spacer_t * pacer, clib_us_time_t time_now)
{
  u64 n_periods = (time_now - pacer->last_update);
  i64 inc;

  if ((inc = (f32) n_periods * pacer->tokens_per_period) > 10)
    {
      pacer->last_update = time_now;
      pacer->bucket = clib_min (pacer->bucket + inc, (i64) pacer->max_burst);
    }

  return pacer->bucket >= 0 ? pacer->max_burst : 0;
}

static inline void
spacer_update_bucket (spacer_t * pacer, u32 bytes)
{
  pacer->bucket -= bytes;
}

static inline void
spacer_set_pace_rate (spacer_t * pacer, u64 rate_bytes_per_sec,
		      clib_us_time_t rtt, clib_time_type_t sec_per_loop)
{
  clib_us_time_t max_time;

  ASSERT (rate_bytes_per_sec != 0);
  pacer->bytes_per_sec = rate_bytes_per_sec;
  pacer->tokens_per_period = rate_bytes_per_sec * CLIB_US_TIME_PERIOD;

  /* Allow a min number of bursts per rtt, if their size is acceptable. Goal
   * is to spread the sending of data over the rtt but to also allow for some
   * coalescing that can potentially
   * 1) reduce load on session layer by reducing scheduling frequency for a
   *    session and
   * 2) optimize sending when tso if available
   *
   * Max "time-length" of a burst cannot be less than 1us or more than 1ms.
   */
  max_time = clib_max (rtt / TRANSPORT_PACER_BURSTS_PER_RTT,
		       (clib_us_time_t) (sec_per_loop * CLIB_US_TIME_FREQ));
  max_time = clib_clamp (max_time, 1 /* 1us */ , 1000 /* 1ms */ );
  pacer->max_burst = (rate_bytes_per_sec * max_time) * CLIB_US_TIME_PERIOD;
  pacer->max_burst = clib_clamp (pacer->max_burst, TRANSPORT_PACER_MIN_BURST,
				 TRANSPORT_PACER_MAX_BURST);
}

static inline u64
spacer_pace_rate (spacer_t * pacer)
{
  return pacer->bytes_per_sec;
}

static inline void
spacer_reset (spacer_t * pacer, clib_us_time_t time_now, u64 bucket)
{
  pacer->last_update = time_now;
  pacer->bucket = bucket;
}

void
transport_connection_tx_pacer_reset (transport_connection_t * tc,
				     u64 rate_bytes_per_sec, u32 start_bucket,
				     clib_us_time_t rtt)
{
  spacer_set_pace_rate (&tc->pacer, rate_bytes_per_sec, rtt,
			transport_seconds_per_loop (tc->thread_index));
  spacer_reset (&tc->pacer, transport_us_time_now (tc->thread_index),
		start_bucket);
}

void
transport_connection_tx_pacer_reset_bucket (transport_connection_t * tc,
					    u32 bucket)
{
  spacer_reset (&tc->pacer, transport_us_time_now (tc->thread_index), bucket);
}

void
transport_connection_tx_pacer_init (transport_connection_t * tc,
				    u64 rate_bytes_per_sec,
				    u32 initial_bucket)
{
  tc->flags |= TRANSPORT_CONNECTION_F_IS_TX_PACED;
  transport_connection_tx_pacer_reset (tc, rate_bytes_per_sec,
				       initial_bucket, 1e6);
}

void
transport_connection_tx_pacer_update (transport_connection_t * tc,
				      u64 bytes_per_sec, clib_us_time_t rtt)
{
  spacer_set_pace_rate (&tc->pacer, bytes_per_sec, rtt,
			transport_seconds_per_loop (tc->thread_index));
}

u32
transport_connection_tx_pacer_burst (transport_connection_t * tc)
{
  return spacer_max_burst (&tc->pacer,
			   transport_us_time_now (tc->thread_index));
}

u64
transport_connection_tx_pacer_rate (transport_connection_t * tc)
{
  return spacer_pace_rate (&tc->pacer);
}

void
transport_connection_update_tx_bytes (transport_connection_t * tc, u32 bytes)
{
  if (transport_connection_is_tx_paced (tc))
    spacer_update_bucket (&tc->pacer, bytes);
}

void
transport_connection_tx_pacer_update_bytes (transport_connection_t * tc,
					    u32 bytes)
{
  spacer_update_bucket (&tc->pacer, bytes);
}

void
transport_update_pacer_time (clib_thread_index_t thread_index,
			     clib_time_type_t now)
{
  session_wrk_update_time (session_main_get_worker (thread_index), now);
}

void
transport_connection_reschedule (transport_connection_t * tc)
{
  tc->flags &= ~TRANSPORT_CONNECTION_F_DESCHED;
  transport_connection_tx_pacer_reset_bucket (tc, 0 /* bucket */);
  if (transport_max_tx_dequeue (tc))
    sesssion_reschedule_tx (tc);
  else
    {
      session_t *s = session_get (tc->s_index, tc->thread_index);
      svm_fifo_unset_event (s->tx_fifo);
      if (svm_fifo_max_dequeue_cons (s->tx_fifo))
	if (svm_fifo_set_event (s->tx_fifo))
	  sesssion_reschedule_tx (tc);
    }
}

void
transport_fifos_init_ooo (transport_connection_t * tc)
{
  session_t *s = session_get (tc->s_index, tc->thread_index);
  svm_fifo_init_ooo_lookup (s->rx_fifo, 0 /* ooo enq */ );
  svm_fifo_init_ooo_lookup (s->tx_fifo, 1 /* ooo deq */ );
}

void
transport_update_time (clib_time_type_t time_now, u8 thread_index)
{
  transport_proto_vft_t *vft;
  vec_foreach (vft, tp_vfts)
  {
    if (vft->update_time)
      (vft->update_time) (time_now, thread_index);
  }
}

void
transport_enable_disable (vlib_main_t * vm, u8 is_en)
{
  transport_proto_vft_t *vft;
  vec_foreach (vft, tp_vfts)
  {
    if (vft->enable)
      if ((vft->enable) (vm, is_en) != 0)
	  continue;

    if (vft->update_time)
      session_register_update_time_fn (vft->update_time, is_en);
  }
}

void
transport_init (void)
{
  vlib_thread_main_t *vtm = vlib_get_thread_main ();
  session_main_t *smm = vnet_get_session_main ();
  transport_main_t *tm = &tp_main;
  u32 num_threads;

  if (smm->local_endpoints_table_buckets == 0)
    smm->local_endpoints_table_buckets = 250000;
  if (smm->local_endpoints_table_memory == 0)
    smm->local_endpoints_table_memory = 512 << 20;

  /* Initialize [port-allocator] random number seed */
  tm->port_allocator_seed = (u32) clib_cpu_time_now ();
  tm->port_allocator_min_src_port = smm->port_allocator_min_src_port;
  tm->port_allocator_max_src_port = smm->port_allocator_max_src_port;

  clib_bihash_init_24_8 (&tm->local_endpoints_table, "local endpoints table",
			 smm->local_endpoints_table_buckets,
			 smm->local_endpoints_table_memory);
  clib_spinlock_init (&tm->local_endpoints_lock);

  num_threads = 1 /* main thread */  + vtm->n_threads;
  if (num_threads > 1)
    {
      /* Main not polled if there are workers */
      smm->transport_cl_thread = 1;
    }
}

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
