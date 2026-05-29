#!/usr/bin/env bash
# flash.sh — Milk-V Vega flasher (NOR + NAND kernel/rootfs).
#
# Usage:
#   flash.sh nor          <file>     # SPI NOR, sectors 0..N
#   flash.sh nand kernel  <file>     # NAND, kernel partition (offset 0)
#   flash.sh nand rootfs  <file>     # NAND, ubifs_nand (offset 0x800000) + zero rest
#
# Environment overrides:
#   OPENOCD     path to openocd binary
#   OPENOCD_CFG path to openocd board cfg
#   WSTUB       NAND write stub binary
#   ESTUB       NAND erase stub binary
set -euo pipefail

if [[ $# -lt 2 ]]; then
    cat >&2 <<EOF
Usage: $0 nor <file>
       $0 nand kernel <file>
       $0 nand rootfs <file>
EOF
    exit 2
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KIT="${HERE}"
OPENOCD="${OPENOCD:-${KIT}/riscv-openocd/src/openocd}"
OPENOCD_CFG="${OPENOCD_CFG:-${KIT}/riscv-openocd/openocd-slow.cfg}"
WSTUB="${WSTUB:-${KIT}/stubs/nand_write_stub.bin}"
ESTUB="${ESTUB:-${KIT}/stubs/nand_erase_stub.bin}"

[[ -x "$OPENOCD"      ]] || { echo "openocd not found: $OPENOCD" >&2; exit 1; }
[[ -f "$OPENOCD_CFG"  ]] || { echo "cfg not found: $OPENOCD_CFG" >&2; exit 1; }

dev="$1"; shift

# --- nor: SPI NOR via flash-region.tcl ---
nor_flash() {
    local file="$1"
    [[ -f "$file" ]] || { echo "file not found: $file" >&2; exit 1; }
    local size=$(stat -f%z "$file" 2>/dev/null || stat -c%s "$file")
    local sectors=$(( (size + 65535) / 65536 ))
    local last=$(( sectors - 1 ))
    local sha=$(shasum -a 256 "$file" | awk '{print $1}')

    echo "NOR  ← $file  ($(numfmt --to=iec --suffix=B $size 2>/dev/null || echo ${size}B)) sha=${sha:0:12}…"
    echo "  sectors 0..$last"

    local staged=/tmp/.flash_nor_$$.bin
    cp "$file" "$staged"
    "$OPENOCD" -f "$OPENOCD_CFG" \
        -c "set IMG_PATH $staged" \
        -c "set FIRST_SECTOR 0" \
        -c "set LAST_SECTOR $last" \
        -f "${KIT}/flash-region.tcl" 2>&1 \
        | grep -E "write done|Error" | tail -3
    rm -f "$staged"
}

# --- shared NAND helpers ---
nand_pad_to_pages() {
    local file="$1" out="$2"
    local size=$(stat -f%z "$file" 2>/dev/null || stat -c%s "$file")
    local pages=$(( (size + 2047) / 2048 ))
    local padded=$(( pages * 2048 ))
    cp "$file" "$out"
    if (( padded > size )); then
        head -c $((padded - size)) </dev/zero | tr '\0' '\377' >> "$out"
    fi
    echo "$pages"
}

# Generate and run a write-stub Tcl for given parameters.
nand_write_pages() {
    local src="$1" first_page="$2" pages="$3"
    local tcl=/tmp/.flash_nand_w_$$.tcl
    cat > "$tcl" <<EOF
adapter_khz 8000
halt
set WSTUB_ADDR 0x47000000
set SRC_ADDR   0x48000000
set SPI        0x10016000
reg priv 3; reg satp 0
reg mstatus 0x00001800
reg dcsr 0x4000b8c3
mww [expr {\$SPI + 0x00}] 3
mww [expr {\$SPI + 0x04}] 0
mww [expr {\$SPI + 0x10}] 1
mww [expr {\$SPI + 0x14}] 0xf
mww [expr {\$SPI + 0x40}] 0x80000
load_image $src                  \$SRC_ADDR   bin
load_image $WSTUB                \$WSTUB_ADDR bin
reg pc   \$WSTUB_ADDR
reg a0   \$SPI
reg a1   \$SRC_ADDR
reg a2   $first_page
reg a3   $pages
reg a4   1
reg sp   [expr {\$WSTUB_ADDR + 0xf000}]
set t0 [clock seconds]
puts "[clock format [clock seconds] -format %T] writing $pages pages @ page $first_page"
resume
catch {wait_halt 600000}
puts "[clock format [clock seconds] -format %T] write done in [expr {[clock seconds] - \$t0}] s"
shutdown
EOF
    "$OPENOCD" -f "$OPENOCD_CFG" -f "$tcl" 2>&1 | grep -E "writing|write done|Error" | tail -5
    rm -f "$tcl"
}

# Erase NAND blocks [first, last_excl).
nand_erase_blocks() {
    local first="$1" last_excl="$2"
    [[ -f "$ESTUB" ]] || { echo "erase stub not found: $ESTUB (run ${KIT}/bootstrap.sh)" >&2; exit 1; }
    local tcl=/tmp/.flash_nand_e_$$.tcl
    cat > "$tcl" <<EOF
adapter_khz 8000
halt
set ESTUB_ADDR 0x47000000
set SPI        0x10016000
reg priv 3; reg satp 0
reg mstatus 0x00001800
reg dcsr 0x4000b8c3
mww [expr {\$SPI + 0x00}] 3
mww [expr {\$SPI + 0x04}] 0
mww [expr {\$SPI + 0x10}] 1
mww [expr {\$SPI + 0x14}] 0xf
mww [expr {\$SPI + 0x40}] 0x80000
load_image $ESTUB \$ESTUB_ADDR bin
reg pc   \$ESTUB_ADDR
reg a0   \$SPI
reg a1   $first
reg a2   $last_excl
reg sp   [expr {\$ESTUB_ADDR + 0xf000}]
set t0 [clock seconds]
puts "[clock format [clock seconds] -format %T] erasing blocks $first..$((last_excl - 1))"
resume
catch {wait_halt 120000}
puts "[clock format [clock seconds] -format %T] erase done in [expr {[clock seconds] - \$t0}] s"
shutdown
EOF
    "$OPENOCD" -f "$OPENOCD_CFG" -f "$tcl" 2>&1 | grep -E "erasing|erase done|Error" | tail -5
    rm -f "$tcl"
}

# --- nand kernel: write at NAND offset 0 (block 0) ---
nand_kernel() {
    local file="$1"
    [[ -f "$file" ]] || { echo "file not found: $file" >&2; exit 1; }
    [[ -f "$WSTUB" ]] || { echo "write stub not found: $WSTUB (run ${KIT}/bootstrap.sh)" >&2; exit 1; }
    local size=$(stat -f%z "$file" 2>/dev/null || stat -c%s "$file")
    local sha=$(shasum -a 256 "$file" | awk '{print $1}')

    local staged=/tmp/.flash_kern_$$.bin
    local pages=$(nand_pad_to_pages "$file" "$staged")
    echo "NAND kernel ← $file  (${size}B, $pages pages)  sha=${sha:0:12}…"
    nand_write_pages "$staged" 0 "$pages"
    rm -f "$staged"
}

# --- nand rootfs: write at NAND offset 0x800000 (page 4096) + erase tail ---
nand_rootfs() {
    local file="$1"
    [[ -f "$file" ]] || { echo "file not found: $file" >&2; exit 1; }
    [[ -f "$WSTUB" ]] || { echo "write stub not found: $WSTUB (run ${KIT}/bootstrap.sh)" >&2; exit 1; }
    local size=$(stat -f%z "$file" 2>/dev/null || stat -c%s "$file")
    local sha=$(shasum -a 256 "$file" | awk '{print $1}')

    local staged=/tmp/.flash_rootfs_$$.bin
    local pages=$(nand_pad_to_pages "$file" "$staged")
    # mtd1 (ubifs_nand) starts at block 64 / page 4096.
    # Last block touched (exclusive) = 64 + ceil(pages/64).
    local last_block=$(( 64 + (pages + 63) / 64 ))
    echo "NAND rootfs ← $file  (${size}B, $pages pages)  sha=${sha:0:12}…"
    echo "  ubifs_nand region: blocks 64..$((last_block - 1)) used, blocks $last_block..1023 will be erased"
    nand_write_pages "$staged" 4096 "$pages"
    nand_erase_blocks "$last_block" 1024
    rm -f "$staged"
}

case "$dev" in
    nor)
        nor_flash "$1"
        ;;
    nand)
        sub="$1"; shift
        case "$sub" in
            kernel)  nand_kernel  "$1" ;;
            rootfs)  nand_rootfs  "$1" ;;
            *) echo "Unknown nand sub-target: $sub (use 'kernel' or 'rootfs')" >&2; exit 2 ;;
        esac
        ;;
    *)
        echo "Unknown device: $dev" >&2
        exit 2
        ;;
esac
