#!/bin/bash
# t_kernel.sh —— 用户态实现与内核 CAN_ISOTP 的互操作验证
#
# 四个方向都跑一遍。如果两个实现在字节层面真的符合 ISO 15765-2，
# 它们就应该能互操作 —— 这比各自跑通自己更有说服力。
cd /root/can-lab || exit 1

B=./05-isotp

echo "########## 1. 用户态发 -> 内核收（100 字节）##########"
pkill -x isotp_kernel 2>/dev/null; pkill -x isotp_fault 2>/dev/null
sleep 0.3
( timeout 6 $B/isotp_kernel recv -i vxcan1 -s 0x700 -d 0x701 -t 3000 > /tmp/k1.log 2>&1 & )
sleep 0.7
$B/isotp_test -i vxcan0 -p vxcan1 -l 100 --no-peer
sleep 0.5
sed 's/^/    /' /tmp/k1.log
echo

echo "########## 2. 内核发 -> 用户态收（100 字节）##########"
( timeout 6 $B/isotp_fault -i vxcan1 --mode normal -t 3000 > /tmp/k2.log 2>&1 & )
sleep 0.7
$B/isotp_kernel send -i vxcan0 -s 0x701 -d 0x700 -l 100
sleep 0.5
sed 's/^/    /' /tmp/k2.log
echo

echo "########## 3. 内核发 -> 内核收（4000 字节）##########"
pkill -x isotp_kernel 2>/dev/null; pkill -x isotp_fault 2>/dev/null
sleep 0.3
( timeout 8 $B/isotp_kernel recv -i vxcan1 -s 0x700 -d 0x701 -t 5000 > /tmp/k3.log 2>&1 & )
sleep 0.7
$B/isotp_kernel send -i vxcan0 -s 0x701 -d 0x700 -l 4000
sleep 0.5
sed 's/^/    /' /tmp/k3.log
echo

echo "########## 4. 内核收端限流 BS=2 STmin=50，看用户态发端是否遵守 ##########"
pkill -x isotp_kernel 2>/dev/null
sleep 0.3
( timeout 10 $B/isotp_kernel recv -i vxcan1 -s 0x700 -d 0x701 --bs 2 --stmin 50 -t 8000 > /tmp/k4.log 2>&1 & )
sleep 0.7
$B/isotp_test -i vxcan0 -p vxcan1 -l 100 --no-peer
sleep 0.5
sed 's/^/    /' /tmp/k4.log
echo

echo "=== 清理 ==="
pkill -x isotp_kernel 2>/dev/null
pkill -x isotp_fault 2>/dev/null
