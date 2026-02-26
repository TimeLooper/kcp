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

IUINT32 iclock()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (IUINT32)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

struct KCPUser {
    int sock;
    struct sockaddr_in peer;
};

int udp_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
    struct KCPUser *u = (struct KCPUser*)user;
    return sendto(u->sock, buf, len, 0, (struct sockaddr*)&u->peer, sizeof(u->peer));
}

int main(int argc, char *argv[])
{
    int sock;
    struct sockaddr_in server_addr;
    char recv_buf[2048];
    ikcpcb *kcp;
    struct KCPUser user;
    srand((unsigned int)time(NULL));
    IUINT32 conv = rand();
    const char *msg = "hello, kcp!";

    // 创建 UDP socket
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    // 设置服务器地址
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr("10.246.84.183");
    server_addr.sin_port = htons(8888);

    // 初始化用户数据
    user.sock = sock;
    user.peer = server_addr;

    // 创建 KCP 对象
    kcp = ikcp_create(conv, &user);
    kcp->output = udp_output;

    // 发送一条消息
    ikcp_send(kcp, msg, strlen(msg));
    ikcp_flush(kcp);  // 立即发送

    printf("Sent: %s\n", msg);

    // 主循环
    while (1) {
        IUINT32 current = iclock();
        IUINT32 next;
        struct timeval timeout;
        fd_set readfds;
        int maxfd = sock + 1;
        int ret;

        next = ikcp_check(kcp, current);
        int timeout_ms = next - current;
        if (timeout_ms < 0) timeout_ms = 0;
        timeout.tv_sec = timeout_ms / 1000;
        timeout.tv_usec = (timeout_ms % 1000) * 1000;

        FD_ZERO(&readfds);
        FD_SET(sock, &readfds);

        ret = select(maxfd, &readfds, NULL, NULL, &timeout);
        if (ret < 0) {
            perror("select");
            break;
        }

        if (FD_ISSET(sock, &readfds)) {
            // 接收 UDP 数据
            int n = recvfrom(sock, recv_buf, sizeof(recv_buf), 0, NULL, NULL);
            if (n < 0) {
                perror("recvfrom");
                break;
            }
            ikcp_input(kcp, recv_buf, n);
        }

        ikcp_update(kcp, iclock());

        // 尝试接收 KCP 数据
        while (1) {
            int hr = ikcp_recv(kcp, recv_buf, sizeof(recv_buf));
            if (hr < 0) break;
            recv_buf[hr] = '\0';
            printf("Recv: %s\n", recv_buf);
            // 收到回声后退出
            // if (strcmp(recv_buf, msg) == 0) {
            //     printf("Echo success, exit.\n");
            //     goto exit;
            // }
            sleep(1);  // 模拟处理时间
            ikcp_send(kcp, recv_buf, hr);  // Echo 回去
            ikcp_flush(kcp);
        }
    }

exit:
    ikcp_release(kcp);
    close(sock);
    return 0;
}