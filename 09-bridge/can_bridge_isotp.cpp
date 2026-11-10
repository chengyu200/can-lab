#include "can_bridge_isotp.h"

#include <cerrno>
#include <cstring>

#include <net/if.h>
#include <unistd.h>

namespace cb {

bool IsotpBridge::start(const std::string& ifname, uint8_t machine_id) {
    ifname_ = ifname;
    machine_id_ = machine_id;

    if (peers_.empty()) {
        logf("[ISOTP] 没配置对端机器号。ISO-TP 是单播，必须知道发给谁：加 --peers 2");
        return false;
    }
    if (if_nametoindex(ifname.c_str()) == 0) {
        logf("[ISOTP] 接口 %s 不存在，先建链路（见 lab10.sh topo up）", ifname.c_str());
        return false;
    }

    for (uint8_t pid : peers_) {
        if (pid == machine_id) continue;

        auto p = std::make_unique<Peer>();
        p->id = pid;
        p->my_tx = id_of(machine_id, pid);
        p->peer_tx = id_of(pid, machine_id);

        // 通道 1：只发。isotp_send 内部会在同一个 fd 上读 FC。
        if (isotp_open(&p->txc, ifname.c_str(), p->my_tx, p->peer_tx) < 0) {
            logf("[ISOTP] 打开通道失败 M%u→M%u (tx=0x%03X rx=0x%03X)：%s",
                 static_cast<unsigned>(machine_id), static_cast<unsigned>(pid), p->my_tx,
                 p->peer_tx, std::strerror(errno));
            return false;
        }
        // 通道 2：只收。同一个过滤器，靠 CAN_RAW 的多播各拿一份副本。
        if (isotp_open(&p->rxc, ifname.c_str(), p->my_tx, p->peer_tx) < 0) {
            logf("[ISOTP] 打开接收通道失败 M%u←M%u：%s", static_cast<unsigned>(pid),
                 static_cast<unsigned>(machine_id), std::strerror(errno));
            isotp_close(&p->txc);
            return false;
        }

        // 本端作为接收方通告的流控：BS=0（一次收完）、STmin=0（不限间隔）。
        // 桥接不是 ECU，不需要控速；真要在窄带宽总线上限速，把 STmin 调大即可 ——
        // 这才是 ISO-TP 比自研协议强的地方：发送节奏由接收方说了算。
        isotp_set_fc(&p->rxc, 0, 0);

        logf("[ISOTP] 通道就绪 M%u ⇄ M%u  tx=0x%03X rx=0x%03X",
             static_cast<unsigned>(machine_id), static_cast<unsigned>(pid), p->my_tx,
             p->peer_tx);
        chans_.push_back(std::move(p));
    }

    if (chans_.empty()) {
        logf("[ISOTP] --peers 里只有自己，没有可用对端");
        return false;
    }

    run_.store(true);
    for (auto& p : chans_) {
        p->tx_th = std::thread(&IsotpBridge::tx_loop, this, p.get());
        p->rx_th = std::thread(&IsotpBridge::rx_loop, this, p.get());
    }
    logf("[ISOTP] 就绪 %s machine=%u，对端 %zu 个", ifname.c_str(),
         static_cast<unsigned>(machine_id_), chans_.size());
    return true;
}

void IsotpBridge::stop() {
    if (!run_.exchange(false)) return;
    for (auto& p : chans_) {
        p->cv.notify_all();
    }
    for (auto& p : chans_) {
        if (p->tx_th.joinable()) p->tx_th.join();
        if (p->rx_th.joinable()) p->rx_th.join();
        isotp_close(&p->txc);
        isotp_close(&p->rxc);
        if (p->dropped) {
            logf("[ISOTP] 警告：M%u 的发送队列曾积压，丢弃 %llu 条",
                 static_cast<unsigned>(p->id), static_cast<unsigned long long>(p->dropped));
        }
    }
}

bool IsotpBridge::forward_local(uint8_t client_id, Kind kind, const std::string& body) {
    // 3 字节头 + 正文，上限 4093（不是自研后端的 255）
    if (body.size() + 3 > kMaxBodyIsotp) {
        logf("[ISOTP] 消息 %zu 字节超过上限 %zu，拒绝发送（不截断）", body.size() + 3,
             kMaxBodyIsotp);
        return false;
    }
    if (!run_.load()) return false;

    Out m;
    m.data.reserve(3 + body.size());
    m.data.push_back(static_cast<char>(machine_id_));
    m.data.push_back(static_cast<char>(client_id));
    m.data.push_back(static_cast<char>(kind));
    m.data += body;

    // 一条消息发给每一个对端：ISO-TP 没有广播，N 台机器就是 N 次发送。
    // 这是它和自研协议最本质的代价差异，别指望「发一遍全网收到」。
    for (auto& p : chans_) {
        bool dropped = false;
        {
            std::lock_guard<std::mutex> lk(p->mu);
            if (p->q.size() >= 1024) {
                p->dropped++;
                dropped = true;
            } else {
                p->q.push_back(m);
            }
        }
        if (dropped) {
            logf("[ISOTP] M%u 队列积压 1024 条，丢弃本次（对端可能掉电）",
                 static_cast<unsigned>(p->id));
            continue;
        }
        p->cv.notify_one();
    }
    return true;
}

void IsotpBridge::tx_loop(Peer* p) {
    while (run_.load()) {
        Out m;
        {
            std::unique_lock<std::mutex> lk(p->mu);
            p->cv.wait(lk, [this, p] { return !p->q.empty() || !run_.load(); });
            if (!run_.load()) return;
            m = std::move(p->q.front());
            p->q.pop_front();
        }

        // 一条消息一次调用：FF/CF/等 FC/按 BS 分组全在 isotp_send 里。
        // 注意它是阻塞的 —— 对端掉电时这里会卡满 N_BS(1s)，但只卡这一个
        // 对端的线程，其它对端照发。这就是「每对端一个 tx 线程」的原因。
        if (isotp_send(&p->txc, m.data.data(), m.data.size()) < 0) {
            err_tx_++;
            const int e = errno;
            if (e == ETIMEDOUT) err_timeout_++;
            const double t = now_s();
            if (t - last_err_log_ > 1.0) {
                last_err_log_ = t;
                logf("[ISOTP] M%u 发送失败 errno=%d (%s)，本条消息放弃",
                     static_cast<unsigned>(p->id), e, std::strerror(e));
            }
            continue;
        }

        const unsigned long delta = p->txc.n_tx_frames - p->seen_tx_frames;
        p->seen_tx_frames = p->txc.n_tx_frames;
        tx_frames_ += delta;
        tx_msgs_++;

        // 排空 txc 里对端发来的数据帧。它们已经由 rxc 那份独立副本收过了，
        // 不扔的话 txc 的接收缓冲区会一点点被填满，最后开始丢 FC。
        drain_tx(p);
    }
}

void IsotpBridge::drain_tx(Peer* p) {
    struct can_frame f {};
    for (;;) {
        ssize_t n = ::recv(p->txc.fd, &f, sizeof(f), MSG_DONTWAIT);
        if (n <= 0) break;
    }
}

void IsotpBridge::rx_loop(Peer* p) {
    std::vector<uint8_t> buf(ISOTP_MAX_MSG);

    while (run_.load()) {
        ssize_t n = isotp_recv(&p->rxc, buf.data(), buf.size(), 200);

        if (n < 0) {
            const int e = errno;
            if (e == ETIMEDOUT || e == EINTR || e == EAGAIN) continue;

            // 接口 down / 控制器掉线：socket 仍然有效，等它回来，绝不能退出线程
            if (e == ENETDOWN || e == ENXIO || e == ENOBUFS) {
                const double t = now_s();
                if (t - last_rx_err_log_ > 1.0) {
                    last_rx_err_log_ = t;
                    logf("[ISOTP] 接口暂不可用 errno=%d (%s)，等待恢复（socket 保留）", e,
                         std::strerror(e));
                }
                usleep(100 * 1000);
                continue;
            }
            if (e == EPROTO) {   // CF 序号错乱，整条已丢
                err_seq_++;
                logf("[ISOTP] M%u 序号错乱，整条消息丢弃（对端会以 ETIMEDOUT 收场）",
                     static_cast<unsigned>(p->id));
                continue;
            }
            if (e == EMSGSIZE) {   // 对端声明的长度不合法
                err_orphan_++;
                continue;
            }
            logf("[ISOTP] M%u 接收错误：%s", static_cast<unsigned>(p->id), std::strerror(e));
            usleep(10 * 1000);
            continue;
        }

        // 3 字节头：[src_machine][src_client][kind] + body
        if (n < 3) {
            err_orphan_++;
            continue;
        }
        const uint8_t src_machine = buf[0];
        const uint8_t src_client = buf[1];
        const auto kind = static_cast<Kind>(buf[2]);

        if (src_machine == machine_id_) {   // 自己发的又回来了，兜底丢弃
            err_echo_++;
            continue;
        }

        std::string body(reinterpret_cast<const char*>(buf.data() + 3),
                         static_cast<size_t>(n) - 3);
        rx_msgs_++;
        if (handler_) handler_(src_machine, kind, src_client, body);
    }
}

}  // namespace cb
