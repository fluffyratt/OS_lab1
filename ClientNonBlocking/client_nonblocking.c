#define _CRT_SECURE_NO_WARNINGS

#include "../common.h"

#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int id;

    uint64_t msgs;
    uint64_t bytes;

    uint64_t t_socket_ns;
    uint64_t t_connect_ns;
    uint64_t t_close_ns;

    int failed;
} worker_t;

static endpoint_t g_ep;

static int g_size = 64;
static long g_count = 100000;
static int g_conns = 1;
static int g_mode_conn = 0;

static HANDLE g_ready;
static HANDLE g_start;

static int wait_socket(SOCKET s, short event)
{
    WSAPOLLFD fd;

    fd.fd = s;
    fd.events = event;
    fd.revents = 0;

    for (;;) {
        int r = WSAPoll(&fd, 1, -1);

        if (r == SOCKET_ERROR) {
            return -1;
        }

        if (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            return -1;
        }

        if (fd.revents & event) {
            return 0;
        }
    }
}


static int connect_nonblocking(SOCKET s, const endpoint_t* ep)
{
    int r = connect(
        s,
        (struct sockaddr*)&ep->addr,
        ep->addrlen
    );

    if (r == 0) {
        return 0;
    }

    int err = WSAGetLastError();

    if (err != WSAEWOULDBLOCK &&
        err != WSAEINPROGRESS &&
        err != WSAEALREADY) {

        return -1;
    }

    if (wait_socket(s, POLLWRNORM) < 0) {
        return -1;
    }

    int so_error = 0;
    int len = sizeof so_error;

    if (getsockopt(
        s,
        SOL_SOCKET,
        SO_ERROR,
        (char*)&so_error,
        &len) == SOCKET_ERROR) {

        return -1;
    }

    if (so_error != 0) {
        WSASetLastError(so_error);
        return -1;
    }

    return 0;
}


static int send_all_nonblocking(
    SOCKET s,
    const char* buf,
    int n)
{
    int sent_total = 0;

    while (sent_total < n) {
        int r = send(
            s,
            buf + sent_total,
            n - sent_total,
            0
        );

        if (r > 0) {
            sent_total += r;
            continue;
        }

        if (r == SOCKET_ERROR) {
            int err = WSAGetLastError();

            if (err == WSAEWOULDBLOCK) {
                if (wait_socket(s, POLLWRNORM) < 0) {
                    return -1;
                }

                continue;
            }
        }

        return -1;
    }

    return 0;
}


static int recv_all_nonblocking(
    SOCKET s,
    char* buf,
    int n)
{
    int received_total = 0;

    while (received_total < n) {
        int r = recv(
            s,
            buf + received_total,
            n - received_total,
            0
        );

        if (r > 0) {
            received_total += r;
            continue;
        }

        if (r == 0) {
            return received_total;
        }

        int err = WSAGetLastError();

        if (err == WSAEWOULDBLOCK) {
            if (wait_socket(s, POLLRDNORM) < 0) {
                return -1;
            }

            continue;
        }

        return -1;
    }

    return received_total;
}


static unsigned __stdcall echo_worker(void* arg)
{
    worker_t* w = (worker_t*)arg;

    char* out = malloc(g_size);
    char* in = malloc(g_size);

    if (!out || !in) {
        w->failed = 1;

        free(out);
        free(in);

        ReleaseSemaphore(g_ready, 1, NULL);
        return 0;
    }

    for (int i = 0; i < g_size; i++) {
        out[i] = (char)('a' + i % 26);
    }

    SOCKET s = ep_socket(&g_ep);

    if (s == INVALID_SOCKET) {
        print_wsa_error("socket");
        w->failed = 1;
    }
    else {
        if (set_nonblock(s, 1) < 0) {
            print_wsa_error("set_nonblock");
            closesocket(s);
            s = INVALID_SOCKET;
            w->failed = 1;
        }
    }

    if (!w->failed) {
        if (connect_nonblocking(s, &g_ep) < 0) {
            print_wsa_error("connect");
            closesocket(s);
            s = INVALID_SOCKET;
            w->failed = 1;
        }
        else {
            tune_socket(s, &g_ep);
        }
    }

    ReleaseSemaphore(g_ready, 1, NULL);
    WaitForSingleObject(g_start, INFINITE);

    if (!w->failed) {
        for (long i = 0; i < g_count; i++) {

            if (send_all_nonblocking(
                s,
                out,
                g_size) < 0) {

                print_wsa_error("send");
                w->failed = 1;
                break;
            }

            if (recv_all_nonblocking(
                s,
                in,
                g_size) != g_size) {

                print_wsa_error("recv");
                w->failed = 1;
                break;
            }

            w->msgs++;
            w->bytes += (uint64_t)g_size;
        }

        closesocket(s);
    }

    free(out);
    free(in);

    return 0;
}


static unsigned __stdcall conn_worker(void* arg)
{
    worker_t* w = (worker_t*)arg;

    ReleaseSemaphore(g_ready, 1, NULL);
    WaitForSingleObject(g_start, INFINITE);

    for (long i = 0; i < g_count; i++) {

        uint64_t t0 = now_ns();

        SOCKET s = ep_socket(&g_ep);

        uint64_t t1 = now_ns();

        if (s == INVALID_SOCKET) {
            print_wsa_error("socket");
            w->failed = 1;
            break;
        }

        if (set_nonblock(s, 1) < 0) {
            print_wsa_error("set_nonblock");
            closesocket(s);
            w->failed = 1;
            break;
        }

        if (connect_nonblocking(s, &g_ep) < 0) {
            print_wsa_error("connect");
            closesocket(s);
            w->failed = 1;
            break;
        }

        uint64_t t2 = now_ns();

        closesocket(s);

        uint64_t t3 = now_ns();

        w->t_socket_ns += t1 - t0;
        w->t_connect_ns += t2 - t1;
        w->t_close_ns += t3 - t2;

        w->msgs++;
    }

    return 0;
}


static void usage(const char* p)
{
    fprintf(
        stderr,
        "usage: %s -a ADDR [-m echo|conn] "
        "[-s SIZE] [-n COUNT] [-c CONNS]\n",
        p
    );

    exit(1);
}


int main(int argc, char** argv)
{
    const char* addr = NULL;

    for (int i = 1; i < argc; i++) {

        const char* option = argv[i];

        if (i + 1 >= argc) {
            usage(argv[0]);
        }

        const char* value = argv[++i];

        if (!strcmp(option, "-a")) {
            addr = value;
        }
        else if (!strcmp(option, "-m")) {
            if (!strcmp(value, "conn")) {
                g_mode_conn = 1;
            }
            else if (!strcmp(value, "echo")) {
                g_mode_conn = 0;
            }
            else {
                usage(argv[0]);
            }
        }
        else if (!strcmp(option, "-s")) {
            g_size = atoi(value);
        }
        else if (!strcmp(option, "-n")) {
            g_count = atol(value);
        }
        else if (!strcmp(option, "-c")) {
            g_conns = atoi(value);
        }
        else {
            usage(argv[0]);
        }
    }

    if (!addr ||
        g_count <= 0 ||
        g_conns <= 0 ||
        (!g_mode_conn && g_size <= 0)) {

        usage(argv[0]);
    }

    if (net_init() < 0) {
        return 1;
    }

    if (ep_parse(addr, &g_ep) < 0) {
        return 1;
    }

    worker_t* workers =
        calloc(g_conns, sizeof * workers);

    HANDLE* threads =
        calloc(g_conns, sizeof * threads);

    if (!workers || !threads) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }

    g_ready =
        CreateSemaphoreA(
            NULL,
            0,
            g_conns,
            NULL
        );

    g_start =
        CreateEventA(
            NULL,
            TRUE,
            FALSE,
            NULL
        );

    for (int i = 0; i < g_conns; i++) {

        workers[i].id = i;

        threads[i] =
            (HANDLE)_beginthreadex(
                NULL,
                0,
                g_mode_conn
                ? conn_worker
                : echo_worker,
                &workers[i],
                0,
                NULL
            );
    }

    for (int i = 0; i < g_conns; i++) {
        WaitForSingleObject(
            g_ready,
            INFINITE
        );
    }

    uint64_t start = now_ns();

    SetEvent(g_start);

    for (int i = 0; i < g_conns; i++) {

        WaitForSingleObject(
            threads[i],
            INFINITE
        );

        CloseHandle(threads[i]);
    }

    double secs =
        (double)(now_ns() - start) / 1e9;

    uint64_t msgs = 0;
    uint64_t bytes = 0;

    uint64_t ts = 0;
    uint64_t tc = 0;
    uint64_t tcl = 0;

    int failed = 0;

    for (int i = 0; i < g_conns; i++) {

        msgs += workers[i].msgs;
        bytes += workers[i].bytes;

        ts += workers[i].t_socket_ns;
        tc += workers[i].t_connect_ns;
        tcl += workers[i].t_close_ns;

        failed |= workers[i].failed;
    }

    const char* transport =
        g_ep.family == AF_UNIX
        ? "unix"
        : "tcp";

    double n =
        msgs
        ? (double)msgs
        : 1.0;

    if (g_mode_conn) {

        printf(
            "[nonblocking/conn] "
            "%s conns=%d n=%.0f "
            "time=%.3fs %.0f conn/s\n",
            transport,
            g_conns,
            (double)msgs,
            secs,
            msgs / secs
        );

        printf(
            "  avg socket()=%.1f us "
            "connect()=%.1f us "
            "close()=%.1f us%s\n",
            ts / n / 1e3,
            tc / n / 1e3,
            tcl / n / 1e3,
            failed
            ? " (SOME FAILED)"
            : ""
        );

        printf(
            "CSV,nonblocking,%s,conn,"
            "0,%d,%.0f,%.4f,%.0f,0,"
            "%.2f,%.2f,%.2f,%d\n",
            transport,
            g_conns,
            (double)msgs,
            secs,
            msgs / secs,
            ts / n / 1e3,
            tc / n / 1e3,
            tcl / n / 1e3,
            failed
        );
    }
    else {

        double mps =
            msgs / secs;

        double mbps =
            bytes / secs /
            (1024.0 * 1024.0);

        double rtt_us =
            msgs
            ? secs * g_conns /
            n * 1e6
            : 0;

        printf(
            "[nonblocking/echo] "
            "%s size=%d conns=%d "
            "msgs=%.0f time=%.3fs\n",
            transport,
            g_size,
            g_conns,
            (double)msgs,
            secs
        );

        printf(
            "  %.0f msg/s "
            "%.2f MB/s "
            "avg RTT=%.1f us%s\n",
            mps,
            mbps,
            rtt_us,
            failed
            ? " (SOME FAILED)"
            : ""
        );

        printf(
            "CSV,nonblocking,%s,echo,"
            "%d,%d,%.0f,%.4f,%.0f,"
            "%.2f,%.2f,0,0,%d\n",
            transport,
            g_size,
            g_conns,
            (double)msgs,
            secs,
            mps,
            mbps,
            rtt_us,
            failed
        );
    }

    CloseHandle(g_ready);
    CloseHandle(g_start);

    free(workers);
    free(threads);

    net_cleanup();

    return failed;
}