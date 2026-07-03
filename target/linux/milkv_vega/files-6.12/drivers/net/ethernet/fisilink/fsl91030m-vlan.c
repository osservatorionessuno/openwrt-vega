// SPDX-License-Identifier: GPL-2.0-only
/*
 * CTAG bridge VLAN support (single service VID 1107) for the Milk-V Vega copper
 * datapath. The offload covers any subset of the copper ports G5..G12: tagged
 * VID1107 membership, INET ingress VLAN filtering on the member ports, and
 * per-egress CTAG stripping through the EPF out-port un_ctag bit. The register
 * recipe is packet-proven on G5/G12 (their IVT translation hash slots are
 * cross-checked against known-good values, 4 and 21); the other copper ports
 * use the same recipe with their hash slot computed from the shared CRC.
 * PVID/untagged ingress and arbitrary VID/FID mappings are still not exposed.
 */
#include <linux/build_bug.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/string.h>

#include "fsl91030m.h"

#define FSL91030M_CTAG_SERVICE_CVID	1107u
#define FSL91030M_CTAG_SERVICE_OP_IDX	1u
#define FSL91030M_IVT_XLATE0_KEYCTL	0x00000104u
#define FSL91030M_IVT_PORT_XLATE0_EN	BIT(14)
#define FSL91030M_CTAG_ROW_COUNT	FSL91030M_VLAN_CTAGSVC_PORTS

struct fsl91030m_ivt_xlate_bank {
	u32 key_base;
	u32 action_base;
};

static const struct fsl91030m_ivt_xlate_bank fsl91030m_ivt_xlate_banks[] = {
	{ FSL_IVT_XKEY_LEFT0_SRM,  FSL_IVT_XLATE_LEFT0_SRM  },
	{ FSL_IVT_XKEY_RIGHT0_SRM, FSL_IVT_XLATE_RIGHT0_SRM },
	{ FSL_IVT_XKEY_LEFT1_SRM,  FSL_IVT_XLATE_LEFT1_SRM  },
	{ FSL_IVT_XKEY_RIGHT1_SRM, FSL_IVT_XLATE_RIGHT1_SRM },
	{ FSL_IVT_XKEY_LEFT2_SRM,  FSL_IVT_XLATE_LEFT2_SRM  },
	{ FSL_IVT_XKEY_RIGHT2_SRM, FSL_IVT_XLATE_RIGHT2_SRM },
	{ FSL_IVT_XKEY_LEFT3_SRM,  FSL_IVT_XLATE_LEFT3_SRM  },
	{ FSL_IVT_XKEY_RIGHT3_SRM, FSL_IVT_XLATE_RIGHT3_SRM },
};

static_assert(FSL91030M_CTAG_ROW_COUNT == FSL91030M_VEGA_PORT_COUNT,
	      "VID1107 service covers every copper port");
static_assert(FSL_INET_VLAN_SRM +
	      FSL91030M_CTAG_SERVICE_VID * FSL_INET_VLAN_SRM_STRIDE +
	      FSL_INET_VLAN_SRM_WORDS * sizeof(u32) <=
	      FSL91030M_SWITCH_WINDOW_SIZE,
	      "VID1107 INET row must fit the switch window");
static_assert(FSL_IVT_PORT_SRM +
	      FSL91030M_VEGA_G12_LPORT * FSL_IVT_PORT_SRM_STRIDE +
	      FSL_IVT_PORT_SRM_WORDS * sizeof(u32) <=
	      FSL91030M_SWITCH_WINDOW_SIZE,
	      "G12 IVT port row must fit the switch window");
static_assert(FSL_INET_PORT_SRM +
	      FSL91030M_VEGA_G12_LPORT * FSL_INET_PORT_SRM_STRIDE +
	      FSL_INET_PORT_SRM_WORDS * sizeof(u32) <=
	      FSL91030M_SWITCH_WINDOW_SIZE,
	      "G12 INET port row must fit the switch window");
static_assert(FSL_EEE_PORT_SRM +
	      FSL91030M_VEGA_G12_LPORT * FSL_EEE_PORT_SRM_STRIDE +
	      FSL_EEE_PORT_SRM_WORDS * sizeof(u32) <=
	      FSL91030M_SWITCH_WINDOW_SIZE,
	      "G12 EEE port row must fit the switch window");
static_assert(FSL_EPF_OUT_PORT_SRM +
	      FSL91030M_VEGA_G12_LPORT * FSL_EPF_OUT_PORT_SRM_STRIDE +
	      FSL_EPF_OUT_PORT_SRM_WORDS * sizeof(u32) <=
	      FSL91030M_SWITCH_WINDOW_SIZE,
	      "G12 EPF out-port row must fit the switch window");

static u32 fsl91030m_crc_hash96(unsigned int alg, const u32 *words)
{
	unsigned int width;
	u32 crc;
	u32 mask;
	u32 poly;
	unsigned int i;

	switch (alg) {
	case 0:
		width = 32;
		poly = 0x04c11db7u;
		mask = 0xffffffffu;
		break;
	case 1:
		width = 16;
		poly = 0x8005u;
		mask = 0x0000ffffu;
		break;
	case 2:
		width = 16;
		poly = 0x1021u;
		mask = 0x0000ffffu;
		break;
	default:
		return words[0];
	}

	crc = mask;
	for (i = 0; i < 96; i++) {
		unsigned int bit = 95 - i;
		u32 input = !!(words[bit / 32] & BIT(bit % 32));
		u32 msb = (crc >> (width - 1)) & 1;

		crc = (crc << 1) & mask;
		if (input != msb)
			crc ^= poly;
	}

	return crc & mask;
}

static u32 fsl91030m_inet_vlan_off(unsigned int vid)
{
	return FSL_INET_VLAN_SRM + vid * FSL_INET_VLAN_SRM_STRIDE;
}

static u32 fsl91030m_inet_port_off(u8 lport)
{
	return FSL_INET_PORT_SRM + lport * FSL_INET_PORT_SRM_STRIDE;
}

static u32 fsl91030m_eee_port_off(u8 lport)
{
	return FSL_EEE_PORT_SRM + lport * FSL_EEE_PORT_SRM_STRIDE;
}

static u32 fsl91030m_eee_vlan_op_off(unsigned int index)
{
	return FSL_EEE_VLAN_OP_SRM + index * FSL_EEE_VLAN_OP_SRM_STRIDE;
}

static u32 fsl91030m_ivt_port_off(u8 lport)
{
	return FSL_IVT_PORT_SRM + lport * FSL_IVT_PORT_SRM_STRIDE;
}

static u32 fsl91030m_epf_out_port_off(u8 lport)
{
	return FSL_EPF_OUT_PORT_SRM + lport * FSL_EPF_OUT_PORT_SRM_STRIDE;
}

static u32
fsl91030m_ivt_xlate_key_off(const struct fsl91030m_ivt_xlate_bank *bank,
			    unsigned int idx)
{
	return bank->key_base + idx * FSL_IVT_XLATE_SRM_STRIDE;
}

static u32
fsl91030m_ivt_xlate_action_off(const struct fsl91030m_ivt_xlate_bank *bank,
			       unsigned int idx)
{
	return bank->action_base + idx * FSL_IVT_XLATE_SRM_STRIDE;
}

static void
fsl91030m_ivt_xlate0_cvid_key(u8 lport, unsigned int cvid, u32 key[2])
{
	key[0] = BIT(0) | ((cvid & 0xfffu) << 17);
	key[1] = (lport & 0x3fu) << 1;
}

static void fsl91030m_ivt_xlate0_action(unsigned int vid, u32 action[2])
{
	action[0] = vid << 10;
	action[1] = 0;
}

static u32 fsl91030m_ivt_xlate0_index(const u32 key[2])
{
	u32 shifted[3] = {};
	unsigned int i;

	for (i = 0; i < 60; i++) {
		unsigned int src = i + 1;

		if (key[src / 32] & BIT(src % 32))
			shifted[i / 32] |= BIT(i % 32);
	}

	return fsl91030m_crc_hash96(0, shifted) & 0x7f;
}

/*
 * G5/G12 have packet-verified IVT translation hash slots; return them so the
 * caller can cross-check the software CRC against known-good hardware values.
 * Any other copper port returns -ENOENT: no anchor, so the computed hash slot
 * is trusted. Non-copper lports are rejected.
 */
static int fsl91030m_ivt_xlate0_expected_idx(u8 lport, unsigned int *idx)
{
	switch (lport) {
	case FSL91030M_VEGA_G5_LPORT:
		*idx = 4;
		return 0;
	case FSL91030M_VEGA_G12_LPORT:
		*idx = 21;
		return 0;
	default:
		return fsl91030m_port_lport_supported(lport) ? -ENOENT : -EINVAL;
	}
}

static bool fsl91030m_ivt_xlate_key_match(const u32 row[2], const u32 key[2])
{
	return (row[0] & BIT(0)) &&
	       ((row[0] ^ key[0]) & ~BIT(0)) == 0 &&
	       ((row[1] ^ key[1]) & GENMASK(28, 0)) == 0;
}

static int
fsl91030m_table_restore_exact(struct fsl91030m *sw, u32 off,
			      const u32 *saved, unsigned int count)
{
	u32 after[FSL_TBL_MAXW] = {};

	return fsl91030m_table_write_verify_exact(sw, off, saved, after, count);
}

static int
fsl91030m_direct_restore_exact(struct fsl91030m *sw, u32 off,
			       const u32 *saved, unsigned int count)
{
	u32 after[FSL_TBL_MAXW] = {};

	return fsl91030m_direct_write_verify_exact(sw, off, saved, after, count);
}

static int
fsl91030m_vlan_ctag_pick_ivt_slot(struct fsl91030m *sw, u8 lport,
				  struct fsl91030m_vlan_ctag_ivt_saved *saved)
{
	const struct fsl91030m_ivt_xlate_bank *bank;
	u32 action[FSL_IVT_XLATE_SRM_WORDS] = {};
	u32 key[FSL_IVT_XLATE_SRM_WORDS] = {};
	int first_empty = -1;
	int chosen = -1;
	unsigned int expected_idx;
	unsigned int idx;
	unsigned int i;
	int ret;

	memset(saved, 0, sizeof(*saved));
	fsl91030m_ivt_xlate0_cvid_key(lport, FSL91030M_CTAG_SERVICE_CVID,
				      key);
	fsl91030m_ivt_xlate0_action(FSL91030M_CTAG_SERVICE_VID, action);
	ret = fsl91030m_ivt_xlate0_expected_idx(lport, &expected_idx);
	if (ret && ret != -ENOENT)
		return ret;
	idx = fsl91030m_ivt_xlate0_index(key);
	if (ret == 0 && idx != expected_idx)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(fsl91030m_ivt_xlate_banks); i++) {
		const struct fsl91030m_ivt_xlate_bank *bank;
		u32 row[FSL_IVT_XLATE_SRM_WORDS] = {};
		u32 act[FSL_IVT_XLATE_SRM_WORDS] = {};
		bool match;
		bool valid;

		bank = &fsl91030m_ivt_xlate_banks[i];
		ret = fsl91030m_table_read(sw,
					   fsl91030m_ivt_xlate_key_off(bank, idx),
					   row, ARRAY_SIZE(row));
		if (ret)
			return ret;
		ret = fsl91030m_table_read(sw,
					   fsl91030m_ivt_xlate_action_off(bank, idx),
					   act, ARRAY_SIZE(act));
		if (ret)
			return ret;

		valid = !!(row[0] & BIT(0));
		match = fsl91030m_ivt_xlate_key_match(row, key);
		if (match) {
			if (memcmp(act, action, sizeof(action)))
				return -EBUSY;
			chosen = i;
			break;
		}
		if (!valid && first_empty < 0)
			first_empty = i;
	}

	if (chosen < 0)
		chosen = first_empty;
	if (chosen < 0)
		return -ENOSPC;

	saved->valid = true;
	saved->lport = lport;
	saved->bank = chosen;
	saved->idx = idx;
	bank = &fsl91030m_ivt_xlate_banks[chosen];

	ret = fsl91030m_table_read(sw, fsl91030m_ivt_port_off(lport),
				   saved->port_row,
				   ARRAY_SIZE(saved->port_row));
	if (ret)
		return ret;
	ret = fsl91030m_table_read(sw, fsl91030m_ivt_xlate_key_off(bank, idx),
				   saved->key_row,
				   ARRAY_SIZE(saved->key_row));
	if (ret)
		return ret;
	return fsl91030m_table_read(sw,
				    fsl91030m_ivt_xlate_action_off(bank, idx),
				    saved->action_row,
				    ARRAY_SIZE(saved->action_row));
}

static int
fsl91030m_vlan_ctag_restore_saved(struct fsl91030m *sw,
				  const struct fsl91030m_vlan_ctag_saved *saved)
{
	u32 eee_op_off = fsl91030m_eee_vlan_op_off(FSL91030M_CTAG_SERVICE_OP_IDX);
	u32 inet_off = fsl91030m_inet_vlan_off(FSL91030M_CTAG_SERVICE_VID);
	unsigned int i;
	int ret = 0;
	int rb_ret;

	for (i = 0; i < saved->n_members; i++) {
		const struct fsl91030m_vlan_ctag_ivt_saved *ivt = &saved->ivt[i];
		const struct fsl91030m_ivt_xlate_bank *bank;
		u32 action_off;
		u32 port_off;
		u32 key_off;

		if (!ivt->valid)
			continue;

		bank = &fsl91030m_ivt_xlate_banks[ivt->bank];
		action_off = fsl91030m_ivt_xlate_action_off(bank, ivt->idx);
		key_off = fsl91030m_ivt_xlate_key_off(bank, ivt->idx);
		port_off = fsl91030m_ivt_port_off(ivt->lport);

		if (ivt->key_row[0] & BIT(0)) {
			rb_ret = fsl91030m_table_restore_exact(sw, action_off,
							       ivt->action_row,
							       ARRAY_SIZE(ivt->action_row));
			if (rb_ret && !ret)
				ret = rb_ret;
			rb_ret = fsl91030m_table_restore_exact(sw, key_off,
							       ivt->key_row,
							       ARRAY_SIZE(ivt->key_row));
		} else {
			rb_ret = fsl91030m_table_restore_exact(sw, key_off,
							       ivt->key_row,
							       ARRAY_SIZE(ivt->key_row));
			if (rb_ret && !ret)
				ret = rb_ret;
			rb_ret = fsl91030m_table_restore_exact(sw, action_off,
							       ivt->action_row,
							       ARRAY_SIZE(ivt->action_row));
		}
		if (rb_ret && !ret)
			ret = rb_ret;

		rb_ret = fsl91030m_table_restore_exact(sw, port_off,
						       ivt->port_row,
						       ARRAY_SIZE(ivt->port_row));
		if (rb_ret && !ret)
			ret = rb_ret;
	}

	rb_ret = fsl91030m_direct_restore_exact(sw, FSL_IVT_XLATE_KEY_CTL,
						&saved->ivt_keyctl, 1);
	if (rb_ret && !ret)
		ret = rb_ret;

	rb_ret = fsl91030m_table_restore_exact(sw, inet_off,
					       saved->inet_vlan,
					       ARRAY_SIZE(saved->inet_vlan));
	if (rb_ret && !ret)
		ret = rb_ret;

	for (i = 0; i < saved->n_members; i++) {
		u8 lport = saved->members[i];

		rb_ret = fsl91030m_table_restore_exact(sw,
						       fsl91030m_eee_port_off(lport),
						       saved->eee_port[i],
						       ARRAY_SIZE(saved->eee_port[i]));
		if (rb_ret && !ret)
			ret = rb_ret;
	}

	rb_ret = fsl91030m_table_restore_exact(sw, eee_op_off,
					       saved->eee_vlan_op,
					       ARRAY_SIZE(saved->eee_vlan_op));
	if (rb_ret && !ret)
		ret = rb_ret;

	for (i = 0; i < saved->n_members; i++) {
		u8 lport = saved->members[i];

		rb_ret = fsl91030m_table_restore_exact(sw,
						       fsl91030m_inet_port_off(lport),
						       saved->inet_port[i],
						       ARRAY_SIZE(saved->inet_port[i]));
		if (rb_ret && !ret)
			ret = rb_ret;
	}

	return ret;
}

static int fsl91030m_vlan_ctag_apply(struct fsl91030m *sw, const u8 *members,
				     unsigned int n_members, u16 member_mask,
				     u16 untagged_mask)
{
	static const u32 inet_vlan[FSL_INET_VLAN_SRM_WORDS] = {
		0x00000001u, 0x00010200u, 0x00000000u,
		0x00000000u, 0x00000000u, 0x00000000u,
	};
	static const u32 eee_port[FSL_EEE_PORT_SRM_WORDS] = {
		0x00000000u, 0x104b0000u, 0x00044000u,
	};
	static const u32 eee_op[FSL_EEE_VLAN_OP_SRM_WORDS] = {
		0x00000050u, 0x00000000u,
	};
	struct fsl91030m_vlan_ctag_saved saved = {};
	u32 keyctl = FSL91030M_IVT_XLATE0_KEYCTL;
	u32 after[FSL_TBL_MAXW] = {};
	u32 eee_op_off;
	u32 inet_off;
	unsigned int i;
	unsigned int cvid = FSL91030M_CTAG_SERVICE_CVID;
	bool mutated = false;
	int ret;

	if (sw->vlan1107.active)
		return -EBUSY;
	if (n_members < 2 || n_members > FSL91030M_VLAN_CTAGSVC_PORTS)
		return -EINVAL;

	saved.active = true;
	saved.n_members = n_members;
	saved.member_mask = member_mask;
	saved.untagged_mask = untagged_mask;
	memcpy(saved.members, members, n_members);
	inet_off = fsl91030m_inet_vlan_off(FSL91030M_CTAG_SERVICE_VID);
	eee_op_off = fsl91030m_eee_vlan_op_off(FSL91030M_CTAG_SERVICE_OP_IDX);

	ret = fsl91030m_table_read(sw, inet_off,
				   saved.inet_vlan,
				   ARRAY_SIZE(saved.inet_vlan));
	if (ret)
		return ret;
	ret = fsl91030m_table_read(sw, eee_op_off,
				   saved.eee_vlan_op,
				   ARRAY_SIZE(saved.eee_vlan_op));
	if (ret)
		return ret;
	ret = fsl91030m_direct_read32(sw, FSL_IVT_XLATE_KEY_CTL,
				      &saved.ivt_keyctl);
	if (ret)
		return ret;

	for (i = 0; i < n_members; i++) {
		u8 lport = members[i];

		ret = fsl91030m_table_read(sw, fsl91030m_inet_port_off(lport),
					   saved.inet_port[i],
					   ARRAY_SIZE(saved.inet_port[i]));
		if (ret)
			return ret;
		ret = fsl91030m_table_read(sw, fsl91030m_eee_port_off(lport),
					   saved.eee_port[i],
					   ARRAY_SIZE(saved.eee_port[i]));
		if (ret)
			return ret;
		ret = fsl91030m_vlan_ctag_pick_ivt_slot(sw, lport,
							&saved.ivt[i]);
		if (ret)
			return ret;
	}

	mutated = true;
	ret = fsl91030m_table_write_verify_exact(sw, eee_op_off,
						 eee_op, after,
						 ARRAY_SIZE(eee_op));
	if (ret)
		goto rollback;

	for (i = 0; i < n_members; i++) {
		u8 lport = members[i];

		ret = fsl91030m_table_write_verify_exact(sw,
							 fsl91030m_eee_port_off(lport),
							 eee_port, after,
							 ARRAY_SIZE(eee_port));
		if (ret)
			goto rollback;
	}

	ret = fsl91030m_table_write_verify_exact(sw, inet_off,
						 inet_vlan, after,
						 ARRAY_SIZE(inet_vlan));
	if (ret)
		goto rollback;

	ret = fsl91030m_direct_write_verify_exact(sw, FSL_IVT_XLATE_KEY_CTL,
						  &keyctl, after, 1);
	if (ret)
		goto rollback;

	for (i = 0; i < n_members; i++) {
		struct fsl91030m_vlan_ctag_ivt_saved *ivt = &saved.ivt[i];
		const struct fsl91030m_ivt_xlate_bank *bank;
		u32 port_row[FSL_IVT_PORT_SRM_WORDS];
		u32 action[FSL_IVT_XLATE_SRM_WORDS] = {};
		u32 key[FSL_IVT_XLATE_SRM_WORDS] = {};
		u32 action_off;
		u32 port_off;
		u32 key_off;

		memcpy(port_row, ivt->port_row, sizeof(port_row));
		port_row[2] |= FSL91030M_IVT_PORT_XLATE0_EN;
		fsl91030m_ivt_xlate0_action(FSL91030M_CTAG_SERVICE_VID, action);
		fsl91030m_ivt_xlate0_cvid_key(ivt->lport, cvid, key);
		bank = &fsl91030m_ivt_xlate_banks[ivt->bank];
		action_off = fsl91030m_ivt_xlate_action_off(bank, ivt->idx);
		key_off = fsl91030m_ivt_xlate_key_off(bank, ivt->idx);
		port_off = fsl91030m_ivt_port_off(ivt->lport);

		ret = fsl91030m_table_write_verify_exact(sw, port_off,
							 port_row, after,
							 ARRAY_SIZE(port_row));
		if (ret)
			goto rollback;
		ret = fsl91030m_table_write_verify_exact(sw, action_off,
							 action, after,
							 ARRAY_SIZE(action));
		if (ret)
			goto rollback;
		ret = fsl91030m_table_write_verify_exact(sw, key_off,
							 key, after,
							 ARRAY_SIZE(key));
		if (ret)
			goto rollback;
	}

	for (i = 0; i < n_members; i++) {
		u8 lport = members[i];
		u32 port_row[FSL_INET_PORT_SRM_WORDS];

		memcpy(port_row, saved.inet_port[i], sizeof(port_row));
		port_row[0] |= FSL_INET_PORT_VLAN_FILTER_EN;

		ret = fsl91030m_table_write_verify_exact(sw,
							 fsl91030m_inet_port_off(lport),
							 port_row, after,
							 ARRAY_SIZE(port_row));
		if (ret)
			goto rollback;
	}

	sw->vlan1107 = saved;
	return 0;

rollback:
	if (mutated) {
		int rb_ret;

		rb_ret = fsl91030m_vlan_ctag_restore_saved(sw, &saved);
		if (rb_ret) {
			dev_err(sw->dev,
				"VID1107 rollback failed after ret=%d rollback=%d\n",
				ret, rb_ret);
			return rb_ret;
		}
	}

	return ret;
}

static int fsl91030m_vlan_ctag_restore(struct fsl91030m *sw)
{
	struct fsl91030m_vlan_ctag_saved saved = sw->vlan1107;
	int ret;

	if (!saved.active)
		return -ENOENT;

	ret = fsl91030m_vlan_ctag_restore_saved(sw, &saved);
	if (!ret)
		memset(&sw->vlan1107, 0, sizeof(sw->vlan1107));

	return ret;
}

static int
fsl91030m_vlan_epf_out_un_ctag_set(struct fsl91030m *sw, u8 lport, bool enable)
{
	u32 row[FSL_EPF_OUT_PORT_SRM_WORDS] = {};
	u32 after[FSL_EPF_OUT_PORT_SRM_WORDS] = {};
	u32 off;
	int ret;

	if (!fsl91030m_port_lport_supported(lport))
		return -EINVAL;

	off = fsl91030m_epf_out_port_off(lport);
	ret = fsl91030m_table_read(sw, off, row, ARRAY_SIZE(row));
	if (ret)
		return ret;

	if (enable)
		row[0] |= FSL_EPF_OUT_UN_CTAG;
	else
		row[0] &= ~FSL_EPF_OUT_UN_CTAG;

	return fsl91030m_table_write_verify_exact(sw, off, row, after,
						  ARRAY_SIZE(row));
}

int fsl91030m_l2_vlan1107_set(struct fsl91030m *sw, unsigned int member_mask,
			      unsigned int untagged_mask)
{
	const unsigned int copper_mask = GENMASK(FSL91030M_VEGA_PORT_COUNT - 1, 0);
	u8 members[FSL91030M_VLAN_CTAGSVC_PORTS];
	unsigned int n_members = 0;
	unsigned int i;
	bool want_active;
	int ret = 0;

	if (!sw || !sw->dev || !sw->full)
		return -ENODEV;

	member_mask &= copper_mask;
	untagged_mask &= member_mask;

	for (i = 0; i < FSL91030M_VEGA_PORT_COUNT; i++)
		if (member_mask & BIT(i))
			members[n_members++] = FSL91030M_VEGA_G5_LPORT + i;

	/* A VLAN forwards only with at least two member ports. */
	want_active = n_members >= 2;

	mutex_lock(&sw->op_lock);

	/*
	 * Reconcile against the applied state. Any change to the member set or
	 * the untagged (egress-strip) set is handled by tearing the service down
	 * and re-applying it, which keeps the save/restore bookkeeping simple.
	 */
	if (sw->vlan1107.active &&
	    (sw->vlan1107.member_mask != member_mask ||
	     sw->vlan1107.untagged_mask != untagged_mask)) {
		for (i = 0; i < sw->vlan1107.n_members; i++) {
			ret = fsl91030m_vlan_epf_out_un_ctag_set(sw,
						sw->vlan1107.members[i], false);
			if (ret)
				goto out_unlock;
		}
		ret = fsl91030m_vlan_ctag_restore(sw);
		if (ret)
			goto out_unlock;
	}

	if (!want_active || sw->vlan1107.active)
		goto out_unlock;

	ret = fsl91030m_vlan_ctag_apply(sw, members, n_members,
					member_mask, untagged_mask);
	if (ret)
		goto out_unlock;

	for (i = 0; i < n_members; i++) {
		bool untag = untagged_mask &
			     BIT(members[i] - FSL91030M_VEGA_G5_LPORT);

		ret = fsl91030m_vlan_epf_out_un_ctag_set(sw, members[i], untag);
		if (ret)
			goto out_unlock;
	}

out_unlock:
	mutex_unlock(&sw->op_lock);
	return ret;
}
