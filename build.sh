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
git clean -fdx -- target/linux/milkv_vega package/boot/vega-spl package/boot/uboot-milkv_vega || true

echo "==> Overlaying port files from $PORT_DIR"
rsync -a "$PORT_DIR/target/linux/milkv_vega/"       target/linux/milkv_vega/
rsync -a "$PORT_DIR/package/boot/vega-spl/"          package/boot/vega-spl/
rsync -a "$PORT_DIR/package/boot/uboot-milkv_vega/"  package/boot/uboot-milkv_vega/

echo "==> Setting up feeds"
./scripts/feeds update -a >/dev/null
./scripts/feeds install -a >/dev/null

echo "==> Configuring for milkv_vega"
cat > .config <<'EOF'
CONFIG_TARGET_milkv_vega=y
CONFIG_TARGET_milkv_vega_milkv_vega=y
CONFIG_TARGET_milkv_vega_milkv_vega_DEVICE_milkv_vega=y
EOF
make defconfig

echo "==> Building (jobs=$JOBS)"
make -j"$JOBS" V=s

mkdir -p bin/targets/milkv_vega/milkv_vega/
cp -v build_dir/target-riscv64_generic_musl/vega-spl/vega-spl.bin \
      bin/targets/milkv_vega/milkv_vega/vega-spl.bin

echo
ls -la bin/targets/milkv_vega/milkv_vega/*.bin bin/targets/milkv_vega/milkv_vega/*.ubi
popd >/dev/null
