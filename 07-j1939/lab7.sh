#!/bin/bash
# lab7.sh —— J1939 的四个基本事实，全用真实抓包说话
#
# 这个脚本按「先看见、再解释」的顺序走：
#    E7.1 地址声明到底长什么样
#    E7.2 两个节点抢同一个地址，谁赢、输的会怎样
#    E7.3 PDU1 与 PDU2：同样的发送代码，拼出完全不同的 29 位 ID
#    E7.4 BAM 与 RTS/CTS：同一份数据，两种分片节奏
#
# 用法： ./lab7.sh [本端接口] [对端接口]     默认 vxcan0 / vxcan1
cd /root/can-lab || exit 1

IF=${1:-vxcan0}
PEER=${2:-vxcan1}

cleanup() {
	pkill -x j1939_node 2>/dev/null
	pkill -x j1939_addr 2>/dev/null
	pkill -x candump 2>/dev/null
}
trap cleanup EXIT INT TERM

sec() {
	echo
	echo "============================================================"
	echo "$*"
	echo "============================================================"
}

# 打印 candump 日志里相邻同一条 TP 会话数据帧的间隔（毫秒）
interval() {
	awk '{ gsub(/[()]/, "", $1); t = $1 * 1000;
	       if (prev) printf "      间隔 %.1f ms\n", t - prev;
	       prev = t }' "$1"
}

cleanup
sleep 0.4

# ---------------------------------------------------------------- E7.1
sec "E7.1  地址声明：J1939 的「我是谁」"
cat <<'TXT'
经典 CAN 没有源地址。J1939 把 29 位 ID 的最低 8 位固定为 SA（源地址），
于是每个 ECU 必须先占一个地址。占地址的方式就是广播一帧「地址声明」：

    PGN = 0xEE00    数据 = 8 字节 NAME（64 位，出厂唯一）

下面这一帧的 ID 可以直接算：
    优先级 6 | PF=0xEE（PDU1）| PS=0xFF（广播）| SA=0x80
    -> 0x18EEFF80
TXT
echo
rm -f /tmp/l7_c1.log /tmp/l7_a1.log
( timeout 4 candump -ta -e "$IF" > /tmp/l7_c1.log 2>&1 & )
sleep 0.4
echo "\$ ./07-j1939/j1939_addr $PEER 0x80 0x1000 --claim-period 800"
timeout -s INT 2.2 ./07-j1939/j1939_addr "$PEER" 0x80 0x1000 --claim-period 800 \
	2>&1 | grep -E "NAME=|声明帧 ID|已广播" | head -4
sleep 0.8
echo
echo "--- 总线上抓到的原始帧（前 3 帧）---"
head -3 /tmp/l7_c1.log
echo
echo "读法：18EEFF80 里，E 是 PF=0xEE，FF 是 PS（广播），80 是 SA。"
echo "      数据 00 10 00 ... 是 NAME 0x1000 的**小端**字节序，别按 hex 反过来读。"

# ---------------------------------------------------------------- E7.2
sec "E7.2  地址仲裁：NAME 小的赢，输的一方连业务报文都发不出去"
cat <<'TXT'
两个 ECU 都想要 0x80 时，比较 64 位 NAME，数值小的赢。
输的一方不是「继续发但被忽略」，而是**内核直接把它的源地址摘掉**，
之后它的 sendto 会返回 EADDRNOTAVAIL —— 这个现象很难从文档看出来。
TXT
echo
rm -f /tmp/l7_n1.log /tmp/l7_n2.log
# A 先上线，NAME 较大
timeout -s INT 5 ./07-j1939/j1939_addr "$IF"   0x80 0x2000 --claim-period 900 \
	> /tmp/l7_n1.log 2>&1 &
PA=$!
sleep 1.1
# B 后上线，NAME 较小
timeout -s INT 4 ./07-j1939/j1939_addr "$PEER" 0x80 0x1000 --claim-period 900 \
	> /tmp/l7_n2.log 2>&1 &
PB=$!
wait $PA $PB 2>/dev/null
echo "节点 A（$IF，NAME=0x2000）："
grep -E "争用 0x80|发送 EEC1 失败" /tmp/l7_n1.log | sed -n '1p;2p'
echo
echo "节点 B（$PEER，NAME=0x1000）："
grep -E "争用 0x80" /tmp/l7_n2.log | sed -n '1p'
echo
echo "注意 A 仍在**收** B 的帧 —— 丢掉地址只是不能发，不影响接收："
grep "^\[RX\]" /tmp/l7_n1.log | head -2

# ---------------------------------------------------------------- E7.3
sec "E7.3  PDU1 与 PDU2：同一行发送代码，两种 ID"
cat <<'TXT'
PF < 0xF0  -> PDU1，PS 是目标地址，PGN 不含 PS
PF >= 0xF0 -> PDU2，PS 是组扩展，属于 PGN

下面用同一个程序发两条，唯一差别是 --pgn 和 --dest：
    Request  PGN=0x0EA00  PF=0xEA  DA=0x90
    EEC1     PGN=0x0F004  PF=0xF0  广播
TXT
echo
rm -f /tmp/l7_c3.log
( timeout 5 candump -ta -e "$IF" > /tmp/l7_c3.log 2>&1 & )
sleep 0.5
echo "\$ ./07-j1939/j1939_node $PEER 0x80 0x1000 --pgn 0xEA00 --dest 0x90 --size 3 --count 1"
./07-j1939/j1939_node "$PEER" 0x80 0x1000 --pgn 0xEA00 --dest 0x90 \
	--size 3 --period 0 --count 1 2>&1 | grep -E "^\[TX"
echo
echo "\$ ./07-j1939/j1939_node $PEER 0x80 0x1000 --pgn 0xF004 --dest 0xFF --size 8 --count 1"
./07-j1939/j1939_node "$PEER" 0x80 0x1000 --pgn 0xF004 --dest 0xFF \
	--size 8 --period 0 --count 1 2>&1 | grep -E "^\[TX"
sleep 0.8
echo
echo "--- 总线上的结果 ---"
grep -vE "18EEFF" /tmp/l7_c3.log | tail -4
echo
echo "读法：18EA9080 的 PS=90 是**目标地址**，PGN 仍是 0xEA00（PS 记 0）。"
echo "      18F00480 的 PS=04 是**组扩展**，和 PF=0xF0 一起构成 PGN 0xF004。"

# ---------------------------------------------------------------- E7.4
sec "E7.4  BAM 与 RTS/CTS：20 字节，两种分片节奏"
cat <<'TXT'
超过 8 字节就要分片。用哪种方式由**目标地址**决定，不需要额外参数：

    DA=0xFF（广播） -> BAM：先发一帧 TP.CM(BAM) 宣布「我要发 20 字节」，
                            然后按固定间隔（规范 50 ms）连发数据包，不等回应
    DA=单播         -> RTS/CTS：先 RTS 问「我能发 3 包，你要几包」，
                            对方回 CTS 指定窗口，然后**立刻**连发，发完 EOMA 收尾

后者的数据包之间几乎是 0 间隔 —— 这一点从下面的时间戳能直接看出来。
TXT
echo
rm -f /tmp/l7_rx.log /tmp/l7_bam.log /tmp/l7_rts.log
timeout -s INT 13 ./07-j1939/j1939_node "$IF" 0x90 0x3000 --rx-only \
	> /tmp/l7_rx.log 2>&1 &
RX=$!
sleep 1

echo "##### 1) --dest 0xFF，广播 -> BAM #####"
( timeout 4 candump -ta -e "$IF" > /tmp/l7_bam.log 2>&1 & )
sleep 0.4
./07-j1939/j1939_node "$PEER" 0x80 0x1000 --pgn 0xFEF1 --dest 0xFF \
	--size 20 --period 0 --count 1 2>&1 | grep -E "^\[TX"
sleep 1.4
grep "18ECFF80" /tmp/l7_bam.log | head -1
echo "  数据帧间隔："
grep "18EBFF80" /tmp/l7_bam.log | interval /dev/stdin
sleep 0.5

echo
echo "##### 2) --dest 0x90，单播 -> RTS/CTS #####"
( timeout 4 candump -ta -e "$IF" > /tmp/l7_rts.log 2>&1 & )
sleep 0.4
./07-j1939/j1939_node "$PEER" 0x80 0x1000 --pgn 0xFEF1 --dest 0x90 \
	--size 20 --period 0 --count 1 2>&1 | grep -E "^\[TX"
sleep 1.4
echo "  RTS / CTS / EOMA："
grep -E "18EC" /tmp/l7_rts.log | sed 's/^/  /'
echo "  数据帧间隔："
grep "18EB9080" /tmp/l7_rts.log | interval /dev/stdin

kill -INT $RX 2>/dev/null
wait $RX 2>/dev/null

echo
echo "--- 接收端看到的（内核已把分片重组成整条消息）---"
grep -E "^\[RX" /tmp/l7_rx.log

sec "小结"
cat <<'TXT'
1. 地址声明是 J1939 的地基：没有 SA，29 位 ID 里「谁发的」这一位就是空的。
2. 仲裁靠 NAME 比大小，**小的赢**；输的一方发送会被内核拒绝（EADDRNOTAVAIL），
   但接收不受影响。
3. PDU1/PDU2 由 PF 决定，这决定了 PS 是「目标地址」还是「PGN 的一部分」。
4. 分片方式由目标地址自动决定：广播 BAM（固定 50 ms 间隔、无协商），
   单播 RTS/CTS（先协商窗口、随后连发）。
5. 应用层完全不接触分片细节，recvmsg 拿到的是重组后的整条消息 ——
   和阶段 5 的 ISO-TP 一样，传输层对上层透明。
TXT
