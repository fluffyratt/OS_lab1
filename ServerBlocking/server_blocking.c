/*
 * Модель: accept() у головному потоці + окремий потік на кожне з'єднання.
 *
 *   server_blocking.exe tcp:127.0.0.1:9000
 *   server_blocking.exe unix:bench.sock
 */
#include "../common.h"

#include <process.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define BUF_SIZE (1 << 16)

static endpoint_t g_ep;

static unsigned __stdcall handle_client(void *arg)
{
    SOCKET s = (SOCKET)(uintptr_t)arg;
    char *buf = malloc(BUF_SIZE);
    if (buf) {
        for (;;) {
            int r = recv(s, buf, BUF_SIZE, 0);     /* блокується */
            if (r <= 0) break;                     /* 0 = клієнт закрив */
            if (send_all(s, buf, r) < 0) break;    /* блокується */
        }
        free(buf);
    }
    closesocket(s);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s unix:PATH | tcp:HOST:PORT\n", argv[0]);
        return 1;
    }
    if (net_init() < 0) return 1;
    if (ep_parse(argv[1], &g_ep) < 0) return 1;

    SOCKET ls = ep_listen(&g_ep, SOMAXCONN);
    if (ls == INVALID_SOCKET) return 1;
    fprintf(stderr, "[blocking] listening on %s  (Ctrl+C to stop)\n", g_ep.text);

    for (;;) {
        SOCKET cs = accept(ls, NULL, NULL);
        if (cs == INVALID_SOCKET) {
            print_wsa_error("accept");
            continue;
        }
        tune_socket(cs, &g_ep);

        uintptr_t th = _beginthreadex(NULL, 256 * 1024, handle_client,
                                      (void *)(uintptr_t)cs, 0, NULL);
        if (th == 0) {
            fprintf(stderr, "_beginthreadex failed\n");
            closesocket(cs);
        } else {
            CloseHandle((HANDLE)th);   /* потік "detached" */
        }
    }
}
