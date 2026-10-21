#ifndef GROUP_CHAT_CONNECTION_H
#define GROUP_CHAT_CONNECTION_H

#include "protocol.h"

enum { INPUT_CAPACITY = MAX_FRAME_SIZE * 4, OUTPUT_CAPACITY = MAX_FRAME_SIZE * 64 };

typedef struct {
    int fd;

    uint32_t peer_ip;
    uint64_t peer_port;
    bool done;

    uint8_t input[INPUT_CAPACITY];
    size_t input_length;

    uint8_t output[OUTPUT_CAPACITY];
    size_t output_begin;
    size_t output_end;
} Connection;

typedef enum {
    IO_PROGRESS,
    IO_WOULD_BLOCK,
    IO_CLOSED,
    IO_ERROR,
    IO_LIMIT,
} IoResult;

void connection_init(Connection *connection, int fd, uint32_t ip, uint16_t port);

void connection_close(Connection *connection);

IoResult connection_receive(Connection *connection);

ParseResult connection_next_message(Connection *connection, bool from_server, Message *message);

bool connection_queue(Connection *connection, const uint8_t *data, size_t length);

bool connection_has_pending_output(const Connection *connection);

IoResult connection_flush(Connection *connection);

int set_nonblocking(int fd);

int write_all(int fd, const uint8_t *data, size_t length);

#endif
