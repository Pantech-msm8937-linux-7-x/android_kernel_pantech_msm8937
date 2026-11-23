// SPDX-License-Identifier: GPL-2.0-only
/*
 * Input boost: when the user touches the screen or presses a key, raise the
 * minimum frequency of the CPUs for a while, so that what the input makes
 * the system do (scrolling, opening an app) does not wait for the governor
 * to see the load. As the input boost of the Qualcomm kernels (cpu-boost),
 * on the frequency QoS of cpufreq.
 *
 * Parameters (in /sys/module/cpu_input_boost/parameters):
 *   input_boost_freq  "cpu:kHz ..." the minimum frequency of the policy of
 *                     each cpu given while boosting (0 or absent: none)
 *   input_boost_ms    how long a boost lasts after the last input
 *   enabled           0 to stop boosting
 */

#define pr_fmt(fmt) "cpu_input_boost: " fmt

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/pm_qos.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

static char input_boost_freq[128] = CONFIG_CPU_INPUT_BOOST_FREQ;
module_param_string(input_boost_freq, input_boost_freq,
		    sizeof(input_boost_freq), 0644);
MODULE_PARM_DESC(input_boost_freq, "cpu:kHz pairs, the boosted minimums");

static unsigned int input_boost_ms = CONFIG_CPU_INPUT_BOOST_MS;
module_param(input_boost_ms, uint, 0644);
MODULE_PARM_DESC(input_boost_ms, "How long a boost lasts, in ms");

static bool enabled = true;
module_param(enabled, bool, 0644);
MODULE_PARM_DESC(enabled, "Boost on input");

/* One request on the minimum of each policy */
struct boost_policy {
	struct list_head node;
	struct cpufreq_policy *policy;
	struct freq_qos_request req;
};

static LIST_HEAD(boost_policies);
static DEFINE_MUTEX(boost_lock);
static struct workqueue_struct *boost_wq;
static bool boosted;

static void unboost_fn(struct work_struct *work);
static void boost_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(unboost_work, unboost_fn);
static DECLARE_WORK(boost_work, boost_fn);

/* The boosted minimum of a policy: that given for one of its cpus */
static unsigned int boost_freq_of(struct cpufreq_policy *policy)
{
	char buf[sizeof(input_boost_freq)];
	char *s = buf, *tok;
	unsigned int cpu, freq, ret = 0;

	strscpy(buf, input_boost_freq, sizeof(buf));
	while ((tok = strsep(&s, " ,")) != NULL) {
		if (sscanf(tok, "%u:%u", &cpu, &freq) != 2)
			continue;
		if (cpu < nr_cpu_ids && cpumask_test_cpu(cpu, policy->related_cpus))
			ret = max(ret, freq);
	}
	return ret;
}

static void boost_fn(struct work_struct *work)
{
	struct boost_policy *bp;

	mutex_lock(&boost_lock);
	list_for_each_entry(bp, &boost_policies, node)
		freq_qos_update_request(&bp->req, boost_freq_of(bp->policy));
	boosted = true;
	mutex_unlock(&boost_lock);
}

static void unboost_fn(struct work_struct *work)
{
	struct boost_policy *bp;

	cancel_work_sync(&boost_work);
	mutex_lock(&boost_lock);
	list_for_each_entry(bp, &boost_policies, node)
		freq_qos_update_request(&bp->req, FREQ_QOS_MIN_DEFAULT_VALUE);
	boosted = false;
	mutex_unlock(&boost_lock);
}

static void boost_input_event(struct input_handle *handle, unsigned int type,
			      unsigned int code, int value)
{
	if (!enabled || !input_boost_ms)
		return;

	/* raise the minimums once, then only push the end further */
	if (!READ_ONCE(boosted))
		queue_work(boost_wq, &boost_work);
	mod_delayed_work(boost_wq, &unboost_work,
			 msecs_to_jiffies(input_boost_ms));
}

static int boost_input_connect(struct input_handler *handler,
			       struct input_dev *dev,
			       const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "cpu_input_boost";

	ret = input_register_handle(handle);
	if (ret)
		goto err_free;
	ret = input_open_device(handle);
	if (ret)
		goto err_unregister;
	return 0;

err_unregister:
	input_unregister_handle(handle);
err_free:
	kfree(handle);
	return ret;
}

static void boost_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

/* Touchscreens, touchpads and keys, as cpu-boost matches them */
static const struct input_device_id boost_ids[] = {
	/* multi-touch touchscreens */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			 INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			    BIT_MASK(ABS_MT_POSITION_X) |
			    BIT_MASK(ABS_MT_POSITION_Y) },
	},
	/* touchpads */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			 INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			    BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
	},
	/* keypads */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{ },
};

static struct input_handler boost_input_handler = {
	.event		= boost_input_event,
	.connect	= boost_input_connect,
	.disconnect	= boost_input_disconnect,
	.name		= "cpu_input_boost",
	.id_table	= boost_ids,
};

static void boost_remove_policies(void)
{
	struct boost_policy *bp, *tmp;

	list_for_each_entry_safe(bp, tmp, &boost_policies, node) {
		freq_qos_remove_request(&bp->req);
		cpufreq_cpu_put(bp->policy);
		list_del(&bp->node);
		kfree(bp);
	}
}

static int __init cpu_input_boost_init(void)
{
	struct cpufreq_policy *policy;
	struct boost_policy *bp;
	unsigned int cpu;
	int ret;

	boost_wq = alloc_workqueue("cpu_input_boost", WQ_HIGHPRI | WQ_UNBOUND, 0);
	if (!boost_wq)
		return -ENOMEM;

	/* a request on the minimum of each policy, at its default for now */
	cpus_read_lock();
	for_each_online_cpu(cpu) {
		policy = cpufreq_cpu_get(cpu);
		if (!policy)
			continue;
		if (cpumask_first(policy->related_cpus) != cpu) {
			cpufreq_cpu_put(policy);
			continue;
		}

		bp = kzalloc(sizeof(*bp), GFP_KERNEL);
		if (!bp) {
			cpufreq_cpu_put(policy);
			ret = -ENOMEM;
			goto err;
		}
		bp->policy = policy;
		ret = freq_qos_add_request(&policy->constraints, &bp->req,
					   FREQ_QOS_MIN, FREQ_QOS_MIN_DEFAULT_VALUE);
		if (ret < 0) {
			cpufreq_cpu_put(policy);
			kfree(bp);
			goto err;
		}
		list_add_tail(&bp->node, &boost_policies);
	}
	cpus_read_unlock();

	if (list_empty(&boost_policies)) {
		pr_err("no cpufreq policies\n");
		ret = -ENODEV;
		goto err_wq;
	}

	ret = input_register_handler(&boost_input_handler);
	if (ret)
		goto err_policies;

	pr_info("boosting to \"%s\" for %u ms on input\n", input_boost_freq,
		input_boost_ms);
	return 0;

err:
	cpus_read_unlock();
err_policies:
	boost_remove_policies();
err_wq:
	destroy_workqueue(boost_wq);
	return ret;
}

static void __exit cpu_input_boost_exit(void)
{
	input_unregister_handler(&boost_input_handler);
	cancel_delayed_work_sync(&unboost_work);
	cancel_work_sync(&boost_work);
	boost_remove_policies();
	destroy_workqueue(boost_wq);
}

/* after cpufreq has its policies */
late_initcall(cpu_input_boost_init);
module_exit(cpu_input_boost_exit);

MODULE_DESCRIPTION("Raise the CPU minimum frequencies on input");
MODULE_LICENSE("GPL");
