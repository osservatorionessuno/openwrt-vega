#!/usr/bin/env bash
# Clone the Nuclei riscv-openocd fork, apply the Vega flash patches, and build.
# Idempotent: safe to re-run.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_URL="${OPENOCD_REPO_URL:-https://github.com/riscv-mcu/riscv-openocd}"
# Pinned to the commit the patches were generated against. Rebase the patches
# before changing this ref.
REPO_REF="${OPENOCD_REPO_REF:-7b954ec9939d69cf4bd88762f392b63fbebf0285}"
SRC_DIR="${OPENOCD_SRC_DIR:-${HERE}/riscv-openocd}"
JOBS="${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)}"

if [[ "${SKIP_DEPS:-0}" != "1" ]]; then
    echo "==> ensuring build deps (macOS via brew, Linux via apt)"
    if [[ "$(uname)" == "Darwin" ]]; then
        if ! command -v brew >/dev/null; then
            echo "Install Homebrew first, or rerun with SKIP_DEPS=1 if deps are already installed." >&2
            exit 1
        fi
        brew list libusb       >/dev/null 2>&1 || brew install libusb
        brew list libftdi      >/dev/null 2>&1 || brew install libftdi
        brew list libtool      >/dev/null 2>&1 || brew install libtool
        brew list automake     >/dev/null 2>&1 || brew install automake
        brew list autoconf     >/dev/null 2>&1 || brew install autoconf
        brew list pkg-config   >/dev/null 2>&1 || brew install pkg-config
    elif command -v apt-get >/dev/null; then
        sudo apt-get update
        sudo apt-get install -y build-essential libtool autoconf automake \
            texinfo pkg-config libusb-1.0-0-dev libftdi1-dev
    fi
else
    echo "==> skipping dependency install (SKIP_DEPS=1)"
fi

if [[ ! -d "${SRC_DIR}/.git" ]]; then
    echo "==> cloning ${REPO_URL}"
    git clone "${REPO_URL}" "${SRC_DIR}"
fi

echo "==> checking out ${REPO_REF}"
git -C "${SRC_DIR}" fetch origin --tags
git -C "${SRC_DIR}" checkout --force "${REPO_REF}"
git -C "${SRC_DIR}" reset --hard "${REPO_REF}"
git -C "${SRC_DIR}" clean -fdx -e build/

echo "==> applying patches"
for p in "${HERE}/patches"/*.patch; do
    echo "    $(basename "$p")"
    git -C "${SRC_DIR}" apply --whitespace=nowarn "$p"
done

echo "==> installing Vega OpenOCD board config"
cp "${HERE}/configs/openocd-board.cfg" "${SRC_DIR}/openocd-slow.cfg"

echo "==> configuring"
pushd "${SRC_DIR}" >/dev/null
./bootstrap
./configure --enable-ftdi --enable-xlnx-pcie-xvc=no --disable-werror
echo "==> building OpenOCD (jobs=${JOBS})"
make -j"${JOBS}"
popd >/dev/null

echo "==> generating NAND helper stubs"
python3 "${HERE}/stubs/asm_read_stub.py"  "${HERE}/stubs/nand_read_stub.bin"
python3 "${HERE}/stubs/asm_write_stub.py" "${HERE}/stubs/nand_write_stub.bin"
python3 "${HERE}/stubs/asm_erase_stub.py" "${HERE}/stubs/nand_erase_stub.bin"

echo
echo "==> done"
echo "    OpenOCD: ${SRC_DIR}/src/openocd"
echo "    Config:  ${HERE}/configs/openocd-board.cfg"
echo "    Compat:  ${SRC_DIR}/openocd-slow.cfg"
echo
echo "    Example:"
echo "    OPENOCD=${SRC_DIR}/src/openocd OPENOCD_CFG=${SRC_DIR}/openocd-slow.cfg \\"
echo "      ${HERE}/flash.sh nor /path/to/vega-spl.bin"
