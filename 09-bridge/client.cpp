// 09-bridge/client.cpp
//
// 模拟你们机器上的 client A/B/C：连上 tcp_server，注册一个名字，之后
//   - 从 stdin 读到一行就发一行
//   - 收到什么都打出来
// 演示脚本靠「管道喂 stdin + --hold 让进程多活一会儿」来控制时序。
//
// 用法: client <host> <port> <name> [--hold 秒]

#include "common.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <netdb.h>
#include <string>
#include <thread>

using namespace cb;

// 桥接演示里会出现几百字节的多帧消息（ISO-TP 后端能到 4KB），
// 整条打进日志会把时间线冲得看不见，这里只留头 48 字节 + 总长。
static std::string brief(const std::string& s) {
    if (s.size() <= 48) return s;
    return s.substr(0, 48) + "…(共 " + std::to_string(s.size()) + " 字节)";
}

static int connect_to(const char* host, int port) {
    struct addrinfo hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    char portstr[16];
    std::snprintf(portstr, sizeof(portstr), "%d", port);

    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;
    int fd = -1;
    for (struct addrinfo* p = res; p; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "用法: %s <host> <port> <name> [--hold 秒]\n", argv[0]);
        return 2;
    }
    const char* host = argv[1];
    const int port = std::atoi(argv[2]);
    const std::string name = argv[3];
    double hold = 2.0;
    for (int i = 4; i < argc; ++i) {
        if (std::string(argv[i]) == "--hold" && i + 1 < argc) hold = std::atof(argv[++i]);
    }

    set_tag(name);
    std::signal(SIGPIPE, SIG_IGN);

    int fd = connect_to(host, port);
    if (fd < 0) {
        logf("连不上 %s:%d", host, port);
        return 1;
    }
    if (!send_msg(fd, "HELLO " + name)) {
        logf("注册失败");
        ::close(fd);
        return 1;
    }

    std::string ack;
    if (recv_msg(fd, ack)) logf("已注册 %s", ack.c_str());

    std::atomic<bool> run{true};
    std::thread reader([&] {
        std::string s;
        while (recv_msg(fd, s)) logf("收到 %s", brief(s).c_str());
        logf("连接已断开");
        run.store(false);
    });

    std::string line;
    while (run.load() && std::getline(std::cin, line)) {
        if (line.empty()) continue;
        if (!send_msg(fd, line)) break;
        logf("发送 %s", brief(line).c_str());
    }

    // 发完了也别马上退，留时间把对端回来的消息打印出来
    for (int i = 0; i < static_cast<int>(hold * 10) && run.load(); ++i) usleep(100 * 1000);

    ::shutdown(fd, SHUT_RDWR);
    reader.join();
    ::close(fd);
    return 0;
}
