/*
 * Copyright (c) 2013-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "cpu-boost: " fmt

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/sched.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/input.h>
#include <linux/time.h>
#include <linux/cpu_boost.h>

struct cpu_sync {
  int cpu;
  unsigned int input_boost_min;
};

#define MAX_FREQ_BIG 2150400
#define MAX_FREQ_LITTLE 1593600

static DEFINE_PER_CPU(struct cpu_sync, sync_info);
static struct workqueue_struct *cpu_boost_wq;

static struct work_struct input_boost_work;
static bool max_boost_active = false;

static unsigned int input_boost_freq_b = 652800;
module_param(input_boost_freq_b, uint, 0644);
static unsigned int input_boost_freq_l = 652800;
module_param(input_boost_freq_l, uint, 0644);
static unsigned int input_boost_enabled = 1;
module_param(input_boost_enabled, uint, 0644);
static unsigned int max_boost_enabled = 1;
module_param(max_boost_enabled, uint, 0644);
static unsigned int mdss_boost_enabled = 1;
module_param(mdss_boost_enabled, uint, 0644);
static unsigned int input_boost_ms = 500;
module_param(input_boost_ms, uint, 0644);
static unsigned int mdss_timeout = 5000;
module_param(mdss_timeout, uint, 0644);
unsigned int smart_boost_enabled = 1;
module_param(smart_boost_enabled, uint, 0644);
unsigned int sb_damp_factor = 10;
module_param(sb_damp_factor, uint, 0644);

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
static int dynamic_stune_boost_ta;
module_param(dynamic_stune_boost_ta, uint, 0644);
static int dynamic_stune_boost_fg;
module_param(dynamic_stune_boost_fg, uint, 0644);
static unsigned int dynamic_stune_boost_ms;
module_param(dynamic_stune_boost_ms, uint, 0644);
static unsigned int dsb_enabled;
module_param(dsb_enabled, uint, 0644);
static bool stune_boost_active_ta;
static bool stune_boost_active_fg;
static int boost_slot;
static struct delayed_work dynamic_stune_boost_rem;
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

static struct delayed_work input_boost_rem;
unsigned long last_input_time;
unsigned long last_ib_time = 0;

/*
 * The CPUFREQ_ADJUST notifier is used to override the current policy min to
 * make sure policy min >= boost_min. The cpufreq framework then does the job
 * of enforcing the new policy.
 *
 * The sync kthread needs to run on the CPU in question to avoid deadlocks in
 * the wake up code. Achieve this by binding the thread to the respective
 * CPU. But a CPU going offline unbinds threads from that CPU. So, set it up
 * again each time the CPU comes back up. We can use CPUFREQ_START to figure
 * out a CPU is coming online instead of registering for hotplug notifiers.
 */
static int boost_adjust_notify(struct notifier_block *nb, unsigned long val,
                               void *data) {
  struct cpufreq_policy *policy = data;
  unsigned int cpu = policy->cpu;
  struct cpu_sync *s = &per_cpu(sync_info, cpu);
  unsigned int ib_min = s->input_boost_min;

  switch (val) {
  case CPUFREQ_ADJUST:
    if (!ib_min)
      break;
    ib_min =
        min((s->input_boost_min == UINT_MAX ? policy->max : s->input_boost_min),
            policy->max);

    pr_debug("CPU%u policy min before boost: %u kHz\n", cpu, policy->min);
    pr_debug("CPU%u boost min: %u kHz\n", cpu, ib_min);

    cpufreq_verify_within_limits(policy, ib_min, UINT_MAX);

    pr_debug("CPU%u policy min after boost: %u kHz\n", cpu, policy->min);
    break;
  }

  return NOTIFY_OK;
}

static struct notifier_block boost_adjust_nb = {
    .notifier_call = boost_adjust_notify,
};

static void update_policy_online(void) {
  unsigned int i;

  /* Re-evaluate policy to trigger adjust notifier for online CPUs */
  get_online_cpus();
  for_each_online_cpu(i) {
    pr_debug("Updating policy for CPU%d\n", i);
    cpufreq_update_policy(i);
  }
  put_online_cpus();
}

static void do_input_boost_rem(struct work_struct *work) {
  unsigned int i;
  struct cpu_sync *i_sync_info;

  /* Reset the input_boost_min for all CPUs in the system */
  pr_debug("Resetting input boost min for all CPUs\n");
  for_each_possible_cpu(i) {
    i_sync_info = &per_cpu(sync_info, i);
    i_sync_info->input_boost_min = 0;
  }

  /* Update policies for all online CPUs */
  update_policy_online();

  if (max_boost_active) {
    max_boost_active = false;
  }
}

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
static void _do_dsb() {
  int ret, err;

  if (stune_boost_active_ta) {
    reset_stune_boost("top-app", boost_slot);
    stune_boost_active_ta = false;
  }
  if (stune_boost_active_fg) {
    reset_stune_boost("foreground", boost_slot);
    stune_boost_active_fg = false;
  }
  cancel_delayed_work_sync(&dynamic_stune_boost_rem);

  /* Set dynamic stune boost value */
  ret = do_stune_boost("top-app", dynamic_stune_boost_ta, &boost_slot);
  if (!ret)
    stune_boost_active_ta = true;
  ret = do_stune_boost("foreground", dynamic_stune_boost_fg, &boost_slot);
  if (!err)
    stune_boost_active_fg = true;

  queue_delayed_work(cpu_boost_wq, &dynamic_stune_boost_rem,
                     msecs_to_jiffies(dynamic_stune_boost_ms));
}

void do_dsb_kick() {
  if (dsb_enabled < 1) {
    return;
  }
  _do_dsb();
}

static void do_dynamic_stune_boost_rem(struct work_struct *work) {
  /* Reset dynamic stune boost value to the default value */
  if (stune_boost_active_ta) {
    reset_stune_boost("top-app", boost_slot);
    stune_boost_active_ta = false;
  }
  if (stune_boost_active_fg) {
    reset_stune_boost("foreground", boost_slot);
    stune_boost_active_fg = false;
  }
}
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

static void do_input_boost(struct work_struct *work) {
  unsigned int i, freq_l, freq_b, sb_freq_l, sb_freq_b;
  struct cpu_sync *i_sync_info;

  if (max_boost_active || input_boost_ms < 1) {
    return;
  }
  if (time_before(jiffies,
                  last_ib_time + msecs_to_jiffies(input_boost_ms * 0.8)))
    return;
  cancel_delayed_work_sync(&input_boost_rem);

  if (smart_boost_enabled < 1) {
    /* Set the input_boost_min for all CPUs in the system */
    pr_debug("Setting input boost min for all CPUs\n");
    for_each_possible_cpu(i) {
      i_sync_info = &per_cpu(sync_info, i);
      if (i < 2) {
        i_sync_info->input_boost_min = input_boost_freq_l;
      } else {
        i_sync_info->input_boost_min = input_boost_freq_b;
      }
    }
  } else {
    freq_l = (smart_load * MAX_FREQ_LITTLE) / 100;
    freq_b = (smart_load * MAX_FREQ_BIG) / 100;

    (freq_l > input_boost_freq_l) ? (sb_freq_l = freq_l)
                                  : (sb_freq_l = input_boost_freq_l);
    (freq_b > input_boost_freq_b) ? (sb_freq_b = freq_b)
                                  : (sb_freq_b = input_boost_freq_b);

    for_each_possible_cpu(i) {
      i_sync_info = &per_cpu(sync_info, i);
      if (i < 2) {
        i_sync_info->input_boost_min = sb_freq_l;
      } else {
        i_sync_info->input_boost_min = sb_freq_b;
      }
    }
  }

  /* Update policies for all online CPUs */
  update_policy_online();

  queue_delayed_work(cpu_boost_wq, &input_boost_rem,
                     msecs_to_jiffies(input_boost_ms));
  do_dsb_kick();
  last_ib_time = jiffies;
}

void mdss_boost_kick() {
  if (mdss_boost_enabled < 1 || work_pending(&input_boost_work) ||
      time_after(jiffies, last_input_time + msecs_to_jiffies(mdss_timeout))) {
    return;
  }

  queue_work(cpu_boost_wq, &input_boost_work);
}

static void do_input_boost_max(unsigned int duration_ms) {
  unsigned int i;
  struct cpu_sync *i_sync_info;
  cancel_delayed_work_sync(&input_boost_rem);

  for_each_possible_cpu(i) {
    i_sync_info = &per_cpu(sync_info, i);
    i_sync_info->input_boost_min = UINT_MAX;
  }

  update_policy_online();

  queue_delayed_work(cpu_boost_wq, &input_boost_rem,
                     msecs_to_jiffies(duration_ms));
  do_dsb_kick();
  max_boost_active = true;
}

void input_boost_max_kick(unsigned int duration_ms) {
  if (max_boost_enabled < 1) {
    return;
  }

  do_input_boost_max(duration_ms);
}

static void cpuboost_input_event(struct input_handle *handle, unsigned int type,
                                 unsigned int code, int value) {
  last_input_time = jiffies;

  if (input_boost_enabled < 1)
    return;

  if (work_pending(&input_boost_work))
    return;

  queue_work(cpu_boost_wq, &input_boost_work);
}

static int cpuboost_input_connect(struct input_handler *handler,
                                  struct input_dev *dev,
                                  const struct input_device_id *id) {
  struct input_handle *handle;
  int error;

  handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
  if (!handle)
    return -ENOMEM;

  handle->dev = dev;
  handle->handler = handler;
  handle->name = "cpufreq";

  error = input_register_handle(handle);
  if (error)
    goto err2;

  error = input_open_device(handle);
  if (error)
    goto err1;

  return 0;
err1:
  input_unregister_handle(handle);
err2:
  kfree(handle);
  return error;
}

static void cpuboost_input_disconnect(struct input_handle *handle) {
#ifdef CONFIG_DYNAMIC_STUNE_BOOST
  /* Reset dynamic stune boost value to the default value */
  reset_stune_boost("top-app", boost_slot);
  stune_boost_active_ta = false;
  reset_stune_boost("foreground", boost_slot);
  stune_boost_active_fg = false;
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */
  input_close_device(handle);
  input_unregister_handle(handle);
  kfree(handle);
}

static const struct input_device_id cpuboost_ids[] = {
    /* multi-touch touchscreen */
    {
        .flags = INPUT_DEVICE_ID_MATCH_EVBIT | INPUT_DEVICE_ID_MATCH_ABSBIT,
        .evbit = {BIT_MASK(EV_ABS)},
        .absbit = {[BIT_WORD(ABS_MT_POSITION_X)] = BIT_MASK(ABS_MT_POSITION_X) |
                                                   BIT_MASK(ABS_MT_POSITION_Y)},
    },
    /* touchpad */
    {
        .flags = INPUT_DEVICE_ID_MATCH_KEYBIT | INPUT_DEVICE_ID_MATCH_ABSBIT,
        .keybit = {[BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH)},
        .absbit = {[BIT_WORD(ABS_X)] = BIT_MASK(ABS_X) | BIT_MASK(ABS_Y)},
    },
    /* Keypad */
    {
        .flags = INPUT_DEVICE_ID_MATCH_EVBIT, .evbit = {BIT_MASK(EV_KEY)},
    },
    {},
};

static struct input_handler cpuboost_input_handler = {
    .event = cpuboost_input_event,
    .connect = cpuboost_input_connect,
    .disconnect = cpuboost_input_disconnect,
    .name = "cpu-boost",
    .id_table = cpuboost_ids,
};

static int cpu_boost_init(void) {
  int cpu, ret;
  struct cpu_sync *s;

  cpu_boost_wq = alloc_workqueue("cpuboost_wq", WQ_HIGHPRI, 0);
  if (!cpu_boost_wq)
    return -EFAULT;

  INIT_WORK(&input_boost_work, do_input_boost);
  INIT_DELAYED_WORK(&input_boost_rem, do_input_boost_rem);
#ifdef CONFIG_DYNAMIC_STUNE_BOOST
  INIT_DELAYED_WORK(&dynamic_stune_boost_rem, do_dynamic_stune_boost_rem);
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

  for_each_possible_cpu(cpu) {
    s = &per_cpu(sync_info, cpu);
    s->cpu = cpu;
  }
  cpufreq_register_notifier(&boost_adjust_nb, CPUFREQ_POLICY_NOTIFIER);
  ret = input_register_handler(&cpuboost_input_handler);

  return ret;
}
late_initcall(cpu_boost_init);
