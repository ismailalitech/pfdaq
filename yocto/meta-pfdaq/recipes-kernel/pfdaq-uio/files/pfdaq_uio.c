// SPDX-License-Identifier: GPL-2.0
/*
 * pfdaq_uio - UIO driver for the pfdaq fabric block (stream source + latency unit) on the
 * PolarFire SoC Discovery Kit. Project code.
 *
 * It exposes the same UIO device as uio_pdrv_genirq ("pfdaq_stream_src", one memory map,
 * the latency channel 0 interrupt), so pfdaq_cap and pfdaq_lat work unchanged. The difference
 * is the interrupt handler:
 *   kernel_ack=1 (default)  the handler acknowledges latency channel 0 itself, so the fabric
 *                           measures interrupt -> kernel handler latency
 *   kernel_ack=0            generic UIO behaviour (mask, notify, user space acknowledges)
 *
 * It is not bound by default. To use it for a measurement:
 *   echo 40000f00.uio > /sys/bus/platform/drivers/uio_pdrv_genirq/unbind
 *   echo pfdaq_uio    > /sys/bus/platform/devices/40000f00.uio/driver_override
 *   echo 40000f00.uio > /sys/bus/platform/drivers/pfdaq_uio/bind
 */
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/uio_driver.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/io.h>

#define LAT0_ACK     0x8C   /* offsets from 0x40000F00 */
#define LAT0_STATUS  0x90

static bool kernel_ack = true;
module_param(kernel_ack, bool, 0644);
MODULE_PARM_DESC(kernel_ack, "acknowledge latency channel 0 in the interrupt handler");

static unsigned long kernel_acks;
module_param(kernel_acks, ulong, 0444);
MODULE_PARM_DESC(kernel_acks, "interrupts acknowledged by the kernel handler");

struct pfdaq_uio {
	struct uio_info info;
	void __iomem *base;
	spinlock_t lock;
	unsigned long masked;
};

static irqreturn_t pfdaq_uio_handler(int irq, struct uio_info *info)
{
	struct pfdaq_uio *p = info->priv;

	if (kernel_ack) {
		if (!(readl(p->base + LAT0_STATUS) & 1))
			return IRQ_NONE;
		writel(1, p->base + LAT0_ACK);
		kernel_acks++;
		return IRQ_HANDLED;          /* the UIO core counts it as an event */
	}
	spin_lock(&p->lock);
	if (!__test_and_set_bit(0, &p->masked))
		disable_irq_nosync(irq);
	spin_unlock(&p->lock);
	return IRQ_HANDLED;
}

static int pfdaq_uio_irqcontrol(struct uio_info *info, s32 on)
{
	struct pfdaq_uio *p = info->priv;
	unsigned long flags;

	spin_lock_irqsave(&p->lock, flags);
	if (on) {
		if (__test_and_clear_bit(0, &p->masked))
			enable_irq(info->irq);
	} else {
		if (!__test_and_set_bit(0, &p->masked))
			disable_irq_nosync(info->irq);
	}
	spin_unlock_irqrestore(&p->lock, flags);
	return 0;
}

static int pfdaq_uio_probe(struct platform_device *pdev)
{
	struct pfdaq_uio *p;
	struct resource *r;
	int irq;

	p = devm_kzalloc(&pdev->dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;
	r = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!r)
		return -ENODEV;
	/* plain ioremap: the region is not requested, exactly like uio_pdrv_genirq */
	p->base = devm_ioremap(&pdev->dev, r->start, resource_size(r));
	if (!p->base)
		return -ENOMEM;
	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	spin_lock_init(&p->lock);
	p->info.name = "pfdaq_stream_src";
	p->info.version = "pfdaq_uio 1.0";
	p->info.mem[0].name = "registers";
	/* the UIO core only maps page-aligned regions: map the page and record the offset,
	   exactly as uio_pdrv_genirq does (the tools add 0xF00 themselves) */
	p->info.mem[0].addr = r->start & PAGE_MASK;
	p->info.mem[0].offs = r->start & ~PAGE_MASK;
	p->info.mem[0].size = PAGE_ALIGN(p->info.mem[0].offs + resource_size(r));
	p->info.mem[0].memtype = UIO_MEM_PHYS;
	p->info.irq = irq;
	p->info.handler = pfdaq_uio_handler;
	p->info.irqcontrol = pfdaq_uio_irqcontrol;
	p->info.priv = p;

	/* drop an interrupt left pending by an earlier user */
	if (readl(p->base + LAT0_STATUS) & 1)
		writel(1, p->base + LAT0_ACK);

	platform_set_drvdata(pdev, p);
	return devm_uio_register_device(&pdev->dev, &p->info);
}

static struct platform_driver pfdaq_uio_driver = {
	.probe = pfdaq_uio_probe,
	.driver = { .name = "pfdaq_uio" },
};
module_platform_driver(pfdaq_uio_driver);

MODULE_DESCRIPTION("pfdaq fabric block UIO driver with in-kernel latency acknowledge");
MODULE_LICENSE("GPL");
