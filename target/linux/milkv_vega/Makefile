# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk

ARCH:=riscv64
BOARD:=milkv_vega
BOARDNAME:=Milk-V Vega (Fisilink FSL91030M)
FEATURES:=squashfs nand ubifs
SUBTARGETS:=milkv_vega

KERNEL_PATCHVER:=6.12
KERNELNAME:=Image

include $(INCLUDE_DIR)/target.mk

DEFAULT_PACKAGES += uboot-envtools

define Target/Description
	Build firmware images for Milk-V Vega (Fisilink FSL91030M, Nuclei UX608)
endef

$(eval $(call BuildTarget))
