#pragma once
// 09-bridge/common.h —— server 与 client 共用：CAN 线上格式 + TCP 分帧 + 日志

#include <arpa/inet.h>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace cb {

// ============================================================================
// 一、CAN 线上格式（经典 CAN，数据场 8 字节）
//
//   byte0  src_machine   发送方机器号     1..254
//   byte1  src_client    发送方 client 在本机的注册序号 1..255
//   byte2  msg_seq       本条消息的流水号，用来识别「漏了首帧」的错拼
//   byte3  frag_idx      分片下标，从 0 开始
//   byte4  total_len     整条消息字节数（含首字节 kind），1..255
//   byte5  data[0..2]    3 字节负载
//
// 每个分片都自带 total_len，接收端任何一帧到手都能算出「总片数」和
// 「末帧有效字节数」，不需要「首帧特殊解析」。代价是 3/8 的载荷效率 ——
// 对控制类消息够用，要传大块数据请换 can_isotp（见 NOTES.md）。
// ============================================================================
constexpr uint32_t kCanId     = 0x500;   // 桥接专用 ID，标准帧
constexpr size_t   kFrameData = 3;       // 每帧能带的用户字节
constexpr size_t   kMaxBody   = 255;     // total_len 是 1 字节

struct WireFrame {
    uint8_t src_machine;
    uint8_t src_client;
    uint8_t msg_seq;
    uint8_t frag_idx;
    uint8_t total_len;
    uint8_t data[kFrameData];
};
static_assert(sizeof(WireFrame) == 8, "WireFrame 必须正好 8 字节");

// 消息种类：放在消息体首字节，跨机后接收端才知道该怎么处理
enum Kind : uint8_t {
    kData  = 1,   // 普通业务消息，转发给所有本地 client
    kJoin  = 2,   // 某台机器上的某个 client 上线了，body 是它的名字
    kLeave = 3,   // 下线
};

inline const char* kind_name(Kind k) {
    switch (k) {
        case kData:  return "DATA";
        case kJoin:  return "JOIN";
        case kLeave: return "LEAVE";
    }
    return "?";
}

// ============================================================================
// 二、TCP 分帧：4 字节大端长度 + 消息体
// TCP 是字节流，没有消息边界。真实项目里你们大概已经有类似约定，照旧即可。
// ============================================================================
inline bool read_all(int fd, void* p, size_t n) {
    char* q = static_cast<char*>(p);
    while (n > 0) {
        ssize_t r = ::recv(fd, q, n, 0);
        if (r == 0) return false;
        if (r < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        q += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

inline bool write_all(int fd, const void* p, size_t n) {
    const char* q = static_cast<const char*>(p);
    while (n > 0) {
        ssize_t r = ::send(fd, q, n, MSG_NOSIGNAL);
        if (r <= 0) {
            if (r < 0 && errno == EINTR) continue;
            return false;
        }
        q += r;
        n -= static_cast<size_t>(r);
    }
    return true;
}

inline bool send_msg(int fd, const std::string& body) {
    uint32_t len = htonl(static_cast<uint32_t>(body.size()));
    if (!write_all(fd, &len, 4)) return false;
    return body.empty() || write_all(fd, body.data(), body.size());
}

inline bool recv_msg(int fd, std::string& body) {
    uint32_t len = 0;
    if (!read_all(fd, &len, 4)) return false;
    len = ntohl(len);
    if (len > (1u << 20)) return false;   // 明显不是合法帧，直接断开
    body.resize(len);
    return len == 0 || read_all(fd, &body[0], len);
}

// ============================================================================
// 三、日志：绝对时间 + 进程标签，多个进程写同一个文件也不会认不出来
// ============================================================================
inline const char*& tag() {
    static const char* t = "?";
    return t;
}
inline void set_tag(const std::string& s) {
    static std::string keep;
    keep = s;
    tag() = keep.c_str();
}

inline double now_s() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

inline void logf(const char* fmt, ...) {
    struct timeval tv{};
    gettimeofday(&tv, nullptr);
    struct tm tm{};
    localtime_r(&tv.tv_sec, &tm);
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    flockfile(stdout);
    std::printf("%02d:%02d:%02d.%03d %-9s %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec,
                static_cast<int>(tv.tv_usec / 1000), tag(), buf);
    funlockfile(stdout);
    std::fflush(stdout);
}

}  // namespace cb
