// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fixed board initialization for the FSL91030M fabric on Milk-V Vega.
 */
#include <linux/bitfield.h>
#include <linux/build_bug.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/limits.h>

#include "fsl91030m.h"

#define FSL_VEGA_TOP_SDXG_EN_VALUE	3u
#define FSL_VEGA_TOP_SD1G_EN_VALUE	1u
#define FSL_VEGA_TOP_HUB_MODE_VALUE	5u
#define FSL_VEGA_TOP_BLINK_RATE_VALUE	3u
#define FSL_VEGA_RGMII_STATE_VALUE	2u
#define FSL_VEGA_RGMII_SPEED_VALUE	3u
#define FSL_VEGA_RGMII_RXDELAY_VALUE	3u
#define FSL_VEGA_RESET_ASSERT_MS		1000
#define FSL_VEGA_RESET_RELEASE_MS	1000
#define FSL_ETH_XSGMII4_MODE_CTRL \
	(FSL_ETH_XSGMII_BASE(4) + ETH_XSGMII_MODE_CTRL)
#define FSL_ETH_XSGMII5_MODE_CTRL \
	(FSL_ETH_XSGMII_BASE(5) + ETH_XSGMII_MODE_CTRL)
#define FSL_VEGA_TOP_WORK_MODE \
	(FIELD_PREP_CONST(TOP_WORK_SDXG0_EN, FSL_VEGA_TOP_SDXG_EN_VALUE) | \
	 FIELD_PREP_CONST(TOP_WORK_SDXG1_EN, FSL_VEGA_TOP_SDXG_EN_VALUE) | \
	 TOP_WORK_LED_ACTIVE_LOW | \
	 FIELD_PREP_CONST(TOP_WORK_BLINK_RATE, \
			  FSL_VEGA_TOP_BLINK_RATE_VALUE) | \
	 TOP_WORK_SERIAL_DATA_EN | \
	 TOP_WORK_SERIAL_CLK_EN)
#define FSL_VEGA_TOP_PCS_SWITCH \
	(FIELD_PREP_CONST(TOP_PCS_SD1G_EN0, FSL_VEGA_TOP_SD1G_EN_VALUE) | \
	 FIELD_PREP_CONST(TOP_PCS_SD1G_EN1, FSL_VEGA_TOP_SD1G_EN_VALUE) | \
	 FIELD_PREP_CONST(TOP_PCS_SD1G_EN2, FSL_VEGA_TOP_SD1G_EN_VALUE) | \
	 FIELD_PREP_CONST(TOP_PCS_SD1G_EN3, FSL_VEGA_TOP_SD1G_EN_VALUE) | \
	 FIELD_PREP_CONST(TOP_PCS_HUB_MODE, FSL_VEGA_TOP_HUB_MODE_VALUE))
#define FSL_VEGA_TOP_RGMII_CSR \
	(FIELD_PREP_CONST(TOP_RGMII_XMII_STATE, \
			  FSL_VEGA_RGMII_STATE_VALUE) | \
	 FIELD_PREP_CONST(TOP_RGMII_XMII_SPEED, \
			  FSL_VEGA_RGMII_SPEED_VALUE) | \
	 FIELD_PREP_CONST(TOP_RGMII_XMII_RXDELAY, \
			  FSL_VEGA_RGMII_RXDELAY_VALUE))
#define FSL_VEGA_XSGMII_MODE \
	(ETH_XSGMII_MODE_MASK_RD_ERROR | \
	 FIELD_PREP_CONST(ETH_XSGMII_MODE_PCS_MODE, \
			  ETH_XSGMII_MODE_PCS_USGMII))
#define FSL_TW_MAC_COUNT		30

/*
 * Fixed Vega board-active fabric map. The RJ45 G5..G12 range is exposed to
 * Linux; the traffic-read/write tables also keep the wider verified board
 * baseline for the complete fabric to converge.
 */
#define FSL_VEGA_FABRIC_MGMT0_MAC	0u
#define FSL_VEGA_FABRIC_MGMT0_LPORT	1u
#define FSL_VEGA_FABRIC_MGMT0_PHY	0u
#define FSL_VEGA_FABRIC_MGMT1_MAC	8u
#define FSL_VEGA_FABRIC_MGMT1_LPORT	0u
#define FSL_VEGA_FABRIC_MGMT1_PHY	8u
#define FSL_VEGA_SERDES0_MAC		24u
#define FSL_VEGA_SERDES0_LPORT		4u
#define FSL_VEGA_SERDES0_PHY		24u
#define FSL_VEGA_SERDES1_MAC		25u
#define FSL_VEGA_SERDES1_LPORT		5u
#define FSL_VEGA_SERDES1_PHY		25u
#define FSL_VEGA_SERDES2_MAC		26u
#define FSL_VEGA_SERDES2_LPORT		2u
#define FSL_VEGA_SERDES2_PHY		26u
#define FSL_VEGA_SERDES3_MAC		27u
#define FSL_VEGA_SERDES3_LPORT		3u
#define FSL_VEGA_SERDES3_PHY		27u

/*
 * Fixed Vega traffic-schedule board init for the lport 0..13 baseline. The
 * G5..G12 datapath needs these scheduler and shaper rows as part of the
 * verified board baseline. Runtime QoS controls remain limited to the paths
 * that have separate packet validation.
 * Program them once and read them back before registering the switchdev ports.
 */
#define FSL_VEGA_TSCHED_FIRST_LPORT	0u
#define FSL_VEGA_TSCHED_LAST_LPORT	FSL91030M_VEGA_G12_LPORT
#define FSL_VEGA_TSCHED_LPORT_COUNT	(FSL_VEGA_TSCHED_LAST_LPORT + 1u)
#define FSL_VEGA_TSCHED_SHP_INTV1	0x494u
#define FSL_VEGA_TSCHED_SHP_INTV1_NUM	7u
#define FSL_VEGA_TSCHED_SHP_INTV0	0x493u
#define FSL_VEGA_TSCHED_SHP_INTV0_NUM	1u
#define FSL_VEGA_TSCHED_PORT_FILLRATE	0x1fffffu
#define FSL_VEGA_TSCHED_PORT_MAXSIZE	2000u
#define FSL_VEGA_TSCHED_PORT_QUANTUM	2u
#define FSL_VEGA_TSCHED_WRR_QUANTUM	0u
#define FSL_VEGA_TSCHED_WRR_PRIORITY	1u
#define FSL_VEGA_TSCHED_SCH_BMP		0u
#define FSL_VEGA_TSCHED_Q0_WEIGHT	1u
#define FSL_VEGA_TSCHED_Q1_WEIGHT	2u
#define FSL_VEGA_TSCHED_Q2_WEIGHT	3u
#define FSL_VEGA_TSCHED_Q3_WEIGHT	4u
#define FSL_VEGA_TSCHED_Q4_WEIGHT	5u
#define FSL_VEGA_TSCHED_Q5_WEIGHT	6u
#define FSL_VEGA_TSCHED_Q6_WEIGHT	7u
#define FSL_VEGA_TSCHED_Q7_WEIGHT	8u
#define FSL_VEGA_TSCHED_SHPUPD1_WORD \
	(FIELD_PREP_CONST(FSL_TS_SHPUPD_INTERVAL, \
			  FSL_VEGA_TSCHED_SHP_INTV1) | \
	 FIELD_PREP_CONST(FSL_TS_SHPUPD_NUM, \
			  FSL_VEGA_TSCHED_SHP_INTV1_NUM))
#define FSL_VEGA_TSCHED_SHPUPD0_WORD \
	(FIELD_PREP_CONST(FSL_TS_SHPUPD_INTERVAL, \
			  FSL_VEGA_TSCHED_SHP_INTV0) | \
	 FIELD_PREP_CONST(FSL_TS_SHPUPD_NUM, \
			  FSL_VEGA_TSCHED_SHP_INTV0_NUM))
#define FSL_VEGA_TSCHED_PORT_RATE_WORD \
	FIELD_PREP_CONST(FSL_TS_PORT_SHP_FILLRATE, \
			 FSL_VEGA_TSCHED_PORT_FILLRATE)
#define FSL_VEGA_TSCHED_PORT_CTL_WORD \
	(FIELD_PREP_CONST(FSL_TS_PORT_SHP_MAXSIZE, \
			  FSL_VEGA_TSCHED_PORT_MAXSIZE) | \
	 FIELD_PREP_CONST(FSL_TS_PORT_SHP_QUANTUM, \
			  FSL_VEGA_TSCHED_PORT_QUANTUM))
#define FSL_VEGA_TSCHED_QUE_WEIGHT0123_WORD \
	(FIELD_PREP_CONST(FSL_TS_QUE_SCH_Q0_WEIGHT, \
			  FSL_VEGA_TSCHED_Q0_WEIGHT) | \
	 FIELD_PREP_CONST(FSL_TS_QUE_SCH_Q1_WEIGHT, \
			  FSL_VEGA_TSCHED_Q1_WEIGHT) | \
	 FIELD_PREP_CONST(FSL_TS_QUE_SCH_Q2_WEIGHT, \
			  FSL_VEGA_TSCHED_Q2_WEIGHT) | \
	 FIELD_PREP_CONST(FSL_TS_QUE_SCH_Q3_WEIGHT, \
			  FSL_VEGA_TSCHED_Q3_WEIGHT))
#define FSL_VEGA_TSCHED_QUE_WEIGHT4567_WORD \
	(FIELD_PREP_CONST(FSL_TS_QUE_SCH_Q4_WEIGHT, \
			  FSL_VEGA_TSCHED_Q4_WEIGHT) | \
	 FIELD_PREP_CONST(FSL_TS_QUE_SCH_Q5_WEIGHT, \
			  FSL_VEGA_TSCHED_Q5_WEIGHT) | \
	 FIELD_PREP_CONST(FSL_TS_QUE_SCH_Q6_WEIGHT, \
			  FSL_VEGA_TSCHED_Q6_WEIGHT) | \
	 FIELD_PREP_CONST(FSL_TS_QUE_SCH_Q7_WEIGHT, \
			  FSL_VEGA_TSCHED_Q7_WEIGHT))
#define FSL_VEGA_TSCHED_QUE_CTL_WORD \
	(FIELD_PREP_CONST(FSL_TS_QUE_SCH_WRR_QUANTUM, \
			  FSL_VEGA_TSCHED_WRR_QUANTUM) | \
	 FIELD_PREP_CONST(FSL_TS_QUE_SCH_WRR_PRI, \
			  FSL_VEGA_TSCHED_WRR_PRIORITY) | \
	 FSL_TS_QUE_SCH_MODE_DWRR | \
	 FIELD_PREP_CONST(FSL_TS_QUE_SCH_BMP, FSL_VEGA_TSCHED_SCH_BMP))

struct fsl_init_fabric_port {
	u8 mac;
	u8 lport;
	u8 phy;
};

static const struct fsl_init_fabric_port fsl_vega_fabric_ports[] = {
	{
		.mac = FSL_VEGA_FABRIC_MGMT0_MAC,
		.lport = FSL_VEGA_FABRIC_MGMT0_LPORT,
		.phy = FSL_VEGA_FABRIC_MGMT0_PHY,
	},
	{
		.mac = FSL_VEGA_FABRIC_MGMT1_MAC,
		.lport = FSL_VEGA_FABRIC_MGMT1_LPORT,
		.phy = FSL_VEGA_FABRIC_MGMT1_PHY,
	},
	{
		.mac = FSL91030M_VEGA_G5_MAC,
		.lport = FSL91030M_VEGA_G5_LPORT,
		.phy = FSL91030M_VEGA_G5_PHY,
	},
	{ .mac = FSL91030M_VEGA_G6_MAC, .lport = FSL91030M_VEGA_G6_LPORT,
	  .phy = FSL91030M_VEGA_G6_PHY },
	{ .mac = FSL91030M_VEGA_G7_MAC, .lport = FSL91030M_VEGA_G7_LPORT,
	  .phy = FSL91030M_VEGA_G7_PHY },
	{ .mac = FSL91030M_VEGA_G8_MAC, .lport = FSL91030M_VEGA_G8_LPORT,
	  .phy = FSL91030M_VEGA_G8_PHY },
	{ .mac = FSL91030M_VEGA_G9_MAC, .lport = FSL91030M_VEGA_G9_LPORT,
	  .phy = FSL91030M_VEGA_G9_PHY },
	{ .mac = FSL91030M_VEGA_G10_MAC, .lport = FSL91030M_VEGA_G10_LPORT,
	  .phy = FSL91030M_VEGA_G10_PHY },
	{ .mac = FSL91030M_VEGA_G11_MAC, .lport = FSL91030M_VEGA_G11_LPORT,
	  .phy = FSL91030M_VEGA_G11_PHY },
	{
		.mac = FSL91030M_VEGA_G12_MAC,
		.lport = FSL91030M_VEGA_G12_LPORT,
		.phy = FSL91030M_VEGA_G12_PHY,
	},
	{ .mac = FSL_VEGA_SERDES0_MAC, .lport = FSL_VEGA_SERDES0_LPORT,
	  .phy = FSL_VEGA_SERDES0_PHY },
	{ .mac = FSL_VEGA_SERDES1_MAC, .lport = FSL_VEGA_SERDES1_LPORT,
	  .phy = FSL_VEGA_SERDES1_PHY },
	{ .mac = FSL_VEGA_SERDES2_MAC, .lport = FSL_VEGA_SERDES2_LPORT,
	  .phy = FSL_VEGA_SERDES2_PHY },
	{ .mac = FSL_VEGA_SERDES3_MAC, .lport = FSL_VEGA_SERDES3_LPORT,
	  .phy = FSL_VEGA_SERDES3_PHY },
};

#define FSL_INIT_WORD_END(off)		((off) + sizeof(u32))
#define FSL_INIT_ROW_END(base, last, stride, words) \
	((base) + (last) * (stride) + (words) * sizeof(u32))
#define FSL_INIT_TW_PORT_MAP_WORD_COUNT \
	DIV_ROUND_UP(FSL_TW_MAC_COUNT, FSL_TW_PORT_MAP_FIELDS_PER_WORD)
#define FSL_INIT_INDEX_FITS(index, count)	((index) < (count))
#define FSL_INIT_VALUE_FITS(value, max)		((value) <= (max))
#define FSL_INIT_WORDS_FIT(words)		((words) <= FSL_TBL_MAXW)
#define FSL_INIT_U32_MASK			U32_MAX

static_assert(FSL91030M_PORT_ID_MASK <= U8_MAX,
	      "FSL91030M port IDs must fit driver port descriptors");
static_assert(FSL91030M_PORT_ID_INVALID <= FSL91030M_PORT_ID_MASK,
	      "invalid port marker must fit hardware port fields");
static_assert(FSL_TW_MAC_COUNT,
	      "traffic-write MAC map must be nonempty");
static_assert(ARRAY_SIZE(fsl_vega_fabric_ports),
	      "Vega fixed fabric map must be nonempty");
static_assert(FSL_TR_PORT_MAP_WORDS && FSL_TR_SLOT_CFG_WORDS,
	      "traffic-read maps must be nonempty");
static_assert(FSL_TW_PORT_MAP_FIELDS_PER_WORD &&
	      sizeof(u32) * BITS_PER_BYTE >=
	      FSL_TW_PORT_MAP_FIELDS_PER_WORD *
	      FSL_TW_PORT_MAP_FIELD_BITS,
	      "traffic-write port map fields must fit one register word");
static_assert(BIT(FSL_TW_PORT_MAP_FIELD_BITS) > FSL91030M_PORT_ID_MASK,
	      "traffic-write port map field must hold one port ID");
static_assert(FSL_INIT_INDEX_FITS(FSL91030M_VEGA_G5_MAC, FSL_TW_MAC_COUNT) &&
	      FSL_INIT_INDEX_FITS(FSL91030M_VEGA_G12_MAC, FSL_TW_MAC_COUNT),
	      "Vega MAC IDs must fit the traffic-write port map");
static_assert(FSL_INIT_INDEX_FITS(FSL_VEGA_FABRIC_MGMT1_MAC,
				  FSL_TW_MAC_COUNT) &&
	      FSL_INIT_INDEX_FITS(FSL91030M_VEGA_G11_MAC,
				  FSL_TW_MAC_COUNT) &&
	      FSL_INIT_INDEX_FITS(FSL_VEGA_SERDES3_MAC, FSL_TW_MAC_COUNT),
	      "Vega fixed-fabric MAC IDs must fit the traffic-write port map");
static_assert(FSL_INIT_INDEX_FITS(FSL91030M_VEGA_G5_LPORT,
				  FSL_TR_PORT_MAP_WORDS) &&
	      FSL_INIT_INDEX_FITS(FSL91030M_VEGA_G12_LPORT,
				  FSL_TR_PORT_MAP_WORDS),
	      "Vega logical ports must fit the traffic-read port map");
static_assert(FSL_INIT_INDEX_FITS(FSL91030M_VEGA_G11_LPORT,
				  FSL_TR_PORT_MAP_WORDS) &&
	      FSL_INIT_INDEX_FITS(FSL_VEGA_SERDES1_LPORT,
				  FSL_TR_PORT_MAP_WORDS),
	      "Vega fixed-fabric lports must fit the traffic-read port map");
static_assert(FSL_INIT_VALUE_FITS(FSL91030M_VEGA_G5_LPORT,
				  FSL_INET_DEF_VLAN_MAX_LPORT) &&
	      FSL_INIT_VALUE_FITS(FSL91030M_VEGA_G12_LPORT,
				  FSL_INET_DEF_VLAN_MAX_LPORT),
	      "Vega logical ports must fit the default VLAN membership map");
static_assert(FSL91030M_VEGA_TR_SLOT_COUNT <= FSL_TR_SLOT_CFG_WORDS,
	      "Vega traffic-read slot count must fit the slot table");
static_assert(FSL91030M_VEGA_TR_SLOT_COUNT <= FSL91030M_PORT_ID_MASK,
	      "Vega traffic-read slot count must fit slot-count field");
static_assert(FSL_INIT_WORDS_FIT(FSL_TS_PORT_SHP_WORDS) &&
	      FSL_INIT_WORDS_FIT(FSL_TS_QUE_SCH_WORDS),
	      "traffic-schedule rows must fit table access buffers");
static_assert(FSL_VEGA_TSCHED_FIRST_LPORT <= FSL_VEGA_TSCHED_LAST_LPORT,
	      "traffic-schedule lport range must be ordered");
static_assert(FSL_VEGA_TSCHED_LAST_LPORT <= FSL91030M_PORT_ID_MASK,
	      "traffic-schedule lport range must fit hardware port IDs");
static_assert(FSL_TS_PORT_SHP_STRIDE >=
	      FSL_TS_PORT_SHP_WORDS * sizeof(u32) &&
	      !(FSL_TS_PORT_SHP_STRIDE & (sizeof(u32) - 1)),
	      "traffic-schedule port shaper rows must be word-aligned");
static_assert(FSL_TS_QUE_SCH_STRIDE >=
	      FSL_TS_QUE_SCH_WORDS * sizeof(u32) &&
	      !(FSL_TS_QUE_SCH_STRIDE & (sizeof(u32) - 1)),
	      "traffic-schedule queue rows must be word-aligned");
static_assert(FSL_INIT_WORD_END(FSL_TOP_CFG_BASE + TOP_RESET_GLOBAL) <=
	      FSL91030M_SWITCH_WINDOW_SIZE &&
	      FSL_INIT_WORD_END(FSL_ETH_XSGMII5_MODE_CTRL) <=
	      FSL91030M_SWITCH_WINDOW_SIZE,
	      "top-level board-init registers must fit the switch window");
static_assert(FSL_INIT_WORD_END(FSL_TRAFFIC_WRITE + FSL_TW_PORT_MAP_OFF +
	      (FSL_INIT_TW_PORT_MAP_WORD_COUNT - 1) * sizeof(u32)) <=
	      FSL91030M_SWITCH_WINDOW_SIZE,
	      "traffic-write map must fit the switch window");
static_assert(FSL_INIT_WORD_END(FSL_TRAFFIC_READ + FSL_TR_SLOT_CTL_OFF) <=
	      FSL91030M_SWITCH_WINDOW_SIZE &&
	      FSL_INIT_ROW_END(FSL_TRAFFIC_READ + FSL_TR_SLOT_CFG_OFF,
			       FSL_TR_SLOT_CFG_WORDS - 1, sizeof(u32), 1) <=
	      FSL91030M_SWITCH_WINDOW_SIZE &&
	      FSL_INIT_ROW_END(FSL_TRAFFIC_READ + FSL_TR_PORT_MAP_OFF,
			       FSL_TR_PORT_MAP_WORDS - 1, sizeof(u32), 1) <=
	      FSL91030M_SWITCH_WINDOW_SIZE,
	      "traffic-read maps must fit the switch window");
static_assert(FSL_INIT_WORD_END(FSL_TRAFFIC_SCHEDULE +
	      FSL_TS_SHPUPD_CTRL_OFF + sizeof(u32)) <=
	      FSL91030M_SWITCH_WINDOW_SIZE &&
	      FSL_INIT_ROW_END(FSL_TRAFFIC_SCHEDULE + FSL_TS_PORT_SHP_OFF,
			       FSL_VEGA_TSCHED_LAST_LPORT,
			       FSL_TS_PORT_SHP_STRIDE,
			       FSL_TS_PORT_SHP_WORDS) <=
	      FSL91030M_SWITCH_WINDOW_SIZE &&
	      FSL_INIT_ROW_END(FSL_TRAFFIC_SCHEDULE + FSL_TS_QUE_SCH_OFF,
			       FSL_VEGA_TSCHED_LAST_LPORT,
			       FSL_TS_QUE_SCH_STRIDE,
			       FSL_TS_QUE_SCH_WORDS) <=
	      FSL91030M_SWITCH_WINDOW_SIZE,
	      "traffic-schedule rows must fit the switch window");
static_assert(FSL_INIT_WORD_END(FSL_INET_DEF_VLAN_CTL_WORD4) <=
	      FSL91030M_SWITCH_WINDOW_SIZE &&
	      FSL_INIT_WORD_END(FSL_IVT_LOOP_CTL) <=
	      FSL91030M_SWITCH_WINDOW_SIZE &&
	      FSL_INIT_WORD_END(FSL_INET_LOOP_CTL) <=
	      FSL91030M_SWITCH_WINDOW_SIZE &&
	      FSL_INIT_WORD_END(FSL_IACL_LOOP_CTL) <=
	      FSL91030M_SWITCH_WINDOW_SIZE &&
	      FSL_INIT_WORD_END(FSL_IFWD_LOOP_CTL) <=
	      FSL91030M_SWITCH_WINDOW_SIZE &&
	      FSL_INIT_WORD_END(FSL_IPOL_LOOP_CTL) <=
	      FSL91030M_SWITCH_WINDOW_SIZE &&
	      FSL_INIT_WORD_END(FSL_IDST_LOOP_CTL) <=
	      FSL91030M_SWITCH_WINDOW_SIZE &&
	      FSL_INIT_WORD_END(FSL_IFWD_BRG_CTL) <=
	      FSL91030M_SWITCH_WINDOW_SIZE,
	      "ingress board-init registers must fit the switch window");
static_assert(FSL_INIT_WORD_END(FSL_EEE_LOOP_CTL) <=
	      FSL91030M_SWITCH_WINDOW_SIZE &&
	      FSL_INIT_WORD_END(FSL_EPF_LOOP_CTL) <=
	      FSL91030M_SWITCH_WINDOW_SIZE &&
	      FSL_INIT_WORD_END(FSL_EACL_LOOP_CTL) <=
	      FSL91030M_SWITCH_WINDOW_SIZE &&
	      FSL_INIT_WORD_END(FSL_EPOL_LOOP_CTL) <=
	      FSL91030M_SWITCH_WINDOW_SIZE &&
	      FSL_INIT_WORD_END(FSL_EDST_LOOP_CTL) <=
	      FSL91030M_SWITCH_WINDOW_SIZE,
	      "egress board-init registers must fit the switch window");
static_assert(FSL_INIT_WORD_END(FSL_TRAFFIC_REP + FSL_TREP_MAP_PORT_OFF) <=
	      FSL91030M_SWITCH_WINDOW_SIZE &&
	      FSL_INIT_WORD_END(FSL_IDST_CTL) <=
	      FSL91030M_SWITCH_WINDOW_SIZE,
	      "CPU trap direct registers must fit the switch window");
static_assert(FSL_INIT_ROW_END(FSL_IDST_MC_GRP_SRM,
			       FSL_IDST_MC_GRP_COUNT - 1,
			       FSL_IDST_MC_GRP_STRIDE,
			       FSL_IDST_MC_GRP_WORDS) <=
	      FSL91030M_SWITCH_WINDOW_SIZE,
	      "IDST multicast group table must fit the switch window");
static_assert(FSL_IFWD_MAC_SRM_WORDS == 2 &&
	      FSL_INIT_WORDS_FIT(FSL_IFWD_MAC_SRM_WORDS),
	      "IFWD MAC rows must fit table access buffers");
static_assert(FSL_INIT_ROW_END(FSL_IFWD_MAC_KEY_LEFT0_SRM +
			       (FSL_IFWD_MAC_SRM_BANKS - 1) *
				       FSL_IFWD_MAC_SRM_BANK_STRIDE,
			       FSL_IFWD_MAC_SRM_COUNT - 1,
			       FSL_IFWD_MAC_SRM_STRIDE,
			       FSL_IFWD_MAC_SRM_WORDS) <=
	      FSL91030M_SWITCH_WINDOW_SIZE &&
	      FSL_INIT_ROW_END(FSL_IFWD_MAC_KEY_RIGHT0_SRM +
			       (FSL_IFWD_MAC_SRM_BANKS - 1) *
				       FSL_IFWD_MAC_SRM_BANK_STRIDE,
			       FSL_IFWD_MAC_SRM_COUNT - 1,
			       FSL_IFWD_MAC_SRM_STRIDE,
			       FSL_IFWD_MAC_SRM_WORDS) <=
	      FSL91030M_SWITCH_WINDOW_SIZE &&
	      FSL_INIT_ROW_END(FSL_IFWD_MAC_LEFT0_SRM +
			       (FSL_IFWD_MAC_SRM_BANKS - 1) *
				       FSL_IFWD_MAC_SRM_BANK_STRIDE,
			       FSL_IFWD_MAC_SRM_COUNT - 1,
			       FSL_IFWD_MAC_SRM_STRIDE,
			       FSL_IFWD_MAC_SRM_WORDS) <=
	      FSL91030M_SWITCH_WINDOW_SIZE &&
	      FSL_INIT_ROW_END(FSL_IFWD_MAC_RIGHT0_SRM +
			       (FSL_IFWD_MAC_SRM_BANKS - 1) *
				       FSL_IFWD_MAC_SRM_BANK_STRIDE,
			       FSL_IFWD_MAC_SRM_COUNT - 1,
			       FSL_IFWD_MAC_SRM_STRIDE,
			       FSL_IFWD_MAC_SRM_WORDS) <=
	      FSL91030M_SWITCH_WINDOW_SIZE,
	      "IFWD MAC tables must fit the switch window");

static int fsl_init_restore32(struct fsl91030m *sw, const char *name,
			      unsigned int index, u32 off, u32 before,
			      int ret)
{
	u32 after = 0;
	int rb_ret;

	rb_ret = fsl91030m_direct_write_verify_exact(sw, off, &before, &after, 1);
	if (!rb_ret)
		return ret;

	dev_err(sw->dev,
		"board init %s[%u] rollback failed: off=%#x before=%#x after=%#x ret=%d rollback=%d\n",
		name, index, off, before, after, ret, rb_ret);

	return rb_ret;
}

static int fsl_init_write32_verify(struct fsl91030m *sw, const char *name,
				   unsigned int index, u32 off, u32 want)
{
	u32 before;
	u32 after;
	int ret;

	if (!sw || !sw->dev || !sw->full)
		return -ENODEV;
	if (!name)
		return -EINVAL;

	ret = fsl91030m_direct_read32(sw, off, &before);
	if (ret)
		return ret;
	ret = fsl91030m_direct_write32(sw, off, want);
	if (ret)
		return ret;
	ret = fsl91030m_direct_read32(sw, off, &after);
	if (ret)
		return fsl_init_restore32(sw, name, index, off, before, ret);

	if (after == want)
		return 0;

	dev_err(sw->dev,
		"board init %s[%u] verify failed: off=%#x want=%#x before=%#x after=%#x\n",
		name, index, off, want, before, after);

	return fsl_init_restore32(sw, name, index, off, before, -EIO);
}

static int fsl91030m_vega_mode_init(struct fsl91030m *sw, bool *reset_done)
{
	int ret;

	if (!reset_done)
		return -EINVAL;

	*reset_done = false;

	ret = fsl_init_write32_verify(sw, "work_mode", 0,
				      FSL_TOP_CFG_BASE + TOP_WORK_MODE_CFG,
				      FSL_VEGA_TOP_WORK_MODE);
	if (ret)
		return ret;

	ret = fsl_init_write32_verify(sw, "pcs_switch", 1,
				      FSL_TOP_CFG_BASE + TOP_PCS_SWITCH_MODE_CFG,
				      FSL_VEGA_TOP_PCS_SWITCH);
	if (ret)
		return ret;

	ret = fsl_init_write32_verify(sw, "rgmii_csr", 2,
				      FSL_TOP_CFG_BASE + TOP_RGMII_CSR,
				      FSL_VEGA_TOP_RGMII_CSR);
	if (ret)
		return ret;

	ret = fsl91030m_direct_write32(sw, FSL_TOP_CFG_BASE + TOP_RESET_GLOBAL,
				       RESET_GLOBAL_ASSERT);
	if (ret)
		return ret;
	msleep(FSL_VEGA_RESET_ASSERT_MS);

	ret = fsl91030m_direct_write32(sw, FSL_TOP_CFG_BASE + TOP_RESET_GLOBAL,
				       RESET_GLOBAL_ALL);
	if (ret)
		return ret;
	msleep(FSL_VEGA_RESET_RELEASE_MS);
	*reset_done = true;

	ret = fsl_init_write32_verify(sw, "xsgmii_mode", 3,
				      FSL_ETH_XSGMII4_MODE_CTRL,
				      FSL_VEGA_XSGMII_MODE);
	if (ret)
		return ret;

	ret = fsl_init_write32_verify(sw, "xsgmii_mode", 4,
				      FSL_ETH_XSGMII5_MODE_CTRL,
				      FSL_VEGA_XSGMII_MODE);
	if (ret)
		return ret;

	return 0;
}

static u32 fsl_init_tw_port_map_off(unsigned int mac)
{
	return FSL_TRAFFIC_WRITE + FSL_TW_PORT_MAP_OFF +
	       (mac / FSL_TW_PORT_MAP_FIELDS_PER_WORD) * sizeof(u32);
}

static unsigned int fsl_init_tw_port_map_shift(unsigned int mac)
{
	return (mac % FSL_TW_PORT_MAP_FIELDS_PER_WORD) *
	       FSL_TW_PORT_MAP_FIELD_BITS;
}

static int fsl_init_tw_mac_to_lport(struct fsl91030m *sw, unsigned int mac,
				    u8 *lport)
{
	u32 word;
	unsigned int shift;
	int ret;

	if (!lport || mac >= FSL_TW_MAC_COUNT)
		return -EINVAL;

	ret = fsl91030m_direct_read32(sw, fsl_init_tw_port_map_off(mac),
				      &word);
	if (ret)
		return ret;
	shift = fsl_init_tw_port_map_shift(mac);
	*lport = (word >> shift) & FSL91030M_PORT_ID_MASK;

	return 0;
}

static int fsl_init_tr_lport_to_phy(struct fsl91030m *sw, unsigned int lport,
				    u8 *phy)
{
	u32 word;
	int ret;

	if (!phy || lport >= FSL_TR_PORT_MAP_WORDS)
		return -EINVAL;

	ret = fsl91030m_direct_read32(sw,
				      FSL_TRAFFIC_READ + FSL_TR_PORT_MAP_OFF +
				      lport * sizeof(u32),
				      &word);
	if (ret)
		return ret;

	*phy = word & FSL91030M_PORT_ID_MASK;
	return 0;
}

static u32 fsl_init_ts_port_shp_off(unsigned int lport)
{
	return FSL_TRAFFIC_SCHEDULE + FSL_TS_PORT_SHP_OFF +
	       lport * FSL_TS_PORT_SHP_STRIDE;
}

static u32 fsl_init_ts_que_sch_off(unsigned int lport)
{
	return FSL_TRAFFIC_SCHEDULE + FSL_TS_QUE_SCH_OFF +
	       lport * FSL_TS_QUE_SCH_STRIDE;
}

static int fsl_init_rmw_verify(struct fsl91030m *sw, const char *name,
			       unsigned int index, u32 off, u32 mask,
			       u32 want)
{
	u32 before, after, val;
	int ret;

	if (!sw || !sw->dev || !sw->full)
		return -ENODEV;
	if (!name || !mask || (want & ~mask))
		return -EINVAL;

	ret = fsl91030m_direct_read32(sw, off, &before);
	if (ret)
		return ret;
	val = (before & ~mask) | want;
	ret = fsl91030m_direct_write32(sw, off, val);
	if (ret)
		return ret;
	ret = fsl91030m_direct_read32(sw, off, &after);
	if (ret)
		return fsl_init_restore32(sw, name, index, off, before, ret);

	if ((after & mask) == want)
		return 0;

	dev_err(sw->dev,
		"board init %s[%u] verify failed: off=%#x mask=%#x want=%#x before=%#x after=%#x\n",
		name, index, off, mask, want, before, after);

	return fsl_init_restore32(sw, name, index, off, before, -EIO);
}

static int fsl_init_tw_port_map(struct fsl91030m *sw, unsigned int mac,
				unsigned int lport)
{
	unsigned int shift;
	u32 off;
	u32 mask;
	u32 want;

	if (mac >= FSL_TW_MAC_COUNT || lport > FSL91030M_PORT_ID_MASK)
		return -EINVAL;

	shift = fsl_init_tw_port_map_shift(mac);
	off = fsl_init_tw_port_map_off(mac);
	mask = FSL91030M_PORT_ID_MASK << shift;
	want = lport << shift;

	return fsl_init_rmw_verify(sw, "tw_port_map", mac, off, mask, want);
}

static int fsl_init_tr_port_map(struct fsl91030m *sw, unsigned int lport,
				unsigned int phy)
{
	u32 off;

	if (lport >= FSL_TR_PORT_MAP_WORDS || phy > FSL91030M_PORT_ID_MASK)
		return -EINVAL;

	off = FSL_TRAFFIC_READ + FSL_TR_PORT_MAP_OFF +
	      lport * sizeof(u32);

	return fsl_init_rmw_verify(sw, "tr_port_map", lport, off,
				   FSL91030M_PORT_ID_MASK, phy);
}

static int fsl_init_tr_slot(struct fsl91030m *sw, unsigned int slot,
			    unsigned int lport)
{
	u32 off;

	if (slot >= FSL_TR_SLOT_CFG_WORDS ||
	    lport > FSL91030M_PORT_ID_MASK)
		return -EINVAL;

	off = FSL_TRAFFIC_READ + FSL_TR_SLOT_CFG_OFF +
	      slot * sizeof(u32);

	return fsl_init_rmw_verify(sw, "tr_slot", slot, off,
				   FSL91030M_PORT_ID_MASK, lport);
}

static int fsl_init_tr_slot_num(struct fsl91030m *sw, unsigned int slot_num)
{
	if (slot_num > FSL91030M_PORT_ID_MASK)
		return -EINVAL;

	return fsl_init_rmw_verify(sw, "tr_slot_num", slot_num,
				   FSL_TRAFFIC_READ + FSL_TR_SLOT_CTL_OFF,
				   FSL91030M_PORT_ID_MASK, slot_num);
}

static u8 fsl_init_mac_lport_lookup(unsigned int mac, u8 default_lport)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(fsl_vega_fabric_ports); i++)
		if (fsl_vega_fabric_ports[i].mac == mac)
			return fsl_vega_fabric_ports[i].lport;

	return default_lport;
}

static u8 fsl_init_lport_phy_lookup(unsigned int lport, u8 default_phy)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(fsl_vega_fabric_ports); i++)
		if (fsl_vega_fabric_ports[i].lport == lport)
			return fsl_vega_fabric_ports[i].phy;

	return default_phy;
}

static u8 fsl_init_tr_slot_lport(unsigned int slot)
{
	static const u8 active_lports[] = {
		FSL91030M_VEGA_G5_LPORT,
		FSL91030M_VEGA_G6_LPORT,
		FSL91030M_VEGA_G7_LPORT,
		FSL91030M_VEGA_G8_LPORT,
		FSL91030M_VEGA_G9_LPORT,
		FSL91030M_VEGA_G10_LPORT,
		FSL91030M_VEGA_G11_LPORT,
		FSL91030M_VEGA_G12_LPORT,
		FSL_VEGA_SERDES0_LPORT,
		FSL_VEGA_SERDES1_LPORT,
		FSL_VEGA_SERDES2_LPORT,
		FSL_VEGA_SERDES3_LPORT,
	};
	unsigned int group = slot / 3;

	static_assert(ARRAY_SIZE(active_lports) * 3 ==
		      FSL91030M_VEGA_TR_SLOT_COUNT,
		      "Vega traffic-read slots must cover mgmt/active triplets");

	if (group >= ARRAY_SIZE(active_lports))
		return FSL91030M_PORT_ID_INVALID;

	switch (slot % 3) {
	case 0:
		return FSL_VEGA_FABRIC_MGMT0_LPORT;
	case 1:
		return FSL_VEGA_FABRIC_MGMT1_LPORT;
	default:
		return active_lports[group];
	}
}

static int fsl_init_vega_fabric_map(struct fsl91030m *sw)
{
	unsigned int i;
	int ret;

	/*
	 * Traffic write maps front-panel MAC IDs to logical ports; traffic read
	 * maps logical ports to PHY IDs. This wider set is the Vega board
	 * baseline. The RJ45 G5..G12 rows are also the production Linux
	 * switchdev surface; SFP and management rows remain fixed fabric
	 * bring-up state. Entries not listed here are programmed to the
	 * documented invalid port ID while keeping the write order identical to
	 * the table order.
	 */
	for (i = 0; i < FSL_TW_MAC_COUNT; i++) {
		u8 lport = fsl_init_mac_lport_lookup(i,
						     FSL91030M_PORT_ID_INVALID);

		ret = fsl_init_tw_port_map(sw, i, lport);
		if (ret)
			return ret;
	}

	for (i = 0; i < FSL_TR_PORT_MAP_WORDS; i++) {
		u8 phy = fsl_init_lport_phy_lookup(i,
						   FSL91030M_PORT_ID_INVALID);

		ret = fsl_init_tr_port_map(sw, i, phy);
		if (ret)
			return ret;
	}

	for (i = 0; i < FSL_TR_SLOT_CFG_WORDS; i++) {
		u8 lport = fsl_init_tr_slot_lport(i);

		ret = fsl_init_tr_slot(sw, i, lport);
		if (ret)
			return ret;
	}

	return fsl_init_tr_slot_num(sw, FSL91030M_VEGA_TR_SLOT_COUNT);
}

static int fsl_init_set_inet_default_policy(struct fsl91030m *sw)
{
	const u32 word0_mask = FSL_INET_DEF_VLAN_CTL_STP_ID |
			       FSL_INET_DEF_VLAN_CTL_L2_KEY_TP |
			       FSL_INET_DEF_VLAN_CTL_LRN_DISABLE;
	const u32 word0_want =
		FIELD_PREP_CONST(FSL_INET_DEF_VLAN_CTL_L2_KEY_TP,
				 FSL_INET_DEF_VLAN_CTL_L2_KEY_BRIDGE);
	const u32 word1_mask = FSL_INET_DEF_VLAN_CTL_LRN_UPD_DISABLE |
			       FSL_INET_DEF_VLAN_CTL_PDU_BYPASS_STP;
	const u32 word2_mask = FSL_INET_DEF_VLAN_CTL_DROP_UKW_UC |
			       FSL_INET_DEF_VLAN_CTL_PFM;
	const u32 word2_want =
		FIELD_PREP_CONST(FSL_INET_DEF_VLAN_CTL_PFM,
				 FSL_INET_DEF_VLAN_CTL_PFM_FLOOD);
	const u32 word4_want =
		FIELD_PREP_CONST(FSL_INET_DEF_VLAN_CTL_FID,
				 FSL91030M_DEFAULT_FID);
	int ret;

	/*
	 * Register manual D section 5.4.4 describes the default VLAN table used
	 * by untagged ingress. Keep the Linux-managed baseline at bridge/FID0,
	 * with the documented learning-disable bits clear and unknown-unicast drop
	 * disabled. Broadcast PFM remains flood: clean-boot and live-mutation
	 * tests showed lookup/drop suppresses RJ45 ARP flooding. CPU admission
	 * is handled by explicit CAM trap/redirect actions, not by changing the
	 * default VLAN flood policy. Membership bits are programmed separately so
	 * bridge join/leave can gate each Linux-managed RJ45 port.
	 */
	ret = fsl_init_rmw_verify(sw, "inet_default_policy", 0,
				  FSL_INET_DEF_VLAN_CTL_WORD0,
				  word0_mask, word0_want);
	if (ret)
		return ret;

	ret = fsl_init_rmw_verify(sw, "inet_default_policy", 1,
				  FSL_INET_DEF_VLAN_MEMBERS,
				  word1_mask, 0);
	if (ret)
		return ret;

	ret = fsl_init_rmw_verify(sw, "inet_default_policy", 2,
				  FSL_INET_DEF_VLAN_CTL_WORD2,
				  word2_mask, word2_want);
	if (ret)
		return ret;

	return fsl_init_rmw_verify(sw, "inet_default_policy", 4,
				   FSL_INET_DEF_VLAN_CTL_WORD4,
				   FSL_INET_DEF_VLAN_CTL_FID, word4_want);
}

static int fsl_init_set_inet_default_members(struct fsl91030m *sw)
{
	const u32 off = FSL_INET_DEF_VLAN_MEMBERS;

	/*
	 * Clear every default-VLAN member bit during board initialization. G5 and
	 * G12 are the only verified Linux-managed switchdev ports; forwarding
	 * starts only when standard bridge membership, netdev administrative
	 * state, and STP state explicitly enable those two bits.
	 */
	return fsl_init_rmw_verify(sw, "inet_default_members", 0, off,
				   FSL_INET_DEF_VLAN_MEMBER_MASK, 0);
}

static int fsl_init_vega_loop_controls(struct fsl91030m *sw)
{
	int ret;

	/*
	 * The register reference describes these loop_ctl bytes as loop-packet
	 * bypass controls. Packet-DMA host traffic is loop-classified by the
	 * fabric. Hardware tests show the vendor-style bypass values are needed
	 * before CPU-bound frames reach xy0 and before CPU replies leave G5
	 * without fabric amplification. Ordinary G5<->G12 forwarding is not
	 * loop-classified and remains governed by the bridge/VLAN/STP tables.
	 */
	ret = fsl_init_rmw_verify(sw, "ivt_loop_ctl", 0, FSL_IVT_LOOP_CTL,
				  FSL_INIT_U32_MASK, FSL_LOOP_CTL_FORWARD_2);
	if (ret)
		return ret;

	ret = fsl_init_rmw_verify(sw, "inet_loop_ctl", 1, FSL_INET_LOOP_CTL,
				  FSL_INIT_U32_MASK,
				  FSL_LOOP_CTL_CPU_BYPASS_2);
	if (ret)
		return ret;

	ret = fsl_init_rmw_verify(sw, "ifwd_loop_ctl", 2, FSL_IFWD_LOOP_CTL,
				  FSL_INIT_U32_MASK,
				  FSL_LOOP_CTL_CPU_BYPASS_3);
	if (ret)
		return ret;

	ret = fsl_init_rmw_verify(sw, "iacl_loop_ctl", 3, FSL_IACL_LOOP_CTL,
				  FSL_INIT_U32_MASK,
				  FSL_LOOP_CTL_CPU_BYPASS_1);
	if (ret)
		return ret;

	ret = fsl_init_rmw_verify(sw, "ipol_loop_ctl", 4, FSL_IPOL_LOOP_CTL,
				  FSL_INIT_U32_MASK,
				  FSL_LOOP_CTL_CPU_BYPASS_2);
	if (ret)
		return ret;

	ret = fsl_init_rmw_verify(sw, "idst_loop_ctl", 5, FSL_IDST_LOOP_CTL,
				  FSL_INIT_U32_MASK,
				  FSL_LOOP_CTL_EGRESS_DST_FORWARD);
	if (ret)
		return ret;

	ret = fsl_init_rmw_verify(sw, "eacl_loop_ctl", 6, FSL_EACL_LOOP_CTL,
				  FSL_INIT_U32_MASK,
				  FSL_LOOP_CTL_EACL_CPU_BYPASS);
	if (ret)
		return ret;

	ret = fsl_init_rmw_verify(sw, "eee_loop_ctl", 7, FSL_EEE_LOOP_CTL,
				  FSL_INIT_U32_MASK,
				  FSL_LOOP_CTL_EGRESS_CPU_BYPASS_1);
	if (ret)
		return ret;

	ret = fsl_init_rmw_verify(sw, "epf_loop_ctl", 8, FSL_EPF_LOOP_CTL,
				  FSL_INIT_U32_MASK,
				  FSL_LOOP_CTL_EGRESS_CPU_BYPASS_2);
	if (ret)
		return ret;

	ret = fsl_init_rmw_verify(sw, "epol_loop_ctl", 9, FSL_EPOL_LOOP_CTL,
				  FSL_INIT_U32_MASK,
				  FSL_LOOP_CTL_EGRESS_CPU_BYPASS_1);
	if (ret)
		return ret;

	ret = fsl_init_rmw_verify(sw, "edst_loop_ctl", 10, FSL_EDST_LOOP_CTL,
				  FSL_INIT_U32_MASK,
				  FSL_LOOP_CTL_EGRESS_DST_FORWARD);
	if (ret)
		return ret;

	return 0;
}

static int fsl_init_vega_ifwd_bridge_ctl(struct fsl91030m *sw)
{
	const u32 mask = FSL_IFWD_BRG_CTL_STP_CHK_EN |
			 FSL_IFWD_BRG_CTL_CC_SRC_PATH_CHK_PORT |
			 FSL_IFWD_BRG_CTL_CC_PORT_NO_MATCH_DROP |
			 FSL_IFWD_BRG_CTL_CC_PORT_NO_MATCH_DROP_TO_CPU |
			 FSL_IFWD_BRG_CTL_CC_NO_MATCH_DROP |
			 FSL_IFWD_BRG_CTL_CC_NO_MATCH_DROP_TO_CPU |
			 FSL_IFWD_BRG_CTL_BC_OBEY_MC_PFM |
			 FSL_IFWD_BRG_CTL_DROP_BC |
			 FSL_IFWD_BRG_CTL_BRG_CAM_EN;

	/*
	 * Register manual D section 5.5.6 keeps CAM enabled. Broadcast and
	 * multicast forwarding obey the default VLAN flood policy while the
	 * broadcast CAM action copies one CPU-bound packet for ARP admission.
	 */
	return fsl_init_rmw_verify(sw, "ifwd_brg_ctl", 0, FSL_IFWD_BRG_CTL,
				   mask, FSL_IFWD_BRG_CTL_BRG_CAM_EN |
				   FSL_IFWD_BRG_CTL_BC_OBEY_MC_PFM);
}

static int fsl_init_vega_cpu_admission(struct fsl91030m *sw)
{
	int ret;

	ret = fsl_init_rmw_verify(sw, "trep_ctrl", 0,
				  FSL_TRAFFIC_REP + FSL_TREP_CTRL_OFF,
				  FSL_TREP_CTRL_TRAP_ENABLE,
				  FSL_TREP_CTRL_TRAP_ENABLE);
	if (ret)
		return ret;

	ret = fsl_init_rmw_verify(sw, "trep_map_port", 0,
				  FSL_TRAFFIC_REP + FSL_TREP_MAP_PORT_OFF,
				  FSL_TREP_MAP_PORT_TRAP_PORT,
				  FIELD_PREP(FSL_TREP_MAP_PORT_TRAP_PORT,
					     FSL91030M_VEGA_CPU_LPORT));
	if (ret)
		return ret;

	ret = fsl_init_rmw_verify(sw, "idst_ctl", 0, FSL_IDST_CTL,
				  FSL_IDST_CTL_TRAP_TO_CPU_PORT |
				  FSL_IDST_CTL_CPU_PORT,
				  FSL_IDST_CTL_TRAP_TO_CPU_PORT |
				  FIELD_PREP(FSL_IDST_CTL_CPU_PORT,
					     FSL91030M_VEGA_CPU_LPORT));
	if (ret)
		return ret;

	return fsl91030m_l2_cpu_broadcast_trap_set(sw, true);
}

static u32 fsl_init_ivt_port_off(u8 lport)
{
	return FSL_IVT_PORT_SRM + lport * FSL_IVT_PORT_SRM_STRIDE;
}

static int fsl_init_vega_cpu_egress_g5(struct fsl91030m *sw)
{
	const u32 off = fsl_init_ivt_port_off(FSL91030M_VEGA_CPU_LPORT);
	u32 after[FSL_IVT_PORT_SRM_WORDS] = {};
	u32 row[FSL_IVT_PORT_SRM_WORDS];
	int ret;

	/*
	 * Packet-DMA transmit injects packets as logical CPU port 31.  Hardware
	 * captures show that leaving I_VT_PORT_SRM[31].OUT_LPORT invalid emits
	 * three copies on G5.  Programming the documented FWD_VLD/OUT_LPORT fields
	 * to G5 is the CPU-egress half of the verified G5 management path.  The
	 * IFWD local-MAC action must still be FEC-only, not DST_TRAP plus FEC, or
	 * Linux receives two ingress copies and answers twice.  G12 CPU egress is
	 * deliberately not enabled until its separate behavior is fully explained.
	 */
	ret = fsl91030m_table_read(sw, off, row, ARRAY_SIZE(row));
	if (ret)
		return ret;

	row[4] &= ~(FSL_IVT_PORT_FWD_VALID | FSL_IVT_PORT_OUT_LPORT);
	row[4] |= FSL_IVT_PORT_FWD_VALID |
		  FIELD_PREP(FSL_IVT_PORT_OUT_LPORT, FSL91030M_VEGA_G5_LPORT);

	return fsl91030m_table_write_verify_exact(sw, off, row, after,
						  ARRAY_SIZE(row));
}

static int fsl_init_vega_linux_ports_blocked(struct fsl91030m *sw)
{
	unsigned int i;

	/*
	 * G5..G12 are Linux-managed switchdev ports. Keep them blocked until
	 * bridge membership, netdev administrative state, and STP state make
	 * each port forwarding.
	 */
	for (i = 0; i < FSL91030M_VEGA_PORT_COUNT; i++) {
		const struct fsl91030m_port_desc *desc = fsl91030m_port_get(i);
		int ret;

		if (!desc)
			return -ENODEV;

		ret = fsl91030m_l2_bridge_port_state_set(sw, desc->lport,
							 FSL_INET_STP_DISABLED);
		if (ret)
			return ret;
	}

	return 0;
}

static int fsl_init_tdma_write_verify(struct fsl91030m *sw, const char *name,
				      unsigned int index, u32 off,
				      const u32 *want, unsigned int words)
{
	u32 after[FSL_TBL_MAXW] = {};
	u32 before[FSL_TBL_MAXW] = {};
	u32 rollback_after[FSL_TBL_MAXW] = {};
	int rb_ret;
	int ret;

	if (!sw || !sw->dev)
		return -ENODEV;
	if (!name || !want || !words || words > FSL_TBL_MAXW)
		return -EINVAL;

	ret = fsl91030m_table_read(sw, off, before, words);
	if (ret)
		return ret;

	ret = fsl91030m_table_write_verify_exact(sw, off, want, after, words);
	if (!ret)
		return 0;

	dev_err(sw->dev,
		"board init %s[%u] table verify failed: off=%#x words=%u ret=%d want0=%#x after0=%#x\n",
		name, index, off, words, ret, want[0], after[0]);

	rb_ret = fsl91030m_table_write_verify_exact(sw, off, before,
						    rollback_after, words);
	if (!rb_ret)
		return ret;

	dev_err(sw->dev,
		"board init %s[%u] table rollback failed: off=%#x words=%u ret=%d rollback=%d before0=%#x after0=%#x\n",
		name, index, off, words, ret, rb_ret, before[0],
		rollback_after[0]);

	return rb_ret;
}

static int fsl_init_vega_traffic_schedule(struct fsl91030m *sw)
{
	static const u32 port_row[FSL_TS_PORT_SHP_WORDS] = {
		FSL_VEGA_TSCHED_PORT_RATE_WORD,
		FSL_VEGA_TSCHED_PORT_CTL_WORD,
	};
	static const u32 que_sch_row[FSL_TS_QUE_SCH_WORDS] = {
		FSL_VEGA_TSCHED_QUE_WEIGHT0123_WORD,
		FSL_VEGA_TSCHED_QUE_WEIGHT4567_WORD,
		FSL_VEGA_TSCHED_QUE_CTL_WORD,
	};
	const u32 shpupd = FSL_TRAFFIC_SCHEDULE + FSL_TS_SHPUPD_CTRL_OFF;
	const u32 mask = FSL_INIT_U32_MASK;
	unsigned int lport;
	int ret;

	ret = fsl_init_rmw_verify(sw, "ts_shpupd", 0, shpupd, mask,
				  FSL_VEGA_TSCHED_SHPUPD1_WORD);
	if (ret)
		return ret;

	ret = fsl_init_rmw_verify(sw, "ts_shpupd", 1, shpupd + sizeof(u32),
				  mask, FSL_VEGA_TSCHED_SHPUPD0_WORD);
	if (ret)
		return ret;

	for (lport = FSL_VEGA_TSCHED_FIRST_LPORT;
	     lport < FSL_VEGA_TSCHED_LPORT_COUNT; lport++) {
		u32 off = fsl_init_ts_port_shp_off(lport);

		ret = fsl_init_tdma_write_verify(sw, "ts_port_shp", lport,
						 off, port_row,
						 ARRAY_SIZE(port_row));
		if (ret)
			return ret;

		off = fsl_init_ts_que_sch_off(lport);
		ret = fsl_init_tdma_write_verify(sw, "ts_que_sch", lport,
						 off, que_sch_row,
						 ARRAY_SIZE(que_sch_row));
		if (ret)
			return ret;
	}

	return 0;
}

static void fsl91030m_board_init_fail_safe(struct fsl91030m *sw, int init_ret)
{
	int ret;

	ret = fsl_init_set_inet_default_members(sw);
	if (ret)
		dev_err(sw->dev,
			"board init failure containment could not clear bridge members: init=%d ret=%d\n",
			init_ret, ret);

	ret = fsl_init_vega_linux_ports_blocked(sw);
	if (ret)
		dev_err(sw->dev,
			"board init failure containment could not block G5..G12: init=%d ret=%d\n",
			init_ret, ret);
}

int fsl91030m_board_init_apply(struct fsl91030m *sw)
{
	unsigned int i;
	int ret;
	u32 slot_ctl;
	u8 slot_num;
	bool reset_done;

	ret = fsl91030m_vega_mode_init(sw, &reset_done);
	if (ret) {
		if (reset_done)
			goto fail_safe;
		return ret;
	}

	ret = fsl_init_vega_fabric_map(sw);
	if (ret)
		goto fail_safe;
	ret = fsl_init_set_inet_default_policy(sw);
	if (ret)
		goto fail_safe;
	ret = fsl_init_set_inet_default_members(sw);
	if (ret)
		goto fail_safe;
	ret = fsl_init_vega_traffic_schedule(sw);
	if (ret)
		goto fail_safe;
	ret = fsl_init_vega_loop_controls(sw);
	if (ret)
		goto fail_safe;
	ret = fsl_init_vega_cpu_admission(sw);
	if (ret)
		goto fail_safe;
	ret = fsl_init_vega_cpu_egress_g5(sw);
	if (ret)
		goto fail_safe;
	ret = fsl_init_vega_ifwd_bridge_ctl(sw);
	if (ret)
		goto fail_safe;
	ret = fsl_init_vega_linux_ports_blocked(sw);
	if (ret)
		goto fail_safe;

	for (i = 0; i < FSL91030M_VEGA_PORT_COUNT; i++) {
		const struct fsl91030m_port_desc *desc = fsl91030m_port_get(i);
		u8 observed_lport;
		u8 observed_phy;

		if (!desc) {
			ret = -ENODEV;
			goto fail_safe;
		}

		ret = fsl_init_tw_mac_to_lport(sw, desc->mac,
					       &observed_lport);
		if (ret)
			goto fail_safe;
		ret = fsl_init_tr_lport_to_phy(sw, desc->lport,
					       &observed_phy);
		if (ret)
			goto fail_safe;

		if (observed_lport != desc->lport ||
		    observed_phy != desc->phy) {
			dev_err(sw->dev,
				"board init verify failed: %s mac%u->lport=%u want=%u lport%u->phy=%u want=%u\n",
				desc->name, desc->mac, observed_lport,
				desc->lport, desc->lport, observed_phy,
				desc->phy);
			ret = -EIO;
			goto fail_safe;
		}
	}

	ret = fsl91030m_direct_read32(sw, FSL_TRAFFIC_READ + FSL_TR_SLOT_CTL_OFF,
				      &slot_ctl);
	if (ret)
		goto fail_safe;
	slot_num = slot_ctl & FSL91030M_PORT_ID_MASK;

	if (slot_num != FSL91030M_VEGA_TR_SLOT_COUNT) {
		dev_err(sw->dev,
			"board init verify failed: slot_num=%u want=%u\n",
			slot_num, FSL91030M_VEGA_TR_SLOT_COUNT);
		ret = -EIO;
		goto fail_safe;
	}

	return 0;

fail_safe:
	fsl91030m_board_init_fail_safe(sw, ret);
	return ret;
}
