// SPDX-License-Identifier: GPL-2.0-only
/*
 * Standard Linux traffic-control offload for the FSL91030M copper switch
 * datapath (G5..G12).
 *
 * Only packet-proven hardware blocks are accepted.  In particular, the EDST
 * priority-to-queue classifier, the DSCP priority map, and queue counters are
 * not programmed here because hardware testing showed that mutating them can
 * break forwarding (and, for the G5 DSCP map, did not recover without a power
 * cycle).  The supported ETS profile therefore maps Linux priorities to ETS
 * band 7 and only programs that port's egress scheduler row for that band, and
 * RED only touches the port's queue-7 green QWRED row.
 *
 * The scheduler and QWRED tables are per-logical-port with a fixed stride, so
 * this safe subset applies uniformly to every copper port.  The G5 and G12
 * rows are runtime traffic-validated; G6..G11 are enabled by structural
 * symmetry with those rows and have NOT been individually traffic-tested.
 */

#include <linux/bitfield.h>
#include <linux/build_bug.h>
#include <linux/errno.h>
#include <linux/netdevice.h>
#include <linux/overflow.h>
#include <net/pkt_cls.h>
#include <net/sch_generic.h>

#include "fsl91030m.h"

#define FSL_QOS_QUEUE_MAX		7u
#define FSL_QOS_Q7_BITMAP		BIT(7)
#define FSL_QOS_TC_ETS_BANDS		8u
#define FSL_QOS_QUANTUM_MAX		3u
#define FSL_QOS_SCHED_WRR_PRI_MAX	15u
#define FSL_QOS_SCHED_WEIGHT_MAX	127u
#define FSL_QOS_QWRED_GREEN		2u
#define FSL_QOS_QWRED_COLOR_MAX		2u
#define FSL_QOS_QWRED_THRESH_MAX	0x3fffu
#define FSL_QOS_QWRED_DROP_MAX		15u
#define FSL_QOS_QWRED_WEIGHT_MAX	15u

#define FSL_QOS_SCHED_DEFAULT_BMP	0u
#define FSL_QOS_SCHED_DEFAULT_MODE_DWRR	1u
#define FSL_QOS_SCHED_DEFAULT_QUANTUM	0u
#define FSL_QOS_SCHED_DEFAULT_WRR_PRI	1u

#define FSL_QOS_TC_ETS_Q7_MODE_WRR	0u
#define FSL_QOS_TC_ETS_Q7_QUANTUM	1u
#define FSL_QOS_TC_ETS_Q7_WRR_PRI	0u

#define FSL_QOS_QWRED_DEFAULT_START	100u
#define FSL_QOS_QWRED_DEFAULT_END	120u
#define FSL_QOS_QWRED_DEFAULT_DROP	15u
#define FSL_QOS_QWRED_DEFAULT_WEIGHT	0u

static const u8 fsl_qos_sched_default_weights[FSL_QOS_TC_ETS_BANDS] = {
	1, 2, 3, 4, 5, 6, 7, 8,
};

static_assert(FSL_TS_QUE_SHP_ENTRY_WORDS <= FSL_TBL_MAXW,
	      "queue shaper rows must fit the table transfer buffer");
static_assert(FSL_TD_QWRED_WORDS <= FSL_TBL_MAXW,
	      "QWRED rows must fit the table transfer buffer");
static_assert(FSL_TS_QUE_SHP_ENTRY_WORDS ==
	      FSL_TS_QUE_SHP_QUE_WORDS * 4,
	      "queue shaper entry layout expects four queues");
static_assert(FSL_TRAFFIC_DROP + FSL_TD_QWRED_OFF +
	      ((FSL91030M_VEGA_G12_LPORT * FSL_TD_QWRED_QUEUES_PER_PORT +
		FSL_QOS_QUEUE_MAX) * FSL_TD_QWRED_STRIDE) +
	      FSL_TD_QWRED_WORDS * sizeof(u32) <=
	      FSL91030M_SWITCH_WINDOW_SIZE,
	      "G12 QWRED row must fit the switch window");
static_assert(FSL_TRAFFIC_SCHEDULE + FSL_TS_QUE_SHP_OFF +
	      (FSL91030M_VEGA_G12_LPORT *
	       FSL_TS_QUE_SHP_LPORT_STRIDE) +
	      FSL_TS_QUE_SHP_ENTRY_WORDS * sizeof(u32) <=
	      FSL91030M_SWITCH_WINDOW_SIZE,
	      "G12 queue shaper row must fit the switch window");
static_assert(FSL_QOS_QUEUE_MAX + 1 == FSL_QOS_TC_ETS_BANDS,
	      "ETS band count must match hardware queue count");
/*
 * The G12 window-fit asserts above use the highest copper lport, so they bound
 * the worst case for every copper port.  The per-port RED state is indexed by
 * (lport - G5_LPORT); guarantee the copper lports are contiguous and that the
 * highest index stays inside the per-port array.
 */
static_assert(FSL91030M_VEGA_G12_LPORT - FSL91030M_VEGA_G5_LPORT + 1 ==
	      FSL91030M_VEGA_PORT_COUNT,
	      "copper lports must be contiguous and match the per-port array size");

static u32 fsl_qos_que_sch_off(u8 lport)
{
	return FSL_TRAFFIC_SCHEDULE + FSL_TS_QUE_SCH_OFF +
	       lport * FSL_TS_QUE_SCH_STRIDE;
}

static u32 fsl_qos_qwred_off(u8 lport, u8 queue)
{
	u32 row = lport * FSL_TD_QWRED_QUEUES_PER_PORT + queue;

	return FSL_TRAFFIC_DROP + FSL_TD_QWRED_OFF +
	       row * FSL_TD_QWRED_STRIDE;
}

static bool fsl_qos_port_egress_mutable(u8 lport)
{
	/*
	 * Any copper front-panel port (G5..G12) owns an independent egress
	 * scheduler row and per-queue QWRED rows, so the safe ETS/RED subset
	 * can be offloaded on all of them.
	 */
	return fsl91030m_port_lport_supported(lport);
}

static int fsl_qos_sched_set(struct fsl91030m *sw, u8 lport,
			     const u8 weight[FSL_QOS_TC_ETS_BANDS],
			     u8 bmp, u8 mode, u8 quantum, u8 wrr_pri)
{
	u32 after[FSL_TS_QUE_SCH_WORDS] = {};
	u32 row[FSL_TS_QUE_SCH_WORDS] = {};
	unsigned int i;

	if (!fsl_qos_port_egress_mutable(lport) || !weight ||
	    mode > 1 || quantum > FSL_QOS_QUANTUM_MAX ||
	    wrr_pri > FSL_QOS_SCHED_WRR_PRI_MAX)
		return -EINVAL;

	for (i = 0; i < FSL_QOS_TC_ETS_BANDS; i++) {
		if (weight[i] > FSL_QOS_SCHED_WEIGHT_MAX)
			return -EINVAL;
	}

	row[0] = FIELD_PREP(FSL_TS_QUE_SCH_Q0_WEIGHT, weight[0]) |
		 FIELD_PREP(FSL_TS_QUE_SCH_Q1_WEIGHT, weight[1]) |
		 FIELD_PREP(FSL_TS_QUE_SCH_Q2_WEIGHT, weight[2]) |
		 FIELD_PREP(FSL_TS_QUE_SCH_Q3_WEIGHT, weight[3]);
	row[1] = FIELD_PREP(FSL_TS_QUE_SCH_Q4_WEIGHT, weight[4]) |
		 FIELD_PREP(FSL_TS_QUE_SCH_Q5_WEIGHT, weight[5]) |
		 FIELD_PREP(FSL_TS_QUE_SCH_Q6_WEIGHT, weight[6]) |
		 FIELD_PREP(FSL_TS_QUE_SCH_Q7_WEIGHT, weight[7]);
	row[2] = FIELD_PREP(FSL_TS_QUE_SCH_WRR_QUANTUM, quantum) |
		 FIELD_PREP(FSL_TS_QUE_SCH_WRR_PRI, wrr_pri) |
		 FIELD_PREP(FSL_TS_QUE_SCH_BMP, bmp);
	if (mode)
		row[2] |= FSL_TS_QUE_SCH_MODE_DWRR;

	return fsl91030m_table_write_verify_exact(sw, fsl_qos_que_sch_off(lport),
						  row, after, ARRAY_SIZE(row));
}

static int fsl_qos_sched_restore(struct fsl91030m *sw, u8 lport)
{
	return fsl_qos_sched_set(sw, lport, fsl_qos_sched_default_weights,
				 FSL_QOS_SCHED_DEFAULT_BMP,
				 FSL_QOS_SCHED_DEFAULT_MODE_DWRR,
				 FSL_QOS_SCHED_DEFAULT_QUANTUM,
				 FSL_QOS_SCHED_DEFAULT_WRR_PRI);
}

static int fsl_qos_qwred_set(struct fsl91030m *sw, u8 lport, u8 queue,
			     u8 color, u16 start, u16 end, u8 max_drop,
			     u8 weight, bool color_aware, bool wred_enable,
			     bool adm_enable)
{
	u32 after[FSL_TD_QWRED_WORDS] = {};
	u32 row[FSL_TD_QWRED_WORDS] = {};
	int ret;

	if (!fsl_qos_port_egress_mutable(lport) || queue != FSL_QOS_QUEUE_MAX ||
	    color > FSL_QOS_QWRED_COLOR_MAX ||
	    start > FSL_QOS_QWRED_THRESH_MAX ||
	    end > FSL_QOS_QWRED_THRESH_MAX ||
	    max_drop > FSL_QOS_QWRED_DROP_MAX ||
	    weight > FSL_QOS_QWRED_WEIGHT_MAX)
		return -EINVAL;

	ret = fsl91030m_table_read(sw, fsl_qos_qwred_off(lport, queue), row,
				   ARRAY_SIZE(row));
	if (ret)
		return ret;

	row[color] = FIELD_PREP(FSL_TD_QWRED_START, start) |
		     FIELD_PREP(FSL_TD_QWRED_END, end) |
		     FIELD_PREP(FSL_TD_QWRED_MAX_DROP, max_drop);
	row[3] = FIELD_PREP(FSL_TD_QWRED_WEIGHT, weight);
	if (color_aware)
		row[3] |= FSL_TD_QWRED_COLOR_AWARE;
	if (wred_enable)
		row[3] |= FSL_TD_QWRED_WRED_EN;
	if (adm_enable)
		row[3] |= FSL_TD_QWRED_ADM_EN;

	return fsl91030m_table_write_verify_exact(sw,
						  fsl_qos_qwred_off(lport, queue),
						  row, after, ARRAY_SIZE(row));
}

static int fsl_qos_qwred_restore(struct fsl91030m *sw, u8 lport)
{
	return fsl_qos_qwred_set(sw, lport, FSL_QOS_QUEUE_MAX,
				 FSL_QOS_QWRED_GREEN,
				 FSL_QOS_QWRED_DEFAULT_START,
				 FSL_QOS_QWRED_DEFAULT_END,
				 FSL_QOS_QWRED_DEFAULT_DROP,
				 FSL_QOS_QWRED_DEFAULT_WEIGHT,
				 false, false, true);
}

static bool
fsl_qos_ets_is_supported_profile(const struct tc_ets_qopt_offload *ets)
{
	const struct tc_ets_qopt_offload_replace_params *p = &ets->replace_params;
	unsigned int i;

	if (ets->parent != TC_H_ROOT || p->bands != FSL_QOS_TC_ETS_BANDS)
		return false;

	/*
	 * Linux ETS defaults all priorities to the last band.  That is the only
	 * profile accepted here because changing the hardware priority map is not
	 * part of the safe production surface yet.
	 */
	for (i = 0; i <= TC_PRIO_MAX; i++) {
		if (p->priomap[i] != FSL_QOS_QUEUE_MAX)
			return false;
	}

	for (i = 0; i < FSL_QOS_QUEUE_MAX; i++) {
		if (p->quanta[i] || p->weights[i])
			return false;
	}

	return p->quanta[FSL_QOS_QUEUE_MAX] != 0;
}

static int fsl_qos_setup_tc_ets(struct fsl91030m *sw, u8 lport,
				struct tc_ets_qopt_offload *ets)
{
	u8 weight[FSL_QOS_TC_ETS_BANDS] = {};
	int ret;

	if (!fsl_qos_port_egress_mutable(lport))
		return -EOPNOTSUPP;

	mutex_lock(&sw->op_lock);
	switch (ets->command) {
	case TC_ETS_REPLACE:
		if (!fsl_qos_ets_is_supported_profile(ets)) {
			ret = -EOPNOTSUPP;
			break;
		}

		memset(weight, 1, sizeof(weight));
		ret = fsl_qos_sched_set(sw, lport, weight, FSL_QOS_Q7_BITMAP,
					FSL_QOS_TC_ETS_Q7_MODE_WRR,
					FSL_QOS_TC_ETS_Q7_QUANTUM,
					FSL_QOS_TC_ETS_Q7_WRR_PRI);
		break;
	case TC_ETS_DESTROY:
		ret = fsl_qos_sched_restore(sw, lport);
		break;
	case TC_ETS_GRAFT:
		ret = ets->graft_params.band == FSL_QOS_QUEUE_MAX ?
		      0 : -EOPNOTSUPP;
		break;
	case TC_ETS_STATS:
		ret = 0;
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}
	mutex_unlock(&sw->op_lock);

	return ret;
}

static u8 fsl_qos_red_probability_to_drop(u32 probability)
{
	u64 scaled = (u64)probability * FSL_QOS_QWRED_DROP_MAX;

	return min_t(u64, DIV_ROUND_CLOSEST_ULL(scaled, U32_MAX),
		     FSL_QOS_QWRED_DROP_MAX);
}

static bool fsl_qos_red_parent_supported(u32 parent)
{
	if (parent == TC_H_ROOT)
		return true;

	return TC_H_MIN(parent) == FSL_QOS_QUEUE_MAX + 1;
}

static struct fsl91030m_qos_red_state *
fsl_qos_red_state(struct fsl91030m *sw, u8 lport)
{
	if (!fsl_qos_port_egress_mutable(lport))
		return NULL;

	/* Copper lports G5..G12 are contiguous, so index the per-port slot. */
	return &sw->qos.red[lport - FSL91030M_VEGA_G5_LPORT];
}

static void fsl_qos_red_state_clear(struct fsl91030m *sw, u8 lport)
{
	struct fsl91030m_qos_red_state *state = fsl_qos_red_state(sw, lport);

	if (!state)
		return;

	state->active = false;
	state->handle = 0;
	state->parent = 0;
}

static void fsl_qos_red_state_set(struct fsl91030m *sw, u8 lport,
				  const struct tc_red_qopt_offload *red)
{
	struct fsl91030m_qos_red_state *state = fsl_qos_red_state(sw, lport);

	if (!state)
		return;

	state->active = true;
	state->handle = red->handle;
	state->parent = red->parent;
}

static bool fsl_qos_red_state_matches(struct fsl91030m *sw, u8 lport,
				      const struct tc_red_qopt_offload *red)
{
	const struct fsl91030m_qos_red_state *state;

	state = fsl_qos_red_state(sw, lport);

	return state && state->active && state->handle == red->handle &&
	       state->parent == red->parent;
}

static int fsl_qos_setup_tc_red(struct fsl91030m *sw, u8 lport,
				struct tc_red_qopt_offload *red)
{
	const struct tc_red_qopt_offload_params *p = &red->set;
	u8 max_drop;
	int ret;

	if (!fsl_qos_port_egress_mutable(lport))
		return -EOPNOTSUPP;

	mutex_lock(&sw->op_lock);
	switch (red->command) {
	case TC_RED_REPLACE:
		if (!fsl_qos_red_parent_supported(red->parent) ||
		    p->is_ecn || p->is_harddrop || p->is_nodrop ||
		    p->min > FSL_QOS_QWRED_THRESH_MAX ||
		    p->max > FSL_QOS_QWRED_THRESH_MAX ||
		    p->min > p->max) {
			/*
			 * Only revert the hardware QWRED row if this rejected
			 * replace actually targets the currently-offloaded RED.
			 * Root and ETS-band-8 RED both map to the same G12 q7
			 * row, so an unsupported RED for a different handle must
			 * not tear down a still-active supported RED offload.  A
			 * same-parent replace with a new handle is reverted by
			 * the later stale-handle TC_RED_DESTROY instead.
			 */
			if (fsl_qos_red_state_matches(sw, lport, red)) {
				fsl_qos_red_state_clear(sw, lport);
				ret = fsl_qos_qwred_restore(sw, lport);
				if (!ret)
					ret = -EOPNOTSUPP;
			} else {
				ret = -EOPNOTSUPP;
			}
			break;
		}

		max_drop = fsl_qos_red_probability_to_drop(p->probability);
		ret = fsl_qos_qwred_set(sw, lport, FSL_QOS_QUEUE_MAX,
					FSL_QOS_QWRED_GREEN, p->min, p->max,
					max_drop,
					FSL_QOS_QWRED_DEFAULT_WEIGHT,
					false, true, true);
		if (ret)
			fsl_qos_red_state_clear(sw, lport);
		else
			fsl_qos_red_state_set(sw, lport, red);
		break;
	case TC_RED_DESTROY:
		if (fsl_qos_red_state_matches(sw, lport, red)) {
			fsl_qos_red_state_clear(sw, lport);
			ret = fsl_qos_qwred_restore(sw, lport);
		} else {
			ret = 0;
		}
		break;
	case TC_RED_GRAFT:
	case TC_RED_STATS:
	case TC_RED_XSTATS:
		ret = fsl_qos_red_state_matches(sw, lport, red) ?
		      0 : -EOPNOTSUPP;
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}
	mutex_unlock(&sw->op_lock);

	return ret;
}

int fsl91030m_qos_setup_tc(struct fsl91030m *sw,
			   const struct fsl91030m_port_desc *desc,
			   enum tc_setup_type type, void *type_data)
{
	if (!sw || !desc || !type_data)
		return -EINVAL;

	switch (type) {
	case TC_SETUP_QDISC_ETS:
		return fsl_qos_setup_tc_ets(sw, desc->lport, type_data);
	case TC_SETUP_QDISC_RED:
		return fsl_qos_setup_tc_red(sw, desc->lport, type_data);
	default:
		return -EOPNOTSUPP;
	}
}
