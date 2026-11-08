#!/bin/bash
# lab8.sh —— 双段网关：协议转换、断链、双向路由
#
# 场景：
#
#   [engine]---vxcan0 ~~ vxcan1---[gw]---vxcan2 ~~ vxcan3---[J1939 接收端]
#               A 段 私有 CAN               B 段 J1939
#
# 要点：
#   1. 0x180 不是被"转发"，而是被**翻译**成 J1939 的 EEC1（0xF004）。
#      量纲不同（0.25 vs 0.125 rpm/bit），直接搬字节会得到 2 倍转速。
#   2. B 段 down 掉后网关不退出，继续接收 A 段并计数，等链路恢复。
#   3. 反方向也通：B 段发 0x200，A 段的发动机能收到。
#
# 用法： ./lab8.sh
cd /root/can-lab || exit 1

cleanup() {
	pkill -x gw 2>/dev/null
	pkill -x ecu_engine 2>/dev/null
	pkill -x j1939_node 2>/dev/null
	pkill -x candump 2>/dev/null
}
trap cleanup EXIT INT TERM

sec() {
	echo
	echo "============================================================"
	echo "$*"
	echo "============================================================"
}

cleanup
sleep 0.4

sec "E8.1  建立两段总线"
./08-gateway/topo.sh reset
sleep 0.6
./08-gateway/topo.sh status

sec "E8.2  A 段发私有帧，B 段收到 J1939"
cat <<'TXT'
A 段只有 11 位标准帧（0x180 / 0x181），B 段是 29 位 J1939。
网关做的是「解信号 -> 按另一套量纲重新编码」，不是原样转发：

    0x180  ENGINE_FAST   11 位 ID          rpm: 0.25 rpm/bit
    0xF004 EEC1          29 位 ID, SA=0x00 rpm: 0.125 rpm/bit

同时 0x181 做 ID 重映射（0x181 -> 0x281），演示"透传 + 改 ID"这种最常见的规则。
TXT
echo
./03-app/ecu_engine vxcan0 > /tmp/l8_engine.log 2>&1 &
sleep 0.4
./08-gateway/gw vxcan1 vxcan2 --stats 4 --trace 6 > /tmp/l8_gw.log 2>&1 &
sleep 0.4
./07-j1939/j1939_node vxcan3 0x90 0x3000 --rx-only --rx-pgn 0xF004 \
	> /tmp/l8_rx.log 2>&1 &
sleep 1.2

echo "--- 网关的转换明细（同一批帧，左边 A 段字节，右边 B 段字节）---"
grep -E "^\[vxcan1->vxcan2\]" /tmp/l8_gw.log | head -6
echo
echo "注意两边字节完全不同：0x180 的转速是 0.25 rpm/bit，EEC1 是 0.125 rpm/bit，"
echo "同一个物理量在两种协议里的原始值差一倍。解出来的物理值才是相等的。"
echo
echo "--- B 段（vxcan3）上实际出现的帧 ---"
timeout 2 candump -ta -e vxcan3 > /tmp/l8_busB.log 2>&1
head -4 /tmp/l8_busB.log
echo
echo "--- J1939 接收端解出来的内容（前 3 条）---"
grep -E "^\[RX" /tmp/l8_rx.log | head -3

sec "E8.3  把 B 段接口 down 掉：真实故障形态"
cat <<'TXT'
这与阶段 4 的 link_watch 不一样：那次是把接口**删掉**，socket 绑定的 ifindex 失效，
必须重新 bind。这里接口还在、只是不通，write 返回 ENETDOWN ——
真实 ECU 掉线、线束断开、收发器休眠，都是这个形态。
网关必须继续活着，因为 A 段一切正常。
TXT
echo
ip link set vxcan2 down
echo "\$ ip link set vxcan2 down"
sleep 4
echo
echo "--- 网关日志（断开前后）---"
grep -E "不可用|恢复|B段" /tmp/l8_gw.log | tail -4

sec "E8.4  恢复 B 段：自动接上，不需要重启"
ip link set vxcan2 up
echo "\$ ip link set vxcan2 up"
sleep 4
echo
grep -E "不可用|恢复" /tmp/l8_gw.log | tail -3
echo
echo "--- 恢复后 B 段又有帧了 ---"
timeout 1.5 candump -ta -e vxcan3 > /tmp/l8_busB2.log 2>&1
head -3 /tmp/l8_busB2.log

sec "E8.5  反方向：B 段的命令回到 A 段"
echo "\$ cansend vxcan3 200#010A"
timeout 2 candump -ta -e vxcan0 > /tmp/l8_busA.log 2>&1 &
sleep 0.4
cansend vxcan3 200#010A
sleep 1
echo
echo "--- A 段（vxcan0）看到 0x200 ---"
grep " 200 " /tmp/l8_busA.log
echo
echo "--- 发动机收到命令后的反应 ---"
grep -E "0x200|降扭" /tmp/l8_engine.log | tail -3

sec "E8.6  网关统计"
# 先让网关优雅退出，才能拿到「含最后一帧」的最终账
pkill -TERM -x gw 2>/dev/null
sleep 0.6
tail -8 /tmp/l8_gw.log
echo
echo "对比一下：规则 0x180 的「进」比「出」多出的部分，就是 B 段断开期间丢掉的。"
echo "真实网关在这里还要决定策略：丢最旧的、丢优先级低的，还是缓存等恢复。"

cleanup
sec "小结"
cat <<'TXT'
1. 网关的核心不是"转发"，而是**翻译**：两段总线的 ID 宽度、信号量纲、
   报文周期都可能不同，逐字节搬运是最典型的错误做法。
2. 断链要区分两种：接口被删（ifindex 失效，必须重新 bind）
   和接口不通（ENETDOWN，socket 仍然有效）。后者才是车上最常见的。
3. 网关必须自己记账。用内核过滤器会让"丢了多少"变成不可见，
   而网关的价值恰恰在于能说清楚每一帧去了哪里。
4. A 段 10 ms 的报文搬到 B 段，带宽需求变了 —— 真实网关要做速率限制
   和优先级映射，否则低速段会被高速段灌满。
TXT
