// SPDX-License-Identifier: GPL-2.0-only
/*
 * Verified L2 state programming for the FSL91030M fabric.
 */
#include <linux/bitfield.h>
#include <linux/build_bug.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/string.h>

#include "fsl91030m.h"

#define FSL_L2_STATE_ROW_FITS(words)	((words) <= FSL_TBL_MAXW)
#define FSL_L2_STP_TABLE_LPORTS \
	(FSL_INET_STP_STATES_PER_WORD * FSL_INET_STP_SRM_WORDS)
#define FSL_L2_STP_LPORT_FITS(lport)	((lport) < FSL_L2_STP_TABLE_LPORTS)
#define FSL_L2_ROW_END(base, lport, stride, words) \
	((base) + (lport) * (stride) + (words) * sizeof(u32))
#define FSL_L2_CPU_LOCAL_CAM_SLOT	30u
#define FSL_L2_CPU_BCAST_CAM_SLOT	31u
#define FSL_L2_CPU_CAM_KEY_MAC_BITS	48u
#define FSL_L2_CPU_CAM_KEY0_BITS	31u
#define FSL_L2_CPU_CAM_KEY1_BITS	31u
#define FSL_L2_CPU_CAM_FID_SHIFT	FSL_L2_CPU_CAM_KEY_MAC_BITS

static_assert(FSL_INET_STP_STATES_PER_WORD &&
	      sizeof(u32) * BITS_PER_BYTE >=
	      FSL_INET_STP_STATES_PER_WORD * FSL_INET_STP_STATE_BITS,
	      "STP states must fit one table word");
static_assert(FSL_INET_STP_STATE_MASK ==
	      GENMASK(FSL_INET_STP_STATE_BITS - 1, 0),
	      "STP state mask must match state width");
static_assert(BIT(FSL_INET_STP_STATE_BITS) > FSL_INET_STP_FORWARDING,
	      "STP state field must hold forwarding state");
static_assert(FSL_L2_STATE_ROW_FITS(FSL_INET_PORT_SRM_WORDS) &&
	      FSL_L2_STATE_ROW_FITS(FSL_INET_STP_SRM_WORDS) &&
	      FSL_L2_STATE_ROW_FITS(FSL_EPF_OUT_PORT_SRM_WORDS),
	      "bridge state rows must fit table access buffers");
static_assert(FSL_L2_STP_LPORT_FITS(FSL91030M_VEGA_G5_LPORT) &&
	      FSL_L2_STP_LPORT_FITS(FSL91030M_VEGA_G12_LPORT),
	      "Vega logical ports must fit the STP state table");
static_assert(FSL_INET_PORT_SRM_STRIDE >=
	      FSL_INET_PORT_SRM_WORDS * sizeof(u32) &&
	      !(FSL_INET_PORT_SRM_STRIDE & (sizeof(u32) - 1)),
	      "INET port rows must be word-aligned");
static_assert(FSL_EPF_OUT_PORT_SRM_STRIDE >=
	      FSL_EPF_OUT_PORT_SRM_WORDS * sizeof(u32) &&
	      !(FSL_EPF_OUT_PORT_SRM_STRIDE & (sizeof(u32) - 1)),
	      "EPF output-port rows must be word-aligned");
static_assert(FSL_L2_ROW_END(FSL_INET_PORT_SRM, FSL91030M_VEGA_G12_LPORT,
			     FSL_INET_PORT_SRM_STRIDE,
			     FSL_INET_PORT_SRM_WORDS) <=
	      FSL91030M_SWITCH_WINDOW_SIZE &&
	      FSL_L2_ROW_END(FSL_EPF_OUT_PORT_SRM, FSL91030M_VEGA_G12_LPORT,
			     FSL_EPF_OUT_PORT_SRM_STRIDE,
			     FSL_EPF_OUT_PORT_SRM_WORDS) <=
	      FSL91030M_SWITCH_WINDOW_SIZE,
	      "Vega bridge state rows must fit the switch window");
static_assert(GENMASK(31, 1) == FSL_IFWD_MAC_CAM_KEY0 &&
	      GENMASK(30, 0) == FSL_IFWD_MAC_CAM_KEY1,
	      "IFWD MAC CAM key field split must match the register book");
static_assert(FSL_L2_CPU_CAM_KEY0_BITS + FSL_L2_CPU_CAM_KEY1_BITS >=
	      FSL_L2_CPU_CAM_KEY_MAC_BITS,
	      "IFWD MAC CAM key fields must hold an Ethernet address");
static_assert(FSL_IFWD_MAC_CAM_STRIDE >=
	      FSL_IFWD_MAC_CAM_WORDS * sizeof(u32) &&
	      !(FSL_IFWD_MAC_CAM_STRIDE & (sizeof(u32) - 1)),
	      "IFWD MAC CAM rows must be word-aligned");
static_assert(FSL_L2_ROW_END(FSL_IFWD_MAC_CAM_CTL,
			     FSL_IFWD_MAC_CAM_COUNT - 1,
			     FSL_IFWD_MAC_CAM_STRIDE,
			     FSL_IFWD_MAC_CAM_WORDS) <=
	      FSL91030M_SWITCH_WINDOW_SIZE &&
	      FSL_L2_ROW_END(FSL_IFWD_MAC_CAM_ACT_CTL,
			     FSL_IFWD_MAC_CAM_COUNT - 1,
			     FSL_IFWD_MAC_CAM_STRIDE,
			     FSL_IFWD_MAC_CAM_WORDS) <=
	      FSL91030M_SWITCH_WINDOW_SIZE,
	      "IFWD MAC CAM rows must fit the switch window");

static u32 fsl91030m_inet_port_off(u8 lport)
{
	return FSL_INET_PORT_SRM + lport * FSL_INET_PORT_SRM_STRIDE;
}

static u32 fsl91030m_epf_out_port_off(u8 lport)
{
	return FSL_EPF_OUT_PORT_SRM + lport * FSL_EPF_OUT_PORT_SRM_STRIDE;
}

static u32 fsl91030m_ifwd_mac_cam_key_off(unsigned int slot)
{
	return FSL_IFWD_MAC_CAM_CTL + slot * FSL_IFWD_MAC_CAM_STRIDE;
}

static u32 fsl91030m_ifwd_mac_cam_action_off(unsigned int slot)
{
	return FSL_IFWD_MAC_CAM_ACT_CTL + slot * FSL_IFWD_MAC_CAM_STRIDE;
}

static int fsl91030m_l2_direct_restore_exact(struct fsl91030m *sw, u32 off,
					     const u32 *saved,
					     unsigned int count)
{
	u32 after[FSL_TBL_MAXW] = {};

	return fsl91030m_direct_write_verify_exact(sw, off, saved, after,
						   count);
}

static int fsl91030m_l2_table_restore_exact(struct fsl91030m *sw, u32 off,
					    const u32 *saved,
					    unsigned int count)
{
	u32 after[FSL_TBL_MAXW] = {};

	return fsl91030m_table_write_verify_exact(sw, off, saved, after,
						  count);
}

static void fsl91030m_l2_cpu_mac_key_pack(const u8 *mac, u32 key[2])
{
	u64 packed = 0;
	unsigned int i;

	for (i = 0; i < ETH_ALEN; i++)
		packed = (packed << BITS_PER_BYTE) | mac[i];
	packed |= (u64)FSL91030M_DEFAULT_FID << FSL_L2_CPU_CAM_FID_SHIFT;

	key[0] = FSL_IFWD_MAC_CAM_VALID |
		 ((u32)(packed & GENMASK_ULL(FSL_L2_CPU_CAM_KEY0_BITS - 1,
					     0)) << 1);
	key[1] = (u32)((packed >> FSL_L2_CPU_CAM_KEY0_BITS) &
		       FSL_IFWD_MAC_CAM_KEY1);
}

static int fsl91030m_l2_cpu_cam_write_exact(struct fsl91030m *sw,
					    unsigned int slot,
					    const u32 key[2],
					    const u32 action[2])
{
	u32 after[FSL_IFWD_MAC_CAM_WORDS] = {};
	int ret;

	if (slot >= FSL_IFWD_MAC_CAM_COUNT)
		return -EINVAL;

	ret = fsl91030m_direct_write_verify_exact(sw,
						  fsl91030m_ifwd_mac_cam_key_off(slot),
						  key, after,
						  FSL_IFWD_MAC_CAM_WORDS);
	if (ret)
		return ret;

	return fsl91030m_direct_write_verify_exact(sw,
						   fsl91030m_ifwd_mac_cam_action_off(slot),
						   action, after,
						   FSL_IFWD_MAC_CAM_WORDS);
}

static int fsl91030m_l2_cpu_cam_clear(struct fsl91030m *sw, unsigned int slot)
{
	const u32 zero[FSL_IFWD_MAC_CAM_WORDS] = {};

	return fsl91030m_l2_cpu_cam_write_exact(sw, slot, zero, zero);
}

static void fsl91030m_l2_cpu_cam_action_trap(u32 action[FSL_IFWD_MAC_CAM_WORDS])
{
	action[0] = FSL_IFWD_MAC_ACT_STATIC | FSL_IFWD_MAC_ACT_DST_TRAP;
	action[1] = 0;
}

static void
fsl91030m_l2_cpu_cam_action_redirect(u32 action[FSL_IFWD_MAC_CAM_WORDS])
{
	/*
	 * A local unicast FEC redirect to logical CPU port 31 is sufficient for
	 * packet-DMA admission.  Combining it with DST_TRAP creates two CPU RX
	 * copies, and Linux answers each copy independently.
	 */
	action[0] = FSL_IFWD_MAC_ACT_STATIC |
		    FIELD_PREP(FSL_IFWD_MAC_ACT_FEC_PATH_TP,
			       FSL_IFWD_MAC_FEC_TP_PORT) |
		    FIELD_PREP(FSL_IFWD_MAC_ACT_FEC_PATH_0,
			       FSL91030M_VEGA_CPU_LPORT);
	action[1] = FIELD_PREP(FSL_IFWD_MAC_ACT_FEC_PATH_1,
			       FSL91030M_VEGA_CPU_LPORT >> 10);
}

static int fsl91030m_l2_cpu_cam_set_mac(struct fsl91030m *sw,
					unsigned int slot, const u8 *mac,
					const u32 action[FSL_IFWD_MAC_CAM_WORDS])
{
	const u32 zero[FSL_IFWD_MAC_CAM_WORDS] = {};
	u32 key[FSL_IFWD_MAC_CAM_WORDS];
	int ret;

	fsl91030m_l2_cpu_mac_key_pack(mac, key);

	ret = fsl91030m_l2_cpu_cam_write_exact(sw, slot, key, zero);
	if (ret)
		return ret;

	ret = fsl91030m_l2_cpu_cam_write_exact(sw, slot, key, action);
	if (!ret)
		return 0;

	fsl91030m_l2_cpu_cam_clear(sw, slot);
	return ret;
}

int fsl91030m_l2_cpu_broadcast_trap_set(struct fsl91030m *sw, bool enable)
{
	static const u8 bcast[ETH_ALEN] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	};
	int ret;

	if (!sw || !sw->dev || !sw->full)
		return -ENODEV;

	mutex_lock(&sw->op_lock);
	if (enable) {
		u32 action[FSL_IFWD_MAC_CAM_WORDS];

		fsl91030m_l2_cpu_cam_action_trap(action);
		ret = fsl91030m_l2_cpu_cam_set_mac(sw,
						   FSL_L2_CPU_BCAST_CAM_SLOT,
						   bcast, action);
	} else {
		ret = fsl91030m_l2_cpu_cam_clear(sw,
						 FSL_L2_CPU_BCAST_CAM_SLOT);
	}
	mutex_unlock(&sw->op_lock);

	return ret;
}

int fsl91030m_l2_cpu_local_mac_set(struct fsl91030m *sw, const u8 *mac,
				   bool enable)
{
	int ret;

	if (!sw || !sw->dev || !sw->full)
		return -ENODEV;
	if (enable && (!mac || !is_valid_ether_addr(mac)))
		return -EINVAL;

	mutex_lock(&sw->op_lock);
	if (enable) {
		u32 action[FSL_IFWD_MAC_CAM_WORDS];

		fsl91030m_l2_cpu_cam_action_redirect(action);
		ret = fsl91030m_l2_cpu_cam_set_mac(sw,
						   FSL_L2_CPU_LOCAL_CAM_SLOT,
						   mac, action);
	} else {
		ret = fsl91030m_l2_cpu_cam_clear(sw,
						 FSL_L2_CPU_LOCAL_CAM_SLOT);
	}
	mutex_unlock(&sw->op_lock);

	return ret;
}

static int fsl91030m_l2_bridge_state_rollback(struct fsl91030m *sw,
					      u32 inet_ctl,
					      const u32 *inet_port,
					      const u32 *inet_stp,
					      const u32 *epf_out,
					      u8 lport)
{
	int ret = 0;
	int rb_ret;

	rb_ret = fsl91030m_l2_direct_restore_exact(sw, FSL_INET_CTL,
						   &inet_ctl, 1);
	if (rb_ret && !ret)
		ret = rb_ret;

	rb_ret = fsl91030m_l2_table_restore_exact(sw,
						  fsl91030m_inet_port_off(lport),
						  inet_port,
						  FSL_INET_PORT_SRM_WORDS);
	if (rb_ret && !ret)
		ret = rb_ret;

	rb_ret = fsl91030m_l2_table_restore_exact(sw, FSL_INET_STP_SRM,
						  inet_stp,
						  FSL_INET_STP_SRM_WORDS);
	if (rb_ret && !ret)
		ret = rb_ret;

	rb_ret = fsl91030m_l2_table_restore_exact(sw,
						  fsl91030m_epf_out_port_off(lport),
						  epf_out,
						  FSL_EPF_OUT_PORT_SRM_WORDS);
	if (rb_ret && !ret)
		ret = rb_ret;

	return ret;
}

int fsl91030m_l2_bridge_membership_set(struct fsl91030m *sw, u8 lport,
				       bool member)
{
	u32 after = 0;
	u32 before;
	u32 want;
	u32 bit;
	int rb_ret;
	int ret = 0;

	if (!sw || !sw->dev || !sw->full)
		return -ENODEV;
	if (!fsl91030m_port_lport_supported(lport) ||
	    lport > FSL_INET_DEF_VLAN_MAX_LPORT)
		return -EINVAL;

	bit = fsl91030m_l2_default_membership_bit(lport);

	mutex_lock(&sw->op_lock);

	ret = fsl91030m_direct_read32(sw, FSL_INET_DEF_VLAN_MEMBERS, &before);
	if (ret)
		goto out;
	if (member)
		want = before | bit;
	else
		want = before & ~bit;

	if (want == before)
		goto out;

	ret = fsl91030m_direct_write_verify_exact(sw,
						  FSL_INET_DEF_VLAN_MEMBERS,
						  &want, &after, 1);
	if (ret)
		goto rollback;
	goto out;

rollback:
	rb_ret = fsl91030m_l2_direct_restore_exact(sw,
						   FSL_INET_DEF_VLAN_MEMBERS,
						   &before, 1);
	if (rb_ret) {
		dev_err(sw->dev,
			"bridge membership rollback failed lport=%u before=%#x want=%#x after=%#x ret=%d rollback=%d\n",
			lport, before, want, after, ret, rb_ret);
		ret = rb_ret;
	}

out:
	mutex_unlock(&sw->op_lock);
	return ret;
}

static int fsl91030m_l2_ageing_flags_set(struct fsl91030m *sw, bool enabled,
					 u32 *saved)
{
	u32 after;
	u32 want;
	int rb_ret;
	int ret;

	ret = fsl91030m_direct_read32(sw, FSL_IFWD_AGING_CTL_FLAGS, saved);
	if (ret)
		return ret;

	want = *saved;
	if (enabled)
		want |= FSL_IFWD_AGING_CTL_AGING_EN;
	else
		want &= ~FSL_IFWD_AGING_CTL_AGING_EN;

	if (want == *saved)
		return 0;

	ret = fsl91030m_direct_write_verify_exact(sw, FSL_IFWD_AGING_CTL_FLAGS,
						  &want, &after, 1);
	if (!ret)
		return 0;

	rb_ret = fsl91030m_l2_direct_restore_exact(sw,
						   FSL_IFWD_AGING_CTL_FLAGS,
						   saved, 1);
	if (rb_ret) {
		dev_err(sw->dev,
			"ageing enable rollback failed: ret=%d rollback=%d\n",
			ret, rb_ret);
		ret = rb_ret;
	}

	return ret;
}

int fsl91030m_l2_ageing_time_set(struct fsl91030m *sw, u32 seconds)
{
	u32 flags_before;
	u32 timer_after;
	u32 timer_before;
	bool restore_timer = false;
	int rb_ret;
	int ret = 0;

	if (!sw || !sw->dev || !sw->full)
		return -ENODEV;
	if (seconds != FSL91030M_AGEING_DISABLE_SEC &&
	    (seconds < FSL91030M_AGEING_MIN_SEC ||
	     seconds > FSL91030M_AGEING_MAX_SEC))
		return -EINVAL;

	mutex_lock(&sw->age_lock);
	mutex_lock(&sw->op_lock);

	/*
	 * Register book sections 5.5.11 and 5.5.14 expose Linux bridge ageing as
	 * a global enable bit plus a normal-ageing second threshold. Linux uses
	 * zero to disable bridge MAC-entry ageing, so map zero to
	 * aging_ctl_aging_en=0 and leave the saved timer threshold untouched. The
	 * driver does not expose switchdev FDB programming.
	 */
	if (seconds == FSL91030M_AGEING_DISABLE_SEC) {
		ret = fsl91030m_l2_ageing_flags_set(sw, false, &flags_before);
		goto out_unlock;
	}

	ret = fsl91030m_direct_read32(sw, FSL_IFWD_AGING_TIMER_CTL,
				      &timer_before);
	if (ret)
		goto out_unlock;

	if (timer_before != seconds) {
		ret = fsl91030m_direct_write_verify_exact(sw,
							  FSL_IFWD_AGING_TIMER_CTL,
							  &seconds,
							  &timer_after, 1);
		if (ret)
			goto rollback_timer;
		restore_timer = true;
	}

	ret = fsl91030m_l2_ageing_flags_set(sw, true, &flags_before);
	if (ret && restore_timer) {
		rb_ret = fsl91030m_l2_direct_restore_exact(sw,
							   FSL_IFWD_AGING_TIMER_CTL,
							   &timer_before, 1);
		if (rb_ret) {
			dev_err(sw->dev,
				"ageing timer rollback failed: ret=%d rollback=%d\n",
				ret, rb_ret);
			ret = rb_ret;
		}
	}
	goto out_unlock;

rollback_timer:
	rb_ret = fsl91030m_l2_direct_restore_exact(sw,
						   FSL_IFWD_AGING_TIMER_CTL,
						   &timer_before, 1);
	if (rb_ret) {
		dev_err(sw->dev,
			"ageing timer rollback failed: ret=%d rollback=%d\n",
			ret, rb_ret);
		ret = rb_ret;
	}

out_unlock:
	mutex_unlock(&sw->op_lock);
	mutex_unlock(&sw->age_lock);

	return ret;
}

int fsl91030m_l2_bridge_port_state_set(struct fsl91030m *sw,
				       u8 lport, u8 state)
{
	u32 inet_ctl_before;
	u32 inet_ctl_after;
	u32 inet_ctl_want;
	u32 inet_port_before[FSL_INET_PORT_SRM_WORDS] = {};
	u32 inet_port_after[FSL_INET_PORT_SRM_WORDS] = {};
	u32 inet_port_want[FSL_INET_PORT_SRM_WORDS] = {};
	u32 inet_stp_before[FSL_INET_STP_SRM_WORDS] = {};
	u32 inet_stp_after[FSL_INET_STP_SRM_WORDS] = {};
	u32 inet_stp_want[FSL_INET_STP_SRM_WORDS] = {};
	u32 epf_after[FSL_EPF_OUT_PORT_SRM_WORDS] = {};
	u32 epf_before[FSL_EPF_OUT_PORT_SRM_WORDS] = {};
	u32 epf_want[FSL_EPF_OUT_PORT_SRM_WORDS] = {};
	unsigned int shift;
	unsigned int word;
	bool forwarding;
	u32 stp_mask;
	int rb_ret;
	int ret;

	if (!sw || !sw->dev || !sw->full)
		return -ENODEV;
	if (!fsl91030m_port_lport_supported(lport) ||
	    state > FSL_INET_STP_FORWARDING)
		return -EINVAL;

	word = lport / FSL_INET_STP_STATES_PER_WORD;
	if (word >= FSL_INET_STP_SRM_WORDS)
		return -EINVAL;
	shift = (lport % FSL_INET_STP_STATES_PER_WORD) *
		FSL_INET_STP_STATE_BITS;
	stp_mask = FSL_INET_STP_STATE_MASK << shift;
	forwarding = state == FSL_INET_STP_FORWARDING;

	mutex_lock(&sw->op_lock);

	ret = fsl91030m_direct_read32(sw, FSL_INET_CTL, &inet_ctl_before);
	if (ret)
		goto out_unlock;

	ret = fsl91030m_table_read(sw, fsl91030m_inet_port_off(lport),
				   inet_port_before, FSL_INET_PORT_SRM_WORDS);
	if (ret)
		goto out_unlock;

	ret = fsl91030m_table_read(sw, FSL_INET_STP_SRM, inet_stp_before,
				   FSL_INET_STP_SRM_WORDS);
	if (ret)
		goto out_unlock;

	ret = fsl91030m_table_read(sw, fsl91030m_epf_out_port_off(lport),
				   epf_before, FSL_EPF_OUT_PORT_SRM_WORDS);
	if (ret)
		goto out_unlock;

	memcpy(inet_port_want, inet_port_before, sizeof(inet_port_want));
	memcpy(inet_stp_want, inet_stp_before, sizeof(inet_stp_want));
	memcpy(epf_want, epf_before, sizeof(epf_want));

	inet_ctl_want = inet_ctl_before | FSL_INET_CTL_STP_CHK_EN;
	inet_port_want[0] |= FSL_INET_PORT_STP_EN;
	inet_stp_want[word] &= ~stp_mask;
	inet_stp_want[word] |= (u32)state << shift;
	if (forwarding)
		epf_want[0] &= ~FSL_EPF_OUT_STP_CHECK_EN;
	else
		epf_want[0] |= FSL_EPF_OUT_STP_CHECK_EN;

	ret = fsl91030m_direct_write_verify_exact(sw, FSL_INET_CTL,
						  &inet_ctl_want,
						  &inet_ctl_after, 1);
	if (ret)
		goto rollback;

	ret = fsl91030m_table_write_verify_exact(sw,
						 fsl91030m_inet_port_off(lport),
						 inet_port_want,
						 inet_port_after,
						 FSL_INET_PORT_SRM_WORDS);
	if (ret)
		goto rollback;

	ret = fsl91030m_table_write_verify_exact(sw, FSL_INET_STP_SRM,
						 inet_stp_want, inet_stp_after,
						 FSL_INET_STP_SRM_WORDS);
	if (ret)
		goto rollback;

	ret = fsl91030m_table_write_verify_exact(sw,
						 fsl91030m_epf_out_port_off(lport),
						 epf_want, epf_after,
						 FSL_EPF_OUT_PORT_SRM_WORDS);
	if (ret)
		goto rollback;

	goto out_unlock;

rollback:
	rb_ret = fsl91030m_l2_bridge_state_rollback(sw, inet_ctl_before,
						    inet_port_before,
						    inet_stp_before,
						    epf_before, lport);
	if (rb_ret) {
		dev_err(sw->dev,
			"bridge port state rollback failed lport=%u ret=%d rollback=%d\n",
			lport, ret, rb_ret);
		ret = rb_ret;
	}

out_unlock:
	mutex_unlock(&sw->op_lock);
	return ret;
}
