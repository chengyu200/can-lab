#pragma once
// 09-bridge/can_bridge_isotp.h —— 用阶段 5 的用户态 ISO-TP 做桥接后端
//
// 为什么值得单独写一个后端：阶段 9 的 CanBridge 用的是自研的 5 字节帧头，
// 每帧只能带 3 字节负载，上限 255。ISO-TP 是标准做法：SF 带 7 字节、
// CF 带 7 字节、上限 4095，而且带流控（BS/STmin），对端慢的时候会被迫减速，
// 而不是像自研协议那样「闷头发完、丢了你也不知道」。
//
// 但 ISO-TP 和自研协议有两个本质差异，改的时候必须认清：
//
//   1) 它是**单播**。ISO-TP 的地址就是 CAN ID 本身，一对 tx_id/rx_id 只能
//      描述「我和某一个对端」这一条会话。自研协议是广播，发一遍所有机器都收到。
//      所以这里必须「每台对端一个通道、一条消息发 N 遍」。
//
//   2) 它的发送是**阻塞**的（首帧发完要等 FC，最多 N_BS=1s）。
//      自研协议的 tx 线程只有一个，若直接在里面调 isotp_send，一台对端掉电
//      就能把整个发送队列卡住 1 秒。所以这里改成「每台对端一个 tx 线程」，
//      一台卡住不影响别人 —— 这是拿线程数换隔离，N 台机器就是 2N 个线程。
//
// 通道 CAN ID 分配（11 位标准帧，最多 15 台机器）：
//      我 → 对端：0x600 + 我*16 + 对端
//      对端 → 我：0x600 + 对端*16 + 我
//  M1 → M2 是 0x612，M2 → M1 是 0x621，两个方向天然不同 ID，ISO-TP 要求
//  的「收发 ID 不能相同」自动满足。

#include "can_bridge.h"

// isotp_user.h 是 C 头、isotp_user.c 是 C 源码（阶段 5 原样复用，一行没改），
// 所以这里必须用 extern "C" 包住，否则 C++ 链接时找不到符号。
extern "C" {
#include "isotp_user.h"
}

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace cb {

// ISO-TP 载荷里还要塞 3 字节头（src_machine / src_client / kind），
// 所以单条消息上限比 ISOTP_MAX_MSG 少 3。4093 字节，够传一段 JSON 了。
constexpr size_t kMaxBodyIsotp = ISOTP_MAX_MSG - 3;

class IsotpBridge : public IBridge {
public:
    // peers: 总线上的其它机器号（不含自己）。两台机器时各写对方一个号即可。
    explicit IsotpBridge(std::vector<uint8_t> peers) : peers_(std::move(peers)) {}
    ~IsotpBridge() override { stop(); }

    bool start(const std::string& ifname, uint8_t machine_id) override;
    void stop() override;
    void set_handler(Handler h) override { handler_ = std::move(h); }
    bool forward_local(uint8_t client_id, Kind kind, const std::string& body) override;

    uint64_t tx_frames() const override { return tx_frames_.load(); }
    uint64_t tx_msgs() const override { return tx_msgs_.load(); }
    uint64_t rx_msgs() const override { return rx_msgs_.load(); }
    uint64_t err_tx() const override { return err_tx_.load(); }
    uint64_t err_echo() const override { return err_echo_.load(); }
    uint64_t err_orphan() const override { return err_orphan_.load(); }
    uint64_t err_seq() const override { return err_seq_.load(); }
    uint64_t err_timeout() const override { return err_timeout_.load(); }

private:
    struct Out {
        std::string data;   // 已经拼好 3 字节头：[machine][client][kind] + body
    };

    struct Peer {
        uint8_t id = 0;
        uint32_t my_tx = 0;      // 我发给它的 CAN ID
        uint32_t peer_tx = 0;    // 它发给我的 CAN ID

        // 两个 socket、同一个过滤器，这是本实现的关键技巧：
        // CAN_RAW 是**多播**的 —— 每个匹配的 socket 各拿一份副本。于是
        //   txc 只用来 send（顺便在 isotp_send 内部读 FC，读到别的帧就丢）
        //   rxc 只用来 recv（isotp_recv 内部发 FC，读到 FC 就忽略）
        // 两个线程各读各的 fd，不会抢同一份数据。阶段 5 那个单 socket 版本
        // 是半双工的（收发共用 fd，发送时会把对端的数据帧当垃圾丢掉），
        // 直接拿来跑双向桥接会丢消息。
        struct isotp_chan txc {};
        struct isotp_chan rxc {};

        std::thread tx_th;
        std::thread rx_th;
        std::mutex mu;
        std::condition_variable cv;
        std::deque<Out> q;
        uint64_t dropped = 0;
        unsigned long seen_tx_frames = 0;
    };

    static uint32_t id_of(uint8_t from, uint8_t to) {
        return 0x600u + static_cast<uint32_t>(from) * 16u + static_cast<uint32_t>(to);
    }

    void tx_loop(Peer* p);
    void rx_loop(Peer* p);
    void drain_tx(Peer* p);

    std::vector<uint8_t> peers_;
    std::vector<std::unique_ptr<Peer>> chans_;
    std::string ifname_;
    uint8_t machine_id_ = 0;
    Handler handler_;

    std::atomic<bool> run_{false};
    std::atomic<uint64_t> tx_frames_{0}, tx_msgs_{0}, rx_msgs_{0};
    std::atomic<uint64_t> err_tx_{0}, err_echo_{0}, err_orphan_{0}, err_seq_{0}, err_timeout_{0};
    double last_err_log_ = 0;
    double last_rx_err_log_ = 0;
};

}  // namespace cb
