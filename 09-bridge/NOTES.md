# 阶段 9：给 tcp_server 加 CAN 互联

> 目标：每台机器一个 `tcp_server`（C++），本机若干 client 连它，消息在本地互转。
> 现在要让两台（以后 N 台）机器的 `tcp_server` 通过 CAN 交换这些消息。

**结论先行**：client 一行不用改；`tcp_server` 只加 4 处、不到 60 行；
新增一个 `CanBridge` 类（约 260 行）和一份 8 字节的线上格式定义。

```
目录
  common.h          TCP 分帧 + CAN 线上格式 + 日志（server/client 共用）
  can_bridge.h/.cpp 要抄进你们项目的那个类
  tcp_server.cpp    「你们现在的 server」的模拟版，4 处改动都有 [BRIDGE-n] 标记
  client.cpp        模拟 client A/B/C
  lab9.sh           两台「机器」+ 一条 CAN 线，一键跑完
```

---

## 1. 改前 / 改后

改前（你们现在的样子）：

```
  A ─┐
  B ─┼─TCP→ tcp_server ──→ 广播给本机其它 client
  C ─┘
```

改后：

```
        机器 1                                   机器 2
  A ─┐                                    ┌─ A'
  B ─┼─TCP→ tcp_server ─┐        ┌───────┼─ B'
  C ─┘                  │        │        └─ C'
                   [CanBridge]  [CanBridge]
                        │           │
                     can0 ~~~~~~~~ can0     ← 一条 CAN 总线
```

消息只有两条路径，**不存在第三条**：

| 来源 | 去哪 |
|---|---|
| 本地 client 发来 | ① 广播给本机其它 client（原有行为）② 送一份上 CAN |
| CAN 收到 | 只广播给本机所有 client |

第二条路径里**绝不能**再调 `forward_local` —— 这就是防环的全部秘密。

---

## 2. 四个插入点

| 编号 | 位置 | 加什么 | 行数 |
|---|---|---|---|
| `[BRIDGE-1]` | `main()`，`listen()` 之前 | `set_handler()` + `start()` | 6 |
| `[BRIDGE-2]` | 本地消息处理函数末尾 + 注册成功处 | `forward_local(...)` | 2 |
| `[BRIDGE-3]` | 新增的 CAN 回调里 | `broadcast_local(...)`，**只此一句** | 15（含分支） |
| `[BRIDGE-4]` | `main()` 退出前 | `stop()` + 打统计 | 3 |

### [BRIDGE-1] 启动

```cpp
// main() 里，listen 之前
CanBridge bridge;                       // 全局唯一，一个进程只允许一个 CAN 写者
bridge.set_handler(on_can_message);     // 先挂回调，再 start，避免帧到了没人接
if (!bridge.start(ifname, machine_id))  // ifname 如 "can0"；machine_id 每台机器必须不同
    return 1;
```

`machine_id` 从配置文件 / 环境变量 / 主机名哈希来，别写死。两台机器配成一样，
就会出现「两台都认为帧是自己发的」→ 全部丢弃（`err_echo` 涨），现象是单向不通。

### [BRIDGE-2] 本地消息 → 广播 + 上 CAN

```cpp
void on_local_message(const Client& c, const std::string& body) {
    broadcast_local(g_machine, c.id, body, c.fd);   // ① 原有行为，一个字没动
    bridge.forward_local(c.id, kData, body);        // ② 新增：再送一份上 CAN
}
```

这一行放在**广播函数之后**，不要放进 `broadcast_local()` 内部 —— 因为
`broadcast_local()` 也会被 CAN 回调调用（[BRIDGE-3]），放进去就成环了。

注册成功处也补一条，让对端知道来的是谁：

```cpp
bridge.forward_local(c.id, kJoin, name);
```

### [BRIDGE-3] CAN → 只广播本地

```cpp
void on_can_message(uint8_t machine, Kind k, uint8_t cid, const std::string& body) {
    switch (k) {
        case kJoin:  /* 记下名字，广播「上线了」 */ break;
        case kLeave: /* 广播「离开了」 */          break;
        case kData:  broadcast_local(machine, cid, body, -1); break;  // 就这一句
    }
    // 这里没有、也不能有 forward_local
}
```

### [BRIDGE-4] 收尾

```cpp
bridge.stop();
logf("发 %llu 条 / %llu 帧，收 %llu 条", bridge.tx_msgs(), bridge.tx_frames(), bridge.rx_msgs());
```

`stop()` 先停队列、再 join 两个线程、最后 close。顺序反了会在 `send()` 里
拿到 `EBADF`，严重时线程还在跑、fd 已经关了。

---

## 3. 三个必须做对的地方

这三条不是"最佳实践"，是**不这么做就一定出故障**。

### 3.1 关掉回环，否则自己转发给自己

CAN 是广播介质，本机发出的帧默认会回到本机（`CAN_RAW_LOOPBACK=1`）。
不关的话，A 发一句话，A 自己会再收到一遍，看起来像"消息重复"。

```c
int off = 0;
setsockopt(fd, SOL_CAN_RAW, CAN_RAW_LOOPBACK, &off, sizeof(off));
```

还有第二道闸：帧头带 `src_machine`，收到自己的直接丢（`err_echo` 计数）。
因为总有人会把回环打开、或者把两台的 machine_id 配成一样，这道闸能兜住。

**副作用要知道**：关掉回环后，本机 `candump` 看不到本机发出的帧（实测见 §6）。
想抓自己发的东西，把 candump 挂在**对端**接口上。

### 3.2 一条消息的多个分片不能交错

这是最容易写错、且只在"两个 client 同时发长消息"时才暴露的 bug。

错误写法（直接在各 client 的线程里 `sendto`）：

```
线程1: A 的 5 帧 ……
线程2:          B 的 3 帧 ……       ← 对端收到的是一条混在一起的垃圾
```

正确做法：`forward_local()` 只入队，**只有一个 tx 线程碰 CAN socket**。

```cpp
void forward_local(...) {          // 任意线程可调
    lock; q_.push_back(msg);  unlock;
    cv_.notify_one();
}
void tx_loop() {                   // 唯一写 socket 的线程
    while (running) { wait; m = pop(); send_one(m); }   // 分片在这里串行展开
}
```

### 3.3 8 字节装不下一条消息，得分片

TCP 上一条消息可以是 4KB，CAN 一帧只有 8 字节。必须自己切，切完在对面拼。

帧格式（经典 CAN，标准帧 0x500）：

| 字节 | 字段 | 说明 |
|---|---|---|
| 0 | `src_machine` | 发送方机器号 1..254 |
| 1 | `src_client` | 发送方 client 在本机的注册序号 |
| 2 | `msg_seq` | 本条消息的流水号（每条 +1） |
| 3 | `frag_idx` | 分片下标，0 起 |
| 4 | `total_len` | 整条消息长度，**每帧都带** |
| 5..7 | `data` | 3 字节负载 |

两个设计决定值得解释：

- **`total_len` 每帧都带**，而不是只放首帧。代价是每帧浪费 1 字节，收益是
  「漏了首帧」也能立即发现下标越界、瞬间丢弃，不会把半条消息当下一条的开头。
- **`msg_seq` 必校验**。序号对不上就整条丢弃 —— 和阶段 5 那个「CF 序号错乱」
  同一个道理：CAN 没有传输层保证，拼错的数据比丢失的数据危险得多。

实测一帧长这样（A 发 `hello-from-A`，13 字节 → 5 帧）：

```
cb1  500  [8]  01 02 03 00 0D 01 68 65     ← machine=1 client=2 seq=3 idx=0 len=13 "he"
cb1  500  [8]  01 02 03 01 0D 6C 6C 6F     ←                             idx=1 "llo"
cb1  500  [8]  01 02 03 02 0D 2D 66 72
cb1  500  [8]  01 02 03 03 0D 6F 6D 2D
cb1  500  [8]  01 02 03 04 0D 41 00 00     ← 末帧只用了 1 字节
```

---

## 4. 与现有代码的其它接缝

| 问题 | 处理 |
|---|---|
| client 显示格式变了 | 广播内容多了前缀 `[M1/A] `。client 不改也能跑，只是会多显示这段；想干净就让 client 认识这个前缀，或者把前缀去掉（`label_of()` 那里一行） |
| 远端 client 的名字 | 靠 `kJoin` 控制消息在启动时广播一次，对端缓存成 `(machine,client) -> name`。没收到就退化显示 `c3` |
| 线程模型 | 你们若是 epoll 单线程，`CanBridge` 的两个后台线程照样能用；但回调里做 TCP 写可能阻塞收帧线程，所以给每个 client fd 设了 `SO_SNDTIMEO=100ms`，写不出去就踢掉 |
| 消息边界 | 用 4 字节长度前缀。如果你们现在用 `\n` 分隔，也能用，只是内容里不能再有换行 |
| 慢 client | 广播是同步写，一个卡住的 client 会拖慢整条广播。演示里靠写超时兜底，生产建议每 client 一个发送队列 |

---

## 5. 实测结果

```sh
./09-bridge/lab9.sh
```

### 5.1 双向都通

```
21:00:04.445 A  发送 hello-from-A
21:00:04.493 B  收到 [M1/A] hello-from-A      ← 本地广播，原有行为
21:00:04.494 C  收到 [M1/A] hello-from-A      ← 跨机，经 CAN

21:00:05.652 C  发送 pong-from-C
21:00:05.700 A  收到 [M2/C] pong-from-C
21:00:05.700 B  收到 [M2/C] pong-from-C
```

### 5.2 断链：本地照转，跨机丢

```
21:00:07.370 M2/srv [CAN] 接口暂不可用 errno=100 (Network is down)，等待恢复（socket 保留）
21:00:07.652 A  发送 while-link-down
21:00:07.653 B  收到 [M1/A] while-link-down    ← 本地不受影响
                                              ← C 没收到，且 M1 侧没有任何报错
```

**这条最值得记住**：`sendto` 返回成功，对端没收到，发送端一个错误都没有。

退出统计对得上的账：

```
M1/srv  [CAN] 统计：发 8 条/22 帧，收 4 条；错误 发送0 回环0 孤片0 错序0 超时0
M2/srv  [CAN] 统计：发 4 条/11 帧，收 7 条；错误 发送0 回环0 孤片0 错序0 超时0
```

M1 发了 8 条，M2 只收到 7 条 —— 差的那 1 条正是 `while-link-down`，
而 M1 的错误计数全是 0。和阶段 5「CF 序号错乱、发送端仍报成功」是同一个根因：
**CAN 没有传输层 ACK**。candump 里也能看到 `msg_seq` 从 04 直接跳到 06。

### 5.3 恢复：不用重启进程

```
21:00:09.160 A  发送 after-recover-A
21:00:09.164 C  收到 [M1/A] after-recover-A     ← 断链 4 秒后自动恢复
21:00:09.166 A  收到 [M2/C] after-recover
```

前提是接收线程遇到 `ENETDOWN` 只等待、不退出。如果按普通错误处理 `break` 掉，
接口 up 回来之后这个进程就只剩半条命（能发不能收），而且日志里毫无痕迹。
这是阶段 8「接口 down 与接口删除是两回事」的直接延续：**down 时 socket 仍有效，
删掉才需要重新 bind**。

---

## 6. 上生产前要定的 5 件事

| # | 问题 | 现在怎么做的 | 建议 |
|---|---|---|---|
| 1 | 单条消息上限 | 255 字节（`total_len` 是 1 字节），超了**拒绝**不截断 | 要更大就把长度改 2 字节；再大（KB 级）换 `can_isotp`，内核模块已有，上限 4095，流控超时都不用自己写 |
| 2 | 可靠性 | best-effort，丢就丢了 | 需要可靠就在应用层加 ACK + 重发（带 msg_seq 去重）。别指望 CAN 硬件层，它只保证「帧没被破坏」，不保证「送到了」 |
| 3 | 顺序 | 不保证。同 ID 的帧按发送顺序上总线，但不同优先级报文会插队 | 需要严格顺序就按 msg_seq 重排；控制类消息建议用不同 CAN ID 拿到更高优先级 |
| 4 | 多于两台 | 现在所有机器都收到所有帧（CAN 是广播） | 加 `dst_machine` 字段（帧头还剩不下，可以放 data 首字节），或者接受「全网广播、本机过滤」—— 车上的做法多半是后者 |
| 5 | 带宽 | 8 字节帧约 111 bit ≈ 222µs，10ms 周期占 2.2% | 3 字节有效载荷意味着**实际占用是 8 倍**。两个 client 同时刷长消息时会堵，务必测一下最坏情况 |

另外两个坑，和本阶段无关但一定会遇到：

- **CAN 口只有一个**：如果机器上还有别的程序用这个 `can0`，记得用 `CAN_RAW_FILTER`
  各自只收自己的 ID（本实现默认已经这么做了），否则两边互相抢帧。
- **`machine_id` 的唯一性**：写进配置文件，不要让程序自己去猜。配重复了现象是
  「本地正常、跨机全丢、日志干净」—— 最难查的一类。

---

## 7. 什么时候该换掉这套自研协议

现在这套分片头是**教学版**：4 字节开销/8 字节载荷、无流控、无重传。
下面三种情况出现任意一条，就该换成标准协议：

1. 单条消息经常超过 255 字节 → `can_isotp`（内核态，`socket(PF_CAN, SOCK_DGRAM, CAN_ISOTP)`）
2. 需要和第三方设备互通 → 对方多半只说 ISO-TP / J1939，不会认你的私有头
3. 需要诊断和刷写共用一条总线 → UDS over ISO-TP（阶段 6 已经验证过）

它们的共同点：**流控、超时、序号回绕都由协议栈负责**，你只面对"一条消息"。
代价是双端都得实现同一套协议，而 `can_isotp` 内核模块只需要两边都 `modprobe`。

---

## 8. 换成 ISO-TP：阶段 5 的用户态实现能直接用在桥接里吗？

能，但**不是 `#include` 进来就完事**。阶段 5 的 `isotp_user.c` 是照着
「一条点对点会话」写的，而桥接要的是「一台对多台、双向同时跑」。中间隔着
三个不匹配，每个都会以「消息丢了但不报错」的形式暴露。

### 8.1 三个不匹配

| # | 阶段 5 的实现 | 桥接需要 | 不改会怎样 |
|---|---|---|---|
| 1 | **单播**：一对 `tx_id/rx_id` 只能描述一个对端 | 一条消息要让 N 台机器都收到 | 三台机器挂在同一条总线上时，所有人抢同一对 ID，收到的是别人的半条消息 |
| 2 | **阻塞发送**：`isotp_send` 首帧发完要等 FC，最长 `N_BS=1s` | 一个对端掉电不能拖累其它对端 | 单 tx 线程会被死掉的对端卡满 1 秒，期间所有消息排队 |
| 3 | **半双工**：收和发共用一个 socket，`wait_fc()` 把非 FC 帧直接丢掉 | 双向同时发 | 本端在发长消息时，对端发来的数据帧被当垃圾扔掉；对方那边看起来「发出去了」，你这边少了半条 |

前两个是设计取舍，第三个是**真的会丢数据的坑**，值得单独说。

### 8.2 解法：每对端两个 socket，靠 CAN_RAW 的多播各拿一份

`wait_fc()` 必须读那个 fd（FC 和业务帧走同一个 CAN ID），所以「一个 socket
两个线程读」是不可能的。但 CAN_RAW 是**多播**的：同一帧会复制给每一个过滤器
匹配的 socket。于是：

```
                 ┌── txc (fd1) 过滤器 = 对端 tx_id ── 只给 tx 线程用（send + 等 FC）
M1 ⇄ M2 一对 ID ─┤
                 └── rxc (fd2) 过滤器 = 对端 tx_id ── 只给 rx 线程用（recv + 发 FC）
```

同一个过滤器开两个 socket，内核各投一份副本：tx 线程读走 FC（顺手把数据帧扔掉，
无所谓，rxc 有一份完整的），rx 线程读走数据帧（FC 帧 `pci==3` 它本来就忽略）。
阶段 5 的代码**一行没改**，问题在包装层解决。

代价：每对端 2 个 socket、2 个线程。3 台机器 = 每条消息发 2 遍、4 个线程。
这就是 ISO-TP 相对自研广播协议最实在的成本。

这个绕过成不成立，只有一个办法能验：**双向同时发**。单向测试永远碰不到
`wait_fc` 丢帧的那一刻。`09-bridge/t_bidir.sh` 就是干这个的 —— 两端各发一条
303 字节消息，只错开 20ms：

```
21:42:08.059 A  发送 300×A
21:42:08.080 C  发送 300×C         ← 才差 21ms，CF 正面撞上
21:42:08.105 C  收到 [M1/A] 307 字节
21:42:08.106 A  收到 [M2/C] 307 字节
M1: 发 2 条/45 帧，收 2 条；错误 错序0 超时0
M2: 发 2 条/45 帧，收 2 条；错误 错序0 超时0
```

45 帧 = 1 帧上线通知 + 44 帧大消息（FF + 43×CF）。两边账都对得上。

### 8.3 CAN ID 分配

```
我 → 对端：0x600 + 我*16 + 对端
对端 → 我：0x600 + 对端*16 + 我
```

M1→M2 = `0x612`，M2→M1 = `0x621`。两个方向天然不同 ID，ISO-TP 要求的
「tx_id ≠ rx_id」自动满足；11 位 ID 里 `0x600~0x6FF` 正好装下 15 台机器。

### 8.4 载荷效率：这是换过去最直接的理由

| 消息 | 自研后端（3 B/帧） | ISO-TP 后端（7 B/帧） |
|---|---|---|
| `hello-from-A`（12 字符） | 5 帧 | FF + 2×CF = **3 帧** |
| `hi`（2 字符） | 1 帧 | 1 帧（SF） |
| 300 字节大消息 | **拒绝**，超过 255 上限 | FF + 43×CF = 44 帧，正常送达 |

有效载荷占比：3/8 = 37.5% → 7/8 = 87.5%。上限：255 → 4093。

线上抓到的样子（`candump -td cb0,600:700`）：

```
cb1  612  [8]  10 0F 01 01 01 68 65 6C     ← FF，声明总长 0x00F=15
cb1  621  [3]  30 00 00                    ← FC，BS=0/STmin=0：一次发完，不限速
cb1  612  [8]  21 6C 6F 2D 66 72 6F 6D     ← CF 序号 1
cb1  612  [8]  22 2D 41 00 00 00 00 00     ← CF 序号 2，只有 2 字节有效
```

头两字节永远是 `[machine][client]`，第三字节 `kind`，之后才是业务正文 ——
ISO-TP 只保证「一条有序字节流」，**消息里谁发的、是什么类型，协议不管**，
这三字节头还是得自己加。

### 8.5 断链行为：两种后端的差别比想象中大

阶段 9 的自研后端，接口一 down，`send()` 立刻返回 `ENETDOWN`，当场就知道这条没出去。

ISO-TP 后端不是这样。实测：

```
21:39:43.690  M2/srv  [ISOTP] 接口暂不可用 errno=100 (Network is down)，等待恢复
21:39:46.387  A 发送 while-link-down          ← 此刻链路是断的
21:39:46.387  B 收到 [M1/A] while-link-down   ← 本地广播照常，客户端毫无感知
                                              ← C 没收到，M1 侧也没有任何错误
21:39:47.397  M1/srv  [ISOTP] M2 发送失败 errno=110 (Connection timed out)
21:39:48.793  A 发送 after-recover-A          ← 链路恢复后
21:39:48.794  C 收到 [M1/A] after-recover-A   ← 自动自愈，进程不用重启
```

关键那 1 秒：**首帧写进内核没报错，但帧根本没到对端**，于是卡在等 FC 上，
`N_BS=1000ms` 之后才以 `ETIMEDOUT` 收场。这就是阶段 6 里 UDS 必须有 P2 超时的
根本原因 —— CAN 没有传输层 ACK，「写成功」和「对方收到」是两件事。
自研后端在这一点上反而更诚实（发送即失败），因为它是 fire-and-forget。

### 8.6 两个后端怎么选

| 场景 | 选哪个 | 理由 |
|---|---|---|
| 只有两台机器，消息都小于 255 字节 | 哪个都行 | 自研的更简单，一个 socket 一个线程 |
| 三台以上 | 自研 | ISO-TP 是单播，N 台要发 N 遍，还要维护 N 对 ID |
| 消息经常超过 255 字节 / 传 JSON、文件片段 | ISO-TP | 上限 4093，且不用自己写分片 |
| 要和第三方 ECU、诊断仪、刷写工具互通 | ISO-TP / UDS | 别人只认标准 |
| 带宽紧张、需要控速 | ISO-TP | `STmin` 是接收方说了算，自研协议没有反压 |
| 对端可能随时掉电，且要求「立刻知道失败」 | 自研 + 应用层 ACK | ISO-TP 要等满 N_BS，且不做重传 |

### 8.7 已知限制（真要上生产得补）

1. **txc 的积压只在每次发送后清空**。双向满负荷时，txc 里堆的对端数据帧会拖慢
   `wait_fc` 找到 FC 的速度。正确做法是把两个方向的读合并进一个事件循环，
   而不是两个线程各读一个 fd。
2. **没有重传**。ISO-TP 规范本身就是「超时报错、上层重试」，UDS 的 P2* 才是重试策略。
   桥接要可靠，得在 3 字节头后面再加 msg_seq 做去重 + ACK。
3. **N_BS 用默认 1s**。链路抖动大的场合应下调到 100~200ms，否则一条坏消息会
   让该对端队列卡顿 1 秒。
4. **`ISOTP_MAX_MSG=4096` 是阶段 5 定的**，真按 4095 上限收会差一个字节的边界，
   桥接这边用 4093（减去 3 字节头）避开了。

---

## 9. 把 tcp_server 包成类之后，两个对象怎么接（v2/）

`09-bridge/tcp_server.cpp` 是**给人看改哪几行**的：全局变量 + 静态函数，
一眼就能数出 `[BRIDGE-1..4]` 四个改动点。但真实工程里 `tcp_server` 是个类，
没有全局变量可用，也不能在类里 include 桥接的头文件。所以另写了一版：

```
09-bridge/v2/server.h      class TcpServer（业务全在这）
09-bridge/v2/server.cpp
09-bridge/v2/main.cpp      只做三件事：造对象、接线、按顺序关停
```

```sh
# 同一套脚本，换个二进制就能跑类化版，输出逐行可比
SRV_BIN=$PWD/09-bridge/v2/tcp_server ./09-bridge/lab10.sh
```

### 9.1 所有权：谁都不拥有谁

```
        main
       /    \
 TcpServer   IBridge(CanBridge 或 IsotpBridge)
```

`main` 用两个栈/unique_ptr 对象持有它们，**两者之间没有任何指针**。
它们互相只认 `std::function`：

| 方向 | 谁设 | 谁调 | 在哪个线程调 | 能否阻塞 |
|---|---|---|---|---|
| 本地 → CAN | `server.set_can_sink(...)` → 里面包 `bridge->forward_local()` | server | 该 client 的读线程 | 短暂（只入队，不碰 socket） |
| CAN → 本地 | `bridge->set_handler(...)` → 里面包 `server.from_can()` | bridge | 桥接的 rx 线程（ISO-TP 版每对端一个） | 会短暂持 server 的锁 |

不互相持有带来三个好处：没有循环引用、不用前向声明对方的头文件、
换桥接实现（J1939 / 共享内存 / 另一个进程）时 `server.cpp` 一行不改。

### 9.2 四条纪律（类化版比 flat 版更容易踩）

1. **绝不持 `mu_` 做 socket 写**。`broadcast_local()` 只在锁里拷一份 client
   列表，出锁再逐个写。flat 版是持锁 `send_msg` 的 —— 一个卡住的客户端会让
   整个 server 连 accept 都进不来。
2. **绝不持 `mu_` 调 `can_sink_`**。桥接可能阻塞（ISO-TP 等 FC 最长 1 秒），
   持锁调用等于把整个 server 押给一条坏链路。
3. **`from_can()` 里绝不回调 `can_sink_`**。防环的第二道闸，和 flat 版同理。
4. **每个 client 一把写锁**（`Client::wmu`）。同一个 fd 可能被两个线程写：
   它自己的读线程在广播，桥接的 rx 线程在投递。不加锁会写出半个长度头。

### 9.3 关停顺序：先断回调来源

```cpp
server.serve();          // 阻塞
if (bridge) bridge->stop();   // ① 先 join 掉桥接的 tx/rx 线程
server.stop();                // ② 再拆 server
```

反过来的话，`server.stop()` 已经开始析构成员，桥接的 rx 线程还能调
`from_can()` 去摸已经销毁的 mutex —— 典型症状是「十次里有一次退出时崩溃」。

`bridge->stop()` 可能要等 1 秒（ISO-TP 的 tx 线程卡在等 FC），
这段时间 server 还完整活着，回调进来是安全的。**先停谁，取决于谁在调谁。**

### 9.4 实测踩到的坑：把运行标志当幂等标志用

第一版 `stop()` 写成：

```cpp
void TcpServer::stop() {
    if (!run_.exchange(false)) return;   // ← 错
    ... 关监听、shutdown client、join 所有连接线程
}
```

信号处理函数里调了 `request_stop()`（把 `run_` 置 false），
等 `main` 再调 `stop()` 时 `run_` 已经是 false，于是**直接返回**：
监听 fd 没关、连接线程一个都没 join。进程退出时 `std::vector<std::thread>`
析构，里面还有 joinable 的线程 → `terminate called without an active exception`。

修复是把两件事分开：

```cpp
std::atomic<bool> run_{false};      // serve() 的循环条件
std::atomic<bool> stopped_{false};  // stop() 的幂等标志

void TcpServer::stop() {
    if (stopped_.exchange(true)) return;   // 只有这里判幂等
    run_.store(false);
    ...
}
```

教训很具体：**「该不该继续跑」和「清理做过没有」是两个状态**，
用同一个原子变量表示，就会在「信号先到、stop 后到」这种时序下丢清理。
而且它只在退出路径上出问题，正常收发全程无感 —— 这种 bug 只能靠
「跑完看退出码 / 盯着 stderr」抓，功能测试抓不到。

### 9.5 统计口径：交给桥接 ≠ 发出去

退出时两行统计是分开的，别混着看：

```
M1/srv [SRV] 统计：client 0 个，本地→CAN 5 条（拒收 0），CAN→本地 2 条
M1/srv [CAN] 统计：发 6 条/54 帧，收 2 条；错误 发送1 回环0 孤片0 错序0 超时1
```

`本地→CAN 5 条` 是 server 把 5 条交给了桥接；`发 6 条` 里有 1 条是上线通知
（kJoin）走的是同一条路；`错误 发送1` 是断链那条在桥接内部失败了。
server 根本不知道那条丢了 —— 这正是「出方向是单向的」的直接体现。
