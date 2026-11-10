// 09-bridge/tcp_server.cpp
//
// ============================================================================
// 这是「你们现在那个 tcp_server」的最小模拟版，加上 CAN 互联的增强。
//
// 原有逻辑只有三件事：
//   1. accept 连接、读第一帧当注册名、分配序号
//   2. 某个 client 发来消息 -> 广播给本机其它 client
//   3. 断连清理
//
// 增强只有 4 处，全部用 [BRIDGE-n] 标出，加起来不到 60 行。
// 你们的 client 一行都不用改 —— 因为消息格式仍然是一行文本，只是多了个
// 来源前缀 [M1/A]，客户端即使不认识也顶多显示得丑一点。
// ============================================================================

#include "can_bridge.h"
#include "can_bridge_isotp.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <netinet/in.h>
#include <sys/socket.h>

using namespace cb;

struct Client {
    int fd = -1;
    uint8_t id = 0;
    std::string name;
};

static std::mutex g_mu;
static std::map<int, Client> g_clients;                 // fd -> Client
static std::map<uint16_t, std::string> g_peer_name;     // (machine<<8|cid) -> 名字
static uint8_t g_next_id = 1;
static uint8_t g_machine = 1;
static int g_listen = -1;
static std::atomic<bool> g_run{true};

static std::unique_ptr<IBridge> g_bridge;   // 全局唯一：一个进程只允许有一个 CAN 写者

static uint16_t key_of(uint8_t machine, uint8_t cid) {
    return static_cast<uint16_t>((machine << 8) | cid);
}

// 显示成 [M1/A]。远端 client 的名字靠 kJoin 通知补上，还没收到就退化显示 c3。
static std::string label_of(uint8_t machine, uint8_t cid) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_peer_name.find(key_of(machine, cid));
    std::string who = (it != g_peer_name.end()) ? it->second : ("c" + std::to_string(cid));
    return "M" + std::to_string(machine) + "/" + who;
}

static void drop_client(int fd, const char* why);

// ---------------------------------------------------------------------------
// 原有逻辑：把一条消息发给本机所有 client（except_fd 用于排除发送者自己）
// ---------------------------------------------------------------------------
static void broadcast_local(uint8_t machine, uint8_t cid, const std::string& body,
                            int except_fd) {
    const std::string line = "[" + label_of(machine, cid) + "] " + body;

    std::vector<int> dead;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (auto& kv : g_clients) {
            if (kv.first == except_fd) continue;
            if (!send_msg(kv.first, line)) dead.push_back(kv.first);
        }
    }
    for (int fd : dead) drop_client(fd, "写入失败");
}

static void drop_client(int fd, const char* why) {
    Client c{};
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_clients.find(fd);
        if (it == g_clients.end()) return;
        c = it->second;
        g_clients.erase(it);
    }
    ::close(fd);
    logf("[SRV] %s (id=%u) 断开：%s", c.name.c_str(), static_cast<unsigned>(c.id), why);
    if (!g_run.load()) return;

    // 先把「离开」发出去，再从名字表里删 —— 顺序反了本地就会显示成 [M1/c2]
    broadcast_local(g_machine, c.id, "*** " + c.name + " 离开了", -1);
    g_bridge->forward_local(c.id, kLeave, c.name);   // 让别的机器也知道
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_peer_name.erase(key_of(g_machine, c.id));
    }
}

// ---------------------------------------------------------------------------
// 入口 A：本地某个 client 发来一条消息
// ---------------------------------------------------------------------------
static void on_local_message(const Client& c, const std::string& body) {
    // ① 原有行为不变：本机其它 client 立刻收到
    broadcast_local(g_machine, c.id, body, c.fd);

    // ② [BRIDGE-2] 新增这一行：再送一份上 CAN
    //    注意只对「本地来源」调用；远端来源的消息绝不能走到这里，否则就是环。
    g_bridge->forward_local(c.id, kData, body);
}

// ---------------------------------------------------------------------------
// 入口 B：CAN 收到别的机器的消息（在 CanBridge 的收线程里被调用）
// ---------------------------------------------------------------------------
static void on_can_message(uint8_t machine, Kind k, uint8_t cid, const std::string& body) {
    switch (k) {
        case kJoin:
            {
                std::lock_guard<std::mutex> lk(g_mu);
                g_peer_name[key_of(machine, cid)] = body;
            }
            broadcast_local(machine, cid, "*** " + body + " 上线了", -1);
            break;

        case kLeave:
            broadcast_local(machine, cid, "*** " + body + " 离开了", -1);
            {
                std::lock_guard<std::mutex> lk(g_mu);
                g_peer_name.erase(key_of(machine, cid));
            }
            break;

        case kData:
            // ③ [BRIDGE-3] 只做本地广播。
            //    这里绝对不能再调 forward_local —— 这是防环的第二道闸。
            broadcast_local(machine, cid, body, -1);
            break;
    }
}

static void handle_client(int fd, std::string peer_ip) {
    std::string hello;
    if (!recv_msg(fd, hello)) {
        ::close(fd);
        return;
    }
    std::string name = (hello.rfind("HELLO ", 0) == 0) ? hello.substr(6) : hello;

    Client c;
    c.fd = fd;
    c.name = name;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        c.id = g_next_id++;
        if (g_next_id == 0) g_next_id = 1;          // 0 留作「未知」
        g_clients[fd] = c;
        g_peer_name[key_of(g_machine, c.id)] = name;
    }
    logf("[SRV] %s 注册成功 id=%u (%s)", name.c_str(), static_cast<unsigned>(c.id),
         peer_ip.c_str());
    send_msg(fd, "OK " + std::to_string(c.id));

    // [BRIDGE-2] 上线通知也要跨机 —— 不然对端只知道编号不知道是谁
    g_bridge->forward_local(c.id, kJoin, name);
    broadcast_local(g_machine, c.id, "*** " + name + " 上线了", fd);

    while (g_run.load()) {
        std::string body;
        if (!recv_msg(fd, body)) break;
        on_local_message(c, body);
    }
    drop_client(fd, "对端关闭");
}

static void on_signal(int) {
    g_run.store(false);
    if (g_listen >= 0) {
        ::shutdown(g_listen, SHUT_RDWR);
        ::close(g_listen);
        g_listen = -1;
    }
}

int main(int argc, char** argv) {
    // 用法: tcp_server <port> <machine_id> <can_ifname> [--no-can]
    //                    [--isotp] [--peers 1,2]
    if (argc < 4) {
        std::fprintf(stderr,
                     "用法: %s <port> <machine_id> <can_ifname> [--no-can]\n"
                     "        [--isotp --peers 1,2]\n"
                     "  machine_id 1..254，两台机器必须不同\n"
                     "  --isotp      改用 ISO-TP 分片（阶段 5 的用户态实现），需配 --peers\n"
                     "  --peers      总线上其它机器号，逗号分隔，不含自己\n",
                     argv[0]);
        return 2;
    }
    const int port = std::atoi(argv[1]);
    g_machine = static_cast<uint8_t>(std::atoi(argv[2]));
    const std::string ifname = argv[3];
    bool use_can = true;
    bool isotp_mode = false;
    std::vector<uint8_t> peers;
    for (int i = 4; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--no-can") {
            use_can = false;
        } else if (a == "--isotp") {
            isotp_mode = true;
        } else if (a == "--peers" && i + 1 < argc) {
            std::string s = argv[++i];
            size_t pos = 0;
            while (pos <= s.size()) {
                size_t comma = s.find(',', pos);
                if (comma == std::string::npos) comma = s.size();
                const std::string tok = s.substr(pos, comma - pos);
                if (!tok.empty()) peers.push_back(static_cast<uint8_t>(std::atoi(tok.c_str())));
                pos = comma + 1;
            }
        }
    }

    char tg[32];
    std::snprintf(tg, sizeof(tg), "M%u/srv", static_cast<unsigned>(g_machine));
    set_tag(tg);

    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    // ======================= [BRIDGE-1] 启动桥接 =======================
    // 位置：main 里，listen 之前（越早越好，否则刚连上的 client 消息发不出去）
    // 顺序很讲究：先 set_handler 再 start，避免已收到帧却没人处理。
    //
    // 这里多了一步「选后端」。除了下面这段选型，[BRIDGE-2/3/4] 三个调用点
    // 一个字都不用改 —— 两种分片方式对上层是同一个接口。
    if (use_can) {
        if (isotp_mode) {
            g_bridge = std::make_unique<IsotpBridge>(std::move(peers));
        } else {
            g_bridge = std::make_unique<CanBridge>();
        }
        g_bridge->set_handler(on_can_message);
        if (!g_bridge->start(ifname, g_machine)) return 1;
    } else {
        logf("[SRV] --no-can：只跑本地转发，作为对照");
    }
    // ===================================================================

    g_listen = ::socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen < 0) {
        std::perror("socket");
        return 1;
    }
    int one = 1;
    setsockopt(g_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(g_listen, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        return 1;
    }
    if (listen(g_listen, 16) < 0) {
        std::perror("listen");
        return 1;
    }
    logf("[SRV] listening :%d", port);

    while (g_run.load()) {
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        int fd = ::accept(g_listen, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (fd < 0) {
            if (!g_run.load()) break;
            continue;
        }

        // 慢客户端不能拖住整个广播：给个写超时，写不出去就踢掉
        struct timeval tv{};
        tv.tv_sec = 0;
        tv.tv_usec = 100 * 1000;
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        std::string ips(ip);
        std::thread(handle_client, fd, ips).detach();
    }

    // ======================= [BRIDGE-4] 收尾 =======================
    if (g_bridge) {
        g_bridge->stop();
        logf("[CAN] 统计：发 %llu 条/%llu 帧，收 %llu 条；错误 发送%llu 回环%llu 孤片%llu "
             "错序%llu 超时%llu",
             static_cast<unsigned long long>(g_bridge->tx_msgs()),
             static_cast<unsigned long long>(g_bridge->tx_frames()),
             static_cast<unsigned long long>(g_bridge->rx_msgs()),
             static_cast<unsigned long long>(g_bridge->err_tx()),
             static_cast<unsigned long long>(g_bridge->err_echo()),
             static_cast<unsigned long long>(g_bridge->err_orphan()),
             static_cast<unsigned long long>(g_bridge->err_seq()),
             static_cast<unsigned long long>(g_bridge->err_timeout()));
    }
    return 0;
}
