#define _CRT_SECURE_NO_WARNINGS

#include "../common.h"

#include <process.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX_CLIENTS 1024
#define BUF_SIZE (1 << 16)

typedef struct {
    SOCKET socket;

    char out_buf[BUF_SIZE];
    int out_size;
    int out_sent;
} client_t;

static client_t clients[MAX_CLIENTS];

static void remove_client(int index)
{
    closesocket(clients[index].socket);

    clients[index] = clients[MAX_CLIENTS - 1];
    clients[MAX_CLIENTS - 1].socket = INVALID_SOCKET;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr,
            "usage: %s unix:PATH | tcp:HOST:PORT\n",
            argv[0]);
        return 1;
    }

    if (net_init() < 0)
        return 1;

    endpoint_t ep;

    if (ep_parse(argv[1], &ep) < 0)
        return 1;

    SOCKET listen_socket = ep_listen(&ep, SOMAXCONN);

    if (listen_socket == INVALID_SOCKET)
        return 1;

    if (set_nonblock(listen_socket, 1) < 0) {
        print_wsa_error("set_nonblock listen");
        closesocket(listen_socket);
        return 1;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        clients[i].socket = INVALID_SOCKET;
    }

    fprintf(stderr,
        "[nonblocking] listening on %s (Ctrl+C to stop)\n",
        ep.text);

    for (;;) {
        WSAPOLLFD pollfds[MAX_CLIENTS + 1];

        int map[MAX_CLIENTS + 1];

        int count = 0;

        /*
         * Listening socket.
         * POLLRDNORM = є нове connection для accept().
         */
        pollfds[count].fd = listen_socket;
        pollfds[count].events = POLLRDNORM;
        pollfds[count].revents = 0;

        map[count] = -1;
        count++;

        /*
         * Client sockets.
         */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].socket == INVALID_SOCKET)
                continue;

            pollfds[count].fd = clients[i].socket;
            pollfds[count].events = POLLRDNORM;

            /*
             * Якщо залишилися дані, які send() не зміг
             * відправити раніше — чекаємо writable.
             */
            if (clients[i].out_sent < clients[i].out_size) {
                pollfds[count].events |= POLLWRNORM;
            }

            pollfds[count].revents = 0;

            map[count] = i;
            count++;
        }

        int ready = WSAPoll(pollfds, count, -1);

        if (ready == SOCKET_ERROR) {
            print_wsa_error("WSAPoll");
            break;
        }

        /*
         * ============================================
         * ACCEPT
         * ============================================
         */

        if (pollfds[0].revents & POLLRDNORM) {
            for (;;) {
                SOCKET client_socket =
                    accept(listen_socket, NULL, NULL);

                if (client_socket == INVALID_SOCKET) {
                    int err = WSAGetLastError();

                    if (err == WSAEWOULDBLOCK)
                        break;

                    print_wsa_error("accept");
                    break;
                }

                if (set_nonblock(client_socket, 1) < 0) {
                    print_wsa_error("set_nonblock client");
                    closesocket(client_socket);
                    continue;
                }

                tune_socket(client_socket, &ep);

                int added = 0;

                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].socket == INVALID_SOCKET) {
                        clients[i].socket = client_socket;
                        clients[i].out_size = 0;
                        clients[i].out_sent = 0;

                        added = 1;
                        break;
                    }
                }

                if (!added) {
                    fprintf(stderr,
                        "too many clients\n");

                    closesocket(client_socket);
                }
            }
        }

        /*
         * ============================================
         * CLIENT I/O
         * ============================================
         */

        for (int p = 1; p < count; p++) {
            int index = map[p];

            if (index < 0)
                continue;

            client_t* client = &clients[index];

            if (client->socket == INVALID_SOCKET)
                continue;

            short events = pollfds[p].revents;

            /*
             * Error / peer disconnected.
             */
            if (events & (POLLERR | POLLHUP | POLLNVAL)) {
                closesocket(client->socket);
                client->socket = INVALID_SOCKET;
                continue;
            }

            /*
             * ----------------------------------------
             * SEND pending echo
             * ----------------------------------------
             */

            if ((events & POLLWRNORM) &&
                client->out_sent < client->out_size) {

                int remaining =
                    client->out_size - client->out_sent;

                int sent =
                    send(client->socket,
                        client->out_buf + client->out_sent,
                        remaining,
                        0);

                if (sent > 0) {
                    client->out_sent += sent;

                    if (client->out_sent ==
                        client->out_size) {

                        client->out_size = 0;
                        client->out_sent = 0;
                    }
                }
                else if (sent == SOCKET_ERROR) {
                    int err = WSAGetLastError();

                    if (err != WSAEWOULDBLOCK) {
                        closesocket(client->socket);
                        client->socket = INVALID_SOCKET;
                        continue;
                    }
                }
            }

            /*
             * ----------------------------------------
             * RECEIVE
             * ----------------------------------------
             *
             * Поки є невідправлений echo, нові дані
             * не читаємо. Так out_buf не перезапишеться.
             */

            if ((events & POLLRDNORM) &&
                client->out_size == 0) {

                int received =
                    recv(client->socket,
                        client->out_buf,
                        BUF_SIZE,
                        0);

                if (received > 0) {
                    client->out_size = received;
                    client->out_sent = 0;

                    /*
                     * Пробуємо send() одразу.
                     * Він non-blocking.
                     */
                    int sent =
                        send(client->socket,
                            client->out_buf,
                            received,
                            0);

                    if (sent > 0) {
                        client->out_sent = sent;

                        if (sent == received) {
                            client->out_size = 0;
                            client->out_sent = 0;
                        }
                    }
                    else if (sent == SOCKET_ERROR) {
                        int err = WSAGetLastError();

                        if (err != WSAEWOULDBLOCK) {
                            closesocket(client->socket);
                            client->socket =
                                INVALID_SOCKET;
                        }
                    }
                }
                else if (received == 0) {
                    /*
                     * Client gracefully closed.
                     */
                    closesocket(client->socket);
                    client->socket = INVALID_SOCKET;
                }
                else {
                    int err = WSAGetLastError();

                    if (err != WSAEWOULDBLOCK) {
                        closesocket(client->socket);
                        client->socket =
                            INVALID_SOCKET;
                    }
                }
            }
        }
    }

    closesocket(listen_socket);
    net_cleanup();

    return 0;
}