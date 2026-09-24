#define _CRT_SECURE_NO_WARNINGS
#define _WINSOCK_DEPRECATED_NO_WARNINGS


#include "../common.h"

#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    OP_NONE,
    OP_SEND,
    OP_RECV
} operation_t;

typedef struct {
    SOCKET socket;

    WSAEVENT event;
    WSAOVERLAPPED overlapped;
    WSABUF wsabuf;

    char* out;
    char* in;

    operation_t operation;

    DWORD send_offset;
    DWORD recv_offset;

    long completed;

    int pending;
    int failed;
} async_client_t;


typedef struct {
    uint64_t msgs;

    uint64_t t_socket_ns;
    uint64_t t_connect_ns;
    uint64_t t_close_ns;

    int failed;
} conn_worker_t;


static endpoint_t g_ep;

static int  g_size = 64;
static long g_count = 100000;
static int  g_conns = 1;
static int  g_mode_conn = 0;


/* =========================================================
 * SOCKET
 * ========================================================= */

static SOCKET create_overlapped_socket(void)
{
    return WSASocketA(
        g_ep.family,
        SOCK_STREAM,
        0,
        NULL,
        0,
        WSA_FLAG_OVERLAPPED
    );
}


/* =========================================================
 * OVERLAPPED HELPERS
 * ========================================================= */

static void prepare_overlapped(async_client_t* client)
{
    WSAEVENT event = client->event;

    memset(
        &client->overlapped,
        0,
        sizeof client->overlapped
    );

    client->overlapped.hEvent = event;

    WSAResetEvent(event);
}


static int post_send(async_client_t* client)
{
    prepare_overlapped(client);

    client->operation = OP_SEND;

    client->wsabuf.buf =
        client->out + client->send_offset;

    client->wsabuf.len =
        g_size - client->send_offset;

    DWORD bytes = 0;

    int r = WSASend(
        client->socket,
        &client->wsabuf,
        1,
        &bytes,
        0,
        &client->overlapped,
        NULL
    );

    if (r == 0) {
        client->pending = 1;
        return 0;
    }

    int err = WSAGetLastError();

    if (err == WSA_IO_PENDING) {
        client->pending = 1;
        return 0;
    }

    print_wsa_error("WSASend");

    return -1;
}


static int post_recv(async_client_t* client)
{
    prepare_overlapped(client);

    client->operation = OP_RECV;

    client->wsabuf.buf =
        client->in + client->recv_offset;

    client->wsabuf.len =
        g_size - client->recv_offset;

    DWORD flags = 0;
    DWORD bytes = 0;

    int r = WSARecv(
        client->socket,
        &client->wsabuf,
        1,
        &bytes,
        &flags,
        &client->overlapped,
        NULL
    );

    if (r == 0) {
        client->pending = 1;
        return 0;
    }

    int err = WSAGetLastError();

    if (err == WSA_IO_PENDING) {
        client->pending = 1;
        return 0;
    }

    print_wsa_error("WSARecv");

    return -1;
}


/* =========================================================
 * ECHO BENCHMARK
 * ========================================================= */

static int run_echo(void)
{
    if (g_conns > WSA_MAXIMUM_WAIT_EVENTS) {
        fprintf(
            stderr,
            "too many connections; max=%d\n",
            WSA_MAXIMUM_WAIT_EVENTS
        );

        return 1;
    }

    async_client_t* clients =
        calloc(g_conns, sizeof * clients);

    if (!clients) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }

    int failed = 0;

    /*
     * Спочатку створюємо та підключаємо всі sockets.
     * Connection setup НЕ входить у echo timer.
     */
    for (int i = 0; i < g_conns; i++) {

        async_client_t* client = &clients[i];

        client->socket =
            create_overlapped_socket();

        if (client->socket == INVALID_SOCKET) {
            print_wsa_error("WSASocket");
            client->failed = 1;
            failed = 1;
            continue;
        }

        if (ep_connect_fd(
            client->socket,
            &g_ep) < 0) {

            print_wsa_error("connect");

            closesocket(client->socket);

            client->socket = INVALID_SOCKET;
            client->failed = 1;
            failed = 1;

            continue;
        }

        tune_socket(client->socket, &g_ep);

        client->event = WSACreateEvent();

        if (client->event == WSA_INVALID_EVENT) {
            print_wsa_error("WSACreateEvent");

            closesocket(client->socket);

            client->socket = INVALID_SOCKET;
            client->failed = 1;
            failed = 1;

            continue;
        }

        client->out = malloc(g_size);
        client->in = malloc(g_size);

        if (!client->out || !client->in) {
            fprintf(stderr, "allocation failed\n");

            free(client->out);
            free(client->in);

            WSACloseEvent(client->event);
            closesocket(client->socket);

            client->socket = INVALID_SOCKET;
            client->failed = 1;
            failed = 1;

            continue;
        }

        for (int j = 0; j < g_size; j++) {
            client->out[j] =
                (char)('a' + j % 26);
        }

        client->operation = OP_NONE;
        client->send_offset = 0;
        client->recv_offset = 0;
        client->completed = 0;
        client->pending = 0;
    }

    if (failed) {
        fprintf(
            stderr,
            "failed to initialize clients\n"
        );
    }

    uint64_t start = now_ns();

    for (int i = 0; i < g_conns; i++) {

        if (clients[i].failed)
            continue;

        if (post_send(&clients[i]) < 0) {
            clients[i].failed = 1;
            failed = 1;
        }
    }

    uint64_t total_messages = 0;

    uint64_t expected_messages =
        (uint64_t)g_count *
        (uint64_t)g_conns;

    while (total_messages < expected_messages) {

        WSAEVENT events[WSA_MAXIMUM_WAIT_EVENTS];
        int map[WSA_MAXIMUM_WAIT_EVENTS];

        DWORD event_count = 0;

        for (int i = 0; i < g_conns; i++) {

            async_client_t* client =
                &clients[i];

            if (client->failed ||
                !client->pending ||
                client->completed >= g_count) {

                continue;
            }

            events[event_count] =
                client->event;

            map[event_count] = i;

            event_count++;
        }

        if (event_count == 0) {
            break;
        }

        DWORD wait_result =
            WSAWaitForMultipleEvents(
                event_count,
                events,
                FALSE,
                WSA_INFINITE,
                FALSE
            );

        if (wait_result == WSA_WAIT_FAILED) {
            print_wsa_error(
                "WSAWaitForMultipleEvents"
            );

            failed = 1;
            break;
        }

        DWORD event_index =
            wait_result -
            WSA_WAIT_EVENT_0;

        if (event_index >= event_count) {
            fprintf(
                stderr,
                "invalid event index\n"
            );

            failed = 1;
            break;
        }

        int client_index =
            map[event_index];

        async_client_t* client =
            &clients[client_index];

        DWORD transferred = 0;
        DWORD flags = 0;

        BOOL ok =
            WSAGetOverlappedResult(
                client->socket,
                &client->overlapped,
                &transferred,
                FALSE,
                &flags
            );

        client->pending = 0;

        if (!ok) {
            print_wsa_error(
                "WSAGetOverlappedResult"
            );

            client->failed = 1;
            failed = 1;

            continue;
        }

        /*
         * =====================================
         * SEND completed
         * =====================================
         */
        if (client->operation == OP_SEND) {

            if (transferred == 0) {
                client->failed = 1;
                failed = 1;
                continue;
            }

            client->send_offset +=
                transferred;

            /*
             * Partial WSASend.
             */
            if (client->send_offset
                < (DWORD)g_size) {

                if (post_send(client) < 0) {
                    client->failed = 1;
                    failed = 1;
                }

                continue;
            }

            client->send_offset = 0;
            client->recv_offset = 0;

            if (post_recv(client) < 0) {
                client->failed = 1;
                failed = 1;
            }

            continue;
        }

        /*
         * =====================================
         * RECEIVE completed
         * =====================================
         */
        if (client->operation == OP_RECV) {

            if (transferred == 0) {
                client->failed = 1;
                failed = 1;
                continue;
            }

            client->recv_offset +=
                transferred;

            /*
             * Partial WSARecv.
             */
            if (client->recv_offset
                < (DWORD)g_size) {

                if (post_recv(client) < 0) {
                    client->failed = 1;
                    failed = 1;
                }

                continue;
            }

            client->recv_offset = 0;

            client->completed++;
            total_messages++;

            if (client->completed < g_count) {

                if (post_send(client) < 0) {
                    client->failed = 1;
                    failed = 1;
                }
            }
        }
    }

    double secs =
        (double)(now_ns() - start) /
        1e9;

    uint64_t bytes =
        total_messages *
        (uint64_t)g_size;

    double mps =
        secs > 0
        ? total_messages / secs
        : 0;

    double mbps =
        secs > 0
        ? bytes /
        secs /
        (1024.0 * 1024.0)
        : 0;

    double rtt_us =
        total_messages
        ? secs *
        g_conns /
        (double)total_messages *
        1e6
        : 0;

    const char* transport =
        g_ep.family == AF_UNIX
        ? "unix"
        : "tcp";

    printf(
        "[async/echo] %s "
        "size=%d conns=%d "
        "msgs=%llu time=%.3fs\n",
        transport,
        g_size,
        g_conns,
        (unsigned long long)total_messages,
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
        "CSV,async,%s,echo,"
        "%d,%d,%llu,%.4f,%.0f,"
        "%.2f,%.2f,0,0,%d\n",
        transport,
        g_size,
        g_conns,
        (unsigned long long)total_messages,
        secs,
        mps,
        mbps,
        rtt_us,
        failed
    );

    /*
     * Cleanup.
     */
    for (int i = 0; i < g_conns; i++) {

        async_client_t* client =
            &clients[i];

        if (client->socket != INVALID_SOCKET) {
            closesocket(client->socket);
        }

        if (client->event !=
            WSA_INVALID_EVENT) {

            WSACloseEvent(client->event);
        }

        free(client->out);
        free(client->in);
    }

    free(clients);

    return failed;
}


/* =========================================================
 * CONNECTION SETUP BENCHMARK
 * ========================================================= */

static unsigned __stdcall
conn_worker(void* arg)
{
    conn_worker_t* worker =
        (conn_worker_t*)arg;

    for (long i = 0; i < g_count; i++) {

        uint64_t t0 = now_ns();

        SOCKET s =
            create_overlapped_socket();

        uint64_t t1 = now_ns();

        if (s == INVALID_SOCKET) {
            print_wsa_error("WSASocket");

            worker->failed = 1;
            break;
        }

        if (ep_connect_fd(
            s,
            &g_ep) < 0) {

            print_wsa_error("connect");

            closesocket(s);

            worker->failed = 1;
            break;
        }

        uint64_t t2 = now_ns();

        closesocket(s);

        uint64_t t3 = now_ns();

        worker->t_socket_ns +=
            t1 - t0;

        worker->t_connect_ns +=
            t2 - t1;

        worker->t_close_ns +=
            t3 - t2;

        worker->msgs++;
    }

    return 0;
}


static int run_conn(void)
{
    conn_worker_t* workers =
        calloc(
            g_conns,
            sizeof * workers
        );

    HANDLE* threads =
        calloc(
            g_conns,
            sizeof * threads
        );

    if (!workers || !threads) {
        fprintf(stderr, "allocation failed\n");
        return 1;
    }

    uint64_t start = now_ns();

    for (int i = 0; i < g_conns; i++) {

        threads[i] =
            (HANDLE)_beginthreadex(
                NULL,
                0,
                conn_worker,
                &workers[i],
                0,
                NULL
            );
    }

    for (int i = 0; i < g_conns; i++) {

        WaitForSingleObject(
            threads[i],
            INFINITE
        );

        CloseHandle(threads[i]);
    }

    double secs =
        (double)(now_ns() - start) /
        1e9;

    uint64_t msgs = 0;

    uint64_t ts = 0;
    uint64_t tc = 0;
    uint64_t tcl = 0;

    int failed = 0;

    for (int i = 0; i < g_conns; i++) {

        msgs += workers[i].msgs;

        ts +=
            workers[i].t_socket_ns;

        tc +=
            workers[i].t_connect_ns;

        tcl +=
            workers[i].t_close_ns;

        failed |=
            workers[i].failed;
    }

    double n =
        msgs
        ? (double)msgs
        : 1.0;

    const char* transport =
        g_ep.family == AF_UNIX
        ? "unix"
        : "tcp";

    printf(
        "[async/conn] %s "
        "conns=%d n=%llu "
        "time=%.3fs %.0f conn/s\n",
        transport,
        g_conns,
        (unsigned long long)msgs,
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
        "CSV,async,%s,conn,"
        "0,%d,%llu,%.4f,%.0f,"
        "0,%.2f,%.2f,%.2f,%d\n",
        transport,
        g_conns,
        (unsigned long long)msgs,
        secs,
        msgs / secs,
        ts / n / 1e3,
        tc / n / 1e3,
        tcl / n / 1e3,
        failed
    );

    free(workers);
    free(threads);

    return failed;
}


static void usage(const char* program)
{
    fprintf(
        stderr,
        "usage: %s -a ADDR "
        "[-m echo|conn] "
        "[-s SIZE] "
        "[-n COUNT] "
        "[-c CONNS]\n",
        program
    );

    exit(1);
}


int main(int argc, char** argv)
{
    const char* addr = NULL;

    for (int i = 1; i < argc; i++) {

        if (i + 1 >= argc) {
            usage(argv[0]);
        }

        const char* option =
            argv[i];

        const char* value =
            argv[++i];

        if (!strcmp(option, "-a")) {
            addr = value;
        }
        else if (!strcmp(option, "-m")) {

            if (!strcmp(value, "echo")) {
                g_mode_conn = 0;
            }
            else if (!strcmp(value, "conn")) {
                g_mode_conn = 1;
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
        (!g_mode_conn &&
            g_size <= 0)) {

        usage(argv[0]);
    }

    if (!g_mode_conn &&
        g_conns >
        WSA_MAXIMUM_WAIT_EVENTS) {

        fprintf(
            stderr,
            "async echo supports "
            "at most %d connections\n",
            WSA_MAXIMUM_WAIT_EVENTS
        );

        return 1;
    }

    if (net_init() < 0) {
        return 1;
    }

    if (ep_parse(
        addr,
        &g_ep) < 0) {

        net_cleanup();
        return 1;
    }

    int result;

    if (g_mode_conn) {
        result = run_conn();
    }
    else {
        result = run_echo();
    }

    net_cleanup();

    return result;
}