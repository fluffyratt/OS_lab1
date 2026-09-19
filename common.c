#define _CRT_SECURE_NO_WARNINGS

#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int net_init(void)
{
    WSADATA wsa;
    int r = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (r != 0) {
        fprintf(stderr, "WSAStartup failed: %d\n", r);
        return -1;
    }
    return 0;
}

void net_cleanup(void)
{
    WSACleanup();
}

void print_wsa_error(const char *what)
{
    int err = WSAGetLastError();
    char msg[256] = "";
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, (DWORD)err, MAKELANGID(LANG_ENGLISH, SUBLANG_DEFAULT),
                   msg, sizeof msg, NULL);
    fprintf(stderr, "%s: WSA error %d: %s", what, err, msg);
}

int ep_parse(const char *s, endpoint_t *ep)
{
    memset(ep, 0, sizeof *ep);
    snprintf(ep->text, sizeof ep->text, "%s", s);

    if (strncmp(s, "unix:", 5) == 0) {
        struct sockaddr_un *un = (struct sockaddr_un *)&ep->addr;
        const char *path = s + 5;
        if (strlen(path) >= sizeof un->sun_path) {
            fprintf(stderr, "unix path too long (max %u)\n",
                    (unsigned)sizeof un->sun_path - 1);
            return -1;
        }
        un->sun_family = AF_UNIX;
        strcpy(un->sun_path, path);
        ep->family = AF_UNIX;
        ep->addrlen = sizeof *un;
        return 0;
    }
    if (strncmp(s, "tcp:", 4) == 0) {
        char host[64];
        const char *colon = strrchr(s + 4, ':');
        size_t hlen = colon ? (size_t)(colon - (s + 4)) : 0;
        if (!colon || hlen >= sizeof host) {
            fprintf(stderr, "bad tcp address, expected tcp:HOST:PORT\n");
            return -1;
        }
        memcpy(host, s + 4, hlen);
        host[hlen] = '\0';

        struct sockaddr_in *in = (struct sockaddr_in *)&ep->addr;
        in->sin_family = AF_INET;
        in->sin_port = htons((unsigned short)atoi(colon + 1));
        if (inet_pton(AF_INET, host, &in->sin_addr) != 1) {
            fprintf(stderr, "bad IPv4 address: %s\n", host);
            return -1;
        }
        ep->family = AF_INET;
        ep->addrlen = sizeof *in;
        return 0;
    }
    fprintf(stderr, "address must start with unix: or tcp:\n");
    return -1;
}

SOCKET ep_socket(const endpoint_t *ep)
{
    return socket(ep->family, SOCK_STREAM, 0);
}

SOCKET ep_listen(const endpoint_t *ep, int backlog)
{
    SOCKET s = ep_socket(ep);
    if (s == INVALID_SOCKET) { print_wsa_error("socket"); return INVALID_SOCKET; }

    if (ep->family == AF_UNIX) {
        /* На Windows файл сокета лишається після закриття — видаляємо. */
        DeleteFileA(((struct sockaddr_un *)&ep->addr)->sun_path);
    }
    /* SO_REUSEADDR на Windows дозволяє "перехопити" чужий порт,
     * тому свідомо його НЕ вмикаємо. */
    if (bind(s, (struct sockaddr *)&ep->addr, ep->addrlen) == SOCKET_ERROR) {
        print_wsa_error("bind"); closesocket(s); return INVALID_SOCKET;
    }
    if (listen(s, backlog) == SOCKET_ERROR) {
        print_wsa_error("listen"); closesocket(s); return INVALID_SOCKET;
    }
    return s;
}

int ep_connect_fd(SOCKET s, const endpoint_t *ep)
{
    return connect(s, (struct sockaddr *)&ep->addr, ep->addrlen) == SOCKET_ERROR
               ? -1 : 0;
}

SOCKET ep_connect(const endpoint_t *ep)
{
    SOCKET s = ep_socket(ep);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    if (ep_connect_fd(s, ep) < 0) { closesocket(s); return INVALID_SOCKET; }
    return s;
}

uint64_t now_ns(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER c;
    if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    /* ділимо частинами, щоб уникнути переповнення */
    uint64_t f = (uint64_t)freq.QuadPart, v = (uint64_t)c.QuadPart;
    return (v / f) * 1000000000ull + (v % f) * 1000000000ull / f;
}

int send_all(SOCKET s, const char *buf, int n)
{
    while (n > 0) {
        int w = send(s, buf, n, 0);
        if (w == SOCKET_ERROR) return -1;
        buf += w;
        n -= w;
    }
    return 0;
}

int recv_all(SOCKET s, char *buf, int n)
{
    int got = 0;
    while (got < n) {
        int r = recv(s, buf + got, n - got, 0);
        if (r == SOCKET_ERROR) return -1;
        if (r == 0) break;                   /* peer closed */
        got += r;
    }
    return got;
}

int set_nonblock(SOCKET s, int on)
{
    u_long mode = on ? 1 : 0;
    return ioctlsocket(s, FIONBIO, &mode) == SOCKET_ERROR ? -1 : 0;
}

void tune_socket(SOCKET s, const endpoint_t *ep)
{
    if (ep->family == AF_INET && !getenv("BENCH_NAGLE")) {
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    }
}
