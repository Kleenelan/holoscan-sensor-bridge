#!/bin/bash
# 延迟测量专用流程（ramplayer + roce_ramplayer_analyzer）。
#
# 输出三类延迟指标（详见 run_ramplayer.md §5 "延迟测量说明"）：
#   1. rtt           控制面 UDP 往返（min 近似链路往返延迟；启动时探测 50 次）
#   2. fpga-latency  FPGA 内部延迟：metadata 发出 - 帧首数据到达（同一 FPGA 时钟域，精确）
#   3. host-interval 到达抖动：主机收帧间隔 对比 FPGA 发帧间隔（fpga-interval）
#
# 用法： bash ./run_latency.sh [frame_size=4096] [timer_usr_clk=1000000] [采集秒=15]
#   默认帧率约 322 fps（timer=1M @322.27MHz ≈ 3.1ms），15s 约 4800 帧，抖动统计更平滑。
set -u

cd /home/ruler/ex_holoscan/tmp08_buils_hsb_test_no_hsdk/holoscan-sensor-bridge
export PATH=$PWD/build/tools/write:$PATH
S=$PWD/tools/roce_ramplayer_analyzer

FRAME_SIZE=${1:-4096}
TIMER=${2:-1000000}
COLLECT_S=${3:-15}
RTT_SAMPLES=50
LOG=/tmp/latency.log

# ① 后台启动分析器（RTT 探测 50 次；统计每 5s 一行）
stdbuf -o0 -e0 timeout 240 ./build/tools/roce_ramplayer_analyzer/roce_ramplayer_analyzer \
    -f "$FRAME_SIZE" -i 5 --rtt "$RTT_SAMPLES" --dump-first > "$LOG" 2>&1 &
APID=$!

# ② 等待 "receiving..."（控制面 + 数据面就绪）；夭折则报错退出
ok=0
for i in $(seq 1 40); do
    sleep 1
    grep -q "receiving" "$LOG" 2>/dev/null && { ok=1; break; }
    kill -0 "$APID" 2>/dev/null || break
done
if [ "$ok" != 1 ]; then
    echo "!! analyzer failed to reach receiving state, log:" >&2
    cat "$LOG" >&2
    exit 1
fi

# ③ 打开 ramplayer
"$S/ramplayer_on.sh" -s "$FRAME_SIZE" -n 1 -t "$TIMER" -f 0 2>/dev/null | tail -1 || exit 1

# ④ 采集
sleep "$COLLECT_S"

# ⑤ 关闭 ramplayer，分析器收尾
"$S/ramplayer_off.sh" 2>/dev/null | tail -1 || exit 1
kill -INT "$APID" 2>/dev/null
wait "$APID" 2>/dev/null

# ⑥ 汇总延迟指标
echo
echo "================ 延迟测量结果 ================"
grep -E "^rtt:" "$LOG"
echo "----------------------------------------------"
grep "fpga-latency" "$LOG" | tail -4
echo "----------------------------------------------"
echo "完整日志: $LOG"
