// SPDX-License-Identifier: GPL-2.0-only
/*
 * The memory accelerator (SRAM read/write assist) of the APC rail of
 * MSM8937: the vendor mem-acc regulator writes a few registers once at boot
 * (its qcom,acc-init-reg-config) and changes others with the CPR corner. The
 * corner registers come up with the TURBO values, which are right at every
 * voltage the CPUs use here, but the boot values of the init registers left
 * the L1/L2 SRAM corrupting data above 1.1 GHz (undefined instructions,
 * wild pointers). Write the init values before cpufreq raises the clocks.
 */

#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

static int mem_acc_init_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	int n, i;
	u32 *vals;

	n = of_property_count_u32_elems(dev->of_node, "qcom,init-values");
	if (n <= 0)
		return dev_err_probe(dev, -EINVAL, "no qcom,init-values\n");

	vals = devm_kcalloc(dev, n, sizeof(*vals), GFP_KERNEL);
	if (!vals)
		return -ENOMEM;
	of_property_read_u32_array(dev->of_node, "qcom,init-values", vals, n);

	for (i = 0; i < n; i++) {
		void __iomem *reg = devm_platform_ioremap_resource(pdev, i);

		if (IS_ERR(reg))
			return PTR_ERR(reg);
		writel_relaxed(vals[i], reg);
	}
	/* before the CPU clocks go up */
	mb();

	return 0;
}

static const struct of_device_id mem_acc_init_match[] = {
	{ .compatible = "qcom,msm8937-mem-acc" },
	{ }
};

static struct platform_driver mem_acc_init_driver = {
	.probe = mem_acc_init_probe,
	.driver = {
		.name = "qcom-mem-acc-init",
		.of_match_table = mem_acc_init_match,
		.suppress_bind_attrs = true,
	},
};

/* before cpufreq: the devices are created at arch_initcall_sync */
static int __init mem_acc_init_register(void)
{
	return platform_driver_register(&mem_acc_init_driver);
}
arch_initcall(mem_acc_init_register);
