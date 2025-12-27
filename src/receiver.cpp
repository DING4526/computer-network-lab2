#include "rdt.h"
#include <map>
#include <vector>

struct SegmentBuf {
    std::vector<uint8_t> data;
};

// Build SACK bitmap for segments after expected_ack
static uint64_t build_sack_mask(uint32_t expected_ack,
                               const std::map<uint32_t, SegmentBuf>& ooo) {
    uint64_t mask = 0;
    for (int i = 0; i < RDT_SACK_BITS; i++) {
        uint32_t seq = expected_ack + uint32_t((i + 1) * RDT_MSS);
        if (ooo.find(seq) != ooo.end()) mask |= (1ull << i);
    }
    return mask;
}

int main(int argc, char** argv) {
    if (argc < 5) {
        std::printf("Usage: receiver.exe <bind_ip> <bind_port> <output_file> <fixed_wnd_segments>\n");
        return 0;
    }
    std::string bind_ip  = argv[1];
    int bind_port        = std::atoi(argv[2]);
    std::string out_file = argv[3];
    int fixed_wnd        = std::atoi(argv[4]);

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

    FILE* fp = std::fopen(out_file.c_str(), "wb");
    if (!fp) die("cannot open output file");

    LOG("Receiver listening on %s:%d, output=%s, fixedWnd=%d",
        bind_ip.c_str(), bind_port, out_file.c_str(), fixed_wnd);

    enum { R_CLOSED, R_SYN_RCVD, R_EST, R_FIN_WAIT } state = R_CLOSED;

    sockaddr_in peer{};
    uint32_t isn_recv    = 1000u + (uint32_t)(now_ms() & 0xFFFF);
    uint32_t sender_isn  = 0;
    uint32_t expected_ack = 0;

    std::map<uint32_t, SegmentBuf> ooo;
    uint64_t start_ms = 0;

    while (true) {
        uint8_t buf[RDT_MAX_PKT];
        sockaddr_in from{};
        int fromlen = sizeof(from);
        int n = recvfrom(sock, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fromlen);

        if (n >= (int)sizeof(RdtHeader)) {
            RdtHeader h{};
            std::memcpy(&h, buf, sizeof(RdtHeader));
            ntoh_header(h);

            uint8_t* payload = buf + sizeof(RdtHeader);
            if (h.len > 0 && (int)(sizeof(RdtHeader) + h.len) > n) continue;
            if (!verify_checksum(h, payload)) continue;

            // only accept one peer (router will be the peer in router environment)
            if (state == R_CLOSED) {
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

                    send_pkt(sock, peer, synack, nullptr);
                    LOG("RX SYN(seq=%u) -> TX SYN|ACK(seq=%u, ack=%u)", sender_isn, isn_recv, expected_ack);
                }
                Sleep(1);
                continue;
            } else {
                if (from.sin_addr.s_addr != peer.sin_addr.s_addr || from.sin_port != peer.sin_port) {
                    Sleep(1);
                    continue;
                }
            }

            if (state == R_SYN_RCVD) {
                if ((h.flags & F_ACK) && h.ack == (isn_recv + 1)) {
                    state = R_EST;
                    start_ms = now_ms();
                    LOG("Connection established.");
                }
                Sleep(1);
                continue;
            }

            if (state == R_EST) {
                if (h.flags & F_FIN) {
                    // ACK peer FIN
                    RdtHeader ack{};
                    ack.seq = isn_recv + 1;
                    ack.ack = h.seq + 1;
                    ack.flags = F_ACK;
                    ack.wnd = (uint16_t)fixed_wnd;
                    ack.len = 0;
                    ack.sack_mask = 0;
                    send_pkt(sock, peer, ack, nullptr);
                    LOG("RX FIN(seq=%u) -> TX ACK(ack=%u)", h.seq, ack.ack);

                    // send our FIN
                    RdtHeader fin{};
                    fin.seq = isn_recv + 2;
                    fin.ack = expected_ack;
                    fin.flags = F_FIN | F_ACK;
                    fin.wnd = (uint16_t)fixed_wnd;
                    fin.len = 0;
                    fin.sack_mask = 0;
                    send_pkt(sock, peer, fin, nullptr);
                    LOG("TX FIN(seq=%u, ack=%u)", fin.seq, fin.ack);

                    state = R_FIN_WAIT;
                    Sleep(1);
                    continue;
                }

                if (h.flags & F_DATA) {
                    if (h.seq == expected_ack) {
                        std::fwrite(payload, 1, h.len, fp);
                        expected_ack += h.len;

                        while (true) {
                            auto it = ooo.find(expected_ack);
                            if (it == ooo.end()) break;
                            std::fwrite(it->second.data.data(), 1, it->second.data.size(), fp);
                            expected_ack += (uint32_t)it->second.data.size();
                            ooo.erase(it);
                        }
                    } else if (h.seq > expected_ack) {
                        uint32_t max_seq = expected_ack + (uint32_t)(fixed_wnd * RDT_MSS);
                        if (h.seq < max_seq) {
                            if (ooo.find(h.seq) == ooo.end()) {
                                SegmentBuf sb;
                                sb.data.assign(payload, payload + h.len);
                                ooo[h.seq] = std::move(sb);
                            }
                        }
                    } else {
                        // duplicate old segment; ignore payload
                    }

                    // send ACK + SACK
                    RdtHeader ack{};
                    ack.seq = isn_recv + 1;
                    ack.ack = expected_ack;
                    ack.flags = F_ACK;
                    ack.wnd = (uint16_t)fixed_wnd;
                    ack.len = 0;
                    ack.sack_mask = build_sack_mask(expected_ack, ooo);
                    send_pkt(sock, peer, ack, nullptr);

                    // logging (optional: keep concise)
                    // LOG("TX ACK(ack=%u, sack=0x%08X)", expected_ack, ack.sack_mask);
                }
            } else if (state == R_FIN_WAIT) {
                if (h.flags & F_ACK) {
                    uint64_t end_ms = now_ms();
                    LOG("Connection closed. Receive time = %.3f s", (end_ms - start_ms) / 1000.0);
                    break;
                }
            }
        }

        Sleep(1);
    }

    std::fclose(fp);
    closesocket(sock);
    WSACleanup();
    return 0;
}
