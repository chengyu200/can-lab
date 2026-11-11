#pragma once
// 09-bridge/v2/server.h —— 把「你们那个 tcp_server」包成一个类
//
// ===========================================================================
// 这个类跟桥接的接缝只有两个函数，一进一出：
//
//   出方向  CanSink   = std::function<bool(client_id, kind, body)>
//                     server 调它把「本地 client 的消息」送上 CAN。
//                     返回 false 表示桥接拒收（超长 / 队列满 / 未启动）。
//
//   入方向  from_can(machine, client_id, kind, body)
//                     桥接收到远端消息后调它。注意它是**在桥接的线程里**
//                     被调用的，不是 server 自己的线程。
//
// 为什么用 std::function 而不是让 server 持有 IBridge*：
//   1. 两个类互不包含对方的头文件，谁都不用 include 谁 —— 编译期彻底解耦，
//      将来把桥接换成 J1939 / 共享内存 / 另一个进程，server.cpp 一行不改。
//   2. 所有权清楚：桥接和 server 都由 main 持有，谁也不拥有谁，
//      没有循环引用，也没有「析构顺序错了就崩」的隐患。
//   代价是多一次 std::function 间接调用（纳秒级，相比一帧 CAN 的 200µs 可忽略）。
//
// ===========================================================================
// 线程契约（这是类化之后最容易出错的地方，比 flat 版更容易出错）
//
//   serve()            主线程，阻塞在 poll+accept
//   handle_connection  每条连接一个线程（workers_）
//   from_can()         桥接的 rx 线程（IsotpBridge 是每对端一个）
//   CanSink            任意 client 线程，同时可能有好几个在调
//
// 所以这个类的所有 public 方法都必须线程安全。三条纪律：
//
//   A. 绝不持 mu_ 做 socket 写。锁只用来拷一份 client 列表，写完就放。
//   B. 绝不持 mu_ 调 can_sink_。桥接那边可能阻塞（等 FC 最长 1 秒），
//      一旦持锁调用，整个 server 就被一条坏链路冻住。
//   C. from_can() 里绝不回调 can_sink_。那是防环的第二道闸：CAN 来的消息
//      只做本地广播，再回写 CAN 就是无限放大。
//
// 关闭顺序（main 里体现）：先 bridge->stop()，再 server.stop()。
// 反了的话，server 正在析构，桥接的 rx 线程还可能调 from_can()，
// 访问已经销毁的 mutex —— 典型的「退出时偶发崩溃」。
// ===========================================================================

#include "common.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cb {

class TcpServer {
public:
    // 出方向：把本地 client 的消息送上 CAN。返回 false = 桥接没收下。
    using CanSink = std::function<bool(uint8_t client_id, Kind kind, const std::string& body)>;

    // machine_id 是本机在 CAN 总线上的编号，同时也是本地广播前缀 [M1/A] 的来源。
    // 它属于「部署配置」，所以从构造函数进来，而不是从全局变量里偷。
    TcpServer(uint16_t port, uint8_t machine_id);
    ~TcpServer();

    TcpServer(const TcpServer&) = delete;
    TcpServer& operator=(const TcpServer&) = delete;

    // 注入出方向。必须在 start() 之前调用；不设也能跑（纯本地转发，等价 --no-can）。
    void set_can_sink(CanSink sink);

    bool start();     // socket / bind / listen
    void serve();     // 阻塞的 accept 循环，直到 request_stop() 或 stop()
    void stop();      // 关监听、踢掉所有 client、join 所有连接线程。幂等。

    // 只做一次原子写，可以从信号处理函数里调（这是它和 stop() 的区别）。
    // 注意它只负责让 serve() 的循环退出，不做任何清理 —— 清理在 stop() 里。
    void request_stop() noexcept { run_.store(false); }

    // 入方向：桥接收到远端消息后调这里。线程安全。
    void from_can(uint8_t machine, uint8_t client_id, Kind kind, const std::string& body);

    struct Stats {
        uint64_t to_can = 0;          // 成功交给桥接的条数
        uint64_t to_can_dropped = 0;  // 桥接拒收的条数
        uint64_t from_can = 0;        // 从桥接收到并广播出去的条数
        size_t clients = 0;           // 当前在线 client 数
    };
    Stats stats() const;

private:
    struct Client {
        int fd = -1;
        uint8_t id = 0;
        std::string name;
        std::mutex wmu;                 // 串行化对这个 fd 的写（多个线程都可能写）
        std::atomic<bool> dead{false};  // 清理只做一次
    };
    using ClientPtr = std::shared_ptr<Client>;

    static uint16_t key_of(uint8_t machine, uint8_t cid) {
        return static_cast<uint16_t>((machine << 8) | cid);
    }

    void handle_connection(int fd, std::string peer_ip);
    void reader_loop(const ClientPtr& c);
    void handle_local(const ClientPtr& c, const std::string& body);
    void drop_client(const ClientPtr& c, const char* why);
    void broadcast_local(uint8_t machine, uint8_t cid, const std::string& body, int except_fd);
    bool send_to(const ClientPtr& c, const std::string& msg);
    std::string label_of(uint8_t machine, uint8_t cid) const;

    const uint16_t port_;
    const uint8_t machine_;
    int listen_fd_ = -1;
    std::atomic<bool> run_{false};
    // stop() 的幂等标志，必须和 run_ 分开：
    // request_stop()（信号）会先把 run_ 置 false，如果 stop() 拿 run_ 当
    // 「是不是已经停过」判据，就会以为「已经停了」直接返回，
    // 于是监听 fd 没关、连接线程没 join —— 析构时 terminate。
    std::atomic<bool> stopped_{false};
    CanSink can_sink_;

    mutable std::mutex mu_;                  // 保护下面三张表
    std::map<int, ClientPtr> clients_;       // fd -> client
    std::map<uint16_t, std::string> names_;  // (machine<<8|cid) -> 名字
    uint8_t next_id_ = 1;

    std::mutex workers_mu_;
    std::vector<std::thread> workers_;

    std::atomic<uint64_t> to_can_{0}, to_can_dropped_{0}, from_can_{0};
};

}  // namespace cb
