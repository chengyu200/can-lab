# CAN 应用编程实验台（SocketCAN on ARM）

在 openEuler 24.03 / aarch64（kernel 6.6）上，用 `vcan` 虚拟总线学习基于 CAN 的应用编程。
所有代码只依赖 SocketCAN 与 libc，接到真实 CAN 硬件时**不需要改任何一行**——
只需把 `ip link` 那一行换成真实接口即可。

## 环境前提

```sh
ip link show vcan0          # 应当已存在且 UP
# 若不存在：
modprobe vcan
ip link add dev vcan0 type vcan
ip link set vcan0 up
```

阶段 5、6 还需要三个模块和一对虚拟网线（本机已装到 `extra/`）：

```sh
modprobe can-isotp can-j1939 vxcan

# vxcan 的两个端点是同一段总线的两端，用来模拟真实双端拓扑
ip link add vxcan0 type vxcan peer name vxcan1
ip link set vxcan0 up && ip link set vxcan1 up
```

阶段 8 需要**两对** vxcan（两段总线，中间由网关桥接），用脚本建即可，
它会顺手把上面这对也补齐：

```sh
./08-gateway/topo.sh up      # vxcan0~~vxcan1（A 段）+ vxcan2~~vxcan3（B 段）
./08-gateway/topo.sh status
./08-gateway/topo.sh down
```

`vcan0` 的 MTU 是 72 而不是 16，说明这是支持 CAN FD 的 vcan。

### 内核模块总表（本机实测，kernel 6.6.0-145）

模块加载不用手写 `modprobe`：内核按 `alias` 自动匹配。`socket(PF_CAN, SOCK_RAW, CAN_RAW)`
会拉 `can_raw`，`ip link add type vcan` 会拉 `vcan`。只有手工放进 `extra/` 的
`can-isotp` / `can-j1939` / `vxcan` 需要显式 `modprobe`（`depmod -a` 之后同样能自动加载）。

| 模块 | alias（自动加载依据） | 作用 | 本实验用在哪 |
|---|---|---|---|
| `can` | `net-pf-29` | PF_CAN 协议族核心：socket 注册、设备列表、`can_send` / `can_rx_register`、向所有协议广播 netdev up/down | 全部阶段（`lsmod` 里 refcount=4） |
| `can_raw` | `can-proto-1` | `SOCK_RAW` 实现：`CAN_RAW_FILTER` 内核侧过滤、`LOOPBACK`/`RECV_OWN_MSGS` 回环开关、`ERR_FILTER`、`FD_FRAMES` | 阶段 1–4、8 |
| `can_bcm` | `can-proto-2` | Broadcast Manager：`TX_SETUP` 用内核定时器周期发送，`RX_SETUP`+`RX_TIMEOUT` 在内核侧判超时 | 阶段 4 |
| `can_isotp` | `can-proto-6` | ISO 15765-2 传输层：分段/重组、FC/BS/STmin、N_BS/N_CR 超时 | 阶段 5、6（与手写栈对照） |
| `can_j1939` | `can-proto-7` | SAE J1939：29 位 ID 解码、地址声明与 NAME 仲裁、TP（BAM 与 RTS/CTS） | 阶段 7、8 |
| `vcan` | `rtnl-link-vcan` | 纯软件回环网卡，支持 FD（MTU 72），无时序保证 | 阶段 0–6 的 `vcan0` |
| `vxcan` | `rtnl-link-vxcan` | 一对接口当一段双绞线两端：跨端转发 + 本端回环，可单独 down 一端 | 阶段 8（两对） |
| `can-dev` | `rtnl-link-can` | CAN 设备公共层：bittiming 计算、bus-off restart、echo skb | 间接，被 vcan/vxcan 依赖 |
| `can-gw` | `can-gw` | `cangw` 的内核 netlink 网关：跨接口转发、改 ID、对 payload 做 and/or/xor | **未用**（原因见下） |
| `c_can` / `slcan` | — | 真实控制器 / 串口转 CAN 驱动 | 未用，接真硬件时替换 vcan |

三条边界，比记住模块名更重要：

- **该下沉内核模块的**：与业务语义无关、要求不依赖进程存活、要求确定时序 —— 周期发送
  （BCM）、超时监控（BCM `RX_TIMEOUT`）、分段重组（ISO-TP）、地址仲裁（J1939）。
  进程被 `SIGSTOP` 冻住或直接崩掉，这些事照样进行（见阶段 4）。
- **必须留用户态的**：信号解码、状态机、量纲换算、跨总线语义映射。阶段 8 的量纲陷阱
  证明了字节级转发必然出错，`can-gw` 同样只能搬字节。
- **`can-gw` 为什么没用**：`cangw` 能改 ID、对 payload 做位运算，但改不了 `factor`。
  0x180 的 ×0.25 和 EEC1 的 ×0.125 之间要除以 2，只能用户态算。纯透传/改 ID 的轻量桥接
  用它会更省（零拷贝、不需要常驻进程），需要换算就得上 `gw.c`。

放到容器里跑的话：CAN 也是 netdev，`ip link set can0 netns <pid>` 可以直接移入容器
netns；但**模块加载只在 init netns 里发生一次**，容器内的 `socket()` 不会因为缺模块而去
自动加载。所以正确做法是宿主先 `modprobe` 好协议模块，容器只给 `CAP_NET_RAW` 并移入
netns —— 别指望在容器里把 `can_isotp` 拉起来。

## 构建

```sh
make            # 全部编译（-Wall -Wextra 零警告）
make clean
```

## 目录

```
include/can_proto.h      ID 表 + 信号下标 + 位操作（协议头）
proto.c                  报文/信号定义表（唯一真相源，等价 DBC）
00-basics/lab0.sh        阶段 0：纯命令行观察
00-basics/NOTES.md       阶段 0：帧结构、DLC、位定时、错误计数
01-hello/can_hello.c     阶段 1：最小收发
02-router/can_router.c   阶段 2：内核过滤 + poll + 分发
02-router/gen_traffic.sh 阶段 2：模拟流量
03-app/ecu_engine.c      阶段 3：发动机节点（发送方）
03-app/ecu_dash.c        阶段 3：仪表节点（接收 + 超时 + 闭环）
03-app/can_decode.c      阶段 3：表驱动通用解码器
03-app/NOTES.md          阶段 3：协议设计、调度、丢帧与卡顿
04-pro/bcm_periodic.c    阶段 4：BCM 周期发送 / 超时监控 / 时序测量
04-pro/link_watch.c      阶段 4：netlink 链路监控与自动恢复
04-pro/fd_demo.c         阶段 4：CAN FD 64 字节与 DLC 编码
04-pro/NOTES.md          阶段 4：BCM 的边界与这台机器的定时精度
05-isotp/isotp_user.c    阶段 5：手写 ISO-TP（分片/流控/超时/序号）
05-isotp/isotp_kernel.c  阶段 5：内核 CAN_ISOTP 对照
05-isotp/isotp_fault.c   阶段 5：坏接收端（故障注入）
05-isotp/isotp_test.c    阶段 5：边界长度验证
05-isotp/lab5.sh         阶段 5：只用工具观察四种 PDU
05-isotp/NOTES.md        阶段 5：协议细节、七个故障场景、与内核对照
06-uds/uds.c             阶段 6：UDS 服务（纯逻辑，不碰 socket）
06-uds/ecu_server.c      阶段 6：诊断 ECU（可切内核栈/手写栈）
06-uds/uds_tester.c      阶段 6：诊断仪（15 步完整序列）
06-uds/NOTES.md          阶段 6：会话/安全/NRC/P2/S3
include/j1939_util.h     阶段 7：29 位 ID 与 64 位 NAME 的拆解
07-j1939/j1939_addr.c    阶段 7：地址声明与 NAME 仲裁
07-j1939/j1939_node.c    阶段 7：PGN 收发 + BAM/RTS-CTS 分片
07-j1939/lab7.sh         阶段 7：四个实验一次跑完
07-j1939/NOTES.md        阶段 7：PDU1/PDU2、仲裁、两套传输协议对比
08-gateway/topo.sh       阶段 8：建立/销毁两段 vxcan 总线
08-gateway/gw.c          阶段 8：双段网关（路由 + 协议转换 + 断链处理）
08-gateway/lab8.sh       阶段 8：全流程演示
08-gateway/NOTES.md      阶段 8：量纲陷阱、两种断链、网关记账
```

## 阶段 0：只用工具看总线（00-basics/lab0.sh）

不写一行代码，先把「一帧在线上长什么样、占多少时间、哪些东西是 vcan 给不了的」搞清楚。

```sh
./00-basics/lab0.sh vcan0
```

八个实验，全程只读（唯一例外是 E0.8 会 down/up 一次 vcan0，退出时自动恢复）：

| 编号 | 内容 | 关键结论 |
|---|---|---|
| E0.1 | 接口与模块 | `mtu=72` 不是以太网 MTU，是 vcan 能承载的最大 FD 帧长 |
| E0.2 | 七种帧形态 | `#` / `##` / `#R` 是语法层，`[8]` / `[02]` / `remote request` 是显示层 |
| E0.3 | candump 过滤 | `iface,180:7FF` 与 C 里的 `can_filter` 同构 |
| E0.4 | 日志与回放 | `candump -l` 落盘，`canplayer -I` 原样重放（连 RTR 的 DLC 都不丢） |
| E0.5 | 接口计数器 | 发一帧 `rx/tx` 同时 +1，差值来自关过 LOOPBACK 的帧 |
| E0.6 | 位定时 | vcan 设 bitrate 报 `Operation not supported`；真硬件要算 TQ/BRG/采样点 |
| E0.7 | 负载估算 | 8 字节标准帧 111 bit ≈ 222 µs，10 ms 周期 ≈ 2.2% |
| E0.8 | 链路断开 | `Network is down`；真硬件的 Bus-Off 更隐蔽，接口 UP 但发不出去 |

配套原理笔记在 `00-basics/NOTES.md`。

## 阶段 1：最小收发（01-hello/can_hello）

```sh
./01-hello/can_hello vcan0
```

| 小节 | 内容 | 结论 |
|---|---|---|
| A | 标准帧 0x123，`loopback=on` + `recv_own=on` | 自己发的帧自己收得到 |
| B | 扩展帧 0x18FF50E5，带 `CAN_EFF_FLAG` | 标志位和 ID 共用一个 32 位字段 |
| C | `can_dlc=0` | 经典 CAN 允许零长度帧（心跳/唤醒常用） |
| D | `recv_own=off` | 帧上了总线，但自己收不到 |
| E | `loopback=off` | vcan 上帧被直接丢弃，谁都收不到 |

**第一个坑**：`CAN_RAW_LOOPBACK` 和 `CAN_RAW_RECV_OWN_MSGS` 是两个独立开关。

```
CAN_RAW_LOOPBACK       默认 1   帧要不要送回本接口（别人能不能看见）
CAN_RAW_RECV_OWN_MSGS  默认 0   发送者自己要不要收自己发的帧
```

默认组合是「别人看得见，自己看不见」。单进程想跑通闭环，必须显式打开 `RECV_OWN_MSGS`。

## 阶段 2：过滤器 + poll + 分发（02-router/can_router）

分两个终端。终端 A 造流量：

```sh
./02-router/gen_traffic.sh vcan0
#   0x180 10ms / 0x181 100ms / 0x200 500ms / 0x2FF 500ms（干扰帧）
```

终端 B 跑分发器：

```sh
./02-router/can_router                # 每秒一行汇总
./02-router/can_router -v             # 逐帧打印
./02-router/can_router 0x180          # 只订阅 0x180
./02-router/can_router 0x2FF          # 订阅那个干扰帧，验证它确实存在
```

观察点：

1. 默认订阅三个 ID 时 `unknown` 恒为 0 —— 说明 0x2FF 是在**内核**被丢掉的，
   根本没进用户态。这是过滤器最实际的价值：省 CPU，也避免队列打满。
2. 只把 `0x2FF` 传进去，它立刻出现 —— 证明干扰帧一直在总线上，是过滤器挡住了它。
3. `gen_traffic.sh` 每 10ms 起一个 `cansend` 进程，进程创建开销把周期拉长到 70/s，
   这是阶段 3 改成进程内定时、阶段 4 交给内核的原因。

## 阶段 3：双节点 + 协议层

### 3.1 协议定义：把 DBC 写成一个 C 表

`include/can_proto.h` 声明结构，`proto.c` 里只有一张表：

```c
static const struct can_signal ef_signals[] = {
    { "rpm",      0, 16, 0.25,  0.0, "rpm" },   /* bit 0..15  0.25 rpm/bit */
    { "throttle",16,  8, 0.4,   0.0, "%"   },
    { "load",    24,  8, 1.0,   0.0, "%"   },
    { "alive",   32,  8, 1.0,   0.0, ""    },   /* 滚动计数器 */
};
```

`start/len/factor/offset` 的语义与 DBC 的 `SG_ ... @1+ (factor,offset)` 完全一致：

```
物理值 = 原始值 × factor + offset
```

**为什么值得这么做**：同一份布局要被三个东西共用 —— 发送端编码、接收端解码、
离线分析工具。写死三份必然漂移；做成表之后，发送端和接收端在结构上就不可能对不上。

实测编码（`candump` 抓到的原始字节）：

```
180   [8]  89 0E 2D 15 00 00 00 00 ...
            └──┬──┘ │  │  └─ alive=0
            0x0E89 │  └─ load=21%
             =3721 │
            ×0.25  └─ throttle=45×0.4=18%
            =930.25 rpm
```

保留字节全是 0 —— 不是洁癖，接收端常把它们留作版本号或校验。

### 3.2 两个节点

终端 A：

```sh
./03-app/ecu_engine vcan0
```

- 内部跑一个小发动机模型（目标转速按 24 秒正弦扫频 800→4200 rpm，水温有热容，油压跟转速）
- 10ms 发 0x180，100ms 发 0x181
- 收 0x200 命令，改自己的目标转速

终端 B：

```sh
./03-app/ecu_dash vcan0
```

- 解 0x180/0x181，画转速条
- 超时监控：100ms 没收到 0x180 就判节点失效，显示冻结
- 闭环：rpm>3000 持续 1.5s → 发 0x200 请求降扭 25%；rpm<2600 持续 3s → 解除

终端 C（可选）：表驱动的通用解码器，相当于人肉版 candump：

```sh
./03-app/can_decode            # 打印协议定义（DBC 的样子）
./03-app/can_decode vcan0      # 逐帧解码成物理值
```

### 3.3 实测：闭环自己跑起来了

```
[t=  7s] rpm=  3029  |================        | 水温  36.0C  油压  300kPa  ... 丢 0  卡 0
[t=  8s] rpm=  3397  |==================      | 水温  38.0C

  [TX] rpm>3000 持续 1500ms  -> 请求降扭 25%

[t=  9s] rpm=  2824  |===============         | 水温  40.0C
        （engine 侧同时打印： [RX] 收到降扭请求 arg=25%  -> 目标转速乘 0.75）
...
[t= 18s] rpm=  1841  |=========               | 水温  49.0C

  [TX] rpm<2600 持续 3000ms  -> 解除降扭

[t= 19s] rpm=  1986  |==========              | 水温  49.0C
```

杀掉发动机进程后：

```
  [!!] 0x180 超时 105 ms —— 发动机节点失效/断线
  [!!] 0x181 超时 —— 慢变量已冻结（显示的是旧值）
[t= 27s] rpm=------  |                        | 水温  45.0C  【信号中断】
```

三个值得记住的点：

1. **周期发送必须用绝对时间**。`clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next)`
   睡的是「某个时刻」，`sleep(10ms)` 睡的是「一段时间」。后者把循环处理耗时累加进周期，
   跑一分钟就漂出几百毫秒；前者被延迟后仍对齐原有时基，**误差不累积**。
2. **沉默必须当成故障**。节点掉线时总线不报错，只是安静下来。
   `poll` 一定要带超时，否则纯阻塞的 `recv` 会一直等，永远发现不了掉线。
3. **迟滞（hysteresis）**。3000 触发、2600 解除，中间 400 rpm 的死区避免在阈值附近抖动发命令。

### 3.4 丢帧与卡顿是两件事

第一版仪表靠到达间隔估丢帧，报了「丢 9 帧」，但发送端明明发了 2600 帧、接收端也收了 2600 帧。
原因是**间隔变大既可能是丢帧，也可能是延迟到达**，用户态无法区分。

改成在协议里放一个滚动计数器（`alive`，每帧 +1，255 回绕）后：

```c
unsigned delta = (seq - last_seq) & 0xFF;
if (delta == 0)        { /* 重复帧 */ }
else if (delta < 128)  { lost += delta - 1; }   /* 精确丢帧 */
/* delta >= 128：计数器回退，多半是节点重启 */
```

于是两个指标分开了：

- `丢 0` —— 计数器连续，发送端确实一帧没少
- `卡 3` —— 有 3 次间隔超过 2.5 个周期（25ms），是**在路上耽搁**了，不是丢了

这就是 AUTOSAR E2E 保护里 counter 字段的最小版本，也是真实报文里几乎总能找到
一个「每帧递增字节」的原因。

## 阶段 4：交给内核、扛住断链、跑 FD

### 4.1 BCM：把周期发送和超时监控搬进内核

```sh
./04-pro/bcm_periodic tx  vcan0 10      # 内核每 10ms 发 0x181
./04-pro/bcm_periodic rx  vcan0 300     # 0x180 超时 300ms 就报警
./04-pro/bcm_periodic mon vcan0 10 0x181 # 测量到达间隔（对照组）
```

`tx` 的关键就是把一个 `TX_SETUP` 写下去，之后进程什么都不用做：

```c
mh->opcode = TX_SETUP;
mh->flags  = SETTIMER | STARTTIMER;
mh->ival2  = 10ms;
mh->can_id = 0x181;
mh->nframes = 1;
write(bcm_fd, mh, offsetof(struct bcm_msg_head, frames) + sizeof(struct can_frame));
```

**证明帧不是进程发的**：把进程 `SIGSTOP` 冻住，总线照发。

```
进程已 SIGSTOP，状态: T
--- 进程被冻结期间，总线上的 0x181（2 秒）---
172
```

**BCM 的边界**：内容一旦交给内核就改不了。所以它适合「内容固定、周期严格」的报文
（心跳、固定状态广播），不适合转速这种每帧都在变的量 —— 这正好解释了阶段 3 为什么
必须用用户态发。

`rx` 模式更有意思：超时判断在内核里做，用户态只是收通知。

```
[ 0.309s] !! 0x180 超时 300 ms，内核判定链路静默
[ 0.519s] 0x180 恢复
[ 3.815s] !! 0x180 超时 300 ms，内核判定链路静默    ← 杀掉 engine 后 300ms
```

### 4.2 链路监控：netlink + 按名字重新查 ifindex

```sh
./04-pro/link_watch vcan0
# 另一个终端：ip link set vcan0 down / up
# 更狠的：ip link del vcan0; ip link add dev vcan0 type vcan; ip link set vcan0 up
```

实测日志（22 → 23 是重点）：

```
[   0.0s] 套接字就绪  vcan0 ifindex=22 fd=4
[   2.0s] <netlink> vcan0 DOWN  ifindex=22
[   4.5s] <netlink> vcan0 UP  ifindex=22
[   4.5s] 套接字就绪  vcan0 ifindex=22 fd=4
[   7.5s] <netlink> vcan0 DOWN  ifindex=22
[   7.6s] <netlink> vcan0 被删除
[   9.6s] <netlink> vcan0 DOWN  ifindex=23
[   9.6s] <netlink> vcan0 UP  ifindex=23
[   9.6s] 套接字就绪  vcan0 ifindex=23 fd=4
```

**最容易踩的一脚**：删掉重建后设备名还叫 `vcan0`，但 ifindex 从 22 变成 23。
如果缓存了旧 ifindex 去 `bind`，会得到 `EINVAL`，现象是「明明设备在，就是绑不上」。
所以恢复时必须重新 `if_nametoindex()`。USB 适配器重插、容器里 veth 重建都是这个坑。

### 4.3 CAN FD：8 → 64 字节

```sh
./04-pro/fd_demo vcan0
```

```
CAN_MTU=16  CANFD_MTU=72
DLC :   0   1   2   3   4   5   6   7   8   9  10  11  12  13  14  15
字节:   0   1   2   3   4   5   6   7   8  12  16  20  24  32  48  64

[B] 发 64 字节 FD 帧（带 BRS）
    读到 72 字节  id=0x180 len=64 flags=0x05 [BRS] [FDF]

[C] 同样是 8 字节，FD 帧和经典帧差在哪
    收到 CAN FD 帧   len=8 [FDF]
    收到 经典帧      dlc=8

[D] 未开 CAN_RAW_FD_FRAMES 的套接字，写 72 字节
    write 返回 -1，errno=22 (Invalid argument)
```

三个要点：

- **DLC 9..15 是台阶**，9 表示 12 字节而不是 9；给 13 字节会被就近向上取整到 16
- 开 FD 后 **recv 缓冲区必须给 72 字节**，只给 16 会被静默截断
- 8 字节的 FD 帧和经典帧只能靠 `CANFD_FDF` 标志位（0x04）区分

### 4.4 这台机器的定时精度（重要，别被数字骗了）

`dmesg` 里写着 `kvm [1]: HYP mode not available` —— 这台虚拟机**没有硬件虚拟化**，
是 QEMU 纯软件模拟（TCG）。所有定时器和进程唤醒都被模拟器拖慢到毫秒级。

用三种方式测同一个「10ms 周期」（间隔取自内核时间戳，不是用户态时钟）：

| 发送方式 | 平均周期 | 最小 | 最大 | 标准差 |
|---|---|---|---|---|
| 用户态 `clock_nanosleep`（绝对时间） | **10.003 ms** | 3.01 | 16.17 | 1.18 ms |
| 内核 BCM `TX_SETUP` | **11.819 ms** | 10.04 | 20.37 | 1.17 ms |
| 纯定时器探针（不碰 CAN） | — | — | — | 平均晚 1.6 ms |

结论要分两层看：

- **抖动**（标准差 ~1.2ms）几乎全部来自这台模拟机，两种方式一样差。纯定时器探针
  不碰 CAN 也抖 1.6ms，说明跟 CAN 栈无关。
- **漂移**不同：用户态用绝对时间基准，被延迟后会在下一拍追回来，所以平均周期是准的
  （代价是间隔忽长忽短，最小 3ms）；BCM 由内核定时器驱动，不会追，每个周期固定多出
  1.8~2.8ms。

所以在**这台机器上**「BCM 比用户态更准」并不成立，是模拟器的锅。真机上二者分别是
几十微秒（用户态，含调度延迟）和几微秒（内核 hrtimer）的量级，这里测不出来。
能确定的是 BCM 的**独立性**：进程被冻住，帧照发。

## 协议约定

11-bit 标准帧，按用途分段（ID 越小仲裁优先级越高）：

| ID | 名称 | 方向 | 周期 | DLC | 信号 |
|---|---|---|---|---|---|
| 0x180 | ENGINE_FAST | engine → bus | 10 ms | 8 | rpm / throttle / load / alive |
| 0x181 | ENGINE_SLOW | engine → bus | 100 ms | 8 | coolant / oil_press / flags |
| 0x200 | DASH_CMD | dash → bus | 事件 | 2 | cmd / arg |
| 0x300 | DIAG_REQ | tester → ecu | 事件 | — | 预留 |
| 0x308 | DIAG_RESP | ecu → tester | 事件 | — | 预留 |

位序全部小端（Intel），与 DBC 的 `@1+` 一致。完整定义跑 `./03-app/can_decode` 打印。

`flags` 位域：`0x01` MIL 故障灯、`0x02` 过温、`0x04` 油压过低。

## 阶段 5：ISO-TP 传输层（05-isotp/）

前四个阶段都在直接摆弄 8 字节帧。但诊断请求、标定数据、刷写镜像都远超 8 字节，
中间需要一层分片协议 —— 车上用的是 ISO 15765-2。

这一阶段的做法是**先自己写一遍，再用内核版对照**：

| 程序 | 作用 |
|---|---|
| `isotp_user.c` | 约 400 行手写实现：分片、等 FC、守 STmin、校验序号 |
| `isotp_kernel.c` | 同样的事交给 `CAN_ISOTP`，调用侧约 20 行 |
| `isotp_fault.c` | 可控的坏接收端：不回 FC / 回 OVFLW / 只回 WAIT / 故意错序 |
| `isotp_test.c` | 7 档边界长度（1 / 7 / 8 / 20 / 100 / 1000 / 4000） |
| `lab5.sh` | 纯 can-utils 观察四种 PDU，一行代码不写 |

四种 PDU 的第一个字节都是 PCI，高 4 位定类型：

```
单帧 SF   0x0L  [data 1..7]          L = 长度，最大 7
首帧 FF   0x1H  0xLL  [6 字节]       长度 = (H << 8) | LL，> 7
连续帧 CF 0x2N  [7 字节]             N = 序号 1..15 循环
流控帧 FC 0x3S  0xBS  0xSTmin        S = 0 继续 / 1 等待 / 2 溢出
```

故障注入的实测结果（`bash 05-isotp/t_stage5.sh`）：

| 场景 | 发送端行为 | 耗时 |
|---|---|---|
| 正常对端 | 成功 | 0.3 ms |
| 不回 FC | `ETIMEDOUT`，发完 1 帧就停 | 1001 ms |
| 回 OVFLW | 立即中止 | 0.2 ms |
| 先回 3 次 WAIT | 继续等，最终成功 | 0.3 ms |
| BS=2、100 字节 | 每 2 帧停一次，共 7 次 FC | 0.6 ms |
| STmin=100 ms | 遵守 100 ms 帧间隔 | 100.6 ms |
| **CF 序号错乱** | **发送端仍然报告成功** | 0.3 ms |

最后一行是这个阶段最重要的结论：接收端已经整条丢弃，发送端因为 `write` 全返回成功
而报告 PASS。CAN 没有传输层 ACK，**"发出去了"和"对方收到了"是两件事** ——
这正是 UDS 需要应用层响应和 P2 超时的原因。

```sh
./05-isotp/lab5.sh              # 先看字节长什么样
./05-isotp/isotp_test           # 再验正确性
bash 05-isotp/t_stage5.sh       # 再踩七条错误路径
bash 05-isotp/t_kernel.sh       # 最后与内核实现互操作
```

细节见 `05-isotp/NOTES.md`，其中三个坑值得先知道：首帧不一定是 `10` 开头（>255 字节
时是 `11`）、序号 15 之后回 1 不回 0、BS 是双向约定（接收端忘了续发 FC 会把对端卡死）。

## 阶段 6：UDS 诊断（06-uds/）

UDS 跑在 ISO-TP 上，规定"请求什么、回什么、什么条件下允许、出错怎么报"。
它不关心分片，也不关心跑在 CAN 还是以太网上。

`uds.c` 是纯函数：进一个字节数组，出一个字节数组，一行 socket 代码都没有。
`ecu_server.c` 负责传输层，**换内核栈和手写栈只改三个函数**，上层完全不知道。
这层解耦是这个阶段最值钱的部分 —— 阶段 5 手写的 ISO-TP 现在能承载真实协议。

| SID | 服务 | 本实现 |
|---|---|---|
| `0x10` | 会话控制 | 默认 / 编程 / 扩展 |
| `0x11` | ECU 复位 | 硬复位 |
| `0x19` | 读 DTC | 按状态掩码读 |
| `0x22` | 读数据 | VIN / 零件号 / 序列号 / 实时转速 |
| `0x27` | 安全访问 | 种子-密钥，错 3 次锁定 |
| `0x2E` | 写数据 | VIN（需扩展会话 + 已解锁） |
| `0x3E` | 保持会话 | 支持 suppress 位 |

15 步自动序列全部通过，响应时间 0～1 ms（P2 上限 50 ms）。最有价值的是第 14、15 步：

```
14. 复位后写 VIN  -> 7F 2E 22   （会话已掉回默认，正确拒绝）
15. 复位后读 VIN  -> 62 F1 90 + VIN  （数据还在）
```

**会话是易失的，数据是持久的**，实现里必须把这两件事分开。

```sh
./06-uds/ecu_server -i vxcan1            # 终端 A：ECU，内核 ISO-TP
./06-uds/ecu_server -i vxcan1 --user     # 或换成手写 ISO-TP
./06-uds/uds_tester -i vxcan0 --auto     # 终端 B：完整序列
./06-uds/uds_tester -i vxcan0 --s3       # 验证 S3 会话超时
./06-uds/uds_tester -i vxcan0            # 交互模式，手敲 "10 03"
```

细节见 `06-uds/NOTES.md`：NRC 分类、检查顺序（长度→会话→安全→参数）、
P2/P2\*/S3 三个超时的区别，以及和量产实现的六项差距。

## 阶段 7：J1939（07-j1939/）

商用车协议。和前面所有阶段最大的区别：**29 位 ID 是一个结构体，不是一个数值。**

```
   28      26 25 24 23      16 15       8 7        0
   +---------+--+--+----------+-----------+----------+
   | Priority|R |DP|    PF    |    PS     |    SA    |
   +---------+--+--+----------+-----------+----------+
       3bit  1  1     8           8           8
```

PF 落在哪一段决定了整个报文的行为：
`PF < 0xF0` 是 PDU1（PS 是目标地址），`PF >= 0xF0` 是 PDU2（PS 属于 PGN）。

```sh
./07-j1939/lab7.sh        # 四个实验一次跑完，约 25 秒
```

### 地址声明与仲裁

每个 ECU 上电先广播 PGN `0xEE00` + 8 字节 NAME 来占地址：

```
 (1794064143.784611)  vxcan0  18EEFF80   [8]  00 10 00 00 00 00 00 00
```

两个节点抢同一地址时 **NAME 小的赢**。输的一方不是"发了被忽略"，
而是内核直接摘掉它的地址映射，之后 `sendto` 返回 `EADDRNOTAVAIL`：

```
节点 A（NAME=0x2000）：-> 本机输
[TX  2] 发送 EEC1 失败: Cannot assign requested address (errno=99)
节点 B（NAME=0x1000）：-> 本机赢
```

但**接收不受影响** —— 丢掉地址只是不能说话，耳朵还在。

### BAM 与 RTS/CTS

超过 8 字节要分片，用哪种方式由**目标地址**自动决定，不需要额外参数：

```
广播 -> 18ECFF80  20 14 00 03 FF F1 FE 00      BAM，数据帧间隔 58~60 ms
单播 -> 18EC9080  10 14 00 03 03 F1 FE 00      RTS
        18EC8090  11 03 01 FF FF F1 FE 00      CTS，一次授权 3 包
        18EB9080  01 / 02 / 03 ...             DT，间隔 0.0 ms
        18EC8090  13 14 00 03 FF F1 FE 00      EOMA
```

**BAM 是按固定间隔一个包一个包地喊，RTS/CTS 是授权多少就连发多少** ——
0 ms 与 58 ms 的对比就是这句话的证据。
（规范写 50 ms，实测 58~60 ms，原因同阶段 4：没有 KVM 的软件模拟机器定时器只到毫秒级。）

细节见 `07-j1939/NOTES.md`，包括两处"内核不替你做"的事：
`bind()` 不广播地址声明、内核**不应答**"请求地址声明"。

## 阶段 8：双段网关（08-gateway/）

把前面所有东西装进一个拓扑 —— 这是真实车上的形态：多条总线，中间靠网关桥接。

```
[engine]---vxcan0 ~~ vxcan1---[gw]---vxcan2 ~~ vxcan3---[J1939 接收端]
            A 段 私有 CAN               B 段 J1939
```

```sh
./08-gateway/topo.sh up
./08-gateway/lab8.sh      # 全流程，约 20 秒
```

网关做三种动作：原样转发、换 ID 转发、**协议转换**。第三种才是真正的网关工作：

```
0x180  A4 0E 2E 15 ...  ->  18F00400  00 8F 92 48 1D ...
       rpm 原始 0x0EA4 = 3748 × 0.25  = 937.0 rpm
       EEC1 原始 0x1D48 = 7496 × 0.125 = 937.0 rpm     原始值差一倍，物理值相等
```

直接把 8 个字节搬过去，B 段会解出 1874 rpm —— 有数、但不正确，最难查的那类 bug。

断链要区分两种：接口被**删**（ifindex 失效，必须重新 bind，见阶段 4）
和接口 **down**（`ENETDOWN`，socket 仍然有效，继续收等着恢复，本阶段）。
车上绝大多数是后者：ECU 掉电、线束断开、收发器休眠。

实测 4 秒断链的账：

```
规则 0x180  进 1462  出 1060      <- 丢 402
规则 0x181  进 146   出 106       <- 丢 40
```

442 帧，正好是 4 秒里 10 ms + 100 ms 两个周期的产量。
**网关是单点，断了就是永久丢数据**，所以真实网关必须回答"断了之后怎么办"。

细节见 `08-gateway/NOTES.md`：为什么这里故意不用内核过滤器、
为什么 TP 报文不能盲目透传到另一段。

## 踩坑记录（都是这台机器上实测踩出来的）

### 1. 自己发的帧自己收不到

`CAN_RAW_LOOPBACK` 和 `CAN_RAW_RECV_OWN_MSGS` 是两个独立开关，
默认值是 `1 / 0`，即「别人看得见，自己看不见」。单进程做收发闭环时
只设 LOOPBACK 是不够的，必须 `RECV_OWN_MSGS=1`。

### 2. 0x999 在总线上变成了 0x199

11 位标准帧的 ID 上限是 0x7FF。`cansend vcan0 999#DEADBEEF` 返回码 0，
但 candump 看到的是 `199`。内核把 bit11 及以上静默截掉了，发送端毫无察觉。

### 3. vcan 上关掉 LOOPBACK 等于把帧扔掉

真实 CAN 控制器关掉 LOOPBACK 只是不往自己这边回，总线上其他节点照样收得到；
vcan 的「总线」本身就是回环，关掉之后帧被直接丢弃，candump 也看不到。

### 4. `struct bcm_msg_head` 不能用 {head; frame;} 直接包装

它的末尾是柔性数组 `frames[]`，而 `ival1/ival2` 里是 `long`。
64 位平台上 `count` 后面会插 4 字节填充，`sizeof(bcm_msg_head)` 正好等于
`offsetof(frames)`（都是 56）；32 位平台上这两个数都不等于 56。
自己写个 `struct { struct bcm_msg_head h; struct can_frame f; }`，
编译器会按自己的对齐规则放 `f`，而内核读 `frames[]` 用 `offsetof`——
一旦不一致，现象是「BCM 建好了但发出来全是 0」。
正确做法：按 `offsetof + sizeof(can_frame)` 分配。见 `bcm_alloc()`。

### 5. `printf` 到文件是块缓冲，程序被 kill 时日志是空的

`./bcm_periodic rx > log &` 之后 `kill -KILL`，log 里连表头都没有。
所有诊断工具启动时加 `setvbuf(stdout, NULL, _IOLBF, 0)`。

### 6. ssh 会话断开后后台脚本变野进程

`gen_traffic.sh` 只 trap 了 `EXIT/INT/TERM`，漏了 `HUP`。ssh 一断，
`while` 循环被 init 收编，每 10ms 继续发帧，后面几十分钟的总线都被污染了。
已补 `HUP` 并加 `pkill -x cansend` 兜底。

### 7. 首帧不一定以 `10` 开头

300 字节的实测首帧是 `11 2C`。长度 300 = 0x12C，超过 255，高 4 位要占用 PCI 的低 4 位。
写死 `if (data[0] == 0x10)` 的代码在小于 256 字节时全对，超过就失效 —— 又是典型的
"小包正常、大包必挂"。

### 8. `CAN_ISOTP_RECV_FC` 必须在 `bind()` 之前设置

顺序反了返回 `EISCONN`（Transport endpoint is already connected）。因为 `bind` 之后
ISO-TP 连接就已经建立，配置窗口关闭了。这个顺序在文档里不显眼，第一次写必然踩到。

### 9. BS 是双向约定，接收端忘了续发就把对端卡死

BS=2 表示"连发 2 帧就停下来等我"。接收端在一个 block 收满后**必须再发一个 FC**。
我在写 `isotp_fault.c` 时漏了这一步，现象是：小于 BS 帧的消息全正常，大一点直接卡到
`N_Bs` 超时。补上之后 100 字节正好 7 次 FC。

### 10. 只打印不发送，编译器帮着抓到了

`ecu_server.c` 第一版在响应分支里只 `printf` 了响应内容，忘了调 `ep_send`。
`-Wall` 的 `-Wunused-function`（`ep_send defined but not used`）暴露了它。
如果这个函数被别处引用过一次，编译器就不会报，而现象会是"ECU 日志一切正常，
诊断仪永远超时"。

### 11. `isotpsend` 的 stdin 必须用空格分隔十六进制

`printf "DEADBEEF" | isotpsend` 只会发出 1 字节 `EF`，而返回码是 0。
`echo "DE AD BE EF"` 才能发出 4 字节。can-utils 的 hex 解析按空格切 token，
连写的字符串会被当成一个非法 token 丢掉大部分内容。

### 12. J1939 的 `bind()` 不会广播地址声明，内核也不应答"请求地址声明"

`bind()` 只是在内核里登记"我这个 NAME 想用这个地址"，**声明帧要应用层自己发**
（PGN `0xEE00` + 8 字节 NAME）。而规范要求"收到 PGN `0xEA00` 请求时必须重播声明"
这件事，内核同样没有实现。后果很隐蔽：**新上线的节点会以为总线上只有自己**。
`j1939_addr.c` 里用 4 行补上了这个义务。

### 13. 输掉地址仲裁后，连业务报文都发不出去

不是"发了没人理"，而是 `sendto` 直接返回 `EADDRNOTAVAIL`。原因是内核发送前
用 NAME 反查地址（`j1939_ac_fixup`），NAME 没有有效单播映射就丢包。
反直觉的地方在于：**发送被禁了，接收完全正常**。

### 14. J1939 的 `sendto` 立刻返回，不代表已经发出去

发送 20 字节时报"耗时 0 ms"，是因为 `sendto` 只是把消息塞进内核的 TP 会话，
真正的分片和 50 ms 定时器都在后台跑。要知道"发完没有"，得开 `SO_J1939_ERRQUEUE`
配合 `SO_TIMESTAMPING` 读错误队列里的 TX_ACK / TX_ABORT。
这和阶段 5 那个"发送端报成功、接收端已丢弃"是同一个根源：
**CAN 没有传输层 ACK，"交出去"和"送到了"永远是两件事。**

### 15. NAME 在总线上是小端，用 hex 打印会读反

`0x1000` 在 candump 里是 `00 10 00 00 00 00 00 00`。直接 `%016llx` 打出来是
`0000000000001000`，两者看着毫不相干，对照抓包时会怀疑人生。
`j1939_util.h` 里专门提供了 `j1939_name_wire_str()` 按总线顺序打印。
