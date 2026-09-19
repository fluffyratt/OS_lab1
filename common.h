/*
 *
 * Адреси задаються рядком:
 *   unix:bench.sock          (AF_UNIX, Windows 10 1803+)
 *   tcp:127.0.0.1:9000       (AF_INET)
 */
#ifndef COMMON_H
#define COMMON_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00         
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#include <windows.h>

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int family;                      /* AF_UNIX або AF_INET */
    struct sockaddr_storage addr;
    int addrlen;
    char text[160];
} endpoint_t;

int      net_init(void);             /* WSAStartup */
void     net_cleanup(void);          /* WSACleanup */
void     print_wsa_error(const char *what);

int      ep_parse(const char *s, endpoint_t *ep);
SOCKET   ep_listen(const endpoint_t *ep, int backlog);
SOCKET   ep_socket(const endpoint_t *ep);            /* лише socket() */
int      ep_connect_fd(SOCKET s, const endpoint_t *ep);
SOCKET   ep_connect(const endpoint_t *ep);           /* socket()+connect() */

uint64_t now_ns(void);                               /* QueryPerformanceCounter */
int      send_all(SOCKET s, const char *buf, int n); /* 0 = ok, -1 = помилка */
int      recv_all(SOCKET s, char *buf, int n);       /* к-сть байт або -1 */
int      set_nonblock(SOCKET s, int on);
void     tune_socket(SOCKET s, const endpoint_t *ep);/* TCP_NODELAY */

#endif
