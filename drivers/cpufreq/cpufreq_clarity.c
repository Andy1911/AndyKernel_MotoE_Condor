/*
 * drivers/cpufreq/cpufreq_clarity.c
 *
 * Copyright (C)  2001 Russell King
 *           (C)  2003 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>.
 *                     Jun Nakajima <jun.nakajima@intel.com>
 *           (C)  2009 Alexander Clouter <alex@digriz.org.uk>
 *           (C)  2016 Ryan Andri <ryan.andri@gmail.com>
 *
 *
 * Author : Ryan Andri a.k.a Rainforce279 @ xda-developer
 * A smart & dynamic cpufreq governor based on conservative
 *
 * inspire from :
 *               * smartass governor by Erasmux
 *               * algorithm frequency limiter by faux123
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/jiffies.h>
#include <linux/kernel_stat.h>
#include <linux/mutex.h>
#include <linux/hrtimer.h>
#include <linux/tick.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/powersuspend.h>


/* Tunables start */
#define DEF_SAMPLING_RATE			(40000)
#define DEF_FREQUENCY_UP_THRESHOLD		(70)
#define DEF_FREQUENCY_DOWN_THRESHOLD		(30)
#define DEF_FREQ_MIDDLE				(787200)
#define DEF_FREQ_MAX_SUSPEND			(787200)
#define DEF_FREQ_AWAKE				(998400)
#define DEF_FREQ_STEP_UP			(5)
#define DEF_FREQ_STEP_DOWN			(5)
#define DEF_IGNORE_NICE_LOADS			(0)
#define DEF_IO_IS_BUSY				(0)
/* Tunables end */


/*
 * Dont edit!
 * Leave this with default values
 */
#define MIN_SAMPLING_RATE_RATIO			(2)
#define LATENCY_MULTIPLIER			(1000)
#define MIN_LATENCY_MULTIPLIER			(100)
#define TRANSITION_LATENCY_LIMIT		(10 * 1000 * 1000)
#define MICRO_FREQUENCY_MIN_SAMPLE_RATE		(10000)

static unsigned int min_sampling_rate;
static bool suspended;
static void do_dbs_timer(struct work_struct *work);

struct cpu_dbs_info_s {
	cputime64_t prev_cpu_idle;
	cputime64_t prev_cpu_wall;
	cputime64_t prev_cpu_nice;
	struct cpufreq_policy *cur_policy;
	struct delayed_work work;
	unsigned int down_skip;
	unsigned int requested_freq;
	unsigned int cpu_max_freq;
	unsigned int cpu_maxcur_freq;
	int cpu;
	unsigned int enable:1;
	/*
	 * percpu mutex that serializes governor limit change with
	 * do_dbs_timer invocation. We do not want do_dbs_timer to run
	 * when user is changing the governor or limits.
	 */
	struct mutex timer_mutex;
};
static DEFINE_PER_CPU(struct cpu_dbs_info_s, clarity_cpu_dbs_info);

static unsigned int dbs_enable;	/* number of CPUs using this policy */

/*
 * dbs_mutex protects dbs_enable in governor start/stop.
 */
static DEFINE_MUTEX(dbs_mutex);

static struct workqueue_struct *dbs_wq;

static struct dbs_tuners {
	unsigned int sampling_rate;
	unsigned int up_threshold;
	unsigned int down_threshold;
	unsigned int ignore_nice;
	unsigned int freq_middle;
	unsigned int freq_step_up;
	unsigned int freq_step_down;
	unsigned int freq_max_suspend;
	unsigned int freq_awake;
	int io_is_busy;
} dbs_tuners_ins = {
	.sampling_rate = DEF_SAMPLING_RATE,
	.up_threshold = DEF_FREQUENCY_UP_THRESHOLD,
	.down_threshold = DEF_FREQUENCY_DOWN_THRESHOLD,
	.ignore_nice = DEF_IGNORE_NICE_LOADS,
	.freq_middle = DEF_FREQ_MIDDLE,
	.freq_step_up = DEF_FREQ_STEP_UP,
	.freq_step_down = DEF_FREQ_STEP_DOWN,
	.io_is_busy = DEF_IO_IS_BUSY,
	.freq_max_suspend = DEF_FREQ_MAX_SUSPEND,
	.freq_awake = DEF_FREQ_AWAKE,
};

static inline cputime64_t get_cpu_idle_time_jiffy(unsigned int cpu,
						  cputime64_t *wall)
{
	u64 idle_time;
	u64 cur_wall_time;
	u64 busy_time;

	cur_wall_time = jiffies64_to_cputime64(get_jiffies_64());

	busy_time  = kcpustat_cpu(cpu).cpustat[CPUTIME_USER];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_SYSTEM];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_IRQ];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_SOFTIRQ];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_STEAL];
	busy_time += kcpustat_cpu(cpu).cpustat[CPUTIME_NICE];

	idle_time = cur_wall_time - busy_time;
	if (wall)
		*wall = jiffies_to_usecs(cur_wall_time);

	return jiffies_to_usecs(idle_time);
}

static inline cputime64_t get_cpu_idle_time(unsigned int cpu,
					    cputime64_t *wall)
{
	u64 idle_time = get_cpu_idle_time_us(cpu, wall);

	if (idle_time == -1ULL)
		idle_time = get_cpu_idle_time_jiffy(cpu, wall);
	else if (dbs_tuners_ins.io_is_busy != 1)
		idle_time += get_cpu_iowait_time_us(cpu, wall);

	return idle_time;
}

/* keep track of frequency transitions */
static int
dbs_cpufreq_notifier(struct notifier_block *nb, unsigned long val,
		     void *data)
{
	struct cpufreq_freqs *freq = data;
	struct cpu_dbs_info_s *this_dbs_info = &per_cpu(clarity_cpu_dbs_info,
							freq->cpu);

	struct cpufreq_policy *policy;

	if (!this_dbs_info->enable)
		return 0;

	policy = this_dbs_info->cur_policy;

	/*
	 * we only care if our internally tracked freq moves outside
	 * the 'valid' ranges of freqency available to us otherwise
	 * we do not change it
	*/
	if (this_dbs_info->requested_freq > policy->max
			|| this_dbs_info->requested_freq < policy->min)
		this_dbs_info->requested_freq = freq->new;

	return 0;
}

static struct notifier_block dbs_cpufreq_notifier_block = {
	.notifier_call = dbs_cpufreq_notifier
};

static void suspend_resume(bool suspend)
{
	struct cpu_dbs_info_s *cpu_info;
	struct cpufreq_policy *policy;
	unsigned int cpu;

	for_each_online_cpu(cpu) {
		cpu_info = &per_cpu(clarity_cpu_dbs_info, cpu);
		policy = cpufreq_cpu_get(cpu);
		if (suspend) {
			cpu_info->cpu_maxcur_freq = policy->max;
			policy->max = dbs_tuners_ins.freq_max_suspend;
			policy->cpuinfo.max_freq = dbs_tuners_ins.freq_max_suspend;
			pr_info("clarity governor (suspended): %u %u\n",
				policy->cpuinfo.max_freq, cpu_info->cpu_max_freq);
		} else {
			if (cpu != 0)
				cpu_info = &per_cpu(clarity_cpu_dbs_info, 0);
			policy->cpuinfo.max_freq = cpu_info->cpu_max_freq;
			policy->max =  cpu_info->cpu_maxcur_freq;
			pr_info("clarity governor (resumed): %u %u\n",
				policy->cpuinfo.max_freq, cpu_info->cpu_max_freq);
		}
		cpufreq_update_policy(cpu);
	}
}

static void clarity_power_suspend(struct power_suspend *handler)
{
	suspended = true;

	if (dbs_tuners_ins.freq_max_suspend == 0)
		return;

	suspend_resume(suspended);
}

static void clarity_late_resume(struct power_suspend *handler)
{
	struct cpu_dbs_info_s *cpu_info;
	struct cpufreq_policy *policy;
	unsigned int cpu;

	suspended = false;

	if (dbs_tuners_ins.freq_max_suspend == 0)
		return;

	suspend_resume(suspended);
	for_each_online_cpu(cpu) {
		cpu_info = &per_cpu(clarity_cpu_dbs_info, cpu);
		policy = cpufreq_cpu_get(cpu);
		__cpufreq_driver_target(cpu_info->cur_policy, dbs_tuners_ins.freq_awake,
				CPUFREQ_RELATION_L);
		pr_info("clarity governor (awake): %u at awake freq by user %u\n",
			policy->cur, dbs_tuners_ins.freq_awake);
	}
}

static struct power_suspend clarity_power_suspend_handler = {
	.suspend = clarity_power_suspend,
	.resume = clarity_late_resume,
};

/************************** sysfs interface ************************/
static ssize_t show_sampling_rate_min(struct kobject *kobj,
				      struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", min_sampling_rate);
}

define_one_global_ro(sampling_rate_min);

/* cpufreq_clarity Governor Tunables */
#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%u\n", dbs_tuners_ins.object);		\
}
show_one(sampling_rate, sampling_rate);
show_one(up_threshold, up_threshold);
show_one(down_threshold, down_threshold);
show_one(ignore_nice_load, ignore_nice);
show_one(freq_middle, freq_middle);
show_one(freq_step_up, freq_step_up);
show_one(freq_step_down, freq_step_down);
show_one(io_is_busy, io_is_busy);
show_one(freq_max_suspend, freq_max_suspend);
show_one(freq_awake, freq_awake);

/**
 * update_sampling_rate - update sampling rate effective immediately if needed.
 * @new_rate: new sampling rate
 *
 * If new rate is smaller than the old, simply updaing
 * dbs_tuners_int.sampling_rate might not be appropriate. For example,
 * if the original sampling_rate was 1 second and the requested new sampling
 * rate is 10 ms because the user needs immediate reaction from ondemand
 * governor, but not sure if higher frequency will be required or not,
 * then, the governor may change the sampling rate too late; up to 1 second
 * later. Thus, if we are reducing the sampling rate, we need to make the
 * new value effective immediately.
 */
static void update_sampling_rate(unsigned int new_rate)
{
	unsigned int cpu;

	dbs_tuners_ins.sampling_rate = new_rate
				     = max(new_rate, min_sampling_rate);

	get_online_cpus();
	for_each_online_cpu(cpu) {
		struct cpufreq_policy *policy;
		struct cpu_dbs_info_s *dbs_info;
		unsigned long next_sampling, appointed_at;

		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;
		dbs_info = &per_cpu(clarity_cpu_dbs_info, policy->cpu);
		cpufreq_cpu_put(policy);

		mutex_lock(&dbs_info->timer_mutex);

		if (!delayed_work_pending(&dbs_info->work)) {
			mutex_unlock(&dbs_info->timer_mutex);
			continue;
		}

		next_sampling  = jiffies + usecs_to_jiffies(new_rate);
		appointed_at = dbs_info->work.timer.expires;

		if (time_before(next_sampling, appointed_at)) {

			mutex_unlock(&dbs_info->timer_mutex);
			cancel_delayed_work_sync(&dbs_info->work);
			mutex_lock(&dbs_info->timer_mutex);

			queue_delayed_work_on(dbs_info->cpu, dbs_wq,
				&dbs_info->work, usecs_to_jiffies(new_rate));

		}
		mutex_unlock(&dbs_info->timer_mutex);
	}
	put_online_cpus();
}

static ssize_t store_sampling_rate(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	update_sampling_rate(input);
	return count;
}

static ssize_t store_up_threshold(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > 100 ||
			input <= dbs_tuners_ins.down_threshold)
		return -EINVAL;

	dbs_tuners_ins.up_threshold = input;
	return count;
}

static ssize_t store_down_threshold(struct kobject *a, struct attribute *b,
				    const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	/* cannot be lower than 11 otherwise freq will not fall */
	if (ret != 1 || input < 11 || input > 100 ||
			input >= dbs_tuners_ins.up_threshold)
		return -EINVAL;

	dbs_tuners_ins.down_threshold = input;
	return count;
}

static ssize_t store_ignore_nice_load(struct kobject *a, struct attribute *b,
				      const char *buf, size_t count)
{
	unsigned int input;
	int ret;

	unsigned int j;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 1)
		input = 1;

	if (input == dbs_tuners_ins.ignore_nice) /* nothing to do */
		return count;

	dbs_tuners_ins.ignore_nice = input;

	/* we need to re-evaluate prev_cpu_idle */
	for_each_online_cpu(j) {
		struct cpu_dbs_info_s *dbs_info;
		dbs_info = &per_cpu(clarity_cpu_dbs_info, j);
		dbs_info->prev_cpu_idle = get_cpu_idle_time(j,
						&dbs_info->prev_cpu_wall);
		if (dbs_tuners_ins.ignore_nice)
			dbs_info->prev_cpu_nice = kcpustat_cpu(j).cpustat[CPUTIME_NICE];
	}
	return count;
}

static ssize_t store_freq_middle(struct kobject *a, struct attribute *b,
			       const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	dbs_tuners_ins.freq_middle = input;

	return count;
}

static ssize_t store_freq_step_up(struct kobject *a, struct attribute *b,
			       const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	if (input > 100)
		input = 100;

	/* no need to test here if freq_step_up is zero as the user might actually
	 * want this, they would be crazy though :) */
	dbs_tuners_ins.freq_step_up = input;
	return count;
}

static ssize_t store_freq_step_down(struct kobject *a, struct attribute *b,
			       const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	if (input > 100)
		input = 100;

	/* no need to test here if freq_step_down is zero as the user might actually
	 * want this, they would be crazy though :) */
	dbs_tuners_ins.freq_step_down = input;
	return count;
}

static ssize_t store_io_is_busy(struct kobject *a, struct attribute *b,
			       const char *buf, size_t count)
{
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > 1)
		return -EINVAL;

	dbs_tuners_ins.io_is_busy = input;

	return count;
}

static ssize_t store_freq_max_suspend(struct kobject *a, struct attribute *b,
			       const char *buf, size_t count)
{
	struct cpufreq_policy *policy;
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	policy = cpufreq_cpu_get(0);

	if (ret != 1 ||
		input > policy->max ||
		input < 0 ||
		input < dbs_tuners_ins.freq_middle)
		return -EINVAL;

	dbs_tuners_ins.freq_max_suspend = input;

	return count;
}

static ssize_t store_freq_awake(struct kobject *a, struct attribute *b,
			       const char *buf, size_t count)
{
	struct cpufreq_policy *policy;
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	policy = cpufreq_cpu_get(0);

	if (ret != 1 ||
		input > policy->max ||
		input < 0)
		return -EINVAL;

	dbs_tuners_ins.freq_awake = input;

	return count;
}

define_one_global_rw(sampling_rate);
define_one_global_rw(up_threshold);
define_one_global_rw(down_threshold);
define_one_global_rw(ignore_nice_load);
define_one_global_rw(freq_middle);
define_one_global_rw(freq_step_up);
define_one_global_rw(freq_step_down);
define_one_global_rw(io_is_busy);
define_one_global_rw(freq_max_suspend);
define_one_global_rw(freq_awake);

static struct attribute *dbs_attributes[] = {
	&sampling_rate_min.attr,
	&sampling_rate.attr,
	&up_threshold.attr,
	&down_threshold.attr,
	&ignore_nice_load.attr,
	&freq_middle.attr,
	&freq_step_up.attr,
	&freq_step_down.attr,
	&io_is_busy.attr,
	&freq_max_suspend.attr,
	&freq_awake.attr,
	NULL
};

static struct attribute_group dbs_attr_group = {
	.attrs = dbs_attributes,
	.name = "clarity",
};

/************************** sysfs end ************************/

static void dbs_check_cpu(struct cpu_dbs_info_s *this_dbs_info)
{
	unsigned int load = 0;
	unsigned int max_load = 0;
	unsigned int freq_target;

	struct cpufreq_policy *policy;
	unsigned int j;

	policy = this_dbs_info->cur_policy;

	/*
	 * Every sampling_rate, we check, if current idle time is less
	 * than 20% (default), then we try to increase frequency
	 * if current idle time is more than 80%, then we try to decrease frequency
	 *
	 * Any frequency increase takes it to the maximum frequency.
	 * Frequency reduction happens at minimum steps of
	 * 5% (default) of maximum frequency
	 */

	/* Get Absolute Load */
	for_each_cpu(j, policy->cpus) {
		struct cpu_dbs_info_s *j_dbs_info;
		cputime64_t cur_wall_time, cur_idle_time;
		unsigned int idle_time, wall_time;

		j_dbs_info = &per_cpu(clarity_cpu_dbs_info, j);

		cur_idle_time = get_cpu_idle_time(j, &cur_wall_time);

		wall_time = (unsigned int)
			(cur_wall_time - j_dbs_info->prev_cpu_wall);
		j_dbs_info->prev_cpu_wall = cur_wall_time;

		idle_time = (unsigned int)
			(cur_idle_time - j_dbs_info->prev_cpu_idle);
		j_dbs_info->prev_cpu_idle = cur_idle_time;

		if (dbs_tuners_ins.ignore_nice) {
			u64 cur_nice;
			unsigned long cur_nice_jiffies;

			cur_nice = kcpustat_cpu(j).cpustat[CPUTIME_NICE] -
					 j_dbs_info->prev_cpu_nice;
			/*
			 * Assumption: nice time between sampling periods will
			 * be less than 2^32 jiffies for 32 bit sys
			 */
			cur_nice_jiffies = (unsigned long)
					cputime64_to_jiffies64(cur_nice);

			j_dbs_info->prev_cpu_nice = kcpustat_cpu(j).cpustat[CPUTIME_NICE];
			idle_time += jiffies_to_usecs(cur_nice_jiffies);
		}

		if (unlikely(!wall_time || wall_time < idle_time))
			continue;

		load = 100 * (wall_time - idle_time) / wall_time;

		if (load > max_load)
			max_load = load;
	}

	/* Check for frequency increase */
	if (max_load > dbs_tuners_ins.up_threshold)
	{
		if (dbs_tuners_ins.freq_step_up == 0)
			return;

		this_dbs_info->down_skip = 0;

		/* if we are already at full speed then break out early */
		if (this_dbs_info->requested_freq == policy->max)
			return;

		freq_target = (dbs_tuners_ins.freq_step_up *
					policy->cpuinfo.max_freq) / 100;

		/* max freq cannot be less than 100. But who knows.... */
		if (unlikely(freq_target == 0))
			freq_target = 5;

		this_dbs_info->requested_freq += freq_target;
		if (this_dbs_info->requested_freq > policy->max)
			this_dbs_info->requested_freq = policy->max;

		__cpufreq_driver_target(policy, this_dbs_info->requested_freq,
			CPUFREQ_RELATION_L);

		return;
	}
	else if (max_load < dbs_tuners_ins.down_threshold)
	{
		if (dbs_tuners_ins.freq_step_down == 0)
			return;

		freq_target = (dbs_tuners_ins.freq_step_down *
					policy->cpuinfo.max_freq) / 100;

		this_dbs_info->requested_freq -= freq_target;
		if (this_dbs_info->requested_freq < policy->min)
			this_dbs_info->requested_freq = policy->min;

		/*
		 * if we cannot reduce the frequency anymore, break out early
		 */
		if (policy->cur == policy->min)
			return;

		__cpufreq_driver_target(policy, this_dbs_info->requested_freq,
				CPUFREQ_RELATION_H);

		return;

	} else {
		if (policy->cur == dbs_tuners_ins.freq_middle)
			return;

		this_dbs_info->requested_freq = dbs_tuners_ins.freq_middle;

		if (this_dbs_info->requested_freq > policy->max)
			this_dbs_info->requested_freq = policy->max;
		else if (this_dbs_info->requested_freq < policy->min)
			this_dbs_info->requested_freq = policy->min;

		__cpufreq_driver_target(policy, this_dbs_info->requested_freq,
				CPUFREQ_RELATION_L);

		return;
	}
}

static void do_dbs_timer(struct work_struct *work)
{
	struct cpu_dbs_info_s *dbs_info =
		container_of(work, struct cpu_dbs_info_s, work.work);
	unsigned int cpu = dbs_info->cpu;

	/* We want all CPUs to do sampling nearly on same jiffy */
	int delay = usecs_to_jiffies(dbs_tuners_ins.sampling_rate);

	delay -= jiffies % delay;

	mutex_lock(&dbs_info->timer_mutex);

	dbs_check_cpu(dbs_info);

	queue_delayed_work_on(cpu, dbs_wq, &dbs_info->work, delay);
	mutex_unlock(&dbs_info->timer_mutex);
}

static inline void dbs_timer_init(struct cpu_dbs_info_s *dbs_info)
{
	/* We want all CPUs to do sampling nearly on same jiffy */
	int delay = usecs_to_jiffies(dbs_tuners_ins.sampling_rate);

	if (num_online_cpus() > 1)
		delay -= jiffies % delay;

	dbs_info->enable = 1;
	INIT_DELAYED_WORK_DEFERRABLE(&dbs_info->work, do_dbs_timer);
	queue_delayed_work_on(dbs_info->cpu, dbs_wq, &dbs_info->work, delay);
}

static inline void dbs_timer_exit(struct cpu_dbs_info_s *dbs_info)
{
	dbs_info->enable = 0;
	cancel_delayed_work_sync(&dbs_info->work);
}

static int cpufreq_governor_dbs(struct cpufreq_policy *policy,
				   unsigned int event)
{
	struct cpu_dbs_info_s *this_dbs_info;
	struct cpu_dbs_info_s *cpu_info;
	unsigned int cpu = policy->cpu;
	unsigned int j;
	int rc;

	this_dbs_info = &per_cpu(clarity_cpu_dbs_info, cpu);

	switch (event) {
	case CPUFREQ_GOV_START:
		if ((!cpu_online(cpu)) || (!policy->cur))
			return -EINVAL;

		dbs_wq = alloc_workqueue("clarity_dbs_wq", WQ_HIGHPRI, 0);
		if (!dbs_wq) {
			printk(KERN_ERR "Failed to create clarity_dbs_wq workqueue\n");
			return -EFAULT;
		}

		mutex_lock(&dbs_mutex);

		suspended = false;

		cpu_info = &per_cpu(clarity_cpu_dbs_info, 0);
		policy = cpufreq_cpu_get(0);
		cpu_info->cpu_max_freq = policy->cpuinfo.max_freq;
		cpu_info->cpu_maxcur_freq = policy->max;

		for_each_cpu(j, policy->cpus) {
			struct cpu_dbs_info_s *j_dbs_info;
			j_dbs_info = &per_cpu(clarity_cpu_dbs_info, j);
			j_dbs_info->cur_policy = policy;

			j_dbs_info->prev_cpu_idle = get_cpu_idle_time(j,
						&j_dbs_info->prev_cpu_wall);
			if (dbs_tuners_ins.ignore_nice)
				j_dbs_info->prev_cpu_nice =
						kcpustat_cpu(j).cpustat[CPUTIME_NICE];
		}
		this_dbs_info->down_skip = 0;
		this_dbs_info->requested_freq = policy->cur;

		mutex_init(&this_dbs_info->timer_mutex);
		dbs_enable++;
		/*
		 * Start the timerschedule work, when this governor
		 * is used for first time
		 */
		if (dbs_enable == 1) {
			u64 idle_time;
			unsigned int latency;
			int cpu = get_cpu();

			idle_time = get_cpu_idle_time_us(cpu, NULL);
			put_cpu();
			if (idle_time != -1ULL)
				min_sampling_rate = MICRO_FREQUENCY_MIN_SAMPLE_RATE;
			else
				min_sampling_rate = MIN_SAMPLING_RATE_RATIO * jiffies_to_usecs(10);

			rc = sysfs_create_group(cpufreq_global_kobject,
						&dbs_attr_group);
			if (rc) {
				mutex_unlock(&dbs_mutex);
				return rc;
			}


			/* policy latency is in nS. Convert it to uS first */
			latency = policy->cpuinfo.transition_latency / 1000;
			if (latency == 0)
				latency = 1;
			/* Bring kernel and HW constraints together */
			min_sampling_rate = max(min_sampling_rate,
					MIN_LATENCY_MULTIPLIER * latency);

			cpufreq_register_notifier(&dbs_cpufreq_notifier_block,
					CPUFREQ_TRANSITION_NOTIFIER);
		}

		register_power_suspend(&clarity_power_suspend_handler);
		mutex_unlock(&dbs_mutex);

		dbs_timer_init(this_dbs_info);

		break;

	case CPUFREQ_GOV_STOP:
		dbs_timer_exit(this_dbs_info);

		mutex_lock(&dbs_mutex);
		dbs_enable--;
		mutex_destroy(&this_dbs_info->timer_mutex);
		unregister_power_suspend(&clarity_power_suspend_handler);
		destroy_workqueue(dbs_wq);

		/*
		 * Stop the timerschedule work, when this governor
		 * is used for first time
		 */
		if (dbs_enable == 0)
			cpufreq_unregister_notifier(
					&dbs_cpufreq_notifier_block,
					CPUFREQ_TRANSITION_NOTIFIER);

		mutex_unlock(&dbs_mutex);
		if (!dbs_enable)
			sysfs_remove_group(cpufreq_global_kobject,
					   &dbs_attr_group);

		break;

	case CPUFREQ_GOV_LIMITS:
		mutex_lock(&this_dbs_info->timer_mutex);
		if (policy->max < this_dbs_info->cur_policy->cur)
			__cpufreq_driver_target(this_dbs_info->cur_policy,
					policy->max, CPUFREQ_RELATION_H);
		else if (policy->min > this_dbs_info->cur_policy->cur)
			__cpufreq_driver_target(this_dbs_info->cur_policy,
					policy->min, CPUFREQ_RELATION_L);
		mutex_unlock(&this_dbs_info->timer_mutex);

		break;
	}
	return 0;
}

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_CLARITY
static
#endif
struct cpufreq_governor cpufreq_gov_clarity = {
	.name			= "clarity",
	.governor		= cpufreq_governor_dbs,
	.max_transition_latency	= TRANSITION_LATENCY_LIMIT,
	.owner			= THIS_MODULE,
};

static int __init cpufreq_gov_dbs_init(void)
{
	return cpufreq_register_governor(&cpufreq_gov_clarity);
}

static void __exit cpufreq_gov_dbs_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_clarity);
}

MODULE_AUTHOR("Ryan Andri <ryan.omnia@gmail.com>");
MODULE_DESCRIPTION("'cpufreq_clarity' - A smart & dynamic cpufreq governor based on conservative");
MODULE_LICENSE("GPL");

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_CLARITY
fs_initcall(cpufreq_gov_dbs_init);
#else
module_init(cpufreq_gov_dbs_init);
#endif
module_exit(cpufreq_gov_dbs_exit);
