#include "server.h"

#include <cerrno>
#include <cstring>

#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace cb {

TcpServer::TcpServer(uint16_t port, uint8_t machine_id) : port_(port), machine_(machine_id) {}

TcpServer::~TcpServer() { stop(); }

void TcpServer::set_can_sink(CanSink sink) { can_sink_ = std::move(sink); }

bool TcpServer::start() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("socket");
        return false;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        ::close(fd);
        return false;
    }
    if (::listen(fd, 16) < 0) {
        std::perror("listen");
        ::close(fd);
        return false;
    }

    listen_fd_ = fd;
    run_.store(true);
    logf("[SRV] M%u listening :%u", static_cast<unsigned>(machine_), static_cast<unsigned>(port_));
    return true;
}

void TcpServer::serve() {
    // 退出靠 poll 的 200ms 超时检查 run_，而不是靠信号打断 accept。
    // 好处：request_stop() 里只有一次原子写，可以放心从信号处理函数里调。
    while (run_.load()) {
        struct pollfd p {};
        p.fd = listen_fd_;
        p.events = POLLIN;
        int r = ::poll(&p, 1, 200);
        if (r < 0) {
            if (errno == EINTR) continue;
            logf("[SRV] poll 失败：%s", std::strerror(errno));
            break;
        }
        if (r == 0 || !(p.revents & POLLIN)) continue;

        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        int cfd = ::accept(listen_fd_, reinterpret_cast<sockaddr*>(&peer), &plen);
        if (cfd < 0) {
            if (errno == EINTR || !run_.load()) continue;
            logf("[SRV] accept 失败：%s", std::strerror(errno));
            continue;
        }

        // 慢客户端不能拖住广播：写不出去就 100ms 后失败，由 drop_client 收尸
        struct timeval tv{};
        tv.tv_usec = 100 * 1000;
        setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));

        std::lock_guard<std::mutex> lk(workers_mu_);
        workers_.emplace_back(&TcpServer::handle_connection, this, cfd, std::string(ip));
    }
}

void TcpServer::handle_connection(int fd, std::string peer_ip) {
    std::string hello;
    if (!recv_msg(fd, hello)) {   // 连上不说话，直接关
        ::close(fd);
        return;
    }
    const std::string name = (hello.rfind("HELLO ", 0) == 0) ? hello.substr(6) : hello;

    auto c = std::make_shared<Client>();
    c->fd = fd;
    c->name = name;
    {
        std::lock_guard<std::mutex> lk(mu_);
        c->id = next_id_++;
        if (next_id_ == 0) next_id_ = 1;   // 0 留作「未知」
        clients_[fd] = c;
        names_[key_of(machine_, c->id)] = name;
    }   // ← 锁在这里就放掉，后面的 ack / 广播 / 上 CAN 全是慢操作

    logf("[SRV] %s 注册成功 id=%u (%s)", name.c_str(), static_cast<unsigned>(c->id),
         peer_ip.c_str());
    send_to(c, "OK " + std::to_string(c->id));

    // 上线通知：先跨机再本地，顺序和 flat 版一致（对端先知道名字，本地再显示）
    if (can_sink_ && !can_sink_(c->id, kJoin, name)) to_can_dropped_++;
    broadcast_local(machine_, c->id, "*** " + name + " 上线了", fd);

    reader_loop(c);
    drop_client(c, "对端关闭");
    ::close(fd);   // fd 的生命周期归 reader 线程：清理可以别人做，close 只在这里做
}

void TcpServer::reader_loop(const ClientPtr& c) {
    while (run_.load()) {
        std::string body;
        if (!recv_msg(c->fd, body)) break;
        handle_local(c, body);
    }
}

void TcpServer::handle_local(const ClientPtr& c, const std::string& body) {
    // ① 原有行为，一个字没改
    broadcast_local(machine_, c->id, body, c->fd);

    // ② 新增的一行：再送一份上 CAN
    //    注意这里没有持 mu_ —— 桥接可能阻塞，持锁调用等于把整个 server 押上去
    if (can_sink_) {
        if (can_sink_(c->id, kData, body)) {
            to_can_++;
        } else {
            to_can_dropped_++;
            logf("[SRV] 桥接拒收 %s 的消息（%zu 字节），本地已广播", c->name.c_str(),
                 body.size());
        }
    }
}

void TcpServer::from_can(uint8_t machine, uint8_t cid, Kind k, const std::string& body) {
    if (!run_.load()) return;   // 正在关停，别再往里塞

    switch (k) {
        case kJoin:
            {
                std::lock_guard<std::mutex> lk(mu_);
                names_[key_of(machine, cid)] = body;
            }
            broadcast_local(machine, cid, "*** " + body + " 上线了", -1);
            break;

        case kLeave:
            broadcast_local(machine, cid, "*** " + body + " 离开了", -1);
            {
                std::lock_guard<std::mutex> lk(mu_);
                names_.erase(key_of(machine, cid));
            }
            break;

        case kData:
            // ③ 只做本地广播，绝不回调 can_sink_ —— 防环的第二道闸
            broadcast_local(machine, cid, body, -1);
            break;
    }
    from_can_++;
}

void TcpServer::drop_client(const ClientPtr& c, const char* why) {
    if (c->dead.exchange(true)) return;   // 别人已经清过了

    {
        std::lock_guard<std::mutex> lk(mu_);
        clients_.erase(c->fd);
    }
    logf("[SRV] %s (id=%u) 断开：%s", c->name.c_str(), static_cast<unsigned>(c->id), why);
    if (!run_.load()) return;

    // 先把「离开」发出去，再从名字表里删 —— 顺序反了本地就会显示成 [M1/c2]
    broadcast_local(machine_, c->id, "*** " + c->name + " 离开了", -1);
    if (can_sink_) can_sink_(c->id, kLeave, c->name);
    {
        std::lock_guard<std::mutex> lk(mu_);
        names_.erase(key_of(machine_, c->id));
    }

    // 不在这里 close：可能是广播线程发现写失败来收尸的，
    // 而 reader 线程还阻塞在这个 fd 的 recv 上。shutdown 把它叫醒即可。
    ::shutdown(c->fd, SHUT_RDWR);
}

void TcpServer::broadcast_local(uint8_t machine, uint8_t cid, const std::string& body,
                                int except_fd) {
    const std::string line = "[" + label_of(machine, cid) + "] " + body;

    // 纪律 A：锁只用来拷一份列表。写 socket 一律在锁外做，
    // 否则一个卡住的客户端会让整个 server 连新连接都接不进来。
    std::vector<ClientPtr> targets;
    {
        std::lock_guard<std::mutex> lk(mu_);
        targets.reserve(clients_.size());
        for (auto& kv : clients_) {
            if (kv.first == except_fd) continue;
            targets.push_back(kv.second);
        }
    }

    for (auto& c : targets) {
        if (!send_to(c, line)) drop_client(c, "写入失败");
    }
}

bool TcpServer::send_to(const ClientPtr& c, const std::string& msg) {
    std::lock_guard<std::mutex> lk(c->wmu);   // 每个 client 一把写锁，不同 client 互不阻塞
    if (c->dead.load()) return true;          // 已经死了，别重复报错
    return send_msg(c->fd, msg);
}

std::string TcpServer::label_of(uint8_t machine, uint8_t cid) const {
    std::string who;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = names_.find(key_of(machine, cid));
        who = (it != names_.end()) ? it->second : ("c" + std::to_string(cid));
    }
    return "M" + std::to_string(machine) + "/" + who;
}

void TcpServer::stop() {
    if (stopped_.exchange(true)) return;   // 幂等：析构时再调一次不会重复清理
    run_.store(false);

    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }

    // 叫醒所有 reader，让它们自然退出，再 join
    std::vector<ClientPtr> all;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& kv : clients_) all.push_back(kv.second);
    }
    for (auto& c : all) ::shutdown(c->fd, SHUT_RDWR);

    std::vector<std::thread> pending;
    {
        std::lock_guard<std::mutex> lk(workers_mu_);
        pending.swap(workers_);
    }
    for (auto& t : pending) {
        if (t.joinable()) t.join();
    }
}

TcpServer::Stats TcpServer::stats() const {
    Stats s;
    s.to_can = to_can_.load();
    s.to_can_dropped = to_can_dropped_.load();
    s.from_can = from_can_.load();
    std::lock_guard<std::mutex> lk(mu_);
    s.clients = clients_.size();
    return s;
}

}  // namespace cb
