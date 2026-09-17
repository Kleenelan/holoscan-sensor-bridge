#!/bin/bash
# 启动 RoCEv2 分析器 + hololink-write 开关 ramplayer 的完整流程。
# 要点：
#   - 分析器 timeout 必须覆盖 配置(可能含重试) + on/off脚本(每次写一次往返) + 采集时间，
#     30s 不够，这里给 120s 兜底（正常 ~45s 即由 SIGINT 提前结束）。
#   - 用 $! 记录分析器 PID，采集完发 SIGINT 让其优雅收尾并 wait 到 TOTAL 打印完毕。
set -u

cd /home/ruler/ex_holoscan/tmp08_buils_hsb_test_no_hsdk/holoscan-sensor-bridge
export PATH=$PWD/build/tools/write:$PATH
S=$PWD/tools/roce_ramplayer_analyzer

# ① 后台启动 RoCEv2 分析器（自动枚举、配 QP、写 FPGA 数据面寄存器）
stdbuf -o0 -e0 timeout 120 ./build/tools/roce_ramplayer_analyzer/roce_ramplayer_analyzer \
    -f 4096 -i 3 --dump-first > /tmp/final.log 2>&1 &
APID=$!

# ② 等待其打印 "receiving..."（控制面就绪、数据面已配置）；若进程夭折则直接报错退出
ok=0
for i in $(seq 1 40); do
    sleep 1
    if grep -q "receiving" /tmp/final.log 2>/dev/null; then ok=1; break; fi
    kill -0 "$APID" 2>/dev/null || break   # 分析器已退出
done
if [ "$ok" != 1 ]; then
    echo "!! analyzer failed to reach receiving state, log:" >&2
    cat /tmp/final.log >&2
    exit 1
fi

# ③ 用 hololink-write 打开 ramplayer（4KiB/帧，每 10M usr_clk ≈ 31ms 一帧）
#    （脚本的 Aborted/core dumped 信息是控制面丢事务后的自动重试，属正常现象）
$S/ramplayer_on.sh -s 4096 -n 1 -t 10000000 -f 0 2>/dev/null | tail -1 || exit 1

sleep 11          # 采集 ~11 秒

# ④ 关闭 ramplayer
$S/ramplayer_off.sh 2>/dev/null | tail -1 || exit 1

# ⑤ 通知分析器收尾并等它退出（teardown + TOTAL 打印完成）
kill -INT "$APID" 2>/dev/null
wait "$APID" 2>/dev/null

cat /tmp/final.log
