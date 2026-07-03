/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * FisiLink FSL91030M L2 Ethernet switch support for the Milk-V Vega RJ45
 * G5..G12 datapath, based on the FSL91030M register reference and hardware
 * readback.
 *
 * Addressing:
 *   The register book lists subsystem base addresses.  CPU-direct MMIO uses
 *   those bases ORed with the 0x60000000 CPU aperture bit, e.g. traffic-read
 *   at 0x66800000.
 *   All accesses are 32-bit word aligned.
 */
#ifndef _FSL91030M_H_
#define _FSL91030M_H_

#include <linux/bits.h>
#include <linux/build_bug.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include "fsl91030m-regs.h"

#define FSL_TBL_MAXW 16
#define FSL91030M_PORT_ID_MASK GENMASK(5, 0)
#define FSL91030M_PORT_ID_INVALID 63u
#define FSL91030M_DEFAULT_FID 0u
#define FSL91030M_AGEING_DISABLE_SEC 0u
#define FSL91030M_AGEING_MIN_SEC 1u
#define FSL91030M_AGEING_MAX_SEC 3600u
#define FSL91030M_SWITCH_WINDOW_SIZE 0x07100000u

struct rtnl_link_stats64;
struct net_device;
struct fsl91030m_switchdev;
enum tc_setup_type;

enum {
	FSL91030M_VEGA_PORT_G5,
	FSL91030M_VEGA_PORT_G6,
	FSL91030M_VEGA_PORT_G7,
	FSL91030M_VEGA_PORT_G8,
	FSL91030M_VEGA_PORT_G9,
	FSL91030M_VEGA_PORT_G10,
	FSL91030M_VEGA_PORT_G11,
	FSL91030M_VEGA_PORT_G12,
	FSL91030M_VEGA_PORT_COUNT,
};

/* VID1107 CTAG service can be offloaded on any subset of the copper ports. */
#define FSL91030M_VLAN_CTAGSVC_PORTS FSL91030M_VEGA_PORT_COUNT
#define FSL91030M_CTAG_SERVICE_VID 1107u

#define FSL91030M_VEGA_G5_MAC		16u
#define FSL91030M_VEGA_G5_LPORT		6u
#define FSL91030M_VEGA_G5_PHY		16u
#define FSL91030M_VEGA_G6_MAC		17u
#define FSL91030M_VEGA_G6_LPORT		7u
#define FSL91030M_VEGA_G6_PHY		17u
#define FSL91030M_VEGA_G7_MAC		18u
#define FSL91030M_VEGA_G7_LPORT		8u
#define FSL91030M_VEGA_G7_PHY		18u
#define FSL91030M_VEGA_G8_MAC		19u
#define FSL91030M_VEGA_G8_LPORT		9u
#define FSL91030M_VEGA_G8_PHY		19u
#define FSL91030M_VEGA_G9_MAC		20u
#define FSL91030M_VEGA_G9_LPORT		10u
#define FSL91030M_VEGA_G9_PHY		20u
#define FSL91030M_VEGA_G10_MAC		21u
#define FSL91030M_VEGA_G10_LPORT	11u
#define FSL91030M_VEGA_G10_PHY		21u
#define FSL91030M_VEGA_G11_MAC		22u
#define FSL91030M_VEGA_G11_LPORT	12u
#define FSL91030M_VEGA_G11_PHY		22u
#define FSL91030M_VEGA_G12_MAC		23u
#define FSL91030M_VEGA_G12_LPORT	13u
#define FSL91030M_VEGA_G12_PHY		23u
#define FSL91030M_VEGA_TR_SLOT_COUNT	36u
#define FSL91030M_VEGA_CPU_LPORT	31u

struct fsl91030m_port_desc {
	const char *name;
	u8 mac;
	u8 lport;
	u8 phy;
};

struct fsl91030m_vlan_ctag_ivt_saved {
	bool valid;
	u8 lport;
	u8 bank;
	u8 idx;
	u32 port_row[FSL_IVT_PORT_SRM_WORDS];
	u32 key_row[FSL_IVT_XLATE_SRM_WORDS];
	u32 action_row[FSL_IVT_XLATE_SRM_WORDS];
};

struct fsl91030m_vlan_ctag_saved {
	bool active;
	u8 n_members;
	u8 members[FSL91030M_VLAN_CTAGSVC_PORTS];
	u16 member_mask;
	u16 untagged_mask;
	u32 inet_vlan[FSL_INET_VLAN_SRM_WORDS];
	u32 inet_port[FSL91030M_VLAN_CTAGSVC_PORTS][FSL_INET_PORT_SRM_WORDS];
	u32 eee_vlan_op[FSL_EEE_VLAN_OP_SRM_WORDS];
	u32 eee_port[FSL91030M_VLAN_CTAGSVC_PORTS][FSL_EEE_PORT_SRM_WORDS];
	u32 ivt_keyctl;
	struct fsl91030m_vlan_ctag_ivt_saved ivt[FSL91030M_VLAN_CTAGSVC_PORTS];
};

struct fsl91030m_qos_red_state {
	bool active;
	u32 handle;
	u32 parent;
};

struct fsl91030m_qos_state {
	/* One RED-offload tracking slot per copper port (G5..G12). */
	struct fsl91030m_qos_red_state red[FSL91030M_VEGA_PORT_COUNT];
};

struct fsl91030m {
	struct device	*dev;
	void __iomem	*full;		/* CPU-direct switch window from DT */
	resource_size_t	full_size;	/* helper-accessible switch window */
	resource_size_t	switch_phys;	/* physical switch-window base */
	/*
	 * SoC DMA engine for SRM/table access.  Wide table entries
	 * are written/read via the DMA engine as a 64-bit burst; direct 32-bit
	 * stores do not commit them.  The fixed scratch window is the supported
	 * DMA buffer.
	 */
	void __iomem	*dma_regs;
	void __iomem	*dma_vbuf;
	u32		dma_vbuf_phys;
	struct mutex	dma_lock;	/* serializes use of the TDMA scratch buffer */
	struct mutex	op_lock;	/* serializes fabric transactions */
	struct mutex	age_lock;	/* serializes ageing timer updates */
	struct fsl91030m_vlan_ctag_saved vlan1107;
	struct fsl91030m_qos_state qos;
	struct fsl91030m_switchdev *swdev;
};

int __must_check fsl91030m_direct_read32(struct fsl91030m *sw, u32 off,
					 u32 *val);
int __must_check fsl91030m_direct_write32(struct fsl91030m *sw, u32 off,
					  u32 val);

static_assert(FSL_INET_DEF_VLAN_MAX_LPORT +
	      FSL_INET_DEF_VLAN_MEMBER_LPORT_SHIFT <
	      sizeof(u32) * BITS_PER_BYTE,
	      "default VLAN membership bits must fit the 32-bit register");

static inline u32 fsl91030m_l2_default_membership_bit(u8 lport)
{
	return BIT(lport + FSL_INET_DEF_VLAN_MEMBER_LPORT_SHIFT);
}

int __must_check fsl91030m_table_read(struct fsl91030m *sw, u32 off,
				      u32 *words, unsigned int count);
int __must_check fsl91030m_table_write_verify_exact(struct fsl91030m *sw,
						    u32 off, const u32 *want,
						    u32 *after,
						    unsigned int count);
int __must_check fsl91030m_direct_write_verify_exact(struct fsl91030m *sw,
						     u32 off,
						     const u32 *want,
						     u32 *after,
						     unsigned int count);

int __must_check fsl91030m_l2_bridge_port_state_set(struct fsl91030m *sw,
						    u8 lport, u8 state);
int __must_check fsl91030m_l2_bridge_membership_set(struct fsl91030m *sw,
						    u8 lport, bool member);
int __must_check fsl91030m_l2_ageing_time_set(struct fsl91030m *sw,
					      u32 seconds);
int __must_check fsl91030m_l2_cpu_broadcast_trap_set(struct fsl91030m *sw,
						     bool enable);
int __must_check fsl91030m_l2_cpu_local_mac_set(struct fsl91030m *sw,
						const u8 *mac, bool enable);
/*
 * Program the VID1107 CTAG service across the copper ports named in
 * @member_mask (bit i == copper port i, lport FSL91030M_VEGA_G5_LPORT + i);
 * @untagged_mask (a subset of @member_mask) selects per-egress CTAG stripping.
 * The offload activates once at least two ports are members.
 */
int __must_check fsl91030m_l2_vlan1107_set(struct fsl91030m *sw,
					   unsigned int member_mask,
					   unsigned int untagged_mask);
int __must_check fsl91030m_port_hw_stats_read(struct fsl91030m *sw,
					      const struct fsl91030m_port_desc *desc,
					      struct rtnl_link_stats64 *stats);
int __must_check fsl91030m_board_init_apply(struct fsl91030m *sw);
int __must_check fsl91030m_switchdev_register(struct fsl91030m *sw);
void fsl91030m_switchdev_unregister(struct fsl91030m *sw);
bool fsl91030m_switchdev_resolve_netdev(const struct net_device *ndev,
					struct fsl91030m **sw,
					const struct fsl91030m_port_desc **desc);
int __must_check fsl91030m_qos_setup_tc(struct fsl91030m *sw,
					const struct fsl91030m_port_desc *desc,
					enum tc_setup_type type,
					void *type_data);

const struct fsl91030m_port_desc *fsl91030m_port_get(unsigned int index);
bool fsl91030m_port_lport_supported(u8 lport);

#endif /* _FSL91030M_H_ */
