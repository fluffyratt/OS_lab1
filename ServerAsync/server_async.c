#define _CRT_SECURE_NO_WARNINGS

#include "../common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define BUF_SIZE (1 << 16)

#define MAX_CLIENTS (WSA_MAXIMUM_WAIT_EVENTS - 1)

typedef enum {
    OP_RECV,
    OP_SEND
} io_operation_t;

typedef struct {
    SOCKET socket;

    WSAEVENT event;
    WSAOVERLAPPED overlapped;

    WSABUF wsabuf;
    char buffer[BUF_SIZE];

    io_operation_t operation;

    DWORD send_total;
    DWORD send_offset;

    int active;
} client_t;

static client_t clients[MAX_CLIENTS];


static SOCKET create_listen_socket(
    const endpoint_t* ep,
    int backlog)
{
    SOCKET s = WSASocketA(
        ep->family,
        SOCK_STREAM,
        0,
        NULL,
        0,
        WSA_FLAG_OVERLAPPED
    );

    if (s == INVALID_SOCKET) {
        print_wsa_error("WSASocket");
        return INVALID_SOCKET;
    }

    if (ep->family == AF_UNIX) {
        struct sockaddr_un* un =
            (struct sockaddr_un*)&ep->addr;

        DeleteFileA(un->sun_path);
    }

    if (bind(
        s,
        (struct sockaddr*)&ep->addr,
        ep->addrlen) == SOCKET_ERROR) {

        print_wsa_error("bind");
        closesocket(s);
        return INVALID_SOCKET;
    }

    if (listen(s, backlog) == SOCKET_ERROR) {
        print_wsa_error("listen");
        closesocket(s);
        return INVALID_SOCKET;
    }

    return s;
}


static void close_client(client_t* client)
{
    if (!client->active)
        return;

    closesocket(client->socket);

    if (client->event != WSA_INVALID_EVENT) {
        WSACloseEvent(client->event);
    }

    memset(client, 0, sizeof * client);

    client->socket = INVALID_SOCKET;
    client->event = WSA_INVALID_EVENT;
    client->active = 0;
}


static void prepare_overlapped(client_t* client)
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


static int post_recv(client_t* client)
{
    prepare_overlapped(client);

    client->operation = OP_RECV;

    client->wsabuf.buf = client->buffer;
    client->wsabuf.len = BUF_SIZE;

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
        return 0;
    }

    int err = WSAGetLastError();

    if (err == WSA_IO_PENDING) {
        return 0;
    }

    print_wsa_error("WSARecv");

    return -1;
}



static int post_send(client_t* client)
{
    prepare_overlapped(client);

    client->operation = OP_SEND;

    client->wsabuf.buf =
        client->buffer + client->send_offset;

    client->wsabuf.len =
        client->send_total - client->send_offset;

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
        return 0;
    }

    int err = WSAGetLastError();

    if (err == WSA_IO_PENDING) {
        return 0;
    }

    print_wsa_error("WSASend");

    return -1;
}


static int add_client(
    SOCKET socket,
    const endpoint_t* ep)
{
    int index = -1;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i].active) {
            index = i;
            break;
        }
    }

    if (index < 0) {
        fprintf(stderr, "too many clients\n");
        closesocket(socket);
        return -1;
    }

    client_t* client = &clients[index];

    memset(client, 0, sizeof * client);

    client->socket = socket;
    client->active = 1;

    WSAEventSelect(socket, NULL, 0);
    set_nonblock(socket, 0);

    tune_socket(socket, ep);

    client->event = WSACreateEvent();

    if (client->event == WSA_INVALID_EVENT) {
        print_wsa_error("WSACreateEvent");
        close_client(client);
        return -1;
    }

    client->overlapped.hEvent = client->event;

    if (post_recv(client) < 0) {
        close_client(client);
        return -1;
    }

    return 0;
}


int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(
            stderr,
            "usage: %s unix:PATH | tcp:HOST:PORT\n",
            argv[0]
        );

        return 1;
    }

    if (net_init() < 0)
        return 1;

    endpoint_t ep;

    if (ep_parse(argv[1], &ep) < 0) {
        net_cleanup();
        return 1;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].socket = INVALID_SOCKET;
        clients[i].event = WSA_INVALID_EVENT;
    }

    SOCKET listen_socket =
        create_listen_socket(&ep, SOMAXCONN);

    if (listen_socket == INVALID_SOCKET) {
        net_cleanup();
        return 1;
    }

    WSAEVENT listen_event = WSACreateEvent();

    if (listen_event == WSA_INVALID_EVENT) {
        print_wsa_error("WSACreateEvent");
        closesocket(listen_socket);
        net_cleanup();
        return 1;
    }

    if (WSAEventSelect(
        listen_socket,
        listen_event,
        FD_ACCEPT | FD_CLOSE)
        == SOCKET_ERROR) {

        print_wsa_error("WSAEventSelect");

        WSACloseEvent(listen_event);
        closesocket(listen_socket);
        net_cleanup();

        return 1;
    }

    fprintf(
        stderr,
        "[async] listening on %s "
        "(Ctrl+C to stop)\n",
        ep.text
    );

    for (;;) {

        WSAEVENT events[WSA_MAXIMUM_WAIT_EVENTS];
        int map[WSA_MAXIMUM_WAIT_EVENTS];

        DWORD event_count = 0;

        events[event_count] = listen_event;
        map[event_count] = -1;
        event_count++;

        for (int i = 0; i < MAX_CLIENTS; i++) {

            if (!clients[i].active)
                continue;

            events[event_count] =
                clients[i].event;

            map[event_count] = i;

            event_count++;
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
            break;
        }

        DWORD event_index =
            wait_result - WSA_WAIT_EVENT_0;

        if (event_index >= event_count) {
            fprintf(
                stderr,
                "bad event index\n"
            );
            break;
        }

        /*
         * ========================================
         * LISTENING EVENT
         * ========================================
         */

        if (event_index == 0) {

            WSANETWORKEVENTS network_events;

            memset(
                &network_events,
                0,
                sizeof network_events
            );

            if (WSAEnumNetworkEvents(
                listen_socket,
                listen_event,
                &network_events)
                == SOCKET_ERROR) {

                print_wsa_error(
                    "WSAEnumNetworkEvents"
                );

                break;
            }

            if (network_events.lNetworkEvents
                & FD_ACCEPT) {

                /*
                 * ќск≥льки listening socket
                 * non-blocking, забираЇмо вс≥
                 * готов≥ connections.
                 */
                for (;;) {

                    SOCKET client_socket =
                        accept(
                            listen_socket,
                            NULL,
                            NULL
                        );

                    if (client_socket
                        == INVALID_SOCKET) {

                        int err =
                            WSAGetLastError();

                        if (err
                            == WSAEWOULDBLOCK) {

                            break;
                        }

                        print_wsa_error("accept");
                        break;
                    }

                    add_client(
                        client_socket,
                        &ep
                    );
                }
            }

            if (network_events.lNetworkEvents
                & FD_CLOSE) {

                fprintf(
                    stderr,
                    "listen socket closed\n"
                );

                break;
            }

            continue;
        }

        /*
         * ========================================
         * CLIENT OVERLAPPED COMPLETION
         * ========================================
         */

        int client_index =
            map[event_index];

        if (client_index < 0
            || client_index >= MAX_CLIENTS) {

            continue;
        }

        client_t* client =
            &clients[client_index];

        if (!client->active)
            continue;

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

        if (!ok) {
            int err = WSAGetLastError();

            if (err != WSAECONNRESET &&
                err != WSAECONNABORTED) {

                print_wsa_error(
                    "WSAGetOverlappedResult"
                );
            }

            close_client(client);
            continue;
        }

        /*
         * ----------------------------------------
         * RECEIVE COMPLETED
         * ----------------------------------------
         */

        if (client->operation == OP_RECV) {

   
            if (transferred == 0) {
                close_client(client);
                continue;
            }
     
            client->send_total = transferred;
            client->send_offset = 0;

            if (post_send(client) < 0) {
                close_client(client);
            }

            continue;
        }

        /*
         * ----------------------------------------
         * SEND COMPLETED
         * ----------------------------------------
         */

        if (client->operation == OP_SEND) {

            if (transferred == 0) {
                close_client(client);
                continue;
            }

            client->send_offset += transferred;

         
            if (client->send_offset
                < client->send_total) {

                if (post_send(client) < 0) {
                    close_client(client);
                }

                continue;
            }

      
            client->send_total = 0;
            client->send_offset = 0;

            if (post_recv(client) < 0) {
                close_client(client);
            }
        }
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        close_client(&clients[i]);
    }

    WSACloseEvent(listen_event);
    closesocket(listen_socket);

    if (ep.family == AF_UNIX) {
        struct sockaddr_un* un =
            (struct sockaddr_un*)&ep.addr;

        DeleteFileA(un->sun_path);
    }

    net_cleanup();

    return 0;
}