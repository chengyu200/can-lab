#!/bin/bash
#
# gen_traffic.sh —— 用 cansend 造四路模拟流量，充当阶段 2 分发器的「对手盘」
#
# 用法: ./gen_traffic.sh [ifname]      默认 vcan0，Ctrl-C 结束
#
# 四路流量：
#   0x180  ENGINE_FAST   ~10ms   （约 70 帧/秒，见下面说明）
#   0x181  ENGINE_SLOW   ~100ms
#   0x200  DASH_CMD      ~500ms
#   0x2FF  干扰帧         ~500ms  <- 故意发一个协议外的 ID，
#                                    用来验证内核过滤器确实把它丢掉了
#
# 注意 1：这里每 10ms 起一个 cansend 进程，实际周期会被进程创建开销拉长到
#   14ms 左右（实测约 70 帧/秒），抖动也大。这正是阶段 4 要引入 CAN_BCM
#   （把周期发送交给内核）的原因——先留着，是为了让你亲眼看到
#   「用户态定时器做周期发送」有多不靠谱。
#
# 注意 2：干扰帧原本写的是 0x999，结果 candump 里看到的是 0x199。
#   原因是 11 位标准帧的 ID 上限只有 0x7FF，0x999 的 bit11 被内核静默截断，
#   而 cansend 照样返回 0。这条踩坑记录保留在 README 里。

IFACE="${1:-vcan0}"
pids=()

cleanup() {
    # 先把子循环停掉，再兜底清一遍 cansend，避免留下孤儿进程持续发帧
    kill "${pids[@]}" 2>/dev/null
    wait 2>/dev/null
    pkill -x cansend 2>/dev/null
}
# HUP 也必须有：ssh 会话断开时内核发的就是 SIGHUP，
# 不处理的话四个 while 循环会被 init 收编，在后台一直发帧。
trap cleanup EXIT INT TERM HUP

echo "在 $IFACE 上造流量，Ctrl-C 结束"
echo "  0x180 10ms / 0x181 100ms / 0x200 500ms / 0x2FF 500ms"

while :; do cansend "$IFACE" 180#1122334455667788; sleep 0.01;  done & pids+=($!)
while :; do cansend "$IFACE" 181#2100FF0000000000; sleep 0.1;   done & pids+=($!)
while :; do cansend "$IFACE" 200#0100;             sleep 0.5;   done & pids+=($!)
while :; do cansend "$IFACE" 2FF#DEADBEEF;         sleep 0.5;   done & pids+=($!)

wait
