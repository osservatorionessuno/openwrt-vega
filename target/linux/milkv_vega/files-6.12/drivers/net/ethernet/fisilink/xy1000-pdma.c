// SPDX-License-Identifier: GPL-2.0-only
/*
 * FisiLink XY1000 packet-DMA host interface for Milk-V Vega.
 *
 * This driver owns only the CPU packet-DMA register window, interrupt lines,
 * fixed packet scratch window, and the 8-byte private packet header. Switch
 * fabric admission and egress policy deliberately remain in the FSL91030M
 * fabric driver.
 */
#include <linux/align.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/platform_device.h>
#include <linux/rcupdate.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>

#include "xy1000-pdma.h"

#define XY1000_DMA_RX_ADDR		0x00
#define XY1000_DMA_RX_STATE		0x04
#define XY1000_DMA_RX_START		0x08
#define XY1000_DMA_TX_ADDR		0x0c
#define XY1000_DMA_TX_CFG		0x10
#define XY1000_DMA_TX_START		0x14
#define XY1000_DMA_AXI_WR_CFG		0x1c
#define XY1000_DMA_AXI_RD_CFG		0x20
#define XY1000_DMA_STATUS		0x34
#define XY1000_DMA_INT_MASK		0x3c

#define XY1000_IRQ_MASK_RX_REQ		BIT(0)
#define XY1000_IRQ_MASK_RX_END		BIT(1)
#define XY1000_IRQ_MASK_TX_END		BIT(2)
#define XY1000_IRQ_MASK_ALL		(XY1000_IRQ_MASK_RX_REQ | \
					 XY1000_IRQ_MASK_RX_END | \
					 XY1000_IRQ_MASK_TX_END)

#define XY1000_STATUS_RX_REQ		BIT(1)

#define XY1000_AXI_CFG_VENDOR		0x1000f
#define XY1000_BUF_SIZE		0x4000
#define XY1000_BUF_TOTAL_SIZE		(2 * XY1000_BUF_SIZE)
#define XY1000_HEADER_LEN		8
#define XY1000_TRAILER_LEN		8
#define XY1000_DMA_PORT		1
#define XY1000_DEFAULT_HOST_LPORT	0
#define XY1000_MAX_WIRE_LEN		16375
#define XY1000_TX_TIMEOUT		(5 * HZ)

static_assert(XY1000_BUF_TOTAL_SIZE <= U32_MAX,
	      "packet-DMA buffer window must fit 32-bit address registers");

static const u8 xy1000_default_mac[ETH_ALEN] = {
	0x00, 0x02, 0xa3, 0xf2, 0x10, 0x00,
};

static const u8 xy1000_header_magic[] = {
	0xc7, 0x04, 0xdd, 0x7b,
};

struct xy1000_pdma {
	struct device *dev;
	struct net_device *ndev;
	u8 __iomem *regs;
	u8 __iomem *rx_buf;
	u8 __iomem *tx_buf;
	u32 rx_phys;
	u32 tx_phys;
	u8 *tx_bounce;
	unsigned int tx_len;
	struct napi_struct napi;
	spinlock_t state_lock;	/* protects running/rx_active/tx_busy and MMIO starts */
	spinlock_t stats_lock;	/* protects software netdev counters */
	struct rtnl_link_stats64 stats;
	struct net_device __rcu *lport_ndev[XY1000_PDMA_MAX_LPORT + 1];
	unsigned int users;
	bool running;
	bool rx_active;
	bool tx_busy;
};

static DEFINE_MUTEX(xy1000_pdma_registry_lock);
static struct xy1000_pdma __rcu *xy1000_pdma_instance;

static u32 xy1000_pdma_read(struct xy1000_pdma *pdma, u32 off)
{
	return readl(pdma->regs + off);
}

static void xy1000_pdma_write(struct xy1000_pdma *pdma, u32 off, u32 val)
{
	writel(val, pdma->regs + off);
}

static void xy1000_pdma_irq_mask(struct xy1000_pdma *pdma, u32 mask)
{
	xy1000_pdma_write(pdma, XY1000_DMA_INT_MASK, mask);
}

static void xy1000_pdma_stats_rx(struct xy1000_pdma *pdma, unsigned int len)
{
	unsigned long flags;

	spin_lock_irqsave(&pdma->stats_lock, flags);
	pdma->stats.rx_packets++;
	pdma->stats.rx_bytes += len;
	spin_unlock_irqrestore(&pdma->stats_lock, flags);
}

static void xy1000_pdma_stats_rx_drop(struct xy1000_pdma *pdma, bool length_err)
{
	unsigned long flags;

	spin_lock_irqsave(&pdma->stats_lock, flags);
	pdma->stats.rx_errors++;
	pdma->stats.rx_dropped++;
	if (length_err)
		pdma->stats.rx_length_errors++;
	spin_unlock_irqrestore(&pdma->stats_lock, flags);
}

static void xy1000_pdma_stats_rx_nomem(struct xy1000_pdma *pdma)
{
	unsigned long flags;

	spin_lock_irqsave(&pdma->stats_lock, flags);
	pdma->stats.rx_dropped++;
	pdma->stats.rx_missed_errors++;
	spin_unlock_irqrestore(&pdma->stats_lock, flags);
}

static void xy1000_pdma_stats_tx(struct xy1000_pdma *pdma, unsigned int len)
{
	unsigned long flags;

	spin_lock_irqsave(&pdma->stats_lock, flags);
	pdma->stats.tx_packets++;
	pdma->stats.tx_bytes += len;
	spin_unlock_irqrestore(&pdma->stats_lock, flags);
}

static void xy1000_pdma_stats_tx_drop(struct xy1000_pdma *pdma)
{
	unsigned long flags;

	spin_lock_irqsave(&pdma->stats_lock, flags);
	pdma->stats.tx_errors++;
	pdma->stats.tx_dropped++;
	spin_unlock_irqrestore(&pdma->stats_lock, flags);
}

static void xy1000_pdma_wake_queues(struct xy1000_pdma *pdma)
{
	unsigned int lport;

	netif_wake_queue(pdma->ndev);

	rcu_read_lock();
	for (lport = 0; lport <= XY1000_PDMA_MAX_LPORT; lport++) {
		struct net_device *ndev;

		ndev = rcu_dereference(pdma->lport_ndev[lport]);
		if (ndev)
			netif_wake_queue(ndev);
	}
	rcu_read_unlock();
}

static void xy1000_pdma_start_rx_locked(struct xy1000_pdma *pdma)
{
	pdma->rx_active = true;
	xy1000_pdma_irq_mask(pdma, XY1000_IRQ_MASK_RX_REQ);
	xy1000_pdma_write(pdma, XY1000_DMA_RX_ADDR, pdma->rx_phys);
	xy1000_pdma_write(pdma, XY1000_DMA_RX_START, 1);
}

static void xy1000_pdma_hw_start_locked(struct xy1000_pdma *pdma)
{
	pdma->running = true;
	pdma->rx_active = false;
	pdma->tx_busy = false;
	pdma->tx_len = 0;
	xy1000_pdma_write(pdma, XY1000_DMA_AXI_WR_CFG, XY1000_AXI_CFG_VENDOR);
	xy1000_pdma_write(pdma, XY1000_DMA_AXI_RD_CFG, XY1000_AXI_CFG_VENDOR);
	if (xy1000_pdma_read(pdma, XY1000_DMA_STATUS) & XY1000_STATUS_RX_REQ)
		xy1000_pdma_start_rx_locked(pdma);
	else
		xy1000_pdma_irq_mask(pdma, XY1000_IRQ_MASK_RX_END);
}

static void xy1000_pdma_hw_stop_locked(struct xy1000_pdma *pdma)
{
	pdma->running = false;
	pdma->rx_active = false;
	pdma->tx_busy = false;
	pdma->tx_len = 0;
	xy1000_pdma_irq_mask(pdma, XY1000_IRQ_MASK_ALL);
}

static void xy1000_pdma_hw_get(struct xy1000_pdma *pdma)
{
	unsigned long flags;

	spin_lock_irqsave(&pdma->state_lock, flags);
	if (!pdma->users)
		xy1000_pdma_hw_start_locked(pdma);
	pdma->users++;
	spin_unlock_irqrestore(&pdma->state_lock, flags);
}

static void xy1000_pdma_hw_put(struct xy1000_pdma *pdma)
{
	unsigned long flags;

	spin_lock_irqsave(&pdma->state_lock, flags);
	if (pdma->users && !--pdma->users)
		xy1000_pdma_hw_stop_locked(pdma);
	spin_unlock_irqrestore(&pdma->state_lock, flags);
}

static void xy1000_pdma_hw_stop(struct xy1000_pdma *pdma)
{
	unsigned long flags;

	spin_lock_irqsave(&pdma->state_lock, flags);
	pdma->users = 0;
	xy1000_pdma_hw_stop_locked(pdma);
	spin_unlock_irqrestore(&pdma->state_lock, flags);
}

static void xy1000_pdma_rx_complete_rearm(struct xy1000_pdma *pdma)
{
	unsigned long flags;

	spin_lock_irqsave(&pdma->state_lock, flags);
	if (!pdma->running) {
		xy1000_pdma_irq_mask(pdma, XY1000_IRQ_MASK_ALL);
	} else if (xy1000_pdma_read(pdma, XY1000_DMA_STATUS) &
		   XY1000_STATUS_RX_REQ) {
		xy1000_pdma_start_rx_locked(pdma);
	} else {
		xy1000_pdma_irq_mask(pdma, XY1000_IRQ_MASK_RX_END);
	}
	spin_unlock_irqrestore(&pdma->state_lock, flags);
}

static bool xy1000_pdma_header_valid(const u8 *hdr)
{
	return !memcmp(hdr, xy1000_header_magic, sizeof(xy1000_header_magic));
}

static unsigned int xy1000_pdma_header_wire_len(const u8 *hdr)
{
	return ((unsigned int)(hdr[6] & 0x3f) << 8) | hdr[7];
}

static unsigned int xy1000_pdma_header_lport(const u8 *hdr)
{
	return (hdr[6] >> 6) | ((hdr[5] & 0x0f) << 2);
}

static struct net_device *
xy1000_pdma_rx_lport_ndev(struct xy1000_pdma *pdma, unsigned int lport)
{
	if (lport > XY1000_PDMA_MAX_LPORT)
		return NULL;

	return rcu_dereference(pdma->lport_ndev[lport]);
}

/* Returns true if a frame was delivered to the stack, false otherwise. */
static bool xy1000_pdma_rx_one(struct xy1000_pdma *pdma)
{
	struct net_device *ndev = pdma->ndev;
	struct net_device *rx_ndev;
	unsigned int state_words;
	unsigned int state_port;
	unsigned int expected_words;
	unsigned int frame_len;
	unsigned int wire_len;
	unsigned int lport;
	struct sk_buff *skb;
	u32 state;
	u8 hdr[XY1000_HEADER_LEN];

	state = xy1000_pdma_read(pdma, XY1000_DMA_RX_STATE);
	state_words = state & 0xfff;
	state_port = (state >> 12) & 0x1;

	memcpy_fromio(hdr, pdma->rx_buf, sizeof(hdr));

	if (!xy1000_pdma_header_valid(hdr)) {
		netdev_dbg(ndev, "dropping RX packet with invalid private header\n");
		xy1000_pdma_stats_rx_drop(pdma, false);
		return false;
	}

	wire_len = xy1000_pdma_header_wire_len(hdr);
	if (wire_len < ETH_ZLEN + ETH_FCS_LEN ||
	    wire_len > XY1000_MAX_WIRE_LEN ||
	    ALIGN(wire_len, 8) + XY1000_HEADER_LEN + XY1000_TRAILER_LEN >
	    XY1000_BUF_SIZE) {
		netdev_dbg(ndev, "dropping RX packet with invalid length %u\n",
			   wire_len);
		xy1000_pdma_stats_rx_drop(pdma, true);
		return false;
	}

	expected_words = (ALIGN(wire_len, 8) + XY1000_HEADER_LEN +
			  XY1000_TRAILER_LEN) / 8;
	if (state_words && state_words != expected_words)
		netdev_dbg(ndev,
			   "RX state length mismatch state=%u expected=%u dma_port=%u lport=%u\n",
			   state_words, expected_words, state_port,
			   xy1000_pdma_header_lport(hdr));

	frame_len = wire_len - ETH_FCS_LEN;
	if (frame_len < ETH_HLEN ||
	    frame_len > ndev->mtu + VLAN_ETH_HLEN) {
		netdev_dbg(ndev, "dropping RX Ethernet frame length %u\n",
			   frame_len);
		xy1000_pdma_stats_rx_drop(pdma, true);
		return false;
	}

	skb = netdev_alloc_skb_ip_align(ndev, frame_len);
	if (!skb) {
		xy1000_pdma_stats_rx_nomem(pdma);
		return false;
	}

	memcpy_fromio(skb_put(skb, frame_len),
		      pdma->rx_buf + XY1000_HEADER_LEN, frame_len);
	lport = xy1000_pdma_header_lport(hdr);

	rcu_read_lock();
	rx_ndev = xy1000_pdma_rx_lport_ndev(pdma, lport);
	if (!rx_ndev)
		rx_ndev = ndev;
	skb->dev = rx_ndev;
	skb->protocol = eth_type_trans(skb, rx_ndev);
	rcu_read_unlock();

	xy1000_pdma_stats_rx(pdma, frame_len);
	napi_gro_receive(&pdma->napi, skb);

	return true;
}

static int xy1000_pdma_poll(struct napi_struct *napi, int budget)
{
	struct xy1000_pdma *pdma = container_of(napi, struct xy1000_pdma, napi);
	int work_done = 0;

	/* budget == 0 is the netpoll path: do not consume or re-arm RX. */
	if (budget <= 0)
		return 0;

	if (xy1000_pdma_rx_one(pdma))
		work_done = 1;

	/*
	 * The engine holds at most one frame per rx-end completion, so once it
	 * is consumed there is no more work this round.  Complete and re-arm
	 * unconditionally rather than gating on work_done < budget: that idiom
	 * (for multi-descriptor NICs) would skip the re-arm when budget == 1
	 * and leave RX masked while NAPI re-polled the already-consumed buffer.
	 * For every real caller budget is >= 8, so work_done (0 or 1) is always
	 * below budget and the NAPI contract is honoured.
	 */
	if (napi_complete_done(napi, work_done))
		xy1000_pdma_rx_complete_rearm(pdma);

	return work_done;
}

static irqreturn_t xy1000_pdma_irq_tx_end(int irq, void *data)
{
	struct xy1000_pdma *pdma = data;
	unsigned int tx_len = 0;
	bool wake = false;
	unsigned long flags;

	spin_lock_irqsave(&pdma->state_lock, flags);
	if (pdma->running && pdma->tx_busy) {
		tx_len = pdma->tx_len;
		pdma->tx_len = 0;
		pdma->tx_busy = false;
		wake = true;
	}
	spin_unlock_irqrestore(&pdma->state_lock, flags);

	if (wake) {
		xy1000_pdma_stats_tx(pdma, tx_len);
		xy1000_pdma_wake_queues(pdma);
	}

	return IRQ_HANDLED;
}

static irqreturn_t xy1000_pdma_irq_rx_req(int irq, void *data)
{
	struct xy1000_pdma *pdma = data;
	unsigned long flags;

	spin_lock_irqsave(&pdma->state_lock, flags);
	if (pdma->running && !pdma->rx_active)
		xy1000_pdma_start_rx_locked(pdma);
	spin_unlock_irqrestore(&pdma->state_lock, flags);

	return IRQ_HANDLED;
}

static irqreturn_t xy1000_pdma_irq_rx_end(int irq, void *data)
{
	struct xy1000_pdma *pdma = data;
	bool schedule = false;
	unsigned long flags;

	spin_lock_irqsave(&pdma->state_lock, flags);
	if (pdma->running && pdma->rx_active) {
		pdma->rx_active = false;
		xy1000_pdma_irq_mask(pdma, XY1000_IRQ_MASK_RX_REQ |
				      XY1000_IRQ_MASK_RX_END);
		schedule = napi_schedule_prep(&pdma->napi);
	}
	spin_unlock_irqrestore(&pdma->state_lock, flags);

	if (schedule)
		__napi_schedule(&pdma->napi);

	return IRQ_HANDLED;
}

static int xy1000_pdma_open(struct net_device *ndev)
{
	struct xy1000_pdma *pdma = netdev_priv(ndev);

	xy1000_pdma_hw_get(pdma);
	netif_carrier_on(ndev);
	netif_start_queue(ndev);

	return 0;
}

static int xy1000_pdma_stop(struct net_device *ndev)
{
	struct xy1000_pdma *pdma = netdev_priv(ndev);

	netif_stop_queue(ndev);
	netif_carrier_off(ndev);
	xy1000_pdma_hw_put(pdma);

	return 0;
}

static void xy1000_pdma_build_tx_header(u8 *hdr, unsigned int wire_len,
					u8 lport)
{
	memset(hdr, 0, XY1000_HEADER_LEN);
	memcpy(hdr, xy1000_header_magic, sizeof(xy1000_header_magic));
	hdr[4] = 0x00;
	hdr[5] = 0x20;

	if (wire_len > ETH_ZLEN + ETH_FCS_LEN) {
		hdr[6] = (wire_len >> 8) & 0x3f;
		hdr[7] = wire_len & 0xff;
	} else {
		hdr[6] = 0;
		hdr[7] = ETH_ZLEN + ETH_FCS_LEN;
	}

	hdr[5] = (hdr[5] & 0xf0) |
		 ((lport & 0x3c) >> 2) | 0x10;
	hdr[6] = (hdr[6] & 0x3f) |
		 ((lport & 0x03) << 6);
}

static netdev_tx_t xy1000_pdma_xmit(struct xy1000_pdma *pdma,
				    struct sk_buff *skb,
				    struct net_device *queue_ndev,
				    u8 lport, bool drop_when_stopped)
{
	unsigned int wire_len = max_t(unsigned int, skb->len + ETH_FCS_LEN,
				      ETH_ZLEN + ETH_FCS_LEN);
	unsigned int aligned_wire_len = ALIGN(wire_len, 8);
	unsigned int total_len = aligned_wire_len + XY1000_HEADER_LEN;
	unsigned long flags;
	int ret;

	if (unlikely(skb->len < ETH_HLEN ||
		     skb->len > queue_ndev->mtu + VLAN_ETH_HLEN ||
		     wire_len > XY1000_MAX_WIRE_LEN ||
		     total_len > XY1000_BUF_SIZE ||
		     lport > XY1000_PDMA_MAX_LPORT)) {
		xy1000_pdma_stats_tx_drop(pdma);
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	spin_lock_irqsave(&pdma->state_lock, flags);
	if (unlikely(!pdma->running)) {
		if (drop_when_stopped) {
			spin_unlock_irqrestore(&pdma->state_lock, flags);
			dev_kfree_skb_any(skb);
			return NETDEV_TX_OK;
		}
		netif_stop_queue(queue_ndev);
		netif_stop_queue(pdma->ndev);
		spin_unlock_irqrestore(&pdma->state_lock, flags);
		return NETDEV_TX_BUSY;
	}
	if (unlikely(pdma->tx_busy)) {
		netif_stop_queue(queue_ndev);
		netif_stop_queue(pdma->ndev);
		spin_unlock_irqrestore(&pdma->state_lock, flags);
		return NETDEV_TX_BUSY;
	}

	pdma->tx_busy = true;
	pdma->tx_len = skb->len;
	netif_stop_queue(queue_ndev);
	netif_stop_queue(pdma->ndev);

	memset(pdma->tx_bounce, 0, total_len);
	xy1000_pdma_build_tx_header(pdma->tx_bounce, wire_len, lport);
	ret = skb_copy_bits(skb, 0, pdma->tx_bounce + XY1000_HEADER_LEN,
			    skb->len);
	if (unlikely(ret)) {
		pdma->tx_busy = false;
		pdma->tx_len = 0;
		netif_wake_queue(queue_ndev);
		netif_wake_queue(pdma->ndev);
		spin_unlock_irqrestore(&pdma->state_lock, flags);
		xy1000_pdma_stats_tx_drop(pdma);
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	memcpy_toio(pdma->tx_buf, pdma->tx_bounce, total_len);
	xy1000_pdma_write(pdma, XY1000_DMA_TX_ADDR, pdma->tx_phys);
	xy1000_pdma_write(pdma, XY1000_DMA_TX_CFG,
			  (XY1000_DMA_PORT << 12) | (total_len / 8));
	xy1000_pdma_write(pdma, XY1000_DMA_TX_START, 1);
	spin_unlock_irqrestore(&pdma->state_lock, flags);

	skb_tx_timestamp(skb);
	dev_consume_skb_any(skb);

	return NETDEV_TX_OK;
}

static netdev_tx_t xy1000_pdma_start_xmit(struct sk_buff *skb,
					  struct net_device *ndev)
{
	struct xy1000_pdma *pdma = netdev_priv(ndev);

	return xy1000_pdma_xmit(pdma, skb, ndev, XY1000_DEFAULT_HOST_LPORT,
				false);
}

netdev_tx_t xy1000_pdma_lport_xmit(struct sk_buff *skb,
				   struct net_device *queue_ndev, u8 lport)
{
	struct xy1000_pdma *pdma;
	netdev_tx_t ret;

	if (!skb || !queue_ndev || lport > XY1000_PDMA_MAX_LPORT) {
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	rcu_read_lock();
	pdma = rcu_dereference(xy1000_pdma_instance);
	if (!pdma) {
		rcu_read_unlock();
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	ret = xy1000_pdma_xmit(pdma, skb, queue_ndev, lport, true);
	rcu_read_unlock();

	return ret;
}
EXPORT_SYMBOL_GPL(xy1000_pdma_lport_xmit);

static void xy1000_pdma_tx_timeout(struct net_device *ndev, unsigned int txqueue)
{
	struct xy1000_pdma *pdma = netdev_priv(ndev);
	unsigned long flags;

	spin_lock_irqsave(&pdma->state_lock, flags);
	/*
	 * The tx-end completion for the stuck frame never arrived.  Merely
	 * clearing tx_busy would let the next frame start while the old TX DMA
	 * may still be in flight, and a late tx-end would then mis-account the
	 * new frame.  Re-initialise the datapath with the same proven bring-up
	 * sequence as hw_get() so no stale TX descriptor survives; this also
	 * clears tx_busy/tx_len and re-arms RX.  hw refcount (users) is left
	 * untouched.
	 */
	if (pdma->running) {
		xy1000_pdma_hw_stop_locked(pdma);
		xy1000_pdma_hw_start_locked(pdma);
	} else {
		pdma->tx_busy = false;
		pdma->tx_len = 0;
	}
	spin_unlock_irqrestore(&pdma->state_lock, flags);

	xy1000_pdma_stats_tx_drop(pdma);
	xy1000_pdma_wake_queues(pdma);
}

int xy1000_pdma_register_lport(u8 lport, struct net_device *ndev)
{
	struct net_device *old;
	struct xy1000_pdma *pdma;
	int ret = 0;

	if (!ndev || lport > XY1000_PDMA_MAX_LPORT)
		return -EINVAL;

	mutex_lock(&xy1000_pdma_registry_lock);
	pdma = rcu_dereference_protected(xy1000_pdma_instance,
					 lockdep_is_held(&xy1000_pdma_registry_lock));
	if (!pdma) {
		ret = -ENODEV;
		goto out;
	}

	old = rcu_dereference_protected(pdma->lport_ndev[lport],
					lockdep_is_held(&xy1000_pdma_registry_lock));
	if (old && old != ndev) {
		ret = -EBUSY;
		goto out;
	}
	if (!old) {
		dev_hold(ndev);
		rcu_assign_pointer(pdma->lport_ndev[lport], ndev);
	}

out:
	mutex_unlock(&xy1000_pdma_registry_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(xy1000_pdma_register_lport);

void xy1000_pdma_unregister_lport(u8 lport, const struct net_device *ndev)
{
	struct net_device *old = NULL;
	struct xy1000_pdma *pdma;

	if (lport > XY1000_PDMA_MAX_LPORT)
		return;

	mutex_lock(&xy1000_pdma_registry_lock);
	pdma = rcu_dereference_protected(xy1000_pdma_instance,
					 lockdep_is_held(&xy1000_pdma_registry_lock));
	if (pdma) {
		old = rcu_dereference_protected(pdma->lport_ndev[lport],
						lockdep_is_held(&xy1000_pdma_registry_lock));
		if (old == ndev)
			RCU_INIT_POINTER(pdma->lport_ndev[lport], NULL);
		else
			old = NULL;
	}
	mutex_unlock(&xy1000_pdma_registry_lock);

	if (old) {
		synchronize_rcu();
		dev_put(old);
	}
}
EXPORT_SYMBOL_GPL(xy1000_pdma_unregister_lport);

int xy1000_pdma_lport_open(u8 lport, const struct net_device *ndev)
{
	struct net_device *registered;
	struct xy1000_pdma *pdma;
	int ret = 0;

	if (!ndev || lport > XY1000_PDMA_MAX_LPORT)
		return -EINVAL;

	rcu_read_lock();
	pdma = rcu_dereference(xy1000_pdma_instance);
	if (!pdma) {
		ret = -ENODEV;
		goto out;
	}

	registered = rcu_dereference(pdma->lport_ndev[lport]);
	if (registered != ndev) {
		ret = -ENODEV;
		goto out;
	}

	xy1000_pdma_hw_get(pdma);

out:
	rcu_read_unlock();
	return ret;
}
EXPORT_SYMBOL_GPL(xy1000_pdma_lport_open);

void xy1000_pdma_lport_stop(u8 lport, const struct net_device *ndev)
{
	struct net_device *registered;
	struct xy1000_pdma *pdma;

	if (!ndev || lport > XY1000_PDMA_MAX_LPORT)
		return;

	rcu_read_lock();
	pdma = rcu_dereference(xy1000_pdma_instance);
	if (!pdma)
		goto out;

	registered = rcu_dereference(pdma->lport_ndev[lport]);
	if (registered == ndev)
		xy1000_pdma_hw_put(pdma);

out:
	rcu_read_unlock();
}
EXPORT_SYMBOL_GPL(xy1000_pdma_lport_stop);

static void xy1000_pdma_registry_clear(struct xy1000_pdma *pdma)
{
	struct net_device *old[XY1000_PDMA_MAX_LPORT + 1] = {};
	unsigned int lport;
	bool sync = false;

	mutex_lock(&xy1000_pdma_registry_lock);
	if (rcu_dereference_protected(xy1000_pdma_instance,
				      lockdep_is_held(&xy1000_pdma_registry_lock)) == pdma)
		RCU_INIT_POINTER(xy1000_pdma_instance, NULL);

	for (lport = 0; lport <= XY1000_PDMA_MAX_LPORT; lport++) {
		old[lport] = rcu_dereference_protected(pdma->lport_ndev[lport],
						       lockdep_is_held(&xy1000_pdma_registry_lock));
		if (old[lport]) {
			RCU_INIT_POINTER(pdma->lport_ndev[lport], NULL);
			sync = true;
		}
	}
	mutex_unlock(&xy1000_pdma_registry_lock);

	if (sync)
		synchronize_rcu();

	for (lport = 0; lport <= XY1000_PDMA_MAX_LPORT; lport++)
		if (old[lport])
			dev_put(old[lport]);
}

static int xy1000_pdma_change_mtu(struct net_device *ndev, int new_mtu)
{
	if (new_mtu < ETH_MIN_MTU || new_mtu > ETH_DATA_LEN)
		return -EINVAL;

	ndev->mtu = new_mtu;
	return 0;
}

static void xy1000_pdma_get_stats64(struct net_device *ndev,
				    struct rtnl_link_stats64 *stats)
{
	struct xy1000_pdma *pdma = netdev_priv(ndev);
	unsigned long flags;

	spin_lock_irqsave(&pdma->stats_lock, flags);
	*stats = pdma->stats;
	spin_unlock_irqrestore(&pdma->stats_lock, flags);
}

static const struct net_device_ops xy1000_pdma_netdev_ops = {
	.ndo_open		= xy1000_pdma_open,
	.ndo_stop		= xy1000_pdma_stop,
	.ndo_start_xmit		= xy1000_pdma_start_xmit,
	.ndo_tx_timeout		= xy1000_pdma_tx_timeout,
	.ndo_change_mtu		= xy1000_pdma_change_mtu,
	.ndo_get_stats64	= xy1000_pdma_get_stats64,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_set_mac_address	= eth_mac_addr,
};

static bool xy1000_pdma_buffer_resource_valid(const struct resource *res)
{
	return resource_size(res) >= XY1000_BUF_TOTAL_SIZE &&
	       res->start <= (resource_size_t)U32_MAX -
			     XY1000_BUF_TOTAL_SIZE + 1 &&
	       IS_ALIGNED(res->start, XY1000_BUF_SIZE);
}

static int xy1000_pdma_request_irq(struct platform_device *pdev,
				   const char *name, irq_handler_t handler,
				   struct xy1000_pdma *pdma)
{
	int irq;

	irq = platform_get_irq_byname(pdev, name);
	if (irq < 0)
		return irq;

	return devm_request_irq(&pdev->dev, irq, handler, 0, name, pdma);
}

static int xy1000_pdma_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *buf_res;
	struct net_device *ndev;
	struct xy1000_pdma *pdma;
	u8 __iomem *buf;
	int ret;

	/*
	 * Allocate the netdev with devres so its lifetime is tied to the
	 * platform device.  The packet-DMA IRQs are requested with
	 * devm_request_irq() against the same device below; because devres
	 * releases in reverse order of acquisition, the IRQs are always freed
	 * before the netdev (and therefore before netdev_priv(), which the IRQ
	 * handlers dereference).  A plain alloc_etherdev()/free_netdev() pair
	 * would free the netdev first and leave the IRQ handlers pointing at
	 * freed memory until devres later ran free_irq().
	 */
	ndev = devm_alloc_etherdev(dev, sizeof(*pdma));
	if (!ndev)
		return -ENOMEM;

	SET_NETDEV_DEV(ndev, dev);
	strscpy(ndev->name, "xy%d", IFNAMSIZ);

	pdma = netdev_priv(ndev);
	pdma->dev = dev;
	pdma->ndev = ndev;
	spin_lock_init(&pdma->state_lock);
	spin_lock_init(&pdma->stats_lock);

	pdma->regs = devm_platform_ioremap_resource_byname(pdev, "regs");
	if (IS_ERR(pdma->regs)) {
		ret = PTR_ERR(pdma->regs);
		return ret;
	}

	buf_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "buffer");
	if (!buf_res) {
		ret = dev_err_probe(dev, -EINVAL,
				    "missing packet-DMA buffer resource\n");
		return ret;
	}
	if (!xy1000_pdma_buffer_resource_valid(buf_res)) {
		ret = dev_err_probe(dev, -EINVAL,
				    "invalid packet-DMA buffer resource\n");
		return ret;
	}

	pdma->rx_buf = devm_ioremap_resource(dev, buf_res);
	if (IS_ERR(pdma->rx_buf)) {
		ret = PTR_ERR(pdma->rx_buf);
		return ret;
	}
	buf = pdma->rx_buf;
	pdma->tx_buf = buf + XY1000_BUF_SIZE;
	pdma->rx_phys = (u32)buf_res->start;
	pdma->tx_phys = (u32)(buf_res->start + XY1000_BUF_SIZE);

	pdma->tx_bounce = devm_kmalloc(dev, XY1000_BUF_SIZE, GFP_KERNEL);
	if (!pdma->tx_bounce) {
		ret = -ENOMEM;
		return ret;
	}

	xy1000_pdma_irq_mask(pdma, XY1000_IRQ_MASK_ALL);

	ret = xy1000_pdma_request_irq(pdev, "tx-end",
				      xy1000_pdma_irq_tx_end, pdma);
	if (ret)
		return ret;
	ret = xy1000_pdma_request_irq(pdev, "rx-end",
				      xy1000_pdma_irq_rx_end, pdma);
	if (ret)
		return ret;
	ret = xy1000_pdma_request_irq(pdev, "rx-req",
				      xy1000_pdma_irq_rx_req, pdma);
	if (ret)
		return ret;

	ndev->netdev_ops = &xy1000_pdma_netdev_ops;
	ndev->watchdog_timeo = XY1000_TX_TIMEOUT;
	ndev->min_mtu = ETH_MIN_MTU;
	ndev->max_mtu = ETH_DATA_LEN;

	ret = of_get_ethdev_address(dev->of_node, ndev);
	if (ret)
		eth_hw_addr_set(ndev, xy1000_default_mac);
	if (!is_valid_ether_addr(ndev->dev_addr))
		eth_hw_addr_set(ndev, xy1000_default_mac);

	netif_napi_add(ndev, &pdma->napi, xy1000_pdma_poll);

	/*
	 * Enable NAPI before register_netdev() publishes the device: once
	 * registered the netdev can be opened (which starts RX and an rx-end
	 * IRQ that schedules this NAPI instance), so NAPI must already be
	 * enabled by then.
	 */
	napi_enable(&pdma->napi);

	ret = register_netdev(ndev);
	if (ret)
		goto err_napi;

	mutex_lock(&xy1000_pdma_registry_lock);
	if (rcu_dereference_protected(xy1000_pdma_instance,
				      lockdep_is_held(&xy1000_pdma_registry_lock))) {
		ret = -EBUSY;
	} else {
		rcu_assign_pointer(xy1000_pdma_instance, pdma);
	}
	mutex_unlock(&xy1000_pdma_registry_lock);
	if (ret)
		goto err_hw_stop;

	platform_set_drvdata(pdev, ndev);
	dev_info(dev,
		 "registered %s using packet buffer %pa-%pa\n",
		 ndev->name, &buf_res->start, &buf_res->end);

	return 0;

err_hw_stop:
	xy1000_pdma_hw_stop(pdma);
	unregister_netdev(ndev);
err_napi:
	napi_disable(&pdma->napi);
	netif_napi_del(&pdma->napi);
	/* ndev and the devm IRQs are released by devres on return. */
	return ret;
}

static void xy1000_pdma_remove(struct platform_device *pdev)
{
	struct net_device *ndev = platform_get_drvdata(pdev);
	struct xy1000_pdma *pdma;

	if (!ndev)
		return;

	pdma = netdev_priv(ndev);
	xy1000_pdma_registry_clear(pdma);
	xy1000_pdma_hw_stop(pdma);
	napi_disable(&pdma->napi);
	unregister_netdev(ndev);
	netif_napi_del(&pdma->napi);
	platform_set_drvdata(pdev, NULL);
	/*
	 * ndev is devm-allocated and is freed by devres after this returns,
	 * which also runs after the devm-managed IRQs are freed.  Do not call
	 * free_netdev() here.
	 */
}

static const struct of_device_id xy1000_pdma_of_match[] = {
	{ .compatible = "milkv,vega-xy1000-pdma" },
	{ }
};
MODULE_DEVICE_TABLE(of, xy1000_pdma_of_match);

static struct platform_driver xy1000_pdma_driver = {
	.probe	= xy1000_pdma_probe,
	.remove	= xy1000_pdma_remove,
	.driver	= {
		.name			= "xy1000-pdma",
		.of_match_table		= xy1000_pdma_of_match,
		.suppress_bind_attrs	= true,
	},
};
module_platform_driver(xy1000_pdma_driver);

MODULE_DESCRIPTION("FisiLink XY1000 packet-DMA host interface (Milk-V Vega)");
MODULE_LICENSE("GPL");
