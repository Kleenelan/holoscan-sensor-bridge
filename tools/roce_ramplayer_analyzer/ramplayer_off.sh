#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES.
# SPDX-License-Identifier: Apache-2.0
#
# Disable the FPGA RAM player: clear ram_ena (register 0x5000_0004 = 0).
# Usage: ./ramplayer_off.sh [-H board_ip]
set -u

HOLOLINK_IP=192.168.0.2
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -x "$SCRIPT_DIR/hololink-write" ]; then
    HL_WRITE="$SCRIPT_DIR/hololink-write"
elif command -v hololink-write >/dev/null 2>&1; then
    HL_WRITE=hololink-write
elif [ -x "$SCRIPT_DIR/../write/hololink-write" ]; then
    HL_WRITE="$SCRIPT_DIR/../write/hololink-write"
else
    echo "error: hololink-write not found" >&2
    exit 1
fi

while getopts "H:h" opt; do
    case "$opt" in
        H) HOLOLINK_IP="$OPTARG" ;;
        h) sed -n '2,8p' "$0"; exit 0 ;;
        *) exit 1 ;;
    esac
done

# Retry: the pynq control plane sporadically drops transactions (see
# ramplayer_on.sh).
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

echo ">> disabling RAM player on $HOLOLINK_IP"
hl_write 0x50000004 0 || exit 1
echo ">> RAM player is OFF"
