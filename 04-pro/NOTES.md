# 阶段 4 笔记：内核替你做的事，以及它做不到的事

阶段 3 的所有周期、超时、重连都是用户态自己撑起来的。阶段 4 把能搬的搬进内核，
然后看清哪些搬不动、哪些搬了反而更差。

三个程序对应三种「内核能力」：

| 程序 | 内核能力 | 用户态剩下什么 |
|---|---|---|
| `bcm_periodic` | BCM 定时器、超时监控 | 只收通知 |
| `link_watch` | netlink 链路事件 | 管理套接字生命周期 |
| `fd_demo` | CAN FD 帧收发 | 缓冲区大小要自己对 |

---

## 1. CAN_BCM 是个什么东西

SocketCAN 不是一个协议，是**一族**协议：

```
PF_CAN / SOCK_RAW   → CAN_RAW   原始帧收发（阶段 1-3 全靠它）
PF_CAN / SOCK_DGRAM → CAN_BCM   广播管理器：定时发送、内容变化检测、超时监控
PF_CAN / SOCK_DGRAM → CAN_ISOTP ISO-TP 分片（本机内核没编）
PF_CAN / SOCK_DGRAM → CAN_J1939 商用车协议（本机没编）
```

BCM 用「opcode + 报文头」的模型，而不是一次 write 一帧：

```c
struct bcm_msg_head {
    __u32 opcode;      /* TX_SETUP / TX_DELETE / RX_SETUP / RX_TIMEOUT / RX_CHANGED ... */
    __u32 flags;       /* SETTIMER / STARTTIMER / TX_COUNTEVT / RX_FILTER_ID ... */
    __u32 count;       /* 前 count 帧用 ival1，之后用 ival2 */
    struct bcm_timeval ival1, ival2;
    canid_t can_id;
    __u32 nframes;
    struct can_frame frames[];   /* 柔性数组，见下面「坑」 */
};
```

同一个 BCM 套接字上可以挂多个任务，按 `can_id` 区分。
本实验台为了讲清楚，一个进程只用一个任务。

### TX_SETUP：一个定时器，一份固定内容

```c
opcode = TX_SETUP; flags = SETTIMER | STARTTIMER;
count = 0; ival2 = 10ms; nframes = 1; frames[0] = 完整的一帧;
```

`count/ival1/ival2` 的组合有一个很实用的用法 —— **唤醒突发**：

```
count = 3, ival1 = 10ms, ival2 = 100ms
→ 前 3 帧每 10ms 发一次（让接收端尽快同步），之后转成 100ms 常速
```

真实系统里节点刚上电时常用这招，避免接收端因为「第一帧还没到」而误判超时。

### RX_SETUP：把超时判断放进内核

```c
opcode = RX_SETUP; can_id = 0x180;
ival1 = 300ms;   /* 超过这么久没收到 → 上报 RX_TIMEOUT */
ival2 = 0;       /* 节流间隔；0 表示不节流，每帧都上报 RX_CHANGED */
```

实测（engine 跑 3 秒后被杀）：

```
[ 0.309s] !! 0x180 超时 300 ms，内核判定链路静默
[ 0.519s] 0x180 恢复
[ 3.815s] !! 0x180 超时 300 ms，内核判定链路静默    ← engine 在 3.5s 被杀
```

好处是**报警不依赖用户态**：进程被 SIGSTOP、被杀、卡在某个系统调用里，
内核照样按 300ms 的判据发出超时通知（只不过没人读而已）。
坏处是「通知」仍然要用户态去读，所以最终还是要有一个看门狗盯着这个进程 ——
真正的车上会用第二个 MCU 或者硬件看门狗。

### 任务的生命周期

BCM 任务是**绑在套接字上**的。实测：直接 `SIGKILL` 掉 tx 进程（没机会执行
`TX_DELETE`），总线上立刻安静 —— 套接字关闭时内核会连带释放任务。

所以 `TX_DELETE` 的意义不是「防止野任务」，而是：同一个进程里想临时停发
但保留套接字时，只能用它。

### 什么时候不该用 BCM

BCM 发的是**固定内容**。内容一旦下发就改不了，要改只能重新 `TX_SETUP`
（或者 `TX_SEND` 单发一次），那比用户态直接 write 还慢。

所以：

- 适合：心跳、固定状态广播、节点存活标志、内容变化很慢的报文
- 不适合：转速/力矩这类每帧都在变的量

这就是阶段 3 的 0x180 必须用户态发、而 0x181 可以交给 BCM 的原因。

## 2. 定时精度：怎么测，以及这台机器测出了什么

想比较「用户态周期」和「内核周期」，测量方法本身很容易把结论带偏：

1. **不能用用户态时钟打时间戳**。接收进程自己在 TCG 下就有 1~2ms 的调度延迟，
   测出来的间隔里混进了接收端的抖动。
   用 `candump -ta`（内核时间戳）或 `recvmsg` 的 `SO_TIMESTAMP`，时间戳在
   skb 入队时打，跟接收进程什么时候醒无关。
2. **要有对照组**。一个不碰 CAN、只做 `clock_nanosleep` 的探针，能直接告诉你
   「这台机器的定时底噪是多少」。
3. **看标准差不要只看平均**。平均周期准，可能是「忽快忽慢但互相抵消」。

实测结果（10ms 周期，间隔取自内核时间戳）：

| 发送方式 | 平均 | 最小 | 最大 | 标准差 |
|---|---|---|---|---|
| 用户态绝对时间 | 10.003 ms | 3.01 | 16.17 | 1.18 ms |
| 内核 BCM | 11.819 ms | 10.04 | 20.37 | 1.17 ms |
| 纯探针（无 CAN） | 平均晚 1.6 ms | 0.08 | 21.7 | 1.4 ms |

读法：

- 标准差三者一样（~1.2ms）→ **抖动的来源是模拟器，不是 CAN 栈**。
  纯探针不碰 CAN 也抖这么多，直接证明这一点。
- 平均周期不同 → **漂移的来源是调度方式**。绝对时间会在下一拍追回来
  （所以最小间隔只有 3ms，出现了「补帧」）；BCM 由内核定时器驱动，
  不追，每个周期固定多出约 2ms。
- 所以在这台机器上「BCM 更准」不成立。`dmesg` 里的
  `kvm [1]: HYP mode not available` 说明它是 QEMU 纯软件模拟，
  定时器精度只有毫秒级。真机上两者分别是几十微秒和几微秒的量级。

**一个能确定的结论**：BCM 的**独立性**是真的 —— 进程 `SIGSTOP` 冻住之后，
2 秒内总线上还有 172 帧（86/s），说明帧确实不是进程发的。

## 3. netlink：为什么不能用 write 的返回值判断链路

`write()` 返回 `ENETDOWN` 是被动的：你多久试一次，就有多久的盲区；
而且接口恢复时它不会告诉你，只能靠不停重试。

正确做法是订阅内核的链路事件：

```c
int fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = RTMGRP_LINK };
bind(fd, ...);
/* 之后 recv 到 RTM_NEWLINK / RTM_DELLINK，解析 ifinfomsg + IFLA_IFNAME */
```

`ip link` 命令背后就是这套。三个必须处理的细节：

1. **`ip link del` 会先发一个 `DOWN` 的 NEWLINK，再发 DELLINK**。实测日志里
   7.5s DOWN、7.6s 被删除 —— 如果只处理 DELLINK，会漏掉前面的状态变化。
2. **设备重建后 ifindex 会变**（实测 22 → 23）。恢复时必须重新
   `if_nametoindex()`，绝不能缓存。
3. **`write` 的 errno 仍要兜底**：`ENETDOWN`（接口 down）、
   `ENODEV` / `ENXIO`（设备已消失）。netlink 事件理论上一定先到，
   但内核队列满时也会丢事件。

## 4. CAN FD 对应用层的三个影响

1. **结构体换了**：`struct canfd_frame` 72 字节（`can_id + len + flags + 64 字节数据`）。
   开 `CAN_RAW_FD_FRAMES` 之后，经典帧和 FD 帧可以走同一个套接字。
2. **DLC 变成台阶**：9..15 表示 12/16/20/24/32/48/64 字节。给 13 字节会被
   就近向上取整到 16，**多出来的 3 字节是填充**（一般填 0 或填充图案，
   接收端必须按 DLC 而不是按有效长度解析）。
3. **缓冲区只大不小**：`recv` 给 16 字节的缓冲区，FD 帧会被静默截断
   （返回 72，你只拿到前 16）。这类 bug 表现为「数据莫名其妙少了一半」。

补充两点真机上才会遇到的：

- **BRS**（Bit Rate Switch）让数据段切到更高的位速率，仲裁段仍用低速以兼容
  老节点。vcan 只是把这个标志位存下来，不体现任何速率差异 ——
  所以在这台机器上 BRS 只是「0x01」这个比特，别指望测出加速。
- **ESI**（Error State Indicator）由发送控制器自动置位，表示「我已经进入
  错误被动状态」。接收端看到 ESI=1 就该知道对方快挂了，这是比错误帧更早的预警。

## 5. 真机上还缺的那一块：错误帧与 Bus-Off

vcan 没有物理层，`rx_errors/tx_errors` 恒为 0，造不出错误帧，也到不了 Bus-Off。
真实控制器上，这一块是必须处理的：

```c
/* 1. 打开错误帧过滤，否则错误帧到不了用户态 */
can_err_mask_t mask = CAN_ERR_MASK;   /* 全要 */
setsockopt(s, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &mask, sizeof(mask));

/* 2. 收帧时先看是不是错误帧 */
if (f.can_id & CAN_ERR_FLAG) {
    if (f.can_id & CAN_ERR_BUSOFF) {
        /* 控制器已 bus-off，停止收发。需要 128×11 个隐性位才能自动恢复 */
    }
    if (f.can_id & CAN_ERR_RESTARTED) {
        /* 自动恢复了。此时必须重建业务状态，不能假装什么都没发生 */
    }
    if (f.can_id & CAN_ERR_CRTL) {
        /* f.data[1] 里是 rx/tx 错误计数状态：
           CAN_ERR_CRTL_RX_WARNING / TX_WARNING / RX_PASSIVE / TX_PASSIVE / ... */
    }
}

/* 3. 判定故障源：错误计数在涨，说明是本节点的问题还是总线的问题 */
```

`CAN_ERR_BUSOFF` 和 `CAN_ERR_RESTARTED` 是本实验台在代码结构上**预留但没有机会
触发**的两条路径 —— 把 `link_watch` 里「链路消失 → 关闭 → 恢复 → 重新 bind」
那套状态机套上去就基本对了。真接硬件时补这一段，其余代码不用动。

## 6. 一句话总结

- 周期发送：能用绝对时间就别用相对延时；内容固定就交给 BCM。
- 超时监控：**沉默必须被当成故障**，而且判断要放在最不容易被阻塞的地方。
- 链路管理：靠事件（netlink），不靠失败（write 返回错误）；靠名字恢复，不靠 ifindex。
- FD：DLC 是台阶，不是长度；缓冲区只大不小。
