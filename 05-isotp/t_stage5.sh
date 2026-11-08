#!/bin/bash
# t_stage5.sh —— 阶段 5 故障注入实测（在 ARM 机器上跑）
#
# 每一轮：先起一个「坏接收端」，再跑用户态发送端，观察发送端的反应。
# 重点不是"能不能通"，而是"出错时多久发现、以什么方式失败"。
#
# 长度按场景挑：
#   20 字节 = 2 个 CF，只够验证基本流程
#   100 字节 = 14 个 CF，才测得出 BS 分组和序号错乱
cd /root/can-lab || exit 1

B=./05-isotp
IF_TX=vxcan0
IF_RX=vxcan1

run() {
	local name=$1
	local len=$2
	shift 2

	echo "########## $name ##########"
	pkill -x isotp_fault 2>/dev/null
	sleep 0.3
	rm -f /tmp/f.log

	( timeout 9 $B/isotp_fault -i "$IF_RX" "$@" -t 6000 > /tmp/f.log 2>&1 & )
	sleep 0.6

	$B/isotp_test -i "$IF_TX" -p "$IF_RX" -l "$len" --no-peer
	sleep 0.4

	echo "--- 对端（坏接收端）日志 ---"
	sed 's/^/    /' /tmp/f.log
	echo
}

run "A. 正常对端（对照）"                 20  --mode normal
run "B. 不回 FC —— 期望 N_Bs 超时"        20  --mode nofc
run "C. 回 OVFLW —— 期望发送端中止"       20  --mode ovflw
run "D. 3 次 WAIT 后再继续"               20  --mode wait
run "E. BS=2，100 字节 —— 每 2 帧停一次"  100 --mode normal --bs 2
run "F. STmin=100ms —— 发送节奏被拉长"    20  --mode normal --stmin 100
run "G. 100 字节，第 2 个 CF 后错序"      100 --mode drop --drop-at 2

pkill -x isotp_fault 2>/dev/null
echo "=== 清理完成 ==="
