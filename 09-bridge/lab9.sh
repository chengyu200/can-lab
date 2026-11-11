#!/bin/bash
# 09-bridge/lab9.sh —— 两台「机器」+ 一条 CAN 线，完整跑一遍
#
#   [clientA]──┐                        ┌──[clientC]
#   [clientB]──┼─TCP→ M1 server ⇄ cb0 ~~ cb1 ⇄ M2 server ←TCP─┘
#
# 两个 server 是同一台实验机上的两个进程、两个端口，各自绑 vxcan 的一端。
# 真实部署时把它们分到两台机器，cb0/cb1 换成 can0 即可，代码一行不改。
set -u

CD=$(cd "$(dirname "$0")" && pwd)
IF_A=${IF_A:-cb0}
IF_B=${IF_B:-cb1}
PORT_A=${PORT_A:-7701}
PORT_B=${PORT_B:-7702}
LOG=$CD/demo.log
CANLOG=$CD/can.log
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

: >"$LOG"
: >"$CANLOG"

# 抓包留证：两端都挂上，只看 0x500。
# 桥接关掉了 LOOPBACK，本机发出的帧在本机抓不到 —— 一个帧只会出现在「对端」
# 那个接口上，所以从接口名就能看出方向（cb1 上的帧都是 M1 发的）。
candump -td "$IF_A",500:7FF "$IF_B",500:7FF >"$CANLOG" 2>&1 &
PIDS+=($!)

"${SRV_BIN:-$CD/tcp_server}" "$PORT_A" 1 "$IF_A" >>"$LOG" 2>&1 &
SRV+=($!)
PIDS+=($!)
"${SRV_BIN:-$CD/tcp_server}" "$PORT_B" 2 "$IF_B" >>"$LOG" 2>&1 &
SRV+=($!)
PIDS+=($!)
sleep 0.8

mark "起 client：A、B 连 M1，C 连 M2（各自的发送时刻写在自己的输入流里）"
( sleep 0.5; echo "hello-from-A"; sleep 3.2; echo "while-link-down"; \
  sleep 1.5; echo "after-recover-A" ) \
    | "$CD/client" 127.0.0.1 "$PORT_A" A --hold 2.5 >>"$LOG" 2>&1 &
PIDS+=($!)
( sleep 2.6; echo "hi" ) | "$CD/client" 127.0.0.1 "$PORT_A" B --hold 4 >>"$LOG" 2>&1 &
PIDS+=($!)
( sleep 1.7; echo "pong-from-C"; sleep 3.5; echo "after-recover" ) \
    | "$CD/client" 127.0.0.1 "$PORT_B" C --hold 2.5 >>"$LOG" 2>&1 &
PIDS+=($!)

mark "步骤 1：A 发 hello-from-A（12 字符 + kind = 13 字节 → 5 帧）"
sleep 3.4

mark "步骤 2/3：C 跨机回 pong-from-C，B 在本地发 hi（单帧快速路径）"
mark "步骤 4：把 cb1 拉 down（模拟对端机器掉电），A 紧接着发 while-link-down"
ip link set "$IF_B" down
sleep 1.4

mark "步骤 5：cb1 恢复 up，A 与 C 各发一条 —— 验证断链后双向自愈"
ip link set "$IF_B" up
sleep 0.2
# candump 遇到 ENETDOWN 会自己退出，重新挂一个继续抓恢复之后的帧
candump -td "$IF_A",500:7FF "$IF_B",500:7FF >>"$CANLOG" 2>&1 &
PIDS+=($!)
sleep 2.8

mark "收尾：SIGTERM 两个 server，打印退出统计"
for p in "${SRV[@]}"; do kill "$p" 2>/dev/null; done
sleep 0.8

echo
echo "===== CAN 线上抓到的帧（candump，时间列是帧间隔） ====="
cat "$CANLOG"
echo
echo "===== 应用日志（按时间排序） ====="
sort "$LOG"
echo
echo "完整日志：$LOG    抓包：$CANLOG"
