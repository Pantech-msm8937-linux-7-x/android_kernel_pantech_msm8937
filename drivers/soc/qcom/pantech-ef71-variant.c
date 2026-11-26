// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pantech VEGA IRON 2 variants
 *
 * The EF71S and the EF71AS share one device tree: the EF71S has an Atmel
 * mXT641T touchscreen on an NT35695 panel, the EF71AS an FT8716 touch and
 * display driver, with both touchscreens on the same pins. As on msm-3.18,
 * the bootloader tells them apart, naming the panel it found in
 * mdss_mdp.panel. Nodes with a "pantech,panel" not named there are not
 * fitted: disable them before anything looks at them, so the pins both
 * touchscreens refer to are not taken for shared ones either.
 *
 * Copyright (c) 2026, hiru <rlawoehd187@naver.com>
 */

#include <linux/init.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/string.h>

static int __init ef71_variant_init(void)
{
	struct device_node *np;
	const char *bootargs, *panel;
	struct property *prop;

	if (!of_machine_is_compatible("pantech,ef71") || !of_chosen ||
	    of_property_read_string(of_chosen, "bootargs", &bootargs))
		return 0;

	for_each_node_with_property(np, "pantech,panel") {
		if (of_property_read_string(np, "pantech,panel", &panel) ||
		    strstr(bootargs, panel))
			continue;

		prop = kzalloc(sizeof(*prop), GFP_KERNEL);
		if (!prop) {
			of_node_put(np);
			return -ENOMEM;
		}
		prop->name = "status";
		prop->value = "disabled";
		prop->length = sizeof("disabled");

		if (of_update_property(np, prop))
			kfree(prop);
		else
			pr_info("%pOF: not fitted\n", np);
	}

	return 0;
}
/* before the shared GPIO scan (postcore) and any device is created */
core_initcall(ef71_variant_init);
