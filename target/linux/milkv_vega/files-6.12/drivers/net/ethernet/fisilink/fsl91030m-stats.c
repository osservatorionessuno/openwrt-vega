// SPDX-License-Identifier: GPL-2.0-only
/*
 * FSL91030M CMAC counter translation for Linux switchdev port stats.
 */

#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/netdevice.h>
#include <linux/string.h>

#include "fsl91030m.h"

#define FSL_CMAC_COUNTER_TABLE_END(base, last_entry) \
	((base) + (last_entry) * FSL_CMAC_COUNTER_STRIDE + \
	 FSL_CMAC_COUNTER_WORDS * sizeof(u32))

static_assert(FSL_CMAC_COUNTER_WORDS == 2,
	      "CMAC counters are documented as 64-bit two-word SRM entries");
static_assert(FSL_CMAC_COUNTER_WORDS <= FSL_TBL_MAXW,
	      "CMAC counter rows must fit the table access buffer");
static_assert(sizeof(u64) == FSL_CMAC_COUNTER_WORDS * sizeof(u32),
	      "CMAC counter packing expects one u64");
static_assert(FSL_CMAC_COUNTER_STRIDE >=
	      FSL_CMAC_COUNTER_WORDS * sizeof(u32),
	      "CMAC counter stride must cover one counter entry");
static_assert(!(FSL_CMAC_COUNTER_STRIDE & (sizeof(u32) - 1)),
	      "CMAC counter stride must stay word-aligned");
static_assert(FSL_CMAC_CAST_MCAST_BASE + FSL_CMAC_PORT_COUNT <=
	      FSL_CMAC_CAST_ENTRY_COUNT,
	      "multicast cast-counter entries must fit the cast SRM table");
static_assert(FSL_CMAC_COUNTER_TABLE_END(FSL_CMACRX_TOTAL_BYTES_CNT_SRM,
					 FSL_CMAC_PORT_COUNT - 1) <=
	      FSL91030M_SWITCH_WINDOW_SIZE &&
	      FSL_CMAC_COUNTER_TABLE_END(FSL_CMACRX_GOOD_FRAME_CNT_SRM,
					 FSL_CMAC_PORT_COUNT - 1) <=
	      FSL91030M_SWITCH_WINDOW_SIZE &&
	      FSL_CMAC_COUNTER_TABLE_END(FSL_CMACRX_CAST_FRAME_CNT_SRM,
					 FSL_CMAC_CAST_ENTRY_COUNT - 1) <=
	      FSL91030M_SWITCH_WINDOW_SIZE &&
	      FSL_CMAC_COUNTER_TABLE_END(FSL_CMACTX_TOTAL_BYTES_CNT_SRM,
					 FSL_CMAC_PORT_COUNT - 1) <=
	      FSL91030M_SWITCH_WINDOW_SIZE &&
	      FSL_CMAC_COUNTER_TABLE_END(FSL_CMACTX_GOOD_FRAME_CNT_SRM,
					 FSL_CMAC_PORT_COUNT - 1) <=
	      FSL91030M_SWITCH_WINDOW_SIZE,
	      "CMAC counter tables must fit the switch window");

static u32 fsl91030m_cmac_counter_off(u32 base, unsigned int entry)
{
	return base + entry * FSL_CMAC_COUNTER_STRIDE;
}

static int fsl91030m_cmac_counter_read(struct fsl91030m *sw, u32 base,
				       unsigned int entry,
				       unsigned int entries, u64 *val)
{
	u32 words[FSL_CMAC_COUNTER_WORDS] = {};
	int ret;

	if (!val || entry >= entries)
		return -EINVAL;

	ret = fsl91030m_table_read(sw, fsl91030m_cmac_counter_off(base, entry),
				   words, FSL_CMAC_COUNTER_WORDS);
	if (ret)
		return ret;

	*val = words[0] | ((u64)words[1] << 32);
	return 0;
}

int fsl91030m_port_hw_stats_read(struct fsl91030m *sw,
				 const struct fsl91030m_port_desc *desc,
				 struct rtnl_link_stats64 *stats)
{
	unsigned int mcast_entry;
	u64 rx_bytes;
	u64 rx_good;
	u64 rx_mcast;
	u64 tx_bytes;
	u64 tx_good;
	int ret;

	if (!sw || !sw->dev || !sw->full)
		return -ENODEV;
	if (!desc || !stats || desc->mac >= FSL_CMAC_PORT_COUNT)
		return -EINVAL;

	mcast_entry = desc->mac + FSL_CMAC_CAST_MCAST_BASE;

	mutex_lock(&sw->op_lock);
	ret = fsl91030m_cmac_counter_read(sw, FSL_CMACRX_TOTAL_BYTES_CNT_SRM,
					  desc->mac, FSL_CMAC_PORT_COUNT,
					  &rx_bytes);
	if (ret)
		goto out;
	ret = fsl91030m_cmac_counter_read(sw, FSL_CMACRX_GOOD_FRAME_CNT_SRM,
					  desc->mac, FSL_CMAC_PORT_COUNT,
					  &rx_good);
	if (ret)
		goto out;
	ret = fsl91030m_cmac_counter_read(sw, FSL_CMACRX_CAST_FRAME_CNT_SRM,
					  mcast_entry,
					  FSL_CMAC_CAST_ENTRY_COUNT,
					  &rx_mcast);
	if (ret)
		goto out;
	ret = fsl91030m_cmac_counter_read(sw, FSL_CMACTX_TOTAL_BYTES_CNT_SRM,
					  desc->mac, FSL_CMAC_PORT_COUNT,
					  &tx_bytes);
	if (ret)
		goto out;
	ret = fsl91030m_cmac_counter_read(sw, FSL_CMACTX_GOOD_FRAME_CNT_SRM,
					  desc->mac, FSL_CMAC_PORT_COUNT,
					  &tx_good);
	if (ret)
		goto out;

	memset(stats, 0, sizeof(*stats));
	stats->rx_bytes = rx_bytes;
	stats->rx_packets = rx_good;
	stats->multicast = rx_mcast;
	stats->tx_bytes = tx_bytes;
	stats->tx_packets = tx_good;

out:
	mutex_unlock(&sw->op_lock);
	return ret;
}
