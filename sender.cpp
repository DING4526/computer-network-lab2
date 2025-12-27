#include "rdt.h"
#include <map>
#include <vector>
#include <algorithm>
#include <fstream>

// ====== CWND logging for plotting ======
static std::ofstream cwnd_log_file;

static void cwnd_log_init() {
    cwnd_log_file.open("cwnd_log.csv", std::ios::out | std::ios::trunc);
    if (cwnd_log_file.is_open()) {
        cwnd_log_file << "time_ms,cwnd\n";
    }
}

static void cwnd_log_record(int cwnd_value) {
    if (cwnd_log_file.is_open()) {
        uint64_t t = now_ms();
        cwnd_log_file << t << "," << cwnd_value << "\n";
    }
}

static void cwnd_log_close() {
    if (cwnd_log_file.is_open()) {
        cwnd_log_file.close();
    }
}

static void cwnd_plot_generate() {
    // Generate the plot using Python script (safe in experiment environment)
    int ret = std::system("python plot_cwnd.py cwnd_log.csv cwnd_curve.png");
    if (ret != 0) {
        // Try python3 if python doesn't work
        ret = std::system("python3 plot_cwnd.py cwnd_log.csv cwnd_curve.png");
    }
    if (ret == 0) {
        LOG("CWND curve plot generated: cwnd_curve.png");
    } else {
        LOG("CWND curve data saved to: cwnd_log.csv (run 'python plot_cwnd.py' to generate plot)");
    }
}

struct OutSeg {
    uint32_t seq;
    uint16_t len;
    std::vector<uint8_t> data;
    bool acked = false;
    uint64_t last_sent_ms = 0;
    int retx = 0;
};

// Mark segments acked by SACK bitmap (relative to cumulative ack)
static void mark_sack_acked(uint32_t cum_ack, uint32_t sack_mask, std::map<uint32_t, OutSeg>& out) {
    for (int i = 0; i < RDT_SACK_BITS; i++) {
        if (sack_mask & (1u << i)) {
            uint32_t seq = cum_ack + uint32_t((i + 1) * RDT_MSS);
            auto it = out.find(seq);
            if (it != out.end()) it->second.acked = true;
        }
    }
}

static sockaddr_in make_addr(const std::string& ip, int port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = inet_addr(ip.c_str());
    return a;
}

int main(int argc, char** argv) {
    if (argc < 7) {
        std::printf("Usage:\n");
        std::printf("  sender.exe <client_ip> <client_port> <router_ip> <router_port> <input_file> <fixed_wnd_segments>\n");
        return 0;
    }

    std::string client_ip = argv[1];
    int client_port       = std::atoi(argv[2]);
    std::string router_ip = argv[3];
    int router_port       = std::atoi(argv[4]);
    std::string in_file   = argv[5];
    int fixed_wnd         = std::atoi(argv[6]);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) die("WSAStartup");

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) die("socket");
    set_nonblocking(sock);

    // IMPORTANT: bind client ip/port (recommended for router env)
    sockaddr_in local = make_addr(client_ip, client_port);
    if (bind(sock, (sockaddr*)&local, sizeof(local)) != 0) die("bind(client)");
    LOG("Sender bind at %s:%d", client_ip.c_str(), client_port);

    // peer is ROUTER
    sockaddr_in peer = make_addr(router_ip, router_port);
    LOG("Peer(router) = %s:%d", router_ip.c_str(), router_port);

    // Read file
    FILE* fp = std::fopen(in_file.c_str(), "rb");
    if (!fp) die("cannot open input file");

    std::fseek(fp, 0, SEEK_END);
    long fsz = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);

    std::vector<uint8_t> filedata((size_t)std::max(0L, fsz));
    if (fsz > 0) std::fread(filedata.data(), 1, (size_t)fsz, fp);
    std::fclose(fp);

    LOG("File size: %ld bytes", fsz);

    // ====== 3-way handshake ======
    uint32_t isn_send = 5000u + (uint32_t)(now_ms() & 0xFFFF);
    uint32_t peer_isn = 0;

    uint32_t base_ack  = isn_send + 1; // data starts from isn+1
    uint32_t next_seq  = base_ack;

    bool established = false;
    uint64_t syn_last = 0;
    int syn_retx = 0;

    LOG("Connecting (SYN) ...");
    while (!established) {
        uint64_t t = now_ms();
        if (t - syn_last >= (uint64_t)RDT_HANDSHAKE_RTO_MS) {
            if (syn_retx++ > RDT_MAX_RETX) die("handshake failed (too many retries)");
            RdtHeader syn{};
            syn.seq = isn_send;
            syn.ack = 0;
            syn.flags = F_SYN;
            syn.wnd = (uint16_t)fixed_wnd;
            syn.len = 0;
            syn.sack_mask = 0;
            send_pkt(sock, peer, syn, nullptr);
            syn_last = t;
            LOG("TX SYN(seq=%u) retx=%d", isn_send, syn_retx - 1);
        }

        uint8_t buf[RDT_MAX_PKT];
        sockaddr_in from{};
        int fromlen = sizeof(from);
        int n = recvfrom(sock, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fromlen);
        if (n >= (int)sizeof(RdtHeader)) {
            RdtHeader h{};
            std::memcpy(&h, buf, sizeof(RdtHeader));
            ntoh_header(h);

            uint8_t* payload = buf + sizeof(RdtHeader);
            if (!verify_checksum(h, payload)) { Sleep(1); continue; }

            if ((h.flags & (F_SYN | F_ACK)) == (F_SYN | F_ACK) && h.ack == isn_send + 1) {
                peer_isn = h.seq;

                RdtHeader ack{};
                ack.seq = isn_send + 1;
                ack.ack = peer_isn + 1;
                ack.flags = F_ACK;
                ack.wnd = (uint16_t)fixed_wnd;
                ack.len = 0;
                ack.sack_mask = 0;
                send_pkt(sock, peer, ack, nullptr);

                established = true;
                LOG("RX SYN|ACK(seq=%u, ack=%u) -> TX ACK(ack=%u). Connected.",
                    peer_isn, h.ack, ack.ack);
                break;
            }
        }
        Sleep(1);
    }

    // ====== Reno congestion control variables ======
    int cwnd = 1;                 // in segments
    int ssthresh = fixed_wnd;     // initial threshold
    int dup_ack_cnt = 0;
    uint32_t last_ack = base_ack;

    // ====== CWND logging initialization ======
    cwnd_log_init();
    cwnd_log_record(cwnd);  // Record initial cwnd value

    // ====== send buffer (sliding window) ======
    std::map<uint32_t, OutSeg> out; // key=seq
    size_t file_off = 0;

    // ====== FIN state ======
    bool fin_sent = false;
    bool fin_acked = false;
    uint64_t fin_last = 0;
    int fin_retx = 0;

    uint64_t start_ms = now_ms();

    while (true) {
        // effective window = min(fixed flow-control wnd, cwnd)
        // inflight：当前在途未确认分片数
        int inflight = 0;
        for (auto& kv : out) if (!kv.second.acked) inflight++;

        int eff_wnd = std::min(cwnd, fixed_wnd);

        // ====== Fill window with DATA ======
        while (inflight < eff_wnd && file_off < filedata.size()) {
            uint16_t chunk = (uint16_t)std::min((size_t)RDT_MSS, filedata.size() - file_off);

            OutSeg seg;
            seg.seq = next_seq;
            seg.len = chunk;
            seg.data.assign(filedata.begin() + file_off, filedata.begin() + file_off + chunk);

            RdtHeader dh{};
            dh.seq = seg.seq;
            dh.ack = 0;
            dh.flags = F_DATA;
            dh.wnd = (uint16_t)fixed_wnd;
            dh.len = seg.len;
            dh.sack_mask = 0;

            send_pkt(sock, peer, dh, seg.data.data());
            seg.last_sent_ms = now_ms();

            out[seg.seq] = std::move(seg);

            inflight++;
            file_off += chunk;
            next_seq += chunk;
        }

        // ====== Receive ACKs / FINs ======
        uint8_t buf[RDT_MAX_PKT];
        sockaddr_in from{};
        int fromlen = sizeof(from);
        int n = recvfrom(sock, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fromlen);

        if (n >= (int)sizeof(RdtHeader)) {
            RdtHeader h{};
            std::memcpy(&h, buf, sizeof(RdtHeader));
            ntoh_header(h);

            uint8_t* payload = buf + sizeof(RdtHeader);
            if (!verify_checksum(h, payload)) goto after_recv;

            // Peer FIN: ACK it and finish
            if (h.flags & F_FIN) {
                RdtHeader ack{};
                ack.seq = next_seq + 1;
                ack.ack = h.seq + 1;
                ack.flags = F_ACK;
                ack.wnd = (uint16_t)fixed_wnd;
                ack.len = 0;
                ack.sack_mask = 0;
                send_pkt(sock, peer, ack, nullptr);
                LOG("RX FIN(seq=%u) -> TX ACK(ack=%u). Done.", h.seq, ack.ack);
                break;
            }

            if (h.flags & F_ACK) {
                uint32_t ackno = h.ack;

                // 1) new cumulative ACK
                if (ackno > last_ack) {
                    dup_ack_cnt = 0;

                    // ====== Reno: Slow Start / Congestion Avoidance ======
                    if (cwnd < ssthresh) {
                        cwnd += 1; // slow start: cwnd += 1 per ACK (segment-granularity)
                        cwnd_log_record(cwnd);  // Record cwnd change
                        LOG("ACK advance to %u, slow start cwnd=%d ssthresh=%d", ackno, cwnd, ssthresh);
                    } else {
                        // congestion avoidance: cwnd += 1/cwnd per ACK (approx)
                        static double ca_acc = 0.0;
                        ca_acc += 1.0 / cwnd;
                        if (ca_acc >= 1.0) {
                            cwnd += 1;
                            ca_acc -= 1.0;
                            cwnd_log_record(cwnd);  // Record cwnd change
                        }
                        LOG("ACK advance to %u, cong avoid cwnd=%d ssthresh=%d", ackno, cwnd, ssthresh);
                    }

                    // 累计ACK：所有 (seq+len)<=ackno 的段 acked=true
                    for (auto& kv : out) {
                        auto& seg = kv.second;
                        if (!seg.acked && (seg.seq + seg.len) <= ackno) seg.acked = true;
                    }

                    // SACK 标记：mark_sack_acked(ackno, h.sack_mask)
                    mark_sack_acked(ackno, h.sack_mask, out);

                    last_ack = ackno;
                }
                // 2) dupACK
                else if (ackno == last_ack) {
                    dup_ack_cnt++;
                    if (dup_ack_cnt == 3) { // 快速重传
                        // ====== Reno: Fast Retransmit + Fast Recovery ======
                        uint32_t oldest = 0;
                        bool found = false;
                        for (auto& kv : out) {
                            if (!kv.second.acked) { oldest = kv.first; found = true; break; }
                        }
                        if (found) {
                            ssthresh = std::max(1, cwnd / 2);
                            cwnd = ssthresh + 3;
                            cwnd_log_record(cwnd);  // Record cwnd change (fast retransmit)

                            auto& seg = out[oldest];
                            RdtHeader dh{};
                            dh.seq = seg.seq;
                            dh.ack = 0;
                            dh.flags = F_DATA;
                            dh.wnd = (uint16_t)fixed_wnd;
                            dh.len = seg.len;
                            dh.sack_mask = 0;

                            send_pkt(sock, peer, dh, seg.data.data());
                            seg.last_sent_ms = now_ms();
                            seg.retx++;

                            LOG("3 dupACK -> Fast Retransmit seq=%u, cwnd=%d ssthresh=%d", seg.seq, cwnd, ssthresh);
                        }
                    } else if (dup_ack_cnt > 3) {
                        cwnd += 1; // fast recovery inflate
                        cwnd_log_record(cwnd);  // Record cwnd change (fast recovery)
                        LOG("dupACK #%d -> fast recovery cwnd=%d", dup_ack_cnt, cwnd);
                    }
                }

                // ====== Check if all data acked -> FIN ======
                bool all_acked = (file_off >= filedata.size());
                if (all_acked) {
                    for (auto& kv : out) {
                        if (!kv.second.acked) { all_acked = false; break; }
                    }
                }

                if (all_acked && !fin_sent) {
                    RdtHeader fin{};
                    fin.seq = next_seq; // FIN consumes 1 seq number
                    fin.ack = 0;
                    fin.flags = F_FIN;
                    fin.wnd = (uint16_t)fixed_wnd;
                    fin.len = 0;
                    fin.sack_mask = 0;
                    send_pkt(sock, peer, fin, nullptr);

                    fin_sent = true;
                    fin_last = now_ms();
                    LOG("TX FIN(seq=%u)", fin.seq);
                }

                if (fin_sent && (h.flags & F_ACK) && h.ack == next_seq + 1) {
                    fin_acked = true;
                    LOG("FIN ACKed (ack=%u). Waiting peer FIN...", h.ack);
                }
            }
        }

    after_recv:
        uint64_t t = now_ms();

        // ====== Timeout retransmission (oldest unacked) ======
        uint32_t oldest = 0;
        bool found = false;
        for (auto& kv : out) {
            if (!kv.second.acked) { oldest = kv.first; found = true; break; }
        }

        if (found) {
            auto& seg = out[oldest];
            if (t - seg.last_sent_ms >= (uint64_t)RDT_RTO_MS) {
                // ====== Reno reaction on timeout ======
                ssthresh = std::max(1, cwnd / 2);
                cwnd = 1;
                dup_ack_cnt = 0;
                cwnd_log_record(cwnd);  // Record cwnd change (timeout)

                RdtHeader dh{};
                dh.seq = seg.seq;
                dh.ack = 0;
                dh.flags = F_DATA;
                dh.wnd = (uint16_t)fixed_wnd;
                dh.len = seg.len;
                dh.sack_mask = 0;

                send_pkt(sock, peer, dh, seg.data.data());
                seg.last_sent_ms = t;
                seg.retx++;

                LOG("TIMEOUT -> Retransmit seq=%u, cwnd=1 ssthresh=%d retx=%d",
                    seg.seq, ssthresh, seg.retx);

                if (seg.retx > RDT_MAX_RETX) die("too many retransmissions");
            }
        }

        // ====== FIN retransmission (handshake-like) ======
        if (fin_sent && !fin_acked) {
            if (t - fin_last >= (uint64_t)RDT_HANDSHAKE_RTO_MS) {
                if (fin_retx++ > RDT_MAX_RETX) die("FIN not acked (too many retries)");
                RdtHeader fin{};
                fin.seq = next_seq;
                fin.ack = 0;
                fin.flags = F_FIN;
                fin.wnd = (uint16_t)fixed_wnd;
                fin.len = 0;
                fin.sack_mask = 0;
                send_pkt(sock, peer, fin, nullptr);
                fin_last = t;
                LOG("RETX FIN(seq=%u) retx=%d", fin.seq, fin_retx);
            }
        }

        Sleep(1);
    }

    uint64_t end_ms = now_ms();
    double sec = (end_ms - start_ms) / 1000.0;
    double throughput = (filedata.size() / 1024.0 / 1024.0) / std::max(1e-9, sec);
    LOG("Transfer done. time=%.3f s, avg throughput=%.3f MB/s", sec, throughput);

    // ====== CWND logging: close and generate plot ======
    cwnd_log_close();
    cwnd_plot_generate();

    closesocket(sock);
    WSACleanup();
    return 0;
}
