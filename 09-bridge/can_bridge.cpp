#include "can_bridge.h"

#include <cerrno>
#include <cstring>
#include <poll.h>

#include <linux/can.h>
#include <linux/can/raw.h>
#include <net/if.h>

namespace cb {

CanBridge::~CanBridge() { stop(); }

bool CanBridge::start(const std::string& ifname, uint8_t machine_id) {
    ifname_ = ifname;
    machine_id_ = machine_id;

    unsigned int ifindex = if_nametoindex(ifname.c_str());
    if (ifindex == 0) {
        logf("[CAN] 接口 %s 不存在，先建链路（见 lab9.sh topo up）", ifname.c_str());
        return false;
    }

    int fd = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd < 0) {
        logf("[CAN] socket(PF_CAN) 失败：%s", std::strerror(errno));
        return false;
    }

    // ---- 防线 1：关掉回环。CAN 是广播介质，本机发出的帧默认会回到本机 ----
    // 不关的话，「本地 client 发消息 -> 送 CAN -> 立刻被自己收到 -> 再广播给本地
    // client」，用户会看到自己刚发的消息又弹回来一次。
    int off = 0;
    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_LOOPBACK, &off, sizeof(off)) < 0) {
        logf("[CAN] 关 LOOPBACK 失败：%s", std::strerror(errno));
    }
    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &off, sizeof(off)) < 0) {
        logf("[CAN] 关 RECV_OWN_MSGS 失败：%s", std::strerror(errno));
    }

    // ---- 只收桥接 ID，总线上其它报文（比如车上的 0x180）在内核就被丢掉 ----
    struct can_filter flt {};
    flt.can_id = can_id_;
    flt.can_mask = CAN_SFF_MASK;
    if (setsockopt(fd, SOL_CAN_RAW, CAN_RAW_FILTER, &flt, sizeof(flt)) < 0) {
        logf("[CAN] 设置过滤器失败：%s", std::strerror(errno));
    }

    struct sockaddr_can addr {};
    addr.can_family = AF_CAN;
    addr.can_ifindex = static_cast<int>(ifindex);
    if (bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        logf("[CAN] bind %s 失败：%s", ifname.c_str(), std::strerror(errno));
        ::close(fd);
        return false;
    }

    fd_ = fd;
    run_.store(true);
    tx_th_ = std::thread(&CanBridge::tx_loop, this);
    rx_th_ = std::thread(&CanBridge::rx_loop, this);
    logf("[CAN] 就绪 %s ifindex=%u can_id=0x%03X machine=%u", ifname.c_str(), ifindex, can_id_,
         static_cast<unsigned>(machine_id_));
    return true;
}

void CanBridge::stop() {
    if (!run_.exchange(false)) return;
    q_cv_.notify_all();
    if (tx_th_.joinable()) tx_th_.join();
    if (rx_th_.joinable()) rx_th_.join();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool CanBridge::forward_local(uint8_t client_id, Kind kind, const std::string& body) {
    const size_t len = body.size() + 1;   // +1 是 kind
    if (len > kMaxBody) {
        logf("[CAN] 消息 %zu 字节超过上限 %zu，拒绝发送（不截断）", len, kMaxBody);
        return false;
    }

    Out m;
    m.client = client_id;
    m.kind = kind;
    m.body.reserve(len);
    m.body.push_back(static_cast<char>(kind));
    m.body += body;

    {
        std::lock_guard<std::mutex> lk(q_mu_);
        if (q_.size() > 4096) {
            logf("[CAN] 发送队列积压 4096 条，丢弃本次（对端可能已断链）");
            return false;
        }
        q_.push_back(std::move(m));
    }
    q_cv_.notify_one();
    return true;
}

void CanBridge::tx_loop() {
    while (true) {
        Out m;
        {
            std::unique_lock<std::mutex> lk(q_mu_);
            q_cv_.wait(lk, [this] { return !q_.empty() || !run_.load(); });
            if (!run_.load()) return;
            m = std::move(q_.front());
            q_.pop_front();
        }
        send_one(m);
    }
}

void CanBridge::send_one(const Out& m) {
    const size_t len = m.body.size();
    const size_t total = (len + kFrameData - 1) / kFrameData;
    const uint8_t seq = ++seq_;

    uint64_t sent = 0;
    for (size_t i = 0; i < total; ++i) {
        WireFrame w{};
        w.src_machine = machine_id_;
        w.src_client = m.client;
        w.msg_seq = seq;
        w.frag_idx = static_cast<uint8_t>(i);
        w.total_len = static_cast<uint8_t>(len);
        const size_t off = i * kFrameData;
        size_t n = len - off;
        if (n > kFrameData) n = kFrameData;
        std::memcpy(w.data, m.body.data() + off, n);

        struct can_frame cf {};
        cf.can_id = can_id_;
        cf.can_dlc = 8;
        std::memcpy(cf.data, &w, 8);

        ssize_t r = ::send(fd_, &cf, sizeof(cf), MSG_DONTWAIT);
        if (r != static_cast<ssize_t>(sizeof(cf))) {
            err_tx_++;
            // 断链时每一帧都会失败，日志限速到 1 秒一条，否则刷屏
            double t = now_s();
            if (t - last_err_log_ > 1.0) {
                last_err_log_ = t;
                logf("[CAN] 发送失败 errno=%d (%s)，本条消息剩余分片放弃", errno,
                     std::strerror(errno));
            }
            break;   // 宁可不发，也不要让对端收到半条消息
        }
        ++sent;
    }

    tx_frames_ += sent;
    if (sent == total) {
        tx_msgs_++;
    }
}

void CanBridge::rx_loop() {
    struct pollfd p {};
    p.fd = fd_;
    p.events = POLLIN;

    while (run_.load()) {
        p.revents = 0;
        int r = ::poll(&p, 1, 200);
        if (r < 0) {
            if (errno == EINTR) continue;
            logf("[CAN] poll 失败：%s", std::strerror(errno));
            break;
        }
        if (r == 0) {
            expire();
            continue;
        }

        struct can_frame cf {};
        ssize_t n = ::recv(fd_, &cf, sizeof(cf), MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            // 接口 down / 控制器掉线 / 队列满都不是「socket 坏了」。阶段 8 的结论：
            // 接口 down 时 socket 仍然有效，等它 up 回来就能继续收，绝不能 break。
            if (errno == ENETDOWN || errno == ENXIO || errno == ENOBUFS) {
                double t = now_s();
                if (t - last_rx_err_log_ > 1.0) {
                    last_rx_err_log_ = t;
                    logf("[CAN] 接口暂不可用 errno=%d (%s)，等待恢复（socket 保留）", errno,
                         std::strerror(errno));
                }
                usleep(100 * 1000);
                continue;
            }
            logf("[CAN] recv 失败：%s", std::strerror(errno));
            break;
        }
        if (n != static_cast<ssize_t>(sizeof(cf))) continue;
        if (cf.can_id & (CAN_RTR_FLAG | CAN_ERR_FLAG)) continue;
        if (cf.can_dlc != 8) continue;

        handle_frame(cf.data);
        expire();
    }
}

void CanBridge::handle_frame(const uint8_t* data) {
    WireFrame w {};
    std::memcpy(&w, data, 8);

    // ---- 防线 3：机器号是自己的，直接丢 ----
    // 正常情况 LOOPBACK 已关，本机收不到自己的帧；但如果有人把回环打开了，
    // 或者两头接口配成同一个 ID，这里能兜住，不至于变成广播风暴。
    if (w.src_machine == machine_id_) {
        err_echo_++;
        return;
    }
    if (w.total_len == 0) return;

    const size_t total = (w.total_len + kFrameData - 1) / kFrameData;
    if (w.frag_idx >= total) return;

    const uint16_t key = static_cast<uint16_t>((w.src_machine << 8) | w.src_client);

    if (w.frag_idx == 0) {
        Reasm r;
        r.total = static_cast<uint8_t>(total);
        r.next = 1;
        r.seq = w.msg_seq;
        r.len = w.total_len;
        r.t0 = now_s();
        r.buf.reserve(w.total_len);
        size_t n = w.total_len < kFrameData ? w.total_len : kFrameData;
        r.buf.append(reinterpret_cast<const char*>(w.data), n);

        if (total == 1) {   // 单帧消息，最常见
            deliver(r, w);
            return;
        }
        // 半条新消息到达，把同源的旧残留顶掉
        reasm_[key] = std::move(r);
        return;
    }

    auto it = reasm_.find(key);
    if (it == reasm_.end()) {
        err_orphan_++;   // 漏了首帧，或者首帧还在路上
        return;
    }
    Reasm& r = it->second;
    if (r.seq != w.msg_seq || r.next != w.frag_idx) {
        // 序号对不上 —— 和阶段 5 那个「CF 序号错乱」同一个坑：
        // 宁可丢掉整条，也不能把两条消息的碎片拼在一起。
        err_seq_++;
        logf("[CAN] 分片错接：M%u/c%u 期望 seq=%u idx=%u，实收 seq=%u idx=%u，丢弃",
             static_cast<unsigned>(w.src_machine), static_cast<unsigned>(w.src_client),
             static_cast<unsigned>(r.seq), static_cast<unsigned>(r.next),
             static_cast<unsigned>(w.msg_seq), static_cast<unsigned>(w.frag_idx));
        reasm_.erase(it);
        return;
    }

    size_t remain = r.len - r.buf.size();
    size_t n = remain < kFrameData ? remain : kFrameData;
    r.buf.append(reinterpret_cast<const char*>(w.data), n);
    ++r.next;

    if (r.next == r.total) {
        Reasm done = std::move(r);
        reasm_.erase(it);
        deliver(done, w);
    }
}

void CanBridge::deliver(const Reasm& r, const WireFrame& w) {
    if (r.buf.empty()) return;
    Kind k = static_cast<Kind>(static_cast<uint8_t>(r.buf[0]));
    std::string body = r.buf.substr(1);
    rx_msgs_++;
    if (handler_) handler_(w.src_machine, k, w.src_client, body);
}

void CanBridge::expire() {
    if (reasm_.empty()) return;
    const double t = now_s();
    for (auto it = reasm_.begin(); it != reasm_.end();) {
        if (t - it->second.t0 > 0.5) {
            err_timeout_++;
            logf("[CAN] 重组超时 M%u/c%u：%zu/%zu 字节，丢弃半条",
                 static_cast<unsigned>(it->first >> 8),
                 static_cast<unsigned>(it->first & 0xFF), it->second.buf.size(),
                 it->second.len);
            it = reasm_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace cb
