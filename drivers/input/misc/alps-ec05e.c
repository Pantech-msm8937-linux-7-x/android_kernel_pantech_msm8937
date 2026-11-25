// SPDX-License-Identifier: GPL-2.0-only
/*
 * ALPS EC05E rotary encoder on two GPIOs, the volume wheel of the Pantech
 * VEGA IRON 2 (EF71).
 *
 * Each detent pulls terminal A and terminal B low and releases them in turn.
 * Turning clockwise, A falls while B is still high and rises while B is
 * low; counterclockwise the same holds for B against A. Each detent reports
 * a key press and release, as the Pantech driver did in its volume mode:
 * linux,keycodes gives the key of clockwise and counterclockwise turns.
 *
 * Copyright (c) 2026, hiru <rlawoehd187@naver.com>
 */

#include <linux/gpio/consumer.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/spinlock.h>

#define EC05E_NAME "gpio-wheel-alps-ec05e"

enum { TERM_A, TERM_B, TERM_NUM };

struct ec05e {
	struct input_dev *input;
	struct gpio_desc *gpio[TERM_NUM];
	int irq[TERM_NUM];
	unsigned int keycode[TERM_NUM];	/* clockwise, counterclockwise */
	bool armed[TERM_NUM];
	spinlock_t lock;
};

static void ec05e_report(struct ec05e *ec, unsigned int code)
{
	input_report_key(ec->input, code, 1);
	input_sync(ec->input);
	input_report_key(ec->input, code, 0);
	input_sync(ec->input);
}

/*
 * The levels are those of the pins (both pulled up, low while a terminal
 * makes contact), as the Pantech driver read them.
 */
static void ec05e_edge(struct ec05e *ec, int term)
{
	int other = term == TERM_A ? TERM_B : TERM_A;
	int self_level = gpiod_get_raw_value(ec->gpio[term]);
	int other_level = gpiod_get_raw_value(ec->gpio[other]);
	unsigned long flags;

	if (self_level < 0 || other_level < 0)
		return;

	spin_lock_irqsave(&ec->lock, flags);
	if (!self_level && other_level) {
		/* this terminal leads the detent */
		ec->armed[term] = true;
	} else if (self_level && !other_level && ec->armed[term]) {
		ec->armed[term] = false;
		ec05e_report(ec, ec->keycode[term]);
	} else {
		ec->armed[term] = false;
	}
	spin_unlock_irqrestore(&ec->lock, flags);
}

static irqreturn_t ec05e_irq_a(int irq, void *data)
{
	ec05e_edge(data, TERM_A);
	return IRQ_HANDLED;
}

static irqreturn_t ec05e_irq_b(int irq, void *data)
{
	ec05e_edge(data, TERM_B);
	return IRQ_HANDLED;
}

static int ec05e_probe(struct platform_device *pdev)
{
	static const char *const names[TERM_NUM] = { "a", "b" };
	static const irq_handler_t handlers[TERM_NUM] = { ec05e_irq_a, ec05e_irq_b };
	struct device *dev = &pdev->dev;
	struct ec05e *ec;
	bool wakeup;
	int i, ret;

	ec = devm_kzalloc(dev, sizeof(*ec), GFP_KERNEL);
	if (!ec)
		return -ENOMEM;
	spin_lock_init(&ec->lock);

	ec->keycode[TERM_A] = KEY_VOLUMEDOWN;
	ec->keycode[TERM_B] = KEY_VOLUMEUP;
	device_property_read_u32_array(dev, "linux,keycodes", ec->keycode, TERM_NUM);

	ec->input = devm_input_allocate_device(dev);
	if (!ec->input)
		return -ENOMEM;
	ec->input->name = EC05E_NAME;
	ec->input->phys = EC05E_NAME "/input0";
	ec->input->id.bustype = BUS_HOST;
	for (i = 0; i < TERM_NUM; i++)
		input_set_capability(ec->input, EV_KEY, ec->keycode[i]);

	for (i = 0; i < TERM_NUM; i++) {
		ec->gpio[i] = devm_gpiod_get(dev, names[i], GPIOD_IN);
		if (IS_ERR(ec->gpio[i]))
			return dev_err_probe(dev, PTR_ERR(ec->gpio[i]),
					     "failed to get the %s terminal\n", names[i]);
		ec->irq[i] = gpiod_to_irq(ec->gpio[i]);
		if (ec->irq[i] < 0)
			return dev_err_probe(dev, ec->irq[i], "no interrupt for the %s terminal\n",
					     names[i]);
	}

	ret = input_register_device(ec->input);
	if (ret)
		return dev_err_probe(dev, ret, "failed to register the input device\n");

	for (i = 0; i < TERM_NUM; i++) {
		ret = devm_request_irq(dev, ec->irq[i], handlers[i],
				       IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
				       EC05E_NAME, ec);
		if (ret)
			return dev_err_probe(dev, ret, "failed to request the %s interrupt\n",
					     names[i]);
	}

	/* The Pantech kernel left the wheel off as a wakeup source */
	wakeup = device_property_read_bool(dev, "wakeup-source");
	device_init_wakeup(dev, wakeup);

	platform_set_drvdata(pdev, ec);
	return 0;
}

static int ec05e_suspend(struct device *dev)
{
	struct ec05e *ec = dev_get_drvdata(dev);
	int i;

	if (device_may_wakeup(dev))
		for (i = 0; i < TERM_NUM; i++)
			enable_irq_wake(ec->irq[i]);
	return 0;
}

static int ec05e_resume(struct device *dev)
{
	struct ec05e *ec = dev_get_drvdata(dev);
	int i;

	if (device_may_wakeup(dev))
		for (i = 0; i < TERM_NUM; i++)
			disable_irq_wake(ec->irq[i]);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(ec05e_pm_ops, ec05e_suspend, ec05e_resume);

static const struct of_device_id ec05e_of_match[] = {
	{ .compatible = "alps,ec05e" },
	{ }
};
MODULE_DEVICE_TABLE(of, ec05e_of_match);

static struct platform_driver ec05e_driver = {
	.probe = ec05e_probe,
	.driver = {
		.name = "alps-ec05e",
		.of_match_table = ec05e_of_match,
		.pm = pm_sleep_ptr(&ec05e_pm_ops),
	},
};
module_platform_driver(ec05e_driver);

MODULE_DESCRIPTION("ALPS EC05E rotary encoder wheel");
MODULE_LICENSE("GPL");
