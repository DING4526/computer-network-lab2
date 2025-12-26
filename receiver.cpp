#include "rdt.h"
#include <map>
#include <vector>
#include <random>

static const int OOO_MAX_SEGMENTS = 128; // ooo缓冲最大段数限制

struct SegmentBuf {
    std::vector<uint8_t> data;
};

static void die(const char* msg) {
    printf("ERROR: %s (WSA=%d)\n", msg, WSAGetLastError());
    exit(1);
}

static void set_nonblocking(SOCKET s) {
    u_long mode = 1;
    if (ioctlsocket(s, FIONBIO, &mode) != 0) die("ioctlsocket nonblocking failed");
}

static double rnd01() {
    static std::mt19937 rng((unsigned)time(nullptr) + 12345);
    static std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(rng);
}

// 生成随机延时（5-10ms）
static int random_delay_ms() {
    return RDT_DELAY_MIN_MS + (int)(rnd01() * (RDT_DELAY_MAX_MS - RDT_DELAY_MIN_MS + 1));
}

static int send_pkt_lossy(SOCKET s, const sockaddr_in& peer, RdtHeader h, const uint8_t* payload, double loss_rate) {
    // 模拟丢包（ACK也可能丢失，实现双向丢包）
    if (loss_rate > 0.0 && rnd01() < loss_rate) {
        return int(sizeof(RdtHeader) + h.len); // pretend sent
    }
    // 模拟网络延时
    Sleep(random_delay_ms());
    
    // checksum在host序算，最后整体转network序发送
    fill_checksum(h, payload);
    RdtHeader net = h;
    hton_header(net);

    uint8_t buf[RDT_MAX_PKT];
    memcpy(buf, &net, sizeof(RdtHeader));
    if (h.len > 0 && payload) memcpy(buf + sizeof(RdtHeader), payload, h.len);

    int n = sendto(s, (const char*)buf, int(sizeof(RdtHeader) + h.len), 0,
                   (const sockaddr*)&peer, sizeof(peer));
    return n;
}

// 计算SACK位图：对 expected_ack 后面的 若干 个分片（扩展到64位）
static uint64_t build_sack_mask(uint32_t expected_ack, int fixed_wnd,
                               const std::map<uint32_t, SegmentBuf>& ooo) {
    uint64_t mask = 0;
    // 位 i 表示 expected_ack + (i+1)*MSS 处的分片起点是否已收到
    int bits_to_check = std::min(RDT_SACK_BITS, fixed_wnd);
    for (int i = 0; i < bits_to_check; i++) {
        uint32_t seq = expected_ack + uint32_t((i + 1) * RDT_MSS);
        if (ooo.find(seq) != ooo.end()) {
            mask |= (1ull << i);
        }
    }
    return mask;
}

int main(int argc, char** argv) {
    if (argc < 6) {
        printf("Usage: receiver.exe <bind_ip> <bind_port> <output_file> <fixed_wnd_segments> <loss_rate>\n");
        return 0;
    }
    std::string bind_ip = argv[1];
    int bind_port = atoi(argv[2]);
    std::string out_file = argv[3];
    int fixed_wnd = atoi(argv[4]);
    double loss_rate = atof(argv[5]);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) die("WSAStartup");

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) die("socket");

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)bind_port);
    addr.sin_addr.s_addr = inet_addr(bind_ip.c_str());
    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) != 0) die("bind");

    set_nonblocking(sock);

    FILE* fp = fopen(out_file.c_str(), "wb");
    if (!fp) {
        printf("ERROR: cannot open output file\n");
        return 1;
    }

    printf("Receiver listening on %s:%d, output=%s, fixedWnd=%d, lossRate=%.2f\n",
           bind_ip.c_str(), bind_port, out_file.c_str(), fixed_wnd, loss_rate);

    // ====== 连接状态 ======
    enum { R_CLOSED, R_SYN_RCVD, R_EST, R_FIN_WAIT } state = R_CLOSED;
    sockaddr_in peer{};
    int peer_len = sizeof(peer);

    uint32_t isn_recv = 1000 + (uint32_t)(now_ms() & 0xFFFF);
    uint32_t sender_isn = 0;
    uint32_t expected_ack = 0; // 下一个期望字节序号（累计ACK）

    std::map<uint32_t, SegmentBuf> ooo; // out-of-order buffer keyed by seq

    uint64_t start_ms = 0;
    uint64_t last_ack_sent_ms = 0;

    while (true) {
        uint8_t buf[RDT_MAX_PKT];
        sockaddr_in from{};
        int fromlen = sizeof(from);
        int n = recvfrom(sock, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fromlen);

        if (n >= (int)sizeof(RdtHeader)) {
            RdtHeader h{};
            memcpy(&h, buf, sizeof(RdtHeader));
            ntoh_header(h);

            uint8_t* payload = buf + sizeof(RdtHeader);
            if (h.len > 0 && (int)(sizeof(RdtHeader) + h.len) > n) {
                continue; // malformed
            }
            if (!verify_checksum(h, payload)) {
                continue; // checksum fail => drop
            }

            // 只接受一个对端（实验用）
            if (state == R_CLOSED) {
                // 期待SYN
                if (h.flags & F_SYN) {
                    peer = from;
                    sender_isn = h.seq;
                    expected_ack = sender_isn + 1;
                    state = R_SYN_RCVD;

                    RdtHeader synack{};
                    synack.seq = isn_recv;
                    synack.ack = expected_ack;
                    synack.flags = F_SYN | F_ACK;
                    synack.wnd = (uint16_t)fixed_wnd;
                    synack.len = 0;
                    synack.sack_mask = 0;
                    send_pkt_lossy(sock, peer, synack, nullptr, loss_rate);
                    continue;
                }
            } else {
                // 已有peer，非peer来的包忽略
                if (from.sin_addr.s_addr != peer.sin_addr.s_addr || from.sin_port != peer.sin_port) {
                    continue;
                }
            }

            if (state == R_SYN_RCVD) {
                if ((h.flags & F_ACK) && h.ack == (isn_recv + 1)) {
                    state = R_EST;
                    start_ms = now_ms();
                    printf("Connection established.\n");
                    continue;
                }
            }

            if (state == R_EST) {
                if (h.flags & F_FIN) {
                    // 对端请求关闭
                    RdtHeader ack{};
                    ack.seq = isn_recv + 1;
                    ack.ack = h.seq + 1;
                    ack.flags = F_ACK;
                    ack.wnd = (uint16_t)fixed_wnd;
                    ack.len = 0;
                    ack.sack_mask = 0;
                    send_pkt_lossy(sock, peer, ack, nullptr, loss_rate);

                    // 发送自己的FIN
                    RdtHeader fin{};
                    fin.seq = isn_recv + 2;
                    fin.ack = expected_ack;
                    fin.flags = F_FIN | F_ACK;
                    fin.wnd = (uint16_t)fixed_wnd;
                    fin.len = 0;
                    fin.sack_mask = 0;
                    send_pkt_lossy(sock, peer, fin, nullptr, loss_rate);
                    state = R_FIN_WAIT;
                    continue;
                }

                if (h.flags & F_DATA) {
                    // 数据段：seq按字节计，长度h.len
                    if (h.seq == expected_ack) {
                        // 正好是期望段：写入并推进expected_ack
                        fwrite(payload, 1, h.len, fp);
                        expected_ack += h.len;

                        // 看看缓存里是否有连续的后续段
                        while (true) {
                            auto it = ooo.find(expected_ack);
                            if (it == ooo.end()) break;
                            fwrite(it->second.data.data(), 1, it->second.data.size(), fp);
                            expected_ack += (uint32_t)it->second.data.size();
                            ooo.erase(it);
                        }
                    } else if (h.seq > expected_ack) {
                        // 乱序：若在接收窗口范围内则缓存（按fixed_wnd*MSS限制）
                        uint32_t max_seq = expected_ack + (uint32_t)(fixed_wnd * RDT_MSS);
                        if (h.seq < max_seq && (int)ooo.size() < OOO_MAX_SEGMENTS) {
                            if (ooo.find(h.seq) == ooo.end()) {
                                SegmentBuf sb;
                                sb.data.assign(payload, payload + h.len);
                                ooo[h.seq] = std::move(sb);
                            }
                        }
                    } else {
                        // h.seq < expected_ack：重复段，忽略（但可以回ACK）
                    }

                    // 回 ACK + SACK
                    RdtHeader ack{};
                    ack.seq = isn_recv + 1;
                    ack.ack = expected_ack;
                    ack.flags = F_ACK;
                    ack.wnd = (uint16_t)fixed_wnd;
                    ack.len = 0;
                    ack.sack_mask = build_sack_mask(expected_ack, fixed_wnd, ooo);
                    send_pkt_lossy(sock, peer, ack, nullptr, loss_rate);
                    last_ack_sent_ms = now_ms();
                }
            } else if (state == R_FIN_WAIT) {
                if ((h.flags & F_ACK)) {
                    // 收到对端对我FIN的ACK就可以结束
                    uint64_t end_ms = now_ms();
                    printf("Connection closed. Receive time = %.3f s\n", (end_ms - start_ms) / 1000.0);
                    break;
                }
            }
        }

        // 简单让CPU不满载（也可用select）
        Sleep(1);
    }

    fclose(fp);
    closesocket(sock);
    WSACleanup();
    return 0;
}
