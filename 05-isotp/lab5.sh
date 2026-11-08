#!/bin/bash
# lab5.sh —— 只用 can-utils 观察 ISO-TP 的四种 PDU
#
# 这个脚本一行 C 代码都没有。目的是先把「字节长什么样」看清楚：
# 后面读 isotp_user.c 时，你会认出每个分支对应这里的哪一帧。
#
# 用法： ./lab5.sh [发送接口] [接收接口]
#        默认 vxcan0 / vxcan1，也可以都用 vcan0（同一接口能互收）
cd /root/can-lab || exit 1

IF=${1:-vxcan0}
PEER=${2:-vxcan1}
S=300		# 发送方地址（帧的 CAN ID）
D=308		# 接收方地址

cleanup() {
	pkill -x isotprecv 2>/dev/null
	pkill -x isotpsend 2>/dev/null
	pkill -x candump 2>/dev/null
	pkill -x isotpdump 2>/dev/null
}
trap cleanup EXIT INT TERM

sec() {
	echo
	echo "============================================================"
	echo "$*"
	echo "============================================================"
}

cleanup
sleep 0.3

# ---------------------------------------------------------------- E5.1
sec "E5.1  单帧 SF：4 字节"
echo "最短的情况。PCI = 0x0L，L 就是长度，数据紧跟其后。"
echo
rm -f /tmp/l5_i1.log /tmp/l5_c1.log
( timeout 4 candump -ta -e "$IF" > /tmp/l5_c1.log 2>&1 & )
( timeout 4 isotprecv -s $D -d $S "$PEER" > /tmp/l5_i1.log 2>&1 & )
sleep 0.6
echo '$ echo "DE AD BE EF" | isotpsend -s 300 -d 308'" $IF"
echo "DE AD BE EF" | isotpsend -s $S -d $D "$IF"
sleep 1.2
echo "--- 对端解出来的应用数据 ---"
cat /tmp/l5_i1.log
echo "--- 总线原始帧 ---"
cat /tmp/l5_c1.log
echo "--- 读法 ---"
echo "  01 EF 是单帧、1 字节；04 DE AD BE EF 才是 4 字节。"
echo "  如果你只看到 1 字节，是 isotpsend 的输入格式问题："
echo "  hex 必须用空格分隔，\"DEADBEEF\" 会被当成一个非法 token。"
cleanup

# ---------------------------------------------------------------- E5.2
sec "E5.2  多帧：20 字节（FF + FC + CF）"
echo "超过 7 字节就必须分片。注意首帧之后先回一个流控帧，才继续发。"
echo
rm -f /tmp/l5_i2.log /tmp/l5_c2.log
( timeout 5 candump -ta -e "$IF" > /tmp/l5_c2.log 2>&1 & )
( timeout 5 isotprecv -s $D -d $S "$PEER" > /tmp/l5_i2.log 2>&1 & )
sleep 0.6
echo '$ echo "11 22 ... 44" | isotpsend -s 300 -d 308'" $IF"	"（20 字节）"
echo "11 22 33 44 55 66 77 88 99 00 AA BB CC DD EE FF 11 22 33 44" | \
	isotpsend -s $S -d $D "$IF"
sleep 1.5
echo "--- 对端解出来的应用数据 ---"
cat /tmp/l5_i2.log
echo "--- 总线原始帧 ---"
cat /tmp/l5_c2.log
echo "--- 读法 ---"
echo "  10 14     首帧：长度 = (0x0 << 8) | 0x14 = 20，带前 6 字节"
echo "  30 00 00  流控：(0x3 是 FC) 0=继续  BS=0（一次发完） STmin=0"
echo "  21 ...    连续帧：序号 1，7 字节"
echo "  22 ...    连续帧：序号 2，7 字节"
echo "  6 + 7 + 7 = 20，正好"
cleanup

# ---------------------------------------------------------------- E5.3
sec "E5.3  大包 300 字节：序号回绕"
echo "序号 1..15 循环，15 之后回到 1（不是 0）。这一条错一次，"
echo "现象会是「小包正常，超过 100 字节就挂」。"
echo
rm -f /tmp/l5_i3.log /tmp/l5_c3.log
sleep 1		# 让上一轮的残留帧走完，否则会被这一轮的 candump 抓到
( timeout 6 candump -ta -e "$IF" > /tmp/l5_c3.log 2>&1 & )
( timeout 6 isotprecv -s $D -d $S "$PEER" > /tmp/l5_i3.log 2>&1 & )
sleep 0.6
echo '$ isotpsend -s 300 -d 308'" $IF"' -D 300'  "（-D 直接发指定长度，不读 stdin）"
isotpsend -s $S -d $D "$IF" -D 300
sleep 2
echo "--- 对端收到的数据量（hex 文本，每字节占 3 个字符）---"
wc -c < /tmp/l5_i3.log
echo "--- 总线共几帧 ---"
grep -c " $S " /tmp/l5_c3.log
echo "--- 序号序列（每帧 PCI 的低 4 位，去掉首帧）---"
grep " $S " /tmp/l5_c3.log | awk '{print $5}' | grep -v "^1" | cut -c2 | tr -d '\n'
echo
echo "--- 读法 ---"
echo "  理论帧数：1 个首帧 + 42 个连续帧 = 43（300 减 6，再按每帧 7 字节算）"
echo "  序号应为 1..15,1..15,1..15,1..13 —— 最后一段不足 15 是正常的"
echo
echo "  注意上面首帧是 11 2C 而不是 10 xx："
echo "  300 = 0x12C，超过 255，长度的高 4 位就放在首帧第一字节的低 4 位里。"
echo "  所以「首帧一定以 10 开头」是错的，写死这个假设的代码在大于 255"
echo "  字节时必然出错，而小于 255 时一切正常 —— 这种 bug 最难查。"
cleanup

# ---------------------------------------------------------------- E5.4
sec "E5.4  接收端限流 BS=2：每 2 帧停一次"
echo "BS 是「块大小」。接收端说 BS=2，发送端每发 2 个连续帧就要"
echo "停下来等下一个流控帧。这是接收端控制节奏的手段。"
echo
rm -f /tmp/l5_i4.log /tmp/l5_c4.log
( timeout 8 candump -ta -e "$IF" > /tmp/l5_c4.log 2>&1 & )
( timeout 8 isotprecv -s $D -d $S -b 2 "$PEER" > /tmp/l5_i4.log 2>&1 & )
sleep 0.6
echo '$ isotprecv -b 2 ... &  然后  isotpsend -D 100'
isotpsend -s $S -d $D "$IF" -D 100
sleep 2.5
echo "--- 流控帧出现次数 ---"
grep -c " 30 " /tmp/l5_c4.log
echo "--- 总线帧序（前 20 行）---"
head -20 /tmp/l5_c4.log
echo "--- 读法 ---"
echo "  100 字节 = 1 FF + 14 CF。BS=2 时，第一个 FC 之后每 2 个 CF"
echo "  就有一次 FC。数一下 30 开头的帧，应该是 7 次左右。"
cleanup

# ---------------------------------------------------------------- E5.5
sec "E5.5  没有接收端：发送端等 FC 超时"
echo "总线上没人回 FC，isotpsend 会等 N_Bs（标准建议 1000ms）后放弃。"
echo
echo '$ time (echo "..." | isotpsend -s 300 -d 308'" $IF"')'
START=$(date +%s%N)
echo "11 22 33 44 55 66 77 88 99 00 AA BB CC DD EE FF 11 22 33 44" | \
	timeout 5 isotpsend -s $S -d $D "$IF"
RC=$?
END=$(date +%s%N)
echo "退出码 $RC  耗时 $(( (END - START) / 1000000 )) ms"
echo "--- 读法 ---"
echo "  isotpsend 只在首帧后等 FC，所以大概 1 秒就返回。"
echo "  这就是为什么 UDS 还需要自己的 P2 超时：传输层的超时只保证"
echo "  「这一帧送出去了」，不保证「对方真的处理了」。"
cleanup

# ---------------------------------------------------------------- E5.6
sec "E5.6  isotpdump：自带解码的观察器"
echo "不想自己数 PCI 的话，isotpdump 会把 SF/FF/CF 翻译成人话。"
echo
rm -f /tmp/l5_d6.log
( timeout 5 isotpdump -s $D -d $S "$IF" > /tmp/l5_d6.log 2>&1 & )
sleep 0.6
isotpsend -s $S -d $D "$IF" -D 40
sleep 2
cat /tmp/l5_d6.log
cleanup

echo
echo "============================================================"
echo "观察完毕。接着看 05-isotp/isotp_user.c，每个分支都对得上。"
echo "============================================================"
