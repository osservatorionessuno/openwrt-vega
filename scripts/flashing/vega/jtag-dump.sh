#!/usr/bin/env bash
# jtag-dump.sh — halt the CPU and print a register/MMU snapshot for triage.
#
# Usage: jtag-dump.sh
#
# Env: OPENOCD, OPENOCD_CFG (same as flash.sh)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KIT="${HERE}"
OPENOCD="${OPENOCD:-${KIT}/riscv-openocd/src/openocd}"
OPENOCD_CFG="${OPENOCD_CFG:-${KIT}/riscv-openocd/openocd-slow.cfg}"

[[ -x "$OPENOCD"     ]] || { echo "openocd not found: $OPENOCD" >&2; exit 1; }
[[ -f "$OPENOCD_CFG" ]] || { echo "cfg not found: $OPENOCD_CFG" >&2; exit 1; }

"$OPENOCD" -f "$OPENOCD_CFG" -f "$HERE/jtag-dump.tcl" 2>&1 \
    | sed -n '/=====/,$p' \
    | grep -vE '^Info |^Warn |^DEPRECATED|^Open On-Chip|^Licensed|^For bug|http:|^riscv\.cpu halted|^cleared protection|^Listening on port|^shutdown command'
