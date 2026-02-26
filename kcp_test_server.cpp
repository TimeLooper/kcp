#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <unistd.h>
#include <time.h>
#include "ikcp.h"

#define MAX_CLIENTS 64
#define SESSION_TIMEOUT 30000  // 30秒无数据断开

// 获取毫秒时间
IUINT32 iclock()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (IUINT32)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

// 客户端会话结构
typedef struct ClientSession {
    ikcpcb *kcp;
    struct sockaddr_in addr;
    IUINT32 last_recv;
    int active;
} ClientSession;

static ClientSession sessions[MAX_CLIENTS];
static int udp_sock = -1;

// 初始化会话数组
void init_sessions()
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        sessions[i].active = 0;
        sessions[i].kcp = NULL;
    }
}

// UDP 发送回调（普通函数，由 KCP 内部调用）
int udp_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
    ClientSession *s = (ClientSession*)user;
    return sendto(udp_sock, buf, len, 0, (struct sockaddr*)&s->addr, sizeof(s->addr));
}

// 根据地址查找会话，返回索引；未找到返回 -1
int find_session_by_addr(struct sockaddr_in *addr)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (sessions[i].active &&
            sessions[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr &&
            sessions[i].addr.sin_port == addr->sin_port) {
            return i;
        }
    }
    return -1;
}

// 创建新会话，指定 conv 和客户端地址
int create_session(IUINT32 conv, struct sockaddr_in *addr)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!sessions[i].active) {
            ikcpcb *kcp = ikcp_create(conv, &sessions[i]);
            kcp->output = udp_output;

            // 可选配置：nodelay 加速
            ikcp_nodelay(kcp, 1, 10, 2, 1);
            // 设置窗口大小（可根据需要调整）
            // ikcp_wndsize(kcp, 128, 128);

            sessions[i].kcp = kcp;
            sessions[i].addr = *addr;
            sessions[i].last_recv = iclock();
            sessions[i].active = 1;

            printf("[新客户端] 连接来自 %s:%d, conv=%u\n",
                   inet_ntoa(addr->sin_addr), ntohs(addr->sin_port), conv);
            return i;
        }
    }
    printf("[警告] 客户端数已达上限，拒绝 %s:%d\n",
           inet_ntoa(addr->sin_addr), ntohs(addr->sin_port));
    return -1;
}

// 清理超时会话
void cleanup_sessions(IUINT32 current)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (sessions[i].active) {
            if (current - sessions[i].last_recv > SESSION_TIMEOUT) {
                printf("[超时] 客户端 %s:%d 断开\n",
                       inet_ntoa(sessions[i].addr.sin_addr),
                       ntohs(sessions[i].addr.sin_port));
                ikcp_release(sessions[i].kcp);
                sessions[i].active = 0;
                sessions[i].kcp = NULL;
            }
        }
    }
}

// 计算所有 KCP 的最小下次刷新时间
IUINT32 get_next_flush_time(IUINT32 current)
{
    IUINT32 next = current + 1000;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (sessions[i].active) {
            IUINT32 t = ikcp_check(sessions[i].kcp, current);
            if (t < next) next = t;
        }
    }
    return next;
}

int main()
{
    srand((unsigned int)time(NULL));
    struct sockaddr_in bind_addr;
    char recv_buf[2048];

    udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) {
        perror("socket");
        return -1;
    }

    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(8888);
    if (bind(udp_sock, (struct sockaddr*)&bind_addr, sizeof(bind_addr)) < 0) {
        perror("bind");
        close(udp_sock);
        return -1;
    }

    printf("多客户端服务器启动，监听端口 8888...\n");
    init_sessions();

    while (1) {
        IUINT32 current = iclock();
        IUINT32 next = get_next_flush_time(current);
        struct timeval timeout;
        int timeout_ms = next - current;
        if (timeout_ms < 0) timeout_ms = 0;
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(udp_sock, &readfds);
        int maxfd = udp_sock + 1;

        int ret = select(maxfd, &readfds, NULL, NULL, &timeout);
        if (ret < 0) {
            perror("select");
            break;
        }

        if (FD_ISSET(udp_sock, &readfds)) {
            struct sockaddr_in peer_addr;
            socklen_t peer_len = sizeof(peer_addr);
            int n = recvfrom(udp_sock, recv_buf, sizeof(recv_buf), 0,
                             (struct sockaddr*)&peer_addr, &peer_len);
            if (n < 0) {
                perror("recvfrom");
                continue;
            }
            if (n < (int)sizeof(IUINT32)) {
                // 包太短，无法提取 conv，丢弃
                continue;
            }

            int r = rand() % 100;
            if (r < 10) {  // 10% 的丢包率
                printf("[丢包] 来自 %s:%d 的数据包被丢弃\n",
                       inet_ntoa(peer_addr.sin_addr), ntohs(peer_addr.sin_port));
                continue;
            }

            // 先根据地址查找会话
            int idx = find_session_by_addr(&peer_addr);
            if (idx < 0) {
                // 新客户端：从数据包中提取 conv（KCP 头部前 4 字节）
                IUINT32 conv = *((IUINT32*)recv_buf);  // 注意主机字节序与发送方一致
                idx = create_session(conv, &peer_addr);
                if (idx < 0) {
                    // 达到上限，丢弃数据
                    continue;
                }
            }

            ClientSession *s = &sessions[idx];
            s->last_recv = current;
            ikcp_input(s->kcp, recv_buf, n);
        }

        // 更新所有 KCP 会话（必须在处理输入后调用，以触发重传等）
        current = iclock();
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (sessions[i].active) {
                ikcp_update(sessions[i].kcp, current);
            }
        }

        // 处理每个会话的接收数据（Echo）
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!sessions[i].active) continue;
            ClientSession *s = &sessions[i];
            while (1) {
                int hr = ikcp_recv(s->kcp, recv_buf, sizeof(recv_buf));
                if (hr < 0) break;
                recv_buf[hr] = '\0';
                printf("[来自 %s:%d] %s\n",
                       inet_ntoa(s->addr.sin_addr),
                       ntohs(s->addr.sin_port),
                       recv_buf);
                // 回声
                ikcp_send(s->kcp, recv_buf, hr);
                ikcp_flush(s->kcp);
            }
        }

        // 清理超时会话
        cleanup_sessions(current);
    }

    close(udp_sock);
    return 0;
}