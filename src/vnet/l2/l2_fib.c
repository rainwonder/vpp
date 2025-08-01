/*
 * l2_fib.c : layer 2 forwarding table (aka mac table)
 *
 * Copyright (c) 2013 Cisco and/or its affiliates.
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


#include <vlib/vlib.h>
#include <vnet/vnet.h>
#include <vnet/ethernet/ethernet.h>
#include <vlib/cli.h>

#include <vppinfra/error.h>
#include <vppinfra/hash.h>
#include <vnet/l2/l2_input.h>
#include <vnet/l2/l2_fib.h>
#include <vnet/l2/l2_learn.h>
#include <vnet/l2/l2_bd.h>
#include <vppinfra/bihash_template.c>
#include <vlibmemory/api.h>

#include <vnet/l2/l2.api_enum.h>
#include <vnet/l2/l2.api_types.h>

#define vl_endianfun
#include <vnet/l2/l2.api.h>
#undef vl_endianfun

/**
 * @file
 * @brief Ethernet MAC Address FIB Table Management.
 *
 * The MAC Address forwarding table for bridge-domains is called the l2fib.
 * Entries are added automatically as part of mac learning, but MAC Addresses
 * entries can also be added manually.
 *
 */

l2fib_main_t l2fib_main;

u8 *
format_l2fib_entry_result_flags (u8 * s, va_list * args)
{
  l2fib_entry_result_flags_t flags = va_arg (*args, int);

  if (L2FIB_ENTRY_RESULT_FLAG_NONE == flags)
    {
      s = format (s, "none");
    }
  else
    {
#define _(a,v,t) {                              \
      if (flags & L2FIB_ENTRY_RESULT_FLAG_##a)  \
        s = format (s, "%s ", t);               \
    }
      foreach_l2fib_entry_result_attr
#undef _
    }
  return (s);
}

static void
incr_mac_address (u8 * mac)
{
  u64 tmp = *((u64 *) mac);
  tmp = clib_net_to_host_u64 (tmp);
  tmp += 1 << 16;		/* skip unused (least significant) octets */
  tmp = clib_host_to_net_u64 (tmp);

  clib_memcpy_fast (mac, &tmp, 6);
}

/** Format sw_if_index. If the value is ~0, use the text "N/A" */
u8 *
format_vnet_sw_if_index_name_with_NA (u8 * s, va_list * args)
{
  vnet_main_t *vnm = va_arg (*args, vnet_main_t *);
  u32 sw_if_index = va_arg (*args, u32);
  if (sw_if_index == ~0)
    return format (s, "N/A");

  vnet_sw_interface_t *swif =
    vnet_get_sw_interface_or_null (vnm, sw_if_index);
  if (!swif)
    return format (s, "Stale");

  return format (s, "%U", format_vnet_sw_if_index_name, vnm, sw_if_index);
}

typedef struct l2fib_dump_walk_ctx_t_
{
  u32 bd_index;
  l2fib_entry_key_t *l2fe_key;
  l2fib_entry_result_t *l2fe_res;
} l2fib_dump_walk_ctx_t;

static int
l2fib_dump_walk_cb (BVT (clib_bihash_kv) * kvp, void *arg)
{
  l2fib_dump_walk_ctx_t *ctx = arg;
  l2fib_entry_result_t result;
  l2fib_entry_key_t key;

  key.raw = kvp->key;
  result.raw = kvp->value;

  if ((ctx->bd_index == ~0) || (ctx->bd_index == key.fields.bd_index))
    {
      vec_add1 (ctx->l2fe_key, key);
      vec_add1 (ctx->l2fe_res, result);
    }

  return (BIHASH_WALK_CONTINUE);
}

void
l2fib_table_dump (u32 bd_index,
		  l2fib_entry_key_t ** l2fe_key,
		  l2fib_entry_result_t ** l2fe_res)
{
  l2fib_main_t *msm = &l2fib_main;
  l2fib_dump_walk_ctx_t ctx = {
    .bd_index = bd_index,
  };

  BV (clib_bihash_foreach_key_value_pair)
    (&msm->mac_table, l2fib_dump_walk_cb, &ctx);

  *l2fe_key = ctx.l2fe_key;
  *l2fe_res = ctx.l2fe_res;
}

void
l2_fib_extract_seq_num (l2fib_seq_num_t sn, u8 * bd_sn, u8 * if_sn)
{
  *bd_sn = sn >> 8;
  *if_sn = sn & 0xff;
}

u8 *
format_l2_fib_seq_num (u8 * s, va_list * a)
{
  l2fib_seq_num_t sn = va_arg (*a, int);
  u8 bd_sn, if_sn;

  l2_fib_extract_seq_num (sn, &bd_sn, &if_sn);

  s = format (s, "%3d/%-3d", bd_sn, if_sn);

  return (s);
}

typedef struct l2fib_show_walk_ctx_t_
{
  u8 first_entry;
  u8 verbose;
  vlib_main_t *vm;
  vnet_main_t *vnm;
  u32 total_entries;
  u32 bd_index;
  u8 learn;
  u8 add;
  u8 now;
} l2fib_show_walk_ctx_t;

static int
l2fib_show_walk_cb (BVT (clib_bihash_kv) * kvp, void *arg)
{
  l2fib_show_walk_ctx_t *ctx = arg;
  l2_bridge_domain_t *bd_config;
  l2fib_entry_result_t result;
  l2fib_entry_key_t key;

  if (ctx->verbose && ctx->first_entry)
    {
      ctx->first_entry = 0;
      vlib_cli_output (ctx->vm,
		       "%=19s%=7s%=7s%=8s%=9s%=7s%=7s%=5s%=30s",
		       "Mac-Address", "BD-Idx", "If-Idx",
		       "BSN-ISN", "Age(min)", "static", "filter",
		       "bvi", "Interface-Name");
    }

  key.raw = kvp->key;
  result.raw = kvp->value;
  ctx->total_entries++;

  if (ctx->verbose &&
      ((ctx->bd_index >> 31) || (ctx->bd_index == key.fields.bd_index)))
    {
      u8 *s = NULL;

      if (ctx->learn && l2fib_entry_result_is_set_AGE_NOT (&result))
	return (BIHASH_WALK_CONTINUE);	/* skip provisioned macs */

      if (ctx->add && !l2fib_entry_result_is_set_AGE_NOT (&result))
	return (BIHASH_WALK_CONTINUE);	/* skip learned macs */

      bd_config = &vec_elt (l2input_main.bd_configs, key.fields.bd_index);

      if (l2fib_entry_result_is_set_AGE_NOT (&result))
	s = format (s, "no");
      else if (bd_config->mac_age == 0)
	s = format (s, "-");
      else
	{
	  i16 delta = ctx->now - result.fields.timestamp;
	  delta += delta < 0 ? 256 : 0;
	  s = format (s, "%d", delta);
	}

      vlib_cli_output (ctx->vm,
		       "%=19U%=7d%=7d %U%=9v%=7s%=7s%=5s%=30U",
		       format_ethernet_address, key.fields.mac,
		       key.fields.bd_index,
		       result.fields.sw_if_index == ~0
		       ? -1 : result.fields.sw_if_index,
		       format_l2_fib_seq_num, result.fields.sn, s,
		       l2fib_entry_result_is_set_STATIC (&result) ? "*" : "-",
		       l2fib_entry_result_is_set_FILTER (&result) ? "*" : "-",
		       l2fib_entry_result_is_set_BVI (&result) ? "*" : "-",
		       format_vnet_sw_if_index_name_with_NA,
		       ctx->vnm, result.fields.sw_if_index);
      vec_free (s);
    }

  return (BIHASH_WALK_CONTINUE);
}

/** Display the contents of the l2fib. */
static clib_error_t *
show_l2fib (vlib_main_t * vm,
	    unformat_input_t * input, vlib_cli_command_t * cmd)
{
  bd_main_t *bdm = &bd_main;
  l2fib_main_t *msm = &l2fib_main;
  u8 raw = 0;
  u32 bd_id;
  l2fib_show_walk_ctx_t ctx = {
    .first_entry = 1,
    .bd_index = ~0,
    .now = (u8) (vlib_time_now (vm) / 60),
    .vm = vm,
    .vnm = msm->vnet_main,
  };

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "raw"))
	{
	  raw = 1;
	  ctx.verbose = 0;
	  break;
	}
      else if (unformat (input, "verbose"))
	ctx.verbose = 1;
      else if (unformat (input, "all"))
	ctx.verbose = 1;
      else if (unformat (input, "bd_index %d", &ctx.bd_index))
	ctx.verbose = 1;
      else if (unformat (input, "learn"))
	{
	  ctx.add = 0;
	  ctx.learn = 1;
	  ctx.verbose = 1;
	}
      else if (unformat (input, "add"))
	{
	  ctx.learn = 0;
	  ctx.add = 1;
	  ctx.verbose = 1;
	}
      else if (unformat (input, "bd_id %d", &bd_id))
	{
	  uword *p = hash_get (bdm->bd_index_by_bd_id, bd_id);
	  if (p)
	    {
	      ctx.verbose = 1;
	      ctx.bd_index = p[0];
	    }
	  else
	    return clib_error_return (0,
				      "bridge domain id %d doesn't exist\n",
				      bd_id);
	}
      else
	break;
    }

  if (msm->mac_table_initialized == 0)
    {
      vlib_cli_output (vm, "no l2fib entries");
      return 0;
    }

  BV (clib_bihash_foreach_key_value_pair)
    (&msm->mac_table, l2fib_show_walk_cb, &ctx);

  if (ctx.total_entries == 0)
    vlib_cli_output (vm, "no l2fib entries");
  else
    {
      l2learn_main_t *lm = &l2learn_main;
      vlib_cli_output (vm, "L2FIB total/learned entries: %d/%d  "
		       "Last scan time: %.4esec  Learn limit: %d ",
		       ctx.total_entries, lm->global_learn_count,
		       msm->age_scan_duration, lm->global_learn_limit);
      if (lm->client_pid)
	vlib_cli_output (vm, "L2MAC events client PID: %d  "
			 "Last e-scan time: %.4esec  Delay: %.2esec  "
			 "Max macs in event: %d",
			 lm->client_pid, msm->evt_scan_duration,
			 msm->event_scan_delay, msm->max_macs_in_event);
    }

  if (raw)
    vlib_cli_output (vm, "Raw Hash Table:\n%U\n",
		     BV (format_bihash), &msm->mac_table, 1 /* verbose */ );

  return 0;
}

/*?
 * This command displays the MAC Address entries of the L2 FIB table.
 * Output can be filtered to just get the number of MAC Addresses or display
 * each MAC Address for all bridge domains or just a single bridge domain.
 *
 * @cliexpar
 * Example of how to display the number of MAC Address entries in the L2
 * FIB table:
 * @cliexstart{show l2fib}
 * 3 l2fib entries
 * @cliexend
 * Example of how to display all the MAC Address entries in the L2
 * FIB table:
 * @cliexstart{show l2fib all}
 *     Mac Address     BD Idx           Interface           Index  static  filter  bvi  refresh  timestamp
 *  52:54:00:53:18:33    1      GigabitEthernet0/8/0.200      3       0       0     0      0         0
 *  52:54:00:53:18:55    1      GigabitEthernet0/8/0.200      3       1       0     0      0         0
 *  52:54:00:53:18:77    1                 N/A                -1      1       1     0      0         0
 * 3 l2fib entries
 * @cliexend
?*/
VLIB_CLI_COMMAND (show_l2fib_cli, static) = {
  .path = "show l2fib",
  .short_help = "show l2fib [all] | [bd_id <nn> | bd_index <nn>] [learn | add] | [raw]",
  .function = show_l2fib,
};

void
l2fib_table_init (void)
{
  l2fib_main_t *mp = &l2fib_main;

  if (mp->mac_table_initialized == 1)
    return;

  BV (clib_bihash_init) (&mp->mac_table, "l2fib mac table",
			 mp->mac_table_n_buckets, mp->mac_table_memory_size);
  mp->mac_table_initialized = 1;
}

/* Remove all entries from the l2fib */
void
l2fib_clear_table (void)
{
  l2fib_main_t *mp = &l2fib_main;
  l2_bridge_domain_t *bd_config;

  if (mp->mac_table_initialized == 0)
    return;

  mp->mac_table_initialized = 0;

  /* Remove all entries */
  BV (clib_bihash_free) (&mp->mac_table);
  l2fib_table_init ();
  l2learn_main.global_learn_count = 0;
  vec_foreach (bd_config, l2input_main.bd_configs)
    bd_config->learn_count = 0;
}

/** Clear all entries in L2FIB.
 * @TODO: Later we may want a way to remove only the non-static entries
 */
static clib_error_t *
clear_l2fib (vlib_main_t * vm,
	     unformat_input_t * input, vlib_cli_command_t * cmd)
{
  l2fib_clear_table ();
  return 0;
}

/*?
 * This command clears all the MAC Address entries from the L2 FIB table.
 *
 * @cliexpar
 * Example of how to clear the L2 FIB Table:
 * @cliexcmd{clear l2fib}
 * Example to show the L2 FIB Table has been cleared:
 * @cliexstart{show l2fib verbose}
 * no l2fib entries
 * @cliexend
?*/
VLIB_CLI_COMMAND (clear_l2fib_cli, static) = {
  .path = "clear l2fib",
  .short_help = "clear l2fib",
  .function = clear_l2fib,
};

static l2fib_seq_num_t
l2fib_cur_seq_num (u32 bd_index, u32 sw_if_index)
{
  l2_bridge_domain_t *bd_config = l2input_bd_config (bd_index);

  return l2_fib_mk_seq_num (bd_config->seq_num,
			    l2_input_seq_num (sw_if_index));
}

/**
 * Add an entry to the l2fib.
 * If the entry already exists then overwrite it
 */
void
l2fib_add_entry (const u8 * mac, u32 bd_index,
		 u32 sw_if_index, l2fib_entry_result_flags_t flags)
{
  l2fib_entry_key_t key;
  l2fib_entry_result_t result;
  __attribute__ ((unused)) u32 bucket_contents;
  l2fib_main_t *fm = &l2fib_main;
  l2learn_main_t *lm = &l2learn_main;
  BVT (clib_bihash_kv) kv;

  if (fm->mac_table_initialized == 0)
    l2fib_table_init ();

  /* set up key */
  key.raw = l2fib_make_key (mac, bd_index);
  kv.key = key.raw;

  /* check if entry already exist */
  if (BV (clib_bihash_search) (&fm->mac_table, &kv, &kv))
    {
      /* decrement counter if overwriting a learned mac  */
      result.raw = kv.value;
      if (!l2fib_entry_result_is_set_AGE_NOT (&result))
	{
	  l2_bridge_domain_t *bd_config =
	    vec_elt_at_index (l2input_main.bd_configs, bd_index);

	  /* check if learn_count == 0 in case of race condition between 2
	   * workers adding an entry simultaneously */
	  /* learn_count variable may have little inaccuracy because they are
	   * not incremented/decremented with atomic operations */
	  /* l2fib_scan is call every 2sec fixing potential inaccuracy */
	  if (lm->global_learn_count)
	    lm->global_learn_count--;
	  if (bd_config->learn_count)
	    bd_config->learn_count--;
	}
    }

  /* set up result */
  result.raw = 0;		/* clear all fields */
  result.fields.sw_if_index = sw_if_index;
  result.fields.flags = flags;

  /* no aging for provisioned entry */
  l2fib_entry_result_set_AGE_NOT (&result);

  kv.value = result.raw;

  BV (clib_bihash_add_del) (&fm->mac_table, &kv, 1 /* is_add */ );
}

/**
 * Add an entry to the L2FIB.
 * The CLI format is:
 *    l2fib add <mac> <bd> <intf> [static] [bvi]
 *    l2fib add <mac> <bd> filter
 * Note that filter and bvi entries are always static
 */
static clib_error_t *
l2fib_add (vlib_main_t * vm,
	   unformat_input_t * input, vlib_cli_command_t * cmd)
{
  bd_main_t *bdm = &bd_main;
  vnet_main_t *vnm = vnet_get_main ();
  clib_error_t *error = 0;
  u8 mac[6];
  u32 bd_id;
  u32 bd_index;
  u32 sw_if_index = ~0;
  uword *p;
  l2fib_entry_result_flags_t flags;

  flags = L2FIB_ENTRY_RESULT_FLAG_NONE;

  if (!unformat (input, "%U", unformat_ethernet_address, mac))
    {
      error = clib_error_return (0, "expected mac address `%U'",
				 format_unformat_error, input);
      goto done;
    }

  if (!unformat (input, "%d", &bd_id))
    {
      error = clib_error_return (0, "expected bridge domain ID `%U'",
				 format_unformat_error, input);
      goto done;
    }

  p = hash_get (bdm->bd_index_by_bd_id, bd_id);
  if (!p)
    {
      error = clib_error_return (0, "bridge domain ID %d invalid", bd_id);
      goto done;
    }
  bd_index = p[0];

  if (unformat (input, "filter"))
    {
      l2fib_add_filter_entry (mac, bd_index);
      return 0;
    }

  if (!unformat_user (input, unformat_vnet_sw_interface, vnm, &sw_if_index))
    {
      error = clib_error_return (0, "unknown interface `%U'",
				 format_unformat_error, input);
      goto done;
    }

  if (unformat (input, "static"))
    flags |= L2FIB_ENTRY_RESULT_FLAG_STATIC;
  else if (unformat (input, "bvi"))
    flags |= (L2FIB_ENTRY_RESULT_FLAG_STATIC | L2FIB_ENTRY_RESULT_FLAG_BVI);

  if (vec_len (l2input_main.configs) <= sw_if_index)
    {
      error = clib_error_return (0, "Interface sw_if_index %d not in L2 mode",
				 sw_if_index);
      goto done;
    }

  l2fib_add_entry (mac, bd_index, sw_if_index, flags);

done:
  return error;
}

/*?
 * This command adds a MAC Address entry to the L2 FIB table
 * of an existing bridge-domain. The MAC Address can be static
 * or dynamic. This command also allows a filter to be added,
 * such that packets with given MAC Addresses (source mac or
 * destination mac match) are dropped.
 *
 * @cliexpar
 * Example of how to add a dynamic MAC Address entry to the L2 FIB table
 * of a bridge-domain (where 200 is the bridge-domain-id):
 * @cliexcmd{l2fib add 52:54:00:53:18:33 200 GigabitEthernet0/8/0.200}
 * Example of how to add a static MAC Address entry to the L2 FIB table
 * of a bridge-domain (where 200 is the bridge-domain-id):
 * @cliexcmd{l2fib add 52:54:00:53:18:55 200 GigabitEthernet0/8/0.200 static}
 * Example of how to add a filter such that a packet with the given MAC
 * Address will be dropped in a given bridge-domain (where 200 is the
 * bridge-domain-id):
 * @cliexcmd{l2fib add 52:54:00:53:18:77 200 filter}
 * Example of show command of the provisioned MAC Addresses and filters:
 * @cliexstart{show l2fib verbose}
 *     Mac Address     BD Idx           Interface           Index  static  filter  bvi  refresh  timestamp
 *  52:54:00:53:18:33    1      GigabitEthernet0/8/0.200      3       0       0     0      0         0
 *  52:54:00:53:18:55    1      GigabitEthernet0/8/0.200      3       1       0     0      0         0
 *  52:54:00:53:18:77    1                 N/A                -1      1       1     0      0         0
 * 3 l2fib entries
 * @cliexend
?*/
VLIB_CLI_COMMAND (l2fib_add_cli, static) = {
  .path = "l2fib add",
  .short_help = "l2fib add <mac> <bridge-domain-id> filter | <intf> [static | bvi]",
  .function = l2fib_add,
};


static clib_error_t *
l2fib_test_command_fn (vlib_main_t * vm,
		       unformat_input_t * input, vlib_cli_command_t * cmd)
{
  u8 mac[8], save_mac[6];
  u32 bd_index = 0;
  u32 sw_if_index = 8;
  u32 is_add = 0;
  u32 is_del = 0;
  u32 is_check = 0;
  u32 count = 1;
  int mac_set = 0;
  int i;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "mac %U", unformat_ethernet_address, mac))
	mac_set = 1;
      else if (unformat (input, "add"))
	is_add = 1;
      else if (unformat (input, "del"))
	is_del = 1;
      else if (unformat (input, "check"))
	is_check = 1;
      else if (unformat (input, "count %d", &count))
	;
      else
	break;
    }

  if (mac_set == 0)
    return clib_error_return (0, "mac not set");

  if (is_add == 0 && is_del == 0 && is_check == 0)
    return clib_error_return (0,
			      "noop: pick at least one of (add,del,check)");

  clib_memcpy_fast (save_mac, mac, 6);

  if (is_add)
    {
      for (i = 0; i < count; i++)
	{
	  l2fib_add_entry (mac, bd_index, sw_if_index,
			   L2FIB_ENTRY_RESULT_FLAG_NONE);
	  incr_mac_address (mac);
	}
    }

  if (is_check)
    {
      BVT (clib_bihash_kv) kv;
      l2fib_main_t *mp = &l2fib_main;

      if (mp->mac_table_initialized == 0)
	return clib_error_return (0, "mac table is not initialized");

      clib_memcpy_fast (mac, save_mac, 6);

      for (i = 0; i < count; i++)
	{
	  kv.key = l2fib_make_key (mac, bd_index);
	  if (BV (clib_bihash_search) (&mp->mac_table, &kv, &kv))
	    {
	      clib_warning ("key %U AWOL", format_ethernet_address, mac);
	      break;
	    }
	  incr_mac_address (mac);
	}
    }

  if (is_del)
    {
      clib_memcpy_fast (mac, save_mac, 6);

      for (i = 0; i < count; i++)
	{
	  l2fib_del_entry (mac, bd_index, 0);
	  incr_mac_address (mac);
	}
    }

  return 0;
}

/*?
 * The set of '<em>test l2fib</em>' commands allow the L2 FIB table of the default
 * bridge domain (bridge-domain-id of 0) to be modified.
 *
 * @cliexpar
 * @parblock
 * Example of how to add a set of 4 sequential MAC Address entries to L2
 * FIB table of the default bridge-domain:
 * @cliexcmd{test l2fib add mac 52:54:00:53:00:00 count 4}
 *
 * Show the set of 4 sequential MAC Address entries that were added:
 * @cliexstart{show l2fib verbose}
 *     Mac Address     BD Idx           Interface           Index  static  filter  bvi  refresh  timestamp
 * 52:54:00:53:00:00    0       GigabitEthernet0/8/0.300     8       0       0     0      0         0
 * 52:54:00:53:00:01    0       GigabitEthernet0/8/0.300     8       0       0     0      0         0
 * 52:54:00:53:00:03    0       GigabitEthernet0/8/0.300     8       0       0     0      0         0
 * 52:54:00:53:00:02    0       GigabitEthernet0/8/0.300     8       0       0     0      0         0
 * 4 l2fib entries
 * @cliexend
 *
 * Example of how to check that the set of 4 sequential MAC Address
 * entries were added to L2 FIB table of the default
 * bridge-domain. Used a count of 5 to produce an error:
 *
 * @cliexcmd{test l2fib check mac 52:54:00:53:00:00 count 5}
 * The output of the check command is in the log files. Log file
 * location may vary based on your OS and Version:
 *
 * <b><em># tail -f /var/log/messages | grep l2fib_test_command_fn</em></b>
 *
 * Sep  7 17:15:24 localhost vnet[4952]: l2fib_test_command_fn:446: key 52:54:00:53:00:04 AWOL
 *
 * Example of how to delete a set of 4 sequential MAC Address entries
 * from L2 FIB table of the default bridge-domain:
 * @cliexcmd{test l2fib del mac 52:54:00:53:00:00 count 4}
 * @endparblock
?*/
VLIB_CLI_COMMAND (l2fib_test_command, static) = {
  .path = "test l2fib",
  .short_help = "test l2fib [add|del|check] mac <base-addr> count <nn>",
  .function = l2fib_test_command_fn,
};


/**
 * Delete an entry from the l2fib.
 * Return 0 if the entry was deleted, or 1 it was not found or if
 * sw_if_index is non-zero and does not match that in the entry.
 */
u32
l2fib_del_entry (const u8 * mac, u32 bd_index, u32 sw_if_index)
{
  l2fib_entry_result_t result;
  l2fib_main_t *mp = &l2fib_main;
  BVT (clib_bihash_kv) kv;

  if (mp->mac_table_initialized == 0)
    return 1;

  /* set up key */
  kv.key = l2fib_make_key (mac, bd_index);

  if (BV (clib_bihash_search) (&mp->mac_table, &kv, &kv))
    return 1;

  result.raw = kv.value;

  /*  check if sw_if_index of entry match */
  if ((sw_if_index != 0) && (sw_if_index != result.fields.sw_if_index))
    return 1;

  /* decrement counter if dynamically learned mac */
  if (!l2fib_entry_result_is_set_AGE_NOT (&result))
    {
      l2_bridge_domain_t *bd_config =
	vec_elt_at_index (l2input_main.bd_configs, bd_index);
      if (l2learn_main.global_learn_count)
	l2learn_main.global_learn_count--;
      if (bd_config->learn_count)
	bd_config->learn_count--;
    }

  /* Remove entry from hash table */
  BV (clib_bihash_add_del) (&mp->mac_table, &kv, 0 /* is_add */ );
  return 0;
}

/**
 * Delete an entry from the L2FIB.
 * The CLI format is:
 *    l2fib del <mac> <bd-id>
 */
static clib_error_t *
l2fib_del (vlib_main_t * vm,
	   unformat_input_t * input, vlib_cli_command_t * cmd)
{
  bd_main_t *bdm = &bd_main;
  clib_error_t *error = 0;
  u8 mac[6];
  u32 bd_id;
  u32 bd_index;
  uword *p;

  if (!unformat (input, "%U", unformat_ethernet_address, mac))
    {
      error = clib_error_return (0, "expected mac address `%U'",
				 format_unformat_error, input);
      goto done;
    }

  if (!unformat (input, "%d", &bd_id))
    {
      error = clib_error_return (0, "expected bridge domain ID `%U'",
				 format_unformat_error, input);
      goto done;
    }

  p = hash_get (bdm->bd_index_by_bd_id, bd_id);
  if (!p)
    {
      error = clib_error_return (0, "bridge domain ID %d invalid", bd_id);
      goto done;
    }
  bd_index = p[0];

  /* Delete the entry */
  if (l2fib_del_entry (mac, bd_index, 0))
    {
      error = clib_error_return (0, "mac entry not found");
      goto done;
    }

done:
  return error;
}

/*?
 * This command deletes an existing MAC Address entry from the L2 FIB
 * table of an existing bridge-domain.
 *
 * @cliexpar
 * Example of how to delete a MAC Address entry from the L2 FIB table of a bridge-domain (where 200 is the bridge-domain-id):
 * @cliexcmd{l2fib del 52:54:00:53:18:33 200}
?*/
VLIB_CLI_COMMAND (l2fib_del_cli, static) = {
  .path = "l2fib del",
  .short_help = "l2fib del <mac> <bridge-domain-id> []",
  .function = l2fib_del,
};

static clib_error_t *
l2fib_set_scan_delay (vlib_main_t *vm, unformat_input_t *input,
		      vlib_cli_command_t *cmd)
{
  clib_error_t *error = 0;
  u32 scan_delay;
  l2fib_main_t *fm = &l2fib_main;

  if (!unformat (input, "%d", &scan_delay))
    {
      error = clib_error_return (0, "expecting delay but got `%U'",
				 format_unformat_error, input);
      goto done;
    }
  fm->event_scan_delay = (f64) (scan_delay) *10e-3;
  l2fib_flush_all_mac (vlib_get_main ());
done:
  return error;
}

/*?
 * This command set scan delay (in 1/10s unit)
 *
?*/
VLIB_CLI_COMMAND (l2fib_set_scan_delay_cli, static) = {
  .path = "set l2fib scan-delay",
  .short_help = "set l2fib scan-delay <delay>",
  .function = l2fib_set_scan_delay,
};

/**
    Kick off ager to scan MACs to age/delete MAC entries
*/
void
l2fib_start_ager_scan (vlib_main_t * vm)
{
  uword evt = L2_MAC_AGE_PROCESS_EVENT_ONE_PASS;

  /* check if there is at least one bd with mac aging enabled */
  l2_bridge_domain_t *bd_config;
  vec_foreach (bd_config, l2input_main.bd_configs)
  {
    if (bd_config->bd_id != ~0 && bd_config->mac_age != 0)
      {
	evt = L2_MAC_AGE_PROCESS_EVENT_START;
	break;
      }
  }

  vlib_process_signal_event (vm, l2fib_mac_age_scanner_process_node.index,
			     evt, 0);
}

/**
    Flush all non static MACs from an interface
*/
void
l2fib_flush_int_mac (vlib_main_t * vm, u32 sw_if_index)
{
  l2_input_seq_num_inc (sw_if_index);
  l2fib_start_ager_scan (vm);
}

static void
l2fib_bd_seq_num_inc (u32 bd_index)
{
  l2_bridge_domain_t *bd_config = l2input_bd_config (bd_index);

  bd_config->seq_num += 1;
  bd_input_walk (bd_index, l2input_recache, NULL);
}

/**
    Flush all non static MACs in a bridge domain
*/
void
l2fib_flush_bd_mac (vlib_main_t * vm, u32 bd_index)
{
  l2fib_bd_seq_num_inc (bd_index);
  l2fib_start_ager_scan (vm);
}

/**
    Flush all non static MACs - flushes all valid BDs
*/
void
l2fib_flush_all_mac (vlib_main_t * vm)
{
  l2input_main_t *mp = &l2input_main;
  l2_bridge_domain_t *bd_config;
  u32 bd_index;

  vec_foreach_index (bd_index, mp->bd_configs)
  {
    bd_config = vec_elt_at_index (mp->bd_configs, bd_index);
    if (bd_is_valid (bd_config))
      l2fib_bd_seq_num_inc (bd_index);
  }

  l2fib_start_ager_scan (vm);
}


/**
    Flush MACs, except static ones, associated with an interface
    The CLI format is:
    l2fib flush-mac interface <if-name>
*/
static clib_error_t *
l2fib_flush_mac_int (vlib_main_t * vm,
		     unformat_input_t * input, vlib_cli_command_t * cmd)
{
  vnet_main_t *vnm = vnet_get_main ();
  clib_error_t *error = 0;
  u32 sw_if_index;

  if (!unformat_user (input, unformat_vnet_sw_interface, vnm, &sw_if_index))
    {
      error = clib_error_return (0, "unknown interface `%U'",
				 format_unformat_error, input);
      goto done;
    }

  l2fib_flush_int_mac (vm, sw_if_index);

done:
  return error;
}

/**
    Flush all MACs, except static ones
    The CLI format is:
    l2fib flush-mac all
*/
static clib_error_t *
l2fib_flush_mac_all (vlib_main_t * vm,
		     unformat_input_t * input, vlib_cli_command_t * cmd)
{
  l2fib_flush_all_mac (vm);
  return 0;
}

/*?
 * This command kick off ager to delete all existing MAC Address entries,
 * except static ones, associated with an interface from the L2 FIB table.
 *
 * @cliexpar
 * Example of how to flush MAC Address entries learned on an interface from the L2 FIB table:
 * @cliexcmd{l2fib flush-mac interface GigabitEthernet2/1/0}
?*/
VLIB_CLI_COMMAND (l2fib_flush_mac_all_cli, static) = {
  .path = "l2fib flush-mac all",
  .short_help = "l2fib flush-mac all",
  .function = l2fib_flush_mac_all,
};

/*?
 * This command kick off ager to delete all existing MAC Address entries,
 * except static ones, associated with an interface from the L2 FIB table.
 *
 * @cliexpar
 * Example of how to flush MAC Address entries learned on an interface from the L2 FIB table:
 * @cliexcmd{l2fib flush-mac interface GigabitEthernet2/1/0}
?*/
VLIB_CLI_COMMAND (l2fib_flush_mac_int_cli, static) = {
  .path = "l2fib flush-mac interface",
  .short_help = "l2fib flush-mac interface <if-name>",
  .function = l2fib_flush_mac_int,
};

/**
    Flush bridge-domain MACs except static ones.
    The CLI format is:
    l2fib flush-mac bridge-domain <bd-id>
*/
static clib_error_t *
l2fib_flush_mac_bd (vlib_main_t * vm,
		    unformat_input_t * input, vlib_cli_command_t * cmd)
{
  bd_main_t *bdm = &bd_main;
  clib_error_t *error = 0;
  u32 bd_index, bd_id;
  uword *p;

  if (!unformat (input, "%d", &bd_id))
    {
      error = clib_error_return (0, "expecting bridge-domain id but got `%U'",
				 format_unformat_error, input);
      goto done;
    }

  p = hash_get (bdm->bd_index_by_bd_id, bd_id);
  if (p)
    bd_index = *p;
  else
    return clib_error_return (0, "No such bridge domain %d", bd_id);

  l2fib_flush_bd_mac (vm, bd_index);

done:
  return error;
}

/*?
 * This command kick off ager to delete all existing MAC Address entries,
 * except static ones, in a bridge domain from the L2 FIB table.
 *
 * @cliexpar
 * Example of how to flush MAC Address entries learned in a bridge domain from the L2 FIB table:
 * @cliexcmd{l2fib flush-mac bridge-domain 1000}
?*/
VLIB_CLI_COMMAND (l2fib_flush_mac_bd_cli, static) = {
  .path = "l2fib flush-mac bridge-domain",
  .short_help = "l2fib flush-mac bridge-domain <bd-id>",
  .function = l2fib_flush_mac_bd,
};

clib_error_t *
l2fib_sw_interface_up_down (vnet_main_t * vnm, u32 sw_if_index, u32 flags)
{
  l2_input_config_t *config = l2input_intf_config (sw_if_index);
  if ((flags & VNET_SW_INTERFACE_FLAG_ADMIN_UP) == 0 &&
      l2_input_is_bridge (config))
    l2fib_flush_int_mac (vnm->vlib_main, sw_if_index);
  return 0;
}

VNET_SW_INTERFACE_ADMIN_UP_DOWN_FUNCTION (l2fib_sw_interface_up_down);

BVT (clib_bihash) * get_mac_table (void)
{
  l2fib_main_t *mp = &l2fib_main;
  return &mp->mac_table;
}

static_always_inline void *
allocate_mac_evt_buf (u32 client, u32 client_index)
{
  l2fib_main_t *fm = &l2fib_main;
  vl_api_l2_macs_event_t *mp = vl_msg_api_alloc
    (sizeof (*mp) + (fm->max_macs_in_event * sizeof (vl_api_mac_entry_t)));
  mp->_vl_msg_id = htons (l2input_main.msg_id_base + VL_API_L2_MACS_EVENT);
  mp->pid = htonl (client);
  mp->client_index = client_index;
  return mp;
}

static_always_inline f64
l2fib_scan (vlib_main_t * vm, f64 start_time, u8 event_only)
{
  l2fib_main_t *fm = &l2fib_main;
  l2learn_main_t *lm = &l2learn_main;

  BVT (clib_bihash) * h = &fm->mac_table;
  int i, j, k;
  f64 last_start = start_time;
  f64 accum_t = 0;
  f64 delta_t = 0;
  u32 evt_idx = 0;
  u32 learn_count = 0;
  u32 client = lm->client_pid;
  u32 cl_idx = lm->client_index;
  vl_api_l2_macs_event_t *mp = 0;
  vl_api_registration_t *reg = 0;
  u32 bd_index;
  static u32 *bd_learn_counts = 0;

  /* Don't scan the l2 fib if it hasn't been instantiated yet */
  if (alloc_arena (h) == 0)
    return 0.0;

  vec_reset_length (bd_learn_counts);
  vec_validate (bd_learn_counts, vec_len (l2input_main.bd_configs) - 1);

  if (client)
    {
      mp = allocate_mac_evt_buf (client, cl_idx);
      reg = vl_api_client_index_to_registration (lm->client_index);
    }

  for (i = 0; i < h->nbuckets; i++)
    {
      /* allow no more than 20us without a pause */
      delta_t = vlib_time_now (vm) - last_start;
      if (delta_t > 20e-6)
	{
	  vlib_process_suspend (vm, 100e-6);	/* suspend for 100 us */
	  /* in case a new bd was created while sleeping */
	  vec_validate (bd_learn_counts,
			vec_len (l2input_main.bd_configs) - 1);
	  last_start = vlib_time_now (vm);
	  accum_t += delta_t;
	}

      if (i < (h->nbuckets - 3))
	{
	  BVT (clib_bihash_bucket) * b =
	    BV (clib_bihash_get_bucket) (h, i + 3);
	  clib_prefetch_load (b);
	  b = BV (clib_bihash_get_bucket) (h, i + 1);
	  if (!BV (clib_bihash_bucket_is_empty) (b))
	    {
	      BVT (clib_bihash_value) * v =
		BV (clib_bihash_get_value) (h, b->offset);
	      clib_prefetch_load (v);
	    }
	}

      BVT (clib_bihash_bucket) * b = BV (clib_bihash_get_bucket) (h, i);
      if (BV (clib_bihash_bucket_is_empty) (b))
	continue;
      BVT (clib_bihash_value) * v = BV (clib_bihash_get_value) (h, b->offset);
      for (j = 0; j < (1 << b->log2_pages); j++)
	{
	  for (k = 0; k < BIHASH_KVP_PER_PAGE; k++)
	    {
	      if (BV (clib_bihash_is_free) (&v->kvp[k]))
		continue;

	      l2fib_entry_key_t key = {.raw = v->kvp[k].key };
	      l2fib_entry_result_t result = {.raw = v->kvp[k].value };

	      if (!l2fib_entry_result_is_set_AGE_NOT (&result))
		{
		  learn_count++;
		  vec_elt (bd_learn_counts, key.fields.bd_index)++;
		}

	      if (client)
		{
		  if (PREDICT_FALSE (evt_idx >= fm->max_macs_in_event))
		    {
		      /* event message full, send it and start a new one */
		      if (reg && vl_api_can_send_msg (reg))
			{
			  mp->n_macs = htonl (evt_idx);
			  vl_api_send_msg (reg, (u8 *) mp);
			  mp = allocate_mac_evt_buf (client, cl_idx);
			}
		      else
			{
			  if (reg)
			    clib_warning ("MAC event to pid %d queue stuffed!"
					  " %d MAC entries lost", client,
					  evt_idx);
			}
		      evt_idx = 0;
		    }

		  if (l2fib_entry_result_is_set_LRN_EVT (&result))
		    {
		      /* copy mac entry to event msg */
		      clib_memcpy_fast (mp->mac[evt_idx].mac_addr,
					key.fields.mac, 6);
		      mp->mac[evt_idx].action =
			l2fib_entry_result_is_set_LRN_MOV (&result) ?
			(vl_api_mac_event_action_t) MAC_EVENT_ACTION_MOVE
			: (vl_api_mac_event_action_t) MAC_EVENT_ACTION_ADD;
		      mp->mac[evt_idx].action =
			htonl (mp->mac[evt_idx].action);
		      mp->mac[evt_idx].sw_if_index =
			htonl (result.fields.sw_if_index);
		      /* clear event bits and update mac entry */
		      l2fib_entry_result_clear_LRN_EVT (&result);
		      l2fib_entry_result_clear_LRN_MOV (&result);
		      BVT (clib_bihash_kv) kv;
		      kv.key = key.raw;
		      kv.value = result.raw;
		      BV (clib_bihash_add_del) (&fm->mac_table, &kv, 1);
		      evt_idx++;
		      continue;	/* skip aging */
		    }
		}

	      if (event_only || l2fib_entry_result_is_set_AGE_NOT (&result))
		continue;	/* skip aging - static_mac always age_not */

	      /* start aging processing */
	      u32 bd_index = key.fields.bd_index;
	      u32 sw_if_index = result.fields.sw_if_index;
	      u16 sn = l2fib_cur_seq_num (bd_index, sw_if_index);
	      if (result.fields.sn != sn)
		goto age_out;	/* stale mac */

	      l2_bridge_domain_t *bd_config =
		vec_elt_at_index (l2input_main.bd_configs, bd_index);

	      if (bd_config->mac_age == 0)
		continue;	/* skip aging */

	      i16 delta = (u8) (start_time / 60) - result.fields.timestamp;
	      delta += delta < 0 ? 256 : 0;

	      if (delta < bd_config->mac_age)
		continue;	/* still valid */

	    age_out:
	      if (client)
		{
		  /* copy mac entry to event msg */
		  clib_memcpy_fast (mp->mac[evt_idx].mac_addr, key.fields.mac,
				    6);
		  mp->mac[evt_idx].action =
		    (vl_api_mac_event_action_t) MAC_EVENT_ACTION_DELETE;
		  mp->mac[evt_idx].action = htonl (mp->mac[evt_idx].action);
		  mp->mac[evt_idx].sw_if_index =
		    htonl (result.fields.sw_if_index);
		  evt_idx++;
		}
	      /* delete mac entry */
	      BVT (clib_bihash_kv) kv;
	      kv.key = key.raw;
	      BV (clib_bihash_add_del) (&fm->mac_table, &kv, 0);
	      learn_count--;
	      vec_elt (bd_learn_counts, key.fields.bd_index)--;
	      /*
	       * Note: we may have just freed the bucket's backing
	       * storage, so check right here...
	       */
	      if (BV (clib_bihash_bucket_is_empty) (b))
		goto doublebreak;
	    }
	  v++;
	}
    doublebreak:
      ;
    }

  /* keep learn count consistent */
  l2learn_main.global_learn_count = learn_count;
  vec_foreach_index (bd_index, l2input_main.bd_configs)
    {
      vec_elt (l2input_main.bd_configs, bd_index).learn_count =
	vec_elt (bd_learn_counts, bd_index);
    }

  if (mp)
    {
      /*  send any outstanding mac event message else free message buffer */
      if (evt_idx)
	{
	  if (reg && vl_api_can_send_msg (reg))
	    {
	      mp->n_macs = htonl (evt_idx);
	      vl_api_send_msg (reg, (u8 *) mp);
	    }
	  else
	    {
	      if (reg)
		clib_warning ("MAC event to pid %d queue stuffed!"
			      " %d MAC entries lost", client, evt_idx);
	      vl_msg_api_free (mp);
	    }
	}
      else
	vl_msg_api_free (mp);
    }
  return delta_t + accum_t;
}

static uword
l2fib_mac_age_scanner_process (vlib_main_t * vm, vlib_node_runtime_t * rt,
			       vlib_frame_t * f)
{
  uword event_type, *event_data = 0;
  l2fib_main_t *fm = &l2fib_main;
  l2learn_main_t *lm = &l2learn_main;
  bool enabled = 0;
  f64 start_time, next_age_scan_time = CLIB_TIME_MAX;

  while (1)
    {
      if (lm->client_pid)
	vlib_process_wait_for_event_or_clock (vm, fm->event_scan_delay);
      else if (enabled)
	{
	  f64 t = next_age_scan_time - vlib_time_now (vm);
	  vlib_process_wait_for_event_or_clock (vm, t);
	}
      else
	vlib_process_wait_for_event (vm);

      event_type = vlib_process_get_events (vm, &event_data);
      vec_reset_length (event_data);

      start_time = vlib_time_now (vm);
      enum
      { SCAN_MAC_AGE, SCAN_MAC_EVENT, SCAN_DISABLE } scan = SCAN_MAC_AGE;

      switch (event_type)
	{
	case ~0:		/* timer expired */
	  if (lm->client_pid != 0 && start_time < next_age_scan_time)
	    scan = SCAN_MAC_EVENT;
	  break;

	case L2_MAC_AGE_PROCESS_EVENT_START:
	  enabled = 1;
	  break;

	case L2_MAC_AGE_PROCESS_EVENT_STOP:
	  enabled = 0;
	  scan = SCAN_DISABLE;
	  break;

	case L2_MAC_AGE_PROCESS_EVENT_ONE_PASS:
	  break;

	default:
	  ASSERT (0);
	}

      if (scan == SCAN_MAC_EVENT)
	l2fib_main.evt_scan_duration = l2fib_scan (vm, start_time, 1);
      else
	{
	  if (scan == SCAN_MAC_AGE)
	    l2fib_main.age_scan_duration = l2fib_scan (vm, start_time, 0);
	  if (scan == SCAN_DISABLE)
	    {
	      l2fib_main.age_scan_duration = 0;
	      l2fib_main.evt_scan_duration = 0;
	    }
	  /* schedule next scan */
	  if (enabled)
	    next_age_scan_time = start_time + L2FIB_AGE_SCAN_INTERVAL;
	  else
	    next_age_scan_time = CLIB_TIME_MAX;
	}
    }
  return 0;
}

VLIB_REGISTER_NODE (l2fib_mac_age_scanner_process_node) = {
    .function = l2fib_mac_age_scanner_process,
    .type = VLIB_NODE_TYPE_PROCESS,
    .name = "l2fib-mac-age-scanner-process",
};

clib_error_t *
l2fib_init (vlib_main_t * vm)
{
  l2fib_main_t *mp = &l2fib_main;
  l2fib_entry_key_t test_key;
  u8 test_mac[6];

  mp->vlib_main = vm;
  mp->vnet_main = vnet_get_main ();
  if (mp->mac_table_n_buckets == 0)
    mp->mac_table_n_buckets = L2FIB_NUM_BUCKETS;
  if (mp->mac_table_memory_size == 0)
    mp->mac_table_memory_size = L2FIB_MEMORY_SIZE;
  mp->mac_table_initialized = 0;

  /* verify the key constructor is good, since it is endian-sensitive */
  clib_memset (test_mac, 0, sizeof (test_mac));
  test_mac[0] = 0x11;
  test_key.raw = 0;
  test_key.raw = l2fib_make_key ((u8 *) & test_mac, 0x1234);
  ASSERT (test_key.fields.mac[0] == 0x11);
  ASSERT (test_key.fields.bd_index == 0x1234);

  return 0;
}

VLIB_INIT_FUNCTION (l2fib_init);

static clib_error_t *
lfib_config (vlib_main_t * vm, unformat_input_t * input)
{
  l2fib_main_t *lm = &l2fib_main;
  uword table_size = ~0;
  u32 n_buckets = ~0;

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "table-size %U", unformat_memory_size,
		    &table_size))
	;
      else if (unformat (input, "num-buckets %u", &n_buckets))
	;
      else
	return clib_error_return (0, "unknown input `%U'",
				  format_unformat_error, input);
    }

  if (n_buckets != ~0)
    {
      if (!is_pow2 (n_buckets))
	return clib_error_return (0, "num-buckets must be power of 2");
      lm->mac_table_n_buckets = n_buckets;
    }

  if (table_size != ~0)
    lm->mac_table_memory_size = table_size;
  return 0;
}

VLIB_CONFIG_FUNCTION (lfib_config, "l2fib");

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
