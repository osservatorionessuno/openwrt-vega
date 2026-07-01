// SPDX-License-Identifier: GPL-2.0-only
/*
 * FisiLink FSL91030M L2 Ethernet switch support for the Milk-V Vega RJ45 G5..G12
 * datapath.  The Linux control surface is bridge/switchdev.
 */
#include <linux/align.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/ioport.h>
#include <linux/limits.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>

#include "fsl91030m.h"

#define FSL91030M_DMA_REG_WINDOW_SIZE	(FSL_DMA_IRQ_CLR + sizeof(u32))

static_assert(FSL_DMA_SCRATCH_BUF_SIZE > 0,
	      "switch DMA scratch size must be nonzero");
static_assert(FSL_DMA_SCRATCH_BUF_SIZE <= U32_MAX,
	      "switch DMA scratch size must fit 32-bit address registers");
static_assert(FSL91030M_DMA_REG_WINDOW_SIZE <= 0x1000,
	      "switch DMA registers must fit the documented window");
static_assert(FSL_DMA_SCRATCH_ALIGN &&
	      !(FSL_DMA_SCRATCH_ALIGN & (FSL_DMA_SCRATCH_ALIGN - 1)),
	      "switch DMA scratch alignment must be a power of two");

static bool fsl91030m_dma_scratch_fits_addr_regs(const struct resource *res)
{
	/*
	 * The table-DMA engine takes 32-bit physical addresses.  Require the
	 * complete scratch buffer, not just its base address, to be representable
	 * before narrowing the DT resource start to the DMA address register.
	 */
	return resource_size(res) >= FSL_DMA_SCRATCH_BUF_SIZE &&
	       res->start <= (resource_size_t)U32_MAX -
			     FSL_DMA_SCRATCH_BUF_SIZE + 1;
}

static int fsl91030m_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *dma_scratch;
	struct resource *dma_res;
	struct resource *switch_res;
	struct fsl91030m *sw;
	int ret;

	sw = devm_kzalloc(dev, sizeof(*sw), GFP_KERNEL);
	if (!sw)
		return -ENOMEM;
	sw->dev = dev;
	mutex_init(&sw->dma_lock);
	mutex_init(&sw->op_lock);
	mutex_init(&sw->age_lock);

	switch_res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						  "switch");
	if (!switch_res)
		return dev_err_probe(dev, -EINVAL,
				     "missing switch resource\n");
	if (resource_size(switch_res) < FSL91030M_SWITCH_WINDOW_SIZE)
		return dev_err_probe(dev, -EINVAL,
				     "switch resource is too small\n");
	if (switch_res->start != FSL_DMA_DSTBASE)
		return dev_err_probe(dev, -EINVAL,
				     "unexpected switch resource base\n");

	sw->full_size = FSL91030M_SWITCH_WINDOW_SIZE;
	sw->switch_phys = switch_res->start;
	sw->full = devm_ioremap_resource(dev, switch_res);
	if (IS_ERR(sw->full))
		return PTR_ERR(sw->full);

	/*
	 * SoC DMA engine for SRM/table access. Without it the switchdev
	 * operations cannot be programmed or read back.
	 */
	dma_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dma");
	if (!dma_res)
		return dev_err_probe(dev, -EINVAL,
				     "missing DMA register resource\n");
	if (resource_size(dma_res) < FSL91030M_DMA_REG_WINDOW_SIZE)
		return dev_err_probe(dev, -EINVAL,
				     "DMA register resource is too small\n");

	sw->dma_regs = devm_ioremap_resource(dev, dma_res);
	if (IS_ERR(sw->dma_regs))
		return PTR_ERR(sw->dma_regs);

	dma_scratch = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						   "dma-scratch");
	if (!dma_scratch)
		return dev_err_probe(dev, -EINVAL,
				     "missing DMA scratch resource\n");
	if (!fsl91030m_dma_scratch_fits_addr_regs(dma_scratch))
		return dev_err_probe(dev, -EINVAL,
				     "invalid DMA scratch resource\n");
	if (!IS_ALIGNED(dma_scratch->start, FSL_DMA_SCRATCH_ALIGN))
		return dev_err_probe(dev, -EINVAL,
				     "unaligned DMA scratch resource\n");

	sw->dma_vbuf = devm_ioremap_resource(dev, dma_scratch);
	if (IS_ERR(sw->dma_vbuf))
		return PTR_ERR(sw->dma_vbuf);
	sw->dma_vbuf_phys = (u32)dma_scratch->start;

	ret = fsl91030m_board_init_apply(sw);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to apply board init\n");

	ret = fsl91030m_switchdev_register(sw);
	if (ret) {
		dev_err(dev, "switchdev control plane failed ret=%d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, sw);

	return 0;
}

static void fsl91030m_remove(struct platform_device *pdev)
{
	struct fsl91030m *sw = platform_get_drvdata(pdev);

	fsl91030m_switchdev_unregister(sw);
	platform_set_drvdata(pdev, NULL);
	/* devm cleans up the mappings. */
}

static const struct of_device_id fsl91030m_of_match[] = {
	{ .compatible = "milkv,vega-fsl91030m-switch" },
	{ }
};
MODULE_DEVICE_TABLE(of, fsl91030m_of_match);

static struct platform_driver fsl91030m_driver = {
	.probe	= fsl91030m_probe,
	.remove	= fsl91030m_remove,
	.driver	= {
		.name			= "fsl91030m-switch",
		.of_match_table		= fsl91030m_of_match,
		.suppress_bind_attrs	= true,
	},
};

module_platform_driver(fsl91030m_driver);

MODULE_DESCRIPTION("FisiLink FSL91030M L2 switch driver (Milk-V Vega)");
MODULE_LICENSE("GPL");
