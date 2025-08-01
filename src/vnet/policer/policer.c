/*
 * Copyright (c) 2015 Cisco and/or its affiliates.
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
#include <stdint.h>
#include <stdbool.h>
#include <vnet/policer/policer.h>
#include <vnet/policer/police_inlines.h>
#include <vnet/classify/vnet_classify.h>
#include <vnet/ip/ip_packet.h>

vnet_policer_main_t vnet_policer_main;

u8 *
format_policer_handoff_trace (u8 *s, va_list *args)
{
  CLIB_UNUSED (vlib_main_t * vm) = va_arg (*args, vlib_main_t *);
  CLIB_UNUSED (vlib_node_t * node) = va_arg (*args, vlib_node_t *);
  policer_handoff_trace_t *t = va_arg (*args, policer_handoff_trace_t *);

  s = format (s, "policer %d, handoff thread %d to %d", t->policer_index,
	      t->current_worker_index, t->next_worker_index);

  return s;
}

vlib_combined_counter_main_t policer_counters[] = {
  {
    .name = "Policer-Conform",
    .stat_segment_name = "/net/policer/conform",
  },
  {
    .name = "Policer-Exceed",
    .stat_segment_name = "/net/policer/exceed",
  },
  {
    .name = "Policer-Violate",
    .stat_segment_name = "/net/policer/violate",
  },
};

int
policer_add (vlib_main_t *vm, const u8 *name, const qos_pol_cfg_params_st *cfg,
	     u32 *policer_index)
{
  vnet_policer_main_t *pm = &vnet_policer_main;
  policer_t test_policer;
  policer_t *policer;
  policer_t *pp;
  qos_pol_cfg_params_st *cp;
  uword *p;
  u32 pi;
  int rv;
  int i;

  p = hash_get_mem (pm->policer_config_by_name, name);

  if (p != NULL)
    return VNET_API_ERROR_VALUE_EXIST;

  /* Vet the configuration before adding it to the table */
  rv = pol_logical_2_physical (cfg, &test_policer);

  if (rv != 0)
    return VNET_API_ERROR_INVALID_VALUE;

  pool_get (pm->configs, cp);
  pool_get_aligned (pm->policers, policer, CLIB_CACHE_LINE_BYTES);

  clib_memcpy (cp, cfg, sizeof (*cp));
  clib_memcpy (policer, &test_policer, sizeof (*pp));

  policer->name = format (0, "%s%c", name, 0);
  pi = policer - pm->policers;

  hash_set_mem (pm->policer_config_by_name, policer->name, cp - pm->configs);
  hash_set_mem (pm->policer_index_by_name, policer->name, pi);
  *policer_index = pi;
  policer->thread_index = ~0;

  for (i = 0; i < NUM_POLICE_RESULTS; i++)
    {
      vlib_validate_combined_counter (&policer_counters[i], pi);
      vlib_zero_combined_counter (&policer_counters[i], pi);
    }

  return 0;
}

int
policer_del (vlib_main_t *vm, u32 policer_index)
{
  vnet_policer_main_t *pm = &vnet_policer_main;
  policer_t *policer;
  uword *p;

  if (pool_is_free_index (pm->policers, policer_index))
    return VNET_API_ERROR_NO_SUCH_ENTRY;

  policer = &pm->policers[policer_index];

  p = hash_get_mem (pm->policer_config_by_name, policer->name);

  /* free policer config */
  if (p != NULL)
    {
      pool_put_index (pm->configs, p[0]);
      hash_unset_mem (pm->policer_config_by_name, policer->name);
    }

  /* free policer */
  hash_unset_mem (pm->policer_index_by_name, policer->name);
  vec_free (policer->name);
  pool_put_index (pm->policers, policer_index);

  return 0;
}

int
policer_update (vlib_main_t *vm, u32 policer_index,
		const qos_pol_cfg_params_st *cfg)
{
  vnet_policer_main_t *pm = &vnet_policer_main;
  policer_t test_policer;
  policer_t *policer;
  qos_pol_cfg_params_st *cp;
  uword *p;
  u8 *name;
  int rv;
  int i;

  if (pool_is_free_index (pm->policers, policer_index))
    return VNET_API_ERROR_NO_SUCH_ENTRY;

  policer = &pm->policers[policer_index];

  /* Vet the configuration before adding it to the table */
  rv = pol_logical_2_physical (cfg, &test_policer);
  if (rv != 0)
    return VNET_API_ERROR_INVALID_VALUE;

  p = hash_get_mem (pm->policer_config_by_name, policer->name);

  if (PREDICT_TRUE (p != NULL))
    {
      cp = &pm->configs[p[0]];
    }
  else
    {
      /* recover from a missing configuration */
      pool_get (pm->configs, cp);
      hash_set_mem (pm->policer_config_by_name, policer->name,
		    cp - pm->configs);
    }

  name = policer->name;

  clib_memcpy (cp, cfg, sizeof (*cp));
  clib_memcpy (policer, &test_policer, sizeof (*policer));

  policer->name = name;
  policer->thread_index = ~0;

  for (i = 0; i < NUM_POLICE_RESULTS; i++)
    vlib_zero_combined_counter (&policer_counters[i], policer_index);

  return 0;
}

int
policer_reset (vlib_main_t *vm, u32 policer_index)
{
  vnet_policer_main_t *pm = &vnet_policer_main;
  policer_t *policer;

  if (pool_is_free_index (pm->policers, policer_index))
    return VNET_API_ERROR_NO_SUCH_ENTRY;

  policer = &pm->policers[policer_index];

  policer->current_bucket = policer->current_limit;
  policer->extended_bucket = policer->extended_limit;

  return 0;
}

int
policer_bind_worker (u32 policer_index, u32 worker, bool bind)
{
  vnet_policer_main_t *pm = &vnet_policer_main;
  policer_t *policer;

  if (pool_is_free_index (pm->policers, policer_index))
    return VNET_API_ERROR_NO_SUCH_ENTRY;

  policer = &pm->policers[policer_index];

  if (bind)
    {
      if (worker >= vlib_num_workers ())
	{
	  return VNET_API_ERROR_INVALID_WORKER;
	}

      policer->thread_index = vlib_get_worker_thread_index (worker);
    }
  else
    {
      policer->thread_index = ~0;
    }
  return 0;
}

int
policer_input (u32 policer_index, u32 sw_if_index, vlib_dir_t dir, bool apply)
{
  vnet_policer_main_t *pm = &vnet_policer_main;

  if (pool_is_free_index (pm->policers, policer_index))
    return VNET_API_ERROR_NO_SUCH_ENTRY;

  if (apply)
    {
      vec_validate (pm->policer_index_by_sw_if_index[dir], sw_if_index);
      pm->policer_index_by_sw_if_index[dir][sw_if_index] = policer_index;
    }
  else
    {
      pm->policer_index_by_sw_if_index[dir][sw_if_index] = ~0;
    }

  if (dir == VLIB_RX)
    {
      vnet_feature_enable_disable ("device-input", "policer-input",
				   sw_if_index, apply, 0, 0);
    }
  else
    {
      vnet_feature_enable_disable ("ip4-output", "policer-output", sw_if_index,
				   apply, 0, 0);
      vnet_feature_enable_disable ("ip6-output", "policer-output", sw_if_index,
				   apply, 0, 0);
    }
  return 0;
}

u8 *
format_policer_instance (u8 * s, va_list * va)
{
  vnet_policer_main_t *pm = &vnet_policer_main;
  policer_t *i = va_arg (*va, policer_t *);
  u32 policer_index = i - pm->policers;
  int result;
  vlib_counter_t counts[NUM_POLICE_RESULTS];

  for (result = 0; result < NUM_POLICE_RESULTS; result++)
    {
      vlib_get_combined_counter (&policer_counters[result], policer_index,
				 &counts[result]);
    }

  s =
    format (s, "Policer at index %d: %s rate, %s color-aware\n", policer_index,
	    i->single_rate ? "single" : "dual", i->color_aware ? "is" : "not");
  s = format (s, "cir %u tok/period, pir %u tok/period, scale %u\n",
	      i->cir_tokens_per_period, i->pir_tokens_per_period, i->scale);
  s = format (s, "cur lim %u, cur bkt %u, ext lim %u, ext bkt %u\n",
	      i->current_limit,
	      i->current_bucket, i->extended_limit, i->extended_bucket);
  s = format (s, "last update %llu\n", i->last_update_time);
  s = format (s, "conform %llu packets, %llu bytes\n",
	      counts[POLICE_CONFORM].packets, counts[POLICE_CONFORM].bytes);
  s = format (s, "exceed %llu packets, %llu bytes\n",
	      counts[POLICE_EXCEED].packets, counts[POLICE_EXCEED].bytes);
  s = format (s, "violate %llu packets, %llu bytes\n",
	      counts[POLICE_VIOLATE].packets, counts[POLICE_VIOLATE].bytes);
  return s;
}

static u8 *
format_policer_round_type (u8 * s, va_list * va)
{
  qos_pol_cfg_params_st *c = va_arg (*va, qos_pol_cfg_params_st *);

  if (c->rnd_type == QOS_ROUND_TO_CLOSEST)
    s = format (s, "closest");
  else if (c->rnd_type == QOS_ROUND_TO_UP)
    s = format (s, "up");
  else if (c->rnd_type == QOS_ROUND_TO_DOWN)
    s = format (s, "down");
  else
    s = format (s, "ILLEGAL");
  return s;
}


static u8 *
format_policer_rate_type (u8 * s, va_list * va)
{
  qos_pol_cfg_params_st *c = va_arg (*va, qos_pol_cfg_params_st *);

  if (c->rate_type == QOS_RATE_KBPS)
    s = format (s, "kbps");
  else if (c->rate_type == QOS_RATE_PPS)
    s = format (s, "pps");
  else
    s = format (s, "ILLEGAL");
  return s;
}

static u8 *
format_policer_type (u8 * s, va_list * va)
{
  qos_pol_cfg_params_st *c = va_arg (*va, qos_pol_cfg_params_st *);

  if (c->rfc == QOS_POLICER_TYPE_1R2C)
    s = format (s, "1r2c");

  else if (c->rfc == QOS_POLICER_TYPE_1R3C_RFC_2697)
    s = format (s, "1r3c");

  else if (c->rfc == QOS_POLICER_TYPE_2R3C_RFC_2698)
    s = format (s, "2r3c-2698");

  else if (c->rfc == QOS_POLICER_TYPE_2R3C_RFC_4115)
    s = format (s, "2r3c-4115");

  else if (c->rfc == QOS_POLICER_TYPE_2R3C_RFC_MEF5CF1)
    s = format (s, "2r3c-mef5cf1");
  else
    s = format (s, "ILLEGAL");
  return s;
}

static u8 *
format_policer_action_type (u8 * s, va_list * va)
{
  qos_pol_action_params_st *a = va_arg (*va, qos_pol_action_params_st *);

  if (a->action_type == QOS_ACTION_DROP)
    s = format (s, "drop");
  else if (a->action_type == QOS_ACTION_TRANSMIT)
    s = format (s, "transmit");
  else if (a->action_type == QOS_ACTION_MARK_AND_TRANSMIT)
    s = format (s, "mark-and-transmit %U", format_ip_dscp, a->dscp);
  else
    s = format (s, "ILLEGAL");
  return s;
}

u8 *
format_policer_config (u8 * s, va_list * va)
{
  qos_pol_cfg_params_st *c = va_arg (*va, qos_pol_cfg_params_st *);

  s = format (s, "type %U cir %u eir %u cb %u eb %u\n",
	      format_policer_type, c,
	      c->rb.kbps.cir_kbps,
	      c->rb.kbps.eir_kbps, c->rb.kbps.cb_bytes, c->rb.kbps.eb_bytes);
  s = format (s, "rate type %U, round type %U\n",
	      format_policer_rate_type, c, format_policer_round_type, c);
  s = format (s, "conform action %U, exceed action %U, violate action %U\n",
	      format_policer_action_type, &c->conform_action,
	      format_policer_action_type, &c->exceed_action,
	      format_policer_action_type, &c->violate_action);
  return s;
}

static uword
unformat_policer_type (unformat_input_t * input, va_list * va)
{
  qos_pol_cfg_params_st *c = va_arg (*va, qos_pol_cfg_params_st *);

  if (!unformat (input, "type"))
    return 0;

  if (unformat (input, "1r2c"))
    c->rfc = QOS_POLICER_TYPE_1R2C;
  else if (unformat (input, "1r3c"))
    c->rfc = QOS_POLICER_TYPE_1R3C_RFC_2697;
  else if (unformat (input, "2r3c-2698"))
    c->rfc = QOS_POLICER_TYPE_2R3C_RFC_2698;
  else if (unformat (input, "2r3c-4115"))
    c->rfc = QOS_POLICER_TYPE_2R3C_RFC_4115;
  else if (unformat (input, "2r3c-mef5cf1"))
    c->rfc = QOS_POLICER_TYPE_2R3C_RFC_MEF5CF1;
  else
    return 0;
  return 1;
}

static uword
unformat_policer_round_type (unformat_input_t * input, va_list * va)
{
  qos_pol_cfg_params_st *c = va_arg (*va, qos_pol_cfg_params_st *);

  if (!unformat (input, "round"))
    return 0;

  if (unformat (input, "closest"))
    c->rnd_type = QOS_ROUND_TO_CLOSEST;
  else if (unformat (input, "up"))
    c->rnd_type = QOS_ROUND_TO_UP;
  else if (unformat (input, "down"))
    c->rnd_type = QOS_ROUND_TO_DOWN;
  else
    return 0;
  return 1;
}

static uword
unformat_policer_rate_type (unformat_input_t * input, va_list * va)
{
  qos_pol_cfg_params_st *c = va_arg (*va, qos_pol_cfg_params_st *);

  if (!unformat (input, "rate"))
    return 0;

  if (unformat (input, "kbps"))
    c->rate_type = QOS_RATE_KBPS;
  else if (unformat (input, "pps"))
    c->rate_type = QOS_RATE_PPS;
  else
    return 0;
  return 1;
}

static uword
unformat_policer_cir (unformat_input_t * input, va_list * va)
{
  qos_pol_cfg_params_st *c = va_arg (*va, qos_pol_cfg_params_st *);

  if (unformat (input, "cir %u", &c->rb.kbps.cir_kbps))
    return 1;
  return 0;
}

static uword
unformat_policer_eir (unformat_input_t * input, va_list * va)
{
  qos_pol_cfg_params_st *c = va_arg (*va, qos_pol_cfg_params_st *);

  if (unformat (input, "eir %u", &c->rb.kbps.eir_kbps))
    return 1;
  return 0;
}

static uword
unformat_policer_cb (unformat_input_t * input, va_list * va)
{
  qos_pol_cfg_params_st *c = va_arg (*va, qos_pol_cfg_params_st *);

  if (unformat (input, "cb %u", &c->rb.kbps.cb_bytes))
    return 1;
  return 0;
}

static uword
unformat_policer_eb (unformat_input_t * input, va_list * va)
{
  qos_pol_cfg_params_st *c = va_arg (*va, qos_pol_cfg_params_st *);

  if (unformat (input, "eb %u", &c->rb.kbps.eb_bytes))
    return 1;
  return 0;
}

static uword
unformat_policer_action_type (unformat_input_t * input, va_list * va)
{
  qos_pol_action_params_st *a = va_arg (*va, qos_pol_action_params_st *);

  if (unformat (input, "drop"))
    a->action_type = QOS_ACTION_DROP;
  else if (unformat (input, "transmit"))
    a->action_type = QOS_ACTION_TRANSMIT;
  else if (unformat (input, "mark-and-transmit %U", unformat_ip_dscp,
		     &a->dscp))
    a->action_type = QOS_ACTION_MARK_AND_TRANSMIT;
  else
    return 0;
  return 1;
}

static uword
unformat_policer_action (unformat_input_t * input, va_list * va)
{
  qos_pol_cfg_params_st *c = va_arg (*va, qos_pol_cfg_params_st *);

  if (unformat (input, "conform-action %U", unformat_policer_action_type,
		&c->conform_action))
    return 1;
  else if (unformat (input, "exceed-action %U", unformat_policer_action_type,
		     &c->exceed_action))
    return 1;
  else if (unformat (input, "violate-action %U", unformat_policer_action_type,
		     &c->violate_action))
    return 1;
  return 0;
}

static uword
unformat_policer_classify_next_index (unformat_input_t * input, va_list * va)
{
  u32 *r = va_arg (*va, u32 *);
  vnet_policer_main_t *pm = &vnet_policer_main;
  uword *p;
  u8 *match_name = 0;

  if (unformat (input, "%s", &match_name))
    ;
  else
    return 0;

  p = hash_get_mem (pm->policer_index_by_name, match_name);
  vec_free (match_name);

  if (p == 0)
    return 0;

  *r = p[0];

  return 1;
}

static uword
unformat_policer_classify_precolor (unformat_input_t * input, va_list * va)
{
  u32 *r = va_arg (*va, u32 *);

  if (unformat (input, "conform-color"))
    *r = POLICE_CONFORM;
  else if (unformat (input, "exceed-color"))
    *r = POLICE_EXCEED;
  else
    return 0;

  return 1;
}

#define foreach_config_param                    \
_(eb)                                           \
_(cb)                                           \
_(eir)                                          \
_(cir)                                          \
_(rate_type)                                    \
_(round_type)                                   \
_(type)                                         \
_(action)

static clib_error_t *
policer_add_command_fn (vlib_main_t *vm, unformat_input_t *input,
			vlib_cli_command_t *cmd)
{
  vnet_policer_main_t *pm = &vnet_policer_main;
  qos_pol_cfg_params_st c;
  unformat_input_t _line_input, *line_input = &_line_input;
  u8 *name = 0;
  uword *p;
  u32 pi;
  u32 policer_index = ~0;
  int rv = 0;
  clib_error_t *error = NULL;
  u8 is_update = cmd->function_arg;

  /* Get a line of input. */
  if (!unformat_user (input, unformat_line_input, line_input))
    return 0;

  clib_memset (&c, 0, sizeof (c));

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "name %s", &name))
	;
      else if (is_update && unformat (line_input, "index %u", &policer_index))
	;
      else if (unformat (line_input, "color-aware"))
	c.color_aware = 1;

#define _(a) else if (unformat (line_input, "%U", unformat_policer_##a, &c)) ;
      foreach_config_param
#undef _
	else
	{
	  error = clib_error_return (0, "unknown input `%U'",
				     format_unformat_error, line_input);
	  goto done;
	}
    }

  if (is_update)
    {
      if (~0 == policer_index && 0 != name)
	{
	  p = hash_get_mem (pm->policer_index_by_name, name);
	  if (p != NULL)
	    policer_index = p[0];
	}

      if (~0 == policer_index)
	{
	  error = clib_error_return (0, "Update policer failure");
	  goto done;
	}

      rv = policer_update (vm, policer_index, &c);
    }
  else
    {
      rv = policer_add (vm, name, &c, &pi);
    }

  switch (rv)
    {
    case VNET_API_ERROR_NO_SUCH_ENTRY:
      error = clib_error_return (0, "No such policer");
      break;
    case VNET_API_ERROR_VALUE_EXIST:
      error = clib_error_return (0, "Policer already exists");
      break;
    case VNET_API_ERROR_INVALID_VALUE:
      error = clib_error_return (0, "Config failed sanity check");
      break;
    }

done:
  unformat_free (line_input);
  vec_free (name);

  return error;
}

static clib_error_t *
policer_del_command_fn (vlib_main_t *vm, unformat_input_t *input,
			vlib_cli_command_t *cmd)
{
  unformat_input_t _line_input, *line_input = &_line_input;
  clib_error_t *error = NULL;
  vnet_policer_main_t *pm = &vnet_policer_main;
  int rv;
  u32 policer_index = ~0;
  uword *p;
  u8 *name = 0;

  /* Get a line of input. */
  if (!unformat_user (input, unformat_line_input, line_input))
    return 0;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "name %s", &name))
	;
      else if (unformat (line_input, "index %u", &policer_index))
	;
      else
	{
	  error = clib_error_return (0, "unknown input `%U'",
				     format_unformat_error, line_input);
	  goto done;
	}
    }

  if (~0 == policer_index && 0 != name)
    {
      p = hash_get_mem (pm->policer_index_by_name, name);
      if (p != NULL)
	policer_index = p[0];
    }

  rv = VNET_API_ERROR_NO_SUCH_ENTRY;
  if (~0 != policer_index)
    rv = policer_del (vm, policer_index);

  switch (rv)
    {
    case VNET_API_ERROR_INVALID_VALUE:
      error = clib_error_return (0, "No such policer configuration");
      break;
    case VNET_API_ERROR_NO_SUCH_ENTRY:
      error = clib_error_return (0, "No such policer");
      break;
    }

done:
  unformat_free (line_input);
  vec_free (name);

  return error;
}

static clib_error_t *
policer_bind_command_fn (vlib_main_t *vm, unformat_input_t *input,
			 vlib_cli_command_t *cmd)
{
  unformat_input_t _line_input, *line_input = &_line_input;
  clib_error_t *error = NULL;
  vnet_policer_main_t *pm = &vnet_policer_main;
  u8 bind = 1;
  u8 *name = 0;
  u32 worker = ~0;
  u32 policer_index = ~0;
  uword *p;
  int rv;

  /* Get a line of input. */
  if (!unformat_user (input, unformat_line_input, line_input))
    return 0;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "name %s", &name))
	;
      else if (unformat (line_input, "index %u", &policer_index))
	;
      else if (unformat (line_input, "unbind"))
	bind = 0;
      else if (unformat (line_input, "%d", &worker))
	;
      else
	{
	  error = clib_error_return (0, "unknown input `%U'",
				     format_unformat_error, line_input);
	  goto done;
	}
    }

  if (bind && ~0 == worker)
    {
      error = clib_error_return (0, "specify worker to bind to: `%U'",
				 format_unformat_error, line_input);
    }
  else
    {
      if (~0 == policer_index && 0 != name)
	{
	  p = hash_get_mem (pm->policer_index_by_name, name);
	  if (p != NULL)
	    policer_index = p[0];
	}

      rv = VNET_API_ERROR_NO_SUCH_ENTRY;
      if (~0 != policer_index)
	rv = policer_bind_worker (policer_index, worker, bind);

      if (rv)
	error = clib_error_return (0, "failed: `%d'", rv);
    }

done:
  unformat_free (line_input);
  vec_free (name);

  return error;
}

static clib_error_t *
policer_input_command_fn (vlib_main_t *vm, unformat_input_t *input,
			  vlib_cli_command_t *cmd)
{
  unformat_input_t _line_input, *line_input = &_line_input;
  clib_error_t *error = NULL;
  vnet_policer_main_t *pm = &vnet_policer_main;
  u8 apply = 1;
  u8 *name = 0;
  u32 sw_if_index = ~0;
  u32 policer_index = ~0;
  uword *p;
  int rv;
  vlib_dir_t dir = cmd->function_arg;

  /* Get a line of input. */
  if (!unformat_user (input, unformat_line_input, line_input))
    return 0;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "name %s", &name))
	;
      else if (unformat (line_input, "index %u", &policer_index))
	;
      else if (unformat (line_input, "unapply"))
	apply = 0;
      else if (unformat (line_input, "%U", unformat_vnet_sw_interface,
			 vnet_get_main (), &sw_if_index))
	;
      else
	{
	  error = clib_error_return (0, "unknown input `%U'",
				     format_unformat_error, line_input);
	  goto done;
	}
    }

  if (~0 == sw_if_index)
    {
      error = clib_error_return (0, "specify interface to apply to: `%U'",
				 format_unformat_error, line_input);
    }
  else
    {
      if (~0 == policer_index && 0 != name)
	{
	  p = hash_get_mem (pm->policer_index_by_name, name);
	  if (p != NULL)
	    policer_index = p[0];
	}

      rv = VNET_API_ERROR_NO_SUCH_ENTRY;
      if (~0 != policer_index)
	rv = policer_input (policer_index, sw_if_index, dir, apply);

      if (rv)
	error = clib_error_return (0, "failed: `%d'", rv);
    }

done:
  unformat_free (line_input);
  vec_free (name);

  return error;
}

static clib_error_t *
policer_reset_command_fn (vlib_main_t *vm, unformat_input_t *input,
			  vlib_cli_command_t *cmd)
{
  unformat_input_t _line_input, *line_input = &_line_input;
  clib_error_t *error = NULL;
  vnet_policer_main_t *pm = &vnet_policer_main;
  int rv;
  u32 policer_index = ~0;
  uword *p;
  u8 *name = 0;

  /* Get a line of input. */
  if (!unformat_user (input, unformat_line_input, line_input))
    return 0;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "name %s", &name))
	;
      else if (unformat (line_input, "index %u", &policer_index))
	;
      else
	{
	  error = clib_error_return (0, "unknown input `%U'",
				     format_unformat_error, line_input);
	  goto done;
	}
    }

  if (~0 == policer_index && 0 != name)
    {
      p = hash_get_mem (pm->policer_index_by_name, name);
      if (p != NULL)
	policer_index = p[0];
    }

  rv = VNET_API_ERROR_NO_SUCH_ENTRY;
  if (~0 != policer_index)
    rv = policer_reset (vm, policer_index);

  switch (rv)
    {
    case VNET_API_ERROR_NO_SUCH_ENTRY:
      error = clib_error_return (0, "No such policer");
      break;
    }

done:
  unformat_free (line_input);
  vec_free (name);

  return error;
}

VLIB_CLI_COMMAND (configure_policer_command, static) = {
  .path = "configure policer",
  .short_help = "configure policer [name <name> | index <index>] [type 1r2c | "
		"1r3c | 2r3c-2698 "
		"| 2r3c-4115] [color-aware] [cir <cir>] [cb <cb>] [eir <eir>] "
		"[eb <eb>] [rate kbps | pps] [round closest | up | down] "
		"[conform-action drop | transmit | mark-and-transmit <dscp>] "
		"[exceed-action drop | transmit | mark-and-transmit <dscp>] "
		"[violate-action drop | transmit | mark-and-transmit <dscp>]",
  .function = policer_add_command_fn,
  .function_arg = 1
};

VLIB_CLI_COMMAND (policer_add_command, static) = {
  .path = "policer add",
  .short_help = "policer add name <name> [type 1r2c | 1r3c | 2r3c-2698 | "
		"2r3c-4115] [color-aware] [cir <cir>] [cb <cb>] [eir <eir>] "
		"[eb <eb>] [rate kbps | pps] [round closest | up | down] "
		"[conform-action drop | transmit | mark-and-transmit <dscp>] "
		"[exceed-action drop | transmit | mark-and-transmit <dscp>] "
		"[violate-action drop | transmit | mark-and-transmit <dscp>]",
  .function = policer_add_command_fn,
  .function_arg = 0
};

VLIB_CLI_COMMAND (policer_del_command, static) = {
  .path = "policer del",
  .short_help = "policer del [name <name> | index <index>]",
  .function = policer_del_command_fn,
};

VLIB_CLI_COMMAND (policer_bind_command, static) = {
  .path = "policer bind",
  .short_help = "policer bind [unbind] [name <name> | index <index>] <worker>",
  .function = policer_bind_command_fn,
};

VLIB_CLI_COMMAND (policer_input_command, static) = {
  .path = "policer input",
  .short_help =
    "policer input [unapply] [name <name> | index <index>] <interface>",
  .function = policer_input_command_fn,
  .function_arg = VLIB_RX,
};

VLIB_CLI_COMMAND (policer_output_command, static) = {
  .path = "policer output",
  .short_help =
    "policer output [unapply] [name <name> | index <index>] <interface>",
  .function = policer_input_command_fn,
  .function_arg = VLIB_TX,
};

VLIB_CLI_COMMAND (policer_reset_command, static) = {
  .path = "policer reset",
  .short_help = "policer reset [name <name> | index <index>]",
  .function = policer_reset_command_fn
};

static clib_error_t *
show_policer_command_fn (vlib_main_t * vm,
			 unformat_input_t * input, vlib_cli_command_t * cmd)
{
  vnet_policer_main_t *pm = &vnet_policer_main;
  unformat_input_t _line_input, *line_input = &_line_input;
  policer_t *policer;
  u32 policer_index = ~0;
  u8 *name = 0;
  uword *ci, *pi;
  qos_pol_cfg_params_st *config;
  clib_error_t *error = 0;

  /* Get a line of input. */
  if (!unformat_user (input, unformat_line_input, line_input))
    {
      pool_foreach (policer, pm->policers)
	{
	  ci = hash_get_mem (pm->policer_config_by_name, policer->name);
	  config = pool_elt_at_index (pm->configs, ci[0]);

	  vlib_cli_output (vm, "Name \"%s\" %U ", policer->name,
			   format_policer_config, config);
	  vlib_cli_output (vm, "%U", format_policer_instance, policer);
	  vlib_cli_output (vm, "-----------");
	}
      return 0;
    }

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "name %s", &name))
	;
      else if (unformat (line_input, "index %u", &policer_index))
	;
      else
	{
	  error = clib_error_return (0, "unknown input `%U'",
				     format_unformat_error, line_input);
	  goto done;
	}
    }

  if (~0 == policer_index && 0 != name)
    {
      pi = hash_get_mem (pm->policer_index_by_name, name);
      if (pi != NULL)
	policer_index = pi[0];
    }

  if (~0 == policer_index || pool_is_free_index (pm->policers, policer_index))
    goto done;

  policer = &pm->policers[policer_index];
  ci = hash_get_mem (pm->policer_config_by_name, policer->name);
  config = pool_elt_at_index (pm->configs, ci[0]);
  vlib_cli_output (vm, "Name \"%s\" %U ", policer->name, format_policer_config,
		   config);
  vlib_cli_output (vm, "%U", format_policer_instance, policer);
  vlib_cli_output (vm, "-----------");

done:
  unformat_free (line_input);
  vec_free (name);

  return error;
}


VLIB_CLI_COMMAND (show_policer_command, static) = {
  .path = "show policer",
  .short_help = "show policer [name <name> | index <index>]",
  .function = show_policer_command_fn,
};

static clib_error_t *
show_policer_pools_command_fn (vlib_main_t * vm,
			       unformat_input_t * input,
			       vlib_cli_command_t * cmd)
{
  vnet_policer_main_t *pm = &vnet_policer_main;

  vlib_cli_output (vm, "pool sizes: configs=%d policers=%d",
		   pool_elts (pm->configs), pool_elts (pm->policers));
  return 0;
}
VLIB_CLI_COMMAND (show_policer_pools_command, static) = {
    .path = "show policer pools",
    .short_help = "show policer pools",
    .function = show_policer_pools_command_fn,
};

/*
 * Return the number of hardware TSC timer ticks per second for the dataplane.
 * This is approximately, but not exactly, the clock speed.
 */
static u64
get_tsc_hz (void)
{
  f64 cpu_freq;

  cpu_freq = os_cpu_clock_frequency ();
  return (u64) cpu_freq;
}

clib_error_t *
policer_init (vlib_main_t * vm)
{
  vnet_policer_main_t *pm = &vnet_policer_main;

  pm->vlib_main = vm;
  pm->vnet_main = vnet_get_main ();
  pm->log_class = vlib_log_register_class ("policer", 0);
  pm->fq_index[VLIB_RX] =
    vlib_frame_queue_main_init (policer_input_node.index, 0);
  pm->fq_index[VLIB_TX] =
    vlib_frame_queue_main_init (policer_output_node.index, 0);

  pm->policer_config_by_name = hash_create_string (0, sizeof (uword));
  pm->policer_index_by_name = hash_create_string (0, sizeof (uword));
  pm->tsc_hz = get_tsc_hz ();

  vnet_classify_register_unformat_policer_next_index_fn
    (unformat_policer_classify_next_index);
  vnet_classify_register_unformat_opaque_index_fn
    (unformat_policer_classify_precolor);

  return 0;
}

VLIB_INIT_FUNCTION (policer_init);



/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
