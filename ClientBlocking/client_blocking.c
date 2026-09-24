#include "../common.h"

#include <process.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int id;
    uint64_t msgs, bytes;
    uint64_t t_socket_ns, t_connect_ns, t_close_ns;
    int failed;
} worker_t;

static endpoint_t g_ep;
static int  g_size  = 64;
static long g_count = 100000;
static int  g_conns = 1;
static int  g_mode_conn = 0;

static HANDLE g_ready;  
static HANDLE g_start;  

static unsigned __stdcall echo_worker(void *arg)
{
    worker_t *w = arg;
    char *out = malloc(g_size), *in = malloc(g_size);
    for (int i = 0; i < g_size; i++) out[i] = (char)('a' + i % 26);

    SOCKET s = ep_connect(&g_ep);
    if (s == INVALID_SOCKET) { print_wsa_error("connect"); w->failed = 1; }
    else tune_socket(s, &g_ep);

    ReleaseSemaphore(g_ready, 1, NULL);
    WaitForSingleObject(g_start, INFINITE);

    if (!w->failed) {
        for (long i = 0; i < g_count; i++) {
            if (send_all(s, out, g_size) < 0 ||
                recv_all(s, in, g_size) != g_size) {
                print_wsa_error("echo");
                w->failed = 1;
                break;
            }
            w->msgs++;
            w->bytes += (uint64_t)g_size;
        }
        closesocket(s);
    }
    free(out); free(in);
    return 0;
}

static unsigned __stdcall conn_worker(void *arg)
{
    worker_t *w = arg;
    ReleaseSemaphore(g_ready, 1, NULL);
    WaitForSingleObject(g_start, INFINITE);

    for (long i = 0; i < g_count; i++) {
        uint64_t t0 = now_ns();
        SOCKET s = ep_socket(&g_ep);
        uint64_t t1 = now_ns();
        if (s == INVALID_SOCKET) { print_wsa_error("socket"); w->failed = 1; break; }
        if (ep_connect_fd(s, &g_ep) < 0) {
            print_wsa_error("connect"); closesocket(s); w->failed = 1; break;
        }
        uint64_t t2 = now_ns();
        closesocket(s);
        uint64_t t3 = now_ns();

        w->t_socket_ns  += t1 - t0;
        w->t_connect_ns += t2 - t1;
        w->t_close_ns   += t3 - t2;
        w->msgs++;
    }
    return 0;
}

static void usage(const char *p)
{
    fprintf(stderr,
        "usage: %s -a ADDR [-m echo|conn] [-s SIZE] [-n COUNT] [-c CONNS]\n", p);
    exit(1);
}

int main(int argc, char **argv)
{
    const char *addr = NULL;
    for (int i = 1; i < argc; i++) {           
        const char *o = argv[i];
        if (i + 1 >= argc) usage(argv[0]);
        const char *v = argv[++i];
        if      (!strcmp(o, "-a")) addr = v;
        else if (!strcmp(o, "-m")) g_mode_conn = !strcmp(v, "conn");
        else if (!strcmp(o, "-s")) g_size  = atoi(v);
        else if (!strcmp(o, "-n")) g_count = atol(v);
        else if (!strcmp(o, "-c")) g_conns = atoi(v);
        else usage(argv[0]);
    }
   // if (!addr || g_size <= 0 || g_count <= 0 || g_conns <= 0) usage(argv[0]);
    if (!addr ||
        g_count <= 0 ||
        g_conns <= 0 ||
        (!g_mode_conn && g_size <= 0))
    {
        usage(argv[0]);
    }
    if (net_init() < 0) return 1;
    if (ep_parse(addr, &g_ep) < 0) return 1;

    worker_t *ws  = calloc(g_conns, sizeof *ws);
    HANDLE   *ths = calloc(g_conns, sizeof *ths);
    g_ready = CreateSemaphoreA(NULL, 0, g_conns, NULL);
    g_start = CreateEventA(NULL, TRUE, FALSE, NULL);

    for (int i = 0; i < g_conns; i++) {
        ws[i].id = i;
        ths[i] = (HANDLE)_beginthreadex(NULL, 0,
                     g_mode_conn ? conn_worker : echo_worker, &ws[i], 0, NULL);
    }
    for (int i = 0; i < g_conns; i++) WaitForSingleObject(g_ready, INFINITE);

    uint64_t start = now_ns();
    SetEvent(g_start);
    for (int i = 0; i < g_conns; i++) {        /* WaitForMultipleObjects ≤ 64 */
        WaitForSingleObject(ths[i], INFINITE);
        CloseHandle(ths[i]);
    }
    double secs = (double)(now_ns() - start) / 1e9;

    uint64_t msgs = 0, bytes = 0, ts = 0, tc = 0, tcl = 0;
    int failed = 0;
    for (int i = 0; i < g_conns; i++) {
        msgs += ws[i].msgs; bytes += ws[i].bytes;
        ts += ws[i].t_socket_ns; tc += ws[i].t_connect_ns; tcl += ws[i].t_close_ns;
        failed |= ws[i].failed;
    }
    const char *transport = g_ep.family == AF_UNIX ? "unix" : "tcp";
    double n = msgs ? (double)msgs : 1.0;

    if (g_mode_conn) {
        printf("[blocking/conn] %s conns=%d n=%.0f time=%.3fs  %.0f conn/s\n",
               transport, g_conns, (double)msgs, secs, msgs / secs);
        printf("  avg socket()=%.1f us  connect()=%.1f us  close()=%.1f us%s\n",
               ts / n / 1e3, tc / n / 1e3, tcl / n / 1e3,
               failed ? "  (SOME FAILED)" : "");
        printf("CSV,blocking,%s,conn,0,%d,%.0f,%.4f,%.0f,0,%.2f,%.2f,%.2f,%d\n",
               transport, g_conns, (double)msgs, secs, msgs / secs,
               ts / n / 1e3, tc / n / 1e3, tcl / n / 1e3, failed);
    } else {
        double mps  = msgs / secs;
        double mbps = bytes / secs / (1024.0 * 1024.0);
        double rtt_us = msgs ? secs * g_conns / n * 1e6 : 0;
        printf("[blocking/echo] %s size=%d conns=%d msgs=%.0f time=%.3fs\n",
               transport, g_size, g_conns, (double)msgs, secs);
        printf("  %.0f msg/s   %.2f MB/s   avg RTT=%.1f us%s\n",
               mps, mbps, rtt_us, failed ? "  (SOME FAILED)" : "");
        printf("CSV,blocking,%s,echo,%d,%d,%.0f,%.4f,%.0f,%.2f,%.2f,0,0,%d\n",
               transport, g_size, g_conns, (double)msgs, secs,
               mps, mbps, rtt_us, failed);
    }
    net_cleanup();
    return failed;
}
