#!/usr/bin/env bash
# sector-diff.sh — print sector indices that differ between two flash images.
#
# Usage:
#   ./sector-diff.sh <current.bin> <new.bin> [sector_size]
#
# Default sector size = 65536 (64 KB, W25Q32). Output: one sector index per line.
set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "Usage: $0 <current.bin> <new.bin> [sector_size]" >&2
    exit 2
fi

CUR="$1"
NEW="$2"
SECT="${3:-65536}"

CUR_SIZE=$(stat -f%z "${CUR}" 2>/dev/null || stat -c%s "${CUR}")
NEW_SIZE=$(stat -f%z "${NEW}" 2>/dev/null || stat -c%s "${NEW}")
MAX_SIZE=$(( CUR_SIZE > NEW_SIZE ? CUR_SIZE : NEW_SIZE ))
NSECT=$(( (MAX_SIZE + SECT - 1) / SECT ))

for ((i=0; i<NSECT; i++)); do
    off=$(( i * SECT ))
    h1=$(dd if="${CUR}" bs=1 skip=${off} count=${SECT} 2>/dev/null | shasum -a 256 | awk '{print $1}')
    h2=$(dd if="${NEW}" bs=1 skip=${off} count=${SECT} 2>/dev/null | shasum -a 256 | awk '{print $1}')
    if [[ "$h1" != "$h2" ]]; then
        echo "$i"
    fi
done
