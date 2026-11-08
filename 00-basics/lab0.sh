#!/bin/bash
#
# lab0.sh —— 阶段 0：只用命令行工具观察 CAN，不写一行代码
#
# 用法: ./lab0.sh [ifname]        默认 vcan0
#
# 这一阶段的目的不是"学会用工具"，而是先建立三个直觉：
#   1) 一帧在总线上到底长什么样、占多少时间；
#   2) 帧的哪些部分会被工具隐藏、哪些会暴露出来；
#   3) vcan 与真实 CAN 控制器在哪些地方"看起来一样"、哪些地方"根本不一样"。
#
# 全程只读 + 少量自己发的帧，不改系统配置（唯一的例外是 E0.8 会短暂
# down/up 一次 vcan0，脚本退出时保证恢复 up）。

set -u

IFACE="${1:-vcan0}"
TMP=$(mktemp -d /tmp/lab0.XXXXXX)

cleanup() {
	pkill -x cansend 2>/dev/null
	ip link set "$IFACE" up 2>/dev/null
	rm -rf "$TMP"
}
trap cleanup EXIT INT TERM HUP

sec() {
	printf '\n============================================================\n'
	printf '%s\n' "$*"
	printf '============================================================\n'
}

# 后台抓一段时间，返回后由调用方 cat 文件
start_dump() {			# start_dump <秒> <输出文件>
	timeout -s INT "$1" candump -ta "$IFACE" > "$2" 2>&1 &
	DUMP_PID=$!
	sleep 0.4
}

stop_dump() {
	wait "$DUMP_PID" 2>/dev/null
}

# ---------------------------------------------------------------- E0.1
sec "E0.1  这个接口在系统里是什么"
echo '$ uname -m';                uname -m
echo '$ ip -d link show '"$IFACE"; ip -d link show "$IFACE" | head -4
echo '$ lsmod | grep -E "can"'
lsmod | grep -E "^can|vcan" | awk '{printf "  %-12s %8s\n", $1, $2}'
echo
printf '  CAN_RAW 协议栈: '
grep -q "CAN_RAW" /proc/net/protocols && echo "已加载" || echo "未加载"
echo
echo '注意 mtu=72：这不是以太网 MTU，而是 vcan 能承载的最大 CAN FD 帧长。'
echo '经典 CAN 帧固定 16 字节（struct can_frame），FD 帧用到完整的 72。'

# ---------------------------------------------------------------- E0.2
sec "E0.2  七种帧形态：cansend 语法 <-> candump 显示"
echo '下面每行先给出 cansend 的写法，再给出 candump 看到的样子。'
echo '差别全在"语法"和"显示"两层，线上是同一个东西。'
echo

start_dump 3 "$TMP/e2.log"

echo '$ cansend '"$IFACE"' 123#1122334455667788      # 标准帧，8 字节'
cansend "$IFACE" 123#1122334455667788

echo '$ cansend '"$IFACE"' 123#                    # 标准帧，零长度'
cansend "$IFACE" 123#

echo '$ cansend '"$IFACE"' 123#R                   # 远程请求帧，请求 0 字节'
cansend "$IFACE" 123#R

echo '$ cansend '"$IFACE"' 123#R8                  # 远程请求帧，请求 8 字节'
cansend "$IFACE" 123#R8

echo '$ cansend '"$IFACE"' 18FF50E5#0102           # 8 位十六进制 ID -> 自动变扩展帧'
cansend "$IFACE" 18FF50E5#0102

echo '$ cansend '"$IFACE"' 123##1AABB              # ## + 标志位 + 数据 -> CAN FD，带 BRS'
cansend "$IFACE" 123##1AABB

echo '$ cansend '"$IFACE"' 123##3<128 个 hex 字符>  # CAN FD + BRS，64 字节'
cansend "$IFACE" "123##3$(printf '5A%.0s' $(seq 1 64))"

stop_dump
echo
echo 'candump 实际收到:'
sed 's/^/  /' "$TMP/e2.log"
echo
echo '读法：'
echo '  [8]   经典帧，8 字节          [02]  CAN FD 帧，2 字节（前导零是 FD 的标记）'
echo '  [0] + remote request          远程请求帧，DLC 表示"我想读几个字节"'
echo '  扩展帧的 ID 列会右移到 8 位宽度，和标准帧的 3 位宽度对齐方式不同'
echo '  注意 RTR 帧在总线上没有数据场，但 DLC 仍然有意义（它编码期望长度）'

# ---------------------------------------------------------------- E0.3
sec "E0.3  过滤：candump 自己就能装过滤器"
echo '语法是  <接口>,<id>:<mask>   —— 与 C 代码里的 can_filter 完全同构。'
echo
start_dump 3 "$TMP/e3.log"
echo '$ cansend 依次发 0x180 0x181 0x190 0x280，但 candump 只订 0x180/0x7FF'
timeout -s INT 2.5 candump -ta "$IFACE,180:7FF" > "$TMP/e3f.log" 2>&1 &
sleep 0.4
for id in 180 181 190 280; do cansend "$IFACE" "$id#AA"; done
stop_dump
wait 2>/dev/null
echo '  candump 无过滤时看到:'; sed 's/^/    /' "$TMP/e3.log"
echo '  candump vcan0,180:7FF 时看到:'; sed 's/^/    /' "$TMP/e3f.log"
echo
echo '  mask=0x7FF 表示 11 位全比较（精确匹配）；mask=0x7F0 则只比较高 7 位，'
echo '  一次收 0x180~0x18F 一整段。这就是阶段 2 在 C 里做的事，只是换了层壳。'

# ---------------------------------------------------------------- E0.4
sec "E0.4  日志与回放：把总线录下来"
cd "$TMP" || exit 1
rm -f candump-*.log
timeout -s INT 2.5 candump -l "$IFACE" >/dev/null 2>&1 &
sleep 0.4
cansend "$IFACE" 123#R8
cansend "$IFACE" 180#1122334455667788
cansend "$IFACE" 200#01
sleep 2.2
wait 2>/dev/null
LOG=$(ls -1 candump-*.log 2>/dev/null | head -1)
echo "\$ candump -l $IFACE        # 自动生成 $(basename "${LOG:-candump-*.log}")"
echo '日志内容:'
sed 's/^/  /' "${LOG:-/dev/null}"
echo
echo '$ canplayer -I <日志>      # 按原始时间间隔重放'
timeout -s INT 2.5 candump -ta "$IFACE" > "$TMP/replay.log" 2>&1 &
sleep 0.4
timeout 2 canplayer -I "${LOG:-/dev/null}"
sleep 0.8
wait 2>/dev/null
echo '回放时总线上出现:'
sed 's/^/  /' "$TMP/replay.log"
echo
echo '  日志格式就是 candump 的紧凑写法：<时间戳> <接口> <ID>#<数据>'
echo '  真实项目里这是最常用的取证手段：现场录一段，拿回实验室 canplayer 复现。'

# ---------------------------------------------------------------- E0.5
sec "E0.5  接口计数器：一帧发出去，哪些计数会动"
cnt() { cat "/sys/class/net/$IFACE/statistics/$1"; }
echo '统计在 sysfs 里，比 ip 命令更好脚本化。当前值:'
echo '$ cat /sys/class/net/'"$IFACE"'/statistics/{rx_packets,tx_packets,rx_bytes,tx_bytes}'
for f in rx_packets tx_packets rx_bytes tx_bytes; do
	printf '  %-12s %s\n' "$f" "$(cnt "$f")"
done

RX0=$(cnt rx_packets); TX0=$(cnt tx_packets)
echo
echo '$ cansend '"$IFACE"' 180#AABBCCDD     # 只发这一帧'
cansend "$IFACE" 180#AABBCCDD
sleep 0.2
printf '  rx_packets +%s   tx_packets +%s\n' \
	"$(( $(cnt rx_packets) - RX0 ))" "$(( $(cnt tx_packets) - TX0 ))"
echo
echo '  发一帧同时 +1/+1：驱动把帧 clone 一份送回协议栈，这就是回环。'
echo '  但累计值可能差几帧 —— 因为本实验台前面演示过关掉 LOOPBACK 的发送，'
echo '  那种帧只增加 tx 不增加 rx（阶段 1 的 [E] 小节）。'
echo
echo '  真实硬件还多两个关键计数：rx_errors / tx_errors（这里恒为 0）——'
echo '  vcan 没有物理层，永远不会出错，也就永远看不到错误帧与 Bus-Off。'

# ---------------------------------------------------------------- E0.6
sec "E0.6  vcan 没有位定时：这就是它和真硬件的根本差别"
echo '$ ip link set '"$IFACE"' type can bitrate 500000'
ip link set "$IFACE" type can bitrate 500000 2>&1
echo "  rc=$?  <- 不支持。位定时是 CAN 控制器的硬件参数，vcan 没有控制器。"
echo
echo '真实控制器上这一步是这样的（以 500kbps、80MHz 时钟、采样点 87.5% 为例）:'
echo '$ can-calc-bit-timing -b 500000 -s 875 -c 80000000 sja1000'
can-calc-bit-timing -b 500000 -s 875 -c 80000000 sja1000 2>&1 | sed 's/^/  /'
echo
echo '  读法：'
echo '    TQ[ns]  一个时间份额 = 1/80MHz × BRP(10) = 125ns'
echo '    每 bit = 1 个同步段 + PrS(6) + PhS1(7) + PhS2(2) = 16 TQ = 2us -> 500kbps'
echo '    采样点 = (1+6+7)/16 = 87.5%，落在 PhS1 与 PhS2 的边界上'
echo '    SJW=1：允许重同步时最多调整 1 个 TQ（时钟误差容忍度）'
echo '    BTR0/BTR1 是 SJA1000 的两个寄存器值，真机上要写进硬件'
echo
echo '  这些数在 vcan 上毫无意义 —— 虚拟总线不按位传输，帧是整块内存拷贝。'
echo '  所以：vcan 可以验证协议、调度、过滤，但永远验证不了位定时与总线仲裁。'

# ---------------------------------------------------------------- E0.7
sec "E0.7  负载估算：一帧占多少时间"
cat <<'EOF'
  标准帧 8 字节的位数（CAN 2.0A，不含位填充）：
    SOF 1 + 仲裁场(11+1) + 控制场(1+1+4) + 数据 64
        + CRC(15+1) + ACK(1+1) + EOF 7 + IFS 3  =  111 bit
  500kbps 下 = 111 / 500000 ≈ 222 us
  位填充最坏情况再增加约 20 bit（每 5 个同值位插 1 位），约 262 us
  于是 10ms 周期发一帧，总线负载约 222us/10ms ≈ 2.2%
EOF
echo
echo '$ canbusload '"$IFACE"'@500000    # 边发边测（下面临时发 150 帧）'
(for i in $(seq 1 150); do cansend "$IFACE" 180#1122334455667788; sleep 0.01; done) &
SENDER=$!
sleep 0.6
LOADLINE=$(timeout -s INT 2 stdbuf -oL canbusload "$IFACE@500000" 2>&1 | grep -v '^$' | tail -1)
printf '  %s\n' "$LOADLINE"
wait "$SENDER" 2>/dev/null
FPS=$(echo "$LOADLINE" | awk '{print $2}')
BPS=$(echo "$LOADLINE" | awk '{print $3}')
PBS=$(echo "$LOADLINE" | awk '{print $4}')
if [ "${FPS:-0}" -gt 0 ]; then
	printf '  核对：%s 帧/秒 × %s bit/帧 = %s bit/秒；%s 帧/秒 × 64 = %s 载荷 bit/秒\n' \
		"$FPS" "$((BPS / FPS))" "$BPS" "$FPS" "$PBS"
fi
echo '  列含义：<接口@位速率> <帧/秒> <bit/秒> <载荷bit/秒> <错误> <负载%>'
echo '  bit/帧 是 111 加位填充后的实际值 —— 数据里连续相同的位越多，填充越多。'
echo '  注意它按你"声明"的 bitrate 估算，vcan 本身没有速率概念。'

# ---------------------------------------------------------------- E0.8
sec "E0.8  链路断开时会发生什么（阶段 4 要处理的情况）"
echo '$ ip link set '"$IFACE"' down'
ip link set "$IFACE" down
echo '$ cansend '"$IFACE"' 123#AA'
cansend "$IFACE" 123#AA 2>&1
echo "  rc=$?"
echo '$ candump '"$IFACE"
timeout 1 candump "$IFACE" 2>&1 | head -2
ip link set "$IFACE" up
echo
echo '$ ip link set '"$IFACE"' up   # 已恢复'
ip link show "$IFACE" | head -1
echo
echo '  真实总线上还有一种更隐蔽的故障：控制器进入 Bus-Off（TEC > 255），'
echo '  接口看起来还是 UP，但发不出任何帧，只能靠读错误帧发现。'
echo '  vcan 没有错误计数器，造不出 Bus-Off —— 这是它最大的盲区。'

# ---------------------------------------------------------------- 收尾
sec "阶段 0 完成"
cat <<'EOF'
  三条结论：
    1. 工具的语法层（cansend 的 # / ## / R）与显示层（[8] / [02] / remote request）
       是把同一帧做了不同投影，看懂映射关系就能读懂任何一份 CAN 日志。
    2. 位定时、仲裁、错误帧全部属于"控制器硬件"，vcan 一概没有；
       协议设计、过滤、调度、日志这些"软件层"能力，vcan 完全等价于真硬件。
    3. 111 bit / 222us / 2.2% 这类手算，是判断"这条总线还撑不撑得住"的基本功，
       比任何工具都可靠。
EOF
echo
echo "临时文件已清理: $TMP"
