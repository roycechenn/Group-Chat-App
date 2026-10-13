#include "../include/client.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <unistd.h>
#define handle_error(msg)                                                      \
  do {                                                                         \
    perror(msg);                                                               \
    exit(EXIT_FAILURE);                                                        \
  } while (0)

#define BUF_SIZE 1024
#define MSG_LEN 10

struct read_arg {
  int fd;
  char *log_path;
};

struct write_arg {
  int fd;
  int num_msg;
};

int convert(uint8_t *buf, ssize_t buf_size, char *str, ssize_t str_size) {
  if (buf == NULL || str == NULL || buf_size <= 0 ||
      str_size < (buf_size * 2 + 1))
    return -1;

  for (int i = 0; i < buf_size; i++)
    sprintf(str + i * 2, "%02X", buf[i]);
  str[buf_size * 2] = '\0';

  return 0;
}

int write_func(int fd, char const *buf, int len) {
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = write(fd, buf + sent, len - sent);
    if (n <= 0)
      return -1;
    sent += (ssize_t)n;
  }
  return 0;
}

void *read_thread(void *arg_) {
  struct read_arg arg = *(struct read_arg *)arg_;
  int fd = arg.fd;
  char *path = arg.log_path;

  int log_fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (log_fd == -1)
    handle_error("open");

  uint8_t buf[BUF_SIZE * 2]; // raw bytes
  size_t n = 0;
  for (;;) {
    ssize_t num_read = read(fd, buf + n, sizeof(buf) - n);
    if (num_read < 0) {
      close(log_fd);
      handle_error("read");
    }
    if (num_read == 0)
      break;

    n += (size_t)num_read;

    while (1) {
      if (n < 1)
        break;
      if (buf[0] == 0) {
        // [0][4b ip][2b port][\n]
        if (n < 8)
          break;

        ssize_t new_line_idx = -1;
        for (int i = 7; i < n; i++) {
          if (buf[i] == '\n') {
            new_line_idx = i;
            break;
          }
        }

        if (new_line_idx == -1)
          break;

        uint32_t ip;
        uint16_t port_;
        memcpy(&ip, buf + 1, sizeof(ip));
        memcpy(&port_, buf + 5, sizeof(port_));

        // convert ip to string
        char str_ip[INET_ADDRSTRLEN];
        if (inet_ntop(AF_INET, &ip, str_ip, sizeof(str_ip)) == NULL)
          handle_error("inet_ntop");

        unsigned port = (unsigned)ntohs(port_);

        size_t len = (size_t)new_line_idx - 7;
        char msg[BUF_SIZE];
        memcpy(msg, buf + 7, len);
        msg[len] = '\0';

        char output[BUF_SIZE];
        sprintf(output, "%-15s%-10u%s\n", str_ip, port, msg);
        printf("%-15s%-10u%s\n", str_ip, port, msg);
        if (write_func(log_fd, output, strlen(output)) == -1)
          handle_error("write");

        size_t end_msg_idx = (size_t)new_line_idx + 1;
        memmove(buf, buf + end_msg_idx, n - end_msg_idx);
        n -= end_msg_idx;
      }
      if (buf[0] == 1) {
        if (n < 2) // 1 \n
          break;
        close(log_fd);
        return NULL;
      }
    }
  }
  close(log_fd);
  return NULL;
}

void *write_thread(void *arg_) {
  struct write_arg arg = *(struct write_arg *)arg_;
  int num_msg = arg.num_msg;
  int fd = arg.fd;

  for (int i = 0; i < num_msg; i++) {
    uint8_t buf[MSG_LEN];
    char str[MSG_LEN * 2 + 1];
    if (getentropy(buf, MSG_LEN) != 0)
      handle_error("getentropy");

    if (convert(buf, sizeof(buf), str, sizeof(str)) == -1)
      handle_error("convert");

    char output[1 + MSG_LEN * 2 + 1];
    output[0] = 0;
    memcpy(output + 1, str, MSG_LEN * 2);
    output[1 + MSG_LEN * 2] = '\n';

    if (write_func(fd, output, sizeof(output)) == -1)
      handle_error("write");
  }

  char buf_type1[2] = {1, '\n'};
  if (write_func(fd, buf_type1, 2) == -1)
    handle_error("write");

  return NULL;
}

int main(int argc, char *argv[]) {
  if (argc != 5) {
    fprintf(stderr, "Must have 5 arguments\n");
    exit(EXIT_FAILURE);
  }

  struct in_addr ip;
  if (inet_pton(AF_INET, argv[1], &ip) != 1)
    handle_error("inet_pton");

  uint16_t port = (uint16_t)atoi(argv[2]);
  int num_msg = (int)atoi(argv[3]);
  char *log_path = argv[4];

  struct sockaddr_in addr;
  int sfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sfd == -1)
    handle_error("socket");

  memset(&addr, 0, sizeof(struct sockaddr_in));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr = ip;

  if (connect(sfd, (struct sockaddr *)&addr, sizeof(struct sockaddr_in)) == -1)
    handle_error("connect");

  struct read_arg read_arg;
  read_arg.fd = sfd;
  read_arg.log_path = log_path;
  struct write_arg write_arg;
  write_arg.fd = sfd;
  write_arg.num_msg = num_msg;

  pthread_t r_tid, w_tid;
  pthread_create(&r_tid, NULL, read_thread, &read_arg);
  pthread_create(&w_tid, NULL, write_thread, &write_arg);

  pthread_join(r_tid, NULL);
  pthread_join(w_tid, NULL);

  close(sfd);
  return 0;
}
