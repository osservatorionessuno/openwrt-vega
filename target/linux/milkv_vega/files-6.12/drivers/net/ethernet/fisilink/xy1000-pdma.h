/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _XY1000_PDMA_H_
#define _XY1000_PDMA_H_

#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/types.h>

#define XY1000_PDMA_MAX_LPORT	63u

int xy1000_pdma_register_lport(u8 lport, struct net_device *ndev);
void xy1000_pdma_unregister_lport(u8 lport, const struct net_device *ndev);
int xy1000_pdma_lport_open(u8 lport, const struct net_device *ndev);
void xy1000_pdma_lport_stop(u8 lport, const struct net_device *ndev);
netdev_tx_t xy1000_pdma_lport_xmit(struct sk_buff *skb,
				   struct net_device *queue_ndev, u8 lport);

#endif /* _XY1000_PDMA_H_ */
