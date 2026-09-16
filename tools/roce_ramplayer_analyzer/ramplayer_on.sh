#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# SPDX-License-Identifier: Apache-2.0
#
# Enable the FPGA RAM player (fpga/nv_hsb_ip/data_gen/ram_player.sv) by
# poking its APB registers with hololink-write.
#
# pynq/rfsoc-pynq address map (FPGA_top.sv: ram_player on apb_psel[4],
# user ports decode address[31:28], port i <-> nibble i+1):
#     0x5000_0000  scratch
#     0x5000_0004  enable: bit0 ram_ena | bit1 ptp_ena | bit2 loop_dis | bit3 ptp_bram_ena
#     0x5000_0008  timer: usr_clk cycles to wait between window bursts (0 = none)
#     0x5000_000C  window_size: bytes per window == one host "frame"
#     0x5000_0010  window_num: windows per burst
#     0x5010_0000+ RAM payload (512-bit x 512 words; written as 32-bit dwords)
#
# Usage:
#   ./ramplayer_on.sh [-H board_ip] [-s window_size] [-n window_num] [-t timer] [-f fill_dwords]
#     -H  board IP                 (default 192.168.0.2)
#     -s  window_size in bytes     (default 4096; must match analyzer --frame-size)
#     -n  window_num               (default 1; windows per burst)
#     -t  timer in usr_clk cycles  (default 10000000, ~30 ms @ ~330 MHz; 0 = back-to-back)
#     -f  fill RAM with an incrementing-dword pattern for this many dwords
#         (default 1024 = one 4 KiB window; 0 skips the fill; each dword costs
#         one hololink-write round trip, ~0.1-0.5 s)
set -u

HOLOLINK_IP=192.168.0.2
WINDOW_SIZE=4096
WINDOW_NUM=1
TIMER=10000000
FILL_DWORD_COUNT=1024

# Locate hololink-write: prefer the installed one next to this script,
# fall back to PATH, then to the in-tree build location.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -x "$SCRIPT_DIR/hololink-write" ]; then
    HL_WRITE="$SCRIPT_DIR/hololink-write"
elif command -v hololink-write >/dev/null 2>&1; then
    HL_WRITE=hololink-write
elif [ -x "$SCRIPT_DIR/../write/hololink-write" ]; then
    HL_WRITE="$SCRIPT_DIR/../write/hololink-write"
else
    echo "error: hololink-write not found next to $0, on PATH, or in ../write" >&2
    exit 1
fi

while getopts "H:s:n:t:f:h" opt; do
    case "$opt" in
        H) HOLOLINK_IP="$OPTARG" ;;
        s) WINDOW_SIZE="$OPTARG" ;;
        n) WINDOW_NUM="$OPTARG" ;;
        t) TIMER="$OPTARG" ;;
        f) FILL_DWORD_COUNT="$OPTARG" ;;
        h) sed -n '2,32p' "$0"; exit 0 ;;
        *) exit 1 ;;
    esac
done

RAM_BASE=0x50100000

# The pynq control plane sporadically drops transactions, and every
# hololink-write invocation first runs a full start() register burst —
# retry each invocation until it actually succeeds.
hl_write() {
    local attempt
    for attempt in $(seq 1 15); do
        if "$HL_WRITE" -H "$HOLOLINK_IP" "$1" "$2" >/dev/null 2>&1; then
            return 0
        fi
    done
    echo "error: hololink-write $1 $2 failed after 15 attempts" >&2
    return 1
}

echo ">> using $HL_WRITE (board $HOLOLINK_IP)"

if [ "$FILL_DWORD_COUNT" -gt 0 ]; then
    # RAM is 16 banks x 512 words (32-bit each). A frame dword j maps to
    # RAM word (j/16 + 1), lane (j%16) — the player pre-increments rd_addr
    # before streaming, so each window starts at word 1, and s_apb_ram_dyn
    # interleaves lanes by bank: address = base + lane*0x800 + word*4.
    echo ">> filling RAM with incrementing-dword pattern ($FILL_DWORD_COUNT dwords)..."
    i=0
    while [ "$i" -lt "$FILL_DWORD_COUNT" ]; do
        word=$(( i / 16 + 1 ))
        lane=$(( i % 16 ))
        addr=$(printf '0x%x' $((RAM_BASE + lane * 0x800 + word * 4)))
        hl_write "$addr" "$i" || exit 1
        if (( i % 256 == 0 )); then printf '\r   %d/%d' "$i" "$FILL_DWORD_COUNT"; fi
        i=$((i + 1))
    done
    printf '\r   %d/%d done\n' "$FILL_DWORD_COUNT" "$FILL_DWORD_COUNT"
fi

echo ">> disabling RAM player while updating registers"
hl_write 0x50000004 0 || exit 1

echo ">> window_size=$WINDOW_SIZE window_num=$WINDOW_NUM timer=$TIMER"
hl_write 0x50000008 "$TIMER" || exit 1
hl_write 0x5000000c "$WINDOW_SIZE" || exit 1
hl_write 0x50000010 "$WINDOW_NUM" || exit 1

echo ">> enabling RAM player (ram_ena=1)"
hl_write 0x50000004 1 || exit 1

echo ">> RAM player is ON: $((WINDOW_SIZE))-byte frame(s) x $WINDOW_NUM every $TIMER usr_clk cycles"
