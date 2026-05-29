#!/usr/bin/env bash
# dump-flash.sh — dump SPI NOR flash to a file via XIP.
#
# Usage:
#   ./dump-flash.sh <out.bin> [size_bytes]
#
# Default size is 0x400000 (4 MB, full W25Q32).
set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 <out.bin> [size_bytes]" >&2
    exit 2
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KIT="${HERE}"
OPENOCD="${OPENOCD:-${KIT}/riscv-openocd/src/openocd}"
OPENOCD_CFG="${OPENOCD_CFG:-${KIT}/riscv-openocd/openocd-slow.cfg}"

OUT="$(cd "$(dirname "$1")" 2>/dev/null && pwd || pwd)/$(basename "$1")"
SIZE="${2:-0x400000}"

if [[ ! -x "${OPENOCD}" ]]; then
    echo "openocd not found at ${OPENOCD}. Run ${KIT}/bootstrap.sh first." >&2
    exit 1
fi
if [[ ! -f "${OPENOCD_CFG}" ]]; then
    echo "cfg not found at ${OPENOCD_CFG}" >&2
    exit 1
fi

exec "${OPENOCD}" \
    -f "${OPENOCD_CFG}" \
    -c "set OUT_PATH ${OUT}" \
    -c "set DUMP_SIZE ${SIZE}" \
    -f "${KIT}/dump-flash.tcl"
