/* http_get.h — 极简 HTTP/1.1 GET 客户端（纯 socket，无第三方库）
 *
 * 只做一件事：GET /pet.txt，把响应体读回缓冲区。
 * 连接超时 200ms、读超时 300ms，失败立即返回，不阻塞主循环。
 *
 * Windows 用 Winsock2，POSIX 用 BSD sockets。
 */
#ifndef PET_HTTP_GET_H
#define PET_HTTP_GET_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef SOCKET sock_t;
  #define SOCK_INVALID INVALID_SOCKET
  #define sock_close closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/time.h>
  typedef int sock_t;
  #define SOCK_INVALID (-1)
  #define sock_close close
#endif

/* strnicmp：Windows 用 _strnicmp，POSIX 用 strncasecmp */
#ifdef _WIN32
  #define _strnicmp_local(a, b, n) _strnicmp((a), (b), (n))
#else
  #define _strnicmp_local(a, b, n) strncasecmp((a), (b), (n))
#endif

static int http_net_init(void)
{
#ifdef _WIN32
    static int done = 0;
    if (done) return 0;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
    done = 1;
#endif
    return 0;
}

/* 返回 0 成功（out 里有响应体），非 0 失败 */
static int http_get(const char *host, int port, const char *path, char *out, int outcap)
{
    if (http_net_init() != 0) return -1;
    out[0] = 0;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return -1;

    sock_t s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == SOCK_INVALID) { freeaddrinfo(res); return -1; }

    /* 设置收发超时，避免卡住主循环 */
#ifdef _WIN32
    DWORD tv = 300;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = 300000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    if (connect(s, res->ai_addr, (int)res->ai_addrlen) != 0) {
        sock_close(s);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    char req[512];
    int rl = snprintf(req, sizeof(req),
                      "GET %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n\r\n",
                      path, host, port);
    if (send(s, req, rl, 0) != rl) { sock_close(s); return -1; }

    /* 读全部（响应很小） */
    char buf[8192];
    int total = 0;
    for (;;) {
        int n = recv(s, buf + total, (int)sizeof(buf) - 1 - total, 0);
        if (n <= 0) break;
        total += n;
        if (total >= (int)sizeof(buf) - 1) break;
    }
    sock_close(s);
    if (total <= 0) return -1;
    buf[total] = 0;

    /* 跳过响应头 */
    char *body = strstr(buf, "\r\n\r\n");
    if (!body) return -1;
    body += 4;

    /* 状态码必须 200 */
    if (strncmp(buf, "HTTP/1.", 7) != 0) return -1;
    int code = 0;
    if (sscanf(buf, "HTTP/%*d.%*d %d", &code) != 1) return -1;
    if (code != 200) return -1;

    /* 处理 chunked（Bun 的服务默认就这么发） */
    int chunked = 0;
    for (char *h = buf; h < body - 4; h++) {
        if ((h[0] == 'T' || h[0] == 't') && _strnicmp_local(h, "transfer-encoding:", 18) == 0) {
            for (char *q = h; q < body - 4 && *q != '\r'; q++)
                if (*q == 'c' || *q == 'C') chunked = 1;
            break;
        }
    }

    if (!chunked) {
        snprintf(out, outcap, "%s", body);
        return 0;
    }

    /* 解 chunked：<hex 长度>CRLF <data> CRLF ... 0 CRLF CRLF */
    char *p = body;
    int o = 0;
    while (*p && o < outcap - 1) {
        char *end = NULL;
        long sz = strtol(p, &end, 16);
        if (!end || end == p || sz < 0) break;
        if (sz == 0) break;
        p = end;
        while (*p == '\r' || *p == '\n') p++;
        for (long i = 0; i < sz && o < outcap - 1; i++) out[o++] = p[i];
        p += sz;
        while (*p == '\r' || *p == '\n') p++;
    }
    out[o] = 0;
    return 0;
}

#endif /* PET_HTTP_GET_H */
