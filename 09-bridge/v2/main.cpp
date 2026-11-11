// 09-bridge/v2/main.cpp —— 只做一件事：把两个对象接起来，然后管它们的生死
//
// 这个文件是这一版的重点。原来 tcp_server.cpp 里 main 既要有业务又要接线，
// 现在业务全在 TcpServer 里，main 只剩三段：
//
//   1. 按参数造对象（桥接可能是 CanBridge 也可能是 IsotpBridge，也可能不造）
//   2. 双向接线（server.set_can_sink / bridge.set_handler）
//   3. 按正确顺序启动、按正确顺序关停
//
// 第 3 步的顺序不是风格问题：桥接的 rx 线程会在任意时刻调 server.from_can()，
// 所以必须先让桥接停下来（join 掉它的线程），再拆 server。反了就是退出时偶发崩溃。

#include "can_bridge.h"
#include "can_bridge_isotp.h"
#include "server.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

using namespace cb;

// 信号处理函数里只能碰这个（一次原子写），别的什么都别做：
// 拿锁、打日志、调 stop() 都可能和正在退出的线程撞死。
static TcpServer* g_server = nullptr;
static void on_signal(int) {
    if (g_server) g_server->request_stop();
}

static void usage(const char* prog) {
    std::fprintf(stderr,
                 "用法: %s <port> <machine_id> <can_ifname> [--no-can] [--isotp --peers 1,2]\n"
                 "  machine_id 1..254，两台机器必须不同\n",
                 prog);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        usage(argv[0]);
        return 2;
    }
    const uint16_t port = static_cast<uint16_t>(std::atoi(argv[1]));
    const uint8_t machine = static_cast<uint8_t>(std::atoi(argv[2]));
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

    char tag[32];
    std::snprintf(tag, sizeof(tag), "M%u/srv", static_cast<unsigned>(machine));
    set_tag(tag);
    std::signal(SIGPIPE, SIG_IGN);

    // ---------------- 1. 造对象 ----------------
    // 桥接是「可选的」：--no-can 时干脆不造，server 的 can_sink_ 保持空，
    // 于是它退化成纯本地转发器 —— 不需要在 server 里写 if (use_can) 分支。
    std::unique_ptr<IBridge> bridge;
    if (use_can) {
        if (isotp_mode) {
            bridge = std::make_unique<IsotpBridge>(std::move(peers));
        } else {
            bridge = std::make_unique<CanBridge>();
        }
    }

    TcpServer server(port, machine);

    // ---------------- 2. 双向接线 ----------------
    // 出方向：server -> CAN
    if (bridge) {
        server.set_can_sink([&bridge](uint8_t client_id, Kind kind, const std::string& body) {
            return bridge->forward_local(client_id, kind, body);
        });
        // 入方向：CAN -> server
        // 注意这个 lambda 会在桥接的 rx 线程里被调用，所以它只能碰 server 的
        // 线程安全接口，绝不能去动 main 的局部变量。
        bridge->set_handler(
            [&server](uint8_t m, Kind kind, uint8_t cid, const std::string& body) {
                server.from_can(m, cid, kind, body);
            });
    }

    // ---------------- 3. 启动 ----------------
    // 先起桥接再起监听：反过来的话，client 一连上发的第一条消息可能没地方去。
    if (bridge && !bridge->start(ifname, machine)) {
        logf("[MAIN] 桥接启动失败，退出");
        return 1;
    }
    if (!server.start()) {
        if (bridge) bridge->stop();
        return 1;
    }

    g_server = &server;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    server.serve();   // 阻塞在这里，直到 request_stop()

    // ---------------- 4. 关停（顺序有讲究） ----------------
    // 先停桥接：它的 stop() 会 join 掉 tx/rx 线程，之后不可能再有人调 from_can()。
    // 之所以要等它 join 完，是因为 IsotpBridge 的 tx 线程可能正卡在等 FC 上，
    // 最长 1 秒 —— 这段时间 server 还活着，回调进来也是安全的。
    if (bridge) bridge->stop();
    server.stop();

    const auto ss = server.stats();
    logf("[SRV] 统计：client %zu 个，本地→CAN %llu 条（拒收 %llu），CAN→本地 %llu 条",
         ss.clients, static_cast<unsigned long long>(ss.to_can),
         static_cast<unsigned long long>(ss.to_can_dropped),
         static_cast<unsigned long long>(ss.from_can));
    if (bridge) {
        logf("[CAN] 统计：发 %llu 条/%llu 帧，收 %llu 条；错误 发送%llu 回环%llu 孤片%llu "
             "错序%llu 超时%llu",
             static_cast<unsigned long long>(bridge->tx_msgs()),
             static_cast<unsigned long long>(bridge->tx_frames()),
             static_cast<unsigned long long>(bridge->rx_msgs()),
             static_cast<unsigned long long>(bridge->err_tx()),
             static_cast<unsigned long long>(bridge->err_echo()),
             static_cast<unsigned long long>(bridge->err_orphan()),
             static_cast<unsigned long long>(bridge->err_seq()),
             static_cast<unsigned long long>(bridge->err_timeout()));
    }
    return 0;
}
