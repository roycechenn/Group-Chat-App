#include "../include/config.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>

static bool parse_number(const char *text, long minimum, long maximum, long *value) {
    if (text == NULL || *text == '\0') return false;

    errno = 0;
    char *end = NULL;
    long parsed = strtol(text, &end, 10);

    if (errno == ERANGE || end == text || *end != '\0' || parsed < minimum || parsed > maximum) return false;

    *value = parsed;
    return true;
}

bool parse_server_args(int argc, char **argv, ServerConfig *config) {
    long port;
    long clients;

    if (argc != 3 || !parse_number(argv[1], 1, 65535, &port) ||
        !parse_number(argv[2], 1, SERVER_CLIENT_LIMIT, &clients))
        return false;

    config->port = port;
    config->max_clients = clients;

    return true;
}

bool parse_client_args(int argc, char **argv, ClientConfig *config) {
    long port;
    long messages;

    if (argc != 5 || !parse_number(argv[2], 1, 65535, &port) || !parse_number(argv[3], 0, INT_MAX, &messages) ||
        inet_pton(AF_INET, argv[1], &config->address) != 1)
        return false;

    config->port = (uint16_t)port;
    config->message_count = (int)messages;
    config->log_path = argv[4];

    return true;
}
