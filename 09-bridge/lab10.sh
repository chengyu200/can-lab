#!/bin/bash
# 09-bridge/lab10.sh —— 同一套 tcp_server，换成 ISO-TP 分片再跑一遍
#
#   [clientA]──┐                        ┌──[clientC]
#   [clientB]──┼─TCP→ M1 ⇄ cb0 ~~ cb1 ⇄ M2 ←TCP─┘
#
# 和 lab9.sh 唯一的区别是 server 后面多了 `--isotp --peers`，以及多了一条
# 300 字节的大消息 —— 自研后端的上限是 255，这条在 lab9 里会被直接拒绝。
set -u

CD=$(cd "$(dirname "$0")" && pwd)
IF_A=${IF_A:-cb0}
IF_B=${IF_B:-cb1}
PORT_A=${PORT_A:-7711}
PORT_B=${PORT_B:-7712}
LOG=$CD/demo_isotp.log
CANLOG=$CD/can_isotp.log
PIDS=()
SRV=()

cleanup() {
    trap - EXIT INT TERM HUP
    for p in "${PIDS[@]:-}"; do kill "$p" 2>/dev/null; done
    sleep 0.3
    pkill -x candump 2>/dev/null
    ip link del "$IF_A" 2>/dev/null
    echo
    echo "拓扑已清理（$IF_A / $IF_B）"
}
trap cleanup EXIT INT TERM HUP

mark() { printf '%s ----- %s -----\n' "$(date +%H:%M:%S.%3N)" "$*" >>"$LOG"; }

if ! ip link add "$IF_A" type vxcan peer name "$IF_B" 2>/dev/null; then
    ip link del "$IF_A" 2>/dev/null
    ip link add "$IF_A" type vxcan peer name "$IF_B" || { echo "建 vxcan 失败，检查 vxcan 模块"; exit 1; }
fi
ip link set "$IF_A" mtu 16 up 2>/dev/null
ip link set "$IF_B" mtu 16 up 2>/dev/null
echo "拓扑: $IF_A <~~> $IF_B"
echo "端口: M1=$PORT_A  M2=$PORT_B"
echo "后端: ISO-TP（阶段 5 用户态实现），M1→M2=0x612  M2→M1=0x621"

: >"$LOG"
: >"$CANLOG"

# 抓 0x600~0x6FF 整段：ISO-TP 的 FF/CF/FC 都在里面，能直接看出分帧过程
candump -td "$IF_A",600:700 "$IF_B",600:700 >"$CANLOG" 2>&1 &
PIDS+=($!)

"${SRV_BIN:-$CD/tcp_server}" "$PORT_A" 1 "$IF_A" --isotp --peers 2 >>"$LOG" 2>&1 &
SRV+=($!)
PIDS+=($!)
"${SRV_BIN:-$CD/tcp_server}" "$PORT_B" 2 "$IF_B" --isotp --peers 1 >>"$LOG" 2>&1 &
SRV+=($!)
PIDS+=($!)
sleep 0.8

BIG=$(printf 'B%.0s' $(seq 1 300))     # 300 字节，自研后端发不出去

mark "起 client：A、B 连 M1，C 连 M2"
# A 的发送时刻（相对启动）：0.5 hello、2.9 大消息、6.2 断链期间、8.6 恢复后
( sleep 0.5; echo "hello-from-A"; sleep 2.4; echo "$BIG"; sleep 3.3; echo "while-link-down"; \
  sleep 2.4; echo "after-recover-A" ) \
    | "$CD/client" 127.0.0.1 "$PORT_A" A --hold 3 >>"$LOG" 2>&1 &
PIDS+=($!)
( sleep 2.2; echo "hi" ) | "$CD/client" 127.0.0.1 "$PORT_A" B --hold 8 >>"$LOG" 2>&1 &
PIDS+=($!)
( sleep 1.6; echo "pong-from-C" ) | "$CD/client" 127.0.0.1 "$PORT_B" C --hold 8 >>"$LOG" 2>&1 &
PIDS+=($!)

mark "步骤 1/2：hello-from-A（15 字节 → FF+2×CF）与 300 字节大消息（303 字节 → FF+43×CF）"
sleep 3.5

mark "步骤 3：cb1 拉 down（模拟对端掉电），0.7 秒后 A 发 while-link-down —— 这条注定丢"
ip link set "$IF_B" down
sleep 2.7

mark "步骤 4：cb1 恢复 up，A 再发 after-recover-A —— 验证断链后自愈"
ip link set "$IF_B" up
sleep 0.2
candump -td "$IF_A",600:700 "$IF_B",600:700 >>"$CANLOG" 2>&1 &
PIDS+=($!)
sleep 3.0

mark "收尾：SIGTERM 两个 server，打印退出统计"
for p in "${SRV[@]}"; do kill "$p" 2>/dev/null; done
sleep 1.2

echo
echo "===== CAN 线上抓到的帧（前 30 行，看 FF / FC / CF 的配合） ====="
head -30 "$CANLOG"
echo "..."
echo "总帧数：$(grep -c 'cb[01]' "$CANLOG")"
echo
echo "===== 应用日志（按时间排序） ====="
sort "$LOG"
echo
echo "完整日志：$LOG    抓包：$CANLOG"
