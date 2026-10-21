#include "../include/protocol.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static ParseResult parse_frame(const uint8_t *data, size_t length, bool include_address, Message *message,
                               size_t *consumed) {
    if (consumed != NULL) *consumed = 0;

    if (message == NULL || consumed == NULL || (data == NULL && length != 0)) return PARSE_INVALID;

    if (length == 0) return PARSE_INCOMPLETE;

    // [type:1][ip:4][port:2]
    Message result = {0};
    result.type = data[0];

    if (result.type == MESSAGE_DONE) { // shut down
        if (length < 2) return PARSE_INCOMPLETE;
        if (data[1] != '\n') return PARSE_INVALID;

        *message = result;
        *consumed = 2;
        return PARSE_COMPLETE;
    }

    if (result.type != MESSAGE_CHAT) return PARSE_INVALID;

    size_t header_size = include_address ? BORADCAST_HEADER_SIZE : 1;
    size_t frame_size = include_address ? MAX_FRAME_SIZE : MAX_CLIENT_FRAME_SIZE;

    if (length <= header_size) return PARSE_INCOMPLETE;

    size_t search_end = length < frame_size ? length : frame_size;
    const uint8_t *newline = memchr(data + header_size, '\n', search_end - header_size);
    if (newline == NULL) return length >= frame_size ? PARSE_INVALID : PARSE_INCOMPLETE;

    size_t text_length = (size_t)(newline - (data + header_size));
    if (text_length > MAX_TEXT_SIZE) return PARSE_INVALID;

    if (include_address) {
        memcpy(&result.sender_ip, data + 1, sizeof(result.sender_ip));
        memcpy(&result.sender_port, data + 5, sizeof(result.sender_port));
    }

    memcpy(&result.text, data + header_size, text_length);
    result.text[text_length] = '\0';
    result.text_length = text_length;

    *message = result;
    *consumed = (size_t)(newline - data) + 1;
    return PARSE_COMPLETE;
}

static bool encode_frame(const Message *message, bool include_address, uint8_t *output, size_t capacity,
                         size_t *written) {
    if (written != NULL) *written = 0;
    if (message == NULL || output == NULL || written == NULL) return false;

    if (message->type == MESSAGE_DONE) {
        if (message->text_length != 0 || capacity < 2) return false;
        output[0] = MESSAGE_DONE;
        output[1] = '\n';
        *written = 2;
        return true;
    }

    if (message->type != MESSAGE_CHAT || message->text_length > MAX_TEXT_SIZE) return false;

    if (memchr(message->text, '\n', message->text_length) != NULL) return false; // no "\n" in msg

    size_t header_size = include_address ? BORADCAST_HEADER_SIZE : 1;
    size_t frame_size = header_size + message->text_length + 1;

    if (frame_size > capacity || frame_size > MAX_FRAME_SIZE) return false;

    output[0] = MESSAGE_CHAT;
    if (include_address) {
        memcpy(output + 1, &message->sender_ip, sizeof(message->sender_ip));
        memcpy(output + 5, &message->sender_port, sizeof(message->sender_port));
    }

    memcpy(output + header_size, message->text, message->text_length);

    output[frame_size - 1] = '\n';
    *written = frame_size;

    return true;
}

ParseResult parse_client_frame(const uint8_t *data, size_t length, Message *message, size_t *consumed) {
    return parse_frame(data, length, false, message, consumed);
}

ParseResult parse_server_frame(const uint8_t *data, size_t length, Message *message, size_t *consumed) {
    return parse_frame(data, length, true, message, consumed);
}

bool encode_client_frame(const Message *message, uint8_t *output, size_t capacity, size_t *written) {
    return encode_frame(message, false, output, capacity, written);
}

bool encode_server_frame(const Message *message, uint8_t *output, size_t capacity, size_t *written) {
    return encode_frame(message, true, output, capacity, written);
}
