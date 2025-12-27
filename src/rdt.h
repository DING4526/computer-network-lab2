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

// ====== Tunables ======
static constexpr int RDT_MSS               = 1000;   // payload max per segment
static constexpr int RDT_SACK_BITS         = 64;     // SACK bitmap length
static constexpr int RDT_MAX_PKT           = 1400;   // UDP payload cap (safe < 15000 of router)
static constexpr int RDT_RTO_MS            = 300;    // retransmission timeout (data)
static constexpr int RDT_HANDSHAKE_RTO_MS  = 300;    // SYN/FIN timeout
static constexpr int RDT_MAX_RETX          = 50;     // safety

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
    uint32_t seq;        // byte-seq of first byte in this segment (or ISN for SYN)
    uint32_t ack;        // cumulative ACK: next expected byte
    uint16_t flags;      // SYN/ACK/FIN/DATA/RST
    uint16_t wnd;        // fixed window size (segments)
    uint16_t len;        // payload length
    uint16_t cksum;      // checksum over header+payload
    uint64_t sack_mask;  // SACK bitmap for 64 segments after ack
};
#pragma pack(pop)

static inline uint64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static inline void LOG(const char* fmt, ...) {
    uint64_t t = now_ms();
    std::printf("[%-10llu ms] ", (unsigned long long)t);
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::printf("\n");
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

// host<->network conversions for header fields
static inline void hton_header(RdtHeader& h) {
    h.seq       = htonl(h.seq);
    h.ack       = htonl(h.ack);
    h.flags     = htons(h.flags);
    h.wnd       = htons(h.wnd);
    h.len       = htons(h.len);
    h.cksum     = htons(h.cksum);
    h.sack_mask = htonll(h.sack_mask);
}
static inline void ntoh_header(RdtHeader& h) {
    h.seq       = ntohl(h.seq);
    h.ack       = ntohl(h.ack);
    h.flags     = ntohs(h.flags);
    h.wnd       = ntohs(h.wnd);
    h.len       = ntohs(h.len);
    h.cksum     = ntohs(h.cksum);
    h.sack_mask = ntohll(h.sack_mask);
}

// checksum is computed in host-order representation consistently on both ends (same as original logic)
static inline void fill_checksum(RdtHeader& h, const uint8_t* payload) {
    h.cksum = 0;
    uint8_t buf[RDT_MAX_PKT];
    const size_t hdr_len = sizeof(RdtHeader);
    std::memcpy(buf, &h, hdr_len);
    if (h.len > 0 && payload) std::memcpy(buf + hdr_len, payload, h.len);
    h.cksum = checksum16(buf, hdr_len + h.len);
}

static inline bool verify_checksum(const RdtHeader& h, const uint8_t* payload) {
    RdtHeader tmp = h;
    tmp.cksum = 0;
    uint8_t buf[RDT_MAX_PKT];
    const size_t hdr_len = sizeof(RdtHeader);
    std::memcpy(buf, &tmp, hdr_len);
    if (h.len > 0 && payload) std::memcpy(buf + hdr_len, payload, h.len);
    uint16_t c = checksum16(buf, hdr_len + h.len);
    return c == h.cksum;
}

static inline void die(const char* msg) {
    std::printf("ERROR: %s (WSA=%d)\n", msg, WSAGetLastError());
    std::exit(1);
}

static inline void set_nonblocking(SOCKET s) {
    u_long mode = 1;
    if (ioctlsocket(s, FIONBIO, &mode) != 0) die("ioctlsocket nonblocking failed");
}

static inline int send_pkt(SOCKET s, const sockaddr_in& peer, RdtHeader h, const uint8_t* payload) {
    fill_checksum(h, payload);
    RdtHeader net = h;
    hton_header(net);

    uint8_t buf[RDT_MAX_PKT];
    std::memcpy(buf, &net, sizeof(RdtHeader));
    if (h.len > 0 && payload) std::memcpy(buf + sizeof(RdtHeader), payload, h.len);

    return sendto(
        s,
        (const char*)buf,
        int(sizeof(RdtHeader) + h.len),
        0,
        (const sockaddr*)&peer,
        sizeof(peer)
    );
}
