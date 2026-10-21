#include "../include/config.h"
#include "../include/connection.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>

enum { RANDOM_BYTE_COUNT = 10 };

typedef struct {
    int fd;
    int message_count;

    /*
     * Written by the sender and read by main only after pthread_join().
     */
    int error;
} SenderArgs;

static void *send_messages(void *context) {
    SenderArgs *args = context;

    static const char hex[] = "0123456789ABCDEF";

    Message message = {.type = MESSAGE_CHAT, .text_length = RANDOM_BYTE_COUNT * 2};

    uint8_t frame[MAX_FRAME_SIZE];
    size_t frame_length;

    for (int i = 0; i < args->message_count; ++i) {
        uint8_t random_bytes[RANDOM_BYTE_COUNT];

        if (getentropy(random_bytes, sizeof(random_bytes)) == -1) goto failed;

        for (size_t j = 0; j < sizeof(random_bytes); ++j) {
            message.text[j * 2] = (uint8_t)hex[random_bytes[j] >> 4];
            message.text[j * 2 + 1] = (uint8_t)hex[random_bytes[j] & 0x0F];
        }

        if (!encode_client_frame(&message, frame, sizeof(frame), &frame_length)) {
            errno = EPROTO;
            goto failed;
        }

        if (write_all(args->fd, frame, frame_length) == -1) goto failed;
    }

    Message done = {.type = MESSAGE_DONE};

    if (!encode_client_frame(&done, frame, sizeof(frame), &frame_length)) {
        errno = EPROTO;
        goto failed;
    }

    if (write_all(args->fd, frame, frame_length) == -1) goto failed;

    return NULL;

failed:
    args->error = errno != 0 ? errno : EIO;

    /* Wake the receiver if it is waiting for a server response. */
    shutdown(args->fd, SHUT_RDWR);
    return NULL;
}

static bool print_and_log_message(const Message *message, FILE *log) {
    char ip[INET_ADDRSTRLEN];

    if (inet_ntop(AF_INET, &message->sender_ip, ip, sizeof(ip)) == NULL) return false;

    unsigned port = ntohs(message->sender_port);

    /*
     * Printed metadata needs more space than the seven-byte wire header.
     */
    char output[15 + 10 + MAX_TEXT_SIZE + 2];

    int prefix_length = snprintf(output, sizeof(output), "%-15s%-10u", ip, port);

    if (prefix_length < 0 || (size_t)prefix_length >= sizeof(output)) return false;

    size_t prefix = (size_t)prefix_length;

    if (message->text_length > sizeof(output) - prefix - 2) return false;

    /*
     * Copy by explicit length, so embedded NUL bytes cannot truncate text.
     */
    memcpy(output + prefix, message->text, message->text_length);

    size_t length = prefix + message->text_length;
    output[length++] = '\n';
    output[length] = '\0';

    if (fwrite(output, 1, length, stdout) != length || fwrite(output, 1, length, log) != length) return false;

    return fflush(stdout) == 0 && fflush(log) == 0;
}

static int receive_messages(Connection *connection, FILE *log) {
    for (;;) {
        IoResult io_result = connection_receive(connection);

        if (io_result == IO_WOULD_BLOCK) continue;

        if (io_result == IO_CLOSED) {
            fprintf(stderr, "Server closed before a complete shutdown message\n");
            return -1;
        }

        if (io_result != IO_PROGRESS) {
            fprintf(stderr, "Failed to receive server data\n");
            return -1;
        }

        for (;;) {
            Message message;

            ParseResult result = connection_next_message(connection, true, &message);

            if (result == PARSE_INCOMPLETE) break;

            if (result == PARSE_INVALID) {
                fprintf(stderr, "Invalid server frame\n");
                return -1;
            }

            if (message.type == MESSAGE_DONE) return 0;

            if (!print_and_log_message(&message, log)) {
                fprintf(stderr, "Failed to print or log a message\n");
                return -1;
            }
        }
    }
}

int main(int argc, char *argv[]) {
    ClientConfig config;

    if (!parse_client_args(argc, argv, &config)) {
        fprintf(stderr,
                "Usage: %s <IPv4 address> <port: 1-65535> "
                "<message count: 0-INT_MAX> <log path>\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        perror("signal");
        return EXIT_FAILURE;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd == -1) {
        perror("socket");
        return EXIT_FAILURE;
    }

    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons(config.port), .sin_addr = config.address};

    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) == -1) {
        perror("connect");
        close(fd);
        return EXIT_FAILURE;
    }

    FILE *log = fopen(config.log_path, "w");

    if (log == NULL) {
        perror("open log");
        close(fd);
        return EXIT_FAILURE;
    }

    Connection connection;
    connection_init(&connection, fd, 0, 0);

    SenderArgs sender_args = {.fd = fd, .message_count = config.message_count, .error = 0};

    pthread_t sender;

    int thread_error = pthread_create(&sender, NULL, send_messages, &sender_args);

    if (thread_error != 0) {
        errno = thread_error;
        perror("pthread_create");
        connection_close(&connection);
        fclose(log);
        return EXIT_FAILURE;
    }

    int result = receive_messages(&connection, log);

    /*
     * A malformed response or premature shutdown must also wake a sender
     * that is blocked in write_all().
     */
    shutdown(fd, SHUT_RDWR);

    thread_error = pthread_join(sender, NULL);

    if (thread_error != 0) {
        errno = thread_error;
        perror("pthread_join");

        /*
         * Do not return from main with a worker potentially using its
         * stack-allocated arguments. Terminate the process on this error.
         */
        exit(EXIT_FAILURE);
    }

    if (sender_args.error != 0) {
        errno = sender_args.error;
        perror("send messages");
        result = -1;
    }

    connection_close(&connection);

    if (fclose(log) == EOF) {
        perror("close log");
        result = -1;
    }

    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
