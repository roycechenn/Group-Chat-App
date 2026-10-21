#include "../include/config.h"
#include "../include/connection.h"

#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum { SHUTDOWN_TIMEOUT_MS = 5000 };

typedef struct {
    int listener;
    Connection *clients;
    size_t capacity;
    size_t done_count;

    bool finishing;
    int64_t shutdown_deadline;
} Server;

static int64_t monotonic_ms(void) {
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) return -1;

    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void disconnect_client(Server *server, size_t index) {
    Connection *connection = &server->clients[index];

    if (connection->fd < 0) return;

    if (connection->done) --server->done_count;

    connection_close(connection);
}

static void server_destroy(Server *server) {
    if (server->clients != NULL) {
        for (size_t i = 0; i < server->capacity; ++i)
            connection_close(&server->clients[i]);

        free(server->clients);
    }

    if (server->listener >= 0) close(server->listener);

    server->clients = NULL;
    server->listener = -1;
}

static bool server_init(Server *server, const ServerConfig *config) {
    *server = (Server){.listener = -1, .capacity = config->max_clients};

    server->clients = calloc(server->capacity, sizeof(*server->clients));
    if (server->clients == NULL) return false;

    for (size_t i = 0; i < server->capacity; ++i)
        server->clients[i].fd = -1;

    server->listener = socket(AF_INET, SOCK_STREAM, 0);

    if (server->listener == -1) return false;

    if (server->listener >= FD_SETSIZE) {
        errno = EMFILE;
        return false;
    }

    int reuse = 1;

    if (setsockopt(server->listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1) return false;

    if (set_nonblocking(server->listener) == -1) return false;

    struct sockaddr_in address = {
        .sin_family = AF_INET, .sin_port = htons(config->port), .sin_addr = {.s_addr = htonl(INADDR_ANY)}

    };

    if (bind(server->listener, (struct sockaddr *)&address, sizeof(address)) == -1) return false;

    return listen(server->listener, (int)server->capacity) == 0;
}

static bool accept_clients(Server *server) {
    for (;;) {
        struct sockaddr_in address;
        socklen_t address_length = sizeof(address);

        int fd = accept(server->listener, (struct sockaddr *)&address, &address_length);

        if (fd == -1) {
            if (errno == EINTR || errno == ECONNABORTED) continue;

            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;

            return false;
        }

        // server gets nonblocking
        if (fd >= FD_SETSIZE || set_nonblocking(fd) == -1) {
            close(fd);
            continue;
        }

        size_t slot = 0;

        while (slot < server->capacity && server->clients[slot].fd >= 0)
            ++slot;

        if (slot == server->capacity) {
            close(fd);
            continue;
        }

        connection_init(&server->clients[slot], fd, address.sin_addr.s_addr, address.sin_port);
    }
}

static void broadcast_message(Server *server, size_t sender_index, Message *message) {
    Connection *sender = &server->clients[sender_index];
    message->sender_ip = sender->peer_ip;
    message->sender_port = sender->peer_port;

    uint8_t frame[MAX_FRAME_SIZE];
    size_t frame_length;

    if (!encode_server_frame(message, frame, sizeof(frame), &frame_length)) {
        disconnect_client(server, sender_index);
        return;
    }

    for (size_t i = 0; i < server->capacity; ++i) {
        Connection *recipient = &server->clients[i];

        if (recipient->fd < 0) continue;

        if (!connection_queue(recipient, frame, frame_length)) {
            fprintf(stderr, "Disconnecting client with a full output queue\n");
            disconnect_client(server, i);
        }
    }
}

static bool begin_shutdown(Server *server) {
    int64_t now = monotonic_ms();

    if (now < 0) return false;

    server->finishing = true;
    server->shutdown_deadline = now + SHUTDOWN_TIMEOUT_MS;

    const uint8_t frame[] = {MESSAGE_DONE, '\n'};

    for (size_t i = 0; i < server->capacity; ++i) {
        Connection *connection = &server->clients[i];

        if (connection->fd >= 0 && !connection_queue(connection, frame, sizeof(frame))) disconnect_client(server, i);
    }

    return true;
}

static bool process_client_input(Server *server, size_t index) {
    Connection *connection = &server->clients[index];

    while (connection->fd >= 0 && !server->finishing) {
        Message message;

        ParseResult result = connection_next_message(connection, false, &message);

        if (result == PARSE_INCOMPLETE) return true;

        if (result == PARSE_INVALID) {
            fprintf(stderr, "Rejecting malformed client frame\n");
            disconnect_client(server, index);
            return true;
        }

        if (message.type == MESSAGE_CHAT) {
            broadcast_message(server, index, &message);
            continue;
        }

        if (!connection->done) {
            connection->done = true;
            ++server->done_count;
        }

        if (server->done_count == server->capacity) return begin_shutdown(server);
    }

    return true;
}

static bool all_output_sent(const Server *server) {
    for (size_t i = 0; i < server->capacity; ++i) {
        const Connection *connection = &server->clients[i];

        if (connection->fd >= 0 && connection_has_pending_output(connection)) return false;
    }

    return true;
}

static int server_run(Server *server) {
    for (;;) {
        struct timeval timeout;
        struct timeval *timeout_pointer = NULL;

        if (server->finishing) {
            if (all_output_sent(server)) return 0;

            int64_t now = monotonic_ms();

            if (now < 0) return -1;

            int64_t remaining = server->shutdown_deadline - now;

            if (remaining <= 0) {
                errno = ETIMEDOUT;
                return -1;
            }

            timeout.tv_sec = remaining / 1000;
            timeout.tv_usec = (remaining % 1000) * 1000;
            timeout_pointer = &timeout;
        }

        fd_set readable;
        fd_set writable;

        FD_ZERO(&readable);
        FD_ZERO(&writable);

        int max_fd = -1;

        if (!server->finishing) {
            FD_SET(server->listener, &readable);
            max_fd = server->listener;
        }

        for (size_t i = 0; i < server->capacity; ++i) {
            Connection *connection = &server->clients[i];

            if (connection->fd < 0) continue;

            if (!server->finishing) FD_SET(connection->fd, &readable);

            if (connection_has_pending_output(connection)) FD_SET(connection->fd, &writable);

            if (connection->fd > max_fd) max_fd = connection->fd;
        }

        int ready = select(max_fd + 1, &readable, &writable, NULL, timeout_pointer);

        if (ready == -1) {
            if (errno == EINTR) continue;

            return -1;
        }

        if (ready == 0) continue;

        if (!server->finishing && FD_ISSET(server->listener, &readable) && !accept_clients(server)) return -1;

        for (size_t i = 0; i < server->capacity && !server->finishing; ++i) {
            Connection *connection = &server->clients[i];

            if (connection->fd < 0 || !FD_ISSET(connection->fd, &readable)) continue;

            IoResult result = connection_receive(connection);

            if (result == IO_WOULD_BLOCK) continue;

            if (result != IO_PROGRESS) {
                if (result == IO_ERROR) perror("client read");

                disconnect_client(server, i);
                continue;
            }

            if (!process_client_input(server, i)) return -1;
        }

        for (size_t i = 0; i < server->capacity; ++i) {
            Connection *connection = &server->clients[i];

            if (connection->fd < 0 || !FD_ISSET(connection->fd, &writable)) continue;

            IoResult result = connection_flush(connection);

            if (result != IO_PROGRESS && result != IO_WOULD_BLOCK) disconnect_client(server, i);
        }
    }
}

int main(int argc, char *argv[]) {
    ServerConfig config;

    if (!parse_server_args(argc, argv, &config)) {
        fprintf(stderr, "Usage: %s <port: 1-65535> <clients: 1-%d>\n", argv[0], SERVER_CLIENT_LIMIT);
        return EXIT_FAILURE;
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return EXIT_FAILURE;
    }

    Server server;

    if (!server_init(&server, &config)) {
        perror("server initialization");
        server_destroy(&server);
        return EXIT_FAILURE;
    }

    int result = server_run(&server);

    if (result == -1) perror("server event loop");

    server_destroy(&server);

    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
