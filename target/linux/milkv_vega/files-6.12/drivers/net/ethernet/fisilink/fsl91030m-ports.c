// SPDX-License-Identifier: GPL-2.0-only
/*
 * Board port map and driver-owned runtime state for the Milk-V Vega
 * FSL91030M switch fabric.
 */
#include <linux/build_bug.h>
#include <linux/kernel.h>

#include "fsl91030m.h"

#define FSL_VEGA_LPORT_FITS_ID(lport)	((lport) <= FSL91030M_PORT_ID_MASK)
#define FSL_VEGA_LPORT_FITS_DEF_MEMBER(lport) \
	((lport) <= FSL_INET_DEF_VLAN_MAX_LPORT)
#define FSL_VEGA_MAC_FITS_COUNTER(mac)	((mac) < FSL_CMAC_PORT_COUNT)
#define FSL_VEGA_PHY_FITS_ID(phy)	((phy) <= FSL91030M_PORT_ID_MASK)
#define FSL_VEGA_LPORTS_DISTINCT(a, b)	((a) != (b))
#define FSL_VEGA_MACS_DISTINCT(a, b)	((a) != (b))

static const struct fsl91030m_port_desc fsl91030m_ports[] = {
	{ .name = "g5", .mac = FSL91030M_VEGA_G5_MAC,
	  .lport = FSL91030M_VEGA_G5_LPORT,
	  .phy = FSL91030M_VEGA_G5_PHY },
	{ .name = "g6", .mac = FSL91030M_VEGA_G6_MAC,
	  .lport = FSL91030M_VEGA_G6_LPORT,
	  .phy = FSL91030M_VEGA_G6_PHY },
	{ .name = "g7", .mac = FSL91030M_VEGA_G7_MAC,
	  .lport = FSL91030M_VEGA_G7_LPORT,
	  .phy = FSL91030M_VEGA_G7_PHY },
	{ .name = "g8", .mac = FSL91030M_VEGA_G8_MAC,
	  .lport = FSL91030M_VEGA_G8_LPORT,
	  .phy = FSL91030M_VEGA_G8_PHY },
	{ .name = "g9", .mac = FSL91030M_VEGA_G9_MAC,
	  .lport = FSL91030M_VEGA_G9_LPORT,
	  .phy = FSL91030M_VEGA_G9_PHY },
	{ .name = "g10", .mac = FSL91030M_VEGA_G10_MAC,
	  .lport = FSL91030M_VEGA_G10_LPORT,
	  .phy = FSL91030M_VEGA_G10_PHY },
	{ .name = "g11", .mac = FSL91030M_VEGA_G11_MAC,
	  .lport = FSL91030M_VEGA_G11_LPORT,
	  .phy = FSL91030M_VEGA_G11_PHY },
	{ .name = "g12", .mac = FSL91030M_VEGA_G12_MAC,
	  .lport = FSL91030M_VEGA_G12_LPORT,
	  .phy = FSL91030M_VEGA_G12_PHY },
};

static_assert(ARRAY_SIZE(fsl91030m_ports) == FSL91030M_VEGA_PORT_COUNT,
	      "Vega switchdev port table must match the port enum");
static_assert(FSL_VEGA_LPORT_FITS_ID(FSL91030M_VEGA_G5_LPORT) &&
	      FSL_VEGA_LPORT_FITS_ID(FSL91030M_VEGA_G6_LPORT) &&
	      FSL_VEGA_LPORT_FITS_ID(FSL91030M_VEGA_G7_LPORT) &&
	      FSL_VEGA_LPORT_FITS_ID(FSL91030M_VEGA_G8_LPORT) &&
	      FSL_VEGA_LPORT_FITS_ID(FSL91030M_VEGA_G9_LPORT) &&
	      FSL_VEGA_LPORT_FITS_ID(FSL91030M_VEGA_G10_LPORT) &&
	      FSL_VEGA_LPORT_FITS_ID(FSL91030M_VEGA_G11_LPORT) &&
	      FSL_VEGA_LPORT_FITS_ID(FSL91030M_VEGA_G12_LPORT),
	      "Vega logical ports must fit packed hardware port fields");
static_assert(FSL_VEGA_LPORT_FITS_DEF_MEMBER(FSL91030M_VEGA_G5_LPORT) &&
	      FSL_VEGA_LPORT_FITS_DEF_MEMBER(FSL91030M_VEGA_G6_LPORT) &&
	      FSL_VEGA_LPORT_FITS_DEF_MEMBER(FSL91030M_VEGA_G7_LPORT) &&
	      FSL_VEGA_LPORT_FITS_DEF_MEMBER(FSL91030M_VEGA_G8_LPORT) &&
	      FSL_VEGA_LPORT_FITS_DEF_MEMBER(FSL91030M_VEGA_G9_LPORT) &&
	      FSL_VEGA_LPORT_FITS_DEF_MEMBER(FSL91030M_VEGA_G10_LPORT) &&
	      FSL_VEGA_LPORT_FITS_DEF_MEMBER(FSL91030M_VEGA_G11_LPORT) &&
	      FSL_VEGA_LPORT_FITS_DEF_MEMBER(FSL91030M_VEGA_G12_LPORT),
	      "Vega logical ports must fit the default membership register");
static_assert(FSL_VEGA_MAC_FITS_COUNTER(FSL91030M_VEGA_G5_MAC) &&
	      FSL_VEGA_MAC_FITS_COUNTER(FSL91030M_VEGA_G6_MAC) &&
	      FSL_VEGA_MAC_FITS_COUNTER(FSL91030M_VEGA_G7_MAC) &&
	      FSL_VEGA_MAC_FITS_COUNTER(FSL91030M_VEGA_G8_MAC) &&
	      FSL_VEGA_MAC_FITS_COUNTER(FSL91030M_VEGA_G9_MAC) &&
	      FSL_VEGA_MAC_FITS_COUNTER(FSL91030M_VEGA_G10_MAC) &&
	      FSL_VEGA_MAC_FITS_COUNTER(FSL91030M_VEGA_G11_MAC) &&
	      FSL_VEGA_MAC_FITS_COUNTER(FSL91030M_VEGA_G12_MAC),
	      "Vega MAC channels must fit the CMAC counter tables");
static_assert(FSL_VEGA_PHY_FITS_ID(FSL91030M_VEGA_G5_PHY) &&
	      FSL_VEGA_PHY_FITS_ID(FSL91030M_VEGA_G6_PHY) &&
	      FSL_VEGA_PHY_FITS_ID(FSL91030M_VEGA_G7_PHY) &&
	      FSL_VEGA_PHY_FITS_ID(FSL91030M_VEGA_G8_PHY) &&
	      FSL_VEGA_PHY_FITS_ID(FSL91030M_VEGA_G9_PHY) &&
	      FSL_VEGA_PHY_FITS_ID(FSL91030M_VEGA_G10_PHY) &&
	      FSL_VEGA_PHY_FITS_ID(FSL91030M_VEGA_G11_PHY) &&
	      FSL_VEGA_PHY_FITS_ID(FSL91030M_VEGA_G12_PHY),
	      "Vega PHY IDs must fit packed hardware port fields");
static_assert(FSL91030M_VEGA_G12_LPORT - FSL91030M_VEGA_G5_LPORT + 1 ==
	      FSL91030M_VEGA_PORT_COUNT,
	      "Vega RJ45 logical ports must be sequential");
static_assert(FSL91030M_VEGA_G12_MAC - FSL91030M_VEGA_G5_MAC + 1 ==
	      FSL91030M_VEGA_PORT_COUNT,
	      "Vega RJ45 MAC channels must be sequential");
static_assert(FSL91030M_VEGA_G12_PHY - FSL91030M_VEGA_G5_PHY + 1 ==
	      FSL91030M_VEGA_PORT_COUNT,
	      "Vega RJ45 PHY IDs must be sequential");
static_assert(FSL_VEGA_LPORTS_DISTINCT(FSL91030M_VEGA_G5_LPORT,
				       FSL91030M_VEGA_G12_LPORT) &&
	      FSL_VEGA_MACS_DISTINCT(FSL91030M_VEGA_G5_MAC,
				     FSL91030M_VEGA_G12_MAC),
	      "Vega RJ45 endpoints must be distinct");

const struct fsl91030m_port_desc *fsl91030m_port_get(unsigned int index)
{
	if (index >= ARRAY_SIZE(fsl91030m_ports))
		return NULL;

	return &fsl91030m_ports[index];
}

bool fsl91030m_port_lport_supported(u8 lport)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(fsl91030m_ports); i++) {
		if (fsl91030m_ports[i].lport == lport)
			return true;
	}

	return false;
}
