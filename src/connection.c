#include "../include/connection.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

void connection_init(Connection *connection, int fd, uint32_t ip, uint16_t port) {

    memset(connection, 0, sizeof(*connection));
    connection->fd = fd;
    connection->peer_ip = ip;
    connection->peer_port = port;
}

void connection_close(Connection *connection) {
    if (connection->fd >= 0) close(connection->fd);

    connection->fd = -1;
    connection->peer_ip = 0;
    connection->peer_port = 0;
    connection->done = false;
    connection->input_length = 0;
    connection->output_begin = 0;
    connection->output_end = 0;
}

IoResult connection_receive(Connection *connection) {
    if (connection->input_length == sizeof(connection->input)) return IO_LIMIT;

    for (;;) {
        ssize_t received = read(connection->fd, connection->input + connection->input_length,
                                sizeof(connection->input) - connection->input_length);

        if (received > 0) {
            connection->input_length += (size_t)received;
            return IO_PROGRESS;
        }

        if (received == 0) return IO_CLOSED;

        if (errno == EINTR) continue; // interrupted by a signal, retry

        if (errno == EAGAIN || errno == EWOULDBLOCK) return IO_WOULD_BLOCK;

        return IO_ERROR;
    }
}

ParseResult connection_next_message(Connection *connection, bool from_server, Message *message) {
    size_t consumed = 0;

    ParseResult result = from_server ? parse_server_frame(connection->input, connection->input_length, message, &consumed)
                                     : parse_client_frame(connection->input, connection->input_length, message, &consumed);

    if (result == PARSE_COMPLETE) {
        size_t remaining = connection->input_length - consumed;

        memmove(connection->input, connection->input + consumed, remaining);

        connection->input_length = remaining;
    }

    return result;
}

bool connection_queue(Connection *connection, const uint8_t *data, size_t length) {
    if (connection->output_begin != 0) {
        size_t remaining = connection->output_end - connection->output_begin;

        memmove(connection->output, connection->output + connection->output_begin, remaining);

        connection->output_begin = 0;
        connection->output_end = remaining;
    }
    if (length > sizeof(connection->output) - connection->output_end) return false;

    memcpy(connection->output + connection->output_end, data, length);
    connection->output_end += length;

    return true;
}

bool connection_has_pending_output(const Connection *connection) { return connection->output_begin < connection->output_end; }

IoResult connection_flush(Connection *connection) {
    while (connection_has_pending_output(connection)) {
        ssize_t sent = write(connection->fd, connection->output + connection->output_begin,
                             connection->output_end - connection->output_begin);
        if (sent > 0) {
            connection->output_begin += (size_t)sent;
            continue;
        }

        if (sent == 0) return IO_CLOSED;

        if (errno == EINTR) continue;

        if (errno == EAGAIN || errno == EWOULDBLOCK) return IO_WOULD_BLOCK;

        return IO_ERROR;
    }
    connection->output_begin = 0;
    connection->output_end = 0;

    return IO_PROGRESS;
}

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags == -1) return -1;

    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int write_all(int fd, const uint8_t *data, size_t length) {
    size_t offset = 0;

    while (offset < length) {
        ssize_t sent = write(fd, data + offset, length - offset);

        if (sent > 0) {
            offset += (size_t)sent;
            continue;
        }

        if (sent < 0 && errno == EINTR) continue;

        if (sent == 0) errno = EPIPE; // broken pipe
        return -1;
    }

    return 0;
}
