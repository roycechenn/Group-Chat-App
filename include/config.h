#ifndef INCLUDE_CONFIG_H
#define INCLUDE_CONFIG_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { SERVER_CLIENT_LIMIT = 100 };

typedef struct {
    uint16_t port;
    size_t max_clients;
} ServerConfig;

typedef struct {
    struct in_addr address;
    uint16_t port;
    int message_count;
    const char *log_path;
} ClientConfig;

bool parse_server_args(int argc, char *argv[], ServerConfig *config);

bool parse_client_args(int argc, char *argv[], ClientConfig *config);

#endif
