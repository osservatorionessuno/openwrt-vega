// SPDX-License-Identifier: GPL-2.0-only
/*
 * Linux bridge/switchdev control surface for the Milk-V Vega FSL91030M RJ45
 * G5..G12 datapath.
 *
 * This registers Linux-visible control netdevs for the supported RJ45 ports and
 * translates bridge port state, bridge ageing, and the verified VID1107 bridge
 * VLAN row into hardware operations.
 * Ageing time zero disables hardware ageing, matching the Linux bridge API.
 * The hardware timer has whole-second granularity; exact whole-second bridge
 * values in the verified 1..3600 second range are programmed unchanged, while
 * other bridge ageing values are rounded up and clamped to the hardware range.
 * Linux queues bridge ageing switchdev updates with SWITCHDEV_F_DEFER before it
 * commits the bridge's own timer.  Once the deferred item has been queued, a
 * later driver error is only logged and cannot roll the bridge state back, so
 * the offload intentionally saturates to the nearest supported hardware value
 * rather than allowing Linux and the switch to diverge. VLAN filtering is
 * admitted only for the packet-proven VID1107 tagged/egress-untag row. While
 * bridge VLAN filtering is enabled, unsupported VIDs and VID1107 PVID ingress
 * are rejected because the hardware would otherwise forward traffic that Linux
 * believes is filtered. Bridge port flag changes are rejected unless they keep
 * the Linux default port policy used by the verified RJ45 bridge datapath,
 * because the driver only programs the fixed hardware gates described below.
 *
 * Internal object callbacks return -EOPNOTSUPP for unsupported objects; the
 * switchdev notifier wrapper converts that to "not handled" so the bridge can
 * keep bookkeeping-only objects in software without changing hardware.
 */
#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/compiler.h>
#include <linux/etherdevice.h>
#include <linux/if_bridge.h>
#include <linux/if_vlan.h>
#include <linux/jiffies.h>
#include <linux/net.h>
#include <linux/netdevice.h>
#include <linux/notifier.h>
#include <linux/rtnetlink.h>
#include <linux/slab.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>
#include <net/switchdev.h>

#include "fsl91030m.h"
#include "xy1000-pdma.h"

#define FSL91030M_SWDEV_NPORTS FSL91030M_VEGA_PORT_COUNT
#define FSL91030M_SWDEV_AGEING_DEFAULT 300u
#define FSL91030M_SWDEV_STATS_INTERVAL HZ

#define FSL91030M_SWDEV_PARENT_ID_LEN 12
#define FSL91030M_SWDEV_PARENT_ID_SWITCH_OFF 4
#define FSL91030M_SWDEV_ADDR_TYPE_MASK \
	(BIT(0) | BIT(1))
#define FSL91030M_SWDEV_ADDR_LOCAL_UNICAST \
	BIT(1)
#define FSL91030M_SWDEV_ADDR_FIRST_BYTE \
	FSL91030M_SWDEV_ADDR_LOCAL_UNICAST
#define FSL91030M_SWDEV_ADDR_LPORT_OFF (ETH_ALEN - 1)
#define FSL91030M_SWDEV_ADDR_FIRST_BYTE_VALID(byte) \
	(((byte) & FSL91030M_SWDEV_ADDR_TYPE_MASK) == \
	 FSL91030M_SWDEV_ADDR_LOCAL_UNICAST)
#define FSL91030M_SWDEV_BRPORT_DEFAULT_FLAGS \
	(BR_LEARNING | BR_FLOOD | BR_MCAST_FLOOD | BR_BCAST_FLOOD)
/*
 * Only the standard bridge default policy is verified.  Treat any requested
 * change outside these fixed flags as unsupported so Linux does not believe
 * unimplemented port policy is offloaded.
 */
#define FSL91030M_SWDEV_BRPORT_SUPPORTED_FLAGS \
	FSL91030M_SWDEV_BRPORT_DEFAULT_FLAGS

static_assert(FSL91030M_SWDEV_NPORTS <= BITS_PER_LONG,
	      "bridge member bitmap must fit unsigned long");
static_assert(FSL91030M_AGEING_DISABLE_SEC < FSL91030M_AGEING_MIN_SEC,
	      "disabled ageing sentinel must not overlap timed ageing values");
static_assert(FSL91030M_SWDEV_PARENT_ID_LEN <= MAX_PHYS_ITEM_ID_LEN,
	      "switchdev parent ID must fit netdev storage");
static_assert((FSL91030M_SWDEV_AGEING_DEFAULT -
	       FSL91030M_AGEING_MIN_SEC) <=
	      (FSL91030M_AGEING_MAX_SEC - FSL91030M_AGEING_MIN_SEC),
	      "switchdev default ageing time must fit hardware range");
static_assert(FSL91030M_SWDEV_ADDR_FIRST_BYTE_VALID(FSL91030M_SWDEV_ADDR_FIRST_BYTE),
	      "switchdev control-port MAC prefix must be locally administered unicast");
static_assert(FSL91030M_SWDEV_ADDR_LPORT_OFF < ETH_ALEN,
	      "switchdev control-port MAC lport byte must fit Ethernet address");
static_assert((FSL91030M_SWDEV_BRPORT_DEFAULT_FLAGS &
	       ~FSL91030M_SWDEV_BRPORT_SUPPORTED_FLAGS) == 0,
	      "switchdev default bridge port flags must be supported");

static const u8 fsl91030m_swdev_parent_prefix[] = {
	0xf5, 0x91, 0x03, 0x0d,
};

static_assert(FSL91030M_SWDEV_PARENT_ID_SWITCH_OFF >=
	      sizeof(fsl91030m_swdev_parent_prefix),
	      "switchdev parent prefix must not overlap switch identity");
static_assert(FSL91030M_SWDEV_PARENT_ID_SWITCH_OFF + sizeof(u64) <=
	      FSL91030M_SWDEV_PARENT_ID_LEN,
	      "switchdev switch identity must fit parent ID");

static const u8 fsl91030m_swdev_addr_prefix[ETH_ALEN] = {
	FSL91030M_SWDEV_ADDR_FIRST_BYTE, 0xf5, 0x91, 0x03, 0x00, 0x00,
};

struct fsl91030m_swdev_port {
	struct fsl91030m_switchdev *sd;
	const struct fsl91030m_port_desc *desc;
	struct net_device *ndev;
	unsigned int index;
	bool bridge_joined;
	bool bridge_member;
	bool bridge_member_valid;
	bool hw_state_valid;
	u8 hw_state;
	bool pdma_active;
	bool vlan1107;
	bool vlan1107_untagged;
	struct rtnl_link_stats64 stats;
	struct rtnl_link_stats64 hw_stats_prev;
	spinlock_t stats_lock;	/* protects cached and software port counters */
	bool hw_stats_valid;
};

struct fsl91030m_switchdev {
	struct fsl91030m *sw;
	struct notifier_block netdevice_nb;
	struct notifier_block switchdev_nb;
	struct notifier_block switchdev_blocking_nb;
	struct delayed_work stats_work;
	struct net_device *bridge_dev;
	struct netdev_phys_item_id parent_id;
	unsigned long bridge_members;
	u8 cpu_trap_mac[ETH_ALEN];
	struct fsl91030m_swdev_port *ports[FSL91030M_SWDEV_NPORTS];
	u32 ageing_time;
	bool ageing_time_valid;
	bool vlan_filtering;
	bool cpu_trap_mac_valid;
	bool stats_active;	/* gates delayed stats polling */
	bool unregistering;	/* blocks notifier work during teardown */
};

static const struct net_device_ops fsl91030m_swdev_netdev_ops;

static bool
fsl91030m_swdev_port_bridge_joined(const struct fsl91030m_swdev_port *port)
{
	return READ_ONCE(port->bridge_joined);
}

static void
fsl91030m_swdev_port_bridge_joined_set(struct fsl91030m_swdev_port *port,
				       bool joined)
{
	WRITE_ONCE(port->bridge_joined, joined);
}

static void fsl91030m_swdev_parent_id_init(struct fsl91030m_switchdev *sd)
{
	unsigned char *id = sd->parent_id.id;

	sd->parent_id.id_len = FSL91030M_SWDEV_PARENT_ID_LEN;
	memcpy(id, fsl91030m_swdev_parent_prefix,
	       sizeof(fsl91030m_swdev_parent_prefix));
	put_unaligned_be64((u64)sd->sw->switch_phys,
			   id + FSL91030M_SWDEV_PARENT_ID_SWITCH_OFF);
}

static int fsl91030m_swdev_cpu_trap_mac_sync(struct fsl91030m_switchdev *sd)
{
	const u8 *mac;
	int ret;

	if (!sd->bridge_dev || !sd->bridge_members) {
		if (!sd->cpu_trap_mac_valid)
			return 0;

		ret = fsl91030m_l2_cpu_local_mac_set(sd->sw, NULL, false);
		if (ret)
			return ret;

		eth_zero_addr(sd->cpu_trap_mac);
		sd->cpu_trap_mac_valid = false;
		return 0;
	}

	mac = sd->bridge_dev->dev_addr;
	if (sd->cpu_trap_mac_valid && ether_addr_equal(sd->cpu_trap_mac, mac))
		return 0;

	ret = fsl91030m_l2_cpu_local_mac_set(sd->sw, mac, true);
	if (ret)
		return ret;

	ether_addr_copy(sd->cpu_trap_mac, mac);
	sd->cpu_trap_mac_valid = true;
	return 0;
}

static bool fsl91030m_swdev_port_dev_check(const struct net_device *ndev)
{
	return ndev && ndev->netdev_ops == &fsl91030m_swdev_netdev_ops;
}

static struct fsl91030m_swdev_port *
fsl91030m_swdev_port_from_dev(const struct net_device *ndev)
{
	if (!fsl91030m_swdev_port_dev_check(ndev))
		return NULL;

	return netdev_priv(ndev);
}

static struct fsl91030m_swdev_port *
fsl91030m_swdev_port_from_dev_for_sd(struct fsl91030m_switchdev *sd,
				     const struct net_device *ndev)
{
	struct fsl91030m_swdev_port *port;

	port = fsl91030m_swdev_port_from_dev(ndev);
	if (!port || port->sd != sd)
		return NULL;

	return port;
}

bool fsl91030m_switchdev_resolve_netdev(const struct net_device *ndev,
					struct fsl91030m **sw,
					const struct fsl91030m_port_desc **desc)
{
	struct fsl91030m_swdev_port *port;

	if (!sw || !desc)
		return false;

	port = fsl91030m_swdev_port_from_dev(ndev);
	if (!port || !port->sd || !port->sd->sw || !port->desc)
		return false;

	*sw = port->sd->sw;
	*desc = port->desc;
	return true;
}

static void fsl91030m_swdev_port_stats_tx_drop(struct fsl91030m_swdev_port *port)
{
	unsigned long flags;

	spin_lock_irqsave(&port->stats_lock, flags);
	port->stats.tx_dropped++;
	spin_unlock_irqrestore(&port->stats_lock, flags);
}

static u64 fsl91030m_swdev_counter_delta(u64 now, u64 prev)
{
	/*
	 * If a CMAC counter decreases after a hardware reset or wrap, continue
	 * from the current hardware value instead of publishing a regression.
	 */
	return now >= prev ? now - prev : now;
}

static void
fsl91030m_swdev_port_stats_hw_update(struct fsl91030m_swdev_port *port,
				     const struct rtnl_link_stats64 *hw)
{
	unsigned long flags;

	spin_lock_irqsave(&port->stats_lock, flags);
	if (!port->hw_stats_valid) {
		port->hw_stats_prev = *hw;
		port->hw_stats_valid = true;
		goto out;
	}

	port->stats.rx_packets +=
		fsl91030m_swdev_counter_delta(hw->rx_packets,
					      port->hw_stats_prev.rx_packets);
	port->stats.rx_bytes +=
		fsl91030m_swdev_counter_delta(hw->rx_bytes,
					      port->hw_stats_prev.rx_bytes);
	port->stats.multicast +=
		fsl91030m_swdev_counter_delta(hw->multicast,
					      port->hw_stats_prev.multicast);
	port->stats.tx_packets +=
		fsl91030m_swdev_counter_delta(hw->tx_packets,
					      port->hw_stats_prev.tx_packets);
	port->stats.tx_bytes +=
		fsl91030m_swdev_counter_delta(hw->tx_bytes,
					      port->hw_stats_prev.tx_bytes);
	port->hw_stats_prev = *hw;
out:
	spin_unlock_irqrestore(&port->stats_lock, flags);
}

static void fsl91030m_swdev_stats_work(struct work_struct *work)
{
	struct fsl91030m_switchdev *sd =
		container_of(to_delayed_work(work), struct fsl91030m_switchdev,
			     stats_work);
	unsigned int i;

	if (!READ_ONCE(sd->stats_active))
		return;

	for (i = 0; i < FSL91030M_SWDEV_NPORTS; i++) {
		struct fsl91030m_swdev_port *port = sd->ports[i];
		struct rtnl_link_stats64 hw = {};
		int ret;

		if (!port)
			continue;

		ret = fsl91030m_port_hw_stats_read(sd->sw, port->desc, &hw);
		if (ret) {
			if (net_ratelimit())
				netdev_err(port->ndev,
					   "failed to read CMAC stats: %d\n",
					   ret);
			continue;
		}

		fsl91030m_swdev_port_stats_hw_update(port, &hw);
	}

	if (READ_ONCE(sd->stats_active))
		schedule_delayed_work(&sd->stats_work,
				      FSL91030M_SWDEV_STATS_INTERVAL);
}

static int fsl91030m_swdev_port_bridge_member_set(struct fsl91030m_swdev_port *port,
						  bool member)
{
	struct fsl91030m *sw = port->sd->sw;
	int ret;

	if (port->bridge_member_valid && port->bridge_member == member)
		return 0;

	ret = fsl91030m_l2_bridge_membership_set(sw, port->desc->lport,
						 member);
	if (ret) {
		port->bridge_member_valid = false;
		return ret;
	}

	port->bridge_member = member;
	port->bridge_member_valid = true;
	return 0;
}

static bool fsl91030m_swdev_stp_state_valid(u8 state)
{
	switch (state) {
	case BR_STATE_DISABLED:
	case BR_STATE_LISTENING:
	case BR_STATE_LEARNING:
	case BR_STATE_FORWARDING:
	case BR_STATE_BLOCKING:
		return true;
	default:
		return false;
	}
}

static u8 fsl91030m_swdev_stp_to_hw(u8 state)
{
	switch (state) {
	case BR_STATE_FORWARDING:
		return FSL_INET_STP_FORWARDING;
	case BR_STATE_LEARNING:
		return FSL_INET_STP_LEARNING;
	case BR_STATE_LISTENING:
	case BR_STATE_BLOCKING:
		return FSL_INET_STP_BLOCKING;
	case BR_STATE_DISABLED:
	default:
		return FSL_INET_STP_DISABLED;
	}
}

static u8
fsl91030m_swdev_port_hw_state_from_stp_active(struct fsl91030m_swdev_port *port,
					      u8 state, bool bridge_joined)
{
	if (!bridge_joined || !netif_running(port->ndev))
		return FSL_INET_STP_DISABLED;

	return fsl91030m_swdev_stp_to_hw(state);
}

static bool fsl91030m_swdev_hw_state_has_membership(u8 hw_state)
{
	return hw_state != FSL_INET_STP_DISABLED;
}

static bool
fsl91030m_swdev_port_default_member_from_state(struct fsl91030m_swdev_port *port,
					       u8 hw_state)
{
	if (READ_ONCE(port->sd->vlan_filtering))
		return false;

	return fsl91030m_swdev_hw_state_has_membership(hw_state);
}

static u8
fsl91030m_swdev_port_wanted_hw_state(struct fsl91030m_swdev_port *port,
				     bool bridge_joined)
{
	u8 state;

	if (!bridge_joined)
		return FSL_INET_STP_DISABLED;

	state = br_port_get_stp_state(port->ndev);
	if (!fsl91030m_swdev_stp_state_valid(state))
		state = BR_STATE_DISABLED;

	return fsl91030m_swdev_port_hw_state_from_stp_active(port, state,
							     bridge_joined);
}

static int fsl91030m_swdev_port_hw_state_set(struct fsl91030m_swdev_port *port,
					     u8 hw_state)
{
	struct fsl91030m *sw = port->sd->sw;
	int ret;

	if (port->hw_state_valid && port->hw_state == hw_state)
		return 0;

	ret = fsl91030m_l2_bridge_port_state_set(sw, port->desc->lport,
						 hw_state);
	if (ret) {
		port->hw_state_valid = false;
		return ret;
	}

	port->hw_state = hw_state;
	port->hw_state_valid = true;
	return 0;
}

static int fsl91030m_swdev_port_block(struct fsl91030m_swdev_port *port)
{
	int state_ret;
	int ret;

	ret = fsl91030m_swdev_port_bridge_member_set(port, false);
	state_ret = fsl91030m_swdev_port_hw_state_set(port,
						      FSL_INET_STP_DISABLED);

	return ret ?: state_ret;
}

static int fsl91030m_swdev_port_hw_state_sync(struct fsl91030m_swdev_port *port)
{
	bool bridge_joined = fsl91030m_swdev_port_bridge_joined(port);
	u8 hw_state = fsl91030m_swdev_port_wanted_hw_state(port, bridge_joined);

	return fsl91030m_swdev_port_hw_state_set(port, hw_state);
}

static int
fsl91030m_swdev_port_default_member_sync(struct fsl91030m_swdev_port *port)
{
	bool bridge_joined = fsl91030m_swdev_port_bridge_joined(port);
	u8 hw_state = fsl91030m_swdev_port_wanted_hw_state(port, bridge_joined);
	bool member;

	member = fsl91030m_swdev_port_default_member_from_state(port, hw_state);

	return fsl91030m_swdev_port_bridge_member_set(port, member);
}

static int
fsl91030m_swdev_default_members_sync(struct fsl91030m_switchdev *sd)
{
	unsigned int i;
	int ret;

	for (i = 0; i < FSL91030M_SWDEV_NPORTS; i++) {
		if (!sd->ports[i])
			continue;

		ret = fsl91030m_swdev_port_default_member_sync(sd->ports[i]);
		if (ret)
			return ret;
	}

	return 0;
}

static int
fsl91030m_swdev_port_stp_state_set(struct fsl91030m_swdev_port *port,
				   u8 stp_state,
				   struct netlink_ext_ack *extack)
{
	u8 hw_state;
	int rb_ret;
	int ret;

	if (!fsl91030m_swdev_stp_state_valid(stp_state)) {
		NL_SET_ERR_MSG_MOD(extack, "unsupported bridge STP state");
		return -EINVAL;
	}

	hw_state = fsl91030m_swdev_port_hw_state_from_stp_active(port,
								 stp_state,
								 true);
	if (hw_state == FSL_INET_STP_FORWARDING) {
		bool member;

		ret = fsl91030m_swdev_port_hw_state_set(port, hw_state);
		if (ret)
			return ret;

		member = fsl91030m_swdev_port_default_member_from_state(port,
									hw_state);
		ret = fsl91030m_swdev_port_bridge_member_set(port, member);
		if (ret) {
			rb_ret = fsl91030m_swdev_port_hw_state_set(port,
								   FSL_INET_STP_DISABLED);
			if (rb_ret) {
				netdev_err(port->ndev,
					   "failed to roll back bridge port state after membership error %d: %d\n",
					   ret, rb_ret);
				ret = rb_ret;
				NL_SET_ERR_MSG_MOD(extack,
						   "failed to roll back bridge port state");
			} else {
				NL_SET_ERR_MSG_MOD(extack,
						   "failed to program bridge membership gate");
			}
		}

		return ret;
	}

	ret = fsl91030m_swdev_port_bridge_member_set(port, false);
	if (ret) {
		rb_ret = fsl91030m_swdev_port_hw_state_set(port,
							   FSL_INET_STP_DISABLED);
		if (rb_ret) {
			netdev_err(port->ndev,
				   "failed to force disabled bridge port state after membership error %d: %d\n",
				   ret, rb_ret);
			ret = rb_ret;
			NL_SET_ERR_MSG_MOD(extack,
					   "failed to disable bridge membership gate and force disabled port state");
		} else {
			NL_SET_ERR_MSG_MOD(extack,
					   "failed to disable bridge membership gate; port state forced disabled");
		}
		return ret;
	}

	ret = fsl91030m_swdev_port_hw_state_set(port, hw_state);
	if (ret) {
		NL_SET_ERR_MSG_MOD(extack,
				   "failed to program bridge port state");
		return ret;
	}

	if (!fsl91030m_swdev_port_default_member_from_state(port, hw_state))
		return 0;

	ret = fsl91030m_swdev_port_bridge_member_set(port, true);
	if (ret)
		NL_SET_ERR_MSG_MOD(extack,
				   "failed to restore bridge membership gate");

	return ret;
}

static netdev_tx_t fsl91030m_swdev_xmit(struct sk_buff *skb,
					struct net_device *ndev)
{
	struct fsl91030m_swdev_port *port = netdev_priv(ndev);

	if (likely(port && port->pdma_active &&
		   port->desc->lport == FSL91030M_VEGA_G5_LPORT))
		return xy1000_pdma_lport_xmit(skb, ndev,
					      FSL91030M_VEGA_CPU_LPORT);

	if (likely(port))
		fsl91030m_swdev_port_stats_tx_drop(port);
	dev_kfree_skb_any(skb);

	return NETDEV_TX_OK;
}

static int fsl91030m_swdev_open(struct net_device *ndev)
{
	struct fsl91030m_swdev_port *port = netdev_priv(ndev);
	bool member;
	u8 hw_state;
	int rb_ret;
	int ret;

	hw_state = fsl91030m_swdev_port_wanted_hw_state(port,
							fsl91030m_swdev_port_bridge_joined(port));
	member = fsl91030m_swdev_port_default_member_from_state(port, hw_state);
	ret = fsl91030m_swdev_port_hw_state_set(port, hw_state);
	if (ret) {
		netdev_err(ndev, "failed to program bridge port state: %d\n",
			   ret);
		return ret;
	}

	ret = fsl91030m_swdev_port_bridge_member_set(port, member);
	if (ret) {
		int member_ret = ret;

		rb_ret = fsl91030m_swdev_port_hw_state_set(port,
							   FSL_INET_STP_DISABLED);
		if (rb_ret) {
			netdev_err(ndev,
				   "failed to roll back bridge port state after membership error %d: %d\n",
				   member_ret, rb_ret);
			ret = rb_ret;
		}
		netdev_err(ndev,
			   "failed to program bridge membership gate: %d\n",
			   member_ret);
		return ret;
	}

	ret = xy1000_pdma_lport_open(port->desc->lport, ndev);
	if (ret) {
		netdev_warn(ndev,
			    "packet-DMA RX unavailable for logical port %u: %d\n",
			    port->desc->lport, ret);
	} else {
		port->pdma_active = true;
	}

	netif_carrier_on(ndev);
	netif_start_queue(ndev);
	return 0;
}

static int fsl91030m_swdev_stop(struct net_device *ndev)
{
	struct fsl91030m_swdev_port *port = netdev_priv(ndev);
	int ret;

	netif_stop_queue(ndev);
	netif_carrier_off(ndev);

	if (port->pdma_active) {
		xy1000_pdma_lport_stop(port->desc->lport, ndev);
		port->pdma_active = false;
	}

	ret = fsl91030m_swdev_port_bridge_member_set(port, false);
	if (ret)
		netdev_err(ndev,
			   "failed to disable bridge membership gate: %d\n",
			   ret);

	ret = fsl91030m_swdev_port_hw_state_set(port, FSL_INET_STP_DISABLED);
	if (ret)
		netdev_err(ndev, "failed to block bridge port state: %d\n",
			   ret);

	return 0;
}

static int fsl91030m_swdev_change_mtu(struct net_device *ndev, int new_mtu)
{
	/*
	 * Hardware MTU programming is not part of the verified RJ45 offload
	 * contract. Keep Linux from advertising a frame-size policy that the
	 * switch fabric is not known to enforce.
	 */
	return new_mtu == ndev->mtu ? 0 : -EOPNOTSUPP;
}

static void fsl91030m_swdev_get_stats64(struct net_device *ndev,
					struct rtnl_link_stats64 *storage)
{
	struct fsl91030m_swdev_port *port = netdev_priv(ndev);
	unsigned long flags;

	/*
	 * These switchdev netdevs expose the Linux control plane for G5..G12.
	 * Hardware CMAC counters are refreshed from process context; this
	 * callback only copies the cached stats plus local software counters
	 * such as unsupported CPU-injection drops.
	 */
	spin_lock_irqsave(&port->stats_lock, flags);
	*storage = port->stats;
	spin_unlock_irqrestore(&port->stats_lock, flags);
}

static int fsl91030m_swdev_get_port_parent_id(struct net_device *ndev,
					      struct netdev_phys_item_id *ppid)
{
	struct fsl91030m_swdev_port *port = netdev_priv(ndev);

	*ppid = port->sd->parent_id;
	return 0;
}

static int fsl91030m_swdev_setup_tc(struct net_device *ndev,
				    enum tc_setup_type type,
				    void *type_data)
{
	struct fsl91030m_swdev_port *port = netdev_priv(ndev);

	return fsl91030m_qos_setup_tc(port->sd->sw, port->desc, type,
				      type_data);
}

static const struct net_device_ops fsl91030m_swdev_netdev_ops = {
	.ndo_open		= fsl91030m_swdev_open,
	.ndo_stop		= fsl91030m_swdev_stop,
	.ndo_start_xmit		= fsl91030m_swdev_xmit,
	.ndo_change_mtu		= fsl91030m_swdev_change_mtu,
	.ndo_get_stats64	= fsl91030m_swdev_get_stats64,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_get_port_parent_id	= fsl91030m_swdev_get_port_parent_id,
	.ndo_setup_tc		= fsl91030m_swdev_setup_tc,
};

static int
fsl91030m_swdev_ageing_seconds_set(struct fsl91030m_switchdev *sd,
				   u32 ageing_time,
				   struct netlink_ext_ack *extack)
{
	int ret;

	if (ageing_time != FSL91030M_AGEING_DISABLE_SEC &&
	    (ageing_time < FSL91030M_AGEING_MIN_SEC ||
	     ageing_time > FSL91030M_AGEING_MAX_SEC)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "ageing time is outside supported hardware range");
		return -ERANGE;
	}

	if (sd->ageing_time_valid && sd->ageing_time == ageing_time)
		return 0;

	ret = fsl91030m_l2_ageing_time_set(sd->sw, ageing_time);
	if (ret) {
		sd->ageing_time_valid = false;
		NL_SET_ERR_MSG_MOD(extack,
				   "failed to program IFWD ageing time");
		return ret;
	}

	sd->ageing_time = ageing_time;
	sd->ageing_time_valid = true;
	return 0;
}

static int fsl91030m_swdev_ageing_time_set(struct fsl91030m_switchdev *sd,
					   clock_t ageing_clock,
					   struct netlink_ext_ack *extack)
{
	unsigned long ageing_secs;
	unsigned long ageing_ticks;

	if (ageing_clock <= 0)
		return fsl91030m_swdev_ageing_seconds_set(sd,
							  FSL91030M_AGEING_DISABLE_SEC,
							  extack);

	ageing_ticks = (unsigned long)ageing_clock;
	ageing_secs = ageing_ticks / (unsigned long)USER_HZ;
	if (ageing_ticks % (unsigned long)USER_HZ)
		ageing_secs++;

	if (ageing_secs < FSL91030M_AGEING_MIN_SEC)
		ageing_secs = FSL91030M_AGEING_MIN_SEC;
	else if (ageing_secs > FSL91030M_AGEING_MAX_SEC)
		ageing_secs = FSL91030M_AGEING_MAX_SEC;

	return fsl91030m_swdev_ageing_seconds_set(sd, (u32)ageing_secs,
						  extack);
}

static int fsl91030m_swdev_vlan1107_sync(struct fsl91030m_switchdev *sd)
{
	struct fsl91030m_swdev_port *g5 = sd->ports[FSL91030M_VEGA_PORT_G5];
	struct fsl91030m_swdev_port *g12 = sd->ports[FSL91030M_VEGA_PORT_G12];
	bool filter = READ_ONCE(sd->vlan_filtering);

	if (!g5 || !g12)
		return -ENODEV;

	return fsl91030m_l2_vlan1107_set(sd->sw, filter && g5->vlan1107,
					 g5->vlan1107_untagged,
					 filter && g12->vlan1107,
					 g12->vlan1107_untagged);
}

static bool
fsl91030m_swdev_port_vlan1107_capable(const struct fsl91030m_swdev_port *port)
{
	u8 lport;

	if (!port || !port->desc)
		return false;

	lport = port->desc->lport;
	return lport == FSL91030M_VEGA_G5_LPORT ||
	       lport == FSL91030M_VEGA_G12_LPORT;
}

static bool
fsl91030m_swdev_vlan_filtering_ports_capable(struct fsl91030m_switchdev *sd)
{
	unsigned int i;

	for (i = 0; i < FSL91030M_SWDEV_NPORTS; i++) {
		struct fsl91030m_swdev_port *port = sd->ports[i];

		if (!port || !(sd->bridge_members & BIT(i)))
			continue;
		if (!fsl91030m_swdev_port_vlan1107_capable(port))
			return false;
	}

	return true;
}

static int fsl91030m_swdev_vlan_filtering_hw_sync(struct fsl91030m_switchdev *sd)
{
	if (READ_ONCE(sd->vlan_filtering)) {
		int ret;

		ret = fsl91030m_swdev_default_members_sync(sd);
		if (ret)
			return ret;

		return fsl91030m_swdev_vlan1107_sync(sd);
	}

	return fsl91030m_swdev_vlan1107_sync(sd) ?:
	       fsl91030m_swdev_default_members_sync(sd);
}

static int
fsl91030m_swdev_vlan_filtering_set(struct fsl91030m_switchdev *sd,
				   bool vlan_filtering,
				   struct netlink_ext_ack *extack)
{
	bool old_filtering = sd->vlan_filtering;
	int rb_ret;
	int ret;

	if (old_filtering == vlan_filtering)
		return 0;

	if (vlan_filtering &&
	    !fsl91030m_swdev_vlan_filtering_ports_capable(sd)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "VLAN filtering is verified only when bridged RJ45 members are G5/G12");
		return -EOPNOTSUPP;
	}

	sd->vlan_filtering = vlan_filtering;
	ret = fsl91030m_swdev_vlan_filtering_hw_sync(sd);
	if (!ret)
		return 0;

	sd->vlan_filtering = old_filtering;
	rb_ret = fsl91030m_swdev_vlan_filtering_hw_sync(sd);
	if (rb_ret)
		dev_err(sd->sw->dev,
			"failed to restore VLAN filtering state after error %d: %d\n",
			ret, rb_ret);

	return ret;
}

static int
fsl91030m_swdev_vlan_add(struct fsl91030m_swdev_port *port,
			 const struct switchdev_obj_port_vlan *vlan,
			 struct netlink_ext_ack *extack)
{
	bool old_untagged;
	bool old_member;
	int rb_ret;
	int ret;

	if (!fsl91030m_swdev_port_vlan1107_capable(port)) {
		if (!READ_ONCE(port->sd->vlan_filtering))
			return -EOPNOTSUPP;

		NL_SET_ERR_MSG_MOD(extack,
				   "VID1107 offload is verified only on G5/G12");
		return -EINVAL;
	}

	if (vlan->vid != FSL91030M_CTAG_SERVICE_VID) {
		if (!READ_ONCE(port->sd->vlan_filtering))
			return -EOPNOTSUPP;

		NL_SET_ERR_MSG_MOD(extack,
				   "VID is not hardware-filtered by this driver");
		return -EINVAL;
	}

	if (vlan->flags & BRIDGE_VLAN_INFO_PVID) {
		NL_SET_ERR_MSG_MOD(extack,
				   "VID1107 PVID ingress offload is not verified");
		return -EINVAL;
	}

	old_member = port->vlan1107;
	old_untagged = port->vlan1107_untagged;
	port->vlan1107 = true;
	port->vlan1107_untagged = vlan->flags & BRIDGE_VLAN_INFO_UNTAGGED;

	ret = fsl91030m_swdev_vlan1107_sync(port->sd);
	if (!ret)
		return 0;

	port->vlan1107 = old_member;
	port->vlan1107_untagged = old_untagged;
	rb_ret = fsl91030m_swdev_vlan1107_sync(port->sd);
	if (rb_ret)
		netdev_err(port->ndev,
			   "failed to restore cached VID1107 state after add error %d: %d\n",
			   ret, rb_ret);

	return ret;
}

static int
fsl91030m_swdev_vlan_del(struct fsl91030m_swdev_port *port,
			 const struct switchdev_obj_port_vlan *vlan)
{
	bool old_untagged;
	bool old_member;
	int rb_ret;
	int ret;

	if (!fsl91030m_swdev_port_vlan1107_capable(port))
		return -EOPNOTSUPP;
	if (vlan->vid != FSL91030M_CTAG_SERVICE_VID)
		return -EOPNOTSUPP;
	if (!port->vlan1107 && !port->vlan1107_untagged)
		return 0;

	old_member = port->vlan1107;
	old_untagged = port->vlan1107_untagged;
	port->vlan1107 = false;
	port->vlan1107_untagged = false;

	ret = fsl91030m_swdev_vlan1107_sync(port->sd);
	if (!ret)
		return 0;

	port->vlan1107 = old_member;
	port->vlan1107_untagged = old_untagged;
	rb_ret = fsl91030m_swdev_vlan1107_sync(port->sd);
	if (rb_ret)
		netdev_err(port->ndev,
			   "failed to restore cached VID1107 state after delete error %d: %d\n",
			   ret, rb_ret);

	return ret;
}

static void
fsl91030m_swdev_vlan1107_clear(struct fsl91030m_swdev_port *port,
			       const char *reason)
{
	int ret;

	if (!port->vlan1107 && !port->vlan1107_untagged)
		return;

	port->vlan1107 = false;
	port->vlan1107_untagged = false;

	ret = fsl91030m_swdev_vlan1107_sync(port->sd);
	if (ret && net_ratelimit())
		netdev_err(port->ndev, "failed to clear VID1107 state on %s: %d\n",
			   reason, ret);
}

static int
fsl91030m_swdev_brport_flags_check(const struct switchdev_brport_flags *flags,
				   struct netlink_ext_ack *extack)
{
	unsigned long changed;

	if (flags->mask & ~FSL91030M_SWDEV_BRPORT_SUPPORTED_FLAGS) {
		NL_SET_ERR_MSG_MOD(extack,
				   "unsupported bridge port flag offload");
		return -EINVAL;
	}

	changed = (flags->val ^ FSL91030M_SWDEV_BRPORT_DEFAULT_FLAGS) &
		  flags->mask;
	if (changed) {
		NL_SET_ERR_MSG_MOD(extack,
				   "unsupported bridge port flag state");
		return -EINVAL;
	}

	return 0;
}

static int
fsl91030m_swdev_brport_flags_attr_check(struct fsl91030m_swdev_port *port,
					struct net_device *ndev,
					const struct switchdev_attr *attr,
					struct netlink_ext_ack *extack)
{
	if (!fsl91030m_swdev_port_bridge_joined(port) || attr->orig_dev != ndev)
		return -EOPNOTSUPP;

	return fsl91030m_swdev_brport_flags_check(&attr->u.brport_flags,
						  extack);
}

static int fsl91030m_swdev_port_obj_add(struct net_device *ndev,
					const void *ctx,
					const struct switchdev_obj *obj,
					struct netlink_ext_ack *extack)
{
	struct fsl91030m_swdev_port *port =
		fsl91030m_swdev_port_from_dev(ndev);

	if (!port)
		return -EOPNOTSUPP;
	if (ctx && ctx != port)
		return -EOPNOTSUPP;

	switch (obj->id) {
	case SWITCHDEV_OBJ_ID_PORT_VLAN:
		return fsl91030m_swdev_vlan_add(port,
						SWITCHDEV_OBJ_PORT_VLAN(obj),
						extack);
	default:
		return -EOPNOTSUPP;
	}
}

static int fsl91030m_swdev_port_obj_del(struct net_device *ndev,
					const void *ctx,
					const struct switchdev_obj *obj)
{
	struct fsl91030m_swdev_port *port =
		fsl91030m_swdev_port_from_dev(ndev);

	if (!port)
		return -EOPNOTSUPP;
	if (ctx && ctx != port)
		return -EOPNOTSUPP;

	switch (obj->id) {
	case SWITCHDEV_OBJ_ID_PORT_VLAN:
		return fsl91030m_swdev_vlan_del(port,
						SWITCHDEV_OBJ_PORT_VLAN(obj));
	default:
		return -EOPNOTSUPP;
	}
}

static int fsl91030m_swdev_port_attr_set(struct net_device *ndev,
					 const void *ctx,
					 const struct switchdev_attr *attr,
					 struct netlink_ext_ack *extack)
{
	struct fsl91030m_swdev_port *port =
		fsl91030m_swdev_port_from_dev(ndev);
	u8 stp_state;

	if (!port)
		return -EOPNOTSUPP;
	if (ctx && ctx != port)
		return -EOPNOTSUPP;

	switch (attr->id) {
	case SWITCHDEV_ATTR_ID_PORT_PRE_BRIDGE_FLAGS:
	case SWITCHDEV_ATTR_ID_PORT_BRIDGE_FLAGS:
		return fsl91030m_swdev_brport_flags_attr_check(port, ndev,
							       attr, extack);
	case SWITCHDEV_ATTR_ID_PORT_STP_STATE:
		if (!fsl91030m_swdev_port_bridge_joined(port) ||
		    attr->orig_dev != ndev)
			return -EOPNOTSUPP;

		stp_state = attr->u.stp_state;
		return fsl91030m_swdev_port_stp_state_set(port, stp_state,
							  extack);
	case SWITCHDEV_ATTR_ID_BRIDGE_VLAN_FILTERING:
		if (!fsl91030m_swdev_port_bridge_joined(port) ||
		    attr->orig_dev != port->sd->bridge_dev)
			return -EOPNOTSUPP;

		return fsl91030m_swdev_vlan_filtering_set(port->sd,
							  attr->u.vlan_filtering,
							  extack);
	case SWITCHDEV_ATTR_ID_BRIDGE_AGEING_TIME:
		if (!fsl91030m_swdev_port_bridge_joined(port) ||
		    attr->orig_dev != ndev)
			return -EOPNOTSUPP;

		return fsl91030m_swdev_ageing_time_set(port->sd,
						       attr->u.ageing_time,
						       extack);
	default:
		return -EOPNOTSUPP;
	}
}

static int
fsl91030m_swdev_switchdev_obj_event(struct fsl91030m_switchdev *sd,
				    struct net_device *ndev,
				    struct switchdev_notifier_port_obj_info *info,
				    bool adding)
{
	struct netlink_ext_ack *extack =
		switchdev_notifier_info_to_extack(&info->info);
	struct fsl91030m_swdev_port *port;
	int ret;

	port = fsl91030m_swdev_port_from_dev_for_sd(sd, ndev);
	if (!port)
		return -EOPNOTSUPP;

	if (adding)
		ret = fsl91030m_swdev_port_obj_add(ndev, info->info.ctx,
						   info->obj, extack);
	else
		ret = fsl91030m_swdev_port_obj_del(ndev, info->info.ctx,
						   info->obj);

	if (ret != -EOPNOTSUPP)
		info->handled = true;

	return ret;
}

static int
fsl91030m_swdev_switchdev_attr_event(struct fsl91030m_switchdev *sd,
				     struct net_device *ndev,
				     struct switchdev_notifier_port_attr_info *info)
{
	struct netlink_ext_ack *extack =
		switchdev_notifier_info_to_extack(&info->info);
	const struct switchdev_attr *attr = info->attr;
	struct fsl91030m_swdev_port *port;
	int ret;

	port = fsl91030m_swdev_port_from_dev_for_sd(sd, ndev);
	if (port) {
		ret = fsl91030m_swdev_port_attr_set(ndev, info->info.ctx,
						    attr, extack);
		if (ret != -EOPNOTSUPP)
			info->handled = true;

		return ret;
	}

	if (attr->id == SWITCHDEV_ATTR_ID_BRIDGE_VLAN_FILTERING &&
	    ndev == sd->bridge_dev && attr->orig_dev == sd->bridge_dev) {
		info->handled = true;
		return fsl91030m_swdev_vlan_filtering_set(sd,
							  attr->u.vlan_filtering,
							  extack);
	}

	if (attr->id != SWITCHDEV_ATTR_ID_BRIDGE_AGEING_TIME ||
	    ndev != sd->bridge_dev || attr->orig_dev != sd->bridge_dev)
		return -EOPNOTSUPP;

	ret = fsl91030m_swdev_ageing_time_set(sd, attr->u.ageing_time,
					      extack);
	info->handled = true;

	return ret;
}

static int
fsl91030m_swdev_switchdev_atomic_attr_event(struct fsl91030m_switchdev *sd,
					    struct net_device *ndev,
					    struct switchdev_notifier_port_attr_info *info,
					    bool *handled)
{
	struct netlink_ext_ack *extack =
		switchdev_notifier_info_to_extack(&info->info);
	const struct switchdev_attr *attr = info->attr;
	struct fsl91030m_swdev_port *port;
	int ret;

	*handled = false;

	if (attr->id != SWITCHDEV_ATTR_ID_PORT_PRE_BRIDGE_FLAGS)
		return 0;

	port = fsl91030m_swdev_port_from_dev_for_sd(sd, ndev);
	if (!port)
		return 0;

	ret = fsl91030m_swdev_brport_flags_attr_check(port, ndev, attr,
						      extack);
	if (ret == -EOPNOTSUPP)
		return 0;

	info->handled = true;
	*handled = true;
	return ret;
}

static int
fsl91030m_swdev_switchdev_event(struct notifier_block *nb,
				unsigned long event, void *ptr)
{
	struct fsl91030m_switchdev *sd =
		container_of(nb, struct fsl91030m_switchdev, switchdev_nb);
	struct net_device *ndev = switchdev_notifier_info_to_dev(ptr);
	struct switchdev_notifier_port_attr_info *attr_info = ptr;
	bool handled;
	int ret;

	if (READ_ONCE(sd->unregistering))
		return NOTIFY_DONE;

	switch (event) {
	case SWITCHDEV_PORT_ATTR_SET:
		ret = fsl91030m_swdev_switchdev_atomic_attr_event(sd, ndev,
								  attr_info,
								  &handled);
		if (!handled)
			return NOTIFY_DONE;
		return notifier_from_errno(ret);
	default:
		return NOTIFY_DONE;
	}
}

static int
fsl91030m_swdev_switchdev_blocking_event(struct notifier_block *nb,
					 unsigned long event, void *ptr)
{
	struct fsl91030m_switchdev *sd =
		container_of(nb, struct fsl91030m_switchdev,
			     switchdev_blocking_nb);
	struct net_device *ndev = switchdev_notifier_info_to_dev(ptr);
	struct switchdev_notifier_port_obj_info *obj_info = ptr;
	struct switchdev_notifier_port_attr_info *attr_info = ptr;
	int ret;

	if (READ_ONCE(sd->unregistering))
		return NOTIFY_DONE;

	switch (event) {
	case SWITCHDEV_PORT_OBJ_ADD:
		ret = fsl91030m_swdev_switchdev_obj_event(sd, ndev, obj_info,
							  true);
		if (ret == -EOPNOTSUPP)
			return NOTIFY_DONE;
		return notifier_from_errno(ret);
	case SWITCHDEV_PORT_OBJ_DEL:
		ret = fsl91030m_swdev_switchdev_obj_event(sd, ndev, obj_info,
							  false);
		if (ret == -EOPNOTSUPP)
			return NOTIFY_DONE;
		return notifier_from_errno(ret);
	case SWITCHDEV_PORT_ATTR_SET:
		ret = fsl91030m_swdev_switchdev_attr_event(sd, ndev,
							   attr_info);
		if (ret == -EOPNOTSUPP)
			return NOTIFY_DONE;
		return notifier_from_errno(ret);
	default:
		return NOTIFY_DONE;
	}
}

static void
fsl91030m_swdev_bridge_join_rollback(struct fsl91030m_swdev_port *port,
				     bool unoffload, const char *reason)
{
	struct fsl91030m_switchdev *sd = port->sd;
	int ret;

	fsl91030m_swdev_port_bridge_joined_set(port, false);
	fsl91030m_swdev_vlan1107_clear(port, reason);

	ret = fsl91030m_swdev_port_bridge_member_set(port, false);
	if (ret && net_ratelimit())
		netdev_err(port->ndev,
			   "failed to disable bridge membership gate on %s: %d\n",
			   reason, ret);

	ret = fsl91030m_swdev_port_hw_state_set(port, FSL_INET_STP_DISABLED);
	if (ret && net_ratelimit())
		netdev_err(port->ndev,
			   "failed to block bridge port state on %s: %d\n",
			   reason, ret);

	if (unoffload) {
		switchdev_bridge_port_unoffload(port->ndev, port,
						&sd->switchdev_nb,
						&sd->switchdev_blocking_nb);
	}

	sd->bridge_members &= ~BIT(port->index);
	if (!sd->bridge_members) {
		sd->bridge_dev = NULL;
		ret = fsl91030m_swdev_cpu_trap_mac_sync(sd);
		if (ret && net_ratelimit())
			netdev_err(port->ndev,
				   "failed to clear CPU trap MAC on %s: %d\n",
				   reason, ret);

		ret = fsl91030m_swdev_ageing_seconds_set(sd,
							 FSL91030M_SWDEV_AGEING_DEFAULT,
							 NULL);
		if (ret && net_ratelimit())
			netdev_err(port->ndev,
				   "failed to restore default ageing time on %s: %d\n",
				   reason, ret);
	}
}

static int
fsl91030m_swdev_bridge_join_validate(struct fsl91030m_swdev_port *port,
				     struct net_device *bridge,
				     struct netlink_ext_ack *extack)
{
	struct fsl91030m_switchdev *sd = port->sd;

	if (!bridge || !netif_is_bridge_master(bridge)) {
		NL_SET_ERR_MSG_MOD(extack, "only Linux bridge uppers are supported");
		return -EOPNOTSUPP;
	}

	if (fsl91030m_swdev_port_bridge_joined(port)) {
		if (sd->bridge_dev == bridge)
			return 0;

		NL_SET_ERR_MSG_MOD(extack, "port is already offloaded");
		return -EOPNOTSUPP;
	}

	if (sd->bridge_dev && sd->bridge_dev != bridge) {
		NL_SET_ERR_MSG_MOD(extack, "only one bridge can be offloaded");
		return -EOPNOTSUPP;
	}

	if (READ_ONCE(sd->vlan_filtering) &&
	    !fsl91030m_swdev_port_vlan1107_capable(port)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "VLAN-filtered bridges are verified only on G5/G12");
		return -EOPNOTSUPP;
	}

	return 0;
}

static int fsl91030m_swdev_bridge_join(struct fsl91030m_swdev_port *port,
				       struct net_device *bridge,
				       struct netlink_ext_ack *extack)
{
	struct fsl91030m_switchdev *sd = port->sd;
	int rb_ret;
	int ret;
	bool member;
	u8 hw_state;

	ret = fsl91030m_swdev_bridge_join_validate(port, bridge, extack);
	if (ret)
		return ret;
	if (fsl91030m_swdev_port_bridge_joined(port))
		return 0;

	sd->bridge_dev = bridge;
	sd->bridge_members |= BIT(port->index);

	ret = fsl91030m_swdev_cpu_trap_mac_sync(sd);
	if (ret) {
		fsl91030m_swdev_bridge_join_rollback(port, false,
						     "bridge join rollback");
		NL_SET_ERR_MSG_MOD(extack,
				   "failed to program bridge CPU trap MAC");
		return ret;
	}

	/*
	 * Keep the local joined gate closed until ageing and the candidate joined
	 * STP state are programmed. The hardware bridge-membership gate is enabled
	 * last so the port cannot forward through stale state during bridge join.
	 */
	ret = switchdev_bridge_port_offload(port->ndev, port->ndev, port,
					    &sd->switchdev_nb,
					    &sd->switchdev_blocking_nb,
					    false, extack);
	if (ret) {
		fsl91030m_swdev_bridge_join_rollback(port, false,
						     "bridge join rollback");
		return ret;
	}

	ret = fsl91030m_swdev_ageing_time_set(sd, br_get_ageing_time(bridge),
					      extack);
	if (ret) {
		fsl91030m_swdev_bridge_join_rollback(port, true,
						     "bridge join rollback");
		return ret;
	}

	if (netif_running(port->ndev)) {
		hw_state = fsl91030m_swdev_port_wanted_hw_state(port, true);
		ret = fsl91030m_swdev_port_hw_state_set(port, hw_state);
		if (ret) {
			fsl91030m_swdev_bridge_join_rollback(port, true,
							     "bridge join rollback");
			NL_SET_ERR_MSG_MOD(extack,
					   "failed to program bridge port state");
			return ret;
		}

		member = fsl91030m_swdev_port_default_member_from_state(port,
									hw_state);
		ret = fsl91030m_swdev_port_bridge_member_set(port, member);
		if (ret) {
			rb_ret = fsl91030m_swdev_port_hw_state_set(port,
								   FSL_INET_STP_DISABLED);
			if (rb_ret) {
				netdev_err(port->ndev,
					   "failed to roll back bridge port state after membership error %d: %d\n",
					   ret, rb_ret);
				ret = rb_ret;
				NL_SET_ERR_MSG_MOD(extack,
						   "failed to roll back bridge port state");
			} else {
				NL_SET_ERR_MSG_MOD(extack,
						   "failed to enable bridge membership gate");
			}
			fsl91030m_swdev_bridge_join_rollback(port, true,
							     "bridge join rollback");
			return ret;
		}
	} else {
		ret = fsl91030m_swdev_port_bridge_member_set(port, false);
		if (ret) {
			fsl91030m_swdev_bridge_join_rollback(port, true,
							     "bridge join rollback");
			NL_SET_ERR_MSG_MOD(extack,
					   "failed to disable bridge membership gate");
			return ret;
		}

		ret = fsl91030m_swdev_port_hw_state_set(port,
							FSL_INET_STP_DISABLED);
		if (ret) {
			fsl91030m_swdev_bridge_join_rollback(port, true,
							     "bridge join rollback");
			NL_SET_ERR_MSG_MOD(extack,
					   "failed to program bridge port state");
			return ret;
		}
	}

	fsl91030m_swdev_port_bridge_joined_set(port, true);
	ret = fsl91030m_swdev_port_hw_state_sync(port);
	if (ret) {
		fsl91030m_swdev_bridge_join_rollback(port, true,
						     "bridge join rollback");
		NL_SET_ERR_MSG_MOD(extack,
				   "failed to program bridge port state");
		return ret;
	}

	/* Re-run bridge replay now that the verified hardware gates are active. */
	ret = switchdev_bridge_port_replay(port->ndev, port->ndev, port,
					   &sd->switchdev_nb,
					   &sd->switchdev_blocking_nb,
					   extack);
	if (ret) {
		fsl91030m_swdev_bridge_join_rollback(port, true,
						     "bridge join replay rollback");
		return ret;
	}

	return 0;
}

static void fsl91030m_swdev_bridge_leave(struct fsl91030m_swdev_port *port)
{
	struct fsl91030m_switchdev *sd = port->sd;
	int ret;

	/*
	 * Symmetric with bridge_join (and with the join rollback path): a port
	 * that never completed an offloaded join owns no bridge membership,
	 * CPU-trap, ageing, or switchdev-offload state.  Tearing that state down
	 * here would clear shared bridge_dev/bridge_members/ageing state that
	 * still belongs to the other joined ports, so a leave for a non-joined
	 * port must be a no-op.
	 */
	if (!fsl91030m_swdev_port_bridge_joined(port))
		return;

	fsl91030m_swdev_port_bridge_joined_set(port, false);
	fsl91030m_swdev_vlan1107_clear(port, "bridge leave");

	ret = fsl91030m_swdev_port_bridge_member_set(port, false);
	if (ret && net_ratelimit())
		netdev_err(port->ndev,
			   "failed to disable bridge membership gate on bridge leave: %d\n",
			   ret);

	ret = fsl91030m_swdev_port_hw_state_set(port, FSL_INET_STP_DISABLED);
	if (ret && net_ratelimit())
		netdev_err(port->ndev,
			   "failed to block bridge port state on bridge leave: %d\n",
			   ret);

	switchdev_bridge_port_unoffload(port->ndev, port,
					&sd->switchdev_nb,
					&sd->switchdev_blocking_nb);

	sd->bridge_members &= ~BIT(port->index);
	if (!sd->bridge_members) {
		sd->bridge_dev = NULL;
		ret = fsl91030m_swdev_cpu_trap_mac_sync(sd);
		if (ret && net_ratelimit())
			netdev_err(port->ndev,
				   "failed to clear CPU trap MAC on bridge leave: %d\n",
				   ret);

		ret = fsl91030m_swdev_ageing_seconds_set(sd,
							 FSL91030M_SWDEV_AGEING_DEFAULT,
							 NULL);
		if (ret && net_ratelimit())
			netdev_err(port->ndev,
				   "failed to restore default ageing time on bridge leave: %d\n",
				   ret);
	}
}

static int fsl91030m_swdev_netdevice_event(struct notifier_block *nb,
					   unsigned long event, void *ptr)
{
	struct fsl91030m_switchdev *sd =
		container_of(nb, struct fsl91030m_switchdev, netdevice_nb);
	struct net_device *ndev = netdev_notifier_info_to_dev(ptr);
	struct netlink_ext_ack *extack = netdev_notifier_info_to_extack(ptr);
	struct fsl91030m_swdev_port *port;
	struct netdev_notifier_changeupper_info *info;
	bool handled = false;
	int ret = 0;

	if (READ_ONCE(sd->unregistering))
		return NOTIFY_DONE;

	if (event == NETDEV_CHANGEADDR && ndev == sd->bridge_dev) {
		ret = fsl91030m_swdev_cpu_trap_mac_sync(sd);
		if (ret && net_ratelimit())
			netdev_err(ndev,
				   "failed to update CPU trap MAC after bridge address change: %d\n",
				   ret);
		return notifier_from_errno(ret);
	}

	port = fsl91030m_swdev_port_from_dev_for_sd(sd, ndev);
	if (!port)
		return NOTIFY_DONE;

	switch (event) {
	case NETDEV_PRECHANGEUPPER:
		info = ptr;
		if (!info->upper_dev || !info->linking)
			break;

		handled = true;
		ret = fsl91030m_swdev_bridge_join_validate(port, info->upper_dev,
							   extack);
		break;
	case NETDEV_CHANGEUPPER:
		info = ptr;
		if (!info->upper_dev || !netif_is_bridge_master(info->upper_dev))
			break;

		if (info->linking) {
			handled = true;
			ret = fsl91030m_swdev_bridge_join(port,
							  info->upper_dev,
							  extack);
		} else if (fsl91030m_swdev_port_bridge_joined(port) &&
			   info->upper_dev == port->sd->bridge_dev) {
			handled = true;
			fsl91030m_swdev_bridge_leave(port);
		}
		break;
	default:
		break;
	}

	if (!handled)
		return NOTIFY_DONE;

	return notifier_from_errno(ret);
}

static int fsl91030m_swdev_alloc_port(struct fsl91030m_switchdev *sd,
				      const struct fsl91030m_port_desc *desc,
				      unsigned int index)
{
	struct fsl91030m_swdev_port *port;
	struct net_device *ndev;
	u8 addr[ETH_ALEN];
	int ret;

	ndev = alloc_netdev(sizeof(*port), desc->name, NET_NAME_PREDICTABLE,
			    ether_setup);
	if (!ndev)
		return -ENOMEM;

	port = netdev_priv(ndev);
	port->sd = sd;
	port->desc = desc;
	port->ndev = ndev;
	port->index = index;
	spin_lock_init(&port->stats_lock);

	SET_NETDEV_DEV(ndev, sd->sw->dev);
	ndev->netdev_ops = &fsl91030m_swdev_netdev_ops;
	/*
	 * G5..G12 are switchdev control netdevs. Hardware MTU programming is not
	 * provided for these ports, so keep the visible MTU fixed at the
	 * verified Ethernet payload size.
	 */
	ndev->min_mtu = ETH_DATA_LEN;
	ndev->max_mtu = ETH_DATA_LEN;
	ndev->priv_flags |= IFF_NO_QUEUE;
	ndev->features |= NETIF_F_HW_TC;
	ndev->hw_features |= NETIF_F_HW_TC;
	netif_carrier_off(ndev);
	memcpy(addr, fsl91030m_swdev_addr_prefix, sizeof(addr));
	addr[FSL91030M_SWDEV_ADDR_LPORT_OFF] = desc->lport;
	eth_hw_addr_set(ndev, addr);

	sd->ports[index] = port;
	ret = fsl91030m_swdev_port_block(port);
	if (ret) {
		sd->ports[index] = NULL;
		free_netdev(ndev);
		return ret;
	}

	ret = register_netdev(ndev);
	if (ret) {
		sd->ports[index] = NULL;
		free_netdev(ndev);
		return ret;
	}

	ret = xy1000_pdma_register_lport(desc->lport, ndev);
	if (ret) {
		unregister_netdev(ndev);
		sd->ports[index] = NULL;
		free_netdev(ndev);
		return ret;
	}

	return 0;
}

int fsl91030m_switchdev_register(struct fsl91030m *sw)
{
	struct fsl91030m_switchdev *sd;
	unsigned int i;
	int ret;

	if (!sw || !sw->dev || !sw->full)
		return -ENODEV;

	sd = devm_kzalloc(sw->dev, sizeof(*sd), GFP_KERNEL);
	if (!sd)
		return -ENOMEM;

	sd->sw = sw;
	sd->switchdev_nb.notifier_call = fsl91030m_swdev_switchdev_event;
	sd->switchdev_blocking_nb.notifier_call =
		fsl91030m_swdev_switchdev_blocking_event;
	sd->netdevice_nb.notifier_call = fsl91030m_swdev_netdevice_event;
	sd->ageing_time = FSL91030M_SWDEV_AGEING_DEFAULT;
	INIT_DELAYED_WORK(&sd->stats_work, fsl91030m_swdev_stats_work);
	fsl91030m_swdev_parent_id_init(sd);
	sw->swdev = sd;

	ret = register_switchdev_notifier(&sd->switchdev_nb);
	if (ret)
		goto err_clear;

	ret = register_switchdev_blocking_notifier(&sd->switchdev_blocking_nb);
	if (ret)
		goto err_switchdev_nb;

	ret = register_netdevice_notifier(&sd->netdevice_nb);
	if (ret)
		goto err_switchdev_blocking_nb;

	for (i = 0; i < FSL91030M_SWDEV_NPORTS; i++) {
		const struct fsl91030m_port_desc *desc;

		desc = fsl91030m_port_get(i);
		if (!desc) {
			ret = -ENODEV;
			goto err_ports;
		}

		ret = fsl91030m_swdev_alloc_port(sd, desc, i);
		if (ret)
			goto err_ports;
	}

	WRITE_ONCE(sd->stats_active, true);
	schedule_delayed_work(&sd->stats_work, 0);

	return 0;

err_ports:
	fsl91030m_switchdev_unregister(sw);
	return ret;
err_switchdev_blocking_nb:
	unregister_switchdev_blocking_notifier(&sd->switchdev_blocking_nb);
err_switchdev_nb:
	unregister_switchdev_notifier(&sd->switchdev_nb);
err_clear:
	sw->swdev = NULL;
	return ret;
}

void fsl91030m_switchdev_unregister(struct fsl91030m *sw)
{
	struct fsl91030m_switchdev *sd;
	unsigned int i;

	if (!sw)
		return;

	sd = sw->swdev;
	if (!sd)
		return;

	WRITE_ONCE(sd->unregistering, true);
	WRITE_ONCE(sd->stats_active, false);
	cancel_delayed_work_sync(&sd->stats_work);

	rtnl_lock();
	for (i = 0; i < FSL91030M_SWDEV_NPORTS; i++) {
		if (!sd->ports[i])
			continue;

		if (fsl91030m_swdev_port_bridge_joined(sd->ports[i]))
			fsl91030m_swdev_bridge_leave(sd->ports[i]);
	}
	rtnl_unlock();

	unregister_netdevice_notifier(&sd->netdevice_nb);
	unregister_switchdev_blocking_notifier(&sd->switchdev_blocking_nb);
	unregister_switchdev_notifier(&sd->switchdev_nb);

	for (i = 0; i < FSL91030M_SWDEV_NPORTS; i++) {
		if (sd->ports[i] && sd->ports[i]->ndev) {
			/*
			 * Unregister the netdev first: if the port is still
			 * administratively up, unregister_netdev() runs
			 * ndo_stop() -> xy1000_pdma_lport_stop(), which only
			 * drops the packet-DMA reference while the lport is
			 * still registered.  Clearing the lport registration
			 * before unregister_netdev() would make lport_stop()
			 * skip its hw_put() and leak the DMA-engine refcount.
			 */
			unregister_netdev(sd->ports[i]->ndev);
			xy1000_pdma_unregister_lport(sd->ports[i]->desc->lport,
						     sd->ports[i]->ndev);
		}
	}

	for (i = 0; i < FSL91030M_SWDEV_NPORTS; i++) {
		if (sd->ports[i] && sd->ports[i]->ndev) {
			free_netdev(sd->ports[i]->ndev);
			sd->ports[i] = NULL;
		}
	}

	sw->swdev = NULL;
}
