# SPDX-License-Identifier: GPL-2.0-only
#
# Milk-V Vega first-stage SPL: DDR init + stitches OpenSBI/U-Boot/DTB
# into a single binary for SPI NOR @ 0x20000000.

include $(TOPDIR)/rules.mk

PKG_NAME:=vega-spl
PKG_VERSION:=1.0
PKG_RELEASE:=1
PKG_FLAGS:=nonshared

PKG_BUILD_DIR:=$(BUILD_DIR)/$(PKG_NAME)
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk

define Package/vega-spl
  SECTION:=boot
  CATEGORY:=Boot Loaders
  DEPENDS:=@TARGET_milkv_vega
  TITLE:=Milk-V Vega first-stage SPL
  URL:=https://milkv.io/
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(CP) $(STAGING_DIR_IMAGE)/fw_dynamic-generic.bin $(PKG_BUILD_DIR)/sbi.bin
	$(CP) $(STAGING_DIR_IMAGE)/u-boot.bin $(PKG_BUILD_DIR)/u-boot.bin
	dtc -I dts -O dtb \
		-o $(PKG_BUILD_DIR)/fdt.dtb \
		$(PKG_BUILD_DIR)/milkv-vega.dts
	cd $(PKG_BUILD_DIR) && \
	$(TARGET_CROSS)gcc -g -march=rv64imac_zicsr_zifencei -mabi=lp64 \
		-nostdlib -static -nostartfiles -T linker.lds \
		-o vega-spl.elf freeloader.S && \
	$(TARGET_CROSS)objcopy -O binary vega-spl.elf vega-spl.bin
endef

define Build/InstallDev
	$(INSTALL_DIR) $(STAGING_DIR_IMAGE)
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/vega-spl.bin $(STAGING_DIR_IMAGE)/vega-spl.bin
endef

define Package/vega-spl/install
endef

$(eval $(call BuildPackage,vega-spl))
