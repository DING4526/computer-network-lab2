#pragma once
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <chrono>

#pragma comment(lib, "ws2_32.lib")

// ====== 可调参数 ======
static const int    RDT_MSS = 1000;              // 每个数据段payload最大长度（字节）
static const int    RDT_SACK_BITS = 64;          // SACK位图长度（扩展至64以支持更大窗口）
static const int    RDT_MAX_PKT = 1400;          // UDP payload最大（保守）
static const int    RDT_RTO_MS = 300;            // 简化：固定RTO
static const int    RDT_HANDSHAKE_RTO_MS = 300;  // SYN/FIN重传超时
static const int    RDT_MAX_RETX = 50;           // 防止死循环
static const int    RDT_DELAY_MIN_MS = 5;        // 网络延时下限（毫秒）
static const int    RDT_DELAY_MAX_MS = 10;       // 网络延时上限（毫秒）
static const int    RDT_OOO_MAX_SEGS = 128;      // 接收端乱序缓冲最大段数

// ====== flags ======
enum : uint16_t {
    F_SYN  = 0x0001,
    F_ACK  = 0x0002,
    F_FIN  = 0x0004,
    F_DATA = 0x0008,
    F_RST  = 0x0010
};

#pragma pack(push, 1)
struct RdtHeader {
    uint32_t seq;        // 本段起始字节序号
    uint32_t ack;        // 累计确认：下一个期望字节
    uint16_t flags;      // SYN/ACK/FIN/DATA/RST
    uint16_t wnd;        // 固定窗口（分片数）
    uint16_t len;        // payload长度
    uint16_t cksum;      // checksum (header+payload)
    uint64_t sack_mask;  // 对 ack 后的 64 个分片的接收情况位图（扩展至64位）
};
#pragma pack(pop)

static inline uint64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// Internet checksum (16-bit one's complement)
static inline uint16_t checksum16(const uint8_t* data, size_t len) {
    uint32_t sum = 0;
    size_t i = 0;
    while (i + 1 < len) {
        uint16_t word = (uint16_t(data[i]) << 8) | uint16_t(data[i + 1]);
        sum += word;
        if (sum & 0x10000) sum = (sum & 0xFFFF) + 1;
        i += 2;
    }
    if (i < len) { // odd byte
        uint16_t word = uint16_t(data[i]) << 8;
        sum += word;
        if (sum & 0x10000) sum = (sum & 0xFFFF) + 1;
    }
    return uint16_t(~sum & 0xFFFF);
}

// 写入header checksum前先置0
static inline void fill_checksum(RdtHeader& h, const uint8_t* payload) {
    h.cksum = 0;
    uint8_t buf[RDT_MAX_PKT];
    size_t hdr_len = sizeof(RdtHeader);
    memcpy(buf, &h, hdr_len);
    if (h.len > 0 && payload) memcpy(buf + hdr_len, payload, h.len);
    h.cksum = checksum16(buf, hdr_len + h.len);
}

static inline bool verify_checksum(const RdtHeader& h, const uint8_t* payload) {
    uint8_t buf[RDT_MAX_PKT];
    RdtHeader tmp = h;
    tmp.cksum = 0;
    size_t hdr_len = sizeof(RdtHeader);
    memcpy(buf, &tmp, hdr_len);
    if (h.len > 0 && payload) memcpy(buf + hdr_len, payload, h.len);
    uint16_t c = checksum16(buf, hdr_len + h.len);
    return c == h.cksum;
}

// 网络字节序转换：header里的多字节字段都要hton/ntoh
static inline void hton_header(RdtHeader& h) {
    h.seq = htonl(h.seq);
    h.ack = htonl(h.ack);
    h.flags = htons(h.flags);
    h.wnd = htons(h.wnd);
    h.len = htons(h.len);
    h.cksum = htons(h.cksum);
    h.sack_mask = htonl(h.sack_mask);
}
static inline void ntoh_header(RdtHeader& h) {
    h.seq = ntohl(h.seq);
    h.ack = ntohl(h.ack);
    h.flags = ntohs(h.flags);
    h.wnd = ntohs(h.wnd);
    h.len = ntohs(h.len);
    h.cksum = ntohs(h.cksum);
    h.sack_mask = ntohl(h.sack_mask);
}
