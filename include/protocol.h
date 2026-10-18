#ifndef GROUP_CHAT_PROTOCOL_H_
#define GROUP_CHAT_PROTOCOL_H_

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

enum {
    MESSAGE_CHAT = 0,
    MESSAGE_DONE = 1,

    MAX_FRAME_SIZE = 1024,
    BORADCAST_HEADER_SIZE = 7, // [type:1 byte][IP:4 bytes][port:2 bytes]
    MAX_TEXT_SIZE = MAX_FRAME_SIZE - BORADCAST_HEADER_SIZE - 1,
    MAX_CLIENT_FRAME_SIZE = MAX_FRAME_SIZE + 2 // [type:1 byte][newline: 1 byte]
};

typedef struct {
    uint8_t type;

    uint32_t sender_ip;
    uint16_t sender_port;

    size_t text_length;
    uint8_t text[MAX_TEXT_SIZE + 1];
} Message;

typedef enum { PARSE_COMPLETE, PARSE_INCOMPLETE, PARSE_INVALID } ParseResult;

ParseResult parse_client_frame(const uint8_t *data, size_t length, Message *message, size_t *consumed);

ParseResult parse_server_frame(const uint8_t *data, size_t length, Message *message, size_t *consumed);

bool encode_client_frame(const Message *message, uint8_t *output, size_t capacity, size_t *written);

bool encode_server_frame(const Message *message, uint8_t *output, size_t capacity, size_t *written);

#endif
