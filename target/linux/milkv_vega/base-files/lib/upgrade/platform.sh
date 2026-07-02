# SPDX-License-Identifier: GPL-2.0-only
#
# In-system (non-JTAG) sysupgrade for Milk-V Vega.
#
# Layout: the kernel is a uImage on the raw SPI-NAND `kernel_nand` mtd
# partition; the root filesystem is a UBIFS volume named `rootfs` in the UBI
# attached to the `ubifs_nand` mtd partition.  nand_do_upgrade() writes the
# kernel with `mtd write` and the rootfs with `ubiupdatevol`, run from the
# sysupgrade ramdisk so the mounted NAND rootfs is not in use.

REQUIRE_IMAGE_METADATA=1
RAMFS_COPY_BIN='fw_printenv fw_setenv'

platform_check_image() {
	# Image compatibility is enforced by the sysupgrade metadata
	# (SUPPORTED_DEVICES = milkv,vega) via fwtool before this runs.
	return 0
}

platform_do_upgrade() {
	CI_KERNPART="kernel_nand"
	CI_UBIPART="ubifs_nand"
	CI_ROOTPART="rootfs"
	nand_do_upgrade "$1"
}
