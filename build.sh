#!/usr/bin/env bash
#
# Build OpenWrt firmware for Milk-V Vega.
# Outputs vega-spl.bin, ubifs-kernel.bin, ubifs-rootfs.ubi

set -euo pipefail

OPENWRT_URL="${OPENWRT_URL:-https://git.openwrt.org/openwrt/openwrt.git}"
OPENWRT_COMMIT="${OPENWRT_COMMIT:-6db1127}"
OPENWRT_DIR="${OPENWRT_DIR:-$(pwd)/openwrt}"
PORT_DIR="$(cd "$(dirname "$0")" && pwd)"
JOBS="${JOBS:-$(nproc)}"

if [[ ! -d "$OPENWRT_DIR/.git" ]]; then
    echo "==> Cloning OpenWrt into $OPENWRT_DIR"
    git clone "$OPENWRT_URL" "$OPENWRT_DIR"
fi

pushd "$OPENWRT_DIR" >/dev/null
echo "==> Resetting OpenWrt to $OPENWRT_COMMIT"
git fetch
git checkout --quiet "$OPENWRT_COMMIT"
git clean -fdx -- target/linux/milkv_vega \
    package/boot/vega-spl package/boot/uboot-milkv_vega || true

echo "==> Overlaying port files from $PORT_DIR"
rsync -a "$PORT_DIR/target/linux/milkv_vega/"       target/linux/milkv_vega/
rsync -a "$PORT_DIR/package/"                        package/

echo "==> Allowing opensbi to build for milkv_vega"
# Stock OpenWrt restricts opensbi-generic to sifiveu/d1; widen to include
# milkv_vega. Idempotent: only adds the target if it's not already listed.
if ! grep -q "TARGET_milkv_vega" package/boot/opensbi/Makefile; then
    sed -i 's/@(TARGET_sifiveu||TARGET_d1)/@(TARGET_sifiveu||TARGET_d1||TARGET_milkv_vega)/' \
        package/boot/opensbi/Makefile
fi

echo "==> Setting up feeds"
./scripts/feeds update -a >/dev/null
./scripts/feeds install -a >/dev/null

echo "==> Refreshing package metadata"
rm -f tmp/.packageinfo tmp/.config-package.in \
      tmp/info/.packageinfo-kernel_linux \
      tmp/info/.files-packageinfo* \
      tmp/info/.overrides-packageinfo-*
make prepare-tmpinfo >/dev/null

echo "==> Configuring for milkv_vega"
cat > .config <<'EOF'
CONFIG_TARGET_milkv_vega=y
CONFIG_TARGET_milkv_vega_milkv_vega=y
CONFIG_TARGET_milkv_vega_milkv_vega_DEVICE_milkv_vega=y
# CONFIG_KERNEL_DEBUG_FS is not set
CONFIG_PACKAGE_luci=y
CONFIG_PACKAGE_tc-full=y
CONFIG_PACKAGE_kmod-xy1000-pdma=y
CONFIG_PACKAGE_kmod-sched=y
CONFIG_PACKAGE_kmod-sched-red=y
CONFIG_PACKAGE_libi2c=y
CONFIG_PACKAGE_i2c-tools=y
# Recovery initramfs (RAM-root image) for in-system reflash of the mounted rootfs.
CONFIG_TARGET_ROOTFS_INITRAMFS=y
CONFIG_TARGET_INITRAMFS_COMPRESSION_XZ=y
EOF
make defconfig

echo "==> Cleaning target kernel build tree"
make target/linux/clean

echo "==> Building (jobs=$JOBS)"
make -j"$JOBS" V=s

mkdir -p bin/targets/milkv_vega/milkv_vega/
cp -v build_dir/target-riscv64_generic_musl/vega-spl/vega-spl.bin \
      bin/targets/milkv_vega/milkv_vega/vega-spl.bin

echo
ls -la bin/targets/milkv_vega/milkv_vega/*.bin bin/targets/milkv_vega/milkv_vega/*.ubi
popd >/dev/null
