#include "rdt.h"
#include <map>
#include <vector>
#include <random>

static void die(const char* msg) {
    printf("ERROR: %s (WSA=%d)\n", msg, WSAGetLastError());
    exit(1);
}

static void set_nonblocking(SOCKET s) {
    u_long mode = 1;
    if (ioctlsocket(s, FIONBIO, &mode) != 0) die("ioctlsocket nonblocking failed");
}

static double rnd01() {
    static std::mt19937 rng((unsigned)time(nullptr));
    static std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(rng);
}

static int send_pkt_lossy(SOCKET s, const sockaddr_in& peer, RdtHeader h, const uint8_t* payload, double loss_rate) {
    // 模拟丢包（便于实验测试不同丢包率）
    if (loss_rate > 0.0 && rnd01() < loss_rate) {
        return int(sizeof(RdtHeader) + h.len); // pretend sent
    }
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

struct OutSeg {
    uint32_t seq;
    uint16_t len;
    std::vector<uint8_t> data;
    bool acked = false;
    uint64_t last_sent_ms = 0;
    int retx = 0;
};

static void mark_sack_acked(uint32_t cum_ack, uint64_t sack_mask, std::map<uint32_t, OutSeg>& out) {
    for (int i = 0; i < RDT_SACK_BITS; i++) {
        if (sack_mask & (1ull << i)) {
            uint32_t seq = cum_ack + uint32_t((i + 1) * RDT_MSS);
            auto it = out.find(seq);
            if (it != out.end()) it->second.acked = true;
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 6) {
        printf("Usage: sender.exe <server_ip> <server_port> <input_file> <fixed_wnd_segments> <loss_rate>\n");
        return 0;
    }
    std::string server_ip = argv[1];
    int server_port = atoi(argv[2]);
    std::string in_file = argv[3];
    int fixed_wnd = atoi(argv[4]);
    double loss_rate = atof(argv[5]);

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) die("WSAStartup");

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) die("socket");
    set_nonblocking(sock);

    sockaddr_in peer{};
    peer.sin_family = AF_INET;
    peer.sin_port = htons((uint16_t)server_port);
    peer.sin_addr.s_addr = inet_addr(server_ip.c_str());

    FILE* fp = fopen(in_file.c_str(), "rb");
    if (!fp) {
        printf("ERROR: cannot open input file\n");
        return 1;
    }

    // ====== 连接：三次握手 ======
    uint32_t isn_send = 5000 + (uint32_t)(now_ms() & 0xFFFF);

    uint32_t peer_isn = 0;
    uint32_t base_ack = isn_send + 1; // 数据从 isn_send+1 开始（像TCP）
    uint32_t next_seq = base_ack;

    // 发送 SYN 并等待 SYN|ACK
    printf("Connecting to %s:%d ...\n", server_ip.c_str(), server_port);

    bool established = false;
    uint64_t syn_last = 0;
    int syn_retx = 0;

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
            send_pkt_lossy(sock, peer, syn, nullptr, loss_rate);
            syn_last = t;
        }

        uint8_t buf[RDT_MAX_PKT];
        sockaddr_in from{};
        int fromlen = sizeof(from);
        int n = recvfrom(sock, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fromlen);
        if (n >= (int)sizeof(RdtHeader)) {
            RdtHeader h{};
            memcpy(&h, buf, sizeof(RdtHeader));
            ntoh_header(h);
            uint8_t* payload = buf + sizeof(RdtHeader);
            if (!verify_checksum(h, payload)) continue;

            if ((h.flags & (F_SYN | F_ACK)) == (F_SYN | F_ACK) && h.ack == isn_send + 1) {
                peer_isn = h.seq;
                // 发送最终ACK
                RdtHeader ack{};
                ack.seq = isn_send + 1;
                ack.ack = peer_isn + 1;
                ack.flags = F_ACK;
                ack.wnd = (uint16_t)fixed_wnd;
                ack.len = 0;
                ack.sack_mask = 0;
                send_pkt_lossy(sock, peer, ack, nullptr, loss_rate);
                established = true;
                printf("Connected.\n");
                break;
            }
        }
        Sleep(1);
    }

    // ====== 拥塞控制变量（Reno）======
    int cwnd = 1;                 // 单位：分片
    int ssthresh = fixed_wnd;     // 初始阈值：可取固定窗口
    int dup_ack_cnt = 0;
    uint32_t last_ack = base_ack;

    // ====== 发送缓冲 ======
    std::map<uint32_t, OutSeg> out; // key=seq

    // 读取文件分片（可以边读边发，这里为简单先全读入）
    fseek(fp, 0, SEEK_END);
    long fsz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    printf("File size: %ld bytes\n", fsz);

    std::vector<uint8_t> filedata((size_t)fsz);
    if (fsz > 0) fread(filedata.data(), 1, (size_t)fsz, fp);
    fclose(fp);

    uint64_t start_ms = now_ms();

    size_t file_off = 0;
    bool fin_sent = false;
    bool fin_acked = false;

    while (true) {
        // === 清理已确认的段，避免out越来越大 ===
        while (!out.empty()) {
            auto it = out.begin();
            if (it->second.acked) {
                out.erase(it);
            } else {
                break; // 遇到未确认的就停止（保持窗口内的段）
            }
        }

        // === 计算当前可发送窗口（流控固定窗口 + 拥塞窗口）===
        int inflight = (int)out.size(); // 所有未清理的都是inflight

        int send_wnd = fixed_wnd;
        int eff_wnd = (cwnd < send_wnd) ? cwnd : send_wnd;

        // === 尽量填满窗口（流水线）===
        while (inflight < eff_wnd && file_off < filedata.size()) {
            uint16_t chunk = (uint16_t)std::min((size_t)RDT_MSS, filedata.size() - file_off);
            OutSeg seg;
            seg.seq = next_seq;
            seg.len = chunk;
            seg.data.assign(filedata.begin() + file_off, filedata.begin() + file_off + chunk);

            // 发送 DATA
            RdtHeader dh{};
            dh.seq = seg.seq;
            dh.ack = 0;
            dh.flags = F_DATA;
            dh.wnd = (uint16_t)fixed_wnd;
            dh.len = seg.len;
            dh.sack_mask = 0;

            send_pkt_lossy(sock, peer, dh, seg.data.data(), loss_rate);
            seg.last_sent_ms = now_ms();

            out[seg.seq] = std::move(seg);
            inflight++;

            file_off += chunk;
            next_seq += chunk;
        }

        // === 收 ACK ===
        uint8_t buf[RDT_MAX_PKT];
        sockaddr_in from{};
        int fromlen = sizeof(from);
        int n = recvfrom(sock, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fromlen);

        if (n >= (int)sizeof(RdtHeader)) {
            RdtHeader h{};
            memcpy(&h, buf, sizeof(RdtHeader));
            ntoh_header(h);
            uint8_t* payload = buf + sizeof(RdtHeader);
            if (!verify_checksum(h, payload)) goto after_recv;

            if (h.flags & F_ACK) {
                uint32_t ackno = h.ack;

                // 1) 标记累计ACK到的分片
                if (ackno > last_ack) {
                    dup_ack_cnt = 0;

                    // Reno：新ACK到来
                    if (cwnd < ssthresh) {
                        cwnd += 1; // 慢启动：每个ACK cwnd+1（按分片近似）
                    } else {
                        // 拥塞避免：约每 RTT cwnd+1，这里用分片级近似：每个ACK += 1/cwnd
                        static double ca_acc = 0.0;
                        ca_acc += 1.0 / cwnd;
                        if (ca_acc >= 1.0) { cwnd += 1; ca_acc -= 1.0; }
                    }

                    // 将所有 end<=ack 的段置为acked
                    for (auto& kv : out) {
                        auto& seg = kv.second;
                        if (!seg.acked && (seg.seq + seg.len) <= ackno) {
                            seg.acked = true;
                        }
                    }

                    // 2) SACK标记
                    mark_sack_acked(ackno, h.sack_mask, out);

                    last_ack = ackno;


                } else if (ackno == last_ack) {
                    // dupACK - 处理SACK信息
                    mark_sack_acked(ackno, h.sack_mask, out);
                    
                    dup_ack_cnt++;
                    if (dup_ack_cnt == 3) {
                        // 快速重传：重传"最早未确认且未被SACK的段"
                        uint32_t oldest = 0;
                        bool found = false;
                        for (auto& kv : out) {
                            if (!kv.second.acked) { oldest = kv.first; found = true; break; }
                        }
                        if (found) {
                            // Reno: ssthresh = cwnd/2, cwnd = ssthresh + 3
                            ssthresh = std::max(1, cwnd / 2);
                            cwnd = ssthresh + 3;

                            auto& seg = out[oldest];
                            RdtHeader dh{};
                            dh.seq = seg.seq;
                            dh.flags = F_DATA;
                            dh.wnd = (uint16_t)fixed_wnd;
                            dh.len = seg.len;
                            dh.ack = 0;
                            dh.sack_mask = 0;
                            send_pkt_lossy(sock, peer, dh, seg.data.data(), loss_rate);
                            seg.last_sent_ms = now_ms();
                            seg.retx++;
                        }
                    } else if (dup_ack_cnt > 3) {
                        // 快速恢复：每个额外dupACK cwnd++
                        cwnd += 1;
                    }
                }

                // 若所有数据都acked，发送FIN（out为空且文件读完）
                bool all_acked = (file_off >= filedata.size()) && out.empty();

                if (all_acked && !fin_sent) {
                    RdtHeader fin{};
                    fin.seq = next_seq;       // FIN占用一个序号
                    fin.ack = 0;
                    fin.flags = F_FIN;
                    fin.wnd = (uint16_t)fixed_wnd;
                    fin.len = 0;
                    fin.sack_mask = 0;
                    send_pkt_lossy(sock, peer, fin, nullptr, loss_rate);
                    fin_sent = true;
                }

                if (fin_sent && (h.flags & F_ACK) && h.ack == next_seq + 1) {
                    fin_acked = true;
                }

                // 收到对端FIN则回ACK并结束
                if (h.flags & F_FIN) {
                    RdtHeader ack{};
                    ack.seq = next_seq + 1;
                    ack.ack = h.seq + 1;
                    ack.flags = F_ACK;
                    ack.wnd = (uint16_t)fixed_wnd;
                    ack.len = 0;
                    ack.sack_mask = 0;
                    send_pkt_lossy(sock, peer, ack, nullptr, loss_rate);
                    break;
                }

                if (fin_sent && fin_acked) {
                    // 等对端FIN（对端会发），继续循环接收即可
                }
            }
        }

    after_recv:
        // === 超时重传：检查所有超时的未确认段（真正的SR重传）===
        uint64_t t = now_ms();
        bool timeout_occurred = false;
        
        for (auto& kv : out) {
            auto& seg = kv.second;
            if (!seg.acked && (t - seg.last_sent_ms >= (uint64_t)RDT_RTO_MS)) {
                if (!timeout_occurred) {
                    // 仅第一次超时触发Reno反应
                    ssthresh = std::max(1, cwnd / 2);
                    cwnd = 1;
                    dup_ack_cnt = 0;
                    timeout_occurred = true;
                }

                RdtHeader dh{};
                dh.seq = seg.seq;
                dh.flags = F_DATA;
                dh.wnd = (uint16_t)fixed_wnd;
                dh.len = seg.len;
                dh.ack = 0;
                dh.sack_mask = 0;
                send_pkt_lossy(sock, peer, dh, seg.data.data(), loss_rate);
                seg.last_sent_ms = t;
                seg.retx++;
                if (seg.retx > RDT_MAX_RETX) die("too many retransmissions");
            }
        }

        // 若无事可做，短暂休眠避免CPU满载
        if (out.empty() && file_off >= filedata.size()) {
            Sleep(1);
        }
    }

    uint64_t end_ms = now_ms();
    double sec = (end_ms - start_ms) / 1000.0;
    double throughput = (filedata.size() / 1024.0 / 1024.0) / sec;
    printf("Transfer done. time=%.3f s, avg throughput=%.3f MB/s\n", sec, throughput);

    closesocket(sock);
    WSACleanup();
    return 0;
}
