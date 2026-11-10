#!/bin/bash
# 09-bridge/t_bidir.sh —— 双向同时发 300 字节，专门验「两个 socket」那个设计
#
# 为什么需要这个测试：阶段 5 的 isotp_chan 收发共用一个 socket，isotp_send()
# 在 wait_fc() 里会把对端发来的数据帧当垃圾丢掉，所以严格来说它是半双工的。
# 桥接用「两个 socket + 同一个过滤器，靠 CAN_RAW 多播各拿一份副本」绕过去。
# 这个绕过是否成立，只有双向同时发才能验出来 —— 单向测试永远碰不到这个坑。
#
# 判据：双方都收到完整的 307 字节，且 错序=0 超时=0。
set -u

CD=$(cd "$(dirname "$0")" && pwd)
IF_A=${IF_A:-bd0}
IF_B=${IF_B:-bd1}
LOG=$(mktemp /tmp/bidir.XXXXXX.log)

cleanup() {
    trap - EXIT INT TERM HUP
    kill ${S1:-} ${S2:-} 2>/dev/null
    sleep 0.5
    ip link del "$IF_A" 2>/dev/null
    rm -f "$LOG"
}
trap cleanup EXIT INT TERM HUP

ip link del "$IF_A" 2>/dev/null
ip link add "$IF_A" type vxcan peer name "$IF_B" || { echo "建 vxcan 失败"; exit 1; }
ip link set "$IF_A" mtu 16 up
ip link set "$IF_B" mtu 16 up

B1=$(printf 'A%.0s' $(seq 1 300))
B2=$(printf 'C%.0s' $(seq 1 300))

"$CD/tcp_server" 7721 1 "$IF_A" --isotp --peers 2 >"$LOG" 2>&1 & S1=$!
"$CD/tcp_server" 7722 2 "$IF_B" --isotp --peers 1 >>"$LOG" 2>&1 & S2=$!
sleep 0.8

# 两条消息只差 20ms：第一台的 CF 还在飞，第二台就开始发 FF，真正撞在一起
( sleep 1.0; echo "$B1"; sleep 2.0 ) | "$CD/client" 127.0.0.1 7721 A --hold 2 >>"$LOG" 2>&1 &
( sleep 1.02; echo "$B2"; sleep 2.0 ) | "$CD/client" 127.0.0.1 7722 C --hold 2 >>"$LOG" 2>&1 &
sleep 4

kill "$S1" "$S2" 2>/dev/null
sleep 1

echo "===== 结果 ====="
grep -E "收到|发送|统计" "$LOG" | sed 's/AAAA*/<300×A>/; s/CCCC*/<300×C>/' | sort
echo
if grep -q "错序0" "$LOG" && [ "$(grep -c '共 307 字节' "$LOG")" -ge 2 ]; then
    echo "PASS：双向各 1 条 307 字节消息都完整送达，错序 0 超时 0"
else
    echo "FAIL：看上面的日志，重点找 错序 / 超时 是否非 0"
fi
