# SPDX-License-Identifier: GPL-2.0-only
include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/image.mk

KERNEL_LOADADDR := 0x41200000

define Device/Default
	PROFILES := Default
	KERNEL_NAME := Image
	KERNEL := kernel-bin | gzip | uImage gzip
	KERNEL_SUFFIX := -uImage
	IMAGES := sysupgrade.bin kernel.bin rootfs.ubi
	IMAGE/sysupgrade.bin := sysupgrade-tar | append-metadata
	IMAGE/kernel.bin := append-kernel
	IMAGE/rootfs.ubi := append-ubi
endef

define Device/milkv_vega
	$(call Device/Default)
	DEVICE_VENDOR := Milk-V
	DEVICE_MODEL := Vega
	DEVICE_DTS := fisilink/milkv-vega
	DEVICE_PACKAGES := ip-bridge kmod-fsl91030m-switch kmod-xy1000-pdma
	SUPPORTED_DEVICES := milkv,vega
	KERNEL_SIZE := 12288k
	BLOCKSIZE := 128k
	PAGESIZE := 2048
	SUBPAGESIZE := 2048
	IMAGE_SIZE := 40m
	MKUBIFS_OPTS := -m 2048 -e 126976 -c 320
endef
TARGET_DEVICES += milkv_vega

$(eval $(call BuildImage))
