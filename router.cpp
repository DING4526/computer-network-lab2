// router.cpp - Linux 版网络模拟路由器
// 功能与 Windows 版 Router.exe 相同
// 用于在 sender 和 receiver 之间转发数据包，模拟丢包和延时
// 对来自 Client 的数据包进行丢包/延时处理后发给 Server
// 对来自 Server 的包不做处理，直接转发给 Client
//
// 使用方式: ./router <router_port> <server_ip> <server_port> <loss_rate%> <delay_ms>
// 示例: ./router 12345 127.0.0.1 54321 3 5
// (丢包率3%，延时5ms)

#include "rdt.h"
#include <queue>
#include <random>

static void die(const char* msg) {
    printf("错误: %s (errno=%d)\n", msg, WSAGetLastError());
    exit(1);
}

static void set_nonblocking(SOCKET s) {
#ifdef _WIN32
    u_long mode = 1;
    if (ioctlsocket(s, FIONBIO, &mode) != 0) die("ioctlsocket nonblocking failed");
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags == -1 || fcntl(s, F_SETFL, flags | O_NONBLOCK) == -1)
        die("fcntl nonblocking failed");
#endif
}

static std::mt19937 rng((unsigned)time(nullptr));
static std::uniform_real_distribution<double> dist(0.0, 1.0);

static double rnd01() {
    return dist(rng);
}

// 延迟队列中的数据包
struct DelayedPacket {
    uint64_t send_time_ms;      // 应该发送的时间
    std::vector<uint8_t> data;  // 数据包内容
    sockaddr_in dest;           // 目标地址
};

int main(int argc, char** argv) {
    if (argc < 6) {
        printf("用法: router <router_port> <server_ip> <server_port> <loss_rate%%> <delay_ms>\n");
        printf("示例: ./router 12345 127.0.0.1 54321 3 5\n");
        printf("\n");
        printf("参数说明:\n");
        printf("  router_port  - 路由器监听端口 (Client 连接此端口)\n");
        printf("  server_ip    - Server(Receiver) 的 IP 地址\n");
        printf("  server_port  - Server(Receiver) 的端口\n");
        printf("  loss_rate%%   - 丢包率百分比 (如 3 表示 3%%)\n");
        printf("  delay_ms     - 延时毫秒数 (如 5 表示 5ms)\n");
        printf("\n");
        printf("拓扑结构:\n");
        printf("  Sender -> Router(router_port) -[丢包/延时]-> Receiver(server_ip:server_port)\n");
        printf("  Sender <- Router(router_port) <-[直接转发]-- Receiver\n");
        return 0;
    }

    int router_port = atoi(argv[1]);
    std::string server_ip = argv[2];
    int server_port = atoi(argv[3]);
    double loss_rate = atof(argv[4]) / 100.0;  // 转换为小数
    int delay_ms = atoi(argv[5]);

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) die("WSAStartup");
#endif

    // 创建 UDP socket
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) die("socket");
    set_nonblocking(sock);

    // 绑定监听端口
    sockaddr_in local_addr{};
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons((uint16_t)router_port);
    local_addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (sockaddr*)&local_addr, sizeof(local_addr)) != 0) die("bind");

    // 目标地址 (Server/Receiver)
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((uint16_t)server_port);
    server_addr.sin_addr.s_addr = inet_addr(server_ip.c_str());

    printf("========================================\n");
    printf("       Router 网络模拟器已启动\n");
    printf("========================================\n");
    printf("监听端口: %d\n", router_port);
    printf("转发目标: %s:%d\n", server_ip.c_str(), server_port);
    printf("丢包率: %.1f%%\n", loss_rate * 100);
    printf("延时: %d ms\n", delay_ms);
    printf("========================================\n");
    printf("等待连接...\n\n");

    // 记录 Client/Sender 地址（第一个非 server 的发送方）
    sockaddr_in client_addr{};
    bool client_known = false;

    // 延迟发送队列
    std::queue<DelayedPacket> delay_queue;

    uint64_t total_from_client = 0;
    uint64_t total_from_server = 0;
    uint64_t dropped_pkts = 0;
    uint64_t forwarded_pkts = 0;

    while (true) {
        uint8_t buf[RDT_MAX_PKT];
        sockaddr_in from{};
        socklen_t fromlen = sizeof(from);

        int n = recvfrom(sock, (char*)buf, sizeof(buf), 0, (sockaddr*)&from, &fromlen);
        if (n > 0) {
            // 判断是来自 Client 还是 Server
            bool from_server = (from.sin_addr.s_addr == server_addr.sin_addr.s_addr &&
                                from.sin_port == server_addr.sin_port);

            if (!from_server && !client_known) {
                client_addr = from;
                client_known = true;
                printf("[连接] Client 已连接: %s:%d\n",
                       inet_ntoa(from.sin_addr), ntohs(from.sin_port));
            }

            if (from_server) {
                // 来自 Server，直接转发给 Client（不做任何处理）
                total_from_server++;
                if (client_known) {
                    sendto(sock, (const char*)buf, n, 0,
                           (const sockaddr*)&client_addr, sizeof(client_addr));
                    forwarded_pkts++;
                }
            } else {
                // 来自 Client，进行丢包和延时处理后转发给 Server
                total_from_client++;
                
                // 模拟丢包
                if (loss_rate > 0.0 && rnd01() < loss_rate) {
                    dropped_pkts++;
                    // 丢弃这个包，不转发
                    continue;
                }

                // 添加到延迟队列
                if (delay_ms > 0) {
                    DelayedPacket pkt;
                    pkt.send_time_ms = now_ms() + delay_ms;
                    pkt.data.assign(buf, buf + n);
                    pkt.dest = server_addr;
                    delay_queue.push(pkt);
                } else {
                    // 无延迟，直接发送
                    sendto(sock, (const char*)buf, n, 0,
                           (const sockaddr*)&server_addr, sizeof(server_addr));
                    forwarded_pkts++;
                }
            }
        }

        // 处理延迟队列，发送到期的包
        uint64_t current_time = now_ms();
        while (!delay_queue.empty() && delay_queue.front().send_time_ms <= current_time) {
            DelayedPacket& pkt = delay_queue.front();
            sendto(sock, (const char*)pkt.data.data(), (int)pkt.data.size(), 0,
                   (const sockaddr*)&pkt.dest, sizeof(pkt.dest));
            forwarded_pkts++;
            delay_queue.pop();
        }

        // 定期打印统计
        static uint64_t last_print = 0;
        if (current_time - last_print >= 3000) {  // 每3秒打印一次
            if (total_from_client > 0) {
                printf("[统计] Client->Server: %llu 包, 丢弃: %llu (%.1f%%), Server->Client: %llu 包, 转发: %llu\n",
                       (unsigned long long)total_from_client,
                       (unsigned long long)dropped_pkts,
                       (total_from_client > 0) ? (dropped_pkts * 100.0 / total_from_client) : 0.0,
                       (unsigned long long)total_from_server,
                       (unsigned long long)forwarded_pkts);
            }
            last_print = current_time;
        }

        Sleep(1);
    }

    closesocket(sock);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
