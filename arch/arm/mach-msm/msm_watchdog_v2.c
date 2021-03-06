/* Copyright (c) 2012, Code Aurora Forum. All rights reserved.
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

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/jiffies.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/of.h>
#include <linux/cpu.h>
#include <linux/platform_device.h>
#include <mach/scm.h>
#include <mach/msm_memory_dump.h>

#define MODULE_NAME "msm_watchdog"
#define WDT0_ACCSCSSNBARK_INT 0
#define TCSR_WDT_CFG	0x30
#define WDT0_RST	0x04
#define WDT0_EN		0x08
#define WDT0_STS	0x0C
#define WDT0_BARK_TIME	0x10
#define WDT0_BITE_TIME	0x14

#define MASK_SIZE		32
#define SCM_SET_REGSAVE_CMD	0x2

struct msm_watchdog_data {
	unsigned int __iomem phys_base;
	size_t size;
	void __iomem *base;
	struct device *dev;
	unsigned int pet_time;
	unsigned int bark_time;
	unsigned int bark_irq;
	unsigned int bite_irq;
	unsigned int do_ipi_ping;
	unsigned long long last_pet;
	unsigned min_slack_ticks;
	unsigned long long min_slack_ns;
	void *scm_regsave;
	cpumask_t alive_mask;
	struct work_struct init_dogwork_struct;
	struct delayed_work dogwork_struct;
	struct notifier_block panic_blk;
};

/*
 * On the kernel command line specify
 * msm_watchdog.enable=1 to enable the watchdog
 * By default watchdog is turned on
 */
static int enable = 1;
module_param(enable, int, 0);

/*
 * On the kernel command line specify
 * msm_watchdog.WDT_HZ=<clock val in HZ> to set Watchdog
 * ticks. By default it is set to 32765.
 */
static long WDT_HZ = 32765;
module_param(WDT_HZ, long, 0);

/*
 * If the watchdog is enabled at bootup (enable=1),
 * the runtime_disable sysfs node at
 * /sys/module/msm_watchdog/parameters/runtime_disable
 * can be used to deactivate the watchdog.
 * This is a one-time setting. The watchdog
 * cannot be re-enabled once it is disabled.
 */
static int runtime_disable;
static int wdog_enable_set(const char *val, struct kernel_param *kp);
module_param_call(runtime_disable, wdog_enable_set, param_get_int,
			&runtime_disable, 0644);

static void pet_watchdog_work(struct work_struct *work);
static void init_watchdog_work(struct work_struct *work);

static void dump_cpu_alive_mask(struct msm_watchdog_data *wdog_dd)
{
	static char alive_mask_buf[MASK_SIZE];
	size_t count = cpulist_scnprintf(alive_mask_buf, MASK_SIZE,
						&wdog_dd->alive_mask);
	alive_mask_buf[count] = '\n';
	alive_mask_buf[count++] = '\0';
	printk(KERN_INFO "cpu alive mask from last pet\n%s", alive_mask_buf);
}

static int msm_watchdog_suspend(struct device *dev)
{
	struct msm_watchdog_data *wdog_dd =
			(struct msm_watchdog_data *)dev_get_drvdata(dev);
	if (!enable)
		return 0;
	__raw_writel(1, wdog_dd->base + WDT0_RST);
	__raw_writel(0, wdog_dd->base + WDT0_EN);
	mb();
	return 0;
}

static int msm_watchdog_resume(struct device *dev)
{
	struct msm_watchdog_data *wdog_dd =
			(struct msm_watchdog_data *)dev_get_drvdata(dev);
	if (!enable)
		return 0;
	__raw_writel(1, wdog_dd->base + WDT0_EN);
	__raw_writel(1, wdog_dd->base + WDT0_RST);
	mb();
	return 0;
}

static int panic_wdog_handler(struct notifier_block *this,
			      unsigned long event, void *ptr)
{
	struct msm_watchdog_data *wdog_dd = container_of(this,
				struct msm_watchdog_data, panic_blk);
	if (panic_timeout == 0) {
		__raw_writel(0, wdog_dd->base + WDT0_EN);
		mb();
	} else {
		__raw_writel(WDT_HZ * (panic_timeout + 4),
				wdog_dd->base + WDT0_BARK_TIME);
		__raw_writel(WDT_HZ * (panic_timeout + 4),
				wdog_dd->base + WDT0_BITE_TIME);
		__raw_writel(1, wdog_dd->base + WDT0_RST);
	}
	return NOTIFY_DONE;
}
/*
 * TODO: implement enable/disable.
 */
static int wdog_enable_set(const char *val, struct kernel_param *kp)
{
	return 0;
}


static void pet_watchdog(struct msm_watchdog_data *wdog_dd)
{
	int slack;
	unsigned long long time_ns;
	unsigned long long slack_ns;
	unsigned long long bark_time_ns = wdog_dd->bark_time * 1000000ULL;

	slack = __raw_readl(wdog_dd->base + WDT0_STS) >> 3;
	slack = ((wdog_dd->bark_time*WDT_HZ)/1000) - slack;
	if (slack < wdog_dd->min_slack_ticks)
		wdog_dd->min_slack_ticks = slack;
	__raw_writel(1, wdog_dd->base + WDT0_RST);
	time_ns = sched_clock();
	slack_ns = (wdog_dd->last_pet + bark_time_ns) - time_ns;
	if (slack_ns < wdog_dd->min_slack_ns)
		wdog_dd->min_slack_ns = slack_ns;
	wdog_dd->last_pet = time_ns;
}

static void keep_alive_response(void *info)
{
	int cpu = smp_processor_id();
	struct msm_watchdog_data *wdog_dd = (struct msm_watchdog_data *)info;
	cpumask_set_cpu(cpu, &wdog_dd->alive_mask);
	smp_mb();
}

/*
 * If this function does not return, it implies one of the
 * other cpu's is not responsive.
 */
static void ping_other_cpus(struct msm_watchdog_data *wdog_dd)
{
	int cpu;
	cpumask_clear(&wdog_dd->alive_mask);
	smp_mb();
	for_each_cpu(cpu, cpu_online_mask)
		smp_call_function_single(cpu, keep_alive_response, wdog_dd, 1);
}

static void pet_watchdog_work(struct work_struct *work)
{
	unsigned long delay_time;
	struct delayed_work *delayed_work = to_delayed_work(work);
	struct msm_watchdog_data *wdog_dd = container_of(delayed_work,
						struct msm_watchdog_data,
							dogwork_struct);
	delay_time = msecs_to_jiffies(wdog_dd->pet_time);
	if (wdog_dd->do_ipi_ping)
		ping_other_cpus(wdog_dd);
	pet_watchdog(wdog_dd);
	if (wdog_dd->do_ipi_ping)
		dump_cpu_alive_mask(wdog_dd);
	if (enable)
		schedule_delayed_work(&wdog_dd->dogwork_struct,
							delay_time);
}

static int msm_watchdog_remove(struct platform_device *pdev)
{
	struct msm_watchdog_data *wdog_dd =
			(struct msm_watchdog_data *)platform_get_drvdata(pdev);
	if (enable) {
		__raw_writel(0, wdog_dd->base + WDT0_EN);
		mb();
		enable = 0;
		/*
		 * TODO: Not sure if we need to call into TZ to disable
		 * secure wdog.
		 */
		/* In case we got suspended mid-exit */
		__raw_writel(0, wdog_dd->base + WDT0_EN);
	}
	printk(KERN_INFO "MSM Watchdog Exit - Deactivated\n");
	kzfree(wdog_dd);
	return 0;
}

static irqreturn_t wdog_bark_handler(int irq, void *dev_id)
{
	struct msm_watchdog_data *wdog_dd = (struct msm_watchdog_data *)dev_id;
	unsigned long nanosec_rem;
	unsigned long long t = sched_clock();

	nanosec_rem = do_div(t, 1000000000);
	printk(KERN_INFO "Watchdog bark! Now = %lu.%06lu\n", (unsigned long) t,
		nanosec_rem / 1000);

	nanosec_rem = do_div(wdog_dd->last_pet, 1000000000);
	printk(KERN_INFO "Watchdog last pet at %lu.%06lu\n", (unsigned long)
		wdog_dd->last_pet, nanosec_rem / 1000);
	if (wdog_dd->do_ipi_ping)
		dump_cpu_alive_mask(wdog_dd);
	panic("Apps watchdog bark received!");
	return IRQ_HANDLED;
}

static void configure_bark_dump(struct msm_watchdog_data *wdog_dd)
{
	int ret;
	struct msm_client_dump dump_entry;
	struct {
		unsigned addr;
		int len;
	} cmd_buf;

	wdog_dd->scm_regsave = (void *)__get_free_page(GFP_KERNEL);
	if (wdog_dd->scm_regsave) {
		cmd_buf.addr = virt_to_phys(wdog_dd->scm_regsave);
		cmd_buf.len  = PAGE_SIZE;
		ret = scm_call(SCM_SVC_UTIL, SCM_SET_REGSAVE_CMD,
					&cmd_buf, sizeof(cmd_buf), NULL, 0);
		if (ret)
			pr_err("Setting register save address failed.\n"
				       "Registers won't be dumped on a dog "
				       "bite\n");
		dump_entry.id = MSM_CPU_CTXT;
		dump_entry.start_addr = virt_to_phys(wdog_dd->scm_regsave);
		dump_entry.end_addr = dump_entry.start_addr + PAGE_SIZE;
		ret = msm_dump_table_register(&dump_entry);
		if (ret)
			pr_err("Setting cpu dump region failed\n"
				"Registers wont be dumped during cpu hang\n");
	} else {
		pr_err("Allocating register save space failed\n"
			       "Registers won't be dumped on a dog bite\n");
		/*
		 * No need to bail if allocation fails. Simply don't
		 * send the command, and the secure side will reset
		 * without saving registers.
		 */
	}
}


static void init_watchdog_work(struct work_struct *work)
{
	struct msm_watchdog_data *wdog_dd = container_of(work,
						struct msm_watchdog_data,
							init_dogwork_struct);
	unsigned long delay_time;
	u64 timeout;
	delay_time = msecs_to_jiffies(wdog_dd->pet_time);
	wdog_dd->min_slack_ticks = UINT_MAX;
	wdog_dd->min_slack_ns = ULLONG_MAX;
	configure_bark_dump(wdog_dd);
	timeout = (wdog_dd->bark_time * WDT_HZ)/1000;
	__raw_writel(timeout, wdog_dd->base + WDT0_BARK_TIME);
	__raw_writel(timeout + 3*WDT_HZ, wdog_dd->base + WDT0_BITE_TIME);

	wdog_dd->panic_blk.notifier_call = panic_wdog_handler;
	atomic_notifier_chain_register(&panic_notifier_list,
				       &wdog_dd->panic_blk);
	schedule_delayed_work(&wdog_dd->dogwork_struct, delay_time);

	__raw_writel(1, wdog_dd->base + WDT0_EN);
	__raw_writel(1, wdog_dd->base + WDT0_RST);
	wdog_dd->last_pet = sched_clock();
	printk(KERN_INFO "MSM Watchdog Initialized\n");
	return;
}

static struct of_device_id msm_wdog_match_table[] = {
	{ .compatible = "qcom,msm-watchdog" },
	{}
};

static void __devinit dump_pdata(struct msm_watchdog_data *pdata)
{
	dev_dbg(pdata->dev, "wdog bark_time %d", pdata->bark_time);
	dev_dbg(pdata->dev, "wdog pet_time %d", pdata->pet_time);
	dev_dbg(pdata->dev, "wdog perform ipi ping %d", pdata->do_ipi_ping);
	dev_dbg(pdata->dev, "wdog base address is 0x%x\n", (unsigned int)
								pdata->base);
}

static int __devinit msm_wdog_dt_to_pdata(struct platform_device *pdev,
					struct msm_watchdog_data *pdata)
{
	struct device_node *node = pdev->dev.of_node;
	struct resource *wdog_resource;
	int ret;

	wdog_resource = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pdata->size = resource_size(wdog_resource);
	pdata->phys_base = wdog_resource->start;
	if (unlikely(!(devm_request_region(&pdev->dev, pdata->phys_base,
					pdata->size, "msm-watchdog")))) {
		dev_err(&pdev->dev, "%s cannot reserve watchdog region\n",
								__func__);
		return -ENXIO;
	}
	pdata->base  = devm_ioremap(&pdev->dev, pdata->phys_base,
							pdata->size);
	if (!pdata->base) {
		dev_err(&pdev->dev, "%s cannot map wdog register space\n",
				__func__);
		return -ENXIO;
	}

	pdata->bark_irq = platform_get_irq(pdev, 0);
	pdata->bite_irq = platform_get_irq(pdev, 1);
	ret = of_property_read_u32(node, "qcom,bark-time", &pdata->bark_time);
	if (ret) {
		dev_err(&pdev->dev, "reading bark time failed\n");
		return -ENXIO;
	}
	ret = of_property_read_u32(node, "qcom,pet-time", &pdata->pet_time);
	if (ret) {
		dev_err(&pdev->dev, "reading pet time failed\n");
		return -ENXIO;
	}
	ret = of_property_read_u32(node, "qcom,ipi-ping", &pdata->do_ipi_ping);
	if (ret) {
		dev_err(&pdev->dev, "reading do ipi failed\n");
		return -ENXIO;
	}
	if (!pdata->bark_time) {
		dev_err(&pdev->dev, "%s watchdog bark time not setup\n",
								__func__);
		return -ENXIO;
	}
	if (!pdata->pet_time) {
		dev_err(&pdev->dev, "%s watchdog pet time not setup\n",
								__func__);
		return -ENXIO;
	}
	if (pdata->do_ipi_ping > 1) {
		dev_err(&pdev->dev, "%s invalid watchdog ipi value\n",
								__func__);
		return -ENXIO;
	}
	dump_pdata(pdata);
	return 0;
}

static int __devinit msm_watchdog_probe(struct platform_device *pdev)
{
	int ret;
	struct msm_watchdog_data *wdog_dd;

	if (!pdev->dev.of_node || !enable)
		return -ENODEV;
	wdog_dd = kzalloc(sizeof(struct msm_watchdog_data), GFP_KERNEL);
	if (!wdog_dd)
		return -EIO;
	ret = msm_wdog_dt_to_pdata(pdev, wdog_dd);
	if (ret)
		goto err;
	wdog_dd->dev = &pdev->dev;
	platform_set_drvdata(pdev, wdog_dd);
	ret = devm_request_irq(&pdev->dev, wdog_dd->bark_irq, wdog_bark_handler,
				IRQF_TRIGGER_RISING, "apps_wdog_bark", wdog_dd);
	if (ret) {
		dev_err(&pdev->dev, "failed to request bark irq\n");
		ret = -ENXIO;
		goto err;
	}
	cpumask_clear(&wdog_dd->alive_mask);
	INIT_WORK(&wdog_dd->init_dogwork_struct, init_watchdog_work);
	INIT_DELAYED_WORK(&wdog_dd->dogwork_struct, pet_watchdog_work);
	schedule_work_on(0, &wdog_dd->init_dogwork_struct);
	return 0;
err:
	kzfree(wdog_dd);
	return ret;
}

static const struct dev_pm_ops msm_watchdog_dev_pm_ops = {
	.suspend_noirq = msm_watchdog_suspend,
	.resume_noirq = msm_watchdog_resume,
};

static struct platform_driver msm_watchdog_driver = {
	.probe = msm_watchdog_probe,
	.remove = msm_watchdog_remove,
	.driver = {
		.name = MODULE_NAME,
		.owner = THIS_MODULE,
		.pm = &msm_watchdog_dev_pm_ops,
		.of_match_table = msm_wdog_match_table,
	},
};

static int __devinit init_watchdog(void)
{
	return platform_driver_register(&msm_watchdog_driver);
}

late_initcall(init_watchdog);
MODULE_DESCRIPTION("MSM Watchdog Driver");
MODULE_LICENSE("GPL v2");
