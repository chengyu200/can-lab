#!/bin/bash
# topo.sh —— 建立/销毁两段 vxcan 总线
#
#   vxcan0 ~~ vxcan1      A 段：动力总成（私有 CAN）
#   vxcan2 ~~ vxcan3      B 段：J1939 语义
#   网关进程同时接在 vxcan1 和 vxcan2 上
#
# vxcan 是点对点的：一端发，只有对端能收，本端看不到。
# 所以两段总线需要两对 vxcan，中间由网关进程桥接 —— 这正是真实网关的角色。
#
# 用法： ./topo.sh up | down | status | reset
cd /root/can-lab || exit 1

PAIR_A="vxcan0 vxcan1"
PAIR_B="vxcan2 vxcan3"

do_up() {
	for base in vxcan0 vxcan2; do
		peer="vxcan$(( ${base#vxcan} + 1 ))"
		if ip link show "$base" >/dev/null 2>&1; then
			continue
		fi
		ip link add "$base" type vxcan peer name "$peer" || return 1
	done
	for d in $PAIR_A $PAIR_B; do
		ip link set "$d" up || return 1
	done
	echo "拓扑已建立："
	ip -br link show type vxcan
}

do_down() {
	# 删一端，对端会一起消失
	for d in vxcan0 vxcan2; do
		ip link del "$d" 2>/dev/null
	done
	echo "拓扑已销毁"
}

do_status() {
	ip -br link show type vxcan || echo "（没有任何 vxcan 接口）"
	echo
	echo "各段统计："
	for d in $PAIR_A $PAIR_B; do
		ip link show "$d" >/dev/null 2>&1 || continue
		printf '  %-8s %s  rx_packets=%s tx_packets=%s\n' "$d" \
			"$(ip -br link show "$d" | awk '{print $2}')" \
			"$(cat /sys/class/net/$d/statistics/rx_packets)" \
			"$(cat /sys/class/net/$d/statistics/tx_packets)"
	done
}

case "$1" in
up)     do_up ;;
down)   do_down ;;
status) do_status ;;
reset)  do_down >/dev/null; sleep 0.3; do_up >/dev/null; echo "拓扑已重建" ;;
*)
	echo "用法: $0 up | down | status | reset"
	exit 1
	;;
esac
