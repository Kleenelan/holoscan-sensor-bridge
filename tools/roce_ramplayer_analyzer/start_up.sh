cd /home/ruler/ex_holoscan/tmp08_buils_hsb_test_no_hsdk/holoscan-sensor-bridge
export PATH=$PWD/build/tools/write:$PATH
S=$PWD/tools/roce_ramplayer_analyzer

# <1.> 后台启动 RoCEv2 分析器（自动枚举、配 QP、写 FPGA 数据面寄存器）
(stdbuf -o0 -e0 timeout 30 ./build/tools/roce_ramplayer_analyzer/roce_ramplayer_analyzer \
    -f 4096 -i 3 --dump-first > /tmp/final.log 2>&1 &)

# <2.> 等待其打印 "receiving..."（控制面就绪、数据面已配置）
for i in $(seq 1 25); do sleep 1; grep -q "receiving" /tmp/final.log && break; done

# <3.> 用 hololink-write 打开 ramplayer（4KiB/帧，每 10M usr_clk ≈ 31ms 一帧）
$S/ramplayer_on.sh -s 4096 -n 1 -t 10000000 -f 0

sleep 11          # 采集 ~11 秒

# <4.> 关闭 ramplayer；分析器随后自动打印 TOTAL 统计
$S/ramplayer_off.sh
cat /tmp/final.log
