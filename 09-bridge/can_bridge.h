#pragma once
// 09-bridge/can_bridge.h —— 要抄进你们 tcp_server 的那个类
//
// 它对外的面只有四个：
//   start()        绑定 CAN 接口、起两个后台线程
//   stop()         收尾
//   forward_local()  「本地 client 发来一条消息」—— 任意线程可调，内部入队
//   set_handler()   「收到远端消息」—— 回调里做本地广播
//
// 关键设计：调用 forward_local 的线程（可能是每个 client 的连接线程）绝不
// 直接碰 CAN socket，只往队列里塞。真正写 socket 的永远只有 tx 线程一个，
// 所以一条消息的多个分片不可能和另一条消息的分片交错。

#include "common.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace cb {

// ============================================================================
// 后端接口：两种实现可互换，tcp_server 只认这个面
//
//   CanBridge      —— 阶段 9 自研分片（3 字节/帧，广播，单 socket）
//   IsotpBridge    —— ISO-TP 分片（7 字节/帧，点对点，每对机器一个通道）
//
// 抽这一层不是为了「面向对象好看」，而是因为两者**语义不同**：
// 自研协议是广播（一条消息只发一遍，所有机器都能收），ISO-TP 是单播
// （N 台机器就要发 N 遍，各自流控）。上层调用方式一样，代价不一样，
// 所以必须能在同一个 server 里换着跑、对着比。
// ============================================================================
class IBridge {
public:
    // machine: 发送方机器号；kind: 消息种类；client: 发送方在本机的序号；body: 去掉 kind 的正文
    using Handler = std::function<void(uint8_t machine, Kind kind, uint8_t client,
                                       const std::string& body)>;

    virtual ~IBridge() = default;

    virtual bool start(const std::string& ifname, uint8_t machine_id) = 0;
    virtual void stop() = 0;
    virtual void set_handler(Handler h) = 0;
    virtual bool forward_local(uint8_t client_id, Kind kind, const std::string& body) = 0;

    virtual uint64_t tx_frames() const = 0;
    virtual uint64_t tx_msgs() const = 0;
    virtual uint64_t rx_msgs() const = 0;
    virtual uint64_t err_tx() const = 0;
    virtual uint64_t err_echo() const = 0;
    virtual uint64_t err_orphan() const = 0;
    virtual uint64_t err_seq() const = 0;
    virtual uint64_t err_timeout() const = 0;
};

class CanBridge : public IBridge {
public:
    using Handler = IBridge::Handler;

    ~CanBridge() override;
    CanBridge(const CanBridge&) = delete;
    CanBridge& operator=(const CanBridge&) = delete;

    explicit CanBridge(uint32_t can_id = kCanId) : can_id_(can_id) {}

    bool start(const std::string& ifname, uint8_t machine_id) override;
    void stop() override;
    void set_handler(Handler h) override { handler_ = std::move(h); }

    // 线程安全。消息超过 kMaxBody 会被拒绝（返回 false），不截断 —— 截断等于
    // 把一条错误的消息悄悄发给对端，比丢弃更难查。
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
        uint8_t client = 0;
        Kind kind = kData;
        std::string body;
    };

    struct Reasm {
        uint8_t total = 0;     // 总片数
        uint8_t next = 0;      // 期望的下一片下标
        uint8_t seq = 0;       // 本条消息的流水号
        size_t len = 0;        // 整条消息长度
        std::string buf;
        double t0 = 0;
    };

    void tx_loop();
    void rx_loop();
    void send_one(const Out& m);
    void handle_frame(const uint8_t* data);
    void deliver(const Reasm& r, const WireFrame& w);
    void expire();

    int fd_ = -1;
    std::string ifname_;
    uint8_t machine_id_ = 0;
    uint32_t can_id_ = kCanId;
    Handler handler_;

    std::atomic<bool> run_{false};
    std::thread tx_th_;
    std::thread rx_th_;

    std::mutex q_mu_;
    std::condition_variable q_cv_;
    std::deque<Out> q_;

    std::mutex reasm_mu_;                     // 只在 rx 线程里用，留着是为了将来多线程收
    std::map<uint16_t, Reasm> reasm_;         // key = (machine << 8) | client
    uint8_t seq_ = 0;
    double last_err_log_ = 0;                 // 发送失败日志限速
    double last_rx_err_log_ = 0;              // 接口不可用日志限速

    std::atomic<uint64_t> tx_frames_{0}, tx_msgs_{0}, rx_msgs_{0};
    std::atomic<uint64_t> err_tx_{0}, err_echo_{0}, err_orphan_{0}, err_seq_{0}, err_timeout_{0};
};

}  // namespace cb
