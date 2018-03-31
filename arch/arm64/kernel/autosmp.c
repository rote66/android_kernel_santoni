/*
 * arch/arm/kernel/autosmp.c
 *
 * automatically hotplug/unplug multiple cpu cores
 * based on cpu load and suspend state
 *
 * based on the msm_mpdecision code by
 * Copyright (c) 2012-2013, Dennis Rassmann <showp1984@gmail.com>
 *
 * Copyright (C) 2013-2014, Rauf Gungor, http://github.com/mrg666
 * rewrite to simplify and optimize, Jul. 2013, http://goo.gl/cdGw6x
 * optimize more, generalize for n cores, Sep. 2013, http://goo.gl/448qBz
 * generalize for all arch, rename as autosmp, Dec. 2013, http://goo.gl/x5oyhy
 *
 * Copyright (C) 2018, Ryan Andri (Rainforce279) <ryanandri@linuxmail.org>
 * 		 Adaptation for Octa core processor.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version. For more details, see the GNU
 * General Public License included with the Linux kernel or available
 * at www.gnu.org/licenses
 */

#include <linux/moduleparam.h>
#include <linux/cpufreq.h>
#include <linux/workqueue.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/hrtimer.h>

#define ASMP_TAG "AutoSMP: "
#define ASMP_STARTDELAY 20000

static struct delayed_work asmp_work;
static struct workqueue_struct *asmp_workq;

/*
 * Flag and NOT editable/tunabled
 */
static bool started = false;

static struct asmp_param_struct {
	unsigned int delay;
	unsigned int max_cpus;
	unsigned int min_cpus;
	unsigned int cpufreq_up;
	unsigned int cpufreq_down;
	unsigned int cycle_up;
	unsigned int cycle_down;
} asmp_param = {
	.delay = 100,
	.max_cpus = 4, /* Max cpu per cluster ! */
	.min_cpus = 2,
	.cpufreq_up = 90,
	.cpufreq_down = 60,
	.cycle_up = 1,
	.cycle_down = 1,
};

static unsigned int cycle = 0, delay0 = 0;
static unsigned long delay_jif = 0;
static int enabled __read_mostly = 0;

static void __ref asmp_work_fn(struct work_struct *work) {
	unsigned int cpu, rate;
	unsigned int slow_cpu_bc = 0, slow_cpu_lc = 4;
	unsigned int cpu_rate_bc, fast_rate_bc;
	unsigned int cpu_rate_lc, fast_rate_lc;
	unsigned int slow_rate_lc = UINT_MAX, slow_rate_bc = UINT_MAX;
	unsigned int max_rate_lc, max_rate_bc;
	unsigned int up_rate_lc, down_rate_lc;
	unsigned int up_rate_bc, down_rate_bc;
	int nr_cpu_online_lc = 0, nr_cpu_online_bc = 0;

	if (!cpu_online(0))
		cpu_up(0);
	if (!cpu_online(4))
		cpu_up(4);

	cycle++;

	if (asmp_param.delay != delay0) {
		delay0 = asmp_param.delay;
		delay_jif = msecs_to_jiffies(delay0);
	}

	/* Little Cluster */
	max_rate_lc  = cpufreq_quick_get_max(4);
	up_rate_lc   = ((asmp_param.cpufreq_up - 10) * max_rate_lc) / 100;
	down_rate_lc = ((asmp_param.cpufreq_down + 5) * max_rate_lc) / 100;

	/* Big Cluster */
	max_rate_bc  = cpufreq_quick_get_max(0);
	up_rate_bc   = ((asmp_param.cpufreq_up + 5) * max_rate_bc) / 100;
	down_rate_bc = ((asmp_param.cpufreq_down + 5) * max_rate_bc) / 100;

	/* find current max and min cpu freq to estimate load */
	get_online_cpus();
	cpu_rate_bc = cpufreq_quick_get(0);
	fast_rate_bc = cpu_rate_bc;
	cpu_rate_lc = cpufreq_quick_get(4);
	fast_rate_lc = cpu_rate_lc;
	for_each_online_cpu(cpu) {
		if (cpu && cpu < 4) {
			nr_cpu_online_bc++;
			rate = cpufreq_quick_get(cpu);
			if (rate <= slow_rate_bc) {
				slow_cpu_bc = cpu;
				slow_rate_bc = rate;
			} else if (rate > fast_rate_bc)
				fast_rate_bc = rate;
		}

		if (cpu > 4) {
			nr_cpu_online_lc++;
			rate = cpufreq_quick_get(cpu);
			if (rate <= slow_rate_lc) {
				slow_cpu_lc = cpu;
				slow_rate_lc = rate;
			} else if (rate > fast_rate_lc)
				fast_rate_lc = rate;
		}
	}
	put_online_cpus();

	if (cpu_rate_lc < slow_rate_lc)
		slow_rate_lc = cpu_rate_lc;

	nr_cpu_online_lc += 1;

	/* hotplug one core if all online cores are over up_rate limit */
	if (slow_rate_lc > up_rate_lc) {
		if ((nr_cpu_online_lc < asmp_param.max_cpus) &&
		    (cycle >= asmp_param.cycle_up)) {
			cpu = cpumask_next_zero(4, cpu_online_mask);
			cpu_up(cpu);
			cycle = 0;
		}
	/* unplug slowest core if all online cores are under down_rate limit */
	} else if ((slow_cpu_lc > 4) && (fast_rate_lc < down_rate_lc)) {
		if ((nr_cpu_online_lc > asmp_param.min_cpus) &&
		    (cycle >= asmp_param.cycle_down)) {
 			cpu_down(slow_cpu_lc);
			cycle = 0;
		}
	} /* else do nothing */

	if (cpu_rate_bc < slow_rate_bc)
		slow_rate_bc = cpu_rate_bc;

	nr_cpu_online_bc += 1;

	/* hotplug one core if all online cores are over up_rate limit */
	if (slow_rate_bc > up_rate_bc) {
		if ((nr_cpu_online_bc < asmp_param.max_cpus) &&
		    (cycle >= asmp_param.cycle_up)) {
			cpu = cpumask_next_zero(0, cpu_online_mask);
			cpu_up(cpu);
			cycle = 0;
		}
	/* unplug slowest core if all online cores are under down_rate limit */
	} else if (slow_cpu_bc && (fast_rate_bc < down_rate_bc)) {
		if ((nr_cpu_online_bc > asmp_param.min_cpus) &&
		    (cycle >= asmp_param.cycle_down)) {
 			cpu_down(slow_cpu_bc);
			cycle = 0;
		}
	} /* else do nothing */

	queue_delayed_work(asmp_workq, &asmp_work, delay_jif);
}

#ifdef CONFIG_SCHED_CORE_CTL
extern void disable_core_control(bool disable);
#endif
static int asmp_start(void)
{
	unsigned int cpu;
	int ret = 0;

	if (started) {
		pr_info(ASMP_TAG"already enabled\n");
		return ret;
	}

	asmp_workq = alloc_workqueue("asmp", WQ_HIGHPRI, 0);
	if (!asmp_workq) {
		ret = -ENOMEM;
		goto err_out;
	}

	for_each_possible_cpu(cpu) {
		if (!cpu_online(cpu))
			cpu_up(cpu);
	}

	INIT_DELAYED_WORK(&asmp_work, asmp_work_fn);
	queue_delayed_work(asmp_workq, &asmp_work,
			msecs_to_jiffies(asmp_param.delay));

	started = true;

	pr_info(ASMP_TAG"enabled\n");

	return ret;

err_out:
#ifdef CONFIG_SCHED_CORE_CTL
	disable_core_control(false);
#endif
	return ret;
}

static void asmp_stop(void)
{
	unsigned int cpu;

	if (!started) {
		pr_info(ASMP_TAG"already disabled\n");
		return;
	}

	cancel_delayed_work_sync(&asmp_work);
	destroy_workqueue(asmp_workq);

	for_each_possible_cpu(cpu) {
		if (!cpu_online(cpu))
			cpu_up(cpu);
	}

	started = false;

	pr_info(ASMP_TAG"disabled\n");
}

static int __ref set_enabled(const char *val,
			     const struct kernel_param *kp)
{
	int ret;

	ret = param_set_bool(val, kp);
	if (enabled) {
#ifdef CONFIG_SCHED_CORE_CTL
		disable_core_control(true);
#endif
		asmp_start();
	} else {
		asmp_stop();
#ifdef CONFIG_SCHED_CORE_CTL
		disable_core_control(false);
#endif
	}
	return ret;
}

static struct kernel_param_ops module_ops = {
	.set = set_enabled,
	.get = param_get_bool,
};

module_param_cb(enabled, &module_ops, &enabled, 0644);
MODULE_PARM_DESC(enabled, "hotplug/unplug cpu cores based on cpu load");

/***************************** SYSFS START *****************************/
#define define_one_global_ro(_name)					\
static struct global_attr _name =					\
__ATTR(_name, 0444, show_##_name, NULL)

#define define_one_global_rw(_name)					\
static struct global_attr _name =					\
__ATTR(_name, 0644, show_##_name, store_##_name)

struct kobject *asmp_kobject;

#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%u\n", asmp_param.object);			\
}
show_one(delay, delay);
show_one(min_cpus, min_cpus);
show_one(max_cpus, max_cpus);
show_one(cpufreq_up,cpufreq_up);
show_one(cpufreq_down,cpufreq_down);
show_one(cycle_up, cycle_up);
show_one(cycle_down, cycle_down);

#define store_one(file_name, object)					\
static ssize_t store_##file_name					\
(struct kobject *a, struct attribute *b, const char *buf, size_t count)	\
{									\
	unsigned int input;						\
	int ret;							\
	ret = sscanf(buf, "%u", &input);				\
	if (ret != 1)							\
		return -EINVAL;						\
	asmp_param.object = input;					\
	return count;							\
}									\
define_one_global_rw(file_name);
store_one(delay, delay);
store_one(min_cpus, min_cpus);
store_one(max_cpus, max_cpus);
store_one(cpufreq_up, cpufreq_up);
store_one(cpufreq_down, cpufreq_down);
store_one(cycle_up, cycle_up);
store_one(cycle_down, cycle_down);

static struct attribute *asmp_attributes[] = {
	&delay.attr,
	&min_cpus.attr,
	&max_cpus.attr,
	&cpufreq_up.attr,
	&cpufreq_down.attr,
	&cycle_up.attr,
	&cycle_down.attr,
	NULL
};

static struct attribute_group asmp_attr_group = {
	.attrs = asmp_attributes,
	.name = "conf",
};

/****************************** SYSFS END ******************************/

static int __init asmp_init(void) {
	int rc = 0;

	asmp_kobject = kobject_create_and_add("autosmp", kernel_kobj);
	if (asmp_kobject) {
		rc = sysfs_create_group(asmp_kobject, &asmp_attr_group);
		if (rc)
			pr_warn(ASMP_TAG"ERROR, create sysfs group");
	} else
		pr_warn(ASMP_TAG"ERROR, create sysfs kobj");

	pr_info(ASMP_TAG"initialized\n");

	return 0;
}
late_initcall(asmp_init);
