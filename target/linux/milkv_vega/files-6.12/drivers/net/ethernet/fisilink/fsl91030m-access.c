// SPDX-License-Identifier: GPL-2.0-only
/*
 * Low-level MMIO/TDMA access helpers for the FSL91030M switch fabric.
 *
 * Wide switch SRM/table entries are written and read via the SoC DMA engine as
 * 64-bit bursts, not with direct 32-bit writel operations.
 */

#include <linux/build_bug.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/string.h>

#include "fsl91030m.h"

static u32 fsl_table_default_msize(unsigned int count)
{
	return count * sizeof(u32);
}

#define FSL_DMA_XFER_POLL_US	1
#define FSL_DMA_XFER_TIMEOUT_US	10000
#define FSL_DMA_XFER_IRQS \
	(FSL_DMA_FTRANS_IRQ | FSL_DMA_RSP_ERR_IRQ)

static_assert(FSL_TBL_MAXW > 0 &&
	      FSL_TBL_MAXW * sizeof(u32) <= FSL_DMA_SCRATCH_BUF_SIZE,
	      "maximum table transfer must fit DMA scratch buffer");
static_assert(((FSL91030M_SWITCH_WINDOW_SIZE - 1) & FSL_DMA_DSTBASE) == 0,
	      "switch table offsets must not overlap DMA destination base bits");
static_assert(FSL_DMA_DSTBASE <=
	      U32_MAX - FSL91030M_SWITCH_WINDOW_SIZE + 1,
	      "switch DMA destination aperture must fit 32-bit addresses");
static_assert(sizeof(u64) <= FSL_DMA_SCRATCH_ALIGN,
	      "table DMA scratch buffer must be 64-bit aligned");
static_assert(!(FSL_DMA_SCRATCH_BUF_SIZE % FSL_DMA_SCRATCH_ALIGN),
	      "table DMA scratch buffer size must preserve alignment");
static_assert((FSL_DMA_MCTRL_WIDTH_64BIT & ~FSL_DMA_MCTRL_WIDTH_MASK) == 0,
	      "table DMA mctrl width fields must hold the 64-bit selector");
static_assert((FSL_DMA_MCTRL_WIDTH_MASK << FSL_DMA_MCTRL_SRC_WIDTH_SHIFT) ==
	      FSL_DMA_MCTRL_SRC_WIDTH_MASK &&
	      (FSL_DMA_MCTRL_WIDTH_MASK << FSL_DMA_MCTRL_DST_WIDTH_SHIFT) ==
	      FSL_DMA_MCTRL_DST_WIDTH_MASK,
	      "table DMA mctrl masks must match their documented shifts");
static_assert(!(FSL_DMA_MCTRL_64BIT & FSL_DMA_MCTRL_START),
	      "table DMA transfer width must not overlap the start bit");
static_assert(!(FSL_DMA_FTRANS_IRQ & FSL_DMA_RSP_ERR_IRQ),
	      "table DMA status bits must not overlap");
static_assert(FSL_DMA_XFER_POLL_US > 0 &&
	      FSL_DMA_XFER_TIMEOUT_US >= FSL_DMA_XFER_POLL_US,
	      "table DMA polling interval must fit inside timeout");

static bool fsl91030m_access_range_valid(const struct fsl91030m *sw, u32 off,
					 unsigned int words)
{
	resource_size_t bytes;

	if (!sw || !sw->full_size || !words || words > FSL_TBL_MAXW ||
	    off & (sizeof(u32) - 1))
		return false;

	bytes = (resource_size_t)words * sizeof(u32);

	return off <= sw->full_size && bytes <= sw->full_size - off;
}

static int fsl91030m_direct_write(struct fsl91030m *sw, u32 off,
				  const u32 *words, unsigned int count)
{
	unsigned int i;

	if (!sw || !sw->dev || !sw->full)
		return -ENODEV;
	if (!words || !fsl91030m_access_range_valid(sw, off, count))
		return -EINVAL;

	for (i = 0; i < count; i++)
		writel(words[i], sw->full + off + i * sizeof(u32));

	return 0;
}

int fsl91030m_direct_write32(struct fsl91030m *sw, u32 off, u32 val)
{
	return fsl91030m_direct_write(sw, off, &val, 1);
}

static int fsl91030m_direct_read(struct fsl91030m *sw, u32 off, u32 *words,
				 unsigned int count)
{
	unsigned int i;

	if (!sw || !sw->dev || !sw->full)
		return -ENODEV;
	if (!words || !fsl91030m_access_range_valid(sw, off, count))
		return -EINVAL;

	for (i = 0; i < count; i++)
		words[i] = readl(sw->full + off + i * sizeof(u32));

	return 0;
}

int fsl91030m_direct_read32(struct fsl91030m *sw, u32 off, u32 *val)
{
	return fsl91030m_direct_read(sw, off, val, 1);
}

static int fsl_dma_xfer(struct fsl91030m *sw, u32 src, u32 dst, u32 msize,
			u32 mctrl_width)
{
	u32 pre_clear_stat, pre_en, pre_stat, post_stat, post_ctrl, clr_stat;
	int clear_ret;
	int ret;

	pre_en = readl(sw->dma_regs + FSL_DMA_IRQ_EN);
	pre_stat = readl(sw->dma_regs + FSL_DMA_IRQ_STAT);
	writel(FSL_DMA_XFER_IRQS, sw->dma_regs + FSL_DMA_IRQ_CLR);
	ret = readl_poll_timeout(sw->dma_regs + FSL_DMA_IRQ_STAT,
				 pre_clear_stat,
				 !(pre_clear_stat & FSL_DMA_XFER_IRQS),
				 FSL_DMA_XFER_POLL_US,
				 FSL_DMA_XFER_TIMEOUT_US);
	if (ret) {
		dev_err_ratelimited(sw->dev,
				    "table DMA status clear timed out pre_stat=%#x clear_stat=%#x\n",
				    pre_stat, pre_clear_stat);
		return ret;
	}

	writel(src, sw->dma_regs + FSL_DMA_MSRCADDR);
	writel(dst, sw->dma_regs + FSL_DMA_MDSTADDR);
	writel(msize, sw->dma_regs + FSL_DMA_MSIZE);
	writel(mctrl_width, sw->dma_regs + FSL_DMA_MCTRL);
	writel(pre_en | FSL_DMA_XFER_IRQS, sw->dma_regs + FSL_DMA_IRQ_EN);
	writel(mctrl_width | FSL_DMA_MCTRL_START, sw->dma_regs + FSL_DMA_MCTRL);
	ret = readl_poll_timeout(sw->dma_regs + FSL_DMA_IRQ_STAT, post_stat,
				 post_stat & FSL_DMA_XFER_IRQS,
				 FSL_DMA_XFER_POLL_US,
				 FSL_DMA_XFER_TIMEOUT_US);
	post_ctrl = readl(sw->dma_regs + FSL_DMA_MCTRL);
	if (!ret && (post_stat & FSL_DMA_RSP_ERR_IRQ))
		ret = -EIO;

	writel(FSL_DMA_XFER_IRQS, sw->dma_regs + FSL_DMA_IRQ_CLR);
	clear_ret = readl_poll_timeout(sw->dma_regs + FSL_DMA_IRQ_STAT,
				       clr_stat,
				       !(clr_stat & FSL_DMA_XFER_IRQS),
				       FSL_DMA_XFER_POLL_US,
				       FSL_DMA_XFER_TIMEOUT_US);
	writel(pre_en, sw->dma_regs + FSL_DMA_IRQ_EN);
	if (ret || clear_ret)
		dev_err_ratelimited(sw->dev,
				    "table DMA failed src=%#x dst=%#x msize=%u ret=%d clear_ret=%d pre_stat=%#x pre_clear=%#x post_stat=%#x post_ctrl=%#x clr_stat=%#x\n",
				    src, dst, msize, ret, clear_ret, pre_stat,
				    pre_clear_stat, post_stat, post_ctrl, clr_stat);
	return ret ?: clear_ret;
}

static int fsl91030m_table_write(struct fsl91030m *sw, u32 off,
				 const u32 *words, unsigned int count)
{
	u32 msize;
	unsigned int i;
	int ret;

	if (!sw || !sw->dev || !sw->dma_regs || !sw->dma_vbuf)
		return -ENODEV;
	if (!words || !fsl91030m_access_range_valid(sw, off, count))
		return -EINVAL;
	mutex_lock(&sw->dma_lock);
	for (i = 0; i < count; i++)
		writel(words[i], sw->dma_vbuf + i * sizeof(u32));
	/* Ensure the scratch buffer is visible before starting table DMA. */
	wmb();
	msize = fsl_table_default_msize(count);
	ret = fsl_dma_xfer(sw, sw->dma_vbuf_phys, off | FSL_DMA_DSTBASE,
			   msize, FSL_DMA_MCTRL_64BIT);
	mutex_unlock(&sw->dma_lock);

	return ret;
}

int fsl91030m_table_read(struct fsl91030m *sw, u32 off, u32 *words,
			 unsigned int count)
{
	u32 msize;
	unsigned int i;
	int ret;

	if (!sw || !sw->dev || !sw->dma_regs || !sw->dma_vbuf)
		return -ENODEV;
	if (!words || !fsl91030m_access_range_valid(sw, off, count))
		return -EINVAL;
	mutex_lock(&sw->dma_lock);
	msize = fsl_table_default_msize(count);
	ret = fsl_dma_xfer(sw, off | FSL_DMA_DSTBASE, sw->dma_vbuf_phys,
			   msize, FSL_DMA_MCTRL_64BIT);
	if (ret) {
		mutex_unlock(&sw->dma_lock);
		return ret;
	}
	/* Ensure table DMA writes are visible before reading the scratch buffer. */
	rmb();
	for (i = 0; i < count; i++)
		words[i] = readl(sw->dma_vbuf + i * sizeof(u32));
	mutex_unlock(&sw->dma_lock);
	return 0;
}

int fsl91030m_table_write_verify_exact(struct fsl91030m *sw, u32 off,
				       const u32 *want, u32 *after,
				       unsigned int count)
{
	int ret;

	if (!want || !after || !count || count > FSL_TBL_MAXW)
		return -EINVAL;

	ret = fsl91030m_table_write(sw, off, want, count);
	if (ret)
		return ret;

	ret = fsl91030m_table_read(sw, off, after, count);
	if (ret)
		return ret;

	return memcmp(after, want, count * sizeof(u32)) ? -EIO : 0;
}

int fsl91030m_direct_write_verify_exact(struct fsl91030m *sw, u32 off,
					const u32 *want, u32 *after,
					unsigned int count)
{
	int ret;

	if (!want || !after || !count || count > FSL_TBL_MAXW)
		return -EINVAL;

	ret = fsl91030m_direct_write(sw, off, want, count);
	if (ret)
		return ret;

	ret = fsl91030m_direct_read(sw, off, after, count);
	if (ret)
		return ret;

	return memcmp(after, want, count * sizeof(u32)) ? -EIO : 0;
}
