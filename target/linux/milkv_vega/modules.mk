# SPDX-License-Identifier: GPL-2.0-only

define KernelPackage/fsl91030m-switch
  SUBMENU:=$(NETWORK_DEVICES_MENU)
  TITLE:=FisiLink FSL91030M Ethernet switch fabric support
  DEPENDS:=@TARGET_milkv_vega +kmod-xy1000-pdma
  KCONFIG:= \
	CONFIG_NET_VENDOR_FISILINK=y \
	CONFIG_FSL91030M_SWITCH
  FILES:=$(LINUX_DIR)/drivers/net/ethernet/fisilink/fsl91030m.ko
  AUTOLOAD:=$(call AutoLoad,36,fsl91030m,1)
endef

define KernelPackage/fsl91030m-switch/description
 Kernel module for the FisiLink FSL91030M switch fabric on Milk-V Vega.
endef

$(eval $(call KernelPackage,fsl91030m-switch))

define KernelPackage/xy1000-pdma
  SUBMENU:=$(NETWORK_DEVICES_MENU)
  TITLE:=FisiLink XY1000 packet-DMA host interface
  DEPENDS:=@TARGET_milkv_vega
  KCONFIG:= \
	CONFIG_NET_VENDOR_FISILINK=y \
	CONFIG_XY1000_PDMA
  FILES:=$(LINUX_DIR)/drivers/net/ethernet/fisilink/xy1000-pdma.ko
  AUTOLOAD:=$(call AutoLoad,35,xy1000-pdma,1)
endef

define KernelPackage/xy1000-pdma/description
 Kernel module for the FisiLink XY1000 packet-DMA CPU host interface on
 Milk-V Vega. The module exposes the CPU packet path as a standard Ethernet
 netdev and does not program switch fabric forwarding policy by itself.
endef

$(eval $(call KernelPackage,xy1000-pdma))
