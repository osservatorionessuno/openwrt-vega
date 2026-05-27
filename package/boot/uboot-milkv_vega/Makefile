# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_VERSION:=2026.04
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://source.denx.de/u-boot/u-boot.git
PKG_SOURCE_VERSION:=v2026.04
PKG_MIRROR_HASH:=skip

include $(INCLUDE_DIR)/u-boot.mk
include $(INCLUDE_DIR)/package.mk

define U-Boot/Default
  BUILD_TARGET:=milkv_vega
  BUILD_SUBTARGET:=milkv_vega
  UBOOT_IMAGE:=u-boot.bin
  DTS_DIR:=arch/riscv/dts
endef

define U-Boot/milkv_vega
  NAME:=Milk-V Vega
  UBOOT_CONFIG:=milkv_vega
  BUILD_DEVICES:=milkv_vega
endef

UBOOT_TARGETS := milkv_vega

define Build/InstallDev
	$(INSTALL_DIR) $(STAGING_DIR_IMAGE)
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/$(UBOOT_IMAGE) $(STAGING_DIR_IMAGE)/u-boot.bin
endef

define Package/u-boot/install/default
endef

$(eval $(call BuildPackage/U-Boot))
